// Included inside the snapshot compiler test's anonymous namespace after the Tile & Mirror tests,
// whose fixture and sampling helpers it reuses.
//
// #510 Procedural Warp: each mode lowers to one field raster of the Procedural Warp instruction. The
// tests prove the lowering (which operations, which program, the canvas, time dependence, mute, the
// identity at zero strength, the values a mode does not read), then evaluate the plans on the CPU
// reference over the HDR, partly transparent checker against the independent binary64 reading of
// each mode's map (src/core/tests/procedural_warp_reference.hpp, included by the test's main file):
// every mode, with the frame's
// region of interest, the render scale, the edge rule, the pixel budget and cancellation; then the
// cache identity of every value each mode reads.

[[nodiscard]] LensFixture
proceduralWarpFixture(const std::unordered_map<std::string, document::ParameterValue>& values = {}) {
    return warpFixture(document::kProceduralWarpNodeType, values);
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
proceduralWarpPlan(const std::unordered_map<std::string, document::ParameterValue>& values) {
    auto fixture = proceduralWarpFixture(values);
    auto result = compileRecipe(std::move(fixture.recipe.project));
    require(result.plan != nullptr, "the Procedural Warp plan compiles");
    return result.plan;
}

// The parameters of the field operation the node's values are, as the documentation states them:
// the centre authored in pixels from the frame's centre with y down, percentages, the frame's size.
[[nodiscard]] core::field::ProceduralWarpParameters
proceduralWarpReading(const std::unordered_map<std::string, document::ParameterValue>& values) {
    using namespace document;
    const auto number = [&](const std::string& role, const double fallback) {
        const auto found = values.find(role);
        return found == values.end() ? fallback : std::get<double>(found->second);
    };
    const auto choice = [&](const std::string& role) {
        const auto found = values.find(role);
        return found == values.end() ? std::int64_t{0} : std::get<std::int64_t>(found->second);
    };
    core::field::ProceduralWarpParameters p;
    p.mode = static_cast<core::field::ProceduralWarpMode>(choice("mode"));
    const auto centre = values.contains("center") ? std::get<Vec2d>(values.at("center")) : Vec2d{};
    p.centreX = centre.x;
    p.centreY = -centre.y;
    p.radius = number("radius", 300.0);
    p.angleDegrees = number("angle", 90.0);
    p.falloff = number("falloff", 100.0) / 100.0;
    p.amount = number("amount", 50.0) / 100.0;
    p.refraction = number("refraction", 1.5);
    p.wavelength = number("wavelength", 40.0);
    p.amplitude = number("amplitude", 10.0);
    p.phaseDegrees = number("phase", 0.0);
    p.speed = number("speed", 0.0);
    p.directionDegrees = number("direction", 90.0);
    p.wave = static_cast<core::field::ProceduralWarpWave>(choice("waveShape"));
    constexpr std::array<std::uint8_t, 8> masks{0, 15, 1, 2, 4, 8, 3, 12};
    p.pinnedEdges = masks[static_cast<std::size_t>(choice("pinning"))];
    p.frameWidth = 1920.0;
    p.frameHeight = 1080.0;
    p.polarToRect = choice("polarDirection") == 1;
    p.interpolation = number("interpolation", 100.0) / 100.0;
    p.squareShape = choice("magnifyShape") == 1;
    p.magnification = number("magnification", 200.0) / 100.0;
    p.feather = number("feather", 0.0);
    if (choice("magnifyLink") >= 1)
        p.radius *= p.magnification;
    if (choice("magnifyLink") >= 2)
        p.feather *= p.magnification;
    return p;
}

void testProceduralWarpLowering(Expectations& expectations) {
    using namespace document;
    using core::field::FieldOp;
    const auto lowered = [&](const std::unordered_map<std::string, ParameterValue>& values,
                             const std::vector<FieldOp>& expected, const std::string& what,
                             const runtime::FieldProgram& fixtureProgram) {
        auto fixture = proceduralWarpFixture(values);
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr, what + " compiles");
        if (!result.plan)
            return;
        const auto ops = recipeOperations(*result.plan, node);
        require(ops.size() == 1, "Procedural Warp lowers to one raster");
        const auto* raster = lensRaster(*result.plan, ops[0]);
        require(raster != nullptr, "the raster is a field raster");
        const auto& effect = std::get<runtime::CompiledImageEffect>(result.plan->operations()[ops[0]]);
        const auto checker = lensCheckerOperation(*result.plan);
        expectations.expect(checker && effect.fieldImages.size() == 1 &&
                                effect.fieldImages[0].value() == *checker &&
                                effect.input.value() == *checker,
                            what + " reads the picture over the input's own window");
        std::vector<FieldOp> sequence;
        for (const auto& instruction : raster->program.instructions)
            sequence.push_back(static_cast<FieldOp>(instruction.op));
        expectations.expect(sequence == expected, what + " is the documented program");
        expectations.expect(raster->samples == 1 &&
                                raster->program.kind ==
                                    static_cast<std::uint8_t>(core::field::FieldResultKind::Premultiplied),
                            what + " is one premultiplied raster");
        expectations.expect(!result.plan->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(ops[0])),
                            what + " with constant values does not vary with time");
        expectations.expect(lensSequence(raster->program) == lensSequence(fixtureProgram),
                            what + ": the coverage fixture's program is the lowering's");
    };
    const std::vector<FieldOp> plain{FieldOp::Coordinate, FieldOp::ProceduralWarp, FieldOp::Image};
    const std::array<const char*, 7> names{"Twirl", "Bulge / Pinch", "Spherize", "Ripple",
                                           "Wave",  "Polar",         "Magnify"};
    for (std::int64_t mode = 0; mode <= kProceduralWarpLastMode; ++mode) {
        const auto program =
            field_test_programs::proceduralWarpProgram(1, mode, 0, 1, 0, 1920, 1080);
        lowered({{"mode", mode}},
                mode == kProceduralWarpMagnify
                    ? std::vector<FieldOp>{FieldOp::Coordinate, FieldOp::ProceduralWarp,
                                           FieldOp::Image, FieldOp::Image, FieldOp::Swizzle,
                                           FieldOp::Math}
                    : plain,
                names[static_cast<std::size_t>(mode)], program);
    }
    // A mode's own choices reach the instruction: the wave, the pinned edges as a mask (Left and
    // Right is 1 | 2), the polar direction, the shape and the link.
    {
        auto fixture = proceduralWarpFixture({{"mode", std::int64_t{4}},
                                              {"waveShape", std::int64_t{2}},
                                              {"pinning", std::int64_t{6}}});
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        const auto ops = result.plan ? recipeOperations(*result.plan, node) : std::vector<std::size_t>{};
        const auto* raster = ops.size() == 1 ? lensRaster(*result.plan, ops[0]) : nullptr;
        expectations.expect(raster != nullptr &&
                                raster->program.instructions[1].choices ==
                                    std::vector<std::int64_t>{4, 2, 3, 0, 0, 0, 1920, 1080},
                            "a Wave's shape and pinned edges reach the instruction");
    }
    {
        auto fixture = proceduralWarpFixture({{"mode", std::int64_t{6}},
                                              {"magnifyShape", std::int64_t{1}},
                                              {"magnifyLink", std::int64_t{2}},
                                              {"waveShape", std::int64_t{3}}});
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        const auto ops = result.plan ? recipeOperations(*result.plan, node) : std::vector<std::size_t>{};
        const auto* raster = ops.size() == 1 ? lensRaster(*result.plan, ops[0]) : nullptr;
        expectations.expect(raster != nullptr &&
                                raster->program.instructions[1].choices ==
                                    std::vector<std::int64_t>{6, 0, 0, 0, 1, 2, 1920, 1080},
                            "a Magnify's shape and link reach the instruction, another mode's "
                            "choice does not");
    }

    // A muted node, a constant zero strength and a constant radius of 0 are the input itself.
    const auto passes = [&](const std::unordered_map<std::string, ParameterValue>& values,
                            const bool muted = false) {
        auto fixture = proceduralWarpFixture(values);
        if (muted)
            fixture.recipe.project.findComposition(kCompositionId)
                ->nodeLayout()[fixture.recipe.node]
                .muted = true;
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        return result.plan != nullptr && recipeOperations(*result.plan, node).empty();
    };
    expectations.expect(passes({}, true), "a muted Procedural Warp passes its Image through");
    expectations.expect(passes({{"angle", 0.0}}), "a Twirl of 0 degrees passes through");
    expectations.expect(passes({{"mode", std::int64_t{1}}, {"amount", 0.0}}),
                        "a Bulge of 0 passes through");
    expectations.expect(passes({{"mode", std::int64_t{2}}, {"amount", 0.0}}),
                        "a Spherize of 0 passes through");
    expectations.expect(passes({{"mode", std::int64_t{3}}, {"amplitude", 0.0}}),
                        "a Ripple of no amplitude passes through");
    expectations.expect(passes({{"mode", std::int64_t{4}}, {"amplitude", 0.0}}),
                        "a Wave of no amplitude passes through");
    expectations.expect(passes({{"mode", std::int64_t{5}}, {"interpolation", 0.0}}),
                        "a Polar conversion of 0% passes through");
    expectations.expect(passes({{"mode", std::int64_t{6}}, {"magnification", 100.0}}),
                        "a Magnify of 100% passes through");
    expectations.expect(passes({{"radius", 0.0}}), "a radius of 0 passes through");
    expectations.expect(!passes({{"mode", std::int64_t{4}}, {"angle", 0.0}}),
                        "another mode's zero does not pass a Wave through");

    // A keyed value of the mode makes the raster vary with time; a keyed value another mode reads is
    // not compiled (it would be a curve nothing references) and changes nothing. A speed that is not
    // zero advances the phase with the clock.
    const auto timed = [&](const std::unordered_map<std::string, ParameterValue>& values,
                           const std::string_view keyedRole, const bool vector,
                           const double from = 10.0, const double to = 50.0) {
        auto fixture = proceduralWarpFixture(values);
        auto* composition = fixture.recipe.project.findComposition(kCompositionId);
        if (!keyedRole.empty()) {
            constexpr auto curveId = AnimationCurveId::fromRaw(1750);
            if (vector) {
                Vec2AnimationCurve curve{curveId};
                curve.components[0].keyframes = {
                    {KeyframeId::fromRaw(1751), core::RationalTime::fromInteger(0), 0.0,
                     KeyframeInterpolation::Linear},
                    {KeyframeId::fromRaw(1752), core::RationalTime::fromInteger(1), 40.0,
                     KeyframeInterpolation::Linear}};
                require(composition->animationCurves().insert(std::move(curve)),
                        "the curve is publishable");
            } else {
                ScalarAnimationCurve curve{curveId, {}};
                curve.keyframes = {{KeyframeId::fromRaw(1751), core::RationalTime::fromInteger(0),
                                    from, KeyframeInterpolation::Linear},
                                   {KeyframeId::fromRaw(1752), core::RationalTime::fromInteger(1),
                                    to, KeyframeInterpolation::Linear}};
                require(composition->animationCurves().insert(std::move(curve)),
                        "the curve is publishable");
            }
            require(composition->parameters().setSource(
                        fixture.recipe.parameters.at(std::string(keyedRole)),
                        AnimationCurveSource{curveId}),
                    "the value animates");
        }
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "a keyed Procedural Warp compiles");
        const auto ops = recipeOperations(*result.plan, node);
        require(ops.size() == 1, "a keyed Procedural Warp keeps its raster");
        return result.plan->operationTimeDependent(runtime::OperationIndex::fromRaw(ops[0]));
    };
    expectations.expect(timed({}, "angle", false), "a keyed twirl angle varies with time");
    expectations.expect(timed({}, "radius", false), "a keyed radius varies with time");
    expectations.expect(timed({}, "center", true), "a keyed centre varies with time");
    expectations.expect(timed({{"mode", std::int64_t{1}}}, "amount", false),
                        "a keyed bulge amount varies with time");
    expectations.expect(timed({{"mode", std::int64_t{2}}}, "refraction", false, 1.2, 2.0),
                        "a keyed refraction index varies with time");
    expectations.expect(timed({{"mode", std::int64_t{3}}}, "phase", false),
                        "a keyed ripple phase varies with time");
    expectations.expect(timed({{"mode", std::int64_t{4}}}, "direction", false),
                        "a keyed wave direction varies with time");
    expectations.expect(timed({{"mode", std::int64_t{5}}}, "interpolation", false),
                        "a keyed polar interpolation varies with time");
    expectations.expect(timed({{"mode", std::int64_t{6}}}, "magnification", false),
                        "a keyed magnification varies with time");
    expectations.expect(timed({{"mode", std::int64_t{3}}, {"speed", 0.5}}, "", false),
                        "a ripple with a speed varies with time");
    expectations.expect(timed({{"mode", std::int64_t{4}}, {"speed", -1.0}}, "", false),
                        "a wave with a speed varies with time");
    expectations.expect(!timed({{"speed", 2.0}}, "", false),
                        "a speed does not move a Twirl, which does not read it");
    expectations.expect(!timed({{"mode", std::int64_t{4}}}, "angle", false),
                        "a keyed twirl angle does not make a Wave vary with time");
    expectations.expect(!timed({{"mode", std::int64_t{5}}}, "center", true),
                        "a keyed centre does not move a Polar conversion");
    expectations.expect(!timed({{"mode", std::int64_t{0}}}, "magnification", false),
                        "a keyed magnification does not move a Twirl");
}

void testProceduralWarpPixels(Expectations& expectations) {
    using namespace document;
    namespace reference = bloom::procedural_warp_reference;
    constexpr std::uint32_t kW = 240;
    constexpr std::uint32_t kH = 135;
    const auto bare = warpBarePlan();
    using Values = std::unordered_map<std::string, ParameterValue>;
    const auto evaluate = [&](const Values& values, const std::uint32_t w = kW,
                              const std::uint32_t h = kH) {
        auto result = lensEvaluate(proceduralWarpPlan(values), w, h);
        require(result.frame() != nullptr, "the Procedural Warp plan evaluates");
        return result;
    };
    // Every pixel of the node's frame against the reading: the input read nearest at the position the
    // map names (Magnify: the plain picture with the magnified one blended over it by the weight).
    // Pixels whose position falls on a texel's edge, or on a step the map has by definition, may
    // read the neighbouring texel in binary32; at most `share` of them in 1000.
    const auto check = [&](const Values& values, const std::string& what,
                           const std::uint32_t scale = 1, const std::size_t share = 10) {
        const auto w = kW * scale;
        const auto h = kH * scale;
        Values nearest = values;
        nearest["filter"] = std::int64_t{1};
        const auto warped = evaluate(nearest, w, h);
        const auto plainFrame = lensEvaluate(bare, w, h);
        require(plainFrame.frame() != nullptr, "the bare checker evaluates");
        const auto& source = plainFrame.frame()->processImage();
        const auto parameters = proceduralWarpReading(values);
        // The node's edge choice (Transparent, Extend, Repeat, Mirror) as warpNearest's rule.
        constexpr std::array<int, 4> edges{1, 0, 2, 3};
        const auto edge = values.contains("edges")
                              ? edges[static_cast<std::size_t>(std::get<std::int64_t>(values.at("edges")))]
                              : 1;
        std::size_t bad = 0;
        for (std::uint32_t j = 0; j < h; ++j)
            for (std::uint32_t i = 0; i < w; ++i) {
                const auto x = (i + 0.5) * kTileWidth / w - kTileWidth / 2.0;
                const auto y = kTileHeight / 2.0 - (j + 0.5) * kTileHeight / h;
                const auto at = reference::reference(parameters, x, y);
                auto want = warpNearest(source, kTileWidth, kTileHeight, at.x, at.y, edge);
                if (parameters.mode == core::field::ProceduralWarpMode::Magnify) {
                    const auto plain = warpNearest(source, kTileWidth, kTileHeight, x, y, edge);
                    for (std::size_t c = 0; c < 4; ++c)
                        want[c] = plain[c] + (want[c] - plain[c]) * at.weight;
                }
                const auto got = lensPixel(warped.frame()->processImage(), i, j);
                double here = 0.0;
                for (std::size_t c = 0; c < 4; ++c)
                    here = std::max(here, std::abs(static_cast<double>(got[c]) - want[c]));
                bad += here > 1.0e-4 ? 1U : 0U;
            }
        expectations.expect(bad * 1000 <= share * static_cast<std::size_t>(w) * h,
                            what + " reads the documented position (" + std::to_string(bad) +
                                " of " + std::to_string(w * h) + " pixels on a texel's edge)");
        return warped;
    };

    // The default node (Twirl 90 degrees, radius 300 about the frame's centre) leaves the frame
    // outside the radius as it is.
    {
        const auto twirled = check({}, "the default Twirl");
        warpDump("frame-procedural-warp-twirl", twirled.frame()->processImage(), kW, kH);
    }
    const Vec2d centre{96.0, -40.0};
    check({{"angle", -200.0}, {"center", centre}, {"radius", 420.0}, {"falloff", 60.0}},
          "a Twirl about a moved centre");
    for (const double amount : {80.0, -80.0}) {
        const auto bulged = check({{"mode", std::int64_t{1}}, {"amount", amount},
                                   {"center", centre}, {"radius", 380.0}, {"falloff", 30.0}},
                                  amount > 0.0 ? "a Bulge" : "a Pinch");
        if (amount > 0.0)
            warpDump("frame-procedural-warp-bulge", bulged.frame()->processImage(), kW, kH);
    }
    for (const double amount : {100.0, -70.0}) {
        const auto sphere = check({{"mode", std::int64_t{2}}, {"amount", amount},
                                   {"refraction", 1.8}, {"center", centre}, {"radius", 400.0}},
                                  amount > 0.0 ? "a Spherize" : "an inverse Spherize");
        if (amount > 0.0)
            warpDump("frame-procedural-warp-spherize", sphere.frame()->processImage(), kW, kH);
    }
    {
        const auto ripple = check({{"mode", std::int64_t{3}}, {"radius", 500.0},
                                   {"wavelength", 70.0}, {"amplitude", 12.0}, {"phase", 45.0},
                                   {"center", centre}},
                                  "a Ripple");
        warpDump("frame-procedural-warp-ripple", ripple.frame()->processImage(), kW, kH);
    }
    for (std::int64_t shape = 0; shape < 4; ++shape) {
        const auto wave = check({{"mode", std::int64_t{4}}, {"waveShape", shape},
                                 {"wavelength", 160.0}, {"amplitude", 24.0}, {"direction", 70.0},
                                 {"phase", 30.0}, {"pinning", std::int64_t{shape % 2 == 0 ? 1 : 6}}},
                                "a Wave of shape " + std::to_string(shape));
        if (shape == 0)
            warpDump("frame-procedural-warp-wave", wave.frame()->processImage(), kW, kH);
    }
    for (const std::int64_t back : {0, 1}) {
        const auto polar = check({{"mode", std::int64_t{5}}, {"polarDirection", back}},
                                 back == 0 ? "Rectangular to Polar" : "Polar to Rectangular", 1,
                                 back == 0 ? 25 : 10);
        warpDump(back == 0 ? "frame-procedural-warp-rect-to-polar"
                           : "frame-procedural-warp-polar-to-rect",
                 polar.frame()->processImage(), kW, kH);
    }
    check({{"mode", std::int64_t{5}}, {"interpolation", 40.0}}, "a part Polar conversion", 1, 25);
    for (const std::int64_t shape : {0, 1}) {
        const auto magnified = check({{"mode", std::int64_t{6}}, {"magnifyShape", shape},
                                      {"magnification", 250.0}, {"radius", 240.0},
                                      {"feather", 80.0}, {"center", centre}},
                                     shape == 0 ? "a feathered round Magnify"
                                                : "a feathered square Magnify");
        if (shape == 0)
            warpDump("frame-procedural-warp-magnify", magnified.frame()->processImage(), kW, kH);
    }
    check({{"mode", std::int64_t{6}}, {"magnifyLink", std::int64_t{2}}, {"radius", 100.0},
           {"feather", 20.0}},
          "a Magnify whose size and feather grow with the magnification");

    // The edge rule: a Wave reaching past the frame reads nothing there with Transparent and the
    // edge's own texels with Extend (the reading holds both to the rule).
    check({{"mode", std::int64_t{4}}, {"amplitude", 60.0}, {"wavelength", 300.0},
           {"direction", 0.0}, {"edges", std::int64_t{0}}},
          "a Wave past the frame, Transparent");
    check({{"mode", std::int64_t{4}}, {"amplitude", 60.0}, {"wavelength", 300.0},
           {"direction", 0.0}, {"edges", std::int64_t{1}}},
          "a Wave past the frame, Extend");

    // The render scale: twice the proxy reads the same map (positions are full-frame pixels).
    check({{"angle", 150.0}, {"center", centre}}, "a Twirl at twice the scale", 2);
    check({{"mode", std::int64_t{3}}, {"radius", 500.0}, {"wavelength", 70.0},
           {"amplitude", 12.0}},
          "a Ripple at twice the scale", 2);

    // A region of interest reads the whole input (a pixel of the region may read any of it): its
    // pixels are the whole frame's. The pixel budget refuses; a cancelled raster publishes nothing.
    {
        const auto plan = proceduralWarpPlan({{"mode", std::int64_t{2}}, {"amount", 100.0},
                                              {"radius", 700.0}, {"refraction", 2.5}});
        const auto whole = lensEvaluate(plan, kW, kH);
        const auto region = render::ImageWindow::create(140, 60, 50, 40);
        const auto part = lensEvaluate(plan, kW, kH, *region.value());
        expectations.expect(whole.frame() != nullptr && part.frame() != nullptr,
                            "a region of interest evaluates");
        if (whole.frame() && part.frame()) {
            bool same = true;
            for (auto y = region.value()->originY(); y < region.value()->maxYExclusive(); ++y)
                for (auto x = region.value()->originX(); x < region.value()->maxXExclusive(); ++x)
                    same = same && lensPixel(part.frame()->processImage(), x, y) ==
                                       lensPixel(whole.frame()->processImage(), x, y);
            expectations.expect(same, "a region's pixels are the whole frame's (it reads the "
                                      "whole input)");
        }
        const auto refused = lensEvaluate(plan, kW, kH, std::nullopt, 64);
        expectations.expect(refused.frame() == nullptr, "the pixel budget refuses the raster");
        runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
        const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                       .id = runtime::TaskOwnerId::fromRaw(5100)};
        bool cancelledOutcome = false;
        auto submission = scheduler.submit<void>(
            runtime::TaskRequest("Procedural Warp cancellation", owner),
            [&](runtime::TaskContext& context) {
                static_cast<void>(scheduler.cancelOwner(owner));
                const auto cancelled = lensEvaluate(plan, kW, kH, std::nullopt,
                                                    std::size_t{1} << 28U, context.cancellation());
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
        expectations.expect(taken && cancelledOutcome,
                            "a cancelled Procedural Warp publishes no image");
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        expectations.expect(scheduler.isQuiescent(),
                            "the cancelled Procedural Warp shuts down safely");
    }
    // The output's data window is the input's.
    {
        const auto warped = evaluate({{"mode", std::int64_t{4}}});
        const auto plainFrame = lensEvaluate(bare, kW, kH);
        expectations.expect(plainFrame.frame() != nullptr &&
                                warped.frame()->processImage().descriptor()->dataWindow() ==
                                    plainFrame.frame()->processImage().descriptor()->dataWindow(),
                            "the output's data window is the input's");
    }
}

// Cache identity: with one evaluator and its operation cache, the same plan twice is a miss then a
// hit; each value the mode reads, the mode, the filter, the edges, the resolution and the picture's
// own identity make the node miss (the picture upstream still hits); a value the mode does not read
// does not.
void testProceduralWarpCacheIdentity(Expectations& expectations) {
    using namespace document;
    runtime::CpuCompositionEvaluator evaluator;
    const auto node = NodeId::fromRaw(1300);
    const auto run = [&](const std::unordered_map<std::string, ParameterValue>& values,
                         const std::uint32_t w = 240, const std::uint32_t h = 135,
                         const std::optional<render::ImageWindow> roi = std::nullopt,
                         const bool recolour = false) {
        auto fixture = proceduralWarpFixture(values);
        if (recolour) {
            const auto* definition =
                builtInNodeDefinitions().find(kCheckerSourceNodeType, kCheckerSourceNodeSchemaVersion);
            std::uint64_t next = 1380;
            for (const auto& parameter : definition->parameters) {
                const auto id = ParameterId::fromRaw(next++);
                if (parameter.role == "colorB")
                    require(fixture.recipe.project.findComposition(kCompositionId)
                                ->parameters()
                                .setSource(id, ConstantValueSource{core::Color4d{0.4, 0.3, 0.2, 1.0}}),
                            "the picture's colour changes");
            }
        }
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the plan compiles");
        const auto extent = render::ImageExtent::create(w, h);
        runtime::EvaluationRequest request{core::RationalTime::fromInteger(0), result.plan->output(),
                                           runtime::ProxyResolution{*extent.value()}};
        request.roi = roi;
        request.pixelStorageByteLimit = std::size_t{1} << 28U;
        runtime::OperationCacheStatistics stats;
        const auto evaluated = evaluator.evaluate(result.plan, request, {}, {}, nullptr, &stats);
        require(evaluated.frame() != nullptr, "the plan evaluates with the cache");
        return stats;
    };
    const auto evaluatedNode = [&](const runtime::OperationCacheStatistics& stats) {
        return std::ranges::find(stats.evaluatedNodes, node) != stats.evaluatedNodes.end();
    };
    using Values = std::unordered_map<std::string, ParameterValue>;
    struct Mode final {
        const char* name;
        Values base;
        std::vector<std::pair<std::string, ParameterValue>> misses;
        std::vector<std::pair<std::string, ParameterValue>> hits;
    };
    const std::vector<std::pair<std::string, ParameterValue>> common{
        {"filter", std::int64_t{1}}, {"edges", std::int64_t{2}}};
    std::vector<Mode> modes{
        // A radius no other base has: the Twirl changed to another mode is not that mode's base.
        {"Twirl",
         {{"radius", 310.0}},
         {{"angle", 45.0}, {"radius", 250.0}, {"falloff", 50.0}, {"center", Vec2d{8.0, 0.0}},
          {"mode", std::int64_t{1}}},
         {{"amount", 20.0}, {"wavelength", 55.0}, {"direction", 10.0}, {"speed", 1.0},
          {"magnification", 300.0}, {"waveShape", std::int64_t{2}}}},
        {"Bulge / Pinch",
         {{"mode", std::int64_t{1}}},
         {{"amount", -30.0}, {"radius", 250.0}, {"falloff", 50.0}, {"center", Vec2d{8.0, 0.0}}},
         {{"angle", 45.0}, {"refraction", 2.0}, {"phase", 30.0}, {"interpolation", 20.0}}},
        {"Spherize",
         {{"mode", std::int64_t{2}}},
         {{"amount", -30.0}, {"radius", 250.0}, {"refraction", 2.0}, {"center", Vec2d{8.0, 0.0}}},
         {{"falloff", 50.0}, {"angle", 45.0}, {"amplitude", 5.0}, {"feather", 3.0}}},
        {"Ripple",
         {{"mode", std::int64_t{3}}},
         {{"radius", 250.0}, {"falloff", 50.0}, {"wavelength", 55.0}, {"amplitude", 5.0},
          {"phase", 30.0}, {"speed", 1.0}, {"center", Vec2d{8.0, 0.0}}},
         {{"angle", 45.0}, {"direction", 10.0}, {"waveShape", std::int64_t{2}},
          {"pinning", std::int64_t{1}}}},
        {"Wave",
         {{"mode", std::int64_t{4}}},
         {{"wavelength", 55.0}, {"amplitude", 5.0}, {"phase", 30.0}, {"speed", 1.0},
          {"direction", 10.0}, {"waveShape", std::int64_t{2}}, {"pinning", std::int64_t{1}}},
         {{"radius", 250.0}, {"center", Vec2d{8.0, 0.0}}, {"falloff", 50.0}, {"angle", 45.0},
          {"polarDirection", std::int64_t{1}}}},
        {"Polar",
         {{"mode", std::int64_t{5}}},
         {{"polarDirection", std::int64_t{1}}, {"interpolation", 20.0}},
         {{"radius", 250.0}, {"center", Vec2d{8.0, 0.0}}, {"amplitude", 5.0},
          {"magnifyShape", std::int64_t{1}}}},
        {"Magnify",
         {{"mode", std::int64_t{6}}},
         {{"radius", 250.0}, {"magnification", 300.0}, {"feather", 3.0},
          {"magnifyShape", std::int64_t{1}}, {"magnifyLink", std::int64_t{1}},
          {"center", Vec2d{8.0, 0.0}}},
         {{"angle", 45.0}, {"falloff", 50.0}, {"amount", 20.0}, {"pinning", std::int64_t{3}}}}};
    for (auto& mode : modes)
        mode.misses.insert(mode.misses.end(), common.begin(), common.end());
    for (const auto& mode : modes) {
        const std::string name = mode.name;
        const auto cold = run(mode.base);
        expectations.expect(evaluatedNode(cold) && cold.misses > 0,
                            name + ": a first run evaluates the node");
        const auto warm = run(mode.base);
        expectations.expect(warm.misses == 0 && warm.hits > 0 && !evaluatedNode(warm),
                            name + ": the same plan again is served from the cache");
        for (const auto& [role, value] : mode.misses) {
            auto changed = mode.base;
            changed[role] = value;
            const auto stats = run(changed);
            expectations.expect(evaluatedNode(stats) && stats.hits > 0,
                                name + ": a changed " + role +
                                    " re-evaluates the node and keeps the picture");
        }
        for (const auto& [role, value] : mode.hits) {
            auto changed = mode.base;
            changed[role] = value;
            const auto stats = run(changed);
            expectations.expect(stats.misses == 0 && !evaluatedNode(stats),
                                name + ": " + role + " is not read by this mode and changes no key");
        }
        expectations.expect(evaluatedNode(run(mode.base, 480, 270)),
                            name + ": another resolution re-evaluates the node");
        const auto recoloured = run(mode.base, 240, 135, std::nullopt, true);
        expectations.expect(evaluatedNode(recoloured),
                            name + ": a different picture re-evaluates the node");
    }
}
