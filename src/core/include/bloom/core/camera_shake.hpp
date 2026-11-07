#pragma once

// #512 Camera Shake: the seeded motion behind the node. A pure function of the settings, the time
// and the frame rate, so the same seed and frame give the same shake in every process, on the CPU
// and the GPU (the node's transform is resolved on the host and drawn by Transform 2D), and in a
// cached or exported frame.
//
// Each of the three channels (horizontal position, vertical position, roll) is an independent
// stream of unit motion in [-1, 1]; the node scales them by its amplitudes.
//
//   Handheld   fractal value noise: random values on a lattice, `frequency` lattice points a
//              second, joined by a smootherstep; octave o runs at 3^o times the rate with
//              `roughness`^o of the weight, `octaves` of them, normalised to stay within [-1, 1].
//   Impact     a hit at every lattice point: a random push that rings out as a damped oscillation
//              (2.5 cycles per interval, decaying by exp(-6 s)), the tails of earlier hits adding.
//   Gate weave a new random registration error on every frame (three quarters) plus the slow
//              Handheld drift (a quarter).
//
// `smoothing` low-passes the motion: the average of nine samples across a window of
// smoothing / frequency seconds centred on the time.
//
// With a loop length the lattice is cut into a whole number of points and wraps, so the motion at
// time t + loop equals the motion at t exactly: the lattice point count is round(loop * frequency)
// (at least 1) and the rate becomes count / loop; Gate weave's frame count is round(loop * rate).

#include <cstdint>

namespace bloom::core::camera_shake {

enum class Mode : std::uint8_t { Handheld, Impact, GateWeave };

struct Settings final {
    Mode mode = Mode::Handheld;
    std::int64_t seed = 1;
    // Lattice points (Handheld, Impact) or drift points (Gate weave) a second, > 0.
    double frequency = 2.0;
    // Seconds before the motion repeats; 0 or less does not loop.
    double loopSeconds = 0.0;
    // Handheld and Gate weave's drift: 1 to 4 octaves, each `roughness` (0 to 1) of the one before.
    int octaves = 2;
    double roughness = 0.35;
    // 0 (none) to 1 (a window of one lattice interval).
    double smoothing = 0.0;

    friend bool operator==(const Settings&, const Settings&) = default;
};

struct Sample final {
    double x = 0.0;
    double y = 0.0;
    double roll = 0.0;

    friend bool operator==(const Sample&, const Sample&) = default;
};

// Unit motion at `seconds`. `frameRate` is frames a second (Gate weave's frames).
[[nodiscard]] Sample evaluate(const Settings& settings, double seconds, double frameRate) noexcept;

// The smallest scale about the frame centre at which the shaken image still covers the frame, for
// every translation within +-`amplitudeX`, +-`amplitudeY` pixels and every roll within
// +-`rollDegrees` (the unit motion never exceeds 1), with four pixels to spare for the resampler (a
// preview at an eighth of the resolution still has a whole edge pixel covered).
// `width` and `height` are the frame in pixels. 1 when nothing moves.
[[nodiscard]] double fillScale(double amplitudeX, double amplitudeY, double rollDegrees,
                               double width, double height) noexcept;

} // namespace bloom::core::camera_shake
