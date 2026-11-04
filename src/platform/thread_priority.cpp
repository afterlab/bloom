#include <bloom/platform/thread_priority.hpp>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#elif defined(__APPLE__)
#include <sys/resource.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace bloom::platform {

#if defined(__linux__)
namespace {
// <linux/ioprio.h> is not part of every libc's headers; the ABI values are stable.
constexpr int kIoprioWhoProcess = 1;
constexpr int kIoprioClassShift = 13;
constexpr int kIoprioClassIdle = 3;

long threadId() noexcept { return syscall(SYS_gettid); }
int ioPriority() noexcept {
    return static_cast<int>(syscall(SYS_ioprio_get, kIoprioWhoProcess, threadId()));
}
bool setIoPriority(const int value) noexcept {
    return syscall(SYS_ioprio_set, kIoprioWhoProcess, threadId(), value) == 0;
}
// SCHED_IDLE counts as nice 20; leaving it needs RLIMIT_NICE to permit the thread's nice value
// (or CAP_SYS_NICE, which is not probed: without the limit the CPU policy is simply left alone).
bool mayLeaveIdlePolicy() noexcept {
    errno = 0;
    const int nice = getpriority(PRIO_PROCESS, static_cast<id_t>(threadId()));
    if (nice == -1 && errno != 0)
        return false;
    rlimit limit{};
    if (getrlimit(RLIMIT_NICE, &limit) != 0)
        return false;
    return limit.rlim_cur == RLIM_INFINITY || static_cast<long long>(limit.rlim_cur) >= 20 - nice;
}
} // namespace

IdleThreadPriorityScope::IdleThreadPriorityScope(const bool threadEndsWithScope) noexcept {
    const int io = ioPriority();
    if (io >= 0 && setIoPriority(kIoprioClassIdle << kIoprioClassShift)) {
        previousIoPriority_ = io;
        ioIdle_ = true;
    }
    sched_param parameter{};
    int policy = 0;
    if (pthread_getschedparam(pthread_self(), &policy, &parameter) != 0 || policy == SCHED_IDLE ||
        (!threadEndsWithScope && !mayLeaveIdlePolicy()))
        return;
    restoreCpu_ = !threadEndsWithScope;
    sched_param idle{};
    idle.sched_priority = 0;
    if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &idle) == 0) {
        previousPolicy_ = policy;
        previousPriority_ = parameter.sched_priority;
        cpuIdle_ = true;
    }
}

IdleThreadPriorityScope::~IdleThreadPriorityScope() {
    if (cpuIdle_ && restoreCpu_) {
        sched_param parameter{};
        parameter.sched_priority = previousPriority_;
        static_cast<void>(pthread_setschedparam(pthread_self(), previousPolicy_, &parameter));
    }
    if (ioIdle_)
        static_cast<void>(setIoPriority(previousIoPriority_));
}
#elif defined(__APPLE__)
// Darwin's background state for one thread: lower CPU priority and throttled I/O (the effect of
// the background quality-of-service class), entered and left through setpriority(2).
IdleThreadPriorityScope::IdleThreadPriorityScope(const bool) noexcept {
    if (getpriority(PRIO_DARWIN_THREAD, 0) != 0)
        return; // already in the background state; leave it to its owner
    if (setpriority(PRIO_DARWIN_THREAD, 0, PRIO_DARWIN_BG) == 0) {
        cpuIdle_ = true;
        ioIdle_ = true;
    }
}

IdleThreadPriorityScope::~IdleThreadPriorityScope() {
    if (cpuIdle_)
        static_cast<void>(setpriority(PRIO_DARWIN_THREAD, 0, 0));
}
#elif defined(_WIN32)
IdleThreadPriorityScope::IdleThreadPriorityScope(const bool) noexcept {
    // Fails when the thread is already in background mode; then nothing is ended either.
    if (SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0) {
        cpuIdle_ = true;
        ioIdle_ = true;
    }
}

IdleThreadPriorityScope::~IdleThreadPriorityScope() {
    if (cpuIdle_)
        static_cast<void>(SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END));
}
#else
IdleThreadPriorityScope::IdleThreadPriorityScope(const bool) noexcept {}
IdleThreadPriorityScope::~IdleThreadPriorityScope() = default;
#endif

} // namespace bloom::platform
