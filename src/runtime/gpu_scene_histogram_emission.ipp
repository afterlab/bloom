// Included inside bloom::runtime::detail after the field emission helpers.
//
// #349 Histogram on the GPU. A pass is the one fixed program (ColorGradeKind::Histogram) dispatched
// over the image it follows, padded when the image it publishes is larger (a short, narrow frame has
// more tile counts to publish than it has pixels, and a plot is larger than its counts): the
// followed image padded to the dispatch window (one exact move). The pass publishes a smaller window
// cut out of the dispatch at its origin: the tile counts, then the sums of them, then the plot. The
// table and geometry are the CPU reference's (histogramProgramData), so both evaluate one kernel
// over identical data.

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitHistogramCommand(const CompiledImageEffect& effect, const PreparedColorGrade& grade,
                     const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                     const GpuSceneOcioContext& ocioContext, const CancellationToken& cancellation,
                     Emit&& emit, Charge&& charge, ChargeCoverage&& chargeCoverage,
                     GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                    "Histogram preparation was cancelled");
    const auto& outputWindowOf = *ctx.outputWindowOf;
    const auto inputValue = effect.input.value();
    // A pass with no image to follow publishes none, as on the CPU reference.
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
                    "The Histogram descriptor is not derivable");
    const auto source = descriptor->data;
    const auto display = descriptor->display;
    const auto aspect = descriptor->pixelAspect;
    const auto sourceWidth = static_cast<std::int64_t>(source.extent().width());
    const auto sourceHeight = static_cast<std::int64_t>(source.extent().height());
    const auto size = histogramOutputSize(grade, sourceWidth, sourceHeight);
    const auto created = render::ImageWindow::create(
        source.originX(), source.originY(), static_cast<std::uint64_t>(size[0]),
        static_cast<std::uint64_t>(size[1]));
    const auto padded = render::ImageWindow::create(
        source.originX(), source.originY(),
        static_cast<std::uint64_t>(std::max(sourceWidth, size[0])),
        static_cast<std::uint64_t>(std::max(sourceHeight, size[1])));
    if (!created || !padded)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "The Histogram output window overflows");
    const render::ImageWindow output = *created.value();
    const render::ImageWindow dispatchWindow = *padded.value();

    GpuSceneCommandIndex base = colour;
    std::string baseKey = colourKey;
    if (dispatchWindow != source) {
        if (const auto error = emitFieldMove(colour, colourKey, source, dispatchWindow, 0, 0,
                                             operation, emit, charge, base, baseKey))
            return error;
    }
    auto data = histogramProgramData(grade, sourceWidth, sourceHeight);
    if (!data)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Histogram geometry cannot be represented");
    GpuOcioTransformSpec leg;
    leg.kind = GpuOcioTransformKind::ColorGrade;
    leg.gradeKind = static_cast<std::uint8_t>(ColorGradeKind::Histogram);
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
