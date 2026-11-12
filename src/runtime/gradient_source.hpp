#pragma once

#include <bloom/core/color.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/compiled_curves.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace bloom::runtime::detail {

// One stop of a resolved gradient: its Float32 position, the Float32 reciprocal of the span to
// the next stop (0 for the last stop and a hard edge) and its straight colour. The CPU reference and
// native GradientV1 read the same values (#541).
struct ResolvedGradientStop final {
    float position = 0.0F;
    float inverseSpan = 0.0F;
    core::Color4d color;
};

// #546: the Float32 reciprocals of the spans before the first stop (from 0) and after the last
// (to 1), 0 for an empty span, which the spline modes blend over.
struct GradientLeads final {
    float in = 0.0F;
    float out = 0.0F;
    friend bool operator==(const GradientLeads&, const GradientLeads&) = default;
};

struct ResolvedGradient final {
    std::vector<ResolvedGradientStop> stops;
    document::Vec2d start;
    document::Vec2d end;
    std::int64_t repeat = 0;
    bool radial = false;
    double aspect = 1.0;
    // #546: how the colour blends between the stops.
    document::GradientModes modes{};
    GradientLeads leads{};
};

// #541: a gradient source (a constant or a whole-value curve) at `time`, or nullopt when its curve
// cannot be sampled or the value is not a valid gradient.
[[nodiscard]] inline std::optional<document::GradientValue>
sampleGradientSource(const std::variant<document::GradientValue, CompiledGradientCurve>& source,
                     const core::RationalTime time) {
    std::optional<document::GradientValue> sampled;
    if (const auto* constant = std::get_if<document::GradientValue>(&source))
        sampled = *constant;
    else
        sampled = sampleGradientCurve(std::get<CompiledGradientCurve>(source), time);
    if (!sampled || !sampled->isValid())
        return std::nullopt;
    return sampled;
}

// #543: `sampled` with the stops its value nodes drive this frame. `value` gives the driver's
// value output (a CompiledValue pointer, null when unavailable); a colour driver reads a Color and
// a position driver a Scalar, and any other value makes the gradient not evaluable.
template <typename Driver, typename Lookup>
[[nodiscard]] std::optional<document::GradientValue>
applyGradientStopDrivers(std::optional<document::GradientValue> sampled,
                         const std::vector<Driver>& drivers, Lookup&& value) {
    if (!sampled || drivers.empty())
        return sampled;
    std::vector<document::GradientStopValue> values;
    values.reserve(drivers.size());
    for (const auto& driver : drivers) {
        const CompiledValue* held = value(driver);
        if (held == nullptr)
            return std::nullopt;
        if (driver.component == document::GradientStopComponent::Color) {
            const auto* color = std::get_if<core::Color4d>(held);
            if (color == nullptr)
                return std::nullopt;
            values.push_back({driver.stop, driver.component, *color});
        } else {
            const auto* position = std::get_if<double>(held);
            if (position == nullptr)
                return std::nullopt;
            values.push_back({driver.stop, driver.component, *position});
        }
    }
    return document::applyGradientStopValues(std::move(*sampled), values);
}

// #543: a compiled gradient parameter at `time` with its driven stops, the value graph's outputs
// for this frame in `outputs`.
[[nodiscard]] inline std::optional<document::GradientValue>
sampleGradientParameter(const CompiledGradientParameter& parameter, const core::RationalTime time,
                        const std::span<const CompiledValue> outputs) {
    return applyGradientStopDrivers(
        sampleGradientSource(parameter.source, time), parameter.drivers,
        [&](const CompiledGradientStopDriver& driver) -> const CompiledValue* {
            const auto index = driver.value.value();
            return index < outputs.size() ? &outputs[index] : nullptr;
        });
}

// The stop table of a gradient: sorted by position (equal positions in stored order), each stop's
// span reciprocal rounded once to Float32 from the Float32 positions.
[[nodiscard]] inline std::vector<ResolvedGradientStop>
resolveGradientStops(const document::GradientValue& value) {
    const auto sorted = document::sortedGradientStops(value);
    std::vector<ResolvedGradientStop> stops;
    stops.reserve(sorted.size());
    for (std::size_t index = 0; index < sorted.size(); ++index) {
        const auto position = static_cast<float>(sorted[index].position);
        float inverse = 0.0F;
        if (index + 1 < sorted.size()) {
            const float span = static_cast<float>(sorted[index + 1].position) - position;
            inverse = span > 0.0F ? 1.0F / span : 0.0F;
            if (!std::isfinite(inverse))
                inverse = 0.0F;
        }
        stops.push_back({position, inverse, sorted[index].color});
    }
    return stops;
}

[[nodiscard]] inline GradientLeads
resolveGradientLeads(const std::span<const ResolvedGradientStop> stops) noexcept {
    GradientLeads leads;
    if (stops.empty())
        return leads;
    const float first = stops.front().position;
    const float after = 1.0F - stops.back().position;
    leads.in = first > 0.0F ? 1.0F / first : 0.0F;
    leads.out = after > 0.0F ? 1.0F / after : 0.0F;
    if (!std::isfinite(leads.in))
        leads.in = 0.0F;
    if (!std::isfinite(leads.out))
        leads.out = 0.0F;
    return leads;
}

// #546: the straight Float32 RGBA of resolved `stops` at `phase` in any mode, the operations
// native GradientV1 performs (document::gradient_math::sample).
[[nodiscard]] inline std::array<float, 4>
sampleResolvedGradient(const std::span<const ResolvedGradientStop> stops, const GradientLeads leads,
                       const document::GradientModes modes, const float phase) noexcept {
    return document::gradient_math::sample<float>(
        stops.size(),
        [&](const std::size_t index) {
            const auto& stop = stops[index];
            return document::gradient_math::Stop<float>{
                stop.position,
                stop.inverseSpan,
                {static_cast<float>(stop.color.red), static_cast<float>(stop.color.green),
                 static_cast<float>(stop.color.blue), static_cast<float>(stop.color.alpha)}};
        },
        leads.in, leads.out, phase, modes);
}

// #541: Glow's octave tint, the colour of sorted `stops` at `t`: held before the first stop and
// after the last, otherwise c + (next - c) * u with one fused multiply-add per channel in the
// segment whose start is the last stop at or before `t` (the last segment includes its end). A
// two-stop gradient at 0 and 1 is exactly schema 1's tint + (outerTint - tint) * t.
[[nodiscard]] inline core::Color4d
gradientTintAt(const std::vector<document::GradientStop>& stops, const double t) {
    if (stops.empty())
        return {};
    if (stops.size() == 1 || t < stops.front().position)
        return stops.front().color;
    if (t > stops.back().position)
        return stops.back().color;
    std::size_t index = 0;
    while (index + 2 < stops.size() && stops[index + 1].position <= t)
        ++index;
    const auto& from = stops[index];
    const auto& to = stops[index + 1];
    const double span = to.position - from.position;
    if (!(span > 0.0))
        return to.color;
    const double u = (t - from.position) / span;
    const auto mix = [u](const double a, const double b) { return std::fma(b - a, u, a); };
    return {mix(from.color.red, to.color.red), mix(from.color.green, to.color.green),
            mix(from.color.blue, to.color.blue), mix(from.color.alpha, to.color.alpha)};
}

// #546: Glow's octave tint from a whole gradient: Linear in RGB exactly as gradientTintAt, any
// other mode through the document's binary64 definition (an octave's tint is one host-resolved
// colour, not a pixel).
[[nodiscard]] inline core::Color4d gradientValueTintAt(const document::GradientValue& value,
                                                       const double t) {
    if (value.modes.isLinearRgb())
        return gradientTintAt(document::sortedGradientStops(value), t);
    return document::gradientColorAt(value, t);
}

// Samples at the full-resolution pixel centre. Straight scene-linear colour and independent alpha
// interpolate first; the final process pixel is premultiplied exactly once. Degenerate geometry
// yields the first stop's colour. Repeat=1 wraps at integer boundaries; repeat=2 mirrors there.
// The colour is held before the first stop and from the last; between two stops it interpolates
// by u = (phase - position) * inverse span in Float32, exactly as native GradientV1.
// Wherever the stops' colours are finite in Float32 (every gradient native GradientV1 prepares),
// colour and alpha follow GradientV1's Float32 operations exactly, so the device's gradient is the
// CPU's bit for bit and a quantizing node after it (Dither, Quantize) gives the same levels on
// both (#676). A stop beyond Float32 keeps binary64, which only the CPU evaluates.
[[nodiscard]] inline render::ImageResult<render::Rgba32f>
gradientPixel(const ResolvedGradient& gradient, const double authorX,
              const double authorY) noexcept {
    const double dx = (gradient.end.x - gradient.start.x) * gradient.aspect;
    const double dy = gradient.end.y - gradient.start.y;
    const double px = (authorX - gradient.start.x) * gradient.aspect;
    const double py = authorY - gradient.start.y;
    const double span = std::hypot(dx, dy);
    double t = 0.0;
    if (gradient.start.x != gradient.end.x || gradient.start.y != gradient.end.y) {
        // Squaring a valid span can overflow or underflow and must not turn a nonzero
        // gradient into the first colour. Normalize before taking the linear projection.
        if (!(span > 0.0) || !std::isfinite(span) || !std::isfinite(px) ||
            !std::isfinite(py))
            return render::ImageResult<render::Rgba32f>::failure(
                render::ImageError::codeOnly(render::ImageErrorCode::NonFiniteResult));
        t = gradient.radial ? std::hypot(px, py) / span
                            : (dx == 0.0 ? 0.0 : (px / span) * (dx / span)) +
                                  (dy == 0.0 ? 0.0 : (py / span) * (dy / span));
    }
    // Clamp can saturate an infinite ratio; periodic modes need a finite phase.
    if (std::isnan(t) || (gradient.repeat != 0 && !std::isfinite(t)) || gradient.stops.empty())
        return render::ImageResult<render::Rgba32f>::failure(
            render::ImageError::codeOnly(render::ImageErrorCode::NonFiniteResult));
    // The phase is reduced in binary64 and rounded once to Float32; Mirror folds that Float32
    // value, exactly as native GradientV1 folds its compensated phase.
    float phase = 0.0F;
    if (gradient.repeat == 1)
        phase = static_cast<float>(t - std::floor(t));
    else if (gradient.repeat == 2) {
        const auto unit = static_cast<float>(t - 2.0 * std::floor(t / 2.0));
        phase = unit <= 1.0F ? unit : 2.0F - unit;
    } else
        phase = static_cast<float>(std::clamp(t, 0.0, 1.0));
    const auto& stops = gradient.stops;
    if (!gradient.modes.isLinearRgb()) {
        // #546: every other mode in Float32, premultiplied once as native GradientV1 does.
        const auto straight = sampleResolvedGradient(stops, gradient.leads, gradient.modes, phase);
        const float alpha = straight[3];
        return render::Rgba32f::fromPremultiplied(straight[0] * alpha, straight[1] * alpha,
                                                  straight[2] * alpha, alpha);
    }
    // A stop's straight colour in Float32, as GradientV1's stop table holds it, when every
    // component is finite there.
    const auto single = [](const core::Color4d& color) -> std::optional<std::array<float, 3>> {
        const std::array values{static_cast<float>(color.red), static_cast<float>(color.green),
                                static_cast<float>(color.blue)};
        if (!std::ranges::all_of(values, [](const float v) { return std::isfinite(v); }))
            return std::nullopt;
        return values;
    };
    // The straight colour premultiplied once: one rounded Float32 product per channel, as
    // GradientV1 writes it.
    const auto premultiply = [](const std::array<float, 3>& straight, const float alpha) {
        const float red = straight[0] * alpha;
        const float green = straight[1] * alpha;
        const float blue = straight[2] * alpha;
        return render::Rgba32f::fromPremultiplied(red, green, blue, alpha);
    };
    const auto held = [&](const core::Color4d& color) {
        const auto alpha = static_cast<float>(color.alpha);
        if (const auto straight = single(color))
            return premultiply(*straight, alpha);
        const auto a = static_cast<double>(alpha);
        return render::Rgba32f::fromPremultiplied(static_cast<float>(color.red * a),
                                                  static_cast<float>(color.green * a),
                                                  static_cast<float>(color.blue * a), alpha);
    };
    if (!(phase > stops.front().position))
        return held(stops.front().color);
    if (phase >= stops.back().position)
        return held(stops.back().color);
    // The last stop at or before the phase; the next one is strictly after it.
    std::size_t index = 0;
    while (index + 2 < stops.size() && stops[index + 1].position <= phase)
        ++index;
    const auto& from = stops[index];
    const auto& to = stops[index + 1];
    const float local = (phase - from.position) * from.inverseSpan;
    // The native GradientV1 Float32 sequence: Float32 factor and stop values, two correctly
    // rounded products and one sum per component, never contracted into an FMA, then one rounded
    // product with alpha per colour channel. Alpha is held to it on every gradient; the colour
    // wherever both stops are finite in Float32 (#676).
    const float remaining = 1.0F - local;
    const float firstAlpha = static_cast<float>(from.color.alpha) * remaining;
    const float secondAlpha = static_cast<float>(to.color.alpha) * local;
    const float alpha = firstAlpha + secondAlpha;
    const auto fromColor = single(from.color);
    const auto toColor = single(to.color);
    if (fromColor && toColor) {
        std::array<float, 3> straight{};
        for (std::size_t channel = 0; channel < straight.size(); ++channel) {
            const float first = (*fromColor)[channel] * remaining;
            const float second = (*toColor)[channel] * local;
            straight[channel] = first + second;
        }
        return premultiply(straight, alpha);
    }
    // A stop beyond Float32 (only the CPU evaluates it): binary64 weights, so a finite sample
    // between oversized endpoints survives.
    const auto u = static_cast<double>(local);
    const double remainingU = 1.0 - u;
    const auto a = static_cast<double>(alpha);
    return render::Rgba32f::fromPremultiplied(
        static_cast<float>((remainingU * from.color.red + u * to.color.red) * a),
        static_cast<float>((remainingU * from.color.green + u * to.color.green) * a),
        static_cast<float>((remainingU * from.color.blue + u * to.color.blue) * a), alpha);
}
} // namespace bloom::runtime::detail
