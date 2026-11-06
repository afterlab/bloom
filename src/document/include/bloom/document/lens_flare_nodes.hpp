#pragma once

// #507 Lens Flare: the durable vocabulary of the one Compositing node that adds an element-stack
// flare to an image. An Image on `input`, an optional matte on `occlusion` (a Mask, or an Image read
// through the occlusion channel selector) and an Image on `image`.
//
// The light sits at `position`, an offset in full-resolution pixels from the centre of the input's
// display window. In the 3D mode `position` and `lightDistance` are a point in front of a camera of
// `cameraZoom` pixels, projected as position * zoom / (zoom + distance); behind the camera there is
// no flare. A tracked point, or a 3D light projected elsewhere, drives `position` through its socket.
//
// The stack, each element off at amount 0:
//   glow        a soft bloom around the light;
//   streaks     needles through the light, `streakCount` lines of two rays;
//   ghosts      soft discs on the axis through the light and the frame's centre;
//   rings       a thin ring around the light;
//   iris        polygon ghosts on the same axis, the aperture's shape;
//   hoop        a wide arc around the frame's centre, on the side away from the light;
//   anamorphic  one long thin streak in its own colour.
// `size`, `rotation`, `chromatic`, `intensity` and `color` act on the whole stack. Sizes are percent
// of the frame's height, so a flare looks the same at any resolution.
//
// `sourceBrightness` scales the flare by how bright the input is at the light (at `sourceWhite` and
// above: full brightness), and the `occlusion` matte hides it by how much of the matte covers the
// light. Both read a disc of `sampleRadius` pixels around the light, so a light passing behind an
// edge fades over the disc. docs/architecture/image-filters.md, "Lens Flare", is the specification.
//
// Schema keys are "bloom.lens-flare.<role>". Every Scalar, Vector 2 and Colour is animatable,
// drivable and has a socket of its own; every Choice and Integer is a constant with no socket.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kLensFlareNodeType = "bloom.lens-flare";
inline constexpr std::uint32_t kLensFlareNodeSchemaVersion = 1;
inline constexpr std::string_view kLensFlareInputPort = "input";
inline constexpr std::string_view kLensFlareOcclusionPort = "occlusion";
inline constexpr std::string_view kLensFlareOutputPort = "image";

// The light's position and distance are finite and within +-1e6 pixels; the sample disc follows the
// blur radius bound of every neighbourhood filter.
inline constexpr double kMaximumLensFlarePosition = 1.0e6;
inline constexpr double kMaximumLensFlareSampleRadius = 256.0;
// The element counts the fixed program's table holds.
inline constexpr std::int64_t kMaximumLensFlareStreaks = 8;
inline constexpr std::int64_t kMaximumLensFlareGhosts = 16;
inline constexpr std::int64_t kMaximumLensFlareIrises = 12;
inline constexpr std::int64_t kMinimumLensFlareBlades = 3;
inline constexpr std::int64_t kMaximumLensFlareBlades = 12;

inline constexpr std::int64_t kLensFlareLight2D = 0;
inline constexpr std::int64_t kLensFlareLight3D = 1;

enum class LensFlareParameterKind : std::uint8_t { Choice, Integer, Scalar, Vector2, Color };

// Where a parameter belongs on the node's card: the light and the whole stack, or one element.
enum class LensFlareGroup : std::uint8_t {
    Light,
    Glow,
    Streaks,
    Ghosts,
    Rings,
    Iris,
    Hoop,
    Anamorphic,
};
inline constexpr std::size_t kLensFlareGroupCount = 8;

struct LensFlareParameterSpec final {
    std::string_view role;
    LensFlareParameterKind kind = LensFlareParameterKind::Scalar;
    LensFlareGroup group = LensFlareGroup::Light;
    // The default: a Scalar's, Integer's or Choice's value in [0], a Vector 2's in [0] and [1], a
    // Colour's straight RGBA.
    std::array<double, 4> value{};
    // The document domain of a Scalar, a Vector 2 component or an Integer.
    double minimum = 0.0;
    double maximum = 0.0;
    std::span<const std::string_view> choices{};
    // The control's label and presentation: the span its slider covers, one step, the decimals
    // shown and the unit.
    std::string_view label{};
    double softMinimum = 0.0;
    double softMaximum = 1.0;
    double step = 0.01;
    int decimals = 2;
    std::string_view unit{};
};

namespace lens_flare_parameters {

using Group = LensFlareGroup;
using Kind = LensFlareParameterKind;

inline constexpr std::array<std::string_view, 2> kLightModes{"2D point", "3D position"};
// The matte selector's stored values (render::MatteSelection), as every matte input's.
inline constexpr std::array<std::string_view, 10> kChannels{
    "Alpha",         "Red",          "Green",          "Blue",          "Luma",
    "Alpha Inverted", "Red Inverted", "Green Inverted", "Blue Inverted", "Luma Inverted"};

inline constexpr LensFlareParameterSpec choice(const std::string_view role, const Group group,
                                               const std::span<const std::string_view> choices,
                                               const double value, const std::string_view label) {
    LensFlareParameterSpec spec;
    spec.role = role;
    spec.kind = Kind::Choice;
    spec.group = group;
    spec.value = {value, 0.0, 0.0, 0.0};
    spec.minimum = 0.0;
    spec.maximum = static_cast<double>(choices.size()) - 1.0;
    spec.choices = choices;
    spec.label = label;
    return spec;
}
inline constexpr LensFlareParameterSpec integer(const std::string_view role, const Group group,
                                                const double value, const double minimum,
                                                const double maximum,
                                                const std::string_view label) {
    LensFlareParameterSpec spec;
    spec.role = role;
    spec.kind = Kind::Integer;
    spec.group = group;
    spec.value = {value, 0.0, 0.0, 0.0};
    spec.minimum = minimum;
    spec.maximum = maximum;
    spec.label = label;
    spec.softMinimum = minimum;
    spec.softMaximum = maximum;
    spec.step = 1.0;
    spec.decimals = 0;
    return spec;
}
inline constexpr LensFlareParameterSpec
scalar(const std::string_view role, const Group group, const double value, const double minimum,
       const double maximum, const std::string_view label, const double softMinimum,
       const double softMaximum, const double step = 0.01, const int decimals = 2,
       const std::string_view unit = {}) {
    LensFlareParameterSpec spec;
    spec.role = role;
    spec.kind = Kind::Scalar;
    spec.group = group;
    spec.value = {value, 0.0, 0.0, 0.0};
    spec.minimum = minimum;
    spec.maximum = maximum;
    spec.label = label;
    spec.softMinimum = softMinimum;
    spec.softMaximum = softMaximum;
    spec.step = step;
    spec.decimals = decimals;
    spec.unit = unit;
    return spec;
}
inline constexpr LensFlareParameterSpec color(const std::string_view role, const Group group,
                                              const std::array<double, 4> value,
                                              const std::string_view label) {
    LensFlareParameterSpec spec;
    spec.role = role;
    spec.kind = Kind::Color;
    spec.group = group;
    spec.value = value;
    spec.minimum = 0.0;
    spec.maximum = 1.0e6;
    spec.label = label;
    return spec;
}
inline constexpr LensFlareParameterSpec position(const std::string_view role, const Group group,
                                                 const double x, const double y,
                                                 const std::string_view label) {
    LensFlareParameterSpec spec;
    spec.role = role;
    spec.kind = Kind::Vector2;
    spec.group = group;
    spec.value = {x, y, 0.0, 0.0};
    spec.minimum = -kMaximumLensFlarePosition;
    spec.maximum = kMaximumLensFlarePosition;
    spec.label = label;
    spec.softMinimum = -2048.0;
    spec.softMaximum = 2048.0;
    spec.step = 1.0;
    spec.decimals = 1;
    spec.unit = "px";
    return spec;
}

inline constexpr std::string_view kPercent = "%";

inline constexpr std::array kParameters{
    position("lightPosition", Group::Light, -360.0, -200.0, "Light position"),
    choice("lightMode", Group::Light, kLightModes, kLensFlareLight2D, "Light is"),
    scalar("lightDistance", Group::Light, 0.0, -kMaximumLensFlarePosition,
           kMaximumLensFlarePosition, "Light distance (Z)", -4000.0, 4000.0, 1.0, 1, "px"),
    scalar("cameraZoom", Group::Light, 2000.0, 1.0, kMaximumLensFlarePosition, "Camera zoom", 100.0,
           8000.0, 1.0, 1, "px"),
    scalar("intensity", Group::Light, 1.0, 0.0, 1000.0, "Intensity", 0.0, 4.0),
    scalar("flareSize", Group::Light, 1.0, 0.01, 100.0, "Size", 0.1, 4.0),
    color("lightColor", Group::Light, {1.0, 0.95, 0.85, 1.0}, "Light color"),
    scalar("flareRotation", Group::Light, 0.0, -1.0e6, 1.0e6, "Rotation", -360.0, 360.0, 1.0, 1,
           "\xc2\xb0"),
    scalar("chromatic", Group::Light, 0.35, 0.0, 1.0, "Chromatic split", 0.0, 1.0),
    scalar("sourceBrightness", Group::Light, 0.0, 0.0, 1.0, "Brightness from source", 0.0, 1.0),
    scalar("sourceWhite", Group::Light, 1.0, 0.001, 1.0e6, "Source white", 0.01, 8.0),
    scalar("occlusionAmount", Group::Light, 1.0, 0.0, 1.0, "Occlusion", 0.0, 1.0),
    choice("occlusionChannel", Group::Light, kChannels, 0.0, "Occlusion channel"),
    scalar("sampleRadius", Group::Light, 8.0, 0.0, kMaximumLensFlareSampleRadius,
           "Light sample radius", 0.0, 64.0, 0.5, 1, "px"),

    scalar("glowAmount", Group::Glow, 1.0, 0.0, 100.0, "Glow amount", 0.0, 2.0),
    scalar("glowSize", Group::Glow, 18.0, 0.0, 1000.0, "Glow size", 0.0, 80.0, 0.1, 1, kPercent),

    scalar("streakAmount", Group::Streaks, 0.5, 0.0, 100.0, "Streak amount", 0.0, 2.0),
    integer("streakCount", Group::Streaks, 3.0, 1.0, kMaximumLensFlareStreaks, "Streak lines"),
    scalar("streakLength", Group::Streaks, 55.0, 0.0, 1000.0, "Streak length", 0.0, 200.0, 0.1, 1,
           kPercent),
    scalar("streakWidth", Group::Streaks, 0.4, 0.0, 100.0, "Streak width", 0.0, 4.0, 0.01, 2,
           kPercent),

    scalar("ghostAmount", Group::Ghosts, 0.3, 0.0, 100.0, "Ghost amount", 0.0, 2.0),
    integer("ghostCount", Group::Ghosts, 6.0, 0.0, kMaximumLensFlareGhosts, "Ghost count"),
    scalar("ghostSize", Group::Ghosts, 5.0, 0.0, 1000.0, "Ghost size", 0.0, 30.0, 0.1, 1, kPercent),
    scalar("ghostSpread", Group::Ghosts, 1.0, 0.0, 10.0, "Ghost spread", 0.0, 2.0),
    integer("seed", Group::Ghosts, 1.0, 0.0, 9999.0, "Ghost pattern"),

    scalar("ringAmount", Group::Rings, 0.12, 0.0, 100.0, "Ring amount", 0.0, 2.0),
    scalar("ringSize", Group::Rings, 30.0, 0.0, 1000.0, "Ring size", 0.0, 100.0, 0.1, 1, kPercent),
    scalar("ringWidth", Group::Rings, 3.0, 0.0, 100.0, "Ring width", 0.0, 20.0, 0.05, 2, kPercent),

    scalar("irisAmount", Group::Iris, 0.2, 0.0, 100.0, "Iris amount", 0.0, 2.0),
    integer("irisCount", Group::Iris, 4.0, 0.0, kMaximumLensFlareIrises, "Iris count"),
    integer("irisBlades", Group::Iris, 6.0, kMinimumLensFlareBlades, kMaximumLensFlareBlades,
            "Iris blades"),
    scalar("irisSize", Group::Iris, 7.0, 0.0, 1000.0, "Iris size", 0.0, 30.0, 0.1, 1, kPercent),
    scalar("irisSpread", Group::Iris, 1.0, 0.0, 10.0, "Iris spread", 0.0, 2.0),

    scalar("hoopAmount", Group::Hoop, 0.12, 0.0, 100.0, "Hoop amount", 0.0, 2.0),
    scalar("hoopSize", Group::Hoop, 70.0, 0.0, 1000.0, "Hoop size", 0.0, 200.0, 0.1, 1, kPercent),
    scalar("hoopWidth", Group::Hoop, 4.0, 0.0, 100.0, "Hoop width", 0.0, 20.0, 0.05, 2, kPercent),

    scalar("anamorphicAmount", Group::Anamorphic, 0.0, 0.0, 100.0, "Anamorphic amount", 0.0, 2.0),
    scalar("anamorphicLength", Group::Anamorphic, 120.0, 0.0, 1000.0, "Anamorphic length", 0.0,
           300.0, 0.1, 1, kPercent),
    scalar("anamorphicWidth", Group::Anamorphic, 0.6, 0.0, 100.0, "Anamorphic width", 0.0, 4.0, 0.01,
           2, kPercent),
    color("anamorphicColor", Group::Anamorphic, {0.35, 0.55, 1.0, 1.0}, "Anamorphic color"),
};

} // namespace lens_flare_parameters

[[nodiscard]] constexpr std::span<const LensFlareParameterSpec> lensFlareParameters() noexcept {
    return lens_flare_parameters::kParameters;
}

// The spec of a "bloom.lens-flare.<role>" schema key, or null.
[[nodiscard]] constexpr const LensFlareParameterSpec*
lensFlareParameterSpec(const std::string_view schemaKey) noexcept {
    constexpr std::string_view prefix = "bloom.lens-flare.";
    if (!schemaKey.starts_with(prefix))
        return nullptr;
    const auto role = schemaKey.substr(prefix.size());
    for (const auto& spec : lens_flare_parameters::kParameters)
        if (spec.role == role)
            return &spec;
    return nullptr;
}

[[nodiscard]] constexpr bool isLensFlareScalarSchemaKey(const std::string_view key) noexcept {
    const auto* spec = lensFlareParameterSpec(key);
    return spec != nullptr && spec->kind == LensFlareParameterKind::Scalar;
}
[[nodiscard]] constexpr bool isLensFlareVec2SchemaKey(const std::string_view key) noexcept {
    const auto* spec = lensFlareParameterSpec(key);
    return spec != nullptr && spec->kind == LensFlareParameterKind::Vector2;
}
[[nodiscard]] constexpr bool isLensFlareColorSchemaKey(const std::string_view key) noexcept {
    const auto* spec = lensFlareParameterSpec(key);
    return spec != nullptr && spec->kind == LensFlareParameterKind::Color;
}
// The Scalars in table order: LensFlareKernel::values holds one per entry.
[[nodiscard]] consteval std::size_t lensFlareScalarCount() noexcept {
    std::size_t count = 0;
    for (const auto& spec : lens_flare_parameters::kParameters)
        count += spec.kind == LensFlareParameterKind::Scalar ? 1U : 0U;
    return count;
}
inline constexpr std::size_t kLensFlareScalarCount = lensFlareScalarCount();
// The index of the Scalar `role` among the Scalars, or kLensFlareScalarCount for another role.
[[nodiscard]] constexpr std::size_t lensFlareScalarIndex(const std::string_view role) noexcept {
    std::size_t index = 0;
    for (const auto& spec : lens_flare_parameters::kParameters) {
        if (spec.kind != LensFlareParameterKind::Scalar)
            continue;
        if (spec.role == role)
            return index;
        ++index;
    }
    return index;
}
// A constant choice (Choice, Integer): no socket and no animation.
[[nodiscard]] constexpr bool isLensFlareConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = lensFlareParameterSpec(key);
    return spec != nullptr && (spec->kind == LensFlareParameterKind::Choice ||
                               spec->kind == LensFlareParameterKind::Integer);
}
// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isLensFlareScalarWithinDomain(const std::string_view key,
                                                           const double value) noexcept {
    const auto* spec = lensFlareParameterSpec(key);
    return spec == nullptr || (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document
