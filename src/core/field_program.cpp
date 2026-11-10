#include "bloom/core/field_program.hpp"

#include "bloom/core/value_primitives.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

namespace bloom::core::field {

// --- The kernel's C++ dialect (field_kernel.glsl, "Each host defines the dialect first") --------

namespace kernel {
namespace {

using uint = std::uint32_t;

struct vec2 final {
    float x = 0.0F;
    float y = 0.0F;
    constexpr vec2() noexcept = default;
    constexpr vec2(const float a, const float b) noexcept : x(a), y(b) {}
};

struct vec3 final {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    constexpr vec3() noexcept = default;
    constexpr vec3(const float a, const float b, const float c) noexcept : x(a), y(b), z(c) {}
};

struct vec4 final {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 0.0F;
    constexpr vec4() noexcept = default;
    constexpr vec4(const float a, const float b, const float c, const float d) noexcept
        : x(a), y(b), z(c), w(d) {}
};

struct Context final {
    std::span<const float> table;
    const FieldImages* images = nullptr;
};

// GLSL's min and max (no NaN reaches them: every register is finite and limited).
[[maybe_unused]] inline float min(const float a, const float b) noexcept { return b < a ? b : a; }
[[maybe_unused]] inline float max(const float a, const float b) noexcept { return a < b ? b : a; }
[[maybe_unused]] inline int min(const int a, const int b) noexcept { return b < a ? b : a; }
[[maybe_unused]] inline int max(const int a, const int b) noexcept { return a < b ? b : a; }
[[maybe_unused]] inline float abs(const float v) noexcept { return std::fabs(v); }
[[maybe_unused]] inline float floor(const float v) noexcept { return std::floor(v); }
[[maybe_unused]] inline float sqrt(const float v) noexcept { return std::sqrt(v); }
[[maybe_unused]] inline float fma(const float a, const float b, const float c) noexcept {
    return std::fma(a, b, c);
}
[[maybe_unused]] inline float ldexp(const float v, const int e) noexcept { return std::ldexp(v, e); }
[[maybe_unused]] inline float frexp(const float v, int& e) noexcept { return std::frexp(v, &e); }

[[maybe_unused]] inline vec4 bfk_data(const Context& context, const int index) noexcept {
    if (index < 0)
        return {};
    const auto offset = static_cast<std::size_t>(index) * 4U;
    if (offset + 4U > context.table.size())
        return {};
    return {context.table[offset], context.table[offset + 1U], context.table[offset + 2U],
            context.table[offset + 3U]};
}

[[maybe_unused]] inline vec4 bfk_texel(const Context& context, const int slot, const int x,
                                       const int y) noexcept {
    if (context.images == nullptr || context.images->fetch == nullptr)
        return {};
    const auto texel = context.images->fetch(context.images->context, slot, x, y);
    return {texel[0], texel[1], texel[2], texel[3]};
}

[[maybe_unused]] inline float bfk_div(const float a, const float b) noexcept { return a / b; }
[[maybe_unused]] inline uint bfk_bits(const float f) noexcept { return std::bit_cast<uint>(f); }
[[maybe_unused]] inline float bfk_float(const uint u) noexcept { return std::bit_cast<float>(u); }

#define BFK_FN [[maybe_unused]] inline
#define BFK_P
#define BFK_OUT(T) T&
#define BFK_INOUT(T) T&
#define BFK_CTX const Context& bfk_context
#define BFK_CTXA bfk_context

#include "field_kernel.glsl"

#undef BFK_FN
#undef BFK_P
#undef BFK_OUT
#undef BFK_INOUT
#undef BFK_CTX
#undef BFK_CTXA

} // namespace
} // namespace kernel

namespace {

constexpr double kTau = 2.0 * std::numbers::pi;
// The largest whole lattice offset a table carries (a lattice coordinate plus it stays in int32).
constexpr double kMaxLatticeOffset = 1073741824.0; // 2^30

bool finite(const double value) noexcept { return std::isfinite(value); }

float narrow(const double value) noexcept { return static_cast<float>(value); }

void push(std::vector<float>& out, const double a, const double b, const double c,
          const double d) {
    out.push_back(narrow(a));
    out.push_back(narrow(b));
    out.push_back(narrow(c));
    out.push_back(narrow(d));
}

// A 32-bit word as two exact 16-bit halves (bfk_word).
void pushWord(std::vector<float>& out, const std::uint32_t word) {
    out.push_back(static_cast<float>(word >> 16U));
    out.push_back(static_cast<float>(word & 0xFFFFU));
}

std::uint32_t wordOfInteger(const std::int64_t value) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) & 0xFFFF'FFFFULL);
}

// The 32-bit lattice key of an authored seed: SplitMix64 (hashWord, kValueHashSemanticsVersion)
// of the seed, its stream and octave folded in by a second finalization, so a seed is hashed with
// the lattice and never added to a coordinate.
std::uint32_t seedKey(const std::int64_t seed, const std::uint32_t stream,
                      const std::uint32_t octave) noexcept {
    const auto mixed = primitives::hashWord(seed) ^
                       (static_cast<std::uint64_t>(stream) << 32U) ^ octave;
    return static_cast<std::uint32_t>(primitives::hashWord(static_cast<std::int64_t>(mixed)) >> 32U);
}

bool allFinite(const std::vector<float>& values) noexcept {
    return std::all_of(values.begin(), values.end(), [](const float v) {
        return std::isfinite(v) && std::fabs(v) <= 1.0e30F;
    });
}

std::optional<std::vector<float>> checked(std::vector<float> values) {
    if (!allFinite(values))
        return std::nullopt;
    return values;
}

double fractional(const double value) noexcept { return value - std::floor(value); }

// A whole-plus-fraction split of a lattice translation; false beyond the representable offset.
bool splitLattice(const double value, std::int64_t& whole, double& fraction) noexcept {
    if (!finite(value))
        return false;
    const auto f = std::floor(value);
    if (std::fabs(f) > kMaxLatticeOffset)
        return false;
    whole = static_cast<std::int64_t>(f);
    fraction = value - f;
    return true;
}

void pushCurve(std::vector<float>& out, const FieldCurve& curve) {
    const auto n = curve.knots.size();
    push(out, static_cast<double>(n), 0.0, 0.0, 0.0);
    const auto packed = [&out](const std::vector<double>& values, const std::size_t count) {
        const auto padded = (count + 3U) / 4U * 4U;
        for (std::size_t i = 0; i < padded; ++i)
            out.push_back(i < values.size() ? narrow(values[i]) : 0.0F);
    };
    packed(curve.knots, n);
    packed(curve.origins, n + 1U);
    for (std::size_t s = 0; s <= n; ++s) {
        const auto segment = s < curve.segments.size() ? curve.segments[s]
                                                       : std::array<double, 4>{0.0, 0.0, 0.0, 0.0};
        push(out, segment[0], segment[1], segment[2], segment[3]);
    }
}

bool validCurve(const FieldCurve& curve) noexcept {
    const auto n = curve.knots.size();
    return n >= 1U && n <= kMaxFieldCurveKnots && curve.origins.size() == n + 1U &&
           curve.segments.size() == n + 1U;
}

std::size_t curveVec4s(const int n) noexcept {
    const auto count = static_cast<std::size_t>(n);
    return 1U + (count + 3U) / 4U + (count + 4U) / 4U + count + 1U;
}

} // namespace

// --- Noise -----------------------------------------------------------------------------------

double noiseEvolutionPhase(const double evolution, const bool loop, const double loopSeconds,
                           const double seconds, std::int64_t& period) noexcept {
    period = 0;
    if (!loop || !(loopSeconds > 0.0) || !finite(seconds) || !finite(evolution))
        return finite(evolution) ? evolution : 0.0;
    const auto cycles = std::max(1.0, std::round(std::fabs(evolution)));
    if (cycles > 1.0e6)
        return 0.0;
    period = static_cast<std::int64_t>(cycles);
    const auto position = fractional(seconds / loopSeconds);
    const auto phase = position * cycles;
    return phase >= cycles ? 0.0 : phase;
}

std::vector<float> encodeConstant(const std::array<double, 4> value) {
    std::vector<float> out;
    push(out, value[0], value[1], value[2], value[3]);
    return out;
}

std::optional<std::vector<float>> encodeNoise(const NoiseParameters& p) {
    if (!finite(p.sizeX) || !finite(p.sizeY) || std::fabs(p.sizeX) <= 1.0e-6 ||
        std::fabs(p.sizeY) <= 1.0e-6 || !finite(p.octaves) || !finite(p.lacunarity) ||
        !finite(p.gain) || !finite(p.evolution) || !finite(p.rotationDegrees) ||
        !finite(p.octaveRotationDegrees) || !finite(p.offsetX) || !finite(p.offsetY) ||
        !finite(p.octaveOffsetX) || !finite(p.octaveOffsetY) || p.loopPeriod < 0 ||
        p.loopPeriod > 1'000'000)
        return std::nullopt;
    const auto fractalNone = p.fractal == NoiseFractal::None;
    const auto octaves = fractalNone ? 1.0
                                     : std::clamp(p.octaves, 1.0,
                                                  static_cast<double>(kMaxNoiseOctaves));
    const auto count = static_cast<int>(std::ceil(octaves - 1.0e-9));
    const auto lastWeight = octaves - std::floor(octaves) > 1.0e-9 ? octaves - std::floor(octaves)
                                                                    : 1.0;
    const auto lacunarity = std::clamp(p.lacunarity, 1.0, 10.0);
    const auto gain = std::clamp(p.gain, 0.0, 1.0);
    const auto simplex = p.type == NoiseType::Simplex;
    const auto period = p.flow ? 0 : p.loopPeriod;
    const auto typeCode = [&]() -> int {
        switch (p.type) {
        case NoiseType::Perlin:
            return 0;
        case NoiseType::Simplex:
            return 1;
        case NoiseType::Linear:
            return 2;
        case NoiseType::SoftLinear:
            return 3;
        case NoiseType::Block:
            return 4;
        case NoiseType::Spline:
            return 5;
        }
        return 0;
    }();
    const auto fractalCode = static_cast<int>(p.fractal);

    std::vector<double> weights;
    double weightSum = 0.0;
    for (int k = 0; k < count; ++k) {
        auto w = std::pow(gain, static_cast<double>(k));
        if (k == count - 1)
            w *= lastWeight;
        weights.push_back(w);
        weightSum += w;
    }
    if (!(weightSum > 0.0))
        weightSum = 1.0;
    auto norm = 1.0 / weightSum;
    if (p.fractal == NoiseFractal::Ridged)
        norm /= std::max(p.ridgeOffset * p.ridgeOffset, 1.0e-6);

    std::vector<float> out;
    push(out, typeCode, fractalCode, count, p.flow ? 1.0 : 0.0);
    push(out, p.ridgeOffset, p.ridgeGain, norm, static_cast<double>(period));
    push(out, p.contrast, p.brightness, p.invert ? 1.0 : 0.0, static_cast<double>(p.overflow));
    push(out, p.bipolar ? 1.0 : 0.0, 0.0, 0.0, 0.0);

    const auto baseAngle = -p.rotationDegrees / 360.0;
    for (int k = 0; k < count; ++k) {
        const auto kd = static_cast<double>(k);
        // The evolution this octave samples: Dynamic advances octave k (k + 1) times as fast,
        // Dynamic Progressive 2^k times; Dynamic Twist turns octave k by k times the evolution.
        auto evolution = p.evolution;
        auto twistTurns = 0.0;
        if (p.fractal == NoiseFractal::Dynamic)
            evolution = p.evolution * (kd + 1.0);
        else if (p.fractal == NoiseFractal::DynamicProgressive)
            evolution = p.evolution * std::ldexp(1.0, k);
        else if (p.fractal == NoiseFractal::DynamicTwist)
            twistTurns = kd * p.evolution;
        if (period > 0) {
            // Whole periods leave a periodic noise unchanged; reducing keeps every value small.
            evolution = std::fmod(evolution, static_cast<double>(period));
            if (evolution < 0.0)
                evolution += static_cast<double>(period);
        }
        const auto turns = baseAngle - kd * p.octaveRotationDegrees / 360.0 - twistTurns;
        const auto angle = kTau * fractional(turns);
        const auto c = std::cos(angle);
        const auto s = std::sin(angle);
        const auto scale = std::pow(lacunarity, kd);
        const auto kx = scale / p.sizeX;
        const auto ky = scale / p.sizeY;
        // Lattice point q = M (p - centre) with M = diag(kx, ky) R(-rotation), centre = offset +
        // k * octave offset.
        const auto m00 = kx * c;
        const auto m01 = -kx * s;
        const auto m10 = ky * s;
        const auto m11 = ky * c;
        const auto centreX = p.offsetX + kd * p.octaveOffsetX;
        const auto centreY = p.offsetY + kd * p.octaveOffsetY;
        // Each octave also sits at its own whole-cell translation, so its lattice never aligns
        // with the previous octave's at the origin.
        const auto shift = 37.0 * kd;
        auto tx = -(m00 * centreX + m01 * centreY) + shift;
        auto ty = -(m10 * centreX + m11 * centreY) + shift;
        auto tz = simplex && !p.flow ? 3.0 * evolution : evolution;
        std::int64_t ox = 0;
        std::int64_t oy = 0;
        std::int64_t oz = 0;
        double fx = 0.0;
        double fy = 0.0;
        double fz = 0.0;
        double alpha = 0.0;
        if (p.flow) {
            alpha = fractional(evolution);
            tz = 0.0;
        }
        if (simplex) {
            // Split the translation in the skewed lattice (Gustavson 2005), so the whole part is a
            // lattice symmetry and the remainder is small.
            if (p.flow) {
                const auto skew = (tx + ty) * (std::sqrt(3.0) - 1.0) / 2.0;
                double rx = 0.0;
                double ry = 0.0;
                if (!splitLattice(tx + skew, ox, rx) || !splitLattice(ty + skew, oy, ry))
                    return std::nullopt;
                const auto unskew = (rx + ry) * (3.0 - std::sqrt(3.0)) / 6.0;
                fx = rx - unskew;
                fy = ry - unskew;
            } else {
                const auto skew = (tx + ty + tz) / 3.0;
                double rx = 0.0;
                double ry = 0.0;
                double rz = 0.0;
                if (!splitLattice(tx + skew, ox, rx) || !splitLattice(ty + skew, oy, ry) ||
                    !splitLattice(tz + skew, oz, rz))
                    return std::nullopt;
                const auto unskew = (rx + ry + rz) / 6.0;
                fx = rx - unskew;
                fy = ry - unskew;
                fz = rz - unskew;
            }
        } else if (!splitLattice(tx, ox, fx) || !splitLattice(ty, oy, fy) ||
                   !splitLattice(tz, oz, fz)) {
            return std::nullopt;
        }
        push(out, m00, m01, m10, m11);
        push(out, fx, fy, fz, weights[static_cast<std::size_t>(k)]);
        pushWord(out, wordOfInteger(ox));
        pushWord(out, wordOfInteger(oy));
        pushWord(out, wordOfInteger(oz));
        pushWord(out, seedKey(p.seed, p.stream, static_cast<std::uint32_t>(k)));
        push(out, alpha, std::max(std::fabs(kx), std::fabs(ky)), 0.0, 0.0);
    }
    return checked(std::move(out));
}

// --- Generators ---------------------------------------------------------------------------------

std::optional<std::vector<float>> encodeVoronoi(const VoronoiParameters& p) {
    if (!finite(p.size) || std::fabs(p.size) <= 1.0e-6 || !finite(p.jitter) ||
        !finite(p.smoothness) || !finite(p.evolution))
        return std::nullopt;
    const auto size = std::fabs(p.size);
    std::vector<float> out;
    push(out, static_cast<double>(p.mode), static_cast<double>(p.metric),
         std::clamp(p.jitter, 0.0, 1.0), std::max(p.smoothness, 0.0) / size);
    push(out, 1.0 / size, size, static_cast<double>(std::clamp<std::int64_t>(p.octaves, 1, 4)),
         static_cast<double>(p.selector));
    pushWord(out, seedKey(p.seed, 0U, 0U));
    out.push_back(narrow(fractional(p.evolution)));
    out.push_back(p.edges ? 1.0F : 0.0F);
    return checked(std::move(out));
}

std::optional<std::vector<float>> encodeRandom(const RandomParameters& p) {
    if (!finite(p.cellX) || !finite(p.cellY) || std::fabs(p.cellX) <= 1.0e-6 ||
        std::fabs(p.cellY) <= 1.0e-6 || !finite(p.evolution) ||
        std::fabs(p.evolution) > 9.0e15)
        return std::nullopt;
    const auto whole = std::floor(p.evolution);
    std::vector<float> out;
    out.push_back(narrow(1.0 / std::fabs(p.cellX)));
    out.push_back(narrow(1.0 / std::fabs(p.cellY)));
    pushWord(out, seedKey(p.seed, 2U, 0U));
    pushWord(out, wordOfInteger(static_cast<std::int64_t>(whole)));
    out.push_back(narrow(p.evolution - whole));
    out.push_back(p.smooth ? 1.0F : 0.0F);
    push(out, p.gaussian ? 1.0 : 0.0, static_cast<double>(p.selector), 0.0, 0.0);
    return checked(std::move(out));
}

std::optional<std::vector<float>> encodePattern(const PatternParameters& p) {
    if (!finite(p.size) || std::fabs(p.size) <= 1.0e-6 || !finite(p.width) || !finite(p.phase) ||
        !finite(p.rotationDegrees) || !finite(p.centreX) || !finite(p.centreY) ||
        !finite(p.softness))
        return std::nullopt;
    const auto size = std::fabs(p.size);
    const auto angle = kTau * fractional(p.rotationDegrees / 360.0);
    std::vector<float> out;
    push(out, static_cast<double>(p.shape), static_cast<double>(p.waveform),
         std::clamp(p.width, 0.0, 1.0), fractional(p.phase));
    push(out, std::cos(angle), std::sin(angle), p.centreX, p.centreY);
    push(out, 1.0 / size, std::max(p.softness, 0.0) / size, 0.0, 0.0);
    return checked(std::move(out));
}

std::optional<std::vector<float>> encodeGradient(const GradientFieldParameters& p) {
    if (!finite(p.startX) || !finite(p.startY) || !finite(p.endX) || !finite(p.endY) ||
        !finite(p.cycle))
        return std::nullopt;
    const auto dx = p.endX - p.startX;
    const auto dy = p.endY - p.startY;
    const auto length2 = dx * dx + dy * dy;
    const auto inv2 = length2 > 1.0e-12 ? 1.0 / length2 : 0.0;
    const auto inv = length2 > 1.0e-12 ? 1.0 / std::sqrt(length2) : 0.0;
    std::vector<float> out;
    push(out, static_cast<double>(p.mode), static_cast<double>(p.repeat), p.cycle, 0.0);
    push(out, p.startX, p.startY, dx, dy);
    push(out, inv2, inv, 0.0, 0.0);
    return checked(std::move(out));
}

std::optional<std::vector<float>> encodeShape(const ShapeParameters& p) {
    if (!finite(p.centreX) || !finite(p.centreY) || !finite(p.sizeX) || !finite(p.sizeY) ||
        !finite(p.rotationDegrees) || !finite(p.cornerRadius) || !finite(p.innerRatio) ||
        !finite(p.falloff) || !finite(p.unitsPerTexel) || p.slot >= kMaxFieldImages)
        return std::nullopt;
    const auto angle = kTau * fractional(p.rotationDegrees / 360.0);
    const auto hx = std::fabs(p.sizeX) * 0.5;
    const auto hy = std::fabs(p.sizeY) * 0.5;
    auto corner = std::max(p.cornerRadius, 0.0);
    const auto ratio = std::clamp(p.innerRatio, 0.0, 1.0);
    double halfX = hx;
    double halfY = hy;
    double sector = 0.0;
    double ax = 0.0;
    double bx = 0.0;
    double by = 0.0;
    switch (p.primitive) {
    case ShapePrimitive::Circle:
        break;
    case ShapePrimitive::Box:
        break;
    case ShapePrimitive::RoundedBox:
        corner = std::min(corner, std::min(hx, hy));
        break;
    case ShapePrimitive::Segment:
        // A capsule along x: the segment of half length size x / 2, size y thick.
        corner = hy;
        break;
    case ShapePrimitive::Ring: {
        // The annulus between diameters size x and size x times the inner ratio.
        const auto inner = hx * ratio;
        halfX = (hx + inner) * 0.5;
        corner = (hx - inner) * 0.5;
        break;
    }
    case ShapePrimitive::Polygon:
    case ShapePrimitive::Star: {
        const auto sides = static_cast<double>(std::clamp<std::int64_t>(p.sides, 3, 64));
        sector = 1.0 / sides;
        const auto halfAngle = std::numbers::pi / sides;
        // Fold to [0, pi / sides]: the edge from the vertex A = (R, 0) to B, the polygon's edge
        // midpoint or the star's inner vertex.
        const auto inner = p.primitive == ShapePrimitive::Polygon ? hx * std::cos(halfAngle)
                                                                  : hx * ratio;
        ax = hx;
        bx = inner * std::cos(halfAngle);
        by = inner * std::sin(halfAngle);
        break;
    }
    }
    std::vector<float> out;
    push(out, p.mask ? 1.0 : 0.0, static_cast<double>(p.primitive), std::max(p.falloff, 0.0),
         static_cast<double>(p.curve));
    push(out, p.centreX, p.centreY, std::cos(angle), std::sin(angle));
    push(out, halfX, halfY, corner, sector);
    push(out, ax, bx, by, p.invert ? 1.0 : 0.0);
    push(out, static_cast<double>(p.slot), p.unitsPerTexel, 0.0, 0.0);
    return checked(std::move(out));
}

std::vector<float> encodeImage(const ImageParameters& p) {
    std::vector<float> out;
    push(out, static_cast<double>(std::min<std::size_t>(p.slot, kMaxFieldImages - 1U)),
         static_cast<double>(p.filter), static_cast<double>(p.edge),
         static_cast<double>(p.selector));
    push(out, static_cast<double>(p.channel), static_cast<double>(p.first),
         static_cast<double>(p.second), 0.0);
    return out;
}

std::optional<std::vector<float>> encodeForce(const ForceParameters& p) {
    if (!finite(p.centreX) || !finite(p.centreY) || !finite(p.strength) || !finite(p.radius) ||
        !finite(p.angleDegrees))
        return std::nullopt;
    const auto curve = p.falloff == ForceFalloff::Curve && p.curve.has_value() &&
                       validCurve(*p.curve);
    const auto falloff = p.falloff == ForceFalloff::Curve && !curve ? ForceFalloff::Linear
                                                                    : p.falloff;
    const auto radius = std::max(std::fabs(p.radius), 1.0e-6);
    const auto angle = kTau * fractional(p.angleDegrees / 360.0);
    std::vector<float> out;
    push(out, static_cast<double>(p.mode), static_cast<double>(falloff), p.strength,
         1.0 / radius);
    push(out, p.centreX, p.centreY, p.clockwise ? -1.0 : 1.0, curve ? 3.0 : 0.0);
    push(out, std::cos(angle), std::sin(angle), 0.0, 0.0);
    if (curve)
        pushCurve(out, *p.curve);
    return checked(std::move(out));
}

std::optional<std::vector<float>> encodeMath(const MathParameters& p) {
    if (!finite(p.smoothness) || !finite(p.angleDegrees))
        return std::nullopt;
    const auto angle = kTau * fractional(p.angleDegrees / 360.0);
    std::vector<float> out;
    push(out, static_cast<double>(p.operation), std::max(p.smoothness, 0.0), std::cos(angle),
         std::sin(angle));
    return checked(std::move(out));
}

std::optional<std::vector<float>> encodeRemap(const RemapParameters& p) {
    if (!finite(p.inLow) || !finite(p.inHigh) || !finite(p.outLow) || !finite(p.outHigh) ||
        !finite(p.bias) || !finite(p.gain))
        return std::nullopt;
    const auto range = p.inHigh - p.inLow;
    // Equal input bounds map every value to the output's low end.
    const auto inverse = std::fabs(range) > 1.0e-12 ? 1.0 / range : 0.0;
    const auto curve = p.curve.has_value() && validCurve(*p.curve);
    std::vector<float> out;
    push(out, p.inLow, inverse, p.outLow, p.outHigh - p.outLow);
    push(out, p.clamp ? 1.0 : 0.0, p.invert ? 1.0 : 0.0,
         static_cast<double>(std::clamp<std::int64_t>(p.steps, 0, 1024)), p.vector ? 1.0 : 0.0);
    push(out, std::clamp(p.bias, 0.001, 0.999), std::clamp(p.gain, 0.001, 0.999),
         curve ? 3.0 : -1.0, 0.0);
    if (curve)
        pushCurve(out, *p.curve);
    return checked(std::move(out));
}

std::optional<std::vector<float>> encodeTransform(const TransformParameters& p) {
    if (!finite(p.translateX) || !finite(p.translateY) || !finite(p.rotationDegrees) ||
        !finite(p.scaleX) || !finite(p.scaleY) || !finite(p.pivotX) || !finite(p.pivotY) ||
        !finite(p.tileX) || !finite(p.tileY) || !finite(p.warpAmount) ||
        std::fabs(p.scaleX) <= 1.0e-9 || std::fabs(p.scaleY) <= 1.0e-9)
        return std::nullopt;
    // The field moves by the transform: the source coordinate is its inverse,
    // A = S^-1 R(-theta), b = pivot - A (pivot + translate).
    const auto angle = kTau * fractional(p.rotationDegrees / 360.0);
    const auto c = std::cos(angle);
    const auto s = std::sin(angle);
    const auto a00 = c / p.scaleX;
    const auto a01 = s / p.scaleX;
    const auto a10 = -s / p.scaleY;
    const auto a11 = c / p.scaleY;
    const auto qx = p.pivotX + p.translateX;
    const auto qy = p.pivotY + p.translateY;
    const auto bx = p.pivotX - (a00 * qx + a01 * qy);
    const auto by = p.pivotY - (a10 * qx + a11 * qy);
    const auto tileX = std::max(std::fabs(p.tileX), 1.0e-3);
    const auto tileY = std::max(std::fabs(p.tileY), 1.0e-3);
    std::vector<float> out;
    push(out, a00, a01, a10, a11);
    push(out, bx, by, static_cast<double>(p.mode),
         static_cast<double>(std::clamp<std::int64_t>(p.segments, 1, 64)));
    push(out, tileX, tileY, 1.0 / tileX, 1.0 / tileY);
    push(out, p.warpAmount, 0.0, 0.0, 0.0);
    return checked(std::move(out));
}

std::optional<std::vector<float>> encodeLensWarp(const LensWarpParameters& p) {
    if (!finite(p.k1) || !finite(p.k2) || !finite(p.p1) || !finite(p.p2) || !finite(p.centreX) ||
        !finite(p.centreY) || !finite(p.scale) || !(p.scale > 1.0e-6))
        return std::nullopt;
    std::vector<float> out;
    push(out, p.k1, p.k2, p.p1, p.p2);
    push(out, p.centreX, p.centreY, p.scale, p.reverse ? 1.0 : 0.0);
    return checked(std::move(out));
}

std::optional<std::vector<float>> encodeTestCard(const TestCardParameters& p) {
    if (p.card < 0 || p.card > 15 || p.variant < 0 || p.variant > 4096 || p.option < 0 ||
        p.option > 4096 || p.text.size() > kTestCardMaxLabel || !finite(p.width) ||
        !finite(p.height) || !(p.width >= 1.0) || !(p.height >= 1.0) || !finite(p.centerX) ||
        !finite(p.centerY))
        return std::nullopt;
    for (const auto v : p.values)
        if (!finite(v))
            return std::nullopt;
    for (const auto v : p.timed)
        if (!finite(v))
            return std::nullopt;
    for (const auto c : p.text)
        if (c > 42)
            return std::nullopt;
    std::vector<float> out;
    const auto flags = (p.label ? 1.0 : 0.0) + 16.0 * static_cast<double>(p.text.size());
    push(out, static_cast<double>(p.card), static_cast<double>(p.variant),
         static_cast<double>(p.option), flags);
    push(out, p.width, p.height, p.centerX, p.centerY);
    push(out, p.values[0], p.values[1], p.values[2], p.values[3]);
    push(out, p.timed[0], p.timed[1], p.timed[2], p.timed[3]);
    // Three six-bit codes to a lane, eight lanes.
    for (std::size_t lane = 0; lane < 8; ++lane) {
        std::uint32_t word = 0;
        for (std::size_t k = 0; k < 3; ++k) {
            const auto at = lane * 3U + k;
            if (at < p.text.size())
                word |= static_cast<std::uint32_t>(p.text[at]) << (6U * k);
        }
        out.push_back(static_cast<float>(word));
    }
    return checked(std::move(out));
}

std::optional<std::vector<float>> encodeRays(const RaysParameters& p) {
    if (!finite(p.centreX) || !finite(p.centreY) || !finite(p.length) || !finite(p.falloff) ||
        !finite(p.boost) || p.length < 0.0 || p.length > 1.0 || p.falloff < 0.0 || p.boost < 0.0 ||
        p.source > 1U)
        return std::nullopt;
    std::vector<float> out;
    push(out, static_cast<double>(std::min<std::size_t>(p.slot, kMaxFieldImages - 1U)),
         static_cast<double>(p.source), p.centreX, p.centreY);
    push(out, p.length, p.falloff, p.boost, 0.0);
    return checked(std::move(out));
}

namespace {

// Solves the 8 x 8 system `a x = b` by Gaussian elimination with partial pivoting. False when a
// pivot is not above `floor` (the system is singular for this purpose).
bool solveEight(std::array<std::array<double, 9>, 8>& rows, std::array<double, 8>& x,
                const double floor) {
    for (std::size_t column = 0; column < 8; ++column) {
        std::size_t best = column;
        for (std::size_t row = column + 1; row < 8; ++row)
            if (std::fabs(rows[row][column]) > std::fabs(rows[best][column]))
                best = row;
        if (!(std::fabs(rows[best][column]) > floor))
            return false;
        std::swap(rows[column], rows[best]);
        for (std::size_t row = column + 1; row < 8; ++row) {
            const double factor = rows[row][column] / rows[column][column];
            for (std::size_t k = column; k < 9; ++k)
                rows[row][k] -= factor * rows[column][k];
        }
    }
    for (std::size_t row = 8; row-- > 0;) {
        double value = rows[row][8];
        for (std::size_t k = row + 1; k < 8; ++k)
            value -= rows[row][k] * x[k];
        x[row] = value / rows[row][row];
    }
    return true;
}

} // namespace

bool homographyFromQuads(const std::array<double, 8>& from, const std::array<double, 8>& to,
                         HomographyParameters& out) {
    out = HomographyParameters{};
    out.valid = false;
    for (std::size_t i = 0; i < 8; ++i)
        if (!finite(from[i]) || !finite(to[i]) || std::fabs(from[i]) > 1.0e9 ||
            std::fabs(to[i]) > 1.0e9)
            return false;
    double cdx = 0.0;
    double cdy = 0.0;
    double csx = 0.0;
    double csy = 0.0;
    for (std::size_t i = 0; i < 4; ++i) {
        cdx += 0.25 * to[2 * i];
        cdy += 0.25 * to[2 * i + 1];
        csx += 0.25 * from[2 * i];
        csy += 0.25 * from[2 * i + 1];
    }
    double reach = 0.0;
    for (std::size_t i = 0; i < 4; ++i) {
        reach = std::max({reach, std::fabs(to[2 * i] - cdx), std::fabs(to[2 * i + 1] - cdy),
                          std::fabs(from[2 * i] - csx), std::fabs(from[2 * i + 1] - csy)});
    }
    if (!(reach > 1.0e-6))
        return false;
    std::array<std::array<double, 9>, 8> rows{};
    for (std::size_t i = 0; i < 4; ++i) {
        const double x = (to[2 * i] - cdx) / reach;
        const double y = (to[2 * i + 1] - cdy) / reach;
        const double sx = (from[2 * i] - csx) / reach;
        const double sy = (from[2 * i + 1] - csy) / reach;
        rows[2 * i] = {x, y, 1.0, 0.0, 0.0, 0.0, -x * sx, -y * sx, sx};
        rows[2 * i + 1] = {0.0, 0.0, 0.0, x, y, 1.0, -x * sy, -y * sy, sy};
    }
    std::array<double, 8> h{};
    if (!solveEight(rows, h, 1.0e-9))
        return false;
    // Back to pixels about the centroids: dx = reach x, and the source is csx + reach X.
    const double g = h[6] / reach;
    const double k = h[7] / reach;
    out.centreX = cdx;
    out.centreY = cdy;
    out.m = {h[0] + csx * g,     h[1] + csx * k,     h[2] * reach + csx,
             h[3] + csy * g,     h[4] + csy * k,     h[5] * reach + csy,
             g,                  k,                  1.0};
    for (const auto v : out.m)
        if (!finite(v))
            return false;
    out.valid = true;
    return true;
}

HomographyParameters similarityHomography(const double offsetX, const double offsetY,
                                          const double angleDegrees, const double scale,
                                          const double pivotX, const double pivotY) {
    HomographyParameters out;
    if (!finite(offsetX) || !finite(offsetY) || !finite(angleDegrees) || !finite(scale) ||
        !finite(pivotX) || !finite(pivotY) || !(std::fabs(scale) > 1.0e-9)) {
        out.valid = false;
        return out;
    }
    constexpr double kRadians = 0.017453292519943295;
    const double c = std::cos(angleDegrees * kRadians) / scale;
    const double s = std::sin(angleDegrees * kRadians) / scale;
    out.centreX = pivotX + offsetX;
    out.centreY = pivotY + offsetY;
    out.m = {c, s, pivotX, -s, c, pivotY, 0.0, 0.0, 1.0};
    return out;
}

std::optional<std::vector<float>> encodeProceduralWarp(const ProceduralWarpParameters& p) {
    const std::array values{p.centreX,     p.centreY,      p.radius,       p.angleDegrees,
                            p.falloff,     p.amount,       p.refraction,   p.wavelength,
                            p.amplitude,   p.phaseDegrees, p.speed,        p.seconds,
                            p.directionDegrees, p.frameWidth, p.frameHeight, p.interpolation,
                            p.magnification, p.feather};
    for (const auto v : values)
        if (!finite(v))
            return std::nullopt;
    const auto mode = static_cast<std::uint8_t>(p.mode);
    if (mode >= kProceduralWarpModeCount || static_cast<std::uint8_t>(p.wave) > 3U ||
        p.pinnedEdges > 15U || !(p.wavelength > 1.0e-6) || !(p.refraction >= 1.0) ||
        !(p.magnification > 1.0e-6) || !(p.frameWidth >= 1.0) || !(p.frameHeight >= 1.0))
        return std::nullopt;
    const auto radius = std::max(p.radius, 0.0);
    const auto inverseRadius = radius > 0.0 ? 1.0 / radius : 0.0;
    const auto falloff = std::clamp(p.falloff, 0.0, 1.0);
    const auto inverseFalloff = falloff > 0.0 ? 1.0 / falloff : 0.0;
    // The phase in cycles at this time, reduced to [0, 1) here so the kernel's argument stays small.
    const auto phase = fractional(p.phaseDegrees / 360.0 + p.speed * p.seconds);
    std::array<double, 11> a{};
    switch (p.mode) {
    case ProceduralWarpMode::Twirl:
        a[0] = p.angleDegrees / 360.0;
        a[1] = inverseFalloff;
        break;
    case ProceduralWarpMode::BulgePinch:
        a[0] = std::clamp(p.amount, -1.0, 1.0);
        a[1] = inverseFalloff;
        break;
    case ProceduralWarpMode::Spherize:
        a[0] = std::clamp(p.amount, -1.0, 1.0);
        a[1] = 1.0 / p.refraction;
        break;
    case ProceduralWarpMode::Ripple:
        a[0] = inverseFalloff;
        a[1] = 1.0 / p.wavelength;
        a[2] = p.amplitude;
        a[3] = phase;
        a[4] = 2.0 / p.wavelength;
        break;
    case ProceduralWarpMode::Wave: {
        const auto direction = kTau * fractional(p.directionDegrees / 360.0);
        a[0] = std::sin(direction);
        a[1] = std::cos(direction);
        a[2] = 1.0 / p.wavelength;
        a[3] = p.amplitude;
        a[4] = phase;
        a[5] = static_cast<double>(p.wave);
        a[6] = static_cast<double>(p.pinnedEdges);
        a[7] = 0.5 * p.frameWidth;
        a[8] = 0.5 * p.frameHeight;
        a[9] = 1.0 / p.wavelength;
        break;
    }
    case ProceduralWarpMode::Polar: {
        const auto polarRadius = 0.5 * std::min(p.frameWidth, p.frameHeight);
        a[0] = p.polarToRect ? 1.0 : 0.0;
        a[1] = std::clamp(p.interpolation, 0.0, 1.0);
        a[2] = p.frameWidth;
        a[3] = p.frameHeight;
        a[4] = 1.0 / p.frameWidth;
        a[5] = 1.0 / p.frameHeight;
        a[6] = polarRadius;
        a[7] = p.frameHeight / polarRadius;
        break;
    }
    case ProceduralWarpMode::Magnify:
        a[0] = p.squareShape ? 1.0 : 0.0;
        a[1] = 1.0 / p.magnification;
        a[2] = p.feather > 0.0 ? 1.0 / p.feather : 0.0;
        break;
    }
    std::vector<float> out;
    push(out, static_cast<double>(mode), p.centreX, p.centreY, radius);
    push(out, inverseRadius, a[0], a[1], a[2]);
    push(out, a[3], a[4], a[5], a[6]);
    push(out, a[7], a[8], a[9], a[10]);
    return checked(std::move(out));
}

std::optional<std::vector<float>> encodeHomography(const HomographyParameters& p) {
    if (!finite(p.centreX) || !finite(p.centreY) || p.windowSlot >= static_cast<int>(kMaxFieldImages))
        return std::nullopt;
    for (const auto v : p.m)
        if (!finite(v))
            return std::nullopt;
    for (const auto v : p.offsets)
        if (!finite(v))
            return std::nullopt;
    // A map no binary32 holds is the invalid map, not an error. A window map is invalid until its
    // window is placed.
    bool valid = p.valid && p.windowSlot < 0;
    for (std::size_t i = 0; i < 8; ++i)
        valid = valid && std::fabs(p.m[i]) < 1.0e15;
    std::vector<float> out;
    push(out, p.windowSlot >= 0 ? 1.0 : 0.0, static_cast<double>(std::max(p.windowSlot, 0)),
         p.windowSlot >= 0 && p.windowCentred ? 1.0 : 0.0, 0.0);
    push(out, p.offsets[0], p.offsets[1], p.offsets[2], p.offsets[3]);
    push(out, p.offsets[4], p.offsets[5], p.offsets[6], p.offsets[7]);
    if (!valid) {
        push(out, 0.0, 0.0, 0.0, 0.0);
        push(out, 0.0, 0.0, 0.0, 0.0);
        push(out, 0.0, 0.0, 0.0, 0.0);
        return checked(std::move(out));
    }
    push(out, p.m[0], p.m[1], p.m[2], p.m[3]);
    push(out, p.m[4], p.m[5], p.m[6], p.m[7]);
    push(out, p.centreX, p.centreY, 1.0, 0.0);
    return checked(std::move(out));
}

std::vector<float> encodeSwizzle(const SwizzleParameters& p) {
    std::vector<float> out;
    push(out, std::min<int>(p.lanes[0], 5), std::min<int>(p.lanes[1], 5),
         std::min<int>(p.lanes[2], 5), std::min<int>(p.lanes[3], 5));
    push(out, p.scales[0], p.scales[1], p.scales[2], p.scales[3]);
    return out;
}

std::vector<float> encodeOffset(const double dx, const double dy) {
    std::vector<float> out;
    push(out, dx, dy, 0.0, 0.0);
    return out;
}

std::vector<float> encodeDerivative(const FieldDerivativeMode mode, const double step,
                                    const double strength) {
    const auto h = std::max(std::fabs(step), 1.0e-3);
    std::vector<float> out;
    push(out, static_cast<double>(mode), 1.0 / (2.0 * h), strength, 0.0);
    return out;
}

std::vector<float> encodeCellCentre(const double spacing) {
    const auto s = std::max(std::fabs(spacing), 1.0);
    std::vector<float> out;
    push(out, s, 1.0 / s, 0.0, 0.0);
    return out;
}

std::vector<float> encodeArrow(const double spacing, const double lengthPerUnit,
                               const double halfWidth) {
    std::vector<float> out;
    push(out, std::max(std::fabs(spacing), 1.0), lengthPerUnit, std::max(halfWidth, 0.0), 0.0);
    return out;
}

std::vector<float> encodeStreak(const double step, const double noiseCell) {
    std::vector<float> out;
    push(out, step, 1.0 / std::max(std::fabs(noiseCell), 1.0e-3), 0.0, 0.0);
    return out;
}

std::optional<std::vector<float>> encodeRamp(const double low, const double high,
                                             const std::span<const std::array<double, 4>> colours) {
    if (!finite(low) || !finite(high) || colours.size() != kFieldRampEntries)
        return std::nullopt;
    const auto range = high - low;
    std::vector<float> out;
    push(out, low, std::fabs(range) > 1.0e-12 ? 1.0 / range : 0.0, 0.0, 0.0);
    for (const auto& c : colours)
        push(out, c[0], c[1], c[2], c[3]);
    return checked(std::move(out));
}

// --- The table builder --------------------------------------------------------------------------

bool FieldProgramBuilder::emit(const FieldOp op, const int output, const FieldInputs inputs,
                               const std::span<const float> parameters) {
    if (code_.size() >= kMaxFieldInstructions || output < 0 ||
        output >= static_cast<int>(kFieldRegisterCount))
        return false;
    for (const auto input : inputs) {
        if (input < -1 || input >= static_cast<int>(kFieldRegisterCount))
            return false;
    }
    Instruction instruction;
    instruction.op = op;
    instruction.output = output;
    instruction.inputs = inputs;
    instruction.parameterOffset = parameters_.size() / 4U;
    parameters_.insert(parameters_.end(), parameters.begin(), parameters.end());
    while (parameters_.size() % 4U != 0U)
        parameters_.push_back(0.0F);
    code_.push_back(instruction);
    return true;
}

std::optional<std::vector<float>> FieldProgramBuilder::finish(const int resultRegister,
                                                              const FieldResultKind kind,
                                                              const FieldOutputMode mode) const {
    if (code_.empty() || resultRegister < 0 ||
        resultRegister >= static_cast<int>(kFieldRegisterCount))
        return std::nullopt;
    const auto parameterBase = kFieldTableCode + 2U * code_.size();
    const auto floats = (parameterBase * 4U) + parameters_.size();
    if (floats > kMaxFieldTableFloats)
        return std::nullopt;
    std::vector<float> table(floats, 0.0F);
    const auto put = [&table](const std::size_t vec4, const float a, const float b, const float c,
                              const float d) {
        table[vec4 * 4U] = a;
        table[vec4 * 4U + 1U] = b;
        table[vec4 * 4U + 2U] = c;
        table[vec4 * 4U + 3U] = d;
    };
    put(kFieldTableHeader, kFieldTableMagic, static_cast<float>(kFieldProgramSemanticsVersion),
        static_cast<float>(code_.size()), static_cast<float>(resultRegister));
    put(kFieldTableMapping, 1.0F, 0.0F, -1.0F, 0.0F);
    put(kFieldTableSampling, 1.0F, 1.0F, static_cast<float>(mode), static_cast<float>(kind));
    put(kFieldTableSamples, 0.5F, 0.5F, 0.5F, 0.5F);
    for (std::size_t i = 0; i < code_.size(); ++i) {
        const auto& instruction = code_[i];
        const auto base = parameterBase + instruction.parameterOffset;
        put(kFieldTableCode + 2U * i, static_cast<float>(instruction.op),
            static_cast<float>(instruction.output), static_cast<float>(instruction.inputs[0]),
            static_cast<float>(instruction.inputs[1]));
        put(kFieldTableCode + 2U * i + 1U, static_cast<float>(instruction.inputs[2]),
            static_cast<float>(instruction.inputs[3]),
            instruction.op == FieldOp::Coordinate ? 0.0F : static_cast<float>(base), 0.0F);
    }
    std::copy(parameters_.begin(), parameters_.end(),
              table.begin() + static_cast<std::ptrdiff_t>(parameterBase * 4U));
    return table;
}

// --- Raster geometry ----------------------------------------------------------------------------

FieldRasterGeometry fieldRasterGeometry(const std::int64_t originX, const std::int64_t originY,
                                        const std::int64_t width, const std::int64_t height,
                                        const double compositionWidth,
                                        const double compositionHeight, const double hScale,
                                        const double vScale, const std::uint32_t samples) {
    FieldRasterGeometry geometry;
    const auto h = hScale > 0.0 ? hScale : 1.0;
    const auto v = vScale > 0.0 ? vScale : 1.0;
    // Evaluation-canvas pixel X covers composition [X / h, (X + 1) / h); the field's x is the
    // composition position less half the width, its y half the height less the position.
    geometry.ax = 1.0 / h;
    geometry.bx = static_cast<double>(originX) / h - compositionWidth * 0.5;
    geometry.ay = -1.0 / v;
    geometry.by = compositionHeight * 0.5 - static_cast<double>(originY) / v;
    geometry.targetWidth = std::max<std::int64_t>(width, 0);
    geometry.targetHeight = std::max<std::int64_t>(height, 0);
    geometry.samples = samples >= 16U ? 16U : (samples >= 4U ? 4U : 1U);
    return geometry;
}

FieldImageSlot fieldImageSlot(const std::int64_t originX, const std::int64_t originY,
                              const std::int64_t width, const std::int64_t height,
                              const double compositionWidth, const double compositionHeight,
                              const double hScale, const double vScale) {
    FieldImageSlot slot;
    const auto h = hScale > 0.0 ? hScale : 1.0;
    const auto v = vScale > 0.0 ? vScale : 1.0;
    slot.kx = h;
    slot.cx = compositionWidth * 0.5 * h - static_cast<double>(originX);
    slot.ky = -v;
    slot.cy = compositionHeight * 0.5 * v - static_cast<double>(originY);
    slot.width = std::max<std::int64_t>(width, 0);
    slot.height = std::max<std::int64_t>(height, 0);
    return slot;
}

namespace {

// Solves every window Homography (field_program.hpp, HomographyParameters::windowSlot) against the
// window its image slot now has, in binary64, and writes the matrix into the block.
void placeHomographyWindows(std::vector<float>& table, const FieldRasterGeometry& geometry) {
    const auto count = static_cast<std::size_t>(std::max(table[2], 0.0F));
    for (std::size_t i = 0; i < count && (kFieldTableCode + 2U * i + 2U) * 4U <= table.size();
         ++i) {
        const auto* a = &table[(kFieldTableCode + 2U * i) * 4U];
        if (a[0] != static_cast<float>(FieldOp::Homography))
            continue;
        const auto base = static_cast<std::size_t>(std::max(a[6], 0.0F));
        if ((base + 6U) * 4U > table.size())
            continue;
        float* block = &table[base * 4U];
        if (block[0] != 1.0F)
            continue;
        const auto slotIndex = static_cast<std::size_t>(std::max(block[1], 0.0F));
        HomographyParameters invalid;
        invalid.valid = false;
        const auto write = [&](const HomographyParameters& map) {
            const double m[9] = {map.m[0], map.m[1], map.m[2], map.m[3], map.m[4],
                                 map.m[5], map.m[6], map.m[7], map.m[8]};
            bool ok = map.valid;
            for (std::size_t k = 0; k < 8; ++k)
                ok = ok && std::fabs(m[k]) < 1.0e15;
            for (std::size_t k = 0; k < 8; ++k)
                block[12U + k] = ok ? narrow(m[k]) : 0.0F;
            block[20] = ok ? narrow(map.centreX) : 0.0F;
            block[21] = ok ? narrow(map.centreY) : 0.0F;
            block[22] = ok ? 1.0F : 0.0F;
            block[23] = 0.0F;
        };
        if (slotIndex >= kMaxFieldImages) {
            write(invalid);
            continue;
        }
        const auto& slot = geometry.slots[slotIndex];
        if (slot.width <= 0 || slot.height <= 0 || !finite(slot.kx) || !finite(slot.ky) ||
            slot.kx == 0.0 || slot.ky == 0.0) {
            write(invalid);
            continue;
        }
        // The window's corners in field positions: texel (0, 0) is its top left, ky < 0.
        const double left = (0.0 - slot.cx) / slot.kx;
        const double right = (static_cast<double>(slot.width) - slot.cx) / slot.kx;
        const double top = (0.0 - slot.cy) / slot.ky;
        const double bottom = (static_cast<double>(slot.height) - slot.cy) / slot.ky;
        if (block[2] == 1.0F) {
            // The translation by the window's centre: q = p + centre.
            HomographyParameters move;
            move.m = {1.0, 0.0, 0.5 * (left + right), 0.0, 1.0, 0.5 * (top + bottom),
                      0.0, 0.0, 1.0};
            write(move);
            continue;
        }
        const std::array<double, 8> from{left, top, right, top, right, bottom, left, bottom};
        std::array<double, 8> to = from;
        for (std::size_t k = 0; k < 8; ++k)
            to[k] += static_cast<double>(block[4U + k]);
        HomographyParameters map;
        static_cast<void>(homographyFromQuads(from, to, map));
        write(map);
    }
}

} // namespace

bool applyFieldRasterGeometry(std::vector<float>& table, const FieldRasterGeometry& geometry) {
    if (table.size() < kFieldTableCode * 4U)
        return false;
    const auto limit = 16777216.0; // 2^24: every integer below it is exact in binary32
    const auto exact = [limit](const std::int64_t value) {
        return value >= 0 && static_cast<double>(value) < limit;
    };
    if (!exact(geometry.targetX) || !exact(geometry.targetY) || !exact(geometry.targetWidth) ||
        !exact(geometry.targetHeight) || !finite(geometry.ax) || !finite(geometry.bx) ||
        !finite(geometry.ay) || !finite(geometry.by))
        return false;
    const auto put = [&table](const std::size_t vec4, const double a, const double b,
                              const double c, const double d) {
        table[vec4 * 4U] = narrow(a);
        table[vec4 * 4U + 1U] = narrow(b);
        table[vec4 * 4U + 2U] = narrow(c);
        table[vec4 * 4U + 3U] = narrow(d);
    };
    put(kFieldTableMapping, geometry.ax, geometry.bx, geometry.ay, geometry.by);
    put(kFieldTableTarget, static_cast<double>(geometry.targetX),
        static_cast<double>(geometry.targetY), static_cast<double>(geometry.targetWidth),
        static_cast<double>(geometry.targetHeight));
    const auto samples = geometry.samples >= 16U ? 16U : (geometry.samples >= 4U ? 4U : 1U);
    const auto side = samples == 16U ? 4U : (samples == 4U ? 2U : 1U);
    const auto pixel = std::max(std::fabs(geometry.ax), std::fabs(geometry.ay));
    table[kFieldTableSampling * 4U] = static_cast<float>(samples);
    table[kFieldTableSampling * 4U + 1U] = narrow(pixel / static_cast<double>(side));
    for (std::size_t s = 0; s < kMaxFieldSamples; ++s) {
        const auto index = s < samples ? s : 0U;
        const auto ox = (static_cast<double>(index % side) + 0.5) / static_cast<double>(side);
        const auto oy = (static_cast<double>(index / side) + 0.5) / static_cast<double>(side);
        table[kFieldTableSamples * 4U + 2U * s] = narrow(ox);
        table[kFieldTableSamples * 4U + 2U * s + 1U] = narrow(oy);
    }
    for (std::size_t i = 0; i < kMaxFieldImages; ++i) {
        const auto& slot = geometry.slots[i];
        if (!finite(slot.kx) || !finite(slot.cx) || !finite(slot.ky) || !finite(slot.cy) ||
            !exact(slot.originX) || !exact(slot.originY) || !exact(slot.width) ||
            !exact(slot.height))
            return false;
        put(kFieldTableSlots + 2U * i, slot.kx, slot.cx, slot.ky, slot.cy);
        put(kFieldTableSlots + 2U * i + 1U, static_cast<double>(slot.originX),
            static_cast<double>(slot.originY), static_cast<double>(slot.width),
            static_cast<double>(slot.height));
    }
    placeHomographyWindows(table, geometry);
    return allFinite(table);
}

void setFieldTexelScale(std::vector<float>& table, const double unitsPerTexel) noexcept {
    if (table.size() < kFieldTableCode * 4U || !finite(unitsPerTexel))
        return;
    const auto count = static_cast<std::size_t>(std::max(table[2], 0.0F));
    for (std::size_t i = 0; i < count && (kFieldTableCode + 2U * i + 2U) * 4U <= table.size();
         ++i) {
        const auto* a = &table[(kFieldTableCode + 2U * i) * 4U];
        if (a[0] != static_cast<float>(FieldOp::Shape))
            continue;
        const auto base = static_cast<std::size_t>(std::max(a[6], 0.0F));
        const auto index = (base + 4U) * 4U + 1U;
        if (index < table.size())
            table[index] = narrow(unitsPerTexel);
    }
}

// --- Validation ---------------------------------------------------------------------------------

namespace {

// The vec4 count an operation's block occupies at `base`, or 0 when its header is malformed.
std::size_t blockSize(const std::span<const float> table, const FieldOp op,
                      const std::size_t base) noexcept {
    const auto at = [&table, base](const std::size_t vec4, const std::size_t lane) {
        const auto index = (base + vec4) * 4U + lane;
        return index < table.size() ? table[index] : -1.0F;
    };
    const auto curveSize = [&](const float offset, const std::size_t fixed) -> std::size_t {
        if (offset < 0.0F)
            return fixed;
        if (offset != static_cast<float>(fixed))
            return 0;
        const auto n = at(fixed, 0);
        if (!(n >= 1.0F) || n > static_cast<float>(kMaxFieldCurveKnots) || n != std::floor(n))
            return 0;
        return fixed + curveVec4s(static_cast<int>(n));
    };
    switch (op) {
    case FieldOp::Coordinate:
        return 0;
    case FieldOp::Constant:
    case FieldOp::Combine:
    case FieldOp::Offset:
    case FieldOp::Derivative:
    case FieldOp::CellCentre:
    case FieldOp::Arrow:
    case FieldOp::Streak:
    case FieldOp::Math:
        return 1;
    case FieldOp::Noise: {
        const auto octaves = at(0, 2);
        if (!(octaves >= 1.0F) || octaves > static_cast<float>(kMaxNoiseOctaves))
            return 0;
        return 4U + 5U * static_cast<std::size_t>(octaves);
    }
    case FieldOp::Voronoi:
    case FieldOp::Random:
    case FieldOp::Pattern:
    case FieldOp::Gradient:
        return 3;
    case FieldOp::Shape:
        return at(4, 0) >= 0.0F && at(4, 0) < static_cast<float>(kMaxFieldImages) ? 5U : 0U;
    case FieldOp::Image:
        return at(0, 0) >= 0.0F && at(0, 0) < static_cast<float>(kMaxFieldImages) ? 2U : 0U;
    case FieldOp::Force: {
        const auto falloff = at(0, 1);
        const auto offset = at(1, 3);
        return falloff == 3.0F ? curveSize(offset, 3) : 3U;
    }
    case FieldOp::Remap:
        return curveSize(at(2, 2), 3);
    case FieldOp::Transform:
        return 4;
    case FieldOp::Swizzle:
    case FieldOp::LensWarp:
        return 2;
    case FieldOp::TestCard:
        return 6;
    case FieldOp::Rays:
        return at(0, 0) >= 0.0F && at(0, 0) < static_cast<float>(kMaxFieldImages) ? 2U : 0U;
    case FieldOp::Homography:
        return 6;
    case FieldOp::ProceduralWarp:
        return at(0, 0) >= 0.0F && at(0, 0) < static_cast<float>(kProceduralWarpModeCount) ? 4U : 0U;
    case FieldOp::Ramp:
        return 1U + kFieldRampEntries;
    }
    return 0;
}

} // namespace

bool validFieldTable(const std::span<const float> table) noexcept {
    if (table.size() % 4U != 0U || table.size() < kFieldTableCode * 4U ||
        table.size() > kMaxFieldTableFloats)
        return false;
    for (const auto v : table) {
        if (!std::isfinite(v) || std::fabs(v) > 1.0e30F)
            return false;
    }
    if (table[0] != kFieldTableMagic ||
        table[1] != static_cast<float>(kFieldProgramSemanticsVersion))
        return false;
    const auto count = table[2];
    const auto result = table[3];
    if (!(count >= 1.0F) || count > static_cast<float>(kMaxFieldInstructions) ||
        count != std::floor(count) || !(result >= 0.0F) ||
        result >= static_cast<float>(kFieldRegisterCount))
        return false;
    const auto instructions = static_cast<std::size_t>(count);
    const auto vec4s = table.size() / 4U;
    const auto parameterBase = kFieldTableCode + 2U * instructions;
    if (parameterBase > vec4s)
        return false;
    const auto samples = table[kFieldTableSampling * 4U];
    if (samples != 1.0F && samples != 4.0F && samples != 16.0F)
        return false;
    for (std::size_t i = 0; i < instructions; ++i) {
        const auto* a = &table[(kFieldTableCode + 2U * i) * 4U];
        const auto* b = a + 4;
        const auto op = a[0];
        if (!(op >= 0.0F) || op > static_cast<float>(FieldOp::ProceduralWarp) || op != std::floor(op))
            return false;
        if (!(a[1] >= 0.0F) || a[1] >= static_cast<float>(kFieldRegisterCount))
            return false;
        for (const auto input : {a[2], a[3], b[0], b[1]}) {
            if (input < -1.0F || input >= static_cast<float>(kFieldRegisterCount) ||
                input != std::floor(input))
                return false;
        }
        const auto fieldOp = static_cast<FieldOp>(static_cast<int>(op));
        if (fieldOp == FieldOp::Coordinate)
            continue;
        const auto base = b[2];
        if (base < static_cast<float>(parameterBase) || base != std::floor(base))
            return false;
        const auto start = static_cast<std::size_t>(base);
        const auto size = blockSize(table, fieldOp, start);
        if (size == 0U || start + size > vec4s)
            return false;
    }
    return true;
}

// --- Evaluation ---------------------------------------------------------------------------------

std::array<float, 4> evaluateField(const std::span<const float> table, const FieldImages& images,
                                   const float x, const float y) noexcept {
    const kernel::Context context{table, &images};
    const auto v = kernel::bfk_run(context, x, y);
    return {v.x, v.y, v.z, v.w};
}

std::array<float, 4> fieldPixel(const std::span<const float> table, const FieldImages& images,
                                const int x, const int y) noexcept {
    const kernel::Context context{table, &images};
    const auto v = kernel::bfk_dispatch_pixel(context, x, y);
    return {v.x, v.y, v.z, v.w};
}

// --- Mask distance --------------------------------------------------------------------------------

void squaredDistance1d(const std::span<const float> f, const std::span<float> d,
                       const std::span<int> vertices, const std::span<double> bounds,
                       const float cap) noexcept {
    const auto n = f.size();
    if (n == 0U || d.size() < n || vertices.size() < n || bounds.size() < n + 1U)
        return;
    const auto inf = std::numeric_limits<double>::infinity();
    // Felzenszwalb and Huttenlocher's lower envelope of the parabolas (q - p)^2 + f(p), its
    // intersections in binary64: exact comparisons for the integer squared distances a mask passes
    // in, so the result is the exact minimum (which the GPU's bounded search also finds).
    const auto intersection = [&f](const int q, const int p) {
        const auto qd = static_cast<double>(q);
        const auto pd = static_cast<double>(p);
        const auto fq = static_cast<double>(f[static_cast<std::size_t>(q)]);
        const auto fp = static_cast<double>(f[static_cast<std::size_t>(p)]);
        return ((fq + qd * qd) - (fp + pd * pd)) / (2.0 * qd - 2.0 * pd);
    };
    std::size_t k = 0;
    vertices[0] = 0;
    bounds[0] = -inf;
    bounds[1] = inf;
    for (std::size_t q = 1; q < n; ++q) {
        const auto qi = static_cast<int>(q);
        auto s = intersection(qi, vertices[k]);
        while (s <= bounds[k]) {
            --k;
            s = intersection(qi, vertices[k]);
        }
        ++k;
        vertices[k] = qi;
        bounds[k] = s;
        bounds[k + 1U] = inf;
    }
    k = 0;
    for (std::size_t q = 0; q < n; ++q) {
        const auto qd = static_cast<double>(q);
        while (bounds[k + 1U] < qd)
            ++k;
        const auto p = vertices[k];
        const auto dq = qd - static_cast<double>(p);
        const auto value = dq * dq + static_cast<double>(f[static_cast<std::size_t>(p)]);
        d[q] = static_cast<float>(std::min(value, static_cast<double>(cap)));
    }
}

} // namespace bloom::core::field
