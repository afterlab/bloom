#include <bloom/core/version.hpp>

#include <cstddef>
#include <string>
#include <string_view>

// The label reaches this file alone (src/core/CMakeLists.txt), so building under another label
// compiles one file.
#ifndef BLOOM_VERSION_LABEL
#error "BLOOM_VERSION_LABEL must name the label this build is released under"
#endif

namespace bloom::core {

namespace {

[[nodiscard]] bool allDigits(const std::string_view text) noexcept {
    if (text.empty())
        return false;
    for (const char character : text) {
        if (character < '0' || character > '9')
            return false;
    }
    return true;
}

[[nodiscard]] bool numericRelease(const std::string_view text) noexcept {
    // Three dot-separated numbers.
    std::size_t parts = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = text.find('.', start);
        const std::string_view part =
            text.substr(start, dot == std::string_view::npos ? dot : dot - start);
        if (!allDigits(part))
            return false;
        ++parts;
        if (dot == std::string_view::npos)
            break;
        start = dot + 1;
    }
    return parts == 3;
}

} // namespace

std::string_view version() noexcept { return BLOOM_VERSION_LABEL; }

std::string buildVersionOf(const std::string_view label) {
    const std::size_t hyphen = label.find('-');
    if (hyphen == std::string_view::npos)
        return std::string(label);
    const std::string_view release = label.substr(0, hyphen);
    const std::string_view preRelease = label.substr(hyphen + 1);
    if (!numericRelease(release))
        return std::string(label);
    struct Kind {
        std::string_view name;
        std::string_view compact;
    };
    static constexpr Kind kKinds[] = {{"alpha.", "a"}, {"beta.", "b"}, {"rc.", "rc"}};
    for (const Kind& kind : kKinds) {
        if (!preRelease.starts_with(kind.name))
            continue;
        const std::string_view number = preRelease.substr(kind.name.size());
        if (!allDigits(number))
            break;
        std::string compact(release);
        compact.append(kind.compact);
        compact.append(number);
        return compact;
    }
    return std::string(label);
}

std::string_view buildVersion() noexcept {
    // The text is built once. Out of memory here ends the program, as it would anywhere.
    static const std::string compact = buildVersionOf(version());
    return compact;
}

} // namespace bloom::core
