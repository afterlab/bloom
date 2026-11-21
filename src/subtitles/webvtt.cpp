// #752: the W3C WebVTT "WebVTT file parsing" algorithm, step by step (webvtt.hpp).
#include <bloom/subtitles/webvtt.hpp>

#include <bloom/document/cue_markup.hpp>

#include <algorithm>
#include <charconv>
#include <limits>
#include <string>
#include <utility>

namespace bloom::subtitles::webvtt {
namespace {

[[nodiscard]] bool isWhitespace(const char c) noexcept {
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}
[[nodiscard]] bool isDigit(const char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] std::vector<std::string_view> splitOnWhitespace(const std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() && isWhitespace(text[index]))
            ++index;
        const auto start = index;
        while (index < text.size() && !isWhitespace(text[index]))
            ++index;
        if (index > start)
            parts.push_back(text.substr(start, index - start));
    }
    return parts;
}

[[nodiscard]] std::optional<double> realNumber(const std::string_view text) {
    double value = 0.0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    if (first != last && *first == '+')
        ++first;
    const auto [end, error] = std::from_chars(first, last, value, std::chars_format::fixed);
    if (end != last)
        return std::nullopt;
    if (error == std::errc::result_out_of_range) {
        // Below the smallest double the number is zero, as the specification's real numbers
        // become in a double; above the largest it is not a number the settings take.
        const auto whole = std::string_view(first, last).substr(
            first != last && *first == '-' ? 1 : 0);
        const auto integer = whole.substr(0, whole.find('.'));
        if (std::ranges::all_of(integer, [](const char c) { return c == '0'; }))
            return 0.0;
        return std::nullopt;
    }
    if (error != std::errc{})
        return std::nullopt;
    return value;
}

// "Parse a percentage string": one or more digits, optionally '.' and one or more digits, then
// '%', in 0..100.
[[nodiscard]] std::optional<double> percentage(const std::string_view text) {
    if (text.size() < 2 || text.back() != '%')
        return std::nullopt;
    const auto number = text.substr(0, text.size() - 1);
    std::size_t index = 0;
    while (index < number.size() && isDigit(number[index]))
        ++index;
    if (index == 0)
        return std::nullopt;
    if (index < number.size()) {
        if (number[index] != '.')
            return std::nullopt;
        ++index;
        const auto fraction = index;
        while (index < number.size() && isDigit(number[index]))
            ++index;
        if (index == fraction || index != number.size())
            return std::nullopt;
    }
    const auto value = realNumber(number);
    if (!value || *value < 0.0 || *value > 100.0)
        return std::nullopt;
    return value;
}

// "Parse the WebVTT cue settings".
void parseSettings(const std::string_view input, Cue& cue, const std::vector<Region>& regions) {
    for (const auto setting : splitOnWhitespace(input)) {
        const auto colon = setting.find(':');
        if (colon == std::string_view::npos || colon == 0 || colon == setting.size() - 1) {
            cue.skippedSettings.emplace_back(setting);
            continue;
        }
        const auto name = setting.substr(0, colon);
        const auto value = setting.substr(colon + 1);
        bool used = false;
        if (name == "region") {
            for (auto index = regions.size(); index > 0; --index)
                if (regions[index - 1].id == value) {
                    cue.region = index - 1;
                    break;
                }
            used = true;
        } else if (name == "vertical") {
            if (value == "rl") {
                cue.vertical = Direction::RightToLeft;
                used = true;
            } else if (value == "lr") {
                cue.vertical = Direction::LeftToRight;
                used = true;
            }
        } else if (name == "line") {
            const auto comma = value.find(',');
            const auto linepos = value.substr(0, comma);
            const std::optional<std::string_view> linealign =
                comma == std::string_view::npos ? std::nullopt
                                                : std::optional(value.substr(comma + 1));
            std::optional<double> number;
            bool digit = false;
            for (const char c : linepos)
                digit = digit || isDigit(c);
            if (digit) {
                if (linepos.back() == '%') {
                    number = percentage(linepos);
                } else {
                    bool valid = true;
                    std::size_t dots = 0;
                    for (std::size_t index = 0; index < linepos.size(); ++index) {
                        const char c = linepos[index];
                        if (!isDigit(c) && c != '-' && c != '.')
                            valid = false;
                        if (c == '-' && index != 0)
                            valid = false;
                        if (c == '.') {
                            ++dots;
                            if (index == 0 || index + 1 == linepos.size() ||
                                !isDigit(linepos[index - 1]) || !isDigit(linepos[index + 1]))
                                valid = false;
                        }
                    }
                    if (valid && dots <= 1)
                        number = realNumber(linepos);
                }
            }
            std::optional<LineAlign> align = LineAlign::Start;
            if (linealign) {
                if (*linealign == "start")
                    align = LineAlign::Start;
                else if (*linealign == "center")
                    align = LineAlign::Center;
                else if (*linealign == "end")
                    align = LineAlign::End;
                else
                    align.reset();
            }
            if (number && align) {
                cue.line = *number;
                cue.snapToLines = linepos.back() != '%';
                cue.lineAlign = *align;
                used = true;
            }
        } else if (name == "position") {
            const auto comma = value.find(',');
            const auto colpos = value.substr(0, comma);
            const std::optional<std::string_view> colalign =
                comma == std::string_view::npos ? std::nullopt
                                                : std::optional(value.substr(comma + 1));
            const auto number = percentage(colpos);
            std::optional<PositionAlign> align = PositionAlign::Auto;
            if (colalign) {
                if (*colalign == "line-left")
                    align = PositionAlign::LineLeft;
                else if (*colalign == "center")
                    align = PositionAlign::Center;
                else if (*colalign == "line-right")
                    align = PositionAlign::LineRight;
                else
                    align.reset();
            }
            if (number && align) {
                cue.position = *number;
                cue.positionAlign = *align;
                used = true;
            }
        } else if (name == "size") {
            if (const auto number = percentage(value)) {
                cue.size = *number;
                used = true;
            }
        } else if (name == "align") {
            used = true;
            if (value == "start")
                cue.align = TextAlign::Start;
            else if (value == "center")
                cue.align = TextAlign::Center;
            else if (value == "end")
                cue.align = TextAlign::End;
            else if (value == "left")
                cue.align = TextAlign::Left;
            else if (value == "right")
                cue.align = TextAlign::Right;
            else
                used = false;
        }
        if (!used)
            cue.skippedSettings.emplace_back(setting);
    }
}

// "Collect WebVTT cue timings and settings".
[[nodiscard]] bool collectTimingsAndSettings(const std::string_view input, Cue& cue,
                                             const std::vector<Region>& regions) {
    std::size_t position = 0;
    const auto skip = [&] {
        while (position < input.size() && isWhitespace(input[position]))
            ++position;
    };
    skip();
    const auto start = document::cue_markup::collectTimestamp(input, position);
    if (!start)
        return false;
    skip();
    for (const char expected : {'-', '-', '>'}) {
        if (position >= input.size() || input[position] != expected)
            return false;
        ++position;
    }
    skip();
    const auto end = document::cue_markup::collectTimestamp(input, position);
    if (!end)
        return false;
    cue.startMilliseconds = *start;
    cue.endMilliseconds = *end;
    parseSettings(input.substr(position), cue, regions);
    return true;
}

// "Collect a WebVTT region settings".
void collectRegionSettings(const std::string_view input, Region& region) {
    for (const auto setting : splitOnWhitespace(input)) {
        const auto colon = setting.find(':');
        if (colon == std::string_view::npos || colon == 0 || colon == setting.size() - 1)
            continue;
        const auto name = setting.substr(0, colon);
        const auto value = setting.substr(colon + 1);
        if (name == "id") {
            if (value.find("-->") == std::string_view::npos)
                region.id = std::string(value);
        } else if (name == "width") {
            if (const auto number = percentage(value))
                region.width = *number;
        } else if (name == "lines") {
            bool digits = !value.empty();
            for (const char c : value)
                digits = digits && isDigit(c);
            if (digits) {
                std::int64_t number = 0;
                const auto [end, error] =
                    std::from_chars(value.data(), value.data() + value.size(), number);
                if (error == std::errc{} && end == value.data() + value.size())
                    region.lines = number;
                else
                    region.lines = std::numeric_limits<std::int64_t>::max();
            }
        } else if (name == "regionanchor" || name == "viewportanchor") {
            const auto comma = value.find(',');
            if (comma == std::string_view::npos)
                continue;
            const auto x = percentage(value.substr(0, comma));
            const auto y = percentage(value.substr(comma + 1));
            if (!x || !y)
                continue;
            if (name == "regionanchor") {
                region.anchorX = *x;
                region.anchorY = *y;
            } else {
                region.viewportAnchorX = *x;
                region.viewportAnchorY = *y;
            }
        } else if (name == "scroll") {
            if (value == "up")
                region.scrollUp = true;
        }
    }
}

class Parser {
  public:
    explicit Parser(std::string input) : input_(std::move(input)) {}

    File run() {
        File file;
        // Step 3-5: the signature.
        if (input_.size() < 6 || input_.compare(0, 6, "WEBVTT") != 0)
            return file;
        if (input_.size() > 6 && input_[6] != ' ' && input_[6] != '\t' && input_[6] != '\n')
            return file;
        file.signatureValid = true;
        // Step 6-9.
        collectNotLineFeeds();
        if (atEnd())
            return file;
        advanceLineFeed();
        if (atEnd())
            return file;
        // Step 10-11: the header.
        if (input_[position_] != '\n')
            static_cast<void>(collectBlock(file, true));
        else
            advanceLineFeed();
        collectLineFeeds();
        // Step 14: the block loop.
        while (!atEnd()) {
            auto block = collectBlock(file, false);
            if (block.cue)
                file.cues.push_back(std::move(*block.cue));
            else if (block.style)
                file.styles.push_back(std::move(*block.style));
            else if (block.region)
                file.regions.push_back(std::move(*block.region));
            collectLineFeeds();
        }
        return file;
    }

  private:
    struct Block {
        std::optional<Cue> cue;
        std::optional<StyleSheet> style;
        std::optional<Region> region;
    };

    [[nodiscard]] bool atEnd() const noexcept { return position_ >= input_.size(); }
    void advanceLineFeed() {
        ++position_;
        ++line_;
    }
    std::string_view collectNotLineFeeds() {
        const auto start = position_;
        while (position_ < input_.size() && input_[position_] != '\n')
            ++position_;
        return std::string_view(input_).substr(start, position_ - start);
    }
    void collectLineFeeds() {
        while (position_ < input_.size() && input_[position_] == '\n')
            advanceLineFeed();
    }

    // "Collect a WebVTT block".
    Block collectBlock(File& file, const bool inHeader) {
        std::size_t lineCount = 0;
        auto previousPosition = position_;
        auto previousLine = line_;
        std::string buffer;
        bool seenEof = false;
        bool seenArrow = false;
        std::optional<Cue> cue;
        std::optional<StyleSheet> style;
        std::optional<Region> region;
        const auto firstLine = line_;
        std::string firstText;
        bool arrowFailed = false;
        std::size_t arrowFailedLine = 0;
        while (true) {
            const auto lineNumber = line_;
            const auto line = std::string(collectNotLineFeeds());
            ++lineCount;
            if (lineCount == 1)
                firstText = line;
            if (atEnd())
                seenEof = true;
            else
                advanceLineFeed();
            if (line.find("-->") != std::string::npos) {
                if (!inHeader && (lineCount == 1 || (lineCount == 2 && !seenArrow))) {
                    seenArrow = true;
                    previousPosition = position_;
                    previousLine = line_;
                    Cue created;
                    created.id = buffer;
                    created.timingLine = lineNumber;
                    created.textLine = lineNumber + 1;
                    if (collectTimingsAndSettings(line, created, file.regions)) {
                        cue = std::move(created);
                        buffer.clear();
                        seenCue_ = true;
                    } else {
                        cue.reset();
                        arrowFailed = true;
                        arrowFailedLine = lineNumber;
                    }
                } else {
                    position_ = previousPosition;
                    line_ = previousLine;
                    break;
                }
            } else if (line.empty()) {
                break;
            } else {
                if (!inHeader && lineCount == 2) {
                    const auto restIsWhitespace = [&](const std::size_t from) {
                        for (std::size_t index = from; index < buffer.size(); ++index)
                            if (!isWhitespace(buffer[index]))
                                return false;
                        return true;
                    };
                    if (!seenCue_ && buffer.starts_with("STYLE") && restIsWhitespace(5)) {
                        style = StyleSheet{{}, firstLine};
                        buffer.clear();
                    } else if (!seenCue_ && buffer.starts_with("REGION") && restIsWhitespace(6)) {
                        region = Region{};
                        region->line = firstLine;
                        buffer.clear();
                    }
                }
                if (!buffer.empty())
                    buffer.push_back('\n');
                buffer += line;
                previousPosition = position_;
                previousLine = line_;
            }
            if (seenEof)
                break;
        }
        Block block;
        if (cue) {
            cue->text = std::move(buffer);
            block.cue = std::move(cue);
        } else if (style) {
            style->text = std::move(buffer);
            block.style = std::move(style);
        } else if (region) {
            collectRegionSettings(buffer, *region);
            block.region = std::move(region);
        } else if (!inHeader) {
            const bool note = firstText.starts_with("NOTE") &&
                              (firstText.size() == 4 || firstText[4] == ' ' ||
                               firstText[4] == '\t');
            file.ignored.push_back(
                {arrowFailed ? arrowFailedLine : firstLine, note && !arrowFailed, firstText});
        }
        return block;
    }

    std::string input_;
    std::size_t position_ = 0;
    std::size_t line_ = 1;
    bool seenCue_ = false;
};

} // namespace

File parse(const std::string_view text) {
    // The preprocessing: NUL is U+FFFD, and CR LF and lone CR are LF.
    std::string input;
    input.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '\0') {
            input += "\xEF\xBF\xBD";
        } else if (c == '\r') {
            input.push_back('\n');
            if (index + 1 < text.size() && text[index + 1] == '\n')
                ++index;
        } else {
            input.push_back(c);
        }
    }
    return Parser(std::move(input)).run();
}

} // namespace bloom::subtitles::webvtt
