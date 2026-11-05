#pragma once

#include <bloom/core/color.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/compiled_curves.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
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

struct ResolvedGradient final {
    std::vector<ResolvedGradientStop> stops;
    document::Vec2d start;
    document::Vec2d end;
    std::int64_t repeat = 0;
    bool radial = false;
    double aspect = 1.0;
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

// Samples at the full-resolution pixel centre. Straight scene-linear colour and independent alpha
// interpolate first; the final process pixel is premultiplied exactly once. Degenerate geometry
// yields the first stop's colour. Repeat=1 wraps at integer boundaries; repeat=2 mirrors there.
// The colour is held before the first stop and from the last; between two stops it interpolates
// by u = (phase - position) * inverse span in Float32, exactly as native GradientV1.
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
    const auto held = [](const core::Color4d& color) {
        const auto alpha = static_cast<float>(color.alpha);
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
    const auto u = static_cast<double>(local);
    const double remainingU = 1.0 - u;
    // Alpha is part of the bit-exact CPU/GPU parity gate, so it follows the native GradientV1
    // Float32 sequence exactly: Float32 factor and stop alphas, two correctly rounded products and
    // one sum, never contracted into an FMA. RGB keeps binary64 interpolation (2e-6 bound).
    const float remaining = 1.0F - local;
    const float firstAlpha = static_cast<float>(from.color.alpha) * remaining;
    const float secondAlpha = static_cast<float>(to.color.alpha) * local;
    const float alpha = firstAlpha + secondAlpha;
    const auto a = static_cast<double>(alpha);
    return render::Rgba32f::fromPremultiplied(
        static_cast<float>((remainingU * from.color.red + u * to.color.red) * a),
        static_cast<float>((remainingU * from.color.green + u * to.color.green) * a),
        static_cast<float>((remainingU * from.color.blue + u * to.color.blue) * a), alpha);
}
} // namespace bloom::runtime::detail
