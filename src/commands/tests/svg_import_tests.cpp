#include "command_test_support.hpp"

#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/value_nodes.hpp>

#include <algorithm>
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
    transaction.emplace<AddSvgArtwork>(composition, nullptr, path, "art", fit);
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

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    bloom::commands::test::structureAndUndo(test);
    bloom::commands::test::hostileFilesLeaveTheDocumentAlone(test);
    bloom::commands::test::scriptsAndExternalImagesDoNothing(test);
    bloom::commands::test::manySiblingsAreHeldInRuns(test);
    return test.failures() == 0 ? 0 : 1;
}
