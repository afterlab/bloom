#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef BLOOM_UPDATE_FIXTURES
#error "BLOOM_UPDATE_FIXTURES must name the fixture directory"
#endif

namespace bloom::update::testing {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition) {
            return;
        }
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }

    [[nodiscard]] int result() const noexcept { return failures_ == 0 ? 0 : 1; }

  private:
    int failures_ = 0;
};

[[nodiscard]] inline std::filesystem::path fixturePath(const std::string_view name) {
    return std::filesystem::path(BLOOM_UPDATE_FIXTURES) / name;
}

[[nodiscard]] inline std::string readFixture(const std::string_view name) {
    std::ifstream file(fixturePath(name), std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

[[nodiscard]] inline std::string trimmedFixture(const std::string_view name) {
    auto text = readFixture(name);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

[[nodiscard]] inline std::span<const std::byte> asBytes(const std::string_view text) noexcept {
    return std::as_bytes(std::span(text.data(), text.size()));
}

[[nodiscard]] inline std::optional<std::vector<std::uint8_t>> fromHex(const std::string_view text) {
    if (text == "-") {
        return std::vector<std::uint8_t>{};
    }
    if (text.size() % 2 != 0) {
        return std::nullopt;
    }
    const auto digit = [](const char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        return -1;
    };
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const int high = digit(text[index]);
        const int low = digit(text[index + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<std::uint8_t>(high * 16 + low));
    }
    return bytes;
}

[[nodiscard]] inline std::string toBase64(const std::span<const std::uint8_t> bytes) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const auto remaining = bytes.size() - offset;
        std::uint32_t group = static_cast<std::uint32_t>(bytes[offset]) << 16U;
        if (remaining > 1) {
            group |= static_cast<std::uint32_t>(bytes[offset + 1]) << 8U;
        }
        if (remaining > 2) {
            group |= bytes[offset + 2];
        }
        text += alphabet[(group >> 18U) & 63U];
        text += alphabet[(group >> 12U) & 63U];
        text += remaining > 1 ? alphabet[(group >> 6U) & 63U] : '=';
        text += remaining > 2 ? alphabet[group & 63U] : '=';
    }
    return text;
}

} // namespace bloom::update::testing
