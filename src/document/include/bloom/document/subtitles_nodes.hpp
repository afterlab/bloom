#pragma once

// #752 Subtitles: the node's durable vocabulary. A Subtitles node holds a cue list (cue_list.hpp)
// in its `cues` parameter, or reads one on its `cues` input, and draws the cues active at each
// frame. Its outputs are the picture, the resolved cue list, and the active cues' text, sorted
// index and progress. docs/architecture/subtitles.md is the specification.
//
// Schema keys are "bloom.subtitles.<role>". The animatable parameters are marked so in the table;
// every other one is a constant. The table is the one description the definition, validation,
// presentation and automation read.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::document {

inline constexpr std::string_view kSubtitlesNodeType = "bloom.subtitles";
inline constexpr std::uint32_t kSubtitlesNodeSchemaVersion = 1;

inline constexpr std::string_view kSubtitlesImagePort = "image";
inline constexpr std::string_view kSubtitlesCuesPort = "cues";
inline constexpr std::string_view kSubtitlesTextPort = "text";
inline constexpr std::string_view kSubtitlesIndexPort = "index";
inline constexpr std::string_view kSubtitlesProgressPort = "progress";
inline constexpr std::string_view kSubtitlesCuesRole = "cues";

// Box.
inline constexpr std::int64_t kSubtitlesBoxOff = 0;
inline constexpr std::int64_t kSubtitlesBoxPerLine = 1;
inline constexpr std::int64_t kSubtitlesBoxBlock = 2;
// Safe area.
inline constexpr std::int64_t kSubtitlesSafeTitle = 0;
inline constexpr std::int64_t kSubtitlesSafeAction = 1;
inline constexpr std::int64_t kSubtitlesSafeFullFrame = 2;
inline constexpr std::int64_t kSubtitlesSafeCustom = 3;
// Wrap.
inline constexpr std::int64_t kSubtitlesWrapBalanced = 0;
inline constexpr std::int64_t kSubtitlesWrapGreedy = 1;
inline constexpr std::int64_t kSubtitlesWrapExplicit = 2;
// Overflow.
inline constexpr std::int64_t kSubtitlesOverflowShrink = 0;
inline constexpr std::int64_t kSubtitlesOverflowShowAll = 1;
// Show speaker.
inline constexpr std::int64_t kSubtitlesSpeakerOff = 0;
inline constexpr std::int64_t kSubtitlesSpeakerPrefix = 1;
// Anchor: the nine CueAnchor values, row by row from the top left; bottom center is 7.
inline constexpr std::int64_t kSubtitlesAnchorBottom = 7;

// The default size is this fraction of the composition height when a node is created, rounded
// (59 px at 1080); the outline width height / 360 (3 px at 1080).
inline constexpr double kSubtitlesSizeOfHeight = 0.055;
inline constexpr double kSubtitlesOutlineOfHeight = 1.0 / 360.0;

enum class SubtitlesParameterKind : std::uint8_t {
    Cues,
    Font,
    Scalar,
    Whole,
    Choice,
    Boolean,
    Color,
    Offset,
    Text,
};

struct SubtitlesParameterSpec final {
    std::string_view role;
    SubtitlesParameterKind kind = SubtitlesParameterKind::Scalar;
    bool animatable = false;
    // Scalar and Whole: the default and the document domain. Choice: the default index. Boolean:
    // 0 or 1. Color: unused (subtitlesDefaultColor()).
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
};

namespace subtitles_parameters {

using Kind = SubtitlesParameterKind;

inline constexpr std::array<std::string_view, 3> kBoxes{"Off", "Per line", "Block"};
inline constexpr std::array<std::string_view, 4> kSafeAreas{"Title safe", "Action safe",
                                                            "Full frame", "Custom"};
inline constexpr std::array<std::string_view, 9> kAnchors{
    "Top left", "Top",    "Top right",   "Left",        "Center",
    "Right",    "Bottom left", "Bottom", "Bottom right"};
inline constexpr std::array<std::string_view, 3> kWraps{"Balanced", "Greedy", "Explicit only"};
inline constexpr std::array<std::string_view, 2> kOverflows{"Shrink to fit", "Show all lines"};
inline constexpr std::array<std::string_view, 3> kAlignments{"Left", "Center", "Right"};
inline constexpr std::array<std::string_view, 2> kSpeakers{"Off", "Name prefix"};

inline constexpr std::array kParameters{
    SubtitlesParameterSpec{"cues", Kind::Cues, false, 0, 0, 0, {}, "Cues", "Source"},
    SubtitlesParameterSpec{"font", Kind::Font, false, 0, 0, 0, {}, "Font", "Style"},
    SubtitlesParameterSpec{"size", Kind::Scalar, true, 59.0, 1.0, 4096.0, {}, "Size", "Style",
                           "px", 8.0, 200.0, 1.0, 1},
    SubtitlesParameterSpec{"fill", Kind::Color, true, 0, 0, 0, {}, "Fill", "Style"},
    SubtitlesParameterSpec{"outlineColor", Kind::Color, true, 0, 0, 0, {}, "Outline color",
                           "Style"},
    SubtitlesParameterSpec{"outlineWidth", Kind::Scalar, true, 3.0, 0.0, 256.0, {},
                           "Outline width", "Style", "px", 0.0, 16.0, 0.25, 2},
    SubtitlesParameterSpec{"box", Kind::Choice, false, 0, 0, 2, kBoxes, "Box", "Style"},
    SubtitlesParameterSpec{"boxColor", Kind::Color, true, 0, 0, 0, {}, "Box color", "Style"},
    SubtitlesParameterSpec{"boxPadding", Kind::Scalar, false, 0.25, 0.0, 4.0, {}, "Box padding",
                           "Style", "em", 0.0, 1.0, 0.05, 2},
    SubtitlesParameterSpec{"boxRadius", Kind::Scalar, false, 0.0, 0.0, 4.0, {}, "Corner radius",
                           "Style", "em", 0.0, 1.0, 0.05, 2},
    SubtitlesParameterSpec{"lineHeight", Kind::Scalar, true, 1.15, 0.25, 10.0, {}, "Line height",
                           "Style", "", 0.5, 3.0, 0.05, 2},
    SubtitlesParameterSpec{"letterSpacing", Kind::Scalar, true, 0.0, -1000.0, 1000.0, {},
                           "Letter spacing", "Style", "px", -20.0, 50.0, 0.1, 1},
    SubtitlesParameterSpec{"opacity", Kind::Scalar, true, 1.0, 0.0, 1.0, {}, "Opacity", "Style",
                           "", 0.0, 1.0, 0.01, 2},
    SubtitlesParameterSpec{"safeArea", Kind::Choice, false, 0, 0, 3, kSafeAreas, "Safe area",
                           "Layout"},
    SubtitlesParameterSpec{"marginX", Kind::Scalar, false, 0.1, 0.0, 0.45, {}, "Side margin",
                           "Layout", "of the width", 0.0, 0.45, 0.005, 3},
    SubtitlesParameterSpec{"marginY", Kind::Scalar, false, 0.1, 0.0, 0.45, {}, "Top and bottom margin",
                           "Layout", "of the height", 0.0, 0.45, 0.005, 3},
    SubtitlesParameterSpec{"anchor", Kind::Choice, false, 7, 0, 8, kAnchors, "Anchor", "Layout"},
    SubtitlesParameterSpec{"offset", Kind::Offset, true, 0, -1.0e6, 1.0e6, {}, "Offset", "Layout",
                           "px"},
    SubtitlesParameterSpec{"maxWidth", Kind::Scalar, false, 1.0, 0.05, 1.0, {}, "Max width",
                           "Layout", "of the safe area", 0.05, 1.0, 0.01, 2},
    SubtitlesParameterSpec{"wrap", Kind::Choice, false, 0, 0, 2, kWraps, "Wrap", "Layout"},
    SubtitlesParameterSpec{"maxLines", Kind::Whole, false, 2, 1, 16, {}, "Max lines", "Layout",
                           "", 1, 16},
    SubtitlesParameterSpec{"overflow", Kind::Choice, false, 0, 0, 1, kOverflows, "Overflow",
                           "Layout"},
    SubtitlesParameterSpec{"textAlign", Kind::Choice, false, 1, 0, 2, kAlignments,
                           "Text alignment", "Layout"},
    SubtitlesParameterSpec{"useCuePositions", Kind::Boolean, false, 1, 0, 1, {},
                           "Use cue positions", "Layout"},
    SubtitlesParameterSpec{"useFileStyles", Kind::Boolean, false, 1, 0, 1, {}, "Use file styles",
                           "Layout"},
    SubtitlesParameterSpec{"fadeIn", Kind::Scalar, false, 0.0, 0.0, 60.0, {}, "Fade in",
                           "Timing", "s", 0.0, 2.0, 1.0 / 24.0, 3},
    SubtitlesParameterSpec{"fadeOut", Kind::Scalar, false, 0.0, 0.0, 60.0, {}, "Fade out",
                           "Timing", "s", 0.0, 2.0, 1.0 / 24.0, 3},
    SubtitlesParameterSpec{"showSpeaker", Kind::Choice, false, 0, 0, 1, kSpeakers,
                           "Show speaker", "Timing"},
    SubtitlesParameterSpec{"burnIn", Kind::Boolean, false, 1, 0, 1, {}, "Burn in", "Track"},
    SubtitlesParameterSpec{"exportTrack", Kind::Boolean, false, 1, 0, 1, {}, "Export as track",
                           "Track"},
    SubtitlesParameterSpec{"language", Kind::Text, false, 0, 0, 0, {}, "Language", "Track"},
    SubtitlesParameterSpec{"trackName", Kind::Text, false, 0, 0, 0, {}, "Track name", "Track"},
    SubtitlesParameterSpec{"maxCharactersPerLine", Kind::Whole, false, 42, 1, 1000, {},
                           "Max characters per line", "Checks", "", 10, 80},
    SubtitlesParameterSpec{"maxReadingSpeed", Kind::Scalar, false, 20.0, 1.0, 1000.0, {},
                           "Max reading speed", "Checks", "characters/s", 5.0, 40.0, 0.5, 1},
    SubtitlesParameterSpec{"minDuration", Kind::Scalar, false, 5.0 / 6.0, 0.0, 60.0, {},
                           "Min duration", "Checks", "s", 0.0, 5.0, 1.0 / 24.0, 3},
    SubtitlesParameterSpec{"minGap", Kind::Whole, false, 2, 0, 1000, {}, "Min gap", "Checks",
                           "frames", 0, 24},
};

} // namespace subtitles_parameters

[[nodiscard]] constexpr std::span<const SubtitlesParameterSpec> subtitlesParameters() noexcept {
    return subtitles_parameters::kParameters;
}

[[nodiscard]] constexpr const SubtitlesParameterSpec*
subtitlesParameterSpecForRole(const std::string_view role) noexcept {
    for (const auto& spec : subtitles_parameters::kParameters)
        if (spec.role == role)
            return &spec;
    return nullptr;
}

// The spec of a "bloom.subtitles.<role>" schema key, or null.
[[nodiscard]] constexpr const SubtitlesParameterSpec*
subtitlesParameterSpec(const std::string_view schemaKey) noexcept {
    constexpr std::string_view prefix = "bloom.subtitles.";
    if (!schemaKey.starts_with(prefix))
        return nullptr;
    return subtitlesParameterSpecForRole(schemaKey.substr(prefix.size()));
}

[[nodiscard]] constexpr bool isSubtitlesSchemaKey(const std::string_view key) noexcept {
    return subtitlesParameterSpec(key) != nullptr;
}

// Whether the parameter is a constant only (no key, no driver): every parameter that is neither
// animatable nor the cue list.
[[nodiscard]] constexpr bool isSubtitlesConstantSchemaKey(const std::string_view key) noexcept {
    const auto* spec = subtitlesParameterSpec(key);
    return spec != nullptr && !spec->animatable && spec->kind != SubtitlesParameterKind::Cues;
}

[[nodiscard]] constexpr bool
isSubtitlesAnimatableSchemaKey(const std::string_view key,
                               const SubtitlesParameterKind kind) noexcept {
    const auto* spec = subtitlesParameterSpec(key);
    return spec != nullptr && spec->animatable && spec->kind == kind;
}

// Whether the Scalar `value` lies in the parameter's domain; true for any other key.
[[nodiscard]] constexpr bool isSubtitlesScalarWithinDomain(const std::string_view key,
                                                           const double value) noexcept {
    const auto* spec = subtitlesParameterSpec(key);
    return spec == nullptr || spec->kind != SubtitlesParameterKind::Scalar ||
           (value >= spec->minimum && value <= spec->maximum);
}

} // namespace bloom::document
