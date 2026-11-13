#pragma once

// #631 the audio analysis table: one pass over a whole sound (48 kHz stereo, as the sound store
// renders it) reduces it to a row every 10 ms. Every analysis node reads its numbers from the
// rows, so a frame's value is a function of the sound and the time only: the same in preview, on
// seek and in export, and independent of playback.
//
// A row holds, for the 2048 frames centred on its time (a periodic Hann window): the power of
// the left channel, of the right channel and their cross power in 64 bands spaced evenly in the
// logarithm of frequency from 20 Hz to 20 kHz; for the 480 frames that start at its time, the
// mean squares of left, right and their product and the two peaks; the spectral flux against the
// row before (the sum over the bands of the rise in magnitude; 0 for the last rows, whose window
// passes the end of the sound); and the power of each of the 12 pitch classes (the peaks between 60 Hz and 5 kHz
// of an 8192-frame transform of the mono sum, renewed every fourth row). Powers are scaled so a sine of amplitude 1 reads 1 (0 dB).
// A transform bin stands for the frequencies half a bin either side of its centre, and a band
// takes the part of each bin that lies inside it, so a band narrower than a bin (below about
// 200 Hz) takes its share and the bands together hold all the power once.
//
// Everything here is a pure function of the samples and the arguments. No device and no Qt.
// docs/architecture/media-io.md, "Audio analysis", is the specification.

#include <bloom/media/audio/audio.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace bloom::media::audio::analysis {

inline constexpr std::uint32_t kTableRate = 48'000;
inline constexpr std::uint32_t kTableHopFrames = 480;
inline constexpr std::uint32_t kTableFftSize = 2048;
inline constexpr std::uint32_t kTableBands = 64;
inline constexpr double kTableLowHz = 20.0;
inline constexpr double kTableHighHz = 20'000.0;
inline constexpr std::uint32_t kTablePitchFftSize = 8192;
inline constexpr std::uint32_t kTablePitchEveryRows = 4;
inline constexpr std::uint32_t kTablePitchClasses = 12;

// Where a row's numbers are.
inline constexpr std::size_t kTableLeftBands = 0;
inline constexpr std::size_t kTableRightBands = kTableBands;
inline constexpr std::size_t kTableCrossBands = 2 * kTableBands;
inline constexpr std::size_t kTableMeanLeft = 3 * kTableBands;
inline constexpr std::size_t kTableMeanRight = kTableMeanLeft + 1;
inline constexpr std::size_t kTableMeanCross = kTableMeanLeft + 2;
inline constexpr std::size_t kTablePeakLeft = kTableMeanLeft + 3;
inline constexpr std::size_t kTablePeakRight = kTableMeanLeft + 4;
inline constexpr std::size_t kTableFlux = kTableMeanLeft + 5;
inline constexpr std::size_t kTablePitch = kTableMeanLeft + 6;
inline constexpr std::size_t kTableRowFloats = kTablePitch + kTablePitchClasses;

// The rows of a sound of `frames` frames: row r is the time r * kTableHopFrames.
[[nodiscard]] constexpr std::uint64_t tableRows(const std::uint64_t frames) noexcept {
    return frames / kTableHopFrames + 1;
}
[[nodiscard]] constexpr double tableRowSeconds(const std::uint64_t row) noexcept {
    return static_cast<double>(row) * kTableHopFrames / kTableRate;
}
// The lower edge of band `band` in hertz; band kTableBands is the upper edge of the last.
[[nodiscard]] double tableBandEdge(std::uint32_t band) noexcept;

// Reads interleaved stereo frames from `first` (which may be negative) into `output`, silence
// where the sound has none. False fails the build.
using TableReader = std::function<bool(std::int64_t first, std::span<float> output)>;
// Takes row `row` (kTableRowFloats numbers). False fails the build.
using TableSink = std::function<bool(std::uint64_t row, std::span<const float> values)>;

// Builds every row of a sound of `frames` frames, in order. Cancelled when `cancel` says so.
[[nodiscard]] AudioResult<std::uint64_t> buildTable(std::uint64_t frames, const TableReader& reader,
                                                    const TableSink& sink,
                                                    const CancelAudioWork& cancel = {}) noexcept;

// Which signal of the stereo pair a reading takes: both channels' power averaged, one channel,
// the mid signal (left plus right, halved) or the side signal (left minus right, halved).
enum class TableChannel : std::uint8_t { Mix, Left, Right, Mid, Side };

// The power of `channel` in the frequencies from `lowHz` to `highHz` in one row. A band the range
// covers in part counts in proportion to the part covered (in the logarithm of frequency).
[[nodiscard]] double tableBandPower(std::span<const float> row, TableChannel channel, double lowHz,
                                    double highHz) noexcept;
// The power of `channel` over the row's 10 ms (a sine of amplitude 1 reads 1).
[[nodiscard]] double tableLevelPower(std::span<const float> row, TableChannel channel) noexcept;

// Power to decibels; silence reads kTableSilenceDb.
inline constexpr double kTableSilenceDb = -120.0;
[[nodiscard]] double tablePowerDb(double power) noexcept;

// How decibels become 0 to 1.
enum class TableNormalize : std::uint8_t {
    // Against the level 95 in 100 rows of the sound stay under: 1 at that level, 0 at `rangeDb`
    // below it.
    Percentile95,
    // Against the loudest row of the sound.
    Peak,
    // Against the loudest row of the `rollingSeconds` before each row (and the row itself).
    Rolling,
    // 0 at `floorDb`, 1 at `ceilingDb`.
    Manual,
};
struct TableEnvelopeSettings final {
    TableNormalize normalize = TableNormalize::Percentile95;
    double rangeDb = 48.0;
    double rollingSeconds = 4.0;
    double floorDb = -48.0;
    double ceilingDb = 0.0;
    // Seconds for the value to cover about two thirds of a rise (attack) or of a fall (release);
    // 0 follows at once. Computed from the first row, so a row's value depends on the sound alone.
    double attackSeconds = 0.0;
    double releaseSeconds = 0.0;
};
// A reference level under which a sound is taken as silent: a silent sound reads 0, not 1.
inline constexpr double kTableReferenceFloorDb = -72.0;

// One value per row, 0 to 1: `power` (one power per row) in decibels, normalized and smoothed.
[[nodiscard]] std::vector<float> tableEnvelope(std::span<const double> power,
                                               const TableEnvelopeSettings& settings);

// The value of a per-row curve at `seconds`, between rows in a straight line; 0 before the first
// row and after the last.
[[nodiscard]] double tableCurveAt(std::span<const float> curve, double seconds) noexcept;

// ---- #634 features of one row ----

// Where the row's power sits in frequency: 0 at the lowest band, 1 at the highest (the power-
// weighted mean of the bands' positions in the logarithm of frequency). 0 for silence.
[[nodiscard]] double tableBrightness(std::span<const float> row, TableChannel channel) noexcept;
// How much the row is noise rather than tone: the geometric mean of the bands' power per hertz
// over its arithmetic mean, near 1 for the same power at every frequency, near 0 for a single
// tone. 0 for silence.
[[nodiscard]] double tableNoisiness(std::span<const float> row, TableChannel channel) noexcept;
// Where the row sits between the channels: -1 all left, 0 centred, 1 all right. 0 for silence.
[[nodiscard]] double tablePan(std::span<const float> row) noexcept;
// How much of the row is the side signal: 0 for equal channels (mono), 1 for opposite channels.
// 0 for silence.
[[nodiscard]] double tableWidth(std::span<const float> row) noexcept;
// The share of each pitch class (0 is C) in the row's pitched power; all 0 for silence.
[[nodiscard]] std::array<double, kTablePitchClasses>
tablePitchShares(std::span<const float> row) noexcept;

// ---- #634 hits ----

struct TableHitSettings final {
    // How far a row's flux must stand above the mean of the fifth of a second around it.
    double sensitivity = 1.5;
    // Rows closer than this to the hit before them are not hits.
    double minimumGapSeconds = 0.08;
};
// The rows where a sound starts (a drum hit, a note): local peaks of the flux (one per row) that
// stand above their surroundings and above a tenth of the sound's largest flux, in order.
[[nodiscard]] std::vector<std::uint32_t> tableHits(std::span<const float> flux,
                                                   const TableHitSettings& settings);

// ---- #635 beats ----

struct TableBeatSettings final {
    double minimumBpm = 60.0;
    double maximumBpm = 180.0;
};
struct TableBeats final {
    // 0 when the sound has no pulse to find (silence, a held tone, noise): the rises of the flux
    // must correlate with themselves one beat later by at least 0.1.
    double bpm = 0.0;
    // The time of each beat in seconds, ascending.
    std::vector<double> beats;
};
// The tempo whose period the flux repeats at most strongly (its autocorrelation, with a mild
// preference for 120 beats a minute), and the beats: the evenly spaced times that best fall on
// rises of the flux (dynamic programming over the flux, after Ellis, "Beat Tracking by Dynamic
// Programming", 2007). A reference method: it follows a steady pulse and does not follow a tempo
// that changes by more than a little.
[[nodiscard]] TableBeats tableBeats(std::span<const float> flux, const TableBeatSettings& settings);

} // namespace bloom::media::audio::analysis
