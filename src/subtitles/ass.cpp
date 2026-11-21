// #752: Advanced SubStation Alpha and SubStation Alpha (.ass, .ssa), read as a reported subset
// (docs/architecture/subtitles.md, "ASS/SSA"). [Script Info] gives the script resolution that
// positions and sizes are scaled from, [V4+ Styles] / [V4 Styles] the style table, and Dialogue
// events the cues. The override tags \i \b \u \c \1c \3c \an \a \pos \N \n \h and \k are kept (\k
// as cue timestamps); every other tag, the Effect field, layers, shadows and drawings are reported
// as dropped. Bloom does not write ASS.
#include "internal.hpp"

#include <bloom/core/int128.hpp>
#include <bloom/document/cue_markup.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <string>

namespace bloom::subtitles::detail {
namespace {

[[nodiscard]] std::vector<std::string_view> splitFields(const std::string_view text,
                                                        const std::size_t count) {
    // The last field takes the rest of the line, commas and all.
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (fields.size() + 1 < count) {
        const auto comma = text.find(',', start);
        if (comma == std::string_view::npos)
            break;
        fields.push_back(trim(text.substr(start, comma - start)));
        start = comma + 1;
    }
    fields.push_back(fields.size() + 1 < count ? trim(text.substr(start)) : text.substr(start));
    return fields;
}

[[nodiscard]] std::optional<double> number(const std::string_view raw) {
    const auto text = trim(raw);
    double value = 0.0;
    const auto* first = text.data();
    if (!text.empty() && text.front() == '+')
        ++first;
    const auto [end, error] = std::from_chars(first, text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

// &HAABBGGRR (or &HBBGGRR, with or without the closing &), or the decimal form SSA writes.
[[nodiscard]] std::optional<core::Color4d> assColor(const std::string_view raw) {
    auto text = trim(raw);
    std::uint64_t value = 0;
    if (text.size() >= 2 && text[0] == '&' && (text[1] == 'H' || text[1] == 'h')) {
        text.remove_prefix(2);
        while (!text.empty() && text.back() == '&')
            text.remove_suffix(1);
        if (text.empty() || text.size() > 8)
            return std::nullopt;
        if (std::from_chars(text.data(), text.data() + text.size(), value, 16).ptr !=
            text.data() + text.size())
            return std::nullopt;
    } else {
        std::int64_t signedValue = 0;
        if (text.empty() || std::from_chars(text.data(), text.data() + text.size(), signedValue)
                                    .ptr != text.data() + text.size())
            return std::nullopt;
        value = static_cast<std::uint64_t>(signedValue) & 0xFFFFFFFFU;
    }
    const auto channel = [&](const unsigned shift) {
        return static_cast<double>((value >> shift) & 0xFFU) / 255.0;
    };
    return core::Color4d{channel(0), channel(8), channel(16), 1.0 - channel(24)};
}

// H:MM:SS.cc, the fraction any number of digits up to nine.
[[nodiscard]] std::optional<core::RationalTime> assTime(const std::string_view raw) {
    const auto text = trim(raw);
    const auto first = text.find(':');
    const auto second = first == std::string_view::npos ? first : text.find(':', first + 1);
    if (second == std::string_view::npos)
        return std::nullopt;
    const auto dot = text.find('.', second);
    const auto whole = [](const std::string_view part, std::int64_t& value) {
        return !part.empty() && part.size() <= 9 &&
               std::from_chars(part.data(), part.data() + part.size(), value).ptr ==
                   part.data() + part.size() &&
               value >= 0;
    };
    std::int64_t hours = 0;
    std::int64_t minutes = 0;
    std::int64_t seconds = 0;
    std::int64_t fraction = 0;
    std::int64_t scale = 1;
    if (!whole(text.substr(0, first), hours) ||
        !whole(text.substr(first + 1, second - first - 1), minutes) ||
        !whole(text.substr(second + 1, dot == std::string_view::npos ? std::string_view::npos
                                                                      : dot - second - 1),
               seconds) ||
        minutes > 59 || seconds > 59)
        return std::nullopt;
    if (dot != std::string_view::npos) {
        const auto digits = text.substr(dot + 1);
        if (!whole(digits, fraction))
            return std::nullopt;
        for (std::size_t index = 0; index < digits.size(); ++index)
            scale *= 10;
    }
    return core::RationalTime::create(((hours * 60 + minutes) * 60 + seconds) * scale + fraction,
                                      scale);
}

struct AssStyle {
    std::string name; // the name in the list
    std::optional<document::CueAnchor> anchor;
    double marginL = 0.0;
    double marginR = 0.0;
    double marginV = 0.0;
};

// Numpad alignment (ASS) or the legacy form (SSA, and the \a tag): 1..3 bottom, 5..7 top,
// 9..11 middle.
[[nodiscard]] std::optional<document::CueAnchor> anchorFrom(const int value, const bool legacy) {
    int row = 0;
    int column = 0;
    if (legacy) {
        if (value >= 1 && value <= 3) {
            row = 2;
            column = value - 1;
        } else if (value >= 5 && value <= 7) {
            row = 0;
            column = value - 5;
        } else if (value >= 9 && value <= 11) {
            row = 1;
            column = value - 9;
        } else {
            return std::nullopt;
        }
    } else {
        if (value < 1 || value > 9)
            return std::nullopt;
        row = value <= 3 ? 2 : value <= 6 ? 1 : 0;
        column = (value - 1) % 3;
    }
    return static_cast<document::CueAnchor>(row * 3 + column);
}

class AssReader {
  public:
    AssReader(const std::string_view text, Builder& builder) : text_(text), builder_(builder) {}

    void run() {
        enum class Section { None, Info, Styles, Events, Other };
        Section section = Section::None;
        for (const auto& line : splitLines(text_)) {
            if (builder_.stopRequested())
                return;
            const auto content = trim(line.text);
            if (content.empty() || content.front() == ';' || content.starts_with("!:"))
                continue;
            if (content.front() == '[' && content.back() == ']') {
                const auto name = lower(content.substr(1, content.size() - 2));
                if (name == "script info") {
                    section = Section::Info;
                } else if (name == "v4+ styles" || name == "v4 styles" || name == "v4 styles+") {
                    section = Section::Styles;
                    legacyStyles_ = name == "v4 styles";
                    styleFormat_.clear();
                } else if (name == "events") {
                    section = Section::Events;
                    eventFormat_.clear();
                } else {
                    section = Section::Other;
                    if (name == "fonts" || name == "graphics")
                        builder_.note(codes::kDropped, "the embedded [" + std::string(content.substr(1, content.size() - 2)) + "] section", line.number);
                    else if (!name.starts_with("aegisub"))
                        builder_.note(codes::kDropped, "the " + std::string(content) + " section",
                                      line.number);
                }
                continue;
            }
            const auto colon = content.find(':');
            if (colon == std::string_view::npos)
                continue;
            const auto key = trim(content.substr(0, colon));
            const auto value = trim(content.substr(colon + 1));
            switch (section) {
            case Section::Info:
                info(key, value, line.number);
                break;
            case Section::Styles:
                if (equalsIgnoringCase(key, "Format"))
                    styleFormat_ = names(value);
                else if (equalsIgnoringCase(key, "Style"))
                    style(value, line.number);
                break;
            case Section::Events:
                if (equalsIgnoringCase(key, "Format"))
                    eventFormat_ = names(value);
                else if (equalsIgnoringCase(key, "Dialogue"))
                    event(content.substr(colon + 1), line.number);
                else if (!equalsIgnoringCase(key, "Comment"))
                    builder_.note(codes::kDropped, std::string(key) + " events", line.number);
                break;
            case Section::None:
            case Section::Other:
                break;
            }
        }
    }

  private:
    static std::vector<std::string> names(const std::string_view value) {
        std::vector<std::string> out;
        std::size_t start = 0;
        for (std::size_t index = 0; index <= value.size(); ++index)
            if (index == value.size() || value[index] == ',') {
                out.push_back(lower(trim(value.substr(start, index - start))));
                start = index + 1;
            }
        return out;
    }

    void info(const std::string_view key, const std::string_view value, const std::size_t line) {
        if (equalsIgnoringCase(key, "PlayResX")) {
            if (const auto x = number(value); x && *x > 0)
                playResX_ = *x;
        } else if (equalsIgnoringCase(key, "PlayResY")) {
            if (const auto y = number(value); y && *y > 0)
                playResY_ = *y;
        } else if (equalsIgnoringCase(key, "WrapStyle")) {
            if (const auto wrap = number(value))
                wrapStyle_ = static_cast<int>(*wrap);
        } else if (equalsIgnoringCase(key, "Language")) {
            if (document::isWellFormedLanguageTag(value))
                builder_.setLanguage(std::string(value));
        } else if (equalsIgnoringCase(key, "ScriptType")) {
            if (lower(value) == "v4.00" && !scriptTypeSeen_)
                legacyStyles_ = true;
            scriptTypeSeen_ = true;
        } else if (equalsIgnoringCase(key, "ScaledBorderAndShadow")) {
            if (lower(value) == "no")
                builder_.note(codes::kApproximated,
                              "ScaledBorderAndShadow: no (outlines are scaled with the script)",
                              line);
        }
    }

    // The script resolution: VSFilter's defaults when the script names none.
    [[nodiscard]] double resX() const {
        if (playResX_ > 0)
            return playResX_;
        return playResY_ > 0 ? playResY_ * 4.0 / 3.0 : 384.0;
    }
    [[nodiscard]] double resY() const {
        if (playResY_ > 0)
            return playResY_;
        return playResX_ > 0 ? playResX_ * 3.0 / 4.0 : 288.0;
    }

    [[nodiscard]] std::string field(const std::vector<std::string>& format,
                                    const std::vector<std::string_view>& fields,
                                    const std::string_view name) const {
        for (std::size_t index = 0; index < format.size() && index < fields.size(); ++index)
            if (format[index] == name)
                return std::string(fields[index]);
        return {};
    }

    void style(const std::string_view value, const std::size_t line) {
        if (styleFormat_.empty())
            styleFormat_ = legacyStyles_
                               ? names("Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
                                       "TertiaryColour, BackColour, Bold, Italic, BorderStyle, "
                                       "Outline, Shadow, Alignment, MarginL, MarginR, MarginV, "
                                       "AlphaLevel, Encoding")
                               : names("Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
                                       "OutlineColour, BackColour, Bold, Italic, Underline, "
                                       "StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, "
                                       "Outline, Shadow, Alignment, MarginL, MarginR, MarginV, "
                                       "Encoding");
        const auto fields = splitFields(value, styleFormat_.size());
        const auto get = [&](const std::string_view name) { return field(styleFormat_, fields, name); };
        auto original = get("name");
        if (!original.empty() && original.front() == '*')
            original.erase(0, 1);
        if (original.empty()) {
            builder_.report(line, 1, codes::kDropped,
                            "line " + std::to_string(line) + ": a style with no name is skipped");
            return;
        }
        document::CueStyle out;
        out.name = original;
        if (document::isBuiltInCueStyleName(out.name) || out.name.size() > document::kMaximumCueNameBytes) {
            out.name = "ass-" + std::to_string(styles_.size() + 1);
            builder_.report(line, 1, codes::kApproximated,
                            "line " + std::to_string(line) + ": the style \"" + original +
                                "\" is renamed \"" + out.name + "\" (its name is a WebVTT class "
                                "Bloom keeps, or too long)");
        }
        if (const auto family = get("fontname"); !family.empty())
            out.fontFamily = family;
        if (const auto size = number(get("fontsize")); size && *size > 0)
            out.fontSize = *size / resY();
        if (const auto color = assColor(get("primarycolour")))
            out.color = color;
        const auto flag = [&](const std::string_view name) -> std::optional<bool> {
            const auto text = get(name);
            if (text.empty())
                return std::nullopt;
            const auto parsed = number(text);
            if (!parsed)
                return std::nullopt;
            return *parsed == -1.0 || *parsed == 1.0 || *parsed >= 600.0;
        };
        out.bold = flag("bold");
        out.italic = flag("italic");
        if (!legacyStyles_)
            out.underline = flag("underline");
        if (flag("strikeout").value_or(false))
            builder_.note(codes::kDropped, "strike-out styles", line);
        const auto border = number(get("borderstyle")).value_or(1.0);
        const auto outlineColor =
            assColor(get(legacyStyles_ ? "backcolour" : "outlinecolour"));
        const auto outline = number(get("outline")).value_or(0.0);
        if (border == 3.0) {
            // An opaque box in the outline color.
            out.backgroundColor = outlineColor;
        } else if (outline > 0.0) {
            out.outlineColor = outlineColor;
            out.outlineWidth = outline / resY();
        }
        if (number(get("shadow")).value_or(0.0) > 0.0)
            builder_.note(codes::kDropped, "shadows", line);
        if (number(get("scalex")).value_or(100.0) != 100.0 ||
            number(get("scaley")).value_or(100.0) != 100.0)
            builder_.note(codes::kDropped, "style scaling (ScaleX, ScaleY)", line);
        if (number(get("spacing")).value_or(0.0) != 0.0)
            builder_.note(codes::kDropped, "style letter spacing (the node's is used)", line);
        if (number(get("angle")).value_or(0.0) != 0.0)
            builder_.note(codes::kDropped, "style rotation (Angle)", line);
        AssStyle entry;
        entry.name = out.name;
        if (const auto alignment = number(get("alignment")))
            entry.anchor = anchorFrom(static_cast<int>(*alignment), legacyStyles_);
        entry.marginL = number(get("marginl")).value_or(0.0);
        entry.marginR = number(get("marginr")).value_or(0.0);
        entry.marginV = number(get("marginv")).value_or(0.0);
        styles_[original] = entry;
        builder_.addStyle(std::move(out));
    }

    // One override block's tags.
    struct State {
        bool italic = false;
        bool bold = false;
        bool underline = false;
        std::string colorClass;
        std::string outlineClass;
        friend bool operator==(const State&, const State&) = default;
    };

    std::string outlineStyle(const core::Color4d& color) {
        auto name = "ass-outline-" + hexName(color);
        if (!builder_.hasStyle(name)) {
            document::CueStyle style;
            style.name = name;
            style.outlineColor = color;
            builder_.addStyle(std::move(style));
        }
        return name;
    }

    void event(const std::string_view value, const std::size_t line) {
        if (eventFormat_.empty())
            eventFormat_ = names(legacyStyles_
                                     ? "Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, "
                                       "Effect, Text"
                                     : "Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
                                       "Effect, Text");
        if (eventFormat_.back() != "text") {
            builder_.report(line, 1, codes::kSkippedCue,
                            "line " + std::to_string(line) +
                                ": the [Events] Format does not end with Text; the event is skipped");
            return;
        }
        const auto fields = splitFields(value, eventFormat_.size());
        if (fields.size() != eventFormat_.size()) {
            builder_.report(line, 1, codes::kSkippedCue,
                            "line " + std::to_string(line) + ": the event has " +
                                std::to_string(fields.size()) + " fields, its Format names " +
                                std::to_string(eventFormat_.size()) + "; it is skipped");
            return;
        }
        const auto get = [&](const std::string_view name) { return field(eventFormat_, fields, name); };
        const auto start = assTime(get("start"));
        const auto end = assTime(get("end"));
        if (!start || !end) {
            builder_.report(line, 1, codes::kSkippedCue,
                            "line " + std::to_string(line) +
                                ": a start or end time that is not H:MM:SS.cc; the event is skipped");
            return;
        }
        if (const auto layer = number(get("layer")); layer && *layer != 0.0)
            builder_.note(codes::kDropped, "event layers (the cues draw in time order)", line);
        if (!trim(get("effect")).empty())
            builder_.note(codes::kDropped, "the Effect field", line);

        ReadCue cue;
        cue.start = *start;
        cue.end = *end;
        cue.line = line;
        cue.column = 1;
        cue.speaker = std::string(trim(get("name")));
        auto styleName = std::string(trim(get("style")));
        if (!styleName.empty() && styleName.front() == '*')
            styleName.erase(0, 1);
        const AssStyle* style = nullptr;
        if (const auto found = styles_.find(styleName); found != styles_.end()) {
            style = &found->second;
            cue.style = style->name;
        } else if (!styleName.empty() && !equalsIgnoringCase(styleName, "Default")) {
            cue.style = styleName; // the builder reports it as undefined
        }

        // Margins the event sets itself place the cue; the style's are the node's margins.
        const auto margin = [&](const std::string_view name) {
            return number(get(name)).value_or(0.0);
        };
        const auto marginL = margin("marginl");
        const auto marginR = margin("marginr");
        const auto marginV = margin("marginv");
        std::optional<document::CueAnchor> anchor = style != nullptr ? style->anchor : std::nullopt;
        std::optional<std::pair<double, double>> position;
        cue.text = text(get("text"), line, *start, *end, anchor, position);
        if (anchor && *anchor != document::CueAnchor::Bottom)
            cue.placement.anchor = anchor;
        if (position) {
            cue.placement.anchor = anchor.value_or(document::CueAnchor::Bottom);
            cue.placement.positionX = std::clamp(position->first / resX(), -1.0, 2.0);
            cue.placement.positionY = std::clamp(position->second / resY(), -1.0, 2.0);
        } else if (marginL != 0.0 || marginR != 0.0 || marginV != 0.0) {
            const auto effective = [&](const double own, const double fallback) {
                return own != 0.0 ? own : fallback;
            };
            const auto left = effective(marginL, style != nullptr ? style->marginL : 0.0);
            const auto right = effective(marginR, style != nullptr ? style->marginR : 0.0);
            const auto vertical = effective(marginV, style != nullptr ? style->marginV : 0.0);
            const auto at = anchor.value_or(document::CueAnchor::Bottom);
            const auto row = static_cast<int>(at) / 3;
            const auto column = static_cast<int>(at) % 3;
            cue.placement.anchor = at;
            cue.placement.positionX = std::clamp(
                column == 0   ? left / resX()
                : column == 2 ? 1.0 - right / resX()
                              : 0.5 + (left - right) / (2.0 * resX()),
                -1.0, 2.0);
            cue.placement.positionY = std::clamp(
                row == 0 ? vertical / resY() : row == 2 ? 1.0 - vertical / resY() : 0.5, -1.0, 2.0);
            const auto width = 1.0 - (left + right) / resX();
            if (width > 0.0 && width <= 1.0)
                cue.placement.width = width;
        }
        builder_.addCue(std::move(cue));
    }

    // ASS event text to cue markup.
    std::string text(const std::string_view raw, const std::size_t line,
                     const core::RationalTime start, const core::RationalTime end,
                     std::optional<document::CueAnchor>& anchor,
                     std::optional<std::pair<double, double>>& position) {
        std::string out;
        State open;
        State current;
        bool drawing = false;
        bool anchorSet = false;
        // Karaoke: the next syllable's start, in centiseconds after the cue's start.
        std::int64_t karaoke = 0;
        bool karaokeSeen = false;
        std::optional<std::int64_t> pendingKaraoke;

        const auto closeAll = [&] {
            if (!open.outlineClass.empty())
                out += "</c>";
            if (!open.colorClass.empty())
                out += "</c>";
            if (open.underline)
                out += "</u>";
            if (open.bold)
                out += "</b>";
            if (open.italic)
                out += "</i>";
            open = State{};
        };
        const auto sync = [&] {
            if (open == current)
                return;
            closeAll();
            if (current.italic)
                out += "<i>";
            if (current.bold)
                out += "<b>";
            if (current.underline)
                out += "<u>";
            if (!current.colorClass.empty())
                out += "<c." + current.colorClass + ">";
            if (!current.outlineClass.empty())
                out += "<c." + current.outlineClass + ">";
            open = current;
        };
        const auto character = [&](const std::string_view piece) {
            if (drawing)
                return;
            if (pendingKaraoke) {
                // A timestamp strictly inside the cue, at millisecond precision.
                const auto at = core::RationalTime::create(
                    static_cast<std::int64_t>(core::Int128(start.numerator()) * core::Int128(100) +
                                              core::Int128(*pendingKaraoke) *
                                                  core::Int128(start.denominator())),
                    static_cast<std::int64_t>(core::Int128(start.denominator()) * core::Int128(100)));
                if (at && *at > start && *at < end) {
                    const auto ms = static_cast<std::int64_t>(
                        core::Int128(at->numerator()) * core::Int128(1000) /
                        core::Int128(at->denominator()));
                    closeAll();
                    out += "<" + document::cue_markup::timestampText(ms) + ">";
                }
                pendingKaraoke.reset();
            }
            sync();
            for (const char c : piece) {
                if (c == '&')
                    out += "&amp;";
                else if (c == '<')
                    out += "&lt;";
                else if (c == '>')
                    out += "&gt;";
                else
                    out.push_back(c);
            }
        };

        std::size_t index = 0;
        while (index < raw.size()) {
            const char c = raw[index];
            if (c == '{') {
                const auto close = raw.find('}', index);
                if (close == std::string_view::npos) {
                    character(raw.substr(index));
                    break;
                }
                overrides(raw.substr(index + 1, close - index - 1), line, current, drawing,
                          anchor, anchorSet, position, karaoke, karaokeSeen, pendingKaraoke);
                index = close + 1;
                continue;
            }
            if (c == '\\' && index + 1 < raw.size()) {
                const char next = raw[index + 1];
                if (next == 'N' || (next == 'n' && wrapStyle_ == 2)) {
                    if (!drawing) {
                        closeAll();
                        out.push_back('\n');
                    }
                    index += 2;
                    continue;
                }
                if (next == 'n') {
                    character(" ");
                    index += 2;
                    continue;
                }
                if (next == 'h') {
                    character("\xC2\xA0");
                    index += 2;
                    continue;
                }
            }
            character(raw.substr(index, 1));
            ++index;
        }
        closeAll();
        // Trailing and leading line breaks carry nothing.
        while (!out.empty() && out.back() == '\n')
            out.pop_back();
        while (!out.empty() && out.front() == '\n')
            out.erase(0, 1);
        return out;
    }

    void overrides(const std::string_view block, const std::size_t line, State& state,
                   bool& drawing, std::optional<document::CueAnchor>& anchor, bool& anchorSet,
                   std::optional<std::pair<double, double>>& position, std::int64_t& karaoke,
                   bool& karaokeSeen, std::optional<std::int64_t>& pendingKaraoke) {
        if (block.find('\\') == std::string_view::npos)
            return; // a comment
        std::size_t index = block.find('\\');
        while (index < block.size()) {
            // One tag: a backslash, a name, then arguments up to the next backslash outside
            // parentheses.
            std::size_t end = index + 1;
            int depth = 0;
            while (end < block.size() && (depth > 0 || block[end] != '\\')) {
                if (block[end] == '(')
                    ++depth;
                else if (block[end] == ')')
                    depth = std::max(0, depth - 1);
                ++end;
            }
            tag(trim(block.substr(index + 1, end - index - 1)), line, state, drawing, anchor,
                anchorSet, position, karaoke, karaokeSeen, pendingKaraoke);
            index = end;
        }
    }

    void tag(const std::string_view text, const std::size_t line, State& state, bool& drawing,
             std::optional<document::CueAnchor>& anchor, bool& anchorSet,
             std::optional<std::pair<double, double>>& position, std::int64_t& karaoke,
             bool& karaokeSeen, std::optional<std::int64_t>& pendingKaraoke) {
        if (text.empty())
            return;
        // The name: a leading digit (\1c, \3c, \2a) and the letters after it. \fn and \r take
        // a name as their argument, written straight after the tag.
        const auto letter = [](const char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
        std::size_t nameEnd = text[0] >= '1' && text[0] <= '4' ? 1 : 0;
        while (nameEnd < text.size() && letter(text[nameEnd]))
            ++nameEnd;
        if (text.starts_with("fn"))
            nameEnd = 2;
        else if (text.front() == 'r' && nameEnd > 1)
            nameEnd = 1;
        const std::string name(text.substr(0, nameEnd));
        const auto argument = trim(text.substr(nameEnd));
        const auto flag = [&](const bool fallback) {
            if (argument.empty())
                return fallback;
            const auto value = number(argument);
            return value && (*value == 1.0 || *value >= 600.0 || *value == -1.0);
        };
        const auto drop = [&](const std::string_view what) {
            builder_.note(codes::kDropped, std::string(what), line);
        };
        if (name == "i") {
            state.italic = flag(false);
        } else if (name == "b") {
            state.bold = flag(false);
        } else if (name == "u") {
            state.underline = flag(false);
        } else if (name == "c" || name == "1c") {
            if (argument.empty())
                state.colorClass.clear();
            else if (const auto color = assColor(argument))
                state.colorClass = builder_.colorStyle("ass", *color);
        } else if (name == "3c") {
            if (argument.empty())
                state.outlineClass.clear();
            else if (const auto color = assColor(argument))
                state.outlineClass = outlineStyle(*color);
        } else if (name == "an" || name == "a") {
            if (const auto value = number(argument); value && !anchorSet) {
                if (const auto parsed = anchorFrom(static_cast<int>(*value), name == "a")) {
                    anchor = parsed;
                    anchorSet = true;
                }
            }
        } else if (name == "pos") {
            if (!position) {
                const auto inner = trim(argument);
                if (inner.size() >= 2 && inner.front() == '(' && inner.back() == ')') {
                    const auto comma = inner.find(',');
                    if (comma != std::string_view::npos) {
                        const auto x = number(inner.substr(1, comma - 1));
                        const auto y = number(inner.substr(comma + 1, inner.size() - comma - 2));
                        if (x && y)
                            position = std::pair{*x, *y};
                    }
                }
            }
        } else if (name == "k" || name == "K" || name == "kf" || name == "ko") {
            if (const auto value = number(argument); value && *value >= 0) {
                if (karaokeSeen)
                    pendingKaraoke = karaoke;
                karaokeSeen = true;
                karaoke += static_cast<std::int64_t>(*value);
                if (name != "k")
                    builder_.note(codes::kApproximated, "\\" + name + " karaoke (shown as \\k)",
                                  line);
            }
        } else if (name == "r") {
            state = State{};
            if (!argument.empty())
                drop("\\r with a style name (reset to the line's style)");
        } else if (name == "p") {
            const auto value = number(argument);
            drawing = value && *value > 0;
            if (drawing)
                drop("\\p drawings");
        } else if (name == "move") {
            drop("\\move");
        } else if (name == "t") {
            drop("\\t animated tags");
        } else if (name == "fad" || name == "fade") {
            drop("\\fad and \\fade (the node's fades are used)");
        } else if (name == "clip" || name == "iclip") {
            drop("\\clip and \\iclip");
        } else if (name.starts_with("fr")) {
            drop("\\fr rotation");
        } else if (name == "fs") {
            drop("\\fs font size");
        } else if (name == "fn") {
            drop("\\fn font name");
        } else {
            drop("\\" + name);
        }
    }

    std::string_view text_;
    Builder& builder_;
    bool legacyStyles_ = false;
    bool scriptTypeSeen_ = false;
    double playResX_ = 0.0;
    double playResY_ = 0.0;
    int wrapStyle_ = 0;
    std::vector<std::string> styleFormat_;
    std::vector<std::string> eventFormat_;
    std::map<std::string, AssStyle> styles_;
};

} // namespace

LoadOutcome readAss(const std::string_view text, Builder& builder, std::string encoding) {
    AssReader(text, builder).run();
    return builder.finish(Format::Ass, std::move(encoding));
}

} // namespace bloom::subtitles::detail
