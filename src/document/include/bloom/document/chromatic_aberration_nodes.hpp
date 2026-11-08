#pragma once

// #486 Chromatic Aberration: the node's parameter vocabulary. Red and blue are read from positions
// that move apart from each other, in opposite directions along the line from the centre, by an
// amount that grows with the distance from the centre; green stays where it is.
//
//   amount  the offset red and blue are read at, as a fraction of half the composition's diagonal,
//           reached at a distance of half the diagonal from the centre (a centred corner): red is
//           read further out, blue the same distance further in, and a negative value swaps them.
//           0 leaves the picture as it is. Animatable.
//   center  the point the colours spread from, an offset in pixels from the composition's centre
//           with y down. Animatable.
//
// docs/architecture/image-filters.md, "Chromatic Aberration", is the specification.
//
// Schema keys are "bloom.chromatic-aberration.<role>". Both are animatable, drivable and have a
// socket of their own.

#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kChromaticAberrationNodeType = "bloom.chromatic-aberration";
inline constexpr unsigned kChromaticAberrationNodeSchemaVersion = 1;

inline constexpr std::string_view kChromaticAberrationAmountSchemaKey =
    "bloom.chromatic-aberration.amount";
inline constexpr std::string_view kChromaticAberrationCenterSchemaKey =
    "bloom.chromatic-aberration.center";

// The document domain of the amount, and the span its slider covers.
inline constexpr double kChromaticAberrationAmountDefault = 0.0;
inline constexpr double kMinimumChromaticAberrationAmount = -0.2;
inline constexpr double kMaximumChromaticAberrationAmount = 0.2;
inline constexpr double kChromaticAberrationAmountSoftMinimum = -0.05;
inline constexpr double kChromaticAberrationAmountSoftMaximum = 0.05;
// The centre's reach, in pixels from the frame's centre.
inline constexpr double kMaximumChromaticAberrationCenter = 1.0e6;

[[nodiscard]] constexpr bool
isChromaticAberrationScalarSchemaKey(const std::string_view key) noexcept {
    return key == kChromaticAberrationAmountSchemaKey;
}
// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool
isChromaticAberrationScalarWithinDomain(const std::string_view key, const double value) noexcept {
    return key != kChromaticAberrationAmountSchemaKey ||
           (value >= kMinimumChromaticAberrationAmount &&
            value <= kMaximumChromaticAberrationAmount);
}

} // namespace bloom::document
