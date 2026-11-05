#pragma once

// #546: how a gradient blends between its stops -- its interpolation, its colour mode and, for HSV
// and HSL, its hue direction -- and the one definition of the colour they give. Blender's Color
// Ramp (BKE_colorband_evaluate, key_curve_position_weights, colorband_hue_interp and its
// rgb_to_hsv / rgb_to_hsl / hsv_to_rgb / hsl_to_rgb) is the reference for Ease, Cardinal, B-Spline,
// the HSV and HSL blends and the four hue directions; Exponential is the exponential easing of
// Blender's F-Curves with its automatic (ease in) direction, the one Bloom's Exponential keyframe
// easing applies, written out so CPU and GPU evaluate the same operations.
//
// The evaluation is a template over the scalar type: the document evaluates in binary64 (the
// editor's bar, Glow's octave tints), the CPU reference in binary32 with the operations the native
// kernels perform in the same order, so the two agree within the process parity gate
// (docs/architecture/evaluation-primitives.md, "Gradient Interpolation And Colour Modes").

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace bloom::document {

// Stored as names; the numbers are what a stop table's mode words carry (0 is each default).
enum class GradientInterpolation : std::uint8_t {
    Linear = 0,
    Constant = 1,
    Ease = 2,
    Cardinal = 3,
    BSpline = 4,
    Exponential = 5,
};
enum class GradientColorMode : std::uint8_t { Rgb = 0, Hsv = 1, Hsl = 2 };
enum class GradientHueDirection : std::uint8_t {
    Near = 0,
    Far = 1,
    Clockwise = 2,
    CounterClockwise = 3,
};
inline constexpr std::array kGradientInterpolations{
    GradientInterpolation::Constant, GradientInterpolation::Linear,
    GradientInterpolation::Ease,     GradientInterpolation::Cardinal,
    GradientInterpolation::BSpline,  GradientInterpolation::Exponential};
inline constexpr std::array kGradientColorModes{GradientColorMode::Rgb, GradientColorMode::Hsv,
                                                GradientColorMode::Hsl};
inline constexpr std::array kGradientHueDirections{
    GradientHueDirection::Near, GradientHueDirection::Far, GradientHueDirection::Clockwise,
    GradientHueDirection::CounterClockwise};

// The stored names: "constant", "linear", "ease", "cardinal", "b-spline", "exponential"; "rgb",
// "hsv", "hsl"; "near", "far", "clockwise", "counter-clockwise".
[[nodiscard]] std::string_view gradientInterpolationName(GradientInterpolation value) noexcept;
[[nodiscard]] std::optional<GradientInterpolation>
gradientInterpolationFromName(std::string_view name) noexcept;
[[nodiscard]] std::string_view gradientColorModeName(GradientColorMode value) noexcept;
[[nodiscard]] std::optional<GradientColorMode>
gradientColorModeFromName(std::string_view name) noexcept;
[[nodiscard]] std::string_view gradientHueDirectionName(GradientHueDirection value) noexcept;
[[nodiscard]] std::optional<GradientHueDirection>
gradientHueDirectionFromName(std::string_view name) noexcept;
// The English labels the editor shows ("B-Spline", "Counter-clockwise", "HSV").
[[nodiscard]] std::string_view gradientInterpolationLabel(GradientInterpolation value) noexcept;
[[nodiscard]] std::string_view gradientColorModeLabel(GradientColorMode value) noexcept;
[[nodiscard]] std::string_view gradientHueDirectionLabel(GradientHueDirection value) noexcept;

[[nodiscard]] constexpr bool isGradientInterpolation(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(GradientInterpolation::Exponential);
}
[[nodiscard]] constexpr bool isGradientColorMode(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(GradientColorMode::Hsl);
}
[[nodiscard]] constexpr bool isGradientHueDirection(const std::uint8_t value) noexcept {
    return value <= static_cast<std::uint8_t>(GradientHueDirection::CounterClockwise);
}

// The three modes of a gradient. HSV and HSL blend linearly between neighbouring stops, as the
// reference does: the interpolation shapes an RGB blend only, and the hue direction an HSV or HSL
// one only, but each keeps its value while the other is in use.
struct GradientModes final {
    GradientInterpolation interpolation = GradientInterpolation::Linear;
    GradientColorMode colorMode = GradientColorMode::Rgb;
    GradientHueDirection hueDirection = GradientHueDirection::Near;
    // Linear in RGB: the blend gradients had before the modes existed.
    [[nodiscard]] constexpr bool isLinearRgb() const noexcept {
        return colorMode == GradientColorMode::Rgb &&
               interpolation == GradientInterpolation::Linear;
    }
    friend bool operator==(const GradientModes&, const GradientModes&) = default;
};

// The modes as a blend reads them: an RGB blend has no hue direction and an HSV or HSL one blends
// linearly, so a mode the blend ignores never changes a stop table or a cache identity.
[[nodiscard]] constexpr GradientModes effectiveGradientModes(GradientModes modes) noexcept {
    if (modes.colorMode == GradientColorMode::Rgb)
        modes.hueDirection = GradientHueDirection::Near;
    else
        modes.interpolation = GradientInterpolation::Linear;
    return modes;
}
// Whether the blend reaches past the end stops (Cardinal or B-Spline in RGB).
[[nodiscard]] constexpr bool gradientModesReachPastEnds(const GradientModes modes) noexcept {
    return modes.colorMode == GradientColorMode::Rgb &&
           (modes.interpolation == GradientInterpolation::Cardinal ||
            modes.interpolation == GradientInterpolation::BSpline);
}

namespace gradient_math {

// One stop of a sorted table: its position, the reciprocal of the span to the next stop (0 for the
// last stop and for a hard edge) and its straight RGBA.
template <typename T> struct Stop final {
    T position{};
    T inverseSpan{};
    std::array<T, 4> color{};
};

// 2^x for x in [-10, 0]: 2^floor(x) times a degree-9 Horner polynomial of 2^f, f in [0, 1), its
// coefficients (ln 2)^k / k! (rounded to binary32), one fused multiply-add per term.
template <typename T> [[nodiscard]] T exp2Unit(const T x) noexcept {
    const T whole = std::floor(x);
    const T f = x - whole;
    T p = T(1.01780863e-07);
    p = std::fma(p, f, T(1.32154867e-06));
    p = std::fma(p, f, T(1.52527336e-05));
    p = std::fma(p, f, T(0.000154035297));
    p = std::fma(p, f, T(0.00133335579));
    p = std::fma(p, f, T(0.00961812865));
    p = std::fma(p, f, T(0.0555041097));
    p = std::fma(p, f, T(0.240226507));
    p = std::fma(p, f, T(0.693147182));
    p = std::fma(p, f, T(1.0));
    return std::ldexp(p, static_cast<int>(whole));
}

// The factor an RGB blend uses at `u` (0 at a segment's first stop, 1 at its next): Ease is
// 3u^2 - 2u^3; Exponential is (2^(10(u - 1)) - 2^-10) / (1 - 2^-10), exactly 0 and 1 at the stops.
template <typename T>
[[nodiscard]] T shapeFactor(const GradientInterpolation interpolation, const T u) noexcept {
    const auto unit = [](const T value) { return std::fmin(std::fmax(value, T(0.0)), T(1.0)); };
    if (interpolation == GradientInterpolation::Ease) {
        const T u2 = u * u;
        const T three = T(3.0) * u2;
        const T two = (T(2.0) * u2) * u;
        return unit(three - two);
    }
    if (interpolation == GradientInterpolation::Exponential) {
        if (u == T(0.0))
            return T(0.0);
        const T scaled = T(10.0) * u;
        const T x = scaled - T(10.0);
        const T shifted = exp2Unit(x) - T(0.0009765625);
        return unit(shifted * T(1.0 / (1.0 - 0.0009765625)));
    }
    return u;
}

// a * wa + b * wb as two rounded products and one sum, never contracted.
template <typename T>
[[nodiscard]] T weighted(const T wa, const T a, const T wb, const T b) noexcept {
    const T first = wa * a;
    const T second = wb * b;
    return first + second;
}

// The reference's rgb_to_hsv, with a zero divisor giving no saturation.
template <typename T> [[nodiscard]] std::array<T, 3> rgbToHsv(T r, T g, T b) noexcept {
    T k = T(0.0);
    if (g < b) {
        const T swap = g;
        g = b;
        b = swap;
        k = T(-1.0);
    }
    T minimum = b;
    if (r < g) {
        const T swap = r;
        r = g;
        g = swap;
        k = T(-0.333333343) - k;
        minimum = std::fmin(g, b);
    }
    const T chroma = r - minimum;
    const T scaled = T(6.0) * chroma;
    const T hue = std::fabs(k + (g - b) / (scaled + T(1e-20)));
    const T divisor = r + T(1e-20);
    const T saturation = divisor != T(0.0) ? chroma / divisor : T(0.0);
    return {hue, saturation, r};
}

// The reference's rgb_to_hsl, with a zero divisor giving no saturation.
template <typename T>
[[nodiscard]] std::array<T, 3> rgbToHsl(const T r, const T g, const T b) noexcept {
    const T high = std::fmax(std::fmax(r, g), b);
    const T low = std::fmin(std::fmin(r, g), b);
    const T lightness = std::fmin(T(1.0), (high + low) * T(0.5));
    if (high == low)
        return {T(0.0), T(0.0), lightness};
    const T d = high - low;
    const T divisor = lightness > T(0.5) ? (T(2.0) - high) - low : high + low;
    const T saturation = divisor != T(0.0) ? d / divisor : T(0.0);
    T hue{};
    if (high == r)
        hue = (g - b) / d + (g < b ? T(6.0) : T(0.0));
    else if (high == g)
        hue = (b - r) / d + T(2.0);
    else
        hue = (r - g) / d + T(4.0);
    return {hue / T(6.0), saturation, lightness};
}

// The hue's red, green and blue weights, each clamped to [0, 1].
template <typename T> [[nodiscard]] std::array<T, 3> hueWeights(const T hue) noexcept {
    const T six = hue * T(6.0);
    const auto clamp = [](const T value) { return std::fmin(std::fmax(value, T(0.0)), T(1.0)); };
    return {clamp(std::fabs(six - T(3.0)) - T(1.0)), clamp(T(2.0) - std::fabs(six - T(2.0))),
            clamp(T(2.0) - std::fabs(six - T(4.0)))};
}

template <typename T>
[[nodiscard]] std::array<T, 3> hsvToRgb(const T hue, const T saturation, const T value) noexcept {
    const auto n = hueWeights(hue);
    std::array<T, 3> rgb{};
    for (std::size_t i = 0; i < 3; ++i) {
        const T scaled = (n[i] - T(1.0)) * saturation;
        rgb[i] = (scaled + T(1.0)) * value;
    }
    return rgb;
}

template <typename T>
[[nodiscard]] std::array<T, 3> hslToRgb(const T hue, const T saturation,
                                        const T lightness) noexcept {
    const auto n = hueWeights(hue);
    const T twice = T(2.0) * lightness;
    const T chroma = (T(1.0) - std::fabs(twice - T(1.0))) * saturation;
    std::array<T, 3> rgb{};
    for (std::size_t i = 0; i < 3; ++i) {
        const T scaled = (n[i] - T(0.5)) * chroma;
        rgb[i] = scaled + lightness;
    }
    return rgb;
}

// The reference's colorband_hue_interp: `h1` is the hue of the segment's later stop and `h2` of
// its earlier one, `fac` 1 at the earlier stop and `mfac` = 1 - fac. Clockwise turns the hue up
// (red towards yellow) from a stop to the next, Counter-clockwise down; Near takes the shorter way
// round and Far the longer one (a full turn between two equal hues).
template <typename T>
[[nodiscard]] T interpolateHue(const GradientHueDirection direction, const T mfac, const T fac,
                               T h1, T h2) noexcept {
    const auto wrap = [](const T h) { return h < T(1.0) ? h : h - T(1.0); };
    h1 = wrap(h1);
    h2 = wrap(h2);
    int mode = 0;
    switch (direction) {
    case GradientHueDirection::Near:
        if (h1 < h2 && h2 - h1 > T(0.5))
            mode = 1;
        else if (h1 > h2 && h2 - h1 < T(-0.5))
            mode = 2;
        break;
    case GradientHueDirection::Far:
        if (h1 == h2)
            mode = 1;
        else if (h1 < h2 && h2 - h1 < T(0.5))
            mode = 1;
        else if (h1 > h2 && h2 - h1 > T(-0.5))
            mode = 2;
        break;
    case GradientHueDirection::CounterClockwise:
        mode = h1 > h2 ? 2 : 0;
        break;
    case GradientHueDirection::Clockwise:
        mode = h1 < h2 ? 1 : 0;
        break;
    }
    if (mode == 1)
        return wrap(weighted(mfac, h1 + T(1.0), fac, h2));
    if (mode == 2)
        return wrap(weighted(mfac, h1, fac, h2 + T(1.0)));
    return weighted(mfac, h1, fac, h2);
}

// The four weights of the reference's key_curve_position_weights at `f` in [0, 1], for the stops
// after the segment, its later stop, its earlier stop and the stop before it. Its constants (0.71,
// 0.16666666 and 0.66666666) and the coefficients made from them are written as the binary32
// values the reference computes.
template <typename T>
[[nodiscard]] std::array<T, 4> splineWeights(const GradientInterpolation interpolation,
                                             const T f) noexcept {
    const T f2 = f * f;
    const T f3 = f2 * f;
    if (interpolation == GradientInterpolation::Cardinal) {
        const T c = T(0.709999979);
        const T a0 = T(-0.709999979) * f3;
        const T b0 = T(1.41999996) * f2;
        const T c0 = c * f;
        const T a1 = T(1.28999996) * f3;
        const T b1 = T(-2.28999996) * f2;
        const T a2 = T(-1.28999996) * f3;
        const T b2 = T(1.58000004) * f2;
        const T c2 = c * f;
        const T a3 = c * f3;
        const T b3 = c * f2;
        return {(a0 + b0) - c0, (a1 + b1) + T(1.0), (a2 + b2) + c2, a3 - b3};
    }
    const T sixth = T(0.166666657);
    const T a0 = T(-0.166666657) * f3;
    const T b0 = T(0.5) * f2;
    const T c0 = T(0.5) * f;
    const T a1 = T(0.5) * f3;
    const T a2 = T(-0.5) * f3;
    const T b2 = T(0.5) * f2;
    const T c2 = T(0.5) * f;
    const T a3 = sixth * f3;
    return {((a0 + b0) - c0) + sixth, (a1 - f2) + T(0.666666687), ((a2 + b2) + c2) + sixth, a3};
}

// The straight RGBA of a gradient at `t` in [0, 1]. `stop(i)` gives sorted stop `i` of `count`
// (at least one); `leadIn` is 1 / the first position (0 when it is 0) and `leadOut` 1 / (1 - the
// last position) (0 when it is 1), the spans the reference's spline modes blend over before the
// first stop and after the last.
//
// - Constant, Linear, Ease and Exponential (and every HSV or HSL blend) hold the first colour up
//   to the first stop and the last from the last stop; between them they blend the last stop at or
//   before `t` with the next one by u = (t - position) * inverse span, Constant keeping the
//   earlier colour and Ease and Exponential shaping u (shapeFactor).
// - A blended alpha is clamped to [0, 1] (rounding may step past it by an ulp).
// - Cardinal and B-Spline weigh four stops around the segment (splineWeights), the first and last
//   stops repeated past the ends and extended to 0 and 1. The reference clamps all four channels
//   to [0, 1]; Bloom's stops are scene-linear and may be HDR, so only alpha is clamped.
// - HSV and HSL blend hue (interpolateHue), saturation, value or lightness and alpha linearly.
template <typename T, typename StopAt>
[[nodiscard]] std::array<T, 4> sample(const std::size_t count, StopAt&& stop, const T leadIn,
                                      const T leadOut, const T t,
                                      const GradientModes modes) noexcept {
    const Stop<T> first = stop(0);
    if (count <= 1)
        return first.color;
    const Stop<T> last = stop(count - 1);
    const bool rgb = modes.colorMode == GradientColorMode::Rgb;
    const bool spline = rgb && (modes.interpolation == GradientInterpolation::Cardinal ||
                                modes.interpolation == GradientInterpolation::BSpline);
    if (!spline) {
        if (!(t > first.position))
            return first.color;
        if (t >= last.position)
            return last.color;
        std::size_t index = 0;
        while (index + 2 < count && stop(index + 1).position <= t)
            ++index;
        const Stop<T> from = stop(index);
        const Stop<T> to = stop(index + 1);
        if (rgb) {
            if (modes.interpolation == GradientInterpolation::Constant)
                return from.color;
            const T u = shapeFactor(modes.interpolation, (t - from.position) * from.inverseSpan);
            const T remaining = T(1.0) - u;
            std::array<T, 4> out{};
            for (std::size_t i = 0; i < 4; ++i) {
                const T a = from.color[i] * remaining;
                const T b = to.color[i] * u;
                out[i] = a + b;
            }
            out[3] = std::fmin(std::fmax(out[3], T(0.0)), T(1.0));
            return out;
        }
        // The reference blends from the later stop: fac is 1 at the earlier one.
        const T fac = (to.position - t) * from.inverseSpan;
        const T mfac = T(1.0) - fac;
        const bool hsv = modes.colorMode == GradientColorMode::Hsv;
        const auto later = hsv ? rgbToHsv(to.color[0], to.color[1], to.color[2])
                               : rgbToHsl(to.color[0], to.color[1], to.color[2]);
        const auto earlier = hsv ? rgbToHsv(from.color[0], from.color[1], from.color[2])
                                 : rgbToHsl(from.color[0], from.color[1], from.color[2]);
        const T hue = interpolateHue(modes.hueDirection, mfac, fac, later[0], earlier[0]);
        const T saturation = weighted(mfac, later[1], fac, earlier[1]);
        const T third = weighted(mfac, later[2], fac, earlier[2]);
        const T alpha =
            std::fmin(std::fmax(weighted(mfac, to.color[3], fac, from.color[3]), T(0.0)), T(1.0));
        const auto rgbOut =
            hsv ? hsvToRgb(hue, saturation, third) : hslToRgb(hue, saturation, third);
        return {rgbOut[0], rgbOut[1], rgbOut[2], alpha};
    }
    // The first stop after t (count when there is none).
    std::size_t after = 0;
    while (after < count && !(stop(after).position > t))
        ++after;
    Stop<T> earlier{};
    Stop<T> later{};
    T fac{};
    if (after == count) {
        earlier = last;
        later = last;
        later.position = T(1.0);
        fac = leadOut == T(0.0) ? T(1.0) : (T(1.0) - t) * leadOut;
    } else if (after == 0) {
        later = first;
        earlier = first;
        earlier.position = T(0.0);
        fac = (first.position - t) * leadIn;
    } else {
        earlier = stop(after - 1);
        later = stop(after);
        fac = (later.position - t) * earlier.inverseSpan;
    }
    const Stop<T> next = after + 1 >= count ? later : stop(after + 1);
    const Stop<T> previous = after < 2 ? earlier : stop(after - 2);
    fac = std::fmin(std::fmax(fac, T(0.0)), T(1.0));
    const auto w = splineWeights(modes.interpolation, fac);
    std::array<T, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) {
        const T a = w[3] * previous.color[i];
        const T b = w[2] * earlier.color[i];
        const T c = w[1] * later.color[i];
        const T d = w[0] * next.color[i];
        out[i] = ((a + b) + c) + d;
    }
    out[3] = std::fmin(std::fmax(out[3], T(0.0)), T(1.0));
    return out;
}

} // namespace gradient_math
} // namespace bloom::document
