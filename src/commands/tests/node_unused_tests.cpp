#include "command_test_support.hpp"

#include <bloom/commands/group_definition_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/node_unused_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/image_preview.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/value_nodes.hpp>

#include <algorithm>
#include <set>
#include <string>

// #701: Delete Unused Nodes removes every node with no path to the composition's output, in one
// undo step, through the same removal Delete uses.
namespace bloom::commands::test {
namespace {
using namespace document;

struct Fixture {
    Document document{makeProject()};
    CommandStack stack{document};

    [[nodiscard]] const Composition& composition(const CompositionId id = kCompositionId) const {
        static thread_local Snapshot held = document.snapshot();
        held = document.snapshot();
        return *held.project().findComposition(id);
    }
};

template <typename Op, typename... Args>
CommandResult apply(Fixture& fixture, const CompositionId id, Args&&... args) {
    Transaction transaction("Unused fixture", fixture.document.snapshot().revision());
    transaction.emplace<Op>(id, std::forward<Args>(args)...);
    return fixture.stack.execute(std::move(transaction));
}

NodeId addNode(Fixture& fixture, const std::string_view type,
               const CompositionId id = kCompositionId) {
    const auto added = apply<AddNode>(fixture, id, std::string(type), Vec2d{});
    const auto node = added.outputId<NodeId>(kAddNodeOutput);
    if (!node)
        throw std::logic_error("unused fixture node");
    return *node;
}

void connect(Fixture& fixture, const NodeId from, const InputPortRef& to,
             const std::string& port = "image", const CompositionId id = kCompositionId) {
    const auto result = apply<ConnectPorts>(fixture, id, OutputPortRef{from, port}, to);
    if (!result.changed())
        throw std::logic_error(
            "unused fixture connect " + port + " from node " + std::to_string(from.value()) +
            (result.operationFailures.empty() ? std::string()
                                              : ": " + result.operationFailures.front().issue.message));
}

InputPortRef imageIn(const NodeId node) { return NodeInputRef{node, "image"}; }
InputPortRef blurIn(const NodeId node) { return NodeInputRef{node, "input"}; }

bool has(const Fixture& fixture, const NodeId id, const CompositionId comp = kCompositionId) {
    return fixture.composition(comp).graph().findNode(id) != nullptr;
}

CommandResult deleteUnused(Fixture& fixture, const CompositionId id = kCompositionId) {
    Transaction transaction("Delete Unused Nodes", fixture.document.snapshot().revision());
    transaction.emplace<DeleteUnusedNodes>(id);
    return fixture.stack.execute(std::move(transaction));
}

// Nodes on the path stay, nodes off it go, and the whole removal is one undo step.
void testOnAndOffThePath(TestContext& test) {
    Fixture fixture;
    const auto source = addNode(fixture, kSolidSourceNodeType);
    connect(fixture, source, imageIn(kFirstLayerNodeId));
    const auto effect = addNode(fixture, "bloom.gaussian-blur");
    connect(fixture, source, blurIn(effect));
    const auto preview = addNode(fixture, kImagePreviewNodeType);
    connect(fixture, effect, imageIn(preview));
    const auto feeder = addNode(fixture, kSolidSourceNodeType);
    const auto onlyUnused = addNode(fixture, "bloom.gaussian-blur");
    connect(fixture, feeder, blurIn(onlyUnused));
    const auto loose = addNode(fixture, kSolidSourceNodeType);

    const auto unused = findUnusedNodes(fixture.composition());
    test.expect(unused.removable == std::set<NodeId>{effect, preview, feeder, onlyUnused, loose},
                "the dead-end branch, the chain feeding only an unused node and a loose node "
                "are unused");
    const auto before = fixture.document.snapshot();
    const auto steps = fixture.stack.size();
    const auto result = deleteUnused(fixture);
    test.expect(result.succeeded() && result.changed(), "Delete Unused Nodes applies");
    test.expect(fixture.stack.size() == steps + 1, "it is one undo step");
    test.expect(has(fixture, source) && has(fixture, kFirstLayerNodeId) &&
                    has(fixture, kSecondLayerNodeId) && has(fixture, kLayerStackNodeId),
                "the path to the output stays");
    for (const auto id : {effect, preview, feeder, onlyUnused, loose})
        test.expect(!has(fixture, id), "an unused node goes");
    test.expect(fixture.document.snapshot().project().validate().ok(),
                "the result is a valid project");
    test.expect(fixture.stack.undo().changed() &&
                    fixture.composition().graph().nodes().size() ==
                        before.project().findComposition(kCompositionId)->graph().nodes().size() &&
                    fixture.composition().graph().edges().size() ==
                        before.project().findComposition(kCompositionId)->graph().edges().size(),
                "one undo restores every removed node");
}

void testNothingUnusedIsNoChange(TestContext& test) {
    Fixture fixture;
    const auto source = addNode(fixture, kSolidSourceNodeType);
    connect(fixture, source, imageIn(kFirstLayerNodeId));
    const auto steps = fixture.stack.size();
    const auto revision = fixture.document.snapshot().revision();
    const auto result = deleteUnused(fixture);
    test.expect(!result.changed() && fixture.stack.size() == steps &&
                    fixture.document.snapshot().revision() == revision,
                "nothing unused is a no-change with no undo step");
    test.expect(!deleteUnused(fixture, CompositionId::fromRaw(999)).succeeded(),
                "an unknown composition is refused");
}

// A value node drives a parameter of a used node (transitively), so it is on the path; one that
// drives only an unused node's parameter is not.
void testDriversCountAsPaths(TestContext& test) {
    Fixture fixture;
    const auto used = addNode(fixture, kScalarMathNodeType);
    const auto relay = addNode(fixture, kScalarValueNodeType);
    const auto unusedDriver = addNode(fixture, kScalarValueNodeType);
    const auto effect = addNode(fixture, "bloom.gaussian-blur");
    const std::string value(kValuePortName);
    const std::string result(kResultPortName);
    connect(fixture, used, NodeInputRef{kFirstLayerNodeId, std::string(kOpacityParameterRole)},
            result);
    connect(fixture, relay, NodeInputRef{used, std::string(kFirstOperandPortName)}, value);
    const auto exposure = std::ranges::find_if(
        fixture.composition().graph().findNode(effect)->parameters,
        [&](const auto& binding) { return binding.role == "radius"; });
    if (exposure == fixture.composition().graph().findNode(effect)->parameters.end())
        throw std::logic_error("unused fixture exposure role");
    connect(fixture, unusedDriver, NodeInputRef{effect, "radius"}, value);

    const auto unused = findUnusedNodes(fixture.composition());
    test.expect(unused.removable == std::set<NodeId>{unusedDriver, effect},
                "a value node driving a used node, directly or through another, is used");
    test.expect(deleteUnused(fixture).changed() && has(fixture, used) && has(fixture, relay) &&
                    !has(fixture, unusedDriver) && !has(fixture, effect),
                "the drivers of used nodes stay and the unused ones go");
    test.expect(fixture.stack.undo().changed() && has(fixture, unusedDriver) && has(fixture, effect),
                "undo restores the driver and what it drove");
}

// #716: an Output node that is not the composition's output is unused like any other node; what
// the composition renders through stays, as it is on the path.
void testProtectedNodesStay(TestContext& test) {
    Fixture fixture;
    const auto output = addNode(fixture, kCompositionOutputNodeType);
    const auto loose = addNode(fixture, kSolidSourceNodeType);
    const auto unused = findUnusedNodes(fixture.composition());
    test.expect(unused.removable == std::set<NodeId>{loose, output} && unused.staying.empty(),
                "an Output node the composition does not render through is unused");
    test.expect(deleteUnused(fixture).changed() && !has(fixture, output) && !has(fixture, loose) &&
                    has(fixture, kLayerStackNodeId),
                "the unused nodes go and the Merge the composition renders through stays");
}

// A frame loses deleted members; one left empty goes; one with no deleted member is untouched.
void testLayoutFrames(TestContext& test) {
    Fixture fixture;
    const auto source = addNode(fixture, kSolidSourceNodeType);
    connect(fixture, source, imageIn(kFirstLayerNodeId));
    const auto a = addNode(fixture, kSolidSourceNodeType);
    const auto b = addNode(fixture, kSolidSourceNodeType);
    const auto c = addNode(fixture, kSolidSourceNodeType);
    const auto allGone = apply<GroupNodes>(fixture, kCompositionId, std::set<NodeId>{a, b}, "All");
    const auto some = apply<GroupNodes>(fixture, kCompositionId, std::set<NodeId>{c, source}, "Some");
    const auto none = apply<GroupNodes>(fixture, kCompositionId,
                                        std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId}, "None");
    const auto allId = allGone.outputId<NodeGroupId>(kGroupNodesOutput);
    const auto someId = some.outputId<NodeGroupId>(kGroupNodesOutput);
    const auto noneId = none.outputId<NodeGroupId>(kGroupNodesOutput);
    if (!allId || !someId || !noneId)
        throw std::logic_error("unused fixture frames");

    test.expect(deleteUnused(fixture).changed(), "unused members are deleted");
    const auto& groups = fixture.composition().nodeGroups();
    test.expect(!groups.contains(*allId), "a frame whose members all went is removed");
    test.expect(groups.contains(*someId) && groups.at(*someId).members == std::set<NodeId>{source},
                "a frame keeps its used members");
    test.expect(groups.contains(*noneId) &&
                    groups.at(*noneId).members ==
                        std::set<NodeId>{kFirstLayerNodeId, kSecondLayerNodeId},
                "a frame with no deleted member is untouched");
    test.expect(fixture.stack.undo().changed() && fixture.composition().nodeGroups().contains(*allId) &&
                    fixture.composition().nodeGroups().at(*someId).members.size() == 2,
                "undo restores the frames with their members");
}

// An encapsulated Node Group node on the path stays with its inside untouched; one off the path
// goes like any node and leaves the definition. Inside a definition, Group Input and Group Output
// stay and what feeds Group Output is used.
void testEncapsulatedGroups(TestContext& test) {
    Fixture fixture;
    const auto onPath = addNode(fixture, kNodeGroupNodeType);
    const auto created = apply<CreateGroupDefinition>(fixture, kCompositionId, "Look", onPath);
    const auto definitionId = created.outputId<CompositionId>(kCreateGroupDefinitionOutput);
    if (!definitionId)
        throw std::logic_error("unused fixture group definition");
    connect(fixture, onPath, imageIn(kFirstLayerNodeId), "image1");
    const auto offPath = addNode(fixture, kNodeGroupNodeType);
    if (!apply<SetNodeParameterValue>(fixture, kCompositionId, offPath,
                                      std::string(kNodeGroupReferenceRole),
                                      static_cast<std::int64_t>(definitionId->value()))
             .changed())
        throw std::logic_error("unused fixture group reference");

    // Inside the definition: an extra node feeding nothing is unused.
    const auto inside = addNode(fixture, kSolidSourceNodeType, *definitionId);
    const auto definitionBefore = fixture.composition(*definitionId).graph().nodes().size();
    test.expect(deleteUnused(fixture, *definitionId).changed() && !has(fixture, inside, *definitionId) &&
                    fixture.composition(*definitionId).graph().nodes().size() == definitionBefore - 1,
                "inside a definition only the unused node goes");
    test.expect(findGroupInput(fixture.composition(*definitionId)) != nullptr &&
                    findGroupOutput(fixture.composition(*definitionId)) != nullptr,
                "Group Input and Group Output stay");
    const auto definitionNodes = fixture.composition(*definitionId).graph().nodes().size();

    test.expect(deleteUnused(fixture).changed(), "the host's unused Node Group is deleted");
    test.expect(has(fixture, onPath) && !has(fixture, offPath),
                "a Node Group on the output path stays, one off it goes");
    test.expect(fixture.composition(*definitionId).graph().nodes().size() == definitionNodes,
                "the definition's inside is untouched");
    test.expect(fixture.document.snapshot().project().validate().ok(), "the project stays valid");
}
} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testOnAndOffThePath(test);
        bloom::commands::test::testNothingUnusedIsNoChange(test);
        bloom::commands::test::testDriversCountAsPaths(test);
        bloom::commands::test::testProtectedNodesStay(test);
        bloom::commands::test::testLayoutFrames(test);
        bloom::commands::test::testEncapsulatedGroups(test);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return test.failures() == 0 ? 0 : 1;
}
