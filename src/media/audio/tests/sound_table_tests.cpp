// #631 the audio analysis table against synthesized sounds whose answers are known.

#include <bloom/media/audio/sound_table.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

using namespace bloom::media::audio;
namespace an = bloom::media::audio::analysis;

namespace {
int failures = 0;
void expect(const bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
template <typename Value>
bool near(const Value value, const double want, const double tolerance) {
    return std::fabs(static_cast<double>(value) - want) <= tolerance;
}

constexpr std::uint32_t kRate = an::kTableRate;

// Interleaved stereo: a sine of `hertz` at `left` and `right` amplitudes from `fromFrame`.
std::vector<float> sine(const std::size_t frames, const double hertz, const double left,
                        const double right, const std::size_t fromFrame = 0) {
    std::vector<float> samples(frames * 2, 0.0F);
    for (std::size_t frame = fromFrame; frame < frames; ++frame) {
        const double value =
            std::sin(2.0 * std::numbers::pi * hertz * static_cast<double>(frame) / kRate);
        samples[frame * 2] = static_cast<float>(left * value);
        samples[frame * 2 + 1] = static_cast<float>(right * value);
    }
    return samples;
}

struct Table final {
    std::uint64_t rows = 0;
    std::vector<float> values;
    [[nodiscard]] std::span<const float> row(const std::uint64_t index) const {
        return std::span<const float>(values).subspan(index * an::kTableRowFloats,
                                                      an::kTableRowFloats);
    }
};

Table build(const std::vector<float>& samples) {
    const std::uint64_t frames = samples.size() / 2;
    Table table;
    const auto built = an::buildTable(
        frames,
        [&](const std::int64_t first, const std::span<float> output) {
            std::fill(output.begin(), output.end(), 0.0F);
            for (std::size_t i = 0; i < output.size() / 2; ++i) {
                const auto frame = first + static_cast<std::int64_t>(i);
                if (frame < 0 || static_cast<std::uint64_t>(frame) >= frames)
                    continue;
                output[i * 2] = samples[static_cast<std::size_t>(frame) * 2];
                output[i * 2 + 1] = samples[static_cast<std::size_t>(frame) * 2 + 1];
            }
            return true;
        },
        [&](const std::uint64_t row, const std::span<const float> values) {
            expect(row == table.rows && values.size() == an::kTableRowFloats,
                   "rows arrive in order, whole");
            table.values.insert(table.values.end(), values.begin(), values.end());
            ++table.rows;
            return true;
        });
    expect(built && *built.value() == table.rows, "the table builds");
    return table;
}

void rowsAndBands() {
    expect(an::tableRows(0) == 1 && an::tableRows(479) == 1 && an::tableRows(480) == 2 &&
               an::tableRows(kRate) == 101,
           "a row every 480 frames, the first at the start");
    expect(near(an::tableBandEdge(0), 20.0, 1e-9) && near(an::tableBandEdge(64), 20000.0, 1e-6) &&
               near(an::tableBandEdge(32), std::sqrt(20.0 * 20000.0), 1e-6),
           "the bands are even in the logarithm of frequency");
}

// A sine of amplitude 0.5 on the left only: its power is 0.25 on the left, nothing on the right,
// and the mid and side signals each carry half its amplitude.
void channels() {
    const auto table = build(sine(kRate / 2, 1000.0, 0.5, 0.0));
    expect(table.rows == 51, "half a second is 51 rows");
    const auto row = table.row(25);
    const auto band = [&](const an::TableChannel channel) {
        return an::tableBandPower(row, channel, 500.0, 2000.0);
    };
    expect(near(band(an::TableChannel::Left), 0.25, 0.005),
           "left band power " + std::to_string(band(an::TableChannel::Left)));
    expect(band(an::TableChannel::Right) < 1e-9, "the right channel is silent");
    expect(near(band(an::TableChannel::Mix), 0.125, 0.003), "the mix is the mean of the channels");
    expect(near(band(an::TableChannel::Mid), 0.0625, 0.002) &&
               near(band(an::TableChannel::Side), 0.0625, 0.002),
           "mid and side each carry half the amplitude");
    expect(near(an::tableBandPower(row, an::TableChannel::Left, 20.0, 20000.0), 0.25, 0.005),
           "the whole range holds the whole power");
    expect(an::tableBandPower(row, an::TableChannel::Left, 4000.0, 20000.0) < 1e-6,
           "bands away from the sine are empty");
    expect(near(an::tableLevelPower(row, an::TableChannel::Left), 0.25, 0.005) &&
               near(an::tableLevelPower(row, an::TableChannel::Mix), 0.125, 0.003),
           "the level of the 10 ms agrees");
    expect(near(row[an::kTablePeakLeft], 0.5, 0.002) && row[an::kTablePeakRight] == 0.0F,
           "the peaks are the largest samples");
    expect(near(an::tablePowerDb(0.25), -6.0206, 1e-3) &&
               an::tablePowerDb(0.0) == an::kTableSilenceDb,
           "power reads in decibels");

    // The same sine on both channels is all mid; in opposite phase it is all side.
    const auto together = build(sine(kRate / 4, 1000.0, 0.5, 0.5));
    const auto apart = build(sine(kRate / 4, 1000.0, 0.5, -0.5));
    const auto mid = [](const Table& t, const an::TableChannel channel) {
        return an::tableBandPower(t.row(12), channel, 500.0, 2000.0);
    };
    expect(near(mid(together, an::TableChannel::Mid), 0.25, 0.005) &&
               mid(together, an::TableChannel::Side) < 1e-9,
           "equal channels are all mid");
    expect(near(mid(apart, an::TableChannel::Side), 0.25, 0.005) &&
               mid(apart, an::TableChannel::Mid) < 1e-9,
           "opposite channels are all side");
}

// A band narrower than a transform bin takes its share of the nearest bin, so a low sine still
// reads its power over the low range.
void lowBands() {
    const auto table = build(sine(kRate / 2, 60.0, 0.5, 0.5));
    const double low = an::tableBandPower(table.row(25), an::TableChannel::Mix, 20.0, 250.0);
    expect(near(low, 0.25, 0.05), "a 60 Hz sine reads its power in the low bands: " +
                                      std::to_string(low));
    expect(an::tableBandPower(table.row(25), an::TableChannel::Mix, 1000.0, 20000.0) < 1e-6,
           "and nothing above");
}

void pitchClasses() {
    const auto share = [](const double hertz, const std::size_t note) {
        const auto table = build(sine(kRate / 2, hertz, 0.5, 0.5));
        const auto row = table.row(24);
        double total = 0.0;
        for (std::size_t index = 0; index < an::kTablePitchClasses; ++index)
            total += static_cast<double>(row[an::kTablePitch + index]);
        return total > 0.0 ? static_cast<double>(row[an::kTablePitch + note]) / total : 0.0;
    };
    expect(share(440.0, 9) > 0.95, "A 440 is the pitch class A");
    expect(share(261.6256, 0) > 0.95, "middle C is the pitch class C");
    expect(share(659.2551, 4) > 0.95, "E5 is the pitch class E");
    // Low notes, whose semitones are about as wide as a transform bin, are still told apart.
    expect(share(110.0, 9) > 0.95 && share(65.4064, 0) > 0.95 && share(184.9972, 6) > 0.95,
           "A2, C2 and F sharp 3 read their own pitch class");
    const auto silent = build(std::vector<float>(kRate / 2, 0.0F));
    const auto shares = an::tablePitchShares(silent.row(10));
    expect(std::all_of(shares.begin(), shares.end(), [](const double v) { return v == 0.0; }),
           "silence has no pitch class");
}

// Interleaved stereo: `seconds` of clicks (5 ms bursts of a 2 kHz sine) every `period` seconds
// from `first`, on both channels.
std::vector<float> clicks(const double seconds, const double period, const double first,
                          const std::vector<int>& skipped = {}) {
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> samples(frames * 2, 0.0F);
    int index = 0;
    for (double at = first; at < seconds; at += period, ++index) {
        if (std::find(skipped.begin(), skipped.end(), index) != skipped.end())
            continue;
        const auto start = static_cast<std::size_t>(std::llround(at * kRate));
        for (std::size_t i = 0; i < 240 && start + i < frames; ++i) {
            const auto value = static_cast<float>(
                0.8 * std::sin(2.0 * std::numbers::pi * 2000.0 * static_cast<double>(i) / kRate));
            samples[(start + i) * 2] = value;
            samples[(start + i) * 2 + 1] = value;
        }
    }
    return samples;
}

std::vector<float> fluxOf(const Table& table) {
    std::vector<float> flux(table.rows);
    for (std::uint64_t row = 0; row < table.rows; ++row)
        flux[row] = table.row(row)[an::kTableFlux];
    return flux;
}

void hits() {
    // Clicks at 0.5, 1.0, 1.5 ... 3.5 seconds: seven hits, each within two rows of its click.
    const auto flux = fluxOf(build(clicks(4.0, 0.5, 0.5)));
    const auto found = an::tableHits(flux, {});
    bool placed = found.size() == 7;
    for (std::size_t index = 0; index < found.size() && placed; ++index) {
        const double want = 0.5 * static_cast<double>(index + 1);
        placed = std::fabs(an::tableRowSeconds(found[index]) - want) <= 0.025;
    }
    std::string list;
    for (const auto row : found)
        list += std::to_string(row) + " ";
    expect(placed, "each click is one hit at its time: rows " + list);
    // A gap longer than the spacing leaves every other hit.
    an::TableHitSettings sparse;
    sparse.minimumGapSeconds = 0.8;
    expect(an::tableHits(flux, sparse).size() == 4, "a minimum gap drops the hits inside it");
    // A held tone has one hit, where it starts; silence has none.
    const auto held = an::tableHits(fluxOf(build(sine(kRate, 500.0, 0.5, 0.5, kRate / 2))), {});
    expect(held.size() == 1 && std::fabs(an::tableRowSeconds(held.front()) - 0.5) <= 0.025,
           "a held tone is one hit, and the end of the sound is not a hit");
    expect(an::tableHits(fluxOf(build(std::vector<float>(kRate, 0.0F))), {}).empty(),
           "silence has no hits");
}

void beats() {
    const auto check = [](const double bpm, const double first, const std::vector<int>& skipped,
                          const std::string& name) {
        const double period = 60.0 / bpm;
        const auto found =
            an::tableBeats(fluxOf(build(clicks(10.0, period, first, skipped))), {});
        expect(std::fabs(found.bpm - bpm) < 1.0,
               name + ": tempo " + std::to_string(found.bpm));
        // Every beat between the first and the last click falls on the grid of the clicks.
        double worst = 0.0;
        std::size_t counted = 0;
        for (const double beat : found.beats) {
            if (beat < first - 0.05 || beat > 9.5)
                continue;
            const double phase = (beat - first) / period;
            worst = std::max(worst, std::fabs(phase - std::round(phase)) * period);
            ++counted;
        }
        const auto expected = static_cast<std::size_t>((9.5 - first) / period);
        expect(worst <= 0.03 && counted + 1 >= expected && counted <= expected + 2,
               name + ": " + std::to_string(counted) + " beats of about " +
                   std::to_string(expected) + ", worst offset " + std::to_string(worst));
    };
    check(120.0, 0.25, {}, "120 a minute");
    check(90.0, 0.4, {}, "90 a minute");
    check(150.0, 0.1, {}, "150 a minute");
    // The beat carries through clicks that are missing.
    check(120.0, 0.25, {5, 6, 11}, "120 a minute with clicks missing");
    const auto none = an::tableBeats(fluxOf(build(sine(4 * kRate, 440.0, 0.5, 0.5))), {});
    const auto silent = an::tableBeats(fluxOf(build(std::vector<float>(8 * kRate, 0.0F))), {});
    expect(silent.bpm == 0.0 && silent.beats.empty(), "silence has no beat");
    expect(none.bpm == 0.0 && none.beats.empty(), "a held tone has no beat");
    std::vector<float> noise(8 * kRate * 2);
    std::uint32_t state = 0x2468ACEU;
    for (auto& sample : noise) {
        state = state * 1664525U + 1013904223U;
        sample = static_cast<float>(static_cast<double>(state >> 8) / 8388608.0 - 1.0) * 0.5F;
    }
    const auto hiss = an::tableBeats(fluxOf(build(noise)), {});
    expect(hiss.bpm == 0.0 && hiss.beats.empty(),
           "noise has no beat: " + std::to_string(hiss.bpm));
}

void features() {
    const auto low = build(sine(kRate / 2, 100.0, 0.5, 0.5));
    const auto high = build(sine(kRate / 2, 8000.0, 0.5, 0.5));
    const double lowBright = an::tableBrightness(low.row(25), an::TableChannel::Mix);
    const double highBright = an::tableBrightness(high.row(25), an::TableChannel::Mix);
    // 100 Hz is 23.3 % of the way from 20 Hz to 20 kHz in the logarithm, 8 kHz is 86.7 %.
    expect(near(lowBright, 0.233, 0.03) && near(highBright, 0.867, 0.03),
           "brightness is where the power sits: " + std::to_string(lowBright) + ", " +
               std::to_string(highBright));
    expect(an::tableNoisiness(high.row(25), an::TableChannel::Mix) < 0.01,
           "a tone is not noise");
    // Noise from a fixed generator: every band has power, so it reads far above a tone.
    std::vector<float> noise(kRate);
    std::uint32_t state = 0x1234567U;
    for (auto& sample : noise) {
        state = state * 1664525U + 1013904223U;
        sample = static_cast<float>(static_cast<double>(state >> 8) / 8388608.0 - 1.0) * 0.5F;
    }
    const auto noisy = build(noise);
    expect(an::tableNoisiness(noisy.row(25), an::TableChannel::Mix) > 0.3,
           "noise is noisy: " +
               std::to_string(an::tableNoisiness(noisy.row(25), an::TableChannel::Mix)));
    const auto left = build(sine(kRate / 4, 1000.0, 0.5, 0.0));
    const auto right = build(sine(kRate / 4, 1000.0, 0.0, 0.5));
    const auto centre = build(sine(kRate / 4, 1000.0, 0.5, 0.5));
    const auto opposite = build(sine(kRate / 4, 1000.0, 0.5, -0.5));
    expect(near(an::tablePan(left.row(12)), -1.0, 1e-6) &&
               near(an::tablePan(right.row(12)), 1.0, 1e-6) &&
               near(an::tablePan(centre.row(12)), 0.0, 1e-6),
           "pan runs from -1 on the left to 1 on the right");
    expect(near(an::tableWidth(centre.row(12)), 0.0, 1e-6) &&
               near(an::tableWidth(opposite.row(12)), 1.0, 1e-6) &&
               near(an::tableWidth(left.row(12)), 0.5, 1e-6),
           "width is the share of the side signal");
    const auto silent = build(std::vector<float>(kRate / 4, 0.0F));
    expect(an::tableBrightness(silent.row(5), an::TableChannel::Mix) == 0.0 &&
               an::tableNoisiness(silent.row(5), an::TableChannel::Mix) == 0.0 &&
               an::tablePan(silent.row(5)) == 0.0 && an::tableWidth(silent.row(5)) == 0.0,
           "silence reads 0 for every feature");
}

// The flux is large where the sound starts and small while it holds.
void flux() {
    const auto table = build(sine(kRate / 2, 1000.0, 0.5, 0.5, kRate / 4));
    std::uint64_t loudest = 0;
    for (std::uint64_t row = 0; row < table.rows; ++row)
        if (table.row(row)[an::kTableFlux] > table.row(loudest)[an::kTableFlux])
            loudest = row;
    // The sound starts at row 25; its window reaches it from row 23.
    expect(loudest >= 23 && loudest <= 26, "the flux peaks where the sound starts: row " +
                                               std::to_string(loudest));
    expect(table.row(10)[an::kTableFlux] == 0.0F, "silence has no flux");
    expect(table.row(40)[an::kTableFlux] < table.row(loudest)[an::kTableFlux] * 0.02F,
           "a held sound has almost none");
}

void envelopes() {
    // Fifty rows at 0 dB, then fifty at -20 dB.
    std::vector<double> power(100, 1.0);
    std::fill(power.begin() + 50, power.end(), 0.01);
    an::TableEnvelopeSettings settings;
    settings.rangeDb = 40.0;
    settings.normalize = an::TableNormalize::Peak;
    auto curve = an::tableEnvelope(power, settings);
    expect(curve.size() == 100 && near(curve[10], 1.0, 1e-6) && near(curve[80], 0.5, 1e-6),
           "against the peak, 20 dB down of 40 reads a half");
    settings.normalize = an::TableNormalize::Percentile95;
    curve = an::tableEnvelope(power, settings);
    expect(near(curve[10], 1.0, 1e-6) && near(curve[80], 0.5, 1e-6),
           "the 95th percentile of this sound is its loud half");
    settings.normalize = an::TableNormalize::Manual;
    settings.floorDb = -40.0;
    settings.ceilingDb = -10.0;
    curve = an::tableEnvelope(power, settings);
    expect(near(curve[10], 1.0, 1e-6) && near(curve[80], 20.0 / 30.0, 1e-6),
           "a manual range maps its floor to 0 and its ceiling to 1");
    settings.normalize = an::TableNormalize::Rolling;
    settings.rollingSeconds = 0.1;
    curve = an::tableEnvelope(power, settings);
    // The window is the ten rows that end with the row, so row 59 no longer sees row 49.
    expect(near(curve[10], 1.0, 1e-6) && near(curve[55], 0.5, 1e-6) && near(curve[58], 0.5, 1e-6) &&
               near(curve[59], 1.0, 1e-6),
           "a rolling reference forgets the loud half after its window");

    // A percentile is not the peak: one loud row in a hundred does not set the reference.
    std::vector<double> spike(100, 0.01);
    spike[40] = 1.0;
    settings = {};
    settings.rangeDb = 40.0;
    curve = an::tableEnvelope(spike, settings);
    expect(near(curve[10], 1.0, 1e-6) && near(curve[40], 1.0, 1e-6),
           "the 95th percentile ignores a single loud row");
    settings.normalize = an::TableNormalize::Peak;
    curve = an::tableEnvelope(spike, settings);
    expect(near(curve[10], 0.5, 1e-6) && near(curve[40], 1.0, 1e-6), "the peak does not");

    // Silence reads 0 in every mode, not 1.
    const std::vector<double> silence(20, 0.0);
    for (const auto mode : {an::TableNormalize::Percentile95, an::TableNormalize::Peak,
                            an::TableNormalize::Rolling, an::TableNormalize::Manual}) {
        settings = {};
        settings.normalize = mode;
        curve = an::tableEnvelope(silence, settings);
        expect(std::all_of(curve.begin(), curve.end(), [](const float v) { return v == 0.0F; }),
               "silence reads 0");
    }

    // Attack and release: one row is 10 ms, so after a tenth of a second at a time constant of a
    // tenth of a second the value has covered 1 - 1/e of the step.
    std::vector<double> step(60, 0.0);
    std::fill(step.begin() + 10, step.begin() + 30, 1.0);
    settings = {};
    settings.normalize = an::TableNormalize::Manual;
    settings.floorDb = -40.0;
    settings.ceilingDb = 0.0;
    settings.attackSeconds = 0.1;
    settings.releaseSeconds = 0.2;
    curve = an::tableEnvelope(step, settings);
    const double rise = 1.0 - std::exp(-1.0);
    expect(curve[9] == 0.0F && near(curve[19], rise, 1e-5),
           "the attack covers 1 - 1/e in its time: " + std::to_string(curve[19]));
    const double top = 1.0 - std::exp(-2.0);
    expect(near(curve[29], top, 1e-5) && near(curve[49], top * std::exp(-1.0), 1e-5),
           "the release falls to 1/e in its time: " + std::to_string(curve[49]));
    settings.attackSeconds = 0.0;
    settings.releaseSeconds = 0.0;
    curve = an::tableEnvelope(step, settings);
    expect(curve[10] == 1.0F && curve[30] == 0.0F, "no attack and no release follow at once");
}

void curveReading() {
    const std::vector<float> curve{0.0F, 1.0F, 0.5F};
    expect(an::tableCurveAt(curve, 0.0) == 0.0 && near(an::tableCurveAt(curve, 0.005), 0.5, 1e-9) &&
               near(an::tableCurveAt(curve, 0.015), 0.75, 1e-9) &&
               near(an::tableCurveAt(curve, 0.02), 0.5, 1e-9),
           "a curve is read in a straight line between rows");
    expect(an::tableCurveAt(curve, -0.01) == 0.0 && an::tableCurveAt(curve, 0.5) == 0.0,
           "outside the sound a curve reads 0");
}

void cancellationAndFailure() {
    const auto samples = sine(kRate / 10, 1000.0, 0.5, 0.5);
    std::uint64_t taken = 0;
    const auto reader = [&](std::int64_t, const std::span<float> output) {
        std::fill(output.begin(), output.end(), 0.0F);
        return true;
    };
    const auto cancelled = an::buildTable(
        samples.size() / 2, reader,
        [&](std::uint64_t, std::span<const float>) {
            ++taken;
            return true;
        },
        [&] { return taken == 3; });
    expect(!cancelled && cancelled.error()->code == AudioErrorCode::Cancelled && taken == 3,
           "a cancelled build stops at the next row");
    const auto refused = an::buildTable(
        samples.size() / 2, [](std::int64_t, std::span<float>) { return false; },
        [](std::uint64_t, std::span<const float>) { return true; });
    expect(!refused && refused.error()->code == AudioErrorCode::FileReadFailed,
           "a read that fails fails the build");
}
} // namespace

int main() {
    rowsAndBands();
    channels();
    lowBands();
    pitchClasses();
    flux();
    hits();
    beats();
    features();
    envelopes();
    curveReading();
    cancellationAndFailure();
    if (failures != 0)
        std::cerr << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
