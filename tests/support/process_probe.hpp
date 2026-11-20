#pragma once

// What tests do to, and ask about, a child process by its id, spelled once for every platform:
// kill it from outside, ask whether it is gone, hold it to see its end (ProcessWatch), end the
// calling process abruptly, and the text a supervisor reports for a process that was killed this
// way.

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
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <time.h>
#endif
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

// A hold on one process, taken while it is known to be running. Its id names the process only
// until it is collected, after which the system may give the id to a new process; the hold names
// this process alone, so what it answers never depends on how soon an id is reused (#731).
// Windows: a process handle. Linux: a process file descriptor (pidfd). macOS: a kqueue that
// watches the process for its exit.
class ProcessWatch final {
  public:
    explicit ProcessWatch(const std::int64_t pid) : pid_(pid) {
#if defined(_WIN32)
        handle_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                              static_cast<DWORD>(pid));
#elif defined(__linux__)
        fd_ = static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0));
#elif defined(__APPLE__)
        queue_ = ::kqueue();
        if (queue_ >= 0) {
            struct kevent change{};
            EV_SET(&change, static_cast<uintptr_t>(pid), EVFILT_PROC, EV_ADD | EV_ONESHOT,
                   NOTE_EXIT, 0, nullptr);
            if (::kevent(queue_, &change, 1, nullptr, 0, nullptr) != 0) {
                ::close(queue_);
                queue_ = -1;
            }
        }
#endif
    }
    ProcessWatch(const ProcessWatch&) = delete;
    ProcessWatch& operator=(const ProcessWatch&) = delete;
    ~ProcessWatch() {
#if defined(_WIN32)
        if (handle_ != nullptr)
            CloseHandle(handle_);
#elif defined(__linux__)
        if (fd_ >= 0)
            ::close(fd_);
#elif defined(__APPLE__)
        if (queue_ >= 0)
            ::close(queue_);
#endif
    }

    // Whether the hold was taken: false when no process had the id.
    [[nodiscard]] bool valid() const noexcept {
#if defined(_WIN32)
        return handle_ != nullptr;
#elif defined(__linux__)
        return fd_ >= 0;
#elif defined(__APPLE__)
        return queue_ >= 0;
#else
        return false;
#endif
    }

    // Whether the process has ended. Never waits.
    [[nodiscard]] bool exited() {
#if defined(_WIN32)
        return handle_ != nullptr && WaitForSingleObject(handle_, 0) == WAIT_OBJECT_0;
#elif defined(__linux__)
        if (fd_ < 0)
            return false;
        pollfd ready{fd_, POLLIN, 0};
        return ::poll(&ready, 1, 0) == 1 && (ready.revents & POLLIN) != 0;
#elif defined(__APPLE__)
        if (!exited_ && queue_ >= 0) {
            struct kevent event{};
            const timespec now{0, 0};
            exited_ = ::kevent(queue_, nullptr, 0, &event, 1, &now) == 1 &&
                      (event.fflags & NOTE_EXIT) != 0;
        }
        return exited_;
#else
        return false;
#endif
    }

    // Whether the process, a child of this process, has ended and been collected by its
    // supervisor: nothing is left of it to wait on. On POSIX only this process's own children can
    // be waited on, so an id reused by an unrelated process still answers collected. Windows keeps
    // no ended child to collect: the end is the answer.
    [[nodiscard]] bool collected() {
        if (!exited())
            return false;
#if defined(_WIN32)
        return true;
#else
        int status = 0;
        errno = 0;
        return ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG) == -1 && errno == ECHILD;
#endif
    }

  private:
    std::int64_t pid_ = 0;
#if defined(_WIN32)
    HANDLE handle_ = nullptr;
#elif defined(__linux__)
    int fd_ = -1;
#elif defined(__APPLE__)
    int queue_ = -1;
    bool exited_ = false;
#endif
};

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
