// #339 Drop Shadow, Glow and Light Wrap: each node lowers to its recipe's chain of existing image
// primitives (compositing_recipes.hpp). Derived values are appended to the value graph after the
// value pass, like a driver's promotion; Layer Bounds can never drive an image parameter, so none
// of them runs after the image pass.

[[nodiscard]] runtime::detail::RecipeEmitter::ValueSink recipeValueSink() {
    return [this](runtime::CompiledValueKernel kernel, const std::uint8_t outputCount) {
        const auto first = runtime::ValueOutputIndex::fromRaw(valueOutputCount_);
        valueOperations_.push_back(
            runtime::CompiledValueOperation{{}, first, outputCount, std::move(kernel), false});
        valueOutputCount_ += outputCount;
        postImageValueOutputs_.resize(valueOutputCount_, 0);
        return first;
    };
}

// An optional image input: nothing when unconnected, a transparent image when connected to an
// empty one (as for Image Mix's second input), or a failure.
[[nodiscard]] std::optional<std::optional<runtime::OperationIndex>>
recipeOptionalInput(const document::NodeId nodeId, const std::string_view port,
                    std::vector<runtime::CompiledOperation>& operations,
                    std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto* edge = fixedInputEdge(nodeId, port);
    if (edge == nullptr)
        return std::optional<runtime::OperationIndex>{};
    if (emptyImages_.contains(edge->source.nodeId) && !indices.contains(edge->source.nodeId)) {
        const auto empty = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledMerge{edge->source.nodeId, {}});
        indices.emplace(edge->source.nodeId, empty);
    }
    const auto source = sourceOperation(edge->source, indices);
    if (!source)
        return std::nullopt;
    return std::optional<runtime::OperationIndex>{*source};
}

[[nodiscard]] static bool isRecipeNodeType(const std::string_view typeId) noexcept {
    return typeId == document::kDropShadowNodeType || typeId == document::kGlowNodeType ||
           typeId == document::kLightWrapNodeType;
}

[[nodiscard]] std::optional<runtime::OperationIndex>
lowerRecipe(const document::NodeRecord& node, std::vector<runtime::CompiledOperation>& operations,
            std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto input = findInputOperation(node.id, "input", indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return std::nullopt;
    }
    const auto scalar = [&](const std::string_view role) {
        return compiledScalarParameter(findParameterBinding(node, role));
    };
    const auto color = [&](const std::string_view role) {
        return compiledColorParameter(findParameterBinding(node, role));
    };
    const auto flag = [&](const std::string_view role) {
        return parameterConstant<bool>(findParameterBinding(node, role));
    };
    runtime::detail::RecipeEmitter emitter(node.id, operations, recipeValueSink());
    if (node.typeId == document::kDropShadowNodeType) {
        auto shadowColor = color("color");
        auto opacity = scalar("opacity");
        auto direction = scalar("direction");
        auto distance = scalar("distance");
        auto softness = scalar("softness");
        const auto* shadowOnly = flag("shadowOnly");
        if (shadowColor && opacity && direction && distance && softness && shadowOnly)
            return runtime::detail::appendDropShadow(
                emitter, *input,
                {*shadowColor, *opacity, *direction, *distance, *softness, *shadowOnly});
        addTopologyFailure(node.id, "Drop Shadow parameters could not be lowered.");
        return std::nullopt;
    }
    if (node.typeId == document::kGlowNodeType) {
        const auto key = recipeOptionalInput(node.id, "key", operations, indices);
        const auto mask = recipeOptionalInput(node.id, "mask", operations, indices);
        auto threshold = scalar("threshold");
        auto knee = scalar("knee");
        auto intensity = scalar("intensity");
        auto size = scalar("size");
        auto falloff = scalar("falloff");
        auto tint = color("tint");
        const auto* octaves = parameterConstant<std::int64_t>(findParameterBinding(node, "octaves"));
        const auto* glowOnly = flag("glowOnly");
        if (key && mask && threshold && knee && intensity && size && falloff && tint && octaves &&
            *octaves >= document::kMinimumGlowOctaves &&
            *octaves <= document::kMaximumGlowOctaves && glowOnly)
            return runtime::detail::appendGlow(
                emitter, *input, *key, *mask,
                {*threshold, *knee, *intensity, *size, *falloff, *tint, *octaves, *glowOnly});
        addTopologyFailure(node.id, "Glow inputs or parameters could not be lowered.");
        return std::nullopt;
    }
    const auto background = recipeOptionalInput(node.id, "background", operations, indices);
    auto radius = scalar("radius");
    auto amount = scalar("amount");
    const auto* wrapOnly = flag("wrapOnly");
    if (!background || !radius || !amount || !wrapOnly) {
        addTopologyFailure(node.id, "Light Wrap inputs or parameters could not be lowered.");
        return std::nullopt;
    }
    // With no background there is nothing to wrap: the foreground passes through unchanged, and
    // the wrap alone is transparent.
    if (!*background) {
        if (!*wrapOnly)
            return *input;
        const auto empty = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledMerge{node.id, {}});
        return empty;
    }
    return runtime::detail::appendLightWrap(emitter, *input, **background,
                                            {*radius, *amount, *wrapOnly});
}
