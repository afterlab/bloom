#include "layer_lock.hpp"
#include "render_footprint.hpp"
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/document/evaluation_inputs.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace bloom::commands {
namespace {

CommandResult makeResult(CommandAction action, CommandStatus status,
                         const document::Snapshot& snapshot, std::string label = {}) {
    return {
        .action = action,
        .status = status,
        .beforeRevision = snapshot.revision(),
        .afterRevision = snapshot.revision(),
        .expectedRevision = std::nullopt,
        .label = std::move(label),
        .operationFailures = {},
        .warnings = {},
        .outputs = {},
        .validation = {},
        .validationOperationIndex = std::nullopt,
        .renderAffecting = true,
        .affectedTimes = std::nullopt,
        .layerIdentityRemaps = std::nullopt,
        .affectedComposition = std::nullopt,
        .evaluationChangedCompositions = std::nullopt,
    };
}

CommandStatus statusForCommit(document::CommitStatus status) {
    switch (status) {
    case document::CommitStatus::Committed:
        return CommandStatus::Succeeded;
    case document::CommitStatus::RevisionConflict:
        return CommandStatus::StaleRevision;
    case document::CommitStatus::InvalidDraft:
        return CommandStatus::ValidationFailed;
    case document::CommitStatus::RevisionOverflow:
        return CommandStatus::RevisionOverflow;
    case document::CommitStatus::ForeignDocument:
        return CommandStatus::ForeignDocument;
    case document::CommitStatus::DraftBaseMismatch:
        return CommandStatus::DraftBaseMismatch;
    }
    return CommandStatus::Rejected;
}

CommandResult resultForCommit(CommandAction action, std::string label,
                              const document::Snapshot& before,
                              document::CommitResult&& commitResult) {
    const auto status = commitResult.status == document::CommitStatus::Committed &&
                                !commitResult.snapshot.has_value()
                            ? CommandStatus::Rejected
                            : statusForCommit(commitResult.status);
    auto result = makeResult(action, status, before, std::move(label));
    result.validation = std::move(commitResult.validation);
    if (commitResult.status == document::CommitStatus::RevisionConflict) {
        result.expectedRevision = before.revision();
    }
    if (commitResult.snapshot.has_value()) {
        result.afterRevision = commitResult.snapshot->revision();
    }
    return result;
}

// The operation that made a transaction's draft invalid: the first prefix that no longer
// validates. Runs only after a refusal, so a valid transaction never pays for it.
std::optional<std::size_t> firstInvalidOperation(const document::Snapshot& before,
                                                 const Transaction& transaction) {
    document::Draft draft = document::Draft::of(before);
    std::size_t index = 0;
    for (const auto& operation : transaction.operations()) {
        static_cast<void>(operation->apply(draft));
        static_cast<void>(reconcileEntryLayers(before.project(), draft));
        if (!draft.validate().ok())
            return index;
        ++index;
    }
    return std::nullopt;
}

} // namespace

// Counts each history-owned shell and immutable payload once. Chunks also reachable from the
// active document do not consume the history allowance. This is an authored-content estimate,
// not allocator RSS (which depends on libc and vector spare capacity).
//
// #744: a composition's content is charged as its own allocation (contentShellByteCharge) plus one
// chunk per record (curve, parameter, node, edge, layer), and a record is held by every content
// that shares it. Consecutive states share every record an edit did not touch, so a step costs
// what it changed and undo depth does not shrink as the project grows. A record is counted while
// any retained content holds it; a content's records are visited only when that content enters or
// leaves the charge.
class HistoryCharge final {
  public:
    void clear() noexcept {
        chunks_.clear();
        total_ = {};
    }

    template <typename State> void add(const State& state) {
        const auto root = state.rootByteCharge();
        addChunk(state.stateIdentity(),
                 root > std::numeric_limits<std::size_t>::max() - sizeof(document::IdAllocator)
                     ? std::numeric_limits<std::size_t>::max()
                     : root + sizeof(document::IdAllocator));
        for (const auto& composition : state.project().compositions())
            addContent(composition);
        for (const auto& record : state.project().extensionRecords())
            if (record.payload.storageIdentity() != nullptr)
                addChunk(record.payload.storageIdentity(), record.payload.size());
        for (const auto& block : state.project().typedDataBlocks())
            if (const auto* payload = std::get_if<document::OpaqueExtensionPayload>(&block.payload);
                payload != nullptr && payload->storageIdentity() != nullptr)
                addChunk(payload->storageIdentity(), payload->size());
    }

    template <typename State> void remove(const State& state) noexcept {
        removeChunk(state.stateIdentity());
        for (const auto& composition : state.project().compositions())
            removeContent(composition);
        for (const auto& record : state.project().extensionRecords())
            if (record.payload.storageIdentity() != nullptr)
                removeChunk(record.payload.storageIdentity());
        for (const auto& block : state.project().typedDataBlocks())
            if (const auto* payload = std::get_if<document::OpaqueExtensionPayload>(&block.payload);
                payload != nullptr && payload->storageIdentity() != nullptr)
                removeChunk(payload->storageIdentity());
    }

    // A commit shares the staged root with the newly published history entry; restore instead
    // makes a new root shell. Both paths share immutable composition and extension payloads.
    template <typename State>
    [[nodiscard]] std::size_t retainedBytes(const State& active, bool sharedRoot) const {
        auto retained = total_;
        const auto subtract = [&](const void* identity) {
            if (const auto entry = chunks_.find(identity); entry != chunks_.end())
                retained.subtract(entry->second.bytes);
        };
        if (sharedRoot)
            subtract(active.stateIdentity());
        std::unordered_set<const void*> seen;
        for (const auto& composition : active.project().compositions()) {
            if (!seen.insert(composition.contentIdentity()).second)
                continue;
            if (chunks_.contains(composition.contentIdentity())) {
                // A charged content's records are all charged: its own charge is their sum.
                retained.subtract(composition.contentByteCharge());
            } else {
                composition.forEachRecord([&](const void* identity, const auto&) {
                    if (seen.insert(identity).second)
                        subtract(identity);
                });
            }
        }
        for (const auto& record : active.project().extensionRecords())
            if (record.payload.storageIdentity() != nullptr &&
                seen.insert(record.payload.storageIdentity()).second)
                subtract(record.payload.storageIdentity());
        for (const auto& block : active.project().typedDataBlocks())
            if (const auto* payload = std::get_if<document::OpaqueExtensionPayload>(&block.payload);
                payload != nullptr && payload->storageIdentity() != nullptr &&
                seen.insert(payload->storageIdentity()).second)
                subtract(payload->storageIdentity());
        return retained.high == 0 ? retained.low : std::numeric_limits<std::size_t>::max();
    }

    // The bytes, roots aside, that only `to` holds and that only `from` holds. A one-step history
    // of the two retains both roots plus one of these when the other state is current, as
    // building a charge of the two states and asking retainedBytes() of each would say, at the
    // cost of the records the step changed.
    template <typename From, typename To>
    [[nodiscard]] static std::pair<std::size_t, std::size_t> stepBytes(const From& from,
                                                                       const To& to) {
        WideBytes onlyTo;
        WideBytes onlyFrom;
        const auto& before = from.project();
        const auto& after = to.project();
        for (const auto& composition : after.compositions()) {
            const auto* previous = before.findComposition(composition.id());
            if (previous != nullptr && previous->contentIdentity() == composition.contentIdentity())
                continue;
            onlyTo.add(composition.contentShellByteCharge());
            if (previous == nullptr) {
                composition.forEachRecord([&](const void*, const auto& record) {
                    onlyTo.add(document::recordByteCharge(record));
                });
                continue;
            }
            onlyFrom.add(previous->contentShellByteCharge());
            const auto changed = [&](const auto earlier, const auto later) {
                const auto diff = document::diffRecords(earlier, later);
                for (const auto* record : diff.added)
                    onlyTo.add(document::recordByteCharge(*record));
                for (const auto* record : diff.removed)
                    onlyFrom.add(document::recordByteCharge(*record));
            };
            changed(previous->parameters().records(), composition.parameters().records());
            changed(previous->animationCurves().records(), composition.animationCurves().records());
            changed(previous->graph().nodes(), composition.graph().nodes());
            changed(previous->graph().edges(), composition.graph().edges());
            changed(previous->graph().layerOutputs(), composition.graph().layerOutputs());
        }
        for (const auto& composition : before.compositions()) {
            if (after.findComposition(composition.id()) != nullptr)
                continue;
            onlyFrom.add(composition.contentShellByteCharge());
            composition.forEachRecord([&](const void*, const auto& record) {
                onlyFrom.add(document::recordByteCharge(record));
            });
        }
        const auto payloads = [](const auto& state) {
            std::unordered_map<const void*, std::size_t> found;
            for (const auto& record : state.project().extensionRecords())
                if (record.payload.storageIdentity() != nullptr)
                    found.emplace(record.payload.storageIdentity(), record.payload.size());
            for (const auto& block : state.project().typedDataBlocks())
                if (const auto* payload =
                        std::get_if<document::OpaqueExtensionPayload>(&block.payload);
                    payload != nullptr && payload->storageIdentity() != nullptr)
                    found.emplace(payload->storageIdentity(), payload->size());
            return found;
        };
        const auto fromPayloads = payloads(from);
        const auto toPayloads = payloads(to);
        for (const auto& [identity, bytes] : toPayloads)
            if (!fromPayloads.contains(identity))
                onlyTo.add(bytes);
        for (const auto& [identity, bytes] : fromPayloads)
            if (!toPayloads.contains(identity))
                onlyFrom.add(bytes);
        const auto narrow = [](const WideBytes& bytes) {
            return bytes.high == 0 ? bytes.low : std::numeric_limits<std::size_t>::max();
        };
        return {narrow(onlyTo), narrow(onlyFrom)};
    }

    [[nodiscard]] std::size_t chunkCount() const noexcept { return chunks_.size(); }

  private:
    // A two-word sum remains exact even if the authored-content estimate crosses SIZE_MAX.
    // Saturating before subtracting active chunks would undercount large shared payloads.
    struct WideBytes final {
        std::size_t low = 0;
        std::size_t high = 0;
        void add(const std::size_t bytes) noexcept {
            if (bytes > std::numeric_limits<std::size_t>::max() - low)
                ++high;
            low += bytes;
        }
        void subtract(const std::size_t bytes) noexcept {
            if (bytes > low)
                --high;
            low -= bytes;
        }
    };
    struct Chunk final {
        std::size_t bytes = 0;
        std::size_t references = 0;
    };
    void addChunk(const void* identity, const std::size_t bytes) {
        auto [entry, inserted] = chunks_.try_emplace(identity, Chunk{bytes, 0});
        ++entry->second.references;
        if (inserted)
            total_.add(bytes);
    }
    void removeChunk(const void* identity) noexcept {
        auto entry = chunks_.find(identity);
        if (entry == chunks_.end())
            return;
        if (--entry->second.references == 0) {
            total_.subtract(entry->second.bytes);
            chunks_.erase(entry);
        }
    }
    void addContent(const document::Composition& composition) {
        auto [entry, inserted] = chunks_.try_emplace(
            composition.contentIdentity(), Chunk{composition.contentShellByteCharge(), 0});
        ++entry->second.references;
        if (!inserted)
            return;
        total_.add(entry->second.bytes);
        composition.forEachRecord([&](const void* identity, const auto& record) {
            auto [held, added] = chunks_.try_emplace(identity, Chunk{0, 0});
            if (added) {
                held->second.bytes = document::recordByteCharge(record);
                total_.add(held->second.bytes);
            }
            ++held->second.references;
        });
    }
    void removeContent(const document::Composition& composition) noexcept {
        auto entry = chunks_.find(composition.contentIdentity());
        if (entry == chunks_.end())
            return;
        if (--entry->second.references != 0)
            return;
        total_.subtract(entry->second.bytes);
        chunks_.erase(entry);
        composition.forEachRecord(
            [&](const void* identity, const auto&) { removeChunk(identity); });
    }

    std::unordered_map<const void*, Chunk> chunks_;
    WideBytes total_;
};

CommandStack::CommandStack(document::Document& document)
    : CommandStack(document, HistoryLimits{}) {}

CommandStack::CommandStack(document::Document& document, const HistoryLimits limits)
    : limits_(limits), historyCharge_(std::make_unique<HistoryCharge>()),
      trackedRevision_(document.snapshot().revision()), document_(document) {
    if (limits_.maxEntries == 0 || limits_.maxChargedBytes == 0 ||
        limits_.maxChargedBytes > std::numeric_limits<std::size_t>::max() / 2)
        throw std::invalid_argument("Invalid Bloom undo history limits");
}

CommandStack::~CommandStack() = default;

CommandObserverId CommandStack::addObserver(CommandObserver observer) {
    if (!observer) {
        return 0;
    }
    const std::scoped_lock lock(observerMutex_);
    if (nextObserverId_ == 0) {
        return 0;
    }
    const auto id = nextObserverId_++;
    if (nextObserverId_ == 0) {
        nextObserverId_ = 0;
    }
    observers_.emplace_back(id, std::move(observer));
    return id;
}

void CommandStack::removeObserver(const CommandObserverId observerId) noexcept {
    if (observerId == 0) {
        return;
    }
    const std::scoped_lock lock(observerMutex_);
    std::erase_if(observers_,
                  [observerId](const auto& entry) { return entry.first == observerId; });
}

void CommandStack::notify(const CommandResult& result) const noexcept {
    std::vector<CommandObserver> callbacks;
    try {
        const std::scoped_lock lock(observerMutex_);
        callbacks.reserve(observers_.size());
        for (const auto& [id, observer] : observers_) {
            static_cast<void>(id);
            callbacks.push_back(observer);
        }
    } catch (...) {
        return;
    }

    // #744: one event serves every observer and both kinds; a result can carry hundreds of
    // thousands of outputs, so it is copied once, not once per observer and kind.
    std::optional<CommandEvent> event;
    try {
        event.emplace(CommandEvent{.kind = CommandEventKind::Rejected, .result = result});
        event->result.origin = currentOrigin_;
    } catch (...) {
        return;
    }
    const auto send = [&callbacks, &event](const CommandEventKind kind) noexcept {
        event->kind = kind;
        for (const auto& callback : callbacks) {
            try {
                callback(*event);
            } catch (...) {
                // Observers cannot change the command outcome or interrupt history publication.
                static_cast<void>(0);
            }
        }
    };
    if (!result.succeeded()) {
        send(CommandEventKind::Rejected);
        return;
    }
    if (result.changed()) {
        send(CommandEventKind::RevisionChanged);
    }
    send(CommandEventKind::HistoryChanged);
}

struct PreparedTransaction::Detail final {
    explicit Detail(const document::Snapshot& baseValue, Transaction&& transactionValue)
        : base(baseValue), transaction(std::move(transactionValue)), origin(transaction.origin()) {}

    document::Snapshot base;
    Transaction transaction;
    CommandOrigin origin = CommandOrigin::User;
    bool cancelled = false;
    // The transaction expected another revision than the base's, so nothing was applied: publish
    // refuses it as stale, or the owner re-bases it on the revision it expects.
    bool unapplied = false;
    // An operation refusal, a reconcile refusal or NoChange, reported as is once the stale checks
    // pass.
    std::optional<CommandResult> terminal;
    // The validated, frozen state to publish; not ready() for an invalid draft, which carries
    // the index of the first operation that made it so.
    std::optional<document::PreparedCommit> commit;
    std::optional<std::size_t> validationOperationIndex;
    std::vector<CommandOutput> outputs;
    std::vector<OperationFailure> warnings;
    bool renderAffecting = false;
    std::optional<AffectedTimeFootprint> affectedTimes;
    std::optional<std::vector<LayerIdentityRemap>> layerIdentityRemaps;
    std::optional<document::CompositionId> affectedComposition;
    std::optional<std::vector<document::CompositionId>> evaluationChangedCompositions;
    CommandStack::Work work;
};

PreparedTransaction::PreparedTransaction(std::unique_ptr<Detail> detail) noexcept
    : detail_(std::move(detail)) {}
PreparedTransaction::PreparedTransaction(PreparedTransaction&&) noexcept = default;
PreparedTransaction& PreparedTransaction::operator=(PreparedTransaction&&) noexcept = default;
PreparedTransaction::~PreparedTransaction() = default;

document::Revision PreparedTransaction::baseRevision() const noexcept {
    return detail_->base.revision();
}
std::string_view PreparedTransaction::label() const noexcept {
    return detail_->transaction.label();
}
CommandOrigin PreparedTransaction::origin() const noexcept { return detail_->origin; }
std::optional<document::Revision> PreparedTransaction::expectedRevision() const noexcept {
    return detail_->transaction.expectedRevision();
}
bool PreparedTransaction::cancelled() const noexcept { return detail_->cancelled; }
Transaction PreparedTransaction::takeTransaction() && { return std::move(detail_->transaction); }

CommandResult CommandStack::execute(Transaction&& transaction) {
    const auto origin = transaction.origin();
    currentOrigin_ = origin;
    const document::Snapshot before = document_.snapshot();
    auto result = publishStep(prepare(before, std::move(transaction)));
    result.origin = origin;
    currentOrigin_ = CommandOrigin::User;
    return result;
}

CommandResult CommandStack::publish(PreparedTransaction&& prepared) {
    const auto origin = prepared.origin();
    currentOrigin_ = origin;
    auto result = publishStep(std::move(prepared));
    result.origin = origin;
    currentOrigin_ = CommandOrigin::User;
    return result;
}

bool CommandStack::rebaseNeeded(const PreparedTransaction& prepared) const {
    const auto live = document_.snapshot().revision();
    if (live == prepared.baseRevision() || trackedRevision_ != live)
        return false;
    const auto expected = prepared.expectedRevision();
    return !expected.has_value() || *expected == live;
}

PreparedTransaction CommandStack::prepare(const document::Snapshot& before,
                                          Transaction&& transaction,
                                          const std::function<bool()>& cancelled,
                                          const std::function<void(std::size_t, std::size_t)>& progress) {
    auto owned = std::make_unique<PreparedTransaction::Detail>(before, std::move(transaction));
    auto& prepared = *owned;
    const auto& ops = prepared.transaction;
    const auto label = [&] { return std::string(ops.label()); };
    const auto stop = [&] {
        if (cancelled && cancelled()) {
            prepared.cancelled = true;
            return true;
        }
        return false;
    };
    if (ops.expectedRevision().has_value() && *ops.expectedRevision() != before.revision()) {
        prepared.unapplied = true;
        return PreparedTransaction(std::move(owned));
    }
    if (ops.empty()) {
        prepared.terminal =
            makeResult(CommandAction::Execute, CommandStatus::NoChange, before, label());
        return PreparedTransaction(std::move(owned));
    }
    if (stop())
        return PreparedTransaction(std::move(owned));

    auto& work = prepared.work;
    document::Draft draft = document::Draft::of(before);
    bool changed = false;
    bool renderAffecting = false;
    // True as soon as an applied render-affecting operation did NOT prove a finite footprint, which
    // makes the transaction's affected time the conservative whole render.
    bool wholeRender = false;
    // Whether any applied render-affecting operation published its own finite footprint.
    bool publishedFootprint = false;
    std::optional<AffectedTimeFootprint> affectedTimes;
    // Issue #355. The one composition whose parameter/animation-only operations deferred their
    // footprint to a single before/after diff once every operation has applied.
    std::optional<document::CompositionId> parameterScope;
    // #355. The one composition every applied render-affecting operation is confined to.
    std::optional<document::CompositionId> renderScope;
    bool renderScopeKnown = true;
    // SPLIT-1. Ordered remaps, appended in operation order. Any unknown/mixed/cap failure clears
    // them and forces whole render.
    std::vector<LayerIdentityRemap> layerIdentityRemaps;
    bool remapsValid = true;
    std::vector<CommandOutput> outputs;
    std::vector<OperationFailure> warnings;
    std::size_t operationIndex = 0;
    for (const auto& operation : ops.operations()) {
        if (operationIndex != 0 && stop())
            return PreparedTransaction(std::move(owned));
        OperationResult operationResult = operation->apply(draft);
        ++work.operationApplies;
        ++work.lockedLayerChecks;
        std::string lockedWhat;
        if (operationResult.status != OperationStatus::Rejected &&
            detail::changesLockedLayers(before.project(), draft.project(), &lockedWhat))
            operationResult =
                OperationResult::rejected(OperationIssueCode::InvalidValue, std::move(lockedWhat));
        if (operationResult.status == OperationStatus::Rejected) {
            auto result =
                makeResult(CommandAction::Execute, CommandStatus::Rejected, before, label());
            if (operationResult.issues.empty()) {
                operationResult.issues.push_back(
                    {OperationIssueCode::InvalidValue,
                     "Operation " + std::string(operation->typeId()) + " (number " +
                         std::to_string(operationIndex) +
                         " in the transaction) rejected the edit without naming a cause"});
            }
            for (auto& issue : operationResult.issues) {
                result.operationFailures.push_back(
                    {operationIndex, std::string(operation->typeId()), std::move(issue)});
            }
            prepared.terminal = std::move(result);
            return PreparedTransaction(std::move(owned));
        }
        for (auto& output : operationResult.outputs) {
            outputs.push_back(
                {operationIndex, std::string(operation->typeId()), std::move(output)});
        }
        for (auto& issue : operationResult.issues)
            warnings.push_back(
                {operationIndex, std::string(operation->typeId()), std::move(issue)});
        changed = changed || operationResult.status == OperationStatus::Applied;
        // Only an operation that actually applied can change pixels. A pixel-affecting operation
        // that reported NoChange contributes nothing, so a layout edit beside it stays layout-only.
        if (operationResult.status == OperationStatus::Applied && operation->renderAffecting()) {
            renderAffecting = true;
            publishedFootprint = publishedFootprint || operationResult.affectedTimes.has_value();
            const auto scope = operationResult.affectedTimes.has_value()
                                   ? std::optional(operationResult.affectedTimes->compositionId)
                                   : operation->renderScope();
            if (!scope.has_value() || (renderScope.has_value() && *renderScope != *scope))
                renderScopeKnown = false;
            else
                renderScope = scope;
            // A proven finite footprint narrows the affected time; an operation that proved none
            // (the default) dominates to the whole render. A mixed-composition union also fails
            // closed to whole render.
            if (operationResult.affectedTimes.has_value() && remapsValid) {
                const auto merged =
                    mergeAffectedTimeFootprints(affectedTimes, *operationResult.affectedTimes);
                if (!merged.has_value()) {
                    wholeRender = true;
                    remapsValid = false;
                } else {
                    affectedTimes = merged;
                    // Ordered remaps ride alongside a finite footprint. Every remap must name the
                    // SAME composition as the finite footprint it accompanies, or the whole
                    // transaction fails closed: normalizing remaps among themselves cannot catch a
                    // footprint/remap composition mismatch. A malformed descriptor, an invalid
                    // composition id, an inverted range, or a cap overflow also fails closed.
                    if (operationResult.layerIdentityRemaps.has_value()) {
                        for (const auto& remap : *operationResult.layerIdentityRemaps) {
                            if (remap.compositionId != merged->compositionId) {
                                wholeRender = true;
                                remapsValid = false;
                                break;
                            }
                        }
                    }
                    if (remapsValid && operationResult.layerIdentityRemaps.has_value()) {
                        const auto normalized = normalizeLayerIdentityRemaps([&] {
                            auto combined = layerIdentityRemaps;
                            combined.insert(combined.end(),
                                            operationResult.layerIdentityRemaps->begin(),
                                            operationResult.layerIdentityRemaps->end());
                            return combined;
                        }());
                        if (normalized.has_value())
                            layerIdentityRemaps = *normalized;
                        else {
                            wholeRender = true;
                            remapsValid = false;
                        }
                    }
                }
            } else if (const auto parameterEdit = operation->parameterEditScope();
                       !operationResult.affectedTimes.has_value() && parameterEdit.has_value() &&
                       remapsValid && (!parameterScope || *parameterScope == *parameterEdit)) {
                parameterScope = parameterEdit;
            } else {
                // An operation that proved no finite footprint, or a transaction already failed
                // closed on remaps, dominates to whole render and can never publish remaps.
                wholeRender = true;
                remapsValid = false;
            }
        }
        ++operationIndex;
        if (progress)
            progress(operationIndex, ops.operations().size());
    }

    // #502. Entries and Layer nodes reconcile once every operation has applied, in the same draft
    // and therefore the same undo step: an inserted Layer node adopts its entry's values, a removed
    // one returns them, and entry layers whose slot vanished leave with their parameters.
    if (stop())
        return PreparedTransaction(std::move(owned));
    if (changed) {
        ++work.entryLayerReconcilePasses;
        EntryLayerReconcileWork reconcileWork;
        auto reconciled = reconcileEntryLayers(before.project(), draft, &reconcileWork);
        work.entryLayerReconcileSkips += reconcileWork.compositionsSkipped;
        work.entryLayerReconcileCompositions += reconcileWork.compositionsReconciled;
        ++work.lockedLayerChecks;
        std::string lockedWhat;
        if (!reconciled.failure && reconciled.changed &&
            detail::changesLockedLayers(before.project(), draft.project(), &lockedWhat))
            reconciled.failure =
                OperationIssue{OperationIssueCode::InvalidValue, std::move(lockedWhat)};
        if (reconciled.failure) {
            auto result =
                makeResult(CommandAction::Execute, CommandStatus::Rejected, before, label());
            result.operationFailures.push_back(
                {ops.size(), "bloom.merge.reconcile-entry-layers", std::move(*reconciled.failure)});
            prepared.terminal = std::move(result);
            return PreparedTransaction(std::move(owned));
        }
        if (reconciled.changed) {
            ++work.entryLayerReconcileEdits;
            // The pass keeps the picture, but it edits records the operations' footprints did not
            // describe: publish the conservative whole render.
            renderAffecting = true;
            wholeRender = true;
            remapsValid = false;
        }
    }

    // #399. Render impact is proven from the document, not only declared by the operations. A
    // transaction whose before and after present identical evaluation inputs
    // (document/evaluation_inputs.hpp) is render-neutral whatever its operations left at the
    // conservative default -- a layer rename, a label color, a group frame, a field added later
    // to a presentation record -- so caches keyed on the retained evaluation snapshot stay valid.
    // Explicit evidence an operation published (a finite footprint, identity remaps) is kept as
    // published. The proof also checks the opt-outs: a transaction whose operations all declared
    // themselves render-neutral but which changed an evaluation input fails closed to a whole
    // render.
    if (changed) {
        ++work.evaluationInputProofs;
        auto inputChanges =
            document::evaluationInputChanges(before.project(), std::as_const(draft).project());
        const bool sameInputs = inputChanges.same();
        if (!inputChanges.project)
            prepared.evaluationChangedCompositions = std::move(inputChanges.compositions);
        if (sameInputs && !publishedFootprint) {
            renderAffecting = false;
        } else if (!sameInputs && !renderAffecting) {
            renderAffecting = true;
            wholeRender = true;
            remapsValid = false;
            renderScopeKnown = false;
        }
    }

    if (changed && renderAffecting && !wholeRender && remapsValid && parameterScope.has_value()) {
        ++work.parameterFootprintDiffs;
        const auto proven = detail::parameterEditFootprint(
            before.project(), std::as_const(draft).project(), *parameterScope);
        const auto merged =
            proven ? mergeAffectedTimeFootprints(affectedTimes, *proven) : std::nullopt;
        if (merged.has_value()) {
            affectedTimes = merged;
        } else {
            wholeRender = true;
            remapsValid = false;
        }
    }

    if (!changed) {
        auto result = makeResult(CommandAction::Execute, CommandStatus::NoChange, before, label());
        result.outputs = std::move(outputs);
        result.warnings = std::move(warnings);
        prepared.terminal = std::move(result);
        return PreparedTransaction(std::move(owned));
    }
    if (stop())
        return PreparedTransaction(std::move(owned));

    // Everything Document::commit does before its lock: the incremental validation (the whole one
    // only for a draft it cannot prove) and the frozen state to publish.
    prepared.commit = document::Document::prepare(draft);
    if (!prepared.commit->ready())
        prepared.validationOperationIndex = firstInvalidOperation(before, ops);
    prepared.outputs = std::move(outputs);
    prepared.warnings = std::move(warnings);
    prepared.renderAffecting = renderAffecting;
    // A finite footprint is published only when the whole transaction's render effect is confined
    // to it. A layout-only transaction (renderAffecting false) publishes none, matching its
    // render-neutral semantics.
    if (renderAffecting && !wholeRender && remapsValid && affectedTimes.has_value()) {
        prepared.affectedTimes = affectedTimes;
        // Remaps require the whole transaction to be finite; a nonempty set is published only when
        // every descriptor validated. An empty set is a legitimate "no remaps".
        const auto normalized = normalizeLayerIdentityRemaps(layerIdentityRemaps);
        if (normalized.has_value())
            prepared.layerIdentityRemaps = *normalized;
        else
            prepared.affectedTimes.reset();
    }
    if (renderAffecting && renderScopeKnown)
        prepared.affectedComposition = renderScope;
    return PreparedTransaction(std::move(owned));
}

CommandResult CommandStack::publishStep(PreparedTransaction&& preparedValue) {
    auto& prepared = *preparedValue.detail_;
    const auto label = [&] { return std::string(prepared.transaction.label()); };
    const auto addWork = [this](const Work& done) {
        work_.operationApplies += done.operationApplies;
        work_.lockedLayerChecks += done.lockedLayerChecks;
        work_.evaluationInputProofs += done.evaluationInputProofs;
        work_.parameterFootprintDiffs += done.parameterFootprintDiffs;
        work_.entryLayerReconcilePasses += done.entryLayerReconcilePasses;
        work_.entryLayerReconcileSkips += done.entryLayerReconcileSkips;
        work_.entryLayerReconcileCompositions += done.entryLayerReconcileCompositions;
        work_.entryLayerReconcileEdits += done.entryLayerReconcileEdits;
    };
    addWork(prepared.work);
    const document::Snapshot before = document_.snapshot();
    if (const auto stale = staleResult(CommandAction::Execute, label(), before)) {
        auto result = *stale;
        notify(result);
        return result;
    }
    if (const auto expected = prepared.transaction.expectedRevision();
        expected.has_value() && *expected != before.revision()) {
        auto result =
            makeResult(CommandAction::Execute, CommandStatus::StaleRevision, before, label());
        result.expectedRevision = expected;
        notify(result);
        return result;
    }
    // #744 (A2): prepared on a base the document has since moved past (rebaseNeeded() says
    // whether to prepare it again), or never applied.
    if (prepared.unapplied || prepared.cancelled || prepared.base.revision() != before.revision()) {
        auto result =
            makeResult(CommandAction::Execute, CommandStatus::StaleRevision, before, label());
        result.expectedRevision = prepared.base.revision();
        notify(result);
        return result;
    }
    if (prepared.terminal.has_value()) {
        auto result = std::move(*prepared.terminal);
        notify(result);
        return result;
    }
    if (!prepared.commit->ready()) {
        // The document names the refusal (and tells its observers), as commit() would.
        auto failed =
            resultForCommit(CommandAction::Execute, label(), before,
                            document_.publish(before.revision(), std::move(*prepared.commit)));
        if (failed.status == CommandStatus::ValidationFailed)
            failed.validationOperationIndex = prepared.validationOperationIndex;
        notify(failed);
        return failed;
    }
    const document::PreparedCommit& candidate = *prepared.commit;

    // #746: a commit of the open group's origin joins the group's entry when that entry is still
    // the newest and nothing has happened since; the merged entry spans from the state before the
    // group's first commit to this one, and the state in between leaves the history.
    const bool groupCommit = group_.has_value() && prepared.origin == group_->origin;
    bool merge = groupCommit && group_->entryOpen && cursor_ > 0 && cursor_ == history_.size() &&
                 history_.back().after.revision() == before.revision() &&
                 group_->entryAfter == before.revision();
    // Preflight the charge of the new undo step before publication. A single step too large for
    // the policy is refused atomically; older steps are retired only when the new one fits.
    // Undo/redo publish a fresh root shell, so reserve enough for either payload direction.
    // A small rename can still proceed when its large immutable payload is shared by both.
    const auto stepTooLarge = [&](const document::Snapshot& from) {
        // #744: what a one-step history of the two states retains, from the records the step
        // changed rather than from charging both states whole.
        const auto [onlyDraft, onlyFrom] = HistoryCharge::stepBytes(from, candidate);
        work_.chargedSnapshotsVisited += 2;
        const auto rootFrom = from.rootByteCharge() + sizeof(document::IdAllocator);
        const auto rootDraft = candidate.rootByteCharge() + sizeof(document::IdAllocator);
        const auto roots = rootFrom + rootDraft;
        const auto worst = std::max(onlyDraft, onlyFrom);
        return roots < rootFrom || worst > limits_.maxChargedBytes ||
               roots > limits_.maxChargedBytes - worst;
    };
    // A merged step too large for the policy is not refused: the group's entry is sealed and this
    // commit starts a new one, as an ordinary step would.
    if (merge && stepTooLarge(history_.back().before))
        merge = false;
    const document::Snapshot entryBefore = merge ? history_.back().before : before;
    if (!merge && stepTooLarge(before)) {
        // Malformed-draft diagnostics come ahead of resource policy: preparation validated it.
        ++work_.overBudgetValidations;
        if (document_.snapshot().revision() != before.revision()) {
            auto stale =
                makeResult(CommandAction::Execute, CommandStatus::StaleRevision, before, label());
            stale.expectedRevision = before.revision();
            notify(stale);
            return stale;
        }
        if (before.revision().value() == std::numeric_limits<std::uint64_t>::max()) {
            auto overflow = makeResult(CommandAction::Execute, CommandStatus::RevisionOverflow,
                                       before, label());
            notify(overflow);
            return overflow;
        }
        auto result =
            makeResult(CommandAction::Execute, CommandStatus::HistoryLimit, before, label());
        notify(result);
        return result;
    }

    HistoryCharge projected = *historyCharge_;
    for (std::size_t index = cursor_; index < history_.size(); ++index) {
        projected.remove(history_[index].before);
        projected.remove(history_[index].after);
        work_.chargedSnapshotsVisited += 2;
    }
    // A merge replaces the group's entry: its two states leave the charge before the merged ones
    // join it, so the history never holds the state in between.
    if (merge) {
        projected.remove(history_.back().before);
        projected.remove(history_.back().after);
        work_.chargedSnapshotsVisited += 2;
    }
    projected.add(entryBefore);
    projected.add(candidate);
    work_.chargedSnapshotsVisited += 2;
    std::size_t oldestDropped = 0;
    // The entries that may be retired: every one before the new step (or before the group's entry
    // it replaces).
    const std::size_t retirable = merge ? cursor_ - 1 : cursor_;
    auto projectedEntries = retirable + 1;
    const auto worstRetainedBytes = [&] {
        // Every retained state can become current through undo/redo. Restores share payloads
        // but allocate a new root; check every reachable payload set before publishing.
        auto worst = projected.retainedBytes(candidate, false);
        worst = std::max(worst, projected.retainedBytes(entryBefore, false));
        for (std::size_t index = oldestDropped; index < retirable; ++index) {
            worst = std::max(worst, projected.retainedBytes(history_[index].before, false));
            worst = std::max(worst, projected.retainedBytes(history_[index].after, false));
        }
        return worst;
    };
    // The step alone fits (checked above), so retiring every older entry always ends the loop.
    bool entryLimitReached = false;
    bool memoryLimitReached = false;
    while (oldestDropped < retirable) {
        if (projectedEntries > limits_.maxEntries)
            entryLimitReached = true;
        else if (worstRetainedBytes() > limits_.maxChargedBytes)
            memoryLimitReached = true;
        else
            break;
        projected.remove(history_[oldestDropped].before);
        projected.remove(history_[oldestDropped].after);
        ++oldestDropped;
        --projectedEntries;
    }
    const auto currentHistoryCharge = projected.retainedBytes(candidate, true);
    auto stagedCharge = std::make_unique<HistoryCharge>(std::move(projected));

    // Stage every allocation needed for history and event publication before Document::commit.
    // A committed edit must never be left without its newest undo step by a later allocation.
    auto result = makeResult(CommandAction::Execute, CommandStatus::Succeeded, before, label());
    result.outputs = std::move(prepared.outputs);
    result.warnings = std::move(prepared.warnings);
    result.renderAffecting = prepared.renderAffecting;
    result.affectedTimes = prepared.affectedTimes;
    result.layerIdentityRemaps = prepared.layerIdentityRemaps;
    result.affectedComposition = prepared.affectedComposition;
    result.evaluationChangedCompositions = std::move(prepared.evaluationChangedCompositions);
    const bool renderAffecting = prepared.renderAffecting;
    result.historyEntriesDropped = oldestDropped;
    result.historyEntryLimitReached = entryLimitReached;
    result.historyMemoryLimitReached = memoryLimitReached;
    result.historyGrouped = groupCommit;
    HistoryEntry staged{groupCommit ? group_->label : label(),
                        entryBefore,
                        entryBefore,
                        renderAffecting,
                        result.affectedTimes,
                        result.layerIdentityRemaps,
                        result.affectedComposition};
    if (merge) {
        // The merged entry restores across every commit of the group, so it publishes the
        // conservative whole render of what any of them touched on undo and redo: no finite
        // footprint or remaps, and a composition only when every commit was confined to it.
        const auto& previous = history_.back();
        staged.renderAffecting = previous.renderAffecting || renderAffecting;
        staged.affectedTimes.reset();
        staged.layerIdentityRemaps.reset();
        if (!previous.renderAffecting)
            staged.affectedComposition = result.affectedComposition;
        else if (renderAffecting && previous.affectedComposition != result.affectedComposition)
            staged.affectedComposition.reset();
        else
            staged.affectedComposition = previous.affectedComposition;
    }
    history_.reserve(history_.size() + 1);
    static_assert(std::is_nothrow_move_constructible_v<HistoryEntry> &&
                  std::is_nothrow_move_assignable_v<HistoryEntry>);

    // The history charge was staged from the prepared state itself, which publication keeps as the
    // new snapshot's state, so nothing is re-keyed after the swap.
    document::CommitResult commitResult =
        document_.publish(before.revision(), std::move(*prepared.commit));
    if (!commitResult.committed() || !commitResult.snapshot.has_value()) {
        auto failed =
            resultForCommit(CommandAction::Execute, label(), before, std::move(commitResult));
        notify(failed);
        return failed;
    }

    const document::Snapshot after = *commitResult.snapshot;
    result.afterRevision = after.revision();
    staged.after = after;
    if (merge) {
        history_.back() = std::move(staged);
    } else {
        history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
        history_.push_back(std::move(staged));
    }
    history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(oldestDropped));
    cursor_ = history_.size();
    if (groupCommit) {
        ++group_->stats.commits;
        if (!merge)
            ++group_->stats.entries;
        group_->entryOpen = true;
        group_->entryAfter = after.revision();
    } else if (group_.has_value() && group_->entryOpen) {
        // The artist's own edit inside the session: it is its own step, after the group's.
        group_->entryOpen = false;
        ++group_->stats.interruptions;
    }
    historyChargedBytes_ = currentHistoryCharge;
    historyCharge_.swap(stagedCharge);
    work_.historyEntriesRetired += oldestDropped;
    trackedRevision_ = after.revision();
    notify(result);
    return result;
}

CommandResult CommandStack::undo(const CommandOrigin origin) {
    currentOrigin_ = origin;
    auto result = undoStep();
    result.origin = origin;
    currentOrigin_ = CommandOrigin::User;
    if (result.changed() && group_.has_value() && group_->entryOpen) {
        group_->entryOpen = false;
        if (origin != group_->origin)
            ++group_->stats.interruptions;
    }
    return result;
}

void CommandStack::beginGroup(std::string label, const CommandOrigin origin) {
    static_cast<void>(endGroup());
    group_ = OpenGroup{std::move(label), origin, false, {}, {}};
}

HistoryGroupStats CommandStack::endGroup() {
    if (!group_.has_value())
        return {};
    const auto stats = group_->stats;
    group_.reset();
    return stats;
}

CommandResult CommandStack::undoStep() {
    const document::Snapshot before = document_.snapshot();
    if (!canUndo()) {
        auto result = makeResult(CommandAction::Undo, CommandStatus::NothingToUndo, before);
        notify(result);
        return result;
    }

    const HistoryEntry& entry = history_[cursor_ - 1];
    if (const auto stale = staleResult(CommandAction::Undo, entry.label, before)) {
        auto result = *stale;
        notify(result);
        return result;
    }

    const auto restoredHistoryCharge = historyCharge_->retainedBytes(entry.before, false);

    document::CommitResult restoreResult = document_.restore(before.revision(), entry.before);
    if (!restoreResult.committed() || !restoreResult.snapshot.has_value()) {
        auto result =
            resultForCommit(CommandAction::Undo, entry.label, before, std::move(restoreResult));
        notify(result);
        return result;
    }

    const document::Revision restoredRevision = restoreResult.snapshot->revision();
    auto result =
        resultForCommit(CommandAction::Undo, entry.label, before, std::move(restoreResult));
    result.renderAffecting = entry.renderAffecting;
    result.affectedTimes = entry.affectedTimes;
    result.affectedComposition = entry.affectedComposition;
    // Undo reverses order and swaps before/after IDs.
    if (entry.layerIdentityRemaps.has_value() && !entry.layerIdentityRemaps->empty())
        result.layerIdentityRemaps = invertLayerIdentityRemaps(entry.layerIdentityRemaps);
    --cursor_;
    historyChargedBytes_ = restoredHistoryCharge;
    trackedRevision_ = restoredRevision;
    notify(result);
    return result;
}

CommandResult CommandStack::redo(const CommandOrigin origin) {
    currentOrigin_ = origin;
    auto result = redoStep();
    result.origin = origin;
    currentOrigin_ = CommandOrigin::User;
    if (result.changed() && group_.has_value() && group_->entryOpen) {
        group_->entryOpen = false;
        if (origin != group_->origin)
            ++group_->stats.interruptions;
    }
    return result;
}

CommandResult CommandStack::redoStep() {
    const document::Snapshot before = document_.snapshot();
    if (!canRedo()) {
        auto result = makeResult(CommandAction::Redo, CommandStatus::NothingToRedo, before);
        notify(result);
        return result;
    }

    const HistoryEntry& entry = history_[cursor_];
    if (const auto stale = staleResult(CommandAction::Redo, entry.label, before)) {
        auto result = *stale;
        notify(result);
        return result;
    }

    const auto restoredHistoryCharge = historyCharge_->retainedBytes(entry.after, false);

    document::CommitResult restoreResult = document_.restore(before.revision(), entry.after);
    if (!restoreResult.committed() || !restoreResult.snapshot.has_value()) {
        auto result =
            resultForCommit(CommandAction::Redo, entry.label, before, std::move(restoreResult));
        notify(result);
        return result;
    }

    const document::Revision restoredRevision = restoreResult.snapshot->revision();
    auto result =
        resultForCommit(CommandAction::Redo, entry.label, before, std::move(restoreResult));
    result.renderAffecting = entry.renderAffecting;
    result.affectedTimes = entry.affectedTimes;
    result.affectedComposition = entry.affectedComposition;
    // Redo replays the stored FORWARD ordered list.
    result.layerIdentityRemaps = entry.layerIdentityRemaps;
    ++cursor_;
    historyChargedBytes_ = restoredHistoryCharge;
    trackedRevision_ = restoredRevision;
    notify(result);
    return result;
}

std::optional<std::string_view> CommandStack::undoLabel() const noexcept {
    if (!canUndo()) {
        return std::nullopt;
    }
    return history_[cursor_ - 1].label;
}

std::vector<std::string> CommandStack::historyLabels() const {
    std::vector<std::string> labels;
    labels.reserve(history_.size());
    for (const auto& entry : history_)
        labels.push_back(entry.label);
    return labels;
}

std::optional<std::string_view> CommandStack::redoLabel() const noexcept {
    if (!canRedo()) {
        return std::nullopt;
    }
    return history_[cursor_].label;
}

void CommandStack::clear() {
    if (history_.empty() && cursor_ == 0) {
        return;
    }
    history_.clear();
    cursor_ = 0;
    if (group_.has_value())
        group_->entryOpen = false;
    historyChargedBytes_ = 0;
    historyCharge_->clear();
    const auto snapshot = document_.snapshot();
    trackedRevision_ = snapshot.revision();
    notify(makeResult(CommandAction::Execute, CommandStatus::NoChange, snapshot));
}

void CommandStack::discardRedo() {
    if (!canRedo()) {
        return;
    }
    for (std::size_t index = cursor_; index < history_.size(); ++index) {
        historyCharge_->remove(history_[index].before);
        historyCharge_->remove(history_[index].after);
        work_.chargedSnapshotsVisited += 2;
    }
    history_.erase(history_.begin() + static_cast<std::ptrdiff_t>(cursor_), history_.end());
    const auto snapshot = document_.snapshot();
    // The active state is a restored root shell, as after Undo.
    historyChargedBytes_ = historyCharge_->retainedBytes(snapshot, false);
    notify(makeResult(CommandAction::Execute, CommandStatus::NoChange, snapshot));
}

std::optional<CommandResult> CommandStack::staleResult(CommandAction action, std::string label,
                                                       const document::Snapshot& current) const {
    if (trackedRevision_ == current.revision()) {
        return std::nullopt;
    }

    auto result = makeResult(action, CommandStatus::StaleRevision, current, std::move(label));
    result.expectedRevision = trackedRevision_;
    return result;
}

} // namespace bloom::commands
