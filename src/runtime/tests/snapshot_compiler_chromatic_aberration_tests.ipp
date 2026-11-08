// Included inside the snapshot compiler test's anonymous namespace after the Lens Distortion tests,
// whose checker fixture and sampling helpers it reuses.
//
// #486 Chromatic Aberration: the node lowers to one field raster reading the input three times. The
// tests prove the lowering (which operation, which program, which operands, time dependence, mute and
// the identity), then evaluate the plan on the CPU reference over a checker grid: red read further
// out and blue the same distance further in, each against an independent double-precision reading of
// the input, green untouched, the alpha the largest of the three, HDR and partial alpha kept, proxy, region of interest, the pixel budget and cancellation.

[[nodiscard]] LensFixture
aberrationFixture(const std::unordered_map<std::string, document::ParameterValue>& values = {},
                  const bool throughNode = true) {
    return lensFixture(values, "image", throughNode, document::kChromaticAberrationNodeType);
}

void testChromaticAberrationLowering(Expectations& expectations) {
    using namespace document;
    using core::field::FieldOp;
    const std::unordered_map<std::string, ParameterValue> shifted{{"amount", 0.02},
                                                                  {"center", Vec2d{40.0, -30.0}}};
    {
        auto fixture = aberrationFixture(shifted);
        const auto node = fixture.recipe.node;
        const auto params = fixture.recipe.parameters;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr, "Chromatic Aberration compiles");
        if (!result.plan)
            return;
        const auto ops = recipeOperations(*result.plan, node);
        require(ops.size() == 1, "Chromatic Aberration lowers to one raster");
        const auto* raster = lensRaster(*result.plan, ops[0]);
        require(raster != nullptr, "the raster is a field raster");
        const auto& effect = std::get<runtime::CompiledImageEffect>(result.plan->operations()[ops[0]]);
        const auto checker = lensCheckerOperation(*result.plan);
        expectations.expect(checker && effect.input.value() == *checker &&
                                effect.fieldImages.size() == 1 &&
                                effect.fieldImages[0].value() == *checker,
                            "the raster covers the input and reads it");
        expectations.expect(raster->samples == 1 &&
                                raster->program.kind ==
                                    static_cast<std::uint8_t>(
                                        core::field::FieldResultKind::Premultiplied) &&
                                lensInstructions(raster->program, FieldOp::LensWarp) == 1 &&
                                lensInstructions(raster->program, FieldOp::Image) == 3,
                            "the program is one warp and three premultiplied image reads");
        const auto& warp = *std::ranges::find_if(raster->program.instructions, [](const auto& i) {
            return i.op == static_cast<std::uint8_t>(FieldOp::LensWarp);
        });
        expectations.expect(warp.choices == std::vector<std::int64_t>{0} &&
                                warp.scalars.size() == 5 && warp.vectors.size() == 1 &&
                                warp.scalars[0].id == params.at("amount") &&
                                std::get<double>(warp.scalars[0].source) == 0.02 &&
                                std::get<double>(warp.scalars[1].source) == 0.0 &&
                                std::get<double>(warp.scalars[2].source) == 0.0 &&
                                std::get<double>(warp.scalars[3].source) == 0.0 &&
                                std::get<double>(warp.scalars[4].source) ==
                                    0.5 * std::hypot(1920.0, 1080.0) &&
                                warp.vectors[0].id == params.at("center") &&
                                std::get<Vec2d>(warp.vectors[0].source) == Vec2d{40.0, -30.0},
                            "the warp owns the amount as its radial term and the centre");
        expectations.expect(!result.plan->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(ops[0])),
                            "a constant Chromatic Aberration does not vary with time");
        expectations.expect(lensSequence(raster->program) ==
                                lensSequence(field_test_programs::chromaticAberrationProgram(1)),
                            "the coverage fixture's program is the lowering's");
    }

    // A constant amount of zero, as the node starts, and a muted node pass the Image through.
    for (const bool muted : {false, true}) {
        auto fixture = aberrationFixture(muted ? shifted
                                               : std::unordered_map<std::string, ParameterValue>{});
        if (muted)
            fixture.recipe.project.findComposition(kCompositionId)
                ->nodeLayout()[fixture.recipe.node]
                .muted = true;
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr &&
                                recipeOperations(*result.plan, node).empty(),
                            muted ? "a muted Chromatic Aberration passes its Image through"
                                  : "a zero amount passes the Image through");
    }

    // A keyed amount keeps the raster even at zero and makes the plan time dependent.
    {
        auto fixture = aberrationFixture();
        auto* composition = fixture.recipe.project.findComposition(kCompositionId);
        constexpr auto curveId = AnimationCurveId::fromRaw(1400);
        require(composition->animationCurves().insert(ScalarAnimationCurve{
                    curveId,
                    {{KeyframeId::fromRaw(1401), core::RationalTime::fromInteger(0), 0.0,
                      KeyframeInterpolation::Linear},
                     {KeyframeId::fromRaw(1402), core::RationalTime::fromInteger(1), 0.03,
                      KeyframeInterpolation::Linear}}}),
                "the amount curve is publishable");
        require(composition->parameters().setSource(fixture.recipe.parameters.at("amount"),
                                                    AnimationCurveSource{curveId}),
                "the amount animates");
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr, "an animated Chromatic Aberration compiles");
        if (result.plan) {
            const auto ops = recipeOperations(*result.plan, node);
            expectations.expect(ops.size() == 1 && result.plan->operationTimeDependent(
                                                       runtime::OperationIndex::fromRaw(ops[0])),
                                "an animated amount keeps the raster and varies with time");
        }
    }
}

void testChromaticAberrationPixels(Expectations& expectations) {
    using namespace document;
    constexpr double kWidth = 1920.0;
    constexpr double kHeight = 1080.0;
    constexpr std::uint32_t kProxyWidth = 240;
    constexpr std::uint32_t kProxyHeight = 135;
    const auto scale = 0.5 * std::hypot(kWidth, kHeight);

    struct Case final {
        double amount;
        Vec2d centre;
        const char* name;
    };
    const std::array<Case, 3> cases{{{0.02, Vec2d{0.0, 0.0}, "outward red, centred"},
                                     {0.03, Vec2d{40.0, -30.0}, "outward red, off centre"},
                                     {-0.02, Vec2d{0.0, 0.0}, "inward red, centred"}}};
    auto bareFixture = aberrationFixture({}, false);
    const auto bareResult = compileRecipe(std::move(bareFixture.recipe.project));
    require(bareResult.plan != nullptr, "the bare checker compiles");
    const auto input = lensEvaluate(bareResult.plan, kProxyWidth, kProxyHeight);
    require(input.frame() != nullptr, "the bare checker evaluates");
    const auto& source = input.frame()->processImage();

    for (const auto& item : cases) {
        auto fixture = aberrationFixture({{"amount", item.amount}, {"center", item.centre}});
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        if (!result.plan) {
            expectations.expect(false, "the aberration plan compiles");
            continue;
        }
        const auto evaluated = lensEvaluate(result.plan, kProxyWidth, kProxyHeight);
        expectations.expect(evaluated.frame() != nullptr,
                            std::string(item.name) + ": the picture evaluates");
        if (!evaluated.frame())
            continue;
        const auto& image = evaluated.frame()->processImage();
        double worst = 0.0;
        double greenError = 0.0;
        std::size_t moved = 0;
        std::size_t fringes = 0;
        for (std::uint32_t j = 0; j < kProxyHeight; ++j)
            for (std::uint32_t i = 0; i < kProxyWidth; ++i) {
                const auto fx = (i + 0.5) * kWidth / kProxyWidth - kWidth / 2.0;
                const auto fy = kHeight / 2.0 - (j + 0.5) * kHeight / kProxyHeight;
                // The independent reading: the radial term of the lens model about the centre.
                const auto cx = item.centre.x;
                const auto cy = -item.centre.y;
                const auto nx = (fx - cx) / scale;
                const auto ny = (fy - cy) / scale;
                const auto r2 = nx * nx + ny * ny;
                const auto outward = 1.0 + item.amount * r2;
                const auto inward = 1.0 - item.amount * r2;
                const auto red = lensSample(source, kWidth, kHeight, cx + nx * outward * scale,
                                            cy + ny * outward * scale);
                const auto green = lensSample(source, kWidth, kHeight, fx, fy);
                const auto blue = lensSample(source, kWidth, kHeight, cx + nx * inward * scale,
                                             cy + ny * inward * scale);
                const auto got = lensPixel(image, i, j);
                const std::array<double, 4> expected{red[0], green[1], blue[2],
                                                     std::max({red[3], green[3], blue[3]})};
                for (std::size_t c = 0; c < 4; ++c)
                    worst = std::max(worst, std::abs(static_cast<double>(got[c]) - expected[c]));
                const auto here = lensPixel(source, i, j);
                greenError = std::max(greenError, std::abs(static_cast<double>(got[1]) - green[1]));
                moved += (got[0] != here[0] || got[2] != here[2]) ? 1U : 0U;
                fringes += (std::abs(got[0] - here[0]) > 0.05F) ? 1U : 0U;
            }
        expectations.expect(worst < 3.0e-4,
                            std::string(item.name) +
                                ": red, green, blue and alpha are the input read at their positions");
        expectations.expect(greenError < 3.0e-4,
                            std::string(item.name) + ": green is read where it is");
        expectations.expect(moved > 0 && fringes > 0,
                            std::string(item.name) + ": the colours are pulled apart");
    }

    // Green is read where it is whichever way the colours move.
    {
        auto plus = aberrationFixture({{"amount", 0.02}});
        auto minus = aberrationFixture({{"amount", -0.02}});
        const auto plusResult = compileRecipe(std::move(plus.recipe.project));
        const auto minusResult = compileRecipe(std::move(minus.recipe.project));
        require(plusResult.plan != nullptr && minusResult.plan != nullptr, "both signs compile");
        const auto a = lensEvaluate(plusResult.plan, kProxyWidth, kProxyHeight);
        const auto b = lensEvaluate(minusResult.plan, kProxyWidth, kProxyHeight);
        require(a.frame() != nullptr && b.frame() != nullptr, "both signs evaluate");
        double worst = 0.0;
        for (std::uint32_t j = 8; j < kProxyHeight - 8; j += 3)
            for (std::uint32_t i = 8; i < kProxyWidth - 8; i += 3) {
                const auto p = lensPixel(a.frame()->processImage(), i, j);
                const auto m = lensPixel(b.frame()->processImage(), i, j);
                worst = std::max(worst, std::abs(static_cast<double>(p[1]) - static_cast<double>(m[1])));
            }
        expectations.expect(worst < 1.0e-5, "green is the same whichever way the colours move");
    }

    // The checker's HDR colour and partial alpha pass through whole at a tiny amount: a cell's
    // middle keeps its premultiplied colour.
    {
        auto fixture = aberrationFixture({{"amount", 0.0004}});
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the tiny amount compiles");
        const auto evaluated = lensEvaluate(result.plan, kProxyWidth, kProxyHeight);
        require(evaluated.frame() != nullptr, "the tiny amount evaluates");
        const auto& image = evaluated.frame()->processImage();
        std::size_t halfAlpha = 0;
        for (std::uint32_t j = 60; j < 70; ++j)
            for (std::uint32_t i = 110; i < 126; ++i) {
                const auto p = lensPixel(image, i, j);
                if (std::abs(p[3] - 0.5F) < 1.0e-5F) {
                    ++halfAlpha;
                    // 2.0, 1.0, 0.5 straight at alpha 0.5 are 1.0, 0.5, 0.25 premultiplied.
                    expectations.expect(std::abs(p[0] - 1.0F) < 1.0e-3F &&
                                            std::abs(p[1] - 0.5F) < 1.0e-3F &&
                                            std::abs(p[2] - 0.25F) < 1.0e-3F,
                                        "a partly transparent HDR cell keeps its premultiplied colour");
                }
            }
        expectations.expect(halfAlpha > 0, "a tiny amount keeps whole cells of the grid");
    }

    // A region of interest reads the whole frame's pixels; the pixel budget refuses the output; a
    // cancelled evaluation publishes nothing.
    {
        auto fixture = aberrationFixture({{"amount", 0.04}, {"center", Vec2d{200.0, 100.0}}});
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the regional fixture compiles");
        const auto whole = lensEvaluate(result.plan, kProxyWidth, kProxyHeight);
        const auto region = render::ImageWindow::create(150, 70, 60, 40);
        const auto part = lensEvaluate(result.plan, kProxyWidth, kProxyHeight, *region.value());
        expectations.expect(whole.frame() != nullptr && part.frame() != nullptr,
                            "a region of interest evaluates");
        if (whole.frame() && part.frame()) {
            bool same = true;
            for (auto y = region.value()->originY(); y < region.value()->maxYExclusive(); ++y)
                for (auto x = region.value()->originX(); x < region.value()->maxXExclusive(); ++x)
                    same = same && lensPixel(part.frame()->processImage(), x, y) ==
                                       lensPixel(whole.frame()->processImage(), x, y);
            expectations.expect(same, "a region's pixels are the whole frame's");
        }
        const auto refused = lensEvaluate(result.plan, kProxyWidth, kProxyHeight, std::nullopt, 64);
        expectations.expect(refused.frame() == nullptr, "the pixel budget refuses the raster");
        runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
        const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                       .id = runtime::TaskOwnerId::fromRaw(4862)};
        bool cancelledOutcome = false;
        auto submission = scheduler.submit<void>(
            runtime::TaskRequest("Chromatic Aberration cancellation", owner),
            [&](runtime::TaskContext& context) {
                static_cast<void>(scheduler.cancelOwner(owner));
                const auto cancelled = lensEvaluate(result.plan, kProxyWidth, kProxyHeight,
                                                    std::nullopt, std::size_t{1} << 28U,
                                                    context.cancellation());
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
        expectations.expect(taken && cancelledOutcome, "a cancelled aberration publishes no image");
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        expectations.expect(scheduler.isQuiescent(), "the cancelled aberration shuts down safely");
    }
}
