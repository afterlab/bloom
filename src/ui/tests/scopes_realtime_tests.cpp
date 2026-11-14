// bloom.ui.scopes-realtime (#724 Scopes): the Viewer is never delayed by the Scopes panel.
//
// A real CompositionPreviewController and PreviewFrameCache, a real Viewer, and a visible Scopes
// panel that follows it measure a 64 x 64 composition with the production measure over the CPU
// route, held shut in a gate the test controls. With the measure held, the session is stepped
// through several frames. Asserted:
//   * the preview controller publishes every stepped frame and the Viewer, and so the panel's
//     followed source, shows each one while the measure is still held;
//   * the held measure ran on a task worker and never on the interface thread, whose event loop
//     kept turning (a queued call ran after every step);
//   * exactly one measure was in flight throughout, and the frames in between were never measured:
//     once released, only the newest shown frame is measured (live, then full).
// What this proves and what it cannot: it proves the structure, that the panel only asks for a
// measure on a task worker, holds one at a time and never waits for it on the interface thread or
// on the preview's own work. It runs the CPU routes, so it says nothing about the device: that the
// table readback does not wait on the Viewer's frame is established on the device by
// bloom.runtime.scope_frame_measure_native and by docs/architecture/scopes.md, "Real-time cost".
// No timing is measured or asserted.

#include "await_qt.hpp"
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/scopes_controller.hpp>
#include <bloom/ui/scopes_editor.hpp>
#include <bloom/ui/scopes_region_store.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QSettings>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace document = bloom::document;
namespace runtime = bloom::runtime;
namespace ui = bloom::ui;
using bloom::test_support::awaitFailureCount;
using bloom::test_support::awaitQtCondition;
using namespace std::chrono_literals;

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

document::CompositionFormat format() {
    const auto value = document::CompositionFormat::create(64, 64);
    if (!value.has_value())
        std::abort();
    return *value;
}

struct Gate final {
    std::mutex mutex;
    std::condition_variable condition;
    bool held = false;
    std::atomic<int> entered{0};
    std::atomic<int> running{0};
    std::atomic<int> maxRunning{0};
    std::atomic<bool> onInterfaceThread{false};
    std::atomic<bool> noProcessFrame{false};
    struct Call final {
        double seconds = 0.0;
        std::uint32_t detail = 0;
    };
    std::vector<Call> calls;

    void hold(const bool value) {
        const std::lock_guard lock(mutex);
        held = value;
        condition.notify_all();
    }
    void enter(const double seconds, const std::uint32_t detail) {
        std::unique_lock lock(mutex);
        calls.push_back({seconds, detail});
        ++entered;
        const int now = ++running;
        int seen = maxRunning.load();
        while (now > seen && !maxRunning.compare_exchange_weak(seen, now)) {
        }
        condition.wait(lock, [&] { return !held; });
    }
    [[nodiscard]] std::vector<Call> callsSince(const std::size_t first) {
        const std::lock_guard lock(mutex);
        return {calls.begin() + static_cast<std::ptrdiff_t>(first), calls.end()};
    }
};

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QTemporaryDir settingsDirectory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    application.setOrganizationName(QStringLiteral("BloomScopesRealtime"));
    application.setApplicationName(QStringLiteral("ScopesRealtime"));
    ui::kit::installAfterlabTheme(application);
    Expectations e;

    auto created = document::makeNewProject("Scopes realtime", "Main",
                                            bloom::core::RationalTime::fromInteger(10), format());
    document::Document doc(std::move(created.project));
    bloom::commands::CommandStack commands(doc);
    ui::CompositionSession session(doc, commands, created.initialCompositionId);

    runtime::NodeDefinitionRegistry definitions;
    if (!runtime::registerBuiltInNodeDefinitions(definitions))
        return 1;
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualified;
    // Enough workers that a held measure cannot starve the preview's own preparation.
    runtime::TaskScheduler scheduler{runtime::TaskSchedulerConfig{.cpuWorkerCount = 3,
                                                                  .blockingIoWorkerCount = 1,
                                                                  .cpuQueueCapacity = 32,
                                                                  .blockingIoQueueCapacity = 4,
                                                                  .terminalHistoryCapacity = 64,
                                                                  .diagnosticsPerTask = 8,
                                                                  .groupRegistryCapacity = 8}};
    ui::TaskUiBridge bridge{scheduler, nullptr, 1ms};
    auto cache = std::make_shared<ui::PreviewFrameCache>(ui::kMinimumPreviewFrameCacheByteBudget);
    ui::CompositionPreviewSettings previewSettings;
    previewSettings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
    previewSettings.colorIntent = session.colorIntent();
    const auto evaluations = std::make_shared<std::atomic<int>>(0);
    const auto pipeline =
        ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer, qualified);
    ui::CompositionPreviewController controller(
        session, scheduler, bridge,
        [pipeline, evaluations](const document::Snapshot& snapshot,
                                const runtime::PreviewRequestIdentity& identity,
                                const std::size_t limit,
                                const std::vector<runtime::SnapshotParameterOverride>& overrides,
                                runtime::TaskContext& context) {
            ++*evaluations;
            return pipeline(snapshot, identity, limit, overrides, context);
        },
        previewSettings, cache);

    // The measure has no evaluator to call (it counts the CPU frame's process picture); the scene
    // runs are the preview's own, counted by wrapping its preparation function.
    const auto production = ui::makeScopesMeasure({});
    const auto gate = std::make_shared<Gate>();
    const auto interfaceThread = std::this_thread::get_id();
    ui::ScopesServices services;
    services.scheduler = &scheduler;
    services.bridge = &bridge;
    services.cache = cache;
    services.measure = [production, gate,
                        interfaceThread](const ui::ScopesMeasureInput& input,
                                         const ui::PreviewScopeIdentity& identity,
                                         const runtime::CancellationToken& cancellation) {
        if (std::this_thread::get_id() == interfaceThread)
            gate->onInterfaceThread = true;
        // The shown frame's picture is the CPU route's process frame: the measure counts it.
        if (input.frame == nullptr || !input.frame->hasProcessFrame())
            gate->noProcessFrame = true;
        gate->enter(input.source.identity.time.toSeconds(), identity.settings.detail);
        auto result = production(input, identity, cancellation);
        --gate->running;
        return result;
    };
    ui::ScopesRegionStore store;
    services.regions = &store;

    {
        ui::ViewerEditor viewer(session, controller);
        viewer.setScopesRegionStore(&store);
        viewer.resize(400, 300);
        viewer.show();
        ui::ScopesEditor panel(services);
        panel.show();
        panel.followViewer(&viewer);
        auto& scopes = *panel.controller();

        const auto shownTime = [&] {
            const auto frame = viewer.scopesSource().frame;
            return frame ? frame->desiredIdentity().time.toSeconds() : -1.0;
        };
        e.expect(
            awaitQtCondition("the first frame is shown and measured, live and full",
                             [&] {
                                 return controller.state().activity == ui::PreviewActivity::Ready &&
                                        controller.state().frame != nullptr && shownTime() == 0.0 &&
                                        scopes.reading().state == ui::ScopesReading::State::Ready &&
                                        !scopes.busy();
                             }),
            "the first frame is shown and measured, live and full");
        const auto base = gate->entered.load();
        const auto started = scopes.measuresStarted();
        e.expect(scopes.reading().scopes != nullptr &&
                     scopes.reading().scopes->identity.source == runtime::ScopeSource::Rendered &&
                     scopes.reading().scopes->tables.route() == runtime::ScopeRoute::Cpu,
                 "the shown frame was counted from its process picture (Rendered, CPU)");

        // The measure is held shut; the session steps through five more frames.
        gate->hold(true);
        for (int second = 1; second <= 5; ++second) {
            e.expect(session.setCurrentTime(bloom::core::RationalTime::fromInteger(second)),
                     "the session steps to the next frame");
            e.expect(awaitQtCondition(
                         "the preview controller publishes the stepped frame",
                         [&] {
                             return controller.state().activity == ui::PreviewActivity::Ready &&
                                    controller.state().frame != nullptr &&
                                    controller.state().frame->desiredIdentity().time.toSeconds() ==
                                        static_cast<double>(second);
                         }),
                     "the preview controller publishes the stepped frame");
            e.expect(awaitQtCondition("the Viewer shows the stepped frame",
                                      [&] { return shownTime() == static_cast<double>(second); }) &&
                         scopes.reading().shown &&
                         scopes.reading().shown->identity.time.toSeconds() ==
                             static_cast<double>(second),
                     "the Viewer, and the panel following it, show the stepped frame");
            if (second == 1)
                e.expect(awaitQtCondition("the first measure of the run started and is held",
                                          [&] { return gate->entered.load() > base; }),
                         "the first measure of the run started and is held");
            bool ran = false;
            QMetaObject::invokeMethod(&application, [&ran] { ran = true; }, Qt::QueuedConnection);
            e.expect(awaitQtCondition("the interface thread's event loop kept turning",
                                      [&ran] { return ran; }),
                     "the interface thread's event loop kept turning");
            e.expect(gate->entered.load() == base + 1 && scopes.measuresInFlight() == 1 &&
                         scopes.maxMeasuresInFlight() == 1 &&
                         scopes.measuresStarted() == started + 1,
                     "exactly one measure is in flight, and it is still the held one");
        }
        e.expect(!gate->onInterfaceThread.load() && gate->running.load() == 1,
                 "the held measure runs on a task worker");

        // Released: only the newest frame is measured, live then full. Every scene run so far was
        // the preview's own frame; the measures now to run add none.
        const auto scenesAtRelease = evaluations->load();
        gate->hold(false);
        e.expect(awaitQtCondition("the scopes settle on the newest frame",
                                  [&] {
                                      return scopes.reading().state ==
                                                 ui::ScopesReading::State::Ready &&
                                             !scopes.busy();
                                  }),
                 "the scopes settle on the newest frame");
        const auto calls = gate->callsSince(static_cast<std::size_t>(base));
        e.expect(calls.size() == 3 && calls[0].seconds == 1.0 && calls[1].seconds == 5.0 &&
                     calls[1].detail == ui::kLiveDetail && calls[2].seconds == 5.0 &&
                     calls[2].detail == 0U,
                 "frames 2 to 4 were never measured; the newest was, live then full");
        e.expect(scopes.reading().measured &&
                     scopes.reading().measured->identity.time.toSeconds() == 5.0 &&
                     scopes.reading().detail == 0U,
                 "the reading is the newest frame's, at full detail");
        e.expect(!gate->onInterfaceThread.load() && gate->maxRunning.load() == 1 &&
                     scopes.maxMeasuresInFlight() == 1,
                 "no measure ran on the interface thread and never two ran at once");
        e.expect(!gate->noProcessFrame.load(),
                 "every measure was handed the shown frame's process picture");
        e.expect(evaluations->load() == scenesAtRelease,
                 "the scopes ran no scene: the released measures evaluated nothing");
    }

    controller.beginShutdown();
    bridge.beginShutdown();
    (void)awaitQtCondition("the scheduler drains after shutdown",
                           [&] { return scheduler.isQuiescent(); });
    if (e.failures() + awaitFailureCount() != 0) {
        std::cerr << e.failures() + awaitFailureCount() << " scope realtime check(s) failed\n";
        return 1;
    }
    std::cout << "PASS: scopes realtime\n";
    return 0;
}
