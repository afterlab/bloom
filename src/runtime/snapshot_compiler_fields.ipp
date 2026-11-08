// #525 Fields: compiling the Fields node family. Included inside CompilePass, beside
// snapshot_compiler_lowering.ipp. A field output is not an operation: a raster (a generator's
// `image` or `mask` output, Rasterize Field's) or a Field Sample compiles the fields it reads into
// one field program, walking the field edges upstream from it. Each node is compiled at a
// coordinate register (a Field Transform or Derivative moves the coordinate its input is compiled
// at), memoized per node, port and coordinate; virtual registers are then packed into the sixteen
// physical ones. runtime/field_raster.hpp has each operation's operand order.

struct FieldBuild final {
    struct Step final {
        runtime::FieldInstruction instruction;
        int output = 0;
        std::array<int, 4> inputs{-1, -1, -1, -1};
    };
    std::vector<Step> steps;
    std::map<std::tuple<document::NodeId, std::string, int>, int> memo;
    std::map<document::NodeId, std::int8_t> slotOf;
    std::vector<runtime::OperationIndex> images;
    int nextRegister = 0;
    int coordinate = -1;
    bool usesTime = false;
    // Field Sample reads no image.
    bool allowImages = true;
    std::vector<runtime::CompiledOperation>* operations = nullptr;
    const std::unordered_map<document::NodeId, runtime::OperationIndex>* indices = nullptr;
};

using FieldOp = core::field::FieldOp;

[[nodiscard]] static runtime::CompiledScalarParameter fieldConstant(const double value) {
    return runtime::CompiledScalarParameter{document::ParameterId{}, value};
}

int fieldEmit(FieldBuild& build, const FieldOp op, const std::array<int, 4> inputs,
              std::vector<std::int64_t> choices = {},
              std::vector<runtime::CompiledScalarParameter> scalars = {},
              std::vector<runtime::CompiledVec2Parameter> vectors = {}) {
    FieldBuild::Step step;
    step.instruction.op = static_cast<std::uint8_t>(op);
    step.instruction.choices = std::move(choices);
    step.instruction.scalars = std::move(scalars);
    step.instruction.vectors = std::move(vectors);
    step.output = build.nextRegister++;
    step.inputs = inputs;
    build.steps.push_back(std::move(step));
    return build.steps.back().output;
}

int fieldSwizzle(FieldBuild& build, const int input, const std::array<std::int64_t, 4> lanes,
                 const std::array<double, 4> scales = {1.0, 1.0, 1.0, 1.0}) {
    return fieldEmit(build, FieldOp::Swizzle, {input, -1, -1, -1},
                     {lanes[0], lanes[1], lanes[2], lanes[3]},
                     {fieldConstant(scales[0]), fieldConstant(scales[1]), fieldConstant(scales[2]),
                      fieldConstant(scales[3])});
}

int fieldBroadcast(FieldBuild& build, const runtime::CompiledScalarParameter& value) {
    return fieldEmit(build, FieldOp::Constant, {-1, -1, -1, -1}, {}, {value, value, value, value});
}

[[nodiscard]] std::optional<runtime::CompiledScalarParameter>
fieldScalar(const document::NodeRecord& node, const std::string_view role) {
    auto value = compiledScalarParameter(findParameterBinding(node, role));
    if (!value)
        addTopologyFailure(node.id, "Field parameter " + std::string(role) +
                                        " could not be lowered.");
    return value;
}

[[nodiscard]] std::optional<runtime::CompiledVec2Parameter>
fieldVector(const document::NodeRecord& node, const std::string_view role) {
    auto value = compiledVec2Parameter(findParameterBinding(node, role));
    if (!value)
        addTopologyFailure(node.id, "Field parameter " + std::string(role) +
                                        " could not be lowered.");
    return value;
}

// A constant choice, Integer, Seed or Boolean as its stored integer (the definition's default when
// absent).
[[nodiscard]] std::int64_t fieldChoice(const document::NodeRecord& node,
                                       const std::string_view role) const {
    const auto* binding = findParameterBinding(node, role);
    if (const auto* integer = parameterConstant<std::int64_t>(binding))
        return *integer;
    if (const auto* flag = parameterConstant<bool>(binding))
        return *flag ? 1 : 0;
    const auto* spec = document::fieldParameterSpec(node.typeId, role);
    return spec == nullptr ? 0 : static_cast<std::int64_t>(spec->defaultX);
}

[[nodiscard]] runtime::SocketValueKind fieldOutputKind(const document::NodeId nodeId,
                                                       const std::string_view port) const {
    const auto definition = definitions_.find(nodeId);
    if (definition == definitions_.end())
        return runtime::SocketValueKind::ScalarField;
    for (const auto& output : definition->second->outputs)
        if (output.name == port)
            return output.valueKind;
    return runtime::SocketValueKind::ScalarField;
}

// The value of the field the input `port` of `node` reads, at coordinate register `coordinate`;
// nullopt when nothing is connected. -2 on failure.
std::optional<int> fieldInput(FieldBuild& build, const document::NodeRecord& node,
                              const std::string_view port, const int coordinate) {
    const auto* edge = fixedInputEdge(node.id, port);
    if (edge == nullptr)
        return std::nullopt;
    return fieldValue(build, edge->source.nodeId, edge->source.port, coordinate);
}

[[nodiscard]] runtime::SocketValueKind fieldInputKind(const document::NodeRecord& node,
                                                      const std::string_view port) const {
    const auto* edge = fixedInputEdge(node.id, port);
    return edge == nullptr ? runtime::SocketValueKind::ScalarField
                           : fieldOutputKind(edge->source.nodeId, edge->source.port);
}

// The image slot an Image Field or mask Shape Distance reads; -1 with a topology failure.
std::int8_t fieldSlot(FieldBuild& build, const document::NodeRecord& node,
                      const std::optional<runtime::OperationIndex> image) {
    if (const auto found = build.slotOf.find(node.id); found != build.slotOf.end())
        return found->second;
    if (!build.allowImages || !image)
        return -1;
    if (build.images.size() >= core::field::kMaxFieldImages) {
        addTopologyFailure(node.id, "A field program reads at most four images.");
        return -2;
    }
    const auto slot = static_cast<std::int8_t>(build.images.size());
    build.images.push_back(*image);
    build.slotOf.emplace(node.id, slot);
    return slot;
}

// The register holding `port` of field node `nodeId` at `coordinate`; -2 on failure.
int fieldValue(FieldBuild& build, const document::NodeId nodeId, const std::string_view port,
               const int coordinate) {
    const auto key = std::tuple{nodeId, std::string(port), coordinate};
    if (const auto found = build.memo.find(key); found != build.memo.end())
        return found->second;
    const auto* node = findNode(nodeId);
    const auto* spec = node == nullptr ? nullptr : document::fieldNodeSpec(node->typeId);
    if (node == nullptr || spec == nullptr) {
        addTopologyFailure(nodeId, "A field input is not fed by a Fields node.");
        return -2;
    }
    if (build.steps.size() > 4 * core::field::kMaxFieldInstructions) {
        addTopologyFailure(nodeId, "The field program exceeds 64 operations.");
        return -2;
    }
    int value = -2;
    if (isMuted(nodeId)) {
        // A muted field node passes its first field input through, or is zero.
        const auto first = std::ranges::find_if(spec->inputs, [](const auto& input) {
            return document::isFieldSocketKind(input.kind) || input.anyField;
        });
        const auto passed = first == spec->inputs.end()
                                ? std::nullopt
                                : fieldInput(build, *node, first->name, coordinate);
        value = passed ? *passed : fieldBroadcast(build, fieldConstant(0.0));
    } else {
        value = compileFieldNode(build, *node, port, coordinate);
    }
    if (value >= 0)
        build.memo.emplace(key, value);
    return value;
}

int compileFieldNode(FieldBuild& build, const document::NodeRecord& node,
                     const std::string_view port, const int c) {
    using namespace document;
    const auto& type = node.typeId;
    const auto fail = [&](const char* text) {
        addTopologyFailure(node.id, text);
        return -2;
    };
    const auto scalars = [&](const std::initializer_list<std::string_view> roles)
        -> std::optional<std::vector<runtime::CompiledScalarParameter>> {
        std::vector<runtime::CompiledScalarParameter> out;
        for (const auto role : roles) {
            auto value = fieldScalar(node, role);
            if (!value)
                return std::nullopt;
            out.push_back(*value);
        }
        return out;
    };
    const auto vectors = [&](const std::initializer_list<std::string_view> roles)
        -> std::optional<std::vector<runtime::CompiledVec2Parameter>> {
        std::vector<runtime::CompiledVec2Parameter> out;
        for (const auto role : roles) {
            auto value = fieldVector(node, role);
            if (!value)
                return std::nullopt;
            out.push_back(*value);
        }
        return out;
    };
    const auto choices = [&](const std::initializer_list<std::string_view> roles) {
        std::vector<std::int64_t> out;
        for (const auto role : roles)
            out.push_back(fieldChoice(node, role));
        return out;
    };
    if (type == kFieldNoiseNodeType) {
        const auto s = scalars({"rotation", "octaves", "lacunarity", "gain", "octaveRotation",
                                "ridgeOffset", "ridgeGain", "evolution", "loopLength",
                                "contrast", "brightness"});
        const auto v = vectors({"size", "offset", "octaveOffset"});
        if (!s || !v)
            return -2;
        const auto noise = [&](const std::int64_t stream) {
            auto picked = choices({"type", "fractal", "seed", "evolutionMode", "loop", "invert",
                                   "overflow", "range"});
            picked.push_back(stream);
            return fieldEmit(build, FieldOp::Noise, {c, -1, -1, -1}, std::move(picked), *s, *v);
        };
        if (fieldChoice(node, "loop") != 0)
            build.usesTime = true;
        if (port == "vector") {
            const auto a = noise(0);
            const auto b = noise(1);
            return fieldEmit(build, FieldOp::Combine, {a, b, -1, -1}, {},
                             {fieldConstant(0.0), fieldConstant(0.0), fieldConstant(0.0),
                              fieldConstant(0.0)});
        }
        const auto n = noise(0);
        if (port == "gradient")
            return fieldSwizzle(build, n, {1, 2, 4, 4});
        if (port == "curl")
            return fieldSwizzle(build, n, {2, 1, 4, 4}, {1.0, -1.0, 1.0, 1.0});
        return n;
    }
    if (type == kFieldVoronoiNodeType) {
        const auto s = scalars({"size", "jitter", "smoothness", "evolution"});
        if (!s)
            return -2;
        const bool position = port == "cellPosition" || port == "direction";
        auto picked = choices({"mode", "metric", "seed", "octaves"});
        picked.push_back(position ? 1 : 0);
        picked.push_back(port == "edgeDistance" ? 1 : 0);
        const auto v = fieldEmit(build, FieldOp::Voronoi, {c, -1, -1, -1}, std::move(picked), *s);
        if (port == "cellId")
            return fieldSwizzle(build, v, {1, 4, 4, 4});
        if (port == "edgeDistance")
            return fieldSwizzle(build, v, {2, 4, 4, 4});
        if (port == "direction")
            return fieldSwizzle(build, v, {2, 3, 4, 4});
        return v;
    }
    if (type == kFieldRandomNodeType) {
        const auto s = scalars({"evolution"});
        const auto v = vectors({"cellSize"});
        if (!s || !v)
            return -2;
        auto picked = choices({"seed", "smooth", "distribution"});
        picked.push_back(port == "vector" ? 1 : (port == "color" ? 2 : 0));
        return fieldEmit(build, FieldOp::Random, {c, -1, -1, -1}, std::move(picked), *s, *v);
    }
    if (type == kFieldPatternNodeType) {
        const auto s = scalars({"size", "width", "rotation", "phase", "speed", "softness"});
        const auto v = vectors({"centre"});
        if (!s || !v)
            return -2;
        const auto& speed = (*s)[4];
        if (const auto* constant = std::get_if<double>(&speed.source);
            constant == nullptr || *constant != 0.0)
            build.usesTime = true;
        return fieldEmit(build, FieldOp::Pattern, {c, -1, -1, -1}, choices({"shape", "waveform"}),
                         *s, *v);
    }
    if (type == kFieldGradientNodeType) {
        const auto s = scalars({"cycle"});
        const auto v = vectors({"start", "end"});
        if (!s || !v)
            return -2;
        return fieldEmit(build, FieldOp::Gradient, {c, -1, -1, -1}, choices({"mode", "repeat"}),
                         *s, *v);
    }
    if (type == kFieldShapeDistanceNodeType) {
        const auto s = scalars({"rotation", "cornerRadius", "innerRatio", "falloff"});
        const auto v = vectors({"centre", "size"});
        if (!s || !v)
            return -2;
        auto picked = choices({"source", "primitive", "falloffCurve", "invert", "sides"});
        std::int8_t slot = -1;
        if (picked[0] == kFieldShapeSourceMask) {
            const auto mask = build.indices == nullptr
                                  ? std::nullopt
                                  : findInputOperation(node.id, "maskInput", *build.indices);
            if (mask && build.allowImages && !build.slotOf.contains(node.id)) {
                // The mask's exact distance transform, reaching its falloff (or, animated, the
                // largest reach) past the mask.
                const auto* falloff = std::get_if<double>(&(*s)[3].source);
                const double reach = falloff != nullptr ? *falloff + 2.0 : 512.0;
                const auto distance = runtime::OperationIndex::fromRaw(build.operations->size());
                build.operations->emplace_back(runtime::CompiledImageEffect{
                    node.id, *mask, runtime::MaskDistanceKernel{reach}});
                slot = fieldSlot(build, node, distance);
            } else {
                slot = fieldSlot(build, node, std::nullopt);
            }
            if (slot == -2)
                return -2;
        }
        const auto shape = [&] {
            FieldBuild::Step step;
            step.instruction.op = static_cast<std::uint8_t>(FieldOp::Shape);
            step.instruction.choices = picked;
            step.instruction.scalars = *s;
            step.instruction.vectors = *v;
            step.instruction.slot = slot;
            step.output = build.nextRegister++;
            step.inputs = {c, -1, -1, -1};
            build.steps.push_back(std::move(step));
            return build.steps.back().output;
        }();
        if (port == "falloff" || port == kFieldImageOutputPort || port == kFieldMaskOutputPort)
            return fieldSwizzle(build, shape, {1, 4, 4, 4});
        if (port == "direction")
            return fieldSwizzle(build, shape, {2, 3, 4, 4});
        return shape;
    }
    if (type == kFieldFromImageNodeType) {
        const auto image = build.indices == nullptr
                               ? std::nullopt
                               : findInputOperation(node.id, "image", *build.indices);
        const auto slot = fieldSlot(build, node, image);
        if (slot == -2)
            return -2;
        auto where = c;
        if (const auto position = fieldInput(build, node, "position", c)) {
            if (*position < 0)
                return -2;
            where = *position;
        }
        auto picked = choices({"filter", "edge"});
        picked.push_back(port == "vector" ? 1 : (port == "color" ? 2 : 0));
        picked.push_back(fieldChoice(node, "channel"));
        picked.push_back(fieldChoice(node, "pairFirst"));
        picked.push_back(fieldChoice(node, "pairSecond"));
        FieldBuild::Step step;
        step.instruction.op = static_cast<std::uint8_t>(FieldOp::Image);
        step.instruction.choices = std::move(picked);
        step.instruction.slot = slot;
        step.output = build.nextRegister++;
        step.inputs = {where, -1, -1, -1};
        build.steps.push_back(std::move(step));
        return build.steps.back().output;
    }
    if (type == kFieldForceNodeType) {
        const auto s = scalars({"strength", "radius", "angle"});
        const auto v = vectors({"centre"});
        if (!s || !v)
            return -2;
        auto picked = choices({"mode", "falloff", "direction"});
        std::optional<runtime::CompiledPathParameter> curve;
        if (picked[1] == 3) {
            const auto* binding = findParameterBinding(node, "curve");
            curve = binding == nullptr ? std::nullopt : compiledPathParameter(binding->parameterId);
            if (!curve)
                return fail("Force curve could not be lowered.");
        }
        const auto force = fieldEmit(build, FieldOp::Force, {c, -1, -1, -1}, std::move(picked),
                                     *s, *v);
        build.steps.back().instruction.curve = curve;
        if (port == "magnitude")
            return fieldSwizzle(build, force, {2, 4, 4, 4});
        return force;
    }
    if (type == kFieldMathNodeType) {
        const auto s = scalars({"valueA", "valueB", "valueT", "smoothness", "angle"});
        if (!s)
            return -2;
        std::array<int, 3> operands{};
        const std::array<std::string_view, 3> ports{"a", "b", "t"};
        for (std::size_t i = 0; i < 3; ++i) {
            const auto input = fieldInput(build, node, ports[i], c);
            if (input && *input < 0)
                return -2;
            operands[i] = input ? *input : fieldBroadcast(build, (*s)[i]);
        }
        return fieldEmit(build, FieldOp::Math, {operands[0], operands[1], operands[2], -1},
                         choices({"operation"}), {(*s)[3], (*s)[4]});
    }
    if (type == kFieldRemapNodeType) {
        const auto s = scalars({"inLow", "inHigh", "outLow", "outHigh", "bias", "gain"});
        if (!s)
            return -2;
        const auto input = fieldInput(build, node, "field", c);
        if (input && *input < 0)
            return -2;
        const auto source = input ? *input : fieldBroadcast(build, fieldConstant(0.0));
        auto picked = choices({"clamp", "invert", "steps"});
        picked.push_back(fieldInputKind(node, "field") == runtime::SocketValueKind::Vector2Field
                             ? 1
                             : 0);
        std::optional<runtime::CompiledPathParameter> curve;
        if (const auto* binding = findParameterBinding(node, "curve")) {
            curve = compiledPathParameter(binding->parameterId);
            // The identity curve changes nothing: leave it out of the program.
            if (curve)
                if (const auto* path = std::get_if<document::PathValue>(&curve->source);
                    path != nullptr && *path == document::defaultFieldCurve())
                    curve.reset();
        }
        const auto remap = fieldEmit(build, FieldOp::Remap, {source, -1, -1, -1},
                                     std::move(picked), *s);
        build.steps.back().instruction.curve = curve;
        return remap;
    }
    if (type == kFieldTransformNodeType) {
        const auto s = scalars({"rotation", "warpAmount"});
        const auto v = vectors({"translate", "scale", "pivot", "tile"});
        if (!s || !v)
            return -2;
        const auto warp = fieldInput(build, node, "warp", c);
        if (warp && *warp < 0)
            return -2;
        const auto moved = fieldEmit(build, FieldOp::Transform, {c, warp ? *warp : -1, -1, -1},
                                     choices({"mode", "segments"}), *s, *v);
        const auto input = fieldInput(build, node, "field", moved);
        if (input && *input < 0)
            return -2;
        return input ? *input : fieldBroadcast(build, fieldConstant(0.0));
    }
    if (type == kFieldDerivativeNodeType) {
        const auto s = scalars({"step", "strength"});
        if (!s)
            return -2;
        const auto mode = fieldChoice(node, "mode");
        const auto* edge = fixedInputEdge(node.id, "field");
        if (edge == nullptr)
            return fieldBroadcast(build, fieldConstant(0.0));
        const bool vector = fieldInputKind(node, "field") == runtime::SocketValueKind::Vector2Field;
        if (!vector && mode == kFieldDerivativeDivergence)
            return fieldBroadcast(build, fieldConstant(0.0));
        const auto* source = findNode(edge->source.nodeId);
        // The Noise's own analytic gradient where it provides one.
        if (!vector && source != nullptr && source->typeId == kFieldNoiseNodeType &&
            edge->source.port == "scalar" && !isMuted(source->id)) {
            const auto n = fieldValue(build, source->id, "scalar", c);
            if (n < 0)
                return -2;
            const auto g = mode == kFieldDerivativeCurl
                               ? fieldSwizzle(build, n, {2, 1, 4, 4}, {1.0, -1.0, 1.0, 1.0})
                               : fieldSwizzle(build, n, {1, 2, 4, 4});
            const auto strength = fieldBroadcast(build, (*s)[1]);
            return fieldEmit(build, FieldOp::Math, {g, strength, -1, -1}, {2},
                             {fieldConstant(0.1), fieldConstant(0.0)});
        }
        std::array<int, 4> taps{};
        const std::array<std::array<std::int64_t, 2>, 4> signs{
            {{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
        for (std::size_t i = 0; i < 4; ++i) {
            const auto offset = fieldEmit(build, FieldOp::Offset, {c, -1, -1, -1},
                                          {signs[i][0], signs[i][1]}, {(*s)[0]});
            taps[i] = fieldValue(build, edge->source.nodeId, edge->source.port, offset);
            if (taps[i] < 0)
                return -2;
        }
        const std::int64_t kernelMode =
            mode == kFieldDerivativeGradient ? 0 : (mode == kFieldDerivativeCurl ? (vector ? 2 : 1) : 3);
        return fieldEmit(build, FieldOp::Derivative, taps, {kernelMode}, *s);
    }
    if (type == kFieldCombineNodeType) {
        const auto s = scalars({"valueX", "valueY", "valueZ", "valueW"});
        if (!s)
            return -2;
        std::array<int, 4> lanes{-1, -1, -1, -1};
        const std::array<std::string_view, 4> ports{"x", "y", "z", "w"};
        for (std::size_t i = 0; i < 4; ++i) {
            const auto input = fieldInput(build, node, ports[i], c);
            if (input && *input < 0)
                return -2;
            lanes[i] = input ? *input : -1;
        }
        return fieldEmit(build, FieldOp::Combine, lanes, {}, *s);
    }
    if (type == kFieldSeparateNodeType) {
        const auto input = fieldInput(build, node, "field", c);
        if (input && *input < 0)
            return -2;
        const auto source = input ? *input : fieldBroadcast(build, fieldConstant(0.0));
        const std::int64_t lane = port == "y" ? 1 : (port == "z" ? 2 : (port == "w" ? 3 : 0));
        return fieldSwizzle(build, source, {lane, 4, 4, 4});
    }
    return fail("This Fields node has no field output.");
}

// The program of a raster or Field Sample reading field `port` of `nodeId`: virtual registers
// packed into the sixteen physical ones by last use.
[[nodiscard]] std::optional<runtime::FieldProgram>
finishFieldProgram(FieldBuild& build, const int result, const core::field::FieldResultKind kind,
                   const core::field::FieldOutputMode mode, const document::NodeId owner) {
    if (result < 0)
        return std::nullopt;
    if (build.steps.size() > core::field::kMaxFieldInstructions) {
        addTopologyFailure(owner, "The field program exceeds 64 operations.");
        return std::nullopt;
    }
    std::vector<std::size_t> lastUse(static_cast<std::size_t>(build.nextRegister), 0);
    for (std::size_t i = 0; i < build.steps.size(); ++i) {
        lastUse[static_cast<std::size_t>(build.steps[i].output)] =
            std::max(lastUse[static_cast<std::size_t>(build.steps[i].output)], i);
        for (const auto input : build.steps[i].inputs)
            if (input >= 0)
                lastUse[static_cast<std::size_t>(input)] = i;
    }
    lastUse[static_cast<std::size_t>(result)] = build.steps.size();
    std::vector<int> physical(static_cast<std::size_t>(build.nextRegister), -1);
    std::vector<bool> busy(core::field::kFieldRegisterCount, false);
    runtime::FieldProgram program;
    for (std::size_t i = 0; i < build.steps.size(); ++i) {
        auto& step = build.steps[i];
        for (std::size_t k = 0; k < 4; ++k) {
            const auto input = step.inputs[k];
            step.instruction.inputs[k] =
                input < 0 ? std::int8_t{-1}
                          : static_cast<std::int8_t>(physical[static_cast<std::size_t>(input)]);
        }
        for (const auto input : step.inputs)
            if (input >= 0 && lastUse[static_cast<std::size_t>(input)] == i &&
                physical[static_cast<std::size_t>(input)] >= 0)
                busy[static_cast<std::size_t>(physical[static_cast<std::size_t>(input)])] = false;
        const auto free = std::ranges::find(busy, false);
        if (free == busy.end()) {
            addTopologyFailure(owner, "The field program needs more than sixteen registers.");
            return std::nullopt;
        }
        *free = true;
        const auto reg = static_cast<int>(free - busy.begin());
        physical[static_cast<std::size_t>(step.output)] = reg;
        step.instruction.output = static_cast<std::int8_t>(reg);
        if (lastUse[static_cast<std::size_t>(step.output)] <= i)
            *free = false;
        program.instructions.push_back(step.instruction);
    }
    program.result = static_cast<std::uint8_t>(physical[static_cast<std::size_t>(result)]);
    program.kind = static_cast<std::uint8_t>(kind);
    program.mode = static_cast<std::uint8_t>(mode);
    program.compositionSeconds = composition_->duration().toSeconds();
    program.usesTime = build.usesTime;
    return program;
}

[[nodiscard]] static core::field::FieldResultKind fieldResultKind(const runtime::SocketValueKind kind) {
    if (kind == runtime::SocketValueKind::Vector2Field)
        return core::field::FieldResultKind::Vector;
    if (kind == runtime::SocketValueKind::ColorField)
        return core::field::FieldResultKind::Color;
    return core::field::FieldResultKind::Scalar;
}

// The primary scalar field a generator's fused `image` and `mask` outputs rasterize.
[[nodiscard]] static std::string_view fieldGeneratorPort(const std::string_view type) {
    using namespace document;
    if (type == kFieldVoronoiNodeType)
        return "distance";
    if (type == kFieldGradientNodeType)
        return "t";
    if (type == kFieldShapeDistanceNodeType)
        return "falloff";
    return "scalar";
}

// One raster of `port` of Fields node `node` into `operations`: an `image` or `mask` output, or
// (for an image input that takes a field, Colorize's) a field output drawn raw over the
// composition.
[[nodiscard]] std::optional<runtime::OperationIndex>
lowerFieldRaster(const document::NodeRecord& node, const std::string_view port,
                 std::vector<runtime::CompiledOperation>& operations,
                 const std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using namespace document;
    using core::field::FieldOutputMode;
    using core::field::FieldResultKind;
    FieldBuild build;
    build.operations = &operations;
    build.indices = &indices;
    build.coordinate = fieldEmit(build, FieldOp::Coordinate, {-1, -1, -1, -1});
    const auto c = build.coordinate;
    const bool mask = port == kFieldMaskOutputPort;
    int result = -2;
    auto kind = FieldResultKind::Scalar;
    auto mode = mask ? FieldOutputMode::Mask : FieldOutputMode::Raw;
    std::uint32_t samples = 1;
    std::optional<runtime::OperationIndex> extentImage;
    if (node.typeId != kFieldRasterizeNodeType) {
        const bool fused = port == kFieldImageOutputPort || mask;
        result = fieldValue(build, node.id, fused ? fieldGeneratorPort(node.typeId) : port, c);
        if (!fused)
            kind = fieldResultKind(fieldOutputKind(node.id, port));
    } else {
        const auto* edge = fixedInputEdge(node.id, "field");
        const auto sourceKind =
            edge == nullptr ? runtime::SocketValueKind::ScalarField
                            : fieldOutputKind(edge->source.nodeId, edge->source.port);
        const auto value = [&](const int at) {
            return edge == nullptr ? fieldBroadcast(build, fieldConstant(0.0))
                                   : fieldValue(build, edge->source.nodeId, edge->source.port, at);
        };
        const auto supersample = fieldChoice(node, "supersample");
        samples = supersample >= 2 ? 16U : (supersample == 1 ? 4U : 1U);
        const auto output = fieldChoice(node, "output");
        const auto inspect = fieldChoice(node, "inspect");
        // A vector the arrows and streamlines follow: the field itself, or a scalar's gradient.
        const auto direction = [&](const int at) {
            if (sourceKind == runtime::SocketValueKind::Vector2Field)
                return value(at);
            std::array<int, 4> taps{};
            const std::array<std::array<std::int64_t, 2>, 4> signs{
                {{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
            for (std::size_t i = 0; i < 4; ++i) {
                const auto offset = fieldEmit(build, FieldOp::Offset, {at, -1, -1, -1},
                                              {signs[i][0], signs[i][1]}, {fieldConstant(1.0)});
                taps[i] = value(offset);
                if (taps[i] < 0)
                    return -2;
            }
            return fieldEmit(build, FieldOp::Derivative, taps, {0},
                             {fieldConstant(1.0), fieldConstant(1.0)});
        };
        const auto rampOf = [&](const int input) -> int {
            if (input < 0)
                return -2;
            auto magnitude = input;
            if (sourceKind == runtime::SocketValueKind::Vector2Field)
                magnitude = fieldEmit(build, FieldOp::Math, {input, input, -1, -1}, {14},
                                      {fieldConstant(0.1), fieldConstant(0.0)});
            const auto low = fieldScalar(node, "rampLow");
            const auto high = fieldScalar(node, "rampHigh");
            const auto* binding = findParameterBinding(node, "ramp");
            auto ramp = binding == nullptr ? std::nullopt
                                           : compiledGradientParameter(node, *binding);
            if (!low || !high || !ramp) {
                addTopologyFailure(node.id, "Rasterize Field ramp could not be lowered.");
                return -2;
            }
            const auto colour = fieldEmit(build, FieldOp::Ramp, {magnitude, -1, -1, -1}, {},
                                          {*low, *high});
            build.steps.back().instruction.ramp = std::move(*ramp);
            return colour;
        };
        if (inspect == kFieldInspectArrows) {
            const auto spacing = fieldScalar(node, "arrowSpacing");
            const auto scale = fieldScalar(node, "arrowScale");
            if (!spacing || !scale)
                return std::nullopt;
            const auto centre = fieldEmit(build, FieldOp::CellCentre, {c, -1, -1, -1}, {},
                                          {*spacing});
            const auto v = direction(centre);
            if (v < 0)
                return std::nullopt;
            result = fieldEmit(build, FieldOp::Arrow, {c, centre, v, -1}, {},
                               {*spacing, *scale, fieldConstant(1.0)});
            mode = FieldOutputMode::Mask;
        } else if (inspect == kFieldInspectStreamlines) {
            const auto v = direction(c);
            if (v < 0)
                return std::nullopt;
            result = fieldEmit(build, FieldOp::Streak, {c, v, -1, -1}, {},
                               {fieldConstant(1.5), fieldConstant(2.0)});
            mode = mask ? FieldOutputMode::Mask : FieldOutputMode::Raw;
        } else if (inspect == kFieldInspectHeatmap ||
                   (!mask && output == kFieldRasterColor)) {
            result = rampOf(value(c));
            kind = FieldResultKind::Color;
            mode = mask ? FieldOutputMode::Mask : FieldOutputMode::Color;
        } else {
            result = value(c);
            kind = fieldResultKind(sourceKind);
            if (!mask && output == kFieldRasterMask)
                mode = FieldOutputMode::Mask;
        }
        if (fieldChoice(node, "extent") == kFieldExtentInput)
            extentImage = findInputOperation(node.id, "extentImage", indices);
    }
    if (result < 0)
        return std::nullopt;
    auto program = finishFieldProgram(build, result, kind, mode, node.id);
    if (!program)
        return std::nullopt;
    // The canvas the raster covers: the composition, the extent image's data window, or an
    // explicit rectangle cut from the composition canvas.
    std::optional<runtime::OperationIndex> canvas = extentImage;
    if (!canvas) {
        const auto& format = composition_->format();
        canvas = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(runtime::CompiledSolid{
            node.id,
            runtime::CompiledColorParameter{document::ParameterId{},
                                            core::Color4d{0.0, 0.0, 0.0, 0.0}},
            fieldConstant(static_cast<double>(format.width())),
            fieldConstant(static_cast<double>(format.height()))});
        if (node.typeId == kFieldRasterizeNodeType &&
            fieldChoice(node, "extent") == kFieldExtentExplicit) {
            const auto* centre =
                parameterConstant<document::Vec2d>(findParameterBinding(node, "explicitCentre"));
            const auto* size =
                parameterConstant<document::Vec2d>(findParameterBinding(node, "explicitSize"));
            const auto width = static_cast<double>(format.width());
            const auto height = static_cast<double>(format.height());
            const document::Vec2d at = centre != nullptr ? *centre : document::Vec2d{};
            const document::Vec2d extent = size != nullptr ? *size : document::Vec2d{width, height};
            // The rectangle in composition pixels (y down) and its margins against the canvas.
            const auto left = width * 0.5 + at.x - extent.x * 0.5;
            const auto top = height * 0.5 - at.y - extent.y * 0.5;
            const auto right = width - (left + extent.x);
            const auto bottom = height - (top + extent.y);
            const std::array<double, 4> margins{left, top, right, bottom};
            const auto cut = [&](const runtime::CanvasImageEffect effect, const double sign) {
                std::array<runtime::CompiledScalarParameter, 4> amounts;
                bool any = false;
                for (std::size_t i = 0; i < 4; ++i) {
                    const auto amount = std::max(sign * margins[i], 0.0);
                    any = any || amount > 0.0;
                    amounts[i] = fieldConstant(std::round(amount));
                }
                if (!any)
                    return;
                const auto index = runtime::OperationIndex::fromRaw(operations.size());
                operations.emplace_back(runtime::CompiledImageEffect{
                    node.id, *canvas, runtime::CanvasImageKernel{effect, amounts}});
                canvas = index;
            };
            cut(runtime::CanvasImageEffect::Crop, 1.0);
            cut(runtime::CanvasImageEffect::Pad, -1.0);
        }
    }
    runtime::CompiledImageEffect effect{node.id, *canvas,
                                        runtime::FieldRasterKernel{std::move(*program), samples}};
    effect.fieldImages = std::move(build.images);
    const auto index = runtime::OperationIndex::fromRaw(operations.size());
    operations.emplace_back(std::move(effect));
    return index;
}

// A Fields node in the main lowering loop: its reachable raster outputs, each one raster
// operation; field outputs alone compile into whoever reads them.
[[nodiscard]] bool
lowerFieldNode(const document::NodeRecord& node,
               std::vector<runtime::CompiledOperation>& operations,
               std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    std::optional<runtime::OperationIndex> first;
    for (const auto port : {document::kFieldImageOutputPort, document::kFieldMaskOutputPort}) {
        const bool inspected = request_.channelInspection &&
                               request_.channelInspection->node == node.id &&
                               request_.channelInspection->port == port;
        const auto* spec = document::fieldNodeSpec(node.typeId);
        const bool declared =
            spec != nullptr && std::ranges::any_of(spec->outputs, [&](const auto& output) {
                return output.name == port;
            });
        if (!declared || (!hasReachableOutput(node.id, port) && !inspected))
            continue;
        std::optional<runtime::OperationIndex> index;
        if (isMuted(node.id)) {
            // A muted raster draws nothing.
            index = runtime::OperationIndex::fromRaw(operations.size());
            operations.emplace_back(runtime::CompiledMerge{node.id, {}});
        } else {
            index = lowerFieldRaster(node, port, operations, indices);
            if (!index)
                return false;
        }
        portOperations_.insert_or_assign({node.id, std::string(port)}, *index);
        if (!first)
            first = index;
    }
    if (first)
        indices.emplace(node.id, *first);
    return true;
}

// #525: the fields an image node's field-taking inputs read (Colorize's `input`, issue 3.4), each
// rasterized raw over the composition as the operation behind that field output.
[[nodiscard]] bool
lowerFieldImageInputs(const document::NodeRecord& node, const runtime::NodeDefinition& definition,
                      std::vector<runtime::CompiledOperation>& operations,
                      std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    for (const auto& input : definition.inputs) {
        if (!input.acceptsFieldKinds || document::isFieldSocketKind(input.valueKind))
            continue;
        const auto* edge = fixedInputEdge(node.id, input.name);
        if (edge == nullptr ||
            !document::isFieldSocketKind(fieldOutputKind(edge->source.nodeId, edge->source.port)) ||
            portOperations_.contains({edge->source.nodeId, edge->source.port}))
            continue;
        const auto* source = findNode(edge->source.nodeId);
        if (source == nullptr || !document::isFieldNodeType(source->typeId)) {
            addTopologyFailure(node.id, "A field input is not fed by a Fields node.");
            return false;
        }
        const auto index = lowerFieldRaster(*source, edge->source.port, operations, indices);
        if (!index)
            return false;
        portOperations_.insert_or_assign({edge->source.nodeId, edge->source.port}, *index);
    }
    return true;
}

// #525 Field Sample: the field read at one position, a value-graph kernel.
[[nodiscard]] std::optional<runtime::CompiledValueKernel>
lowerFieldSample(const document::NodeRecord& node, const runtime::NodeDefinition& definition) {
    const auto* edge = fixedInputEdge(node.id, "field");
    FieldBuild build;
    build.allowImages = false;
    build.coordinate = fieldEmit(build, FieldOp::Coordinate, {-1, -1, -1, -1});
    const auto kind = edge == nullptr ? runtime::SocketValueKind::ScalarField
                                      : fieldOutputKind(edge->source.nodeId, edge->source.port);
    const auto result =
        edge == nullptr ? fieldBroadcast(build, fieldConstant(0.0))
                        : fieldValue(build, edge->source.nodeId, edge->source.port,
                                     build.coordinate);
    auto program = finishFieldProgram(build, result, fieldResultKind(kind),
                                      core::field::FieldOutputMode::Raw, node.id);
    auto position = valueOperand(node, definition, "position");
    if (!program || !position)
        return std::nullopt;
    runtime::CompiledValueFieldSample sample;
    // The program's Scalar and Vector 2 operands as value operands, in forEachFieldOperand order.
    bool converted = true;
    runtime::forEachFieldOperand(*program, [&](const auto& operand) {
        using Operand = std::decay_t<decltype(operand)>;
        if constexpr (std::is_same_v<Operand, runtime::CompiledScalarParameter> ||
                      std::is_same_v<Operand, runtime::CompiledVec2Parameter>) {
            runtime::CompiledValueOperand value{operand.id, runtime::CompiledValue{}};
            std::visit(
                [&](const auto& source) {
                    using Source = std::decay_t<decltype(source)>;
                    if constexpr (std::is_same_v<Source, double> ||
                                  std::is_same_v<Source, document::Vec2d>)
                        value.source = runtime::CompiledValue{source};
                    else
                        value.source = source;
                },
                operand.source);
            sample.operands.push_back(std::move(value));
        } else {
            static_cast<void>(converted);
        }
    });
    sample.program = std::make_shared<const runtime::FieldProgram>(std::move(*program));
    sample.position = std::move(*position);
    return runtime::CompiledValueKernel{std::move(sample)};
}

// #486 Lens Distortion: the warped image and the ST map of the same warp, each a field raster over
// the input's data window. The warp is one Lens Warp instruction (the Brown-Conrady model, in
// normalised coordinates about the centre); the image reads the input at that position through the
// field program's Image instruction (bilinear over premultiplied texels, transparent outside the
// input), the ST map draws the position itself, normalised over the composition frame. With every
// coefficient a constant zero the warp is the identity and the image is the input itself. A muted
// node is that identity.
[[nodiscard]] bool
lowerLensDistortion(const document::NodeRecord& node,
                    std::vector<runtime::CompiledOperation>& operations,
                    std::unordered_map<document::NodeId, runtime::OperationIndex>& indices) {
    using namespace document;
    using core::field::FieldOutputMode;
    using core::field::FieldResultKind;
    const auto input = findInputOperation(node.id, "input", indices);
    if (!input) {
        addTopologyFailure(node.id, "Image effect input could not be lowered.");
        return false;
    }
    const bool muted = isMuted(node.id);
    std::array<runtime::CompiledScalarParameter, 4> coefficients{};
    std::size_t next = 0;
    for (const auto role : {"k1", "k2", "p1", "p2"}) {
        const auto value = compiledScalarParameter(findParameterBinding(node, role));
        if (!value) {
            addTopologyFailure(node.id, "Lens Distortion parameters could not be lowered.");
            return false;
        }
        coefficients[next++] = muted ? fieldConstant(0.0) : *value;
    }
    auto centre = compiledVec2Parameter(findParameterBinding(node, "center"));
    const auto* mode = parameterConstant<std::int64_t>(findParameterBinding(node, "mode"));
    if (!centre || mode == nullptr || *mode < kLensUndistort || *mode > kLensRedistort) {
        addTopologyFailure(node.id, "Lens Distortion parameters could not be lowered.");
        return false;
    }
    const auto& format = composition_->format();
    const auto width = static_cast<double>(format.width());
    const auto height = static_cast<double>(format.height());
    // Coordinates are normalised by half the composition's diagonal.
    const auto scale = std::max(0.5 * std::hypot(width, height), 1.0);
    const bool identity = std::ranges::all_of(coefficients, [](const auto& coefficient) {
        const auto* constant = std::get_if<double>(&coefficient.source);
        return constant != nullptr && *constant == 0.0;
    });
    const auto wanted = [&](const std::string_view port) {
        const bool inspected = request_.channelInspection &&
                               request_.channelInspection->node == node.id &&
                               request_.channelInspection->port == port;
        return hasReachableOutput(node.id, port) || inspected;
    };
    const bool wantImage = wanted(kLensDistortionImagePort) || !wanted(kLensDistortionStMapPort);
    const bool wantMap = wanted(kLensDistortionStMapPort);
    const auto raster = [&](FieldBuild& build, const int result, const FieldResultKind kind)
        -> std::optional<runtime::OperationIndex> {
        auto program = finishFieldProgram(build, result, kind, FieldOutputMode::Raw, node.id);
        if (!program)
            return std::nullopt;
        runtime::CompiledImageEffect effect{node.id, *input,
                                            runtime::FieldRasterKernel{std::move(*program), 1}};
        effect.fieldImages = std::move(build.images);
        const auto index = runtime::OperationIndex::fromRaw(operations.size());
        operations.emplace_back(std::move(effect));
        return index;
    };
    const auto warp = [&](FieldBuild& build) {
        return fieldEmit(build, FieldOp::LensWarp, {build.coordinate, -1, -1, -1}, {*mode},
                         {coefficients[0], coefficients[1], coefficients[2], coefficients[3],
                          fieldConstant(scale)},
                         {*centre});
    };
    std::optional<runtime::OperationIndex> first;
    if (wantImage) {
        auto index = *input;
        if (!identity) {
            FieldBuild build;
            build.operations = &operations;
            build.indices = &indices;
            build.coordinate = fieldEmit(build, FieldOp::Coordinate, {-1, -1, -1, -1});
            const auto slot = fieldSlot(build, node, *input);
            if (slot < 0)
                return false;
            const auto where = warp(build);
            FieldBuild::Step step;
            step.instruction.op = static_cast<std::uint8_t>(FieldOp::Image);
            // Bilinear, transparent outside the input, the texel as stored (premultiplied).
            step.instruction.choices = {0, 1, 3, 4, 0, 1};
            step.instruction.slot = slot;
            step.output = build.nextRegister++;
            step.inputs = {where, -1, -1, -1};
            build.steps.push_back(std::move(step));
            const auto drawn = raster(build, build.steps.back().output,
                                      FieldResultKind::Premultiplied);
            if (!drawn)
                return false;
            index = *drawn;
        }
        portOperations_.insert_or_assign({node.id, std::string(kLensDistortionImagePort)}, index);
        first = index;
    }
    if (wantMap) {
        FieldBuild build;
        build.operations = &operations;
        build.indices = &indices;
        build.coordinate = fieldEmit(build, FieldOp::Coordinate, {-1, -1, -1, -1});
        const auto where = warp(build);
        // The position over the composition frame, 0 to 1 from the bottom left.
        const auto scaled = fieldSwizzle(build, where, {0, 1, 4, 4},
                                         {1.0 / width, 1.0 / height, 1.0, 1.0});
        const auto half = fieldEmit(build, FieldOp::Constant, {-1, -1, -1, -1}, {},
                                    {fieldConstant(0.5), fieldConstant(0.5), fieldConstant(0.0),
                                     fieldConstant(0.0)});
        const auto position = fieldEmit(build, FieldOp::Math, {scaled, half, -1, -1}, {0},
                                        {fieldConstant(0.1), fieldConstant(0.0)});
        const auto drawn = raster(build, position, FieldResultKind::Vector);
        if (!drawn)
            return false;
        portOperations_.insert_or_assign({node.id, std::string(kLensDistortionStMapPort)}, *drawn);
        if (!first)
            first = *drawn;
    }
    indices.emplace(node.id, *first);
    return true;
}
