#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bloom::update {

// The bytes a standard Base64 text (RFC 4648 section 4, with padding) stands for. Anything else,
// white space and the URL alphabet included, is no text: a signature has one spelling.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decodeBase64(std::string_view text);

// The same spelling of `bytes`: what `decodeBase64` gives these bytes back for.
[[nodiscard]] std::string encodeBase64(std::span<const std::uint8_t> bytes);

} // namespace bloom::update
