#pragma once

// #752: a list of timed subtitle cues, the value of the cue-list socket and parameter kind. One
// list is one language: cues with stable ids, exact rational times and text in WebVTT cue-text
// markup, the named style overrides they refer to, and where the list came from.
// docs/architecture/subtitles.md is the specification.
//
// Cues are held by shared immutable pointers, so a copy of a list (a history entry, a draft, a
// compiled plan) shares every cue it did not change: an edit to one cue allocates that cue and one
// pointer per cue, never a copy of every cue's text.

#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::document {

inline constexpr std::string_view kCueListValueParameterSchemaKey = "bloom.subtitles.cues";

// The limits a list is held to wherever it enters the document (docs/architecture/subtitles.md,
// "Limits"). MCP advertises each under its own name.
inline constexpr std::size_t kMaximumCues = 50000;                    // subtitleCues
inline constexpr std::size_t kMaximumCueTextBytes = 4096;             // subtitleCueTextBytes
inline constexpr std::size_t kMaximumCueLines = 16;                   // before wrapping
inline constexpr std::size_t kMaximumCueStyles = 256;                 // subtitleStyles
inline constexpr std::size_t kMaximumCueNameBytes = 256;              // a speaker or style name
inline constexpr std::size_t kMaximumSubtitleFileBytes = 16U << 20U;  // subtitleFileBytes
inline constexpr std::size_t kMaximumActiveCues = 16;                 // subtitleActiveCues
inline constexpr std::size_t kMaximumSubtitleGlyphsPerFrame = 8192;
inline constexpr std::size_t kMaximumCueLanguageBytes = 64;
inline constexpr std::size_t kMaximumCueLocatorBytes = 4096;
// The largest time a cue may name: about 31.7 years, far past any composition, small enough that
// every sum and difference of two cue times stays exact in RationalTime's 64-bit terms.
inline constexpr std::int64_t kMaximumCueSeconds = std::int64_t{1} << 30;

// Nine-point anchor, row by row from the top left.
enum class CueAnchor : std::uint8_t {
    TopLeft,
    Top,
    TopRight,
    Left,
    Center,
    Right,
    BottomLeft,
    Bottom,
    BottomRight,
};
inline constexpr std::size_t kCueAnchorCount = 9;

enum class CueTextAlign : std::uint8_t { Left, Center, Right };

[[nodiscard]] std::string_view cueAnchorName(CueAnchor anchor) noexcept;
[[nodiscard]] std::optional<CueAnchor> cueAnchorFromName(std::string_view name) noexcept;
[[nodiscard]] std::string_view cueTextAlignName(CueTextAlign align) noexcept;
[[nodiscard]] std::optional<CueTextAlign> cueTextAlignFromName(std::string_view name) noexcept;

// Where a cue asks to be drawn, from its file (a WebVTT cue's settings, an ASS \an or \pos, a TTML
// region). Every member is optional: a cue that names only its alignment keeps the node's anchor.
// `position` is a fraction of the frame, x to the right and y down, of the anchor point; `width` a
// fraction of the frame's width; `lineOffset` whole lines from the anchored edge.
struct CuePlacement {
    std::optional<CueAnchor> anchor;
    std::optional<double> positionX;
    std::optional<double> positionY;
    std::optional<std::int64_t> lineOffset;
    std::optional<double> width;
    std::optional<CueTextAlign> textAlign;

    [[nodiscard]] bool empty() const noexcept {
        return !anchor && !positionX && !positionY && !lineOffset && !width && !textAlign;
    }
    friend bool operator==(const CuePlacement&, const CuePlacement&) = default;
};

// A named style override. Colors are as subtitle formats write them: straight (not premultiplied)
// sRGB-encoded display values in 0..1, so a file's #RRGGBB reads back exactly; the render converts
// them into the working space. Sizes are fractions of the frame height, so a style means the same
// thing at every composition size and proxy scale.
struct CueStyle {
    std::string name;
    std::optional<core::Color4d> color;
    std::optional<core::Color4d> backgroundColor;
    std::optional<core::Color4d> outlineColor;
    std::optional<double> outlineWidth;
    std::optional<bool> bold;
    std::optional<bool> italic;
    std::optional<bool> underline;
    std::optional<std::string> fontFamily;
    std::optional<double> fontSize;

    friend bool operator==(const CueStyle&, const CueStyle&) = default;
};

// One cue. `start` and `end` are exact seconds on the node's clock, the end exclusive: a cue shows
// on frame k when start <= t_k < end. `speaker` and `style` are empty when the cue names none.
struct Cue {
    std::uint64_t id = 0;
    core::RationalTime start;
    core::RationalTime end;
    std::string text;
    std::string speaker;
    std::string style;
    CuePlacement placement;

    friend bool operator==(const Cue&, const Cue&) = default;
};

using CueHandle = std::shared_ptr<const Cue>;

// Where a list came from.
struct CueAuthoredProvenance {
    friend bool operator==(const CueAuthoredProvenance&, const CueAuthoredProvenance&) = default;
};
// A list loaded from (and linked to) a file. The cues are held in the project, so a render never
// reads the file; the digest and stamp are what the linked-file monitor compares.
struct CueFileProvenance {
    // As an asset locator: "project-relative" or "absolute", and the path in that form ('/'
    // separators).
    std::string portability = "project-relative";
    std::string path;
    std::string format;   // srt, vtt, ttml or ass
    std::string encoding; // utf-8, utf-16le, utf-16be or a WHATWG single-byte encoding name
    std::string digest;   // "sha256:" and 64 lower-case hex digits of the file's bytes
    std::uint64_t size = 0;
    std::int64_t modifiedNanoseconds = 0;
    friend bool operator==(const CueFileProvenance&, const CueFileProvenance&) = default;
};
// A list a producer wrote (a transcription job, a script).
struct CueProducerProvenance {
    std::string name;
    std::string version;
    std::string parametersDigest;
    friend bool operator==(const CueProducerProvenance&, const CueProducerProvenance&) = default;
};
using CueProvenance =
    std::variant<CueAuthoredProvenance, CueFileProvenance, CueProducerProvenance>;

struct CueListValue {
    std::string language = "und";
    // The next id a new cue takes; ids are never reused within a list.
    std::uint64_t nextCueId = 1;
    CueProvenance provenance = CueAuthoredProvenance{};
    // Sorted by name, names unique.
    std::vector<CueStyle> styles;
    // Sorted by (start, end, id), ids unique.
    std::vector<CueHandle> cues;

    // Whether the list holds to every rule of docs/architecture/subtitles.md; problem() names the
    // first rule it breaks.
    [[nodiscard]] bool isValid() const;
    [[nodiscard]] std::optional<std::string> problem() const;
    [[nodiscard]] const CueStyle* findStyle(std::string_view name) const noexcept;
    [[nodiscard]] const Cue* findCue(std::uint64_t id) const noexcept;
    // The position of `id` in the sorted cues, or nullopt.
    [[nodiscard]] std::optional<std::size_t> indexOf(std::uint64_t id) const noexcept;

    // Deep equality: two lists are equal when their cues are, wherever the cues are held.
    friend bool operator==(const CueListValue& left, const CueListValue& right);
};

// The order cues are kept in.
[[nodiscard]] bool cueOrderLess(const Cue& left, const Cue& right) noexcept;
// Sorts the cues into (start, end, id) order and the styles by name. Does not validate.
void canonicalizeCueList(CueListValue& list);

// The WebVTT default color classes, which are built-in styles: `white lime cyan red yellow
// magenta blue black` and their `bg_` forms. Every list may name them without declaring them.
[[nodiscard]] bool isBuiltInCueStyleName(std::string_view name) noexcept;
// A built-in class's color (opaque, sRGB-encoded), or nullopt for any other name.
[[nodiscard]] std::optional<core::Color4d> builtInCueClassColor(std::string_view name) noexcept;

// Whether `tag` has the shape of a BCP 47 language tag: subtags of one to eight ASCII letters and
// digits joined by '-', the first two to eight letters, at most kMaximumCueLanguageBytes. "und" is
// the undetermined language.
[[nodiscard]] bool isWellFormedLanguageTag(std::string_view tag) noexcept;

// The problem with one cue's text, or nullopt: not UTF-8, a control character other than tab and
// line feed, more than kMaximumCueTextBytes or kMaximumCueLines, or markup that does not tokenize
// (cue_markup.hpp, strict).
[[nodiscard]] std::optional<std::string> cueTextProblem(std::string_view text);

// The most cues that show at one time, and the first time that many show.
struct CueOverlap {
    std::size_t count = 0;
    core::RationalTime at;
};
[[nodiscard]] CueOverlap maximumCueOverlap(const CueListValue& list);

// "HH:MM:SS.mmm" for a time, rounded down to the millisecond (never negative).
[[nodiscard]] std::string cueTimestampText(core::RationalTime time);

} // namespace bloom::document
