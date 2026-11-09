// Included inside bloom::runtime::detail after the field emission helpers.
//
// #514 Temporal denoise on the GPU. A step is the one fixed denoise program
// (bloom.render.denoise.v1, its Temporal branch) dispatched over the running mean with the current
// frame and the earlier frame beside it: an atlas as Codec Degrade's, the mean padded to it (one
// exact move) with each other image moved exactly into place and blended over it in Normal mode
// (the regions are disjoint, so each pixel is one image's exact value). On the first step the mean
// is the current frame itself, so only the earlier frame is placed. The table and geometry are the
// CPU reference's (temporalDenoiseProgramData), so both evaluate one kernel over identical data;
// the step's output is cut back to the mean's window.

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitTemporalDenoiseCommand(const CompiledImageEffect& effect, const PreparedDenoise& denoise,
                           const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                           const GpuSceneOcioContext& ocioContext,
                           const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
                           ChargeCoverage&& chargeCoverage, GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                    "Temporal denoise preparation was cancelled");
    const auto& outputWindowOf = *ctx.outputWindowOf;
    const auto inputValue = effect.input.value();
    if (effect.fieldImages.size() != 2 || effect.fieldImages[0].value() >= outputWindowOf.size() ||
        effect.fieldImages[1].value() >= outputWindowOf.size())
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "The temporal denoise frames are invalid");
    // A mean with no pixels has a current frame with none: nothing to weigh, as on the CPU.
    if (!outputWindowOf[inputValue] || !outputWindowOf[effect.fieldImages[0].value()]) {
        result.identity = true;
        return std::nullopt;
    }
    GpuSceneCommandIndex mean = kInvalidGpuSceneCommand;
    std::string meanKey;
    if (const auto error = residentFieldInput(inputValue, operation, ctx, cancellation, emit, charge,
                                              chargeCoverage, mean, meanKey, result))
        return error;
    const auto descriptor = gpuSceneCommandDescriptorOf(*ctx.commands, mean, 0);
    if (!descriptor)
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "The temporal denoise descriptor is not derivable");
    const auto output = descriptor->data;
    const auto display = descriptor->display;
    const auto aspect = descriptor->pixelAspect;
    const auto outW = static_cast<std::int64_t>(output.extent().width());
    const auto outH = static_cast<std::int64_t>(output.extent().height());

    // The cells beside the mean: the current frame (unless the mean is it) and the earlier frame.
    struct Slot final {
        TemporalDenoiseCell* cell = nullptr;
        GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
        std::string key;
        render::ImageWindow window;
    };
    TemporalDenoiseCell current{0, 0, 0, 0, outW, outH};
    TemporalDenoiseCell earlier{};
    std::vector<Slot> slots;
    const std::array<std::pair<std::size_t, TemporalDenoiseCell*>, 2> wanted{
        {{effect.fieldImages[0].value(), &current}, {effect.fieldImages[1].value(), &earlier}}};
    for (const auto& [value, cell] : wanted) {
        // The first step's mean is its current frame: read it where the mean is.
        if (cell == &current && value == inputValue)
            continue;
        // An image with no pixels reads transparent, as on the CPU reference.
        if (!outputWindowOf[value])
            continue;
        Slot slot{cell, kInvalidGpuSceneCommand, {}, *outputWindowOf[value]};
        if (const auto error = residentFieldInput(value, operation, ctx, cancellation, emit, charge,
                                                  chargeCoverage, slot.command, slot.key, result))
            return error;
        slots.push_back(std::move(slot));
    }
    std::int64_t rowWidth = outW, rowHeight = outH, columnWidth = outW, columnHeight = outH;
    for (const auto& slot : slots) {
        const auto w = static_cast<std::int64_t>(slot.window.extent().width());
        const auto h = static_cast<std::int64_t>(slot.window.extent().height());
        rowWidth += w;
        rowHeight = std::max(rowHeight, h);
        columnWidth = std::max(columnWidth, w);
        columnHeight += h;
    }
    GpuSceneCommandIndex base = mean;
    std::string baseKey = meanKey;
    render::ImageWindow dispatchWindow = output;
    if (!slots.empty()) {
        const bool row = std::max(rowWidth, rowHeight) <= std::max(columnWidth, columnHeight);
        const auto atlasCreated = render::ImageWindow::create(
            output.originX(), output.originY(),
            static_cast<std::uint64_t>(row ? rowWidth : columnWidth),
            static_cast<std::uint64_t>(row ? rowHeight : columnHeight));
        if (!atlasCreated)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "The temporal denoise atlas window overflows");
        dispatchWindow = *atlasCreated.value();
        if (const auto error = emitFieldMove(mean, meanKey, output, dispatchWindow, 0, 0, operation,
                                             emit, charge, base, baseKey))
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
                            "A temporal denoise frame does not fit the atlas");
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
                movedKey, baseKey, core::BlendMode::Normal, *placed.value(), dispatchWindow, aspect,
                std::string{kGpuBlendSpirvSha256});
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
                                             .artifactDigest = std::string{kGpuBlendSpirvSha256},
                                             .semanticKey = blendKey});
            baseKey = std::move(blendKey);
            // A pixel p of the mean (relative to its origin) is at global origin + p; the cell
            // holds global g at atlas (g - window origin) + (atlasX, atlasY).
            const auto lowX = slot.window.originX() - output.originX();
            const auto lowY = slot.window.originY() - output.originY();
            *slot.cell = {atlasX - lowX,
                          atlasY - lowY,
                          lowX,
                          lowY,
                          static_cast<std::int64_t>(slot.window.extent().width()),
                          static_cast<std::int64_t>(slot.window.extent().height())};
        }
    }
    auto data = temporalDenoiseProgramData(denoise, outW, outH, current, earlier);
    if (!data)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Temporal denoise geometry cannot be represented");
    GpuOcioTransformSpec leg;
    leg.kind = GpuOcioTransformKind::Denoise;
    leg.denoise = std::move(*data);
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
