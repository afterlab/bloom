// Included inside the snapshot compiler test's anonymous namespace after the Chromatic Aberration
// tests, whose checker fixture and sampling helpers it reuses.
//
// #508 Light Rays: the node lowers to one field raster reading the input twice, once gathered along
// each pixel's ray and once as itself. The tests prove the lowering (which operation, which program,
// which operands, time dependence, mute and the identities), then evaluate the plan on the CPU
// reference over a checker grid against an independent double-precision gather of the input: from
// luminance and from alpha, the falloff, the length, the boost and the tint, the input kept under
// the rays, HDR and partial alpha kept, proxy, region of interest, the pixel budget and
// cancellation.

[[nodiscard]] LensFixture
raysFixture(const std::unordered_map<std::string, document::ParameterValue>& values = {},
            const bool throughNode = true) {
    return lensFixture(values, "image", throughNode, document::kLightRaysNodeType);
}

struct RaysModel final {
    bool alpha = false;
    // The centre as authored: pixels from the composition's centre, y down.
    document::Vec2d centre{};
    double length = 0.5;
    double falloff = 1.0;
    double boost = 1.5;
    std::array<double, 3> tint{1.0, 1.0, 1.0};
};

// The input plus the rays gathered at the field position (x, y), in double precision.
[[nodiscard]] std::array<double, 4> raysExpected(const RaysModel& m, const render::Rgba32fImage& source,
                                                 const double width, const double height,
                                                 const double x, const double y) {
    auto result = lensSample(source, width, height, x, y);
    const auto dx = m.centre.x - x;
    const auto dy = -m.centre.y - y;
    const auto reach = std::hypot(dx, dy) * m.length;
    if (!(reach > 0.0))
        return result;
    // The sample count is the kernel's, found in binary32 as it is.
    const auto n = std::min(static_cast<int>(std::floor(static_cast<float>(reach) * 0.5F)) + 1, 96);
    double sum = 0.0;
    double weights = 0.0;
    for (int i = 0; i < n; ++i) {
        const auto t = (i + 0.5) / n;
        const auto texel = lensSample(source, width, height, x + dx * t * m.length,
                                      y + dy * t * m.length);
        const auto value = m.alpha ? texel[3]
                                   : 0.2126 * texel[0] + 0.7152 * texel[1] + 0.0722 * texel[2];
        const auto w = std::pow(1.0 - t, m.falloff);
        sum += value * w;
        weights += w;
    }
    const auto mean = weights > 0.0 ? sum / weights * m.boost : 0.0;
    result[0] += mean * m.tint[0];
    result[1] += mean * m.tint[1];
    result[2] += mean * m.tint[2];
    result[3] += std::min(mean, 1.0);
    result[3] = std::min(result[3], 1.0);
    return result;
}

void testLightRaysLowering(Expectations& expectations) {
    using namespace document;
    using core::field::FieldOp;
    const std::unordered_map<std::string, ParameterValue> lit{
        {"source", std::int64_t{1}}, {"center", Vec2d{40.0, -30.0}}, {"length", 0.6},
        {"falloff", 2.0},            {"boost", 3.0},                  {"tint", core::Color4d{1.0, 0.5, 0.25, 1.0}}};
    {
        auto fixture = raysFixture(lit);
        const auto node = fixture.recipe.node;
        const auto params = fixture.recipe.parameters;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr, "Light Rays compiles");
        if (!result.plan)
            return;
        const auto ops = recipeOperations(*result.plan, node);
        require(ops.size() == 1, "Light Rays lowers to one raster");
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
                                lensInstructions(raster->program, FieldOp::Rays) == 1 &&
                                lensInstructions(raster->program, FieldOp::Image) == 1,
                            "the program is one gather and one premultiplied image read");
        const auto& rays = *std::ranges::find_if(raster->program.instructions, [](const auto& i) {
            return i.op == static_cast<std::uint8_t>(FieldOp::Rays);
        });
        expectations.expect(rays.choices == std::vector<std::int64_t>{1} && rays.scalars.size() == 3 &&
                                rays.vectors.size() == 1 && rays.scalars[0].id == params.at("length") &&
                                rays.scalars[1].id == params.at("falloff") &&
                                rays.scalars[2].id == params.at("boost") &&
                                std::get<double>(rays.scalars[0].source) == 0.6 &&
                                std::get<double>(rays.scalars[1].source) == 2.0 &&
                                std::get<double>(rays.scalars[2].source) == 3.0 &&
                                rays.vectors[0].id == params.at("center") &&
                                std::get<Vec2d>(rays.vectors[0].source) == Vec2d{40.0, -30.0},
                            "the gather owns the source, the length, the falloff, the boost and the centre");
        expectations.expect(!result.plan->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(ops[0])),
                            "a constant Light Rays does not vary with time");
        expectations.expect(lensSequence(raster->program) ==
                                lensSequence(field_test_programs::lightRaysProgram(1, true)),
                            "the coverage fixture's program is the lowering's");
    }

    // A constant length or boost of zero, and a muted node, pass the Image through.
    for (const char* zero : {"length", "boost", "muted"}) {
        std::unordered_map<std::string, ParameterValue> values = lit;
        if (std::string_view(zero) != "muted")
            values[zero] = 0.0;
        auto fixture = raysFixture(values);
        if (std::string_view(zero) == "muted")
            fixture.recipe.project.findComposition(kCompositionId)
                ->nodeLayout()[fixture.recipe.node]
                .muted = true;
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr && recipeOperations(*result.plan, node).empty(),
                            std::string("Light Rays passes its Image through when ") + zero);
    }

    // A keyed length keeps the raster even at zero and makes the plan time dependent.
    {
        auto fixture = raysFixture();
        auto* composition = fixture.recipe.project.findComposition(kCompositionId);
        constexpr auto curveId = AnimationCurveId::fromRaw(1410);
        require(composition->animationCurves().insert(ScalarAnimationCurve{
                    curveId,
                    {{KeyframeId::fromRaw(1411), core::RationalTime::fromInteger(0), 0.0,
                      KeyframeInterpolation::Linear},
                     {KeyframeId::fromRaw(1412), core::RationalTime::fromInteger(1), 0.5,
                      KeyframeInterpolation::Linear}}}),
                "the length curve is publishable");
        require(composition->parameters().setSource(fixture.recipe.parameters.at("length"),
                                                    AnimationCurveSource{curveId}),
                "the length animates");
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr, "an animated Light Rays compiles");
        if (result.plan) {
            const auto ops = recipeOperations(*result.plan, node);
            expectations.expect(ops.size() == 1 && result.plan->operationTimeDependent(
                                                       runtime::OperationIndex::fromRaw(ops[0])),
                                "an animated length keeps the raster and varies with time");
        }
    }
}

void testLightRaysPixels(Expectations& expectations) {
    using namespace document;
    constexpr double kWidth = 1920.0;
    constexpr double kHeight = 1080.0;
    constexpr std::uint32_t kProxyWidth = 240;
    constexpr std::uint32_t kProxyHeight = 135;

    auto bareFixture = raysFixture({}, false);
    const auto bareResult = compileRecipe(std::move(bareFixture.recipe.project));
    require(bareResult.plan != nullptr, "the bare checker compiles");
    const auto input = lensEvaluate(bareResult.plan, kProxyWidth, kProxyHeight);
    require(input.frame() != nullptr, "the bare checker evaluates");
    const auto& source = input.frame()->processImage();

    struct Case final {
        RaysModel model;
        const char* name;
    };
    std::vector<Case> cases;
    cases.push_back({RaysModel{false, Vec2d{0.0, 0.0}, 0.5, 1.0, 1.5, {1.0, 1.0, 1.0}},
                     "luminance, centred"});
    cases.push_back({RaysModel{true, Vec2d{200.0, -100.0}, 0.8, 2.0, 2.0, {1.0, 1.0, 1.0}},
                     "alpha, off centre, steeper falloff"});
    cases.push_back({RaysModel{false, Vec2d{-120.0, 60.0}, 1.0, 0.0, 1.0, {1.0, 0.5, 0.25}},
                     "luminance, even weights, tinted"});
    for (const auto& item : cases) {
        const auto& m = item.model;
        auto fixture = raysFixture({{"source", std::int64_t{m.alpha ? 1 : 0}},
                                    {"center", m.centre},
                                    {"length", m.length},
                                    {"falloff", m.falloff},
                                    {"boost", m.boost},
                                    {"tint", core::Color4d{m.tint[0], m.tint[1], m.tint[2], 1.0}}});
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        if (!result.plan) {
            expectations.expect(false, "the rays plan compiles");
            continue;
        }
        const auto evaluated = lensEvaluate(result.plan, kProxyWidth, kProxyHeight);
        expectations.expect(evaluated.frame() != nullptr,
                            std::string(item.name) + ": the picture evaluates");
        if (!evaluated.frame())
            continue;
        const auto& image = evaluated.frame()->processImage();
        std::size_t mismatched = 0;
        std::size_t brightened = 0;
        double worst = 0.0;
        for (std::uint32_t j = 0; j < kProxyHeight; ++j)
            for (std::uint32_t i = 0; i < kProxyWidth; ++i) {
                const auto fx = (i + 0.5) * kWidth / kProxyWidth - kWidth / 2.0;
                const auto fy = kHeight / 2.0 - (j + 0.5) * kHeight / kProxyHeight;
                const auto expected = raysExpected(m, source, kWidth, kHeight, fx, fy);
                const auto got = lensPixel(image, i, j);
                const auto here = lensPixel(source, i, j);
                double error = 0.0;
                for (std::size_t c = 0; c < 4; ++c)
                    error = std::max(error, std::abs(static_cast<double>(got[c]) - expected[c]) /
                                                std::max(1.0, std::abs(expected[c])));
                worst = std::max(worst, error);
                // The sample count may differ by one at a rounding boundary of the reach.
                mismatched += error > 4.0e-3 ? 1U : 0U;
                brightened += got[0] > here[0] + 0.01F ? 1U : 0U;
            }
        expectations.expect(mismatched * 200U < static_cast<std::size_t>(kProxyWidth) * kProxyHeight,
                            std::string(item.name) +
                                ": the pixels are the input plus the independently gathered rays");
        expectations.expect(brightened > 0, std::string(item.name) + ": the rays add light");
        static_cast<void>(worst);
    }

    // The input is kept under the rays: with a tiny boost the picture is the input.
    {
        auto fixture = raysFixture({{"boost", 1.0e-6}});
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the faint rays compile");
        const auto evaluated = lensEvaluate(result.plan, kProxyWidth, kProxyHeight);
        require(evaluated.frame() != nullptr, "the faint rays evaluate");
        double worst = 0.0;
        for (std::uint32_t j = 0; j < kProxyHeight; j += 2)
            for (std::uint32_t i = 0; i < kProxyWidth; i += 2) {
                const auto p = lensPixel(evaluated.frame()->processImage(), i, j);
                const auto q = lensPixel(source, i, j);
                for (std::size_t c = 0; c < 4; ++c)
                    worst = std::max(worst, std::abs(static_cast<double>(p[c]) - static_cast<double>(q[c])));
            }
        expectations.expect(worst < 1.0e-4, "the input is kept under the rays");
    }

    // A region of interest reads the whole frame's pixels; the pixel budget refuses the output; a
    // cancelled evaluation publishes nothing.
    {
        auto fixture = raysFixture({{"center", Vec2d{200.0, 100.0}}, {"length", 0.9}});
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
                                       .id = runtime::TaskOwnerId::fromRaw(5082)};
        bool cancelledOutcome = false;
        auto submission = scheduler.submit<void>(
            runtime::TaskRequest("Light Rays cancellation", owner),
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
        expectations.expect(taken && cancelledOutcome, "a cancelled ray gather publishes no image");
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        expectations.expect(scheduler.isQuiescent(), "the cancelled ray gather shuts down safely");
    }
}
