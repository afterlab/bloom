#ifndef BLOOM_RENDER_STYLED_TEXT_HPP
#define BLOOM_RENDER_STYLED_TEXT_HPP

// #752: styled text layout, shared with the Text node's layout (text_raster.hpp) through the same
// glyph access (text_font_access.hpp): one module, so shaping (#345) later upgrades both.
//
// A block is a sequence of runs. Each run names a face from the caller's face table, whether that
// face is drawn as a synthesized oblique (a 12 degree shear of its outlines, for a font with no
// italic), a paint index the caller resolves to a color, and whether it is underlined. Lines break
// at mandatory breaks ('\n') and, when a width is given, at the Unicode line break opportunities of
// UAX #14 (lineBreakOpportunities()). Layout is in author pixels: advances, kerning and letter
// spacing as the Text node measures them, with no shaping and no bidi.
//
// Everything here is pure, deterministic, Qt-free and safe on any thread; the outlines feed the
// path coverage reference (path_raster.hpp) on every route.

#include <bloom/render/embedded_fonts.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/render/path_raster.hpp>
#include <bloom/render/text_raster.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::render {

// The UAX #14 line break classes the layout distinguishes, after the LB1 resolution the table
// generator applies (AI, SG, XX and unlisted code points are AL; SA is CM for marks and AL
// otherwise; CJ is NS).
enum class LineBreakClass : std::uint8_t {
    AL, BA, BB, BK, B2, CB, CL, CM, CP, CR, EB, EM, EX, GL, H2, H3, HL, HY, ID, IN, IS, JL, JT, JV,
    LF, NL, NS, NU, OP, PO, PR, Quotation, RI, SP, SY, WJ, ZW, ZWJ, AK, AP, AS, VF, VI,
};

// The Unicode Character Database version the tables were generated from.
[[nodiscard]] std::string_view unicodeTextTablesVersion() noexcept;
[[nodiscard]] LineBreakClass lineBreakClass(char32_t scalar) noexcept;

// Before scalar i (i in 1..size-1): whether a line may or must break there. Index 0 is always
// Prohibited, and the end of the text is not represented (it is the end of a line).
enum class LineBreakAction : std::uint8_t { Prohibited, Allowed, Mandatory };
[[nodiscard]] std::vector<LineBreakAction> lineBreakOpportunities(std::u32string_view text);

// The name of the first script in `text` that Bloom cannot lay out correctly without shaping or
// bidi (Arabic, Hebrew, the Indic scripts, Thai and others, by the Unicode Script property), or
// nullopt.
[[nodiscard]] std::optional<std::string_view> firstShapedScript(std::u32string_view text) noexcept;

// The tangent of the synthesized oblique's 12 degree slant.
inline constexpr double kSyntheticObliqueSlant = 0.21255656167002213;

struct StyledRun final {
    std::u32string text;
    std::size_t face = 0;
    bool oblique = false;
    std::uint32_t paint = 0;
    bool underline = false;
    friend bool operator==(const StyledRun&, const StyledRun&) = default;
};

enum class TextWrapMode : std::uint8_t { Balanced, Greedy, Explicit };

struct StyledTextOptions final {
    // Pixels per em, horizontally and vertically (a proxy scales each axis).
    double horizontalSize = 0.0;
    double verticalSize = 0.0;
    double lineHeight = 1.0;
    // Pixels added between consecutive glyphs of a line.
    double letterSpacing = 0.0;
    // Lines wrap to this width; 0 never wraps.
    double maxWidth = 0.0;
    TextWrapMode wrap = TextWrapMode::Greedy;
    TextAlignment alignment = TextAlignment::Center;
    // #758: every ASCII digit takes the widest digit advance of its face, drawn centered in it, and
    // no kerning pairs a digit, so a changing number keeps its width (tabular figures).
    bool fixedDigits = false;
};

struct StyledGlyph final {
    std::size_t run = 0;
    int glyph = 0;
    // The glyph's pen position from the line's left edge.
    double x = 0.0;
    double advance = 0.0;
    // #758: the glyph's cluster, its index in the runs' text with carriage returns removed (one
    // Unicode scalar per cluster until shaping, #345), and how far right of the pen the outline is
    // drawn (half the difference a fixed-width digit adds, otherwise 0).
    std::size_t cluster = 0;
    double inset = 0.0;
};

struct StyledLine final {
    std::vector<StyledGlyph> glyphs;
    // The width of the line's ink advance, trailing spaces excluded.
    double width = 0.0;
    // The left edge of the line within the block (its alignment).
    double offset = 0.0;
    // The number of characters (scalars) the line shows, spaces included, trailing spaces not.
    std::size_t characters = 0;
    // #758: the clusters [firstCluster, endCluster) the line holds, its trailing spaces included and
    // the hard break that ends it not.
    std::size_t firstCluster = 0;
    std::size_t endCluster = 0;
};

struct StyledTextBlock final {
    std::vector<StyledLine> lines;
    // The block's width (its widest line) and height (lines times the line step).
    double width = 0.0;
    double height = 0.0;
    double lineStep = 0.0;
    // The first face's ascent and descent, in pixels: the baseline of line i is at
    // i * lineStep + ascent from the block's top.
    double ascent = 0.0;
    double descent = 0.0;
    // How many glyphs the block draws.
    std::size_t glyphCount = 0;
};

// Lays the runs out. Fails with InvalidParameter for a size, line height, spacing or width that is
// not finite or out of range, InvalidState for a face that does not parse or when cancelled.
[[nodiscard]] ImageResult<StyledTextBlock>
layoutStyledText(std::span<const TextFont> faces, std::span<const StyledRun> runs,
                 const StyledTextOptions& options, const PathCancellation& cancelled = {});

// One paint's outlines: glyph fills and underline rectangles.
struct StyledPaintOutlines final {
    std::uint32_t paint = 0;
    std::vector<Path> paths;
};

// The glyph outlines and underlines of `block`, placed with the block's top left at (x, y), grouped
// by paint in the order paints first appear. Coordinates are author pixels, y down.
[[nodiscard]] ImageResult<std::vector<StyledPaintOutlines>>
styledTextOutlines(std::span<const TextFont> faces, std::span<const StyledRun> runs,
                   const StyledTextBlock& block, const StyledTextOptions& options, double x,
                   double y, const PathCancellation& cancelled = {});

// #758: the per-cluster layout record. One entry per drawn glyph, in reading order: its cluster
// (StyledGlyph::cluster), the line it is on, its run's paint, its pen position and baseline (the
// block placed with its top left at (x, y)) and advance, and its outlines placed there: the glyph
// and, for an underlined run, the underline under its advance up to the next glyph of the same
// stretch. Every path of styledTextOutlines() is in exactly one entry.
struct StyledClusterOutline final {
    std::size_t cluster = 0;
    std::size_t line = 0;
    std::size_t run = 0;
    std::uint32_t paint = 0;
    double x = 0.0;
    double baseline = 0.0;
    double advance = 0.0;
    std::vector<Path> paths;
};
[[nodiscard]] ImageResult<std::vector<StyledClusterOutline>>
styledTextClusterOutlines(std::span<const TextFont> faces, std::span<const StyledRun> runs,
                          const StyledTextBlock& block, const StyledTextOptions& options, double x,
                          double y, const PathCancellation& cancelled = {});

} // namespace bloom::render

#endif // BLOOM_RENDER_STYLED_TEXT_HPP
