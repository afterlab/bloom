#pragma once

// Test fixture folders that belong to one process (#749). Two test runs on one machine (two build
// trees, or a developer's run beside a validation run) must never share a fixture folder, so a test
// never joins the system temporary directory with a fixed name. It uses one of these instead:
//
//   processTempDirectory()     one folder for the whole test process, made on first use and
//                              removed when the process exits normally. A test keeps its own fixed
//                              subfolder names under it:
//                                  const auto folder = processTempDirectory() / "bloom_exr_ingest";
//   UniqueTempDir{"prefix"}    a folder of its own, removed when the object is destroyed.
//
// Both are named "<prefix>-<process id>-<random hex>" under std::filesystem::temp_directory_path()
// and created exclusively: an existing name is never reused, so another process's folder is never
// written or removed. A folder left behind by a process that crashed is left alone too. A child
// process that should write into the same folder receives its path by argument or environment.
// Qt tests may keep using QTemporaryDir. This header is the only test source allowed to ask for the
// system temporary directory (the repository hygiene check, tools/quality/repository_checks).

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace bloom::test_support {

namespace detail {

[[nodiscard]] inline std::uint64_t currentProcessId() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

[[nodiscard]] inline std::string randomHex(const int digits) {
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::random_device device;
    std::string text;
    std::uint64_t value = 0;
    for (int index = 0; index < digits; ++index) {
        if (index % 8 == 0)
            value = device();
        text.push_back(kDigits[value & 0xFU]);
        value >>= 4U;
    }
    return text;
}

} // namespace detail

class UniqueTempDir {
  public:
    // Makes "<prefix>-<pid>-<random>" under the system temporary directory. Throws
    // std::runtime_error when no folder could be made: a fixture that has no folder cannot run.
    explicit UniqueTempDir(const std::string_view prefix) {
        if (prefix.empty() || prefix.find_first_of("/\\") != std::string_view::npos)
            throw std::invalid_argument("UniqueTempDir: the prefix must be a plain name");
        std::error_code error;
        const auto base = std::filesystem::temp_directory_path(error);
        if (error || base.empty())
            throw std::runtime_error("UniqueTempDir: no system temporary directory: " +
                                     error.message());
        const auto stem =
            std::string(prefix) + "-" + std::to_string(detail::currentProcessId()) + "-";
        for (int attempt = 0; attempt < 64; ++attempt) {
            auto candidate = base / (stem + detail::randomHex(12));
            // create_directory answers false for a path that already exists, so a name is used only
            // by the process that made it.
            if (std::filesystem::create_directory(candidate, error) && !error) {
                path_ = std::move(candidate);
                return;
            }
            if (error && error != std::errc::file_exists)
                break;
            error.clear();
        }
        throw std::runtime_error("UniqueTempDir: could not create a folder under " + base.string() +
                                 ": " + error.message());
    }

    UniqueTempDir(const UniqueTempDir&) = delete;
    UniqueTempDir& operator=(const UniqueTempDir&) = delete;
    UniqueTempDir(UniqueTempDir&& other) noexcept : path_(std::exchange(other.path_, {})) {}
    UniqueTempDir& operator=(UniqueTempDir&& other) noexcept {
        if (this != &other) {
            remove();
            path_ = std::exchange(other.path_, {});
        }
        return *this;
    }

    // Removes the folder and everything in it. A file another program still holds open (Windows
    // refuses to delete it) is retried a few times at once, then left and named on stderr; cleanup
    // alone never fails a test.
    ~UniqueTempDir() { remove(); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path operator/(const std::filesystem::path& child) const {
        return path_ / child;
    }

  private:
    void remove() noexcept {
        if (path_.empty())
            return;
        std::error_code error;
        for (int attempt = 0; attempt < 3; ++attempt) {
            error.clear();
            std::filesystem::remove_all(path_, error);
            if (!error)
                break;
        }
        if (error) {
            try {
                std::cerr << "note: test folder " << path_.string()
                          << " was not removed: " << error.message() << '\n';
            } catch (...) {
            }
        }
        path_.clear();
    }

    std::filesystem::path path_;
};

// The test process's own folder under the system temporary directory, made on first use and
// removed when the process exits normally (static destruction). Thread-safe.
[[nodiscard]] inline const std::filesystem::path& processTempDirectory() {
    static const UniqueTempDir directory{"bloom-test"};
    return directory.path();
}

} // namespace bloom::test_support
