// Tolerance profile bloom.aac-f32.v2 on synthetic streams (#650). The "decoded" streams here are
// models of what a perceptual codec does to a correct encode (shaped noise under the signal, the
// top band dropped, a transient's error spread before it) and of the defects the verifier must
// refuse. The first group are streams the v1 limits (every sample within 0.25, stream RMS error
// within 0.05) judged wrongly: v1 is evaluated beside v2 so each blind spot is on record.

#include "lossy_audio_compare.hpp"
#include "program_audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

namespace {
using bloom::media::ffmpeg::LossyAudioCompare;
using bloom::media::ffmpeg::LossyAudioMeasure;

using namespace bloom::media::testing;

int failures = 0;
void check(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// What a correct perceptual encode returns, modeled: the band above 16 kHz removed (a
// symmetric windowed-sinc low-pass, so no delay), noise added in proportion to the local level
// (20 dB under it), and a share of each sample's error spread over the 1024 frames before it
// (pre-echo of a long transform block).
std::vector<float> coded(const std::vector<float>& source) {
    constexpr std::size_t kTaps = 63;
    std::vector<double> taps(kTaps);
    double sum = 0;
    for (std::size_t tap = 0; tap < kTaps; ++tap) {
        const auto n = static_cast<double>(tap);
        const double x = n - (static_cast<double>(kTaps) - 1) / 2.0;
        const double sinc = x == 0 ? 1.0
                                   : std::sin(2 * std::numbers::pi * 16000.0 / kRate * x) /
                                         (2 * std::numbers::pi * 16000.0 / kRate * x);
        taps[tap] = sinc * (0.5 - 0.5 * std::cos(2 * std::numbers::pi * n /
                                                 (static_cast<double>(kTaps) - 1)));
        sum += taps[tap];
    }
    std::vector<float> out(source.size());
    Random random{99};
    for (unsigned c = 0; c < kChannels; ++c) {
        std::vector<double> filtered(kFrames);
        for (std::size_t i = 0; i < kFrames; ++i) {
            double value = 0;
            for (std::size_t n = 0; n < kTaps; ++n) {
                const auto at = static_cast<std::int64_t>(i) + static_cast<std::int64_t>(n) -
                                static_cast<std::int64_t>((kTaps - 1) / 2);
                if (at >= 0 && at < static_cast<std::int64_t>(kFrames))
                    value += taps[n] * static_cast<double>(
                                           source[static_cast<std::size_t>(at) * kChannels + c]);
            }
            filtered[i] = value / sum;
        }
        // Block energy of the next 1024 frames drives noise in this block: pre-echo.
        for (std::size_t block = 0; block * 1024 < kFrames; ++block) {
            const std::size_t begin = block * 1024, end = std::min(kFrames, begin + 2048);
            double energy = 0;
            for (std::size_t i = begin; i < end; ++i)
                energy += filtered[i] * filtered[i];
            const double noise = std::sqrt(energy / static_cast<double>(end - begin)) * 0.1;
            for (std::size_t i = begin; i < std::min(kFrames, begin + 1024); ++i)
                out[i * kChannels + c] = static_cast<float>(filtered[i] + noise * random.next());
        }
    }
    return out;
}

std::vector<float> shifted(const std::vector<float>& stream, const std::size_t frames) {
    std::vector<float> out(stream.size(), 0.0F);
    std::copy(stream.begin(), stream.end() - static_cast<std::ptrdiff_t>(frames * kChannels),
              out.begin() + static_cast<std::ptrdiff_t>(frames * kChannels));
    return out;
}

std::vector<float> scaled(const std::vector<float>& stream, const double gain) {
    std::vector<float> out(stream.size());
    for (std::size_t i = 0; i < stream.size(); ++i)
        out[i] = static_cast<float>(static_cast<double>(stream[i]) * gain);
    return out;
}

LossyAudioMeasure v2(const std::vector<float>& reference, const std::vector<float>& decoded) {
    LossyAudioCompare compare(kChannels, kRate);
    // In blocks of an AAC frame, as the verifier feeds it.
    for (std::size_t at = 0; at < reference.size(); at += 1024U * kChannels) {
        const auto count = std::min<std::size_t>(1024U * kChannels, reference.size() - at);
        compare.push(std::span(reference).subspan(at, count), std::span(decoded).subspan(at, count));
    }
    return compare.finish();
}

// The v1 limits, as the verifier applied them before #650.
bool v1(const std::vector<float>& reference, const std::vector<float>& decoded) {
    double squared = 0;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        const double error = static_cast<double>(decoded[i]) - static_cast<double>(reference[i]);
        if (!std::isfinite(decoded[i]) || std::abs(error) > 0.25)
            return false;
        squared += error * error;
    }
    return squared / static_cast<double>(reference.size()) <= 0.0025;
}

} // namespace

int main() {
    const auto source = programAudio();
    const auto encoded = coded(source);
    float peak = 0;
    for (const float sample : source)
        peak = std::max(peak, std::abs(sample));
    check(std::abs(peak - kPeak) < 1e-6F, "the program peaks at -0.3 dBFS");

    // A correct encode of loud transients: v2 accepts it; v1 refused it (#650).
    const auto accepted = v2(source, encoded);
    check(accepted.failure.empty() && accepted.bandSnrMeasured && accepted.bandSnrDb > 6.0 &&
              accepted.frames == kFrames,
          "a correct encode of full-scale transients verifies: " + accepted.failure);
    check(!v1(source, encoded),
          "v1 refused that correct encode (the per-sample limit at a full-scale transient)");
    std::cout << "correct encode: band SNR " << accepted.bandSnrDb << " dB over "
              << accepted.windows << " windows\n";
    check(v2(source, source).failure.empty(), "an identical stream verifies");

    // Real defects at full level: both profiles refuse them.
    const auto late = v2(source, shifted(encoded, 1025));
    check(!late.failure.empty(), "a stream more than one AAC frame late is refused");
    check(!v2(source, shifted(encoded, 64)).failure.empty(),
          "a stream 64 frames late is refused");
    const auto other = coded(programAudio(1234, 1.19));
    check(!v2(source, other).failure.empty(), "another source is refused");
    check(!v2(source, std::vector<float>(source.size(), 0.0F)).failure.empty(),
          "silence against a loud source is refused");
    check(!v2(std::vector<float>(source.size(), 0.0F), encoded).failure.empty(),
          "sound against a silent source is refused");
    check(!v2(source, scaled(encoded, 0.5)).failure.empty(), "a stream 6 dB down is refused");
    check(!v2(source, scaled(encoded, 2.0)).failure.empty(), "a stream 6 dB up is refused");
    {
        auto swapped = encoded;
        for (std::size_t i = 0; i < kFrames; ++i)
            std::swap(swapped[i * 2], swapped[i * 2 + 1]);
        check(!v2(source, swapped).failure.empty(), "swapped channels are refused");
        auto dropout = encoded;
        std::fill(dropout.begin() + 10 * kRate * kChannels,
                  dropout.begin() + (10 * kRate + 4800) * kChannels, 0.0F);
        const auto measure = v2(source, dropout);
        check(!measure.failure.empty() &&
                  measure.failure.find("quieter") != std::string::npos,
              "a 100 ms dropout is refused as missing sound: " + measure.failure);
        auto hiss = encoded;
        Random random{5};
        for (std::size_t i = 20 * kRate * kChannels; i < 21 * kRate * kChannels; ++i)
            hiss[i] = static_cast<float>(static_cast<double>(hiss[i]) + 0.5 * random.next());
        check(!v2(source, hiss).failure.empty(), "a second of added noise is refused");
    }

    // The v1 blind spot: program quieter than its limits. At -30 dBFS v1 passed every one of
    // these defects; v2 refuses them all and still accepts the correct encode.
    const auto quiet = scaled(source, std::pow(10.0, -30.0 / 20.0));
    const auto quietEncoded = coded(quiet);
    check(v2(quiet, quietEncoded).failure.empty(),
          "a correct encode of quiet program verifies: " + v2(quiet, quietEncoded).failure);
    const std::vector<float> silence(quiet.size(), 0.0F);
    check(v1(quiet, silence) && !v2(quiet, silence).failure.empty(),
          "silence against quiet program: v1 passed it, v2 refuses it");
    const auto quietLate = shifted(quietEncoded, 1025);
    check(v1(quiet, quietLate) && !v2(quiet, quietLate).failure.empty(),
          "quiet program more than one AAC frame late: v1 passed it, v2 refuses it");
    const auto quietOther = scaled(other, std::pow(10.0, -30.0 / 20.0));
    check(v1(quiet, quietOther) && !v2(quiet, quietOther).failure.empty(),
          "another quiet source: v1 passed it, v2 refuses it");

    // Silence in, silence out is not a failure, and a stream shorter than one window is judged.
    check(v2(silence, silence).failure.empty(), "silence verifies against silence");
    {
        LossyAudioCompare brief(kChannels, kRate);
        brief.push(std::span(source).subspan(96000, 1000), std::span(encoded).subspan(96000, 1000));
        const auto measure = brief.finish();
        check(measure.failure.empty() && measure.frames == 500 && measure.windows == 1,
              "a stream shorter than one window verifies: " + measure.failure);
        LossyAudioCompare mismatched(kChannels, kRate);
        mismatched.push(std::span(source).subspan(0, 1000), std::span(encoded).subspan(0, 998));
        check(!mismatched.finish().failure.empty(), "mismatched block sizes are refused");
        auto broken = encoded;
        broken[4242] = std::nanf("");
        check(!v2(source, broken).failure.empty(), "a non-finite decoded sample is refused");
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
