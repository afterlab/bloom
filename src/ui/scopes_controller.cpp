#include <bloom/ui/scopes_controller.hpp>

#include <bloom/runtime/input_color_context.hpp>
#include <bloom/ui/scope_fill_taps.hpp>
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

namespace {

// A tap's outcome as the measure of a frame.
[[nodiscard]] runtime::ScopeFrameMeasure
measureOfTap(const runtime::GpuScopeTapMeasureOutcome& outcome) {
    runtime::ScopeFrameMeasure measure;
    measure.status = outcome.status == runtime::GpuScopeTapStatus::Measured
                         ? runtime::ScopeFrameStatus::Measured
                         : runtime::ScopeFrameStatus::EmptyArea;
    measure.route = outcome.route;
    measure.source = outcome.source;
    measure.counts = outcome.counts;
    measure.area = outcome.area;
    measure.colorDispatches = outcome.colorDispatches;
    measure.tableReadbacks = outcome.tableReadbacks;
    measure.tableReadbackBytes = outcome.tableReadbackBytes;
    measure.pictureReadbacks = outcome.pictureReadbacks;
    return measure;
}

[[nodiscard]] ScopesMeasured unavailable(ScopesMeasured result, std::string message) {
    result.measure.status = runtime::ScopeFrameStatus::Unavailable;
    result.measure.message = std::move(message);
    return result;
}

} // namespace

ScopesMeasure makeScopesMeasure(ScopesMeasureSources sources) {
    return [sources = std::move(sources), colors = std::make_shared<ColorCache>()](
               const ScopesMeasureInput& input, const PreviewScopeIdentity& identity,
               const runtime::CancellationToken& cancellation) {
        ScopesMeasured result;
        result.detail = identity.settings.detail;
        result.color = colorFor(*colors, input.source.identity, identity, result.measure.message);
        if (result.color == nullptr)
            return result;
        const auto cancelled = [&] {
            result.measure.status = runtime::ScopeFrameStatus::Cancelled;
            return result;
        };
        if (cancellation.isCancellationRequested())
            return cancelled();

        // (a) The scene picture the Viewer's device kept for the frame (a Viewer frame's tap, or
        // the RAM preview fill frame's own).
        const bool fillTapped = input.fillTap != nullptr && input.fillTap->tapped();
        const bool tapExpected =
            fillTapped || (input.fillTap == nullptr && sources.previewService != nullptr &&
                           deviceProvider(input.source.provider));
        if (tapExpected) {
            const auto key = fillTapped ? input.fillTap->key()
                                        : runtime::GpuScopeTapKey::of(input.source.identity);
            const auto ocio = sources.ocio ? sources.ocio() : nullptr;
            if (!fillTapped && !sources.previewService->scopeTapGeometry(key)) {
                result.tapNote = input.priorTapNote.empty()
                                     ? std::string("the Viewer's device holds no picture of this "
                                                   "frame")
                                     : input.priorTapNote;
            } else if (ocio == nullptr) {
                result.tapNote = "the shared color context is not available";
                result.tapRetry = true;
            } else {
                const auto job = runtime::makeScopeMeasureJob(identity.settings, *result.color,
                                                              *ocio);
                const auto outcome = fillTapped
                                         ? input.fillTap->measure(job, cancellation)
                                         : sources.previewService->measureScopeTap(key, job,
                                                                                   cancellation);
                using Status = runtime::GpuScopeTapStatus;
                if (outcome.status == Status::Measured || outcome.status == Status::EmptyArea) {
                    result.measure = measureOfTap(outcome);
                    return result;
                }
                if (outcome.status == Status::Cancelled)
                    return cancelled();
                result.tapNote = outcome.message;
                result.tapRetry = !(outcome.status == Status::Skipped &&
                                    outcome.skip == runtime::GpuScopeTapSkip::TapGone);
            }
        }

        // (b) The CPU route's process picture the frame still holds.
        if (input.frame != nullptr && input.frame->hasProcessFrame()) {
            result.measure = runtime::measureScopeFrameCpu(*input.frame->processFrame(),
                                                           identity.settings, *result.color,
                                                           cancellation);
            return result;
        }
        if (input.renderedOnly)
            return unavailable(std::move(result), result.tapNote.empty()
                                                      ? std::string("no Rendered picture")
                                                      : result.tapNote);

        // (c) The retained display picture.
        if (input.frame == nullptr)
            return unavailable(std::move(result), "The frame is not available to measure.");
        const auto view = input.frame->displayBufferView();
        const auto& plan = input.source.process.plan;
        if (!view || plan == nullptr)
            return unavailable(std::move(result),
                               "The frame holds no picture the scopes can count.");
        if (input.fillTap != nullptr)
            input.fillTap->release();
        auto retained = identity.settings;
        // The CPU counts the same pixels at either detail: the retained picture is counted once,
        // in full.
        retained.detail = 0;
        result.detail = 0;
        result.measure = runtime::measureScopeFrameRetained(
            *view,
            runtime::RetainedScopePicture{plan->format().width(), plan->format().height(),
                                          input.frame->desiredIdentity().viewAdjust},
            retained, *result.color, cancellation);
        if (result.measure.status == runtime::ScopeFrameStatus::Unavailable &&
            !result.tapNote.empty())
            result.measure.message += " (" + result.tapNote + ")";
        return result;
    };
}

ScopesController::ScopesController(runtime::TaskScheduler& scheduler, TaskUiBridge& bridge,
                                   PreviewFrameCacheHandle cache, ScopesMeasure measure,
                                   ScopeFillTaps* const fillTaps, QObject* parent)
    : QObject(parent), scheduler_(scheduler), bridge_(bridge), cache_(std::move(cache)),
      measure_(std::move(measure)), fillTaps_(fillTaps) {
    settings_.scopes = 0;
    connect(&bridge_, &TaskUiBridge::resultsAvailable, this, &ScopesController::poll);
    if (fillTaps_ != nullptr)
        connect(fillTaps_, &ScopeFillTaps::tapsAvailable, this, &ScopesController::startNext);
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
    // A tap this controller holds is answered by letting go of it; the panel no longer wants any.
    if (fillTaps_ != nullptr)
        fillTaps_->setWanted(this, false);
    if (inFlight_) {
        inFlight_->handle.cancel();
        if (inFlight_->request.tap != nullptr)
            inFlight_->request.tap->release();
    }
}

PreviewScopeIdentity ScopesController::identityFor(const runtime::ViewAdjust viewAdjust,
                                                   const std::uint32_t detail,
                                                   const runtime::ScopeSource source) const {
    PreviewScopeIdentity identity;
    identity.settings = settings_;
    identity.settings.detail = detail;
    identity.source = source;
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
    // A panel with a scope open wants the RAM preview fill's taps; one with none (hidden, or every
    // section collapsed) does not, and the taps already received are dropped.
    if (fillTaps_ != nullptr)
        fillTaps_->setWanted(this, settings_.scopes != 0);
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
                                  shown->process.plan == shown_->process.plan &&
                                  // The same picture with its process picture, or without it,
                                  // is measured from what it holds.
                                  frame->hasProcessFrame() == shownFrame_->hasProcessFrame())) &&
                      viewAdjust == viewAdjust_;
    if (same) {
        // The same picture under a newer request: measures that start from now on use this handle.
        shownFrame_ = frame;
        return;
    }
    shown_ = std::move(shown);
    shownFrame_ = frame;
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

std::string ScopesController::priorNote(const PreviewFrameCacheKey& key) const {
    return fillTaps_ != nullptr ? fillTaps_->failureFor(key) : std::string{};
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
        bool full = false;
        if (cache_ != nullptr) {
            // The tables the frame's entry holds, best first: Rendered full, Rendered live,
            // Retained full, Retained live.
            const auto lookup = [&](const std::uint32_t detail, const runtime::ScopeSource source) {
                return cache_->scopesFor(key, identityFor(viewAdjust_, detail, source));
            };
            if ((cached = lookup(0, runtime::ScopeSource::Rendered)) != nullptr)
                full = true;
            else if ((cached = lookup(kLiveDetail, runtime::ScopeSource::Rendered)) != nullptr)
                full = false;
            else if ((cached = lookup(0, runtime::ScopeSource::Retained)) != nullptr)
                full = true;
            else
                cached = lookup(kLiveDetail, runtime::ScopeSource::Retained);
            if (cached != nullptr) {
                ++cacheHits_;
                reading.state = ScopesReading::State::Ready;
                reading.scopes = cached;
                reading.measured = shown_;
                reading.color = cached->color;
                reading.message = cached->note;
                reading.detail = cached->identity.settings.detail;
                // A live sidecar is shown at once and the full measure follows; Rendered live
                // tables are replaced only by Rendered full ones.
                if (!full)
                    wanted = Request{*shown_,
                                     key,
                                     fullIdentity,
                                     shownFrame_,
                                     nullptr,
                                     cached->identity.source == runtime::ScopeSource::Rendered,
                                     priorNote(key)};
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
            wanted = Request{*shown_, key, liveIdentity, shownFrame_, nullptr, false,
                             priorNote(key)};
        }
    }
    // Latest wins: a measure in flight that is not the wanted one is cancelled (it observes that
    // between its steps) and keeps the slot until its task has finished; the one waiting request is
    // replaced.
    const bool sameInFlight = inFlight_ && inFlight_->shown && !inFlight_->cancelled && wanted &&
                              inFlight_->request.key == wanted->key &&
                              inFlight_->request.identity == wanted->identity &&
                              inFlight_->request.renderedOnly == wanted->renderedOnly;
    if (inFlight_ && inFlight_->shown && !sameInFlight)
        cancelInFlight();
    if (want_ && !wantInFlight_)
        ++measuresSkipped_;
    want_ = std::move(wanted);
    wantInFlight_ = sameInFlight;
    // A fill frame's measure under other settings or another view adjust is dropped too.
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
    // A fill tap that was not measured is answered now rather than when its task is reaped.
    if (inFlight_->request.tap != nullptr && !inFlight_->shown)
        inFlight_->request.tap->release();
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
        [measure = measure_,
         input = ScopesMeasureInput{source, wanted.frame, wanted.tap, wanted.renderedOnly,
                                    wanted.priorTapNote},
         source,
         identity = wanted.identity](runtime::TaskContext& context) {
            using TaskResult = runtime::TaskResult<ResultHandle>;
            if (context.isCancellationRequested())
                return TaskResult::cancelled();
            auto measured = measure(input, identity, context.cancellation());
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

PreviewScopeSidecarHandle
ScopesController::sidecarOf(const Result& result, const PreviewScopeIdentity& asked) const {
    const auto& measure = result.measured.measure;
    if (measure.status != runtime::ScopeFrameStatus::Measured &&
        measure.status != runtime::ScopeFrameStatus::EmptyArea)
        return nullptr;
    auto sidecar = std::make_shared<PreviewScopeSidecar>();
    sidecar->identity = asked;
    // The sidecar names what was counted: the picture and the detail it was counted at.
    sidecar->identity.source = measure.source;
    sidecar->identity.settings.detail = result.measured.detail;
    // An empty area has no tables: the sidecar records that nothing was there to count.
    if (measure.status == runtime::ScopeFrameStatus::Measured)
        sidecar->tables = runtime::ScopeTables::pack(measure.counts, measure.route);
    sidecar->area = measure.area;
    sidecar->note = measure.message;
    if (measure.source == runtime::ScopeSource::Retained) {
        sidecar->note = "counted from the cached picture";
        if (!result.measured.tapNote.empty())
            sidecar->note += ": " + result.measured.tapNote;
    }
    sidecar->color = result.measured.color;
    return sidecar;
}

void ScopesController::poll() {
    if (inFlight_) {
        if (auto taken = inFlight_->handle.tryTakeResult()) {
            const auto pending = std::move(*inFlight_);
            inFlight_.reset();
            // A tap not measured by now is answered: the task may never have run.
            if (pending.request.tap != nullptr)
                pending.request.tap->release();
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
            } else {
                takeFill(succeeded ? value->get() : nullptr, pending);
            }
        }
    }
    startNext();
}

void ScopesController::takeFill(const Result* const result, const Pending& pending) {
    const auto& request = pending.request;
    const bool current = settings_.scopes != 0 && request.identity == identityFor(viewAdjust_, 0);
    PreviewScopeSidecarHandle sidecar;
    if (result != nullptr && current)
        sidecar = sidecarOf(*result, request.identity);
    if (request.tap == nullptr) {
        // A retained frame's measure.
        if (sidecar != nullptr && cache_ != nullptr)
            static_cast<void>(attachScopeSidecar(*cache_, request.key, sidecar));
        return;
    }
    if (result != nullptr && !result->measured.tapNote.empty() && fillTaps_ != nullptr)
        fillTaps_->rememberFailure(request.key, result->measured.tapNote);
    // A fill frame's tap. Its tables go to its frame (now, or when the frame is retained); a tap
    // that was refused, given back or not measured falls back to the retained picture, which the
    // frame's retention queues.
    if (sidecar != nullptr && fillTaps_ != nullptr) {
        fillTaps_->deliver(request.key, std::move(sidecar));
        return;
    }
    // A frame that is not retained yet is queued by its retention.
    if (current && cache_ != nullptr && cache_->contains(request.key))
        fillQueue_.push_front(request.key);
}

void ScopesController::takeShown(const Result& result, const Pending& pending) {
    ScopesReading reading = reading_;
    reading.shown = shown_;
    const auto& request = pending.request;
    if (auto sidecar = sidecarOf(result, request.identity)) {
        if (cache_ != nullptr)
            static_cast<void>(attachScopeSidecar(*cache_, request.key, sidecar));
        reading.state = ScopesReading::State::Ready;
        reading.message = sidecar->note;
        reading.detail = sidecar->identity.settings.detail;
        const bool rendered = sidecar->identity.source == runtime::ScopeSource::Rendered;
        const bool live = sidecar->identity.settings.detail != 0;
        reading.scopes = std::move(sidecar);
        reading.measured = result.source;
        reading.color = reading.scopes->color;
        reading.behind = false;
        // The live tables are on show: the frame, when it stays, is measured once more in full,
        // from a Rendered source only, so the tables never change source. Retained tables of a
        // frame whose device tap was refused for a reason that may pass are replaced once, when
        // the tap measures at full detail.
        if (rendered && live) {
            auto full = request;
            full.identity.settings.detail = 0;
            full.renderedOnly = true;
            want_ = std::move(full);
        } else if (!rendered && result.measured.tapRetry && !request.renderedOnly) {
            auto retry = request;
            retry.identity.settings.detail = 0;
            retry.renderedOnly = true;
            want_ = std::move(retry);
        } else {
            want_.reset();
        }
    } else if (request.renderedOnly) {
        // A Rendered upgrade of tables already on show that could not be made: they stay.
        want_.reset();
        return;
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
    // The fill's frames are measured only when no shown-frame request waits or is in flight: the
    // frames that still hold their scene picture first (their picture is held until measured), then
    // the retained ones.
    if (!want_ && !startTap())
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

bool ScopesController::startTap() {
    if (fillTaps_ == nullptr || settings_.scopes == 0)
        return false;
    auto tap = fillTaps_->takeTap(this);
    if (tap == nullptr)
        return false;
    const auto key = PreviewFrameCacheKey::forIdentity(tap->identity());
    const auto& frame = tap->frame();
    PreviewScopeSource source{frame->desiredIdentity(), frame->processIdentity(),
                              frame->provenance().provider};
    inFlight_ = submit(Request{std::move(source), key, identityFor(viewAdjust_, 0), frame, tap,
                               false, {}},
                       false);
    if (!inFlight_) {
        // Not started: the tap is answered and the frame is counted from its retained picture.
        tap->release();
        if (cache_ != nullptr && cache_->contains(key))
            fillQueue_.push_front(key);
        return false;
    }
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
        auto frame = cache_->peekScopeFrame(key);
        if (!frame.has_value() ||
            cache_->scopesFor(key, identityFor(viewAdjust_, 0, runtime::ScopeSource::Rendered)) !=
                nullptr ||
            cache_->scopesFor(key, identityFor(viewAdjust_, 0, runtime::ScopeSource::Retained)) !=
                nullptr)
            continue;
        inFlight_ = submit(
            Request{std::move(frame->source), key, identity, std::move(frame->frame), nullptr,
                    false, {}},
            false);
        if (inFlight_) {
            maxInFlight_ = std::max<std::uint32_t>(maxInFlight_, 1U);
            return true;
        }
    }
    return false;
}

} // namespace bloom::ui
