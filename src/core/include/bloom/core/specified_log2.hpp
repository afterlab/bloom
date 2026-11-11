#pragma once

// A base-two logarithm that gives the same bits on every processor and every GPU.
//
// The C library's log2 and a GPU's log2 are each accurate to about a unit in the last place, but
// they are not required to agree with each other, and where a result is multiplied by an image
// size before its fractional part is used (the height of a histogram bar), a last-bit difference
// becomes a visible one. specifiedLog2 is instead a fixed sequence of binary32 operations, each
// rounded once: the CPU reference runs it here, compiled without contraction, and a shader runs
// the same sequence from specifiedLog2Glsl, so the two agree exactly.
//
// For x = m * 2^e with m in (sqrt(2)/2, sqrt(2)]:
//
//   t = m - 1                       exact
//   s = t / (m + 1)                 one rounded add, one correctly rounded division
//   z = s * s
//   p = (((z*C9 + C7)*z + C5)*z + C3)*z + C1    with Ck = (2 / ln 2) / k
//   log2(x) = s * p + e
//
// The domain is finite x >= 1 (a histogram's 1 + count). There specifiedLog2(1) is exactly 0, a
// power of two gives its exponent exactly, and the result never decreases as x grows over the
// whole numbers. Against a correctly rounded log2 it is within 0.56 units in the last place for
// every whole number up to 2^24 (and equal to it for 99.4% of them), and within 3 units in the
// last place (2.2e-7 relative) for any other value in [1, 2^24]. Outside the domain the result is
// still the same on every platform, but it is not a logarithm.

#include <string_view>

namespace bloom::core {

[[nodiscard]] float specifiedLog2(float x) noexcept;

// GLSL for `precise float bloom_specified_log2(float x)`, the same operations. The text calls
// `bloom_ocio_cpu_div(float, float)`, the correctly rounded division Bloom's programs already
// carry, which must be declared before it.
[[nodiscard]] std::string_view specifiedLog2Glsl() noexcept;

} // namespace bloom::core
