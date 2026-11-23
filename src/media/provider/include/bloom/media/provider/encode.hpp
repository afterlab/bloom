#pragma once
#include <bloom/media/provider/contract.hpp>

#include <limits>

namespace bloom::media::provider {
// #752: one cue track an export embeds: 3GPP Timed Text (FFmpeg's mov_text) in MOV and MP4,
// WebVTT in WebM. Cues are in output frames: a cue shows frames [first, end). `text` is the
// sample's text: plain UTF-8 lines joined by '\n' for 3GPP Timed Text, the WebVTT cue payload (cue
// text markup) for WebVTT. A MOV or MP4 track's cues never overlap (the host joins cues that show
// together); a WebM track's cues are ordered by `first`. The track is neither default nor forced.
struct SubtitleCueV1 {
    std::uint64_t first = 0, end = 0;
    std::string text;
    friend bool operator==(const SubtitleCueV1&, const SubtitleCueV1&) = default;
};
struct SubtitleTrackV1 {
    // ISO 639-2 three-letter code, lower case ("und" when unknown); the title may be empty.
    std::string language = "und";
    std::string title;
    std::vector<SubtitleCueV1> cues;
    friend bool operator==(const SubtitleTrackV1&, const SubtitleTrackV1&) = default;
};
inline constexpr std::uint32_t kSubtitleTracksMaximum = 64;
// #761: one picture or sound stream of a written movie or WAV, read back from the file by the
// worker's verifying reopen. `codec` is the codec's short name ("h264", "hevc", "prores", "dnxhd",
// "vp9", "av1", "aac", "opus", "pcm_s16le", "pcm_s24le"), `profile` its profile as the codec
// names it ("High", "Main 10", "LC"; empty where the codec has none). A video stream has its
// picture size and frame rate, an audio stream its channel count and sample rate. `bitrate` is
// the stream's average over its duration, in bits per second, from the bytes of its packets.
struct EncodedStreamV1 {
    MediaKind kind = MediaKind::Video;
    std::string codec, profile;
    std::uint32_t width = 0, height = 0;
    Rational rate{0, 1};
    std::uint32_t channels = 0, sampleRate = 0;
    std::uint64_t bitrate = 0;
    friend bool operator==(const EncodedStreamV1&, const EncodedStreamV1&) = default;
};
inline constexpr std::uint32_t kEncodedStreamsMaximum = 2;
inline constexpr std::uint32_t kSubtitleCueTextBytes = 16384;
[[nodiscard]] bool valid(const SubtitleTrackV1& track, std::string_view container,
                         std::uint64_t frames);
// Whether the container carries an embedded subtitle track (mov, mp4, webm).
[[nodiscard]] bool subtitleContainer(std::string_view container) noexcept;
// The first difference between the tracks an encode sent and the tracks its artifact reopens with,
// as a sentence ("Reopened subtitle track 1 differs: cue 3 shows frames 40-52, sent 40-51"); empty
// when they are equal.
[[nodiscard]] std::string subtitleTrackDifference(const std::vector<SubtitleTrackV1>& sent,
                                                  const std::vector<SubtitleTrackV1>& reopened);

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
    // #752: embedded cue tracks (MOV, MP4 and WebM only), written after the video and audio.
    std::vector<SubtitleTrackV1> subtitles;
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
    // The video profile and bit depth the reopened stream actually carries ("Main 10", 10). HEVC
    // is always delivered at the requested depth (#408); a stream at another depth fails the
    // reopen check instead of being reported as an adjustment.
    std::string profile;
    std::uint32_t bitDepth = 0;
    // Issue #455: the encoder that produced the video stream, as the worker knows it: the system
    // encoder's name and whether it is a hardware one on macOS, the codec implementation's name
    // in the FFmpeg worker. Empty when there is no video stream.
    std::string encoder;
    // #752: the subtitle tracks the reopened artifact holds, read back by FFmpeg's demuxer: each
    // cue in the frames a player shows it on (the first frame whose time is at or after the cue's
    // start, through the last before its end), its text as the sample holds it, the track's
    // language and title. Empty samples (the gaps between cues) are left out.
    std::vector<SubtitleTrackV1> subtitles;
    // #761: the picture and sound streams the reopened artifact holds, in stream order (the video
    // first), as the verifying reopen read them from the file. Empty for a still image.
    std::vector<EncodedStreamV1> streams;
    // #771: the sound as the verifying reopen decoded it from the written file, measured with the
    // reference library (libebur128, ITU-R BS.1770 / EBU R128): integrated loudness in LUFS and the
    // highest true peak of any channel in dBTP. `audioMeasured` is false when nothing was measured
    // (no audio stream). `audioLoudnessMeasured` is false for sound with nothing above the
    // absolute gate (silence), whose loudness is negative infinity; digital silence's true peak is
    // negative infinity too. Never NaN or positive infinity.
    bool audioMeasured = false;
    bool audioLoudnessMeasured = false;
    double audioLufs = -std::numeric_limits<double>::infinity();
    double audioTruePeakDbtp = -std::numeric_limits<double>::infinity();
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
// Lossy audio (AAC, and Opus through the same measure). v1 bounded every decoded sample to 0.25
// of the sample the encoder received and the whole stream's RMS error to 0.05. That failed
// correct encodes of loud transients, whose coding error is spread over a transform block
// (#650), and it bounded nothing in program quieter than those limits. v2 requires the
// container to declare the encoder's priming, then compares, per channel: the waveform below
// 3 kHz (signal-to-error ratio of at least 6 dB through one 8th-order Butterworth low-pass on both
// signals), and the level of every 2048-frame window against the source's same and neighbouring
// windows (within 3 dB, band and full range). Energies under -60 dBFS count as silence. The
// measure and its derivation are in apps/bloom-media-worker/lossy_audio_compare.hpp and
// docs/architecture/media-io.md.
inline constexpr std::string_view kAacToleranceV2 =
    "bloom.aac-f32.v2;sample-count=exact;priming=declared;band=butterworth8-lowpass-3000hz;"
    "band-snr-db-min=6;window=2048;window-level-db-max=3;floor-dbfs=-60";
[[nodiscard]] bool valid(const EncodeSettingsV1& value);
[[nodiscard]] MediaDeterminismV1 encodeDeterminism(const EncodeSettingsV1& value);
[[nodiscard]] Digest encodeTolerance(const EncodeSettingsV1& value);
// The versioned names of the tolerance profiles encodeTolerance binds
// ("bloom.video-rgba16-srgb.v3", "bloom.aac-f32.v2", or both joined by '+' in video-then-audio
// order); empty when it binds none.
[[nodiscard]] std::string encodeToleranceName(const EncodeSettingsV1& value);
[[nodiscard]] Result<Digest> digest(const EncodeSettingsV1& value);
} // namespace bloom::media::provider
