#include <bloom/commands/node_unused_operations.hpp>

#include "node_operation_support.hpp"

#include <bloom/commands/node_operations.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/project.hpp>

#include <map>
#include <variant>
#include <vector>

namespace bloom::commands {

namespace {

// Every node the given one reads from: the sources of its incoming links (a Merge's slots
// included), the sources of its driven parameters, and the nodes the layers it carries read as
// parent, track matte or mask matte.
void forEachInput(const document::Composition& composition,
                  const std::multimap<document::NodeId, document::NodeId>& linked,
                  const document::NodeId id, const auto& visit) {
    const auto& graph = composition.graph();
    const auto [first, last] = linked.equal_range(id);
    for (auto link = first; link != last; ++link)
        visit(link->second);
    if (const auto* node = graph.findNode(id))
        for (const auto& binding : node->parameters) {
            const auto* parameter = composition.parameters().find(binding.parameterId);
            const auto* driver =
                parameter ? std::get_if<document::DriverBindingSource>(&parameter->source)
                          : nullptr;
            if (driver != nullptr)
                visit(driver->sourceNodeId);
        }
    for (const auto& layer : graph.layerOutputs()) {
        if (layer.nodeId != id)
            continue;
        if (layer.parent)
            if (const auto* parent = graph.findLayer(*layer.parent))
                visit(parent->nodeId);
        if (layer.trackMatte) {
            if (const auto* source = graph.findLayer(layer.trackMatte->source))
                visit(source->nodeId);
            if (layer.trackMatte->node)
                visit(layer.trackMatte->node->nodeId);
        }
        for (const auto& mask : layer.masks)
            if (mask.matte)
                visit(mask.matte->nodeId);
    }
}

} // namespace

UnusedNodes findUnusedNodes(const document::Composition& composition) {
    const auto& graph = composition.graph();
    std::multimap<document::NodeId, document::NodeId> linked; // destination node -> source node
    for (const auto& edge : graph.edges())
        linked.emplace(detail::destinationNode(edge.destination), edge.source.nodeId);

    std::set<document::NodeId> used;
    std::vector<document::NodeId> pending;
    const auto reach = [&](const document::NodeId id) {
        if (graph.findNode(id) != nullptr && used.insert(id).second)
            pending.push_back(id);
    };
    if (graph.compositionOutput())
        reach(graph.compositionOutput()->nodeId);
    // A group definition is read through its Group Output and fed through its Group Input by
    // every Node Group naming it, so both stay whatever they connect to.
    for (const auto& node : graph.nodes())
        if (node.typeId == document::kGroupOutputNodeType ||
            node.typeId == document::kGroupInputNodeType)
            reach(node.id);
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        forEachInput(composition, linked, id, reach);
    }

    UnusedNodes result;
    // Without an output nothing is "on the path": refuse to call a whole graph unused.
    if (!graph.compositionOutput())
        return result;
    for (const auto& node : graph.nodes()) {
        if (used.contains(node.id))
            continue;
        if (nodeRemovalRefusal(composition, node.id) == NodeRemovalRefusal::None)
            result.removable.insert(node.id);
        else
            result.staying.insert(node.id);
    }
    return result;
}

OperationResult DeleteUnusedNodes::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(compositionId_);
    if (composition == nullptr)
        return detail::missingComposition(compositionId_);
    auto unused = findUnusedNodes(*composition);
    if (unused.removable.empty())
        return OperationResult::noChange();
    return RemoveNodes(compositionId_, std::move(unused.removable), registry_).apply(draft);
}

} // namespace bloom::commands
