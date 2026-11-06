// Included inside bloom::runtime::detail after the field emission helpers.
//
// #484 Motion blur on the GPU. Every motion kernel runs the pass plan its CPU reference runs
// (planMotion, motion_blur_image_effect.hpp), step for step: a Place is its first source moved
// exactly into the step's window (padded with zero, cropped) and every further source moved into
// place and blended over it in Normal mode (the plan's regions are disjoint, so each pixel is one
// source's exact value); a Run is the one fixed motion program (ColorGradeKind::Motion) over the
// whole source with the step's table. The tables and windows are the CPU reference's, so both
// evaluate one kernel over identical data. No pixel leaves the device: every intermediate is a
// resident command.

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitMotionCommand(const CompiledImageEffect& effect, const PreparedColorGrade& grade,
                  const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                  const GpuSceneOcioContext& ocioContext, const CancellationToken& cancellation,
                  Emit&& emit, Charge&& charge, ChargeCoverage&& chargeCoverage,
                  GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                    "Motion blur preparation was cancelled");
    const auto& outputWindowOf = *ctx.outputWindowOf;
    const auto count = motionInputCount(effect.kernel);
    // The plan's inputs: the effect input, the accumulated sample, or the kernel's slots.
    std::vector<std::optional<std::size_t>> sources(count);
    sources[0] = effect.input.value();
    if (std::holds_alternative<TemporalAccumulateKernel>(effect.kernel) && effect.secondaryInput)
        sources[1] = effect.secondaryInput->value();
    const auto slot = [&](const std::size_t at, const std::int8_t image)
        -> std::optional<GpuSceneLeafFailure> {
        if (image < 0)
            return std::nullopt;
        if (static_cast<std::size_t>(image) >= effect.fieldImages.size())
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "A motion slot is invalid");
        sources[at] = effect.fieldImages[static_cast<std::size_t>(image)].value();
        return std::nullopt;
    };
    if (const auto* vector = std::get_if<VectorBlurKernel>(&effect.kernel))
        for (std::size_t s = 0; s < kVectorSlots; ++s)
            if (const auto error = slot(1 + s, vector->slotImages[s]))
                return error;
    if (const auto* flow = std::get_if<OpticalFlowKernel>(&effect.kernel))
        for (std::size_t s = 0; s < flow->slotImages.size(); ++s)
            if (const auto error = slot(1 + s, flow->slotImages[s]))
                return error;

    struct Resident final {
        GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
        std::string key;
        std::optional<render::ImageWindow> window;
    };
    std::vector<Resident> inputs(count);
    std::vector<MotionRect> rects(count);
    std::optional<render::Rgba32fImageDescriptor> colour;
    for (std::size_t i = 0; i < count; ++i) {
        if (!sources[i])
            continue;
        const auto value = *sources[i];
        if (value >= outputWindowOf.size())
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "A motion image is invalid");
        // An image with no pixels is an absent input, as on the CPU reference.
        if (!outputWindowOf[value])
            continue;
        auto& resident = inputs[i];
        if (const auto error = residentFieldInput(value, operation, ctx, cancellation, emit, charge,
                                                  chargeCoverage, resident.command, resident.key,
                                                  result))
            return error;
        resident.window = *outputWindowOf[value];
        rects[i] = {resident.window->originX(), resident.window->originY(),
                    static_cast<std::int64_t>(resident.window->extent().width()),
                    static_cast<std::int64_t>(resident.window->extent().height())};
        if (!colour) {
            const auto descriptor = gpuSceneCommandDescriptorOf(*ctx.commands, resident.command, 0);
            if (!descriptor)
                return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                            "The motion input descriptor is not derivable");
            const auto created = render::Rgba32fImageDescriptor::create(
                descriptor->data, descriptor->display, descriptor->pixelAspect);
            if (created)
                colour = *created.value();
            if (!colour)
                return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                            "The motion input descriptor is invalid");
        }
    }
    // Nothing to read publishes nothing, as on the CPU reference.
    if (!colour) {
        result.command = kInvalidGpuSceneCommand;
        result.window = std::nullopt;
        return std::nullopt;
    }
    const auto display = colour->displayWindow();
    const auto aspect = colour->pixelAspect();
    MotionGeometry geometry;
    geometry.display = {display.originX(), display.originY(),
                        static_cast<std::int64_t>(display.extent().width()),
                        static_cast<std::int64_t>(display.extent().height())};
    geometry.pixelAspect = static_cast<double>(aspect.numerator()) /
                           static_cast<double>(aspect.denominator());
    geometry.horizontalScale = ctx.hScale;
    geometry.verticalScale = ctx.vScale;
    if (ctx.bounds != nullptr && effect.input.value() < ctx.bounds->size()) {
        const auto centre = (*ctx.bounds)[effect.input.value()].output.centre();
        geometry.centreX = centre.x;
        geometry.centreY = centre.y;
    }
    const auto plan = planMotion(effect.kernel, grade, rects, geometry);
    if (!plan)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Motion blur geometry cannot be represented");
    if (!plan->output) {
        result.command = kInvalidGpuSceneCommand;
        result.window = std::nullopt;
        return std::nullopt;
    }
    std::vector<Resident> buffers(plan->buffers);
    for (std::size_t i = 0; i < count; ++i)
        buffers[i] = inputs[i];
    const auto windowOf = [](const MotionRect r) {
        return render::ImageWindow::create(r.x, r.y, static_cast<std::uint64_t>(r.width),
                                           static_cast<std::uint64_t>(r.height));
    };
    // Only the effect's output is worth keeping: a solver writes hundreds of frame-sized atlases
    // that nothing can ask for again, and retaining them cost seconds at 1080p once they outgrew the
    // device (and would evict everything useful in a bounded cache).
    std::size_t lastOutputStep = plan->steps.size();
    for (std::size_t index = 0; index < plan->steps.size(); ++index)
        if (plan->steps[index].target == *plan->output)
            lastOutputStep = index;
    bool keepResult = false;
    std::uint16_t diagnosticPass = 0;
    const auto emitPass = [&emit, &keepResult, &diagnosticPass](auto command) {
        if constexpr (requires { command.transientResult; })
            command.transientResult = !keepResult;
        if constexpr (requires { command.diagnosticPass; })
            command.diagnosticPass = diagnosticPass;
        return emit(std::move(command));
    };
    for (std::size_t stepIndex = 0; stepIndex < plan->steps.size(); ++stepIndex) {
        const auto& step = plan->steps[stepIndex];
        keepResult = stepIndex == lastOutputStep;
        diagnosticPass = step.kind == MotionStepKind::Place || step.table.empty()
                             ? std::uint16_t{0}
                             : static_cast<std::uint16_t>(step.table[0]);
        if (cancellation.isCancellationRequested())
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                        "Motion blur preparation was cancelled");
        const auto targetCreated = windowOf(step.window);
        if (!targetCreated)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "A motion blur window overflows");
        const auto target = *targetCreated.value();
        auto& out = buffers[step.target];
        if (step.kind == MotionStepKind::Place) {
            bool first = true;
            for (const auto& placement : step.sources) {
                const auto& source = buffers[placement.buffer];
                if (!source.window || source.command == kInvalidGpuSceneCommand)
                    return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                                "A motion blur source is not resident");
                if (first) {
                    if (const auto error = emitFieldMove(source.command, source.key, *source.window,
                                                         target, placement.shiftX,
                                                         placement.shiftY, operation, emitPass, charge,
                                                         out.command, out.key))
                        return error;
                    out.window = target;
                    first = false;
                    continue;
                }
                // The source moved to its place (within the target), then blended over the base.
                const MotionRect moved{source.window->originX() + placement.shiftX,
                                       source.window->originY() + placement.shiftY,
                                       static_cast<std::int64_t>(source.window->extent().width()),
                                       static_cast<std::int64_t>(source.window->extent().height())};
                const auto x0 = std::max(moved.x, step.window.x);
                const auto y0 = std::max(moved.y, step.window.y);
                const auto x1 = std::min(moved.x + moved.width, step.window.x + step.window.width);
                const auto y1 =
                    std::min(moved.y + moved.height, step.window.y + step.window.height);
                if (x1 <= x0 || y1 <= y0)
                    continue;
                const auto placed = windowOf({x0, y0, x1 - x0, y1 - y0});
                if (!placed)
                    return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                                "A motion blur image does not fit its atlas");
                GpuSceneCommandIndex movedCommand = kInvalidGpuSceneCommand;
                std::string movedKey;
                if (const auto error = emitFieldMove(source.command, source.key, *source.window,
                                                     *placed.value(), placement.shiftX,
                                                     placement.shiftY, operation, emitPass, charge,
                                                     movedCommand, movedKey))
                    return error;
                if (const auto error = charge(target.extent().width(), target.extent().height(),
                                              sizeof(render::Rgba32f)))
                    return error;
                auto blendKey = makeGpuSceneBlendSemanticKey(
                    movedKey, out.key, core::BlendMode::Normal, *placed.value(), target, aspect,
                    std::string{kGpuBlendSpirvSha256});
                out.command = emitPass(GpuSceneBlendCommand{.sourceOperation = operation,
                                                        .source = movedCommand,
                                                        .destination = out.command,
                                                        .sourceKey = movedKey,
                                                        .destinationKey = out.key,
                                                        .mode = core::BlendMode::Normal,
                                                        .imageCombine = {},
                                                        .sourceWindow = *placed.value(),
                                                        .outputWindow = target,
                                                        .pixelAspect = aspect,
                                                        .artifactDigest =
                                                            std::string{kGpuBlendSpirvSha256},
                                                        .semanticKey = blendKey});
                out.key = std::move(blendKey);
            }
            if (first)
                return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                            "A motion blur placement has no source");
            continue;
        }
        const auto& source = buffers[step.source];
        if (!source.window || source.command == kInvalidGpuSceneCommand)
            return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                        "A motion blur pass reads no resident image");
        GpuOcioTransformSpec leg;
        leg.kind = GpuOcioTransformKind::ColorGrade;
        leg.gradeKind = static_cast<std::uint8_t>(ColorGradeKind::Motion);
        leg.gradeData = step.table;
        std::shared_ptr<const PreparedGpuOcioCommand> program;
        if (const auto error =
                prepareFieldLeg(leg, *source.window, ctx, ocioContext, cancellation, program))
            return error;
        if (const auto error = emitFieldDispatch(std::move(program), source.command, source.key,
                                                 *source.window, display, aspect, operation, emitPass,
                                                 charge, out.command, out.key))
            return error;
        out.window = source.window;
    }
    const auto& output = buffers[*plan->output];
    if (!output.window || output.command == kInvalidGpuSceneCommand)
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "The motion blur output is not resident");
    result.command = output.command;
    result.key = output.key;
    result.window = *output.window;
    result.local = boundsForWindow(*output.window, ctx.hScale, ctx.vScale);
    result.output = result.local;
    return std::nullopt;
}
