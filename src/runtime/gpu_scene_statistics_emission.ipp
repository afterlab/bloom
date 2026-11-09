// Included inside bloom::runtime::detail after the field emission helpers.
//
// #349 Image Statistics on the GPU. A pass is the one fixed program (ColorGradeKind::Statistics)
// dispatched over the image it follows, padded when the image it publishes is larger (a narrow, short
// frame has more tile rows to publish than it has pixels). The pass publishes a smaller window cut out
// of the dispatch at its origin: the tiles' statistics, or the statistics of the frame. The table and
// geometry are the CPU reference's (statisticsProgramData), so both evaluate one kernel over identical
// data.

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitStatisticsCommand(const CompiledImageEffect& effect, const PreparedColorGrade& grade,
                     const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                     const GpuSceneOcioContext& ocioContext, const CancellationToken& cancellation,
                     Emit&& emit, Charge&& charge, ChargeCoverage&& chargeCoverage,
                     GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                    "Image Statistics preparation was cancelled");
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
                    "The Image Statistics descriptor is not derivable");
    const auto source = descriptor->data;
    const auto display = descriptor->display;
    const auto aspect = descriptor->pixelAspect;
    const auto sourceWidth = static_cast<std::int64_t>(source.extent().width());
    const auto sourceHeight = static_cast<std::int64_t>(source.extent().height());
    const auto size = statisticsOutputSize(grade, sourceWidth, sourceHeight);
    // The result is relocated to the origin of the composition, as on the CPU reference.
    const auto created = render::ImageWindow::create(0, 0, static_cast<std::uint64_t>(size[0]),
                                                     static_cast<std::uint64_t>(size[1]));
    const auto padded = render::ImageWindow::create(
        source.originX(), source.originY(),
        static_cast<std::uint64_t>(std::max(sourceWidth, size[0])),
        static_cast<std::uint64_t>(std::max(sourceHeight, size[1])));
    if (!created || !padded)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "The Image Statistics output window overflows");
    const render::ImageWindow output = *created.value();
    const render::ImageWindow dispatchWindow = *padded.value();

    GpuSceneCommandIndex base = colour;
    std::string baseKey = colourKey;
    if (dispatchWindow != source) {
        if (const auto error = emitFieldMove(colour, colourKey, source, dispatchWindow, 0, 0,
                                             operation, emit, charge, base, baseKey))
            return error;
    }
    auto data = statisticsProgramData(grade, sourceWidth, sourceHeight);
    if (!data)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Image Statistics geometry cannot be represented");
    GpuOcioTransformSpec leg;
    leg.kind = GpuOcioTransformKind::ColorGrade;
    leg.gradeKind = static_cast<std::uint8_t>(ColorGradeKind::Statistics);
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
        if (const auto error = emitFieldMove(command, std::string(key), dispatchWindow, output,
                                             -source.originX(), -source.originY(), operation, emit,
                                             charge, command, key))
            return error;
    result.command = command;
    result.key = std::move(key);
    result.window = output;
    result.local = boundsForWindow(output, ctx.hScale, ctx.vScale);
    result.output = result.local;
    return std::nullopt;
}
