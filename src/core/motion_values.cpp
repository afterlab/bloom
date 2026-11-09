#include <bloom/core/motion_values.hpp>

#include <bloom/core/value_primitives.hpp>

#include <algorithm>
#include <cmath>

namespace bloom::core::motion_values {
namespace {

constexpr double kPi = 3.14159265358979323846;

double finiteOr(const double value, const double fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

double smootherstep(const double t) noexcept { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// A value in [-1, 1] of lattice `index` in `octave`.
double lattice(const std::int64_t seed, const std::int64_t octave, const std::int64_t index) noexcept {
    const auto key = primitives::hashWord(seed) ^
                     (static_cast<std::uint64_t>(octave + 1) * 0x9E37'79B9'7F4A'7C15ULL);
    const auto mixed = key + static_cast<std::uint64_t>(index) * 0xD6E8'FEB8'6659'FD93ULL;
    return primitives::hashUnitValue(static_cast<std::int64_t>(mixed)) * 2.0 - 1.0;
}

} // namespace

double oscillate(const OscillatorSettings& raw, const double seconds) noexcept {
    const OscillatorSettings defaults;
    const double frequency = std::clamp(finiteOr(raw.frequency, defaults.frequency),
                                        -kMaximumFrequency, kMaximumFrequency);
    const double amplitude = finiteOr(raw.amplitude, defaults.amplitude);
    const double offset = finiteOr(raw.offset, defaults.offset);
    const double phase = finiteOr(raw.phase, defaults.phase);
    const double t = finiteOr(seconds, 0.0);
    // The cycle position in [0, 1): the fractional part keeps a large time precise.
    double cycles = frequency * t + phase / 360.0;
    cycles -= std::floor(cycles);
    double wave = 0.0;
    switch (raw.wave) {
    case Wave::Sine:
        wave = std::sin(2.0 * kPi * cycles);
        break;
    case Wave::Triangle:
        // 0 at the start, up to 1 at a quarter, 0 at a half, -1 at three quarters.
        wave = cycles < 0.25 ? 4.0 * cycles : (cycles < 0.75 ? 2.0 - 4.0 * cycles : 4.0 * cycles - 4.0);
        break;
    case Wave::Square:
        wave = cycles < 0.5 ? 1.0 : -1.0;
        break;
    case Wave::Sawtooth:
        wave = 2.0 * cycles - 1.0;
        break;
    }
    return offset + amplitude * wave;
}

double wiggle(const WiggleSettings& raw, const double seconds) noexcept {
    const WiggleSettings defaults;
    const double frequency = std::clamp(finiteOr(raw.frequency, defaults.frequency),
                                        -kMaximumFrequency, kMaximumFrequency);
    const double amplitude = finiteOr(raw.amplitude, defaults.amplitude);
    const double offset = finiteOr(raw.offset, defaults.offset);
    const auto octaves = std::clamp<std::int64_t>(raw.octaves, 1, kMaximumOctaves);
    const double roughness = std::clamp(finiteOr(raw.roughness, defaults.roughness), 0.0, 1.0);
    const double t = finiteOr(seconds, 0.0);
    double sum = 0.0;
    double weights = 0.0;
    double weight = 1.0;
    double rate = frequency;
    for (std::int64_t octave = 0; octave < octaves; ++octave) {
        const double u = t * rate;
        const double base = std::floor(u);
        const auto index = static_cast<std::int64_t>(base);
        const double a = lattice(raw.seed, octave, index);
        const double b = lattice(raw.seed, octave, index + 1);
        sum += weight * (a + (b - a) * smootherstep(u - base));
        weights += weight;
        weight *= roughness;
        rate *= 2.0;
    }
    return offset + amplitude * (weights > 0.0 ? sum / weights : 0.0);
}

LoopResult loop(const LoopSettings& raw, const double seconds) noexcept {
    const LoopSettings defaults;
    const double start = finiteOr(raw.start, defaults.start);
    const double end = finiteOr(raw.end, defaults.end);
    const double duration = std::max(finiteOr(raw.duration, defaults.duration), kMinimumDuration);
    const double t = finiteOr(seconds, 0.0);
    const double position = t / duration;
    const double whole = std::floor(position);
    const double fraction = position - whole;
    LoopResult result;
    result.cycle = static_cast<std::int64_t>(std::clamp(whole, -9.0e15, 9.0e15));
    double along = fraction;
    if (raw.mode == LoopMode::PingPong)
        // Forward on even cycles, back on odd ones; the cycle count below is the same.
        along = (result.cycle % 2 == 0) ? fraction : 1.0 - fraction;
    result.value = start + (end - start) * along;
    return result;
}

} // namespace bloom::core::motion_values
