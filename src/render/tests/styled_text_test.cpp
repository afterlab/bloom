#include <bloom/render/styled_text.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// bloom.render.styled_text (#752): the styled text layout the Subtitles node draws with. The line
// break opportunities are checked against UAX #14's own examples, and each wrap is checked against
// an independent brute force over the same measurements.
namespace {

namespace render = bloom::render;
using render::LineBreakAction;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

[[nodiscard]] std::vector<std::size_t> allowed(const std::u32string_view text) {
    const auto actions = render::lineBreakOpportunities(text);
    std::vector<std::size_t> out;
    for (std::size_t index = 0; index < actions.size(); ++index)
        if (actions[index] != LineBreakAction::Prohibited)
            out.push_back(index);
    return out;
}

void testOpportunities() {
    using List = std::vector<std::size_t>;
    expect(allowed(U"Hello world") == List{6}, "a space breaks before the next word");
    expect(allowed(U"Hello  world") == List{7}, "spaces stay at the end of the line");
    expect(allowed(U"self-made") == List{5}, "a hyphen breaks after itself");
    expect(allowed(U"(word)") == List{}, "no break inside brackets");
    expect(allowed(U"12,345.67") == List{}, "no break inside a number");
    expect(allowed(U"$12 50%") == List{4}, "a prefix and a postfix stay on their number");
    expect(allowed(U"\u65E5\u672C\u8A9E") == List{1, 2}, "ideographs break between characters");
    expect(allowed(U"\u65E5\u672C\u3002\u8A9E") == List{1, 3},
           "an ideographic full stop never starts a line");
    expect(allowed(U"\u300C\u65E5\u672C\u300D") == List{2},
           "an opening bracket keeps its character, a closing one its predecessor");
    expect(allowed(U"word\u00A0word") == List{}, "a no-break space glues");
    expect(allowed(U"e\u0301clair test") == List{8}, "a combining mark stays with its base");
    const auto hard = render::lineBreakOpportunities(U"a\nb");
    expect(hard.size() == 3 && hard[2] == LineBreakAction::Mandatory,
           "a line feed is a mandatory break");
    expect(allowed(U"\uAC00\uAC01 \uAC02") == List{1, 3},
           "Korean syllables break between syllables and at spaces (LB31)");
    expect(render::lineBreakClass(U'\u3002') == render::LineBreakClass::CL,
           "the table classifies U+3002 as CL");
    expect(render::unicodeTextTablesVersion() == "16.0.0", "the tables are UCD 16.0.0");
}

void testScripts() {
    expect(!render::firstShapedScript(U"Hello, world").has_value(), "Latin needs no shaping");
    expect(!render::firstShapedScript(U"\u65E5\u672C").has_value(), "Han needs no shaping");
    expect(render::firstShapedScript(U"Hi \u0645\u0631\u062D\u0628\u0627") == "Arabic",
           "Arabic is named");
    expect(render::firstShapedScript(U"\u05E9\u05DC\u05D5\u05DD") == "Hebrew", "Hebrew is named");
    expect(render::firstShapedScript(U"\u0E2A\u0E27\u0E31\u0E2A") == "Thai", "Thai is named");
    expect(render::firstShapedScript(U"\u0928\u092E") == "Devanagari", "Devanagari is named");
}

const std::vector<render::TextFont> kFaces{render::EmbeddedFace::DejaVuSans};

[[nodiscard]] render::StyledTextOptions options(const double maxWidth,
                                                const render::TextWrapMode wrap) {
    render::StyledTextOptions result;
    result.horizontalSize = 40.0;
    result.verticalSize = 40.0;
    result.lineHeight = 1.15;
    result.maxWidth = maxWidth;
    result.wrap = wrap;
    return result;
}

[[nodiscard]] render::StyledTextBlock layout(const std::u32string& text, const double maxWidth,
                                             const render::TextWrapMode wrap) {
    const std::vector<render::StyledRun> runs{{text, 0, false, 0, false}};
    auto result = render::layoutStyledText(kFaces, runs, options(maxWidth, wrap));
    expect(result.hasValue(), "the layout succeeds");
    return result.hasValue() ? *result.value() : render::StyledTextBlock{};
}

// The width of a piece laid out on its own line.
[[nodiscard]] double widthOf(const std::u32string& text) {
    return layout(text, 0.0, render::TextWrapMode::Explicit).width;
}

void testGreedyAndBalanced() {
    const std::u32string text = U"one two three four five six";
    const auto limit = widthOf(U"one two three four five") + 0.5;
    const auto greedy = layout(text, limit, render::TextWrapMode::Greedy);
    expect(greedy.lines.size() == 2, "greedy wraps to two lines");
    expect(greedy.lines.size() == 2 && greedy.lines[1].glyphs.size() == 3,
           "greedy fills the first line and leaves 'six'");
    const auto balanced = layout(text, limit, render::TextWrapMode::Balanced);
    expect(balanced.lines.size() == 2, "balanced keeps the greedy line count");
    // The brute force: every word boundary, the widest line as narrow as possible, a longer
    // bottom line on a tie.
    const std::vector<std::size_t> spaces{3, 7, 13, 18, 23};
    double bestWidest = std::numeric_limits<double>::infinity();
    double bestBottom = 0.0;
    std::size_t bestTop = 0;
    for (const auto space : spaces) {
        const auto top = text.substr(0, space);
        const auto bottom = text.substr(space + 1);
        const auto widest = std::max(widthOf(top), widthOf(bottom));
        if (widest < bestWidest - 1e-9 ||
            (std::abs(widest - bestWidest) <= 1e-9 && widthOf(bottom) > bestBottom)) {
            bestWidest = widest;
            bestBottom = widthOf(bottom);
            bestTop = top.size();
        }
    }
    expect(balanced.lines.size() == 2 && balanced.lines[0].characters == bestTop,
           "balanced picks the brute force's break");
    expect(balanced.lines.size() == 2 && std::abs(balanced.width - bestWidest) < 1e-9,
           "balanced lines are as wide as the brute force's widest");
    expect(balanced.width < greedy.width, "balanced is narrower than greedy");
    const auto explicitOnly = layout(text, limit, render::TextWrapMode::Explicit);
    expect(explicitOnly.lines.size() == 1, "explicit only never wraps");
    const auto hard = layout(U"one\ntwo", 0.0, render::TextWrapMode::Explicit);
    expect(hard.lines.size() == 2, "a line feed always breaks");
}

void testCjkWraps() {
    const std::u32string text = U"\u65E5\u672C\u8A9E\u3002\u65E5\u672C\u8A9E\u3002";
    const auto one = widthOf(U"\u65E5");
    const auto block = layout(text, one * 3.5, render::TextWrapMode::Greedy);
    expect(block.lines.size() >= 2, "ideographs wrap without spaces");
    for (const auto& line : block.lines)
        expect(line.width <= one * 3.5 + 1e-9, "each line fits");
    // The full stop stays on its line: with room for three characters and a full stop's break
    // prohibited before it, the first line is the three ideographs and the stop (it overflows
    // only if it must) or the break moves earlier.
    std::size_t total = 0;
    for (const auto& line : block.lines)
        total += line.characters;
    expect(total == text.size(), "every character is laid out");
    expect(block.lines.size() >= 2 && block.lines[1].glyphs.size() > 0, "the second line draws");
}

void testRunsOutlines() {
    const std::vector<render::StyledRun> plain{{U"l", 0, false, 0, false}};
    const std::vector<render::StyledRun> oblique{{U"l", 0, true, 0, false}};
    const std::vector<render::StyledRun> underlined{{U"l", 0, false, 0, true}};
    const auto opts = options(0.0, render::TextWrapMode::Explicit);
    const auto rightmost = [&](const std::vector<render::StyledRun>& runs, std::size_t& paths) {
        const auto block = render::layoutStyledText(kFaces, runs, opts);
        const auto outlines =
            render::styledTextOutlines(kFaces, runs, *block.value(), opts, 0.0, 0.0);
        double right = -1e9;
        paths = 0;
        for (const auto& paint : *outlines.value()) {
            paths += paint.paths.size();
            for (const auto& path : paint.paths)
                for (const auto& anchor : path.anchors)
                    right = std::max(right, anchor.point.x);
        }
        return right;
    };
    std::size_t plainPaths = 0;
    std::size_t obliquePaths = 0;
    std::size_t underlinePaths = 0;
    const auto plainRight = rightmost(plain, plainPaths);
    const auto obliqueRight = rightmost(oblique, obliquePaths);
    rightmost(underlined, underlinePaths);
    // An 'l' is about 0.76 em tall: a 12 degree slant moves its top about 0.16 em right.
    expect(obliqueRight - plainRight > 0.1 * 40.0 && obliqueRight - plainRight < 0.25 * 40.0,
           "the synthesized oblique slants the glyph by 12 degrees");
    expect(obliquePaths == plainPaths, "the oblique draws the same contours");
    expect(underlinePaths == plainPaths + 1, "an underline adds one rectangle");
    // Two paints come back in first-use order.
    const std::vector<render::StyledRun> two{{U"a", 0, false, 3, false},
                                             {U"b", 0, false, 1, false}};
    const auto block = render::layoutStyledText(kFaces, two, opts);
    const auto outlines = render::styledTextOutlines(kFaces, two, *block.value(), opts, 0.0, 0.0);
    expect(outlines.value()->size() == 2 && (*outlines.value())[0].paint == 3 &&
               (*outlines.value())[1].paint == 1,
           "paints are grouped in the order they appear");
}

void testRefusals() {
    const std::vector<render::StyledRun> runs{{U"a", 0, false, 0, false}};
    auto bad = options(0.0, render::TextWrapMode::Greedy);
    bad.horizontalSize = 0.0;
    expect(!render::layoutStyledText(kFaces, runs, bad).hasValue(), "a zero size is refused");
    const std::vector<render::StyledRun> missingFace{{U"a", 3, false, 0, false}};
    expect(!render::layoutStyledText(kFaces, missingFace, options(0.0, render::TextWrapMode::Greedy))
                .hasValue(),
           "a face outside the table is refused");
    const auto cancelled = render::layoutStyledText(
        kFaces, runs, options(0.0, render::TextWrapMode::Greedy), [] { return true; });
    expect(!cancelled.hasValue(), "a cancelled layout publishes nothing");
}

// #758: the per-cluster layout record and fixed-width digits.
void testClustersAndDigits() {
    auto opts = options(0.0, render::TextWrapMode::Greedy);
    opts.alignment = render::TextAlignment::Left;
    // Clusters are scalars: a line feed and the spaces count, a carriage return does not.
    const std::vector<render::StyledRun> runs{{U"ab c\r\nd", 0, false, 0, false},
                                             {U"e", 0, false, 2, true}};
    const auto block = render::layoutStyledText(kFaces, runs, opts);
    expect(block.hasValue(), "the text lays out");
    if (!block.hasValue())
        return;
    const auto& lines = block.value()->lines;
    expect(lines.size() == 2 && lines[0].firstCluster == 0 && lines[0].endCluster == 4 &&
               lines[1].firstCluster == 5 && lines[1].endCluster == 7,
           "each line names the clusters it holds, the hard break in neither");
    std::vector<std::size_t> clusterOf;
    for (const auto& line : lines)
        for (const auto& glyph : line.glyphs)
            clusterOf.push_back(glyph.cluster);
    expect(clusterOf == std::vector<std::size_t>{0, 1, 2, 3, 5, 6},
           "every glyph names its cluster");
    const auto clusters = render::styledTextClusterOutlines(kFaces, runs, *block.value(), opts,
                                                            10.0, 20.0);
    const auto grouped = render::styledTextOutlines(kFaces, runs, *block.value(), opts, 10.0, 20.0);
    expect(clusters.hasValue() && grouped.hasValue(), "both outline forms build");
    if (!clusters.hasValue() || !grouped.hasValue())
        return;
    std::size_t clusterPaths = 0;
    for (const auto& cluster : *clusters.value())
        clusterPaths += cluster.paths.size();
    std::size_t groupedPaths = 0;
    for (const auto& paint : *grouped.value())
        groupedPaths += paint.paths.size();
    expect(clusters.value()->size() == 6, "one record per drawn glyph");
    // The underlined 'e' is one glyph, so its piece is the same one rectangle.
    expect(clusterPaths == groupedPaths, "every path is in exactly one record");
    const auto& e = clusters.value()->back();
    const auto& d = (*clusters.value())[4];
    expect(e.cluster == 6 && e.line == 1 && e.paint == 2 && d.paint == 0,
           "a record names its line and paint");
    expect(std::abs(d.x - 10.0) < 1e-12 &&
               std::abs(d.baseline - (20.0 + block.value()->lineStep + block.value()->ascent)) <
                   1e-12,
           "a record's pen and baseline place the block at (x, y)");
    // Fixed-width digits: every digit takes the widest digit advance, so "1" and "8" are as wide.
    // Inter's default digits are proportional (DejaVu Sans's are already tabular).
    const std::vector<render::TextFont> inter{render::EmbeddedFace::InterRegular};
    auto fixed = opts;
    fixed.fixedDigits = true;
    const auto width = [&](const std::u32string& text, const render::StyledTextOptions& o) {
        const std::vector<render::StyledRun> one{{text, 0, false, 0, false}};
        const auto laid = render::layoutStyledText(inter, one, o);
        return laid.hasValue() ? laid.value()->width : -1.0;
    };
    expect(width(U"1111", opts) < width(U"8888", opts), "the face's digits are proportional");
    expect(std::abs(width(U"1.11", fixed) - width(U"8.88", fixed)) < 1e-9 &&
               std::abs(width(U"10.5", fixed) - width(U"47.0", fixed)) < 1e-9,
           "fixed-width digits keep a number's width");
    expect(width(U"1111", opts) < width(U"1111", fixed), "a narrow digit widens");
    const std::vector<render::StyledRun> one{{U"1.5", 0, false, 0, false}};
    const auto laid = render::layoutStyledText(inter, one, fixed);
    const auto plain = render::layoutStyledText(inter, one, opts);
    expect(laid.hasValue() && plain.hasValue() && laid.value()->lines[0].glyphs[0].inset > 0.0 &&
               plain.value()->lines[0].glyphs[0].inset == 0.0,
           "a narrow fixed-width digit is centered in its advance");
}

} // namespace

int main() {
    testClustersAndDigits();
    testOpportunities();
    testScripts();
    testGreedyAndBalanced();
    testCjkWraps();
    testRunsOutlines();
    testRefusals();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "bloom.render.styled_text: all passed\n";
    return 0;
}
