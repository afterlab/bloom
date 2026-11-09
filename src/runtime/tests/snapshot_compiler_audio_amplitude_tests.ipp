// #347 Audio Amplitude through the compiler and the CPU evaluator: the lowering to a value operation
// (the source its audio input ends in, the measure and unit, the window and offset operands; no source
// for an unwired or muted node) and the value it reads from a generated WAV with a known level, as it
// drives a layer's opacity. Uses the helpers of snapshot_compiler_field_tests.ipp.

constexpr auto kAmplitudeNode = document::NodeId::fromRaw(60);
constexpr auto kAmplitudeAudioNode = document::NodeId::fromRaw(61);

struct AmplitudeCase final {
    std::int64_t measure = document::kAudioMeasurePeak;
    std::int64_t unit = document::kAudioUnitLinear;
    double windowMs = 100.0;
    bool wired = true;
    bool muted = false;
};

[[nodiscard]] runtime::SnapshotCompileResult compileAudioAmplitude(const AmplitudeCase& item) {
    namespace apt = bloom::audio_plot_test;
    const auto& fixture = apt::sharedAudioFixture();
    auto project = makeProject(singleLayerOptions());
    require(project.addAsset(fixture.asset), "the audio asset is added");
    // The source second 0.5 is read at time zero: 12 frames at 24 frames per second.
    addFieldNode(project, kAmplitudeAudioNode, document::kAudioSourceNodeType,
                 {{"asset", std::to_string(fixture.asset.id.value())},
                  {"startFrame", std::int64_t{-12}}});
    addFieldNode(project, kAmplitudeNode, document::kAudioAmplitudeNodeType,
                 {{"measure", item.measure}, {"unit", item.unit}, {"window", item.windowMs}});
    if (item.wired)
        connect(project, 930, kAmplitudeAudioNode, std::string(document::kAudioSourceOutputPort),
                kAmplitudeNode, std::string(document::kAudioAnalysisInputPort));
    auto* composition = project.findComposition(kCompositionId);
    require(composition->parameters().setSource(
                kFirstColor, document::ConstantValueSource{core::Color4d{1.0, 1.0, 1.0, 1.0}}),
            "the layer's solid is opaque white");
    require(composition->parameters().setSource(
                kFirstOpacity,
                document::DriverBindingSource{kAmplitudeNode,
                                              std::string(document::kAudioAmplitudeOutputPort)}),
            "the amplitude drives the layer's opacity");
    if (item.muted)
        composition->nodeLayout()[kAmplitudeAudioNode].muted = true;
    require(project.validate().ok(), "an Audio Amplitude document is valid");
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "audio amplitude built-ins");
    registry.freeze();
    return compile(std::move(project), registry);
}

[[nodiscard]] const runtime::CompiledValueAudioAmplitude*
amplitudeKernel(const runtime::CompiledCompositionPlan& plan) {
    for (const auto& operation : plan.valueOperations())
        if (const auto* kernel = std::get_if<runtime::CompiledValueAudioAmplitude>(&operation.kernel))
            return kernel;
    return nullptr;
}

void testAudioAmplitudeLowering(Expectations& expectations) {
    using namespace document;
    namespace apt = bloom::audio_plot_test;
    const auto* definition = builtInNodeDefinitions().find(kAudioAmplitudeNodeType, 1);
    require(definition != nullptr, "the Audio Amplitude definition exists");
    expectations.expect(std::ranges::any_of(definition->inputs,
                                            [](const auto& input) {
                                                return input.name == kAudioAnalysisInputPort &&
                                                       input.valueKind == SocketValueKind::Audio;
                                            }) &&
                            definition->outputs.size() == 1 &&
                            definition->category == NodeCategory::Values &&
                            definition->lowering == runtime::NodeLoweringKind::ValueAudioAmplitude,
                        "Audio Amplitude takes audio, gives one Scalar and is a value node");
    {
        const auto result = compileAudioAmplitude({kAudioMeasurePeak, kAudioUnitDecibels, 250.0});
        expectations.expect(result.plan != nullptr, "a document with Audio Amplitude compiles");
        const auto* kernel = result.plan ? amplitudeKernel(*result.plan) : nullptr;
        expectations.expect(kernel != nullptr, "the node lowers to an Audio Amplitude value operation");
        if (kernel != nullptr) {
            expectations.expect(kernel->measure == kAudioMeasurePeak &&
                                    kernel->unit == kAudioUnitDecibels,
                                "the measure and the unit reach the kernel");
            expectations.expect(kernel->source != nullptr &&
                                    kernel->source->asset.contentDigest ==
                                        apt::sharedAudioFixture().asset.contentDigest &&
                                    kernel->source->startFrame == -12,
                                "the kernel reads the audio source its input ends in, at its start frame");
            const auto* literal = std::get_if<runtime::CompiledValue>(&kernel->window.source);
            const auto* milliseconds = literal != nullptr ? std::get_if<double>(literal) : nullptr;
            expectations.expect(milliseconds != nullptr && *milliseconds == 250.0,
                                "the window is the node's own, 250 ms");
        }
    }
    {
        const auto result = compileAudioAmplitude({.wired = false});
        const auto* kernel = result.plan ? amplitudeKernel(*result.plan) : nullptr;
        expectations.expect(kernel != nullptr && kernel->source == nullptr,
                            "an unwired node has no audio to measure");
    }
    {
        const auto result = compileAudioAmplitude({.muted = true});
        const auto* kernel = result.plan ? amplitudeKernel(*result.plan) : nullptr;
        expectations.expect(kernel != nullptr && kernel->source == nullptr,
                            "a muted audio source is no audio to measure");
    }
}

// The opacity the amplitude drives, read from the layer's solid (a white layer, so its alpha is the
// opacity the amplitude gave).
[[nodiscard]] std::optional<float> amplitudeOpacity(const runtime::SnapshotCompileResult& compiled) {
    namespace apt = bloom::audio_plot_test;
    if (compiled.plan == nullptr)
        return std::nullopt;
    runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(apt::sharedAudioFixture().directory);
    const auto result = evaluator.evaluate(
        compiled.plan,
        {.time = core::RationalTime::fromInteger(0),
         .output = compiled.plan->output(),
         .resolution = runtime::CompositionFormatResolution{},
         .quality = runtime::EvaluationQuality::Reference,
         .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
         .pixelStorageByteLimit = 1U << 27U,
         .bypassOperationCache = true},
        runtime::CancellationToken{});
    if (result.frame() == nullptr)
        return std::nullopt;
    const auto& image = result.frame()->processImage();
    const auto window = image.descriptor()->dataWindow();
    const auto pixel = image.read(window.originX() + 1, window.originY() + 1);
    if (!pixel)
        return std::nullopt;
    return pixel.value()->alpha();
}

void testAudioAmplitudeEvaluation(Expectations& expectations) {
    using namespace document;
    runtime::clearAudioAnalysis();
    // The left channel is a sine of 0.5 and the right a constant 0.25: the peak over any window of the
    // first second is 0.5 and the RMS sqrt((0.25 / 2 + 0.0625) / 2).
    const auto near = [](const std::optional<float>& value, const double want) {
        return value.has_value() && std::fabs(static_cast<double>(*value) - want) < 2e-3;
    };
    const auto peak = amplitudeOpacity(compileAudioAmplitude({kAudioMeasurePeak, kAudioUnitLinear}));
    expectations.expect(near(peak, 0.5), "Audio Amplitude reads a peak of 0.5 from the known level");
    const auto rms = amplitudeOpacity(compileAudioAmplitude({kAudioMeasureRms, kAudioUnitLinear}));
    expectations.expect(near(rms, std::sqrt((0.25 / 2.0 + 0.0625) / 2.0)),
                        "Audio Amplitude reads the RMS of the known level");
    const auto statistics = runtime::audioAnalysisStatistics();
    expectations.expect(statistics.computed >= 2, "each measure prepared its own analysis");
    // An unwired node reads silence; its opacity is zero, and a muted source the same.
    expectations.expect(near(amplitudeOpacity(compileAudioAmplitude({.wired = false})), 0.0),
                        "an unwired Audio Amplitude reads silence");
    expectations.expect(near(amplitudeOpacity(compileAudioAmplitude({.muted = true})), 0.0),
                        "a muted audio source reads silence");
    runtime::clearAudioAnalysis();
}
