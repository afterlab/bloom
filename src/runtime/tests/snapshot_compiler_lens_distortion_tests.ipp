// Included inside the snapshot compiler test's anonymous namespace after the camera shake tests.
//
// #486 Lens Distortion: the node lowers to one field raster per output the graph reads. The tests
// prove the lowering (which operations, which program, which operands, time dependence, mute and the
// identity), then evaluate the plans on the CPU reference over a checker grid: the ST map against an
// independent double-precision reading of the Brown-Conrady model in both modes, Redistort as the
// inverse of Undistort, the warped image as the bilinear premultiplied reading of the input at the
// ST map's positions (HDR and partial alpha kept, transparent outside the input), proxy, region of
// interest, the pixel budget and cancellation.

struct LensModel final {
    double k1 = 0.0;
    double k2 = 0.0;
    double p1 = 0.0;
    double p2 = 0.0;
    // The centre as authored: pixels from the composition's centre, y down.
    document::Vec2d centre{};
    bool reverse = false;
};

// The model's distorted position of the normalised (x, y), in double precision.
[[nodiscard]] std::array<double, 2> lensDistort(const LensModel& m, const double x, const double y) {
    const auto r2 = x * x + y * y;
    const auto radial = 1.0 + m.k1 * r2 + m.k2 * r2 * r2;
    return {x * radial + 2.0 * m.p1 * x * y + m.p2 * (r2 + 2.0 * x * x),
            y * radial + m.p1 * (r2 + 2.0 * y * y) + 2.0 * m.p2 * x * y};
}

// The field position (pixels from the composition's centre, y up) the node reads for the field
// position (x, y): the model for Undistort, its inverse (Newton in double precision) for Redistort.
[[nodiscard]] std::array<double, 2> lensSource(const LensModel& m, const double width,
                                               const double height, const double x,
                                               const double y) {
    const auto scale = 0.5 * std::hypot(width, height);
    const auto cx = m.centre.x;
    const auto cy = -m.centre.y;
    const auto nx = (x - cx) / scale;
    const auto ny = (y - cy) / scale;
    if (!m.reverse) {
        const auto d = lensDistort(m, nx, ny);
        return {cx + d[0] * scale, cy + d[1] * scale};
    }
    double ux = nx;
    double uy = ny;
    for (int i = 0; i < 60; ++i) {
        const auto d = lensDistort(m, ux, uy);
        const double h = 1.0e-6;
        const auto dx = lensDistort(m, ux + h, uy);
        const auto dy = lensDistort(m, ux, uy + h);
        const double a = (dx[0] - d[0]) / h;
        const double b = (dy[0] - d[0]) / h;
        const double c = (dx[1] - d[1]) / h;
        const double e = (dy[1] - d[1]) / h;
        const double det = a * e - b * c;
        if (std::abs(det) < 1.0e-12)
            break;
        const double fx = d[0] - nx;
        const double fy = d[1] - ny;
        ux -= (e * fx - b * fy) / det;
        uy -= (a * fy - c * fx) / det;
    }
    return {cx + ux * scale, cy + uy * scale};
}

struct LensFixture final {
    RecipeFixture recipe;
    document::NodeId checker;
};

// A checker grid (HDR, one colour partly transparent) -> Lens Distortion -> Layer, the checker the
// size of the composition.
[[nodiscard]] LensFixture
lensFixture(const std::unordered_map<std::string, document::ParameterValue>& values = {},
            const std::string_view outputPort = "image", const bool throughLens = true,
            const std::string_view nodeType = document::kLensDistortionNodeType) {
    using namespace document;
    LensFixture fixture{recipeFixture(nodeType, values, {}, outputPort, true),
                        NodeId::fromRaw(1380)};
    auto* composition = fixture.recipe.project.findComposition(kCompositionId);
    auto& graph = composition->graph();
    const auto* definition =
        builtInNodeDefinitions().find(kCheckerSourceNodeType, kCheckerSourceNodeSchemaVersion);
    require(definition != nullptr, "the checker definition exists");
    std::vector<ParameterBinding> bindings;
    std::uint64_t next = 1380;
    for (const auto& parameter : definition->parameters) {
        const auto id = ParameterId::fromRaw(next++);
        ParameterValue value = parameter.defaultValue;
        if (parameter.role == "colorA")
            value = core::Color4d{2.0, 1.0, 0.5, 0.5};
        if (parameter.role == "colorB")
            value = core::Color4d{0.1, 0.2, 0.4, 1.0};
        if (parameter.role == "cellWidth" || parameter.role == "cellHeight")
            value = std::int64_t{64};
        if (parameter.role == "width")
            value = static_cast<std::int64_t>(composition->format().width());
        if (parameter.role == "height")
            value = static_cast<std::int64_t>(composition->format().height());
        require(composition->parameters().insert({id, parameter.schemaKey, ConstantValueSource{value}}),
                "checker parameter is valid");
        bindings.push_back({parameter.role, id});
    }
    require(graph.addNode({fixture.checker, std::string(kCheckerSourceNodeType), std::move(bindings),
                           definition->key.schemaVersion}),
            "the checker is accepted");
    require(graph.eraseEdge(kFirstSourceEdge), "the solid's edge is removed");
    require(graph.addEdge({kFirstSourceEdge, {fixture.checker, "image"},
                           NodeInputRef{fixture.recipe.node, "input"}}),
            "the checker feeds the lens");
    // The layer is unmoved and opaque, so the composition is the content's own pixels.
    require(composition->parameters().setSource(kFirstPosition, ConstantValueSource{Vec2d{static_cast<double>(composition->format().width()) / 2.0,
                                                          static_cast<double>(composition->format().height()) / 2.0}}),
            "the layer is centred");
    require(composition->parameters().setSource(kFirstOpacity, ConstantValueSource{1.0}),
            "the layer is opaque");
    if (!throughLens) {
        require(graph.eraseEdge(EdgeId::fromRaw(1300)), "the lens is bypassed");
        require(graph.addEdge({EdgeId::fromRaw(1399), {fixture.checker, "image"},
                               NodeInputRef{kFirstLayerNode,
                                            std::string(kLayerOutputContentInputPort)}}),
                "the checker feeds the layer");
    }
    return fixture;
}

[[nodiscard]] std::optional<std::size_t>
lensCheckerOperation(const runtime::CompiledCompositionPlan& plan) {
    for (std::size_t index = 0; index < plan.operations().size(); ++index)
        if (std::holds_alternative<runtime::CompiledChecker>(plan.operations()[index]))
            return index;
    return std::nullopt;
}

[[nodiscard]] const runtime::FieldRasterKernel*
lensRaster(const runtime::CompiledCompositionPlan& plan, const std::size_t index) {
    return recipeKernel<runtime::FieldRasterKernel>(plan, index);
}

[[nodiscard]] std::size_t lensInstructions(const runtime::FieldProgram& program,
                                           const core::field::FieldOp op) {
    return static_cast<std::size_t>(std::ranges::count_if(
        program.instructions, [op](const auto& instruction) {
            return instruction.op == static_cast<std::uint8_t>(op);
        }));
}

// The picture of the plan's Composition Output at a proxy size: the layer is unmoved and opaque, so
// it is the content's own pixels.
[[nodiscard]] runtime::EvaluationResult
lensEvaluate(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
             const std::uint32_t width, const std::uint32_t height,
             const std::optional<render::ImageWindow> roi = std::nullopt,
             const std::size_t byteLimit = std::size_t{1} << 28U,
             const runtime::CancellationToken& cancellation = {}) {
    const auto extent = render::ImageExtent::create(width, height);
    runtime::EvaluationRequest request{core::RationalTime::fromInteger(0), plan->output(),
                                       runtime::ProxyResolution{*extent.value()}};
    request.pixelStorageByteLimit = byteLimit;
    request.bypassOperationCache = true;
    request.roi = roi;
    const runtime::CpuCompositionEvaluator evaluator;
    return evaluator.evaluate(plan, request, cancellation);
}

[[nodiscard]] std::array<float, 4> lensPixel(const render::Rgba32fImage& image,
                                             const std::int64_t x, const std::int64_t y) {
    std::array<float, 4> value{};
    if (const auto pixel = image.read(x, y))
        std::ranges::copy(pixel.value()->components(), value.begin());
    return value;
}

// The input read bilinearly over premultiplied texels at the field position (x, y), transparent
// outside it, as an independent reading of the Image instruction.
[[nodiscard]] std::array<double, 4> lensSample(const render::Rgba32fImage& image,
                                               const double compositionWidth,
                                               const double compositionHeight, const double x,
                                               const double y) {
    const auto width = static_cast<double>(image.descriptor()->dataWindow().extent().width());
    const auto height = static_cast<double>(image.descriptor()->dataWindow().extent().height());
    const auto u = (x + compositionWidth / 2.0) * (width / compositionWidth) - 0.5;
    const auto v = (compositionHeight / 2.0 - y) * (height / compositionHeight) - 0.5;
    const auto iu = std::floor(u);
    const auto iv = std::floor(v);
    const auto tu = u - iu;
    const auto tv = v - iv;
    const auto tap = [&](const double dx, const double dy) {
        const auto tx = static_cast<std::int64_t>(iu + dx);
        const auto ty = static_cast<std::int64_t>(iv + dy);
        std::array<double, 4> value{};
        if (tx < 0 || ty < 0 || tx >= static_cast<std::int64_t>(width) ||
            ty >= static_cast<std::int64_t>(height))
            return value;
        const auto p = lensPixel(image, tx, ty);
        for (std::size_t c = 0; c < 4; ++c)
            value[c] = static_cast<double>(p[c]);
        return value;
    };
    const auto a = tap(0, 0);
    const auto b = tap(1, 0);
    const auto c = tap(0, 1);
    const auto d = tap(1, 1);
    std::array<double, 4> out{};
    for (std::size_t k = 0; k < 4; ++k) {
        const auto top = a[k] + tu * (b[k] - a[k]);
        const auto bottom = c[k] + tu * (d[k] - c[k]);
        out[k] = top + tv * (bottom - top);
    }
    return out;
}

// The instruction sequence of a program: each operation with its constant choices.
[[nodiscard]] std::vector<std::pair<std::uint8_t, std::vector<std::int64_t>>>
lensSequence(const runtime::FieldProgram& program) {
    std::vector<std::pair<std::uint8_t, std::vector<std::int64_t>>> sequence;
    for (const auto& instruction : program.instructions)
        sequence.emplace_back(instruction.op, instruction.choices);
    return sequence;
}

void testLensDistortionLowering(Expectations& expectations) {
    using namespace document;
    using core::field::FieldOp;
    const std::unordered_map<std::string, ParameterValue> bent{
        {"mode", std::int64_t{1}}, {"k1", -0.2}, {"k2", 0.05}, {"p1", 0.01}, {"p2", -0.02},
        {"center", Vec2d{40.0, -30.0}}};

    // The image output: one raster reading the input through the warp, the model's operands on it.
    {
        auto fixture = lensFixture(bent);
        const auto node = fixture.recipe.node;
        const auto params = fixture.recipe.parameters;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr, "Lens Distortion compiles");
        if (!result.plan)
            return;
        const auto ops = recipeOperations(*result.plan, node);
        require(ops.size() == 1, "an image output alone lowers to one raster");
        const auto* raster = lensRaster(*result.plan, ops[0]);
        require(raster != nullptr, "the raster is a field raster");
        const auto& effect = std::get<runtime::CompiledImageEffect>(result.plan->operations()[ops[0]]);
        const auto checker = lensCheckerOperation(*result.plan);
        expectations.expect(checker && effect.input.value() == *checker &&
                                effect.fieldImages.size() == 1 &&
                                effect.fieldImages[0].value() == *checker,
                            "the raster covers the input and reads it");
        expectations.expect(raster->samples == 1 &&
                                raster->program.kind ==
                                    static_cast<std::uint8_t>(
                                        core::field::FieldResultKind::Premultiplied) &&
                                lensInstructions(raster->program, FieldOp::LensWarp) == 1 &&
                                lensInstructions(raster->program, FieldOp::Image) == 1,
                            "the program is the warp and one premultiplied image read");
        const auto& warp = *std::ranges::find_if(raster->program.instructions, [](const auto& i) {
            return i.op == static_cast<std::uint8_t>(FieldOp::LensWarp);
        });
        expectations.expect(warp.choices == std::vector<std::int64_t>{1} &&
                                warp.scalars.size() == 5 && warp.vectors.size() == 1 &&
                                warp.scalars[0].id == params.at("k1") &&
                                warp.scalars[1].id == params.at("k2") &&
                                warp.scalars[2].id == params.at("p1") &&
                                warp.scalars[3].id == params.at("p2") &&
                                std::get<double>(warp.scalars[0].source) == -0.2 &&
                                std::get<double>(warp.scalars[4].source) ==
                                    0.5 * std::hypot(1920.0, 1080.0) &&
                                warp.vectors[0].id == params.at("center") &&
                                std::get<Vec2d>(warp.vectors[0].source) == Vec2d{40.0, -30.0},
                            "the warp owns the model's operands and the mode");
        expectations.expect(!result.plan->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(ops[0])),
                            "a constant Lens Distortion does not vary with time");
        expectations.expect(lensSequence(raster->program) ==
                                lensSequence(field_test_programs::lensDistortionProgram(1, false, true)),
                            "the coverage fixture's image program is the lowering's");
    }

    // The ST map output alone: one raster over the input's window reading no image.
    {
        auto fixture = lensFixture(bent, "stMap");
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr, "the ST map output compiles");
        if (!result.plan)
            return;
        const auto ops = recipeOperations(*result.plan, node);
        require(ops.size() == 1, "an ST map output alone lowers to one raster");
        const auto* raster = lensRaster(*result.plan, ops[0]);
        const auto& effect = std::get<runtime::CompiledImageEffect>(result.plan->operations()[ops[0]]);
        expectations.expect(raster != nullptr && effect.fieldImages.empty() &&
                                raster->program.kind ==
                                    static_cast<std::uint8_t>(core::field::FieldResultKind::Vector) &&
                                lensInstructions(raster->program, FieldOp::LensWarp) == 1 &&
                                lensInstructions(raster->program, FieldOp::Image) == 0,
                            "the ST map draws the warp's position and reads no image");
        expectations.expect(raster != nullptr &&
                                lensSequence(raster->program) ==
                                    lensSequence(field_test_programs::lensDistortionProgram(1, true, true)),
                            "the coverage fixture's ST map program is the lowering's");
    }

    // All coefficients constant zero: the image is the input itself and the node adds nothing; the
    // ST map is still the (identity) warp. A muted node is the same.
    for (const bool muted : {false, true}) {
        auto fixture = lensFixture(muted ? bent : std::unordered_map<std::string, ParameterValue>{});
        if (muted)
            fixture.recipe.project.findComposition(kCompositionId)
                ->nodeLayout()[fixture.recipe.node]
                .muted = true;
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr &&
                                recipeOperations(*result.plan, node).empty(),
                            muted ? "a muted Lens Distortion passes its Image through"
                                  : "zero coefficients pass the Image through");
    }
    {
        auto fixture = lensFixture(bent, "stMap");
        fixture.recipe.project.findComposition(kCompositionId)
            ->nodeLayout()[fixture.recipe.node]
            .muted = true;
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr &&
                                recipeOperations(*result.plan, node).size() == 1,
                            "a muted Lens Distortion still draws an (identity) ST map");
        if (result.plan) {
            const auto ops = recipeOperations(*result.plan, node);
            const auto evaluated = lensEvaluate(result.plan, 240, 135);
            expectations.expect(evaluated.frame() != nullptr, "the identity ST map evaluates");
            if (evaluated.frame()) {
                const auto& image = evaluated.frame()->processImage();
                const auto p = lensPixel(image, 60, 30);
                expectations.expect(std::abs(p[0] - (60.5F / 240.0F)) < 1.0e-5F &&
                                        std::abs(p[1] - (1.0F - 30.5F / 135.0F)) < 1.0e-5F &&
                                        p[2] == 0.0F && p[3] == 1.0F,
                                    "a muted node's ST map is the pixel's own position");
            }
        }
    }

    // A keyed coefficient keeps the raster even at zero and makes the plan time dependent.
    {
        auto fixture = lensFixture();
        auto* composition = fixture.recipe.project.findComposition(kCompositionId);
        constexpr auto curveId = AnimationCurveId::fromRaw(1400);
        require(composition->animationCurves().insert(ScalarAnimationCurve{
                    curveId,
                    {{KeyframeId::fromRaw(1401), core::RationalTime::fromInteger(0), 0.0,
                      KeyframeInterpolation::Linear},
                     {KeyframeId::fromRaw(1402), core::RationalTime::fromInteger(1), -0.3,
                      KeyframeInterpolation::Linear}}}),
                "k1 curve is publishable");
        require(composition->parameters().setSource(fixture.recipe.parameters.at("k1"),
                                                    AnimationCurveSource{curveId}),
                "k1 animates");
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr, "an animated Lens Distortion compiles");
        if (result.plan) {
            const auto ops = recipeOperations(*result.plan, node);
            expectations.expect(ops.size() == 1 && result.plan->operationTimeDependent(
                                                       runtime::OperationIndex::fromRaw(ops[0])),
                                "an animated coefficient keeps the raster and varies with time");
        }
    }
}

void testLensDistortionPixels(Expectations& expectations) {
    using namespace document;
    constexpr double kWidth = 1920.0;
    constexpr double kHeight = 1080.0;
    struct Case final {
        LensModel model;
        const char* name;
    };
    const std::array<Case, 4> cases{{
        {{-0.2, 0.05, 0.01, -0.02, Vec2d{40.0, -30.0}, false}, "Undistort, barrel with tangential"},
        {{-0.2, 0.05, 0.01, -0.02, Vec2d{40.0, -30.0}, true}, "Redistort, barrel with tangential"},
        {{0.3, -0.04, 0.0, 0.0, Vec2d{0.0, 0.0}, false}, "Undistort, pincushion"},
        {{0.3, -0.04, 0.0, 0.0, Vec2d{0.0, 0.0}, true}, "Redistort, pincushion"},
    }};
    for (const auto& item : cases) {
        const auto& m = item.model;
        const std::unordered_map<std::string, ParameterValue> values{
            {"mode", std::int64_t{m.reverse ? 1 : 0}}, {"k1", m.k1}, {"k2", m.k2}, {"p1", m.p1},
            {"p2", m.p2}, {"center", m.centre}};
        auto mapFixture = lensFixture(values, "stMap");
        const auto mapNode = mapFixture.recipe.node;
        const auto mapResult = compileRecipe(std::move(mapFixture.recipe.project));
        auto imageFixture = lensFixture(values);
        const auto imageNode = imageFixture.recipe.node;
        const auto imageResult = compileRecipe(std::move(imageFixture.recipe.project));
        if (!mapResult.plan || !imageResult.plan) {
            expectations.expect(false, "the lens plans compile");
            continue;
        }
        static_cast<void>(mapNode);
        static_cast<void>(imageNode);

        // The ST map at two proxy sizes: every pixel is the model's source position over the frame.
        for (const std::uint32_t width : {240U, 120U}) {
            const std::uint32_t height = width * 9U / 16U;
            const auto evaluated = lensEvaluate(mapResult.plan, width, height);
            expectations.expect(evaluated.frame() != nullptr,
                                std::string(item.name) + ": the ST map evaluates");
            if (!evaluated.frame())
                continue;
            const auto& map = evaluated.frame()->processImage();
            double worst = 0.0;
            double inverseError = 0.0;
            for (std::uint32_t j = 0; j < height; j += 3)
                for (std::uint32_t i = 0; i < width; i += 3) {
                    const auto fx = (i + 0.5) * kWidth / width - kWidth / 2.0;
                    const auto fy = kHeight / 2.0 - (j + 0.5) * kHeight / height;
                    const auto source = lensSource(m, kWidth, kHeight, fx, fy);
                    const auto p = lensPixel(map, i, j);
                    worst = std::max({worst,
                                      std::abs(static_cast<double>(p[0]) - (source[0] / kWidth + 0.5)),
                                      std::abs(static_cast<double>(p[1]) - (source[1] / kHeight + 0.5))});
                    expectations.expect(p[2] == 0.0F && p[3] == 1.0F,
                                        std::string(item.name) + ": the ST map's blue is 0 and alpha 1");
                    if (m.reverse) {
                        // Redistort undoes Undistort: the forward model at the position it reads
                        // is the pixel's own.
                        LensModel forward = m;
                        forward.reverse = false;
                        const auto sx = (static_cast<double>(p[0]) - 0.5) * kWidth;
                        const auto sy = (static_cast<double>(p[1]) - 0.5) * kHeight;
                        const auto back = lensSource(forward, kWidth, kHeight, sx, sy);
                        inverseError = std::max({inverseError, std::abs(back[0] - fx),
                                                 std::abs(back[1] - fy)});
                    }
                }
            expectations.expect(worst < 2.0e-5,
                                std::string(item.name) +
                                    ": the ST map is the model's position over the frame");
            if (m.reverse)
                expectations.expect(inverseError < 0.25,
                                    std::string(item.name) +
                                        ": Redistort is the inverse of Undistort (within a quarter "
                                        "of a pixel at 1920 across)");
        }

        // The image: the input read at the ST map's positions, bilinearly over premultiplied texels.
        auto bareFixture = lensFixture(values, "image", false);
        const auto bareResult = compileRecipe(std::move(bareFixture.recipe.project));
        require(bareResult.plan != nullptr, "the bare checker compiles");
        const auto warped = lensEvaluate(imageResult.plan, 240, 135);
        const auto input = lensEvaluate(bareResult.plan, 240, 135);
        expectations.expect(warped.frame() != nullptr && input.frame() != nullptr,
                            std::string(item.name) + ": the warped image evaluates");
        if (!warped.frame() || !input.frame())
            continue;
        const auto& image = warped.frame()->processImage();
        const auto& source = input.frame()->processImage();
        double worst = 0.0;
        std::size_t transparent = 0;
        for (std::uint32_t j = 0; j < 135; j += 2)
            for (std::uint32_t i = 0; i < 240; i += 2) {
                const auto fx = (i + 0.5) * kWidth / 240.0 - kWidth / 2.0;
                const auto fy = kHeight / 2.0 - (j + 0.5) * kHeight / 135.0;
                const auto at = lensSource(m, kWidth, kHeight, fx, fy);
                const auto expected = lensSample(source, kWidth, kHeight, at[0], at[1]);
                const auto got = lensPixel(image, i, j);
                for (std::size_t c = 0; c < 4; ++c)
                    worst = std::max(worst, std::abs(static_cast<double>(got[c]) - expected[c]));
                transparent += got[3] == 0.0F ? 1U : 0U;
            }
        expectations.expect(worst < 2.0e-4,
                            std::string(item.name) +
                                ": the image is the input read at the warp's positions");
        if (m.reverse && m.k1 < 0.0)
            expectations.expect(transparent > 0,
                                std::string(item.name) + ": outside the input is transparent");
    }

    // A small barrel leaves the middle of a cell whole: HDR colour and partial alpha as stored.
    {
        auto fixture = lensFixture({{"k1", -0.02}});
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the small barrel compiles");
        const auto evaluated = lensEvaluate(result.plan, 240, 135);
        require(evaluated.frame() != nullptr, "the small barrel evaluates");
        const auto& image = evaluated.frame()->processImage();
        // Pixel (116, 64) sits inside one 8 px cell of the grid, next to the frame's centre.
        std::size_t halfAlpha = 0;
        for (std::uint32_t j = 60; j < 70; ++j)
            for (std::uint32_t i = 110; i < 126; ++i) {
                const auto p = lensPixel(image, i, j);
                if (std::abs(p[3] - 0.5F) < 1.0e-5F) {
                    ++halfAlpha;
                    // 2.0, 1.0, 0.5 straight at alpha 0.5 are 1.0, 0.5, 0.25 premultiplied.
                    expectations.expect(std::abs(p[0] - 1.0F) < 1.0e-4F &&
                                            std::abs(p[1] - 0.5F) < 1.0e-4F &&
                                            std::abs(p[2] - 0.25F) < 1.0e-4F,
                                        "a partly transparent HDR cell keeps its premultiplied colour");
                }
            }
        expectations.expect(halfAlpha > 0, "the small barrel keeps whole cells of the grid");
    }

    // A region of interest reads the whole frame's pixels, wherever the warp reads from.
    {
        auto fixture = lensFixture({{"mode", std::int64_t{1}}, {"k1", -0.3}, {"p1", 0.02}});
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the regional fixture compiles");
        const auto whole = lensEvaluate(result.plan, 240, 135);
        const auto region = render::ImageWindow::create(150, 70, 60, 40);
        const auto part = lensEvaluate(result.plan, 240, 135, *region.value());
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

        // The pixel budget refuses the output, and a cancelled evaluation publishes nothing.
        const auto refused = lensEvaluate(result.plan, 240, 135, std::nullopt, 64);
        expectations.expect(refused.frame() == nullptr, "the pixel budget refuses the raster");
        runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
        const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                       .id = runtime::TaskOwnerId::fromRaw(4861)};
        bool cancelledOutcome = false;
        auto submission = scheduler.submit<void>(
            runtime::TaskRequest("Lens Distortion cancellation", owner),
            [&](runtime::TaskContext& context) {
                static_cast<void>(scheduler.cancelOwner(owner));
                const auto cancelled = lensEvaluate(result.plan, 240, 135, std::nullopt,
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
        expectations.expect(taken && cancelledOutcome, "a cancelled lens warp publishes no image");
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        expectations.expect(scheduler.isQuiescent(), "the cancelled lens warp shuts down safely");
    }
}
