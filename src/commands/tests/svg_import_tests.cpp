#include "command_test_support.hpp"

#include <bloom/commands/data_block_operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/document/svg_artwork.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/media/svg_scene.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/font_id.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/platform/font_catalog.hpp>

#include <algorithm>
#include <iostream>
#include <map>
#include <optional>
#include <iterator>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <utility>

// #343: an SVG file imports as native layers, in one undo step, and a hostile or unreadable file
// leaves the document alone.

namespace bloom::commands::test {
namespace {

std::filesystem::path writeFile(const std::string& name, const std::string& text) {
    const auto path = std::filesystem::temp_directory_path() / ("bloom-svg-import-" + name + ".svg");
    std::ofstream(path, std::ios::binary) << text;
    return path;
}

const Composition& compositionOf(const Document& document, const CompositionId id) {
    const auto* composition = document.snapshot().project().findComposition(id);
    requireFixture(composition != nullptr, "composition fixture");
    return *composition;
}

// What a rejected import must leave unchanged: the size of the graph and of the parameter store.
std::tuple<std::size_t, std::size_t, std::size_t> signature(const Document& document,
                                                            const CompositionId id) {
    const auto& composition = compositionOf(document, id);
    return {composition.graph().nodes().size(), composition.graph().layerOutputs().size(),
            composition.parameters().records().size()};
}

std::size_t countNodes(const Composition& composition, const std::string_view type) {
    return static_cast<std::size_t>(std::ranges::count_if(
        composition.graph().nodes(), [&](const auto& node) { return node.typeId == type; }));
}

bool hasLayerNamed(const Composition& composition, const std::string_view name) {
    return std::ranges::any_of(composition.graph().layerOutputs(),
                               [&](const auto& layer) { return layer.name == name; });
}

CommandResult importFile(Document& document, CommandStack& stack, const CompositionId composition,
                         const std::filesystem::path& path, const bool fit = true) {
    Transaction transaction("Import SVG", document.snapshot().revision());
    transaction.emplace<AddSvgArtwork>(composition, nullptr, path, "art", fit, SvgImportMode::Shapes);
    return stack.execute(std::move(transaction));
}

void structureAndUndo(TestContext& test) {
    auto initial = document::makeNewProject("Svg", "Main", core::RationalTime::fromInteger(4));
    const auto composition = initial.initialCompositionId;
    Document document(std::move(initial.project));
    CommandStack stack(document);
    const auto sigBefore = signature(document, composition);
    const auto path = writeFile("structure", R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
<g id="Group"><rect id="Box" width="40" height="30" fill="#f00"/>
<path id="Ring" fill="#0f0" fill-rule="evenodd" d="M60 10H100V50H60Z M70 20H90V40H70Z"/></g>
<circle id="Dot" cx="150" cy="50" r="20" fill="url(#g)" stroke="#00f" stroke-width="4"/>
<text id="Label" x="10" y="90" font-size="12">Hi</text>
<defs><linearGradient id="g"><stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#000"/></linearGradient></defs></svg>)svg");
    const auto result = importFile(document, stack, composition, path);
    test.expect(result.succeeded(), "a well-formed file imports");
    const auto& imported = compositionOf(document, composition);
    test.expect(countNodes(imported, document::kPathValueNodeType) == 3,
                "every drawn shape has its own Path node");
    test.expect(countNodes(imported, document::kTextSourceNodeType) == 1, "text becomes a Text layer");
    test.expect(hasLayerNamed(imported, "Group") && hasLayerNamed(imported, "Box") &&
                    hasLayerNamed(imported, "Ring") && hasLayerNamed(imported, "Dot") &&
                    hasLayerNamed(imported, "Label"),
                "names come from the file");
    test.expect(countNodes(imported, document::kLinearGradientNodeType) == 1,
                "a gradient fill becomes a Gradient node");
    const auto ring = std::ranges::find_if(imported.graph().nodes(), [&](const auto& node) {
        return node.typeId == document::kPathValueNodeType;
    });
    bool compound = false;
    for (const auto& node : imported.graph().nodes())
        if (node.typeId == document::kPathValueNodeType)
            for (const auto& binding : node.parameters)
                if (const auto* record = imported.parameters().find(binding.parameterId))
                    if (const auto* constant = std::get_if<ConstantValueSource>(&record->source))
                        if (const auto* value = std::get_if<document::PathValue>(&constant->value))
                            compound = compound || value->contours.size() == 2;
    (void)ring;
    test.expect(compound, "a compound shape keeps both contours in one editable path value");
    test.expect(imported.graph().validate(imported.parameters()).ok(), "the result validates");
    test.expect(stack.undo().changed() && signature(document, composition) == sigBefore,
                "one undo removes the whole import");
    test.expect(stack.redo().changed() && countNodes(compositionOf(document, composition),
                                                      document::kPathValueNodeType) == 3,
                "redo brings it back");
    std::filesystem::remove(path);
}

void hostileFilesLeaveTheDocumentAlone(TestContext& test) {
    auto initial = document::makeNewProject("Svg", "Main", core::RationalTime::fromInteger(4));
    const auto composition = initial.initialCompositionId;
    Document document(std::move(initial.project));
    CommandStack stack(document);
    const auto sigBefore = signature(document, composition);
    const auto refuse = [&](const std::string& name, const std::string& text, const char* what) {
        const auto path = writeFile(name, text);
        const auto result = importFile(document, stack, composition, path);
        test.expect(result.status == CommandStatus::Rejected && signature(document, composition) == sigBefore,
                    what);
        std::filesystem::remove(path);
    };
    refuse("bomb",
           R"(<!DOCTYPE svg [<!ENTITY a "aaaaaaaaaa"><!ENTITY b "&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;">]><svg xmlns="http://www.w3.org/2000/svg"><text>&b;</text></svg>)",
           "an entity bomb is refused");
    refuse("external",
           R"(<!DOCTYPE svg [<!ENTITY x SYSTEM "file:///etc/passwd">]><svg xmlns="http://www.w3.org/2000/svg"><text>&x;</text></svg>)",
           "an external entity is refused");
    refuse("broken", "<svg xmlns=\"http://www.w3.org/2000/svg\"><g></svg>", "malformed XML is refused");
    refuse("notsvg", "<html></html>", "a file that is not an SVG is refused");
    std::string deep = "<svg xmlns=\"http://www.w3.org/2000/svg\">";
    for (int i = 0; i < 300; ++i)
        deep += "<g>";
    deep += "<rect width=\"1\" height=\"1\"/>";
    for (int i = 0; i < 300; ++i)
        deep += "</g>";
    deep += "</svg>";
    refuse("deep", deep, "absurd nesting is refused");
    const auto missing = std::filesystem::temp_directory_path() / "bloom-svg-import-missing.svg";
    std::filesystem::remove(missing);
    test.expect(importFile(document, stack, composition, missing).status == CommandStatus::Rejected &&
                    signature(document, composition) == sigBefore,
                "a missing file is refused");
}

void scriptsAndExternalImagesDoNothing(TestContext& test) {
    auto initial = document::makeNewProject("Svg", "Main", core::RationalTime::fromInteger(4));
    const auto composition = initial.initialCompositionId;
    Document document(std::move(initial.project));
    CommandStack stack(document);
    const auto path = writeFile("script", R"svg(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="10" height="10">
<script>alert(1)</script><image xlink:href="http://example.invalid/a.png" width="5" height="5"/>
<use xlink:href="http://example.invalid/b.svg#x"/><rect id="Kept" width="5" height="5"/></svg>)svg");
    const auto result = importFile(document, stack, composition, path);
    test.expect(result.succeeded() && hasLayerNamed(compositionOf(document, composition), "Kept"),
                "the drawable content imports");
    const auto warned = std::ranges::count_if(result.warnings, [](const auto& failure) {
        return failure.issue.message.find("script") != std::string::npos ||
               failure.issue.message.find("image") != std::string::npos ||
               failure.issue.message.find("external") != std::string::npos;
    });
    test.expect(warned >= 3, "the script, the image and the external reference are each named");
    std::filesystem::remove(path);
}

void manySiblingsAreHeldInRuns(TestContext& test) {
    auto initial = document::makeNewProject("Svg", "Main", core::RationalTime::fromInteger(4));
    const auto composition = initial.initialCompositionId;
    Document document(std::move(initial.project));
    CommandStack stack(document);
    std::string text = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\">";
    for (int i = 0; i < 600; ++i)
        text += "<rect id=\"r" + std::to_string(i) + "\" x=\"" + std::to_string(i % 90) + "\" y=\"" +
                std::to_string(i / 7 % 90) + "\" width=\"5\" height=\"5\"/>";
    text += "</svg>";
    const auto path = writeFile("many", text);
    const auto result = importFile(document, stack, composition, path);
    test.expect(result.succeeded(), "600 shapes import");
    const auto& imported = compositionOf(document, composition);
    test.expect(countNodes(imported, document::kPathValueNodeType) == 600 &&
                    countNodes(imported, document::kLayerStackNodeType) >= 4,
                "600 siblings are held as runs of at most 256, each its own Merge");
    test.expect(hasLayerNamed(imported, "r0") && hasLayerNamed(imported, "r599"),
                "every shape keeps its name");
    std::filesystem::remove(path);
}

// #343: an element and a group turn about their own centres, a Gaussian blur filter becomes a Gaussian
// Blur node, text is set in the file's font when it is installed, and what is not applied is named by
// id or label, or by where it sits.
const document::ParameterValue* constantOf(const Composition& composition, const document::ParameterId id) {
    const auto* record = composition.parameters().find(id);
    const auto* constant = record ? std::get_if<ConstantValueSource>(&record->source) : nullptr;
    return constant ? &constant->value : nullptr;
}

void pivotsBlurFontsAndNames(TestContext& test) {
    auto initial = document::makeNewProject("Svg", "Main", core::RationalTime::fromInteger(4));
    const auto composition = initial.initialCompositionId;
    Document document(std::move(initial.project));
    CommandStack stack(document);
    const auto path = writeFile("pivots", R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
<defs><filter id="soft"><feGaussianBlur stdDeviation="2"/></filter>
<filter id="shadow"><feOffset dx="2" dy="2"/><feGaussianBlur stdDeviation="2"/></filter></defs>
<rect id="Box" x="20" y="10" width="40" height="20" fill="#f00"/>
<g id="Pair"><rect id="A" x="100" y="10" width="20" height="20"/><rect id="B" x="140" y="50" width="20" height="30"/></g>
<rect id="Soft" x="20" y="60" width="30" height="30" filter="url(#soft)"/>
<rect x="70" y="60" width="30" height="30" filter="url(#shadow)"/>
<text id="Real" x="10" y="95" font-size="12" font-family="Test Sans" font-weight="700" font-style="italic">Hi</text>
<text id="Lost" x="60" y="95" font-size="12" font-family="No Such Family">Hi</text>
</svg>)svg");
    auto fonts = std::make_shared<platform::FontCatalogue>();
    fonts->status = platform::FontCatalogueStatus::Available;
    platform::FontFace bold;
    bold.family = "Test Sans";
    bold.style = "Bold";
    bold.weight = 700;
    bold.postscriptName = "TestSans-Bold";
    bold.path = "/nonexistent/TestSans-Bold.ttf";
    fonts->faces.push_back(bold);
    Transaction transaction("Import SVG", document.snapshot().revision());
    transaction.emplace<AddSvgArtwork>(composition, nullptr, path, "art", true, SvgImportMode::Shapes).withFonts(fonts);
    const auto result = stack.execute(std::move(transaction));
    test.expect(result.succeeded(), "the file imports");
    const auto& imported = compositionOf(document, composition);
    const auto& graph = imported.graph();
    const auto layerNamed = [&](const std::string& name) -> const document::LayerOutputBoundary& {
        for (const auto& layer : graph.layerOutputs())
            if (layer.name == name)
                return layer;
        requireFixture(false, "layer " + name);
        return graph.layerOutputs().front();
    };
    const auto vec2 = [&](const std::string& name, const std::string_view role) {
        const auto* binding = document::layerParameterBinding(graph, layerNamed(name), role);
        const auto* value = binding ? constantOf(imported, binding->parameterId) : nullptr;
        const auto* vec = value ? std::get_if<document::Vec2d>(value) : nullptr;
        return vec ? *vec : document::Vec2d{-1e9, -1e9};
    };
    // The composition is 1920 by 1080 here, so the file is fitted by 5.4 about the centre of the frame.
    const double scale = std::min(1920.0 / 200.0, 1080.0 / 100.0);
    const double offsetX = (1920.0 - 200.0 * scale) / 2.0, offsetY = (1080.0 - 100.0 * scale) / 2.0;
    const auto boxCentre = document::Vec2d{offsetX + 40.0 * scale, offsetY + 20.0 * scale};
    test.expect(std::abs(vec2("Box", document::kPositionParameterRole).x - boxCentre.x) < 0.07 &&
                    std::abs(vec2("Box", document::kPositionParameterRole).y - boxCentre.y) < 0.07,
                "an element's Position is the centre of its bounds");
    test.expect(std::abs(vec2("Box", document::kAnchorParameterRole).x - (boxCentre.x - 960.0)) < 0.07 &&
                    std::abs(vec2("Box", document::kAnchorParameterRole).y - (boxCentre.y - 540.0)) < 0.07,
                "its Anchor offsets the frame's centre to that pivot");
    const auto pairCentre = document::Vec2d{offsetX + 100.0 * scale + 30.0 * scale, offsetY + 10.0 * scale + 35.0 * scale};
    test.expect(std::abs(vec2("Pair", document::kPositionParameterRole).x - pairCentre.x) < 0.07 &&
                    std::abs(vec2("Pair", document::kPositionParameterRole).y - pairCentre.y) < 0.07,
                "a group's Position is the centre of its children's bounds");
    test.expect(layerNamed("Box").pivot == document::LayerPivot::Frame,
                "the pivot is a fixed point on a frame-pivot layer, so editing points moves nothing else");

    // The blur: one node, three times the deviation at the fit scale.
    const auto blur = std::ranges::find_if(graph.nodes(), [](const auto& node) {
        return node.typeId == "bloom.gaussian-blur";
    });
    test.expect(countNodes(imported, "bloom.gaussian-blur") == 1 && blur != graph.nodes().end(),
                "one single-blur filter becomes one Gaussian Blur node, and the shadow filter none");
    if (blur != graph.nodes().end()) {
        const auto binding = std::ranges::find(blur->parameters, "radius", &document::ParameterBinding::role);
        const auto* value = binding != blur->parameters.end() ? constantOf(imported, binding->parameterId) : nullptr;
        const auto* radius = value ? std::get_if<double>(value) : nullptr;
        test.expect(radius != nullptr && std::abs(*radius - 6.0 * scale) < 1e-6,
                    "its radius is three times the file's deviation at the fit scale");
    }

    // The warnings name each element: by id when it has one, by place when it has not.
    const auto warned = [&](const std::string& needle) {
        return std::ranges::any_of(result.warnings, [&](const auto& failure) {
            return failure.issue.message.find(needle) != std::string::npos;
        });
    };
    test.expect(warned("rect 5 in the file's root: the filter #shadow (feGaussianBlur, feOffset)"),
                "an element without an id is named by its place: " +
                    [&] {
                        std::string all;
                        for (const auto& w : result.warnings)
                            all += " | " + w.issue.message;
                        return all;
                    }());
    test.expect(warned("Soft: the filter region of #soft") && !warned("Soft: the filter #") && !warned("rect 3 in"),
                "an element with an id keeps its id, and a blur the node can hold is not listed as unapplied");
    test.expect(warned("Lost: the font 'No Such Family' is not installed"),
                "a font that is not installed is listed with the element's name");
    test.expect(warned("Real: 'Test Sans' has no italic weight 700"), "a missing slope is listed");
    test.expect(!warned("Real: the font"), "an installed font is not substituted");

    // The text that found its face carries it.
    bool realFont = false, defaultFont = false;
    for (const auto& node : graph.nodes()) {
        if (node.typeId != document::kTextSourceNodeType)
            continue;
        const auto font = std::ranges::find(node.parameters, document::kTextFontParameterRole,
                                            &document::ParameterBinding::role);
        const auto text = std::ranges::find(node.parameters, document::kTextParameterRole,
                                            &document::ParameterBinding::role);
        if (font == node.parameters.end() || text == node.parameters.end())
            continue;
        const auto* fontValue = constantOf(imported, font->parameterId);
        const auto* name = fontValue ? std::get_if<std::string>(fontValue) : nullptr;
        const auto id = name ? document::decodeFontId(*name) : std::nullopt;
        if (id && id->family == "Test Sans" && id->weight == 700 && id->source == document::FontSource::System)
            realFont = true;
        if (id && id->family == document::defaultFontId().family)
            defaultFont = true;
    }
    test.expect(realFont, "text found in the catalogue is set in that face");
    test.expect(defaultFont, "text whose family is not installed keeps Bloom's default font");
    std::filesystem::remove(path);
}


// ---- #642: as one layer ----

constexpr const char* kLayerArtwork = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
<defs><linearGradient id="g"><stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#000"/></linearGradient></defs>
<g id="Group"><rect id="Box" width="40" height="30" fill="#f00"/>
<path id="Ring" fill="#0f0" fill-rule="evenodd" d="M60 10H100V50H60Z M70 20H90V40H70Z"/></g>
<circle id="Dot" cx="150" cy="50" r="20" fill="url(#g)" stroke="#00f" stroke-width="4"/>
<line id="Dash" x1="0" y1="95" x2="200" y2="95" stroke="#000" stroke-dasharray="4 2 1"/>
<foreignObject id="Embed" width="10" height="10"/></svg>)svg";

std::shared_ptr<const platform::FontCatalogue> noFonts() {
    auto fonts = std::make_shared<platform::FontCatalogue>();
    fonts->status = platform::FontCatalogueStatus::Available;
    return fonts;
}

std::vector<std::byte> fileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::ranges::transform(chars, bytes.begin(), [](const char c) { return static_cast<std::byte>(c); });
    return bytes;
}

struct Session final {
    document::NewProject initial = document::makeNewProject("Svg", "Main", core::RationalTime::fromInteger(4));
    CompositionId composition = initial.initialCompositionId;
    Document document{std::move(initial.project)};
    CommandStack stack{document};

    CommandResult import(const std::filesystem::path& path, std::optional<SvgImportMode> mode = std::nullopt) {
        Transaction transaction("Import SVG", document.snapshot().revision());
        auto& operation = mode ? transaction.emplace<AddSvgArtwork>(composition, nullptr, path, "art", true, *mode)
                               : transaction.emplace<AddSvgArtwork>(composition, nullptr, path, "art");
        operation.withFonts(noFonts()).withProjectDirectory(path.parent_path());
        return stack.execute(std::move(transaction));
    }
    const Composition& main() const { return compositionOf(document, composition); }
    const document::Project& project() const { return document.snapshot().project(); }
};

std::vector<std::string> warningsOf(const CommandResult& result) {
    std::vector<std::string> out;
    for (const auto& warning : result.warnings)
        out.push_back(warning.issue.message);
    return out;
}

template <typename Id> std::optional<Id> outputOf(const CommandResult& result, const std::string_view name) {
    return result.outputId<Id>(name);
}

void oneLayerIsTheDefault(TestContext& test) {
    const auto path = writeFile("one-layer", kLayerArtwork);
    Session session;
    const auto nodesBefore = session.main().graph().nodes().size();
    const auto layersBefore = session.main().graph().layerOutputs().size();
    const auto result = session.import(path);
    test.expect(result.succeeded(), "an SVG imports as one layer by default");
    const auto& imported = session.main();
    test.expect(imported.graph().nodes().size() == nodesBefore + 1 &&
                    imported.graph().layerOutputs().size() == layersBefore + 1,
                "one layer and one node join the graph");
    test.expect(countNodes(imported, document::kVectorArtworkNodeType) == 1 &&
                    countNodes(imported, document::kPathValueNodeType) == 0 &&
                    countNodes(imported, document::kShapeSourceNodeType) == 0 &&
                    countNodes(imported, document::kLayerStackNodeType) == 1,
                "the parts are not nodes in the graph: no Path, Shape or Merge node of the file");
    test.expect(hasLayerNamed(imported, "art"), "the layer is named after the file");
    const auto node = outputOf<document::NodeId>(result, "node");
    const auto blockId = outputOf<document::DataBlockRecordId>(result, "dataBlock");
    const auto asset = outputOf<document::AssetId>(result, "asset");
    test.expect(node && blockId && asset && outputOf<LayerId>(result, "layer"), "the import names its layer, node, block and asset");
    if (!node || !blockId || !asset)
        return;
    const auto* record = imported.graph().findNode(*node);
    const auto* block = record ? document::vectorArtworkBlock(session.project(), *record, imported.parameters()) : nullptr;
    test.expect(block != nullptr && std::get<document::DataBlockRecordId>(block->id) == *blockId,
                "the Vector Artwork node names its data block");
    if (block == nullptr)
        return;
    const auto bytes = fileBytes(path);
    const auto& payload = std::get<document::OpaqueExtensionPayload>(block->payload);
    test.expect(std::ranges::equal(payload.bytes(), bytes), "the block holds the file's bytes exactly");
    core::Sha256Hasher hasher;
    (void)hasher.update(bytes);
    const auto digest = hasher.finalize();
    test.expect(block->provenance.contentDigest == digest && block->mediaType == "image/svg+xml" &&
                    block->typeId == document::kVectorArtworkBlockTypeId,
                "the block records the bytes' SHA-256 and SVG's media type");
    const auto* source = session.project().findAsset(*asset);
    test.expect(source != nullptr && source->contentDigest == digest &&
                    source->locator.path == path.filename().generic_string() &&
                    document::vectorArtworkSourceAsset(session.project(), *block) == source,
                "the Assets record holds the project-relative path and the SHA-256, as the editable import's does");
    test.expect(document::vectorArtworkFits(*record, imported.parameters()), "fit is on by default");
    test.expect(session.project().validate().ok(), "the project validates");

    // Undo removes the layer, the node, the block and the record in one step; redo brings them back.
    test.expect(session.stack.undo().changed(), "one undo");
    test.expect(countNodes(session.main(), document::kVectorArtworkNodeType) == 0 &&
                    session.main().graph().layerOutputs().size() == layersBefore &&
                    session.project().typedDataBlocks().empty() && session.project().assets().empty(),
                "one undo removes the whole import");
    test.expect(session.stack.redo().changed() && countNodes(session.main(), document::kVectorArtworkNodeType) == 1 &&
                    session.project().typedDataBlocks().size() == 1 && session.project().assets().size() == 1,
                "redo brings it back");
    std::filesystem::remove(path);
}

void bothModesNameTheSameWarnings(TestContext& test) {
    const auto path = writeFile("warnings", kLayerArtwork);
    Session layer;
    Session shapes;
    const auto one = layer.import(path, SvgImportMode::Layer);
    const auto editable = shapes.import(path, SvgImportMode::Shapes);
    test.expect(one.succeeded() && editable.succeeded(), "the file imports in both modes");
    const auto names = warningsOf(one);
    test.expect(!names.empty() && names == warningsOf(editable), "both modes name the same warnings, in the same order");
    const auto named = [&](const std::string_view text) {
        return std::ranges::any_of(names, [&](const std::string& note) { return note.find(text) != std::string::npos; });
    };
    test.expect(named("Embed") && named("Dash"), "what the file holds and the artwork does not take is named");
    test.expect(countNodes(shapes.main(), document::kPathValueNodeType) == 4 &&
                    countNodes(shapes.main(), document::kVectorArtworkNodeType) == 0,
                "as editable shapes is the breakdown it always was");
    // The expansion of the one layer holds exactly the nodes the editable import made; the layer's
    // transform drawn through the geometry (the default, #642) adds one carrier layer and nothing else.
    document::VectorArtworkExpansionContext context;
    context.fonts = noFonts();
    const auto composed = document::expandVectorArtwork(layer.project(), layer.main(), context);
    context.composeLayerTransform = false;
    const auto expansion = document::expandVectorArtwork(layer.project(), layer.main(), context);
    if (composed.composition && expansion.composition) {
        std::map<std::string, std::size_t> withCarrier, without;
        for (const auto& node : composed.composition->graph().nodes())
            ++withCarrier[node.typeId];
        for (const auto& node : expansion.composition->graph().nodes())
            ++without[node.typeId];
        ++without[std::string(document::kLayerOutputNodeType)];
        test.expect(composed.composed.size() == 1 && composed.resampled.empty() && withCarrier == without &&
                        composed.composition->graph().layerOutputs().size() ==
                            expansion.composition->graph().layerOutputs().size() + 1 &&
                        composed.composition->graph().validate(composed.composition->parameters()).ok(),
                    "the composed expansion adds one carrier layer to the same nodes and validates");
    } else {
        test.expect(false, "the layer expands with and without its transform composed");
    }
    test.expect(expansion.composition && expansion.unresolved.empty() && expansion.roots.size() == 1,
                "the layer expands");
    if (expansion.composition) {
        std::map<std::string, std::size_t> expanded, editableTypes;
        for (const auto& node : expansion.composition->graph().nodes())
            ++expanded[node.typeId];
        for (const auto& node : shapes.main().graph().nodes())
            ++editableTypes[node.typeId];
        test.expect(expanded == editableTypes, "the expansion makes the editable import's nodes, type for type");
        test.expect(expansion.composition->graph().layerOutputs().size() == shapes.main().graph().layerOutputs().size(),
                    "and its layers, layer for layer");
        test.expect(expansion.composition->graph().validate(expansion.composition->parameters()).ok(),
                    "the expanded graph validates");
        const auto notes = expansion.notes.begin()->second.listed;
        test.expect(std::ranges::all_of(notes, [&](const std::string& note) {
                        return std::ranges::find(names, note) != names.end();
                    }),
                    "the expansion names the import's warnings");
    }
    std::filesystem::remove(path);
}

void oneBlockPerFile(TestContext& test) {
    const auto path = writeFile("shared", kLayerArtwork);
    Session session;
    test.expect(session.import(path).succeeded() && session.import(path).succeeded(), "the file imports twice");
    test.expect(session.project().typedDataBlocks().size() == 1 && session.project().assets().size() == 1,
                "the same bytes are held once and recorded once");
    test.expect(countNodes(session.main(), document::kVectorArtworkNodeType) == 2, "two layers read them");
    test.expect(session.import(path, SvgImportMode::Shapes).succeeded() && session.project().assets().size() == 1,
                "an editable import of the same file reuses the record");
    // A block a layer reads cannot be removed from under it.
    const auto id = std::get<document::DataBlockRecordId>(session.project().typedDataBlocks().front().id);
    Transaction remove("Remove block", session.document.snapshot().revision());
    remove.emplace<RemoveDataBlock>(id);
    test.expect(!session.stack.execute(std::move(remove)).succeeded() && session.project().typedDataBlocks().size() == 1,
                "the block a Vector Artwork layer reads cannot be removed");
    std::filesystem::remove(path);
}

void oneLayerRefusesWhatItCannotHold(TestContext& test) {
    Session session;
    const auto missing = std::filesystem::temp_directory_path() / "bloom-svg-import-no-such-file.svg";
    std::filesystem::remove(missing);
    const auto refused = session.import(missing);
    test.expect(!refused.succeeded() && countNodes(session.main(), document::kVectorArtworkNodeType) == 0,
                "a file that cannot be read adds nothing");
    // A project already holding 128 MiB of embedded data takes no more. Two blocks share one buffer.
    {
        const document::OpaqueExtensionPayload large{std::vector<std::byte>(document::kMaxDataBlockPayloadBytes)};
        Transaction fill("Fill", session.document.snapshot().revision());
        for (int index = 0; index < 2; ++index) {
            document::DataBlockRecord block;
            block.kind = document::DataBlockKind::Opaque;
            block.typeId = "test.filler";
            block.schemaVersion = {1, 0};
            block.mediaType = "application/octet-stream";
            block.provenance.source = document::DataBlockProducer{"test.filler", "1", {}};
            block.payload = large;
            fill.emplace<CreateDataBlock>(std::move(block));
        }
        requireFixture(session.stack.execute(std::move(fill)).succeeded(), "filler blocks");
    }
    const auto path = writeFile("full", kLayerArtwork);
    const auto full = session.import(path);
    const bool named = !full.operationFailures.empty() &&
                       full.operationFailures.front().issue.message.find("bloom-svg-import-full.svg") != std::string::npos &&
                       full.operationFailures.front().issue.message.find("editable shapes") != std::string::npos;
    test.expect(!full.succeeded() && named, "an SVG that does not fit is refused by name, offering editable shapes");
    test.expect(countNodes(session.main(), document::kVectorArtworkNodeType) == 0 && session.project().assets().empty(),
                "and nothing is added");
    test.expect(session.import(path, SvgImportMode::Shapes).succeeded(), "editable shapes need no embedded copy");
    std::filesystem::remove(path);
}

void preparedSourcesReadNothingLater(TestContext& test) {
    const auto path = writeFile("prepared", kLayerArtwork);
    std::vector<SvgPreparePhase> phases;
    const auto read = prepareSvgSource(path, {}, [&](const SvgPreparePhase phase) { phases.push_back(phase); });
    test.expect(read.source && read.source->bytes && read.source->scene && !read.source->fonts,
                "a file without text is read, hashed and parsed, and no fonts are enumerated");
    test.expect(phases == std::vector<SvgPreparePhase>{SvgPreparePhase::Reading}, "the phase is reported");
    if (read.source) {
        test.expect(read.source->scene->hasSourceDigest && read.source->scene->sourceDigest == read.source->digest &&
                        read.source->scene->sourcePath == path,
                    "the scene names its file and digest");
        test.expect(std::ranges::any_of(read.source->warnings,
                                        [](const SvgSourceWarning& warning) {
                                            return warning.code == "bloom.svg.not-imported" &&
                                                   warning.summary.find("Embed") != std::string::npos;
                                        }),
                    "the job's warnings are named");
    }
    // The file is gone before the import applies: nothing reads it again.
    std::filesystem::remove(path);
    Session session;
    Transaction transaction("Import SVG", session.document.snapshot().revision());
    transaction.emplace<AddSvgArtwork>(session.composition, read.source, "art", SvgImportMode::Layer)
        .withFonts(noFonts());
    test.expect(session.stack.execute(std::move(transaction)).succeeded() &&
                    countNodes(session.main(), document::kVectorArtworkNodeType) == 1,
                "a prepared file imports with the file gone");
    // Cancellation stops the read.
    const auto text = writeFile("prepared-text", R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20"><text x="1" y="10">A</text></svg>)svg");
    const auto cancelled = prepareSvgSource(text, [] { return true; });
    test.expect(!cancelled.source && cancelled.cancelled(), "a cancelled read yields nothing");
    std::filesystem::remove(text);
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    bloom::commands::test::structureAndUndo(test);
    bloom::commands::test::hostileFilesLeaveTheDocumentAlone(test);
    bloom::commands::test::scriptsAndExternalImagesDoNothing(test);
    bloom::commands::test::manySiblingsAreHeldInRuns(test);
    bloom::commands::test::pivotsBlurFontsAndNames(test);
    bloom::commands::test::oneLayerIsTheDefault(test);
    bloom::commands::test::bothModesNameTheSameWarnings(test);
    bloom::commands::test::oneBlockPerFile(test);
    bloom::commands::test::oneLayerRefusesWhatItCannotHold(test);
    bloom::commands::test::preparedSourcesReadNothingLater(test);
    return test.failures() == 0 ? 0 : 1;
}
