// Included inside the snapshot compiler class (snapshot_compiler.cpp). #483 Z Defocus and Depth
// Fog. Both read one channel of their depth input through a depth map (DepthMapKernel); Z Defocus
// then gathers its input by the map (DefocusKernel) beside the map's tile maximum (a Dilate), the
// input box-prefiltered at the widest ring spacing and the bokeh image; Depth Fog mixes its input
// with the fog colour or image by the fog factor (an Image Mix with the map as its Mask). Values
// derived from the authored ones (the tile and prefilter radii) are value-graph operations, so an
// animated or driven size stays animated or driven. docs/architecture/image-filters.md, "Depth
// effects", is the specification.

// The image of a channel set's channel `name` (empty: the set's first data channel, else its
// first channel) as a Pass reads it: Raw, with non-finite samples 0.
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerDepthChannelSet(const document::NodeRecord& node, const document::EdgeRecord& edge,
                     const std::string& name, std::vector<runtime::CompiledOperation>& operations,
                     const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    runtime::CompiledChannelShuffle shuffle;
    shuffle.sourceNodeId = node.id;
    std::map<std::pair<document::NodeId, std::string>, std::uint32_t> memo;
    const auto root = lowerChannelStep(edge.source, shuffle.channels, memo, 0, &indices);
    if (!root)
        return std::nullopt;
    if (*root + 1 != shuffle.channels.steps.size()) {
        auto copy = shuffle.channels.steps[*root];
        shuffle.channels.steps.push_back(std::move(copy));
    }
    shuffle.mode = document::kChannelShuffleRawMode;
    shuffle.nonFinite = 0.0F;
    shuffle.pass = name;
    shuffle.associated = false;
    const auto set = runtime::detail::staticChannelSet(shuffle.channels);
    std::vector<runtime::detail::PassCandidate> candidates;
    bool named = false;
    for (const auto& channel : set.known) {
        candidates.push_back({channel.name, channel.descriptor});
        named = named || (!name.empty() && channel.kind == runtime::detail::StaticChannelKind::Image &&
                          (channel.name == name || channel.descriptor.layer == name));
    }
    std::optional<std::array<std::string, 4>> mapping;
    if (!set.files || named)
        mapping = runtime::detail::passMapping(candidates, name);
    return emitChannelShuffle(std::move(shuffle), mapping, node.id, operations);
}

// The R, G, B or A picker's channel of an image (0..3), or nothing for another name.
[[nodiscard]] static std::optional<std::uint8_t> depthImageChannel(std::string name,
                                                                    const bool mask) {
    std::ranges::transform(name, name.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.empty())
        return static_cast<std::uint8_t>(mask ? 3 : 0);
    if (name == "r" || name == "red")
        return std::uint8_t{0};
    if (name == "g" || name == "green")
        return std::uint8_t{1};
    if (name == "b" || name == "blue")
        return std::uint8_t{2};
    if (name == "a" || name == "alpha")
        return std::uint8_t{3};
    return std::nullopt;
}

[[nodiscard]] std::optional<runtime::OperationIndex>
lowerDepthEffect(const document::NodeRecord& node,
                 std::vector<runtime::CompiledOperation>& operations,
                 std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const bool defocus = node.typeId == document::kZDefocusNodeType;
    const auto input = findInputOperation(node.id, document::kDepthEffectInputPort, indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return std::nullopt;
    }
    bool lowered = true;
    const auto integer = [&](const std::string_view role) -> std::int64_t {
        const auto* value = parameterConstant<std::int64_t>(findParameterBinding(node, role));
        lowered = lowered && value != nullptr;
        return value != nullptr ? *value : 0;
    };
    const auto flag = [&](const std::string_view role) {
        const auto* value = parameterConstant<bool>(findParameterBinding(node, role));
        lowered = lowered && value != nullptr;
        return value != nullptr && *value;
    };
    const auto scalar = [&](const std::string_view role, runtime::CompiledScalarParameter& target) {
        const auto value = compiledScalarParameter(findParameterBinding(node, role));
        lowered = lowered && value.has_value();
        if (value)
            target = *value;
    };
    const auto* channelName =
        parameterConstant<std::string>(findParameterBinding(node, "depthChannel"));
    const auto mode = defocus ? integer("mode") : document::kDefocusModeDepth;
    if (!channelName || !lowered) {
        addTopologyFailure(node.id, "Depth effect parameters could not be lowered.");
        return std::nullopt;
    }

    // The depth source: an Image or Mask's picked channel, or a channel set's named channel.
    std::optional<runtime::OperationIndex> depth;
    std::uint8_t channel = 0;
    if (const auto* edge = fixedInputEdge(node.id, document::kDepthEffectDepthPort);
        edge != nullptr && !(defocus && mode == document::kDefocusModeUniform)) {
        const auto kind = composition_->graph().outputKind(edge->source, registry_);
        if (kind == runtime::SocketValueKind::ChannelSet) {
            depth = lowerDepthChannelSet(node, *edge, *channelName, operations, indices);
            if (!depth)
                return std::nullopt;
        } else {
            const auto picked =
                depthImageChannel(*channelName, kind == runtime::SocketValueKind::Mask);
            if (!picked) {
                addTopologyFailure(node.id, "The depth channel \"" + *channelName +
                                                "\" is not R, G, B or A of an image; connect "
                                                "a channel set to pick a channel by name.");
                return std::nullopt;
            }
            channel = *picked;
            const auto source = recipeOptionalInput(node.id, document::kDepthEffectDepthPort,
                                                    operations, indices);
            if (!source)
                return std::nullopt;
            depth = *source;
        }
    }
    // Without a depth (and not Uniform) there is nothing to read: the input passes through.
    if (!depth && !(defocus && mode == document::kDefocusModeUniform))
        return *input;

    runtime::DepthMapKernel map;
    map.channel = channel;
    map.interpretation = integer("interpretation");
    map.invert = flag("invert");
    map.mistFalloff = integer("mistFalloff");
    map.background = integer("background");
    scalar("near", map.nearDistance);
    scalar("far", map.farDistance);
    runtime::detail::RecipeEmitter emitter(node.id, operations, recipeValueSink());

    if (!defocus) {
        runtime::detail::DepthFogOperands fog;
        fog.map = std::move(map);
        fog.map.falloff = integer("falloff");
        scalar("amount", fog.map.amount);
        scalar("start", fog.map.start);
        scalar("end", fog.map.end);
        scalar("density", fog.map.density);
        if (fog.map.falloff == document::kFogFalloffCurve) {
            const auto* binding = findParameterBinding(node, "curve");
            fog.map.curve =
                binding == nullptr ? std::nullopt : compiledPathParameter(binding->parameterId);
            lowered = lowered && fog.map.curve.has_value();
        }
        const auto color = compiledColorParameter(findParameterBinding(node, "color"));
        fog.fillEmpty = flag("fillEmpty");
        const auto image =
            recipeOptionalInput(node.id, document::kDepthFogImagePort, operations, indices);
        if (!lowered || !color || !image) {
            addTopologyFailure(node.id, "Depth Fog parameters could not be lowered.");
            return std::nullopt;
        }
        fog.color = *color;
        return runtime::detail::appendDepthFog(emitter, *input, *depth, *image, std::move(fog));
    }

    // Z Defocus.
    runtime::detail::ZDefocusOperands lens;
    auto& gather = lens.gather;
    gather.mode = mode;
    gather.shape = integer("shape");
    gather.blades = integer("blades");
    gather.quality = integer("quality");
    scalar("size", gather.size);
    scalar("maxSize", gather.maxSize);
    scalar("rotation", gather.rotation);
    scalar("roundness", gather.roundness);
    scalar("highlightGain", gather.highlightGain);
    scalar("highlightThreshold", gather.highlightThreshold);
    scalar("chromatic", gather.chromatic);
    lens.map = std::move(map);
    lens.map.focusFrom = integer("focusFrom");
    lens.map.blurRegion = integer("blurRegion");
    lens.map.units = integer("units");
    lens.map.edgeExpand = integer("edgeExpand");
    scalar("focusDistance", lens.map.focusDistance);
    scalar("depthOfField", lens.map.depthOfField);
    scalar("focalLength", lens.map.focalLength);
    scalar("sensorWidth", lens.map.sensorWidth);
    scalar("fStop", lens.map.fStop);
    if (const auto point = compiledVec2Parameter(findParameterBinding(node, "focusPoint")))
        lens.map.focusPoint = *point;
    else
        lowered = false;
    const auto bokeh = recipeOptionalInput(node.id, document::kZDefocusBokehPort, operations,
                                           indices);
    if (!lowered || !bokeh || gather.quality < document::kDefocusQualityDraft ||
        gather.quality > document::kDefocusQualityMaximum) {
        addTopologyFailure(node.id, "Z Defocus parameters could not be lowered.");
        return std::nullopt;
    }
    return runtime::detail::appendZDefocus(emitter, *input, depth, *bokeh, std::move(lens));
}
