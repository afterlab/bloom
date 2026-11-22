#pragma once

// #757 Text Source styling: inline markup, accent colors, outline and background box, and (#758)
// fixed-width digits. The thirteen parameters follow the twelve layout parameters of the Text Source definition (schema 2): an
// unbound role lowers at its default, so a node saved before they existed draws exactly as it did
// (docs/architecture/project-format.md, "Node Version Validation"). Schema keys are
// "bloom.text.<role>". The table is the one description the definition, validation, presentation
// and automation read. docs/architecture/evaluation-primitives.md, "Styled Text Source".
//
// The table's order is the order Properties, the node card and the timeline show the rows in,
// grouped by function: Markup, the accents, the outline, the background, then Fixed-width digits.
// The animatable parameters
// carry an operand socket (they are drivable); Markup, Outline join and Background are constants
// with none, so the definition check matches each socket to its parameter by role and kind rather
// than reading the socketed parameters as a prefix.

#include <bloom/core/color.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

// Outline join.
inline constexpr std::int64_t kTextOutlineJoinRound = 0;
inline constexpr std::int64_t kTextOutlineJoinMiter = 1;
inline constexpr std::int64_t kTextOutlineJoinBevel = 2;
// Background box.
inline constexpr std::int64_t kTextBackgroundOff = 0;
inline constexpr std::int64_t kTextBackgroundPerLine = 1;
inline constexpr std::int64_t kTextBackgroundBlock = 2;

// The number of accent colors a color span may name (<c.accent1> .. <c.accent4>).
inline constexpr std::size_t kTextAccentCount = 4;

enum class TextStylingParameterKind : std::uint8_t { Color, Scalar, Choice, Boolean };

struct TextStylingParameterSpec final {
    std::string_view role;
    TextStylingParameterKind kind = TextStylingParameterKind::Scalar;
    bool animatable = false;
    // Scalar: the default and the document domain. Choice: the default index. Boolean: 0 or 1.
    double value = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    std::span<const std::string_view> choices{};
    std::string_view label{};
    std::string_view unit{};
    double softMinimum = 0.0;
    double softMaximum = 1.0;
    double step = 1.0;
    int decimals = 0;
    // Color: the default, straight linear RGBA.
    core::Color4d color{};
};

namespace text_styling_parameters {

using Kind = TextStylingParameterKind;

inline constexpr std::array<std::string_view, 3> kJoins{"Round", "Miter", "Bevel"};
inline constexpr std::array<std::string_view, 3> kBackgrounds{"Off", "Per line", "Block"};

inline constexpr std::array kParameters{
    TextStylingParameterSpec{
        .role = "markup", .kind = Kind::Boolean, .value = 0.0, .maximum = 1.0, .label = "Markup"},
    TextStylingParameterSpec{.role = "accent1",
                             .kind = Kind::Color,
                             .animatable = true,
                             .label = "Accent 1",
                             .color = {1.0, 0.55, 0.0, 1.0}},
    TextStylingParameterSpec{.role = "accent2",
                             .kind = Kind::Color,
                             .animatable = true,
                             .label = "Accent 2",
                             .color = {0.0, 0.55, 1.0, 1.0}},
    TextStylingParameterSpec{.role = "accent3",
                             .kind = Kind::Color,
                             .animatable = true,
                             .label = "Accent 3",
                             .color = {1.0, 0.08, 0.35, 1.0}},
    TextStylingParameterSpec{.role = "accent4",
                             .kind = Kind::Color,
                             .animatable = true,
                             .label = "Accent 4",
                             .color = {0.15, 0.85, 0.25, 1.0}},
    TextStylingParameterSpec{.role = "outlineColor",
                             .kind = Kind::Color,
                             .animatable = true,
                             .label = "Outline color",
                             .color = {0.0, 0.0, 0.0, 1.0}},
    TextStylingParameterSpec{.role = "outlineWidth",
                             .kind = Kind::Scalar,
                             .animatable = true,
                             .value = 0.0,
                             .minimum = 0.0,
                             .maximum = 256.0,
                             .label = "Outline width",
                             .unit = "px",
                             .softMinimum = 0.0,
                             .softMaximum = 16.0,
                             .step = 0.25,
                             .decimals = 2},
    TextStylingParameterSpec{.role = "outlineJoin",
                             .kind = Kind::Choice,
                             .value = 0.0,
                             .maximum = 2.0,
                             .choices = kJoins,
                             .label = "Outline join"},
    TextStylingParameterSpec{.role = "background",
                             .kind = Kind::Choice,
                             .value = 0.0,
                             .maximum = 2.0,
                             .choices = kBackgrounds,
                             .label = "Background"},
    TextStylingParameterSpec{.role = "backgroundColor",
                             .kind = Kind::Color,
                             .animatable = true,
                             .label = "Background color",
                             .color = {0.0, 0.0, 0.0, 0.6}},
    TextStylingParameterSpec{.role = "backgroundPadding",
                             .kind = Kind::Scalar,
                             .animatable = true,
                             .value = 0.25,
                             .minimum = 0.0,
                             .maximum = 4.0,
                             .label = "Background padding",
                             .unit = "em",
                             .softMinimum = 0.0,
                             .softMaximum = 1.0,
                             .step = 0.05,
                             .decimals = 2},
    TextStylingParameterSpec{.role = "backgroundRadius",
                             .kind = Kind::Scalar,
                             .animatable = true,
                             .value = 0.0,
                             .minimum = 0.0,
                             .maximum = 4096.0,
                             .label = "Background corner radius",
                             .unit = "px",
                             .softMinimum = 0.0,
                             .softMaximum = 64.0,
                             .step = 0.5,
                             .decimals = 1},
    // #758: every digit takes the widest digit advance of the face, so a counter does not jitter.
    TextStylingParameterSpec{.role = "fixedDigits",
                             .kind = Kind::Boolean,
                             .value = 0.0,
                             .maximum = 1.0,
                             .label = "Fixed-width digits"},
};

// The number of socketed (animatable) parameters.
inline constexpr std::size_t kSocketedCount = 9;

} // namespace text_styling_parameters

[[nodiscard]] constexpr std::span<const TextStylingParameterSpec> textStylingParameters() noexcept {
    return text_styling_parameters::kParameters;
}

[[nodiscard]] constexpr const TextStylingParameterSpec*
textStylingParameterSpecForRole(const std::string_view role) noexcept {
    for (const auto& spec : text_styling_parameters::kParameters)
        if (spec.role == role)
            return &spec;
    return nullptr;
}

// The spec of a "bloom.text.<role>" styling schema key, or null (also for the layout keys).
[[nodiscard]] constexpr const TextStylingParameterSpec*
textStylingParameterSpec(const std::string_view schemaKey) noexcept {
    constexpr std::string_view prefix = "bloom.text.";
    if (!schemaKey.starts_with(prefix))
        return nullptr;
    return textStylingParameterSpecForRole(schemaKey.substr(prefix.size()));
}

[[nodiscard]] constexpr bool isTextStylingSchemaKey(const std::string_view key) noexcept {
    return textStylingParameterSpec(key) != nullptr;
}

// Whether the parameter is a constant only (no key, no driver).
[[nodiscard]] constexpr bool isTextStylingConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = textStylingParameterSpec(key);
    return spec != nullptr && !spec->animatable;
}

[[nodiscard]] constexpr bool
isTextStylingAnimatableSchemaKey(const std::string_view key,
                                 const TextStylingParameterKind kind) noexcept {
    const auto* spec = textStylingParameterSpec(key);
    return spec != nullptr && spec->animatable && spec->kind == kind;
}

// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isTextStylingScalarWithinDomain(const std::string_view key,
                                                             const double value) noexcept {
    const auto* spec = textStylingParameterSpec(key);
    return spec == nullptr || spec->kind != TextStylingParameterKind::Scalar ||
           (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document
