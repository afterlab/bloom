#pragma once

#include <bloom/core/worker_thread.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

// Reacting to memory pressure between process samples. Reusable caches fill memory beyond the RAM
// headroom; when another process needs it back, waiting for the next five-second process sample
// (or for the OS to swap the caches out) is too late. The monitor samples the machine on its own
// thread at a short interval, wakes at once on a Linux PSI trigger, and hands any sample that
// leaves the machine short of the headroom to its owner, which applies it to the ledger so the
// caches evict right away. Freed cache memory is then returned to the OS.
namespace bloom::runtime {

// Makes freed large blocks go back to the OS. glibc's dynamic mmap threshold grows to the size of
// the largest freed block (up to 32 MiB), after which frame-sized buffers come from heap arenas
// and stay resident after free. A fixed threshold keeps every pixel/frame buffer an mmap that
// free() unmaps. Process-wide and idempotent; elsewhere a no-op. The macOS allocator has no such
// setting and keeps freed large blocks charged to the process, so there Bloom's pixel and frame
// buffers are mapped from the system directly (core::allocateHostBlock).
void configureHostMemoryReturn() noexcept;
// Returns free heap pages to the OS after a bulk eviction (glibc malloc_trim; the macOS
// allocator's pressure relief, which covers its small blocks only). Not on the UI thread: it can
// walk large arenas. A no-op where the allocator has no such call.
void releaseFreedHostMemory() noexcept;
// Cumulative system memory-pressure counters. Linux: PSI stall time from /proc/pressure/memory
// ("some": at least one task stalled; "full": all non-idle tasks stalled) and swap traffic from
// /proc/vmstat (pswpin/pswpout pages). Absent where the platform does not report them; macOS and
// Windows report pressure through MachineMemorySample::memoryStall instead.
struct MemoryPressureCounters final {
    std::optional<std::uint64_t> someStallMicroseconds;
    std::optional<std::uint64_t> fullStallMicroseconds;
    std::optional<std::uint64_t> swapInPages;
    std::optional<std::uint64_t> swapOutPages;
};
[[nodiscard]] MemoryPressureCounters memoryPressureCounters() noexcept;
// The process's resident memory in bytes, if the platform reports it (diagnostics and tests): the
// resident set on Linux, the working set on Windows, the physical footprint on macOS.
[[nodiscard]] std::optional<std::size_t> processResidentBytes() noexcept;

class MemoryPressureMonitor final {
  public:
    using Sampler = std::function<MachineMemorySample()>;
    using CounterSampler = std::function<MemoryPressureCounters()>;
    // Called on the monitor thread with a sample that leaves the machine short of the headroom.
    // The owner applies it with MemoryBudgetLedger::applyPressureSample on the thread that owns
    // the registered caches' callbacks.
    using Deliver = std::function<void(const MachineMemorySample&)>;

    struct Options final {
        std::chrono::milliseconds interval{250};
        Sampler sampler = machineMemorySample;
        CounterSampler counterSampler = memoryPressureCounters;
        // A severe stall: PSI "full" stall time (every non-idle task waiting on memory) of at least
        // this share of the wall time between two samples. Light "some" stalls, swap occupancy
        // and swap traffic are not pressure: zram systems keep stale swap and steady low stalls
        // while memory is plentiful (owner decision, 2026-10-31).
        double fullStallFraction = 0.10;
        // Bytes released by evictions before the monitor returns free heap to the OS.
        std::size_t trimThresholdBytes = std::size_t{32} * 1024U * 1024U;
        // Use a Linux PSI trigger to wake before the interval elapses.
        bool psiTrigger = true;
        // The time source for the stall fraction (default: the steady clock). A test passes a
        // manual clock.
        std::function<std::chrono::steady_clock::time_point()> now = [] {
            return std::chrono::steady_clock::now();
        };
        // False: no sampling thread; the owner drives each sampling pass with poll(). For tests.
        bool background = true;
    };

    MemoryPressureMonitor(MemoryBudgetLedger& ledger, Deliver deliver, Options options);
    MemoryPressureMonitor(MemoryBudgetLedger& ledger, Deliver deliver);
    ~MemoryPressureMonitor();
    MemoryPressureMonitor(const MemoryPressureMonitor&) = delete;
    MemoryPressureMonitor& operator=(const MemoryPressureMonitor&) = delete;

    // Joins the thread; no delivery starts after it returns.
    void stop() noexcept;
    // Samples now instead of at the next interval (a headroom change, for example).
    void wake() noexcept;
    // One sampling pass on the calling thread. Only for a monitor created with
    // `background = false`; it is not safe to call alongside a sampling thread.
    void poll() noexcept;
    [[nodiscard]] std::uint64_t deliveries() const noexcept { return deliveries_.load(); }
    [[nodiscard]] std::uint64_t trims() const noexcept { return trims_.load(); }
    [[nodiscard]] std::uint64_t samples() const noexcept { return samples_.load(); }

  private:
    void run();
    void samplePass() noexcept;
    [[nodiscard]] bool waitForNextSample(int psiDescriptor);

    MemoryBudgetLedger& ledger_;
    Deliver deliver_;
    Options options_;
    std::mutex mutex_;
    std::condition_variable wakeCondition_;
    bool stopping_ = false;
    bool woken_ = false;
    std::atomic<std::uint64_t> deliveries_{0};
    std::atomic<std::uint64_t> trims_{0};
    std::atomic<std::uint64_t> samples_{0};
    // State of the sampling pass: the thread's, or the poll() caller's.
    std::optional<MemoryPressureCounters> last_;
    std::chrono::steady_clock::time_point lastAt_;
    std::size_t releasedPending_ = 0;
    bloom::core::WorkerThread thread_;
};

} // namespace bloom::runtime
