#pragma once

// What tests do to, and ask about, a child process by its id, spelled once for every platform:
// kill it from outside, ask whether it is gone, end the calling process abruptly, and the text a
// supervisor reports for a process that was killed this way.

#include <cstdint>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace bloom::test_support {

// The exit code a process killed by killProcess or endAbruptly reports on Windows, where there
// are no signals. Nine, so the number matches the POSIX signal in messages.
inline constexpr unsigned kKilledExitCode = 9;

// Ends the process `pid` at once, as SIGKILL does. True when the request was delivered.
[[nodiscard]] inline bool killProcess(const std::int64_t pid) {
#if defined(_WIN32)
    const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
        return false;
    const bool killed = TerminateProcess(process, kKilledExitCode) != 0;
    CloseHandle(process);
    return killed;
#else
    return ::kill(static_cast<pid_t>(pid), SIGKILL) == 0;
#endif
}

// Whether `pid`, a child this process started, has ended and been collected by its supervisor:
// nothing is left of it for this process to wait on.
[[nodiscard]] inline bool processCollected(const std::int64_t pid) {
#if defined(_WIN32)
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                       static_cast<DWORD>(pid));
    if (process == nullptr)
        return true;
    const bool ended = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
    return ended;
#else
    int status = 0;
    errno = 0;
    return ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG) == -1 && errno == ECHILD;
#endif
}

// Whether no process with this id is running.
[[nodiscard]] inline bool processGone(const std::int64_t pid) {
#if defined(_WIN32)
    return processCollected(pid);
#else
    return ::kill(static_cast<pid_t>(pid), 0) != 0;
#endif
}

[[nodiscard]] inline std::int64_t currentProcessId() {
#if defined(_WIN32)
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}

// Ends the calling process the way killProcess ends another one.
[[noreturn]] inline void endAbruptly() {
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), kKilledExitCode);
    for (;;) {
    }
#else
    ::raise(SIGKILL);
    for (;;) {
    }
#endif
}

// What describeProcessExit says for a process ended by killProcess or endAbruptly.
[[nodiscard]] inline std::string_view killedDescription() {
#if defined(_WIN32)
    return "exited with code 9";
#else
    return "signal 9";
#endif
}

} // namespace bloom::test_support
