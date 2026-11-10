// Included inside the snapshot compiler test's anonymous namespace after the Chromatic Aberration
// tests, whose checker fixture and sampling helpers it reuses.
//
// #568 Rolling Shutter: the node lowers to one field raster. The tests prove the lowering (which
// operation, which program, the three copies of the Camera Shake it reads at -1/2, 0 and +1/2 of the
// readout, time dependence, mute and the identities, the refusal of any other source of camera
// motion), then evaluate the plan on the CPU reference over a checker grid: each row against an
// independent double-precision reading of the input through the quadratic the three shake samples
// make, a flow image skewing by the row's phase, the region of interest, the pixel budget and
// cancellation.

struct ShutterFixture final {
    LensFixture lens;
    document::NodeId shake;
};

// checker -> [Camera Shake] -> Rolling Shutter -> Layer. With `withShake` the shake sits between the
// checker and the node and its Offset and Roll drive the node's camera sockets; with `withFlow` the
// checker is also the node's motion image.
[[nodiscard]] ShutterFixture
shutterFixture(const std::unordered_map<std::string, document::ParameterValue>& values = {},
               const bool withShake = false, const bool withFlow = false,
               const std::unordered_map<std::string, document::ParameterValue>& shakeValues = {}) {
    using namespace document;
    ShutterFixture fixture{lensFixture(values, "image", true, kRollingShutterNodeType),
                           NodeId::fromRaw(1560)};
    auto* composition = fixture.lens.recipe.project.findComposition(kCompositionId);
    auto& graph = composition->graph();
    if (withShake) {
        const auto* definition = builtInNodeDefinitions().find(kCameraShakeNodeType, 1);
        require(definition != nullptr, "Camera Shake definition exists");
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = 1560;
        for (const auto& parameter : definition->parameters) {
            const auto id = ParameterId::fromRaw(next++);
            ParameterValue value = parameter.defaultValue;
            if (parameter.role == "seed")
                value = std::int64_t{5};
            if (parameter.role == "amplitudeX")
                value = 40.0;
            if (parameter.role == "amplitudeY")
                value = 24.0;
            if (parameter.role == "rollAmount")
                value = 0.0;
            if (const auto found = shakeValues.find(parameter.role); found != shakeValues.end())
                value = found->second;
            require(composition->parameters().insert({id, parameter.schemaKey, ConstantValueSource{value}}),
                    "Camera Shake parameter is valid");
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({fixture.shake, std::string(kCameraShakeNodeType), std::move(bindings), 1}),
                "Camera Shake is accepted");
        require(graph.eraseEdge(kFirstSourceEdge), "the checker's edge is removed");
        require(graph.addEdge({EdgeId::fromRaw(1581), {fixture.lens.checker, "image"},
                               NodeInputRef{fixture.shake, "input"}}),
                "the checker feeds the shake");
        require(graph.addEdge({kFirstSourceEdge, {fixture.shake, "image"},
                               NodeInputRef{fixture.lens.recipe.node, "input"}}),
                "the shake feeds the node");
        const auto& parameters = fixture.lens.recipe.parameters;
        require(composition->parameters().setSource(
                    parameters.at("cameraOffset"),
                    DriverBindingSource{fixture.shake, std::string(kCameraShakeOffsetPort)}),
                "the shake's offset drives the node");
        require(composition->parameters().setSource(
                    parameters.at("cameraRoll"),
                    DriverBindingSource{fixture.shake, std::string(kCameraShakeRollPort)}),
                "the shake's roll drives the node");
    }
    if (withFlow)
        require(graph.addEdge({EdgeId::fromRaw(1582), {fixture.lens.checker, "image"},
                               NodeInputRef{fixture.lens.recipe.node,
                                            std::string(kRollingShutterMotionPort)}}),
                "the checker is the node's motion image");
    return fixture;
}

void testRollingShutterLowering(Expectations& expectations) {
    using namespace document;
    using core::field::FieldOp;

    // Nothing to skew by: no connection, no flow. The picture passes through, as it does muted, with no
    // readout or with no amount.
    {
        const std::array<std::pair<std::unordered_map<std::string, ParameterValue>, const char*>, 4>
            identities{{{{}, "with no camera and no flow"},
                        {{{"readout", 0.0}}, "with no readout"},
                        {{{"amount", 0.0}}, "with no amount"},
                        {{}, "when muted"}}};
        for (std::size_t index = 0; index < identities.size(); ++index) {
            auto fixture = shutterFixture(identities[index].first, index != 0, false);
            if (index == 3)
                fixture.lens.recipe.project.findComposition(kCompositionId)
                    ->nodeLayout()[fixture.lens.recipe.node]
                    .muted = true;
            const auto node = fixture.lens.recipe.node;
            const auto result = compileRecipe(std::move(fixture.lens.recipe.project));
            expectations.expect(result.plan != nullptr &&
                                    recipeOperations(*result.plan, node).empty(),
                                std::string("Rolling Shutter passes the picture through ") +
                                    identities[index].second);
        }
    }

    // A Camera Shake behind both sockets and a flow image: the program of the coverage fixture.
    {
        auto fixture = shutterFixture({{"readout", 0.5}}, true, true);
        const auto node = fixture.lens.recipe.node;
        const auto shake = fixture.shake;
        const auto result = compileRecipe(std::move(fixture.lens.recipe.project));
        expectations.expect(result.plan != nullptr, "Rolling Shutter compiles");
        if (!result.plan)
            return;
        const auto ops = recipeOperations(*result.plan, node);
        require(ops.size() == 1, "Rolling Shutter lowers to one raster");
        const auto* raster = lensRaster(*result.plan, ops[0]);
        require(raster != nullptr, "the raster is a field raster");
        const auto& effect = std::get<runtime::CompiledImageEffect>(result.plan->operations()[ops[0]]);
        expectations.expect(effect.fieldImages.size() == 2 && effect.fieldImages[0] == effect.input,
                            "the raster reads the picture and the flow");
        expectations.expect(raster->samples == 1 &&
                                raster->program.kind ==
                                    static_cast<std::uint8_t>(
                                        core::field::FieldResultKind::Premultiplied) &&
                                lensInstructions(raster->program, FieldOp::Image) == 2,
                            "the program is a premultiplied picture read and a flow read");
        // The three copies of the shake, at -1/4, 0 and +1/4 of a frame, publishing scalars.
        std::vector<double> offsets;
        std::size_t originals = 0;
        for (const auto& operation : result.plan->valueOperations())
            if (const auto* copy = std::get_if<runtime::CompiledValueShake>(&operation.kernel)) {
                if (operation.sourceNodeId == shake) {
                    ++originals;
                    expectations.expect(!copy->components && copy->timeOffsetFrames == 0.0,
                                        "the shake's own operation is untouched");
                } else if (operation.sourceNodeId == node) {
                    offsets.push_back(copy->timeOffsetFrames);
                    expectations.expect(copy->components && operation.outputCount == 3,
                                        "a copy publishes the offset and roll as three scalars");
                }
            }
        expectations.expect(originals == 1 && offsets == std::vector<double>{-0.25, 0.0, 0.25},
                            "the node reads the shake at -1/2, 0 and +1/2 of its readout");
        expectations.expect(result.plan->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(ops[0])),
                            "a shake-driven Rolling Shutter varies with time");
        expectations.expect(lensSequence(raster->program) ==
                                lensSequence(field_test_programs::rollingShutterProgram(1)),
                            "the coverage fixture's program is the lowering's");
    }

    // The native harness's programs for every direction and edge mode are the lowering's (the lanes
    // the phase reads and the edge choice are in the sequence).
    for (const std::int64_t direction : {kRollingShutterTopToBottom, kRollingShutterBottomToTop,
                                         kRollingShutterLeftToRight, kRollingShutterRightToLeft})
        for (const std::int64_t edges : {kRollingShutterEdgeExtend, kRollingShutterEdgeTransparent}) {
            auto fixture = shutterFixture(
                {{"readout", 0.5}, {"direction", direction}, {"edges", edges}}, true, true);
            const auto node = fixture.lens.recipe.node;
            const auto result = compileRecipe(std::move(fixture.lens.recipe.project));
            const auto ops = result.plan ? recipeOperations(*result.plan, node)
                                         : std::vector<std::size_t>{};
            const auto* raster = ops.size() == 1 ? lensRaster(*result.plan, ops[0]) : nullptr;
            expectations.expect(raster != nullptr &&
                                    lensSequence(raster->program) ==
                                        lensSequence(field_test_programs::rollingShutterProgram(
                                            1, direction, edges)),
                                "the native harness's program for direction " +
                                    std::to_string(direction) + (edges == 0 ? " Extend" : " Transparent") +
                                    " is the lowering's");
        }

    // A camera offset that is not a Camera Shake's output cannot be read at another moment.
    {
        auto fixture = shutterFixture({}, false, false);
        auto* composition = fixture.lens.recipe.project.findComposition(kCompositionId);
        constexpr auto curveId = AnimationCurveId::fromRaw(1590);
        require(composition->animationCurves().insert(ScalarAnimationCurve{
                    curveId,
                    {{KeyframeId::fromRaw(1591), core::RationalTime::fromInteger(0), 0.0,
                      KeyframeInterpolation::Linear},
                     {KeyframeId::fromRaw(1592), core::RationalTime::fromInteger(1), 3.0,
                      KeyframeInterpolation::Linear}}}),
                "the roll curve is publishable");
        require(composition->parameters().setSource(fixture.lens.recipe.parameters.at("amount"),
                                                    AnimationCurveSource{curveId}),
                "the amount animates");
        const auto node = fixture.lens.recipe.node;
        const auto result = compileRecipe(std::move(fixture.lens.recipe.project));
        expectations.expect(result.plan != nullptr && recipeOperations(*result.plan, node).empty(),
                            "an animated amount with nothing to skew by is still the identity");
    }
}

void testRollingShutterPixels(Expectations& expectations) {
    using namespace document;
    constexpr double kWidth = 1920.0;
    constexpr double kHeight = 1080.0;
    constexpr std::uint32_t kProxyWidth = 240;
    constexpr std::uint32_t kProxyHeight = 135;
    // The shake of the fixture, as the node reads it: three moments across the readout.
    const core::camera_shake::Settings settings{core::camera_shake::Mode::Handheld, 5, 2.0, 0.0, 2,
                                                0.35, 0.0};
    // The picture the node reads is the shaken checker, so its pixels are the muted node's: a muted
    // Rolling Shutter passes its input through. Its frame rate is the composition's.
    auto bare = shutterFixture({}, true, false);
    const auto rate = bare.lens.recipe.project.findComposition(kCompositionId)->format().frameRate();
    const double kRate =
        static_cast<double>(rate.numerator()) / static_cast<double>(rate.denominator());
    const auto shakeAt = [&](const double frames) {
        const auto sample = core::camera_shake::evaluate(settings, frames / kRate, kRate);
        return Vec2d{40.0 * sample.x, 24.0 * sample.y};
    };
    bare.lens.recipe.project.findComposition(kCompositionId)
        ->nodeLayout()[bare.lens.recipe.node]
        .muted = true;
    const auto bareResult = compileRecipe(std::move(bare.lens.recipe.project));
    require(bareResult.plan != nullptr, "the shaken checker compiles");
    const auto input = lensEvaluate(bareResult.plan, kProxyWidth, kProxyHeight);
    require(input.frame() != nullptr, "the shaken checker evaluates");
    const auto& source = input.frame()->processImage();

    // Top to bottom, transparent edges, half a frame of readout.
    for (const double readout : {0.5, 1.0}) {
        auto fixture = shutterFixture({{"readout", readout}, {"edges", std::int64_t{1}}}, true, false);
        const auto result = compileRecipe(std::move(fixture.lens.recipe.project));
        if (!result.plan) {
            expectations.expect(false, "the shake-driven plan compiles");
            continue;
        }
        const auto evaluated = lensEvaluate(result.plan, kProxyWidth, kProxyHeight);
        expectations.expect(evaluated.frame() != nullptr,
                            "the skewed picture evaluates" +
                                (evaluated.diagnostics().empty()
                                     ? std::string{}
                                     : ": " + evaluated.diagnostics().front().summary + " / " +
                                           evaluated.diagnostics().front().detail));
        if (!evaluated.frame())
            continue;
        const auto& image = evaluated.frame()->processImage();
        // The shake the checker already carries at the frame's time is in the input, so the change
        // is relative to it: the quadratic through the shake at -1/2, 0 and +1/2 of the readout.
        const auto a = shakeAt(-0.5 * readout);
        const auto b = shakeAt(0.0);
        const auto c = shakeAt(0.5 * readout);
        double worst = 0.0;
        double exactGap = 0.0;
        std::size_t moved = 0;
        for (std::uint32_t j = 0; j < kProxyHeight; ++j)
            for (std::uint32_t i = 0; i < kProxyWidth; ++i) {
                const auto fx = (i + 0.5) * kWidth / kProxyWidth - kWidth / 2.0;
                const auto fy = kHeight / 2.0 - (j + 0.5) * kHeight / kProxyHeight;
                const auto s = -fy / kHeight;
                const Vec2d change{s * (c.x - a.x) + 2.0 * s * s * (c.x + a.x - 2.0 * b.x),
                                   s * (c.y - a.y) + 2.0 * s * s * (c.y + a.y - 2.0 * b.y)};
                const auto exactAt = shakeAt(s * readout);
                exactGap = std::max({exactGap, std::abs(change.x - (exactAt.x - b.x)),
                                     std::abs(change.y - (exactAt.y - b.y))});
                // Field positions are y up, the shake's y down.
                const auto expected = lensSample(source, kWidth, kHeight, fx - change.x,
                                                 fy + change.y);
                const auto got = lensPixel(image, i, j);
                for (std::size_t k = 0; k < 4; ++k)
                    worst = std::max(worst, std::abs(static_cast<double>(got[k]) - expected[k]));
                moved += got != lensPixel(source, i, j) ? 1U : 0U;
            }
        expectations.expect(worst < 3.0e-4,
                            "each row is the input read at the shake's change across the readout");
        expectations.expect(exactGap < 0.5,
                            "the quadratic through three samples follows the shake within half a pixel");
        expectations.expect(moved > 0, "the rows are skewed");
    }

    // A flow image skews by the row's phase: a pixel reads the input back by flow * s * readout.
    {
        auto plainFixture = shutterFixture({}, false, false);
        const auto plainResult = compileRecipe(std::move(plainFixture.lens.recipe.project));
        require(plainResult.plan != nullptr, "the plain checker compiles");
        const auto plain = lensEvaluate(plainResult.plan, kProxyWidth, kProxyHeight);
        require(plain.frame() != nullptr, "the plain checker evaluates");
        const auto& plainSource = plain.frame()->processImage();
        auto fixture = shutterFixture({{"readout", 1.0}, {"edges", std::int64_t{1}}}, false, true);
        const auto result = compileRecipe(std::move(fixture.lens.recipe.project));
        require(result.plan != nullptr, "the flow-driven plan compiles");
        const auto evaluated = lensEvaluate(result.plan, kProxyWidth, kProxyHeight);
        require(evaluated.frame() != nullptr, "the flow-driven picture evaluates");
        const auto& image = evaluated.frame()->processImage();
        double worst = 0.0;
        std::size_t moved = 0;
        for (std::uint32_t j = 0; j < kProxyHeight; ++j)
            for (std::uint32_t i = 0; i < kProxyWidth; ++i) {
                const auto fx = (i + 0.5) * kWidth / kProxyWidth - kWidth / 2.0;
                const auto fy = kHeight / 2.0 - (j + 0.5) * kHeight / kProxyHeight;
                const auto s = -fy / kHeight;
                // The flow is the picture's stored red and green read bilinearly at the pixel; the
                // proxy's texel centre is the pixel.
                const auto flow = lensSample(plainSource, kWidth, kHeight, fx, fy);
                const auto expected =
                    lensSample(plainSource, kWidth, kHeight, fx - flow[0] * s, fy + flow[1] * s);
                const auto got = lensPixel(image, i, j);
                for (std::size_t k = 0; k < 4; ++k)
                    worst = std::max(worst, std::abs(static_cast<double>(got[k]) - expected[k]));
                moved += got != lensPixel(plainSource, i, j) ? 1U : 0U;
            }
        expectations.expect(worst < 3.0e-4, "a flow image skews each row by its phase");
        expectations.expect(moved > 0, "the flow moves the picture");
    }

    // A region of interest reads the whole frame's pixels; the pixel budget refuses the output; a
    // cancelled evaluation publishes nothing.
    {
        auto fixture = shutterFixture({{"readout", 1.0}}, true, false);
        const auto result = compileRecipe(std::move(fixture.lens.recipe.project));
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
                                       .id = runtime::TaskOwnerId::fromRaw(5682)};
        bool cancelledOutcome = false;
        auto submission = scheduler.submit<void>(
            runtime::TaskRequest("Rolling Shutter cancellation", owner),
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
        expectations.expect(taken && cancelledOutcome, "a cancelled skew publishes no image");
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        expectations.expect(scheduler.isQuiescent(), "the cancelled skew shuts down safely");
    }
}

// ---- Every read direction and both edge modes ------------------------------------------------

namespace rolling_shutter_reference {

// The input read bilinearly over premultiplied texels at the field position (x, y): a tap outside
// the picture is transparent (Transparent) or the nearest edge texel (Extend), as the Image
// instruction's two edge modes read it. An independent double-precision reading.
struct Reading final {
    std::array<double, 4> value{};
    // Whether any of the four taps lay outside the picture.
    bool outside = false;
    // Whether every tap lay outside it on the same side, so the result is one edge texel (or none).
    bool entirelyOutside = false;
};

[[nodiscard]] Reading sample(const render::Rgba32fImage& image, const double compositionWidth,
                             const double compositionHeight, const double x, const double y,
                             const bool extend) {
    const auto width = static_cast<std::int64_t>(image.descriptor()->dataWindow().extent().width());
    const auto height = static_cast<std::int64_t>(image.descriptor()->dataWindow().extent().height());
    const auto u = (x + compositionWidth / 2.0) * (static_cast<double>(width) / compositionWidth) - 0.5;
    const auto v = (compositionHeight / 2.0 - y) * (static_cast<double>(height) / compositionHeight) - 0.5;
    const auto iu = std::floor(u);
    const auto iv = std::floor(v);
    const auto tu = u - iu;
    const auto tv = v - iv;
    Reading reading;
    std::size_t outsideTaps = 0;
    const auto tap = [&](const double dx, const double dy) {
        auto tx = static_cast<std::int64_t>(iu + dx);
        auto ty = static_cast<std::int64_t>(iv + dy);
        const bool out = tx < 0 || ty < 0 || tx >= width || ty >= height;
        outsideTaps += out ? 1U : 0U;
        reading.outside = reading.outside || out;
        std::array<double, 4> value{};
        if (out && !extend)
            return value;
        tx = std::clamp<std::int64_t>(tx, 0, width - 1);
        ty = std::clamp<std::int64_t>(ty, 0, height - 1);
        const auto p = lensPixel(image, tx, ty);
        for (std::size_t c = 0; c < 4; ++c)
            value[c] = static_cast<double>(p[c]);
        return value;
    };
    const auto a = tap(0, 0);
    const auto b = tap(1, 0);
    const auto c = tap(0, 1);
    const auto d = tap(1, 1);
    for (std::size_t k = 0; k < 4; ++k) {
        const auto top = a[k] + tu * (b[k] - a[k]);
        const auto bottom = c[k] + tu * (d[k] - c[k]);
        reading.value[k] = top + tv * (bottom - top);
    }
    reading.entirelyOutside = outsideTaps == 4;
    return reading;
}

// The row (or column) phase at the field position (x, y) in composition pixels, y up: -y/height for
// Top to Bottom, y/height for Bottom to Top, x/width for Left to Right and -x/width for Right to Left.
[[nodiscard]] double phase(const std::int64_t direction, const double width, const double height,
                           const double x, const double y) {
    switch (direction) {
    case document::kRollingShutterTopToBottom:
        return -y / height;
    case document::kRollingShutterBottomToTop:
        return y / height;
    case document::kRollingShutterLeftToRight:
        return x / width;
    default:
        return -x / width;
    }
}

} // namespace rolling_shutter_reference

void testRollingShutterDirectionsAndEdges(Expectations& expectations) {
    using namespace document;
    namespace ref = rolling_shutter_reference;
    constexpr double kWidth = 1920.0;
    constexpr double kHeight = 1080.0;
    constexpr std::uint32_t kProxyWidth = 240;
    constexpr std::uint32_t kProxyHeight = 135;
    // A shake large enough that rows and columns read past the picture's edge.
    constexpr double kAmplitudeX = 300.0;
    constexpr double kAmplitudeY = 200.0;
    constexpr double kFlowAmount = 4.0;
    const core::camera_shake::Settings settings{core::camera_shake::Mode::Handheld, 5, 2.0, 0.0, 2,
                                                0.35, 0.0};
    const std::unordered_map<std::string, ParameterValue> shakeValues{
        {"amplitudeX", kAmplitudeX}, {"amplitudeY", kAmplitudeY}};

    // The shaken checker the node reads (the node muted passes its input through).
    auto bare = shutterFixture({}, true, false, shakeValues);
    const auto rate = bare.lens.recipe.project.findComposition(kCompositionId)->format().frameRate();
    const double kRate =
        static_cast<double>(rate.numerator()) / static_cast<double>(rate.denominator());
    const auto shakeAt = [&](const double frames) {
        const auto sample = core::camera_shake::evaluate(settings, frames / kRate, kRate);
        return Vec2d{kAmplitudeX * sample.x, kAmplitudeY * sample.y};
    };
    bare.lens.recipe.project.findComposition(kCompositionId)
        ->nodeLayout()[bare.lens.recipe.node]
        .muted = true;
    const auto bareResult = compileRecipe(std::move(bare.lens.recipe.project));
    require(bareResult.plan != nullptr, "the large shake of the checker compiles");
    const auto shaken = lensEvaluate(bareResult.plan, kProxyWidth, kProxyHeight);
    require(shaken.frame() != nullptr, "the large shake of the checker evaluates");
    const auto& shakenSource = shaken.frame()->processImage();
    // The unshaken checker is the flow's picture and the source of the flow-driven skew.
    auto plainFixture = shutterFixture({}, false, false);
    const auto plainResult = compileRecipe(std::move(plainFixture.lens.recipe.project));
    require(plainResult.plan != nullptr, "the plain checker compiles");
    const auto plain = lensEvaluate(plainResult.plan, kProxyWidth, kProxyHeight);
    require(plain.frame() != nullptr, "the plain checker evaluates");
    const auto& plainSource = plain.frame()->processImage();

    struct Case final {
        std::int64_t direction;
        double readout;
        bool flow;
        std::int64_t edges;
    };
    const auto name = [](const Case& item) {
        static constexpr std::array<const char*, 4> directions{"Top to Bottom", "Bottom to Top",
                                                               "Left to Right", "Right to Left"};
        return std::string(directions[static_cast<std::size_t>(item.direction)]) + ", readout " +
               (item.readout == 0.5 ? "0.5" : "1.0") + (item.flow ? ", flow" : ", shake") +
               (item.edges == kRollingShutterEdgeExtend ? ", Extend" : ", Transparent");
    };
    // What a case renders on the CPU reference at the proxy.
    const auto render = [&](const Case& item) {
        std::unordered_map<std::string, ParameterValue> values{
            {"readout", item.readout}, {"direction", item.direction}, {"edges", item.edges}};
        if (item.flow)
            values.insert_or_assign("amount", kFlowAmount);
        auto fixture = item.flow ? shutterFixture(values, false, true)
                                 : shutterFixture(values, true, false, shakeValues);
        const auto result = compileRecipe(std::move(fixture.lens.recipe.project));
        require(result.plan != nullptr, "the direction fixture compiles");
        auto evaluated = lensEvaluate(result.plan, kProxyWidth, kProxyHeight);
        require(evaluated.frame() != nullptr, "the direction fixture evaluates");
        return evaluated;
    };

    struct Outcome final {
        double worst = 0.0;
        std::size_t moved = 0;
        std::size_t reachedPastEdge = 0;
        std::size_t entirelyPast = 0;
        // Extend's pixels that are opaque where Transparent's source lay entirely outside.
        std::size_t extendFilled = 0;
        std::size_t transparentEmpty = 0;
    };
    const auto check = [&](const Case& item, const render::Rgba32fImage& image) {
        Outcome outcome;
        const bool extend = item.edges == kRollingShutterEdgeExtend;
        const auto& input = item.flow ? plainSource : shakenSource;
        const auto a = shakeAt(-0.5 * item.readout);
        const auto b = shakeAt(0.0);
        const auto c = shakeAt(0.5 * item.readout);
        for (std::uint32_t j = 0; j < kProxyHeight; ++j)
            for (std::uint32_t i = 0; i < kProxyWidth; ++i) {
                const auto fx = (i + 0.5) * kWidth / kProxyWidth - kWidth / 2.0;
                const auto fy = kHeight / 2.0 - (j + 0.5) * kHeight / kProxyHeight;
                const auto s = ref::phase(item.direction, kWidth, kHeight, fx, fy);
                double qx = fx;
                double qy = fy;
                if (item.flow) {
                    // The flow is the plain picture's stored red and green at the pixel (the
                    // proxy's texel centre is the pixel), y down: q -= f * s * readout * amount.
                    const auto flow = ref::sample(plainSource, kWidth, kHeight, fx, fy, true).value;
                    qx = fx - flow[0] * s * item.readout * kFlowAmount;
                    qy = fy + flow[1] * s * item.readout * kFlowAmount;
                } else {
                    const Vec2d change{s * (c.x - a.x) + 2.0 * s * s * (c.x + a.x - 2.0 * b.x),
                                       s * (c.y - a.y) + 2.0 * s * s * (c.y + a.y - 2.0 * b.y)};
                    qx = fx - change.x;
                    qy = fy + change.y;
                }
                const auto expected = ref::sample(input, kWidth, kHeight, qx, qy, extend);
                const auto got = lensPixel(image, i, j);
                for (std::size_t k = 0; k < 4; ++k)
                    outcome.worst = std::max(outcome.worst,
                                             std::abs(static_cast<double>(got[k]) - expected.value[k]));
                outcome.moved += got != lensPixel(input, i, j) ? 1U : 0U;
                outcome.reachedPastEdge += expected.outside ? 1U : 0U;
                if (expected.entirelyOutside) {
                    ++outcome.entirelyPast;
                    outcome.extendFilled += extend && got[3] > 0.0F ? 1U : 0U;
                    outcome.transparentEmpty +=
                        !extend && got == std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F} ? 1U : 0U;
                }
            }
        return outcome;
    };

    constexpr std::array<std::int64_t, 4> directions{kRollingShutterTopToBottom,
                                                     kRollingShutterBottomToTop,
                                                     kRollingShutterLeftToRight,
                                                     kRollingShutterRightToLeft};
    std::map<std::pair<std::int64_t, bool>, std::shared_ptr<const runtime::ProcessFrame>> sample;
    for (const auto direction : directions)
        for (const double readout : {0.5, 1.0})
            for (const bool flow : {false, true}) {
                // Transparent edges for every direction (Top to Bottom is covered above for the
                // shake; here it anchors the flow case), then Extend for all four.
                std::array<std::shared_ptr<const runtime::ProcessFrame>, 2> frames;
                std::array<Outcome, 2> outcomes;
                for (const auto edges : {kRollingShutterEdgeTransparent, kRollingShutterEdgeExtend}) {
                    const Case item{direction, readout, flow, edges};
                    const auto evaluated = render(item);
                    const auto& image = evaluated.frame()->processImage();
                    const auto outcome = check(item, image);
                    const auto slot = static_cast<std::size_t>(edges);
                    frames[slot] = evaluated.frame();
                    outcomes[slot] = outcome;
                    expectations.expect(outcome.worst < 3.0e-4,
                                        name(item) + ": each pixel is the input read through the "
                                                     "independent reference");
                    expectations.expect(outcome.moved > 0, name(item) + ": the picture is skewed");
                    expectations.expect(outcome.reachedPastEdge > 0,
                                        name(item) + ": the motion reads past the picture's edge");
                    if (edges == kRollingShutterEdgeExtend && outcome.entirelyPast > 0)
                        expectations.expect(outcome.extendFilled == outcome.entirelyPast,
                                            name(item) + ": a read entirely past the edge sees "
                                                         "the edge pixel, not nothing");
                    if (edges == kRollingShutterEdgeTransparent && outcome.entirelyPast > 0)
                        expectations.expect(outcome.transparentEmpty == outcome.entirelyPast,
                                            name(item) + ": a read entirely past the edge is "
                                                         "transparent");
                }
                // Extend and Transparent agree exactly wherever no tap left the picture and
                // differ where one did.
                std::size_t agree = 0;
                std::size_t differ = 0;
                const auto& transparent = frames[1]->processImage();
                const auto& extended = frames[0]->processImage();
                for (std::uint32_t j = 0; j < kProxyHeight; ++j)
                    for (std::uint32_t i = 0; i < kProxyWidth; ++i) {
                        const bool same = lensPixel(transparent, i, j) == lensPixel(extended, i, j);
                        (same ? agree : differ) += 1U;
                    }
                expectations.expect(differ > 0 && agree > 0,
                                    name({direction, readout, flow, kRollingShutterEdgeExtend}) +
                                        ": Extend and Transparent differ at the edge and agree "
                                        "elsewhere");
                // They differ exactly at the pixels whose reference taps left the picture.
                std::size_t mismatched = 0;
                {
                    const auto a = shakeAt(-0.5 * readout);
                    const auto b = shakeAt(0.0);
                    const auto c = shakeAt(0.5 * readout);
                    for (std::uint32_t j = 0; j < kProxyHeight; ++j)
                        for (std::uint32_t i = 0; i < kProxyWidth; ++i) {
                            const auto fx = (i + 0.5) * kWidth / kProxyWidth - kWidth / 2.0;
                            const auto fy = kHeight / 2.0 - (j + 0.5) * kHeight / kProxyHeight;
                            const auto s = ref::phase(direction, kWidth, kHeight, fx, fy);
                            double qx = fx;
                            double qy = fy;
                            if (flow) {
                                const auto f =
                                    ref::sample(plainSource, kWidth, kHeight, fx, fy, true).value;
                                qx = fx - f[0] * s * readout * kFlowAmount;
                                qy = fy + f[1] * s * readout * kFlowAmount;
                            } else {
                                const Vec2d change{
                                    s * (c.x - a.x) + 2.0 * s * s * (c.x + a.x - 2.0 * b.x),
                                    s * (c.y - a.y) + 2.0 * s * s * (c.y + a.y - 2.0 * b.y)};
                                qx = fx - change.x;
                                qy = fy + change.y;
                            }
                            const auto reading = ref::sample(flow ? plainSource : shakenSource,
                                                             kWidth, kHeight, qx, qy, true);
                            const bool same = lensPixel(transparent, i, j) == lensPixel(extended, i, j);
                            // A pixel whose taps stayed inside must be identical in both modes; one
                            // that left may coincide only when the edge texel is itself transparent.
                            if (!reading.outside && !same)
                                ++mismatched;
                        }
                }
                expectations.expect(mismatched == 0,
                                    name({direction, readout, flow, kRollingShutterEdgeExtend}) +
                                        ": the two edge modes agree wherever the read stays inside");
                sample.emplace(std::pair{direction * 10 + static_cast<std::int64_t>(readout * 2.0), flow},
                               frames[0]);
            }

    // A horizontal read skews by the column's phase, not the row's: Left to Right and Top to Bottom
    // give different pictures from the same flow, and in Left to Right a column is read at one phase,
    // so two pixels of one column over a flow constant along the column read the same offset. The
    // flow here is the checker's, so the proof is the reference match above plus the pictures
    // differing.
    {
        const auto& leftToRight = sample.at({kRollingShutterLeftToRight * 10 + 2, true})->processImage();
        const auto& topToBottom = sample.at({kRollingShutterTopToBottom * 10 + 2, true})->processImage();
        std::size_t differs = 0;
        for (std::uint32_t j = 0; j < kProxyHeight; ++j)
            for (std::uint32_t i = 0; i < kProxyWidth; ++i)
                differs += lensPixel(leftToRight, i, j) != lensPixel(topToBottom, i, j) ? 1U : 0U;
        expectations.expect(differs > 0,
                            "a horizontal read skews a flow image by the column's phase, not the row's");
    }

    // The default edge mode is Extend: a node that does not name it renders what Extend renders.
    {
        const auto* definition = builtInNodeDefinitions().find(kRollingShutterNodeType, 1);
        require(definition != nullptr, "the Rolling Shutter definition exists");
        const auto edgesRole = std::ranges::find(definition->parameters, std::string("edges"),
                                                 &ParameterDefinition::role);
        expectations.expect(edgesRole != definition->parameters.end() &&
                                std::get_if<std::int64_t>(&edgesRole->defaultValue) != nullptr &&
                                *std::get_if<std::int64_t>(&edgesRole->defaultValue) ==
                                    kRollingShutterEdgeExtend,
                            "the schema's default for edges is Extend");
        auto unspecified = shutterFixture({{"readout", 1.0}}, true, false, shakeValues);
        const auto unspecifiedResult = compileRecipe(std::move(unspecified.lens.recipe.project));
        auto named = shutterFixture({{"readout", 1.0}, {"edges", kRollingShutterEdgeExtend}}, true,
                                    false, shakeValues);
        const auto namedResult = compileRecipe(std::move(named.lens.recipe.project));
        require(unspecifiedResult.plan != nullptr && namedResult.plan != nullptr,
                "the default-edge fixtures compile");
        const auto left = lensEvaluate(unspecifiedResult.plan, kProxyWidth, kProxyHeight);
        const auto right = lensEvaluate(namedResult.plan, kProxyWidth, kProxyHeight);
        bool same = left.frame() != nullptr && right.frame() != nullptr;
        for (std::uint32_t j = 0; same && j < kProxyHeight; ++j)
            for (std::uint32_t i = 0; i < kProxyWidth; ++i)
                same = same && lensPixel(left.frame()->processImage(), i, j) ==
                                   lensPixel(right.frame()->processImage(), i, j);
        expectations.expect(same, "an unspecified edges setting renders as Extend");
    }
}
