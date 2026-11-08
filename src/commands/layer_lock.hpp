#pragma once
#include <algorithm>
#include <string>
#include <variant>
#include <vector>
#include <bloom/document/project.hpp>
namespace bloom::commands::detail {
// `locked`, when given, receives what was locked and where, for the refusal message.
inline bool changesLockedLayers(const document::Project& before, const document::Project& after,
                                std::string* locked = nullptr) {
    for (const auto& composition : before.compositions()) {
        const auto lockedLayer = [&](const document::LayerOutputBoundary& layer) {
            if (locked != nullptr)
                *locked = "Layer " + std::to_string(layer.layerId.value()) + " \"" + layer.name +
                          "\" in composition " + std::to_string(composition.id().value()) +
                          " is locked; unlock it before editing";
            return true;
        };
        const auto lockedNode = [&](const document::NodeRecord& node) {
            if (locked != nullptr)
                *locked = "Node " + std::to_string(node.id.value()) + " (" + node.typeId +
                          ") in composition " + std::to_string(composition.id().value()) +
                          " is locked with its layer; unlock the layer before editing";
            return true;
        };
        const auto* changed = after.findComposition(composition.id());
        if (!changed)
            continue; // Composition deletion is a separate authoring scope.
        // #357: untouched content cannot have changed a locked record. `before` is a published
        // snapshot, so its lock index was built once at freeze time and nodeLocked() is a lookup.
        if (changed->contentIdentity() == composition.contentIdentity() &&
            changed->duration() == composition.duration())
            continue;
        for (const auto& layer : composition.graph().layerOutputs()) {
            if (!layer.locked)
                continue;
            const auto* next = changed->graph().findLayer(layer.layerId);
            if (!next || layer.inPoint != next->inPoint ||
                layer.endPoint(composition.duration()) != next->endPoint(changed->duration()))
                return lockedLayer(layer);
            // #470: a locked layer still takes view and naming state: Enable/Disable, Solo, Shy,
            // its label color and its name (and whether the name follows the source), and it can
            // be unlocked. Everything else in the layer record is content (masks, track matte,
            // timing, parent, pivot, bound parameters) and is refused. A deleted parent leaves
            // its child unparented; that cascade is not an edit of the child.
            auto expected = layer;
            auto actual = *next;
            for (auto* record : {&expected, &actual}) {
                record->enabled = true;
                record->solo = false;
                record->locked = true;
                record->shy = false;
                record->labelColor.reset();
                record->name.clear();
                record->nameFollowsSource = false;
            }
            if (layer.parent && !changed->graph().findLayer(*layer.parent))
                actual.parent = expected.parent;
            if (expected != actual)
                return lockedLayer(layer);
            // #502: a locked entry layer's own parameters (and curves) are locked like a Layer
            // node's.
            if (layer.parameters != next->parameters)
                return lockedLayer(layer);
            for (const auto& binding : layer.parameters) {
                const auto* parameter = composition.parameters().find(binding.parameterId);
                const auto* edited = changed->parameters().find(binding.parameterId);
                if (!parameter || !edited || *parameter != *edited)
                    return lockedLayer(layer);
                if (const auto* curve =
                        std::get_if<document::AnimationCurveSource>(&parameter->source)) {
                    const auto* oldCurve = composition.animationCurves().find(curve->curveId);
                    const auto* newCurve = changed->animationCurves().find(curve->curveId);
                    if (!oldCurve || !newCurve || *oldCurve != *newCurve)
                        return lockedLayer(layer);
                }
            }
        }
        std::vector<document::NodeId> lockedIds;
        for (const auto& node : composition.graph().nodes()) {
            if (!composition.nodeLocked(node.id))
                continue;
            lockedIds.push_back(node.id);
            const auto* next = changed->graph().findNode(node.id);
            if (!next || node != *next)
                return lockedNode(node);
            for (const auto& binding : node.parameters) {
                const auto* parameter = composition.parameters().find(binding.parameterId);
                const auto* edited = changed->parameters().find(binding.parameterId);
                if (!parameter || !edited || *parameter != *edited)
                    return lockedNode(node);
                if (const auto* curve =
                        std::get_if<document::AnimationCurveSource>(&parameter->source)) {
                    const auto* oldCurve = composition.animationCurves().find(curve->curveId);
                    const auto* newCurve = changed->animationCurves().find(curve->curveId);
                    if (!oldCurve || !newCurve || *oldCurve != *newCurve)
                        return lockedNode(node);
                }
            }
        }
        // #470: what feeds a locked node (its image inputs and its drivers) is locked with it, so
        // an effect cannot be inserted, removed or rewired upstream of a locked layer.
        if (!lockedIds.empty()) {
            const auto edgesInto = [&](const document::Composition& source) {
                std::vector<document::EdgeRecord> edges;
                for (const auto& edge : source.graph().edges())
                    if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
                        input != nullptr && std::ranges::find(lockedIds, input->nodeId) !=
                                                lockedIds.end())
                        edges.push_back(edge);
                std::ranges::sort(edges, {}, [](const auto& edge) { return edge.id.value(); });
                return edges;
            };
            if (edgesInto(composition) != edgesInto(*changed))
                for (const auto& node : composition.graph().nodes())
                    if (std::ranges::find(lockedIds, node.id) != lockedIds.end())
                        return lockedNode(node);
        }
    }
    return false;
}
} // namespace bloom::commands::detail
