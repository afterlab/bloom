// #524 and #514 evidence frames: each extension evaluated by the CPU reference through the compiler
// on synthetic sources, written as PPM files when BLOOM_FRAME_DUMP_DIR names a directory (nothing
// otherwise). The frames show the extension next to its off state; the assertions on them are the
// known-answer tests in the files above.

struct FrameStep final {
    std::string type;
    std::map<std::string, document::ParameterValue> values;
};

[[nodiscard]] runtime::EvaluationResult evaluateChain(const std::string_view source,
                                                      const std::map<std::string, document::ParameterValue>& sourceValues,
                                                      const std::vector<FrameStep>& steps,
                                                      const std::uint32_t width,
                                                      const std::uint32_t height) {
    auto project = testCardProject(width, height);
    auto next = document::NodeId::fromRaw(60);
    addFieldNode(project, next, source, sourceValues);
    auto previous = next;
    std::uint64_t edge = 7000;
    for (const auto& step : steps) {
        next = document::NodeId::fromRaw(next.value() + 1);
        addFieldNode(project, next, step.type, step.values);
        connect(project, edge++, previous, "image", next, "input");
        previous = next;
    }
    feedLayer(project, previous, "image");
    require(project.validate().ok(), "an evidence chain is valid document truth");
    runtime::NodeDefinitionRegistry registry;
    require(runtime::registerBuiltInNodeDefinitions(registry), "frame built-ins");
    registry.freeze();
    const auto compiled = compile(std::move(project), registry);
    require(compiled.plan != nullptr, "an evidence chain compiles");
    return evaluateTestCard(compiled, 0, 0, 1U << 28U);
}

void dumpFrame(const std::string& name, const runtime::EvaluationResult& evaluated) {
    const char* directory = std::getenv("BLOOM_FRAME_DUMP_DIR");
    if (directory == nullptr || evaluated.frame() == nullptr)
        return;
    const auto& image = evaluated.frame()->processImage();
    const auto window = image.descriptor()->dataWindow();
    std::ofstream out(std::string(directory) + "/" + name + ".ppm", std::ios::binary);
    out << "P6\n" << window.extent().width() << ' ' << window.extent().height() << "\n255\n";
    const auto encode = [](const float linear) {
        const double v = std::clamp(static_cast<double>(linear), 0.0, 1.0);
        const double s = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
        return static_cast<char>(static_cast<unsigned char>(std::lround(s * 255.0)));
    };
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y)
        for (auto x = window.originX(); x < window.maxXExclusive(); ++x) {
            const auto p = cardPixel(image, x, y);
            // Over mid gray, so coverage reads.
            const double a = static_cast<double>(p[3]);
            const char rgb[3] = {encode(static_cast<float>(static_cast<double>(p[0]) + (1.0 - a) * 0.18)),
                                 encode(static_cast<float>(static_cast<double>(p[1]) + (1.0 - a) * 0.18)),
                                 encode(static_cast<float>(static_cast<double>(p[2]) + (1.0 - a) * 0.18))};
            out.write(rgb, 3);
        }
}

void testExtensionFrames(Expectations& expectations) {
    using namespace document;
    const char* directory = std::getenv("BLOOM_FRAME_DUMP_DIR");
    if (directory == nullptr)
        return;
    const auto run = [&](const std::string& name, const std::string_view source,
                         const std::map<std::string, ParameterValue>& sourceValues,
                         const std::vector<FrameStep>& steps, const std::uint32_t w = 320,
                         const std::uint32_t h = 180) {
        const auto evaluated = evaluateChain(source, sourceValues, steps, w, h);
        expectations.expect(evaluated.frame() != nullptr, "the evidence frame " + name + " evaluates");
        dumpFrame(name, evaluated);
    };
    // Four-Color Gradient: the default and a sharp blend with moved points.
    run("four-color-default", kFourColorGradientNodeType, {}, {});
    run("four-color-sharp", kFourColorGradientNodeType,
        {{"blend", 6.0}, {"point1", Vec2d{-60.0, 40.0}}, {"point4", Vec2d{60.0, -40.0}}}, {});
    // Blur on a checker: off, separate axes (wide horizontal), per-channel (red wide, blue none).
    run("blur-off", kCheckerSourceNodeType, {}, {});
    run("blur-separate", kCheckerSourceNodeType, {},
        {{"bloom.gaussian-blur", {{"radius", 14.0}, {"separate", true}, {"radiusY", 0.0}}}});
    run("blur-per-channel", kCheckerSourceNodeType, {},
        {{"bloom.gaussian-blur",
          {{"radius", 14.0}, {"perChannel", true}, {"redScale", 1.0}, {"greenScale", 0.4}, {"blueScale", 0.0}}}});
    // Transform 2D: skew and opacity on the checker.
    run("transform-skew-opacity", kCheckerSourceNodeType, {},
        {{"bloom.transform-2d", {{"skew", 25.0}, {"skewAxis", 0.0}, {"opacity", 0.6}}}});
    // Hue/Saturation: vibrance and colorize on the gradient.
    run("hue-off", kFourColorGradientNodeType, {{"color1", core::Color4d{0.5, 0.4, 0.35, 1.0}}}, {});
    run("hue-vibrance", kFourColorGradientNodeType, {{"color1", core::Color4d{0.5, 0.4, 0.35, 1.0}}},
        {{"bloom.hue-saturation", {{"vibrance", 1.0}}}});
    run("hue-colorize", kFourColorGradientNodeType, {},
        {{"bloom.hue-saturation", {{"colorize", true}, {"hue", 200.0}, {"saturation", 1.0}}}});
}
