#include "crypto_library.hpp"

#include <bloom/update/ed25519.hpp>

#if defined(_WIN32)
#include <bloom/update/install_kind.hpp>

#include <mutex>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bloom::update {

#if defined(_WIN32)
namespace detail {

std::filesystem::path cryptoLibraryFile(const std::filesystem::path& programDirectory,
                                        const std::filesystem::path& buildDirectory) {
    constexpr const wchar_t* kName = L"libcrypto-3-x64.dll";
    std::error_code error;
    if (!programDirectory.empty()) {
        // A package carries the interpreter beside its programs, and nothing outside the
        // package is ever looked at for a program that has one.
        const auto home = programDirectory / L"python";
        if (std::filesystem::is_directory(home, error)) {
            const auto packaged = home / L"DLLs" / kName;
            return std::filesystem::is_regular_file(packaged, error) ? packaged
                                                                      : std::filesystem::path{};
        }
    }
    if (!buildDirectory.empty()) {
        const auto built = buildDirectory / kName;
        if (std::filesystem::is_regular_file(built, error)) {
            return built;
        }
    }
    return {};
}

} // namespace detail

// The program imports libcrypto late (the linker's delay load), so that a program which never
// checks an update needs no library file, and so that the file is taken from one known place and
// never from the search path. It is loaded here by its full name before anything of it is
// called; the late import then finds it already loaded.
bool cryptoLibraryReady() noexcept {
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] {
        try {
            std::filesystem::path build;
#if defined(BLOOM_UPDATE_CRYPTO_BUILD_DIRECTORY)
            build = std::filesystem::path(BLOOM_UPDATE_CRYPTO_BUILD_DIRECTORY);
#endif
            const auto file =
                detail::cryptoLibraryFile(currentExecutablePath().parent_path(), build);
            if (file.empty()) {
                return;
            }
            ready = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) !=
                    nullptr;
        } catch (...) {
            ready = false;
        }
    });
    return ready;
}
#else
bool cryptoLibraryReady() noexcept {
    return true;
}
#endif

} // namespace bloom::update
