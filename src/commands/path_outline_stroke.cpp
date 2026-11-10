#include <bloom/commands/path_outline_stroke.hpp>

#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_layout_placement.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_utility_nodes.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace bloom::commands {
namespace {

using document::CanonicalGraph;
using document::LayerId;
using document::LayerOutputBoundary;
using document::NodeId;
using document::Vec2d;

template <class Id>
[[nodiscard]] std::optional<Id> outputOf(const OperationResult& result,
                                         const std::string_view name) {
    if (result.status != OperationStatus::Applied && result.status != OperationStatus::NoChange)
        return std::nullopt;
    for (const auto& output : result.outputs)
        if (output.name == name)
            if (const auto* id = std::get_if<Id>(&output.id))
                return *id;
    return std::nullopt;
}

[[nodiscard]] bool applied(const OperationResult& result) {
    return result.status == OperationStatus::Applied || result.status == OperationStatus::NoChange;
}

[[nodiscard]] const document::EdgeRecord* edgeInto(const CanonicalGraph& graph, const NodeId node,
                                                   const std::string_view port) {
    for (const auto& edge : graph.edges())
        if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            input != nullptr && input->nodeId == node && input->port == port)
            return &edge;
    return nullptr;
}

// The node whose image a layer shows: what feeds its Layer node's content, or its Merge entry.
[[nodiscard]] std::optional<NodeId> layerContentNode(const CanonicalGraph& graph,
                                                     const LayerOutputBoundary& layer) {
    if (layer.entryOwned()) {
        const auto* edge = document::mergeSlotImageEdge(graph, layer.nodeId, *layer.entrySlot);
        return edge != nullptr ? std::optional(edge->source.nodeId) : std::nullopt;
    }
    const auto* edge = edgeInto(graph, layer.nodeId, document::kLayerOutputContentInputPort);
    return edge != nullptr ? std::optional(edge->source.nodeId) : std::nullopt;
}

// The Text Source a layer shows: its content, or the first Image input of each image effect above
// it, walked up to the text. Anything else (a Merge, another source) has no single text.
[[nodiscard]] const document::NodeRecord* layerText(const CanonicalGraph& graph,
                                                    const LayerOutputBoundary& layer) {
    auto current = layerContentNode(graph, layer);
    std::unordered_set<NodeId> visited;
    while (current && visited.insert(*current).second) {
        const auto* node = graph.findNode(*current);
        if (node == nullptr)
            return nullptr;
        if (node->typeId == document::kTextSourceNodeType)
            return node;
        const auto* definition =
            document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
        if (definition == nullptr ||
            !std::ranges::any_of(definition->outputs, [](const auto& output) {
                return output.valueKind == document::SocketValueKind::Image;
            }))
            return nullptr;
        const auto input = std::ranges::find(definition->inputs, document::SocketValueKind::Image,
                                             &document::InputPortDefinition::valueKind);
        if (input == definition->inputs.end())
            return nullptr;
        const auto* edge = edgeInto(graph, node->id, input->name);
        current = edge != nullptr ? std::optional(edge->source.nodeId) : std::nullopt;
    }
    return nullptr;
}

[[nodiscard]] bool hasPathMask(const LayerOutputBoundary& layer) {
    return std::ranges::any_of(layer.masks,
                               [](const document::LayerMask& mask) { return !mask.matte; });
}

} // namespace

bool canStrokeLayerOutline(const document::Composition& composition, const LayerId layer,
                           const OutlineStrokeSource source) {
    const auto* boundary = composition.graph().findLayer(layer);
    if (boundary == nullptr || document::mergeEntriesOfLayer(composition.graph(), layer).empty())
        return false;
    return source == OutlineStrokeSource::Text ? layerText(composition.graph(), *boundary) != nullptr
                                               : hasPathMask(*boundary);
}

std::string_view StrokeLayerOutline::typeId() const noexcept {
    return "bloom.path.stroke-layer-outline";
}

OperationResult StrokeLayerOutline::apply(document::Draft& draft) const {
    using namespace document;
    const auto* readable = std::as_const(draft).project().findComposition(composition_);
    if (readable == nullptr)
        return OperationResult::rejected(OperationIssueCode::MissingReference,
                                         "The composition does not exist");
    const auto& graph = readable->graph();
    const auto* boundary = graph.findLayer(layer_);
    if (boundary == nullptr)
        return OperationResult::rejected(OperationIssueCode::MissingReference,
                                         "The layer to stroke does not exist");
    const auto entries = mergeEntriesOfLayer(graph, layer_);
    if (entries.empty())
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "The layer is not an entry of any Merge");
    const auto entry = entries.front();
    const auto inPoint = boundary->inPoint;
    const auto outPoint = boundary->outPoint;
    const auto layerName = boundary->name;
    const bool entryOwned = boundary->entryOwned();
    const auto layerNode = boundary->nodeId;

    std::optional<NodeId> sourceNode;
    if (source_ == OutlineStrokeSource::Text) {
        if (const auto* text = layerText(graph, *boundary))
            sourceNode = text->id;
        else
            return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                             "The layer shows no text to outline");
    } else if (!hasPathMask(*boundary)) {
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "The layer has no path mask to stroke");
    }

    std::optional<OperationResult> refusal;
    const auto check = [&](OperationResult result) {
        if (!refusal && !applied(result))
            refusal = std::move(result);
        return !refusal.has_value();
    };
    if (source_ == OutlineStrokeSource::Masks) {
        // Mask Path reads a Layer node's masks. A Merge entry's own layer gets one, which takes its
        // values (masks included) in the same step, so the picture does not change.
        if (entryOwned) {
            auto inserted = InsertLayerNode(composition_, entry.merge, entry.slot).apply(draft);
            sourceNode = outputOf<NodeId>(inserted, kEntryLayerNodeOutput);
            if (!check(std::move(inserted)) || !sourceNode)
                return refusal.value_or(OperationResult::rejected(
                    OperationIssueCode::InvalidValue, "The layer's masks could not be read"));
        } else {
            sourceNode = layerNode;
        }
    }

    // The new nodes sit left of the source, in a row.
    const auto* current = std::as_const(draft).project().findComposition(composition_);
    const auto origin = effectiveNodePosition(*current, *sourceNode);
    const auto add = [&](const std::string_view type, const Vec2d position) -> NodeId {
        auto result = AddNode(composition_, std::string(type), position).apply(draft);
        const auto id = outputOf<NodeId>(result, kAddNodeOutput);
        check(std::move(result));
        return id.value_or(NodeId{});
    };
    const auto set = [&](const NodeId node, const std::string_view role, ParameterValue value) {
        if (!refusal)
            check(SetNodeParameterValue(composition_, node, std::string(role), std::move(value))
                      .apply(draft));
    };
    const auto connect = [&](const NodeId from, const std::string_view output, const NodeId to,
                             const std::string_view input) {
        if (!refusal)
            check(ConnectPorts(composition_, OutputPortRef{from, std::string(output)},
                               NodeInputRef{to, std::string(input)})
                      .apply(draft));
    };
    const auto reader = add(source_ == OutlineStrokeSource::Text ? kTextOutlineNodeType
                                                                 : kMaskPathNodeType,
                            Vec2d{origin.x, origin.y + 260.0});
    const auto stroke = add(kPathStrokeNodeType, Vec2d{origin.x + 240.0, origin.y + 260.0});
    const auto shape = add(kShapeSourceNodeType, Vec2d{origin.x + 480.0, origin.y + 260.0});
    if (refusal)
        return *refusal;
    set(reader, kPathSourceParameterRole, static_cast<std::int64_t>(sourceNode->value()));
    set(shape, "kind", static_cast<std::int64_t>(ShapeKind::Path));
    set(shape, "fillColor", core::Color4d{1.0, 1.0, 1.0, 1.0});
    connect(reader, "path", stroke, "path");
    connect(stroke, "path", shape, "path");
    if (refusal)
        return *refusal;

    // A new entry directly above the source's, with its own layer at the identity, parented to the
    // source layer so it draws in the source layer's own space.
    auto entered = ConnectPorts(composition_, OutputPortRef{shape, std::string("image")},
                                LayerStackInputRef{entry.merge, {},
                                                   std::string(kLayerStackContentInputRole)},
                                builtInNodeDefinitions(), entry.slot)
                       .apply(draft);
    const auto slot = outputOf<LayerSlotId>(entered, kConnectPortsSlotOutput);
    if (!check(std::move(entered)) || !slot)
        return refusal.value_or(OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                          "The stroke could not enter the Merge"));
    auto ensured = EnsureEntryLayer(composition_, entry.merge, *slot).apply(draft);
    const auto layer = outputOf<LayerId>(ensured, kEntryLayerOutput);
    if (!check(std::move(ensured)) || !layer)
        return refusal.value_or(OperationResult::rejected(OperationIssueCode::InvalidValue,
                                                          "The stroke's layer could not be made"));
    if (!check(SetLayerParent(composition_, *layer, layer_).apply(draft)))
        return *refusal;
    auto* composition = draft.project().findComposition(composition_);
    auto* made = composition != nullptr ? composition->graph().findLayer(*layer) : nullptr;
    if (made == nullptr || made->pivot != LayerPivot::Frame)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The stroke's layer is not at the identity");
    made->name = layerName + (source_ == OutlineStrokeSource::Text ? " Outline" : " Mask Stroke");
    made->nameFollowsSource = false;
    made->inPoint = inPoint;
    made->outPoint = outPoint;
    return OperationResult::applied({{std::string(kOutlineStrokeReaderOutput), reader},
                                     {std::string(kOutlineStrokeStrokeOutput), stroke},
                                     {std::string(kOutlineStrokeShapeOutput), shape},
                                     {std::string(kOutlineStrokeLayerOutput), *layer}});
}

} // namespace bloom::commands
