// Included inside the snapshot compiler class (snapshot_compiler_lowering.ipp). #502 every Merge
// entry is a layer: Layer nodes, a Merge entry's own layer (LayerOutputBoundary::entryOwned), the
// Merge's entries with their track mattes, and the Adjustment entry that replaces the stack below
// it. docs/architecture/layer-graph-model.md ("Merge", "Adjustment") is the specification.

// The asset a footage-backed entry layer shows: the image or video source its slot's content chain
// starts from through each node's own image input (the rule document::layerSource applies to a
// Layer node's content input).
[[nodiscard]] std::optional<document::AssetId>
entryLayerFootage(const document::LayerOutputBoundary& layer) const {
    const auto& graph = composition_->graph();
    const auto* edge = document::mergeSlotImageEdge(graph, layer.nodeId, *layer.entrySlot);
    const auto* node = edge ? graph.findNode(edge->source.nodeId) : nullptr;
    for (std::size_t step = 0; node != nullptr && step <= graph.nodes().size(); ++step) {
        if (node->typeId == "bloom.image-source" || node->typeId == "bloom.video-source") {
            const auto* text = parameterConstant<std::string>(findParameterBinding(*node, "asset"));
            std::uint64_t raw = 0;
            if (text == nullptr || text->empty() ||
                std::from_chars(text->data(), text->data() + text->size(), raw).ec != std::errc{} ||
                raw == 0)
                return std::nullopt;
            return document::AssetId::fromRaw(raw);
        }
        const auto next = std::ranges::find_if(graph.edges(), [&](const auto& candidate) {
            const auto* input = std::get_if<document::NodeInputRef>(&candidate.destination);
            return input != nullptr && input->nodeId == node->id &&
                   input->port == document::kLayerOutputContentInputPort;
        });
        node = next == graph.edges().end() ? nullptr : graph.findNode(next->source.nodeId);
    }
    return std::nullopt;
}

// #381: the footage's pixel aspect override, relative to its decoded (file) ratio.
[[nodiscard]] double footagePixelAspect(const std::optional<document::AssetId> assetId) const {
    if (!assetId)
        return 1.0;
    const auto* asset = request_.snapshot.project().findAsset(*assetId);
    if (asset == nullptr || !asset->interpretation.pixelAspect)
        return 1.0;
    const auto file = document::footageFilePixelAspect(*asset);
    const auto shown = *asset->interpretation.pixelAspect;
    return (static_cast<double>(shown.numerator()) / shown.denominator()) /
           (static_cast<double>(file.numerator()) / file.denominator());
}

// Any layer's Layer Output over `input`: a Layer node's (owner = the node) or an entry layer's
// (owner = its Merge), with the parameters the layer binds by role.
[[nodiscard]] std::optional<runtime::CompiledLayerOutput>
lowerLayerBoundary(const document::LayerOutputBoundary& boundary, const document::NodeId owner,
                   const runtime::OperationIndex input, const double sourcePixelAspect,
                   const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using namespace document;
    const auto& graph = composition_->graph();
    const auto bound = [&](const std::string_view role) {
        return layerParameterBinding(graph, boundary, role);
    };
    const auto* ownerNode = findNode(owner);
    const auto* positionBinding = bound(kPositionParameterRole);
    const auto* anchorBinding = bound(kAnchorParameterRole);
    const auto* scaleBinding = bound(kScaleParameterRole);
    const auto* rotationBinding = bound(kRotationParameterRole);
    const auto* opacityBinding = bound(kOpacityParameterRole);
    const auto* blendModeBinding = bound(kBlendModeParameterRole);
    // The blend mode lowers like CompiledText's three values: a validated constant read straight
    // off the parameter store, not a curve source, because the schema declares it non-animatable.
    // An integer naming no implemented mode cannot reach here -- ParameterStore refuses it on
    // insert and document validation refuses it on publication -- so failing to resolve one is a
    // topology failure, exactly as a missing transform binding is.
    const auto* storedBlendMode = parameterConstant<std::int64_t>(blendModeBinding);
    const auto blendMode = storedBlendMode == nullptr
                               ? std::nullopt
                               : core::blendModeFromStoredValue(*storedBlendMode);
    // Task DRIVE-1: driven, the mode has no stored constant, so the plan carries the registry
    // default beside the driver and the evaluator reads the driver. A mode is a jump between two
    // named modes rather than a value with a midpoint, which is exactly what a driver expresses.
    const auto drivenBlendMode = drivenOutput(blendModeBinding, runtime::SocketValueKind::Integer);
    if (ownerNode == nullptr ||
        unreadableDriver(*ownerNode, blendModeBinding, runtime::SocketValueKind::Integer,
                         drivenBlendMode))
        return std::nullopt;
    const auto position = compiledVec2Parameter(positionBinding);
    const auto anchor = compiledVec2Parameter(anchorBinding);
    const auto scale = compiledVec2Parameter(scaleBinding);
    const auto rotation = compiledScalarParameter(rotationBinding);
    const auto opacity = compiledScalarParameter(opacityBinding);
    if (positionBinding == nullptr || anchorBinding == nullptr || scaleBinding == nullptr ||
        rotationBinding == nullptr || opacityBinding == nullptr || blendModeBinding == nullptr ||
        !position.has_value() || !anchor.has_value() || !scale.has_value() ||
        !rotation.has_value() || !opacity.has_value() ||
        (!blendMode.has_value() && !drivenBlendMode.has_value())) {
        addTopologyFailure(owner, "Validated Layer Output could not be lowered.");
        return std::nullopt;
    }
    std::optional<runtime::OperationIndex> parentIndex;
    if (boundary.parent) {
        const auto* parent = graph.findLayer(*boundary.parent);
        parentIndex = parent ? layerOperationOf(*parent, indices) : std::nullopt;
        if (!parentIndex) {
            if (!hasFailure_)
                addTopologyFailure(owner, "Layer parent must be lowered before its child.");
            return std::nullopt;
        }
    }
    const auto time = lowerLayerTime(owner, boundary, bound(kTimeRemapParameterRole));
    if (!time)
        return std::nullopt;
    runtime::CompiledLayerOutput result{owner,
                                        boundary.layerId,
                                        input,
                                        *position,
                                        *anchor,
                                        *scale,
                                        *rotation,
                                        *opacity,
                                        blendModeBinding->parameterId,
                                        blendMode.value_or(core::kDefaultBlendMode),
                                        boundary.inPoint,
                                        boundary.endPoint(composition_->duration()),
                                        drivenBlendMode,
                                        parentIndex,
                                        *time,
                                        sourcePixelAspect};
    result.framePivot = boundary.pivot == LayerPivot::Frame;
    // #642: a Vector Artwork layer whose transform its artwork's geometry draws (svg_artwork.hpp).
    if (const auto composed = artwork_.composed.find(boundary.layerId); composed != artwork_.composed.end()) {
        const auto* carrier = graph.findLayer(composed->second.carrier);
        const auto carrierIndex = carrier ? layerOperationOf(*carrier, indices) : std::nullopt;
        if (!carrierIndex) {
            addTopologyFailure(owner, "Vector Artwork layer's transform carrier was not lowered.");
            return std::nullopt;
        }
        runtime::CompiledLayerOutput::ComposedArtwork artwork{*carrierIndex, {}};
        for (const auto leaf : composed->second.leaves)
            if (const auto found = entryLayerOps_.find(leaf); found != entryLayerOps_.end())
                artwork.leaves.push_back(found->second);
        result.composedArtwork = std::move(artwork);
    }
    // #642: a Vector Artwork layer whose artwork is drawn at its output scale, a group's reader
    // that maps its larger picture back, and a layer drawing into such a group (svg_artwork.hpp).
    if (const auto content = artwork_.contentScales.find(boundary.layerId); content != artwork_.contentScales.end())
        result.contentScale = content->second;
    if (const auto output = artwork_.outputScales.find(boundary.layerId); output != artwork_.outputScales.end())
        result.outputScale = output->second;
    return result;
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerLayerOutput(const document::NodeRecord& node,
                 const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices,
                 std::optional<runtime::OperationIndex> emptyInput = std::nullopt) {
    using namespace document;
    const auto input = emptyInput
                           ? emptyInput
                           : findInputOperation(node.id, kLayerOutputContentInputPort, indices);
    const auto boundary = layerOutputs_.find(node.id);
    if (!input || boundary == layerOutputs_.end()) {
        addTopologyFailure(node.id, "Validated Layer Output could not be lowered.");
        return std::nullopt;
    }
    std::optional<AssetId> footage;
    if (const auto source = layerSource(*composition_, *boundary->second))
        if (const auto* assetId = std::get_if<AssetId>(&*source))
            footage = *assetId;
    auto layer = lowerLayerBoundary(*boundary->second, node.id, *input, footagePixelAspect(footage),
                                    indices);
    if (!layer)
        return std::nullopt;
    return runtime::CompiledOperation{std::move(*layer)};
}

// The operation of a layer used as a parent or a matte source: a Layer node's own (absent when it
// draws nothing and is no parent), or an entry layer's, lowered on demand.
[[nodiscard]] std::optional<runtime::OperationIndex>
layerOperationOf(const document::LayerOutputBoundary& layer,
                 const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    if (layer.entryOwned())
        return entryLayerOperation(layer, indices);
    const auto found = indices.find(layer.nodeId);
    return found == indices.end() ? std::nullopt : std::optional(found->second);
}

// A frame-sized opaque white image owned by `owner`, as the Mask node's canvas: synthesized
// operands name no document parameter.
[[nodiscard]] runtime::OperationIndex appendWhiteFrame(const document::NodeId owner) {
    auto& operations = *loweringOperations_;
    const auto& format = composition_->format();
    const auto index = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(runtime::CompiledSolid{
        owner,
        runtime::CompiledColorParameter{document::ParameterId{}, core::Color4d{1.0, 1.0, 1.0, 1.0}},
        runtime::CompiledScalarParameter{document::ParameterId{},
                                         static_cast<double>(format.width())},
        runtime::CompiledScalarParameter{document::ParameterId{},
                                         static_cast<double>(format.height())}});
    return index;
}

// #502: an entry layer's Layer Output, once per layer: over its slot's image content (masked in
// content space first), over a transparent image when its slot draws nothing (a parent still owns
// its transform), or -- for an adjustment entry -- over the white frame its coverage is.
[[nodiscard]] std::optional<runtime::OperationIndex>
entryLayerOperation(const document::LayerOutputBoundary& layer,
                    const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    if (const auto found = entryLayerOps_.find(layer.layerId); found != entryLayerOps_.end())
        return found->second;
    if (!entryLayersInProgress_.insert(layer.layerId).second) {
        addTopologyFailure(layer.nodeId, "Entry layer parents form a cycle.");
        return std::nullopt;
    }
    auto& operations = *loweringOperations_;
    const auto merge = layer.nodeId;
    const auto slot = *layer.entrySlot;
    const bool adjustment = entryAdjustment(merge, slot).has_value();
    std::optional<runtime::OperationIndex> content;
    double pixelAspect = 1.0;
    if (adjustment) {
        content = appendWhiteFrame(merge);
    } else if (const auto* edge =
                   layerSlotInputEdge(merge, slot, document::kLayerStackContentInputRole);
               edge != nullptr && !emptyImages_.contains(edge->source.nodeId)) {
        content = sourceOperation(edge->source, indices);
        if (!content) {
            addTopologyFailure(merge, "Merge entry content was not lowered before its layer.");
            return std::nullopt;
        }
        pixelAspect = footagePixelAspect(entryLayerFootage(layer));
    }
    if (!content) {
        content = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledMerge{merge, {}});
    } else if (!layer.masks.empty()) {
        // ADR 0028 layer masks: in content space, before the layer transforms it.
        content = lowerMaskedContent(merge, layer, *content, indices);
        if (!content)
            return std::nullopt;
    }
    auto lowered = lowerLayerBoundary(layer, merge, *content, pixelAspect, indices);
    if (!lowered)
        return std::nullopt;
    const auto index = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(std::move(*lowered));
    entryLayerOps_.emplace(layer.layerId, index);
    entryLayersInProgress_.erase(layer.layerId);
    return index;
}

// ADR 0028 track matte: the entry's image times its matte source layer's coverage (an absent
// source is a transparent matte). The Merge reads the target layer's blend mode and clock through
// it (runtime::mergeEntryLayerIndex).
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerTrackMatte(const document::NodeId merge, const document::LayerOutputBoundary& target,
                const runtime::OperationIndex input,
                const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    auto& operations = *loweringOperations_;
    const auto owner = target.entryOwned() ? merge : target.nodeId;
    const auto* source = composition_->graph().findLayer(target.trackMatte->source);
    std::optional<runtime::OperationIndex> matte;
    // #383: a node matte is that Mask output, in composition space and time.
    if (target.trackMatte->node) {
        matte = matteNodeOperation(owner, *target.trackMatte->node, indices);
        if (!matte)
            return std::nullopt;
    } else if (source != nullptr) {
        matte = layerOperationOf(*source, indices);
        if (!matte && hasFailure_)
            return std::nullopt;
    }
    if (!matte) {
        matte = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledMerge{owner, {}});
    }
    runtime::CompiledImageEffect effect{
        owner,
        input,
        runtime::ImageCombineKernel{runtime::ImageCombineMode::MultiplyAlpha,
                                    runtime::CompiledScalarParameter{document::ParameterId{}, 1.0},
                                    target.trackMatte->channel == document::TrackMatteChannel::Luma
                                        ? render::ImageCoverageChannel::Luma
                                        : render::ImageCoverageChannel::Alpha,
                                    std::nullopt, target.trackMatte->inverted},
        false,
        false,
        *matte};
    effect.trackMatte = true;
    const auto index = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(std::move(effect));
    return index;
}

// The layer's authored blend mode when it is a constant; a driven mode (a Layer node's only) is
// read as Normal where only a constant can be honoured, and the artist is told.
[[nodiscard]] core::BlendMode constantBlendMode(const document::LayerOutputBoundary& layer,
                                                const document::NodeId owner) {
    const auto* binding = document::layerParameterBinding(composition_->graph(), layer,
                                                          document::kBlendModeParameterRole);
    const auto* stored = parameterConstant<std::int64_t>(binding);
    if (const auto mode = stored ? core::blendModeFromStoredValue(*stored) : std::nullopt)
        return *mode;
    if (binding != nullptr) {
        auto diagnosticSubject = subject(owner, "parameter.blendMode");
        diagnosticSubject.parameterId = binding->parameterId;
        diagnosticSubject.layerId = layer.layerId;
        addWarning(runtime::CompileDiagnosticCode::UnsupportedParameterSource,
                   std::move(diagnosticSubject),
                   "A driven blend mode is drawn Normal on this Merge entry",
                   "Only a constant blend mode applies where the layer is not the entry's "
                   "direct image or the entry is an adjustment entry.");
    }
    return core::BlendMode::Normal;
}

// #502: one Merge's entries from position `first` (topmost first) to its bottom, each through its
// own layer and track matte. The first adjustment entry met becomes one plain entry holding its
// composite, which replaces every entry below it.
[[nodiscard]] std::optional<std::vector<runtime::CompiledMergeInput>>
lowerMergeEntries(const document::NodeId merge, const runtime::NodeDefinition& definition,
                  const std::size_t first,
                  const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto* stack = composition_->graph().merge(merge);
    const auto& layerSlotInput = *definition.layerSlotInput;
    const auto slots = stack->entries();
    struct Lowered {
        const document::LayerStackEntry* entry;
        const document::LayerOutputBoundary* layer;
        runtime::OperationIndex input;
        bool composite;
    };
    std::vector<Lowered> lowered;
    for (auto position = first; position < slots.size(); ++position) {
        const auto& entry = slots[position];
        if ((isMuted(merge) && entry.slotId != slots.front().slotId) || entryExcluded(merge, entry))
            continue;
        if (cancelled())
            return std::nullopt;
        const auto* edge = layerSlotInputEdge(merge, entry.slotId, layerSlotInput.role);
        if (edge == nullptr) {
            // An audio-only layer occupies the same stable stack slot but has no image operation.
            // Its typed audio edge is lowered by lowerAudioMix(), so it is intentionally absent
            // from the image merge entries.
            if (definition.audioLayerSlotInput.has_value() &&
                layerSlotInputEdge(merge, entry.slotId, definition.audioLayerSlotInput->role) !=
                    nullptr)
                continue;
            addTopologyFailure(merge, "Validated Layer Stack input could not be lowered.");
            return std::nullopt;
        }
        if (emptyImages_.contains(edge->source.nodeId))
            continue;
        if (const auto adjustment = entryAdjustment(merge, entry.slotId)) {
            const auto composite =
                lowerAdjustmentComposite(merge, entry, *edge, *adjustment, definition, indices);
            if (!composite)
                return std::nullopt;
            lowered.push_back({&entry, nullptr, *composite, true});
            break;
        }
        const auto* layer = entryLayerOf(merge, entry.slotId);
        std::optional<runtime::OperationIndex> input;
        // #343: a track matte source layer is lowered before the layer it mattes, so the matte's
        // image exists when that layer is drawn and bounds what of it is drawn
        // (runtime::detail::matteBoundedLayers). lowerTrackMatte reuses this operation.
        if (layer != nullptr && layer->trackMatte && !layer->trackMatte->node) {
            if (const auto* source = composition_->graph().findLayer(layer->trackMatte->source);
                source != nullptr && !layerOperationOf(*source, indices) && hasFailure_)
                return std::nullopt;
        }
        if (layer != nullptr && layer->entryOwned()) {
            input = entryLayerOperation(*layer, indices);
            if (!input)
                return std::nullopt;
        } else if (const auto source = indices.find(edge->source.nodeId); source != indices.end()) {
            input = source->second;
        } else {
            addTopologyFailure(merge, "Layer Stack input operation is unavailable.");
            return std::nullopt;
        }
        lowered.push_back({&entry, layer, *input, false});
    }
    // ADR 0028 track mattes, after every entry's own layer.
    for (auto& item : lowered) {
        if (item.composite || item.layer == nullptr || !item.layer->trackMatte)
            continue;
        const auto matted = lowerTrackMatte(merge, *item.layer, item.input, indices);
        if (!matted)
            return std::nullopt;
        item.input = *matted;
    }
    std::vector<runtime::CompiledMergeInput> entries;
    entries.reserve(lowered.size());
    for (const auto& item : lowered) {
        if (item.composite || item.layer == nullptr) {
            entries.push_back({item.entry->slotId, {}, item.input});
            continue;
        }
        runtime::CompiledMergeInput input{item.entry->slotId, item.layer->layerId, item.input};
        // A chain the evaluator cannot walk back to the layer's Layer Output (a recipe or channel
        // operation between them) composites as a plain image with the layer's constant mode.
        if (!std::holds_alternative<runtime::CompiledLayerOutput>(
                (*loweringOperations_)[runtime::mergeEntryLayerIndex(*loweringOperations_, input)
                                           .value()])) {
            const auto mode = constantBlendMode(*item.layer, merge);
            input.layerId = {};
            if (mode != core::BlendMode::Normal)
                input.blendMode = mode;
        }
        entries.push_back(input);
    }
    return entries;
}

[[nodiscard]] std::optional<runtime::CompiledOperation>
lowerLayerStack(const document::NodeRecord& node, const runtime::NodeDefinition& definition,
                const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    if (!definition.layerSlotInput.has_value()) {
        addTopologyFailure(node.id, "Layer Stack definition has no slot-input contract.");
        return std::nullopt;
    }
    if (!composition_->graph().merge(node.id)->enabled())
        return runtime::CompiledMerge{node.id, {}};
    auto entries = lowerMergeEntries(node.id, definition, 0, indices);
    if (!entries)
        return std::nullopt;
    return runtime::CompiledMerge{node.id, std::move(*entries)};
}

// #502 Adjustment: whether an adjustment entry's coverage is statically the whole frame at every
// time -- no layer, or one at its identity defaults with no masks, matte, parent or range -- so its
// composite is its image alone.
[[nodiscard]] bool coversWholeFrame(const document::LayerOutputBoundary* layer) {
    if (layer == nullptr)
        return true;
    const auto duration = composition_->duration();
    if (!layer->masks.empty() || layer->trackMatte || layer->parent ||
        core::RationalTime{} < layer->inPoint || layer->endPoint(duration) < duration)
        return false;
    const auto& graph = composition_->graph();
    const auto vector = [&](const std::string_view role) {
        return parameterConstant<document::Vec2d>(
            document::layerParameterBinding(graph, *layer, role));
    };
    const auto scalar = [&](const std::string_view role) {
        return parameterConstant<double>(document::layerParameterBinding(graph, *layer, role));
    };
    const auto* position = vector(document::kPositionParameterRole);
    const auto* anchor = vector(document::kAnchorParameterRole);
    const auto* scale = vector(document::kScaleParameterRole);
    const auto* rotation = scalar(document::kRotationParameterRole);
    const auto* opacity = scalar(document::kOpacityParameterRole);
    if (position == nullptr || anchor == nullptr || scale == nullptr || rotation == nullptr ||
        opacity == nullptr)
        return false;
    // The coverage is the white frame, whose centre is the frame's under either pivot.
    const auto& format = composition_->format();
    const double centreX = static_cast<double>(format.width()) / 2.0;
    const double centreY = static_cast<double>(format.height()) / 2.0;
    return *opacity == 1.0 && *rotation == 0.0 && scale->x == 1.0 && scale->y == 1.0 &&
           position->x == centreX + anchor->x && position->y == centreY + anchor->y;
}

// #502 Adjustment: the entry REPLACES the stack below it, B (the Adjustment's own image):
//   out = lerp(B, X', k), X' = X (Normal) or X blended over B with the layer's mode,
// where X is the entry's image and k its coverage -- the entry layer's opacity, masks, matte and
// range, placed by its transform: K = white frame -> masks -> Layer Output -> matte. Lowered with
// existing operations only: out = Image Mix(base X', second B, factor 1, mask K-inverse) with
// K-inverse = Merge[{K, Stencil}, {white, Normal}]. Coverage that is statically whole is X'.
[[nodiscard]] std::optional<runtime::OperationIndex> lowerAdjustmentComposite(
    const document::NodeId merge, const document::LayerStackEntry& entry,
    const document::EdgeRecord& edge, const document::NodeId adjustment,
    const runtime::NodeDefinition& definition,
    const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto key = std::pair{merge, entry.slotId};
    if (const auto found = adjustmentComposites_.find(key); found != adjustmentComposites_.end())
        return found->second;
    static_cast<void>(definition);
    auto& operations = *loweringOperations_;
    const auto below = indices.find(adjustment);
    if (below == indices.end()) {
        addTopologyFailure(adjustment, "Adjustment was not lowered before its entry.");
        return std::nullopt;
    }
    const auto stackBelow = below->second;
    const auto* layer = entryLayerOf(merge, entry.slotId);
    // A Layer node feeding the slot directly is the entry's layer: its values place the coverage,
    // not the adjusted image, exactly as the same values on the entry would.
    std::optional<runtime::OperationIndex> image;
    if (layer != nullptr && !layer->entryOwned() && edge.source.nodeId == layer->nodeId)
        image = findInputOperation(layer->nodeId, document::kLayerOutputContentInputPort, indices);
    else
        image = sourceOperation(edge.source, indices);
    if (!image) {
        addTopologyFailure(merge, "Adjustment entry image was not lowered.");
        return std::nullopt;
    }
    auto adjusted = *image;
    if (layer != nullptr) {
        if (const auto mode = constantBlendMode(*layer, merge); mode != core::BlendMode::Normal) {
            adjusted = runtime::OperationIndex::fromRaw(operations.size());
            operations.emplace_back(
                runtime::CompiledMerge{merge, {{{}, {}, *image, mode}, {{}, {}, stackBelow}}});
        }
    }
    auto result = adjusted;
    if (!coversWholeFrame(layer)) {
        // The coverage K: the white frame through the entry layer, then its matte.
        std::optional<runtime::OperationIndex> coverage;
        auto white = runtime::OperationIndex::fromRaw(0);
        if (layer->entryOwned()) {
            coverage = entryLayerOperation(*layer, indices);
            if (!coverage)
                return std::nullopt;
            const auto* lowered =
                std::get_if<runtime::CompiledLayerOutput>(&operations[coverage->value()]);
            auto content = lowered->input;
            if (const auto* masked =
                    std::get_if<runtime::CompiledImageEffect>(&operations[content.value()]))
                content = masked->input;
            white = content;
        } else {
            white = appendWhiteFrame(merge);
            auto content = white;
            if (!layer->masks.empty()) {
                const auto masked = lowerMaskedContent(merge, *layer, white, indices);
                if (!masked)
                    return std::nullopt;
                content = *masked;
            }
            auto lowered = lowerLayerBoundary(*layer, merge, content, 1.0, indices);
            if (!lowered)
                return std::nullopt;
            coverage = runtime::OperationIndex::fromRaw(operations.size());
            operations.emplace_back(std::move(*lowered));
        }
        if (layer->trackMatte) {
            coverage = lowerTrackMatte(merge, *layer, *coverage, indices);
            if (!coverage)
                return std::nullopt;
        }
        const auto inverse = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledMerge{
            merge, {{{}, {}, *coverage, core::BlendMode::Stencil}, {{}, {}, white}}});
        runtime::CompiledImageEffect mix{
            merge,
            adjusted,
            runtime::ImageCombineKernel{
                runtime::ImageCombineMode::Mix,
                runtime::CompiledScalarParameter{document::ParameterId{}, 1.0},
                render::ImageCoverageChannel::Alpha},
            false,
            false,
            stackBelow};
        mix.maskInput = inverse;
        result = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(std::move(mix));
    }
    adjustmentComposites_.emplace(key, result);
    return result;
}

// #502 Adjustment: its operation is the stack below its entry -- a Merge of the entries below,
// lowered with every per-entry rule (a lower adjustment entry composites its own stack below).
[[nodiscard]] std::optional<runtime::OperationIndex> lowerStackBelowAdjustment(
    const document::NodeId adjustment,
    const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto target = singleAdjustmentTarget(adjustment);
    if (!target) {
        addTopologyFailure(adjustment, "Adjustment target disappeared before lowering.");
        return std::nullopt;
    }
    const auto key = std::pair{target->merge, target->slot};
    if (const auto found = stacksBelow_.find(key); found != stacksBelow_.end())
        return found->second;
    auto& operations = *loweringOperations_;
    const auto* stack = composition_->graph().merge(target->merge);
    const auto* mergeNode = findNode(target->merge);
    const auto* definition =
        mergeNode ? registry_.find(mergeNode->typeId, mergeNode->schemaVersion) : nullptr;
    if (stack == nullptr || definition == nullptr || !definition->layerSlotInput) {
        addTopologyFailure(adjustment, "Adjustment target is not a Merge entry.");
        return std::nullopt;
    }
    std::vector<runtime::CompiledMergeInput> entries;
    if (stack->enabled()) {
        const auto slots = stack->entries();
        const auto position =
            std::ranges::find(slots, target->slot, &document::LayerStackEntry::slotId);
        auto lowered = lowerMergeEntries(
            target->merge, *definition,
            static_cast<std::size_t>(std::distance(slots.begin(), position)) + 1, indices);
        if (!lowered)
            return std::nullopt;
        entries = std::move(*lowered);
    }
    const auto index = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(runtime::CompiledMerge{adjustment, std::move(entries)});
    stacksBelow_.emplace(key, index);
    return index;
}
