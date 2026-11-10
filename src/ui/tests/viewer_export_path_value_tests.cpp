// #589: a path travels the Path socket through the routes an artist actually uses. A keyed compound
// path on a Path node drives Shape Source's `path`; the real Viewer preview path shows it at
// several times, and the real export route renders the same frames as a still, a PNG range and a
// movie. Each Viewer frame equals the exported frame (RGB within one code, exact alpha), the
// picture moves with the keys, and the second key morphs into a key of a different shape.
//
// The CPU reference half always runs (CPU preview pipeline against the CPU export provider). The
// GPU half runs the GPU-prepared packed preview product against the GPU export provider when
// --loader names a Vulkan loader and a qualified device exists, and requires every exported frame
// to have been evaluated on the device; otherwise it prints NotRun explicitly, and --require-device
// turns that into a failure. A movie needs the media worker: where it is absent the test says
// NotRun for that route.
//
// #509 runs the same routes again over the path-stroke features: the keyed path stroked as Diamond
// stamps and outlined by Alpha Contour, a text whose Text Outline is stroked over it in a Merge, a
// solid's mask stroked through Mask Path, and the Saber glowing path preset.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/path_glow_presets.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
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
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#if defined(BLOOM_UI_PATH_VALUE_ROUTES_GPU)
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

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 48;
constexpr std::uint64_t kFps = 24;

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
    // #509: the composition also carries the path-stroke features (see addPathStrokeFeatures).
    bool features = false;
};

document::PathContour square(const double left, const double top, const double size) {
    document::PathContour contour;
    contour.closed = true;
    contour.anchors = {{{left, top}, {}, {}},
                       {{left + size, top}, {}, {}},
                       {{left + size, top + size}, {}, {}},
                       {{left, top + size}, {}, {}}};
    return contour;
}

document::PathValue framed(const double inner) {
    document::PathValue value;
    value.contours = {square(4, 4, 40), square(inner, inner, 12)};
    value.fillRule = document::PathFillRule::EvenOdd;
    return value;
}

// One authored composition shared by the Viewer session and the export route: a Path node, keyed at
// 0, 1 and 2 seconds, driving a Shape Source. The first two keys interpolate and the third has a
// different contour count, so the second morphs into it: its hole shrinks to the hole's center.
struct Authored final {
    std::unique_ptr<scripting::Session> scripting;
    std::unique_ptr<ui::CompositionSession> session;
    document::CompositionId composition;
};

template <typename Operation, typename... Arguments>
commands::CommandResult edit(Authored& authored, Arguments&&... arguments) {
    commands::Transaction transaction("Path value", authored.scripting->snapshot().revision());
    transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
    return authored.scripting->commandStack()->execute(std::move(transaction));
}

[[nodiscard]] std::optional<Authored> author(Expectations& checks) {
    const auto format = document::CompositionFormat::create(kWidth, kHeight);
    checks.expect(format.has_value(), "the composition format is valid");
    if (!format.has_value()) {
        return std::nullopt;
    }
    auto created = scripting::Session::createNew("Path value routes", "Main",
                                                 core::RationalTime::fromInteger(4), *format);
    checks.expect(static_cast<bool>(created), "the authoring session is created");
    if (!created) {
        return std::nullopt;
    }
    Authored authored;
    authored.scripting = std::move(created).takeSession();
    authored.composition = authored.scripting->snapshot().project().compositions().front().id();
    authored.session = std::make_unique<ui::CompositionSession>(
        *authored.scripting->document(), *authored.scripting->commandStack(), authored.composition);

    const auto added =
        edit<commands::AddNode>(authored, authored.composition,
                                std::string(document::kPathValueNodeType), document::Vec2d{0, 0});
    const auto producer = added.outputId<document::NodeId>(commands::kAddNodeOutput);
    commands::ShapeLayerGeometry geometry;
    geometry.path = document::PathValue::fromContour(square(1, 1, 2));
    commands::ShapeLayerStyle style;
    style.fillColor = core::Color4d{0.25, 0.5, 0.75, 1.0};
    const auto shape = edit<commands::AddShapeLayer>(authored, authored.composition,
                                                     document::ShapeKind::Path, geometry, style)
                           .outputId<document::NodeId>("shapeNode");
    checks.expect(producer.has_value() && shape.has_value(),
                  "the Path node and shape are authored");
    if (!producer.has_value() || !shape.has_value()) {
        return std::nullopt;
    }
    checks.expect(edit<commands::ConnectPorts>(
                      authored, authored.composition,
                      document::OutputPortRef{*producer, std::string(document::kValuePortName)},
                      document::NodeInputRef{*shape, "path"})
                      .succeeded(),
                  "the Path output connects to Shape Source's path");
    const auto* main =
        authored.scripting->snapshot().project().findComposition(authored.composition);
    std::optional<document::ParameterId> parameter;
    for (const auto& binding : main->graph().findNode(*producer)->parameters) {
        if (binding.role == document::kValueParameterRole) {
            parameter = binding.parameterId;
        }
    }
    checks.expect(parameter.has_value(), "the Path node has its value parameter");
    if (!parameter.has_value()) {
        return std::nullopt;
    }
    auto oneContour = framed(16.0);
    oneContour.contours.pop_back();
    const std::array<document::PathValue, 3> keys{framed(8.0), framed(24.0), oneContour};
    for (std::size_t second = 0; second < keys.size(); ++second) {
        checks.expect(edit<commands::SetKeyframeAtTimeForParameter>(
                          authored, authored.composition, *parameter,
                          core::RationalTime::fromInteger(static_cast<std::int64_t>(second)),
                          keys.at(second))
                          .succeeded(),
                      "the Path node takes a whole-value key");
    }
    return authored;
}

// #509: the path-stroke features, added to the authored composition. The keyed path is stroked as
// Diamond stamps that follow it and its Shape is outlined by Alpha Contour; a text is stroked along
// its Text Outline, the stroke merged over it as the text layer's content; a solid's mask is
// stroked through Mask Path, on top of the composition's Merge; and a Saber glowing path preset
// is drawn by a layer of its own. Nothing added moves with time, so the keyed path is still the
// only motion.
[[nodiscard]] bool addPathStrokeFeatures(Expectations& checks, Authored& authored) {
    const auto comp = authored.composition;
    bool ok = true;
    const auto must = [&](commands::CommandResult result, const std::string& what) {
        checks.expect(result.succeeded(), "#509 features: " + what);
        ok = ok && result.succeeded();
        return result;
    };
    const auto graph = [&]() -> const document::CanonicalGraph& {
        return authored.scripting->snapshot().project().findComposition(comp)->graph();
    };
    const auto add = [&](const std::string_view type) {
        return must(edit<commands::AddNode>(authored, comp, std::string(type),
                                            document::Vec2d{0, 0}),
                    "adds " + std::string(type))
            .outputId<document::NodeId>(commands::kAddNodeOutput)
            .value_or(document::NodeId{});
    };
    const auto set = [&](const document::NodeId node, const std::string_view role,
                         document::ParameterValue value) {
        const auto* record = graph().findNode(node);
        if (record == nullptr)
            return;
        for (const auto& binding : record->parameters)
            if (binding.role == role)
                (void)must(edit<commands::SetParameterSource>(
                               authored, comp, binding.parameterId,
                               document::ConstantValueSource{std::move(value)}),
                           "sets " + std::string(role));
    };
    const auto connect = [&](const document::NodeId from, const std::string_view output,
                             const document::NodeId to, const std::string_view input) {
        const document::NodeInputRef into{to, std::string(input)};
        if (std::ranges::any_of(graph().edges(), [&](const auto& edge) {
                const auto* target = std::get_if<document::NodeInputRef>(&edge.destination);
                return target != nullptr && *target == into;
            }))
            (void)must(edit<commands::DisconnectInput>(authored, comp, into), "disconnects");
        (void)must(edit<commands::ConnectPorts>(
                       authored, comp, document::OutputPortRef{from, std::string(output)}, into),
                   "connects " + std::string(input));
    };
    // `source`'s image as a new entry of `merge`, on top of it or at its bottom.
    const auto enter = [&](const document::NodeId source, const document::NodeId merge,
                           const bool onTop) {
        std::optional<document::LayerSlotId> top;
        if (const auto* stack = graph().merge(merge);
            onTop && stack != nullptr && !stack->entries().empty())
            top = stack->entries().front().slotId;
        (void)must(edit<commands::ConnectPorts>(
                       authored, comp, document::OutputPortRef{source, "image"},
                       document::InputPortRef{document::LayerStackInputRef{merge, {}, "content"}},
                       document::builtInNodeDefinitions(), top),
                   "enters a Merge");
    };
    // The input an output feeds (the one edge from it), to put a node between them.
    const auto fed = [&](const document::NodeId from,
                         const std::string_view output) -> std::optional<document::NodeInputRef> {
        for (const auto& edge : graph().edges())
            if (const auto* target = std::get_if<document::NodeInputRef>(&edge.destination);
                target != nullptr && edge.source.nodeId == from && edge.source.port == output)
                return *target;
        return std::nullopt;
    };

    // The keyed path, stroked as Diamond stamps, and the Shape outlined by Alpha Contour.
    std::optional<document::NodeId> pathNode;
    std::optional<document::NodeId> shape;
    for (const auto& node : graph().nodes()) {
        if (node.typeId == document::kPathValueNodeType)
            pathNode = node.id;
        if (node.typeId == document::kShapeSourceNodeType)
            shape = node.id;
    }
    const auto layerInput = shape ? fed(*shape, "image") : std::nullopt;
    checks.expect(pathNode && shape && layerInput, "#509 features: the keyed shape is found");
    if (!pathNode || !shape || !layerInput)
        return false;
    const auto stroke = add(document::kPathStrokeNodeType);
    set(stroke, "width", 4.0);
    set(stroke, "spacing", 6.0);
    set(stroke, "stamp", document::selectorStoredValue(document::StrokeStamp::Diamond));
    set(stroke, "stampAlign", document::selectorStoredValue(document::StampAlign::FollowPath));
    connect(*pathNode, document::kValuePortName, stroke, "path");
    connect(stroke, "path", *shape, "path");
    const auto contour = add(document::kAlphaContourNodeType);
    set(contour, "width", 2.0);
    set(contour, "offset", 1.0);
    set(contour, "color", core::Color4d{1.0, 0.6, 0.2, 1.0});
    connect(*shape, "image", contour, "input");
    connect(contour, "image", layerInput->nodeId, layerInput->port);

    // A text stroked along its Text Outline, the stroke merged over it.
    const auto text = must(edit<commands::AddTextLayer>(authored, comp, "Title", "Ag",
                                                        document::Vec2d{40, 30}, 1.0, 18.0),
                           "adds a text");
    const auto textNode = text.outputId<document::NodeId>(commands::kAddTextLayerTextNodeOutput);
    const auto textLayer =
        text.outputId<document::NodeId>(commands::kAddTextLayerLayerOutputNodeOutput);
    if (!textNode || !textLayer)
        return false;
    const auto outline = add(document::kTextOutlineNodeType);
    const auto outlineStroke = add(document::kPathStrokeNodeType);
    set(outlineStroke, "width", 1.5);
    connect(outline, "path", outlineStroke, "path");
    const auto outlineShape = add(document::kShapeSourceNodeType);
    set(outlineShape, "kind", static_cast<std::int64_t>(document::ShapeKind::Path));
    set(outlineShape, "fillColor", core::Color4d{0.9, 0.2, 0.6, 1.0});
    connect(outlineStroke, "path", outlineShape, "path");
    const auto textMerge = add(document::kLayerStackNodeType);
    enter(*textNode, textMerge, false);
    enter(outlineShape, textMerge, true);
    connect(textMerge, "image", *textLayer, std::string(document::kLayerOutputContentInputPort));

    // A solid's mask, stroked through Mask Path, on top of the composition's Merge.
    const auto solid = must(
        edit<commands::AddSolidLayer>(authored, comp, "Plate", core::Color4d{0.1, 0.1, 0.1, 1.0}),
        "adds a solid");
    const auto plate = solid.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
    const auto plateNode =
        solid.outputId<document::NodeId>(commands::kAddSolidLayerLayerOutputNodeOutput);
    if (!plate || !plateNode)
        return false;
    (void)must(edit<commands::AddLayerMask>(authored, comp, *plate,
                                            document::PathValue::fromContour(square(6, 26, 16))),
               "masks the solid");
    std::optional<document::NodeId> mainMerge;
    for (const auto& edge : graph().edges())
        if (const auto* entry = std::get_if<document::LayerStackInputRef>(&edge.destination);
            entry != nullptr && edge.source.nodeId == *plateNode)
            mainMerge = entry->stackNodeId;
    checks.expect(mainMerge.has_value(), "#509 features: the solid is an entry of a Merge");
    if (!mainMerge)
        return false;
    const auto maskPath = add(document::kMaskPathNodeType);
    set(maskPath, document::kPathSourceParameterRole,
        static_cast<std::int64_t>(plateNode->value()));
    const auto maskStroke = add(document::kPathStrokeNodeType);
    set(maskStroke, "width", 2.0);
    connect(maskPath, "path", maskStroke, "path");
    const auto maskShape = add(document::kShapeSourceNodeType);
    set(maskShape, "kind", static_cast<std::int64_t>(document::ShapeKind::Path));
    set(maskShape, "fillColor", core::Color4d{0.9, 0.8, 0.2, 1.0});
    connect(maskStroke, "path", maskShape, "path");
    enter(maskShape, *mainMerge, true);

    // The Saber preset, drawn by a layer of its own.
    const auto saber =
        must(edit<commands::CreatePathGlowPreset>(authored, comp, commands::PathGlowPreset::Saber,
                                                  document::Vec2d{0, 0}),
             "adds the Saber preset")
            .outputId<document::NodeId>(commands::kPathGlowPresetNodeOutput);
    const auto glowLayer =
        must(edit<commands::AddSolidLayer>(authored, comp, "Saber",
                                           core::Color4d{0.0, 0.0, 0.0, 1.0}),
             "adds the Saber's layer")
            .outputId<document::NodeId>(commands::kAddSolidLayerLayerOutputNodeOutput);
    if (!saber || !glowLayer)
        return false;
    connect(*saber, "image1", *glowLayer, std::string(document::kLayerOutputContentInputPort));
    return ok;
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
    if (route.features && !addPathStrokeFeatures(checks, *authored)) {
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

    // The Viewer route's shader tools: their first frame compiles every program it needs.
    std::shared_ptr<runtime::GpuOcioContextResolver> viewerTools;
#if defined(BLOOM_UI_PATH_VALUE_ROUTES_GPU)
    std::unique_ptr<runtime::GpuPreviewDisplayService> service;
    if (route.gpu) {
        auto resolver = host::makePackagedGpuOcioResolver(
            QCoreApplication::applicationFilePath().toStdString());
        viewerTools = resolver;
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
        const auto waitStarted = std::chrono::steady_clock::now();
        const auto before =
            index == 0 ? ui::PreparedPreviewFrameHandle{} : viewer.displayedFrameForTest();
        // The first frame of a route also prepares every program the composition needs (the #509
        // features need 17, compiled by 16 runs of the shader tools): about a second on an idle
        // machine and four with every core oversubscribed twice, well inside the wait. Its
        // FIRST-FRAME line below says what it compiled and how long it waited.
        const bool ready = waitUntil(
            [&] {
                const auto displayed = viewer.displayedFrameForTest();
                return controller.state().activity == ui::PreviewActivity::Ready &&
                       displayed != nullptr && (index == 0 || displayed != before || index == 3) &&
                       displayed->provenance().provider == expectedProvider &&
                       viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current;
            },
            30s);
        // What the first frame waited on: the shader programs it compiled (each a run of the
        // packaged shader tools on the frame's worker) and how long the wait was.
        if (index == 0 && viewerTools != nullptr) {
            const auto preparer = viewerTools->preparer();
            const auto counters = preparer != nullptr ? preparer->counters()
                                                      : runtime::GpuOcioPreparerCounters{};
            std::cout << "FIRST-FRAME route=" << route.name << " ready=" << ready
                      << " waitedMs="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - waitStarted)
                             .count()
                      << " toolResolves=" << viewerTools->counters().resolves
                      << " programs=" << counters.compiles
                      << " shaderToolRuns=" << counters.shaderCompilerRuns
                      << " compileFailures=" << counters.compileFailures << '\n';
        }
        if (!ready) {
            const auto& state = controller.state();
            const auto displayed = viewer.displayedFrameForTest();
            std::string why =
                "activity " + std::to_string(static_cast<int>(state.activity)) + ", provider " +
                (displayed ? std::to_string(static_cast<int>(displayed->provenance().provider))
                           : std::string("none")) +
                ", " + state.message.toStdString();
            for (const auto& diagnostic : state.diagnostics)
                why += "; " + diagnostic.summary;
            checks.expect(false, label +
                                     "the Viewer displays a current frame from the expected "
                                     "route (" +
                                     why + ")");
        }
        shown.at(index) = pixelsOf(viewer.displayedFrameForTest());
        checks.expect(shown.at(index).ok && shown.at(index).width == kWidth &&
                          shown.at(index).height == kHeight,
                      label + "the Viewer process frame is readable at the composition size");
        if (!ready || !shown.at(index).ok) {
            return;
        }
    }

    // The picture moves with the keys. The layer centers the 4..44 square on the frame, so a path
    // point (x, y) shows at (x + 8, y): the hole's center goes from (22, 14) to (38, 30), and then
    // the hole (24..36) shrinks toward that center on its way to the key without a hole. (With the
    // #509 features the path is stroked, not filled, so only the motion and the morph are asked of
    // it.)
    if (!route.features) {
        checks.expect(shown[0].at(22, 14)[3] == 0 && shown[0].at(38, 30)[3] == 255,
                      prefix + "the first key's hole shows in the Viewer");
        checks.expect(shown[2].at(22, 14)[3] == 255 && shown[2].at(38, 30)[3] == 0,
                      prefix + "the second key's hole shows in the Viewer");
    }
    checks.expect(shown[1].rgba != shown[0].rgba && shown[1].rgba != shown[2].rgba,
                  prefix + "the frame between them moves");
    if (!route.features)
        checks.expect(shown[2].at(33, 25)[3] == 0 && shown[3].at(33, 25)[3] == 255 &&
                          shown[3].at(38, 30)[3] == 0,
                      prefix + "the second key morphs into the key of a different shape: the hole "
                               "shrinks toward its center");
    else
        checks.expect(shown[3].rgba != shown[2].rgba,
                      prefix + "the stroked second key morphs into the key of a different shape "
                               "instead of holding");

    // The export route, rendered from the same document.
    runtime::TaskScheduler exportScheduler;
    const runtime::SnapshotCompiler exportCompiler(document::builtInNodeDefinitions());
    std::shared_ptr<host::GpuExportProvider> provider;
#if defined(BLOOM_UI_PATH_VALUE_ROUTES_GPU)
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
    (void)waitUntil([&] { return rig.scheduler.isQuiescent(); }, 10s);
#if defined(BLOOM_UI_PATH_VALUE_ROUTES_GPU)
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
    QCoreApplication::setOrganizationName("BloomPathValueRoutes");
    QCoreApplication::setApplicationName("ViewerExportPathValue");
    QSettings().setValue("viewer/resolution", "Full");

    Expectations checks;
    const std::filesystem::path workDir = temporary.path().toStdString();
    exerciseRoute(checks, {"cpu", false}, workDir, loader);
    exerciseRoute(checks, {"cpu-509", false, true}, workDir, loader);

#if defined(BLOOM_UI_PATH_VALUE_ROUTES_GPU)
    if (loader.empty()) {
        std::cout << "NotRun: gpu route: no Vulkan loader was given (--loader)\n";
        checks.expect(!gRequireDevice, "--require-device needs --loader");
    } else {
        exerciseRoute(checks, {"gpu", true}, workDir, loader);
        exerciseRoute(checks, {"gpu-509", true, true}, workDir, loader);
    }
#else
    std::cout << "NotRun: gpu route: this build has no Vulkan support\n";
    checks.expect(!gRequireDevice, "--require-device needs a Vulkan build");
#endif

    if (checks.failures() != 0) {
        std::cerr << "viewer/export path value: " << checks.failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: viewer/export path value\n";
    return 0;
}
