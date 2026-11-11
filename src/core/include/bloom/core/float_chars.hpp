#pragma once

// Reading a floating-point number from text, the same on every platform.
//
// std::from_chars for float and double is the conversion Bloom wants: it ignores the process
// locale, rounds correctly and reports failure through its result. One standard library ships it
// only from a recent operating system release (macOS 26), later than the release Bloom supports,
// so production code calls bloom::core::fromChars instead. It has std::from_chars's signature,
// grammar and results:
//
//   - no leading whitespace and no leading '+'; a leading '-' is accepted;
//   - `fixed` reads digits with an optional fraction, `scientific` requires an exponent, `general`
//     accepts either, `hex` reads hexadecimal digits with an optional binary exponent and no "0x";
//   - "inf", "infinity", "nan" and "nan(...)" are accepted in any letter case;
//   - the result points one past the last character read; text that matches nothing answers
//     std::errc::invalid_argument and leaves `value` alone;
//   - a magnitude too large for the type, or one that rounds to zero without being zero, answers
//     std::errc::result_out_of_range and leaves `value` alone. A subnormal result is in range.
//
// Where the standard library's conversion is usable it is called directly. On macOS the text is
// matched here, so the C library sees only what std::from_chars would read, and converted by the C
// library's correctly rounding strtod with a C locale object (never the process locale). Both
// round to nearest, so the same text gives the same bits everywhere. That second implementation is
// built on every platform as float_chars_detail::fromCharsPortable, so a platform that has both
// can test one against the other.
//
// Integer std::from_chars and every std::to_chars are unaffected and are called directly.

#include <charconv>

namespace bloom::core {

[[nodiscard]] std::from_chars_result
fromChars(const char* first, const char* last, double& value,
          std::chars_format format = std::chars_format::general) noexcept;

[[nodiscard]] std::from_chars_result
fromChars(const char* first, const char* last, float& value,
          std::chars_format format = std::chars_format::general) noexcept;

namespace float_chars_detail {
// The implementation macOS uses. Production code calls fromChars.
[[nodiscard]] std::from_chars_result fromCharsPortable(const char* first, const char* last,
                                                       double& value,
                                                       std::chars_format format) noexcept;
[[nodiscard]] std::from_chars_result fromCharsPortable(const char* first, const char* last,
                                                       float& value,
                                                       std::chars_format format) noexcept;
} // namespace float_chars_detail

} // namespace bloom::core
