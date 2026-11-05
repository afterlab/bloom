// Included inside the snapshot compiler class (snapshot_compiler.cpp). #383 matte workflow: Mask
// Combine, Image to Mask, the effect mask, matte layer masks and node track mattes. Everything
// lowers to the existing Channel Select, Multiply Alpha and Image Mix combines, so the CPU reference
// and the GPU route are exactly those kernels'. docs/architecture/image-filters.md ("Matte
// workflow") is the specification.

[[nodiscard]] runtime::OperationIndex appendMatteOperation(runtime::CompiledOperation operation) {
    auto& operations = *loweringOperations_;
    const auto index = runtime::OperationIndex::fromRaw(operations.size());
    operations.push_back(std::move(operation));
    return index;
}

// Zero coverage: the transparent image an empty or absent matte is.
[[nodiscard]] runtime::OperationIndex appendEmptyMatte(const document::NodeId owner) {
    return appendMatteOperation(runtime::CompiledMerge{owner, {}});
}

// Opaque white over `image`'s data window (a Channel Select of four ones).
[[nodiscard]] runtime::OperationIndex appendWhiteOver(const document::NodeId owner,
                                                      const runtime::OperationIndex image) {
    using Source = runtime::ImageChannelSource;
    return appendMatteOperation(runtime::CompiledImageEffect{
        owner, image, runtime::ChannelSelectKernel{{Source::One, Source::One, Source::One, Source::One}}});
}

// `base` times the selected coverage of `matte` times `factor` (Multiply Alpha).
[[nodiscard]] runtime::OperationIndex
appendMatteMultiply(const document::NodeId owner, const runtime::OperationIndex base,
                    const runtime::OperationIndex matte, const render::MatteSelection selection,
                    const runtime::CompiledScalarParameter factor = {document::ParameterId{},
                                                                     1.0}) {
    return appendMatteOperation(runtime::CompiledImageEffect{
        owner, base,
        runtime::ImageCombineKernel{runtime::ImageCombineMode::MultiplyAlpha, factor,
                                    selection.channel, std::nullopt, selection.invert},
        false, false, matte});
}

// lerp(base, second, coverage of mask): Image Mix at factor one with a Mask.
[[nodiscard]] runtime::OperationIndex appendMaskedMix(const document::NodeId owner,
                                                      const runtime::OperationIndex base,
                                                      const runtime::OperationIndex second,
                                                      const runtime::OperationIndex mask) {
    runtime::CompiledImageEffect mix{
        owner, base,
        runtime::ImageCombineKernel{runtime::ImageCombineMode::Mix,
                                    runtime::CompiledScalarParameter{document::ParameterId{}, 1.0},
                                    render::ImageCoverageChannel::Alpha},
        false, false, second};
    mix.maskInput = mask;
    return appendMatteOperation(std::move(mix));
}

// The accumulated coverage `a` (a Mask over `white`'s window, `white` its opaque white) combined
// with the coverage `c` by an ADR 0028 mode:
//   Add a + c - a c = lerp(a, 1, c); Subtract a (1 - c); Intersect a c;
//   Difference a + c - 2 a c = lerp(a, 1 - a, c).
[[nodiscard]] runtime::OperationIndex appendMaskCombine(const document::NodeId owner,
                                                        const document::LayerMaskMode mode,
                                                        const runtime::OperationIndex a,
                                                        const runtime::OperationIndex c,
                                                        const runtime::OperationIndex white) {
    using Channel = render::ImageCoverageChannel;
    switch (mode) {
    case document::LayerMaskMode::Add:
        return appendMaskedMix(owner, a, white, c);
    case document::LayerMaskMode::Subtract:
        return appendMatteMultiply(owner, a, c, {Channel::Alpha, true});
    case document::LayerMaskMode::Intersect:
        return appendMatteMultiply(owner, a, c, {Channel::Alpha, false});
    case document::LayerMaskMode::Difference:
        return appendMaskedMix(owner, a, appendMatteMultiply(owner, white, a, {Channel::Alpha, true}),
                               c);
    case document::LayerMaskMode::None:
        break;
    }
    return a;
}

// The operation behind a Mask output a layer's matte names: zero coverage when its node draws
// nothing.
[[nodiscard]] std::optional<runtime::OperationIndex>
matteNodeOperation(const document::NodeId owner, const document::OutputPortRef& matte,
                   const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    if (emptyImages_.contains(matte.nodeId))
        return appendEmptyMatte(owner);
    if (const auto found = sourceOperation(matte, indices))
        return found;
    addTopologyFailure(owner, "A matte node was not lowered before the layer it mattes.");
    return std::nullopt;
}

// The operation feeding `port` of `nodeId`, or zero coverage when nothing (or an empty image)
// does.
[[nodiscard]] std::optional<runtime::OperationIndex>
matteInputOperation(const document::NodeId nodeId, const std::string_view port,
                    const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto* edge = fixedInputEdge(nodeId, port);
    if (edge == nullptr || emptyImages_.contains(edge->source.nodeId))
        return appendEmptyMatte(nodeId);
    if (const auto found = sourceOperation(edge->source, indices))
        return found;
    addTopologyFailure(nodeId, "A matte input was not lowered before its node.");
    return std::nullopt;
}

// Mask Combine: both Masks placed on the composition canvas, then combined by the mode. Image to
// Mask: the selected coverage of the image over the canvas.
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerMatteNode(const document::NodeRecord& node,
               const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using Channel = render::ImageCoverageChannel;
    if (node.typeId == document::kMaskCombineNodeType) {
        const auto* stored =
            parameterConstant<std::int64_t>(findParameterBinding(node, document::kMaskCombineModeRole));
        if (stored == nullptr || *stored < 0 || *stored >= document::kMaskCombineModeCount) {
            addTopologyFailure(node.id, "Mask Combine mode could not be lowered.");
            return std::nullopt;
        }
        static constexpr std::array kModes{
            document::LayerMaskMode::Add, document::LayerMaskMode::Subtract,
            document::LayerMaskMode::Intersect, document::LayerMaskMode::Difference};
        const auto a = matteInputOperation(node.id, document::kMaskCombineFirstInputPort, indices);
        const auto b = a ? matteInputOperation(node.id, document::kMaskCombineSecondInputPort, indices)
                         : std::nullopt;
        if (!a || !b)
            return std::nullopt;
        const auto canvas = appendWhiteFrame(node.id);
        const auto placed = appendMatteMultiply(node.id, canvas, *a, {Channel::Alpha, false});
        return appendMaskCombine(node.id, kModes[static_cast<std::size_t>(*stored)], placed, *b,
                                 canvas);
    }
    const auto* stored =
        parameterConstant<std::int64_t>(findParameterBinding(node, document::kImageToMaskChannelRole));
    const auto selection = stored ? render::matteSelectionFromStoredValue(*stored) : std::nullopt;
    const auto input = selection
                           ? matteInputOperation(node.id, document::kImageToMaskInputPort, indices)
                           : std::nullopt;
    if (!selection || !input) {
        if (!hasFailure_)
            addTopologyFailure(node.id, "Image to Mask could not be lowered.");
        return std::nullopt;
    }
    return appendMatteMultiply(node.id, appendWhiteFrame(node.id), *input, *selection);
}

// The effect mask: the effect's result where the mask covers it and its own input elsewhere.
// A grade or colour effect keeps its input's data window, so it is one Image Mix,
//   lerp(input, effect, m) = Image Mix(input, effect, mask: m),
// with the selector applied first (Multiply Alpha of white) unless it is plain Alpha. A filter may
// grow the window (a blur's halo), so the effect stays the base there:
//   lerp(effect, input, 1 - m) = Image Mix(effect, input, mask: white(effect) x (1 - m)).
// `effect` is returned when no mask is connected.
[[nodiscard]] std::optional<runtime::OperationIndex>
applyEffectMask(const document::NodeRecord& node, const runtime::OperationIndex effect,
                const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto* edge = fixedInputEdge(node.id, document::kEffectMaskInputPort);
    if (edge == nullptr)
        return effect;
    const auto* stored = parameterConstant<std::int64_t>(
        findParameterBinding(node, document::kEffectMaskChannelRole));
    auto selection = render::matteSelectionFromStoredValue(stored ? *stored : 0);
    auto original = findInputOperation(node.id, "input", indices);
    if (!original)
        if (const auto* lowered =
                std::get_if<runtime::CompiledImageEffect>(&(*loweringOperations_)[effect.value()]))
            original = lowered->input;
    const auto mask = matteInputOperation(node.id, document::kEffectMaskInputPort, indices);
    const auto* definition = registry_.find(node.typeId, node.schemaVersion);
    if (!selection || !original || !mask || definition == nullptr) {
        if (!hasFailure_)
            addTopologyFailure(node.id, "The effect mask could not be lowered.");
        return std::nullopt;
    }
    // A Mask is read as its coverage; only the selector's invert applies to one.
    if (composition_->graph().outputKind(edge->source, registry_) ==
        runtime::SocketValueKind::Mask)
        selection->channel = render::ImageCoverageChannel::Alpha;
    if (definition->category != document::NodeCategory::Filters) {
        auto coverage = *mask;
        if (selection->channel != render::ImageCoverageChannel::Alpha || selection->invert)
            coverage = appendMatteMultiply(node.id, appendWhiteOver(node.id, *original), *mask,
                                           *selection);
        return appendMaskedMix(node.id, *original, effect, coverage);
    }
    const auto keep = appendMatteMultiply(node.id, appendWhiteOver(node.id, effect), *mask,
                                          {selection->channel, !selection->invert});
    return appendMaskedMix(node.id, effect, *original, keep);
}

// ADR 0028 layer masks on `content`, in content space. Path masks alone are the one LayerMaskKernel
// they always were. With a matte mask each mask's coverage is its own Mask over the content's
// window -- a path mask the kernel over white, a matte mask its node's Mask times its invert and
// opacity -- accumulated in list order by the same modes (a leading run of path masks is still one
// kernel), and the content is multiplied by the result.
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerMaskedContent(const document::NodeId owner, const document::LayerOutputBoundary& boundary,
                   const runtime::OperationIndex content,
                   const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using Channel = render::ImageCoverageChannel;
    const auto& masks = boundary.masks;
    if (std::ranges::none_of(masks, [](const auto& mask) { return mask.matte.has_value(); })) {
        auto kernel = lowerLayerMasks(boundary);
        if (!kernel)
            return std::nullopt;
        return appendMatteOperation(runtime::CompiledImageEffect{owner, content, std::move(*kernel)});
    }
    const auto white = appendWhiteOver(owner, content);
    const auto compileMask = [&](const document::LayerMask& mask, const document::LayerMaskMode mode)
        -> std::optional<runtime::CompiledLayerMask> {
        auto compiled = compiledLayerMask(mask.id, mode, mask.inverted, mask.path, mask.feather,
                                          mask.expansion, mask.opacity);
        if (!compiled)
            addTopologyFailure(owner, "Validated layer mask could not be lowered.");
        return compiled;
    };
    std::optional<runtime::OperationIndex> accumulated;
    std::size_t next = 0;
    const auto leading = static_cast<std::size_t>(std::distance(
        masks.begin(),
        std::ranges::find_if(masks, [](const auto& mask) { return mask.matte.has_value(); })));
    if (std::any_of(masks.begin(), masks.begin() + static_cast<std::ptrdiff_t>(leading),
                    [](const auto& mask) { return mask.mode != document::LayerMaskMode::None; })) {
        runtime::LayerMaskKernel kernel;
        for (; next < leading; ++next) {
            auto compiled = compileMask(masks[next], masks[next].mode);
            if (!compiled)
                return std::nullopt;
            kernel.masks.push_back(std::move(*compiled));
        }
        accumulated =
            appendMatteOperation(runtime::CompiledImageEffect{owner, white, std::move(kernel)});
    }
    for (; next < masks.size(); ++next) {
        const auto& mask = masks[next];
        if (mask.mode == document::LayerMaskMode::None)
            continue;
        runtime::OperationIndex coverage = white;
        if (mask.matte) {
            const auto opacity = compiledScalarParameterFor(mask.opacity);
            const auto matte = matteNodeOperation(owner, *mask.matte, indices);
            if (!opacity || !matte) {
                if (!hasFailure_)
                    addTopologyFailure(owner, "Validated matte mask could not be lowered.");
                return std::nullopt;
            }
            // Intersect multiplies the accumulation by the Mask (inverted, times the opacity)
            // directly, and Subtract at a constant full opacity by its complement: one Multiply
            // Alpha instead of the coverage and the combine.
            const auto* constant = std::get_if<double>(&opacity->source);
            if (accumulated && mask.mode == document::LayerMaskMode::Intersect) {
                accumulated = appendMatteMultiply(owner, *accumulated, *matte,
                                                  {Channel::Alpha, mask.inverted}, *opacity);
                continue;
            }
            if (accumulated && mask.mode == document::LayerMaskMode::Subtract &&
                constant != nullptr && *constant == 1.0) {
                accumulated = appendMatteMultiply(owner, *accumulated, *matte,
                                                  {Channel::Alpha, !mask.inverted});
                continue;
            }
            coverage = appendMatteMultiply(owner, white, *matte, {Channel::Alpha, mask.inverted},
                                           *opacity);
        } else {
            auto compiled = compileMask(mask, document::LayerMaskMode::Add);
            if (!compiled)
                return std::nullopt;
            runtime::LayerMaskKernel kernel;
            kernel.masks.push_back(std::move(*compiled));
            coverage =
                appendMatteOperation(runtime::CompiledImageEffect{owner, white, std::move(kernel)});
        }
        if (!accumulated) {
            // The first mode sets the start: 0 for Add and Difference (the coverage itself), 1 for
            // Intersect (the coverage) and Subtract (its complement).
            accumulated = mask.mode == document::LayerMaskMode::Subtract
                              ? appendMatteMultiply(owner, white, coverage, {Channel::Alpha, true})
                              : coverage;
            continue;
        }
        accumulated = appendMaskCombine(owner, mask.mode, *accumulated, coverage, white);
    }
    if (!accumulated)
        return content; // only None masks: unmasked
    return appendMatteMultiply(owner, content, *accumulated, {Channel::Alpha, false});
}
