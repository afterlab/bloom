// #642: the shared SVG artwork builder. The editable import and the Vector Artwork layer's
// expansion both write what buildSvgArtwork() describes, so the description itself is checked
// here: it is deterministic, every step names only what an earlier step made, every drawn element
// has its Path node, and the warnings are named.

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/svgartwork/svg_artwork.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/media/svg_scene.hpp>
#include <bloom/platform/font_catalog.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace bloom;
using namespace bloom::document;

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::string_view kArtwork = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
<defs><linearGradient id="g"><stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#000"/></linearGradient>
<clipPath id="c"><rect width="100" height="100"/></clipPath>
<filter id="b"><feGaussianBlur stdDeviation="2"/></filter></defs>
<g id="Group" opacity="0.5" clip-path="url(#c)"><rect id="Box" width="40" height="30" fill="#f00"/>
<path id="Ring" fill="#0f0" fill-rule="evenodd" d="M60 10H100V50H60Z M70 20H90V40H70Z"/></g>
<circle id="Dot" cx="150" cy="50" r="20" fill="url(#g)" stroke="#00f" stroke-width="4" filter="url(#b)"/>
<line id="Dash" x1="0" y1="95" x2="200" y2="95" stroke="#000" stroke-dasharray="4 2 1"/>
<text id="Label" x="10" y="90" font-family="sans-serif" font-size="12">Hi</text>
<foreignObject id="Embed" width="10" height="10"/></svg>)svg";

std::shared_ptr<const media::SvgScene> parse(const std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    auto read = media::parseSvgScene(bytes);
    if (!read.scene) {
        std::cerr << "fixture does not parse: " << read.diagnostic << '\n';
        std::exit(EXIT_FAILURE);
    }
    return read.scene;
}

SvgArtworkOptions options(const bool fit = true) {
    SvgArtworkOptions out;
    out.width = 400;
    out.height = 300;
    out.fit = fit;
    out.name = "art";
    // A fixed, empty catalogue: the test never depends on the machine's fonts.
    out.fonts = std::make_shared<const platform::FontCatalogue>();
    return out;
}

// Drawn elements anywhere in the scene, clip and mask content included.
std::size_t drawnElements(const media::SvgElement& element) {
    std::size_t count = element.kind == media::SvgElement::Kind::Shape ? 1 : 0;
    for (const auto& child : element.children)
        count += drawnElements(child);
    if (element.clip)
        count += drawnElements(*element.clip);
    if (element.mask)
        count += drawnElements(*element.mask);
    return count;
}

std::size_t countType(const SvgArtworkDescription& description, const std::string_view type) {
    return static_cast<std::size_t>(std::ranges::count_if(
        description.nodes, [&](const SvgArtworkNode& node) { return node.type == type; }));
}

void descriptionIsDeterministicAndOrdered() {
    const auto scene = parse(kArtwork);
    const auto first = buildSvgArtwork(*scene, options());
    const auto second = buildSvgArtwork(*scene, options());
    expect(first == second, "two builds of one scene describe the same artwork");
    expect(!first.nodes.empty() && first.nodes.front().type == kLayerStackNodeType,
           "the root (node 0) is a Merge");
    expect(countType(first, kPathValueNodeType) == drawnElements(scene->root),
           "every drawn element has its own Path node");
    expect(countType(first, kTextSourceNodeType) == 1, "the text stays a Text source");
    expect(countType(first, kLinearGradientNodeType) == 1, "the gradient fill is a Gradient node");
    expect(countType(first, "bloom.gaussian-blur") == 1, "the blur filter is a Gaussian Blur node");
    // Every step names only nodes, slots and layers made by an earlier step.
    std::vector<bool> made(first.nodes.size(), false);
    made[0] = true;
    std::size_t slots = 0, layers = 0;
    bool ordered = true;
    bool matte = false;
    for (const auto& step : first.steps) {
        const auto has = [&](const std::size_t node) { return node < made.size() && made[node]; };
        if (const auto* add = std::get_if<SvgArtworkAddNode>(&step)) {
            ordered = ordered && add->node < made.size() && !made[add->node];
            for (const auto& [role, value] : first.nodes[add->node].values)
                if (const auto* driver = std::get_if<SvgArtworkDriver>(&value))
                    ordered = ordered && has(driver->node);
            made[add->node] = true;
        } else if (const auto* slot = std::get_if<SvgArtworkAddSlot>(&step)) {
            ordered = ordered && has(slot->merge) && has(slot->source) &&
                      first.nodes[slot->merge].type == kLayerStackNodeType;
            ++slots;
        } else if (const auto* entry = std::get_if<SvgArtworkAddEntry>(&step)) {
            ordered = ordered && entry->slot < slots && has(entry->merge);
            ++layers;
        } else if (const auto* value = std::get_if<SvgArtworkSetLayerValue>(&step)) {
            ordered = ordered && value->layer < layers;
        } else if (const auto* set = std::get_if<SvgArtworkSetTrackMatte>(&step)) {
            ordered = ordered && set->layer < layers && set->matte < layers;
            matte = true;
        } else if (const auto* connect = std::get_if<SvgArtworkConnect>(&step)) {
            ordered = ordered && has(connect->source) && has(connect->destination);
        }
    }
    expect(ordered, "every step names only what an earlier step made");
    expect(slots == first.slotCount && layers == first.layerCount, "the counts match the steps");
    expect(std::ranges::all_of(made, [](const bool value) { return value; }), "every node is made once");
    expect(matte, "the clip and the gradient become track mattes");
    for (const auto& frame : first.frames)
        expect(std::ranges::is_sorted(frame.members) && std::ranges::adjacent_find(frame.members) == frame.members.end(),
               "a frame lists its members once, in order");
}

void notesAreNamed() {
    const auto scene = parse(kArtwork);
    const auto description = buildSvgArtwork(*scene, options());
    const auto names = [&](const std::string_view text) {
        return std::ranges::any_of(description.notes.listed,
                                   [&](const std::string& note) { return note.find(text) != std::string::npos; });
    };
    expect(names("Embed"), "an unsupported element is listed by name");
    // #759: a pattern of up to six lengths imports exactly, so only a longer one is noted.
    expect(!names("Dash:") && !names("first dash and gap"), "a three-length dash pattern is not noted");
    expect(names("Label") && names("generic font family"), "a generic font family is named on its text");
}

// #759: stroke-dasharray of up to six lengths is Path Stroke's three dash and gap pairs exactly (an
// odd count repeated, as SVG reads it); a longer one keeps its first six and is noted.
void dashPatternsImportExactly() {
    constexpr std::string_view kDashes = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
<line id="DashDot" x1="0" y1="10" x2="200" y2="10" stroke="#000" stroke-linecap="round" stroke-dasharray="12 4 0 4" stroke-dashoffset="3"/>
<line id="Dotted" x1="0" y1="30" x2="200" y2="30" stroke="#000" stroke-linecap="round" stroke-dasharray="0 6"/>
<line id="Three" x1="0" y1="50" x2="200" y2="50" stroke="#000" stroke-dasharray="4 2 1"/>
<line id="Five" x1="0" y1="70" x2="200" y2="70" stroke="#000" stroke-dasharray="1 2 3 4 5"/></svg>)svg";
    const auto scene = parse(kDashes);
    const auto description = buildSvgArtwork(*scene, options(false));
    std::vector<const SvgArtworkNode*> strokes;
    for (const auto& node : description.nodes)
        if (node.type == kPathStrokeNodeType)
            strokes.push_back(&node);
    expect(strokes.size() == 4, "each dashed line is stroked by a Path Stroke");
    if (strokes.size() != 4)
        return;
    const auto number = [](const SvgArtworkNode& node, const std::string& role) -> std::optional<double> {
        const auto found = node.values.find(role);
        if (found == node.values.end())
            return std::nullopt;
        const auto* constant = std::get_if<ParameterValue>(&found->second);
        const auto* value = constant != nullptr ? std::get_if<double>(constant) : nullptr;
        return value != nullptr ? std::optional(*value) : std::nullopt;
    };
    const auto pattern = [&](const SvgArtworkNode& node) {
        std::vector<double> lengths;
        for (const char* role : {"dashLength", "dashGap", "dash2", "gap2", "dash3", "gap3"})
            lengths.push_back(number(node, role).value_or(0.0));
        return lengths;
    };
    expect(pattern(*strokes[0]) == std::vector<double>{12, 4, 0, 4, 0, 0} && number(*strokes[0], "dashOffset") == 3.0,
           "12 4 0 4 imports as dash 12, gap 4, a dot and gap 4, with its offset");
    expect(pattern(*strokes[1]) == std::vector<double>{0, 6, 0, 6, 0, 0},
           "0 6 imports as dots every 6 (the pair repeated, so a dash of 0 is a dot, not solid)");
    expect(pattern(*strokes[2]) == std::vector<double>{4, 2, 1, 4, 2, 1}, "an odd count is repeated, as SVG reads it");
    expect(pattern(*strokes[3]) == std::vector<double>{1, 2, 3, 4, 5, 1}, "a longer pattern keeps its first six");
    const auto names = [&](const std::string_view text) {
        return std::ranges::any_of(description.notes.listed,
                                   [&](const std::string& note) { return note.find(text) != std::string::npos; });
    };
    expect(names("Five") && names("10 lengths keeps its first six"), "only the pattern past six lengths is noted");
    expect(!names("DashDot") && !names("Dotted") && !names("Three"), "a pattern of up to six lengths is not noted");
}

void fitMovesTheGeometry() {
    const auto scene = parse(kArtwork);
    const auto fitted = buildSvgArtwork(*scene, options(true));
    const auto own = buildSvgArtwork(*scene, options(false));
    expect(fitted.steps.size() == own.steps.size() && fitted.nodes.size() == own.nodes.size(),
           "fit changes no structure");
    expect(!(fitted == own), "fit changes the geometry");
}

void notesKeepTheirCap() {
    SvgArtworkNotes notes;
    for (std::size_t index = 0; index < kMaximumListedSvgNotes + 3; ++index)
        notes.add("item " + std::to_string(index) + ": not imported (pattern)");
    expect(notes.listed.size() == kMaximumListedSvgNotes, "at most the listed cap is named");
    expect(notes.omitted.size() == 1 && notes.omitted.begin()->second == 3, "the rest are counted by reason");
    SvgArtworkNotes leading;
    leading.add("The source file's path cannot be recorded in the project");
    leading.append(notes);
    expect(leading.listed.size() == kMaximumListedSvgNotes && leading.omitted.begin()->second == 4,
           "a note added first pushes one more past the cap, as one sequence would");
    expect(svgArtworkName(std::string("a\x01" "b"), "x") == "ab" && svgArtworkName("", "fallback") == "fallback",
           "names drop control characters and fall back when empty");
}

} // namespace

int main() {
    descriptionIsDeterministicAndOrdered();
    notesAreNamed();
    dashPatternsImportExactly();
    fitMovesTheGeometry();
    notesKeepTheirCap();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "PASS: the SVG artwork description\n";
    return EXIT_SUCCESS;
}
