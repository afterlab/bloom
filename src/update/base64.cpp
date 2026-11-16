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

std::string encodeBase64(const std::span<const std::uint8_t> bytes) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    text.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const std::size_t count = bytes.size() - offset < 3 ? bytes.size() - offset : 3;
        std::uint32_t group = static_cast<std::uint32_t>(bytes[offset]) << 16U;
        if (count > 1) {
            group |= static_cast<std::uint32_t>(bytes[offset + 1]) << 8U;
        }
        if (count > 2) {
            group |= static_cast<std::uint32_t>(bytes[offset + 2]);
        }
        text.push_back(alphabet[(group >> 18U) & 0x3fU]);
        text.push_back(alphabet[(group >> 12U) & 0x3fU]);
        text.push_back(count > 1 ? alphabet[(group >> 6U) & 0x3fU] : '=');
        text.push_back(count > 2 ? alphabet[group & 0x3fU] : '=');
    }
    return text;
}

} // namespace bloom::update
