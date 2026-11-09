#pragma once

// #342 Reusable Group Input, Group Output and Node Group, and #590 shared function definitions.
//
// A GROUP DEFINITION is a composition that holds a Group Output node. Its graph is authored with
// the ordinary node commands, so editing it is one undo step like any other edit, and every
// Node Group node that names it reads the same graph. Nothing about a definition is stored beside
// the nodes: the marker is the Group Output node, so no document schema moves.
//
// A Node Group node (the INSTANCE) names a definition by composition id and carries the values
// the definition's Group Input reads. The slots are a fixed catalogue of typed sockets, below. The
// compiler expands an instance in place before it lowers anything (`expandNodeGroups`), so the
// instance's nodes are ordinary nodes on every route: the CPU reference, the GPU route and every
// export see the same plan, and there is no second evaluator to keep in parity.
//
// A group that nests itself, directly or through other groups, is a cycle. It is refused at edit
// time through the same nesting check Composition Source uses.

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bloom::document {

inline constexpr std::string_view kGroupInputNodeType = "bloom.group-input";
inline constexpr std::string_view kGroupOutputNodeType = "bloom.group-output";
inline constexpr std::string_view kNodeGroupNodeType = "bloom.node-group";
// #590: a node that evaluates a group as a function without an instance, so one definition serves
// one value (Function) or many (Function Plot).
inline constexpr std::string_view kFunctionNodeType = "bloom.function";
inline constexpr std::string_view kFunctionArgumentRole = "x";
inline constexpr std::string_view kFunctionResultPort = "y";
// #595: the same function sampled over a range of arguments into a Path value.
inline constexpr std::string_view kFunctionPlotNodeType = "bloom.function-plot";
inline constexpr std::string_view kFunctionPlotStartRole = "start";
inline constexpr std::string_view kFunctionPlotEndRole = "end";
inline constexpr std::string_view kFunctionPlotSamplesRole = "samples";
inline constexpr std::string_view kFunctionPlotOriginRole = "origin";
inline constexpr std::string_view kFunctionPlotScaleRole = "scale";
inline constexpr std::string_view kFunctionPlotSmoothRole = "smooth";
inline constexpr std::string_view kFunctionPlotPathPort = "path";
inline constexpr std::int64_t kMinimumFunctionPlotSamples = 2;
// One contour holds at most kMaximumPathAnchors, so a plot of this many samples is one contour.
inline constexpr std::int64_t kMaximumFunctionPlotSamples = 4096;
inline constexpr std::int64_t kDefaultFunctionPlotSamples = 256;
inline constexpr std::uint32_t kNodeGroupNodeSchemaVersion = 1;

// The parameter on an instance (and on Function Plot) that names the definition.
inline constexpr std::string_view kNodeGroupReferenceRole = "group";
inline constexpr std::string_view kNodeGroupReferenceSchemaKey = "bloom.node-group.group";

// One typed slot of the fixed catalogue. The same slot name means the same socket on Group Input
// (an output), Group Output (an input) and Node Group (an input and an output), which is how the
// expansion pairs them without a table of its own.
struct NodeGroupSlot final {
    std::string_view name;
    SocketValueKind socket;
    // Pixel slots (Image, Mask, ChannelSet) connect by edge and have no parameter. A value slot is
    // an operand: its parameter is the value when nothing drives it.
    bool pixel;
};

// Image x2, Mask, ChannelSet, then Scalar x4, Vector 2, Color, Integer, Boolean. The scalar
// slots are the numbers a function takes and gives: scalar1 is the function's `x` in and `y` out.
inline constexpr std::array<NodeGroupSlot, 12> kNodeGroupSlots{{
    {"image1", SocketValueKind::Image, true},
    {"image2", SocketValueKind::Image, true},
    {"mask1", SocketValueKind::Mask, true},
    {"channels1", SocketValueKind::ChannelSet, true},
    {"scalar1", SocketValueKind::Scalar, false},
    {"scalar2", SocketValueKind::Scalar, false},
    {"scalar3", SocketValueKind::Scalar, false},
    {"scalar4", SocketValueKind::Scalar, false},
    {"vector1", SocketValueKind::Vector2, false},
    {"color1", SocketValueKind::Color, false},
    {"integer1", SocketValueKind::Integer, false},
    {"boolean1", SocketValueKind::Boolean, false},
}};

// The function interface #590 builds on: the slot a function reads its argument from, and the slot
// it answers on.
inline constexpr std::string_view kFunctionInputSlot = "scalar1";
inline constexpr std::string_view kFunctionOutputSlot = "scalar1";

[[nodiscard]] bool isNodeGroupBoundaryType(std::string_view typeId) noexcept;
// Whether the node type names a group in its `group` parameter: a Node Group, and the nodes that
// evaluate a group as a function.
[[nodiscard]] bool usesGroupReference(std::string_view typeId) noexcept;
// Whether the node type reads a group's function body rather than being replaced by the group.
[[nodiscard]] bool isFunctionConsumerType(std::string_view typeId) noexcept;

// The registered definitions.
[[nodiscard]] std::vector<NodeDefinition> nodeGroupNodeDefinitions();
// Whether `definition` is exactly the generated shape of its type (the registry's shape check).
[[nodiscard]] bool hasValidNodeGroupShape(const NodeDefinition& definition);

// The definition a node references: the constant value of its `group` parameter, or nothing when
// the node has none, or it is not constant, or it is not positive.
[[nodiscard]] std::optional<CompositionId> nodeGroupReference(const NodeRecord& node,
                                                              const ParameterStore& parameters);

// Whether the composition is a group definition: it holds a Group Output node.
[[nodiscard]] bool isNodeGroupDefinition(const Composition& composition) noexcept;
[[nodiscard]] const NodeRecord* findGroupInput(const Composition& composition) noexcept;
[[nodiscard]] const NodeRecord* findGroupOutput(const Composition& composition) noexcept;

// Where a function body's boundary nodes ended up in an expanded composition.
struct NodeGroupBody final {
    NodeId input;
    NodeId output;
    // Every node of the body that answers `scalar1`: the closure of what Group Output's scalar1
    // reads, Group Output included. The compiler lowers exactly these into the body's own program.
    std::vector<NodeId> nodes;
};

struct NodeGroupExpansion final {
    // The composition with every resolvable instance replaced by the nodes of its definition. Empty
    // when the composition holds no instance, so the caller keeps reading the original.
    std::optional<Composition> composition;
    // A function consumer's body, by the consumer's node id.
    std::unordered_map<NodeId, NodeGroupBody> functionBodies;
    // Why an instance stayed unexpanded: it names no group, or one that is missing or has no
    // Group Input and Group Output, or nesting went past the depth bound.
    std::unordered_map<NodeId, std::string> unresolved;
};

inline constexpr std::size_t kMaximumNodeGroupDepth = 8;
inline constexpr std::size_t kMaximumExpandedNodes = 20000;

[[nodiscard]] NodeGroupExpansion expandNodeGroups(const Project& project,
                                                  const Composition& composition);

} // namespace bloom::document
