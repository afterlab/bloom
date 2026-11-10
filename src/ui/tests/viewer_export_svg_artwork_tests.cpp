// #343: imported SVG artwork through the routes an artist actually uses. A generated file of 1,200
// elements (stroked curves, polygons, a radial gradient, a clip, nested opacity groups, so more
// than a thousand prepared commands) imports through AddSvgArtwork, and one of its Path nodes is
// keyed so its points move past the first frame's bounds. The real Viewer preview path shows it at
// several times; a RAM preview fills the range cold at Half and replays it warm with no new work;
// the real export route renders the same frames as a still, a PNG range and a movie. Each Viewer
// frame equals the exported frame (RGB within one code, exact alpha).
//
// The CPU reference half always runs (CPU preview pipeline against the CPU export provider). The
// GPU half runs the GPU-prepared packed preview product against the GPU export provider when
// --loader names a Vulkan loader and a qualified device exists, and requires every Viewer, RAM and
// exported frame to come from the device with no CPU fallback; otherwise it prints NotRun, and
// --require-device turns that into a failure. A movie needs the media worker: where it is absent
// the test says NotRun for that route.
//
// #642: the same routes again with the file imported as one layer (a Vector Artwork layer the
// compiler expands), whose position, scale, rotation and opacity are keyed instead of a Path node:
// Viewer frames from the expected route matching the CPU route's, a cold and a warm RAM preview,
// a still, a range and a movie with their provenance, and on the GPU route a 1920 x 1080 and a
// 3840 x 2160 still exported from the device with no CPU reference frame.
//
// `--svg <file> --size <W>x<H>` (with --loader) instead shows a real file's first frame in the
// Viewer at that size, through the GPU-prepared packed product wired as the application wires it,
// and requires the frame from the device with no CPU fallback. Not part of the ctest run (it needs
// the file); docs/architecture/vector-artwork.md records the results.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#if defined(BLOOM_UI_SVG_ARTWORK_ROUTES_GPU)
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/ui/gpu_viewer_bootstrap.hpp>
#include <bloom/ui/memory_budget_controller.hpp>
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
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
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

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 180;
constexpr std::uint64_t kFps = 24;
// Half a second: the range a RAM preview fills.
constexpr std::uint64_t kFrames = kFps / 2;

template <typename Predicate>
bool waitUntil(Predicate predicate, const std::chrono::milliseconds limit = 60s) {
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

// A deterministic generator, so the file is the same on every machine.
struct Lcg final {
    std::uint64_t state = 343;
    double next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53);
    }
};

// 1,200 elements in nested groups. The first element is the "mover" whose Path node is keyed.
[[nodiscard]] std::string artworkSvg() {
    Lcg random;
    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << kWidth << ' ' << kHeight
        << "\" width=\"" << kWidth << "\" height=\"" << kHeight << "\">\n"
        << "<defs><radialGradient id=\"glow\" cx=\"0.5\" cy=\"0.5\" r=\"0.5\">"
        << "<stop offset=\"0\" stop-color=\"#ffd040\"/><stop offset=\"1\" stop-color=\"#c02060\"/>"
        << "</radialGradient><clipPath id=\"disc\"><circle cx=\"250\" cy=\"60\" r=\"40\"/>"
        << "</clipPath><linearGradient id=\"sweep\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"1\">"
        << "<stop offset=\"0\" stop-color=\"#f0f020\"/><stop offset=\"1\" stop-color=\"#2020f0\"/>"
        << "</linearGradient><clipPath id=\"window\"><rect x=\"110\" y=\"100\" width=\"130\" "
           "height=\"70\"/></clipPath></defs>\n"
        << "<path id=\"mover\" fill=\"#2060c0\" d=\"M20 20 L60 20 L60 60 L20 60 Z\"/>\n"
        << "<ellipse id=\"wash\" cx=\"160\" cy=\"90\" rx=\"300\" ry=\"200\" fill=\"url(#glow)\" "
           "opacity=\"0.4\"/>\n"
        << "<rect id=\"clipped\" x=\"200\" y=\"10\" width=\"120\" height=\"100\" fill=\"#30a050\" "
           "clip-path=\"url(#disc)\"/>\n";
    int element = 3;
    for (int group = 0; element < 1200; ++group) {
        svg << "<g id=\"g" << group << "\" opacity=\"" << (group % 2 == 0 ? "0.9" : "1") << "\">\n";
        for (int index = 0; index < 120 && element < 1200; ++index, ++element) {
            const double x = random.next() * kWidth;
            const double y = random.next() * kHeight;
            const int hue = static_cast<int>(random.next() * 0xffffff);
            svg << std::hex;
            if (element % 3 == 0)
                svg << "<path fill=\"none\" stroke=\"#" << hue << std::dec
                    << "\" stroke-width=\"1.5\" d=\"M" << x << ' ' << y << " C" << x + 20 << ' '
                    << y - 15 << ' ' << x + 30 << ' ' << y + 15 << ' ' << x + 45 << ' ' << y
                    << "\"/>\n";
            else
                svg << "<polygon fill=\"#" << hue << std::dec << "\" points=\"" << x << ',' << y
                    << ' ' << x + 9 << ',' << y + 3 << ' ' << x + 4 << ',' << y + 10 << "\"/>\n";
            svg << std::dec;
        }
        svg << "</g>\n";
    }
    // On top of everything: one compound shape (an outer square with a square counter, even-odd)
    // filled with a gradient, matted by a clip, inside two nested opacity groups. Its counter's
    // corner is keyed, so the compound value is edited, not just drawn.
    svg << "<g id=\"ringOuter\" opacity=\"0.9\"><g id=\"ringInner\" opacity=\"0.8\">"
        << "<path id=\"ring\" fill=\"url(#sweep)\" fill-rule=\"evenodd\" clip-path=\"url(#window)\" "
           "d=\"M130 105 H230 V165 H130 Z M160 120 H200 V150 H160 Z\"/>"
        << "</g></g>\n";
    svg << "</svg>\n";
    return svg.str();
}

// The CPU route's Viewer frames, kept so the GPU route's can be held against them: the same
// document through the CPU reference and the device must show the same picture.
std::array<std::array<std::vector<std::uint8_t>, 3>, 2> gCpuViewer;
std::array<std::array<std::uint32_t, 3>, 2> gCpuViewerWidth{};

struct Route final {
    const char* name;
    bool gpu;
    // #642: the file imported as one layer rather than as editable shapes.
    bool layer = false;
};

// The service counters this test reads (zero on the CPU route, which has no service).
struct RouteCounters final {
    std::uint64_t nativeDispatches = 0;
    std::uint64_t cpuFallbacks = 0;
};

struct Authored final {
    std::unique_ptr<scripting::Session> scripting;
    std::unique_ptr<ui::CompositionSession> session;
    document::CompositionId composition;
};

template <typename Operation, typename... Arguments>
commands::CommandResult edit(Authored& authored, Arguments&&... arguments) {
    commands::Transaction transaction("SVG artwork", authored.scripting->snapshot().revision());
    transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
    return authored.scripting->commandStack()->execute(std::move(transaction));
}

// The layer's own keys (#642): it moves, grows, turns and fades over the range.
[[nodiscard]] bool keyLayer(Authored& authored, const document::LayerId layer) {
    const auto* main = authored.scripting->snapshot().project().findComposition(authored.composition);
    const auto& graph = main->graph();
    const auto* boundary = graph.findLayer(layer);
    if (boundary == nullptr)
        return false;
    const auto id = [&](const std::string_view role) {
        return document::layerParameterBinding(graph, *boundary, role)->parameterId;
    };
    const auto last = *core::RationalTime::create(static_cast<std::int64_t>(kFrames - 1),
                                                  static_cast<std::int64_t>(kFps));
    const auto position = id(document::kPositionParameterRole);
    const auto scale = id(document::kScaleParameterRole);
    const auto rotation = id(document::kRotationParameterRole);
    const auto opacity = id(document::kOpacityParameterRole);
    bool ok = true;
    const auto key = [&](const document::ParameterId parameter, const auto first, const auto end) {
        // The first key holds the value the constant already has, so the last key goes in first.
        ok = ok && edit<commands::SetKeyframeAtTimeForParameter>(authored, authored.composition, parameter,
                                                                 last, end)
                       .succeeded();
        ok = ok && edit<commands::SetKeyframeAtTimeForParameter>(authored, authored.composition, parameter,
                                                                 core::RationalTime{}, first)
                       .succeeded();
    };
    key(position, document::Vec2d{kWidth / 2.0, kHeight / 2.0}, document::Vec2d{kWidth / 2.0 + 40.0, kHeight / 2.0 - 10.0});
    key(scale, document::Vec2d{1.0, 1.0}, document::Vec2d{1.8, 1.6});
    key(rotation, 0.0, 20.0);
    key(opacity, 1.0, 0.7);
    return ok;
}

[[nodiscard]] std::optional<Authored> author(Expectations& checks,
                                             const std::filesystem::path& workDir,
                                             const commands::SvgImportMode mode =
                                                 commands::SvgImportMode::Shapes,
                                             const std::uint32_t width = kWidth,
                                             const std::uint32_t height = kHeight) {
    const auto rate = document::FrameRate::create(static_cast<std::int64_t>(kFps), 1);
    const auto format = document::CompositionFormat::create(
        width, height, core::PixelAspectRatio::square(), *rate);
    checks.expect(format.has_value(), "the composition format is valid");
    if (!format.has_value()) {
        return std::nullopt;
    }
    auto created = scripting::Session::createNew(
        "SVG artwork routes", "Main",
        *core::RationalTime::create(static_cast<std::int64_t>(kFrames),
                                    static_cast<std::int64_t>(kFps)),
        *format);
    checks.expect(static_cast<bool>(created), "the authoring session is created");
    if (!created) {
        return std::nullopt;
    }
    Authored authored;
    authored.scripting = std::move(created).takeSession();
    authored.composition = authored.scripting->snapshot().project().compositions().front().id();
    authored.session = std::make_unique<ui::CompositionSession>(
        *authored.scripting->document(), *authored.scripting->commandStack(), authored.composition);

    const auto file = workDir / "artwork.svg";
    {
        std::ofstream out(file);
        out << artworkSvg();
    }
    const auto imported = edit<commands::AddSvgArtwork>(authored, authored.composition, nullptr,
                                                        file, "artwork", true, mode);
    checks.expect(imported.succeeded(), "the generated artwork imports");
    std::filesystem::remove(file);
    if (!imported.succeeded()) {
        return std::nullopt;
    }
    if (mode == commands::SvgImportMode::Layer) {
        const auto layer = imported.outputId<document::LayerId>("layer");
        const auto* main =
            authored.scripting->snapshot().project().findComposition(authored.composition);
        checks.expect(std::ranges::none_of(main->graph().nodes(),
                                           [](const auto& node) {
                                               return node.typeId == document::kPathValueNodeType;
                                           }),
                      "the artwork imports as one layer (no Path node in the graph)");
        checks.expect(layer.has_value() && keyLayer(authored, *layer),
                      "the layer takes its position, scale, rotation and opacity keys");
        return authored;
    }

    // Key the mover's Path node: at the last frame its square has moved 200 px right and grown,
    // past the bounds it had at frame 0.
    const auto* main =
        authored.scripting->snapshot().project().findComposition(authored.composition);
    std::optional<document::ParameterId> mover;
    std::optional<document::PathValue> start;
    for (const auto& node : main->graph().nodes()) {
        if (node.typeId != document::kPathValueNodeType)
            continue;
        for (const auto& binding : node.parameters) {
            if (binding.role != document::kValueParameterRole)
                continue;
            const auto* record = main->parameters().find(binding.parameterId);
            const auto* constant =
                record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
            const auto* value =
                constant ? std::get_if<document::PathValue>(&constant->value) : nullptr;
            if (value != nullptr && !value->contours.empty() &&
                value->contours[0].anchors.size() == 4 && !mover) {
                mover = binding.parameterId;
                start = *value;
            }
        }
    }
    checks.expect(mover.has_value(), "the mover has its Path node");
    if (!mover.has_value()) {
        return std::nullopt;
    }
    auto moved = *start;
    for (auto& anchor : moved.contours[0].anchors)
        anchor.point.x +=
            200.0 * (anchor.point.x > moved.contours[0].anchors[0].point.x ? 1.2 : 1.0);
    checks.expect(edit<commands::SetKeyframeAtTimeForParameter>(
                      authored, authored.composition, *mover,
                      *core::RationalTime::create(static_cast<std::int64_t>(kFrames - 1),
                                                  static_cast<std::int64_t>(kFps)),
                      moved)
                      .succeeded(),
                  "the mover takes its last-frame key");
    checks.expect(edit<commands::SetKeyframeAtTimeForParameter>(
                      authored, authored.composition, *mover, core::RationalTime{}, *start)
                      .succeeded(),
                  "the mover takes its first-frame key");
    // The compound ring: drag one corner of its counter and one corner of its outline.
    std::optional<document::ParameterId> ring;
    std::optional<document::PathValue> ringStart;
    for (const auto& node : main->graph().nodes()) {
        if (node.typeId != document::kPathValueNodeType)
            continue;
        for (const auto& binding : node.parameters) {
            if (binding.role != document::kValueParameterRole)
                continue;
            const auto* record = main->parameters().find(binding.parameterId);
            const auto* constant =
                record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
            const auto* value =
                constant ? std::get_if<document::PathValue>(&constant->value) : nullptr;
            if (value != nullptr && value->contours.size() == 2 && !ring) {
                ring = binding.parameterId;
                ringStart = *value;
            }
        }
    }
    checks.expect(ring.has_value(), "the compound ring imports as one Path node of two contours");
    if (!ring.has_value()) {
        return std::nullopt;
    }
    auto ringMoved = *ringStart;
    ringMoved.contours[0].anchors[0].point.x -= 15.0;
    ringMoved.contours[0].anchors[0].point.y -= 10.0;
    ringMoved.contours[1].anchors[0].point.x += 12.0;
    ringMoved.contours[1].anchors[0].point.y += 8.0;
    checks.expect(edit<commands::SetKeyframeAtTimeForParameter>(
                      authored, authored.composition, *ring,
                      *core::RationalTime::create(static_cast<std::int64_t>(kFrames - 1),
                                                  static_cast<std::int64_t>(kFps)),
                      ringMoved)
                      .succeeded(),
                  "the ring takes its last-frame key (a corner and a counter point moved)");
    checks.expect(edit<commands::SetKeyframeAtTimeForParameter>(
                      authored, authored.composition, *ring, core::RationalTime{}, *ringStart)
                      .succeeded(),
                  "the ring takes its first-frame key");
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

void exerciseRoute(Expectations& checks, const Route& route, const std::filesystem::path& workDir,
                   [[maybe_unused]] const std::filesystem::path& loader) {
    const std::string prefix = std::string(route.name) + ": ";
    auto authored = author(checks, workDir,
                           route.layer ? commands::SvgImportMode::Layer
                                       : commands::SvgImportMode::Shapes);
    if (!authored.has_value()) {
        return;
    }
    auto& session = *authored->session;
    const std::size_t mode = route.layer ? 1 : 0;

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
    settings.pixelStorageByteLimit = std::size_t{1} << 30U;
    ui::PreviewPreparationSubmitter submitter;
    runtime::PreviewDisplayProvider expectedProvider = runtime::PreviewDisplayProvider::CpuOcio;
    std::function<RouteCounters()> counters = [] { return RouteCounters{}; };

#if defined(BLOOM_UI_SVG_ARTWORK_ROUTES_GPU)
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
        options.previewByteAllowance = std::size_t{1} << 30U;
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

    // The Viewer at the first, a middle and the last frame.
    constexpr std::array<std::uint64_t, 3> kShownFrames{0, kFrames / 2, kFrames - 1};
    std::array<Pixels, kShownFrames.size()> shown;
    const auto fallbacksBefore = counters().cpuFallbacks;
    for (std::size_t index = 0; index < kShownFrames.size(); ++index) {
        const auto frame = kShownFrames.at(index);
        const auto label = prefix + "Viewer frame " + std::to_string(frame) + ": ";
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
                   displayed != nullptr && (index == 0 || displayed != before) &&
                   displayed->provenance().provider == expectedProvider &&
                   viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current;
        });
        checks.expect(ready, label + "the Viewer displays a current frame from the expected route");
        shown.at(index) = pixelsOf(viewer.displayedFrameForTest());
        checks.expect(shown.at(index).ok && shown.at(index).width == kWidth &&
                          shown.at(index).height == kHeight,
                      label + "the Viewer frame is readable at the composition size");
        if (!ready || !shown.at(index).ok) {
            return;
        }
    }
    checks.expect(counters().cpuFallbacks == fallbacksBefore,
                  prefix + "every Viewer frame came from the expected route, none from a fallback");
    checks.expect(shown[0].rgba != shown[2].rgba && shown[1].rgba != shown[0].rgba,
                  prefix + (route.layer ? "the keyed layer moves the picture in the Viewer"
                                        : "the keyed path moves the picture in the Viewer"));
    // The compound ring (gradient, clip matte, nested opacity, keyed corner and counter point)
    // differs between the first and last frame inside its window, where nothing else is keyed.
    {
        const auto inWindow = [&](const Pixels& pixels) {
            std::vector<std::uint8_t> region;
            for (std::uint32_t y = 100; y < 170; ++y)
                for (std::uint32_t x = 110; x < 240; ++x)
                    for (std::size_t c = 0; c < 4; ++c)
                        region.push_back(pixels.rgba[(static_cast<std::size_t>(y) * pixels.width + x) * 4 + c]);
            return region;
        };
        checks.expect(inWindow(shown[0]) != inWindow(shown[2]),
                      prefix + (route.layer ? "the keyed layer moves the matted compound shape"
                                            : "dragging the ring's corner and counter point changes the matted compound shape"));
    }
    // CPU against GPU: the CPU route runs first and keeps its frames.
    for (std::size_t index = 0; index < kShownFrames.size(); ++index) {
        if (!route.gpu) {
            gCpuViewer.at(mode).at(index) = shown.at(index).rgba;
            gCpuViewerWidth.at(mode).at(index) = shown.at(index).width;
        } else if (!gCpuViewer.at(mode).at(index).empty()) {
            Pixels cpu;
            cpu.ok = true;
            cpu.width = gCpuViewerWidth.at(mode).at(index);
            cpu.height = shown.at(index).height;
            cpu.rgba = gCpuViewer.at(mode).at(index);
            checks.expect(framesMatch(cpu, shown.at(index)),
                          prefix + "Viewer frame " + std::to_string(kShownFrames.at(index)) +
                              " matches the CPU route (paths, gradient, matte, opacity, nesting" +
                              (route.layer ? ", the layer's keyed transform" : "") +
                              "; RGB within one code, exact alpha)");
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
                  prefix + "the Viewer shows the artwork at Half from the expected route");
    ui::RamPreviewController ram(session, controller, rig.scheduler, *rig.bridge, pipeline, nullptr,
                                 submitter);
    const auto beforeCold = counters();
    ram.start();
    const bool coldDone = waitUntil([&] { return !ram.isCaching(); }, 180s);
    const auto afterCold = counters();
    checks.expect(coldDone && ram.cachedFrameCount() == kFrames &&
                      ram.lastCompletion() == ui::RamPreviewController::Completion::Complete,
                  prefix + "a cold RAM preview at Half retains every frame (" +
                      std::to_string(ram.cachedFrameCount()) + " of " + std::to_string(kFrames) +
                      ")");
    checks.expect(afterCold.cpuFallbacks == beforeCold.cpuFallbacks,
                  prefix + "the cold RAM preview took no CPU fallback");
    checks.expect(!route.gpu || afterCold.nativeDispatches > beforeCold.nativeDispatches,
                  prefix + "the cold RAM preview ran on the device");
    ram.start();
    const bool warmDone = waitUntil([&] { return !ram.isCaching(); }, 60s);
    const auto afterWarm = counters();
    checks.expect(warmDone && ram.preparedFrameCount() == 0 && ram.cachedFrameCount() == kFrames,
                  prefix + "a warm RAM preview prepares nothing");
    checks.expect(afterWarm.nativeDispatches == afterCold.nativeDispatches &&
                      afterWarm.cpuFallbacks == afterCold.cpuFallbacks,
                  prefix + "a warm RAM preview dispatches nothing");
    ram.beginShutdown();

    // The export route, rendered from the same document at full size.
    runtime::TaskScheduler exportScheduler;
    const runtime::SnapshotCompiler exportCompiler(document::builtInNodeDefinitions());
    std::shared_ptr<host::GpuExportProvider> provider;
#if defined(BLOOM_UI_SVG_ARTWORK_ROUTES_GPU)
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
    for (std::size_t index = 0; index < kShownFrames.size(); ++index) {
        const auto label = prefix + "still " + std::to_string(kShownFrames.at(index));
        const auto still =
            workDir / (std::string(route.name) + "-still-" + std::to_string(index) + ".png");
        const auto result =
            scripting::Render::run(*authored->scripting, exportScheduler, exportCompiler,
                                   {.composition = authored->composition,
                                    .frame = kShownFrames.at(index),
                                    .range = std::nullopt,
                                    .preset = output::OutputPresetV1::PngRgba8SrgbV1,
                                    .destination = still},
                                   {}, provider);
        expectProvenance(result, 1, 1, label);
        checks.expect(framesMatch(shown.at(index), decodePng(still)),
                      label + " matches the Viewer (RGB within one code, exact alpha)");
    }
    const auto folder = workDir / (std::string(route.name) + "-range");
    std::filesystem::create_directories(folder);
    const auto ranged = scripting::Render::run(
        *authored->scripting, exportScheduler, exportCompiler,
        {.composition = authored->composition,
         .frame = std::nullopt,
         .range = std::pair<std::uint64_t, std::uint64_t>{kFrames / 2 - 1, kFrames / 2 + 1},
         .preset = output::OutputPresetV1::PngRgba8SrgbV1,
         .destination = folder / "range.png"},
        {}, provider);
    expectProvenance(ranged, 3, 3, prefix + "range");
    std::vector<std::filesystem::path> members;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
        if (entry.path().extension() == ".png")
            members.push_back(entry.path());
    std::ranges::sort(members);
    checks.expect(members.size() == 3, prefix + "the range writes one file per frame");
    if (members.size() == 3) {
        checks.expect(framesMatch(shown[1], decodePng(members[1])),
                      prefix + "the range's middle frame matches the Viewer's");
    }
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
    // #642: the one layer at 1080p and 4K, exported from the device with no CPU reference frame.
    if (route.gpu && route.layer) {
        for (const auto& [width, height] : {std::pair{1920U, 1080U}, std::pair{3840U, 2160U}}) {
            const auto size = std::to_string(width) + "x" + std::to_string(height);
            auto large = author(checks, workDir, commands::SvgImportMode::Layer, width, height);
            if (!large.has_value())
                continue;
            const auto still = workDir / (std::string(route.name) + "-still-" + size + ".png");
            const auto result =
                scripting::Render::run(*large->scripting, exportScheduler, exportCompiler,
                                       {.composition = large->composition,
                                        .frame = kFrames - 1,
                                        .range = std::nullopt,
                                        .preset = output::OutputPresetV1::PngRgba8SrgbV1,
                                        .destination = still},
                                       {}, provider);
            expectProvenance(result, 1, 1, prefix + size + " still");
            const auto decoded = decodePng(still);
            checks.expect(decoded.ok && decoded.width == width && decoded.height == height,
                          prefix + size + " still is written at its size");
            std::cout << route.name << " " << size << " still: " << result.gpuEvaluatedFrames
                      << " device frame(s), " << result.cpuReferenceFrames << " CPU reference frame(s)\n";
            std::filesystem::remove(still);
        }
    }
    checks.expect(provider->shutdownAndWait(10s), prefix + "the export provider retires");

    controller.beginShutdown();
    rig.bridge->beginShutdown();
    (void)waitUntil([&] { return rig.scheduler.isQuiescent(); }, 10s);
#if defined(BLOOM_UI_SVG_ARTWORK_ROUTES_GPU)
    if (service != nullptr) {
        service->beginShutdown();
        (void)waitUntil([&] {
            return service->status().state == runtime::GpuPreviewDisplayServiceState::Stopped;
        });
    }
#endif
}

#if defined(BLOOM_UI_SVG_ARTWORK_ROUTES_GPU)
// A real file in the interactive Viewer at a real size, on the application's own memory wiring
// (the shared composition-root wiring the soak harness also runs): a scheduler with the delivered
// budget view, the processing policy sampled from this machine through the application's sampler,
// the GPU scene stage with the application's processing phase, the service with the application's
// resident budget plan and default request allowance.
int viewerAtSize(const std::filesystem::path& svg, const std::uint32_t width,
                 const std::uint32_t height, const std::filesystem::path& loader) {
    Expectations checks;
    const std::string prefix =
        svg.filename().string() + ' ' + std::to_string(width) + 'x' + std::to_string(height) + ": ";
    const auto rate = document::FrameRate::create(static_cast<std::int64_t>(kFps), 1);
    const auto format =
        document::CompositionFormat::create(width, height, core::PixelAspectRatio::square(), *rate);
    if (!format.has_value()) {
        std::cerr << "FAIL: " << prefix << "the composition format is invalid\n";
        return 1;
    }
    auto created = scripting::Session::createNew(
        "SVG artwork Viewer", "Main",
        *core::RationalTime::create(2, static_cast<std::int64_t>(kFps)), *format);
    if (!created) {
        std::cerr << "FAIL: " << prefix << "the authoring session is not created\n";
        return 1;
    }
    Authored authored;
    authored.scripting = std::move(created).takeSession();
    authored.composition = authored.scripting->snapshot().project().compositions().front().id();
    authored.session = std::make_unique<ui::CompositionSession>(
        *authored.scripting->document(), *authored.scripting->commandStack(), authored.composition);
    const auto imported = edit<commands::AddSvgArtwork>(authored, authored.composition, nullptr,
                                                        svg, "artwork", true,
                                                        commands::SvgImportMode::Shapes);
    if (!imported.succeeded()) {
        std::cerr << "FAIL: " << prefix << "the file does not import\n";
        return 1;
    }

    runtime::processMemoryBudgetLedger().beginProcessSampling();
    const auto taskBudgetView = std::make_shared<runtime::TaskBudgetView>();
    auto taskConfig = runtime::TaskSchedulerConfig::defaults();
    taskConfig.budgetView = taskBudgetView;
    runtime::TaskScheduler scheduler(taskConfig);
    runtime::NodeDefinitionRegistry definitions;
    checks.expect(runtime::registerBuiltInNodeDefinitions(definitions),
                  prefix + "node definitions register");
    definitions.freeze();
    const runtime::SnapshotCompiler compiler(definitions);
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualified;
    qualified.publish(runtime::buildBloomNeutralQualifiedDisplayProcessor());
    const auto retirementOwner = render::PackedHostRetirementOwner::create();
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    const auto policy =
        std::make_shared<runtime::GpuProcessingPhasePolicy>(32, retirementOwner, taskBudgetView);
    ui::MemoryBudgetController memoryController(scheduler, bridge,
                                                runtime::processMemoryBudgetLedger(),
                                                ui::applicationMemorySampler(policy));
    static_cast<void>(ui::connectApplicationMemoryBudget(
        memoryController, policy, runtime::processMemoryBudgetLedger(), &bridge));
    auto planCache = std::make_shared<ui::CompiledPlanCache>();
    auto pipeline = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                       qualified, planCache);

    runtime::GpuPreviewDisplayServiceOptions options;
    options.enabled = true;
    options.loaderPath = loader;
    options.product = runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
    options.presentation = runtime::GpuPreviewDisplayServicePresentationMode::Disabled;
    ui::applyGpuResidentBudgetPlan(
        options, ui::gpuResidentBudgetPlanFor(std::numeric_limits<std::size_t>::max()));
    options.previewByteAllowance.reset();
    evaluator.operationCache()->bindHostRetirement(retirementOwner);
    std::shared_ptr<runtime::GpuOcioContextResolver> resolver =
        host::makePackagedGpuOcioResolver(QCoreApplication::applicationFilePath().toStdString());
    if (resolver == nullptr)
        resolver = std::make_shared<runtime::GpuOcioContextResolver>();
    auto gpuStage = ui::makeSessionRefreshingGpuSceneStage(
        compiler, evaluator, qualified, std::make_shared<runtime::GpuSceneCoverageCache>(),
        planCache, resolver,
        {.policy = policy,
         .envelope = {.metadataBytes = 16ULL * 1024 * 1024,
                      .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                      .growsRetainedSceneStorage = true,
                      .requestedExecutorBytes = std::nullopt,
                      .deviceWorkingBytes =
                          512ULL * 1024 * 1024 + options.packedTransferByteAllowance}});
    options.processingPhasePolicy = policy;
    options.cpuStoragePlanner =
        ui::makeCompositionPreviewCpuStoragePlanner(compiler, evaluator, planCache);
    auto service = std::make_unique<runtime::GpuPreviewDisplayService>(
        scheduler, std::move(gpuStage),
        ui::makeCompositionPreviewCpuStage(compiler, evaluator, qualified, planCache),
        ui::makeCompositionPreviewCpuDisplayFallback(displayPreparer), options);
    const auto shutDown = [&] {
        memoryController.beginShutdown();
        bridge.beginShutdown();
        (void)waitUntil([&] { return scheduler.isQuiescent(); }, 10s);
        service->beginShutdown();
        (void)waitUntil([&] {
            return service->status().state == runtime::GpuPreviewDisplayServiceState::Stopped;
        });
        evaluator.operationCache()->close();
        retirementOwner->beginShutdown();
        const bool retired = waitUntil([&] { return retirementOwner->exitAcknowledged(); }, 15s);
        while (retired && !retirementOwner->collect())
            QCoreApplication::processEvents();
        checks.expect(retired, prefix + "the packed host retirement shuts down");
    };
    waitUntil(
        [&] {
            return service->status().state != runtime::GpuPreviewDisplayServiceState::Initializing;
        },
        90s);
    if (service->status().state != runtime::GpuPreviewDisplayServiceState::Ready) {
        std::cout << "NotRun: " << prefix << "no qualified device ("
                  << service->status().residentDetail << ")" << std::endl;
        shutDown();
        return gRequireDevice ? 1 : 77;
    }
    const ui::PreviewPreparationSubmitter submitter =
        [svc = service.get()](runtime::TaskRequest request, const document::Snapshot& snapshot,
                              const runtime::PreviewRequestIdentity& identity,
                              const std::size_t bytes,
                              const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            return svc->submit(std::move(request), snapshot, identity, bytes, overrides);
        };
    ui::CompositionPreviewSettings settings;
    settings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
    {
        ui::CompositionPreviewController controller(*authored.session, scheduler, bridge, pipeline,
                                                    settings, nullptr, nullptr, submitter);
        ui::ViewerEditor viewer(*authored.session, controller);
        viewer.resize(960, 540);
        viewer.show();
        const auto before = service->status().counters;
        controller.requestRefresh();
        const bool ready = waitUntil(
            [&] {
                const auto displayed = viewer.displayedFrameForTest();
                return controller.state().activity == ui::PreviewActivity::Ready &&
                       displayed != nullptr &&
                       viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current;
            },
            120s);
        const auto displayed = viewer.displayedFrameForTest();
        const auto after = service->status().counters;
        const auto shown = pixelsOf(displayed);
        const bool onDevice =
            displayed != nullptr &&
            displayed->provenance().provider == runtime::PreviewDisplayProvider::GpuPreparedPacked;
        checks.expect(ready && onDevice,
                      prefix + "the Viewer shows a current frame prepared on the device: " +
                          service->status().residentDetail);
        checks.expect(shown.ok && shown.width == width && shown.height == height,
                      prefix + "the Viewer frame is the composition size");
        checks.expect(after.cpuFallbacks == before.cpuFallbacks &&
                          after.nativeDispatches > before.nativeDispatches,
                      prefix + "the device ran it with no CPU fallback");
        std::cout << prefix << "Viewer frame from "
                  << (onDevice ? "GpuPreparedPacked" : "another route") << ", "
                  << (after.nativeDispatches - before.nativeDispatches) << " native dispatches, "
                  << (after.cpuFallbacks - before.cpuFallbacks) << " CPU fallbacks" << std::endl;
        controller.beginShutdown();
        viewer.releasePackedFramesForShutdown();
        controller.releasePublishedFramesForShutdown();
    }
    shutDown();
    if (checks.failures() != 0) {
        std::cerr << "viewer SVG artwork: " << checks.failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: " << prefix << "the Viewer prepares it on the GPU\n";
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    std::filesystem::path loader;
    std::filesystem::path svg;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<char*> forwarded{argv[0]};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            loader = argv[++index];
        } else if (argument == "--require-device") {
            gRequireDevice = true;
        } else if (argument == "--svg" && index + 1 < argc) {
            svg = argv[++index];
        } else if (argument == "--size" && index + 1 < argc) {
            const std::string size = argv[++index];
            const auto x = size.find('x');
            if (x != std::string::npos) {
                width = static_cast<std::uint32_t>(std::stoul(size.substr(0, x)));
                height = static_cast<std::uint32_t>(std::stoul(size.substr(x + 1)));
            }
        } else {
            forwarded.push_back(argv[index]);
        }
    }
    int forwardedCount = static_cast<int>(forwarded.size());
    QApplication application(forwardedCount, forwarded.data());
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName("BloomSvgArtworkRoutes");
    QCoreApplication::setApplicationName("ViewerExportSvgArtwork");
    QSettings().setValue("viewer/resolution", "Full");

    if (!svg.empty()) {
#if defined(BLOOM_UI_SVG_ARTWORK_ROUTES_GPU)
        if (loader.empty() || width == 0 || height == 0) {
            std::cerr << "FAIL: --svg needs --loader and --size <W>x<H>\n";
            return 1;
        }
        return viewerAtSize(svg, width, height, loader);
#else
        std::cout << "NotRun: --svg: this build has no Vulkan support\n";
        return gRequireDevice ? 1 : 77;
#endif
    }

    Expectations checks;
    const std::filesystem::path workDir = temporary.path().toStdString();
    exerciseRoute(checks, {"cpu", false}, workDir, loader);
    exerciseRoute(checks, {"cpu-layer", false, true}, workDir, loader);

#if defined(BLOOM_UI_SVG_ARTWORK_ROUTES_GPU)
    if (loader.empty()) {
        std::cout << "NotRun: gpu route: no Vulkan loader was given (--loader)\n";
        checks.expect(!gRequireDevice, "--require-device needs --loader");
    } else {
        exerciseRoute(checks, {"gpu", true}, workDir, loader);
        exerciseRoute(checks, {"gpu-layer", true, true}, workDir, loader);
    }
#else
    std::cout << "NotRun: gpu route: this build has no Vulkan support\n";
    checks.expect(!gRequireDevice, "--require-device needs a Vulkan build");
#endif

    if (checks.failures() != 0) {
        std::cerr << "viewer/export SVG artwork: " << checks.failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: viewer/export SVG artwork\n";
    return 0;
}
