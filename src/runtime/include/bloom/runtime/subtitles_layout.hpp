#pragma once

// #752: the layout of a Subtitles node's cues (docs/architecture/subtitles.md, "Layout"), shared by
// the frame's host preparation (every render route), the per-cue checks and the read-backs. A cue's
// markup becomes styled runs (render/styled_text.hpp): italic, bold, underline and color from its
// tags, its classes (the WebVTT color classes and the list's styles) and its whole-cue style. The
// block wraps to the node's width, shrinks to fit its Max lines, and is placed in the safe area at
// the anchor plus the offset, or where the cue asks to be when Use cue positions is on. Cues that
// show together in one anchor region stack away from the anchored edge in start order.

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/cue_list.hpp>
#include <bloom/render/styled_text.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace bloom::runtime {

// The node's styling at one frame.
struct SubtitlesStyleValues final {
    double size = 59.0;
    double outlineWidth = 3.0;
    double lineHeight = 1.15;
    double letterSpacing = 0.0;
    double opacity = 1.0;
    core::Color4d fill{1.0, 1.0, 1.0, 1.0};
    core::Color4d outlineColor{0.0, 0.0, 0.0, 1.0};
    core::Color4d boxColor{0.0, 0.0, 0.0, 0.6};
    document::Vec2d offset{};
};

// One cue, laid out and placed. Colors are working-space values (a file's sRGB colors are
// decoded to linear); positions are author pixels, y down.
struct SubtitleCueLayout final {
    std::size_t index = 0;
    std::uint64_t id = 0;
    std::vector<render::StyledRun> runs;
    std::vector<core::Color4d> paints;
    core::Color4d outlineColor;
    double outlineWidth = 0.0;
    core::Color4d boxColor;
    render::StyledTextOptions options;
    render::StyledTextBlock block;
    // The text block's top left, and the box padding around it.
    double x = 0.0;
    double y = 0.0;
    double padding = 0.0;
    // The lines the cue wraps to at its full size, and whether Shrink to fit reduced it.
    std::size_t naturalLines = 0;
    bool shrunk = false;
    // The first script in the cue that needs shaping, when there is one.
    std::optional<std::string> unshapedScript;
};

// The cues at `indices` of `list`, laid out together (so they stack). Fails with a message for a
// layout the fonts cannot produce, or when cancelled (the message is then empty).
[[nodiscard]] std::variant<std::vector<SubtitleCueLayout>, std::string>
layoutSubtitleCues(const CompiledSubtitles& subtitles, const document::CueListValue& list,
                   std::span<const std::size_t> indices, const SubtitlesStyleValues& style,
                   const render::PathCancellation& cancelled = {});

// #752: where each cue that shows at `time` (on the node's clock) is drawn: its block (its box
// when the box is on), author pixels, in stacking order. What the Viewer outlines and hit-tests;
// the same layout the frame draws. Fails as layoutSubtitleCues() does.
struct SubtitlesPlacedBlock final {
    std::size_t index = 0;
    std::uint64_t id = 0;
    render::PathBounds block;
};
[[nodiscard]] std::variant<std::vector<SubtitlesPlacedBlock>, std::string>
placeSubtitleCues(const CompiledSubtitles& subtitles, const document::CueListValue& list,
                  core::RationalTime time, const SubtitlesStyleValues& style,
                  const render::PathCancellation& cancelled = {});

// The safe area the node places cues in, author pixels.
[[nodiscard]] render::PathBounds subtitlesSafeArea(const CompiledSubtitlesSettings& settings);

// One finding of the checks (docs/architecture/subtitles.md, "Checks"): a readout that never
// changes the render.
struct SubtitleCueCheck final {
    std::string code;
    std::string message;
    friend bool operator==(const SubtitleCueCheck&, const SubtitleCueCheck&) = default;
};

struct SubtitlesCheckSettings final {
    std::int64_t maxCharactersPerLine = 42;
    double maxReadingSpeed = 20.0;
    double minDuration = 5.0 / 6.0;
    std::int64_t minGapFrames = 2;
    document::FrameRate frameRate = document::FrameRate::framesPerSecond24();
    core::RationalTime compositionDuration;
};

// The checks of every cue of `list`, one vector per cue in list order. Each cue is laid out on its
// own at the node's styling.
[[nodiscard]] std::vector<std::vector<SubtitleCueCheck>>
checkSubtitleCues(const CompiledSubtitles& subtitles, const document::CueListValue& list,
                  const SubtitlesStyleValues& style, const SubtitlesCheckSettings& settings);

// The node's styling as its constants hold it (an animated or driven parameter reads its authored
// fallback), for the checks and the read-backs.
[[nodiscard]] SubtitlesStyleValues subtitlesConstantStyle(const CompiledSubtitles& subtitles);

} // namespace bloom::runtime
