// Read-only menu eligibility for node operations (issue #357).
//
// Each predicate mirrors the rejection rules of the operation's apply() and the command stack's
// locked-layer refusal, answering "would executing this be accepted?" from the published
// composition alone. None of them copies the document, opens a draft or validates a project;
// where apply() would fail on its final graph validation, the predicate checks the specific
// condition that validation would find. A NoChange outcome is accepted, exactly as
// canApplyNodeOperation reports it.
#include <bloom/commands/node_operations.hpp>

#include "node_operation_support.hpp"

#include <bloom/document/persisted_text.hpp>

#include <algorithm>
#include <variant>

namespace bloom::commands {
namespace detail {
std::optional<DissolvePath> dissolvePath(const document::Composition& composition,
                                         const document::NodeId nodeId,
                                         const document::NodeDefinitionRegistry& registry) {
    const auto& graph = composition.graph();
    const auto* node = graph.findNode(nodeId);
    if (node == nullptr)
        return std::nullopt;
    const auto* definition = registry.find(node->typeId, node->schemaVersion);
    if (definition == nullptr)
        return std::nullopt;
    auto input = std::ranges::find(definition->inputs, document::SocketValueKind::Image,
                                   &document::InputPortDefinition::valueKind);
    auto output = std::ranges::find(definition->outputs, document::SocketValueKind::Image,
                                    &document::OutputPortDefinition::valueKind);
    if (input == definition->inputs.end() || output == definition->outputs.end()) {
        // No Image pair: the first output that has an input of its own kind.
        input = definition->inputs.end();
        for (output = definition->outputs.begin(); output != definition->outputs.end(); ++output) {
            input = std::ranges::find(definition->inputs, output->valueKind,
                                      &document::InputPortDefinition::valueKind);
            if (input != definition->inputs.end())
                break;
        }
        if (input == definition->inputs.end())
            return std::nullopt;
    }
    // The upstream is the link into that input: an edge, or (task S7) the driver of the operand
    // parameter the socket stands for.
    std::optional<document::OutputPortRef> upstreamSource;
    const document::InputPortRef through = document::NodeInputRef{nodeId, input->name};
    if (const auto* edge = detail::inputEdge(graph, through)) {
        upstreamSource = edge->source;
    } else if (const auto* binding = detail::parameterSocketBinding(graph, through)) {
        const auto* parameter = composition.parameters().find(binding->parameterId);
        const auto* driver = parameter
                                 ? std::get_if<document::DriverBindingSource>(&parameter->source)
                                 : nullptr;
        if (driver != nullptr)
            upstreamSource = document::OutputPortRef{driver->sourceNodeId, driver->outputPort};
    }
    if (!upstreamSource)
        return std::nullopt;
    const auto* incoming = &*upstreamSource;
    // #723: each consumer is re-linked to the dissolved node's upstream source on its own. One that
    // cannot take it (CanonicalGraph::addEdge refuses a link whose kinds no longer connect; a Merge
    // refuses a Layer node it already holds as another entry) is listed, and loses its link.
    DissolvePath path{input->name, output->name, *incoming, {}};
    const auto sourceKind = graph.outputKind(*incoming, registry);
    const auto upstream = std::ranges::find_if(graph.layerOutputs(), [&](const auto& layer) {
        return !layer.entryOwned() && layer.nodeId == incoming->nodeId;
    });
    const auto consider = [&](const document::InputPortRef& destination) {
        const auto targetKind = graph.inputKind(destination, registry);
        if (sourceKind && targetKind && !graph.acceptsConnection(*sourceKind, destination, registry)) {
            path.refused.push_back(
                {destination, DissolveRefusalReason::KindMismatch, sourceKind, targetKind});
            return;
        }
        // #502: a slot fed straight by a Layer node names that node's layer.
        const auto* slot = std::get_if<document::LayerStackInputRef>(&destination);
        if (slot == nullptr || upstream == graph.layerOutputs().end())
            return;
        const auto* stack = graph.merge(slot->stackNodeId);
        if (stack != nullptr && std::ranges::any_of(stack->entries(), [&](const auto& entry) {
                return entry.slotId != slot->slotId && entry.layerId == upstream->layerId;
            }))
            path.refused.push_back(
                {destination, DissolveRefusalReason::LayerAlreadyInMerge, sourceKind, targetKind});
    };
    for (const auto& edge : graph.edges())
        if (edge.source.nodeId == nodeId && edge.source.port == output->name)
            consider(edge.destination);
    for (const auto& [destination, parameter] : drivenInputs(composition, nodeId, output->name)) {
        (void)parameter;
        consider(destination);
    }
    return path;
}

std::vector<std::pair<document::InputPortRef, document::ParameterId>>
drivenInputs(const document::Composition& composition, const document::NodeId nodeId,
             const std::string_view output) {
    std::vector<std::pair<document::InputPortRef, document::ParameterId>> driven;
    for (const auto& node : composition.graph().nodes()) {
        if (node.id == nodeId)
            continue;
        for (const auto& binding : node.parameters) {
            const auto* parameter = composition.parameters().find(binding.parameterId);
            const auto* driver =
                parameter ? std::get_if<document::DriverBindingSource>(&parameter->source)
                          : nullptr;
            if (driver != nullptr && driver->sourceNodeId == nodeId && driver->outputPort == output)
                driven.emplace_back(document::NodeInputRef{node.id, binding.role},
                                    binding.parameterId);
        }
    }
    return driven;
}
} // namespace detail

std::vector<DissolveRefusal> dissolveRefusals(const document::Composition& composition,
                                              const document::NodeId nodeId,
                                              const document::NodeDefinitionRegistry& registry) {
    auto path = detail::dissolvePath(composition, nodeId, registry);
    return path ? std::move(path->refused) : std::vector<DissolveRefusal>{};
}

// #704, #716: Delete removes any node that is not locked, the composition's Output and the Merge
// feeding it included (the composition then has no output until an Output is added again); a
// surviving parameter this node drove goes back to its registered default.
NodeRemovalRefusal nodeRemovalRefusal(const document::Composition& composition,
                                      const document::NodeId nodeId) {
    const auto& graph = composition.graph();
    const auto* node = graph.findNode(nodeId);
    if (node == nullptr)
        return NodeRemovalRefusal::Missing;
    if (composition.nodeLocked(nodeId))
        return NodeRemovalRefusal::Locked;
    return NodeRemovalRefusal::None;
}

bool canRemoveNodes(const document::Composition& composition,
                    const std::set<document::NodeId>& nodes) {
    if (nodes.empty())
        return true; // NoChange.
    const auto& graph = composition.graph();
    for (const auto id : nodes)
        if (!graph.findNode(id) || composition.nodeLocked(id))
            return false;
    return true;
}

// #704: Dissolve is available wherever Delete is. It reconnects through the node where its types
// allow (detail::dissolvePath) and otherwise deletes it, breaking its links.
bool canDissolveNode(const document::Composition& composition, const document::NodeId nodeId,
                     const document::NodeDefinitionRegistry& /*registry*/) {
    return canRemoveNodes(composition, {nodeId});
}

bool canSetNodeMuted(const document::Composition& composition, const document::NodeId nodeId,
                     bool /*muted*/) {
    return composition.graph().findNode(nodeId) != nullptr;
}

bool canSetNodeCollapsed(const document::Composition& composition, const document::NodeId nodeId,
                         bool /*collapsed*/) {
    return composition.graph().findNode(nodeId) != nullptr;
}

bool canRenameLayer(const document::Composition& composition, const document::LayerId layerId,
                    const std::string_view name) {
    return document::isValidHumanFacingName(name) && composition.graph().findLayer(layerId);
}

bool canGroupNodes(const document::Composition& composition, const document::IdAllocator& ids,
                   const std::set<document::NodeId>& nodes, const std::string_view name) {
    if (nodes.empty() || !document::isValidHumanFacingName(name))
        return false;
    for (const auto id : nodes)
        if (!composition.graph().findNode(id))
            return false;
    auto available = ids;
    return available.allocateNodeGroup().has_value();
}
} // namespace bloom::commands
