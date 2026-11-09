// #514 Denoise & Smooth through the compiler and the CPU evaluator: Auto strength measures the noise
// of the input over the node's sample region with the Image Statistics route, sets the strength the
// mode's factor of the measured deviation, follows the noise, and is invalidated by moving the
// region. Uses the helpers of snapshot_compiler_field_tests.ipp and the test card project.

struct DenoiseChain final {
    runtime::SnapshotCompileResult compiled;
    runtime::EvaluationResult frame;
};

// A flat grey gradient (all four colours equal), Film Grain of `grain` and a Denoise of `values`,
// into the layer of a `width` x `height` composition, compiled and evaluated at frame 0.
// A Denoise node bound to every parameter of its definition but the `omitted` ones, as a node saved
// before those parameters existed.
void addDenoiseWithout(document::Project& project, const document::NodeId id,
                       const std::map<std::string, document::ParameterValue>& values,
                       const std::set<std::string>& omitted) {
    using namespace document;
    auto* composition = project.findComposition(kCompositionId);
    const auto* definition = builtInNodeDefinitions().find(kDenoiseNodeType, 1);
    require(definition != nullptr, "the Denoise definition exists");
    std::vector<ParameterBinding> bindings;
    std::uint64_t next = 5000 + id.value() * 100;
    for (const auto& parameter : definition->parameters) {
        if (omitted.contains(parameter.role))
            continue;
        auto value = parameter.defaultValue;
        if (const auto found = values.find(parameter.role); found != values.end())
            value = found->second;
        const auto parameterId = ParameterId::fromRaw(next++);
        require(composition->parameters().insert(
                    {parameterId, parameter.schemaKey, ConstantValueSource{value}}),
                "a Denoise parameter is valid: " + parameter.role);
        bindings.push_back({parameter.role, parameterId});
    }
    require(composition->graph().addNode({id, std::string(kDenoiseNodeType), std::move(bindings), 1}),
            "the Denoise node is accepted");
}

[[nodiscard]] DenoiseChain
denoiseChain(const double grain, const std::map<std::string, document::ParameterValue>& values,
             const std::uint32_t width = 160, const std::uint32_t height = 120,
             const std::set<std::string>& omitted = {}) {
    using namespace document;
    auto project = testCardProject(width, height);
    const auto source = NodeId::fromRaw(60);
    const auto noise = NodeId::fromRaw(61);
    const auto denoise = NodeId::fromRaw(62);
    const core::Color4d grey{0.4, 0.4, 0.4, 1.0};
    addFieldNode(project, source, kFourColorGradientNodeType,
                 {{"color1", grey}, {"color2", grey}, {"color3", grey}, {"color4", grey}});
    addFieldNode(project, noise, kFilmGrainNodeType, {{"intensity", grain}});
    if (omitted.empty())
        addFieldNode(project, denoise, kDenoiseNodeType, values);
    else
        addDenoiseWithout(project, denoise, values, omitted);
    connect(project, 7100, source, "image", noise, "input");
    connect(project, 7101, noise, "image", denoise, "input");
    feedLayer(project, denoise, "image");
    require(project.validate().ok(), "a Denoise chain is valid document truth");
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "denoise built-ins");
    registry.freeze();
    auto compiled = compile(std::move(project), registry);
    require(compiled.plan != nullptr, "a Denoise chain compiles");
    auto frame = evaluateTestCard(compiled, 0, 0, std::size_t{1} << 28U);
    return {std::move(compiled), std::move(frame)};
}

// The statistics plan a compiled Auto strength reads, or null.
[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
denoiseSamplePlan(const runtime::CompiledCompositionPlan& plan) {
    for (const auto& operation : plan.valueOperations())
        if (const auto* statistics =
                std::get_if<runtime::CompiledValueImageStatistics>(&operation.kernel))
            return statistics->plan;
    return nullptr;
}

// The deviation the statistics route measures for `samplePlan` at frame 0, as the CPU reference
// route runs it for a frame.
[[nodiscard]] std::optional<double>
measuredDeviation(const std::shared_ptr<const runtime::CompiledCompositionPlan>& framePlan,
                  const std::shared_ptr<const runtime::CompiledCompositionPlan>& samplePlan) {
    runtime::EvaluationRequest frame{core::RationalTime::fromInteger(0), framePlan->output(),
                                     runtime::CompositionFormatResolution{}};
    frame.quality = runtime::EvaluationQuality::Reference;
    frame.colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene;
    frame.pixelStorageByteLimit = std::size_t{1} << 28U;
    frame.bypassOperationCache = true;
    const runtime::ImageStatisticsQuery query{document::NodeId::fromRaw(62), samplePlan,
                                              frame.time};
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(
        samplePlan, runtime::imageStatisticsRequest(frame, query), {});
    if (result.frame() == nullptr)
        return std::nullopt;
    const auto values = runtime::readImageStatistics(result.frame()->processImage(),
                                                     runtime::ImageStatisticsRoute::CpuReference);
    return values ? std::optional{values->deviation} : std::nullopt;
}

[[nodiscard]] bool sameFramePixels(const runtime::EvaluationResult& a,
                                   const runtime::EvaluationResult& b) {
    if (a.frame() == nullptr || b.frame() == nullptr)
        return false;
    const auto& left = a.frame()->processImage();
    const auto& right = b.frame()->processImage();
    const auto window = left.descriptor()->dataWindow();
    if (window != right.descriptor()->dataWindow())
        return false;
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y)
        for (auto x = window.originX(); x < window.maxXExclusive(); ++x)
            if (cardPixel(left, x, y) != cardPixel(right, x, y))
                return false;
    return true;
}

void testDenoiseAutoStrength(Expectations& expectations) {
    using namespace document;
    constexpr auto bilateral = static_cast<std::int64_t>(DenoiseNodeMode::Bilateral);
    // The measured strength follows the noise: the same frame as a hand-set strength of the mode's
    // factor times the measured deviation, and more grain measures more.
    std::vector<double> strengths;
    for (const double grain : {0.04, 0.12}) {
        const auto automatic =
            denoiseChain(grain, {{"mode", bilateral}, {"radius", 2.0}, {"autoStrength", true}});
        expectations.expect(automatic.frame.frame() != nullptr, "an Auto strength frame renders");
        const auto sample = denoiseSamplePlan(*automatic.compiled.plan);
        expectations.expect(sample != nullptr, "Auto strength reads a statistics plan of its own");
        if (sample == nullptr)
            continue;
        // The plan cuts the default region (a 64 x 64 square at the centre) out of the input.
        bool region = false;
        for (const auto& operation : sample->operations())
            if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation))
                if (const auto* canvas = std::get_if<runtime::CanvasImageKernel>(&effect->kernel)) {
                    const auto margin = [&](const std::size_t side) {
                        const auto* value = std::get_if<double>(&canvas->margins[side].source);
                        return value != nullptr ? *value : -1.0;
                    };
                    region = canvas->effect == runtime::CanvasImageEffect::Crop &&
                             margin(0) == 48.0 && margin(1) == 28.0 && margin(2) == 48.0 &&
                             margin(3) == 28.0;
                }
        expectations.expect(region, "the statistics plan crops the default sample region");
        const auto deviation = measuredDeviation(automatic.compiled.plan, sample);
        expectations.expect(deviation.has_value() && *deviation > 0.0,
                            "the sample region measures the grain");
        if (!deviation)
            continue;
        const double expected =
            std::min(*deviation * denoiseAutoStrengthFactor(DenoiseNodeMode::Bilateral),
                     kMaximumDenoiseStrengthValue);
        strengths.push_back(expected);
        const auto manual = denoiseChain(
            grain, {{"mode", bilateral}, {"radius", 2.0}, {"strength", expected}});
        expectations.expect(sameFramePixels(automatic.frame, manual.frame),
                            "Auto strength renders the frame of the measured strength set by hand");
        const auto noisy = denoiseChain(grain, {{"mode", bilateral}, {"radius", 0.0}});
        expectations.expect(!sameFramePixels(automatic.frame, noisy.frame),
                            "Auto strength denoises the grain");
    }
    expectations.expect(strengths.size() == 2 && strengths[1] > 2.0 * strengths[0],
                        "three times the grain measures a strength more than twice as large");

    // Moving the region is a different measure: a different statistics plan (so its cached result
    // is not reused) and, over different grain, a different strength.
    const auto centred = denoiseChain(0.12, {{"mode", bilateral}, {"autoStrength", true}});
    const auto moved = denoiseChain(0.12, {{"mode", bilateral},
                                           {"autoStrength", true},
                                           {"sampleFrom", Vec2d{-70.0, 50.0}},
                                           {"sampleTo", Vec2d{-20.0, 10.0}}});
    const auto centredPlan = denoiseSamplePlan(*centred.compiled.plan);
    const auto movedPlan = denoiseSamplePlan(*moved.compiled.plan);
    expectations.expect(centredPlan != nullptr && movedPlan != nullptr &&
                            !(*centredPlan == *movedPlan),
                        "moving the sample region changes the statistics plan");
    const auto centredDeviation =
        centredPlan ? measuredDeviation(centred.compiled.plan, centredPlan) : std::nullopt;
    const auto movedDeviation =
        movedPlan ? measuredDeviation(moved.compiled.plan, movedPlan) : std::nullopt;
    expectations.expect(centredDeviation && movedDeviation && *centredDeviation != *movedDeviation &&
                            !sameFramePixels(centred.frame, moved.frame),
                        "moving the sample region re-measures and changes the frame");

    // Temporal and Median measure the same way; with Auto off the region and its plan are absent.
    const auto temporal = denoiseChain(
        0.12, {{"mode", static_cast<std::int64_t>(DenoiseNodeMode::Temporal)},
               {"autoStrength", true}});
    expectations.expect(temporal.frame.frame() != nullptr &&
                            denoiseSamplePlan(*temporal.compiled.plan) != nullptr,
                        "a Temporal Auto strength measures and renders");
    const auto off = denoiseChain(0.12, {{"mode", bilateral}});
    expectations.expect(denoiseSamplePlan(*off.compiled.plan) == nullptr,
                        "without Auto strength no statistics plan is compiled");

    // A node saved before Auto strength existed (no autoStrength, sampleFrom or sampleTo binding)
    // compiles and renders as Auto strength off; one with Auto strength on and no region measures
    // the default region.
    const auto legacy = denoiseChain(0.12, {{"mode", bilateral}}, 160, 120,
                                     {"autoStrength", "sampleFrom", "sampleTo"});
    expectations.expect(legacy.frame.frame() != nullptr &&
                            denoiseSamplePlan(*legacy.compiled.plan) == nullptr &&
                            sameFramePixels(legacy.frame, off.frame),
                        "a node saved without Auto strength renders as Auto strength off");
    const auto regionless = denoiseChain(0.12, {{"mode", bilateral}, {"autoStrength", true}}, 160,
                                         120, {"sampleFrom", "sampleTo"});
    expectations.expect(regionless.frame.frame() != nullptr &&
                            sameFramePixels(regionless.frame, centred.frame),
                        "Auto strength without a saved region measures the default region");
}
