#pragma once

// #504 Colorize: the durable vocabulary of the one Grade node that replaces an image's colour from
// its own tones. An Image in and out, one constant mode:
//   Fill (0)          a flat colour, the alpha kept.
//   Tint (1)          luma mapped linearly from `shadows` (black) to `highlights` (white).
//   Tritone (2)       luma through the three-stop ramp shadows, midtones (at `midpoint`),
//                     highlights.
//   Gradient Map (3)  luma or one channel (`source`) through the same ramp, scaled by
//                     `repetitions`, shifted by `phase` and clamped, repeated or mirrored
//                     (`cycle`).
//   Black & White (4) a grey from the six hue weights (reds, yellows, greens, cyans, blues,
//                     magentas).
// `amount` blends the result with the original straight colour. Every mode works on the straight
// scene-linear values as stored (HDR, signed), never transforms colour and keeps alpha.
// docs/architecture/image-filters.md, "Colorize", is the specification.

#include <cstdint>
#include <optional>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kColorizeNodeType = "bloom.colorize";

inline constexpr std::string_view kColorizeModeSchemaKey = "bloom.colorize.mode";
inline constexpr std::string_view kColorizeAmountSchemaKey = "bloom.colorize.amount";
inline constexpr std::string_view kColorizeColorSchemaKey = "bloom.colorize.fill-color";
inline constexpr std::string_view kColorizeShadowsSchemaKey = "bloom.colorize.shadows";
inline constexpr std::string_view kColorizeMidtonesSchemaKey = "bloom.colorize.midtones";
inline constexpr std::string_view kColorizeHighlightsSchemaKey = "bloom.colorize.highlights";
inline constexpr std::string_view kColorizeMidpointSchemaKey = "bloom.colorize.midpoint";
inline constexpr std::string_view kColorizeSourceSchemaKey = "bloom.colorize.source";
inline constexpr std::string_view kColorizeCycleSchemaKey = "bloom.colorize.cycle";
inline constexpr std::string_view kColorizeRepetitionsSchemaKey = "bloom.colorize.repetitions";
inline constexpr std::string_view kColorizePhaseSchemaKey = "bloom.colorize.phase";
inline constexpr std::string_view kColorizeRedsSchemaKey = "bloom.colorize.reds";
inline constexpr std::string_view kColorizeYellowsSchemaKey = "bloom.colorize.yellows";
inline constexpr std::string_view kColorizeGreensSchemaKey = "bloom.colorize.greens";
inline constexpr std::string_view kColorizeCyansSchemaKey = "bloom.colorize.cyans";
inline constexpr std::string_view kColorizeBluesSchemaKey = "bloom.colorize.blues";
inline constexpr std::string_view kColorizeMagentasSchemaKey = "bloom.colorize.magentas";

inline constexpr std::int64_t kColorizeFill = 0;
inline constexpr std::int64_t kColorizeTint = 1;
inline constexpr std::int64_t kColorizeTritone = 2;
inline constexpr std::int64_t kColorizeGradientMap = 3;
inline constexpr std::int64_t kColorizeBlackAndWhite = 4;

// What Gradient Map reads: the Rec. 709 luma or one straight channel.
inline constexpr std::int64_t kColorizeSourceLuma = 0;
inline constexpr std::int64_t kColorizeSourceRed = 1;
inline constexpr std::int64_t kColorizeSourceGreen = 2;
inline constexpr std::int64_t kColorizeSourceBlue = 3;
inline constexpr std::int64_t kColorizeSourceAlpha = 4;

// How Gradient Map treats a position outside the ramp: the Linear Gradient's vocabulary.
inline constexpr std::int64_t kColorizeCycleClamp = 0;
inline constexpr std::int64_t kColorizeCycleRepeat = 1;
inline constexpr std::int64_t kColorizeCycleMirror = 2;

// Every colour's RGB is finite and within +-1e6, like the grade values; its alpha is unused.
inline constexpr double kMaximumColorizeColor = 1.0e6;

struct ColorizeScalarDomain final {
    double minimum = 0.0;
    double maximum = 0.0;
    double defaultValue = 0.0;
};

// The domain and default of every animatable Scalar of the node, or nothing for any other key. The
// midpoint keeps both ramp segments at least 1% wide; the default 0.18 is scene-linear mid grey.
// The six weights default to the classic black-and-white mix and span -200% to 300%.
[[nodiscard]] constexpr std::optional<ColorizeScalarDomain>
colorizeScalarDomain(const std::string_view schemaKey) noexcept {
    if (schemaKey == kColorizeAmountSchemaKey)
        return ColorizeScalarDomain{0.0, 1.0, 1.0};
    if (schemaKey == kColorizeMidpointSchemaKey)
        return ColorizeScalarDomain{0.01, 0.99, 0.18};
    if (schemaKey == kColorizeRepetitionsSchemaKey)
        return ColorizeScalarDomain{0.0, 100.0, 1.0};
    if (schemaKey == kColorizePhaseSchemaKey)
        return ColorizeScalarDomain{-1000.0, 1000.0, 0.0};
    if (schemaKey == kColorizeRedsSchemaKey || schemaKey == kColorizeGreensSchemaKey)
        return ColorizeScalarDomain{-2.0, 3.0, 0.4};
    if (schemaKey == kColorizeYellowsSchemaKey || schemaKey == kColorizeCyansSchemaKey)
        return ColorizeScalarDomain{-2.0, 3.0, 0.6};
    if (schemaKey == kColorizeBluesSchemaKey)
        return ColorizeScalarDomain{-2.0, 3.0, 0.2};
    if (schemaKey == kColorizeMagentasSchemaKey)
        return ColorizeScalarDomain{-2.0, 3.0, 0.8};
    return std::nullopt;
}

[[nodiscard]] constexpr bool isColorizeScalarSchemaKey(const std::string_view schemaKey) noexcept {
    return colorizeScalarDomain(schemaKey).has_value();
}

// The animatable straight colours (their alpha is not used).
[[nodiscard]] constexpr bool isColorizeColorSchemaKey(const std::string_view schemaKey) noexcept {
    return schemaKey == kColorizeColorSchemaKey || schemaKey == kColorizeShadowsSchemaKey ||
           schemaKey == kColorizeMidtonesSchemaKey || schemaKey == kColorizeHighlightsSchemaKey;
}

// The constant choices (they decide the arithmetic): no socket, no animation, each closed.
struct ColorizeIntegerDomain final {
    std::int64_t minimum = 0;
    std::int64_t maximum = 0;
    std::int64_t defaultValue = 0;
};
[[nodiscard]] constexpr std::optional<ColorizeIntegerDomain>
colorizeIntegerDomain(const std::string_view schemaKey) noexcept {
    if (schemaKey == kColorizeModeSchemaKey)
        return ColorizeIntegerDomain{kColorizeFill, kColorizeBlackAndWhite, kColorizeTint};
    if (schemaKey == kColorizeSourceSchemaKey)
        return ColorizeIntegerDomain{kColorizeSourceLuma, kColorizeSourceAlpha,
                                     kColorizeSourceLuma};
    if (schemaKey == kColorizeCycleSchemaKey)
        return ColorizeIntegerDomain{kColorizeCycleClamp, kColorizeCycleMirror,
                                     kColorizeCycleClamp};
    return std::nullopt;
}
[[nodiscard]] constexpr bool isColorizeIntegerSchemaKey(const std::string_view schemaKey) noexcept {
    return colorizeIntegerDomain(schemaKey).has_value();
}

} // namespace bloom::document
