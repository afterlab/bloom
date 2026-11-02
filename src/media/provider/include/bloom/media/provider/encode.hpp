#pragma once
#include <bloom/media/provider/contract.hpp>

namespace bloom::media::provider {
// Closed CPU export intake. RGB is straight, display-referred RGBA16LE sRGB/Rec.709.
// Audio blocks are planar finite float at the declared source rate; timestamps start at zero.
struct EncodeSettingsV1 {
    std::string container = "mov", videoCodec = "prores_ks", profile = "hq";
    std::string audioCodec;
    std::uint32_t width = 1, height = 1;
    Rational rate{24, 1};
    std::uint64_t frames = 1, audioSamples = 0;
    std::uint32_t sampleRate = 48000, channels = 2;
    std::string bwfDescription;
    // Issue #369: the destination's free space minus a safety margin, bounded by the container.
    std::uint64_t byteLimit = Limits::exportBytes;
    // Container colour tags written for the output colour intent (H.273 code points; the range
    // uses FFmpeg's convention, 1 limited and 2 full). The matrix also selects the RGB -> YUV
    // coefficients. The default is the legacy BT.709 limited-range tagging.
    ColourTags colour{1, 1, 1, 1};
    // Target video bit rate for H.264/HEVC; zero means the codec default.
    std::uint32_t videoBitrateKbps = 0;
    // Encoder threads; zero means automatic. Threading never changes decoded semantics beyond the
    // tolerance profile; lossy codecs already claim DecodedSemanticTolerance.
    std::uint32_t threads = 1;
    // VA-API render node for hardware encode ("/dev/dri/renderD128" when empty).
    std::string vaapiDevice;
    friend bool operator==(const EncodeSettingsV1&, const EncodeSettingsV1&) = default;
};
// Issue #406: prores_ks is frame-threaded. Every encoder thread keeps its own 10-bit 4:4:4:4 (or
// 4:2:2) input frame, slice coefficients and worst-case packet, so the worker's address space
// grows with the thread count as well as the frame extent: 1080p 4444 at 64 automatic threads
// passed the 2 GiB media-v1 floor and failed to allocate. Each thread is budgeted 32 bytes per
// pixel on top of the stream's media-v1 worker address space (workerAddressSpaceBytes); 4444 was
// measured at 22 to 25 bytes per pixel per thread in 1080p and 20 in UHD.
inline constexpr std::uint64_t kProresEncodeThreadBytesPerPixel = 32;
// FFmpeg's frame-threaded encoder maximum, and the largest EncodeSettingsV1::threads.
inline constexpr std::uint32_t kEncodeMaximumThreads = 64;
// The address space a ProRes encode worker gets for `threads` encoder threads: the stream's
// media-v1 worker address space plus kProresEncodeThreadBytesPerPixel per pixel per thread,
// rounded up to 256 MiB and never above kWorkerAddressSpaceCeiling.
[[nodiscard]] std::uint64_t proresEncodeAddressSpaceBytes(std::uint32_t width, std::uint32_t height,
                                                          std::uint32_t threads) noexcept;
// The ProRes encoder thread count that is actually run: the request, or for zero (automatic)
// the hardware thread count, never above kEncodeMaximumThreads and reduced until its
// proresEncodeAddressSpaceBytes fits under the ceiling, never below one.
[[nodiscard]] std::uint32_t proresEncodeThreads(std::uint32_t requested,
                                                std::uint32_t hardwareThreads, std::uint32_t width,
                                                std::uint32_t height) noexcept;
// Issue #369: each container's own audio channel limit, replacing the fixed 8-channel cap.
[[nodiscard]] std::uint32_t containerChannelLimit(std::string_view container,
                                                  std::string_view audioCodec) noexcept;
struct EncodedChunkV1 {
    std::uint64_t offset = 0;
    Bytes bytes;
};
struct EncodeQcV1 {
    std::uint64_t bytes = 0, frames = 0, audioSamples = 0;
    Rational duration;
    Digest artifact, firstFrame, lastFrame, audio;
    // Fixed point: maximum and mean absolute sample error in unsigned 16-bit units, each the worst
    // over the compared frames. maximumError is the raw maximum and is reported only; tolerance
    // profile v3 gates percentileMaximumError, the largest error left in a frame once its top
    // 0.01% of color samples are set aside (issue #424), never above maximumError.
    std::uint32_t maximumError = 0, meanError = 0, percentileMaximumError = 0;
    // Issue #369: read back from the reopened artifact.
    ColourTags colour;
    std::string container;
    // The video profile and bit depth the reopened stream actually carries ("main", 8), and,
    // when the serving encoder delivered something other than the request (the bundled 8-bit
    // Kvazaar serving a Main 10 request), the reason. Never silent: the host logs it and the
    // export evidence states it.
    std::string profile;
    std::uint32_t bitDepth = 0;
    std::string adjustment;
};
inline constexpr std::uint32_t kEncodeChunkBytes = 1024U * 1024U;
// v2 compared against the reference taken through the stream's declared sample format, matrix,
// range and chroma siting (v1 used the unsampled reference, failing 4:2:2/4:2:0 decimation of
// small saturated detail as if it were codec error). v3 (issue #424) keeps v2's reference and
// bounds but gates the per-frame maximum after setting aside the frame's largest 0.01% of colour
// samples (an exact 99.99th percentile), so two lossy-codec samples at a saturated chroma edge no
// longer fail a 45 dB frame. The raw maximum is still measured and reported. Alpha keeps its
// absolute bound.
inline constexpr std::string_view kVideoToleranceV3 =
    "bloom.video-rgba16-srgb.v3;first-last;reference=declared-sampling;"
    "percentile-max=p99.99-per-frame:22938;absolute-max=reported;mean-ceil=1967;alpha-max=64;"
    "timing=exact;stream-layout=exact;pcm=exact";
// The bounds kVideoToleranceV3 names, in unsigned 16-bit units.
inline constexpr std::uint32_t kVideoTolerancePercentileMaximum = 22938;
inline constexpr std::uint32_t kVideoToleranceMeanCeiling = 1967;
inline constexpr std::uint32_t kVideoToleranceAlphaMaximum = 64;
// The 99.99th percentile sets aside floor(samples / 10000) color samples of each frame: never
// more than 0.01%, so a frame under 10000 samples sets aside none.
inline constexpr std::uint64_t kVideoToleranceSetAsideDivisor = 10000;
inline constexpr std::string_view kAacToleranceV1 =
    "bloom.aac-f32.v1;sample-count=exact;priming=trim;absolute-max=0.25;rms=0.05";
[[nodiscard]] bool valid(const EncodeSettingsV1& value);
[[nodiscard]] MediaDeterminismV1 encodeDeterminism(const EncodeSettingsV1& value);
[[nodiscard]] Digest encodeTolerance(const EncodeSettingsV1& value);
// The versioned names of the tolerance profiles encodeTolerance binds
// ("bloom.video-rgba16-srgb.v3", "bloom.aac-f32.v1", or both joined by '+' in video-then-audio
// order); empty when it binds none.
[[nodiscard]] std::string encodeToleranceName(const EncodeSettingsV1& value);
[[nodiscard]] Result<Digest> digest(const EncodeSettingsV1& value);
} // namespace bloom::media::provider
