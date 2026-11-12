#ifndef BLOOM_CORE_INT128_HPP
#define BLOOM_CORE_INT128_HPP

#include <cstdint>
#include <limits>
#include <type_traits>

namespace bloom::core {

// A signed 128-bit integer for exact intermediate rational arithmetic. GCC and Clang provide it
// natively and Int128 is that type unchanged. MSVC has no 128-bit integer, so there Int128 is a
// small two's-complement class with the same operator semantics for the operations Bloom uses:
// + - * / % (division truncates toward zero, the remainder takes the dividend's sign), unary minus,
// the comparisons, and explicit conversion to and from the built-in integer types. Overflow past
// 128 bits wraps; callers bound their operands (two 64-bit terms) so it cannot occur.
#if defined(__SIZEOF_INT128__)
__extension__ typedef __int128 Int128;
#else
class Int128 final {
  public:
    constexpr Int128() noexcept = default;
    template <typename T>
        requires std::is_integral_v<T>
    constexpr Int128(const T value) noexcept // NOLINT(google-explicit-constructor)
        : high_(signExtension(value)), low_(static_cast<std::uint64_t>(value)) {}

    template <typename T>
        requires std::is_integral_v<T>
    [[nodiscard]] explicit constexpr operator T() const noexcept {
        return static_cast<T>(low_);
    }

    [[nodiscard]] friend constexpr Int128 operator-(const Int128 value) noexcept {
        return fromBits(~value.high_ + (value.low_ == 0 ? 1U : 0U), ~value.low_ + 1U);
    }
    [[nodiscard]] friend constexpr Int128 operator+(const Int128 a, const Int128 b) noexcept {
        const std::uint64_t low = a.low_ + b.low_;
        return fromBits(a.high_ + b.high_ + (low < a.low_ ? 1U : 0U), low);
    }
    [[nodiscard]] friend constexpr Int128 operator-(const Int128 a, const Int128 b) noexcept {
        return a + (-b);
    }
    [[nodiscard]] friend constexpr Int128 operator*(const Int128 a, const Int128 b) noexcept {
        // Low 128 bits of the product: the full 64x64 low product plus the cross terms shifted up.
        const std::uint64_t a0 = a.low_ & 0xFFFFFFFFU;
        const std::uint64_t a1 = a.low_ >> 32U;
        const std::uint64_t b0 = b.low_ & 0xFFFFFFFFU;
        const std::uint64_t b1 = b.low_ >> 32U;
        const std::uint64_t p00 = a0 * b0;
        const std::uint64_t p01 = a0 * b1;
        const std::uint64_t p10 = a1 * b0;
        const std::uint64_t p11 = a1 * b1;
        const std::uint64_t middle = (p00 >> 32U) + (p01 & 0xFFFFFFFFU) + (p10 & 0xFFFFFFFFU);
        const std::uint64_t low = (p00 & 0xFFFFFFFFU) | (middle << 32U);
        const std::uint64_t high = p11 + (p01 >> 32U) + (p10 >> 32U) + (middle >> 32U) +
                                   a.high_ * b.low_ + a.low_ * b.high_;
        return fromBits(high, low);
    }
    [[nodiscard]] friend constexpr Int128 operator/(const Int128 a, const Int128 b) noexcept {
        return divide(a, b, false);
    }
    [[nodiscard]] friend constexpr Int128 operator%(const Int128 a, const Int128 b) noexcept {
        return divide(a, b, true);
    }
    constexpr Int128& operator+=(const Int128 other) noexcept { return *this = *this + other; }
    constexpr Int128& operator-=(const Int128 other) noexcept { return *this = *this - other; }
    constexpr Int128& operator*=(const Int128 other) noexcept { return *this = *this * other; }
    constexpr Int128& operator/=(const Int128 other) noexcept { return *this = *this / other; }
    constexpr Int128& operator%=(const Int128 other) noexcept { return *this = *this % other; }

    [[nodiscard]] friend constexpr bool operator==(const Int128 a, const Int128 b) noexcept {
        return a.high_ == b.high_ && a.low_ == b.low_;
    }
    [[nodiscard]] friend constexpr bool operator<(const Int128 a, const Int128 b) noexcept {
        const bool aNegative = a.negative();
        if (aNegative != b.negative()) {
            return aNegative;
        }
        return a.high_ != b.high_ ? a.high_ < b.high_ : a.low_ < b.low_;
    }
    [[nodiscard]] friend constexpr bool operator>(const Int128 a, const Int128 b) noexcept {
        return b < a;
    }
    [[nodiscard]] friend constexpr bool operator<=(const Int128 a, const Int128 b) noexcept {
        return !(b < a);
    }
    [[nodiscard]] friend constexpr bool operator>=(const Int128 a, const Int128 b) noexcept {
        return !(a < b);
    }

  private:
    template <typename T>
    [[nodiscard]] static constexpr std::uint64_t signExtension(const T value) {
        if constexpr (std::is_signed_v<T>) {
            return value < 0 ? ~std::uint64_t{0} : std::uint64_t{0};
        } else {
            static_cast<void>(value);
            return 0;
        }
    }
    [[nodiscard]] static constexpr Int128 fromBits(const std::uint64_t high,
                                                   const std::uint64_t low) noexcept {
        Int128 result;
        result.high_ = high;
        result.low_ = low;
        return result;
    }
    [[nodiscard]] constexpr bool negative() const noexcept { return (high_ >> 63U) != 0; }

    // Shift-subtract division of magnitudes; the caller's operands are bounded, so the 128
    // iterations are cheap and the code stays obviously correct.
    [[nodiscard]] static constexpr Int128 divide(const Int128 dividend, const Int128 divisor,
                                                 const bool wantRemainder) noexcept {
        const bool dividendNegative = dividend.negative();
        const bool divisorNegative = divisor.negative();
        const Int128 n = dividendNegative ? -dividend : dividend;
        const Int128 d = divisorNegative ? -divisor : divisor;
        if (d.high_ == 0 && d.low_ == 0) {
            return Int128{}; // Division by zero is a caller error; never trap.
        }
        std::uint64_t quotientHigh = 0;
        std::uint64_t quotientLow = 0;
        std::uint64_t remainderHigh = 0;
        std::uint64_t remainderLow = 0;
        for (int bit = 127; bit >= 0; --bit) {
            remainderHigh = (remainderHigh << 1U) | (remainderLow >> 63U);
            const std::uint64_t source = bit >= 64 ? n.high_ : n.low_;
            remainderLow = (remainderLow << 1U) | ((source >> (bit % 64)) & 1U);
            const bool atLeast =
                remainderHigh != d.high_ ? remainderHigh > d.high_ : remainderLow >= d.low_;
            if (atLeast) {
                const std::uint64_t low = remainderLow - d.low_;
                remainderHigh = remainderHigh - d.high_ - (remainderLow < d.low_ ? 1U : 0U);
                remainderLow = low;
                if (bit >= 64) {
                    quotientHigh |= std::uint64_t{1} << (bit - 64);
                } else {
                    quotientLow |= std::uint64_t{1} << bit;
                }
            }
        }
        if (wantRemainder) {
            const Int128 remainder = fromBits(remainderHigh, remainderLow);
            return dividendNegative ? -remainder : remainder;
        }
        const Int128 quotient = fromBits(quotientHigh, quotientLow);
        return dividendNegative != divisorNegative ? -quotient : quotient;
    }

    std::uint64_t high_ = 0;
    std::uint64_t low_ = 0;
};
#endif

} // namespace bloom::core

#endif // BLOOM_CORE_INT128_HPP
