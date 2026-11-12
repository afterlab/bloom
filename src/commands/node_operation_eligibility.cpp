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
    const auto* incoming = detail::inputEdge(graph, document::NodeInputRef{nodeId, input->name});
    if (!incoming)
        return std::nullopt;
    // Each consumer is re-linked to the dissolved node's upstream source; CanonicalGraph::addEdge
    // refuses a link whose source and destination kinds no longer connect.
    const auto sourceKind = graph.outputKind(incoming->source, registry);
    for (const auto& edge : graph.edges()) {
        if (edge.source.nodeId != nodeId || edge.source.port != output->name ||
            std::holds_alternative<document::LayerStackInputRef>(edge.destination))
            continue;
        const auto targetKind = graph.inputKind(edge.destination, registry);
        if (sourceKind && targetKind &&
            !graph.acceptsConnection(*sourceKind, edge.destination, registry))
            return std::nullopt;
    }
    // #502: a Layer node's slots take its upstream; an upstream Layer node becomes their layer,
    // which a Merge already holding that Layer node as another entry refuses.
    if (node->typeId == document::kLayerOutputNodeType) {
        const auto upstream = std::ranges::find_if(graph.layerOutputs(), [&](const auto& layer) {
            return !layer.entryOwned() && layer.nodeId == incoming->source.nodeId;
        });
        if (upstream != graph.layerOutputs().end())
            for (const auto& edge : graph.edges()) {
                const auto* slot = std::get_if<document::LayerStackInputRef>(&edge.destination);
                if (slot == nullptr || edge.source.nodeId != nodeId ||
                    edge.source.port != output->name)
                    continue;
                const auto* stack = graph.merge(slot->stackNodeId);
                if (stack != nullptr &&
                    std::ranges::any_of(stack->entries(), [&](const auto& entry) {
                        return entry.slotId != slot->slotId && entry.layerId == upstream->layerId;
                    }))
                    return std::nullopt;
            }
    }
    return DissolvePath{input->name, output->name, incoming->source};
}
} // namespace detail

// #704: Delete removes any node that is not the composition's Output (or the Merge feeding it) or
// locked; a surviving parameter this node drove goes back to its registered default.
bool canRemoveNodes(const document::Composition& composition,
                    const std::set<document::NodeId>& nodes) {
    if (nodes.empty())
        return true; // NoChange.
    const auto& graph = composition.graph();
    for (const auto id : nodes)
        if (!graph.findNode(id) || detail::protectedNode(graph, id) || composition.nodeLocked(id))
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
