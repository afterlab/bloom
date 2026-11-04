// Mute is the one node-layout field evaluation reads (document/evaluation_inputs.hpp, #399).
[[nodiscard]] bool isMuted(const document::NodeId id) const {
    return document::nodeMuted(composition_->nodeLayout(), id);
}

[[nodiscard]] bool hasImageOutput(const runtime::NodeDefinition& definition) const {
    return std::ranges::any_of(definition.outputs, [](const auto& output) {
        return output.valueKind == runtime::SocketValueKind::Image;
    });
}

[[nodiscard]] std::optional<document::InputPortRef>
firstImageInput(const document::NodeRecord& node) const {
    const auto* definition = registry_.find(node.typeId, node.schemaVersion);
    // The image-output test is what excludes a VALUE node, which carries no pixels for a mute
    // bypass to pass through. The composition Output carries pixels -- it is where they end -- and
    // since task FIX1, item H it publishes no socket at all, so it is named here rather than
    // failing the test and taking its own input edge out of the reachable set when muted.
    // A Mask-only node (#337 Matte Blur) carries its first Mask input through the same way; the
    // twin in document/render_reachability.cpp applies the identical rule.
    if (definition == nullptr)
        return std::nullopt;
    const bool maskOnly =
        !hasImageOutput(*definition) &&
        std::ranges::any_of(definition->outputs, [](const auto& output) {
            return output.valueKind == runtime::SocketValueKind::Mask;
        });
    if (!hasImageOutput(*definition) && !maskOnly &&
        definition->lowering != runtime::NodeLoweringKind::CompositionOutput)
        return std::nullopt;
    const auto carried = maskOnly ? runtime::SocketValueKind::Mask : runtime::SocketValueKind::Image;
    for (const auto& input : definition->inputs) {
        if (input.valueKind == carried)
            return document::NodeInputRef{node.id, input.name};
    }
    const auto* stack = composition_->graph().merge(node.id);
    const auto entries = stack ? stack->entries() : std::span<const document::LayerStackEntry>{};
    if (definition->layerSlotInput &&
        definition->layerSlotInput->valueKind == runtime::SocketValueKind::Image &&
        !entries.empty()) {
        return document::LayerStackInputRef{node.id, entries.front().slotId,
                                            definition->layerSlotInput->role};
    }
    return std::nullopt;
}

// #502: the entry's layer (document::mergeEntryLayer): the Layer node nearest the Merge up its image
// chain, else the entry's own layer, else none (a plain slot, an identity layer at its defaults).
[[nodiscard]] const document::LayerOutputBoundary* entryLayerOf(const document::NodeId merge,
                                                               const document::LayerSlotId slot) const {
    const auto key = std::pair{merge, slot};
    if (const auto found = entryLayerCache_.find(key); found != entryLayerCache_.end())
        return found->second;
    const auto* layer = document::mergeEntryLayer(composition_->graph(), merge, slot, registry_);
    entryLayerCache_.emplace(key, layer);
    return layer;
}

// A disabled, solo-excluded or (Layer node) muted layer contributes nothing to its Merges.
[[nodiscard]] bool mutedLayer(const document::LayerOutputBoundary& layer) const {
    return !layer.enabled || (anySolo_ && !layer.solo) ||
           (!layer.entryOwned() && isMuted(layer.nodeId));
}

// ADR 0028: a layer that is any layer's track matte contributes nothing to its Merge.
[[nodiscard]] bool isMatteSource(const document::LayerId layer) const {
    return matteSources_.contains(layer);
}

// Whether a Merge entry is left out of its Merge's image: its layer is muted, or (unless
// `hideMatteSources` is false, for the audio mix) its layer is a track matte source.
[[nodiscard]] bool entryExcluded(const document::NodeId merge, const document::LayerStackEntry& entry,
                                 const bool hideMatteSources = true) const {
    const auto* layer = entryLayerOf(merge, entry.slotId);
    return layer != nullptr &&
           (mutedLayer(*layer) || (hideMatteSources && isMatteSource(layer->layerId)));
}

// A track matte source's slot carries no pixels, but its audio still plays, so the audio mix asks
// with `hideMatteSources` false.
[[nodiscard]] bool consumesEdge(const document::NodeRecord& node, const document::EdgeRecord& edge,
                                const bool hideMatteSources = true) const {
    if (const auto* merge = composition_->graph().merge(node.id); merge && !merge->enabled())
        return false;
    if (isMuted(node.id)) {
        const auto input = firstImageInput(node);
        if (!input || *input != edge.destination)
            return false;
    }
    if (const auto* slot = std::get_if<document::LayerStackInputRef>(&edge.destination)) {
        const auto* stack = composition_->graph().merge(slot->stackNodeId);
        const auto* entry = stack ? stack->find(slot->slotId) : nullptr;
        if (entry && entryExcluded(slot->stackNodeId, *entry, hideMatteSources))
            return false;
    }
    return true;
}

// #502 Adjustment: the Merge entries the Adjustment's image paths first reach (memoized).
[[nodiscard]] const std::vector<document::MergeEntryRef>&
adjustmentTargetsOf(const document::NodeId adjustment) const {
    if (const auto found = adjustmentTargetCache_.find(adjustment);
        found != adjustmentTargetCache_.end())
        return found->second;
    return adjustmentTargetCache_
        .emplace(adjustment, document::adjustmentTargets(composition_->graph(), adjustment, registry_))
        .first->second;
}

// The one entry an Adjustment serves; none when its chain reaches no entry or several.
[[nodiscard]] std::optional<document::MergeEntryRef>
singleAdjustmentTarget(const document::NodeId adjustment) const {
    const auto& targets = adjustmentTargetsOf(adjustment);
    return targets.size() == 1 ? std::optional(targets.front()) : std::nullopt;
}

// The Adjustment an entry is the adjustment entry of: a reachable, unmuted Adjustment whose only
// target is this entry. Several Adjustments serving one entry all read the same stack below; the
// lowest node id names it.
[[nodiscard]] std::optional<document::NodeId>
entryAdjustment(const document::NodeId merge, const document::LayerSlotId slot) const {
    if (!hasAdjustments_)
        return std::nullopt;
    for (const auto* node : reachableNodes_) {
        if (node->typeId != document::kAdjustmentNodeType || isMuted(node->id))
            continue;
        const auto target = singleAdjustmentTarget(node->id);
        if (target && target->merge == merge && target->slot == slot)
            return node->id;
    }
    return std::nullopt;
}
