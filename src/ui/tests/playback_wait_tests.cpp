// Waiting for frames (#497): with a frame availability provider the playhead never moves past a
// frame that is not ready. It holds on the frame it shows (still Playing) until the next one is
// ready, then plays on; cached frames keep real time. Every test drives time with a manual clock
// and a scripted availability, so no wall-clock sleep decides an outcome. Event-loop waits only let
// real asynchronous work (CPU frames, the audio worker) land.
#include <bloom/commands/command_stack.hpp>
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
#include <set>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Availability = bloom::ui::PlaybackFrameAvailability;

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

[[nodiscard]] bloom::core::RationalTime frame(const std::int64_t index) { return time(index, 25); }

// 25 fps, 100 frames: one frame is exactly 40 ms, so manual-clock steps land on frame boundaries.
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
// `ready` and `unavailable` script the availability the transport sees, by frame index, and a
// frame really in the preview frame cache is ready too; every other frame is Pending.
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
    std::set<std::int64_t> ready;
    std::set<std::int64_t> unavailable;

    explicit Fixture(std::string name, const bool scripted = true)
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
          playback(session, controller, [this] { return clock.now; }, 16ms) {
        if (scripted)
            playback.setFrameAvailabilityProvider([this](const bloom::core::RationalTime at) {
                const auto index =
                    static_cast<std::int64_t>(at.numerator() * 25 / at.denominator());
                if (ready.contains(index) || cached(index))
                    return Availability::Ready;
                if (unavailable.contains(index))
                    return Availability::Unavailable;
                return Availability::Pending;
            });
    }

    [[nodiscard]] bool at(const std::int64_t index) const {
        return session.currentTime() == frame(index);
    }
    [[nodiscard]] bool cached(const std::int64_t index) const {
        const auto key = controller.cacheKeyForTime(frame(index));
        return key && controller.frameCache().contains(*key);
    }
    // Renders and caches one frame the way a stopped Viewer does (a Visible request).
    bool cacheFrame(const std::int64_t index) {
        if (session.currentTime() == frame(index))
            controller.requestRefresh();
        else
            (void)session.setCurrentTime(frame(index));
        return waitUntil([&] { return cached(index); });
    }
    void step(const std::chrono::nanoseconds delta) {
        clock.advance(delta);
        playback.tick();
    }

    void finish(Expectations& expectations) {
        playback.pause();
        controller.beginShutdown();
        bridge.beginShutdown();
        expectations.expect(waitUntil([&] { return scheduler.isQuiescent(); }),
                            "the wait fixture reaches scheduler quiescence");
    }
};

// The playhead holds on an uncached frame without stopping, then plays on as soon as it is ready,
// with the clock restarted from that frame.
void testHoldsOnUncachedFrame(Expectations& expectations) {
    Fixture fixture("Hold");
    bloom::ui::WindowStatusBar status(fixture.session, &fixture.controller);
    status.resize(1920, 40); // wide enough that the activity text is not elided
    status.show();
    fixture.playback.play();
    expectations.expect(fixture.playback.state() == bloom::ui::PlaybackState::Playing &&
                            fixture.controller.playbackActive(),
                        "Play starts playback");
    fixture.step(40ms);
    expectations.expect(fixture.at(0), "the playhead holds on frame 0 while frame 1 is pending");
    expectations.expect(fixture.playback.state() == bloom::ui::PlaybackState::Playing,
                        "holding is not stopping: the run stays Playing");
    expectations.expect(fixture.playback.isHoldingForFrame() &&
                            fixture.playback.isWaitingForFrames() &&
                            fixture.controller.playbackWaiting(),
                        "the hold is reported as waiting for frames");
    expectations.expect(
        status.activityTextForTest().contains(QStringLiteral("Playback waiting for frames")),
        "the activity surface says playback is waiting for frames");
    fixture.step(2s);
    expectations.expect(fixture.at(0) && fixture.controller.droppedFrameCount() == 0,
                        "a long wait neither advances nor counts dropped frames");

    fixture.ready.insert(1);
    fixture.playback.tick();
    expectations.expect(fixture.at(1), "frame 1 shows as soon as it is ready, with no clock time");
    expectations.expect(!fixture.playback.isHoldingForFrame() &&
                            !fixture.playback.isWaitingForFrames() &&
                            !fixture.controller.playbackWaiting(),
                        "the hold ends when the frame arrives");
    expectations.expect(!status.activityTextForTest().contains(QStringLiteral("waiting")),
                        "the activity surface stops saying waiting");
    fixture.ready.insert(2);
    fixture.step(39ms);
    expectations.expect(fixture.at(1),
                        "the clock restarted from frame 1: the 2 s wait is not caught up");
    fixture.step(1ms);
    expectations.expect(fixture.at(2), "one frame period after frame 1, frame 2 shows");
    fixture.finish(expectations);
}

// Cached frames play at the composition's real-time rate, as without a provider: a late wake
// skips straight to the frame elapsed time asks for.
void testCachedFramesPlayRealTime(Expectations& expectations) {
    Fixture fixture("Real time");
    for (std::int64_t index = 16; index >= 0; --index)
        expectations.expect(fixture.cacheFrame(index), "frame cached");
    fixture.playback.play();
    for (std::int64_t index = 1; index <= 10; ++index) {
        fixture.step(40ms);
        expectations.expect(fixture.at(index), "each 40 ms shows the next cached frame");
    }
    fixture.step(200ms);
    expectations.expect(fixture.at(15), "a late wake jumps to the frame elapsed time asks for");
    expectations.expect(fixture.controller.droppedFrameCount() == 4 &&
                            !fixture.playback.isWaitingForFrames(),
                        "cached playback drops late frames and never waits");
    fixture.finish(expectations);
}

// The playhead advances through ready frames in order and never skips a pending one, so the shown
// frames, and the cache the read-ahead fills behind them, are contiguous from the playhead.
void testNeverSkipsPendingFrame(Expectations& expectations) {
    Fixture fixture("Order");
    fixture.ready = {3, 5};
    fixture.playback.play();
    fixture.step(200ms);
    expectations.expect(fixture.at(0), "ready frames past a pending one do not pull the playhead");
    fixture.ready.insert(1);
    fixture.playback.tick();
    expectations.expect(fixture.at(1), "frame 1 shows when ready");
    fixture.step(40ms);
    expectations.expect(fixture.at(1) && fixture.playback.isHoldingForFrame(),
                        "frame 2 is pending, so the playhead holds even though frame 3 is ready");
    fixture.ready.insert(2);
    fixture.playback.tick();
    expectations.expect(fixture.at(2), "frame 2 shows when ready");
    fixture.step(40ms);
    expectations.expect(fixture.at(3), "frame 3 was ready: it plays one period later");
    fixture.ready.insert(4);
    fixture.ready.insert(6);
    int moves = 0;
    const auto counter =
        QObject::connect(&fixture.session, &bloom::ui::CompositionSession::currentTimeChanged,
                         [&moves] { ++moves; });
    fixture.step(200ms);
    QObject::disconnect(counter);
    expectations.expect(
        fixture.at(6) && moves == 1 && fixture.playback.isHoldingForFrame(),
        "a late wake goes as far as the ready frames reach in one move, then holds");
    fixture.finish(expectations);
}

// Frames nothing will cache play through the ordinary playback request instead of waiting.
void testUnavailableFramesDoNotWait(Expectations& expectations) {
    Fixture fixture("Unavailable");
    for (std::int64_t index = 0; index < 100; ++index)
        fixture.unavailable.insert(index);
    fixture.playback.play();
    fixture.step(40ms);
    expectations.expect(fixture.at(1) && !fixture.playback.isHoldingForFrame(),
                        "an unavailable frame does not hold the playhead");
    fixture.step(80ms);
    expectations.expect(fixture.at(3), "unavailable frames keep real time");
    fixture.finish(expectations);
}

// Pause, a seek, the end of a non-looping range and the loop wrap behave as they always have.
void testTransportEdges(Expectations& expectations) {
    Fixture fixture("Edges");
    fixture.playback.play();
    fixture.step(40ms);
    expectations.expect(fixture.playback.isHoldingForFrame(), "holding on frame 0");
    fixture.playback.pause();
    expectations.expect(
        fixture.playback.state() == bloom::ui::PlaybackState::Stopped &&
            !fixture.playback.isHoldingForFrame() && !fixture.playback.isWaitingForFrames() &&
            !fixture.controller.playbackWaiting() && !fixture.controller.playbackActive(),
        "pause during a hold stops and clears the waiting state");
    fixture.ready.insert(1);
    fixture.step(1s);
    expectations.expect(fixture.at(0) &&
                            fixture.playback.state() == bloom::ui::PlaybackState::Stopped,
                        "a later tick neither resumes nor moves time");

    fixture.playback.play();
    fixture.step(80ms);
    expectations.expect(fixture.at(1) && fixture.playback.isHoldingForFrame(), "holding on 1");
    (void)fixture.session.setCurrentTime(frame(50));
    expectations.expect(fixture.playback.state() == bloom::ui::PlaybackState::Stopped &&
                            !fixture.controller.playbackWaiting(),
                        "a seek during a hold pauses, as a seek during playback always has");

    // The loop wrap waits for the work area's first frame like any other.
    fixture.ready = {99};
    (void)fixture.session.setCurrentTime(frame(99));
    fixture.playback.play();
    fixture.step(40ms);
    expectations.expect(fixture.at(99) && fixture.playback.isHoldingForFrame(),
                        "the loop wrap holds while frame 0 is pending");
    fixture.ready.insert(0);
    fixture.playback.tick();
    expectations.expect(fixture.at(0), "the run wraps to frame 0 once it is ready");
    fixture.playback.pause();

    // A non-looping run still stops on the last frame, after waiting for it.
    fixture.playback.setLooping(false);
    fixture.ready = {98};
    (void)fixture.session.setCurrentTime(frame(98));
    fixture.playback.play();
    fixture.step(40ms);
    expectations.expect(fixture.at(98) && fixture.playback.isHoldingForFrame(),
                        "the last frame is waited for");
    fixture.ready.insert(99);
    fixture.playback.tick();
    expectations.expect(fixture.at(99) &&
                            fixture.playback.state() == bloom::ui::PlaybackState::Playing,
                        "the last frame shows when ready");
    fixture.step(40ms);
    expectations.expect(fixture.at(99) &&
                            fixture.playback.state() == bloom::ui::PlaybackState::Stopped,
                        "a non-looping run stops on its last frame, as before");
    fixture.finish(expectations);
}

// Audio mutes while the playhead waits and resumes in sync at the playhead once real-time playback
// resumes; a preroll that reached its maximum starts waiting with audio muted.
void testAudioMutesWhileWaiting(Expectations& expectations) {
    namespace audio = bloom::media::audio::playback;
    Fixture fixture("Audio");
    auto owned = std::make_unique<audio::AudioEngine>(std::make_unique<audio::NullBackend>(),
                                                      audio::AudioEngine::Config{1000, 2, 4096});
    auto* engine = owned.get();
    bloom::ui::AudioDeviceControl control(fixture.scheduler, fixture.bridge, std::move(owned));
    fixture.playback.setAudioDeviceControl(&control);
    bloom::runtime::AudioMixDescription mix;
    mix.clips.push_back({bloom::document::NodeId::fromRaw(1), bloom::document::AssetId::fromRaw(1),
                         time(0), time(10)});
    fixture.playback.setAudioMix(mix, {audio::AudioClip{nullptr, time(0)}});
    expectations.expect(waitUntil([&] { return control.isIdle(); }), "the mix is applied");

    fixture.ready = {1};
    fixture.playback.play();
    expectations.expect(waitUntil([&] { return fixture.playback.audioClockActive(); }),
                        "audio drives a run whose next frame is ready");
    expectations.expect(!engine->renderForTesting(40), "audio reaches frame 1");
    fixture.playback.tick();
    expectations.expect(fixture.at(1), "frame 1 plays on the audio clock");
    expectations.expect(!engine->renderForTesting(40), "audio reaches frame 2");
    fixture.playback.tick();
    expectations.expect(fixture.at(1) && fixture.playback.isWaitingForFrames(),
                        "frame 2 is pending: the playhead holds");
    expectations.expect(waitUntil([&] { return control.isIdle(); }) && !control.isPlaying() &&
                            !fixture.playback.audioClockActive(),
                        "audio is muted while the playhead waits");
    fixture.step(1s);
    expectations.expect(fixture.at(1), "the wall clock does not move a held playhead either");

    fixture.ready.insert(2);
    fixture.ready.insert(3);
    fixture.playback.tick();
    expectations.expect(fixture.at(2) && !fixture.playback.isWaitingForFrames(),
                        "frame 2 arrives and real-time playback resumes");
    expectations.expect(waitUntil([&] { return fixture.playback.audioClockActive(); }),
                        "audio resumes and drives the clock again");
    expectations.expect(control.positionNow() == frame(2),
                        "audio resumes at the playhead, in sync with the shown frame");
    expectations.expect(!engine->renderForTesting(40), "audio reaches frame 3");
    fixture.playback.tick();
    expectations.expect(fixture.at(3), "after the wait, audio and pictures stay in step");
    fixture.playback.pause();
    expectations.expect(waitUntil([&] { return control.isIdle(); }) && !control.isPlaying(),
                        "pause stops audio");

    // A preroll that reaches its maximum starts the run waiting, without starting audio.
    fixture.playback.setPrerollFrameCountProvider([] { return std::size_t{2}; });
    fixture.ready.clear();
    (void)fixture.session.setCurrentTime(frame(10));
    fixture.playback.play();
    expectations.expect(fixture.playback.isPreparing(), "Play holds for the preroll");
    fixture.step(500ms);
    expectations.expect(!fixture.playback.isPreparing() && fixture.playback.isWaitingForFrames() &&
                            fixture.controller.playbackWaiting(),
                        "a preroll at its maximum starts the run waiting for frames");
    expectations.expect(waitUntil([&] { return control.isIdle(); }) && !control.isPlaying() &&
                            !fixture.playback.audioClockActive(),
                        "audio does not start while the run waits");
    fixture.ready.insert(11);
    fixture.step(40ms);
    expectations.expect(fixture.at(11) && fixture.playback.isWaitingForFrames(),
                        "with a two-frame window, one ready frame plays but audio stays muted");
    fixture.ready.insert(12);
    fixture.ready.insert(13);
    fixture.step(40ms);
    expectations.expect(fixture.at(12) && !fixture.playback.isWaitingForFrames(),
                        "the window ahead is ready: real-time playback resumes");
    expectations.expect(waitUntil([&] { return fixture.playback.audioClockActive(); }) &&
                            control.positionNow() == frame(12),
                        "audio resumes at the playhead");
    fixture.playback.setAudioDeviceControl(nullptr);
    fixture.finish(expectations);
    control.beginShutdown();
    control.drainAfterEventLoop();
}

// The real read-ahead: the transport waits on the frames it caches, which enter the cache in
// order from the playhead, and the playhead shows every frame in turn.
void testReadAheadFillsContiguously(Expectations& expectations) {
    Fixture fixture("Read-ahead", false);
    bloom::ui::BackgroundPreviewController background(fixture.session, fixture.controller,
                                                      fixture.scheduler, fixture.bridge,
                                                      fixture.pipeline.pipeline);
    background.setReadAheadWidthProvider([](std::uint64_t) { return std::size_t{3}; });
    background.setTransportWaitsForFrames(true);
    fixture.playback.setFrameAvailabilityProvider([&](const bloom::core::RationalTime at) {
        return background.playbackFrameAvailability(at);
    });
    std::vector<std::int64_t> order;
    QObject::connect(&fixture.controller.frameCache(), &bloom::ui::PreviewFrameCache::entryRetained,
                     [&](const bloom::ui::PreviewFrameCacheKey& key) {
                         order.push_back(key.time.numerator() * 25 / key.time.denominator());
                     });
    std::vector<std::int64_t> shown{0};
    bool shownUncached = false;
    QObject::connect(&fixture.session, &bloom::ui::CompositionSession::currentTimeChanged, [&] {
        const auto now = fixture.session.currentTime();
        shown.push_back(now.numerator() * 25 / now.denominator());
        shownUncached = shownUncached || !fixture.cached(shown.back());
    });
    expectations.expect(background.playbackFrameAvailability(frame(1)) == Availability::Pending ||
                            background.playbackFrameAvailability(frame(1)) ==
                                Availability::Unavailable,
                        "an uncached frame is not ready");
    fixture.playback.play();
    // One frame per wake: a transport that does not wait would move onto uncached frames.
    expectations.expect(waitUntil([&] {
                            fixture.step(40ms);
                            return fixture.session.currentTime() >= frame(12);
                        }),
                        "uncached playback reaches frame 12");
    fixture.playback.pause();
    bool contiguous = shown.size() >= 13;
    for (std::size_t index = 1; index < shown.size() && index <= 12; ++index)
        contiguous = contiguous && shown[index] == shown[index - 1] + 1;
    expectations.expect(contiguous, "the playhead showed every frame from 0 to 12 in turn");
    expectations.expect(!shownUncached, "the playhead only ever moved onto cached frames");
    bool ordered = true;
    for (std::size_t index = 1; index < order.size(); ++index)
        ordered = ordered && order[index] > order[index - 1];
    expectations.expect(ordered && !order.empty() && order.front() <= 1,
                        "the read-ahead cached frames in order from the playhead");
    for (std::int64_t index = 1; index <= 12; ++index)
        expectations.expect(fixture.cached(index), "every frame the playhead passed is cached");
    expectations.expect(background.playbackFrameAvailability(frame(5)) == Availability::Ready,
                        "a cached frame is ready");
    fixture.finish(expectations);
}

// No provider: the transport keeps real time over uncached frames, exactly as before.
void testNoProviderNeverWaits(Expectations& expectations) {
    Fixture fixture("No provider", false);
    fixture.playback.play();
    fixture.step(200ms);
    expectations.expect(fixture.at(5) && !fixture.playback.isHoldingForFrame(),
                        "without a provider the playhead follows elapsed time");
    fixture.finish(expectations);
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("BloomPlaybackWaitTests"));
    app.setApplicationName(QStringLiteral("PlaybackWait"));
    bloom::ui::kit::installKinetikTheme(app);
    Expectations expectations;
    testHoldsOnUncachedFrame(expectations);
    testCachedFramesPlayRealTime(expectations);
    testNeverSkipsPendingFrame(expectations);
    testUnavailableFramesDoNotWait(expectations);
    testTransportEdges(expectations);
    testAudioMutesWhileWaiting(expectations);
    testReadAheadFillsContiguously(expectations);
    testNoProviderNeverWaits(expectations);
    if (expectations.failures() != 0) {
        std::cerr << expectations.failures() << " playback wait expectation(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "playback wait tests passed\n";
    return EXIT_SUCCESS;
}
