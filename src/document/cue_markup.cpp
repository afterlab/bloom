// #752: WebVTT cue text, docs/architecture/subtitles.md "Cue markup". The tokenizer and the tree
// builder follow the W3C WebVTT specification's "WebVTT cue text tokenizer" and "WebVTT cue text
// parsing rules" state by state; the character references follow the HTML tokenizer's "consume a
// character reference" outside an attribute, over the HTML Standard's full table of names.
#include <bloom/document/cue_markup.hpp>

#include <bloom/core/utf8.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace bloom::document::cue_markup {
namespace {

struct HtmlNamedReference {
    std::string_view name;
    char32_t first;
    char32_t second;
};

#include "html_named_references.inc"

constexpr std::size_t kLongestReferenceName = [] {
    std::size_t longest = 0;
    for (const auto& reference : kHtmlNamedReferences)
        longest = std::max(longest, reference.name.size());
    return longest;
}();

void appendUtf8(std::string& out, const char32_t point) {
    if (point < 0x80) {
        out.push_back(static_cast<char>(point));
    } else if (point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (point >> 6)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else if (point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    }
}

[[nodiscard]] bool isAsciiDigit(const char c) noexcept { return c >= '0' && c <= '9'; }
[[nodiscard]] bool isAsciiHexDigit(const char c) noexcept {
    return isAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
[[nodiscard]] bool isAsciiWhitespace(const char c) noexcept {
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

// The HTML tokenizer's replacements for numeric references to the C1 range (windows-1252).
[[nodiscard]] char32_t numericReplacement(const std::uint32_t number) noexcept {
    static constexpr std::array<char32_t, 32> kC1{
        0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x8D,   0x017D, 0x8F,   0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178};
    if (number == 0 || number > 0x10FFFF || (number >= 0xD800 && number <= 0xDFFF))
        return 0xFFFD;
    if (number >= 0x80 && number <= 0x9F)
        return kC1[number - 0x80];
    return static_cast<char32_t>(number);
}

[[nodiscard]] const HtmlNamedReference* findReference(const std::string_view name) noexcept {
    const auto found = std::lower_bound(
        kHtmlNamedReferences.begin(), kHtmlNamedReferences.end(), name,
        [](const HtmlNamedReference& entry, const std::string_view key) { return entry.name < key; });
    return found != kHtmlNamedReferences.end() && found->name == name ? &*found : nullptr;
}

enum class TokenKind : std::uint8_t { String, StartTag, EndTag, Timestamp };

struct Token {
    TokenKind kind = TokenKind::String;
    std::string value; // the string, the tag name, or the timestamp text
    std::vector<std::string> classes;
    std::string annotation;
    std::size_t offset = 0;
};

// The WebVTT cue text tokenizer: one token from `position`.
Token nextToken(const std::string_view input, std::size_t& position) {
    enum class State : std::uint8_t {
        Data,
        Tag,
        StartTag,
        StartTagClass,
        StartTagAnnotation,
        EndTag,
        Timestamp
    };
    auto state = State::Data;
    std::string result;
    std::string buffer;
    std::vector<std::string> classes;
    Token token;
    token.offset = position;
    const auto finishAnnotation = [&] {
        // Strip leading and trailing ASCII whitespace and collapse each run to one space.
        std::string collapsed;
        bool space = false;
        for (const char c : buffer) {
            if (isAsciiWhitespace(c)) {
                space = !collapsed.empty();
            } else {
                if (space)
                    collapsed.push_back(' ');
                space = false;
                collapsed.push_back(c);
            }
        }
        return collapsed;
    };
    while (true) {
        const bool end = position >= input.size();
        const char c = end ? '\0' : input[position];
        switch (state) {
        case State::Data:
            if (!end && c == '&') {
                ++position;
                if (!consumeCharacterReference(input, position, '\0', result))
                    result.push_back('&');
                continue;
            }
            if (!end && c == '<') {
                if (result.empty()) {
                    state = State::Tag;
                    ++position;
                    continue;
                }
                token.kind = TokenKind::String;
                token.value = std::move(result);
                return token;
            }
            if (end) {
                token.kind = TokenKind::String;
                token.value = std::move(result);
                return token;
            }
            result.push_back(c);
            ++position;
            continue;
        case State::Tag:
            if (end) {
                token.kind = TokenKind::StartTag;
                return token;
            }
            if (c == '\t' || c == '\n' || c == '\f' || c == ' ') {
                state = State::StartTagAnnotation;
            } else if (c == '.') {
                state = State::StartTagClass;
            } else if (c == '/') {
                state = State::EndTag;
            } else if (isAsciiDigit(c)) {
                result.push_back(c);
                state = State::Timestamp;
            } else if (c == '>') {
                ++position;
                token.kind = TokenKind::StartTag;
                return token;
            } else {
                result.push_back(c);
                state = State::StartTag;
            }
            ++position;
            continue;
        case State::StartTag:
            if (end) {
                token.kind = TokenKind::StartTag;
                token.value = std::move(result);
                return token;
            }
            if (c == '\t' || c == '\f' || c == ' ') {
                state = State::StartTagAnnotation;
            } else if (c == '\n') {
                buffer.push_back(c);
                state = State::StartTagAnnotation;
            } else if (c == '.') {
                state = State::StartTagClass;
            } else if (c == '>') {
                ++position;
                token.kind = TokenKind::StartTag;
                token.value = std::move(result);
                return token;
            } else {
                result.push_back(c);
            }
            ++position;
            continue;
        case State::StartTagClass:
            if (end) {
                classes.push_back(std::move(buffer));
                token.kind = TokenKind::StartTag;
                token.value = std::move(result);
                token.classes = std::move(classes);
                return token;
            }
            if (c == '\t' || c == '\f' || c == ' ') {
                classes.push_back(std::move(buffer));
                buffer.clear();
                state = State::StartTagAnnotation;
            } else if (c == '\n') {
                classes.push_back(std::move(buffer));
                buffer.assign(1, c);
                state = State::StartTagAnnotation;
            } else if (c == '.') {
                classes.push_back(std::move(buffer));
                buffer.clear();
            } else if (c == '>') {
                ++position;
                classes.push_back(std::move(buffer));
                token.kind = TokenKind::StartTag;
                token.value = std::move(result);
                token.classes = std::move(classes);
                return token;
            } else {
                buffer.push_back(c);
            }
            ++position;
            continue;
        case State::StartTagAnnotation:
            if (end || c == '>') {
                if (!end)
                    ++position;
                token.kind = TokenKind::StartTag;
                token.value = std::move(result);
                token.classes = std::move(classes);
                token.annotation = finishAnnotation();
                return token;
            }
            if (c == '&') {
                ++position;
                if (!consumeCharacterReference(input, position, '>', buffer))
                    buffer.push_back('&');
                continue;
            }
            buffer.push_back(c);
            ++position;
            continue;
        case State::EndTag:
            if (end || c == '>') {
                if (!end)
                    ++position;
                token.kind = TokenKind::EndTag;
                token.value = std::move(result);
                return token;
            }
            result.push_back(c);
            ++position;
            continue;
        case State::Timestamp:
            if (end || c == '>') {
                if (!end)
                    ++position;
                token.kind = TokenKind::Timestamp;
                token.value = std::move(result);
                return token;
            }
            result.push_back(c);
            ++position;
            continue;
        }
    }
}

[[nodiscard]] std::string_view elementName(const NodeKind kind) noexcept {
    switch (kind) {
    case NodeKind::Class:
        return "c";
    case NodeKind::Italic:
        return "i";
    case NodeKind::Bold:
        return "b";
    case NodeKind::Underline:
        return "u";
    case NodeKind::Ruby:
        return "ruby";
    case NodeKind::RubyText:
        return "rt";
    case NodeKind::Voice:
        return "v";
    case NodeKind::Language:
        return "lang";
    case NodeKind::Text:
    case NodeKind::Timestamp:
        break;
    }
    return {};
}

void appendPlain(const std::vector<Node>& nodes, std::string& out) {
    for (const auto& node : nodes) {
        if (node.kind == NodeKind::Text)
            out += node.text;
        else if (node.kind != NodeKind::RubyText && node.kind != NodeKind::Timestamp)
            appendPlain(node.children, out);
    }
}

void appendMarkup(const std::vector<Node>& nodes, std::string& out) {
    for (const auto& node : nodes) {
        switch (node.kind) {
        case NodeKind::Text:
            out += escapeText(node.text);
            continue;
        case NodeKind::Timestamp:
            out += '<';
            out += timestampText(node.milliseconds);
            out += '>';
            continue;
        default:
            break;
        }
        const auto name = elementName(node.kind);
        out += '<';
        out += name;
        for (const auto& name_ : node.classes) {
            out += '.';
            out += name_;
        }
        if ((node.kind == NodeKind::Voice || node.kind == NodeKind::Language) &&
            !node.text.empty()) {
            out += ' ';
            out += escapeText(node.text);
        }
        out += '>';
        appendMarkup(node.children, out);
        out += "</";
        out += name;
        out += '>';
    }
}

} // namespace

bool consumeCharacterReference(const std::string_view text, std::size_t& position,
                               const char allowed, std::string& out) {
    if (position >= text.size())
        return false;
    const char first = text[position];
    if (first == '\t' || first == '\n' || first == '\f' || first == ' ' || first == '<' ||
        first == '&' || (allowed != '\0' && first == allowed))
        return false;
    if (first == '#') {
        auto cursor = position + 1;
        bool hex = false;
        if (cursor < text.size() && (text[cursor] == 'x' || text[cursor] == 'X')) {
            hex = true;
            ++cursor;
        }
        const auto digitsStart = cursor;
        std::uint32_t number = 0;
        bool overflow = false;
        while (cursor < text.size() &&
               (hex ? isAsciiHexDigit(text[cursor]) : isAsciiDigit(text[cursor]))) {
            const char c = text[cursor];
            const std::uint32_t digit = isAsciiDigit(c)               ? std::uint32_t(c - '0')
                                        : (c >= 'a' && c <= 'f') ? std::uint32_t(c - 'a' + 10)
                                                                      : std::uint32_t(c - 'A' + 10);
            if (number > 0x10FFFF)
                overflow = true;
            else
                number = number * (hex ? 16U : 10U) + digit;
            ++cursor;
        }
        if (cursor == digitsStart)
            return false;
        if (cursor < text.size() && text[cursor] == ';')
            ++cursor;
        appendUtf8(out, overflow ? char32_t{0xFFFD} : numericReplacement(number));
        position = cursor;
        return true;
    }
    // The longest name in the table that the text begins with.
    const auto available = std::min(kLongestReferenceName, text.size() - position);
    for (auto length = available; length > 0; --length) {
        if (const auto* reference = findReference(text.substr(position, length))) {
            appendUtf8(out, reference->first);
            if (reference->second != 0)
                appendUtf8(out, reference->second);
            position += length;
            return true;
        }
    }
    return false;
}

std::optional<std::int64_t> collectTimestamp(const std::string_view text, std::size_t& position) {
    const auto digits = [&](std::int64_t& value) {
        const auto start = position;
        value = 0;
        while (position < text.size() && isAsciiDigit(text[position])) {
            if (value < (std::int64_t{1} << 40))
                value = value * 10 + (text[position] - '0');
            ++position;
        }
        return position - start;
    };
    bool hours = false;
    std::int64_t value1 = 0;
    std::int64_t value2 = 0;
    std::int64_t value3 = 0;
    std::int64_t value4 = 0;
    if (position >= text.size() || !isAsciiDigit(text[position]))
        return std::nullopt;
    const auto length1 = digits(value1);
    if (length1 != 2 || value1 > 59)
        hours = true;
    if (position >= text.size() || text[position] != ':')
        return std::nullopt;
    ++position;
    if (digits(value2) != 2)
        return std::nullopt;
    if (hours || (position < text.size() && text[position] == ':')) {
        if (position >= text.size() || text[position] != ':')
            return std::nullopt;
        ++position;
        if (digits(value3) != 2)
            return std::nullopt;
    } else {
        value3 = value2;
        value2 = value1;
        value1 = 0;
    }
    if (position >= text.size() || text[position] != '.')
        return std::nullopt;
    ++position;
    if (digits(value4) != 3)
        return std::nullopt;
    if (value2 > 59 || value3 > 59)
        return std::nullopt;
    if (value1 > (std::int64_t{1} << 30))
        return std::nullopt;
    return ((value1 * 60 + value2) * 60 + value3) * 1000 + value4;
}

ParseResult parse(const std::string_view text) {
    ParseResult result;
    // The path from the root to the current node: indices into each level's children.
    std::vector<Node*> stack;
    const auto children = [&]() -> std::vector<Node>& {
        return stack.empty() ? result.tree.children : stack.back()->children;
    };
    const auto current = [&]() -> Node* { return stack.empty() ? nullptr : stack.back(); };
    std::size_t position = 0;
    while (position < text.size()) {
        auto token = nextToken(text, position);
        switch (token.kind) {
        case TokenKind::String:
            if (!token.value.empty())
                {
                Node node;
                node.text = std::move(token.value);
                children().push_back(std::move(node));
            }
            break;
        case TokenKind::StartTag: {
            std::optional<NodeKind> kind;
            if (token.value == "c")
                kind = NodeKind::Class;
            else if (token.value == "i")
                kind = NodeKind::Italic;
            else if (token.value == "b")
                kind = NodeKind::Bold;
            else if (token.value == "u")
                kind = NodeKind::Underline;
            else if (token.value == "ruby")
                kind = NodeKind::Ruby;
            else if (token.value == "rt" && current() != nullptr &&
                     current()->kind == NodeKind::Ruby)
                kind = NodeKind::RubyText;
            else if (token.value == "v")
                kind = NodeKind::Voice;
            else if (token.value == "lang")
                kind = NodeKind::Language;
            if (!kind) {
                result.ignored.push_back(
                    {token.offset, token.value == "rt"
                                       ? std::string("<rt> outside <ruby>")
                                       : "unknown tag <" + token.value + ">"});
                break;
            }
            Node node;
            node.kind = *kind;
            for (auto& name : token.classes)
                if (!name.empty())
                    node.classes.push_back(std::move(name));
            if (*kind == NodeKind::Voice || *kind == NodeKind::Language)
                node.text = std::move(token.annotation);
            auto& level = children();
            level.push_back(std::move(node));
            stack.push_back(&level.back());
            break;
        }
        case TokenKind::EndTag: {
            const auto* open = current();
            if (open != nullptr && token.value == elementName(open->kind)) {
                stack.pop_back();
            } else if (open != nullptr && token.value == "ruby" &&
                       open->kind == NodeKind::RubyText) {
                stack.pop_back();
                stack.pop_back();
            } else {
                result.ignored.push_back(
                    {token.offset, "end tag </" + token.value + "> closes nothing"});
            }
            break;
        }
        case TokenKind::Timestamp: {
            std::size_t cursor = 0;
            const auto time = collectTimestamp(token.value, cursor);
            if (time && cursor == token.value.size())
            {
                Node node;
                node.kind = NodeKind::Timestamp;
                node.milliseconds = *time;
                children().push_back(std::move(node));
            }
            else
                result.ignored.push_back(
                    {token.offset, "<" + token.value + "> is not a timestamp"});
            break;
        }
        }
    }
    return result;
}

std::optional<std::string> strictProblem(const std::string_view text) {
    auto parsed = parse(text);
    if (parsed.ignored.empty())
        return std::nullopt;
    const auto& first = parsed.ignored.front();
    return "the markup at byte " + std::to_string(first.offset) + " does not read: " +
           first.message;
}

std::string plainText(const Tree& tree) {
    std::string out;
    appendPlain(tree.children, out);
    return out;
}

std::string plainText(const std::string_view markup) { return plainText(parse(markup).tree); }

std::string serialize(const Tree& tree) {
    std::string out;
    appendMarkup(tree.children, out);
    return out;
}

std::string escapeText(const std::string_view plain) {
    std::string out;
    out.reserve(plain.size());
    for (const char c : plain) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out.push_back(c);
    }
    return out;
}

std::string timestampText(const std::int64_t milliseconds) {
    const auto clamped = std::max<std::int64_t>(milliseconds, 0);
    const auto hours = clamped / 3600000;
    const auto minutes = clamped / 60000 % 60;
    const auto seconds = clamped / 1000 % 60;
    const auto millis = clamped % 1000;
    std::string out = std::to_string(hours);
    if (out.size() < 2)
        out.insert(0, 2 - out.size(), '0');
    const auto two = [&](const std::int64_t value) {
        out.push_back(static_cast<char>('0' + value / 10));
        out.push_back(static_cast<char>('0' + value % 10));
    };
    out.push_back(':');
    two(minutes);
    out.push_back(':');
    two(seconds);
    out.push_back('.');
    out.push_back(static_cast<char>('0' + millis / 100));
    out.push_back(static_cast<char>('0' + millis / 10 % 10));
    out.push_back(static_cast<char>('0' + millis % 10));
    return out;
}

} // namespace bloom::document::cue_markup
