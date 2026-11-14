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
                    startNext();
                });
        connect(cache_.get(), &PreviewFrameCache::entriesCleared, this,
                [this] { fillQueue_.clear(); });
    }
}

ScopesController::~ScopesController() {
    if (inFlight_)
        inFlight_->handle.cancel();
}

PreviewScopeIdentity ScopesController::identityFor(const runtime::ViewAdjust viewAdjust,
                                                   const std::uint32_t detail) const {
    PreviewScopeIdentity identity;
    identity.settings = settings_;
    identity.settings.detail = detail;
    // The Working signal is measured before the display transform: the Viewer's exposure and
    // gamma do not reach it.
    if (settings_.signal == runtime::ScopeSignal::Display)
        identity.viewAdjust = viewAdjust;
    return identity;
}

void ScopesController::setSettings(const runtime::ScopeMeasureSettings& requested) {
    // The detail is the controller's choice, not the panel's.
    auto settings = requested;
    settings.detail = 0;
    if (settings_ == settings)
        return;
    settings_ = settings;
    // Frames queued for the old settings are measured under the new ones when their turn comes;
    // a retained frame's measure already running is for the old ones and is cancelled (refresh).
    if (settings_.scopes == 0)
        fillQueue_.clear();
    refresh();
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
    // Frames the fill already retained stay queued when it ends: the fill's last frames are
    // retained long before the background measure reaches them. Only new frames stop being queued;
    // hiding the panel or closing every scope empties the queue (setSettings).
    rangeFillActive_ = active;
}

void ScopesController::publish(ScopesReading reading) {
    reading_ = std::move(reading);
    emit readingChanged();
}

void ScopesController::refresh() {
    ++generation_;
    // What is wanted now, if anything has to be measured for it.
    std::optional<Request> wanted;
    ScopesReading reading;
    reading.shown = shown_;
    if (shown_ && settings_.scopes != 0) {
        const auto key = PreviewFrameCacheKey::forIdentity(shown_->identity);
        const auto fullIdentity = identityFor(viewAdjust_, 0);
        const auto liveIdentity = identityFor(viewAdjust_, kLiveDetail);
        PreviewScopeSidecarHandle cached;
        if (cache_ != nullptr) {
            cached = cache_->scopesFor(key, fullIdentity);
            const bool full = cached != nullptr;
            if (!full)
                cached = cache_->scopesFor(key, liveIdentity);
            if (cached != nullptr) {
                ++cacheHits_;
                reading.state = ScopesReading::State::Ready;
                reading.scopes = cached;
                reading.measured = shown_;
                reading.color = cached->color;
                reading.message = cached->note;
                reading.detail = cached->identity.settings.detail;
                // A live sidecar is shown at once and the full measure follows.
                if (!full)
                    wanted = Request{*shown_, key, fullIdentity};
            }
        }
        if (cached == nullptr) {
            // The earlier frame's tables stay on show, labeled, until this frame's arrive.
            reading.state = ScopesReading::State::Measuring;
            reading.scopes = reading_.scopes;
            reading.measured = reading_.measured;
            reading.color = reading_.color;
            reading.detail = reading_.detail;
            reading.behind = reading.scopes != nullptr;
            wanted = Request{*shown_, key, liveIdentity};
        }
    }
    // Latest wins: a measure in flight that is not the wanted one is cancelled (it observes that
    // between its steps) and keeps the slot until its task has finished; the one waiting request is
    // replaced.
    const bool sameInFlight = inFlight_ && inFlight_->shown && !inFlight_->cancelled && wanted &&
                              inFlight_->request.key == wanted->key &&
                              inFlight_->request.identity == wanted->identity;
    if (inFlight_ && inFlight_->shown && !sameInFlight)
        cancelInFlight();
    if (want_ && !wantInFlight_)
        ++measuresSkipped_;
    want_ = std::move(wanted);
    wantInFlight_ = sameInFlight;
    // A retained frame's measure under other settings or another view adjust is dropped too.
    if (inFlight_ && !inFlight_->shown &&
        inFlight_->request.identity != identityFor(viewAdjust_, 0))
        cancelInFlight();
    publish(std::move(reading));
    startNext();
}

void ScopesController::cancelInFlight() {
    if (!inFlight_)
        return;
    inFlight_->cancelled = true;
    inFlight_->handle.cancel();
}

std::optional<ScopesController::Pending> ScopesController::submit(const Request& wanted,
                                                                  const bool shown) {
    if (!measure_ || bridge_.isShuttingDown())
        return std::nullopt;
    const auto& source = wanted.source;
    runtime::TaskRequest request(
        "Measure scopes",
        {.kind = runtime::TaskOwnerKind::PanelRequest,
         .id = runtime::TaskOwnerId::fromRaw(source.identity.compositionId.value())},
        shown ? runtime::TaskPriority::Visible : runtime::TaskPriority::Background);
    request.sourceVersion = {.documentRevision = source.identity.sourceRevision.value(),
                             .requestGeneration = generation_};
    request.usesPurgeableCaches = true;
    auto submission = scheduler_.submit<ResultHandle>(
        std::move(request),
        [measure = measure_, source, identity = wanted.identity](runtime::TaskContext& context) {
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
    return Pending{std::move(submission.handle), wanted, shown, false};
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
    if (inFlight_) {
        if (auto taken = inFlight_->handle.tryTakeResult()) {
            const auto pending = std::move(*inFlight_);
            inFlight_.reset();
            const auto& value = taken->value();
            const bool succeeded =
                taken->state() == runtime::TaskState::Succeeded && value && *value;
            if (succeeded)
                lastTableReadbackBytes_ = (*value)->measured.measure.tableReadbackBytes;
            if (pending.shown) {
                // A result is taken only when it is for what is wanted still: the measure of a
                // frame or identity that is no longer wanted is neither shown nor cached.
                const bool wanted = wantInFlight_ && want_ && want_->key == pending.request.key &&
                                    want_->identity == pending.request.identity;
                if (wanted)
                    wantInFlight_ = false;
                if (wanted && succeeded)
                    takeShown(**value, pending);
                else if (wanted) {
                    want_.reset();
                    if (taken->state() != runtime::TaskState::Cancelled) {
                        ScopesReading reading = reading_;
                        reading.shown = shown_;
                        reading.state = ScopesReading::State::Failed;
                        reading.message = "The scope measure did not finish.";
                        publish(std::move(reading));
                    }
                }
            } else if (succeeded && cache_ != nullptr &&
                       pending.request.identity == identityFor(viewAdjust_, 0)) {
                if (auto sidecar = sidecarOf(**value))
                    static_cast<void>(
                        cache_->attachScopes(pending.request.key, std::move(sidecar)));
            }
        }
    }
    startNext();
}

void ScopesController::takeShown(const Result& result, const Pending& pending) {
    ScopesReading reading = reading_;
    reading.shown = shown_;
    if (auto sidecar = sidecarOf(result)) {
        if (cache_ != nullptr)
            static_cast<void>(cache_->attachScopes(pending.request.key, sidecar));
        reading.state = ScopesReading::State::Ready;
        reading.message = sidecar->note;
        reading.detail = sidecar->identity.settings.detail;
        reading.scopes = std::move(sidecar);
        reading.measured = result.source;
        reading.color = reading.scopes->color;
        reading.behind = false;
        // The live tables are on show: the frame, when it stays, is measured once more in full.
        if (pending.request.identity.settings.detail != 0) {
            auto full = pending.request;
            full.identity.settings.detail = 0;
            want_ = std::move(full);
        } else {
            want_.reset();
        }
    } else {
        want_.reset();
        reading.state = ScopesReading::State::Failed;
        reading.message = result.measured.measure.message.empty()
                              ? std::string("The frame could not be measured.")
                              : result.measured.measure.message;
    }
    publish(std::move(reading));
}

void ScopesController::startNext() {
    if (inFlight_)
        return;
    if (want_ && !wantInFlight_) {
        if (startShown())
            return;
        // Could not start: say so rather than leave the reading measuring forever.
        want_.reset();
        ScopesReading reading = reading_;
        reading.shown = shown_;
        reading.state = ScopesReading::State::Failed;
        reading.message = "The scope measure could not be started.";
        publish(std::move(reading));
        return;
    }
    // A retained frame is measured only when no shown-frame request waits or is in flight.
    if (!want_)
        static_cast<void>(startFill());
}

bool ScopesController::startShown() {
    inFlight_ = submit(*want_, true);
    if (!inFlight_)
        return false;
    wantInFlight_ = true;
    maxInFlight_ = std::max<std::uint32_t>(maxInFlight_, 1U);
    return true;
}

bool ScopesController::startFill() {
    if (cache_ == nullptr || settings_.scopes == 0)
        return false;
    const auto identity = identityFor(viewAdjust_, 0);
    while (!fillQueue_.empty()) {
        const auto key = std::move(fillQueue_.front());
        fillQueue_.pop_front();
        const auto source = cache_->scopeSourceFor(key);
        if (!source || cache_->scopesFor(key, identity) != nullptr)
            continue;
        inFlight_ = submit(Request{*source, key, identity}, false);
        if (inFlight_) {
            maxInFlight_ = std::max<std::uint32_t>(maxInFlight_, 1U);
            return true;
        }
    }
    return false;
}

} // namespace bloom::ui
