#pragma once

// #347 audio analysis: the numbers Audio Amplitude, Audio Waveform and Audio Spectrum draw from a
// window of an audio buffer. Every function reads only the frames of its window, through the
// buffer's bounded playback window when it has one, checks `cancel` between chunks, and is a pure
// function of the samples and the arguments, so the same window of the same source always gives
// the same numbers. Frames before the start or past the end of the source are silence.
//
// docs/architecture/media-io.md, "Audio analysis", is the specification. No device and no Qt.

#include <bloom/media/audio/audio.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bloom::media::audio::analysis {

// The longest window one analysis reads (about a minute and a half at 48 kHz).
inline constexpr std::uint64_t kMaximumWindowFrames = std::uint64_t{1} << 22;
inline constexpr std::uint32_t kMaximumColumns = 4096;
inline constexpr std::uint32_t kMaximumLanes = 8;
inline constexpr std::uint32_t kMinimumFftSize = 256;
inline constexpr std::uint32_t kMaximumFftSize = 16384;

enum class Measure : std::uint8_t {
    // The largest absolute sample of any channel.
    Peak,
    // The square root of the mean square of every channel's samples.
    Rms,
};

// `frames` frames from `first` (which may be negative), as the linear level of `measure`, 0 to 1
// for a full-scale signal.
[[nodiscard]] AudioResult<double> amplitude(const AudioBuffer& buffer, std::int64_t first,
                                            std::uint64_t frames, Measure measure,
                                            const CancelAudioWork& cancel = {}) noexcept;

// The minimum and maximum of each lane in each column: `lanes` is 1 (the channels averaged) or the
// source's channel count (each channel), at most kMaximumLanes. `values` is column-major, a
// minimum then a maximum for each lane: [column][lane][min, max]. Column c covers the frames from
// floor(c * frames / columns) to floor((c + 1) * frames / columns), and at least one frame.
struct WaveformColumns final {
    std::uint32_t columns = 0;
    std::uint32_t lanes = 0;
    std::vector<float> values;
};
[[nodiscard]] AudioResult<WaveformColumns>
waveform(const AudioBuffer& buffer, std::int64_t first, std::uint64_t frames,
         std::uint32_t columns, bool separateChannels, const CancelAudioWork& cancel = {}) noexcept;

// The magnitude spectrum of the `fftSize` frames centred on `center` (the channels averaged, a
// periodic Hann window, magnitudes scaled so a sine of amplitude A reads A at its own frequency),
// mapped onto `columns` columns from `lowHz` to `highHz`, linear or logarithmic in frequency. A
// column takes the largest magnitude of the bins whose centre it covers, or the nearest bin when it
// covers none. Each value is the magnitude in decibels mapped from [floorDb, ceilingDb] onto [0, 1]
// and limited to it.
struct SpectrumSettings final {
    std::uint32_t fftSize = 2048;
    double lowHz = 20.0;
    double highHz = 20000.0;
    bool logarithmic = true;
    double floorDb = -90.0;
    double ceilingDb = 0.0;
};
[[nodiscard]] AudioResult<std::vector<float>>
spectrum(const AudioBuffer& buffer, std::int64_t center, const SpectrumSettings& settings,
         std::uint32_t columns, const CancelAudioWork& cancel = {}) noexcept;

// Reads `count` frames from `first` as interleaved samples, silence where the source has none.
[[nodiscard]] AudioResult<std::vector<float>> readFrames(const AudioBuffer& buffer,
                                                         std::int64_t first, std::uint64_t count,
                                                         const CancelAudioWork& cancel = {}) noexcept;

} // namespace bloom::media::audio::analysis
