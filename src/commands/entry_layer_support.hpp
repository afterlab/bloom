#pragma once
// #502 helpers shared by the entry-layer operations, their reconcile pass and the layer commands
// that read or write a layer's own parameter bindings.
#include <bloom/commands/result.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/document/project.hpp>

#include <optional>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace bloom::commands::detail {

// The bindings a layer owns, mutable: its Layer node's, or an entry layer's own.
inline std::vector<document::ParameterBinding>*
mutableLayerBindings(document::CanonicalGraph& graph, document::LayerOutputBoundary& layer) {
    if (layer.entryOwned())
        return &layer.parameters;
    auto* node = graph.findNode(layer.nodeId);
    return node == nullptr ? nullptr : &node->parameters;
}

// The default an entry layer's parameter for `role` starts at: the Layer node definition's, with
// the position at the composition centre so the entry layer is the identity.
inline std::optional<document::ParameterValue>
entryLayerDefaultValue(const document::Composition& composition, const std::string_view role) {
    if (role == document::kPositionParameterRole)
        return document::Vec2d{static_cast<double>(composition.format().width()) / 2.0,
                               static_cast<double>(composition.format().height()) / 2.0};
    const auto* definition = document::builtInNodeDefinitions().find(
        document::kLayerOutputNodeType, document::kLayerOutputNodeSchemaVersion);
    if (definition == nullptr)
        return std::nullopt;
    for (const auto& parameter : definition->parameters)
        if (parameter.role == role)
            return parameter.defaultValue;
    return std::nullopt;
}

// A copy of `curve` from `source` into `target` with fresh curve and keyframe ids.
inline std::optional<document::AnimationCurveId>
copyCurveFrom(document::IdAllocator& ids, const document::AnimationCurveStore& source,
              document::AnimationCurveStore& target, const document::AnimationCurveId curve) {
    const auto* record = source.find(curve);
    if (record == nullptr)
        return std::nullopt;
    auto copy = *record;
    const auto id = ids.allocateAnimationCurve();
    if (!id)
        return std::nullopt;
    bool allocated = true;
    std::visit(
        [&](auto& held) {
            held.id = *id;
            using Curve = std::remove_cvref_t<decltype(held)>;
            const auto renumber = [&](auto& key) {
                const auto keyId = ids.allocateKeyframe();
                if (!keyId)
                    allocated = false;
                else
                    key.id = *keyId;
            };
            if constexpr (document::kWholeValueAnimationCurve<Curve>) {
                for (auto& key : held.keyframes)
                    renumber(key);
            } else {
                for (auto& component : held.components)
                    for (auto& key : component.keyframes)
                        renumber(key);
            }
        },
        copy);
    if (!allocated || !target.insert(std::move(copy)))
        return std::nullopt;
    return id;
}

// A copy of parameter `parameter` of `source` inserted into `target` with a fresh id (and a fresh
// copy of its curve). A driven parameter copies as `fallback` when one is given: an entry layer's
// parameters are constants or curves.
inline std::optional<document::ParameterId>
copyParameterFrom(document::IdAllocator& ids, const document::Composition& source,
                  document::Composition& target, const document::ParameterId parameter,
                  const std::optional<document::ParameterValue>& fallback = std::nullopt) {
    const auto* record = source.parameters().find(parameter);
    if (record == nullptr)
        return std::nullopt;
    auto copy = *record;
    const auto id = ids.allocateParameter();
    if (!id)
        return std::nullopt;
    copy.id = *id;
    if (std::holds_alternative<document::DriverBindingSource>(copy.source)) {
        if (!fallback)
            return std::nullopt;
        copy.source = document::ConstantValueSource{*fallback};
    } else if (auto* animated = std::get_if<document::AnimationCurveSource>(&copy.source)) {
        const auto curve =
            copyCurveFrom(ids, source.animationCurves(), target.animationCurves(), animated->curveId);
        if (!curve)
            return std::nullopt;
        animated->curveId = *curve;
    }
    if (!target.parameters().insert(std::move(copy)))
        return std::nullopt;
    return id;
}

// Every parameter id a layer boundary or node of `graph` binds or a mask owns.
inline bool parameterReferenced(const document::CanonicalGraph& graph,
                                const document::ParameterId id) {
    for (const auto& node : graph.nodes())
        for (const auto& binding : node.parameters)
            if (binding.parameterId == id)
                return true;
    for (const auto& layer : graph.layerOutputs()) {
        for (const auto& binding : layer.parameters)
            if (binding.parameterId == id)
                return true;
        for (const auto& mask : layer.masks)
            if (mask.path == id || mask.feather == id || mask.expansion == id || mask.opacity == id)
                return true;
    }
    return false;
}

// An entry layer's copy: a new slot of the same Merge directly below the original, fed by the same
// image and audio sources, holding a copy of the entry layer (fresh layer, slot, parameter, curve
// and mask ids; name + " copy"; parent and matte as the original's). Outputs "layer.<original>",
// "slot.<original slot>" and "parameter.<original>" like DuplicateNodes. Implemented in
// entry_layer_operations.cpp.
struct EntryLayerCopy final {
    document::LayerId layer;
    document::LayerSlotId slot;
};
[[nodiscard]] std::optional<OperationResult>
duplicateEntryLayer(document::Draft& draft, document::CompositionId composition,
                    document::LayerId original, EntryLayerCopy& copy,
                    std::vector<OperationOutput>& outputs);

// Deletes an entry layer: its slot and the slot's edges, its record, its parameters (with their
// curves) and its masks', and every parent or matte reference to it. The Merge and the slot's
// sources stay here; DeleteLayers removes the sources only the deleted layers use (#714).
[[nodiscard]] std::optional<OperationResult> deleteEntryLayer(document::Composition& composition,
                                                              document::LayerId layer);

} // namespace bloom::commands::detail
