// #608: OCIO File Transform (a LUT) on the routes an artist uses. A solid whose color is keyed goes
// through a File Transform holding a 2-point 3D `.cube` that rotates the channels. The real Viewer
// preview path shows it at several times, a RAM preview fills the range cold at Half and replays
// it warm with no new work, and the real export route renders the same frames as a still, a PNG
// range and a movie. Each Viewer frame equals the exported frame (RGB within one code, exact
// alpha), and the LUT is seen to act: the channels come out rotated, and with the transform
// bypassed or its LUT cleared the picture is the plain solid again.
//
// The CPU reference half always runs (CPU preview pipeline against the CPU export provider). The
// GPU half runs the GPU-prepared packed preview product against the GPU export provider when
// --loader names a Vulkan loader and a qualified device exists. It requires every Viewer frame to
// come from the device with no CPU fallback, every exported frame to have been evaluated on the
// device with no CPU reference frame, and each Viewer frame to match the CPU route's; otherwise it
// prints NotRun explicitly, and --require-device turns that into a failure. A movie needs the
// media worker: where it is absent the test says NotRun for that route.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>
#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#if defined(BLOOM_UI_LUT_ROUTES_GPU)
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/ui/gpu_viewer_bootstrap.hpp>
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using namespace std::chrono_literals;

class Expectations final {
  public:
    void expect(const bool ok, const std::string& message) {
        if (ok) {
            return;
        }
        ++failures_;
        std::cerr << "FAIL: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

bool gRequireDevice = false;
// The CPU route's Viewer frames, kept for the GPU route to be compared to.
std::array<std::vector<std::uint8_t>, 4> gCpuViewer;

// The preview service counters this test reads (zero on the CPU route, which has no service).
struct RouteCounters final {
    std::uint64_t nativeDispatches = 0;
    std::uint64_t cpuFallbacks = 0;
};

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 48;
constexpr std::uint64_t kFps = 24;
// The session's frames: two seconds.
constexpr std::size_t kSessionFrames = 2 * kFps;

template <typename Predicate>
bool waitUntil(Predicate predicate, const std::chrono::milliseconds limit = 30s) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < limit.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

struct Pixels final {
    bool ok = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; // Straight RGBA8, row-major.

    [[nodiscard]] const std::uint8_t* at(const std::uint32_t x, const std::uint32_t y) const {
        return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4U;
    }
};

[[nodiscard]] Pixels pixelsOf(const ui::PreparedPreviewFrameHandle& frame) {
    Pixels out;
    const auto view = frame != nullptr ? frame->displayBufferView() : std::nullopt;
    if (!view.has_value() ||
        view->layout.rowStrideBytes !=
            static_cast<std::size_t>(view->displayWindow.extent().width()) * 4U) {
        return out;
    }
    out.width = view->displayWindow.extent().width();
    out.height = view->displayWindow.extent().height();
    if (view->pixels.size() != static_cast<std::size_t>(out.width) * out.height) {
        return out;
    }
    out.rgba.reserve(view->pixels.size() * 4U);
    for (const auto& pixel : view->pixels) {
        out.rgba.insert(out.rgba.end(), {pixel.red, pixel.green, pixel.blue, pixel.alpha});
    }
    out.ok = true;
    return out;
}

[[nodiscard]] Pixels decodePng(const std::filesystem::path& path) {
    Pixels out;
    QImage image(QString::fromStdString(path.string()));
    if (image.isNull()) {
        return out;
    }
    // Straight-to-straight: no premultiplication round trip.
    image = image.convertToFormat(QImage::Format_RGBA8888);
    out.width = static_cast<std::uint32_t>(image.width());
    out.height = static_cast<std::uint32_t>(image.height());
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4U);
    for (int y = 0; y < image.height(); ++y) {
        std::copy_n(image.constScanLine(y), static_cast<std::size_t>(image.width()) * 4U,
                    out.rgba.begin() + static_cast<std::ptrdiff_t>(y) * image.width() * 4);
    }
    out.ok = true;
    return out;
}

// RGB within one 8-bit code, alpha exactly.
[[nodiscard]] bool framesMatch(const Pixels& a, const Pixels& b) {
    if (!a.ok || !b.ok || a.width != b.width || a.height != b.height) {
        return false;
    }
    for (std::size_t index = 0; index < a.rgba.size(); ++index) {
        const int difference =
            std::abs(static_cast<int>(a.rgba[index]) - static_cast<int>(b.rgba[index]));
        if (index % 4U == 3U ? difference != 0 : difference > 1) {
            return false;
        }
    }
    return true;
}

struct Route final {
    const char* name;
    bool gpu;
};

// One authored composition shared by the Viewer session and the export route: a solid layer whose
// color is keyed at 0 and 1 seconds (the second key holds up to the end), with an OCIO File
// Transform between the solid and its layer. The LUT rotates the channels: output red is input
// green, output green is input blue, output blue is input red.
struct Authored final {
    std::unique_ptr<scripting::Session> scripting;
    std::unique_ptr<ui::CompositionSession> session;
    document::CompositionId composition;
    document::NodeId transform;
    document::AssetId lut;
};

template <typename Operation, typename... Arguments>
commands::CommandResult edit(Authored& authored, Arguments&&... arguments) {
    commands::Transaction transaction("LUT routes", authored.scripting->snapshot().revision());
    transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
    return authored.scripting->commandStack()->execute(std::move(transaction));
}

[[nodiscard]] std::optional<document::ParameterId>
parameterOf(const Authored& authored, const document::NodeId node, const std::string_view role) {
    const auto* main =
        authored.scripting->snapshot().project().findComposition(authored.composition);
    for (const auto& binding : main->graph().findNode(node)->parameters)
        if (binding.role == role)
            return binding.parameterId;
    return std::nullopt;
}

[[nodiscard]] std::optional<Authored> author(Expectations& checks,
                                             const std::filesystem::path& workDir) {
    const auto format = document::CompositionFormat::create(kWidth, kHeight);
    checks.expect(format.has_value(), "the composition format is valid");
    if (!format.has_value()) {
        return std::nullopt;
    }
    auto created = scripting::Session::createNew("LUT routes", "Main",
                                                 core::RationalTime::fromInteger(2), *format);
    checks.expect(static_cast<bool>(created), "the authoring session is created");
    if (!created) {
        return std::nullopt;
    }
    Authored authored;
    authored.scripting = std::move(created).takeSession();
    authored.composition = authored.scripting->snapshot().project().compositions().front().id();
    authored.session = std::make_unique<ui::CompositionSession>(
        *authored.scripting->document(), *authored.scripting->commandStack(), authored.composition);

    // The LUT: red is the fastest index, and each entry is its input with the channels rotated.
    const auto cube = workDir / "rotate.cube";
    {
        std::ofstream out(cube);
        out << "LUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r)
                    out << g << ' ' << b << ' ' << r << '\n';
    }
    {
        auto import = std::make_unique<commands::ImportAssets>(
            std::vector<std::filesystem::path>{cube}, workDir);
        commands::Transaction transaction("Import LUT", authored.scripting->snapshot().revision());
        checks.expect(transaction.add(std::move(import)), "the LUT import is prepared");
        checks.expect(authored.scripting->commandStack()->execute(std::move(transaction)).changed(),
                      "the LUT is imported");
    }
    const auto assets = authored.scripting->snapshot().project().assets();
    checks.expect(assets.size() == 1 && assets.front().kind == document::AssetKind::Lut,
                  "the project holds the LUT asset");
    if (assets.size() != 1) {
        return std::nullopt;
    }
    authored.lut = assets.front().id;

    checks.expect(edit<commands::AddSolidLayer>(authored, authored.composition,
                                                std::string("Solid"),
                                                core::Color4d{0.8, 0.3, 0.1, 1.0})
                      .succeeded(),
                  "the solid layer is added");
    // The solid and its layer, through the layer's content edge.
    document::NodeId layer;
    document::NodeId solid;
    const auto* main =
        authored.scripting->snapshot().project().findComposition(authored.composition);
    for (const auto& edge : main->graph().edges()) {
        const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
        const auto* node = input != nullptr ? main->graph().findNode(input->nodeId) : nullptr;
        if (node != nullptr && input->port == document::kLayerOutputContentInputPort &&
            node->typeId == "bloom.layer-output") {
            layer = input->nodeId;
            solid = edge.source.nodeId;
        }
    }
    const auto transform =
        edit<commands::AddNode>(authored, authored.composition,
                                std::string("bloom.ocio-file-transform"), document::Vec2d{0, 0})
            .outputId<document::NodeId>(commands::kAddNodeOutput);
    checks.expect(layer.isValid() && solid.isValid() && transform.has_value(),
                  "the solid, its layer and the File Transform exist");
    if (!layer.isValid() || !solid.isValid() || !transform.has_value()) {
        return std::nullopt;
    }
    authored.transform = *transform;
    checks.expect(edit<commands::ConnectPorts>(
                      authored, authored.composition,
                      document::OutputPortRef{solid, std::string(document::kSolidSourceOutputPort)},
                      document::NodeInputRef{*transform, "input"})
                      .succeeded(),
                  "the solid feeds the File Transform");
    checks.expect(
        edit<commands::ConnectPorts>(
            authored, authored.composition, document::OutputPortRef{*transform, "image"},
            document::NodeInputRef{layer, std::string(document::kLayerOutputContentInputPort)})
            .succeeded(),
        "the File Transform feeds the layer");
    checks.expect(authored.session->setNodeParameterValue(
                      *transform, "lut", static_cast<std::int64_t>(authored.lut.value()),
                      QStringLiteral("Select LUT")),
                  "the File Transform takes the LUT");

    const auto color = parameterOf(authored, solid, document::kSolidColorParameterRole);
    checks.expect(color.has_value(), "the solid's color parameter exists");
    if (!color.has_value()) {
        return std::nullopt;
    }
    checks.expect(edit<commands::CreateAnimationForParameter>(authored, authored.composition,
                                                              *color, core::RationalTime{})
                      .succeeded(),
                  "the color is animated");
    checks.expect(edit<commands::SetKeyframeAtTimeForParameter>(
                      authored, authored.composition, *color, core::RationalTime::fromInteger(1),
                      core::Color4d{0.1, 0.6, 0.9, 1.0})
                      .succeeded(),
                  "the color takes a second key");
    return authored;
}

struct PreviewRig final {
    runtime::TaskScheduler scheduler;
    runtime::NodeDefinitionRegistry definitions;
    std::unique_ptr<runtime::SnapshotCompiler> compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualified;
    std::unique_ptr<ui::TaskUiBridge> bridge;
};

// Exercises one route: the Viewer's process frame against the exported still.
void exerciseRoute(Expectations& checks, const Route& route, const std::filesystem::path& workDir,
                   [[maybe_unused]] const std::filesystem::path& loader) {
    const std::string prefix = std::string(route.name) + ": ";
    auto authored = author(checks, workDir);
    if (!authored.has_value()) {
        return;
    }
    auto& session = *authored->session;

    PreviewRig rig;
    checks.expect(runtime::registerBuiltInNodeDefinitions(rig.definitions),
                  prefix + "node definitions register");
    rig.definitions.freeze();
    rig.compiler = std::make_unique<runtime::SnapshotCompiler>(rig.definitions);
    rig.qualified.publish(runtime::buildBloomNeutralQualifiedDisplayProcessor());
    rig.bridge = std::make_unique<ui::TaskUiBridge>(rig.scheduler, nullptr, 1ms);

    auto pipeline = ui::makeCompositionPreviewPipeline(*rig.compiler, rig.evaluator,
                                                       rig.displayPreparer, rig.qualified);
    ui::CompositionPreviewSettings settings;
    settings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
    ui::PreviewPreparationSubmitter submitter;
    runtime::PreviewDisplayProvider expectedProvider = runtime::PreviewDisplayProvider::CpuOcio;
    std::function<RouteCounters()> counters = [] { return RouteCounters{}; };

#if defined(BLOOM_UI_LUT_ROUTES_GPU)
    std::unique_ptr<runtime::GpuPreviewDisplayService> service;
    if (route.gpu) {
        auto resolver = host::makePackagedGpuOcioResolver(
            QCoreApplication::applicationFilePath().toStdString());
        auto gpuStage = ui::makeSessionRefreshingGpuSceneStage(
            *rig.compiler, rig.evaluator, rig.qualified,
            std::make_shared<runtime::GpuSceneCoverageCache>(), nullptr, resolver);
        runtime::GpuPreviewDisplayServiceOptions options;
        options.enabled = true;
        options.loaderPath = loader;
        options.product = runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
        options.presentation = runtime::GpuPreviewDisplayServicePresentationMode::Disabled;
        options.previewByteAllowance = std::size_t{1} << 28U;
        service = std::make_unique<runtime::GpuPreviewDisplayService>(
            rig.scheduler, gpuStage,
            ui::makeCompositionPreviewCpuStage(*rig.compiler, rig.evaluator, rig.qualified),
            ui::makeCompositionPreviewCpuDisplayFallback(rig.displayPreparer), options);
        waitUntil(
            [&] {
                return service->status().state !=
                       runtime::GpuPreviewDisplayServiceState::Initializing;
            },
            90s);
        if (service->status().state != runtime::GpuPreviewDisplayServiceState::Ready) {
            std::cout << "NotRun: " << route.name << " preview: no qualified device ("
                      << service->status().residentDetail << ")\n";
            checks.expect(!gRequireDevice, prefix + "--require-device needs a GPU device");
            service->beginShutdown();
            return;
        }
        submitter = [svc = service.get()](
                        runtime::TaskRequest request, const document::Snapshot& snapshot,
                        const runtime::PreviewRequestIdentity& identity, const std::size_t bytes,
                        const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            return svc->submit(std::move(request), snapshot, identity, bytes, overrides);
        };
        settings.pixelStorageByteLimit = std::size_t{1} << 28U;
        counters = [svc = service.get()] {
            const auto status = svc->status().counters;
            return RouteCounters{status.nativeDispatches, status.cpuFallbacks};
        };
        expectedProvider = runtime::PreviewDisplayProvider::GpuPreparedPacked;
    }
#endif

    ui::CompositionPreviewController controller(session, rig.scheduler, *rig.bridge, pipeline,
                                                settings, nullptr, nullptr, submitter);
    ui::ViewerEditor viewer(session, controller);
    viewer.resize(480, 320);
    viewer.show();
    // The Viewer at each tested frame, in the order an artist might scrub them.
    constexpr std::array<std::uint64_t, 4> kFrames{0, kFps / 2, kFps, kFps * 3 / 2};
    std::array<Pixels, kFrames.size()> shown;
    for (std::size_t index = 0; index < kFrames.size(); ++index) {
        const auto frame = kFrames.at(index);
        const auto label = prefix + "frame " + std::to_string(frame) + ": ";
        // The session starts at the first of them, so only the later ones move the playhead.
        checks.expect(session.setCurrentTime(*core::RationalTime::create(
                          static_cast<std::int64_t>(frame), static_cast<std::int64_t>(kFps))) ||
                          index == 0,
                      label + "the playhead moves");
        controller.requestRefresh();
        const auto before =
            index == 0 ? ui::PreparedPreviewFrameHandle{} : viewer.displayedFrameForTest();
        const bool ready = waitUntil([&] {
            const auto displayed = viewer.displayedFrameForTest();
            return controller.state().activity == ui::PreviewActivity::Ready &&
                   displayed != nullptr && (index == 0 || displayed != before || index == 3) &&
                   displayed->provenance().provider == expectedProvider &&
                   viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current;
        });
        checks.expect(ready, label + "the Viewer displays a current frame from the expected route");
        shown.at(index) = pixelsOf(viewer.displayedFrameForTest());
        checks.expect(shown.at(index).ok && shown.at(index).width == kWidth &&
                          shown.at(index).height == kHeight,
                      label + "the Viewer process frame is readable at the composition size");
        if (!ready || !shown.at(index).ok) {
            return;
        }
    }

    checks.expect(counters().cpuFallbacks == 0, prefix + "the Viewer frames took no CPU fallback");
    checks.expect(!route.gpu || counters().nativeDispatches > 0,
                  prefix + "the Viewer frames ran on the device");
    // The picture moves with the keys, and the frame past the last key holds it.
    checks.expect(shown[1].rgba != shown[0].rgba && shown[1].rgba != shown[2].rgba &&
                      shown[2].rgba != shown[0].rgba,
                  prefix + "the keyed color changes the picture in the Viewer");
    checks.expect(shown[3].rgba == shown[2].rgba, prefix + "the last key holds the picture");
    // CPU against GPU: the CPU route runs first and keeps its frames.
    for (std::size_t index = 0; index < kFrames.size(); ++index) {
        if (!route.gpu) {
            gCpuViewer.at(index) = shown.at(index).rgba;
        } else if (!gCpuViewer.at(index).empty()) {
            Pixels cpu = shown.at(index);
            cpu.rgba = gCpuViewer.at(index);
            checks.expect(framesMatch(cpu, shown.at(index)),
                          prefix + "Viewer frame " + std::to_string(kFrames.at(index)) +
                              " matches the CPU route (RGB within one code, exact alpha)");
        }
    }

    // RAM preview at Half: cold fills every frame, warm prepares nothing and dispatches nothing.
    controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Half);
    checks.expect(waitUntil([&] {
                      const auto displayed = viewer.displayedFrameForTest();
                      return controller.state().activity == ui::PreviewActivity::Ready &&
                             displayed != nullptr &&
                             displayed->provenance().provider == expectedProvider &&
                             pixelsOf(displayed).width == kWidth / 2;
                  }),
                  prefix + "the Viewer shows the picture at Half from the expected route");
    ui::RamPreviewController ram(session, controller, rig.scheduler, *rig.bridge, pipeline, nullptr,
                                 submitter);
    const auto beforeCold = counters();
    ram.start();
    const bool coldDone = waitUntil([&] { return !ram.isCaching(); }, 180s);
    const auto afterCold = counters();
    checks.expect(coldDone && ram.cachedFrameCount() == kSessionFrames &&
                      ram.lastCompletion() == ui::RamPreviewController::Completion::Complete,
                  prefix + "a cold RAM preview at Half retains every frame (" +
                      std::to_string(ram.cachedFrameCount()) + " of " +
                      std::to_string(kSessionFrames) + ")");
    checks.expect(afterCold.cpuFallbacks == beforeCold.cpuFallbacks,
                  prefix + "the cold RAM preview took no CPU fallback");
    checks.expect(!route.gpu || afterCold.nativeDispatches > beforeCold.nativeDispatches,
                  prefix + "the cold RAM preview ran on the device");
    ram.start();
    const bool warmDone = waitUntil([&] { return !ram.isCaching(); }, 60s);
    const auto afterWarm = counters();
    checks.expect(warmDone && ram.preparedFrameCount() == 0 &&
                      ram.cachedFrameCount() == kSessionFrames,
                  prefix + "a warm RAM preview prepares nothing");
    checks.expect(afterWarm.nativeDispatches == afterCold.nativeDispatches &&
                      afterWarm.cpuFallbacks == afterCold.cpuFallbacks,
                  prefix + "a warm RAM preview dispatches nothing");
    ram.beginShutdown();
    // Back to full size for the export comparison.
    controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Full);

    // The export route, rendered from the same document.
    runtime::TaskScheduler exportScheduler;
    const runtime::SnapshotCompiler exportCompiler(document::builtInNodeDefinitions());
    std::shared_ptr<host::GpuExportProvider> provider;
#if defined(BLOOM_UI_LUT_ROUTES_GPU)
    if (route.gpu) {
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.enabled = true;
        options.loaderPath = loader;
        options.ocioResolver = host::makePackagedGpuOcioResolver(host::currentExecutablePath());
        provider = host::GpuExportProvider::create(options);
    }
#endif
    if (!route.gpu) {
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.enabled = false;
        provider = host::GpuExportProvider::create(options);
    }
    provider->prepare(exportScheduler);
    const auto expectProvenance = [&](const scripting::RenderResult& result,
                                      const std::uint64_t frames, const std::uint64_t artifacts,
                                      const std::string& label) {
        checks.expect(result.succeeded, label + " publishes: " + result.diagnostic);
        checks.expect(result.publishedFrames == artifacts, label + " publishes every artifact");
        checks.expect(result.gpuEvaluatedFrames == (route.gpu ? frames : 0),
                      route.gpu ? label + " was genuinely evaluated on the device"
                                : label + " on the CPU reference never touched a device");
        checks.expect(!route.gpu || result.cpuReferenceFrames == 0,
                      label + " took no CPU reference frame on the GPU route");
    };

    // Still export, one still per tested frame.
    for (std::size_t index = 0; index < kFrames.size(); ++index) {
        const auto label = prefix + "still " + std::to_string(kFrames.at(index));
        const auto still =
            workDir / (std::string(route.name) + "-still-" + std::to_string(index) + ".png");
        const auto result =
            scripting::Render::run(*authored->scripting, exportScheduler, exportCompiler,
                                   {.composition = authored->composition,
                                    .frame = kFrames.at(index),
                                    .range = std::nullopt,
                                    .preset = output::OutputPresetV1::PngRgba8SrgbV1,
                                    .destination = still},
                                   {}, provider);
        expectProvenance(result, 1, 1, label);
        const Pixels exported = decodePng(still);
        checks.expect(framesMatch(shown.at(index), exported),
                      label + " matches the Viewer (RGB within one code, exact alpha)");
    }

    // A sequence over the frames the first two keys interpolate, with their exports in order.
    const auto folder = workDir / (std::string(route.name) + "-range");
    std::filesystem::create_directories(folder);
    const auto ranged = scripting::Render::run(
        *authored->scripting, exportScheduler, exportCompiler,
        {.composition = authored->composition,
         .frame = std::nullopt,
         .range = std::pair<std::uint64_t, std::uint64_t>{kFps / 2 - 1, kFps / 2 + 1},
         .preset = output::OutputPresetV1::PngRgba8SrgbV1,
         .destination = folder / "range.png"},
        {}, provider);
    expectProvenance(ranged, 3, 3, prefix + "range");
    std::vector<std::filesystem::path> members;
    if (std::filesystem::exists(folder)) {
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (entry.path().extension() == ".png") {
                members.push_back(entry.path());
            }
        }
    }
    std::ranges::sort(members);
    checks.expect(members.size() == 3, prefix + "the range writes one file per frame, not " +
                                           std::to_string(members.size()));
    if (members.size() == 3) {
        const auto middle = decodePng(members[1]);
        checks.expect(framesMatch(shown[1], middle),
                      prefix + "the range's middle frame matches the Viewer's");
        checks.expect(decodePng(members[0]).rgba != middle.rgba,
                      prefix + "the picture changes from frame to frame in the range");
    }

    // A movie, where the media worker exists.
    if (output::mediaWorkerAvailableV1()) {
        const auto movie = workDir / (std::string(route.name) + "-movie.mov");
        const auto encoded =
            scripting::Render::run(*authored->scripting, exportScheduler, exportCompiler,
                                   {.composition = authored->composition,
                                    .frame = std::nullopt,
                                    .range = std::pair<std::uint64_t, std::uint64_t>{0, 3},
                                    .preset = output::OutputPresetV1::ProResMovV1,
                                    .destination = movie},
                                   {}, provider);
        expectProvenance(encoded, 4, 1, prefix + "movie");
        checks.expect(std::filesystem::exists(movie) && std::filesystem::file_size(movie) > 0,
                      prefix + "the movie is written");
    } else {
        std::cout << "NotRun: " << route.name << " movie: no media worker here\n";
    }
    // The LUT is seen to act: the first key's color (0.8, 0.3, 0.1) comes out with its channels
    // rotated, so blue leads and green trails.
    const auto* lutPixel = shown[0].at(kWidth / 2, kHeight / 2);
    checks.expect(lutPixel[2] > lutPixel[0] && lutPixel[0] > lutPixel[1],
                  prefix + "the LUT rotates the channels in the Viewer");
    // The transform's other states, on the same route: bypassed, and with no LUT selected. Both
    // show the plain solid, and both stay on the device on the GPU route.
    checks.expect(session.setCurrentTime(core::RationalTime{}), prefix + "the playhead returns");
    const auto showState = [&](const std::string& label, const std::string& file) -> Pixels {
        const auto before = viewer.displayedFrameForTest();
        controller.requestRefresh();
        const bool ready = waitUntil([&] {
            const auto displayed = viewer.displayedFrameForTest();
            return controller.state().activity == ui::PreviewActivity::Ready &&
                   displayed != nullptr && displayed != before &&
                   displayed->provenance().provider == expectedProvider &&
                   viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current;
        });
        checks.expect(ready,
                      label + ": the Viewer displays a current frame from the expected route");
        const Pixels viewed = pixelsOf(viewer.displayedFrameForTest());
        const auto still = workDir / (std::string(route.name) + "-" + file + ".png");
        const auto result =
            scripting::Render::run(*authored->scripting, exportScheduler, exportCompiler,
                                   {.composition = authored->composition,
                                    .frame = 0,
                                    .range = std::nullopt,
                                    .preset = output::OutputPresetV1::PngRgba8SrgbV1,
                                    .destination = still},
                                   {}, provider);
        expectProvenance(result, 1, 1, label + " still");
        checks.expect(framesMatch(viewed, decodePng(still)),
                      label + ": the still matches the Viewer (RGB within one code, exact alpha)");
        return viewed;
    };
    const auto fallbacksBefore = counters().cpuFallbacks;
    checks.expect(session.setNodeParameterValue(authored->transform, "bypass", true,
                                                QStringLiteral("Bypass")),
                  prefix + "the File Transform is bypassed");
    const Pixels bypassed = showState(prefix + "bypassed", "bypassed");
    if (bypassed.ok) {
        const auto* plain = bypassed.at(kWidth / 2, kHeight / 2);
        checks.expect(plain[0] > plain[1] && plain[1] > plain[2],
                      prefix + "bypassed, the Viewer shows the solid's own color");
    }
    checks.expect(session.setNodeParameterValue(authored->transform, "bypass", false,
                                                QStringLiteral("Bypass")) &&
                      session.setNodeParameterValue(authored->transform, "lut", std::int64_t{0},
                                                    QStringLiteral("Clear LUT")),
                  prefix + "the LUT is cleared");
    const Pixels cleared = showState(prefix + "no LUT", "cleared");
    checks.expect(framesMatch(bypassed, cleared),
                  prefix + "with no LUT the picture is the bypassed one");
    checks.expect(counters().cpuFallbacks == fallbacksBefore,
                  prefix + "the bypassed and cleared states took no CPU fallback");
    checks.expect(provider->shutdownAndWait(10s), prefix + "the export provider retires");

    controller.beginShutdown();
    rig.bridge->beginShutdown();
    (void)waitUntil([&] { return rig.scheduler.isQuiescent(); }, 10s);
#if defined(BLOOM_UI_LUT_ROUTES_GPU)
    if (service != nullptr) {
        service->beginShutdown();
        (void)waitUntil([&] {
            return service->status().state == runtime::GpuPreviewDisplayServiceState::Stopped;
        });
    }
#endif
}

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    std::filesystem::path loader;
    std::vector<char*> forwarded{argv[0]};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            loader = argv[++index];
        } else if (argument == "--require-device") {
            gRequireDevice = true;
        } else {
            forwarded.push_back(argv[index]);
        }
    }
    int forwardedCount = static_cast<int>(forwarded.size());
    QApplication application(forwardedCount, forwarded.data());
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName("BloomLutRoutes");
    QCoreApplication::setApplicationName("ViewerExportLut");
    QSettings().setValue("viewer/resolution", "Full");

    Expectations checks;
    const std::filesystem::path workDir = temporary.path().toStdString();
    exerciseRoute(checks, {"cpu", false}, workDir, loader);

#if defined(BLOOM_UI_LUT_ROUTES_GPU)
    if (loader.empty()) {
        std::cout << "NotRun: gpu route: no Vulkan loader was given (--loader)\n";
        checks.expect(!gRequireDevice, "--require-device needs --loader");
    } else {
        exerciseRoute(checks, {"gpu", true}, workDir, loader);
    }
#else
    std::cout << "NotRun: gpu route: this build has no Vulkan support\n";
    checks.expect(!gRequireDevice, "--require-device needs a Vulkan build");
#endif

    if (checks.failures() != 0) {
        std::cerr << "viewer/export LUT: " << checks.failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: viewer/export LUT\n";
    return 0;
}
