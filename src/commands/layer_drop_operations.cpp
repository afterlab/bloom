#include <bloom/commands/layer_drop_operations.hpp>

#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/layer_timing_gesture.hpp>
#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/core/frame_time_mapping.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>

#include <algorithm>
#include <span>

namespace bloom::commands {
namespace {
std::optional<document::LayerId> outputLayer(const OperationResult& result) {
    for (const auto& output : result.outputs)
        if (output.name == "layer")
            if (const auto* id = std::get_if<document::LayerId>(&output.id))
                return *id;
    return std::nullopt;
}

// The composition frame nearest `time`, clamped into the composition: the same snap the Timeline's
// frame math (docs/architecture/animation-and-time.md, "Session Time And Scrubbing") applies.
core::RationalTime frameStart(const document::Composition& composition,
                              const core::RationalTime time) {
    const auto rate = composition.format().frameRate();
    const auto created = core::FrameTimeMapping::create(composition.duration(), rate.numerator(),
                                                        rate.denominator());
    if (!created.hasValue())
        return core::RationalTime{};
    const auto mapped = created.value()->timeForFrame(created.value()->nearestFrameIndex(time));
    return mapped.hasValue() ? *mapped.value() : core::RationalTime{};
}
} // namespace

bool droppableAsLayer(const document::Project& project, const document::CompositionId target,
                      const LayerDropItem& item) {
    if (const auto* composition = std::get_if<document::CompositionId>(&item))
        return *composition != target && project.findComposition(*composition) != nullptr;
    const auto* asset = project.findAsset(std::get<document::AssetId>(item));
    return asset != nullptr &&
           (asset->kind == document::AssetKind::Image ||
            asset->kind == document::AssetKind::Sequence ||
            asset->kind == document::AssetKind::Video || asset->kind == document::AssetKind::Audio);
}

namespace {
// The slot new layers go directly above (nullopt: below every layer).
std::optional<OperationResult> resolveAnchor(const document::Composition& composition,
                                             const LayerDropPlacement& placement,
                                             std::optional<document::LayerSlotId>& anchor) {
    const auto entries = composition.graph().layerStack().entries();
    switch (placement.stack) {
    case LayerDropPlacement::Stack::Top:
        if (!entries.empty())
            anchor = entries.front().slotId;
        break;
    case LayerDropPlacement::Stack::Before: {
        auto before = placement.before;
        if (!before.isValid() && placement.beforeLayer.isValid()) {
            const auto slot = std::ranges::find(entries, placement.beforeLayer,
                                                &document::LayerStackEntry::layerId);
            if (slot != entries.end())
                before = slot->slotId;
        }
        if (composition.graph().layerStack().find(before) == nullptr)
            return OperationResult::rejected(OperationIssueCode::MissingReference,
                                             "The layer to insert above no longer exists");
        anchor = before;
        break;
    }
    case LayerDropPlacement::Stack::Bottom:
        break;
    }
    return std::nullopt;
}

// Moves the new layer above `anchor` and to `start`, the way a Timeline body drag does: start, in
// and out together, on frames, keeping at least one frame inside the composition. `end` receives
// the layer's out point (where a sequenced next layer starts).
OperationResult placeLayer(document::Draft& draft, const document::CompositionId compositionId,
                           const document::LayerId layerId,
                           const std::optional<document::LayerSlotId> anchor,
                           const core::RationalTime start, core::RationalTime& end) {
    const auto* composition = draft.project().findComposition(compositionId);
    const auto* layer = composition ? composition->graph().findLayer(layerId) : nullptr;
    const auto stack = composition ? composition->graph().layerStack().entries()
                                   : std::span<const document::LayerStackEntry>{};
    const auto slot = std::ranges::find(stack, layerId, &document::LayerStackEntry::layerId);
    if (layer == nullptr || slot == stack.end())
        return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                         "The new layer is not in the layer stack");
    auto moved = MoveLayerBefore(compositionId, slot->slotId, anchor).apply(draft);
    if (moved.status == OperationStatus::Rejected)
        return moved;
    composition = draft.project().findComposition(compositionId);
    layer = composition->graph().findLayer(layerId);
    const auto duration = composition->duration();
    const auto proposal =
        proposeLayerTiming({.start = layer->timing.startTime,
                            .in = layer->inPoint,
                            .out = layer->endPoint(duration),
                            .speed = layer->timing.speed,
                            .rate = composition->format().frameRate(),
                            .duration = duration,
                            .extentStart = std::nullopt,
                            .extentEnd = std::nullopt,
                            .handle = 0,
                            .alt = false,
                            .deltaSeconds = start.toSeconds() - layer->inPoint.toSeconds()});
    if (!proposal)
        return OperationResult::rejected(OperationIssueCode::InvalidValue,
                                         "The layer cannot start there");
    auto timed = SetLayerTiming(compositionId, layerId, proposal->start, proposal->in,
                                proposal->out, proposal->speed)
                     .apply(draft);
    if (timed.status == OperationStatus::Rejected)
        return timed;
    end = proposal->out;
    return OperationResult::applied();
}
} // namespace

OperationResult placeDroppedLayer(document::Draft& draft, const document::CompositionId composition,
                                  const document::LayerId layer, const LayerDropPlacement& placement) {
    const auto* target = draft.project().findComposition(composition);
    if (target == nullptr)
        return OperationResult::rejected(
            OperationIssueCode::InvalidTarget,
            "Composition " + std::to_string(composition.value()) + " does not exist");
    // The new layer is already in the stack (on top, usually): the anchor is worked out without it.
    std::optional<document::LayerSlotId> anchor;
    if (placement.stack == LayerDropPlacement::Stack::Top) {
        for (const auto& entry : target->graph().layerStack().entries())
            if (entry.layerId != layer) {
                anchor = entry.slotId;
                break;
            }
    } else if (auto failure = resolveAnchor(*target, placement, anchor)) {
        return std::move(*failure);
    }
    core::RationalTime end;
    return placeLayer(draft, composition, layer, anchor, frameStart(*target, placement.start), end);
}

AddDroppedLayers& AddDroppedLayers::withSvgSources(SvgAssetSources sources) {
    svg_ = std::make_shared<const SvgAssetSources>(std::move(sources));
    return *this;
}

OperationResult AddDroppedLayers::apply(document::Draft& draft) const {
    const auto* composition = draft.project().findComposition(composition_);
    if (composition == nullptr)
        return OperationResult::rejected(
            OperationIssueCode::InvalidTarget,
            "Composition " + std::to_string(composition_.value()) + " does not exist");
    if (items_.empty())
        return OperationResult::rejected(OperationIssueCode::InvalidValue, "Nothing to add");
    for (const auto& item : items_)
        if (!droppableAsLayer(draft.project(), composition_, item))
            return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                             "This item cannot be a layer here");
    // The new layers go directly above `anchor` (nullopt: below every layer), in item order.
    std::optional<document::LayerSlotId> anchor;
    if (auto failure = resolveAnchor(*composition, placement_, anchor))
        return std::move(*failure);
    auto start = frameStart(*composition, placement_.start);
    std::vector<OperationOutput> outputs;
    std::vector<OperationIssue> issues;
    for (const auto& item : items_) {
        OperationResult added;
        if (const auto* nested = std::get_if<document::CompositionId>(&item))
            added = AddCompositionSource(composition_, *nested, {}, true).apply(draft);
        else if (const auto id = std::get<document::AssetId>(item);
                 draft.project().findAsset(id)->kind == document::AssetKind::Audio)
            added = AddAudioLayer(composition_, id).apply(draft);
        else if (document::isSvgSourceAsset(*draft.project().findAsset(id))) {
            // #646: an imported SVG is not footage; its artwork is built through the SVG import.
            const SvgAssetSources none;
            added = addSvgAssetArtwork(draft, composition_, id, svg_ ? *svg_ : none);
        } else
            added = AddImageLayer(composition_, id).apply(draft);
        if (added.status == OperationStatus::Rejected)
            return added;
        issues.insert(issues.end(), added.issues.begin(), added.issues.end());
        const auto layerId = outputLayer(added);
        if (!layerId)
            return OperationResult::rejected(OperationIssueCode::InvalidTarget,
                                             "The new layer is not in the layer stack");
        core::RationalTime end;
        auto placed = placeLayer(draft, composition_, *layerId, anchor, start, end);
        if (placed.status == OperationStatus::Rejected)
            return placed;
        if (placement_.sequence)
            start = end;
        outputs.push_back({"layer", *layerId});
    }
    auto result = OperationResult::applied(std::move(outputs));
    result.issues = std::move(issues);
    return result;
}

} // namespace bloom::commands
