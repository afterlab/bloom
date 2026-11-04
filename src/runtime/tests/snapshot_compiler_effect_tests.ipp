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
            bool ordered = ids.size() + (type == kHueSaturationNodeType || type == kGradeNodeType
                                             ? 0U
                                             : 1U) ==
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
                        *blue == gradeCurvePath({{0.0, 0.0}, {0.5, 0.8}, {1.0, 1.0}}) &&
                        hueLuma && *hueLuma == gradeCurvePath({{0.25, 1.5}}) && master &&
                        *master == defaultGradeCurve(GradeCurve::Master) && hueHue &&
                        hueHue->anchors.empty(),
                    "Curves lowers its point lists, hue curves and clamp");
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
