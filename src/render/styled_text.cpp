#include <bloom/render/styled_text.hpp>

#include "text_font_access.hpp"
#include "text_glyph_source.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace bloom::render {
namespace {

struct LineBreakRange final {
    char32_t first;
    char32_t last;
    LineBreakClass value;
};
struct ScriptRange final {
    char32_t first;
    char32_t last;
    std::uint8_t script;
};

#include "unicode_text_tables.inc"

using Class = LineBreakClass;

[[nodiscard]] ImageError codeError(const ImageErrorCode code) noexcept {
    return ImageError::codeOnly(code);
}

[[nodiscard]] bool isAlphabetic(const Class value) noexcept {
    return value == Class::AL || value == Class::HL;
}
[[nodiscard]] bool isIdeographicLike(const Class value) noexcept {
    return value == Class::ID || value == Class::EB || value == Class::EM;
}
[[nodiscard]] bool isHangul(const Class value) noexcept {
    return value == Class::JL || value == Class::JV || value == Class::JT || value == Class::H2 ||
           value == Class::H3;
}

// UAX #14 rules LB12 to LB31 between two non-space classes, `before` being the class of the last
// non-space before the opportunity (spaces between them already handled) and `spaces` whether
// spaces separate them. Returns whether a break is prohibited.
[[nodiscard]] bool prohibitedAcross(const Class before, const Class after, const bool spaces,
                                    const Class previous) noexcept {
    // LB14: OP SP* x
    if (before == Class::OP)
        return true;
    // LB15a/b (simplified to the common rule): Quotation SP* x OP
    if (before == Class::Quotation && after == Class::OP)
        return true;
    // LB16: (CL | CP) SP* x NS
    if ((before == Class::CL || before == Class::CP) && after == Class::NS)
        return true;
    // LB17: B2 SP* x B2
    if (before == Class::B2 && after == Class::B2)
        return true;
    if (spaces)
        return false; // LB18: SP break
    // LB12: GL x
    if (before == Class::GL)
        return true;
    // LB12a: [^SP BA HY] x GL
    if (after == Class::GL && before != Class::BA && before != Class::HY)
        return true;
    // LB19: x Quotation, Quotation x
    if (after == Class::Quotation || before == Class::Quotation)
        return true;
    // LB20: break CB, CB break
    if (after == Class::CB || before == Class::CB)
        return false;
    // LB21: x BA, x HY, x NS, BB x
    if (after == Class::BA || after == Class::HY || after == Class::NS || before == Class::BB)
        return true;
    // LB21a: HL (HY | BA) x
    if ((before == Class::HY || before == Class::BA) && previous == Class::HL)
        return true;
    // LB21b: SY x HL
    if (before == Class::SY && after == Class::HL)
        return true;
    // LB22: x IN
    if (after == Class::IN)
        return true;
    // LB23: (AL | HL) x NU, NU x (AL | HL)
    if ((isAlphabetic(before) && after == Class::NU) || (before == Class::NU && isAlphabetic(after)))
        return true;
    // LB23a: PR x (ID | EB | EM), (ID | EB | EM) x PO
    if ((before == Class::PR && isIdeographicLike(after)) ||
        (isIdeographicLike(before) && after == Class::PO))
        return true;
    // LB24: (PR | PO) x (AL | HL), (AL | HL) x (PR | PO)
    if (((before == Class::PR || before == Class::PO) && isAlphabetic(after)) ||
        (isAlphabetic(before) && (after == Class::PR || after == Class::PO)))
        return true;
    // LB25: numbers.
    if (after == Class::NU &&
        (before == Class::PR || before == Class::PO || before == Class::HY ||
         before == Class::IS || before == Class::NU || before == Class::SY))
        return true;
    if ((before == Class::NU || before == Class::CL || before == Class::CP) &&
        (after == Class::PO || after == Class::PR))
        return true;
    if ((before == Class::PO || before == Class::PR) && after == Class::OP)
        return true;
    // LB26, LB27: Korean syllable blocks.
    if (before == Class::JL &&
        (after == Class::JL || after == Class::JV || after == Class::H2 || after == Class::H3))
        return true;
    if ((before == Class::JV || before == Class::H2) && (after == Class::JV || after == Class::JT))
        return true;
    if ((before == Class::JT || before == Class::H3) && after == Class::JT)
        return true;
    if ((isHangul(before) && after == Class::PO) || (before == Class::PR && isHangul(after)))
        return true;
    // LB28: (AL | HL) x (AL | HL)
    if (isAlphabetic(before) && isAlphabetic(after))
        return true;
    // LB28a: Brahmic orthographic syllables (aksara).
    if (before == Class::AP && (after == Class::AK || after == Class::AS))
        return true;
    if ((before == Class::AK || before == Class::AS) &&
        (after == Class::VF || after == Class::VI))
        return true;
    if (before == Class::VI && (after == Class::AK || after == Class::AS))
        return true;
    // LB29: IS x (AL | HL)
    if (before == Class::IS && isAlphabetic(after))
        return true;
    // LB30: (AL | HL | NU) x OP, CP x (AL | HL | NU)
    if ((isAlphabetic(before) || before == Class::NU) && after == Class::OP)
        return true;
    if (before == Class::CP && (isAlphabetic(after) || after == Class::NU))
        return true;
    // LB30b: EB x EM
    if (before == Class::EB && after == Class::EM)
        return true;
    return false; // LB31
}

// One face of the caller's table, opened once for the whole layout.
using OpenFace = std::variant<EmbeddedFace, detail::ExternalFontFace>;

[[nodiscard]] OpenFace openFace(const TextFont& font) {
    return std::visit(
        [](const auto& face) -> OpenFace {
            using Face = std::decay_t<decltype(face)>;
            if constexpr (std::is_same_v<Face, EmbeddedFace>)
                return face;
            else
                return detail::openExternalFont(face);
        },
        font);
}

struct Measured final {
    char32_t scalar = 0;
    std::size_t run = 0;
    int glyph = 0;
    double advance = 0.0;
    // Added before this scalar when the previous one is on the same line: letter spacing and the
    // face's kern pair.
    double join = 0.0;
    Class breakClass = Class::AL;
};

[[nodiscard]] bool isTrailingSpace(const char32_t scalar) noexcept {
    return scalar == U' ' || scalar == U'\t' || scalar == U'\u3000';
}

} // namespace

std::string_view unicodeTextTablesVersion() noexcept { return kUnicodeTextTablesVersion; }

LineBreakClass lineBreakClass(const char32_t scalar) noexcept {
    const auto* end = std::end(kLineBreakRanges);
    const auto* found = std::upper_bound(
        std::begin(kLineBreakRanges), end, scalar,
        [](const char32_t value, const LineBreakRange& range) { return value < range.first; });
    if (found == std::begin(kLineBreakRanges))
        return Class::AL;
    --found;
    return scalar <= found->last ? found->value : Class::AL;
}

std::optional<std::string_view> firstShapedScript(const std::u32string_view text) noexcept {
    for (const auto scalar : text) {
        const auto* end = std::end(kShapedScriptRanges);
        const auto* found = std::upper_bound(
            std::begin(kShapedScriptRanges), end, scalar,
            [](const char32_t value, const ScriptRange& range) { return value < range.first; });
        if (found == std::begin(kShapedScriptRanges))
            continue;
        --found;
        if (scalar <= found->last)
            return kShapedScriptNames[found->script];
    }
    return std::nullopt;
}

std::vector<LineBreakAction> lineBreakOpportunities(const std::u32string_view text) {
    std::vector<LineBreakAction> actions(text.size(), LineBreakAction::Prohibited);
    if (text.empty())
        return actions;
    // The class each position resolves to under LB9 and LB10: a combining mark or ZWJ takes the
    // class of what it follows (unless that is a space or a break), and a lone one is AL.
    std::vector<Class> classes(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        auto value = lineBreakClass(text[index]);
        if ((value == Class::CM || value == Class::ZWJ) && index > 0) {
            const auto base = classes[index - 1];
            if (base != Class::SP && base != Class::BK && base != Class::CR && base != Class::LF &&
                base != Class::NL && base != Class::ZW)
                value = base;
            else
                value = Class::AL;
        } else if (value == Class::CM || value == Class::ZWJ) {
            value = Class::AL;
        }
        classes[index] = value;
    }
    // The last non-space class before each position and the class before that one, whether
    // spaces separate it from the position, and whether a ZW SP* run is open (LB8).
    Class lastNonSpace = classes[0];
    Class beforeLast = Class::AL;
    bool spaces = false;
    bool afterZeroWidth = classes[0] == Class::ZW;
    std::size_t regionalRun = classes[0] == Class::RI ? 1 : 0;
    for (std::size_t index = 1; index < text.size(); ++index) {
        const auto previous = classes[index - 1];
        const auto previousRaw = lineBreakClass(text[index - 1]);
        const auto raw = lineBreakClass(text[index]);
        const auto current = classes[index];
        auto action = LineBreakAction::Prohibited;
        if (previous == Class::BK || previous == Class::LF || previous == Class::NL ||
            (previous == Class::CR && current != Class::LF)) {
            action = LineBreakAction::Mandatory; // LB4, LB5
        } else if (current == Class::BK || current == Class::CR || current == Class::LF ||
                   current == Class::NL || current == Class::SP || current == Class::ZW) {
            action = LineBreakAction::Prohibited; // LB6, LB7
        } else if (afterZeroWidth) {
            action = LineBreakAction::Allowed; // LB8
        } else if (previousRaw == Class::ZWJ ||
                   ((raw == Class::CM || raw == Class::ZWJ) && previous != Class::SP)) {
            action = LineBreakAction::Prohibited; // LB8a, LB9
        } else if (current == Class::WJ) {
            action = LineBreakAction::Prohibited; // LB11
        } else if (lastNonSpace == Class::WJ) {
            action = spaces ? LineBreakAction::Allowed : LineBreakAction::Prohibited; // LB11
        } else if (current == Class::CL || current == Class::CP || current == Class::EX ||
                   current == Class::SY || current == Class::IS) {
            action = LineBreakAction::Prohibited; // LB13
        } else if (current == Class::RI && lastNonSpace == Class::RI && !spaces) {
            // LB30a: pairs of regional indicators.
            action = regionalRun % 2 == 1 ? LineBreakAction::Prohibited : LineBreakAction::Allowed;
        } else {
            action = prohibitedAcross(lastNonSpace, current, spaces, beforeLast)
                         ? LineBreakAction::Prohibited
                         : LineBreakAction::Allowed;
        }
        actions[index] = action;
        if (current == Class::SP) {
            spaces = true;
        } else {
            beforeLast = spaces ? Class::SP : lastNonSpace;
            lastNonSpace = current;
            spaces = false;
            afterZeroWidth = current == Class::ZW;
        }
        regionalRun = current == Class::RI ? regionalRun + 1 : 0;
    }
    return actions;
}

ImageResult<StyledTextBlock> layoutStyledText(const std::span<const TextFont> faces,
                                              const std::span<const StyledRun> runs,
                                              const StyledTextOptions& options,
                                              const PathCancellation& cancelled) {
    using Result = ImageResult<StyledTextBlock>;
    if (!std::isfinite(options.horizontalSize) || !std::isfinite(options.verticalSize) ||
        options.horizontalSize <= 0.0 || options.verticalSize <= 0.0 ||
        options.horizontalSize > kMaximumTextPixelSize ||
        options.verticalSize > kMaximumTextPixelSize || !std::isfinite(options.lineHeight) ||
        options.lineHeight <= 0.0 || !std::isfinite(options.letterSpacing) ||
        !std::isfinite(options.maxWidth) || options.maxWidth < 0.0 || faces.empty())
        return Result::failure(codeError(ImageErrorCode::InvalidParameter));
    std::vector<OpenFace> open;
    open.reserve(faces.size());
    std::vector<float> scales;
    for (const auto& font : faces) {
        open.push_back(openFace(font));
        const bool parsed =
            std::visit([](const auto& face) { return font_access::fontIsParsed(face); },
                       open.back());
        if (!parsed)
            return Result::failure(codeError(ImageErrorCode::InvalidState));
        scales.push_back(std::visit(
            [&](const auto& face) {
                return font_access::fontScaleForEmPixelSize(face, options.horizontalSize);
            },
            open.back()));
        if (!(scales.back() > 0.0F))
            return Result::failure(codeError(ImageErrorCode::InvalidParameter));
    }
    const auto verticalScale = std::visit(
        [&](const auto& face) {
            return font_access::fontScaleForEmPixelSize(face, options.verticalSize);
        },
        open.front());
    const auto metrics = std::visit(
        [](const auto& face) { return font_access::fontVerticalMetrics(face); }, open.front());

    // Every scalar with its glyph, advance and join.
    std::vector<Measured> scalars;
    std::u32string text;
    for (std::size_t run = 0; run < runs.size(); ++run) {
        if (runs[run].face >= faces.size())
            return Result::failure(codeError(ImageErrorCode::InvalidParameter));
        for (const auto scalar : runs[run].text) {
            if (scalar == U'\r')
                continue;
            Measured measured;
            measured.scalar = scalar;
            measured.run = run;
            text.push_back(scalar);
            scalars.push_back(measured);
        }
    }
    const auto breaks = lineBreakOpportunities(text);
    for (std::size_t index = 0; index < scalars.size(); ++index) {
        if (cancelled && cancelled())
            return Result::failure(codeError(ImageErrorCode::InvalidState));
        auto& measured = scalars[index];
        measured.breakClass = lineBreakClass(measured.scalar);
        if (measured.scalar == U'\n')
            continue;
        const auto faceIndex = runs[measured.run].face;
        const auto scale = static_cast<double>(scales[faceIndex]);
        std::visit(
            [&](const auto& face) {
                measured.glyph = font_access::fontGlyphIndex(face, measured.scalar);
                measured.advance = static_cast<double>(font_access::fontGlyphHorizontalMetrics(
                                                           face, measured.glyph)
                                                           .advanceWidth) *
                                   scale;
                if (index > 0 && scalars[index - 1].scalar != U'\n') {
                    measured.join = options.letterSpacing;
                    if (runs[scalars[index - 1].run].face == faceIndex)
                        measured.join += static_cast<double>(font_access::fontGlyphKernAdvance(
                                             face, scalars[index - 1].glyph, measured.glyph)) *
                                         scale;
                }
            },
            open[faceIndex]);
    }
    // Prefix sums: the width of [a, e) is its advances plus every join after its first scalar.
    std::vector<double> advanceSum(scalars.size() + 1, 0.0);
    std::vector<double> joinSum(scalars.size() + 1, 0.0);
    for (std::size_t index = 0; index < scalars.size(); ++index) {
        advanceSum[index + 1] = advanceSum[index] + scalars[index].advance;
        joinSum[index + 1] = joinSum[index] + scalars[index].join;
    }
    const auto trimmedEnd = [&](const std::size_t begin, std::size_t end) {
        while (end > begin && isTrailingSpace(scalars[end - 1].scalar))
            --end;
        return end;
    };
    const auto widthOf = [&](const std::size_t begin, const std::size_t end) {
        const auto last = trimmedEnd(begin, end);
        if (last <= begin)
            return 0.0;
        return (advanceSum[last] - advanceSum[begin]) + (joinSum[last] - joinSum[begin + 1]);
    };

    // Paragraphs: the spans between mandatory breaks ('\n' and the other hard breaks).
    std::vector<std::pair<std::size_t, std::size_t>> lineSpans;
    const bool wraps = options.maxWidth > 0.0 && options.wrap != TextWrapMode::Explicit;
    std::size_t paragraph = 0;
    const auto layoutParagraph = [&](const std::size_t begin, const std::size_t end) {
        if (!wraps || widthOf(begin, end) <= options.maxWidth) {
            lineSpans.emplace_back(begin, end);
            return;
        }
        // The opportunities inside, plus the paragraph's end.
        std::vector<std::size_t> candidates;
        for (std::size_t index = begin + 1; index < end; ++index)
            if (breaks[index] == LineBreakAction::Allowed)
                candidates.push_back(index);
        candidates.push_back(end);
        // Greedy: the furthest opportunity that fits; a piece with none that fits breaks between
        // characters, as the Text node does.
        std::vector<std::pair<std::size_t, std::size_t>> greedy;
        std::size_t start = begin;
        std::size_t cursor = 0;
        while (start < end) {
            std::size_t chosen = 0;
            while (cursor < candidates.size() && candidates[cursor] <= start)
                ++cursor;
            for (std::size_t probe = cursor; probe < candidates.size(); ++probe) {
                if (widthOf(start, candidates[probe]) <= options.maxWidth)
                    chosen = candidates[probe];
                else
                    break;
            }
            if (chosen == 0) {
                const auto limit = cursor < candidates.size() ? candidates[cursor] : end;
                chosen = start + 1;
                while (chosen < limit && widthOf(start, chosen + 1) <= options.maxWidth)
                    ++chosen;
                if (chosen < limit) {
                    // An emergency break is an opportunity for the balanced pass too.
                    candidates.insert(
                        std::lower_bound(candidates.begin(), candidates.end(), chosen), chosen);
                }
            }
            greedy.emplace_back(start, chosen);
            start = chosen;
        }
        if (options.wrap != TextWrapMode::Balanced || greedy.size() < 2) {
            lineSpans.insert(lineSpans.end(), greedy.begin(), greedy.end());
            return;
        }
        // Balanced: the same number of lines, the widest as narrow as it can be, a longer last
        // line preferred among equal ones. best[l][j] is the narrowest widest line of l lines
        // ending at positions[j].
        std::vector<std::size_t> positions{begin};
        positions.insert(positions.end(), candidates.begin(), candidates.end());
        const auto count = greedy.size();
        constexpr double kInfinite = std::numeric_limits<double>::infinity();
        std::vector<std::vector<double>> best(count + 1,
                                              std::vector<double>(positions.size(), kInfinite));
        std::vector<std::vector<std::size_t>> from(
            count + 1, std::vector<std::size_t>(positions.size(), 0));
        best[0][0] = 0.0;
        for (std::size_t lines = 1; lines <= count; ++lines) {
            for (std::size_t to = 1; to < positions.size(); ++to) {
                for (std::size_t at = 0; at < to; ++at) {
                    if (best[lines - 1][at] == kInfinite)
                        continue;
                    const auto width = widthOf(positions[at], positions[to]);
                    if (width > options.maxWidth)
                        continue;
                    const auto widest = std::max(best[lines - 1][at], width);
                    const bool better = widest < best[lines][to] - 1e-9;
                    const bool tie = !better && std::abs(widest - best[lines][to]) <= 1e-9;
                    if (better ||
                        (tie && lines == count && to + 1 == positions.size() &&
                         width > widthOf(positions[from[lines][to]], positions[to]))) {
                        best[lines][to] = widest;
                        from[lines][to] = at;
                    }
                }
            }
        }
        if (best[count].back() == kInfinite) {
            lineSpans.insert(lineSpans.end(), greedy.begin(), greedy.end());
            return;
        }
        std::vector<std::pair<std::size_t, std::size_t>> balanced;
        std::size_t to = positions.size() - 1;
        for (std::size_t lines = count; lines > 0; --lines) {
            const auto at = from[lines][to];
            balanced.emplace_back(positions[at], positions[to]);
            to = at;
        }
        std::reverse(balanced.begin(), balanced.end());
        lineSpans.insert(lineSpans.end(), balanced.begin(), balanced.end());
    };
    for (std::size_t index = 0; index <= scalars.size(); ++index) {
        if (cancelled && cancelled())
            return Result::failure(codeError(ImageErrorCode::InvalidState));
        if (index < scalars.size()) {
            const auto kind = scalars[index].breakClass;
            if (kind != Class::BK && kind != Class::LF && kind != Class::NL && kind != Class::CR)
                continue;
        }
        // A hard break ends its paragraph and is not drawn.
        layoutParagraph(paragraph, index);
        paragraph = index + 1;
    }

    StyledTextBlock block;
    block.lineStep = options.verticalSize * options.lineHeight;
    block.ascent = static_cast<double>(metrics.ascent) * static_cast<double>(verticalScale);
    block.descent = -static_cast<double>(metrics.descent) * static_cast<double>(verticalScale);
    for (const auto& [begin, end] : lineSpans) {
        StyledLine line;
        const auto last = trimmedEnd(begin, end);
        double pen = 0.0;
        for (auto index = begin; index < last; ++index) {
            if (index > begin)
                pen += scalars[index].join;
            line.glyphs.push_back(
                {scalars[index].run, scalars[index].glyph, pen, scalars[index].advance});
            pen += scalars[index].advance;
        }
        line.width = widthOf(begin, end);
        line.characters = last - begin;
        block.glyphCount += line.glyphs.size();
        block.width = std::max(block.width, line.width);
        block.lines.push_back(std::move(line));
    }
    for (auto& line : block.lines) {
        const auto difference = block.width - line.width;
        line.offset = options.alignment == TextAlignment::Left     ? 0.0
                      : options.alignment == TextAlignment::Center ? difference / 2.0
                                                                   : difference;
    }
    block.height = block.lineStep * static_cast<double>(block.lines.size());
    if (!std::isfinite(block.width) || !std::isfinite(block.height))
        return Result::failure(codeError(ImageErrorCode::ArithmeticOverflow));
    return Result::success(std::move(block));
}

ImageResult<std::vector<StyledPaintOutlines>>
styledTextOutlines(const std::span<const TextFont> faces, const std::span<const StyledRun> runs,
                   const StyledTextBlock& block, const StyledTextOptions& options, const double x,
                   const double y, const PathCancellation& cancelled) {
    using Result = ImageResult<std::vector<StyledPaintOutlines>>;
    std::vector<OpenFace> open;
    std::vector<double> horizontal;
    std::vector<double> vertical;
    for (const auto& font : faces) {
        open.push_back(openFace(font));
        horizontal.push_back(static_cast<double>(std::visit(
            [&](const auto& face) {
                return font_access::fontScaleForEmPixelSize(face, options.horizontalSize);
            },
            open.back())));
        vertical.push_back(static_cast<double>(std::visit(
            [&](const auto& face) {
                return font_access::fontScaleForEmPixelSize(face, options.verticalSize);
            },
            open.back())));
    }
    std::vector<StyledPaintOutlines> paints;
    const auto paintPaths = [&](const std::uint32_t paint) -> std::vector<Path>& {
        for (auto& entry : paints)
            if (entry.paint == paint)
                return entry.paths;
        paints.push_back({paint, {}});
        return paints.back().paths;
    };
    const auto underlineOffset = 0.12 * options.verticalSize;
    const auto underlineThickness = std::max(0.06 * options.verticalSize, 0.5);
    for (std::size_t lineIndex = 0; lineIndex < block.lines.size(); ++lineIndex) {
        const auto& line = block.lines[lineIndex];
        const auto baseline = y + static_cast<double>(lineIndex) * block.lineStep + block.ascent;
        const auto left = x + line.offset;
        for (std::size_t index = 0; index < line.glyphs.size(); ++index) {
            if (cancelled && cancelled())
                return Result::failure(codeError(ImageErrorCode::InvalidState));
            const auto& glyph = line.glyphs[index];
            const auto& run = runs[glyph.run];
            if (run.face >= open.size())
                return Result::failure(codeError(ImageErrorCode::InvalidParameter));
            auto paths = std::visit(
                [&](const auto& face) { return font_access::fontGlyphOutlines(face, glyph.glyph); },
                open[run.face]);
            const auto scaleX = horizontal[run.face];
            const auto scaleY = vertical[run.face];
            const auto slant = run.oblique ? kSyntheticObliqueSlant : 0.0;
            const auto penX = left + glyph.x;
            const auto map = [&](const PathPoint p) -> PathPoint {
                return {penX + (p.x + slant * p.y) * scaleX, baseline - p.y * scaleY};
            };
            auto& target = paintPaths(run.paint);
            for (auto& path : paths) {
                for (auto& anchor : path.anchors) {
                    anchor.point = map(anchor.point);
                    if (anchor.inHandle)
                        anchor.inHandle = map(*anchor.inHandle);
                    if (anchor.outHandle)
                        anchor.outHandle = map(*anchor.outHandle);
                }
                target.push_back(std::move(path));
            }
        }
        // Underlines: one rectangle per stretch of consecutive underlined glyphs of one paint.
        for (std::size_t index = 0; index < line.glyphs.size();) {
            const auto& run = runs[line.glyphs[index].run];
            if (!run.underline) {
                ++index;
                continue;
            }
            auto last = index;
            while (last + 1 < line.glyphs.size() && runs[line.glyphs[last + 1].run].underline &&
                   runs[line.glyphs[last + 1].run].paint == run.paint)
                ++last;
            const auto from = left + line.glyphs[index].x;
            const auto to = left + line.glyphs[last].x + line.glyphs[last].advance;
            if (to > from) {
                auto rectangle = rectanglePath(to - from, underlineThickness);
                for (auto& anchor : rectangle.anchors)
                    anchor.point = {anchor.point.x + from,
                                    anchor.point.y + baseline + underlineOffset};
                paintPaths(run.paint).push_back(std::move(rectangle));
            }
            index = last + 1;
        }
    }
    return Result::success(std::move(paints));
}

} // namespace bloom::render
