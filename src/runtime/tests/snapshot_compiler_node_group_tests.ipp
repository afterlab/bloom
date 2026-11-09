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

// ---------------------------------------------------------------------------------------------
// #590 shared function definitions: a Function node reads the same group a Node Group instance
// does, as a function of one number.

constexpr auto kDecayComposition = document::CompositionId::fromRaw(7);
constexpr auto kDecayInput = document::NodeId::fromRaw(5601);
constexpr auto kDecayOutput = document::NodeId::fromRaw(5602);
constexpr auto kDecaySine = document::NodeId::fromRaw(5603);
constexpr auto kDecayNegate = document::NodeId::fromRaw(5604);
constexpr auto kDecayExp = document::NodeId::fromRaw(5605);
constexpr auto kDecayProduct = document::NodeId::fromRaw(5606);
constexpr auto kFunctionA = document::NodeId::fromRaw(6201);
constexpr auto kFunctionB = document::NodeId::fromRaw(6202);
constexpr auto kFunctionSum = document::NodeId::fromRaw(6203);

void connectDriver(document::Composition& composition, const document::NodeId reader,
                   const std::string_view role, const document::NodeId source,
                   const std::string& port = std::string(document::kResultPortName)) {
    require(composition.parameters().setSource(parameterOf(composition, reader, role),
                                               document::DriverBindingSource{source, port}),
            "the parameter reads its source");
}

// y = sin(x) * exp(-x), built from real Math nodes.
[[nodiscard]] document::Composition makeDecayGroup(const core::primitives::ScalarPrimitive combine) {
    using namespace document;
    using core::primitives::ScalarPrimitive;
    auto group = makeStackedComposition(kDecayComposition, "Decay", 5700);
    std::uint64_t next = 5800;
    addDefaultedNode(group, kDecayInput, kGroupInputNodeType, next);
    addDefaultedNode(group, kDecayOutput, kGroupOutputNodeType, next);
    for (const auto& [id, operation] :
         {std::pair{kDecaySine, ScalarPrimitive::Sine}, std::pair{kDecayNegate, ScalarPrimitive::Negate},
          std::pair{kDecayExp, ScalarPrimitive::Exp}, std::pair{kDecayProduct, combine}})
        addDefaultedNode(group, id, kScalarMathNodeType, next,
                         {{"operation", scalarOperationStoredValue(operation)}});
    connectDriver(group, kDecaySine, "a", kDecayInput, "scalar1");
    connectDriver(group, kDecayNegate, "a", kDecayInput, "scalar1");
    connectDriver(group, kDecayExp, "a", kDecayNegate);
    connectDriver(group, kDecayProduct, "a", kDecaySine);
    connectDriver(group, kDecayProduct, "b", kDecayExp);
    connectDriver(group, kDecayOutput, "scalar1", kDecayProduct);
    return group;
}

// The main composition with two Function nodes reading Decay at `a` and `b`. Their sum drives the
// first layer's opacity so both are reachable.
[[nodiscard]] document::Project
makeFunctionProject(const double a, const double b,
                    const core::primitives::ScalarPrimitive combine =
                        core::primitives::ScalarPrimitive::Multiply) {
    using namespace document;
    auto project = makeProject(singleLayerOptions());
    require(project.addComposition(makeDecayGroup(combine)), "the function group is added");
    auto* composition = project.findComposition(kCompositionId);
    std::uint64_t next = 6300;
    const auto group = static_cast<std::int64_t>(kDecayComposition.value());
    addDefaultedNode(*composition, kFunctionA, kFunctionNodeType, next, {{"group", group}, {"x", a}});
    addDefaultedNode(*composition, kFunctionB, kFunctionNodeType, next, {{"group", group}, {"x", b}});
    addDefaultedNode(*composition, kFunctionSum, kScalarMathNodeType, next,
                     {{"operation",
                       scalarOperationStoredValue(core::primitives::ScalarPrimitive::Add)}});
    connectDriver(*composition, kFunctionSum, "a", kFunctionA, "y");
    connectDriver(*composition, kFunctionSum, "b", kFunctionB, "y");
    require(composition->parameters().setSource(
                kFirstOpacity, DriverBindingSource{kFunctionSum, std::string(kResultPortName)}),
            "the layer opacity reads the sum");
    const auto validation = project.validate();
    for (const auto& issue : validation.issues())
        std::cerr << "function fixture: " << issue.path << ": " << issue.message << '\n';
    require(validation.ok(), "the function fixture is valid document truth");
    return project;
}

[[nodiscard]] std::vector<const runtime::CompiledValueOperation*>
functionOperations(const runtime::CompiledCompositionPlan& plan) {
    std::vector<const runtime::CompiledValueOperation*> found;
    for (const auto& operation : plan.valueOperations())
        if (std::holds_alternative<runtime::CompiledValueFunction>(operation.kernel))
            found.push_back(&operation);
    return found;
}

void testFunctionGroups(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    const double kFirst = 0.5;
    const double kSecond = 2.0;
    const auto result = compile(makeFunctionProject(kFirst, kSecond), registry);
    expectations.expect(result.plan != nullptr, "two Function nodes reading one group compile");
    if (!result.plan)
        return;
    const auto functions = functionOperations(*result.plan);
    expectations.expect(functions.size() == 2, "each Function node is one operation");
    if (functions.size() != 2)
        return;
    // The group's own nodes live in the function's body, not in the plan's main value program.
    expectations.expect(
        std::ranges::none_of(result.plan->valueOperations(),
                             [](const runtime::CompiledValueOperation& operation) {
                                 return std::holds_alternative<runtime::CompiledValueGroupBoundary>(
                                     operation.kernel);
                             }) &&
            result.plan->valueOperations().size() == 3,
        "the group's nodes are cut out of the main value program");
    const auto& body = *std::get<runtime::CompiledValueFunction>(functions[0]->kernel).body;
    const auto& otherBody = *std::get<runtime::CompiledValueFunction>(functions[1]->kernel).body;
    bool sameShape = otherBody.operations.size() == body.operations.size();
    for (std::size_t index = 0; sameShape && index < body.operations.size(); ++index)
        sameShape = body.operations[index].kernel.index() == otherBody.operations[index].kernel.index();
    expectations.expect(sameShape && body.result == otherBody.result,
                        "both Function nodes run a body of the same shape");
    expectations.expect(body.operations.size() == 6 && body.inputOperation.has_value() &&
                            !body.timeDependent,
                        "the body is the six operations the group holds and does not read time");

    // Values against the known answer, computed independently with the standard library.
    const auto evaluation = runtime::evaluateValueGraph(
        result.plan->valueOperations(), result.plan->valueOutputCount(),
        core::RationalTime::fromInteger(0), result.plan->format().frameRate());
    expectations.expect(evaluation.diagnostics.empty(), "the function graph evaluates cleanly");
    const auto read = [&](const runtime::CompiledValueOperation* operation) {
        const auto* value = std::get_if<double>(&evaluation.outputs.at(operation->firstOutput.value()));
        return value == nullptr ? std::nan("") : *value;
    };
    const auto known = [](const double x) { return std::sin(x) * std::exp(-x); };
    expectations.expect(std::abs(read(functions[0]) - known(kFirst)) < 1e-12 &&
                            std::abs(read(functions[1]) - known(kSecond)) < 1e-12,
                        "a Function node answers sin(x) * exp(-x) at its own argument");

    // One evaluator serves many arguments, each matching the known answer.
    runtime::FunctionEvaluator evaluator(body, core::RationalTime::fromInteger(0),
                                         result.plan->format().frameRate(), {});
    bool allMatch = true;
    for (int index = 0; index < 256; ++index) {
        const double x = -3.0 + 0.0625 * index;
        const auto value = evaluator.at(x);
        allMatch = allMatch && value.has_value() && std::abs(*value - known(x)) < 1e-12;
    }
    expectations.expect(allMatch && evaluator.diagnostics().empty(),
                        "one body evaluator matches the known answer over 256 arguments");

    // Editing the one definition reaches both consumers.
    const auto edited = compile(makeFunctionProject(kFirst, kSecond, core::primitives::ScalarPrimitive::Add),
                                registry);
    expectations.expect(edited.plan != nullptr, "the edited function group compiles");
    if (edited.plan) {
        const auto editedFunctions = functionOperations(*edited.plan);
        const auto editedEvaluation = runtime::evaluateValueGraph(
            edited.plan->valueOperations(), edited.plan->valueOutputCount(),
            core::RationalTime::fromInteger(0), edited.plan->format().frameRate());
        const auto sum = [&](const double x) { return std::sin(x) + std::exp(-x); };
        const auto at = [&](const std::size_t index) {
            const auto* value = std::get_if<double>(
                &editedEvaluation.outputs.at(editedFunctions.at(index)->firstOutput.value()));
            return value == nullptr ? std::nan("") : *value;
        };
        expectations.expect(editedFunctions.size() == 2 && std::abs(at(0) - sum(kFirst)) < 1e-12 &&
                                std::abs(at(1) - sum(kSecond)) < 1e-12,
                            "editing the group changes every Function that reads it");
    }

    // A Function naming no group is refused with a diagnostic on that node.
    auto unset = makeFunctionProject(kFirst, kSecond);
    auto* composition = unset.findComposition(kCompositionId);
    require(composition->parameters().setSource(parameterOf(*composition, kFunctionA, "group"),
                                                ConstantValueSource{std::int64_t{0}}),
            "the group is cleared");
    const auto refused = compile(std::move(unset), registry);
    expectations.expect(refused.plan == nullptr &&
                            hasDiagnostic(refused, runtime::CompileDiagnosticCode::TopologyInvariant,
                                          kFunctionA),
                        "a Function naming no group is refused with a diagnostic on that node");
}

// A Node Group instance and a Function read the same group: the number the instance publishes is
// the number the Function answers at the same argument, and the Function's body holds no pixels.
void testFunctionSharesGroupWithInstance(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    auto project = makeGroupedProject(false, 1.0, false);
    auto* composition = project.findComposition(kCompositionId);
    std::uint64_t next = 6400;
    addDefaultedNode(*composition, kFunctionA, kFunctionNodeType, next,
                     {{"group", static_cast<std::int64_t>(kGroupComposition.value())}, {"x", 0.25}});
    addDefaultedNode(*composition, kFunctionSum, kScalarMathNodeType, next,
                     {{"operation",
                       scalarOperationStoredValue(core::primitives::ScalarPrimitive::Add)}});
    require(composition->parameters().setSource(parameterOf(*composition, kInstanceA, "scalar1"),
                                                ConstantValueSource{0.25}),
            "the instance is given 0.25");
    connectDriver(*composition, kFunctionSum, "a", kInstanceA, "scalar1");
    connectDriver(*composition, kFunctionSum, "b", kFunctionA, "y");
    require(composition->parameters().setSource(
                kFirstOpacity, DriverBindingSource{kFunctionSum, std::string(kResultPortName)}),
            "the layer opacity reads the sum");
    require(project.validate().ok(), "the shared-group fixture is valid document truth");
    const auto withFunction = compile(std::move(project), registry);
    const auto without = compile(makeGroupedProject(false, 1.0, false), registry);
    expectations.expect(withFunction.plan != nullptr && without.plan != nullptr,
                        "a group read by an instance and a Function compiles");
    if (!withFunction.plan || !without.plan)
        return;
    expectations.expect(operationShape(*withFunction.plan) == operationShape(*without.plan),
                        "the Function adds no image operation: its body holds no pixels");
    const auto functions = functionOperations(*withFunction.plan);
    require(functions.size() == 1, "the plan holds one Function");
    const auto evaluation = runtime::evaluateValueGraph(
        withFunction.plan->valueOperations(), withFunction.plan->valueOutputCount(),
        core::RationalTime::fromInteger(0), withFunction.plan->format().frameRate());
    const auto* answer =
        std::get_if<double>(&evaluation.outputs.at(functions[0]->firstOutput.value()));
    // The instance's Group Output is the last boundary operation in the main program.
    const runtime::CompiledValueOperation* boundary = nullptr;
    for (const auto& operation : withFunction.plan->valueOperations())
        if (std::holds_alternative<runtime::CompiledValueGroupBoundary>(operation.kernel))
            boundary = &operation;
    require(boundary != nullptr, "the instance's boundary operations are in the main program");
    // Group Output republishes its scalar slot; the instance's answer is the one number there.
    std::vector<double> published;
    for (std::size_t slot = 0; slot < boundary->outputCount; ++slot)
        if (const auto* value = std::get_if<double>(
                &evaluation.outputs.at(boundary->firstOutput.value() + slot)))
            published.push_back(*value);
    expectations.expect(evaluation.diagnostics.empty() && answer != nullptr && *answer == 0.75 &&
                            std::ranges::count(published, 0.75) == 1,
                        "the Function answers what the instance publishes at the same argument");
}
