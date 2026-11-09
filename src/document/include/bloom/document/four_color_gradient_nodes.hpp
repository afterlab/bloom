#pragma once

// Four-Color Gradient: the node's parameter vocabulary. A source that fills the composition with
// four colors, each anchored at a point; every pixel takes the average of the four colors weighted
// by the inverse of its distance to each point raised to the blend power, so a point's own color
// holds at the point and the colors run into each other between points.
//
//   point1..point4  where each color is anchored, an offset in pixels from the composition's center
//                   with y up, like a field position. Animatable.
//   color1..color4  the colors (straight, scene-linear), alpha included. Animatable.
//   blend           the power of the inverse distance: a low power spreads each color over the
//                   frame, a high one confines it to its point. Animatable.
//
// docs/architecture/image-filters.md, "Four-Color Gradient", is the specification.
//
// Schema keys are "bloom.four-color-gradient.<role>"; every parameter is animatable, drivable and
// has a socket of its own.

#include <array>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kFourColorGradientNodeType = "bloom.four-color-gradient";
inline constexpr unsigned kFourColorGradientNodeSchemaVersion = 1;
inline constexpr std::string_view kFourColorGradientImagePort = "image";
inline constexpr std::string_view kFourColorGradientBlendSchemaKey =
    "bloom.four-color-gradient.blend";

inline constexpr std::size_t kFourColorGradientPoints = 4;
inline constexpr std::array<std::string_view, 4> kFourColorGradientPointRoles{"point1", "point2",
                                                                              "point3", "point4"};
inline constexpr std::array<std::string_view, 4> kFourColorGradientColorRoles{"color1", "color2",
                                                                              "color3", "color4"};
inline constexpr std::array<std::string_view, 4> kFourColorGradientPointSchemaKeys{
    "bloom.four-color-gradient.point1", "bloom.four-color-gradient.point2",
    "bloom.four-color-gradient.point3", "bloom.four-color-gradient.point4"};
inline constexpr std::array<std::string_view, 4> kFourColorGradientColorSchemaKeys{
    "bloom.four-color-gradient.color1", "bloom.four-color-gradient.color2",
    "bloom.four-color-gradient.color3", "bloom.four-color-gradient.color4"};

// Document domains.
inline constexpr double kFourColorGradientBlendDefault = 2.0;
inline constexpr double kMinimumFourColorGradientBlend = 0.25;
inline constexpr double kMaximumFourColorGradientBlend = 8.0;
// A point's reach, in pixels from the frame's center, and the largest color channel.
inline constexpr double kMaximumFourColorGradientPoint = 1.0e6;
inline constexpr double kMaximumFourColorGradientColor = 100.0;

[[nodiscard]] constexpr bool isFourColorGradientPointSchemaKey(const std::string_view key) noexcept {
    for (const auto point : kFourColorGradientPointSchemaKeys)
        if (key == point)
            return true;
    return false;
}
[[nodiscard]] constexpr bool isFourColorGradientColorSchemaKey(const std::string_view key) noexcept {
    for (const auto color : kFourColorGradientColorSchemaKeys)
        if (key == color)
            return true;
    return false;
}
[[nodiscard]] constexpr bool
isFourColorGradientScalarSchemaKey(const std::string_view key) noexcept {
    return key == kFourColorGradientBlendSchemaKey;
}
[[nodiscard]] constexpr bool
isFourColorGradientBlendWithinDomain(const double value) noexcept {
    return value >= kMinimumFourColorGradientBlend && value <= kMaximumFourColorGradientBlend;
}

} // namespace bloom::document
