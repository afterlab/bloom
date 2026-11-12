#include <bloom/core/int128.hpp>

#include <cstdint>
#include <iostream>
#include <limits>

namespace {

int failures = 0;

void expect(const bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

using bloom::core::Int128;

[[nodiscard]] constexpr std::int64_t toInt64(const Int128 value) {
    return static_cast<std::int64_t>(value);
}

} // namespace

int main() {
    constexpr auto int64Max = std::numeric_limits<std::int64_t>::max();
    constexpr auto int64Min = std::numeric_limits<std::int64_t>::min();
    const Int128 twoTo64 = Int128(std::int64_t{1} << 32U) * Int128(std::int64_t{1} << 32U);

    // (2^63 - 1)^2 = 0x3FFFFFFFFFFFFFFF_0000000000000001
    const Int128 square = Int128(int64Max) * Int128(int64Max);
    expect(toInt64(square / twoTo64) == 0x3FFFFFFFFFFFFFFFLL && toInt64(square % twoTo64) == 1,
           "a product of two 63-bit values keeps all 126 bits");
    expect(square > Int128(int64Max) && Int128(int64Max) < square,
           "wide values order above narrow");
    expect(square / Int128(int64Max) == Int128(int64Max) && square % Int128(int64Max) == Int128(0),
           "an exact quotient divides back");

    // Truncating division and the remainder's sign follow the dividend.
    expect(toInt64(Int128(-7) / Int128(2)) == -3 && toInt64(Int128(-7) % Int128(2)) == -1,
           "negative dividend truncates toward zero");
    expect(toInt64(Int128(7) / Int128(-2)) == -3 && toInt64(Int128(7) % Int128(-2)) == 1,
           "negative divisor truncates toward zero");
    expect(toInt64(Int128(-7) / Int128(-2)) == 3 && toInt64(Int128(-7) % Int128(-2)) == -1,
           "two negatives give a positive quotient");

    // Sums and differences across the 64-bit boundary.
    const Int128 pastMax = Int128(int64Max) + Int128(1);
    expect(pastMax > Int128(int64Max) && pastMax - Int128(1) == Int128(int64Max),
           "a sum past int64 stays exact");
    const Int128 pastMin = Int128(int64Min) - Int128(1);
    expect(pastMin < Int128(int64Min) && pastMin + Int128(1) == Int128(int64Min),
           "a difference below int64 stays exact");
    expect(-Int128(int64Min) > Int128(int64Max), "negating the int64 minimum is exact");
    expect(Int128(-1) < Int128(0) && Int128(0) < Int128(1) && Int128(-5) <= Int128(-5) &&
               Int128(3) >= Int128(3),
           "comparisons order signed values");

    // The reduction loop of cpu_layer_time_resolution: gcd by repeated remainder.
    Int128 a = Int128(int64Max - 1) * Int128(12);
    Int128 b = Int128(int64Max - 1) * Int128(18);
    while (b != Int128(0)) {
        const Int128 t = a % b;
        a = b;
        b = t;
    }
    expect(a == Int128(int64Max - 1) * Int128(6), "the gcd of two wide multiples is exact");

    // Signed and unsigned narrow sources convert faithfully.
    expect(Int128(std::uint64_t{0xFFFFFFFFFFFFFFFFULL}) > Int128(int64Max),
           "an unsigned 64-bit source is not sign-extended");
    expect(toInt64(Int128(std::int8_t{-3})) == -3, "a narrow signed source sign-extends");
    return failures == 0 ? 0 : 1;
}
