#pragma once

#include <bloom/commands/node_operations.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <variant>

namespace bloom::commands::detail {
inline OperationResult invalidTarget() {
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Composition or node target does not exist");
}
// Missing-object refusals: each says which object was not found and where, so a caller that
// addressed the wrong composition, layer or node learns which one.
inline OperationResult missingComposition(const document::CompositionId composition) {
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Composition " + std::to_string(composition.value()) +
                                         " does not exist");
}
inline OperationResult missingLayer(const document::Composition* composition,
                                    const document::CompositionId compositionId,
                                    const document::LayerId layer) {
    if (composition == nullptr)
        return missingComposition(compositionId);
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Layer " + std::to_string(layer.value()) +
                                         " does not exist in composition " +
                                         std::to_string(compositionId.value()));
}
inline OperationResult missingNode(const document::Composition* composition,
                                   const document::CompositionId compositionId,
                                   const document::NodeId node) {
    if (composition == nullptr)
        return missingComposition(compositionId);
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Node " + std::to_string(node.value()) +
                                         " does not exist in composition " +
                                         std::to_string(compositionId.value()));
}
inline OperationResult missingMask(const document::LayerId layer, const document::LayerMaskId mask) {
    return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                     "Mask " + std::to_string(mask.value()) +
                                         " does not exist on layer " +
                                         std::to_string(layer.value()));
}
inline OperationResult lockedLayer(const document::LayerOutputBoundary& layer) {
    return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                     "Layer " + std::to_string(layer.layerId.value()) + " \"" +
                                         layer.name + "\" is locked; unlock it before editing");
}
// `kinds` names the ID spaces the failed allocation drew from ("keyframe", "node and parameter").
inline OperationResult exhaustedIds(const std::string_view kinds) {
    return OperationResult::rejected(OperationIssueCode::Unsupported,
                                     "The " + std::string(kinds) + " ID space is exhausted; the project cannot create another one");
}
inline bool finite(const document::Vec2d value) {
    return std::isfinite(value.x) && std::isfinite(value.y);
}
inline document::NodeId destinationNode(const document::InputPortRef& input) {
    return std::visit(
        [](const auto& port) {
            if constexpr (std::is_same_v<std::decay_t<decltype(port)>, document::NodeInputRef>)
                return port.nodeId;
            else
                return port.stackNodeId;
        },
        input);
}
inline const document::EdgeRecord* inputEdge(const document::CanonicalGraph& graph,
                                             const document::InputPortRef& input) {
    const auto edge = std::ranges::find(graph.edges(), input, &document::EdgeRecord::destination);
    return edge == graph.edges().end() ? nullptr : &*edge;
}
// The parameter an input port fills, or nullptr when the port is image transport (or a Reroute's
// pass-through, which carries someone else's value and therefore has no parameter of its own).
// Task S7: a non-Image input socket and the ParameterDefinition behind it share one name, which is
// what makes "linked" one question with one answer rather than a per-node table.
inline const document::ParameterBinding*
parameterSocketBinding(const document::CanonicalGraph& graph, const document::InputPortRef& input) {
    const auto* fixed = std::get_if<document::NodeInputRef>(&input);
    if (fixed == nullptr) {
        return nullptr;
    }
    const auto* node = graph.findNode(fixed->nodeId);
    if (node == nullptr) {
        return nullptr;
    }
    const auto match =
        std::ranges::find(node->parameters, fixed->port, &document::ParameterBinding::role);
    return match == node->parameters.end() ? nullptr : &*match;
}

// The value the registry declares for this parameter role -- what an explicit disconnect restores.
// The document has nowhere to remember a constant a driver superseded, and inventing a durable
// "previous value" field would be a new persisted concept; undo is what restores the exact constant
// the artist had, because a link and its removal are each one transaction.
inline std::optional<document::ParameterValue>
registeredDefault(const document::CanonicalGraph& graph, const document::NodeId nodeId,
                  const std::string_view role, const document::NodeDefinitionRegistry& registry) {
    const auto* node = graph.findNode(nodeId);
    const auto* definition =
        node == nullptr ? nullptr : registry.find(node->typeId, node->schemaVersion);
    if (definition == nullptr) {
        return std::nullopt;
    }
    const auto declared =
        std::ranges::find(definition->parameters, role, &document::ParameterDefinition::role);
    return declared == definition->parameters.end() ? std::nullopt
                                                    : std::optional(declared->defaultValue);
}

inline bool protectedNode(const document::CanonicalGraph& graph, const document::NodeId id) {
    const auto* node = graph.findNode(id);
    return graph.outputMergeId() == id ||
           (graph.compositionOutput() && graph.compositionOutput()->nodeId == id) ||
           (node && node->typeId == document::kCompositionOutputNodeType);
}
inline document::NodeLayoutRecord layoutFor(const document::Composition& composition,
                                            const document::NodeId id) {
    const auto record = composition.nodeLayout().find(id);
    if (record != composition.nodeLayout().end())
        return record->second;
    return document::defaultNodeLayout(composition.graph().nodes()).at(id);
}
// Takes `nodes` out of whatever groups hold them -- all of them, or every one but `except` -- and
// removes any group left with no members. Shared by every command that can empty a group: grouping
// nodes away from an older frame, setting a frame's membership, moving a card out of one, and
// removing or dissolving a node. One place decides what an emptied frame becomes.
inline bool detachFromNodeGroups(document::Composition& composition,
                                 const std::set<document::NodeId>& nodes,
                                 const std::optional<document::NodeGroupId> except = std::nullopt) {
    bool changed = false;
    for (auto group = composition.nodeGroups().begin(); group != composition.nodeGroups().end();) {
        if (except && group->first == *except) {
            ++group;
            continue;
        }
        for (const auto id : nodes)
            changed = group->second.members.erase(id) > 0 || changed;
        if (group->second.members.empty()) {
            group = composition.nodeGroups().erase(group);
            changed = true;
        } else {
            ++group;
        }
    }
    return changed;
}

inline std::optional<OperationResult> validateGraph(
    const document::Composition& composition,
    const document::NodeDefinitionRegistry& registry = document::builtInNodeDefinitions()) {
    const auto validation = composition.graph().validate(composition.parameters(), registry);
    for (const auto& issue : validation.issues()) {
        if (issue.severity == document::ValidationSeverity::Warning)
            continue;
        const auto code = issue.code == document::ValidationCode::GraphCycle
                              ? OperationIssueCode::GraphCycle
                          : issue.code == document::ValidationCode::SocketKindMismatch
                              ? OperationIssueCode::SocketKindMismatch
                              : OperationIssueCode::InvalidValue;
        return OperationResult::rejected(code, issue.message);
    }
    return std::nullopt;
}
// A track matte whose source layer is parented, directly or through its parents, to the layer it
// mattes. A parent link and a matte share one same-time evaluation order (docs/architecture/
// layer-graph-model.md, Transform Parenting), so that layer would wait on itself. The generic cycle
// refusal is replaced by one that names both layers and the way out.
inline std::optional<OperationResult>
matteParentedToItsTarget(const document::CanonicalGraph& graph) {
    for (const auto& target : graph.layerOutputs()) {
        if (!target.trackMatte)
            continue;
        const auto source = target.trackMatte->source;
        std::size_t remaining = graph.layerOutputs().size();
        for (const auto* layer = graph.findLayer(source);
             layer != nullptr && layer->parent && remaining-- > 0;
             layer = graph.findLayer(*layer->parent)) {
            if (*layer->parent != target.layerId)
                continue;
            const auto name = [&](const document::LayerId id) {
                return "layer " + std::to_string(id.value());
            };
            return OperationResult::rejected(
                OperationIssueCode::GraphCycle,
                "The track matte of " + name(target.layerId) + " is " + name(source) +
                    ", which is parented to it" +
                    (layer->layerId == source ? std::string()
                                              : " through " + name(layer->layerId)) +
                    ". Bloom evaluates a parent link and a matte in one order, so a layer's matte "
                    "cannot be its own child; unparent the matte layer or matte with a copy of it "
                    "that is not parented to " +
                    name(target.layerId) + ".");
        }
    }
    return std::nullopt;
}

// #383: why `layer` may not read the Mask output `matte` as its track matte or a matte mask, or
// nothing when it may: the output must be a Mask of this composition, and its node must not depend
// on the layer (its Layer node, or for a Merge entry's own layer its Merge) through image edges.
inline std::optional<OperationResult> matteNodeRefusal(const document::CanonicalGraph& graph,
                                                       const document::OutputPortRef& matte,
                                                       const document::LayerOutputBoundary& layer) {
    if (graph.findNode(matte.nodeId) == nullptr)
        return OperationResult::rejected(OperationIssueCode::MissingReference,
                                         "The matte node is not in the composition");
    if (graph.outputKind(matte) != document::SocketValueKind::Mask)
        return OperationResult::rejected(OperationIssueCode::SocketKindMismatch,
                                         "A matte must be a node's Mask output");
    if (document::dependsOnNode(graph, matte.nodeId, layer.nodeId))
        return OperationResult::rejected(
            OperationIssueCode::GraphCycle,
            "The matte node is computed from this layer, so it cannot matte it");
    return std::nullopt;
}

// A reroute that now has nothing on either side, removed (task FIX1, item I). A reroute is a bend
// in a wire and nothing else -- it carries no value of its own, has no parameter to edit, and shows
// no name -- so once both of its links are gone there is nothing for it to be. Answers whether it
// removed one, and is a no-op for every node that is not a stranded reroute.
inline bool eraseStrandedReroute(document::Composition& composition, const document::NodeId id) {
    auto& graph = composition.graph();
    const auto* node = graph.findNode(id);
    if (node == nullptr || !document::isRerouteNodeType(node->typeId)) {
        return false;
    }
    const auto edges = graph.edges();
    const bool linked =
        std::ranges::any_of(edges.begin(), edges.end(), [id](const document::EdgeRecord& edge) {
            return edge.source.nodeId == id || destinationNode(edge.destination) == id;
        });
    if (linked) {
        return false;
    }
    (void)graph.eraseNode(id);
    composition.nodeLayout().erase(id);
    (void)detachFromNodeGroups(composition, {id});
    return true;
}

inline void eraseOrphanedParameters(document::Composition& composition,
                                    const std::set<document::ParameterId>& candidates) {
    for (const auto id : candidates) {
        // #502: an entry layer's own parameters and every mask's are owned outside node bindings.
        const bool referenced =
            std::ranges::any_of(composition.graph().nodes(),
                                [id](const auto& node) {
                                    return std::ranges::any_of(
                                        node.parameters, [id](const auto& binding) {
                                            return binding.parameterId == id;
                                        });
                                }) ||
            std::ranges::any_of(composition.graph().layerOutputs(), [id](const auto& layer) {
                return std::ranges::any_of(
                           layer.parameters,
                           [id](const auto& binding) { return binding.parameterId == id; }) ||
                       std::ranges::any_of(layer.masks, [id](const auto& mask) {
                           return mask.path == id || mask.feather == id ||
                                  mask.expansion == id || mask.opacity == id;
                       });
            });
        if (referenced)
            continue;
        const auto* parameter = composition.parameters().find(id);
        const auto* animation =
            parameter ? std::get_if<document::AnimationCurveSource>(&parameter->source) : nullptr;
        const auto curve = animation ? std::optional(animation->curveId) : std::nullopt;
        (void)composition.parameters().erase(id);
        if (curve &&
            std::ranges::none_of(composition.parameters().records(), [&](const auto& record) {
                const auto* source = std::get_if<document::AnimationCurveSource>(&record.source);
                return source && source->curveId == *curve;
            }))
            (void)composition.animationCurves().erase(*curve);
    }
}
} // namespace bloom::commands::detail
