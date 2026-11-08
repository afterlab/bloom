// #587 Test Card through the compiler and the CPU evaluator: the lowering (canvas, optional
// crop/pad, one field raster, the color space transform), the size following the composition and a
// Custom size overriding it (portrait, wide and square), the exact values of a working-space Patch,
// the geometry of the Circle on the actual pixel size, a large size within budget, proxy, mute and
// cancellation. Uses the helpers of snapshot_compiler_field_tests.ipp.

// The document's color space ids are the ones the color configuration synthesizes.
static_assert(document::kTestCardRec709Space == color::kBloomRec709CameraColorSpaceId);
static_assert(document::kTestCardHlgSpace == color::kBloomRec2100HlgColorSpaceId);
static_assert(document::kTestCardPqSpace == color::kBloomRec2100PqColorSpaceId);

[[nodiscard]] document::Project testCardProject(const std::uint32_t width, const std::uint32_t height) {
    auto options = singleLayerOptions();
    options.format = requireValue(document::CompositionFormat::create(width, height), "card extent");
    auto project = makeProject(options);
    auto& parameters = project.findComposition(kCompositionId)->parameters();
    require(parameters.setSource(kFirstPosition,
                                 document::ConstantValueSource{document::Vec2d{
                                     static_cast<double>(width) / 2.0,
                                     static_cast<double>(height) / 2.0}}),
            "center the card layer");
    require(parameters.setSource(kFirstOpacity, document::ConstantValueSource{1.0}),
            "the card layer is opaque");
    return project;
}

constexpr auto kTestCardNode = document::NodeId::fromRaw(40);

[[nodiscard]] runtime::SnapshotCompileResult
compileTestCard(const std::uint32_t width, const std::uint32_t height,
                const std::map<std::string, document::ParameterValue>& values,
                const bool muted = false) {
    auto project = testCardProject(width, height);
    addFieldNode(project, kTestCardNode, document::kTestCardNodeType, values);
    feedLayer(project, kTestCardNode, document::kTestCardImagePort);
    if (muted)
        project.findComposition(kCompositionId)->nodeLayout()[kTestCardNode].muted = true;
    require(project.validate().ok(), "a Test Card is valid document truth");
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "test card built-ins");
    registry.freeze();
    return compile(std::move(project), registry);
}

[[nodiscard]] runtime::EvaluationResult
evaluateTestCard(const runtime::SnapshotCompileResult& compiled, const std::uint32_t proxyWidth = 0,
                 const std::uint32_t proxyHeight = 0, const std::size_t byteLimit = 1U << 26U,
                 const runtime::CancellationToken& cancellation = {}) {
    require(compiled.plan != nullptr, "the card compiles");
    runtime::EvaluationRequest request{
        core::RationalTime::fromInteger(0), compiled.plan->output(),
        runtime::CompositionFormatResolution{}};
    if (proxyWidth != 0) {
        const auto extent = render::ImageExtent::create(proxyWidth, proxyHeight);
        request.resolution = runtime::ProxyResolution{*extent.value()};
    }
    request.quality = runtime::EvaluationQuality::Reference;
    request.colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene;
    request.pixelStorageByteLimit = byteLimit;
    request.bypassOperationCache = true;
    return runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, request, cancellation);
}

[[nodiscard]] std::array<float, 4> cardPixel(const render::Rgba32fImage& image, const std::int64_t x,
                                             const std::int64_t y) {
    std::array<float, 4> value{};
    if (const auto pixel = image.read(x, y))
        std::ranges::copy(pixel.value()->components(), value.begin());
    return value;
}

void testTestCardLowering(Expectations& expectations) {
    using namespace document;
    // A Composition-sized card: canvas, raster, then the color space transform.
    {
        const auto compiled = compileTestCard(160, 90, {{"card", std::int64_t{1}}});
        expectations.expect(compiled.plan != nullptr, "a Test Card compiles");
        if (compiled.plan) {
            const auto& plan = *compiled.plan;
            const auto ops = recipeOperations(plan, kTestCardNode);
            expectations.expect(ops.size() == 2 &&
                                    recipeKernel<runtime::FieldRasterKernel>(plan, ops[0]) != nullptr &&
                                    recipeKernel<runtime::CstKernel>(plan, ops[1]) != nullptr,
                                "a Test Card is one field raster and one color space transform");
            const auto* cst = ops.size() == 2 ? recipeKernel<runtime::CstKernel>(plan, ops[1]) : nullptr;
            expectations.expect(cst != nullptr && cst->fromId == kTestCardRec709Space &&
                                    cst->toId.empty(),
                                "the signal is the Rec.709 camera encoding, into the working space");
            const auto* raster = ops.empty() ? nullptr
                                             : recipeKernel<runtime::FieldRasterKernel>(plan, ops[0]);
            expectations.expect(raster != nullptr &&
                                    std::ranges::any_of(
                                        raster->program.instructions,
                                        [](const auto& i) {
                                            return i.op == static_cast<std::uint8_t>(
                                                               core::field::FieldOp::TestCard);
                                        }),
                                "the raster runs the test card instruction");
        }
    }
    // A working-space Patch has no transform; an animated card is time dependent.
    {
        const auto compiled = compileTestCard(
            160, 90, {{"card", std::int64_t{4}}, {"patchSpace", std::int64_t{3}}});
        expectations.expect(compiled.plan != nullptr &&
                                recipeOperations(*compiled.plan, kTestCardNode).size() == 1,
                            "a working-space Patch needs no color space transform");
    }
    // A muted card draws nothing.
    {
        const auto compiled = compileTestCard(32, 18, {{"card", std::int64_t{8}}}, true);
        expectations.expect(compiled.plan != nullptr, "a muted Test Card compiles");
        const auto evaluated = evaluateTestCard(compiled);
        expectations.expect(evaluated.frame() != nullptr &&
                                std::ranges::all_of(evaluated.frame()->processImage().pixels(),
                                                    [](const auto& p) {
                                                        return p == render::Rgba32f::transparent();
                                                    }),
                            "a muted Test Card draws nothing");
    }
}

void testTestCardPixels(Expectations& expectations) {
    using namespace document;
    // A working-space gray patch is its exact signal at every pixel (opaque, premultiplied).
    {
        const auto compiled = compileTestCard(
            48, 27,
            {{"card", std::int64_t{4}}, {"patchSpace", std::int64_t{3}},
             {"patchValue", std::int64_t{7}}});
        const auto evaluated = evaluateTestCard(compiled);
        expectations.expect(evaluated.frame() != nullptr, "a Patch evaluates");
        if (evaluated.frame()) {
            const auto& image = evaluated.frame()->processImage();
            bool exact = true;
            for (std::int64_t y = 0; y < 27; ++y)
                for (std::int64_t x = 0; x < 48; ++x) {
                    const auto p = cardPixel(image, x, y);
                    exact = exact && std::abs(p[0] - 0.5F) < 1.0e-5F && p[0] == p[1] &&
                            p[1] == p[2] && p[3] == 1.0F;
                }
            expectations.expect(exact, "a working-space 50% Patch is 0.5 at every pixel");
        }
    }
    // The composition's size is the card's size: the Circle is symmetric on every axis, so it is
    // round and centerd, in a wide, a square and a portrait composition.
    for (const auto& [w, h] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
             {64, 36}, {40, 40}, {36, 64}}) {
        const auto compiled = compileTestCard(w, h, {{"card", std::int64_t{8}}, {"label", false}});
        const auto evaluated = evaluateTestCard(compiled);
        expectations.expect(evaluated.frame() != nullptr, "a Circle evaluates");
        if (!evaluated.frame())
            continue;
        const auto& image = evaluated.frame()->processImage();
        expectations.expect(image.descriptor()->dataWindow().extent().width() == w &&
                                image.descriptor()->dataWindow().extent().height() == h,
                            "the card is the composition's size");
        // The sub-samples are not a mirror image of one another, so symmetry is of the picture's
        // weight: the centroid of its brightness is the frame's center, and it is drawn.
        double total = 0.0;
        double sumX = 0.0;
        double sumY = 0.0;
        bool drawn = false;
        for (std::int64_t y = 0; y < h; ++y)
            for (std::int64_t x = 0; x < w; ++x) {
                const auto p = cardPixel(image, x, y);
                total += static_cast<double>(p[0]);
                sumX += static_cast<double>(p[0]) * (static_cast<double>(x) + 0.5);
                sumY += static_cast<double>(p[0]) * (static_cast<double>(y) + 0.5);
                drawn = drawn || p != cardPixel(image, 0, 0);
            }
        const bool symmetric = total > 0.0 &&
                               std::abs(sumX / total - static_cast<double>(w) / 2.0) < 0.1 &&
                               std::abs(sumY / total - static_cast<double>(h) / 2.0) < 0.1;
        expectations.expect(symmetric && drawn, "the Circle is drawn and centerd on both axes");
    }
    // A Custom size overrides the composition's: outside the card nothing is drawn, inside is opaque.
    {
        const auto compiled = compileTestCard(
            64, 36,
            {{"card", std::int64_t{4}}, {"patchSpace", std::int64_t{3}},
             {"sizeMode", std::int64_t{1}}, {"width", std::int64_t{20}}, {"height", std::int64_t{10}}});
        const auto evaluated = evaluateTestCard(compiled);
        expectations.expect(evaluated.frame() != nullptr, "a Custom size evaluates");
        if (evaluated.frame()) {
            const auto& image = evaluated.frame()->processImage();
            const auto inside = cardPixel(image, 32, 18);
            const auto outside = cardPixel(image, 5, 5);
            const auto edgeIn = cardPixel(image, 22, 13);
            const auto edgeOut = cardPixel(image, 21, 13);
            expectations.expect(inside[3] == 1.0F && outside[3] == 0.0F && edgeIn[3] == 1.0F &&
                                    edgeOut[3] == 0.0F,
                                "a Custom size overrides the composition's, centerd, with no stretch");
        }
    }
    // A Custom size larger than the composition is cut to it and still centerd.
    {
        const auto compiled = compileTestCard(
            32, 18,
            {{"card", std::int64_t{4}}, {"patchSpace", std::int64_t{3}},
             {"sizeMode", std::int64_t{1}}, {"width", std::int64_t{200}}, {"height", std::int64_t{100}}});
        const auto evaluated = evaluateTestCard(compiled);
        expectations.expect(evaluated.frame() != nullptr &&
                                cardPixel(evaluated.frame()->processImage(), 0, 0)[3] == 1.0F &&
                                cardPixel(evaluated.frame()->processImage(), 31, 17)[3] == 1.0F,
                            "a Custom size beyond the composition fills it");
    }
    // A proxy keeps the geometry: the Circle at half size is still symmetric.
    {
        const auto compiled = compileTestCard(64, 36, {{"card", std::int64_t{8}}, {"label", false}});
        const auto evaluated = evaluateTestCard(compiled, 32, 18);
        expectations.expect(evaluated.frame() != nullptr, "a proxy Circle evaluates");
        if (evaluated.frame()) {
            const auto& image = evaluated.frame()->processImage();
            double total = 0.0;
            double sumX = 0.0;
            double sumY = 0.0;
            for (std::int64_t y = 0; y < 18; ++y)
                for (std::int64_t x = 0; x < 32; ++x) {
                    const auto p = static_cast<double>(cardPixel(image, x, y)[0]);
                    total += p;
                    sumX += p * (static_cast<double>(x) + 0.5);
                    sumY += p * (static_cast<double>(y) + 0.5);
                }
            const bool symmetric = total > 0.0 && std::abs(sumX / total - 16.0) < 0.1 &&
                                   std::abs(sumY / total - 9.0) < 0.1;
            expectations.expect(image.descriptor()->dataWindow().extent().width() == 32 && symmetric,
                                "a proxy keeps the card's proportions");
        }
    }
    // The pixel budget refuses what it cannot hold, explicitly; cancellation stops the evaluation.
    {
        const auto compiled = compileTestCard(64, 36, {{"card", std::int64_t{7}}});
        const auto refused = evaluateTestCard(compiled, 0, 0, 64);
        expectations.expect(refused.frame() == nullptr, "a card over the pixel budget is refused");
        runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
        const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                       .id = runtime::TaskOwnerId::fromRaw(5871)};
        bool cancelledOutcome = false;
        auto submission = scheduler.submit<void>(
            runtime::TaskRequest("Test Card cancellation", owner),
            [&](runtime::TaskContext& context) {
                static_cast<void>(scheduler.cancelOwner(owner));
                const auto cancelled =
                    evaluateTestCard(compiled, 0, 0, 1U << 26U, context.cancellation());
                cancelledOutcome = cancelled.frame() == nullptr;
                return runtime::TaskResult<void>::cancelled();
            });
        expectations.expect(submission.accepted(), "the cancellation task is admitted");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::optional<runtime::TaskResult<void>> taken;
        while (!taken && std::chrono::steady_clock::now() < deadline) {
            taken = submission.handle.tryTakeResult();
            std::this_thread::yield();
        }
        expectations.expect(taken && cancelledOutcome, "a cancelled card publishes no image");
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        expectations.expect(scheduler.isQuiescent(), "the cancelled card shuts down safely");
    }
    // A large card within budget lowers without a stored raster: only the size is data.
    {
        const auto compiled = compileTestCard(
            1920, 1080,
            {{"card", std::int64_t{0}}, {"sizeMode", std::int64_t{1}}, {"width", std::int64_t{16384}},
             {"height", std::int64_t{8192}}});
        expectations.expect(compiled.plan != nullptr, "a very large Custom card compiles");
    }
}

// The pixels of a Rec.709 Patch are its signal taken through the color space transform into the
// working space: the arithmetic the card's expected display value starts from.
void testTestCardPatchMatchesTheTransform(Expectations& expectations) {
    auto resolution = color::resolveBloomNeutralV1BuiltIn(
        color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
        color::kBloomNeutralV1ConfigDigest);
    require(resolution.ready(), "the built-in configuration resolves");
    const auto resolved = std::move(resolution).takeResolved();
    require(resolved.has_value(), "the built-in configuration is taken");
    const auto prepared = color::CpuColorSpaceProcessor::prepare(
        *resolved, document::kTestCardRec709Space, resolved->processColorSpaceId());
    require(static_cast<bool>(prepared), "the Rec.709 transform prepares");
    for (const std::int64_t value : {std::int64_t{3}, std::int64_t{7}, std::int64_t{15},
                                     std::int64_t{20}, std::int64_t{26}}) {
        const auto compiled = compileTestCard(
            16, 9, {{"card", std::int64_t{4}}, {"patchValue", value}});
        const auto evaluated = evaluateTestCard(compiled);
        expectations.expect(evaluated.frame() != nullptr, "a Rec.709 Patch evaluates");
        if (!evaluated.frame())
            continue;
        const auto signal = document::testCardPatchSignal(value);
        std::array<std::array<float, 4>, 1> expected{
            {{static_cast<float>(signal[0]), static_cast<float>(signal[1]),
              static_cast<float>(signal[2]), 1.0F}}};
        require(prepared.processor()->apply(expected), "the transform applies");
        const auto p = cardPixel(evaluated.frame()->processImage(), 8, 4);
        expectations.expect(std::abs(p[0] - expected[0][0]) < 1.0e-4F &&
                                std::abs(p[1] - expected[0][1]) < 1.0e-4F &&
                                std::abs(p[2] - expected[0][2]) < 1.0e-4F && p[3] == 1.0F,
                            "a Rec.709 Patch's pixels are its signal through the transform");
    }
}

// A level below black survives the color space transform: the SD Bars' PLUGE step at -4% is
// negative in the working space, black is zero and the +4% step is above it.
void testTestCardKeepsLevelsBelowBlack(Expectations& expectations) {
    const auto compiled = compileTestCard(210, 120, {{"card", std::int64_t{0}}});
    const auto evaluated = evaluateTestCard(compiled);
    expectations.expect(evaluated.frame() != nullptr, "SD Bars evaluate");
    if (!evaluated.frame())
        return;
    const auto& image = evaluated.frame()->processImage();
    // The PLUGE group spans 5/7 to 6/7 of the width in three equal steps, in the bottom quarter.
    const auto below = cardPixel(image, 153, 110);
    const auto at = cardPixel(image, 160, 110);
    const auto above = cardPixel(image, 173, 110);
    expectations.expect(below[0] < 0.0F && std::abs(at[0]) < 1.0e-5F && above[0] > 0.0F &&
                            below[3] == 1.0F,
                        "the PLUGE steps below, at and above black are kept through the transform");
}

// Standard bars keep their 16:9 layout in any frame: in a portrait frame they are the largest centerd
// 16:9 area, black outside it and never stretched.
void testTestCardBarsArePaddedNotStretched(Expectations& expectations) {
    for (const std::int64_t card : {std::int64_t{0}, std::int64_t{1}, std::int64_t{2}}) {
        const auto compiled = compileTestCard(36, 64, {{"card", card}});
        const auto evaluated = evaluateTestCard(compiled);
        expectations.expect(evaluated.frame() != nullptr, "portrait bars evaluate");
        if (!evaluated.frame())
            continue;
        const auto& image = evaluated.frame()->processImage();
        // The 16:9 area is 36 by 20.25 pixels, from row 21.9 to 42.1.
        const auto top = cardPixel(image, 5, 5);
        const auto bottom = cardPixel(image, 5, 58);
        const auto bars = cardPixel(image, 5, 28);
        expectations.expect(top[3] == 1.0F && top[0] == 0.0F && top[1] == 0.0F && top[2] == 0.0F &&
                                bottom[3] == 1.0F && bottom[0] == 0.0F && bottom[1] == 0.0F &&
                                bottom[2] == 0.0F && (bars[0] > 0.0F || bars[1] > 0.0F || bars[2] > 0.0F),
                            "portrait bars are padded in black above and below, not stretched");
    }
}
