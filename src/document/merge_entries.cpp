#include <bloom/document/merge_entries.hpp>

#include <bloom/document/value_nodes.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <string>
#include <unordered_set>

namespace bloom::document {
namespace {

constexpr std::array kEntryLayerRoles{
    EntryLayerRole{kPositionParameterRole, kPositionParameterSchemaKey},
    EntryLayerRole{kAnchorParameterRole, kAnchorParameterSchemaKey},
    EntryLayerRole{kScaleParameterRole, kScaleParameterSchemaKey},
    EntryLayerRole{kRotationParameterRole, kRotationParameterSchemaKey},
    EntryLayerRole{kOpacityParameterRole, kOpacityParameterSchemaKey},
    EntryLayerRole{kBlendModeParameterRole, kBlendModeParameterSchemaKey},
    EntryLayerRole{kTimeRemapParameterRole, kTimeRemapParameterSchemaKey},
};

[[nodiscard]] const EdgeRecord* edgeInto(const CanonicalGraph& graph, const NodeId node,
                                         const std::string_view port) {
    for (const auto& edge : graph.edges())
        if (const auto* input = std::get_if<NodeInputRef>(&edge.destination);
            input != nullptr && input->nodeId == node && input->port == port)
            return &edge;
    return nullptr;
}

// The input an image chain continues through: a reroute's pass-through, or the first Image input
// of a node with an Image output (an image effect's `input`). Sources, Merges, Layer nodes and
// sinks are not walked through here.
[[nodiscard]] std::optional<std::string> chainInput(const NodeRecord& node,
                                                    const NodeDefinitionRegistry& registry) {
    if (isRerouteNodeType(node.typeId))
        return std::string(kValuePortName);
    const auto* definition = registry.find(node.typeId, node.schemaVersion);
    if (definition == nullptr || definition->lowering == NodeLoweringKind::LayerStack ||
        definition->lowering == NodeLoweringKind::LayerOutput ||
        definition->lowering == NodeLoweringKind::CompositionOutput)
        return std::nullopt;
    const bool producesImage = std::ranges::any_of(definition->outputs, [](const auto& output) {
        return output.valueKind == SocketValueKind::Image;
    });
    if (!producesImage)
        return std::nullopt;
    for (const auto& input : definition->inputs)
        if (input.valueKind == SocketValueKind::Image)
            return input.name;
    return std::nullopt;
}

} // namespace

const EdgeRecord* mergeSlotImageEdge(const CanonicalGraph& graph, const NodeId merge,
                                     const LayerSlotId slot) {
    for (const auto& edge : graph.edges())
        if (const auto* input = std::get_if<LayerStackInputRef>(&edge.destination);
            input != nullptr && input->stackNodeId == merge && input->slotId == slot &&
            input->role == kLayerStackContentInputRole)
            return &edge;
    return nullptr;
}

std::optional<MergeEntryRef> mergeEntryOfSlot(const CanonicalGraph& graph, const LayerSlotId slot) {
    for (const auto& stack : graph.merges())
        if (stack.find(slot) != nullptr)
            return MergeEntryRef{stack.nodeId(), slot};
    return std::nullopt;
}

std::span<const ParameterBinding> layerParameterBindings(const CanonicalGraph& graph,
                                                         const LayerOutputBoundary& layer) {
    if (layer.entryOwned())
        return layer.parameters;
    if (const auto* node = graph.findNode(layer.nodeId))
        return node->parameters;
    return {};
}

const ParameterBinding* layerParameterBinding(const CanonicalGraph& graph,
                                              const LayerOutputBoundary& layer,
                                              const std::string_view role) {
    for (const auto& binding : layerParameterBindings(graph, layer))
        if (binding.role == role)
            return &binding;
    return nullptr;
}

const LayerOutputBoundary* chainLayer(const CanonicalGraph& graph, const OutputPortRef& source,
                                      const NodeDefinitionRegistry& registry) {
    std::unordered_set<NodeId> visited;
    auto current = source;
    while (visited.insert(current.nodeId).second) {
        const auto* node = graph.findNode(current.nodeId);
        if (node == nullptr)
            return nullptr;
        if (node->typeId == kLayerOutputNodeType) {
            for (const auto& boundary : graph.layerOutputs())
                if (!boundary.entryOwned() && boundary.nodeId == node->id)
                    return &boundary;
            return nullptr;
        }
        const auto input = chainInput(*node, registry);
        if (!input)
            return nullptr;
        const auto* edge = edgeInto(graph, node->id, *input);
        if (edge == nullptr)
            return nullptr;
        current = edge->source;
    }
    return nullptr;
}

const LayerOutputBoundary* entryOwnedLayer(const CanonicalGraph& graph, const NodeId merge,
                                           const LayerSlotId slot) {
    const auto* stack = graph.merge(merge);
    const auto* entry = stack ? stack->find(slot) : nullptr;
    if (entry == nullptr || !entry->layerId.isValid())
        return nullptr;
    const auto* layer = graph.findLayer(entry->layerId);
    return layer != nullptr && layer->entryOwned() && layer->nodeId == merge &&
                   *layer->entrySlot == slot
               ? layer
               : nullptr;
}

const LayerOutputBoundary* mergeEntryLayer(const CanonicalGraph& graph, const NodeId merge,
                                           const LayerSlotId slot,
                                           const NodeDefinitionRegistry& registry) {
    if (const auto* edge = mergeSlotImageEdge(graph, merge, slot))
        if (const auto* layer = chainLayer(graph, edge->source, registry))
            return layer;
    if (const auto* owned = entryOwnedLayer(graph, merge, slot))
        return owned;
    // An audio-only slot of a Layer node: the slot's direct layer identity.
    const auto* stack = graph.merge(merge);
    const auto* entry = stack ? stack->find(slot) : nullptr;
    if (entry != nullptr && entry->layerId.isValid())
        return graph.findLayer(entry->layerId);
    return nullptr;
}

std::vector<MergeEntryRef> mergeEntriesOfLayer(const CanonicalGraph& graph, const LayerId layer,
                                               const NodeDefinitionRegistry& registry) {
    std::vector<MergeEntryRef> entries;
    for (const auto& stack : graph.merges())
        for (const auto& entry : stack.entries())
            if (const auto* found = mergeEntryLayer(graph, stack.nodeId(), entry.slotId, registry);
                found != nullptr && found->layerId == layer)
                entries.push_back({stack.nodeId(), entry.slotId});
    return entries;
}

std::vector<MergeEntryRef> adjustmentTargets(const CanonicalGraph& graph, const NodeId adjustment,
                                             const NodeDefinitionRegistry& registry) {
    std::vector<MergeEntryRef> targets;
    std::unordered_set<NodeId> visited{adjustment};
    std::deque<NodeId> pending{adjustment};
    while (!pending.empty()) {
        const auto current = pending.front();
        pending.pop_front();
        for (const auto& edge : graph.edges()) {
            if (edge.source.nodeId != current ||
                graph.outputKind(edge.source, registry) != SocketValueKind::Image)
                continue;
            if (const auto* slot = std::get_if<LayerStackInputRef>(&edge.destination)) {
                if (slot->role != kLayerStackContentInputRole)
                    continue;
                const MergeEntryRef target{slot->stackNodeId, slot->slotId};
                if (std::ranges::find(targets, target) == targets.end())
                    targets.push_back(target);
                continue;
            }
            const auto& input = std::get<NodeInputRef>(edge.destination);
            if (visited.insert(input.nodeId).second)
                pending.push_back(input.nodeId);
        }
    }
    std::ranges::sort(targets, [](const MergeEntryRef& left, const MergeEntryRef& right) {
        return left.merge.value() != right.merge.value() ? left.merge.value() < right.merge.value()
                                                         : left.slot.value() < right.slot.value();
    });
    return targets;
}

std::unordered_map<MergeEntryRef, NodeId, MergeEntryRefHash>
adjustmentEntries(const CanonicalGraph& graph, const NodeDefinitionRegistry& registry) {
    std::unordered_map<MergeEntryRef, NodeId, MergeEntryRefHash> entries;
    for (const auto& node : graph.nodes()) {
        if (node.typeId != kAdjustmentNodeType)
            continue;
        const auto targets = adjustmentTargets(graph, node.id, registry);
        if (targets.size() == 1)
            entries.emplace(targets.front(), node.id);
    }
    return entries;
}

std::optional<NodeId> mergeEntryAdjustment(const CanonicalGraph& graph, const NodeId merge,
                                           const LayerSlotId slot,
                                           const NodeDefinitionRegistry& registry) {
    if (std::ranges::none_of(graph.nodes(),
                             [](const auto& node) { return node.typeId == kAdjustmentNodeType; }))
        return std::nullopt;
    const auto entries = adjustmentEntries(graph, registry);
    if (const auto found = entries.find({merge, slot}); found != entries.end())
        return found->second;
    return std::nullopt;
}

std::span<const EntryLayerRole> entryLayerRoles() noexcept { return kEntryLayerRoles; }

ValidationResult validateEntryLayers(const CanonicalGraph& graph,
                                     const ParameterStore& parameters) {
    ValidationResult result;
    std::unordered_set<ParameterId> nodeBound;
    for (const auto& node : graph.nodes())
        for (const auto& binding : node.parameters)
            nodeBound.insert(binding.parameterId);
    for (const auto& layer : graph.layerOutputs())
        for (const auto& mask : layer.masks)
            for (const auto id : {mask.path, mask.feather, mask.expansion, mask.opacity})
                nodeBound.insert(id);
    std::unordered_set<ParameterId> entryBound;
    std::unordered_set<LayerSlotId> owningSlots;
    for (const auto& layer : graph.layerOutputs()) {
        const auto path = "layerOutputs[" + std::to_string(layer.layerId.value()) + "]";
        if (layer.pivot != LayerPivot::Content && layer.pivot != LayerPivot::Frame)
            result.add(ValidationCode::InvalidValue, path + ".pivot",
                       "Layer pivot must be Content or Frame");
        if (!layer.entryOwned()) {
            if (!layer.parameters.empty())
                result.add(ValidationCode::InvalidLayerBoundary, path + ".parameters",
                           "Only a Merge entry's own layer owns parameters");
            continue;
        }
        const auto* stack = graph.merge(layer.nodeId);
        const auto* entry = stack ? stack->find(*layer.entrySlot) : nullptr;
        if (entry == nullptr || entry->layerId != layer.layerId) {
            result.add(ValidationCode::InvalidLayerBoundary, path + ".slotId",
                       "An entry layer must name a Merge slot that names it");
        } else if (!owningSlots.insert(*layer.entrySlot).second) {
            result.add(ValidationCode::DuplicateId, path + ".slotId",
                       "A Merge slot owns at most one entry layer");
        }
        if (const auto* edge = mergeSlotImageEdge(graph, layer.nodeId, *layer.entrySlot)) {
            if (const auto* source = graph.findNode(edge->source.nodeId);
                source != nullptr && source->typeId == kLayerOutputNodeType)
                result.add(ValidationCode::InvalidLayerBoundary, path + ".slotId",
                           "A slot fed by a Layer node has no entry layer of its own");
        }
        std::unordered_set<std::string_view> seen;
        for (const auto& binding : layer.parameters) {
            const auto bindingPath = path + ".parameters[" + binding.role + "]";
            const auto role = std::ranges::find(kEntryLayerRoles, std::string_view(binding.role),
                                                &EntryLayerRole::role);
            if (role == kEntryLayerRoles.end() || !seen.insert(role->role).second) {
                result.add(ValidationCode::InvalidValue, bindingPath,
                           "Entry layer parameter role is unknown or repeated");
                continue;
            }
            const auto* record = parameters.find(binding.parameterId);
            if (record == nullptr) {
                result.add(ValidationCode::MissingReference, bindingPath,
                           "Entry layer parameter does not resolve to a parameter");
                continue;
            }
            if (record->schemaKey != role->schemaKey)
                result.add(ValidationCode::TypeMismatch, bindingPath,
                           "Entry layer parameter has the wrong schema");
            if (nodeBound.contains(binding.parameterId) ||
                !entryBound.insert(binding.parameterId).second)
                result.add(ValidationCode::SharedReference, bindingPath,
                           "An entry layer parameter is owned by exactly one layer");
            if (std::holds_alternative<DriverBindingSource>(record->source))
                result.add(ValidationCode::InvalidValue, bindingPath,
                           "An entry layer parameter is a constant or an animation curve");
        }
        for (const auto& role : kEntryLayerRoles)
            if (role.role != kTimeRemapParameterRole && !seen.contains(role.role))
                result.add(ValidationCode::MissingReference, path + ".parameters",
                           "Entry layer must bind " + std::string(role.role));
    }
    return result;
}

} // namespace bloom::document
