// MCP parity operations (the UI edits that had no registry op): each one runs through the registry
// exactly as transact and Python run it, makes one undo step, and undo/redo restores and replays
// it. Every parity op also carries a full description.

#include <bloom/document/graph.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/scripting/json_script.hpp>
#include <bloom/scripting/session.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <source_location>
#include <string>
#include <vector>

namespace {

using namespace bloom;
using scripting::Arguments;
using scripting::StableId;
using scripting::Value;
using scripting::ValueArray;

int failures = 0;

void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
}

[[nodiscard]] ValueArray ids(const std::vector<std::uint64_t>& raw) {
    ValueArray values;
    for (const auto id : raw)
        values.emplace_back(StableId{id});
    return values;
}

struct Fixture final {
    std::unique_ptr<scripting::Session> session;
    document::CompositionId main;

    Fixture() {
        auto created =
            scripting::Session::createNew("Parity", "Main", core::RationalTime::fromInteger(4));
        if (!created)
            std::abort();
        session = std::move(created).takeSession();
        main = session->snapshot().project().compositions().front().id();
    }

    scripting::SessionCommandResult run(const std::string& op, Arguments arguments) {
        return session->executeJsonOperation(op, arguments, op);
    }

    // The ids of every output named `name`, in order.
    template <typename Id>
    static std::vector<Id> outputs(const scripting::SessionCommandResult& result,
                                   const std::string& name) {
        std::vector<Id> found;
        if (!result.command)
            return found;
        for (const auto& output : result.command->outputs)
            if (output.output.name == name)
                if (const auto* id = std::get_if<Id>(&output.output.id))
                    found.push_back(*id);
        return found;
    }

    [[nodiscard]] const document::Composition& composition(const document::CompositionId id) const {
        const auto* found = snapshot ? snapshot->project().findComposition(id) : nullptr;
        if (found == nullptr)
            std::abort();
        return *found;
    }
    void refresh() { snapshot.emplace(session->snapshot()); }

    [[nodiscard]] std::vector<document::LayerId> stack(const document::CompositionId id) {
        refresh();
        std::vector<document::LayerId> layers;
        for (const auto& entry : composition(id).graph().layerStack().entries())
            layers.push_back(entry.layerId);
        return layers;
    }

    std::optional<document::Snapshot> snapshot;
};

void layerOperations() {
    Fixture fixture;
    const auto solid = [&](const std::string& name) {
        auto result = fixture.run("bloom.layer.add-solid",
                                  {{"composition", Value(StableId{fixture.main.value()})},
                                   {"name", Value(name)},
                                   {"color", Value(core::Color4d{1, 0, 0, 1})}});
        expect(result.succeeded(), "a solid is added");
        const auto layers = Fixture::outputs<document::LayerId>(result, "layer");
        return layers.empty() ? document::LayerId{} : layers.front();
    };
    const auto red = solid("Red");
    const auto blue = solid("Blue");
    expect(fixture.stack(fixture.main).size() == 2, "two layers to start");

    // Duplicate: one copy per layer, the copy directly below its original, one undo step.
    auto duplicated = fixture.run("bloom.layer.duplicate",
                                  {{"composition", Value(StableId{fixture.main.value()})},
                                   {"layers", Value(ids({red.value()}))}});
    expect(duplicated.succeeded(), "duplicate succeeds");
    const auto copies = Fixture::outputs<document::LayerId>(duplicated, "layer");
    expect(copies.size() == 1 && copies.front() != red, "duplicate outputs the copy's layer id");
    auto stack = fixture.stack(fixture.main);
    expect(stack.size() == 3, "duplicate adds one layer");
    const auto original = std::ranges::find(stack, red);
    expect(original != stack.end() && original + 1 != stack.end() && *(original + 1) == copies[0],
           "the copy sits directly below its original");
    expect(fixture.session->undo().succeeded(), "undo duplicate");
    expect(fixture.stack(fixture.main).size() == 2, "undo removes the copy");
    expect(fixture.session->redo().succeeded(), "redo duplicate");
    expect(fixture.stack(fixture.main).size() == 3, "redo restores the copy");

    // #434 O5: a layer named N times is copied N times, one "layer" output per mention.
    auto repeated = fixture.run("bloom.layer.duplicate",
                                {{"composition", Value(StableId{fixture.main.value()})},
                                 {"layers", Value(ids({blue.value(), blue.value(), blue.value()}))}});
    const auto repeatedCopies = Fixture::outputs<document::LayerId>(repeated, "layer");
    stack = fixture.stack(fixture.main);
    expect(repeated.succeeded() && repeatedCopies.size() == 3 && stack.size() == 6 &&
               std::ranges::all_of(repeatedCopies,
                                   [&](const auto copy) {
                                       return copy != blue && std::ranges::count(stack, copy) == 1;
                                   }) &&
               repeatedCopies[0] != repeatedCopies[1] && repeatedCopies[1] != repeatedCopies[2] &&
               repeatedCopies[0] != repeatedCopies[2],
           "a layer named three times is copied three times");
    expect(fixture.session->undo().succeeded() && fixture.stack(fixture.main).size() == 3,
           "one undo removes every repeated copy");

    // Delete: both named layers go in one step; undo brings them back with their ids.
    auto deleted =
        fixture.run("bloom.layer.delete", {{"composition", Value(StableId{fixture.main.value()})},
                                           {"layers", Value(ids({red.value(), blue.value()}))}});
    expect(deleted.succeeded(), "delete succeeds");
    stack = fixture.stack(fixture.main);
    expect(stack.size() == 1 && stack.front() == copies[0],
           "delete removes exactly the named layers");
    expect(fixture.session->undo().succeeded(), "undo delete");
    stack = fixture.stack(fixture.main);
    expect(stack.size() == 3 && std::ranges::count(stack, red) == 1 &&
               std::ranges::count(stack, blue) == 1,
           "undo restores the deleted layers with their ids");
    expect(fixture.session->redo().succeeded(), "redo delete");
    expect(fixture.stack(fixture.main).size() == 1, "redo deletes them again");

    // A stale id is rejected and changes nothing.
    const auto before = fixture.session->snapshot().revision();
    auto stale =
        fixture.run("bloom.layer.delete", {{"composition", Value(StableId{fixture.main.value()})},
                                           {"layers", Value(ids({red.value()}))}});
    expect(!stale.succeeded(), "deleting a deleted layer is rejected");
    expect(fixture.session->snapshot().revision() == before, "a rejection leaves the revision");
}

void addFromAssets() {
    Fixture fixture;
    auto added = fixture.run("bloom.composition.add",
                             {{"name", Value("Nested")}, {"duration", Value(std::int64_t{2})}});
    expect(added.succeeded(), "a second composition is added");
    const auto nestedIds = Fixture::outputs<document::CompositionId>(added, "composition");
    expect(!nestedIds.empty(), "composition.add outputs the composition");
    if (nestedIds.empty())
        return;
    const auto nested = nestedIds.front();
    auto layers =
        fixture.run("bloom.layer.add-from-assets",
                    {{"composition", Value(StableId{fixture.main.value()})},
                     {"time", Value(ValueArray{Value(std::int64_t{1}), Value(std::int64_t{1})})},
                     {"compositions", Value(ids({nested.value(), nested.value()}))},
                     {"sequence", Value(true)}});
    expect(layers.succeeded(), "two nested-composition layers are added in one step");
    const auto made = Fixture::outputs<document::LayerId>(layers, "layer");
    expect(made.size() == 2, "one layer output per item");
    fixture.refresh();
    if (made.size() == 2) {
        const auto& graph = fixture.composition(fixture.main).graph();
        const auto* first = graph.findLayer(made[0]);
        const auto* second = graph.findLayer(made[1]);
        expect(first && first->name == "Nested", "a nested layer is named after its composition");
        expect(first && second && second->inPoint > first->inPoint,
               "sequenced layers start one after another");
        expect(first && first->inPoint == core::RationalTime::fromInteger(1),
               "the first layer starts at the requested time");
        const auto stack = fixture.stack(fixture.main);
        expect(stack.size() >= 2 && stack[0] == made[0] && stack[1] == made[1],
               "the first item is uppermost");
    }
    expect(fixture.session->undo().succeeded(), "undo the add");
    expect(fixture.stack(fixture.main).empty(), "undo removes every added layer at once");
    expect(fixture.session->redo().succeeded(), "redo the add");
    expect(fixture.stack(fixture.main).size() == 2, "redo adds them again");

    // The target composition is not a layer of itself.
    auto self = fixture.run("bloom.layer.add-from-assets",
                            {{"composition", Value(StableId{fixture.main.value()})},
                             {"time", Value(std::int64_t{0})},
                             {"compositions", Value(ids({fixture.main.value()}))}});
    expect(!self.succeeded(), "a composition cannot be its own layer");
    auto nothing = fixture.run(
        "bloom.layer.add-from-assets",
        {{"composition", Value(StableId{fixture.main.value()})}, {"time", Value(std::int64_t{0})}});
    expect(!nothing.succeeded() && nothing.diagnostic.has_value() &&
               nothing.diagnostic->argument == "assets",
           "an empty add names the missing items");
    auto before = fixture.run("bloom.layer.add-from-assets",
                              {{"composition", Value(StableId{fixture.main.value()})},
                               {"time", Value(std::int64_t{0})},
                               {"compositions", Value(ids({nested.value()}))},
                               {"placement", Value("before")}});
    expect(!before.succeeded() && before.diagnostic && before.diagnostic->argument == "before",
           "placement before needs a layer");
    if (!made.empty()) {
        auto below = fixture.run("bloom.layer.add-from-assets",
                                 {{"composition", Value(StableId{fixture.main.value()})},
                                  {"time", Value(std::int64_t{0})},
                                  {"compositions", Value(ids({nested.value()}))},
                                  {"before", Value(StableId{made[1].value()})}});
        expect(below.succeeded(), "a layer goes above a named layer");
        const auto inserted = Fixture::outputs<document::LayerId>(below, "layer");
        const auto stack = fixture.stack(fixture.main);
        expect(inserted.size() == 1 && stack.size() == 3 && stack[1] == inserted[0] &&
                   stack[2] == made[1],
               "before inserts directly above that layer");
    }
}

void nodeOperations() {
    Fixture fixture;
    auto added = fixture.run("bloom.composition.add", {{"name", Value("Source")}});
    const auto source = Fixture::outputs<document::CompositionId>(added, "composition");
    expect(!source.empty(), "a source composition exists");
    if (source.empty())
        return;
    auto node = fixture.run("bloom.node.add-composition-source",
                            {{"composition", Value(StableId{fixture.main.value()})},
                             {"source", Value(StableId{source.front().value()})},
                             {"position", Value(document::Vec2d{10, 20})}});
    expect(node.succeeded(), "a composition source node is added");
    const auto nodes = Fixture::outputs<document::NodeId>(node, "node");
    expect(nodes.size() == 1, "the source node is output");
    fixture.refresh();
    expect(fixture.composition(fixture.main).graph().layerOutputs().empty(),
           "a canvas source node makes no layer");
    expect(fixture.session->undo().succeeded() && fixture.session->redo().succeeded(),
           "the source add undoes and redoes");
    if (nodes.empty())
        return;

    // A Blur node fed by a Solid node's image output, then a reroute inserted into that link.
    auto value =
        fixture.run("bloom.node.add", {{"composition", Value(StableId{fixture.main.value()})},
                                       {"nodeType", Value("bloom.solid-source")}});
    const auto values = Fixture::outputs<document::NodeId>(value, "node");
    expect(values.size() == 1, "a source node is added");
    if (values.empty())
        return;
    auto fed = fixture.run("bloom.node.add-fed-by",
                           {{"composition", Value(StableId{fixture.main.value()})},
                            {"nodeType", Value("bloom.gaussian-blur")},
                            {"node", Value(StableId{values.front().value()})},
                            {"port", Value("image")}});
    expect(fed.succeeded(), "a node fed by an output is added and wired");
    const auto fedNodes = Fixture::outputs<document::NodeId>(fed, "editorNode");
    expect(fedNodes.size() == 1, "the fed node is output");
    if (!fedNodes.empty()) {
        fixture.refresh();
        std::optional<std::string> port;
        std::size_t before = 0;
        {
            const auto& graph = fixture.composition(fixture.main).graph();
            for (const auto& edge : graph.edges())
                if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
                    input && edge.source.nodeId == values.front() && input->nodeId == fedNodes[0])
                    port = input->port;
            before = graph.nodes().size();
        }
        expect(port.has_value(), "the new node reads the named output");
        if (port) {
            auto reroute = fixture.run("bloom.node.insert-reroute",
                                       {{"composition", Value(StableId{fixture.main.value()})},
                                        {"node", Value(StableId{fedNodes[0].value()})},
                                        {"port", Value(*port)}});
            expect(reroute.succeeded(), "a reroute is inserted into the link");
            fixture.refresh();
            expect(fixture.composition(fixture.main).graph().nodes().size() == before + 1,
                   "the reroute is one new node");
            expect(fixture.session->undo().succeeded(), "undo the reroute");
            fixture.refresh();
            expect(fixture.composition(fixture.main).graph().nodes().size() == before,
                   "undo removes the reroute in one step");
        }
    }
    auto arranged =
        fixture.run("bloom.node.arrange", {{"composition", Value(StableId{fixture.main.value()})}});
    expect(arranged.succeeded(), "arrange all lays the graph out");
    auto stray =
        fixture.run("bloom.node.arrange", {{"composition", Value(StableId{fixture.main.value()})},
                                           {"nodes", Value(ids({999999}))}});
    expect(!stray.succeeded(), "arranging an unknown node is refused");
    auto missing = fixture.run(
        "bloom.node.add-asset-source",
        {{"composition", Value(StableId{fixture.main.value()})}, {"asset", Value(StableId{999})}});
    expect(!missing.succeeded(), "a missing asset makes no source node");
}

void parameterOperations() {
    Fixture fixture;
    auto solid = fixture.run("bloom.layer.add-solid",
                             {{"composition", Value(StableId{fixture.main.value()})},
                              {"name", Value("Red")},
                              {"color", Value(core::Color4d{1, 0, 0, 1})}});
    expect(solid.succeeded(), "a solid is added");
    fixture.refresh();
    // The first node parameter whose definition default is a number.
    document::ParameterId parameter;
    double fallback = 0.0;
    for (const auto& node : fixture.composition(fixture.main).graph().nodes()) {
        const auto* definition =
            document::builtInNodeDefinitions().find(node.typeId, node.schemaVersion);
        if (definition == nullptr)
            continue;
        for (const auto& binding : node.parameters) {
            const auto declared = std::ranges::find(definition->parameters, binding.role,
                                                    &document::ParameterDefinition::role);
            if (declared != definition->parameters.end() &&
                std::holds_alternative<double>(declared->defaultValue) && !parameter.isValid()) {
                parameter = binding.parameterId;
                fallback = std::get<double>(declared->defaultValue);
            }
        }
    }
    expect(parameter.isValid(), "the solid has a numeric node parameter");
    if (!parameter.isValid())
        return;
    const auto valueOf = [&] {
        fixture.refresh();
        const auto* record = fixture.composition(fixture.main).parameters().find(parameter);
        const auto* constant =
            record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
        const auto* number = constant ? std::get_if<double>(&constant->value) : nullptr;
        return number ? *number : -12345.0;
    };
    auto changed = fixture.run("bloom.parameter.set-source",
                               {{"composition", Value(StableId{fixture.main.value()})},
                                {"parameter", Value(StableId{parameter.value()})},
                                {"value", Value(fallback + 0.25)}});
    expect(changed.succeeded() && valueOf() == fallback + 0.25, "the parameter is changed");
    auto reset = fixture.run("bloom.parameter.reset",
                             {{"composition", Value(StableId{fixture.main.value()})},
                              {"time", Value(std::int64_t{0})},
                              {"parameter", Value(StableId{parameter.value()})}});
    expect(reset.succeeded() && valueOf() == fallback, "reset restores the definition default");
    expect(fixture.session->undo().succeeded() && valueOf() == fallback + 0.25,
           "undo restores the edited value");
    expect(fixture.session->redo().succeeded() && valueOf() == fallback, "redo resets again");
    auto stale = fixture.run("bloom.parameter.reset",
                             {{"composition", Value(StableId{fixture.main.value()})},
                              {"time", Value(std::int64_t{0})},
                              {"parameter", Value(StableId{999999})}});
    expect(!stale.succeeded(), "an unknown parameter is refused");
}

// #434 O2/O3: one path object for shape outlines, masks and Path parameters. add-shape takes the
// same {closed, anchors: [{point, inHandle, outHandle, mode}]} object add-mask takes, and
// set-source repoints a Path parameter at a new outline after the layer exists.
void pathValues() {
    Fixture fixture;
    const auto anchor = [](const double x, const double y,
                           const std::optional<document::Vec2d> in = std::nullopt,
                           const std::optional<document::Vec2d> out = std::nullopt,
                           const document::PathAnchorMode mode = document::PathAnchorMode::Corner) {
        return document::PathAnchor{.point = {x, y}, .inHandle = in, .outHandle = out, .mode = mode};
    };
    document::PathValue outline;
    outline.closed = true;
    outline.anchors = {anchor(0, 0), anchor(100, 0, document::Vec2d{80, -20},
                                             document::Vec2d{120, 20},
                                             document::PathAnchorMode::Mirrored),
                       anchor(50, 80)};
    auto shape = fixture.run("bloom.layer.add-shape",
                             {{"composition", Value(StableId{fixture.main.value()})},
                              {"kind", Value("path")},
                              {"path", Value(outline)}});
    expect(shape.succeeded(), "add-shape takes the shared path object");
    fixture.refresh();
    document::ParameterId pathParameter;
    for (const auto& record : fixture.composition(fixture.main).parameters().records())
        if (const auto* constant = std::get_if<document::ConstantValueSource>(&record.source);
            constant != nullptr && std::holds_alternative<document::PathValue>(constant->value))
            pathParameter = record.id;
    expect(pathParameter.isValid(), "the path shape stores its outline in a Path parameter");
    if (!pathParameter.isValid())
        return;
    const auto stored = [&] {
        fixture.refresh();
        const auto* record = fixture.composition(fixture.main).parameters().find(pathParameter);
        const auto* constant =
            record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
        const auto* path = constant ? std::get_if<document::PathValue>(&constant->value) : nullptr;
        return path ? std::optional(*path) : std::nullopt;
    };
    expect(stored() == outline, "the outline, handles and anchor mode are kept as authored");

    document::PathValue replacement;
    replacement.closed = false;
    replacement.anchors = {anchor(10, 10), anchor(200, 40, std::nullopt, std::nullopt,
                                                  document::PathAnchorMode::Corner)};
    auto repointed = fixture.run("bloom.parameter.set-source",
                                 {{"composition", Value(StableId{fixture.main.value()})},
                                  {"parameter", Value(StableId{pathParameter.value()})},
                                  {"value", Value(replacement)}});
    expect(repointed.succeeded() && stored() == replacement,
           "set-source repoints a Path parameter at a new outline");
    expect(fixture.session->undo().succeeded() && stored() == outline,
           "undo restores the previous outline");
    auto wrongKind = fixture.run("bloom.parameter.set-source",
                                 {{"composition", Value(StableId{fixture.main.value()})},
                                  {"parameter", Value(StableId{pathParameter.value()})},
                                  {"value", Value(1.0)}});
    expect(!wrongKind.succeeded() && stored() == outline,
           "a number on a Path parameter is refused");
}

// #434 O4: add-feeding on an input that is already fed inserts the new node into the link. An
// OCIO colour space transform added in front of a layer's image keeps the solid feeding it, so the
// composition still compiles, and one undo restores the original link.
void addFeedingInserts() {
    Fixture fixture;
    auto solid = fixture.run("bloom.layer.add-solid",
                             {{"composition", Value(StableId{fixture.main.value()})},
                              {"name", Value("Red")},
                              {"color", Value(core::Color4d{1, 0, 0, 1})}});
    const auto layers = Fixture::outputs<document::LayerId>(solid, "layer");
    expect(solid.succeeded() && layers.size() == 1, "a solid layer is added");
    if (layers.empty())
        return;
    fixture.refresh();
    const auto* boundary = fixture.composition(fixture.main).graph().findLayer(layers.front());
    expect(boundary != nullptr, "the layer has a Layer Output node");
    if (boundary == nullptr)
        return;
    const auto layerNode = boundary->nodeId;
    const document::InputPortRef image =
        document::NodeInputRef{layerNode, std::string(document::kLayerOutputContentInputPort)};
    const auto feeding = [&](const document::InputPortRef& input)
        -> std::optional<document::OutputPortRef> {
        fixture.refresh();
        for (const auto& edge : fixture.composition(fixture.main).graph().edges())
            if (edge.destination == input)
                return edge.source;
        return std::nullopt;
    };
    const auto original = feeding(image);
    expect(original.has_value(), "the solid feeds the layer's image");

    auto added = fixture.run("bloom.node.add-feeding",
                             {{"composition", Value(StableId{fixture.main.value()})},
                              {"nodeType", Value("bloom.ocio-colour-space-transform")},
                              {"node", Value(StableId{layerNode.value()})},
                              {"port", Value(std::string(document::kLayerOutputContentInputPort))}});
    const auto inserted = Fixture::outputs<document::NodeId>(added, "editorNode");
    expect(added.succeeded() && inserted.size() == 1, "add-feeding adds the transform");
    if (inserted.empty() || !original)
        return;
    const auto fed = feeding(image);
    expect(fed.has_value() && fed->nodeId == inserted.front(),
           "the transform now feeds the layer's image");
    std::optional<document::OutputPortRef> upstream;
    fixture.refresh();
    for (const auto& edge : fixture.composition(fixture.main).graph().edges())
        if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            input != nullptr && input->nodeId == inserted.front())
            upstream = edge.source;
    expect(upstream == original, "what fed the layer now feeds the inserted transform");
    expect(fixture.session->undo().succeeded() && feeding(image) == original,
           "one undo restores the original link");
}

// #434 O6: a JSON script reads `null` as an omitted argument, as transact and Python do, so
// set-parent with "parent": null clears the parent instead of refusing the script.
void nullArguments() {
    const auto parsed = scripting::parseScriptJson(
        R"([{"op":"bloom.layer.set-parent","args":{"composition":1,"layer":2,"parent":null}}])");
    expect(static_cast<bool>(parsed) && parsed.transactions().size() == 1 &&
               !parsed.transactions().front().arguments.contains("parent") &&
               parsed.transactions().front().arguments.contains("layer"),
           "a null argument parses as an omitted one");
}

// #434 O8: an export output that names a built-in preset and gives no settings takes that preset's
// settings, so a ProRes job is a ProRes job and its .mov destination is accepted; a preset that
// cannot be resolved is refused with its name instead of silently becoming H.264 MP4.
void exportPresetOutputs() {
    Fixture fixture;
    const auto addJob = [&](const std::string& output) {
        return fixture.run("bloom.export.add-job",
                           {{"composition", Value(StableId{fixture.main.value()})},
                            {"outputs", Value(ValueArray{Value(output)})}});
    };
    auto added = addJob(R"({"preset":"builtin:prores-422hq-mov",)"
                        R"("destination":{"path":"Exports/{name}.mov"}})");
    const auto jobs = Fixture::outputs<document::ExportJobId>(added, "exportJob");
    expect(added.succeeded() && jobs.size() == 1, "a job with a preset-only output is added");
    if (jobs.empty())
        return;
    fixture.refresh();
    const auto* job = fixture.snapshot->project().findExportJob(jobs.front());
    expect(job != nullptr && job->outputs.size() == 1 &&
               job->outputs.front().presetId == "builtin:prores-422hq-mov" &&
               job->outputs.front().settings.format == document::ExportFormat::ProResMov &&
               job->outputs.front().destination.path == "Exports/{name}.mov",
           "the output takes the ProRes preset's settings and keeps its .mov destination");

    auto unknown = addJob(R"({"preset":"builtin:no-such-preset"})");
    expect(!unknown.succeeded() && unknown.diagnostic &&
               unknown.diagnostic->message.find("builtin:no-such-preset") != std::string::npos,
           "an unknown preset without settings is refused by name");
}

void projectItems() {
    Fixture fixture;
    auto folder = fixture.run("bloom.asset.create-folder", {{"name", Value("Shots")}});
    const auto folders = Fixture::outputs<document::AssetFolderId>(folder, "folder");
    auto nested = fixture.run("bloom.composition.add", {{"name", Value("Inside")}});
    const auto inside = Fixture::outputs<document::CompositionId>(nested, "composition");
    expect(folders.size() == 1 && inside.size() == 1, "a folder and a composition exist");
    if (folders.empty() || inside.empty())
        return;
    auto filed = fixture.run("bloom.asset.move-compositions",
                             {{"compositions", Value(ids({inside[0].value()}))},
                              {"folder", Value(StableId{folders[0].value()})}});
    expect(filed.succeeded(), "the composition is filed in the folder");
    const auto compositionCount = [&] {
        fixture.refresh();
        return fixture.snapshot->project().compositions().size();
    };
    auto alone =
        fixture.run("bloom.asset.delete-items", {{"folders", Value(ids({folders[0].value()}))}});
    expect(alone.succeeded() && compositionCount() == 2,
           "a folder deleted alone keeps what it held");
    expect(fixture.session->undo().succeeded(), "undo the folder delete");
    auto all =
        fixture.run("bloom.asset.delete-items",
                    {{"folders", Value(ids({folders[0].value()}))}, {"withContents", Value(true)}});
    expect(all.succeeded() && compositionCount() == 1,
           "withContents deletes the folder's composition too");
    expect(fixture.session->undo().succeeded() && compositionCount() == 2,
           "one undo restores the folder and its contents");
    auto empty = fixture.run("bloom.asset.delete-items", {});
    expect(!empty.succeeded(), "an empty delete is refused");
}

void colorSettings() {
    Fixture fixture;
    const auto revision = fixture.session->snapshot().revision();
    auto aces = fixture.session->setColorSettings("aces", std::string("ACEScg"));
    expect(aces.code == "updated" && aces.settings &&
               aces.settings->processColorSpaceId == "ACEScg",
           "the project switches to ACES with ACEScg");
    expect(fixture.session->setColorSettings("aces", std::nullopt).code == "no-change",
           "an omitted working space keeps the project's");
    expect(fixture.session->setColorSettings("neutral", std::string("ACEScg")).code ==
               "invalid-settings",
           "a working space the configuration does not offer is refused");
    expect(fixture.session->setColorSettings("srgb", std::nullopt).code == "invalid-config",
           "an unknown configuration is refused");
    auto neutral = fixture.session->setColorSettings("neutral", std::nullopt);
    expect(neutral.code == "updated" && neutral.settings &&
               neutral.settings->processColorSpaceId == "lin_rec709_scene",
           "neutral takes its only working space");
    expect(fixture.session->snapshot().revision() == revision,
           "color settings are not a document edit");
}

void descriptions() {
    const auto registry = scripting::OperationRegistry::builtIn();
    for (const auto* id :
         {"bloom.layer.delete", "bloom.layer.duplicate", "bloom.layer.add-from-assets",
          "bloom.node.add-asset-source", "bloom.node.add-composition-source",
          "bloom.node.add-feeding", "bloom.node.add-fed-by", "bloom.node.insert-reroute",
          "bloom.node.arrange", "bloom.asset.delete-items", "bloom.composition.add-from-assets",
          "bloom.parameter.reset", "bloom.node.set-parameter-value",
          "bloom.node.set-video-audio-stream"}) {
        const auto* descriptor = registry.find(id);
        expect(descriptor != nullptr, std::string(id) + " is registered");
        if (descriptor == nullptr)
            continue;
        const auto& text = descriptor->schema.description;
        for (const auto* part : {"Errors:", "Undo:", "MCP example:"})
            expect(text.find(part) != std::string::npos, std::string(id) + " describes " + part);
    }
}

} // namespace

int main() {
    layerOperations();
    addFromAssets();
    nodeOperations();
    parameterOperations();
    pathValues();
    addFeedingInserts();
    nullArguments();
    exportPresetOutputs();
    projectItems();
    colorSettings();
    descriptions();
    if (failures != 0) {
        std::cerr << failures << " parity operation check(s) failed\n";
        return 1;
    }
    std::cout << "parity operations ok\n";
    return 0;
}
