// Included inside bloom::runtime::detail after the shared effect-emission vocabulary.
//
// #525 Fields on the GPU. A field raster is the one fixed field program (ColorGradeKind::FieldRaster)
// dispatched over an image holding its target and its program's images: the slots are moved beside
// the target (an atlas, a source-over Merge of exact translations over a transparent base), the
// program reads each slot at its atlas position, and the target is cut back out. Without images the
// dispatched image is the target's own transparent window. The table is the CPU reference's, with
// the same geometry (fieldRasterProgramData), so both evaluate one kernel over identical data.
//
// A mask's signed distance is the two fixed exact passes (buildMaskDistanceProgram) over the mask
// padded by the reach, as on the CPU reference (applyMaskDistance).

// The image of plan operation `value` as a resident command, materializing a deferred vector leaf.
template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
residentFieldInput(const std::size_t value, const OperationIndex operation,
                   const GpuSceneEffectEmissionContext& ctx, const CancellationToken& cancellation,
                   Emit&& emit, Charge&& charge, ChargeCoverage&& chargeCoverage,
                   GpuSceneCommandIndex& command, std::string& key,
                   GpuSceneEffectEmission& result) {
    command = (*ctx.commandForOperation)[value];
    key = (*ctx.keyOf)[value];
    if (command != kInvalidGpuSceneCommand)
        return std::nullopt;
    const auto& chain = (*ctx.vectors)[value];
    const auto& window = (*ctx.outputWindowOf)[value];
    if (!chain || !window || chain->source >= ctx.plan->operations().size())
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "A field image is not materializable");
    const auto& leafResolved =
        ctx.frameResolved == nullptr
            ? *ctx.resolved
            : resolvedForClock(*ctx.frameResolved,
                               ctx.plan->operationClock(OperationIndex::fromRaw(chain->source)));
    GpuSceneEffectEmission::ConsumedLeaf consumed{chain->source, false, false};
    if (const auto error = emitVectorLeafLayer(
            ctx.plan->operations()[chain->source], *ctx.plan, leafResolved, chain->matrix, *window,
            ctx.fullDisplayWindow, ctx.fullPixelAspect, ctx.hScale, ctx.vScale, chain->opacity,
            false, ctx.allowance, operation, ctx.coverageCache, cancellation, emit, charge,
            chargeCoverage, command, key, consumed.text, consumed.shape, true))
        return error;
    result.consumedFieldLeaves.push_back(consumed);
    return std::nullopt;
}

// Prepares the one fixed program `leg` over a `width` x `height` dispatch.
[[nodiscard]] inline std::optional<GpuSceneLeafFailure>
prepareFieldLeg(const GpuOcioTransformSpec& leg, const render::ImageWindow window,
                const GpuSceneEffectEmissionContext& ctx, const GpuSceneOcioContext& ocioContext,
                const CancellationToken& cancellation,
                std::shared_ptr<const PreparedGpuOcioCommand>& program) {
    if (ocioContext.preparer == nullptr)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "No GPU program preparer is configured for fields");
    const auto config = resolveInputColorConfig(ctx.request->colorIntent);
    if (!config)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "The colour configuration for the field program is unavailable");
    const GpuOcioCommandGeometry geometry{
        .width = static_cast<std::uint32_t>(window.extent().width()),
        .height = static_cast<std::uint32_t>(window.extent().height())};
    auto prepared =
        ocioContext.preparer->prepare(*config, leg, geometry, ocioContext.compileOptions,
                                      [&cancellation] { return cancellation.isCancellationRequested(); });
    if (!prepared) {
        if (prepared.error == GpuOcioPreparationError::CompileCancelled ||
            cancellation.isCancellationRequested())
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                        "GPU field preparation was cancelled");
        if (prepared.error == GpuOcioPreparationError::CompileUnavailable)
            return fail(PreparedGpuSceneDiagnosticCode::AwaitingCapacity,
                        "GPU field program compile is unavailable; the frame waits");
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "GPU field preparation failed: " +
                        std::string{gpuOcioPreparationErrorName(prepared.error)});
    }
    program = std::move(prepared.command);
    return std::nullopt;
}

// One fixed-program dispatch over resident `input` (window `window`).
template <typename Emit, typename Charge>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitFieldDispatch(std::shared_ptr<const PreparedGpuOcioCommand> program,
                  const GpuSceneCommandIndex input, const std::string& inputKey,
                  const render::ImageWindow window, const render::ImageWindow display,
                  const core::PixelAspectRatio aspect, const OperationIndex operation, Emit&& emit,
                  Charge&& charge, GpuSceneCommandIndex& command, std::string& key) {
    if (const auto error =
            charge(window.extent().width(), window.extent().height(), sizeof(render::Rgba32f)))
        return error;
    GpuSceneOcioEffectCommand dispatch{.index = kInvalidGpuSceneCommand,
                                       .sourceOperation = operation,
                                       .input = input,
                                       .inputKey = inputKey,
                                       .program = std::move(program),
                                       .outputWindow = window,
                                       .displayWindow = display,
                                       .pixelAspect = aspect,
                                       .semanticKey = {}};
    dispatch.semanticKey = makeGpuSceneOcioEffectSemanticKey(
        inputKey, dispatch.program->identity(), dispatch.outputWindow, dispatch.pixelAspect);
    key = dispatch.semanticKey;
    command = emit(std::move(dispatch));
    return std::nullopt;
}

// An exact integer move of `input` from `source` to `output` (a pad, a crop or a relocation).
template <typename Emit, typename Charge>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitFieldMove(const GpuSceneCommandIndex input, const std::string& inputKey,
              const render::ImageWindow source, const render::ImageWindow output,
              const std::int64_t shiftX, const std::int64_t shiftY, const OperationIndex operation,
              Emit&& emit, Charge&& charge, GpuSceneCommandIndex& command, std::string& key) {
    if (const auto error =
            charge(output.extent().width(), output.extent().height(), sizeof(render::Rgba32f)))
        return error;
    OperationKey moveKey;
    moveKey.add(std::string{"gpu-field-move-v1"});
    moveKey.add(inputKey);
    addWindowToKey(moveKey, source);
    addWindowToKey(moveKey, output);
    moveKey.add(shiftX);
    moveKey.add(shiftY);
    moveKey.add(std::string{kGpuTranslationOpacitySpirvSha256});
    key = moveKey.digest();
    command = emit(GpuSceneTranslationCommand{
        .index = kInvalidGpuSceneCommand,
        .sourceOperation = operation,
        .input = input,
        .sourceWindow = source,
        .outputWindow = output,
        .translationX = static_cast<double>(source.originX() + shiftX - output.originX()),
        .translationY = static_cast<double>(source.originY() + shiftY - output.originY()),
        .opacity = 1.0F,
        .semanticKey = key});
    return std::nullopt;
}

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitFieldRasterCommand(const CompiledImageEffect& effect, const PreparedColorGrade& grade,
                       const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                       const GpuSceneOcioContext& ocioContext,
                       const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
                       ChargeCoverage&& chargeCoverage, GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Field preparation was cancelled");
    const auto& outputWindowOf = *ctx.outputWindowOf;
    const auto inputValue = effect.input.value();
    // A canvas with no image publishes none, as on the CPU reference.
    if (!outputWindowOf[inputValue]) {
        result.identity = true;
        return std::nullopt;
    }
    const auto canvas = *outputWindowOf[inputValue];
    auto display = ctx.fullDisplayWindow;
    auto aspect = ctx.fullPixelAspect;
    if (const auto command = (*ctx.commandForOperation)[inputValue];
        command != kInvalidGpuSceneCommand)
        if (const auto descriptor = gpuSceneCommandDescriptorOf(*ctx.commands, command, 0)) {
            display = descriptor->display;
            aspect = descriptor->pixelAspect;
        }
    FieldRasterPlacement placement;
    placement.originX = canvas.originX();
    placement.originY = canvas.originY();
    placement.width = static_cast<std::int64_t>(canvas.extent().width());
    placement.height = static_cast<std::int64_t>(canvas.extent().height());
    placement.compositionWidth = static_cast<double>(ctx.plan->format().width());
    placement.compositionHeight = static_cast<double>(ctx.plan->format().height());
    placement.horizontalScale = ctx.hScale;
    placement.verticalScale = ctx.vScale;
    // The slots in a row to the right of the target, or a column below it, whichever is smaller.
    struct Slot final {
        GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
        std::string key;
        render::ImageWindow window;
    };
    std::vector<std::pair<std::size_t, Slot>> slots;
    std::int64_t rowWidth = placement.width;
    std::int64_t rowHeight = placement.height;
    std::int64_t columnWidth = placement.width;
    std::int64_t columnHeight = placement.height;
    for (std::size_t i = 0; i < effect.fieldImages.size() && i < placement.slots.size(); ++i) {
        const auto value = effect.fieldImages[i].value();
        if (value >= outputWindowOf.size())
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "A field image is invalid");
        // An image with no pixels is an absent slot: every read is transparent.
        if (!outputWindowOf[value])
            continue;
        Slot slot{kInvalidGpuSceneCommand, {}, *outputWindowOf[value]};
        if (const auto error = residentFieldInput(value, operation, ctx, cancellation, emit, charge,
                                                  chargeCoverage, slot.command, slot.key, result))
            return error;
        const auto w = static_cast<std::int64_t>(slot.window.extent().width());
        const auto h = static_cast<std::int64_t>(slot.window.extent().height());
        rowWidth += w;
        rowHeight = std::max(rowHeight, h);
        columnWidth = std::max(columnWidth, w);
        columnHeight += h;
        slots.emplace_back(i, std::move(slot));
    }
    const bool row = std::max(rowWidth, rowHeight) <= std::max(columnWidth, columnHeight);
    auto atlas = canvas;
    if (!slots.empty()) {
        const auto created = render::ImageWindow::create(
            canvas.originX(), canvas.originY(),
            static_cast<std::uint64_t>(row ? rowWidth : columnWidth),
            static_cast<std::uint64_t>(row ? rowHeight : columnHeight));
        if (!created)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "The field atlas window overflows");
        atlas = *created.value();
    }
    // The dispatched image: the slots moved into the atlas over a transparent base.
    OperationKey atlasKey;
    atlasKey.add(std::string{"gpu-field-atlas-v1"});
    addWindowToKey(atlasKey, atlas);
    addWindowToKey(atlasKey, display);
    addPixelAspectToKey(atlasKey, aspect);
    std::vector<GpuSceneCommandIndex> foregrounds;
    std::int64_t cursor = row ? placement.width : placement.height;
    for (auto& [index, slot] : slots) {
        const auto atlasX = row ? cursor : 0;
        const auto atlasY = row ? 0 : cursor;
        cursor += row ? static_cast<std::int64_t>(slot.window.extent().width())
                      : static_cast<std::int64_t>(slot.window.extent().height());
        const auto placed = render::ImageWindow::create(
            atlas.originX() + atlasX, atlas.originY() + atlasY, slot.window.extent().width(),
            slot.window.extent().height());
        if (!placed)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "A field image does not fit the atlas");
        GpuSceneCommandIndex moved = kInvalidGpuSceneCommand;
        std::string movedKey;
        if (const auto error = emitFieldMove(
                slot.command, slot.key, slot.window, *placed.value(),
                placed.value()->originX() - slot.window.originX(),
                placed.value()->originY() - slot.window.originY(), operation, emit, charge, moved,
                movedKey))
            return error;
        foregrounds.push_back(moved);
        atlasKey.add(movedKey);
        auto& target = placement.slots[index];
        target.originX = slot.window.originX();
        target.originY = slot.window.originY();
        target.width = static_cast<std::int64_t>(slot.window.extent().width());
        target.height = static_cast<std::int64_t>(slot.window.extent().height());
        target.atlasX = atlasX;
        target.atlasY = atlasY;
    }
    atlasKey.add(std::string{kGpuSourceOverSpirvSha256});
    if (const auto error =
            charge(atlas.extent().width(), atlas.extent().height(), sizeof(render::Rgba32f)))
        return error;
    const auto atlasSemanticKey = atlasKey.digest();
    const auto base = emit(GpuSceneMergeCommand{.sourceOperation = operation,
                                                .foregrounds = std::move(foregrounds),
                                                .outputWindow = atlas,
                                                .displayWindow = display,
                                                .pixelAspect = aspect,
                                                .semanticKey = atlasSemanticKey});
    auto data = fieldRasterProgramData(grade, placement);
    if (!data)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Field raster geometry cannot be represented");
    GpuOcioTransformSpec leg;
    leg.kind = GpuOcioTransformKind::ColorGrade;
    leg.gradeKind = static_cast<std::uint8_t>(ColorGradeKind::FieldRaster);
    leg.gradeData = std::move(*data);
    std::shared_ptr<const PreparedGpuOcioCommand> program;
    if (const auto error = prepareFieldLeg(leg, atlas, ctx, ocioContext, cancellation, program))
        return error;
    GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
    std::string key;
    if (const auto error = emitFieldDispatch(std::move(program), base, atlasSemanticKey, atlas,
                                             display, aspect, operation, emit, charge, command,
                                             key))
        return error;
    // The target cut back out of the atlas.
    if (atlas != canvas)
        if (const auto error =
                emitFieldMove(command, std::string(key), atlas, canvas, 0, 0, operation, emit,
                              charge, command, key))
            return error;
    result.command = command;
    result.key = std::move(key);
    result.window = canvas;
    result.local = boundsForWindow(canvas, ctx.hScale, ctx.vScale);
    result.output = result.local;
    return std::nullopt;
}

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitMaskDistanceCommand(const CompiledImageEffect& effect, const MaskDistanceKernel& kernel,
                        const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                        const GpuSceneOcioContext& ocioContext,
                        const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
                        ChargeCoverage&& chargeCoverage, GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                    "Mask distance preparation was cancelled");
    if (!std::isfinite(kernel.reach) || kernel.reach < 0.0)
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Mask distance reach is out of domain");
    const auto inputValue = effect.input.value();
    const auto inputWindow = (*ctx.outputWindowOf)[inputValue];
    // A mask with no pixels has no distance image, as on the CPU reference.
    if (!inputWindow) {
        result.identity = true;
        return std::nullopt;
    }
    GpuSceneCommandIndex input = kInvalidGpuSceneCommand;
    std::string inputKey;
    if (const auto error = residentFieldInput(inputValue, operation, ctx, cancellation, emit,
                                              charge, chargeCoverage, input, inputKey, result))
        return error;
    const auto descriptor = gpuSceneCommandDescriptorOf(*ctx.commands, input, 0);
    if (!descriptor)
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "The mask descriptor is not derivable");
    const auto reach = maskDistanceReach(kernel, ctx.hScale, ctx.vScale);
    const auto window = maskDistanceWindow(descriptor->data, reach);
    if (!window)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "The mask distance window overflows");
    GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
    std::string key;
    if (const auto error = emitFieldMove(input, inputKey, descriptor->data, *window, 0, 0,
                                         operation, emit, charge, command, key))
        return error;
    for (int pass = 0; pass < 2; ++pass) {
        GpuOcioTransformSpec leg;
        leg.kind = GpuOcioTransformKind::MaskDistance;
        leg.maskDistanceReach = reach;
        leg.maskDistancePass = pass;
        std::shared_ptr<const PreparedGpuOcioCommand> program;
        if (const auto error =
                prepareFieldLeg(leg, *window, ctx, ocioContext, cancellation, program))
            return error;
        const auto inputCommand = command;
        const auto passInputKey = key;
        if (const auto error = emitFieldDispatch(std::move(program), inputCommand, passInputKey,
                                                 *window, descriptor->display,
                                                 descriptor->pixelAspect, operation, emit, charge,
                                                 command, key))
            return error;
    }
    result.command = command;
    result.key = std::move(key);
    result.window = *window;
    result.local = boundsForWindow(*window, ctx.hScale, ctx.vScale);
    result.output = result.local;
    return std::nullopt;
}
