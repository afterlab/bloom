// Playback preroll (#404): Play holds the transport clock until the first frames are cached, for at
// most half a second. Every test drives time with a manual clock: no wall-clock sleeps decide an
// outcome. Event-loop waits only let real asynchronous work (CPU frames, the audio worker) land.
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/media/audio/playback/audio_engine.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/audio_device_control.hpp>
#include <bloom/ui/background_preview_controller.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/playback_preroll.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <source_location>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using Preroll = bloom::ui::PlaybackPreroll;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// Returns as soon as the predicate holds; the bound only stops a broken test from hanging.
template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 4'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (std::invoke(predicate))
            return true;
        std::this_thread::yield();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return std::invoke(predicate);
}

[[nodiscard]] bloom::core::RationalTime time(const std::int64_t numerator,
                                             const std::int64_t denominator = 1) {
    const auto value = bloom::core::RationalTime::create(numerator, denominator);
    if (!value.has_value())
        std::abort();
    return *value;
}

// 25 fps: one frame is exactly 40 ms, so manual-clock steps land exactly on frame boundaries.
bloom::document::NewProject makeProject(std::string name) {
    const auto rate = bloom::document::FrameRate::create(25, 1);
    const auto format = rate ? bloom::document::CompositionFormat::create(
                                   4, 3, bloom::core::PixelAspectRatio::square(), *rate)
                             : std::nullopt;
    if (!format)
        std::abort();
    return bloom::document::makeNewProject(std::move(name), "Main", time(4), *format);
}

struct ManualClock final {
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    void advance(const std::chrono::nanoseconds delta) { now += delta; }
};

struct PipelineFixture final {
    bloom::runtime::NodeDefinitionRegistry definitions;
    bloom::runtime::SnapshotCompiler compiler;
    bloom::runtime::CpuCompositionEvaluator evaluator;
    bloom::runtime::CpuReferenceDisplayPreparer displayPreparer;
    bloom::runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    bloom::ui::PreviewPreparationFunction pipeline;

    PipelineFixture() : compiler(definitions) {
        if (!bloom::runtime::registerBuiltInNodeDefinitions(definitions))
            std::abort();
        definitions.freeze();
        pipeline = bloom::ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                             qualifiedProcessorProvider);
    }
};

// The real document, session, scheduler, bridge and preview controller; never a mock of either.
struct Fixture final {
    bloom::document::NewProject seed;
    bloom::document::Document document;
    bloom::commands::CommandStack commands;
    bloom::ui::CompositionSession session;
    bloom::runtime::TaskScheduler scheduler;
    bloom::ui::TaskUiBridge bridge;
    PipelineFixture pipeline;
    bloom::ui::CompositionPreviewController controller;
    ManualClock clock;
    bloom::ui::PlaybackController playback;

    explicit Fixture(std::string name)
        : seed(makeProject(std::move(name))), document(std::move(seed.project)), commands(document),
          session(document, commands, seed.initialCompositionId),
          scheduler(bloom::runtime::TaskSchedulerConfig{.cpuWorkerCount = 1,
                                                        .blockingIoWorkerCount = 1,
                                                        .cpuQueueCapacity = 16,
                                                        .blockingIoQueueCapacity = 4,
                                                        .terminalHistoryCapacity = 32,
                                                        .diagnosticsPerTask = 8,
                                                        .groupRegistryCapacity = 8}),
          bridge(scheduler, nullptr, 1ms),
          controller(session, scheduler, bridge, pipeline.pipeline),
          playback(session, controller, [this] { return clock.now; }, 16ms) {}

    [[nodiscard]] bool cached(const std::int64_t frame) const {
        const auto key = controller.cacheKeyForTime(time(frame, 25));
        return key && controller.frameCache().contains(*key);
    }
    // Renders and caches one frame the way a stopped Viewer does (a Visible request).
    bool cacheFrame(const std::int64_t frame) {
        if (session.currentTime() == time(frame, 25))
            controller.requestRefresh();
        else
            (void)session.setCurrentTime(time(frame, 25));
        return waitUntil([&] { return cached(frame); });
    }

    void finish(Expectations& expectations) {
        playback.pause();
        controller.beginShutdown();
        bridge.beginShutdown();
        expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                            "the preroll fixture reaches scheduler quiescence");
    }
};

// The state machine alone, with explicit time points.
void testStateMachine(Expectations& expectations) {
    const auto t0 = Preroll::Clock::time_point{} + 1h;
    Preroll preroll;
    expectations.expect(!preroll.begin(t0, true), "ready frames do not hold");
    expectations.expect(preroll.last().outcome == Preroll::Outcome::Immediate,
                        "a start with ready frames is Immediate");

    expectations.expect(preroll.begin(t0, false), "missing frames hold");
    expectations.expect(!preroll.poll(t0 + 499ms, false), "under the maximum the hold continues");
    expectations.expect(preroll.remaining(t0 + 499ms) == 1ms, "one millisecond remains");
    const auto ready = preroll.poll(t0 + 120ms, true);
    expectations.expect(ready && ready->outcome == Preroll::Outcome::Ready && ready->held == 120ms,
                        "ready frames release the hold and report how long it held");
    expectations.expect(!preroll.holding(), "a released hold no longer holds");

    expectations.expect(preroll.begin(t0, false), "hold again");
    const auto capped = preroll.poll(t0 + 500ms, false);
    expectations.expect(capped && capped->outcome == Preroll::Outcome::CapReached &&
                            capped->held == 500ms,
                        "the hold releases exactly at the maximum of 0.5 s");

    expectations.expect(preroll.begin(t0, false), "hold again");
    preroll.restart(t0 + 400ms);
    expectations.expect(!preroll.poll(t0 + 800ms, false),
                        "a restart measures the maximum from the new position");
    const auto restarted = preroll.poll(t0 + 900ms, false);
    expectations.expect(restarted && restarted->outcome == Preroll::Outcome::CapReached &&
                            restarted->held == 900ms && restarted->restarts == 1,
                        "a restarted hold reports its full hold and one restart");

    expectations.expect(preroll.begin(t0, false), "hold again");
    preroll.cancel(t0 + 10ms);
    expectations.expect(!preroll.holding() && preroll.last().outcome == Preroll::Outcome::Cancelled,
                        "cancel ends the hold as Cancelled");
    expectations.expect(!preroll.poll(t0 + 1s, false), "a cancelled hold never releases");
}

// Hold until ready: the read-ahead caches the frames from the playhead while the clock waits, and
// the hold ends on the cache insertion with no clock time spent.
void testHoldsUntilReady(Expectations& expectations) {
    Fixture fixture("Hold until ready");
    bloom::ui::BackgroundPreviewController background(fixture.session, fixture.controller,
                                                      fixture.scheduler, fixture.bridge,
                                                      fixture.pipeline.pipeline);
    background.setReadAheadWidthProvider([](std::uint64_t) { return std::size_t{3}; });
    expectations.expect(background.prerollFrameCount() == 3,
                        "the preroll is the read-ahead window");
    fixture.playback.setPrerollFrameCountProvider([&] { return background.prerollFrameCount(); });
    bloom::ui::WindowStatusBar status(fixture.session, &fixture.controller);
    status.resize(1920, 40); // wide enough that the activity text is not elided
    status.show();
    expectations.expect(fixture.cacheFrame(0), "the stopped frame is cached");
    expectations.expect(!fixture.cached(1) && !fixture.cached(2), "the next frames are not");

    fixture.playback.play();
    expectations.expect(fixture.playback.state() == bloom::ui::PlaybackState::Playing,
                        "Play enters Playing during the hold");
    expectations.expect(fixture.playback.isPreparing() && fixture.controller.playbackPreparing(),
                        "the hold is reported as preparing");
    expectations.expect(status.activityTextForTest().contains(QStringLiteral("Preparing playback")),
                        "the activity surface says playback is preparing");
    expectations.expect(!fixture.controller.playbackActive() &&
                            !fixture.controller.isCountingDroppedFrames(),
                        "the hold is not playback yet: no deadline and no dropped-frame count");
    fixture.clock.advance(200ms);
    fixture.playback.tick();
    expectations.expect(fixture.playback.isPreparing() && fixture.session.currentTime() == time(0),
                        "the clock holds on the start frame while frames are missing");

    // Count read-ahead work the release cancels: none, the run is continuous.
    std::size_t cancelledAtRelease = 0;
    QObject::connect(&fixture.controller,
                     &bloom::ui::CompositionPreviewController::playbackPreparingChanged,
                     [&](const bool preparing) {
                         if (!preparing && !background.active())
                             ++cancelledAtRelease;
                     });
    expectations.expect(waitUntil([&] { return !fixture.playback.isPreparing(); }),
                        "the read-ahead's frames release the hold");
    expectations.expect(cancelledAtRelease == 0 && background.active(),
                        "the release keeps the read-ahead running, with its frames in flight");
    expectations.expect(fixture.cached(1) && fixture.cached(2), "the window is cached");
    const auto& last = fixture.playback.lastPreroll();
    expectations.expect(last.outcome == Preroll::Outcome::Ready && last.held == 200ms,
                        "the hold ended Ready after the 200 ms of clock it spent");
    expectations.expect(fixture.controller.playbackActive() &&
                            fixture.controller.isCountingDroppedFrames() &&
                            fixture.controller.droppedFrameCount() == 0,
                        "the release starts playback and its dropped-frame count at zero");
    expectations.expect(!status.activityTextForTest().contains(QStringLiteral("Preparing")),
                        "the activity surface stops saying preparing");
    expectations.expect(fixture.session.currentTime() == time(0),
                        "the clock starts at the start frame, not 200 ms into the run");
    fixture.clock.advance(40ms);
    fixture.playback.tick();
    expectations.expect(fixture.session.currentTime() == time(1, 25),
                        "one frame after the release shows frame one, in sync");
    expectations.expect(fixture.controller.droppedFrameCount() == 0, "nothing was dropped");
    fixture.finish(expectations);
}

// Release at the cap: frames that never arrive hold the clock for exactly 0.5 s.
void testReleasesAtCap(Expectations& expectations) {
    Fixture fixture("Cap");
    fixture.playback.setPrerollFrameCountProvider([] { return std::size_t{4}; });
    fixture.playback.play();
    expectations.expect(fixture.playback.isPreparing(), "uncached frames hold");
    fixture.clock.advance(499ms);
    fixture.playback.tick();
    expectations.expect(fixture.playback.isPreparing(), "0.499 s still holds");
    fixture.clock.advance(1ms);
    fixture.playback.tick();
    expectations.expect(!fixture.playback.isPreparing() &&
                            fixture.playback.lastPreroll().outcome ==
                                Preroll::Outcome::CapReached &&
                            fixture.playback.lastPreroll().held == 500ms,
                        "0.5 s releases the clock without the frames");
    expectations.expect(fixture.session.currentTime() == time(0),
                        "the released clock starts at the start frame");
    fixture.clock.advance(120ms);
    fixture.playback.tick();
    expectations.expect(fixture.session.currentTime() == time(3, 25),
                        "after the cap the run keeps real time from the release");
    expectations.expect(fixture.controller.droppedFrameCount() == 2,
                        "after the cap the ordinary dropped-frame behavior applies");
    fixture.finish(expectations);
}

// Stop or Pause during the hold cancels it cleanly.
void testStopCancelsHold(Expectations& expectations) {
    Fixture fixture("Cancel");
    fixture.playback.setPrerollFrameCountProvider([] { return std::size_t{2}; });
    for (const bool useToggle : {false, true}) {
        fixture.playback.play();
        expectations.expect(fixture.playback.isPreparing(), "Play holds");
        fixture.clock.advance(100ms);
        if (useToggle)
            fixture.playback.toggle();
        else
            fixture.playback.pause();
        expectations.expect(fixture.playback.state() == bloom::ui::PlaybackState::Stopped,
                            "Stop during the hold stops");
        expectations.expect(
            !fixture.playback.isPreparing() && !fixture.controller.playbackPreparing() &&
                fixture.playback.lastPreroll().outcome == Preroll::Outcome::Cancelled,
            "Stop during the hold cancels it");
        expectations.expect(!fixture.controller.playbackActive() &&
                                !fixture.controller.isCountingDroppedFrames(),
                            "a cancelled hold never started playback or counting");
        fixture.clock.advance(1s);
        fixture.playback.tick();
        expectations.expect(fixture.session.currentTime() == time(0) &&
                                fixture.playback.state() == bloom::ui::PlaybackState::Stopped,
                            "a later tick neither releases nor moves time");
    }
    fixture.finish(expectations);
}

// A seek or a scrub during the hold restarts it at the new position.
void testSeekRestartsHold(Expectations& expectations) {
    Fixture fixture("Seek");
    fixture.playback.setPrerollFrameCountProvider([] { return std::size_t{2}; });
    fixture.playback.play();
    fixture.clock.advance(300ms);
    fixture.playback.tick();
    (void)fixture.session.setCurrentTime(time(10, 25));
    expectations.expect(fixture.playback.state() == bloom::ui::PlaybackState::Playing &&
                            fixture.playback.isPreparing(),
                        "a seek during the hold keeps the run and keeps holding");
    fixture.controller.beginInteractiveScrub();
    expectations.expect(fixture.playback.state() == bloom::ui::PlaybackState::Playing,
                        "a scrub gesture during the hold does not stop the run");
    fixture.controller.notifyScrubEnded();
    fixture.clock.advance(300ms);
    fixture.playback.tick();
    expectations.expect(fixture.playback.isPreparing(),
                        "the maximum is measured from the seek, not from Play");
    fixture.clock.advance(200ms);
    fixture.playback.tick();
    expectations.expect(!fixture.playback.isPreparing() &&
                            fixture.playback.lastPreroll().restarts == 1 &&
                            fixture.playback.lastPreroll().held == 800ms,
                        "the restarted hold releases at its own maximum");
    fixture.clock.advance(40ms);
    fixture.playback.tick();
    expectations.expect(fixture.session.currentTime() == time(11, 25),
                        "the run starts at the seek position");
    (void)fixture.session.setCurrentTime(time(20, 25));
    expectations.expect(fixture.playback.state() == bloom::ui::PlaybackState::Stopped,
                        "after the release a seek pauses, as it always has");
    fixture.finish(expectations);
}

// Cached frames (a RAM preview) start at once, with no hold.
void testCachedFramesStartImmediately(Expectations& expectations) {
    Fixture fixture("Cached");
    fixture.playback.setPrerollFrameCountProvider([] { return std::size_t{3}; });
    for (const auto frame : {0, 1, 2})
        expectations.expect(fixture.cacheFrame(frame), "frame cached");
    (void)fixture.session.setCurrentTime(time(0));
    fixture.playback.play();
    expectations.expect(!fixture.playback.isPreparing() &&
                            fixture.playback.lastPreroll().outcome == Preroll::Outcome::Immediate,
                        "cached frames start without a hold");
    expectations.expect(fixture.controller.playbackActive() &&
                            fixture.controller.isCountingDroppedFrames(),
                        "playback is live at Play");
    fixture.clock.advance(40ms);
    fixture.playback.tick();
    expectations.expect(fixture.session.currentTime() == time(1, 25), "the clock started at Play");
    fixture.playback.pause();

    // The last frame of a non-looping run needs nothing past the end of the work area.
    fixture.playback.setLooping(false);
    expectations.expect(fixture.cacheFrame(99), "the last frame is cached");
    fixture.playback.play();
    expectations.expect(!fixture.playback.isPreparing(),
                        "a non-looping run on its last frame waits for nothing past the end");
    fixture.finish(expectations);
}

// Audio starts with the released clock, never at a Play that is still holding.
void testAudioStartsAtRelease(Expectations& expectations) {
    namespace audio = bloom::media::audio::playback;
    Fixture fixture("Audio");
    auto engine = std::make_unique<audio::AudioEngine>(std::make_unique<audio::NullBackend>(),
                                                       audio::AudioEngine::Config{1000, 2, 4096});
    bloom::ui::AudioDeviceControl control(fixture.scheduler, fixture.bridge, std::move(engine));
    fixture.playback.setAudioDeviceControl(&control);
    fixture.playback.setPrerollFrameCountProvider([] { return std::size_t{2}; });
    bloom::runtime::AudioMixDescription mix;
    mix.clips.push_back({bloom::document::NodeId::fromRaw(1), bloom::document::AssetId::fromRaw(1),
                         time(0), time(10)});
    fixture.playback.setAudioMix(mix, {audio::AudioClip{nullptr, time(0)}});
    expectations.expect(waitUntil([&] { return control.isIdle(); }), "the mix is applied");

    fixture.playback.play();
    expectations.expect(fixture.playback.isPreparing(), "Play holds");
    expectations.expect(waitUntil([&] { return control.isIdle(); }),
                        "the audio worker drains the requests Play made");
    expectations.expect(!control.isPlaying() && !fixture.playback.audioClockActive(),
                        "audio does not start while the clock holds");
    // A mix that arrives during the hold does not start audio either.
    fixture.playback.setAudioMix({}, {});
    fixture.playback.setAudioMix(mix, {audio::AudioClip{nullptr, time(0)}});
    expectations.expect(waitUntil([&] { return control.isIdle(); }) && !control.isPlaying(),
                        "a mix during the hold waits for the release");

    fixture.clock.advance(500ms);
    fixture.playback.tick();
    expectations.expect(!fixture.playback.isPreparing(), "the cap releases the clock");
    expectations.expect(waitUntil([&] { return fixture.playback.audioClockActive(); }),
                        "audio starts with the released clock and becomes the master");
    expectations.expect(control.positionNow() == time(0),
                        "audio starts at the run's anchor, not 0.5 s into it");

    // Stop during a later hold leaves the audio stopped.
    fixture.playback.pause();
    expectations.expect(waitUntil([&] { return control.isIdle(); }) && !control.isPlaying(),
                        "pause stops audio");
    (void)fixture.session.setCurrentTime(time(40, 25));
    fixture.playback.play();
    expectations.expect(fixture.playback.isPreparing(), "the second Play holds");
    fixture.playback.pause();
    expectations.expect(waitUntil([&] { return control.isIdle(); }) && !control.isPlaying(),
                        "Stop during the hold never started audio");
    fixture.playback.setAudioDeviceControl(nullptr);
    fixture.finish(expectations);
    control.beginShutdown();
    control.drainAfterEventLoop();
}

// No provider: Play starts the clock at once, exactly as before preroll existed.
void testNoProviderNoHold(Expectations& expectations) {
    Fixture fixture("No provider");
    fixture.playback.play();
    expectations.expect(!fixture.playback.isPreparing() && fixture.controller.playbackActive(),
                        "without a provider Play starts the clock at once");
    fixture.clock.advance(40ms);
    fixture.playback.tick();
    expectations.expect(fixture.session.currentTime() == time(1, 25), "the clock runs from Play");
    fixture.finish(expectations);
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("BloomPrerollTests"));
    app.setApplicationName(QStringLiteral("PlaybackPreroll"));
    bloom::ui::kit::installKinetikTheme(app);
    Expectations expectations;
    testStateMachine(expectations);
    testHoldsUntilReady(expectations);
    testReleasesAtCap(expectations);
    testStopCancelsHold(expectations);
    testSeekRestartsHold(expectations);
    testCachedFramesStartImmediately(expectations);
    testAudioStartsAtRelease(expectations);
    testNoProviderNoHold(expectations);
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " playback preroll expectation(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "playback preroll tests passed\n";
    return EXIT_SUCCESS;
}
