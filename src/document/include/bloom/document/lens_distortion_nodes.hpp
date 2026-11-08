#pragma once

// #486 Lens Distortion: the durable vocabulary of the node that undistorts or redistorts an image
// with the Brown-Conrady lens model. An Image in; the warped Image and the ST map of the same warp
// out.
//
//   mode    Undistort or Redistort, inverses of each other. Constant.
//   k1, k2  the radial coefficients: with r the distance from the centre in normalised units, a
//           point moves to a radius of r (1 + k1 r^2 + k2 r^4). Negative values are barrel,
//           positive pincushion. Animatable.
//   p1, p2  the tangential (decentring) coefficients. Animatable.
//   center  the lens centre, an offset in pixels from the composition's centre with y down.
//           Animatable.
//
// Coordinates are normalised by half the composition's diagonal, so a corner of the frame is at
// r = 1 for a centred lens. docs/architecture/image-filters.md, "Lens Distortion", is the
// specification.
//
// Schema keys are "bloom.lens-distortion.<role>". Every Scalar and the centre are animatable,
// drivable and have a socket of their own; the mode is a constant with no socket.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kLensDistortionNodeType = "bloom.lens-distortion";
inline constexpr std::uint32_t kLensDistortionNodeSchemaVersion = 1;

// The warped image, and the same warp as an ST map: the red and green channels hold the position
// each pixel reads, normalised over the composition frame (0, 0 the bottom left, 1, 1 the top
// right), blue 0 and alpha 1.
inline constexpr std::string_view kLensDistortionImagePort = "image";
inline constexpr std::string_view kLensDistortionStMapPort = "stMap";

inline constexpr std::int64_t kLensUndistort = 0;
inline constexpr std::int64_t kLensRedistort = 1;

// The centre's reach, in pixels from the frame's centre.
inline constexpr double kMaximumLensDistortionCenter = 1.0e6;

enum class LensDistortionParameterKind : std::uint8_t { Choice, Scalar };

struct LensDistortionParameterSpec final {
    std::string_view role;
    LensDistortionParameterKind kind = LensDistortionParameterKind::Scalar;
    double value = 0.0;
    // The document domain.
    double minimum = 0.0;
    double maximum = 0.0;
    std::span<const std::string_view> choices{};
    // The control's label and presentation: the span its slider covers, one displayed step and the
    // decimals shown.
    std::string_view label{};
    double softMinimum = 0.0;
    double softMaximum = 1.0;
    double step = 0.001;
    int decimals = 3;
};

namespace lens_distortion_parameters {

using Kind = LensDistortionParameterKind;

inline constexpr std::array<std::string_view, 2> kModes{"Undistort", "Redistort"};

inline constexpr std::array kParameters{
    LensDistortionParameterSpec{"mode", Kind::Choice, 0.0, 0.0, 1.0, kModes, "Mode"},
    LensDistortionParameterSpec{"k1", Kind::Scalar, 0.0, -2.0, 2.0, {}, "Radial k1", -1.0, 1.0, 0.001, 3},
    LensDistortionParameterSpec{"k2", Kind::Scalar, 0.0, -2.0, 2.0, {}, "Radial k2", -1.0, 1.0, 0.001, 3},
    LensDistortionParameterSpec{"p1", Kind::Scalar, 0.0, -0.5, 0.5, {}, "Tangential p1", -0.1, 0.1, 0.001, 3},
    LensDistortionParameterSpec{"p2", Kind::Scalar, 0.0, -0.5, 0.5, {}, "Tangential p2", -0.1, 0.1, 0.001, 3},
};

} // namespace lens_distortion_parameters

[[nodiscard]] constexpr std::span<const LensDistortionParameterSpec>
lensDistortionParameters() noexcept {
    return lens_distortion_parameters::kParameters;
}

inline constexpr std::string_view kLensDistortionCenterSchemaKey = "bloom.lens-distortion.center";

// The spec of a "bloom.lens-distortion.<role>" schema key, or null (the centre has none: it is a
// position).
[[nodiscard]] constexpr const LensDistortionParameterSpec*
lensDistortionParameterSpec(const std::string_view schemaKey) noexcept {
    constexpr std::string_view prefix = "bloom.lens-distortion.";
    if (!schemaKey.starts_with(prefix))
        return nullptr;
    const auto role = schemaKey.substr(prefix.size());
    for (const auto& spec : lens_distortion_parameters::kParameters)
        if (spec.role == role)
            return &spec;
    return nullptr;
}

[[nodiscard]] constexpr bool isLensDistortionScalarSchemaKey(const std::string_view key) noexcept {
    const auto* spec = lensDistortionParameterSpec(key);
    return spec != nullptr && spec->kind == LensDistortionParameterKind::Scalar;
}
// A constant (the mode): no socket and no animation.
[[nodiscard]] constexpr bool isLensDistortionConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = lensDistortionParameterSpec(key);
    return spec != nullptr && spec->kind == LensDistortionParameterKind::Choice;
}
// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isLensDistortionScalarWithinDomain(const std::string_view key,
                                                                const double value) noexcept {
    const auto* spec = lensDistortionParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document
