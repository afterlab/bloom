#pragma once

// #752: Bloom's cue markup is WebVTT cue text (W3C WebVTT, "WebVTT cue text parsing rules"): the
// elements <c.class> <i> <b> <u> <ruby> <rt> <v name> <lang tag>, timestamp tags <00:00:01.500>,
// HTML character references and '\n' line breaks. parse() is the specification's tokenizer and
// tree builder, step by step, so it reads anything a WebVTT file can hold the way a browser does;
// strictProblem() is the narrower rule a cue's text is held to where it enters the document.
// docs/architecture/subtitles.md, "Cue markup".

#include <bloom/core/rational_time.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::document::cue_markup {

enum class NodeKind : std::uint8_t {
    Text,
    Class,     // <c>
    Italic,    // <i>
    Bold,      // <b>
    Underline, // <u>
    Ruby,      // <ruby>
    RubyText,  // <rt>, only inside <ruby>
    Voice,     // <v name>
    Language,  // <lang tag>
    Timestamp, // <00:00:01.500>
};

struct Node {
    NodeKind kind = NodeKind::Text;
    // Text: the characters, references decoded. Voice and Language: the annotation.
    std::string text;
    std::vector<std::string> classes;
    // Timestamp: the time the tag names, in milliseconds.
    std::int64_t milliseconds = 0;
    std::vector<Node> children;

    friend bool operator==(const Node&, const Node&) = default;
};

struct Tree {
    std::vector<Node> children;
    friend bool operator==(const Tree&, const Tree&) = default;
};

// What a lenient parse ignored, with the byte offset of the token in the text.
struct Issue {
    std::size_t offset = 0;
    std::string message;
};

struct ParseResult {
    Tree tree;
    std::vector<Issue> ignored;
};

// The WebVTT cue text parsing rules. Never fails: what the rules ignore is listed in `ignored`.
[[nodiscard]] ParseResult parse(std::string_view text);

// The rule for a cue's text in the document: everything parse() reads, with nothing ignored (no
// unknown tag, no <rt> outside <ruby>, no end tag that closes nothing, no malformed timestamp).
// Returns the problem, naming the offset, or nullopt.
[[nodiscard]] std::optional<std::string> strictProblem(std::string_view text);

// The characters a cue shows: its text with every tag removed and references decoded. Ruby text
// (<rt>) is left out; the base text is what is drawn.
[[nodiscard]] std::string plainText(const Tree& tree);
[[nodiscard]] std::string plainText(std::string_view markup);

// The canonical markup of a tree: every element closed, '&', '<' and '>' in text escaped.
[[nodiscard]] std::string serialize(const Tree& tree);
// Plain characters as markup text.
[[nodiscard]] std::string escapeText(std::string_view plain);

// "HH:MM:SS.mmm" (at least two hour digits), the WebVTT timestamp syntax.
[[nodiscard]] std::string timestampText(std::int64_t milliseconds);

// The WebVTT "collect a WebVTT timestamp" rule over `text` from `position`; advances `position`
// past what it read. Returns whole milliseconds, or nullopt (position then undefined).
[[nodiscard]] std::optional<std::int64_t> collectTimestamp(std::string_view text,
                                                           std::size_t& position);

// The HTML character reference at text[position] (just past the '&'), as the HTML tokenizer's
// "consume a character reference" reads it outside an attribute, `allowed` being the additional
// allowed character (0 for none). On a match appends the characters to `out`, advances
// `position` and returns true; otherwise leaves both unchanged and returns false.
[[nodiscard]] bool consumeCharacterReference(std::string_view text, std::size_t& position,
                                             char allowed, std::string& out);

} // namespace bloom::document::cue_markup
