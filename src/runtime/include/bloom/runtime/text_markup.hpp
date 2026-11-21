#pragma once

// #757: a Text Source's words as styled runs (render/styled_text.hpp). With Markup on, the text is
// read as cue-text markup by the same parser Subtitles uses (document/cue_markup.hpp): <b>, <i>,
// <u> and color spans <c.accent1> .. <c.accent4>, <c.#RRGGBB> (sRGB, also #RGB and #RRGGBBAA) and
// the WebVTT color classes (<c.yellow>). Anything the parser would ignore (an unknown tag, an end
// tag that closes nothing, a malformed timestamp, a lone '<') is drawn as the characters it is,
// with a warning naming it. With Markup off the text is one literal run.
// docs/architecture/evaluation-primitives.md, "Styled Text Source".

#include <bloom/core/color.hpp>
#include <bloom/render/styled_text.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::runtime {

// A run's paint: 0 is the node's Color, 1 .. 4 its accents, then the literal colors in the order
// they first appear.
inline constexpr std::uint32_t kTextFillPaint = 0;
inline constexpr std::uint32_t kTextFirstAccentPaint = 1;
inline constexpr std::uint32_t kTextFirstLiteralPaint = 5;

struct TextMarkupRuns final {
    std::vector<render::StyledRun> runs;
    // The literal colors, straight linear RGBA; literal i is paint kTextFirstLiteralPaint + i.
    std::vector<core::Color4d> literals;
    // What was drawn as written rather than read as markup, and why; at most a few, in order.
    std::vector<std::string> warnings;
    // Whether a run asks for bold (the regular weight draws it when the font has no bold).
    bool usesBold = false;
};

// `faces` decides which runs draw their face as a synthesized oblique.
[[nodiscard]] TextMarkupRuns textMarkupRuns(std::string_view content, bool markup,
                                            const CompiledSubtitlesFaces& faces);

} // namespace bloom::runtime
