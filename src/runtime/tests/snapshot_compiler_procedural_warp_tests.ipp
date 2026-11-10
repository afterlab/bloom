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
    // The modes after Magnify, and Magnify's pull and Ripple's pulse.
    const auto vec = [&](const std::string& role, const Vec2d fallback) {
        return values.contains(role) ? std::get<Vec2d>(values.at(role)) : fallback;
    };
    p.pull = number("pull", 0.0) / 100.0;
    p.pulseRadius = number("pulseRadius", 0.0);
    p.pulseSpeed = number("pulseSpeed", 0.0);
    p.warpStyle = static_cast<std::uint8_t>(choice("warpStyle"));
    p.bend = number("bend", 50.0) / 100.0;
    p.horizontalDistortion = number("horizontalDistortion", 0.0) / 100.0;
    p.verticalDistortion = number("verticalDistortion", 0.0) / 100.0;
    p.height = number("height", 400.0);
    p.splitDistance = number("splitDistance", 40.0);
    p.mirror = values.contains("mirror") && std::get<bool>(values.at("mirror"));
    p.asymmetric = values.contains("asymmetric") && std::get<bool>(values.at("asymmetric"));
    p.oppositeDistance = number("oppositeDistance", 40.0);
    p.displacement = number("displacement", 50.0);
    p.seed = values.contains("seed") ? std::get<std::int64_t>(values.at("seed")) : 1;
    p.drops = values.contains("drops") ? std::get<std::int64_t>(values.at("drops")) : 12;
    // Split's line runs between its two points, Magnify's pull towards its point; Bend's centre is
    // the centre.
    if (p.mode == core::field::ProceduralWarpMode::Split) {
        const auto start = vec("lineStart", Vec2d{0.0, 200.0});
        const auto end = vec("lineEnd", Vec2d{0.0, -200.0});
        p.centreX = start.x;
        p.centreY = -start.y;
        p.secondX = end.x;
        p.secondY = -end.y;
    } else {
        const auto pullPoint = vec("pullPoint", Vec2d{200.0, 0.0});
        p.secondX = pullPoint.x;
        p.secondY = -pullPoint.y;
    }
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
    const std::array<const char*, 12> names{"Twirl", "Bulge / Pinch", "Spherize", "Ripple",
                                            "Wave",  "Polar",         "Magnify",  "Warp",
                                            "Bend",  "Split",         "Turbulent", "Drizzle"};
    for (std::int64_t mode = 0; mode <= kProceduralWarpLastMode; ++mode) {
        const auto program =
            field_test_programs::proceduralWarpProgram(1, mode, 0, 1, 0, 1920, 1080);
        std::vector<FieldOp> expected = plain;
        if (mode == kProceduralWarpMagnify)
            expected = {FieldOp::Coordinate, FieldOp::ProceduralWarp, FieldOp::Image,
                        FieldOp::Image, FieldOp::Swizzle, FieldOp::Math};
        else if (mode == kProceduralWarpSplit)
            expected = {FieldOp::Coordinate, FieldOp::ProceduralWarp, FieldOp::Image,
                        FieldOp::Swizzle, FieldOp::Math};
        lowered({{"mode", mode}}, expected, names[static_cast<std::size_t>(mode)], program);
    }
    // Every Warp style, and Split with its mirror, lowers to the program the fixture holds.
    for (std::int64_t style = 0; style <= kWarpStyleLast; ++style)
        lowered({{"mode", kProceduralWarpWarp}, {"warpStyle", style}}, plain,
                std::string("Warp style ") + std::to_string(style),
                field_test_programs::proceduralWarpProgram(1, kProceduralWarpWarp, 0, 1, style, 1920,
                                                           1080));
    lowered({{"mode", kProceduralWarpSplit}, {"mirror", true}},
            {FieldOp::Coordinate, FieldOp::ProceduralWarp, FieldOp::Image, FieldOp::Swizzle,
             FieldOp::Math},
            "Split with the mirror",
            field_test_programs::proceduralWarpProgram(1, kProceduralWarpSplit, 0, 1, 1, 1920,
                                                       1080));
    lowered({{"mode", kProceduralWarpSplit}, {"asymmetric", true}},
            {FieldOp::Coordinate, FieldOp::ProceduralWarp, FieldOp::Image, FieldOp::Swizzle,
             FieldOp::Math},
            "Split with its halves opened separately",
            field_test_programs::proceduralWarpProgram(1, kProceduralWarpSplit, 0, 1, 4, 1920,
                                                       1080));
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
                                    std::vector<std::int64_t>{4, 2, 3, 0, 0, 0, 1920, 1080, 0, 0, 12, 1, 0},
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
                                    std::vector<std::int64_t>{6, 0, 0, 0, 1, 2, 1920, 1080, 0, 0, 12, 1, 0},
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
    expectations.expect(passes({{"mode", std::int64_t{6}}, {"pull", 30.0}, {"magnification", 100.0}}) == false,
                        "a Magnify of 100% with a pull does not pass through");
    expectations.expect(passes({{"mode", std::int64_t{7}}, {"bend", 0.0}}),
                        "a Warp of no bend and no distortion passes through");
    expectations.expect(!passes({{"mode", std::int64_t{7}}, {"bend", 0.0},
                                 {"horizontalDistortion", 20.0}}),
                        "a Warp with a distortion does not");
    expectations.expect(passes({{"mode", std::int64_t{8}}, {"bend", 0.0}}),
                        "a Bend of 0 passes through");
    expectations.expect(passes({{"mode", std::int64_t{8}}, {"height", 0.0}}),
                        "a Bend of no height passes through");
    expectations.expect(passes({{"mode", std::int64_t{9}}, {"splitDistance", 0.0}}),
                        "a Split of no distance passes through");
    expectations.expect(!passes({{"mode", std::int64_t{9}}, {"splitDistance", 0.0},
                                 {"mirror", true}}),
                        "a mirror with no distance is a reflection, not a pass-through");
    expectations.expect(passes({{"mode", std::int64_t{9}}, {"splitDistance", 0.0},
                                {"asymmetric", true}, {"oppositeDistance", 0.0}}),
                        "an asymmetric Split of no distance on either side passes through");
    expectations.expect(!passes({{"mode", std::int64_t{9}}, {"splitDistance", 0.0},
                                 {"asymmetric", true}, {"oppositeDistance", 30.0}}),
                        "an asymmetric Split with one side open does not");
    expectations.expect(passes({{"mode", std::int64_t{9}}, {"splitDistance", 0.0},
                                {"oppositeDistance", 30.0}}),
                        "the opposite distance is not read while Asymmetric is off");
    expectations.expect(passes({{"mode", std::int64_t{10}}, {"displacement", 0.0}}),
                        "a Turbulent of no displacement passes through");
    expectations.expect(passes({{"mode", std::int64_t{11}}, {"amplitude", 0.0}}),
                        "a Drizzle of no amplitude passes through");
    expectations.expect(passes({{"mode", std::int64_t{11}}, {"radius", 0.0}}),
                        "a Drizzle of no radius passes through");
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
    expectations.expect(timed({{"mode", std::int64_t{3}}, {"pulseSpeed", 30.0}}, "", false),
                        "a ripple with a pulse speed varies with time");
    expectations.expect(timed({{"mode", std::int64_t{3}}}, "pulseRadius", false),
                        "a keyed pulse radius varies with time");
    expectations.expect(timed({{"mode", std::int64_t{6}}}, "pull", false, 0.0, 60.0),
                        "a keyed magnify pull varies with time");
    expectations.expect(timed({{"mode", std::int64_t{6}}}, "pullPoint", true),
                        "a keyed pull point varies with time");
    expectations.expect(timed({{"mode", std::int64_t{7}}}, "bend", false),
                        "a keyed warp bend varies with time");
    expectations.expect(timed({{"mode", std::int64_t{8}}}, "height", false, 200.0, 600.0),
                        "a keyed bend height varies with time");
    expectations.expect(timed({{"mode", std::int64_t{9}}}, "lineEnd", true),
                        "a keyed split line end varies with time");
    expectations.expect(timed({{"mode", std::int64_t{9}}}, "splitDistance", false),
                        "a keyed split distance varies with time");
    expectations.expect(timed({{"mode", std::int64_t{9}}, {"asymmetric", true}},
                             "oppositeDistance", false),
                        "a keyed opposite distance varies with time while Asymmetric is on");
    expectations.expect(!timed({{"mode", std::int64_t{9}}}, "oppositeDistance", false),
                        "a keyed opposite distance does not while Asymmetric is off");
    expectations.expect(timed({{"mode", std::int64_t{10}}}, "evolution", false, 0.0, 2.0),
                        "a keyed turbulence evolution varies with time");
    expectations.expect(timed({{"mode", std::int64_t{10}}}, "displacement", false),
                        "a keyed turbulence displacement varies with time");
    expectations.expect(timed({{"mode", std::int64_t{11}}, {"speed", 0.5}}, "", false),
                        "a drizzle with a speed varies with time");
    expectations.expect(!timed({{"mode", std::int64_t{10}}, {"speed", 0.5}}, "", false),
                        "a speed does not move a Turbulent, which does not read it");
    expectations.expect(!timed({{"mode", std::int64_t{10}}}, "bend", false),
                        "a keyed bend does not make a Turbulent vary with time");
    expectations.expect(!timed({{"speed", 2.0}}, "", false),
                        "a speed does not move a Twirl, which does not read it");
    expectations.expect(!timed({{"mode", std::int64_t{4}}}, "angle", false),
                        "a keyed twirl angle does not make a Wave vary with time");
    expectations.expect(!timed({{"mode", std::int64_t{5}}}, "center", true),
                        "a keyed centre does not move a Polar conversion");
    expectations.expect(!timed({{"mode", std::int64_t{0}}}, "magnification", false),
                        "a keyed magnification does not move a Twirl");
}

// A Split saved before Asymmetric Split has neither value (they are optional). It compiles to the
// plan, and renders the frame, of the same node with Asymmetric off, byte for byte; and equal
// distances with Asymmetric on are the symmetric split too.
void testProceduralWarpSavedBeforeAsymmetricSplit(Expectations& expectations) {
    using namespace document;
    using Values = std::unordered_map<std::string, ParameterValue>;
    const Values split{{"mode", std::int64_t{9}}, {"splitDistance", 120.0},
                       {"lineStart", Vec2d{-100.0, 300.0}}, {"lineEnd", Vec2d{200.0, -250.0}},
                       {"filter", std::int64_t{1}}};
    // The node as the earlier version saved it: the two bindings and their parameters are gone.
    auto earlier = proceduralWarpFixture(split);
    {
        auto* composition = earlier.recipe.project.findComposition(kCompositionId);
        auto* node = composition->graph().findNode(earlier.recipe.node);
        require(node != nullptr, "the earlier Split node exists");
        const auto before = node->parameters.size();
        for (const char* role : {"asymmetric", "oppositeDistance"}) {
            const auto id = earlier.recipe.parameters.at(role);
            std::erase_if(node->parameters,
                          [&](const ParameterBinding& binding) { return binding.parameterId == id; });
            require(composition->parameters().erase(id), "the earlier parameter is removed");
        }
        require(node->parameters.size() + 2 == before, "the earlier node lacks the two bindings");
    }
    const auto earlierResult = compileRecipe(std::move(earlier.recipe.project));
    expectations.expect(earlierResult.plan != nullptr,
                        "a Split saved before Asymmetric Split compiles");
    if (!earlierResult.plan)
        return;
    const auto current = proceduralWarpPlan(split);
    constexpr std::uint32_t kW = 240;
    constexpr std::uint32_t kH = 135;
    const auto expectSame = [&](const std::shared_ptr<const runtime::CompiledCompositionPlan>& a,
                                const std::shared_ptr<const runtime::CompiledCompositionPlan>& b,
                                const std::string& what, const std::uint32_t scale = 1) {
        const auto first = lensEvaluate(a, kW * scale, kH * scale);
        const auto second = lensEvaluate(b, kW * scale, kH * scale);
        expectations.expect(first.frame() != nullptr && second.frame() != nullptr,
                            what + " evaluates");
        if (!first.frame() || !second.frame())
            return;
        std::size_t differing = 0;
        std::size_t gap = 0;
        for (std::uint32_t j = 0; j < kH * scale; ++j)
            for (std::uint32_t i = 0; i < kW * scale; ++i) {
                const auto x = lensPixel(first.frame()->processImage(), i, j);
                const auto y = lensPixel(second.frame()->processImage(), i, j);
                differing += x != y ? 1U : 0U;
                gap += x[3] == 0.0F ? 1U : 0U;
            }
        expectations.expect(differing == 0, what + " is the same frame, bit for bit");
        expectations.expect(gap > 0, what + ": the frame has a gap, so it is not a pass-through");
    };
    expectSame(earlierResult.plan, current, "a Split saved before Asymmetric Split");
    expectSame(earlierResult.plan, current, "a Split saved before Asymmetric Split, at twice the scale", 2);
    // Equal distances with Asymmetric on are the same split.
    Values equal = split;
    equal["asymmetric"] = true;
    equal["oppositeDistance"] = 120.0;
    expectSame(earlierResult.plan, proceduralWarpPlan(equal), "an asymmetric Split of equal distances");
    // And the opposite distance is not read while Asymmetric is off.
    Values ignored = split;
    ignored["oppositeDistance"] = 7.0;
    expectSame(earlierResult.plan, proceduralWarpPlan(ignored), "a symmetric Split with an opposite distance");
    // Different distances are a different frame.
    Values wider = split;
    wider["asymmetric"] = true;
    wider["oppositeDistance"] = 10.0;
    const auto a = lensEvaluate(current, kW, kH);
    const auto b = lensEvaluate(proceduralWarpPlan(wider), kW, kH);
    bool differs = false;
    if (a.frame() && b.frame())
        for (std::uint32_t j = 0; j < kH && !differs; ++j)
            for (std::uint32_t i = 0; i < kW && !differs; ++i)
                differs = lensPixel(a.frame()->processImage(), i, j) !=
                          lensPixel(b.frame()->processImage(), i, j);
    expectations.expect(differs, "an asymmetric Split with other distances is another frame");
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
        require(result.frame() != nullptr,
                std::string("the Procedural Warp plan evaluates (mode ") +
                    (values.contains("mode") ? std::to_string(std::get<std::int64_t>(values.at("mode")))
                                             : std::string("default")) +
                    ")" +
                    (result.diagnostics().empty() ? std::string{}
                                                  : ": " + result.diagnostics().front().summary +
                                                        " " + result.diagnostics().front().detail));
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
                } else if (parameters.mode == core::field::ProceduralWarpMode::Split) {
                    // Nothing in the gap: the picture times the weight.
                    for (std::size_t c = 0; c < 4; ++c)
                        want[c] *= at.weight;
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

    // Magnify's pull (Flo Motion), Ripple's pulse (Ripple Pulse).
    check({{"mode", std::int64_t{6}}, {"magnification", 220.0}, {"radius", 260.0},
           {"feather", 40.0}, {"pull", 70.0}, {"pullPoint", Vec2d{300.0, -120.0}},
           {"center", centre}},
          "a Magnify drawn towards its pull point");
    {
        const auto pulse = check({{"mode", std::int64_t{3}}, {"radius", 600.0},
                                  {"wavelength", 60.0}, {"amplitude", 14.0},
                                  {"pulseRadius", 200.0}, {"pulseSpeed", 90.0},
                                  {"center", centre}},
                                 "a Ripple pulse");
        warpDump("frame-procedural-warp-ripple-pulse", pulse.frame()->processImage(), kW, kH);
    }
    // Warp: every style, with a bend and with the distortions.
    for (std::int64_t style = 0; style <= kWarpStyleLast; ++style) {
        const auto warped = check({{"mode", std::int64_t{7}}, {"warpStyle", style},
                                   {"bend", style % 2 == 0 ? 60.0 : -45.0},
                                   {"horizontalDistortion", style % 3 == 0 ? 0.0 : 25.0},
                                   {"verticalDistortion", style % 3 == 0 ? 0.0 : -20.0}},
                                  "Warp style " + std::to_string(style), 1, 25);
        if (style == 0 || style == 5 || style == 9)
            warpDump("frame-procedural-warp-warp-" + std::to_string(style),
                     warped.frame()->processImage(), kW, kH);
    }
    for (const double direction : {90.0, 20.0}) {
        const auto bent = check({{"mode", std::int64_t{8}}, {"direction", direction},
                                 {"bend", 70.0}, {"height", 900.0}, {"center", centre}},
                                "a Bend towards " + std::to_string(direction));
        if (direction == 90.0)
            warpDump("frame-procedural-warp-bend", bent.frame()->processImage(), kW, kH);
    }
    for (const bool mirror : {false, true}) {
        const auto split = check({{"mode", std::int64_t{9}}, {"mirror", mirror},
                                  {"splitDistance", 160.0}, {"lineStart", Vec2d{-100.0, 300.0}},
                                  {"lineEnd", Vec2d{200.0, -250.0}}},
                                 mirror ? "a Split mirrored" : "a Split", 1, 25);
        warpDump(mirror ? "frame-procedural-warp-split-mirror" : "frame-procedural-warp-split",
                 split.frame()->processImage(), kW, kH);
    }
    // Asymmetric Split: each half of the line opens by its own distance (nothing at all on one side
    // is allowed), against the reading of the documentation.
    for (const double opposite : {0.0, 60.0, 260.0}) {
        const auto split = check({{"mode", std::int64_t{9}}, {"asymmetric", true},
                                  {"splitDistance", 160.0}, {"oppositeDistance", opposite},
                                  {"lineStart", Vec2d{-100.0, 300.0}},
                                  {"lineEnd", Vec2d{200.0, -250.0}}},
                                 "an asymmetric Split with an opposite distance of " +
                                     std::to_string(opposite),
                                 1, 25);
        if (opposite == 60.0)
            warpDump("frame-procedural-warp-split-asymmetric", split.frame()->processImage(), kW, kH);
    }
    for (const std::int64_t drops : {1, 12, 40}) {
        const auto drizzle = check({{"mode", std::int64_t{11}}, {"drops", drops}, {"seed", std::int64_t{9}},
                                    {"radius", 420.0}, {"wavelength", 60.0}, {"amplitude", 14.0},
                                    {"phase", 140.0}},
                                   "a Drizzle of " + std::to_string(drops) + " drops", 1, 25);
        if (drops == 12)
            warpDump("frame-procedural-warp-drizzle", drizzle.frame()->processImage(), kW, kH);
    }
    check({{"mode", std::int64_t{7}}, {"warpStyle", std::int64_t{4}}, {"bend", 80.0}},
          "a Warp at twice the scale", 2, 25);
    // Turbulent (the field Noise operation's vector moves the pixels): the same project twice is the
    // same frame; another seed and another evolution are other frames; the pinned edges stay put; a
    // displacement of 0 is the input. The noise itself is tested with the field operation.
    {
        const auto run = [&](const Values& values) {
            Values nearest = values;
            nearest["mode"] = std::int64_t{10};
            nearest["filter"] = std::int64_t{1};
            return evaluate(nearest);
        };
        const auto differing = [&](const runtime::EvaluationResult& a,
                                   const runtime::EvaluationResult& b) {
            std::size_t count = 0;
            for (std::uint32_t j = 0; j < kH; ++j)
                for (std::uint32_t i = 0; i < kW; ++i)
                    count += lensPixel(a.frame()->processImage(), i, j) !=
                                     lensPixel(b.frame()->processImage(), i, j)
                                 ? 1U
                                 : 0U;
            return count;
        };
        const auto first = run({{"displacement", 60.0}, {"seed", std::int64_t{4}}});
        const auto again = run({{"displacement", 60.0}, {"seed", std::int64_t{4}}});
        const auto other = run({{"displacement", 60.0}, {"seed", std::int64_t{5}}});
        const auto evolved =
            run({{"displacement", 60.0}, {"seed", std::int64_t{4}}, {"evolution", 0.7}});
        const auto plainFrame = lensEvaluate(warpBarePlan(), kW, kH);
        expectations.expect(first.frame() && again.frame() && other.frame() && evolved.frame(),
                            "the Turbulent plans evaluate");
        if (first.frame() && again.frame() && other.frame() && evolved.frame()) {
            expectations.expect(differing(first, again) == 0,
                                "Turbulent: the same seed and evolution are the same frame");
            expectations.expect(differing(first, other) > kW * kH / 50,
                                "Turbulent: another seed is another frame");
            expectations.expect(differing(first, evolved) > kW * kH / 50,
                                "Turbulent: another evolution is another frame");
            expectations.expect(differing(first, plainFrame) > kW * kH / 50,
                                "Turbulent moves the picture");
        }
        // All four edges pinned: the outermost pixels read where they are.
        const auto pinned = run({{"displacement", 60.0}, {"pinning", std::int64_t{1}}});
        if (pinned.frame()) {
            std::size_t moved = 0;
            std::size_t ring = 0;
            for (std::uint32_t j = 0; j < kH; ++j)
                for (std::uint32_t i = 0; i < kW; ++i) {
                    if (i != 0 && j != 0 && i != kW - 1 && j != kH - 1)
                        continue;
                    ++ring;
                    moved += lensPixel(pinned.frame()->processImage(), i, j) !=
                                     lensPixel(plainFrame.frame()->processImage(), i, j)
                                 ? 1U
                                 : 0U;
                }
            expectations.expect(moved * 20 <= ring,
                                "Turbulent: the pinned edges hold the picture still (" +
                                    std::to_string(moved) + " of " + std::to_string(ring) +
                                    " edge pixels moved)");
        }
    }

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
          {"phase", 30.0}, {"speed", 1.0}, {"center", Vec2d{8.0, 0.0}}, {"pulseRadius", 90.0},
          {"pulseSpeed", 20.0}},
         {{"angle", 45.0}, {"direction", 10.0}, {"waveShape", std::int64_t{2}},
          {"pinning", std::int64_t{1}}, {"pull", 20.0}, {"bend", 10.0}}},
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
          {"center", Vec2d{8.0, 0.0}}, {"pull", 40.0}, {"pullPoint", Vec2d{60.0, 20.0}}},
         {{"angle", 45.0}, {"falloff", 50.0}, {"amount", 20.0}, {"pinning", std::int64_t{3}},
          {"pulseRadius", 50.0}}},
        {"Warp",
         {{"mode", std::int64_t{7}}},
         {{"warpStyle", std::int64_t{5}}, {"bend", -20.0}, {"horizontalDistortion", 30.0},
          {"verticalDistortion", -30.0}},
         {{"radius", 250.0}, {"center", Vec2d{8.0, 0.0}}, {"height", 80.0}, {"amplitude", 5.0},
          {"seed", std::int64_t{9}}}},
        {"Bend",
         {{"mode", std::int64_t{8}}},
         {{"direction", 20.0}, {"bend", -20.0}, {"height", 300.0}, {"center", Vec2d{8.0, 0.0}}},
         {{"radius", 250.0}, {"warpStyle", std::int64_t{5}}, {"horizontalDistortion", 30.0},
          {"splitDistance", 5.0}}},
        {"Split",
         {{"mode", std::int64_t{9}}},
         {{"splitDistance", 90.0}, {"mirror", true}, {"lineStart", Vec2d{8.0, 40.0}},
          {"lineEnd", Vec2d{70.0, -60.0}}},
         {{"center", Vec2d{8.0, 0.0}}, {"radius", 250.0}, {"bend", 10.0}, {"pull", 20.0},
          {"pullPoint", Vec2d{3.0, 3.0}}}},
        // The symmetric split does not read the opposite distance; an asymmetric one does.
        {"Split symmetric",
         {{"mode", std::int64_t{9}}, {"splitDistance", 85.0}, {"lineStart", Vec2d{8.0, 40.0}},
          {"lineEnd", Vec2d{70.0, -60.0}}},
         {{"splitDistance", 30.0}, {"asymmetric", true}, {"mirror", true}},
         {{"oppositeDistance", 12.0}}},
        {"Split asymmetric",
         {{"mode", std::int64_t{9}}, {"asymmetric", true}, {"splitDistance", 77.0},
          {"oppositeDistance", 50.0}, {"lineStart", Vec2d{8.0, 40.0}},
          {"lineEnd", Vec2d{70.0, -60.0}}},
         {{"oppositeDistance", 20.0}, {"splitDistance", 30.0}, {"asymmetric", false},
          {"mirror", true}},
         {{"center", Vec2d{8.0, 0.0}}, {"radius", 250.0}, {"bend", 10.0}}},
        {"Turbulent",
         {{"mode", std::int64_t{10}}},
         {{"displacement", 20.0}, {"complexity", 5.0}, {"evolution", 0.5},
          {"seed", std::int64_t{7}}, {"noiseSize", Vec2d{80.0, 120.0}},
          {"noiseOffset", Vec2d{12.0, -4.0}}, {"pinning", std::int64_t{1}}},
         {{"radius", 250.0}, {"center", Vec2d{8.0, 0.0}}, {"speed", 1.0}, {"amplitude", 5.0},
          {"drops", std::int64_t{30}}}},
        {"Drizzle",
         {{"mode", std::int64_t{11}}},
         {{"radius", 250.0}, {"wavelength", 55.0}, {"amplitude", 5.0}, {"phase", 30.0},
          {"speed", 1.0}, {"seed", std::int64_t{9}}, {"drops", std::int64_t{30}}},
         {{"center", Vec2d{8.0, 0.0}}, {"falloff", 50.0}, {"displacement", 5.0},
          {"complexity", 6.0}, {"noiseSize", Vec2d{50.0, 50.0}}}}};
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
