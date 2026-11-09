// Included inside the snapshot compiler test's anonymous namespace after the recipe tests.
//
// #484: every motion-blur node lowers to the one temporal-sampling engine. Directional, Spin and
// Zoom are the path sampler with their parameter identities; Echo and Motion Blur are time samples
// (Offset clocks) of their upstream summed by the accumulator; Vector Motion Blur reads the split
// vectors (passing its input through without them); Optical Flow solves TV-L1 by default and its
// diagnostics are separate ports; Motion Blur (Optical Flow) is the layered splat over four solves;
// Transform 2D with its motion blur on is the path sampler through the transform. Each compiled
// plan evaluates.
void testMotionBlurLowering(Expectations& expectations) {
    using namespace document;
    const auto effectsOf = [](const runtime::CompiledCompositionPlan& plan, const NodeId node) {
        std::vector<const runtime::CompiledImageEffect*> effects;
        for (const auto& operation : plan.operations())
            if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
                effect != nullptr && effect->sourceNodeId == node)
                effects.push_back(effect);
        return effects;
    };
    const auto count = [](const auto& effects, const auto predicate) {
        return static_cast<std::size_t>(std::ranges::count_if(effects, predicate));
    };
    // Merge entries are top first: a matte's foreground splat, then the background's.
    const auto foregroundOnTop = [](const runtime::CompiledCompositionPlan& plan) {
        const auto layerOf = [&](const runtime::OperationIndex index) -> int {
            const auto* effect =
                std::get_if<runtime::CompiledImageEffect>(&plan.operations()[index.value()]);
            const auto* kernel =
                effect ? std::get_if<runtime::VectorBlurKernel>(&effect->kernel) : nullptr;
            return kernel != nullptr && kernel->stage == runtime::VectorBlurStage::Splat
                       ? static_cast<int>(kernel->layer)
                       : -1;
        };
        for (const auto& operation : plan.operations())
            if (const auto* merge = std::get_if<runtime::CompiledMerge>(&operation);
                merge != nullptr && merge->entries.size() == 2 &&
                layerOf(merge->entries[0].input) >= 0 && layerOf(merge->entries[1].input) >= 0)
                return layerOf(merge->entries[0].input) == 1 && layerOf(merge->entries[1].input) == 0;
        return false;
    };
    const runtime::CpuCompositionEvaluator evaluator;
    const auto evaluates = [&](const runtime::SnapshotCompileResult& result) {
        if (!result.plan)
            return false;
        // A small proxy of the fixture's 1080p frame: the kernels run in full on the CPU reference.
        const auto extent = render::ImageExtent::create(240, 135);
        runtime::EvaluationRequest request{core::RationalTime::fromInteger(1), result.plan->output(),
                                           runtime::ProxyResolution{*extent.value()}};
        request.pixelStorageByteLimit = std::size_t{1} << 28U;
        const auto evaluated = evaluator.evaluate(result.plan, request, {});
        for (const auto& diagnostic : evaluated.diagnostics())
            std::cerr << "  " << diagnostic.summary << ": " << diagnostic.detail << '\n';
        return evaluated.frame() != nullptr;
    };

    // Directional, Spin and Zoom.
    for (const auto type : {kDirectionalBlurNodeType, kSpinBlurNodeType, kZoomBlurNodeType}) {
        auto fixture = recipeFixture(type, {{"quality", kMotionQualityDraft}});
        const auto node = fixture.node;
        const auto params = fixture.parameters;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, std::string(type) + " compiles");
        if (!result.plan)
            continue;
        const auto effects = effectsOf(*result.plan, node);
        const auto* path = effects.size() == 1
                               ? std::get_if<runtime::MotionPathBlurKernel>(&effects[0]->kernel)
                               : nullptr;
        const bool directional = type == kDirectionalBlurNodeType;
        expectations.expect(
            path != nullptr &&
                path->mode == (directional          ? runtime::MotionPathMode::Directional
                               : type == kSpinBlurNodeType ? runtime::MotionPathMode::Spin
                                                           : runtime::MotionPathMode::Zoom) &&
                (directional ? path->angle.id == params.at("angle") &&
                                   path->length.id == params.at("length")
                             : path->centre.id == params.at("centre") &&
                                   path->amount.id == params.at("amount")) &&
                path->shutter.quality == kMotionQualityDraft && path->shutter.shutter == 1.0,
            std::string(type) + " is the path sampler with its parameter identities");
        expectations.expect(evaluates(result), std::string(type) + " evaluates");
    }

    // Echo: four time samples back in time, the accumulator chain, Offset clocks.
    {
        auto fixture = recipeFixture(kEchoNodeType,
                                     {{"echoes", std::int64_t{4}}, {"interval", -1.0}});
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Echo compiles");
        if (result.plan) {
            const auto effects = effectsOf(*result.plan, node);
            const auto samples = count(effects, [](const auto* e) {
                return std::holds_alternative<runtime::TimeSampleKernel>(e->kernel);
            });
            const auto sums = count(effects, [](const auto* e) {
                return std::holds_alternative<runtime::TemporalAccumulateKernel>(e->kernel);
            });
            const bool offsetClock = std::ranges::any_of(result.plan->clocks(), [](const auto& c) {
                return c.kind == runtime::CompiledClockKind::Offset;
            });
            expectations.expect(samples == 3 && sums == 3 && offsetClock,
                                "Echo samples its input on Offset clocks and accumulates them");
            expectations.expect(evaluates(result), "Echo evaluates");
        }
    }

    // Frame Rate Hold: one time sample holding the rate; the irregular cadence is off until asked.
    for (const bool irregular : {false, true}) {
        auto fixture = recipeFixture(kFrameRateHoldNodeType,
                                     {{"rate", 15.0},
                                      {"irregular", irregular},
                                      {"amount", 0.4},
                                      {"seed", std::int64_t{7}}});
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Frame Rate Hold compiles");
        if (!result.plan)
            continue;
        const auto effects = effectsOf(*result.plan, node);
        const auto* sample = effects.size() == 1
                                 ? std::get_if<runtime::TimeSampleKernel>(&effects[0]->kernel)
                                 : nullptr;
        expectations.expect(
            sample != nullptr && sample->posterizeNumerator == 15000 &&
                sample->posterizeDenominator == 1000 && sample->offset == core::RationalTime{} &&
                sample->cadenceChance == (irregular ? 200 : 0) &&
                sample->cadenceSeed == (irregular ? 7 : 0),
            std::string("Frame Rate Hold is one posterizing time sample") +
                (irregular ? " with its seeded cadence" : " with a regular cadence"));
        expectations.expect(evaluates(result), "Frame Rate Hold evaluates");
    }

    // Motion Blur: the sub-frame samples hold footage; Draft takes four.
    {
        auto fixture = recipeFixture(kMotionBlurNodeType, {{"quality", kMotionQualityDraft}});
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Motion Blur compiles");
        if (result.plan) {
            const auto effects = effectsOf(*result.plan, node);
            const auto held = count(effects, [](const auto* e) {
                const auto* sample = std::get_if<runtime::TimeSampleKernel>(&e->kernel);
                return sample != nullptr && sample->holdFootage;
            });
            const auto sums = count(effects, [](const auto* e) {
                return std::holds_alternative<runtime::TemporalAccumulateKernel>(e->kernel);
            });
            expectations.expect(held == 4 && sums == 3,
                                "Motion Blur takes its quality's sub-frame samples, holding footage");
            expectations.expect(evaluates(result), "Motion Blur evaluates");
        }
        auto closed = recipeFixture(kMotionBlurNodeType, {{"shutterAngle", 0.0}});
        const auto closedNode = closed.node;
        auto closedResult = compileRecipe(std::move(closed.project));
        expectations.expect(closedResult.plan != nullptr &&
                                effectsOf(*closedResult.plan, closedNode).empty(),
                            "a closed shutter passes the input through");
    }

    // Vector Motion Blur: no vectors pass through; an Image's vectors are split and gathered.
    {
        auto none = recipeFixture(kVectorMotionBlurNodeType);
        const auto noneNode = none.node;
        auto noneResult = compileRecipe(std::move(none.project));
        expectations.expect(noneResult.plan != nullptr && effectsOf(*noneResult.plan, noneNode).empty(),
                            "Vector Motion Blur without vectors passes its input through");
        auto fixture = recipeFixture(kVectorMotionBlurNodeType,
                                     {{"quality", kMotionQualityStandard}}, {"vectors"});
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Vector Motion Blur compiles");
        if (result.plan) {
            const auto effects = effectsOf(*result.plan, node);
            const runtime::VectorBlurKernel* gather = nullptr;
            for (const auto* e : effects)
                if (const auto* k = std::get_if<runtime::VectorBlurKernel>(&e->kernel))
                    gather = k;
            expectations.expect(gather != nullptr &&
                                    gather->stage == runtime::VectorBlurStage::Gather &&
                                    gather->slotImages[runtime::kVectorForwardSlot] == 0,
                                "Vector Motion Blur gathers along the split forward vectors");
            expectations.expect(evaluates(result), "Vector Motion Blur evaluates");
        }
        // RSMB: MaxDisplace scales the stored value, y points up, alpha marks valid vectors.
        auto rsmb = recipeFixture(kVectorMotionBlurNodeType,
                                  {{"convention", kVectorConventionRsmb}, {"maxDisplace", 1024.0}},
                                  {"vectors"});
        const auto rsmbNode = rsmb.node;
        auto rsmbResult = compileRecipe(std::move(rsmb.project));
        const runtime::VectorBlurKernel* rsmbKernel = nullptr;
        if (rsmbResult.plan)
            for (const auto* e : effectsOf(*rsmbResult.plan, rsmbNode))
                if (const auto* k = std::get_if<runtime::VectorBlurKernel>(&e->kernel))
                    rsmbKernel = k;
        expectations.expect(rsmbKernel != nullptr && rsmbKernel->decode.scale == 1024.0 &&
                                rsmbKernel->decode.flipY && rsmbKernel->decode.alphaValid &&
                                rsmbKernel->shutter.maxBlur == 512.0,
                            "the RSMB convention reads MaxDisplace, y up, alpha valid");
        auto layered = recipeFixture(kVectorMotionBlurNodeType,
                                     {{"quality", kMotionQualityHigh}}, {"vectors", "matte"});
        const auto layeredNode = layered.node;
        auto layeredResult = compileRecipe(std::move(layered.project));
        if (layeredResult.plan) {
            const auto effects = effectsOf(*layeredResult.plan, layeredNode);
            const auto splats = count(effects, [](const auto* e) {
                const auto* k = std::get_if<runtime::VectorBlurKernel>(&e->kernel);
                return k != nullptr && k->stage == runtime::VectorBlurStage::Splat;
            });
            const auto fills = count(effects, [](const auto* e) {
                const auto* k = std::get_if<runtime::VectorBlurKernel>(&e->kernel);
                return k != nullptr && k->stage == runtime::VectorBlurStage::Fill;
            });
            expectations.expect(splats == 2 && fills == 1,
                                "High quality splats the matte's layers over the filled background");
            expectations.expect(foregroundOnTop(*layeredResult.plan),
                                "the matte's foreground merges over the background");
        }
        expectations.expect(evaluates(layeredResult), "layered Vector Motion Blur evaluates");
    }

    // Optical Flow: TV-L1 by default.
    {
        auto fixture = recipeFixture(kOpticalFlowNodeType, {{"quality", kMotionQualityDraft}}, {},
                                     kFlowForwardPort);
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Optical Flow compiles");
        for (const auto& diagnostic : result.diagnostics)
            std::cerr << "  " << diagnostic.summary << ": " << diagnostic.detail << '\n';
        if (result.plan) {
            const runtime::OpticalFlowKernel* flow = nullptr;
            for (const auto* e : effectsOf(*result.plan, node))
                if (const auto* k = std::get_if<runtime::OpticalFlowKernel>(&e->kernel))
                    flow = k;
            expectations.expect(flow != nullptr && flow->method == 0,
                                "Optical Flow solves DIS at Draft (Automatic)");
        }
        auto standard = recipeFixture(kOpticalFlowNodeType, {}, {}, kFlowForwardPort);
        const auto standardNode = standard.node;
        auto standardResult = compileRecipe(std::move(standard.project));
        if (standardResult.plan) {
            const runtime::OpticalFlowKernel* flow = nullptr;
            for (const auto* e : effectsOf(*standardResult.plan, standardNode))
                if (const auto* k = std::get_if<runtime::OpticalFlowKernel>(&e->kernel))
                    flow = k;
            expectations.expect(flow != nullptr && flow->method == 1 &&
                                    flow->stage == runtime::OpticalFlowStage::Solve,
                                "Optical Flow solves TV-L1 by default");
        }
        expectations.expect(evaluates(standardResult), "Optical Flow evaluates");
    }

    // Motion Blur (Optical Flow): four solves, the consistency pair, the layered splat.
    {
        auto fixture = recipeFixture(kOpticalFlowMotionBlurNodeType,
                                     {{"quality", kMotionQualityDraft}});
        const auto node = fixture.node;
        auto result = compileRecipe(std::move(fixture.project));
        expectations.expect(result.plan != nullptr, "Motion Blur (Optical Flow) compiles");
        if (result.plan) {
            const auto effects = effectsOf(*result.plan, node);
            const auto solves = count(effects, [](const auto* e) {
                return std::holds_alternative<runtime::OpticalFlowKernel>(e->kernel);
            });
            const runtime::VectorBlurKernel* splat = nullptr;
            for (const auto* e : effects)
                if (const auto* k = std::get_if<runtime::VectorBlurKernel>(&e->kernel);
                    k != nullptr && k->stage == runtime::VectorBlurStage::Splat)
                    splat = k;
            expectations.expect(solves == 4 && splat != nullptr && splat->zMode == 2 &&
                                    splat->shutter.shutter == 0.5 &&
                                    splat->slotImages[runtime::kVectorConsistencySlot] >= 0,
                                "Motion Blur (Optical Flow) splats along four solved flows");
            expectations.expect(evaluates(result), "Motion Blur (Optical Flow) evaluates");
        }
        // With a matte each layer solves its own flow, and High's path is the quadratic.
        auto layered = recipeFixture(kOpticalFlowMotionBlurNodeType,
                                     {{"quality", kMotionQualityHigh}, {"matteShrink", 1.0}},
                                     {"matte"});
        const auto layeredNode = layered.node;
        auto layeredResult = compileRecipe(std::move(layered.project));
        expectations.expect(layeredResult.plan != nullptr, "layered flow blur compiles");
        if (layeredResult.plan) {
            const auto effects = effectsOf(*layeredResult.plan, layeredNode);
            const auto foreground = count(effects, [](const auto* e) {
                const auto* k = std::get_if<runtime::OpticalFlowKernel>(&e->kernel);
                return k != nullptr && k->layer == 1 && k->slotImages[1] == 1;
            });
            const auto background = count(effects, [](const auto* e) {
                const auto* k = std::get_if<runtime::OpticalFlowKernel>(&e->kernel);
                return k != nullptr && k->layer == 2 && k->slotImages[1] == 1;
            });
            const auto quadratic = count(effects, [](const auto* e) {
                const auto* k = std::get_if<runtime::VectorBlurKernel>(&e->kernel);
                return k != nullptr && k->stage == runtime::VectorBlurStage::Splat && k->curved;
            });
            expectations.expect(foreground == 4 && background == 4,
                                "each matte layer solves its own four flows");
            expectations.expect(quadratic == 2, "High integrates the quadratic path");
            expectations.expect(foregroundOnTop(*layeredResult.plan),
                                "the flow blur's foreground merges over its background");
        }
    }

    // Transform 2D with its motion blur on.
    for (const bool skewedAndFaded : {false, true}) {
        auto project = makeProject(singleLayerOptions());
        auto* composition = project.findComposition(kCompositionId);
        auto& graph = composition->graph();
        const auto effectId = NodeId::fromRaw(skewedAndFaded ? 1584 : 1484);
        const auto* definition = builtInNodeDefinitions().find(kTransform2DNodeType, 1);
        require(definition != nullptr, "Transform 2D definition exists");
        std::vector<ParameterBinding> bindings;
        std::uint64_t next = skewedAndFaded ? 1584 : 1484;
        for (const auto& parameter : definition->parameters) {
            const auto id = ParameterId::fromRaw(next++);
            auto value = parameter.defaultValue;
            if (parameter.role == "motionBlur")
                value = true;
            if (parameter.role == "quality")
                value = kMotionQualityDraft;
            // Skew and opacity at other than their defaults ride in the shutter samples.
            if (skewedAndFaded && parameter.role == "skew")
                value = 20.0;
            if (skewedAndFaded && parameter.role == "skewAxis")
                value = 15.0;
            if (skewedAndFaded && parameter.role == "opacity")
                value = 0.5;
            require(composition->parameters().insert(
                        {id, parameter.schemaKey, ConstantValueSource{value}}),
                    "Transform 2D parameter is valid");
            bindings.push_back({parameter.role, id});
        }
        require(graph.addNode({effectId, std::string(kTransform2DNodeType), std::move(bindings), 1}),
                "Transform 2D node is accepted");
        require(graph.eraseEdge(kFirstSourceEdge), "source edge is removed");
        require(graph.addEdge({kFirstSourceEdge, {kFirstSolidNode, "image"},
                               NodeInputRef{effectId, "input"}}),
                "source feeds Transform 2D");
        require(graph.addEdge({EdgeId::fromRaw(skewedAndFaded ? 1584 : 1484), {effectId, "image"},
                               NodeInputRef{kFirstLayerNode,
                                            std::string(kLayerOutputContentInputPort)}}),
                "Transform 2D feeds Layer");
        auto result = compileRecipe(std::move(project));
        expectations.expect(result.plan != nullptr,
                            skewedAndFaded ? "Transform 2D with motion blur, skew and opacity compiles"
                                           : "Transform 2D with motion blur compiles");
        if (result.plan) {
            const auto effects = effectsOf(*result.plan, effectId);
            const auto* path = effects.size() == 1
                                   ? std::get_if<runtime::MotionPathBlurKernel>(&effects[0]->kernel)
                                   : nullptr;
            expectations.expect(path != nullptr && path->mode == runtime::MotionPathMode::Transform &&
                                    path->transform.has_value() &&
                                    path->shutter.quality == kMotionQualityDraft,
                                "Transform 2D's motion blur is the path sampler through it");
            if (skewedAndFaded) {
                const auto* skew = path && path->transform
                                       ? std::get_if<double>(&path->transform->skew.source)
                                       : nullptr;
                const auto* opacity = path && path->transform
                                          ? std::get_if<double>(&path->transform->opacity.source)
                                          : nullptr;
                expectations.expect(skew && *skew == 20.0 && opacity && *opacity == 0.5,
                                    "Transform 2D's motion blur keeps its skew and opacity");
            }
            expectations.expect(evaluates(result), "Transform 2D with motion blur evaluates");
        }
    }
}
