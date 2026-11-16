#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::update {

// A release label such as 0.1.0 or 0.1.0-alpha.1, read and ordered as Semantic Versioning 2.0.0
// says: 0.1.0-alpha.1 < 0.1.0-alpha.2 < 0.1.0-beta.1 < 0.1.0. Build metadata (after `+`) is
// accepted and takes no part in the order.
struct ReleaseVersion final {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
    std::vector<std::string> prerelease;

    [[nodiscard]] static std::optional<ReleaseVersion> parse(std::string_view text);

    // The release channel a label belongs to: its first pre-release word (`alpha`, `beta`), or
    // `stable` for a label with none.
    [[nodiscard]] std::string channel() const;
    [[nodiscard]] std::string text() const;

    friend std::strong_ordering operator<=>(const ReleaseVersion& first,
                                            const ReleaseVersion& second) noexcept;
    friend bool operator==(const ReleaseVersion& first, const ReleaseVersion& second) noexcept {
        return (first <=> second) == std::strong_ordering::equal;
    }
};

} // namespace bloom::update
