// Included inside the snapshot compiler class (snapshot_compiler.cpp). #484 Motion blur: every
// motion-blur node lowers to the one temporal-sampling engine's kernels (compiled_plan.hpp): time
// samples of its upstream on Offset clocks and their accumulator, the path sampler, the
// motion-vector stages and the optical flow solver. docs/architecture/motion-blur.md is the
// specification.

[[nodiscard]] static std::uint32_t motionSeed(const document::NodeId node) noexcept {
    auto v = static_cast<std::uint64_t>(node.value());
    v ^= v >> 33U;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33U;
    return static_cast<std::uint32_t>(v & 0xFFFFFFU);
}

// The node's shutter (an unbound parameter is its default, as Transform 2D's before #484).
[[nodiscard]] runtime::CompiledShutter lowerShutter(const document::NodeRecord& node,
                                                    bool& lowered) {
    runtime::CompiledShutter shutter;
    shutter.rate = composition_->format().frameRate();
    const auto bound = [&](const std::string_view role) {
        return findParameterBinding(node, role);
    };
    const auto real = [&](const std::string_view role, const double fallback) {
        const auto* binding = bound(role);
        if (binding == nullptr)
            return fallback;
        const auto* value = parameterConstant<double>(binding);
        lowered = lowered && value != nullptr;
        return value != nullptr ? *value : fallback;
    };
    const auto whole = [&](const std::string_view role, const std::int64_t fallback) {
        const auto* binding = bound(role);
        if (binding == nullptr)
            return fallback;
        const auto* value = parameterConstant<std::int64_t>(binding);
        lowered = lowered && value != nullptr;
        return value != nullptr ? *value : fallback;
    };
    const auto flag = [&](const std::string_view role) {
        const auto* binding = bound(role);
        if (binding == nullptr)
            return false;
        const auto* value = parameterConstant<bool>(binding);
        lowered = lowered && value != nullptr;
        return value != nullptr && *value;
    };
    shutter.shutter = document::shutterFrames(real("shutterAngle", 180.0));
    shutter.offset = whole("shutterOffset", document::kShutterOffsetCentred);
    shutter.phase = document::shutterFrames(real("shutterPhase", 0.0));
    shutter.curve = whole("shutterCurve", document::kShutterCurveBox);
    if (const auto* binding = bound("shutterShape")) {
        const auto* value = parameterConstant<document::PathValue>(binding);
        lowered = lowered && value != nullptr;
        if (value != nullptr)
            shutter.customCurve = *value;
    }
    shutter.quality = whole("quality", document::kMotionQualityFinal);
    shutter.minSamples = whole("minSamples", 8);
    shutter.maxSamples = std::max(shutter.minSamples, whole("maxSamples", 64));
    shutter.jitter = flag("jitter");
    shutter.maxBlur = real("maxBlur", shutter.maxBlur);
    return shutter;
}

// The sample count of a time-sampled shutter: the quality's, Final its maximum (a power of two).
[[nodiscard]] static std::int64_t subFrameSamples(const runtime::CompiledShutter& shutter) {
    return runtime::detail::shutterSampleCount(shutter, static_cast<double>(shutter.maxSamples));
}

// The footage leaves (an image sequence or a video) `root` reaches.
[[nodiscard]] static std::vector<std::size_t>
footageLeaves(const runtime::OperationIndex root,
              const std::vector<runtime::CompiledOperation>& operations) {
    std::vector<std::size_t> leaves;
    std::vector<bool> seen(root.value() + 1, false);
    std::vector<std::size_t> work{root.value()};
    while (!work.empty()) {
        const auto index = work.back();
        work.pop_back();
        if (index >= seen.size() || seen[index])
            continue;
        seen[index] = true;
        const auto& operation = operations[index];
        const auto* image = std::get_if<runtime::CompiledImageSource>(&operation);
        if (std::holds_alternative<runtime::CompiledVideoSource>(operation) ||
            (image != nullptr && image->asset && image->asset->kind == document::AssetKind::Sequence)) {
            leaves.push_back(index);
            continue;
        }
        runtime::detail::forEachOperationInput(
            operation, [&](const auto input) { work.push_back(input.value()); });
    }
    std::ranges::sort(leaves);
    return leaves;
}

// The four channel names of a vector layer in a source's channel set (its first four channels),
// or the explicit names `text` lists (whitespace or comma separated). Empty when none resolve.
[[nodiscard]] static std::vector<std::string>
vectorChannelNames(const runtime::detail::StaticChannelSet& set, const std::string& text,
                   const std::string_view fallbackLayer) {
    std::vector<std::string> names;
    std::string word;
    for (const char c : text + " ") {
        if (c == ' ' || c == ',' || c == '\t') {
            if (!word.empty())
                names.push_back(word);
            word.clear();
        } else {
            word += c;
        }
    }
    const auto layerChannels = [&](const std::string_view layer) {
        std::vector<std::string> found;
        for (const auto& channel : set.known)
            if (channel.kind == runtime::detail::StaticChannelKind::Image &&
                channel.descriptor.layer == layer && found.size() < 4)
                found.push_back(channel.name);
        return found;
    };
    if (names.size() == 1 && names[0].find('.') == std::string::npos)
        names = layerChannels(names[0]);
    if (names.empty() && !fallbackLayer.empty())
        names = layerChannels(fallbackLayer);
    if (names.empty())
        for (const auto layer : {"motion", "vector", "velocity", "mv", "forward", "motionvector",
                                 "Vector", "MotionVectors", "VRayVelocity"}) {
            names = layerChannels(layer);
            if (!names.empty())
                break;
        }
    if (names.size() > 4)
        names.resize(4);
    return names;
}

// The forward (first two channels) and backward (last two) vector images of a channel set, each
// [x, y, 0, 1] raw.
[[nodiscard]] std::optional<std::array<std::optional<runtime::OperationIndex>, 2>>
lowerVectorChannels(const document::NodeId owner, const document::OutputPortRef& source,
                    const std::string& text, const std::string_view fallbackLayer,
                    std::vector<runtime::CompiledOperation>& operations,
                    const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices,
                    const bool requireFound) {
    runtime::CompiledChannelShuffle base;
    base.sourceNodeId = owner;
    std::map<std::pair<document::NodeId, std::string>, std::uint32_t> memo;
    const auto root = lowerChannelStep(source, base.channels, memo, 0, &indices);
    if (!root)
        return std::nullopt;
    if (*root + 1 != base.channels.steps.size()) {
        auto copy = base.channels.steps[*root];
        base.channels.steps.push_back(std::move(copy));
    }
    base.mode = document::kChannelShuffleRawMode;
    base.nonFinite = 0.0F;
    base.associated = false;
    const auto set = runtime::detail::staticChannelSet(base.channels);
    const auto names = vectorChannelNames(set, text, fallbackLayer);
    std::array<std::optional<runtime::OperationIndex>, 2> images{};
    if (names.size() < 2) {
        if (requireFound)
            addTopologyFailure(owner, "No motion vector channels were found; name them in "
                                      "Vector channels.");
        return requireFound ? std::nullopt
                            : std::optional{std::array<std::optional<runtime::OperationIndex>, 2>{}};
    }
    for (std::size_t half = 0; half < 2; ++half) {
        if (half == 1 && names.size() < 4)
            break;
        auto shuffle = base;
        // The forward image's third channel carries the last channel, alpha validity's coverage.
        const std::array<std::string, 4> mapping{
            names[2 * half], names[2 * half + 1],
            half == 0 ? (names.size() >= 4 ? names[3] : std::string("1")) : std::string("0"), "1"};
        images[half] = emitChannelShuffle(std::move(shuffle), mapping, owner, operations);
    }
    return images;
}


[[nodiscard]] std::optional<runtime::OperationIndex>
lowerMotionNode(const document::NodeRecord& node,
                std::vector<runtime::CompiledOperation>& operations,
                std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    const auto input = findInputOperation(node.id, document::kMotionInputPort, indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return std::nullopt;
    }
    bool lowered = true;
    const auto integer = [&](const std::string_view role, const std::int64_t fallback = 0) {
        const auto* value = parameterConstant<std::int64_t>(findParameterBinding(node, role));
        lowered = lowered && value != nullptr;
        return value != nullptr ? *value : fallback;
    };
    const auto real = [&](const std::string_view role, const double fallback = 0.0) {
        const auto* value = parameterConstant<double>(findParameterBinding(node, role));
        lowered = lowered && value != nullptr;
        return value != nullptr ? *value : fallback;
    };
    const auto flag = [&](const std::string_view role) {
        const auto* value = parameterConstant<bool>(findParameterBinding(node, role));
        lowered = lowered && value != nullptr;
        return value != nullptr && *value;
    };
    const auto text = [&](const std::string_view role) {
        const auto* value = parameterConstant<std::string>(findParameterBinding(node, role));
        lowered = lowered && value != nullptr;
        return value != nullptr ? *value : std::string{};
    };
    const auto optionalImage = [&](const std::string_view port)
        -> std::optional<std::optional<runtime::OperationIndex>> {
        return recipeOptionalInput(node.id, port, operations, indices);
    };
    auto shutter = lowerShutter(node, lowered);
    const auto seed = motionSeed(node.id);
    runtime::detail::RecipeEmitter emitter(node.id, operations, recipeValueSink());
    const auto failed = [&](const std::string& what) -> std::optional<runtime::OperationIndex> {
        addTopologyFailure(node.id, what);
        return std::nullopt;
    };

    // --- Directional, Spin and Zoom: the path sampler over the shutter's span ------------------
    if (node.typeId == document::kDirectionalBlurNodeType ||
        node.typeId == document::kSpinBlurNodeType || node.typeId == document::kZoomBlurNodeType) {
        runtime::MotionPathBlurKernel kernel;
        kernel.mode = node.typeId == document::kDirectionalBlurNodeType ? runtime::MotionPathMode::Directional
                      : node.typeId == document::kSpinBlurNodeType      ? runtime::MotionPathMode::Spin
                                                                        : runtime::MotionPathMode::Zoom;
        kernel.shutter = shutter;
        kernel.shutter.shutter = 1.0;
        kernel.shutter.phase = 0.0;
        if (node.typeId == document::kDirectionalBlurNodeType) {
            kernel.shutter.maxBlur = document::kMaximumMotionBlurPixels;
            const auto angle = compiledScalarParameter(findParameterBinding(node, "angle"));
            const auto length = compiledScalarParameter(findParameterBinding(node, "length"));
            if (!angle || !length)
                lowered = false;
            else {
                kernel.angle = *angle;
                kernel.length = *length;
            }
        } else {
            const auto centre = compiledVec2Parameter(findParameterBinding(node, "centre"));
            const auto amount = compiledScalarParameter(findParameterBinding(node, "amount"));
            if (!centre || !amount)
                lowered = false;
            else {
                kernel.centre = *centre;
                kernel.amount = *amount;
            }
        }
        kernel.seed = seed;
        if (!lowered)
            return failed("Blur parameters could not be lowered.");
        return emitter.effect(*input, std::move(kernel));
    }

    // --- Echo: time samples of the input back (or around) in time, by the operator -------------
    if (node.typeId == document::kEchoNodeType) {
        const auto mode = integer("mode");
        const auto echoes = std::clamp<std::int64_t>(integer("echoes", 4), 1,
                                                     document::kMaximumEchoes);
        const auto interval = real("interval", -1.0);
        const auto intensity = real("intensity", 1.0);
        const auto decay = real("decay", 0.5);
        const auto op = integer("operator");
        const bool posterize = flag("posterize");
        const auto rate = real("posterizeRate", 12.0);
        if (!lowered || op < document::kEchoOperatorAdd || op > document::kEchoOperatorUnder)
            return failed("Echo parameters could not be lowered.");
        std::vector<double> offsets;
        std::vector<double> weights;
        for (std::int64_t k = 0; k < echoes; ++k) {
            if (mode == document::kEchoModeFrameBlend) {
                offsets.push_back((static_cast<double>(k) - static_cast<double>(echoes - 1) / 2.0) *
                                  std::abs(interval));
                weights.push_back(1.0 / static_cast<double>(echoes));
            } else {
                offsets.push_back(static_cast<double>(k) * interval);
                weights.push_back(intensity * std::pow(decay, static_cast<double>(k)));
            }
        }
        const auto numerator = posterize ? static_cast<std::int64_t>(std::llround(rate * 1000.0)) : 0;
        const auto result =
            runtime::detail::appendTimeSamples(emitter, composition_->format().frameRate(), *input, offsets, weights,
                              static_cast<runtime::AccumulateOperator>(op), false,
                              numerator, posterize ? 1000 : 1);
        if (!result)
            return failed("Echo times cannot be represented.");
        return result;
    }

    // --- Optical Flow: forward and backward flow, confidence, occlusion and cuts ---------------
    if (node.typeId == document::kOpticalFlowNodeType) {
        const auto method = integer("method");
        const auto quality = integer("quality", document::kMotionQualityStandard);
        const auto border = integer("border", document::kFlowBorderExtend);
        const bool cuts = flag("cutDetection");
        const auto threshold = real("cutThreshold", 24.0);
        const auto next = runtime::detail::motionFramesToTime(composition_->format().frameRate(), 1.0);
        const auto previous = runtime::detail::motionFramesToTime(composition_->format().frameRate(), -1.0);
        if (!lowered || !next || !previous)
            return failed("Optical Flow parameters could not be lowered.");
        const auto guide = optionalImage(document::kMotionGuidePort);
        const auto matte = optionalImage(document::kMotionMattePort);
        if (!guide || !matte)
            return failed("Optical Flow inputs could not be lowered.");
        auto flow = runtime::detail::flowKernel(method, quality, border);
        // The guide is the clip the motion is read from; a matte weights the solve to its subject.
        if (*matte)
            flow.layer = 1;
        const auto source = guide->value_or(*input);
        const auto timed = [&](const runtime::OperationIndex image, const core::RationalTime at) {
            return emitter.effect(image, runtime::TimeSampleKernel{at, 0, 1, false});
        };
        const auto frameNext = timed(source, *next);
        const auto forward =
            runtime::detail::appendFlowSolve(emitter, source, frameNext, flow, *matte);
        portOperations_.insert_or_assign({node.id, std::string(document::kFlowForwardPort)}, forward);
        if (hasReachableOutput(node.id, document::kFlowBackwardPort))
            portOperations_.insert_or_assign(
                {node.id, std::string(document::kFlowBackwardPort)},
                runtime::detail::appendFlowSolve(emitter, source, timed(source, *previous), flow,
                                                 *matte));
        const bool confidence = hasReachableOutput(node.id, document::kFlowConfidencePort);
        const bool occlusion = hasReachableOutput(node.id, document::kFlowOcclusionPort);
        if (confidence || occlusion) {
            // The solve back from the next frame reads the matte at that frame.
            const auto matteNext = *matte ? std::optional(timed(**matte, *next)) : std::nullopt;
            const auto back =
                runtime::detail::appendFlowSolve(emitter, frameNext, source, flow, matteNext);
            runtime::VectorBlurKernel check;
            check.stage = runtime::VectorBlurStage::Consistency;
            check.border = border;
            check.shutter = shutter;
            if (confidence)
                portOperations_.insert_or_assign(
                    {node.id, std::string(document::kFlowConfidencePort)},
                    runtime::detail::appendVectorStage(emitter, forward, check,
                                                       {{runtime::kVectorForwardSlot, back}}));
            if (occlusion) {
                check.layer = 1;
                portOperations_.insert_or_assign(
                    {node.id, std::string(document::kFlowOcclusionPort)},
                    runtime::detail::appendVectorStage(emitter, forward, check,
                                                       {{runtime::kVectorForwardSlot, back}}));
            }
        }
        if (hasReachableOutput(node.id, document::kFlowCutPort)) {
            if (cuts) {
                auto cut = flow;
                cut.stage = runtime::OpticalFlowStage::CutDetect;
                cut.cutThreshold = threshold;
                cut.slotImages = {0, -1, 1};
                const auto effect = emitter.effect(source, cut);
                emitter.setImages(effect, {frameNext, forward});
                portOperations_.insert_or_assign({node.id, std::string(document::kFlowCutPort)},
                                                 effect);
            } else {
                // Off (the default): the diagnostic is an empty Mask.
                const auto empty = runtime::OperationIndex::fromRaw(operations.size());
                operations.emplace_back(runtime::CompiledMerge{node.id, {}});
                portOperations_.insert_or_assign({node.id, std::string(document::kFlowCutPort)},
                                                 empty);
            }
        }
        return forward;
    }

    // --- Motion Blur (Optical Flow) ------------------------------------------------------------
    if (node.typeId == document::kOpticalFlowMotionBlurNodeType) {
        runtime::detail::FlowBlurOptions options;
        options.shutter = shutter;
        options.shutter.shutter = real("amount", 0.5);
        options.shutter.offset = document::kShutterOffsetCentred;
        options.shutter.quality = integer("quality", document::kMotionQualityStandard);
        options.border = integer("border", document::kFlowBorderExtend);
        options.flow = runtime::detail::flowKernel(integer("method"), options.shutter.quality, options.border);
        options.holeFill = integer("holeFill");
        options.matteShrink = real("matteShrink");
        options.alphaBoost = real("alphaBoost", 1.0);
        options.seed = seed;
        const auto matte = optionalImage(document::kMotionMattePort);
        const auto guide = optionalImage(document::kMotionGuidePort);
        if (!lowered || !matte || !guide)
            return failed("Motion Blur (Optical Flow) parameters could not be lowered.");
        options.matte = *matte;
        options.guide = *guide;
        if (options.shutter.shutter == 0.0)
            return *input;
        const auto result = runtime::detail::appendFlowMotionBlur(
            emitter, composition_->format().frameRate(), *input, options);
        if (!result)
            return failed("Motion Blur (Optical Flow) times cannot be represented.");
        // The solved flow and its diagnostics, for a graph that reads them.
        if (hasReachableOutput(node.id, document::kFlowForwardPort))
            portOperations_.insert_or_assign({node.id, std::string(document::kFlowForwardPort)},
                                             result->flows.forward);
        if (hasReachableOutput(node.id, document::kFlowBackwardPort))
            portOperations_.insert_or_assign({node.id, std::string(document::kFlowBackwardPort)},
                                             result->flows.backward);
        runtime::VectorBlurKernel check;
        check.stage = runtime::VectorBlurStage::Consistency;
        check.border = options.border;
        check.shutter = options.shutter;
        if (hasReachableOutput(node.id, document::kFlowConfidencePort))
            portOperations_.insert_or_assign(
                {node.id, std::string(document::kFlowConfidencePort)},
                runtime::detail::appendVectorStage(
                    emitter, result->flows.forward, check,
                    {{runtime::kVectorForwardSlot, result->flows.forwardBack}}));
        if (hasReachableOutput(node.id, document::kFlowOcclusionPort)) {
            check.layer = 1;
            portOperations_.insert_or_assign(
                {node.id, std::string(document::kFlowOcclusionPort)},
                runtime::detail::appendVectorStage(
                    emitter, result->flows.forward, check,
                    {{runtime::kVectorForwardSlot, result->flows.forwardBack}}));
        }
        return result->image;
    }

    // --- Vector Motion Blur ---------------------------------------------------------------------
    if (node.typeId == document::kVectorMotionBlurNodeType) {
        const auto convention = integer("convention");
        const auto channels = text("vectorChannels");
        runtime::CompiledVectorDecode decode;
        if (convention == document::kVectorConventionCustom) {
            decode.scale = real("vectorScale", 1.0);
            decode.offset = real("vectorOffset", 0.0);
            decode.flipY = flag("flipY");
            decode.layout = integer("layout");
            decode.units = integer("units");
            decode.alphaValid = flag("alphaValid");
        } else {
            const auto preset = document::vectorConvention(convention);
            decode.scale = preset.scale;
            decode.offset = preset.offset;
            decode.flipY = preset.flipY;
            decode.layout = preset.layout;
            decode.units = preset.units;
            decode.alphaValid = preset.alphaValid;
            if (convention == document::kVectorConventionRsmb)
                decode.scale = real("maxDisplace", preset.scale);
        }
        const auto algorithm = integer("algorithm");
        const bool curved = flag("curved");
        const auto depthName = text("depthChannel");
        const auto softDepth = real("softDepth", 1.0);
        const auto matte = optionalImage(document::kMotionMattePort);
        if (!lowered || !matte)
            return failed("Vector Motion Blur parameters could not be lowered.");
        const auto* vectorEdge = fixedInputEdge(node.id, document::kMotionVectorsPort);
        // Without vectors there is nothing to blur by: the input passes through.
        if (vectorEdge == nullptr || shutter.shutter == 0.0)
            return *input;
        std::array<std::optional<runtime::OperationIndex>, 2> vectors{};
        if (composition_->graph().outputKind(vectorEdge->source, registry_) ==
            runtime::SocketValueKind::ChannelSet) {
            const auto found = lowerVectorChannels(node.id, vectorEdge->source, channels,
                                                   document::vectorConvention(convention).channels,
                                                   operations, indices, true);
            if (!found)
                return std::nullopt;
            vectors = *found;
        } else {
            const auto image = optionalImage(document::kMotionVectorsPort);
            if (!image || !*image)
                return failed("The motion vectors could not be lowered.");
            const auto split = runtime::detail::splitVectorImage(emitter, **image);
            vectors = {split[0], split[1]};
        }
        if (decode.layout == document::kVectorLayoutSingle)
            vectors[1].reset();
        std::optional<runtime::OperationIndex> depth;
        std::int64_t depthChannel = 0;
        if (const auto* edge = fixedInputEdge(node.id, document::kMotionDepthPort)) {
            const auto kind = composition_->graph().outputKind(edge->source, registry_);
            if (kind == runtime::SocketValueKind::ChannelSet) {
                depth = lowerDepthChannelSet(node, *edge, depthName, operations, indices);
                if (!depth)
                    return std::nullopt;
            } else {
                const auto picked =
                    depthImageChannel(depthName, kind == runtime::SocketValueKind::Mask);
                if (!picked)
                    return failed("The depth channel \"" + depthName +
                                  "\" is not R, G, B or A of an image; connect a channel set to "
                                  "pick a channel by name.");
                depthChannel = *picked;
                const auto source = optionalImage(document::kMotionDepthPort);
                if (!source || !*source)
                    return failed("The depth input could not be lowered.");
                depth = **source;
            }
        }
        const bool layered = algorithm == document::kVectorAlgorithmLayered ||
                             (algorithm == document::kVectorAlgorithmAuto &&
                              shutter.quality >= document::kMotionQualityHigh);
        runtime::VectorBlurKernel kernel;
        kernel.stage = layered ? runtime::VectorBlurStage::Splat : runtime::VectorBlurStage::Gather;
        kernel.decode = decode;
        kernel.shutter = shutter;
        kernel.curved = curved;
        kernel.depthChannel = depthChannel;
        kernel.softZ = softDepth;
        kernel.zMode = depth ? 1 : 0;
        kernel.seed = seed;
        const auto stage = [&](const runtime::OperationIndex colour, const std::int64_t layer,
                               const std::optional<runtime::OperationIndex> layerMatte) {
            auto k = kernel;
            k.layer = layer;
            std::vector<std::pair<std::size_t, runtime::OperationIndex>> slots{
                {runtime::kVectorForwardSlot, *vectors[0]}};
            if (vectors[1])
                slots.emplace_back(runtime::kVectorBackwardSlot, *vectors[1]);
            if (depth)
                slots.emplace_back(runtime::kVectorDepthSlot, *depth);
            if (layerMatte)
                slots.emplace_back(runtime::kVectorMatteSlot, *layerMatte);
            return runtime::detail::appendVectorStage(emitter, colour, k, slots);
        };
        if (!*matte || !layered)
            return stage(*input, 0, std::nullopt);
        // A matte (an ID or coverage) separates the foreground: it is splatted over the
        // background, filled outside-in behind it.
        runtime::VectorBlurKernel fill;
        fill.stage = runtime::VectorBlurStage::Fill;
        fill.threshold = 0.5;
        fill.shutter = shutter;
        const auto background =
            runtime::detail::appendVectorStage(emitter, *input, fill, {{runtime::kVectorMatteSlot, **matte}});
        kernel.stage = runtime::VectorBlurStage::Splat;
        // Merge entries are top first: the foreground over the blurred background.
        const auto foreground = stage(*input, 1, **matte);
        return emitter.merge({{foreground, core::BlendMode::Normal},
                              {stage(background, 0, std::nullopt), core::BlendMode::Normal}});
    }

    // --- Motion Blur: sub-frame accumulation of everything upstream ----------------------------
    if (node.typeId == document::kMotionBlurNodeType) {
        const auto footage = integer("footage");
        const auto layer = text("vectorLayer");
        if (!lowered || footage < document::kFootageAuto || footage > document::kFootageVectors)
            return failed("Motion Blur parameters could not be lowered.");
        if (shutter.shutter == 0.0)
            return *input;
        // Footage between frames: held, or (Flow, Vectors, Automatic with a vector layer) each
        // footage leaf replaced by its own optical-flow or vector motion blur, held.
        runtime::OperationIndex source = *input;
        std::map<std::size_t, runtime::OperationIndex> substitutes;
        for (const auto leaf : footageLeaves(*input, operations)) {
            const auto& operation = operations[leaf];
            const auto owner = std::visit(
                [](const auto& step) -> document::NodeId {
                    if constexpr (requires { step.sourceNodeId; })
                        return step.sourceNodeId;
                    else
                        return {};
                },
                operation);
            if (footage == document::kFootageFlow) {
                runtime::detail::FlowBlurOptions options;
                options.shutter = shutter;
                options.flow = runtime::detail::flowKernel(document::kFlowMethodAuto, shutter.quality,
                                          document::kFlowBorderExtend);
                options.seed = seed;
                const auto blurred = runtime::detail::appendFlowMotionBlur(emitter, composition_->format().frameRate(), runtime::OperationIndex::fromRaw(leaf), options);
                if (!blurred)
                    return failed("Motion Blur times cannot be represented.");
                substitutes.emplace(leaf, blurred->image);
            } else if (footage == document::kFootageVectors || footage == document::kFootageAuto) {
                const auto* sourceNode = composition_->graph().findNode(owner);
                const auto port = sourceNode != nullptr && sourceNode->typeId == document::kVideoSourceNodeType
                                      ? document::kVideoSourceChannelsOutputPort
                                      : document::kImageSourceChannelsOutputPort;
                if (sourceNode == nullptr)
                    continue;
                const auto found = lowerVectorChannels(
                    node.id, document::OutputPortRef{owner, std::string(port)}, layer, {},
                    operations, indices, footage == document::kFootageVectors);
                if (!found)
                    return std::nullopt;
                if (!(*found)[0])
                    continue; // Automatic without a vector layer: held.
                runtime::VectorBlurKernel kernel;
                kernel.stage = shutter.quality >= document::kMotionQualityHigh
                                   ? runtime::VectorBlurStage::Splat
                                   : runtime::VectorBlurStage::Gather;
                kernel.decode.layout = (*found)[1] ? document::kVectorLayoutForwardBackward
                                                   : document::kVectorLayoutSingle;
                kernel.shutter = shutter;
                kernel.seed = seed;
                std::vector<std::pair<std::size_t, runtime::OperationIndex>> slots{
                    {runtime::kVectorForwardSlot, *(*found)[0]}};
                if ((*found)[1])
                    slots.emplace_back(runtime::kVectorBackwardSlot, *(*found)[1]);
                substitutes.emplace(leaf, runtime::detail::appendVectorStage(emitter, runtime::OperationIndex::fromRaw(leaf),
                                                            kernel, slots));
            }
        }
        if (!substitutes.empty())
            source = runtime::detail::cloneWithSubstitutes(*input, substitutes, operations);
        const auto n = subFrameSamples(shutter);
        const auto samples = runtime::detail::shutterSamples(shutter, n);
        const auto result = runtime::detail::appendTimeSamples(emitter, composition_->format().frameRate(), source, samples.centre, samples.weight,
                                              runtime::AccumulateOperator::Add, true, 0, 1);
        if (!result)
            return failed("Motion Blur times cannot be represented.");
        return result;
    }
    return failed("Unknown motion blur node.");
}

// Transform 2D with motion blur on: the path sampler resampling through the transform at each
// sub-frame of the shutter.
[[nodiscard]] std::optional<runtime::ImageEffectKernel>
transformMotionBlurKernel(const document::NodeRecord& node, const runtime::Transform2DKernel& transform) {
    const auto* binding = findParameterBinding(node, "motionBlur");
    const auto* enabled = binding != nullptr ? parameterConstant<bool>(binding) : nullptr;
    if (enabled == nullptr || !*enabled)
        return std::nullopt;
    bool lowered = true;
    runtime::MotionPathBlurKernel kernel;
    kernel.mode = runtime::MotionPathMode::Transform;
    kernel.shutter = lowerShutter(node, lowered);
    kernel.shutter.maxBlur = document::kMaximumMotionBlurPixels;
    kernel.transform = transform;
    kernel.seed = motionSeed(node.id);
    if (!lowered)
        return std::nullopt;
    return runtime::ImageEffectKernel{std::move(kernel)};
}
