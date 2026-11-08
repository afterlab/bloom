#pragma once

// #566 Auto Exposure: the durable vocabulary of the node that measures a frame's brightness and
// colour and applies a gain that follows the footage with a lag, the way a phone camera hunts when a
// light dims or flares. An Image in and out, and the optional effect mask every filter takes. One
// node does both corrections because they share one look-back window and one measurement per frame.
//
//   exposure      how much of the exposure correction is applied: 0% leaves the exposure alone, 100%
//                 brings the measured level all the way to the target. Animatable.
//   target        the scene-linear level the measured mean luminance is brought to. Animatable.
//   attack        the time the correction takes to follow the footage getting brighter (the gain
//                 falling), as the time constant of a one-pole lag in seconds; 0 follows at once.
//                 Animatable.
//   release       the same for the footage getting darker (the gain rising). Animatable.
//   minGain       the most the exposure gain may fall, in stops (at most 0). Animatable.
//   maxGain       the most the exposure gain may rise, in stops (at least 0). Animatable.
//   whiteBalance  how much of the white balance correction is applied, 0% to 100%. The correction
//                 makes the mean colour of the frame neutral. Animatable.
//   balanceLag    the time constant of the white balance's lag, in seconds; 0 follows at once.
//                 Animatable.
//   balanceLimit  the most the white balance may move the red or the blue against the green, in
//                 stops. Animatable.
//   lookBack      the frames the node looks back over, counted with the current frame. A frame is
//                 computed from these frames alone, so it is the same in preview, scrubbing and
//                 export, and a lag longer than the look-back is cut short. Constant.
//
// docs/architecture/image-filters.md, "Auto Exposure", is the specification.
//
// Schema keys are "bloom.auto-exposure.<role>". Every Scalar is animatable, drivable and has a socket
// of its own; the look-back is a constant with no socket.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kAutoExposureNodeType = "bloom.auto-exposure";
inline constexpr std::uint32_t kAutoExposureNodeSchemaVersion = 1;

// The look-back, in frames.
inline constexpr std::int64_t kMinimumAutoExposureLookBack = 4;
inline constexpr std::int64_t kMaximumAutoExposureLookBack = 64;
inline constexpr std::int64_t kDefaultAutoExposureLookBack = 48;

enum class AutoExposureParameterKind : std::uint8_t { Scalar, Count };

struct AutoExposureParameterSpec final {
    std::string_view role;
    AutoExposureParameterKind kind = AutoExposureParameterKind::Scalar;
    double value = 0.0;
    // The document domain.
    double minimum = 0.0;
    double maximum = 0.0;
    // The control's label and presentation: the span its slider covers, one displayed step, the
    // decimals shown, the unit and the display scale of a stored value.
    std::string_view label{};
    double softMinimum = 0.0;
    double softMaximum = 1.0;
    double step = 1.0;
    int decimals = 0;
    std::string_view unit{};
    double displayScale = 1.0;
};

namespace auto_exposure_parameters {

using Kind = AutoExposureParameterKind;

inline constexpr std::array kParameters{
    AutoExposureParameterSpec{"exposure", Kind::Scalar, 1.0, 0.0, 1.0, "Exposure", 0.0, 1.0, 1.0, 0,
                              "%", 100.0},
    AutoExposureParameterSpec{"target", Kind::Scalar, 0.18, 0.01, 4.0, "Target", 0.05, 0.5, 0.01, 2},
    AutoExposureParameterSpec{"attack", Kind::Scalar, 0.25, 0.0, 30.0, "Attack", 0.0, 5.0, 0.05, 2,
                              "s"},
    AutoExposureParameterSpec{"release", Kind::Scalar, 0.75, 0.0, 30.0, "Release", 0.0, 5.0, 0.05, 2,
                              "s"},
    AutoExposureParameterSpec{"minGain", Kind::Scalar, -3.0, -8.0, 0.0, "Minimum gain", -8.0, 0.0,
                              0.1, 1, "st"},
    AutoExposureParameterSpec{"maxGain", Kind::Scalar, 3.0, 0.0, 8.0, "Maximum gain", 0.0, 8.0, 0.1,
                              1, "st"},
    AutoExposureParameterSpec{"whiteBalance", Kind::Scalar, 1.0, 0.0, 1.0, "White balance", 0.0, 1.0,
                              1.0, 0, "%", 100.0},
    AutoExposureParameterSpec{"balanceLag", Kind::Scalar, 1.0, 0.0, 30.0, "Balance lag", 0.0, 5.0,
                              0.05, 2, "s"},
    AutoExposureParameterSpec{"balanceLimit", Kind::Scalar, 1.0, 0.0, 4.0, "Balance limit", 0.0, 4.0,
                              0.1, 1, "st"},
    AutoExposureParameterSpec{"lookBack", Kind::Count,
                              static_cast<double>(kDefaultAutoExposureLookBack),
                              static_cast<double>(kMinimumAutoExposureLookBack),
                              static_cast<double>(kMaximumAutoExposureLookBack), "Look-back",
                              static_cast<double>(kMinimumAutoExposureLookBack),
                              static_cast<double>(kMaximumAutoExposureLookBack), 1.0, 0, "f"},
};

} // namespace auto_exposure_parameters

[[nodiscard]] constexpr std::span<const AutoExposureParameterSpec>
autoExposureParameters() noexcept {
    return auto_exposure_parameters::kParameters;
}

[[nodiscard]] constexpr const AutoExposureParameterSpec*
autoExposureParameterSpec(const std::string_view schemaKey) noexcept {
    constexpr std::string_view prefix = "bloom.auto-exposure.";
    if (!schemaKey.starts_with(prefix))
        return nullptr;
    const auto role = schemaKey.substr(prefix.size());
    for (const auto& spec : auto_exposure_parameters::kParameters)
        if (spec.role == role)
            return &spec;
    return nullptr;
}

[[nodiscard]] constexpr bool isAutoExposureScalarSchemaKey(const std::string_view key) noexcept {
    const auto* spec = autoExposureParameterSpec(key);
    return spec != nullptr && spec->kind == AutoExposureParameterKind::Scalar;
}
// A constant (the look-back): no socket and no animation.
[[nodiscard]] constexpr bool isAutoExposureConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = autoExposureParameterSpec(key);
    return spec != nullptr && spec->kind != AutoExposureParameterKind::Scalar;
}
// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isAutoExposureScalarWithinDomain(const std::string_view key,
                                                              const double value) noexcept {
    const auto* spec = autoExposureParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

// The Scalars in table order: AutoExposureKernel::values holds one per entry.
inline constexpr std::size_t kAutoExposureScalarCount = 9;
// The index of the Scalar `role` among the Scalars, or kAutoExposureScalarCount for another role.
[[nodiscard]] constexpr std::size_t autoExposureScalarIndex(const std::string_view role) noexcept {
    std::size_t index = 0;
    for (const auto& spec : auto_exposure_parameters::kParameters) {
        if (spec.kind != AutoExposureParameterKind::Scalar)
            continue;
        if (spec.role == role)
            return index;
        ++index;
    }
    return index;
}

} // namespace bloom::document
