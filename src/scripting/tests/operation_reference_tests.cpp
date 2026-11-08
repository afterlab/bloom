// The reference is the agent surfaces' manual: an agent must never need Bloom's source. Every
// operation and tool is described; every argument says its type and, where a bare number would be
// ambiguous, its unit; every closed argument lists its values; every enumeration parameter of
// the node registry has its labelled values; and the help answer is the same text whichever
// surface asks.
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter_presentation.hpp>
#include <bloom/scripting/node_reference.hpp>
#include <bloom/scripting/operation_reference.hpp>

#include <algorithm>
#include <initializer_list>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace {

using namespace bloom::scripting;

int failures = 0;

void expect(const bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

[[nodiscard]] bool contains(const std::string& text, const std::string_view part) {
    return text.find(part) != std::string::npos;
}

void everyOperationIsDescribed() {
    const auto registry = OperationRegistry::builtIn();
    for (const auto& descriptor : registry.descriptors()) {
        const auto& schema = descriptor.schema;
        expect(!schema.description.empty(), schema.typeId + " has no description");
        for (const auto& argument : schema.arguments) {
            const auto where = schema.typeId + "." + argument.name;
            expect(!argument.summary.empty(), where + " has no summary");
            // A bare number must say what it counts.
            const bool needsUnit =
                argument.kind == ValueKind::Time || argument.kind == ValueKind::Id ||
                argument.kind == ValueKind::Color4 || argument.kind == ValueKind::Path ||
                argument.kind == ValueKind::Value ||
                (argument.kind == ValueKind::Vec2 && argument.name != "offset" &&
                 !schema.typeId.starts_with("bloom.node."));
            if (needsUnit)
                expect(!argument.unit.empty(), where + " documents no unit");
            if (argument.kind == ValueKind::Time)
                expect(contains(argument.unit, "frames") && contains(argument.unit, "seconds"),
                       where + " does not say integer times are frames and pairs are seconds");
            // A closed vocabulary named only in prose is not discoverable.
            const auto& summary = argument.summary;
            const bool listsValues = (contains(summary, "\"") && contains(summary, " or ")) ||
                                     contains(summary, "0 None") || contains(summary, "0 Alpha");
            // A structured object (a path, or an output given as JSON) describes its fields, not a
            // closed vocabulary.
            const bool structured = argument.kind == ValueKind::Array ||
                                    argument.kind == ValueKind::Path || contains(summary, "JSON");
            if (listsValues && !structured)
                expect(!argument.choices.empty(), where + " names values but lists no choices");
        }
    }
    // The (d) description conflicts stay fixed.
    const auto* settings = registry.find("bloom.composition.set-settings");
    expect(settings != nullptr, "set-settings is registered");
    if (settings != nullptr) {
        for (const auto& argument : settings->schema.arguments)
            if (argument.name == "duration" || argument.name == "start")
                expect(!contains(argument.summary, "in seconds"),
                       "set-settings." + argument.name + " still claims seconds");
    }
    for (const auto* id :
         {"bloom.layer.set-range", "bloom.layer.set-timing", "bloom.composition.set-work-area"}) {
        const auto* descriptor = registry.find(id);
        expect(descriptor != nullptr, std::string(id) + " is registered");
        if (descriptor == nullptr)
            continue;
        for (const auto& argument : descriptor->schema.arguments)
            if (argument.name == "end")
                expect(contains(argument.summary, "exclusive"),
                       std::string(id) + ".end does not say it is exclusive");
    }
    if (const auto* connect = registry.find("bloom.node.connect")) {
        for (const auto& argument : connect->schema.arguments)
            if (argument.name == "slot")
                expect(!contains(argument.summary, "0,"), "node.connect.slot still advertises 0");
    }
}

void everyEntryIsComplete() {
    const auto& reference = OperationReference::builtIn();
    std::set<std::string> names;
    std::set<ReferenceKind> kinds;
    for (const auto& entry : reference.entries()) {
        expect(names.insert(entry.name).second, entry.name + " is listed twice");
        kinds.insert(entry.kind);
        expect(!entry.summary.empty(), entry.name + " has no summary");
        expect(!entry.area.empty(), entry.name + " has no area");
        for (const auto& argument : entry.arguments) {
            expect(!argument.type.empty(), entry.name + "." + argument.name + " has no type");
            expect(!argument.summary.empty(), entry.name + "." + argument.name + " no summary");
        }
        if (entry.kind == ReferenceKind::Tool || entry.kind == ReferenceKind::Operation ||
            entry.kind == ReferenceKind::QueryKind) {
            expect(!entry.result.empty(), entry.name + " has no result shape");
            expect(!entry.examples.empty(), entry.name + " has no example");
        }
        if (entry.kind == ReferenceKind::Operation) {
            expect(entry.examples.size() == 3, entry.name + " lacks an MCP, Python or CLI example");
            expect(!entry.errors.empty(), entry.name + " lists no errors");
            for (const auto& error : entry.errors)
                expect(!error.remedy.empty(), entry.name + " error without a remedy");
        }
        const auto json = referenceJson(entry);
        expect(json.front() == '{' && json.back() == '}', entry.name + " JSON is not an object");
        for (const auto& related : entry.related)
            expect(reference.find(related) != nullptr,
                   entry.name + " relates to unknown entry " + related);
    }
    for (const auto kind :
         {ReferenceKind::Concept, ReferenceKind::Tool, ReferenceKind::QueryKind,
          ReferenceKind::Operation, ReferenceKind::CliCommand, ReferenceKind::Python})
        expect(kinds.contains(kind), "a reference kind has no entries");
    for (const auto* name :
         {"help", "query", "transact", "history", "events", "render", "export", "capture",
          "export.start", "export.wait", "query:layers", "query:keyframes", "query:node-types",
          "bloom-cli render", "bloom-cli help", "concept:time", "bloom.help"})
        expect(reference.find(name) != nullptr, std::string(name) + " is not in the reference");
}

void lookupAcceptsEverySpelling() {
    const auto& reference = OperationReference::builtIn();
    for (const auto* spelling : {"bloom.layer.add-solid", "bloom.ops.layer.add_solid",
                                 "layer.add_solid", "layer.add-solid", "BLOOM.LAYER.ADD-SOLID"})
        expect(reference.find(spelling) != nullptr &&
                   reference.find(spelling)->name == "bloom.layer.add-solid",
               std::string("lookup of ") + spelling);
    expect(reference.find("bloom.ops.asset.import_") != nullptr &&
               reference.find("bloom.ops.asset.import_")->name == "bloom.asset.import",
           "Python keyword spelling resolves");
    for (const auto* spelling : {"query:layers", "query layers", "query kind=layers", "layers"})
        expect(reference.find(spelling) != nullptr &&
                   reference.find(spelling)->name == "query:layers",
               std::string("lookup of ") + spelling);
    expect(reference.find("cli render") != nullptr &&
               reference.find("cli render")->name == "bloom-cli render",
           "cli spelling resolves");
    expect(reference.find("render")->name == "render", "a bare tool name is the tool");
    const auto suggestions = reference.suggest("add-solid");
    expect(!suggestions.empty() && suggestions.front() == "bloom.layer.add-solid",
           "suggestions find an operation by its verb");
    const auto missing = referenceHelp("bloom.layer.add-solidd");
    expect(!missing.found && contains(missing.text, "bloom.layer.add-solid"),
           "an unknown name suggests the closest entry");
}

void helpTextCarriesTheReference() {
    const auto answer = referenceHelp("bloom.layer.add-solid");
    expect(answer.found, "add-solid is found");
    for (const auto* part : {"Arguments:", "color  color4  required", "Unit:", "Effects:",
                             "undoable", "Result:", "Errors:", "Examples:", "MCP:", "Python:",
                             "CLI:", "bloom.ops.layer.add_solid(", "Related:", "layer"})
        expect(contains(answer.text, part), std::string("add-solid help lacks ") + part);
    expect(contains(answer.json, "\"entry\":{") && contains(answer.json, "\"text\":"),
           "the JSON answer carries the entry and the text");
    const auto index = referenceHelp("");
    for (const auto* part : {"MCP tools", "Query kinds", "Operations", "bloom-cli commands",
                             "bloom.layer.add-solid", "query:keyframes", "concept:time"})
        expect(contains(index.text, part), std::string("the index lacks ") + part);
    const auto interpolation = referenceHelp("bloom.animation.set-keyframe-interpolation");
    expect(contains(interpolation.text, "\"ease-in-out\"  Ease In-Out"),
           "interpolation lists its values with labels");
    const auto modes = referenceHelp("bloom.layer.set-time-modes");
    expect(contains(modes.text, "2  Loop"), "time modes list their values");
}

void everyChoiceParameterHasValues() {
    for (const auto& definition : bloom::document::builtInNodeDefinitions().definitions()) {
        for (const auto& parameter : definition.parameters) {
            const auto presentation = bloom::document::parameterPresentation(parameter);
            if (presentation.kind != bloom::document::ParameterPresentationKind::Choice)
                continue;
            // References (a composition, a data block, a part or layer name) are chosen from
            // the project, not from a closed list.
            const auto& key = parameter.schemaKey;
            if (key == "bloom.composition-source.composition" || key == "bloom.data.block" ||
                key == "bloom.ocio-file.lut" || contains(key, ".part") || contains(key, ".layer") ||
                contains(key, "matte-id"))
                continue;
            expect(!parameterChoices(key).empty(), definition.key.typeId + "." + parameter.role +
                                                       " (" + key + ") is a Choice without values");
        }
    }
    const auto blend = parameterChoices("bloom.layer.blend-mode");
    expect(blend.size() == 18 && blend[3].label == "Screen" && blend[3].value == 3,
           "blend modes are labelled");
    const auto markdown = nodeReferenceMarkdown();
    for (const auto* part : {"# Bloom node and parameter reference", "## Blend modes", "3 Screen",
                             "bloom.solid-source", "| Role |"})
        expect(contains(markdown, part), std::string("node reference lacks ") + part);
}

void timesReadBackInBothUnits() {
    const auto rate = *bloom::document::FrameRate::create(24, 1);
    expect(timeJson(*bloom::core::RationalTime::create(2, 1), rate) ==
               R"({"frames":48,"seconds":[2,1]})",
           "two seconds are 48 frames at 24 fps");
    const auto ntsc = *bloom::document::FrameRate::create(24000, 1001);
    expect(timeJson(*bloom::core::RationalTime::create(1001, 24000), ntsc) ==
               R"({"frames":1,"seconds":[1001,24000]})",
           "one NTSC frame is exact");
}

// #554: the `category` that MCP `nodes` and `node-types` return is the node's group, the same tree
// the Add menu shows. The grouping's own guard is node_group_tests; this pins the wire spelling.
void everyNodeGroupIsNamed() {
    for (const auto& definition : bloom::document::builtInNodeDefinitions().definitions())
        expect(!nodeCategoryName(definition).empty(), definition.key.typeId + " has no category");
    const auto typeJson = [](const std::string_view typeId) {
        const auto& registry = bloom::document::builtInNodeDefinitions();
        for (const auto& definition : registry.definitions())
            if (definition.key.typeId == typeId)
                return nodeTypeJson(definition, false);
        return std::string{};
    };
    const auto pinned = [&](const std::initializer_list<std::string_view> types,
                            const std::string_view category) {
        for (const auto type : types)
            expect(contains(typeJson(type), "\"category\":\"" + std::string(category) + "\""),
                   std::string(type) + " reports category " + std::string(category));
    };
    pinned({"bloom.grade", "bloom.levels", "bloom.curves", "bloom.hue-saturation", "bloom.exposure",
            "bloom.offset", "bloom.multiply", "bloom.invert", "bloom.colorize"},
           "color-correction");
    pinned({"bloom.ocio-colour-space-transform", "bloom.ocio-file-transform"}, "color-management");
    pinned({"bloom.gaussian-blur", "bloom.box-blur", "bloom.z-defocus", "bloom.motion-blur",
            "bloom.sharpen"},
           "blur-sharpen");
    pinned({"bloom.matte-blur", "bloom.matte-erode", "bloom.matte-dilate", "bloom.luma-key"},
           "keying-mattes");
    pinned({"bloom.vignette", "bloom.film-grain", "bloom.codec-degrade"}, "stylize");
    pinned({"bloom.lens-distortion"}, "distort");
    pinned({"bloom.separate-channels", "bloom.combine-channels"}, "channels");
    pinned({"bloom.drop-shadow", "bloom.glow", "bloom.light-wrap", "bloom.lens-flare"},
           "light-shadow");
    pinned({"bloom.echo"}, "time");
    pinned({"bloom.alpha-association", "bloom.layer-stack", "bloom.layer-output"}, "compositing");
    pinned({"bloom.frame-number"}, "time-values");
    // The node reference resource is headed by the same names the Add menu shows.
    const auto reference = nodeReferenceMarkdown();
    expect(contains(reference, "\n## Blur & Sharpen\n") &&
               contains(reference, "\n## Color Correction\n") &&
               !contains(reference, "\n## Filters\n") && !contains(reference, "\n## Layers\n"),
           "the node reference is headed by the group names");
}

} // namespace

int main() {
    everyOperationIsDescribed();
    everyEntryIsComplete();
    lookupAcceptsEverySpelling();
    helpTextCarriesTheReference();
    everyChoiceParameterHasValues();
    timesReadBackInBothUnits();
    everyNodeGroupIsNamed();
    if (failures != 0) {
        std::cerr << failures << " reference check(s) failed\n";
        return 1;
    }
    std::cout << "operation reference: ok\n";
    return 0;
}
