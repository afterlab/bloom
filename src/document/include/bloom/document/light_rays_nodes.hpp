#pragma once

// #508 Light Rays: the node's parameter vocabulary. Volumetric rays that radiate from a point: each
// pixel gathers the source along the line towards the center, so a bright or opaque area streaks
// away from the center; the streaks are tinted, boosted and added to the input.
//
//   source   what drives the rays: the input's luminance (0) or its alpha (1). Constant.
//   center   the point the rays radiate from, an offset in pixels from the composition's center
//            with y down. Animatable.
//   length   how far along the line towards the center each pixel gathers, as a fraction of its
//            distance to the center: 0 gathers nothing, 1 reaches the center. Animatable.
//   falloff  how fast the rays fade with distance from their source: a sample at a fraction t of
//            the way along the gathered span weighs (1 - t) to this power. 0 weighs them evenly.
//            Animatable.
//   boost    a multiplier on the gathered rays. Animatable.
//   tint     the color the rays are multiplied by (white leaves the source's own color).
//            Constant.
//
// docs/architecture/image-filters.md, "Light Rays", is the specification.
//
// Schema keys are "bloom.light-rays.<role>". The center, length, falloff and boost are animatable,
// drivable and have a socket of their own; the source and the color are constants with no socket.

#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kLightRaysNodeType = "bloom.light-rays";
inline constexpr unsigned kLightRaysNodeSchemaVersion = 1;

inline constexpr std::string_view kLightRaysSourceSchemaKey = "bloom.light-rays.source";
inline constexpr std::string_view kLightRaysCenterSchemaKey = "bloom.light-rays.center";
inline constexpr std::string_view kLightRaysLengthSchemaKey = "bloom.light-rays.length";
inline constexpr std::string_view kLightRaysFalloffSchemaKey = "bloom.light-rays.falloff";
inline constexpr std::string_view kLightRaysBoostSchemaKey = "bloom.light-rays.boost";
inline constexpr std::string_view kLightRaysColorSchemaKey = "bloom.light-rays.tint";

inline constexpr long long kLightRaysFromLuminance = 0;
inline constexpr long long kLightRaysFromAlpha = 1;

// Document domains, and the span each slider covers.
inline constexpr double kLightRaysLengthDefault = 0.5;
inline constexpr double kMinimumLightRaysLength = 0.0;
inline constexpr double kMaximumLightRaysLength = 1.0;
inline constexpr double kLightRaysFalloffDefault = 1.0;
inline constexpr double kMinimumLightRaysFalloff = 0.0;
inline constexpr double kMaximumLightRaysFalloff = 8.0;
inline constexpr double kLightRaysFalloffSoftMaximum = 4.0;
inline constexpr double kLightRaysBoostDefault = 1.5;
inline constexpr double kMinimumLightRaysBoost = 0.0;
inline constexpr double kMaximumLightRaysBoost = 100.0;
inline constexpr double kLightRaysBoostSoftMaximum = 4.0;
// The center's reach, in pixels from the frame's center.
inline constexpr double kMaximumLightRaysCenter = 1.0e6;
// The largest color channel.
inline constexpr double kMaximumLightRaysColor = 100.0;

[[nodiscard]] constexpr bool isLightRaysScalarSchemaKey(const std::string_view key) noexcept {
    return key == kLightRaysLengthSchemaKey || key == kLightRaysFalloffSchemaKey ||
           key == kLightRaysBoostSchemaKey;
}
// The parameters that are constants: no socket and no animation.
[[nodiscard]] constexpr bool isLightRaysConstantSchemaKey(const std::string_view key) noexcept {
    return key == kLightRaysSourceSchemaKey || key == kLightRaysColorSchemaKey;
}
// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isLightRaysScalarWithinDomain(const std::string_view key,
                                                           const double value) noexcept {
    if (key == kLightRaysLengthSchemaKey)
        return value >= kMinimumLightRaysLength && value <= kMaximumLightRaysLength;
    if (key == kLightRaysFalloffSchemaKey)
        return value >= kMinimumLightRaysFalloff && value <= kMaximumLightRaysFalloff;
    if (key == kLightRaysBoostSchemaKey)
        return value >= kMinimumLightRaysBoost && value <= kMaximumLightRaysBoost;
    return true;
}

} // namespace bloom::document
