#pragma once

// #339 Drop Shadow, Glow and Light Wrap: the durable vocabulary of the three compositing recipe
// nodes. Each is an Image in and out (Glow also takes an optional key Image and an optional Mask;
// Light Wrap takes an optional background Image) and lowers to a fixed chain of the existing image
// primitives -- the separable blurs, Transform 2D, Luma Key, Set Alpha, Multiply Alpha, Image Mix,
// Levels and an explicitly blended Merge -- so it has no pixel kernel of its own
// (docs/architecture/image-filters.md, "Drop Shadow, Glow and Light Wrap").

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
// glow tint (its alpha is not used).
[[nodiscard]] constexpr bool isRecipeColorSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kDropShadowColorSchemaKey || schemaKey == kGlowTintSchemaKey;
}

// The constant switches and the octave count: no socket, no animation.
[[nodiscard]] constexpr bool isRecipeBooleanSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kDropShadowShadowOnlySchemaKey || schemaKey == kGlowGlowOnlySchemaKey ||
           schemaKey == kLightWrapWrapOnlySchemaKey;
}

} // namespace bloom::document
