// Included inside bloom::runtime::detail after the field emission helpers.
//
// #566 Auto Exposure on the GPU. A pass is the one fixed program (ColorGradeKind::AutoExposure)
// dispatched over an image. A Reduce reads one image and publishes a smaller window cut out of the
// dispatch at its origin: the tile sums of a frame, then the frame's mean. A Filter follows its
// newest mean and reads the window's older means beside it, an atlas row of one pixel each, and
// publishes one pixel. An Apply follows the frame and reads the Filter's pixel in one more column of
// its atlas. The atlas is built as Z Defocus's and Codec Degrade's is: the followed image padded to
// it (one exact move) with each other image, moved exactly into place, blended over it in Normal mode
// (the regions are disjoint, so each pixel is one image's exact value). The table and geometry are
// the CPU reference's (autoExposureProgramData), so both evaluate one kernel over identical data.

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitAutoExposureCommand(const CompiledImageEffect& effect, const PreparedColorGrade& grade,
                        const OperationIndex operation, const GpuSceneEffectEmissionContext& ctx,
                        const GpuSceneOcioContext& ocioContext,
                        const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
                        ChargeCoverage&& chargeCoverage, GpuSceneEffectEmission& result) {
    if (cancellation.isCancellationRequested())
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                    "Auto Exposure preparation was cancelled");
    const auto& outputWindowOf = *ctx.outputWindowOf;
    const auto inputValue = effect.input.value();
    const auto stage = autoExposureStage(grade);
    for (const auto image : effect.fieldImages)
        if (image.value() >= outputWindowOf.size())
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "The Auto Exposure images are invalid");
    // A pass with no image to follow publishes none, as on the CPU reference.
    if (!outputWindowOf[inputValue]) {
        result.identity = true;
        return std::nullopt;
    }
    // An application with no gain pixel leaves its input as it is.
    if (stage == AutoExposureStage::Apply &&
        (effect.fieldImages.size() != 1 || !outputWindowOf[effect.fieldImages[0].value()])) {
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
                    "The Auto Exposure descriptor is not derivable");
    const auto source = descriptor->data;
    const auto display = descriptor->display;
    const auto aspect = descriptor->pixelAspect;
    const auto sourceWidth = static_cast<std::int64_t>(source.extent().width());
    const auto sourceHeight = static_cast<std::int64_t>(source.extent().height());
    const auto size = autoExposureOutputSize(grade, sourceWidth, sourceHeight);
    const auto created = render::ImageWindow::create(
        source.originX(), source.originY(), static_cast<std::uint64_t>(size[0]),
        static_cast<std::uint64_t>(size[1]));
    if (!created)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "The Auto Exposure output window overflows");
    const render::ImageWindow output = *created.value();

    AutoExposurePlacement placement;
    placement.sourceWidth = sourceWidth;
    placement.sourceHeight = sourceHeight;
    GpuSceneCommandIndex base = colour;
    std::string baseKey = colourKey;
    render::ImageWindow dispatchWindow = source;

    // The images beside the followed one, moved to their cells of the atlas.
    struct Cell final {
        GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
        std::string key;
        render::ImageWindow window;
        std::int64_t atlasX = 0;
    };
    std::vector<Cell> cells;
    if (stage == AutoExposureStage::Filter) {
        if (sourceWidth != 1 || sourceHeight != 1)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "An Auto Exposure mean is not one pixel");
        const auto count = static_cast<std::int64_t>(effect.fieldImages.size()) + 1;
        placement.cells.assign(static_cast<std::size_t>(count), {-1, -1});
        placement.cells.back() = {0, 0};
        for (std::size_t i = 0; i < effect.fieldImages.size(); ++i) {
            const auto value = effect.fieldImages[i].value();
            if (!outputWindowOf[value])
                continue;
            Cell cell{kInvalidGpuSceneCommand, {}, *outputWindowOf[value],
                      static_cast<std::int64_t>(i) + 1};
            if (cell.window.extent().width() != 1 || cell.window.extent().height() != 1)
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                            "An Auto Exposure mean is not one pixel");
            if (const auto error = residentFieldInput(value, operation, ctx, cancellation, emit,
                                                      charge, chargeCoverage, cell.command,
                                                      cell.key, result))
                return error;
            placement.cells[i] = {cell.atlasX, 0};
            cells.push_back(std::move(cell));
        }
        const auto atlas = render::ImageWindow::create(source.originX(), source.originY(),
                                                       static_cast<std::uint64_t>(count), 1);
        if (!atlas)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "The Auto Exposure atlas window overflows");
        dispatchWindow = *atlas.value();
    } else if (stage == AutoExposureStage::Apply) {
        const auto value = effect.fieldImages[0].value();
        Cell cell{kInvalidGpuSceneCommand, {}, *outputWindowOf[value], sourceWidth};
        if (cell.window.extent().width() != 1 || cell.window.extent().height() != 1)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "An Auto Exposure gain is not one pixel");
        if (const auto error = residentFieldInput(value, operation, ctx, cancellation, emit, charge,
                                                  chargeCoverage, cell.command, cell.key, result))
            return error;
        placement.hasState = true;
        placement.stateX = cell.atlasX;
        placement.stateY = 0;
        const auto atlas = render::ImageWindow::create(
            source.originX(), source.originY(), static_cast<std::uint64_t>(sourceWidth + 1),
            static_cast<std::uint64_t>(sourceHeight));
        if (!atlas)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "The Auto Exposure atlas window overflows");
        dispatchWindow = *atlas.value();
        cells.push_back(std::move(cell));
    }
    if (dispatchWindow != source) {
        if (const auto error = emitFieldMove(colour, colourKey, source, dispatchWindow, 0, 0,
                                             operation, emit, charge, base, baseKey))
            return error;
        for (auto& cell : cells) {
            const auto placed = render::ImageWindow::create(
                dispatchWindow.originX() + cell.atlasX, dispatchWindow.originY(), 1, 1);
            if (!placed)
                return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "An Auto Exposure image does not fit the atlas");
            GpuSceneCommandIndex moved = kInvalidGpuSceneCommand;
            std::string movedKey;
            if (const auto error = emitFieldMove(
                    cell.command, cell.key, cell.window, *placed.value(),
                    placed.value()->originX() - cell.window.originX(),
                    placed.value()->originY() - cell.window.originY(), operation, emit, charge,
                    moved, movedKey))
                return error;
            if (const auto error = charge(dispatchWindow.extent().width(),
                                          dispatchWindow.extent().height(),
                                          sizeof(render::Rgba32f)))
                return error;
            auto blendKey = makeGpuSceneBlendSemanticKey(
                movedKey, baseKey, core::BlendMode::Normal, *placed.value(), dispatchWindow,
                aspect, std::string{kGpuBlendSpirvSha256});
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
    }
    auto data = autoExposureProgramData(grade, placement);
    if (!data)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Auto Exposure geometry cannot be represented");
    GpuOcioTransformSpec leg;
    leg.kind = GpuOcioTransformKind::ColorGrade;
    leg.gradeKind = static_cast<std::uint8_t>(ColorGradeKind::AutoExposure);
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
