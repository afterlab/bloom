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

// ---- Per-card pixels through the production compile and the CPU reference (#639) -------------
//
// Every card a Test Card draws is held to the values docs/user-guide/test-card.md and the standards
// it cites give, at exact pixels of the composition-sized card, through the compiled node (a field
// raster and the Rec.709 transform into the working space) -- not the kernel alone. The expected
// signal is written here from the documented fractions and levels; the working-space value it must
// become is the colour library's own Rec.709 transform of that signal (the arithmetic
// testTestCardPatchMatchesTheTransform uses), never a number taken from running the card. Where the
// guide states a card's look but not a level (the Grid's ground, the guide outlines' colours), the
// level is the definition Bloom's kernel test (field_program_tests.cpp) states for it.
//
// Cards whose kernel-level pixel assertions already stand in field_program_tests.cpp's testTestCard
// (SD, HD, EBU and HDR Bars, Patch, Circle, Zone Plate, Siemens Star, Countdown) are not repeated
// through the node here, except Patch, Circle and the bars' framing that testTestCardPixels and its
// neighbours above already cover.

namespace test_card_proof {

using Rgba = std::array<float, 4>;

[[nodiscard]] runtime::EvaluationResult
evaluateAt(const runtime::SnapshotCompileResult& compiled, const core::RationalTime time,
           const std::uint32_t proxyWidth = 0, const std::uint32_t proxyHeight = 0) {
    require(compiled.plan != nullptr, "the card compiles");
    runtime::EvaluationRequest request{time, compiled.plan->output(),
                                       runtime::CompositionFormatResolution{}};
    if (proxyWidth != 0) {
        const auto extent = render::ImageExtent::create(proxyWidth, proxyHeight);
        request.resolution = runtime::ProxyResolution{*extent.value()};
    }
    request.quality = runtime::EvaluationQuality::Reference;
    request.colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene;
    request.pixelStorageByteLimit = 1U << 26U;
    request.bypassOperationCache = true;
    return runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, request, {});
}

} // namespace test_card_proof

void testTestCardEveryCardPixels(Expectations& expectations) {
    using namespace document;
    namespace proof = test_card_proof;
    auto resolution = color::resolveBloomNeutralV1BuiltIn(
        color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
        color::kBloomNeutralV1ConfigDigest);
    require(resolution.ready(), "the built-in configuration resolves");
    const auto resolved = std::move(resolution).takeResolved();
    require(resolved.has_value(), "the built-in configuration is taken");
    const auto prepared = color::CpuColorSpaceProcessor::prepare(
        *resolved, document::kTestCardRec709Space, resolved->processColorSpaceId());
    require(static_cast<bool>(prepared), "the Rec.709 transform prepares");
    // The working-space colour of a Rec.709 signal, opaque.
    const auto working = [&](const double r, const double g, const double b) {
        std::array<std::array<float, 4>, 1> value{
            {{static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), 1.0F}}};
        require(prepared.processor()->apply(value), "the transform applies");
        return proof::Rgba{value[0][0], value[0][1], value[0][2], 1.0F};
    };
    const auto close = [](const proof::Rgba& a, const proof::Rgba& b, const float tolerance = 1.0e-4F) {
        for (std::size_t i = 0; i < 4; ++i)
            if (std::abs(a[i] - b[i]) > tolerance)
                return false;
        return true;
    };
    const auto render = [&](const std::uint32_t w, const std::uint32_t h,
                            const std::map<std::string, ParameterValue>& values,
                            const core::RationalTime time = core::RationalTime::fromInteger(0),
                            const std::uint32_t proxyWidth = 0, const std::uint32_t proxyHeight = 0) {
        const auto compiled = compileTestCard(w, h, values);
        auto evaluated = proof::evaluateAt(compiled, time, proxyWidth, proxyHeight);
        require(evaluated.frame() != nullptr, "the card evaluates");
        return evaluated.frame();
    };

    // ---- Color Chart: 24 patches (6 by 4) in the largest centred 3:2 area, a dark grid between.
    // Row one: neutrals from white to black in steps of 20%; rows two and three: red, green, blue,
    // cyan, magenta and yellow at 100% and at 50%; row four: six skin tones from light to dark.
    {
        constexpr std::int64_t kW = 192;
        constexpr std::int64_t kH = 108;
        const auto frame = render(kW, kH, {{"card", std::int64_t{5}}});
        const auto& image = frame->processImage();
        // The chart is 162 x 108 pixels about the centre (left edge 15), 27 pixels a patch.
        const double left = (kW - 1.5 * kH) / 2.0;
        const double patch = kH / 4.0;
        const std::array<std::array<double, 3>, 6> skin{{{0.90, 0.74, 0.64}, {0.82, 0.62, 0.50},
                                                        {0.72, 0.52, 0.40}, {0.58, 0.40, 0.30},
                                                        {0.42, 0.28, 0.21}, {0.28, 0.18, 0.14}}};
        const std::array<std::array<double, 3>, 6> hues{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1},
                                                        {0, 1, 1}, {1, 0, 1}, {1, 1, 0}}};
        bool all = true;
        for (std::int64_t row = 0; row < 4; ++row)
            for (std::int64_t col = 0; col < 6; ++col) {
                std::array<double, 3> signal{};
                if (row == 0) {
                    const double level = 1.0 - 0.2 * static_cast<double>(col);
                    signal = {level, level, level};
                } else if (row < 3) {
                    const double level = row == 1 ? 1.0 : 0.5;
                    for (std::size_t k = 0; k < 3; ++k)
                        signal[k] = hues[static_cast<std::size_t>(col)][k] * level;
                } else {
                    signal = skin[static_cast<std::size_t>(col)];
                }
                const auto x = static_cast<std::int64_t>(std::floor(left + (static_cast<double>(col) + 0.5) * patch));
                const auto y = static_cast<std::int64_t>(std::floor((static_cast<double>(row) + 0.5) * patch));
                const bool ok = close(cardPixel(image, x, y), working(signal[0], signal[1], signal[2]));
                if (!ok)
                    std::cerr << "Color Chart patch (" << col << ", " << row << ") differs\n";
                all = all && ok;
            }
        expectations.expect(all, "all 24 Color Chart patch centres are the documented levels");
        // A dark separating line between patches and black outside the 3:2 area.
        expectations.expect(close(cardPixel(image, static_cast<std::int64_t>(left) + 0, 0), working(0.12, 0.12, 0.12)) &&
                                close(cardPixel(image, 1, 50), working(0.0, 0.0, 0.0)),
                            "the Color Chart has a dark grid between patches and black outside its 3:2 area");
    }

    // ---- Gray Ramp: a smooth ramp over the top four fifths, a PLUGE row in the bottom fifth of
    // nine segments, black alternating with steps at -4%, -2%, +2% and +4%.
    {
        constexpr std::int64_t kW = 180;
        constexpr std::int64_t kH = 100;
        const auto frame = render(kW, kH, {{"card", std::int64_t{6}}});
        const auto& image = frame->processImage();
        bool ramp = true;
        for (const std::int64_t x : {std::int64_t{0}, std::int64_t{45}, std::int64_t{90},
                                     std::int64_t{135}, kW - 1}) {
            const double level = (static_cast<double>(x) + 0.5) / static_cast<double>(kW);
            ramp = ramp && close(cardPixel(image, x, 40), working(level, level, level));
        }
        expectations.expect(ramp, "the Gray Ramp is the signal u across the top four fifths, from "
                                  "near black at the first pixel to near white at the last");
        // The ramp does not step: every neighbouring pair rises by the same fraction of the frame.
        bool smooth = true;
        for (std::int64_t x = 1; x < kW; ++x)
            smooth = smooth && cardPixel(image, x, 10)[0] > cardPixel(image, x - 1, 10)[0];
        expectations.expect(smooth, "the Gray Ramp rises at every pixel");
        const std::array<double, 9> pluge{0.0, -0.04, 0.0, -0.02, 0.0, 0.02, 0.0, 0.04, 0.0};
        bool steps = true;
        for (std::int64_t k = 0; k < 9; ++k) {
            const double level = pluge[static_cast<std::size_t>(k)];
            steps = steps && close(cardPixel(image, 10 + 20 * k, 90), working(level, level, level));
        }
        expectations.expect(steps, "the Gray Ramp's PLUGE row has nine segments: black, -4%, black, "
                                   "-2%, black, +2%, black, +4%, black");
        // The row boundary is at four fifths of the height.
        expectations.expect(close(cardPixel(image, 90, 79), working(0.5026, 0.5026, 0.5026), 2.0e-3F) &&
                                close(cardPixel(image, 90, 80), working(0.0, 0.0, 0.0)),
                            "the ramp ends and the PLUGE row begins at four fifths of the height");
    }

    // ---- Grid: lines every `spacing` pixels of `line width`, the two through the centre yellow,
    // cells square in a wide and in a portrait frame.
    {
        const std::map<std::string, ParameterValue> values{{"card", std::int64_t{7}},
                                                           {"gridSpacing", 20.0},
                                                           {"gridWidth", 6.0},
                                                           {"label", false}};
        for (const auto& [w, h] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
                 {161, 91}, {91, 161}, {101, 101}}) {
            const auto frame = render(w, h, values);
            const auto& image = frame->processImage();
            const auto cx = static_cast<std::int64_t>(w / 2);
            const auto cy = static_cast<std::int64_t>(h / 2);
            const auto line = working(0.85, 0.85, 0.85);
            const auto ground = working(0.08, 0.08, 0.08);
            const auto yellow = working(0.9, 0.8, 0.1);
            const std::string frameName = std::to_string(w) + " x " + std::to_string(h);
            // A vertical line 20 pixels right of centre: its centre and two pixels either side
            // (within half the 6-pixel width) are lines; four pixels away is ground; the middle of
            // a cell, ten pixels from each line, is ground.
            bool vertical = true;
            for (const std::int64_t d : {std::int64_t{0}, std::int64_t{2}, std::int64_t{-2}})
                vertical = vertical && close(cardPixel(image, cx + 20 + d, cy + 10), line);
            vertical = vertical && close(cardPixel(image, cx + 24, cy + 10), ground) &&
                       close(cardPixel(image, cx + 16, cy + 10), ground) &&
                       close(cardPixel(image, cx + 10, cy + 10), ground);
            bool horizontal = true;
            for (const std::int64_t d : {std::int64_t{0}, std::int64_t{2}, std::int64_t{-2}})
                horizontal = horizontal && close(cardPixel(image, cx + 10, cy + 20 + d), line);
            horizontal = horizontal && close(cardPixel(image, cx + 10, cy + 24), ground) &&
                         close(cardPixel(image, cx + 10, cy + 16), ground);
            expectations.expect(vertical && horizontal,
                                "Grid " + frameName + ": lines 20 pixels apart, 6 wide, ground between");
            expectations.expect(close(cardPixel(image, cx, cy + 10), yellow) &&
                                    close(cardPixel(image, cx + 10, cy), yellow) &&
                                    close(cardPixel(image, cx + 2, cy + 10), yellow),
                                "Grid " + frameName + ": the two lines through the centre are yellow");
            // Square cells: the same spacing across and down, whatever the frame's shape.
            expectations.expect(close(cardPixel(image, cx - 20, cy + 10), line) &&
                                    close(cardPixel(image, cx + 10, cy - 20), line) &&
                                    close(cardPixel(image, cx + 40, cy + 10), line) &&
                                    close(cardPixel(image, cx + 10, cy + 40), line) &&
                                    close(cardPixel(image, cx + 30, cy + 10), ground) &&
                                    close(cardPixel(image, cx + 10, cy + 30), ground),
                                "Grid " + frameName + ": cells are 20 pixels square across and down");
        }
        // A thinner line is thinner: with a width of 2 pixels the pixel 2 away from a line is ground.
        const auto thin = render(161, 91, {{"card", std::int64_t{7}}, {"gridSpacing", 20.0},
                                           {"gridWidth", 2.0}, {"label", false}});
        expectations.expect(close(cardPixel(thin->processImage(), 80 + 20, 55), working(0.85, 0.85, 0.85)) &&
                                close(cardPixel(thin->processImage(), 80 + 22, 55), working(0.08, 0.08, 0.08)),
                            "Grid: the line width is the width asked for");
    }

    // ---- Safe Areas: action safe at 93% (yellow) and title safe at 90% (cyan) of the width and
    // the height, a centre mark, and the crop guides for 16:9, 9:16, 1:1 and 2.39:1, each the
    // largest such rectangle centred, each switchable. The whole frame is compared with an
    // independent statement of those rectangles and a line of max(2, shorter side / 360) pixels.
    {
        constexpr std::uint32_t kW = 252;
        constexpr std::uint32_t kH = 140;
        struct Guide final {
            const char* role;
            double aspect;
            std::array<double, 3> colour;
        };
        const std::array<Guide, 4> guides{{{"guide169", 16.0 / 9.0, {0.95, 0.45, 0.1}},
                                          {"guide916", 9.0 / 16.0, {0.5, 0.9, 0.2}},
                                          {"guide11", 1.0, {0.95, 0.3, 0.7}},
                                          {"guide239", 2.39, {0.4, 0.5, 1.0}}}};
        const auto signalAt = [&](const double px, const double py, const unsigned mask) {
            const double w = kW;
            const double h = kH;
            const double lw = std::max(2.0, std::min(w, h) / 360.0);
            const double hw = lw / 2.0;
            const auto outline = [&](const double x0, const double y0, const double x1, const double y1) {
                if (px < x0 - hw || px > x1 + hw || py < y0 - hw || py > y1 + hw)
                    return false;
                return !(px > x0 + hw && px < x1 - hw && py > y0 + hw && py < y1 - hw);
            };
            const double cx = w / 2.0;
            const double cy = h / 2.0;
            if (outline(cx - 0.465 * w, cy - 0.465 * h, cx + 0.465 * w, cy + 0.465 * h))
                return std::array<double, 3>{0.9, 0.8, 0.1};
            if (outline(cx - 0.45 * w, cy - 0.45 * h, cx + 0.45 * w, cy + 0.45 * h))
                return std::array<double, 3>{0.1, 0.8, 0.9};
            for (std::size_t i = 0; i < guides.size(); ++i) {
                if ((mask & (1U << i)) == 0)
                    continue;
                double cw = w;
                double ch = h;
                if (h * guides[i].aspect <= w)
                    cw = h * guides[i].aspect;
                else
                    ch = w / guides[i].aspect;
                if (outline(cx - cw / 2.0, cy - ch / 2.0, cx + cw / 2.0, cy + ch / 2.0))
                    return guides[i].colour;
            }
            const double arm = std::min(w, h) * 0.03;
            const double dx = std::abs(px - cx);
            const double dy = std::abs(py - cy);
            if ((dx <= hw && dy <= arm) || (dy <= hw && dx <= arm))
                return std::array<double, 3>{0.95, 0.95, 0.95};
            return std::array<double, 3>{0.12, 0.12, 0.12};
        };
        // The card is drawn with four sub-samples a pixel (at a quarter and three quarters of it,
        // across and down), averaged in the signal before the colour space transform.
        const auto expected = [&](const std::int64_t x, const std::int64_t y, const unsigned mask) {
            std::array<double, 3> sum{};
            for (const double dy : {0.25, 0.75})
                for (const double dx : {0.25, 0.75}) {
                    const auto signal = signalAt(static_cast<double>(x) + dx, static_cast<double>(y) + dy, mask);
                    for (std::size_t c = 0; c < 3; ++c)
                        sum[c] += signal[c] / 4.0;
                }
            return working(sum[0], sum[1], sum[2]);
        };
        for (unsigned mask : {0U, 1U, 2U, 4U, 8U, 15U}) {
            std::map<std::string, ParameterValue> values{{"card", std::int64_t{9}}, {"label", false}};
            for (std::size_t i = 0; i < guides.size(); ++i)
                values[guides[i].role] = (mask & (1U << i)) != 0;
            const auto frame = render(kW, kH, values);
            const auto& image = frame->processImage();
            std::size_t wrong = 0;
            std::size_t yellow = 0;
            std::size_t cyan = 0;
            for (std::int64_t y = 0; y < kH; ++y)
                for (std::int64_t x = 0; x < kW; ++x) {
                    const auto want = expected(x, y, mask);
                    const auto got = cardPixel(image, x, y);
                    wrong += close(got, want) ? 0U : 1U;
                    yellow += close(got, working(0.9, 0.8, 0.1)) ? 1U : 0U;
                    cyan += close(got, working(0.1, 0.8, 0.9)) ? 1U : 0U;
                }
            expectations.expect(wrong == 0 && yellow > 0 && cyan > 0,
                                "Safe Areas with the crop guides " + std::to_string(mask) +
                                    ": action 93%, title 90%, each selected crop guide and the centre "
                                    "mark are where the guide puts them (" + std::to_string(wrong) +
                                    " pixels differ)");
        }
        // Each guide switches: with its toggle off its line is the ground.
        const auto onFrame = render(kW, kH, {{"card", std::int64_t{9}}, {"label", false},
                                             {"guide169", false}, {"guide916", true}, {"guide11", false},
                                             {"guide239", false}});
        const auto offFrame = render(kW, kH, {{"card", std::int64_t{9}}, {"label", false},
                                              {"guide169", false}, {"guide916", false},
                                              {"guide11", false}, {"guide239", false}});
        // The 9:16 guide is 78.75 pixels wide about the centre (126): its right edge is at x = 165.375.
        expectations.expect(close(cardPixel(onFrame->processImage(), 165, 30), working(0.5, 0.9, 0.2)) &&
                                close(cardPixel(offFrame->processImage(), 165, 30), working(0.12, 0.12, 0.12)),
                            "a crop guide is drawn only when switched on");
    }

    // ---- UV Grid: square cells (the width divided by Cells across), coloured by position -- red
    // across, green up -- a dark line between cells and each cell's column and row written in it.
    {
        constexpr std::uint32_t kSize = 200;
        const auto frame = render(kSize, kSize, {{"card", std::int64_t{10}}, {"uvCells", std::int64_t{2}},
                                                 {"label", false}});
        const auto& image = frame->processImage();
        const auto position = [&](const std::int64_t x, const std::int64_t y) {
            const double u = (static_cast<double>(x) + 0.5) / kSize;
            const double v = 1.0 - (static_cast<double>(y) + 0.5) / kSize;
            return working(u, v, 0.25);
        };
        // Corners of each of the four cells (away from the digits written at their centres).
        bool corners = true;
        for (const auto& [x, y] : std::vector<std::pair<std::int64_t, std::int64_t>>{
                 {5, 5}, {94, 5}, {5, 94}, {94, 94}, {105, 5}, {194, 5}, {105, 94}, {194, 94},
                 {5, 105}, {94, 105}, {5, 194}, {94, 194}, {105, 105}, {194, 105}, {105, 194}, {194, 194}})
            corners = corners && close(cardPixel(image, x, y), position(x, y));
        expectations.expect(corners, "UV Grid: a cell's colour is its position: red across, green up, "
                                     "in every corner of every cell");
        // The dark line between cells: a cell is 100 pixels, the line about 3 wide about the edge.
        expectations.expect(close(cardPixel(image, 99, 50), working(0.0, 0.0, 0.0)) &&
                                close(cardPixel(image, 100, 50), working(0.0, 0.0, 0.0)) &&
                                close(cardPixel(image, 50, 99), working(0.0, 0.0, 0.0)) &&
                                close(cardPixel(image, 50, 0), working(0.0, 0.0, 0.0)),
                            "UV Grid: a dark line separates the cells");
        // The cell's column and row are written in it: white ink near its centre, and the writing
        // differs from cell to cell.
        const auto ink = [&](const std::int64_t cellX, const std::int64_t cellY) {
            std::vector<bool> lit;
            for (std::int64_t y = 20; y < 80; ++y)
                for (std::int64_t x = 20; x < 80; ++x)
                    lit.push_back(close(cardPixel(image, cellX * 100 + x, cellY * 100 + y),
                                        working(1.0, 1.0, 1.0)));
            return lit;
        };
        const auto first = ink(0, 0);
        const auto second = ink(1, 0);
        const auto third = ink(0, 1);
        expectations.expect(std::ranges::count(first, true) > 20 && std::ranges::count(second, true) > 20 &&
                                first != second && first != third && second != third,
                            "UV Grid: every cell has its own column and row written in white");
        // The orientation: red grows to the right and green to the top.
        expectations.expect(cardPixel(image, 190, 50)[0] > cardPixel(image, 110, 50)[0] &&
                                cardPixel(image, 5, 10)[1] > cardPixel(image, 5, 190)[1],
                            "UV Grid: red rises across and green rises up");
    }

    // ---- Moving Bar: a white bar of Bar width (a percent of the frame) crossing in Crossing time
    // seconds, over a dark ground; at phase p its leading edge is p (frame + bar) - bar from the
    // start. A 200 x 100 frame, a 10% bar (20 pixels) crossing in 2 s.
    {
        const auto bar = [&](const core::RationalTime time, const std::int64_t direction,
                             const std::uint32_t proxyWidth = 0, const std::uint32_t proxyHeight = 0) {
            return render(200, 100,
                          {{"card", std::int64_t{13}}, {"barWidth", 10.0}, {"barDuration", 2.0},
                           {"barDirection", direction}, {"label", false}},
                          time, proxyWidth, proxyHeight);
        };
        const auto white = working(1.0, 1.0, 1.0);
        const auto ground = working(0.1, 0.1, 0.1);
        // Phase 0: the bar has not yet entered. A half second in: phase one quarter, the bar at
        // [35, 55). One second: phase one half, the bar centred at [90, 110).
        const auto start = bar(core::RationalTime::fromInteger(0), 0);
        const auto quarter = bar(*core::RationalTime::create(1, 2), 0);
        const auto half = bar(core::RationalTime::fromInteger(1), 0);
        bool offFrame = true;
        for (std::int64_t x = 0; x < 200; ++x)
            offFrame = offFrame && close(cardPixel(start->processImage(), x, 50), ground);
        expectations.expect(offFrame, "Moving Bar: at the start of the crossing the bar is not in the frame");
        const auto& q = quarter->processImage();
        expectations.expect(close(cardPixel(q, 34, 30), ground) && close(cardPixel(q, 35, 30), white) &&
                                close(cardPixel(q, 54, 30), white) && close(cardPixel(q, 55, 30), ground) &&
                                close(cardPixel(q, 10, 30), ground) && close(cardPixel(q, 150, 30), ground),
                            "Moving Bar: a quarter of the way the 20-pixel bar covers pixels 35 to 54");
        const auto& hf = half->processImage();
        expectations.expect(close(cardPixel(hf, 89, 70), ground) && close(cardPixel(hf, 90, 70), white) &&
                                close(cardPixel(hf, 109, 70), white) && close(cardPixel(hf, 110, 70), ground),
                            "Moving Bar: half way the bar is centred in the frame");
        // The bar spans the full height; vertical crossing moves it down over the rows.
        const auto vertical = bar(core::RationalTime::fromInteger(1), 1);
        const auto& v = vertical->processImage();
        // A 10% bar of a vertical crossing is 10 pixels of the 100-pixel height; centred: [45, 55).
        expectations.expect(close(cardPixel(v, 20, 44), ground) && close(cardPixel(v, 20, 45), white) &&
                                close(cardPixel(v, 150, 54), white) && close(cardPixel(v, 20, 55), ground),
                            "Moving Bar: a vertical crossing moves the bar down the rows");
        // On a half-size proxy the bar is where the fractions put it.
        const auto proxy = bar(*core::RationalTime::create(1, 2), 0, 100, 50);
        const auto& p = proxy->processImage();
        // The bar is 10 pixels here, a quarter of the way: [17.5, 27.5); pixels 17 and 27 are half covered.
        expectations.expect(close(cardPixel(p, 16, 20), ground) && close(cardPixel(p, 18, 20), white) &&
                                close(cardPixel(p, 26, 20), white) && close(cardPixel(p, 28, 20), ground),
                            "Moving Bar at half size: the bar is at the same fractions of the frame");
    }

    // ---- Alpha Ramp: six bands of colour, each with an alpha rising from 0 to 1 across the width
    // over transparency; the colour is straight, the pixels premultiplied.
    {
        constexpr std::uint32_t kW = 200;
        constexpr std::uint32_t kH = 120;
        const auto frame = render(kW, kH, {{"card", std::int64_t{15}}});
        const auto& image = frame->processImage();
        bool alphaRamp = true;
        bool premultiplied = true;
        for (std::int64_t band = 0; band < 6; ++band) {
            const std::int64_t y = band * 20 + 10;
            std::array<float, 3> straight{};
            bool first = true;
            bool constantColour = true;
            for (const std::int64_t x : {std::int64_t{49}, std::int64_t{99}, std::int64_t{149},
                                         std::int64_t{199}}) {
                const auto p = cardPixel(image, x, y);
                const double alpha = (static_cast<double>(x) + 0.5) / kW;
                alphaRamp = alphaRamp && std::abs(static_cast<double>(p[3]) - alpha) < 1.0e-5;
                const std::array<float, 3> colour{p[0] / p[3], p[1] / p[3], p[2] / p[3]};
                if (first)
                    straight = colour;
                first = false;
                for (std::size_t c = 0; c < 3; ++c)
                    constantColour = constantColour && std::abs(colour[c] - straight[c]) < 2.0e-4F;
            }
            premultiplied = premultiplied && constantColour;
        }
        expectations.expect(alphaRamp, "Alpha Ramp: alpha is the column's fraction of the width in every band");
        expectations.expect(premultiplied, "Alpha Ramp: each band's colour is constant and straight, the "
                                           "pixels premultiplied by the alpha");
        // Transparent at the left edge, opaque at the right; white and red in the first two bands.
        const auto left = cardPixel(image, 0, 10);
        expectations.expect(left[3] < 0.0051F && left[0] < 0.0051F,
                            "Alpha Ramp: nearly transparent at the left edge");
        const auto whiteBand = cardPixel(image, 99, 10);
        const auto redBand = cardPixel(image, 99, 30);
        const double a = 99.5 / kW;
        expectations.expect(close(whiteBand, {static_cast<float>(a), static_cast<float>(a),
                                              static_cast<float>(a), static_cast<float>(a)}) &&
                                close(redBand, {static_cast<float>(a), 0.0F, 0.0F, static_cast<float>(a)}),
                            "Alpha Ramp: the first band is white and the second red, over transparency");
        // At half size the same fractions.
        const auto proxy = render(kW, kH, {{"card", std::int64_t{15}}}, core::RationalTime::fromInteger(0),
                                  100, 60);
        const auto p = cardPixel(proxy->processImage(), 49, 5);
        expectations.expect(std::abs(static_cast<double>(p[3]) - 49.5 / 100.0) < 1.0e-5,
                            "Alpha Ramp at half size: alpha is the same fraction of the width");
    }
}
