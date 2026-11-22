#pragma once

// #758 Text Animator: the node's durable vocabulary (docs/architecture/evaluation-primitives.md,
// "Text Animator"). A Text Animator reads a Text Source on its `text` input, directly or through
// other Text Animators (its animator chain), and draws that text with per-cluster changes: range
// selectors give each cluster an amount, and the properties are blended per cluster by it; a reveal
// hides the clusters past it without laying the text out again. Its image output is the animated
// text, which a Layer redraws as vectors like any styled text.
//
// Schema keys are "bloom.text-animator.<role>". The table is the one description the definition,
// validation, presentation, activity and automation read. Its order is the order Properties, the
// node card and the timeline show the rows in: four selectors, the properties, the reveal. Every
// value but the selector switches, the color switches, Cursor and the cursor character carries an
// operand socket: the numbers and colors are animatable and drivable, the choices and Randomize
// order are drivable by an Integer or a Boolean.

#include <bloom/core/color.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kTextAnimatorNodeType = "bloom.text-animator";
inline constexpr std::uint32_t kTextAnimatorNodeSchemaVersion = 1;
inline constexpr std::string_view kTextAnimatorTextPort = "text";
inline constexpr std::string_view kTextAnimatorImagePort = "image";

// The number of range selectors one node holds.
inline constexpr std::size_t kTextAnimatorSelectorCount = 4;

// Selector mode: how a selector's amount combines with the selectors before it.
inline constexpr std::int64_t kTextSelectorAdd = 0;
inline constexpr std::int64_t kTextSelectorSubtract = 1;
inline constexpr std::int64_t kTextSelectorIntersect = 2;
// Units a selector or the reveal counts.
inline constexpr std::int64_t kTextUnitCharacters = 0;
inline constexpr std::int64_t kTextUnitCharactersExcludingSpaces = 1;
inline constexpr std::int64_t kTextUnitWords = 2;
inline constexpr std::int64_t kTextUnitLines = 3;
// Range by: start, end and offset in percent of the units, or as unit indices.
inline constexpr std::int64_t kTextRangePercent = 0;
inline constexpr std::int64_t kTextRangeIndex = 1;
// Selector shape.
inline constexpr std::int64_t kTextShapeSquare = 0;
inline constexpr std::int64_t kTextShapeRampUp = 1;
inline constexpr std::int64_t kTextShapeRampDown = 2;
inline constexpr std::int64_t kTextShapeTriangle = 3;
inline constexpr std::int64_t kTextShapeRound = 4;
inline constexpr std::int64_t kTextShapeSmooth = 5;
// Reveal by: a percent of the units, or a count of them.
inline constexpr std::int64_t kTextRevealPercent = 0;
inline constexpr std::int64_t kTextRevealCount = 1;

enum class TextAnimatorParameterKind : std::uint8_t { Boolean, Choice, Scalar, Vec2, Color, Text };

struct TextAnimatorParameterSpec final {
    std::string_view role;
    TextAnimatorParameterKind kind = TextAnimatorParameterKind::Scalar;
    // Keyable (a curve), and drivable through its operand socket.
    bool animatable = false;
    bool socketed = false;
    // Scalar and Vec2 (both components): the default and the document domain. Choice: the default
    // index and the last one. Boolean: 0 or 1.
    double value = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    std::span<const std::string_view> choices{};
    std::string_view label{};
    std::string_view group{};
    std::string_view unit{};
    double softMinimum = 0.0;
    double softMaximum = 1.0;
    double step = 1.0;
    int decimals = 0;
    // A stored factor shown multiplied by this (opacity and scale show as percent).
    double displayScale = 1.0;
    // Color: the default, straight linear RGBA.
    core::Color4d color{};
};

namespace text_animator_parameters {

using Kind = TextAnimatorParameterKind;
using Spec = TextAnimatorParameterSpec;

inline constexpr std::array<std::string_view, 3> kModes{"Add", "Subtract", "Intersect"};
inline constexpr std::array<std::string_view, 4> kUnits{"Characters", "Characters excluding spaces",
                                                        "Words", "Lines"};
inline constexpr std::array<std::string_view, 2> kRangeBy{"Percent", "Index"};
inline constexpr std::array<std::string_view, 6> kShapes{"Square",   "Ramp up", "Ramp down",
                                                         "Triangle", "Round",   "Smooth"};
inline constexpr std::array<std::string_view, 2> kRevealBy{"Percent", "Count"};

// clang-format off
inline constexpr std::array kParameters{
    Spec{.role = "selector1", .kind = Kind::Boolean, .value = 1, .maximum = 1.0, .label = "Selector 1", .group = "Selector 1"},
    Spec{.role = "selector1Mode", .kind = Kind::Choice, .socketed = true, .maximum = 2.0, .choices = kModes, .label = "Mode", .group = "Selector 1"},
    Spec{.role = "selector1Units", .kind = Kind::Choice, .socketed = true, .maximum = 3.0, .choices = kUnits, .label = "Units", .group = "Selector 1"},
    Spec{.role = "selector1RangeBy", .kind = Kind::Choice, .socketed = true, .maximum = 1.0, .choices = kRangeBy, .label = "Range by", .group = "Selector 1"},
    Spec{.role = "selector1Start", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -1.0e6, .maximum = 1.0e6, .label = "Start", .group = "Selector 1", .softMinimum = 0.0, .softMaximum = 100.0, .step = 1.0, .decimals = 2},
    Spec{.role = "selector1End", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 100.0, .minimum = -1.0e6, .maximum = 1.0e6, .label = "End", .group = "Selector 1", .softMinimum = 0.0, .softMaximum = 100.0, .step = 1.0, .decimals = 2},
    Spec{.role = "selector1Offset", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -1.0e6, .maximum = 1.0e6, .label = "Offset", .group = "Selector 1", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 2},
    Spec{.role = "selector1Shape", .kind = Kind::Choice, .socketed = true, .maximum = 5.0, .choices = kShapes, .label = "Shape", .group = "Selector 1"},
    Spec{.role = "selector1Amount", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 100.0, .minimum = -100.0, .maximum = 100.0, .label = "Amount", .group = "Selector 1", .unit = "%", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 1},
    Spec{.role = "selector1EaseHigh", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -100.0, .maximum = 100.0, .label = "Ease high", .group = "Selector 1", .unit = "%", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 1},
    Spec{.role = "selector1EaseLow", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -100.0, .maximum = 100.0, .label = "Ease low", .group = "Selector 1", .unit = "%", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 1},
    Spec{.role = "selector1Randomize", .kind = Kind::Boolean, .socketed = true, .maximum = 1.0, .label = "Randomize order", .group = "Selector 1"},
    Spec{.role = "selector1Seed", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = 0.0, .maximum = 1.0e6, .label = "Seed", .group = "Selector 1", .softMinimum = 0.0, .softMaximum = 100.0, .step = 1.0, .decimals = 0},
    Spec{.role = "selector2", .kind = Kind::Boolean, .value = 0, .maximum = 1.0, .label = "Selector 2", .group = "Selector 2"},
    Spec{.role = "selector2Mode", .kind = Kind::Choice, .socketed = true, .maximum = 2.0, .choices = kModes, .label = "Mode", .group = "Selector 2"},
    Spec{.role = "selector2Units", .kind = Kind::Choice, .socketed = true, .maximum = 3.0, .choices = kUnits, .label = "Units", .group = "Selector 2"},
    Spec{.role = "selector2RangeBy", .kind = Kind::Choice, .socketed = true, .maximum = 1.0, .choices = kRangeBy, .label = "Range by", .group = "Selector 2"},
    Spec{.role = "selector2Start", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -1.0e6, .maximum = 1.0e6, .label = "Start", .group = "Selector 2", .softMinimum = 0.0, .softMaximum = 100.0, .step = 1.0, .decimals = 2},
    Spec{.role = "selector2End", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 100.0, .minimum = -1.0e6, .maximum = 1.0e6, .label = "End", .group = "Selector 2", .softMinimum = 0.0, .softMaximum = 100.0, .step = 1.0, .decimals = 2},
    Spec{.role = "selector2Offset", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -1.0e6, .maximum = 1.0e6, .label = "Offset", .group = "Selector 2", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 2},
    Spec{.role = "selector2Shape", .kind = Kind::Choice, .socketed = true, .maximum = 5.0, .choices = kShapes, .label = "Shape", .group = "Selector 2"},
    Spec{.role = "selector2Amount", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 100.0, .minimum = -100.0, .maximum = 100.0, .label = "Amount", .group = "Selector 2", .unit = "%", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 1},
    Spec{.role = "selector2EaseHigh", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -100.0, .maximum = 100.0, .label = "Ease high", .group = "Selector 2", .unit = "%", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 1},
    Spec{.role = "selector2EaseLow", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -100.0, .maximum = 100.0, .label = "Ease low", .group = "Selector 2", .unit = "%", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 1},
    Spec{.role = "selector2Randomize", .kind = Kind::Boolean, .socketed = true, .maximum = 1.0, .label = "Randomize order", .group = "Selector 2"},
    Spec{.role = "selector2Seed", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = 0.0, .maximum = 1.0e6, .label = "Seed", .group = "Selector 2", .softMinimum = 0.0, .softMaximum = 100.0, .step = 1.0, .decimals = 0},
    Spec{.role = "selector3", .kind = Kind::Boolean, .value = 0, .maximum = 1.0, .label = "Selector 3", .group = "Selector 3"},
    Spec{.role = "selector3Mode", .kind = Kind::Choice, .socketed = true, .maximum = 2.0, .choices = kModes, .label = "Mode", .group = "Selector 3"},
    Spec{.role = "selector3Units", .kind = Kind::Choice, .socketed = true, .maximum = 3.0, .choices = kUnits, .label = "Units", .group = "Selector 3"},
    Spec{.role = "selector3RangeBy", .kind = Kind::Choice, .socketed = true, .maximum = 1.0, .choices = kRangeBy, .label = "Range by", .group = "Selector 3"},
    Spec{.role = "selector3Start", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -1.0e6, .maximum = 1.0e6, .label = "Start", .group = "Selector 3", .softMinimum = 0.0, .softMaximum = 100.0, .step = 1.0, .decimals = 2},
    Spec{.role = "selector3End", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 100.0, .minimum = -1.0e6, .maximum = 1.0e6, .label = "End", .group = "Selector 3", .softMinimum = 0.0, .softMaximum = 100.0, .step = 1.0, .decimals = 2},
    Spec{.role = "selector3Offset", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -1.0e6, .maximum = 1.0e6, .label = "Offset", .group = "Selector 3", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 2},
    Spec{.role = "selector3Shape", .kind = Kind::Choice, .socketed = true, .maximum = 5.0, .choices = kShapes, .label = "Shape", .group = "Selector 3"},
    Spec{.role = "selector3Amount", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 100.0, .minimum = -100.0, .maximum = 100.0, .label = "Amount", .group = "Selector 3", .unit = "%", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 1},
    Spec{.role = "selector3EaseHigh", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -100.0, .maximum = 100.0, .label = "Ease high", .group = "Selector 3", .unit = "%", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 1},
    Spec{.role = "selector3EaseLow", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -100.0, .maximum = 100.0, .label = "Ease low", .group = "Selector 3", .unit = "%", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 1},
    Spec{.role = "selector3Randomize", .kind = Kind::Boolean, .socketed = true, .maximum = 1.0, .label = "Randomize order", .group = "Selector 3"},
    Spec{.role = "selector3Seed", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = 0.0, .maximum = 1.0e6, .label = "Seed", .group = "Selector 3", .softMinimum = 0.0, .softMaximum = 100.0, .step = 1.0, .decimals = 0},
    Spec{.role = "selector4", .kind = Kind::Boolean, .value = 0, .maximum = 1.0, .label = "Selector 4", .group = "Selector 4"},
    Spec{.role = "selector4Mode", .kind = Kind::Choice, .socketed = true, .maximum = 2.0, .choices = kModes, .label = "Mode", .group = "Selector 4"},
    Spec{.role = "selector4Units", .kind = Kind::Choice, .socketed = true, .maximum = 3.0, .choices = kUnits, .label = "Units", .group = "Selector 4"},
    Spec{.role = "selector4RangeBy", .kind = Kind::Choice, .socketed = true, .maximum = 1.0, .choices = kRangeBy, .label = "Range by", .group = "Selector 4"},
    Spec{.role = "selector4Start", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -1.0e6, .maximum = 1.0e6, .label = "Start", .group = "Selector 4", .softMinimum = 0.0, .softMaximum = 100.0, .step = 1.0, .decimals = 2},
    Spec{.role = "selector4End", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 100.0, .minimum = -1.0e6, .maximum = 1.0e6, .label = "End", .group = "Selector 4", .softMinimum = 0.0, .softMaximum = 100.0, .step = 1.0, .decimals = 2},
    Spec{.role = "selector4Offset", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -1.0e6, .maximum = 1.0e6, .label = "Offset", .group = "Selector 4", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 2},
    Spec{.role = "selector4Shape", .kind = Kind::Choice, .socketed = true, .maximum = 5.0, .choices = kShapes, .label = "Shape", .group = "Selector 4"},
    Spec{.role = "selector4Amount", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 100.0, .minimum = -100.0, .maximum = 100.0, .label = "Amount", .group = "Selector 4", .unit = "%", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 1},
    Spec{.role = "selector4EaseHigh", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -100.0, .maximum = 100.0, .label = "Ease high", .group = "Selector 4", .unit = "%", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 1},
    Spec{.role = "selector4EaseLow", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = -100.0, .maximum = 100.0, .label = "Ease low", .group = "Selector 4", .unit = "%", .softMinimum = -100.0, .softMaximum = 100.0, .step = 1.0, .decimals = 1},
    Spec{.role = "selector4Randomize", .kind = Kind::Boolean, .socketed = true, .maximum = 1.0, .label = "Randomize order", .group = "Selector 4"},
    Spec{.role = "selector4Seed", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 0.0, .minimum = 0.0, .maximum = 1.0e6, .label = "Seed", .group = "Selector 4", .softMinimum = 0.0, .softMaximum = 100.0, .step = 1.0, .decimals = 0},
    Spec{.role = "opacity", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 1.0, .minimum = 0.0, .maximum = 1.0, .label = "Opacity", .group = "Properties", .unit = "%", .softMinimum = 0.0, .softMaximum = 1.0, .step = 1.0, .decimals = 1, .displayScale = 100.0},
    Spec{.role = "position", .kind = Kind::Vec2, .animatable = true, .socketed = true, .minimum = -1.0e6, .maximum = 1.0e6, .label = "Position", .group = "Properties", .unit = "px", .softMinimum = -200.0, .softMaximum = 200.0, .step = 1.0, .decimals = 2},
    Spec{.role = "scale", .kind = Kind::Vec2, .animatable = true, .socketed = true, .value = 1.0, .minimum = -1.0e4, .maximum = 1.0e4, .label = "Scale", .group = "Properties", .unit = "%", .softMinimum = 0.0, .softMaximum = 2.0, .step = 1.0, .decimals = 1, .displayScale = 100.0},
    Spec{.role = "rotation", .kind = Kind::Scalar, .animatable = true, .socketed = true, .minimum = -1.0e6, .maximum = 1.0e6, .label = "Rotation", .group = "Properties", .unit = "\xc2\xb0", .softMinimum = -360.0, .softMaximum = 360.0, .step = 1.0, .decimals = 2},
    Spec{.role = "skew", .kind = Kind::Scalar, .animatable = true, .socketed = true, .minimum = -85.0, .maximum = 85.0, .label = "Skew", .group = "Properties", .unit = "\xc2\xb0", .softMinimum = -60.0, .softMaximum = 60.0, .step = 1.0, .decimals = 2},
    Spec{.role = "fill", .kind = Kind::Boolean, .maximum = 1.0, .label = "Change fill color", .group = "Properties"},
    Spec{.role = "fillColor", .kind = Kind::Color, .animatable = true, .socketed = true, .label = "Fill color", .group = "Properties", .color = {1.0, 0.55, 0.0, 1.0}},
    Spec{.role = "outline", .kind = Kind::Boolean, .maximum = 1.0, .label = "Change outline color", .group = "Properties"},
    Spec{.role = "outlineColor", .kind = Kind::Color, .animatable = true, .socketed = true, .label = "Outline color", .group = "Properties", .color = {0.0, 0.0, 0.0, 1.0}},
    Spec{.role = "outlineWidth", .kind = Kind::Scalar, .animatable = true, .socketed = true, .minimum = -256.0, .maximum = 256.0, .label = "Outline width", .group = "Properties", .unit = "px", .softMinimum = -8.0, .softMaximum = 8.0, .step = 0.25, .decimals = 2},
    Spec{.role = "tracking", .kind = Kind::Scalar, .animatable = true, .socketed = true, .minimum = -1000.0, .maximum = 1000.0, .label = "Tracking", .group = "Properties", .unit = "px", .softMinimum = -20.0, .softMaximum = 50.0, .step = 0.5, .decimals = 1},
    Spec{.role = "revealBy", .kind = Kind::Choice, .socketed = true, .maximum = 1.0, .choices = kRevealBy, .label = "Reveal by", .group = "Reveal"},
    Spec{.role = "reveal", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 100.0, .minimum = 0.0, .maximum = 1.0e6, .label = "Reveal", .group = "Reveal", .softMinimum = 0.0, .softMaximum = 100.0, .step = 1.0, .decimals = 2},
    Spec{.role = "revealUnits", .kind = Kind::Choice, .socketed = true, .maximum = 3.0, .choices = kUnits, .label = "Reveal units", .group = "Reveal"},
    Spec{.role = "cursor", .kind = Kind::Boolean, .maximum = 1.0, .label = "Cursor", .group = "Reveal"},
    Spec{.role = "cursorCharacter", .kind = Kind::Text, .label = "Cursor character", .group = "Reveal"},
    Spec{.role = "cursorBlink", .kind = Kind::Scalar, .animatable = true, .socketed = true, .value = 2.0, .minimum = 0.0, .maximum = 60.0, .label = "Cursor blinks", .group = "Reveal", .unit = "per second", .softMinimum = 0.0, .softMaximum = 4.0, .step = 0.25, .decimals = 2},
};
// clang-format on

// The parameters each selector holds, in the table's order from `selector<N>`.
inline constexpr std::size_t kSelectorParameterCount = 13;

} // namespace text_animator_parameters

[[nodiscard]] constexpr std::span<const TextAnimatorParameterSpec> textAnimatorParameters() noexcept {
    return text_animator_parameters::kParameters;
}

[[nodiscard]] constexpr const TextAnimatorParameterSpec*
textAnimatorParameterSpecForRole(const std::string_view role) noexcept {
    for (const auto& spec : text_animator_parameters::kParameters)
        if (spec.role == role)
            return &spec;
    return nullptr;
}

// The spec of a "bloom.text-animator.<role>" schema key, or null.
[[nodiscard]] constexpr const TextAnimatorParameterSpec*
textAnimatorParameterSpec(const std::string_view schemaKey) noexcept {
    constexpr std::string_view prefix = "bloom.text-animator.";
    if (!schemaKey.starts_with(prefix))
        return nullptr;
    return textAnimatorParameterSpecForRole(schemaKey.substr(prefix.size()));
}

[[nodiscard]] constexpr bool isTextAnimatorSchemaKey(const std::string_view key) noexcept {
    return textAnimatorParameterSpec(key) != nullptr;
}

// Whether the parameter is a constant only (no key, no driver).
[[nodiscard]] constexpr bool isTextAnimatorConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = textAnimatorParameterSpec(key);
    return spec != nullptr && !spec->socketed;
}

[[nodiscard]] constexpr bool
isTextAnimatorAnimatableSchemaKey(const std::string_view key,
                                  const TextAnimatorParameterKind kind) noexcept {
    const auto* spec = textAnimatorParameterSpec(key);
    return spec != nullptr && spec->animatable && spec->kind == kind;
}

// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isTextAnimatorScalarWithinDomain(const std::string_view key,
                                                              const double value) noexcept {
    const auto* spec = textAnimatorParameterSpec(key);
    return spec == nullptr ||
           (spec->kind != TextAnimatorParameterKind::Scalar &&
            spec->kind != TextAnimatorParameterKind::Vec2) ||
           (value >= spec->minimum && value <= spec->maximum);
}

// The selector (1..4) a role belongs to, or 0 for the properties and the reveal.
[[nodiscard]] constexpr std::size_t textAnimatorSelectorOf(const std::string_view role) noexcept {
    constexpr std::string_view prefix = "selector";
    if (!role.starts_with(prefix) || role.size() <= prefix.size())
        return 0;
    const auto digit = role[prefix.size()];
    return digit >= '1' && digit <= '4' ? static_cast<std::size_t>(digit - '0') : 0;
}

} // namespace bloom::document
