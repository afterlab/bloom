// #752: what the subtitle readers share (internal.hpp).
#include "internal.hpp"

#include <bloom/core/float_chars.hpp>
#include <bloom/core/utf8.hpp>
#include <bloom/document/cue_markup.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <utility>

namespace bloom::subtitles::detail {
namespace {

struct NamedColor {
    std::string_view name;
    std::uint32_t rgb;
};
// CSS Color Module Level 4 named colors, sorted by name.
constexpr std::array<NamedColor, 148> kNamedColors{{
    {"aliceblue", 0xF0F8FF},      {"antiquewhite", 0xFAEBD7},     {"aqua", 0x00FFFF},
    {"aquamarine", 0x7FFFD4},     {"azure", 0xF0FFFF},            {"beige", 0xF5F5DC},
    {"bisque", 0xFFE4C4},         {"black", 0x000000},            {"blanchedalmond", 0xFFEBCD},
    {"blue", 0x0000FF},           {"blueviolet", 0x8A2BE2},       {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887},      {"cadetblue", 0x5F9EA0},        {"chartreuse", 0x7FFF00},
    {"chocolate", 0xD2691E},      {"coral", 0xFF7F50},            {"cornflowerblue", 0x6495ED},
    {"cornsilk", 0xFFF8DC},       {"crimson", 0xDC143C},          {"cyan", 0x00FFFF},
    {"darkblue", 0x00008B},       {"darkcyan", 0x008B8B},         {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9},       {"darkgreen", 0x006400},        {"darkgrey", 0xA9A9A9},
    {"darkkhaki", 0xBDB76B},      {"darkmagenta", 0x8B008B},      {"darkolivegreen", 0x556B2F},
    {"darkorange", 0xFF8C00},     {"darkorchid", 0x9932CC},       {"darkred", 0x8B0000},
    {"darksalmon", 0xE9967A},     {"darkseagreen", 0x8FBC8F},     {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F},  {"darkslategrey", 0x2F4F4F},    {"darkturquoise", 0x00CED1},
    {"darkviolet", 0x9400D3},     {"deeppink", 0xFF1493},         {"deepskyblue", 0x00BFFF},
    {"dimgray", 0x696969},        {"dimgrey", 0x696969},          {"dodgerblue", 0x1E90FF},
    {"firebrick", 0xB22222},      {"floralwhite", 0xFFFAF0},      {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF},        {"gainsboro", 0xDCDCDC},        {"ghostwhite", 0xF8F8FF},
    {"gold", 0xFFD700},           {"goldenrod", 0xDAA520},        {"gray", 0x808080},
    {"green", 0x008000},          {"greenyellow", 0xADFF2F},      {"grey", 0x808080},
    {"honeydew", 0xF0FFF0},       {"hotpink", 0xFF69B4},          {"indianred", 0xCD5C5C},
    {"indigo", 0x4B0082},         {"ivory", 0xFFFFF0},            {"khaki", 0xF0E68C},
    {"lavender", 0xE6E6FA},       {"lavenderblush", 0xFFF0F5},    {"lawngreen", 0x7CFC00},
    {"lemonchiffon", 0xFFFACD},   {"lightblue", 0xADD8E6},        {"lightcoral", 0xF08080},
    {"lightcyan", 0xE0FFFF},      {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3},
    {"lightgreen", 0x90EE90},     {"lightgrey", 0xD3D3D3},        {"lightpink", 0xFFB6C1},
    {"lightsalmon", 0xFFA07A},    {"lightseagreen", 0x20B2AA},    {"lightskyblue", 0x87CEFA},
    {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},   {"lightsteelblue", 0xB0C4DE},
    {"lightyellow", 0xFFFFE0},    {"lime", 0x00FF00},             {"limegreen", 0x32CD32},
    {"linen", 0xFAF0E6},          {"magenta", 0xFF00FF},          {"maroon", 0x800000},
    {"mediumaquamarine", 0x66CDAA}, {"mediumblue", 0x0000CD},     {"mediumorchid", 0xBA55D3},
    {"mediumpurple", 0x9370DB},   {"mediumseagreen", 0x3CB371},   {"mediumslateblue", 0x7B68EE},
    {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC}, {"mediumvioletred", 0xC71585},
    {"midnightblue", 0x191970},   {"mintcream", 0xF5FFFA},        {"mistyrose", 0xFFE4E1},
    {"moccasin", 0xFFE4B5},       {"navajowhite", 0xFFDEAD},      {"navy", 0x000080},
    {"oldlace", 0xFDF5E6},        {"olive", 0x808000},            {"olivedrab", 0x6B8E23},
    {"orange", 0xFFA500},         {"orangered", 0xFF4500},        {"orchid", 0xDA70D6},
    {"palegoldenrod", 0xEEE8AA},  {"palegreen", 0x98FB98},        {"paleturquoise", 0xAFEEEE},
    {"palevioletred", 0xDB7093},  {"papayawhip", 0xFFEFD5},       {"peachpuff", 0xFFDAB9},
    {"peru", 0xCD853F},           {"pink", 0xFFC0CB},             {"plum", 0xDDA0DD},
    {"powderblue", 0xB0E0E6},     {"purple", 0x800080},           {"rebeccapurple", 0x663399},
    {"red", 0xFF0000},            {"rosybrown", 0xBC8F8F},        {"royalblue", 0x4169E1},
    {"saddlebrown", 0x8B4513},    {"salmon", 0xFA8072},           {"sandybrown", 0xF4A460},
    {"seagreen", 0x2E8B57},       {"seashell", 0xFFF5EE},         {"sienna", 0xA0522D},
    {"silver", 0xC0C0C0},         {"skyblue", 0x87CEEB},          {"slateblue", 0x6A5ACD},
    {"slategray", 0x708090},      {"slategrey", 0x708090},        {"snow", 0xFFFAFA},
    {"springgreen", 0x00FF7F},    {"steelblue", 0x4682B4},        {"tan", 0xD2B48C},
    {"teal", 0x008080},           {"thistle", 0xD8BFD8},          {"tomato", 0xFF6347},
    {"turquoise", 0x40E0D0},      {"violet", 0xEE82EE},           {"wheat", 0xF5DEB3},
    {"white", 0xFFFFFF},          {"whitesmoke", 0xF5F5F5},       {"yellow", 0xFFFF00},
    {"yellowgreen", 0x9ACD32},
}};

[[nodiscard]] int hexDigit(const char c) noexcept {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

[[nodiscard]] std::optional<double> number(const std::string_view text) {
    double value = 0.0;
    const auto trimmed = trim(text);
    const auto [end, error] =
        core::fromChars(trimmed.data(), trimmed.data() + trimmed.size(), value);
    if (error != std::errc{} || end != trimmed.data() + trimmed.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

} // namespace

std::vector<Line> splitLines(const std::string_view text) {
    std::vector<Line> lines;
    std::size_t start = 0;
    std::size_t number = 1;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index == text.size() || text[index] == '\n' || text[index] == '\r') {
            if (index == text.size() && start == text.size() && !lines.empty())
                break;
            lines.push_back({text.substr(start, index - start), number++});
            if (index < text.size() && text[index] == '\r' && index + 1 < text.size() &&
                text[index + 1] == '\n')
                ++index;
            start = index + 1;
        }
    }
    return lines;
}

std::string_view trim(std::string_view text) noexcept {
    const auto space = [](const char c) {
        return c == ' ' || c == '\t' || c == '\f' || c == '\n' || c == '\r';
    };
    while (!text.empty() && space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && space(text.back()))
        text.remove_suffix(1);
    return text;
}

std::string lower(const std::string_view text) {
    std::string out(text);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool equalsIgnoringCase(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        auto a = left[index];
        auto b = right[index];
        if (a >= 'A' && a <= 'Z')
            a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

unsigned byteOf(const double component) noexcept {
    return static_cast<unsigned>(std::lround(std::clamp(component, 0.0, 1.0) * 255.0));
}

std::string hexName(const core::Color4d& color) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    const auto put = [&](const double component) {
        const auto value = byteOf(component);
        out.push_back(digits[value >> 4U]);
        out.push_back(digits[value & 15U]);
    };
    put(color.red);
    put(color.green);
    put(color.blue);
    if (byteOf(color.alpha) != 255)
        put(color.alpha);
    return out;
}

std::optional<core::Color4d> parseCssColor(const std::string_view input) {
    const auto text = lower(trim(input));
    if (text.empty())
        return std::nullopt;
    if (text.front() == '#') {
        const auto hex = std::string_view(text).substr(1);
        if (hex.size() != 3 && hex.size() != 4 && hex.size() != 6 && hex.size() != 8)
            return std::nullopt;
        std::array<int, 8> values{};
        for (std::size_t index = 0; index < hex.size(); ++index) {
            values[index] = hexDigit(hex[index]);
            if (values[index] < 0)
                return std::nullopt;
        }
        const bool shortForm = hex.size() <= 4;
        const auto channel = [&](const std::size_t which) {
            const auto value = shortForm ? values[which] * 17
                                         : values[which * 2] * 16 + values[which * 2 + 1];
            return value / 255.0;
        };
        const auto alpha = (hex.size() == 4 || hex.size() == 8) ? channel(3) : 1.0;
        return core::Color4d{channel(0), channel(1), channel(2), alpha};
    }
    if (text.starts_with("rgb(") || text.starts_with("rgba(")) {
        const auto open = text.find('(');
        const auto close = text.rfind(')');
        if (close == std::string::npos || close != text.size() - 1)
            return std::nullopt;
        auto inside = std::string_view(text).substr(open + 1, close - open - 1);
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        for (std::size_t index = 0; index <= inside.size(); ++index)
            if (index == inside.size() || inside[index] == ',' || inside[index] == '/') {
                parts.push_back(trim(inside.substr(start, index - start)));
                start = index + 1;
            }
        if (parts.size() == 1) {
            // The space-separated form: rgb(255 0 0 / 50%).
            parts.clear();
            std::size_t first = 0;
            for (std::size_t index = 0; index <= inside.size(); ++index)
                if (index == inside.size() || inside[index] == ' ') {
                    if (index > first)
                        parts.push_back(inside.substr(first, index - first));
                    first = index + 1;
                }
        }
        if (parts.size() != 3 && parts.size() != 4)
            return std::nullopt;
        std::array<double, 4> channels{0.0, 0.0, 0.0, 1.0};
        for (std::size_t index = 0; index < parts.size(); ++index) {
            auto part = parts[index];
            const bool percent = !part.empty() && part.back() == '%';
            if (percent)
                part.remove_suffix(1);
            const auto value = number(part);
            if (!value)
                return std::nullopt;
            channels[index] = index == 3 ? (percent ? *value / 100.0 : *value)
                                         : (percent ? *value / 100.0 : *value / 255.0);
            channels[index] = std::clamp(channels[index], 0.0, 1.0);
        }
        return core::Color4d{channels[0], channels[1], channels[2], channels[3]};
    }
    if (text == "transparent")
        return core::Color4d{0.0, 0.0, 0.0, 0.0};
    const auto found = std::ranges::lower_bound(kNamedColors, std::string_view(text), {},
                                                &NamedColor::name);
    if (found == kNamedColors.end() || found->name != text)
        return std::nullopt;
    return core::Color4d{((found->rgb >> 16U) & 255U) / 255.0, ((found->rgb >> 8U) & 255U) / 255.0,
                         (found->rgb & 255U) / 255.0, 1.0};
}

core::RationalTime milliseconds(const std::int64_t value) {
    return *core::RationalTime::create(value, 1000);
}

void Builder::report(const std::size_t line, const std::size_t column, const std::string_view code,
                     std::string message) {
    reports_.push_back({line, column, std::string(code), std::move(message)});
}

void Builder::addStyle(document::CueStyle style) {
    auto name = style.name;
    styles_[std::move(name)] = std::move(style);
}

void Builder::note(const std::string_view code, std::string feature, const std::size_t line,
                   const std::size_t column) {
    for (auto& noted : notes_)
        if (noted.code == code && noted.feature == feature) {
            ++noted.count;
            return;
        }
    notes_.push_back({std::string(code), std::move(feature), line, column, 1});
}

void Builder::flushNotes() {
    for (auto& noted : notes_) {
        const auto verb = noted.code == codes::kApproximated ? "approximated"
                          : noted.code == codes::kCounter    ? "tolerated"
                          : noted.code == codes::kMarkup     ? "ignored"
                                                             : "dropped";
        auto message = noted.feature + ": " + verb;
        if (noted.count > 1)
            message += ", " + std::to_string(noted.count) + " times; the first on line " +
                       std::to_string(noted.line);
        else if (noted.line != 0)
            message += ", on line " + std::to_string(noted.line);
        reports_.push_back({noted.line, noted.column, noted.code, std::move(message)});
    }
    notes_.clear();
    std::ranges::stable_sort(reports_, [](const Report& left, const Report& right) {
        return left.line < right.line;
    });
}

bool Builder::hasStyle(const std::string_view name) const {
    return styles_.contains(std::string(name));
}

std::string Builder::colorStyle(const std::string_view prefix, const core::Color4d& color) {
    auto name = std::string(prefix) + "-" + hexName(color);
    if (!styles_.contains(name)) {
        document::CueStyle style;
        style.name = name;
        style.color = color;
        styles_.emplace(name, std::move(style));
    }
    return name;
}

void Builder::addCue(ReadCue cue) {
    const auto where = "line " + std::to_string(cue.line) + ": ";
    if (cue.start < core::RationalTime{} || cue.end < core::RationalTime{}) {
        report(cue.line, cue.column, codes::kSkippedCue,
               where + "a negative time is refused; the cue is skipped");
        return;
    }
    if (cue.end < cue.start) {
        report(cue.line, cue.column, codes::kSkippedCue,
               where + "the cue ends at " + document::cueTimestampText(cue.end) +
                   ", before it starts at " + document::cueTimestampText(cue.start) +
                   "; the cue is skipped");
        return;
    }
    if (cue.end == cue.start) {
        report(cue.line, cue.column, codes::kZeroLength,
               where + "the cue starts and ends at " + document::cueTimestampText(cue.start) +
                   ", so it can never be seen; it is dropped");
        return;
    }
    if (core::RationalTime::fromInteger(document::kMaximumCueSeconds) < cue.end) {
        report(cue.line, cue.column, codes::kSkippedCue,
               where + "the cue ends after " + std::to_string(document::kMaximumCueSeconds) +
                   " seconds; it is skipped");
        return;
    }
    // Markup the WebVTT rules read only in part is kept as they read it.
    if (core::isValidUtf8(cue.text)) {
        auto parsed = document::cue_markup::parse(cue.text);
        if (!parsed.ignored.empty()) {
            for (const auto& ignored : parsed.ignored)
                report(cue.line, cue.column, codes::kMarkup,
                       where + ignored.message + "; it is left out");
            cue.text = document::cue_markup::serialize(parsed.tree);
        }
    }
    if (auto problem = document::cueTextProblem(cue.text)) {
        report(cue.line, cue.column, codes::kSkippedCue,
               where + *problem + "; the cue is skipped");
        return;
    }
    if (!cue.speaker.empty() &&
        (cue.speaker.size() > document::kMaximumCueNameBytes || !core::isValidUtf8(cue.speaker))) {
        report(cue.line, cue.column, codes::kDropped,
               where + "the speaker name is longer than " +
                   std::to_string(document::kMaximumCueNameBytes) + " bytes; it is dropped");
        cue.speaker.clear();
    }
    if (cues_.size() >= document::kMaximumCues) {
        tooMany_ = true;
        return;
    }
    cues_.push_back(std::move(cue));
}

LoadFailure Builder::failure(const std::size_t line, const std::size_t column,
                             const std::string_view code, std::string message) const {
    return LoadFailure{line, column, std::string(code), std::move(message), reports_};
}

LoadOutcome Builder::finish(const Format format, std::string encoding) {
    flushNotes();
    if (stopRequested())
        return failure(0, 0, codes::kCancelled, "The load was cancelled.");
    if (tooMany_)
        return failure(0, 0, codes::kLimit,
                       "The file holds more than " + std::to_string(document::kMaximumCues) +
                           " cues; a list holds at most " +
                           std::to_string(document::kMaximumCues));
    if (styles_.size() > document::kMaximumCueStyles)
        return failure(0, 0, codes::kLimit,
                       "The file defines " + std::to_string(styles_.size()) +
                           " styles; a list holds at most " +
                           std::to_string(document::kMaximumCueStyles));
    if (cues_.empty())
        return failure(0, 0, codes::kNoCues,
                       reports_.empty() ? "The file holds no cues."
                                        : "No cue in the file could be read: " +
                                              reports_.front().message);
    if (options_.strict && !reports_.empty())
        return failure(reports_.front().line, reports_.front().column, codes::kStrict,
                       "A strict load refuses any warning; the first is: " +
                           reports_.front().message);
    LoadResult result;
    result.format = format;
    result.encoding = std::move(encoding);
    result.cues.language = document::isWellFormedLanguageTag(language_) ? language_ : "und";
    for (auto& [name, style] : styles_) {
        static_cast<void>(name);
        result.cues.styles.push_back(std::move(style));
    }
    // Cues out of order are sorted; equal times keep their order in the file.
    std::ranges::stable_sort(cues_, [](const ReadCue& left, const ReadCue& right) {
        if (left.start != right.start)
            return left.start < right.start;
        return left.end < right.end;
    });
    std::uint64_t id = 1;
    for (auto& read : cues_) {
        if (!read.style.empty() && !document::isBuiltInCueStyleName(read.style) &&
            !styles_.contains(read.style) &&
            std::ranges::none_of(result.cues.styles, [&](const document::CueStyle& style) {
                return style.name == read.style;
            })) {
            report(read.line, read.column, codes::kDropped,
                   "line " + std::to_string(read.line) + ": the style \"" + read.style +
                       "\" is not defined; the cue is drawn in the node's style");
            read.style.clear();
        }
        auto cue = std::make_shared<document::Cue>();
        cue->id = id++;
        cue->start = read.start;
        cue->end = read.end;
        cue->text = std::move(read.text);
        cue->speaker = std::move(read.speaker);
        cue->style = std::move(read.style);
        cue->placement = read.placement;
        result.cues.cues.push_back(std::move(cue));
    }
    result.cues.nextCueId = id;
    document::canonicalizeCueList(result.cues);
    if (const auto overlap = document::maximumCueOverlap(result.cues);
        overlap.count > document::kMaximumActiveCues)
        return failure(0, 0, codes::kLimit,
                       std::to_string(overlap.count) + " cues show at once at " +
                           document::cueTimestampText(overlap.at) + "; at most " +
                           std::to_string(document::kMaximumActiveCues) + " may");
    if (auto problem = result.cues.problem())
        return failure(0, 0, codes::kMalformed, "The cues do not form a valid list: " + *problem);
    if (options_.strict && !reports_.empty())
        return failure(reports_.front().line, reports_.front().column, codes::kStrict,
                       "A strict load refuses any warning; the first is: " +
                           reports_.front().message);
    result.reports = reports_;
    return result;
}

} // namespace bloom::subtitles::detail
