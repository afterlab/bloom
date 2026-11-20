#include "process_signal_log.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <fstream>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#endif
#endif

namespace bloom::platform::detail {
namespace {
std::string utcNow() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()) %
                        1000;
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &seconds);
#else
    gmtime_r(&seconds, &parts);
#endif
    char text[40];
    std::snprintf(text, sizeof text, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", parts.tm_year + 1900,
                  parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec,
                  static_cast<int>(millis.count()));
    return text;
}
// The name the system reports for the target now. For a process group it is the group leader's,
// whose number the group carries.
std::string currentName(const std::int64_t target) {
#if defined(__linux__)
    std::ifstream comm("/proc/" + std::to_string(target) + "/comm");
    std::string name;
    if (!comm || !std::getline(comm, name))
        return "no such process";
    return name;
#elif defined(__APPLE__)
    char name[2 * MAXCOMLEN + 1] = {};
    if (::proc_name(static_cast<int>(target), name, sizeof name) <= 0)
        return "no such process";
    return name;
#else
    // Windows signals through the process and job handles it holds, which name no other process.
    static_cast<void>(target);
    return "held by handle";
#endif
}
std::int64_t selfId() {
#if defined(_WIN32)
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}
void append(const char* path, const std::string& line) {
#if defined(_WIN32)
    const std::filesystem::path file(path);
    const HANDLE handle = CreateFileW(file.c_str(), FILE_APPEND_DATA,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    static_cast<void>(
        WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr));
    CloseHandle(handle);
#else
    // One write of the whole line with O_APPEND, so lines from concurrent processes never mix.
    const int fd = ::open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    static_cast<void>(::write(fd, line.data(), line.size()));
    ::close(fd);
#endif
}
} // namespace

void recordProcessSignal(const std::int64_t target, const bool group, const std::string_view signal,
                         const std::string_view executable,
                         const std::string_view reason) noexcept {
    try {
        const char* path = std::getenv(kProcessSignalLogVariable); // NOLINT(concurrency-mt-unsafe)
        if (path == nullptr || *path == '\0')
            return;
        std::string line = utcNow();
        line += " process ";
        line += std::to_string(selfId());
        line += " sends ";
        line += signal;
        line += group ? " to process group " : " to process ";
        line += std::to_string(target);
        line += " (started as ";
        line += std::filesystem::path(std::string(executable)).filename().string();
        line += ", now ";
        line += currentName(target);
        line += "): ";
        line += reason;
        line += '\n';
        append(path, line);
    } catch (...) {
        // A diagnostic never changes what the supervisor does.
    }
}
} // namespace bloom::platform::detail
