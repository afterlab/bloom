#include <bloom/ui/scopes_controller.hpp>

#include "composition_preview_stage_shared.hpp"

#include <bloom/runtime/input_color_context.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <algorithm>
#include <bit>
#include <mutex>
#include <utility>
#include <vector>

namespace bloom::ui {
namespace {

// The color objects of a signal are kept for the last few signals a session measured.
constexpr std::size_t kColorCapacity = 8;
// Retained frames waiting for a background measure during a fill.
constexpr std::size_t kFillQueueCapacity = 4096;

[[nodiscard]] bool deviceProvider(const runtime::PreviewDisplayProvider provider) noexcept {
    return provider == runtime::PreviewDisplayProvider::GpuResident ||
           provider == runtime::PreviewDisplayProvider::GpuPreparedPacked;
}

struct ColorCache final {
    struct Entry final {
        std::string key;
        std::shared_ptr<const runtime::ScopeSignalColor> color;
    };
    std::mutex mutex;
    std::vector<Entry> entries; // most recently used last
};

[[nodiscard]] std::string colorKey(const runtime::PreviewRequestIdentity& frame,
                                   const PreviewScopeIdentity& identity) {
    const auto revision = frame.colorIntent.ocioConfigRevision.toLowercaseHex();
    std::string key(frame.colorIntent.ocioConfigUri);
    key += '|';
    key.append(revision.data(), revision.size());
    key += '|';
    key += frame.colorIntent.workingColorSpaceId;
    key += identity.settings.signal == runtime::ScopeSignal::Display ? "|display|" : "|working|";
    if (identity.settings.signal == runtime::ScopeSignal::Display) {
        key += frame.displayName;
        key += '|';
        key += frame.viewName;
        key += '|';
        key += std::to_string(std::bit_cast<std::uint64_t>(identity.viewAdjust.exposure));
        key += '|';
        key += std::to_string(std::bit_cast<std::uint64_t>(identity.viewAdjust.gamma));
    }
    return key;
}

// Blocking (OCIO): worker only.
[[nodiscard]] std::shared_ptr<const runtime::ScopeSignalColor>
colorFor(ColorCache& cache, const runtime::PreviewRequestIdentity& frame,
         const PreviewScopeIdentity& identity, std::string& message) {
    auto key = colorKey(frame, identity);
    {
        const std::lock_guard lock(cache.mutex);
        const auto found = std::ranges::find(cache.entries, key, &ColorCache::Entry::key);
        if (found != cache.entries.end()) {
            std::rotate(found, found + 1, cache.entries.end());
            return cache.entries.back().color;
        }
    }
    auto config = runtime::detail::resolveInputColorConfig(frame.colorIntent);
    if (!config) {
        message = "The project's color config is not available.";
        return nullptr;
    }
    auto prepared = runtime::ScopeSignalColor::prepare(
        {.config = std::make_shared<const color::ResolvedBloomNeutralConfig>(std::move(*config)),
         .signal = identity.settings.signal,
         .display = frame.displayName,
         .view = frame.viewName,
         .viewAdjust = identity.viewAdjust});
    if (!prepared) {
        message = std::move(prepared.message);
        return nullptr;
    }
    const std::lock_guard lock(cache.mutex);
    if (cache.entries.size() >= kColorCapacity)
        cache.entries.erase(cache.entries.begin());
    cache.entries.push_back({std::move(key), prepared.color});
    return prepared.color;
}

} // namespace

ScopesMeasure
makeScopesMeasure(runtime::ScopeFrameRoutes routes,
                  std::function<std::optional<std::filesystem::path>()> assetBaseDirectory,
                  const std::size_t pixelStorageByteLimit) {
    return [routes = std::move(routes), assetBaseDirectory = std::move(assetBaseDirectory),
            pixelStorageByteLimit, colors = std::make_shared<ColorCache>()](
               const PreviewScopeSource& source, const PreviewScopeIdentity& identity,
               const runtime::CancellationToken& cancellation) {
        ScopesMeasured result;
        if (source.process.plan == nullptr) {
            result.measure.message = "The frame carries no plan to measure it from.";
            return result;
        }
        result.color = colorFor(*colors, source.identity, identity, result.measure.message);
        if (result.color == nullptr)
            return result;
        if (cancellation.isCancellationRequested()) {
            result.measure.status = runtime::ScopeFrameStatus::Cancelled;
            return result;
        }
        const auto request = detail::evaluationRequestFor(source.identity, *source.process.plan,
                                                          pixelStorageByteLimit);
        result.measure =
            runtime::measureScopeFrame(routes, source.process.plan, request, identity.settings,
                                       *result.color, deviceProvider(source.provider), cancellation,
                                       assetBaseDirectory ? assetBaseDirectory() : std::nullopt);
        return result;
    };
}

ScopesController::ScopesController(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                                   PreviewFrameCacheHandle cache, ScopesMeasure measure,
                                   QObject* parent)
    : QObject(parent), scheduler_(scheduler), bridge_(bridge), cache_(std::move(cache)),
      measure_(std::move(measure)) {
    settings_.scopes = 0;
    connect(&bridge_, &TaskUiBridge::resultsAvailable, this, &ScopesController::poll);
    if (cache_ != nullptr) {
        connect(cache_.get(), &PreviewFrameCache::entryRetained, this,
                [this](const PreviewFrameCacheKey& key) {
                    if (!rangeFillActive_ || settings_.scopes == 0)
                        return;
                    if (fillQueue_.size() >= kFillQueueCapacity)
                        fillQueue_.pop_front();
                    fillQueue_.push_back(key);
                    startFill();
                });
        connect(cache_.get(), &PreviewFrameCache::entriesCleared, this,
                [this] { fillQueue_.clear(); });
    }
}

ScopesController::~ScopesController() {
    if (active_)
        active_->handle.cancel();
    if (fill_)
        fill_->handle.cancel();
}

PreviewScopeIdentity ScopesController::identityFor(const runtime::ViewAdjust viewAdjust) const {
    PreviewScopeIdentity identity;
    identity.settings = settings_;
    // The Working signal is measured before the display transform: the Viewer's exposure and
    // gamma do not reach it.
    if (settings_.signal == runtime::ScopeSignal::Display)
        identity.viewAdjust = viewAdjust;
    return identity;
}

void ScopesController::setSettings(const runtime::ScopeMeasureSettings& settings) {
    if (settings_ == settings)
        return;
    settings_ = settings;
    // Frames queued for the old settings are measured under the new ones when their turn comes;
    // a background measure already running is for the old ones and is dropped.
    if (fill_) {
        fill_->handle.cancel();
        fill_.reset();
    }
    if (settings_.scopes == 0)
        fillQueue_.clear();
    refresh();
    startFill();
}

void ScopesController::showFrame(const PreparedPreviewFrameHandle& frame,
                                 const runtime::ViewAdjust viewAdjust) {
    std::optional<PreviewScopeSource> shown;
    if (frame != nullptr)
        shown = PreviewScopeSource{frame->desiredIdentity(), frame->processIdentity(),
                                   frame->provenance().provider};
    const bool same = shown.has_value() == shown_.has_value() &&
                      (!shown || (PreviewFrameCacheKey::forIdentity(shown->identity) ==
                                      PreviewFrameCacheKey::forIdentity(shown_->identity) &&
                                  shown->process.plan == shown_->process.plan)) &&
                      viewAdjust == viewAdjust_;
    if (same)
        return;
    shown_ = std::move(shown);
    viewAdjust_ = viewAdjust;
    refresh();
}

void ScopesController::setRangeFillActive(const bool active) {
    if (rangeFillActive_ == active)
        return;
    rangeFillActive_ = active;
    if (!active)
        fillQueue_.clear();
}

void ScopesController::publish(ScopesReading reading) {
    reading_ = std::move(reading);
    emit readingChanged();
}

void ScopesController::refresh() {
    ++generation_;
    if (active_) {
        active_->handle.cancel();
        active_.reset();
    }
    ScopesReading reading;
    reading.shown = shown_;
    if (!shown_ || settings_.scopes == 0) {
        // Nothing is shown from another frame without the frame it belongs to.
        publish(std::move(reading));
        return;
    }
    const auto key = PreviewFrameCacheKey::forIdentity(shown_->identity);
    const auto identity = identityFor(viewAdjust_);
    if (cache_ != nullptr) {
        if (auto cached = cache_->scopesFor(key, identity)) {
            ++cacheHits_;
            reading.state = ScopesReading::State::Ready;
            reading.scopes = std::move(cached);
            reading.measured = shown_;
            reading.color = reading.scopes->color;
            reading.message = reading.scopes->note;
            publish(std::move(reading));
            return;
        }
    }
    // The earlier frame's tables stay on show, labeled, until this frame's arrive.
    reading.state = ScopesReading::State::Measuring;
    reading.scopes = reading_.scopes;
    reading.measured = reading_.measured;
    reading.color = reading_.color;
    reading.behind = reading.scopes != nullptr;
    active_ = submit(*shown_, identity, runtime::TaskPriority::Visible);
    if (!active_) {
        reading.state = ScopesReading::State::Failed;
        reading.message = "The scope measure could not be started.";
    }
    publish(std::move(reading));
}

std::optional<ScopesController::Pending>
ScopesController::submit(const PreviewScopeSource& source, const PreviewScopeIdentity& identity,
                         const runtime::TaskPriority priority) {
    if (!measure_ || bridge_.isShuttingDown())
        return std::nullopt;
    runtime::TaskRequest request(
        "Measure scopes",
        {.kind = runtime::TaskOwnerKind::PanelRequest,
         .id = runtime::TaskOwnerId::fromRaw(source.identity.compositionId.value())},
        priority);
    request.sourceVersion = {.documentRevision = source.identity.sourceRevision.value(),
                             .requestGeneration = generation_};
    request.usesPurgeableCaches = true;
    auto submission = scheduler_.submit<ResultHandle>(
        std::move(request), [measure = measure_, source, identity](runtime::TaskContext& context) {
            using TaskResult = runtime::TaskResult<ResultHandle>;
            if (context.isCancellationRequested())
                return TaskResult::cancelled();
            auto measured = measure(source, identity, context.cancellation());
            if (context.isCancellationRequested() ||
                measured.measure.status == runtime::ScopeFrameStatus::Cancelled)
                return TaskResult::cancelled();
            return TaskResult::succeeded(
                std::make_shared<const Result>(Result{std::move(measured), source, identity}));
        });
    if (!submission.accepted())
        return std::nullopt;
    ++measuresStarted_;
    bridge_.wake();
    return Pending{std::move(submission.handle), PreviewFrameCacheKey::forIdentity(source.identity),
                   identity};
}

PreviewScopeSidecarHandle ScopesController::sidecarOf(const Result& result) const {
    const auto& measure = result.measured.measure;
    if (measure.status != runtime::ScopeFrameStatus::Measured &&
        measure.status != runtime::ScopeFrameStatus::EmptyArea)
        return nullptr;
    auto sidecar = std::make_shared<PreviewScopeSidecar>();
    sidecar->identity = result.identity;
    // An empty area has no tables: the sidecar records that nothing was there to count.
    if (measure.status == runtime::ScopeFrameStatus::Measured)
        sidecar->tables = runtime::ScopeTables::pack(measure.counts, measure.route);
    sidecar->area = measure.area;
    sidecar->note = measure.message;
    sidecar->color = result.measured.color;
    return sidecar;
}

void ScopesController::poll() {
    if (active_) {
        if (auto taken = active_->handle.tryTakeResult()) {
            const auto pending = std::move(*active_);
            active_.reset();
            const auto& value = taken->value();
            ScopesReading reading = reading_;
            reading.shown = shown_;
            if (taken->state() == runtime::TaskState::Succeeded && value && *value) {
                const Result& result = **value;
                if (auto sidecar = sidecarOf(result)) {
                    if (cache_ != nullptr)
                        static_cast<void>(cache_->attachScopes(pending.key, sidecar));
                    reading.state = ScopesReading::State::Ready;
                    reading.message = sidecar->note;
                    reading.scopes = std::move(sidecar);
                    reading.measured = result.source;
                    reading.color = reading.scopes->color;
                    reading.behind = false;
                } else {
                    reading.state = ScopesReading::State::Failed;
                    reading.message = result.measured.measure.message.empty()
                                          ? std::string("The frame could not be measured.")
                                          : result.measured.measure.message;
                }
                publish(std::move(reading));
            } else if (taken->state() != runtime::TaskState::Cancelled) {
                reading.state = ScopesReading::State::Failed;
                reading.message = "The scope measure did not finish.";
                publish(std::move(reading));
            }
        }
    }
    if (fill_) {
        if (auto taken = fill_->handle.tryTakeResult()) {
            const auto pending = std::move(*fill_);
            fill_.reset();
            const auto& value = taken->value();
            if (taken->state() == runtime::TaskState::Succeeded && value && *value &&
                cache_ != nullptr && pending.identity == identityFor(viewAdjust_)) {
                if (auto sidecar = sidecarOf(**value))
                    static_cast<void>(cache_->attachScopes(pending.key, std::move(sidecar)));
            }
        }
    }
    startFill();
}

void ScopesController::startFill() {
    if (fill_ || cache_ == nullptr || settings_.scopes == 0)
        return;
    const auto identity = identityFor(viewAdjust_);
    while (!fillQueue_.empty()) {
        const auto key = std::move(fillQueue_.front());
        fillQueue_.pop_front();
        const auto source = cache_->scopeSourceFor(key);
        if (!source || cache_->scopesFor(key, identity) != nullptr)
            continue;
        fill_ = submit(*source, identity, runtime::TaskPriority::Background);
        if (fill_)
            return;
    }
}

} // namespace bloom::ui
