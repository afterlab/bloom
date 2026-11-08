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
    testDistanceTransform(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
