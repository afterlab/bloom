#include <bloom/core/light_flicker.hpp>

#include <bloom/core/value_primitives.hpp>

#include <algorithm>
#include <cmath>

namespace bloom::core::light_flicker {
namespace {

constexpr std::uint64_t kFlickerStream = 1;
constexpr std::uint64_t kDropoutPresence = 2;
constexpr std::uint64_t kDropoutStart = 3;
constexpr std::uint64_t kDropoutSize = 4;

// A value in [0, 1) of `index` in `stream`.
double unit(const std::int64_t seed, const std::uint64_t stream, const std::int64_t index) noexcept {
    const auto key = primitives::hashWord(seed) ^ (stream * 0x9E37'79B9'7F4A'7C15ULL);
    const auto mixed = key + static_cast<std::uint64_t>(index) * 0xD6E8'FEB8'6659'FD93ULL;
    return primitives::hashUnitValue(static_cast<std::int64_t>(mixed));
}

double smootherstep(const double t) noexcept { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

double finiteOr(const double value, const double fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

} // namespace

double evaluate(const Settings& raw, const double seconds) noexcept {
    const Settings defaults;
    const double rate = std::clamp(finiteOr(raw.rate, defaults.rate), kMinimumRate, kMaximumRate);
    const double depth = std::clamp(finiteOr(raw.depth, defaults.depth), 0.0, 1.0);
    const double dropoutRate =
        std::clamp(finiteOr(raw.dropoutRate, defaults.dropoutRate), 0.0, kMaximumDropoutRate);
    const double length = std::clamp(finiteOr(raw.dropoutLength, defaults.dropoutLength),
                                     kMinimumDropoutLength, kMaximumDropoutLength);
    const double floorLevel = std::clamp(finiteOr(raw.dropoutLevel, defaults.dropoutLevel), 0.0, 1.0);
    const double softness = std::clamp(finiteOr(raw.softness, defaults.softness), 0.0, 1.0);
    const double t = finiteOr(seconds, 0.0);

    // Flicker: value noise in [0, 1].
    const double u = t * rate;
    const double base = std::floor(u);
    const auto index = static_cast<std::int64_t>(base);
    const double a = unit(raw.seed, kFlickerStream, index);
    const double b = unit(raw.seed, kFlickerStream, index + 1);
    const double f = u - base;
    // Softness 0 holds the lattice value until the next point; 1 is the smootherstep; between,
    // the move towards the next value is that fraction of the smootherstep.
    const double noise = a + (b - a) * smootherstep(f) * softness;
    double gain = 1.0 - depth * noise;

    // Dropouts.
    if (dropoutRate > 0.0 && floorLevel < 1.0) {
        const double slot = 2.0 / dropoutRate;
        const auto current = static_cast<std::int64_t>(std::floor(t / slot));
        const auto reach = static_cast<std::int64_t>(std::ceil(length / slot)) + 1;
        double dip = 0.0;
        for (std::int64_t k = current - reach; k <= current; ++k) {
            if (unit(raw.seed, kDropoutPresence, k) >= 0.5)
                continue;
            const double start = (static_cast<double>(k) + unit(raw.seed, kDropoutStart, k)) * slot;
            const double span = length * (0.5 + 0.5 * unit(raw.seed, kDropoutSize, k));
            const double into = t - start;
            if (into < 0.0 || into > span)
                continue;
            const double edge = std::max(softness * span * 0.5, 1.0e-9);
            const double rise = std::clamp(into / edge, 0.0, 1.0);
            const double fall = std::clamp((span - into) / edge, 0.0, 1.0);
            const double shape = std::min(softness > 0.0 ? smootherstep(rise) : 1.0,
                                          softness > 0.0 ? smootherstep(fall) : 1.0);
            dip = std::max(dip, shape);
        }
        gain *= 1.0 - dip * (1.0 - floorLevel);
    }
    return gain;
}

double stopsOf(const double gain) noexcept {
    if (!(gain > 0.0))
        return kLowestStops;
    return std::clamp(std::log2(gain), kLowestStops, 16.0);
}

} // namespace bloom::core::light_flicker
