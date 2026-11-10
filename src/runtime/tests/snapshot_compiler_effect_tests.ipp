void testImageEffectLowering(Expectations& expectations) {
    using namespace document;
    for (const auto* type : {"bloom.ocio-colour-space-transform", "bloom.ocio-file-transform"}) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto effectId = NodeId::fromRaw(900);
        const auto* definition = builtInNodeDefinitions().find(type, 1);
        require(definition != nullptr, "CST definition exists");
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = 900;
        for (const auto& parameter : definition->parameters) {
            const auto id = ParameterId::fromRaw(next++);
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{parameter.defaultValue}}),
                    "CST parameter is valid");
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({effectId, definition->key.typeId, std::move(bindings), 1}),
                "CST node is accepted");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
        require(
            graph.addEdge(
                {kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{effectId, "input"}}),
            "source feeds effect");
        require(graph.addEdge(
                    {EdgeId::fromRaw(900),
                     {effectId, "image"},
                     NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
                "effect feeds Layer");
        expectations.expect(project.validate().ok(), "source/effect/Layer is valid document truth");
        runtime::NodeDefinitionRegistry registry;
        require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
        registry.freeze();
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "CST graph compiles");
        if (result.plan) {
            const auto effect =
                std::ranges::find_if(result.plan->operations(), [](const auto& operation) {
                    return std::holds_alternative<runtime::CompiledImageEffect>(operation);
                });
            expectations.expect(effect != result.plan->operations().end(),
                                "CST lowers through the reusable image-effect operation");
            if (effect != result.plan->operations().end()) {
                const auto& lowered = std::get<runtime::CompiledImageEffect>(*effect);
                expectations.expect(
                    lowered.sourceNodeId == effectId &&
                        (std::string_view(type) == "bloom.ocio-colour-space-transform"
                             ? std::holds_alternative<runtime::CstKernel>(lowered.kernel)
                             : std::holds_alternative<runtime::FileTransformKernel>(
                                   lowered.kernel)),
                    "lowering preserves CST node identity and typed kernel");
            }
        }
    }
}

void testMultiplyInvertLowering(Expectations& expectations) {
    using namespace document;
    for (const auto* type : {"bloom.multiply", "bloom.invert"}) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto effectId = NodeId::fromRaw(900);
        const auto* definition = builtInNodeDefinitions().find(type, 1);
        require(definition != nullptr, "pointwise definition exists");
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = 900;
        for (const auto& parameter : definition->parameters) {
            const auto id = ParameterId::fromRaw(next++);
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{parameter.defaultValue}}),
                    "pointwise parameter is valid");
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({effectId, definition->key.typeId, std::move(bindings), 1}),
                "pointwise node is accepted");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
        require(graph.addEdge(
                    {kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{effectId, "input"}}),
                "source feeds pointwise effect");
        require(graph.addEdge(
                    {EdgeId::fromRaw(900), {effectId, "image"},
                     NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
                "pointwise effect feeds Layer");
        expectations.expect(project.validate().ok(), "pointwise graph is valid document truth");
        runtime::NodeDefinitionRegistry registry;
        require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
        registry.freeze();
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "pointwise graph compiles");
        if (!result.plan)
            continue;
        const auto operation = std::ranges::find_if(result.plan->operations(), [](const auto& item) {
            return std::holds_alternative<runtime::CompiledImageEffect>(item);
        });
        expectations.expect(operation != result.plan->operations().end(),
                            "pointwise node lowers through the image-effect operation");
        if (operation == result.plan->operations().end())
            continue;
        const auto& lowered = std::get<runtime::CompiledImageEffect>(*operation);
        const auto* kernel = std::get_if<runtime::PointwiseImageKernel>(&lowered.kernel);
        const bool isMultiply = std::string_view(type) == "bloom.multiply";
        expectations.expect(
            lowered.sourceNodeId == effectId && kernel &&
                kernel->effect == (isMultiply ? runtime::PointwiseImageEffect::Multiply
                                             : runtime::PointwiseImageEffect::Invert) &&
                (isMultiply ? kernel->amount &&
                                  std::get_if<double>(&kernel->amount->source) &&
                                  *std::get_if<double>(&kernel->amount->source) == 1.0
                            : !kernel->amount),
            "Multiply and Invert retain typed operation, neutral factor and node identity");
    }
}

void testExposureOffsetLowering(Expectations& expectations) {
    using namespace document;
    for (const auto* type : {"bloom.exposure", "bloom.offset"}) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto effectId = NodeId::fromRaw(920);
        const auto* definition = builtInNodeDefinitions().find(type, 1);
        // #383: the scalar control, then the effect mask's channel selector.
        require(definition != nullptr && definition->parameters.size() == 2 &&
                    definition->parameters.back().role == "maskChannel",
                "pointwise definition exists with one scalar control and the mask channel");
        const auto parameterId = ParameterId::fromRaw(920);
        require(composition->parameters().insert(
                    {parameterId, definition->parameters.front().schemaKey,
                     ConstantValueSource{1.25}}),
                "pointwise amount is valid");
        require(graph.addNode({effectId, definition->key.typeId, {{"amount", parameterId}}, 1}),
                "pointwise node is accepted");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
        require(graph.addEdge(
                    {kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{effectId, "input"}}),
                "source feeds pointwise effect");
        require(graph.addEdge(
                    {EdgeId::fromRaw(920), {effectId, "image"},
                     NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
                "pointwise effect feeds Layer");
        expectations.expect(project.validate().ok(), "pointwise graph is valid document truth");
        runtime::NodeDefinitionRegistry registry;
        require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
        registry.freeze();
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "pointwise graph compiles");
        if (!result.plan)
            continue;
        const auto effect = std::ranges::find_if(result.plan->operations(), [](const auto& operation) {
            return std::holds_alternative<runtime::CompiledImageEffect>(operation);
        });
        expectations.expect(effect != result.plan->operations().end(),
                            "pointwise lowers through image-effect operation");
        if (effect == result.plan->operations().end())
            continue;
        const auto& lowered = std::get<runtime::CompiledImageEffect>(*effect);
        const auto* kernel = std::get_if<runtime::PointwiseImageKernel>(&lowered.kernel);
        expectations.expect(kernel && kernel->amount &&
                                kernel->effect ==
                                    (std::string_view(type) == "bloom.exposure"
                                         ? runtime::PointwiseImageEffect::Exposure
                                         : runtime::PointwiseImageEffect::Offset) &&
                                kernel->amount->id == parameterId,
                            "pointwise lowering retains mode and editable parameter identity");
    }
}

void testCanvasLowering(Expectations& expectations) {
    using namespace document;
    for (const auto* type : {"bloom.crop", "bloom.pad"}) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto effectId = NodeId::fromRaw(940);
        const auto* definition = builtInNodeDefinitions().find(type, 1);
        require(definition != nullptr && definition->parameters.size() == 4,
                "canvas definition has four margins");
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = 940;
        for (const auto& parameter : definition->parameters) {
            const auto id = ParameterId::fromRaw(next++);
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{1.0}}),
                    "canvas margin is valid");
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({effectId, definition->key.typeId, std::move(bindings), 1}),
                "canvas node is accepted");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
        require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                               NodeInputRef{effectId, "input"}}),
                "source feeds canvas effect");
        require(graph.addEdge({EdgeId::fromRaw(940), {effectId, "image"},
                               NodeInputRef{kFirstLayerNode,
                                            std::string(kLayerOutputContentInputPort)}}),
                "canvas effect feeds Layer");
        expectations.expect(project.validate().ok(), "canvas graph is valid document truth");
        runtime::NodeDefinitionRegistry registry;
        require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
        registry.freeze();
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "canvas graph compiles");
        if (!result.plan)
            continue;
        const auto effect = std::ranges::find_if(result.plan->operations(), [](const auto& op) {
            return std::holds_alternative<runtime::CompiledImageEffect>(op);
        });
        expectations.expect(effect != result.plan->operations().end(),
                            "canvas node lowers through the image-effect operation");
        if (effect == result.plan->operations().end())
            continue;
        const auto* kernel = std::get_if<runtime::CanvasImageKernel>(
            &std::get<runtime::CompiledImageEffect>(*effect).kernel);
        expectations.expect(kernel &&
                                kernel->effect ==
                                    (std::string_view(type) == "bloom.crop"
                                         ? runtime::CanvasImageEffect::Crop
                                         : runtime::CanvasImageEffect::Pad) &&
                                kernel->margins[0].id == ParameterId::fromRaw(940) &&
                                kernel->margins[3].id == ParameterId::fromRaw(943),
                            "canvas lowering retains mode and editable margin identities");
        const auto effectIndex = static_cast<std::size_t>(
            std::distance(result.plan->operations().begin(), effect));
        expectations.expect(!result.plan->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(effectIndex)),
                            "constant canvas margins remain time invariant");
        auto animatedDefinition = result.plan->copyDefinition();
        auto& animatedKernel = std::get<runtime::CanvasImageKernel>(
            std::get<runtime::CompiledImageEffect>(
                animatedDefinition.operations[effectIndex]).kernel);
        animatedKernel.margins[0].source = runtime::ScalarCurveIndex::fromRaw(0);
        animatedDefinition.scalarCurves.push_back(
            runtime::CompiledScalarCurve{AnimationCurveId::fromRaw(940),
                                         {{KeyframeId::fromRaw(940),
                                           core::RationalTime::fromInteger(0), 0.0},
                                          {KeyframeId::fromRaw(941),
                                           core::RationalTime::fromInteger(1), 1.0}}});
        const auto animated = std::make_shared<const runtime::CompiledCompositionPlan>(
            std::move(animatedDefinition));
        expectations.expect(animated->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(effectIndex)) &&
                                animated->operationTimeDependent(animated->output()),
                            "animated canvas margin invalidates itself and downstream output");
        auto drivenDefinition = result.plan->copyDefinition();
        auto& drivenKernel = std::get<runtime::CanvasImageKernel>(
            std::get<runtime::CompiledImageEffect>(drivenDefinition.operations[effectIndex]).kernel);
        drivenKernel.margins[3].source = runtime::ValueOutputIndex::fromRaw(0);
        drivenDefinition.valueOperations = {
            {NodeId::fromRaw(950), runtime::ValueOutputIndex::fromRaw(0), 2,
             runtime::CompiledValueTime{}}};
        drivenDefinition.valueOutputCount = 2;
        const auto driven = std::make_shared<const runtime::CompiledCompositionPlan>(
            std::move(drivenDefinition));
        expectations.expect(driven->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(effectIndex)) &&
                                driven->operationTimeDependent(driven->output()),
                            "driven canvas margin invalidates itself and downstream output");
    }
}

void testBlurLowering(Expectations& expectations) {
    using namespace document;
    for (const auto* type : {"bloom.gaussian-blur", "bloom.box-blur"}) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto node = NodeId::fromRaw(960);
        const auto radius = ParameterId::fromRaw(960);
        const auto edge = ParameterId::fromRaw(961);
        require(composition->parameters().insert(
                    {radius, "bloom.blur.radius", ConstantValueSource{2.5}}),
                "blur radius is valid");
        require(composition->parameters().insert(
                    {edge, "bloom.blur.edge", ConstantValueSource{std::int64_t{1}}}),
                "blur edge is valid");
        require(graph.addNode({node, type, {{"radius", radius}, {"edge", edge}}, 1}),
                "blur node adds");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge removed");
        require(graph.addEdge(
                    {kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{node, "input"}}),
                "image enters blur");
        require(graph.addEdge(
                    {EdgeId::fromRaw(960),
                     {node, "image"},
                     NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
                "blur enters Layer");
        expectations.expect(project.validate().ok(), "blur document validates");
        runtime::NodeDefinitionRegistry registry;
        require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
        registry.freeze();
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "blur graph compiles");
        if (!result.plan)
            continue;
        const auto operation =
            std::ranges::find_if(result.plan->operations(), [](const auto& value) {
                return std::holds_alternative<runtime::CompiledImageEffect>(value);
            });
        require(operation != result.plan->operations().end(), "blur operation exists");
        const auto* blur = std::get_if<runtime::SeparableBlurKernel>(
            &std::get<runtime::CompiledImageEffect>(*operation).kernel);
        expectations.expect(blur && blur->radius.id == radius &&
                                blur->edge == runtime::BlurEdgeMode::Clamp &&
                                blur->kind == (std::string_view(type) == "bloom.gaussian-blur"
                                                   ? runtime::SeparableBlurKind::Gaussian
                                                   : runtime::SeparableBlurKind::Box),
                            "typed blur lowering retains parameter identity, kind and edge policy");
    }
}

// Gaussian Blur's separate axes and per-channel radii: the kernel keeps the vertical radius, and a
// per-channel blur lowers to five blurs and four channel combines over the input. An animated
// vertical radius with per-channel radii on and separate axes off is read by nothing: it is not
// lowered, and its frames still render.
void testBlurExtensionLowering(Expectations& expectations) {
    using namespace document;
    const auto* definition = builtInNodeDefinitions().find("bloom.gaussian-blur", 1);
    // Radius and edge, then separate axes, the vertical radius, per-channel and the four channel
    // radii, then the effect mask's channel every effect carries.
    require(definition != nullptr && definition->parameters.size() == 10,
            "Gaussian Blur has its radius, edge, separate axes, vertical radius, per-channel and "
            "four channel radii");
    for (int scenario = 0; scenario != 4; ++scenario) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto node = NodeId::fromRaw(960);
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = 960;
        for (const auto& parameter : definition->parameters) {
            const auto id = ParameterId::fromRaw(next++);
            auto value = parameter.defaultValue;
            if (parameter.role == "radius")
                value = 6.0;
            if (parameter.role == "radiusY")
                value = 2.0;
            if (parameter.role == "separate")
                value = scenario == 1;
            if (parameter.role == "perChannel")
                value = scenario == 2 || scenario == 3;
            if (parameter.role == "redScale")
                value = 0.5;
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{value}}),
                    "blur parameter is valid");
            if (scenario == 3 && parameter.role == "radiusY") {
                const auto curveId = AnimationCurveId::fromRaw(965);
                require(composition->animationCurves().insert(ScalarAnimationCurve{
                            curveId,
                            {{KeyframeId::fromRaw(966), core::RationalTime::fromInteger(0), 0.0,
                              KeyframeInterpolation::Linear},
                             {KeyframeId::fromRaw(967), core::RationalTime::fromInteger(1), 9.0,
                              KeyframeInterpolation::Linear}}}),
                        "a vertical radius curve is publishable");
                require(composition->parameters().setSource(id, AnimationCurveSource{curveId}),
                        "the vertical radius animates");
            }
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({node, "bloom.gaussian-blur", std::move(bindings), 1}),
                "blur node adds");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge removed");
        require(graph.addEdge(
                    {kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{node, "input"}}),
                "image enters blur");
        require(graph.addEdge(
                    {EdgeId::fromRaw(960),
                     {node, "image"},
                     NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
                "blur enters Layer");
        expectations.expect(project.validate().ok(), "extended blur document validates");
        runtime::NodeDefinitionRegistry registry;
        require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
        registry.freeze();
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "extended blur graph compiles");
        if (!result.plan)
            continue;
        std::size_t blurs = 0;
        std::size_t combines = 0;
        const runtime::SeparableBlurKernel* first = nullptr;
        for (const auto& operation : result.plan->operations()) {
            const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
            if (effect == nullptr)
                continue;
            if (const auto* blur = std::get_if<runtime::SeparableBlurKernel>(&effect->kernel)) {
                ++blurs;
                first = first != nullptr ? first : blur;
            }
            combines += std::holds_alternative<runtime::ChannelCombineKernel>(effect->kernel);
        }
        if (scenario == 2 || scenario == 3) {
            expectations.expect(blurs == 5 && combines == 4,
                                "a per-channel blur is five blurs and four channel combines");
            if (scenario == 3) {
                bool curveRead = false;
                for (const auto& operation : result.plan->operations())
                    if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation))
                        if (const auto* blur =
                                std::get_if<runtime::SeparableBlurKernel>(&effect->kernel))
                            curveRead = curveRead || blur->separate ||
                                        !std::holds_alternative<double>(blur->radiusY.source);
                expectations.expect(!curveRead && result.plan->scalarCurves().empty(),
                                    "an animated vertical radius with per-channel radii and no "
                                    "separate axes is neither lowered nor compiled");
                // The frames render: the curve nothing reads does not refuse the plan.
                for (const std::int64_t frame : {0, 1}) {
                    runtime::EvaluationRequest request{core::RationalTime::fromInteger(frame),
                                                       result.plan->output(),
                                                       runtime::CompositionFormatResolution{}};
                    request.quality = runtime::EvaluationQuality::Reference;
                    request.colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene;
                    request.pixelStorageByteLimit = std::size_t{1} << 29U;
                    request.bypassOperationCache = true;
                    const auto evaluated =
                        runtime::CpuCompositionEvaluator{}.evaluate(result.plan, request, {});
                    expectations.expect(evaluated.frame() != nullptr,
                                        "a per-channel blur with an unread animated vertical "
                                        "radius renders frame " + std::to_string(frame));
                }
            }
        } else {
            expectations.expect(blurs == 1 && combines == 0 && first != nullptr &&
                                    first->separate == (scenario == 1),
                                "a blur is one operation, separate only when asked");
            const auto* vertical =
                first != nullptr ? std::get_if<double>(&first->radiusY.source) : nullptr;
            expectations.expect(scenario != 1 || (vertical != nullptr && *vertical == 2.0 &&
                                                  first->radiusY.id == ParameterId::fromRaw(963)),
                                "separate axes keep the vertical radius and its identity");
        }
    }
}

// A value a node does not read in its current mode is not lowered, so an animated one must not
// compile a curve nothing references (the plan would be refused). Every node whose mode or toggle
// decides which values it reads is built with every unread value animated, and its frames render.
void testUnreadAnimatedValuesRender(Expectations& expectations) {
    using namespace document;
    struct Case final {
        std::string_view type;
        std::vector<std::pair<std::string, ParameterValue>> constants;
        std::vector<std::string> animated;
    };
    const auto weights = std::vector<std::string>{"reds",  "yellows", "greens",
                                                  "cyans", "blues",   "magentas"};
    const auto with = [](std::vector<std::string> first, const std::vector<std::string>& rest) {
        first.insert(first.end(), rest.begin(), rest.end());
        return first;
    };
    std::vector<Case> cases;
    cases.push_back({kVignetteNodeType, {{"mode", kVignetteMultiply}}, {"exposure"}});
    cases.push_back({kVignetteNodeType, {{"mode", kVignetteExposure}}, {"amount"}});
    cases.push_back({kColorizeNodeType, {{"mode", kColorizeFill}},
                     with({"shadows", "midtones", "highlights", "midpoint", "repetitions", "phase"},
                          weights)});
    cases.push_back({kColorizeNodeType, {{"mode", kColorizeTint}},
                     with({"fillColor", "midtones", "midpoint", "repetitions", "phase"}, weights)});
    cases.push_back({kColorizeNodeType, {{"mode", kColorizeTritone}},
                     with({"fillColor", "repetitions", "phase"}, weights)});
    cases.push_back({kColorizeNodeType, {{"mode", kColorizeGradientMap}},
                     with({"fillColor", "shadows", "midtones", "highlights", "midpoint"}, weights)});
    cases.push_back({kColorizeNodeType, {{"mode", kColorizeBlackAndWhite}},
                     {"fillColor", "shadows", "midtones", "highlights", "midpoint", "repetitions",
                      "phase"}});
    {
        std::vector<std::string> sensorOnly;
        std::vector<std::string> filmOnly;
        for (const auto& spec : filmGrainParameters()) {
            if (spec.kind != FilmGrainParameterKind::Scalar)
                continue;
            if (spec.optional)
                sensorOnly.emplace_back(spec.role);
            if (spec.role == "shadows" || spec.role == "midtones" || spec.role == "highlights")
                filmOnly.emplace_back(spec.role);
        }
        cases.push_back({kFilmGrainNodeType, {{"mode", kFilmGrainFilm}}, sensorOnly});
        cases.push_back({kFilmGrainNodeType, {{"mode", kFilmGrainSensorNoise}}, filmOnly});
    }
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    for (const auto& item : cases) {
        const std::string label = std::string(item.type) + " with " +
                                  std::to_string(item.animated.size()) + " unread animated values";
        const auto* definition = builtInNodeDefinitions().find(item.type, 1);
        require(definition != nullptr, "the definition of a mode-dependent node exists");
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto node = NodeId::fromRaw(980);
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = 980;
        std::uint64_t curve = 3000;
        const auto keys = [&](const double from, const double to) {
            std::vector<ScalarKeyframe> result;
            result.push_back({KeyframeId::fromRaw(curve++), core::RationalTime::fromInteger(0),
                              from, KeyframeInterpolation::Linear});
            result.push_back({KeyframeId::fromRaw(curve++), core::RationalTime::fromInteger(1), to,
                              KeyframeInterpolation::Linear});
            return result;
        };
        for (const auto& parameter : definition->parameters) {
            const auto id = ParameterId::fromRaw(next++);
            auto value = parameter.defaultValue;
            for (const auto& [role, constant] : item.constants)
                if (parameter.role == role)
                    value = constant;
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{value}}),
                    "a mode-dependent parameter is valid");
            if (std::ranges::find(item.animated, parameter.role) != item.animated.end()) {
                const auto curveId = AnimationCurveId::fromRaw(curve++);
                if (const auto* scalar = std::get_if<double>(&value)) {
                    require(composition->animationCurves().insert(
                                ScalarAnimationCurve{curveId, keys(*scalar, *scalar * 0.5 + 0.1)}),
                            "a scalar curve is publishable");
                } else if (std::holds_alternative<core::Color4d>(value)) {
                    Color4AnimationCurve color{curveId, {}};
                    for (auto& component : color.components)
                        component.keyframes = keys(0.2, 0.6);
                    require(composition->animationCurves().insert(std::move(color)),
                            "a colour curve is publishable");
                } else {
                    require(false, "an animated unread value is a scalar or a colour");
                }
                require(composition->parameters().setSource(id, AnimationCurveSource{curveId}),
                        "the unread value animates");
            }
            bindings.push_back({parameter.role, id});
        }
        for (const auto& role : item.animated)
            require(std::ranges::any_of(bindings,
                                        [&](const auto& binding) { return binding.role == role; }),
                    "an animated role is a parameter of the node");
        require(graph.addNode({node, std::string(item.type), std::move(bindings), 1}),
                "a mode-dependent node adds");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge removed");
        require(graph.addEdge(
                    {kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{node, "input"}}),
                "image enters the node");
        require(graph.addEdge({EdgeId::fromRaw(980),
                               {node, "image"},
                               NodeInputRef{kFirstLayerNode,
                                            std::string(kLayerOutputContentInputPort)}}),
                "the node enters Layer");
                expectations.expect(project.validate().ok(), label + " validates");
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, label + " compiles");
        if (!result.plan)
            continue;
        expectations.expect(result.plan->scalarCurves().empty() &&
                                result.plan->color4Curves().empty(),
                            label + " compiles no curve");
        for (const std::int64_t frame : {0, 1}) {
            runtime::EvaluationRequest request{core::RationalTime::fromInteger(frame),
                                               result.plan->output(),
                                               runtime::CompositionFormatResolution{}};
            request.quality = runtime::EvaluationQuality::Reference;
            request.colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene;
            request.pixelStorageByteLimit = std::size_t{1} << 29U;
            request.bypassOperationCache = true;
            const auto evaluated =
                runtime::CpuCompositionEvaluator{}.evaluate(result.plan, request, {});
            expectations.expect(evaluated.frame() != nullptr,
                                label + " renders frame " + std::to_string(frame));
        }
    }
}

// Hue/Saturation's vibrance and Colorize reach the kernel; a node saved without them lowers to none.
void testHueSaturationExtensionLowering(Expectations& expectations) {
    using namespace document;
    const auto* definition = builtInNodeDefinitions().find("bloom.hue-saturation", 1);
    // Then the effect mask's channel every effect carries.
    require(definition != nullptr && definition->parameters.size() == 5,
            "Hue/Saturation has hue, saturation, vibrance and Colorize");
    for (int scenario = 0; scenario != 2; ++scenario) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto node = NodeId::fromRaw(960);
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = 960;
        for (const auto& parameter : definition->parameters) {
            if (scenario == 1 && (parameter.role == "vibrance" || parameter.role == "colorize"))
                continue; // a document saved before these existed
            const auto id = ParameterId::fromRaw(next++);
            auto value = parameter.defaultValue;
            if (parameter.role == "vibrance")
                value = 0.75;
            if (parameter.role == "colorize")
                value = true;
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{value}}),
                    "Hue/Saturation parameter is valid");
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({node, "bloom.hue-saturation", std::move(bindings), 1}),
                "Hue/Saturation node adds");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge removed");
        require(graph.addEdge(
                    {kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{node, "input"}}),
                "image enters Hue/Saturation");
        require(graph.addEdge(
                    {EdgeId::fromRaw(960),
                     {node, "image"},
                     NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
                "Hue/Saturation enters Layer");
        expectations.expect(project.validate().ok(), "Hue/Saturation document validates");
        runtime::NodeDefinitionRegistry registry;
        require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
        registry.freeze();
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "Hue/Saturation graph compiles");
        if (!result.plan)
            continue;
        const runtime::HueSaturationKernel* kernel = nullptr;
        for (const auto& operation : result.plan->operations())
            if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation))
                if (const auto* found = std::get_if<runtime::HueSaturationKernel>(&effect->kernel))
                    kernel = found;
        require(kernel != nullptr, "Hue/Saturation lowers to its kernel");
        const auto* vibrance = std::get_if<double>(&kernel->vibrance.source);
        expectations.expect(scenario == 0 ? (kernel->colorize && vibrance && *vibrance == 0.75)
                                          : (!kernel->colorize && vibrance && *vibrance == 0.0),
                            "vibrance and Colorize lower, and default to off when not saved");
    }
}

void testImageCombineChannelLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "combine built-ins register");
    registry.freeze();
    for (const auto* type : {"bloom.set-alpha", "bloom.multiply-alpha", "bloom.image-mix"}) {
        // Scenarios: 0 constant, 1 linked Integer, 2 linked frame number, 3 empty matte source,
        // 4 unconnected matte (#361), 5 constant inverted selector 7 = Green Inverted (#361).
        for (int scenario = 0; scenario != 6; ++scenario) {
            const bool mix = std::string_view(type) == "bloom.image-mix";
            if (mix && scenario != 3)
                continue;
            auto project = makeProject();
            auto* composition = project.findComposition(kCompositionId);
            auto& graph = composition->graph();
            const auto effectId = NodeId::fromRaw(900);
            const auto driverId = NodeId::fromRaw(910);
            const auto* definition = builtInNodeDefinitions().find(type, 1);
            require(definition != nullptr, "combine definition exists");
            std::vector<ParameterBinding> bindings;
            ParameterId channel;
            std::uint64_t next = 900;
            for (const auto& parameter : definition->parameters) {
                const auto id = ParameterId::fromRaw(next++);
                require(composition->parameters().insert(
                            {id, parameter.schemaKey, ConstantValueSource{parameter.defaultValue}}),
                        "combine parameter is valid");
                bindings.push_back({parameter.role, id});
                if (parameter.role == "channel")
                    channel = id;
            }
            require(graph.addNode({effectId, type, std::move(bindings), 1}),
                    "combine node is accepted");
            require(graph.eraseEdge(kFirstSourceEdge), "base source edge is removed");
            require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                                   NodeInputRef{effectId, "input"}}), "base feeds combine");
            if (scenario != 4)
                require(graph.addEdge({EdgeId::fromRaw(900), {kSecondSolidNode, "image"},
                                       NodeInputRef{effectId, mix ? "second" : "matte"}}),
                        "second image feeds combine");
            if (scenario == 5)
                require(composition->parameters().setSource(
                            channel, ConstantValueSource{std::int64_t{7}}),
                        "the closed matte selector admits an inverted channel");
            require(graph.addEdge({EdgeId::fromRaw(901), {effectId, "image"},
                                   NodeInputRef{kFirstLayerNode,
                                                std::string(kLayerOutputContentInputPort)}}),
                    "combine feeds layer");
            if (scenario == 1) {
                attachValueDriver(project, driverId, ParameterId::fromRaw(910),
                                  kIntegerValueNodeType, kIntegerValueParameterSchemaKey,
                                  std::int64_t{4}, channel);
            } else if (scenario == 2) {
                require(graph.addNode({driverId, std::string(kFrameNumberNodeType), {},
                                       kValueNodeSchemaVersion}), "frame driver is accepted");
                require(composition->parameters().setSource(
                            channel, DriverBindingSource{driverId, std::string(kResultPortName)}),
                        "frame driver connects to matte channel");
            } else if (scenario == 3) {
                composition->nodeLayout().at(kSecondSolidNode).muted = true;
            }
            expectations.expect(project.validate().ok(), "combine document remains valid");
            const auto result = compile(std::move(project), registry);
            expectations.expect(result.plan != nullptr, "constant, linked and empty-matte combine compiles");
            if (!result.plan)
                continue;
            const auto operation = std::ranges::find_if(result.plan->operations(),
                [effectId](const auto& item) {
                    const auto* effect = std::get_if<runtime::CompiledImageEffect>(&item);
                    return effect && effect->sourceNodeId == effectId;
                });
            require(operation != result.plan->operations().end(), "combine operation exists");
            const auto& effect = std::get<runtime::CompiledImageEffect>(*operation);
            const auto& kernel = std::get<runtime::ImageCombineKernel>(effect.kernel);
            expectations.expect(kernel.drivenChannel.has_value() == (scenario == 1 || scenario == 2),
                                "linked matte channel retains its value output");
            const auto index = runtime::OperationIndex::fromRaw(static_cast<std::size_t>(
                std::distance(result.plan->operations().begin(), operation)));
            expectations.expect(result.plan->operationTimeDependent(index) == (scenario == 2) &&
                                    result.plan->operationTimeDependent(result.plan->output()) ==
                                        (scenario == 2),
                                "only a time-dependent matte channel invalidates downstream frames");
            if (scenario == 5)
                expectations.expect(kernel.channel == render::ImageCoverageChannel::Green &&
                                        kernel.invert,
                                    "stored selector 7 lowers to the inverted green matte");
            else
                expectations.expect(!kernel.invert, "selectors 0-4 lower without invert");
            if (scenario == 3 || scenario == 4) {
                const auto* empty = effect.secondaryInput
                    ? std::get_if<runtime::CompiledMerge>(
                          &result.plan->operations()[effect.secondaryInput->value()])
                    : nullptr;
                expectations.expect(empty && empty->entries.empty(),
                                    "empty matte materializes a transparent image operation");
            }
        }
    }
}

// #336: Transform 2D lowers through the image-effect operation with its parameter identities,
// extent and filter; animated or driven transform values make it (and the output) time dependent.
void testTransform2DLowering(Expectations& expectations) {
    using namespace document;
    auto project = makeProject(singleLayerOptions());
    auto* composition = project.findComposition(kCompositionId);
    auto& graph = composition->graph();
    const auto effectId = NodeId::fromRaw(970);
    const auto* definition = builtInNodeDefinitions().find("bloom.transform-2d", 1);
    // Its six transform parameters, then (#484) its optional motion blur's, then skew and opacity.
    require(definition != nullptr &&
                definition->parameters.size() ==
                    6 + document::transformMotionBlurParameterDefinitions().size() + 3,
            "Transform 2D definition has six parameters, its motion blur's, then skew and opacity");
    std::vector<ParameterBinding> bindings;
    std::uint64_t next = 970;
    for (const auto& parameter : definition->parameters) {
        const auto id = ParameterId::fromRaw(next++);
        auto value = parameter.defaultValue;
        if (parameter.role == "translate")
            value = Vec2d{12.5, -4.0};
        if (parameter.role == "rotation")
            value = 30.0;
        if (parameter.role == "extent")
            value = std::int64_t{2};
        if (parameter.role == "skew")
            value = 20.0;
        if (parameter.role == "skewAxis")
            value = 15.0;
        if (parameter.role == "opacity")
            value = 0.5;
        require(composition->parameters().insert({id, parameter.schemaKey, ConstantValueSource{value}}),
                "Transform 2D parameter is valid");
        bindings.push_back({parameter.role, id});
    }
    require(graph.addNode({effectId, "bloom.transform-2d", std::move(bindings), 1}),
            "Transform 2D node is accepted");
    require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
    require(graph.addEdge(
                {kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{effectId, "input"}}),
            "source feeds Transform 2D");
    require(graph.addEdge({EdgeId::fromRaw(970), {effectId, "image"},
                           NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
            "Transform 2D feeds Layer");
    expectations.expect(project.validate().ok(), "Transform 2D graph is valid document truth");
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    const auto result = compile(std::move(project), registry);
    expectations.expect(result.plan != nullptr, "Transform 2D graph compiles");
    if (!result.plan)
        return;
    const auto effect = std::ranges::find_if(result.plan->operations(), [](const auto& op) {
        return std::holds_alternative<runtime::CompiledImageEffect>(op);
    });
    require(effect != result.plan->operations().end(), "Transform 2D lowers to an image effect");
    const auto* kernel = std::get_if<runtime::Transform2DKernel>(
        &std::get<runtime::CompiledImageEffect>(*effect).kernel);
    const auto* translate = kernel ? std::get_if<Vec2d>(&kernel->translate.source) : nullptr;
    expectations.expect(kernel && kernel->translate.id == ParameterId::fromRaw(970) &&
                            kernel->rotation.id == ParameterId::fromRaw(973) && translate &&
                            *translate == Vec2d{12.5, -4.0} &&
                            kernel->extent == runtime::Transform2DExtent::Display &&
                            kernel->filter == 0,
                        "Transform 2D lowering retains identities, values, extent and filter");
    const auto* skew = kernel ? std::get_if<double>(&kernel->skew.source) : nullptr;
    const auto* skewAxis = kernel ? std::get_if<double>(&kernel->skewAxis.source) : nullptr;
    const auto* opacity = kernel ? std::get_if<double>(&kernel->opacity.source) : nullptr;
    expectations.expect(skew && *skew == 20.0 && skewAxis && *skewAxis == 15.0 && opacity &&
                            *opacity == 0.5 && kernel->skew.id.value() != 0 &&
                            !kernel->plainSkewOpacity(),
                        "Transform 2D lowering retains skew, skew axis and opacity");
    const auto effectIndex =
        static_cast<std::size_t>(std::distance(result.plan->operations().begin(), effect));
    expectations.expect(
        !result.plan->operationTimeDependent(runtime::OperationIndex::fromRaw(effectIndex)),
        "constant Transform 2D values remain time invariant");
    auto animatedDefinition = result.plan->copyDefinition();
    std::get<runtime::Transform2DKernel>(
        std::get<runtime::CompiledImageEffect>(animatedDefinition.operations[effectIndex]).kernel)
        .rotation.source = runtime::ScalarCurveIndex::fromRaw(0);
    animatedDefinition.scalarCurves.push_back(runtime::CompiledScalarCurve{
        AnimationCurveId::fromRaw(970),
        {{KeyframeId::fromRaw(970), core::RationalTime::fromInteger(0), 0.0},
         {KeyframeId::fromRaw(971), core::RationalTime::fromInteger(1), 90.0}}});
    const auto animated =
        std::make_shared<const runtime::CompiledCompositionPlan>(std::move(animatedDefinition));
    expectations.expect(
        animated->operationTimeDependent(runtime::OperationIndex::fromRaw(effectIndex)) &&
            animated->operationTimeDependent(animated->output()),
        "animated Transform 2D rotation invalidates itself and the downstream output");
    auto drivenDefinition = result.plan->copyDefinition();
    std::get<runtime::Transform2DKernel>(
        std::get<runtime::CompiledImageEffect>(drivenDefinition.operations[effectIndex]).kernel)
        .translate.source = runtime::ValueOutputIndex::fromRaw(0);
    drivenDefinition.valueOperations = {{NodeId::fromRaw(980), runtime::ValueOutputIndex::fromRaw(0),
                                         2, runtime::CompiledValueTime{}}};
    drivenDefinition.valueOutputCount = 2;
    const auto driven =
        std::make_shared<const runtime::CompiledCompositionPlan>(std::move(drivenDefinition));
    expectations.expect(
        driven->operationTimeDependent(runtime::OperationIndex::fromRaw(effectIndex)) &&
            driven->operationTimeDependent(driven->output()),
        "driven Transform 2D translation invalidates itself and the downstream output");
}

// #337: a Mask node feeds Matte Blur, which feeds Image Mix's mask. Matte Blur lowers to the
// Gaussian Coverage blur of the Mask operation; muted, it passes the Mask through; with a muted
// (empty) Mask upstream it is empty too and Image Mix reads a transparent mask.
void testMatteBlurLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    const auto maskId = NodeId::fromRaw(985);
    const auto blurId = NodeId::fromRaw(986);
    const auto mixId = NodeId::fromRaw(987);
    for (int scenario = 0; scenario != 3; ++scenario) {
        auto project = makeProject();
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        std::uint64_t next = 985;
        const auto add = [&](const NodeId id, const std::string_view type) {
            const auto* definition = builtInNodeDefinitions().find(type, 1);
            require(definition != nullptr, "definition exists");
            std::vector<ParameterBinding> bindings;
            for (const auto& parameter : definition->parameters) {
                const auto parameterId = ParameterId::fromRaw(next++);
                auto value = parameter.defaultValue;
                if (parameter.schemaKey == "bloom.blur.radius")
                    value = 3.5;
                if (parameter.schemaKey == "bloom.blur.edge")
                    value = std::int64_t{1};
                require(composition->parameters().insert(
                            {parameterId, parameter.schemaKey, ConstantValueSource{value}}),
                        "parameter is valid");
                bindings.push_back({parameter.role, parameterId});
            }
            require(graph.addNode({id, std::string(type), std::move(bindings), 1}),
                    "node is accepted");
        };
        add(maskId, kMaskNodeType);
        add(blurId, "bloom.matte-blur");
        add(mixId, "bloom.image-mix");
        require(graph.eraseEdge(kFirstSourceEdge), "base source edge is removed");
        require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                               NodeInputRef{mixId, "input"}}),
                "base feeds Image Mix");
        require(graph.addEdge({EdgeId::fromRaw(985), {kSecondSolidNode, "image"},
                               NodeInputRef{mixId, "second"}}),
                "second feeds Image Mix");
        require(graph.addEdge({EdgeId::fromRaw(986), {maskId, std::string(kMaskNodeOutputPort)},
                               NodeInputRef{blurId, "input"}}),
                "Mask feeds Matte Blur");
        require(graph.addEdge({EdgeId::fromRaw(987), {blurId, "mask"}, NodeInputRef{mixId, "mask"}}),
                "Matte Blur feeds Image Mix's mask");
        require(graph.addEdge({EdgeId::fromRaw(988), {mixId, "image"},
                               NodeInputRef{kFirstLayerNode,
                                            std::string(kLayerOutputContentInputPort)}}),
                "Image Mix feeds Layer");
        if (scenario == 1)
            composition->nodeLayout()[blurId].muted = true;
        if (scenario == 2)
            composition->nodeLayout()[maskId].muted = true;
        expectations.expect(project.validate().ok(), "Matte Blur graph is valid document truth");
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "Matte Blur graph compiles");
        if (!result.plan)
            continue;
        const auto& operations = result.plan->operations();
        const auto find = [&](const NodeId id, const auto predicate) {
            return std::ranges::find_if(operations, [&](const auto& item) {
                const auto* effect = std::get_if<runtime::CompiledImageEffect>(&item);
                return effect && effect->sourceNodeId == id && predicate(*effect);
            });
        };
        const auto mix = find(mixId, [](const auto&) { return true; });
        require(mix != operations.end(), "Image Mix lowers");
        const auto& mixEffect = std::get<runtime::CompiledImageEffect>(*mix);
        require(mixEffect.maskInput.has_value(), "Image Mix keeps its connected mask");
        const auto& maskSource = operations[mixEffect.maskInput->value()];
        if (scenario == 2) {
            const auto* empty = std::get_if<runtime::CompiledMerge>(&maskSource);
            expectations.expect(empty && empty->entries.empty(),
                                "an empty Mask through Matte Blur is a transparent mask");
            continue;
        }
        const auto masked = find(maskId, [](const auto& effect) {
            return std::holds_alternative<runtime::LayerMaskKernel>(effect.kernel);
        });
        require(masked != operations.end(), "Mask node lowers");
        const auto maskedIndex =
            static_cast<std::size_t>(std::distance(operations.begin(), masked));
        if (scenario == 1) {
            expectations.expect(mixEffect.maskInput->value() == maskedIndex &&
                                    find(blurId, [](const auto&) { return true; }) ==
                                        operations.end(),
                                "a muted Matte Blur passes its Mask through unchanged");
            continue;
        }
        const auto* blurEffect = std::get_if<runtime::CompiledImageEffect>(&maskSource);
        const auto* kernel =
            blurEffect ? std::get_if<runtime::SeparableBlurKernel>(&blurEffect->kernel) : nullptr;
        const auto* radius = kernel ? std::get_if<double>(&kernel->radius.source) : nullptr;
        expectations.expect(blurEffect && blurEffect->sourceNodeId == blurId && kernel &&
                                blurEffect->input.value() == maskedIndex &&
                                kernel->kind == runtime::SeparableBlurKind::Gaussian &&
                                kernel->channels == runtime::BlurChannels::Coverage &&
                                kernel->edge == runtime::BlurEdgeMode::Clamp && radius &&
                                *radius == 3.5,
                            "Matte Blur lowers to the Gaussian Coverage blur of the Mask");
    }
}

// #338: Matte Erode and Matte Dilate lower exactly as Matte Blur does (Mask node -> filter -> Image
// Mix mask), to the Erode/Dilate Coverage kernel; muted they pass the Mask through, and an empty
// upstream Mask empties them.
void testMatteMorphologyLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    const auto maskId = NodeId::fromRaw(985);
    const auto filterId = NodeId::fromRaw(986);
    const auto mixId = NodeId::fromRaw(987);
    for (const auto* type : {"bloom.matte-erode", "bloom.matte-dilate"}) {
        const auto kind = std::string_view(type) == "bloom.matte-erode"
                              ? runtime::SeparableBlurKind::Erode
                              : runtime::SeparableBlurKind::Dilate;
        for (int scenario = 0; scenario != 3; ++scenario) {
            auto project = makeProject();
            auto* composition = project.findComposition(kCompositionId);
            auto& graph = composition->graph();
            std::uint64_t next = 985;
            const auto add = [&](const NodeId id, const std::string_view nodeType) {
                const auto* definition = builtInNodeDefinitions().find(nodeType, 1);
                require(definition != nullptr, "definition exists");
                std::vector<ParameterBinding> bindings;
                for (const auto& parameter : definition->parameters) {
                    const auto parameterId = ParameterId::fromRaw(next++);
                    auto value = parameter.defaultValue;
                    if (parameter.schemaKey == "bloom.blur.radius")
                        value = 2.5;
                    if (parameter.schemaKey == "bloom.blur.edge")
                        value = std::int64_t{1};
                    require(composition->parameters().insert(
                                {parameterId, parameter.schemaKey, ConstantValueSource{value}}),
                            "parameter is valid");
                    bindings.push_back({parameter.role, parameterId});
                }
                require(graph.addNode({id, std::string(nodeType), std::move(bindings), 1}),
                        "node is accepted");
            };
            add(maskId, kMaskNodeType);
            add(filterId, type);
            add(mixId, "bloom.image-mix");
            require(graph.eraseEdge(kFirstSourceEdge), "base source edge is removed");
            require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                                   NodeInputRef{mixId, "input"}}),
                    "base feeds Image Mix");
            require(graph.addEdge({EdgeId::fromRaw(985), {kSecondSolidNode, "image"},
                                   NodeInputRef{mixId, "second"}}),
                    "second feeds Image Mix");
            require(graph.addEdge({EdgeId::fromRaw(986), {maskId, std::string(kMaskNodeOutputPort)},
                                   NodeInputRef{filterId, "input"}}),
                    "Mask feeds the matte filter");
            require(graph.addEdge(
                        {EdgeId::fromRaw(987), {filterId, "mask"}, NodeInputRef{mixId, "mask"}}),
                    "the matte filter feeds Image Mix's mask");
            require(graph.addEdge({EdgeId::fromRaw(988), {mixId, "image"},
                                   NodeInputRef{kFirstLayerNode,
                                                std::string(kLayerOutputContentInputPort)}}),
                    "Image Mix feeds Layer");
            if (scenario == 1)
                composition->nodeLayout()[filterId].muted = true;
            if (scenario == 2)
                composition->nodeLayout()[maskId].muted = true;
            expectations.expect(project.validate().ok(), "matte filter graph is valid");
            const auto result = compile(std::move(project), registry);
            expectations.expect(result.plan != nullptr, "matte filter graph compiles");
            if (!result.plan)
                continue;
            const auto& operations = result.plan->operations();
            const auto find = [&](const NodeId id, const auto predicate) {
                return std::ranges::find_if(operations, [&](const auto& item) {
                    const auto* effect = std::get_if<runtime::CompiledImageEffect>(&item);
                    return effect && effect->sourceNodeId == id && predicate(*effect);
                });
            };
            const auto mix = find(mixId, [](const auto&) { return true; });
            require(mix != operations.end(), "Image Mix lowers");
            const auto& mixEffect = std::get<runtime::CompiledImageEffect>(*mix);
            require(mixEffect.maskInput.has_value(), "Image Mix keeps its connected mask");
            const auto& maskSource = operations[mixEffect.maskInput->value()];
            if (scenario == 2) {
                const auto* empty = std::get_if<runtime::CompiledMerge>(&maskSource);
                expectations.expect(empty && empty->entries.empty(),
                                    "an empty Mask through Matte Erode/Dilate stays empty");
                continue;
            }
            const auto masked = find(maskId, [](const auto& effect) {
                return std::holds_alternative<runtime::LayerMaskKernel>(effect.kernel);
            });
            require(masked != operations.end(), "Mask node lowers");
            const auto maskedIndex =
                static_cast<std::size_t>(std::distance(operations.begin(), masked));
            if (scenario == 1) {
                expectations.expect(mixEffect.maskInput->value() == maskedIndex &&
                                        find(filterId, [](const auto&) { return true; }) ==
                                            operations.end(),
                                    "a muted Matte Erode/Dilate passes its Mask through");
                continue;
            }
            const auto* filterEffect = std::get_if<runtime::CompiledImageEffect>(&maskSource);
            const auto* kernel = filterEffect
                                     ? std::get_if<runtime::SeparableBlurKernel>(&filterEffect->kernel)
                                     : nullptr;
            const auto* radius = kernel ? std::get_if<double>(&kernel->radius.source) : nullptr;
            expectations.expect(filterEffect && filterEffect->sourceNodeId == filterId && kernel &&
                                    filterEffect->input.value() == maskedIndex &&
                                    kernel->kind == kind &&
                                    kernel->channels == runtime::BlurChannels::Coverage &&
                                    kernel->edge == runtime::BlurEdgeMode::Clamp && radius &&
                                    *radius == 2.5,
                                "Matte Erode/Dilate lowers to its Coverage morphology kernel");
        }
    }
}

// #338: an Image keyed by Luma Key feeds Image Mix's mask. It lowers to the LumaKeyKernel of the
// image; muted it is an empty Mask (an Image never passes through as a Mask); an empty upstream
// Image keys to an empty Mask.
void testLumaKeyLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    const auto keyId = NodeId::fromRaw(990);
    const auto mixId = NodeId::fromRaw(991);
    for (int scenario = 0; scenario != 3; ++scenario) {
        auto project = makeProject();
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        std::uint64_t next = 990;
        const auto add = [&](const NodeId id, const std::string_view nodeType) {
            const auto* definition = builtInNodeDefinitions().find(nodeType, 1);
            require(definition != nullptr, "definition exists");
            std::vector<ParameterBinding> bindings;
            for (const auto& parameter : definition->parameters) {
                const auto parameterId = ParameterId::fromRaw(next++);
                auto value = parameter.defaultValue;
                if (parameter.schemaKey == "bloom.luma-key.low")
                    value = 0.25;
                if (parameter.schemaKey == "bloom.luma-key.high")
                    value = 0.75;
                if (parameter.schemaKey == "bloom.luma-key.invert")
                    value = true;
                require(composition->parameters().insert(
                            {parameterId, parameter.schemaKey, ConstantValueSource{value}}),
                        "parameter is valid");
                bindings.push_back({parameter.role, parameterId});
            }
            require(graph.addNode({id, std::string(nodeType), std::move(bindings), 1}),
                    "node is accepted");
        };
        add(keyId, "bloom.luma-key");
        add(mixId, "bloom.image-mix");
        require(graph.eraseEdge(kFirstSourceEdge), "base source edge is removed");
        require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                               NodeInputRef{mixId, "input"}}),
                "base feeds Image Mix");
        require(graph.addEdge({EdgeId::fromRaw(990), {kSecondSolidNode, "image"},
                               NodeInputRef{mixId, "second"}}),
                "second feeds Image Mix");
        require(graph.addEdge({EdgeId::fromRaw(991), {kSecondSolidNode, "image"},
                               NodeInputRef{keyId, "input"}}),
                "an Image feeds Luma Key");
        require(graph.addEdge({EdgeId::fromRaw(992), {keyId, "mask"}, NodeInputRef{mixId, "mask"}}),
                "Luma Key feeds Image Mix's mask");
        require(graph.addEdge({EdgeId::fromRaw(993), {mixId, "image"},
                               NodeInputRef{kFirstLayerNode,
                                            std::string(kLayerOutputContentInputPort)}}),
                "Image Mix feeds Layer");
        if (scenario == 1)
            composition->nodeLayout()[keyId].muted = true;
        if (scenario == 2)
            composition->nodeLayout()[kSecondSolidNode].muted = true;
        expectations.expect(project.validate().ok(), "Luma Key graph is valid document truth");
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "Luma Key graph compiles");
        if (!result.plan)
            continue;
        const auto& operations = result.plan->operations();
        const auto mix = std::ranges::find_if(operations, [&](const auto& item) {
            const auto* effect = std::get_if<runtime::CompiledImageEffect>(&item);
            return effect && effect->sourceNodeId == mixId;
        });
        require(mix != operations.end(), "Image Mix lowers");
        const auto& mixEffect = std::get<runtime::CompiledImageEffect>(*mix);
        require(mixEffect.maskInput.has_value(), "Image Mix keeps its connected mask");
        const auto& maskSource = operations[mixEffect.maskInput->value()];
        if (scenario != 0) {
            const auto* empty = std::get_if<runtime::CompiledMerge>(&maskSource);
            expectations.expect(empty && empty->entries.empty() &&
                                    std::ranges::none_of(operations,
                                                         [&](const auto& item) {
                                                             const auto* effect = std::get_if<
                                                                 runtime::CompiledImageEffect>(
                                                                 &item);
                                                             return effect &&
                                                                    effect->sourceNodeId == keyId;
                                                         }),
                                scenario == 1
                                    ? "a muted Luma Key is an empty Mask, never its Image"
                                    : "an empty Image keys to an empty Mask");
            continue;
        }
        const auto* keyEffect = std::get_if<runtime::CompiledImageEffect>(&maskSource);
        const auto* kernel =
            keyEffect ? std::get_if<runtime::LumaKeyKernel>(&keyEffect->kernel) : nullptr;
        const auto* low = kernel ? std::get_if<double>(&kernel->low.source) : nullptr;
        const auto* high = kernel ? std::get_if<double>(&kernel->high.source) : nullptr;
        const auto* source = keyEffect && keyEffect->input.value() < operations.size()
                                 ? std::get_if<runtime::CompiledSolid>(
                                       &operations[keyEffect->input.value()])
                                 : nullptr;
        expectations.expect(keyEffect && keyEffect->sourceNodeId == keyId && kernel && low &&
                                high && *low == 0.25 && *high == 0.75 && kernel->invert &&
                                source && source->sourceNodeId == kSecondSolidNode,
                            "Luma Key lowers to the LumaKeyKernel of its Image");
    }
}

// #490: an Image Preview never enters the composition's plan, and the Viewer-only inspection plan
// it is previewed through is exactly the inspected node's output -- here a Gaussian Blur's image --
// without the layer stack or any other part of the composition.
// #340: Levels, Curves and Hue/Saturation between a Solid and its Layer lower to their kernels in
// channel-major order with every parameter identity and value (#493: Curves schema 2 point
// lists); a driven Levels value is a value-graph operand; a muted grade passes its Image through.
void testColorGradeLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    for (const auto type : {kLevelsNodeType, kCurvesNodeType, kHueSaturationNodeType,
                            kGradeNodeType}) {
        for (const bool muted : {false, true}) {
            auto project = makeProject(singleLayerOptions());
            auto* composition = project.findComposition(kCompositionId);
            auto& graph = composition->graph();
            const auto effectId = NodeId::fromRaw(995);
            const auto* definition = builtInNodeDefinitions().find(
                type, type == kCurvesNodeType ? kCurvesNodeSchemaVersion : 1U);
            require(definition != nullptr, "grade definition exists");
            std::vector<ParameterBinding> bindings;
            std::uint64_t next = 2000;
            for (const auto& parameter : definition->parameters) {
                auto value = parameter.defaultValue;
                if (parameter.schemaKey == "bloom.levels.red.gamma")
                    value = 2.2;
                if (parameter.schemaKey == "bloom.levels.alpha.output-white")
                    value = 0.5;
                if (parameter.schemaKey == kLevelsClampSchemaKey ||
                    parameter.schemaKey == kCurvesClampSchemaKey)
                    value = true;
                if (parameter.schemaKey == gradeCurveSchemaKey(GradeCurve::Blue))
                    value = gradeCurvePath({{0.0, 0.0}, {0.5, 0.8}, {1.0, 1.0}});
                if (parameter.schemaKey == gradeCurveSchemaKey(GradeCurve::HueVsLuma))
                    value = gradeCurvePath({{0.25, 1.5}});
                if (parameter.schemaKey == kHueSaturationHueSchemaKey)
                    value = -45.0;
                if (parameter.schemaKey == kHueSaturationSaturationSchemaKey)
                    value = 1.5;
                if (parameter.schemaKey == gradeWheelSchemaKey(GradeWheel::Gain, 2))
                    value = 1.25;
                if (parameter.schemaKey == gradePrimarySchemaKey(GradePrimary::Exposure))
                    value = -0.5;
                if (parameter.schemaKey == gradeCurveSchemaKey(GradeCurve::Red)) {
                    // #498: a keyed curve is a whole-value Path curve, which the compiler accepts
                    // as the Curves parameter's source like any other animated parameter.
                    const auto curveId = AnimationCurveId::fromRaw(7000);
                    require(composition->animationCurves().insert(PathAnimationCurve{
                                curveId,
                                {{KeyframeId::fromRaw(7001), core::RationalTime::fromInteger(0),
                                  gradeCurvePath({{0.0, 0.0}, {1.0, 1.0}})},
                                 {KeyframeId::fromRaw(7002), core::RationalTime::fromInteger(1),
                                  gradeCurvePath({{0.0, 0.0}, {0.5, 0.7}, {1.0, 1.0}})}}}),
                            "the keyed Red curve is publishable");
                    require(composition->parameters().insert(
                                {ParameterId::fromRaw(next), parameter.schemaKey,
                                 AnimationCurveSource{curveId, value}}),
                            "the keyed Red curve parameter is valid");
                    bindings.push_back({parameter.role, ParameterId::fromRaw(next++)});
                    continue;
                }
                require(composition->parameters().insert({ParameterId::fromRaw(next),
                                                          parameter.schemaKey,
                                                          ConstantValueSource{value}}),
                        "grade parameter is valid");
                bindings.push_back({parameter.role, ParameterId::fromRaw(next++)});
            }
            require(graph.addNode({effectId, std::string(type), std::move(bindings),
                                   definition->key.schemaVersion}),
                    "grade node is accepted");
            require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
            require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                                   NodeInputRef{effectId, "input"}}),
                    "source feeds the grade");
            require(graph.addEdge({EdgeId::fromRaw(995), {effectId, "image"},
                                   NodeInputRef{kFirstLayerNode,
                                                std::string(kLayerOutputContentInputPort)}}),
                    "grade feeds Layer");
            if (muted)
                composition->nodeLayout()[effectId].muted = true;
            expectations.expect(project.validate().ok(), "grade graph is valid document truth");
            const auto result = compile(std::move(project), registry);
            expectations.expect(result.plan != nullptr, "grade graph compiles");
            if (!result.plan)
                continue;
            const auto effect = std::ranges::find_if(result.plan->operations(), [&](const auto& op) {
                const auto* item = std::get_if<runtime::CompiledImageEffect>(&op);
                return item && item->sourceNodeId == effectId;
            });
            if (muted) {
                expectations.expect(effect == result.plan->operations().end(),
                                    "a muted grade passes its Image through");
                continue;
            }
            require(effect != result.plan->operations().end(), "grade lowers to an image effect");
            const auto& kernel = std::get<runtime::CompiledImageEffect>(*effect).kernel;
            std::vector<ParameterId> ids;
            runtime::forEachColorGradeOperand(kernel,
                                              [&](const auto& operand) { ids.push_back(operand.id); });
            // #383: the effect mask's channel selector is the last parameter, never an operand.
            // Levels and Curves have a clamp switch, and Hue/Saturation its Colorize switch, that
            // are not operands.
            bool ordered = ids.size() + (type == kGradeNodeType ? 0U : 1U) + 1U ==
                           definition->parameters.size();
            for (std::size_t i = 0; ordered && i < ids.size(); ++i)
                ordered = ids[i] == ParameterId::fromRaw(2000 + i);
            expectations.expect(ordered, "grade operands keep their channel-major identities");
            if (const auto* levels = std::get_if<runtime::LevelsKernel>(&kernel)) {
                const auto* gamma = std::get_if<double>(&levels->channels[1].gamma.source);
                const auto* white = std::get_if<double>(&levels->channels[4].outputWhite.source);
                expectations.expect(levels->clamp && gamma && *gamma == 2.2 && white &&
                                        *white == 0.5,
                                    "Levels lowers its values and clamp");
            } else if (const auto* curves = std::get_if<runtime::CurvesKernel>(&kernel)) {
                const auto* blue = std::get_if<PathValue>(
                    &curves->curves[gradeCurveIndex(GradeCurve::Blue)].source);
                const auto* hueLuma = std::get_if<PathValue>(
                    &curves->curves[gradeCurveIndex(GradeCurve::HueVsLuma)].source);
                const auto* master = std::get_if<PathValue>(&curves->curves[0].source);
                const auto* hueHue = std::get_if<PathValue>(
                    &curves->curves[gradeCurveIndex(GradeCurve::HueVsHue)].source);
                expectations.expect(
                    curves->clamp && blue &&
                        *blue == gradeCurvePath({{0.0, 0.0}, {0.5, 0.8}, {1.0, 1.0}}) && hueLuma &&
                        *hueLuma == gradeCurvePath({{0.25, 1.5}}) && master &&
                        *master == defaultGradeCurve(GradeCurve::Master) && hueHue &&
                        hueHue->firstContour().anchors.empty(),
                    "Curves lowers its point lists, hue curves and clamp");
                const auto* red = std::get_if<runtime::CompiledPathCurve>(
                    &curves->curves[gradeCurveIndex(GradeCurve::Red)].source);
                expectations.expect(red && red->keyframes.size() == 2,
                                    "a keyed Curves curve lowers to its compiled Path curve");
            } else if (const auto* grade = std::get_if<runtime::GradeKernel>(&kernel)) {
                // #494: gain green is the third gain value; exposure the last operand.
                const auto* green = std::get_if<double>(&grade->values[10].source);
                const auto* exposure = std::get_if<double>(&grade->values[21].source);
                const auto* pivot = std::get_if<double>(&grade->values[19].source);
                expectations.expect(green && *green == 1.25 && exposure && *exposure == -0.5 &&
                                        pivot && *pivot == kDefaultGradePivot,
                                    "Grade lowers its wheels and primaries in order");
            } else {
                const auto* hue = std::get_if<runtime::HueSaturationKernel>(&kernel);
                const auto* h = hue ? std::get_if<double>(&hue->hue.source) : nullptr;
                const auto* sat = hue ? std::get_if<double>(&hue->saturation.source) : nullptr;
                expectations.expect(h && *h == -45.0 && sat && *sat == 1.5,
                                    "Hue/Saturation lowers its hue and saturation");
            }
        }
    }
}

// #479: Separate Channels lowers one Channel Select per used output; Combine Channels lowers a
// Channel Select of its first connected input then one Channel Combine per further input (alpha
// last, Straight premultiplying only in the last), folding inputs that read the same image. A
// muted Separate passes its Image through every output, a Combine with no input is empty, and an
// Image Preview of one Separate output inspects exactly that output's operation.
void testImageChannelLowering(Expectations& expectations) {
    using namespace document;
    using runtime::ImageChannelSource;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    const auto separateId = NodeId::fromRaw(1990);
    const auto combineId = NodeId::fromRaw(1991);
    enum class Wiring { Separated, SameImage, AlphaOnly, None };
    const auto build = [&](const Wiring wiring, const std::int64_t mode, const bool muteSeparate,
                           const bool muteCombine) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        std::uint64_t next = 2100;
        std::vector<ParameterBinding> bindings;
        const auto* definition = builtInNodeDefinitions().find(kCombineChannelsNodeType, 1);
        require(definition != nullptr, "Combine Channels definition exists");
        for (const auto& parameter : definition->parameters) {
            auto value = parameter.defaultValue;
            if (parameter.role == kCombineChannelsAlphaModeRole)
                value = mode;
            if (parameter.role == "alphaSource" && wiring == Wiring::SameImage)
                value = kImageChannelSourceAlpha;
            if (parameter.role == "alphaSource" && wiring == Wiring::AlphaOnly)
                value = kImageChannelSourceLuma;
            require(
                composition->parameters().insert(
                    {ParameterId::fromRaw(next), parameter.schemaKey, ConstantValueSource{value}}),
                "Combine Channels parameter is valid");
            bindings.push_back({parameter.role, ParameterId::fromRaw(next++)});
        }
        require(graph.addNode({separateId, std::string(kSeparateChannelsNodeType), {}, 1}) &&
                    graph.addNode(
                        {combineId, std::string(kCombineChannelsNodeType), std::move(bindings), 1}),
                "channel nodes are accepted");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
        std::uint64_t edge = 1990;
        require(
            graph.addEdge(
                {kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{separateId, "input"}}),
            "source feeds Separate Channels");
        for (std::size_t slot = 0; slot < kImageChannelCount; ++slot) {
            const auto port = std::string(kImageChannelPortNames[slot]);
            if (wiring == Wiring::None || (wiring == Wiring::AlphaOnly && slot != 3))
                continue;
            const OutputPortRef source = wiring == Wiring::Separated
                                             ? OutputPortRef{separateId, port}
                                             : OutputPortRef{kFirstSolidNode, "image"};
            require(graph.addEdge({EdgeId::fromRaw(edge++), source, NodeInputRef{combineId, port}}),
                    "an input feeds Combine Channels");
        }
        require(graph.addEdge(
                    {EdgeId::fromRaw(edge++),
                     {combineId, "image"},
                     NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
                "Combine Channels feeds the Layer");
        if (muteSeparate)
            composition->nodeLayout()[separateId].muted = true;
        if (muteCombine)
            composition->nodeLayout()[combineId].muted = true;
        expectations.expect(project.validate().ok(), "channel graph is valid document truth");
        return project;
    };
    const auto effectsOf = [](const runtime::CompiledCompositionPlan& plan, const NodeId node) {
        std::vector<runtime::CompiledImageEffect> effects;
        for (const auto& operation : plan.operations())
            if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
                effect && effect->sourceNodeId == node)
                effects.push_back(*effect);
        return effects;
    };
    const auto grey = [](const ImageChannelSource c) {
        return runtime::ImageEffectKernel{
            runtime::ChannelSelectKernel{{c, c, c, ImageChannelSource::One}, false}};
    };
    for (const auto mode : {kCombineChannelsPremultiplied, kCombineChannelsStraight}) {
        const auto result = compile(build(Wiring::Separated, mode, false, false), registry);
        expectations.expect(result.plan != nullptr, "Separate into Combine compiles");
        if (!result.plan)
            continue;
        const auto separate = effectsOf(*result.plan, separateId);
        const auto combine = effectsOf(*result.plan, combineId);
        expectations.expect(separate.size() == 4 &&
                                separate[0].kernel == grey(ImageChannelSource::Red) &&
                                separate[1].kernel == grey(ImageChannelSource::Green) &&
                                separate[2].kernel == grey(ImageChannelSource::Blue) &&
                                separate[3].kernel == grey(ImageChannelSource::Alpha) &&
                                separate[0].input == separate[3].input,
                            "Separate Channels lowers one grey Channel Select per used output");
        const bool straight = mode == kCombineChannelsStraight;
        expectations.expect(
            combine.size() == 4 &&
                combine[0].kernel == runtime::ImageEffectKernel{runtime::ChannelSelectKernel{
                                         {ImageChannelSource::Red, ImageChannelSource::Zero,
                                          ImageChannelSource::Zero, ImageChannelSource::One},
                                         false}} &&
                combine[1].kernel == runtime::ImageEffectKernel{runtime::ChannelCombineKernel{
                                         1, ImageChannelSource::Green, false}} &&
                combine[2].kernel == runtime::ImageEffectKernel{runtime::ChannelCombineKernel{
                                         2, ImageChannelSource::Blue, false}} &&
                combine[3].kernel == runtime::ImageEffectKernel{runtime::ChannelCombineKernel{
                                         3, ImageChannelSource::Red, straight}} &&
                combine[1].secondaryInput && combine[3].secondaryInput &&
                *combine[1].secondaryInput != *combine[3].secondaryInput,
            "Combine Channels selects its base then sets each further slot, alpha last");
    }
    {
        const auto result = compile(
            build(Wiring::SameImage, kCombineChannelsPremultiplied, false, false), registry);
        const auto combine = result.plan ? effectsOf(*result.plan, combineId)
                                         : std::vector<runtime::CompiledImageEffect>{};
        expectations.expect(combine.size() == 1 &&
                                combine[0].kernel ==
                                    runtime::ImageEffectKernel{runtime::ChannelSelectKernel{}} &&
                                effectsOf(*result.plan, separateId).empty(),
                            "inputs reading the same image fold into one Channel Select");
    }
    {
        const auto result =
            compile(build(Wiring::AlphaOnly, kCombineChannelsStraight, false, false), registry);
        const auto combine = result.plan ? effectsOf(*result.plan, combineId)
                                         : std::vector<runtime::CompiledImageEffect>{};
        expectations.expect(combine.size() == 1 &&
                                combine[0].kernel ==
                                    runtime::ImageEffectKernel{runtime::ChannelSelectKernel{
                                        {ImageChannelSource::Zero, ImageChannelSource::Zero,
                                         ImageChannelSource::Zero, ImageChannelSource::Luma},
                                        true}},
                            "unconnected colour inputs are zero and a lone input is the base");
    }
    {
        const auto result =
            compile(build(Wiring::None, kCombineChannelsPremultiplied, false, false), registry);
        expectations.expect(result.plan != nullptr && effectsOf(*result.plan, combineId).empty(),
                            "a Combine Channels with no input is an empty image");
    }
    {
        const auto result =
            compile(build(Wiring::Separated, kCombineChannelsPremultiplied, true, false), registry);
        const auto combine = result.plan ? effectsOf(*result.plan, combineId)
                                         : std::vector<runtime::CompiledImageEffect>{};
        expectations.expect(result.plan != nullptr && effectsOf(*result.plan, separateId).empty() &&
                                combine.size() == 1 &&
                                combine[0].kernel ==
                                    runtime::ImageEffectKernel{runtime::ChannelSelectKernel{
                                        {ImageChannelSource::Red, ImageChannelSource::Green,
                                         ImageChannelSource::Blue, ImageChannelSource::Red},
                                        false}},
                            "a muted Separate passes its Image through every output");
        const auto mutedCombine =
            compile(build(Wiring::Separated, kCombineChannelsPremultiplied, false, true), registry);
        expectations.expect(mutedCombine.plan != nullptr &&
                                effectsOf(*mutedCombine.plan, combineId).empty() &&
                                effectsOf(*mutedCombine.plan, separateId).size() == 1,
                            "a muted Combine passes its red input through");
    }
    document::Document document(
        build(Wiring::Separated, kCombineChannelsPremultiplied, false, false));
    runtime::SnapshotCompiler compiler(registry);
    const auto inspected =
        compiler.compile({.snapshot = document.snapshot(),
                          .compositionId = kCompositionId,
                          .parameterOverrides = {},
                          .channelInspection = runtime::ChannelInspection{separateId, "blue", {}}},
                         runtime::CancellationToken{});
    expectations.expect(inspected.plan != nullptr, "a Separate Channels output can be inspected");
    if (!inspected.plan)
        return;
    const auto& operations = inspected.plan->operations();
    const auto* output = std::get_if<runtime::CompiledCompositionOutput>(
        &operations[inspected.plan->output().value()]);
    const auto* merge =
        output ? std::get_if<runtime::CompiledMerge>(&operations[output->input.value()]) : nullptr;
    const auto* shown = merge && merge->entries.size() == 1
                            ? std::get_if<runtime::CompiledImageEffect>(
                                  &operations[merge->entries.front().input.value()])
                            : nullptr;
    expectations.expect(shown != nullptr && shown->sourceNodeId == separateId &&
                            shown->kernel == grey(ImageChannelSource::Blue),
                        "the inspection shows the inspected output's own Channel Select");
}

void testImagePreviewInspection(Expectations& expectations) {
    using namespace document;
    const auto blurNode = NodeId::fromRaw(970);
    const auto previewNode = NodeId::fromRaw(971);
    const auto build = [&](const bool withPreview) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto radius = ParameterId::fromRaw(970);
        const auto edge = ParameterId::fromRaw(971);
        require(composition->parameters().insert(
                    {radius, "bloom.blur.radius", ConstantValueSource{2.5}}),
                "blur radius is valid");
        require(composition->parameters().insert(
                    {edge, "bloom.blur.edge", ConstantValueSource{std::int64_t{1}}}),
                "blur edge is valid");
        require(graph.addNode({blurNode, "bloom.gaussian-blur",
                               {{"radius", radius}, {"edge", edge}}, 1}),
                "blur node adds");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge removed");
        require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                               NodeInputRef{blurNode, "input"}}),
                "image enters blur");
        require(graph.addEdge(
                    {EdgeId::fromRaw(970),
                     {blurNode, "image"},
                     NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
                "blur enters Layer");
        if (withPreview) {
            const auto display = ParameterId::fromRaw(972);
            require(composition->parameters().insert(
                        {display, std::string(kImagePreviewDisplayParameterSchemaKey),
                         ConstantValueSource{std::int64_t{2}}}),
                    "display is valid");
            require(graph.addNode({previewNode, std::string(kImagePreviewNodeType),
                                   {{std::string(kImagePreviewDisplayParameterRole), display}},
                                   kImagePreviewNodeSchemaVersion}),
                    "preview adds");
            require(graph.addEdge({EdgeId::fromRaw(971), {blurNode, "image"},
                                   NodeInputRef{previewNode, "image"}}),
                    "blur enters the preview");
        }
        return project;
    };
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();

    auto previewed = build(true);
    expectations.expect(previewed.validate().ok(), "an Image Preview document validates");
    const auto with = compile(std::move(previewed), registry);
    const auto without = compile(build(false), registry);
    expectations.expect(with.plan && without.plan &&
                            std::ranges::equal(with.plan->operations(), without.plan->operations()) &&
                            with.plan->output() == without.plan->output(),
                        "an Image Preview leaves the composition's compiled plan unchanged");

    document::Document document(build(true));
    runtime::SnapshotCompiler compiler(registry);
    const auto inspected = compiler.compile(
        {.snapshot = document.snapshot(),
         .compositionId = kCompositionId,
         .parameterOverrides = {},
         .channelInspection = runtime::ChannelInspection{blurNode, "image", {}}},
        runtime::CancellationToken{});
    expectations.expect(inspected.plan != nullptr, "a blur's image output can be inspected");
    if (!inspected.plan)
        return;
    const auto& operations = inspected.plan->operations();
    const auto* output =
        std::get_if<runtime::CompiledCompositionOutput>(&operations[inspected.plan->output().value()]);
    const auto* merge =
        output ? std::get_if<runtime::CompiledMerge>(&operations[output->input.value()]) : nullptr;
    const auto* shown = merge && merge->entries.size() == 1
                            ? std::get_if<runtime::CompiledImageEffect>(
                                  &operations[merge->entries.front().input.value()])
                            : nullptr;
    expectations.expect(shown != nullptr && shown->sourceNodeId == blurNode,
                        "the inspection plan shows the blur's own operation");
    expectations.expect(std::ranges::none_of(operations,
                                             [](const auto& operation) {
                                                 return std::holds_alternative<
                                                     runtime::CompiledLayerOutput>(operation);
                                             }),
                        "the inspection plan carries none of the layer stack");
    const auto refused = compiler.compile(
        {.snapshot = document.snapshot(),
         .compositionId = kCompositionId,
         .parameterOverrides = {},
         .channelInspection = runtime::ChannelInspection{blurNode, "radius", {}}},
        runtime::CancellationToken{});
    expectations.expect(refused.plan == nullptr,
                        "an inspection of a port with no pixels is refused");
}

// #503 Alpha Association lowers to one alpha association kernel with its constant mode and white
// switch. Remove Color Matting keeps the background parameter's identity; every other mode lowers a
// constant black background without an identity, so animating the unused colour neither claims nor
// invalidates anything. A muted node passes its Image through.
void testAlphaAssociationLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    const auto effectId = NodeId::fromRaw(996);
    const auto* definition = builtInNodeDefinitions().find(kAlphaAssociationNodeType, 1);
    require(definition != nullptr, "the alpha association definition exists");
    for (const auto mode : {kAlphaAssociationUnmult, kAlphaAssociationPremultiply,
                            kAlphaAssociationUnpremultiply, kAlphaAssociationRemoveMatting}) {
        for (const bool muted : {false, true}) {
            auto project = makeProject(singleLayerOptions());
            auto* composition = project.findComposition(kCompositionId);
            auto& graph = composition->graph();
            std::vector<ParameterBinding> bindings;
            std::uint64_t next = 2600;
            for (const auto& parameter : definition->parameters) {
                auto value = parameter.defaultValue;
                if (parameter.role == "mode")
                    value = mode;
                if (parameter.role == "white")
                    value = true;
                if (parameter.role == "background")
                    value = core::Color4d{0.2, 0.4, 1.5, 1.0};
                require(composition->parameters().insert({ParameterId::fromRaw(next),
                                                          parameter.schemaKey,
                                                          ConstantValueSource{value}}),
                        "alpha association parameter is valid");
                bindings.push_back({parameter.role, ParameterId::fromRaw(next++)});
            }
            require(graph.addNode({effectId, std::string(kAlphaAssociationNodeType),
                                   std::move(bindings), 1}),
                    "alpha association node is accepted");
            require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
            require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                                   NodeInputRef{effectId, "input"}}),
                    "source feeds the alpha association");
            require(graph.addEdge({EdgeId::fromRaw(996), {effectId, "image"},
                                   NodeInputRef{kFirstLayerNode,
                                                std::string(kLayerOutputContentInputPort)}}),
                    "alpha association feeds Layer");
            if (muted)
                composition->nodeLayout()[effectId].muted = true;
            expectations.expect(project.validate().ok(),
                                "alpha association graph is valid document truth");
            const auto result = compile(std::move(project), registry);
            expectations.expect(result.plan != nullptr, "alpha association graph compiles");
            if (!result.plan)
                continue;
            const auto effect = std::ranges::find_if(result.plan->operations(), [&](const auto& op) {
                const auto* item = std::get_if<runtime::CompiledImageEffect>(&op);
                return item && item->sourceNodeId == effectId;
            });
            if (muted) {
                expectations.expect(effect == result.plan->operations().end(),
                                    "a muted alpha association passes its Image through");
                continue;
            }
            require(effect != result.plan->operations().end(),
                    "alpha association lowers to an image effect");
            const auto* kernel = std::get_if<runtime::AlphaAssociationKernel>(
                &std::get<runtime::CompiledImageEffect>(*effect).kernel);
            const auto* color =
                kernel ? std::get_if<core::Color4d>(&kernel->background.source) : nullptr;
            const bool matting = mode == kAlphaAssociationRemoveMatting;
            expectations.expect(
                kernel && static_cast<std::int64_t>(kernel->mode) == mode && kernel->white &&
                    color &&
                    (matting ? kernel->background.id == ParameterId::fromRaw(2602) &&
                                   *color == core::Color4d{0.2, 0.4, 1.5, 1.0}
                             : !kernel->background.id.isValid() &&
                                   *color == core::Color4d{0.0, 0.0, 0.0, 1.0}),
                "alpha association lowers its mode, white switch and (Remove Color Matting "
                "only) its background");
        }
    }
}

// #518 Tone Map lowers to one color tool kernel with its constant method and gamut switch and its
// six scalars (with their parameter identities) in the kernel's order. A muted node passes its
// Image through.
void testColorToolLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    const auto effectId = NodeId::fromRaw(995);
    const auto* definition = builtInNodeDefinitions().find(kToneMapNodeType, 1);
    require(definition != nullptr, "the Tone Map definition exists");
    for (const bool muted : {false, true}) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        std::vector<ParameterBinding> bindings;
        std::map<std::string, ParameterId> ids;
        std::uint64_t next = 2800;
        for (const auto& parameter : definition->parameters) {
            auto value = parameter.defaultValue;
            if (parameter.role == "method")
                value = kToneMapBrightestChannel;
            if (parameter.role == "gamutCompress")
                value = true;
            if (parameter.role == "knee")
                value = 1.5;
            if (parameter.role == "ceiling")
                value = 4.0;
            if (parameter.role == "shadows")
                value = 0.5;
            const auto id = ParameterId::fromRaw(next++);
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{value}}),
                    "Tone Map parameter is valid");
            ids[parameter.role] = id;
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({effectId, std::string(kToneMapNodeType), std::move(bindings), 1}),
                "Tone Map node is accepted");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
        require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                               NodeInputRef{effectId, "input"}}),
                "source feeds Tone Map");
        require(graph.addEdge({EdgeId::fromRaw(995), {effectId, "image"},
                               NodeInputRef{kFirstLayerNode,
                                            std::string(kLayerOutputContentInputPort)}}),
                "Tone Map feeds Layer");
        if (muted)
            composition->nodeLayout()[effectId].muted = true;
        expectations.expect(project.validate().ok(), "Tone Map graph is valid document truth");
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "Tone Map graph compiles");
        if (!result.plan)
            continue;
        const auto effect = std::ranges::find_if(result.plan->operations(), [&](const auto& op) {
            const auto* item = std::get_if<runtime::CompiledImageEffect>(&op);
            return item && item->sourceNodeId == effectId;
        });
        if (muted) {
            expectations.expect(effect == result.plan->operations().end(),
                                "a muted Tone Map passes its Image through");
            continue;
        }
        require(effect != result.plan->operations().end(), "Tone Map lowers to an image effect");
        const auto* kernel = std::get_if<runtime::ColorToolKernel>(
            &std::get<runtime::CompiledImageEffect>(*effect).kernel);
        const auto scalar = [&](const std::size_t slot, const char* role, const double value) {
            const auto* source = kernel ? std::get_if<double>(&kernel->values[slot].source) : nullptr;
            return source != nullptr && *source == value && kernel->values[slot].id == ids[role];
        };
        expectations.expect(
            kernel && kernel->operation == runtime::ColorToolOperation::ToneMap &&
                kernel->choices[0] == kToneMapBrightestChannel && kernel->choices[1] == 1 &&
                scalar(0, "knee", 1.5) && scalar(1, "ceiling", 4.0) && scalar(2, "shadows", 0.5) &&
                scalar(3, "shadowRange", 0.18) && scalar(4, "gamutThreshold", 0.8) &&
                scalar(5, "gamutLimit", 1.2),
            "Tone Map lowers its method, gamut switch and six scalars with their identities");
    }
}

// #517 Color Matrix lowers to one color tool kernel with its four constant choices and sixteen
// elements (with their parameter identities) in row order.
void testColorMatrixLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    const auto effectId = NodeId::fromRaw(994);
    const auto* definition = builtInNodeDefinitions().find(kColorMatrixNodeType, 1);
    require(definition != nullptr, "the Color Matrix definition exists");
    auto project = makeProject(singleLayerOptions());
    auto* composition = project.findComposition(kCompositionId);
    auto& graph = composition->graph();
    std::vector<ParameterBinding> bindings;
    std::map<std::string, ParameterId> ids;
    std::uint64_t next = 2900;
    for (const auto& parameter : definition->parameters) {
        auto value = parameter.defaultValue;
        if (parameter.role == "preset")
            value = std::int64_t{6};
        if (parameter.role == "matrixSize")
            value = kColorMatrixSize4;
        if (parameter.role == "inputModel")
            value = kColorModelHsv;
        if (parameter.role == "outputModel")
            value = kColorModelLab;
        if (parameter.role == "rg")
            value = 0.25;
        if (parameter.role == "aa")
            value = 0.5;
        const auto id = ParameterId::fromRaw(next++);
        require(composition->parameters().insert({id, parameter.schemaKey, ConstantValueSource{value}}),
                "Color Matrix parameter is valid");
        ids[parameter.role] = id;
        bindings.push_back({parameter.role, id});
    }
    require(graph.addNode({effectId, std::string(kColorMatrixNodeType), std::move(bindings), 1}),
            "Color Matrix node is accepted");
    require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
    require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                           NodeInputRef{effectId, "input"}}),
            "source feeds Color Matrix");
    require(graph.addEdge({EdgeId::fromRaw(994), {effectId, "image"},
                           NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
            "Color Matrix feeds Layer");
    expectations.expect(project.validate().ok(), "Color Matrix graph is valid document truth");
    const auto result = compile(std::move(project), registry);
    expectations.expect(result.plan != nullptr, "Color Matrix graph compiles");
    if (!result.plan)
        return;
    const auto effect = std::ranges::find_if(result.plan->operations(), [&](const auto& op) {
        const auto* item = std::get_if<runtime::CompiledImageEffect>(&op);
        return item && item->sourceNodeId == effectId;
    });
    require(effect != result.plan->operations().end(), "Color Matrix lowers to an image effect");
    const auto* kernel = std::get_if<runtime::ColorToolKernel>(
        &std::get<runtime::CompiledImageEffect>(*effect).kernel);
    const auto element = [&](const std::size_t slot, const char* role, const double value) {
        const auto* source = kernel ? std::get_if<double>(&kernel->values[slot].source) : nullptr;
        return source != nullptr && *source == value && kernel->values[slot].id == ids[role];
    };
    expectations.expect(
        kernel && kernel->operation == runtime::ColorToolOperation::ColorMatrix &&
            kernel->choices[0] == 6 && kernel->choices[1] == kColorMatrixSize4 &&
            kernel->choices[2] == kColorModelHsv && kernel->choices[3] == kColorModelLab &&
            element(0, "rr", 1.0) && element(1, "rg", 0.25) && element(5, "gg", 1.0) &&
            element(15, "aa", 0.5),
        "Color Matrix lowers its choices and sixteen elements with their identities");
}

// #519 Quantize lowers to one color tool kernel with its mode, basis, invert and the packed link and
// alpha switches, and the scalars the mode reads in the kernel's order (with their identities); a
// scalar the mode does not read is a fixed constant with no identity, so a keyed value of the other
// mode neither varies the picture with time nor reaches the kernel. A muted node passes its Image
// through.
void testQuantizeLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    const auto effectId = NodeId::fromRaw(993);
    const auto* definition = builtInNodeDefinitions().find(kQuantizeNodeType, 1);
    require(definition != nullptr, "the Quantize definition exists");
    struct Lowered final {
        std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
        const runtime::ColorToolKernel* kernel = nullptr;
        std::map<std::string, ParameterId> ids;
        bool timeDependent = false;
        bool passedThrough = false;
    };
    const auto lower = [&](const std::map<std::string, ParameterValue>& values,
                           const std::string_view keyedRole = {}, const bool muted = false) {
        Lowered out;
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = 3000;
        for (const auto& parameter : definition->parameters) {
            const auto found = values.find(parameter.role);
            const auto id = ParameterId::fromRaw(next++);
            require(composition->parameters().insert(
                        {id, parameter.schemaKey,
                         ConstantValueSource{found == values.end() ? parameter.defaultValue
                                                                   : found->second}}),
                    "Quantize parameter is valid");
            out.ids[parameter.role] = id;
            bindings.push_back({parameter.role, id});
        }
        if (!keyedRole.empty()) {
            constexpr auto curveId = AnimationCurveId::fromRaw(3100);
            ScalarAnimationCurve curve{curveId, {}};
            curve.keyframes = {{KeyframeId::fromRaw(3101), core::RationalTime::fromInteger(0), 3.0,
                                KeyframeInterpolation::Linear},
                               {KeyframeId::fromRaw(3102), core::RationalTime::fromInteger(1), 9.0,
                                KeyframeInterpolation::Linear}};
            require(composition->animationCurves().insert(std::move(curve)), "the curve is publishable");
            require(composition->parameters().setSource(out.ids.at(std::string(keyedRole)),
                                                        AnimationCurveSource{curveId}),
                    "the value animates");
        }
        require(graph.addNode({effectId, std::string(kQuantizeNodeType), std::move(bindings), 1}),
                "Quantize node is accepted");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
        require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                               NodeInputRef{effectId, "input"}}),
                "source feeds Quantize");
        require(graph.addEdge({EdgeId::fromRaw(993), {effectId, "image"},
                               NodeInputRef{kFirstLayerNode,
                                            std::string(kLayerOutputContentInputPort)}}),
                "Quantize feeds Layer");
        if (muted)
            composition->nodeLayout()[effectId].muted = true;
        expectations.expect(project.validate().ok(), "Quantize graph is valid document truth");
        auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "Quantize graph compiles");
        out.plan = result.plan;
        if (!out.plan)
            return out;
        std::size_t index = 0;
        for (; index < out.plan->operations().size(); ++index) {
            const auto* item = std::get_if<runtime::CompiledImageEffect>(&out.plan->operations()[index]);
            if (item && item->sourceNodeId == effectId) {
                out.kernel = std::get_if<runtime::ColorToolKernel>(&item->kernel);
                out.timeDependent =
                    out.plan->operationTimeDependent(runtime::OperationIndex::fromRaw(index));
                break;
            }
        }
        out.passedThrough = out.kernel == nullptr;
        return out;
    };
    const auto number = [](const runtime::ColorToolKernel& kernel, const std::size_t slot) {
        const auto* source = std::get_if<double>(&kernel.values[slot].source);
        return source != nullptr ? *source : -1.0;
    };
    // The defaults: a luma threshold at 0.5, hard, not inverted, links on, alpha off.
    {
        const auto base = lower({});
        require(base.kernel != nullptr, "Quantize lowers to a color tool kernel");
        expectations.expect(
            base.kernel->operation == runtime::ColorToolOperation::Quantize &&
                base.kernel->choices[0] == kQuantizeThreshold &&
                base.kernel->choices[1] == kQuantizeBasisLuma && base.kernel->choices[2] == 0 &&
                base.kernel->choices[3] == 1 && number(*base.kernel, 0) == 0.5 &&
                number(*base.kernel, 1) == 0.0 && base.kernel->values[0].id == base.ids.at("level") &&
                base.kernel->values[1].id == base.ids.at("softness") &&
                !base.kernel->values[2].id.isValid() && !base.kernel->values[3].id.isValid(),
            "Quantize's defaults lower the threshold's level and softness with their identities and "
            "fix the posterize values");
    }
    // A posterize: linked reads the master, unlinked the three channels, alpha its own levels.
    {
        const auto linked = lower({{"mode", std::int64_t{kQuantizePosterize}}, {"levels", 5.0}});
        require(linked.kernel != nullptr, "a linked posterize lowers");
        expectations.expect(linked.kernel->choices[0] == kQuantizePosterize &&
                                linked.kernel->choices[3] == 1 && number(*linked.kernel, 2) == 5.0 &&
                                linked.kernel->values[2].id == linked.ids.at("levels") &&
                                !linked.kernel->values[3].id.isValid() &&
                                !linked.kernel->values[0].id.isValid(),
                            "a linked posterize lowers the master levels and no channel's");
        const auto unlinked = lower({{"mode", std::int64_t{kQuantizePosterize}},
                                     {"linkLevels", false},
                                     {"redLevels", 2.0},
                                     {"greenLevels", 3.0},
                                     {"blueLevels", 4.0},
                                     {"posterizeAlpha", true},
                                     {"alphaLevels", 6.0}});
        require(unlinked.kernel != nullptr, "an unlinked posterize lowers");
        expectations.expect(unlinked.kernel->choices[3] == 2 && number(*unlinked.kernel, 3) == 2.0 &&
                                number(*unlinked.kernel, 4) == 3.0 && number(*unlinked.kernel, 5) == 4.0 &&
                                number(*unlinked.kernel, 6) == 6.0 &&
                                unlinked.kernel->values[3].id == unlinked.ids.at("redLevels") &&
                                unlinked.kernel->values[6].id == unlinked.ids.at("alphaLevels") &&
                                !unlinked.kernel->values[2].id.isValid(),
                            "an unlinked posterize with alpha lowers the channels' and alpha's levels");
        const auto inverted = lower({{"invert", true}, {"basis", std::int64_t{kQuantizeBasisPerChannel}}});
        expectations.expect(inverted.kernel != nullptr && inverted.kernel->choices[1] == 1 &&
                                inverted.kernel->choices[2] == 1,
                            "a per-channel inverted threshold lowers its basis and invert switch");
    }
    // Time dependence: a keyed value the mode reads varies with time, one it does not read does not.
    expectations.expect(lower({}, "level").timeDependent, "a keyed level varies with time");
    expectations.expect(lower({}, "softness").timeDependent, "a keyed softness varies with time");
    expectations.expect(!lower({}, "levels").timeDependent,
                        "a keyed master levels value does not vary a threshold");
    expectations.expect(!lower({}, "redLevels").timeDependent,
                        "a keyed channel levels value does not vary a threshold");
    const std::map<std::string, ParameterValue> posterize{{"mode", std::int64_t{kQuantizePosterize}}};
    expectations.expect(lower(posterize, "levels").timeDependent, "a keyed master levels value varies a linked posterize");
    expectations.expect(!lower(posterize, "level").timeDependent,
                        "a keyed threshold level does not vary a posterize");
    expectations.expect(!lower(posterize, "redLevels").timeDependent,
                        "a keyed channel levels value does not vary a linked posterize");
    expectations.expect(!lower(posterize, "alphaLevels").timeDependent,
                        "a keyed alpha levels value does not vary a posterize without alpha");
    auto unlinkedAlpha = posterize;
    unlinkedAlpha["linkLevels"] = false;
    unlinkedAlpha["posterizeAlpha"] = true;
    expectations.expect(lower(unlinkedAlpha, "blueLevels").timeDependent &&
                            lower(unlinkedAlpha, "alphaLevels").timeDependent &&
                            !lower(unlinkedAlpha, "levels").timeDependent,
                        "an unlinked posterize with alpha reads the channels' and alpha's levels, not the master");
    // Muted: the Image passes through.
    expectations.expect(lower({}, {}, true).passedThrough, "a muted Quantize passes its Image through");
}

// #523: an Image qualified by HSL Qualifier feeds Image Mix's mask. It lowers to a ColorTool kernel
// with the qualifier's choices and nine operands with their identities; muted it is an empty Mask.
void testHslQualifierLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    const auto keyId = NodeId::fromRaw(996);
    const auto mixId = NodeId::fromRaw(997);
    for (int scenario = 0; scenario != 2; ++scenario) {
        auto project = makeProject();
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        std::uint64_t next = 3000;
        std::map<std::string, ParameterId> ids;
        const auto add = [&](const NodeId id, const std::string_view nodeType) {
            const auto* definition = builtInNodeDefinitions().find(nodeType, 1);
            require(definition != nullptr, "definition exists");
            std::vector<ParameterBinding> bindings;
            for (const auto& parameter : definition->parameters) {
                const auto parameterId = ParameterId::fromRaw(next++);
                auto value = parameter.defaultValue;
                if (parameter.role == "hueCenter")
                    value = 350.0;
                if (parameter.role == "satHigh")
                    value = 0.75;
                if (parameter.role == "invert")
                    value = true;
                require(composition->parameters().insert(
                            {parameterId, parameter.schemaKey, ConstantValueSource{value}}),
                        "parameter is valid");
                ids[parameter.role] = parameterId;
                bindings.push_back({parameter.role, parameterId});
            }
            require(graph.addNode({id, std::string(nodeType), std::move(bindings), 1}),
                    "node is accepted");
        };
        add(keyId, std::string(kHslQualifierNodeType));
        add(mixId, "bloom.image-mix");
        require(graph.eraseEdge(kFirstSourceEdge), "base source edge is removed");
        require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                               NodeInputRef{mixId, "input"}}),
                "base feeds Image Mix");
        require(graph.addEdge({EdgeId::fromRaw(996), {kSecondSolidNode, "image"},
                               NodeInputRef{mixId, "second"}}),
                "second feeds Image Mix");
        require(graph.addEdge({EdgeId::fromRaw(997), {kSecondSolidNode, "image"},
                               NodeInputRef{keyId, "input"}}),
                "an Image feeds HSL Qualifier");
        require(graph.addEdge({EdgeId::fromRaw(998), {keyId, "mask"}, NodeInputRef{mixId, "mask"}}),
                "HSL Qualifier feeds Image Mix's mask");
        require(graph.addEdge({EdgeId::fromRaw(999), {mixId, "image"},
                               NodeInputRef{kFirstLayerNode,
                                            std::string(kLayerOutputContentInputPort)}}),
                "Image Mix feeds Layer");
        if (scenario == 1)
            composition->nodeLayout()[keyId].muted = true;
        expectations.expect(project.validate().ok(), "HSL Qualifier graph is valid document truth");
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "HSL Qualifier graph compiles");
        if (!result.plan)
            continue;
        const auto& operations = result.plan->operations();
        const auto mix = std::ranges::find_if(operations, [&](const auto& item) {
            const auto* effect = std::get_if<runtime::CompiledImageEffect>(&item);
            return effect && effect->sourceNodeId == mixId;
        });
        require(mix != operations.end(), "Image Mix lowers");
        const auto& mixEffect = std::get<runtime::CompiledImageEffect>(*mix);
        require(mixEffect.maskInput.has_value(), "Image Mix keeps its connected mask");
        const auto& maskSource = operations[mixEffect.maskInput->value()];
        if (scenario == 1) {
            const auto* empty = std::get_if<runtime::CompiledMerge>(&maskSource);
            expectations.expect(empty && empty->entries.empty(),
                                "a muted HSL Qualifier is an empty Mask, never its Image");
            continue;
        }
        const auto* keyEffect = std::get_if<runtime::CompiledImageEffect>(&maskSource);
        const auto* kernel =
            keyEffect ? std::get_if<runtime::ColorToolKernel>(&keyEffect->kernel) : nullptr;
        const auto value = [&](const std::size_t slot, const char* role, const double expected) {
            const auto* source =
                kernel ? std::get_if<double>(&kernel->values[slot].source) : nullptr;
            return source != nullptr && *source == expected && kernel->values[slot].id == ids[role];
        };
        expectations.expect(keyEffect && keyEffect->sourceNodeId == keyId && kernel &&
                                kernel->operation == runtime::ColorToolOperation::HslQualifier &&
                                kernel->choices[0] == 1 && value(0, "hueCenter", 350.0) &&
                                value(1, "hueWidth", 360.0) && value(4, "satHigh", 0.75) &&
                                value(8, "lumaSoftness", 0.1),
                            "HSL Qualifier lowers its choice and nine operands with identities");
    }
}

// #504 Colorize lowers to one colorize kernel with its constant mode. Each mode keeps the
// identities of only the operands it reads (Gradient Map also its source and cycle); every other
// operand stays the kernel's constant default without an identity, so animating it neither claims
// nor invalidates anything. A muted node passes its Image through. #541: a Gradient Map with its
// gradient bound lowers the gradient (one inline operand) instead of the three-colour ramp; one
// saved without it keeps the ramp.
void testColorizeLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    const auto effectId = NodeId::fromRaw(997);
    const auto* definition = builtInNodeDefinitions().find(kColorizeNodeType, 1);
    require(definition != nullptr, "the colorize definition exists");
    for (const auto mode : {kColorizeFill, kColorizeTint, kColorizeTritone, kColorizeGradientMap,
                            kColorizeBlackAndWhite}) {
      for (const bool withGradient : {true, false}) {
        if (!withGradient && mode != kColorizeGradientMap)
            continue;
        for (const bool muted : {false, true}) {
            auto project = makeProject(singleLayerOptions());
            auto* composition = project.findComposition(kCompositionId);
            auto& graph = composition->graph();
            std::vector<ParameterBinding> bindings;
            std::map<std::string, ParameterId> ids;
            std::uint64_t next = 2700;
            for (const auto& parameter : definition->parameters) {
                if (parameter.role == "gradient" && !withGradient)
                    continue;
                auto value = parameter.defaultValue;
                if (parameter.role == "mode")
                    value = mode;
                if (parameter.role == "source")
                    value = kColorizeSourceGreen;
                if (parameter.role == "cycle")
                    value = kColorizeCycleMirror;
                if (parameter.role == "amount")
                    value = 0.75;
                const auto id = ParameterId::fromRaw(next++);
                require(composition->parameters().insert(
                            {id, parameter.schemaKey, ConstantValueSource{value}}),
                        "colorize parameter is valid");
                ids[parameter.role] = id;
                bindings.push_back({parameter.role, id});
            }
            require(graph.addNode({effectId, std::string(kColorizeNodeType), std::move(bindings),
                                   1}),
                    "colorize node is accepted");
            require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
            require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                                   NodeInputRef{effectId, "input"}}),
                    "source feeds the colorize");
            require(graph.addEdge({EdgeId::fromRaw(997), {effectId, "image"},
                                   NodeInputRef{kFirstLayerNode,
                                                std::string(kLayerOutputContentInputPort)}}),
                    "colorize feeds Layer");
            if (muted)
                composition->nodeLayout()[effectId].muted = true;
            expectations.expect(project.validate().ok(), "colorize graph is valid document truth");
            const auto result = compile(std::move(project), registry);
            expectations.expect(result.plan != nullptr, "colorize graph compiles");
            if (!result.plan)
                continue;
            const auto effect = std::ranges::find_if(result.plan->operations(), [&](const auto& op) {
                const auto* item = std::get_if<runtime::CompiledImageEffect>(&op);
                return item && item->sourceNodeId == effectId;
            });
            if (muted) {
                expectations.expect(effect == result.plan->operations().end(),
                                    "a muted colorize passes its Image through");
                continue;
            }
            require(effect != result.plan->operations().end(),
                    "colorize lowers to an image effect");
            const auto* kernel = std::get_if<runtime::ColorizeKernel>(
                &std::get<runtime::CompiledImageEffect>(*effect).kernel);
            require(kernel != nullptr, "colorize lowers to a colorize kernel");
            const bool map = mode == kColorizeGradientMap;
            const bool stops = map && withGradient;
            const bool ramp = mode == kColorizeTritone || (map && !withGradient);
            const auto owns = [&](const ParameterId id, const char* role, const bool used) {
                return used ? id == ids.at(role) : !id.isValid();
            };
            bool weights = true;
            const std::array<const char*, 6> roles{"reds",  "yellows", "greens",
                                                   "cyans", "blues",   "magentas"};
            for (std::size_t index = 0; index < roles.size(); ++index)
                weights = weights && owns(kernel->weights[index].id, roles[index],
                                          mode == kColorizeBlackAndWhite);
            expectations.expect(
                static_cast<std::int64_t>(kernel->mode) == mode &&
                    kernel->source == (map ? runtime::ColorizeSource::Green
                                           : runtime::ColorizeSource::Luma) &&
                    kernel->cycle ==
                        (map ? runtime::ColorizeCycle::Mirror : runtime::ColorizeCycle::Clamp) &&
                    owns(kernel->amount.id, "amount", true) &&
                    std::get<double>(kernel->amount.source) == 0.75 &&
                    owns(kernel->color.id, "fillColor", mode == kColorizeFill) &&
                    kernel->gradient.has_value() == stops &&
                    (!stops || (kernel->gradient->id == ids.at("gradient") &&
                                std::get<GradientValue>(kernel->gradient->source) ==
                                    defaultColorizeGradient())) &&
                    owns(kernel->shadows.id, "shadows", mode == kColorizeTint || ramp) &&
                    owns(kernel->highlights.id, "highlights", mode == kColorizeTint || ramp) &&
                    owns(kernel->midtones.id, "midtones", ramp) &&
                    owns(kernel->midpoint.id, "midpoint", ramp) &&
                    owns(kernel->repetitions.id, "repetitions", map) &&
                    owns(kernel->phase.id, "phase", map) && weights,
                "colorize lowers its mode and only the operands that mode reads");
        }
      }
    }
}

// #481 Image Math, Image Map Range and Image Clamp lower to one pixel math kernel: the constant
// operation, channels and premultiplication, and only the values the operation reads (an Image Math
// whose `second` input is connected reads that image and keeps no value). A muted node passes its
// Image through.
void testPixelMathLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    struct Variant final {
        std::string_view type;
        std::int64_t operation = 0;
        bool second = false;
        bool muted = false;
    };
    const std::array<Variant, 8> variants{{{kImageMathNodeType, kImageMathPower, false, false},
                                           {kImageMathNodeType, kImageMathAbsolute, false, false},
                                           {kImageMathNodeType, kImageMathDivide, true, false},
                                           {kImageMathNodeType, kImageMathAdd, false, true},
                                           {kImageMapRangeNodeType, 0, false, false},
                                           {kImageMapRangeNodeType, 0, false, true},
                                           {kImageClampNodeType, 0, false, false},
                                           {kImageClampNodeType, 0, false, true}}};
    for (const auto& variant : variants) {
        const auto effectId = NodeId::fromRaw(997);
        const auto* definition = builtInNodeDefinitions().find(variant.type, 1);
        require(definition != nullptr, "the pixel math definition exists");
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        std::vector<ParameterBinding> bindings;
        std::map<std::string, ParameterId> ids;
        std::uint64_t next = 2900;
        for (const auto& parameter : definition->parameters) {
            auto value = parameter.defaultValue;
            if (parameter.role == "operation")
                value = variant.operation;
            if (parameter.role == "channels")
                value = kImagePixelColorAndAlpha;
            if (parameter.role == "space")
                value = kImagePixelStored;
            if (parameter.role == "value")
                value = 2.5;
            if (parameter.role == "clamp")
                value = true;
            if (parameter.role == "outputMax")
                value = 9.0;
            if (parameter.role == "maxGreen")
                value = 3.0;
            const auto id = ParameterId::fromRaw(next++);
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{value}}),
                    "pixel math parameter is valid");
            ids[parameter.role] = id;
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({effectId, std::string(variant.type), std::move(bindings), 1}),
                "pixel math node is accepted");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
        require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                               NodeInputRef{effectId, "input"}}),
                "source feeds the pixel math node");
        if (variant.second)
            require(graph.addEdge({EdgeId::fromRaw(997), {kFirstSolidNode, "image"},
                                   NodeInputRef{effectId, "second"}}),
                    "the source also feeds the second input");
        require(graph.addEdge({EdgeId::fromRaw(998), {effectId, "image"},
                               NodeInputRef{kFirstLayerNode,
                                            std::string(kLayerOutputContentInputPort)}}),
                "pixel math feeds Layer");
        if (variant.muted)
            composition->nodeLayout()[effectId].muted = true;
        expectations.expect(project.validate().ok(), "pixel math graph is valid document truth");
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "pixel math graph compiles");
        if (!result.plan)
            continue;
        const auto effect = std::ranges::find_if(result.plan->operations(), [&](const auto& op) {
            const auto* item = std::get_if<runtime::CompiledImageEffect>(&op);
            return item && item->sourceNodeId == effectId;
        });
        if (variant.muted) {
            expectations.expect(effect == result.plan->operations().end(),
                                "a muted pixel math node passes its Image through");
            continue;
        }
        require(effect != result.plan->operations().end(), "pixel math lowers to an image effect");
        const auto& lowered = std::get<runtime::CompiledImageEffect>(*effect);
        const auto* kernel = std::get_if<runtime::PixelMathKernel>(&lowered.kernel);
        require(kernel != nullptr, "pixel math lowers to a pixel math kernel");
        const auto owns = [&](const std::size_t slot, const char* role) {
            return kernel->values[slot].id == ids.at(role);
        };
        const bool math = variant.type == kImageMathNodeType;
        const bool range = variant.type == kImageMapRangeNodeType;
        expectations.expect(kernel->straight == false &&
                                (math ? kernel->operation == static_cast<runtime::PixelMathOperation>(
                                                                 variant.operation)
                                      : kernel->operation ==
                                            (range ? runtime::PixelMathOperation::MapRange
                                                   : runtime::PixelMathOperation::Clamp)) &&
                                kernel->second == variant.second &&
                                lowered.secondaryInput.has_value() == variant.second,
                            "pixel math lowers its operation, space and second input");
        if (math) {
            const bool reads = !variant.second && variant.operation != kImageMathAbsolute;
            expectations.expect(kernel->channels == kImagePixelColorAndAlpha &&
                                    (reads ? owns(0, "value") &&
                                                 std::get<double>(kernel->values[0].source) == 2.5
                                           : !kernel->values[0].id.isValid()),
                                "an Image Math lowers only the operand its operation reads");
        } else if (range) {
            expectations.expect(kernel->clamp && owns(0, "inputMin") && owns(1, "inputMax") &&
                                    owns(2, "outputMin") && owns(3, "outputMax") &&
                                    std::get<double>(kernel->values[3].source) == 9.0,
                                "an Image Map Range lowers its range and clamp");
        } else {
            expectations.expect(owns(0, "minRed") && owns(1, "maxRed") && owns(3, "maxGreen") &&
                                    std::get<double>(kernel->values[3].source) == 3.0 &&
                                    owns(6, "minAlpha") && owns(7, "maxAlpha"),
                                "an Image Clamp lowers every limit");
        }
    }
}

// #349 Image Compare lowers to one pixel math kernel that reads its second image, and Histogram to
// the three passes of its chain (Bin, Sum, Plot) with the graph's choices; neither takes an effect
// mask, a muted node passes its Image through, and Image Compare without its second image does not
// compile.
void testHistogramCompareLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    for (const auto type : {kImageCompareNodeType, kHistogramNodeType}) {
        const auto* definition = builtInNodeDefinitions().find(type, 1);
        require(definition != nullptr, "the analysis definition exists");
        expectations.expect(std::ranges::none_of(definition->inputs,
                                                 [](const auto& input) {
                                                     return input.name == "mask";
                                                 }),
                            "an analysis node publishes its own image and takes no mask");
        for (const bool muted : {false, true}) {
            for (const bool second : {true, false}) {
                if (type == kHistogramNodeType && second)
                    continue;
                const auto effectId = NodeId::fromRaw(997);
                auto project = makeProject(singleLayerOptions());
                auto* composition = project.findComposition(kCompositionId);
                auto& graph = composition->graph();
                std::vector<ParameterBinding> bindings;
                std::map<std::string, ParameterId> ids;
                std::uint64_t next = 2900;
                for (const auto& parameter : definition->parameters) {
                    auto value = parameter.defaultValue;
                    if (parameter.role == "mode")
                        value = kImageCompareSplit;
                    if (parameter.role == "splitPosition")
                        value = 0.25;
                    if (parameter.role == "angle")
                        value = 30.0;
                    if (parameter.role == "cellSize")
                        value = 12.0;
                    if (parameter.role == "channels")
                        value = kHistogramRgbLuminance;
                    if (parameter.role == "space")
                        value = kImagePixelStored;
                    if (parameter.role == "rangeMax")
                        value = 4.0;
                    if (parameter.role == "bins")
                        value = std::int64_t{1};
                    if (parameter.role == "height")
                        value = std::int64_t{3};
                    if (parameter.role == "graphScale")
                        value = kHistogramLogarithmic;
                    const auto id = ParameterId::fromRaw(next++);
                    require(composition->parameters().insert(
                                {id, parameter.schemaKey, ConstantValueSource{value}}),
                            "analysis parameter is valid");
                    ids[parameter.role] = id;
                    bindings.push_back({parameter.role, id});
                }
                require(graph.addNode({effectId, std::string(type), std::move(bindings), 1}),
                        "analysis node is accepted");
                require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
                require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                                       NodeInputRef{effectId, "input"}}),
                        "source feeds the analysis node");
                if (second)
                    require(graph.addEdge({EdgeId::fromRaw(997), {kFirstSolidNode, "image"},
                                           NodeInputRef{effectId, "second"}}),
                            "the source also feeds the second input");
                require(graph.addEdge({EdgeId::fromRaw(998), {effectId, "image"},
                                       NodeInputRef{kFirstLayerNode,
                                                    std::string(kLayerOutputContentInputPort)}}),
                        "the analysis node feeds Layer");
                if (muted)
                    composition->nodeLayout()[effectId].muted = true;
                const auto result = compile(std::move(project), registry);
                if (type == kImageCompareNodeType && !second && !muted) {
                    expectations.expect(result.plan == nullptr,
                                        "an Image Compare without its second image does not compile");
                    continue;
                }
                expectations.expect(result.plan != nullptr,
                                    std::string("the analysis graph compiles: ") +
                                        std::string(type) + (second ? " with" : " without") +
                                        " a second image" + (muted ? ", muted" : ""));
                if (!result.plan)
                    continue;
                std::vector<const runtime::CompiledImageEffect*> effects;
                for (const auto& operation : result.plan->operations())
                    if (const auto* item = std::get_if<runtime::CompiledImageEffect>(&operation);
                        item && item->sourceNodeId == effectId)
                        effects.push_back(item);
                if (muted) {
                    expectations.expect(effects.empty(),
                                        "a muted analysis node passes its Image through");
                    continue;
                }
                if (type == kImageCompareNodeType) {
                    require(effects.size() == 1, "Image Compare lowers to one image effect");
                    const auto* kernel = std::get_if<runtime::PixelMathKernel>(&effects[0]->kernel);
                    require(kernel != nullptr, "Image Compare lowers to a pixel math kernel");
                    expectations.expect(
                        kernel->operation == runtime::PixelMathOperation::Compare && kernel->second &&
                            effects[0]->secondaryInput.has_value() &&
                            kernel->compareMode == kImageCompareSplit &&
                            kernel->values[0].id == ids.at("gain") &&
                            kernel->values[1].id == ids.at("splitPosition") &&
                            std::get<double>(kernel->values[1].source) == 0.25 &&
                            kernel->values[2].id == ids.at("angle") &&
                            std::get<double>(kernel->values[2].source) == 30.0 &&
                            kernel->values[3].id == ids.at("cellSize") &&
                            std::get<double>(kernel->values[3].source) == 12.0,
                        "Image Compare lowers its mode, second image and four values");
                    continue;
                }
                require(effects.size() == 3, "Histogram lowers to three passes");
                std::array<const runtime::HistogramKernel*, 3> kernels{};
                for (std::size_t pass = 0; pass < 3; ++pass) {
                    kernels[pass] = std::get_if<runtime::HistogramKernel>(&effects[pass]->kernel);
                    require(kernels[pass] != nullptr, "every Histogram pass is a histogram kernel");
                }
                expectations.expect(
                    kernels[0]->role == runtime::HistogramRole::Bin &&
                        kernels[1]->role == runtime::HistogramRole::Sum &&
                        kernels[2]->role == runtime::HistogramRole::Plot &&
                        effects[2]->input.value() == effects[1]->input.value() + 1 &&
                        kernels[0]->values[1].id == ids.at("rangeMax") &&
                        std::get<double>(kernels[0]->values[1].source) == 4.0 &&
                        !kernels[1]->values[0].id.isValid() && !kernels[2]->values[1].id.isValid(),
                    "only the counting pass holds the range");
                for (const auto* kernel : kernels)
                    expectations.expect(kernel->channels == kHistogramRgbLuminance &&
                                            !kernel->straight && kernel->logarithmic &&
                                            kernel->bins == kHistogramBins[1] &&
                                            kernel->height == kHistogramHeights[3],
                                        "every pass carries the graph's choices");
            }
        }
    }
}

// #349 Image Statistics lowers to a value operation with five outputs whose kernel carries a plan of
// its own: the picture its input ends in, then the two reduction passes, shown through the
// composition output. A parameter driven by an output reads the operation's output; the picture is not
// part of the plan that contains the node; the node's choices reach its passes; an unwired node gives
// no plan; a muted node gives no plan; and a node that reads a picture which depends on its own result
// is not a valid document.
void testImageStatisticsLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    const auto* definition = builtInNodeDefinitions().find(kImageStatisticsNodeType, 1);
    require(definition != nullptr, "the Image Statistics definition exists");
    expectations.expect(definition->outputs.size() == 5 && definition->inputs.size() == 1 &&
                            definition->category == NodeCategory::Values &&
                            definition->lowering == runtime::NodeLoweringKind::ValueImageStatistics,
                        "Image Statistics has one Image in, five Scalars out and is a value node");
    for (const bool wired : {true, false}) {
        for (const bool muted : {false, true}) {
            const auto statsId = NodeId::fromRaw(996);
            auto project = makeProject(singleLayerOptions());
            auto* composition = project.findComposition(kCompositionId);
            auto& graph = composition->graph();
            std::vector<ParameterBinding> bindings;
            std::uint64_t next = 3100;
            for (const auto& parameter : definition->parameters) {
                auto value = parameter.defaultValue;
                if (parameter.role == "channel")
                    value = kStatisticsAlpha;
                if (parameter.role == "space")
                    value = kImagePixelStored;
                if (parameter.role == "pixels")
                    value = kStatisticsOpaqueOnly;
                const auto id = ParameterId::fromRaw(next++);
                require(composition->parameters().insert(
                            {id, parameter.schemaKey, ConstantValueSource{value}}),
                        "statistics parameter is valid");
                bindings.push_back({parameter.role, id});
            }
            require(graph.addNode({statsId, std::string(kImageStatisticsNodeType),
                                   std::move(bindings), 1}),
                    "statistics node is accepted");
            if (wired)
                require(graph.addEdge({EdgeId::fromRaw(995), {kFirstSolidNode, "image"},
                                       NodeInputRef{statsId, "input"}}),
                        "the source feeds the statistics node");
            require(composition->parameters().setSource(
                        kFirstOpacity, DriverBindingSource{statsId, "maximum"}),
                    "the maximum drives the layer's opacity");
            if (muted)
                composition->nodeLayout()[statsId].muted = true;
            const auto validated = project.validate();
            expectations.expect(validated.ok(), "the statistics document is valid");
            if (!validated.ok()) {
                std::cerr << "wired " << wired << " muted " << muted << " issues:";
                for (const auto& issue : validated.issues())
                    std::cerr << ' ' << issue.message;
                std::cerr << '\n';
                continue;
            }
            const auto result = compile(std::move(project), registry);
            expectations.expect(result.plan != nullptr, "a document with Image Statistics compiles");
            if (!result.plan)
                continue;
            const runtime::CompiledValueImageStatistics* kernel = nullptr;
            std::uint8_t outputs = 0;
            for (const auto& operation : result.plan->valueOperations())
                if (const auto* item = std::get_if<runtime::CompiledValueImageStatistics>(&operation.kernel)) {
                    kernel = item;
                    outputs = operation.outputCount;
                }
            if (!kernel)
                continue;
            expectations.expect(outputs == 5 && kernel->node == statsId,
                                "the value operation has the five outputs");
            if (!wired || muted) {
                expectations.expect(kernel->plan == nullptr,
                                    "an unwired or muted node has no picture to measure");
                continue;
            }
            require(kernel->plan != nullptr, "a wired node carries the plan of its picture");
            const auto operations = kernel->plan->operations();
            std::vector<const runtime::StatisticsKernel*> passes;
            for (const auto& operation : operations)
                if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation))
                    if (const auto* pass = std::get_if<runtime::StatisticsKernel>(&effect->kernel))
                        passes.push_back(pass);
            require(passes.size() == 2, "the plan reduces the picture in two passes");
            expectations.expect(passes[0]->role == runtime::StatisticsRole::Reduce &&
                                    passes[1]->role == runtime::StatisticsRole::Sum &&
                                    passes[0]->channel == kStatisticsAlpha && !passes[0]->straight &&
                                    passes[0]->opaqueOnly && passes[1]->opaqueOnly,
                                "both passes carry the node's choices");
            expectations.expect(
                std::ranges::none_of(result.plan->operations(), [](const auto& operation) {
                    const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
                    return effect != nullptr &&
                           std::holds_alternative<runtime::StatisticsKernel>(effect->kernel);
                }),
                "the picture and its reduction are not part of the plan that contains the node");
            expectations.expect(std::ranges::any_of(operations, [](const auto& operation) {
                                    return std::holds_alternative<runtime::CompiledSolid>(operation);
                                }),
                                "the picture the node's input ends in is part of its plan");
        }
    }
    // A node that reads a picture that depends on its own result is refused, not looped over.
    {
        const auto statsId = NodeId::fromRaw(996);
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = 3200;
        for (const auto& parameter : definition->parameters) {
            const auto id = ParameterId::fromRaw(next++);
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{parameter.defaultValue}}),
                    "statistics parameter is valid");
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({statsId, std::string(kImageStatisticsNodeType), std::move(bindings), 1}),
                "statistics node is accepted");
        require(graph.addEdge({EdgeId::fromRaw(995), {kFirstSolidNode, "image"},
                               NodeInputRef{statsId, "input"}}),
                "the source feeds the statistics node");
        // The source's own opacity-less width is driven by the node that reads the source.
        const auto solid = std::ranges::find_if(graph.nodes(), [](const auto& node) {
            return node.id == kFirstSolidNode;
        });
        require(solid != graph.nodes().end(), "the source node exists");
        const auto width = std::ranges::find_if(solid->parameters, [](const auto& binding) {
            return binding.role == "width";
        });
        require(width != solid->parameters.end(), "the source has a width");
        require(composition->parameters().setSource(width->parameterId,
                                                    DriverBindingSource{statsId, "maximum"}),
                "the maximum drives the width of the picture it measures");
        require(composition->parameters().setSource(kFirstOpacity,
                                                    DriverBindingSource{statsId, "mean"}),
                "the mean drives the layer");
        // The document refuses the loop before a compile could meet it; the compiler's own guard
        // (a node in its own statistics stack) is the second line.
        expectations.expect(!project.validate().ok(),
                            "an Image Statistics node that measures its own result is not a valid "
                            "document");
    }
}

// #505 Vignette lowers to one vignette kernel with its constant shape and mode. The mode keeps the
// identity of only the strength it reads (Multiply the amount, Exposure the stops); the other stays
// the kernel's constant default without an identity. Every shared value and the centre keep theirs.
// A muted node passes its Image through.
void testVignetteLowering(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    const auto effectId = NodeId::fromRaw(996);
    const auto* definition = builtInNodeDefinitions().find(kVignetteNodeType, 1);
    require(definition != nullptr, "the vignette definition exists");
    for (const auto mode : {kVignetteMultiply, kVignetteExposure}) {
        for (const bool muted : {false, true}) {
            auto project = makeProject(singleLayerOptions());
            auto* composition = project.findComposition(kCompositionId);
            auto& graph = composition->graph();
            std::vector<ParameterBinding> bindings;
            std::map<std::string, ParameterId> ids;
            std::uint64_t next = 2800;
            for (const auto& parameter : definition->parameters) {
                auto value = parameter.defaultValue;
                if (parameter.role == "mode")
                    value = mode;
                if (parameter.role == "shape")
                    value = kVignetteRectangular;
                if (parameter.role == "feather")
                    value = 0.25;
                if (parameter.role == "center")
                    value = Vec2d{-32.0, 18.5};
                const auto id = ParameterId::fromRaw(next++);
                require(composition->parameters().insert(
                            {id, parameter.schemaKey, ConstantValueSource{value}}),
                        "vignette parameter is valid");
                ids[parameter.role] = id;
                bindings.push_back({parameter.role, id});
            }
            require(graph.addNode({effectId, std::string(kVignetteNodeType), std::move(bindings),
                                   1}),
                    "vignette node is accepted");
            require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
            require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                                   NodeInputRef{effectId, "input"}}),
                    "source feeds the vignette");
            require(graph.addEdge({EdgeId::fromRaw(996), {effectId, "image"},
                                   NodeInputRef{kFirstLayerNode,
                                                std::string(kLayerOutputContentInputPort)}}),
                    "vignette feeds Layer");
            if (muted)
                composition->nodeLayout()[effectId].muted = true;
            expectations.expect(project.validate().ok(), "vignette graph is valid document truth");
            const auto result = compile(std::move(project), registry);
            expectations.expect(result.plan != nullptr, "vignette graph compiles");
            if (!result.plan)
                continue;
            const auto effect = std::ranges::find_if(result.plan->operations(), [&](const auto& op) {
                const auto* item = std::get_if<runtime::CompiledImageEffect>(&op);
                return item && item->sourceNodeId == effectId;
            });
            if (muted) {
                expectations.expect(effect == result.plan->operations().end(),
                                    "a muted vignette passes its Image through");
                continue;
            }
            require(effect != result.plan->operations().end(),
                    "vignette lowers to an image effect");
            const auto* kernel = std::get_if<runtime::VignetteKernel>(
                &std::get<runtime::CompiledImageEffect>(*effect).kernel);
            require(kernel != nullptr, "vignette lowers to a vignette kernel");
            const bool multiply = mode == kVignetteMultiply;
            const auto owns = [&](const ParameterId id, const char* role, const bool used) {
                return used ? id == ids.at(role) : !id.isValid();
            };
            expectations.expect(
                kernel->shape == runtime::VignetteShape::Rectangular &&
                    static_cast<std::int64_t>(kernel->mode) == mode &&
                    owns(kernel->amount.id, "amount", multiply) &&
                    owns(kernel->exposure.id, "exposure", !multiply) &&
                    owns(kernel->midpoint.id, "midpoint", true) &&
                    owns(kernel->roundness.id, "roundness", true) &&
                    owns(kernel->feather.id, "feather", true) &&
                    std::get<double>(kernel->feather.source) == 0.25 &&
                    owns(kernel->highlightProtection.id, "highlightProtection", true) &&
                    owns(kernel->center.id, "center", true) &&
                    std::get<Vec2d>(kernel->center.source) == Vec2d{-32.0, 18.5},
                "vignette lowers its shape, its mode and only the strength that mode reads");
        }
    }
}

// #514 Denoise & Smooth: Median, Bilateral and Spatial lower to one neighborhood kernel with the
// authored radius and strength; Temporal lowers to the look-back window and one weighted step per
// earlier frame; a strength wired from Image Statistics over a flat patch reaches the kernel as a
// driven value, not a constant.
void testDenoiseLowering(Expectations& expectations) {
    using namespace document;
    const auto* definition = builtInNodeDefinitions().find(kDenoiseNodeType, 1);
    require(definition != nullptr && definition->parameters.size() == 8,
            "Denoise has mode, radius, strength, frames, Auto strength, the sample region and the "
            "effect mask channel");
    for (int scenario = 0; scenario != 6; ++scenario) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto node = NodeId::fromRaw(970);
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = 970;
        for (const auto& parameter : definition->parameters) {
            const auto id = ParameterId::fromRaw(next++);
            auto value = parameter.defaultValue;
            if (parameter.role == "mode")
                value = std::int64_t{scenario <= 3 ? scenario : (scenario == 5 ? 3 : 1)};
            if (parameter.role == "radius")
                value = 3.0;
            if (parameter.role == "strength")
                value = 0.25;
            if (parameter.role == "frames")
                value = std::int64_t{4};
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{value}}),
                    "denoise parameter is valid");
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({node, std::string(kDenoiseNodeType), std::move(bindings), 1}),
                "denoise node adds");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge removed");
        require(graph.addEdge(
                    {kFirstSourceEdge, {kFirstSolidNode, "image"}, NodeInputRef{node, "input"}}),
                "image enters denoise");
        require(graph.addEdge(
                    {EdgeId::fromRaw(970),
                     {node, "image"},
                     NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
                "denoise enters Layer");
        if (scenario == 4) {
            // The noise profile: Image Statistics over the picture, its deviation driving strength.
            const auto* statistics = builtInNodeDefinitions().find(kImageStatisticsNodeType, 1);
            require(statistics != nullptr, "Image Statistics exists");
            std::vector<ParameterBinding> statBindings;
            for (const auto& parameter : statistics->parameters) {
                const auto id = ParameterId::fromRaw(next++);
                require(composition->parameters().insert(
                            {id, parameter.schemaKey, ConstantValueSource{parameter.defaultValue}}),
                        "statistics parameter is valid");
                statBindings.push_back({parameter.role, id});
            }
            const auto stats = NodeId::fromRaw(971);
            require(graph.addNode({stats, std::string(kImageStatisticsNodeType),
                                   std::move(statBindings), 1}),
                    "statistics node adds");
            require(graph.addEdge({EdgeId::fromRaw(971),
                                   {kFirstSolidNode, "image"},
                                   NodeInputRef{stats, "input"}}),
                    "a patch enters statistics");
            const auto strength = std::ranges::find_if(graph.nodes(), [&](const auto& candidate) {
                return candidate.id == node;
            });
            require(strength != graph.nodes().end(), "the denoise node exists");
            const auto binding = std::ranges::find_if(strength->parameters, [](const auto& b) {
                return b.role == "strength";
            });
            require(binding != strength->parameters.end(), "the node has a strength");
            require(composition->parameters().setSource(binding->parameterId,
                                                        DriverBindingSource{stats, "deviation"}),
                    "the deviation drives strength");
        }
        if (scenario == 5) {
            // Temporal reads the strength (its tolerance between frames) but not the radius: a key
            // on the radius (left from another mode) must not leave a curve nothing reads, which
            // would refuse the whole plan, and the keyed strength drives the steps.
            std::uint64_t curve = 1500;
            for (const char* role : {"radius", "strength"}) {
                const auto owner = std::ranges::find_if(graph.nodes(), [&](const auto& candidate) {
                    return candidate.id == node;
                });
                const auto binding = std::ranges::find_if(owner->parameters, [&](const auto& b) {
                    return b.role == role;
                });
                const auto curveId = AnimationCurveId::fromRaw(curve);
                require(composition->animationCurves().insert(ScalarAnimationCurve{
                            curveId,
                            {{KeyframeId::fromRaw(curve + 1), core::RationalTime::fromInteger(0), 0.0,
                              KeyframeInterpolation::Linear},
                             {KeyframeId::fromRaw(curve + 2), core::RationalTime::fromInteger(1), 1.0,
                              KeyframeInterpolation::Linear}}}),
                        "a denoise curve is publishable");
                require(composition->parameters().setSource(binding->parameterId,
                                                            AnimationCurveSource{curveId}),
                        "the denoise value animates");
                curve += 10;
            }
        }
        expectations.expect(project.validate().ok(), "denoise document validates");
        runtime::NodeDefinitionRegistry registry;
        require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
        registry.freeze();
        const auto result = compile(std::move(project), registry);
        expectations.expect(result.plan != nullptr, "denoise graph compiles");
        if (!result.plan)
            continue;
        std::size_t denoises = 0;
        std::size_t samples = 0;
        std::size_t mixes = 0;
        std::size_t steps = 0;
        bool stepsWellFormed = true;
        const runtime::DenoiseKernel* kernel = nullptr;
        std::optional<runtime::OperationIndex> current;
        for (std::size_t index = 0; index < result.plan->operations().size(); ++index) {
            const auto* effect =
                std::get_if<runtime::CompiledImageEffect>(&result.plan->operations()[index]);
            if (effect == nullptr)
                continue;
            if (const auto* found = std::get_if<runtime::DenoiseKernel>(&effect->kernel)) {
                if (found->mode == runtime::DenoiseMode::Temporal) {
                    // Each step reads the current frame and one earlier frame; the first's mean is
                    // the current frame and the last ends the chain.
                    stepsWellFormed = stepsWellFormed && effect->fieldImages.size() == 2 &&
                                      found->first == (steps == 0) && found->last == (steps == 2) &&
                                      (!current || effect->fieldImages[0] == *current) &&
                                      (steps != 0 || effect->input == effect->fieldImages[0]);
                    current = effect->fieldImages[0];
                    ++steps;
                } else {
                    ++denoises;
                }
                kernel = found;
            }
            samples += std::holds_alternative<runtime::TimeSampleKernel>(effect->kernel) ? 1U : 0U;
            if (const auto* combine = std::get_if<runtime::ImageCombineKernel>(&effect->kernel))
                mixes += combine->mode == runtime::ImageCombineMode::Mix ? 1U : 0U;
        }
        if (scenario == 3 || scenario == 5) {
            expectations.expect(denoises == 0 && samples == 3 && mixes == 0 && steps == 3 &&
                                    stepsWellFormed,
                                "Temporal lowers to the look-back window and one weighted step per "
                                "earlier frame");
            if (kernel != nullptr && scenario == 3)
                expectations.expect(std::holds_alternative<double>(kernel->strength.source) &&
                                        std::get<double>(kernel->strength.source) == 0.25,
                                    "the temporal steps keep the authored strength");
            if (scenario == 5) {
                expectations.expect(result.plan->scalarCurves().size() == 1,
                                    "Temporal compiles the strength's curve and not the radius's");
                runtime::EvaluationRequest request{core::RationalTime::fromInteger(1),
                                                   result.plan->output(),
                                                   runtime::CompositionFormatResolution{}};
                request.quality = runtime::EvaluationQuality::Reference;
                request.colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene;
                request.pixelStorageByteLimit = std::size_t{1} << 29U;
                request.bypassOperationCache = true;
                expectations.expect(
                    runtime::CpuCompositionEvaluator{}.evaluate(result.plan, request, {}).frame() !=
                        nullptr,
                    "a Temporal denoise with a keyed radius and strength renders");
            }
        } else {
            expectations.expect(denoises == 1 && samples == 0 && kernel != nullptr &&
                                    static_cast<int>(kernel->mode) == (scenario == 4 ? 1 : scenario),
                                "a spatial mode lowers to one neighborhood kernel");
            if (kernel != nullptr && scenario != 4)
                expectations.expect(std::holds_alternative<double>(kernel->radius.source) &&
                                        std::get<double>(kernel->radius.source) == 3.0 &&
                                        std::holds_alternative<double>(kernel->strength.source) &&
                                        std::get<double>(kernel->strength.source) == 0.25,
                                    "the kernel keeps the authored radius and strength");
            if (kernel != nullptr && scenario == 4)
                expectations.expect(!std::holds_alternative<double>(kernel->strength.source),
                                    "a strength wired from Image Statistics is a driven value");
        }
    }
}
