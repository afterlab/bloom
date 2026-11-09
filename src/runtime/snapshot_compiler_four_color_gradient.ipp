// Four-Color Gradient: lowering the source node that fills the composition with four colors
// anchored at four points. Included inside CompilePass, beside snapshot_compiler_fields.ipp, whose
// field program builder it uses.
//
// The node is one field raster, built from the field vocabulary that already has a CPU reference and
// a GPU program (no instruction of its own), over a transparent canvas the size of the composition.
// With q_i the squared distance from the field position to point i in units of the frame's squared
// diagonal, plus a quarter pixel squared so a point's own pixel is finite, the weight of color i is
// w_i = (q_i / q_min)^(-blend / 2), the inverse distance power weight (normalized by the nearest
// point's, so it cannot underflow) with no square root (a device may round a
// square root differently from the CPU; the dot product is exact). The
// straight color is sum(w_i c_i) / sum(w_i) over red, green, blue and alpha, and the raster
// premultiplies it once, as the gradient sources do. Each color is a field Constant whose four
// lanes are its parameter's components; the points are Transform translations of the coordinate.

[[nodiscard]] bool
lowerFourColorGradient(const document::NodeRecord& node,
                       std::vector<runtime::CompiledOperation>& operations,
                       std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using namespace document;
    using core::field::FieldOutputMode;
    using core::field::FieldResultKind;
    std::array<runtime::CompiledVec2Parameter, kFourColorGradientPoints> points;
    std::array<runtime::CompiledColorParameter, kFourColorGradientPoints> colors;
    bool lowered = true;
    for (std::size_t i = 0; i < kFourColorGradientPoints; ++i) {
        const auto point =
            compiledVec2Parameter(findParameterBinding(node, kFourColorGradientPointRoles[i]));
        const auto color =
            compiledColorParameter(findParameterBinding(node, kFourColorGradientColorRoles[i]));
        lowered = lowered && point.has_value() && color.has_value();
        if (point)
            points[i] = *point;
        if (color)
            colors[i] = *color;
    }
    const auto blend = compiledScalarParameter(findParameterBinding(node, "blend"));
    if (!lowered || !blend) {
        addTopologyFailure(node.id, "Four-Color Gradient parameters could not be lowered.");
        return false;
    }
    if (isMuted(node.id)) {
        // A muted source draws nothing.
        const auto index = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledMerge{node.id, {}});
        portOperations_.insert_or_assign({node.id, std::string(kFourColorGradientImagePort)},
                                         index);
        indices.emplace(node.id, index);
        return true;
    }
    const auto& format = composition_->format();
    const auto width = static_cast<double>(format.width());
    const auto height = static_cast<double>(format.height());
    const auto canvas = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(runtime::CompiledSolid{
        node.id,
        runtime::CompiledColorParameter{document::ParameterId{}, core::Color4d{0.0, 0.0, 0.0, 0.0}},
        fieldConstant(width), fieldConstant(height)});

    FieldBuild build;
    build.operations = &operations;
    build.indices = &indices;
    build.coordinate = fieldEmit(build, FieldOp::Coordinate, {-1, -1, -1, -1});
    runtime::detail::RecipeEmitter emitter(node.id, operations, recipeValueSink());
    const auto math = [&](const std::int64_t operation, const int a, const int b) {
        return fieldEmit(build, FieldOp::Math, {a, b, -1, -1}, {operation},
                         {fieldConstant(0.1), fieldConstant(0.0)});
    };
    constexpr std::int64_t kAdd = 0;
    constexpr std::int64_t kMultiply = 2;
    constexpr std::int64_t kDivide = 3;
    constexpr std::int64_t kMin = 4;
    constexpr std::int64_t kPower = 6;
    constexpr std::int64_t kDot = 13;
    const auto constant = [&](const double value) {
        return fieldEmit(build, FieldOp::Constant, {-1, -1, -1, -1}, {},
                         {fieldConstant(value), fieldConstant(value), fieldConstant(value),
                          fieldConstant(value)});
    };
    // Distances are measured in frame diagonals, so a weight stays a normal binary32 number for any
    // point and any blend in the document's domains.
    const double diagonal = std::hypot(width, height);
    const auto inverseDiagonal = constant(1.0 / (diagonal * diagonal));
    const auto halfPixel = constant(0.25 / (diagonal * diagonal));
    // The blend in one instruction (an authored value belongs to one), negated by a swizzle.
    const auto blendOnce =
        fieldEmit(build, FieldOp::Constant, {-1, -1, -1, -1}, {},
                  {*blend, fieldConstant(0.0), fieldConstant(0.0), fieldConstant(0.0)});
    const auto negativeBlend = fieldSwizzle(build, blendOnce, {0, 0, 0, 0}, {-0.5, -0.5, -0.5, -0.5});
    // Each weight is (q_i / q_min)^(-blend / 2): the nearest point weighs 1 and every other at most
    // 1, so the sum never falls below 1 whatever the distances. The unnormalized q_i^(-blend / 2)
    // underflows binary32 when every point is far from the pixel (a small frame, points near the
    // domain's limit, a high blend), which leaves 0 / 0.
    std::array<int, kFourColorGradientPoints> padded{};
    int nearest = -1;
    for (std::size_t i = 0; i < kFourColorGradientPoints; ++i) {
        const auto offset =
            fieldEmit(build, FieldOp::Transform, {build.coordinate, -1, -1, -1}, {0, 6},
                      {fieldConstant(0.0), fieldConstant(0.0)}, {points[i]});
        const auto distance = math(kDot, offset, offset);
        const auto scaled = math(kMultiply, distance, inverseDiagonal);
        padded[i] = math(kAdd, scaled, halfPixel);
        nearest = nearest < 0 ? padded[i] : math(kMin, nearest, padded[i]);
    }
    int weightSum = -1;
    int colorSum = -1;
    for (std::size_t i = 0; i < kFourColorGradientPoints; ++i) {
        const auto relative = math(kDivide, padded[i], nearest);
        const auto weight = math(kPower, relative, negativeBlend);
        const auto weightLanes = fieldSwizzle(build, weight, {0, 0, 0, 0});
        const auto components = emitter.components(colors[i]);
        const auto color =
            fieldEmit(build, FieldOp::Constant, {-1, -1, -1, -1}, {},
                      {components[0], components[1], components[2], components[3]});
        const auto weighted = math(kMultiply, color, weightLanes);
        weightSum = weightSum < 0 ? weightLanes : math(kAdd, weightSum, weightLanes);
        colorSum = colorSum < 0 ? weighted : math(kAdd, colorSum, weighted);
    }
    const auto straight = math(kDivide, colorSum, weightSum);
    // Premultiply once: (r, g, b, a) times (a, a, a, 1).
    const auto alpha = fieldSwizzle(build, straight, {3, 3, 3, 5});
    const auto premultiplied = math(kMultiply, straight, alpha);
    auto program = finishFieldProgram(build, premultiplied, FieldResultKind::Premultiplied,
                                      FieldOutputMode::Raw, node.id);
    if (!program)
        return false;
    const auto last = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(runtime::CompiledImageEffect{
        node.id, canvas, runtime::FieldRasterKernel{std::move(*program), 1U}});
    portOperations_.insert_or_assign({node.id, std::string(kFourColorGradientImagePort)}, last);
    indices.emplace(node.id, last);
    return true;
}
