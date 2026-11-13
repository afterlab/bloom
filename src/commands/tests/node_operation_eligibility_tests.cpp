#include "command_test_support.hpp"

#include <bloom/commands/entry_layer_operations.hpp>

#include "layer_lock.hpp"

#include <bloom/commands/layer_operations.hpp>
#include <bloom/document/value_nodes.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bloom::commands::test {
namespace {
using namespace document;

struct Fixture {
    Document document{makeProject()};
    CommandStack stack{document};
};

template <typename Op, typename... Args> CommandResult apply(Fixture& fixture, Args&&... args) {
    Transaction transaction("Eligibility fixture", fixture.document.snapshot().revision());
    transaction.emplace<Op>(kCompositionId, std::forward<Args>(args)...);
    return fixture.stack.execute(std::move(transaction));
}

NodeId addNode(Fixture& fixture, const std::string_view type) {
    const auto id =
        apply<AddNode>(fixture, std::string(type), Vec2d{}).outputId<NodeId>(kAddNodeOutput);
    if (!id)
        throw std::logic_error("eligibility fixture node");
    return *id;
}

void connect(Fixture& fixture, const NodeId from, const InputPortRef& to) {
    if (!apply<ConnectPorts>(fixture, OutputPortRef{from, "image"}, to).changed())
        throw std::logic_error("eligibility fixture connect");
}

// What the command stack would accept: the operation applies, the draft validates, and no locked
// layer changes. This is the reference every predicate is compared against.
bool reference(const Snapshot& snapshot, const Operation& operation) {
    Document isolated(snapshot.project(), snapshot.ids().highWater());
    auto draft = isolated.draft(isolated.snapshot());
    const auto result = operation.apply(draft);
    if (result.status == OperationStatus::Rejected)
        return false;
    // #502: the command stack reconciles entries and Layer nodes before it checks and validates.
    if (reconcileEntryLayers(snapshot.project(), draft).failure)
        return false;
    if (detail::changesLockedLayers(snapshot.project(), draft.project()))
        return false;
    return draft.validate().ok();
}

struct Checker {
    TestContext& test;
    const Snapshot snapshot;
    const Composition& comp;
    std::vector<std::string> failures;

    Checker(TestContext& testContext, const Fixture& fixture)
        : test(testContext), snapshot(fixture.document.snapshot()),
          comp(*snapshot.project().findComposition(kCompositionId)) {}

    void same(const bool predicate, const Operation& operation, const std::string_view what) {
        test.expect(predicate == reference(snapshot, operation),
                    std::string("predicate disagrees with command acceptance: ") +
                        std::string(what));
    }

    std::vector<NodeId> nodeIds() const {
        std::vector<NodeId> ids{NodeId::fromRaw(9999)};
        for (const auto& node : comp.graph().nodes())
            ids.push_back(node.id);
        return ids;
    }

    void run() {
        const auto ids = nodeIds();
        for (const auto id : ids) {
            const auto label = "node " + std::to_string(id.value());
            same(canRemoveNodes(comp, {id}), RemoveNodes(kCompositionId, {id}), "remove " + label);
            same(canDissolveNode(comp, id), DissolveNode(kCompositionId, id), "dissolve " + label);
            for (const bool value : {false, true}) {
                same(canSetNodeMuted(comp, id, value), SetNodeMuted(kCompositionId, id, value),
                     "mute " + label);
                same(canSetNodeCollapsed(comp, id, value),
                     SetNodeCollapsed(kCompositionId, id, value), "collapse " + label);
            }
            same(canGroupNodes(comp, snapshot.ids(), {id}, kDefaultNodeGroupName),
                 GroupNodes(kCompositionId, {id}, std::string(kDefaultNodeGroupName)),
                 "group " + label);
        }
        // Multi-selection: every pair, plus all nodes together.
        for (std::size_t a = 0; a < ids.size(); ++a)
            for (std::size_t b = a + 1; b < ids.size(); ++b) {
                const std::set<NodeId> pair{ids[a], ids[b]};
                same(canRemoveNodes(comp, pair), RemoveNodes(kCompositionId, pair), "remove pair");
                same(canGroupNodes(comp, snapshot.ids(), pair, kDefaultNodeGroupName),
                     GroupNodes(kCompositionId, pair, std::string(kDefaultNodeGroupName)),
                     "group pair");
            }
        const std::set<NodeId> all(ids.begin(), ids.end());
        same(canRemoveNodes(comp, all), RemoveNodes(kCompositionId, all), "remove all + missing");
        same(canRemoveNodes(comp, {}), RemoveNodes(kCompositionId, {}), "remove empty");
        same(canGroupNodes(comp, snapshot.ids(), {}, kDefaultNodeGroupName),
             GroupNodes(kCompositionId, {}, std::string(kDefaultNodeGroupName)), "group empty");
        same(canGroupNodes(comp, snapshot.ids(), {kFirstLayerNodeId}, ""),
             GroupNodes(kCompositionId, {kFirstLayerNodeId}, ""), "group empty name");
        for (const auto& layer : comp.graph().layerOutputs()) {
            same(canRenameLayer(comp, layer.layerId, layer.name),
                 RenameLayer(kCompositionId, layer.layerId, layer.name), "rename same");
            same(canRenameLayer(comp, layer.layerId, "Renamed"),
                 RenameLayer(kCompositionId, layer.layerId, "Renamed"), "rename new");
            same(canRenameLayer(comp, layer.layerId, ""),
                 RenameLayer(kCompositionId, layer.layerId, ""), "rename empty");
        }
        same(canRenameLayer(comp, LayerId::fromRaw(9999), "Renamed"),
             RenameLayer(kCompositionId, LayerId::fromRaw(9999), "Renamed"), "rename missing");
    }
};

void testBase(TestContext& test) {
    Fixture fixture;
    Checker(test, fixture).run();
    const Checker checker(test, fixture);
    test.expect(canRemoveNodes(checker.comp, {kLayerStackNodeId}) &&
                    canDissolveNode(checker.comp, kLayerStackNodeId),
                "#716: the composition's Merge can be removed and dissolved");
    test.expect(canDissolveNode(checker.comp, kFirstLayerNodeId),
                "#704: a Layer Output without a connected image input dissolves by deleting");
    test.expect(canRemoveNodes(checker.comp, {kFirstLayerNodeId}), "a Layer Output can be removed");
    test.expect(!canRemoveNodes(checker.comp, {NodeId::fromRaw(9999)}), "missing node refused");
}

// #704, #716: every Merge deletes and dissolves, the one that is the composition's output included;
// only a stale id (and a locked layer's node) refuses.
void testRemovalRefusals(TestContext& test) {
    Fixture fixture;
    const auto merge = addNode(fixture, kLayerStackNodeType);
    const auto& comp =
        *fixture.document.snapshot().project().findComposition(kCompositionId);
    // This fixture's composition output is the layer stack itself; the UI tests cover an Output node.
    test.expect(nodeRemovalRefusal(comp, kLayerStackNodeId) == NodeRemovalRefusal::None,
                "#716: the Merge that is the composition's output can be deleted");
    test.expect(nodeRemovalRefusal(comp, NodeId::fromRaw(9999)) == NodeRemovalRefusal::Missing,
                "#704: a stale id is reported as missing");
    test.expect(nodeRemovalRefusal(comp, merge) == NodeRemovalRefusal::None &&
                    canRemoveNodes(comp, {merge}) && canDissolveNode(comp, merge),
                "#704: a Merge the user added can be deleted and dissolved");
    test.expect(apply<RemoveNodes>(fixture, std::set<NodeId>{merge}).succeeded() &&
                    !fixture.document.snapshot()
                         .project()
                         .findComposition(kCompositionId)
                         ->graph()
                         .findNode(merge),
                "#704: Delete removes a Merge the user added");
    test.expect(fixture.stack.undo().succeeded(), "#704: the Merge's delete undoes");
    test.expect(apply<DissolveNode>(fixture, merge).succeeded() &&
                    !fixture.document.snapshot()
                         .project()
                         .findComposition(kCompositionId)
                         ->graph()
                         .findNode(merge),
                "#704: Dissolve removes a Merge the user added");
}

void testLinkedGraph(TestContext& test) {
    Fixture fixture;
    const auto source = addNode(fixture, kSolidSourceNodeType);
    const auto loose = addNode(fixture, kLayerOutputNodeType);
    const auto output = addNode(fixture, kCompositionOutputNodeType);
    (void)output;
    connect(fixture, source, NodeInputRef{loose, "image"});
    for (const auto target : {kFirstLayerNodeId, kSecondLayerNodeId}) {
        if (!apply<ConnectPorts>(fixture, OutputPortRef{loose, "image"},
                                 InputPortRef{NodeInputRef{target, "image"}})
                 .changed())
            throw std::logic_error("eligibility consumer");
    }
    (void)apply<SetNodeMuted>(fixture, source, true);
    (void)apply<SetNodeCollapsed>(fixture, loose, true);
    (void)apply<GroupNodes>(fixture, std::set<NodeId>{source, loose},
                            std::string(kDefaultNodeGroupName));
    Checker(test, fixture).run();
    const Checker checker(test, fixture);
    test.expect(canDissolveNode(checker.comp, loose), "connected pass-through dissolves");
    test.expect(canDissolveNode(checker.comp, kFirstLayerNodeId),
                "a Layer node fed by a Layer node dissolves; its entry takes the upstream layer");
    test.expect(canDissolveNode(checker.comp, source),
                "#704: a source without an image input dissolves by deleting");
    test.expect(canSetNodeMuted(checker.comp, source, true), "already-muted is NoChange, accepted");
}

void testDrivenParameter(TestContext& test) {
    Fixture fixture;
    const auto scalar = addNode(fixture, kScalarValueNodeType);
    if (!apply<ConnectPorts>(
             fixture, OutputPortRef{scalar, std::string(kValuePortName)},
             InputPortRef{NodeInputRef{kFirstLayerNodeId, std::string(kOpacityParameterRole)}})
             .changed())
        throw std::logic_error("eligibility driver");
    Checker(test, fixture).run();
    const Checker checker(test, fixture);
    test.expect(canRemoveNodes(checker.comp, {scalar}),
                "#704: removing the driver of a surviving parameter is accepted; the parameter "
                "returns to its default");
    test.expect(canRemoveNodes(checker.comp, {scalar, kFirstLayerNodeId}),
                "removing driver and driven node together is accepted");
}

void testLocked(TestContext& test) {
    Fixture fixture;
    const auto source = addNode(fixture, kSolidSourceNodeType);
    const auto unrelated = addNode(fixture, kSolidSourceNodeType);
    (void)unrelated;
    connect(fixture, source, NodeInputRef{kFirstLayerNodeId, "image"});
    if (!apply<SetLayerLocked>(fixture, kFirstLayerId, true).changed())
        throw std::logic_error("eligibility lock");
    Checker(test, fixture).run();
    const Checker checker(test, fixture);
    test.expect(!canRemoveNodes(checker.comp, {source}), "upstream of a locked layer is protected");
    test.expect(!canRemoveNodes(checker.comp, {kFirstLayerNodeId}), "locked boundary is protected");
    test.expect(!canDissolveNode(checker.comp, source), "locked upstream node cannot dissolve");
    test.expect(canRemoveNodes(checker.comp, {kSecondLayerNodeId}), "unlocked layer still removes");
    test.expect(nodeRemovalRefusal(checker.comp, source) == NodeRemovalRefusal::Locked &&
                    nodeRemovalRefusal(checker.comp, kSecondLayerNodeId) == NodeRemovalRefusal::None,
                "#704: a node of a locked layer stays with the lock as its reason");
    test.expect(canRenameLayer(checker.comp, kFirstLayerId, "Locked rename"),
                "rename of a locked layer matches command acceptance");
    // #470: what feeds a locked layer is locked with it.
    test.expect(!apply<ConnectPorts>(fixture, OutputPortRef{unrelated, "image"},
                                     InputPortRef{NodeInputRef{kFirstLayerNodeId, "image"}})
                     .changed(),
                "a locked layer's image input cannot be rewired");
}

void testNoValidationAndNoDocumentCopy(TestContext& test) {
    Fixture fixture;
    const auto source = addNode(fixture, kSolidSourceNodeType);
    connect(fixture, source, NodeInputRef{kFirstLayerNodeId, "image"});
    const auto snapshot = fixture.document.snapshot();
    const auto& comp = *snapshot.project().findComposition(kCompositionId);
    const auto validations = Project::validationCount();
    const auto lockBuilds = Composition::lockIndexBuilds();
    bool sink = false;
    for (int round = 0; round < 50; ++round)
        for (const auto& node : comp.graph().nodes()) {
            sink ^= canRemoveNodes(comp, {node.id});
            sink ^= canDissolveNode(comp, node.id);
            sink ^= canSetNodeMuted(comp, node.id, true);
            sink ^= canSetNodeCollapsed(comp, node.id, true);
            sink ^= canGroupNodes(comp, snapshot.ids(), {node.id}, kDefaultNodeGroupName);
        }
    for (const auto& layer : comp.graph().layerOutputs())
        sink ^= canRenameLayer(comp, layer.layerId, "Other");
    test.expect(Project::validationCount() == validations,
                "eligibility predicates perform no project validation");
    test.expect(Composition::lockIndexBuilds() == lockBuilds,
                "eligibility predicates answer lock queries from the published index");
    (void)sink;
}

// #704: Delete and Dissolve work on every registered node type, alone and wired, and each is one
// undo step. A type the menu offers Delete or Dissolve for is deleted or dissolved by the command;
// the ones the commands refuse are only the composition Output and its Merge.
void testEveryNodeTypeRemovesAndDissolves(TestContext& test) {
    const auto& registry = builtInNodeDefinitions();
    std::size_t tried = 0;
    for (const auto& definition : registry.definitions()) {
        const std::string type = definition.key.typeId;
        Fixture fixture;
        const auto added = apply<AddNode>(fixture, type, Vec2d{});
        const auto id = added.outputId<NodeId>(kAddNodeOutput);
        if (!id)
            continue; // A singleton already present, or a type with its own creation command.
        ++tried;
        const auto describe = [&](const char* what) { return std::string(what) + " " + type; };
        // Wire it: one provider node per kind of output the registry offers, each connected to the
        // first input of the node that accepts it; then its first output into a Layer node.
        std::map<int, std::pair<NodeId, std::string>> providers;
        for (const auto& provider : registry.definitions()) {
            if (provider.outputs.empty() || provider.key.typeId == type)
                continue;
            const int kind = static_cast<int>(provider.outputs.front().valueKind);
            if (providers.contains(kind))
                continue;
            const auto sourceId =
                apply<AddNode>(fixture, provider.key.typeId, Vec2d{}).outputId<NodeId>(kAddNodeOutput);
            if (sourceId)
                providers.emplace(kind, std::pair{*sourceId, provider.outputs.front().name});
        }
        for (const auto& input : definition.inputs)
            for (const auto& [kind, provider] : providers)
                if (apply<ConnectPorts>(fixture, OutputPortRef{provider.first, provider.second},
                                        InputPortRef{NodeInputRef{*id, input.name}})
                        .changed())
                    break;
        for (const auto& output : definition.outputs) {
            if (apply<ConnectPorts>(fixture, OutputPortRef{*id, output.name},
                                    InputPortRef{NodeInputRef{kFirstLayerNodeId, "image"}})
                    .changed())
                break;
            if (apply<ConnectPorts>(
                    fixture, OutputPortRef{*id, output.name},
                    InputPortRef{NodeInputRef{kFirstLayerNodeId, std::string(kOpacityParameterRole)}})
                    .changed())
                break;
        }
        const auto before = fixture.stack.size();
        const auto composition = fixture.document.snapshot().project().findComposition(kCompositionId);
        const bool locked = composition != nullptr && composition->nodeLocked(*id);
        test.expect(!locked, describe("added node is not locked"));
        for (const bool dissolve : {false, true}) {
            const auto result =
                dissolve ? apply<DissolveNode>(fixture, *id) : apply<RemoveNodes>(fixture, std::set<NodeId>{*id});
            const bool removed = !fixture.document.snapshot()
                                      .project()
                                      .findComposition(kCompositionId)
                                      ->graph()
                                      .findNode(*id);
            test.expect(result.succeeded() && removed,
                        describe(dissolve ? "Dissolve removes" : "Delete removes"));
            if (!result.succeeded())
                continue;
            test.expect(fixture.stack.size() == before + 1,
                        describe(dissolve ? "Dissolve is one undo step for" : "Delete is one undo step for"));
            test.expect(fixture.stack.undo().succeeded(), describe("undo restores after"));
            test.expect(fixture.document.snapshot()
                            .project()
                            .findComposition(kCompositionId)
                            ->graph()
                            .findNode(*id) != nullptr,
                        describe("undo brings the node back for"));
        }
    }
    test.expect(tried > 100, "the loop reaches the registry (" + std::to_string(tried) + " types)");
}

void testDissolveReconnectsOrDeletes(TestContext& test) {
    Fixture fixture;
    const auto source = addNode(fixture, kSolidSourceNodeType);
    const auto middle = addNode(fixture, kLayerOutputNodeType);
    connect(fixture, source, NodeInputRef{middle, "image"});
    if (!apply<ConnectPorts>(fixture, OutputPortRef{middle, "image"},
                             InputPortRef{NodeInputRef{kFirstLayerNodeId, "image"}})
             .changed())
        throw std::logic_error("dissolve consumer");
    // Types allow: the consumer is reconnected to the source.
    test.expect(apply<DissolveNode>(fixture, middle).succeeded(), "Dissolve through a connected node");
    const auto& graph = fixture.document.snapshot().project().findComposition(kCompositionId)->graph();
    test.expect(!graph.findNode(middle) &&
                    std::ranges::any_of(graph.edges(),
                                        [&](const auto& edge) {
                                            const auto* destination =
                                                std::get_if<NodeInputRef>(&edge.destination);
                                            return edge.source.nodeId == source && destination != nullptr &&
                                                   destination->nodeId == kFirstLayerNodeId;
                                        }),
                "Dissolve reconnects the consumer to the source when the types allow");
    // Nothing to reconnect through: the node is deleted and its links break.
    const auto loose = addNode(fixture, kSolidSourceNodeType);
    connect(fixture, loose, NodeInputRef{kSecondLayerNodeId, "image"});
    test.expect(apply<DissolveNode>(fixture, loose).succeeded(), "Dissolve of a source deletes it");
    const auto& after = fixture.document.snapshot().project().findComposition(kCompositionId)->graph();
    test.expect(!after.findNode(loose) &&
                    std::ranges::none_of(after.edges(),
                                         [&](const auto& edge) { return edge.source.nodeId == loose; }),
                "and breaks its links");
}

// A value node that drives a surviving parameter deletes, and the parameter returns to its default.
void testDeleteBreaksDrivenParameter(TestContext& test) {
    Fixture fixture;
    const auto scalar = addNode(fixture, kScalarValueNodeType);
    if (!apply<ConnectPorts>(
             fixture, OutputPortRef{scalar, std::string(kValuePortName)},
             InputPortRef{NodeInputRef{kFirstLayerNodeId, std::string(kOpacityParameterRole)}})
             .changed())
        throw std::logic_error("driver");
    const auto before = fixture.stack.size();
    test.expect(apply<RemoveNodes>(fixture, std::set<NodeId>{scalar}).succeeded(),
                "Delete of a node that drives a surviving parameter");
    test.expect(fixture.stack.size() == before + 1, "in one undo step");
    const auto& composition = *fixture.document.snapshot().project().findComposition(kCompositionId);
    const auto* layer = composition.graph().findNode(kFirstLayerNodeId);
    const auto binding = std::ranges::find(layer->parameters, std::string(kOpacityParameterRole),
                                           &ParameterBinding::role);
    const auto* parameter = binding == layer->parameters.end()
                                ? nullptr
                                : composition.parameters().find(binding->parameterId);
    test.expect(parameter != nullptr &&
                    std::holds_alternative<ConstantValueSource>(parameter->source),
                "the driven parameter is a constant again");
    test.expect(fixture.stack.undo().succeeded() &&
                    fixture.document.snapshot().project().findComposition(kCompositionId)->graph().findNode(
                        scalar) != nullptr,
                "and undo gives the driver back");
}
} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testBase(test);
        bloom::commands::test::testRemovalRefusals(test);
        bloom::commands::test::testLinkedGraph(test);
        bloom::commands::test::testDrivenParameter(test);
        bloom::commands::test::testLocked(test);
        bloom::commands::test::testNoValidationAndNoDocumentCopy(test);
        bloom::commands::test::testDissolveReconnectsOrDeletes(test);
        bloom::commands::test::testDeleteBreaksDrivenParameter(test);
        bloom::commands::test::testEveryNodeTypeRemovesAndDissolves(test);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return test.failures() == 0 ? 0 : 1;
}
