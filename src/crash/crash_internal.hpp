#pragma once

// Internal to the crash module: system facts for the text report, time formats, and a file that
// one process holds open and locked while it runs.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::crash::detail {

// "Linux x86_64", "macOS arm64", "Windows x86_64".
[[nodiscard]] std::string platformName();
// The operating system's own name and version: the distribution and kernel on Linux, the product
// version and build on macOS, the version and build number on Windows.
[[nodiscard]] std::string operatingSystemVersion();
// The build version, the configuration, the compiler and, when the build was given it, the source
// revision.
[[nodiscard]] std::string buildDescription();
[[nodiscard]] std::uint32_t currentProcessId() noexcept;

// "2026-11-20 10:31:02 +0700", the local time with its offset.
[[nodiscard]] std::string readableTime(std::chrono::system_clock::time_point time);
// "2026-11-20_10-31-02", the local time for a folder name.
[[nodiscard]] std::string folderTime(std::chrono::system_clock::time_point time);
[[nodiscard]] std::optional<std::chrono::system_clock::time_point>
parseFolderTime(std::string_view text);

// A file this process creates, holds open and locks for its own life. Another process can read
// it; while it is held, another process can neither lock nor remove it (heldByAnotherProcess()
// answers true). The lock goes with the process, so a file left behind by a program that exited
// or crashed is free.
class HeldFile final {
  public:
    HeldFile() = default;
    HeldFile(const HeldFile&) = delete;
    HeldFile& operator=(const HeldFile&) = delete;
    HeldFile(HeldFile&& other) noexcept;
    HeldFile& operator=(HeldFile&& other) noexcept;
    ~HeldFile();

    // Creates (or truncates) and locks the file. False when it cannot.
    [[nodiscard]] bool open(const std::filesystem::path& path);
    // Waits for the lock instead of refusing when another process holds it.
    [[nodiscard]] bool openWaiting(const std::filesystem::path& path);
    [[nodiscard]] bool isOpen() const noexcept;
    // Replaces the whole content, in place.
    bool replace(std::string_view content) noexcept;
    bool append(std::string_view content) noexcept;
    [[nodiscard]] std::uint64_t size() const noexcept;
    // Closes and, when asked, removes the file.
    void close(bool remove) noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    [[nodiscard]] bool openLocked(const std::filesystem::path& path, bool wait);
    std::filesystem::path path_;
    std::intptr_t handle_ = -1;
};

// Whether a running process holds the file (a HeldFile). False when the file is free or gone.
[[nodiscard]] bool heldByAnotherProcess(const std::filesystem::path& path);

} // namespace bloom::crash::detail
