// #524 Four-Color Gradient through the compiler and the CPU evaluator: the lowering (a transparent
// canvas and one field raster, no instruction of its own), the coverage fixture's program being the
// lowering's, the exact color at a point, an independent double-precision reading of the inverse
// distance weighting at other pixels, a symmetric layout giving a symmetric picture, premultiplied
// partial alpha, proxy, mute and cancellation. Uses the helpers of the Test Card tests.

constexpr auto kFourColorNode = document::NodeId::fromRaw(41);

[[nodiscard]] runtime::SnapshotCompileResult
compileFourColor(const std::map<std::string, document::ParameterValue>& values,
                 const bool muted = false) {
    auto project = testCardProject(16, 12);
    addFieldNode(project, kFourColorNode, document::kFourColorGradientNodeType, values);
    feedLayer(project, kFourColorNode, document::kFourColorGradientImagePort);
    if (muted)
        project.findComposition(kCompositionId)->nodeLayout()[kFourColorNode].muted = true;
    require(project.validate().ok(), "a Four-Color Gradient is valid document truth");
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "four color built-ins");
    registry.freeze();
    return compile(std::move(project), registry);
}

// The pixel centre (x, y) of the 16 x 12 frame in field units: pixels from the centre, y up.
[[nodiscard]] std::array<double, 2> fourColorField(const std::int64_t x, const std::int64_t y) {
    return {static_cast<double>(x) + 0.5 - 8.0, 6.0 - (static_cast<double>(y) + 0.5)};
}

// The straight color the node defines at a field position, in double precision.
[[nodiscard]] std::array<double, 4>
fourColorReference(const std::array<document::Vec2d, 4>& points,
                   const std::array<std::array<double, 4>, 4>& colors, const double blend,
                   const std::array<double, 2> at) {
    const double squared = 16.0 * 16.0 + 12.0 * 12.0;
    double total = 0.0;
    std::array<double, 4> sum{};
    for (std::size_t i = 0; i < 4; ++i) {
        const double dx = at[0] - points[i].x;
        const double dy = at[1] - points[i].y;
        const double q = (dx * dx + dy * dy + 0.25) / squared;
        const double w = std::pow(q, -blend / 2.0);
        total += w;
        for (std::size_t c = 0; c < 4; ++c)
            sum[c] += w * colors[i][c];
    }
    for (auto& c : sum)
        c /= total;
    return sum;
}

void testFourColorGradientLowering(Expectations& expectations) {
    using namespace document;
    const std::array<Vec2d, 4> points{Vec2d{-5.5, 3.5}, Vec2d{5.5, 3.5}, Vec2d{-5.5, -3.5},
                                      Vec2d{5.5, -3.5}};
    const std::array<std::array<double, 4>, 4> colors{
        {{1.0, 0.0, 0.0, 1.0}, {0.0, 1.0, 0.0, 1.0}, {0.0, 0.0, 1.0, 1.0}, {1.0, 1.0, 0.0, 1.0}}};
    const auto compiled = compileFourColor(
        {{"point1", points[0]}, {"point2", points[1]}, {"point3", points[2]}, {"point4", points[3]},
         {"color1", core::Color4d{1.0, 0.0, 0.0, 1.0}}, {"color2", core::Color4d{0.0, 1.0, 0.0, 1.0}},
         {"color3", core::Color4d{0.0, 0.0, 1.0, 1.0}}, {"color4", core::Color4d{1.0, 1.0, 0.0, 1.0}},
         {"blend", 2.0}});
    expectations.expect(compiled.plan != nullptr, "a Four-Color Gradient compiles");
    if (!compiled.plan)
        return;
    const auto& plan = *compiled.plan;
    const auto ops = recipeOperations(plan, kFourColorNode);
    expectations.expect(ops.size() == 1 &&
                            recipeKernel<runtime::FieldRasterKernel>(plan, ops[0]) != nullptr,
                        "a Four-Color Gradient is one field raster");
    const auto* raster =
        ops.empty() ? nullptr : recipeKernel<runtime::FieldRasterKernel>(plan, ops[0]);
    if (raster != nullptr) {
        const auto sequence = [](const runtime::FieldProgram& program) {
            std::vector<std::pair<std::uint8_t, std::vector<std::int64_t>>> out;
            for (const auto& instruction : program.instructions)
                out.emplace_back(instruction.op, instruction.choices);
            return out;
        };
        expectations.expect(sequence(raster->program) ==
                                sequence(field_test_programs::fourColorGradientProgram(
                                    1, 2.0, points, colors)),
                            "the coverage fixture's program is the lowering's");
        expectations.expect(raster->program.kind ==
                                static_cast<std::uint8_t>(core::field::FieldResultKind::Premultiplied),
                            "the raster is premultiplied");
    }
    // A muted source draws nothing.
    {
        const auto muted = compileFourColor({}, true);
        expectations.expect(muted.plan != nullptr, "a muted Four-Color Gradient compiles");
        const auto evaluated = evaluateTestCard(muted);
        expectations.expect(evaluated.frame() != nullptr &&
                                std::ranges::all_of(evaluated.frame()->processImage().pixels(),
                                                    [](const auto& p) {
                                                        return p == render::Rgba32f::transparent();
                                                    }),
                            "a muted Four-Color Gradient draws nothing");
    }
}

void testFourColorGradientPixels(Expectations& expectations) {
    using namespace document;
    // The pixel centres (2, 2), (13, 2), (2, 9) and (13, 9) as field positions.
    const std::array<Vec2d, 4> points{Vec2d{-5.5, 3.5}, Vec2d{5.5, 3.5}, Vec2d{-5.5, -3.5},
                                      Vec2d{5.5, -3.5}};
    const std::array<std::array<double, 4>, 4> colors{
        {{1.0, 0.0, 0.0, 1.0}, {0.0, 1.0, 0.0, 1.0}, {0.0, 0.0, 1.0, 1.0}, {1.0, 1.0, 0.0, 1.0}}};
    const auto values = [&](const double blend, const double alpha) {
        std::map<std::string, ParameterValue> out{
            {"point1", points[0]}, {"point2", points[1]}, {"point3", points[2]},
            {"point4", points[3]},
            {"color1", core::Color4d{colors[0][0], colors[0][1], colors[0][2], alpha}},
            {"color2", core::Color4d{colors[1][0], colors[1][1], colors[1][2], alpha}},
            {"color3", core::Color4d{colors[2][0], colors[2][1], colors[2][2], alpha}},
            {"color4", core::Color4d{colors[3][0], colors[3][1], colors[3][2], alpha}},
            {"blend", blend}};
        return out;
    };
    // At a point the sharpest weighting returns that point's color.
    {
        const auto evaluated = evaluateTestCard(compileFourColor(values(8.0, 1.0)));
        expectations.expect(evaluated.frame() != nullptr, "a Four-Color Gradient evaluates");
        if (evaluated.frame()) {
            const auto& image = evaluated.frame()->processImage();
            const std::array<std::pair<std::int64_t, std::int64_t>, 4> pixels{
                {{2, 2}, {13, 2}, {2, 9}, {13, 9}}};
            bool exact = true;
            for (std::size_t i = 0; i < 4; ++i) {
                const auto p = cardPixel(image, pixels[i].first, pixels[i].second);
                for (std::size_t c = 0; c < 4; ++c)
                    exact = exact && std::abs(static_cast<double>(p[c]) - colors[i][c]) < 1.0e-3;
            }
            expectations.expect(exact, "at its point the gradient is that point's color");
        }
    }
    // Every pixel against the independent reading, at two blends (and the picture premultiplied at
    // partial alpha).
    for (const double blend : {1.0, 2.5}) {
        const double alpha = blend < 2.0 ? 1.0 : 0.5;
        const auto evaluated = evaluateTestCard(compileFourColor(values(blend, alpha)));
        expectations.expect(evaluated.frame() != nullptr, "a blend evaluates");
        if (!evaluated.frame())
            continue;
        const auto& image = evaluated.frame()->processImage();
        double worst = 0.0;
        for (std::int64_t y = 0; y < 12; ++y)
            for (std::int64_t x = 0; x < 16; ++x) {
                auto expected = fourColorReference(points, colors, blend, fourColorField(x, y));
                // The alpha of every color is `alpha`; the raster premultiplies once.
                expected[3] = alpha;
                const auto p = cardPixel(image, x, y);
                for (std::size_t c = 0; c < 3; ++c)
                    worst = std::max(worst, std::abs(static_cast<double>(p[c]) - expected[c] * alpha));
                worst = std::max(worst, std::abs(static_cast<double>(p[3]) - alpha));
            }
        expectations.expect(worst < 2.0e-4,
                            "the gradient is the inverse distance weighting at every pixel");
    }
    // A symmetric layout gives a picture symmetric about the vertical axis for matching colors.
    {
        auto layout = values(2.0, 1.0);
        layout["color2"] = layout["color1"];
        layout["color4"] = layout["color3"];
        const auto evaluated = evaluateTestCard(compileFourColor(layout));
        expectations.expect(evaluated.frame() != nullptr, "a symmetric layout evaluates");
        if (evaluated.frame()) {
            const auto& image = evaluated.frame()->processImage();
            bool mirrored = true;
            for (std::int64_t y = 0; y < 12; ++y)
                for (std::int64_t x = 0; x < 8; ++x) {
                    const auto a = cardPixel(image, x, y);
                    const auto b = cardPixel(image, 15 - x, y);
                    for (std::size_t c = 0; c < 4; ++c)
                        mirrored = mirrored && std::abs(a[c] - b[c]) < 1.0e-4F;
                }
            expectations.expect(mirrored, "mirrored points and colors give a mirrored picture");
        }
    }
    // A reduced render scale keeps the picture's field positions in composition units.
    {
        const auto compiled = compileFourColor(values(2.0, 1.0));
        const auto evaluated = evaluateTestCard(compiled, 8, 6);
        expectations.expect(evaluated.frame() != nullptr, "a proxy evaluates");
        if (evaluated.frame()) {
            const auto& image = evaluated.frame()->processImage();
            const auto p = cardPixel(image, 1, 1);
            // The proxy pixel (1, 1) covers the full-size pixels 2..3: its center is the field
            // position (-5, 3).
            const auto expected = fourColorReference(points, colors, 2.0, {-5.0, 3.0});
            expectations.expect(std::abs(static_cast<double>(p[0]) - expected[0]) < 2.0e-4 &&
                                    std::abs(static_cast<double>(p[1]) - expected[1]) < 2.0e-4 &&
                                    std::abs(static_cast<double>(p[2]) - expected[2]) < 2.0e-4,
                                "a proxy samples the same field positions");
        }
    }
    // Cancellation stops the raster.
    {
        const auto compiled = compileFourColor(values(2.0, 1.0));
        runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
        const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                       .id = runtime::TaskOwnerId::fromRaw(5241)};
        bool cancelledOutcome = false;
        auto submission = scheduler.submit<void>(
            runtime::TaskRequest("Four-Color Gradient cancellation", owner),
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
        expectations.expect(taken && cancelledOutcome, "a cancelled gradient publishes no image");
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        expectations.expect(scheduler.isQuiescent(), "the cancelled gradient shuts down safely");
    }
    // A budget below the frame's pixels is refused, not drawn.
    {
        const auto evaluated = evaluateTestCard(compileFourColor(values(2.0, 1.0)), 0, 0, 64U);
        expectations.expect(evaluated.frame() == nullptr, "a gradient over the budget is refused");
    }
}
