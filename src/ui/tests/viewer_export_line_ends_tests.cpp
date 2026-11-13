// #599: a Line's ends travel the keyed and driven routes an artist actually uses. A Line Shape
// Source has its `lineEnd` keyed and its `lineStart` driven by a keyed Vector2 value node; the real
// Viewer preview path shows it at several times, a RAM preview fills the range cold at Half and
// replays it warm with no new work, and the real export route renders the same frames as a still, a
// PNG range and a movie. Each Viewer frame equals the exported frame (RGB within one code, exact
// alpha), the picture moves with the keys, and the key that holds holds.
//
// The CPU reference half always runs (CPU preview pipeline against the CPU export provider). The
// GPU half runs the GPU-prepared packed preview product against the GPU export provider when
// --loader names a Vulkan loader and a qualified device exists, and requires every exported frame
// to have been evaluated on the device; otherwise it prints NotRun explicitly, and --require-device
// turns that into a failure. A movie needs the media worker: where it is absent the test says
// NotRun for that route.

#include "await_qt.hpp"
#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
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

#if defined(BLOOM_UI_LINE_END_ROUTES_GPU)
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

// One authored composition shared by the Viewer session and the export route: a Line Shape Source
// whose `lineEnd` is keyed at 0 and 1 seconds (a horizontal line to a vertical one; the second key
// holds up to the end) and whose `lineStart` a Vector2 value node keyed the other way drives.
struct Authored final {
    std::unique_ptr<scripting::Session> scripting;
    std::unique_ptr<ui::CompositionSession> session;
    document::CompositionId composition;
};

template <typename Operation, typename... Arguments>
commands::CommandResult edit(Authored& authored, Arguments&&... arguments) {
    commands::Transaction transaction("Line ends", authored.scripting->snapshot().revision());
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

[[nodiscard]] std::optional<Authored> author(Expectations& checks) {
    const auto format = document::CompositionFormat::create(kWidth, kHeight);
    checks.expect(format.has_value(), "the composition format is valid");
    if (!format.has_value()) {
        return std::nullopt;
    }
    auto created = scripting::Session::createNew("Line end routes", "Main",
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

    commands::ShapeLayerGeometry geometry;
    geometry.position = document::Vec2d{kWidth / 2.0, kHeight / 2.0};
    geometry.lineStart = document::Vec2d{-20, 0};
    geometry.lineEnd = document::Vec2d{20, 0};
    commands::ShapeLayerStyle style;
    style.strokeColor = core::Color4d{0.9, 0.5, 0.1, 1.0};
    style.strokeWidth = 4.0;
    const auto shape = edit<commands::AddShapeLayer>(authored, authored.composition,
                                                     document::ShapeKind::Line, geometry, style)
                           .outputId<document::NodeId>("shapeNode");
    const auto added = edit<commands::AddNode>(authored, authored.composition,
                                               std::string(document::kVector2ValueNodeType),
                                               document::Vec2d{0, 0});
    const auto producer = added.outputId<document::NodeId>(commands::kAddNodeOutput);
    checks.expect(producer.has_value() && shape.has_value(),
                  "the Line and the Vector2 node are authored");
    if (!producer.has_value() || !shape.has_value()) {
        return std::nullopt;
    }
    checks.expect(edit<commands::ConnectPorts>(
                      authored, authored.composition,
                      document::OutputPortRef{*producer, std::string(document::kValuePortName)},
                      document::NodeInputRef{*shape, "lineStart"})
                      .succeeded(),
                  "the Vector2 output connects to the Line's lineStart");
    const auto driver = parameterOf(authored, *producer, document::kValueParameterRole);
    const auto end = parameterOf(authored, *shape, "lineEnd");
    checks.expect(driver.has_value() && end.has_value(), "the keyed parameters exist");
    if (!driver.has_value() || !end.has_value()) {
        return std::nullopt;
    }
    const auto key = [&](const document::ParameterId id, const std::int64_t second,
                         const document::Vec2d value) {
        checks.expect(edit<commands::SetKeyframeAtTimeForParameter>(
                          authored, authored.composition, id,
                          core::RationalTime::fromInteger(second), value)
                          .succeeded(),
                      "a Line end takes a key");
    };
    // lineEnd: horizontal to the right, then up; lineStart (driven): the left, then down-left.
    checks.expect(edit<commands::CreateAnimationForParameter>(authored, authored.composition,
                                                              *end, core::RationalTime{})
                      .succeeded(),
                  "lineEnd is animated");
    key(*end, 1, document::Vec2d{0, -18});
    checks.expect(edit<commands::CreateAnimationForParameter>(authored, authored.composition,
                                                              *driver, core::RationalTime{})
                      .succeeded(),
                  "the driver is animated");
    key(*driver, 0, document::Vec2d{-20, 0});
    key(*driver, 1, document::Vec2d{-8, 18});
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
    auto authored = author(checks);
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

#if defined(BLOOM_UI_LINE_END_ROUTES_GPU)
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
        const bool initialized = bloom::test_support::awaitQtCondition(
            "the GPU preview service leaves Initializing",
            [&] {
                return service->status().state !=
                       runtime::GpuPreviewDisplayServiceState::Initializing;
            },
            90s);
        if (!initialized ||
            service->status().state != runtime::GpuPreviewDisplayServiceState::Ready) {
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
        const bool ready = bloom::test_support::awaitQtCondition(
            "Viewer displays a current frame from the expected route", [&] {
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

    // The picture moves with the keys, and the frame past the last key holds it.
    checks.expect(shown[1].rgba != shown[0].rgba && shown[1].rgba != shown[2].rgba &&
                      shown[2].rgba != shown[0].rgba,
                  prefix + "the keyed and driven ends move the line in the Viewer");
    checks.expect(shown[3].rgba == shown[2].rgba,
                  prefix + "the last key holds the line");
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
    checks.expect(bloom::test_support::awaitQtCondition(
                      "Viewer shows the line at Half from the expected route",
                      [&] {
                          const auto displayed = viewer.displayedFrameForTest();
                          return controller.state().activity == ui::PreviewActivity::Ready &&
                                 displayed != nullptr &&
                                 displayed->provenance().provider == expectedProvider &&
                                 pixelsOf(displayed).width == kWidth / 2;
                      }),
                  prefix + "the Viewer shows the line at Half from the expected route");
    ui::RamPreviewController ram(session, controller, rig.scheduler, *rig.bridge, pipeline, nullptr,
                                 submitter);
    const auto beforeCold = counters();
    ram.start();
    const bool coldDone = bloom::test_support::awaitQtCondition(
        "cold RAM preview at Half finishes caching", [&] { return !ram.isCaching(); }, 180s);
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
    const bool warmDone = bloom::test_support::awaitQtCondition(
        "warm RAM preview finishes caching", [&] { return !ram.isCaching(); }, 60s);
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
#if defined(BLOOM_UI_LINE_END_ROUTES_GPU)
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
                      prefix + "the path moves from frame to frame in the range");
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
    checks.expect(provider->shutdownAndWait(10s), prefix + "the export provider retires");

    controller.beginShutdown();
    rig.bridge->beginShutdown();
    checks.expect(
        bloom::test_support::awaitQtCondition("the rig scheduler reaches quiescence",
                                              [&] { return rig.scheduler.isQuiescent(); }),
        "the rig scheduler reaches quiescence");
#if defined(BLOOM_UI_LINE_END_ROUTES_GPU)
    if (service != nullptr) {
        service->beginShutdown();
        checks.expect(bloom::test_support::awaitQtCondition(
                          "the GPU preview service stops",
                          [&] {
                              return service->status().state ==
                                     runtime::GpuPreviewDisplayServiceState::Stopped;
                          }),
                      "the GPU preview service stops");
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
    QCoreApplication::setOrganizationName("BloomLineEndRoutes");
    QCoreApplication::setApplicationName("ViewerExportLineEnds");
    QSettings().setValue("viewer/resolution", "Full");

    Expectations checks;
    const std::filesystem::path workDir = temporary.path().toStdString();
    exerciseRoute(checks, {"cpu", false}, workDir, loader);

#if defined(BLOOM_UI_LINE_END_ROUTES_GPU)
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

    if (checks.failures() + bloom::test_support::awaitFailureCount() != 0) {
        std::cerr << "viewer/export line ends: " << checks.failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: viewer/export line ends\n";
    return 0;
}
