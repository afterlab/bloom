#pragma once
// The reopen comparison for lossy audio (tolerance profile bloom.aac-f32.v2). Worker-private and
// free of FFmpeg types, so it is unit-tested on its own.
//
// A perceptual codec does not return the samples it was given: quantization noise is shaped under
// the signal, noise-like bands above 4 kHz may be replaced by synthetic noise of the same energy,
// the band above the encoder's cutoff is dropped, and the error of a transient is spread over the
// transform block before it (pre-echo). A per-sample limit therefore fails correct encodes of
// loud transients and passes anything at all in quiet program. What a correct encode does keep:
//   - the waveform below 3 kHz, which every supported encoder codes sample-accurately at the
//     preset bitrate (it lies under the noise-substitution limit and under the lowest cutoff);
//   - the energy of each short stretch of sound, within the loss of the dropped top band.
// Both are measured here, per channel, against the samples the encoder received:
//   1. Band SNR. Reference and decoded pass through the same 8th-order 3 kHz Butterworth low-pass.
//      The energy of their difference must stay kBandSnrDb below the reference's. A stream that
//      is shifted by a few samples, comes from another source, has swapped channels, or is silent
//      fails this whatever its level.
//   2. Window level. In windows of kWindowFrames frames, the decoded energy may not exceed the
//      loudest of the reference's same and neighbouring windows by more than kLevelDb (band and
//      full range), and the reference's band energy must reappear in the decoded same or
//      neighbouring windows within kLevelDb. The neighbours absorb pre-echo and a window edge
//      through a transient. Added noise, dropouts and gain changes fail this.
// Energies under kFloorDbfs are treated as silence on both sides.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace bloom::media::ffmpeg {
inline constexpr double kLossyAudioBandHz = 3000.0;
inline constexpr std::uint32_t kLossyAudioWindowFrames = 2048;
inline constexpr double kLossyAudioLevelDb = 3.0;
inline constexpr double kLossyAudioBandSnrDb = 6.0;
inline constexpr double kLossyAudioFloorDbfs = -60.0;
struct LossyAudioMeasure {
    // Non-empty when the stream is outside the profile; names the measure and the channel.
    std::string failure;
    // The lowest per-channel band SNR among channels that carry band energy (dB), and whether any
    // channel did.
    double bandSnrDb = 0.0;
    bool bandSnrMeasured = false;
    // The largest window level excess over its bound (dB; zero or less when every window passed).
    double worstLevelExcessDb = 0.0;
    std::uint64_t frames = 0, windows = 0;
};
class LossyAudioCompare final {
  public:
    LossyAudioCompare(unsigned channels, unsigned sampleRate);
    // Equal counts of interleaved frames of the reference the encoder received and of the decoded
    // stream, in order from the first frame.
    void push(std::span<const float> reference, std::span<const float> decoded);
    [[nodiscard]] LossyAudioMeasure finish();

  private:
    struct Biquad {
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };
    struct FilterState {
        std::array<double, 4> z1{}, z2{};
    };
    struct Window {
        double referenceBand = 0, decodedBand = 0, referenceFull = 0, decodedFull = 0;
        bool valid = false;
    };
    struct Channel {
        FilterState reference, decoded;
        Window previous, current, accumulating;
        double referenceBandEnergy = 0, errorBandEnergy = 0;
    };
    [[nodiscard]] double filter(FilterState& state, double sample) const noexcept;
    void closeWindow();
    void judge(unsigned channel, const Window* before, const Window& window, const Window* after);
    unsigned channelCount_;
    std::array<Biquad, 4> sections_{};
    std::vector<Channel> channels_;
    std::uint32_t windowFill_ = 0;
    LossyAudioMeasure measure_;
    bool nonFinite_ = false;
};
} // namespace bloom::media::ffmpeg
