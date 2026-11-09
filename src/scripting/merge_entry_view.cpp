#include <bloom/scripting/merge_entry_view.hpp>

#include <bloom/document/merge_entries.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_nodes.hpp>

#include <algorithm>
#include <unordered_set>

namespace bloom::scripting {
namespace {

using document::CanonicalGraph;
using document::NodeId;

const document::EdgeRecord* edgeInto(const CanonicalGraph& graph, const NodeId node,
                                     const std::string_view port) {
    for (const auto& edge : graph.edges())
        if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            input != nullptr && input->nodeId == node && input->port == port)
            return &edge;
    return nullptr;
}

// The Merge an entry's image comes from, walking up through Layer nodes, reroutes and image
// filters (their first Image input), as the Timeline groups a nested Merge.
std::optional<NodeId> nestedMergeOf(const CanonicalGraph& graph, NodeId current) {
    const auto& registry = document::builtInNodeDefinitions();
    std::unordered_set<NodeId> visited;
    while (visited.insert(current).second) {
        const auto* node = graph.findNode(current);
        if (node == nullptr)
            return std::nullopt;
        if (node->typeId == document::kLayerStackNodeType)
            return current;
        std::optional<std::string> input;
        if (document::isRerouteNodeType(node->typeId)) {
            input = std::string(document::kValuePortName);
        } else if (node->typeId == document::kLayerOutputNodeType) {
            input = std::string(document::kLayerOutputContentInputPort);
        } else if (const auto* definition = registry.find(node->typeId, node->schemaVersion)) {
            const bool image = std::ranges::any_of(definition->outputs, [](const auto& output) {
                return output.valueKind == document::SocketValueKind::Image;
            });
            if (image)
                for (const auto& port : definition->inputs)
                    if (port.valueKind == document::SocketValueKind::Image) {
                        input = port.name;
                        break;
                    }
        }
        if (!input)
            return std::nullopt;
        const auto* edge = edgeInto(graph, current, *input);
        if (edge == nullptr)
            return std::nullopt;
        current = edge->source.nodeId;
    }
    return std::nullopt;
}

std::optional<document::ParameterValue> entryDefault(const document::Composition& composition,
                                                     const std::string_view role) {
    if (role == document::kPositionParameterRole)
        return document::Vec2d{static_cast<double>(composition.format().width()) / 2.0,
                               static_cast<double>(composition.format().height()) / 2.0};
    const auto* definition = document::builtInNodeDefinitions().find(
        document::kLayerOutputNodeType, document::kLayerOutputNodeSchemaVersion);
    if (definition != nullptr)
        for (const auto& parameter : definition->parameters)
            if (parameter.role == role)
                return parameter.defaultValue;
    return std::nullopt;
}

MergeEntryView viewOf(const document::Composition& composition, const document::LayerStack& merge,
                      const std::size_t index, const bool outputMerge) {
    const auto& graph = composition.graph();
    const auto slot = merge.entries()[index].slotId;
    MergeEntryView view;
    view.merge = merge.nodeId();
    view.slot = slot;
    view.index = index;
    view.outputMerge = outputMerge;
    if (const auto* edge = document::mergeSlotImageEdge(graph, merge.nodeId(), slot)) {
        view.sourceNode = edge->source.nodeId;
        if (const auto* node = graph.findNode(edge->source.nodeId))
            view.sourceType = node->typeId;
        view.nestedMerge = nestedMergeOf(graph, edge->source.nodeId);
    }
    view.layer = document::mergeEntryLayer(graph, merge.nodeId(), slot);
    if (view.layer != nullptr) {
        view.kind = view.layer->entryOwned() ? EntryLayerKind::Entry : EntryLayerKind::Node;
        if (!view.layer->entryOwned())
            view.layerNode = view.layer->nodeId;
        view.pivot = view.layer->pivot;
        // An entry-owned layer belongs to exactly its own slot; only a Layer node can be shared, so
        // an imported document with thousands of entry layers is not scanned once per entry.
        view.shared = !view.layer->entryOwned() &&
                      document::mergeEntriesOfLayer(graph, view.layer->layerId).size() > 1;
    }
    view.adjustment = document::mergeEntryAdjustment(graph, merge.nodeId(), slot);
    view.roles = layerRoleValues(composition, view.layer);
    return view;
}

} // namespace

std::string_view entryLayerKindName(const EntryLayerKind kind) noexcept {
    switch (kind) {
    case EntryLayerKind::Node:
        return "node";
    case EntryLayerKind::Entry:
        return "entry";
    case EntryLayerKind::None:
        break;
    }
    return "none";
}

std::string_view layerPivotName(const document::LayerPivot pivot) noexcept {
    return pivot == document::LayerPivot::Frame ? "frame" : "content";
}

std::vector<LayerRoleValue> layerRoleValues(const document::Composition& composition,
                                            const document::LayerOutputBoundary* layer) {
    std::vector<LayerRoleValue> values;
    for (const auto& entry : document::entryLayerRoles()) {
        if (layer == nullptr) {
            if (entry.role == document::kTimeRemapParameterRole)
                continue;
            values.push_back({.role = entry.role,
                              .parameter = std::nullopt,
                              .value = entryDefault(composition, entry.role)});
            continue;
        }
        const auto* binding = document::layerParameterBinding(composition.graph(), *layer,
                                                               entry.role);
        if (binding == nullptr)
            continue;
        LayerRoleValue value{.role = entry.role, .parameter = binding->parameterId, .value = {}};
        if (const auto* record = composition.parameters().find(binding->parameterId))
            if (const auto* constant = std::get_if<document::ConstantValueSource>(&record->source))
                value.value = constant->value;
        values.push_back(std::move(value));
    }
    return values;
}

std::vector<MergeEntryView> mergeEntryViews(const document::Composition& composition) {
    const auto& graph = composition.graph();
    const auto output = graph.outputMergeId();
    std::vector<const document::LayerStack*> merges;
    for (const auto& merge : graph.merges())
        merges.push_back(&merge);
    std::ranges::stable_partition(merges, [&](const document::LayerStack* merge) {
        return output && merge->nodeId() == *output;
    });
    std::vector<MergeEntryView> views;
    for (const auto* merge : merges)
        for (std::size_t index = 0; index < merge->entries().size(); ++index)
            views.push_back(
                viewOf(composition, *merge, index, output && merge->nodeId() == *output));
    return views;
}

std::optional<MergeEntryView> mergeEntryView(const document::Composition& composition,
                                             const NodeId merge, const document::LayerSlotId slot) {
    const auto& graph = composition.graph();
    const auto* stack = graph.merge(merge);
    if (stack == nullptr)
        return std::nullopt;
    const auto entries = stack->entries();
    for (std::size_t index = 0; index < entries.size(); ++index)
        if (entries[index].slotId == slot)
            return viewOf(composition, *stack, index, graph.outputMergeId() == merge);
    return std::nullopt;
}

} // namespace bloom::scripting
