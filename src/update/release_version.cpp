#include <bloom/update/release_version.hpp>

#include <algorithm>
#include <charconv>

namespace bloom::update {
namespace {

[[nodiscard]] bool isDigit(const char character) noexcept {
    return character >= '0' && character <= '9';
}

[[nodiscard]] bool isLetter(const char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

[[nodiscard]] bool isNumeric(const std::string_view identifier) noexcept {
    return !identifier.empty() && std::ranges::all_of(identifier, isDigit);
}

// A number without a sign or leading zeros that fits 32 bits.
[[nodiscard]] std::optional<std::uint32_t> readNumber(const std::string_view text) noexcept {
    if (!isNumeric(text) || (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// Dot-separated identifiers of letters, digits and hyphens; a numeric one has no leading zero
// when `strictNumbers` (pre-release identifiers; build metadata allows them).
[[nodiscard]] std::optional<std::vector<std::string>> readIdentifiers(std::string_view text,
                                                                      const bool strictNumbers) {
    std::vector<std::string> identifiers;
    while (true) {
        const auto dot = text.find('.');
        const auto identifier = text.substr(0, dot);
        if (identifier.empty() ||
            !std::ranges::all_of(identifier, [](const char character) {
                return isDigit(character) || isLetter(character) || character == '-';
            })) {
            return std::nullopt;
        }
        if (strictNumbers && isNumeric(identifier) && identifier.size() > 1 &&
            identifier.front() == '0') {
            return std::nullopt;
        }
        identifiers.emplace_back(identifier);
        if (dot == std::string_view::npos) {
            return identifiers;
        }
        text.remove_prefix(dot + 1);
    }
}

[[nodiscard]] std::strong_ordering compareIdentifiers(const std::string& first,
                                                      const std::string& second) noexcept {
    const bool firstNumeric = isNumeric(first);
    const bool secondNumeric = isNumeric(second);
    if (firstNumeric && secondNumeric) {
        // No leading zeros, so the longer number is the larger.
        if (first.size() != second.size()) {
            return first.size() <=> second.size();
        }
        return first.compare(second) <=> 0;
    }
    if (firstNumeric != secondNumeric) {
        return firstNumeric ? std::strong_ordering::less : std::strong_ordering::greater;
    }
    return first.compare(second) <=> 0;
}

} // namespace

std::optional<ReleaseVersion> ReleaseVersion::parse(std::string_view text) {
    ReleaseVersion version;
    if (const auto plus = text.find('+'); plus != std::string_view::npos) {
        if (!readIdentifiers(text.substr(plus + 1), false).has_value()) {
            return std::nullopt;
        }
        text = text.substr(0, plus);
    }
    if (const auto hyphen = text.find('-'); hyphen != std::string_view::npos) {
        auto identifiers = readIdentifiers(text.substr(hyphen + 1), true);
        if (!identifiers.has_value()) {
            return std::nullopt;
        }
        version.prerelease = std::move(*identifiers);
        text = text.substr(0, hyphen);
    }
    const auto firstDot = text.find('.');
    const auto secondDot =
        firstDot == std::string_view::npos ? firstDot : text.find('.', firstDot + 1);
    if (secondDot == std::string_view::npos) {
        return std::nullopt;
    }
    const auto major = readNumber(text.substr(0, firstDot));
    const auto minor = readNumber(text.substr(firstDot + 1, secondDot - firstDot - 1));
    const auto patch = readNumber(text.substr(secondDot + 1));
    if (!major.has_value() || !minor.has_value() || !patch.has_value()) {
        return std::nullopt;
    }
    version.major = *major;
    version.minor = *minor;
    version.patch = *patch;
    return version;
}

std::string ReleaseVersion::channel() const {
    if (prerelease.empty() || prerelease.front().empty() ||
        !std::ranges::all_of(prerelease.front(), isLetter)) {
        return "stable";
    }
    std::string name = prerelease.front();
    std::ranges::transform(name, name.begin(), [](const char character) {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                    : character;
    });
    return name;
}

std::string ReleaseVersion::text() const {
    std::string result =
        std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(patch);
    for (std::size_t index = 0; index < prerelease.size(); ++index) {
        result += index == 0 ? '-' : '.';
        result += prerelease[index];
    }
    return result;
}

std::strong_ordering operator<=>(const ReleaseVersion& first,
                                 const ReleaseVersion& second) noexcept {
    if (const auto order = first.major <=> second.major; order != 0) {
        return order;
    }
    if (const auto order = first.minor <=> second.minor; order != 0) {
        return order;
    }
    if (const auto order = first.patch <=> second.patch; order != 0) {
        return order;
    }
    // A pre-release comes before the release it leads to.
    if (first.prerelease.empty() != second.prerelease.empty()) {
        return first.prerelease.empty() ? std::strong_ordering::greater
                                        : std::strong_ordering::less;
    }
    const auto shared = std::min(first.prerelease.size(), second.prerelease.size());
    for (std::size_t index = 0; index < shared; ++index) {
        if (const auto order = compareIdentifiers(first.prerelease[index], second.prerelease[index]);
            order != 0) {
            return order;
        }
    }
    return first.prerelease.size() <=> second.prerelease.size();
}

} // namespace bloom::update
