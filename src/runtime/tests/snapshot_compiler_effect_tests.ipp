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
        require(definition != nullptr && definition->parameters.size() == 1,
                "pointwise definition exists with one scalar control");
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
    require(definition != nullptr && definition->parameters.size() == 6,
            "Transform 2D definition has six parameters");
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
