#include <bloom/ui/playback_controller.hpp>

#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/audio_device_control.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/document/project.hpp>

#include <QAction>
#include <QKeySequence>
#include <QThread>
#include <QWidget>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bloom::ui {
namespace {

// The one place that turns the current composition's duration/frame rate into a checked
// bloom::core::FrameTimeMapping, refusing (std::nullopt) exactly the cases design decision 5's
// "playback of an empty/zero-duration composition is a no-op" covers, plus any other invalid
// rate/duration FrameTimeMapping::create() itself refuses. play() and tick() both call this rather
// than re-deriving it, so they cannot silently drift from each other's notion of valid.
[[nodiscard]] std::optional<core::FrameTimeMapping>
mappingForComposition(const document::Composition& composition) noexcept {
    const auto rate = composition.format().frameRate();
    const auto result = core::FrameTimeMapping::create(composition.duration(), rate.numerator(),
                                                       rate.denominator());
    if (!result.hasValue()) {
        return std::nullopt;
    }
    return *result.value();
}

// An authored endpoint can become off-grid after a rate change. The first admitted frame must
// lie at or after that endpoint, even when nearest rounding would choose the preceding frame.
[[nodiscard]] std::optional<std::uint64_t>
firstFrameAtOrAfter(const core::FrameTimeMapping& mapping,
                    const core::RationalTime start) noexcept {
    const auto floor = mapping.frameOffsetForTime(start);
    if (!floor)
        return std::nullopt;
    const auto floorTime = mapping.timeForFrame(*floor);
    if (!floorTime)
        return std::nullopt;
    if (*floorTime.value() >= start)
        return floor;
    if (*floor == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
    return *floor + 1;
}

// Refuse invalid injected extremes BEFORE signed subtraction or duration conversion.
[[nodiscard]] std::optional<std::uint64_t>
elapsedNanoseconds(const std::chrono::steady_clock::time_point now,
                   const std::chrono::steady_clock::time_point start) noexcept {
    using Clock = std::chrono::steady_clock;
    using Rep = Clock::duration::rep;
    if (now < start)
        return std::nullopt;
    const auto current = now.time_since_epoch().count();
    const auto origin = start.time_since_epoch().count();
    if constexpr (std::is_signed_v<Rep>) {
        if (origin < 0 && current > std::numeric_limits<Rep>::max() + origin)
            return std::nullopt;
    }
    const Clock::duration elapsed{current - origin};
    if constexpr (!std::ratio_less_v<Clock::period, std::nano>) {
        if (elapsed > std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds::max()))
            return std::nullopt;
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

struct WorkAreaFrames final {
    std::uint64_t first = 0;
    std::uint64_t last = 0;
};

// The work area's valid frames, [first, last], the same derivation play() and tick() use.
[[nodiscard]] std::optional<WorkAreaFrames>
workAreaFrames(const core::FrameTimeMapping& mapping, const document::WorkArea& area) noexcept {
    const auto first = firstFrameAtOrAfter(mapping, area.start);
    const auto end = core::FrameTimeMapping::create(area.end, mapping.rateNumerator(),
                                                    mapping.rateDenominator());
    if (!first || !end)
        return std::nullopt;
    const auto last = end.value()->maximumFrameIndex();
    if (last < *first || last - *first == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
    return WorkAreaFrames{*first, last};
}

// The audio loop is the video loop's exact frame grid, [first, last + 1). The mixer's rational wrap
// and tick()'s frame-modulo wrap then share one period, so they cannot drift apart however many
// passes play, even when the authored work-area end is off the frame grid.
[[nodiscard]] std::optional<media::audio::playback::LoopRange>
gridLoopRange(const core::FrameTimeMapping& mapping, const WorkAreaFrames frames) noexcept {
    const auto start = mapping.timeForFrame(frames.first);
    const auto denominator = static_cast<std::uint64_t>(mapping.rateDenominator());
    if (!start ||
        frames.last >=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / denominator)
        return std::nullopt;
    const auto end =
        core::RationalTime::create(static_cast<std::int64_t>((frames.last + 1) * denominator),
                                   static_cast<std::int64_t>(mapping.rateNumerator()));
    if (!end)
        return std::nullopt;
    return media::audio::playback::LoopRange{*start.value(), *end};
}

} // namespace

PlaybackController::PlaybackController(CompositionSession& session,
                                       CompositionPreviewController& previewController,
                                       ClockFunction clock,
                                       const std::chrono::milliseconds tickInterval,
                                       QObject* parent)
    : QObject(parent), session_(session), previewController_(previewController),
      clock_(std::move(clock)) {
    if (!clock_)
        throw std::invalid_argument("Playback clock must be callable");
    if (tickInterval <= std::chrono::milliseconds::zero())
        throw std::invalid_argument("Playback wake interval must be positive");
    wakeCap_ = std::min(tickInterval, std::chrono::milliseconds{16});
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(static_cast<int>(wakeCap_.count()));
    connect(&timer_, &QTimer::timeout, this, &PlaybackController::tick);
    connect(&previewController_, &CompositionPreviewController::interactiveScrubStarted, this,
            &PlaybackController::handleInteractiveScrubStarted);
    // A frame entering the cache may complete the preroll: check on the next turn rather than
    // waiting for the next wake, and never from inside the cache's own insert.
    connect(
        &previewController_.frameCache(), &PreviewFrameCache::entryRetained, this,
        [this] {
            if (preroll_.holding())
                tick();
        },
        Qt::QueuedConnection);

    // Design decision 2: a composition switch or document rebind stops playback rather than
    // leaving it running against whatever composition happens to be live next.
    // CompositionSession::setComposition()/rebind() both emit compositionChanged() (verified by
    // reading composition_session.cpp), so this one connection covers both triggers.
    connect(&session_, &CompositionSession::compositionChanged, this,
            &PlaybackController::handleCompositionChanged);
    // Scrub-during-playback rule (design decision 3, reported per this task's "when done" list):
    // a scrub gesture PAUSES playback -- the simplest honest rule, chosen because playback and
    // scrub both drive CompositionSession::setCurrentTime() and TimelineRuler's own scrub gesture
    // has no way to know playback is running and coordinate with it; letting both write
    // concurrently would make the playhead fight the artist's drag every tick. currentTimeChanged()
    // fires for every setCurrentTime() call regardless of caller, so applyingOwnTimeChange_
    // distinguishes tick()'s own writes (ignored here) from every other source (scrub, direct
    // time entry, undo/redo) reaching the session while playing.
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            &PlaybackController::handleCurrentTimeChanged);
    connect(&session_, &CompositionSession::workAreaChanged, this,
            &PlaybackController::handleWorkAreaChanged);
    connect(&session_, &CompositionSession::snapshotChanged, this,
            &PlaybackController::handleSnapshotChanged);
}

PlaybackController::~PlaybackController() {
    pause();
    if (audioControl_)
        audioControl_->setCompletionHandler({});
}

void PlaybackController::installWindowShortcut(QWidget& window) {
    auto* action = new QAction(tr("Play/Pause"), &window);
    action->setObjectName(QStringLiteral("playPauseAction"));
    action->setShortcut(QKeySequence(Qt::Key_Space));
    action->setShortcutContext(Qt::WindowShortcut);
    action->setAutoRepeat(false);
    window.addAction(action);
    connect(action, &QAction::triggered, this, &PlaybackController::toggle);
}

void PlaybackController::setAudioDeviceControl(AudioDeviceControl* control) {
    Q_ASSERT(QThread::currentThread() == thread());
    pause();
    if (audioControl_)
        audioControl_->setCompletionHandler({});
    audioControl_ = control;
    if (audioControl_)
        audioControl_->setCompletionHandler(
            [this](const std::uint64_t generation, const bool playing,
                   const bool transportStarted) {
                handleAudioCompletion(generation, playing, transportStarted);
            });
    audioClockActive_ = false;
    audioStartPending_ = false;
}

void PlaybackController::useWallClock() {
    const bool wasAudioMaster = audioClockActive_;
    audioClockActive_ = false;
    // play() and work-area changes already establish the wall origin. A delayed stop/failure or
    // repeated empty mix while wall is master must not throw away elapsed time on every callback.
    if (!wasAudioMaster || state_ != PlaybackState::Playing)
        return;
    const auto* composition = session_.composition();
    const auto mapping = composition ? mappingForComposition(*composition) : std::nullopt;
    if (!mapping) {
        pause();
        return;
    }
    startClock_ = clock_();
    startFrameIndex_ = mapping->nearestFrameIndex(session_.currentTime());
    appliedOffset_ = 0;
    completedPassFrames_ = 0;
    lastAppliedFrameIndex_ = startFrameIndex_;
    scheduleTick(wakeCap_);
}

void PlaybackController::setAudioEnabled(const bool enabled) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (audioEnabled_ == enabled) {
        return;
    }
    audioEnabled_ = enabled;
    if (!audioEnabled_ && audioControl_) {
        expectedAudioGeneration_ = audioControl_->requestStop();
        audioStartPending_ = false;
        useWallClock();
    }
}

void PlaybackController::setAudioMix(runtime::AudioMixDescription description,
                                     std::vector<media::audio::playback::AudioClip> clips) {
    Q_ASSERT(QThread::currentThread() == thread());
    const bool hadClips = audioClipsAvailable_;
    audioClipsAvailable_ = !clips.empty();
    audioMix_ = std::move(description);
    if (audioControl_) {
        expectedAudioGeneration_ = audioControl_->replaceClips(std::move(clips));
        if (!audioClipsAvailable_) {
            expectedAudioGeneration_ = audioControl_->requestStop();
            audioStartPending_ = false;
            useWallClock();
        } else if (!hadClips && state_ == PlaybackState::Playing && audioEnabled_ &&
                   !preroll_.holding()) {
            // During a preroll hold the audio starts with the released clock instead.
            audioAlignmentRetried_ = false;
            audioStartPending_ = true;
            expectedAudioGeneration_ = audioControl_->requestSeek(session_.currentTime());
        }
    }
}

PlaybackState PlaybackController::state() const noexcept { return state_; }

bool PlaybackController::isLooping() const noexcept { return looping_; }

void PlaybackController::setLooping(const bool looping) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (looping_ == looping) {
        return;
    }
    if (!looping && state_ == PlaybackState::Playing) {
        // Passes already played stay played: the run finishes the pass it is in, then stops on the
        // last frame, instead of reading its whole unwrapped offset as already past the end.
        const auto* composition = session_.composition();
        const auto mapping = composition ? mappingForComposition(*composition) : std::nullopt;
        const auto frames = mapping ? workAreaFrames(*mapping, session_.workArea()) : std::nullopt;
        if (frames && startFrameIndex_ >= frames->first &&
            appliedOffset_ <=
                std::numeric_limits<std::uint64_t>::max() - (startFrameIndex_ - frames->first)) {
            const auto raw = startFrameIndex_ - frames->first + appliedOffset_;
            completedPassFrames_ = raw - raw % (frames->last - frames->first + 1);
        }
    }
    looping_ = looping;
    if (state_ == PlaybackState::Playing)
        publishAudioLoop();
    emit loopingChanged(looping_);
}

void PlaybackController::publishAudioLoop() {
    if (!audioControl_)
        return;
    std::optional<media::audio::playback::LoopRange> loop;
    if (looping_) {
        const auto* composition = session_.composition();
        const auto mapping = composition ? mappingForComposition(*composition) : std::nullopt;
        if (const auto frames =
                mapping ? workAreaFrames(*mapping, session_.workArea()) : std::nullopt)
            loop = gridLoopRange(*mapping, *frames);
    }
    expectedAudioGeneration_ = audioControl_->setLoopRange(loop);
}

void PlaybackController::play() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_ == PlaybackState::Playing) {
        return;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        return;
    }
    const auto mapping = mappingForComposition(*composition);
    if (!mapping.has_value()) {
        // Covers the zero/invalid-duration no-op (design decision 5) and any other rate/duration
        // FrameTimeMapping itself refuses.
        return;
    }

    // A later play() after a pause resumes from the CURRENT session time, never the original
    // start (design decision 2) -- read fresh here every time, not cached from a prior play().
    startClock_ = clock_();
    const auto range = session_.workArea();
    const auto firstIndex = firstFrameAtOrAfter(*mapping, range.start);
    if (!firstIndex)
        return;
    const auto first = *firstIndex;
    const auto endMapping =
        core::FrameTimeMapping::create(range.end, composition->format().frameRate().numerator(),
                                       composition->format().frameRate().denominator());
    if (!endMapping)
        return;
    const auto last = endMapping.value()->maximumFrameIndex();
    if (last < first || last - first == std::numeric_limits<std::uint64_t>::max())
        return;
    startFrameIndex_ = std::clamp(mapping->nearestFrameIndex(session_.currentTime()), first, last);
    if (session_.currentTime() < range.start || session_.currentTime() >= range.end) {
        startFrameIndex_ = first;
        if (const auto time = mapping->timeForFrame(first); time)
            (void)session_.setCurrentTime(*time.value());
    }
    appliedOffset_ = 0;
    completedPassFrames_ = 0;
    lastAppliedFrameIndex_ = startFrameIndex_;
    runRateNumerator_ = mapping->rateNumerator();
    runRateDenominator_ = mapping->rateDenominator();
    runRangeStart_ = range.start;
    runRangeEnd_ = range.end;

    audioClockActive_ = false;
    audioStartPending_ = false;
    if (!mapping->timeForFrame(startFrameIndex_))
        return;
    // Published even before clips exist, so a mix that arrives mid-run starts inside the loop.
    publishAudioLoop();

    // Preroll (#404): hold the clock until the first frames are cached. Cached frames (a RAM
    // preview) are ready at once and start without a hold.
    prerollFrames_ = prerollProvider_ ? std::max<std::size_t>(1, prerollProvider_()) : 0;
    const bool hold = prerollFrames_ != 0 && preroll_.begin(clock_(), prerollReady(*mapping));
    state_ = PlaybackState::Playing;
    if (hold) {
        // Not yet playback for the preview: a seek or scrub during the hold renders its frame as
        // it would while stopped, and nothing is counted as dropped. The read-ahead follows the
        // preparing state.
        previewController_.setPlaybackPreparing(true);
        scheduleTick(wakeCap_);
    } else {
        startRunClock(*mapping);
    }
    emit stateChanged(state_);
}

void PlaybackController::startRunClock(const core::FrameTimeMapping& mapping) {
    startClock_ = clock_();
    appliedOffset_ = 0;
    completedPassFrames_ = 0;
    lastAppliedFrameIndex_ = startFrameIndex_;
    // Audio starts with the clock, never at a Play that is still holding.
    if (audioEnabled_ && audioControl_ && audioClipsAvailable_) {
        if (const auto anchor = mapping.timeForFrame(startFrameIndex_); anchor) {
            // Match the established nearest-grid wall origin while preserving immediate session
            // time.
            audioAlignmentRetried_ = false;
            audioStartPending_ = true;
            expectedAudioGeneration_ = audioControl_->requestPlay(*anchor.value());
        }
    }
    previewController_.setPlaybackActive(true);
    previewController_.beginDroppedFrameCounting();
    timer_.start();
}

void PlaybackController::setPrerollFrameCountProvider(PrerollFrameCount provider) {
    Q_ASSERT(QThread::currentThread() == thread());
    prerollProvider_ = std::move(provider);
}

bool PlaybackController::prerollReady(const core::FrameTimeMapping& mapping) const {
    const auto frames = workAreaFrames(mapping, session_.workArea());
    if (!frames || startFrameIndex_ < frames->first || startFrameIndex_ > frames->last)
        return true; // nothing playable to wait for; the run's own checks handle the range
    const auto count = frames->last - frames->first + 1;
    const auto wanted = std::min<std::uint64_t>(prerollFrames_, count);
    const auto& cache = previewController_.frameCache();
    for (std::uint64_t index = 0; index < wanted; ++index) {
        auto offset = startFrameIndex_ - frames->first + index;
        if (offset >= count) {
            // Past the end the run wraps (looping) or stops on the last frame.
            if (!looping_)
                break;
            offset %= count;
        }
        const auto time = mapping.timeForFrame(frames->first + offset);
        if (!time)
            return false;
        const auto key = previewController_.cacheKeyForTime(*time.value());
        if (!key || !cache.contains(*key))
            return false;
    }
    return true;
}

void PlaybackController::restartPreroll() {
    const auto* composition = session_.composition();
    const auto mapping = composition ? mappingForComposition(*composition) : std::nullopt;
    const auto frames = mapping ? workAreaFrames(*mapping, session_.workArea()) : std::nullopt;
    if (!frames) {
        pause();
        return;
    }
    const auto range = session_.workArea();
    startFrameIndex_ =
        std::clamp(mapping->nearestFrameIndex(session_.currentTime()), frames->first, frames->last);
    if (session_.currentTime() < range.start || session_.currentTime() >= range.end) {
        startFrameIndex_ = frames->first;
        if (const auto time = mapping->timeForFrame(frames->first); time) {
            applyingOwnTimeChange_ = true;
            (void)session_.setCurrentTime(*time.value());
            applyingOwnTimeChange_ = false;
        }
    }
    runRateNumerator_ = mapping->rateNumerator();
    runRateDenominator_ = mapping->rateDenominator();
    runRangeStart_ = range.start;
    runRangeEnd_ = range.end;
    appliedOffset_ = 0;
    completedPassFrames_ = 0;
    lastAppliedFrameIndex_ = startFrameIndex_;
    publishAudioLoop();
    preroll_.restart(clock_());
    scheduleTick(wakeCap_);
}

void PlaybackController::handleInteractiveScrubStarted() {
    // A scrub during the hold moves where the run will start (each time change restarts the
    // hold); during a running clock it pauses, as it always has.
    if (!preroll_.holding())
        pause();
}

void PlaybackController::pause() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_ != PlaybackState::Playing) {
        return;
    }
    state_ = PlaybackState::Stopped;
    audioStartPending_ = false;
    timer_.stop();
    if (preroll_.holding()) {
        // Stop during the hold cancels it: the clock, the audio and the count never started.
        preroll_.cancel(clock_());
        previewController_.setPlaybackPreparing(false);
    }
    if (audioControl_)
        expectedAudioGeneration_ = audioControl_->requestStop();
    audioClockActive_ = false;
    previewController_.setPlaybackActive(false);
    previewController_.endDroppedFrameCounting();
    emit stateChanged(state_);
}

void PlaybackController::toggle() {
    if (state_ == PlaybackState::Playing) {
        pause();
    } else {
        play();
    }
}

void PlaybackController::tick() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_ != PlaybackState::Playing) {
        return;
    }
    const auto* composition = session_.composition();
    if (composition == nullptr) {
        pause();
        return;
    }
    const auto mapping = mappingForComposition(*composition);
    if (!mapping.has_value()) {
        pause();
        return;
    }

    if (mapping->rateNumerator() != runRateNumerator_ ||
        mapping->rateDenominator() != runRateDenominator_) {
        handleWorkAreaChanged();
        return;
    }
    if (preroll_.holding()) {
        const auto now = clock_();
        if (!preroll_.poll(now, prerollReady(*mapping))) {
            // Wake no later than the maximum hold; cache insertions also wake the check.
            scheduleTick(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::max(preroll_.remaining(now), PlaybackPreroll::Clock::duration{1})));
            return;
        }
        // Playback goes active before preparing clears, so the read-ahead sees one continuous
        // run and keeps the frames it has in flight.
        startRunClock(*mapping);
        previewController_.setPlaybackPreparing(false);
        scheduleTick(wakeCap_);
        return;
    }
    // Every early refusal still rearms a bounded positive wake; no timer spin or missed restart.
    scheduleTick(wakeCap_);
    std::optional<std::uint64_t> frameOffset;
    std::optional<std::uint64_t> untilNextFrame;
    if (audioClockActive_ && audioControl_) {
        const auto position = audioControl_->positionNow();
        const auto audioFrame = mapping->frameOffsetForTime(position);
        if (!audioFrame || *audioFrame < startFrameIndex_) {
            return;
        }
        frameOffset = *audioFrame - startFrameIndex_;
        untilNextFrame =
            mapping->nanosecondsUntilNextFrame(position, audioControl_->playbackRateRatio());
    } else {
        const auto now = clock_();
        const auto elapsed = elapsedNanoseconds(now, startClock_);
        if (!elapsed)
            return;
        const auto elapsedCount = *elapsed;

        // Real-time, drop-frames-never-slow policy: the target frame is derived from TOTAL elapsed
        // time since play()'s fixed start every tick (never `lastFrame + 1`, never an accumulated
        // running clock), so a slow tick jumps straight to the frame elapsed time now demands
        // instead of catching up frame-by-frame.
        frameOffset = mapping->frameOffsetForElapsedNanoseconds(elapsedCount);
        const auto elapsedTime =
            core::RationalTime::create(static_cast<std::int64_t>(elapsedCount), 1'000'000'000);
        if (elapsedTime)
            untilNextFrame = mapping->nanosecondsUntilNextFrame(*elapsedTime);
    }
    if (!frameOffset || !untilNextFrame ||
        *untilNextFrame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        // Checked-arithmetic overflow guard (unreachable for any realistic elapsed duration and
        // frame rate) -- no-op rather than wrap.
        return;
    }

    const auto range = session_.workArea();
    const auto firstIndex = firstFrameAtOrAfter(*mapping, range.start);
    if (!firstIndex) {
        pause();
        return;
    }
    const auto first = *firstIndex;
    const auto rate = composition->format().frameRate();
    const auto endMapping =
        core::FrameTimeMapping::create(range.end, rate.numerator(), rate.denominator());
    if (!endMapping)
        return;
    const auto last = endMapping.value()->maximumFrameIndex();
    if (last < first || last - first == std::numeric_limits<std::uint64_t>::max() ||
        startFrameIndex_ < first || startFrameIndex_ > last) {
        handleWorkAreaChanged();
        return;
    }
    const auto frameCount = last - first + 1;
    const auto frameBudget = std::chrono::nanoseconds{static_cast<std::int64_t>(*untilNextFrame)};
    scheduleTick(frameBudget);
    if (*frameOffset <= appliedOffset_) {
        // The frame this run is already showing is still the one elapsed time asks for.
        return;
    }
    const auto nextOffset = *frameOffset;
    if (nextOffset > std::numeric_limits<std::uint64_t>::max() - startFrameIndex_) {
        // Same checked-overflow discipline applied to the addition below.
        return;
    }
    // Looping (design decision 2): wraps within [0, duration) via exact modulo of the frame count.
    // The offset is exact in either master clock, so repeated wraps
    // never drift.
    //
    // Task VIEW-1 made that wrap conditional rather than unconditional. With looping off, the run
    // lands on the LAST frame of the work area exactly once and pauses there: it neither wraps nor
    // freezes still-playing on a frame it already showed, and the offset arithmetic below is the
    // same exact integer arithmetic either way -- only the reduction differs.
    const auto runOffset = startFrameIndex_ - first + nextOffset;
    const auto rawOffset = runOffset >= completedPassFrames_ ? runOffset - completedPassFrames_ : 0;
    const bool pastEnd = !looping_ && rawOffset >= frameCount;
    const auto targetFrameIndex =
        looping_ ? first + rawOffset % frameCount : first + std::min(rawOffset, frameCount - 1);
    previewController_.noteDroppedFrames(nextOffset - appliedOffset_ - 1);
    appliedOffset_ = nextOffset;

    const auto targetTime = mapping->timeForFrame(targetFrameIndex);
    if (!targetTime.hasValue()) {
        return;
    }

    lastAppliedFrameIndex_ = targetFrameIndex;
    applyingOwnTimeChange_ = true;
    (void)session_.setCurrentTime(*targetTime.value());
    applyingOwnTimeChange_ = false;
    previewController_.presentPlaybackFrame(frameBudget);
    if (pastEnd) {
        // After the frame is applied and presented, so the artist is left looking at the last
        // frame of the range rather than at whatever the run showed one tick earlier.
        pause();
    }
}

void PlaybackController::scheduleTick(const std::chrono::nanoseconds untilNextFrame) {
    if (state_ != PlaybackState::Playing)
        return;
    const auto positive = std::max(untilNextFrame, std::chrono::nanoseconds{1});
    const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(positive);
    timer_.start(static_cast<int>(std::min(milliseconds, wakeCap_).count()));
}

void PlaybackController::handleWorkAreaChanged() {
    if (state_ != PlaybackState::Playing)
        return;
    if (preroll_.holding()) {
        restartPreroll();
        return;
    }
    const auto* composition = session_.composition();
    const auto mapping = composition ? mappingForComposition(*composition) : std::nullopt;
    if (!mapping) {
        pause();
        return;
    }
    const auto area = session_.workArea();
    const auto firstIndex = firstFrameAtOrAfter(*mapping, area.start);
    if (!firstIndex) {
        pause();
        return;
    }
    const auto first = *firstIndex;
    const auto end = core::FrameTimeMapping::create(area.end, mapping->rateNumerator(),
                                                    mapping->rateDenominator());
    if (!end || end.value()->maximumFrameIndex() < first ||
        end.value()->maximumFrameIndex() - first == std::numeric_limits<std::uint64_t>::max()) {
        pause();
        return;
    }
    const auto index =
        std::clamp(mapping->nearestFrameIndex(session_.currentTime() < area.start ||
                                                      session_.currentTime() >= area.end
                                                  ? area.start
                                                  : session_.currentTime()),
                   first, end.value()->maximumFrameIndex());
    const auto anchor = mapping->timeForFrame(index);
    if (!anchor) {
        pause();
        return;
    }
    if (runRateNumerator_ == mapping->rateNumerator() &&
        runRateDenominator_ == mapping->rateDenominator() && runRangeStart_ == area.start &&
        runRangeEnd_ == area.end)
        return;
    runRateNumerator_ = mapping->rateNumerator();
    runRateDenominator_ = mapping->rateDenominator();
    runRangeStart_ = area.start;
    runRangeEnd_ = area.end;
    startClock_ = clock_();
    startFrameIndex_ = index;
    appliedOffset_ = 0;
    completedPassFrames_ = 0;
    lastAppliedFrameIndex_ = index;
    const bool moved = session_.currentTime() != *anchor.value();
    applyingOwnTimeChange_ = true;
    (void)session_.setCurrentTime(*anchor.value());
    applyingOwnTimeChange_ = false;
    // The loop goes ahead of the seek so the restarted mix wraps at the new range. The seek itself
    // is needed: this relocates the run (new anchor, new grid), it is not a wrap.
    publishAudioLoop();
    if (audioEnabled_ && audioControl_ && audioClipsAvailable_) {
        useWallClock();
        audioAlignmentRetried_ = false;
        audioStartPending_ = true;
        expectedAudioGeneration_ = audioControl_->requestSeek(*anchor.value());
    }
    const auto period =
        audioEnabled_ && audioControl_ && audioClipsAvailable_
            ? mapping->nanosecondsUntilNextFrame(*anchor.value(), audioControl_->playbackRateRatio())
            : mapping->nanosecondsUntilNextFrame(core::RationalTime{});
    if (!period || *period > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        pause();
        return;
    }
    const auto budget = std::chrono::nanoseconds{static_cast<std::int64_t>(*period)};
    scheduleTick(budget);
    if (moved)
        previewController_.presentPlaybackFrame(budget);
}

void PlaybackController::handleSnapshotChanged() {
    const auto* composition = session_.composition();
    if (state_ == PlaybackState::Playing && composition &&
        (composition->format().frameRate().numerator() != runRateNumerator_ ||
         composition->format().frameRate().denominator() != runRateDenominator_))
        handleWorkAreaChanged();
}

void PlaybackController::handleCompositionChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    pause();
    audioMix_.reset();
    audioClipsAvailable_ = false;
    if (audioControl_)
        (void)audioControl_->replaceClips({});
}

void PlaybackController::handleCurrentTimeChanged() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_ == PlaybackState::Playing && !applyingOwnTimeChange_) {
        // A seek during the preroll hold restarts it at the new position.
        if (preroll_.holding())
            restartPreroll();
        else
            pause();
    }
}

void PlaybackController::handleAudioCompletion(const std::uint64_t generation, const bool playing,
                                               const bool transportStarted) {
    if (generation != expectedAudioGeneration_ || state_ != PlaybackState::Playing ||
        !audioEnabled_ || !audioControl_)
        return;
    if (!playing) {
        audioStartPending_ = false;
        useWallClock();
        return;
    }
    // A mix-only update during a live run must not compare the unwrapped device clock with the
    // loop-wrapped session time. It is only a start handoff when an earlier start was still pending.
    if (!transportStarted && !audioStartPending_)
        return;
    if (!audioStartPending_)
        return;
    const auto* composition = session_.composition();
    const auto mapping = composition ? mappingForComposition(*composition) : std::nullopt;
    if (!mapping)
        return;
    const auto deviceFrame = mapping->frameOffsetForTime(audioControl_->positionNow());
    const auto currentFrame = mapping->nearestFrameIndex(session_.currentTime());
    if (!deviceFrame || *deviceFrame < currentFrame ||
        (currentFrame != std::numeric_limits<std::uint64_t>::max() &&
         *deviceFrame > currentFrame + 1)) {
        // A delayed device start must not pull the already advancing visual clock backward.
        useWallClock();
        if (!audioAlignmentRetried_) {
            audioAlignmentRetried_ = true;
            audioStartPending_ = true;
            expectedAudioGeneration_ = audioControl_->requestSeek(session_.currentTime());
        } else {
            audioStartPending_ = false;
            expectedAudioGeneration_ = audioControl_->requestStop();
        }
        return;
    }
    audioClockActive_ = true;
    audioStartPending_ = false;
    startFrameIndex_ = currentFrame;
    appliedOffset_ = 0;
    completedPassFrames_ = 0;
    lastAppliedFrameIndex_ = currentFrame;
    scheduleTick(wakeCap_);
}

} // namespace bloom::ui
