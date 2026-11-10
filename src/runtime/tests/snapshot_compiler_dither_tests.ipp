// Included inside the snapshot compiler test's anonymous namespace after the Corner Pin, ST Map,
// Displace and Stabilize tests, whose fixture it reuses.
//
// #527 Dither through the evaluator's operation cache: the same plan again is served from it; every
// value the node reads (the amplitude, the seed, the pattern, the matrix size of Ordered, the bit
// depth, the domain, the animation and Monochrome) re-evaluates the node; the matrix size under Blue
// noise changes no key; another frame re-evaluates an Every Frame dither and is served from the cache
// for a Held one; another resolution and another picture re-evaluate it.
void testDitherCacheIdentity(Expectations& expectations) {
    using namespace document;
    runtime::CpuCompositionEvaluator evaluator;
    using Values = std::unordered_map<std::string, ParameterValue>;
    const auto run = [&](const Values& values, const std::int64_t frame = 0,
                         const std::uint32_t w = 240, const std::uint32_t h = 135,
                         const bool recolour = false) {
        auto fixture = warpFixture(kDitherNodeType, values);
        if (recolour) {
            const auto* definition =
                builtInNodeDefinitions().find(kCheckerSourceNodeType, kCheckerSourceNodeSchemaVersion);
            std::uint64_t next = 1380;
            for (const auto& parameter : definition->parameters) {
                const auto id = ParameterId::fromRaw(next++);
                if (parameter.role == "colorB")
                    require(fixture.recipe.project.findComposition(kCompositionId)
                                ->parameters()
                                .setSource(id, ConstantValueSource{core::Color4d{0.4, 0.3, 0.2, 1.0}}),
                            "the picture's colour changes");
            }
        }
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the Dither plan compiles");
        const auto extent = render::ImageExtent::create(w, h);
        runtime::EvaluationRequest request{core::RationalTime::fromInteger(frame), result.plan->output(),
                                           runtime::ProxyResolution{*extent.value()}};
        request.pixelStorageByteLimit = std::size_t{1} << 28U;
        runtime::OperationCacheStatistics stats;
        const auto evaluated = evaluator.evaluate(result.plan, request, {}, {}, nullptr, &stats);
        require(evaluated.frame() != nullptr, "the Dither plan evaluates with the cache");
        return std::pair{stats, std::ranges::find(stats.evaluatedNodes, node) != stats.evaluatedNodes.end()};
    };
    const Values base{{"amplitude", 2.0}};
    const auto cold = run(base);
    expectations.expect(cold.second && cold.first.misses > 0, "Dither: a first run evaluates the node");
    const auto warm = run(base);
    expectations.expect(warm.first.misses == 0 && warm.first.hits > 0 && !warm.second,
                        "Dither: the same plan again is served from the cache");
    const std::vector<std::pair<std::string, ParameterValue>> misses{
        {"amplitude", 2.5},
        {"seed", std::int64_t{9}},
        {"pattern", std::int64_t{kDitherOrdered}},
        {"bitDepth", std::int64_t{1}},
        {"domain", std::int64_t{kDitherStored}},
        {"animation", std::int64_t{kDitherHeld}},
        {"monochrome", true}};
    for (const auto& [role, value] : misses) {
        auto changed = base;
        changed[role] = value;
        expectations.expect(run(changed).second, "Dither: a changed " + role + " re-evaluates the node");
    }
    auto bayer = base;
    bayer["pattern"] = std::int64_t{kDitherOrdered};
    static_cast<void>(run(bayer));
    auto bayerSize = bayer;
    bayerSize["matrixSize"] = std::int64_t{0};
    expectations.expect(run(bayerSize).second, "Dither: Ordered re-evaluates for another matrix size");
    auto blueSize = base;
    blueSize["matrixSize"] = std::int64_t{0};
    const auto unread = run(blueSize);
    expectations.expect(unread.first.misses == 0 && !unread.second,
                        "Dither: the matrix size under Blue noise changes no key");
    expectations.expect(run(base, 1).second, "Dither: another frame re-evaluates Every Frame");
    auto held = base;
    held["animation"] = std::int64_t{kDitherHeld};
    static_cast<void>(run(held, 0));
    expectations.expect(!run(held, 1).second, "Dither: another frame of a Held dither is served from the cache");
    expectations.expect(run(base, 0, 480, 270).second, "Dither: another resolution re-evaluates the node");
    expectations.expect(run(base, 0, 240, 135, true).second,
                        "Dither: a different picture re-evaluates the node");
}
