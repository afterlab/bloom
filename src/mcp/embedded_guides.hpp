#pragma once

// Private to src/mcp: the user guides the MCP resources serve, embedded at build time from
// docs/user-guide (cmake/BloomEmbedText.cmake) so an installed server never reads the source tree.

#include <span>
#include <string_view>

namespace bloom::mcp {

struct EmbeddedGuide final {
    std::string_view name;
    std::string_view text;
};

[[nodiscard]] std::span<const EmbeddedGuide> embeddedGuides() noexcept;

} // namespace bloom::mcp
