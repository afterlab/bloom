#pragma once

#include <cstdint>

namespace bloom::platform {
// Runs the calling thread at the operating system's idle priority for the scope's lifetime and
// restores what it changed when the scope ends (#495: background footage verification hashes on
// a shared I/O worker without competing with interactive work). Construct and destroy it on the
// same thread.
//  - Linux: the I/O class becomes IOPRIO_CLASS_IDLE (ioprio_set on the thread id), and the CPU
//    policy becomes SCHED_IDLE when the thread may leave SCHED_IDLE again. An unprivileged thread
//    may return to SCHED_OTHER only when RLIMIT_NICE permits its nice value, and a worker left
//    idle would starve every later task it runs, so without that permission the CPU policy is
//    left unchanged and only the I/O class is lowered.
//  - macOS: the thread enters Darwin's background state (setpriority PRIO_DARWIN_THREAD with
//    PRIO_DARWIN_BG: background CPU priority and throttled I/O), and leaves it afterwards.
//  - Windows: THREAD_MODE_BACKGROUND_BEGIN (CPU, I/O and memory priority), then
//    THREAD_MODE_BACKGROUND_END.
// Every step is best effort: a refused change leaves that part of the thread as it was, and the
// accessors say what was applied. `threadEndsWithScope` declares that the calling thread exits
// when the scope ends (a dedicated thread): Linux then applies SCHED_IDLE even when the thread
// could not leave it again, and does not restore the CPU policy.
class IdleThreadPriorityScope final {
  public:
    explicit IdleThreadPriorityScope(bool threadEndsWithScope = false) noexcept;
    ~IdleThreadPriorityScope();
    IdleThreadPriorityScope(const IdleThreadPriorityScope&) = delete;
    IdleThreadPriorityScope& operator=(const IdleThreadPriorityScope&) = delete;
    IdleThreadPriorityScope(IdleThreadPriorityScope&&) = delete;
    IdleThreadPriorityScope& operator=(IdleThreadPriorityScope&&) = delete;

    // Whether the scope lowered the thread's CPU scheduling (SCHED_IDLE, the Darwin background
    // state or Windows background mode).
    [[nodiscard]] bool cpuIdle() const noexcept { return cpuIdle_; }
    // Whether the scope lowered the thread's I/O priority (idle I/O class, the Darwin background
    // state or Windows background mode).
    [[nodiscard]] bool ioIdle() const noexcept { return ioIdle_; }

  private:
    bool cpuIdle_ = false;
    bool ioIdle_ = false;
#if defined(__linux__)
    bool restoreCpu_ = true;
    // The previous Linux state: CPU policy, its priority and the I/O priority value.
    int previousPolicy_ = 0;
    int previousPriority_ = 0;
    int previousIoPriority_ = 0;
#endif
};
} // namespace bloom::platform
