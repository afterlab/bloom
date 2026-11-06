#pragma once

// #486 Film Grain: the noise behind the node, the same on every device and at every resolution, so
// a frame's grain is part of its document. The CPU reference (noise) and the GPU program
// (kernelGlsl) run the same integer hash and the same binary32 operations.
//
// The plane is cut into unit cells. Each cell holds one grain per channel, at a position and with an
// amplitude hashed from the cell and the stream, so the channels' grains lie apart as a stock's dye
// layers do. A point adds up each channel's grains within distance 1 of it (those in its own cell and
// the eight around it), each weighted by (1 - d^2)^2. Hashed positions leave no lattice in the picture,
// and the sum has the same variance everywhere. The sum uses only additions and multiplications, each
// exact on every device.

#include <array>
#include <cstdint>
#include <string_view>

namespace bloom::core::film_grain {

// A cell's 32-bit stream: which grains it holds. The same seed and frame always give the same one,
// and neighbouring frames or seeds give unrelated ones, so grain never swims from frame to frame.
[[nodiscard]] std::uint32_t stream(std::int64_t seed, std::int64_t frame) noexcept;

// The grain of each channel at (x, y), in cells: zero mean and, with kNormalise applied, a standard
// deviation of 1. Uniform amplitudes have variance 1/3, a grain's squared weight integrates to pi/5
// over its disc, and there is one grain per cell, so the sum's variance is pi/15.
inline constexpr double kNormalise = 2.1850968611841584; // sqrt(15 / pi)
[[nodiscard]] std::array<float, 3> noise(float x, float y, std::uint32_t stream) noexcept;

// The same noise as GLSL: `vec3 bloom_film_grain_noise(float x, float y, uint stream)`.
[[nodiscard]] std::string_view kernelGlsl() noexcept;

} // namespace bloom::core::film_grain
