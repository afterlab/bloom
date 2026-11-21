#pragma once

// #752: the `subtitles` read every client shares (MCP query kind=subtitles, Python
// bloom.data.subtitles): a Subtitles node's cues, paged, with their times in frames and seconds,
// text and plain text, and the list's summary (count, language, provenance, linked-file state and
// styles), or the list as SRT or WebVTT text. Without a node it lists the composition's Subtitles
// nodes. It reads the snapshot it is given; a linked file's state is read from disk (its size and
// stamp, and its digest only when those changed), on the calling client thread.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/cue_list.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/document/document.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::scripting {

inline constexpr std::size_t kSubtitlesQueryDefaultPageSize = 64;
inline constexpr std::size_t kSubtitlesQueryMaximumPageSize = 64;

enum class SubtitlesQueryFormat : std::uint8_t { Json, Srt, Vtt };

struct SubtitlesQuery final {
    document::CompositionId composition;
    std::optional<document::NodeId> node;
    // Only the cues showing at `at` (start <= at < end); or those overlapping [from, to).
    std::optional<core::RationalTime> at;
    std::optional<core::RationalTime> from;
    std::optional<core::RationalTime> to;
    std::size_t pageSize = kSubtitlesQueryDefaultPageSize;
    std::string cursor;
    SubtitlesQueryFormat format = SubtitlesQueryFormat::Json;
};

enum class SubtitlesQueryStatus : std::uint8_t {
    Read,
    MissingComposition,
    MissingNode,
    NotSubtitles,
    // The node's cues come from another node (an edge or a driver into its Cues input).
    Connected,
    InvalidCursor,
    InvalidRange,
};

struct SubtitleCueRow final {
    std::uint64_t id = 0;
    std::size_t index = 0;
    core::RationalTime start;
    core::RationalTime end;
    // end - start; nullopt only when it cannot be held exactly in 64-bit terms.
    std::optional<core::RationalTime> duration;
    // "HH:MM:SS.mmm --> HH:MM:SS.mmm", the form an SRT or WebVTT file shows.
    std::string timestamp;
    std::string text;
    std::string plainText;
    std::string speaker;
    std::string style;
    document::CuePlacement placement;
    std::vector<std::string> warnings;
};

struct SubtitleNodeSummary final {
    document::NodeId node;
    // Empty when the cues come from another node.
    std::optional<std::size_t> count;
    std::string language;
    std::string provenance; // authored, file, producer or connected
};

struct SubtitlesReading final {
    SubtitlesQueryStatus status = SubtitlesQueryStatus::Read;
    std::string diagnostic;
    document::FrameRate frameRate = document::FrameRate::framesPerSecond24();
    // Without a node: the composition's Subtitles nodes.
    std::vector<SubtitleNodeSummary> nodes;
    // With a node: the list's summary and the page of cues.
    std::size_t count = 0;
    std::size_t matching = 0;
    std::string language;
    document::CueProvenance provenance;
    // none, in-sync, changed, missing or unreadable.
    std::string linkedFile = "none";
    std::vector<document::CueStyle> styles;
    std::vector<SubtitleCueRow> cues;
    std::string nextCursor;
    // With format srt or vtt: the whole list as that file's text, and what it could not carry.
    std::optional<std::string> text;
    std::vector<std::string> textWarnings;
    std::uint64_t revision = 0;
};

// "HH:MM:SS.mmm" or "HH:MM:SS,mmm" (hours any number of digits; a leading '-' negates it), exact
// in milliseconds: the timestamp form every subtitle time argument also takes.
[[nodiscard]] std::optional<core::RationalTime> parseCueTimestamp(std::string_view text);

[[nodiscard]] SubtitlesReading readSubtitles(const document::Snapshot& snapshot,
                                             const std::filesystem::path& projectDirectory,
                                             const SubtitlesQuery& query,
                                             std::stop_token stop = {});

} // namespace bloom::scripting
