#ifndef BLOOM_RUNTIME_TEXT_STYLED_FRAME_HPP
#define BLOOM_RUNTIME_TEXT_STYLED_FRAME_HPP

// Private to src/runtime. #757: the host preparation of a styled Text Source's frame
// (docs/architecture/evaluation-primitives.md, "Styled Text Source"), shared by the CPU reference
// and the GPU scene builder so both draw exactly the same geometry, and by Text Outline for the
// glyphs. The words become styled runs (text_markup.hpp), are laid out with the styled text layout
// Subtitles uses (render/styled_text.hpp), placed by the Text Source's own box, alignment, anchor
// and vertical alignment, and drawn as the background box, the outline and the fills by paint, in
// that order, as path coverage layers (subtitles_frame.hpp's SubtitlesLayer). No pixel is produced
// here.

#include "subtitles_frame.hpp"

#include <bloom/document/document.hpp>
#include <bloom/render/path_raster.hpp>
#include <bloom/render/styled_text.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::runtime::detail {

// The most glyphs one styled text draws.
inline constexpr std::size_t kStyledTextGlyphLimit = 8192;
// #758: the most layers (distinct fill colors, outlines, the box and cursors) an animated text
// draws in one frame.
inline constexpr std::size_t kStyledTextLayerLimit = 256;

// What the layout reads, resolved at the frame.
struct StyledTextSourceInput final {
    std::string_view content;
    bool markup = false;
    const CompiledSubtitlesFaces* faces = nullptr;
    double size = 0.0;
    double lineHeight = 1.0;
    double letterSpacing = 0.0;
    std::int64_t alignment = 0;
    document::Vec2d box{};
    bool wrap = false;
    std::int64_t verticalAlignment = 0;
    std::int64_t anchorMode = 0;
    std::int64_t overflow = 0;
    // #758: fixed-width digits, and whether to keep the per-cluster record (a Text Animator).
    bool fixedDigits = false;
    bool clusters = false;
};

// One laid-out line: its advance box (author pixels, the text origin at (0, 0)).
struct StyledTextLine final {
    double left = 0.0;
    double top = 0.0;
    double width = 0.0;
};

struct StyledTextGeometry final {
    std::vector<render::StyledPaintOutlines> fills;
    std::vector<StyledTextLine> lines;
    double lineStep = 0.0;
    std::size_t glyphs = 0;
    std::vector<std::string> warnings;
    // The paints the runs use (kTextFillPaint, the accents, the literals), sorted.
    std::vector<std::uint32_t> paints;
    std::vector<core::Color4d> literals;
    // #758 (with StyledTextSourceInput::clusters): one record per drawn glyph, placed as `fills`
    // is; the text's clusters, each cluster's line, and the line count; the first run's face,
    // size options and the text's top, for the cursor.
    std::vector<render::StyledClusterOutline> clusters;
    std::u32string text;
    std::vector<std::size_t> lineOf;
    std::size_t lineCount = 0;
    double ascent = 0.0;
    double top = 0.0;
    render::StyledTextOptions options;
};

// Lays the text out. Fails with a message (empty when cancelled).
[[nodiscard]] std::variant<StyledTextGeometry, std::string>
layoutStyledTextSource(const StyledTextSourceInput& input, const render::PathCancellation& cancel);

// The node's frame at the evaluation's proxy scales. The frame's bounds are the leaf's content
// bounds (its window over the scales, pivot as the Text Source's); `contentKey` holds everything
// the layers are made from and only the colors of the paints the runs use.
// #758: `time` is the operation's clock time; only a blinking cursor reads it.
[[nodiscard]] std::variant<SubtitlesFrame, SubtitlesFailure>
prepareStyledTextFrame(const CompiledText& text, const CompiledCompositionPlan& plan,
                       const ResolvedEvaluation& resolved, core::RationalTime time,
                       const CancellationToken& cancellation);

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_TEXT_STYLED_FRAME_HPP
