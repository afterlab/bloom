// Included inside bloom::runtime::detail after the field emission helpers.
//
// #507 Lens Flare on the GPU. The stack is its one fixed program (ColorGradeKind::LensFlare) over the
// input padded to the frame. When it reads the input's brightness at the light or an occlusion
// matte, each probe is the other fixed program (ColorGradeKind::LensFlareProbe) over the light's
// sample disc cut out of its image, which leaves one pixel holding the disc's mean; those pixels sit
// in a column beside the frame (an atlas), the stack reads them there and the frame is cut back out.
// The table and geometry are the CPU reference's (lensFlareProgramData), so both evaluate one kernel
// over identical data.

template <typename Emit, typename Charge>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitLensFlareProbe(const GpuSceneCommandIndex image, const std::string& imageKey,
                   const render::ImageWindow imageWindow, const render::ImageWindow disc,
                   std::vector<float> data, const render::ImageWindow placed,
                   const render::ImageWindow display, const core::PixelAspectRatio aspect,
                   const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                   const GpuSceneOcioContext& ocioContext, const CancellationToken& cancellation,
                   Emit&& emit, Charge&& charge, GpuSceneCommandIndex& command, std::string& key) {
    GpuSceneCommandIndex cut = kInvalidGpuSceneCommand;
    std::string cutKey;
    if (const auto error =
            emitFieldMove(image, imageKey, imageWindow, disc, 0, 0, operation, emit, charge, cut, cutKey))
        return error;
    GpuOcioTransformSpec leg;
    leg.kind = GpuOcioTransformKind::ColorGrade;
    leg.gradeKind = static_cast<std::uint8_t>(ColorGradeKind::LensFlareProbe);
    leg.gradeData = std::move(data);
    std::shared_ptr<const PreparedGpuOcioCommand> program;
    if (const auto error = prepareFieldLeg(leg, disc, ctx, ocioContext, cancellation, program))
        return error;
    GpuSceneCommandIndex probed = kInvalidGpuSceneCommand;
    std::string probedKey;
    if (const auto error = emitFieldDispatch(std::move(program), cut, cutKey, disc, display, aspect,
                                             operation, emit, charge, probed, probedKey))
        return error;
    // The mean sits at the disc's first pixel.
    return emitFieldMove(probed, probedKey, disc, placed, placed.originX() - disc.originX(),
                         placed.originY() - disc.originY(), operation, emit, charge, command, key);
}

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitLensFlareCommand(const CompiledImageEffect& effect, const PreparedColorGrade& grade,
                     const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                     const GpuSceneOcioContext& ocioContext, const CancellationToken& cancellation,
                     Emit&& emit, Charge&& charge, ChargeCoverage&& chargeCoverage,
                     GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                    "Lens Flare preparation was cancelled");
    const auto& outputWindowOf = *ctx.outputWindowOf;
    const auto inputValue = effect.input.value();
    // An input with no pixels has no frame to light, as on the CPU reference.
    if (!outputWindowOf[inputValue]) {
        result.identity = true;
        return std::nullopt;
    }
    GpuSceneCommandIndex input = kInvalidGpuSceneCommand;
    std::string inputKey;
    if (const auto error = residentFieldInput(inputValue, operation, ctx, cancellation, emit, charge,
                                              chargeCoverage, input, inputKey, result))
        return error;
    const auto descriptor = gpuSceneCommandDescriptorOf(*ctx.commands, input, 0);
    if (!descriptor)
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "The Lens Flare input descriptor is not derivable");
    const auto inputWindow = descriptor->data;
    const auto display = descriptor->display;
    const auto aspect = descriptor->pixelAspect;
    const double pixelAspect =
        static_cast<double>(aspect.numerator()) / static_cast<double>(aspect.denominator());
    const auto output = lensFlareOutputWindow(inputWindow, display);
    if (!output)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "The Lens Flare window overflows");
    if (effect.fieldImages.size() > 1)
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "A Lens Flare image is invalid");
    const bool hasOcclusion = !effect.fieldImages.empty();
    const auto probes =
        lensFlareProbes(grade, display, ctx.hScale, ctx.vScale, hasOcclusion);
    if (!probes)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Lens Flare geometry cannot be represented");
    const auto overlaps = [](const render::ImageWindow& a, const render::ImageWindow& b) {
        return a.originX() < b.maxXExclusive() && b.originX() < a.maxXExclusive() &&
               a.originY() < b.maxYExclusive() && b.originY() < a.maxYExclusive();
    };

    // The probes that have pixels to read: a disc outside its image reads transparent, the constant
    // the program data then holds.
    struct Probe final {
        GpuSceneCommandIndex image = kInvalidGpuSceneCommand;
        std::string key;
        render::ImageWindow window;
        const std::vector<float>* data = nullptr;
        LensFlareProbeRead* read = nullptr;
    };
    LensFlareProbeRead sourceRead;
    LensFlareProbeRead occlusionRead;
    sourceRead.active = probes->source;
    occlusionRead.active = probes->occlusion;
    std::vector<Probe> reads;
    if (probes->source && overlaps(inputWindow, probes->window))
        reads.push_back({input, inputKey, inputWindow, &probes->sourceData, &sourceRead});
    if (probes->occlusion) {
        const auto value = effect.fieldImages[0].value();
        if (value >= outputWindowOf.size())
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "A Lens Flare image is invalid");
        if (outputWindowOf[value] && overlaps(*outputWindowOf[value], probes->window)) {
            Probe probe{kInvalidGpuSceneCommand, {}, *outputWindowOf[value], &probes->occlusionData,
                        &occlusionRead};
            if (const auto error =
                    residentFieldInput(value, operation, ctx, cancellation, emit, charge,
                                       chargeCoverage, probe.image, probe.key, result))
                return error;
            reads.push_back(std::move(probe));
        }
    }

    // The frame and a column beside it holding the probes' one pixel each.
    const auto atlasCreated = render::ImageWindow::create(
        output->originX(), output->originY(),
        output->extent().width() + static_cast<std::uint64_t>(reads.size()),
        output->extent().height());
    if (!atlasCreated)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "The Lens Flare atlas window overflows");
    const auto atlas = *atlasCreated.value();
    GpuSceneCommandIndex base = input;
    std::string baseKey = inputKey;
    if (atlas != inputWindow)
        if (const auto error = emitFieldMove(input, inputKey, inputWindow, atlas, 0, 0, operation,
                                             emit, charge, base, baseKey))
            return error;
    const auto columns = static_cast<std::int64_t>(output->extent().width());
    for (std::size_t slot = 0; slot < reads.size(); ++slot) {
        const auto atlasX = columns + static_cast<std::int64_t>(slot);
        const auto placed =
            render::ImageWindow::create(atlas.originX() + atlasX, atlas.originY(), 1, 1);
        if (!placed)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "A Lens Flare probe does not fit the atlas");
        GpuSceneCommandIndex moved = kInvalidGpuSceneCommand;
        std::string movedKey;
        auto& probe = reads[slot];
        if (const auto error = emitLensFlareProbe(
                probe.image, probe.key, probe.window, probes->window, *probe.data, *placed.value(),
                display, aspect, operation, ctx, ocioContext, cancellation, emit, charge, moved,
                movedKey))
            return error;
        if (const auto error =
                charge(atlas.extent().width(), atlas.extent().height(), sizeof(render::Rgba32f)))
            return error;
        auto blendKey = makeGpuSceneBlendSemanticKey(movedKey, baseKey, core::BlendMode::Normal,
                                                     *placed.value(), atlas, aspect,
                                                     std::string{kGpuBlendSpirvSha256});
        base = emit(GpuSceneBlendCommand{.sourceOperation = operation,
                                         .source = moved,
                                         .destination = base,
                                         .sourceKey = movedKey,
                                         .destinationKey = baseKey,
                                         .mode = core::BlendMode::Normal,
                                         .imageCombine = {},
                                         .sourceWindow = *placed.value(),
                                         .outputWindow = atlas,
                                         .pixelAspect = aspect,
                                         .artifactDigest = std::string{kGpuBlendSpirvSha256},
                                         .semanticKey = blendKey});
        baseKey = std::move(blendKey);
        probe.read->x = static_cast<float>(atlasX);
        probe.read->y = 0.0F;
    }
    auto data = lensFlareProgramData(grade, *output, display, pixelAspect, ctx.hScale, ctx.vScale,
                                     sourceRead, occlusionRead);
    if (!data)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Lens Flare geometry cannot be represented");
    GpuOcioTransformSpec leg;
    leg.kind = GpuOcioTransformKind::ColorGrade;
    leg.gradeKind = static_cast<std::uint8_t>(ColorGradeKind::LensFlare);
    leg.gradeData = std::move(*data);
    std::shared_ptr<const PreparedGpuOcioCommand> program;
    if (const auto error = prepareFieldLeg(leg, atlas, ctx, ocioContext, cancellation, program))
        return error;
    GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
    std::string key;
    if (const auto error = emitFieldDispatch(std::move(program), base, baseKey, atlas, display,
                                             aspect, operation, emit, charge, command, key))
        return error;
    if (atlas != *output)
        if (const auto error = emitFieldMove(command, std::string(key), atlas, *output, 0, 0,
                                             operation, emit, charge, command, key))
            return error;
    result.command = command;
    result.key = std::move(key);
    result.window = *output;
    result.local = boundsForWindow(*output, ctx.hScale, ctx.vScale);
    result.output = result.local;
    return std::nullopt;
}
