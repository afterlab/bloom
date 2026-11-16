#include <bloom/update/base64.hpp>

#include <array>

namespace bloom::update {
namespace {

[[nodiscard]] int valueOf(const char character) noexcept {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

} // namespace

std::optional<std::vector<std::uint8_t>> decodeBase64(const std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 4 * 3);
    for (std::size_t offset = 0; offset < text.size(); offset += 4) {
        const bool last = offset + 4 == text.size();
        std::array<int, 4> values{};
        int padding = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const char character = text[offset + index];
            if (character == '=') {
                // Padding ends the text, and never covers the first two characters of a group.
                if (!last || index < 2) {
                    return std::nullopt;
                }
                ++padding;
                values[index] = 0;
                continue;
            }
            if (padding != 0) {
                return std::nullopt;
            }
            values[index] = valueOf(character);
            if (values[index] < 0) {
                return std::nullopt;
            }
        }
        const auto group = static_cast<std::uint32_t>((values[0] << 18) | (values[1] << 12) |
                                                      (values[2] << 6) | values[3]);
        // The bits padding leaves unused are zero in the one spelling of a value.
        if ((padding == 1 && (group & 0xffU) != 0) || (padding == 2 && (group & 0xffffU) != 0)) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<std::uint8_t>(group >> 16U));
        if (padding < 2) {
            bytes.push_back(static_cast<std::uint8_t>((group >> 8U) & 0xffU));
        }
        if (padding < 1) {
            bytes.push_back(static_cast<std::uint8_t>(group & 0xffU));
        }
    }
    return bytes;
}

} // namespace bloom::update
