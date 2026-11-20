#include "project_capacity_ledger.hpp"

#include <array>
#include <new>
#include <string>
#include <utility>

namespace bloom::host {
namespace {

using Thread = commands::CommandStack::CapacityCheck::Thread;

[[nodiscard]] std::uint64_t limitValue(const project::ProjectCapacityLimit limit,
                                       const project::ProjectCapacityLimits& limits) noexcept {
    switch (limit) {
    case project::ProjectCapacityLimit::JsonValues:
        return limits.jsonValues;
    case project::ProjectCapacityLimit::DocumentBytes:
        return limits.documentBytes;
    case project::ProjectCapacityLimit::ContainerEntries:
        return limits.containerEntries;
    case project::ProjectCapacityLimit::StringBytes:
        return limits.stringBytes;
    }
    return 0;
}

[[nodiscard]] commands::ProjectCapacityRefusal
refusalOf(const project::ProjectCapacityExcess& excess) {
    return {.limit = std::string(project::projectCapacityLimitName(excess.limit)),
            .value = excess.value,
            .actual = excess.actual,
            .unit = std::string(project::projectCapacityLimitUnit(excess.limit)),
            .message = project::projectCapacityMessage(excess)};
}

} // namespace

ProjectCapacityLedger::ProjectCapacityLedger(
    std::shared_ptr<const project::RoundTripState> roundTrip, const std::uint32_t schemaMinor)
    : roundTrip_(std::move(roundTrip)), schemaMinor_(schemaMinor) {}

ProjectCapacityLedger::~ProjectCapacityLedger() = default;

void ProjectCapacityLedger::colorSettingsState(const std::uint64_t id,
                                               const document::ColorSettings& settings) {
    auto shared = std::make_shared<const document::ColorSettings>(settings);
    const std::lock_guard lock(mutex_);
    colorSettings_.insert_or_assign(id, std::move(shared));
}

void ProjectCapacityLedger::start(const document::Snapshot& snapshot,
                                  std::shared_ptr<const project::CanonicalDocumentSizeIndex> index,
                                  const bool measureNow) {
    if (index == nullptr && measureNow) {
        try {
            if (auto measured = whole(snapshot))
                index = std::move(measured.index);
        } catch (const std::bad_alloc&) {
            // Measured in the background instead.
        }
    }
    if (index != nullptr) {
        offer(snapshot, std::move(index));
        return;
    }
    measureInBackground(snapshot);
}

std::shared_ptr<const commands::CommandStack::CapacityCheck> ProjectCapacityLedger::check() {
    auto self = shared_from_this();
    auto check = std::make_shared<commands::CommandStack::CapacityCheck>();
    check->measure = [self](const document::Snapshot& before, const document::Snapshot& candidate,
                            const Thread thread) {
        return self->measure(before, candidate, thread);
    };
    check->published = [self](const document::Snapshot& snapshot,
                              const std::shared_ptr<const void>& size) {
        self->published(snapshot, size);
    };
    return check;
}

ProjectCapacityWork ProjectCapacityLedger::work() const {
    return {.known = known().index != nullptr,
            .wholeMeasures = wholeMeasures_.load(),
            .ownerMeasures = ownerMeasures_.load(),
            .ownerRecordsCounted = ownerRecordsCounted_.load(),
            .ownerCompositionsCounted = ownerCompositionsCounted_.load(),
            .preparationMeasures = preparationMeasures_.load(),
            .preparationWholeMeasures = preparationWholeMeasures_.load(),
            .unmeasuredEdits = unmeasuredEdits_.load(),
            .adopted = adopted_.load()};
}

ProjectCapacityLedger::Known ProjectCapacityLedger::known() const {
    const std::lock_guard lock(mutex_);
    return known_;
}

void ProjectCapacityLedger::offer(
    const document::Snapshot& snapshot,
    std::shared_ptr<const project::CanonicalDocumentSizeIndex> index) {
    const std::lock_guard lock(mutex_);
    if (known_.index != nullptr)
        return;
    known_.snapshot = snapshot;
    known_.index = std::move(index);
}

void ProjectCapacityLedger::published(const document::Snapshot& snapshot,
                                      const std::shared_ptr<const void>& size) noexcept {
    if (size == nullptr)
        return;
    auto index = std::static_pointer_cast<const project::CanonicalDocumentSizeIndex>(size);
    const std::lock_guard lock(mutex_);
    known_.snapshot = snapshot;
    known_.index = std::move(index);
    adopted_.fetch_add(1);
}

void ProjectCapacityLedger::measureInBackground(const document::Snapshot& snapshot) {
    // Started once per content; a measure that failed is not retried (edits stay admitted
    // unmeasured, and a save still names any failure).
    if (background_.joinable())
        return;
    try {
        background_ = core::StoppableWorkerThread([this, snapshot](const std::stop_token& stop) {
            try {
                if (auto measured = whole(snapshot, stop))
                    offer(snapshot, std::move(measured.index));
            } catch (const std::bad_alloc&) {
                // Unmeasured.
            }
        });
    } catch (...) {
        // No thread: unmeasured, as above.
    }
}

std::shared_ptr<const document::ColorSettings>
ProjectCapacityLedger::colorSettingsOf(const document::Snapshot& snapshot) const {
    const std::lock_guard lock(mutex_);
    const auto found = colorSettings_.find(snapshot.project().colorSettingsState());
    return found != colorSettings_.end() ? found->second : nullptr;
}

project::CanonicalDocumentIndexResult
ProjectCapacityLedger::whole(const document::Snapshot& snapshot, std::stop_token stop) {
    const auto colors = colorSettingsOf(snapshot);
    if (colors == nullptr)
        return {.error = project::CanonicalDocumentError::MissingInput};
    wholeMeasures_.fetch_add(1);
    return project::indexCanonicalDocument({.snapshot = &snapshot,
                                            .colorSettings = colors.get(),
                                            .roundTrip = roundTrip_.get(),
                                            .schemaMinor = schemaMinor_},
                                           std::move(stop));
}

project::CanonicalDocumentIndexResult ProjectCapacityLedger::advance(const Known& from,
                                                                     const document::Snapshot& to) {
    if (from.snapshot->stateIdentity() == to.stateIdentity())
        return {.index = from.index};
    const auto colors = colorSettingsOf(to);
    if (colors == nullptr)
        return {.error = project::CanonicalDocumentError::MissingInput};
    return project::advanceCanonicalDocumentIndex(*from.index, *from.snapshot,
                                                  {.snapshot = &to,
                                                   .colorSettings = colors.get(),
                                                   .roundTrip = roundTrip_.get(),
                                                   .schemaMinor = schemaMinor_});
}

commands::ProjectCapacityMeasure ProjectCapacityLedger::measure(const document::Snapshot& before,
                                                                const document::Snapshot& candidate,
                                                                const Thread thread) {
    try {
        auto from = known();
        if (from.index == nullptr) {
            if (thread == Thread::Owner) {
                // Not known yet (it is being measured in the background): admitted unmeasured,
                // never refused on a guess. The background measure then covers what follows.
                unmeasuredEdits_.fetch_add(1);
                return {};
            }
            // Off the owner thread a preparation measures its own base, once, and shares it.
            auto base = whole(before);
            if (!base)
                return {};
            preparationWholeMeasures_.fetch_add(1);
            offer(before, base.index);
            from = {.snapshot = before, .index = std::move(base.index)};
        }
        auto after = advance(from, candidate);
        if (thread == Thread::Owner) {
            ownerMeasures_.fetch_add(1);
            ownerRecordsCounted_.fetch_add(after.recordsCounted);
            ownerCompositionsCounted_.fetch_add(after.compositionsCounted);
        } else {
            preparationMeasures_.fetch_add(1);
        }
        if (!after) {
            // Past what the format itself can hold: refused, naming the limit it passed. Any
            // other failure is a project a save refuses on its own, with its cause.
            if (const auto excess = project::projectCapacityExcessOf(after.error))
                return {.refusal = refusalOf(*excess), .size = nullptr};
            return {};
        }
        // The first limit the edit takes the project past. An edit that does not grow what is
        // already past a limit (a project opened at that size, a value changed in place, a
        // deletion) stays allowed.
        const project::ProjectCapacityLimits limits;
        std::optional<project::CanonicalDocumentIndexResult> previous;
        for (const auto limit : {project::ProjectCapacityLimit::JsonValues,
                                 project::ProjectCapacityLimit::DocumentBytes,
                                 project::ProjectCapacityLimit::ContainerEntries,
                                 project::ProjectCapacityLimit::StringBytes}) {
            const auto value = limitValue(limit, limits);
            const auto amount = project::projectCapacityAmount(after.index->measure, limit);
            if (amount <= value)
                continue;
            if (!previous)
                previous = advance(from, before);
            if (*previous &&
                project::projectCapacityAmount((*previous).index->measure, limit) >= amount)
                continue;
            return {.refusal = refusalOf({.limit = limit, .value = value, .actual = amount}),
                    .size = nullptr};
        }
        return {.refusal = std::nullopt, .size = std::move(after.index)};
    } catch (const std::bad_alloc&) {
        // Not enough memory to measure: the edit goes on and a save names any failure.
        return {};
    }
}

} // namespace bloom::host
