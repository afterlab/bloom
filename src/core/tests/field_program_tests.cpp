#include <bloom/core/field_program.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <source_location>
#include <span>
#include <string_view>
#include <vector>

namespace {

namespace field = bloom::core::field;
using field::FieldOp;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

constexpr field::FieldImages kNoImages{};

// A one-generator program: r0 = coordinate, r1 = op(r0).
std::vector<float> single(const FieldOp op, const std::vector<float>& parameters,
                          const field::FieldResultKind kind = field::FieldResultKind::Scalar) {
    field::FieldProgramBuilder builder;
    builder.emit(FieldOp::Coordinate, 0, field::kNoFieldInputs, {});
    builder.emit(op, 1, {0, -1, -1, -1}, parameters);
    auto table = builder.finish(1, kind, field::FieldOutputMode::Raw);
    return table.value_or(std::vector<float>{});
}

std::array<double, 4> at(const std::vector<float>& table, const double x, const double y) {
    const auto v =
        field::evaluateField(table, kNoImages, static_cast<float>(x), static_cast<float>(y));
    return {static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]),
            static_cast<double>(v[3])};
}

void testBuilderAndValidation(Expectations& e) {
    const auto table = single(FieldOp::Constant, field::encodeConstant({1.0, 2.0, 3.0, 4.0}));
    e.expect(field::validFieldTable(table), "a built table is valid");
    const auto v = at(table, 0.0, 0.0);
    e.expect(v[0] == 1.0 && v[3] == 4.0, "a constant reads its block");

    auto corrupt = table;
    corrupt[0] = 1.0F;
    e.expect(!field::validFieldTable(corrupt), "a wrong magic is rejected");
    corrupt = table;
    corrupt[(field::kFieldTableCode + 2U) * 4U] = 99.0F;
    e.expect(!field::validFieldTable(corrupt), "an unknown op is rejected");
    corrupt = table;
    corrupt[(field::kFieldTableCode + 3U) * 4U + 2U] = 1.0e6F;
    e.expect(!field::validFieldTable(corrupt), "a block beyond the table is rejected");
    corrupt = table;
    corrupt.back() = std::numeric_limits<float>::infinity();
    e.expect(!field::validFieldTable(corrupt), "a non-finite value is rejected");

    field::FieldProgramBuilder full;
    for (std::size_t i = 0; i < field::kMaxFieldInstructions; ++i)
        e.expect(full.emit(FieldOp::Coordinate, 0, field::kNoFieldInputs, {}), "within the bound");
    e.expect(!full.emit(FieldOp::Coordinate, 0, field::kNoFieldInputs, {}),
             "the 65th instruction is refused");
    field::FieldProgramBuilder registers;
    e.expect(!registers.emit(FieldOp::Coordinate, 16, field::kNoFieldInputs, {}),
             "a register beyond sixteen is refused");
    e.expect(!field::fieldKernelGlsl().empty() &&
                 field::fieldKernelGlsl().find("bfk_dispatch_pixel") != std::string_view::npos,
             "the kernel text is embedded");
}

field::NoiseParameters noise(const field::NoiseType type, const field::NoiseFractal fractal) {
    field::NoiseParameters p;
    p.type = type;
    p.fractal = fractal;
    p.seed = 7;
    p.sizeX = 64.0;
    p.sizeY = 64.0;
    p.octaves = 4.5;
    return p;
}

void testNoise(Expectations& e) {
    using field::NoiseFractal;
    using field::NoiseType;
    const std::array types{NoiseType::Perlin, NoiseType::Simplex, NoiseType::Linear,
                           NoiseType::SoftLinear, NoiseType::Block, NoiseType::Spline};
    const std::array fractals{NoiseFractal::None,       NoiseFractal::Fbm,
                              NoiseFractal::Turbulence, NoiseFractal::Ridged,
                              NoiseFractal::Billow,     NoiseFractal::Dynamic,
                              NoiseFractal::DynamicProgressive, NoiseFractal::DynamicTwist};
    for (const auto type : types) {
        for (const auto fractal : fractals) {
            for (const auto flow : {false, true}) {
                auto p = noise(type, fractal);
                p.flow = flow;
                p.evolution = 0.3;
                p.overflow = field::NoiseOverflow::Clip;
                const auto encoded = field::encodeNoise(p);
                e.expect(encoded.has_value(), "every noise encodes");
                if (!encoded)
                    continue;
                const auto table = single(FieldOp::Noise, *encoded);
                e.expect(field::validFieldTable(table), "every noise table is valid");
                double low = 1.0;
                double high = 0.0;
                bool inRange = true;
                for (int j = 0; j < 24; ++j) {
                    for (int i = 0; i < 24; ++i) {
                        const auto v = at(table, i * 13.7 - 160.0, j * 11.3 - 140.0)[0];
                        inRange = inRange && v >= 0.0 && v <= 1.0 && std::isfinite(v);
                        low = std::min(low, v);
                        high = std::max(high, v);
                    }
                }
                e.expect(inRange, "Clip keeps a noise in [0, 1]");
                e.expect(high - low > 0.05, "a noise varies across the field");
            }
        }
    }

    // The analytic gradient (Quilez, "noise derivatives") agrees with central differences
    // wherever the noise is smooth.
    for (const auto type : {NoiseType::Perlin, NoiseType::Simplex, NoiseType::Linear,
                            NoiseType::SoftLinear, NoiseType::Spline}) {
        auto p = noise(type, NoiseFractal::Fbm);
        p.octaves = 3.0;
        p.rotationDegrees = 30.0;
        p.evolution = 0.4;
        const auto table = single(FieldOp::Noise, *field::encodeNoise(p));
        int agree = 0;
        int total = 0;
        for (int k = 0; k < 40; ++k) {
            const auto x = 17.31 * k - 300.0;
            const auto y = 9.77 * k - 150.0;
            const auto v = at(table, x, y);
            const double h = 0.01;
            const auto dx = (at(table, x + h, y)[0] - at(table, x - h, y)[0]) / (2.0 * h);
            const auto dy = (at(table, x, y + h)[0] - at(table, x, y - h)[0]) / (2.0 * h);
            ++total;
            const auto scale = std::max(1.0e-3, std::hypot(dx, dy));
            if (std::fabs(v[1] - dx) <= 0.05 * scale + 1.0e-4 &&
                std::fabs(v[2] - dy) <= 0.05 * scale + 1.0e-4)
                ++agree;
        }
        e.expect(agree >= total - 2, "the analytic gradient matches central differences");
    }

    // Determinism, seeds and decorrelated streams.
    auto p = noise(NoiseType::Perlin, NoiseFractal::Fbm);
    const auto a = single(FieldOp::Noise, *field::encodeNoise(p));
    e.expect(at(a, 12.5, -7.25)[0] == at(a, 12.5, -7.25)[0], "a noise is deterministic");
    p.seed = 8;
    const auto b = single(FieldOp::Noise, *field::encodeNoise(p));
    p.seed = 7;
    p.stream = 1;
    const auto c = single(FieldOp::Noise, *field::encodeNoise(p));
    int differs = 0;
    for (int k = 0; k < 16; ++k) {
        const auto x = 31.0 * k;
        differs += at(a, x, 3.0)[0] != at(b, x, 3.0)[0] ? 1 : 0;
        differs += at(a, x, 3.0)[0] != at(c, x, 3.0)[0] ? 1 : 0;
    }
    e.expect(differs == 32, "a seed or stream gives another noise");

    // #647 the vector output: one instruction holding both streams is (stream 0, stream 1, 0, 0),
    // exactly the two noises read separately, from a valid table; a table cut short of the second
    // block is not valid.
    {
        auto paired = noise(NoiseType::Perlin, NoiseFractal::Fbm);
        paired.paired = true;
        const auto both = single(FieldOp::Noise, *field::encodeNoise(paired), field::FieldResultKind::Vector);
        e.expect(field::validFieldTable(both), "a paired noise is a valid table");
        bool same = true;
        for (int k = 0; k < 24 && same; ++k) {
            const auto x = 17.0 * k - 90.0;
            const auto y = 9.5 * k - 40.0;
            const auto v = at(both, x, y);
            same = v[0] == at(a, x, y)[0] && v[1] == at(c, x, y)[0] && v[2] == 0.0 && v[3] == 0.0;
        }
        e.expect(same, "a paired noise is the noise on stream 0 and the noise on stream 1, exactly");
        auto cut = *field::encodeNoise(paired);
        cut.resize(cut.size() / 2);
        e.expect(!field::validFieldTable(single(FieldOp::Noise, cut, field::FieldResultKind::Vector)),
                 "a paired noise cut short of its second block is refused");
    }

    // A loop of N cycles repeats exactly: phase 0 and phase N are one table.
    for (const auto type : {NoiseType::Perlin, NoiseType::Simplex, NoiseType::Spline}) {
        auto loop = noise(type, NoiseFractal::Dynamic);
        std::int64_t period = 0;
        const auto start = field::noiseEvolutionPhase(3.0, true, 2.0, 0.0, period);
        loop.loopPeriod = period;
        loop.evolution = start;
        const auto first = single(FieldOp::Noise, *field::encodeNoise(loop));
        const auto end = field::noiseEvolutionPhase(3.0, true, 2.0, 2.0, period);
        loop.evolution = end;
        const auto again = single(FieldOp::Noise, *field::encodeNoise(loop));
        e.expect(period == 3 && first == again, "a looping noise repeats at its loop length");
        loop.evolution = 2.9999;
        const auto late = single(FieldOp::Noise, *field::encodeNoise(loop));
        e.expect(std::fabs(at(late, 40.0, 40.0)[0] - at(first, 40.0, 40.0)[0]) < 0.02,
                 "a looping noise is continuous across the loop");
        loop.evolution = 1.5;
        const auto mid = single(FieldOp::Noise, *field::encodeNoise(loop));
        e.expect(at(mid, 40.0, 40.0)[0] != at(first, 40.0, 40.0)[0], "a loop evolves");
    }

    // A large offset is split into whole lattice cells: no precision is lost far from the origin.
    auto near = noise(NoiseType::Perlin, NoiseFractal::Fbm);
    auto far = near;
    far.offsetX = 64.0 * 1.0e6;
    const auto nearTable = single(FieldOp::Noise, *field::encodeNoise(near));
    const auto farTable = single(FieldOp::Noise, *field::encodeNoise(far));
    const auto nv = at(nearTable, 10.5, 3.25)[0];
    const auto fv = at(farTable, 10.5, 3.25)[0];
    e.expect(std::isfinite(fv) && fv >= 0.0 && nv != fv, "a far offset stays exact");

    // Overflow modes.
    auto hot = noise(NoiseType::Perlin, NoiseFractal::Fbm);
    hot.contrast = 4.0;
    hot.overflow = field::NoiseOverflow::AllowHdr;
    const auto hdr = single(FieldOp::Noise, *field::encodeNoise(hot));
    hot.overflow = field::NoiseOverflow::WrapBack;
    const auto wrap = single(FieldOp::Noise, *field::encodeNoise(hot));
    hot.overflow = field::NoiseOverflow::SoftClamp;
    const auto soft = single(FieldOp::Noise, *field::encodeNoise(hot));
    bool beyond = false;
    bool wrapped = true;
    bool softened = true;
    for (int k = 0; k < 200; ++k) {
        const auto x = 7.3 * k;
        const auto y = 5.1 * k;
        const auto h = at(hdr, x, y)[0];
        beyond = beyond || h > 1.0 || h < 0.0;
        const auto w = at(wrap, x, y)[0];
        wrapped = wrapped && w >= 0.0 && w <= 1.0;
        const auto s = at(soft, x, y)[0];
        softened = softened && s > 0.0 && s < 1.0;
    }
    e.expect(beyond, "Allow HDR Results passes values outside [0, 1]");
    e.expect(wrapped, "Wrap Back folds into [0, 1]");
    e.expect(softened, "Soft Clamp stays strictly inside (0, 1)");
}

void testVoronoi(Expectations& e) {
    field::VoronoiParameters p;
    p.size = 50.0;
    p.jitter = 0.0;
    p.edges = true;
    const auto table = single(FieldOp::Voronoi, *field::encodeVoronoi(p));
    bool exact = true;
    for (int k = 0; k < 60; ++k) {
        const auto x = 7.9 * k - 211.0;
        const auto y = 4.3 * k - 97.0;
        // With no jitter every feature point is its cell's centre.
        const auto u = x / 50.0;
        const auto v = y / 50.0;
        const auto du = u - (std::floor(u) + 0.5);
        const auto dv = v - (std::floor(v) + 0.5);
        const auto f1 = std::hypot(du, dv);
        const auto edge = 0.5 - std::max(std::fabs(du), std::fabs(dv));
        const auto r = at(table, x, y);
        exact = exact && std::fabs(r[0] - f1) < 2.0e-5 && std::fabs(r[2] - edge) < 2.0e-5;
    }
    e.expect(exact, "jitter 0 gives the exact distance to the cell centre and its edge");

    p.jitter = 1.0;
    p.seed = 3;
    p.mode = field::VoronoiMode::F2MinusF1;
    const auto f21 = single(FieldOp::Voronoi, *field::encodeVoronoi(p));
    p.mode = field::VoronoiMode::SmoothF1;
    p.smoothness = 10.0;
    const auto smooth = single(FieldOp::Voronoi, *field::encodeVoronoi(p));
    p.mode = field::VoronoiMode::F1;
    const auto f1 = single(FieldOp::Voronoi, *field::encodeVoronoi(p));
    bool ordered = true;
    for (int k = 0; k < 60; ++k) {
        const auto x = 11.0 * k;
        ordered = ordered && at(f21, x, 2.0)[0] >= 0.0 &&
                  at(smooth, x, 2.0)[0] <= at(f1, x, 2.0)[0] + 1.0e-5;
    }
    e.expect(ordered, "F2 - F1 is non-negative and the smooth minimum lies below F1");
}

void testGenerators(Expectations& e) {
    field::RandomParameters r;
    r.cellX = 10.0;
    r.cellY = 10.0;
    const auto random = single(FieldOp::Random, *field::encodeRandom(r));
    double sum = 0.0;
    bool unit = true;
    for (int j = 0; j < 40; ++j) {
        for (int i = 0; i < 40; ++i) {
            const auto v = at(random, i * 10.0 + 5.0, j * 10.0 + 5.0)[0];
            unit = unit && v >= 0.0 && v < 1.0;
            sum += v;
        }
    }
    e.expect(unit, "Random lies in [0, 1)");
    e.expect(std::fabs(sum / 1600.0 - 0.5) < 0.03, "Random is uniform about 1/2");
    e.expect(at(random, 1.0, 1.0)[0] == at(random, 9.0, 9.0)[0], "a cell holds one value");

    field::PatternParameters pattern;
    pattern.size = 40.0;
    pattern.softness = 0.0;
    const auto stripes = single(FieldOp::Pattern, *field::encodePattern(pattern));
    double worst = 0.0;
    for (int k = 0; k < 400; ++k) {
        const auto x = 0.37 * k - 70.0;
        const auto expected = 0.5 + 0.5 * std::sin(2.0 * std::numbers::pi * x / 40.0);
        worst = std::max(worst, std::fabs(at(stripes, x, 0.0)[0] - expected));
    }
    e.expect(worst < 1.0e-6, "the sine stripes are Bloom's sine to 1e-6");
    pattern.waveform = field::PatternWaveform::Square;
    pattern.softness = 40.0;
    const auto blurred = single(FieldOp::Pattern, *field::encodePattern(pattern));
    e.expect(std::fabs(at(blurred, 3.0, 0.0)[0] - 0.5) < 1.0e-5,
             "a square wave box-filtered over one period is its duty");

    field::GradientFieldParameters g;
    g.mode = field::GradientFieldMode::Angular;
    g.startX = 0.0;
    g.endX = 100.0;
    const auto angular = single(FieldOp::Gradient, *field::encodeGradient(g));
    worst = 0.0;
    for (int k = 0; k < 360; ++k) {
        const auto a = (k + 0.5) / 360.0 * 2.0 * std::numbers::pi;
        const auto t = at(angular, 50.0 * std::cos(a), 50.0 * std::sin(a))[0];
        worst = std::max(worst, std::fabs(t - (k + 0.5) / 360.0));
    }
    e.expect(worst < 2.0e-6, "the angular gradient is Bloom's atan2 to 2e-6 turns");
    g.mode = field::GradientFieldMode::Linear;
    const auto linear = single(FieldOp::Gradient, *field::encodeGradient(g));
    e.expect(std::fabs(at(linear, 25.0, 80.0)[0] - 0.25) < 1.0e-6, "a linear gradient");

    field::ShapeParameters shape;
    shape.primitive = field::ShapePrimitive::Box;
    shape.sizeX = 100.0;
    shape.sizeY = 50.0;
    shape.falloff = 10.0;
    const auto box = single(FieldOp::Shape, *field::encodeShape(shape));
    e.expect(std::fabs(at(box, 60.0, 0.0)[0] - 10.0) < 1.0e-4, "a box's outside distance");
    e.expect(std::fabs(at(box, 0.0, 20.0)[0] + 5.0) < 1.0e-4, "a box's inside distance");
    e.expect(std::fabs(at(box, 55.0, 0.0)[1] - 0.5) < 1.0e-4, "a linear falloff");
    e.expect(std::fabs(at(box, 60.0, 0.0)[2] - 1.0) < 1.0e-3, "the direction points away");
    shape.primitive = field::ShapePrimitive::Polygon;
    shape.sides = 4;
    shape.sizeX = 100.0;
    const auto square = single(FieldOp::Shape, *field::encodeShape(shape));
    // A square of circumradius 50: its edges lie at 50 cos(45 degrees).
    e.expect(std::fabs(at(square, 50.0, 0.0)[0]) < 1.0e-4, "a polygon's vertex");
    e.expect(std::fabs(at(square, 0.0, 0.0)[0] + 50.0 * std::numbers::sqrt2 / 2.0) < 1.0e-3,
             "a polygon's inside distance");

    field::ForceParameters force;
    force.radius = 100.0;
    const auto vortex = single(FieldOp::Force, *field::encodeForce(force),
                               field::FieldResultKind::Vector);
    const auto v = at(vortex, 50.0, 0.0);
    e.expect(std::fabs(v[0]) < 1.0e-6 && std::fabs(v[1] - 0.5) < 1.0e-6,
             "a counter-clockwise vortex turns +x towards +y");
}

std::vector<float> binary(const FieldOp op, const std::vector<float>& parameters,
                          const std::array<double, 4> a, const std::array<double, 4> b) {
    field::FieldProgramBuilder builder;
    builder.emit(FieldOp::Constant, 0, field::kNoFieldInputs, field::encodeConstant(a));
    builder.emit(FieldOp::Constant, 1, field::kNoFieldInputs, field::encodeConstant(b));
    builder.emit(op, 2, {0, 1, -1, -1}, parameters);
    return builder.finish(2, field::FieldResultKind::Scalar, field::FieldOutputMode::Raw)
        .value_or(std::vector<float>{});
}

void testMathAndRemap(Expectations& e) {
    field::MathParameters power;
    power.operation = field::FieldMathOperation::Power;
    double worst = 0.0;
    for (int k = 1; k < 200; ++k) {
        const auto base = 0.013 * k;
        const auto exponent = 0.5 + 0.017 * k;
        const auto table = binary(FieldOp::Math, *field::encodeMath(power), {base, 0, 0, 0},
                                  {exponent, 0, 0, 0});
        const auto expected = std::pow(static_cast<double>(static_cast<float>(base)),
                                       static_cast<double>(static_cast<float>(exponent)));
        worst = std::max(worst, std::fabs(at(table, 0, 0)[0] - expected) / expected);
    }
    e.expect(worst < 4.0e-6, "Bloom's pow is accurate to 4e-6 relative");

    field::MathParameters smin;
    smin.operation = field::FieldMathOperation::SmoothMin;
    smin.smoothness = 0.1;
    const auto s = binary(FieldOp::Math, *field::encodeMath(smin), {0.3, 0, 0, 0}, {0.3, 0, 0, 0});
    e.expect(at(s, 0, 0)[0] < 0.3, "the smooth minimum lies below equal operands");

    field::RemapParameters remap;
    remap.inLow = 0.0;
    remap.inHigh = 2.0;
    remap.outLow = 10.0;
    remap.outHigh = 20.0;
    field::FieldProgramBuilder builder;
    builder.emit(FieldOp::Constant, 0, field::kNoFieldInputs, field::encodeConstant({1.5, 0, 0, 0}));
    builder.emit(FieldOp::Remap, 1, {0, -1, -1, -1}, *field::encodeRemap(remap));
    const auto mapped = *builder.finish(1, field::FieldResultKind::Scalar,
                                        field::FieldOutputMode::Raw);
    e.expect(std::fabs(at(mapped, 0, 0)[0] - 17.5) < 1.0e-5, "a linear remap");
    remap.steps = 4;
    field::FieldProgramBuilder stepped;
    stepped.emit(FieldOp::Constant, 0, field::kNoFieldInputs, field::encodeConstant({1.5, 0, 0, 0}));
    stepped.emit(FieldOp::Remap, 1, {0, -1, -1, -1}, *field::encodeRemap(remap));
    const auto steps = *stepped.finish(1, field::FieldResultKind::Scalar,
                                       field::FieldOutputMode::Raw);
    e.expect(std::fabs(at(steps, 0, 0)[0] - 20.0) < 1.0e-5, "four steps quantize");

    // Transform: a translated gradient moves with it; a derivative of a linear gradient is flat.
    field::GradientFieldParameters g;
    g.startX = 0.0;
    g.endX = 100.0;
    field::TransformParameters t;
    t.translateX = 25.0;
    field::FieldProgramBuilder moved;
    moved.emit(FieldOp::Coordinate, 0, field::kNoFieldInputs, {});
    moved.emit(FieldOp::Transform, 1, {0, -1, -1, -1}, *field::encodeTransform(t));
    moved.emit(FieldOp::Gradient, 2, {1, -1, -1, -1}, *field::encodeGradient(g));
    const auto shifted = *moved.finish(2, field::FieldResultKind::Scalar,
                                       field::FieldOutputMode::Raw);
    e.expect(std::fabs(at(shifted, 75.0, 0.0)[0] - 0.5) < 1.0e-6, "a translated field moves");

    field::FieldProgramBuilder derivative;
    derivative.emit(FieldOp::Coordinate, 0, field::kNoFieldInputs, {});
    derivative.emit(FieldOp::Offset, 1, {0, -1, -1, -1}, field::encodeOffset(1.0, 0.0));
    derivative.emit(FieldOp::Offset, 2, {0, -1, -1, -1}, field::encodeOffset(-1.0, 0.0));
    derivative.emit(FieldOp::Offset, 3, {0, -1, -1, -1}, field::encodeOffset(0.0, 1.0));
    derivative.emit(FieldOp::Offset, 4, {0, -1, -1, -1}, field::encodeOffset(0.0, -1.0));
    g.endX = 50.0;
    for (int k = 1; k <= 4; ++k)
        derivative.emit(FieldOp::Gradient, 4 + k, {k, -1, -1, -1}, *field::encodeGradient(g));
    derivative.emit(FieldOp::Derivative, 9, {5, 6, 7, 8},
                    field::encodeDerivative(field::FieldDerivativeMode::Gradient, 1.0, 1.0));
    const auto slope = *derivative.finish(9, field::FieldResultKind::Vector,
                                          field::FieldOutputMode::Raw);
    const auto d = at(slope, 20.0, 5.0);
    e.expect(std::fabs(d[0] - 0.02) < 1.0e-6 && std::fabs(d[1]) < 1.0e-6,
             "the central difference of a linear gradient is its slope");
}

struct TestImage final {
    int width = 0;
    int height = 0;
    std::vector<std::array<float, 4>> texels;
};

std::array<float, 4> fetchTest(const void* context, int /*slot*/, const int x, const int y) {
    const auto* image = static_cast<const TestImage*>(context);
    return image->texels[static_cast<std::size_t>(y * image->width + x)];
}

void testImagesAndRaster(Expectations& e) {
    TestImage image{2, 1, {{0.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}}};
    const field::FieldImages images{&image, &fetchTest};
    field::ImageParameters p;
    p.channel = 0;
    auto table = single(FieldOp::Image, field::encodeImage(p));
    // A 2 x 1 image covering composition pixels [0, 2) x [0, 1) of a 2 x 1 composition.
    auto geometry = field::fieldRasterGeometry(0, 0, 2, 1, 2.0, 1.0, 1.0, 1.0, 1);
    geometry.slots[0] = field::fieldImageSlot(0, 0, 2, 1, 2.0, 1.0, 1.0, 1.0);
    e.expect(field::applyFieldRasterGeometry(table, geometry), "the geometry applies");
    e.expect(field::validFieldTable(table), "the raster table is valid");
    // Field x = 0 is the composition's centre: halfway between the two texel centres.
    const auto v = field::evaluateField(table, images, 0.0F, 0.0F);
    e.expect(std::fabs(v[0] - 0.5F) < 1.0e-6F, "bilinear sampling at the field origin");
    const auto left = field::fieldPixel(table, images, 0, 0);
    const auto right = field::fieldPixel(table, images, 1, 0);
    e.expect(left[0] == 0.0F && right[0] == 1.0F && left[3] == 1.0F,
             "each target pixel samples its own texel centre");
    const auto outside = field::fieldPixel(table, images, 5, 0);
    e.expect(outside[3] == 0.0F, "outside the target is transparent");

    // Supersampling a hard pattern averages its sub-samples.
    field::PatternParameters checker;
    checker.shape = field::PatternShape::Checker;
    checker.size = 1.0;
    checker.softness = 0.0;
    auto fine = single(FieldOp::Pattern, *field::encodePattern(checker));
    auto ss = field::fieldRasterGeometry(0, 0, 4, 4, 4.0, 4.0, 1.0, 1.0, 16);
    e.expect(field::applyFieldRasterGeometry(fine, ss), "a supersampled geometry applies");
    const auto pixel = field::fieldPixel(fine, kNoImages, 1, 1);
    e.expect(pixel[0] >= 0.0F && pixel[0] <= 1.0F && pixel[3] == 1.0F, "sixteen sub-samples");
}

// #486 Lens Warp: the Brown-Conrady model against an independent double-precision reading, the
// identity at zero coefficients, Undistort and Redistort as inverses, the centre, and the
// premultiplied Image selector and result kind a warped image is drawn with.
std::array<double, 2> lensReference(const field::LensWarpParameters& p, const double x,
                                    const double y) {
    const auto nx = (x - p.centreX) / p.scale;
    const auto ny = (y - p.centreY) / p.scale;
    const auto r2 = nx * nx + ny * ny;
    const auto radial = 1.0 + p.k1 * r2 + p.k2 * r2 * r2;
    const auto dx = nx * radial + 2.0 * p.p1 * nx * ny + p.p2 * (r2 + 2.0 * nx * nx);
    const auto dy = ny * radial + p.p1 * (r2 + 2.0 * ny * ny) + 2.0 * p.p2 * nx * ny;
    return {p.centreX + dx * p.scale, p.centreY + dy * p.scale};
}

std::vector<float> lensTable(field::LensWarpParameters p, const bool reverse) {
    p.reverse = reverse;
    const auto block = field::encodeLensWarp(p);
    return block ? single(FieldOp::LensWarp, *block, field::FieldResultKind::Vector)
                 : std::vector<float>{};
}

void testLensWarp(Expectations& e) {
    field::LensWarpParameters lens;
    lens.k1 = -0.2;
    lens.k2 = 0.05;
    lens.p1 = 0.01;
    lens.p2 = -0.02;
    lens.centreX = 6.0;
    lens.centreY = -4.0;
    lens.scale = 100.0;
    const auto forward = lensTable(lens, false);
    const auto inverse = lensTable(lens, true);
    e.expect(field::validFieldTable(forward) && field::validFieldTable(inverse),
             "a lens warp table is valid");
    double worst = 0.0;
    double roundTrip = 0.0;
    double backTrip = 0.0;
    for (int j = -10; j <= 10; ++j)
        for (int i = -12; i <= 12; ++i) {
            const double x = 10.0 * i;
            const double y = 10.0 * j;
            const auto expected = lensReference(lens, x, y);
            const auto warped = at(forward, x, y);
            worst = std::max({worst, std::fabs(warped[0] - expected[0]),
                              std::fabs(warped[1] - expected[1])});
            // Redistort undoes Undistort and the other way round.
            const auto back = at(inverse, warped[0], warped[1]);
            roundTrip = std::max({roundTrip, std::fabs(back[0] - x), std::fabs(back[1] - y)});
            const auto there = at(inverse, x, y);
            const auto again = at(forward, there[0], there[1]);
            backTrip = std::max({backTrip, std::fabs(again[0] - x), std::fabs(again[1] - y)});
        }
    e.expect(worst < 5.0e-3, "Undistort reads the Brown-Conrady position of the model");
    e.expect(roundTrip < 2.0e-2, "Redistort undoes Undistort within a fiftieth of a pixel");
    e.expect(backTrip < 2.0e-2, "Undistort undoes Redistort within a fiftieth of a pixel");

    const auto centre = at(forward, lens.centreX, lens.centreY);
    e.expect(std::fabs(centre[0] - lens.centreX) < 1.0e-4 &&
                 std::fabs(centre[1] - lens.centreY) < 1.0e-4,
             "the lens centre does not move");

    // Barrel: with a negative k1 an Undistort pixel reads nearer the centre; pincushion farther.
    field::LensWarpParameters radial;
    radial.k1 = -0.2;
    radial.scale = 100.0;
    const auto barrel = at(lensTable(radial, false), 50.0, 0.0);
    radial.k1 = 0.2;
    const auto pincushion = at(lensTable(radial, false), 50.0, 0.0);
    e.expect(barrel[0] < 50.0 && barrel[0] > 45.0 && pincushion[0] > 50.0 && pincushion[0] < 55.0,
             "barrel reads nearer the centre and pincushion farther");

    // No coefficients is the identity in both modes.
    field::LensWarpParameters none;
    none.scale = 100.0;
    for (const bool reverse : {false, true}) {
        const auto identity = at(lensTable(none, reverse), 37.0, -22.0);
        e.expect(identity[0] == 37.0 && identity[1] == -22.0, "zero coefficients are the identity");
    }

    field::LensWarpParameters bad;
    bad.scale = 0.0;
    e.expect(!field::encodeLensWarp(bad), "a zero scale cannot be encoded");
    bad.scale = 100.0;
    bad.k1 = std::numeric_limits<double>::quiet_NaN();
    e.expect(!field::encodeLensWarp(bad), "a non-finite coefficient cannot be encoded");

    // The premultiplied selector and result kind keep a texel whole, colour at zero alpha included.
    TestImage image{1, 1, {{0.5F, 0.25F, 0.125F, 0.0F}}};
    const field::FieldImages images{&image, &fetchTest};
    field::ImageParameters read;
    read.selector = 3;
    read.edge = field::FieldImageEdge::Transparent;
    auto drawn = single(FieldOp::Image, field::encodeImage(read), field::FieldResultKind::Premultiplied);
    auto geometry = field::fieldRasterGeometry(0, 0, 1, 1, 1.0, 1.0, 1.0, 1.0, 1);
    geometry.slots[0] = field::fieldImageSlot(0, 0, 1, 1, 1.0, 1.0, 1.0, 1.0);
    e.expect(field::applyFieldRasterGeometry(drawn, geometry), "the image geometry applies");
    const auto kept = field::fieldPixel(drawn, images, 0, 0);
    e.expect(kept[0] == 0.5F && kept[1] == 0.25F && kept[2] == 0.125F && kept[3] == 0.0F,
             "a premultiplied texel is drawn as stored, even at zero alpha");
    auto straight = single(FieldOp::Image, field::encodeImage([&] {
                               auto p = read;
                               p.selector = 2;
                               return p;
                           }()),
                           field::FieldResultKind::Color);
    e.expect(field::applyFieldRasterGeometry(straight, geometry), "the straight geometry applies");
    const auto lost = field::fieldPixel(straight, images, 0, 0);
    e.expect(lost[0] == 0.0F && lost[3] == 0.0F, "the straight colour selector drops zero alpha");
}

// #348 Homography: a quad mapped to a quad against an independent forward map (the square-to-quad
// closed form), the identity, a similarity against its forward transform, the degenerate quad and
// the far side of the horizon reading nothing, and the sampler's refusal of an out-of-range position.
std::array<double, 2> forwardQuad(const std::array<double, 8>& from, const std::array<double, 8>& to,
                                  const double x, const double y) {
    // Heckbert's square to quad ("Fundamentals of Texture Mapping and Image Warping", 1989): the
    // unit square's (0,0), (1,0), (1,1), (0,1) go to the quad's bottom left, bottom right, top right
    // and top left. `from` is an axis aligned rectangle, so (u, v) is the position in it.
    const double x0 = to[6];
    const double y0 = to[7];
    const double x1 = to[4];
    const double y1 = to[5];
    const double x2 = to[2];
    const double y2 = to[3];
    const double x3 = to[0];
    const double y3 = to[1];
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
    const double ee = y3 - y0 + h * y3;
    const double u = (x - from[6]) / (from[4] - from[6]);
    const double v = (y - from[7]) / (from[1] - from[7]);
    const double w = g * u + h * v + 1.0;
    return {(a * u + b * v + x0) / w, (d * u + ee * v + y0) / w};
}

std::vector<float> homographyTable(const field::HomographyParameters& p,
                                   const field::FieldResultKind kind = field::FieldResultKind::Vector) {
    const auto block = field::encodeHomography(p);
    return block ? single(FieldOp::Homography, *block, kind) : std::vector<float>{};
}

void testHomography(Expectations& e) {
    // The picture's rectangle (corners top left, top right, bottom right, bottom left, y up).
    const std::array<double, 8> frame{-60.0, 40.0, 60.0, 40.0, 60.0, -40.0, -60.0, -40.0};
    const std::array<double, 8> quad{-50.0, 45.0, 70.0, 30.0, 55.0, -50.0, -65.0, -35.0};
    field::HomographyParameters pin;
    e.expect(field::homographyFromQuads(frame, quad, pin) && pin.valid, "a quad solves");
    const auto table = homographyTable(pin);
    e.expect(field::validFieldTable(table), "a homography table is valid");
    // Each output pixel inside the quad reads the point whose forward image is that pixel.
    double worst = 0.0;
    for (int j = -3; j <= 3; ++j)
        for (int i = -4; i <= 4; ++i) {
            const double x = 12.0 * i + 3.0;
            const double y = 9.0 * j - 2.0;
            const auto read = at(table, x, y);
            const auto back = forwardQuad(frame, quad, read[0], read[1]);
            worst = std::max({worst, std::fabs(back[0] - x), std::fabs(back[1] - y)});
        }
    e.expect(worst < 5.0e-3, "a pixel reads the point the quad's map sends to it");
    // The quad's own corners read the frame's corners.
    double corners = 0.0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto read = at(table, quad[2 * i], quad[2 * i + 1]);
        corners = std::max({corners, std::fabs(read[0] - frame[2 * i]),
                            std::fabs(read[1] - frame[2 * i + 1])});
    }
    e.expect(corners < 2.0e-3, "each corner of the quad reads the matching corner of the frame");

    field::HomographyParameters same;
    e.expect(field::homographyFromQuads(frame, frame, same), "a quad onto itself solves");
    const auto identity = at(homographyTable(same), 17.0, -23.0);
    e.expect(std::fabs(identity[0] - 17.0) < 1.0e-3 && std::fabs(identity[1] + 23.0) < 1.0e-3,
             "a quad onto itself reads the pixel's own position");

    // A pinched quad (two corners together) and a line cannot be solved: the map is invalid.
    auto pinched = frame;
    pinched[2] = pinched[0];
    pinched[3] = pinched[1];
    field::HomographyParameters bad;
    e.expect(!field::homographyFromQuads(frame, pinched, bad) && !bad.valid,
             "a quad with two corners together is invalid");
    const std::array<double, 8> line{-60.0, 0.0, -20.0, 0.0, 20.0, 0.0, 60.0, 0.0};
    e.expect(!field::homographyFromQuads(frame, line, bad), "a quad on one line is invalid");
    const auto nowhere = at(homographyTable(bad), 3.0, 4.0);
    e.expect(nowhere[0] > 1.0e15, "an invalid map reads a position no image reaches");
    e.expect(field::validFieldTable(homographyTable(bad)), "an invalid map still encodes");
    auto notANumber = frame;
    notANumber[0] = std::numeric_limits<double>::quiet_NaN();
    e.expect(!field::homographyFromQuads(notANumber, quad, bad), "a non-finite corner is invalid");

    // A similarity: scaled by 2 and turned a quarter about the pivot, then moved. The output pixel
    // that the forward transform sends a source point to reads that point.
    const auto sim = field::similarityHomography(10.0, -5.0, 90.0, 2.0, 4.0, 6.0);
    const auto simTable = homographyTable(sim);
    double similarity = 0.0;
    for (const auto& source : {std::array<double, 2>{0.0, 0.0}, {30.0, 10.0}, {-20.0, 35.0}}) {
        // Forward: p = pivot + offset + 2 R(90) (q - pivot), R(90) (x, y) = (-y, x).
        const double rx = source[0] - 4.0;
        const double ry = source[1] - 6.0;
        const double px = 4.0 + 10.0 + 2.0 * (-ry);
        const double py = 6.0 - 5.0 + 2.0 * rx;
        const auto read = at(simTable, px, py);
        similarity = std::max({similarity, std::fabs(read[0] - source[0]),
                               std::fabs(read[1] - source[1])});
    }
    e.expect(similarity < 1.0e-3, "a similarity reads the point the forward transform sends here");
    e.expect(!field::similarityHomography(0.0, 0.0, 0.0, 0.0, 0.0, 0.0).valid,
             "a zero scale is invalid");

    // The far side of the horizon reads nothing: a strong perspective whose vanishing line crosses
    // the output.
    const std::array<double, 8> keystone{-10.0, 40.0, 10.0, 40.0, 60.0, -40.0, -60.0, -40.0};
    field::HomographyParameters horizon;
    e.expect(field::homographyFromQuads(frame, keystone, horizon), "a keystone solves");
    const auto horizonTable = homographyTable(horizon);
    bool folded = false;
    bool inside = true;
    for (double y = -40.0; y < 4000.0; y += 10.0) {
        const auto read = at(horizonTable, 0.0, y);
        if (y <= 40.0)
            inside = inside && std::fabs(read[1]) < 41.0;
        folded = folded || read[1] > 1.0e15;
    }
    e.expect(inside, "pixels inside the keystone read inside the frame");
    e.expect(folded, "pixels past the vanishing line read nothing instead of folding back");

    // A window map is solved when the image's window is placed: the corners of a 16 x 12 window moved
    // by offsets read back the window's own corners.
    {
        field::HomographyParameters pinWindow;
        pinWindow.windowSlot = 0;
        pinWindow.offsets = {-2.0, 1.0, 1.0, -1.5, -1.0, 2.0, 2.5, 0.5};
        auto window = homographyTable(pinWindow);
        auto geometry = field::fieldRasterGeometry(0, 0, 16, 12, 16.0, 12.0, 1.0, 1.0, 1);
        auto unplaced = window;
        e.expect(field::applyFieldRasterGeometry(unplaced, geometry), "the bare geometry applies");
        const auto before = field::fieldPixel(unplaced, kNoImages, 3, 4);
        e.expect(before[0] > 1.0e15F,
                 "a window map before its window is placed is invalid");
        geometry.slots[0] = field::fieldImageSlot(0, 0, 16, 12, 16.0, 12.0, 1.0, 1.0);
        e.expect(field::applyFieldRasterGeometry(window, geometry), "the window geometry applies");
        const std::array<double, 8> frameCorners{-8.0, 6.0, 8.0, 6.0, 8.0, -6.0, -8.0, -6.0};
        std::array<double, 8> moved = frameCorners;
        for (std::size_t k = 0; k < 8; ++k)
            moved[k] += pinWindow.offsets[k];
        double windowWorst = 0.0;
        for (int j = 2; j < 10; j += 3)
            for (int i = 2; i < 14; i += 3) {
                const auto pixel = field::fieldPixel(window, kNoImages, i, j);
                const double x = static_cast<double>(i) + 0.5 - 8.0;
                const double y = 6.0 - (static_cast<double>(j) + 0.5);
                const auto back = forwardQuad(frameCorners, moved, static_cast<double>(pixel[0]),
                                              static_cast<double>(pixel[1]));
                windowWorst = std::max({windowWorst, std::fabs(back[0] - x), std::fabs(back[1] - y)});
            }
        e.expect(windowWorst < 5.0e-3, "a window map reads the window pinned by its offsets");
        // No offsets: every pixel reads its own position.
        field::HomographyParameters flat;
        flat.windowSlot = 0;
        auto flatTable = homographyTable(flat);
        e.expect(field::applyFieldRasterGeometry(flatTable, geometry), "the flat geometry applies");
        const auto own = field::fieldPixel(flatTable, kNoImages, 3, 4);
        e.expect(std::fabs(own[0] + 4.5F) < 1.0e-3F && std::fabs(own[1] - 1.5F) < 1.0e-3F,
                 "a window map with no offsets reads the pixel's own position");
        // An absent image leaves the map invalid.
        auto absent = homographyTable(flat);
        auto none = geometry;
        none.slots[0] = {};
        e.expect(field::applyFieldRasterGeometry(absent, none), "the absent geometry applies");
        const auto lost = field::fieldPixel(absent, kNoImages, 3, 4);
        e.expect(lost[0] > 1.0e15F, "a window map with no image reads nothing");
    }

    // The sampler refuses a position it cannot index, on the same terms as an edge: transparent.
    TestImage image{1, 1, {{0.5F, 0.25F, 0.125F, 1.0F}}};
    const field::FieldImages images{&image, &fetchTest};
    field::FieldProgramBuilder builder;
    builder.emit(FieldOp::Constant, 0, field::kNoFieldInputs, field::encodeConstant({1.0e12, 0.0, 0.0, 0.0}));
    field::ImageParameters read;
    read.selector = 3;
    read.edge = field::FieldImageEdge::Clamp;
    builder.emit(FieldOp::Image, 1, {0, -1, -1, -1}, field::encodeImage(read));
    auto far = builder.finish(1, field::FieldResultKind::Premultiplied, field::FieldOutputMode::Raw)
                   .value_or(std::vector<float>{});
    auto geometry = field::fieldRasterGeometry(0, 0, 1, 1, 1.0, 1.0, 1.0, 1.0, 1);
    geometry.slots[0] = field::fieldImageSlot(0, 0, 1, 1, 1.0, 1.0, 1.0, 1.0);
    e.expect(field::applyFieldRasterGeometry(far, geometry), "the far geometry applies");
    const auto nothing = field::fieldPixel(far, images, 0, 0);
    e.expect(nothing[3] == 0.0F && nothing[0] == 0.0F,
             "a position beyond any texel index reads transparent, not the edge pixel");
}

// #508 Rays: the weighted gather along the segment towards the centre against an independent
// double-precision reading, the identities (zero length, zero boost, the centre itself), the alpha
// source, and the encoder's refusals.
void testRays(Expectations& e) {
    // A 64 x 1 image: the left half black, the right half white, opaque.
    TestImage image{64, 1, {}};
    for (int x = 0; x < 64; ++x) {
        const float v = x >= 32 ? 1.0F : 0.0F;
        image.texels.push_back({v, v, v, x >= 32 ? 1.0F : 0.5F});
    }
    const field::FieldImages images{&image, &fetchTest};
    const auto build = [&](const double length, const double falloff, const double boost,
                           const std::uint8_t source) {
        field::RaysParameters p;
        p.source = source;
        p.centreX = 20.0;
        p.centreY = 0.0;
        p.length = length;
        p.falloff = falloff;
        p.boost = boost;
        const auto block = field::encodeRays(p);
        auto table = block ? single(FieldOp::Rays, *block, field::FieldResultKind::Vector)
                           : std::vector<float>{};
        auto geometry = field::fieldRasterGeometry(0, 0, 64, 1, 64.0, 1.0, 1.0, 1.0, 1);
        geometry.slots[0] = field::fieldImageSlot(0, 0, 64, 1, 64.0, 1.0, 1.0, 1.0);
        if (!table.empty())
            e.expect(field::applyFieldRasterGeometry(table, geometry) &&
                         field::validFieldTable(table),
                     "a rays table is valid");
        return table;
    };
    const auto reference = [&](const double length, const double falloff, const double boost,
                               const bool alpha, const double x) {
        const double dx = 20.0 - x;
        const auto reach = std::fabs(dx) * length;
        const int n = std::min(static_cast<int>(std::floor(static_cast<float>(reach) * 0.5F)) + 1, 96);
        double sum = 0.0;
        double weights = 0.0;
        for (int i = 0; i < n; ++i) {
            const double t = (i + 0.5) / n;
            const double u = (x + dx * t * length + 32.0) - 0.5;
            const int u0 = static_cast<int>(std::floor(u));
            const double f = u - u0;
            const auto tap = [&](const int k) {
                if (k < 0 || k >= 64)
                    return 0.0;
                const auto& t4 = image.texels[static_cast<std::size_t>(k)];
                const auto r = static_cast<double>(t4[0]);
                const auto g = static_cast<double>(t4[1]);
                const auto bl = static_cast<double>(t4[2]);
                return alpha ? static_cast<double>(t4[3]) : r * 0.2126 + g * 0.7152 + bl * 0.0722;
            };
            const double value = tap(u0) + f * (tap(u0 + 1) - tap(u0));
            const double w = std::pow(1.0 - t, falloff);
            sum += value * w;
            weights += w;
        }
        return weights > 0.0 ? sum / weights * boost : 0.0;
    };
    double worst = 0.0;
    for (const auto falloff : {0.0, 1.0, 3.0})
        for (const auto length : {0.25, 0.7, 1.0})
            for (const std::uint8_t source : {std::uint8_t{0}, std::uint8_t{1}}) {
                const auto table = build(length, falloff, 2.0, source);
                for (const double x : {-28.5, -20.0, -3.25, 5.5, 14.0, 27.75}) {
                    const auto v = field::evaluateField(table, images, static_cast<float>(x), 0.0F);
                    const auto expected = reference(length, falloff, 2.0, source == 1, x);
                    worst = std::max(worst, std::fabs(static_cast<double>(v[0]) - expected));
                    worst = std::max(worst, std::fabs(static_cast<double>(v[3]) -
                                                      std::min(expected, 1.0)));
                    worst = std::max(worst, std::fabs(static_cast<double>(v[1]) -
                                                      static_cast<double>(v[0])));
                }
            }
    e.expect(worst < 2.0e-3, "the rays gather the image along the segment towards the centre");

    // Even weights over a half dark, half bright segment are their mean.
    const auto even = field::evaluateField(build(1.0, 0.0, 1.0, 0), images, -20.0F, 0.0F);
    e.expect(std::fabs(even[0] - 0.5F) < 1.0e-3F, "an even gather of a half bright segment is a half");
    // A steeper falloff weighs the near, dark side more.
    const auto steep = field::evaluateField(build(1.0, 3.0, 1.0, 0), images, -20.0F, 0.0F);
    e.expect(steep[0] < even[0], "a steeper falloff weighs the pixel's own side more");
    const auto none = field::evaluateField(build(0.0, 1.0, 2.0, 0), images, -20.0F, 0.0F);
    e.expect(none[0] == 0.0F && none[3] == 0.0F, "a zero length gathers nothing");
    const auto dark = field::evaluateField(build(1.0, 1.0, 0.0, 0), images, -20.0F, 0.0F);
    e.expect(dark[0] == 0.0F, "a zero boost gathers nothing");
    const auto atCentre = field::evaluateField(build(1.0, 1.0, 2.0, 0), images, 20.0F, 0.0F);
    e.expect(atCentre[0] == 0.0F, "the centre itself gathers nothing");

    field::RaysParameters bad;
    bad.length = 1.5;
    e.expect(!field::encodeRays(bad), "a length beyond one cannot be encoded");
    bad.length = 0.5;
    bad.boost = -1.0;
    e.expect(!field::encodeRays(bad), "a negative boost cannot be encoded");
    bad.boost = 1.0;
    bad.falloff = std::numeric_limits<double>::quiet_NaN();
    e.expect(!field::encodeRays(bad), "a non-finite falloff cannot be encoded");
    bad.falloff = 1.0;
    bad.source = 2;
    e.expect(!field::encodeRays(bad), "an unknown source cannot be encoded");
}

void testDistanceTransform(Expectations& e) {
    std::uint32_t state = 12345U;
    const auto next = [&state]() {
        state = state * 1664525U + 1013904223U;
        return state >> 8U;
    };
    for (int trial = 0; trial < 50; ++trial) {
        const auto n = static_cast<std::size_t>(1 + next() % 200U);
        const float cap = 400.0F;
        std::vector<float> f(n);
        for (auto& v : f)
            v = (next() % 7U == 0U) ? 0.0F : static_cast<float>(next() % 4U == 0U ? cap : next() % 300U);
        std::vector<float> d(n);
        std::vector<int> vertices(n);
        std::vector<double> bounds(n + 1U);
        field::squaredDistance1d(f, d, vertices, bounds, cap);
        bool exact = true;
        for (std::size_t q = 0; q < n; ++q) {
            auto best = static_cast<double>(cap);
            for (std::size_t p = 0; p < n; ++p) {
                const auto dq = static_cast<double>(q) - static_cast<double>(p);
                best = std::min(best, dq * dq + static_cast<double>(f[p]));
            }
            exact = exact && d[q] == static_cast<float>(best);
        }
        e.expect(exact, "the 1D distance transform is the exact lower envelope");
    }
}

// --- Test Card ---------------------------------------------------------------------------------

// The card's premultiplied RGBA at pixel position (px, py) of its own frame (top left origin, y down).
std::array<double, 4> cardAt(const field::TestCardParameters& p, const double px, const double py) {
    const auto block = field::encodeTestCard(p);
    if (!block)
        return {-9.0, -9.0, -9.0, -9.0};
    const auto table = single(FieldOp::TestCard, *block, field::FieldResultKind::Premultiplied);
    return at(table, px - p.width * 0.5 + p.centerX, p.height * 0.5 - py + p.centerY);
}

field::TestCardParameters cardOf(const std::int64_t card, const double w = 1920.0,
                                 const double h = 1080.0) {
    field::TestCardParameters p;
    p.card = card;
    p.width = w;
    p.height = h;
    return p;
}

bool near(const std::array<double, 4>& v, const std::array<double, 4>& want,
          const double tolerance = 1.0e-6) {
    for (std::size_t i = 0; i < 4; ++i)
        if (std::abs(v[i] - want[i]) > tolerance)
            return false;
    return true;
}

void testTestCard(Expectations& e) {
    // Encoding limits.
    auto bad = cardOf(16);
    e.expect(!field::encodeTestCard(bad), "a card beyond the sixteen is refused");
    bad = cardOf(0, 0.0, 10.0);
    e.expect(!field::encodeTestCard(bad), "a frame under a pixel is refused");
    bad = cardOf(0);
    bad.text.assign(25, 1);
    e.expect(!field::encodeTestCard(bad), "a label beyond 24 characters is refused");
    bad = cardOf(0);
    bad.text = {43};
    e.expect(!field::encodeTestCard(bad), "a glyph beyond the table is refused");
    bad = cardOf(0);
    bad.values[0] = std::numeric_limits<double>::infinity();
    e.expect(!field::encodeTestCard(bad), "a non-finite number is refused");
    e.expect(field::validFieldTable(
                 single(FieldOp::TestCard, *field::encodeTestCard(cardOf(0)),
                        field::FieldResultKind::Premultiplied)),
             "a Test Card table is valid");

    // SD Bars (the whole frame, 75% bars, castellation, -I, white, +Q, black and PLUGE).
    const auto sd = cardOf(0);
    e.expect(near(cardAt(sd, 137.0, 300.0), {0.75, 0.75, 0.75, 1.0}), "SD: the white bar is 75%");
    e.expect(near(cardAt(sd, 411.0, 300.0), {0.75, 0.75, 0.0, 1.0}), "SD: the yellow bar");
    e.expect(near(cardAt(sd, 1783.0, 300.0), {0.0, 0.0, 0.75, 1.0}), "SD: the blue bar");
    e.expect(near(cardAt(sd, 137.0, 780.0), {0.0, 0.0, 0.75, 1.0}), "SD: the castellation reverses");
    e.expect(near(cardAt(sd, 411.0, 780.0), {0.0, 0.0, 0.0, 1.0}), "SD: black between castellations");
    e.expect(near(cardAt(sd, 1783.0, 780.0), {0.75, 0.75, 0.75, 1.0}), "SD: the castellation ends white");
    e.expect(near(cardAt(sd, 137.0, 1000.0), {0.0, 0.246575341, 0.410958916, 1.0}),
             "SD: -I below the white bar");
    e.expect(near(cardAt(sd, 411.0, 1000.0), {1.0, 1.0, 1.0, 1.0}), "SD: 100% white");
    e.expect(near(cardAt(sd, 960.0, 1000.0), {0.255707771, 0.0, 0.465753436, 1.0}), "SD: +Q");
    e.expect(near(cardAt(sd, 1400.0, 1000.0), {-0.04, -0.04, -0.04, 1.0}),
             "SD: the PLUGE step below black is kept below zero");
    e.expect(near(cardAt(sd, 1500.0, 1000.0), {0.0, 0.0, 0.0, 1.0}), "SD: the PLUGE black");
    e.expect(near(cardAt(sd, 1600.0, 1000.0), {0.04, 0.04, 0.04, 1.0}), "SD: the PLUGE step above black");
    e.expect(near(cardAt(sd, 1800.0, 1000.0), {0.0, 0.0, 0.0, 1.0}), "SD: black beside the PLUGE");
    // A portrait frame holds the 16:9 layout whole, centerd (rows 656.25 to 1263.75), black around it.
    const auto sdPortrait = cardOf(0, 1080.0, 1920.0);
    e.expect(near(cardAt(sdPortrait, 1080.0 * 0.2, 800.0), {0.75, 0.75, 0.0, 1.0}) &&
                 near(cardAt(sdPortrait, 1080.0 * 0.2, 300.0), {0.0, 0.0, 0.0, 1.0}) &&
                 near(cardAt(sdPortrait, 1080.0 * 0.2, 1600.0), {0.0, 0.0, 0.0, 1.0}),
             "SD: a portrait frame holds the 16:9 layout, padded in black, not stretched");

    // HD Bars (SMPTE RP 219-1): in 1920 x 1080 the widths of annex C table C.1 and C.2.
    const auto hd = cardOf(1);
    e.expect(near(cardAt(hd, 100.5, 300.0), {0.4, 0.4, 0.4, 1.0}), "HD: 40% gray beside the bars");
    e.expect(near(cardAt(hd, 300.5, 300.0), {0.75, 0.75, 0.75, 1.0}), "HD: the 75% white bar");
    e.expect(near(cardAt(hd, 1578.5, 300.0), {0.0, 0.0, 0.75, 1.0}), "HD: the 75% blue bar");
    e.expect(near(cardAt(hd, 100.5, 660.0), {0.0, 1.0, 1.0, 1.0}), "HD: 100% cyan");
    e.expect(near(cardAt(hd, 900.5, 660.0), {0.75, 0.75, 0.75, 1.0}), "HD: the 75% white chroma reference");
    e.expect(near(cardAt(hd, 1800.5, 660.0), {0.0, 0.0, 1.0, 1.0}), "HD: 100% blue");
    e.expect(near(cardAt(hd, 100.5, 750.0), {1.0, 1.0, 0.0, 1.0}), "HD: 100% yellow");
    e.expect(near(cardAt(hd, 300.5, 750.0), {0.0, 0.0, 0.0, 1.0}), "HD: 0% black left of the ramp");
    e.expect(near(cardAt(hd, 1000.5, 750.0),
                  {(1000.5 - 445.714294) / 1028.57141, (1000.5 - 445.714294) / 1028.57141,
                   (1000.5 - 445.714294) / 1028.57141, 1.0},
                  1.0e-5),
             "HD: the ramp is linear from 0% to 100%");
    e.expect(near(cardAt(hd, 1600.5, 750.0), {1.0, 1.0, 1.0, 1.0}), "HD: 100% white right of the ramp");
    e.expect(near(cardAt(hd, 1800.5, 750.0), {1.0, 0.0, 0.0, 1.0}), "HD: 100% red");
    e.expect(near(cardAt(hd, 100.5, 900.0), {0.15, 0.15, 0.15, 1.0}), "HD: 15% gray");
    e.expect(near(cardAt(hd, 700.5, 900.0), {1.0, 1.0, 1.0, 1.0}), "HD: the 100% white block");
    e.expect(near(cardAt(hd, 1150.5, 900.0), {-0.02, -0.02, -0.02, 1.0}), "HD: PLUGE -2% below black");
    e.expect(near(cardAt(hd, 1230.5, 900.0), {0.0, 0.0, 0.0, 1.0}), "HD: PLUGE black");
    e.expect(near(cardAt(hd, 1300.5, 900.0), {0.02, 0.02, 0.02, 1.0}), "HD: PLUGE +2%");
    e.expect(near(cardAt(hd, 1440.5, 900.0), {0.04, 0.04, 0.04, 1.0}), "HD: PLUGE +4%");
    // A wide frame pads the 16:9 layout in black at the sides; a tall one above and below.
    const auto hdWide = cardOf(1, 2048.0, 858.0);
    e.expect(near(cardAt(hdWide, 10.0, 400.0), {0.0, 0.0, 0.0, 1.0}), "HD: a wide frame is padded in black");
    e.expect(cardAt(hdWide, 1024.0, 429.0)[3] == 1.0, "HD: the wide frame's layout is opaque");
    const auto hdTall = cardOf(1, 1080.0, 1920.0);
    e.expect(near(cardAt(hdTall, 540.0, 100.0), {0.0, 0.0, 0.0, 1.0}), "HD: a tall frame is padded in black");
    // The layout rectangle of the wide frame is 1525.33 x 858 about the center: the 75% yellow bar.
    e.expect(near(cardAt(hdWide, 1024.0 - 762.67 + (1525.33 / 1920.0) * 500.0, 200.0),
                  {0.75, 0.75, 0.0, 1.0}),
             "HD: the layout inside the padding keeps its proportions");

    // EBU Bars (ITU-R BT.471, 100/0/75/0).
    const auto ebu = cardOf(2);
    e.expect(near(cardAt(ebu, 100.0, 500.0), {1.0, 1.0, 1.0, 1.0}), "EBU: 100% white");
    e.expect(near(cardAt(ebu, 360.0, 500.0), {0.75, 0.75, 0.0, 1.0}), "EBU: 75% yellow");
    e.expect(near(cardAt(ebu, 1800.0, 500.0), {0.0, 0.0, 0.0, 1.0}), "EBU: the black bar");

    // HDR Bars (ITU-R BT.2111-3): signal = (code - 64) / 876 narrow, code / 1023 full range.
    const auto narrow = [](const int code) { return (code - 64) / 876.0; };
    for (const std::int64_t variant : {0, 1, 2}) {
        auto hdr = cardOf(3);
        hdr.variant = variant;
        const auto sig = [&](const int code) {
            return variant == 2 ? code / 1023.0 : narrow(code);
        };
        const int white = variant == 0 ? 721 : (variant == 1 ? 573 : 594);
        const int gray = variant == 2 ? 409 : 414;
        const auto name = std::string(" (variant ") + std::to_string(variant) + ")";
        e.expect(near(cardAt(hdr, 100.5, 300.0), {sig(gray), sig(gray), sig(gray), 1.0}, 1.0e-6),
                 "HDR: 40% gray" + name);
        e.expect(near(cardAt(hdr, 300.5, 300.0), {sig(white), sig(white), sig(white), 1.0}, 1.0e-6),
                 "HDR: the reference white bar" + name);
        e.expect(near(cardAt(hdr, 300.5, 40.0),
                      {sig(variant == 2 ? 1023 : 940), sig(variant == 2 ? 1023 : 940),
                       sig(variant == 2 ? 1023 : 940), 1.0}, 1.0e-6),
                 "HDR: the 100% white bar above" + name);
        e.expect(near(cardAt(hdr, 500.5, 40.0),
                      {sig(variant == 2 ? 1023 : 940), sig(variant == 2 ? 1023 : 940), 0.0, 1.0},
                      1.0e-6),
                 "HDR: the 100% yellow bar above" + name);
        // The stair: -7% below black (0% in full range) over the white bar, then 0% and 10% over yellow.
        e.expect(near(cardAt(hdr, 300.5, 680.0),
                      {sig(variant == 2 ? 0 : 4), sig(variant == 2 ? 0 : 4), sig(variant == 2 ? 0 : 4), 1.0},
                      1.0e-6),
                 "HDR: the first step" + name);
        e.expect(near(cardAt(hdr, 600.5, 680.0),
                      {sig(variant == 2 ? 102 : 152), sig(variant == 2 ? 102 : 152),
                       sig(variant == 2 ? 102 : 152), 1.0}, 1.0e-6),
                 "HDR: the 10% step" + name);
        e.expect(near(cardAt(hdr, 1650.5, 680.0),
                      {sig(variant == 2 ? 1023 : 1019), sig(variant == 2 ? 1023 : 1019),
                       sig(variant == 2 ? 1023 : 1019), 1.0}, 1.0e-6),
                 "HDR: the last step" + name);
        // The ramp: flat, then one code per 1920th of the width.
        if (variant != 2) {
            e.expect(near(cardAt(hdr, 900.5, 765.0),
                          {(4.0 + (900.5 - 799.0) - 64.0) / 876.0,
                           (4.0 + (900.5 - 799.0) - 64.0) / 876.0,
                           (4.0 + (900.5 - 799.0) - 64.0) / 876.0, 1.0},
                          1.0e-5),
                     "HDR: the narrow-range ramp rises a code a pixel" + name);
            e.expect(near(cardAt(hdr, 400.5, 765.0), {narrow(4), narrow(4), narrow(4), 1.0}, 1.0e-6),
                     "HDR: the ramp starts at -7%" + name);
        } else {
            e.expect(near(cardAt(hdr, 1000.5, 765.0),
                          {(1000.5 - 858.0) / 1023.0, (1000.5 - 858.0) / 1023.0,
                           (1000.5 - 858.0) / 1023.0, 1.0},
                          1.0e-5),
                     "HDR: the full-range ramp starts at the green bar" + name);
        }
        // The black steps: -2% (not in full range), 0, +2%, 0, +4%.
        e.expect(near(cardAt(hdr, 410.5, 900.0),
                      {sig(variant == 2 ? 0 : 48), sig(variant == 2 ? 0 : 48), sig(variant == 2 ? 0 : 48), 1.0},
                      1.0e-6),
                 "HDR: the -2% black step" + name);
        e.expect(near(cardAt(hdr, 550.5, 900.0),
                      {sig(variant == 2 ? 19 : 80), sig(variant == 2 ? 19 : 80),
                       sig(variant == 2 ? 19 : 80), 1.0}, 1.0e-6),
                 "HDR: the +2% black step" + name);
        e.expect(near(cardAt(hdr, 690.5, 900.0),
                      {sig(variant == 2 ? 41 : 99), sig(variant == 2 ? 41 : 99),
                       sig(variant == 2 ? 41 : 99), 1.0}, 1.0e-6),
                 "HDR: the +4% black step" + name);
        e.expect(near(cardAt(hdr, 1100.5, 900.0), {sig(white), sig(white), sig(white), 1.0}, 1.0e-6),
                 "HDR: the reference white block" + name);
        const int r = variant == 0 ? 713 : (variant == 1 ? 569 : 589);
        const int g = variant == 0 ? 719 : (variant == 1 ? 572 : 593);
        const int b = variant == 0 ? 316 : (variant == 1 ? 381 : 370);
        e.expect(near(cardAt(hdr, 40.5, 900.0), {sig(r), sig(g), sig(b), 1.0}, 1.0e-6),
                 "HDR: the BT.709 yellow bar" + name);
    }

    // Patch: the exact value, full field or in a centerd window over black.
    auto patch = cardOf(4);
    patch.variant = 14;
    e.expect(near(cardAt(patch, 500.0, 500.0), {1.0, 1.0, 1.0, 1.0}), "Patch: white 100%");
    patch.variant = 7;
    e.expect(near(cardAt(patch, 500.0, 500.0), {0.5, 0.5, 0.5, 1.0}, 1.0e-6), "Patch: gray 50%");
    patch.variant = 2;
    e.expect(near(cardAt(patch, 1.0, 1.0), {0.04, 0.04, 0.04, 1.0}), "Patch: near black 4%");
    patch.variant = 15;
    e.expect(near(cardAt(patch, 960.0, 540.0), {0.75, 0.0, 0.0, 1.0}), "Patch: red 75%");
    patch.variant = 26;
    e.expect(near(cardAt(patch, 960.0, 540.0), {1.0, 1.0, 0.0, 1.0}), "Patch: yellow 100%");
    patch.variant = 14;
    patch.option = 3;
    e.expect(near(cardAt(patch, 960.0, 540.0), {1.0, 1.0, 1.0, 1.0}), "Patch: the window's center");
    e.expect(near(cardAt(patch, 100.0, 100.0), {0.0, 0.0, 0.0, 1.0}), "Patch: black around the window");
    // A tenth of the area: the window is about 0.316 of each side.
    e.expect(near(cardAt(patch, 960.0 + 0.158 * 1920.0 - 2.0, 540.0), {1.0, 1.0, 1.0, 1.0}) &&
                 near(cardAt(patch, 960.0 + 0.158 * 1920.0 + 2.0, 540.0), {0.0, 0.0, 0.0, 1.0}),
             "Patch: a window of a tenth of the frame's area");
    // The set follows the numbers of the kernel in the document vocabulary.
    for (std::int64_t index = 0; index < 27; ++index) {
        auto each = cardOf(4);
        each.variant = index;
        const auto v = cardAt(each, 500.0, 500.0);
        e.expect(v[3] == 1.0 && v[0] >= 0.0, "Patch: every value of the set is drawn");
    }

    // Gray Ramp.
    const auto ramp = cardOf(6);
    e.expect(near(cardAt(ramp, 960.0, 300.0), {0.5, 0.5, 0.5, 1.0}), "Gray Ramp: the middle is 50%");
    e.expect(near(cardAt(ramp, 480.0, 300.0), {0.25, 0.25, 0.25, 1.0}), "Gray Ramp: a quarter along");
    e.expect(near(cardAt(ramp, 320.0, 1000.0), {-0.04, -0.04, -0.04, 1.0}),
             "Gray Ramp: a PLUGE step below black");
    e.expect(near(cardAt(ramp, 1200.0, 1000.0), {0.02, 0.02, 0.02, 1.0}) &&
                 near(cardAt(ramp, 1500.0, 1000.0), {0.04, 0.04, 0.04, 1.0}),
             "Gray Ramp: PLUGE steps above black");

    // Color Chart: 24 patches in a 3:2 rectangle (here 1620 x 1080 of 1920).
    const auto chart = cardOf(5);
    const double chartLeft = (1920.0 - 1620.0) / 2.0;
    const auto patchCenter = [&](const int col, const int row) {
        return std::array<double, 2>{chartLeft + (col + 0.5) * 270.0, (row + 0.5) * 270.0};
    };
    auto chartAt = [&](const int col, const int row) {
        const auto c = patchCenter(col, row);
        return cardAt(chart, c[0], c[1]);
    };
    e.expect(near(chartAt(0, 0), {1.0, 1.0, 1.0, 1.0}), "Chart: the white patch");
    e.expect(near(chartAt(3, 0), {0.4, 0.4, 0.4, 1.0}, 1.0e-6), "Chart: a gray patch");
    e.expect(near(chartAt(5, 0), {0.0, 0.0, 0.0, 1.0}, 1.0e-6), "Chart: the black patch");
    e.expect(near(chartAt(0, 1), {1.0, 0.0, 0.0, 1.0}), "Chart: red");
    e.expect(near(chartAt(4, 2), {0.5, 0.0, 0.5, 1.0}), "Chart: half magenta");
    e.expect(near(chartAt(0, 3), {0.90, 0.74, 0.64, 1.0}, 1.0e-6), "Chart: the lightest skin tone");
    e.expect(near(chartAt(5, 3), {0.28, 0.18, 0.14, 1.0}, 1.0e-6), "Chart: the darkest skin tone");
    e.expect(near(cardAt(chart, chartLeft + 2.0, 2.0), {0.12, 0.12, 0.12, 1.0}), "Chart: the gap between patches");
    e.expect(near(cardAt(chart, 10.0, 500.0), {0.0, 0.0, 0.0, 1.0}), "Chart: black outside the chart");

    // Grid: square cells, lines through the center, the spacing and line width.
    auto grid = cardOf(7);
    grid.values = {90.0, 2.0, 0.0, 0.0};
    e.expect(near(cardAt(grid, 960.0 + 90.0, 540.0 + 45.0), {0.85, 0.85, 0.85, 1.0}), "Grid: a vertical line");
    e.expect(near(cardAt(grid, 960.0 + 45.0, 540.0 + 90.0), {0.85, 0.85, 0.85, 1.0}), "Grid: a horizontal line");
    e.expect(near(cardAt(grid, 960.0 + 45.0, 540.0 + 45.0), {0.08, 0.08, 0.08, 1.0}), "Grid: the cell between");
    e.expect(near(cardAt(grid, 960.0, 540.0 + 45.0), {0.9, 0.8, 0.1, 1.0}), "Grid: the center line");
    e.expect(near(cardAt(grid, 960.0 + 90.0 + 1.5, 540.0 + 45.0), {0.08, 0.08, 0.08, 1.0}),
             "Grid: the line is the width asked");
    auto gridPortrait = cardOf(7, 1080.0, 1920.0);
    gridPortrait.values = {90.0, 2.0, 0.0, 0.0};
    e.expect(cardAt(gridPortrait, 540.0 + 90.0, 960.0 + 45.0) == cardAt(gridPortrait, 540.0 + 45.0, 960.0 + 90.0) &&
                 near(cardAt(gridPortrait, 540.0 + 90.0, 960.0 + 45.0), {0.85, 0.85, 0.85, 1.0}),
             "Grid: the spacing is the same across and down in a portrait frame");

    // Circle: round in a wide, a portrait and a square frame.
    for (const auto& [w, h] : {std::pair{1920.0, 1080.0}, std::pair{1080.0, 1920.0},
                               std::pair{1000.0, 1000.0}, std::pair{2048.0, 858.0}}) {
        const auto circle = cardOf(8, w, h);
        const double radius = 0.45 * std::min(w, h);
        const auto ring = cardAt(circle, w / 2.0 + radius, h / 2.0 + 10.0);
        e.expect(near(ring, {0.95, 0.95, 0.95, 1.0}) &&
                     near(cardAt(circle, w / 2.0 + 10.0, h / 2.0 + radius), ring) &&
                     near(cardAt(circle, w / 2.0 - radius, h / 2.0 + 10.0), ring) &&
                     near(cardAt(circle, w / 2.0 + 10.0, h / 2.0 - radius), ring),
                 "Circle: the ring is at the same distance across and down");
        const double d = radius / std::sqrt(2.0);
        e.expect(near(cardAt(circle, w / 2.0 + d, h / 2.0 + d), ring),
                 "Circle: the ring passes the diagonal at the same radius");
    }

    // Safe Areas: action safe at 93% and title safe at 90%.
    auto safe = cardOf(9);
    safe.option = 0;
    e.expect(near(cardAt(safe, 960.0 + 0.465 * 1920.0, 300.0), {0.9, 0.8, 0.1, 1.0}), "Safe: action safe at 93%");
    e.expect(near(cardAt(safe, 960.0 + 0.45 * 1920.0, 300.0), {0.1, 0.8, 0.9, 1.0}), "Safe: title safe at 90%");
    e.expect(near(cardAt(safe, 300.0, 300.0), {0.12, 0.12, 0.12, 1.0}), "Safe: the field");
    safe.option = 2;
    // The 9:16 guide in a 16:9 frame is 607.5 pixels wide about the center.
    e.expect(near(cardAt(safe, 960.0 + 303.75, 300.0), {0.5, 0.9, 0.2, 1.0}), "Safe: the 9:16 guide");
    safe.option = 8;
    // The 2.39:1 guide is the frame wide and 803.3 pixels tall.
    e.expect(near(cardAt(safe, 300.0, 540.0 - 1920.0 / 2.39 / 2.0), {0.4, 0.5, 1.0, 1.0}), "Safe: the 2.39:1 guide");

    // UV Grid: square cells colored by position, labeled.
    auto uv = cardOf(10);
    uv.values = {120.0, 0.0, 0.0, 0.0};
    e.expect(near(cardAt(uv, 0.2, 500.0), {0.0, 0.0, 0.0, 1.0}), "UV: the line between cells");
    const auto cell = cardAt(uv, 120.0 * 5.0 + 4.0, 120.0 * 2.0 + 4.0);
    e.expect(std::abs(cell[0] - (604.0 / 1920.0)) < 1.0e-5 && std::abs(cell[1] - (1.0 - 244.0 / 1080.0)) < 1.0e-5 &&
                 cell[2] == 0.25,
             "UV: a cell is colored by its position");
    int lit = 0;
    for (int y = 0; y < 120; ++y)
        for (int x = 0; x < 120; ++x) {
            const auto v = cardAt(uv, 120.0 * 5.0 + x + 0.5, 120.0 * 2.0 + y + 0.5);
            if (v[0] == 1.0 && v[1] == 1.0 && v[2] == 1.0)
                ++lit;
        }
    e.expect(lit > 20, "UV: each cell has its column and row written in it");

    // Zone Plate and Siemens Star.
    const auto zone = cardOf(11);
    e.expect(near(cardAt(zone, 960.0, 540.0), {1.0, 1.0, 1.0, 1.0}, 1.0e-6), "Zone: the center is white");
    e.expect(near(cardAt(zone, 960.0 + 31.0, 540.0), cardAt(zone, 960.0, 540.0 + 31.0), 1.0e-5),
             "Zone: the rings are round");
    auto moving = cardOf(11);
    moving.timed[0] = 0.5;
    e.expect(near(cardAt(moving, 960.0, 540.0), {0.0, 0.0, 0.0, 1.0}, 1.0e-5), "Zone: half a turn of phase");
    auto star = cardOf(12);
    star.values[0] = 36.0;
    e.expect(near(cardAt(star, 960.0 + 100.0, 540.0 - 0.1), {1.0, 1.0, 1.0, 1.0}) ||
                 near(cardAt(star, 960.0 + 100.0, 540.0 - 0.1), {0.0, 0.0, 0.0, 1.0}),
             "Star: a spoke is black or white");
    e.expect(near(cardAt(star, 960.0 + 500.0, 540.0), {0.18, 0.18, 0.18, 1.0}), "Star: outside its circle");
    {
        const double r = 300.0;
        const double a0 = 0.25 / 36.0 * 2.0 * std::numbers::pi;
        const double a1 = 0.75 / 36.0 * 2.0 * std::numbers::pi;
        const auto s0 = cardAt(star, 960.0 + r * std::cos(a0), 540.0 + r * std::sin(a0));
        const auto s1 = cardAt(star, 960.0 + r * std::cos(a1), 540.0 + r * std::sin(a1));
        e.expect(near(s0, {1.0, 1.0, 1.0, 1.0}) && near(s1, {0.0, 0.0, 0.0, 1.0}), "Star: the spokes alternate");
    }

    // Moving Bar: a bar of 100 px at half way.
    auto bar = cardOf(13);
    bar.values = {100.0, 2.0, 0.0, 0.0};
    bar.timed[0] = 0.5;
    e.expect(near(cardAt(bar, 950.0, 200.0), {1.0, 1.0, 1.0, 1.0}) && near(cardAt(bar, 900.0, 200.0), {0.1, 0.1, 0.1, 1.0}) &&
                 near(cardAt(bar, 1020.0, 200.0), {0.1, 0.1, 0.1, 1.0}),
             "Bar: the bar is where its time puts it");
    bar.option = 1;
    e.expect(near(cardAt(bar, 200.0, 540.0 + 4.0), {1.0, 1.0, 1.0, 1.0}) && near(cardAt(bar, 200.0, 300.0), {0.1, 0.1, 0.1, 1.0}),
             "Bar: vertical");

    // Countdown: the flash frame is white, other frames show their digits on near black.
    auto count = cardOf(14);
    count.values = {24.0, 0.0, 0.0, 0.0};
    count.timed = {0.0, 7.0, 3.0, 0.0};
    int lit2 = 0;
    for (int y = 100; y < 980; y += 3)
        for (int x = 400; x < 1500; x += 3)
            if (cardAt(count, x + 0.5, y + 0.5)[0] > 0.9)
                ++lit2;
    e.expect(lit2 > 50 && near(cardAt(count, 5.0, 5.0), {0.1, 0.1, 0.1, 1.0}), "Countdown: digits on near black");
    count.timed[3] = 1.0;
    e.expect(near(cardAt(count, 5.0, 5.0), {1.0, 1.0, 1.0, 1.0}), "Countdown: the flash frame is white");

    // Alpha Ramp: straight color bands whose alpha rises across the width, premultiplied.
    const auto alpha = cardOf(15);
    e.expect(near(cardAt(alpha, 480.0, 90.0), {0.25 * 1.0, 0.25, 0.25, 0.25}, 1.0e-6), "Alpha: a quarter along over white");
    e.expect(near(cardAt(alpha, 480.0, 270.0), {0.25, 0.0, 0.0, 0.25}, 1.0e-6), "Alpha: over red");
    e.expect(near(cardAt(alpha, 0.0, 90.0), {0.0, 0.0, 0.0, 0.0}), "Alpha: transparent at the left edge");

    // The label: a dark plate with light text in the corner.
    auto labeled = cardOf(7);
    labeled.values = {90.0, 2.0, 0.0, 0.0};
    labeled.label = true;
    labeled.text = {19, 6, 1, 2, 15, 0, 2, 1};
    int plate = 0;
    int ink = 0;
    for (int y = 0; y < 80; ++y)
        for (int x = 0; x < 300; ++x) {
            const auto v = cardAt(labeled, x + 0.5, y + 0.5);
            if (std::abs(v[0] - 0.04) < 1.0e-6)
                ++plate;
            if (std::abs(v[0] - 0.95) < 1.0e-6)
                ++ink;
        }
    e.expect(plate > 500 && ink > 50, "Label: text on a dark plate in the corner");
}

} // namespace

int main() {
    Expectations expectations;
    testBuilderAndValidation(expectations);
    testNoise(expectations);
    testVoronoi(expectations);
    testGenerators(expectations);
    testMathAndRemap(expectations);
    testImagesAndRaster(expectations);
    testLensWarp(expectations);
    testHomography(expectations);
    testTestCard(expectations);
    testRays(expectations);
    testDistanceTransform(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
