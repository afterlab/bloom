// Issue #414 soak: moving the window between a 1x and a 1.5x screen, resizing, maximizing and
// hiding it must not grow memory per cycle or allocate a large transient peak.
//
// The harness is wired like the desktop application (apps/bloom/main.cpp): the process memory
// ledger with the application's MemoryBudgetController wiring, settings-derived cache budgets, the
// GPU-prepared packed preview service with native presentation disabled (the shipping Viewer
// path), background read-ahead, the RAM Preview controller, and the application's MainWindow with
// every foundation editor in the default workspace. The project is shaped like the reported one: a
// 2048x858, 386-frame composition whose layers are 16-bit RGBA PNG sequences. The Viewer is
// parked on frame 0 at the shipping Auto resolution, and the pointer rests over it, a few pixels
// elsewhere after every window change as a hand does, so the pixel probe is live.
//
// It runs offscreen only. Before QApplication exists it installs a two-screen offscreen layout: a
// 1920x1080 screen at 1x and a 3840x2160 screen at 1.5x. The window sizes on each screen are the
// reported ones, and they put Auto on Half on the first screen and Full on the second, so every
// move between them is a genuine device-pixel-ratio change, screen change and preview divisor
// flip. Each cycle samples process RSS (and on Linux the cycle's peak RSS), every reusable pool on
// the ledger, GPU image allocation bytes, the widget and window counts, preview preparations, and
// retained presenter records. After the first Full/Half round trip (the warm-up cycle), the run
// asserts a plateau: no per-cycle growth beyond the stated tolerances, no new preview preparation
// for an unchanged frame, and a bounded transient peak per cycle.
//
// Before the fix the probe's one-pixel CPU analysis stored every layer's full-frame resampled
// footage in the operation cache under a key that named the probed pixel, so each window change
// (and each new pointer position) retained another set: about 100 MiB per move here.

#include "await_qt.hpp"
#include "gpu_service_chain_fixture.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
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
#include <QImage>
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

// One warm-up cycle: the first Full/Half round trip prepares and caches both resolutions once.
constexpr int kWarmupCycles = 1;
constexpr int kDefaultMeasuredCycles = 8;
// The reported project: a 2048x858 composition over 16-bit RGBA PNG sequence layers.
constexpr int kCompositionWidth = 2048;
constexpr int kCompositionHeight = 858;
constexpr int kFootageLayers = 4;
constexpr int kFootageMembers = 4;
constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
// Tolerances, stated once. RSS may wander by allocator reuse and the scheduler's bounded terminal
// history (256 tasks) filling during the run; it must not trend. Pools and GPU images hold exactly
// what they held once the warm-up round trip cached both resolutions.
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

// Linux only: resident anonymous memory (heap and anonymous mappings), the figure the reported
// capture tracked. Zero elsewhere.
[[nodiscard]] std::uint64_t anonymousRssBytes() {
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("RssAnon:")) {
            std::istringstream value(line.substr(8));
            std::uint64_t kibibytes = 0;
            value >> kibibytes;
            return kibibytes * 1024ULL;
        }
    }
#endif
    return 0;
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
    std::uint64_t gpuImages = 0;
    std::uint64_t preparations = 0;
    std::uint64_t viewerCpuFallbacks = 0;
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
        << " decodedMiB=" << sample.decoded / kMiB << " gpuImageMiB=" << sample.gpuImages / kMiB
        << " preparations=" << sample.preparations
        << " viewerCpuFallbacks=" << sample.viewerCpuFallbacks << " widgets=" << sample.widgets
        << " windows=" << sample.windows << " retainedPresenters=" << sample.retainedPresenters
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
#if defined(_WIN32)
    // Qt splits the platform string at every colon, so a path with a drive letter cannot be
    // passed. The layout is written to the working folder and named without its folder.
    static_cast<void>(directory);
    const std::filesystem::path config =
        "bloom-soak-screens-" + std::to_string(QCoreApplication::applicationPid()) + ".json";
#else
    const auto config = directory / "screens.json";
#endif
    std::ofstream out(config);
    out << R"({"synchronousWindowSystemEvents": false, "windowFrameMargins": false, "screens": [)"
        << R"({"name": "BloomSoakA", "x": 0, "y": 0, "width": 1920, "height": 1080,)"
        << R"( "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1},)"
        << R"({"name": "BloomSoakB", "x": 1920, "y": 0, "width": 3840, "height": 2160,)"
        << R"( "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1}]})";
    out.close();
    qputenv("QT_QPA_PLATFORM",
            QByteArray("offscreen:configfile=") + QFile::encodeName(config.string().c_str()));
    // The second screen is a 3840x2160 monitor at 1.5x (2560x1440 logical): a move onto it changes
    // every widget's device pixel ratio.
    qputenv("QT_SCREEN_SCALE_FACTORS", "BloomSoakA=1;BloomSoakB=1.5");
    return true;
}

[[nodiscard]] bool writeProject(const std::filesystem::path& path) {
    auto format = document::CompositionFormat::create(kCompositionWidth, kCompositionHeight);
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

// Writes one 16-bit RGBA PNG sequence per layer beside the project and returns each sequence's
// first member. Every member is distinct and partly transparent, so each layer contributes.
[[nodiscard]] std::vector<std::filesystem::path> writeFootage(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> firstMembers;
    for (int layer = 0; layer < kFootageLayers; ++layer) {
        for (int member = 0; member < kFootageMembers; ++member) {
            QImage image(kCompositionWidth, kCompositionHeight, QImage::Format_RGBA64);
            for (int y = 0; y < kCompositionHeight; ++y) {
                auto* row = reinterpret_cast<QRgba64*>(image.scanLine(y));
                for (int x = 0; x < kCompositionWidth; ++x) {
                    const auto red = static_cast<quint16>((x * 32 + layer * 9001) & 0xFFFF);
                    const auto green = static_cast<quint16>((y * 64 + member * 4099) & 0xFFFF);
                    const auto blue = static_cast<quint16>(((x ^ y) * 16 + layer * 777) & 0xFFFF);
                    row[x] = QRgba64::fromRgba64(red, green, blue, 40000).premultiplied();
                }
            }
            const auto name =
                "plate" + std::to_string(layer) + "." +
                QStringLiteral("%1").arg(member, 4, 10, QLatin1Char('0')).toStdString() + ".png";
            const auto file = folder / name;
            if (!image.save(QString::fromStdString(file.string()), "PNG"))
                return {};
            if (member == 0)
                firstMembers.push_back(file);
        }
    }
    return firstMembers;
}

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    // Unbuffered, so a cycle's line is in the log the moment it is sampled.
    std::cout.setf(std::ios::unitbuf);
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
#if defined(_WIN32)
    {
        // The layout was read while the application was made.
        std::error_code ignored;
        std::filesystem::remove("bloom-soak-screens-" +
                                    std::to_string(QCoreApplication::applicationPid()) + ".json",
                                ignored);
    }
#endif
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
    if (!bloom::test_support::awaitQtCondition(
            "project open finishes", [&] { return replaced || openOutcome.has_value(); }, 120s) ||
        !replaced) {
        std::cerr << "the soak project did not open\n";
        return 1;
    }
    auto [document, commandStack] = projectHost.liveDocumentAndStack();
    if (document == nullptr || commandStack == nullptr)
        return 1;
    ui::CompositionSession session(*document, *commandStack, projectHost.lowestCompositionId());
    const auto projectDirectory = projectPath.parent_path();
    const auto footage = writeFootage(projectDirectory);
    checks.expect(footage.size() == static_cast<std::size_t>(kFootageLayers),
                  "the 16-bit PNG sequences are written");
    commands::Transaction import("Import footage", session.snapshot().revision());
    import.emplace<commands::ImportAssets>(footage, projectDirectory);
    checks.expect(session.executeTransaction(std::move(import)).succeeded(),
                  "the PNG sequences import");
    const auto assets = session.snapshot().project().assets();
    for (const auto& asset : assets) {
        checks.expect(asset.kind == document::AssetKind::Sequence &&
                          asset.manifest.members.size() ==
                              static_cast<std::size_t>(kFootageMembers),
                      "each plate imports as one sequence");
        commands::Transaction add("Add plate", session.snapshot().revision());
        add.emplace<commands::AddImageLayer>(session.compositionId(), asset.id);
        checks.expect(session.executeTransaction(std::move(add)).succeeded(),
                      "a sequence layer is authored");
    }
    checks.expect(assets.size() == static_cast<std::size_t>(kFootageLayers),
                  "every plate is a layer");
    // A new session already sits on frame 0; setting it again is a no-op.
    static_cast<void>(session.setCurrentTime(core::RationalTime::fromInteger(0)));

    runtime::NodeDefinitionRegistry definitions;
    if (!runtime::registerBuiltInNodeDefinitions(definitions))
        return 2;
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(projectDirectory);
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
    evaluator.operationCache()->bindHostRetirement(retirementOwner);
    std::shared_ptr<runtime::GpuOcioContextResolver> resolver =
        host::makePackagedGpuOcioResolver(QCoreApplication::applicationFilePath().toStdString());
    if (resolver == nullptr)
        resolver = std::make_shared<runtime::GpuOcioContextResolver>();
    const auto deviceWorkingBytes = (512ULL << 20) + serviceOptionsApp.packedTransferByteAllowance;
    auto gpuStage = ui::makeSessionRefreshingGpuSceneStage(
        compiler, evaluator, qualifiedProvider, std::make_shared<runtime::GpuSceneCoverageCache>(),
        planCache, resolver,
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
    window->resize(1700, 960);
    window->show();

    const auto shutdown = [&] {
        ram.beginShutdown();
        background.beginShutdown();
        controller.beginShutdown();
        memoryController.beginShutdown();
        bridge.beginShutdown();
        service.beginShutdown();
        const bool stopped = bloom::test_support::awaitQtCondition(
            "scheduler quiescent and GPU service stopped", [&] {
                return scheduler.isQuiescent() &&
                       service.status().state == runtime::GpuPreviewDisplayServiceState::Stopped;
            });
        if (auto* viewer = window->displayedViewer())
            viewer->releasePackedFramesForShutdown();
        window->completeShutdown();
        window.reset();
        controller.releasePublishedFramesForShutdown();
        frameCache->clear();
        evaluator.operationCache()->close();
        retirementOwner->beginShutdown();
        const bool retired =
            bloom::test_support::awaitQtCondition("packed retirement owner acknowledges exit", [&] {
                return retirementOwner->exitAcknowledged();
            });
        const bool collected = retired && bloom::test_support::awaitQtCondition(
                                              "packed retirement owner collected",
                                              [&] { return retirementOwner->collect(); });
        checks.expect(stopped && retired && collected, "the soak shuts down cleanly");
    };

    if (!bloom::test_support::awaitQtCondition(
            "GPU preview service leaves Initializing",
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
    // #452: a CPU fallback before the service is ready is counted as GpuUnavailable and is not part
    // of the measured window changes below. Since #542 a frame requested while the service is
    // still initializing waits for the startup instead of taking the CPU path.
    const auto initializing = service.status().counters;
    checks.expect(initializing.cpuFallbacks ==
                      initializing.cpuFallbacksByReason[static_cast<std::size_t>(
                          runtime::GpuPreviewCpuFallbackReason::GpuUnavailable)],
                  "every CPU fallback before the service is ready is counted as GpuUnavailable");
    const auto startupCpuFallbacks = initializing.cpuFallbacks + initializing.gpuAdmissionRefusals;
    if (!bloom::test_support::awaitQtCondition("window has a displayed Viewer", [&] {
            return window->displayedViewer() != nullptr;
        })) {
        shutdown();
        return 1;
    }
    auto* viewer = window->displayedViewer();
    const auto current = [&] {
        return viewer->displayedPreviewState().freshness == ui::FrameFreshness::Current;
    };
    checks.expect(bloom::test_support::awaitQtCondition("parked frame is Current", current, 120s),
                  "the parked frame is painted Current");

    // Rest the pointer over the Viewer, as an artist's hand does while moving the window. A hand
    // never returns to the same pixel, so every rest lands a few pixels from the previous one.
    int rests = 0;
    const auto restPointer = [&] {
        ++rests;
        const QPointF local(viewer->width() * 0.5 + (rests * 7) % 61,
                            viewer->height() * 0.45 + (rests * 5) % 37);
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

    int slowSettles = 0;
    const auto settle = [&] {
        // Quiescent means: the Viewer shows the parked frame Current and no task is running. The
        // checks after a settle compare exact cache contents, which only mean something once the
        // preview has finished, so a slow settle (a compiling machine) is waited out rather than
        // abandoned; it is counted and reported.
        if (!bloom::test_support::awaitQtCondition(
                "Viewer shows the parked frame Current and the scheduler is idle",
                [&] { return current() && scheduler.isQuiescent(); }))
            ++slowSettles;
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    };
    // 2548x1388 on the 1.5x screen is the reported placement. On the 1x screen the reported
    // session's Viewer showed the composition below half its size; this workspace gives the Viewer
    // a larger share of the window, so 1700x960 reproduces that Half selection.
    const QSize firstSize(1700, 960);
    const QSize secondSize(2548, 1388);
    // A move places the window on the target screen at its size there, and waits until the window
    // system has delivered the screen, ratio and size change, as a compositor does after a drag.
    const auto moveTo = [&](QScreen* screen, const QSize size) {
        if (auto* handle = window->windowHandle())
            handle->setScreen(screen);
        window->setGeometry(QRect(screen->geometry().topLeft() + QPoint(6, 46), size));
        if (!bloom::test_support::awaitQtCondition(
                "viewer and window land on the requested screen and size", [&] {
                    return window->screen() == screen && window->size() == size &&
                           viewer->devicePixelRatioF() == screen->devicePixelRatio();
                }))
            std::cout << "SOAK move did not land: screen=" << window->screen()->name().toStdString()
                      << " size=" << window->width() << 'x' << window->height()
                      << " dpr=" << viewer->devicePixelRatioF() << '\n';
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
        const auto images = render::GpuDevice::processImageReclamationStats();
        value.gpuImages = images.liveAllocationBytes + images.retiringAllocationBytes;
        const auto counters = service.status().counters;
        value.preparations = counters.residentGraphJobs + counters.cpuFallbacks +
                             frameCache->statistics().insertions;
        value.viewerCpuFallbacks = counters.cpuFallbacks + counters.gpuAdmissionRefusals;
        value.widgets = static_cast<std::size_t>(QApplication::allWidgets().size());
        value.windows = static_cast<std::size_t>(QGuiApplication::allWindows().size());
        for (auto* editor : window->findChildren<ui::ViewerEditor*>()) {
            if (editor->gpuPresenterStateForTest() == ui::ViewerGpuPresenter::State::Retained)
                ++value.retainedPresenters;
        }
        value.quarantined = runtime::GpuPresentationCoordinator::quarantinedGenerationCount();
        return value;
    };

    // One cycle: a move onto the 1.5x screen (Auto flips Half to Full) and back (Full to Half),
    // the reported round trip, then resizes and maximize/restore on the 1x screen and a hide/show
    // (an unmap and remap, as a virtual-desktop switch can be). The pointer rests over the Viewer
    // after every step.
    const std::vector<QSize> sizes{{1640, 940}, {1600, 920}, {1700, 960}};
    window->showNormal();
    moveTo(first, firstSize);
    restPointer();
    settle();
    const auto divisorOnFirst = controller.resolutionDivisor();
    std::cout << "SOAK first screen dpr=" << viewer->devicePixelRatioF()
              << " viewer=" << viewer->width() << 'x' << viewer->height()
              << " divisor=" << divisorOnFirst << '\n';
    std::vector<Sample> samples;
    std::uint32_t divisorOnSecond = 0;
    int moves = 0;
    auto lastAccess = evaluator.operationCache()->statistics();
    auto lastOperationBytes =
        evaluator.operationCache()->retainedBytes(runtime::OperationCacheEntryKind::Operation);
    for (int cycle = 0; cycle < kWarmupCycles + measuredCycles; ++cycle) {
        settle();
        const auto cycleStarted = std::chrono::steady_clock::now();
        const auto start = currentRssBytes();
        sampledPeak = start;
        resetKernelPeakRss();
        // Per monitor change: anonymous RSS against the ledger's reusable host bytes and each
        // pool, and the operation cache's lookups, so a move that adds entries is visible.
        const auto reportMove = [&](const char* destination) {
            ++moves;
            const auto holding = runtime::processMemoryBudgetLedger().reusableHolding();
            const auto& operations = *evaluator.operationCache();
            const auto access = operations.statistics();
            const auto mib = [](const std::uint64_t bytes) {
                return static_cast<double>(bytes) / static_cast<double>(kMiB);
            };
            const auto operationBytes =
                operations.retainedBytes(runtime::OperationCacheEntryKind::Operation);
            std::cout << "SOAK move=" << moves << " cycle=" << cycle << " to=" << destination
                      << " divisor=" << controller.resolutionDivisor()
                      << " anonMiB=" << mib(anonymousRssBytes())
                      << " ledgerHostMiB=" << mib(holding.hostBytes)
                      << " ledgerPendingMiB=" << mib(holding.hostPendingRetirementBytes)
                      << " ledgerDeviceMiB=" << mib(holding.deviceBytes)
                      << " operationEntriesMiB=" << mib(operationBytes) << " decodedMiB="
                      << mib(operations.retainedBytes(
                             runtime::OperationCacheEntryKind::DecodedMedia))
                      << " previewMiB=" << mib(frameCache->residentBytes())
                      << " cacheHits=" << access.hits - lastAccess.hits
                      << " cacheMisses=" << access.misses - lastAccess.misses << '\n';
            if (cycle >= kWarmupCycles)
                checks.expect(
                    operationBytes == lastOperationBytes,
                    "a monitor change after the first round trip adds no operation entry");
            lastAccess = access;
            lastOperationBytes = operationBytes;
        };
        moveTo(second, secondSize);
        restPointer();
        settle();
        reportMove("1.5x");
        checks.expect(viewer->devicePixelRatioF() == 1.5,
                      "the second screen scales the Viewer to 1.5x");
        divisorOnSecond = controller.resolutionDivisor();
        if (cycle == 0)
            std::cout << "SOAK second screen dpr=" << viewer->devicePixelRatioF()
                      << " viewer=" << viewer->width() << 'x' << viewer->height()
                      << " divisor=" << divisorOnSecond << '\n';
        moveTo(first, firstSize);
        restPointer();
        settle();
        if (viewer->devicePixelRatioF() != 1.0 || controller.resolutionDivisor() != divisorOnFirst)
            std::cout << "SOAK return dpr=" << viewer->devicePixelRatioF()
                      << " divisor=" << controller.resolutionDivisor()
                      << " viewer=" << viewer->width() << 'x' << viewer->height() << '\n';
        checks.expect(viewer->devicePixelRatioF() == 1.0 &&
                          controller.resolutionDivisor() == divisorOnFirst,
                      "the first screen returns the Viewer to 1x and its divisor");
        reportMove("1x");
        for (const auto& size : sizes) {
            window->resize(size);
            restPointer();
            settle();
        }
        window->showMaximized();
        restPointer();
        settle();
        window->showNormal();
        moveTo(first, firstSize);
        restPointer();
        settle();
        window->hide();
        // A hidden Viewer paints nothing, so only the scheduler's quiescence is awaited here.
        if (!bloom::test_support::awaitQtCondition("scheduler quiescent while the window is hidden",
                                                   [&] { return scheduler.isQuiescent(); }))
            ++slowSettles;
        window->show();
        restPointer();
        settle();
        const auto value = sample(start);
        std::cout << describe(cycle, value) << " divisorFirst=" << divisorOnFirst
                  << " divisorSecond=" << divisorOnSecond << " slowSettles=" << slowSettles
                  << " seconds="
                  << std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - cycleStarted)
                         .count()
                  << (cycle < kWarmupCycles ? " (warm-up)" : "") << '\n';
        if (cycle >= kWarmupCycles)
            samples.push_back(value);
    }
    checks.expect(divisorOnFirst == 2 && divisorOnSecond == 1,
                  "Auto previews Half on the 1x screen and Full on the 1.5x screen");
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
    // The Viewer frame, at either divisor, is prepared on the production GPU route. Only the pixel
    // probe's one-pixel analysis evaluates on the CPU, and it is not a Viewer frame.
    checks.expect(!gpu || lastSample.viewerCpuFallbacks == startupCpuFallbacks,
                  "no Viewer frame takes the CPU fallback or an admission refusal");
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
    if (checks.failures() + bloom::test_support::awaitFailureCount() != 0) {
        std::cerr << "window surface soak: " << checks.failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: window surface memory soak (" << measuredCycles << " measured cycles)\n";
    return 0;
}
