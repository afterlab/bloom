// See bloom/core/float_chars.hpp.
#include <bloom/core/float_chars.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <system_error>

#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace bloom::core {
namespace {

[[nodiscard]] constexpr char lowered(const char character) noexcept {
    return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                : character;
}
[[nodiscard]] constexpr bool isDecimalDigit(const char character) noexcept {
    return character >= '0' && character <= '9';
}
[[nodiscard]] constexpr bool isHexDigit(const char character) noexcept {
    return isDecimalDigit(character) || (lowered(character) >= 'a' && lowered(character) <= 'f');
}
[[nodiscard]] constexpr bool isNanPayloadCharacter(const char character) noexcept {
    return isDecimalDigit(character) || character == '_' ||
           (lowered(character) >= 'a' && lowered(character) <= 'z');
}

[[nodiscard]] bool startsWith(const char* first, const char* const last,
                              const char* word) noexcept {
    for (; *word != '\0'; ++first, ++word) {
        if (first == last || lowered(*first) != *word)
            return false;
    }
    return true;
}

enum class Kind { None, Number, Infinity, NotANumber };

// The longest prefix of [first, last) that std::from_chars would read for `format`.
struct Match {
    Kind kind = Kind::None;
    bool negative = false;
    bool hex = false;
    // The number's own characters, after any sign: [body, end).
    const char* body = nullptr;
    const char* end = nullptr;
    // Whether the significand has a digit other than zero.
    bool nonZero = false;
};

[[nodiscard]] Match match(const char* const first, const char* const last,
                          const std::chars_format format) noexcept {
    Match result;
    const char* cursor = first;
    if (cursor != last && *cursor == '-') {
        result.negative = true;
        ++cursor;
    }
    result.body = cursor;

    if (startsWith(cursor, last, "inf")) {
        result.kind = Kind::Infinity;
        result.end = startsWith(cursor, last, "infinity") ? cursor + 8 : cursor + 3;
        return result;
    }
    if (startsWith(cursor, last, "nan")) {
        result.kind = Kind::NotANumber;
        result.end = cursor + 3;
        if (result.end != last && *result.end == '(') {
            const char* payload = result.end + 1;
            while (payload != last && isNanPayloadCharacter(*payload))
                ++payload;
            if (payload != last && *payload == ')')
                result.end = payload + 1;
        }
        return result;
    }

    result.hex = format == std::chars_format::hex;
    const auto isDigit = result.hex ? &isHexDigit : &isDecimalDigit;
    bool digits = false;
    const auto readDigits = [&] {
        for (; cursor != last && isDigit(*cursor); ++cursor) {
            digits = true;
            result.nonZero = result.nonZero || *cursor != '0';
        }
    };
    readDigits();
    if (cursor != last && *cursor == '.') {
        const char* const point = cursor;
        ++cursor;
        readDigits();
        // A lone '.' is not a number; "1." is read up to and including the point.
        if (!digits)
            cursor = point;
    }
    if (!digits)
        return result;

    const bool fixed = (format & std::chars_format::fixed) != std::chars_format{};
    const bool scientific = (format & std::chars_format::scientific) != std::chars_format{};
    const bool exponentAllowed = result.hex || scientific;
    const bool exponentRequired = !result.hex && scientific && !fixed;
    const char marker = result.hex ? 'p' : 'e';
    bool exponent = false;
    if (exponentAllowed && cursor != last && lowered(*cursor) == marker) {
        const char* probe = cursor + 1;
        if (probe != last && (*probe == '+' || *probe == '-'))
            ++probe;
        if (probe != last && isDecimalDigit(*probe)) {
            while (probe != last && isDecimalDigit(*probe))
                ++probe;
            cursor = probe;
            exponent = true;
        }
    }
    if (exponentRequired && !exponent)
        return result;

    result.kind = Kind::Number;
    result.end = cursor;
    return result;
}

// The C library's conversion in a C locale object made once for the process. The process locale is
// never read or changed: it can change at any time under the application, and changing it is not
// safe with other threads running.
#if defined(_WIN32)
using CLocale = _locale_t;
[[nodiscard]] CLocale makeCLocale() noexcept { return _create_locale(LC_ALL, "C"); }
[[nodiscard]] double convert(const char* text, const CLocale locale, const double&) noexcept {
    return _strtod_l(text, nullptr, locale);
}
[[nodiscard]] float convert(const char* text, const CLocale locale, const float&) noexcept {
    return _strtof_l(text, nullptr, locale);
}
#else
using CLocale = locale_t;
[[nodiscard]] CLocale makeCLocale() noexcept {
    return newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
}
[[nodiscard]] double convert(const char* text, const CLocale locale, const double&) noexcept {
    return strtod_l(text, nullptr, locale);
}
[[nodiscard]] float convert(const char* text, const CLocale locale, const float&) noexcept {
    return strtof_l(text, nullptr, locale);
}
#endif

// Null only when the C library could not make the locale object, which is then reported as a
// failed read rather than worked around with the process locale.
[[nodiscard]] CLocale cLocale() noexcept {
    static const CLocale locale = makeCLocale();
    return locale;
}

template <class Float>
[[nodiscard]] std::from_chars_result read(const char* const first, const char* const last,
                                          Float& value, const std::chars_format format) noexcept {
    const Match found = match(first, last, format);
    if (found.kind == Kind::None)
        return {first, std::errc::invalid_argument};
    if (found.kind == Kind::Infinity) {
        const Float infinity = static_cast<Float>(INFINITY);
        value = found.negative ? -infinity : infinity;
        return {found.end, std::errc{}};
    }
    if (found.kind == Kind::NotANumber) {
        const Float notANumber = static_cast<Float>(NAN);
        value = found.negative ? -notANumber : notANumber;
        return {found.end, std::errc{}};
    }

    // strtod needs a terminated string and a "0x" prefix for hexadecimal digits.
    const auto length = static_cast<std::size_t>(found.end - found.body);
    const std::size_t needed = length + 4;
    std::array<char, 128> local{};
    std::unique_ptr<char[]> heap;
    char* buffer = local.data();
    if (needed > local.size()) {
        heap.reset(new (std::nothrow) char[needed]);
        if (!heap)
            return {found.end, std::errc::result_out_of_range};
        buffer = heap.get();
    }
    char* out = buffer;
    if (found.negative)
        *out++ = '-';
    if (found.hex) {
        *out++ = '0';
        *out++ = 'x';
    }
    std::memcpy(out, found.body, length);
    out[length] = '\0';

    const CLocale locale = cLocale();
    if (locale == CLocale{})
        return {first, std::errc::invalid_argument};
    const Float converted = convert(buffer, locale, value);
    // Out of range by the result, not by errno: the C library also raises ERANGE for a subnormal,
    // which is a representable answer.
    if (std::isinf(converted) || (converted == Float{0} && found.nonZero))
        return {found.end, std::errc::result_out_of_range};
    value = converted;
    return {found.end, std::errc{}};
}

} // namespace

namespace float_chars_detail {

std::from_chars_result fromCharsPortable(const char* const first, const char* const last,
                                         double& value, const std::chars_format format) noexcept {
    return read(first, last, value, format);
}

std::from_chars_result fromCharsPortable(const char* const first, const char* const last,
                                         float& value, const std::chars_format format) noexcept {
    return read(first, last, value, format);
}

} // namespace float_chars_detail

std::from_chars_result fromChars(const char* const first, const char* const last, double& value,
                                 const std::chars_format format) noexcept {
#if defined(__APPLE__)
    return float_chars_detail::fromCharsPortable(first, last, value, format);
#else
    return std::from_chars(first, last, value, format);
#endif
}

std::from_chars_result fromChars(const char* const first, const char* const last, float& value,
                                 const std::chars_format format) noexcept {
#if defined(__APPLE__)
    return float_chars_detail::fromCharsPortable(first, last, value, format);
#else
    return std::from_chars(first, last, value, format);
#endif
}

} // namespace bloom::core
