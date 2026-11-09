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

// #514 Denoise & Smooth. Median, Bilateral and Spatial are one DenoiseKernel on the input. Temporal
// is the look-back window of the input (the current frame and those before it, each a time sample
// on the plan's frame grid) and one weighted step per earlier frame, nearest first: each step adds
// one earlier frame to the running mean with the weight its patch similarity to the current frame
// gives it (denoise_image_effect.hpp). With Auto strength the strength is measured: the deviation of
// the stored green channel over the sample region of the input (an Image Statistics plan of the
// node's own, run for the frame through the statistics route and its one named readback) times the
// mode's factor, limited to the strength's domain.
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerDenoise(const document::NodeRecord& node, std::vector<runtime::CompiledOperation>& operations,
             std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto input = findInputOperation(node.id, "input", indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return std::nullopt;
    }
    const auto* mode = parameterConstant<std::int64_t>(findParameterBinding(node, "mode"));
    const auto* frames = parameterConstant<std::int64_t>(findParameterBinding(node, "frames"));
    const auto* automatic = parameterConstant<bool>(findParameterBinding(node, "autoStrength"));
    if (mode == nullptr || frames == nullptr || *mode < 0 || *mode >= document::kDenoiseModeCount ||
        *frames < document::kMinimumDenoiseFrames || *frames > document::kMaximumDenoiseFrames) {
        addTopologyFailure(node.id, "Denoise parameters could not be lowered.");
        return std::nullopt;
    }
    const auto nodeMode = static_cast<document::DenoiseNodeMode>(*mode);
    const bool temporal = nodeMode == document::DenoiseNodeMode::Temporal;
    const bool measured = automatic != nullptr && *automatic;
    runtime::detail::RecipeEmitter emitter(node.id, operations, recipeValueSink());
    // Only the values the mode reads are lowered (document::isParameterInactive): Temporal has no
    // radius, and a measured strength replaces the authored one.
    const auto radius = temporal ? std::optional<runtime::CompiledScalarParameter>{
                                       runtime::CompiledScalarParameter{}}
                                 : compiledScalarParameter(findParameterBinding(node, "radius"));
    std::optional<runtime::CompiledScalarParameter> strength;
    if (measured) {
        const auto sample = lowerStatisticsPlan(node);
        if (!sample.ok) {
            addTopologyFailure(node.id, "The Denoise sample region could not be measured.");
            return std::nullopt;
        }
        const auto first = recipeValueSink()(
            runtime::CompiledValueKernel{runtime::CompiledValueImageStatistics{node.id, sample.plan}},
            5);
        const runtime::CompiledScalarParameter deviation{
            {}, runtime::ValueOutputIndex::fromRaw(first.value() + 3)};
        using core::primitives::ScalarPrimitive;
        strength = emitter.scalar(
            ScalarPrimitive::Minimum,
            {emitter.scalar(ScalarPrimitive::Multiply,
                            {deviation, runtime::detail::RecipeEmitter::constant(
                                            document::denoiseAutoStrengthFactor(nodeMode))}),
             runtime::detail::RecipeEmitter::constant(document::kMaximumDenoiseStrengthValue)});
    } else {
        strength = compiledScalarParameter(findParameterBinding(node, "strength"));
    }
    if (!radius || !strength) {
        addTopologyFailure(node.id, "Denoise parameters could not be lowered.");
        return std::nullopt;
    }
    if (!temporal)
        return emitter.effect(*input, runtime::DenoiseKernel{static_cast<runtime::DenoiseMode>(*mode),
                                                              *radius, *strength});
    const auto lowered = runtime::detail::appendTemporalDenoise(emitter, *input, *frames, *strength);
    if (!lowered)
        addTopologyFailure(node.id, "Denoise look-back window is out of bounds.");
    return lowered;
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

// Gaussian Blur with a radius per channel: five blurs and four channel combines
// (appendChannelBlur).
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerChannelBlur(const document::NodeRecord& node,
                 std::vector<runtime::CompiledOperation>& operations,
                 std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto input = findInputOperation(node.id, "input", indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return std::nullopt;
    }
    runtime::detail::ChannelBlurOperands operands;
    const auto radius = compiledScalarParameter(findParameterBinding(node, "radius"));
    const auto* edge = parameterConstant<std::int64_t>(findParameterBinding(node, "edge"));
    const auto* separate = parameterConstant<bool>(findParameterBinding(node, "separate"));
    // The vertical radius is read only with separate axes: an animated one with the switch off
    // must not register a curve nothing reads.
    const auto radiusY = separate != nullptr && *separate
                             ? compiledScalarParameter(findParameterBinding(node, "radiusY"))
                             : std::optional<runtime::CompiledScalarParameter>{
                                   runtime::CompiledScalarParameter{}};
    bool lowered = radius && edge && *edge >= 0 && *edge <= 1 && separate && radiusY;
    constexpr std::array<const char*, 4> roles{"redScale", "greenScale", "blueScale",
                                               "alphaScale"};
    for (std::size_t channel = 0; channel < roles.size(); ++channel) {
        const auto scale = compiledScalarParameter(findParameterBinding(node, roles[channel]));
        lowered = lowered && scale.has_value();
        if (scale)
            operands.scales[channel] = *scale;
    }
    if (!lowered) {
        addTopologyFailure(node.id, "Gaussian Blur parameters could not be lowered.");
        return std::nullopt;
    }
    operands.radius = *radius;
    operands.radiusY = *radiusY;
    operands.separate = *separate;
    operands.edge = static_cast<runtime::BlurEdgeMode>(*edge);
    runtime::detail::RecipeEmitter emitter(node.id, operations, recipeValueSink());
    return runtime::detail::appendChannelBlur(emitter, *input, operands);
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

// #514: the sample region of a Denoise node's Auto strength, cut out of the picture, reduced to the
// statistics of the stored green channel. The corners are full-resolution pixels from the
// composition's centre, +y up; the crop's margins are measured from the display window's edges, so
// the region scales with the render like any crop. A region is kept inside the frame and at least
// one pixel wide and tall.
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerDenoiseSampleReduction(const document::NodeRecord& node, const runtime::OperationIndex input,
                            std::vector<runtime::CompiledOperation>& operations) {
    // A node saved without a region measures the default one.
    const auto* from =
        parameterConstant<document::Vec2d>(findParameterBinding(node, "sampleFrom"));
    const auto* to = parameterConstant<document::Vec2d>(findParameterBinding(node, "sampleTo"));
    if (from == nullptr)
        from = &document::kDefaultDenoiseSampleFrom;
    if (to == nullptr)
        to = &document::kDefaultDenoiseSampleTo;
    const auto width = static_cast<double>(composition_->format().width());
    const auto height = static_cast<double>(composition_->format().height());
    // In pixels from the frame's left and top edges.
    const auto clampTo = [](const double value, const double high) {
        return std::clamp(value, 0.0, high);
    };
    auto left = clampTo(width / 2.0 + std::min(from->x, to->x), width - 1.0);
    auto right = clampTo(width / 2.0 + std::max(from->x, to->x), width);
    auto top = clampTo(height / 2.0 - std::max(from->y, to->y), height - 1.0);
    auto bottom = clampTo(height / 2.0 - std::min(from->y, to->y), height);
    right = std::max(right, left + 1.0);
    bottom = std::max(bottom, top + 1.0);
    using runtime::detail::RecipeEmitter;
    RecipeEmitter emitter(node.id, operations, recipeValueSink());
    const auto region = emitter.effect(
        input, runtime::CanvasImageKernel{runtime::CanvasImageEffect::Crop,
                                          {RecipeEmitter::constant(left), RecipeEmitter::constant(top),
                                           RecipeEmitter::constant(width - right),
                                           RecipeEmitter::constant(height - bottom)}});
    runtime::detail::StatisticsOperands operands;
    operands.channel = document::kStatisticsGreen;
    operands.straight = false;
    operands.opaqueOnly = false;
    return runtime::detail::appendStatistics(emitter, region, operands);
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
    // #514: a Denoise node's Auto strength measures the noise of its input over its sample region:
    // the region cut out with the canvas crop, then the deviation of the stored green channel.
    if (node->typeId == document::kDenoiseNodeType)
        return lowerDenoiseSampleReduction(*node, input, operations);
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
