// See bloom/core/specified_log2.hpp. This file is compiled with contraction off (bloom_core is a
// strict floating-point target), calls no math library function, and names every constant by its
// bits, so each line below is one IEEE binary32 operation.
#include <bloom/core/specified_log2.hpp>

#include <bit>
#include <cstdint>

namespace bloom::core {
namespace {

constexpr float kSqrt2 = std::bit_cast<float>(std::uint32_t{0x3FB504F3U});
// (2 / ln 2) / k for k = 1, 3, 5, 7, 9, each rounded to binary32.
constexpr float kC1 = std::bit_cast<float>(std::uint32_t{0x4038AA3BU});
constexpr float kC3 = std::bit_cast<float>(std::uint32_t{0x3F76384FU});
constexpr float kC5 = std::bit_cast<float>(std::uint32_t{0x3F13BB63U});
constexpr float kC7 = std::bit_cast<float>(std::uint32_t{0x3ED30BB1U});
constexpr float kC9 = std::bit_cast<float>(std::uint32_t{0x3EA4258AU});

} // namespace

float specifiedLog2(const float x) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(x);
    auto exponent = static_cast<std::int32_t>((bits >> 23U) & 0xFFU) - 127;
    float mantissa = std::bit_cast<float>((bits & 0x007FFFFFU) | 0x3F800000U);
    if (mantissa > kSqrt2) {
        mantissa = mantissa * 0.5F;
        exponent = exponent + 1;
    }
    const float t = mantissa - 1.0F;
    const float denominator = mantissa + 1.0F;
    const float s = t / denominator;
    const float z = s * s;
    float p = z * kC9;
    p = p + kC7;
    p = p * z;
    p = p + kC5;
    p = p * z;
    p = p + kC3;
    p = p * z;
    p = p + kC1;
    const float fraction = s * p;
    return fraction + static_cast<float>(exponent);
}

std::string_view specifiedLog2Glsl() noexcept {
    return R"GLSL(
precise float bloom_specified_log2(float x)
{
 uint bits = floatBitsToUint(x);
 int exponent = int((bits >> 23u) & 0xFFu) - 127;
 float mantissa = uintBitsToFloat((bits & 0x007FFFFFu) | 0x3F800000u);
 if (mantissa > uintBitsToFloat(0x3FB504F3u)) {
  mantissa = mantissa * 0.5;
  exponent = exponent + 1;
 }
 precise float t = mantissa - 1.0;
 precise float denominator = mantissa + 1.0;
 precise float s = bloom_ocio_cpu_div(t, denominator);
 precise float z = s * s;
 precise float p = z * uintBitsToFloat(0x3EA4258Au);
 p = p + uintBitsToFloat(0x3ED30BB1u);
 p = p * z;
 p = p + uintBitsToFloat(0x3F13BB63u);
 p = p * z;
 p = p + uintBitsToFloat(0x3F76384Fu);
 p = p * z;
 p = p + uintBitsToFloat(0x4038AA3Bu);
 precise float fraction = s * p;
 return fraction + float(exponent);
}
)GLSL";
}

} // namespace bloom::core
