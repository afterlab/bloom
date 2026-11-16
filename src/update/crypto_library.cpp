#include "crypto_library.hpp"

#include <bloom/update/ed25519.hpp>

#include <string>

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
        // A package carries the interpreter's modules beside its programs, and nothing outside
        // the package is ever looked at for a program that has them. (A folder named `python`
        // alone is not that: a build folder may hold one beside its test programs.)
        const auto modules = programDirectory / L"python" / L"DLLs";
        if (std::filesystem::is_directory(modules, error)) {
            const auto packaged = modules / kName;
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
namespace {

struct LibraryState final {
    bool ready = false;
    std::string problem;
};

const LibraryState& libraryState() noexcept {
    static std::once_flag once;
    static LibraryState state;
    std::call_once(once, [] {
        try {
            std::filesystem::path build;
#if defined(BLOOM_UPDATE_CRYPTO_BUILD_DIRECTORY)
            build = std::filesystem::path(BLOOM_UPDATE_CRYPTO_BUILD_DIRECTORY);
#endif
            const auto program = currentExecutablePath().parent_path();
            auto file = detail::cryptoLibraryFile(program, build);
            if (file.empty()) {
                state.problem = "libcrypto-3-x64.dll is not in python\\DLLs beside the program (" +
                                program.string() + ")";
                return;
            }
            // The loader wants an absolute name with its own separators.
            file.make_preferred();
            if (LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) != nullptr) {
                state.ready = true;
                return;
            }
            state.problem = file.string() + " cannot be loaded (system error " +
                            std::to_string(GetLastError()) + ")";
        } catch (...) {
            state.ready = false;
            state.problem = "the library file's name cannot be worked out";
        }
    });
    return state;
}

} // namespace

bool cryptoLibraryReady() noexcept {
    return libraryState().ready;
}

std::string cryptoLibraryProblem() {
    return libraryState().problem;
}
#else
bool cryptoLibraryReady() noexcept {
    return true;
}

std::string cryptoLibraryProblem() {
    return {};
}
#endif

} // namespace bloom::update
