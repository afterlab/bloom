// #703: the socket matching an Add placed on a link uses, and the InsertNodeOnLink command that the
// Node editor and bloom.node.insert-on-link share.
#include "command_test_support.hpp"

#include <bloom/commands/node_link_insertion.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/value_nodes.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace bloom::commands::test {
namespace {
using namespace document;

[[nodiscard]] NodeDefinition definition(std::vector<InputPortDefinition> inputs,
                                        std::vector<OutputPortDefinition> outputs,
                                        std::vector<ParameterDefinition> parameters = {}) {
    NodeDefinition result;
    result.key = {"test.node", 1};
    result.inputs = std::move(inputs);
    result.outputs = std::move(outputs);
    result.parameters = std::move(parameters);
    return result;
}

[[nodiscard]] auto accepting(const SocketValueKind destination) {
    return [destination](const SocketValueKind kind) {
        return isAcceptedSocketConnection(kind, destination);
    };
}

void testSocketMatching(TestContext& test) {
    // An effect: a Mask input that also takes an image (listed first), its Image input, and a
    // Scalar parameter socket.
    InputPortDefinition matte{"mask", SocketValueKind::Mask, false};
    matte.acceptsImageMatte = true;
    const auto effect = definition({matte,
                                    {"input", SocketValueKind::Image},
                                    {"radius", SocketValueKind::Scalar, false}},
                                   {{"matte", SocketValueKind::Mask}, {"image", SocketValueKind::Image}},
                                   {{"radius", "test.radius"}});
    const auto image = linkInsertionSockets(effect, SocketValueKind::Image, SocketValueKind::Image,
                                            accepting(SocketValueKind::Image));
    test.expect(image && image->input == "input" && !image->layerSlot && image->output == "image",
                "an image link runs through the image input and output, not the matte that also "
                "takes an image");
    const auto mask = linkInsertionSockets(effect, SocketValueKind::Mask, SocketValueKind::Mask,
                                           accepting(SocketValueKind::Mask));
    test.expect(mask && mask->input == "mask" && mask->output == "matte",
                "a mask link runs through the mask sockets");

    // A transport socket beats a parameter's socket of the same kind.
    const auto value = definition({{"amount", SocketValueKind::Scalar, false},
                                   {"value", SocketValueKind::Scalar}},
                                  {{"value", SocketValueKind::Scalar}}, {{"amount", "test.amount"}});
    const auto scalar = linkInsertionSockets(value, SocketValueKind::Scalar, SocketValueKind::Scalar,
                                             accepting(SocketValueKind::Scalar));
    test.expect(scalar && scalar->input == "value",
                "a transport input is chosen before a parameter's socket");

    // An exact kind beats a promotion; a promotion is used when nothing is exact.
    const auto vectors = definition({{"offset", SocketValueKind::Vector2},
                                     {"amount", SocketValueKind::Scalar}},
                                    {{"length", SocketValueKind::Scalar},
                                     {"vector", SocketValueKind::Vector2}});
    const auto exact = linkInsertionSockets(vectors, SocketValueKind::Scalar,
                                            SocketValueKind::Vector2,
                                            accepting(SocketValueKind::Vector2));
    test.expect(exact && exact->input == "amount" && exact->output == "vector",
                "exact kinds win over a Scalar promoted to a Vector2, on both sides");
    const auto promoted = linkInsertionSockets(
        definition({{"offset", SocketValueKind::Vector2}}, {{"length", SocketValueKind::Scalar}}),
        SocketValueKind::Scalar, SocketValueKind::Vector2, accepting(SocketValueKind::Vector2));
    test.expect(promoted && promoted->input == "offset" && promoted->output == "length",
                "a promotion is accepted when no socket matches exactly");

    // Either side without a match leaves the link alone.
    test.expect(!linkInsertionSockets(definition({}, {{"value", SocketValueKind::Scalar}}),
                                      SocketValueKind::Image, SocketValueKind::Image,
                                      accepting(SocketValueKind::Image)),
                "a source node has no input for the link");
    test.expect(!linkInsertionSockets(definition({{"input", SocketValueKind::Image}},
                                                 {{"value", SocketValueKind::Scalar}}),
                                      SocketValueKind::Image, SocketValueKind::Image,
                                      accepting(SocketValueKind::Image)),
                "a node whose outputs the destination does not take is not inserted");

    // A Merge takes an image link as a new layer slot.
    auto merge = definition({}, {{"image", SocketValueKind::Image}});
    merge.layerSlotInput = LayerSlotInputDefinition{"content", SocketValueKind::Image};
    const auto stacked = linkInsertionSockets(merge, SocketValueKind::Image, SocketValueKind::Image,
                                              accepting(SocketValueKind::Image));
    test.expect(stacked && stacked->layerSlot && stacked->input == "content",
                "a Merge takes the link as a new slot of its content stack");
}

struct Fixture {
    Document document{makeProject()};
    CommandStack stack{document};
};

template <typename Op, typename... Args> CommandResult apply(Fixture& fixture, Args&&... args) {
    Transaction transaction("Insert fixture", fixture.document.snapshot().revision());
    transaction.emplace<Op>(kCompositionId, std::forward<Args>(args)...);
    return fixture.stack.execute(std::move(transaction));
}

NodeId addNode(Fixture& fixture, const std::string_view type, const Vec2d at = {}) {
    const auto id = apply<AddNode>(fixture, std::string(type), at).outputId<NodeId>(kAddNodeOutput);
    if (!id)
        throw std::logic_error("insert fixture node");
    return *id;
}

[[nodiscard]] std::optional<OutputPortRef> source(Fixture& fixture, const InputPortRef& input) {
    const auto snapshot = fixture.document.snapshot();
    return linkSource(*snapshot.project().findComposition(kCompositionId), input);
}

void testInsertIsOneUndoStep(TestContext& test) {
    Fixture fixture;
    const auto solid = addNode(fixture, kSolidSourceNodeType, {0, 0});
    const auto blur = addNode(fixture, "bloom.gaussian-blur", {800, 0});
    const InputPortRef blurInput = NodeInputRef{blur, "input"};
    test.expect(apply<ConnectPorts>(fixture, OutputPortRef{solid, "image"}, blurInput).changed(),
                "the fixture link connects");
    const auto before = fixture.stack.size();
    const auto inserted = apply<InsertNodeOnLink>(fixture, std::string("bloom.gaussian-blur"),
                                                  blurInput, Vec2d{400, 0});
    const auto node = inserted.outputId<NodeId>("editorNode");
    test.expect(inserted.changed() && node.has_value() && fixture.stack.size() == before + 1,
                "inserting on a link is one history entry");
    if (!node)
        return;
    test.expect(source(fixture, blurInput) == OutputPortRef{*node, "image"},
                "the new node's image output feeds the old destination");
    test.expect(source(fixture, NodeInputRef{*node, "input"}) == OutputPortRef{solid, "image"},
                "the old source feeds the new node's image input");
    test.expect(fixture.stack.undo().changed() &&
                    source(fixture, blurInput) == OutputPortRef{solid, "image"} &&
                    fixture.document.snapshot()
                            .project()
                            .findComposition(kCompositionId)
                            ->graph()
                            .findNode(*node) == nullptr,
                "one undo removes the node and restores the link");

    const auto stale = apply<InsertNodeOnLink>(fixture, std::string("bloom.gaussian-blur"),
                                               InputPortRef{NodeInputRef{solid, "missing"}},
                                               Vec2d{});
    test.expect(!stale.succeeded(), "a link that does not exist is refused");
    const auto unmatched = apply<InsertNodeOnLink>(
        fixture, std::string(kScalarValueNodeType), blurInput, Vec2d{});
    test.expect(!unmatched.succeeded() &&
                    source(fixture, blurInput) == OutputPortRef{solid, "image"},
                "a node without matching sockets is refused and the link stays");
}

void testInsertIntoDriverLink(TestContext& test) {
    Fixture fixture;
    const auto value = addNode(fixture, kScalarValueNodeType, {0, 0});
    const auto blur = addNode(fixture, "bloom.gaussian-blur", {800, 0});
    const InputPortRef radius = NodeInputRef{blur, "radius"};
    test.expect(apply<ConnectPorts>(fixture, OutputPortRef{value, "value"}, radius).changed(),
                "the value drives the radius");
    const auto inserted = apply<InsertNodeOnLink>(fixture, std::string(kScalarMathNodeType),
                                                  radius, Vec2d{400, 0});
    const auto node = inserted.outputId<NodeId>("editorNode");
    test.expect(inserted.changed() && node.has_value(), "a math node goes into the driver link");
    if (!node)
        return;
    const auto driver = source(fixture, radius);
    test.expect(driver.has_value() && driver->nodeId == *node,
                "the math node now drives the radius");
}

} // namespace
} // namespace bloom::commands::test

int main() {
    bloom::commands::test::TestContext test;
    try {
        bloom::commands::test::testSocketMatching(test);
        bloom::commands::test::testInsertIsOneUndoStep(test);
        bloom::commands::test::testInsertIntoDriverLink(test);
    } catch (const std::exception& error) {
        test.expect(false, std::string("unexpected exception: ") + error.what());
    }
    if (test.failures() != 0) {
        std::cerr << test.failures() << " node link insertion check(s) failed\n";
        return 1;
    }
    return 0;
}
