#pragma once

// #514 Denoise & Smooth: the durable vocabulary of the node. An Image in and out, and the optional
// effect mask every filter takes.
//
//   mode      Median (Dust & Scratches), Bilateral (Smart), Spatial (non-local means) or Temporal
//             (similarity-weighted temporal averaging with motion rejection).
//   radius    the neighborhood searched, in full-resolution pixels, [0, 6]. Temporal ignores it.
//   strength  Median: the largest difference from the neighborhood's median a value keeps, so 0 is
//             the plain median and a larger value removes only outliers (dust, scratches).
//             Bilateral and Spatial: how different a neighbor may be and still contribute (the noise
//             level). Temporal: how different the same place in an earlier frame may be and still
//             contribute, so static noise averages and moving content is rejected. In linear scene
//             values, [0, 10].
//   frames    Temporal only: how many frames, the current one and those before it, are weighed,
//             [2, 16].
//   autoStrength  measure the noise in the sample region of the input on every frame and set the
//             strength from it (Image Statistics' deviation of the stored green channel times the
//             mode's factor below), in place of the authored strength.
//   sampleFrom, sampleTo  Auto strength only: two opposite corners of the sample region, in
//             full-resolution pixels from the composition's centre, +y up (the field convention);
//             default a 64 x 64 square at the centre. Pick a flat, static area of the plate.
//
// docs/architecture/image-filters.md, "Denoise & Smooth", is the specification. Schema keys are
// "bloom.denoise.<role>". Radius and strength are animatable, drivable and have sockets of their
// own; mode, frames, the Auto strength switch and the sample region change the node's structure (or
// the measure's plan) and are constants.

#include <bloom/document/parameter.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kDenoiseNodeType = "bloom.denoise";
inline constexpr std::uint32_t kDenoiseNodeSchemaVersion = 1;

inline constexpr std::string_view kDenoiseModeSchemaKey = "bloom.denoise.mode";
inline constexpr std::string_view kDenoiseRadiusSchemaKey = "bloom.denoise.radius";
inline constexpr std::string_view kDenoiseStrengthSchemaKey = "bloom.denoise.strength";
inline constexpr std::string_view kDenoiseFramesSchemaKey = "bloom.denoise.frames";
inline constexpr std::string_view kDenoiseAutoStrengthSchemaKey = "bloom.denoise.auto-strength";
inline constexpr std::string_view kDenoiseSampleFromSchemaKey = "bloom.denoise.sample-from";
inline constexpr std::string_view kDenoiseSampleToSchemaKey = "bloom.denoise.sample-to";

enum class DenoiseNodeMode : std::int64_t { Median = 0, Bilateral = 1, Spatial = 2, Temporal = 3 };
inline constexpr std::int64_t kDenoiseModeCount = 4;
inline constexpr double kMaximumDenoiseRadiusPixels = 6.0;
inline constexpr double kMaximumDenoiseStrengthValue = 10.0;
inline constexpr std::int64_t kMinimumDenoiseFrames = 2;
inline constexpr std::int64_t kMaximumDenoiseFrames = 16;
inline constexpr std::int64_t kDefaultDenoiseFrames = 5;
inline constexpr double kDefaultDenoiseRadius = 2.0;
inline constexpr double kDefaultDenoiseStrength = 0.1;
// The default sample region: a 64 x 64 square at the composition's centre.
inline constexpr double kDefaultDenoiseSampleHalfSize = 32.0;
// The default region's corners: its top left and bottom right in field units (+y up).
inline constexpr Vec2d kDefaultDenoiseSampleFrom{-kDefaultDenoiseSampleHalfSize,
                                                 kDefaultDenoiseSampleHalfSize};
inline constexpr Vec2d kDefaultDenoiseSampleTo{kDefaultDenoiseSampleHalfSize,
                                               -kDefaultDenoiseSampleHalfSize};
// The farthest a sample corner may sit from the centre, in full-resolution pixels.
inline constexpr double kMaximumDenoiseSampleOffset = 16384.0;

// Auto strength: the measured noise (the standard deviation of one stored channel over the sample
// region) times the mode's factor is the strength. Each factor makes the distance two noisy
// versions of the same flat value have on average a quarter of strength^2, so pure noise keeps
// about (1 - 1/4)^2 = 0.56 of its weight and a difference of a few deviations is rejected:
//   Median    3: a value further than three deviations from its neighborhood's median is an
//             outlier;
//   Bilateral 2 sqrt(6): the four-channel squared distance of two noisy pixels is 2 * 3 sigma^2
//             on average (alpha carries no noise), so strength^2 = 24 sigma^2;
//   Spatial and Temporal sqrt(6): the mean over a 3 x 3 patch's 36 values is 2 * 3 sigma^2 / 4 =
//             1.5 sigma^2, so strength^2 = 6 sigma^2.
// The measured strength is limited to the strength's domain, [0, 10].
[[nodiscard]] constexpr double denoiseAutoStrengthFactor(const DenoiseNodeMode mode) noexcept {
    switch (mode) {
    case DenoiseNodeMode::Median:
        return 3.0;
    case DenoiseNodeMode::Bilateral:
        return 4.898979485566356; // 2 sqrt(6)
    case DenoiseNodeMode::Spatial:
    case DenoiseNodeMode::Temporal:
        return 2.449489742783178; // sqrt(6)
    }
    return 0.0;
}

[[nodiscard]] constexpr bool isDenoiseScalarWithinDomain(const std::string_view key,
                                                         const double value) noexcept {
    if (key == kDenoiseRadiusSchemaKey)
        return value >= 0.0 && value <= kMaximumDenoiseRadiusPixels;
    if (key == kDenoiseStrengthSchemaKey)
        return value >= 0.0 && value <= kMaximumDenoiseStrengthValue;
    if (key == kDenoiseSampleFromSchemaKey || key == kDenoiseSampleToSchemaKey)
        return value >= -kMaximumDenoiseSampleOffset && value <= kMaximumDenoiseSampleOffset;
    return true;
}

} // namespace bloom::document
