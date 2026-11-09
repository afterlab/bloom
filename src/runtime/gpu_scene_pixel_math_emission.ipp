// Included inside bloom::runtime::detail after the field emission helpers.
//
// #481 Image Math with a second image on the GPU. The one fixed program (ColorGradeKind::PixelMath)
// is dispatched over an atlas twice the output window wide: the input padded to the atlas (one exact
// move) with the second image, moved exactly so that its pixel at a global position lies one window
// width to the right of the input's pixel at that position, blended over it in Normal mode (the
// regions are disjoint, so each pixel is one image's exact value). A second image that covers less
// than the window is transparent black beyond its own pixels, one that covers more is cut to the
// window, as the CPU reference reads it. The program computes inside the output window and copies
// the rest; the output window is then cut back out. A second image with no pixels is transparent
// black and needs no atlas. The table and geometry are the CPU reference's (pixelMathProgramData).

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitPixelMathSecondCommand(const CompiledImageEffect& effect, const PreparedColorGrade& grade,
                           const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                           const GpuSceneOcioContext& ocioContext,
                           const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
                           ChargeCoverage&& chargeCoverage, GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                    "Image Math preparation was cancelled");
    const auto& outputWindowOf = *ctx.outputWindowOf;
    const auto inputValue = effect.input.value();
    if (!effect.secondaryInput || effect.secondaryInput->value() >= outputWindowOf.size())
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "The Image Math second input is invalid");
    const auto secondValue = effect.secondaryInput->value();
    // An input with no pixels publishes none, as on the CPU reference.
    if (!outputWindowOf[inputValue]) {
        result.identity = true;
        return std::nullopt;
    }
    GpuSceneCommandIndex colour = kInvalidGpuSceneCommand;
    std::string colourKey;
    if (const auto error = residentFieldInput(inputValue, operation, ctx, cancellation, emit, charge,
                                              chargeCoverage, colour, colourKey, result))
        return error;
    const auto descriptor = gpuSceneCommandDescriptorOf(*ctx.commands, colour, 0);
    if (!descriptor)
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "The Image Math descriptor is not derivable");
    const auto source = descriptor->data;
    const auto display = descriptor->display;
    const auto aspect = descriptor->pixelAspect;
    const auto width = static_cast<std::int64_t>(source.extent().width());
    const auto height = static_cast<std::int64_t>(source.extent().height());
    const bool hasSecond = outputWindowOf[secondValue].has_value();

    GpuSceneCommandIndex base = colour;
    std::string baseKey = colourKey;
    render::ImageWindow dispatchWindow = source;
    if (hasSecond) {
        const auto secondWindow = *outputWindowOf[secondValue];
        GpuSceneCommandIndex secondCommand = kInvalidGpuSceneCommand;
        std::string secondKey;
        if (const auto error =
                residentFieldInput(secondValue, operation, ctx, cancellation, emit, charge,
                                   chargeCoverage, secondCommand, secondKey, result))
            return error;
        const auto atlas = render::ImageWindow::create(source.originX(), source.originY(),
                                                       static_cast<std::uint64_t>(2 * width),
                                                       static_cast<std::uint64_t>(height));
        const auto placed = render::ImageWindow::create(source.originX() + width, source.originY(),
                                                        source.extent().width(),
                                                        source.extent().height());
        if (!atlas || !placed)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "The Image Math atlas window overflows");
        dispatchWindow = *atlas.value();
        if (const auto error = emitFieldMove(colour, colourKey, source, dispatchWindow, 0, 0,
                                             operation, emit, charge, base, baseKey))
            return error;
        GpuSceneCommandIndex moved = kInvalidGpuSceneCommand;
        std::string movedKey;
        if (const auto error = emitFieldMove(secondCommand, secondKey, secondWindow, *placed.value(),
                                             width, 0, operation, emit, charge, moved, movedKey))
            return error;
        if (const auto error = charge(dispatchWindow.extent().width(),
                                      dispatchWindow.extent().height(), sizeof(render::Rgba32f)))
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
    }
    auto data = pixelMathProgramData(grade, hasSecond ? width : 0);
    if (!data)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Image Math geometry cannot be represented");
    GpuOcioTransformSpec leg;
    leg.kind = GpuOcioTransformKind::ColorGrade;
    leg.gradeKind = static_cast<std::uint8_t>(ColorGradeKind::PixelMath);
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
    if (dispatchWindow != source)
        if (const auto error = emitFieldMove(command, std::string(key), dispatchWindow, source, 0, 0,
                                             operation, emit, charge, command, key))
            return error;
    result.command = command;
    result.key = std::move(key);
    result.window = source;
    result.local = boundsForWindow(source, ctx.hScale, ctx.vScale);
    result.output = result.local;
    return std::nullopt;
}
