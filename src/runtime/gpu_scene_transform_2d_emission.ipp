// Included inside bloom::runtime::detail after the shared effect-emission vocabulary.
//
// #336 standalone Transform 2D: the CPU reference's checked geometry (transform_2d_geometry.hpp)
// emitted through the Layer Output's raster route: one resident TranslationOpacity pass for a pure
// move, otherwise one AffineBilinearV1 command. The display window and pixel aspect follow
// the input, the content bounds follow the input, and a degenerate or fully clipped transform
// publishes no command, exactly as the CPU publishes no image.
template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitTransform2DCommand(const Transform2DKernel& kernel, const OperationIndex input,
                       const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                       const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
                       ChargeCoverage&& chargeCoverage, GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                    "Transform 2D preparation was cancelled");
    if (!validTransform2DKernel(kernel))
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Transform 2D extent or filter is invalid");
    const auto& plan = *ctx.plan;
    const auto& resolved = *ctx.resolved;
    const auto translate = resolveParameter(kernel.translate, plan, resolved);
    const auto pivot = resolveParameter(kernel.pivot, plan, resolved);
    const auto scale = resolveParameter(kernel.scale, plan, resolved);
    const auto rotation = resolveParameter(kernel.rotation, plan, resolved);
    if (!translate || !pivot || !scale || !rotation)
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Transform 2D parameters are not evaluable");
    const auto& inputBounds = (*ctx.bounds)[input.value()].output;
    const PreparedTransform2D prepared{kernel.extent, kernel.filter,
                                       Transform2DValues{translate->value, pivot->value,
                                                         scale->value, rotation->value,
                                                         inputBounds.centre()}};
    if (!validTransform2DValues(prepared.values))
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Transform 2D parameters are not finite");
    const auto inputWindow = (*ctx.outputWindowOf)[input.value()];
    if (!inputWindow || prepared.identity()) {
        result.identity = true;
        return std::nullopt;
    }
    auto inputCommand = (*ctx.commandForOperation)[input.value()];
    auto inputKey = (*ctx.keyOf)[input.value()];
    if (inputCommand == kInvalidGpuSceneCommand) {
        const auto& chain = (*ctx.vectors)[input.value()];
        if (!chain || chain->source >= plan.operations().size())
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "Transform 2D input cannot be materialized");
        const auto& leafResolved =
            ctx.frameResolved == nullptr
                ? resolved
                : resolvedForClock(*ctx.frameResolved,
                                   plan.operationClock(OperationIndex::fromRaw(chain->source)));
        if (const auto error = emitVectorLeafLayer(
                plan.operations()[chain->source], plan, leafResolved, chain->matrix, *inputWindow,
                ctx.fullDisplayWindow, ctx.fullPixelAspect, ctx.hScale, ctx.vScale, chain->opacity,
                false, ctx.allowance, operation, ctx.coverageCache, cancellation, emit, charge,
                chargeCoverage, inputCommand, inputKey, result.consumedText, result.consumedShape))
            return error;
        result.consumedLeaf = chain->source;
    }
    const auto source = gpuSceneCommandDescriptorOf(*ctx.commands, inputCommand, 0);
    if (!source || source->data != *inputWindow)
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "Transform 2D input descriptor does not match its data window");
    const auto sourceDescriptor =
        render::Rgba32fImageDescriptor::create(source->data, source->display, source->pixelAspect);
    if (!sourceDescriptor)
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Transform 2D input descriptor is invalid");
    const auto geometry =
        resolveTransform2DGeometry(prepared, *sourceDescriptor.value(), ctx.hScale, ctx.vScale);
    if (!geometry)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedTransform,
                    "Transform 2D exceeds the supported coordinate range");
    result.local = inputBounds;
    result.output = inputBounds;
    if (!geometry->window) {
        // No pixels: no command, no window. Downstream treats the operation as empty content.
        result.command = kInvalidGpuSceneCommand;
        result.window = std::nullopt;
        return std::nullopt;
    }
    const auto output = *geometry->window;
    // Exactly the Layer Output's raster route: its exact translation pass when the resolved map
    // only moves, otherwise the accepted affine command for the same author-space matrix.
    GpuSceneCommandIndex emitted = kInvalidGpuSceneCommand;
    std::string key;
    if (const auto delta = translationOnlyDeviceDelta(*geometry->transform, source->data, output)) {
        if (const auto error = emitTranslationOpacityCommand(
                inputCommand, inputKey, source->data, output, delta->first, delta->second, 1.0,
                source->pixelAspect, operation, charge, emit, emitted, key))
            return error;
    } else if (const auto error = emitAffineLayerCommand(
                   inputCommand, inputKey, source->data, output, geometry->matrix, ctx.hScale,
                   ctx.vScale, 1.0, source->pixelAspect, operation, charge, emit, emitted, key)) {
        return error;
    }
    result.command = emitted;
    result.key = std::move(key);
    result.window = output;
    return std::nullopt;
}
