#pragma once
// #761: the picture and sound streams of a written movie or WAV, worker-private (FFmpeg types never
// leave the media worker). The FFmpeg worker's verifying reopen reads them from the demuxer it
// opened; the macOS worker reads its AVAssetWriter file back here too, so every platform's encoder
// reports the same facts the same way.
#include <bloom/media/provider/encode.hpp>

#include <string>
#include <vector>

struct AVFormatContext;

namespace bloom::media::ffmpeg {

// The video and audio streams of an opened, probed demuxer, in stream order (see
// EncodeQcV1::streams). Reads every packet to count each stream's bytes; a stream's bit rate is
// those bytes over its own duration (`videoDuration` for the picture, `audioDuration` for the
// sound, in seconds), rounded to the nearest bit per second, and zero when that duration is zero.
// Subtitle and data streams are left out. The caller does not use the demuxer afterwards. Throws
// provider::Unavailable for a packet it cannot read.
[[nodiscard]] std::vector<provider::EncodedStreamV1>
readWrittenStreams(AVFormatContext& opened, provider::Rational videoDuration,
                   provider::Rational audioDuration);
// The same for a movie or WAV file opened by its path.
[[nodiscard]] std::vector<provider::EncodedStreamV1>
readWrittenStreams(const std::string& path, provider::Rational videoDuration,
                   provider::Rational audioDuration);

} // namespace bloom::media::ffmpeg
