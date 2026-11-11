#include <bloom/core/float_chars.hpp>

#include <array>
#include <bit>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>

// bloom::core::fromChars must read text exactly as std::from_chars does on every platform: the
// same characters consumed, the same errors, and the same bits. The expected bits below are the
// correctly rounded values, so the test holds whichever implementation is behind the call.
//
// Every check runs twice: through fromChars, and through the implementation macOS uses, which is
// built on every platform. Where the standard library's own conversion is usable, that
// implementation is also compared with it directly over generated text.

namespace {

enum class Implementation { Public, Portable };
Implementation activeImplementation = Implementation::Public;

template <class Float>
[[nodiscard]] std::from_chars_result
fromChars(const char* const first, const char* const last, Float& value,
          const std::chars_format format = std::chars_format::general) noexcept {
    return activeImplementation == Implementation::Public
               ? bloom::core::fromChars(first, last, value, format)
               : bloom::core::float_chars_detail::fromCharsPortable(first, last, value, format);
}

class ExpectationContext final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::string_view subject = {},
                const std::source_location location = std::source_location::current()) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message;
        if (!subject.empty()) {
            std::cerr << " [" << subject << ']';
        }
        std::cerr << '\n';
    }

    [[nodiscard]] bool ok() const noexcept { return failures_ == 0; }

  private:
    std::size_t failures_ = 0;
};

constexpr double kUntouched = 123.25;

void testDoubleBits(ExpectationContext& expectations) {
    struct Exact final {
        std::string_view text;
        std::uint64_t bits;
    };
    static constexpr Exact kExact[]{
        {"0", 0x0000000000000000ULL},
        {"-0", 0x8000000000000000ULL},
        {"1", 0x3FF0000000000000ULL},
        {"-1.5", 0xBFF8000000000000ULL},
        {"0.1", 0x3FB999999999999AULL},
        {"1e23", 0x44B52D02C7E14AF6ULL},
        // Halfway between two doubles: ties go to the even significand.
        {"9007199254740993", 0x4340000000000000ULL},
        {"9007199254740995", 0x4340000000000002ULL},
        // The largest subnormal and the smallest normal, one decimal digit apart.
        {"2.2250738585072011e-308", 0x000FFFFFFFFFFFFFULL},
        {"2.2250738585072012e-308", 0x0010000000000000ULL},
        // Subnormals are representable answers.
        {"4.9e-324", 0x0000000000000001ULL},
        {"2.4703282292062328e-324", 0x0000000000000001ULL},
        {"1.7976931348623157e308", 0x7FEFFFFFFFFFFFFFULL},
        {"1E3", 0x408F400000000000ULL},
        {".5", 0x3FE0000000000000ULL},
    };
    for (const auto& exact : kExact) {
        double value = kUntouched;
        const auto result =
            fromChars(exact.text.data(), exact.text.data() + exact.text.size(), value);
        expectations.expect(result.ec == std::errc{} &&
                                result.ptr == exact.text.data() + exact.text.size() &&
                                std::bit_cast<std::uint64_t>(value) == exact.bits,
                            "a double reads to its correctly rounded bits", exact.text);
    }

    static constexpr std::string_view kOutOfRange[]{
        "1e309",
        "-1e309",
        "1.7976931348623159e308",
        // Below half of the smallest subnormal: the answer would be a zero nobody wrote.
        "2.4703282292062327e-324",
        "1e-400",
        "-1e-400",
    };
    for (const auto text : kOutOfRange) {
        double value = kUntouched;
        const auto result = fromChars(text.data(), text.data() + text.size(), value);
        expectations.expect(result.ec == std::errc::result_out_of_range &&
                                result.ptr == text.data() + text.size() && value == kUntouched,
                            "a magnitude outside the range is refused and the value is left alone",
                            text);
    }
}

void testFloatBits(ExpectationContext& expectations) {
    struct Exact final {
        std::string_view text;
        std::uint32_t bits;
    };
    static constexpr Exact kExact[]{
        {"0.1", 0x3DCCCCCDU},
        {"16777217", 0x4B800000U},
        {"3.4028235e38", 0x7F7FFFFFU},
        {"1.4e-45", 0x00000001U},
        {"-2.5", 0xC0200000U},
        // Just below the midpoint of two floats. Reading it as a double first lands exactly on the
        // midpoint and then rounds the other way; a float must be rounded once, from the text.
        {"1.00000017881393432617187499", 0x3F800001U},
    };
    for (const auto& exact : kExact) {
        float value = 7.0F;
        const auto result =
            fromChars(exact.text.data(), exact.text.data() + exact.text.size(), value);
        expectations.expect(result.ec == std::errc{} &&
                                result.ptr == exact.text.data() + exact.text.size() &&
                                std::bit_cast<std::uint32_t>(value) == exact.bits,
                            "a float reads to its correctly rounded bits", exact.text);
    }
    for (const std::string_view text : {"1e39", "-1e39", "1e-46"}) {
        float value = 7.0F;
        const auto result = fromChars(text.data(), text.data() + text.size(), value);
        expectations.expect(result.ec == std::errc::result_out_of_range &&
                                result.ptr == text.data() + text.size() && value == 7.0F,
                            "a float magnitude outside the range is refused", text);
    }
}

void testGrammar(ExpectationContext& expectations) {
    struct Read final {
        std::string_view text;
        std::chars_format format;
        std::size_t consumed;
        double value;
    };
    static constexpr Read kReads[]{
        {"1.5abc", std::chars_format::general, 3, 1.5},
        {"1.", std::chars_format::general, 2, 1.0},
        {"1e", std::chars_format::general, 1, 1.0},
        {"1e+", std::chars_format::general, 1, 1.0},
        {"1e5x", std::chars_format::general, 3, 100000.0},
        {"0x10", std::chars_format::general, 1, 0.0},
        {"12 ", std::chars_format::general, 2, 12.0},
        {"1.5e3", std::chars_format::scientific, 5, 1500.0},
        {"1.5e3", std::chars_format::fixed, 3, 1.5},
        {"1.5", std::chars_format::fixed, 3, 1.5},
        {"1p4", std::chars_format::hex, 3, 16.0},
        {"a.8", std::chars_format::hex, 3, 10.5},
        {"-1.8p1", std::chars_format::hex, 6, -3.0},
        {"0x10", std::chars_format::hex, 1, 0.0},
    };
    for (const auto& read : kReads) {
        double value = kUntouched;
        const auto result =
            fromChars(read.text.data(), read.text.data() + read.text.size(), value, read.format);
        expectations.expect(result.ec == std::errc{} &&
                                result.ptr == read.text.data() + read.consumed &&
                                value == read.value,
                            "the longest matching prefix is read and nothing after it", read.text);
    }

    struct Refused final {
        std::string_view text;
        std::chars_format format;
    };
    static constexpr Refused kRefused[]{
        {"", std::chars_format::general},       {"+1", std::chars_format::general},
        {" 1", std::chars_format::general},     {".", std::chars_format::general},
        {"-", std::chars_format::general},      {"-.", std::chars_format::general},
        {"e5", std::chars_format::general},     {"abc", std::chars_format::general},
        {"1.5", std::chars_format::scientific}, {"1.5e", std::chars_format::scientific},
        {"p4", std::chars_format::hex},
    };
    for (const auto& refused : kRefused) {
        double value = kUntouched;
        const auto result = fromChars(
            refused.text.data(), refused.text.data() + refused.text.size(), value, refused.format);
        expectations.expect(result.ec == std::errc::invalid_argument &&
                                result.ptr == refused.text.data() && value == kUntouched,
                            "text that is not a number is refused at its first character",
                            refused.text);
    }

    struct Special final {
        std::string_view text;
        std::size_t consumed;
        bool infinite;
        bool negative;
    };
    static constexpr Special kSpecials[]{
        {"inf", 3, true, false},      {"-INF", 4, true, true},
        {"Infinity", 8, true, false}, {"infinit", 3, true, false},
        {"nan", 3, false, false},     {"NaN(abc_1)", 10, false, false},
        {"nan(", 3, false, false},    {"nan(a-b)", 3, false, false},
        {"-nan", 4, false, true},
    };
    for (const auto& special : kSpecials) {
        double value = kUntouched;
        const auto result =
            fromChars(special.text.data(), special.text.data() + special.text.size(), value);
        const bool kind = special.infinite ? std::isinf(value) : std::isnan(value);
        expectations.expect(result.ec == std::errc{} &&
                                result.ptr == special.text.data() + special.consumed && kind &&
                                std::signbit(value) == special.negative,
                            "infinity and not-a-number spellings are read", special.text);
    }
}

// Text longer than any fixed buffer.
void testLongText(ExpectationContext& expectations) {
    const std::string tiny = "0." + std::string(400, '0') + "1";
    double value = kUntouched;
    auto result = fromChars(tiny.data(), tiny.data() + tiny.size(), value);
    expectations.expect(result.ec == std::errc::result_out_of_range &&
                            result.ptr == tiny.data() + tiny.size() && value == kUntouched,
                        "a long fraction that rounds to zero is out of range");

    const std::string huge = "1" + std::string(400, '0');
    result = fromChars(huge.data(), huge.data() + huge.size(), value);
    expectations.expect(result.ec == std::errc::result_out_of_range && value == kUntouched,
                        "a long integer past the range is out of range");

    const std::string scaled = "1" + std::string(300, '0') + "e-300";
    result = fromChars(scaled.data(), scaled.data() + scaled.size(), value);
    expectations.expect(result.ec == std::errc{} && result.ptr == scaled.data() + scaled.size() &&
                            value == 1.0,
                        "a long significand with a cancelling exponent reads exactly");

    const std::string padded = std::string(500, '0') + "1.25";
    result = fromChars(padded.data(), padded.data() + padded.size(), value);
    expectations.expect(result.ec == std::errc{} && value == 1.25,
                        "leading zeros of any length are read");
}

[[nodiscard]] std::uint64_t nextRandom(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t mixed = state;
    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
    return mixed ^ (mixed >> 31U);
}

// Every finite value survives the shortest text std::to_chars writes for it, and the 17 (or 9)
// significant digit scientific form: a correctly rounding reader returns the same bits.
void testRoundTrips(ExpectationContext& expectations) {
    std::uint64_t state = 0x626C6F6F6DULL;
    std::array<char, 64> text{};
    std::size_t doubleFailures = 0;
    std::size_t floatFailures = 0;
    for (int index = 0; index < 200000; ++index) {
        const std::uint64_t bits = nextRandom(state);
        const double original = std::bit_cast<double>(bits);
        if (std::isfinite(original)) {
            for (const bool shortest : {true, false}) {
                const auto written =
                    shortest ? std::to_chars(text.data(), text.data() + text.size(), original)
                             : std::to_chars(text.data(), text.data() + text.size(), original,
                                             std::chars_format::scientific, 16);
                double restored = 0.0;
                const auto result = fromChars(text.data(), written.ptr, restored);
                if (written.ec != std::errc{} || result.ec != std::errc{} ||
                    result.ptr != written.ptr || std::bit_cast<std::uint64_t>(restored) != bits) {
                    ++doubleFailures;
                }
            }
        }
        const auto narrowBits = static_cast<std::uint32_t>(bits >> 32U);
        const float narrow = std::bit_cast<float>(narrowBits);
        if (std::isfinite(narrow)) {
            for (const bool shortest : {true, false}) {
                const auto written =
                    shortest ? std::to_chars(text.data(), text.data() + text.size(), narrow)
                             : std::to_chars(text.data(), text.data() + text.size(), narrow,
                                             std::chars_format::scientific, 8);
                float restored = 0.0F;
                const auto result = fromChars(text.data(), written.ptr, restored);
                if (written.ec != std::errc{} || result.ec != std::errc{} ||
                    result.ptr != written.ptr ||
                    std::bit_cast<std::uint32_t>(restored) != narrowBits) {
                    ++floatFailures;
                }
            }
        }
    }
    expectations.expect(doubleFailures == 0, "every double survives its written text");
    expectations.expect(floatFailures == 0, "every float survives its written text");
}

// A process locale whose decimal separator is a comma changes nothing. Where no such locale is
// installed the check still runs, in whatever locale is active.
void testLocaleIndependence(ExpectationContext& expectations) {
    const std::string previous = std::setlocale(LC_ALL, nullptr);
    for (const char* const name : {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8"}) {
        if (std::setlocale(LC_ALL, name) != nullptr) {
            break;
        }
    }
    const std::string_view text = "1.5,2";
    double value = kUntouched;
    const auto result = fromChars(text.data(), text.data() + text.size(), value);
    expectations.expect(result.ec == std::errc{} && result.ptr == text.data() + 3 && value == 1.5,
                        "the process locale does not change what is read");
    std::setlocale(LC_ALL, previous.c_str());
}

#if !defined(__APPLE__)
template <class Float>
[[nodiscard]] bool sameRead(const std::string& text, const std::chars_format format) noexcept {
    const char* const first = text.data();
    const char* const last = first + text.size();
    Float standard = Float{42};
    Float portable = Float{42};
    const auto expected = std::from_chars(first, last, standard, format);
    const auto actual =
        bloom::core::float_chars_detail::fromCharsPortable(first, last, portable, format);
    if (expected.ec != actual.ec || expected.ptr != actual.ptr) {
        return false;
    }
    if (std::isnan(standard) || std::isnan(portable)) {
        return std::isnan(standard) && std::isnan(portable) &&
               std::signbit(standard) == std::signbit(portable);
    }
    return std::memcmp(&standard, &portable, sizeof(Float)) == 0;
}

// The implementation macOS uses against the standard library's, over text made three ways:
// numbers written in every format, the same numbers cut short or with other characters put in,
// and short strings of the characters a number is made of. Reading is compared in the three
// decimal formats. The hexadecimal format is left to the tables above: one standard library reads
// a doubled exponent sign ("1p+-3") there, which the grammar does not allow.
void testAgainstStandardLibrary(ExpectationContext& expectations) {
    static constexpr std::array kWriteFormats{std::chars_format::general, std::chars_format::fixed,
                                              std::chars_format::scientific,
                                              std::chars_format::hex};
    static constexpr std::array kFormats{std::chars_format::general, std::chars_format::fixed,
                                         std::chars_format::scientific};
    static constexpr std::string_view kAlphabet = "0123456789.eE+-pPxXinfatyINFAN()_ ,";
    std::uint64_t state = 0x666C6F6174ULL;
    std::array<char, 512> buffer{};
    std::size_t compared = 0;
    std::string firstDifference;
    const auto compare = [&](const std::string& text) {
        for (const auto format : kFormats) {
            ++compared;
            if ((!sameRead<double>(text, format) || !sameRead<float>(text, format)) &&
                firstDifference.empty()) {
                firstDifference = text;
            }
        }
    };
    for (int index = 0; index < 60000; ++index) {
        const std::uint64_t bits = nextRandom(state);
        const double value = std::bit_cast<double>(bits);
        if (!std::isfinite(value)) {
            continue;
        }
        const auto writeFormat = kWriteFormats[nextRandom(state) % kWriteFormats.size()];
        const auto written =
            std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, writeFormat);
        if (written.ec != std::errc{}) {
            continue;
        }
        std::string text(buffer.data(), written.ptr);
        compare(text);
        // Cut short.
        compare(text.substr(0, nextRandom(state) % (text.size() + 1)));
        // One character replaced, and one inserted.
        std::string replaced = text;
        replaced[nextRandom(state) % replaced.size()] =
            kAlphabet[nextRandom(state) % kAlphabet.size()];
        compare(replaced);
        std::string inserted = text;
        inserted.insert(inserted.begin() +
                            static_cast<std::ptrdiff_t>(nextRandom(state) % (inserted.size() + 1)),
                        kAlphabet[nextRandom(state) % kAlphabet.size()]);
        compare(inserted);
    }
    for (int index = 0; index < 200000; ++index) {
        std::string text(nextRandom(state) % 10, ' ');
        for (auto& character : text) {
            character = kAlphabet[nextRandom(state) % kAlphabet.size()];
        }
        compare(text);
    }
    expectations.expect(compared > 0 && firstDifference.empty(),
                        "the portable reader agrees with std::from_chars on generated text",
                        firstDifference);
}
#endif

} // namespace

int main() {
    ExpectationContext expectations;
    for (const auto implementation : {Implementation::Public, Implementation::Portable}) {
        activeImplementation = implementation;
        testDoubleBits(expectations);
        testFloatBits(expectations);
        testGrammar(expectations);
        testLongText(expectations);
        testRoundTrips(expectations);
        testLocaleIndependence(expectations);
    }
#if !defined(__APPLE__)
    testAgainstStandardLibrary(expectations);
#endif
    return expectations.ok() ? EXIT_SUCCESS : EXIT_FAILURE;
}
