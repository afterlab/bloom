// The reference is the agent surfaces' manual: an agent must never need Bloom's source. Every
// operation and tool is described; every argument says its type and, where a bare number would be
// ambiguous, its unit; every closed argument lists its values; every enumeration parameter of
// the node registry has its labelled values; and the help answer is the same text whichever
// surface asks.
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/parameter_presentation.hpp>
#include <bloom/document/value_utility_nodes.hpp>
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
            // the project, not from a closed list. Mask Path and Text Outline name the node they
            // read (#509).
            const auto& key = parameter.schemaKey;
            if (key == "bloom.composition-source.composition" || key == "bloom.node-group.group" || key == "bloom.data.block" ||
                bloom::document::isPathSourceParameterSchemaKey(key) ||
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
            "bloom.offset", "bloom.multiply", "bloom.invert", "bloom.colorize",
            "bloom.auto-exposure", "bloom.tone-map", "bloom.color-matrix"},
           "color-correction");
    pinned({"bloom.ocio-colour-space-transform", "bloom.ocio-file-transform"}, "color-management");
    pinned({"bloom.gaussian-blur", "bloom.box-blur", "bloom.z-defocus", "bloom.motion-blur",
            "bloom.sharpen"},
           "blur-sharpen");
    pinned({"bloom.matte-blur", "bloom.matte-erode", "bloom.matte-dilate", "bloom.luma-key"},
           "keying-mattes");
    pinned({"bloom.vignette", "bloom.film-grain", "bloom.codec-degrade", "bloom.alpha-contour"},
           "stylize");
    pinned({"bloom.lens-distortion", "bloom.chromatic-aberration", "bloom.rolling-shutter",
            "bloom.corner-pin", "bloom.st-map", "bloom.displace"},
           "distort");
    pinned({"bloom.test-card"}, "sources");
    pinned({"bloom.separate-channels", "bloom.combine-channels"}, "channels");
    pinned({"bloom.drop-shadow", "bloom.glow", "bloom.light-wrap", "bloom.lens-flare"},
           "light-shadow");
    pinned({"bloom.echo", "bloom.frame-rate-hold"}, "time");
    pinned({"bloom.alpha-association", "bloom.layer-stack", "bloom.layer-output",
            "bloom.edge-extend"},
           "compositing");
    pinned({"bloom.frame-number"}, "time-values");
    // The node reference resource is headed by the same names the Add menu shows.
    const auto reference = nodeReferenceMarkdown();
    expect(contains(reference, "\n## Blur & Sharpen\n") &&
               contains(reference, "\n## Color Correction\n") &&
               !contains(reference, "\n## Filters\n") && !contains(reference, "\n## Layers\n"),
           "the node reference is headed by the group names");
}

// #390: what an operation reports and what it changes beyond its target are declared before the
// call, in the reference every surface prints.
void outputsAndSideEffectsAreDeclared() {
    const auto& reference = OperationReference::builtIn();
    const auto has = [](const ReferenceEntry& entry, const std::string_view name) {
        return std::ranges::any_of(entry.outputs,
                                   [name](const auto& output) { return output.name == name; });
    };
    const auto output = [](const ReferenceEntry& entry, const std::string_view name) {
        return *std::ranges::find(entry.outputs, name, &ReferenceOutput::name);
    };
    const auto* curve = reference.find("bloom.animation.create-for-parameter");
    expect(curve != nullptr && has(*curve, "animationCurve") && has(*curve, "keyframe"),
           "create-for-parameter declares its curve and key");
    if (curve != nullptr && has(*curve, "animationCurve"))
        expect(output(*curve, "animationCurve").kind == "animation-curve",
               "an output names the kind of id it carries");
    const auto* paste = reference.find("bloom.animation.paste-keyframes");
    expect(paste != nullptr && has(*paste, "keyframe") && output(*paste, "keyframe").repeated,
           "paste-keyframes declares one key per pasted key as repeated");
    const auto* duplicate = reference.find("bloom.layer.duplicate");
    expect(duplicate != nullptr && has(*duplicate, "layer") &&
               output(*duplicate, "layer").repeated,
           "layer.duplicate declares one layer per copy, from its own description");
    const auto* feeding = reference.find("bloom.node.add-feeding");
    expect(feeding != nullptr && has(*feeding, "node") && has(*feeding, "editorNode") &&
               has(*feeding, "parameter.<role>"),
           "add-feeding declares the node, the editor node and the parameter family");
    const auto* job = reference.find("bloom.export.add-job");
    expect(job != nullptr && has(*job, "exportJob") && has(*job, "exportOutput") &&
               output(*job, "exportOutput").repeated,
           "export.add-job declares the job and one output per output");
    const auto* rename = reference.find("bloom.project.set-name");
    expect(rename != nullptr && rename->outputs.empty() && rename->sideEffects.empty(),
           "an operation with no ids and no side effects declares none");
    for (const auto& entry : reference.entries())
        if (entry.kind == ReferenceKind::Operation)
            for (const auto& declared : entry.outputs)
                expect(!declared.name.empty() && !declared.kind.empty(),
                       entry.name + " declares an output without a name or kind");

    const auto effect = [&](const std::string_view id, const std::string_view part) {
        const auto* entry = reference.find(id);
        return entry != nullptr &&
               std::ranges::any_of(entry->sideEffects, [part](const std::string& line) {
                   return contains(line, part);
               });
    };
    expect(effect("bloom.node.add-feeding", "replaces the existing link"),
           "add-feeding declares that a value node replaces the link");
    expect(effect("bloom.node.duplicate", "collapse"), "node.duplicate declares that ids collapse");
    expect(effect("bloom.layer.duplicate", "copied N times"),
           "layer.duplicate declares that it copies repeated ids");
    expect(effect("bloom.parameter.set-source", "retires its animation curve"),
           "set-source declares that an animated parameter's curve is retired");
    expect(effect("bloom.node.connect", "replaces that link"),
           "node.connect declares that a fed input's link is replaced");
    expect(effect("bloom.node.remove", "deletes that layer"),
           "node.remove declares that a Layer Output takes its layer");
    expect(effect("bloom.asset.import", "external locators"),
           "asset.import declares what an unsaved project keeps");
    const auto text = referenceHelp("bloom.node.duplicate").text;
    expect(contains(text, "Side effects:") && contains(text, "Outputs (ids the result carries):"),
           "the help text prints both sections");

    // The data block's closed and open vocabularies are named.
    const auto* block = reference.find("bloom.data-block.create");
    expect(block != nullptr, "data-block.create is documented");
    if (block != nullptr) {
        for (const auto& argument : block->arguments) {
            if (argument.name == "kind")
                expect(argument.choices.size() == 4 && contains(argument.unit, "refused"),
                       "the data block kinds the operation can create are listed");
            if (argument.name == "typeId")
                expect(contains(argument.unit, "bloom.media.asset"),
                       "the block type id says what it is and what Bloom itself uses");
            if (argument.name == "mediaType")
                expect(contains(argument.unit, "application/vnd.bloom.asset"),
                       "the media type says what it is and what Bloom itself writes");
        }
    }

    // The new query kinds, the origin and the export script's format are in the reference.
    for (const auto* name : {"query:text-metrics", "query:layer-bounds", "query:history",
                             "query:viewer", "query:preview-cache", "query:jobs", "query:pixels",
                             "query:render-route"})
        expect(reference.find(name) != nullptr, std::string(name) + " is in the reference");
    // #390: an unsaved project's import is documented, and the new readings state their limits.
    const auto* import = reference.find("bloom.asset.import");
    bool unsavedNoted = false;
    bool refusalListed = false;
    if (import != nullptr) {
        for (const auto& note : import->sideEffects)
            unsavedNoted = unsavedNoted ||
                           (contains(note, "never been saved") && contains(note, "external locators"));
        for (const auto& error : import->errors)
            refusalListed = refusalListed || contains(error.when, "never been saved");
    }
    expect(unsavedNoted, "asset.import documents the unsaved project");
    expect(refusalListed, "asset.import lists the relative-path refusal in an unsaved project");
    const auto* pixels = reference.find("query:pixels");
    expect(pixels != nullptr && contains(pixels->description, "256") &&
               contains(pixels->description, "scene-linear") &&
               contains(pixels->description, "stops when the request is cancelled"),
           "query:pixels documents its limit, color space and cancellation");
    const auto* route = reference.find("query:render-route");
    expect(route != nullptr && contains(route->description, "never reported as GPU"),
           "query:render-route documents that a CPU frame is never labeled GPU");
    const auto* jobs = reference.find("query:jobs");
    expect(jobs != nullptr && contains(jobs->description, "failure"),
           "query:jobs documents the failure reason");
    const auto* events = reference.find("events");
    expect(events != nullptr && contains(events->description, "origin"),
           "events documents the edit origin");
    const auto* script = reference.find("export.script");
    expect(script != nullptr && contains(script->result, "bloom.export.add-job") &&
               contains(script->result, "export.start"),
           "export.script documents the format of its output");
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
    outputsAndSideEffectsAreDeclared();
    if (failures != 0) {
        std::cerr << failures << " reference check(s) failed\n";
        return 1;
    }
    std::cout << "operation reference: ok\n";
    return 0;
}
