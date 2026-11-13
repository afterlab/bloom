#include <bloom/core/environment.hpp>
#include <bloom/platform/cache_directory.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// Sets a variable for one scope and puts the earlier value back.
class EnvironmentVariable final {
  public:
    explicit EnvironmentVariable(std::string name) : name_(std::move(name)) {
        const auto* existing = std::getenv(name_.c_str()); // NOLINT(concurrency-mt-unsafe)
        if (existing != nullptr)
            saved_ = existing;
    }
    ~EnvironmentVariable() {
        if (saved_.has_value())
            static_cast<void>(bloom::core::setEnvironmentVariable(name_.c_str(), saved_->c_str()));
        else
            static_cast<void>(bloom::core::unsetEnvironmentVariable(name_.c_str()));
    }
    EnvironmentVariable(const EnvironmentVariable&) = delete;
    EnvironmentVariable& operator=(const EnvironmentVariable&) = delete;
    void set(const std::string& value) const {
        static_cast<void>(bloom::core::setEnvironmentVariable(name_.c_str(), value.c_str()));
    }
    void unset() const { static_cast<void>(bloom::core::unsetEnvironmentVariable(name_.c_str())); }

  private:
    std::string name_;
    std::optional<std::string> saved_;
};

} // namespace

int main() {
    Expectations check;
    namespace platform = bloom::platform;

#if defined(_WIN32)
    // Windows keeps per-user caches under the local application data folder.
    EnvironmentVariable local("LOCALAPPDATA");
    local.set("C:/bloom-cache-directory-test-local");
    const auto fromLocal = platform::userCacheDirectory("bloom");
    check.expect(fromLocal.has_value() &&
                     *fromLocal ==
                         std::filesystem::path("C:/bloom-cache-directory-test-local/bloom/Cache"),
                 "the local application data folder gets the app subdirectory and Cache appended");
    check.expect(!platform::userCacheDirectory("").has_value(),
                 "an empty app subdirectory is refused");
    // #457: the GPU pipeline cache lives under the same per-user root.
    check.expect(platform::gpuPipelineCacheDirectory() ==
                     std::filesystem::path(
                         "C:/bloom-cache-directory-test-local/bloom/Cache/gpu-pipeline-cache"),
                 "the pipeline cache directory is a leaf of the Bloom cache root");
    local.unset();
    check.expect(!platform::userCacheDirectory("bloom").has_value(),
                 "no local application data folder returns nullopt rather than a relative path");
    check.expect(platform::gpuPipelineCacheDirectory().empty(),
                 "no cache root leaves the pipeline cache in memory");
#else
    EnvironmentVariable xdg("XDG_CACHE_HOME");
    EnvironmentVariable home("HOME");
    // #718: macOS keeps per-user caches under Library/Caches; Linux under .cache.
#if defined(__APPLE__)
    constexpr const char* kHomeCache = "/tmp/bloom-cache-directory-test-home/Library/Caches/bloom";
#else
    constexpr const char* kHomeCache = "/tmp/bloom-cache-directory-test-home/.cache/bloom";
#endif

    xdg.set("/tmp/bloom-cache-directory-test-xdg");
    const auto fromXdg = platform::userCacheDirectory("bloom");
    check.expect(fromXdg.has_value() &&
                     *fromXdg == std::filesystem::path("/tmp/bloom-cache-directory-test-xdg/bloom"),
                 "an absolute XDG_CACHE_HOME wins and gets the app subdirectory appended");

    xdg.set("relative/not-absolute");
    home.set("/tmp/bloom-cache-directory-test-home");
    const auto relativeXdgIgnored = platform::userCacheDirectory("bloom");
    check.expect(relativeXdgIgnored.has_value() &&
                     *relativeXdgIgnored ==
                         std::filesystem::path(kHomeCache),
                 "a relative XDG_CACHE_HOME is ignored in favor of the home cache folder");

    xdg.unset();
    const auto fromHome = platform::userCacheDirectory("bloom");
    check.expect(fromHome.has_value() &&
                     *fromHome ==
                         std::filesystem::path(kHomeCache),
                 "no XDG_CACHE_HOME falls back to the home cache folder");

    home.unset();
    const auto neither = platform::userCacheDirectory("bloom");
    check.expect(!neither.has_value(),
                 "no resolvable environment variable returns nullopt rather than a relative path");

    home.set("/tmp/bloom-cache-directory-test-home");
    const auto emptySubdirectory = platform::userCacheDirectory("");
    check.expect(!emptySubdirectory.has_value(), "an empty app subdirectory is refused");

    // #457: the GPU pipeline cache lives under the same per-user root.
    xdg.set("/tmp/bloom-cache-directory-test-xdg");
    check.expect(
        platform::gpuPipelineCacheDirectory() ==
            std::filesystem::path("/tmp/bloom-cache-directory-test-xdg/bloom/gpu-pipeline-cache"),
        "the pipeline cache directory is a leaf of the Bloom cache root");
    xdg.unset();
    home.unset();
    check.expect(platform::gpuPipelineCacheDirectory().empty(),
                 "no cache root leaves the pipeline cache in memory");

#endif

    return check.failures() == 0 ? 0 : 1;
}
