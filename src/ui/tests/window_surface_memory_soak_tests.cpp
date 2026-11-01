// Issue #414 soak: window resizes, maximize, moves between screens with different device pixel
// ratios, and hide/show must not grow memory per cycle or allocate a large transient peak.
//
// The harness is wired like the desktop application (apps/bloom/main.cpp): the process memory
// ledger with the application's MemoryBudgetController wiring, settings-derived cache budgets, the
// GPU-prepared packed preview service with native presentation disabled (the shipping Viewer
// path), background read-ahead, the RAM Preview controller, and the application's MainWindow with
// every foundation editor in the default workspace. One 386-frame composition is open and the
// Viewer is parked on frame 0, with the pointer resting over it so the pixel probe is live.
//
// It runs offscreen only. Before QApplication exists it installs a two-screen offscreen layout
// whose second screen is scaled by 1.5, so a move between them is a genuine DevicePixelRatioChange
// and screen change, as a move between a 1x and a 1.5x monitor is. Each cycle samples process RSS
// (and on Linux the cycle's peak RSS), every reusable pool on the ledger, GPU image allocation
// bytes, the widget and window counts, preview preparations, and retained presenter records.
// After a warm-up, the run asserts a plateau: no per-cycle growth beyond the stated tolerances, no
// new preview preparation for an unchanged frame, and a bounded transient peak per cycle.

#include "gpu_service_chain_fixture.hpp"

#include <bloom/document/new_project.hpp>
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/gpu_prepared_upload_cache.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/scripting/session.hpp>
#include <bloom/ui/background_preview_controller.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/gpu_viewer_bootstrap.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/memory_budget_controller.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/qualified_display_processor_bootstrap.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QFile>
#include <QMouseEvent>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <psapi.h>
#include <windows.h>
#pragma comment(lib, "psapi.lib")
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

using namespace bloom;
using namespace bloom::ui::verticalproof;
using namespace std::chrono_literals;

namespace {

constexpr int kWarmupCycles = 3;
constexpr int kDefaultMeasuredCycles = 12;
constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
// Tolerances, stated once. RSS may wander by allocator reuse and the scheduler's bounded terminal
// history (256 tasks) filling during the run; it must not trend. Pools and GPU images hold exactly
// what they held once the warm-up cached every variant.
constexpr std::uint64_t kRssPlateauTolerance = 48 * kMiB;
constexpr std::uint64_t kPoolPlateauTolerance = 4 * kMiB;
constexpr std::uint64_t kGpuPlateauTolerance = 0;
// A cycle's transient peak above its starting RSS: window backing stores at 1.5x plus paint
// temporaries, never a re-render or a full-frame reallocation per window change.
constexpr std::uint64_t kCyclePeakTolerance = 160 * kMiB;

[[nodiscard]] std::uint64_t currentRssBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)))
        return 0;
    return static_cast<std::uint64_t>(counters.WorkingSetSize);
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                    &count) != KERN_SUCCESS)
        return 0;
    return static_cast<std::uint64_t>(info.resident_size);
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t size = 0, resident = 0;
    statm >> size >> resident;
    return resident * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
}

// Linux only: the kernel's high-water mark since the last reset. Elsewhere the cycle peak is the
// highest RSS the harness's own 10 ms sampler observed.
[[nodiscard]] std::optional<std::uint64_t> kernelPeakRssBytes() {
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("VmHWM:")) {
            std::istringstream value(line.substr(6));
            std::uint64_t kibibytes = 0;
            value >> kibibytes;
            return kibibytes * 1024ULL;
        }
    }
#endif
    return std::nullopt;
}

void resetKernelPeakRss() {
#if defined(__linux__)
    std::ofstream reset("/proc/self/clear_refs");
    reset << "5";
#endif
}

struct Sample final {
    std::uint64_t rss = 0;
    std::uint64_t peak = 0;
    std::uint64_t ledger = 0;
    std::uint64_t preview = 0;
    std::uint64_t operation = 0;
    std::uint64_t decoded = 0;
    std::uint64_t uploads = 0;
    std::uint64_t gpuImages = 0;
    std::uint64_t preparations = 0;
    std::size_t widgets = 0;
    std::size_t windows = 0;
    std::size_t retainedPresenters = 0;
    std::size_t quarantined = 0;
};

[[nodiscard]] std::string describe(const int cycle, const Sample& sample) {
    std::ostringstream out;
    out << "SOAK cycle=" << cycle << " rssMiB=" << sample.rss / kMiB
        << " cyclePeakMiB=" << sample.peak / kMiB << " ledgerMiB=" << sample.ledger / kMiB
        << " previewMiB=" << sample.preview / kMiB << " operationMiB=" << sample.operation / kMiB
        << " decodedMiB=" << sample.decoded / kMiB << " uploadsMiB=" << sample.uploads / kMiB
        << " gpuImageMiB=" << sample.gpuImages / kMiB << " preparations=" << sample.preparations
        << " widgets=" << sample.widgets << " windows=" << sample.windows
        << " retainedPresenters=" << sample.retainedPresenters
        << " quarantinedGenerations=" << sample.quarantined;
    return out.str();
}

[[nodiscard]] double mean(const std::vector<Sample>& samples, const std::size_t first,
                          const std::size_t last, std::uint64_t Sample::* field) {
    double total = 0.0;
    for (std::size_t index = first; index < last; ++index)
        total += static_cast<double>(samples[index].*field);
    return last > first ? total / static_cast<double>(last - first) : 0.0;
}

// Installs the two-screen offscreen layout before QApplication is constructed. CTest launches the
// test with QT_QPA_PLATFORM=offscreen; any other platform is left alone and the soak is skipped.
[[nodiscard]] bool installOffscreenScreens(const std::filesystem::path& directory) {
    const auto platform = qEnvironmentVariable("QT_QPA_PLATFORM");
    if (platform != QStringLiteral("offscreen"))
        return false;
    const auto config = directory / "screens.json";
    std::ofstream out(config);
    out << R"({"synchronousWindowSystemEvents": false, "windowFrameMargins": false, "screens": [)"
        << R"({"name": "BloomSoakA", "x": 0, "y": 0, "width": 1920, "height": 1080,)"
        << R"( "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1},)"
        << R"({"name": "BloomSoakB", "x": 1920, "y": 0, "width": 2560, "height": 1440,)"
        << R"( "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1}]})";
    out.close();
    qputenv("QT_QPA_PLATFORM",
            QByteArray("offscreen:configfile=") + QFile::encodeName(config.string().c_str()));
    // The second screen is a 1.5x monitor: a move onto it changes every widget's device pixel
    // ratio.
    qputenv("QT_SCREEN_SCALE_FACTORS", "BloomSoakA=1;BloomSoakB=1.5");
    return true;
}

[[nodiscard]] bool writeProject(const std::filesystem::path& path) {
    auto format = document::CompositionFormat::create(1920, 1080);
    const auto duration = core::RationalTime::create(386, 24);
    if (!format || !duration)
        return false;
    // 386 frames at 24 fps, the owner's reported composition length.
    auto created =
        scripting::Session::createNew("Window Surface Soak", "Soak 386", *duration, *format);
    if (!created)
        return false;
    auto session = std::move(created).takeSession();
    return session != nullptr && session->saveAs(path).succeeded();
}

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    // `--cycles N` lengthens the measured window for a manual soak; CTest uses the default.
    int measuredCycles = kDefaultMeasuredCycles;
    std::vector<char*> forwarded{argv[0]};
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--cycles" && index + 1 < argc)
            measuredCycles = std::max(6, std::atoi(argv[++index]));
        else
            forwarded.push_back(argv[index]);
    }
    const auto options = parseOptions(static_cast<int>(forwarded.size()), forwarded.data());
    if (!options.valid)
        return 2;
    QTemporaryDir layoutDirectory;
    if (!layoutDirectory.isValid() ||
        !installOffscreenScreens(layoutDirectory.path().toStdString())) {
        std::cout << "Unsupported: the soak runs only on the offscreen platform\n";
        return options.require_device ? 1 : 77;
    }
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName("BloomSoak");
    QCoreApplication::setApplicationName("WindowSurfaceSoak");
    // The shipping default: Auto picks the preview resolution from the displayed scale, so a move
    // to the 1.5x screen is allowed to need another resolution once, and only once.
    QSettings().setValue("viewer/resolution", "Auto");
    const auto screens = QGuiApplication::screens();
    if (screens.size() < 2) {
        std::cout << "Unsupported: the offscreen two-screen layout did not load\n";
        return options.require_device ? 1 : 77;
    }
    QScreen* first = screens.at(0);
    QScreen* second = screens.at(1);

    Expectations checks;
    const std::filesystem::path projectPath =
        std::filesystem::path(temporary.path().toStdString()) / "window-surface-soak.bloom";
    if (!writeProject(projectPath)) {
        std::cerr << "the soak project could not be written\n";
        return 1;
    }

    runtime::processMemoryBudgetLedger().beginProcessSampling();
    const auto taskBudgetView = std::make_shared<runtime::TaskBudgetView>();
    auto taskConfig = runtime::TaskSchedulerConfig::defaults();
    taskConfig.budgetView = taskBudgetView;
    runtime::TaskScheduler scheduler(taskConfig);
    ui::ProjectHost projectHost(scheduler);
    bool replaced = false;
    std::optional<ui::ProjectHostOperationOutcome> openOutcome;
    QObject::connect(&projectHost, &ui::ProjectHost::sessionReplaced, [&] { replaced = true; });
    QObject::connect(&projectHost, &ui::ProjectHost::openFinished,
                     [&](const ui::ProjectHostOperationOutcome outcome, const QString&) {
                         openOutcome = outcome;
                     });
    projectHost.setUnsavedChangeDecisionProvider([] { return ui::UnsavedChangeDecision::Discard; });
    projectHost.setOpenPathProvider([&] { return std::optional(projectPath); });
    projectHost.requestOpen();
    if (!waitUntil([&] { return replaced || openOutcome.has_value(); }, 120s) || !replaced) {
        std::cerr << "the soak project did not open\n";
        return 1;
    }
    auto [document, commandStack] = projectHost.liveDocumentAndStack();
    if (document == nullptr || commandStack == nullptr)
        return 1;
    ui::CompositionSession session(*document, *commandStack, projectHost.lowestCompositionId());
    checks.expect(session.addSolidLayer(QStringLiteral("Back"), {0.1, 0.2, 0.5, 1.0}),
                  "the background solid is authored");
    checks.expect(session.addSolidLayer(QStringLiteral("Accent"), {0.8, 0.05, 0.02, 1.0}),
                  "the accent solid is authored");
    checks.expect(session.setSelectedScale(0.6, 0.6), "the accent leaves the background visible");
    checks.expect(session.addTextLayer(QStringLiteral("Title"), QStringLiteral("Soak"), 120.0),
                  "the title text is authored");
    // A new session already sits on frame 0; setting it again is a no-op.
    static_cast<void>(session.setCurrentTime(core::RationalTime::fromInteger(0)));

    runtime::NodeDefinitionRegistry definitions;
    if (!runtime::registerBuiltInNodeDefinitions(definitions))
        return 2;
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(projectPath.parent_path());
    const QSettings settingsView;
    const auto cacheBudgets = ui::cacheMemoryBudgetsFromSettings(settingsView);
    evaluator.operationCache()->setByteBudget(cacheBudgets.operationCacheByteBudget);
    runtime::CpuReferenceDisplayPreparer referenceDisplayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProvider;
    const auto retirementOwner = render::PackedHostRetirementOwner::create();
    ui::TaskUiBridge bridge(scheduler);
    const auto policy =
        std::make_shared<runtime::GpuProcessingPhasePolicy>(32, retirementOwner, taskBudgetView);
    ui::MemoryBudgetController memoryController(scheduler, bridge,
                                                runtime::processMemoryBudgetLedger(),
                                                ui::applicationMemorySampler(policy));
    static_cast<void>(ui::connectApplicationMemoryBudget(
        memoryController, policy, runtime::processMemoryBudgetLedger(), &bridge));
    ui::QualifiedDisplayProcessorBootstrap bootstrap(scheduler, bridge, qualifiedProvider);
    auto frameCache =
        std::make_shared<ui::PreviewFrameCache>(cacheBudgets.previewFrameCacheByteBudget);
    auto planCache = std::make_shared<ui::CompiledPlanCache>();
    const auto pipeline = ui::makeCompositionPreviewPipeline(
        compiler, evaluator, referenceDisplayPreparer, qualifiedProvider, planCache);

    // The shipping preview service: GPU-prepared packed frames, native presentation disabled.
    // Without a pinned loader the service stays disabled and every frame takes the CPU stage.
    runtime::GpuPreviewDisplayServiceOptions serviceOptionsApp;
    serviceOptionsApp.enabled = !options.loader_path.empty();
    serviceOptionsApp.loaderPath = options.loader_path;
    serviceOptionsApp.product = runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
    serviceOptionsApp.presentation = runtime::GpuPreviewDisplayServicePresentationMode::Disabled;
    const auto residentPlan =
        ui::gpuResidentBudgetPlanFor(cacheBudgets.previewFrameCacheByteBudget);
    ui::applyGpuResidentBudgetPlan(serviceOptionsApp, residentPlan);
    serviceOptionsApp.previewByteAllowance.reset();
    ui::applyGpuResidentCacheLimits(*frameCache, residentPlan);
    auto preparedCache = std::make_shared<runtime::GpuPreparedUploadCache>(
        evaluator.operationCache(), retirementOwner);
    std::shared_ptr<runtime::GpuOcioContextResolver> resolver =
        host::makePackagedGpuOcioResolver(QCoreApplication::applicationFilePath().toStdString());
    if (resolver == nullptr)
        resolver = std::make_shared<runtime::GpuOcioContextResolver>();
    const auto deviceWorkingBytes = (512ULL << 20) + serviceOptionsApp.packedTransferByteAllowance;
    auto gpuStage = ui::makeSessionRefreshingGpuSceneStage(
        compiler, evaluator, qualifiedProvider, std::make_shared<runtime::GpuSceneCoverageCache>(),
        preparedCache, planCache, resolver,
        {.policy = policy,
         .envelope = {.metadataBytes = 16ULL << 20,
                      .retainedCoverageByteLimit = 16ULL << 20,
                      .requestedExecutorBytes = std::nullopt,
                      .deviceWorkingBytes = deviceWorkingBytes}});
    serviceOptionsApp.processingPhasePolicy = policy;
    serviceOptionsApp.cpuStoragePlanner =
        ui::makeCompositionPreviewCpuStoragePlanner(compiler, evaluator, planCache);
    auto serviceOwner = std::make_unique<runtime::GpuPreviewDisplayService>(
        scheduler, std::move(gpuStage),
        ui::makeCompositionPreviewCpuStage(compiler, evaluator, qualifiedProvider, planCache),
        ui::makeCompositionPreviewCpuDisplayFallback(referenceDisplayPreparer), serviceOptionsApp);
    auto& service = *serviceOwner;
    const ui::PreviewPreparationSubmitter submitter =
        [&service](runtime::TaskRequest request, const document::Snapshot& snapshot,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t bytes,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            return service.submit(std::move(request), snapshot, identity, bytes, overrides);
        };
    ui::CompositionPreviewSettings settings;
    settings.colorIntent = session.colorIntent();
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                frameCache, nullptr, submitter, nullptr, policy);
    QObject::connect(&memoryController, &ui::MemoryBudgetController::sampleApplied, &controller,
                     &ui::CompositionPreviewController::notifyProcessingCapacityChanged);
    const auto widthProvider = [policy, deviceWorkingBytes](const std::uint64_t pixels) {
        return runtime::previewFramesThatFit(*policy, pixels, deviceWorkingBytes);
    };
    ui::BackgroundPreviewController background(session, controller, scheduler, bridge, pipeline,
                                               nullptr, submitter);
    background.setReadAheadWidthProvider(widthProvider);
    ui::RamPreviewController ram(session, controller, scheduler, bridge, pipeline, nullptr,
                                 submitter);
    ram.setFillWidthProvider(widthProvider);
    ui::EditorRegistry registry;
    if (!ui::registerFoundationEditors(registry, session, controller, &ram, &projectHost))
        return 2;
    auto window =
        std::make_unique<ui::MainWindow>(registry, session, projectHost, &ram, &controller, nullptr,
                                         &controller.playbackController());
    window->resize(1600, 960);
    window->show();

    const auto shutdown = [&] {
        ram.beginShutdown();
        background.beginShutdown();
        controller.beginShutdown();
        memoryController.beginShutdown();
        bridge.beginShutdown();
        service.beginShutdown();
        const bool stopped = waitUntil(
            [&] {
                return scheduler.isQuiescent() &&
                       service.status().state == runtime::GpuPreviewDisplayServiceState::Stopped;
            },
            30s);
        if (auto* viewer = window->displayedViewer())
            viewer->releasePackedFramesForShutdown();
        window->completeShutdown();
        window.reset();
        controller.releasePublishedFramesForShutdown();
        frameCache->clear();
        evaluator.operationCache()->closePreparedUploads();
        retirementOwner->beginShutdown();
        const bool retired = waitUntil([&] { return retirementOwner->exitAcknowledged(); }, 15s);
        while (retired && !retirementOwner->collect())
            QCoreApplication::processEvents();
        checks.expect(stopped && retired, "the soak shuts down cleanly");
    };

    if (!waitUntil(
            [&] {
                return service.status().state !=
                       runtime::GpuPreviewDisplayServiceState::Initializing;
            },
            90s)) {
        shutdown();
        return 1;
    }
    const bool gpu = service.status().state == runtime::GpuPreviewDisplayServiceState::Ready;
    if (!gpu && options.require_device) {
        std::cout << "Unsupported: " << service.status().residentDetail << '\n';
        shutdown();
        return 1;
    }
    std::cout << "SOAK route=" << (gpu ? "gpu-prepared-packed" : "cpu") << '\n';
    if (!waitUntil([&] { return window->displayedViewer() != nullptr; }, 30s)) {
        shutdown();
        return 1;
    }
    auto* viewer = window->displayedViewer();
    const auto current = [&] {
        return viewer->displayedPreviewState().freshness == ui::FrameFreshness::Current;
    };
    checks.expect(waitUntil(current, 120s), "the parked frame is painted Current");

    // Rest the pointer over the Viewer, as an artist's hand does while moving the window.
    const auto restPointer = [&] {
        const QPointF local(viewer->width() * 0.5, viewer->height() * 0.45);
        QMouseEvent move(QEvent::MouseMove, local, viewer->mapToGlobal(local), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewer, &move);
    };
    restPointer();

    // Peak sampling. The kernel high-water mark is exact on Linux; the 10 ms sampler covers the
    // other platforms and also runs on Linux as a cross-check.
    std::uint64_t sampledPeak = 0;
    QTimer peakTimer;
    peakTimer.setInterval(10);
    QObject::connect(&peakTimer, &QTimer::timeout,
                     [&] { sampledPeak = std::max(sampledPeak, currentRssBytes()); });
    peakTimer.start();

    const auto settle = [&] {
        // Quiescent means: the Viewer shows the parked frame Current and no task is running.
        static_cast<void>(waitUntil([&] { return current() && scheduler.isQuiescent(); }, 20s));
        const auto until = std::chrono::steady_clock::now() + 150ms;
        while (std::chrono::steady_clock::now() < until)
            QApplication::processEvents();
    };
    const auto moveTo = [&](QScreen* screen) {
        if (auto* handle = window->windowHandle())
            handle->setScreen(screen);
        window->move(screen->geometry().topLeft() + QPoint(40, 40));
    };
    const auto sample = [&](const std::uint64_t cycleStartRss) {
        Sample value;
        value.rss = currentRssBytes();
        const auto peak = std::max(sampledPeak, kernelPeakRssBytes().value_or(0));
        value.peak = peak > cycleStartRss ? peak - cycleStartRss : 0;
        value.ledger = runtime::processMemoryBudgetLedger().reusableRetainedBytes();
        value.preview = frameCache->residentBytes();
        value.operation = evaluator.operationCache()->retainedBytes();
        value.decoded = evaluator.operationCache()->retainedBytes(
            runtime::OperationCacheEntryKind::DecodedMedia);
        value.uploads = evaluator.operationCache()->retainedBytes(
            runtime::OperationCacheEntryKind::PreparedUpload);
        const auto images = render::GpuDevice::processImageReclamationStats();
        value.gpuImages = images.liveAllocationBytes + images.retiringAllocationBytes;
        const auto counters = service.status().counters;
        value.preparations = counters.residentGraphJobs + counters.cpuFallbacks +
                             frameCache->statistics().insertions;
        value.widgets = static_cast<std::size_t>(QApplication::allWidgets().size());
        value.windows = static_cast<std::size_t>(QGuiApplication::allWindows().size());
        for (auto* editor : window->findChildren<ui::ViewerEditor*>()) {
            if (editor->gpuPresenterStateForTest() == ui::ViewerGpuPresenter::State::Retained)
                ++value.retainedPresenters;
        }
        value.quarantined = runtime::GpuPresentationCoordinator::quarantinedGenerationCount();
        return value;
    };

    // One cycle: four resizes, maximize and restore, a move onto the 1.5x screen and a resize
    // there, a move back, and hide/show (an unmap and remap, as a virtual-desktop switch can be).
    const std::vector<QSize> sizes{{1500, 900}, {1320, 820}, {1660, 980}, {1410, 860}};
    std::vector<Sample> samples;
    for (int cycle = 0; cycle < kWarmupCycles + measuredCycles; ++cycle) {
        settle();
        const auto start = currentRssBytes();
        sampledPeak = start;
        resetKernelPeakRss();
        for (const auto& size : sizes) {
            window->resize(size);
            restPointer();
            settle();
        }
        window->showMaximized();
        settle();
        window->showNormal();
        settle();
        moveTo(second);
        restPointer();
        settle();
        checks.expect(viewer->devicePixelRatioF() == 1.5 || cycle > 0,
                      "the second screen scales the Viewer to 1.5x");
        window->resize(1700, 1000);
        restPointer();
        settle();
        moveTo(first);
        restPointer();
        settle();
        window->hide();
        settle();
        window->show();
        restPointer();
        settle();
        const auto value = sample(start);
        std::cout << describe(cycle, value) << (cycle < kWarmupCycles ? " (warm-up)" : "") << '\n';
        if (cycle >= kWarmupCycles)
            samples.push_back(value);
    }
    peakTimer.stop();

    // Plateau: the mean of the last third against the first third, and the final sample against
    // the first measured one. Exact counts must not move at all.
    const auto third = samples.size() / 3;
    const auto growth = [&](std::uint64_t Sample::* field) {
        return mean(samples, samples.size() - third, samples.size(), field) -
               mean(samples, 0, third, field);
    };
    const auto& firstSample = samples.front();
    const auto& lastSample = samples.back();
    std::cout << "SOAK plateau rssGrowthMiB=" << growth(&Sample::rss) / kMiB
              << " ledgerGrowthMiB=" << growth(&Sample::ledger) / kMiB
              << " gpuImageGrowthMiB=" << growth(&Sample::gpuImages) / kMiB
              << " preparationsDuringMeasure=" << lastSample.preparations - firstSample.preparations
              << " maxCyclePeakMiB="
              << std::max_element(
                     samples.begin(), samples.end(),
                     [](const Sample& a, const Sample& b) {
                         return a.peak < b.peak;
                     })->peak /
                     kMiB
              << '\n';
    checks.expect(growth(&Sample::rss) <= static_cast<double>(kRssPlateauTolerance),
                  "RSS reaches a plateau within 48 MiB across the measured cycles");
    checks.expect(lastSample.rss <= firstSample.rss + kRssPlateauTolerance,
                  "the final RSS stays within 48 MiB of the first measured cycle");
    checks.expect(growth(&Sample::ledger) <= static_cast<double>(kPoolPlateauTolerance) &&
                      lastSample.ledger <= firstSample.ledger + kPoolPlateauTolerance,
                  "the ledger's reusable pools reach a plateau");
    checks.expect(lastSample.preview <= firstSample.preview + kPoolPlateauTolerance &&
                      lastSample.operation <= firstSample.operation + kPoolPlateauTolerance,
                  "the preview and operation caches hold no new bytes per window change");
    checks.expect(lastSample.gpuImages <= firstSample.gpuImages + kGpuPlateauTolerance,
                  "GPU image allocations do not grow per window change");
    checks.expect(lastSample.preparations == firstSample.preparations,
                  "a window change never re-prepares the unchanged parked frame");
    for (const auto& value : samples) {
        checks.expect(value.peak <= kCyclePeakTolerance,
                      "a cycle's transient peak stays within 160 MiB of its starting RSS");
        checks.expect(value.widgets == firstSample.widgets && value.windows == firstSample.windows,
                      "widget and window counts do not grow per cycle");
        checks.expect(value.retainedPresenters == 0,
                      "no presenter record is retained by a window change");
        checks.expect(value.quarantined == firstSample.quarantined,
                      "no presentation generation is quarantined by a window change");
    }

    shutdown();
    if (checks.failures() != 0) {
        std::cerr << "window surface soak: " << checks.failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: window surface memory soak (" << measuredCycles << " measured cycles)\n";
    return 0;
}
