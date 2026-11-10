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

// #513 Stabilize and Match Move: the picture moved by the difference of two points. The track point
// (the feature as chosen) and the position (where the track found it in this frame, keyed by the
// tracking job) are Vec2 parameters; the Homography instruction's translation mode turns them into
// the move (Stabilize holds the picture still, Match Move carries it along) and the picture is read
// where that move came from. The canvas is the input, so the frame keeps its size.
[[nodiscard]] bool
lowerStabilize(const document::NodeRecord& node, std::vector<runtime::CompiledOperation>& operations,
               std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using namespace document;
    const auto input = findInputOperation(node.id, "input", indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return false;
    }
    const auto* mode = parameterConstant<std::int64_t>(findParameterBinding(node, "mode"));
    const auto* filter = parameterConstant<std::int64_t>(findParameterBinding(node, "filter"));
    const auto* edges = parameterConstant<std::int64_t>(findParameterBinding(node, "edges"));
    const auto held = compiledVec2Parameter(findParameterBinding(node, "trackPoint"));
    const auto at = compiledVec2Parameter(findParameterBinding(node, "tracked"));
    if (mode == nullptr || filter == nullptr || edges == nullptr || !held || !at ||
        *mode < kStabilizeModeStabilize || *mode > kStabilizeModeMatchMove ||
        *filter < kWarpFilterBilinear || *filter > kWarpFilterBicubic ||
        *edges < kWarpEdgeTransparent || *edges > kWarpEdgeMirror) {
        addTopologyFailure(node.id, "Stabilize parameters could not be lowered.");
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
    const auto where = fieldEmit(build, FieldOp::Homography, {c, -1, -1, -1}, {1, *mode}, {},
                                 {*held, *at});
    const auto read = warpRead(build, where, slot, {*filter, warpEdge(*edges), 3, 4, 0, 1});
    return finishWarp(node, build, read, *input, operations, indices);
}

// #511 Tile & Mirror: four modes on the same inverse mapping, one field raster each. Field positions
// are pixels from the frame's centre with y up; the node's points are pixels from the centre with
// y down, moved into the program by the Homography instruction's translation mode, whose y flip
// they need (held point P, zero second point): direction 0 reads p - P, direction 1 reads p + P.
//
// The canvas of Motion Tile and Kaleidoscope is the composition frame whatever the input's window
// (a shape or text layer's window is its own small box at the layer-local origin): the input's
// window is laid with its centre at the frame's centre (Homography choice 2, the translation by the
// window's centre), and the lattice, the point and the wedge are placed in the frame about it. For
// an input that fills the frame the translation is zero.
//
//   Motion Tile   the pixel, in tiles (p / tile size, less the phase), folded into the cell (Transform
//                 Tile, or Mirror with Mirror Edges), back to pixels, moved to the tile centre; with
//                 Clamp to Output, a pixel beyond half the output size reads a position no image
//                 reaches, so it is transparent. The canvas is the composition frame.
//   Wrap Offset   the pixel reads p - shift, the edge rule Repeat about the input's data window. The
//                 canvas is the input.
//   Mirror        about the line through the point at the angle: p - P, turned so the line is the x
//                 axis, y folded to one side (Min or Max with y negated), turned back, plus P.
//   Kaleidoscope  p - C, turned by the rotation, divided by the size, folded into a wedge (Transform
//                 Kaleidoscope), plus C.
[[nodiscard]] int warpTranslate(FieldBuild& build, const int where,
                                const runtime::CompiledVec2Parameter& point,
                                const std::int64_t direction) {
    const runtime::CompiledVec2Parameter none{document::ParameterId{}, document::Vec2d{}};
    return fieldEmit(build, FieldOp::Homography, {where, -1, -1, -1}, {1, direction}, {},
                     {point, none});
}

// The point as a position register (field units, y up): the translation applied to nothing. A point is
// one operand of the program, so a mode that moves out of the point's frame and back reads this
// register twice.
int warpPosition(FieldBuild& build, const runtime::CompiledVec2Parameter& point) {
    const auto nothing = fieldBroadcast(build, fieldConstant(0.0));
    return warpTranslate(build, nothing, point, 1);
}

// The position `where` read in the window of image slot `slot` laid with its centre at the field
// origin: the translation by the window's centre, solved when the slot's window is placed.
int warpWindowCentre(FieldBuild& build, const int where, const std::int8_t slot) {
    const auto read = fieldEmit(build, FieldOp::Homography, {where, -1, -1, -1}, {2});
    build.steps.back().instruction.slot = slot;
    return read;
}

// A Math operation on `a` and `b` (the operation's index in core::field::FieldMathOperation).
int warpMath(FieldBuild& build, const std::int64_t operation, const int a, const int b,
             const runtime::CompiledScalarParameter angle = fieldConstant(0.0)) {
    return fieldEmit(build, FieldOp::Math, {a, b, -1, -1}, {operation},
                     {fieldConstant(0.1), angle});
}

[[nodiscard]] bool
lowerTileMirror(const document::NodeRecord& node, std::vector<runtime::CompiledOperation>& operations,
                std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using namespace document;
    constexpr std::int64_t kAdd = 0;
    constexpr std::int64_t kSubtract = 1;
    constexpr std::int64_t kMultiply = 2;
    constexpr std::int64_t kDivide = 3;
    constexpr std::int64_t kMin = 4;
    constexpr std::int64_t kMax = 5;
    constexpr std::int64_t kAbs = 7;
    constexpr std::int64_t kDot = 13;
    constexpr std::int64_t kRotate = 17;
    const auto input = findInputOperation(node.id, "input", indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return false;
    }
    const auto fail = [this, &node] {
        addTopologyFailure(node.id, "Tile & Mirror parameters could not be lowered.");
        return false;
    };
    const auto* mode = parameterConstant<std::int64_t>(findParameterBinding(node, "mode"));
    const auto* filter = parameterConstant<std::int64_t>(findParameterBinding(node, "filter"));
    if (mode == nullptr || filter == nullptr || *mode < kTileMirrorModeTile ||
        *mode > kTileMirrorModeKaleidoscope || *filter < kWarpFilterBilinear ||
        *filter > kWarpFilterBicubic)
        return fail();
    // Wrap Offset's edge rule is its own: Repeat.
    std::int64_t edge = warpEdge(kWarpEdgeRepeat);
    if (*mode != kTileMirrorModeWrapOffset) {
        const auto* edges = parameterConstant<std::int64_t>(findParameterBinding(node, "edges"));
        if (edges == nullptr || *edges < kWarpEdgeTransparent || *edges > kWarpEdgeMirror)
            return fail();
        edge = warpEdge(*edges);
    }
    const auto scalar = [this, &node](const std::string_view role) {
        return compiledScalarParameter(findParameterBinding(node, role));
    };
    const auto point = [this, &node](const std::string_view role) {
        return compiledVec2Parameter(findParameterBinding(node, role));
    };
    const auto flag = [this, &node](const std::string_view role) -> std::optional<bool> {
        const auto* value = parameterConstant<bool>(findParameterBinding(node, role));
        return value == nullptr ? std::nullopt : std::optional<bool>{*value};
    };
    // Every value of the mode is read before a muted node is passed through, so a bad node refuses.
    std::optional<runtime::CompiledVec2Parameter> pointValue;
    std::optional<runtime::CompiledScalarParameter> first;
    std::optional<runtime::CompiledScalarParameter> second;
    std::optional<runtime::CompiledScalarParameter> third;
    std::optional<runtime::CompiledScalarParameter> fourth;
    std::optional<runtime::CompiledScalarParameter> fifth;
    std::optional<runtime::CompiledScalarParameter> sixth;
    bool clamp = false;
    bool linkedSize = false;
    bool mirror = false;
    bool upper = false;
    std::int64_t segments = 6;
    switch (*mode) {
    case kTileMirrorModeTile: {
        const auto link = flag("linkTileSize");
        const auto clampValue = flag("clampToOutput");
        const auto mirrorValue = flag("mirrorEdges");
        pointValue = point("tileCenter");
        first = scalar("tileWidth");
        second = link.value_or(false) ? first : scalar("tileHeight");
        third = scalar("horizontalPhase");
        fourth = scalar("verticalPhase");
        if (!link || !clampValue || !mirrorValue || !pointValue || !first || !second || !third ||
            !fourth)
            return fail();
        clamp = *clampValue;
        linkedSize = *link;
        mirror = *mirrorValue;
        if (clamp) {
            fifth = scalar("outputWidth");
            sixth = scalar("outputHeight");
            if (!fifth || !sixth)
                return fail();
        }
        break;
    }
    case kTileMirrorModeWrapOffset:
        pointValue = point("offset");
        if (!pointValue)
            return fail();
        break;
    case kTileMirrorModeMirror: {
        const auto* side = parameterConstant<std::int64_t>(findParameterBinding(node, "mirrorSide"));
        pointValue = point("mirrorPoint");
        first = scalar("mirrorAngle");
        if (side == nullptr || *side < kTileMirrorSideLeftBelow || *side > kTileMirrorSideRightAbove ||
            !pointValue || !first)
            return fail();
        upper = *side == kTileMirrorSideRightAbove;
        break;
    }
    default: {
        const auto* count =
            parameterConstant<std::int64_t>(findParameterBinding(node, "kaleidoscopeSegments"));
        pointValue = point("kaleidoscopeCenter");
        first = scalar("kaleidoscopeAngle");
        second = scalar("kaleidoscopeSize");
        if (count == nullptr || *count < kMinimumKaleidoscopeSegments ||
            *count > kMaximumKaleidoscopeSegments || !pointValue || !first || !second)
            return fail();
        segments = *count;
        break;
    }
    }
    if (isMuted(node.id)) {
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
    const auto slot = warpSlot(build, node, *input);
    if (slot < 0)
        return false;
    int where = c;
    runtime::OperationIndex canvas = *input;
    // A transparent solid of the composition frame's size.
    const auto frameCanvas = [&] {
        const auto solid = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledSolid{
            node.id,
            runtime::CompiledColorParameter{document::ParameterId{}, core::Color4d{0.0, 0.0, 0.0, 0.0}},
            fieldConstant(width), fieldConstant(height)});
        return solid;
    };
    switch (*mode) {
    case kTileMirrorModeTile: {
        // The tile size in pixels, and the phase and output size in the units they are used in.
        // A parameter is one operand of the program: a linked height reads the width's lane again.
        const bool linked = linkedSize;
        const auto sizes = fieldEmit(build, FieldOp::Constant, {-1, -1, -1, -1}, {},
                                     {*first, linked ? fieldConstant(0.0) : *second,
                                      fieldConstant(0.0), fieldConstant(0.0)});
        const auto tile = fieldSwizzle(build, sizes, {0, linked ? 0 : 1, 4, 4},
                                       {width / 100.0, height / 100.0, 1.0, 1.0});
        const auto phases = fieldEmit(build, FieldOp::Constant, {-1, -1, -1, -1}, {},
                                      {*third, *fourth, fieldConstant(0.0), fieldConstant(0.0)});
        // 360 degrees is one tile; the picture moves right and down (y down) for a positive phase.
        const auto shift = fieldSwizzle(build, phases, {0, 1, 4, 4}, {1.0 / 360.0, -1.0 / 360.0, 1.0, 1.0});
        const auto tiles = warpMath(build, kDivide, c, tile);
        const auto phased = warpMath(build, kSubtract, tiles, shift);
        const runtime::CompiledVec2Parameter origin{document::ParameterId{}, document::Vec2d{}};
        const runtime::CompiledVec2Parameter unit{document::ParameterId{}, document::Vec2d{1.0, 1.0}};
        const auto cell = fieldEmit(build, FieldOp::Transform, {phased, -1, -1, -1},
                                    {mirror ? 3 : 2, 6}, {fieldConstant(0.0), fieldConstant(0.0)},
                                    {origin, unit, origin, unit});
        const auto pixels = warpMath(build, kMultiply, cell, tile);
        where = warpTranslate(build, pixels, *pointValue, 1);
        if (clamp) {
            const auto extent = fieldEmit(build, FieldOp::Constant, {-1, -1, -1, -1}, {},
                                          {*fifth, *sixth, fieldConstant(0.0), fieldConstant(0.0)});
            const auto half = fieldSwizzle(build, extent, {0, 1, 4, 4}, {width / 200.0, height / 200.0, 1.0, 1.0});
            const auto distance = warpMath(build, kAbs, c, c);
            const auto beyond = warpMath(build, kSubtract, distance, half);
            const auto none = fieldBroadcast(build, fieldConstant(0.0));
            const auto outside = warpMath(build, kMax, beyond, none);
            // A pixel past the output reads a position no texel holds, so it is transparent.
            const auto far = fieldSwizzle(build, outside, {0, 1, 4, 4}, {1.0e19, 1.0e19, 1.0, 1.0});
            where = warpMath(build, kAdd, where, far);
        }
        where = warpWindowCentre(build, where, slot);
        canvas = frameCanvas();
        break;
    }
    case kTileMirrorModeWrapOffset:
        where = warpTranslate(build, c, *pointValue, 0);
        break;
    case kTileMirrorModeMirror: {
        // With the angle's cosine and sine from one turned unit vector (an operand is read once), the
        // unit normal m = (sin, cos) is the side the line's angle names; d = q . m is the distance
        // from the line, and the pixels on the far side read q - 2 d m, their reflection.
        const auto origin = warpPosition(build, *pointValue);
        const auto local = warpMath(build, kSubtract, c, origin);
        const auto unit = fieldEmit(build, FieldOp::Constant, {-1, -1, -1, -1}, {},
                                    {fieldConstant(1.0), fieldConstant(0.0), fieldConstant(0.0),
                                     fieldConstant(0.0)});
        const auto turned = warpMath(build, kRotate, unit, -1, *first);
        const auto normal = fieldSwizzle(build, turned, {1, 0, 4, 4});
        const auto distance = warpMath(build, kDot, local, normal);
        const auto none = fieldBroadcast(build, fieldConstant(0.0));
        // Left or Below keeps the negative side of the normal, so the positive side is reflected.
        const auto far = warpMath(build, upper ? kMin : kMax, distance, none);
        const auto along = warpMath(build, kMultiply, far, normal);
        const auto twice = fieldSwizzle(build, along, {0, 1, 4, 4}, {2.0, 2.0, 1.0, 1.0});
        const auto reflected = warpMath(build, kSubtract, local, twice);
        where = warpMath(build, kAdd, reflected, origin);
        break;
    }
    default: {
        const auto origin = warpPosition(build, *pointValue);
        const auto local = warpMath(build, kSubtract, c, origin);
        const auto turned = warpMath(build, kRotate, local, -1, *first);
        const auto once = fieldEmit(build, FieldOp::Constant, {-1, -1, -1, -1}, {},
                                    {*second, fieldConstant(0.0), fieldConstant(0.0), fieldConstant(0.0)});
        const auto size = fieldSwizzle(build, once, {0, 0, 4, 4});
        const auto divided = warpMath(build, kDivide, turned, size);
        const auto scaled = fieldSwizzle(build, divided, {0, 1, 4, 4}, {100.0, 100.0, 1.0, 1.0});
        const auto wedge = fieldEmit(build, FieldOp::Transform, {scaled, -1, -1, -1}, {4, segments},
                                     {fieldConstant(0.0), fieldConstant(0.0)});
        where = warpWindowCentre(build, warpMath(build, kAdd, wedge, origin), slot);
        canvas = frameCanvas();
        break;
    }
    }
    const auto read = warpRead(build, where, slot, {*filter, edge, 3, 4, 0, 1});
    return finishWarp(node, build, read, canvas, operations, indices);
}
