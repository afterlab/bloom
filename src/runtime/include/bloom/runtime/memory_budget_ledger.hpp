#pragma once

#include <bloom/runtime/memory_eviction_priority.hpp>
#include <bloom/runtime/memory_pool_counters.hpp>
#include <bloom/runtime/process_memory_sample.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bloom::runtime {

inline constexpr std::size_t kDefaultOperationCacheBytes = std::size_t{1} * 1024U * 1024U * 1024U;
inline constexpr std::size_t kMinimumPreviewFrameCacheByteBudget =
    std::size_t{2} * 1024U * 1024U * 1024U;
// The automatic total while physical capacity is still unmeasured. Explicit preferences never
// inherit it.
inline constexpr std::size_t kFallbackUsableMemoryBudget =
    kMinimumPreviewFrameCacheByteBudget + kDefaultOperationCacheBytes;

// Selected per-pool budgets. Real process constraints can separately reduce reusable admission.
struct MemoryBudgetAllocation final {
    std::size_t usableByteBudget = 0;
    std::size_t operationCacheByteBudget = 0;
    std::size_t previewFrameCacheByteBudget = 0;

    friend bool operator==(const MemoryBudgetAllocation&, const MemoryBudgetAllocation&) = default;
};

[[nodiscard]] std::size_t physicalMemoryBytes() noexcept;

// Missing availability is distinct from a measured zero. Swap counters are zero on a machine
// without swap or when the platform cannot report it; observed activity never rewrites targets.
struct MachineMemorySample final {
    std::optional<std::size_t> availableBytes;
    std::size_t swapTotalBytes = 0;
    std::size_t swapUsedBytes = 0;
    // This process's own memory that the OS paged out or compressed (Linux VmSwap, macOS
    // compressed task memory, Windows private bytes outside the working set). A cache page in swap
    // is memory the machine could not leave resident, so it never counts as available.
    std::size_t processSwappedBytes = 0;
    // The machine is stalling severely on memory right now: Linux PSI "full" stall time of at least
    // a tenth of the interval since the previous pressure sample (MemoryPressureMonitor), the macOS
    // VM pressure level, the Windows low-memory notification. It counts only while available
    // memory is within the headroom plus margin: then caches give back the deficit to headroom
    // plus margin and do not grow. Light stalls, swap occupancy and swap traffic never count.
    bool memoryStall = false;
};
[[nodiscard]] MachineMemorySample machineMemorySample() noexcept;
[[nodiscard]] std::size_t availableMemoryBytes() noexcept;
[[nodiscard]] std::size_t startupAvailableMemoryBytes() noexcept;

struct MemoryBudgetSample final {
    MachineMemorySample machine;
    std::size_t physicalBytes = 0;
    ProcessMemorySample process;
};
// Sampling is worker-only; applying a completed snapshot remains the owner's thread responsibility.
[[nodiscard]] MemoryBudgetSample
memoryBudgetSample(const CancellationToken& cancellation = {}) noexcept;

enum class MemoryBudgetOwnerKind { ReusableRetention, LiveWork };

// What one registered pool holds, by where the bytes live. Each distinct allocation is counted
// once, however many entries reference it, at its real allocation size.
struct MemoryPoolHolding final {
    // Host bytes the pool still retains and can give back by evicting.
    std::size_t hostBytes = 0;
    // Host bytes already evicted that stay allocated until their asynchronous retirement (after
    // the GPU work that reads them). Resident host memory, but not reclaimable a second time.
    std::size_t hostPendingRetirementBytes = 0;
    // Device-resident bytes. Reported for attribution; never charged to the host budgets.
    std::size_t deviceBytes = 0;
    // Everything the pool has allocated on the host: retained plus pending retirement.
    [[nodiscard]] std::size_t hostAllocatedBytes() const noexcept;
    friend bool operator==(const MemoryPoolHolding&, const MemoryPoolHolding&) = default;
};

struct MemoryBudgetState final {
    // Selected operation/preview total and that total within the observed absolute OS limit.
    // Auxiliary registered pools can share a finite limit; inspect owners for actual pool budgets.
    std::size_t configuredBytes = 0;
    std::size_t effectiveBytes = 0;
    // Host bytes every registered pool has allocated (MemoryPoolHolding::hostAllocatedBytes).
    std::size_t retainedBytes = 0;
    std::size_t reusableHostCreditBytes = 0;
    // Hardware capacity only. Live phase admission separately checks fresh usage and reservations.
    std::size_t processingEffectiveBytes = 0;
    ProcessMemorySample process;
    std::optional<std::size_t> processCacheCeilingBytes;
    bool awaitingProcessSample = false;
    bool processSampleFresh = false;
    std::uint64_t sampleSerial = 0;
    MachineMemorySample machine;
    // When the ledger applied `machine` (a process sample, a pressure sample or a poll); empty
    // before the first.
    std::optional<std::chrono::steady_clock::time_point> machineAppliedAt;
    // Compatibility field: named budgets no longer use a percentage retention policy.
    unsigned retentionPercent = 100;
    bool memoryPressure = false;
    bool swapPressure = false;
    bool memoryNotice = false;
    bool swapNotice = false;
};

class MemoryBudgetLedger final {
  public:
    using Clock = std::chrono::steady_clock;
    // Construction never probes the operating system. Explicit injected host capacity is useful
    // for standalone owners/tests; production activates bounded asynchronous process sampling.
    explicit MemoryBudgetLedger(std::size_t physicalMemory = 0,
                                std::size_t availableMemory = 0) noexcept;

    // A registered ceiling is that pool's selected allowance. Only an observed finite OS limit can
    // require reusable pools to divide a shared total. Live work is separately phase-admitted.
    // Registration/removal and callbacks are serialized, so removal waits for any in-progress
    // callback before destruction.
    // Poll on the UI thread when UI-owned pools are registered. Callbacks must not perform I/O,
    // wait for workers, or modify registrations. Cache locks must never enclose ledger calls.
    // `priority` orders which reusable pools give memory back first while a RAM Preview fill is
    // active (MemoryEvictionPriority); outside a fill it has no effect.
    // `retained` reports host bytes only, all of them reclaimable. A pool that retires evicted
    // entries asynchronously or holds device memory reports a MemoryPoolHolding instead: budgets,
    // eviction shares and yieldToPreview see only its reclaimable host bytes, host totals include
    // its pending retirement once, and device bytes never reach a host budget.
    void registerCache(const void* owner, std::size_t ceiling,
                       std::function<std::size_t()> retained,
                       std::function<void(std::size_t)> applyBudget,
                       MemoryBudgetOwnerKind kind = MemoryBudgetOwnerKind::ReusableRetention,
                       MemoryEvictionPriority priority = MemoryEvictionPriority::Ordinary);
    void registerCache(const void* owner, std::size_t ceiling,
                       std::function<MemoryPoolHolding()> holding,
                       std::function<void(std::size_t)> applyBudget,
                       MemoryBudgetOwnerKind kind = MemoryBudgetOwnerKind::ReusableRetention,
                       MemoryEvictionPriority priority = MemoryEvictionPriority::Ordinary);
    void unregisterCache(const void* owner);
    // How a registered pool appears in the Profiler (#423): its name, kind and counters, or its
    // rows. Read only by copyProfilerPools; budgets never see it. Unregistering drops it.
    void setPoolProfile(const void* owner, MemoryPoolProfile profile);
    // One row per registered pool (or per row a pool supplies): name, kind, host, pending and
    // device bytes, the registered ceiling and the pool's counters. A pool without a profile is
    // one unnamed row whose counters stay absent. Rows beyond `out` are counted in `dropped`.
    // Never allocates; same thread rules as poll (it asks every pool what it holds).
    [[nodiscard]] std::size_t copyProfilerPools(std::span<ProfilerPool> out,
                                                std::uint32_t& dropped) const;
    [[nodiscard]] std::size_t setCacheCeiling(const void* owner, std::size_t ceiling);
    [[nodiscard]] std::size_t cacheByteBudget(const void* owner) const;
    // Host bytes every reusable pool has allocated now, pending retirement included (asks each
    // pool; same thread rules as poll).
    [[nodiscard]] std::size_t reusableRetainedBytes() const;
    // The same, split into reclaimable host, pending retirement and device bytes.
    [[nodiscard]] MemoryPoolHolding reusableHolding() const;
    void setConfiguredTotal(std::size_t bytes);
    // The RAM headroom preference. Once set, each reusable pool may hold what it retains now plus
    // all measured available memory beyond the headroom: caches fill usable memory and are evicted
    // on demand (admission reclaimers, the next sample). An observed OS limit still applies.
    void setRamHeadroom(std::size_t bytes);
    [[nodiscard]] MemoryBudgetState poll(MachineMemorySample sample,
                                         Clock::time_point now = Clock::now());
    [[nodiscard]] MemoryBudgetState applySample(MemoryBudgetSample sample,
                                                Clock::time_point now = Clock::now());
    // A fast machine-only pressure sample (MemoryPressureMonitor): applies the reusable budgets at
    // once so caches evict without waiting for the next process sample. Same thread rules as poll.
    MemoryBudgetState applyPressureSample(const MachineMemorySample& sample,
                                          Clock::time_point now = Clock::now());
    // Bytes by which `sample` leaves the machine short of the RAM headroom (available memory minus
    // this process's swapped memory); under a severe stall with available memory inside headroom
    // plus margin, the shortfall to headroom plus margin. Zero without a headroom or a reading.
    [[nodiscard]] std::size_t pressureDeficit(const MachineMemorySample& sample) const;
    // Reusable bytes the budgets released since the last call (the monitor returns them to the OS).
    [[nodiscard]] std::size_t takeReleasedBytes() noexcept;
    // A full clear outside the budgets (a user purge) freed memory: the monitor trims at its next
    // pass. Thread-safe.
    void requestHostTrim() noexcept;
    // Evict-on-demand for active work: every reusable pool gives back its share of `bytes`
    // (proportional to what it holds, least recently used entries first), below what it holds
    // now. During a RAM Preview fill the Ordinary pools give first and finished preview frames only
    // the remainder (memoryEvictionShares). Caches are never a reason for work to wait. Same thread
    // rules as poll; returns the bytes the pools released.
    std::size_t reclaim(std::size_t bytes);

    // An active RAM Preview fill. While at least one is held, Ordinary pools yield before
    // FinishedPreview pools in budget deficits, in reclaim() and through yieldToPreview(). Ending
    // (or destroying) the last one restores proportional yielding. The first fill applies the
    // budgets at once (same thread rules as poll). Move-only; must not outlive the ledger.
    class PreviewFill final {
      public:
        PreviewFill() noexcept = default;
        PreviewFill(PreviewFill&& other) noexcept;
        PreviewFill& operator=(PreviewFill&& other) noexcept;
        PreviewFill(const PreviewFill&) = delete;
        PreviewFill& operator=(const PreviewFill&) = delete;
        ~PreviewFill() { end(); }
        void end() noexcept;
        [[nodiscard]] bool active() const noexcept { return ledger_ != nullptr; }

      private:
        friend class MemoryBudgetLedger;
        explicit PreviewFill(MemoryBudgetLedger& ledger) noexcept : ledger_(&ledger) {}
        MemoryBudgetLedger* ledger_ = nullptr;
    };
    [[nodiscard]] PreviewFill beginPreviewFill();
    [[nodiscard]] bool previewFillActive() const;
    // Preview first: a FinishedPreview pool that has no room for a finished frame asks here before
    // it evicts one of its own. During a fill, with a RAM headroom (usable-capacity retention), the
    // Ordinary pools give back up to `bytes` (least recently used entries first), and the caller
    // may raise its own budget by the returned bytes: what they evicted, including entries a pool
    // retires asynchronously, never the same bytes twice. The exchange is memory-neutral:
    // the preview pool never takes free memory or the headroom the next frame needs this way.
    // Returns 0 outside a fill, without a headroom, or for any other owner. Same thread rules as
    // poll; the caller must not hold its own cache lock.
    std::size_t yieldToPreview(const void* owner, std::size_t bytes);
    // Memory admitted to active work but not yet allocated (processing-phase claims minus their
    // materialized backing, which measured usage already includes) that reusable pools must not
    // grow into; applied with the next budgets. Thread-safe.
    void setActiveWorkClaims(std::size_t bytes);
    [[nodiscard]] std::optional<std::size_t> ramHeadroom() const;
    // Production activates this before constructing cache owners; it never gates live processing.
    void beginProcessSampling();
    [[nodiscard]] MemoryBudgetState state() const;

    // Reported physical capacity, or the automatic fallback when unavailable. This is not a clamp
    // on selected per-pool preferences and is not a whole-process allocation guarantee.
    [[nodiscard]] std::size_t usableByteBudget() const noexcept {
        const std::lock_guard lock(mutex_);
        return usableByteBudget_;
    }

    // Usable memory (ADR 0037): measured physical capacity minus the RAM headroom preference (its
    // default share of the capacity until a preference is applied); the fallback total while
    // capacity is unmeasured. There is no other fraction. Each automatic pool may use all of it;
    // the pools compete inside it, and an observed OS limit still applies.
    [[nodiscard]] std::size_t defaultTotalByteBudget() const noexcept {
        const std::lock_guard lock(mutex_);
        return defaultTotalByteBudget_;
    }

    // Compatibility accessor. Cache retention has no implicit host-memory reserve.
    [[nodiscard]] std::size_t reserveByteBudget() const noexcept {
        const std::lock_guard lock(mutex_);
        return reserveByteBudget_;
    }

    [[nodiscard]] std::size_t physicalByteBudget() const noexcept {
        const std::lock_guard lock(mutex_);
        return physicalMemory_;
    }

    // Missing/zero overrides select each pool's automatic default. Every nonzero override is exact
    // and independent of the other selection, physical capacity, and registered auxiliary pools.
    [[nodiscard]] MemoryBudgetAllocation
    allocate(std::optional<std::size_t> operationOverride = {},
             std::optional<std::size_t> previewOverride = {}) const noexcept;

    [[nodiscard]] static std::size_t
    usableByteBudgetForPhysicalMemory(std::size_t physicalMemory) noexcept;
    [[nodiscard]] static std::size_t reserveForPhysicalMemory(std::size_t physicalMemory) noexcept;

  private:
    struct Cache final {
        std::size_t ceiling;
        MemoryBudgetOwnerKind kind;
        MemoryEvictionPriority priority;
        std::function<MemoryPoolHolding()> holding;
        std::function<void(std::size_t)> applyBudget;
        MemoryPoolProfile profile{};
        [[nodiscard]] MemoryPoolHolding held() const {
            return holding ? holding() : MemoryPoolHolding{};
        }
    };
    [[nodiscard]] std::size_t cacheByteBudgetLocked(const Cache& cache) const;
    // Returns true only while swap is actively growing beyond the entry/sustain threshold. Static
    // occupancy, a first sample, a released sample, a non-advancing clock and missing counters all
    // rebaseline and assert nothing.
    [[nodiscard]] bool observeSwapActivity(const MachineMemorySample& sample, Clock::time_point now,
                                           bool wasSwapPressure) noexcept;
    void updatePhysicalCapacity(std::size_t physical);
    void resolveAutomaticTotalLocked() noexcept;
    void applyBudgetsLocked();
    [[nodiscard]] std::size_t reusableRetainedLocked() const;
    void applyProcessCeilingLocked();
    [[nodiscard]] MemoryBudgetState pollHostLocked(MachineMemorySample sample,
                                                   Clock::time_point now);
    std::size_t physicalMemory_;
    // Recursive only for a pool that reads the ledger back from its own budget callback.
    mutable std::recursive_mutex mutex_;
    std::map<const void*, Cache> caches_;
    MemoryBudgetState state_;
    // Swap is judged by change, never by occupancy: these carry the previous valid sample.
    std::optional<std::size_t> lastSwapUsedBytes_;
    std::optional<Clock::time_point> lastSwapSampleTime_;
    bool hasPolled_ = false;
    bool processSampling_ = false;
    std::optional<std::size_t> ramHeadroom_;
    std::size_t releasedSinceTake_ = 0;
    std::size_t activeWorkClaims_ = 0;
    std::size_t previewFills_ = 0;
    // The budget each Ordinary pool was last given by yieldToPreview, until budgets are applied
    // again: what it may still report beyond this is already on its way out.
    std::map<const Cache*, std::size_t> yieldTargets_;
    void endPreviewFill() noexcept;
    // What each reusable pool holds now, with its priority, for memoryEvictionShares.
    [[nodiscard]] std::vector<std::pair<const Cache*, MemoryEvictionHolding>>
    reusableHoldingsLocked() const;
    // Available memory as reusable pools see it: the machine's available memory minus this
    // process's swapped memory, bounded by the room left in any finite process/cgroup domain.
    [[nodiscard]] std::size_t poolAvailableLocked(const MachineMemorySample& machine) const;
    bool defaultsResolved_ = false;
    bool configuredTotalSet_ = false;
    std::optional<Clock::time_point> lastProcessSample_;
    ProcessMemoryConstraintHistory processConstraintHistory_;
    std::size_t usableByteBudget_ = 0;
    std::size_t defaultTotalByteBudget_ = 0;
    std::size_t reserveByteBudget_ = 0;
};

// Hysteresis above the RAM headroom. Under the headroom, reusable caches shrink until available
// memory is back above headroom plus this margin; between the two they hold and never grow, so a
// pool does not refill the memory it has just released.
[[nodiscard]] std::size_t memoryPressureMarginBytes(std::size_t headroom) noexcept;
// Available memory as the cache budgets see it: available minus this process's swapped memory.
[[nodiscard]] std::size_t effectiveAvailableBytes(const MachineMemorySample& sample) noexcept;

// One narrowly scoped process budget coordinator, shared by all windows and cache owners.
[[nodiscard]] MemoryBudgetLedger& processMemoryBudgetLedger();
[[nodiscard]] std::size_t defaultOperationCacheByteBudget() noexcept;

} // namespace bloom::runtime
