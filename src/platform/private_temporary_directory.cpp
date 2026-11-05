// Owner-only temporary directories. See bloom/platform/private_temporary_directory.hpp.
#include <bloom/platform/private_temporary_directory.hpp>

#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <cstdint>
#include <random>
#else
#include <cstdlib>

#include <unistd.h>
#endif

namespace bloom::platform {

std::optional<std::filesystem::path>
createPrivateTemporaryDirectory(const std::string_view prefix) {
    if (prefix.find_first_of("/\\") != std::string_view::npos)
        return std::nullopt;
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    if (error || base.empty())
        return std::nullopt;
#if defined(_WIN32)
    // The per-user %TEMP% grants only this user; a new directory inherits that. create_directory
    // reports false for an existing path, so a name is never reused.
    std::random_device device;
    constexpr std::string_view digits = "0123456789abcdef";
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::uint64_t value = (static_cast<std::uint64_t>(device()) << 32U) | device();
        std::string name(prefix);
        for (int nibble = 0; nibble < 16; ++nibble, value >>= 4U)
            name.push_back(digits[value & 0xFU]);
        auto candidate = base / name;
        if (std::filesystem::create_directory(candidate, error) && !error)
            return candidate;
    }
    return std::nullopt;
#else
    // mkdtemp creates the directory exclusively with mode 0700 and an unpredictable suffix.
    const auto pattern = (base / (std::string(prefix) + "XXXXXX")).string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (::mkdtemp(buffer.data()) == nullptr)
        return std::nullopt;
    return std::filesystem::path(buffer.data());
#endif
}

} // namespace bloom::platform
