#include <bloom/core/film_grain_noise.hpp>

#include <cmath>

namespace bloom::core::film_grain {
namespace {

// Jarzynski and Olano's pcg3d, with a closing shift so all of each word is usable.
struct Hash final {
    std::uint32_t x;
    std::uint32_t y;
    std::uint32_t z;
};
[[nodiscard]] constexpr Hash hash(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
    constexpr std::uint32_t kMultiplier = 1664525U;
    constexpr std::uint32_t kIncrement = 1013904223U;
    x = x * kMultiplier + kIncrement;
    y = y * kMultiplier + kIncrement;
    z = z * kMultiplier + kIncrement;
    x += y * z;
    y += z * x;
    z += x * y;
    x ^= x >> 16U;
    y ^= y >> 16U;
    z ^= z >> 16U;
    x += y * z;
    y += z * x;
    z += x * y;
    x ^= x >> 16U;
    y ^= y >> 16U;
    z ^= z >> 16U;
    return {x, y, z};
}

// One channel's grain in one 32-bit word: 11 bits each for its position in the cell, 10 for its
// amplitude, centred on zero so the 1024 amplitudes are symmetric in (-1, 1).
constexpr float kToUnit = 0x1.0p-11F;
constexpr float kToSigned = 0x1.0p-9F;
constexpr float kSignedOffset = 0x1.0p-10F - 1.0F;

// What the grain of `word`, in the cell (dx, dy) from the point's, adds at the point.
[[nodiscard]] float contribution(const std::uint32_t word, const float dx, const float dy,
                                 const float fractionX, const float fractionY) noexcept {
    const float offsetX = dx + static_cast<float>(word >> 21U) * kToUnit - fractionX;
    const float offsetY = dy + static_cast<float>((word >> 10U) & 2047U) * kToUnit - fractionY;
    const float reach = offsetX * offsetX + offsetY * offsetY;
    if (!(reach < 1.0F))
        return 0.0F;
    float weight = 1.0F - reach;
    weight = weight * weight;
    return weight * (static_cast<float>(word & 1023U) * kToSigned + kSignedOffset);
}

} // namespace

std::uint32_t stream(const std::int64_t seed, const std::int64_t frame) noexcept {
    std::uint64_t z = (static_cast<std::uint64_t>(seed) * 0x9E3779B97F4A7C15ULL) +
                      (static_cast<std::uint64_t>(frame) * 0xBF58476D1CE4E5B9ULL) +
                      0x94D049BB133111EBULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    return static_cast<std::uint32_t>(z >> 32U);
}

std::array<float, 3> noise(const float x, const float y, const std::uint32_t stream) noexcept {
    const float cellX = std::floor(x);
    const float cellY = std::floor(y);
    const float fractionX = x - cellX;
    const float fractionY = y - cellY;
    const auto column = static_cast<std::int32_t>(cellX);
    const auto row = static_cast<std::int32_t>(cellY);
    std::array<float, 3> sum{0.0F, 0.0F, 0.0F};
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            const auto h = hash(static_cast<std::uint32_t>(column) + static_cast<std::uint32_t>(dx),
                                static_cast<std::uint32_t>(row) + static_cast<std::uint32_t>(dy),
                                stream);
            const auto cx = static_cast<float>(dx);
            const auto cy = static_cast<float>(dy);
            sum[0] = sum[0] + contribution(h.x, cx, cy, fractionX, fractionY);
            sum[1] = sum[1] + contribution(h.y, cx, cy, fractionX, fractionY);
            sum[2] = sum[2] + contribution(h.z, cx, cy, fractionX, fractionY);
        }
    }
    return sum;
}

std::string_view kernelGlsl() noexcept {
    return "uvec3 bloom_film_grain_hash(uvec3 v) {\n"
           " v = v * 1664525u + 1013904223u;\n"
           " v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;\n"
           " v ^= v >> 16u;\n"
           " v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;\n"
           " v ^= v >> 16u;\n"
           " return v;\n"
           "}\n"
           "float bloom_film_grain_add(uint word, float dx, float dy, float fx, float fy) {\n"
           " precise float offsetX = dx + float(word >> 21u) * 0.00048828125 - fx;\n"
           " precise float offsetY = dy + float((word >> 10u) & 2047u) * 0.00048828125 - fy;\n"
           " precise float reach = offsetX * offsetX + offsetY * offsetY;\n"
           " if (!(reach < 1.0)) { return 0.0; }\n"
           " precise float weight = 1.0 - reach;\n"
           " weight = weight * weight;\n"
           " precise float amplitude = float(word & 1023u) * 0.001953125 + -0.9990234375;\n"
           " precise float added = weight * amplitude;\n"
           " return added;\n"
           "}\n"
           "vec3 bloom_film_grain_noise(float x, float y, uint stream) {\n"
           " float cellX = floor(x);\n"
           " float cellY = floor(y);\n"
           " precise float fractionX = x - cellX;\n"
           " precise float fractionY = y - cellY;\n"
           " int column = int(cellX);\n"
           " int row = int(cellY);\n"
           " precise float r = 0.0;\n"
           " precise float g = 0.0;\n"
           " precise float b = 0.0;\n"
           " for (int dy = -1; dy <= 1; ++dy) {\n"
           "  for (int dx = -1; dx <= 1; ++dx) {\n"
           "   uvec3 h = bloom_film_grain_hash(uvec3(uint(column + dx), uint(row + dy), stream));\n"
           "   float cx = float(dx);\n"
           "   float cy = float(dy);\n"
           "   r = r + bloom_film_grain_add(h.x, cx, cy, fractionX, fractionY);\n"
           "   g = g + bloom_film_grain_add(h.y, cx, cy, fractionX, fractionY);\n"
           "   b = b + bloom_film_grain_add(h.z, cx, cy, fractionX, fractionY);\n"
           "  }\n"
           " }\n"
           " return vec3(r, g, b);\n"
           "}\n";
}

} // namespace bloom::core::film_grain
