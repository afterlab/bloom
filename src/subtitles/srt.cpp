// #752: SubRip (.srt), docs/architecture/subtitles.md "SubRip". SRT has no specification; this
// reads the de facto grammar as leniently as common players do (a ',' or '.' before the
// milliseconds, any number of hour digits, missing or duplicate counters, extra blank lines,
// <i> <b> <u> <font color> and {\an1..9}) and writes the form every player reads.
#include "internal.hpp"

#include <bloom/document/cue_markup.hpp>

#include <algorithm>
#include <charconv>
#include <string>

namespace bloom::subtitles::detail {
namespace {

[[nodiscard]] bool isDigit(const char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] bool allDigits(const std::string_view text) noexcept {
    return !text.empty() && std::ranges::all_of(text, isDigit);
}

// H+:MM:SS[,.]mmm (the fraction one to three digits, or absent). Returns milliseconds.
[[nodiscard]] std::optional<std::int64_t> srtTime(const std::string_view text, std::size_t& position) {
    const auto digits = [&](const std::size_t most, std::int64_t& value) {
        const auto start = position;
        value = 0;
        while (position < text.size() && isDigit(text[position]) && position - start < most) {
            value = value * 10 + (text[position] - '0');
            ++position;
        }
        return position - start;
    };
    std::int64_t hours = 0;
    std::int64_t minutes = 0;
    std::int64_t seconds = 0;
    if (digits(9, hours) == 0 || position >= text.size() || text[position] != ':')
        return std::nullopt;
    ++position;
    if (digits(2, minutes) == 0 || position >= text.size() || text[position] != ':')
        return std::nullopt;
    ++position;
    if (digits(2, seconds) == 0 || minutes > 59 || seconds > 59)
        return std::nullopt;
    std::int64_t millis = 0;
    if (position < text.size() && (text[position] == ',' || text[position] == '.')) {
        ++position;
        std::int64_t fraction = 0;
        const auto count = digits(3, fraction);
        if (count == 0)
            return std::nullopt;
        millis = fraction * (count == 1 ? 100 : count == 2 ? 10 : 1);
        while (position < text.size() && isDigit(text[position]))
            ++position; // finer digits than a millisecond are not kept
    }
    return ((hours * 60 + minutes) * 60 + seconds) * 1000 + millis;
}

struct Timing {
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::string rest;
};

[[nodiscard]] std::optional<Timing> timingLine(const std::string_view line) {
    std::size_t position = 0;
    const auto skip = [&] {
        while (position < line.size() && (line[position] == ' ' || line[position] == '\t'))
            ++position;
    };
    skip();
    const auto start = srtTime(line, position);
    if (!start)
        return std::nullopt;
    skip();
    if (line.substr(position, 3) != "-->")
        return std::nullopt;
    position += 3;
    skip();
    const auto end = srtTime(line, position);
    if (!end)
        return std::nullopt;
    return Timing{*start, *end, std::string(trim(line.substr(position)))};
}

// SRT text (one cue, lines joined by '\n') as Bloom cue markup.
class TextConverter {
  public:
    TextConverter(Builder& builder, const std::size_t line) : builder_(builder), line_(line) {}

    std::string convert(std::string_view text, document::CuePlacement& placement) {
        std::string out;
        std::size_t index = 0;
        while (index < text.size()) {
            const char c = text[index];
            if (c == '{' && index + 1 < text.size() && text[index + 1] == '\\') {
                const auto close = text.find('}', index);
                if (close != std::string_view::npos) {
                    override(text.substr(index + 2, close - index - 2), placement);
                    index = close + 1;
                    continue;
                }
            }
            if (c == '<') {
                const auto close = text.find('>', index);
                if (close != std::string_view::npos) {
                    const auto inside = text.substr(index + 1, close - index - 1);
                    if (tag(inside, out)) {
                        index = close + 1;
                        continue;
                    }
                }
                out += "&lt;";
                ++index;
                continue;
            }
            if (c == '&')
                out += "&amp;";
            else if (c == '>')
                out += "&gt;";
            else
                out.push_back(c);
            ++index;
        }
        while (!open_.empty()) {
            out += "</" + open_.back() + ">";
            open_.pop_back();
        }
        return out;
    }

  private:
    void override(const std::string_view inside, document::CuePlacement& placement) {
        // {\an1}..{\an9}: the numeric keypad's positions.
        if (inside.size() == 3 && inside.starts_with("an") && inside[2] >= '1' && inside[2] <= '9') {
            static constexpr std::array<document::CueAnchor, 9> kKeypad{
                document::CueAnchor::BottomLeft, document::CueAnchor::Bottom,
                document::CueAnchor::BottomRight, document::CueAnchor::Left,
                document::CueAnchor::Center, document::CueAnchor::Right,
                document::CueAnchor::TopLeft, document::CueAnchor::Top,
                document::CueAnchor::TopRight};
            placement.anchor = kKeypad[static_cast<std::size_t>(inside[2] - '1')];
            return;
        }
        builder_.note(codes::kDropped, "override tags in SRT other than {\\an1}..{\\an9}", line_);
    }

    // Returns false when `inside` is not a tag at all (then '<' is a character).
    bool tag(const std::string_view inside, std::string& out) {
        if (inside.empty())
            return false;
        const bool closing = inside.front() == '/';
        auto body = trim(closing ? inside.substr(1) : inside);
        std::size_t nameEnd = 0;
        while (nameEnd < body.size() &&
               ((body[nameEnd] >= 'a' && body[nameEnd] <= 'z') ||
                (body[nameEnd] >= 'A' && body[nameEnd] <= 'Z')))
            ++nameEnd;
        if (nameEnd == 0)
            return false;
        const auto name = lower(body.substr(0, nameEnd));
        const auto attributes = body.substr(nameEnd);
        if (name == "i" || name == "b" || name == "u") {
            if (closing)
                close(name, out);
            else {
                out += "<" + name + ">";
                open_.push_back(name);
            }
            return true;
        }
        if (name == "font") {
            if (closing) {
                close("font", out);
                return true;
            }
            std::optional<core::Color4d> color;
            const auto colorAt = lower(attributes).find("color");
            if (colorAt != std::string::npos) {
                auto value = attributes.substr(colorAt + 5);
                value = trim(value);
                if (!value.empty() && value.front() == '=') {
                    value = trim(value.substr(1));
                    if (!value.empty() && (value.front() == '"' || value.front() == '\'')) {
                        const auto quote = value.front();
                        const auto end = value.find(quote, 1);
                        value = value.substr(1, end == std::string_view::npos ? std::string_view::npos
                                                                               : end - 1);
                    } else {
                        value = value.substr(0, value.find_first_of(" \t"));
                    }
                    color = parseCssColor(value);
                    if (!color && !value.empty() && value.front() != '#')
                        color = parseCssColor("#" + std::string(value));
                }
            }
            const auto other = lower(attributes);
            if (other.find("face") != std::string::npos || other.find("size") != std::string::npos)
                builder_.note(codes::kDropped, "<font face> and <font size>", line_);
            if (color) {
                const auto style = builder_.colorStyle("srt", *color);
                out += "<c." + style + ">";
                open_.push_back("font");
            } else {
                builder_.note(codes::kDropped, "<font> without a color Bloom reads", line_);
                open_.push_back("font-none");
            }
            return true;
        }
        builder_.note(codes::kDropped, "the <" + name + "> tag", line_);
        return true;
    }

    void close(const std::string& name, std::string& out) {
        const auto found = std::find_if(open_.rbegin(), open_.rend(), [&](const std::string& open) {
            return open == name || (name == "font" && open == "font-none");
        });
        if (found == open_.rend()) {
            builder_.note(codes::kMarkup, "a closing </" + name + "> with no opening tag", line_);
            return;
        }
        // Close everything opened after it too, so the markup stays nested.
        while (!open_.empty()) {
            const auto top = open_.back();
            open_.pop_back();
            if (top != "font-none")
                out += "</" + std::string(top == "font" ? "c" : top) + ">";
            if (top == name || (name == "font" && top == "font-none"))
                break;
        }
    }

    Builder& builder_;
    std::size_t line_;
    std::vector<std::string> open_;
};

} // namespace

LoadOutcome readSrt(const std::string_view text, Builder& builder, std::string encoding) {
    const auto lines = splitLines(text);
    std::size_t index = 0;
    std::optional<std::int64_t> previousCounter;
    while (index < lines.size()) {
        if (builder.stopRequested())
            return builder.finish(Format::Srt, encoding);
        if (trim(lines[index].text).empty()) {
            ++index;
            continue;
        }
        const auto blockLine = lines[index].number;
        std::optional<std::int64_t> counter;
        std::optional<Timing> timing;
        auto first = trim(lines[index].text);
        if (first.starts_with("\xEF\xBB\xBF"))
            first.remove_prefix(3);
        if (auto direct = timingLine(first)) {
            timing = std::move(direct);
            builder.note(codes::kCounter, "a missing cue counter", blockLine);
            ++index;
        } else if (index + 1 < lines.size() && (timing = timingLine(lines[index + 1].text))) {
            if (allDigits(first)) {
                std::int64_t value = 0;
                std::from_chars(first.data(), first.data() + first.size(), value);
                counter = value;
            } else {
                builder.note(codes::kCounter, "a cue counter that is not a number", blockLine);
            }
            index += 2;
        } else {
            builder.report(blockLine, 1, codes::kSkippedCue,
                           "line " + std::to_string(blockLine) +
                               ": a SubRip cue is a counter, a timing line \"00:00:01,000 --> "
                               "00:00:02,000\" and its text; this block has no timing line and "
                               "is skipped");
            while (index < lines.size() && !trim(lines[index].text).empty())
                ++index;
            continue;
        }
        if (counter && previousCounter && *counter == *previousCounter)
            builder.note(codes::kCounter, "a duplicate cue counter", blockLine);
        if (counter)
            previousCounter = counter;
        const auto timingNumber = lines[index - 1].number;
        if (!timing->rest.empty())
            builder.note(codes::kDropped, "text after the end time (SubRip coordinates)",
                         timingNumber);
        std::string body;
        bool firstLine = true;
        while (index < lines.size() && !trim(lines[index].text).empty()) {
            // A timing line inside the text starts the next cue (a missing blank line), its
            // counter being the line before it.
            if (timingLine(lines[index].text))
                break;
            if (index + 1 < lines.size() && allDigits(trim(lines[index].text)) &&
                timingLine(lines[index + 1].text))
                break;
            if (!firstLine)
                body.push_back('\n');
            body += lines[index].text;
            firstLine = false;
            ++index;
        }
        ReadCue cue;
        cue.start = milliseconds(timing->start);
        cue.end = milliseconds(timing->end);
        cue.line = timingNumber;
        cue.column = 1;
        cue.text = TextConverter(builder, timingNumber + 1).convert(body, cue.placement);
        builder.addCue(std::move(cue));
    }
    return builder.finish(Format::Srt, std::move(encoding));
}

namespace {

// Bloom markup as SRT text.
class SrtWriter {
  public:
    SrtWriter(const document::CueListValue& list, std::vector<Report>& reports, const std::uint64_t id)
        : list_(list), reports_(reports), id_(id) {}

    std::string text(const document::Cue& cue) {
        std::string out;
        if (cue.placement.anchor) {
            static constexpr std::array<char, 9> kKeypad{'7', '8', '9', '4', '5', '6', '1', '2', '3'};
            out += "{\\an";
            out.push_back(kKeypad[static_cast<std::size_t>(*cue.placement.anchor)]);
            out += "}";
        }
        if (cue.placement.positionX || cue.placement.positionY || cue.placement.lineOffset ||
            cue.placement.width || cue.placement.textAlign)
            drop("its position, line, width or alignment");
        if (!cue.speaker.empty())
            drop("its speaker");
        std::string suffix;
        if (!cue.style.empty())
            open(cue.style, out, suffix);
        const auto parsed = document::cue_markup::parse(cue.text);
        nodes(parsed.tree.children, out);
        return out + suffix;
    }

  private:
    void drop(const std::string& what) {
        reports_.push_back({0, 0, std::string(codes::kDropped),
                            "cue " + std::to_string(id_) + ": SubRip cannot carry " + what});
    }

    // The tags a style name stands for, opened into `out`, and their closing tags into `suffix`.
    void open(const std::string& name, std::string& out, std::string& suffix) {
        std::optional<core::Color4d> color;
        bool italic = false;
        bool bold = false;
        bool underline = false;
        if (const auto* style = list_.findStyle(name)) {
            color = style->color;
            italic = style->italic.value_or(false);
            bold = style->bold.value_or(false);
            underline = style->underline.value_or(false);
            if (style->backgroundColor || style->outlineColor || style->outlineWidth ||
                style->fontFamily || style->fontSize)
                drop("the background, outline, font or size of style \"" + name + "\"");
        } else if (!name.starts_with("bg_")) {
            color = document::builtInCueClassColor(name);
        } else {
            drop("the background class \"" + name + "\"");
        }
        if (color) {
            out += "<font color=\"#" + hexName(*color) + "\">";
            suffix.insert(0, "</font>");
        }
        for (const auto& [on, tag] : {std::pair{italic, "i"}, {bold, "b"}, {underline, "u"}})
            if (on) {
                out += "<" + std::string(tag) + ">";
                suffix.insert(0, "</" + std::string(tag) + ">");
            }
    }

    void nodes(const std::vector<document::cue_markup::Node>& children, std::string& out) {
        using Kind = document::cue_markup::NodeKind;
        for (const auto& node : children) {
            switch (node.kind) {
            case Kind::Text:
                out += node.text;
                break;
            case Kind::Italic:
            case Kind::Bold:
            case Kind::Underline: {
                const auto tag = node.kind == Kind::Italic ? "i" : node.kind == Kind::Bold ? "b" : "u";
                out += "<" + std::string(tag) + ">";
                nodes(node.children, out);
                out += "</" + std::string(tag) + ">";
                break;
            }
            case Kind::Class: {
                std::string suffix;
                for (const auto& name : node.classes)
                    open(name, out, suffix);
                nodes(node.children, out);
                out += suffix;
                break;
            }
            case Kind::Voice:
                drop("a <v> speaker span");
                nodes(node.children, out);
                break;
            case Kind::Language:
                nodes(node.children, out);
                break;
            case Kind::Ruby:
                for (const auto& child : node.children)
                    if (child.kind != Kind::RubyText)
                        nodes({child}, out);
                    else
                        drop("ruby text");
                break;
            case Kind::RubyText:
                break;
            case Kind::Timestamp:
                drop("word timestamps");
                break;
            }
        }
    }

    const document::CueListValue& list_;
    std::vector<Report>& reports_;
    std::uint64_t id_;
};

[[nodiscard]] std::string srtTimestamp(const core::RationalTime time) {
    auto text = document::cueTimestampText(time);
    text[text.size() - 4] = ',';
    return text;
}

} // namespace

std::string writeSrt(const document::CueListValue& cues, std::vector<Report>& reports) {
    std::string out;
    std::size_t counter = 1;
    for (const auto& cue : cues.cues) {
        if (counter > 1)
            out += '\n'; // a blank line between cues, none after the last
        out += std::to_string(counter++);
        out += '\n';
        out += srtTimestamp(cue->start) + " --> " + srtTimestamp(cue->end);
        out += '\n';
        out += SrtWriter(cues, reports, cue->id).text(*cue);
        out += '\n';
    }
    return out;
}

} // namespace bloom::subtitles::detail
