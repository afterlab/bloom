#pragma once
// The color tags an H.264 stream itself carries, against the tags of the container around it
// (issue #667). Worker-private: FFmpeg types never leave the media worker.
#include <string>
extern "C" {
#include <libavutil/pixfmt.h>
}
struct AVFormatContext;
namespace bloom::media::ffmpeg {
// A container's tags reach a reader of the file; a reader of the stream alone (a player that
// ignores the container's color box, a stream cut out of its file, a QuickTime file, which has
// no place for the range) sees only what the stream's own parameter set says. An export is right
// only when both say the same thing.
//
// `opened` is a demuxer that has only been opened: nothing has probed its streams, so the tags of
// stream `stream` are still the container's own. The stream's first picture is then decoded by a
// decoder that is given the stream's parameter sets and none of the container's tags, so what it
// reports is what the stream says. The two have to agree in primaries, transfer and matrix, and
// the stream's range has to be `requestedRange` (and the container's, where the container states
// one). A stream that states no range is limited range, as H.264 defines.
//
// Returns an empty string when they agree, and otherwise the reason, naming both sets of tags.
// Reads packets from `opened`; the caller does not use it afterwards.
[[nodiscard]] std::string h264StreamTagFailure(AVFormatContext& opened, unsigned stream,
                                               AVColorRange requestedRange);
// The same for a movie file opened by its path.
[[nodiscard]] std::string h264StreamTagFailure(const std::string& path, unsigned stream,
                                               AVColorRange requestedRange);
} // namespace bloom::media::ffmpeg
