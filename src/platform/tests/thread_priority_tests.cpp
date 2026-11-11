// bloom.platform.thread-priority (#495): IdleThreadPriorityScope lowers the calling thread to the
// operating system's idle priority and restores it. On Linux the test reads the thread's I/O class
// and CPU policy inside and after the scope; on other platforms it checks only that the scope
// constructs and ends cleanly.
#include <bloom/platform/thread_priority.hpp>

#include <iostream>
#include <string>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

#if defined(__linux__)
int ioPriority() {
    return static_cast<int>(syscall(SYS_ioprio_get, 1, static_cast<long>(syscall(SYS_gettid))));
}
int policy() {
    int value = 0;
    sched_param parameter{};
    return pthread_getschedparam(pthread_self(), &value, &parameter) == 0 ? value : -1;
}
bool mayLeaveIdle() {
    rlimit limit{};
    const int nice = getpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)));
    return getrlimit(RLIMIT_NICE, &limit) == 0 &&
           (limit.rlim_cur == RLIM_INFINITY || static_cast<long long>(limit.rlim_cur) >= 20 - nice);
}

void linuxScope() {
    const int ioBefore = ioPriority();
    const int policyBefore = policy();
    {
        const bloom::platform::IdleThreadPriorityScope idle;
        expect(idle.ioIdle(), "the I/O class is lowered (no privilege needed)");
        expect((ioPriority() >> 13) == 3, "inside the scope the thread's I/O class is idle");
        expect(idle.cpuIdle() == (policyBefore != SCHED_IDLE && mayLeaveIdle()),
               "SCHED_IDLE is applied exactly when the thread may leave it again");
        expect((policy() == SCHED_IDLE) == (idle.cpuIdle() || policyBefore == SCHED_IDLE),
               "inside the scope the CPU policy is SCHED_IDLE when the scope applied it");
    }
    expect(ioPriority() == ioBefore, "the I/O priority is restored when the scope ends");
    expect(policy() == policyBefore, "the CPU policy is restored when the scope ends");
}
#endif
} // namespace

int main() {
    // A fresh thread, so a refused restore could never leak into the test's main thread.
    std::thread worker([] {
#if defined(__linux__)
        linuxScope();
        // A second scope on the same thread behaves the same once the first was restored.
        linuxScope();
#else
        const bloom::platform::IdleThreadPriorityScope idle;
        expect(idle.cpuIdle() == idle.ioIdle(),
               "one background state lowers the CPU and I/O priority together");
#endif
    });
    worker.join();
#if defined(__linux__)
    // A dedicated thread that ends with the scope enters SCHED_IDLE without any privilege.
    std::thread dedicated([] {
        const bloom::platform::IdleThreadPriorityScope idle(true);
        expect(idle.cpuIdle() && policy() == SCHED_IDLE,
               "a thread that ends with the scope runs at SCHED_IDLE");
        expect((ioPriority() >> 13) == 3, "and in the idle I/O class");
    });
    dedicated.join();
#endif
    return failures == 0 ? 0 : 1;
}
