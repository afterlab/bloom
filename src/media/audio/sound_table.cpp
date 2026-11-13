#include <bloom/media/audio/sound_table.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>
#include <new>

namespace bloom::media::audio::analysis {
namespace {

constexpr double kPi = 3.14159265358979323846;

// One of a row's (or a curve's) numbers, as the double the arithmetic here is done in.
[[nodiscard]] double number(const std::span<const float> values, const std::size_t index) noexcept {
    return static_cast<double>(values[index]);
}

// An in-place radix-2 transform of one power-of-two length, with its tables made once.
class Transform final {
  public:
    explicit Transform(const std::size_t size) : size_(size), reversed_(size), twiddle_(size / 2) {
        for (std::size_t i = 1, j = 0; i < size; ++i) {
            std::size_t bit = size >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            reversed_[i] = j;
        }
        for (std::size_t k = 0; k < size / 2; ++k) {
            const double angle = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(size);
            twiddle_[k] = {std::cos(angle), std::sin(angle)};
        }
    }
    void run(std::vector<std::complex<double>>& a) const {
        for (std::size_t i = 1; i < size_; ++i)
            if (i < reversed_[i])
                std::swap(a[i], a[reversed_[i]]);
        for (std::size_t length = 2; length <= size_; length <<= 1) {
            const std::size_t step = size_ / length;
            for (std::size_t i = 0; i < size_; i += length)
                for (std::size_t k = 0; k < length / 2; ++k) {
                    const auto u = a[i + k];
                    const auto v = a[i + k + length / 2] * twiddle_[k * step];
                    a[i + k] = u + v;
                    a[i + k + length / 2] = u - v;
                }
        }
    }

  private:
    std::size_t size_;
    std::vector<std::size_t> reversed_;
    std::vector<std::complex<double>> twiddle_;
};

[[nodiscard]] std::vector<double> hann(const std::size_t size) {
    std::vector<double> window(size);
    for (std::size_t i = 0; i < size; ++i)
        window[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                         static_cast<double>(size));
    return window;
}

// With a periodic Hann window the squares of a transform's bins around a sine of amplitude A sum
// to A^2 * N * (3N / 8) / 4, so this factor makes a sine of amplitude 1 read a power of 1.
[[nodiscard]] constexpr double powerScale(const std::size_t size) noexcept {
    return 32.0 / (3.0 * static_cast<double>(size) * static_cast<double>(size));
}

// The share of each transform bin a band takes. A bin stands for the frequencies half a bin
// either side of its centre, and a band takes the part of each bin that lies inside it, so the
// bands together hold every bin exactly once however narrow they are.
struct BandBins final {
    std::uint32_t first = 0;
    std::vector<double> shares;
};

[[nodiscard]] std::vector<BandBins> bandBins() {
    std::vector<BandBins> bands(kTableBands);
    const double binWidth = static_cast<double>(kTableRate) / kTableFftSize;
    const std::uint32_t top = kTableFftSize / 2 - 1;
    for (std::uint32_t band = 0; band < kTableBands; ++band) {
        const double low = tableBandEdge(band);
        const double high = tableBandEdge(band + 1);
        const auto first = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(std::llround(low / binWidth), 1, top));
        const auto last = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(std::llround(high / binWidth), 1, top));
        bands[band].first = first;
        for (std::uint32_t bin = first; bin <= last; ++bin) {
            const double from = std::max(low, (static_cast<double>(bin) - 0.5) * binWidth);
            const double to = std::min(high, (static_cast<double>(bin) + 0.5) * binWidth);
            bands[band].shares.push_back(std::max(0.0, to - from) / binWidth);
        }
    }
    return bands;
}

// The pitch class (0 is C) of a frequency.
[[nodiscard]] std::size_t pitchClassOf(const double frequency) noexcept {
    const auto note = std::llround(69.0 + 12.0 * std::log2(frequency / 440.0));
    return static_cast<std::size_t>(((note % 12) + 12) % 12);
}

// The pitched range: from just under C2 (65.4 Hz), under which the partials of a note are closer
// than the transform can tell apart, to 5 kHz, above which there are overtones and noise rather
// than notes.
constexpr double kPitchLowHz = 60.0;
constexpr double kPitchHighHz = 5000.0;

struct ChannelWeights final {
    double left = 0.0;
    double right = 0.0;
    double cross = 0.0;
};
[[nodiscard]] constexpr ChannelWeights weightsOf(const TableChannel channel) noexcept {
    switch (channel) {
    case TableChannel::Left:
        return {1.0, 0.0, 0.0};
    case TableChannel::Right:
        return {0.0, 1.0, 0.0};
    case TableChannel::Mid:
        return {0.25, 0.25, 0.5};
    case TableChannel::Side:
        return {0.25, 0.25, -0.5};
    case TableChannel::Mix:
        break;
    }
    return {0.5, 0.5, 0.0};
}

} // namespace

double tableBandEdge(const std::uint32_t band) noexcept {
    return kTableLowHz * std::pow(kTableHighHz / kTableLowHz,
                                  static_cast<double>(std::min(band, kTableBands)) / kTableBands);
}

AudioResult<std::uint64_t> buildTable(const std::uint64_t frames, const TableReader& reader,
                                      const TableSink& sink,
                                      const CancelAudioWork& cancel) noexcept {
    using Result = AudioResult<std::uint64_t>;
    if (!reader || !sink)
        return Result::failure(AudioError::codeOnly(AudioErrorCode::InvalidBuffer));
    try {
        const Transform transform(kTableFftSize);
        const Transform pitchTransform(kTablePitchFftSize);
        const auto window = hann(kTableFftSize);
        const auto pitchWindow = hann(kTablePitchFftSize);
        const auto bands = bandBins();
        const double scale = powerScale(kTableFftSize);
        const double pitchScale = powerScale(kTablePitchFftSize);
        std::vector<float> samples(std::size_t{kTableFftSize} * 2U);
        std::vector<float> pitchSamples(std::size_t{kTablePitchFftSize} * 2U);
        std::vector<std::complex<double>> a(kTableFftSize);
        std::vector<std::complex<double>> p(kTablePitchFftSize);
        std::array<float, kTableRowFloats> row{};
        std::array<double, kTableBands> previous{};
        std::array<float, kTablePitchClasses> pitch{};
        const std::uint64_t rows = tableRows(frames);
        for (std::uint64_t index = 0; index < rows; ++index) {
            if (cancel && cancel())
                return Result::failure(AudioError::codeOnly(AudioErrorCode::Cancelled));
            const auto center = static_cast<std::int64_t>(index * kTableHopFrames);
            if (!reader(center - static_cast<std::int64_t>(kTableFftSize / 2), samples))
                return Result::failure(AudioError::codeOnly(AudioErrorCode::FileReadFailed));
            // One transform carries both channels: left in the real part, right in the imaginary.
            for (std::size_t i = 0; i < kTableFftSize; ++i)
                a[i] = {static_cast<double>(samples[i * 2]) * window[i],
                        static_cast<double>(samples[i * 2 + 1]) * window[i]};
            transform.run(a);
            double flux = 0.0;
            for (std::uint32_t band = 0; band < kTableBands; ++band) {
                double left = 0.0;
                double right = 0.0;
                double cross = 0.0;
                for (std::size_t part = 0; part < bands[band].shares.size(); ++part) {
                    const auto bin = bands[band].first + static_cast<std::uint32_t>(part);
                    const double share = bands[band].shares[part];
                    const auto positive = a[bin];
                    const auto negative = std::conj(a[kTableFftSize - bin]);
                    const auto l = (positive + negative) * 0.5;
                    const auto r = (positive - negative) * std::complex<double>(0.0, -0.5);
                    left += std::norm(l) * share;
                    right += std::norm(r) * share;
                    cross += (l * std::conj(r)).real() * share;
                }
                const double factor = scale;
                row[kTableLeftBands + band] = static_cast<float>(left * factor);
                row[kTableRightBands + band] = static_cast<float>(right * factor);
                row[kTableCrossBands + band] = static_cast<float>(cross * factor);
                const double magnitude = std::sqrt((left + right) * 0.5 * factor);
                flux += std::max(0.0, magnitude - previous[band]);
                previous[band] = magnitude;
            }
            // The 10 ms that start at the row's time sit in the middle of the window just read.
            double meanLeft = 0.0;
            double meanRight = 0.0;
            double meanCross = 0.0;
            float peakLeft = 0.0F;
            float peakRight = 0.0F;
            for (std::size_t i = 0; i < kTableHopFrames; ++i) {
                const float l = samples[(kTableFftSize / 2 + i) * 2];
                const float r = samples[(kTableFftSize / 2 + i) * 2 + 1];
                meanLeft += static_cast<double>(l) * static_cast<double>(l);
                meanRight += static_cast<double>(r) * static_cast<double>(r);
                meanCross += static_cast<double>(l) * static_cast<double>(r);
                peakLeft = std::max(peakLeft, std::fabs(l));
                peakRight = std::max(peakRight, std::fabs(r));
            }
            row[kTableMeanLeft] = static_cast<float>(meanLeft / kTableHopFrames);
            row[kTableMeanRight] = static_cast<float>(meanRight / kTableHopFrames);
            row[kTableMeanCross] = static_cast<float>(meanCross / kTableHopFrames);
            row[kTablePeakLeft] = peakLeft;
            row[kTablePeakRight] = peakRight;
            // A window that passes the end of the sound sees the sound cut off, which spreads
            // over the bands like a click. The end of the sound is not a start, so those rows
            // have no flux.
            const bool pastEnd =
                static_cast<std::uint64_t>(center) + kTableFftSize / 2 > frames;
            row[kTableFlux] = pastEnd ? 0.0F : static_cast<float>(flux);
            if (index % kTablePitchEveryRows == 0) {
                if (!reader(center - static_cast<std::int64_t>(kTablePitchFftSize / 2),
                            pitchSamples))
                    return Result::failure(AudioError::codeOnly(AudioErrorCode::FileReadFailed));
                for (std::size_t i = 0; i < kTablePitchFftSize; ++i)
                    p[i] = {0.5 *
                                (static_cast<double>(pitchSamples[i * 2]) +
                                 static_cast<double>(pitchSamples[i * 2 + 1])) *
                                pitchWindow[i],
                            0.0};
                pitchTransform.run(p);
                // Each partial is a peak of the spectrum. Its power (the peak bin and its two
                // neighbours, which hold nearly all of a windowed tone) goes to the pitch class
                // of the peak's frequency, read between bins from the parabola through the three
                // logarithms, so a low note is not smeared over its neighbours.
                std::array<double, kTablePitchClasses> sums{};
                const double pitchBin = static_cast<double>(kTableRate) / kTablePitchFftSize;
                const auto firstBin = static_cast<std::size_t>(std::ceil(kPitchLowHz / pitchBin));
                const auto lastBin = static_cast<std::size_t>(std::floor(kPitchHighHz / pitchBin));
                for (std::size_t bin = firstBin; bin <= lastBin; ++bin) {
                    const double before = std::norm(p[bin - 1]);
                    const double at = std::norm(p[bin]);
                    const double after = std::norm(p[bin + 1]);
                    if (!(at > before) || !(at >= after) || !(at > 0.0))
                        continue;
                    double offset = 0.0;
                    if (before > 0.0 && after > 0.0) {
                        const double a0 = std::log(before);
                        const double a1 = std::log(at);
                        const double a2 = std::log(after);
                        const double bend = a0 - 2.0 * a1 + a2;
                        if (bend < 0.0)
                            offset = std::clamp(0.5 * (a0 - a2) / bend, -0.5, 0.5);
                    }
                    sums[pitchClassOf((static_cast<double>(bin) + offset) * pitchBin)] +=
                        before + at + after;
                }
                for (std::size_t note = 0; note < kTablePitchClasses; ++note)
                    pitch[note] = static_cast<float>(sums[note] * pitchScale);
            }
            std::copy(pitch.begin(), pitch.end(), row.begin() + kTablePitch);
            if (!sink(index, row))
                return Result::failure(AudioError::codeOnly(AudioErrorCode::FileReadFailed));
        }
        return Result::success(std::uint64_t{rows});
    } catch (const std::bad_alloc&) {
        return Result::failure(AudioError::codeOnly(AudioErrorCode::AllocationFailure));
    } catch (...) {
        return Result::failure(AudioError::codeOnly(AudioErrorCode::InvalidState));
    }
}

double tableBandPower(const std::span<const float> row, const TableChannel channel,
                      const double lowHz, const double highHz) noexcept {
    if (row.size() < kTableRowFloats || !(highHz > lowHz) || !(lowHz > 0.0))
        return 0.0;
    const auto weights = weightsOf(channel);
    const double span = std::log(kTableHighHz / kTableLowHz) / kTableBands;
    const double from = std::log(std::max(lowHz, kTableLowHz) / kTableLowHz) / span;
    const double to = std::log(std::min(highHz, kTableHighHz) / kTableLowHz) / span;
    double power = 0.0;
    for (std::uint32_t band = 0; band < kTableBands; ++band) {
        const double covered = std::min<double>(to, band + 1) - std::max<double>(from, band);
        if (covered <= 0.0)
            continue;
        power += covered * (weights.left * number(row, kTableLeftBands + band) +
                            weights.right * number(row, kTableRightBands + band) +
                            weights.cross * number(row, kTableCrossBands + band));
    }
    return std::max(power, 0.0);
}

double tableLevelPower(const std::span<const float> row, const TableChannel channel) noexcept {
    if (row.size() < kTableRowFloats)
        return 0.0;
    const auto weights = weightsOf(channel);
    // Twice the mean square, so a sine of amplitude 1 reads 1.
    return std::max(0.0, 2.0 * (weights.left * number(row, kTableMeanLeft) +
                                weights.right * number(row, kTableMeanRight) +
                                weights.cross * number(row, kTableMeanCross)));
}

double tablePowerDb(const double power) noexcept {
    if (!(power > 0.0))
        return kTableSilenceDb;
    return std::max(kTableSilenceDb, 10.0 * std::log10(power));
}

std::vector<float> tableEnvelope(const std::span<const double> power,
                                 const TableEnvelopeSettings& settings) {
    const std::size_t rows = power.size();
    std::vector<float> curve(rows, 0.0F);
    if (rows == 0)
        return curve;
    std::vector<double> db(rows);
    for (std::size_t row = 0; row < rows; ++row)
        db[row] = tablePowerDb(power[row]);
    const double range = settings.rangeDb > 0.0 ? settings.rangeDb : 48.0;
    const auto against = [&](const double value, const double reference) {
        const double held = std::max(reference, kTableReferenceFloorDb);
        return std::clamp((value - (held - range)) / range, 0.0, 1.0);
    };
    std::vector<double> value(rows);
    switch (settings.normalize) {
    case TableNormalize::Percentile95: {
        auto sorted = db;
        const auto nth = static_cast<std::size_t>(
            std::min<double>(static_cast<double>(rows - 1),
                             std::ceil(0.95 * static_cast<double>(rows)) - 1.0));
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(nth),
                         sorted.end());
        for (std::size_t row = 0; row < rows; ++row)
            value[row] = against(db[row], sorted[nth]);
        break;
    }
    case TableNormalize::Peak: {
        const double peak = *std::max_element(db.begin(), db.end());
        for (std::size_t row = 0; row < rows; ++row)
            value[row] = against(db[row], peak);
        break;
    }
    case TableNormalize::Rolling: {
        const double seconds = settings.rollingSeconds > 0.0 ? settings.rollingSeconds : 4.0;
        const auto span = static_cast<std::size_t>(std::max<long long>(
            1, std::llround(seconds * kTableRate / kTableHopFrames)));
        // The rows of the span whose level no later row of the span exceeds, loudest first.
        std::deque<std::size_t> loudest;
        for (std::size_t row = 0; row < rows; ++row) {
            while (!loudest.empty() && db[loudest.back()] <= db[row])
                loudest.pop_back();
            loudest.push_back(row);
            if (loudest.front() + span <= row)
                loudest.pop_front();
            value[row] = against(db[row], db[loudest.front()]);
        }
        break;
    }
    case TableNormalize::Manual: {
        const double floor = settings.floorDb;
        const double ceiling = settings.ceilingDb > floor ? settings.ceilingDb : floor + 1.0;
        for (std::size_t row = 0; row < rows; ++row)
            value[row] = std::clamp((db[row] - floor) / (ceiling - floor), 0.0, 1.0);
        break;
    }
    }
    const double hop = static_cast<double>(kTableHopFrames) / kTableRate;
    const auto step = [hop](const double seconds) {
        return seconds > 0.0 ? 1.0 - std::exp(-hop / seconds) : 1.0;
    };
    const double attack = step(settings.attackSeconds);
    const double release = step(settings.releaseSeconds);
    double held = 0.0;
    for (std::size_t row = 0; row < rows; ++row) {
        held += (value[row] - held) * (value[row] > held ? attack : release);
        curve[row] = static_cast<float>(held);
    }
    return curve;
}

double tableCurveAt(const std::span<const float> curve, const double seconds) noexcept {
    if (curve.empty() || !(seconds >= 0.0))
        return 0.0;
    const double position = seconds * kTableRate / kTableHopFrames;
    const double last = static_cast<double>(curve.size() - 1);
    if (position > last)
        return 0.0;
    const auto row = static_cast<std::size_t>(position);
    if (row + 1 >= curve.size())
        return number(curve, row);
    const double part = position - static_cast<double>(row);
    return number(curve, row) + (number(curve, row + 1) - number(curve, row)) * part;
}

namespace {

// The power of `channel` in each band of a row.
[[nodiscard]] std::array<double, kTableBands> bandPowers(const std::span<const float> row,
                                                         const TableChannel channel) noexcept {
    std::array<double, kTableBands> powers{};
    if (row.size() < kTableRowFloats)
        return powers;
    const auto weights = weightsOf(channel);
    for (std::uint32_t band = 0; band < kTableBands; ++band)
        powers[band] = std::max(0.0, weights.left * number(row, kTableLeftBands + band) +
                                         weights.right * number(row, kTableRightBands + band) +
                                         weights.cross * number(row, kTableCrossBands + band));
    return powers;
}

// Under this power a row is silence to the features: -90 dB.
constexpr double kFeatureSilence = 1e-9;

} // namespace

double tableBrightness(const std::span<const float> row, const TableChannel channel) noexcept {
    const auto powers = bandPowers(row, channel);
    double total = 0.0;
    double weighted = 0.0;
    for (std::uint32_t band = 0; band < kTableBands; ++band) {
        total += powers[band];
        weighted += powers[band] * static_cast<double>(band);
    }
    if (!(total > kFeatureSilence))
        return 0.0;
    return std::clamp(weighted / total / (kTableBands - 1), 0.0, 1.0);
}

double tableNoisiness(const std::span<const float> row, const TableChannel channel) noexcept {
    const auto powers = bandPowers(row, channel);
    double total = 0.0;
    double densities = 0.0;
    double logarithms = 0.0;
    for (std::uint32_t band = 0; band < kTableBands; ++band) {
        total += powers[band];
        // The bands widen with frequency, so what is compared is the power per hertz: noise
        // with the same power at every frequency reads flat.
        const double density = powers[band] / (tableBandEdge(band + 1) - tableBandEdge(band));
        densities += density;
        // A band with nothing in it counts as far below the others, not as minus infinity.
        logarithms += std::log(std::max(density, 1e-18));
    }
    if (!(total > kFeatureSilence))
        return 0.0;
    const double geometric = std::exp(logarithms / kTableBands);
    return std::clamp(geometric / (densities / kTableBands), 0.0, 1.0);
}

double tablePan(const std::span<const float> row) noexcept {
    if (row.size() < kTableRowFloats)
        return 0.0;
    const double left = number(row, kTableMeanLeft);
    const double right = number(row, kTableMeanRight);
    if (!(left + right > kFeatureSilence))
        return 0.0;
    return std::clamp((right - left) / (right + left), -1.0, 1.0);
}

double tableWidth(const std::span<const float> row) noexcept {
    const double mid = tableLevelPower(row, TableChannel::Mid);
    const double side = tableLevelPower(row, TableChannel::Side);
    if (!(mid + side > kFeatureSilence))
        return 0.0;
    return std::clamp(side / (mid + side), 0.0, 1.0);
}

std::array<double, kTablePitchClasses> tablePitchShares(const std::span<const float> row) noexcept {
    std::array<double, kTablePitchClasses> shares{};
    if (row.size() < kTableRowFloats)
        return shares;
    double total = 0.0;
    for (std::size_t note = 0; note < kTablePitchClasses; ++note)
        total += number(row, kTablePitch + note);
    if (!(total > kFeatureSilence))
        return shares;
    for (std::size_t note = 0; note < kTablePitchClasses; ++note)
        shares[note] = number(row, kTablePitch + note) / total;
    return shares;
}

std::vector<std::uint32_t> tableHits(const std::span<const float> flux,
                                     const TableHitSettings& settings) {
    std::vector<std::uint32_t> hits;
    const std::size_t rows = flux.size();
    if (rows == 0)
        return hits;
    const float largest = *std::max_element(flux.begin(), flux.end());
    if (!(largest > 0.0F))
        return hits;
    const double sensitivity = settings.sensitivity > 0.0 ? settings.sensitivity : 1.5;
    const auto gap = static_cast<std::size_t>(std::max<long long>(
        1, std::llround(std::max(settings.minimumGapSeconds, 0.0) * kTableRate / kTableHopFrames)));
    constexpr std::size_t kAround = 10; // A tenth of a second each side.
    constexpr std::size_t kPeak = 3;
    // Running sums, so the mean around a row is two reads.
    std::vector<double> sums(rows + 1, 0.0);
    for (std::size_t row = 0; row < rows; ++row)
        sums[row + 1] = sums[row] + number(flux, row);
    for (std::size_t row = 0; row < rows; ++row) {
        const float value = flux[row];
        if (!(value > 0.1F * largest))
            continue;
        bool peak = true;
        for (std::size_t other = row > kPeak ? row - kPeak : 0;
             other < std::min(rows, row + kPeak + 1) && peak; ++other)
            // The earlier of two equal neighbours is the peak.
            peak = other == row || flux[other] < value || (flux[other] == value && other > row);
        if (!peak)
            continue;
        const std::size_t first = row > kAround ? row - kAround : 0;
        const std::size_t last = std::min(rows, row + kAround + 1);
        const double mean = (sums[last] - sums[first]) / static_cast<double>(last - first);
        if (!(static_cast<double>(value) > sensitivity * mean))
            continue;
        if (!hits.empty() && row - hits.back() < gap)
            continue;
        hits.push_back(static_cast<std::uint32_t>(row));
    }
    return hits;
}

namespace {
// The least the onset strength must correlate with itself one beat later for the sound to have a
// beat at all.
constexpr double kBeatRepeatFloor = 0.1;
} // namespace

TableBeats tableBeats(const std::span<const float> flux, const TableBeatSettings& settings) {
    TableBeats result;
    const std::size_t rows = flux.size();
    const double rowsPerSecond = static_cast<double>(kTableRate) / kTableHopFrames;
    const double slowest = std::clamp(settings.minimumBpm, 20.0, 400.0);
    const double fastest = std::clamp(std::max(settings.maximumBpm, slowest + 1.0), 21.0, 480.0);
    const auto shortest = static_cast<std::size_t>(std::floor(60.0 / fastest * rowsPerSecond));
    const auto longest = static_cast<std::size_t>(std::ceil(60.0 / slowest * rowsPerSecond));
    if (shortest < 2 || rows < longest * 2 + 2)
        return result;
    // The onset strength: the flux above the mean of the half second around it, in units of its
    // own spread, so the answer does not depend on how loud the sound is.
    std::vector<double> sums(rows + 1, 0.0);
    for (std::size_t row = 0; row < rows; ++row)
        sums[row + 1] = sums[row] + number(flux, row);
    constexpr std::size_t kAround = 25;
    std::vector<double> onset(rows);
    double square = 0.0;
    for (std::size_t row = 0; row < rows; ++row) {
        const std::size_t first = row > kAround ? row - kAround : 0;
        const std::size_t last = std::min(rows, row + kAround + 1);
        const double mean = (sums[last] - sums[first]) / static_cast<double>(last - first);
        onset[row] = std::max(0.0, number(flux, row) - mean);
        square += onset[row] * onset[row];
    }
    const double spread = std::sqrt(square / static_cast<double>(rows));
    if (!(spread > 1e-12))
        return result;
    for (auto& value : onset)
        value /= spread;
    // The period: the lag at which the onset strength best repeats, preferring 120 a minute.
    // "Repeats" is the correlation of the strength with itself one lag later (about its mean, in
    // units of its variance): 1 for a perfect pulse, about 0 for a sound with none.
    double mean = 0.0;
    for (const double value : onset)
        mean += value;
    mean /= static_cast<double>(rows);
    double variance = 0.0;
    for (const double value : onset)
        variance += (value - mean) * (value - mean);
    variance /= static_cast<double>(rows);
    if (!(variance > 1e-12))
        return result;
    const auto repeat = [&](const std::size_t lag) {
        double sum = 0.0;
        for (std::size_t row = lag; row < rows; ++row)
            sum += (onset[row] - mean) * (onset[row - lag] - mean);
        return sum / static_cast<double>(rows - lag) / variance;
    };
    double best = 0.0;
    std::size_t bestLag = 0;
    for (std::size_t lag = shortest; lag <= longest; ++lag) {
        const double bpm = 60.0 * rowsPerSecond / static_cast<double>(lag);
        const double octaves = std::log2(bpm / 120.0);
        const double score = repeat(lag) * std::exp(-0.5 * octaves * octaves);
        if (score > best) {
            best = score;
            bestLag = lag;
        }
    }
    // A sound whose strength does not repeat (a held tone, noise, one hit) has no beat.
    if (bestLag == 0 || !(repeat(bestLag) > kBeatRepeatFloor))
        return result;
    // Between rows: the top of the parabola through the best lag and its neighbours.
    double period = static_cast<double>(bestLag);
    if (bestLag > shortest && bestLag < longest) {
        const double before = repeat(bestLag - 1);
        const double at = repeat(bestLag);
        const double after = repeat(bestLag + 1);
        const double bend = before - 2.0 * at + after;
        if (bend < 0.0)
            period += std::clamp(0.5 * (before - after) / bend, -0.5, 0.5);
    }
    result.bpm = 60.0 * rowsPerSecond / period;
    // The beats: each row's best score is its own onset strength plus the best score one period
    // or so before it, less a penalty for a spacing that is not the period.
    constexpr double kTightness = 100.0;
    std::vector<double> score(rows);
    std::vector<std::int64_t> from(rows, -1);
    const auto nearest = static_cast<std::size_t>(std::max(1.0, std::floor(period * 0.5)));
    const auto farthest = static_cast<std::size_t>(std::ceil(period * 2.0));
    for (std::size_t row = 0; row < rows; ++row) {
        double carried = 0.0;
        std::int64_t source = -1;
        for (std::size_t back = nearest; back <= farthest && back <= row; ++back) {
            const double spacing = std::log(static_cast<double>(back) / period);
            const double candidate = score[row - back] - kTightness * spacing * spacing;
            if (source < 0 || candidate > carried) {
                carried = candidate;
                source = static_cast<std::int64_t>(row - back);
            }
        }
        score[row] = onset[row] + (source >= 0 ? std::max(carried, 0.0) : 0.0);
        from[row] = source >= 0 && carried > 0.0 ? source : -1;
    }
    // The last beat is the best row of the last period; the rest are read back from it.
    std::size_t last = rows - 1;
    for (std::size_t row = rows > farthest ? rows - nearest * 2 : 0; row < rows; ++row)
        if (score[row] > score[last])
            last = row;
    std::vector<double> beats;
    for (std::int64_t row = static_cast<std::int64_t>(last); row >= 0;
         row = from[static_cast<std::size_t>(row)])
        beats.push_back(static_cast<double>(row) / rowsPerSecond);
    std::reverse(beats.begin(), beats.end());
    result.beats = std::move(beats);
    return result;
}

} // namespace bloom::media::audio::analysis
