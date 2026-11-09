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
           typeId == document::kLightWrapNodeType || typeId == document::kEdgeExtendNodeType;
}

// #486 Sharpen: its input's blur and the unsharp mask over the two.
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerSharpen(const document::NodeRecord& node, std::vector<runtime::CompiledOperation>& operations,
             std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto input = findInputOperation(node.id, "input", indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return std::nullopt;
    }
    const auto amount = compiledScalarParameter(findParameterBinding(node, "amount"));
    const auto radius = compiledScalarParameter(findParameterBinding(node, "radius"));
    const auto threshold = compiledScalarParameter(findParameterBinding(node, "threshold"));
    if (!amount || !radius || !threshold) {
        addTopologyFailure(node.id, "Sharpen parameters could not be lowered.");
        return std::nullopt;
    }
    runtime::detail::RecipeEmitter emitter(node.id, operations, recipeValueSink());
    return runtime::detail::appendSharpen(emitter, *input, {*amount, *radius, *threshold});
}

// #563 Codec Degrade: its look-back chain of codec passes over time samples of its input.
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerCodecDegrade(const document::NodeRecord& node,
                  std::vector<runtime::CompiledOperation>& operations,
                  std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto input = findInputOperation(node.id, "input", indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return std::nullopt;
    }
    const auto quality = compiledScalarParameter(findParameterBinding(node, "quality"));
    const auto persistence = compiledScalarParameter(findParameterBinding(node, "persistence"));
    const auto* block = parameterConstant<std::int64_t>(findParameterBinding(node, "blockSize"));
    const auto* gop = parameterConstant<std::int64_t>(findParameterBinding(node, "gopLength"));
    const auto* chroma = parameterConstant<bool>(findParameterBinding(node, "chromaBleed"));
    const auto blockPixels = block != nullptr ? document::codecDegradeBlockPixels(*block) : 0;
    const auto gopFrames = gop != nullptr ? document::codecDegradeGopFrames(*gop) : 0;
    if (!quality || !persistence || blockPixels == 0 || gopFrames == 0 || chroma == nullptr) {
        addTopologyFailure(node.id, "Codec Degrade parameters could not be lowered.");
        return std::nullopt;
    }
    runtime::detail::RecipeEmitter emitter(node.id, operations, recipeValueSink());
    return runtime::detail::appendCodecDegrade(
        emitter, *input, {blockPixels, gopFrames, *chroma, *quality, *persistence});
}

// #566 Auto Exposure: the look-back window of reductions, the filter over their means and the gain.
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerAutoExposure(const document::NodeRecord& node,
                  std::vector<runtime::CompiledOperation>& operations,
                  std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto input = findInputOperation(node.id, "input", indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return std::nullopt;
    }
    runtime::detail::AutoExposureOperands operands;
    bool valid = true;
    for (const auto& spec : document::autoExposureParameters()) {
        if (spec.kind != document::AutoExposureParameterKind::Scalar)
            continue;
        const auto scalar = compiledScalarParameter(findParameterBinding(node, spec.role));
        if (!scalar) {
            valid = false;
            continue;
        }
        operands.values[document::autoExposureScalarIndex(spec.role)] = *scalar;
    }
    const auto* lookBack =
        parameterConstant<std::int64_t>(findParameterBinding(node, "lookBack"));
    if (!valid || lookBack == nullptr || *lookBack < document::kMinimumAutoExposureLookBack ||
        *lookBack > document::kMaximumAutoExposureLookBack) {
        addTopologyFailure(node.id, "Auto Exposure parameters could not be lowered.");
        return std::nullopt;
    }
    operands.lookBackFrames = *lookBack;
    runtime::detail::RecipeEmitter emitter(node.id, operations, recipeValueSink());
    const auto lowered = runtime::detail::appendAutoExposure(emitter, *input, operands);
    if (!lowered)
        addTopologyFailure(node.id, "Auto Exposure look-back window is out of bounds.");
    return lowered;
}

// #349 Image Statistics: the reduction of the picture an Image Statistics node reads, to the
// two-pixel result its tap plan ends in.
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerStatisticsReduction(const document::NodeId statisticsNode, const runtime::OperationIndex input,
                         std::vector<runtime::CompiledOperation>& operations) {
    const auto* node = findNode(statisticsNode);
    if (node == nullptr) {
        addTopologyFailure(statisticsNode, "The Image Statistics node disappeared before lowering.");
        return std::nullopt;
    }
    const auto choice = [&](const std::string_view role, const std::int64_t high,
                            std::int64_t& target) {
        const auto* value = parameterConstant<std::int64_t>(findParameterBinding(*node, role));
        if (value == nullptr || *value < 0 || *value > high)
            return false;
        target = *value;
        return true;
    };
    runtime::detail::StatisticsOperands operands;
    std::int64_t space = 0;
    std::int64_t pixels = 0;
    if (!choice("channel", document::kStatisticsAlpha, operands.channel) ||
        !choice("space", document::kImagePixelStored, space) ||
        !choice("pixels", document::kStatisticsOpaqueOnly, pixels)) {
        addTopologyFailure(statisticsNode, "Image Statistics parameters could not be lowered.");
        return std::nullopt;
    }
    operands.straight = space == document::kImagePixelStraight;
    operands.opaqueOnly = pixels == document::kStatisticsOpaqueOnly;
    runtime::detail::RecipeEmitter emitter(statisticsNode, operations, recipeValueSink());
    return runtime::detail::appendStatistics(emitter, input, operands);
}

// #349 Histogram: the tile counts, their sum and the plot.
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerHistogram(const document::NodeRecord& node, std::vector<runtime::CompiledOperation>& operations,
               std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto input = findInputOperation(node.id, "input", indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return std::nullopt;
    }
    const auto integer = [&](const std::string_view role, const std::int64_t high,
                             std::int64_t& target) {
        const auto* value = parameterConstant<std::int64_t>(findParameterBinding(node, role));
        if (value == nullptr || *value < 0 || *value > high)
            return false;
        target = *value;
        return true;
    };
    runtime::detail::HistogramOperands operands;
    std::int64_t space = 0;
    std::int64_t scale = 0;
    std::int64_t bins = 0;
    std::int64_t height = 0;
    const auto rangeMin = compiledScalarParameter(findParameterBinding(node, "rangeMin"));
    const auto rangeMax = compiledScalarParameter(findParameterBinding(node, "rangeMax"));
    const bool valid =
        integer("channels", document::kHistogramAlpha, operands.channels) &&
        integer("space", document::kImagePixelStored, space) &&
        integer("graphScale", document::kHistogramLogarithmic, scale) &&
        integer("bins", document::kHistogramBins.size() - 1, bins) &&
        integer("height", document::kHistogramHeights.size() - 1, height) && rangeMin && rangeMax;
    if (!valid) {
        addTopologyFailure(node.id, "Histogram parameters could not be lowered.");
        return std::nullopt;
    }
    operands.straight = space == document::kImagePixelStraight;
    operands.logarithmic = scale == document::kHistogramLogarithmic;
    operands.bins = document::kHistogramBins[static_cast<std::size_t>(bins)];
    operands.height = document::kHistogramHeights[static_cast<std::size_t>(height)];
    operands.values = {*rangeMin, *rangeMax};
    runtime::detail::RecipeEmitter emitter(node.id, operations, recipeValueSink());
    return runtime::detail::appendHistogram(emitter, *input, operands);
}

// #347 Audio Waveform and Audio Spectrum: a transparent canvas of the plot's size and the pass that
// draws the analysis of the audio on it. A muted node draws nothing.
[[nodiscard]] bool
lowerAudioPlot(const document::NodeRecord& node, std::vector<runtime::CompiledOperation>& operations,
               std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using namespace document;
    const bool spectrum = node.typeId == kAudioSpectrumNodeType;
    const auto integer = [&](const std::string_view role, const std::int64_t high,
                             std::int64_t& target) {
        const auto* value = parameterConstant<std::int64_t>(findParameterBinding(node, role));
        if (value == nullptr || *value < 0 || *value > high)
            return false;
        target = *value;
        return true;
    };
    runtime::AudioPlotKernel kernel;
    kernel.kind = spectrum ? runtime::AudioPlotKind::Spectrum : runtime::AudioPlotKind::Waveform;
    std::int64_t choice = 0;
    std::int64_t axis = 0;
    bool valid = integer("width", kAudioPlotMaximumSide, kernel.width) &&
                 integer("height", kAudioPlotMaximumSide, kernel.height) &&
                 kernel.width >= kAudioPlotMinimumSide && kernel.height >= kAudioPlotMinimumSide;
    if (spectrum) {
        valid = valid && integer("transformSize", kAudioSpectrumSizes.size() - 1, choice) &&
                integer("axis", kAudioAxisLinear, axis);
        if (valid) {
            kernel.fftSize = kAudioSpectrumSizes[static_cast<std::size_t>(choice)];
            kernel.logarithmic = axis == kAudioAxisLogarithmic;
        }
    } else {
        valid = valid && integer("channels", kAudioWaveformSeparate, choice);
        kernel.separate = choice == kAudioWaveformSeparate;
    }
    const std::array<std::string_view, 2> waveformRoles{"window", "verticalScale"};
    const std::array<std::string_view, 4> spectrumRoles{"lowFrequency", "highFrequency", "floor",
                                                        "ceiling"};
    const auto roles = spectrum ? std::span<const std::string_view>(spectrumRoles)
                                : std::span<const std::string_view>(waveformRoles);
    for (std::size_t slot = 0; slot < roles.size() && valid; ++slot) {
        const auto parameter = compiledScalarParameter(findParameterBinding(node, roles[slot]));
        valid = parameter.has_value();
        if (parameter)
            kernel.values[slot] = *parameter;
    }
    const auto color = compiledColorParameter(findParameterBinding(node, "plotColor"));
    const auto background = compiledColorParameter(findParameterBinding(node, "background"));
    if (!valid || !color || !background) {
        addTopologyFailure(node.id, "Audio plot parameters could not be lowered.");
        return false;
    }
    kernel.color = *color;
    kernel.background = *background;
    if (isMuted(node.id)) {
        const auto index = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledMerge{node.id, {}});
        indices.emplace(node.id, index);
        return true;
    }
    const auto lowered = lowerAudioAnalysisSource(node);
    if (!lowered.ok)
        return false;
    kernel.source = lowered.source;
    const auto canvas = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(runtime::CompiledSolid{
        node.id,
        runtime::CompiledColorParameter{document::ParameterId{}, core::Color4d{0.0, 0.0, 0.0, 0.0}},
        fieldConstant(static_cast<double>(kernel.width)),
        fieldConstant(static_cast<double>(kernel.height))});
    const auto plot = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(runtime::CompiledImageEffect{node.id, canvas, std::move(kernel)});
    indices.emplace(node.id, plot);
    return true;
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
    if (node.typeId == document::kEdgeExtendNodeType) {
        auto distance = scalar("distance");
        auto amount = scalar("amount");
        if (!distance || !amount) {
            addTopologyFailure(node.id, "Edge Extend parameters could not be lowered.");
            return std::nullopt;
        }
        return runtime::detail::appendEdgeExtend(emitter, *input, {*distance, *amount});
    }
    if (node.typeId == document::kGlowNodeType) {
        const auto key = recipeOptionalInput(node.id, "key", operations, indices);
        const auto mask = recipeOptionalInput(node.id, "mask", operations, indices);
        const auto dirt = recipeOptionalInput(node.id, "dirt", operations, indices);
        auto threshold = scalar("threshold");
        auto knee = scalar("knee");
        auto intensity = scalar("intensity");
        auto size = scalar("size");
        auto falloff = scalar("falloff");
        auto tint = color("tint");
        const auto* octaves = parameterConstant<std::int64_t>(findParameterBinding(node, "octaves"));
        const auto* glowOnly = flag("glowOnly");
        if (!key || !mask || !dirt || !threshold || !knee || !intensity || !size || !falloff ||
            !tint || !octaves || *octaves < document::kMinimumGlowOctaves ||
            *octaves > document::kMaximumGlowOctaves || !glowOnly) {
            addTopologyFailure(node.id, "Glow inputs or parameters could not be lowered.");
            return std::nullopt;
        }
        runtime::detail::GlowOperands operands{*threshold, *knee,  *intensity, *size,
                                               *falloff,   *tint, *octaves,   *glowOnly};
        // #506: the modes and shared controls are optional; a Glow saved before them binds none
        // and keeps every default, which is exactly the Classic chain. A bound one must lower.
        bool lowered = true;
        const auto optionalScalar = [&](const std::string_view role,
                                        runtime::CompiledScalarParameter& target) {
            if (const auto* binding = findParameterBinding(node, role)) {
                const auto value = compiledScalarParameter(binding);
                lowered = lowered && value.has_value();
                if (value)
                    target = *value;
            }
        };
        const auto optionalInteger = [&](const std::string_view role, std::int64_t& target) {
            if (const auto* binding = findParameterBinding(node, role)) {
                const auto* value = parameterConstant<std::int64_t>(binding);
                lowered = lowered && value != nullptr;
                if (value)
                    target = *value;
            }
        };
        std::int64_t mode = document::kGlowModeClassic;
        optionalInteger("mode", mode);
        optionalInteger("points", operands.points);
        optionalScalar("exponent", operands.exponent);
        optionalScalar("radiusRed", operands.radius[0]);
        optionalScalar("radiusGreen", operands.radius[1]);
        optionalScalar("radiusBlue", operands.radius[2]);
        optionalScalar("angle", operands.angle);
        optionalScalar("length", operands.length);
        optionalScalar("chromatic", operands.chromatic);
        optionalScalar("dirtAmount", operands.dirtAmount);
        optionalScalar("rollOff", operands.rollOff);
        if (findParameterBinding(node, "gradient") != nullptr) {
            const auto* gradient = flag("gradient");
            lowered = lowered && gradient != nullptr;
            if (gradient)
                operands.gradient = *gradient;
        }
        if (const auto* binding = findParameterBinding(node, "tintGradient")) {
            auto tintGradient = compiledGradientParameter(node, *binding);
            lowered = lowered && tintGradient.has_value();
            if (tintGradient)
                operands.tintGradient = std::move(*tintGradient);
        }
        if (!lowered || mode < document::kGlowModeClassic || mode > document::kGlowModeStreak ||
            operands.points < document::kMinimumGlowPoints ||
            operands.points > document::kMaximumGlowPoints) {
            addTopologyFailure(node.id, "Glow mode or parameters could not be lowered.");
            return std::nullopt;
        }
        operands.mode = static_cast<runtime::detail::GlowMode>(mode);
        return runtime::detail::appendGlow(emitter, *input, *key, *mask, operands, *dirt);
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
