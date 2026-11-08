// #570 export loudness against reference signals: the EBU Tech 3341 style sine references for the
// meter, an intersample peak for the true-peak estimate, and the limiter's ceiling and alignment.
#include <bloom/media/audio/loudness.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace bloom::media::audio::loudness;

constexpr double kPi = 3.14159265358979323846;
constexpr std::uint32_t kRate = 48000;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

double fromDb(const double db) { return std::pow(10.0, db / 20.0); }

// Interleaved stereo: the same sine on both channels.
std::vector<float> stereoSine(const double frequency, const double peakDbfs, const double seconds,
                              const double phase = 0.0) {
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> samples(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto value = static_cast<float>(
            fromDb(peakDbfs) *
            std::sin(2.0 * kPi * frequency * static_cast<double>(frame) / kRate + phase));
        samples[frame * 2] = value;
        samples[frame * 2 + 1] = value;
    }
    return samples;
}

Measurement measure(const std::vector<float>& samples, const std::size_t chunk = 4800 * 2) {
    Meter meter(kRate, 2);
    for (std::size_t at = 0; at < samples.size(); at += chunk)
        expect(meter.add(std::span<const float>(samples).subspan(
                   at, std::min(chunk, samples.size() - at))),
               "the meter takes a block");
    return meter.measurement();
}

void referenceSineLoudness() {
    // A 997 Hz stereo sine reads its own level in LUFS (the K weighting is unity there and the two
    // channels add 3 dB to the -3 dB of a sine's mean square).
    for (const double level : {-20.0, -23.0, -30.0}) {
        const auto result = measure(stereoSine(997.0, level, 10.0));
        expect(result.loudnessMeasured && std::abs(result.integratedLufs - level) < 0.1,
               "a " + std::to_string(level) + " dBFS stereo sine measures " +
                   std::to_string(level) + " LUFS (got " + std::to_string(result.integratedLufs) +
                   ")");
        expect(std::abs(result.truePeakDbtp - level) < 0.1,
               "its true peak is its level (got " + std::to_string(result.truePeakDbtp) + ")");
        expect(result.frames == 10 * kRate, "the meter counted every frame");
    }
    // Block size does not change the measurement.
    const auto signal = stereoSine(997.0, -26.0, 5.0);
    expect(measure(signal, 2 * 100).integratedLufs == measure(signal, 2 * 4800).integratedLufs,
           "the measurement does not depend on how the audio was cut");
}

void silenceHasNoLoudness() {
    const std::vector<float> silence(2 * kRate * 2, 0.0F);
    const auto result = measure(silence);
    expect(!result.loudnessMeasured && !std::isfinite(result.integratedLufs),
           "silence has no integrated loudness");
    expect(gainToTarget(result, Target{}) == 0.0, "silence is not given a gain");
    // A tone below the -70 LUFS absolute gate is silence for the measurement too.
    expect(!measure(stereoSine(997.0, -90.0, 4.0)).loudnessMeasured,
           "audio under the absolute gate has no integrated loudness");
}

void truePeakSeesInterSamplePeaks() {
    // A quarter-rate sine with a 45 degree phase has samples of 0.707 of its peak: the sample
    // peak is 3 dB below the true peak.
    const auto result = measure(stereoSine(kRate / 4.0, -6.0, 2.0, kPi / 4.0));
    expect(std::abs(result.truePeakDbtp - (-6.0)) < 0.2,
           "the true peak finds the peak between samples (got " +
               std::to_string(result.truePeakDbtp) + ")");
}

void gainReachesTheTarget() {
    const auto result = measure(stereoSine(997.0, -22.0, 10.0));
    const Target target{-16.0, -1.0};
    expect(std::abs(gainToTarget(result, target) - 6.0) < 0.1, "the gain to -16 LUFS from -22");
}

std::vector<float> applyLimiter(const std::vector<float>& input, const double gainDb,
                                const double ceiling, const std::size_t chunk) {
    Limiter limiter(kRate, 2, gainDb, ceiling);
    std::vector<float> out;
    for (std::size_t at = 0; at < input.size(); at += chunk)
        limiter.process(std::span<const float>(input).subspan(
                            at, std::min(chunk, input.size() - at)),
                        out);
    limiter.flush(out);
    return out;
}

void limiterKeepsTheCeilingAndTheAlignment() {
    // A quiet 220 Hz tone with a few loud clicks: after +10 dB the clicks are far over -1 dBTP.
    auto input = stereoSine(220.0, -24.0, 4.0);
    for (const std::size_t at : {20000U, 21000U, 90000U, 150000U})
        for (std::size_t offset = 0; offset < 6; ++offset) {
            const float click = static_cast<float>(0.9 * std::sin(kPi * static_cast<double>(offset + 1) / 7.0));
            input[(at + offset) * 2] = click;
            input[(at + offset) * 2 + 1] = -click;
        }
    const auto out = applyLimiter(input, 10.0, -1.0, 4800 * 2);
    expect(out.size() == input.size(), "the limiter keeps the stream's length");
    const auto measured = measure(out);
    expect(measured.truePeakDbtp <= -1.0 + 0.02,
           "the limited true peak is under the ceiling (got " +
               std::to_string(measured.truePeakDbtp) + " dBTP)");
    expect(measured.truePeakDbtp > -1.5, "the limiter does not over-attenuate the peaks (got " +
                                              std::to_string(measured.truePeakDbtp) + ")");
    // Far from the clicks the tone is the input at +10 dB, sample for sample (aligned, no ripple).
    const double gain = fromDb(10.0);
    double worst = 0.0;
    for (std::size_t frame = 40000; frame < 80000; ++frame)
        worst = std::max(worst, std::abs(static_cast<double>(out[frame * 2]) -
                                         static_cast<double>(input[frame * 2]) * gain));
    expect(worst < 1e-5, "outside the peaks the stream is the input times the gain, in place (" +
                             std::to_string(worst) + ")");
    // The same stream cut differently is the same samples.
    expect(applyLimiter(input, 10.0, -1.0, 2 * 37) == out,
           "the limiter gives the same samples however the audio is cut");
}

void limiterIsTransparentUnderTheCeiling() {
    const auto input = stereoSine(440.0, -30.0, 2.0);
    const auto out = applyLimiter(input, 6.0, -1.0, 2 * 1000);
    const double gain = fromDb(6.0);
    bool ok = out.size() == input.size();
    double worst = 0.0;
    for (std::size_t index = 0; ok && index < input.size(); ++index)
        worst = std::max(worst, std::abs(static_cast<double>(out[index]) -
                                         static_cast<double>(input[index]) * gain));
    expect(ok && worst < 1e-6, "audio under the ceiling is only turned up (" +
                                   std::to_string(worst) + ")");
}

void normalizedProgrammeReachesTheTarget() {
    // Measure, take the gain, limit, measure again: the reference flow of the export.
    const auto input = stereoSine(997.0, -26.0, 12.0);
    const Target target{-16.0, -1.0};
    const auto before = measure(input);
    const auto out = applyLimiter(input, gainToTarget(before, target), target.truePeakDbtp, 9600);
    const auto after = measure(out);
    expect(std::abs(after.integratedLufs - (-16.0)) < 0.1,
           "the normalized programme measures the target (got " +
               std::to_string(after.integratedLufs) + ")");
    expect(after.truePeakDbtp <= -1.0, "its true peak is under the ceiling");
}

void invalidMeters() {
    Meter bad(100, 2);
    expect(!bad.valid() && !bad.add(std::vector<float>(4, 0.0F)), "a meter refuses a bad rate");
    Meter odd(kRate, 2);
    expect(odd.valid() && !odd.add(std::vector<float>(3, 0.0F)), "a meter refuses a partial frame");
}

} // namespace

int main() {
    referenceSineLoudness();
    silenceHasNoLoudness();
    truePeakSeesInterSamplePeaks();
    gainReachesTheTarget();
    limiterKeepsTheCeilingAndTheAlignment();
    limiterIsTransparentUnderTheCeiling();
    normalizedProgrammeReachesTheTarget();
    invalidMeters();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
