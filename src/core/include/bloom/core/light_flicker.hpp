#pragma once

// #567 Light Flicker: the seeded brightness of a failing or unsteady light. A pure function of the
// settings and the time in seconds, so the same seed and time give the same level in every process,
// at every frame the node is asked for, in any order.
//
//   flicker   value noise: a random level at each lattice point, `rate` points a second, joined by
//             a blend that runs from a hold (softness 0, a step at every point) to a smootherstep
//             (softness 1). The level is 1 - depth * noise, noise in [0, 1].
//   dropouts  time is cut into slots of 2 / dropoutRate seconds; each slot holds one dropout with
//             probability one half, starting at a seeded moment of the slot and lasting between
//             half and the whole of `dropoutLength` seconds. A dropout takes the level down to
//             `dropoutLevel` times what the flicker left, with edges of `softness` times half its
//             length. Earlier slots' dropouts can still be running, so every slot a dropout could
//             reach from is checked.
//
// The result is a gain, 1 for a steady light. docs/architecture/value-node-library.md, "Light
// Flicker", is the specification.

#include <cstdint>

namespace bloom::core::light_flicker {

struct Settings final {
    std::int64_t seed = 1;
    // Lattice points a second, in [kMinimumRate, kMaximumRate].
    double rate = 12.0;
    // 0 (steady) to 1 (the dimmest lattice point is black).
    double depth = 0.35;
    // Average dropouts a second, 0 to kMaximumDropoutRate.
    double dropoutRate = 0.5;
    // Seconds a dropout lasts at most, kMinimumDropoutLength to kMaximumDropoutLength.
    double dropoutLength = 0.12;
    // The gain a dropout takes the light to, 0 (dark) to 1 (no dropout).
    double dropoutLevel = 0.1;
    // 0 (hard steps and edges) to 1 (smooth).
    double softness = 0.5;

    friend bool operator==(const Settings&, const Settings&) = default;
};

inline constexpr double kMinimumRate = 0.1;
inline constexpr double kMaximumRate = 120.0;
inline constexpr double kMaximumDropoutRate = 10.0;
inline constexpr double kMinimumDropoutLength = 0.005;
inline constexpr double kMaximumDropoutLength = 2.0;
// The darkest stop count `stops` reports: a gain of 0 would be minus infinity.
inline constexpr double kLowestStops = -10.0;

// The gain at `seconds`. Out-of-domain settings are clamped, non-finite ones take the defaults.
[[nodiscard]] double evaluate(const Settings& settings, double seconds) noexcept;
// log2 of a gain, limited to [kLowestStops, 16].
[[nodiscard]] double stopsOf(double gain) noexcept;

} // namespace bloom::core::light_flicker
