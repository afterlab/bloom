// #642: an SVG imported as one layer (a Vector Artwork layer) against the same file imported as
// editable shapes, on the CPU reference, through the snapshot compiler's expansion.
//
// - The layer's own opacity (keyed), blend mode, transform, parent, track matte, mask and an effect
//   between its source and its slot compose through the expansion's root exactly as they compose
//   through the editable import's root group, whole frame, over a few frames.
// - The plain layer is the editable import, pixel for pixel, on files that use gradients, clips,
//   masks, opacity, blur, dashes and text.
// - Saved and reopened with the file deleted, the layer keeps its mode and its SVG and renders the
//   same frame.
// - Scaled up and turned, the layer stays sharp: its transform is drawn through the artwork's
//   geometry (VectorArtworkComposedLayer), so it matches the editable import of the same file with
//   the transform baked into its points, not a resampled picture; its published bounds are the
//   layer's own box through its transform.
// - The compiler keeps one plan per revision (an animated transform renders every frame from it),
//   parses a block once per digest (VectorArtworkSceneCache: hit, miss, admission, retirement),
//   cancels, refuses an unreadable SVG by name, and says which layers keep the resampled picture.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/core/blend_mode.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/svg_artwork.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/platform/font_catalog.hpp>
#include <bloom/project/open_archive.hpp>
#include <bloom/project/save_archive.hpp>
#include <bloom/media/svg_scene.hpp>
#include <bloom/runtime/compiled_plan_cache.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/vector_artwork_scene_cache.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <typeinfo>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace bloom;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::filesystem::path writeSvg(const std::string& name, const std::string& text) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("bloom-svg-layer-" + name + "-" + std::to_string(stamp) + ".svg");
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

std::shared_ptr<const platform::FontCatalogue> noFonts() {
    auto fonts = std::make_shared<platform::FontCatalogue>();
    fonts->status = platform::FontCatalogueStatus::Available;
    return fonts;
}

// The files: every kind of element the import builds, small enough to render at once.
const std::string kComposite = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120">
<defs><linearGradient id="g" x1="0" x2="1"><stop offset="0" stop-color="#ff0"/><stop offset="1" stop-color="#08f"/></linearGradient>
<radialGradient id="r" gradientTransform="translate(0.5 0.5) scale(0.5 0.3) translate(-0.5 -0.5)"><stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#f0f" stop-opacity="0.4"/></radialGradient>
<clipPath id="c"><circle cx="60" cy="60" r="40"/></clipPath>
<mask id="m"><rect x="120" y="10" width="70" height="100" fill="#fff"/><circle cx="155" cy="60" r="20" fill="#000"/></mask>
<filter id="b"><feGaussianBlur stdDeviation="1.5"/></filter></defs>
<rect width="200" height="120" fill="#123"/>
<g id="Clipped" clip-path="url(#c)" opacity="0.8"><rect x="10" y="10" width="100" height="100" fill="url(#g)"/>
<path d="M20 100 L60 20 L100 100 Z" fill="#e33" fill-rule="evenodd"/></g>
<g id="Masked" mask="url(#m)"><rect x="110" y="0" width="90" height="120" fill="url(#r)"/></g>
<circle id="Soft" cx="160" cy="95" r="12" fill="#0c6" filter="url(#b)"/>
<polyline id="Dashes" points="5,115 60,105 120,115 195,105" fill="none" stroke="#fff" stroke-width="3" stroke-dasharray="6 3" stroke-linecap="round"/>
<text id="Label" x="100" y="20" font-size="14" text-anchor="middle" fill="#fff">SVG</text>
</svg>)svg";

const std::string kIcon = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="48" height="48">
<path d="M12 2a10 10 0 1 0 0 20a10 10 0 1 0 0-20zm0 4a6 6 0 1 1 0 12a6 6 0 1 1 0-12z" fill="#36c" fill-rule="evenodd"/>
<path d="M11 7h2v6h-2zM11 15h2v2h-2z" fill="#fc0" stroke="#000" stroke-width="0.5" stroke-linejoin="bevel"/></svg>)svg";

struct Fixture final {
    document::NewProject initial;
    document::CompositionId main;
    document::Document document;
    commands::CommandStack stack;
    document::LayerId art;
    std::optional<document::NodeId> artworkNode;
    std::optional<document::NodeId> rootMerge;

    Fixture(const std::filesystem::path& file, const commands::SvgImportMode mode, const std::uint32_t width,
            const std::uint32_t height, const bool fit = true)
        : initial(document::makeNewProject(
              "SVG layer", "Main", core::RationalTime::fromInteger(4),
              *document::CompositionFormat::create(width, height, core::PixelAspectRatio::square(),
                                                   *document::FrameRate::create(24, 1)))),
          main(initial.initialCompositionId), document(std::move(initial.project)), stack(document) {
        commands::Transaction transaction("Import SVG", document.snapshot().revision());
        transaction.emplace<commands::AddSvgArtwork>(main, nullptr, file, "art", fit, mode)
            .withFonts(noFonts())
            .withProjectDirectory(file.parent_path());
        const auto result = stack.execute(std::move(transaction));
        if (!result.changed())
            throw std::runtime_error("the SVG import was refused");
        art = *result.outputId<document::LayerId>("layer");
        artworkNode = result.outputId<document::NodeId>("node");
        rootMerge = result.outputId<document::NodeId>("merge");
    }

    [[nodiscard]] const document::Composition& composition() const {
        return *document.snapshot().project().findComposition(main);
    }
    template <typename Operation, typename... Arguments> commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Edit", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = stack.execute(std::move(transaction));
        if (!result.changed()) {
            std::string why;
            for (const auto& failure : result.operationFailures)
                why += " " + failure.issue.message;
            throw std::runtime_error("a fixture edit (" + std::string(typeid(Operation).name()) + ") was refused:" + why);
        }
        return result;
    }
    [[nodiscard]] document::ParameterId layerParameter(const document::LayerId layer, const std::string_view role) const {
        const auto& graph = composition().graph();
        const auto* boundary = graph.findLayer(layer);
        const auto* binding = boundary ? document::layerParameterBinding(graph, *boundary, role) : nullptr;
        if (binding == nullptr)
            throw std::runtime_error("a layer parameter is missing");
        return binding->parameterId;
    }
    [[nodiscard]] document::ParameterId nodeParameter(const document::NodeId node, const std::string_view role) const {
        const auto* record = composition().graph().findNode(node);
        for (const auto& binding : record->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("a node parameter is missing");
    }
};

struct Frame final {
    std::vector<render::Rgba32f> pixels;
    std::int64_t width = 0;
};

// The CPU reference of one composition of a project at a time.
std::optional<Frame> renderProject(const document::Document& document, const document::CompositionId id,
                                   const core::RationalTime time, const std::string& label) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({document.snapshot(), id}, {});
    expect(compiled.plan != nullptr,
           label + ": compiles: " + runtime::describeCompileFailure(compiled, document.snapshot().project(), id));
    if (!compiled.plan)
        return std::nullopt;
    runtime::EvaluationRequest request{.time = time,
                                       .output = compiled.plan->output(),
                                       .resolution = runtime::CompositionFormatResolution{},
                                       .quality = runtime::EvaluationQuality::Reference,
                                       .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                       .pixelStorageByteLimit = std::size_t{1} << 30};
    request.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, request, {});
    expect(result.frame() != nullptr, label + ": the CPU reference evaluates");
    if (result.frame() == nullptr)
        return std::nullopt;
    const auto& image = result.frame()->processImage();
    const auto pixels = image.pixels();
    return Frame{{pixels.begin(), pixels.end()}, static_cast<std::int64_t>(image.descriptor()->dataWindow().extent().width())};
}

// The one-layer composition through the snapshot compiler, which expands every Vector Artwork node.
std::optional<Frame> renderExpanded(const document::Document& document, const document::CompositionId id,
                                    const core::RationalTime time, const std::string& label) {
    return renderProject(document, id, time, label);
}

// The one-layer composition expanded by hand with the given context and compiled: with
// composeLayerTransform off it is the resampled picture an editable import's root group gives.
std::optional<Frame> renderExpandedWith(const document::Document& document, const document::CompositionId id,
                                        const core::RationalTime time, const std::string& label,
                                        document::VectorArtworkExpansionContext context) {
    const auto snapshot = document.snapshot();
    const auto* composition = snapshot.project().findComposition(id);
    if (!context.fonts)
        context.fonts = noFonts();
    const auto expansion = document::expandVectorArtwork(snapshot.project(), *composition, context);
    expect(expansion.composition.has_value() && expansion.unresolved.empty(), label + ": the layer expands");
    if (!expansion.composition)
        return std::nullopt;
    expect(std::ranges::none_of(expansion.composition->graph().nodes(),
                                [](const auto& node) { return node.typeId == document::kVectorArtworkNodeType; }),
           label + ": no Vector Artwork node is left");
    document::Project project = snapshot.project();
    *project.findComposition(id) = *expansion.composition;
    const document::Document expanded(std::move(project));
    return renderProject(expanded, id, time, label);
}

// The largest difference of any channel, relative above 1 (the corpus gate's measure).
double difference(const Frame& a, const Frame& b) {
    if (a.pixels.size() != b.pixels.size() || a.width != b.width)
        return std::numeric_limits<double>::infinity();
    double worst = 0.0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i)
        for (std::size_t c = 0; c < 4; ++c) {
            const auto x = static_cast<double>(a.pixels[i].components()[c]);
            const auto y = static_cast<double>(b.pixels[i].components()[c]);
            if (!std::isfinite(x) || !std::isfinite(y))
                return std::numeric_limits<double>::infinity();
            worst = std::max(worst, std::abs(x - y) / std::max(1.0, std::max(std::abs(x), std::abs(y))));
        }
    return worst;
}

double coverage(const Frame& frame) {
    double alpha = 0.0;
    for (const auto& pixel : frame.pixels)
        alpha += static_cast<double>(pixel.alpha());
    return alpha;
}

constexpr double kGate = 2e-6;

core::RationalTime frame(const std::int64_t index) { return *core::RationalTime::create(index, 24); }

// The same layer edits on either import's root layer: everything a layer carries that a Merge
// entry's root could compose differently.
void dressLayer(Fixture& fixture, const document::NodeId source) {
    using namespace commands;
    const auto main = fixture.main;
    // A parent that moves and turns the layer, itself drawn at zero opacity.
    const auto parent = fixture.run<AddSolidLayer>(main, "Parent", core::Color4d{1, 0, 0, 1},
                                                   document::Vec2d{150, 80}, 0.0);
    const auto parentLayer = *parent.outputId<document::LayerId>(kAddSolidLayerLayerOutput);
    fixture.run<SetParameterSource>(main, fixture.layerParameter(parentLayer, document::kRotationParameterRole),
                                    document::ConstantValueSource{document::ParameterValue{8.0}});
    // A smaller white solid below the layer, used as its alpha track matte.
    const auto matte = fixture.run<AddSolidLayer>(main, "Matte", core::Color4d{1, 1, 1, 1},
                                                  document::Vec2d{120, 70}, 1.0);
    const auto matteLayer = *matte.outputId<document::LayerId>(kAddSolidLayerLayerOutput);
    const auto matteNode = *matte.outputId<document::NodeId>(kAddSolidLayerSolidNodeOutput);
    fixture.run<SetParameterSource>(main, fixture.nodeParameter(matteNode, document::kSolidWidthParameterRole),
                                    document::ConstantValueSource{document::ParameterValue{std::int64_t{150}}});
    fixture.run<SetParameterSource>(main, fixture.nodeParameter(matteNode, document::kSolidHeightParameterRole),
                                    document::ConstantValueSource{document::ParameterValue{std::int64_t{90}}});
    // The art layer goes on top again, so the matte sits directly below it.
    fixture.run<SetLayerParent>(main, fixture.art, parentLayer);
    fixture.run<SetTrackMatte>(main, fixture.art,
                               document::TrackMatte{matteLayer, document::TrackMatteChannel::Alpha, false, std::nullopt});
    // Its own transform, blend mode and an opacity keyed across the frames.
    fixture.run<SetParameterSource>(main, fixture.layerParameter(fixture.art, document::kScaleParameterRole),
                                    document::ConstantValueSource{document::ParameterValue{document::Vec2d{1.2, 0.85}}});
    fixture.run<SetParameterSource>(main, fixture.layerParameter(fixture.art, document::kRotationParameterRole),
                                    document::ConstantValueSource{document::ParameterValue{-11.0}});
    fixture.run<SetParameterSource>(
        main, fixture.layerParameter(fixture.art, document::kBlendModeParameterRole),
        document::ConstantValueSource{document::ParameterValue{static_cast<std::int64_t>(core::BlendMode::Screen)}});
    const auto opacity = fixture.layerParameter(fixture.art, document::kOpacityParameterRole);
    fixture.run<SetKeyframeAtTimeForParameter>(main, opacity, frame(0), 0.35);
    fixture.run<SetKeyframeAtTimeForParameter>(main, opacity, frame(2), 0.9);
    // A mask on the layer.
    document::PathValue mask;
    document::PathContour contour;
    contour.closed = true;
    for (const auto& point : {document::Vec2d{20, 10}, document::Vec2d{230, 20}, document::Vec2d{210, 130},
                              document::Vec2d{30, 120}})
        contour.anchors.push_back(document::PathAnchor{point, std::nullopt, std::nullopt});
    mask.contours.push_back(contour);
    fixture.run<AddLayerMask>(main, fixture.art, mask);
    // An effect between the source and its slot.
    const auto blur = *fixture.run<AddNode>(main, "bloom.gaussian-blur", document::Vec2d{0, -300})
                           .outputId<document::NodeId>(kAddNodeOutput);
    fixture.run<SetParameterSource>(main, fixture.nodeParameter(blur, "radius"),
                                    document::ConstantValueSource{document::ParameterValue{3.0}});
    const auto* boundary = fixture.composition().graph().findLayer(fixture.art);
    const auto top = boundary->nodeId;
    const auto slot = *boundary->entrySlot;
    fixture.run<ConnectPorts>(main, document::OutputPortRef{source, std::string(document::kLayerStackOutputPort)},
                              document::NodeInputRef{blur, "input"});
    fixture.run<ConnectPorts>(main, document::OutputPortRef{blur, std::string(document::kLayerStackOutputPort)},
                              document::LayerStackInputRef{top, slot, std::string(document::kLayerStackContentInputRole)});
    const auto* edge = document::mergeSlotImageEdge(fixture.composition().graph(), top, slot);
    if (edge == nullptr || edge->source.nodeId != blur)
        throw std::runtime_error("the blur is not between the source and the slot");
}

void layerComposesThroughTheExpansionRoot() {
    const auto file = writeSvg("compose", kComposite);
    Fixture shapes(file, commands::SvgImportMode::Shapes, 256, 144);
    Fixture layer(file, commands::SvgImportMode::Layer, 256, 144);
    dressLayer(shapes, *shapes.rootMerge);
    dressLayer(layer, *layer.artworkNode);
    expect(std::ranges::count_if(layer.composition().graph().nodes(), [](const auto& node) {
               return node.typeId == document::kVectorArtworkNodeType;
           }) == 1,
           "the dressed layer still holds one Vector Artwork node");
    // #642 B2: the dressed layer (scaled 1.2x, turned, masked, a blur between) is drawn at its
    // output scale by default (2x here); with that off, the expansion's root is the editable root
    // exactly. The default picture is the same artwork drawn from more pixels: its coverage is the
    // resampled one's.
    document::VectorArtworkExpansionContext resample;
    resample.composeLayerTransform = false;
    for (const std::int64_t index : {0, 1, 2}) {
        const auto label = "frame " + std::to_string(index);
        const auto editable = renderProject(shapes.document, shapes.main, frame(index), "editable " + label);
        const auto one = renderExpandedWith(layer.document, layer.main, frame(index), "one layer " + label, resample);
        const auto sharp = renderExpanded(layer.document, layer.main, frame(index), "one layer sharp " + label);
        if (!editable || !one || !sharp)
            continue;
        const auto worst = difference(*editable, *one);
        expect(worst <= kGate, label + ": opacity, blend, transform, parent, track matte, mask and effect compose "
                                       "through the expansion's root as through the editable root (worst " +
                                   std::to_string(worst) + ")");
        expect(coverage(*one) > 100.0, label + ": the dressed layer draws");
        expect(std::abs(coverage(*sharp) - coverage(*one)) <= 0.01 * coverage(*one),
               label + ": drawn at its output scale, the dressed layer covers what the resampled one does");
    }
    {
        const auto snapshot = layer.document.snapshot();
        document::VectorArtworkExpansionContext context;
        context.fonts = noFonts();
        const auto expansion =
            document::expandVectorArtwork(snapshot.project(), *snapshot.project().findComposition(layer.main), context);
        const auto found = expansion.oversampled.find(layer.art);
        expect(found != expansion.oversampled.end() && found->second.scale == 2.0,
               "the dressed layer is drawn at 2x (scaled 1.2x and turned)");
    }
    // The opacity key moves the picture: the frames differ, so the comparison above saw animation.
    const auto first = renderExpanded(layer.document, layer.main, frame(0), "frame 0");
    const auto last = renderExpanded(layer.document, layer.main, frame(2), "frame 2");
    expect(first && last && difference(*first, *last) > 1e-3, "the keyed opacity changes the frame");
    std::filesystem::remove(file);
}

void oneLayerIsTheEditableImport() {
    for (const auto& [name, svg] : {std::pair{std::string("composite"), kComposite}, std::pair{std::string("icon"), kIcon}}) {
        const auto file = writeSvg(name, svg);
        for (const bool fit : {true, false}) {
            // Unfitted, the editable import is made so; the layer is imported fitted and switched
            // off afterwards, since fit is the layer's own switch.
            Fixture shapes(file, commands::SvgImportMode::Shapes, 512, 288, fit);
            Fixture layer(file, commands::SvgImportMode::Layer, 512, 288);
            if (!fit)
                layer.run<commands::SetParameterSource>(
                    layer.main, layer.nodeParameter(*layer.artworkNode, document::kVectorArtworkFitParameterRole),
                    document::ConstantValueSource{document::ParameterValue{false}});
            for (const std::int64_t index : {0, 2}) {
                const auto label = name + (fit ? " fitted" : " unfitted") + " frame " + std::to_string(index);
                const auto editable = renderProject(shapes.document, shapes.main, frame(index), "editable " + label);
                const auto one = renderExpanded(layer.document, layer.main, frame(index), "one layer " + label);
                if (!editable || !one)
                    continue;
                const auto worst = difference(*editable, *one);
                expect(worst <= kGate, label + ": the one layer is the editable import, whole frame (worst " +
                                           std::to_string(worst) + ")");
                expect(coverage(*one) > 100.0, label + ": the artwork draws");
            }
        }
        std::filesystem::remove(file);
    }
}

project::ProjectIoOperationMemory memory() {
    auto coordinator = project::ProjectIoMemoryCoordinator::create();
    if (!coordinator)
        throw std::runtime_error("memory coordinator");
    auto operation = coordinator->createOperation();
    if (!operation)
        throw std::runtime_error("memory operation");
    return std::move(*operation);
}

void savedAndReopenedWithTheFileGone() {
    const auto file = writeSvg("reopen", kComposite);
    Fixture layer(file, commands::SvgImportMode::Layer, 256, 144);
    const auto before = renderExpanded(layer.document, layer.main, frame(1), "before saving");
    const auto snapshot = layer.document.snapshot();
    const auto settings = document::makeBloomNeutralColorSettingsV1({});
    auto saved = project::buildVerifiedSaveArchive({}, {.snapshot = &snapshot, .colorSettings = &settings}, {},
                                                   memory());
    expect(static_cast<bool>(saved) && saved.archive() != nullptr, "the project saves and verifies");
    if (!saved || !saved.archive())
        return;
    // The source file is deleted before the project opens again.
    std::filesystem::remove(file);
    auto opened = project::openProjectArchive(saved.archive()->bytes(), {}, memory());
    expect(opened.outcome() == project::OpenArchiveOutcome::Opened, "the project opens with its SVG gone");
    if (opened.outcome() != project::OpenArchiveOutcome::Opened)
        return;
    auto reopened = std::move(opened).takeOpened();
    const auto reopenedSnapshot = reopened.document->snapshot();
    const auto& project = reopenedSnapshot.project();
    const auto* composition = project.findComposition(layer.main);
    expect(composition != nullptr, "the composition reopens");
    if (composition == nullptr)
        return;
    const auto* node = composition->graph().findNode(*layer.artworkNode);
    expect(node != nullptr && node->typeId == document::kVectorArtworkNodeType &&
               std::ranges::none_of(composition->graph().nodes(),
                                    [](const auto& candidate) { return candidate.typeId == document::kPathValueNodeType; }),
           "the layer reopens as one layer: the mode is kept");
    const auto* block = node ? document::vectorArtworkBlock(project, *node, composition->parameters()) : nullptr;
    const auto* original = document::vectorArtworkBlock(snapshot.project(), *snapshot.project().findComposition(layer.main)->graph().findNode(*layer.artworkNode),
                                                         snapshot.project().findComposition(layer.main)->parameters());
    expect(block != nullptr && original != nullptr && *block == *original,
           "the SVG's bytes, digest and locator reopen exactly");
    expect(block != nullptr && document::vectorArtworkSourceAsset(project, *block) != nullptr,
           "the Assets record of the source reopens, its file missing and no error");
    const auto after = renderExpanded(*reopened.document, layer.main, frame(1), "after reopening");
    expect(before && after && difference(*before, *after) == 0.0 && coverage(*after) > 100.0,
           "the reopened layer renders the same frame with its source file deleted");
}

// ---- #642 package B: the compiler's expansion ----

// A plan and the CPU reference frame of one composition at a time (the operation cache bypassed).
struct Evaluated final {
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    std::shared_ptr<const runtime::ProcessFrame> frame;
};

runtime::EvaluationRequest referenceRequest(const runtime::CompiledCompositionPlan& plan, const core::RationalTime time) {
    runtime::EvaluationRequest request{.time = time,
                                       .output = plan.output(),
                                       .resolution = runtime::CompositionFormatResolution{},
                                       .quality = runtime::EvaluationQuality::Reference,
                                       .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                       .pixelStorageByteLimit = std::size_t{1} << 30};
    request.bypassOperationCache = true;
    return request;
}

std::optional<Evaluated> evaluate(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
                                  const core::RationalTime time, const std::string& label) {
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(plan, referenceRequest(*plan, time), {});
    expect(result.frame() != nullptr, label + ": the CPU reference evaluates");
    if (result.frame() == nullptr)
        return std::nullopt;
    return Evaluated{plan, result.frame()};
}

Frame pixelsOf(const runtime::ProcessFrame& frame) {
    const auto& image = frame.processImage();
    const auto pixels = image.pixels();
    return Frame{{pixels.begin(), pixels.end()}, static_cast<std::int64_t>(image.descriptor()->dataWindow().extent().width())};
}

std::shared_ptr<const runtime::CompiledCompositionPlan> compilePlan(const document::Document& document,
                                                                    const document::CompositionId id,
                                                                    const std::string& label) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({document.snapshot(), id}, {});
    expect(compiled.plan != nullptr,
           label + ": compiles: " + runtime::describeCompileFailure(compiled, document.snapshot().project(), id));
    return compiled.plan;
}

const runtime::EvaluatedOperationBounds* boundsOf(const runtime::ProcessFrame& frame, const document::LayerId layer) {
    for (const auto& bounds : frame.evaluatedBounds())
        if (bounds.layerId == layer)
            return &bounds;
    return nullptr;
}

// Pixels any channel of which differs by more than `tolerance`.
std::size_t differingPixels(const Frame& a, const Frame& b, const double tolerance) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < std::min(a.pixels.size(), b.pixels.size()); ++i)
        for (std::size_t c = 0; c < 4; ++c)
            if (std::abs(static_cast<double>(a.pixels[i].components()[c]) - static_cast<double>(b.pixels[i].components()[c])) >
                tolerance) {
                ++count;
                break;
            }
    return count;
}

// Pixels whose alpha is neither empty nor full: the width of the artwork's edges.
std::size_t partialPixels(const Frame& frame) {
    std::size_t count = 0;
    for (const auto& pixel : frame.pixels)
        if (pixel.alpha() > 0.02F && pixel.alpha() < 0.98F)
            ++count;
    return count;
}

// The editable import's own transform: its root layer's scale and rotation about the frame centre.
void setTransform(Fixture& fixture, const document::LayerId layer, const document::Vec2d scale, const double degrees) {
    fixture.run<commands::SetParameterSource>(fixture.main, fixture.layerParameter(layer, document::kScaleParameterRole),
                                              document::ConstantValueSource{document::ParameterValue{scale}});
    // An edit that changes nothing is no transaction.
    if (degrees != 0.0)
        fixture.run<commands::SetParameterSource>(fixture.main, fixture.layerParameter(layer, document::kRotationParameterRole),
                                                  document::ConstantValueSource{document::ParameterValue{degrees}});
}

// Flat shapes, a compound path and a stroke: no gradient (a gradient's own image is resampled by
// design, so it is not an edge) and no text (fonts are the machine's).
std::string sharpSvg(const std::string& transform, const bool stroke = true) {
    return R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80"><g transform=")svg" +
           transform + R"svg(">
<rect x="8" y="8" width="40" height="28" fill="#e33"/>
<circle cx="84" cy="24" r="16" fill="#3a6"/>
<path d="M14 46 H58 V74 H14 Z M26 54 H46 V66 H26 Z" fill="#36c" fill-rule="evenodd"/>
</g>)svg" + std::string(stroke ? R"svg(<g transform=")svg" + transform + R"svg("><path d="M66 70 L90 46 L112 70" fill="none" stroke="#fd0" stroke-width="3" stroke-linejoin="round"/></g>)svg" : "") + "</svg>";
}

// Scaled up and turned, the layer is the editable import of the same file with that transform baked
// into its points (not a resampled picture), whole frame, on the CPU reference.
void sharpWhenScaledAndTurned() {
    struct Case final {
        std::string name;
        document::Vec2d scale;
        double degrees;
        std::string baked;
        // A stroke under a stretch is drawn as transformed geometry by the layer, while the editable
        // import gives a baked stroke one width (its anisotropic widths are an approximation it
        // names), so the stretched case compares filled outlines only.
        bool stroke = true;
    };
    // Unfitted, the file's viewport is centred on the frame, so the layer's frame-pivot transform
    // turns about the file's centre (60, 40), which is where the baked transform turns.
    const std::vector<Case> cases{
        {"scaled 3x", {3.0, 3.0}, 0.0, "translate(60 40) scale(3) translate(-60 -40)"},
        {"scaled 2.2x and turned 27 degrees", {2.2, 2.2}, 27.0, "translate(60 40) rotate(27) scale(2.2) translate(-60 -40)"},
        {"stretched and turned -40 degrees", {2.5, 1.6}, -40.0, "translate(60 40) rotate(-40) scale(2.5 1.6) translate(-60 -40)", false}};
    for (const auto& item : cases) {
        const auto plain = writeSvg("sharp", sharpSvg("", item.stroke));
        const auto bakedFile = writeSvg("sharp-baked", sharpSvg(item.baked, item.stroke));
        Fixture layer(plain, commands::SvgImportMode::Layer, 512, 288);
        layer.run<commands::SetParameterSource>(
            layer.main, layer.nodeParameter(*layer.artworkNode, document::kVectorArtworkFitParameterRole),
            document::ConstantValueSource{document::ParameterValue{false}});
        setTransform(layer, layer.art, item.scale, item.degrees);
        Fixture baked(bakedFile, commands::SvgImportMode::Shapes, 512, 288, false);
        const auto composed = renderExpanded(layer.document, layer.main, frame(0), item.name + " one layer");
        const auto reference = renderProject(baked.document, baked.main, frame(0), item.name + " baked shapes");
        document::VectorArtworkExpansionContext resample;
        resample.composeLayerTransform = false;
        const auto resampled = renderExpandedWith(layer.document, layer.main, frame(0), item.name + " resampled", resample);
        std::filesystem::remove(bakedFile);
        std::filesystem::remove(plain);
        if (!composed || !reference || !resampled)
            continue;
        const auto worst = difference(*composed, *reference);
        const auto moved = differingPixels(*composed, *reference, 1e-3);
        const auto edges = partialPixels(*composed);
        const auto bakedEdges = partialPixels(*reference);
        const auto softEdges = partialPixels(*resampled);
        std::cout << item.name << ": worst difference to the baked import " << worst << " on " << moved
                  << " pixels, edge pixels " << edges << " (baked " << bakedEdges << ", resampled " << softEdges << ")\n";
        // The same outlines covered through the same matrix. Fills match exactly; a stroke's round
        // join is built in the shape's own units and then transformed, while the baked import
        // builds it at the scaled width, so a pixel or two on a join may differ by a few coverage
        // codes. Never the spread of a resample.
        expect(worst <= 0.1 && moved * 100 <= edges,
               item.name + ": the layer is the baked editable import (worst " + std::to_string(worst) + " on " +
                   std::to_string(moved) + " pixels)");
        expect(edges <= bakedEdges + bakedEdges / 50 + 4,
               item.name + ": its edges are as narrow as the baked import's (" + std::to_string(edges) + " against " +
                   std::to_string(bakedEdges) + ")");
        expect(softEdges > edges + edges / 2,
               item.name + ": a resampled picture would be visibly softer (" + std::to_string(softEdges) + " edge pixels)");
        expect(coverage(*composed) > 1000.0, item.name + ": the artwork draws");
    }
}

// The layer reports its own box: the artwork's content bounds through its transform, as the
// resampled layer does, so the Viewer's box, handles and gestures see the same layer.
void boundsFollowTheLayer() {
    const auto file = writeSvg("bounds", sharpSvg(""));
    for (const auto& [scale, degrees] : {std::pair{document::Vec2d{1.8, 1.8}, 0.0}, std::pair{document::Vec2d{1.5, 0.75}, 33.0}}) {
        Fixture layer(file, commands::SvgImportMode::Layer, 512, 288);
        setTransform(layer, layer.art, scale, degrees);
        layer.run<commands::SetParameterSource>(layer.main, layer.layerParameter(layer.art, document::kPositionParameterRole),
                                                document::ConstantValueSource{document::ParameterValue{document::Vec2d{270.0, 150.0}}});
        const auto plan = compilePlan(layer.document, layer.main, "bounds");
        const auto evaluated = plan ? evaluate(plan, frame(0), "bounds") : std::nullopt;
        if (!evaluated)
            continue;
        const auto* bounds = boundsOf(*evaluated->frame, layer.art);
        expect(bounds != nullptr && !bounds->local.empty(), "bounds: the layer publishes its bounds");
        if (bounds == nullptr || bounds->local.empty())
            continue;
        // The polygon's edges are the layer's linear map applied to its local box.
        const double width = bounds->local.right - bounds->local.left;
        const double height = bounds->local.bottom - bounds->local.top;
        const document::Vec2d x{(bounds->polygon[1].x - bounds->polygon[0].x) / width,
                                (bounds->polygon[1].y - bounds->polygon[0].y) / width};
        const document::Vec2d y{(bounds->polygon[3].x - bounds->polygon[0].x) / height,
                                (bounds->polygon[3].y - bounds->polygon[0].y) / height};
        const double radians = degrees * 3.14159265358979323846 / 180.0;
        const bool linear = std::abs(x.x - std::cos(radians) * scale.x) < 1e-9 &&
                            std::abs(x.y - std::sin(radians) * scale.x) < 1e-9 &&
                            std::abs(y.x + std::sin(radians) * scale.y) < 1e-9 &&
                            std::abs(y.y - std::cos(radians) * scale.y) < 1e-9;
        expect(linear, "bounds: the polygon carries the layer's scale and rotation");
        expect(std::abs(bounds->anchor.x - 270.0) < 1e-9 && std::abs(bounds->anchor.y - 150.0) < 1e-9,
               "bounds: the anchor is where the layer's position puts it");
        // The local box is the artwork's own (untransformed) box: the box the same layer reports at
        // the identity, to within the pixel its data window rounds to.
        {
            Fixture identity(file, commands::SvgImportMode::Layer, 512, 288);
            const auto identityPlan = compilePlan(identity.document, identity.main, "bounds identity");
            const auto identityFrame = identityPlan ? evaluate(identityPlan, frame(0), "bounds identity") : std::nullopt;
            const auto* own = identityFrame ? boundsOf(*identityFrame->frame, identity.art) : nullptr;
            expect(own != nullptr && std::abs(own->local.left - bounds->local.left) <= 1.0 &&
                       std::abs(own->local.top - bounds->local.top) <= 1.0 &&
                       std::abs(own->local.right - bounds->local.right) <= 1.0 &&
                       std::abs(own->local.bottom - bounds->local.bottom) <= 1.0 && width > 100 && height > 100,
                   "bounds: the local box is the artwork's own box");
        }
        // The resampled layer reports the same box through the same matrix (its content is the
        // artwork's picture, so its box is that picture's data window).
        document::VectorArtworkExpansionContext resample;
        resample.composeLayerTransform = false;
        const auto snapshot = layer.document.snapshot();
        resample.fonts = noFonts();
        auto expansion = document::expandVectorArtwork(snapshot.project(), *snapshot.project().findComposition(layer.main), resample);
        document::Project project = snapshot.project();
        *project.findComposition(layer.main) = *expansion.composition;
        const document::Document expanded(std::move(project));
        const auto resampledPlan = compilePlan(expanded, layer.main, "bounds resampled");
        const auto resampled = resampledPlan ? evaluate(resampledPlan, frame(0), "bounds resampled") : std::nullopt;
        const auto* other = resampled ? boundsOf(*resampled->frame, layer.art) : nullptr;
        if (other == nullptr)
            continue;
        double worst = 0.0;
        for (std::size_t i = 0; i < 4; ++i)
            worst = std::max({worst, std::abs(other->polygon[i].x - bounds->polygon[i].x),
                              std::abs(other->polygon[i].y - bounds->polygon[i].y)});
        expect(worst <= 1.5 * std::max(scale.x, scale.y),
               "bounds: the polygon is the resampled layer's within a pixel of the picture's edge (" +
                   std::to_string(worst) + ")");
        expect(std::abs(other->anchor.x - bounds->anchor.x) < 1e-9 && std::abs(other->anchor.y - bounds->anchor.y) < 1e-9,
               "bounds: the anchor is the resampled layer's");
    }
    std::filesystem::remove(file);
}

// A token whose task was cancelled (the scheduler's own cancellation, as a preview or an export
// sees it).
runtime::CancellationToken cancelledToken() {
    using namespace runtime;
    TaskSchedulerConfig config = TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    CancellationToken token;
    auto submission = scheduler.submit<void>(
        TaskRequest("svg layer cancel token", {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(642)}),
        [&](TaskContext& context) {
            {
                std::lock_guard lock(mutex);
                token = context.cancellation();
                entered = true;
            }
            condition.notify_all();
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] { return release; });
            return TaskResult<void>::succeeded();
        });
    if (!submission.accepted())
        throw std::logic_error("cancel token task was refused");
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return entered; });
    }
    submission.handle.cancel();
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!submission.handle.tryTakeResult().has_value() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return token;
}

const document::DataBlockRecord& blockOf(const Fixture& fixture) {
    const auto& composition = fixture.composition();
    const auto* node = composition.graph().findNode(*fixture.artworkNode);
    const auto* block = node ? document::vectorArtworkBlock(fixture.document.snapshot().project(), *node, composition.parameters())
                             : nullptr;
    if (block == nullptr)
        throw std::runtime_error("the layer holds no SVG block");
    return *block;
}

// One plan per revision; an animated transform renders every frame from it. A block is parsed once
// per digest: a new revision, another layer of the same file and an edited parameter read the kept
// scene; a different file is parsed.
void oneCompilePerRevisionAndOneParsePerFile() {
    const auto file = writeSvg("identity", kComposite);
    const auto other = writeSvg("identity-other", kIcon);
    Fixture layer(file, commands::SvgImportMode::Layer, 256, 144);
    auto& scenes = runtime::processVectorArtworkSceneCache();
    scenes.retire();
    const auto before = scenes.statistics();
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    runtime::CompiledPlanCache plans;
    const auto compile = [&](const Fixture& fixture) {
        return plans.compile(compiler, {fixture.document.snapshot(), fixture.main}, {});
    };
    // An animated transform and opacity.
    const auto key = [&](const std::string_view role, const auto first, const auto last) {
        // The first key holds the value the constant already has, so the moved key goes in first.
        const auto parameter = layer.layerParameter(layer.art, role);
        layer.run<commands::SetKeyframeAtTimeForParameter>(layer.main, parameter, frame(2), last);
        layer.run<commands::SetKeyframeAtTimeForParameter>(layer.main, parameter, frame(0), first);
    };
    key(document::kScaleParameterRole, document::Vec2d{1, 1}, document::Vec2d{1.6, 1.3});
    key(document::kRotationParameterRole, 0.0, 24.0);
    key(document::kOpacityParameterRole, 1.0, 0.4);
    const auto keyed = compile(layer);
    expect(keyed.plan != nullptr, "identity: the keyed layer compiles");
    const auto afterFirst = scenes.statistics();
    expect(afterFirst.parses == before.parses + 1 && afterFirst.entries == 1,
           "identity: the first compile parses the block once and keeps it");
    std::vector<Frame> frames;
    for (const std::int64_t index : {0, 1, 2})
        if (const auto evaluated = keyed.plan ? evaluate(keyed.plan, frame(index), "identity") : std::nullopt)
            frames.push_back(pixelsOf(*evaluated->frame));
    expect(frames.size() == 3 && difference(frames[0], frames[1]) > 1e-3 && difference(frames[1], frames[2]) > 1e-3,
           "identity: the animated transform changes every frame");
    const auto again = compile(layer);
    expect(again.plan == keyed.plan && plans.statistics().compiles == 1 && plans.statistics().hits == 1,
           "identity: the same revision is one plan; animating the transform never compiles again");
    // The keyed transform lives on the carrier, by the layer's own parameter ids: one plan holds
    // the curves, and a Viewer gesture's override of the layer's scale moves the artwork.
    {
        runtime::SnapshotCompileRequest request{layer.document.snapshot(), layer.main};
        request.parameterOverrides.push_back({layer.document.snapshot().revision(),
                                              layer.layerParameter(layer.art, document::kPositionParameterRole),
                                              document::Vec2d{40.0, 30.0}});
        const auto overridden = compiler.compile(request, {});
        const auto moved = overridden.plan ? evaluate(overridden.plan, frame(0), "identity override") : std::nullopt;
        expect(moved && !frames.empty() && difference(pixelsOf(*moved->frame), frames[0]) > 1e-3,
               "identity: an override of the layer's position (a Viewer drag) moves the artwork");
    }

    // A new revision of the same file: compiled again, never parsed again.
    layer.run<commands::SetParameterSource>(layer.main, layer.layerParameter(layer.art, document::kBlendModeParameterRole),
                                            document::ConstantValueSource{document::ParameterValue{
                                                static_cast<std::int64_t>(core::BlendMode::Screen)}});
    const auto edited = compile(layer);
    expect(edited.plan != nullptr && edited.plan != keyed.plan && plans.statistics().compiles == 2,
           "identity: an edited layer is a new plan");
    expect(scenes.statistics().parses == afterFirst.parses && scenes.statistics().hits > afterFirst.hits,
           "identity: a new revision reads the kept scene");
    // The node's own parameter (fit) is a miss of the plan and a different frame, but not a parse.
    const auto fitted = edited.plan ? evaluate(edited.plan, frame(0), "fitted") : std::nullopt;
    layer.run<commands::SetParameterSource>(
        layer.main, layer.nodeParameter(*layer.artworkNode, document::kVectorArtworkFitParameterRole),
        document::ConstantValueSource{document::ParameterValue{false}});
    const auto unfitted = compile(layer);
    const auto unfittedFrame = unfitted.plan ? evaluate(unfitted.plan, frame(0), "unfitted") : std::nullopt;
    expect(unfitted.plan != nullptr && unfitted.plan != edited.plan && fitted && unfittedFrame &&
               difference(pixelsOf(*fitted->frame), pixelsOf(*unfittedFrame->frame)) > 1e-3,
           "identity: the fit switch is a new plan and a new picture");
    expect(scenes.statistics().parses == afterFirst.parses, "identity: switching fit parses nothing");
    // A second layer of the same file shares the block and the scene; another file is parsed.
    layer.run<commands::AddSvgArtwork>(layer.main, nullptr, file, "again", true, commands::SvgImportMode::Layer);
    const auto twice = compile(layer);
    expect(twice.plan != nullptr && scenes.statistics().parses == afterFirst.parses,
           "identity: a second layer of the same file reads the same scene");
    layer.run<commands::AddSvgArtwork>(layer.main, nullptr, other, "icon", true, commands::SvgImportMode::Layer);
    const auto otherFile = compile(layer);
    expect(otherFile.plan != nullptr && scenes.statistics().parses == afterFirst.parses + 1 &&
               scenes.statistics().entries == 2,
           "identity: a different file is parsed once and kept beside the first");
    std::filesystem::remove(file);
    std::filesystem::remove(other);
}

// Cancellation reaches the expansion: a cancelled compile publishes nothing, a cancelled parse
// keeps nothing, and a cancelled evaluation of the expanded plan publishes no frame.
void cancellation() {
    const auto file = writeSvg("cancel", kComposite);
    Fixture layer(file, commands::SvgImportMode::Layer, 256, 144);
    const auto token = cancelledToken();
    expect(token.isCancellationRequested(), "cancel: the token is cancelled");
    runtime::processVectorArtworkSceneCache().retire();
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({layer.document.snapshot(), layer.main}, token);
    expect(compiled.status == runtime::SnapshotCompileStatus::Cancelled && compiled.plan == nullptr,
           "cancel: a cancelled compile of a Vector Artwork layer reports Cancelled and no plan");
    expect(runtime::processVectorArtworkSceneCache().statistics().entries == 0,
           "cancel: a cancelled compile keeps no scene");
    // A cancelled parse keeps nothing.
    runtime::VectorArtworkSceneCache scenes;
    const auto lookup = scenes.scene(blockOf(layer), [] { return true; });
    expect(lookup.cancelled && lookup.scene == nullptr && scenes.statistics().entries == 0 &&
               scenes.statistics().parses == 0,
           "cancel: a cancelled parse keeps nothing");
    const auto plan = compilePlan(layer.document, layer.main, "cancel");
    if (plan) {
        const auto result = runtime::CpuCompositionEvaluator{}.evaluate(plan, referenceRequest(*plan, frame(0)), token);
        expect(result.status() == runtime::EvaluationStatus::Cancelled && result.frame() == nullptr,
               "cancel: a cancelled evaluation of the expanded layer publishes no frame");
    }
    std::filesystem::remove(file);
}

// The scene cache is bounded: scenes count against a byte budget, one larger than a quarter of it
// is parsed but never kept, the least recently used is retired to make room, and retire() empties
// it. The render itself keeps the request's pixel budget.
void budgetAdmissionAndRetirement() {
    const auto composite = writeSvg("budget-composite", kComposite);
    const auto icon = writeSvg("budget-icon", kIcon);
    const auto sharp = writeSvg("budget-sharp", sharpSvg(""));
    Fixture a(composite, commands::SvgImportMode::Layer, 256, 144);
    Fixture b(icon, commands::SvgImportMode::Layer, 256, 144);
    Fixture c(sharp, commands::SvgImportMode::Layer, 256, 144);
    runtime::VectorArtworkSceneCache probe;
    const auto sizeOf = [&](const Fixture& fixture) {
        const auto lookup = probe.scene(blockOf(fixture));
        return lookup.scene ? runtime::VectorArtworkSceneCache::estimatedBytes(*lookup.scene) : std::size_t{0};
    };
    const auto sa = sizeOf(a), sb = sizeOf(b), sc = sizeOf(c);
    expect(sa > 0 && sb > 0 && sc > 0, "budget: every scene has a size");
    // Room for two of the three; each well under a quarter of it.
    const auto largest = std::max({sa, sb, sc});
    runtime::VectorArtworkSceneCache scenes(std::max(4 * largest, sa + sb + sc - std::min({sa, sb, sc}) / 2));
    const auto budget = scenes.statistics().byteBudget;
    const bool allFit = sa + sb + sc <= budget;
    static_cast<void>(scenes.scene(blockOf(a)));
    static_cast<void>(scenes.scene(blockOf(b)));
    static_cast<void>(scenes.scene(blockOf(a))); // a is now the most recently used
    static_cast<void>(scenes.scene(blockOf(c)));
    auto stats = scenes.statistics();
    expect(stats.retainedBytes <= stats.byteBudget, "budget: the kept scenes stay within the budget");
    expect(allFit ? stats.entries == 3 : stats.entries == 2 && stats.retirements == 1,
           "budget: the least recently used scene is retired to make room");
    const auto hits = stats.hits;
    static_cast<void>(scenes.scene(blockOf(a)));
    expect(scenes.statistics().hits == hits + 1, "budget: the most recently used scene was kept");
    // A budget below four times a scene refuses to keep it, and still answers.
    runtime::VectorArtworkSceneCache tiny(sa * 2);
    const auto refused = tiny.scene(blockOf(a));
    expect(refused.scene != nullptr && tiny.statistics().refusedAdmissions == 1 && tiny.statistics().entries == 0,
           "admission: a scene too large to keep is parsed for the compile and not kept");
    // Retirement: everything goes, and a later compile parses again.
    scenes.retire();
    stats = scenes.statistics();
    expect(stats.entries == 0 && stats.retainedBytes == 0, "retirement: retire() drops every scene");
    static_cast<void>(scenes.scene(blockOf(b)));
    expect(scenes.statistics().misses == stats.misses + 1, "retirement: a retired scene is parsed again");
    // Shrinking the budget retires down to it.
    scenes.setByteBudget(0);
    expect(scenes.statistics().entries == 0, "retirement: a smaller budget retires what no longer fits");

    // The render keeps the request's pixel budget: too small a request is refused by name.
    const auto plan = compilePlan(a.document, a.main, "budget");
    if (plan) {
        auto request = referenceRequest(*plan, frame(0));
        request.pixelStorageByteLimit = 4096;
        const auto result = runtime::CpuCompositionEvaluator{}.evaluate(plan, request, {});
        std::string why;
        for (const auto& diagnostic : result.diagnostics())
            why += diagnostic.summary + " " + diagnostic.detail + " ";
        expect(result.frame() == nullptr && result.status() == runtime::EvaluationStatus::Failed && !why.empty(),
               "budget: a request too small for the expanded artwork is refused, with a reason: " + why);
    }
    for (const auto& path : {composite, icon, sharp})
        std::filesystem::remove(path);
}

// An SVG that cannot be read is refused by the compiler by name, never drawn empty.
void unreadableArtworkIsRefusedByName() {
    const auto file = writeSvg("unreadable", kIcon);
    Fixture layer(file, commands::SvgImportMode::Layer, 256, 144);
    document::Project project = layer.document.snapshot().project();
    const auto& block = blockOf(layer);
    auto broken = block;
    std::string garbage = "<svg xmlns=\"http://www.w3.org/2000/svg\"><rect";
    std::vector<std::byte> bytes;
    for (const char c : garbage)
        bytes.push_back(static_cast<std::byte>(c));
    broken.payload = document::OpaqueExtensionPayload(bytes);
    broken.provenance.contentDigest = *core::Sha256Hasher::hash(bytes);
    auto* stored = project.findDataBlock(std::get<document::DataBlockRecordId>(block.id));
    expect(stored != nullptr, "unreadable: the block is found");
    if (stored == nullptr)
        return;
    *stored = broken;
    const document::Document document(std::move(project));
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({document.snapshot(), layer.main}, {});
    const auto described = runtime::describeCompileFailure(compiled, document.snapshot().project(), layer.main);
    expect(compiled.plan == nullptr && described.find("Vector Artwork cannot be drawn") != std::string::npos,
           "unreadable: the compile is refused, naming the Vector Artwork: " + described);
    bool named = false;
    for (const auto& diagnostic : compiled.diagnostics)
        named = named || (diagnostic.subject.nodeId == layer.artworkNode && !diagnostic.detail.empty() &&
                          diagnostic.detail.find("not expanded") == std::string::npos);
    expect(named, "unreadable: the refusal names the node and why the SVG does not read");
    std::filesystem::remove(file);
}

// Which layers keep the resampled picture, and why; what is parented to a composed layer follows
// it exactly.
void whatResamplesAndWhatFollows() {
    const auto file = writeSvg("follows", sharpSvg(""));
    const auto expansionOf = [](const Fixture& fixture) {
        document::VectorArtworkExpansionContext context;
        context.fonts = noFonts();
        const auto snapshot = fixture.document.snapshot();
        return document::expandVectorArtwork(snapshot.project(), *snapshot.project().findComposition(fixture.main), context);
    };
    {
        Fixture plain(file, commands::SvgImportMode::Layer, 256, 144);
        const auto expansion = expansionOf(plain);
        const auto composed = expansion.composed.find(plain.art);
        expect(composed != expansion.composed.end() && composed->second.leaves.size() == 4 && expansion.resampled.empty(),
               "composed: a plain layer draws its transform through its four shapes");
        if (composed != expansion.composed.end() && expansion.composition) {
            const auto* carrier = expansion.composition->graph().findLayer(composed->second.carrier);
            const auto* art = expansion.composition->graph().findLayer(plain.art);
            const auto* carrierNode = carrier ? expansion.composition->graph().findNode(carrier->nodeId) : nullptr;
            const auto bound = [&](const document::LayerOutputBoundary& layer) {
                return document::layerParameterBinding(expansion.composition->graph(), layer, document::kScaleParameterRole)->parameterId;
            };
            expect(carrierNode != nullptr && carrierNode->typeId == document::kLayerOutputNodeType && art != nullptr &&
                       bound(*carrier) == plain.layerParameter(plain.art, document::kScaleParameterRole) &&
                       bound(*art) != bound(*carrier),
                   "composed: the carrier holds the layer's own transform parameters; the layer draws at the identity");
        }
    }
    {
        Fixture masked(file, commands::SvgImportMode::Layer, 256, 144);
        document::PathValue mask;
        document::PathContour contour;
        contour.closed = true;
        for (const auto& point : {document::Vec2d{60, 40}, document::Vec2d{200, 40}, document::Vec2d{200, 110}})
            contour.anchors.push_back(document::PathAnchor{point, std::nullopt, std::nullopt});
        mask.contours.push_back(contour);
        masked.run<commands::AddLayerMask>(masked.main, masked.art, mask);
        // #642 B2: at the identity it needs no more pixels than the composition's; scaled up it is
        // drawn at its output scale (VectorArtworkOversampledLayer) rather than resampled.
        const auto identity = expansionOf(masked);
        expect(identity.composed.empty() && identity.oversampled.empty() && identity.resampled.empty(),
               "a masked layer at the identity is drawn at the composition's scale, which is its output scale");
        setTransform(masked, masked.art, {2.0, 2.0}, 0.0);
        const auto expansion = expansionOf(masked);
        const auto found = expansion.oversampled.find(masked.art);
        expect(expansion.composed.empty() && expansion.resampled.empty() && found != expansion.oversampled.end() &&
                   found->second.scale == 2.0,
               "oversampled: a masked layer scaled 2x draws its artwork at 2x instead of resampling it");
    }
    {
        Fixture retimed(file, commands::SvgImportMode::Layer, 256, 144);
        retimed.run<commands::SetLayerTiming>(retimed.main, retimed.art, frame(1), core::RationalTime{},
                                              core::RationalTime::fromInteger(4));
        retimed.run<commands::SetParameterSource>(
            retimed.main, retimed.layerParameter(retimed.art, document::kRotationParameterRole),
            document::ConstantValueSource{document::ParameterValue{12.0}});
        const auto expansion = expansionOf(retimed);
        const auto found = expansion.oversampled.find(retimed.art);
        expect(expansion.composed.empty() && expansion.resampled.empty() && found != expansion.oversampled.end() &&
                   found->second.scale == 2.0,
               "oversampled: a retimed layer that turns draws its artwork at 2x instead of resampling it");
        expect(static_cast<bool>(renderExpanded(retimed.document, retimed.main, frame(2), "retimed")),
               "oversampled: the retimed layer renders");
    }
    {
        // A Layer node between the artwork and its slot (it adopts the entry's values, pivot
        // included) is composed too, and a solid parented to it -- scaled, turned, moving -- lands
        // exactly where it would on the resampled layer (the artwork itself is drawn at zero
        // opacity, so only the child is seen). An entry layer cannot have a child in its own
        // Merge (the parent link would be a cycle), so the Layer node is where an artist parents.
        Fixture parent(file, commands::SvgImportMode::Layer, 256, 144);
        const auto* entry = parent.composition().graph().findLayer(parent.art);
        const auto merge = entry->nodeId;
        const auto slot = *entry->entrySlot;
        const auto layerNode = *parent.run<commands::AddNode>(parent.main, std::string(document::kLayerOutputNodeType),
                                                              document::Vec2d{0, -200})
                                    .outputId<document::NodeId>(commands::kAddNodeOutput);
        parent.run<commands::ConnectPorts>(parent.main,
                                           document::OutputPortRef{*parent.artworkNode, std::string(document::kVectorArtworkOutputPort)},
                                           document::NodeInputRef{layerNode, std::string(document::kLayerOutputContentInputPort)});
        parent.run<commands::ConnectPorts>(parent.main,
                                           document::OutputPortRef{layerNode, std::string(document::kLayerOutputOutputPort)},
                                           document::LayerStackInputRef{merge, slot, std::string(document::kLayerStackContentInputRole)});
        std::optional<document::LayerId> held;
        for (const auto& boundary : parent.composition().graph().layerOutputs())
            if (!boundary.entryOwned() && boundary.nodeId == layerNode)
                held = boundary.layerId;
        expect(held.has_value(), "follows: the Layer node holds the artwork's layer");
        if (!held)
            return;
        setTransform(parent, *held, {1.4, 0.8}, 21.0);
        const auto position = parent.layerParameter(*held, document::kPositionParameterRole);
        parent.run<commands::SetKeyframeAtTimeForParameter>(parent.main, position, frame(2), document::Vec2d{150, 60});
        parent.run<commands::SetKeyframeAtTimeForParameter>(parent.main, position, frame(0), document::Vec2d{128, 72});
        parent.run<commands::SetParameterSource>(parent.main, parent.layerParameter(*held, document::kOpacityParameterRole),
                                                 document::ConstantValueSource{document::ParameterValue{0.0}});
        const auto child = parent.run<commands::AddSolidLayer>(parent.main, "Child", core::Color4d{1, 0.5, 0, 1},
                                                               document::Vec2d{180, 60}, 1.0);
        const auto childLayer = *child.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
        parent.run<commands::SetLayerParent>(parent.main, childLayer, *held);
        const auto expansion = expansionOf(parent);
        expect(expansion.composed.contains(*held), "follows: the Layer node's layer is composed");
        for (const std::int64_t index : {0, 2}) {
            document::VectorArtworkExpansionContext resample;
            resample.composeLayerTransform = false;
            const auto composed = renderExpanded(parent.document, parent.main, frame(index), "child composed");
            const auto resampled = renderExpandedWith(parent.document, parent.main, frame(index), "child resampled", resample);
            expect(composed && resampled && difference(*composed, *resampled) == 0.0 && coverage(*composed) > 100.0,
                   "follows: a layer parented to the artwork's layer moves exactly as before, frame " + std::to_string(index));
        }
    }
    std::filesystem::remove(file);
}

// ---- #642 package B2: a layer the composition changes stays sharp ----
//
// A layer that pivots on its content, has masks or its own timing, or reads the artwork through
// effects is drawn as a pre-composition `factor` times larger (VectorArtworkOversampledLayer). Its
// reference is built independently through the editable import: the file with its points `factor`
// times larger about the composition's origin, the same edits with every length times `factor`,
// and the root layer's own matrix times a scale of 1 / factor (its scale divided, its pivot
// multiplied). That is the scaled pre-composition by definition; the one layer must equal it, and
// the resampled picture (composeLayerTransform off) must be visibly softer.

constexpr std::uint32_t kB2Width = 512;
constexpr std::uint32_t kB2Height = 288;
const document::Vec2d kB2Centre{kB2Width / 2.0, kB2Height / 2.0};

// The file with its points `factor` times larger about the composition's origin, imported unfitted:
// the 120 x 80 viewport is centred, a point p landing at p + o, o = (196, 104), and factor * (p + o)
// = (factor * p + (factor - 1) * o) + o.
std::string bakedSharpSvg(const double factor, const bool stroke = true) {
    const double ox = (kB2Width - 120.0) / 2.0 * (factor - 1.0);
    const double oy = (kB2Height - 80.0) / 2.0 * (factor - 1.0);
    return sharpSvg("translate(" + std::to_string(ox) + " " + std::to_string(oy) + ") scale(" + std::to_string(factor) + ")",
                    stroke);
}

void unfit(Fixture& fixture) {
    fixture.run<commands::SetParameterSource>(
        fixture.main, fixture.nodeParameter(*fixture.artworkNode, document::kVectorArtworkFitParameterRole),
        document::ConstantValueSource{document::ParameterValue{false}});
}

struct LayerEdits final {
    document::Vec2d scale{1.0, 1.0};
    double degrees = 0.0;
    bool mask = false;
    bool retime = false;
    // A Gaussian Blur then a Drop Shadow between the artwork and the slot.
    bool effects = false;
    // A Hue/Saturation node between (no length of its own).
    bool colour = false;
    // Parented to a null that scales by this (zero opacity).
    double parentScale = 0.0;
};

void setLayerValue(Fixture& fixture, const document::LayerId layer, const std::string_view role,
                   const document::ParameterValue value) {
    const auto id = fixture.layerParameter(layer, role);
    const auto* record = fixture.composition().parameters().find(id);
    const auto* constant = record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
    if (constant != nullptr && constant->value == value)
        return;
    fixture.run<commands::SetParameterSource>(fixture.main, id, document::ConstantValueSource{value});
}

void setNodeValue(Fixture& fixture, const document::NodeId node, const std::string_view role, const double value) {
    const auto id = fixture.nodeParameter(node, role);
    const auto* record = fixture.composition().parameters().find(id);
    const auto* constant = record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
    if (constant != nullptr && constant->value == document::ParameterValue{value})
        return;
    fixture.run<commands::SetParameterSource>(fixture.main, id, document::ConstantValueSource{document::ParameterValue{value}});
}

document::NodeId addNode(Fixture& fixture, const std::string& type) {
    return *fixture.run<commands::AddNode>(fixture.main, type, document::Vec2d{0, -300})
                .outputId<document::NodeId>(commands::kAddNodeOutput);
}

// Puts `nodes`, in order, between `source` and the slot of `layer` (an entry layer).
void insertBetween(Fixture& fixture, const document::LayerId layer, const document::NodeId source,
                   const std::vector<document::NodeId>& nodes) {
    using namespace commands;
    const auto* boundary = fixture.composition().graph().findLayer(layer);
    const auto merge = boundary->nodeId;
    const auto slot = *boundary->entrySlot;
    auto from = source;
    for (const auto node : nodes) {
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{from, "image"}, document::NodeInputRef{node, "input"});
        from = node;
    }
    fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{from, "image"},
                              document::LayerStackInputRef{merge, slot, std::string(document::kLayerStackContentInputRole)});
}

// The edits on `layer` (whose artwork is `source`), with every length times `factor` and the
// layer's matrix times a scale of 1 / factor about the origin: factor 1 is the one layer's own
// edits, a larger one the scaled pre-composition's.
void applyEdits(Fixture& fixture, const document::LayerId layer, const document::NodeId source, const LayerEdits& edits,
                const double factor) {
    using namespace commands;
    if (edits.parentScale != 0.0) {
        const auto parent = fixture.run<AddSolidLayer>(fixture.main, "Null", core::Color4d{1, 0, 0, 1},
                                                       document::Vec2d{200, 120}, 0.0);
        const auto parentLayer = *parent.outputId<document::LayerId>(kAddSolidLayerLayerOutput);
        setLayerValue(fixture, parentLayer, document::kScaleParameterRole,
                      document::Vec2d{edits.parentScale, edits.parentScale});
        setLayerValue(fixture, parentLayer, document::kRotationParameterRole, 9.0);
        fixture.run<SetLayerParent>(fixture.main, layer, parentLayer);
    }
    setLayerValue(fixture, layer, document::kScaleParameterRole,
                  document::Vec2d{edits.scale.x / factor, edits.scale.y / factor});
    setLayerValue(fixture, layer, document::kRotationParameterRole, edits.degrees);
    // A frame pivot is the frame's centre plus the anchor: times `factor`, (factor - 1) * centre.
    setLayerValue(fixture, layer, document::kAnchorParameterRole,
                  document::Vec2d{(factor - 1.0) * kB2Centre.x, (factor - 1.0) * kB2Centre.y});
    if (edits.mask) {
        document::PathValue mask;
        document::PathContour contour;
        contour.closed = true;
        for (const auto& point : {document::Vec2d{200, 104}, document::Vec2d{318, 110}, document::Vec2d{300, 186},
                                  document::Vec2d{214, 176}})
            contour.anchors.push_back(document::PathAnchor{{point.x * factor, point.y * factor}, std::nullopt, std::nullopt});
        mask.contours.push_back(contour);
        fixture.run<AddLayerMask>(fixture.main, layer, mask);
        const auto feather = fixture.composition().graph().findLayer(layer)->masks.back().feather;
        fixture.run<SetParameterSource>(fixture.main, feather,
                                        document::ConstantValueSource{document::ParameterValue{1.5 * factor}});
    }
    if (edits.retime)
        fixture.run<SetLayerTiming>(fixture.main, layer, frame(1), core::RationalTime{}, core::RationalTime::fromInteger(4));
    std::vector<document::NodeId> between;
    if (edits.effects) {
        const auto blur = addNode(fixture, "bloom.gaussian-blur");
        setNodeValue(fixture, blur, "radius", 1.5 * factor);
        const auto shadow = addNode(fixture, std::string(document::kDropShadowNodeType));
        setNodeValue(fixture, shadow, "distance", 5.0 * factor);
        setNodeValue(fixture, shadow, "softness", 2.0 * factor);
        between = {blur, shadow};
    }
    if (edits.colour) {
        const auto hue = addNode(fixture, "bloom.hue-saturation");
        between.push_back(hue);
    }
    if (!between.empty())
        insertBetween(fixture, layer, source, between);
}

document::VectorArtworkExpansion artworkExpansion(const Fixture& fixture,
                                                   document::VectorArtworkExpansionContext context = {}) {
    if (!context.fonts)
        context.fonts = noFonts();
    const auto snapshot = fixture.document.snapshot();
    return document::expandVectorArtwork(snapshot.project(), *snapshot.project().findComposition(fixture.main), context);
}

struct SharpnessReport final {
    double worst = 0.0;
    std::size_t moved = 0;
    std::size_t edges = 0;
    std::size_t softMoved = 0;
    std::size_t softEdges = 0;
};

// The one layer against its scaled pre-composition and the resampled picture, at `time`.
std::optional<SharpnessReport> compareSharp(const Fixture& layer, const Fixture& reference, const core::RationalTime time,
                                            const std::string& label) {
    const auto one = renderExpanded(layer.document, layer.main, time, label + " one layer");
    const auto precomposed = renderProject(reference.document, reference.main, time, label + " scaled pre-composition");
    document::VectorArtworkExpansionContext resample;
    resample.composeLayerTransform = false;
    const auto soft = renderExpandedWith(layer.document, layer.main, time, label + " resampled", resample);
    if (!one || !precomposed || !soft)
        return std::nullopt;
    SharpnessReport report;
    report.worst = difference(*one, *precomposed);
    report.moved = differingPixels(*one, *precomposed, 1e-3);
    report.edges = partialPixels(*one);
    report.softMoved = differingPixels(*soft, *precomposed, 1e-2);
    report.softEdges = partialPixels(*soft);
    std::cout << label << ": worst difference to the scaled pre-composition " << report.worst << " on " << report.moved
              << " pixels (edge pixels " << report.edges << "); the resampled picture differs on " << report.softMoved
              << " pixels (edge pixels " << report.softEdges << ")\n";
    expect(coverage(*one) > 1000.0, label + ": the artwork draws");
    expect(std::abs(coverage(*one) - coverage(*soft)) <= 0.01 * coverage(*soft),
           label + ": the sharp picture covers what the resampled one does (the same artwork, placed alike)");
    return report;
}

// The one layer is its scaled pre-composition, and the resampled picture is far softer. At a band of
// 2 the two are the same pixels. At a band of 4 the curves' flattenings differ: the one layer
// flattens in the file's units through a 4x matrix, the baked file at its own 4x size, and each
// polyline is within 1/32 pixel of the curve at that size, but where a curve passes that close to
// one of a pixel's 4 x 4 coverage samples the two may land it on opposite sides, one or two of the
// sixteen samples (the composed layer of sharpWhenScaledAndTurned shows the same at 4x). Those few
// edge pixels then pass through the layer's single resample. Nothing else differs.
void expectSharp(const SharpnessReport& report, const std::string& label, const double factor, const bool countEdges) {
    if (factor <= 2.0)
        expect(report.worst <= 1e-6, label + ": the layer is its scaled pre-composition, pixel for pixel (worst " +
                                         std::to_string(report.worst) + " on " + std::to_string(report.moved) + " pixels)");
    else
        expect(report.worst <= 2.0 / 16.0 + 1.0 / 255.0 && report.moved * 4 <= report.edges,
               label + ": the layer is its scaled pre-composition up to two of a pixel's sixteen coverage samples on "
                       "curved edges (worst " + std::to_string(report.worst) + " on " + std::to_string(report.moved) +
                   " of " + std::to_string(report.edges) + " edge pixels)");
    expect(report.softMoved >= 10 * report.moved + 1000,
           label + ": the resampled picture is far from it (" + std::to_string(report.softMoved) + " pixels against " +
               std::to_string(report.moved) + ")");
    if (countEdges)
        expect(report.softEdges > report.edges + report.edges / 2,
               label + ": a resampled picture would be visibly softer (" + std::to_string(report.softEdges) +
                   " edge pixels against " + std::to_string(report.edges) + ")");
}

// Each case of a layer the composition changes is drawn at its output scale.
void sharpUnderEveryLayerEdit() {
    struct Case final {
        std::string name;
        LayerEdits edits;
        double factor;
        std::int64_t frame = 0;
        // Effects soften every edge they reach, so their edge count is no measure of sharpness;
        // the distance to the scaled pre-composition still is.
        bool countEdges = true;
    };
    const std::vector<Case> cases{
        {"retimed, scaled 2x", {.scale = {2.0, 2.0}, .retime = true}, 2.0, 2},
        {"masked, scaled 1.6x and turned 20 degrees", {.scale = {1.6, 1.6}, .degrees = 20.0, .mask = true}, 2.0},
        {"masked, scaled 3x and turned 20 degrees", {.scale = {3.0, 3.0}, .degrees = 20.0, .mask = true}, 4.0},
        {"retimed, scaled 3x", {.scale = {3.0, 3.0}, .retime = true}, 4.0, 2},
        {"a blur and a drop shadow between, scaled 2.2x and turned 27 degrees",
         {.scale = {2.2, 2.2}, .degrees = 27.0, .effects = true},
         4.0,
         0,
         false},
        {"a colour node between, scaled 3x", {.scale = {3.0, 3.0}, .colour = true}, 4.0},
        {"masked under a parent scaled 2x, scaled 1.5x", {.scale = {1.5, 1.5}, .mask = true, .parentScale = 2.0}, 4.0},
    };
    const auto plain = writeSvg("b2-plain", sharpSvg(""));
    for (const auto& item : cases) {
        const auto bakedFile = writeSvg("b2-baked", bakedSharpSvg(item.factor));
        Fixture layer(plain, commands::SvgImportMode::Layer, kB2Width, kB2Height);
        unfit(layer);
        applyEdits(layer, layer.art, *layer.artworkNode, item.edits, 1.0);
        Fixture reference(bakedFile, commands::SvgImportMode::Shapes, kB2Width, kB2Height, false);
        applyEdits(reference, reference.art, *reference.rootMerge, item.edits, item.factor);
        std::filesystem::remove(bakedFile);
        const auto expansion = artworkExpansion(layer);
        const auto oversampled = expansion.oversampled.find(layer.art);
        expect(expansion.composed.empty() && expansion.resampled.empty() && oversampled != expansion.oversampled.end() &&
                   oversampled->second.scale == item.factor && oversampled->second.limited.empty() &&
                   !oversampled->second.leaves.empty(),
               item.name + ": the layer's artwork is drawn at " + std::to_string(item.factor) + "x");
        const auto report = compareSharp(layer, reference, frame(item.frame), item.name);
        if (report)
            expectSharp(*report, item.name, item.factor, item.countEdges);
    }
    std::filesystem::remove(plain);
}

// A layer pivoting on its content: an artwork read by a Layer node (content pivot) that feeds the
// entry's own Layer node. Its pivot is the centre of the picture at the composition's scale.
void sharpOnAContentPivot() {
    constexpr double kFactor = 4.0;
    const auto plain = writeSvg("b2-pivot", sharpSvg(""));
    const auto bakedFile = writeSvg("b2-pivot-baked", bakedSharpSvg(kFactor));
    const auto build = [](Fixture& fixture, const document::NodeId source, const double factor) {
        using namespace commands;
        const auto* entry = fixture.composition().graph().findLayer(fixture.art);
        const auto merge = entry->nodeId;
        const auto slot = *entry->entrySlot;
        const auto pivoting = addNode(fixture, std::string(document::kLayerOutputNodeType));
        const auto outer = addNode(fixture, std::string(document::kLayerOutputNodeType));
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{source, "image"},
                                  document::NodeInputRef{pivoting, std::string(document::kLayerOutputContentInputPort)});
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{pivoting, std::string(document::kLayerOutputOutputPort)},
                                  document::NodeInputRef{outer, std::string(document::kLayerOutputContentInputPort)});
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{outer, std::string(document::kLayerOutputOutputPort)},
                                  document::LayerStackInputRef{merge, slot, std::string(document::kLayerStackContentInputRole)});
        std::optional<document::LayerId> held;
        for (const auto& boundary : fixture.composition().graph().layerOutputs())
            if (!boundary.entryOwned() && boundary.nodeId == pivoting)
                held = boundary.layerId;
        if (!held || fixture.composition().graph().findLayer(*held)->pivot != document::LayerPivot::Content)
            throw std::runtime_error("the Layer node does not pivot on its content");
        // A content pivot is the content's centre plus the anchor: times `factor`, the anchor too.
        setLayerValue(fixture, *held, document::kScaleParameterRole, document::Vec2d{2.5 / factor, 2.5 / factor});
        setLayerValue(fixture, *held, document::kRotationParameterRole, 15.0);
        setLayerValue(fixture, *held, document::kAnchorParameterRole, document::Vec2d{6.0 * factor, -4.0 * factor});
        return *held;
    };
    Fixture layer(plain, commands::SvgImportMode::Layer, kB2Width, kB2Height);
    unfit(layer);
    const auto held = build(layer, *layer.artworkNode, 1.0);
    Fixture reference(bakedFile, commands::SvgImportMode::Shapes, kB2Width, kB2Height, false);
    static_cast<void>(build(reference, *reference.rootMerge, kFactor));
    const auto expansion = artworkExpansion(layer);
    const auto oversampled = expansion.oversampled.find(held);
    expect(oversampled != expansion.oversampled.end() && oversampled->second.scale == kFactor,
           "content pivot: the Layer node's artwork is drawn at 4x");
    if (const auto report = compareSharp(layer, reference, frame(0), "content pivot, scaled 2.5x and turned 15 degrees"))
        expectSharp(*report, "content pivot", kFactor, true);
    // Its bounds are the resampled layer's: the same box, polygon and pivot.
    const auto plan = compilePlan(layer.document, layer.main, "content pivot bounds");
    const auto evaluated = plan ? evaluate(plan, frame(0), "content pivot bounds") : std::nullopt;
    document::VectorArtworkExpansionContext resample;
    resample.composeLayerTransform = false;
    resample.fonts = noFonts();
    auto softExpansion = artworkExpansion(layer, resample);
    document::Project project = layer.document.snapshot().project();
    *project.findComposition(layer.main) = *softExpansion.composition;
    const document::Document soft(std::move(project));
    const auto softPlan = compilePlan(soft, layer.main, "content pivot bounds resampled");
    const auto softEvaluated = softPlan ? evaluate(softPlan, frame(0), "content pivot bounds resampled") : std::nullopt;
    const auto* bounds = evaluated ? boundsOf(*evaluated->frame, held) : nullptr;
    const auto* softBounds = softEvaluated ? boundsOf(*softEvaluated->frame, held) : nullptr;
    expect(bounds != nullptr && softBounds != nullptr, "content pivot: both publish the layer's bounds");
    if (bounds != nullptr && softBounds != nullptr) {
        expect(std::abs(bounds->local.left - softBounds->local.left) <= 1.0 &&
                   std::abs(bounds->local.right - softBounds->local.right) <= 1.0 &&
                   std::abs(bounds->local.top - softBounds->local.top) <= 1.0 &&
                   std::abs(bounds->local.bottom - softBounds->local.bottom) <= 1.0,
               "content pivot: the local box is the picture's at the composition's scale");
        double worst = 0.0;
        for (std::size_t i = 0; i < 4; ++i)
            worst = std::max({worst, std::abs(bounds->polygon[i].x - softBounds->polygon[i].x),
                              std::abs(bounds->polygon[i].y - softBounds->polygon[i].y)});
        expect(worst <= 4.0, "content pivot: the polygon is the resampled layer's within its edge pixels (" +
                                 std::to_string(worst) + ")");
        expect(std::abs(bounds->anchor.x - softBounds->anchor.x) <= 0.5 && std::abs(bounds->anchor.y - softBounds->anchor.y) <= 0.5,
               "content pivot: the anchor is the resampled layer's");
    }
    std::filesystem::remove(plain);
    std::filesystem::remove(bakedFile);
}

// What still cannot be drawn sharp is named, and still renders: a node not known to draw the same
// at a larger scale, a layer scale and a blur radius driven by the graph. The band follows the
// window the frame shows (B3), is at most 64x, is halved while its window's estimate is past the
// named pixel budget, and is halved while a length would leave its range.
void namedWhenItCannotBeSharp() {
    using namespace commands;
    const auto plain = writeSvg("b2-named", sharpSvg(""));
    const auto renders = [](const Fixture& fixture, const std::string& label) {
        const auto frame0 = renderExpanded(fixture.document, fixture.main, frame(0), label);
        expect(frame0 && coverage(*frame0) > 100.0, label + ": still renders");
    };
    {
        Fixture fixture(plain, SvgImportMode::Layer, kB2Width, kB2Height);
        setLayerValue(fixture, fixture.art, document::kScaleParameterRole, document::Vec2d{3.0, 3.0});
        const auto vignette = addNode(fixture, "bloom.vignette");
        insertBetween(fixture, fixture.art, *fixture.artworkNode, {vignette});
        const auto expansion = artworkExpansion(fixture);
        const auto why = expansion.resampled.find(*fixture.artworkNode);
        expect(expansion.oversampled.empty() && why != expansion.resampled.end() &&
                   why->second.find("bloom.vignette") != std::string::npos &&
                   why->second.find("not known to draw the same at a larger scale") != std::string::npos,
               "named: a node not known to scale is named: " + (why != expansion.resampled.end() ? why->second : ""));
        renders(fixture, "named vignette");
    }
    {
        Fixture fixture(plain, SvgImportMode::Layer, kB2Width, kB2Height);
        setLayerValue(fixture, fixture.art, document::kScaleParameterRole, document::Vec2d{3.0, 3.0});
        const auto blur = addNode(fixture, "bloom.gaussian-blur");
        insertBetween(fixture, fixture.art, *fixture.artworkNode, {blur});
        const auto value = addNode(fixture, "bloom.value-scalar");
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{value, "value"}, document::NodeInputRef{blur, "radius"});
        const auto expansion = artworkExpansion(fixture);
        const auto why = expansion.resampled.find(*fixture.artworkNode);
        expect(expansion.oversampled.empty() && why != expansion.resampled.end() &&
                   why->second.find("radius driven by the graph") != std::string::npos,
               "named: a blur radius driven by the graph is named: " + (why != expansion.resampled.end() ? why->second : ""));
        renders(fixture, "named driven radius");
    }
    {
        // A Layer node reading the artwork, its scale driven by a value node, masked.
        Fixture fixture(plain, SvgImportMode::Layer, kB2Width, kB2Height);
        const auto* entry = fixture.composition().graph().findLayer(fixture.art);
        const auto merge = entry->nodeId;
        const auto slot = *entry->entrySlot;
        const auto node = addNode(fixture, std::string(document::kLayerOutputNodeType));
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{*fixture.artworkNode, "image"},
                                  document::NodeInputRef{node, std::string(document::kLayerOutputContentInputPort)});
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{node, std::string(document::kLayerOutputOutputPort)},
                                  document::LayerStackInputRef{merge, slot, std::string(document::kLayerStackContentInputRole)});
        std::optional<document::LayerId> held;
        for (const auto& boundary : fixture.composition().graph().layerOutputs())
            if (!boundary.entryOwned() && boundary.nodeId == node)
                held = boundary.layerId;
        const auto value = addNode(fixture, "bloom.value-vector2");
        fixture.run<SetParameterSource>(fixture.main, fixture.nodeParameter(value, "value"),
                                        document::ConstantValueSource{document::ParameterValue{document::Vec2d{2.0, 2.0}}});
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{value, "value"},
                                  document::NodeInputRef{node, std::string(document::kScaleParameterRole)});
        document::PathValue mask;
        document::PathContour contour;
        contour.closed = true;
        for (const auto& point : {document::Vec2d{200, 100}, document::Vec2d{320, 100}, document::Vec2d{260, 190}})
            contour.anchors.push_back(document::PathAnchor{point, std::nullopt, std::nullopt});
        mask.contours.push_back(contour);
        fixture.run<AddLayerMask>(fixture.main, *held, mask);
        const auto expansion = artworkExpansion(fixture);
        const auto why = expansion.resampled.find(*fixture.artworkNode);
        expect(expansion.oversampled.empty() && why != expansion.resampled.end() &&
                   why->second.find("scale is driven by the graph") != std::string::npos,
               "named: a layer scale driven by the graph is named: " + (why != expansion.resampled.end() ? why->second : ""));
        renders(fixture, "named driven scale");
    }
    {
        // #642 B3: only the window the frame shows is drawn, so a 3840 x 2160 composition draws a
        // layer scaled 6x at 8x (about a frame of pixels). Expansion only: no 4K CPU frame.
        Fixture fixture(plain, SvgImportMode::Layer, 3840, 2160);
        applyEdits(fixture, fixture.art, *fixture.artworkNode, {.scale = {6.0, 6.0}, .mask = true}, 1.0);
        const auto expansion = artworkExpansion(fixture);
        const auto found = expansion.oversampled.find(fixture.art);
        expect(found != expansion.oversampled.end() && found->second.scale == 8.0 && found->second.limited.empty(),
               "window: a 4K composition draws a layer scaled 6x at 8x: " +
                   (found != expansion.oversampled.end() ? std::to_string(found->second.scale) + " " + found->second.limited
                                                         : std::string("not oversampled")));
        Fixture small(plain, SvgImportMode::Layer, kB2Width, kB2Height);
        applyEdits(small, small.art, *small.artworkNode, {.scale = {100.0, 100.0}, .mask = true}, 1.0);
        const auto capped = artworkExpansion(small);
        const auto most = capped.oversampled.find(small.art);
        expect(most != capped.oversampled.end() && most->second.scale == document::kVectorArtworkMaxOversample &&
                   most->second.limited.find("the most any layer oversamples") != std::string::npos,
               "cap: no layer is drawn past 64x, and it says so");
    }
    {
        // #642 B3, the named pixel budget: a whole-frame artwork keyed from 1x to 8x in a 4K
        // composition would draw sixty-four frames of pixels at its first key; it is drawn at 2x
        // and says why. A content pivot cannot be clipped (its matrix follows its content), so a
        // whole-frame one scaled 6x is drawn at 2x too, named. Expansion only.
        Fixture keyed(plain, SvgImportMode::Layer, 3840, 2160);
        applyEdits(keyed, keyed.art, *keyed.artworkNode, {.mask = true}, 1.0);
        const auto scale = keyed.layerParameter(keyed.art, document::kScaleParameterRole);
        keyed.run<SetKeyframeAtTimeForParameter>(keyed.main, scale, frame(2), document::Vec2d{8.0, 8.0});
        keyed.run<SetKeyframeAtTimeForParameter>(keyed.main, scale, frame(0), document::Vec2d{1.0, 1.0});
        const auto expansion = artworkExpansion(keyed);
        const auto found = expansion.oversampled.find(keyed.art);
        expect(found != expansion.oversampled.end() && found->second.scale == 2.0 &&
                   found->second.limited.find("pixel budget") != std::string::npos,
               "budget: a whole-frame artwork keyed 1x to 8x at 4K is drawn at 2x, the budget named: " +
                   (found != expansion.oversampled.end() ? found->second.limited : std::string("not oversampled")));
        Fixture pivot(plain, SvgImportMode::Layer, 3840, 2160);
        const auto* entry = pivot.composition().graph().findLayer(pivot.art);
        const auto merge = entry->nodeId;
        const auto slot = *entry->entrySlot;
        // A Layer node pivoting on its content, read by the entry's own Layer node (as in
        // sharpOnAContentPivot).
        const auto node = addNode(pivot, std::string(document::kLayerOutputNodeType));
        const auto outer = addNode(pivot, std::string(document::kLayerOutputNodeType));
        pivot.run<ConnectPorts>(pivot.main, document::OutputPortRef{*pivot.artworkNode, "image"},
                                document::NodeInputRef{node, std::string(document::kLayerOutputContentInputPort)});
        pivot.run<ConnectPorts>(pivot.main, document::OutputPortRef{node, std::string(document::kLayerOutputOutputPort)},
                                document::NodeInputRef{outer, std::string(document::kLayerOutputContentInputPort)});
        pivot.run<ConnectPorts>(pivot.main, document::OutputPortRef{outer, std::string(document::kLayerOutputOutputPort)},
                                document::LayerStackInputRef{merge, slot, std::string(document::kLayerStackContentInputRole)});
        std::optional<document::LayerId> held;
        for (const auto& boundary : pivot.composition().graph().layerOutputs())
            if (!boundary.entryOwned() && boundary.nodeId == node && boundary.pivot == document::LayerPivot::Content)
                held = boundary.layerId;
        expect(held.has_value(), "budget: the Layer node pivots on its content");
        if (held) {
            setLayerValue(pivot, *held, document::kScaleParameterRole, document::Vec2d{6.0, 6.0});
            const auto pivoted = artworkExpansion(pivot);
            const auto drawn = pivoted.oversampled.find(*held);
            expect(drawn != pivoted.oversampled.end() && drawn->second.scale == 2.0 &&
                       drawn->second.limited.find("pixel budget") != std::string::npos,
                   "budget: a whole-frame content-pivot layer scaled 6x at 4K is drawn at 2x, the budget named: " +
                       (drawn != pivoted.oversampled.end() ? drawn->second.limited : std::string("not oversampled")));
        }
    }
    {
        // A blur radius of 100 at 4x would be 400, past the 256 a blur takes: drawn at 2x, named.
        Fixture fixture(plain, SvgImportMode::Layer, kB2Width, kB2Height);
        setLayerValue(fixture, fixture.art, document::kScaleParameterRole, document::Vec2d{3.0, 3.0});
        const auto blur = addNode(fixture, "bloom.gaussian-blur");
        setNodeValue(fixture, blur, "radius", 100.0);
        insertBetween(fixture, fixture.art, *fixture.artworkNode, {blur});
        const auto expansion = artworkExpansion(fixture);
        const auto found = expansion.oversampled.find(fixture.art);
        expect(found != expansion.oversampled.end() && found->second.scale == 2.0 &&
                   found->second.limited.find("bloom.blur.radius") != std::string::npos,
               "domain: a length past its range at the band halves it, named");
        renders(fixture, "domain-limited blur");
    }
    {
        // A layer that neither scales up nor turns needs no more pixels than the composition's.
        Fixture fixture(plain, SvgImportMode::Layer, kB2Width, kB2Height);
        applyEdits(fixture, fixture.art, *fixture.artworkNode, {.scale = {0.8, 0.8}, .mask = true}, 1.0);
        const auto expansion = artworkExpansion(fixture);
        expect(expansion.oversampled.empty() && expansion.composed.empty() && expansion.resampled.empty(),
               "a masked layer at 0.8x is drawn at the composition's scale, which is already sharp");
    }
    std::filesystem::remove(plain);
}

// Cache identity: the band is part of the plan. An animated transform renders every frame from one
// plan and one oversampled picture of the artwork (operation-cache hits); an edit inside the band
// keeps the artwork's picture (its operations hit across revisions); an edit that moves the band
// draws a new one. A preview override of the layer's scale moves the band; an override of a mask's
// path is multiplied as the stored path is. Cancellation and the request's budget reach the
// oversampled layer.
void oversampledCacheIdentity() {
    using namespace commands;
    const auto plain = writeSvg("b2-identity", sharpSvg(""));
    Fixture layer(plain, SvgImportMode::Layer, kB2Width, kB2Height);
    unfit(layer);
    applyEdits(layer, layer.art, *layer.artworkNode, {.mask = true}, 1.0);
    const auto scale = layer.layerParameter(layer.art, document::kScaleParameterRole);
    const auto rotation = layer.layerParameter(layer.art, document::kRotationParameterRole);
    layer.run<SetKeyframeAtTimeForParameter>(layer.main, scale, frame(2), document::Vec2d{3.0, 3.0});
    layer.run<SetKeyframeAtTimeForParameter>(layer.main, scale, frame(0), document::Vec2d{1.2, 1.2});
    layer.run<SetKeyframeAtTimeForParameter>(layer.main, rotation, frame(2), 18.0);
    layer.run<SetKeyframeAtTimeForParameter>(layer.main, rotation, frame(0), 0.0);
    const auto contentScaleOf = [&](const runtime::CompiledCompositionPlan& plan) {
        double found = 0.0;
        for (const auto& operation : plan.operations())
            if (const auto* output = std::get_if<runtime::CompiledLayerOutput>(&operation); output && output->layerId == layer.art)
                found = output->contentScale;
        return found;
    };
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    runtime::CompiledPlanCache plans;
    const auto keyed = plans.compile(compiler, {layer.document.snapshot(), layer.main}, {});
    expect(keyed.plan != nullptr && contentScaleOf(*keyed.plan) == 4.0,
           "identity: a scale keyed from 1.2x to 3x is drawn in the 4x band");
    runtime::CpuCompositionEvaluator evaluator;
    const auto cachedAt = [&](const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan, const std::int64_t index) {
        auto request = referenceRequest(*plan, frame(index));
        request.bypassOperationCache = false;
        const auto before = evaluator.operationCache()->statistics().hits;
        const auto result = evaluator.evaluate(plan, request, {});
        expect(result.frame() != nullptr, "identity: frame " + std::to_string(index) + " evaluates");
        return std::pair{evaluator.operationCache()->statistics().hits - before,
                         result.frame() ? std::optional{pixelsOf(*result.frame())} : std::nullopt};
    };
    std::vector<Frame> frames;
    std::size_t animatedHits = 0;
    for (const std::int64_t index : {0, 1, 2}) {
        const auto [hits, pixels] = cachedAt(keyed.plan, index);
        if (index > 0)
            animatedHits += hits;
        if (pixels)
            frames.push_back(*pixels);
    }
    expect(frames.size() == 3 && difference(frames[0], frames[1]) > 1e-3 && difference(frames[1], frames[2]) > 1e-3,
           "identity: the animated transform changes every frame");
    const auto again = plans.compile(compiler, {layer.document.snapshot(), layer.main}, {});
    expect(again.plan == keyed.plan && plans.statistics().compiles == 1,
           "identity: animating the transform never compiles again");
    expect(animatedHits > 0, "identity: frames 1 and 2 reuse the oversampled artwork's picture from the cache (" +
                                 std::to_string(animatedHits) + " hits)");
    // An edit inside the band: a new plan (a new revision), the same picture of the artwork.
    layer.run<SetKeyframeAtTimeForParameter>(layer.main, scale, frame(2), document::Vec2d{3.6, 3.6});
    const auto inside = plans.compile(compiler, {layer.document.snapshot(), layer.main}, {});
    expect(inside.plan != nullptr && inside.plan != keyed.plan && contentScaleOf(*inside.plan) == 4.0,
           "identity: a key moved inside the band keeps the 4x band");
    const auto insideHits = inside.plan ? cachedAt(inside.plan, 0).first : 0;
    expect(insideHits > 0, "identity: inside the band the artwork's operations hit the cache (" +
                               std::to_string(insideHits) + " hits)");
    // An edit that moves the band: a picture of its own.
    layer.run<SetKeyframeAtTimeForParameter>(layer.main, scale, frame(2), document::Vec2d{5.0, 5.0});
    const auto moved = plans.compile(compiler, {layer.document.snapshot(), layer.main}, {});
    expect(moved.plan != nullptr && contentScaleOf(*moved.plan) == 8.0, "identity: a key past 4x moves to the 8x band");
    const auto movedHits = moved.plan ? cachedAt(moved.plan, 0).first : 0;
    expect(movedHits < insideHits, "identity: a new band draws a new picture of the artwork (" + std::to_string(movedHits) +
                                       " hits against " + std::to_string(insideHits) + ")");

    // Preview overrides: the layer's scale sets the band; a mask path is multiplied like the stored one.
    {
        Fixture still(plain, SvgImportMode::Layer, kB2Width, kB2Height);
        unfit(still);
        applyEdits(still, still.art, *still.artworkNode, {.scale = {1.5, 1.5}, .mask = true}, 1.0);
        runtime::SnapshotCompileRequest grow{still.document.snapshot(), still.main};
        grow.parameterOverrides.push_back({still.document.snapshot().revision(),
                                           still.layerParameter(still.art, document::kScaleParameterRole),
                                           document::Vec2d{6.0, 6.0}});
        const auto grown = compiler.compile(grow, {});
        double grownScale = 0.0;
        if (grown.plan)
            for (const auto& operation : grown.plan->operations())
                if (const auto* output = std::get_if<runtime::CompiledLayerOutput>(&operation); output && output->layerId == still.art)
                    grownScale = output->contentScale;
        expect(grownScale == 8.0, "override: dragging the layer's scale to 6x draws its artwork in the 8x band");
        // The mask dragged: an override equals the same path stored.
        const auto maskPath = still.composition().graph().findLayer(still.art)->masks.back().path;
        document::PathValue dragged;
        document::PathContour contour;
        contour.closed = true;
        for (const auto& point : {document::Vec2d{190, 90}, document::Vec2d{330, 120}, document::Vec2d{250, 200}})
            contour.anchors.push_back(document::PathAnchor{point, std::nullopt, std::nullopt});
        dragged.contours.push_back(contour);
        runtime::SnapshotCompileRequest drag{still.document.snapshot(), still.main};
        drag.parameterOverrides.push_back({still.document.snapshot().revision(), maskPath, dragged});
        const auto preview = compiler.compile(drag, {});
        const auto previewFrame = preview.plan ? evaluate(preview.plan, frame(0), "mask override") : std::nullopt;
        still.run<SetParameterSource>(still.main, maskPath, document::ConstantValueSource{document::ParameterValue{dragged}});
        const auto stored = renderExpanded(still.document, still.main, frame(0), "mask stored");
        expect(previewFrame && stored && difference(pixelsOf(*previewFrame->frame), *stored) == 0.0,
               "override: a mask dragged in the Viewer previews exactly what it draws once stored");
    }

    // Cancellation and the request's budget.
    {
        const auto token = cancelledToken();
        const auto cancelledCompile = compiler.compile({layer.document.snapshot(), layer.main}, token);
        expect(cancelledCompile.status == runtime::SnapshotCompileStatus::Cancelled && cancelledCompile.plan == nullptr,
               "cancel: a cancelled compile of an oversampled layer reports Cancelled and no plan");
        if (moved.plan) {
            const auto result = runtime::CpuCompositionEvaluator{}.evaluate(moved.plan, referenceRequest(*moved.plan, frame(2)), token);
            expect(result.status() == runtime::EvaluationStatus::Cancelled && result.frame() == nullptr,
                   "cancel: a cancelled evaluation of an oversampled layer publishes no frame");
            auto request = referenceRequest(*moved.plan, frame(2));
            request.pixelStorageByteLimit = 4096;
            const auto refused = runtime::CpuCompositionEvaluator{}.evaluate(moved.plan, request, {});
            expect(refused.frame() == nullptr && refused.status() == runtime::EvaluationStatus::Failed &&
                       !refused.diagnostics().empty(),
                   "budget: a request too small for the oversampled picture is refused, with a reason");
        }
    }
    std::filesystem::remove(plain);
}


// ---- #642 package B3: the groups above the layer, and the window ----
//
// A group layer (a Merge in another Merge's entry, or read by a Layer node) that scales or turns the
// artwork's picture counts in its band: the group's picture is drawn at its output scale too
// (VectorArtworkOversampledGroup), every entry's layer drawing into it that many times larger and
// the group's layer mapping it back. The reference is again the scaled pre-composition built through
// the editable import: the file baked `factor` times larger in the same groups, each group's layer
// carrying its matrix between the picture inside (`inner` times larger) and the one it draws into.

struct Group final {
    document::NodeId merge;
    // The entry layer holding the grouped picture inside the group.
    document::LayerId inner;
};

// Puts the picture of `source` (the artwork node, the editable import's root Merge, or a group's
// Merge) alone in a new group: a Merge holding it in one entry whose layer is at its defaults (the
// identity), read by the slot `source` fed, whose layer (`fixture.art`) becomes the group's layer.
Group groupAround(Fixture& fixture, const document::NodeId source) {
    using namespace commands;
    const auto* boundary = fixture.composition().graph().findLayer(fixture.art);
    const auto top = boundary->nodeId;
    const auto slot = *boundary->entrySlot;
    const auto merge = addNode(fixture, std::string(document::kLayerStackNodeType));
    const auto entered = fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{source, "image"},
                                                   document::LayerStackInputRef{merge, {}, std::string(document::kLayerStackContentInputRole)});
    const auto innerSlot = *entered.outputId<document::LayerSlotId>(kConnectPortsSlotOutput);
    fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{merge, "image"},
                              document::LayerStackInputRef{top, slot, std::string(document::kLayerStackContentInputRole)});
    const auto ensured = fixture.run<EnsureEntryLayer>(fixture.main, merge, innerSlot);
    return {merge, *ensured.outputId<document::LayerId>(kEntryLayerOutput)};
}

// A frame-pivot layer's transform (its position the frame's centre) carried between a picture drawn
// `inner` times larger and one drawn `outer` times larger, as a scaled pre-composition carries it:
// position times outer, pivot times inner, scale times outer / inner.
void precompose(Fixture& fixture, const document::LayerId layer, const double scale, const double degrees,
                const double inner, const double outer) {
    setLayerValue(fixture, layer, document::kPositionParameterRole, document::Vec2d{kB2Centre.x * outer, kB2Centre.y * outer});
    setLayerValue(fixture, layer, document::kAnchorParameterRole,
                  document::Vec2d{(inner - 1.0) * kB2Centre.x, (inner - 1.0) * kB2Centre.y});
    setLayerValue(fixture, layer, document::kScaleParameterRole, document::Vec2d{scale * outer / inner, scale * outer / inner});
    setLayerValue(fixture, layer, document::kRotationParameterRole, degrees);
}

// The artwork in a group scaled 3x, in one scaled 1.6x and turned, and in a group scaled 1.5x inside
// a group scaled 2x: drawn at the groups' output scale, sharp.
void sharpInsideScaledGroups() {
    struct Case final {
        std::string name;
        double inner;  // the scale of the group holding the artwork
        double degrees;
        double outer;  // the scale of a group holding that group; 0: none
        double factor; // the band of the artwork and of the group holding it
        double outerFactor;
    };
    const std::vector<Case> cases{
        {"in a group scaled 3x", 3.0, 0.0, 0.0, 4.0, 0.0},
        {"in a group scaled 1.6x and turned 25 degrees", 1.6, 25.0, 0.0, 2.0, 0.0},
        {"in a group scaled 1.5x inside a group scaled 2x", 1.5, 0.0, 2.0, 4.0, 2.0},
    };
    const auto plain = writeSvg("b3-group", sharpSvg(""));
    for (const auto& item : cases) {
        const auto bakedFile = writeSvg("b3-group-baked", bakedSharpSvg(item.factor));
        Fixture layer(plain, commands::SvgImportMode::Layer, kB2Width, kB2Height);
        unfit(layer);
        const auto group = groupAround(layer, *layer.artworkNode);
        std::optional<Group> outer;
        if (item.outer != 0.0)
            outer = groupAround(layer, group.merge);
        // Without an outer group the group's layer is `art`; with one, `art` is the outer group's.
        const auto groupLayer = outer ? outer->inner : layer.art;
        setLayerValue(layer, groupLayer, document::kScaleParameterRole, document::Vec2d{item.inner, item.inner});
        setLayerValue(layer, groupLayer, document::kRotationParameterRole, item.degrees);
        if (outer)
            setLayerValue(layer, layer.art, document::kScaleParameterRole, document::Vec2d{item.outer, item.outer});

        Fixture reference(bakedFile, commands::SvgImportMode::Shapes, kB2Width, kB2Height, false);
        std::filesystem::remove(bakedFile);
        const auto referenceGroup = groupAround(reference, *reference.rootMerge);
        if (item.outer != 0.0) {
            const auto referenceOuter = groupAround(reference, referenceGroup.merge);
            precompose(reference, referenceOuter.inner, item.inner, item.degrees, item.factor, item.outerFactor);
            precompose(reference, reference.art, item.outer, 0.0, item.outerFactor, 1.0);
        } else {
            precompose(reference, reference.art, item.inner, item.degrees, item.factor, 1.0);
        }

        const auto expansion = artworkExpansion(layer);
        const auto drawn = expansion.groups.find(group.merge);
        const auto held = expansion.oversampled.find(group.inner);
        expect(drawn != expansion.groups.end() && drawn->second.scale == item.factor && drawn->second.reader == groupLayer &&
                   drawn->second.limited.empty() &&
                   std::ranges::find(drawn->second.entries, group.inner) != drawn->second.entries.end() &&
                   expansion.contentScales.at(groupLayer) == item.factor &&
                   expansion.outputScales.at(group.inner) == item.factor,
               item.name + ": the group's picture is drawn at " + std::to_string(item.factor) + "x, mapped back by its layer");
        expect(held != expansion.oversampled.end() && held->second.scale == item.factor && expansion.resampled.empty() &&
                   expansion.groupsResampled.empty(),
               item.name + ": the artwork counts the group in its band");
        if (outer) {
            const auto outerDrawn = expansion.groups.find(outer->merge);
            expect(outerDrawn != expansion.groups.end() && outerDrawn->second.scale == item.outerFactor &&
                       expansion.outputScales.at(outer->inner) == item.outerFactor &&
                       expansion.contentScales.at(layer.art) == item.outerFactor,
                   item.name + ": the outer group is drawn at " + std::to_string(item.outerFactor) + "x");
        }
        if (const auto report = compareSharp(layer, reference, frame(0), item.name))
            expectSharp(*report, item.name, item.factor, true);
    }
    std::filesystem::remove(plain);
}

// A group drawn larger publishes the bounds it would publish at 1: a sibling of the artwork in the
// group keeps its box, polygon and anchor exactly, the group's layer and the artwork's layer keep
// theirs within the picture's edge pixels, and the sibling still draws.
void groupBoundsAndSiblings() {
    using namespace commands;
    const auto plain = writeSvg("b3-sibling", sharpSvg(""));
    Fixture layer(plain, SvgImportMode::Layer, kB2Width, kB2Height);
    unfit(layer);
    const auto group = groupAround(layer, *layer.artworkNode);
    const auto solid = addNode(layer, std::string(document::kSolidSourceNodeType));
    layer.run<SetParameterSource>(layer.main, layer.nodeParameter(solid, "color"),
                                  document::ConstantValueSource{document::ParameterValue{core::Color4d{0.9, 0.5, 0.1, 1.0}}});
    layer.run<SetParameterSource>(layer.main, layer.nodeParameter(solid, document::kSolidWidthParameterRole),
                                  document::ConstantValueSource{document::ParameterValue{std::int64_t{60}}});
    layer.run<SetParameterSource>(layer.main, layer.nodeParameter(solid, document::kSolidHeightParameterRole),
                                  document::ConstantValueSource{document::ParameterValue{std::int64_t{30}}});
    const auto entered = layer.run<ConnectPorts>(layer.main, document::OutputPortRef{solid, "image"},
                                                 document::LayerStackInputRef{group.merge, {}, std::string(document::kLayerStackContentInputRole)});
    const auto siblingSlot = *entered.outputId<document::LayerSlotId>(kConnectPortsSlotOutput);
    const auto sibling = *layer.run<EnsureEntryLayer>(layer.main, group.merge, siblingSlot).outputId<document::LayerId>(kEntryLayerOutput);
    setLayerValue(layer, sibling, document::kPositionParameterRole, document::Vec2d{230, 120});
    setLayerValue(layer, sibling, document::kRotationParameterRole, 12.0);
    setLayerValue(layer, layer.art, document::kScaleParameterRole, document::Vec2d{2.5, 2.5});
    setLayerValue(layer, layer.art, document::kRotationParameterRole, -8.0);
    const auto expansion = artworkExpansion(layer);
    const auto drawn = expansion.groups.find(group.merge);
    expect(drawn != expansion.groups.end() && drawn->second.scale == 4.0 &&
               std::ranges::find(drawn->second.entries, sibling) != drawn->second.entries.end(),
           "siblings: the sibling draws into the group's larger picture too");
    const auto plan = compilePlan(layer.document, layer.main, "siblings");
    const auto evaluated = plan ? evaluate(plan, frame(0), "siblings") : std::nullopt;
    document::VectorArtworkExpansionContext resample;
    resample.composeLayerTransform = false;
    resample.fonts = noFonts();
    auto softExpansion = artworkExpansion(layer, resample);
    document::Project project = layer.document.snapshot().project();
    *project.findComposition(layer.main) = *softExpansion.composition;
    const document::Document soft(std::move(project));
    const auto softPlan = compilePlan(soft, layer.main, "siblings resampled");
    const auto softEvaluated = softPlan ? evaluate(softPlan, frame(0), "siblings resampled") : std::nullopt;
    if (!evaluated || !softEvaluated)
        return;
    const auto near = [](const runtime::EvaluatedOperationBounds& a, const runtime::EvaluatedOperationBounds& b,
                         const double tolerance) {
        double worst = std::max({std::abs(a.output.left - b.output.left), std::abs(a.output.right - b.output.right),
                                 std::abs(a.output.top - b.output.top), std::abs(a.output.bottom - b.output.bottom),
                                 std::abs(a.anchor.x - b.anchor.x), std::abs(a.anchor.y - b.anchor.y)});
        for (std::size_t i = 0; i < 4; ++i)
            worst = std::max({worst, std::abs(a.polygon[i].x - b.polygon[i].x), std::abs(a.polygon[i].y - b.polygon[i].y)});
        return worst <= tolerance;
    };
    const auto* siblingBounds = boundsOf(*evaluated->frame, sibling);
    const auto* softSibling = boundsOf(*softEvaluated->frame, sibling);
    expect(siblingBounds != nullptr && softSibling != nullptr && siblingBounds->local == softSibling->local &&
               near(*siblingBounds, *softSibling, 1e-9),
           "siblings: a sibling in the larger group publishes the box, polygon and anchor it publishes at 1");
    const auto* innerBounds = boundsOf(*evaluated->frame, group.inner);
    const auto* softInner = boundsOf(*softEvaluated->frame, group.inner);
    expect(innerBounds != nullptr && softInner != nullptr && near(*innerBounds, *softInner, 1.0),
           "siblings: the artwork's layer in the larger group publishes its bounds at 1, within its edge pixel");
    const auto* groupBounds = boundsOf(*evaluated->frame, layer.art);
    const auto* softGroup = boundsOf(*softEvaluated->frame, layer.art);
    expect(groupBounds != nullptr && softGroup != nullptr && near(*groupBounds, *softGroup, 4.0),
           "siblings: the group's layer publishes the resampled group's box, polygon and anchor within its edge pixels");
    const auto sharpFrame = pixelsOf(*evaluated->frame);
    const auto softFrame = pixelsOf(*softEvaluated->frame);
    expect(std::abs(coverage(sharpFrame) - coverage(softFrame)) <= 0.01 * coverage(softFrame) && coverage(sharpFrame) > 1000.0,
           "siblings: the larger group covers what the resampled one does");
    std::filesystem::remove(plain);
}

// What keeps a group from being drawn larger is named, the artwork still draws at its own band, and
// the frame renders: a node between the group and its layer not known to scale, a group layer
// whose scale is driven by the graph, an Adjustment entry in the group.
void groupsNamedWhenTheyCannotBe() {
    using namespace commands;
    const auto plain = writeSvg("b3-named", sharpSvg(""));
    const auto check = [&](Fixture& fixture, const Group& group, const std::string& phrase, const std::string& label) {
        const auto expansion = artworkExpansion(fixture);
        const auto why = expansion.groupsResampled.find(group.merge);
        const auto held = expansion.oversampled.find(group.inner);
        expect(!expansion.groups.contains(group.merge) && why != expansion.groupsResampled.end() &&
                   why->second.find(phrase) != std::string::npos,
               label + ": named: " + (why != expansion.groupsResampled.end() ? why->second : std::string("not named")));
        expect(held == expansion.oversampled.end() && expansion.composed.contains(group.inner),
               label + ": the artwork is still drawn sharp through its own layer's geometry");
        const auto rendered = renderExpanded(fixture.document, fixture.main, frame(0), label);
        expect(rendered && coverage(*rendered) > 100.0, label + ": still renders");
    };
    {
        Fixture fixture(plain, SvgImportMode::Layer, kB2Width, kB2Height);
        unfit(fixture);
        const auto group = groupAround(fixture, *fixture.artworkNode);
        setLayerValue(fixture, group.inner, document::kScaleParameterRole, document::Vec2d{1.5, 1.5});
        setLayerValue(fixture, fixture.art, document::kScaleParameterRole, document::Vec2d{3.0, 3.0});
        const auto vignette = addNode(fixture, "bloom.vignette");
        const auto* boundary = fixture.composition().graph().findLayer(fixture.art);
        const auto top = boundary->nodeId;
        const auto slot = *boundary->entrySlot;
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{group.merge, "image"}, document::NodeInputRef{vignette, "input"});
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{vignette, "image"},
                                  document::LayerStackInputRef{top, slot, std::string(document::kLayerStackContentInputRole)});
        check(fixture, group, "bloom.vignette", "a vignette between the group and its layer");
    }
    {
        Fixture fixture(plain, SvgImportMode::Layer, kB2Width, kB2Height);
        unfit(fixture);
        const auto group = groupAround(fixture, *fixture.artworkNode);
        setLayerValue(fixture, group.inner, document::kScaleParameterRole, document::Vec2d{1.5, 1.5});
        // The group's layer is a Layer node whose scale a value node drives.
        const auto* boundary = fixture.composition().graph().findLayer(fixture.art);
        const auto top = boundary->nodeId;
        const auto slot = *boundary->entrySlot;
        const auto node = addNode(fixture, std::string(document::kLayerOutputNodeType));
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{group.merge, "image"},
                                  document::NodeInputRef{node, std::string(document::kLayerOutputContentInputPort)});
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{node, std::string(document::kLayerOutputOutputPort)},
                                  document::LayerStackInputRef{top, slot, std::string(document::kLayerStackContentInputRole)});
        const auto value = addNode(fixture, "bloom.value-vector2");
        fixture.run<SetParameterSource>(fixture.main, fixture.nodeParameter(value, "value"),
                                        document::ConstantValueSource{document::ParameterValue{document::Vec2d{3.0, 3.0}}});
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{value, "value"},
                                  document::NodeInputRef{node, std::string(document::kScaleParameterRole)});
        check(fixture, group, "driven by the graph", "a group layer whose scale is driven");
    }
    {
        Fixture fixture(plain, SvgImportMode::Layer, kB2Width, kB2Height);
        unfit(fixture);
        const auto group = groupAround(fixture, *fixture.artworkNode);
        setLayerValue(fixture, group.inner, document::kScaleParameterRole, document::Vec2d{1.5, 1.5});
        setLayerValue(fixture, fixture.art, document::kScaleParameterRole, document::Vec2d{3.0, 3.0});
        const auto adjustment = addNode(fixture, std::string(document::kAdjustmentNodeType));
        const auto curves = addNode(fixture, "bloom.curves");
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{adjustment, "image"}, document::NodeInputRef{curves, "input"});
        fixture.run<ConnectPorts>(fixture.main, document::OutputPortRef{curves, "image"},
                                  document::LayerStackInputRef{group.merge, {}, std::string(document::kLayerStackContentInputRole)});
        check(fixture, group, "Adjustment", "an Adjustment entry in the group");
    }
    std::filesystem::remove(plain);
}

// #642 B3 cache identity and the window rule. The band (the plan) follows the largest scale over
// the animation; the window each picture is drawn in follows what the frame shows of it, and is part
// of each operation's key for that frame, not of the plan. So a pan compiles nothing new, a frame
// that shows another part of the oversampled picture draws that part, and a frame shown again reuses
// what it drew. The group's band behaves like the layer's. Cancellation and the request's budget
// reach the group.
void windowAndGroupIdentity() {
    using namespace commands;
    const auto plain = writeSvg("b3-identity", sharpSvg(""));
    // Fitted (the artwork fills the frame), masked, scaled 4x: most of the 4x picture is off the
    // frame. The pan moves the window across it.
    Fixture layer(plain, SvgImportMode::Layer, kB2Width, kB2Height);
    applyEdits(layer, layer.art, *layer.artworkNode, {.scale = {4.0, 4.0}, .mask = true}, 1.0);
    const auto position = layer.layerParameter(layer.art, document::kPositionParameterRole);
    layer.run<SetKeyframeAtTimeForParameter>(layer.main, position, frame(2), document::Vec2d{kB2Centre.x + 300.0, kB2Centre.y + 90.0});
    layer.run<SetKeyframeAtTimeForParameter>(layer.main, position, frame(0), kB2Centre);
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    runtime::CompiledPlanCache plans;
    const auto panned = plans.compile(compiler, {layer.document.snapshot(), layer.main}, {});
    const auto contentScaleOf = [](const runtime::CompiledCompositionPlan& plan, const document::LayerId id) {
        for (const auto& operation : plan.operations())
            if (const auto* output = std::get_if<runtime::CompiledLayerOutput>(&operation); output && output->layerId == id)
                return output->contentScale;
        return 0.0;
    };
    expect(panned.plan != nullptr && contentScaleOf(*panned.plan, layer.art) == 4.0, "window: the panned layer is drawn at 4x");
    if (!panned.plan)
        return;
    runtime::CpuCompositionEvaluator evaluator;
    const auto hitsAt = [&](const std::int64_t index) {
        auto request = referenceRequest(*panned.plan, frame(index));
        request.bypassOperationCache = false;
        const auto before = evaluator.operationCache()->statistics().hits;
        const auto result = evaluator.evaluate(panned.plan, request, {});
        expect(result.frame() != nullptr, "window: frame " + std::to_string(index) + " evaluates");
        return evaluator.operationCache()->statistics().hits - before;
    };
    static_cast<void>(hitsAt(0));
    const auto movedHits = hitsAt(2);
    const auto again = plans.compile(compiler, {layer.document.snapshot(), layer.main}, {});
    expect(again.plan == panned.plan && plans.statistics().compiles == 1, "window: a pan never compiles again");
    const auto shownAgain = hitsAt(0);
    std::cout << "window: frame 2 (panned) after frame 0: " << movedHits << " operation-cache hits; frame 0 shown again: "
              << shownAgain << " hits; compiles " << plans.statistics().compiles << "\n";
    expect(shownAgain > movedHits,
           "window: the window is part of each frame's key: a frame showing another part draws it (" +
               std::to_string(movedHits) + " hits), a frame shown again reuses its pictures (" + std::to_string(shownAgain) +
               " hits)");

    // The group: its layer keyed from 1.5x to 3x renders every frame from one plan in the 4x band.
    Fixture grouped(plain, SvgImportMode::Layer, kB2Width, kB2Height);
    unfit(grouped);
    const auto group = groupAround(grouped, *grouped.artworkNode);
    const auto groupScale = grouped.layerParameter(grouped.art, document::kScaleParameterRole);
    grouped.run<SetKeyframeAtTimeForParameter>(grouped.main, groupScale, frame(2), document::Vec2d{3.0, 3.0});
    grouped.run<SetKeyframeAtTimeForParameter>(grouped.main, groupScale, frame(0), document::Vec2d{1.5, 1.5});
    runtime::CompiledPlanCache groupPlans;
    const auto keyed = groupPlans.compile(compiler, {grouped.document.snapshot(), grouped.main}, {});
    expect(keyed.plan != nullptr && contentScaleOf(*keyed.plan, grouped.art) == 4.0 &&
               contentScaleOf(*keyed.plan, group.inner) == 4.0,
           "group identity: a group keyed from 1.5x to 3x is drawn in the 4x band");
    std::vector<Frame> frames;
    for (const std::int64_t index : {0, 1, 2})
        if (keyed.plan)
            if (const auto evaluated = evaluate(keyed.plan, frame(index), "group frame " + std::to_string(index)))
                frames.push_back(pixelsOf(*evaluated->frame));
    const auto twice = groupPlans.compile(compiler, {grouped.document.snapshot(), grouped.main}, {});
    expect(frames.size() == 3 && difference(frames[0], frames[2]) > 1e-3 && twice.plan == keyed.plan &&
               groupPlans.statistics().compiles == 1,
           "group identity: the keyed group changes every frame from one plan");
    grouped.run<SetKeyframeAtTimeForParameter>(grouped.main, groupScale, frame(2), document::Vec2d{6.0, 6.0});
    const auto moved = groupPlans.compile(compiler, {grouped.document.snapshot(), grouped.main}, {});
    expect(moved.plan != nullptr && contentScaleOf(*moved.plan, grouped.art) == 8.0,
           "group identity: a key past 4x moves the group to the 8x band");

    // Cancellation and the request's budget.
    const auto token = cancelledToken();
    const auto cancelledCompile = compiler.compile({grouped.document.snapshot(), grouped.main}, token);
    expect(cancelledCompile.status == runtime::SnapshotCompileStatus::Cancelled && cancelledCompile.plan == nullptr,
           "group cancel: a cancelled compile of a grouped artwork reports Cancelled and no plan");
    if (moved.plan) {
        const auto result = runtime::CpuCompositionEvaluator{}.evaluate(moved.plan, referenceRequest(*moved.plan, frame(2)), token);
        expect(result.status() == runtime::EvaluationStatus::Cancelled && result.frame() == nullptr,
               "group cancel: a cancelled evaluation of a larger group publishes no frame");
        auto request = referenceRequest(*moved.plan, frame(2));
        request.pixelStorageByteLimit = 4096;
        const auto refused = runtime::CpuCompositionEvaluator{}.evaluate(moved.plan, request, {});
        std::string reason;
        for (const auto& diagnostic : refused.diagnostics())
            reason += diagnostic.summary + " ";
        expect(refused.frame() == nullptr && refused.status() == runtime::EvaluationStatus::Failed &&
                   reason.find("budget") != std::string::npos,
               "group budget: a request too small for the larger group is refused, the budget named: " + reason);
    }
    std::filesystem::remove(plain);
}

} // namespace

int main() {
    try {
        layerComposesThroughTheExpansionRoot();
        oneLayerIsTheEditableImport();
        savedAndReopenedWithTheFileGone();
        sharpWhenScaledAndTurned();
        boundsFollowTheLayer();
        oneCompilePerRevisionAndOneParsePerFile();
        cancellation();
        budgetAdmissionAndRetirement();
        unreadableArtworkIsRefusedByName();
        whatResamplesAndWhatFollows();
        sharpUnderEveryLayerEdit();
        sharpOnAContentPivot();
        namedWhenItCannotBeSharp();
        oversampledCacheIdentity();
        sharpInsideScaledGroups();
        groupBoundsAndSiblings();
        groupsNamedWhenTheyCannotBe();
        windowAndGroupIdentity();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "PASS: a Vector Artwork layer compiles, composes, renders sharp and reopens as the editable import does\n";
    return EXIT_SUCCESS;
}
