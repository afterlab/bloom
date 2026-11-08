#include "field_raster.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <bloom/document/field_parameters.hpp>
#include <bloom/document/grade_curves.hpp>
#include <bloom/document/gradient_interpolation.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/runtime/grade_curve.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace bloom::runtime::detail {

namespace {

namespace field = core::field;
using field::FieldOp;

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic = EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, text, text};
    return result;
}

// Reads one instruction's sampled operands in order.
class OperandReader final {
  public:
    explicit OperandReader(const std::span<const double> values) noexcept : values_(values) {}
    [[nodiscard]] double next() noexcept {
        if (at_ >= values_.size()) {
            ok_ = false;
            return 0.0;
        }
        const auto v = values_[at_++];
        if (!std::isfinite(v))
            ok_ = false;
        return std::isfinite(v) ? v : 0.0;
    }
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t position() const noexcept { return at_; }

  private:
    std::span<const double> values_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

// A Curves master curve sampled as its point count and (x, y) pairs, as a Fritsch-Carlson cubic.
std::optional<field::FieldCurve> readCurve(OperandReader& reader) {
    const auto count = reader.next();
    if (!(count >= 0.0) || count > static_cast<double>(document::kMaximumFieldCurvePoints))
        return std::nullopt;
    document::PathValue path;
    const auto n = static_cast<std::size_t>(count);
    std::vector<document::Vec2d> points;
    for (std::size_t i = 0; i < n; ++i) {
        const auto x = reader.next();
        const auto y = reader.next();
        points.push_back({x, y});
    }
    if (!reader.ok() || points.empty())
        return std::nullopt;
    const auto shape =
        prepareGradeCurveShape(document::GradeCurve::Master, document::gradeCurvePath(points));
    if (!shape || shape->x.empty() || shape->x.size() > field::kMaxFieldCurveKnots)
        return std::nullopt;
    return field::FieldCurve{shape->x, shape->origin, shape->segments};
}

// A gradient sampled as sampleColorGradeOperands writes one, baked to the 256-entry ramp.
std::optional<std::vector<std::array<double, 4>>> readRamp(OperandReader& reader) {
    const auto count = reader.next();
    if (!(count >= 1.0) || count > 1024.0)
        return std::nullopt;
    document::GradientValue gradient;
    for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
        document::GradientStop stop;
        stop.position = reader.next();
        stop.color = core::Color4d{reader.next(), reader.next(), reader.next(), reader.next()};
        gradient.stops.push_back(stop);
    }
    gradient.modes.interpolation = static_cast<document::GradientInterpolation>(
        static_cast<std::uint8_t>(reader.next()));
    gradient.modes.colorMode =
        static_cast<document::GradientColorMode>(static_cast<std::uint8_t>(reader.next()));
    gradient.modes.hueDirection =
        static_cast<document::GradientHueDirection>(static_cast<std::uint8_t>(reader.next()));
    if (!reader.ok() || !gradient.isValid())
        return std::nullopt;
    std::vector<std::array<double, 4>> ramp(field::kFieldRampEntries);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        const auto c = document::gradientColorAt(
            gradient, static_cast<double>(i) / static_cast<double>(ramp.size() - 1U));
        ramp[i] = {c.red, c.green, c.blue, c.alpha};
    }
    return ramp;
}

std::int64_t choiceAt(const FieldInstruction& instruction, const std::size_t index,
                      const std::int64_t fallback = 0) noexcept {
    return index < instruction.choices.size() ? instruction.choices[index] : fallback;
}

template <typename Enum> Enum enumAt(const FieldInstruction& instruction, const std::size_t index) {
    return static_cast<Enum>(static_cast<std::uint8_t>(choiceAt(instruction, index)));
}

// One instruction's parameter block from its sampled operands.
std::optional<std::vector<float>> encode(const FieldInstruction& in, OperandReader& reader,
                                         const double seconds, const double compositionSeconds) {
    std::vector<double> s;
    for (std::size_t i = 0; i < in.scalars.size(); ++i)
        s.push_back(reader.next());
    std::vector<document::Vec2d> v;
    for (std::size_t i = 0; i < in.vectors.size(); ++i) {
        const auto x = reader.next();
        const auto y = reader.next();
        v.push_back({x, y});
    }
    std::optional<field::FieldCurve> curve;
    if (in.curve) {
        curve = readCurve(reader);
        if (!curve)
            return std::nullopt;
    }
    std::optional<std::vector<std::array<double, 4>>> ramp;
    if (in.ramp) {
        ramp = readRamp(reader);
        if (!ramp)
            return std::nullopt;
    }
    if (!reader.ok())
        return std::nullopt;
    const auto scalar = [&s](const std::size_t i, const double fallback = 0.0) {
        return i < s.size() ? s[i] : fallback;
    };
    const auto vector = [&v](const std::size_t i, const document::Vec2d fallback = {}) {
        return i < v.size() ? v[i] : fallback;
    };
    switch (static_cast<FieldOp>(in.op)) {
    case FieldOp::Coordinate:
        return std::vector<float>{};
    case FieldOp::Constant:
        return field::encodeConstant({scalar(0), scalar(1), scalar(2), scalar(3)});
    case FieldOp::Noise: {
        field::NoiseParameters p;
        p.type = enumAt<field::NoiseType>(in, 0);
        p.fractal = enumAt<field::NoiseFractal>(in, 1);
        p.seed = choiceAt(in, 2);
        p.flow = choiceAt(in, 3) != 0;
        const bool loop = choiceAt(in, 4) != 0;
        p.invert = choiceAt(in, 5) != 0;
        p.overflow = enumAt<field::NoiseOverflow>(in, 6);
        p.bipolar = choiceAt(in, 7) != 0;
        p.stream = static_cast<std::uint32_t>(choiceAt(in, 8));
        p.rotationDegrees = scalar(0);
        p.octaves = scalar(1, 6.0);
        p.lacunarity = scalar(2, 2.0);
        p.gain = scalar(3, 0.5);
        p.octaveRotationDegrees = scalar(4);
        p.ridgeOffset = scalar(5, 1.0);
        p.ridgeGain = scalar(6, 2.0);
        const auto loopLength = scalar(8) > 0.0 ? scalar(8) : compositionSeconds;
        std::int64_t period = 0;
        p.evolution = field::noiseEvolutionPhase(scalar(7), loop, loopLength, seconds, period);
        p.loopPeriod = period;
        p.contrast = scalar(9, 1.0);
        p.brightness = scalar(10);
        p.sizeX = vector(0, {200.0, 200.0}).x;
        p.sizeY = vector(0, {200.0, 200.0}).y;
        p.offsetX = vector(1).x;
        p.offsetY = vector(1).y;
        p.octaveOffsetX = vector(2).x;
        p.octaveOffsetY = vector(2).y;
        return field::encodeNoise(p);
    }
    case FieldOp::Voronoi: {
        field::VoronoiParameters p;
        p.mode = enumAt<field::VoronoiMode>(in, 0);
        p.metric = enumAt<field::VoronoiMetric>(in, 1);
        p.seed = choiceAt(in, 2);
        p.octaves = choiceAt(in, 3, 1);
        p.selector = static_cast<std::uint8_t>(choiceAt(in, 4));
        p.edges = choiceAt(in, 5) != 0;
        p.size = scalar(0, 100.0);
        p.jitter = scalar(1, 1.0);
        p.smoothness = scalar(2);
        p.evolution = scalar(3);
        return field::encodeVoronoi(p);
    }
    case FieldOp::Random: {
        field::RandomParameters p;
        p.seed = choiceAt(in, 0);
        p.smooth = choiceAt(in, 1) != 0;
        p.gaussian = choiceAt(in, 2) != 0;
        p.selector = static_cast<std::uint8_t>(choiceAt(in, 3));
        p.evolution = scalar(0);
        p.cellX = vector(0, {1.0, 1.0}).x;
        p.cellY = vector(0, {1.0, 1.0}).y;
        return field::encodeRandom(p);
    }
    case FieldOp::Pattern: {
        field::PatternParameters p;
        p.shape = enumAt<field::PatternShape>(in, 0);
        p.waveform = enumAt<field::PatternWaveform>(in, 1);
        p.size = scalar(0, 50.0);
        p.width = scalar(1, 0.5);
        p.rotationDegrees = scalar(2);
        // The phase advances by the speed in cycles a second, from exact clock time.
        p.phase = scalar(3) + scalar(4) * seconds;
        p.softness = scalar(5, 1.0);
        p.centreX = vector(0).x;
        p.centreY = vector(0).y;
        return field::encodePattern(p);
    }
    case FieldOp::Gradient: {
        field::GradientFieldParameters p;
        p.mode = enumAt<field::GradientFieldMode>(in, 0);
        p.repeat = enumAt<field::GradientFieldRepeat>(in, 1);
        p.cycle = scalar(0);
        p.startX = vector(0).x;
        p.startY = vector(0).y;
        p.endX = vector(1).x;
        p.endY = vector(1).y;
        return field::encodeGradient(p);
    }
    case FieldOp::Shape: {
        field::ShapeParameters p;
        p.mask = choiceAt(in, 0) != 0;
        p.primitive = enumAt<field::ShapePrimitive>(in, 1);
        p.curve = enumAt<field::ShapeFalloffCurve>(in, 2);
        p.invert = choiceAt(in, 3) != 0;
        p.sides = choiceAt(in, 4, 5);
        p.slot = static_cast<std::uint8_t>(std::max<int>(in.slot, 0));
        p.rotationDegrees = scalar(0);
        p.cornerRadius = scalar(1);
        p.innerRatio = scalar(2, 0.5);
        p.falloff = scalar(3, 100.0);
        p.centreX = vector(0).x;
        p.centreY = vector(0).y;
        p.sizeX = vector(1, {200.0, 200.0}).x;
        p.sizeY = vector(1, {200.0, 200.0}).y;
        return field::encodeShape(p);
    }
    case FieldOp::Image: {
        field::ImageParameters p;
        p.slot = static_cast<std::uint8_t>(std::max<int>(in.slot, 0));
        p.filter = enumAt<field::FieldImageFilter>(in, 0);
        p.edge = enumAt<field::FieldImageEdge>(in, 1);
        p.selector = static_cast<std::uint8_t>(choiceAt(in, 2));
        p.channel = static_cast<std::uint8_t>(choiceAt(in, 3, 4));
        p.first = static_cast<std::uint8_t>(choiceAt(in, 4, 0));
        p.second = static_cast<std::uint8_t>(choiceAt(in, 5, 1));
        return field::encodeImage(p);
    }
    case FieldOp::Force: {
        field::ForceParameters p;
        p.mode = enumAt<field::ForceMode>(in, 0);
        p.falloff = enumAt<field::ForceFalloff>(in, 1);
        p.clockwise = choiceAt(in, 2) != 0;
        p.strength = scalar(0, 1.0);
        p.radius = scalar(1, 200.0);
        p.angleDegrees = scalar(2);
        p.centreX = vector(0).x;
        p.centreY = vector(0).y;
        p.curve = curve;
        return field::encodeForce(p);
    }
    case FieldOp::Math: {
        field::MathParameters p;
        p.operation = enumAt<field::FieldMathOperation>(in, 0);
        p.smoothness = scalar(0, 0.1);
        p.angleDegrees = scalar(1);
        return field::encodeMath(p);
    }
    case FieldOp::Remap: {
        field::RemapParameters p;
        p.clamp = choiceAt(in, 0) != 0;
        p.invert = choiceAt(in, 1) != 0;
        p.steps = choiceAt(in, 2);
        p.vector = choiceAt(in, 3) != 0;
        p.inLow = scalar(0);
        p.inHigh = scalar(1, 1.0);
        p.outLow = scalar(2);
        p.outHigh = scalar(3, 1.0);
        p.bias = scalar(4, 0.5);
        p.gain = scalar(5, 0.5);
        p.curve = curve;
        return field::encodeRemap(p);
    }
    case FieldOp::Transform: {
        field::TransformParameters p;
        p.mode = enumAt<field::FieldCoordinateMode>(in, 0);
        p.segments = choiceAt(in, 1, 6);
        p.rotationDegrees = scalar(0);
        p.warpAmount = scalar(1);
        p.translateX = vector(0).x;
        p.translateY = vector(0).y;
        p.scaleX = vector(1, {1.0, 1.0}).x;
        p.scaleY = vector(1, {1.0, 1.0}).y;
        p.pivotX = vector(2).x;
        p.pivotY = vector(2).y;
        p.tileX = vector(3, {200.0, 200.0}).x;
        p.tileY = vector(3, {200.0, 200.0}).y;
        return field::encodeTransform(p);
    }
    case FieldOp::Swizzle: {
        field::SwizzleParameters p;
        for (std::size_t i = 0; i < 4; ++i) {
            p.lanes[i] = static_cast<std::uint8_t>(std::clamp<std::int64_t>(choiceAt(in, i, 4), 0, 5));
            p.scales[i] = scalar(i, 1.0);
        }
        return field::encodeSwizzle(p);
    }
    case FieldOp::LensWarp: {
        field::LensWarpParameters p;
        p.reverse = choiceAt(in, 0) != 0;
        p.k1 = scalar(0);
        p.k2 = scalar(1);
        p.p1 = scalar(2);
        p.p2 = scalar(3);
        p.scale = scalar(4, 1.0);
        // The centre is authored as an offset from the composition's centre with y down; a field
        // position has y up.
        p.centreX = vector(0).x;
        p.centreY = -vector(0).y;
        return field::encodeLensWarp(p);
    }
    case FieldOp::TestCard: {
        field::TestCardParameters p;
        p.card = choiceAt(in, 0);
        p.variant = choiceAt(in, 1);
        p.option = choiceAt(in, 2);
        const auto flags = choiceAt(in, 3);
        p.label = (flags & 1) != 0;
        const bool animated = (flags & 2) != 0;
        for (std::size_t i = 4; i < in.choices.size(); ++i)
            p.text.push_back(static_cast<std::uint8_t>(std::clamp<std::int64_t>(in.choices[i], 0, 255)));
        p.width = scalar(0, 1.0);
        p.height = scalar(1, 1.0);
        p.centreX = scalar(2);
        p.centreY = scalar(3);
        p.values = {scalar(4), scalar(5), scalar(6), scalar(7)};
        const auto fps = std::max(scalar(8, 24.0), 1.0);
        const auto frame = std::floor(std::max(seconds, 0.0) * fps + 1.0e-6);
        switch (p.card) {
        case 4: // Patch: one value of the set to a frame
            if (animated)
                p.variant = static_cast<std::int64_t>(std::fmod(frame, 27.0));
            break;
        case 11: // Zone Plate: two seconds to a turn of phase
            if (animated)
                p.timed[0] = std::fmod(std::max(seconds, 0.0) * 0.5, 1.0);
            break;
        case 13: { // Moving Bar: the share of its crossing done
            const auto duration = std::max(scalar(5, 2.0), 0.1);
            p.timed[0] = std::fmod(std::max(seconds, 0.0) / duration, 1.0);
            break;
        }
        case 14: { // Countdown
            const auto whole = std::max(std::round(fps), 1.0);
            const auto start = std::max(std::floor(scalar(4, 10.0)), 0.0);
            const auto elapsed = std::floor(frame / whole);
            const auto within = std::fmod(frame, whole);
            p.timed[1] = std::max(start - elapsed, 0.0);
            p.timed[2] = within;
            p.timed[3] = (within < 0.5 && elapsed <= start) ? 1.0 : 0.0;
            p.values[0] = whole;
            break;
        }
        default:
            break;
        }
        return field::encodeTestCard(p);
    }
    case FieldOp::Combine:
        return field::encodeConstant({scalar(0), scalar(1), scalar(2), scalar(3)});
    case FieldOp::Offset: {
        const auto step = scalar(0, 1.0);
        return field::encodeOffset(static_cast<double>(choiceAt(in, 0)) * step,
                                   static_cast<double>(choiceAt(in, 1)) * step);
    }
    case FieldOp::Derivative:
        return field::encodeDerivative(enumAt<field::FieldDerivativeMode>(in, 0), scalar(0, 1.0),
                                       scalar(1, 1.0));
    case FieldOp::CellCentre:
        return field::encodeCellCentre(scalar(0, 32.0));
    case FieldOp::Arrow:
        return field::encodeArrow(scalar(0, 32.0), scalar(1, 20.0), scalar(2, 1.0));
    case FieldOp::Streak:
        return field::encodeStreak(scalar(0, 1.0), scalar(1, 2.0));
    case FieldOp::Ramp:
        if (!ramp)
            return std::nullopt;
        return field::encodeRamp(scalar(0), scalar(1, 1.0), *ramp);
    }
    return std::nullopt;
}

std::optional<std::vector<float>> buildTable(const FieldProgram& program,
                                             const std::span<const double> values,
                                             const std::uint32_t samples) {
    if (values.empty() || program.instructions.empty() ||
        program.instructions.size() > field::kMaxFieldInstructions)
        return std::nullopt;
    const auto seconds = values.back();
    OperandReader reader(values.first(values.size() - 1U));
    field::FieldProgramBuilder builder;
    for (const auto& instruction : program.instructions) {
        if (instruction.op > static_cast<std::uint8_t>(FieldOp::TestCard))
            return std::nullopt;
        const auto block = encode(instruction, reader, seconds, program.compositionSeconds);
        if (!block)
            return std::nullopt;
        const field::FieldInputs inputs{instruction.inputs[0], instruction.inputs[1],
                                        instruction.inputs[2], instruction.inputs[3]};
        if (!builder.emit(static_cast<FieldOp>(instruction.op), instruction.output, inputs,
                          *block))
            return std::nullopt;
    }
    if (reader.position() != values.size() - 1U)
        return std::nullopt;
    auto table = builder.finish(program.result, static_cast<field::FieldResultKind>(program.kind),
                                static_cast<field::FieldOutputMode>(program.mode));
    if (!table)
        return std::nullopt;
    (*table)[field::kFieldTableSampling * 4U] =
        static_cast<float>(samples >= 16U ? 16U : (samples >= 4U ? 4U : 1U));
    return table;
}

} // namespace

std::optional<std::vector<float>> prepareFieldTable(const FieldProgram& program,
                                                    const std::span<const double> values,
                                                    const std::uint32_t samples) {
    auto table = buildTable(program, values, samples);
    if (!table || !field::validFieldTable(*table))
        return std::nullopt;
    return table;
}

std::optional<PreparedColorGrade> prepareFieldRasterGrade(const FieldRasterKernel& kernel,
                                                          const std::span<const double> values) {
    auto table = prepareFieldTable(kernel.program, values, kernel.samples);
    if (!table)
        return std::nullopt;
    return PreparedColorGrade{ColorGradeKind::FieldRaster, std::move(*table), false};
}

bool validFieldProgramData(const std::vector<float>& data) noexcept {
    return field::validFieldTable(data);
}

std::optional<std::vector<float>> fieldRasterProgramData(const PreparedColorGrade& grade,
                                                         const FieldRasterPlacement& placement) {
    if (grade.kind != ColorGradeKind::FieldRaster || grade.data.size() < field::kFieldTableCode * 4U)
        return std::nullopt;
    auto data = grade.data;
    const auto samples = static_cast<std::uint32_t>(data[field::kFieldTableSampling * 4U]);
    auto geometry = field::fieldRasterGeometry(
        placement.originX, placement.originY, placement.width, placement.height,
        placement.compositionWidth, placement.compositionHeight, placement.horizontalScale,
        placement.verticalScale, samples);
    geometry.targetX = placement.targetX;
    geometry.targetY = placement.targetY;
    for (std::size_t i = 0; i < placement.slots.size(); ++i) {
        const auto& slot = placement.slots[i];
        if (slot.width <= 0 || slot.height <= 0)
            continue;
        geometry.slots[i] = field::fieldImageSlot(
            slot.originX, slot.originY, slot.width, slot.height, placement.compositionWidth,
            placement.compositionHeight, placement.horizontalScale, placement.verticalScale);
        geometry.slots[i].originX = slot.atlasX;
        geometry.slots[i].originY = slot.atlasY;
    }
    if (!field::applyFieldRasterGeometry(data, geometry))
        return std::nullopt;
    // A mask Shape Distance reads its distance image in evaluation pixels: one texel is 1 / scale
    // field pixels.
    field::setFieldTexelScale(data, 1.0 / std::max(placement.horizontalScale, 1.0e-9));
    if (!field::validFieldTable(data))
        return std::nullopt;
    return data;
}

const std::string& fieldRasterProgramText() {
    static const std::string text = [] {
        std::string out =
            "layout(set = 0, binding = 0, std430) readonly buffer BloomFieldData { vec4 values[]; "
            "} bloom_field_data;\n"
            "precise float bloom_ocio_cpu_div(float a, float b);\n"
            "#define BFK_FN\n"
            "#define BFK_P precise\n"
            "#define BFK_OUT(T) out T\n"
            "#define BFK_INOUT(T) inout T\n"
            "#define BFK_CTX int bfk_context\n"
            "#define BFK_CTXA bfk_context\n"
            "vec4 bfk_data(int c, int i) { return bloom_field_data.values[i]; }\n"
            "vec4 bfk_texel(int c, int slot, int x, int y) {\n"
            " ivec2 size = imageSize(bloom_ocio_input);\n"
            " if (x < 0 || y < 0 || x >= size.x || y >= size.y) { return vec4(0.0); }\n"
            " return imageLoad(bloom_ocio_input, ivec2(x, y));\n"
            "}\n"
            "precise float bfk_div(float a, float b) { return bloom_ocio_cpu_div(a, b); }\n";
        out += field::fieldKernelGlsl();
        out += "\nvec4 bloom_field(ivec2 c) { return bfk_dispatch_pixel(0, c.x, c.y); }\n";
        return out;
    }();
    return text;
}

namespace {

struct SlotRows final {
    std::vector<std::span<const render::Rgba32f>> rows;
    std::int64_t width = 0;
};
struct SlotImages final {
    std::array<SlotRows, field::kMaxFieldImages> slots;
};

std::array<float, 4> fetchSlot(const void* context, const int slot, const int x, const int y) {
    const auto* images = static_cast<const SlotImages*>(context);
    if (slot < 0 || static_cast<std::size_t>(slot) >= images->slots.size())
        return {};
    const auto& rows = images->slots[static_cast<std::size_t>(slot)];
    if (y < 0 || static_cast<std::size_t>(y) >= rows.rows.size() || x < 0 || x >= rows.width)
        return {};
    const auto& p = rows.rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
    return {p.red(), p.green(), p.blue(), p.alpha()};
}

} // namespace

ImageEffectResult applyFieldRaster(const PreparedColorGrade& grade,
                                   const render::Rgba32fImage& canvas,
                                   const std::span<const render::Rgba32fImage* const> slots,
                                   const double compositionWidth, const double compositionHeight,
                                   const double horizontalScale, const double verticalScale,
                                   const std::size_t budget, const CancellationToken& cancellation,
                                   const OperationIndex operation,
                                   const EvaluationProgressCallback& progress,
                                   CpuRowBandExecutor* const rowBands) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    const auto& descriptor = *canvas.descriptor();
    const auto window = descriptor.dataWindow();
    FieldRasterPlacement placement;
    placement.originX = window.originX();
    placement.originY = window.originY();
    placement.width = static_cast<std::int64_t>(window.extent().width());
    placement.height = static_cast<std::int64_t>(window.extent().height());
    placement.compositionWidth = compositionWidth;
    placement.compositionHeight = compositionHeight;
    placement.horizontalScale = horizontalScale;
    placement.verticalScale = verticalScale;
    SlotImages images;
    std::vector<render::Rgba32fImageView> views;
    views.reserve(slots.size());
    for (std::size_t i = 0; i < slots.size() && i < field::kMaxFieldImages; ++i) {
        if (slots[i] == nullptr)
            continue;
        const auto view = slots[i]->view();
        if (!view)
            return failed(EvaluationDiagnosticCode::InvalidPixel, "Field image is invalid");
        views.push_back(*view.value());
        const auto data = slots[i]->descriptor()->dataWindow();
        auto& slot = placement.slots[i];
        slot.originX = data.originX();
        slot.originY = data.originY();
        slot.width = static_cast<std::int64_t>(data.extent().width());
        slot.height = static_cast<std::int64_t>(data.extent().height());
        images.slots[i].width = slot.width;
        for (auto y = data.originY(); y < data.maxYExclusive(); ++y) {
            const auto row = views.back().row(y);
            if (!row)
                return failed(EvaluationDiagnosticCode::InternalInvariant,
                              "Field image row is unavailable");
            images.slots[i].rows.push_back(*row.value());
        }
    }
    const auto data = fieldRasterProgramData(grade, placement);
    if (!data)
        return failed(EvaluationDiagnosticCode::InvalidParameter,
                      "Field raster geometry cannot be represented");
    auto builder = render::Rgba32fImageBuilder::create(descriptor, budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Field raster output exceeds the pixel budget");
    auto& image = *builder.value();
    const field::FieldImages fetch{&images, &fetchSlot};
    const auto rows = static_cast<std::uint32_t>(window.extent().height());
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, rows});
    const auto outcome = runRowBandPass(
        rowBands, cancellation, rows, window.originY(),
        [&](const std::int64_t y) -> std::optional<EvaluationDiagnostic> {
            auto row = image.row(y);
            if (!row)
                return EvaluationDiagnostic{EvaluationDiagnosticCode::InternalInvariant,
                                            DiagnosticSeverity::Error,
                                            {},
                                            "Field raster row is unavailable",
                                            {}};
            const auto j = static_cast<int>(y - window.originY());
            for (std::size_t column = 0; column < row.value()->size(); ++column) {
                if ((column & 1023U) == 0 && cancellation.isCancellationRequested())
                    return std::nullopt;
                const auto p = field::fieldPixel(*data, fetch, static_cast<int>(column), j);
                const auto pixel = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
                if (!pixel)
                    return EvaluationDiagnostic{EvaluationDiagnosticCode::InvalidPixel,
                                                DiagnosticSeverity::Error,
                                                {},
                                                "Field raster produced an invalid pixel",
                                                {}};
                (*row.value())[column] = *pixel.value();
            }
            return std::nullopt;
        });
    if (outcome.cancelled || cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    if (outcome.incomplete || outcome.failure) {
        ImageEffectResult result;
        result.diagnostic = outcome.failure ? *outcome.failure
                                            : EvaluationDiagnostic{
                                                  EvaluationDiagnosticCode::AllocationFailure,
                                                  DiagnosticSeverity::Error,
                                                  {},
                                                  "A field raster row band could not complete",
                                                  {}};
        return result;
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, rows, rows});
    auto frozen = std::move(image).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel,
                      "Field raster image publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

std::optional<std::array<float, 4>> sampleFieldProgram(const FieldProgram& program,
                                                       const std::span<const double> values,
                                                       const double x, const double y) {
    const auto table = prepareFieldTable(program, values, 1);
    if (!table || !std::isfinite(x) || !std::isfinite(y))
        return std::nullopt;
    // A sample reads no image: every slot is absent, so an Image Field reads transparent.
    return field::evaluateField(*table, field::FieldImages{}, static_cast<float>(x),
                                static_cast<float>(y));
}

// --- Mask distance ----------------------------------------------------------------------------

std::int64_t maskDistanceReach(const MaskDistanceKernel& kernel, const double horizontalScale,
                               const double verticalScale) noexcept {
    const auto scale = std::max(horizontalScale, verticalScale);
    const auto reach = std::ceil(std::max(kernel.reach, 0.0) * scale) + 2.0;
    if (!std::isfinite(reach))
        return kMaximumMaskDistanceReach;
    return std::clamp<std::int64_t>(static_cast<std::int64_t>(reach), 2,
                                    kMaximumMaskDistanceReach);
}

std::optional<render::ImageWindow> maskDistanceWindow(const render::ImageWindow input,
                                                      const std::int64_t reach) {
    const auto window = render::ImageWindow::create(
        input.originX() - reach, input.originY() - reach,
        input.extent().width() + 2U * static_cast<std::uint64_t>(reach),
        input.extent().height() + 2U * static_cast<std::uint64_t>(reach));
    if (!window)
        return std::nullopt;
    return *window.value();
}

ImageEffectResult applyMaskDistance(const MaskDistanceKernel& kernel,
                                    const render::Rgba32fImage& mask,
                                    const double horizontalScale, const double verticalScale,
                                    const std::size_t budget,
                                    const CancellationToken& cancellation,
                                    const OperationIndex operation,
                                    const EvaluationProgressCallback& progress) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    const auto reach = maskDistanceReach(kernel, horizontalScale, verticalScale);
    const auto input = mask.descriptor()->dataWindow();
    const auto output = maskDistanceWindow(input, reach);
    if (!output)
        return failed(EvaluationDiagnosticCode::InvalidParameter,
                      "Mask distance window cannot be represented");
    const auto width = static_cast<std::size_t>(output->extent().width());
    const auto height = static_cast<std::size_t>(output->extent().height());
    if (width * height > budget / sizeof(float) / 4U)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Mask distance exceeds the pixel budget");
    const auto view = mask.view();
    if (!view)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Mask distance input is invalid");
    const auto cap = static_cast<float>((reach + 1) * (reach + 1));
    // f_in: 0 on inside texels (coverage at least 1/2), the cap elsewhere; f_out the reverse.
    std::vector<float> in(width * height, cap);
    std::vector<float> out(width * height, 0.0F);
    for (auto y = input.originY(); y < input.maxYExclusive(); ++y) {
        const auto row = view.value()->row(y);
        if (!row)
            return failed(EvaluationDiagnosticCode::InternalInvariant,
                          "Mask distance row is unavailable");
        const auto oy = static_cast<std::size_t>(y - output->originY());
        for (std::size_t column = 0; column < row.value()->size(); ++column) {
            const auto ox = static_cast<std::size_t>(input.originX() - output->originX()) + column;
            if ((*row.value())[column].alpha() >= 0.5F) {
                in[oy * width + ox] = 0.0F;
                out[oy * width + ox] = cap;
            }
        }
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, 2});
    const auto longest = std::max(width, height);
    std::vector<float> f(longest);
    std::vector<float> d(longest);
    std::vector<int> vertices(longest);
    std::vector<double> bounds(longest + 1U);
    for (auto* grid : {&in, &out}) {
        for (std::size_t x = 0; x < width; ++x) {
            if ((x & 63U) == 0 && cancellation.isCancellationRequested())
                return ImageEffectResult{{}, {}, true};
            for (std::size_t y = 0; y < height; ++y)
                f[y] = (*grid)[y * width + x];
            field::squaredDistance1d(std::span(f).first(height), std::span(d).first(height),
                                     vertices, bounds, cap);
            for (std::size_t y = 0; y < height; ++y)
                (*grid)[y * width + x] = d[y];
        }
        for (std::size_t y = 0; y < height; ++y) {
            if ((y & 63U) == 0 && cancellation.isCancellationRequested())
                return ImageEffectResult{{}, {}, true};
            std::copy_n(grid->begin() + static_cast<std::ptrdiff_t>(y * width), width, f.begin());
            field::squaredDistance1d(std::span(f).first(width), std::span(d).first(width),
                                     vertices, bounds, cap);
            std::copy_n(d.begin(), width, grid->begin() + static_cast<std::ptrdiff_t>(y * width));
        }
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 1, 2});
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *output, mask.descriptor()->displayWindow(), mask.descriptor()->pixelAspect());
    if (!descriptor)
        return failed(EvaluationDiagnosticCode::InvalidParameter,
                      "Mask distance descriptor is invalid");
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Mask distance output exceeds the pixel budget");
    for (std::size_t y = 0; y < height; ++y) {
        auto row = builder.value()->row(output->originY() + static_cast<std::int64_t>(y));
        if (!row)
            return failed(EvaluationDiagnosticCode::InternalInvariant,
                          "Mask distance row is unavailable");
        for (std::size_t x = 0; x < width; ++x) {
            const auto toInside = in[y * width + x];
            const auto toOutside = out[y * width + x];
            // Signed, half a texel from each texel centre to the boundary between them.
            const auto sd = toInside > 0.0F ? std::sqrt(toInside) - 0.5F
                                            : 0.5F - std::sqrt(toOutside);
            const auto pixel = render::Rgba32f::fromPremultiplied(sd, sd, sd, 1.0F);
            if (!pixel)
                return failed(EvaluationDiagnosticCode::InvalidPixel,
                              "Mask distance produced an invalid pixel");
            (*row.value())[x] = *pixel.value();
        }
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 2, 2});
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel,
                      "Mask distance image publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

render::OcioGpuProgramResult buildMaskDistanceProgram(const std::int64_t reach, const int pass) {
    if (reach < 1 || reach > kMaximumMaskDistanceReach || pass < 0 || pass > 1)
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    // The bounded search finds the CPU's exact minimum (docs/architecture/fields.md): every
    // candidate beyond reach + 1 texels already exceeds the cap. Integer squared distances below
    // 2^24 are exact in binary32 on both routes.
    static const std::string text =
        "layout(set = 0, binding = 0, std430) readonly buffer BloomMaskDistanceData { vec4 "
        "values[]; } bloom_mask_distance_data;\n"
        "vec4 bloom_mask_distance_tap(ivec2 c) {\n"
        " ivec2 size = imageSize(bloom_ocio_input);\n"
        " if (c.x < 0 || c.y < 0 || c.x >= size.x || c.y >= size.y) { return vec4(0.0); }\n"
        " return imageLoad(bloom_ocio_input, c);\n"
        "}\n"
        "vec4 bloom_mask_distance(ivec2 c) {\n"
        " vec4 k = bloom_mask_distance_data.values[0];\n"
        " int reach = int(k.x) + 1;\n"
        " precise float cap = k.y;\n"
        " precise float toInside = cap;\n"
        " precise float toOutside = cap;\n"
        " if (k.z == 0.0) {\n"
        "  for (int d = -reach; d <= reach; ++d) {\n"
        "   vec4 t = bloom_mask_distance_tap(ivec2(c.x, c.y + d));\n"
        "   precise float dd = float(d * d);\n"
        "   if (t.a >= 0.5) { toInside = min(toInside, dd); } else { toOutside = min(toOutside, "
        "dd); }\n"
        "  }\n"
        "  return vec4(toInside, toOutside, 0.0, 1.0);\n"
        " }\n"
        " for (int d = -reach; d <= reach; ++d) {\n"
        "  ivec2 p = ivec2(c.x + d, c.y);\n"
        "  ivec2 size = imageSize(bloom_ocio_input);\n"
        "  precise float dd = float(d * d);\n"
        "  vec4 t = (p.x < 0 || p.x >= size.x) ? vec4(cap, 0.0, 0.0, 1.0) : imageLoad(bloom_ocio_input, p);\n"
        "  precise float a = dd + t.x;\n"
        "  precise float b = dd + t.y;\n"
        "  toInside = min(toInside, a);\n"
        "  toOutside = min(toOutside, b);\n"
        " }\n"
        " toInside = min(toInside, cap);\n"
        " toOutside = min(toOutside, cap);\n"
        " precise float sd = toInside > 0.0 ? sqrt(toInside) - 0.5 : 0.5 - sqrt(toOutside);\n"
        " return vec4(sd, sd, sd, 1.0);\n"
        "}\n";
    const std::array<float, 4> data{static_cast<float>(reach),
                                    static_cast<float>((reach + 1) * (reach + 1)),
                                    static_cast<float>(pass), 0.0F};
    render::OcioGpuProgramDesc program;
    program.stage = render::OcioGpuProgramStage::ProcessNeighborhood;
    program.functionName = "bloom_mask_distance";
    program.resourcePrefix = "bloom_mask_distance_";
    program.semanticsId = "bloom.render.mask-distance.v1";
    program.shaderText = text;
    program.uniformBufferStorage = true;
    program.uniformBufferSize = data.size() * sizeof(float);
    program.uniformBufferData.resize(data.size() * sizeof(float));
    std::memcpy(program.uniformBufferData.data(), data.data(), data.size() * sizeof(float));
    program.shaderTextDigest = render::computeOcioGpuShaderTextDigest(program.shaderText);
    program.resourceDigest = render::computeOcioGpuResourceDigest(program);
    program.contentIdentity = render::computeOcioGpuContentIdentity(
        {program.semanticsId, {}, {}, program.shaderTextDigest, program.resourceDigest});
    return render::OcioGpuProgramResult::success(std::move(program));
}

} // namespace bloom::runtime::detail
