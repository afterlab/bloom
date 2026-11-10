#pragma once

// Private to src/runtime. #642: the bounds a composed Vector Artwork layer publishes
// (CompiledLayerOutput::composedArtwork), the same on the CPU reference and the GPU route.

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"
#include "layer_parent_transform.hpp"

#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <vector>

namespace bloom::runtime::detail {

// Rewrites `geometry` (the layer's own bounds, at the identity) to its artwork's content bounds
// through the carrier's matrix: `local` is the union of the leaves' content bounds, `polygon` its
// corners through `carrier`, `output` the polygon's box and `anchor` the carrier's pivot. Leaves
// that published nothing this frame are skipped; with none, `geometry` is left as it is. False
// only when the carrier's anchor cannot be resolved.
[[nodiscard]] inline bool publishComposedArtworkBounds(const CompiledLayerOutput& layer,
                                                       const CompiledCompositionPlan& plan,
                                                       const ResolvedEvaluation& frameResolved,
                                                       std::span<const EvaluatedOperationBounds> bounds,
                                                       const LayerMatrix& carrier,
                                                       EvaluatedOperationBounds& geometry) {
    if (!layer.composedArtwork)
        return true;
    const auto& artwork = *layer.composedArtwork;
    ContentBounds local;
    bool any = false;
    for (const auto leaf : artwork.leaves) {
        if (leaf.value() >= bounds.size())
            continue;
        const auto& published = bounds[leaf.value()];
        if (!published.layerId.isValid() || published.local.empty())
            continue;
        if (!any) {
            local = {published.local.left, published.local.top, published.local.right, published.local.bottom};
            any = true;
            continue;
        }
        local.left = std::min(local.left, published.local.left);
        local.top = std::min(local.top, published.local.top);
        local.right = std::max(local.right, published.local.right);
        local.bottom = std::max(local.bottom, published.local.bottom);
    }
    if (!any)
        return true;
    const auto* carrierLayer = artwork.carrier.value() < plan.operations().size()
                                   ? std::get_if<CompiledLayerOutput>(&plan.operations()[artwork.carrier.value()])
                                   : nullptr;
    if (carrierLayer == nullptr)
        return false;
    const auto anchor = resolveParameter(carrierLayer->anchor, plan,
                                         resolvedForClock(frameResolved, plan.operationClock(artwork.carrier)));
    if (!anchor)
        return false;
    geometry.local = local;
    geometry.polygon = boundsCorners(local);
    for (auto& point : geometry.polygon)
        point = carrier.map(point);
    geometry.output = {geometry.polygon[0].x, geometry.polygon[0].y, geometry.polygon[0].x, geometry.polygon[0].y};
    for (const auto point : geometry.polygon) {
        geometry.output.left = std::min(geometry.output.left, point.x);
        geometry.output.top = std::min(geometry.output.top, point.y);
        geometry.output.right = std::max(geometry.output.right, point.x);
        geometry.output.bottom = std::max(geometry.output.bottom, point.y);
    }
    geometry.anchor = carrier.map({static_cast<double>(plan.format().width()) / 2.0 + anchor->value.x,
                                   static_cast<double>(plan.format().height()) / 2.0 + anchor->value.y});
    return true;
}

// #642: the bounds a frame publishes for a layer drawing into an oversampled group
// (CompiledLayerOutput::outputScale). While the frame is evaluated its output box, polygon and
// anchor are in the group's larger picture, where the Merge and the group's reader read them; what
// the frame publishes is what the layer would publish at a scale of 1 (its local box is its
// content's, which the scale does not touch). Run once, on the finished bounds, by the CPU
// reference and the GPU preparation alike.
inline void publishOutputScaledBounds(const CompiledCompositionPlan& plan,
                                      std::vector<EvaluatedOperationBounds>& bounds) {
    const auto operations = plan.operations();
    for (std::size_t index = 0; index < operations.size() && index < bounds.size(); ++index) {
        const auto* layer = std::get_if<CompiledLayerOutput>(&operations[index]);
        if (layer == nullptr || layer->outputScale == 1.0 || !bounds[index].layerId.isValid())
            continue;
        const double factor = layer->outputScale;
        auto& geometry = bounds[index];
        geometry.output = {geometry.output.left / factor, geometry.output.top / factor,
                           geometry.output.right / factor, geometry.output.bottom / factor,
                           geometry.output.pivotX ? std::optional(*geometry.output.pivotX / factor)
                                                  : std::nullopt};
        for (auto& point : geometry.polygon)
            point = {point.x / factor, point.y / factor};
        geometry.anchor = {geometry.anchor.x / factor, geometry.anchor.y / factor};
    }
}

} // namespace bloom::runtime::detail
