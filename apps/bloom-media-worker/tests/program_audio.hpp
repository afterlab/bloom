// The synthetic program the AAC verification tests share (#650): thirty seconds of music-like
// stereo with transients and isolated clicks, peaking at -0.3 dBFS. Deterministic, so every
// platform and every test builds the same samples.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numbers>
#include <vector>

namespace bloom::media::testing {

inline constexpr unsigned kRate = 48000;
inline constexpr unsigned kChannels = 2;
inline constexpr std::size_t kFrames = 30U * kRate;
// The opening the encode test exports and the sync test reads back: three seconds, 72 video frames
// at 24 fps. It holds two of the clicks and three whole one-second levels.
inline constexpr std::size_t kExportFrames = 3U * kRate;
inline constexpr std::size_t kExportVideoFrames = 72;
// -0.3 dBFS.
inline const float kPeak = static_cast<float>(std::pow(10.0, -0.3 / 20.0));
// The isolated clicks: one frame at full scale each, away from the drum hits.
inline constexpr std::size_t kClicks = 20;
inline std::size_t clickFrame(const std::size_t click) {
    return static_cast<std::size_t>((1.37 + 1.4 * static_cast<double>(click)) * kRate);
}

// A deterministic generator, so the streams are the same on every platform.
struct Random {
    std::uint64_t state;
    double next() { // uniform in [-1, 1)
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(static_cast<std::int64_t>(state >> 11) -
                                   (std::int64_t{1} << 52)) /
               static_cast<double>(std::int64_t{1} << 52);
    }
};

// Thirty seconds of music-like stereo: a chord whose notes come and go, a bass line, drum hits
// (a decaying noise burst with a sharp attack) and isolated full-scale clicks, peaking at
// -0.3 dBFS. Interleaved.
// `pitch` transposes the notes and the bass and changes the tempo, for a different piece.
inline std::vector<float> programAudio(const std::uint64_t seed = 7, const double pitch = 1.0) {
    std::vector<double> mix(kFrames * kChannels, 0.0);
    Random random{seed};
    const double notes[] = {220.0, 277.18, 329.63, 440.0, 554.37, 1318.5, 2637.0};
    for (std::size_t i = 0; i < kFrames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        double left = 0, right = 0;
        for (std::size_t note = 0; note < std::size(notes); ++note) {
            const auto n = static_cast<double>(note);
            const double gate =
                0.5 + 0.5 * std::sin(2 * std::numbers::pi * (0.21 + 0.07 * n) * t + n);
            const double tone =
                gate * std::sin(2 * std::numbers::pi * notes[note] * pitch * t + 0.3 * n);
            left += tone * (0.6 + 0.4 * std::cos(n));
            right += tone * (0.6 - 0.4 * std::cos(n));
        }
        const double bass = std::sin(2 * std::numbers::pi * 55.0 * pitch * t) *
                            (std::fmod(t, 0.5 / pitch) < 0.35 / pitch ? 1.0 : 0.0);
        const double beat = std::fmod(t, 0.5 / pitch);
        const double drum = std::exp(-beat * 40.0) * random.next();
        mix[i * 2] = 0.09 * left + 0.25 * bass + 0.6 * drum;
        mix[i * 2 + 1] = 0.09 * right + 0.25 * bass + 0.55 * drum;
    }
    // Clicks: one sample at full scale, alternating sign, away from the drum hits.
    for (std::size_t click = 0; click < kClicks; ++click) {
        const std::size_t at = clickFrame(click);
        if (at < kFrames) {
            mix[at * 2] = click % 2 ? -4.0 : 4.0;
            mix[at * 2 + 1] = click % 2 ? 4.0 : -4.0;
        }
    }
    double peak = 0;
    for (const double sample : mix)
        peak = std::max(peak, std::abs(sample));
    std::vector<float> out(mix.size());
    for (std::size_t i = 0; i < mix.size(); ++i)
        out[i] = static_cast<float>(mix[i] / peak * static_cast<double>(kPeak));
    return out;
}

} // namespace bloom::media::testing
