#pragma once
// #752: embedded subtitle tracks, worker-private (FFmpeg types never leave the media worker).
// The FFmpeg worker muxes them through FFmpeg's mov_text (3GPP Timed Text) and WebVTT encoders;
// the macOS worker writes them through AVAssetWriter. Both reopen checks read the artifact back
// here, with FFmpeg's MOV and Matroska demuxers, so every platform's file passes the same check.
#include <bloom/media/provider/encode.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

struct AVFormatContext;

namespace bloom::media::ffmpeg {

// The ASS dialogue text FFmpeg's subtitle encoders read for `text`: line feeds become "\N" and
// the two sequences ASS would read as markup ("\n", "\N" and "{\") are broken by an empty
// override block, so the sample holds exactly `text`.
[[nodiscard]] std::string assDialogueText(std::string_view text);

// The minimal ASS script header FFmpeg's subtitle encoders are opened with (one Default style).
[[nodiscard]] std::string assScriptHeader(std::uint32_t width, std::uint32_t height);

// The cue time on the track's clock: exact frames for MOV and MP4 (the track's timescale is the
// frame rate), milliseconds rounded down for WebM (Matroska's clock), so a cue starts on its first
// frame and is off on the frame after its last.
[[nodiscard]] std::int64_t subtitleTrackTime(std::uint64_t frame, provider::Rational rate,
                                             bool milliseconds);

// The subtitle tracks of an opened, probed demuxer, in stream order (see EncodeQcV1::subtitles):
// language, title, and each non-empty sample's text and the frames [first, end) at `rate` a
// player shows it on. Reads every packet; the caller does not use the demuxer afterwards. Throws
// provider::Unavailable for a packet it cannot read.
[[nodiscard]] std::vector<provider::SubtitleTrackV1>
readSubtitleTracks(AVFormatContext& opened, provider::Rational rate);
// The same for a movie file opened by its path.
[[nodiscard]] std::vector<provider::SubtitleTrackV1>
readSubtitleTracks(const std::string& path, provider::Rational rate);

// FFmpeg's MOV muxer marks the first text track enabled, which readers take as the default
// track. Clears the enabled flag in the track header of every text track of the ISO BMFF file
// `file` (`size` bytes), so the tracks stay selectable but none is shown unasked. False when the
// file's boxes cannot be walked.
[[nodiscard]] bool clearSubtitleTracksEnabled(std::FILE* file, std::uint64_t size);

} // namespace bloom::media::ffmpeg
