#include <bloom/core/camera_shake.hpp>

#include <bloom/core/value_primitives.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace bloom::core::camera_shake {
namespace {

constexpr std::size_t kChannels = 3;
constexpr std::uint64_t kStreamsPerChannel = 8;
constexpr std::uint64_t kWeaveStream = 7;
constexpr double kOctaveRate = 3.0;
constexpr double kImpactDecay = 6.0;
constexpr double kImpactCycles = 2.5;
// The largest value of exp(-6 s) sin(2 pi 2.5 s): the ring's first peak, so a hit of amount 1 moves
// the image by about that amount.
constexpr double kImpactPeak = 0.5889;
constexpr double kWeaveJitter = 0.75;
constexpr double kWeaveDrift = 0.25;
constexpr int kSmoothingTaps = 9;
// The resampler blends a pixel with its neighbours, so the image must reach half a pixel past the
// frame's edge pixels in the resolution being drawn. Four full-resolution pixels cover a preview at
// an eighth of the resolution.
constexpr double kEdgeMargin = 4.0;

// A value in [-1, 1) of the lattice point `index` of `stream` (a channel and an octave).
double lattice(const std::int64_t seed, const std::uint64_t stream,
               const std::int64_t index) noexcept {
    const auto key = primitives::hashWord(seed) ^ (stream * 0x9E37'79B9'7F4A'7C15ULL);
    const auto mixed = key + static_cast<std::uint64_t>(index) * 0xD6E8'FEB8'6659'FD93ULL;
    return primitives::hashUnitValue(static_cast<std::int64_t>(mixed)) * 2.0 - 1.0;
}

std::int64_t wrap(const std::int64_t index, const std::int64_t period) noexcept {
    if (period <= 0)
        return index;
    const auto r = index % period;
    return r < 0 ? r + period : r;
}

double smootherstep(const double t) noexcept { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// One octave of value noise at lattice position `u`; `period` lattice points when it loops, else 0.
double valueNoise(const std::int64_t seed, const std::uint64_t stream, const double u,
                  const std::int64_t period) noexcept {
    const double base = std::floor(u);
    const double f = u - base;
    const auto i = static_cast<std::int64_t>(base);
    const double a = lattice(seed, stream, wrap(i, period));
    const double b = lattice(seed, stream, wrap(i + 1, period));
    return a + (b - a) * smootherstep(f);
}

// Fractal value noise in [-1, 1].
double fractal(const Settings& settings, const std::uint64_t stream, const double u,
               const std::int64_t period) noexcept {
    const int octaves = std::clamp(settings.octaves, 1, 4);
    const double gain = std::clamp(settings.roughness, 0.0, 1.0);
    double sum = 0.0;
    double weights = 0.0;
    double weight = 1.0;
    double rate = 1.0;
    std::int64_t octavePeriod = period;
    for (int octave = 0; octave < octaves; ++octave) {
        sum += weight * valueNoise(settings.seed, stream + static_cast<std::uint64_t>(octave),
                                   u * rate, octavePeriod);
        weights += weight;
        weight *= gain;
        rate *= kOctaveRate;
        octavePeriod *= 3;
    }
    return sum / weights;
}

double impact(const std::int64_t seed, const std::uint64_t stream, const double u,
              const std::int64_t period) noexcept {
    const double base = std::floor(u);
    const double f = u - base;
    const auto k = static_cast<std::int64_t>(base);
    double sum = 0.0;
    // A hit's tail is below 1e-5 after two intervals.
    for (std::int64_t j = 0; j < 3; ++j) {
        const double s = f + static_cast<double>(j);
        sum += lattice(seed, stream, wrap(k - j, period)) * std::exp(-kImpactDecay * s) *
               std::sin(2.0 * std::numbers::pi * kImpactCycles * s);
    }
    return std::clamp(sum / kImpactPeak, -1.0, 1.0);
}

Sample point(const Settings& settings, const double seconds, const double frameRate) noexcept {
    const double frequency = settings.frequency > 0.0 ? settings.frequency : 1.0;
    const bool loops = settings.loopSeconds > 0.0;
    std::int64_t period = 0;
    double u = seconds * frequency;
    if (loops) {
        period = std::max<std::int64_t>(1, std::llround(settings.loopSeconds * frequency));
        u = seconds / settings.loopSeconds * static_cast<double>(period);
    }
    // Snapped to 1e-9 so the loop's end lands exactly on its start.
    u = std::round(u * 1.0e9) * 1.0e-9;
    std::array<double, kChannels> unit{};
    for (std::size_t channel = 0; channel < kChannels; ++channel) {
        const std::uint64_t stream = channel * kStreamsPerChannel;
        switch (settings.mode) {
        case Mode::Handheld:
            unit[channel] = fractal(settings, stream, u, period);
            break;
        case Mode::Impact:
            unit[channel] = impact(settings.seed, stream, u, period);
            break;
        case Mode::GateWeave: {
            const double rate = frameRate > 0.0 ? frameRate : 24.0;
            auto frame = static_cast<std::int64_t>(std::floor(seconds * rate + 1.0e-6));
            if (loops)
                frame = wrap(frame,
                             std::max<std::int64_t>(1, std::llround(settings.loopSeconds * rate)));
            unit[channel] = kWeaveJitter * lattice(settings.seed, stream + kWeaveStream, frame) +
                            kWeaveDrift * fractal(settings, stream, u, period);
            break;
        }
        }
    }
    return {unit[0], unit[1], unit[2]};
}

} // namespace

Sample evaluate(const Settings& settings, const double seconds, const double frameRate) noexcept {
    const double smoothing = std::clamp(settings.smoothing, 0.0, 1.0);
    if (smoothing == 0.0)
        return point(settings, seconds, frameRate);
    const double frequency = settings.frequency > 0.0 ? settings.frequency : 1.0;
    const double window = smoothing / frequency;
    Sample sum;
    for (int tap = 0; tap < kSmoothingTaps; ++tap) {
        const double offset =
            window * (static_cast<double>(tap) / (kSmoothingTaps - 1) - 0.5);
        const auto s = point(settings, seconds + offset, frameRate);
        sum.x += s.x;
        sum.y += s.y;
        sum.roll += s.roll;
    }
    return {sum.x / kSmoothingTaps, sum.y / kSmoothingTaps, sum.roll / kSmoothingTaps};
}

double fillScale(const double amplitudeX, const double amplitudeY, const double rollDegrees,
                 const double width, const double height) noexcept {
    if (!(width > 0.0) || !(height > 0.0))
        return 1.0;
    const double ax = std::abs(amplitudeX);
    const double ay = std::abs(amplitudeY);
    if (ax == 0.0 && ay == 0.0 && rollDegrees == 0.0)
        return 1.0;
    const double limit = std::min(std::abs(rollDegrees), 90.0) * std::numbers::pi / 180.0;
    // The frame's corners, kEdgeMargin pixels out, mapped back into the image's own frame at the
    // worst translation (a corner of the amplitude box) and the worst roll (swept across its range).
    const double cornerX = width * 0.5 + kEdgeMargin;
    const double cornerY = height * 0.5 + kEdgeMargin;
    constexpr int kSteps = 256;
    double need = 1.0;
    for (int step = 0; step <= kSteps; ++step) {
        const double angle = limit * (2.0 * static_cast<double>(step) / kSteps - 1.0);
        const double c = std::cos(angle);
        const double s = std::sin(angle);
        for (const double sx : {-1.0, 1.0})
            for (const double sy : {-1.0, 1.0})
                for (const double qx : {-1.0, 1.0})
                    for (const double qy : {-1.0, 1.0}) {
                        const double dx = qx * cornerX - sx * ax;
                        const double dy = qy * cornerY - sy * ay;
                        const double ux = dx * c + dy * s;
                        const double uy = -dx * s + dy * c;
                        need = std::max({need, std::abs(ux) / (width * 0.5),
                                         std::abs(uy) / (height * 0.5)});
                    }
    }
    return need * (1.0 + 1.0e-4);
}

} // namespace bloom::core::camera_shake
