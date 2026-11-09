#pragma once

// #341 Oscillator, Wiggle and Loop Value: motion utilities that are pure functions of the settings and
// a time in seconds, so the same settings and time give the same value in every process, at every
// frame the node is asked for, in any order. None keeps state, so evaluation at an arbitrary time
// (the `timeOffset` operand moves it) costs the same as at the next frame. There is no expression
// engine: each is one closed-form function.
//
//   oscillator  offset + amplitude * wave(frequency * t + phase / 360), a sine, triangle, square or
//               sawtooth that starts at its zero crossing (the sawtooth and square rise from their
//               start), range [offset - amplitude, offset + amplitude].
//   wiggle      offset + amplitude * noise(t), the versioned fractal value noise below, in
//               [offset - amplitude, offset + amplitude], smooth, with mean near the offset.
//   loop        a ramp from `start` to `end` over `duration` seconds that repeats, or runs back and
//               forth (ping-pong); also reports the whole cycles elapsed.
//
// The noise is versioned: kNoiseVersion names the definition, and a change to the hash, the blend or
// the octave rule is a new version, never an edit, because a document that stores a seed stores the
// motion it produced. Version 1: the lattice level at an integer index is
// `primitives::hashUnitValue(mix(seed, octave, index))` (the same hash Light Flicker uses), mapped to
// [-1, 1]; between lattice points the level is blended with a smootherstep; octave k runs at
// frequency * 2^k with amplitude roughness^k, and the sum is divided by the sum of the amplitudes.
// docs/architecture/value-node-library.md, "Motion utilities", is the specification.

#include <cstdint>

namespace bloom::core::motion_values {

inline constexpr std::int64_t kNoiseVersion = 1;

enum class Wave : std::uint8_t { Sine = 0, Triangle = 1, Square = 2, Sawtooth = 3 };
enum class LoopMode : std::uint8_t { Repeat = 0, PingPong = 1 };

inline constexpr double kMaximumFrequency = 1000.0;
inline constexpr std::int64_t kMaximumOctaves = 8;
inline constexpr double kMinimumDuration = 0.001;

struct OscillatorSettings final {
    Wave wave = Wave::Sine;
    // Cycles a second; a negative frequency runs the wave backwards.
    double frequency = 1.0;
    double amplitude = 1.0;
    double offset = 0.0;
    // Degrees; 360 is one whole cycle.
    double phase = 0.0;
};

struct WiggleSettings final {
    std::int64_t seed = 1;
    // Lattice points a second of the first octave.
    double frequency = 2.0;
    double amplitude = 1.0;
    double offset = 0.0;
    std::int64_t octaves = 1;
    // Each further octave's amplitude relative to the one before, 0 to 1.
    double roughness = 0.5;
};

struct LoopSettings final {
    LoopMode mode = LoopMode::Repeat;
    double start = 0.0;
    double end = 1.0;
    double duration = 1.0;
};

struct LoopResult final {
    double value = 0.0;
    // Whole cycles elapsed (negative before time zero), rounded down.
    std::int64_t cycle = 0;
};

// Out-of-domain settings are clamped; non-finite ones take the defaults; a non-finite time is zero.
[[nodiscard]] double oscillate(const OscillatorSettings& settings, double seconds) noexcept;
[[nodiscard]] double wiggle(const WiggleSettings& settings, double seconds) noexcept;
[[nodiscard]] LoopResult loop(const LoopSettings& settings, double seconds) noexcept;

} // namespace bloom::core::motion_values
