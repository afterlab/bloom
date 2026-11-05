// Included inside bloom::runtime::detail after the field emission helpers.
//
// #483 Depth effects on the GPU. A depth map is its one fixed program (ColorGradeKind::DepthMap)
// over the depth image, with the same geometry the CPU reference appends (depthMapProgramData).
// Z Defocus's gather is its one fixed program (ColorGradeKind::Defocus) over an atlas, as a field
// raster's: the colour input at its own position inside the output window, and beside that window
// the tile maximum (carrying the depth map in red), the prefiltered colour and the bokeh image. The
// atlas is the colour padded to it (one exact move) with each slot, moved exactly into place,
// blended over it in Normal mode (the regions are disjoint, so each pixel is one image's exact
// value); every full-atlas pass costs the whole atlas, so it is built with one pass a slot and the
// prefiltered colour is left out when its mix is zero everywhere. The program gathers inside the
// output window and copies the rest; the output window is then cut back out. The table and
// geometry are the CPU reference's (defocusProgramData), so both evaluate one kernel over identical
// data.

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitDepthMapCommand(const CompiledImageEffect& effect, const PreparedColorGrade& grade,
                    const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                    const GpuSceneOcioContext& ocioContext, const CancellationToken& cancellation,
                    Emit&& emit, Charge&& charge, ChargeCoverage&& chargeCoverage,
                    GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Depth map preparation was cancelled");
    const auto inputValue = effect.input.value();
    // A depth input with no pixels has no map, as on the CPU reference.
    if (!(*ctx.outputWindowOf)[inputValue]) {
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
                    "The depth descriptor is not derivable");
    auto data = depthMapProgramData(grade, descriptor->data, descriptor->display, ctx.hScale);
    if (!data)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Depth map geometry cannot be represented");
    GpuOcioTransformSpec leg;
    leg.kind = GpuOcioTransformKind::ColorGrade;
    leg.gradeKind = static_cast<std::uint8_t>(ColorGradeKind::DepthMap);
    leg.gradeData = std::move(*data);
    std::shared_ptr<const PreparedGpuOcioCommand> program;
    if (const auto error =
            prepareFieldLeg(leg, descriptor->data, ctx, ocioContext, cancellation, program))
        return error;
    GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
    std::string key;
    if (const auto error = emitFieldDispatch(std::move(program), input, inputKey, descriptor->data,
                                             descriptor->display, descriptor->pixelAspect,
                                             operation, emit, charge, command, key))
        return error;
    result.command = command;
    result.key = std::move(key);
    result.window = descriptor->data;
    result.local = boundsForWindow(descriptor->data, ctx.hScale, ctx.vScale);
    result.output = result.local;
    return std::nullopt;
}

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitDefocusCommand(const CompiledImageEffect& effect, const DefocusKernel& kernel,
                   const PreparedColorGrade& grade, const OperationIndex operation,
                   const GpuSceneEffectEmissionContext& ctx, const GpuSceneOcioContext& ocioContext,
                   const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
                   ChargeCoverage&& chargeCoverage, GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Defocus preparation was cancelled");
    const auto& outputWindowOf = *ctx.outputWindowOf;
    const auto inputValue = effect.input.value();
    // A colour input with no pixels has nothing to blur, as on the CPU reference.
    if (!outputWindowOf[inputValue]) {
        result.identity = true;
        return std::nullopt;
    }
    GpuSceneCommandIndex color = kInvalidGpuSceneCommand;
    std::string colorKey;
    if (const auto error = residentFieldInput(inputValue, operation, ctx, cancellation, emit,
                                              charge, chargeCoverage, color, colorKey, result))
        return error;
    const auto descriptor = gpuSceneCommandDescriptorOf(*ctx.commands, color, 0);
    if (!descriptor)
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "The defocus input descriptor is not derivable");
    const auto colorWindow = descriptor->data;
    const auto grow = defocusReach(grade, ctx.hScale);
    const auto outputCreated = render::ImageWindow::create(
        colorWindow.originX() - grow, colorWindow.originY() - grow,
        colorWindow.extent().width() + 2 * static_cast<std::uint64_t>(grow),
        colorWindow.extent().height() + 2 * static_cast<std::uint64_t>(grow));
    if (!outputCreated)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "The defocus window overflows");
    const auto output = *outputCreated.value();
    DefocusPlacement placement;
    placement.color = {colorWindow.originX(),
                       colorWindow.originY(),
                       static_cast<std::int64_t>(colorWindow.extent().width()),
                       static_cast<std::int64_t>(colorWindow.extent().height()),
                       colorWindow.originX() - output.originX(),
                       colorWindow.originY() - output.originY()};
    // The slots in a row to the right of the output window, or a column below it, whichever is
    // smaller, as a field raster's.
    struct Slot final {
        std::size_t index = 0;
        GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
        std::string key;
        render::ImageWindow window;
    };
    std::vector<Slot> slots;
    const auto outW = static_cast<std::int64_t>(output.extent().width());
    const auto outH = static_cast<std::int64_t>(output.extent().height());
    std::int64_t rowWidth = outW, rowHeight = outH, columnWidth = outW, columnHeight = outH;
    const bool prefilter = defocusPrefilterUsed(grade, ctx.hScale);
    for (std::size_t s = 0; s < kDefocusSlots; ++s) {
        if (kernel.slotImages[s] < 0 || (s == kDefocusPrefilterSlot && !prefilter))
            continue;
        const auto index = static_cast<std::size_t>(kernel.slotImages[s]);
        if (index >= effect.fieldImages.size())
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "A defocus slot is invalid");
        const auto value = effect.fieldImages[index].value();
        if (value >= outputWindowOf.size())
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "A defocus image is invalid");
        // An image with no pixels is an absent slot, as on the CPU reference.
        if (!outputWindowOf[value])
            continue;
        Slot slot{s, kInvalidGpuSceneCommand, {}, *outputWindowOf[value]};
        if (const auto error = residentFieldInput(value, operation, ctx, cancellation, emit, charge,
                                                  chargeCoverage, slot.command, slot.key, result))
            return error;
        const auto w = static_cast<std::int64_t>(slot.window.extent().width());
        const auto h = static_cast<std::int64_t>(slot.window.extent().height());
        rowWidth += w;
        rowHeight = std::max(rowHeight, h);
        columnWidth = std::max(columnWidth, w);
        columnHeight += h;
        slots.push_back(std::move(slot));
    }
    const bool row = std::max(rowWidth, rowHeight) <= std::max(columnWidth, columnHeight);
    const auto atlasCreated = render::ImageWindow::create(
        output.originX(), output.originY(),
        static_cast<std::uint64_t>(row ? rowWidth : columnWidth),
        static_cast<std::uint64_t>(row ? rowHeight : columnHeight));
    if (!atlasCreated)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "The defocus atlas window overflows");
    const auto atlas = *atlasCreated.value();
    const auto display = descriptor->display;
    const auto aspect = descriptor->pixelAspect;
    // The colour padded to the atlas, then each slot moved into place and blended over it.
    GpuSceneCommandIndex base = kInvalidGpuSceneCommand;
    std::string atlasSemanticKey;
    if (const auto error = emitFieldMove(color, colorKey, colorWindow, atlas, 0, 0, operation, emit,
                                         charge, base, atlasSemanticKey))
        return error;
    std::int64_t cursor = row ? outW : outH;
    for (auto& slot : slots) {
        const auto atlasX = row ? cursor : 0;
        const auto atlasY = row ? 0 : cursor;
        cursor += row ? static_cast<std::int64_t>(slot.window.extent().width())
                      : static_cast<std::int64_t>(slot.window.extent().height());
        const auto placed =
            render::ImageWindow::create(atlas.originX() + atlasX, atlas.originY() + atlasY,
                                        slot.window.extent().width(), slot.window.extent().height());
        if (!placed)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "A defocus image does not fit the atlas");
        GpuSceneCommandIndex moved = kInvalidGpuSceneCommand;
        std::string movedKey;
        if (const auto error = emitFieldMove(slot.command, slot.key, slot.window, *placed.value(),
                                             placed.value()->originX() - slot.window.originX(),
                                             placed.value()->originY() - slot.window.originY(),
                                             operation, emit, charge, moved, movedKey))
            return error;
        if (const auto error =
                charge(atlas.extent().width(), atlas.extent().height(), sizeof(render::Rgba32f)))
            return error;
        auto blendKey = makeGpuSceneBlendSemanticKey(movedKey, atlasSemanticKey,
                                                     core::BlendMode::Normal, *placed.value(),
                                                     atlas, aspect, std::string{kGpuBlendSpirvSha256});
        base = emit(GpuSceneBlendCommand{.sourceOperation = operation,
                                         .source = moved,
                                         .destination = base,
                                         .sourceKey = movedKey,
                                         .destinationKey = atlasSemanticKey,
                                         .mode = core::BlendMode::Normal,
                                         .imageCombine = {},
                                         .sourceWindow = *placed.value(),
                                         .outputWindow = atlas,
                                         .pixelAspect = aspect,
                                         .artifactDigest = std::string{kGpuBlendSpirvSha256},
                                         .semanticKey = blendKey});
        atlasSemanticKey = std::move(blendKey);
        placement.slots[slot.index] = {slot.window.originX(),
                                       slot.window.originY(),
                                       static_cast<std::int64_t>(slot.window.extent().width()),
                                       static_cast<std::int64_t>(slot.window.extent().height()),
                                       atlasX,
                                       atlasY};
    }
    auto data = defocusProgramData(grade, placement, ctx.hScale);
    if (!data)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Defocus geometry cannot be represented");
    GpuOcioTransformSpec leg;
    leg.kind = GpuOcioTransformKind::ColorGrade;
    leg.gradeKind = static_cast<std::uint8_t>(ColorGradeKind::Defocus);
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
    if (atlas != output)
        if (const auto error = emitFieldMove(command, std::string(key), atlas, output, 0, 0,
                                             operation, emit, charge, command, key))
            return error;
    result.command = command;
    result.key = std::move(key);
    result.window = output;
    result.local = boundsForWindow(output, ctx.hScale, ctx.vScale);
    result.output = result.local;
    return std::nullopt;
}
