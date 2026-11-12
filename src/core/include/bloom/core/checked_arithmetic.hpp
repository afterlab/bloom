#ifndef BLOOM_CORE_CHECKED_ARITHMETIC_HPP
#define BLOOM_CORE_CHECKED_ARITHMETIC_HPP

#include <concepts>
#include <limits>
#include <type_traits>

namespace bloom::core {

// Overflow-reporting integer arithmetic with the contract of the GCC/Clang __builtin_*_overflow
// family: each function stores the wrapped result in `out` and returns true when the exact result
// does not fit the operand type. The compilers that have the builtins use them unchanged; the
// portable definitions serve MSVC.
template <std::integral T>
[[nodiscard]] constexpr bool addOverflows(const T a, const T b, T& out) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_add_overflow(a, b, &out);
#else
    using U = std::make_unsigned_t<T>;
    out = static_cast<T>(static_cast<U>(static_cast<U>(a) + static_cast<U>(b)));
    if constexpr (std::is_signed_v<T>) {
        return b > 0 ? a > std::numeric_limits<T>::max() - b
                     : a < std::numeric_limits<T>::min() - b;
    } else {
        return out < a;
    }
#endif
}

template <std::integral T>
[[nodiscard]] constexpr bool subOverflows(const T a, const T b, T& out) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_sub_overflow(a, b, &out);
#else
    using U = std::make_unsigned_t<T>;
    out = static_cast<T>(static_cast<U>(static_cast<U>(a) - static_cast<U>(b)));
    if constexpr (std::is_signed_v<T>) {
        return b > 0 ? a < std::numeric_limits<T>::min() + b
                     : a > std::numeric_limits<T>::max() + b;
    } else {
        return b > a;
    }
#endif
}

template <std::integral T>
[[nodiscard]] constexpr bool mulOverflows(const T a, const T b, T& out) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_mul_overflow(a, b, &out);
#else
    using U = std::make_unsigned_t<T>;
    out = static_cast<T>(static_cast<U>(static_cast<U>(a) * static_cast<U>(b)));
    if (a == 0 || b == 0) {
        return false;
    }
    if constexpr (std::is_signed_v<T>) {
        if (a > 0) {
            return b > 0 ? a > std::numeric_limits<T>::max() / b
                         : b < std::numeric_limits<T>::min() / a;
        }
        return b > 0 ? a < std::numeric_limits<T>::min() / b
                     : a < std::numeric_limits<T>::max() / b;
    } else {
        return a > std::numeric_limits<T>::max() / b;
    }
#endif
}

} // namespace bloom::core

#endif // BLOOM_CORE_CHECKED_ARITHMETIC_HPP
