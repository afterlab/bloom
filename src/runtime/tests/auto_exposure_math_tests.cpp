#include "auto_exposure_math.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <iostream>

// Auto Exposure's CPU reference must be the same frame on every platform, so its logarithm and
// power must return the same bits everywhere. The expected values here are bits, not tolerances:
// the nearest float to the true result, worked out in 60-digit decimal arithmetic and not with a
// C library. A platform whose library rounds differently fails here.

namespace {

struct Case final {
    std::uint32_t input;
    std::uint32_t expected;
};

constexpr std::array kLog2Cases{
    Case{0x3F800000U, 0x00000000U}, // 1.0
    Case{0x40000000U, 0x3F800000U}, // 2.0
    Case{0x40400000U, 0x3FCAE00DU}, // 3.0
    Case{0x3F000000U, 0xBF800000U}, // 0.5
    Case{0x3E800000U, 0xC0000000U}, // 0.25
    Case{0x3DCCCCCDU, 0xC0549A78U}, // 0.100000
    Case{0x3E3851ECU, 0xC01E54E3U}, // 0.180000
    Case{0x3F800001U, 0x3438AA3AU}, // 1.000000
    Case{0x3F7FFFFFU, 0xB3B8AA3CU}, // 0.999999
    Case{0x3FC00000U, 0x3F15C01AU}, // 1.5
    Case{0x40A00000U, 0x40149A78U}, // 5.0
    Case{0x41200000U, 0x40549A78U}, // 10.0
    Case{0x3A83126FU, 0xC11F73DAU}, // 0.001000
    Case{0x358637BDU, 0xC19F73DAU}, // 9.999999e-07
    Case{0x42C80000U, 0x40D49A78U}, // 100.0
    Case{0x3EAAAAABU, 0xBFCAE00DU}, // 0.333333
    Case{0x3F2AAAABU, 0xBF15C01AU}, // 0.666666
    Case{0x3F99999AU, 0x3E86AC74U}, // 1.200000
    Case{0x40490FDBU, 0x3FD3643AU}, // 3.141592
    Case{0x3D4CCCCDU, 0xC08A4D3CU}, // 0.050000
};

constexpr std::array kExp2Cases{
    Case{0x00000000U, 0x3F800000U}, // 0.0
    Case{0x3F800000U, 0x40000000U}, // 1.0
    Case{0xBF800000U, 0x3F000000U}, // -1.0
    Case{0x3F000000U, 0x3FB504F3U}, // 0.5
    Case{0xBF000000U, 0x3F3504F3U}, // -0.5
    Case{0x3E800000U, 0x3F9837F0U}, // 0.25
    Case{0x3DCCCCCDU, 0x3F892FDFU}, // 0.100000
    Case{0xBDCCCCCDU, 0x3F6EDB40U}, // -0.100000
    Case{0x40000000U, 0x40800000U}, // 2.0
    Case{0xC0000000U, 0x3E800000U}, // -2.0
    Case{0x40400000U, 0x41000000U}, // 3.0
    Case{0x3EAAAAABU, 0x3FA14518U}, // 0.333333
    Case{0xBEAAAAABU, 0x3F4B2FF5U}, // -0.333333
    Case{0x3FC00000U, 0x403504F3U}, // 1.5
    Case{0xBFC00000U, 0x3EB504F3U}, // -1.5
    Case{0x40900000U, 0x41B504F3U}, // 4.5
    Case{0xC0900000U, 0x3D3504F3U}, // -4.5
    Case{0x3A83126FU, 0x3F8016B9U}, // 0.001000
    Case{0xBA83126FU, 0x3F7FD297U}, // -0.001000
    Case{0x40C90FDBU, 0x429BC2AFU}, // 6.283185
};

} // namespace

int main() {
    bool ok = true;
    const auto check = [&](const char* what, const Case& item, const float got) {
        const auto bits = std::bit_cast<std::uint32_t>(got);
        if (bits == item.expected)
            return;
        ok = false;
        std::cerr << std::hex << what << ": input 0x" << item.input << " got 0x" << bits
                  << " want 0x" << item.expected << std::dec << '\n';
    };
    for (const auto& item : kLog2Cases)
        check("log2", item,
              bloom::runtime::detail::autoExposureLog2(std::bit_cast<float>(item.input)));
    for (const auto& item : kExp2Cases)
        check("exp2", item,
              bloom::runtime::detail::autoExposureExp2(std::bit_cast<float>(item.input)));
    return ok ? 0 : 1;
}
