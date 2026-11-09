// #348 Corner Pin, ST Map and Displace: lowering the three inverse-mapping nodes. Included inside
// CompilePass, beside snapshot_compiler_fields.ipp, whose field program builder they use.
//
// Each is one field raster: the output pixel computes the field position to read the input at, and
// the program's Image instruction reads it (the filtered premultiplied texel as stored, so values
// above one, negative values and partial alpha keep their range and their ratio, and nothing is
// divided by alpha). A map is read the same way, as stored: the channel pair of the texel, unclamped
// and with no colour conversion.
//
//   Corner Pin  the Homography instruction pins the input's data window to the quad of the four
//               moved corners. The canvas is the composition frame, since the quad may leave the
//               input's window.
//   ST Map      the canvas is the map. The map is read at the pixel, nearest, two of its channels
//               are the position as a fraction of the composition frame, which is scaled to field
//               pixels and read from the input.
//   Displace    the canvas is the input. The map is read at the pixel (bilinear, so a map of another
//               size is resampled), (value - midpoint) times the amount is the picture's move, and
//               the pixel reads the input where that move came from.
//
// A node with nothing to apply (muted, no map, nothing to move) is its input.

// The field raster's edge rule for the node's choice (Transparent, Extend, Repeat, Mirror).
[[nodiscard]] static std::int64_t warpEdge(const std::int64_t choice) {
    switch (choice) {
    case document::kWarpEdgeExtend:
        return 0;
    case document::kWarpEdgeRepeat:
        return 2;
    case document::kWarpEdgeMirror:
        return 3;
    default:
        return 1;
    }
}

// An image read by the program: its slot, with the four-image bound checked. -2 with a failure.
[[nodiscard]] std::int8_t warpSlot(FieldBuild& build, const document::NodeRecord& node,
                                   const runtime::OperationIndex image) {
    if (build.images.size() >= core::field::kMaxFieldImages) {
        addTopologyFailure(node.id, "A field program reads at most four images.");
        return -2;
    }
    build.images.push_back(image);
    return static_cast<std::int8_t>(build.images.size() - 1);
}

// One Image instruction reading slot `slot` at the position in register `where`; `choices` are the
// raster's [filter, edge, selector, channel, first, second].
int warpRead(FieldBuild& build, const int where, const std::int8_t slot,
             std::vector<std::int64_t> choices) {
    FieldBuild::Step step;
    step.instruction.op = static_cast<std::uint8_t>(FieldOp::Image);
    step.instruction.choices = std::move(choices);
    step.instruction.slot = slot;
    step.output = build.nextRegister++;
    step.inputs = {where, -1, -1, -1};
    build.steps.push_back(std::move(step));
    return build.steps.back().output;
}

// The field raster of `build` over `canvas`, published as the node's image.
[[nodiscard]] bool
finishWarp(const document::NodeRecord& node, FieldBuild& build, const int result,
           const runtime::OperationIndex canvas, std::vector<runtime::CompiledOperation>& operations,
           std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    auto program = finishFieldProgram(build, result, core::field::FieldResultKind::Premultiplied,
                                      core::field::FieldOutputMode::Raw, node.id);
    if (!program)
        return false;
    runtime::CompiledImageEffect effect{node.id, canvas,
                                        runtime::FieldRasterKernel{std::move(*program), 1}};
    effect.fieldImages = std::move(build.images);
    const auto index = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(std::move(effect));
    portOperations_.insert_or_assign({node.id, std::string(document::kWarpImagePort)}, index);
    indices.emplace(node.id, index);
    return true;
}

void passWarpThrough(const document::NodeRecord& node, const runtime::OperationIndex input,
                     std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    portOperations_.insert_or_assign({node.id, std::string(document::kWarpImagePort)}, input);
    indices.emplace(node.id, input);
}

[[nodiscard]] bool
lowerCornerPin(const document::NodeRecord& node, std::vector<runtime::CompiledOperation>& operations,
               std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using namespace document;
    const auto input = findInputOperation(node.id, "input", indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return false;
    }
    const auto* filter = parameterConstant<std::int64_t>(findParameterBinding(node, "filter"));
    std::array<runtime::CompiledVec2Parameter, 4> corners;
    for (std::size_t i = 0; i < 4; ++i) {
        auto corner = compiledVec2Parameter(findParameterBinding(node, cornerPinCornerRoles()[i]));
        if (!corner) {
            addTopologyFailure(node.id, "Corner Pin parameters could not be lowered.");
            return false;
        }
        corners[i] = *corner;
    }
    if (filter == nullptr || *filter < kWarpFilterBilinear || *filter > kWarpFilterBicubic) {
        addTopologyFailure(node.id, "Corner Pin parameters could not be lowered.");
        return false;
    }
    if (isMuted(node.id)) {
        passWarpThrough(node, *input, indices);
        return true;
    }
    FieldBuild build;
    build.operations = &operations;
    build.indices = &indices;
    const auto c = fieldEmit(build, FieldOp::Coordinate, {-1, -1, -1, -1});
    const auto slot = warpSlot(build, node, *input);
    if (slot < 0)
        return false;
    const auto where = fieldEmit(build, FieldOp::Homography, {c, -1, -1, -1}, {0}, {},
                                 {corners[0], corners[1], corners[2], corners[3]});
    build.steps.back().instruction.slot = slot;
    // The filter, transparent outside the input, the texel as stored (premultiplied).
    const auto read = warpRead(build, where, slot, {*filter, 1, 3, 4, 0, 1});
    const auto& format = composition_->format();
    const auto canvas = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(runtime::CompiledSolid{
        node.id,
        runtime::CompiledColorParameter{document::ParameterId{}, core::Color4d{0.0, 0.0, 0.0, 0.0}},
        fieldConstant(static_cast<double>(format.width())),
        fieldConstant(static_cast<double>(format.height()))});
    return finishWarp(node, build, read, canvas, operations, indices);
}

[[nodiscard]] bool
lowerStMap(const document::NodeRecord& node, std::vector<runtime::CompiledOperation>& operations,
           std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using namespace document;
    const auto input = findInputOperation(node.id, "input", indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return false;
    }
    const auto* u = parameterConstant<std::int64_t>(findParameterBinding(node, "uChannel"));
    const auto* v = parameterConstant<std::int64_t>(findParameterBinding(node, "vChannel"));
    const auto* filter = parameterConstant<std::int64_t>(findParameterBinding(node, "filter"));
    const auto* edges = parameterConstant<std::int64_t>(findParameterBinding(node, "edges"));
    if (u == nullptr || v == nullptr || filter == nullptr || edges == nullptr ||
        *u < kWarpChannelRed || *u > kWarpChannelAlpha || *v < kWarpChannelRed ||
        *v > kWarpChannelAlpha || *filter < kWarpFilterBilinear || *filter > kWarpFilterBicubic ||
        *edges < kWarpEdgeTransparent || *edges > kWarpEdgeMirror) {
        addTopologyFailure(node.id, "ST Map parameters could not be lowered.");
        return false;
    }
    const auto map = findInputOperation(node.id, kWarpMapPort, indices);
    if (isMuted(node.id) || !map) {
        passWarpThrough(node, *input, indices);
        return true;
    }
    const auto& format = composition_->format();
    const auto width = static_cast<double>(format.width());
    const auto height = static_cast<double>(format.height());
    FieldBuild build;
    build.operations = &operations;
    build.indices = &indices;
    const auto c = fieldEmit(build, FieldOp::Coordinate, {-1, -1, -1, -1});
    const auto mapSlot = warpSlot(build, node, *map);
    if (mapSlot < 0)
        return false;
    const auto sourceSlot = warpSlot(build, node, *input);
    if (sourceSlot < 0)
        return false;
    // The map's two channels as stored, at the pixel itself (nearest, the edge pixel).
    const auto uv = warpRead(build, c, mapSlot, {kWarpFilterNearest, 0, 1, 0, *u, *v});
    // A fraction of the composition frame (0, 0 at the bottom left) to field pixels from its centre.
    const auto scaled = fieldSwizzle(build, uv, {0, 1, 4, 4}, {width, height, 1.0, 1.0});
    const auto half = fieldEmit(build, FieldOp::Constant, {-1, -1, -1, -1}, {},
                                {fieldConstant(0.5 * width), fieldConstant(0.5 * height),
                                 fieldConstant(0.0), fieldConstant(0.0)});
    const auto where = fieldEmit(build, FieldOp::Math, {scaled, half, -1, -1}, {1},
                                 {fieldConstant(0.1), fieldConstant(0.0)});
    const auto read = warpRead(build, where, sourceSlot,
                               {*filter, warpEdge(*edges), 3, 4, 0, 1});
    return finishWarp(node, build, read, *map, operations, indices);
}

[[nodiscard]] bool
lowerDisplace(const document::NodeRecord& node, std::vector<runtime::CompiledOperation>& operations,
              std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using namespace document;
    const auto input = findInputOperation(node.id, "input", indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return false;
    }
    const auto* xChannel = parameterConstant<std::int64_t>(findParameterBinding(node, "xChannel"));
    const auto* yChannel = parameterConstant<std::int64_t>(findParameterBinding(node, "yChannel"));
    const auto* filter = parameterConstant<std::int64_t>(findParameterBinding(node, "filter"));
    const auto* edges = parameterConstant<std::int64_t>(findParameterBinding(node, "edges"));
    const auto xAmount = compiledScalarParameter(findParameterBinding(node, "xAmount"));
    const auto yAmount = compiledScalarParameter(findParameterBinding(node, "yAmount"));
    const auto midpoint = compiledScalarParameter(findParameterBinding(node, "midpoint"));
    if (xChannel == nullptr || yChannel == nullptr || filter == nullptr || edges == nullptr ||
        !xAmount || !yAmount || !midpoint || *xChannel < kDisplaceChannelNone ||
        *xChannel > kWarpChannelAlpha + 1 || *yChannel < kDisplaceChannelNone ||
        *yChannel > kWarpChannelAlpha + 1 || *filter < kWarpFilterBilinear ||
        *filter > kWarpFilterBicubic || *edges < kWarpEdgeTransparent ||
        *edges > kWarpEdgeMirror) {
        addTopologyFailure(node.id, "Displace parameters could not be lowered.");
        return false;
    }
    const auto map = findInputOperation(node.id, kWarpMapPort, indices);
    const auto zero = [](const runtime::CompiledScalarParameter& amount) {
        const auto* constant = std::get_if<double>(&amount.source);
        return constant != nullptr && *constant == 0.0;
    };
    const bool noX = *xChannel == kDisplaceChannelNone || zero(*xAmount);
    const bool noY = *yChannel == kDisplaceChannelNone || zero(*yAmount);
    if (isMuted(node.id) || !map || (noX && noY)) {
        passWarpThrough(node, *input, indices);
        return true;
    }
    FieldBuild build;
    build.operations = &operations;
    build.indices = &indices;
    const auto c = fieldEmit(build, FieldOp::Coordinate, {-1, -1, -1, -1});
    const auto sourceSlot = warpSlot(build, node, *input);
    if (sourceSlot < 0)
        return false;
    const auto mapSlot = warpSlot(build, node, *map);
    if (mapSlot < 0)
        return false;
    // The map's two channels as stored (a channel not used reads the first, and its move is zero).
    const auto first = std::max<std::int64_t>(*xChannel - 1, 0);
    const auto second = std::max<std::int64_t>(*yChannel - 1, 0);
    const auto value = warpRead(build, c, mapSlot, {kWarpFilterBilinear, 0, 1, 0, first, second});
    // (value - midpoint): the midpoint is one operand, a swizzle spreads it to both lanes.
    const auto midOnce = fieldEmit(build, FieldOp::Constant, {-1, -1, -1, -1}, {},
                                   {*midpoint, fieldConstant(0.0), fieldConstant(0.0),
                                    fieldConstant(0.0)});
    const auto mid = fieldSwizzle(build, midOnce, {0, 0, 4, 4});
    const auto relative = fieldEmit(build, FieldOp::Math, {value, mid, -1, -1}, {1},
                                    {fieldConstant(0.1), fieldConstant(0.0)});
    const auto amounts = fieldEmit(build, FieldOp::Constant, {-1, -1, -1, -1}, {},
                                   {*xAmount, *yAmount, fieldConstant(0.0), fieldConstant(0.0)});
    const auto moved = fieldEmit(build, FieldOp::Math, {relative, amounts, -1, -1}, {2},
                                 {fieldConstant(0.1), fieldConstant(0.0)});
    // The picture moves right and down (y down) for a map above the midpoint; a field position has
    // y up. An axis with no channel does not move.
    const auto shift = fieldSwizzle(build, moved, {0, 1, 4, 4},
                                    {noX ? 0.0 : 1.0, noY ? 0.0 : -1.0, 0.0, 0.0});
    const auto where = fieldEmit(build, FieldOp::Math, {c, shift, -1, -1}, {1},
                                 {fieldConstant(0.1), fieldConstant(0.0)});
    const auto read = warpRead(build, where, sourceSlot,
                               {*filter, warpEdge(*edges), 3, 4, 0, 1});
    return finishWarp(node, build, read, *input, operations, indices);
}
