#include <bloom/document/drill_levels.hpp>

#include <bloom/document/merge_entries.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace bloom::document {
namespace {

// A small group is the importer's fill-and-stroke group: a handful of entries. Anything larger is
// treated as an ordinary group without looking further.
constexpr std::size_t kSmallGroupLeafLimit = 8;

struct ChainEnd final {
    std::optional<NodeId> merge;
    std::optional<NodeId> source;
};

[[nodiscard]] const EdgeRecord* edgeInto(const CanonicalGraph& graph, const NodeId node,
                                         const std::string_view port) {
    for (const auto& edge : graph.edges())
        if (const auto* input = std::get_if<NodeInputRef>(&edge.destination);
            input != nullptr && input->nodeId == node && input->port == port)
            return &edge;
    return nullptr;
}

// Walks up an image chain through Layer nodes, image effects' first image input and reroutes, to
// the Merge or the source it starts from.
[[nodiscard]] ChainEnd walkImageChain(const CanonicalGraph& graph, OutputPortRef current,
                                      const NodeDefinitionRegistry& registry) {
    std::unordered_set<NodeId> visited;
    while (visited.insert(current.nodeId).second) {
        const auto* node = graph.findNode(current.nodeId);
        if (node == nullptr)
            return {};
        std::optional<std::string> input;
        if (isRerouteNodeType(node->typeId)) {
            input = std::string(kValuePortName);
        } else {
            const auto* definition = registry.find(node->typeId, node->schemaVersion);
            if (definition == nullptr)
                return {};
            if (definition->lowering == NodeLoweringKind::LayerStack)
                return {node->id, std::nullopt};
            if (definition->lowering == NodeLoweringKind::CompositionOutput)
                return {};
            for (const auto& declared : definition->inputs)
                if (declared.valueKind == SocketValueKind::Image) {
                    input = declared.name;
                    break;
                }
            if (!input)
                return {std::nullopt, node->id};
        }
        const auto* edge = edgeInto(graph, node->id, *input);
        if (edge == nullptr)
            return {};
        current = edge->source;
    }
    return {};
}

[[nodiscard]] ChainEnd entryChainEnd(const CanonicalGraph& graph, const NodeId merge,
                                     const LayerSlotId slot,
                                     const NodeDefinitionRegistry& registry) {
    const auto* edge = mergeSlotImageEdge(graph, merge, slot);
    return edge != nullptr ? walkImageChain(graph, edge->source, registry) : ChainEnd{};
}

[[nodiscard]] ChainEnd layerChainEnd(const Composition& composition, const LayerId layer,
                                     const NodeDefinitionRegistry& registry) {
    const auto& graph = composition.graph();
    if (graph.findLayer(layer) == nullptr)
        return {};
    const auto entries = mergeEntriesOfLayer(graph, layer, registry);
    if (entries.empty())
        return {};
    return entryChainEnd(graph, entries.front().merge, entries.front().slot, registry);
}

[[nodiscard]] const ParameterRecord*
boundRecord(const Composition& composition, const NodeRecord& node, const std::string_view role) {
    const auto found = std::ranges::find(node.parameters, role, &ParameterBinding::role);
    return found == node.parameters.end() ? nullptr
                                          : composition.parameters().find(found->parameterId);
}

[[nodiscard]] bool isPathKindShape(const Composition& composition, const NodeRecord& node) {
    if (node.typeId != kShapeSourceNodeType)
        return false;
    const auto* kind = boundRecord(composition, node, "kind");
    const auto* constant = kind ? std::get_if<ConstantValueSource>(&kind->source) : nullptr;
    const auto* value = constant ? std::get_if<std::int64_t>(&constant->value) : nullptr;
    return value != nullptr && *value == static_cast<std::int64_t>(ShapeKind::Path);
}

// The Path node a Path-kind Shape's outline comes from: directly (its `path` is driven by the Path
// node) or through a Path Stroke node whose path is.
struct PathReading final {
    std::optional<NodeId> pathNode;
    bool direct = false;
    // The Shape's `path` holds its own value (no driver).
    bool own = false;
};

[[nodiscard]] PathReading shapePathReading(const Composition& composition,
                                           const NodeRecord& shape) {
    const auto* path = boundRecord(composition, shape, "path");
    if (path == nullptr)
        return {};
    const auto* driver = std::get_if<DriverBindingSource>(&path->source);
    if (driver == nullptr)
        return {std::nullopt, false, true};
    const auto& graph = composition.graph();
    const auto* source = graph.findNode(driver->sourceNodeId);
    if (source == nullptr)
        return {};
    if (source->typeId == kPathValueNodeType && driver->outputPort == kValuePortName)
        return {source->id, true, false};
    if (source->typeId != kPathStrokeNodeType)
        return {};
    for (const auto& binding : source->parameters) {
        const auto* record = composition.parameters().find(binding.parameterId);
        const auto* inner = record ? std::get_if<DriverBindingSource>(&record->source) : nullptr;
        const auto* innerNode = inner ? graph.findNode(inner->sourceNodeId) : nullptr;
        if (innerNode != nullptr && innerNode->typeId == kPathValueNodeType)
            return {innerNode->id, false, false};
    }
    return {};
}

// Every entry layer under `merge`, recursively, with its chain end: the leaves of a small group.
struct Leaf final {
    LayerId layer;
    ChainEnd end;
};
bool collectLeaves(const Composition& composition, const NodeId merge, std::vector<Leaf>& leaves,
                   std::unordered_set<NodeId>& seen, const NodeDefinitionRegistry& registry) {
    if (!seen.insert(merge).second)
        return false;
    const auto& graph = composition.graph();
    const auto* stack = graph.merge(merge);
    if (stack == nullptr)
        return false;
    for (const auto& entry : stack->entries()) {
        const auto* layer = mergeEntryLayer(graph, merge, entry.slotId, registry);
        if (layer == nullptr)
            return false;
        const auto end = entryChainEnd(graph, merge, entry.slotId, registry);
        // The importer writes a shape's fill and stroke parts side by side; a group inside makes
        // it an ordinary group.
        if (end.merge)
            return false;
        leaves.push_back({layer->layerId, end});
        if (leaves.size() > kSmallGroupLeafLimit)
            return false;
    }
    return true;
}

} // namespace

std::optional<NodeId> layerContentMerge(const Composition& composition, const LayerId layer,
                                        const NodeDefinitionRegistry& registry) {
    return layerChainEnd(composition, layer, registry).merge;
}

std::optional<NodeId> layerContentSource(const Composition& composition, const LayerId layer,
                                         const NodeDefinitionRegistry& registry) {
    return layerChainEnd(composition, layer, registry).source;
}

std::optional<NodeId> drillLevelMerge(const Composition& composition,
                                      const std::span<const LayerId> chain,
                                      const NodeDefinitionRegistry& registry) {
    if (chain.empty())
        return composition.graph().outputMergeId();
    if (validDrillPrefix(composition, chain, registry) != chain.size())
        return std::nullopt;
    return layerContentMerge(composition, chain.back(), registry);
}

bool isTrackMatteSource(const Composition& composition, const NodeId merge, const LayerId layer,
                        const NodeDefinitionRegistry& registry) {
    for (const auto other : mergeEntryLayers(composition, merge, registry)) {
        const auto* boundary = composition.graph().findLayer(other);
        if (other != layer && boundary != nullptr && boundary->trackMatte &&
            boundary->trackMatte->source == layer)
            return true;
    }
    return false;
}

std::vector<DrillEntry> mergeEntryContents(const Composition& composition, const NodeId merge,
                                           const NodeDefinitionRegistry& registry) {
    std::vector<DrillEntry> entries;
    const auto& graph = composition.graph();
    const auto* stack = graph.merge(merge);
    if (stack == nullptr)
        return entries;
    for (const auto& entry : stack->entries()) {
        const auto* layer = mergeEntryLayer(graph, merge, entry.slotId, registry);
        if (layer == nullptr ||
            std::ranges::find(entries, layer->layerId, &DrillEntry::layer) != entries.end())
            continue;
        const auto end = entryChainEnd(graph, merge, entry.slotId, registry);
        entries.push_back({layer->layerId, end.merge, end.source, false});
    }
    for (auto& entry : entries)
        for (const auto& other : entries)
            if (const auto* boundary = graph.findLayer(other.layer);
                other.layer != entry.layer && boundary != nullptr && boundary->trackMatte &&
                boundary->trackMatte->source == entry.layer)
                entry.matteSource = true;
    return entries;
}

std::vector<LayerId> mergeEntryLayers(const Composition& composition, const NodeId merge,
                                      const NodeDefinitionRegistry& registry) {
    std::vector<LayerId> layers;
    const auto& graph = composition.graph();
    const auto* stack = graph.merge(merge);
    if (stack == nullptr)
        return layers;
    for (const auto& entry : stack->entries())
        if (const auto* layer = mergeEntryLayer(graph, merge, entry.slotId, registry);
            layer != nullptr && std::ranges::find(layers, layer->layerId) == layers.end())
            layers.push_back(layer->layerId);
    return layers;
}

std::vector<LayerId> drillChildren(const Composition& composition, const NodeId merge,
                                   const NodeDefinitionRegistry& registry) {
    auto layers = mergeEntryLayers(composition, merge, registry);
    std::unordered_set<LayerId> mattes;
    for (const auto layer : layers)
        if (const auto* boundary = composition.graph().findLayer(layer);
            boundary != nullptr && boundary->trackMatte && boundary->trackMatte->source.isValid())
            mattes.insert(boundary->trackMatte->source);
    std::erase_if(layers, [&](const LayerId layer) { return mattes.contains(layer); });
    return layers;
}

std::optional<std::vector<LayerId>> drillAncestors(const Composition& composition,
                                                   const LayerId layer,
                                                   const NodeDefinitionRegistry& registry) {
    const auto& graph = composition.graph();
    const auto top = graph.outputMergeId();
    if (!top || graph.findLayer(layer) == nullptr)
        return std::nullopt;
    // One pass over every Merge: which Merge each layer is an entry of, and which group layer
    // shows each Merge.
    std::unordered_map<LayerId, NodeId> entryMerge;
    std::unordered_map<NodeId, LayerId> groupOf;
    for (const auto& stack : graph.merges()) {
        for (const auto& entry : stack.entries()) {
            const auto* found = mergeEntryLayer(graph, stack.nodeId(), entry.slotId, registry);
            if (found == nullptr)
                continue;
            entryMerge.try_emplace(found->layerId, stack.nodeId());
            if (const auto end = entryChainEnd(graph, stack.nodeId(), entry.slotId, registry);
                end.merge)
                groupOf.try_emplace(*end.merge, found->layerId);
        }
    }
    std::vector<LayerId> chain;
    auto current = entryMerge.find(layer);
    if (current == entryMerge.end())
        return std::nullopt;
    auto merge = current->second;
    for (std::size_t guard = 0; merge != *top; ++guard) {
        const auto group = groupOf.find(merge);
        if (group == groupOf.end() || guard > graph.merges().size())
            return std::nullopt;
        chain.push_back(group->second);
        current = entryMerge.find(group->second);
        if (current == entryMerge.end())
            return std::nullopt;
        merge = current->second;
    }
    std::ranges::reverse(chain);
    return chain;
}

std::optional<DrillShape> drillShape(const Composition& composition, const LayerId layer,
                                     const NodeDefinitionRegistry& registry) {
    const auto& graph = composition.graph();
    const auto end = layerChainEnd(composition, layer, registry);
    if (end.source) {
        const auto* node = graph.findNode(*end.source);
        if (node == nullptr || !isPathKindShape(composition, *node))
            return std::nullopt;
        const auto reading = shapePathReading(composition, *node);
        if (reading.own)
            return DrillShape{layer, std::nullopt, false};
        if (reading.pathNode && reading.direct)
            return DrillShape{layer, reading.pathNode, false};
        return std::nullopt;
    }
    if (!end.merge)
        return std::nullopt;
    std::vector<Leaf> leaves;
    std::unordered_set<NodeId> seen;
    // At least two parts (fill and stroke, or a gradient and the outline it is matted by): a group
    // holding one shape is an ordinary group around it.
    if (!collectLeaves(composition, *end.merge, leaves, seen, registry) || leaves.size() < 2)
        return std::nullopt;
    std::optional<NodeId> pathNode;
    std::optional<LayerId> edited;
    bool editedIsMatte = false;
    std::size_t shapes = 0;
    for (const auto& leaf : leaves) {
        const auto* node = leaf.end.source ? graph.findNode(*leaf.end.source) : nullptr;
        if (node == nullptr)
            return std::nullopt;
        if (!isPathKindShape(composition, *node)) {
            // A gradient part draws through a matte that holds the outline.
            const auto* boundary = graph.findLayer(leaf.layer);
            if (boundary == nullptr || !boundary->trackMatte)
                return std::nullopt;
            continue;
        }
        const auto reading = shapePathReading(composition, *node);
        if (!reading.pathNode || (pathNode && *pathNode != *reading.pathNode))
            return std::nullopt;
        pathNode = reading.pathNode;
        ++shapes;
        if (!reading.direct)
            continue;
        // Prefer the visible fill to a matte that also reads the path.
        bool matte = false;
        for (const auto& other : leaves)
            if (const auto* boundary = graph.findLayer(other.layer);
                boundary != nullptr && boundary->trackMatte &&
                boundary->trackMatte->source == leaf.layer)
                matte = true;
        if (!edited || (editedIsMatte && !matte)) {
            edited = leaf.layer;
            editedIsMatte = matte;
        }
    }
    if (shapes == 0 || !edited)
        return std::nullopt;
    return DrillShape{*edited, pathNode, true};
}

std::optional<LayerId> shapeLayerOfPathNode(const Composition& composition, const NodeId pathNode,
                                            const NodeDefinitionRegistry& registry) {
    const auto& graph = composition.graph();
    for (const auto& stack : graph.merges())
        for (const auto& entry : stack.entries()) {
            const auto* layer = mergeEntryLayer(graph, stack.nodeId(), entry.slotId, registry);
            const auto end =
                layer ? entryChainEnd(graph, stack.nodeId(), entry.slotId, registry) : ChainEnd{};
            const auto* node = end.source ? graph.findNode(*end.source) : nullptr;
            if (node == nullptr || !isPathKindShape(composition, *node))
                continue;
            const auto reading = shapePathReading(composition, *node);
            if (reading.direct && reading.pathNode == pathNode)
                return layer->layerId;
        }
    return std::nullopt;
}

bool isDrillGroup(const Composition& composition, const LayerId layer,
                  const NodeDefinitionRegistry& registry) {
    if (!layerContentMerge(composition, layer, registry))
        return false;
    const auto shape = drillShape(composition, layer, registry);
    return !shape || !shape->smallGroup;
}

DrillLevelCheck checkDrillLevel(const Composition& composition,
                                const std::span<const LayerId> chain,
                                const std::optional<LayerId> points,
                                const NodeDefinitionRegistry& registry) {
    const auto& graph = composition.graph();
    auto merge = graph.outputMergeId();
    if (!merge)
        return {DrillLevelRefusal::NotAChain, chain.empty() ? LayerId{} : chain.front()};
    for (const auto layer : chain) {
        if (graph.findLayer(layer) == nullptr)
            return {DrillLevelRefusal::UnknownLayer, layer};
        if (const auto children = drillChildren(composition, *merge, registry);
            std::ranges::find(children, layer) == children.end())
            return {DrillLevelRefusal::NotAChain, layer};
        if (!isDrillGroup(composition, layer, registry))
            return {DrillLevelRefusal::NotAGroup, layer};
        merge = layerContentMerge(composition, layer, registry);
        if (!merge)
            return {DrillLevelRefusal::NotAGroup, layer};
    }
    if (points) {
        if (graph.findLayer(*points) == nullptr)
            return {DrillLevelRefusal::UnknownLayer, *points};
        const auto children = drillChildren(composition, *merge, registry);
        if (std::ranges::find(children, *points) == children.end())
            return {DrillLevelRefusal::NotAShape, *points};
        if (!drillShape(composition, *points, registry))
            return {DrillLevelRefusal::NotAShape, *points};
    }
    return {};
}

std::vector<LayerId> drillIsolationDimmed(const Composition& composition,
                                          const std::span<const LayerId> chain,
                                          const NodeDefinitionRegistry& registry) {
    std::vector<LayerId> dimmed;
    if (chain.empty() || validDrillPrefix(composition, chain, registry) != chain.size())
        return dimmed;
    auto merge = composition.graph().outputMergeId();
    for (const auto entered : chain) {
        if (!merge)
            return {};
        for (const auto child : drillChildren(composition, *merge, registry))
            if (child != entered)
                dimmed.push_back(child);
        merge = layerContentMerge(composition, entered, registry);
    }
    return dimmed;
}

std::size_t validDrillPrefix(const Composition& composition, const std::span<const LayerId> chain,
                             const NodeDefinitionRegistry& registry) {
    for (std::size_t length = chain.size(); length > 0; --length)
        if (checkDrillLevel(composition, chain.first(length), std::nullopt, registry).ok())
            return length;
    return 0;
}

std::string drillLevelRefusalText(const Composition& composition, const DrillLevelCheck& check) {
    const auto* boundary = composition.graph().findLayer(check.layer);
    const auto name = boundary != nullptr && !boundary->name.empty()
                          ? "'" + boundary->name + "'"
                          : "layer " + std::to_string(check.layer.value());
    switch (check.refusal) {
    case DrillLevelRefusal::None:
        return {};
    case DrillLevelRefusal::UnknownLayer:
        return "No layer " + std::to_string(check.layer.value()) +
               " in this composition (query kind=layers lists them)";
    case DrillLevelRefusal::NotAGroup:
        return name + " is not a group, so the Viewer cannot enter it";
    case DrillLevelRefusal::NotAChain:
        return name +
               " is not a child of the level before it; list the groups from the top, outermost "
               "first";
    case DrillLevelRefusal::NotAShape:
        return name + " is not a shape with editable points among the entered group's children";
    }
    return {};
}

} // namespace bloom::document
