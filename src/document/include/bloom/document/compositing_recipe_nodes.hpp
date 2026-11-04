#pragma once

// #339 Drop Shadow, Glow and Light Wrap: the durable vocabulary of the three compositing recipe
// nodes. Each is an Image in and out (Glow also takes an optional key Image, an optional Mask and,
// since #506, an optional lens-dirt Image; Light Wrap takes an optional background Image) and
// lowers to a fixed chain of the image primitives -- the separable blurs, Transform 2D, Luma Key,
// Set Alpha, Multiply Alpha, Image Mix, Levels, Channel Select and an explicitly blended Merge, and
// for #506 Glow's Streak mode and roll-off the Directional Blur and Highlight Roll-off primitives --
// so no recipe has a pixel kernel of its own (docs/architecture/image-filters.md, "Drop Shadow, Glow and Light Wrap").

#include <cstdint>
#include <optional>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kDropShadowNodeType = "bloom.drop-shadow";
inline constexpr std::string_view kGlowNodeType = "bloom.glow";
inline constexpr std::string_view kLightWrapNodeType = "bloom.light-wrap";

// Drop Shadow. The shadow is the input's alpha, softened, moved `distance` pixels toward
// `direction` (degrees clockwise from up: 135 is down and to the right), coloured, faded by
// `opacity` and composited under the input.
inline constexpr std::string_view kDropShadowColorSchemaKey = "bloom.drop-shadow.color";
inline constexpr std::string_view kDropShadowOpacitySchemaKey = "bloom.drop-shadow.opacity";
inline constexpr std::string_view kDropShadowDirectionSchemaKey = "bloom.drop-shadow.direction";
inline constexpr std::string_view kDropShadowDistanceSchemaKey = "bloom.drop-shadow.distance";
inline constexpr std::string_view kDropShadowSoftnessSchemaKey = "bloom.drop-shadow.softness";
inline constexpr std::string_view kDropShadowShadowOnlySchemaKey = "bloom.drop-shadow.shadow-only";

// Glow. The pixels whose luma passes `threshold` (softened over `knee`) are blurred at `octaves`
// radii doubling up to `size`, each wider octave weighing `falloff` times the one inside it,
// tinted, scaled by `intensity` and added to the input.
inline constexpr std::string_view kGlowThresholdSchemaKey = "bloom.glow.threshold";
inline constexpr std::string_view kGlowKneeSchemaKey = "bloom.glow.knee";
inline constexpr std::string_view kGlowIntensitySchemaKey = "bloom.glow.intensity";
inline constexpr std::string_view kGlowSizeSchemaKey = "bloom.glow.size";
inline constexpr std::string_view kGlowFalloffSchemaKey = "bloom.glow.falloff";
inline constexpr std::string_view kGlowTintSchemaKey = "bloom.glow.tint";
inline constexpr std::string_view kGlowOctavesSchemaKey = "bloom.glow.octaves";
inline constexpr std::string_view kGlowGlowOnlySchemaKey = "bloom.glow.glow-only";
inline constexpr std::int64_t kMinimumGlowOctaves = 1;
inline constexpr std::int64_t kMaximumGlowOctaves = 8;
inline constexpr std::int64_t kDefaultGlowOctaves = 5;

// #506 Glow modes and shared controls. Every one of these parameters is optional: a Glow node saved
// before #506 has none of them bound and lowers exactly as Classic with the defaults below, so its
// picture is unchanged. A new node binds them all.
//   mode        Classic (0, the #339 glow), Physical (1: multi-scale, inverse-power falloff,
//               energy-conserving) or Streak (2: N-point star or anamorphic streaks). Constant.
//   exponent    Physical: the point-spread falloff exponent p, PSF ~ r^-p (2 is inverse square).
//   radiusRed/Green/Blue   per-channel fraction of `size` (Streak: of `length`), so a channel's
//               glow can be tighter than the others.
//   gradient, outerTint    with the gradient on, the octaves run from `tint` (tightest) to
//               `outerTint` (widest) instead of all taking `tint`.
//   points, angle, length, chromatic   Streak: the number of rays (2 is an anamorphic streak), the
//               first ray's angle in degrees clockwise from screen right, the ray length in pixels,
//               and the dispersion that shortens green and blue against red.
//   dirtAmount  the gain of the light the optional `dirt` Image adds (the glow times its luma).
//   rollOff     the glow layer's highlight roll-off: c / (1 + rollOff * luma), 0 is off.
inline constexpr std::string_view kGlowModeSchemaKey = "bloom.glow.mode";
inline constexpr std::string_view kGlowExponentSchemaKey = "bloom.glow.exponent";
inline constexpr std::string_view kGlowRadiusRedSchemaKey = "bloom.glow.radius-red";
inline constexpr std::string_view kGlowRadiusGreenSchemaKey = "bloom.glow.radius-green";
inline constexpr std::string_view kGlowRadiusBlueSchemaKey = "bloom.glow.radius-blue";
inline constexpr std::string_view kGlowGradientSchemaKey = "bloom.glow.gradient";
inline constexpr std::string_view kGlowOuterTintSchemaKey = "bloom.glow.outer-tint";
inline constexpr std::string_view kGlowPointsSchemaKey = "bloom.glow.points";
inline constexpr std::string_view kGlowAngleSchemaKey = "bloom.glow.angle";
inline constexpr std::string_view kGlowLengthSchemaKey = "bloom.glow.length";
inline constexpr std::string_view kGlowChromaticSchemaKey = "bloom.glow.chromatic";
inline constexpr std::string_view kGlowDirtAmountSchemaKey = "bloom.glow.dirt-amount";
inline constexpr std::string_view kGlowRollOffSchemaKey = "bloom.glow.roll-off";
inline constexpr std::int64_t kGlowModeClassic = 0;
inline constexpr std::int64_t kGlowModePhysical = 1;
inline constexpr std::int64_t kGlowModeStreak = 2;
inline constexpr std::int64_t kMinimumGlowPoints = 1;
inline constexpr std::int64_t kMaximumGlowPoints = 16;
inline constexpr std::int64_t kDefaultGlowPoints = 4;
inline constexpr double kMaximumGlowStreakLength = 1024.0;
inline constexpr double kMaximumGlowExponent = 8.0;
inline constexpr double kMaximumGlowRollOff = 100.0;

// Light Wrap. The blurred background replaces the foreground's colour inside its edges, by
// `amount` times the part of a `radius` blur of the foreground's alpha that is missing there.
inline constexpr std::string_view kLightWrapRadiusSchemaKey = "bloom.light-wrap.radius";
inline constexpr std::string_view kLightWrapAmountSchemaKey = "bloom.light-wrap.amount";
inline constexpr std::string_view kLightWrapWrapOnlySchemaKey = "bloom.light-wrap.wrap-only";

// The radii are the blur radii, so they share kMaximumBlurRadiusPixels (parameter.hpp).
inline constexpr double kMaximumRecipeBlurRadius = 256.0;
inline constexpr double kMaximumDropShadowDistance = 16384.0;
inline constexpr double kMaximumDropShadowDirection = 1.0e6;
inline constexpr double kMaximumGlowThreshold = 1.0e5;
inline constexpr double kMaximumGlowIntensity = 1000.0;

struct RecipeScalarDomain final {
    double minimum = 0.0;
    double maximum = 0.0;
    double defaultValue = 0.0;
};

// The domain and default of every animatable Scalar of the three nodes, or nothing for any other
// schema key. Every value is finite within its domain.
[[nodiscard]] constexpr std::optional<RecipeScalarDomain>
recipeScalarDomain(const std::string_view schemaKey) noexcept {
    if (schemaKey == kDropShadowOpacitySchemaKey)
        return RecipeScalarDomain{0.0, 1.0, 0.75};
    if (schemaKey == kDropShadowDirectionSchemaKey)
        return RecipeScalarDomain{-kMaximumDropShadowDirection, kMaximumDropShadowDirection, 135.0};
    if (schemaKey == kDropShadowDistanceSchemaKey)
        return RecipeScalarDomain{0.0, kMaximumDropShadowDistance, 10.0};
    if (schemaKey == kDropShadowSoftnessSchemaKey)
        return RecipeScalarDomain{0.0, kMaximumRecipeBlurRadius, 8.0};
    if (schemaKey == kGlowThresholdSchemaKey)
        return RecipeScalarDomain{-kMaximumGlowThreshold, kMaximumGlowThreshold, 1.0};
    if (schemaKey == kGlowKneeSchemaKey)
        return RecipeScalarDomain{0.0, kMaximumGlowThreshold, 0.5};
    if (schemaKey == kGlowIntensitySchemaKey)
        return RecipeScalarDomain{0.0, kMaximumGlowIntensity, 1.0};
    if (schemaKey == kGlowSizeSchemaKey)
        return RecipeScalarDomain{0.0, kMaximumRecipeBlurRadius, 64.0};
    if (schemaKey == kGlowFalloffSchemaKey)
        return RecipeScalarDomain{0.0, 1.0, 0.6};
    if (schemaKey == kGlowExponentSchemaKey)
        return RecipeScalarDomain{0.0, kMaximumGlowExponent, 2.0};
    if (schemaKey == kGlowRadiusRedSchemaKey || schemaKey == kGlowRadiusGreenSchemaKey ||
        schemaKey == kGlowRadiusBlueSchemaKey)
        return RecipeScalarDomain{0.0, 1.0, 1.0};
    if (schemaKey == kGlowAngleSchemaKey)
        return RecipeScalarDomain{-kMaximumDropShadowDirection, kMaximumDropShadowDirection, 0.0};
    if (schemaKey == kGlowLengthSchemaKey)
        return RecipeScalarDomain{0.0, kMaximumGlowStreakLength, 128.0};
    if (schemaKey == kGlowChromaticSchemaKey)
        return RecipeScalarDomain{0.0, 1.0, 0.0};
    if (schemaKey == kGlowDirtAmountSchemaKey)
        return RecipeScalarDomain{0.0, kMaximumGlowIntensity, 1.0};
    if (schemaKey == kGlowRollOffSchemaKey)
        return RecipeScalarDomain{0.0, kMaximumGlowRollOff, 0.0};
    if (schemaKey == kLightWrapRadiusSchemaKey)
        return RecipeScalarDomain{0.0, kMaximumRecipeBlurRadius, 16.0};
    if (schemaKey == kLightWrapAmountSchemaKey)
        return RecipeScalarDomain{0.0, 1.0, 0.75};
    return std::nullopt;
}

[[nodiscard]] constexpr bool isRecipeScalarSchemaKey(const std::string_view schemaKey) noexcept {
    return recipeScalarDomain(schemaKey).has_value();
}

// The animatable straight RGBA colours: the shadow colour (its alpha scales the opacity) and the
// glow tints (their alpha is not used).
[[nodiscard]] constexpr bool isRecipeColorSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kDropShadowColorSchemaKey || schemaKey == kGlowTintSchemaKey ||
           schemaKey == kGlowOuterTintSchemaKey;
}

// The constant switches: no socket, no animation.
[[nodiscard]] constexpr bool isRecipeBooleanSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kDropShadowShadowOnlySchemaKey || schemaKey == kGlowGlowOnlySchemaKey ||
           schemaKey == kLightWrapWrapOnlySchemaKey || schemaKey == kGlowGradientSchemaKey;
}

// The constant integers (they decide the chain's shape): no socket, no animation, each within its
// closed range.
struct RecipeIntegerDomain final {
    std::int64_t minimum = 0;
    std::int64_t maximum = 0;
    std::int64_t defaultValue = 0;
};
[[nodiscard]] constexpr std::optional<RecipeIntegerDomain>
recipeIntegerDomain(const std::string_view schemaKey) noexcept {
    if (schemaKey == kGlowOctavesSchemaKey)
        return RecipeIntegerDomain{kMinimumGlowOctaves, kMaximumGlowOctaves, kDefaultGlowOctaves};
    if (schemaKey == kGlowModeSchemaKey)
        return RecipeIntegerDomain{kGlowModeClassic, kGlowModeStreak, kGlowModeClassic};
    if (schemaKey == kGlowPointsSchemaKey)
        return RecipeIntegerDomain{kMinimumGlowPoints, kMaximumGlowPoints, kDefaultGlowPoints};
    return std::nullopt;
}
[[nodiscard]] constexpr bool isRecipeIntegerSchemaKey(const std::string_view schemaKey) noexcept {
    return recipeIntegerDomain(schemaKey).has_value();
}

} // namespace bloom::document
