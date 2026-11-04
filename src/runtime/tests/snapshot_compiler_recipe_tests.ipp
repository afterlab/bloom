// #339 Drop Shadow, Glow and Light Wrap lower to their recipe chains of existing primitives.

struct RecipeFixture final {
    document::Project project;
    document::NodeId node;
    std::unordered_map<std::string, document::ParameterId> parameters;
};

// Solid 1 -> recipe `input` -> Layer 1, with each parameter at its default unless `values` names
// it. `extra` wires further inputs (Solid 2's image into the named port).
[[nodiscard]] RecipeFixture
recipeFixture(const std::string_view type,
              const std::unordered_map<std::string, document::ParameterValue>& values = {},
              const std::vector<std::string>& secondSolidPorts = {}) {
    using namespace document;
    RecipeFixture fixture{makeProject(), NodeId::fromRaw(1300), {}};
    auto* composition = fixture.project.findComposition(kCompositionId);
    auto& graph = composition->graph();
    const auto* definition = builtInNodeDefinitions().find(type, 1);
    require(definition != nullptr, "recipe definition exists");
    std::vector<ParameterBinding> bindings;
    std::uint64_t next = 1300;
    for (const auto& parameter : definition->parameters) {
        const auto id = ParameterId::fromRaw(next++);
        const auto value = values.find(parameter.role);
        require(composition->parameters().insert(
                    {id, parameter.schemaKey,
                     ConstantValueSource{value == values.end() ? parameter.defaultValue
                                                               : value->second}}),
                "recipe parameter is valid");
        bindings.push_back({parameter.role, id});
        fixture.parameters.emplace(parameter.role, id);
    }
    require(graph.addNode({fixture.node, std::string(type), std::move(bindings), 1}),
            "recipe node is accepted");
    require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
    require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                           NodeInputRef{fixture.node, "input"}}),
            "source feeds the recipe");
    require(graph.addEdge({EdgeId::fromRaw(1300), {fixture.node, "image"},
                           NodeInputRef{kFirstLayerNode, std::string(kLayerOutputContentInputPort)}}),
            "recipe feeds Layer");
    std::uint64_t edge = 1301;
    for (const auto& port : secondSolidPorts) {
        // A Mask input reads the Mask node's coverage; every other port reads Solid 2.
        if (port == "mask") {
            const auto maskNode = NodeId::fromRaw(1390);
            const auto* maskDefinition = builtInNodeDefinitions().find(kMaskNodeType, 1);
            std::vector<ParameterBinding> maskBindings;
            for (const auto& parameter : maskDefinition->parameters) {
                const auto id = ParameterId::fromRaw(next++);
                require(composition->parameters().insert(
                            {id, parameter.schemaKey, ConstantValueSource{parameter.defaultValue}}),
                        "Mask parameter is valid");
                maskBindings.push_back({parameter.role, id});
            }
            require(graph.addNode({maskNode, std::string(kMaskNodeType), std::move(maskBindings), 1}),
                    "Mask node is accepted");
            require(graph.addEdge({EdgeId::fromRaw(edge++),
                                   {maskNode, std::string(kMaskNodeOutputPort)},
                                   NodeInputRef{fixture.node, port}}),
                    "Mask feeds the recipe");
            continue;
        }
        require(graph.addEdge({EdgeId::fromRaw(edge++), {kSecondSolidNode, "image"},
                               NodeInputRef{fixture.node, port}}),
                "second solid feeds the recipe");
    }
    return fixture;
}

[[nodiscard]] runtime::SnapshotCompileResult compileRecipe(document::Project project) {
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "built-ins register");
    registry.freeze();
    return compile(std::move(project), registry);
}

// The node's own operations, in plan order.
[[nodiscard]] std::vector<std::size_t> recipeOperations(const runtime::CompiledCompositionPlan& plan,
                                                        const document::NodeId node) {
    std::vector<std::size_t> found;
    for (std::size_t index = 0; index < plan.operations().size(); ++index) {
        const auto& operation = plan.operations()[index];
        if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
            effect && effect->sourceNodeId == node)
            found.push_back(index);
        if (const auto* merge = std::get_if<runtime::CompiledMerge>(&operation);
            merge && merge->sourceNodeId == node)
            found.push_back(index);
    }
    return found;
}

template <typename Kernel>
[[nodiscard]] const Kernel* recipeKernel(const runtime::CompiledCompositionPlan& plan,
                                         const std::size_t index) {
    const auto* effect = std::get_if<runtime::CompiledImageEffect>(&plan.operations()[index]);
    return effect ? std::get_if<Kernel>(&effect->kernel) : nullptr;
}

[[nodiscard]] const runtime::CompiledMerge* recipeMerge(const runtime::CompiledCompositionPlan& plan,
                                                        const std::size_t index) {
    return std::get_if<runtime::CompiledMerge>(&plan.operations()[index]);
}

// Drop Shadow: Coverage blur -> Transform 2D -> Levels -> Normal Merge with the input on top. The
// offset folds direction and distance to a constant, the colour alpha scales the opacity, and an
// animated distance becomes a derived value-graph operand that makes the chain time dependent.
void testDropShadowLowering(Expectations& expectations) {
    using namespace document;
    auto fixture = recipeFixture(kDropShadowNodeType,
                                 {{"color", core::Color4d{0.25, 0.5, 1.5, 0.5}},
                                  {"opacity", 0.8},
                                  {"direction", 90.0},
                                  {"distance", 12.0},
                                  {"softness", 3.5}});
    const auto node = fixture.node;
    const auto params = fixture.parameters;
    auto result = compileRecipe(std::move(fixture.project));
    expectations.expect(result.plan != nullptr, "Drop Shadow compiles");
    if (!result.plan)
        return;
    const auto& plan = *result.plan;
    const auto ops = recipeOperations(plan, node);
    require(ops.size() == 4, "Drop Shadow lowers to four primitives");
    const auto* blur = recipeKernel<runtime::SeparableBlurKernel>(plan, ops[0]);
    const auto* move = recipeKernel<runtime::Transform2DKernel>(plan, ops[1]);
    const auto* levels = recipeKernel<runtime::LevelsKernel>(plan, ops[2]);
    const auto* merge = recipeMerge(plan, ops[3]);
    const auto* radius = blur ? std::get_if<double>(&blur->radius.source) : nullptr;
    expectations.expect(blur && blur->channels == runtime::BlurChannels::Coverage &&
                            blur->kind == runtime::SeparableBlurKind::Gaussian &&
                            blur->edge == runtime::BlurEdgeMode::Transparent && radius &&
                            *radius == 3.5 && blur->radius.id == params.at("softness"),
                        "Drop Shadow softens the input's coverage with the Matte Blur filter");
    const auto* offset = move ? std::get_if<Vec2d>(&move->translate.source) : nullptr;
    expectations.expect(move && offset && std::abs(offset->x - 12.0) < 1e-12 &&
                            std::abs(offset->y) < 1e-12 &&
                            move->extent == runtime::Transform2DExtent::Transformed,
                        "direction 90 and distance 12 fold to a constant offset of (12, 0)");
    const auto* red = levels ? std::get_if<double>(&levels->channels[1].outputWhite.source) : nullptr;
    const auto* blue = levels ? std::get_if<double>(&levels->channels[3].outputWhite.source) : nullptr;
    const auto* alpha =
        levels ? std::get_if<double>(&levels->channels[4].outputWhite.source) : nullptr;
    expectations.expect(red && *red == 0.25 && blue && *blue == 1.5 && alpha && *alpha == 0.4 &&
                            !levels->clamp,
                        "Levels colours the coverage and fades it by opacity * colour alpha");
    const auto* input = std::get_if<runtime::CompiledImageEffect>(&plan.operations()[ops[0]]);
    expectations.expect(merge && merge->entries.size() == 2 && input &&
                            merge->entries[0].input == input->input &&
                            merge->entries[0].blendMode == core::BlendMode::Normal &&
                            merge->entries[1].input.value() == ops[2] &&
                            merge->entries[1].blendMode == core::BlendMode::Normal &&
                            !merge->entries[0].layerId.isValid(),
                        "the input is composited over its shadow by a Normal Merge");
    expectations.expect(plan.valueOperations().empty() &&
                            !plan.operationTimeDependent(runtime::OperationIndex::fromRaw(ops[3])),
                        "constant Drop Shadow values fold and stay time invariant");

    // Shadow only publishes the coloured shadow itself.
    auto only = recipeFixture(kDropShadowNodeType, {{"shadowOnly", true}});
    const auto onlyNode = only.node;
    auto onlyResult = compileRecipe(std::move(only.project));
    expectations.expect(onlyResult.plan &&
                            recipeOperations(*onlyResult.plan, onlyNode).size() == 3,
                        "Shadow only drops the composite");

    // An animated distance keeps animating through the derived offset.
    auto animated = recipeFixture(kDropShadowNodeType);
    auto* composition = animated.project.findComposition(kCompositionId);
    constexpr auto curveId = AnimationCurveId::fromRaw(1350);
    require(composition->animationCurves().insert(ScalarAnimationCurve{
                curveId,
                {{KeyframeId::fromRaw(1351), core::RationalTime::fromInteger(0), 0.0,
                  KeyframeInterpolation::Linear},
                 {KeyframeId::fromRaw(1352), core::RationalTime::fromInteger(1), 20.0,
                  KeyframeInterpolation::Linear}}}),
            "distance curve is publishable");
    require(composition->parameters().setSource(animated.parameters.at("distance"),
                                                AnimationCurveSource{curveId}),
            "distance animates");
    const auto animatedNode = animated.node;
    auto animatedResult = compileRecipe(std::move(animated.project));
    expectations.expect(animatedResult.plan != nullptr, "an animated Drop Shadow compiles");
    if (animatedResult.plan) {
        const auto animatedOps = recipeOperations(*animatedResult.plan, animatedNode);
        const auto* animatedMove =
            animatedOps.size() == 4
                ? recipeKernel<runtime::Transform2DKernel>(*animatedResult.plan, animatedOps[1])
                : nullptr;
        expectations.expect(
            animatedMove &&
                std::holds_alternative<runtime::ValueOutputIndex>(animatedMove->translate.source) &&
                !animatedResult.plan->valueOperations().empty() &&
                animatedResult.plan->operationTimeDependent(
                    runtime::OperationIndex::fromRaw(animatedOps[1])) &&
                !animatedResult.plan->operationTimeDependent(
                    runtime::OperationIndex::fromRaw(animatedOps[0])) &&
                animatedResult.plan->operationTimeDependent(animatedResult.plan->output()),
            "an animated distance derives the offset per frame and invalidates only downstream");
    }
}

// Glow: Luma Key and Set Alpha keep the bright pixels, the octaves blur from the widest, Image Mix
// folds them with normalized weights, Levels tints and scales, and an Add Merge adds the input. A
// key pass replaces the keyed source; a Mask multiplies it.
void testGlowLowering(Expectations& expectations) {
    using namespace document;
    auto fixture = recipeFixture(kGlowNodeType,
                                 {{"threshold", 2.0},
                                  {"knee", 1.0},
                                  {"intensity", 3.0},
                                  {"size", 32.0},
                                  {"falloff", 0.5},
                                  {"tint", core::Color4d{1.0, 0.5, 0.25, 1.0}},
                                  {"octaves", std::int64_t{3}}});
    const auto node = fixture.node;
    auto result = compileRecipe(std::move(fixture.project));
    expectations.expect(result.plan != nullptr, "Glow compiles");
    if (!result.plan)
        return;
    const auto& plan = *result.plan;
    const auto ops = recipeOperations(plan, node);
    // key, set alpha, 3 blurs, 2 mixes, levels, merge
    require(ops.size() == 9, "Glow with three octaves lowers to nine primitives");
    const auto* key = recipeKernel<runtime::LumaKeyKernel>(plan, ops[0]);
    const auto* low = key ? std::get_if<double>(&key->low.source) : nullptr;
    const auto* high = key ? std::get_if<double>(&key->high.source) : nullptr;
    expectations.expect(key && low && *low == 1.5 && high && *high == 2.5 && !key->invert,
                        "the key ramps from threshold - knee/2 to threshold + knee/2");
    const auto* bright = recipeKernel<runtime::ImageCombineKernel>(plan, ops[1]);
    const auto* brightEffect = std::get_if<runtime::CompiledImageEffect>(&plan.operations()[ops[1]]);
    expectations.expect(bright && bright->mode == runtime::ImageCombineMode::SetAlpha &&
                            brightEffect->secondaryInput &&
                            brightEffect->secondaryInput->value() == ops[0],
                        "Set Alpha keeps the straight colour at the keyed coverage");
    std::vector<double> radii;
    std::vector<double> factors;
    for (const auto index : ops) {
        if (const auto* blur = recipeKernel<runtime::SeparableBlurKernel>(plan, index))
            if (const auto* value = std::get_if<double>(&blur->radius.source);
                value && blur->channels == runtime::BlurChannels::Rgba)
                radii.push_back(*value);
        if (const auto* mix = recipeKernel<runtime::ImageCombineKernel>(plan, index);
            mix && mix->mode == runtime::ImageCombineMode::Mix && mix->factor)
            if (const auto* value = std::get_if<double>(&mix->factor->source))
                factors.push_back(*value);
    }
    expectations.expect(radii == std::vector<double>{32.0, 16.0, 8.0},
                        "the octaves double up to the size, widest first");
    expectations.expect(factors.size() == 2 && std::abs(factors[0] - 1.0 / 1.5) < 1e-15 &&
                            std::abs(factors[1] - 1.0 / 1.75) < 1e-15,
                        "Image Mix folds octave j with weight 1 / (1 + f + ... + f^(n-1-j))");
    const auto* levels = recipeKernel<runtime::LevelsKernel>(plan, ops[7]);
    const auto* green =
        levels ? std::get_if<double>(&levels->channels[2].outputWhite.source) : nullptr;
    const auto* alpha =
        levels ? std::get_if<double>(&levels->channels[4].outputWhite.source) : nullptr;
    expectations.expect(green && *green == 1.5 && alpha && *alpha == 1.0,
                        "Levels scales the tint by the intensity and keeps alpha");
    const auto* merge = recipeMerge(plan, ops[8]);
    expectations.expect(merge && merge->entries.size() == 2 &&
                            merge->entries[0].input.value() == ops[7] &&
                            merge->entries[0].blendMode == core::BlendMode::Add &&
                            merge->entries[1].blendMode == core::BlendMode::Normal,
                        "an Add Merge adds the glow over the input");

    // A key pass and a Mask: the key keys the pass, and the Mask multiplies it.
    auto keyed = recipeFixture(kGlowNodeType, {{"octaves", std::int64_t{1}}, {"glowOnly", true}},
                               {"key", "mask"});
    const auto keyedNode = keyed.node;
    auto keyedResult = compileRecipe(std::move(keyed.project));
    expectations.expect(keyedResult.plan != nullptr, "Glow with a key and a Mask compiles");
    if (keyedResult.plan) {
        const auto& keyedPlan = *keyedResult.plan;
        const auto keyedOps = recipeOperations(keyedPlan, keyedNode);
        require(keyedOps.size() == 5, "a keyed, masked single-octave glow only lowers five");
        const auto* keyEffect =
            std::get_if<runtime::CompiledImageEffect>(&keyedPlan.operations()[keyedOps[0]]);
        const auto* masked = recipeKernel<runtime::ImageCombineKernel>(keyedPlan, keyedOps[2]);
        const auto* keySource =
            keyEffect ? std::get_if<runtime::CompiledSolid>(
                            &keyedPlan.operations()[keyEffect->input.value()])
                      : nullptr;
        expectations.expect(keySource && keySource->sourceNodeId == kSecondSolidNode &&
                                masked &&
                                masked->mode == runtime::ImageCombineMode::MultiplyAlpha &&
                                recipeKernel<runtime::LevelsKernel>(keyedPlan, keyedOps[4]),
                            "the key pass is keyed, the Mask multiplies it and Glow only stops "
                            "before the Merge");
    }
}

// Light Wrap: Rgba Clamp blur of the background, Coverage blur of the foreground, Multiply Alpha
// by the inverted coverage, Levels scaling the alpha by the amount, then an Atop (wrap only: In)
// Merge over the foreground.
// With no background the foreground passes through.
void testLightWrapLowering(Expectations& expectations) {
    using namespace document;
    auto fixture =
        recipeFixture(kLightWrapNodeType, {{"radius", 6.0}, {"amount", 0.5}}, {"background"});
    const auto node = fixture.node;
    const auto amountId = fixture.parameters.at("amount");
    auto result = compileRecipe(std::move(fixture.project));
    expectations.expect(result.plan != nullptr, "Light Wrap compiles");
    if (!result.plan)
        return;
    const auto& plan = *result.plan;
    const auto ops = recipeOperations(plan, node);
    require(ops.size() == 5, "Light Wrap lowers to five primitives");
    const auto* background = recipeKernel<runtime::SeparableBlurKernel>(plan, ops[0]);
    const auto* spread = recipeKernel<runtime::SeparableBlurKernel>(plan, ops[1]);
    const auto* wrap = recipeKernel<runtime::ImageCombineKernel>(plan, ops[2]);
    const auto* levels = recipeKernel<runtime::LevelsKernel>(plan, ops[3]);
    const auto* merge = recipeMerge(plan, ops[4]);
    expectations.expect(background && background->channels == runtime::BlurChannels::Rgba &&
                            background->edge == runtime::BlurEdgeMode::Clamp && spread &&
                            spread->channels == runtime::BlurChannels::Coverage &&
                            spread->edge == runtime::BlurEdgeMode::Transparent,
                        "the background blurs with Clamp and the foreground coverage spreads");
    const auto* unit = wrap && wrap->factor ? std::get_if<double>(&wrap->factor->source) : nullptr;
    const auto* amount = levels ? std::get_if<double>(&levels->channels[4].outputWhite.source)
                                : nullptr;
    expectations.expect(wrap && wrap->mode == runtime::ImageCombineMode::MultiplyAlpha &&
                            wrap->invert && unit && *unit == 1.0 && amount && *amount == 0.5 &&
                            levels->channels[4].outputWhite.id == amountId,
                        "the wrap is the background held out by the inverted spread, faded by "
                        "the amount");
    expectations.expect(merge && merge->entries.size() == 2 &&
                            merge->entries[0].blendMode == core::BlendMode::Atop &&
                            merge->entries[1].blendMode == core::BlendMode::Normal,
                        "the wrap is composited Atop the foreground");

    auto only = recipeFixture(kLightWrapNodeType, {{"wrapOnly", true}}, {"background"});
    const auto onlyNode = only.node;
    auto onlyResult = compileRecipe(std::move(only.project));
    const auto onlyOps =
        onlyResult.plan ? recipeOperations(*onlyResult.plan, onlyNode) : std::vector<std::size_t>{};
    const auto* onlyMerge =
        onlyOps.size() == 5 ? recipeMerge(*onlyResult.plan, onlyOps[4]) : nullptr;
    expectations.expect(onlyMerge && onlyMerge->entries[0].blendMode == core::BlendMode::In,
                        "Wrap only composites the wrap In the foreground");

    auto bare = recipeFixture(kLightWrapNodeType);
    const auto bareNode = bare.node;
    auto bareResult = compileRecipe(std::move(bare.project));
    expectations.expect(bareResult.plan && recipeOperations(*bareResult.plan, bareNode).empty(),
                        "with no background the foreground passes through");
}

// A muted recipe passes its input through, like every image effect.
void testRecipeMute(Expectations& expectations) {
    using namespace document;
    for (const auto type : {kDropShadowNodeType, kGlowNodeType, kLightWrapNodeType}) {
        auto fixture = recipeFixture(type, {}, type == kLightWrapNodeType
                                                   ? std::vector<std::string>{"background"}
                                                   : std::vector<std::string>{});
        fixture.project.findComposition(kCompositionId)->nodeLayout()[fixture.node].muted = true;
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan && recipeOperations(*result.plan, node).empty(),
                            "a muted recipe passes its input through");
    }
}
