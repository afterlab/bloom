// #525 Fields through the compiler and the CPU evaluator: a generator's image and mask outputs,
// a chain of field nodes compiled into one program behind Rasterize Field (Explicit extent cut
// from the composition canvas), a Field Sample driving a layer's opacity, image slots (Image Field
// and a mask's Shape Distance), the program limits, mute, and time dependence.

[[nodiscard]] document::Project fieldProject() {
    auto options = singleLayerOptions();
    options.format = requireValue(document::CompositionFormat::create(16, 12), "field extent");
    auto project = makeProject(options);
    auto& parameters = project.findComposition(kCompositionId)->parameters();
    require(parameters.setSource(kFirstPosition,
                                 document::ConstantValueSource{document::Vec2d{8.0, 6.0}}),
            "center the layer");
    return project;
}

// Adds Fields node `type` with its definition's defaults, `overrides` replacing some.
void addFieldNode(document::Project& project, const document::NodeId id,
                  const std::string_view type,
                  const std::map<std::string, document::ParameterValue>& overrides = {},
                  std::map<std::string, document::ParameterId>* ids = nullptr) {
    using namespace document;
    auto* composition = project.findComposition(kCompositionId);
    const auto* definition = builtInNodeDefinitions().find(type, 1);
    require(definition != nullptr, "the Fields definition exists");
    std::vector<ParameterBinding> bindings;
    std::uint64_t next = 5000 + id.value() * 100;
    for (const auto& parameter : definition->parameters) {
        auto value = parameter.defaultValue;
        if (const auto found = overrides.find(parameter.role); found != overrides.end())
            value = found->second;
        const auto parameterId = ParameterId::fromRaw(next++);
        require(composition->parameters().insert(
                    {parameterId, parameter.schemaKey, ConstantValueSource{value}}),
                "a Fields parameter is valid: " + parameter.role);
        bindings.push_back({parameter.role, parameterId});
        if (ids != nullptr)
            (*ids)[parameter.role] = parameterId;
    }
    require(composition->graph().addNode({id, std::string(type), std::move(bindings), 1}),
            "the Fields node is accepted");
}

void connect(document::Project& project, const std::uint64_t edge, const document::NodeId from,
             const std::string_view port, const document::NodeId to, const std::string_view input) {
    require(project.findComposition(kCompositionId)
                ->graph()
                .addEdge({document::EdgeId::fromRaw(edge), {from, std::string(port)},
                          document::NodeInputRef{to, std::string(input)}}),
            "a Fields edge is accepted");
}

void feedLayer(document::Project& project, const document::NodeId from,
               const std::string_view port) {
    auto& graph = project.findComposition(kCompositionId)->graph();
    require(graph.eraseEdge(kFirstSourceEdge), "the solid edge is removed");
    require(graph.addEdge({kFirstSourceEdge, {from, std::string(port)},
                           document::NodeInputRef{kFirstLayerNode,
                                                  std::string(document::kLayerOutputContentInputPort)}}),
            "the raster feeds the layer");
}

[[nodiscard]] runtime::EvaluationResult evaluateField(const runtime::SnapshotCompileResult& compiled,
                                                      const core::RationalTime time = {}) {
    require(compiled.plan != nullptr, "the field graph compiles");
    return runtime::CpuCompositionEvaluator{}.evaluate(
        compiled.plan,
        {.time = time,
         .output = compiled.plan->output(),
         .resolution = runtime::CompositionFormatResolution{},
         .quality = runtime::EvaluationQuality::Reference,
         .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
         .pixelStorageByteLimit = 1U << 22U},
        runtime::CancellationToken{});
}

[[nodiscard]] const runtime::CompiledImageEffect*
fieldRaster(const runtime::CompiledCompositionPlan& plan, const document::NodeId owner) {
    for (const auto& operation : plan.operations())
        if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
            effect != nullptr && effect->sourceNodeId == owner &&
            std::holds_alternative<runtime::FieldRasterKernel>(effect->kernel))
            return effect;
    return nullptr;
}

[[nodiscard]] bool hasFieldOp(const runtime::FieldProgram& program, const core::field::FieldOp op) {
    return std::ranges::any_of(program.instructions, [op](const auto& instruction) {
        return instruction.op == static_cast<std::uint8_t>(op);
    });
}

void testFieldGeneratorRasters(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "field built-ins");
    registry.freeze();
    const auto noise = NodeId::fromRaw(30);
    for (const auto port : {kFieldImageOutputPort, kFieldMaskOutputPort}) {
        for (const bool muted : {false, true}) {
            auto project = fieldProject();
            addFieldNode(project, noise, kFieldNoiseNodeType,
                         {{"size", Vec2d{4.0, 3.0}}, {"seed", std::int64_t{9}}});
            if (port == kFieldMaskOutputPort) {
                // A mask reaches the layer as the Image Mix mask between the layer's solid and
                // a checker.
                const auto mix = NodeId::fromRaw(31);
                const auto checker = NodeId::fromRaw(32);
                addFieldNode(project, mix, "bloom.image-mix");
                addFieldNode(project, checker, document::kCheckerSourceNodeType,
                             {{"cellWidth", std::int64_t{4}},
                              {"cellHeight", std::int64_t{4}},
                              {"width", std::int64_t{16}},
                              {"height", std::int64_t{12}}});
                connect(project, 920, kFirstSolidNode, "image", mix, "input");
                connect(project, 921, checker, "image", mix, "second");
                connect(project, 922, noise, port, mix, "mask");
                feedLayer(project, mix, "image");
            } else {
                feedLayer(project, noise, port);
            }
            if (muted)
                project.findComposition(kCompositionId)->nodeLayout()[noise].muted = true;
            expectations.expect(project.validate().ok(), "a Noise raster is valid document truth");
            const auto compiled = compile(std::move(project), registry);
            expectations.expect(compiled.plan != nullptr, "a Noise raster compiles");
            if (!compiled.plan)
                continue;
            const auto* raster = fieldRaster(*compiled.plan, noise);
            const auto evaluated = evaluateField(compiled);
            expectations.expect(evaluated.frame() != nullptr, "a Noise raster evaluates");
            if (muted) {
                expectations.expect(raster == nullptr && evaluated.frame() &&
                                        (port == kFieldMaskOutputPort ||
                                         std::ranges::all_of(
                                            evaluated.frame()->processImage().pixels(),
                                            [](const auto& p) {
                                                return p == render::Rgba32f::transparent();
                                            })),
                                    "a muted generator draws nothing");
                continue;
            }
            expectations.expect(raster != nullptr &&
                                    std::holds_alternative<runtime::CompiledSolid>(
                                        compiled.plan->operations()[raster->input.value()]),
                                "a generator's raster replaces a composition-size canvas");
            if (raster == nullptr || !evaluated.frame())
                continue;
            const auto& program = std::get<runtime::FieldRasterKernel>(raster->kernel).program;
            expectations.expect(
                hasFieldOp(program, core::field::FieldOp::Noise) &&
                    program.mode == static_cast<std::uint8_t>(
                                        port == kFieldMaskOutputPort
                                            ? core::field::FieldOutputMode::Mask
                                            : core::field::FieldOutputMode::Raw),
                "the raster's program is the noise, in the output's mode");
            const auto& pixels = evaluated.frame()->processImage().pixels();
            const bool varies = std::ranges::any_of(
                pixels, [&](const auto& p) { return !(p == pixels.front()); });
            const bool bounded = std::ranges::all_of(pixels, [](const auto& p) {
                return std::isfinite(p.red()) && p.alpha() >= 0.0F && p.alpha() <= 1.0F;
            });
            expectations.expect(varies && bounded, "the noise raster varies and stays finite");
            const auto again = evaluateField(compiled);
            expectations.expect(again.frame() &&
                                    std::ranges::equal(again.frame()->processImage().pixels(),
                                                       pixels),
                                "the same frame evaluates to the same pixels");
        }
    }
}

// Noise -> Remap -> Rasterize Field (Explicit), one program; a looping noise reads the clock.
void testFieldChainCompilesToOneProgram(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "field built-ins");
    registry.freeze();
    const auto noise = NodeId::fromRaw(31);
    const auto remap = NodeId::fromRaw(32);
    const auto rasterize = NodeId::fromRaw(33);
    for (const bool loop : {false, true}) {
        auto project = fieldProject();
        addFieldNode(project, noise, kFieldNoiseNodeType,
                     {{"size", Vec2d{5.0, 5.0}},
                      {"loop", loop},
                      {"evolution", 1.0},
                      {"loopLength", 2.0}});
        addFieldNode(project, remap, kFieldRemapNodeType, {{"outHigh", 2.0}});
        addFieldNode(project, rasterize, kFieldRasterizeNodeType,
                     {{"extent", kFieldExtentExplicit},
                      {"explicitCentre", Vec2d{2.0, -1.0}},
                      {"explicitSize", Vec2d{8.0, 6.0}}});
        connect(project, 900, noise, "scalar", remap, "field");
        connect(project, 901, remap, "scalar", rasterize, "field");
        feedLayer(project, rasterize, kFieldImageOutputPort);
        expectations.expect(project.validate().ok(), "a field chain is valid document truth");
        const auto compiled = compile(std::move(project), registry);
        expectations.expect(compiled.plan != nullptr, "a field chain compiles");
        if (!compiled.plan)
            continue;
        const auto rasters = std::ranges::count_if(compiled.plan->operations(), [](const auto& op) {
            const auto* effect = std::get_if<runtime::CompiledImageEffect>(&op);
            return effect && std::holds_alternative<runtime::FieldRasterKernel>(effect->kernel);
        });
        const auto* raster = fieldRaster(*compiled.plan, rasterize);
        expectations.expect(rasters == 1 && raster != nullptr,
                            "the whole chain is one field raster, owned by Rasterize Field");
        if (raster == nullptr)
            continue;
        const auto& program = std::get<runtime::FieldRasterKernel>(raster->kernel).program;
        expectations.expect(hasFieldOp(program, core::field::FieldOp::Noise) &&
                                hasFieldOp(program, core::field::FieldOp::Remap) &&
                                program.usesTime == loop,
                            "the program holds the noise and the remap, and reads the clock only "
                            "when the noise loops");
        // The Explicit extent: an 8 x 6 window cut from the composition canvas.
        const auto* cut = std::get_if<runtime::CompiledImageEffect>(
            &compiled.plan->operations()[raster->input.value()]);
        expectations.expect(cut != nullptr &&
                                std::holds_alternative<runtime::CanvasImageKernel>(cut->kernel),
                            "the explicit extent is a canvas cut");
        const auto evaluated = evaluateField(compiled);
        expectations.expect(evaluated.frame() != nullptr, "the field chain evaluates");
        if (loop) {
            const auto later = evaluateField(compiled, core::RationalTime::fromInteger(1));
            const auto lapped = evaluateField(compiled, core::RationalTime::fromInteger(2));
            expectations.expect(
                evaluated.frame() && later.frame() && lapped.frame() &&
                    !std::ranges::equal(evaluated.frame()->processImage().pixels(),
                                        later.frame()->processImage().pixels()) &&
                    std::ranges::equal(evaluated.frame()->processImage().pixels(),
                                       lapped.frame()->processImage().pixels()),
                "a looping noise moves with the clock and repeats after its loop length");
        }
    }
}

// Field Sample: a field read at one position drives a layer's opacity.
void testFieldSampleDrivesAValue(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "field built-ins");
    registry.freeze();
    const auto gradient = NodeId::fromRaw(34);
    const auto sample = NodeId::fromRaw(35);
    auto project = fieldProject();
    addFieldNode(project, gradient, kFieldGradientNodeType,
                 {{"start", Vec2d{-10.0, 0.0}}, {"end", Vec2d{10.0, 0.0}}});
    addFieldNode(project, sample, kFieldSampleNodeType, {{"position", Vec2d{5.0, 0.0}}});
    connect(project, 902, gradient, "t", sample, "field");
    auto& parameters = project.findComposition(kCompositionId)->parameters();
    require(parameters.setSource(kFirstOpacity, DriverBindingSource{sample, "scalar"}),
            "the sample drives the opacity");
    expectations.expect(project.validate().ok(), "a Field Sample driver is valid document truth");
    const auto compiled = compile(std::move(project), registry);
    expectations.expect(compiled.plan != nullptr, "a Field Sample compiles");
    if (!compiled.plan)
        return;
    const auto& plan = *compiled.plan;
    const auto found = std::ranges::find_if(plan.valueOperations(), [](const auto& operation) {
        return std::holds_alternative<runtime::CompiledValueFieldSample>(operation.kernel);
    });
    expectations.expect(found != plan.valueOperations().end(),
                        "Field Sample is a value operation holding the field's program");
    expectations.expect(std::ranges::none_of(plan.operations(), [](const auto& op) {
                            const auto* effect = std::get_if<runtime::CompiledImageEffect>(&op);
                            return effect &&
                                   std::holds_alternative<runtime::FieldRasterKernel>(effect->kernel);
                        }),
                        "a sampled field draws no raster");
    if (found == plan.valueOperations().end())
        return;
    const auto evaluation = runtime::evaluateValueGraph(
        plan.valueOperations(), plan.valueOutputCount(), core::RationalTime{},
        plan.format().frameRate());
    const auto* value = std::get_if<double>(&evaluation.outputs[found->firstOutput.value()]);
    // The gradient from x = -10 to x = 10 is 0.75 at x = 5.
    expectations.expect(evaluation.diagnostics.empty() && value != nullptr &&
                            std::abs(*value - 0.75) <= 1.0e-6,
                        "Field Sample reads the gradient at its position");
}

// Image Field reads an image slot; Shape Distance from a mask reads the mask's distance image.
void testFieldImageSlotsAndLimits(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "field built-ins");
    registry.freeze();
    {
        const auto reader = NodeId::fromRaw(36);
        const auto rasterize = NodeId::fromRaw(37);
        auto project = fieldProject();
        addFieldNode(project, reader, kFieldFromImageNodeType);
        addFieldNode(project, rasterize, kFieldRasterizeNodeType);
        connect(project, 903, kFirstSolidNode, kSolidSourceOutputPort, reader, "image");
        connect(project, 904, reader, "color", rasterize, "field");
        feedLayer(project, rasterize, kFieldImageOutputPort);
        expectations.expect(project.validate().ok(), "an Image Field is valid document truth");
        const auto compiled = compile(std::move(project), registry);
        const auto* raster = compiled.plan ? fieldRaster(*compiled.plan, rasterize) : nullptr;
        expectations.expect(raster != nullptr && raster->fieldImages.size() == 1 &&
                                std::holds_alternative<runtime::CompiledSolid>(
                                    compiled.plan->operations()[raster->fieldImages[0].value()]),
                            "an Image Field reads its image through the raster's first slot");
        if (raster != nullptr)
            expectations.expect(evaluateField(compiled).frame() != nullptr,
                                "an Image Field raster evaluates");
    }
    {
        const auto mask = NodeId::fromRaw(38);
        const auto shape = NodeId::fromRaw(39);
        auto project = fieldProject();
        addFieldNode(project, mask, kFieldShapeDistanceNodeType,
                     {{"size", Vec2d{6.0, 4.0}}, {"falloff", 0.0}});
        addFieldNode(project, shape, kFieldShapeDistanceNodeType,
                     {{"source", kFieldShapeSourceMask}, {"falloff", 3.0}});
        connect(project, 905, mask, kFieldMaskOutputPort, shape, "maskInput");
        feedLayer(project, shape, kFieldImageOutputPort);
        expectations.expect(project.validate().ok(), "a mask Shape Distance is valid document truth");
        const auto compiled = compile(std::move(project), registry);
        const auto* raster = compiled.plan ? fieldRaster(*compiled.plan, shape) : nullptr;
        const auto* distance =
            raster != nullptr && raster->fieldImages.size() == 1
                ? std::get_if<runtime::CompiledImageEffect>(
                      &compiled.plan->operations()[raster->fieldImages[0].value()])
                : nullptr;
        expectations.expect(distance != nullptr &&
                                std::holds_alternative<runtime::MaskDistanceKernel>(distance->kernel),
                            "a mask Shape Distance reads the mask's exact distance image");
        if (distance != nullptr)
            expectations.expect(evaluateField(compiled).frame() != nullptr,
                                "a mask Shape Distance evaluates");
    }
    {
        // Five images in one program is past the limit of four.
        auto project = fieldProject();
        const auto math = NodeId::fromRaw(40);
        addFieldNode(project, math, kFieldCombineNodeType);
        const std::array<std::string_view, 4> lanes{"x", "y", "z", "w"};
        std::uint64_t edge = 910;
        std::vector<NodeId> readers;
        for (std::uint64_t i = 0; i < 5; ++i) {
            const auto reader = NodeId::fromRaw(41 + i);
            addFieldNode(project, reader, kFieldFromImageNodeType);
            connect(project, edge++, kFirstSolidNode, kSolidSourceOutputPort, reader, "image");
            readers.push_back(reader);
        }
        const auto sum = NodeId::fromRaw(50);
        addFieldNode(project, sum, kFieldMathNodeType);
        connect(project, edge++, readers[3], "scalar", sum, "a");
        connect(project, edge++, readers[4], "scalar", sum, "b");
        for (std::size_t i = 0; i < 3; ++i)
            connect(project, edge++, readers[i], "scalar", math, lanes[i]);
        connect(project, edge++, sum, "scalar", math, "w");
        const auto rasterize = NodeId::fromRaw(51);
        addFieldNode(project, rasterize, kFieldRasterizeNodeType);
        connect(project, edge++, math, "color", rasterize, "field");
        feedLayer(project, rasterize, kFieldImageOutputPort);
        const auto compiled = compile(std::move(project), registry);
        expectations.expect(compiled.plan == nullptr &&
                                std::ranges::any_of(compiled.diagnostics, [](const auto& d) {
                                    return (d.summary + d.detail).find("four images") != std::string::npos;
                                }),
                            "a field program reading five images is refused by name");
    }
}

// #525 (issue 3.4): Colorize takes a field as its input. The field is drawn raw over the
// composition (a scalar as grey) and the Colorize maps that picture.
void testColorizeReadsAField(Expectations& expectations) {
    using namespace document;
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "field built-ins");
    registry.freeze();
    const auto noise = NodeId::fromRaw(60);
    const auto colorize = NodeId::fromRaw(61);
    auto project = fieldProject();
    addFieldNode(project, noise, kFieldNoiseNodeType, {{"size", Vec2d{4.0, 3.0}}});
    addFieldNode(project, colorize, kColorizeNodeType);
    connect(project, 960, noise, "scalar", colorize, "input");
    feedLayer(project, colorize, "image");
    expectations.expect(project.validate().ok(), "a field into Colorize is valid document truth");
    const auto compiled = compile(std::move(project), registry);
    expectations.expect(compiled.plan != nullptr, "a field into Colorize compiles");
    if (!compiled.plan)
        return;
    const auto* raster = fieldRaster(*compiled.plan, noise);
    const runtime::CompiledImageEffect* colour = nullptr;
    for (const auto& operation : compiled.plan->operations())
        if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
            effect != nullptr && effect->sourceNodeId == colorize &&
            std::holds_alternative<runtime::ColorizeKernel>(effect->kernel))
            colour = effect;
    expectations.expect(
        raster != nullptr && colour != nullptr &&
            std::get<runtime::FieldRasterKernel>(raster->kernel).program.mode ==
                static_cast<std::uint8_t>(core::field::FieldOutputMode::Raw) &&
            std::get<runtime::FieldRasterKernel>(raster->kernel).program.kind ==
                static_cast<std::uint8_t>(core::field::FieldResultKind::Scalar) &&
            std::holds_alternative<runtime::CompiledImageEffect>(
                compiled.plan->operations()[colour->input.value()]) &&
            &std::get<runtime::CompiledImageEffect>(
                compiled.plan->operations()[colour->input.value()]) == raster,
        "Colorize reads the field's raw raster over the composition");
    const auto evaluated = evaluateField(compiled);
    expectations.expect(evaluated.frame() != nullptr, "a field through Colorize evaluates");
    if (!evaluated.frame())
        return;
    const auto& pixels = evaluated.frame()->processImage().pixels();
    expectations.expect(
        std::ranges::any_of(pixels, [&](const auto& p) { return !(p == pixels.front()); }),
        "the colorized field varies");
}
