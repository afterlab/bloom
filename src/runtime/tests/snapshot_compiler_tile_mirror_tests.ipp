// Included inside the snapshot compiler test's anonymous namespace after the Corner Pin, ST Map,
// Displace and Stabilize tests, whose fixture and sampling helpers it reuses.
//
// #511 Tile & Mirror: each of the four modes lowers to one field raster. The tests prove the lowering
// (which operations, which program, which canvas, time dependence, mute, the values a mode does not
// read), then evaluate the plans on the CPU reference over the checker grid against independent
// double-precision readings of the documented rules: the Tile cell rule, phase, Mirror Edges and
// Clamp to Output; a Wrap Offset that is exact and seamless for whole-pixel shifts; the reflection
// about a Mirror line, either side; the segment symmetry of the Kaleidoscope; the region of interest,
// the render scale, the pixel budget and cancellation.

// The composition frame and the proxy sizes the pixel tests read at.
constexpr double kTileWidth = 1920.0;
constexpr double kTileHeight = 1080.0;

[[nodiscard]] LensFixture
tileMirrorFixture(const std::unordered_map<std::string, document::ParameterValue>& values = {}) {
    return warpFixture(document::kTileMirrorNodeType, values);
}

// The plan of a Tile & Mirror with `values`, over the checker.
[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
tileMirrorPlan(const std::unordered_map<std::string, document::ParameterValue>& values) {
    auto fixture = tileMirrorFixture(values);
    auto result = compileRecipe(std::move(fixture.recipe.project));
    require(result.plan != nullptr, "the Tile & Mirror plan compiles");
    return result.plan;
}

// The premultiplied texel of `image` (a proxy of the composition) at the field position (x, y).
[[nodiscard]] std::array<double, 4> tileSource(const render::Rgba32fImage& image, const double x,
                                               const double y, const int edge = 1) {
    return warpNearest(image, kTileWidth, kTileHeight, x, y, edge);
}

// The field position (pixels from the centre, y up) of the centre of pixel (i, j) of a proxy.
[[nodiscard]] std::array<double, 2> tileField(const std::uint32_t i, const std::uint32_t j,
                                              const std::uint32_t width, const std::uint32_t height) {
    return {(i + 0.5) * kTileWidth / width - kTileWidth / 2.0,
            kTileHeight / 2.0 - (j + 0.5) * kTileHeight / height};
}

// Tile's cell rule in binary64. `centre` and the sizes are as authored (pixels, y down; percent of
// the frame); returns the field position the pixel reads, and whether Clamp to Output cut it.
struct TileRule final {
    document::Vec2d centre{};
    double widthPercent = 100.0;
    double heightPercent = 100.0;
    double outputWidthPercent = 100.0;
    double outputHeightPercent = 100.0;
    double horizontalPhase = 0.0;
    double verticalPhase = 0.0;
    bool mirror = false;
    bool clamp = true;
};

[[nodiscard]] std::optional<std::array<double, 2>> tileRead(const TileRule& r, const double x,
                                                            const double y) {
    if (r.clamp && (std::abs(x) > r.outputWidthPercent * kTileWidth / 200.0 ||
                    std::abs(y) > r.outputHeightPercent * kTileHeight / 200.0))
        return std::nullopt;
    const auto tw = r.widthPercent * kTileWidth / 100.0;
    const auto th = r.heightPercent * kTileHeight / 100.0;
    const auto fold = [&](const double u) {
        if (!r.mirror)
            return (u + 0.5) - std::floor(u + 0.5) - 0.5;
        const auto m = u + 0.5;
        const auto g = m - 2.0 * std::floor(m / 2.0);
        return (g > 1.0 ? 2.0 - g : g) - 0.5;
    };
    const auto ux = x / tw - r.horizontalPhase / 360.0;
    const auto uy = y / th + r.verticalPhase / 360.0;
    return std::array<double, 2>{fold(ux) * tw + r.centre.x, fold(uy) * th - r.centre.y};
}

[[nodiscard]] double tileWorst(const render::Rgba32fImage& image,
                               const std::function<std::array<double, 4>(double, double)>& want,
                               const std::uint32_t width, const std::uint32_t height,
                               std::size_t* mismatched = nullptr) {
    double worst = 0.0;
    std::size_t bad = 0;
    for (std::uint32_t j = 0; j < height; ++j)
        for (std::uint32_t i = 0; i < width; ++i) {
            const auto at = tileField(i, j, width, height);
            const auto expected = want(at[0], at[1]);
            const auto got = lensPixel(image, i, j);
            double here = 0.0;
            for (std::size_t c = 0; c < 4; ++c)
                here = std::max(here, std::abs(static_cast<double>(got[c]) - expected[c]));
            worst = std::max(worst, here);
            if (here > 1.0e-5 && bad++ < 3 && std::getenv("BLOOM_TILE_DEBUG") != nullptr) // NOLINT
                std::cerr << "mismatch at " << i << "," << j << " field " << at[0] << "," << at[1]
                          << " got " << got[0] << "," << got[1] << "," << got[2] << "," << got[3]
                          << " want " << expected[0] << "," << expected[1] << "," << expected[2]
                          << "," << expected[3] << "\n";
        }
    if (mismatched != nullptr)
        *mismatched = bad;
    return worst;
}

void testTileMirrorLowering(Expectations& expectations) {
    using namespace document;
    using core::field::FieldOp;
    const auto lowered = [&](const std::unordered_map<std::string, ParameterValue>& values,
                             const std::int64_t mode, const bool expectSolidCanvas,
                             const std::vector<FieldOp>& expected, const char* what,
                             const runtime::FieldProgram* fixtureProgram) {
        auto fixture = tileMirrorFixture(values);
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr, std::string(what) + " compiles");
        if (!result.plan)
            return;
        const auto ops = recipeOperations(*result.plan, node);
        require(ops.size() == 1, "Tile & Mirror lowers to one raster");
        const auto* raster = lensRaster(*result.plan, ops[0]);
        require(raster != nullptr, "the raster is a field raster");
        const auto& effect = std::get<runtime::CompiledImageEffect>(result.plan->operations()[ops[0]]);
        const auto checker = lensCheckerOperation(*result.plan);
        expectations.expect(checker && effect.fieldImages.size() == 1 &&
                                effect.fieldImages[0].value() == *checker,
                            std::string(what) + " reads the picture");
        const bool solid = std::holds_alternative<runtime::CompiledSolid>(
            result.plan->operations()[effect.input.value()]);
        expectations.expect(solid == expectSolidCanvas &&
                                (expectSolidCanvas || effect.input.value() == *checker),
                            std::string(what) + (expectSolidCanvas
                                                     ? " covers the composition frame"
                                                     : " keeps the input's window"));
        std::vector<FieldOp> ops2;
        for (const auto& instruction : raster->program.instructions)
            ops2.push_back(static_cast<FieldOp>(instruction.op));
        expectations.expect(ops2 == expected, std::string(what) + " is the documented program");
        expectations.expect(raster->samples == 1 &&
                                raster->program.kind ==
                                    static_cast<std::uint8_t>(core::field::FieldResultKind::Premultiplied),
                            std::string(what) + " is one premultiplied read");
        expectations.expect(!result.plan->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(ops[0])),
                            std::string(what) + " with constant values does not vary with time");
        if (fixtureProgram != nullptr)
            expectations.expect(lensSequence(raster->program) == lensSequence(*fixtureProgram),
                                std::string(what) + ": the coverage fixture's program is the lowering's");
        static_cast<void>(mode);
    };
    const std::vector<FieldOp> tileOps{FieldOp::Coordinate, FieldOp::Constant,  FieldOp::Swizzle,
                                       FieldOp::Constant,   FieldOp::Swizzle,   FieldOp::Math,
                                       FieldOp::Math,       FieldOp::Transform, FieldOp::Math,
                                       FieldOp::Homography, FieldOp::Image};
    auto clampOps = tileOps;
    clampOps.insert(clampOps.end() - 1,
                    {FieldOp::Constant, FieldOp::Swizzle, FieldOp::Math, FieldOp::Math, FieldOp::Constant,
                     FieldOp::Math, FieldOp::Swizzle, FieldOp::Math});
    const auto tileProgram = field_test_programs::tileMirrorProgram(1, 0, 1, 1, 0);
    lowered({{"clampToOutput", false}, {"filter", std::int64_t{1}}}, 0, true, tileOps, "Tile",
            &tileProgram);
    const auto clampProgram = field_test_programs::tileMirrorProgram(1, 0, 1, 1, 3);
    lowered({{"mirrorEdges", true}, {"filter", std::int64_t{1}}}, 0, true, clampOps,
            "Tile with Mirror Edges and Clamp to Output", &clampProgram);
    const auto wrapProgram = field_test_programs::tileMirrorProgram(1, 1, 1);
    lowered({{"mode", std::int64_t{1}}, {"filter", std::int64_t{1}}}, 1, false,
            {FieldOp::Coordinate, FieldOp::Homography, FieldOp::Image}, "Wrap Offset", &wrapProgram);
    for (const std::int64_t side : {0, 1}) {
        const auto mirrorProgram = field_test_programs::tileMirrorProgram(1, 2, 1, 1, side);
        lowered({{"mode", std::int64_t{2}}, {"mirrorSide", side}, {"filter", std::int64_t{1}}}, 2,
                false,
                {FieldOp::Coordinate, FieldOp::Constant, FieldOp::Homography, FieldOp::Math,
                 FieldOp::Constant, FieldOp::Math, FieldOp::Swizzle, FieldOp::Math, FieldOp::Constant,
                 FieldOp::Math, FieldOp::Math, FieldOp::Swizzle, FieldOp::Math, FieldOp::Math,
                 FieldOp::Image},
                side == 0 ? "Mirror (left or below)" : "Mirror (right or above)", &mirrorProgram);
    }
    const auto kaleidoscopeProgram = field_test_programs::tileMirrorProgram(1, 3, 1, 1, 7);
    lowered({{"mode", std::int64_t{3}}, {"kaleidoscopeSegments", std::int64_t{7}},
             {"filter", std::int64_t{1}}},
            3, false,
            {FieldOp::Coordinate, FieldOp::Constant, FieldOp::Homography, FieldOp::Math,
             FieldOp::Math, FieldOp::Constant, FieldOp::Swizzle, FieldOp::Math, FieldOp::Swizzle,
             FieldOp::Transform, FieldOp::Math, FieldOp::Image},
            "Kaleidoscope", &kaleidoscopeProgram);

    // A muted node is its input.
    {
        auto fixture = tileMirrorFixture();
        fixture.recipe.project.findComposition(kCompositionId)
            ->nodeLayout()[fixture.recipe.node]
            .muted = true;
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr && recipeOperations(*result.plan, node).empty(),
                            "a muted Tile & Mirror passes its Image through");
    }

    // A keyed value of the mode makes the raster vary with time; a keyed value of a mode that is
    // not the node's mode is not compiled (it would be a curve nothing references) and changes
    // nothing.
    const auto keyed = [&](const std::string_view role, const bool vector, const std::int64_t mode) {
        auto fixture = tileMirrorFixture({{"mode", mode}});
        auto* composition = fixture.recipe.project.findComposition(kCompositionId);
        constexpr auto curveId = AnimationCurveId::fromRaw(1700);
        if (vector) {
            Vec2AnimationCurve curve{curveId};
            curve.components[0].keyframes = {
                {KeyframeId::fromRaw(1701), core::RationalTime::fromInteger(0), 0.0,
                 KeyframeInterpolation::Linear},
                {KeyframeId::fromRaw(1702), core::RationalTime::fromInteger(1), 40.0,
                 KeyframeInterpolation::Linear}};
            require(composition->animationCurves().insert(std::move(curve)), "the curve is publishable");
        } else {
            ScalarAnimationCurve curve{curveId, {}};
            curve.keyframes = {{KeyframeId::fromRaw(1701), core::RationalTime::fromInteger(0), 10.0,
                                KeyframeInterpolation::Linear},
                               {KeyframeId::fromRaw(1702), core::RationalTime::fromInteger(1), 50.0,
                                KeyframeInterpolation::Linear}};
            require(composition->animationCurves().insert(std::move(curve)), "the curve is publishable");
        }
        require(composition->parameters().setSource(fixture.recipe.parameters.at(std::string(role)),
                                                    AnimationCurveSource{curveId}),
                "the value animates");
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "a keyed Tile & Mirror compiles");
        const auto ops = recipeOperations(*result.plan, node);
        require(ops.size() == 1, "a keyed Tile & Mirror keeps its raster");
        return result.plan->operationTimeDependent(runtime::OperationIndex::fromRaw(ops[0]));
    };
    expectations.expect(keyed("tileWidth", false, 0), "a keyed tile width varies with time");
    expectations.expect(keyed("horizontalPhase", false, 0), "a keyed phase varies with time");
    expectations.expect(keyed("tileCenter", true, 0), "a keyed tile centre varies with time");
    expectations.expect(keyed("offset", true, 1), "a keyed wrap offset varies with time");
    expectations.expect(keyed("mirrorAngle", false, 2), "a keyed mirror angle varies with time");
    expectations.expect(keyed("mirrorPoint", true, 2), "a keyed mirror point varies with time");
    expectations.expect(keyed("kaleidoscopeAngle", false, 3), "a keyed rotation varies with time");
    expectations.expect(keyed("kaleidoscopeSize", false, 3), "a keyed size varies with time");
    expectations.expect(keyed("kaleidoscopeCenter", true, 3), "a keyed centre varies with time");
    expectations.expect(!keyed("mirrorAngle", false, 0),
                        "a keyed value another mode reads does not make this mode vary with time");
    expectations.expect(!keyed("offset", true, 2),
                        "a keyed wrap offset does not move a Mirror");
    expectations.expect(!keyed("outputWidth", false, 1),
                        "a keyed output size does not move a Wrap Offset");
}

void testTileMirrorPixels(Expectations& expectations) {
    using namespace document;
    constexpr std::uint32_t kW = 240;
    constexpr std::uint32_t kH = 135;
    const auto bare = warpBarePlan();
    const auto input = lensEvaluate(bare, kW, kH);
    require(input.frame() != nullptr, "the bare checker evaluates");
    const auto& source = input.frame()->processImage();
    const auto evaluate = [&](const std::unordered_map<std::string, ParameterValue>& values,
                              const std::uint32_t w = kW, const std::uint32_t h = kH) {
        const auto plan = tileMirrorPlan(values);
        auto result = lensEvaluate(plan, w, h);
        if (result.frame() == nullptr) {
            for (const auto& diagnostic : result.diagnostics())
                std::cerr << "diagnostic: " << diagnostic.summary << " / " << diagnostic.detail << "\n";
            for (const auto& [role, value] : values) {
                if (const auto* integer = std::get_if<std::int64_t>(&value)) {
                    std::cerr << "unevaluated plan: " << role << " = " << *integer << "\n";
                } else if (const auto* number = std::get_if<double>(&value)) {
                    std::cerr << "unevaluated plan: " << role << " = " << *number << "\n";
                } else if (const auto* flag = std::get_if<bool>(&value)) {
                    std::cerr << "unevaluated plan: " << role << " = " << *flag << "\n";
                }
            }
        }
        require(result.frame() != nullptr, "the plan evaluates");
        return result;
    };

    // The defaults: Tile of the whole frame, the picture itself.
    {
        const auto same = evaluate({});
        double worst = 0.0;
        for (std::uint32_t j = 0; j < kH; ++j)
            for (std::uint32_t i = 0; i < kW; ++i) {
                const auto got = lensPixel(same.frame()->processImage(), i, j);
                const auto want = lensPixel(source, i, j);
                for (std::size_t c = 0; c < 4; ++c)
                    worst = std::max(worst, static_cast<double>(std::abs(got[c] - want[c])));
            }
        expectations.expect(worst < 1.0e-4, "the default Tile & Mirror leaves the picture as it is");
    }

    // Tile, one value at a time: the size, the centre, each phase.
    for (int which = 0; which < 4; ++which) {
        TileRule rule;
        rule.widthPercent = 30.0;
        rule.heightPercent = 20.0;
        rule.clamp = false;
        std::unordered_map<std::string, ParameterValue> values{
            {"filter", std::int64_t{1}}, {"tileWidth", 30.0}, {"tileHeight", 20.0},
            {"clampToOutput", false}};
        if (which == 1) {
            rule.centre = Vec2d{64.0, -40.0};
            values["tileCenter"] = rule.centre;
        } else if (which == 2) {
            rule.horizontalPhase = 90.0;
            values["horizontalPhase"] = 90.0;
        } else if (which == 3) {
            rule.verticalPhase = 90.0;
            values["verticalPhase"] = 90.0;
        }
        const auto tiled = evaluate(values);
        std::size_t bad = 0;
        static_cast<void>(tileWorst(
            tiled.frame()->processImage(),
            [&](const double x, const double y) {
                const auto at = tileRead(rule, x, y);
                return at ? tileSource(source, (*at)[0], (*at)[1]) : std::array<double, 4>{};
            },
            kW, kH, &bad));
        expectations.expect(bad * 200 <= static_cast<std::size_t>(kW) * kH,
                            which == 0   ? "Tile reads the cell rule for a plain cell"
                            : which == 1 ? "Tile reads the cell rule about a moved centre"
                            : which == 2 ? "Tile reads the cell rule with a horizontal phase"
                                         : "Tile reads the cell rule with a vertical phase");
    }

    // Tile: the cell rule against the independent reading, for a cell of 30% by 20% about a moved
    // centre, with a phase, with and without Mirror Edges, with and without Clamp to Output.
    for (const bool mirror : {false, true})
        for (const bool clamp : {false, true}) {
            TileRule rule;
            rule.centre = Vec2d{64.0, -40.0};
            rule.widthPercent = 30.0;
            rule.heightPercent = 20.0;
            rule.outputWidthPercent = 80.0;
            rule.outputHeightPercent = 60.0;
            rule.horizontalPhase = 90.0;
            rule.verticalPhase = 90.0;
            rule.mirror = mirror;
            rule.clamp = clamp;
            const auto tiled = evaluate({{"filter", std::int64_t{1}},
                                         {"tileCenter", rule.centre},
                                         {"tileWidth", rule.widthPercent},
                                         {"tileHeight", rule.heightPercent},
                                         {"outputWidth", rule.outputWidthPercent},
                                         {"outputHeight", rule.outputHeightPercent},
                                         {"horizontalPhase", rule.horizontalPhase},
                                         {"verticalPhase", rule.verticalPhase},
                                         {"mirrorEdges", mirror},
                                         {"clampToOutput", clamp}});
            if (!mirror && !clamp)
                warpDump("frame-tile", tiled.frame()->processImage(), kW, kH);
            if (mirror && clamp)
                warpDump("frame-tile-mirror-clamped", tiled.frame()->processImage(), kW, kH);
            std::size_t bad = 0;
            const auto worst = tileWorst(
                tiled.frame()->processImage(),
                [&](const double x, const double y) {
                    const auto at = tileRead(rule, x, y);
                    return at ? tileSource(source, (*at)[0], (*at)[1]) : std::array<double, 4>{};
                },
                kW, kH, &bad);
            static_cast<void>(worst);
            expectations.expect(bad * 200 <= static_cast<std::size_t>(kW) * kH,
                                std::string("Tile reads the cell rule") +
                                    (mirror ? " with Mirror Edges" : "") +
                                    (clamp ? " clamped to the output" : " filling the frame"));
        }

    // Tile: a phase of 360 degrees is a whole tile, so the picture is the same as at 0.
    {
        const auto zero = evaluate({{"filter", std::int64_t{1}}, {"tileWidth", 30.0},
                                    {"tileHeight", 20.0}, {"clampToOutput", false}});
        const auto turned = evaluate({{"filter", std::int64_t{1}}, {"tileWidth", 30.0},
                                      {"tileHeight", 20.0}, {"clampToOutput", false},
                                      {"horizontalPhase", 360.0}, {"verticalPhase", -360.0}});
        std::size_t differing = 0;
        for (std::uint32_t j = 0; j < kH; ++j)
            for (std::uint32_t i = 0; i < kW; ++i)
                differing += lensPixel(zero.frame()->processImage(), i, j) !=
                                     lensPixel(turned.frame()->processImage(), i, j)
                                 ? 1U
                                 : 0U;
        expectations.expect(differing * 100 <= static_cast<std::size_t>(kW) * kH,
                            "a phase of 360 degrees is a whole tile");
    }

    // Link Tile Size: the height follows the width.
    {
        const auto linked = evaluate({{"filter", std::int64_t{1}}, {"tileWidth", 40.0},
                                      {"tileHeight", 15.0}, {"linkTileSize", true},
                                      {"clampToOutput", false}});
        const auto square = evaluate({{"filter", std::int64_t{1}}, {"tileWidth", 40.0},
                                      {"tileHeight", 40.0}, {"clampToOutput", false}});
        bool same = true;
        for (std::uint32_t j = 0; j < kH && same; ++j)
            for (std::uint32_t i = 0; i < kW && same; ++i)
                same = lensPixel(linked.frame()->processImage(), i, j) ==
                       lensPixel(square.frame()->processImage(), i, j);
        expectations.expect(same, "a linked tile size takes the width for both");
    }

    // Wrap Offset: a whole-pixel shift is the picture rotated around its window, exactly, with
    // every filter and no seam; a fractional shift (bilinear) blends across the wrap.
    for (const std::int64_t filter : {std::int64_t{0}, std::int64_t{1}, std::int64_t{2}}) {
        const Vec2d shift{200.0, -120.0}; // 25 and 15 proxy pixels
        const auto wrapped = evaluate({{"mode", std::int64_t{1}}, {"filter", filter}, {"offset", shift}});
        double worst = 0.0;
        for (std::uint32_t j = 0; j < kH; ++j)
            for (std::uint32_t i = 0; i < kW; ++i) {
                // The picture moves right by 25 and down by -15 (up by 15) proxy pixels.
                const auto sx = (static_cast<std::int64_t>(i) - 25 + 240 * 4) % 240;
                const auto sy = (static_cast<std::int64_t>(j) + 15 + 135 * 4) % 135;
                const auto got = lensPixel(wrapped.frame()->processImage(), i, j);
                const auto want = lensPixel(source, sx, sy);
                for (std::size_t c = 0; c < 4; ++c)
                    worst = std::max(worst, static_cast<double>(std::abs(got[c] - want[c])));
            }
        expectations.expect(worst < (filter == 2 ? 1.0e-4 : 1.0e-5),
                            filter == 0 ? "a whole-pixel wrap offset is exact and seamless (bilinear)"
                            : filter == 1 ? "a whole-pixel wrap offset is exact and seamless (nearest)"
                                          : "a whole-pixel wrap offset is exact and seamless (bicubic)");
        if (filter == 1)
            warpDump("frame-wrap-offset", wrapped.frame()->processImage(), kW, kH);
    }
    {
        // The window is kept: the data window of the output is the input's.
        const auto wrapped = evaluate({{"mode", std::int64_t{1}}, {"offset", Vec2d{13.0, 7.0}}});
        const auto& window = wrapped.frame()->processImage().descriptor()->dataWindow();
        const auto& inWindow = source.descriptor()->dataWindow();
        expectations.expect(window.extent().width() == inWindow.extent().width() &&
                                window.extent().height() == inWindow.extent().height(),
                            "a wrap offset keeps the input's data window");
    }

    // Mirror: the reflection about a vertical line (the default) and a horizontal one, either
    // side, in the symmetry the node promises, and the kept side is the input.
    for (const double angle : {90.0, 0.0})
        for (const std::int64_t side : {std::int64_t{0}, std::int64_t{1}}) {
            const auto mirrored = evaluate({{"mode", std::int64_t{2}}, {"filter", std::int64_t{1}},
                                            {"mirrorAngle", angle}, {"mirrorSide", side}});
            const auto& image = mirrored.frame()->processImage();
            warpDump(angle == 90.0 ? (side == 0 ? "frame-mirror-left" : "frame-mirror-right")
                                   : (side == 0 ? "frame-mirror-below" : "frame-mirror-above"),
                     image, kW, kH);
            std::size_t asymmetric = 0;
            std::size_t keptChanged = 0;
            for (std::uint32_t j = 0; j < kH; ++j)
                for (std::uint32_t i = 0; i < kW; ++i) {
                    // The mirror pixel across the line through the frame's centre.
                    const auto mi = angle == 90.0 ? kW - 1 - i : i;
                    const auto mj = angle == 90.0 ? j : kH - 1 - j;
                    asymmetric += lensPixel(image, i, j) != lensPixel(image, mi, mj) ? 1U : 0U;
                    // Left or below (screen: smaller x, larger row) is kept for side 0.
                    const bool kept = angle == 90.0 ? (side == 0 ? i < kW / 2 : i >= kW / 2)
                                                    : (side == 0 ? j > kH / 2 : j < kH / 2);
                    const bool onLine = angle != 90.0 && j == kH / 2;
                    if (kept && !onLine && lensPixel(image, i, j) != lensPixel(source, i, j))
                        ++keptChanged;
                }
            expectations.expect(asymmetric == 0,
                                "the mirrored picture is symmetric about the line");
            expectations.expect(keptChanged == 0,
                                "the side that is kept is the input as it was");
        }
    // Mirror about a line through a moved point at 30 degrees, against the independent reflection.
    {
        const Vec2d point{80.0, 40.0};
        const double angle = 30.0;
        const auto mirrored = evaluate({{"mode", std::int64_t{2}}, {"filter", std::int64_t{0}},
                                        {"mirrorAngle", angle}, {"mirrorPoint", point},
                                        {"mirrorSide", std::int64_t{1}}});
        // The line direction: clockwise from screen right, so (cos, -sin) with y up.
        const double dx = std::cos(angle * std::numbers::pi / 180.0);
        const double dy = -std::sin(angle * std::numbers::pi / 180.0);
        const double px = point.x;
        const double py = -point.y;
        std::size_t bad = 0;
        const auto worst = tileWorst(
            mirrored.frame()->processImage(),
            [&](const double x, const double y) {
                // Signed distance from the line; the normal that points to the kept side is the
                // direction turned a quarter turn counter-clockwise (up for a line to the right).
                const double nx = -dy;
                const double ny = dx;
                const double d = (x - px) * nx + (y - py) * ny;
                double sx = x;
                double sy = y;
                if (d < 0.0) { // "Right or above" keeps the positive side of the normal
                    sx = x - 2.0 * d * nx;
                    sy = y - 2.0 * d * ny;
                }
                return lensSample(source, kTileWidth, kTileHeight, sx, sy);
            },
            kW, kH, &bad);
        expectations.expect(worst < 2.0e-2 && bad * 50 <= static_cast<std::size_t>(kW) * kH,
                            "a mirror about a slanted line through a moved point is the reflection");
    }

    // Kaleidoscope: reflection symmetry about the wedge axes and the rotation by a half turn of an
    // even count; against the independent fold for six segments about a moved centre.
    {
        const auto folded = evaluate({{"mode", std::int64_t{3}}, {"filter", std::int64_t{1}},
                                      {"kaleidoscopeSegments", std::int64_t{4}}});
        const auto& image = folded.frame()->processImage();
        warpDump("frame-kaleidoscope", image, kW, kH);
        std::size_t asymmetric = 0;
        for (std::uint32_t j = 0; j < kH; ++j)
            for (std::uint32_t i = 0; i < kW; ++i) {
                // About the frame's centre: the half turn and the reflection in the x axis.
                asymmetric += lensPixel(image, i, j) != lensPixel(image, kW - 1 - i, kH - 1 - j) ? 1U : 0U;
                asymmetric += lensPixel(image, i, j) != lensPixel(image, i, kH - 1 - j) ? 1U : 0U;
            }
        expectations.expect(asymmetric * 100 <= 2 * static_cast<std::size_t>(kW) * kH,
                            "a four-segment kaleidoscope is symmetric about its axes and a half turn");
    }
    {
        const Vec2d centre{48.0, -24.0};
        const double rotation = 20.0;
        const double size = 150.0;
        const std::int64_t count = 6;
        const auto folded = evaluate({{"mode", std::int64_t{3}}, {"filter", std::int64_t{1}},
                                      {"kaleidoscopeSegments", count}, {"kaleidoscopeCenter", centre},
                                      {"kaleidoscopeAngle", rotation}, {"kaleidoscopeSize", size}});
        std::size_t bad = 0;
        const auto worst = tileWorst(
            folded.frame()->processImage(),
            [&](const double x, const double y) {
                // The pixel about the centre, turned counter-clockwise by the rotation, divided by
                // the size, folded into the wedge of 360 / count degrees, and read about the centre.
                const double lx = x - centre.x;
                const double ly = y + centre.y;
                const double a = rotation * std::numbers::pi / 180.0;
                const double rx = (std::cos(a) * lx - std::sin(a) * ly) * 100.0 / size;
                const double ry = (std::sin(a) * lx + std::cos(a) * ly) * 100.0 / size;
                const double turns = std::atan2(ry, rx) / (2.0 * std::numbers::pi);
                const double wedge = 1.0 / static_cast<double>(count);
                const double whole = std::floor(turns / wedge);
                const double rem = turns - whole * wedge;
                const double f = rem > wedge / 2.0 ? wedge - rem : rem;
                const double r = std::hypot(rx, ry);
                return tileSource(source, centre.x + r * std::cos(2.0 * std::numbers::pi * f),
                                  -centre.y + r * std::sin(2.0 * std::numbers::pi * f));
            },
            kW, kH, &bad);
        static_cast<void>(worst);
        expectations.expect(bad * 50 <= static_cast<std::size_t>(kW) * kH,
                            "a six-segment kaleidoscope about a moved centre is the independent fold");
    }

    // A region of interest and the render scale: a region's pixels are the whole frame's; a render at
    // twice the scale reads the same rule (the whole check against the same independent reading).
    {
        const auto plan = tileMirrorPlan({{"mode", std::int64_t{3}}, {"filter", std::int64_t{0}},
                                          {"kaleidoscopeSegments", std::int64_t{5}},
                                          {"kaleidoscopeAngle", 33.0}});
        const auto whole = lensEvaluate(plan, kW, kH);
        const auto region = render::ImageWindow::create(150, 70, 60, 40);
        const auto part = lensEvaluate(plan, kW, kH, *region.value());
        expectations.expect(whole.frame() != nullptr && part.frame() != nullptr,
                            "a region of interest evaluates");
        if (whole.frame() && part.frame()) {
            bool same = true;
            for (auto y = region.value()->originY(); y < region.value()->maxYExclusive(); ++y)
                for (auto x = region.value()->originX(); x < region.value()->maxXExclusive(); ++x)
                    same = same && lensPixel(part.frame()->processImage(), x, y) ==
                                       lensPixel(whole.frame()->processImage(), x, y);
            expectations.expect(same, "a region's pixels are the whole frame's");
        }
        const auto refused = lensEvaluate(plan, kW, kH, std::nullopt, 64);
        expectations.expect(refused.frame() == nullptr, "the pixel budget refuses the raster");
        runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
        const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                       .id = runtime::TaskOwnerId::fromRaw(4863)};
        bool cancelledOutcome = false;
        auto submission = scheduler.submit<void>(
            runtime::TaskRequest("Tile & Mirror cancellation", owner),
            [&](runtime::TaskContext& context) {
                static_cast<void>(scheduler.cancelOwner(owner));
                const auto cancelled = lensEvaluate(plan, kW, kH, std::nullopt,
                                                    std::size_t{1} << 28U, context.cancellation());
                cancelledOutcome = cancelled.frame() == nullptr;
                return runtime::TaskResult<void>::cancelled();
            });
        expectations.expect(submission.accepted(), "the cancellation task is admitted");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::optional<runtime::TaskResult<void>> taken;
        while (!taken && std::chrono::steady_clock::now() < deadline) {
            taken = submission.handle.tryTakeResult();
            std::this_thread::yield();
        }
        expectations.expect(taken && cancelledOutcome, "a cancelled Tile & Mirror publishes no image");
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        expectations.expect(scheduler.isQuiescent(), "the cancelled Tile & Mirror shuts down safely");
    }
    {
        // Half the scale (120 x 68 is not the frame's ratio; 480 x 270 and 240 x 135 are): the Tile
        // rule holds at both.
        TileRule rule;
        rule.widthPercent = 25.0;
        rule.heightPercent = 20.0;
        rule.clamp = false;
        for (const std::uint32_t scale : {1U, 2U}) {
            const auto w = kW * scale;
            const auto h = kH * scale;
            const auto bareAtScale = lensEvaluate(bare, w, h);
            const auto tiled = evaluate({{"filter", std::int64_t{1}}, {"tileWidth", 25.0},
                                         {"tileHeight", 20.0}, {"clampToOutput", false}},
                                        w, h);
            std::size_t bad = 0;
            static_cast<void>(tileWorst(
                tiled.frame()->processImage(),
                [&](const double x, const double y) {
                    const auto at = tileRead(rule, x, y);
                    return at ? tileSource(bareAtScale.frame()->processImage(), (*at)[0], (*at)[1])
                              : std::array<double, 4>{};
                },
                w, h, &bad));
            expectations.expect(bad * 100 <= static_cast<std::size_t>(w) * h,
                                scale == 1 ? "Tile at the proxy scale reads the cell rule"
                                           : "Tile at twice the scale reads the same cell rule");
        }
    }
}

// Cache identity: with one evaluator and its operation cache, the same plan twice is a miss then a hit;
// each value the mode reads, the mode, the filter, the edges, the resolution, the region and the
// picture's own identity make the node miss (the picture upstream still hits); a value the mode does
// not read, or an identical plan, does not.
void testTileMirrorCacheIdentity(Expectations& expectations) {
    using namespace document;
    runtime::CpuCompositionEvaluator evaluator;
    const auto node = NodeId::fromRaw(1300);
    const auto run = [&](const std::unordered_map<std::string, ParameterValue>& values,
                         const std::uint32_t w = 240, const std::uint32_t h = 135,
                         const std::optional<render::ImageWindow> roi = std::nullopt,
                         const bool recolour = false) {
        auto fixture = tileMirrorFixture(values);
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
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the plan compiles");
        const auto extent = render::ImageExtent::create(w, h);
        runtime::EvaluationRequest request{core::RationalTime::fromInteger(0), result.plan->output(),
                                           runtime::ProxyResolution{*extent.value()}};
        request.roi = roi;
        request.pixelStorageByteLimit = std::size_t{1} << 28U;
        runtime::OperationCacheStatistics stats;
        const auto evaluated = evaluator.evaluate(result.plan, request, {}, {}, nullptr, &stats);
        require(evaluated.frame() != nullptr, "the plan evaluates with the cache");
        return stats;
    };
    const auto evaluatedNode = [&](const runtime::OperationCacheStatistics& stats) {
        return std::ranges::find(stats.evaluatedNodes, node) != stats.evaluatedNodes.end();
    };
    using Values = std::unordered_map<std::string, ParameterValue>;
    // Per mode: the base plan, then each change that must miss and each that must not.
    struct Mode final {
        const char* name;
        Values base;
        std::vector<std::pair<std::string, ParameterValue>> misses;
        std::vector<std::pair<std::string, ParameterValue>> hits;
    };
    const std::vector<Mode> modes{
        {"Tile",
         {{"tileWidth", 40.0}, {"tileHeight", 30.0}},
         {{"tileWidth", 41.0}, {"tileHeight", 31.0}, {"tileCenter", Vec2d{8.0, 0.0}},
          {"outputWidth", 90.0}, {"outputHeight", 90.0}, {"horizontalPhase", 10.0},
          {"verticalPhase", 10.0}, {"mirrorEdges", true}, {"clampToOutput", false},
          {"linkTileSize", true}, {"filter", std::int64_t{1}}, {"edges", std::int64_t{2}},
          {"mode", std::int64_t{1}}},
         {{"offset", Vec2d{16.0, 0.0}}, {"mirrorAngle", 33.0}, {"mirrorSide", std::int64_t{1}},
          {"kaleidoscopeAngle", 10.0}, {"kaleidoscopeSegments", std::int64_t{9}}}},
        {"Wrap Offset",
         {{"mode", std::int64_t{1}}, {"offset", Vec2d{8.0, 0.0}}},
         {{"offset", Vec2d{16.0, 8.0}}, {"filter", std::int64_t{1}}},
         {{"edges", std::int64_t{3}}, {"tileWidth", 55.0}, {"horizontalPhase", 20.0},
          {"mirrorAngle", 33.0}, {"kaleidoscopeSize", 120.0}}},
        {"Mirror",
         {{"mode", std::int64_t{2}}},
         {{"mirrorAngle", 33.0}, {"mirrorPoint", Vec2d{8.0, 0.0}}, {"mirrorSide", std::int64_t{1}},
          {"filter", std::int64_t{1}}, {"edges", std::int64_t{2}}},
         {{"offset", Vec2d{16.0, 0.0}}, {"tileWidth", 55.0}, {"kaleidoscopeAngle", 10.0},
          {"kaleidoscopeCenter", Vec2d{8.0, 8.0}}}},
        {"Kaleidoscope",
         {{"mode", std::int64_t{3}}},
         {{"kaleidoscopeAngle", 10.0}, {"kaleidoscopeSize", 120.0},
          {"kaleidoscopeSegments", std::int64_t{9}}, {"kaleidoscopeCenter", Vec2d{8.0, 8.0}},
          {"filter", std::int64_t{1}}, {"edges", std::int64_t{2}}},
         {{"mirrorAngle", 33.0}, {"mirrorPoint", Vec2d{8.0, 0.0}}, {"tileWidth", 55.0},
          {"offset", Vec2d{16.0, 0.0}}}}};
    for (const auto& mode : modes) {
        const std::string name = mode.name;
        const auto cold = run(mode.base);
        expectations.expect(evaluatedNode(cold) && cold.misses > 0, name + ": a first run evaluates the node");
        const auto warm = run(mode.base);
        expectations.expect(warm.misses == 0 && warm.hits > 0 && !evaluatedNode(warm),
                            name + ": the same plan again is served from the cache");
        for (const auto& [role, value] : mode.misses) {
            auto changed = mode.base;
            changed[role] = value;
            const auto stats = run(changed);
            expectations.expect(evaluatedNode(stats) && stats.hits > 0,
                                name + ": a changed " + role + " re-evaluates the node and keeps the picture");
        }
        for (const auto& [role, value] : mode.hits) {
            auto changed = mode.base;
            changed[role] = value;
            const auto stats = run(changed);
            expectations.expect(stats.misses == 0 && !evaluatedNode(stats),
                                name + ": " + role + " is not read by this mode and changes no key");
        }
        expectations.expect(evaluatedNode(run(mode.base, 480, 270)),
                            name + ": another resolution re-evaluates the node");
        const auto region = render::ImageWindow::create(100, 50, 40, 30);
        // A region inside the full frame already cached is served from it (the cache keeps the
        // covering result); the region's pixels are held to the full frame by the ROI test.
        const auto regional = run(mode.base, 240, 135, *region.value());
        expectations.expect(!evaluatedNode(regional),
                            name + ": a region inside the cached frame is served from it");
        const auto recoloured = run(mode.base, 240, 135, std::nullopt, true);
        expectations.expect(evaluatedNode(recoloured),
                            name + ": a different picture re-evaluates the node");
    }
}
