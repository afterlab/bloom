#pragma once

// #505 Vignette: the durable vocabulary of the one node that darkens (or brightens) an image
// towards the edges of its frame. An Image in and out. The falloff is measured from `center` (an
// offset in pixels from the centre of the input's display window) in units of the frame's own
// half-width and half-height, so the shape follows the frame at any resolution:
//   shape      Elliptical (0) or Rectangular (1, a rounded rectangle), constant.
//   roundness  above 0 pulls the shape towards a circle (a square) whatever the frame's aspect;
//              below 0 squares its corners off.
//   midpoint   how far out the falloff is centred: 0 near the middle, 1 out past the corners.
//   feather    how wide the falloff is: 0 a hard edge, 1 from the centre to twice the midpoint.
//   mode       Multiply (0): the colour times 1 - amount * mask; Exposure (1): the colour times
//              2^(exposure * mask), in stops. Constant.
//   highlightProtection keeps that share of the original exposure for pixels at or above 1.0
//              (scene-linear), easing in from mid grey (0.18).
// Every value but the two choices is animatable and drivable. The node never transforms colour
// and keeps alpha. docs/architecture/image-filters.md, "Vignette", is the specification.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kVignetteNodeType = "bloom.vignette";

inline constexpr std::string_view kVignetteShapeSchemaKey = "bloom.vignette.shape";
inline constexpr std::string_view kVignetteModeSchemaKey = "bloom.vignette.mode";
inline constexpr std::string_view kVignetteAmountSchemaKey = "bloom.vignette.amount";
inline constexpr std::string_view kVignetteExposureSchemaKey = "bloom.vignette.exposure";
inline constexpr std::string_view kVignetteMidpointSchemaKey = "bloom.vignette.midpoint";
inline constexpr std::string_view kVignetteRoundnessSchemaKey = "bloom.vignette.roundness";
inline constexpr std::string_view kVignetteFeatherSchemaKey = "bloom.vignette.feather";
inline constexpr std::string_view kVignetteHighlightProtectionSchemaKey =
    "bloom.vignette.highlight-protection";
inline constexpr std::string_view kVignetteCenterSchemaKey = "bloom.vignette.center";

inline constexpr std::int64_t kVignetteElliptical = 0;
inline constexpr std::int64_t kVignetteRectangular = 1;

inline constexpr std::int64_t kVignetteMultiply = 0;
inline constexpr std::int64_t kVignetteExposure = 1;

// The centre offset is finite and within +-1e6 pixels on each axis.
inline constexpr double kMaximumVignetteCenter = 1.0e6;
// Highlight protection eases in from scene-linear mid grey and is complete at 1.0.
inline constexpr double kVignetteProtectionLow = 0.18;
inline constexpr double kVignetteProtectionHigh = 1.0;

struct VignetteScalarDomain final {
    double minimum = 0.0;
    double maximum = 0.0;
    double defaultValue = 0.0;
};

// The domain and default of every animatable Scalar of the node, or nothing for any other key.
[[nodiscard]] constexpr std::optional<VignetteScalarDomain>
vignetteScalarDomain(const std::string_view schemaKey) noexcept {
    if (schemaKey == kVignetteAmountSchemaKey)
        return VignetteScalarDomain{-1.0, 1.0, 0.5};
    if (schemaKey == kVignetteExposureSchemaKey)
        return VignetteScalarDomain{-10.0, 10.0, -1.0};
    if (schemaKey == kVignetteMidpointSchemaKey)
        return VignetteScalarDomain{0.0, 1.0, 0.5};
    if (schemaKey == kVignetteRoundnessSchemaKey)
        return VignetteScalarDomain{-1.0, 1.0, 0.0};
    if (schemaKey == kVignetteFeatherSchemaKey)
        return VignetteScalarDomain{0.0, 1.0, 0.5};
    if (schemaKey == kVignetteHighlightProtectionSchemaKey)
        return VignetteScalarDomain{0.0, 1.0, 0.0};
    return std::nullopt;
}
[[nodiscard]] constexpr bool isVignetteScalarSchemaKey(const std::string_view schemaKey) noexcept {
    return vignetteScalarDomain(schemaKey).has_value();
}

// The constant choices (they decide the arithmetic): no socket, no animation, each closed.
struct VignetteIntegerDomain final {
    std::int64_t minimum = 0;
    std::int64_t maximum = 0;
    std::int64_t defaultValue = 0;
};
[[nodiscard]] constexpr std::optional<VignetteIntegerDomain>
vignetteIntegerDomain(const std::string_view schemaKey) noexcept {
    if (schemaKey == kVignetteShapeSchemaKey)
        return VignetteIntegerDomain{kVignetteElliptical, kVignetteRectangular,
                                     kVignetteElliptical};
    if (schemaKey == kVignetteModeSchemaKey)
        return VignetteIntegerDomain{kVignetteMultiply, kVignetteExposure, kVignetteMultiply};
    return std::nullopt;
}
[[nodiscard]] constexpr bool isVignetteIntegerSchemaKey(const std::string_view schemaKey) noexcept {
    return vignetteIntegerDomain(schemaKey).has_value();
}

// The falloff a shape, midpoint, roundness and feather describe, shared by the renderer and the
// card's preview so both draw the same vignette. Distances are in half-frame units: the shape
// with roundness 0 touches the frame's edges at distance 1.
struct VignetteFalloff final {
    // The mask rises from 0 at `inner` to 1 at `outer` (a smoothstep); equal is a hard edge.
    double inner = 0.0;
    double outer = 0.0;
    // The superellipse exponent: 2 an ellipse, larger towards a rectangle.
    double exponent = 2.0;
    // How far the half-axes move from the frame's towards a circle's, in [0, 1].
    double circle = 0.0;
};
[[nodiscard]] inline VignetteFalloff vignetteFalloff(const std::int64_t shape, const double midpoint,
                                                     const double roundness,
                                                     const double feather) noexcept {
    const double centre = 0.4 + 1.2 * midpoint;
    const double base = shape == kVignetteRectangular ? 6.0 : 2.0;
    return {centre * (1.0 - feather), centre * (1.0 + feather),
            roundness < 0.0 ? base * std::pow(4.0, -roundness) : base, std::max(roundness, 0.0)};
}
// The half-axes, in the same units as the frame's physical `width` and `height`: the frame's own
// halves moved geometrically by `circle` towards their geometric mean (a circle of the same area).
struct VignetteHalfAxes final {
    double x = 1.0;
    double y = 1.0;
};
[[nodiscard]] inline VignetteHalfAxes vignetteHalfAxes(const double width, const double height,
                                                       const double circle) noexcept {
    const double x = 0.5 * width;
    const double y = 0.5 * height;
    const double mean = std::sqrt(x * y);
    return {x * std::pow(mean / x, circle), y * std::pow(mean / y, circle)};
}
// The mask (0 untouched, 1 the full vignette) at (u, v), a point already divided by the half-axes.
[[nodiscard]] inline double vignetteMask(const VignetteFalloff& falloff, const double u,
                                         const double v) noexcept {
    const double x = std::min(std::abs(u), 4.0);
    const double y = std::min(std::abs(v), 4.0);
    const double d =
        falloff.exponent == 2.0
            ? std::sqrt(x * x + y * y)
            : std::pow(std::pow(x, falloff.exponent) + std::pow(y, falloff.exponent),
                       1.0 / falloff.exponent);
    if (!(falloff.outer > falloff.inner))
        return d >= falloff.inner ? 1.0 : 0.0;
    const double t = std::clamp((d - falloff.inner) / (falloff.outer - falloff.inner), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

} // namespace bloom::document
