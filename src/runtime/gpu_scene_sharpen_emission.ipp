// Included inside bloom::runtime::detail after the field emission helpers.
//
// #486 Sharpen on the GPU. One resident two-input dispatch (ImageCombineV1's Unsharp mode) over the
// input and its blur, the operation before it: no atlas and no readback. The amount and threshold are
// its parameters, so changing either keeps the blur.

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitSharpenCommand(const CompiledImageEffect& effect, const PreparedColorGrade& grade,
                   const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                   const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
                   ChargeCoverage&& chargeCoverage, GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Sharpen preparation was cancelled");
    const auto& outputWindowOf = *ctx.outputWindowOf;
    const auto inputValue = effect.input.value();
    // An input with no pixels has nothing to sharpen, as on the CPU reference.
    if (!outputWindowOf[inputValue]) {
        result.identity = true;
        return std::nullopt;
    }
    if (effect.fieldImages.size() != 1 || effect.fieldImages[0].value() >= outputWindowOf.size())
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "The Sharpen blur is invalid");
    GpuSceneCommandIndex input = kInvalidGpuSceneCommand;
    std::string inputKey;
    if (const auto error = residentFieldInput(inputValue, operation, ctx, cancellation, emit, charge,
                                              chargeCoverage, input, inputKey, result))
        return error;
    const auto blurValue = effect.fieldImages[0].value();
    // A blur with no image reads transparent, so nothing is added, as on the CPU reference.
    const bool absent = !outputWindowOf[blurValue];
    GpuSceneCommandIndex blurred = input;
    std::string blurredKey = inputKey;
    if (!absent)
        if (const auto error = residentFieldInput(blurValue, operation, ctx, cancellation, emit,
                                                  charge, chargeCoverage, blurred, blurredKey, result))
            return error;
    const auto baseDescriptor = gpuSceneCommandDescriptorOf(*ctx.commands, input, 0);
    const auto blurDescriptor = gpuSceneCommandDescriptorOf(*ctx.commands, blurred, 0);
    if (!baseDescriptor || !blurDescriptor)
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "The Sharpen resident descriptors are not derivable");
    const auto outputWindow = baseDescriptor->data;
    if (const auto error = charge(outputWindow.extent().width(), outputWindow.extent().height(),
                                  sizeof(render::Rgba32f)))
        return error;
    render::GpuImageCombineParameters parameters{render::GpuImageCombineKind::Unsharp,
                                                 sharpenAmount(grade),
                                                 render::ImageCoverageChannel::Alpha,
                                                 false,
                                                 absent};
    parameters.threshold = sharpenThreshold(grade);
    const std::string identity(render::gpuImageCombineShaderIdentity());
    auto key = makeGpuSceneBlendSemanticKey(blurredKey, inputKey, core::BlendMode::Normal,
                                            blurDescriptor->data, outputWindow,
                                            baseDescriptor->pixelAspect, identity, parameters);
    result.command = emit(GpuSceneBlendCommand{.sourceOperation = operation,
                                               .source = blurred,
                                               .destination = input,
                                               .sourceKey = blurredKey,
                                               .destinationKey = inputKey,
                                               .mode = core::BlendMode::Normal,
                                               .imageCombine = parameters,
                                               .sourceWindow = blurDescriptor->data,
                                               .outputWindow = outputWindow,
                                               .pixelAspect = baseDescriptor->pixelAspect,
                                               .artifactDigest = identity,
                                               .semanticKey = key});
    result.key = std::move(key);
    result.window = outputWindow;
    result.local = boundsForWindow(outputWindow, ctx.hScale, ctx.vScale);
    result.output = result.local;
    return std::nullopt;
}
