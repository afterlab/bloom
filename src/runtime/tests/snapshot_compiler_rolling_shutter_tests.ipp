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
