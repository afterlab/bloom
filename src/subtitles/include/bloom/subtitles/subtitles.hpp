#pragma once

// #752: Bloom's subtitle file readers and writers (docs/architecture/subtitles.md, "Formats").
// Qt-free; every reader follows its standard (the W3C WebVTT parser algorithm, TTML/IMSC 1.1 Text
// through the locked Expat, the de facto SubRip grammar, ASS/SSA's script format) and reports
// every feature it dropped or approximated as a typed row with the line and column it came from.
// A load is pure: bytes in, a document::CueListValue out. The callers own file access and where
// the result goes (commands, the linked-file monitor, scripting).

#include <bloom/core/rational_time.hpp>
#include <bloom/document/cue_list.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bloom::subtitles {

enum class Format : std::uint8_t { Srt, WebVtt, Ttml, Ass };

// "srt", "vtt", "ttml" or "ass".
[[nodiscard]] std::string_view formatName(Format format) noexcept;
// The format a name means: srt, vtt (or webvtt), ttml (or dfxp, imsc) and ass (or ssa).
[[nodiscard]] std::optional<Format> formatFromName(std::string_view name) noexcept;
// The format a file extension names (".srt", ".vtt", ".ttml", ".dfxp", ".xml" as TTML, ".ass",
// ".ssa"); nullopt for any other extension.
[[nodiscard]] std::optional<Format> formatFromExtension(std::string_view extension) noexcept;
// Whether Bloom writes the format (SRT, WebVTT and IMSC 1.1 Text; not ASS).
[[nodiscard]] constexpr bool isWritableFormat(const Format format) noexcept {
    return format != Format::Ass;
}

// One row of a load's report: a cue that was skipped, or a feature that was dropped or drawn
// differently. `line` and `column` are 1-based in the decoded text (0 when the row is about the
// whole file).
struct Report {
    std::size_t line = 0;
    std::size_t column = 0;
    std::string code;
    std::string message;

    friend bool operator==(const Report&, const Report&) = default;
};

// The stable report codes (docs/architecture/subtitles.md, "Reports").
namespace codes {
inline constexpr std::string_view kSkippedCue = "skipped-cue";
inline constexpr std::string_view kZeroLength = "zero-length-cue";
inline constexpr std::string_view kDropped = "dropped";
inline constexpr std::string_view kApproximated = "approximated";
inline constexpr std::string_view kCounter = "counter";
inline constexpr std::string_view kMarkup = "markup";
// Failures (LoadFailure::code).
inline constexpr std::string_view kEncoding = "encoding";
inline constexpr std::string_view kUnrecognized = "unrecognized";
inline constexpr std::string_view kRefusedFormat = "refused-format";
inline constexpr std::string_view kNoCues = "no-cues";
inline constexpr std::string_view kStrict = "strict";
inline constexpr std::string_view kLimit = "limit";
inline constexpr std::string_view kMalformed = "malformed";
inline constexpr std::string_view kCancelled = "cancelled";
inline constexpr std::string_view kFile = "file";
} // namespace codes

struct LoadOptions {
    // nullopt: sniff the format from the text (and the file name, when there is one).
    std::optional<Format> format;
    // Empty: automatic (a UTF-8 or UTF-16 byte order mark, else UTF-8). Otherwise "utf-8",
    // "utf-16le", "utf-16be" or a label of a WHATWG single-byte encoding.
    std::string encoding;
    // Refuse the whole load on any report row.
    bool strict = false;
    // The file's name or path, for sniffing by extension and for messages; may be empty.
    std::string sourceName;
    std::stop_token stop;
};

struct LoadResult {
    document::CueListValue cues; // provenance authored; the caller names the file
    Format format = Format::Srt;
    std::string encoding;
    std::vector<Report> reports;
};

struct LoadFailure {
    std::size_t line = 0;
    std::size_t column = 0;
    std::string code;
    std::string message;
    // What the reader reported before it refused (for a strict refusal: every row).
    std::vector<Report> reports;
};

using LoadOutcome = std::variant<LoadResult, LoadFailure>;

// Reads cues from bytes. At most document::kMaximumSubtitleFileBytes.
[[nodiscard]] LoadOutcome load(std::span<const std::byte> bytes, const LoadOptions& options);
[[nodiscard]] LoadOutcome load(std::string_view text, const LoadOptions& options);

// A file read for loading: its bytes (bounded), SHA-256 and stamp.
struct FileContents {
    std::vector<std::byte> bytes;
    std::string digest; // "sha256:<hex>"
    std::uint64_t size = 0;
    std::int64_t modifiedNanoseconds = 0;
};
using FileReadOutcome = std::variant<FileContents, LoadFailure>;
// Reads `path` whole, refusing a file larger than kMaximumSubtitleFileBytes before reading it.
[[nodiscard]] FileReadOutcome readFile(const std::filesystem::path& path, std::stop_token stop = {});
// The size and modification time of `path`, without reading it; nullopt when it is missing.
struct FileStamp {
    std::uint64_t size = 0;
    std::int64_t modifiedNanoseconds = 0;
    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};
[[nodiscard]] std::optional<FileStamp> statFile(const std::filesystem::path& path);
// "sha256:<hex>" of bytes.
[[nodiscard]] std::string digestOf(std::span<const std::byte> bytes);

struct WriteOptions {
    // TTML: a rate the times are counted in when every time is a whole frame of it
    // (ttp:frameRate and ttp:frameRateMultiplier); otherwise ticks.
    std::optional<std::pair<std::uint32_t, std::uint32_t>> frameRate;
};

struct WriteResult {
    std::string text;
    // What the format cannot carry, cue by cue (line 0, column 0; the message names the cue).
    std::vector<Report> reports;
};

// Writes a list as SRT, WebVTT or IMSC 1.1 Text (UTF-8, '\n' line ends). SRT and WebVTT times are
// truncated to the millisecond, so a cue keeps its first frame and stays off the frame after its
// last. ASS is refused (std::nullopt).
[[nodiscard]] std::optional<WriteResult> write(const document::CueListValue& cues, Format format,
                                               const WriteOptions& options = {});

// ---------------------------------------------------------------------------------------------
// Encodings (WHATWG Encoding Standard).

struct DecodedText {
    std::string text; // UTF-8, no byte order mark
    std::string encoding;
};
struct DecodeFailure {
    std::size_t line = 0;
    std::size_t byteOffset = 0;
    std::string message;
};
[[nodiscard]] std::variant<DecodedText, DecodeFailure> decodeText(std::span<const std::byte> bytes,
                                                                  std::string_view encoding);
// The canonical name of a WHATWG single-byte encoding a label names (e.g. "latin1" is
// "windows-1252"), or nullopt.
[[nodiscard]] std::optional<std::string_view> singleByteEncodingName(std::string_view label);
// Every WHATWG single-byte encoding's canonical name.
[[nodiscard]] std::vector<std::string_view> singleByteEncodingNames();

} // namespace bloom::subtitles
