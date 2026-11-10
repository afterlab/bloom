// #642: an SVG imported as one layer (a Vector Artwork layer) against the same file imported as
// editable shapes, on the CPU reference.
//
// - The layer's own opacity (keyed), blend mode, transform, parent, track matte, mask and an effect
//   between its source and its slot compose through the expansion's root exactly as they compose
//   through the editable import's root group, whole frame, over a few frames.
// - The plain layer is the editable import, pixel for pixel, on files that use gradients, clips,
//   masks, opacity, blur, dashes and text.
// - Saved and reopened with the file deleted, the layer keeps its mode and its SVG and renders the
//   same frame.
//
// The compiler does not expand the layer here: the test expands it with
// document::expandVectorArtwork() and compiles the expanded composition, which is the expansion the
// compiler runs.

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
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
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
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
            throw std::runtime_error("a fixture edit was refused:" + why);
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

// The one-layer composition as the compiler reads it: every Vector Artwork node expanded.
std::optional<Frame> renderExpanded(const document::Document& document, const document::CompositionId id,
                                    const core::RationalTime time, const std::string& label) {
    const auto snapshot = document.snapshot();
    const auto* composition = snapshot.project().findComposition(id);
    document::VectorArtworkExpansionContext context;
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
    for (const std::int64_t index : {0, 1, 2}) {
        const auto label = "frame " + std::to_string(index);
        const auto editable = renderProject(shapes.document, shapes.main, frame(index), "editable " + label);
        const auto one = renderExpanded(layer.document, layer.main, frame(index), "one layer " + label);
        if (!editable || !one)
            continue;
        const auto worst = difference(*editable, *one);
        expect(worst <= kGate, label + ": opacity, blend, transform, parent, track matte, mask and effect compose "
                                       "through the expansion's root as through the editable root (worst " +
                                   std::to_string(worst) + ")");
        expect(coverage(*one) > 100.0, label + ": the dressed layer draws");
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

} // namespace

int main() {
    try {
        layerComposesThroughTheExpansionRoot();
        oneLayerIsTheEditableImport();
        savedAndReopenedWithTheFileGone();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "PASS: a Vector Artwork layer composes, renders and reopens as the editable import does\n";
    return EXIT_SUCCESS;
}
