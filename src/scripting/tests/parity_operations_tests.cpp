// MCP parity operations (the UI edits that had no registry op): each one runs through the registry
// exactly as transact and Python run it, makes one undo step, and undo/redo restores and replays
// it. Every parity op also carries a full description.

#include <bloom/document/graph.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/scripting/json_script.hpp>
#include <bloom/scripting/merge_entry_view.hpp>
#include <bloom/scripting/operation_registry.hpp>
#include <bloom/scripting/session.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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
    auto repeated =
        fixture.run("bloom.layer.duplicate",
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
        return document::PathAnchor{
            .point = {x, y}, .inHandle = in, .outHandle = out, .mode = mode};
    };
    document::PathValue outline;
    outline.editableContour().closed = true;
    outline.editableContour().anchors = {anchor(0, 0),
                                         anchor(100, 0, document::Vec2d{80, -20},
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
    replacement.editableContour().closed = false;
    replacement.editableContour().anchors = {
        anchor(10, 10),
        anchor(200, 40, std::nullopt, std::nullopt, document::PathAnchorMode::Corner)};
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
    const auto feeding =
        [&](const document::InputPortRef& input) -> std::optional<document::OutputPortRef> {
        fixture.refresh();
        for (const auto& edge : fixture.composition(fixture.main).graph().edges())
            if (edge.destination == input)
                return edge.source;
        return std::nullopt;
    };
    const auto original = feeding(image);
    expect(original.has_value(), "the solid feeds the layer's image");

    auto added =
        fixture.run("bloom.node.add-feeding",
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

// #434 O7: parenting is a transform-only dependency, so a layer's track matte may be its own child.
// Both ways of reaching that setup succeed.
void matteParentedToItsTarget() {
    Fixture fixture;
    const auto solid = [&](const std::string& name) {
        auto result = fixture.run("bloom.layer.add-solid",
                                  {{"composition", Value(StableId{fixture.main.value()})},
                                   {"name", Value(name)},
                                   {"color", Value(core::Color4d{1, 0, 0, 1})}});
        const auto layers = Fixture::outputs<document::LayerId>(result, "layer");
        return layers.empty() ? document::LayerId{} : layers.front();
    };
    const auto target = solid("Target");
    const auto matte = solid("Matte");
    const auto setMatte = [&] {
        return fixture.run("bloom.layer.set-track-matte",
                           {{"composition", Value(StableId{fixture.main.value()})},
                            {"layer", Value(StableId{target.value()})},
                            {"source", Value(StableId{matte.value()})},
                            {"channel", Value(std::int64_t{0})},
                            {"inverted", Value(false)}});
    };
    const auto setParent = [&] {
        return fixture.run("bloom.layer.set-parent",
                           {{"composition", Value(StableId{fixture.main.value()})},
                            {"layer", Value(StableId{matte.value()})},
                            {"parent", Value(StableId{target.value()})}});
    };
    expect(setParent().succeeded(), "the matte layer is parented to the target");
    expect(setMatte().succeeded(), "a matte from the target's child is accepted");
    expect(fixture.session->undo().succeeded() && fixture.session->undo().succeeded(),
           "both edits undo");
    expect(setMatte().succeeded() && setParent().succeeded(),
           "parenting a matte under the layer it mattes is accepted");
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
          "bloom.node.set-video-audio-stream", "bloom.merge.ensure-entry-layer",
          "bloom.merge.insert-layer-node", "bloom.merge.add-adjustment-layer",
          "bloom.merge.set-entry-parameter"}) {
        const auto* descriptor = registry.find(id);
        expect(descriptor != nullptr, std::string(id) + " is registered");
        if (descriptor == nullptr)
            continue;
        const auto& text = descriptor->schema.description;
        for (const auto* part : {"Errors:", "Undo:", "MCP example:"})
            expect(text.find(part) != std::string::npos, std::string(id) + " describes " + part);
    }
}


// #502 Every Merge entry is a layer: a plain entry (a source wired straight into the Merge) is
// edited by merge and slot, its own layer created in the same undo step; an Adjustment layer is
// added; a Layer node is inserted and adopts the entry's values; everything reads back.
void mergeEntryOperations() {
    Fixture fixture;
    fixture.refresh();
    const auto merge = fixture.composition(fixture.main).graph().outputMergeId();
    expect(merge.has_value(), "the composition has an output Merge");
    if (!merge)
        return;
    const auto composition = Value(StableId{fixture.main.value()});
    const auto mergeId = Value(StableId{merge->value()});
    const auto plainEntry = [&] {
        auto node = fixture.run("bloom.node.add", {{"composition", composition},
                                                   {"nodeType", Value("bloom.solid-source")}});
        const auto nodes = Fixture::outputs<document::NodeId>(node, "node");
        if (nodes.empty())
            return document::LayerSlotId{};
        auto connected = fixture.run("bloom.node.connect",
                                     {{"composition", composition},
                                      {"sourceNode", Value(StableId{nodes.front().value()})},
                                      {"sourcePort", Value("image")},
                                      {"targetNode", mergeId},
                                      {"role", Value("content")}});
        const auto slots = Fixture::outputs<document::LayerSlotId>(connected, "layerSlot");
        return slots.empty() ? document::LayerSlotId{} : slots.front();
    };
    const auto view = [&](const document::LayerSlotId slot) {
        fixture.refresh();
        return scripting::mergeEntryView(fixture.composition(fixture.main), *merge, slot);
    };
    const auto roleValue = [](const scripting::MergeEntryView& entry, const std::string_view role)
        -> std::optional<document::ParameterValue> {
        for (const auto& item : entry.roles)
            if (item.role == role)
                return item.value;
        return std::nullopt;
    };
    const auto first = plainEntry();
    const auto second = plainEntry();
    expect(first.isValid() && second.isValid(), "two plain entries are wired into the Merge");
    if (!first.isValid() || !second.isValid())
        return;
    const auto at = [&](const document::LayerSlotId slot) {
        return Arguments{{"composition", composition},
                         {"merge", mergeId},
                         {"slot", Value(StableId{slot.value()})}};
    };

    // A plain entry reads back with no layer and the identity defaults it renders with.
    auto plain = view(first);
    expect(plain && plain->kind == scripting::EntryLayerKind::None && plain->layer == nullptr &&
               roleValue(*plain, "opacity") == document::ParameterValue(1.0) &&
               roleValue(*plain, "blendMode") == document::ParameterValue(std::int64_t{0}) &&
               plain->outputMerge,
           "a plain entry reports no layer and its default values");

    // A layer edit by merge and slot creates the entry layer in the same undo step.
    const auto before = fixture.stack(fixture.main);
    auto args = at(first);
    args["solo"] = Value(true);
    auto solo = fixture.run("bloom.layer.set-solo", args);
    expect(solo.succeeded(), "set-solo addresses a plain entry by merge and slot");
    const auto soloLayers = Fixture::outputs<document::LayerId>(solo, "layer");
    expect(soloLayers.size() == 1 &&
               !Fixture::outputs<document::ParameterId>(solo, "parameter.opacity").empty(),
           "the created entry layer and its parameters are output");
    plain = view(first);
    expect(plain && plain->kind == scripting::EntryLayerKind::Entry && plain->layer &&
               plain->layer->solo && plain->layer->entryOwned() &&
               plain->pivot == document::LayerPivot::Frame && !soloLayers.empty() &&
               plain->layer->layerId == soloLayers.front(),
           "the entry layer holds the edit");
    expect(fixture.session->undo().succeeded(), "undo the entry edit");
    plain = view(first);
    expect(plain && plain->kind == scripting::EntryLayerKind::None,
           "one undo removes both the edit and the entry layer");
    expect(fixture.stack(fixture.main) == before, "undo restores the stack");
    expect(fixture.session->redo().succeeded(), "redo the entry edit");
    plain = view(first);
    expect(plain && plain->layer && plain->layer->solo, "redo replays both");

    // Once the layer exists, merge and slot keep addressing it; layer and merge together refuse.
    args = at(first);
    args["name"] = Value("Plate");
    expect(fixture.run("bloom.layer.rename", args).succeeded(), "rename an entry layer by entry");
    plain = view(first);
    expect(plain && plain->layer && plain->layer->name == "Plate", "the entry layer is renamed");
    args["layer"] = Value(StableId{soloLayers.empty() ? 1 : soloLayers.front().value() + 1000});
    auto both = fixture.run("bloom.layer.rename", args);
    expect(!both.succeeded(), "layer together with merge and slot must name that entry's layer");
    args["layer"] = Value(StableId{plain && plain->layer ? plain->layer->layerId.value() : 0});
    args["name"] = Value("Plate 2");
    expect(fixture.run("bloom.layer.rename", args).succeeded(),
           "layer together with merge and slot naming that entry's layer applies");
    auto halfAddress = fixture.run("bloom.layer.set-shy", {{"composition", composition},
                                                           {"merge", mergeId},
                                                           {"shy", Value(true)}});
    expect(!halfAddress.succeeded() && halfAddress.diagnostic &&
               halfAddress.diagnostic->argument == "slot",
           "merge without slot is refused naming slot");

    // A selected layer does not fill `layer` when the call names an entry.
    const auto registry = scripting::OperationRegistry::builtIn();
    if (const auto* descriptor = registry.find("bloom.layer.set-shy")) {
        auto contextual = at(second);
        scripting::OperationContext context;
        context.layers = {12345};
        const auto filled = scripting::applyContextDefaults(descriptor->schema, contextual, context);
        expect(std::ranges::find(filled, std::string("layer")) == filled.end() &&
                   !contextual.contains("layer"),
               "the selected layer is not used when merge names an entry");
    }

    // An entry's opacity by role: materializes its layer in one undo step.
    args = at(second);
    args["role"] = Value("opacity");
    args["value"] = Value(0.5);
    auto opacity = fixture.run("bloom.merge.set-entry-parameter", args);
    expect(opacity.succeeded(), "set-entry-parameter sets a plain entry's opacity");
    auto entry = view(second);
    expect(entry && entry->kind == scripting::EntryLayerKind::Entry &&
               roleValue(*entry, "opacity") == document::ParameterValue(0.5),
           "the entry layer holds opacity 0.5");
    expect(fixture.session->undo().succeeded(), "undo the opacity edit");
    entry = view(second);
    expect(entry && entry->kind == scripting::EntryLayerKind::None,
           "undo removes the opacity edit and its entry layer together");
    expect(fixture.session->redo().succeeded(), "redo the opacity edit");
    // Blend mode by label, and a keyed position.
    args = at(second);
    args["role"] = Value("blendMode");
    args["value"] = Value("screen");
    expect(fixture.run("bloom.merge.set-entry-parameter", args).succeeded(),
           "an entry's blend mode takes its label");
    args = at(second);
    args["role"] = Value("position");
    args["value"] = Value(document::Vec2d{10.0, 20.0});
    args["time"] = Value(std::int64_t{1});
    auto keyed = fixture.run("bloom.merge.set-entry-parameter", args);
    expect(keyed.succeeded(), "an entry's position is keyed at a time");
    entry = view(second);
    expect(entry && roleValue(*entry, "position") == std::nullopt &&
               roleValue(*entry, "blendMode") != document::ParameterValue(std::int64_t{0}),
           "the keyed position reads back as animated and the blend mode as Screen");
    args = at(second);
    args["role"] = Value("bogus");
    args["value"] = Value(1.0);
    auto bogus = fixture.run("bloom.merge.set-entry-parameter", args);
    expect(!bogus.succeeded() && bogus.diagnostic && bogus.diagnostic->argument == "role",
           "an unknown role is refused at creation");
    args["role"] = Value("timeRemap");
    expect(!fixture.run("bloom.merge.set-entry-parameter", args).succeeded(),
           "Time Remap is refused while it is off");

    // Insert a Layer node: it adopts the entry's values and becomes the entry's layer.
    auto inserted = fixture.run("bloom.merge.insert-layer-node", at(second));
    expect(inserted.succeeded() &&
               Fixture::outputs<document::NodeId>(inserted, "node").size() == 1,
           "insert-layer-node outputs the Layer node");
    entry = view(second);
    expect(entry && entry->kind == scripting::EntryLayerKind::Node && entry->layerNode &&
               roleValue(*entry, "opacity") == document::ParameterValue(0.5) &&
               roleValue(*entry, "position") == std::nullopt,
           "the Layer node adopted the entry's opacity and keyed position");
    fixture.refresh();
    expect(document::entryOwnedLayer(fixture.composition(fixture.main).graph(), *merge, second) ==
               nullptr,
           "the entry layer left with the adoption");

    // An Adjustment layer at the top of the Merge.
    auto adjustment = fixture.run("bloom.merge.add-adjustment-layer",
                                  {{"composition", composition}, {"merge", mergeId}});
    const auto adjustmentNodes = Fixture::outputs<document::NodeId>(adjustment, "node");
    const auto adjustmentSlots = Fixture::outputs<document::LayerSlotId>(adjustment, "layerSlot");
    expect(adjustment.succeeded() && adjustmentNodes.size() == 1 && adjustmentSlots.size() == 1,
           "add-adjustment-layer outputs its node and slot");
    if (!adjustmentNodes.empty() && !adjustmentSlots.empty()) {
        entry = view(adjustmentSlots.front());
        expect(entry && entry->index == 0 && entry->adjustment == adjustmentNodes.front() &&
                   entry->kind == scripting::EntryLayerKind::Entry && entry->layer &&
                   entry->layer->name == "Adjustment Layer 1",
               "the Adjustment layer is the top entry and reads back as an adjustment");
        args = at(adjustmentSlots.front());
        args["role"] = Value("opacity");
        args["value"] = Value(0.25);
        expect(fixture.run("bloom.merge.set-entry-parameter", args).succeeded(),
               "the Adjustment layer's opacity is set");
        entry = view(adjustmentSlots.front());
        expect(entry && roleValue(*entry, "opacity") == document::ParameterValue(0.25),
               "the adjustment opacity reads back");
        auto above = fixture.run("bloom.merge.add-adjustment-layer",
                                 {{"composition", composition},
                                  {"merge", mergeId},
                                  {"above", Value(StableId{first.value()})}});
        const auto aboveSlots = Fixture::outputs<document::LayerSlotId>(above, "layerSlot");
        const auto firstEntry = view(first);
        expect(above.succeeded() && !aboveSlots.empty() && firstEntry &&
                   view(aboveSlots.front())->index + 1 == firstEntry->index,
               "an Adjustment layer lands directly above the named entry");
    }

    // ensure-entry-layer reports an existing layer and creates a missing one.
    const auto third = plainEntry();
    auto ensured = fixture.run("bloom.merge.ensure-entry-layer", at(third));
    expect(ensured.succeeded() &&
               Fixture::outputs<document::LayerId>(ensured, "layer").size() == 1 &&
               Fixture::outputs<document::ParameterId>(ensured, "parameter.position").size() == 1,
           "ensure-entry-layer creates the layer and outputs its parameters");
    auto existing = fixture.run("bloom.merge.ensure-entry-layer", at(second));
    const auto existingLayer = Fixture::outputs<document::LayerId>(existing, "layer");
    entry = view(second);
    expect(entry && entry->layer && existingLayer.size() == 1 &&
               existingLayer.front() == entry->layer->layerId,
           "ensure-entry-layer outputs a Layer node entry's layer without a change");
    auto stale = fixture.run("bloom.merge.ensure-entry-layer",
                             {{"composition", composition},
                              {"merge", mergeId},
                              {"slot", Value(StableId{999999})}});
    expect(!stale.succeeded(), "a stale slot is refused");
    fixture.refresh();
    expect(fixture.snapshot->project().validate().ok(), "the project validates");
    expect(scripting::mergeEntryViews(fixture.composition(fixture.main)).size() ==
               fixture.composition(fixture.main).graph().merge(*merge)->entries().size(),
           "every entry is listed once");
}

// #502: every layer edit reaches an entry layer through merge and slot, each one undo step that
// leaves a valid project.
void entryLayerEdits() {
    Fixture fixture;
    fixture.refresh();
    const auto merge = fixture.composition(fixture.main).graph().outputMergeId();
    if (!merge) {
        expect(false, "the composition has an output Merge");
        return;
    }
    const auto composition = Value(StableId{fixture.main.value()});
    const auto plainEntry = [&] {
        auto node = fixture.run("bloom.node.add", {{"composition", composition},
                                                   {"nodeType", Value("bloom.solid-source")}});
        const auto nodes = Fixture::outputs<document::NodeId>(node, "node");
        if (nodes.empty())
            return document::LayerSlotId{};
        const auto slots = Fixture::outputs<document::LayerSlotId>(
            fixture.run("bloom.node.connect",
                        {{"composition", composition},
                         {"sourceNode", Value(StableId{nodes.front().value()})},
                         {"sourcePort", Value("image")},
                         {"targetNode", Value(StableId{merge->value()})},
                         {"role", Value("content")}}),
            "layerSlot");
        return slots.empty() ? document::LayerSlotId{} : slots.front();
    };
    const auto a = plainEntry();
    const auto b = plainEntry();
    const auto layerOf = [&](const document::LayerSlotId slot) {
        fixture.refresh();
        const auto* layer =
            document::mergeEntryLayer(fixture.composition(fixture.main).graph(), *merge, slot);
        return layer ? layer->layerId : document::LayerId{};
    };
    const auto edit = [&](const std::string& op, const document::LayerSlotId slot,
                          Arguments extra) {
        extra["composition"] = composition;
        extra["merge"] = Value(StableId{merge->value()});
        extra["slot"] = Value(StableId{slot.value()});
        const auto before = fixture.session->snapshot().revision();
        auto result = fixture.run(op, extra);
        expect(result.succeeded(), op + " by merge and slot applies");
        fixture.refresh();
        expect(fixture.snapshot->project().validate().ok(), op + " leaves a valid project");
        expect(fixture.snapshot->revision() != before, op + " makes a revision");
        return result;
    };
    document::PathValue square;
    square.editableContour().closed = true;
    square.editableContour().anchors = {{{0, 0}, std::nullopt, std::nullopt},
                                        {{50, 0}, std::nullopt, std::nullopt},
                                        {{50, 50}, std::nullopt, std::nullopt}};
    edit("bloom.layer.set-enabled", a, {{"enabled", Value(false)}});
    edit("bloom.layer.set-enabled", a, {{"enabled", Value(true)}});
    edit("bloom.layer.set-locked", a, {{"locked", Value(true)}});
    edit("bloom.layer.set-locked", a, {{"locked", Value(false)}});
    edit("bloom.layer.set-shy", a, {{"shy", Value(true)}});
    edit("bloom.layer.set-label-color", a, {{"color", Value(document::Vec3d{255, 0, 0})}});
    edit("bloom.layer.rename", a, {{"name", Value("A")}});
    edit("bloom.layer.set-range", a,
         {{"start", Value(std::int64_t{0})}, {"end", Value(std::int64_t{3})}});
    edit("bloom.layer.set-timing", a,
         {{"startTime", Value(std::int64_t{0})},
          {"start", Value(std::int64_t{0})},
          {"end", Value(std::int64_t{4})},
          {"speedNumerator", Value(std::int64_t{1})},
          {"speedDenominator", Value(std::int64_t{1})}});
    edit("bloom.layer.set-time-modes", a,
         {{"before", Value(std::int64_t{1})}, {"after", Value(std::int64_t{1})}});
    edit("bloom.layer.enable-time-remap", a, {});
    edit("bloom.merge.set-entry-parameter", a,
         {{"role", Value("timeRemap")}, {"value", Value(0.0)}, {"time", Value(std::int64_t{2})}});
    edit("bloom.layer.disable-time-remap", a, {});
    auto masked = edit("bloom.layer.add-mask", a, {{"path", Value(square)}});
    const auto masks = Fixture::outputs<document::LayerMaskId>(masked, "mask");
    expect(masks.size() == 1, "add-mask on an entry layer outputs the mask");
    if (!masks.empty()) {
        edit("bloom.layer.set-mask-mode", a,
             {{"mask", Value(StableId{masks.front().value()})}, {"mode", Value(std::int64_t{2})}});
        edit("bloom.layer.set-mask-inverted", a,
             {{"mask", Value(StableId{masks.front().value()})}, {"inverted", Value(true)}});
        edit("bloom.layer.rename-mask", a,
             {{"mask", Value(StableId{masks.front().value()})}, {"name", Value("Hole")}});
    }
    // b gets its layer from its first edit; then a uses it as parent and track matte.
    // Setting a value the entry already has is no edit, so it creates no layer; a real one does.
    edit("bloom.layer.set-shy", b, {{"shy", Value(true)}});
    const auto bLayer = layerOf(b);
    expect(bLayer.isValid(), "b has an entry layer");
    edit("bloom.layer.set-parent", a, {{"parent", Value(StableId{bLayer.value()})}});
    edit("bloom.layer.set-track-matte", a,
         {{"source", Value(StableId{bLayer.value()})},
          {"channel", Value(std::int64_t{1})},
          {"inverted", Value(false)}});
    edit("bloom.layer.clear-track-matte", a, {});
    edit("bloom.layer.set-parent", a, {});
    auto split = edit("bloom.layer.split-at-time", a, {{"time", Value(std::int64_t{2})}});
    expect(Fixture::outputs<document::LayerId>(split, "layer").size() >= 1,
           "split-at-time of an entry layer outputs the tail");
    const auto aLayer = layerOf(a);
    auto duplicated = fixture.run("bloom.layer.duplicate",
                                  {{"composition", composition},
                                   {"layers", Value(ids({aLayer.value()}))}});
    const auto copies = Fixture::outputs<document::LayerId>(duplicated, "layer");
    expect(duplicated.succeeded() && copies.size() == 1, "an entry layer duplicates");
    if (!copies.empty()) {
        auto removed = fixture.run("bloom.layer.delete", {{"composition", composition},
                                                         {"layers", Value(ids({copies[0].value()}))}});
        expect(removed.succeeded(), "an entry layer deletes");
    }
    auto moved = fixture.run("bloom.layer.move-before",
                             {{"composition", composition},
                              {"slot", Value(StableId{b.value()})},
                              {"before", Value(StableId{a.value()})}});
    expect(moved.succeeded(), "an entry slot moves");
    fixture.refresh();
    expect(fixture.snapshot->project().validate().ok(), "the project validates at the end");
}

} // namespace


// #557: curve records with every curve and keyframe id zeroed, so two compositions' animation can
// be compared for equality independent of the fresh ids a copy takes.
[[nodiscard]] std::vector<document::AnimationCurveRecord>
withoutIds(const document::Composition& composition) {
    std::vector<document::AnimationCurveRecord> records;
    for (auto record : composition.animationCurves().records()) {
        std::visit(
            [](auto& curve) {
                using Curve = std::remove_cvref_t<decltype(curve)>;
                curve.id = document::AnimationCurveId{};
                if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                    for (auto& key : curve.keyframes)
                        key.id = document::KeyframeId{};
                } else {
                    for (auto& component : curve.components)
                        for (auto& key : component.keyframes)
                            key.id = document::KeyframeId{};
                }
            },
            record);
        records.push_back(std::move(record));
    }
    return records;
}

[[nodiscard]] std::vector<document::KeyframeId>
keyframeIds(const document::Composition& composition) {
    std::vector<document::KeyframeId> found;
    for (const auto& record : composition.animationCurves().records())
        std::visit(
            [&](const auto& curve) {
                using Curve = std::remove_cvref_t<decltype(curve)>;
                if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                    for (const auto& key : curve.keyframes)
                        found.push_back(key.id);
                } else {
                    for (const auto& component : curve.components)
                        for (const auto& key : component.keyframes)
                            found.push_back(key.id);
                }
            },
            record);
    return found;
}

void duplicateCompositionWithKeyframes() {
    Fixture fixture;
    const auto mainArgument = Value(StableId{fixture.main.value()});
    for (const auto* name : {"One", "Two", "Three"}) {
        expect(fixture
                   .run("bloom.layer.add-solid", {{"composition", mainArgument},
                                                  {"name", Value(std::string(name))},
                                                  {"color", Value(core::Color4d{1, 0, 0, 1})}})
                   .succeeded(),
               "a solid layer is added");
    }
    fixture.refresh();
    // Key every constant scalar and 2D parameter of the layers and their nodes at two times:
    // many curves in one composition, which is what a template shot looks like.
    std::size_t keyed = 0;
    for (const auto& record : fixture.composition(fixture.main).parameters().records()) {
        const auto* constant = std::get_if<document::ConstantValueSource>(&record.source);
        if (constant == nullptr)
            continue;
        for (const std::int64_t time : {1, 2}) {
            Value value(0.0);
            if (const auto* scalar = std::get_if<double>(&constant->value))
                value = Value(*scalar + static_cast<double>(time));
            else if (const auto* vec = std::get_if<document::Vec2d>(&constant->value))
                value = Value(document::Vec2d{vec->x + static_cast<double>(time), vec->y});
            else
                break;
            if (fixture
                    .run("bloom.animation.set-keyframe-at-time-for-parameter",
                         {{"composition", mainArgument},
                          {"parameter", Value(StableId{record.id.value()})},
                          {"time", Value(time)},
                          {"value", value}})
                    .succeeded() &&
                time == 2)
                ++keyed;
        }
    }
    expect(keyed >= 8, "several parameters across layers and nodes are keyed");
    fixture.refresh();
    const auto source = fixture.composition(fixture.main);
    expect(source.animationCurves().records().size() == keyed, "each keyed parameter has a curve");

    auto duplicated = fixture.run("bloom.composition.duplicate", {{"composition", mainArgument}});
    expect(duplicated.succeeded(), "duplicating a composition with many animated properties works");
    const auto copies = Fixture::outputs<document::CompositionId>(duplicated, "composition");
    expect(copies.size() == 1, "duplicate outputs the copy");
    if (copies.size() != 1)
        return;
    fixture.refresh();
    const auto copy = fixture.composition(copies.front());
    expect(withoutIds(copy) == withoutIds(source), "the copy's animation equals the original's");
    expect(copy.parameters().records().size() == source.parameters().records().size() &&
               copy.graph().nodes().size() == source.graph().nodes().size(),
           "the copy has the same parameters and nodes");
    auto sourceKeys = keyframeIds(source);
    const auto copyKeys = keyframeIds(copy);
    expect(copyKeys.size() == sourceKeys.size() && !copyKeys.empty() &&
               std::ranges::none_of(copyKeys,
                                    [&](const auto id) {
                                        return std::ranges::find(sourceKeys, id) !=
                                               sourceKeys.end();
                                    }),
           "the copy's keyframes have fresh ids");
    expect(std::ranges::none_of(copy.animationCurves().records(),
                                [&](const auto& record) {
                                    return source.animationCurves().find(
                                               document::animationCurveId(record)) != nullptr;
                                }),
           "the copy's curves have fresh ids");

    // Independent curves: re-key the copy's first animated parameter; the original is unchanged.
    const auto animated =
        std::ranges::find_if(copy.parameters().records(), [&](const auto& record) {
            const auto* held = std::get_if<document::AnimationCurveSource>(&record.source);
            return held != nullptr && copy.animationCurves().findScalar(held->curveId) != nullptr;
        });
    expect(animated != copy.parameters().records().end(), "the copy has an animated scalar");
    if (animated != copy.parameters().records().end()) {
        expect(fixture
                   .run("bloom.animation.set-keyframe-at-time-for-parameter",
                        {{"composition", Value(StableId{copies.front().value()})},
                         {"parameter", Value(StableId{animated->id.value()})},
                         {"time", Value(std::int64_t{1})},
                         {"value", Value(12345.0)}})
                   .succeeded(),
               "the copy's keyframe is edited");
        fixture.refresh();
        expect(withoutIds(fixture.composition(fixture.main)) == withoutIds(source),
               "editing the copy leaves the original's curves untouched");
        expect(withoutIds(fixture.composition(copies.front())) != withoutIds(source),
               "the edit changed the copy");
        expect(fixture.session->undo().succeeded(), "undo the copy edit");
    }

    // Undo and redo the duplicate itself.
    expect(fixture.session->undo().succeeded(), "undo the duplicate");
    fixture.refresh();
    expect(fixture.snapshot->project().findComposition(copies.front()) == nullptr,
           "undo removes the copy");
    expect(fixture.session->redo().succeeded(), "redo the duplicate");
    fixture.refresh();
    expect(fixture.snapshot->project().findComposition(copies.front()) != nullptr &&
               withoutIds(fixture.composition(copies.front())) == withoutIds(source),
           "redo restores the copy with its animation");

    // Save and reopen.
    const auto path =
        std::filesystem::temp_directory_path() /
        ("bloom-duplicate-keys-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bloom");
    std::filesystem::remove(path);
    expect(fixture.session->saveAs(path).succeeded(), "the project saves");
    auto reopened = scripting::Session::open(path);
    expect(static_cast<bool>(reopened), "the project reopens");
    if (reopened) {
        const auto snapshot = reopened.session()->snapshot();
        const auto* reopenedCopy = snapshot.project().findComposition(copies.front());
        expect(reopenedCopy != nullptr &&
                   std::ranges::equal(reopenedCopy->animationCurves().records(),
                                      copy.animationCurves().records()),
               "save and reopen keep the copy's curves and keys");
    }
    std::filesystem::remove(path);
}

int main() {
    duplicateCompositionWithKeyframes();
    layerOperations();
    addFromAssets();
    nodeOperations();
    parameterOperations();
    pathValues();
    addFeedingInserts();
    nullArguments();
    exportPresetOutputs();
    matteParentedToItsTarget();
    projectItems();
    colorSettings();
    descriptions();
    mergeEntryOperations();
    entryLayerEdits();
    if (failures != 0) {
        std::cerr << failures << " parity operation check(s) failed\n";
        return 1;
    }
    std::cout << "parity operations ok\n";
    return 0;
}
