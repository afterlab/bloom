// Included inside the snapshot compiler test's anonymous namespace after the Rolling Shutter tests,
// whose checker fixture and sampling helpers it reuses.
//
// #348 Corner Pin, ST Map and Displace: each node lowers to one field raster. The tests prove the
// lowering (which operations, which program, which images, time dependence, mute and the cases that
// pass the picture through), then evaluate the plans on the CPU reference over a checker grid: a
// pinned quad against an independent projective map, a map read as data (unclamped, any two channels,
// each edge rule, a position no texel holds), an ST map from Lens Distortion reproducing that node's
// own image, a displacement against an independent reading, HDR and partial alpha kept, the region of
// interest, the pixel budget and cancellation.

// With BLOOM_GRAMMAR_ARTIFACT_DIR set, writes an image as a binary PPM (colour over mid grey, clamped)
// beside the other captures, so an effect can be looked at. Without it, nothing is written.
void warpDump(const std::string_view name, const render::Rgba32fImage& image, const std::uint32_t width,
              const std::uint32_t height) {
    const char* directory = std::getenv("BLOOM_GRAMMAR_ARTIFACT_DIR"); // NOLINT(concurrency-mt-unsafe)
    if (directory == nullptr || *directory == '\0')
        return;
    std::ofstream out(std::string(directory) + "/" + std::string(name) + ".ppm", std::ios::binary);
    out << "P6\n" << width << " " << height << "\n255\n";
    for (std::uint32_t j = 0; j < height; ++j)
        for (std::uint32_t i = 0; i < width; ++i) {
            const auto p = lensPixel(image, i, j);
            for (std::size_t c = 0; c < 3; ++c) {
                const auto v = std::clamp(p[c] + (1.0F - p[3]) * 0.4F, 0.0F, 1.0F);
                out.put(static_cast<char>(static_cast<unsigned char>(v * 255.0F + 0.5F)));
            }
        }
}

// Adds `type` to the fixture's composition with each parameter at its default unless `values` names
// it, under the node id `node` and parameter ids from `first`.
void warpAddNode(document::Project& project, const std::string_view type, const document::NodeId node,
                 const std::uint64_t first,
                 const std::unordered_map<std::string, document::ParameterValue>& values = {}) {
    using namespace document;
    auto* composition = project.findComposition(kCompositionId);
    const auto* definition = builtInNodeDefinitions().find(
        type, type == kSolidSourceNodeType ? kSolidSourceNodeSchemaVersion : 1);
    require(definition != nullptr, "the definition exists");
    std::vector<ParameterBinding> bindings;
    std::uint64_t next = first;
    for (const auto& parameter : definition->parameters) {
        const auto id = ParameterId::fromRaw(next++);
        const auto value = values.find(parameter.role);
        require(composition->parameters().insert(
                    {id, parameter.schemaKey,
                     ConstantValueSource{value == values.end() ? parameter.defaultValue
                                                               : value->second}}),
                "the parameter is valid");
        bindings.push_back({parameter.role, id});
    }
    require(composition->graph().addNode({node, std::string(type), std::move(bindings),
                                          definition->key.schemaVersion}),
            "the node is accepted");
}

// A Solid of `color` over the whole composition feeding `port` of the fixture's node.
void warpSolidMap(LensFixture& fixture, const core::Color4d color, const std::string_view port = "map") {
    using namespace document;
    constexpr auto solid = NodeId::fromRaw(1590);
    auto* composition = fixture.recipe.project.findComposition(kCompositionId);
    warpAddNode(fixture.recipe.project, kSolidSourceNodeType, solid, 1600,
                {{"color", color},
                 {"width", static_cast<double>(composition->format().width())},
                 {"height", static_cast<double>(composition->format().height())}});
    require(composition->graph().addEdge({EdgeId::fromRaw(1590), {solid, "image"},
                                          NodeInputRef{fixture.recipe.node, std::string(port)}}),
            "the solid is the map");
}

// A Lens Distortion of the checker feeding its ST map into `map`.
void warpLensMap(LensFixture& fixture,
                 const std::unordered_map<std::string, document::ParameterValue>& values) {
    using namespace document;
    constexpr auto lens = NodeId::fromRaw(1591);
    auto* composition = fixture.recipe.project.findComposition(kCompositionId);
    warpAddNode(fixture.recipe.project, kLensDistortionNodeType, lens, 1620, values);
    require(composition->graph().addEdge({EdgeId::fromRaw(1591), {fixture.checker, "image"},
                                          NodeInputRef{lens, "input"}}),
            "the checker feeds the lens");
    require(composition->graph().addEdge({EdgeId::fromRaw(1592), {lens, "stMap"},
                                          NodeInputRef{fixture.recipe.node, "map"}}),
            "the lens's ST map is the map");
}

[[nodiscard]] LensFixture
warpFixture(const std::string_view type,
            const std::unordered_map<std::string, document::ParameterValue>& values = {}) {
    return lensFixture(values, "image", true, type);
}

// The input without the node, at a proxy size.
[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
warpBarePlan() {
    auto fixture = warpFixture(document::kStMapNodeType);
    auto result = compileRecipe(std::move(lensFixture({}, "image", false, document::kStMapNodeType)
                                              .recipe.project));
    require(result.plan != nullptr, "the bare checker compiles");
    static_cast<void>(fixture);
    return result.plan;
}

// The premultiplied texel of `image` the field position (x, y) falls in, transparent outside; edge
// 0 clamps, 1 transparent, 2 repeats, 3 mirrors the index.
[[nodiscard]] std::array<double, 4> warpNearest(const render::Rgba32fImage& image,
                                                const double compositionWidth,
                                                const double compositionHeight, const double x,
                                                const double y, const int edge = 1) {
    const auto width = static_cast<std::int64_t>(image.descriptor()->dataWindow().extent().width());
    const auto height = static_cast<std::int64_t>(image.descriptor()->dataWindow().extent().height());
    auto tx = static_cast<std::int64_t>(std::floor((x + compositionWidth / 2.0) *
                                                   (static_cast<double>(width) / compositionWidth)));
    auto ty = static_cast<std::int64_t>(std::floor((compositionHeight / 2.0 - y) *
                                                   (static_cast<double>(height) / compositionHeight)));
    const auto fold = [edge](std::int64_t i, const std::int64_t n) -> std::int64_t {
        const auto floorDiv = [](const std::int64_t a, const std::int64_t b) {
            return a / b - ((a % b != 0 && ((a < 0) != (b < 0))) ? 1 : 0);
        };
        switch (edge) {
        case 0:
            return std::clamp<std::int64_t>(i, 0, n - 1);
        case 1:
            return i < 0 || i >= n ? -1 : i;
        case 2:
            return i - floorDiv(i, n) * n;
        default: {
            const auto period = 2 * n;
            const auto m = i - floorDiv(i, period) * period;
            return m < n ? m : period - 1 - m;
        }
        }
    };
    tx = fold(tx, width);
    ty = fold(ty, height);
    std::array<double, 4> out{};
    if (tx < 0 || ty < 0)
        return out;
    const auto p = lensPixel(image, tx, ty);
    for (std::size_t c = 0; c < 4; ++c)
        out[c] = static_cast<double>(p[c]);
    return out;
}

// The projective map (Heckbert, "Fundamentals of Texture Mapping and Image Warping", 1989) taking
// the unit square to the quad of `corners` (top left, top right, bottom right, bottom left, field
// pixels, y up), and its inverse on a point: the (u, v) of the square that lands on (x, y), u from the
// left and v from the bottom. Independent of the node's solver.
[[nodiscard]] std::array<double, 2> warpUnitSquare(const std::array<double, 8>& corners,
                                                   const double x, const double y) {
    const double x0 = corners[6];
    const double y0 = corners[7];
    const double x1 = corners[4];
    const double y1 = corners[5];
    const double x2 = corners[2];
    const double y2 = corners[3];
    const double x3 = corners[0];
    const double y3 = corners[1];
    const double sx = x0 - x1 + x2 - x3;
    const double sy = y0 - y1 + y2 - y3;
    const double dx1 = x1 - x2;
    const double dx2 = x3 - x2;
    const double dy1 = y1 - y2;
    const double dy2 = y3 - y2;
    const double det = dx1 * dy2 - dx2 * dy1;
    const double g = (sx * dy2 - dx2 * sy) / det;
    const double h = (dx1 * sy - sx * dy1) / det;
    const double a = x1 - x0 + g * x1;
    const double b = x3 - x0 + h * x3;
    const double d = y1 - y0 + g * y1;
    const double e = y3 - y0 + h * y3;
    // The matrix [[a b x0] [d e y0] [g h 1]] and its adjugate (the inverse up to a scale).
    const std::array<double, 9> m{a, b, x0, d, e, y0, g, h, 1.0};
    const std::array<double, 9> inv{
        m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
        m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
        m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3]};
    const double w = inv[6] * x + inv[7] * y + inv[8];
    return {(inv[0] * x + inv[1] * y + inv[2]) / w, (inv[3] * x + inv[4] * y + inv[5]) / w};
}

void testCornerPinLowering(Expectations& expectations) {
    using namespace document;
    using core::field::FieldOp;
    const std::unordered_map<std::string, ParameterValue> pinned{
        {"topLeft", Vec2d{-20.0, 10.0}}, {"topRight", Vec2d{10.0, -15.0}},
        {"bottomRight", Vec2d{-10.0, 20.0}}, {"bottomLeft", Vec2d{25.0, 5.0}},
        {"filter", std::int64_t{1}}};
    {
        auto fixture = warpFixture(kCornerPinNodeType, pinned);
        const auto node = fixture.recipe.node;
        const auto params = fixture.recipe.parameters;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr, "Corner Pin compiles");
        if (!result.plan)
            return;
        const auto ops = recipeOperations(*result.plan, node);
        require(ops.size() == 1, "Corner Pin lowers to one raster");
        const auto* raster = lensRaster(*result.plan, ops[0]);
        require(raster != nullptr, "the raster is a field raster");
        const auto& effect = std::get<runtime::CompiledImageEffect>(result.plan->operations()[ops[0]]);
        const auto checker = lensCheckerOperation(*result.plan);
        expectations.expect(checker && effect.fieldImages.size() == 1 &&
                                effect.fieldImages[0].value() == *checker &&
                                effect.input.value() != *checker,
                            "the raster reads the picture and covers a canvas of its own");
        const auto* canvas = std::get_if<runtime::CompiledSolid>(
            &result.plan->operations()[effect.input.value()]);
        expectations.expect(canvas != nullptr, "the canvas is a transparent composition-size solid");
        expectations.expect(raster->samples == 1 &&
                                raster->program.kind ==
                                    static_cast<std::uint8_t>(
                                        core::field::FieldResultKind::Premultiplied) &&
                                lensInstructions(raster->program, FieldOp::Homography) == 1 &&
                                lensInstructions(raster->program, FieldOp::Image) == 1,
                            "the program is the pin and one premultiplied image read");
        const auto& pin = *std::ranges::find_if(raster->program.instructions, [](const auto& i) {
            return i.op == static_cast<std::uint8_t>(FieldOp::Homography);
        });
        const std::array<const char*, 4> roles{"topLeft", "topRight", "bottomRight", "bottomLeft"};
        bool corners = pin.vectors.size() == 4 && pin.choices == std::vector<std::int64_t>{0} &&
                       pin.slot == 0;
        for (std::size_t i = 0; corners && i < 4; ++i)
            corners = pin.vectors[i].id == params.at(roles[i]) &&
                      std::get<Vec2d>(pin.vectors[i].source) ==
                          std::get<Vec2d>(pinned.at(roles[i]));
        expectations.expect(corners, "the pin owns the four corners, in order");
        const auto& read = *std::ranges::find_if(raster->program.instructions, [](const auto& i) {
            return i.op == static_cast<std::uint8_t>(FieldOp::Image);
        });
        expectations.expect(read.choices == std::vector<std::int64_t>{1, 1, 3, 4, 0, 1},
                            "the picture is read nearest, transparent outside, as stored");
        expectations.expect(!result.plan->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(ops[0])),
                            "a constant Corner Pin does not vary with time");
        expectations.expect(lensSequence(raster->program) ==
                                lensSequence(field_test_programs::cornerPinProgram(1, 1)),
                            "the coverage fixture's program is the lowering's");
    }
    // A muted node is its input; a keyed corner makes the raster vary with time.
    {
        auto fixture = warpFixture(kCornerPinNodeType, pinned);
        fixture.recipe.project.findComposition(kCompositionId)
            ->nodeLayout()[fixture.recipe.node]
            .muted = true;
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr && recipeOperations(*result.plan, node).empty(),
                            "a muted Corner Pin passes its Image through");
    }
    {
        auto fixture = warpFixture(kCornerPinNodeType);
        auto* composition = fixture.recipe.project.findComposition(kCompositionId);
        constexpr auto curveId = AnimationCurveId::fromRaw(1700);
        Vec2AnimationCurve curve{curveId};
        curve.components[0].keyframes = {
            {KeyframeId::fromRaw(1701), core::RationalTime::fromInteger(0), 0.0,
             KeyframeInterpolation::Linear},
            {KeyframeId::fromRaw(1702), core::RationalTime::fromInteger(1), 40.0,
             KeyframeInterpolation::Linear}};
        require(composition->animationCurves().insert(std::move(curve)),
                "the corner's curve is publishable");
        require(composition->parameters().setSource(fixture.recipe.parameters.at("topLeft"),
                                                    AnimationCurveSource{curveId}),
                "a corner animates");
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr, "an animated Corner Pin compiles");
        if (result.plan) {
            const auto ops = recipeOperations(*result.plan, node);
            expectations.expect(ops.size() == 1 && result.plan->operationTimeDependent(
                                                       runtime::OperationIndex::fromRaw(ops[0])),
                                "an animated corner keeps the raster and varies with time");
        }
    }
}

void testCornerPinPixels(Expectations& expectations) {
    using namespace document;
    constexpr double kWidth = 1920.0;
    constexpr double kHeight = 1080.0;
    const auto bare = warpBarePlan();
    const auto input = lensEvaluate(bare, 240, 135);
    require(input.frame() != nullptr, "the bare checker evaluates");
    const auto& source = input.frame()->processImage();

    // The corners moved (pixels, y down): the output against the projective map, pixel by pixel.
    const std::unordered_map<std::string, ParameterValue> offsets{
        {"topLeft", Vec2d{300.0, 120.0}}, {"topRight", Vec2d{-120.0, 200.0}},
        {"bottomRight", Vec2d{-260.0, -150.0}}, {"bottomLeft", Vec2d{180.0, -90.0}}};
    {
        auto fixture = warpFixture(kCornerPinNodeType, offsets);
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the pinned plan compiles");
        const auto pinned = lensEvaluate(result.plan, 240, 135);
        expectations.expect(pinned.frame() != nullptr, "the pinned image evaluates");
        if (pinned.frame()) {
            const auto& image = pinned.frame()->processImage();
            warpDump("frame-corner-pin-source", source, 240, 135);
            warpDump("frame-corner-pin", image, 240, 135);
            const std::array<double, 8> quad{
                -960.0 + 300.0, 540.0 - 120.0, 960.0 - 120.0, 540.0 - 200.0,
                960.0 - 260.0,  -540.0 + 150.0, -960.0 + 180.0, -540.0 + 90.0};
            double worst = 0.0;
            std::size_t inside = 0;
            std::size_t outside = 0;
            for (std::uint32_t j = 0; j < 135; ++j)
                for (std::uint32_t i = 0; i < 240; ++i) {
                    const auto fx = (i + 0.5) * kWidth / 240.0 - kWidth / 2.0;
                    const auto fy = kHeight / 2.0 - (j + 0.5) * kHeight / 135.0;
                    const auto uv = warpUnitSquare(quad, fx, fy);
                    const auto got = lensPixel(image, i, j);
                    if (uv[0] < -0.03 || uv[0] > 1.03 || uv[1] < -0.03 || uv[1] > 1.03) {
                        // Outside the pinned quad the pixel is transparent.
                        if (uv[0] < -0.03 || uv[0] > 1.03 || uv[1] < -0.03 || uv[1] > 1.03) {
                            ++outside;
                            expectations.expect(got[3] == 0.0F && got[0] == 0.0F,
                                                "outside the pinned quad is transparent");
                        }
                        continue;
                    }
                    if (uv[0] < 0.03 || uv[0] > 0.97 || uv[1] < 0.03 || uv[1] > 0.97)
                        continue;
                    ++inside;
                    // The picture's point at (u, v) of its own frame; v from the bottom.
                    const auto at = lensSample(source, kWidth, kHeight, (uv[0] - 0.5) * kWidth,
                                               (uv[1] - 0.5) * kHeight);
                    for (std::size_t c = 0; c < 4; ++c)
                        worst = std::max(worst, std::abs(static_cast<double>(got[c]) - at[c]));
                }
            expectations.expect(inside > 5000 && outside > 3000,
                                "the quad covers a good part of the frame and leaves some");
            expectations.expect(worst < 2.0e-3,
                                "the pinned image is the picture read through the projective map");
        }
    }

    // No offsets: the picture itself, to float rounding.
    {
        auto fixture = warpFixture(kCornerPinNodeType);
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the identity pin compiles");
        const auto pinned = lensEvaluate(result.plan, 240, 135);
        expectations.expect(pinned.frame() != nullptr, "the identity pin evaluates");
        if (pinned.frame()) {
            double worst = 0.0;
            for (std::uint32_t j = 0; j < 135; ++j)
                for (std::uint32_t i = 0; i < 240; ++i) {
                    const auto got = lensPixel(pinned.frame()->processImage(), i, j);
                    const auto want = lensPixel(source, i, j);
                    for (std::size_t c = 0; c < 4; ++c)
                        worst = std::max(worst, static_cast<double>(std::abs(got[c] - want[c])));
                }
            expectations.expect(worst < 1.0e-4, "a pin with no offsets leaves the picture as it is");
        }
    }

    // Nearest takes one texel of the picture exactly, with no blend, HDR and partial alpha kept.
    {
        auto values = offsets;
        values["filter"] = std::int64_t{1};
        auto fixture = warpFixture(kCornerPinNodeType, values);
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the nearest pin compiles");
        const auto pinned = lensEvaluate(result.plan, 240, 135);
        expectations.expect(pinned.frame() != nullptr, "the nearest pin evaluates");
        if (pinned.frame()) {
            const auto& image = pinned.frame()->processImage();
            std::size_t matched = 0;
            std::size_t checked = 0;
            for (std::uint32_t j = 0; j < 135; ++j)
                for (std::uint32_t i = 0; i < 240; ++i) {
                    const auto got = lensPixel(image, i, j);
                    if (got[3] == 0.0F)
                        continue;
                    ++checked;
                    // Every opaque or half transparent output pixel is one of the picture's own
                    // texel values: the two cell colours, premultiplied.
                    const bool cellA = std::abs(got[3] - 0.5F) < 1.0e-6F &&
                                       std::abs(got[0] - 1.0F) < 1.0e-5F &&
                                       std::abs(got[1] - 0.5F) < 1.0e-5F &&
                                       std::abs(got[2] - 0.25F) < 1.0e-5F;
                    const bool cellB = std::abs(got[3] - 1.0F) < 1.0e-6F &&
                                       std::abs(got[0] - 0.1F) < 1.0e-5F &&
                                       std::abs(got[1] - 0.2F) < 1.0e-5F &&
                                       std::abs(got[2] - 0.4F) < 1.0e-5F;
                    matched += (cellA || cellB) ? 1U : 0U;
                }
            expectations.expect(checked > 5000 && matched == checked,
                                "nearest copies whole texels: no pixel is a blend");
        }
    }

    // Two corners together (the quad has no area) and a corner pulled onto the next: transparent.
    {
        auto fixture = warpFixture(kCornerPinNodeType, {{"topRight", Vec2d{-1920.0, 0.0}}});
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the folded pin compiles");
        const auto pinned = lensEvaluate(result.plan, 240, 135);
        bool clear = pinned.frame() != nullptr;
        if (clear)
            for (std::uint32_t j = 0; j < 135 && clear; j += 5)
                for (std::uint32_t i = 0; i < 240 && clear; i += 5)
                    clear = lensPixel(pinned.frame()->processImage(), i, j)[3] == 0.0F;
        expectations.expect(clear, "a quad with two corners together draws nothing");
    }

    // A region of interest reads the whole picture, the budget refuses, a cancelled run publishes
    // nothing.
    {
        auto fixture = warpFixture(kCornerPinNodeType, offsets);
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
        const auto refused = lensEvaluate(result.plan, 240, 135, std::nullopt, 64);
        expectations.expect(refused.frame() == nullptr, "the pixel budget refuses the raster");
        runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
        const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                       .id = runtime::TaskOwnerId::fromRaw(4862)};
        bool cancelledOutcome = false;
        auto submission = scheduler.submit<void>(
            runtime::TaskRequest("Corner Pin cancellation", owner),
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
        expectations.expect(taken && cancelledOutcome, "a cancelled pin publishes no image");
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        expectations.expect(scheduler.isQuiescent(), "the cancelled pin shuts down safely");
    }
}

void testStMapLowering(Expectations& expectations) {
    using namespace document;
    using core::field::FieldOp;
    {
        auto fixture = warpFixture(kStMapNodeType, {{"uChannel", std::int64_t{2}},
                                                    {"vChannel", std::int64_t{3}},
                                                    {"filter", std::int64_t{1}},
                                                    {"edges", std::int64_t{2}}});
        warpSolidMap(fixture, core::Color4d{0.25, 0.5, 0.75, 1.0});
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr, "ST Map compiles");
        if (!result.plan)
            return;
        const auto ops = recipeOperations(*result.plan, node);
        require(ops.size() == 1, "ST Map lowers to one raster");
        const auto* raster = lensRaster(*result.plan, ops[0]);
        require(raster != nullptr, "the raster is a field raster");
        const auto& effect = std::get<runtime::CompiledImageEffect>(result.plan->operations()[ops[0]]);
        const auto checker = lensCheckerOperation(*result.plan);
        expectations.expect(checker && effect.fieldImages.size() == 2 &&
                                effect.fieldImages[0] == effect.input &&
                                effect.fieldImages[1].value() == *checker &&
                                effect.input.value() != *checker,
                            "the raster covers the map and reads the map, then the picture");
        expectations.expect(raster->samples == 1 &&
                                raster->program.kind ==
                                    static_cast<std::uint8_t>(
                                        core::field::FieldResultKind::Premultiplied) &&
                                lensInstructions(raster->program, FieldOp::Image) == 2,
                            "the program is a map read and a picture read");
        std::vector<std::vector<std::int64_t>> reads;
        for (const auto& instruction : raster->program.instructions)
            if (instruction.op == static_cast<std::uint8_t>(FieldOp::Image))
                reads.push_back(instruction.choices);
        expectations.expect(reads.size() == 2 &&
                                reads[0] == std::vector<std::int64_t>{1, 0, 1, 0, 2, 3} &&
                                reads[1] == std::vector<std::int64_t>{1, 2, 3, 4, 0, 1},
                            "the map is read nearest as stored, blue and alpha, and the picture "
                            "with the filter and edge rule chosen");
        expectations.expect(lensSequence(raster->program) ==
                                lensSequence(field_test_programs::stMapProgram(1, 1, 2, 2, 3)),
                            "the coverage fixture's program is the lowering's");
    }
    // No map, or a muted node: the picture itself.
    {
        auto fixture = warpFixture(kStMapNodeType);
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr && recipeOperations(*result.plan, node).empty(),
                            "ST Map with no map passes its Image through");
    }
    {
        auto fixture = warpFixture(kStMapNodeType);
        warpSolidMap(fixture, core::Color4d{0.25, 0.5, 0.75, 1.0});
        fixture.recipe.project.findComposition(kCompositionId)
            ->nodeLayout()[fixture.recipe.node]
            .muted = true;
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr && recipeOperations(*result.plan, node).empty(),
                            "a muted ST Map passes its Image through");
    }
}

void testStMapPixels(Expectations& expectations) {
    using namespace document;
    constexpr double kWidth = 1920.0;
    constexpr double kHeight = 1080.0;
    const auto bare = warpBarePlan();
    const auto input = lensEvaluate(bare, 240, 135);
    require(input.frame() != nullptr, "the bare checker evaluates");
    const auto& source = input.frame()->processImage();

    // Lens Distortion's ST map applied to the checker is Lens Distortion's own image.
    {
        const std::unordered_map<std::string, ParameterValue> bent{
            {"k1", -0.2}, {"k2", 0.05}, {"p1", 0.01}, {"p2", -0.02}, {"center", Vec2d{40.0, -30.0}}};
        auto mapped = warpFixture(kStMapNodeType, {{"filter", std::int64_t{0}}});
        warpLensMap(mapped, bent);
        const auto mappedResult = compileRecipe(std::move(mapped.recipe.project));
        auto lens = lensFixture(bent);
        const auto lensResult = compileRecipe(std::move(lens.recipe.project));
        require(mappedResult.plan != nullptr && lensResult.plan != nullptr, "the plans compile");
        const auto viaMap = lensEvaluate(mappedResult.plan, 240, 135);
        const auto direct = lensEvaluate(lensResult.plan, 240, 135);
        expectations.expect(viaMap.frame() != nullptr && direct.frame() != nullptr,
                            "ST Map and Lens Distortion evaluate");
        if (viaMap.frame() && direct.frame()) {
            double worst = 0.0;
            std::size_t transparent = 0;
            for (std::uint32_t j = 0; j < 135; ++j)
                for (std::uint32_t i = 0; i < 240; ++i) {
                    if (i == 0 && j == 0) {
                        warpDump("frame-st-map-source", source, 240, 135);
                        warpDump("frame-st-map-via-lens-map", viaMap.frame()->processImage(), 240, 135);
                    }
                    const auto a = lensPixel(viaMap.frame()->processImage(), i, j);
                    const auto b = lensPixel(direct.frame()->processImage(), i, j);
                    for (std::size_t c = 0; c < 4; ++c)
                        worst = std::max(worst, static_cast<double>(std::abs(a[c] - b[c])));
                    transparent += b[3] == 0.0F ? 1U : 0U;
                }
            expectations.expect(worst < 2.0e-3,
                                "an ST map applied to the picture is the warp it was made from");
            static_cast<void>(transparent);
        }
    }

    // A constant map of (0.2503, 0.7501) with the channels it names: every pixel reads the nearest
    // texel there. The map is raw: nothing is clamped, so a value past 1 follows the edge rule.
    struct Case final {
        core::Color4d color;
        std::int64_t u;
        std::int64_t v;
        std::int64_t edges;
        const char* name;
    };
    const std::array<Case, 6> cases{{
        {{0.2503, 0.7501, 0.0, 1.0}, 0, 1, 0, "red and green"},
        {{0.9, 0.2503, 0.7501, 1.0}, 1, 2, 0, "green and blue"},
        {{1.5, 0.5, 0.0, 1.0}, 0, 1, 1, "past the frame, transparent"},
        {{1.5, 0.5, 0.0, 1.0}, 0, 1, 0, "past the frame, extended"},
        {{1.5, 0.5, 0.0, 1.0}, 0, 1, 2, "past the frame, repeated"},
        {{1.5, 0.5, 0.0, 1.0}, 0, 1, 3, "past the frame, mirrored"},
    }};
    for (const auto& item : cases) {
        auto fixture = warpFixture(kStMapNodeType, {{"uChannel", item.u},
                                                    {"vChannel", item.v},
                                                    {"filter", std::int64_t{1}},
                                                    {"edges", item.edges}});
        warpSolidMap(fixture, item.color);
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the constant map compiles");
        const auto drawn = lensEvaluate(result.plan, 240, 135);
        expectations.expect(drawn.frame() != nullptr, std::string(item.name) + ": evaluates");
        if (!drawn.frame())
            continue;
        const std::array<double, 4> channels{item.color.red, item.color.green, item.color.blue,
                                             item.color.alpha};
        const auto x = (channels[static_cast<std::size_t>(item.u)] - 0.5) * kWidth;
        const auto y = (channels[static_cast<std::size_t>(item.v)] - 0.5) * kHeight;
        // Edge rule numbers: the node's Transparent, Extend, Repeat, Mirror are the raster's 1, 0, 2, 3.
        const int edge = item.edges == 0 ? 1 : (item.edges == 1 ? 0 : static_cast<int>(item.edges));
        const auto expected = warpNearest(source, kWidth, kHeight, x, y, edge);
        double worst = 0.0;
        for (std::uint32_t j = 0; j < 135; j += 7)
            for (std::uint32_t i = 0; i < 240; i += 7) {
                const auto got = lensPixel(drawn.frame()->processImage(), i, j);
                for (std::size_t c = 0; c < 4; ++c)
                    worst = std::max(worst, std::abs(static_cast<double>(got[c]) - expected[c]));
            }
        expectations.expect(worst < 1.0e-6,
                            std::string(item.name) + ": every pixel is the one texel the map names");
    }

    // A value no texel index holds reads transparent, not the edge pixel, and the node survives it.
    {
        auto fixture = warpFixture(kStMapNodeType, {{"edges", std::int64_t{1}}});
        warpSolidMap(fixture, core::Color4d{3.0e30, 1.0e12, 0.0, 1.0});
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the huge map compiles");
        const auto drawn = lensEvaluate(result.plan, 240, 135);
        expectations.expect(drawn.frame() != nullptr &&
                                lensPixel(drawn.frame()->processImage(), 100, 60) ==
                                    std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F},
                            "a map position beyond any texel index reads transparent");
    }

    // A region of interest, the budget, cancellation.
    {
        auto fixture = warpFixture(kStMapNodeType);
        warpLensMap(fixture, {{"mode", std::int64_t{1}}, {"k1", -0.3}, {"p1", 0.02}});
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
        const auto refused = lensEvaluate(result.plan, 240, 135, std::nullopt, 64);
        expectations.expect(refused.frame() == nullptr, "the pixel budget refuses the raster");
        runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
        const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                       .id = runtime::TaskOwnerId::fromRaw(4863)};
        bool cancelledOutcome = false;
        auto submission = scheduler.submit<void>(
            runtime::TaskRequest("ST Map cancellation", owner),
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
        expectations.expect(taken && cancelledOutcome, "a cancelled ST Map publishes no image");
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        expectations.expect(scheduler.isQuiescent(), "the cancelled ST Map shuts down safely");
    }
}

void testDisplaceLowering(Expectations& expectations) {
    using namespace document;
    using core::field::FieldOp;
    {
        auto fixture = warpFixture(kDisplaceNodeType, {{"xChannel", std::int64_t{3}},
                                                       {"yChannel", std::int64_t{4}},
                                                       {"xAmount", 30.0},
                                                       {"yAmount", -12.0},
                                                       {"midpoint", 0.25},
                                                       {"filter", std::int64_t{2}},
                                                       {"edges", std::int64_t{3}}});
        warpSolidMap(fixture, core::Color4d{0.25, 0.5, 0.75, 1.0});
        const auto node = fixture.recipe.node;
        const auto params = fixture.recipe.parameters;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr, "Displace compiles");
        if (!result.plan)
            return;
        const auto ops = recipeOperations(*result.plan, node);
        require(ops.size() == 1, "Displace lowers to one raster");
        const auto* raster = lensRaster(*result.plan, ops[0]);
        require(raster != nullptr, "the raster is a field raster");
        const auto& effect = std::get<runtime::CompiledImageEffect>(result.plan->operations()[ops[0]]);
        const auto checker = lensCheckerOperation(*result.plan);
        expectations.expect(checker && effect.input.value() == *checker &&
                                effect.fieldImages.size() == 2 &&
                                effect.fieldImages[0].value() == *checker &&
                                effect.fieldImages[1].value() != *checker,
                            "the raster covers the picture and reads it, then the map");
        std::vector<std::vector<std::int64_t>> reads;
        for (const auto& instruction : raster->program.instructions)
            if (instruction.op == static_cast<std::uint8_t>(FieldOp::Image))
                reads.push_back(instruction.choices);
        expectations.expect(reads.size() == 2 &&
                                reads[0] == std::vector<std::int64_t>{0, 0, 1, 0, 2, 3} &&
                                reads[1] == std::vector<std::int64_t>{2, 3, 3, 4, 0, 1},
                            "the map is read bilinear as stored, blue and alpha, and the picture "
                            "with the filter and edge rule chosen");
        // The amounts and the midpoint are the program's operands, each in one instruction.
        bool amounts = false;
        bool midpoint = false;
        for (const auto& instruction : raster->program.instructions)
            if (instruction.op == static_cast<std::uint8_t>(FieldOp::Constant)) {
                if (instruction.scalars.size() == 4 && instruction.scalars[0].id == params.at("xAmount") &&
                    instruction.scalars[1].id == params.at("yAmount"))
                    amounts = std::get<double>(instruction.scalars[0].source) == 30.0 &&
                              std::get<double>(instruction.scalars[1].source) == -12.0;
                if (instruction.scalars.size() == 4 && instruction.scalars[0].id == params.at("midpoint"))
                    midpoint = std::get<double>(instruction.scalars[0].source) == 0.25;
            }
        expectations.expect(amounts && midpoint, "the program owns the amounts and the midpoint");
        expectations.expect(!result.plan->operationTimeDependent(
                                runtime::OperationIndex::fromRaw(ops[0])),
                            "a constant Displace does not vary with time");
        expectations.expect(lensSequence(raster->program) ==
                                lensSequence(field_test_programs::displaceProgram(1, 2, 3, 2, 3)),
                            "the coverage fixture's program is the lowering's");
    }
    // Nothing to move: no map, a muted node, both channels None or both amounts constant zero.
    const std::array<std::pair<std::unordered_map<std::string, ParameterValue>, const char*>, 4> still{{
        {{{"xChannel", std::int64_t{0}}, {"yChannel", std::int64_t{0}}}, "with no channel"},
        {{{"xAmount", 0.0}, {"yAmount", 0.0}}, "with no amount"},
        {{}, "with no map"},
        {{}, "when muted"}}};
    for (std::size_t index = 0; index < still.size(); ++index) {
        auto fixture = warpFixture(kDisplaceNodeType, still[index].first);
        if (index != 2)
            warpSolidMap(fixture, core::Color4d{0.25, 0.5, 0.75, 1.0});
        if (index == 3)
            fixture.recipe.project.findComposition(kCompositionId)
                ->nodeLayout()[fixture.recipe.node]
                .muted = true;
        const auto node = fixture.recipe.node;
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        expectations.expect(result.plan != nullptr && recipeOperations(*result.plan, node).empty(),
                            std::string("Displace passes the picture through ") + still[index].second);
    }
}

void testDisplacePixels(Expectations& expectations) {
    using namespace document;
    constexpr double kWidth = 1920.0;
    constexpr double kHeight = 1080.0;
    const auto bare = warpBarePlan();
    const auto input = lensEvaluate(bare, 240, 135);
    require(input.frame() != nullptr, "the bare checker evaluates");
    const auto& source = input.frame()->processImage();

    struct Case final {
        core::Color4d color;
        std::unordered_map<std::string, ParameterValue> values;
        // The move of the picture in field pixels (y up) this case must make.
        std::array<double, 2> move;
        const char* name;
    };
    const std::array<Case, 4> cases{{
        {{0.75, 0.25, 0.0, 1.0}, {{"xAmount", 80.0}, {"yAmount", 40.0}, {"midpoint", 0.5}},
         {80.0 * 0.25, -40.0 * (0.25 - 0.5)}, "red and green about 0.5"},
        {{0.0, 0.0, 2.0, 1.0},
         {{"xChannel", std::int64_t{3}}, {"yChannel", std::int64_t{0}}, {"xAmount", 12.0},
          {"midpoint", 0.0}},
         {12.0 * 2.0, 0.0}, "an unclamped blue value, no vertical channel"},
        {{0.5, 0.5, 0.0, 1.0}, {{"xAmount", 200.0}, {"yAmount", 200.0}, {"midpoint", 0.5}},
         {0.0, 0.0}, "a map at its midpoint"},
        {{1.0, 0.0, 0.0, 1.0},
         {{"xAmount", -64.0}, {"yAmount", 64.0}, {"midpoint", 0.0}, {"filter", std::int64_t{1}}},
         {-64.0, 0.0 + -64.0 * 0.0}, "negative amounts, nearest"},
    }};
    for (const auto& item : cases) {
        auto fixture = warpFixture(kDisplaceNodeType, item.values);
        warpSolidMap(fixture, item.color);
        const auto result = compileRecipe(std::move(fixture.recipe.project));
        require(result.plan != nullptr, "the displaced plan compiles");
        const auto drawn = lensEvaluate(result.plan, 240, 135);
        expectations.expect(drawn.frame() != nullptr, std::string(item.name) + ": evaluates");
        if (!drawn.frame())
            continue;
        if (&item == &cases.front()) {
            warpDump("frame-displace-source", source, 240, 135);
            warpDump("frame-displace-red-green", drawn.frame()->processImage(), 240, 135);
        }
        const bool nearest = item.values.contains("filter");
        double worst = 0.0;
        for (std::uint32_t j = 0; j < 135; j += 2)
            for (std::uint32_t i = 0; i < 240; i += 2) {
                const auto fx = (i + 0.5) * kWidth / 240.0 - kWidth / 2.0;
                const auto fy = kHeight / 2.0 - (j + 0.5) * kHeight / 135.0;
                // The pixel reads where the picture came from: its own position less the move.
                const auto expected =
                    nearest ? warpNearest(source, kWidth, kHeight, fx - item.move[0], fy - item.move[1])
                            : lensSample(source, kWidth, kHeight, fx - item.move[0], fy - item.move[1]);
                const auto got = lensPixel(drawn.frame()->processImage(), i, j);
                for (std::size_t c = 0; c < 4; ++c)
                    worst = std::max(worst, std::abs(static_cast<double>(got[c]) - expected[c]));
            }
        expectations.expect(worst < 2.0e-4,
                            std::string(item.name) + ": the picture moves by (value - midpoint) "
                                                     "times the amount");
    }

    // A region of interest, the budget, cancellation.
    {
        auto fixture = warpFixture(kDisplaceNodeType, {{"xAmount", 300.0}, {"yAmount", 300.0}});
        warpLensMap(fixture, {{"k1", -0.3}});
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
        const auto refused = lensEvaluate(result.plan, 240, 135, std::nullopt, 64);
        expectations.expect(refused.frame() == nullptr, "the pixel budget refuses the raster");
        runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
        const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                       .id = runtime::TaskOwnerId::fromRaw(4864)};
        bool cancelledOutcome = false;
        auto submission = scheduler.submit<void>(
            runtime::TaskRequest("Displace cancellation", owner),
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
        expectations.expect(taken && cancelledOutcome, "a cancelled Displace publishes no image");
        scheduler.beginShutdown();
        while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        expectations.expect(scheduler.isQuiescent(), "the cancelled Displace shuts down safely");
    }
}
