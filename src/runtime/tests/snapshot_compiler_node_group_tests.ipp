// #342 Node Group: a group definition is a composition that holds a Group Output; the compiler
// replaces every instance with the nodes of the group it names. These tests build real groups out
// of real nodes and prove the replacement is exact: the plan a group compiles to is the plan the
// same nodes compile to without it, on the CPU reference, and so the GPU route (which reads the
// plan) takes the same operations.

constexpr auto kGroupComposition = document::CompositionId::fromRaw(3);
constexpr auto kGroupInputNode = document::NodeId::fromRaw(5001);
constexpr auto kGroupOutputNode = document::NodeId::fromRaw(5002);
constexpr auto kGroupEffectNode = document::NodeId::fromRaw(5003);
constexpr auto kGroupMathNode = document::NodeId::fromRaw(5004);
constexpr auto kInstanceA = document::NodeId::fromRaw(6001);
constexpr auto kInstanceB = document::NodeId::fromRaw(6002);

// A node with every parameter its registered definition declares, at the definition's default unless
// `overrides` names the role.
void addDefaultedNode(document::Composition& composition, const document::NodeId id,
                      const std::string_view typeId, std::uint64_t& nextParameter,
                      const std::map<std::string, document::ParameterValue>& overrides = {}) {
    using namespace document;
    const auto* definition = builtInNodeDefinitions().find(typeId, 1);
    require(definition != nullptr, "the node type is registered");
    NodeRecord node{id, std::string(typeId), {}, 1};
    for (const auto& parameter : definition->parameters) {
        const auto parameterId = ParameterId::fromRaw(nextParameter++);
        const auto found = overrides.find(parameter.role);
        require(composition.parameters().insert(
                    {parameterId, parameter.schemaKey,
                     ConstantValueSource{found == overrides.end() ? parameter.defaultValue
                                                                  : found->second}}),
                "the node's parameter is valid");
        node.parameters.push_back({parameter.role, parameterId});
    }
    require(composition.graph().addNode(std::move(node)), "the node is accepted");
}

[[nodiscard]] document::ParameterId parameterOf(const document::Composition& composition,
                                                const document::NodeId node,
                                                const std::string_view role) {
    const auto* record = composition.graph().findNode(node);
    require(record != nullptr, "the node exists");
    const auto binding = std::ranges::find(record->parameters, role, &document::ParameterBinding::role);
    require(binding != record->parameters.end(), "the node binds the role");
    return binding->parameterId;
}

// A composition like any other: a stack and an output, ids from `base`.
[[nodiscard]] document::Composition makeStackedComposition(const document::CompositionId id,
                                                           const std::string& name,
                                                           const std::uint64_t base) {
    using namespace document;
    const auto stack = NodeId::fromRaw(base);
    const auto output = NodeId::fromRaw(base + 1);
    CanonicalGraph graph(stack);
    require(graph.addNode({stack, std::string(kLayerStackNodeType), {}, kLayerStackNodeSchemaVersion}) &&
                graph.addNode({output, std::string(kCompositionOutputNodeType), {},
                               kCompositionOutputNodeSchemaVersion}) &&
                graph.addEdge({EdgeId::fromRaw(base + 2),
                               {stack, std::string(kLayerStackOutputPort)},
                               NodeInputRef{output, std::string(kCompositionOutputInputPort)}}),
            "the composition has a stack and an output");
    graph.setCompositionOutput({output, std::string(kCompositionOutputOutputPort)});
    return Composition(id, name, core::RationalTime::fromInteger(5), std::move(graph),
                       CompositionFormat{});
}

// The group "Brighten": Group Input image1 -> Exposure -> Group Output image1, and the number on
// scalar1 plus one half out on scalar1.
[[nodiscard]] document::Composition makeGroupDefinition(const double stops) {
    using namespace document;
    auto group = makeStackedComposition(kGroupComposition, "Brighten", 5300);
    std::uint64_t next = 5100;
    addDefaultedNode(group, kGroupInputNode, kGroupInputNodeType, next);
    addDefaultedNode(group, kGroupOutputNode, kGroupOutputNodeType, next);
    addDefaultedNode(group, kGroupEffectNode, "bloom.exposure", next, {{"amount", stops}});
    addDefaultedNode(group, kGroupMathNode, kScalarMathNodeType, next,
                     {{"operation", scalarOperationStoredValue(core::primitives::ScalarPrimitive::Add)}});
    require(group.graph().addEdge({EdgeId::fromRaw(5201),
                                   {kGroupInputNode, "image1"},
                                   NodeInputRef{kGroupEffectNode, "input"}}),
            "Group Input feeds the effect");
    require(group.graph().addEdge({EdgeId::fromRaw(5202),
                                   {kGroupEffectNode, "image"},
                                   NodeInputRef{kGroupOutputNode, "image1"}}),
            "the effect feeds Group Output");
    // a = Group Input scalar1, b = 0.5; Group Output scalar1 = the sum.
    require(group.parameters().setSource(parameterOf(group, kGroupMathNode, "a"),
                                         DriverBindingSource{kGroupInputNode, "scalar1"}),
            "the sum reads Group Input");
    require(group.parameters().setSource(parameterOf(group, kGroupMathNode, "b"),
                                         ConstantValueSource{0.5}),
            "the sum adds one half");
    require(group.parameters().setSource(parameterOf(group, kGroupOutputNode, "scalar1"),
                                         DriverBindingSource{kGroupMathNode, std::string(kResultPortName)}),
            "Group Output reads the sum");
    return group;
}

// The main composition with the first layer's source routed through an instance (and the second
// layer's too when `both`), or through the bare effect node when `flat`.
[[nodiscard]] document::Project makeGroupedProject(const bool flat, const double stops,
                                                   const bool both) {
    using namespace document;
    auto project = makeProject(both ? ProjectOptions{} : singleLayerOptions());
    auto* composition = project.findComposition(kCompositionId);
    std::uint64_t next = 6100;
    const auto route = [&](const NodeId instance, const NodeId solid, const EdgeId sourceEdge,
                           const EdgeId outEdge, const NodeId layer) {
        if (flat)
            addDefaultedNode(*composition, instance, "bloom.exposure", next,
                             {{"amount", stops}});
        else
            addDefaultedNode(*composition, instance, kNodeGroupNodeType, next,
                             {{"group", static_cast<std::int64_t>(kGroupComposition.value())}});
        require(composition->graph().eraseEdge(sourceEdge), "the direct source edge is removed");
        require(composition->graph().addEdge({outEdge,
                                              {solid, "image"},
                                              NodeInputRef{instance, flat ? "input" : "image1"}}),
                "the source feeds the group");
        require(composition->graph().addEdge({EdgeId::fromRaw(outEdge.value() + 1000),
                                              {instance, flat ? "image" : "image1"},
                                              NodeInputRef{layer,
                                                           std::string(kLayerOutputContentInputPort)}}),
                "the group feeds the layer");
    };
    route(kInstanceA, kFirstSolidNode, kFirstSourceEdge, EdgeId::fromRaw(7001), kFirstLayerNode);
    if (both)
        route(kInstanceB, kSecondSolidNode, kSecondSourceEdge, EdgeId::fromRaw(7002),
              kSecondLayerNode);
    if (!flat)
        require(project.addComposition(makeGroupDefinition(stops)), "the group definition is added");
    const auto validation = project.validate();
    for (const auto& issue : validation.issues())
        std::cerr << "group fixture: " << issue.path << ": " << issue.message << '\n';
    require(validation.ok(), "the group fixture is valid document truth");
    return project;
}

[[nodiscard]] std::vector<render::Rgba32f>
renderReference(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan) {
    runtime::CpuCompositionEvaluator evaluator;
    const auto evaluated =
        evaluator.evaluate(plan,
                           {.time = core::RationalTime::fromInteger(0),
                            .output = plan->output(),
                            .resolution = runtime::CompositionFormatResolution{},
                            .quality = runtime::EvaluationQuality::Reference,
                            .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                            .pixelStorageByteLimit = 1U << 28U},
                           runtime::CancellationToken{});
    require(evaluated.frame() != nullptr, "the group composition renders");
    const auto pixels = evaluated.frame()->processImage().pixels();
    return {pixels.begin(), pixels.end()};
}

// Operations by kind, in order: the shape of a plan with the node ids taken out.
[[nodiscard]] std::vector<std::size_t> operationShape(const runtime::CompiledCompositionPlan& plan) {
    std::vector<std::size_t> shape;
    for (const auto& operation : plan.operations()) {
        shape.push_back(operation.index());
        if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation))
            shape.push_back(effect->kernel.index());
    }
    return shape;
}

void testNodeGroupExpansion(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    const auto flat = compile(makeGroupedProject(true, 1.0, true), registry);
    const auto grouped = compile(makeGroupedProject(false, 1.0, true), registry);
    expectations.expect(flat.plan != nullptr && grouped.plan != nullptr,
                        "the grouped and the flat graph both compile");
    if (!flat.plan || !grouped.plan)
        return;
    expectations.expect(operationShape(*flat.plan) == operationShape(*grouped.plan),
                        "a group compiles to the operations its nodes compile to without it");
    const auto flatPixels = renderReference(flat.plan);
    const auto groupedPixels = renderReference(grouped.plan);
    expectations.expect(!flatPixels.empty() && flatPixels == groupedPixels,
                        "a group renders the same pixels on the CPU reference as its nodes do");

    // Two instances share one definition: one edit to it changes both layers, and still matches the
    // flat graph with the same edit.
    auto edited = makeGroupedProject(false, 1.0, true);
    auto* definition = edited.findComposition(kGroupComposition);
    require(definition != nullptr, "the definition is in the project");
    require(definition->parameters().setSource(parameterOf(*definition, kGroupEffectNode, "amount"),
                                               ConstantValueSource{2.0}),
            "the definition's exposure is edited");
    const auto editedResult = compile(std::move(edited), registry);
    const auto flatEdited = compile(makeGroupedProject(true, 2.0, true), registry);
    expectations.expect(editedResult.plan && flatEdited.plan &&
                            renderReference(editedResult.plan) == renderReference(flatEdited.plan) &&
                            renderReference(editedResult.plan) != groupedPixels,
                        "editing the definition updates every instance");
}

void testNodeGroupValues(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    auto project = makeGroupedProject(false, 1.0, false);
    auto* composition = project.findComposition(kCompositionId);
    // The instance takes 0.25 on scalar1 and answers 0.75; the first layer's opacity reads the answer.
    require(composition->parameters().setSource(parameterOf(*composition, kInstanceA, "scalar1"),
                                                ConstantValueSource{0.25}),
            "the instance is given 0.25");
    require(composition->parameters().setSource(kFirstOpacity,
                                                DriverBindingSource{kInstanceA, "scalar1"}),
            "the layer opacity reads the instance");
    require(project.validate().ok(), "the grouped value graph is valid document truth");
    const auto result = compile(std::move(project), registry);
    expectations.expect(result.plan != nullptr, "a group that passes a number compiles");
    if (!result.plan)
        return;
    const auto layer = std::ranges::find_if(result.plan->operations(), [](const auto& operation) {
        return std::holds_alternative<runtime::CompiledLayerOutput>(operation);
    });
    require(layer != result.plan->operations().end(), "the plan holds the layer");
    const auto* driven = std::get_if<runtime::ValueOutputIndex>(
        &std::get<runtime::CompiledLayerOutput>(*layer).opacity.source);
    expectations.expect(driven != nullptr, "the opacity reads a value output");
    if (driven == nullptr)
        return;
    const auto evaluation = runtime::evaluateValueGraph(
        result.plan->valueOperations(), result.plan->valueOutputCount(),
        core::RationalTime::fromInteger(0), result.plan->format().frameRate());
    const auto* value = driven->value() < evaluation.outputs.size()
                            ? std::get_if<double>(&evaluation.outputs[driven->value()])
                            : nullptr;
    expectations.expect(evaluation.diagnostics.empty() && value != nullptr && *value == 0.75,
                        "the number a group computes (0.25 + 0.5) reaches its reader");
}

void testNodeGroupRefusals(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    // An instance naming no group does not compile, and says why.
    auto unset = makeGroupedProject(false, 1.0, false);
    auto* composition = unset.findComposition(kCompositionId);
    require(composition->parameters().setSource(parameterOf(*composition, kInstanceA, "group"),
                                                ConstantValueSource{std::int64_t{0}}),
            "the group is cleared");
    const auto result = compile(std::move(unset), registry);
    expectations.expect(result.plan == nullptr &&
                            hasDiagnostic(result, runtime::CompileDiagnosticCode::TopologyInvariant,
                                          kInstanceA),
                        "a Node Group naming no group is refused with a diagnostic on that node");

    // A group that contains an instance of itself is a nesting cycle, refused as document truth.
    auto cyclic = makeGroupedProject(false, 1.0, false);
    auto* definition = cyclic.findComposition(kGroupComposition);
    std::uint64_t next = 5300;
    addDefaultedNode(*definition, NodeId::fromRaw(5010), kNodeGroupNodeType, next,
                     {{"group", static_cast<std::int64_t>(kGroupComposition.value())}});
    expectations.expect(!cyclic.validate().ok(), "a group that contains itself is refused");
    expectations.expect(!cyclic.validateCompositionNesting().ok(),
                        "the nesting check names the cycle");

    // An indirect cycle through a second group.
    auto indirect = makeGroupedProject(false, 1.0, false);
    auto other = makeStackedComposition(CompositionId::fromRaw(4), "Other", 5400);
    std::uint64_t nextOther = 5450;
    addDefaultedNode(other, NodeId::fromRaw(5020), kGroupOutputNodeType, nextOther);
    addDefaultedNode(other, NodeId::fromRaw(5021), kNodeGroupNodeType, nextOther,
                     {{"group", static_cast<std::int64_t>(kGroupComposition.value())}});
    require(indirect.addComposition(std::move(other)), "the second group is added");
    auto* first = indirect.findComposition(kGroupComposition);
    std::uint64_t nextFirst = 5500;
    addDefaultedNode(*first, NodeId::fromRaw(5011), kNodeGroupNodeType, nextFirst,
                     {{"group", std::int64_t{4}}});
    expectations.expect(!indirect.validateCompositionNesting().ok(),
                        "two groups that contain each other are refused");
}
