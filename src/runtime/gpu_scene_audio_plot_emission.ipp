// Included inside bloom::runtime::detail after the field emission helpers.
//
// #347 Audio Waveform and Audio Spectrum on the GPU. A plot is the one fixed program
// (ColorGradeKind::AudioPlot) dispatched over the canvas it follows, whose window is exactly the
// plot's width and height. The table is the CPU reference's (prepareAudioPlotGrade) and carries the
// analysis a worker prepared, so both evaluate one program over identical data.

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitAudioPlotCommand(const CompiledImageEffect& effect, const PreparedColorGrade& grade,
                     const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                     const GpuSceneOcioContext& ocioContext, const CancellationToken& cancellation,
                     Emit&& emit, Charge&& charge, ChargeCoverage&& chargeCoverage,
                     GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                    "Audio plot preparation was cancelled");
    const auto& outputWindowOf = *ctx.outputWindowOf;
    const auto inputValue = effect.input.value();
    // A plot with no canvas to follow publishes none, as on the CPU reference.
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
                    "The audio plot descriptor is not derivable");
    const auto source = descriptor->data;
    if (!validAudioPlotProgramData(grade.data) ||
        static_cast<std::int64_t>(source.extent().width()) != static_cast<std::int64_t>(grade.data[1]) ||
        static_cast<std::int64_t>(source.extent().height()) != static_cast<std::int64_t>(grade.data[2]))
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "The audio plot canvas is not the size of its analysis");
    GpuOcioTransformSpec leg;
    leg.kind = GpuOcioTransformKind::ColorGrade;
    leg.gradeKind = static_cast<std::uint8_t>(ColorGradeKind::AudioPlot);
    leg.gradeData = grade.data;
    std::shared_ptr<const PreparedGpuOcioCommand> program;
    if (const auto error = prepareFieldLeg(leg, source, ctx, ocioContext, cancellation, program))
        return error;
    GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
    std::string key;
    if (const auto error = emitFieldDispatch(std::move(program), colour, colourKey, source,
                                             descriptor->display, descriptor->pixelAspect, operation,
                                             emit, charge, command, key))
        return error;
    result.command = command;
    result.key = std::move(key);
    result.window = source;
    result.local = boundsForWindow(source, ctx.hScale, ctx.vScale);
    result.output = result.local;
    return std::nullopt;
}
