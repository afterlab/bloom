#include <bloom/core/checked_arithmetic.hpp>

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

// Every pair of a narrow type against exact arithmetic in a wider one.
template <typename T> void exhaustive(const char* name) {
    constexpr long long low = std::numeric_limits<T>::min();
    constexpr long long high = std::numeric_limits<T>::max();
    bool agrees = true;
    for (long long a = low; a <= high; ++a) {
        for (long long b = low; b <= high; ++b) {
            const auto ta = static_cast<T>(a);
            const auto tb = static_cast<T>(b);
            const auto fits = [&](const long long exact) { return exact >= low && exact <= high; };
            T sum{};
            T difference{};
            T product{};
            const bool sumOverflows = bloom::core::addOverflows(ta, tb, sum);
            const bool differenceOverflows = bloom::core::subOverflows(ta, tb, difference);
            const bool productOverflows = bloom::core::mulOverflows(ta, tb, product);
            agrees = agrees && sumOverflows == !fits(a + b) &&
                     differenceOverflows == !fits(a - b) && productOverflows == !fits(a * b) &&
                     sum == static_cast<T>(static_cast<unsigned long long>(a + b)) &&
                     difference == static_cast<T>(static_cast<unsigned long long>(a - b)) &&
                     product == static_cast<T>(static_cast<unsigned long long>(a * b));
        }
    }
    expect(agrees, name);
}

} // namespace

int main() {
    using bloom::core::addOverflows;
    using bloom::core::mulOverflows;
    using bloom::core::subOverflows;

    exhaustive<std::int8_t>("signed 8-bit add, subtract and multiply match exact arithmetic");
    exhaustive<std::uint8_t>("unsigned 8-bit add, subtract and multiply match exact arithmetic");

    constexpr auto signedMax = std::numeric_limits<std::int64_t>::max();
    constexpr auto signedMin = std::numeric_limits<std::int64_t>::min();
    std::int64_t signedOut = 0;
    expect(!addOverflows<std::int64_t>(signedMax - 1, 1, signedOut) && signedOut == signedMax,
           "an addition landing on the maximum does not overflow");
    expect(addOverflows<std::int64_t>(signedMax, 1, signedOut), "maximum plus one overflows");
    expect(addOverflows<std::int64_t>(signedMin, -1, signedOut),
           "minimum plus minus one overflows");
    expect(!subOverflows<std::int64_t>(signedMin + 1, 1, signedOut) && signedOut == signedMin,
           "a subtraction landing on the minimum does not overflow");
    expect(subOverflows<std::int64_t>(signedMin, 1, signedOut), "minimum minus one overflows");
    expect(subOverflows<std::int64_t>(signedMax, -1, signedOut),
           "maximum minus minus one overflows");
    expect(mulOverflows<std::int64_t>(signedMin, -1, signedOut),
           "minimum times minus one overflows");
    expect(mulOverflows<std::int64_t>(signedMax / 2 + 1, 2, signedOut),
           "a product past the maximum overflows");
    expect(!mulOverflows<std::int64_t>(signedMin / 2, 2, signedOut) && signedOut == signedMin,
           "a product landing on the minimum does not overflow");

    constexpr auto unsignedMax = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t unsignedOut = 0;
    expect(addOverflows<std::uint64_t>(unsignedMax, 1, unsignedOut) && unsignedOut == 0,
           "an unsigned addition wraps and reports it");
    expect(subOverflows<std::uint64_t>(0, 1, unsignedOut) && unsignedOut == unsignedMax,
           "an unsigned subtraction below zero wraps and reports it");
    expect(mulOverflows<std::uint64_t>(unsignedMax, 2, unsignedOut),
           "an unsigned product past the maximum overflows");
    expect(!mulOverflows<std::uint64_t>(1ULL << 32U, 1ULL << 31U, unsignedOut) &&
               unsignedOut == (1ULL << 63U),
           "an unsigned product that fits does not overflow");
    return failures == 0 ? 0 : 1;
}
