#pragma once

// #527 Dither: the threshold patterns behind the node, the same on every platform and at every
// resolution, so a frame's dither is part of its document. Each pattern is a square of side n (a power
// of two) holding every rank 0 .. n*n - 1 once; the renderer tiles it over the picture and turns a rank
// r into the threshold (r + 0.5) / (n * n).
//
// - Ordered (Bayer): the recursive Bayer matrix (Bayer, "An optimum method for two-level rendition of
//   continuous-tone pictures", 1973), M(1) = [0] and M(2k) = [[4 M(k), 4 M(k) + 2], [4 M(k) + 3,
//   4 M(k) + 1]], for n = 2, 4, 8 and 16.
// - Blue noise: Ulichney's void-and-cluster ("The void-and-cluster method for dither array
//   generation", SPIE 1913, 1993) on a 64 x 64 torus, computed on the host in integers so it is the
//   same bit for bit everywhere (docs/architecture/image-filters.md, "Dither").
//
// A pattern is moved per seed, frame and channel by a whole-cell shift and a rotation of its ranks
// (offsets), so seeds and frames give unrelated patterns that keep their spectrum.

#include <cstdint>
#include <span>
#include <vector>

namespace bloom::core::dither {

inline constexpr int kBlueNoiseSide = 64;
// The Gaussian of the void-and-cluster energy, sigma 1.5 (Ulichney's value), as integer weights.
inline constexpr double kBlueNoiseSigma = 1.5;
inline constexpr double kBlueNoiseWeightScale = 1073741824.0; // 2^30
// The share of the initial binary pattern's cells that are set.
inline constexpr int kBlueNoiseInitialDivisor = 10;

// Whether `side` is a Bayer side the node offers (2, 4, 8 or 16).
[[nodiscard]] constexpr bool isBayerSide(const int side) noexcept {
    return side == 2 || side == 4 || side == 8 || side == 16;
}

// The Bayer matrix of `side` (a power of two from 1 to 256), row major. Empty for another side.
[[nodiscard]] std::vector<std::uint16_t> bayerMatrix(int side);

// The void-and-cluster ranks of a `side` x `side` torus (a power of two from 4 to 128), row major.
// The initial pattern's set cells come from SplitMix64 seeded with `seed`. Empty for another side.
[[nodiscard]] std::vector<std::uint16_t> voidAndCluster(int side, std::uint64_t seed = 1);

// The integer energy weight of a torus offset (dx, dy) with dx and dy each in [0, side): the
// Gaussian of the shorter wrapped distance times 2^30, rounded to the nearest integer, with e^-x from
// a fixed series of IEEE additions, multiplications and divisions only (no library exp).
[[nodiscard]] std::int64_t voidAndClusterWeight(int dx, int dy, int side) noexcept;

// The node's blue-noise table: voidAndCluster(kBlueNoiseSide), computed once.
[[nodiscard]] std::span<const std::uint16_t> blueNoiseTable();

// A pattern's placement for one seed, frame and channel (0 red, 1 green, 2 blue): a shift of whole
// cells in x and y, each in [0, side), and a rotation of the ranks in [0, side * side).
struct Offset final {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t rank = 0;
    friend bool operator==(const Offset&, const Offset&) = default;
};
[[nodiscard]] Offset offset(std::int64_t seed, std::int64_t frame, int channel, int side) noexcept;

} // namespace bloom::core::dither
