#pragma once

// #568 Rolling Shutter: the durable vocabulary of the node that skews and wobbles a picture the way
// a sensor that reads its rows one after another does. An Image in; the Image out.
//
// A rolling shutter reads the frame over a time, not at an instant, so each row of the picture shows
// the scene a little later than the row before it. Whatever moves while the sensor reads is skewed
// (a pan leans, a fast shake wobbles). Row phase s runs from -1/2 at the first row read to +1/2 at
// the last, and the row read at phase s shows the scene s * readout frames after the frame's time.
//
//   readout    how long the sensor takes to read the frame, as a fraction of a frame (0 is a global
//              shutter and leaves the picture alone). Constant.
//   direction  the order the rows are read: top to bottom, bottom to top, left to right (columns)
//              or right to left. Constant.
//   edges      what a row reads past the picture's edge: the edge pixel (Extend) or transparent.
//              Constant.
//   amount     scales every motion below; 1 is the motion as it is. Animatable.
//   cameraOffset, cameraRoll
//              the Offset and Roll outputs of a Camera Shake, connected exactly. The node evaluates
//              that shake at three times across the readout and so follows its true motion
//              (translation and roll), not an estimate from the frame's own change.
//   motion     an Image input: a forward optical flow in pixels a frame (Optical Flow's forward
//              output), read at each pixel for the skew of whatever moves in the footage.
//
// docs/architecture/image-filters.md, "Rolling Shutter", is the specification.
//
// Schema keys are "bloom.rolling-shutter.<role>". The amount, the camera offset and the camera roll
// are animatable, drivable and have a socket of their own.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kRollingShutterNodeType = "bloom.rolling-shutter";
inline constexpr std::uint32_t kRollingShutterNodeSchemaVersion = 1;

inline constexpr std::string_view kRollingShutterMotionPort = "motion";
inline constexpr std::string_view kRollingShutterCameraOffsetRole = "cameraOffset";
inline constexpr std::string_view kRollingShutterCameraRollRole = "cameraRoll";

inline constexpr std::int64_t kRollingShutterTopToBottom = 0;
inline constexpr std::int64_t kRollingShutterBottomToTop = 1;
inline constexpr std::int64_t kRollingShutterLeftToRight = 2;
inline constexpr std::int64_t kRollingShutterRightToLeft = 3;
inline constexpr std::int64_t kRollingShutterEdgeExtend = 0;
inline constexpr std::int64_t kRollingShutterEdgeTransparent = 1;

// The camera roll and offset reach a value can be typed to (a connected Camera Shake is exact).
inline constexpr double kMaximumRollingShutterCameraReach = 1.0e6;

enum class RollingShutterParameterKind : std::uint8_t { Choice, Scalar };

struct RollingShutterParameterSpec final {
    std::string_view role;
    RollingShutterParameterKind kind = RollingShutterParameterKind::Scalar;
    double value = 0.0;
    // The document domain.
    double minimum = 0.0;
    double maximum = 0.0;
    std::span<const std::string_view> choices{};
    // The control's label and presentation: the span its slider covers, one displayed step, the
    // decimals shown, and a unit and display scale (none here: the card's plain number cells show the
    // stored number, so a percentage would read differently there and in Properties).
    std::string_view label{};
    double softMinimum = 0.0;
    double softMaximum = 1.0;
    double step = 0.01;
    int decimals = 0;
    std::string_view unit{};
    double displayScale = 1.0;
    // Constant: no socket and no animation.
    bool constant = false;
};

namespace rolling_shutter_parameters {

using Kind = RollingShutterParameterKind;

inline constexpr std::array<std::string_view, 4> kDirections{"Top to Bottom", "Bottom to Top",
                                                             "Left to Right", "Right to Left"};
inline constexpr std::array<std::string_view, 2> kEdges{"Extend", "Transparent"};

inline constexpr std::array kParameters{
    RollingShutterParameterSpec{"readout", Kind::Scalar, 0.5, 0.0, 1.0, {}, "Readout", 0.0, 1.0,
                                0.01, 2, {}, 1.0, true},
    RollingShutterParameterSpec{"direction", Kind::Choice, 0.0, 0.0, 3.0, kDirections, "Direction",
                                0.0, 1.0, 0.01, 0, {}, 1.0, true},
    RollingShutterParameterSpec{"edges", Kind::Choice, 0.0, 0.0, 1.0, kEdges, "Edges", 0.0, 1.0,
                                0.01, 0, {}, 1.0, true},
    RollingShutterParameterSpec{"amount", Kind::Scalar, 1.0, 0.0, 4.0, {}, "Amount", 0.0, 2.0, 0.01,
                                2, {}, 1.0, false},
};

} // namespace rolling_shutter_parameters

[[nodiscard]] constexpr std::span<const RollingShutterParameterSpec>
rollingShutterParameters() noexcept {
    return rolling_shutter_parameters::kParameters;
}

inline constexpr std::string_view kRollingShutterCameraOffsetSchemaKey =
    "bloom.rolling-shutter.cameraOffset";
inline constexpr std::string_view kRollingShutterCameraRollSchemaKey =
    "bloom.rolling-shutter.cameraRoll";

// The spec of a "bloom.rolling-shutter.<role>" schema key, or null (the camera offset and roll have
// none: they are connections).
[[nodiscard]] constexpr const RollingShutterParameterSpec*
rollingShutterParameterSpec(const std::string_view schemaKey) noexcept {
    constexpr std::string_view prefix = "bloom.rolling-shutter.";
    if (!schemaKey.starts_with(prefix))
        return nullptr;
    const auto role = schemaKey.substr(prefix.size());
    for (const auto& spec : rolling_shutter_parameters::kParameters)
        if (spec.role == role)
            return &spec;
    return nullptr;
}

[[nodiscard]] constexpr bool isRollingShutterScalarSchemaKey(const std::string_view key) noexcept {
    const auto* spec = rollingShutterParameterSpec(key);
    return (spec != nullptr && spec->kind == RollingShutterParameterKind::Scalar && !spec->constant) ||
           key == kRollingShutterCameraRollSchemaKey;
}
// A constant (the readout, direction and edges): no socket and no animation.
[[nodiscard]] constexpr bool
isRollingShutterConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = rollingShutterParameterSpec(key);
    return spec != nullptr && spec->constant;
}
// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool
isRollingShutterScalarWithinDomain(const std::string_view key, const double value) noexcept {
    if (key == kRollingShutterCameraRollSchemaKey)
        return value >= -kMaximumRollingShutterCameraReach &&
               value <= kMaximumRollingShutterCameraReach;
    const auto* spec = rollingShutterParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document
