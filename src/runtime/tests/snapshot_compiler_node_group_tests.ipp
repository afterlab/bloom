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
// #342 pixel slots other than images: a Mask and a ChannelSet routed through a group, nested
// groups and the depth and node caps.

void wire(document::Composition& composition, std::uint64_t& edge, const document::NodeId from,
          const std::string_view fromPort, const document::NodeId to, const std::string_view toPort) {
    using namespace document;
    require(composition.graph().addEdge({EdgeId::fromRaw(edge++),
                                         {from, std::string(fromPort)},
                                         NodeInputRef{to, std::string(toPort)}}),
            "the link is accepted");
}

[[nodiscard]] document::Composition makeBoundedGroup(const document::CompositionId id,
                                                     const std::string& name,
                                                     const std::uint64_t base,
                                                     const document::NodeId input,
                                                     const document::NodeId output,
                                                     std::uint64_t& parameters) {
    auto group = makeStackedComposition(id, name, base);
    addDefaultedNode(group, input, document::kGroupInputNodeType, parameters);
    addDefaultedNode(group, output, document::kGroupOutputNodeType, parameters);
    return group;
}

[[nodiscard]] bool samePixelBits(const std::vector<render::Rgba32f>& a,
                                 const std::vector<render::Rgba32f>& b) {
    return !a.empty() && a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(render::Rgba32f)) == 0;
}

// The contact sheet of the Mask case when BLOOM_NODE_GROUP_CAPTURE names a directory: the grouped
// render, the flat render and their absolute difference (black when the group changes nothing).
void captureMaskSheet(const std::vector<render::Rgba32f>& grouped,
                      const std::vector<render::Rgba32f>& flat, const std::size_t width,
                      const std::size_t height) {
    const char* directory = std::getenv("BLOOM_NODE_GROUP_CAPTURE");
    if (directory == nullptr || *directory == '\0' || grouped.size() != flat.size() ||
        grouped.size() != width * height)
        return;
    const auto encode = [](float value) {
        value = std::clamp(value, 0.0F, 1.0F);
        const float srgb = value <= 0.0031308F ? 12.92F * value
                                                : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
        return static_cast<char>(static_cast<unsigned char>(std::lround(srgb * 255.0F)));
    };
    const auto write = [&](const std::string& name, const auto& pixel) {
        std::ofstream out(std::string(directory) + "/" + name, std::ios::binary);
        out << "P6\n" << width << ' ' << height << "\n255\n";
        for (std::size_t i = 0; i < grouped.size(); ++i) {
            const auto value = pixel(i);
            for (const float channel : value)
                out.put(encode(channel));
        }
    };
    write("mask-grouped.ppm", [&](const std::size_t i) {
        return std::array<float, 3>{grouped[i].red(), grouped[i].green(), grouped[i].blue()};
    });
    write("mask-flat.ppm", [&](const std::size_t i) {
        return std::array<float, 3>{flat[i].red(), flat[i].green(), flat[i].blue()};
    });
    write("mask-difference.ppm", [&](const std::size_t i) {
        return std::array<float, 3>{std::abs(grouped[i].red() - flat[i].red()),
                                    std::abs(grouped[i].green() - flat[i].green()),
                                    std::abs(grouped[i].blue() - flat[i].blue())};
    });
}

// ---- Mask ----------------------------------------------------------------------------------

enum class MaskCase { Enter, Leave, EnterMuted, LeaveMuted, EnterUnconnected };

constexpr auto kMaskKeyNode = document::NodeId::fromRaw(8001);
constexpr auto kMaskMixNode = document::NodeId::fromRaw(8002);
constexpr auto kMaskGroupKeyNode = document::NodeId::fromRaw(8101);
constexpr auto kMaskGroupMixNode = document::NodeId::fromRaw(8102);

// Solid two is both the second image of an Image Mix and the plate a Luma Key cuts a Mask from; the
// mix of the first layer's plate takes that Mask. `flat` keeps the key and mix in the main
// composition; otherwise the mix (the Mask enters the group at mask1) or the key (the Mask leaves it
// at mask1) sits inside the group and the other outside.
[[nodiscard]] document::Project makeMaskProject(const MaskCase which, const bool flat) {
    using namespace document;
    auto project = makeProject();
    auto* main = project.findComposition(kCompositionId);
    const bool leaves = which == MaskCase::Leave || which == MaskCase::LeaveMuted;
    const bool muted = which == MaskCase::EnterMuted || which == MaskCase::LeaveMuted;
    std::uint64_t parameters = 8200;
    std::uint64_t edges = 8300;
    std::uint64_t groupParameters = 8400;
    std::uint64_t groupEdges = 8500;
    const auto content = std::string(kLayerOutputContentInputPort);
    require(main->graph().eraseEdge(kFirstSourceEdge), "the direct source edge is removed");
    const auto key = [](Composition& composition, const NodeId id, std::uint64_t& next) {
        addDefaultedNode(composition, id, "bloom.luma-key", next, {{"low", 0.0}, {"high", 1.0}});
    };
    const auto mix = [](Composition& composition, const NodeId id, std::uint64_t& next) {
        addDefaultedNode(composition, id, "bloom.image-mix", next, {{"factor", 1.0}});
    };
    if (flat) {
        key(*main, kMaskKeyNode, parameters);
        mix(*main, kMaskMixNode, parameters);
        wire(*main, edges, kSecondSolidNode, "image", kMaskKeyNode, "input");
        wire(*main, edges, kFirstSolidNode, "image", kMaskMixNode, "input");
        wire(*main, edges, kSecondSolidNode, "image", kMaskMixNode, "second");
        if (which != MaskCase::EnterUnconnected)
            wire(*main, edges, kMaskKeyNode, "mask", kMaskMixNode, "mask");
        wire(*main, edges, kMaskMixNode, "image", kFirstLayerNode, content);
        if (muted)
            main->nodeLayout()[kMaskKeyNode].muted = true;
        return project;
    }
    addDefaultedNode(*main, kInstanceA, kNodeGroupNodeType, parameters,
                     {{"group", static_cast<std::int64_t>(kGroupComposition.value())}});
    auto group = makeBoundedGroup(kGroupComposition, "Mask", 5300, kGroupInputNode,
                                  kGroupOutputNode, groupParameters);
    if (!leaves) {
        key(*main, kMaskKeyNode, parameters);
        mix(group, kMaskGroupMixNode, groupParameters);
        wire(*main, edges, kSecondSolidNode, "image", kMaskKeyNode, "input");
        wire(*main, edges, kFirstSolidNode, "image", kInstanceA, "image1");
        wire(*main, edges, kSecondSolidNode, "image", kInstanceA, "image2");
        if (which != MaskCase::EnterUnconnected)
            wire(*main, edges, kMaskKeyNode, "mask", kInstanceA, "mask1");
        wire(*main, edges, kInstanceA, "image1", kFirstLayerNode, content);
        wire(group, groupEdges, kGroupInputNode, "image1", kMaskGroupMixNode, "input");
        wire(group, groupEdges, kGroupInputNode, "image2", kMaskGroupMixNode, "second");
        wire(group, groupEdges, kGroupInputNode, "mask1", kMaskGroupMixNode, "mask");
        wire(group, groupEdges, kMaskGroupMixNode, "image", kGroupOutputNode, "image1");
        if (muted)
            main->nodeLayout()[kMaskKeyNode].muted = true;
    } else {
        mix(*main, kMaskMixNode, parameters);
        key(group, kMaskGroupKeyNode, groupParameters);
        wire(*main, edges, kSecondSolidNode, "image", kInstanceA, "image1");
        wire(*main, edges, kFirstSolidNode, "image", kMaskMixNode, "input");
        wire(*main, edges, kSecondSolidNode, "image", kMaskMixNode, "second");
        wire(*main, edges, kInstanceA, "mask1", kMaskMixNode, "mask");
        wire(*main, edges, kMaskMixNode, "image", kFirstLayerNode, content);
        wire(group, groupEdges, kGroupInputNode, "image1", kMaskGroupKeyNode, "input");
        wire(group, groupEdges, kMaskGroupKeyNode, "mask", kGroupOutputNode, "mask1");
        if (muted)
            group.nodeLayout()[kMaskGroupKeyNode].muted = true;
    }
    require(project.addComposition(std::move(group)), "the group definition is added");
    return project;
}

void testNodeGroupMask(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    const auto flatUnmasked = compile(makeMaskProject(MaskCase::EnterUnconnected, true), registry);
    const auto flatMasked = compile(makeMaskProject(MaskCase::Enter, true), registry);
    const auto flatMuted = compile(makeMaskProject(MaskCase::EnterMuted, true), registry);
    if (!flatUnmasked.plan || !flatMasked.plan || !flatMuted.plan) {
        expectations.expect(false, "the flat Mask graphs compile");
        return;
    }
    const auto maskedPixels = renderReference(flatMasked.plan);
    expectations.expect(!samePixelBits(maskedPixels, renderReference(flatUnmasked.plan)) &&
                            !samePixelBits(maskedPixels, renderReference(flatMuted.plan)),
                        "the Mask changes the Image Mix: the flat graph with it differs from the "
                        "one without and from the one with the key muted");

    const struct {
        MaskCase which;
        const char* label;
    } cases[] = {{MaskCase::Enter, "a Mask entering the group at mask1"},
                 {MaskCase::Leave, "a Mask leaving the group at mask1"},
                 {MaskCase::EnterMuted, "a muted Luma Key outside feeding mask1"},
                 {MaskCase::LeaveMuted, "a muted Luma Key inside feeding the group's mask1"},
                 {MaskCase::EnterUnconnected, "an unconnected mask1"}};
    for (const auto& item : cases) {
        const MaskCase flatCase = item.which == MaskCase::Leave ? MaskCase::Enter
                                  : item.which == MaskCase::LeaveMuted ? MaskCase::EnterMuted
                                                                       : item.which;
        const auto grouped = compile(makeMaskProject(item.which, false), registry);
        const auto flat = compile(makeMaskProject(flatCase, true), registry);
        if (!grouped.plan || !flat.plan) {
            expectations.expect(false, std::string(item.label) + " compiles");
            continue;
        }
        expectations.expect(operationShape(*flat.plan) == operationShape(*grouped.plan),
                            std::string(item.label) + " compiles to the flat plan's operations");
        const auto groupedPixels = renderReference(grouped.plan);
        const auto flatPixels = renderReference(flat.plan);
        expectations.expect(samePixelBits(groupedPixels, flatPixels),
                            std::string(item.label) + " renders the flat graph's pixels exactly");
        if (item.which == MaskCase::Enter)
            captureMaskSheet(groupedPixels, flatPixels, grouped.plan->format().width(),
                             grouped.plan->format().height());
    }
}

// ---- ChannelSet ----------------------------------------------------------------------------

enum class ChannelCase { Enter, Leave, EnterUnconnected };

constexpr auto kChannelPackNode = document::NodeId::fromRaw(8601);
constexpr auto kChannelPassNode = document::NodeId::fromRaw(8602);
constexpr auto kChannelGroupPackNode = document::NodeId::fromRaw(8611);
constexpr auto kChannelGroupPassNode = document::NodeId::fromRaw(8612);

// Image to Channel Set packs solid one as a layer of channels and Channel Pass reads the layer
// back as an image. Entering: the pack is outside and the pass inside the group; leaving: the
// pack inside, the pass outside.
[[nodiscard]] document::Project makeChannelProject(const ChannelCase which, const bool flat) {
    using namespace document;
    auto project = makeProject(singleLayerOptions());
    auto* main = project.findComposition(kCompositionId);
    std::uint64_t parameters = 8700;
    std::uint64_t edges = 8800;
    std::uint64_t groupParameters = 8900;
    std::uint64_t groupEdges = 9000;
    const auto content = std::string(kLayerOutputContentInputPort);
    require(main->graph().eraseEdge(kFirstSourceEdge), "the direct source edge is removed");
    const auto pack = [](Composition& composition, const NodeId id, std::uint64_t& next) {
        addDefaultedNode(composition, id, kImageToChannelSetNodeType, next, {{"layer", std::string("pass")}});
    };
    const auto pass = [](Composition& composition, const NodeId id, std::uint64_t& next) {
        addDefaultedNode(composition, id, kChannelPassNodeType, next, {{"pass", std::string("pass")}});
    };
    if (flat) {
        pack(*main, kChannelPackNode, parameters);
        pass(*main, kChannelPassNode, parameters);
        wire(*main, edges, kFirstSolidNode, "image", kChannelPackNode, "image");
        if (which != ChannelCase::EnterUnconnected)
            wire(*main, edges, kChannelPackNode, "channels", kChannelPassNode, "channels");
        wire(*main, edges, kChannelPassNode, "image", kFirstLayerNode, content);
        return project;
    }
    addDefaultedNode(*main, kInstanceA, kNodeGroupNodeType, parameters,
                     {{"group", static_cast<std::int64_t>(kGroupComposition.value())}});
    auto group = makeBoundedGroup(kGroupComposition, "Channels", 5300, kGroupInputNode,
                                  kGroupOutputNode, groupParameters);
    if (which != ChannelCase::Leave) {
        pack(*main, kChannelPackNode, parameters);
        pass(group, kChannelGroupPassNode, groupParameters);
        wire(*main, edges, kFirstSolidNode, "image", kChannelPackNode, "image");
        if (which != ChannelCase::EnterUnconnected)
            wire(*main, edges, kChannelPackNode, "channels", kInstanceA, "channels1");
        wire(*main, edges, kInstanceA, "image1", kFirstLayerNode, content);
        wire(group, groupEdges, kGroupInputNode, "channels1", kChannelGroupPassNode, "channels");
        wire(group, groupEdges, kChannelGroupPassNode, "image", kGroupOutputNode, "image1");
    } else {
        pass(*main, kChannelPassNode, parameters);
        pack(group, kChannelGroupPackNode, groupParameters);
        wire(*main, edges, kFirstSolidNode, "image", kInstanceA, "image1");
        wire(*main, edges, kInstanceA, "channels1", kChannelPassNode, "channels");
        wire(*main, edges, kChannelPassNode, "image", kFirstLayerNode, content);
        wire(group, groupEdges, kGroupInputNode, "image1", kChannelGroupPackNode, "image");
        wire(group, groupEdges, kChannelGroupPackNode, "channels", kGroupOutputNode, "channels1");
    }
    require(project.addComposition(std::move(group)), "the group definition is added");
    return project;
}

void testNodeGroupChannelSet(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    const auto flat = compile(makeChannelProject(ChannelCase::Enter, true), registry);
    if (!flat.plan) {
        expectations.expect(false, "the flat ChannelSet graph compiles");
        return;
    }
    const auto flatPixels = renderReference(flat.plan);
    // The pass reads the packed colour back (data values unchanged): a lit frame.
    expectations.expect(std::ranges::any_of(flatPixels,
                                            [](const render::Rgba32f& pixel) {
                                                return pixel.red() > 0.0F && pixel.alpha() > 0.0F;
                                            }),
                        "a Channel Pass over a packed layer reads the plate's colour back");
    for (const auto which : {ChannelCase::Enter, ChannelCase::Leave}) {
        const char* label = which == ChannelCase::Enter ? "a ChannelSet entering the group"
                                                        : "a ChannelSet leaving the group";
        const auto grouped = compile(makeChannelProject(which, false), registry);
        expectations.expect(grouped.plan != nullptr, std::string(label) + " compiles");
        if (!grouped.plan)
            continue;
        expectations.expect(operationShape(*flat.plan) == operationShape(*grouped.plan),
                            std::string(label) + " compiles to the flat plan's operations");
        expectations.expect(samePixelBits(renderReference(grouped.plan), flatPixels),
                            std::string(label) + " renders the flat graph's pixels exactly");
    }
    // A channel set that never arrives is handled as the flat graph handles it.
    const auto flatMissing = compile(makeChannelProject(ChannelCase::EnterUnconnected, true), registry);
    const auto groupedMissing = compile(makeChannelProject(ChannelCase::EnterUnconnected, false), registry);
    expectations.expect(
        (flatMissing.plan == nullptr) == (groupedMissing.plan == nullptr) &&
            (flatMissing.plan == nullptr ||
             (operationShape(*flatMissing.plan) == operationShape(*groupedMissing.plan) &&
              samePixelBits(renderReference(flatMissing.plan), renderReference(groupedMissing.plan)))),
        "an unconnected channels1 compiles and renders as the flat graph's unconnected Pass does");
}

// ---- Wrong kinds ---------------------------------------------------------------------------

void testNodeGroupWrongKinds(Expectations& expectations) {
    using namespace document;
    std::uint64_t edges = 9800;
    const auto tries = [&](Project& project, const NodeId from, const std::string_view output,
                           const std::string_view input) {
        auto* main = project.findComposition(kCompositionId);
        return main->graph().addEdge({EdgeId::fromRaw(edges++),
                                      {from, std::string(output)},
                                      NodeInputRef{kInstanceA, std::string(input)}});
    };
    // mask1 and channels1 are free here; the first layer's plate and the key are outside the group.
    auto project = makeMaskProject(MaskCase::EnterUnconnected, false);
    std::uint64_t parameters = 9700;
    addDefaultedNode(*project.findComposition(kCompositionId), NodeId::fromRaw(9701),
                     kImageToChannelSetNodeType, parameters);
    expectations.expect(!tries(project, kSecondSolidNode, "image", "mask1"),
                        "an Image cannot connect to mask1");
    expectations.expect(!tries(project, kSecondSolidNode, "image", "channels1"),
                        "an Image cannot connect to channels1");
    expectations.expect(!tries(project, NodeId::fromRaw(9701), "channels", "mask1"),
                        "a ChannelSet cannot connect to mask1");
    expectations.expect(!tries(project, kMaskKeyNode, "mask", "channels1"),
                        "a Mask cannot connect to channels1");
    expectations.expect(project.validate().ok(),
                        "the refused links leave the project valid document truth");
    expectations.expect(tries(project, kMaskKeyNode, "mask", "mask1") && project.validate().ok(),
                        "the right kind (a Mask to mask1) is accepted, so the refusals are about kind");
    // image2 is free when the Mask leaves the group.
    auto leaving = makeMaskProject(MaskCase::Leave, false);
    std::uint64_t leavingParameters = 9750;
    addDefaultedNode(*leaving.findComposition(kCompositionId), NodeId::fromRaw(9751),
                     kImageToChannelSetNodeType, leavingParameters);
    expectations.expect(!tries(leaving, NodeId::fromRaw(9751), "channels", "image2"),
                        "a ChannelSet cannot connect to image2");
}

// ---- Nested groups -------------------------------------------------------------------------

constexpr std::uint64_t kNestedGroupBase = 100;

[[nodiscard]] document::CompositionId nestedGroupId(const std::size_t level) {
    return document::CompositionId::fromRaw(kNestedGroupBase + level);
}
[[nodiscard]] document::NodeId nestedNode(const std::size_t level, const std::uint64_t part) {
    return document::NodeId::fromRaw(20000 + level * 100 + part);
}
constexpr std::uint64_t kNestedInput = 1;
constexpr std::uint64_t kNestedOutput = 2;
constexpr std::uint64_t kNestedExposure = 3;
constexpr std::uint64_t kNestedInstance = 4;

[[nodiscard]] double nestedStops(const std::size_t level) { return 0.1 * static_cast<double>(level); }

// Group k holds Group Input -> Exposure(0.1 k) -> [instance of group k+1] -> Group Output, group
// `levels` holding no instance. The main composition's first layer reads group 1 (flat: the same
// exposures in a row). `innermostStops` overrides the innermost group's exposure.
[[nodiscard]] document::Project
makeNestedProject(const std::size_t levels, const bool flat, const bool instanceUnreached = false,
                  const std::optional<double> innermostStops = std::nullopt) {
    using namespace document;
    auto project = makeProject(singleLayerOptions());
    auto* main = project.findComposition(kCompositionId);
    std::uint64_t parameters = 6100;
    std::uint64_t edges = 6500;
    const auto content = std::string(kLayerOutputContentInputPort);
    const auto stopsOf = [&](const std::size_t level) {
        return level == levels && innermostStops ? *innermostStops : nestedStops(level);
    };
    require(main->graph().eraseEdge(kFirstSourceEdge), "the direct source edge is removed");
    if (flat) {
        NodeId previous = kFirstSolidNode;
        for (std::size_t level = 1; level <= levels; ++level) {
            const auto exposure = nestedNode(level, 60);
            addDefaultedNode(*main, exposure, "bloom.exposure", parameters,
                             {{"amount", stopsOf(level)}});
            wire(*main, edges, previous, "image", exposure, "input");
            previous = exposure;
        }
        wire(*main, edges, previous, "image", kFirstLayerNode, content);
        return project;
    }
    addDefaultedNode(*main, kInstanceA, kNodeGroupNodeType, parameters,
                     {{"group", static_cast<std::int64_t>(nestedGroupId(1).value())}});
    wire(*main, edges, kFirstSolidNode, "image", kInstanceA, "image1");
    // Unread: the instance takes the plate but nothing reads its answer; the layer keeps the plate.
    wire(*main, edges, instanceUnreached ? kFirstSolidNode : kInstanceA,
         instanceUnreached ? "image" : "image1", kFirstLayerNode, content);
    for (std::size_t level = 1; level <= levels; ++level) {
        std::uint64_t groupParameters = 30000 + level * 200;
        std::uint64_t groupEdges = 31000 + level * 200;
        auto group = makeStackedComposition(nestedGroupId(level), "Level " + std::to_string(level),
                                            nestedNode(level, 50).value());
        addDefaultedNode(group, nestedNode(level, kNestedInput), kGroupInputNodeType,
                         groupParameters);
        addDefaultedNode(group, nestedNode(level, kNestedOutput), kGroupOutputNodeType,
                         groupParameters);
        addDefaultedNode(group, nestedNode(level, kNestedExposure), "bloom.exposure",
                         groupParameters, {{"amount", stopsOf(level)}});
        wire(group, groupEdges, nestedNode(level, kNestedInput), "image1",
             nestedNode(level, kNestedExposure), "input");
        if (level < levels) {
            addDefaultedNode(group, nestedNode(level, kNestedInstance), kNodeGroupNodeType,
                             groupParameters,
                             {{"group", static_cast<std::int64_t>(nestedGroupId(level + 1).value())}});
            wire(group, groupEdges, nestedNode(level, kNestedExposure), "image",
                 nestedNode(level, kNestedInstance), "image1");
            wire(group, groupEdges, nestedNode(level, kNestedInstance), "image1",
                 nestedNode(level, kNestedOutput), "image1");
        } else {
            wire(group, groupEdges, nestedNode(level, kNestedExposure), "image",
                 nestedNode(level, kNestedOutput), "image1");
        }
        require(project.addComposition(std::move(group)), "the nested group is added");
    }
    return project;
}

// The node ids of every image effect in a plan, in order.
[[nodiscard]] std::vector<document::NodeId> effectNodes(const runtime::CompiledCompositionPlan& plan) {
    std::vector<document::NodeId> ids;
    for (const auto& operation : plan.operations())
        if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation))
            ids.push_back(effect->sourceNodeId);
    return ids;
}

void testNodeGroupNesting(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    for (const std::size_t levels : {std::size_t{2}, kMaximumNodeGroupDepth}) {
        const std::string label = std::to_string(levels) + " nested groups";
        auto project = makeNestedProject(levels, false);
        expectations.expect(project.validate().ok(), label + " are valid document truth");
        const auto grouped = compile(std::move(project), registry);
        const auto flat = compile(makeNestedProject(levels, true), registry);
        if (!grouped.plan || !flat.plan) {
            for (const auto& diagnostic : grouped.diagnostics)
                std::cerr << label << ": " << diagnostic.summary << ": " << diagnostic.detail << '\n';
            expectations.expect(false, label + " compile" + (grouped.plan ? "" : " (grouped)"));
            continue;
        }
        expectations.expect(operationShape(*grouped.plan) == operationShape(*flat.plan) &&
                                effectNodes(*grouped.plan).size() == levels,
                            label + " compile to one Exposure per level, the flat plan's shape");
        const auto groupedPixels = renderReference(grouped.plan);
        expectations.expect(samePixelBits(groupedPixels, renderReference(flat.plan)),
                            label + " render the flat graph's pixels exactly");

        // Derived ids are unique, none is an authored id, and a second compile derives the same.
        const auto ids = effectNodes(*grouped.plan);
        const std::set<document::NodeId> distinct(ids.begin(), ids.end());
        expectations.expect(distinct.size() == ids.size() &&
                                std::ranges::none_of(ids, [](const NodeId id) {
                                    return id.value() < (std::uint64_t{1} << 63U);
                                }),
                            label + ": every instance's nodes get a unique derived id");
        const auto again = compile(makeNestedProject(levels, false), registry);
        expectations.expect(again.plan && std::ranges::equal(grouped.plan->operations(),
                                                             again.plan->operations()),
                            label + ": a second compile derives the same ids and the same plan");

        // The innermost definition is edited: the outer instances all follow, as the flat edit does.
        auto edited = makeNestedProject(levels, false);
        auto* innermost = edited.findComposition(nestedGroupId(levels));
        require(innermost->parameters().setSource(
                    parameterOf(*innermost, nestedNode(levels, kNestedExposure), "amount"),
                    ConstantValueSource{1.5}),
                "the innermost exposure is edited");
        const auto editedResult = compile(std::move(edited), registry);
        const auto flatEdited = compile(makeNestedProject(levels, true, false, 1.5), registry);
        expectations.expect(editedResult.plan && flatEdited.plan &&
                                samePixelBits(renderReference(editedResult.plan),
                                              renderReference(flatEdited.plan)) &&
                                !samePixelBits(renderReference(editedResult.plan), groupedPixels),
                            label + ": editing the innermost definition updates the outermost "
                                    "instance and matches the flat edit");
    }
}

void testNodeGroupDepthRefusal(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    const auto deep = kMaximumNodeGroupDepth + 1;
    const auto refused = compile(makeNestedProject(deep, false), registry);
    const bool named = std::ranges::any_of(refused.diagnostics, [](const auto& diagnostic) {
        return diagnostic.detail.find("Node Groups are nested too deeply") != std::string::npos &&
               diagnostic.code == runtime::CompileDiagnosticCode::TopologyInvariant &&
               diagnostic.subject.nodeId.has_value();
    });
    expectations.expect(refused.plan == nullptr && named,
                        "nine nested groups are refused with the nested-too-deeply diagnostic on "
                        "the instance that goes past the depth");
    // The expansion itself reports it and does not crash: the deepest instance stays unresolved.
    const auto project = makeNestedProject(deep, false);
    const auto expansion = expandNodeGroups(project, *project.findComposition(kCompositionId));
    expectations.expect(expansion.unresolved.size() == 1 &&
                            expansion.unresolved.begin()->second ==
                                "Node Groups are nested too deeply",
                        "the expansion leaves exactly the ninth level's instance unresolved");
    // A too-deep instance nothing reads does not stop the rest of the composition compiling.
    const auto unread = compile(makeNestedProject(deep, false, true), registry);
    expectations.expect(unread.plan != nullptr,
                        "a too-deep instance that nothing reads leaves the rest compiling");
}

// ---- Node cap ------------------------------------------------------------------------------

[[nodiscard]] document::Project makeCappedProject(const std::size_t extraNodes) {
    using namespace document;
    auto project = makeProject(singleLayerOptions());
    auto* main = project.findComposition(kCompositionId);
    std::uint64_t parameters = 6100;
    std::uint64_t edges = 6500;
    require(main->graph().eraseEdge(kFirstSourceEdge), "the direct source edge is removed");
    addDefaultedNode(*main, kInstanceA, kNodeGroupNodeType, parameters,
                     {{"group", static_cast<std::int64_t>(kGroupComposition.value())}});
    wire(*main, edges, kFirstSolidNode, "image", kInstanceA, "image1");
    wire(*main, edges, kInstanceA, "image1", kFirstLayerNode,
         std::string(kLayerOutputContentInputPort));
    std::uint64_t groupParameters = 100000;
    std::uint64_t groupEdges = 7000;
    auto group = makeBoundedGroup(kGroupComposition, "Large", 5300, kGroupInputNode,
                                  kGroupOutputNode, groupParameters);
    wire(group, groupEdges, kGroupInputNode, "image1", kGroupOutputNode, "image1");
    // The expansion refuses when the composition's nodes plus the definition's pass the cap.
    const auto have = main->graph().nodes().size() + group.graph().nodes().size();
    require(have <= kMaximumExpandedNodes, "the fixture starts under the cap");
    const auto fillers = kMaximumExpandedNodes - have + extraNodes;
    for (std::size_t index = 0; index < fillers; ++index)
        addDefaultedNode(group, NodeId::fromRaw(200000 + index), kScalarValueNodeType,
                         groupParameters);
    require(project.addComposition(std::move(group)), "the large group is added");
    return project;
}

void testNodeGroupNodeCap(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    // Exactly at the cap the group expands; one node over, the instance stays and says why.
    const auto at = compile(makeCappedProject(0), registry);
    expectations.expect(at.plan != nullptr, "a group that brings the composition to exactly the "
                                            "node cap expands");
    const auto over = compile(makeCappedProject(1), registry);
    const bool named = std::ranges::any_of(over.diagnostics, [](const auto& diagnostic) {
        return diagnostic.detail.find("The expanded groups hold too many nodes") !=
                   std::string::npos &&
               diagnostic.subject.nodeId == kInstanceA;
    });
    expectations.expect(over.plan == nullptr && named,
                        "a group one node over the cap is refused with the too-many-nodes "
                        "diagnostic on the instance");
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

// ---------------------------------------------------------------------------------------------
// #595 Function Plot: the function sampled into a Path.

constexpr auto kReciprocalComposition = document::CompositionId::fromRaw(8);
constexpr auto kPlotNode = document::NodeId::fromRaw(6501);
constexpr auto kPlotShape = document::NodeId::fromRaw(6502);

// y = 1 / x, which has no answer at zero.
[[nodiscard]] document::Composition makeReciprocalGroup() {
    using namespace document;
    using core::primitives::ScalarPrimitive;
    auto group = makeStackedComposition(kReciprocalComposition, "Reciprocal", 6700);
    std::uint64_t next = 6800;
    addDefaultedNode(group, NodeId::fromRaw(6801), kGroupInputNodeType, next);
    addDefaultedNode(group, NodeId::fromRaw(6802), kGroupOutputNodeType, next);
    addDefaultedNode(group, NodeId::fromRaw(6803), kScalarMathNodeType, next,
                     {{"operation", scalarOperationStoredValue(ScalarPrimitive::Divide)}, {"a", 1.0}});
    connectDriver(group, NodeId::fromRaw(6803), "b", NodeId::fromRaw(6801), "scalar1");
    connectDriver(group, NodeId::fromRaw(6802), "scalar1", NodeId::fromRaw(6803));
    return group;
}

struct PlotSettings final {
    document::CompositionId group = kDecayComposition;
    double start = -3.0;
    double end = 3.0;
    std::int64_t samples = 65;
    document::Vec2d origin{10.0, 20.0};
    document::Vec2d scale{100.0, 50.0};
    bool smooth = true;
    // The shape layer's position, when the frame is to be looked at. A path shape is anchored at the
    // center of its anchors' bounds, so this is what makes path pixels frame pixels.
    document::Vec2d position{0.0, 0.0};
};

// The main composition with a Shape Source (kind Path) in place of the first layer's solid. Its
// path is driven by a Function Plot, or holds `constant` when one is given.
[[nodiscard]] document::Project makePlotProject(const PlotSettings& plot,
                                                const std::optional<document::PathValue>& constant,
                                                const bool axes = false,
                                                const bool stroked = false) {
    using namespace document;
    auto project = makeProject(singleLayerOptions());
    require(project.addComposition(makeDecayGroup(core::primitives::ScalarPrimitive::Multiply)) &&
                project.addComposition(makeReciprocalGroup()),
            "the plotted groups are added");
    auto* composition = project.findComposition(kCompositionId);
    std::uint64_t next = 6900;
    require(composition->graph().eraseEdge(kFirstSourceEdge), "the solid is disconnected");
    addDefaultedNode(*composition, kPlotShape, kShapeSourceNodeType, next,
                     {{"kind", static_cast<std::int64_t>(ShapeKind::Path)}});
    require(composition->graph().addEdge({EdgeId::fromRaw(7101),
                                          {kPlotShape, "image"},
                                          NodeInputRef{kFirstLayerNode,
                                                       std::string(kLayerOutputContentInputPort)}}),
            "the shape feeds the layer");
    const auto pathParameter = parameterOf(*composition, kPlotShape, "path");
    if (axes) {
        // The frame of a plot: a stroked Axes path, no fill.
        addDefaultedNode(*composition, kPlotNode, kAxesNodeType, next,
                         {{"xMin", plot.start}, {"xMax", plot.end}, {"yMin", -1.0}, {"yMax", 1.0},
                          {"xStep", 1.0}, {"yStep", 0.5}, {"origin", plot.origin},
                          {"pixelsPerUnit", plot.scale}});
        require(composition->parameters().setSource(pathParameter,
                                                    DriverBindingSource{kPlotNode, "path"}),
                "the shape reads the axes");
    } else if (constant) {
        require(composition->parameters().setSource(pathParameter, ConstantValueSource{*constant}),
                "the shape holds the sampled path");
    } else {
        addDefaultedNode(*composition, kPlotNode, kFunctionPlotNodeType, next,
                         {{"group", static_cast<std::int64_t>(plot.group.value())},
                          {"start", plot.start},
                          {"end", plot.end},
                          {"samples", plot.samples},
                          {"origin", plot.origin},
                          {"pixelsPerUnit", plot.scale},
                          {"smooth", plot.smooth}});
        require(composition->parameters().setSource(pathParameter,
                                                    DriverBindingSource{kPlotNode, "path"}),
                "the shape reads the plot");
    }
    if (axes || stroked) {
        require(composition->parameters().setSource(parameterOf(*composition, kPlotShape, "fillEnabled"),
                                                    ConstantValueSource{false}) &&
                    composition->parameters().setSource(
                        parameterOf(*composition, kPlotShape, "strokeEnabled"),
                        ConstantValueSource{true}) &&
                    composition->parameters().setSource(
                        parameterOf(*composition, kPlotShape, "strokeColor"),
                        ConstantValueSource{core::Color4d{1, 1, 1, 1}}) &&
                    composition->parameters().setSource(
                        parameterOf(*composition, kPlotShape, "strokeWidth"),
                        ConstantValueSource{3.0}),
                "the shape is stroked, not filled");
        // The layer sits at the composition's origin and is opaque, so path pixels are frame pixels.
        require(composition->parameters().setSource(kFirstPosition, ConstantValueSource{plot.position}) &&
                    composition->parameters().setSource(kFirstOpacity, ConstantValueSource{1.0}),
                "the layer is placed at the origin");
    }
    const auto validation = project.validate();
    for (const auto& issue : validation.issues())
        std::cerr << "plot fixture: " << issue.path << ": " << issue.message << '\n';
    require(validation.ok(), "the plot fixture is valid document truth");
    return project;
}

[[nodiscard]] std::shared_ptr<const document::PathValue>
evaluatePlot(const runtime::CompiledCompositionPlan& plan, std::size_t* diagnostics = nullptr) {
    const runtime::CompiledValueOperation* found = nullptr;
    for (const auto& operation : plan.valueOperations())
        if (std::holds_alternative<runtime::CompiledValueFunctionPlot>(operation.kernel))
            found = &operation;
    require(found != nullptr, "the plan holds the plot");
    const auto evaluation =
        runtime::evaluateValueGraph(plan.valueOperations(), plan.valueOutputCount(),
                                    core::RationalTime::fromInteger(0), plan.format().frameRate());
    if (diagnostics != nullptr)
        *diagnostics = evaluation.diagnostics.size();
    const auto* path = std::get_if<runtime::CompiledPathValue>(
        &evaluation.outputs.at(found->firstOutput.value()));
    return path == nullptr ? nullptr : path->value;
}

void testFunctionPlot(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    const PlotSettings settings;
    const auto driven = compile(makePlotProject(settings, std::nullopt), registry);
    expectations.expect(driven.plan != nullptr, "a Function Plot driving a shape compiles");
    if (!driven.plan)
        return;
    std::size_t diagnostics = 0;
    const auto path = evaluatePlot(*driven.plan, &diagnostics);
    expectations.expect(path != nullptr && diagnostics == 0 && path->contours.size() == 1 &&
                            path->contours[0].anchors.size() == 65 && !path->contours[0].closed,
                        "a plot of 65 samples is one open contour of 65 anchors");
    if (path == nullptr || path->contours.size() != 1)
        return;

    // Every anchor against the known answer, computed independently with the standard library.
    PathValue expected;
    PathContour contour;
    bool anchorsMatch = path->contours[0].anchors.size() == 65;
    for (std::size_t index = 0; index < 65; ++index) {
        const double x = index == 64 ? settings.end
                                     : settings.start + (settings.end - settings.start) *
                                                            static_cast<double>(index) / 64.0;
        const double y = std::sin(x) * std::exp(-x);
        const Vec2d point{settings.origin.x + x * settings.scale.x,
                          settings.origin.y - y * settings.scale.y};
        contour.anchors.push_back({point, std::nullopt, std::nullopt, PathAnchorMode::Auto});
        if (index < path->contours[0].anchors.size()) {
            const auto& anchor = path->contours[0].anchors[index];
            anchorsMatch = anchorsMatch && std::abs(anchor.point.x - point.x) < 1e-9 &&
                           std::abs(anchor.point.y - point.y) < 1e-9 &&
                           anchor.mode == PathAnchorMode::Auto && !anchor.inHandle &&
                           !anchor.outHandle;
        }
    }
    expected.contours.push_back(std::move(contour));
    expectations.expect(anchorsMatch,
                        "every anchor is origin + (x * scale.x, -y * scale.y) for y = sin(x) * exp(-x)");

    // The shape the plot drives draws the same pixels as a shape that holds those anchors.
    const auto constant = compile(makePlotProject(settings, expected), registry);
    expectations.expect(constant.plan != nullptr, "the shape holding the sampled path compiles");
    if (constant.plan) {
        const auto drivenPixels = renderReference(driven.plan);
        expectations.expect(!drivenPixels.empty() && drivenPixels == renderReference(constant.plan),
                            "a plot-driven shape renders what a shape holding its anchors renders");
    }

    // Corner anchors when smoothing is off.
    PlotSettings polyline = settings;
    polyline.smooth = false;
    const auto corner = compile(makePlotProject(polyline, std::nullopt), registry);
    const auto cornerPath = corner.plan ? evaluatePlot(*corner.plan) : nullptr;
    expectations.expect(cornerPath && cornerPath->contours.size() == 1 &&
                            std::ranges::all_of(cornerPath->contours[0].anchors,
                                                [](const PathAnchor& anchor) {
                                                    return anchor.mode == PathAnchorMode::Corner;
                                                }),
                        "a plot with smoothing off holds Corner anchors");

    // The sample count is held to what one contour holds: at least two, at most 4096.
    for (const auto& [asked, held] : {std::pair{std::int64_t{1}, std::size_t{2}},
                                      std::pair{std::int64_t{100000}, std::size_t{4096}}}) {
        PlotSettings clamped = settings;
        clamped.samples = asked;
        const auto result = compile(makePlotProject(clamped, std::nullopt), registry);
        const auto clampedPath = result.plan ? evaluatePlot(*result.plan) : nullptr;
        expectations.expect(clampedPath && clampedPath->contours.size() == 1 &&
                                clampedPath->contours[0].anchors.size() == held,
                            "a sample count of " + std::to_string(asked) + " is held to " +
                                std::to_string(held));
    }

    // A sample with no answer ends the contour: 1 / x over [-1, 1] in nine samples has no value at
    // zero, so two contours of four anchors, and no line drawn across the pole.
    PlotSettings pole = settings;
    pole.group = kReciprocalComposition;
    pole.start = -1.0;
    pole.end = 1.0;
    pole.samples = 9;
    pole.smooth = false;
    const auto gap = compile(makePlotProject(pole, std::nullopt), registry);
    const auto gapPath = gap.plan ? evaluatePlot(*gap.plan) : nullptr;
    bool gapKnown = gapPath && gapPath->contours.size() == 2 &&
                    gapPath->contours[0].anchors.size() == 4 && gapPath->contours[1].anchors.size() == 4;
    if (gapKnown)
        for (std::size_t half = 0; half < 2; ++half)
            for (std::size_t index = 0; index < 4; ++index) {
                const double x = -1.0 + 0.25 * static_cast<double>(half * 5 + index);
                const auto& anchor = gapPath->contours[half].anchors[index];
                gapKnown = gapKnown && std::abs(anchor.point.x - (pole.origin.x + x * pole.scale.x)) < 1e-9 &&
                           std::abs(anchor.point.y - (pole.origin.y - (1.0 / x) * pole.scale.y)) < 1e-9;
            }
    expectations.expect(gapKnown, "1 / x leaves a gap at zero: two contours of four known anchors");
}

// With BLOOM_FRAME_CAPTURE_DIR set, the plot and its axes are rendered on the CPU reference and written
// as binary PPM frames (a mid-gray backdrop under the straight-alpha picture, display gamma 2.2) so a
// person can look at them.
void writeFrame(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
                const std::string& name) {
    const char* directory = std::getenv("BLOOM_FRAME_CAPTURE_DIR");
    if (directory == nullptr)
        return;
    const auto pixels = renderReference(plan);
    const auto width = static_cast<std::size_t>(plan->format().width());
    const auto height = static_cast<std::size_t>(plan->format().height());
    require(pixels.size() == width * height, "the frame has the composition's size");
    std::ofstream file(std::string(directory) + "/" + name + ".ppm", std::ios::binary);
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (const auto& pixel : pixels) {
        const float alpha = std::clamp(pixel.alpha(), 0.0F, 1.0F);
        for (const float channel : {pixel.red(), pixel.green(), pixel.blue()}) {
            const float straight = alpha > 0.0F ? channel / alpha : 0.0F;
            const float shown = straight * alpha + 0.2F * (1.0F - alpha);
            file.put(static_cast<char>(
                std::lround(255.0F * std::pow(std::clamp(shown, 0.0F, 1.0F), 1.0F / 2.2F))));
        }
    }
}

void testPlotFrames(Expectations& expectations) {
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    PlotSettings settings;
    settings.start = -1.0;
    settings.end = 9.0;
    settings.samples = 257;
    settings.origin = {200.0, 540.0};
    settings.scale = {120.0, 150.0};
    // The center of the anchors' bounds, from a first compile, is where the layer must sit.
    const auto centerOf = [&](const bool axes) {
        const auto probe = compile(makePlotProject(settings, std::nullopt, axes, true), registry);
        require(probe.plan != nullptr, "the probe compiles");
        const auto evaluation = runtime::evaluateValueGraph(
            probe.plan->valueOperations(), probe.plan->valueOutputCount(),
            core::RationalTime::fromInteger(0), probe.plan->format().frameRate());
        double left = 1e300, top = 1e300, right = -1e300, bottom = -1e300;
        for (const auto& output : evaluation.outputs)
            if (const auto* path = std::get_if<runtime::CompiledPathValue>(&output))
                for (const auto& contour : path->value->contours)
                    for (const auto& anchor : contour.anchors) {
                        left = std::min(left, anchor.point.x);
                        right = std::max(right, anchor.point.x);
                        top = std::min(top, anchor.point.y);
                        bottom = std::max(bottom, anchor.point.y);
                    }
        return document::Vec2d{(left + right) / 2.0, (top + bottom) / 2.0};
    };
    PlotSettings plotAt = settings;
    plotAt.position = centerOf(false);
    PlotSettings framesAt = settings;
    framesAt.position = centerOf(true);
    const auto plot = compile(makePlotProject(plotAt, std::nullopt, false, true), registry);
    const auto frame = compile(makePlotProject(framesAt, std::nullopt, true), registry);
    expectations.expect(plot.plan != nullptr && frame.plan != nullptr,
                        "the stroked plot and the stroked axes compile");
    if (!plot.plan || !frame.plan)
        return;
    const auto count = [](const std::vector<render::Rgba32f>& pixels) {
        return std::ranges::count_if(pixels, [](const auto& pixel) { return pixel.alpha() > 0.5F; });
    };
    expectations.expect(count(renderReference(plot.plan)) > 100 && count(renderReference(frame.plan)) > 100,
                        "the stroked plot and axes put pixels on the frame");
    writeFrame(plot.plan, "function-plot");
    writeFrame(frame.plan, "axes");
}

// #344, #530: a chain of path nodes drives a shape, and the shape renders what a shape holding the
// chain's answer renders (the production fill is the same on every route; the chain only supplies
// the path).
[[nodiscard]] document::PathValue polygonPathValue(const std::vector<document::Vec2d>& points) {
    document::PathContour contour;
    contour.closed = true;
    for (const auto point : points)
        contour.anchors.push_back({point, std::nullopt, std::nullopt, document::PathAnchorMode::Corner});
    document::PathValue value;
    value.contours.push_back(std::move(contour));
    return value;
}

[[nodiscard]] document::PathValue rectanglePathValue(const double left, const double top,
                                                      const double right, const double bottom) {
    return polygonPathValue({{left, top}, {right, top}, {right, bottom}, {left, bottom}});
}

struct PathStep final {
    std::string_view type;
    std::map<std::string, document::ParameterValue> values;
};

// The stroked shape project with a chain of path nodes between `source` and the shape's path.
[[nodiscard]] document::Project makeChainProject(const PlotSettings& settings,
                                                 const document::PathValue& source,
                                                 const std::vector<PathStep>& steps) {
    using namespace document;
    auto project = makePlotProject(settings, source, false, true);
    auto* composition = project.findComposition(kCompositionId);
    require(composition != nullptr, "the composition is there");
    std::uint64_t next = 8300;
    std::uint64_t nodeId = 8200;
    std::optional<NodeId> previous;
    for (const auto& step : steps) {
        const auto id = NodeId::fromRaw(++nodeId);
        auto values = step.values;
        const std::string inputRole = step.type == kBooleanPathsNodeType ? "first" : "path";
        if (!previous)
            values[inputRole] = source;
        addDefaultedNode(*composition, id, step.type, next, values);
        if (previous)
            require(composition->parameters().setSource(parameterOf(*composition, id, inputRole),
                                                        DriverBindingSource{*previous, "path"}),
                    "a step reads the one before it");
        previous = id;
    }
    require(previous.has_value() &&
                composition->parameters().setSource(parameterOf(*composition, kPlotShape, "path"),
                                                    DriverBindingSource{*previous, "path"}),
            "the chain drives the shape");
    const auto validation = project.validate();
    for (const auto& issue : validation.issues())
        std::cerr << "path chain fixture: " << issue.path << ": " << issue.message << '\n';
    require(validation.ok(), "the path chain fixture is valid document truth");
    return project;
}

// The last non-empty path any value operation answers at the first frame: the chain's result (a
// stroke's empty core is not it).
[[nodiscard]] std::shared_ptr<const document::PathValue>
chainResult(const runtime::CompiledCompositionPlan& plan) {
    const auto evaluation =
        runtime::evaluateValueGraph(plan.valueOperations(), plan.valueOutputCount(),
                                    core::RationalTime::fromInteger(0), plan.format().frameRate());
    std::shared_ptr<const document::PathValue> last;
    for (const auto& output : evaluation.outputs)
        if (const auto* path = std::get_if<runtime::CompiledPathValue>(&output);
            path != nullptr && !path->value->contours.empty())
            last = path->value;
    return last;
}

[[nodiscard]] document::Vec2d centerOfBounds(const document::PathValue& path) {
    double left = 1e300, top = 1e300, right = -1e300, bottom = -1e300;
    for (const auto& contour : path.contours)
        for (const auto& anchor : contour.anchors) {
            left = std::min(left, anchor.point.x);
            right = std::max(right, anchor.point.x);
            top = std::min(top, anchor.point.y);
            bottom = std::max(bottom, anchor.point.y);
        }
    return {(left + right) / 2.0, (top + bottom) / 2.0};
}

void testPathProcessingDrivesShape(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    PlotSettings settings;
    settings.position = {0.0, 0.0};
    const auto source = rectanglePathValue(0, 0, 200, 150);

    // Transform (move to 30, 20; half size) then Offset (grow 5): 30..130 by 20..95, grown to
    // 25..135 by 15..100.
    const auto drivenCompile = compile(
        makeChainProject(settings, source,
                         {{kTransformPathNodeType,
                           {{"position", Vec2d{30.0, 20.0}}, {"scale", Vec2d{0.5, 0.5}}}},
                          {kOffsetPathNodeType, {{"amount", 5.0}}}}),
        registry);
    const auto heldCompile =
        compile(makePlotProject(settings, rectanglePathValue(25, 15, 135, 100), false, true), registry);
    const auto unmovedCompile = compile(makePlotProject(settings, source, false, true), registry);
    expectations.expect(drivenCompile.plan && heldCompile.plan && unmovedCompile.plan,
                        "a shape driven by Transform Path and Offset Path compiles");
    if (!drivenCompile.plan || !heldCompile.plan || !unmovedCompile.plan)
        return;
    const auto pixels = renderReference(drivenCompile.plan);
    expectations.expect(!pixels.empty() && pixels == renderReference(heldCompile.plan),
                        "the chain renders what a shape holding its answer renders");
    expectations.expect(pixels != renderReference(unmovedCompile.plan),
                        "the chain moves the shape");
}

// Each path node on a shape with a notch, as frames: the nodes working, drawn by the production fill.
void testPathProcessingFrames(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();
    const auto notched = polygonPathValue(
        {{300, 200}, {900, 200}, {900, 420}, {700, 420}, {700, 330}, {500, 330}, {500, 420}, {300, 420}});
    const auto bar = rectanglePathValue(560, 150, 640, 500);
    struct Frame final {
        std::string name;
        std::vector<PathStep> steps;
    };
    const std::vector<Frame> frames{
        {"path-source", {{kTransformPathNodeType, {}}}},
        {"transform-path",
         {{kTransformPathNodeType,
           {{"position", Vec2d{700.0, 520.0}}, {"anchor", Vec2d{600.0, 310.0}},
            {"scale", Vec2d{0.7, 0.7}}, {"rotation", 20.0}}}}},
        {"trim-path", {{kTrimPathNodeType, {{"start", 10.0}, {"end", 60.0}}}}},
        {"offset-path", {{kOffsetPathNodeType, {{"amount", 24.0}}}}},
        {"offset-path-round", {{kOffsetPathNodeType, {{"amount", 24.0}, {"join", std::int64_t{1}}}}}},
        {"round-corners", {{kRoundCornersNodeType, {{"radius", 40.0}}}}},
        {"path-stroke-dashed",
         {{kPathStrokeNodeType, {{"width", 24.0}, {"dashLength", 50.0}, {"dashGap", 30.0}}}}},
        {"path-stroke-write-on",
         {{kPathStrokeNodeType,
           {{"width", 30.0}, {"end", 70.0}, {"taperStart", 30.0}, {"taperEnd", 40.0},
            {"wiggle", 6.0}, {"coreWidth", 40.0}}}}},
        {"merge-paths-subtract",
         {{kMergePathsNodeType, {{"operation", std::int64_t{1}}}}}},
    };
    PathValue pair = notched;
    pair.contours.push_back(bar.contours.front());
    for (const auto& frame : frames) {
        const auto& input = frame.name == "merge-paths-subtract" ? pair : notched;
        PlotSettings settings;
        const auto probe = compile(makeChainProject(settings, input, frame.steps), registry);
        require(probe.plan != nullptr, "the frame's chain compiles");
        const auto result = chainResult(*probe.plan);
        require(result != nullptr && !result->contours.empty(), "the frame's chain answers a path");
        settings.position = centerOfBounds(*result);
        const auto drawn = compile(makeChainProject(settings, input, frame.steps), registry);
        require(drawn.plan != nullptr, "the placed chain compiles");
        writeFrame(drawn.plan, frame.name);
    }
    // Boolean Paths takes two paths: the notched shape less the bar.
    {
        PlotSettings settings;
        auto project = makeChainProject(settings, notched, {{kBooleanPathsNodeType, {{"operation", std::int64_t{1}}}}});
        auto* composition = project.findComposition(kCompositionId);
        require(composition->parameters().setSource(
                    parameterOf(*composition, NodeId::fromRaw(8201), "second"), ConstantValueSource{bar}),
                "the second path is set");
        const auto probe = compile(project, registry);
        require(probe.plan != nullptr, "the boolean frame compiles");
        const auto result = chainResult(*probe.plan);
        require(result != nullptr && result->contours.size() == 2, "the bar cuts the shape in two");
        settings.position = centerOfBounds(*result);
        auto placed = makeChainProject(settings, notched, {{kBooleanPathsNodeType, {{"operation", std::int64_t{1}}}}});
        auto* placedComposition = placed.findComposition(kCompositionId);
        require(placedComposition->parameters().setSource(
                    parameterOf(*placedComposition, NodeId::fromRaw(8201), "second"),
                    ConstantValueSource{bar}),
                "the second path is set");
        const auto drawn = compile(placed, registry);
        require(drawn.plan != nullptr, "the placed boolean compiles");
        writeFrame(drawn.plan, "boolean-paths-subtract");
    }
    expectations.expect(true, "the path processing frames are written when asked for");
}
