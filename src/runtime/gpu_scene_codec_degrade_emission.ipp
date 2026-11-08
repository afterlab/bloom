// Included inside bloom::runtime::detail after the field emission helpers.
//
// #563 Codec Degrade on the GPU. A pass is the one fixed program (ColorGradeKind::CodecDegrade)
// dispatched over an image. An Encode, a keyframe or a Finish pass reads one image, dispatched as it
// is. A predicted pass follows its Encoded colour and reads the chain so far and the optical flow
// beside it: an atlas as Z Defocus's, the colour padded to it (one exact move) with each other image,
// moved exactly into place, blended over it in Normal mode (the regions are disjoint, so each pixel
// is one image's exact value). The table and geometry are the CPU reference's
// (codecDegradeProgramData), so both evaluate one kernel over identical data.

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitCodecDegradeCommand(const CompiledImageEffect& effect, const PreparedColorGrade& grade,
                        const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                        const GpuSceneOcioContext& ocioContext,
                        const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
                        ChargeCoverage&& chargeCoverage, GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                    "Codec Degrade preparation was cancelled");
    const auto& outputWindowOf = *ctx.outputWindowOf;
    const auto inputValue = effect.input.value();
    const auto stage = codecDegradeStage(grade);
    const bool predicted = stage == CodecStage::Predicted;
    // The image the output follows: a predicted pass's Encoded colour, otherwise the input. With no
    // pixels there is nothing to code, as on the CPU reference.
    std::size_t followed = inputValue;
    if (predicted) {
        if (effect.fieldImages.size() != 2 || effect.fieldImages[0].value() >= outputWindowOf.size() ||
            effect.fieldImages[1].value() >= outputWindowOf.size())
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "The Codec Degrade images are invalid");
        followed = effect.fieldImages[0].value();
    }
    if (!outputWindowOf[followed]) {
        // A pass with no input to follow publishes none: its input has none either, or, for a
        // predicted pass, the frame it codes does.
        if (!predicted) {
            result.identity = true;
            return std::nullopt;
        }
        result.window.reset();
        result.command = kInvalidGpuSceneCommand;
        return std::nullopt;
    }
    GpuSceneCommandIndex colour = kInvalidGpuSceneCommand;
    std::string colourKey;
    if (const auto error = residentFieldInput(followed, operation, ctx, cancellation, emit, charge,
                                              chargeCoverage, colour, colourKey, result))
        return error;
    const auto descriptor = gpuSceneCommandDescriptorOf(*ctx.commands, colour, 0);
    if (!descriptor)
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "The Codec Degrade descriptor is not derivable");
    const auto output = descriptor->data;
    const auto display = descriptor->display;
    const auto aspect = descriptor->pixelAspect;
    const auto regionOf = [](const render::ImageWindow& w) {
        return CodecRegion{w.originX(), w.originY(), static_cast<std::int64_t>(w.extent().width()),
                           static_cast<std::int64_t>(w.extent().height()), 0, 0};
    };
    CodecPlacement placement;
    placement.colour = regionOf(output);

    GpuSceneCommandIndex base = colour;
    std::string baseKey = colourKey;
    render::ImageWindow dispatchWindow = output;
    if (predicted) {
        struct Slot final {
            CodecRegion* region = nullptr;
            GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
            std::string key;
            render::ImageWindow window;
        };
        std::vector<Slot> slots;
        const std::array<std::pair<std::size_t, CodecRegion*>, 2> wanted{
            {{inputValue, &placement.previous}, {effect.fieldImages[1].value(), &placement.flow}}};
        const auto outW = static_cast<std::int64_t>(output.extent().width());
        const auto outH = static_cast<std::int64_t>(output.extent().height());
        std::int64_t rowWidth = outW, rowHeight = outH, columnWidth = outW, columnHeight = outH;
        for (const auto& [value, region] : wanted) {
            // An image with no pixels is an absent one, as on the CPU reference.
            if (value >= outputWindowOf.size() || !outputWindowOf[value])
                continue;
            Slot slot{region, kInvalidGpuSceneCommand, {}, *outputWindowOf[value]};
            if (const auto error = residentFieldInput(value, operation, ctx, cancellation, emit,
                                                      charge, chargeCoverage, slot.command,
                                                      slot.key, result))
                return error;
            const auto w = static_cast<std::int64_t>(slot.window.extent().width());
            const auto h = static_cast<std::int64_t>(slot.window.extent().height());
            rowWidth += w;
            rowHeight = std::max(rowHeight, h);
            columnWidth = std::max(columnWidth, w);
            columnHeight += h;
            slots.push_back(std::move(slot));
        }
        if (!slots.empty()) {
            const bool row = std::max(rowWidth, rowHeight) <= std::max(columnWidth, columnHeight);
            const auto atlasCreated = render::ImageWindow::create(
                output.originX(), output.originY(),
                static_cast<std::uint64_t>(row ? rowWidth : columnWidth),
                static_cast<std::uint64_t>(row ? rowHeight : columnHeight));
            if (!atlasCreated)
                return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "The Codec Degrade atlas window overflows");
            dispatchWindow = *atlasCreated.value();
            if (const auto error = emitFieldMove(colour, colourKey, output, dispatchWindow, 0, 0,
                                                 operation, emit, charge, base, baseKey))
                return error;
            std::int64_t cursor = row ? outW : outH;
            for (auto& slot : slots) {
                const auto atlasX = row ? cursor : 0;
                const auto atlasY = row ? 0 : cursor;
                cursor += row ? static_cast<std::int64_t>(slot.window.extent().width())
                              : static_cast<std::int64_t>(slot.window.extent().height());
                const auto placed = render::ImageWindow::create(
                    dispatchWindow.originX() + atlasX, dispatchWindow.originY() + atlasY,
                    slot.window.extent().width(), slot.window.extent().height());
                if (!placed)
                    return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                                "A Codec Degrade image does not fit the atlas");
                GpuSceneCommandIndex moved = kInvalidGpuSceneCommand;
                std::string movedKey;
                if (const auto error = emitFieldMove(
                        slot.command, slot.key, slot.window, *placed.value(),
                        placed.value()->originX() - slot.window.originX(),
                        placed.value()->originY() - slot.window.originY(), operation, emit, charge,
                        moved, movedKey))
                    return error;
                if (const auto error = charge(dispatchWindow.extent().width(),
                                              dispatchWindow.extent().height(),
                                              sizeof(render::Rgba32f)))
                    return error;
                auto blendKey = makeGpuSceneBlendSemanticKey(
                    movedKey, baseKey, core::BlendMode::Normal, *placed.value(), dispatchWindow,
                    aspect, std::string{kGpuBlendSpirvSha256});
                base = emit(GpuSceneBlendCommand{.sourceOperation = operation,
                                                 .source = moved,
                                                 .destination = base,
                                                 .sourceKey = movedKey,
                                                 .destinationKey = baseKey,
                                                 .mode = core::BlendMode::Normal,
                                                 .imageCombine = {},
                                                 .sourceWindow = *placed.value(),
                                                 .outputWindow = dispatchWindow,
                                                 .pixelAspect = aspect,
                                                 .artifactDigest =
                                                     std::string{kGpuBlendSpirvSha256},
                                                 .semanticKey = blendKey});
                baseKey = std::move(blendKey);
                *slot.region = {slot.window.originX(),
                                slot.window.originY(),
                                static_cast<std::int64_t>(slot.window.extent().width()),
                                static_cast<std::int64_t>(slot.window.extent().height()),
                                atlasX,
                                atlasY};
            }
        }
    }
    auto data = codecDegradeProgramData(grade, placement, ctx.hScale);
    if (!data)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Codec Degrade geometry cannot be represented");
    GpuOcioTransformSpec leg;
    leg.kind = GpuOcioTransformKind::ColorGrade;
    leg.gradeKind = static_cast<std::uint8_t>(ColorGradeKind::CodecDegrade);
    leg.gradeData = std::move(*data);
    std::shared_ptr<const PreparedGpuOcioCommand> program;
    if (const auto error =
            prepareFieldLeg(leg, dispatchWindow, ctx, ocioContext, cancellation, program))
        return error;
    GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
    std::string key;
    if (const auto error = emitFieldDispatch(std::move(program), base, baseKey, dispatchWindow,
                                             display, aspect, operation, emit, charge, command, key))
        return error;
    if (dispatchWindow != output)
        if (const auto error = emitFieldMove(command, std::string(key), dispatchWindow, output, 0, 0,
                                             operation, emit, charge, command, key))
            return error;
    result.command = command;
    result.key = std::move(key);
    result.window = output;
    result.local = boundsForWindow(output, ctx.hScale, ctx.vScale);
    result.output = result.local;
    return std::nullopt;
}

// Narrows the reachable operations to what the output reads this frame. A Codec Degrade step after
// the current frame in its group of pictures is an exact identity of its input, so the optical flow
// and the coded colour that only it would read are not prepared: a keyframe emits only its own Encode
// and Finish passes, and a predicted frame the steps up to its position.
inline void pruneIdentityCodecInputs(const CompiledCompositionPlan& plan,
                                     const ResolvedEvaluation& frameResolved,
                                     const core::RationalTime requestTime,
                                     const std::size_t outputIndex, std::vector<bool>& reachable) {
    std::vector<bool> narrowed(reachable.size(), false);
    std::vector<std::size_t> pending{outputIndex};
    while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        if (index >= narrowed.size() || narrowed[index] || !reachable[index])
            continue;
        narrowed[index] = true;
        const auto& operation = plan.operations()[index];
        const auto* effect = std::get_if<CompiledImageEffect>(&operation);
        if (effect != nullptr && std::holds_alternative<CodecDegradeKernel>(effect->kernel)) {
            const auto operationIndex = OperationIndex::fromRaw(index);
            const auto clock = plan.operationClock(operationIndex);
            const auto values = sampleColorGradeOperands(
                effect->kernel, plan, resolvedForClock(frameResolved, clock),
                timeForClock(frameResolved, requestTime, clock));
            const auto grade = values ? prepareColorGrade(effect->kernel, *values) : std::nullopt;
            if (grade && grade->identity) {
                pending.push_back(effect->input.value());
                continue;
            }
        }
        forEachInput(operation,
                     [&pending](const OperationIndex input) { pending.push_back(input.value()); });
        if (const auto* layer = std::get_if<CompiledLayerOutput>(&operation);
            layer && layer->parent)
            pending.push_back(layer->parent->value());
    }
    reachable = std::move(narrowed);
}
