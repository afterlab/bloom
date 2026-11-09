#pragma once

// #514 Denoise & Smooth: the durable vocabulary of the node. An Image in and out, and the optional
// effect mask every filter takes.
//
//   mode      Median (Dust & Scratches), Bilateral (Smart), Spatial (non-local means) or Temporal.
//   radius    the neighborhood searched, in full-resolution pixels, [0, 6]. Temporal ignores it.
//   strength  Median: the largest difference from the neighborhood's median a value keeps, so 0 is
//             the plain median and a larger value removes only outliers (dust, scratches).
//             Bilateral and Spatial: how different a neighbor may be and still contribute (the noise
//             level; measure it with Image Statistics over a flat patch). Temporal: the same
//             tolerance between frames. In linear scene values, [0, 10].
//   frames    Temporal only: how many frames, the current one and those before it, are averaged,
//             [2, 16].
//
// docs/architecture/image-filters.md, "Denoise & Smooth", is the specification. Schema keys are
// "bloom.denoise.<role>". Radius and strength are animatable, drivable and have sockets of their
// own; mode and frames change the node's structure and are constants.

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

enum class DenoiseNodeMode : std::int64_t { Median = 0, Bilateral = 1, Spatial = 2, Temporal = 3 };
inline constexpr std::int64_t kDenoiseModeCount = 4;
inline constexpr double kMaximumDenoiseRadiusPixels = 6.0;
inline constexpr double kMaximumDenoiseStrengthValue = 10.0;
inline constexpr std::int64_t kMinimumDenoiseFrames = 2;
inline constexpr std::int64_t kMaximumDenoiseFrames = 16;
inline constexpr std::int64_t kDefaultDenoiseFrames = 5;
inline constexpr double kDefaultDenoiseRadius = 2.0;
inline constexpr double kDefaultDenoiseStrength = 0.1;

[[nodiscard]] constexpr bool isDenoiseScalarWithinDomain(const std::string_view key,
                                                         const double value) noexcept {
    if (key == kDenoiseRadiusSchemaKey)
        return value >= 0.0 && value <= kMaximumDenoiseRadiusPixels;
    if (key == kDenoiseStrengthSchemaKey)
        return value >= 0.0 && value <= kMaximumDenoiseStrengthValue;
    return true;
}

} // namespace bloom::document
