// #752: WebVTT onto Bloom cues and back (docs/architecture/subtitles.md, "WebVTT"). The parse is
// webvtt.cpp, the specification's algorithm; this maps its cues, settings, regions and STYLE
// blocks onto document::Cue and reports what the mapping drops or approximates.
#include "internal.hpp"

#include <bloom/document/cue_markup.hpp>
#include <bloom/subtitles/webvtt.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>

namespace bloom::subtitles::detail {
namespace {

using document::CueAnchor;

enum class Row : std::uint8_t { Top, Middle, Bottom };
enum class Column : std::uint8_t { Left, Center, Right };

[[nodiscard]] CueAnchor anchorOf(const Row row, const Column column) {
    return static_cast<CueAnchor>(static_cast<std::size_t>(row) * 3 + static_cast<std::size_t>(column));
}
[[nodiscard]] Row rowOf(const CueAnchor anchor) {
    return static_cast<Row>(static_cast<std::size_t>(anchor) / 3);
}
[[nodiscard]] Column columnOf(const CueAnchor anchor) {
    return static_cast<Column>(static_cast<std::size_t>(anchor) % 3);
}

[[nodiscard]] std::string numberText(const double value) {
    // The shortest text that reads back as the same value, without an exponent.
    std::array<char, 64> buffer{};
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::fixed);
    if (error != std::errc{})
        return "0";
    std::string text(buffer.data(), end);
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0')
            text.pop_back();
        if (!text.empty() && text.back() == '.')
            text.pop_back();
    }
    return text;
}

[[nodiscard]] std::string stripComments(const std::string_view css) {
    std::string out;
    std::size_t index = 0;
    while (index < css.size()) {
        if (css.substr(index, 2) == "/*") {
            const auto end = css.find("*/", index + 2);
            index = end == std::string_view::npos ? css.size() : end + 2;
            continue;
        }
        out.push_back(css[index++]);
    }
    return out;
}

// One STYLE block's rules: ::cue(.class) { declarations } becomes a style named by the class.
void readStyleSheet(const webvtt::StyleSheet& sheet, Builder& builder) {
    const auto css = stripComments(sheet.text);
    std::size_t index = 0;
    while (index < css.size()) {
        const auto open = css.find('{', index);
        if (open == std::string::npos)
            break;
        const auto close = css.find('}', open);
        const auto selectors = trim(std::string_view(css).substr(index, open - index));
        const auto body = std::string_view(css).substr(
            open + 1, (close == std::string::npos ? css.size() : close) - open - 1);
        index = close == std::string::npos ? css.size() : close + 1;
        std::vector<std::string> names;
        std::size_t start = 0;
        const auto list = std::string(selectors);
        for (std::size_t cursor = 0; cursor <= list.size(); ++cursor) {
            if (cursor != list.size() && list[cursor] != ',')
                continue;
            const auto selector = trim(std::string_view(list).substr(start, cursor - start));
            start = cursor + 1;
            std::string_view inner;
            if (selector.starts_with("::cue(") && selector.ends_with(")"))
                inner = trim(selector.substr(6, selector.size() - 7));
            if (inner.starts_with("c."))
                inner.remove_prefix(1);
            if (inner.size() > 1 && inner.front() == '.' &&
                inner.find_first_of(" .#[:>") == 0 && inner.find_first_of(" #[:>", 1) == std::string_view::npos &&
                inner.find('.', 1) == std::string_view::npos) {
                names.emplace_back(inner.substr(1));
            } else {
                builder.note(codes::kDropped,
                             "the STYLE selector \"" + std::string(selector) +
                                 "\" (Bloom reads ::cue(.class) rules)",
                             sheet.line);
            }
        }
        if (names.empty())
            continue;
        document::CueStyle style;
        std::size_t from = 0;
        const auto declarations = std::string(body);
        for (std::size_t cursor = 0; cursor <= declarations.size(); ++cursor) {
            if (cursor != declarations.size() && declarations[cursor] != ';')
                continue;
            const auto declaration = trim(std::string_view(declarations).substr(from, cursor - from));
            from = cursor + 1;
            if (declaration.empty())
                continue;
            const auto colon = declaration.find(':');
            if (colon == std::string_view::npos)
                continue;
            const auto property = lower(trim(declaration.substr(0, colon)));
            auto value = trim(declaration.substr(colon + 1));
            if (value.ends_with("!important"))
                value = trim(value.substr(0, value.size() - 10));
            const auto valueLower = lower(value);
            bool read = true;
            if (property == "color") {
                style.color = parseCssColor(value);
                read = style.color.has_value();
            } else if (property == "background-color" || property == "background") {
                style.backgroundColor = parseCssColor(value);
                read = style.backgroundColor.has_value();
            } else if (property == "font-style") {
                style.italic = valueLower == "italic" || valueLower == "oblique";
                read = style.italic.value() || valueLower == "normal";
            } else if (property == "font-weight") {
                int weight = 0;
                const auto [end, error] =
                    std::from_chars(valueLower.data(), valueLower.data() + valueLower.size(), weight);
                const bool numeric = error == std::errc{} && end == valueLower.data() + valueLower.size();
                style.bold = valueLower == "bold" || valueLower == "bolder" || (numeric && weight >= 600);
                read = style.bold.value() || valueLower == "normal" || numeric;
            } else if (property == "text-decoration" || property == "text-decoration-line") {
                style.underline = valueLower.find("underline") != std::string::npos;
                read = style.underline.value() || valueLower == "none";
            } else if (property == "font-family") {
                auto family = trim(value.substr(0, value.find(',')));
                if (family.size() >= 2 && (family.front() == '"' || family.front() == '\''))
                    family = family.substr(1, family.size() - 2);
                const auto generic = lower(family);
                if (generic != "sans-serif" && generic != "serif" && generic != "monospace" &&
                    generic != "cursive" && generic != "fantasy" && generic != "system-ui" &&
                    !family.empty())
                    style.fontFamily = std::string(family);
            } else if (property == "font-size" && valueLower.ends_with("vh")) {
                double size = 0.0;
                const auto [end, error] = std::from_chars(
                    valueLower.data(), valueLower.data() + valueLower.size() - 2, size);
                read = error == std::errc{} && end == valueLower.data() + valueLower.size() - 2 &&
                       size > 0.0 && size <= 100.0;
                if (read)
                    style.fontSize = size / 100.0;
            } else {
                read = false;
            }
            if (!read)
                builder.note(codes::kDropped, "the CSS property " + property + ": " + std::string(value),
                             sheet.line);
        }
        for (const auto& name : names) {
            if (document::isBuiltInCueStyleName(name)) {
                builder.note(codes::kDropped,
                             "a STYLE rule for the built-in class ." + name, sheet.line);
                continue;
            }
            auto named = style;
            named.name = name;
            builder.addStyle(std::move(named));
        }
    }
}

// The first <v name> annotation in a cue's text.
[[nodiscard]] std::string voiceOf(const std::vector<document::cue_markup::Node>& nodes) {
    for (const auto& node : nodes) {
        if (node.kind == document::cue_markup::NodeKind::Voice && !node.text.empty())
            return node.text;
        if (auto inner = voiceOf(node.children); !inner.empty())
            return inner;
    }
    return {};
}

} // namespace

LoadOutcome readWebVtt(const std::string_view text, Builder& builder, std::string encoding) {
    const auto file = webvtt::parse(text);
    if (!file.signatureValid)
        return builder.failure(1, 1, codes::kUnrecognized,
                               "line 1: a WebVTT file starts with WEBVTT");
    // The header's "Language:" line (written by HLS packagers and many tools).
    for (const auto& line : splitLines(text)) {
        if (line.number == 1)
            continue;
        if (trim(line.text).empty())
            break;
        const auto entry = trim(line.text);
        if (lower(entry.substr(0, 9)) == "language:")
            builder.setLanguage(std::string(trim(entry.substr(9))));
    }
    for (const auto& ignored : file.ignored) {
        if (ignored.note)
            builder.note(codes::kDropped, "NOTE blocks (comments)", ignored.line);
        else if (ignored.firstLine.find("-->") != std::string::npos ||
                 !trim(ignored.firstLine).empty())
            builder.report(ignored.line, 1, codes::kSkippedCue,
                           "line " + std::to_string(ignored.line) +
                               ": the block does not start with a timing line "
                               "\"00:00:01.000 --> 00:00:02.000\"; it is skipped");
    }
    for (const auto& sheet : file.styles)
        readStyleSheet(sheet, builder);
    for (const auto& cue : file.cues) {
        if (builder.stopRequested())
            break;
        ReadCue read;
        read.start = milliseconds(cue.startMilliseconds);
        read.end = milliseconds(cue.endMilliseconds);
        read.text = cue.text;
        read.line = cue.timingLine;
        read.column = 1;
        if (!cue.id.empty())
            builder.note(codes::kDropped, "cue identifiers", cue.timingLine);
        read.speaker = voiceOf(document::cue_markup::parse(cue.text).tree.children);
        for (const auto& setting : cue.skippedSettings)
            builder.note(codes::kDropped, "the cue setting \"" + setting + "\"", cue.timingLine);
        if (cue.vertical != webvtt::Direction::Horizontal)
            builder.note(codes::kApproximated, "vertical text (drawn horizontally)", cue.timingLine);
        auto& placement = read.placement;
        // Text alignment.
        switch (cue.align) {
        case webvtt::TextAlign::Start:
        case webvtt::TextAlign::Left:
            placement.textAlign = document::CueTextAlign::Left;
            break;
        case webvtt::TextAlign::End:
        case webvtt::TextAlign::Right:
            placement.textAlign = document::CueTextAlign::Right;
            break;
        case webvtt::TextAlign::Center:
            break;
        }
        std::optional<Row> row;
        std::optional<Column> column;
        if (cue.region) {
            const auto& region = file.regions[*cue.region];
            // A region places its lines at its viewport anchor, its width the region's.
            row = region.anchorY <= 33.0 ? Row::Top : region.anchorY >= 67.0 ? Row::Bottom : Row::Middle;
            column = region.anchorX <= 33.0 ? Column::Left
                     : region.anchorX >= 67.0 ? Column::Right
                                              : Column::Center;
            placement.positionX = region.viewportAnchorX / 100.0;
            placement.positionY = region.viewportAnchorY / 100.0;
            placement.width = region.width / 100.0;
            builder.note(codes::kApproximated, "regions (placed at their viewport anchor)",
                         cue.timingLine);
            if (region.scrollUp)
                builder.note(codes::kDropped, "region scrolling", cue.timingLine);
        }
        if (cue.line) {
            if (cue.snapToLines) {
                const auto whole = std::trunc(*cue.line);
                if (whole != *cue.line || std::abs(whole) > 1000.0)
                    builder.note(codes::kApproximated, "a fractional or very large line number",
                                 cue.timingLine);
                const auto clamped = std::clamp(whole, -1000.0, 1000.0);
                if (clamped >= 0.0) {
                    row = Row::Top;
                    placement.lineOffset = static_cast<std::int64_t>(clamped);
                } else {
                    row = Row::Bottom;
                    placement.lineOffset = static_cast<std::int64_t>(-clamped) - 1;
                }
            } else {
                placement.positionY = *cue.line / 100.0;
                row = cue.lineAlign == webvtt::LineAlign::Start    ? Row::Top
                      : cue.lineAlign == webvtt::LineAlign::Center ? Row::Middle
                                                                   : Row::Bottom;
            }
        }
        if (cue.position) {
            placement.positionX = *cue.position / 100.0;
            switch (cue.positionAlign) {
            case webvtt::PositionAlign::LineLeft:
                column = Column::Left;
                break;
            case webvtt::PositionAlign::Center:
                column = Column::Center;
                break;
            case webvtt::PositionAlign::LineRight:
                column = Column::Right;
                break;
            case webvtt::PositionAlign::Auto:
                column = placement.textAlign == document::CueTextAlign::Left    ? Column::Left
                         : placement.textAlign == document::CueTextAlign::Right ? Column::Right
                                                                                : Column::Center;
                break;
            }
        }
        if (cue.size != 100.0)
            placement.width = cue.size / 100.0;
        if (row || column) {
            if (!column)
                column = placement.textAlign == document::CueTextAlign::Left    ? Column::Left
                         : placement.textAlign == document::CueTextAlign::Right ? Column::Right
                                                                                : Column::Center;
            placement.anchor = anchorOf(row.value_or(Row::Bottom), *column);
        }
        if (placement.positionY && (*placement.positionY < -1.0 || *placement.positionY > 2.0))
            placement.positionY = std::clamp(*placement.positionY, -1.0, 2.0);
        builder.addCue(std::move(read));
    }
    return builder.finish(Format::WebVtt, std::move(encoding));
}

namespace {

[[nodiscard]] std::string cssIdentifier(const std::string_view name) {
    std::string out;
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                           static_cast<unsigned char>(c) >= 0x80;
        if (!plain)
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

[[nodiscard]] std::string cssColor(const core::Color4d& color) { return "#" + hexName(color); }

[[nodiscard]] std::string settingsOf(const document::Cue& cue, std::vector<Report>& reports) {
    const auto& placement = cue.placement;
    std::string out;
    const auto add = [&](const std::string& setting) { out += " " + setting; };
    if (placement.anchor || placement.lineOffset || placement.positionY) {
        const auto row = placement.anchor ? rowOf(*placement.anchor) : Row::Bottom;
        if (placement.positionY) {
            const auto align = row == Row::Top ? "start" : row == Row::Middle ? "center" : "end";
            add("line:" + numberText(std::clamp(*placement.positionY, 0.0, 1.0) * 100.0) + "%," +
                align);
        } else if (placement.lineOffset || row != Row::Bottom) {
            const auto offset = placement.lineOffset.value_or(0);
            if (row == Row::Bottom)
                add("line:" + std::to_string(-offset - 1));
            else if (row == Row::Top)
                add("line:" + std::to_string(offset));
            else
                add("line:50%,center");
        }
    }
    // The column a cue with no position takes from its text alignment, as the reader reads it.
    const auto impliedColumn = placement.textAlign == document::CueTextAlign::Left    ? Column::Left
                               : placement.textAlign == document::CueTextAlign::Right ? Column::Right
                                                                                      : Column::Center;
    if (placement.positionX ||
        (placement.anchor && columnOf(*placement.anchor) != impliedColumn)) {
        const auto column = placement.anchor ? columnOf(*placement.anchor) : Column::Center;
        const auto x = placement.positionX.value_or(column == Column::Left    ? 0.0
                                                    : column == Column::Right ? 1.0
                                                                              : 0.5);
        const auto align = column == Column::Left    ? "line-left"
                           : column == Column::Right ? "line-right"
                                                     : "center";
        add("position:" + numberText(std::clamp(x, 0.0, 1.0) * 100.0) + "%," + align);
    }
    if (placement.width)
        add("size:" + numberText(std::clamp(*placement.width, 0.0, 1.0) * 100.0) + "%");
    if (placement.textAlign)
        add("align:" + std::string(document::cueTextAlignName(*placement.textAlign)));
    if (placement.positionX && (*placement.positionX < 0.0 || *placement.positionX > 1.0))
        reports.push_back({0, 0, std::string(codes::kApproximated),
                           "cue " + std::to_string(cue.id) +
                               ": a position outside the frame is written at its edge"});
    return out;
}

} // namespace

std::string writeWebVtt(const document::CueListValue& cues, std::vector<Report>& reports) {
    std::string out = "WEBVTT\n";
    if (cues.language != "und")
        out += "Language: " + cues.language + "\n";
    out += "\n";
    if (!cues.styles.empty()) {
        out += "STYLE\n";
        for (const auto& style : cues.styles) {
            std::string rule = "::cue(." + cssIdentifier(style.name) + ") {";
            if (style.color)
                rule += " color: " + cssColor(*style.color) + ";";
            if (style.backgroundColor)
                rule += " background-color: " + cssColor(*style.backgroundColor) + ";";
            if (style.italic)
                rule += std::string(" font-style: ") + (*style.italic ? "italic" : "normal") + ";";
            if (style.bold)
                rule += std::string(" font-weight: ") + (*style.bold ? "bold" : "normal") + ";";
            if (style.underline)
                rule += std::string(" text-decoration: ") + (*style.underline ? "underline" : "none") +
                        ";";
            if (style.fontFamily)
                rule += " font-family: \"" + *style.fontFamily + "\";";
            if (style.fontSize)
                rule += " font-size: " + numberText(*style.fontSize * 100.0) + "vh;";
            if (style.outlineColor || style.outlineWidth)
                reports.push_back({0, 0, std::string(codes::kDropped),
                                   "style \"" + style.name + "\": WebVTT has no text outline"});
            out += rule + " }\n";
        }
        out += "\n";
    }
    for (const auto& cue : cues.cues) {
        out += document::cueTimestampText(cue->start) + " --> " +
               document::cueTimestampText(cue->end) + settingsOf(*cue, reports) + "\n";
        std::string text = cue->text;
        const auto parsed = document::cue_markup::parse(text);
        if (!cue->speaker.empty() && voiceOf(parsed.tree.children).empty())
            text = "<v " + document::cue_markup::escapeText(cue->speaker) + ">" + text + "</v>";
        if (!cue->style.empty())
            text = "<c." + cue->style + ">" + text + "</c>";
        // A blank line would end the cue; a line holding "-->" would start another.
        std::string safe;
        for (const auto& line : splitLines(text)) {
            if (!safe.empty())
                safe.push_back('\n');
            if (line.text.empty()) {
                safe += "&#32;"; // nothing is lost: a reference to a space
                continue;
            }
            std::string part(line.text);
            for (auto at = part.find("-->"); at != std::string::npos; at = part.find("-->", at))
                part.replace(at + 2, 1, "&gt;");
            safe += part;
        }
        out += safe + "\n\n";
    }
    // A blank line between blocks, none after the last.
    if (out.ends_with("\n\n"))
        out.pop_back();
    return out;
}

} // namespace bloom::subtitles::detail
