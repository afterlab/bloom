// #752: TTML and IMSC 1.1 Text (docs/architecture/subtitles.md, "TTML / IMSC"), parsed with the
// locked Expat. The parser is namespace aware; a document type declaration, an entity declaration
// or an external entity is refused, nesting is capped at 64, and no element may carry more than
// 256 attributes (checked before Expat sees the text: Expat 2.8.3's attribute handling is
// quadratic in an element's attribute count, docs/architecture/dependency-intake.md "Expat").
// timeBase="media" only. EBU-TT-D and SMPTE-TT read as TTML profiles; their image forms are refused.
#include "internal.hpp"

#include <bloom/core/float_chars.hpp>
#include <bloom/core/int128.hpp>
#include <bloom/document/cue_markup.hpp>

#include <expat.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <utility>

namespace bloom::subtitles::detail {
namespace {

constexpr std::string_view kTt = "http://www.w3.org/ns/ttml";
constexpr std::string_view kParameter = "http://www.w3.org/ns/ttml#parameter";
constexpr std::string_view kStyling = "http://www.w3.org/ns/ttml#styling";
constexpr std::string_view kMetadata = "http://www.w3.org/ns/ttml#metadata";
constexpr std::string_view kXml = "http://www.w3.org/XML/1998/namespace";
constexpr std::string_view kSmpte = "http://www.smpte-ra.org/schemas/2052-1/2010/smpte";
constexpr std::size_t kMaximumDepth = 64;
constexpr std::size_t kMaximumAttributes = 256;

// The DFXP drafts' namespaces read as TTML.
[[nodiscard]] std::string_view canonicalNamespace(const std::string_view space) {
    for (const auto draft : {std::string_view("http://www.w3.org/2006/10/ttaf1"),
                             std::string_view("http://www.w3.org/2006/04/ttaf1")}) {
        if (space == draft)
            return kTt;
        if (space.starts_with(draft) && space.size() > draft.size() && space[draft.size()] == '#') {
            const auto suffix = space.substr(draft.size());
            if (suffix == "#parameter")
                return kParameter;
            if (suffix == "#styling" || suffix == "#style")
                return kStyling;
            if (suffix == "#metadata")
                return kMetadata;
        }
    }
    return space;
}

struct Element {
    std::string space;
    std::string name;
    std::map<std::string, std::string> attributes; // "<namespace> <local>" or "<local>"
    std::vector<std::variant<std::unique_ptr<Element>, std::string>> children;
    std::size_t line = 0;
    std::size_t column = 0;
    Element* parent = nullptr;

    [[nodiscard]] bool is(const std::string_view wantedSpace, const std::string_view local) const {
        return space == wantedSpace && name == local;
    }
    [[nodiscard]] const std::string* attribute(const std::string_view wantedSpace,
                                               const std::string_view local) const {
        const auto key = wantedSpace.empty() ? std::string(local)
                                             : std::string(wantedSpace) + " " + std::string(local);
        const auto found = attributes.find(key);
        return found == attributes.end() ? nullptr : &found->second;
    }
};

struct Splits {
    std::string space;
    std::string local;
};
[[nodiscard]] Splits split(const XML_Char* name) {
    const std::string_view text(name);
    const auto separator = text.rfind(' ');
    if (separator == std::string_view::npos)
        return {{}, std::string(text)};
    return {std::string(canonicalNamespace(text.substr(0, separator))),
            std::string(text.substr(separator + 1))};
}

struct ParseState {
    XML_Parser parser = nullptr;
    std::unique_ptr<Element> root;
    Element* current = nullptr;
    std::size_t depth = 0;
    std::optional<std::string> refusal;
    std::size_t refusalLine = 0;
    const std::stop_token* stop = nullptr;
};

void refuse(ParseState& state, std::string message) {
    if (!state.refusal) {
        state.refusal = std::move(message);
        state.refusalLine = static_cast<std::size_t>(XML_GetCurrentLineNumber(state.parser));
    }
    XML_StopParser(state.parser, XML_FALSE);
}

void XMLCALL onStart(void* data, const XML_Char* name, const XML_Char** attributes) {
    auto& state = *static_cast<ParseState*>(data);
    if (++state.depth > kMaximumDepth) {
        refuse(state, "the document nests elements deeper than 64 levels");
        return;
    }
    auto element = std::make_unique<Element>();
    auto [space, local] = split(name);
    element->space = std::move(space);
    element->name = std::move(local);
    element->line = static_cast<std::size_t>(XML_GetCurrentLineNumber(state.parser));
    element->column = static_cast<std::size_t>(XML_GetCurrentColumnNumber(state.parser)) + 1;
    for (auto** attribute = attributes; attribute[0] != nullptr; attribute += 2) {
        auto [attributeSpace, attributeLocal] = split(attribute[0]);
        element->attributes[attributeSpace.empty() ? attributeLocal
                                                   : attributeSpace + " " + attributeLocal] =
            attribute[1];
    }
    element->parent = state.current;
    auto* raw = element.get();
    if (state.current == nullptr)
        state.root = std::move(element);
    else
        state.current->children.emplace_back(std::move(element));
    state.current = raw;
}

void XMLCALL onEnd(void* data, const XML_Char*) {
    auto& state = *static_cast<ParseState*>(data);
    --state.depth;
    if (state.current != nullptr)
        state.current = state.current->parent;
}

void XMLCALL onText(void* data, const XML_Char* text, const int length) {
    auto& state = *static_cast<ParseState*>(data);
    if (state.current == nullptr)
        return;
    auto& children = state.current->children;
    if (!children.empty())
        if (auto* last = std::get_if<std::string>(&children.back())) {
            last->append(text, static_cast<std::size_t>(length));
            return;
        }
    children.emplace_back(std::string(text, static_cast<std::size_t>(length)));
}

void XMLCALL onDoctype(void* data, const XML_Char*, const XML_Char*, const XML_Char*, int) {
    refuse(*static_cast<ParseState*>(data),
           "a document type declaration (DTD) is refused in subtitle files");
}

void XMLCALL onEntity(void* data, const XML_Char*, int, const XML_Char*, int, const XML_Char*,
                      const XML_Char*, const XML_Char*, const XML_Char*) {
    refuse(*static_cast<ParseState*>(data), "an entity declaration is refused in subtitle files");
}

int XMLCALL onExternalEntity(XML_Parser, const XML_Char*, const XML_Char*, const XML_Char*,
                             const XML_Char*) {
    return XML_STATUS_ERROR;
}

// The attribute-count guard, over the raw text before Expat reads it.
[[nodiscard]] std::optional<std::size_t> elementWithTooManyAttributes(const std::string_view text) {
    std::size_t line = 1;
    std::size_t index = 0;
    while (index < text.size()) {
        const char c = text[index];
        if (c == '\n')
            ++line;
        if (c != '<' || index + 1 >= text.size() || text[index + 1] == '!' ||
            text[index + 1] == '?' || text[index + 1] == '/') {
            ++index;
            continue;
        }
        const auto tagLine = line;
        std::size_t equals = 0;
        char quote = 0;
        ++index;
        while (index < text.size()) {
            const char d = text[index];
            if (d == '\n')
                ++line;
            if (quote != 0) {
                if (d == quote)
                    quote = 0;
            } else if (d == '"' || d == '\'') {
                quote = d;
            } else if (d == '=') {
                if (++equals > kMaximumAttributes)
                    return tagLine;
            } else if (d == '>') {
                break;
            }
            ++index;
        }
    }
    return std::nullopt;
}

// Exact rational seconds.
struct Rational {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;
};
[[nodiscard]] std::optional<Rational> reduce(const core::Int128 numerator,
                                             const core::Int128 denominator) {
    if (denominator == core::Int128(0))
        return std::nullopt;
    auto n = numerator;
    auto d = denominator;
    if (d < core::Int128(0)) {
        n = -n;
        d = -d;
    }
    auto a = n < core::Int128(0) ? -n : n;
    auto b = d;
    while (b != core::Int128(0)) {
        const auto t = a % b;
        a = b;
        b = t;
    }
    if (a != core::Int128(0)) {
        n = n / a;
        d = d / a;
    }
    const auto limit = core::Int128(std::numeric_limits<std::int64_t>::max());
    if (n > limit || -n > limit || d > limit)
        return std::nullopt;
    return Rational{static_cast<std::int64_t>(n), static_cast<std::int64_t>(d)};
}
[[nodiscard]] std::optional<Rational> add(const Rational a, const Rational b) {
    return reduce(core::Int128(a.numerator) * core::Int128(b.denominator) +
                      core::Int128(b.numerator) * core::Int128(a.denominator),
                  core::Int128(a.denominator) * core::Int128(b.denominator));
}
[[nodiscard]] std::optional<Rational> multiply(const Rational a, const Rational b) {
    return reduce(core::Int128(a.numerator) * core::Int128(b.numerator),
                  core::Int128(a.denominator) * core::Int128(b.denominator));
}
[[nodiscard]] bool less(const Rational a, const Rational b) {
    return core::Int128(a.numerator) * core::Int128(b.denominator) <
           core::Int128(b.numerator) * core::Int128(a.denominator);
}
[[nodiscard]] core::RationalTime timeOf(const Rational value) {
    return *core::RationalTime::create(value.numerator, value.denominator);
}

// A decimal "123.4567" exactly; at most nine fraction digits.
[[nodiscard]] std::optional<Rational> decimal(const std::string_view text) {
    const auto dot = text.find('.');
    const auto whole = text.substr(0, dot);
    const auto fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() || fraction.size() > 9 || (dot != std::string_view::npos && fraction.empty()))
        return std::nullopt;
    std::int64_t integer = 0;
    std::int64_t part = 0;
    if (std::from_chars(whole.data(), whole.data() + whole.size(), integer).ptr !=
        whole.data() + whole.size())
        return std::nullopt;
    if (!fraction.empty() && std::from_chars(fraction.data(), fraction.data() + fraction.size(), part)
                                     .ptr != fraction.data() + fraction.size())
        return std::nullopt;
    std::int64_t scale = 1;
    for (std::size_t index = 0; index < fraction.size(); ++index)
        scale *= 10;
    if (integer > (std::int64_t{1} << 40))
        return std::nullopt;
    return reduce(core::Int128(integer) * core::Int128(scale) + core::Int128(part),
                  core::Int128(scale));
}

struct TimingParameters {
    Rational frameRate{30, 1}; // frames per second, the multiplier applied
    Rational tickRate{1, 1};
};

// A TTML time expression in seconds.
[[nodiscard]] std::optional<Rational> timeExpression(const std::string_view raw,
                                                     const TimingParameters& timing) {
    const auto text = trim(raw);
    if (text.empty())
        return std::nullopt;
    if (text.find(':') != std::string_view::npos) {
        // clock-time: hh:mm:ss(.fraction | :frames(.subframes))
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        for (std::size_t index = 0; index <= text.size(); ++index)
            if (index == text.size() || text[index] == ':') {
                parts.push_back(text.substr(start, index - start));
                start = index + 1;
            }
        if (parts.size() != 3 && parts.size() != 4)
            return std::nullopt;
        const auto hours = decimal(parts[0]);
        const auto minutes = decimal(parts[1]);
        const auto seconds = decimal(parts[2]);
        if (!hours || !minutes || !seconds || hours->denominator != 1 || minutes->denominator != 1 ||
            parts[1].size() != 2 || parts[2].substr(0, parts[2].find('.')).size() != 2 ||
            minutes->numerator > 59 || less(Rational{60, 1}, *seconds) ||
            !less(*seconds, Rational{61, 1}))
            return std::nullopt;
        auto total = add(Rational{(hours->numerator * 60 + minutes->numerator) * 60, 1}, *seconds);
        if (parts.size() == 4) {
            if (parts[2].find('.') != std::string_view::npos)
                return std::nullopt;
            const auto frames = decimal(parts[3]);
            if (!frames || !total)
                return std::nullopt;
            const auto inFrames = multiply(*frames, Rational{timing.frameRate.denominator,
                                                             timing.frameRate.numerator});
            if (!inFrames)
                return std::nullopt;
            total = add(*total, *inFrames);
        }
        return total;
    }
    // offset-time: count metric.
    std::size_t index = 0;
    while (index < text.size() && ((text[index] >= '0' && text[index] <= '9') || text[index] == '.'))
        ++index;
    const auto count = decimal(text.substr(0, index));
    const auto metric = text.substr(index);
    if (!count)
        return std::nullopt;
    if (metric == "h")
        return multiply(*count, Rational{3600, 1});
    if (metric == "m")
        return multiply(*count, Rational{60, 1});
    if (metric == "s")
        return *count;
    if (metric == "ms")
        return multiply(*count, Rational{1, 1000});
    if (metric == "f")
        return multiply(*count, Rational{timing.frameRate.denominator, timing.frameRate.numerator});
    if (metric == "t")
        return multiply(*count, Rational{timing.tickRate.denominator, timing.tickRate.numerator});
    return std::nullopt;
}

struct Geometry {
    double rootWidth = 0.0;  // px, 0 when the root names no extent
    double rootHeight = 0.0;
    double cellColumns = 32.0;
    double cellRows = 15.0;
};

// A TTML length as a fraction of the frame along one axis; `vertical` picks the axis.
[[nodiscard]] std::optional<double> lengthFraction(const std::string_view raw, const bool vertical,
                                                   const Geometry& geometry, bool& approximated) {
    const auto text = trim(raw);
    std::size_t index = 0;
    while (index < text.size() && ((text[index] >= '0' && text[index] <= '9') || text[index] == '.' ||
                                   text[index] == '-' || text[index] == '+'))
        ++index;
    double value = 0.0;
    const auto number = text.substr(0, index);
    const auto unit = text.substr(index);
    const auto* first = number.data();
    if (!number.empty() && number.front() == '+')
        ++first;
    if (core::fromChars(first, number.data() + number.size(), value).ptr !=
            number.data() + number.size() ||
        !std::isfinite(value))
        return std::nullopt;
    if (unit == "%")
        return value / 100.0;
    if (unit == "c")
        return value / (vertical ? geometry.cellRows : geometry.cellColumns);
    if (unit == "rh")
        return value / 100.0;
    if (unit == "rw")
        return value / 100.0;
    if (unit == "px") {
        const auto size = vertical ? geometry.rootHeight : geometry.rootWidth;
        if (size > 0.0)
            return value / size;
        approximated = true;
        return value / (vertical ? 1080.0 : 1920.0);
    }
    return std::nullopt;
}

// TTML's colors: CSS's forms, except that rgba() takes its alpha as 0..255.
[[nodiscard]] std::optional<core::Color4d> ttmlColor(const std::string_view raw) {
    const auto text = lower(trim(raw));
    if (text.starts_with("rgba(") && text.ends_with(")")) {
        std::array<double, 4> channels{};
        std::size_t start = 5;
        for (std::size_t which = 0; which < 4; ++which) {
            const auto end = which == 3 ? text.size() - 1 : text.find(',', start);
            if (end == std::string::npos)
                return std::nullopt;
            const auto part = trim(std::string_view(text).substr(start, end - start));
            int value = 0;
            if (std::from_chars(part.data(), part.data() + part.size(), value).ptr !=
                    part.data() + part.size() ||
                value < 0 || value > 255)
                return std::nullopt;
            channels[which] = value / 255.0;
            start = end + 1;
        }
        return core::Color4d{channels[0], channels[1], channels[2], channels[3]};
    }
    return parseCssColor(text);
}

// The styling properties Bloom reads, one element's worth.
struct StyleSet {
    std::optional<core::Color4d> color;
    std::optional<core::Color4d> backgroundColor;
    std::optional<core::Color4d> outlineColor;
    std::optional<double> outlineWidth;
    std::optional<bool> italic;
    std::optional<bool> bold;
    std::optional<bool> underline;
    std::optional<std::string> fontFamily;
    std::optional<double> fontSize;
    std::optional<std::string> textAlign;
    std::optional<std::string> displayAlign;

    void overlay(const StyleSet& above) {
        const auto take = [](auto& mine, const auto& theirs) {
            if (theirs)
                mine = theirs;
        };
        take(color, above.color);
        take(backgroundColor, above.backgroundColor);
        take(outlineColor, above.outlineColor);
        take(outlineWidth, above.outlineWidth);
        take(italic, above.italic);
        take(bold, above.bold);
        take(underline, above.underline);
        take(fontFamily, above.fontFamily);
        take(fontSize, above.fontSize);
        take(textAlign, above.textAlign);
        take(displayAlign, above.displayAlign);
    }
    [[nodiscard]] bool textPropertiesEmpty() const {
        return !color && !backgroundColor && !outlineColor && !outlineWidth && !italic && !bold &&
               !underline && !fontFamily && !fontSize;
    }
    [[nodiscard]] document::CueStyle cueStyle(std::string name) const {
        document::CueStyle style;
        style.name = std::move(name);
        style.color = color;
        style.backgroundColor = backgroundColor;
        style.outlineColor = outlineColor;
        style.outlineWidth = outlineWidth;
        style.italic = italic;
        style.bold = bold;
        style.underline = underline;
        style.fontFamily = fontFamily;
        style.fontSize = fontSize;
        return style;
    }
    friend bool operator==(const StyleSet&, const StyleSet&) = default;
};

class Reader {
  public:
    Reader(const Element& root, Builder& builder) : root_(root), builder_(builder) {}

    std::optional<LoadFailure> run() {
        if (const auto* base = root_.attribute(kParameter, "timeBase");
            base != nullptr && trim(*base) != "media")
            return builder_.failure(root_.line, root_.column, codes::kRefusedFormat,
                                    "line " + std::to_string(root_.line) + ": ttp:timeBase=\"" +
                                        std::string(trim(*base)) +
                                        "\" is not read; Bloom reads media time only");
        readParameters();
        if (const auto* language = root_.attribute(kXml, "lang"))
            builder_.setLanguage(std::string(trim(*language)));
        if (auto image = findImage(root_))
            return builder_.failure(image->line, image->column, codes::kRefusedFormat,
                                    "line " + std::to_string(image->line) +
                                        ": image subtitles (IMSC Image, SMPTE-TT images) are "
                                        "not read; Bloom reads text subtitles");
        for (const auto& child : root_.children)
            if (const auto* element = std::get_if<std::unique_ptr<Element>>(&child))
                if ((*element)->is(kTt, "head"))
                    readHead(**element);
        const Element* body = nullptr;
        for (const auto& child : root_.children)
            if (const auto* element = std::get_if<std::unique_ptr<Element>>(&child))
                if ((*element)->is(kTt, "body"))
                    body = element->get();
        if (body != nullptr)
            walk(*body, Rational{0, 1}, std::nullopt, StyleSet{}, regionOf(*body, nullptr));
        return std::nullopt;
    }

  private:
    struct Region {
        std::optional<double> x, y, width, height;
        StyleSet style;
    };

    static const Element* findImage(const Element& element) {
        if (element.is(kTt, "image") || element.attribute(kSmpte, "backgroundImage") != nullptr)
            return &element;
        for (const auto& child : element.children)
            if (const auto* inner = std::get_if<std::unique_ptr<Element>>(&child))
                if (const auto* found = findImage(**inner))
                    return found;
        return nullptr;
    }

    void readParameters() {
        Rational rate{30, 1};
        bool rateGiven = false;
        if (const auto* text = root_.attribute(kParameter, "frameRate")) {
            if (const auto value = decimal(trim(*text)); value && value->numerator > 0) {
                rate = *value;
                rateGiven = true;
            }
        }
        if (const auto* text = root_.attribute(kParameter, "frameRateMultiplier")) {
            const auto parts = trim(*text);
            const auto space = parts.find(' ');
            if (space != std::string_view::npos) {
                const auto numerator = decimal(trim(parts.substr(0, space)));
                const auto denominator = decimal(trim(parts.substr(space + 1)));
                if (numerator && denominator && numerator->numerator > 0 &&
                    denominator->numerator > 0)
                    if (auto scaled = multiply(rate, Rational{numerator->numerator,
                                                              denominator->numerator}))
                        rate = *scaled;
            }
        }
        timing_.frameRate = rate;
        Rational subFrames{1, 1};
        if (const auto* text = root_.attribute(kParameter, "subFrameRate"))
            if (const auto value = decimal(trim(*text)); value && value->numerator > 0)
                subFrames = *value;
        timing_.tickRate = Rational{1, 1};
        if (rateGiven)
            if (auto ticks = multiply(rate, subFrames))
                timing_.tickRate = *ticks;
        if (const auto* text = root_.attribute(kParameter, "tickRate"))
            if (const auto value = decimal(trim(*text)); value && value->numerator > 0)
                timing_.tickRate = *value;
        if (const auto* text = root_.attribute(kParameter, "cellResolution")) {
            const auto parts = trim(*text);
            const auto space = parts.find(' ');
            if (space != std::string_view::npos) {
                const auto columns = decimal(trim(parts.substr(0, space)));
                const auto rows = decimal(trim(parts.substr(space + 1)));
                if (columns && rows && columns->numerator > 0 && rows->numerator > 0) {
                    geometry_.cellColumns = static_cast<double>(columns->numerator);
                    geometry_.cellRows = static_cast<double>(rows->numerator);
                }
            }
        }
        if (const auto* text = root_.attribute(kStyling, "extent")) {
            const auto parts = trim(*text);
            const auto space = parts.find(' ');
            if (space != std::string_view::npos && parts.ends_with("px")) {
                const auto width = trim(parts.substr(0, space));
                const auto height = trim(parts.substr(space + 1));
                double w = 0.0;
                double h = 0.0;
                (void)core::fromChars(width.data(), width.data() + width.size() - 2, w);
                (void)core::fromChars(height.data(), height.data() + height.size() - 2, h);
                geometry_.rootWidth = w;
                geometry_.rootHeight = h;
            }
        }
    }

    // The tts: attributes of one element (and the dropped ones, noted).
    StyleSet inlineStyle(const Element& element) {
        StyleSet style;
        for (const auto& [key, value] : element.attributes) {
            if (!key.starts_with(std::string(kStyling) + " "))
                continue;
            const auto name = std::string_view(key).substr(kStyling.size() + 1);
            bool approximated = false;
            if (name == "color") {
                style.color = ttmlColor(value);
            } else if (name == "backgroundColor") {
                style.backgroundColor = ttmlColor(value);
            } else if (name == "fontStyle") {
                style.italic = trim(value) == "italic" || trim(value) == "oblique";
            } else if (name == "fontWeight") {
                style.bold = trim(value) == "bold";
            } else if (name == "textDecoration") {
                const auto text = std::string(value);
                if (text.find("noUnderline") != std::string::npos || text == "none")
                    style.underline = false;
                else if (text.find("underline") != std::string::npos)
                    style.underline = true;
                if (text.find("lineThrough") != std::string::npos ||
                    text.find("overline") != std::string::npos)
                    builder_.note(codes::kDropped, "tts:textDecoration line-through and overline",
                                  element.line);
            } else if (name == "textOutline") {
                std::vector<std::string_view> parts;
                std::size_t start = 0;
                const auto text = trim(value);
                for (std::size_t index = 0; index <= text.size(); ++index)
                    if (index == text.size() || text[index] == ' ') {
                        if (index > start)
                            parts.push_back(text.substr(start, index - start));
                        start = index + 1;
                    }
                if (parts.size() == 1 && parts[0] == "none")
                    continue;
                std::size_t at = 0;
                if (!parts.empty())
                    if (auto color = ttmlColor(parts[0])) {
                        style.outlineColor = color;
                        at = 1;
                    }
                if (at < parts.size())
                    style.outlineWidth = lengthFraction(parts[at], true, geometry_, approximated);
                if (at + 1 < parts.size())
                    builder_.note(codes::kDropped, "the blur of tts:textOutline", element.line);
            } else if (name == "fontSize") {
                const auto text = trim(value);
                const auto space = text.rfind(' ');
                const auto vertical = space == std::string_view::npos ? text : text.substr(space + 1);
                if (vertical.ends_with("%") || vertical.ends_with("em")) {
                    // Relative to the parent's size, which is one cell unless a parent sets it.
                    double scale = 0.0;
                    const auto number = vertical.substr(0, vertical.size() - (vertical.ends_with("%") ? 1 : 2));
                    (void)core::fromChars(number.data(), number.data() + number.size(), scale);
                    style.fontSize =
                        (vertical.ends_with("%") ? scale / 100.0 : scale) / geometry_.cellRows;
                } else {
                    style.fontSize = lengthFraction(vertical, true, geometry_, approximated);
                }
            } else if (name == "fontFamily") {
                const auto text = trim(value);
                auto family = trim(text.substr(0, text.find(',')));
                if (family.size() >= 2 && (family.front() == '"' || family.front() == '\''))
                    family = family.substr(1, family.size() - 2);
                static constexpr std::array<std::string_view, 8> kGeneric{
                    "default",           "monospace",          "sansSerif",
                    "serif",             "monospaceSansSerif", "monospaceSerif",
                    "proportionalSansSerif", "proportionalSerif"};
                if (std::ranges::find(kGeneric, family) == kGeneric.end() && !family.empty())
                    style.fontFamily = std::string(family);
            } else if (name == "textAlign") {
                style.textAlign = std::string(trim(value));
            } else if (name == "displayAlign") {
                style.displayAlign = std::string(trim(value));
            } else if (name == "origin" || name == "extent") {
                // Region geometry, read by regionOf().
            } else if (name == "writingMode") {
                if (!trim(value).starts_with("lr") && !trim(value).starts_with("rl"))
                    builder_.note(codes::kApproximated, "vertical text (drawn horizontally)",
                                  element.line);
            } else {
                builder_.note(codes::kDropped, "tts:" + std::string(name), element.line);
            }
            if (approximated)
                builder_.note(codes::kApproximated,
                              "pixel lengths with no tts:extent on the root (read against 1920x1080)",
                              element.line);
        }
        return style;
    }

    // The style an element's `style` attribute names, chained styles first, then its own tts:.
    StyleSet referencedStyle(const Element& element, const std::size_t depth = 0) {
        StyleSet style;
        if (depth > 16)
            return style;
        if (const auto* names = element.attribute({}, "style")) {
            std::size_t start = 0;
            const std::string_view list = *names;
            for (std::size_t index = 0; index <= list.size(); ++index)
                if (index == list.size() || list[index] == ' ') {
                    const auto id = std::string(list.substr(start, index - start));
                    start = index + 1;
                    if (id.empty())
                        continue;
                    const auto found = styles_.find(id);
                    if (found == styles_.end())
                        continue;
                    style.overlay(referencedStyle(*found->second, depth + 1));
                    style.overlay(inlineStyle(*found->second));
                }
        }
        return style;
    }

    StyleSet styleOf(const Element& element) {
        auto style = referencedStyle(element);
        style.overlay(inlineStyle(element));
        return style;
    }

    void readHead(const Element& head) {
        for (const auto& child : head.children) {
            const auto* section = std::get_if<std::unique_ptr<Element>>(&child);
            if (section == nullptr)
                continue;
            for (const auto& entry : (*section)->children) {
                const auto* element = std::get_if<std::unique_ptr<Element>>(&entry);
                if (element == nullptr)
                    continue;
                const auto* id = (*element)->attribute(kXml, "id");
                if ((*element)->is(kTt, "style") && id != nullptr)
                    styles_[*id] = element->get();
                else if ((*element)->is(kTt, "region") && id != nullptr)
                    regions_[*id] = element->get();
                else if ((*element)->is(kMetadata, "agent") && id != nullptr)
                    agents_[*id] = agentName(**element, *id);
            }
        }
    }

    static std::string agentName(const Element& agent, const std::string& id) {
        for (const auto& child : agent.children)
            if (const auto* element = std::get_if<std::unique_ptr<Element>>(&child))
                if ((*element)->is(kMetadata, "name"))
                    for (const auto& text : (*element)->children)
                        if (const auto* value = std::get_if<std::string>(&text))
                            if (!trim(*value).empty())
                                return std::string(trim(*value));
        return id;
    }

    std::optional<Region> regionOf(const Element& element, const std::optional<Region>* inherited) {
        const auto* id = element.attribute({}, "region");
        if (id == nullptr)
            return inherited != nullptr ? *inherited : std::nullopt;
        const auto found = regions_.find(*id);
        if (found == regions_.end())
            return inherited != nullptr ? *inherited : std::nullopt;
        const auto& region = *found->second;
        Region result;
        result.style = styleOf(region);
        bool approximated = false;
        const auto pair = [&](const std::string_view attribute, std::optional<double>& a,
                              std::optional<double>& b) {
            if (const auto* text = region.attribute(kStyling, attribute)) {
                const auto parts = trim(*text);
                const auto space = parts.find(' ');
                if (space == std::string_view::npos)
                    return;
                a = lengthFraction(parts.substr(0, space), false, geometry_, approximated);
                b = lengthFraction(trim(parts.substr(space + 1)), true, geometry_, approximated);
            }
        };
        pair("origin", result.x, result.y);
        pair("extent", result.width, result.height);
        if (approximated)
            builder_.note(codes::kApproximated,
                          "pixel lengths with no tts:extent on the root (read against 1920x1080)",
                          region.line);
        return result;
    }

    std::optional<std::pair<Rational, std::optional<Rational>>>
    interval(const Element& element, const Rational parentBegin,
             const std::optional<Rational> parentEnd) {
        Rational begin = parentBegin;
        if (const auto* text = element.attribute({}, "begin")) {
            const auto offset = timeExpression(*text, timing_);
            if (!offset)
                return std::nullopt;
            if (auto sum = add(parentBegin, *offset))
                begin = *sum;
        }
        std::optional<Rational> end = parentEnd;
        if (const auto* text = element.attribute({}, "end")) {
            const auto offset = timeExpression(*text, timing_);
            if (!offset)
                return std::nullopt;
            auto absolute = add(parentBegin, *offset);
            if (absolute && (!end || less(*absolute, *end)))
                end = absolute;
        } else if (const auto* durText = element.attribute({}, "dur")) {
            const auto duration = timeExpression(*durText, timing_);
            if (!duration)
                return std::nullopt;
            auto fromBegin = add(begin, *duration);
            if (fromBegin && (!end || less(*fromBegin, *end)))
                end = fromBegin;
        }
        return std::pair{begin, end};
    }

    void walk(const Element& element, const Rational parentBegin,
              const std::optional<Rational> parentEnd, StyleSet inherited,
              std::optional<Region> region) {
        if (builder_.stopRequested())
            return;
        for (const auto& child : element.children) {
            const auto* inner = std::get_if<std::unique_ptr<Element>>(&child);
            if (inner == nullptr)
                continue;
            const auto& node = **inner;
            if (!node.is(kTt, "div") && !node.is(kTt, "p")) {
                if (node.space == kTt && (node.name == "set" || node.name == "animate"))
                    builder_.note(codes::kDropped, "<" + node.name + "> animation", node.line);
                continue;
            }
            const auto times = interval(node, parentBegin, parentEnd);
            if (!times) {
                builder_.report(node.line, node.column, codes::kSkippedCue,
                                "line " + std::to_string(node.line) +
                                    ": a begin, end or dur that is not a TTML time expression; "
                                    "the element is skipped");
                continue;
            }
            auto style = inherited;
            style.overlay(styleOf(node));
            auto own = regionOf(node, &region);
            if (node.attribute({}, "timeContainer") != nullptr &&
                *node.attribute({}, "timeContainer") == "seq")
                builder_.note(codes::kApproximated, "timeContainer=\"seq\" (read as par)",
                              node.line);
            if (node.is(kTt, "div")) {
                walk(node, times->first, times->second, style, own);
                continue;
            }
            paragraph(node, times->first, times->second, style, own);
        }
    }

    void paragraph(const Element& p, const Rational begin, const std::optional<Rational> end,
                   const StyleSet& style, const std::optional<Region>& region) {
        if (!end) {
            builder_.report(p.line, p.column, codes::kSkippedCue,
                            "line " + std::to_string(p.line) +
                                ": the paragraph has no end (no end or dur on it or its "
                                "parents); it is skipped");
            return;
        }
        ReadCue cue;
        cue.start = timeOf(begin);
        cue.end = timeOf(*end);
        cue.line = p.line;
        cue.column = p.column;
        if (const auto* agent = p.attribute(kMetadata, "agent")) {
            const auto found = agents_.find(std::string(trim(*agent)));
            cue.speaker = found != agents_.end() ? found->second : std::string(trim(*agent));
        }
        StyleSet effective = style;
        if (region)
            effective = [&] {
                auto merged = region->style;
                merged.overlay(style);
                return merged;
            }();
        std::string text;
        content(p, begin, effective, text);
        cue.text = collapse(text);
        // The paragraph's text style is the cue's style.
        if (!effective.textPropertiesEmpty()) {
            auto textOnly = effective;
            textOnly.textAlign.reset();
            textOnly.displayAlign.reset();
            cue.style = styleName(textOnly, p);
        }
        // Placement from the region, the alignments from the style.
        const auto textAlign = effective.textAlign.value_or("start");
        const auto displayAlign = effective.displayAlign.value_or("before");
        const std::size_t column =
            (textAlign == "center") ? 1 : (textAlign == "right" || textAlign == "end") ? 2 : 0;
        const std::size_t row = displayAlign == "center" ? 1 : displayAlign == "after" ? 2 : 0;
        if (textAlign != "start")
            cue.placement.textAlign = column == 0   ? document::CueTextAlign::Left
                                      : column == 1 ? document::CueTextAlign::Center
                                                    : document::CueTextAlign::Right;
        if (region && region->x && region->y && region->width && region->height) {
            cue.placement.anchor = static_cast<document::CueAnchor>(row * 3 + column);
            cue.placement.positionX = std::clamp(
                *region->x + *region->width * static_cast<double>(column) / 2.0, -1.0, 2.0);
            cue.placement.positionY = std::clamp(
                *region->y + *region->height * static_cast<double>(row) / 2.0, -1.0, 2.0);
            cue.placement.width = std::clamp(*region->width, 1.0e-6, 1.0);
        }
        builder_.addCue(std::move(cue));
    }

    // A span's (or the paragraph's) content as cue markup, `text` collecting it.
    void content(const Element& element, const Rational paragraphBegin, const StyleSet& outer,
                 std::string& text) {
        for (const auto& child : element.children) {
            if (const auto* characters = std::get_if<std::string>(&child)) {
                for (const char c : *characters)
                    text.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
                continue;
            }
            const auto& node = *std::get<std::unique_ptr<Element>>(child);
            if (node.is(kTt, "br")) {
                text += '\x01'; // a line break, kept through whitespace collapsing
                continue;
            }
            if (!node.is(kTt, "span")) {
                if (node.space == kTt)
                    builder_.note(codes::kDropped, "<" + node.name + "> inside a paragraph",
                                  node.line);
                continue;
            }
            std::string open;
            std::string close;
            if (const auto* begin = node.attribute({}, "begin"))
                if (const auto offset = timeExpression(*begin, timing_))
                    if (auto absolute = add(paragraphBegin, *offset)) {
                        const auto millis = multiply(*absolute, Rational{1000, 1});
                        if (millis) {
                            if (millis->denominator != 1)
                                builder_.note(codes::kApproximated,
                                              "word times finer than a millisecond", node.line);
                            text += "<" +
                                    document::cue_markup::timestampText(
                                        millis->numerator / millis->denominator) +
                                    ">";
                        }
                    }
            if (node.attribute({}, "end") != nullptr || node.attribute({}, "dur") != nullptr)
                builder_.note(codes::kApproximated,
                              "a span's end (the word stays until the cue ends)", node.line);
            if (const auto* language = node.attribute(kXml, "lang")) {
                open += "<lang " + document::cue_markup::escapeText(*language) + ">";
                close.insert(0, "</lang>");
            }
            auto own = styleOf(node);
            own.textAlign.reset();
            own.displayAlign.reset();
            // A span that is only italic, bold or underlined is that element.
            const auto plain = [&](const std::optional<bool>& flag) {
                return !flag || *flag;
            };
            if (own.italic && *own.italic && plain(own.italic)) {
                open += "<i>";
                close.insert(0, "</i>");
                own.italic.reset();
            }
            if (own.bold && *own.bold) {
                open += "<b>";
                close.insert(0, "</b>");
                own.bold.reset();
            }
            if (own.underline && *own.underline) {
                open += "<u>";
                close.insert(0, "</u>");
                own.underline.reset();
            }
            if (!own.textPropertiesEmpty()) {
                open += "<c." + styleName(own, node) + ">";
                close.insert(0, "</c>");
            }
            static_cast<void>(outer);
            text += open;
            content(node, paragraphBegin, outer, text);
            text += close;
        }
    }

    // XML text as cue markup: whitespace runs collapse to one space, none at a line's ends.
    static std::string collapse(const std::string& raw) {
        std::string out;
        bool space = false;
        for (std::size_t index = 0; index < raw.size(); ++index) {
            const char c = raw[index];
            if (c == ' ') {
                space = true;
                continue;
            }
            if (c == '\x01') {
                out.push_back('\n');
                space = false;
                continue;
            }
            if (space && !out.empty() && out.back() != '\n')
                out.push_back(' ');
            space = false;
            if (c == '&')
                out += "&amp;";
            else if (c == '<' && raw.compare(index, 1, "<") == 0 && !isMarkupAt(raw, index))
                out += "&lt;";
            else
                out.push_back(c);
        }
        return out;
    }

    // Whether raw[index] starts markup this reader wrote (a tag), not a character from the file.
    static bool isMarkupAt(const std::string& raw, const std::size_t index) {
        // The reader writes markup only as complete tags; XML text never holds a raw '<' (Expat
        // decoded &lt; into one, which then is text). Text '<' is escaped as it is appended.
        return raw.compare(index, 2, "<i") == 0 || raw.compare(index, 2, "<b") == 0 ||
               raw.compare(index, 2, "<u") == 0 || raw.compare(index, 3, "<c.") == 0 ||
               raw.compare(index, 5, "<lang") == 0 || raw.compare(index, 2, "</") == 0 ||
               (index + 1 < raw.size() && raw[index + 1] >= '0' && raw[index + 1] <= '9');
    }

    std::string styleName(const StyleSet& style, const Element& element) {
        // A paragraph or span that names exactly one style and adds nothing keeps that id.
        std::string candidate;
        if (const auto* names = element.attribute({}, "style");
            names != nullptr && std::string_view(*names).find(' ') == std::string_view::npos) {
            const auto found = styles_.find(*names);
            if (found != styles_.end())
                candidate = *names;
        }
        for (const auto& [name, set] : generated_)
            if (set == style)
                return name;
        if (candidate.empty() || generatedNames_.contains(candidate) ||
            document::isBuiltInCueStyleName(candidate))
            candidate = "ttml-" + std::to_string(generated_.size() + 1);
        generated_.emplace_back(candidate, style);
        generatedNames_.insert(candidate);
        builder_.addStyle(style.cueStyle(candidate));
        return candidate;
    }

    const Element& root_;
    Builder& builder_;
    TimingParameters timing_;
    Geometry geometry_;
    std::map<std::string, const Element*> styles_;
    std::map<std::string, const Element*> regions_;
    std::map<std::string, std::string> agents_;
    std::vector<std::pair<std::string, StyleSet>> generated_;
    std::set<std::string> generatedNames_;
};

// Escapes XML text and attribute values.
[[nodiscard]] std::string xmlEscape(const std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(c);
        }
    }
    return out;
}

[[nodiscard]] bool isNcName(const std::string_view name) {
    if (name.empty())
        return false;
    const auto start = [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    if (!start(name.front()))
        return false;
    return std::ranges::all_of(name, [&](const char c) {
        return start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
    });
}

} // namespace

LoadOutcome readTtml(const std::string_view text, Builder& builder, std::string encoding) {
    if (const auto line = elementWithTooManyAttributes(text))
        return builder.failure(*line, 1, codes::kMalformed,
                               "line " + std::to_string(*line) + ": an element carries more than " +
                                   std::to_string(kMaximumAttributes) + " attributes");
    ParseState state;
    const auto parser = std::unique_ptr<std::remove_pointer_t<XML_Parser>, void (*)(XML_Parser)>(
        XML_ParserCreateNS("UTF-8", ' '), XML_ParserFree);
    if (!parser)
        return builder.failure(0, 0, codes::kMalformed, "The XML parser could not start.");
    state.parser = parser.get();
    XML_SetUserData(parser.get(), &state);
    XML_SetElementHandler(parser.get(), onStart, onEnd);
    XML_SetCharacterDataHandler(parser.get(), onText);
    XML_SetStartDoctypeDeclHandler(parser.get(), onDoctype);
    XML_SetEntityDeclHandler(parser.get(), onEntity);
    XML_SetExternalEntityRefHandler(parser.get(), onExternalEntity);
    XML_SetParamEntityParsing(parser.get(), XML_PARAM_ENTITY_PARSING_NEVER);
    // Feed in pieces, so a cancellation is seen while a large document parses.
    constexpr std::size_t kPiece = 1U << 16U;
    std::size_t offset = 0;
    bool ok = true;
    do {
        if (builder.stopRequested())
            return builder.finish(Format::Ttml, encoding);
        const auto size = std::min(kPiece, text.size() - offset);
        const bool last = offset + size >= text.size();
        if (XML_Parse(parser.get(), text.data() + offset, static_cast<int>(size),
                      last ? XML_TRUE : XML_FALSE) != XML_STATUS_OK) {
            ok = false;
            break;
        }
        offset += size;
    } while (offset < text.size());
    if (state.refusal)
        return builder.failure(state.refusalLine, 1, codes::kMalformed,
                               "line " + std::to_string(state.refusalLine) + ": " + *state.refusal);
    if (!ok) {
        const auto line = static_cast<std::size_t>(XML_GetCurrentLineNumber(parser.get()));
        const auto column = static_cast<std::size_t>(XML_GetCurrentColumnNumber(parser.get())) + 1;
        return builder.failure(line, column, codes::kMalformed,
                               "line " + std::to_string(line) + ", column " +
                                   std::to_string(column) + ": the XML does not read: " +
                                   XML_ErrorString(XML_GetErrorCode(parser.get())));
    }
    if (state.root == nullptr || !state.root->is(kTt, "tt"))
        return builder.failure(1, 1, codes::kUnrecognized,
                               "the XML is not TTML: its root is not <tt> in the TTML namespace "
                               "http://www.w3.org/ns/ttml");
    if (auto failed = Reader(*state.root, builder).run())
        return *failed;
    return builder.finish(Format::Ttml, std::move(encoding));
}

namespace {

// The region a cue the node's layout places is written in: the bottom of the 10% safe area.
constexpr std::string_view kDefaultRegion =
    " tts:origin=\"10% 10%\" tts:extent=\"80% 80%\" tts:displayAlign=\"after\" "
    "tts:textAlign=\"center\"";

class TtmlWriter {
  public:
    TtmlWriter(const document::CueListValue& list, const WriteOptions& options,
               std::vector<Report>& reports)
        : list_(list), reports_(reports) {
        // Every time a whole frame of the given rate: frames; else ticks at the least common
        // multiple of the denominators; else milliseconds.
        if (options.frameRate && options.frameRate->first > 0 && options.frameRate->second > 0) {
            const auto [numerator, denominator] = *options.frameRate;
            const auto whole = [&](const core::RationalTime time) {
                const auto frames = core::Int128(time.numerator()) * core::Int128(numerator);
                const auto per = core::Int128(time.denominator()) * core::Int128(denominator);
                return frames % per == core::Int128(0);
            };
            frames_ = std::ranges::all_of(list.cues, [&](const document::CueHandle& cue) {
                return whole(cue->start) && whole(cue->end);
            });
            if (frames_) {
                rateNumerator_ = numerator;
                rateDenominator_ = denominator;
            }
        }
        if (!frames_) {
            std::int64_t lcm = 1;
            for (const auto& cue : list.cues)
                for (const auto time : {cue->start, cue->end}) {
                    lcm = std::lcm(lcm, time.denominator());
                    if (lcm > (std::int64_t{1} << 31)) {
                        lcm = 0;
                        break;
                    }
                }
            tickRate_ = lcm == 0 ? 1000 : lcm;
            milliseconds_ = lcm == 0;
            if (milliseconds_)
                reports_.push_back({0, 0, std::string(codes::kApproximated),
                                    "the cue times share no tick rate TTML can name; they are "
                                    "written to the millisecond"});
        }
    }

    std::string run() {
        // Styles and regions.
        std::map<std::string, std::string> ids;
        std::size_t generated = 0;
        for (const auto& style : list_.styles) {
            auto id = isNcName(style.name) ? style.name : "s" + std::to_string(++generated);
            if (id != style.name)
                reports_.push_back({0, 0, std::string(codes::kApproximated),
                                    "style \"" + style.name + "\" is written with the id " + id});
            ids[style.name] = id;
        }
        styleIds_ = ids;
        std::map<std::string, std::string> agents;
        for (const auto& cue : list_.cues)
            if (!cue->speaker.empty() && !agents.contains(cue->speaker))
                agents[cue->speaker] = "a" + std::to_string(agents.size() + 1);
        std::string body;
        for (const auto& cue : list_.cues) {
            body += "      <p begin=\"" + time(cue->start) + "\" end=\"" + time(cue->end) + "\"";
            if (!cue->style.empty())
                body += " style=\"" + xmlEscape(styleId(cue->style)) + "\"";
            if (!cue->placement.empty())
                body += " region=\"" + region(cue->placement) + "\"";
            else
                body += " region=\"bottom\"";
            if (!cue->speaker.empty())
                body += " ttm:agent=\"" + agents[cue->speaker] + "\"";
            body += ">";
            const auto parsed = document::cue_markup::parse(cue->text);
            body += nodes(parsed.tree.children, *cue);
            body += "</p>\n";
        }
        std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        out += "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:ttp=\"http://www.w3.org/ns/ttml#parameter\" "
               "xmlns:tts=\"http://www.w3.org/ns/ttml#styling\" "
               "xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\" "
               "ttp:contentProfiles=\"http://www.w3.org/ns/ttml/profile/imsc1.1/text\" "
               "ttp:timeBase=\"media\" ttp:cellResolution=\"32 15\"";
        if (frames_) {
            if (rateDenominator_ == 1) {
                out += " ttp:frameRate=\"" + std::to_string(rateNumerator_) + "\"";
            } else {
                // frameRate is an integer; the multiplier makes it exact (30000/1001 is 30 x
                // 1000/1001).
                const std::uint64_t base = (rateNumerator_ + rateDenominator_ - 1) / rateDenominator_;
                const std::uint64_t numerator = rateNumerator_;
                const std::uint64_t denominator = base * rateDenominator_;
                const auto common = std::gcd(numerator, denominator);
                out += " ttp:frameRate=\"" + std::to_string(base) + "\" ttp:frameRateMultiplier=\"" +
                       std::to_string(numerator / common) + " " +
                       std::to_string(denominator / common) + "\"";
            }
        } else if (!milliseconds_) {
            out += " ttp:tickRate=\"" + std::to_string(tickRate_) + "\"";
        }
        out += " xml:lang=\"" + xmlEscape(list_.language == "und" ? "" : list_.language) + "\">\n";
        out += "  <head>\n";
        if (!agents.empty()) {
            out += "    <metadata>\n";
            for (const auto& [name, id] : agents)
                out += "      <ttm:agent xml:id=\"" + id + "\" type=\"person\"><ttm:name type=\"full\">" +
                       xmlEscape(name) + "</ttm:name></ttm:agent>\n";
            out += "    </metadata>\n";
        }
        out += "    <styling>\n";
        for (const auto& style : list_.styles)
            out += "      <style xml:id=\"" + xmlEscape(ids[style.name]) + "\"" + attributes(style) +
                   "/>\n";
        out += "    </styling>\n    <layout>\n";
        out += "      <region xml:id=\"bottom\"" + std::string(kDefaultRegion) + "/>\n";
        for (const auto& [key, id] : regionIds_)
            out += "      <region xml:id=\"" + id + "\"" + key + "/>\n";
        out += "    </layout>\n  </head>\n  <body>\n    <div>\n" + body + "    </div>\n  </body>\n</tt>\n";
        return out;
    }

  private:
    static std::string percent(const double fraction) {
        std::array<char, 64> buffer{};
        // Four decimals: a ten-thousandth of a percent is far below a pixel.
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                                fraction * 100.0, std::chars_format::fixed, 4);
        std::string text(buffer.data(), error == std::errc{} ? end : buffer.data());
        if (text.find('.') != std::string::npos) {
            while (text.back() == '0')
                text.pop_back();
            if (text.back() == '.')
                text.pop_back();
        }
        return text + "%";
    }

    static std::string color(const core::Color4d& value) {
        auto hex = hexName(value);
        if (hex.size() == 6)
            hex += "ff";
        return "#" + hex;
    }

    static std::string attributes(const document::CueStyle& style) {
        std::string out;
        if (style.color)
            out += " tts:color=\"" + color(*style.color) + "\"";
        if (style.backgroundColor)
            out += " tts:backgroundColor=\"" + color(*style.backgroundColor) + "\"";
        if (style.italic)
            out += std::string(" tts:fontStyle=\"") + (*style.italic ? "italic" : "normal") + "\"";
        if (style.bold)
            out += std::string(" tts:fontWeight=\"") + (*style.bold ? "bold" : "normal") + "\"";
        if (style.underline)
            out += std::string(" tts:textDecoration=\"") +
                   (*style.underline ? "underline" : "noUnderline") + "\"";
        if (style.fontFamily)
            out += " tts:fontFamily=\"" + xmlEscape(*style.fontFamily) + "\"";
        if (style.fontSize)
            out += " tts:fontSize=\"" + percent(*style.fontSize).substr(0, percent(*style.fontSize).size() - 1) + "rh\"";
        if (style.outlineColor || style.outlineWidth)
            out += " tts:textOutline=\"" +
                   color(style.outlineColor.value_or(core::Color4d{0.0, 0.0, 0.0, 1.0})) + " " +
                   percent(style.outlineWidth.value_or(0.0)).substr(0, percent(style.outlineWidth.value_or(0.0)).size() - 1) + "rh\"";
        return out;
    }

    std::string styleId(const std::string& name) {
        const auto found = styleIds_.find(name);
        if (found != styleIds_.end())
            return found->second;
        // A built-in class: written as its own style the first time it is used.
        auto id = "builtin-" + name;
        if (!builtIn_.contains(name)) {
            builtIn_.insert(name);
            document::CueStyle style;
            style.name = id;
            if (const auto value = document::builtInCueClassColor(name)) {
                if (name.starts_with("bg_"))
                    style.backgroundColor = value;
                else
                    style.color = value;
            }
            builtInStyles_.push_back(style);
        }
        return id;
    }

    std::string region(const document::CuePlacement& placement) {
        // A region whose anchor point is the placement's position.
        const auto anchor = placement.anchor.value_or(document::CueAnchor::Bottom);
        const auto row = static_cast<std::size_t>(anchor) / 3;
        const auto column = static_cast<std::size_t>(anchor) % 3;
        const auto width = placement.width.value_or(0.8);
        const auto height = 0.8;
        const auto x = placement.positionX.value_or(column == 0 ? 0.1 : column == 1 ? 0.5 : 0.9);
        const auto y = placement.positionY.value_or(row == 0 ? 0.1 : row == 1 ? 0.5 : 0.9);
        const auto originX = x - width * static_cast<double>(column) / 2.0;
        const auto originY = y - height * static_cast<double>(row) / 2.0;
        if (placement.lineOffset)
            reports_.push_back({0, 0, std::string(codes::kApproximated),
                                "a cue's line offset is written as its region's position"});
        const auto display = row == 0 ? "before" : row == 1 ? "center" : "after";
        const auto align = placement.textAlign
                               ? std::string(document::cueTextAlignName(*placement.textAlign))
                               : std::string(column == 0 ? "left" : column == 1 ? "center" : "right");
        const auto key = " tts:origin=\"" + percent(originX) + " " + percent(originY) +
                         "\" tts:extent=\"" + percent(width) + " " + percent(height) +
                         "\" tts:displayAlign=\"" + display + "\" tts:textAlign=\"" + align + "\"";
        if (key == kDefaultRegion)
            return "bottom";
        const auto found = regionIds_.find(key);
        if (found != regionIds_.end())
            return found->second;
        auto id = "r" + std::to_string(regionIds_.size() + 1);
        regionIds_.emplace(key, id);
        return id;
    }

    std::string time(const core::RationalTime value) const {
        if (frames_) {
            const auto frames = core::Int128(value.numerator()) * core::Int128(rateNumerator_) /
                                (core::Int128(value.denominator()) * core::Int128(rateDenominator_));
            return std::to_string(static_cast<std::int64_t>(frames)) + "f";
        }
        if (milliseconds_)
            return document::cueTimestampText(value);
        const auto ticks = core::Int128(value.numerator()) * core::Int128(tickRate_) /
                           core::Int128(value.denominator());
        return std::to_string(static_cast<std::int64_t>(ticks)) + "t";
    }

    std::string nodes(const std::vector<document::cue_markup::Node>& children,
                      const document::Cue& cue) {
        using Kind = document::cue_markup::NodeKind;
        std::string out;
        std::size_t open = 0;
        for (const auto& node : children) {
            switch (node.kind) {
            case Kind::Text: {
                const auto lines = splitLines(node.text);
                for (std::size_t index = 0; index < lines.size(); ++index) {
                    if (index > 0)
                        out += "<br/>";
                    out += xmlEscape(lines[index].text);
                }
                break;
            }
            case Kind::Italic:
                out += "<span tts:fontStyle=\"italic\">" + nodes(node.children, cue) + "</span>";
                break;
            case Kind::Bold:
                out += "<span tts:fontWeight=\"bold\">" + nodes(node.children, cue) + "</span>";
                break;
            case Kind::Underline:
                out += "<span tts:textDecoration=\"underline\">" + nodes(node.children, cue) +
                       "</span>";
                break;
            case Kind::Class: {
                std::string names;
                for (const auto& name : node.classes)
                    names += (names.empty() ? "" : " ") + styleId(name);
                out += names.empty() ? nodes(node.children, cue)
                                     : "<span style=\"" + xmlEscape(names) + "\">" +
                                           nodes(node.children, cue) + "</span>";
                break;
            }
            case Kind::Voice:
                out += nodes(node.children, cue);
                break;
            case Kind::Language:
                out += "<span xml:lang=\"" + xmlEscape(node.text) + "\">" +
                       nodes(node.children, cue) + "</span>";
                break;
            case Kind::Ruby:
                for (const auto& child : node.children)
                    if (child.kind != Kind::RubyText)
                        out += nodes({child}, cue);
                    else
                        reports_.push_back({0, 0, std::string(codes::kDropped),
                                            "cue " + std::to_string(cue.id) +
                                                ": ruby text is not written to TTML"});
                break;
            case Kind::RubyText:
                break;
            case Kind::Timestamp: {
                // The rest of this level, timed from the timestamp (relative to the paragraph).
                const auto at = *core::RationalTime::create(node.milliseconds, 1000);
                const auto offset = core::Int128(at.numerator()) * core::Int128(cue.start.denominator()) -
                                    core::Int128(cue.start.numerator()) * core::Int128(at.denominator());
                if (offset < core::Int128(0))
                    break;
                const auto relative = core::RationalTime::create(
                    static_cast<std::int64_t>(offset),
                    static_cast<std::int64_t>(core::Int128(at.denominator()) *
                                              core::Int128(cue.start.denominator())));
                if (!relative)
                    break;
                out += "<span begin=\"" + relativeTime(*relative) + "\">";
                ++open;
                break;
            }
            }
        }
        for (; open > 0; --open)
            out += "</span>";
        return out;
    }

    std::string relativeTime(const core::RationalTime value) const {
        // Word times are milliseconds; written as seconds, exact.
        const auto millis = core::Int128(value.numerator()) * core::Int128(1000) /
                            core::Int128(value.denominator());
        const auto whole = static_cast<std::int64_t>(millis);
        auto text = std::to_string(whole / 1000) + "." + std::to_string(1000 + whole % 1000).substr(1);
        return text + "s";
    }

    const document::CueListValue& list_;
    std::vector<Report>& reports_;
    bool frames_ = false;
    bool milliseconds_ = false;
    std::uint32_t rateNumerator_ = 0;
    std::uint32_t rateDenominator_ = 1;
    std::int64_t tickRate_ = 1000;
    std::map<std::string, std::string> styleIds_;
    std::map<std::string, std::string> regionIds_;
    std::set<std::string> builtIn_;

  public:
    std::vector<document::CueStyle> builtInStyles_;
};

} // namespace

std::string writeTtml(const document::CueListValue& cues, const WriteOptions& options,
                      std::vector<Report>& reports) {
    TtmlWriter writer(cues, options, reports);
    auto text = writer.run();
    if (!writer.builtInStyles_.empty()) {
        // Built-in classes used in the cues, written into the styling section.
        std::string extra;
        for (const auto& style : writer.builtInStyles_) {
            extra += "      <style xml:id=\"" + style.name + "\"";
            if (style.color)
                extra += " tts:color=\"#" + hexName(*style.color) + "ff\"";
            if (style.backgroundColor)
                extra += " tts:backgroundColor=\"#" + hexName(*style.backgroundColor) + "ff\"";
            extra += "/>\n";
        }
        const auto at = text.find("    </styling>");
        text.insert(at, extra);
    }
    return text;
}

} // namespace bloom::subtitles::detail
