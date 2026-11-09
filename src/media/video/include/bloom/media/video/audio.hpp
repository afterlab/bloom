#pragma once
#include <bloom/media/audio/audio.hpp>
#include <bloom/media/video/session.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
namespace bloom::media::video {
// Adapter to the existing bounded clip mixer; no device or codec ownership crosses here.
[[nodiscard]] std::optional<std::uint64_t>
audioClipFrameCount(const provider::StreamDescriptor& stream) noexcept;
// The `ordinal`-th audio stream (0-based among audio streams), or nullptr.
[[nodiscard]] const provider::StreamDescriptor* audioStreamAt(const provider::ProbeResult& probe,
                                                              std::uint32_t ordinal) noexcept;
// The frames one refill of a video container's audio window decodes. A refill is one worker
// request, so it is larger than the 4096-frame window of a file decoded in process.
inline constexpr std::size_t kVideoAudioWindowFrames = 16384;
// The probe a stream-backed buffer for `stream` carries; empty when the stream has no usable
// frame count or channel layout.
[[nodiscard]] std::optional<audio::AudioProbe>
videoAudioProbe(const provider::StreamDescriptor& stream) noexcept;
// The `audioStream`-th audio stream of a video container as a bounded playback window (#329): the
// buffer carries a stream that decodes forward through `session`'s retained worker and repositions
// it for a backward or distant request, so only the window is resident however long the audio is.
// Calls on the stream do worker I/O and belong on a worker thread. `session` and `probe` stay alive
// with the stream; the source's digest in `probe` is re-verified by every request.
[[nodiscard]] provider::Result<audio::AudioBuffer>
openWindowedVideoAudio(std::shared_ptr<VideoDecodeSession> session, provider::ProbeResult probe,
                       std::uint32_t audioStream = 0,
                       std::size_t windowFrames = kVideoAudioWindowFrames);
// The whole audio decoded into memory; kept for callers that need every sample at once.
[[nodiscard]] provider::Result<audio::AudioBuffer>
decodeAudioClip(VideoDecodeSession& session, const provider::ProbeResult& probe,
                std::uint64_t admittedSamples, const Cancel& cancel = {},
                std::uint32_t audioStream = 0);
} // namespace bloom::media::video
