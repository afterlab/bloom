#pragma once

// #570 export loudness: programme loudness and true peak measured per EBU R128 / ITU-R BS.1770 by
// the reference library (libebur128), and the gain and true-peak limiter that bring a mix to a
// target. The measurement is the library's; the limiter only decides how much to turn the audio
// down so the interpolated peak stays under the ceiling.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace bloom::media::audio::loudness {

// What a measured programme came to. Silence (nothing above the BS.1770 absolute gate) has no
// loudness: `loudnessMeasured` is false and `integratedLufs` is negative infinity.
struct Measurement final {
    bool loudnessMeasured = false;
    double integratedLufs = -std::numeric_limits<double>::infinity();
    // The highest interpolated sample peak of any channel, in dB relative to full scale (dBTP);
    // negative infinity for digital silence.
    double truePeakDbtp = -std::numeric_limits<double>::infinity();
    std::uint64_t frames = 0;
};

// A streaming BS.1770 meter. Create it with the stream's sample rate and channel count, feed it
// interleaved float frames in order, and read the programme measurement at the end (or at any
// point). Not thread-safe.
class Meter final {
  public:
    Meter(std::uint32_t sampleRate, std::uint32_t channels);
    ~Meter();
    Meter(const Meter&) = delete;
    Meter& operator=(const Meter&) = delete;

    // False when the rate or channel count is outside what the library measures.
    [[nodiscard]] bool valid() const noexcept;
    // `samples` is whole interleaved frames. False on a refusal (an invalid meter, a partial frame,
    // or the library's allocation failing).
    [[nodiscard]] bool add(std::span<const float> samples) noexcept;
    [[nodiscard]] Measurement measurement() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
    std::uint64_t frames_ = 0;
    std::uint32_t channels_ = 0;
};

// The loudness target of an export: integrated loudness in LUFS and the true-peak ceiling in dBTP.
struct Target final {
    double integratedLufs = -16.0;
    double truePeakDbtp = -1.0;
};

inline constexpr double kMinimumTargetLufs = -40.0;
inline constexpr double kMaximumTargetLufs = -5.0;
inline constexpr double kMinimumTruePeakDbtp = -12.0;
inline constexpr double kMaximumTruePeakDbtp = 0.0;

// The gain in dB that brings `measured` to `target.integratedLufs`; zero when the programme has no
// measurable loudness.
[[nodiscard]] double gainToTarget(const Measurement& measured, const Target& target) noexcept;

// Applies a constant gain and keeps the interpolated peak under a ceiling with a look-ahead
// limiter, one stream at a time. The limiter looks ahead, so audio comes out `latencyFrames()`
// frames after it went in: feed it the whole programme, then call flush() for the rest. Output
// frame n is input frame n, so the stream keeps its length and its alignment.
class Limiter final {
  public:
    Limiter(std::uint32_t sampleRate, std::uint32_t channels, double gainDb, double ceilingDbtp);
    ~Limiter();
    Limiter(const Limiter&) = delete;
    Limiter& operator=(const Limiter&) = delete;

    [[nodiscard]] std::size_t latencyFrames() const noexcept;
    // The largest gain reduction applied so far by the limiter itself (not the programme gain), in
    // dB; zero when it has not acted.
    [[nodiscard]] double maximumReductionDb() const noexcept;
    // Appends the output frames that became ready to `out`. `samples` is whole interleaved frames.
    void process(std::span<const float> samples, std::vector<float>& out);
    // Appends the frames still held back. Call once, after the last process().
    void flush(std::vector<float>& out);

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace bloom::media::audio::loudness
