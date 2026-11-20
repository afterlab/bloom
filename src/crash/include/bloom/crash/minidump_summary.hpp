#pragma once

// What a minidump says about the crash, read from its streams (the documented minidump format
// Crashpad writes on every platform): the process, its threads and their stacks, and the
// exception. The text report names the crash from it, and the tests check a crash's minidump with
// it. Reads a file; never call it on a UI thread.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bloom::crash {

struct MinidumpThread final {
    std::uint32_t id = 0;
    // Bytes of stack memory the minidump holds for the thread.
    std::uint32_t stackBytes = 0;
};

struct MinidumpSummary final {
    std::optional<std::uint32_t> processId;
    std::vector<MinidumpThread> threads;
    // The exception stream: the thread that crashed and the platform's exception code (a signal
    // number on Linux, an EXC_ code on macOS, an NTSTATUS on Windows).
    std::optional<std::uint32_t> crashedThreadId;
    std::optional<std::uint32_t> exceptionCode;
    // The exception's flags: on macOS, for a crash a signal caused (an abort), the signal.
    std::optional<std::uint32_t> exceptionFlags;
    // The system information stream's platform: "linux", "macos", "windows" or empty.
    std::string platform;
};

// Nothing when the file is not a minidump or a stream lies outside it.
[[nodiscard]] std::optional<MinidumpSummary> readMinidumpSummary(const std::filesystem::path& file);

// "SIGSEGV", "EXC_BAD_ACCESS", "EXCEPTION_ACCESS_VIOLATION" and the like, with the code.
[[nodiscard]] std::string describeException(const MinidumpSummary& summary);

} // namespace bloom::crash
