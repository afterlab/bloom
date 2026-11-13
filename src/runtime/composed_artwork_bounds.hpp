#pragma once

// Private to src/runtime. #642: the bounds a composed Vector Artwork layer publishes
// (CompiledLayerOutput::composedArtwork), the same on the CPU reference and the GPU route.

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"
#include "layer_parent_transform.hpp"

#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <unordered_map>
#include <variant>
#include <vector>

namespace bloom::runtime::detail {

// #712: what the picture at `index` can show of a composed artwork, as a box in the artwork's own
// space, or nothing when it shows nothing this frame. A leaf (a drawn layer of the artwork, the
// identity under the carrier) shows its published content box. A group's layer and an effect show
// what their input shows, a Merge what its entries show together, and a track matte (an SVG clip
// path or mask, or the outline a gradient is painted through) shows only where its layer and its
// matte source overlap. So a shape larger than its clip path, a gradient larger than the outline
// it fills and a matte source itself add nothing outside what is drawn.
[[nodiscard]] inline std::optional<ContentBounds>
visibleArtworkBounds(const std::span<const CompiledOperation> operations,
                     const std::span<const EvaluatedOperationBounds> bounds,
                     const std::span<const OperationIndex> leaves, const OperationIndex index,
                     std::unordered_map<std::size_t, std::optional<ContentBounds>>& known) {
    if (index.value() >= operations.size())
        return std::nullopt;
    if (const auto found = known.find(index.value()); found != known.end())
        return found->second;
    const auto unite = [](std::optional<ContentBounds>& total, const std::optional<ContentBounds>& part) {
        if (!part)
            return;
        if (!total) {
            total = part;
            return;
        }
        total->left = std::min(total->left, part->left);
        total->top = std::min(total->top, part->top);
        total->right = std::max(total->right, part->right);
        total->bottom = std::max(total->bottom, part->bottom);
    };
    std::optional<ContentBounds> result;
    const auto& operation = operations[index.value()];
    if (std::ranges::find(leaves, index) != leaves.end()) {
        if (index.value() < bounds.size()) {
            const auto& published = bounds[index.value()];
            if (published.layerId.isValid() && !published.local.empty())
                result = ContentBounds{published.local.left, published.local.top,
                                       published.local.right, published.local.bottom};
        }
    } else if (const auto* group = std::get_if<CompiledLayerOutput>(&operation)) {
        result = visibleArtworkBounds(operations, bounds, leaves, group->input, known);
    } else if (const auto* merge = std::get_if<CompiledMerge>(&operation)) {
        for (const auto& entry : merge->entries)
            unite(result, visibleArtworkBounds(operations, bounds, leaves, entry.input, known));
    } else if (const auto* effect = std::get_if<CompiledImageEffect>(&operation)) {
        result = visibleArtworkBounds(operations, bounds, leaves, effect->input, known);
        const auto* combine = std::get_if<ImageCombineKernel>(&effect->kernel);
        // An inverted or driven matte keeps what lies outside its source, so it bounds nothing.
        if (result && effect->trackMatte && effect->secondaryInput && combine != nullptr &&
            !combine->invert && !combine->drivenChannel) {
            const auto matte =
                visibleArtworkBounds(operations, bounds, leaves, *effect->secondaryInput, known);
            if (!matte) {
                result.reset();
            } else {
                result->left = std::max(result->left, matte->left);
                result->top = std::max(result->top, matte->top);
                result->right = std::min(result->right, matte->right);
                result->bottom = std::min(result->bottom, matte->bottom);
                if (result->empty())
                    result.reset();
            }
        }
    }
    known.emplace(index.value(), result);
    return result;
}

// Rewrites `geometry` (the layer's own bounds, at the identity) to its artwork's content bounds
// through the carrier's matrix: `local` is what the artwork shows (visibleArtworkBounds, #712: the
// leaves' content bounds, each clipped by the mattes above it), `polygon` its corners through
// `carrier`, `output` the polygon's box and `anchor` the carrier's pivot. Leaves that published
// nothing this frame are skipped; with none, `geometry` is left as it is. False only when the
// carrier's anchor cannot be resolved.
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
    // #712: the leaves' union is every drawn layer's whole content, matte sources and clipped
    // parts included; what the artwork shows is at most that, read through the mattes.
    {
        std::unordered_map<std::size_t, std::optional<ContentBounds>> known;
        if (const auto visible = visibleArtworkBounds(plan.operations(), bounds, artwork.leaves,
                                                      layer.input, known))
            local = *visible;
    }
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
