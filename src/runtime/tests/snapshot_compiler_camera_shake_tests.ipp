// Included inside the snapshot compiler test's anonymous namespace after the motion tests.
//
// #512 Camera Shake: the node is Transform 2D (the same kernel, resampler and motion blur) driven by
// one seeded shake value operation whose offset and roll outputs are also the node's own value
// outputs. The tests prove that the values it publishes are the transform it applies, that its
// automatic overscan leaves no edge showing at the largest shake, that the loop closes, that the
// outputs can drive another node's parameters, and that a plan evaluates.
void testCameraShake(Expectations& expectations) {
    using namespace document;
    // One layer, so nothing else covers the frame behind the shaken picture.
    const auto recipeFixture = [](const std::string_view type,
                                  const std::unordered_map<std::string, ParameterValue>& values) {
        return ::recipeFixture(type, values, {}, "image", true);
    };
    const auto evaluateAt = [&](const runtime::SnapshotCompileResult& result,
                                const core::RationalTime time, const bool proxy) {
        const auto extent = render::ImageExtent::create(240, 135);
        runtime::EvaluationRequest request{time, result.plan->output(),
                                           runtime::ProxyResolution{*extent.value()}};
        if (!proxy)
            request.resolution = runtime::CompositionFormatResolution{};
        request.pixelStorageByteLimit = std::size_t{1} << 28U;
        // A fresh evaluator: its frame cache is keyed by the plan's identity, which two projects
        // that differ only in a value share.
        const runtime::CpuCompositionEvaluator evaluator;
        return evaluator.evaluate(result.plan, request, {});
    };
    // The first output of the node's shake operation: the offset, then the roll, then the scale.
    const auto shakeOutput = [](const runtime::CompiledCompositionPlan& plan, const NodeId node)
        -> std::optional<std::size_t> {
        for (const auto& operation : plan.valueOperations())
            if (operation.sourceNodeId == node &&
                std::holds_alternative<runtime::CompiledValueShake>(operation.kernel))
                return operation.firstOutput.value();
        return std::nullopt;
    };
    const auto pixelsOf = [](const runtime::EvaluationResult& evaluated) {
        const auto span = evaluated.frame()->processImage().pixels();
        return std::vector<render::Rgba32f>(span.begin(), span.end());
    };
    const auto outputOf = [](const runtime::EvaluationResult& evaluated, const std::size_t index) {
        return evaluated.frame()->valueOutputs()[index];
    };

    // The lowering: a Transform 2D kernel reading the shake's outputs, and no other operation of
    // the node's own.
    {
        auto fixture = recipeFixture(kCameraShakeNodeType, {{"seed", std::int64_t{5}}});
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Camera Shake compiles");
        if (!result.plan)
            return;
        const auto first = shakeOutput(*result.plan, node);
        expectations.expect(first.has_value(), "Camera Shake compiles one shake operation");
        const auto operations = recipeOperations(*result.plan, node);
        const auto* transform =
            operations.size() == 1 ? recipeKernel<runtime::Transform2DKernel>(*result.plan,
                                                                              operations[0])
                                   : nullptr;
        expectations.expect(transform != nullptr && first &&
                                transform->extent == runtime::Transform2DExtent::Input &&
                                std::get_if<runtime::ValueOutputIndex>(&transform->translate.source) &&
                                std::get_if<runtime::ValueOutputIndex>(&transform->translate.source)
                                        ->value() == *first &&
                                std::get_if<runtime::ValueOutputIndex>(&transform->rotation.source) &&
                                std::get_if<runtime::ValueOutputIndex>(&transform->rotation.source)
                                        ->value() == *first + 1 &&
                                std::get_if<runtime::ValueOutputIndex>(&transform->scale.source) &&
                                std::get_if<runtime::ValueOutputIndex>(&transform->scale.source)
                                        ->value() == *first + 2,
                            "Camera Shake is a Transform 2D driven by the shake's outputs");
    }

    // The published offset and roll are the shake of the settings at the frame, for every style.
    for (const auto mode : {kShakeModeHandheld, kShakeModeImpact, kShakeModeGateWeave}) {
        auto fixture = recipeFixture(kCameraShakeNodeType,
                                     {{"mode", mode},
                                      {"seed", std::int64_t{77}},
                                      {"amplitudeX", 30.0},
                                      {"amplitudeY", 12.0},
                                      {"rollAmount", 2.0},
                                      {"octaves", std::int64_t{3}},
                                      {"roughness", 0.5},
                                      {"smoothing", 0.25}});
        const auto node = fixture.node;
        const auto rate = fixture.project.findComposition(kCompositionId)->format().frameRate();
        const double frameRate =
            static_cast<double>(rate.numerator()) / static_cast<double>(rate.denominator());
        auto result = compileRecipe(std::move(fixture.project));
        if (!result.plan) {
            expectations.expect(false, "Camera Shake compiles in every style");
            continue;
        }
        const auto first = shakeOutput(*result.plan, node);
        for (const std::int64_t t : {0, 1, 3, 8}) {
            const auto evaluated = evaluateAt(result, core::RationalTime::fromInteger(t), true);
            if (!first || evaluated.frame() == nullptr) {
                expectations.expect(false, "Camera Shake evaluates");
                continue;
            }
            const auto unit = core::camera_shake::evaluate(
                {static_cast<core::camera_shake::Mode>(mode), 77, 2.0, 0.0, 3, 0.5, 0.25},
                static_cast<double>(t), frameRate);
            const auto* offset = std::get_if<Vec2d>(&evaluated.frame()->valueOutputs()[*first]);
            const auto* roll = std::get_if<double>(&evaluated.frame()->valueOutputs()[*first + 1]);
            expectations.expect(offset != nullptr && roll != nullptr &&
                                    *offset == Vec2d{30.0 * unit.x, 12.0 * unit.y} &&
                                    *roll == 2.0 * unit.roll,
                                "the published offset and roll are the shake at the frame");
        }
    }

    // The values it publishes are the transform it applies: the same picture as a Transform 2D
    // holding those values.
    {
        auto fixture = recipeFixture(kCameraShakeNodeType, {{"seed", std::int64_t{31}},
                                                            {"amplitudeX", 40.0},
                                                            {"amplitudeY", 25.0},
                                                            {"rollAmount", 4.0}});
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        const auto first = result.plan ? shakeOutput(*result.plan, node) : std::nullopt;
        if (!first) {
            expectations.expect(false, "Camera Shake compiles for the transform comparison");
        } else {
            for (const std::int64_t t : {2, 5}) {
                const auto time = core::RationalTime::fromInteger(t);
                const auto shaken = evaluateAt(result, time, true);
                if (shaken.frame() == nullptr) {
                    expectations.expect(false, "Camera Shake evaluates for the comparison");
                    continue;
                }
                const auto offset = std::get<Vec2d>(outputOf(shaken, *first));
                const auto roll = std::get<double>(outputOf(shaken, *first + 1));
                const auto scale = std::get<Vec2d>(outputOf(shaken, *first + 2));
                expectations.expect(offset.x != 0.0 || offset.y != 0.0 || roll != 0.0,
                                    "the shake moves at the frame");

                auto project = makeProject(singleLayerOptions());
                auto* composition = project.findComposition(kCompositionId);
                auto& graph = composition->graph();
                const auto transformId = NodeId::fromRaw(1512);
                const auto* definition = builtInNodeDefinitions().find(kTransform2DNodeType, 1);
                require(definition != nullptr, "Transform 2D definition exists");
                std::vector<ParameterBinding> bindings;
                std::uint64_t next = 1512;
                for (const auto& parameter : definition->parameters) {
                    const auto id = ParameterId::fromRaw(next++);
                    auto value = parameter.defaultValue;
                    if (parameter.role == "translate")
                        value = offset;
                    if (parameter.role == "scale")
                        value = scale;
                    if (parameter.role == "rotation")
                        value = roll;
                    if (parameter.role == "extent")
                        value = std::int64_t{1};
                    require(composition->parameters().insert(
                                {id, parameter.schemaKey, ConstantValueSource{value}}),
                            "Transform 2D parameter is valid");
                    bindings.push_back({parameter.role, id});
                }
                require(graph.addNode({transformId, std::string(kTransform2DNodeType),
                                       std::move(bindings), 1}),
                        "Transform 2D node is accepted");
                require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
                require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                                       NodeInputRef{transformId, "input"}}),
                        "source feeds Transform 2D");
                require(graph.addEdge({EdgeId::fromRaw(1512), {transformId, "image"},
                                       NodeInputRef{kFirstLayerNode,
                                                    std::string(kLayerOutputContentInputPort)}}),
                        "Transform 2D feeds Layer");
                auto reference = compileRecipe(std::move(project));
                if (!reference.plan) {
                    expectations.expect(false, "the reference Transform 2D compiles");
                    continue;
                }
                const auto expected = evaluateAt(reference, time, true);
                expectations.expect(expected.frame() != nullptr &&
                                        pixelsOf(expected) == pixelsOf(shaken),
                                    "Camera Shake draws the transform its outputs publish");
            }
        }
    }

    // The loop closes: the frame one loop length on is the first frame, exactly.
    {
        auto fixture = recipeFixture(kCameraShakeNodeType, {{"seed", std::int64_t{9}},
                                                            {"loop", true},
                                                            {"loopLength", 3.0}});
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        const auto first = result.plan ? shakeOutput(*result.plan, node) : std::nullopt;
        if (!first) {
            expectations.expect(false, "a looping Camera Shake compiles");
        } else {
            const auto start = evaluateAt(result, core::RationalTime::fromInteger(0), true);
            const auto end = evaluateAt(result, core::RationalTime::fromInteger(3), true);
            const auto middle = evaluateAt(result, core::RationalTime::fromInteger(1), true);
            expectations.expect(start.frame() && end.frame() && middle.frame() &&
                                    outputOf(start, *first) == outputOf(end, *first) &&
                                    outputOf(start, *first + 1) == outputOf(end, *first + 1) &&
                                    pixelsOf(start) == pixelsOf(end) &&
                                    outputOf(start, *first) != outputOf(middle, *first),
                                "frame zero equals the frame one loop length on");
        }
    }

    // Auto overscan: at the largest shake the picture still covers every pixel it covers without
    // the shake (the fixture's Layer places and fades its Solid, so "covered" is the unshaken
    // frame's own alpha), at full size and in an eighth-size preview; without overscan the moved
    // picture leaves some of it bare.
    // The Layer's own partly covered edge pixels differ by a fraction of a percent between two
    // resamples; a bare picture edge is a tenth or more.
    constexpr float kAlphaTolerance = 0.01F;
    for (const bool overscan : {true, false}) {
        auto fixture = recipeFixture(kCameraShakeNodeType, {{"seed", std::int64_t{3}},
                                                            {"amplitudeX", 60.0},
                                                            {"amplitudeY", 40.0},
                                                            {"rollAmount", 3.0},
                                                            {"overscan", overscan}});
        auto still = recipeFixture(kCameraShakeNodeType, {{"amplitudeX", 0.0},
                                                          {"amplitudeY", 0.0},
                                                          {"rollAmount", 0.0}});
        auto result = compileRecipe(std::move(fixture.project));
        auto reference = compileRecipe(std::move(still.project));
        if (!result.plan || !reference.plan) {
            expectations.expect(false, "Camera Shake compiles for the overscan check");
            continue;
        }
        bool covered = true;
        bool anyBare = false;
        for (const bool proxy : {true, false}) {
            const auto baseline = evaluateAt(reference, core::RationalTime::fromInteger(0), proxy);
            if (baseline.frame() == nullptr) {
                covered = false;
                continue;
            }
            const auto bare = pixelsOf(baseline);
            for (std::int64_t t = 0; t < (proxy ? 5 : 2); ++t) {
                const auto evaluated = evaluateAt(result, core::RationalTime::fromInteger(t), proxy);
                if (evaluated.frame() == nullptr) {
                    covered = false;
                    continue;
                }
                const auto shaken = pixelsOf(evaluated);
                for (std::size_t i = 0; i < shaken.size() && i < bare.size(); ++i) {
                    covered = covered && shaken[i].alpha() >= bare[i].alpha() - kAlphaTolerance;
                    anyBare = anyBare || shaken[i].alpha() < bare[i].alpha() - kAlphaTolerance;
                }
            }
        }
        if (overscan)
            expectations.expect(covered && !anyBare,
                                "auto overscan leaves no edge showing at the largest shake");
        else
            expectations.expect(anyBare, "without overscan the moved image shows its edge");
    }

    // The outputs drive other parameters: a Transform 2D after the shake takes its translation and
    // rotation from the shake's offset and roll.
    {
        auto fixture = recipeFixture(kCameraShakeNodeType, {{"seed", std::int64_t{4}}});
        const auto shake = fixture.node;
        auto* composition = fixture.project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto transformId = NodeId::fromRaw(1513);
        const auto* definition = builtInNodeDefinitions().find(kTransform2DNodeType, 1);
        require(definition != nullptr, "Transform 2D definition exists");
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = 1513;
        for (const auto& parameter : definition->parameters) {
            const auto id = ParameterId::fromRaw(next++);
            const auto port = parameter.role == "translate" ? std::string(kCameraShakeOffsetPort)
                              : parameter.role == "rotation" ? std::string(kCameraShakeRollPort)
                                                             : std::string{};
            require(composition->parameters().insert(
                        {id, parameter.schemaKey,
                         port.empty() ? ParameterSource{ConstantValueSource{parameter.defaultValue}}
                                      : ParameterSource{DriverBindingSource{shake, port}}}),
                    "Transform 2D parameter is valid");
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({transformId, std::string(kTransform2DNodeType),
                               std::move(bindings), 1}),
                "Transform 2D node is accepted");
        // The shake feeds the Transform 2D, which replaces the shake as the Layer's content.
        require(graph.eraseEdge(EdgeId::fromRaw(1300)), "shake's Layer edge is removed");
        require(graph.addEdge({EdgeId::fromRaw(1514), {shake, "image"},
                               NodeInputRef{transformId, "input"}}),
                "shake feeds Transform 2D");
        require(graph.addEdge({EdgeId::fromRaw(1515), {transformId, "image"},
                               NodeInputRef{kFirstLayerNode,
                                            std::string(kLayerOutputContentInputPort)}}),
                "Transform 2D feeds Layer");
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "a node driven by the shake's outputs compiles");
        if (result.plan) {
            const auto first = shakeOutput(*result.plan, shake);
            bool driven = false;
            for (const auto& operation : result.plan->operations())
                if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
                    effect != nullptr && effect->sourceNodeId == transformId)
                    if (const auto* kernel = std::get_if<runtime::Transform2DKernel>(&effect->kernel)) {
                        const auto* translate =
                            std::get_if<runtime::ValueOutputIndex>(&kernel->translate.source);
                        const auto* rotation =
                            std::get_if<runtime::ValueOutputIndex>(&kernel->rotation.source);
                        driven = first && translate && rotation && translate->value() == *first &&
                                 rotation->value() == *first + 1;
                    }
            expectations.expect(driven, "the shake's offset and roll drive another node");
            const auto evaluated = evaluateAt(result, core::RationalTime::fromInteger(2), true);
            expectations.expect(evaluated.frame() != nullptr, "the driven graph evaluates");
        }
    }

    // Motion blur: the path sampler through the shake, which evaluates.
    {
        auto fixture = recipeFixture(kCameraShakeNodeType, {{"seed", std::int64_t{12}},
                                                            {"motionBlur", true},
                                                            {"quality", kMotionQualityDraft}});
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Camera Shake with motion blur compiles");
        if (result.plan) {
            const auto operations = recipeOperations(*result.plan, node);
            const auto* path = operations.size() == 1
                                   ? recipeKernel<runtime::MotionPathBlurKernel>(*result.plan,
                                                                                 operations[0])
                                   : nullptr;
            expectations.expect(path != nullptr && path->mode == runtime::MotionPathMode::Transform &&
                                    path->transform.has_value(),
                                "Camera Shake's motion blur is the path sampler through it");
            const auto blurred = evaluateAt(result, core::RationalTime::fromInteger(2), true);
            expectations.expect(blurred.frame() != nullptr, "Camera Shake with motion blur evaluates");
        }
    }
}
