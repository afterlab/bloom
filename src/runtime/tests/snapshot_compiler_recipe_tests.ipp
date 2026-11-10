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
              const std::vector<std::string>& secondSolidPorts = {},
              const std::string_view outputPort = "image", const bool singleLayer = false) {
    using namespace document;
    RecipeFixture fixture{makeProject(singleLayer ? singleLayerOptions() : ProjectOptions{}),
                          NodeId::fromRaw(1300), {}};
    auto* composition = fixture.project.findComposition(kCompositionId);
    auto& graph = composition->graph();
    const auto* definition = builtInNodeDefinitions().find(
        type, type == kGlowNodeType ? kGlowNodeSchemaVersion : kRecipeNodeSchemaVersion);
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
    require(graph.addNode({fixture.node, std::string(type), std::move(bindings),
                           definition->key.schemaVersion}),
            "recipe node is accepted");
    require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
    require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                           NodeInputRef{fixture.node, "input"}}),
            "source feeds the recipe");
    require(graph.addEdge({EdgeId::fromRaw(1300), {fixture.node, std::string(outputPort)},
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

// #541: with the gradient on, each octave's Levels takes the tint gradient's colour at
// j / (n - 1): a constant gradient folds to constants (here a three-stop gradient whose middle stop
// is off centre), and a keyed one samples per octave in the value graph, time dependent.
void testGlowTintGradientLowering(Expectations& expectations) {
    using namespace document;
    const GradientValue stops{{{0.0, core::Color4d{0.0, 1.0, 1.0, 1.0}},
                               {0.25, core::Color4d{1.0, 1.0, 1.0, 1.0}},
                               {1.0, core::Color4d{0.5, 1.0, 1.0, 1.0}}}};
    const std::unordered_map<std::string, ParameterValue> values{
        {"intensity", 3.0}, {"octaves", std::int64_t{3}}, {"gradient", true},
        {"tintGradient", stops}};
    {
        auto fixture = recipeFixture(kGlowNodeType, values);
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "a Glow with a tint gradient compiles");
        if (result.plan) {
            std::vector<double> reds;
            for (const auto index : recipeOperations(*result.plan, node))
                if (const auto* levels = recipeKernel<runtime::LevelsKernel>(*result.plan, index))
                    if (const auto* red = std::get_if<double>(&levels->channels[1].outputWhite.source))
                        reds.push_back(*red);
            const auto has = [&](const double expected) {
                return std::ranges::any_of(
                    reds, [&](const double red) { return std::abs(red - expected) < 1e-12; });
            };
            expectations.expect(has(0.0) && has(1.0 + (0.5 - 1.0) * (0.25 / 0.75)) && has(0.5),
                                "the octaves take the gradient's colour at 0, 1/2 and 1");
        }
    }
    auto keyed = recipeFixture(kGlowNodeType, values);
    auto* composition = keyed.project.findComposition(kCompositionId);
    constexpr auto curveId = AnimationCurveId::fromRaw(1360);
    auto later = stops;
    later.stops[1].position = 0.75;
    require(composition->animationCurves().insert(GradientAnimationCurve{
                curveId,
                {{KeyframeId::fromRaw(1361), core::RationalTime::fromInteger(0), stops},
                 {KeyframeId::fromRaw(1362), core::RationalTime::fromInteger(1), later}}}),
            "tint gradient curve is publishable");
    require(composition->parameters().setSource(keyed.parameters.at("tintGradient"),
                                                AnimationCurveSource{curveId}),
            "the tint gradient animates");
    auto keyedResult = compileRecipe(std::move(keyed.project));
    expectations.expect(keyedResult.plan != nullptr, "a Glow with a keyed tint gradient compiles");
    if (keyedResult.plan) {
        std::vector<double> positions;
        for (const auto& operation : keyedResult.plan->valueOperations())
            if (const auto* sample =
                    std::get_if<runtime::CompiledValueGradientSample>(&operation.kernel);
                sample && std::holds_alternative<runtime::CompiledGradientCurve>(sample->gradient))
                positions.push_back(sample->position);
        expectations.expect(positions.size() == 3 &&
                                std::ranges::is_permutation(positions,
                                                            std::vector<double>{0.0, 0.5, 1.0}),
                            "a keyed tint gradient samples once per octave in the value graph");
    }
}

// #506: a Glow saved before the modes binds none of their parameters and lowers to exactly the
// operations a new node with every default does (Classic). Physical keys the premultiplied luma of
// the opaque colour, weighs the octaves 2^(2 - exponent) and Mixes the input's colour away under
// the key; Streak chains five Directional Blur passes per ray; per-channel radius builds the glow
// per channel; lens dirt and roll-off finish the glow layer.
void testGlowModeLowering(Expectations& expectations) {
    using namespace document;
    using runtime::ImageChannelSource;
    {
        const std::unordered_map<std::string, ParameterValue> values{
            {"threshold", 2.0}, {"intensity", 3.0}, {"octaves", std::int64_t{3}}};
        auto current = recipeFixture(kGlowNodeType, values);
        auto legacy = recipeFixture(kGlowNodeType, values);
        auto* composition = legacy.project.findComposition(kCompositionId);
        auto* node = composition->graph().findNode(legacy.node);
        const std::vector<std::string> classic{"threshold", "knee",    "intensity", "size",
                                               "falloff",   "tint",    "octaves",   "glowOnly"};
        std::erase_if(node->parameters, [&](const ParameterBinding& binding) {
            if (std::ranges::find(classic, binding.role) != classic.end())
                return false;
            require(composition->parameters().erase(binding.parameterId),
                    "an unbound #506 parameter is removed");
            return true;
        });
        const auto currentResult = compileRecipe(std::move(current.project));
        const auto legacyResult = compileRecipe(std::move(legacy.project));
        expectations.expect(currentResult.plan && legacyResult.plan &&
                                std::ranges::equal(currentResult.plan->operations(),
                                                   legacyResult.plan->operations()) &&
                                std::ranges::equal(currentResult.plan->valueOperations(),
                                                   legacyResult.plan->valueOperations()),
                            "a Glow saved before the modes lowers exactly as a default Classic "
                            "Glow");
    }
    {
        auto fixture = recipeFixture(kGlowNodeType, {{"mode", kGlowModePhysical},
                                                     {"intensity", 2.0},
                                                     {"exponent", 3.0},
                                                     {"octaves", std::int64_t{3}}});
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Physical Glow compiles");
        if (result.plan) {
            const auto& plan = *result.plan;
            const auto ops = recipeOperations(plan, node);
            // select, key, multiply alpha, 3 blurs, 2 mixes, levels, unlit, removal mix, merge
            require(ops.size() == 12, "Physical Glow with three octaves lowers to twelve");
            const auto* opaque = recipeKernel<runtime::ChannelSelectKernel>(plan, ops[0]);
            const auto* bright = recipeKernel<runtime::ImageCombineKernel>(plan, ops[2]);
            expectations.expect(
                opaque && opaque->sources[3] == ImageChannelSource::One &&
                    recipeKernel<runtime::LumaKeyKernel>(plan, ops[1]) && bright &&
                    bright->mode == runtime::ImageCombineMode::MultiplyAlpha,
                "Physical keys the premultiplied luma and multiplies the source by it");
            std::vector<double> factors;
            for (std::size_t i = 3; i < 8; ++i)
                if (const auto* mix = recipeKernel<runtime::ImageCombineKernel>(plan, ops[i]);
                    mix && mix->mode == runtime::ImageCombineMode::Mix && mix->factor)
                    if (const auto* value = std::get_if<double>(&mix->factor->source))
                        factors.push_back(*value);
            expectations.expect(factors.size() == 2 && std::abs(factors[0] - 1.0 / 1.5) < 1e-15 &&
                                    std::abs(factors[1] - 1.0 / 1.75) < 1e-15,
                                "exponent 3 weighs each wider octave half the one inside it");
            const auto* removal = std::get_if<runtime::CompiledImageEffect>(&plan.operations()[ops[10]]);
            const auto* removalMix = recipeKernel<runtime::ImageCombineKernel>(plan, ops[10]);
            const auto* scatter =
                removalMix && removalMix->factor
                    ? std::get_if<double>(&removalMix->factor->source)
                    : nullptr;
            expectations.expect(removal && removalMix &&
                                    removalMix->mode == runtime::ImageCombineMode::Mix &&
                                    scatter && *scatter == 1.0 && removal->maskInput &&
                                    removal->maskInput->value() == ops[1] &&
                                    removal->secondaryInput &&
                                    removal->secondaryInput->value() == ops[9],
                                "the scattered light leaves the input under the key, at most all "
                                "of it");
            const auto* merge = recipeMerge(plan, ops[11]);
            expectations.expect(merge && merge->entries[1].input.value() == ops[10],
                                "the glow is added to the input it left");
        }
    }
    {
        auto fixture = recipeFixture(kGlowNodeType, {{"mode", kGlowModeStreak},
                                                     {"points", std::int64_t{3}},
                                                     {"angle", 10.0},
                                                     {"length", 102.3}});
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Streak Glow compiles");
        if (result.plan) {
            const auto& plan = *result.plan;
            const auto ops = recipeOperations(plan, node);
            std::vector<std::array<double, 3>> passes;
            std::size_t thirds = 0;
            for (const auto index : ops) {
                if (const auto* pass = recipeKernel<runtime::DirectionalBlurKernel>(plan, index)) {
                    const auto* angle = std::get_if<double>(&pass->angle.source);
                    const auto* spacing = std::get_if<double>(&pass->spacing.source);
                    if (angle && spacing)
                        passes.push_back({*angle, *spacing, pass->decay});
                }
                if (const auto* levels = recipeKernel<runtime::LevelsKernel>(plan, index))
                    if (const auto* alpha =
                            std::get_if<double>(&levels->channels[4].outputWhite.source);
                        alpha && std::abs(*alpha - 1.0 / 3.0) < 1e-15)
                        ++thirds;
            }
            bool chained = passes.size() == 15;
            for (std::size_t i = 0; chained && i < passes.size(); ++i) {
                const double reach = std::ldexp(1.0, static_cast<int>(2 * (i % 5))) / 1023.0;
                chained = std::abs(passes[i][0] - (10.0 + 120.0 * static_cast<double>(i / 5))) <
                              1e-12 &&
                          std::abs(passes[i][1] - 102.3 * reach) < 1e-12 &&
                          std::abs(passes[i][2] - std::pow(0.01, reach)) < 1e-15;
            }
            expectations.expect(chained && thirds == 3,
                                "three rays of five chained passes, 120 degrees apart, each "
                                "carrying a third of the light");
        }
    }
    {
        auto fixture = recipeFixture(kGlowNodeType,
                                     {{"radiusGreen", 0.5},
                                      {"rollOff", 0.5},
                                      {"dirtAmount", 2.0},
                                      {"octaves", std::int64_t{2}}},
                                     {"dirt"});
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Glow with shared controls compiles");
        if (result.plan) {
            const auto& plan = *result.plan;
            const auto ops = recipeOperations(plan, node);
            std::size_t selects = 0;
            std::size_t blurs = 0;
            const runtime::CompiledImageEffect* dirty = nullptr;
            for (const auto index : ops) {
                selects += recipeKernel<runtime::ChannelSelectKernel>(plan, index) != nullptr;
                blurs += recipeKernel<runtime::SeparableBlurKernel>(plan, index) != nullptr;
                if (const auto* combine = recipeKernel<runtime::ImageCombineKernel>(plan, index);
                    combine && combine->channel == render::ImageCoverageChannel::Luma)
                    dirty = std::get_if<runtime::CompiledImageEffect>(&plan.operations()[index]);
            }
            const auto* rollOff = recipeKernel<runtime::HighlightRollOffKernel>(plan, ops.back() - 1);
            expectations.expect(selects == 3 && blurs == 6,
                                "a per-channel radius builds the glow once per channel");
            const auto* dirtSource =
                dirty && dirty->secondaryInput
                    ? std::get_if<runtime::CompiledSolid>(
                          &plan.operations()[dirty->secondaryInput->value()])
                    : nullptr;
            expectations.expect(dirtSource && dirtSource->sourceNodeId == kSecondSolidNode,
                                "the lens dirt modulates the glow by its luma");
            expectations.expect(rollOff != nullptr, "the roll-off finishes the glow layer");
        }
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

// #516 Edge Extend: Multiply Alpha by the input's own alpha, a transparent-edge Rgba blur by the
// distance, Unpremultiply, a Channel Combine of the input's alpha with re-association, a white
// held out by the inverted alpha (the mask), then an Image Mix by the amount through that mask.
void testEdgeExtendLowering(Expectations& expectations) {
    using namespace document;
    auto fixture = recipeFixture(kEdgeExtendNodeType, {{"distance", 6.0}, {"amount", 0.5}});
    const auto node = fixture.node;
    const auto distanceId = fixture.parameters.at("distance");
    const auto amountId = fixture.parameters.at("amount");
    auto result = compileRecipe(std::move(fixture.project));
    expectations.expect(result.plan != nullptr, "Edge Extend compiles");
    if (!result.plan)
        return;
    const auto& plan = *result.plan;
    const auto ops = recipeOperations(plan, node);
    require(ops.size() == 7, "Edge Extend lowers to seven primitives");
    const auto* weighted = recipeKernel<runtime::ImageCombineKernel>(plan, ops[0]);
    const auto* spread = recipeKernel<runtime::SeparableBlurKernel>(plan, ops[1]);
    const auto* straight = recipeKernel<runtime::AlphaAssociationKernel>(plan, ops[2]);
    const auto* combine = recipeKernel<runtime::ChannelCombineKernel>(plan, ops[3]);
    const auto* white = recipeKernel<runtime::ChannelSelectKernel>(plan, ops[4]);
    const auto* held = recipeKernel<runtime::ImageCombineKernel>(plan, ops[5]);
    const auto* mix = recipeKernel<runtime::ImageCombineKernel>(plan, ops[6]);
    expectations.expect(weighted && weighted->mode == runtime::ImageCombineMode::MultiplyAlpha &&
                            !weighted->invert,
                        "the input is scaled by its own alpha");
    const auto* radius = spread ? std::get_if<double>(&spread->radius.source) : nullptr;
    expectations.expect(spread && spread->channels == runtime::BlurChannels::Rgba &&
                            spread->edge == runtime::BlurEdgeMode::Transparent && radius &&
                            *radius == 6.0 && spread->radius.id == distanceId,
                        "the weighted input blurs by the distance with a transparent edge");
    expectations.expect(straight && straight->mode == runtime::AlphaAssociationMode::Unpremultiply,
                        "the blur is divided by its alpha");
    expectations.expect(combine && combine->slot == 3 &&
                            combine->source == runtime::ImageChannelSource::Alpha &&
                            combine->premultiply,
                        "the input's alpha is put back and the colour re-associated");
    expectations.expect(white && white->sources[3] == runtime::ImageChannelSource::One &&
                            held && held->mode == runtime::ImageCombineMode::MultiplyAlpha &&
                            held->invert,
                        "the mask is white held out by the input's inverted alpha");
    const auto* factor = mix && mix->factor ? std::get_if<double>(&mix->factor->source) : nullptr;
    const auto* mixEffect = std::get_if<runtime::CompiledImageEffect>(&plan.operations()[ops[6]]);
    expectations.expect(mix && mix->mode == runtime::ImageCombineMode::Mix && factor &&
                            *factor == 0.5 && mix->factor->id == amountId && mixEffect &&
                            mixEffect->maskInput.has_value() &&
                            mixEffect->maskInput->value() == ops[5] &&
                            mixEffect->secondaryInput.has_value() &&
                            mixEffect->secondaryInput->value() == ops[3],
                        "Image Mix moves the input towards the extended colour by the amount "
                        "through the transparency mask");
}

// #509 Alpha Contour: the input's mask distance transform reaching just past the band, a field
// raster over it reading it from image slot 0 (the eight-instruction band program, premultiplied),
// then a Normal Merge with the input on top for Under. Driven values reach the widest band the
// domains allow; Contour only publishes the raster itself.
void testAlphaContourLowering(Expectations& expectations) {
    using namespace document;
    auto fixture =
        recipeFixture(kAlphaContourNodeType, {{"width", 6.0},
                                              {"softness", 2.0},
                                              {"offset", 3.0},
                                              {"composite", std::int64_t{kAlphaContourUnder}}});
    const auto node = fixture.node;
    auto result = compileRecipe(std::move(fixture.project));
    expectations.expect(result.plan != nullptr, "Alpha Contour compiles");
    if (!result.plan)
        return;
    const auto& plan = *result.plan;
    const auto ops = recipeOperations(plan, node);
    require(ops.size() == 3, "Alpha Contour lowers to a distance, a raster and a Merge");
    const auto* distance = recipeKernel<runtime::MaskDistanceKernel>(plan, ops[0]);
    const auto* raster = recipeKernel<runtime::FieldRasterKernel>(plan, ops[1]);
    const auto* rasterEffect =
        std::get_if<runtime::CompiledImageEffect>(&plan.operations()[ops[1]]);
    const auto* merge = recipeMerge(plan, ops[2]);
    // |offset| + width / 2 + 1/2 + softness / 2 + 2.
    expectations.expect(distance && distance->reach == 9.5,
                        "the distance transform reaches just past the band");
    expectations.expect(
        raster && raster->program.instructions.size() == 8 &&
            raster->program.kind ==
                static_cast<std::uint8_t>(core::field::FieldResultKind::Premultiplied) &&
            rasterEffect && rasterEffect->input.value() == ops[0] &&
            rasterEffect->fieldImages.size() == 1 && rasterEffect->fieldImages[0].value() == ops[0],
        "the band's field rasters over the distance image it reads");
    expectations.expect(merge && merge->entries.size() == 2 &&
                            merge->entries[1].input.value() == ops[1] &&
                            merge->entries[0].blendMode == core::BlendMode::Normal,
                        "Under puts the input over the band");

    auto alone =
        recipeFixture(kAlphaContourNodeType, {{"composite", std::int64_t{kAlphaContourOnly}}});
    const auto aloneNode = alone.node;
    auto aloneResult = compileRecipe(std::move(alone.project));
    expectations.expect(aloneResult.plan &&
                            recipeOperations(*aloneResult.plan, aloneNode).size() == 2,
                        "Contour only publishes the band's raster");
}

// A muted recipe passes its input through, like every image effect.
void testRecipeMute(Expectations& expectations) {
    using namespace document;
    for (const auto type : {kDropShadowNodeType, kGlowNodeType, kLightWrapNodeType,
                            kEdgeExtendNodeType, kAlphaContourNodeType}) {
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

// #483 Z Defocus and Depth Fog lower to their production chains: the depth map (its picked channel
// of an Image), then the gather beside the map's tile maximum and the box prefilter, or the fog
// mix; without a depth (outside Uniform) the input passes through, an unknown channel of an Image
// is a topology failure, and an effect mask mixes the result back with the input.
void testDepthEffectLowering(Expectations& expectations) {
    using namespace document;
    const auto kernelsOf = [](const runtime::CompiledCompositionPlan& plan, const NodeId node) {
        std::vector<const runtime::CompiledImageEffect*> effects;
        for (const auto index : recipeOperations(plan, node))
            if (const auto* effect =
                    std::get_if<runtime::CompiledImageEffect>(&plan.operations()[index]))
                effects.push_back(effect);
        return effects;
    };
    {
        auto fixture = recipeFixture(kZDefocusNodeType,
                                     {{"depthChannel", std::string("G")}, {"maxSize", 12.0}},
                                     {"depth"});
        const auto node = fixture.node;
        const auto params = fixture.parameters;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Z Defocus compiles");
        if (result.plan) {
            const auto effects = kernelsOf(*result.plan, node);
            const runtime::DepthMapKernel* map = nullptr;
            const runtime::DefocusKernel* gather = nullptr;
            const runtime::CompiledImageEffect* gatherEffect = nullptr;
            std::size_t dilates = 0;
            std::size_t boxes = 0;
            for (const auto* effect : effects) {
                if (const auto* item = std::get_if<runtime::DepthMapKernel>(&effect->kernel))
                    map = item;
                if (const auto* item = std::get_if<runtime::DefocusKernel>(&effect->kernel)) {
                    gather = item;
                    gatherEffect = effect;
                }
                if (const auto* item = std::get_if<runtime::SeparableBlurKernel>(&effect->kernel)) {
                    // The tile maximum carries the map's colour (the signed radius in red).
                    dilates += item->kind == runtime::SeparableBlurKind::Dilate &&
                                       item->channels == runtime::BlurChannels::Rgba
                                   ? 1U
                                   : 0U;
                    boxes += item->kind == runtime::SeparableBlurKind::Box ? 1U : 0U;
                }
            }
            expectations.expect(map != nullptr && map->channel == 1 &&
                                    map->output == runtime::DepthMapOutput::CircleOfConfusion &&
                                    map->mode == kDefocusModeDepth,
                                "Z Defocus reads the picked G channel into a circle of confusion");
            expectations.expect(
                gather != nullptr && gatherEffect != nullptr && dilates == 1 && boxes == 1 &&
                    gather->slotImages[runtime::kDefocusTileSlot] == 0 &&
                    gather->slotImages[runtime::kDefocusPrefilterSlot] == 1 &&
                    gather->slotImages[runtime::kDefocusBokehSlot] == -1 &&
                    gatherEffect->fieldImages.size() == 2 &&
                    gather->maxSize.id == params.at("maxSize") &&
                    std::get<double>(gather->maxSize.source) == 12.0,
                "the gather reads the tile maximum (carrying the map) and the prefilter");
        }
    }
    {
        auto fixture = recipeFixture(kZDefocusNodeType);
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr && kernelsOf(*result.plan, node).empty(),
                            "Z Defocus without a depth passes its input through");
    }
    {
        auto fixture = recipeFixture(kZDefocusNodeType, {{"mode", kDefocusModeUniform}});
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        const runtime::DefocusKernel* gather = nullptr;
        if (result.plan)
            for (const auto* effect : kernelsOf(*result.plan, node))
                if (const auto* item = std::get_if<runtime::DefocusKernel>(&effect->kernel))
                    gather = item;
        expectations.expect(gather != nullptr && gather->slotImages[runtime::kDefocusTileSlot] == -1 &&
                                gather->slotImages[runtime::kDefocusPrefilterSlot] == 0,
                            "Uniform blurs without a depth: only the prefilter slot");
    }
    {
        auto fixture = recipeFixture(kZDefocusNodeType, {{"depthChannel", std::string("Z")}},
                                     {"depth"});
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan == nullptr,
                            "a named channel of an Image is a topology failure");
    }
    {
        auto fixture = recipeFixture(kZDefocusNodeType, {}, {"depth", "mask"});
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        bool mixed = false;
        if (result.plan)
            for (const auto* effect : kernelsOf(*result.plan, node))
                if (const auto* item = std::get_if<runtime::ImageCombineKernel>(&effect->kernel))
                    mixed = mixed || item->mode == runtime::ImageCombineMode::Mix;
        expectations.expect(mixed, "an effect mask mixes Z Defocus back with its input");
    }
    {
        auto fixture = recipeFixture(kDepthFogNodeType,
                                     {{"falloff", kFogFalloffExponential}, {"density", 0.25}},
                                     {"depth"});
        const auto node = fixture.node;
        const auto params = fixture.parameters;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Depth Fog compiles");
        if (result.plan) {
            const runtime::DepthMapKernel* map = nullptr;
            const runtime::CompiledImageEffect* mix = nullptr;
            const auto effects = kernelsOf(*result.plan, node);
            for (const auto* effect : effects) {
                if (const auto* item = std::get_if<runtime::DepthMapKernel>(&effect->kernel))
                    map = item;
                if (const auto* item = std::get_if<runtime::ImageCombineKernel>(&effect->kernel);
                    item != nullptr && item->mode == runtime::ImageCombineMode::Mix)
                    mix = effect;
            }
            expectations.expect(map != nullptr && map->output == runtime::DepthMapOutput::Fog &&
                                    map->falloff == kFogFalloffExponential &&
                                    map->density.id == params.at("density") && map->channel == 0,
                                "Depth Fog reads the first channel into an exponential fog factor");
            expectations.expect(mix != nullptr && mix->maskInput.has_value() &&
                                    !effects.empty() && effects.back() == mix,
                                "the fog layer mixes over the input by the fog factor");
        }
    }
    {
        auto fixture = recipeFixture(kDepthFogNodeType);
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr && kernelsOf(*result.plan, node).empty(),
                            "Depth Fog without a depth passes its input through");
    }
}

// #507 Lens Flare lowers to one kernel with its constant counts and choices and every value's
// identity; a connected occlusion input is its one image, an Image keeping the channel selector and
// a Mask read as its coverage with only the selector's invert; unconnected there is no image. A
// muted node passes its Image through.
void testLensFlareLowering(Expectations& expectations) {
    using namespace document;
    const auto flareOf = [](const runtime::CompiledCompositionPlan& plan, const NodeId node)
        -> const runtime::CompiledImageEffect* {
        for (const auto& operation : plan.operations())
            if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
                effect != nullptr && effect->sourceNodeId == node &&
                std::holds_alternative<runtime::LensFlareKernel>(effect->kernel))
                return effect;
        return nullptr;
    };
    {
        auto fixture = recipeFixture(kLensFlareNodeType,
                                     {{"ghostCount", std::int64_t{9}},
                                      {"streakCount", std::int64_t{5}},
                                      {"seed", std::int64_t{42}},
                                      {"irisCount", std::int64_t{2}},
                                      {"irisBlades", std::int64_t{8}},
                                      {"lightMode", kLensFlareLight3D},
                                      {"glowAmount", 0.75},
                                      {"lightPosition", Vec2d{-40.0, 12.5}}});
        const auto node = fixture.node;
        const auto params = fixture.parameters;
        const auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Lens Flare compiles");
        const auto* effect = result.plan ? flareOf(*result.plan, node) : nullptr;
        expectations.expect(effect != nullptr, "Lens Flare lowers to one kernel");
        if (effect != nullptr) {
            const auto& kernel = std::get<runtime::LensFlareKernel>(effect->kernel);
            const auto& glow = kernel.values[lensFlareScalarIndex("glowAmount")];
            expectations.expect(
                kernel.ghostCount == 9 && kernel.streakCount == 5 && kernel.seed == 42 &&
                    kernel.irisCount == 2 && kernel.irisBlades == 8 &&
                    kernel.lightMode == kLensFlareLight3D && effect->fieldImages.empty() &&
                    glow.id == params.at("glowAmount") && std::get<double>(glow.source) == 0.75 &&
                    kernel.position.id == params.at("lightPosition") &&
                    std::get<Vec2d>(kernel.position.source) == Vec2d{-40.0, 12.5} &&
                    kernel.color.id == params.at("lightColor") &&
                    kernel.anamorphicColor.id == params.at("anamorphicColor"),
                "Lens Flare lowers its counts, its choices and each value's identity");
        }
    }
    {
        auto fixture = recipeFixture(kLensFlareNodeType, {{"occlusionChannel", std::int64_t{7}}},
                                     {std::string(kLensFlareOcclusionPort)});
        const auto node = fixture.node;
        const auto result = compileRecipe(std::move(fixture.project));
        const auto* effect = result.plan ? flareOf(*result.plan, node) : nullptr;
        expectations.expect(
            effect != nullptr && effect->fieldImages.size() == 1 &&
                std::get<runtime::LensFlareKernel>(effect->kernel).occlusionChannel == 7,
            "an Image on the occlusion input keeps the channel selector");
    }
    for (const auto [stored, lowered] : {std::pair<std::int64_t, std::int64_t>{7, 5}, {2, 0}}) {
        auto fixture = recipeFixture(kLensFlareNodeType, {{"occlusionChannel", stored}});
        auto* composition = fixture.project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto maskNode = NodeId::fromRaw(1395);
        const auto* maskDefinition = builtInNodeDefinitions().find(kMaskNodeType, 1);
        std::vector<ParameterBinding> maskBindings;
        std::uint64_t next = 1500;
        for (const auto& parameter : maskDefinition->parameters) {
            const auto id = ParameterId::fromRaw(next++);
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{parameter.defaultValue}}),
                    "Mask parameter is valid");
            maskBindings.push_back({parameter.role, id});
        }
        require(graph.addNode({maskNode, std::string(kMaskNodeType), std::move(maskBindings), 1}),
                "Mask node is accepted");
        require(graph.addEdge({EdgeId::fromRaw(1395), {maskNode, "mask"},
                               NodeInputRef{fixture.node, std::string(kLensFlareOcclusionPort)}}),
                "the Mask feeds the occlusion input");
        const auto node = fixture.node;
        const auto result = compileRecipe(std::move(fixture.project));
        const auto* effect = result.plan ? flareOf(*result.plan, node) : nullptr;
        expectations.expect(
            effect != nullptr && effect->fieldImages.size() == 1 &&
                std::get<runtime::LensFlareKernel>(effect->kernel).occlusionChannel == lowered,
            "a Mask on the occlusion input is read as its coverage, keeping only the invert");
    }
    {
        auto fixture = recipeFixture(kLensFlareNodeType);
        fixture.project.findComposition(kCompositionId)->nodeLayout()[fixture.node].muted = true;
        const auto node = fixture.node;
        const auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr && flareOf(*result.plan, node) == nullptr,
                            "a muted Lens Flare passes its Image through");
    }
}

// #486 Film Grain lowers to one kernel with its constant animation and seed and every value's
// identity, and Every Frame varies with time while Held does not. A muted node passes its Image
// through.
void testFilmGrainLowering(Expectations& expectations) {
    using namespace document;
    const auto grainOf = [](const runtime::CompiledCompositionPlan& plan, const NodeId node)
        -> std::optional<std::size_t> {
        for (std::size_t index = 0; index < plan.operations().size(); ++index)
            if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&plan.operations()[index]);
                effect != nullptr && effect->sourceNodeId == node &&
                std::holds_alternative<runtime::FilmGrainKernel>(effect->kernel))
                return index;
        return std::nullopt;
    };
    for (const auto animation : {kFilmGrainEveryFrame, kFilmGrainHeld}) {
        auto fixture = recipeFixture(kFilmGrainNodeType, {{"animation", animation},
                                                          {"seed", std::int64_t{42}},
                                                          {"size", 3.5},
                                                          {"highlights", 0.25}});
        const auto node = fixture.node;
        const auto params = fixture.parameters;
        const auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Film Grain compiles");
        const auto index = result.plan ? grainOf(*result.plan, node) : std::nullopt;
        expectations.expect(index.has_value(), "Film Grain lowers to one kernel");
        if (!index)
            continue;
        const auto& effect = std::get<runtime::CompiledImageEffect>(result.plan->operations()[*index]);
        const auto& kernel = std::get<runtime::FilmGrainKernel>(effect.kernel);
        const auto& size = kernel.values[filmGrainScalarIndex("size")];
        const auto& highlights = kernel.values[filmGrainScalarIndex("highlights")];
        expectations.expect(kernel.animation == animation && kernel.seed == 42 &&
                                size.id == params.at("size") &&
                                std::get<double>(size.source) == 3.5 &&
                                highlights.id == params.at("highlights") &&
                                std::get<double>(highlights.source) == 0.25,
                            "Film Grain lowers its constants and each value's identity");
        expectations.expect(result.plan->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(*index)) ==
                                (animation == kFilmGrainEveryFrame),
                            "only grain drawn for every frame varies with time");
    }
    // Monochrome lowers on the kernel, so a toggle is a new plan; unbound, it lowers off.
    for (const bool monochrome : {false, true}) {
        auto fixture = recipeFixture(kFilmGrainNodeType, {{"monochrome", monochrome}});
        const auto node = fixture.node;
        const auto result = compileRecipe(std::move(fixture.project));
        const auto index = result.plan ? grainOf(*result.plan, node) : std::nullopt;
        expectations.expect(index.has_value() &&
                                std::get<runtime::FilmGrainKernel>(
                                    std::get<runtime::CompiledImageEffect>(
                                        result.plan->operations()[*index])
                                        .kernel)
                                        .monochrome == monochrome,
                            "Film Grain lowers its Monochrome");
    }
    {
        auto fixture = recipeFixture(kFilmGrainNodeType);
        auto* composition = fixture.project.findComposition(kCompositionId);
        auto* record = composition->graph().findNode(fixture.node);
        std::erase_if(record->parameters, [&](const ParameterBinding& binding) {
            if (binding.role != "monochrome")
                return false;
            static_cast<void>(composition->parameters().erase(binding.parameterId));
            return true;
        });
        const auto node = fixture.node;
        const auto result = compileRecipe(std::move(fixture.project));
        const auto index = result.plan ? grainOf(*result.plan, node) : std::nullopt;
        expectations.expect(index.has_value() &&
                                !std::get<runtime::FilmGrainKernel>(
                                     std::get<runtime::CompiledImageEffect>(
                                         result.plan->operations()[*index])
                                         .kernel)
                                     .monochrome,
                            "a Film Grain with no Monochrome binding lowers with it off");
    }
    {
        auto fixture = recipeFixture(kFilmGrainNodeType);
        fixture.project.findComposition(kCompositionId)->nodeLayout()[fixture.node].muted = true;
        const auto node = fixture.node;
        const auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr && !grainOf(*result.plan, node).has_value(),
                            "a muted Film Grain passes its Image through");
    }
}

// #563 Codec Degrade lowers to an Encode pass, a keyframe step, one step per further frame of its GOP
// and a Finish pass, all on the node, each value's identity on the first step; a GOP of one frame is
// the Encode, one keyframe and the Finish. The chain follows time. A muted node passes its Image
// through.
void testCodecDegradeLowering(Expectations& expectations) {
    using namespace document;
    const auto kernelsOf = [](const runtime::CompiledCompositionPlan& plan, const NodeId node) {
        std::vector<const runtime::CodecDegradeKernel*> kernels;
        for (const auto& operation : plan.operations())
            if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
                effect != nullptr && effect->sourceNodeId == node)
                if (const auto* kernel = std::get_if<runtime::CodecDegradeKernel>(&effect->kernel))
                    kernels.push_back(kernel);
        return kernels;
    };
    std::size_t previous = 0;
    for (const std::int64_t gop : {std::int64_t{1}, std::int64_t{4}}) {
        auto fixture = recipeFixture(kCodecDegradeNodeType, {{"gopLength", gop == 1 ? std::int64_t{0}
                                                                               : std::int64_t{2}},
                                                             {"blockSize", std::int64_t{1}},
                                                             {"chromaBleed", false},
                                                             {"quality", 33.0},
                                                             {"persistence", 0.25}});
        const auto node = fixture.node;
        const auto params = fixture.parameters;
        const auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Codec Degrade compiles");
        if (!result.plan)
            continue;
        const auto kernels = kernelsOf(*result.plan, node);
        expectations.expect(kernels.size() >= 3 && kernels.size() > previous,
                            "a longer GOP lowers to more steps");
        previous = kernels.size();
        if (kernels.size() < 3)
            continue;
        expectations.expect(kernels.front()->role == runtime::CodecDegradeRole::Encode &&
                                kernels.back()->role == runtime::CodecDegradeRole::Finish,
                            "Codec Degrade starts with an Encode and ends with a Finish");
        const auto steps = std::count_if(kernels.begin(), kernels.end(), [](const auto* kernel) {
            return kernel->role == runtime::CodecDegradeRole::Step;
        });
        expectations.expect(steps >= gop, "one step per frame of the GOP, at least");
        expectations.expect(kernels.front()->blockPixels == 16 && !kernels.front()->chromaBleed &&
                                kernels.front()->gopFrames == (gop == 1 ? 1 : 4),
                            "Codec Degrade lowers its constants");
        const auto ops = recipeOperations(*result.plan, node);
        expectations.expect(!ops.empty(), "Codec Degrade's operations belong to its node");
        bool animated = false;
        for (std::size_t index = 0; index < result.plan->operations().size(); ++index)
            animated = animated || result.plan->operationTimeDependent(
                                       runtime::OperationIndex::fromRaw(index));
        expectations.expect(animated, "Codec Degrade follows time");
    }
    {
        auto fixture = recipeFixture(kCodecDegradeNodeType);
        fixture.project.findComposition(kCompositionId)->nodeLayout()[fixture.node].muted = true;
        const auto node = fixture.node;
        const auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr && kernelsOf(*result.plan, node).empty(),
                            "a muted Codec Degrade passes its Image through");
    }
}

// #566 Auto Exposure lowers to a look-back window: one time sample for each frame before the current
// (the current is the input itself), two reduction passes for each frame of the window, one filter
// that reads every mean with the node's values on it, and the gain on the input. A muted node passes
// its Image through.
void testAutoExposureLowering(Expectations& expectations) {
    using namespace document;
    for (const std::int64_t lookBack : {std::int64_t{4}, std::int64_t{12}, std::int64_t{64}}) {
        auto fixture = recipeFixture(kAutoExposureNodeType, {{"lookBack", lookBack},
                                                             {"target", 0.25},
                                                             {"attack", 0.5},
                                                             {"release", 2.0},
                                                             {"minGain", -2.0},
                                                             {"maxGain", 4.0},
                                                             {"exposure", 0.75},
                                                             {"whiteBalance", 0.5},
                                                             {"balanceLag", 3.0},
                                                             {"balanceLimit", 2.0}});
        const auto node = fixture.node;
        const auto params = fixture.parameters;
        const auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Auto Exposure compiles");
        if (!result.plan)
            continue;
        std::size_t samples = 0;
        std::size_t reductions = 0;
        std::size_t tiles = 0;
        std::vector<const runtime::CompiledImageEffect*> filters;
        std::vector<const runtime::CompiledImageEffect*> gains;
        for (const auto& operation : result.plan->operations()) {
            const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
            if (effect == nullptr || effect->sourceNodeId != node)
                continue;
            if (const auto* sample = std::get_if<runtime::TimeSampleKernel>(&effect->kernel)) {
                samples += sample->lookBack && sample->lagFrames >= 1 &&
                                   sample->lagFrames < lookBack
                               ? 1
                               : 0;
            } else if (const auto* kernel = std::get_if<runtime::AutoExposureKernel>(&effect->kernel)) {
                if (kernel->role == runtime::AutoExposureRole::Reduce) {
                    ++reductions;
                    tiles += kernel->grid == 64 ? 1 : 0;
                } else if (kernel->role == runtime::AutoExposureRole::Filter) {
                    filters.push_back(effect);
                } else {
                    gains.push_back(effect);
                }
            }
        }
        const auto frames = static_cast<std::size_t>(lookBack);
        expectations.expect(samples == frames - 1, "one time sample for each frame before the current");
        expectations.expect(reductions == 2 * frames && tiles == frames,
                            "two reductions for each frame of the window, the first by tiles");
        expectations.expect(filters.size() == 1 && gains.size() == 1,
                            "one filter and one gain pass");
        if (filters.size() != 1 || gains.size() != 1)
            continue;
        const auto& filter = *filters.front();
        const auto& kernel = std::get<runtime::AutoExposureKernel>(filter.kernel);
        expectations.expect(filter.fieldImages.size() == frames - 1,
                            "the filter reads the window's older means beside the newest");
        const auto value = [&](const char* role) -> const runtime::CompiledScalarParameter& {
            return kernel.values[autoExposureScalarIndex(role)];
        };
        expectations.expect(value("target").id == params.at("target") &&
                                std::get<double>(value("target").source) == 0.25 &&
                                std::get<double>(value("attack").source) == 0.5 &&
                                std::get<double>(value("release").source) == 2.0 &&
                                std::get<double>(value("minGain").source) == -2.0 &&
                                std::get<double>(value("maxGain").source) == 4.0 &&
                                std::get<double>(value("exposure").source) == 0.75 &&
                                std::get<double>(value("whiteBalance").source) == 0.5 &&
                                std::get<double>(value("balanceLag").source) == 3.0 &&
                                std::get<double>(value("balanceLimit").source) == 2.0 &&
                                value("release").id == params.at("release"),
                            "the filter owns the node's values");
        const auto& gain = *gains.front();
        expectations.expect(gain.fieldImages.size() == 1 &&
                                gain.fieldImages[0].value() < result.plan->operations().size() &&
                                std::get_if<runtime::CompiledImageEffect>(
                                    &result.plan->operations()[gain.fieldImages[0].value()]) !=
                                    nullptr,
                            "the gain reads the filter's pixel");
    }
    {
        auto fixture = recipeFixture(kAutoExposureNodeType);
        fixture.project.findComposition(kCompositionId)->nodeLayout()[fixture.node].muted = true;
        const auto node = fixture.node;
        const auto result = compileRecipe(std::move(fixture.project));
        bool any = false;
        if (result.plan)
            for (const auto& operation : result.plan->operations())
                if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation))
                    any = any || (effect->sourceNodeId == node &&
                                  std::holds_alternative<runtime::AutoExposureKernel>(effect->kernel));
        expectations.expect(result.plan != nullptr && !any, "a muted Auto Exposure passes its Image through");
    }
}

// #486 Sharpen lowers to the Gaussian blur of its input, frame edge clamped, and one kernel over the
// input and that blur, each value's identity on the operation it drives. A muted node passes its Image
// through.
void testSharpenLowering(Expectations& expectations) {
    using namespace document;
    auto fixture = recipeFixture(kSharpenNodeType,
                                 {{"amount", 2.5}, {"radius", 7.5}, {"threshold", 0.125}});
    const auto node = fixture.node;
    const auto params = fixture.parameters;
    const auto result = compileRecipe(std::move(fixture.project));
    expectations.expect(result.plan != nullptr, "Sharpen compiles");
    if (!result.plan)
        return;
    const auto ops = recipeOperations(*result.plan, node);
    require(ops.size() == 2, "Sharpen lowers to a blur and its mask");
    const auto* blur = recipeKernel<runtime::SeparableBlurKernel>(*result.plan, ops[0]);
    const auto* sharpen = recipeKernel<runtime::SharpenKernel>(*result.plan, ops[1]);
    require(blur != nullptr && sharpen != nullptr, "Sharpen's operations are its kernels");
    expectations.expect(blur->kind == runtime::SeparableBlurKind::Gaussian &&
                            blur->edge == runtime::BlurEdgeMode::Clamp &&
                            blur->channels == runtime::BlurChannels::Rgba &&
                            blur->radius.id == params.at("radius") &&
                            std::get<double>(blur->radius.source) == 7.5,
                        "the blur is a clamped Gaussian owning the radius");
    expectations.expect(sharpen->amount.id == params.at("amount") &&
                            std::get<double>(sharpen->amount.source) == 2.5 &&
                            sharpen->threshold.id == params.at("threshold") &&
                            std::get<double>(sharpen->threshold.source) == 0.125 &&
                            std::get<double>(sharpen->radius.source) == 7.5 &&
                            sharpen->radius.id != params.at("radius"),
                        "the mask owns its amount and threshold and reuses the radius");
    const auto& effect =
        std::get<runtime::CompiledImageEffect>(result.plan->operations()[ops[1]]);
    const auto& blurEffect =
        std::get<runtime::CompiledImageEffect>(result.plan->operations()[ops[0]]);
    expectations.expect(effect.fieldImages.size() == 1 && effect.fieldImages[0].value() == ops[0] &&
                            effect.input == blurEffect.input,
                        "the mask reads the input and its blur");
    for (const auto index : ops)
        expectations.expect(!result.plan->operationTimeDependent(runtime::OperationIndex::fromRaw(index)),
                            "a constant Sharpen does not vary with time");
    {
        auto muted = recipeFixture(kSharpenNodeType);
        muted.project.findComposition(kCompositionId)->nodeLayout()[muted.node].muted = true;
        const auto mutedNode = muted.node;
        const auto compiled = compileRecipe(std::move(muted.project));
        expectations.expect(compiled.plan != nullptr &&
                                recipeOperations(*compiled.plan, mutedNode).empty(),
                            "a muted Sharpen passes its Image through");
    }
}
