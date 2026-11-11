#include <bloom/core/specified_log2.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>

// bloom::core::specifiedLog2 must return the same bits on every platform, so the expected values
// here are bits, not tolerances. They were produced by an independent binary32 model of the
// operations the header lists, not by this implementation; a build that fuses, reorders or
// substitutes any operation fails here.

namespace {

struct Case final {
    std::uint32_t input;
    std::uint32_t expected;
};

constexpr std::array kCases{
    Case{0x3F800000U, 0x00000000U}, // 1
    Case{0x40000000U, 0x3F800000U}, // 2
    Case{0x40400000U, 0x3FCAE00DU}, // 3
    Case{0x40800000U, 0x40000000U}, // 4
    Case{0x40A00000U, 0x40149A78U}, // 5
    Case{0x40E00000U, 0x4033ABB4U}, // 7
    Case{0x41200000U, 0x40549A78U}, // 10
    Case{0x42C80000U, 0x40D49A78U}, // 100
    Case{0x437F0000U, 0x40FFD1BEU}, // 255
    Case{0x43800000U, 0x41000000U}, // 256
    Case{0x43808000U, 0x4100170AU}, // 257
    Case{0x447A0000U, 0x411F73DAU}, // 1000
    Case{0x45800800U, 0x41400171U}, // 4097
    Case{0x477FFF00U, 0x417FFFE9U}, // 65535
    Case{0x47800000U, 0x41800000U}, // 65536
    Case{0x49742400U, 0x419F73DAU}, // 1000000
    Case{0x4B7FFFFFU, 0x41C00000U}, // 16777215
    Case{0x4B800000U, 0x41C00000U}, // 16777216
    Case{0x3FC00000U, 0x3F15C01AU}, // 1.5
    Case{0x3FB504F3U, 0x3EFFFFFEU}, // the largest mantissa kept as it is
    Case{0x3FB504F4U, 0x3F000002U}, // the smallest mantissa halved
    Case{0x3F800001U, 0x3438AA3BU}, // the value after 1
    Case{0x403FFFFFU, 0x3FCAE00CU}, // the value before 3
    Case{0x4640E6B6U, 0x415977AEU}, // 12345.678
};

} // namespace

int main() {
    bool ok = true;
    const auto fail = [&](const char* what, const std::uint32_t input, const std::uint32_t got,
                          const std::uint32_t want) {
        ok = false;
        std::cerr << std::hex << what << ": input 0x" << input << " got 0x" << got << " want 0x"
                  << want << std::dec << '\n';
    };
    for (const auto& item : kCases) {
        const auto got = std::bit_cast<std::uint32_t>(
            bloom::core::specifiedLog2(std::bit_cast<float>(item.input)));
        if (got != item.expected)
            fail("fixed bits", item.input, got, item.expected);
    }

    // Over every whole number a histogram can count: a power of two gives its exponent, the result
    // never decreases, and it stays within one unit in the last place of the C library's value
    // (the library is not the reference, only a guard against a wrong constant).
    float previous = 0.0F;
    for (std::uint32_t n = 1; n <= 16777216U; ++n) {
        const auto x = static_cast<float>(n);
        const float y = bloom::core::specifiedLog2(x);
        if (y < previous)
            fail("decreases", std::bit_cast<std::uint32_t>(x), std::bit_cast<std::uint32_t>(y),
                 std::bit_cast<std::uint32_t>(previous));
        previous = y;
        if ((n & (n - 1)) == 0 && y != static_cast<float>(std::countr_zero(n)))
            fail("power of two", std::bit_cast<std::uint32_t>(x), std::bit_cast<std::uint32_t>(y),
                 std::bit_cast<std::uint32_t>(static_cast<float>(std::countr_zero(n))));
        const double exact = std::log2(static_cast<double>(n));
        const double unit = static_cast<double>(std::nextafter(y, 100.0F)) - static_cast<double>(y);
        if (std::abs(static_cast<double>(y) - exact) > unit) {
            fail("accuracy", std::bit_cast<std::uint32_t>(x), std::bit_cast<std::uint32_t>(y),
                 std::bit_cast<std::uint32_t>(static_cast<float>(exact)));
            break;
        }
        if (!ok)
            break;
    }

    // The shader text carries the same constants.
    const auto glsl = bloom::core::specifiedLog2Glsl();
    for (const auto* constant :
         {"0x3FB504F3u", "0x4038AA3Bu", "0x3F76384Fu", "0x3F13BB63u", "0x3ED30BB1u", "0x3EA4258Au",
          "bloom_ocio_cpu_div(t, denominator)", "precise float bloom_specified_log2(float x)"}) {
        if (glsl.find(constant) == std::string_view::npos) {
            ok = false;
            std::cerr << "the shader text lacks " << constant << '\n';
        }
    }
    if (!ok) {
        std::cerr << "FAIL: specified log2 expectations failed\n";
        return 1;
    }
    return 0;
}
