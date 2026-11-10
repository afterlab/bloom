// Included inside the snapshot compiler test's anonymous namespace after the Node Group tests, whose
// fixtures (addDefaultedNode, wire, parameterOf) it reuses.
//
// #639: proof the audit of closed issues found missing, through the production compile and the CPU
// reference. HSL Qualifier (#523) cuts a Mask that limits an Image Mix, held to an independent
// per-pixel computation of the documented model; Light Flicker (#567) drives an Exposure through a
// driver binding, held to source * 2^stops, with plan invalidation and time dependence.

namespace node_proof {

constexpr std::uint32_t kWidth = 32;
constexpr std::uint32_t kHeight = 24;

[[nodiscard]] ProjectOptions smallOptions(const bool secondLayer = false) {
    auto options = singleLayerOptions();
    options.secondLayer = secondLayer;
    options.format = requireValue(document::CompositionFormat::create(kWidth, kHeight),
                                  "the proof format is valid");
    return options;
}

// The layer is unmoved and opaque, so the composition is the content's own pixels.
void centreLayer(document::Project& project) {
    using namespace document;
    auto* composition = project.findComposition(kCompositionId);
    require(composition->parameters().setSource(
                kFirstPosition, ConstantValueSource{Vec2d{kWidth / 2.0, kHeight / 2.0}}),
            "the layer is centred");
    require(composition->parameters().setSource(kFirstOpacity, ConstantValueSource{1.0}),
            "the layer is opaque");
}

[[nodiscard]] std::vector<render::Rgba32f>
renderAt(runtime::CpuCompositionEvaluator& evaluator,
         const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
         const core::RationalTime time, const bool bypassCache,
         std::uint64_t* misses = nullptr) {
    runtime::EvaluationRequest request{.time = time,
                                       .output = plan->output(),
                                       .resolution = runtime::CompositionFormatResolution{},
                                       .quality = runtime::EvaluationQuality::Reference,
                                       .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                       .pixelStorageByteLimit = 1U << 26U};
    request.bypassOperationCache = bypassCache;
    const auto evaluated = evaluator.evaluate(plan, request, runtime::CancellationToken{});
    require(evaluated.frame() != nullptr, "the proof composition renders");
    if (misses != nullptr)
        *misses = evaluated.frame()->operationCacheStatistics().misses;
    const auto pixels = evaluated.frame()->processImage().pixels();
    return {pixels.begin(), pixels.end()};
}

[[nodiscard]] std::vector<render::Rgba32f>
renderAt(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
         const core::RationalTime time = core::RationalTime::fromInteger(0)) {
    runtime::CpuCompositionEvaluator evaluator;
    return renderAt(evaluator, plan, time, true);
}

// ---- HSL Qualifier into Image Mix ----------------------------------------------------------

struct Qualifier final {
    double center = 0.0;
    double width = 360.0;
    double softness = 20.0;
    bool invert = false;
};

// The documented selection (docs/architecture/image-filters.md, HSL Qualifier), in double
// precision on the straight colour: HSV hue and saturation, Rec. 709 luma, each range falling
// linearly from selected to 0 over its softness; the three multiply; invert is 1 - selection; the
// coverage is the selection times alpha.
[[nodiscard]] double qualifierCoverage(const Qualifier& q, const double r, const double g,
                                       const double b, const double alpha) {
    const double high = std::max({r, g, b});
    const double low = std::min({r, g, b});
    const double spread = high - low;
    const double saturation = high > 0.0 ? spread / high : 0.0;
    double hue = 0.0;
    if (spread > 0.0) {
        if (high == r)
            hue = 60.0 * ((g - b) / spread);
        else if (high == g)
            hue = 60.0 * ((b - r) / spread + 2.0);
        else
            hue = 60.0 * ((r - g) / spread + 4.0);
        if (hue < 0.0)
            hue += 360.0;
    }
    const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const auto fall = [](const double distance, const double softness) {
        if (distance <= 0.0)
            return 1.0;
        return softness > 0.0 ? std::max(0.0, 1.0 - distance / softness) : 0.0;
    };
    double around = std::abs(hue - q.center);
    around = std::min(around, 360.0 - around);
    const double hueSelection = fall(around - q.width / 2.0, q.softness);
    // Saturation and luma ranges stay at their defaults (everything selected): 0 to 1 and 0 to 1
    // with softness 0.1 for saturation, luma 0 to 1 with 0.1 (the node's defaults).
    const double satSelection = fall(std::max(0.0 - saturation, saturation - 1.0), 0.1);
    const double lumaSelection = fall(std::max(0.0 - luma, luma - 1.0), 0.1);
    double selection = hueSelection * satSelection * lumaSelection;
    if (q.invert)
        selection = 1.0 - selection;
    return selection * alpha;
}

struct QualifiedChain final {
    document::Project project;
    document::NodeId checker;
};

constexpr auto kProofChecker = document::NodeId::fromRaw(9201);
constexpr auto kProofKey = document::NodeId::fromRaw(9202);
constexpr auto kProofMix = document::NodeId::fromRaw(9203);
constexpr core::Color4d kCellA{1.0, 2.0 / 3.0, 0.0, 1.0}; // hue 40 degrees
constexpr core::Color4d kCellB{0.0, 1.0, 0.0, 1.0};       // hue 120 degrees
constexpr core::Color4d kBase{0.8, 0.2, 0.1, 1.0};
constexpr core::Color4d kOther{0.1, 0.3, 0.9, 1.0};

// base solid -> Image Mix input, other solid -> Image Mix second, a checker -> HSL Qualifier -> Mask
// -> Image Mix mask, Image Mix -> Layer. `checkerOnly` instead shows the checker itself.
[[nodiscard]] document::Project qualifierProject(const Qualifier& q, const double factor,
                                                 const bool muted, const bool checkerOnly) {
    using namespace document;
    auto project = makeProject(smallOptions(true));
    centreLayer(project);
    auto* composition = project.findComposition(kCompositionId);
    require(composition->parameters().setSource(kFirstColor, ConstantValueSource{kBase}) &&
                composition->parameters().setSource(kSecondColor, ConstantValueSource{kOther}) &&
                // The second plate is only the Image Mix's second image: its own layer is hidden.
                composition->parameters().setSource(kSecondOpacity, ConstantValueSource{0.0}),
            "the two plates have their colours");
    std::uint64_t parameters = 9300;
    std::uint64_t edges = 9400;
    addDefaultedNode(*composition, kProofChecker, kCheckerSourceNodeType, parameters,
                     {{"colorA", kCellA},
                      {"colorB", kCellB},
                      {"cellWidth", std::int64_t{8}},
                      {"cellHeight", std::int64_t{8}},
                      {"width", static_cast<std::int64_t>(kWidth)},
                      {"height", static_cast<std::int64_t>(kHeight)}});
    require(composition->graph().eraseEdge(kFirstSourceEdge), "the plate's direct edge is removed");
    const auto content = std::string(kLayerOutputContentInputPort);
    if (checkerOnly) {
        wire(*composition, edges, kProofChecker, "image", kFirstLayerNode, content);
        return project;
    }
    addDefaultedNode(*composition, kProofKey, kHslQualifierNodeType, parameters,
                     {{"hueCenter", q.center},
                      {"hueWidth", q.width},
                      {"hueSoftness", q.softness},
                      {"invert", q.invert}});
    addDefaultedNode(*composition, kProofMix, "bloom.image-mix", parameters, {{"factor", factor}});
    wire(*composition, edges, kFirstSolidNode, "image", kProofMix, "input");
    wire(*composition, edges, kSecondSolidNode, "image", kProofMix, "second");
    wire(*composition, edges, kProofChecker, "image", kProofKey, "input");
    wire(*composition, edges, kProofKey, "mask", kProofMix, "mask");
    wire(*composition, edges, kProofMix, "image", kFirstLayerNode, content);
    if (muted)
        composition->nodeLayout()[kProofKey].muted = true;
    require(project.validate().ok(), "the HSL Qualifier chain is valid document truth");
    return project;
}

} // namespace node_proof

void testHslQualifierChainPixels(Expectations& expectations) {
    using namespace document;
    namespace proof = node_proof;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    const auto checkerPlan = compile(proof::qualifierProject({}, 1.0, false, true), registry);
    require(checkerPlan.plan != nullptr, "the checker alone compiles");
    const auto cells = proof::renderAt(checkerPlan.plan);
    require(cells.size() == std::size_t{proof::kWidth} * proof::kHeight, "the checker renders");
    const auto mixed = [&](const proof::Qualifier& q, const double factor, const bool muted) {
        const auto result = compile(proof::qualifierProject(q, factor, muted, false), registry);
        require(result.plan != nullptr, "the HSL Qualifier chain compiles");
        return proof::renderAt(result.plan);
    };
    struct Config final {
        proof::Qualifier qualifier;
        double factor;
        const char* label;
        bool soft = false;
    };
    // The cells are hue 40 and hue 120 degrees: the first config selects the 40-degree cells half
    // (their distance past the window is half the softness), the second fully, the third selects
    // only the 120-degree cells; the last two invert and halve the factor.
    const Config configs[] = {{{0.0, 40.0, 40.0, false}, 1.0, "a soft edge (selection one half)", true},
                              {{40.0, 40.0, 20.0, false}, 1.0, "the 40-degree cells selected"},
                              {{120.0, 20.0, 10.0, false}, 1.0, "the 120-degree cells selected"},
                              {{40.0, 40.0, 20.0, true}, 1.0, "the selection inverted"},
                              {{40.0, 40.0, 20.0, false}, 0.6, "a factor of 0.6"}};
    for (const auto& config : configs) {
        const auto got = mixed(config.qualifier, config.factor, false);
        double worst = 0.0;
        std::size_t full = 0;
        std::size_t none = 0;
        std::size_t partial = 0;
        for (std::size_t i = 0; i < got.size(); ++i) {
            const double coverage = proof::qualifierCoverage(
                config.qualifier, static_cast<double>(cells[i].red()),
                static_cast<double>(cells[i].green()), static_cast<double>(cells[i].blue()),
                static_cast<double>(cells[i].alpha()));
            const double t = config.factor * coverage;
            const std::array<double, 3> expected{
                proof::kBase.red + t * (proof::kOther.red - proof::kBase.red),
                proof::kBase.green + t * (proof::kOther.green - proof::kBase.green),
                proof::kBase.blue + t * (proof::kOther.blue - proof::kBase.blue)};
            const std::array<float, 3> pixel{got[i].red(), got[i].green(), got[i].blue()};
            for (std::size_t c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs(static_cast<double>(pixel[c]) - expected[c]));
            worst = std::max(worst, std::abs(static_cast<double>(got[i].alpha()) - 1.0));
            full += coverage >= 1.0 - 1e-9 ? 1U : 0U;
            none += coverage <= 1e-9 ? 1U : 0U;
            partial += coverage > 1e-9 && coverage < 1.0 - 1e-9 ? 1U : 0U;
        }
        expectations.expect(worst < 2e-5,
                            std::string("HSL Qualifier into Image Mix, ") + config.label +
                                ": every pixel is the plate mixed by factor * the qualifier's "
                                "coverage (worst difference " +
                                std::to_string(worst) + ")");
        expectations.expect(config.soft ? partial > 0 && none > 0 : full > 0 && none > 0,
                            std::string("the fixture for ") + config.label +
                                " holds the pixels the case is about");
    }
    // A muted qualifier is an empty Mask: nothing is mixed in and the plate is untouched.
    const auto muted = mixed({40.0, 40.0, 20.0, false}, 1.0, true);
    bool plate = true;
    for (const auto& pixel : muted)
        plate = plate && std::abs(static_cast<double>(pixel.red()) - proof::kBase.red) < 1e-6 &&
                std::abs(static_cast<double>(pixel.green()) - proof::kBase.green) < 1e-6 &&
                std::abs(static_cast<double>(pixel.blue()) - proof::kBase.blue) < 1e-6;
    expectations.expect(plate, "a muted HSL Qualifier mixes nothing: the first image is untouched");
}

// ---- Light Flicker driving Exposure ---------------------------------------------------------

namespace node_proof {

constexpr auto kFlickerNode = document::NodeId::fromRaw(9501);
constexpr auto kExposureNode = document::NodeId::fromRaw(9502);
constexpr auto kUnrelatedNode = document::NodeId::fromRaw(9503);
constexpr core::Color4d kPlate{0.25, 0.5, 0.125, 1.0};

struct FlickerSettings final {
    double rate = 12.0;
    double depth = 0.35;
    double dropoutRate = 0.5;
    double dropoutLength = 0.12;
    double dropoutLevel = 0.1;
    double softness = 0.5;
    std::int64_t seed = 7;
};

[[nodiscard]] core::light_flicker::Settings coreSettings(const FlickerSettings& s) {
    core::light_flicker::Settings out;
    out.seed = s.seed;
    out.rate = s.rate;
    out.depth = s.depth;
    out.dropoutRate = s.dropoutRate;
    out.dropoutLength = s.dropoutLength;
    out.dropoutLevel = s.dropoutLevel;
    out.softness = s.softness;
    return out;
}

// plate -> Exposure -> Layer, with the Exposure's amount driven by Light Flicker's `stops`.
[[nodiscard]] document::Project flickerProject(const FlickerSettings& s,
                                               const bool withUnrelated = false,
                                               const double unrelatedValue = 0.0) {
    using namespace document;
    auto project = makeProject(smallOptions());
    centreLayer(project);
    auto* composition = project.findComposition(kCompositionId);
    require(composition->parameters().setSource(kFirstColor, ConstantValueSource{kPlate}),
            "the plate has its colour");
    std::uint64_t parameters = 9600;
    std::uint64_t edges = 9700;
    addDefaultedNode(*composition, kFlickerNode, kLightFlickerNodeType, parameters,
                     {{"rate", s.rate},
                      {"depth", s.depth},
                      {"dropoutRate", s.dropoutRate},
                      {"dropoutLength", s.dropoutLength},
                      {"dropoutLevel", s.dropoutLevel},
                      {"softness", s.softness},
                      {"seed", s.seed}});
    addDefaultedNode(*composition, kExposureNode, "bloom.exposure", parameters);
    require(composition->parameters().setSource(
                parameterOf(*composition, kExposureNode, "amount"),
                DriverBindingSource{kFlickerNode, "stops"}),
            "Light Flicker's stops drive the Exposure");
    require(composition->graph().eraseEdge(kFirstSourceEdge), "the plate's direct edge is removed");
    wire(*composition, edges, kFirstSolidNode, "image", kExposureNode, "input");
    wire(*composition, edges, kExposureNode, "image", kFirstLayerNode,
         std::string(kLayerOutputContentInputPort));
    if (withUnrelated)
        addDefaultedNode(*composition, kUnrelatedNode, kScalarValueNodeType, parameters,
                         {{std::string(kValueParameterRole), unrelatedValue}});
    require(project.validate().ok(), "the Light Flicker chain is valid document truth");
    return project;
}

} // namespace node_proof

void testLightFlickerDrivesExposure(Expectations& expectations) {
    using namespace document;
    namespace proof = node_proof;
    runtime::NodeDefinitionRegistry registry;
    populateRegistry(registry);
    registry.freeze();

    const std::array<core::RationalTime, 5> times{
        core::RationalTime::fromInteger(0), *core::RationalTime::create(1, 4),
        *core::RationalTime::create(1, 2), core::RationalTime::fromInteger(1),
        *core::RationalTime::create(13, 4)};
    const auto expectedPixel = [&](const proof::FlickerSettings& s, const core::RationalTime time) {
        const auto gain = core::light_flicker::evaluate(proof::coreSettings(s), time.toSeconds());
        const double scale = std::exp2(core::light_flicker::stopsOf(gain));
        return std::array<double, 3>{proof::kPlate.red * scale, proof::kPlate.green * scale,
                                     proof::kPlate.blue * scale};
    };
    // Every pixel of the frame is the plate scaled by 2^stops(t): hand-computed from the core
    // function's number only.
    const auto frameMatches = [&](const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
                                  const proof::FlickerSettings& s, const core::RationalTime time) {
        const auto pixels = proof::renderAt(plan, time);
        const auto expected = expectedPixel(s, time);
        bool all = !pixels.empty();
        for (const auto& pixel : pixels)
            all = all && std::abs(static_cast<double>(pixel.red()) - expected[0]) <= 1e-5 * std::max(1.0, expected[0]) &&
                  std::abs(static_cast<double>(pixel.green()) - expected[1]) <= 1e-5 * std::max(1.0, expected[1]) &&
                  std::abs(static_cast<double>(pixel.blue()) - expected[2]) <= 1e-5 * std::max(1.0, expected[2]) &&
                  pixel.alpha() == 1.0F;
        return all;
    };

    const proof::FlickerSettings base;
    const auto result = compile(proof::flickerProject(base), registry);
    expectations.expect(result.plan != nullptr, "Light Flicker driving an Exposure compiles");
    if (!result.plan)
        return;
    std::vector<std::vector<render::Rgba32f>> frames;
    for (const auto time : times) {
        expectations.expect(frameMatches(result.plan, base, time),
                            "the Exposure at t = " + std::to_string(time.toSeconds()) +
                                " s is the plate times 2^stops of Light Flicker");
        frames.push_back(proof::renderAt(result.plan, time));
    }
    bool differ = false;
    for (std::size_t i = 1; i < frames.size(); ++i)
        differ = differ || !samePixelBits(frames[0], frames[i]);
    expectations.expect(differ, "with depth above zero the frames differ between times");

    // Depth 0 with no dropouts is a steady light: every frame is the plate itself.
    proof::FlickerSettings steady = base;
    steady.depth = 0.0;
    steady.dropoutRate = 0.0;
    const auto steadyResult = compile(proof::flickerProject(steady), registry);
    require(steadyResult.plan != nullptr, "the steady flicker compiles");
    const auto first = proof::renderAt(steadyResult.plan, times[0]);
    bool equal = true;
    for (const auto time : times)
        equal = equal && samePixelBits(first, proof::renderAt(steadyResult.plan, time));
    expectations.expect(equal && frameMatches(steadyResult.plan, steady, times[0]),
                        "with depth 0 the frames are equal and the plate is unchanged");

    // Time dependence and the cache: the exposure operation is time dependent; a warm frame at
    // the same time reuses every operation; another time recomputes the exposure and not the plate.
    {
        const auto exposure = std::ranges::find_if(
            result.plan->operations(), [](const auto& operation) {
                const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation);
                return effect != nullptr && effect->sourceNodeId == proof::kExposureNode;
            });
        require(exposure != result.plan->operations().end(), "the Exposure lowers");
        const auto exposureIndex = runtime::OperationIndex::fromRaw(
            static_cast<std::size_t>(exposure - result.plan->operations().begin()));
        expectations.expect(result.plan->operationTimeDependent(exposureIndex),
                            "the Exposure driven by Light Flicker is time dependent");
        runtime::CpuCompositionEvaluator evaluator;
        std::uint64_t coldMisses = 0;
        std::uint64_t warmMisses = 1;
        std::uint64_t otherMisses = 0;
        const auto atA = proof::renderAt(evaluator, result.plan, times[1], false, &coldMisses);
        const auto again = proof::renderAt(evaluator, result.plan, times[1], false, &warmMisses);
        const auto atB = proof::renderAt(evaluator, result.plan, times[3], false, &otherMisses);
        expectations.expect(coldMisses > 0 && warmMisses == 0 && samePixelBits(atA, again),
                            "a frame at the same time is served from the cache with identical pixels");
        expectations.expect(otherMisses > 0 && !samePixelBits(atA, atB),
                            "a cached frame at one time is not reused at another");
        expectations.expect(samePixelBits(atB, proof::renderAt(result.plan, times[3])),
                            "the frame computed through the cache equals the one computed without");
    }

    // Plan invalidation: each setting changes the compiled plan and the frames; an unrelated edit
    // changes neither.
    const auto edited = [&](const char* label, const proof::FlickerSettings& s) {
        const auto changed = compile(proof::flickerProject(s), registry);
        require(changed.plan != nullptr, "the edited flicker compiles");
        expectations.expect(!(*changed.plan == *result.plan),
                            std::string("changing Light Flicker's ") + label +
                                " changes the compiled plan");
        for (const auto time : times)
            expectations.expect(frameMatches(changed.plan, s, time),
                                std::string("after changing ") + label + " the Exposure at t = " +
                                    std::to_string(time.toSeconds()) + " s still follows the function");
        // The edit changes the frames where the function says it does: scan the frames of the first
        // five seconds for one whose gain differs between the two settings (a dropout may be rare).
        std::optional<core::RationalTime> probe;
        for (std::int64_t frame = 0; frame < 120 && !probe; ++frame) {
            const auto time = *core::RationalTime::create(frame, 24);
            if (std::abs(core::light_flicker::evaluate(proof::coreSettings(s), time.toSeconds()) -
                         core::light_flicker::evaluate(proof::coreSettings(base), time.toSeconds())) > 1e-3)
                probe = time;
        }
        expectations.expect(probe.has_value(), std::string("the fixture's ") + label +
                                                   " edit changes the function within five seconds");
        if (probe)
            expectations.expect(!samePixelBits(proof::renderAt(changed.plan, *probe),
                                               proof::renderAt(result.plan, *probe)) &&
                                    frameMatches(changed.plan, s, *probe),
                                std::string("changing Light Flicker's ") + label +
                                    " changes the frame where the function changes");
    };
    {
        auto s = base;
        s.seed = 8;
        edited("seed", s);
        s = base;
        s.depth = 0.9;
        edited("depth", s);
        s = base;
        s.rate = 3.0;
        edited("rate", s);
        s = base;
        s.dropoutRate = 6.0;
        s.dropoutLevel = 0.0;
        edited("dropout rate", s);
        s = base;
        s.softness = 0.0;
        edited("softness", s);
        s = base;
        s.dropoutRate = 6.0;
        s.dropoutLength = 0.5;
        edited("dropout length", s);
        s = base;
        s.dropoutRate = 6.0;
        s.dropoutLevel = 0.6;
        edited("dropout level", s);
    }
    const auto withUnrelated = compile(proof::flickerProject(base, true, 3.0), registry);
    const auto unrelatedEdit = compile(proof::flickerProject(base, true, 42.0), registry);
    auto moved = proof::flickerProject(base, true, 3.0);
    moved.findComposition(kCompositionId)->nodeLayout()[proof::kFlickerNode].muted = false;
    const auto movedResult = compile(std::move(moved), registry);
    expectations.expect(withUnrelated.plan && unrelatedEdit.plan && movedResult.plan &&
                            *withUnrelated.plan == *unrelatedEdit.plan &&
                            *withUnrelated.plan == *movedResult.plan &&
                            *withUnrelated.plan == *result.plan,
                        "editing a value node nothing reads leaves the compiled plan unchanged");
}
