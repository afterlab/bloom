#include <bloom/core/telemetry_counters.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/memory_headroom.hpp>
#include <bloom/runtime/memory_pressure_monitor.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The page-file declarations require the Windows SDK types first.
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

namespace bloom::runtime {
namespace {

// Every entry point that runs pool budget callbacks names why, so the evictions those callbacks
// make synchronously are attributed to that cause in the Profiler's pressure section: a changed
// ceiling or preference, a measured memory sample, or room made for live work.
using core::telemetry::EvictionCause;
using core::telemetry::EvictionCauseScope;

[[nodiscard]] std::size_t saturatedAdd(const std::size_t left, const std::size_t right) noexcept {
    return left + std::min(right, std::numeric_limits<std::size_t>::max() - left);
}

// Swap pressure is activity, not occupancy. A swap file can sit almost full for the life of the
// boot -- pages written long ago, untouched since -- while RAM is plentiful, so a static reading is
// never a reason to trim. Only sustained growth relative to the previous valid sample counts; the
// entry bar ignores small background churn, and once active a quarter-sized sustaining bar keeps a
// real episode latched without letting a borderline value flap every poll.
inline constexpr std::size_t kSwapGrowthMinimumBytes = std::size_t{16} * 1024U * 1024U;
inline constexpr std::size_t kSwapGrowthBytesPerSecond = std::size_t{4} * 1024U * 1024U;
inline constexpr std::size_t kSwapSustainDivisor = 4;

[[nodiscard]] std::size_t swapGrowthThreshold(const std::chrono::seconds elapsed,
                                              const bool sustaining) noexcept {
    const auto seconds =
        elapsed.count() > 0 ? static_cast<std::size_t>(elapsed.count()) : std::size_t{0};
    const auto rate = seconds > std::numeric_limits<std::size_t>::max() / kSwapGrowthBytesPerSecond
                          ? std::numeric_limits<std::size_t>::max()
                          : kSwapGrowthBytesPerSecond * seconds;
    const auto entry = std::max(kSwapGrowthMinimumBytes, rate);
    return sustaining ? std::max<std::size_t>(entry / kSwapSustainDivisor, 1U) : entry;
}

#if defined(__linux__)
[[nodiscard]] MachineMemorySample sampleProcMeminfo() {
    std::ifstream meminfo("/proc/meminfo");
    MachineMemorySample sample;
    std::optional<std::size_t> swapFree;
    std::string line;
    while (std::getline(meminfo, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const auto label = std::string_view(line).substr(0, colon);
        if (label != "MemAvailable" && label != "SwapTotal" && label != "SwapFree")
            continue;
        auto rest = std::string_view(line).substr(colon + 1);
        while (!rest.empty() && rest.front() == ' ')
            rest.remove_prefix(1);
        std::uint64_t kibibytes = 0;
        const auto parsed = std::from_chars(rest.data(), rest.data() + rest.size(), kibibytes);
        if (parsed.ec != std::errc{})
            continue;
        const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
                               kibibytes, std::numeric_limits<std::size_t>::max() / 1024U)) *
                           1024U;
        if (label == "MemAvailable")
            sample.availableBytes = bytes;
        else if (label == "SwapTotal")
            sample.swapTotalBytes = bytes;
        else
            swapFree = bytes;
    }
    if (swapFree)
        sample.swapUsedBytes = sample.swapTotalBytes - std::min(*swapFree, sample.swapTotalBytes);
    // This process's own swapped (or zram-compressed) anonymous memory.
    std::ifstream status("/proc/self/status");
    while (std::getline(status, line)) {
        if (!line.starts_with("VmSwap:"))
            continue;
        auto rest = std::string_view(line).substr(7);
        while (!rest.empty() && rest.front() == ' ')
            rest.remove_prefix(1);
        std::uint64_t kibibytes = 0;
        if (std::from_chars(rest.data(), rest.data() + rest.size(), kibibytes).ec == std::errc{})
            sample.processSwappedBytes =
                static_cast<std::size_t>(std::min<std::uint64_t>(
                    kibibytes, std::numeric_limits<std::size_t>::max() / 1024U)) *
                1024U;
        break;
    }
    return sample;
}
#endif

} // namespace

std::size_t physicalMemoryBytes() noexcept {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0)
        return 0;
    return static_cast<std::size_t>(status.ullTotalPhys);
#elif defined(__APPLE__)
    std::uint64_t bytes = 0;
    auto size = sizeof(bytes);
    return sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0
               ? static_cast<std::size_t>(bytes)
               : 0;
#else
    const auto pages = sysconf(_SC_PHYS_PAGES);
    const auto pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || pageSize <= 0)
        return 0;
    const auto pagesAsSize = static_cast<std::size_t>(pages);
    const auto pageSizeAsSize = static_cast<std::size_t>(pageSize);
    if (pagesAsSize > std::numeric_limits<std::size_t>::max() / pageSizeAsSize)
        return std::numeric_limits<std::size_t>::max();
    return pagesAsSize * pageSizeAsSize;
#endif
}

MachineMemorySample machineMemorySample() noexcept {
    MachineMemorySample sample;
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0)
        sample.availableBytes = static_cast<std::size_t>(status.ullAvailPhys);
    // Actual page-file usage, not commit charge (ullTotalPageFile includes physical RAM).
    const auto callback = [](LPVOID context, PENUM_PAGE_FILE_INFORMATION info, LPCWSTR) -> BOOL {
        auto& result = *static_cast<MachineMemorySample*>(context);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        result.swapTotalBytes =
            saturatedAdd(result.swapTotalBytes, info->TotalSize * system.dwPageSize);
        result.swapUsedBytes =
            saturatedAdd(result.swapUsedBytes, info->TotalInUse * system.dwPageSize);
        return TRUE;
    };
    static_cast<void>(K32EnumPageFilesW(callback, &sample));
    // Private bytes outside the working set are paged out (or not yet touched, which large pixel
    // buffers are not). Windows has no direct per-process page-file figure.
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters), // NOLINT
                                sizeof(counters)) != 0 &&
        counters.PrivateUsage > counters.WorkingSetSize)
        sample.processSwappedBytes =
            static_cast<std::size_t>(counters.PrivateUsage - counters.WorkingSetSize);
    static const HANDLE lowMemory = CreateMemoryResourceNotification(LowMemoryResourceNotification);
    BOOL low = FALSE;
    if (lowMemory != nullptr && QueryMemoryResourceNotification(lowMemory, &low) != 0)
        sample.memoryStall = low != FALSE;
#elif defined(__APPLE__)
    const auto host = mach_host_self();
    vm_statistics64_data_t statistics{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t pageSize = 0;
    if (host_page_size(host, &pageSize) == KERN_SUCCESS &&
        host_statistics64(
            host, HOST_VM_INFO64,
            reinterpret_cast<host_info64_t>(&statistics), // NOLINT(*-reinterpret-cast)
            &count) == KERN_SUCCESS) {
        // The Linux sampler reads MemAvailable, which includes reclaimable page cache. macOS keeps
        // most file cache in active/inactive pages, so free + inactive alone understates what the
        // system can hand back and falsely declares pressure on a machine that is merely caching.
        // Count free, inactive, speculative and purgeable pages, plus file-backed pages, which the
        // VM reclaims under pressure.
        auto pages = static_cast<std::size_t>(statistics.free_count);
        pages = saturatedAdd(pages, static_cast<std::size_t>(statistics.inactive_count));
        pages = saturatedAdd(pages, static_cast<std::size_t>(statistics.speculative_count));
        pages = saturatedAdd(pages, static_cast<std::size_t>(statistics.purgeable_count));
        pages = saturatedAdd(pages, static_cast<std::size_t>(statistics.external_page_count));
        sample.availableBytes = pages * pageSize;
    }
    mach_port_deallocate(mach_task_self(), host);
    xsw_usage swap{};
    auto size = sizeof(swap);
    if (sysctlbyname("vm.swapusage", &swap, &size, nullptr, 0) == 0) {
        sample.swapTotalBytes = static_cast<std::size_t>(swap.xsu_total);
        sample.swapUsedBytes = static_cast<std::size_t>(swap.xsu_used);
    }
    // The compressor is macOS's first stage of paging; compressed task memory is not resident.
    task_vm_info_data_t vmInfo{};
    mach_msg_type_number_t vmInfoCount = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&vmInfo), // NOLINT(*-reinterpret-cast)
                  &vmInfoCount) == KERN_SUCCESS)
        sample.processSwappedBytes = static_cast<std::size_t>(vmInfo.compressed);
    // kern.memorystatus_vm_pressure_level: 1 normal, 2 warning, 4 critical.
    int pressureLevel = 0;
    auto pressureSize = sizeof(pressureLevel);
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &pressureLevel, &pressureSize, nullptr,
                     0) == 0)
        sample.memoryStall = pressureLevel >= 2;
#else
#if defined(__linux__)
    try {
        sample = sampleProcMeminfo();
    } catch (...) {
        // Sampling is best effort even while the process is short of allocation headroom.
        sample = {};
    }
#endif
    if (!sample.availableBytes) {
        const auto pages = sysconf(_SC_AVPHYS_PAGES);
        const auto pageSize = sysconf(_SC_PAGE_SIZE);
        if (pages >= 0 && pageSize > 0)
            sample.availableBytes =
                static_cast<std::size_t>(pages) * static_cast<std::size_t>(pageSize);
    }
#endif
    return sample;
}

std::size_t availableMemoryBytes() noexcept {
    return machineMemorySample().availableBytes.value_or(0);
}

std::size_t startupAvailableMemoryBytes() noexcept {
    static const std::size_t latched = availableMemoryBytes();
    return latched;
}

std::size_t MemoryBudgetLedger::reserveForPhysicalMemory(const std::size_t) noexcept { return 0; }

std::size_t
MemoryBudgetLedger::usableByteBudgetForPhysicalMemory(const std::size_t physicalMemory) noexcept {
    return physicalMemory == 0 ? kFallbackUsableMemoryBudget : physicalMemory;
}

MemoryBudgetLedger::MemoryBudgetLedger(const std::size_t physicalMemory,
                                       const std::size_t availableMemory) noexcept
    : physicalMemory_(physicalMemory), defaultsResolved_(physicalMemory != 0),
      usableByteBudget_(usableByteBudgetForPhysicalMemory(physicalMemory)) {
    // Automatic budgets are selected from hardware capacity and the headroom preference,
    // independent of how much other work happens to be resident at startup.
    resolveAutomaticTotalLocked();
    state_.effectiveBytes = defaultTotalByteBudget_;
    state_.processingEffectiveBytes = usableByteBudget_;
    if (availableMemory != 0)
        state_.machine.availableBytes = availableMemory;
}

MemoryBudgetSample memoryBudgetSample(const CancellationToken& cancellation) noexcept {
    MemoryBudgetSample result;
    if (cancellation.isCancellationRequested())
        return result;
    result.physicalBytes = physicalMemoryBytes();
    result.machine = machineMemorySample();
    result.process = sampleProcessMemory(cancellation);
    return result;
}

MemoryBudgetAllocation
MemoryBudgetLedger::allocate(const std::optional<std::size_t> operationOverride,
                             const std::optional<std::size_t> previewOverride) const noexcept {
    const std::lock_guard lock(mutex_);
    // Preferences name independent pools. A nonzero value is the selected budget, even when the
    // two selections exceed the automatic total or physical capacity. Automatic pools are not
    // split by a fixed fraction: each may use all usable memory and they compete inside it
    // (finished preview frames evicted last). A real process constraint can separately reduce
    // reusable admission; it never rewrites this selection.
    const auto operation =
        operationOverride.value_or(0) != 0 ? *operationOverride : defaultTotalByteBudget_;
    const auto preview =
        previewOverride.value_or(0) != 0 ? *previewOverride : defaultTotalByteBudget_;

    return {.usableByteBudget = usableByteBudget_,
            .operationCacheByteBudget = operation,
            .previewFrameCacheByteBudget = preview};
}

std::size_t MemoryPoolHolding::hostAllocatedBytes() const noexcept {
    return saturatedAdd(hostBytes, hostPendingRetirementBytes);
}

void MemoryBudgetLedger::registerCache(const void* owner, const std::size_t ceiling,
                                       std::function<std::size_t()> retained,
                                       std::function<void(std::size_t)> applyBudget,
                                       const MemoryBudgetOwnerKind kind,
                                       const MemoryEvictionPriority priority) {
    std::function<MemoryPoolHolding()> holding;
    if (retained)
        holding = [retained = std::move(retained)] {
            return MemoryPoolHolding{.hostBytes = retained()};
        };
    registerCache(owner, ceiling, std::move(holding), std::move(applyBudget), kind, priority);
}

void MemoryBudgetLedger::registerCache(const void* owner, const std::size_t ceiling,
                                       std::function<MemoryPoolHolding()> holding,
                                       std::function<void(std::size_t)> applyBudget,
                                       const MemoryBudgetOwnerKind kind,
                                       const MemoryEvictionPriority priority) {
    const std::lock_guard lock(mutex_);
    const EvictionCauseScope cause(EvictionCause::Ceiling);
    const auto entry =
        caches_
            .insert_or_assign(
                owner, Cache{ceiling, kind, priority, std::move(holding), std::move(applyBudget)})
            .first;
    // The new owner initialized its storage before registration and is not published yet.
    // A finite shared constraint must also update existing participants immediately.
    if (hasPolled_) {
        try {
            if (processSampling_)
                applyProcessCeilingLocked();
            if (state_.processCacheCeilingBytes)
                applyBudgetsLocked();
            else
                entry->second.applyBudget(cacheByteBudgetLocked(entry->second));
        } catch (...) {
            caches_.erase(entry);
            throw;
        }
    }
}

void MemoryBudgetLedger::unregisterCache(const void* owner) {
    const std::lock_guard lock(mutex_);
    const EvictionCauseScope cause(EvictionCause::Ceiling);
    if (const auto found = caches_.find(owner); found != caches_.end()) {
        yieldTargets_.erase(&found->second);
        caches_.erase(found);
    }
    if (hasPolled_ && state_.processCacheCeilingBytes) {
        applyProcessCeilingLocked();
        applyBudgetsLocked();
    }
}

std::size_t MemoryBudgetLedger::setCacheCeiling(const void* owner, const std::size_t ceiling) {
    const std::lock_guard lock(mutex_);
    const EvictionCauseScope cause(EvictionCause::Ceiling);
    const auto found = caches_.find(owner);
    if (found == caches_.end())
        return ceiling;
    found->second.ceiling = ceiling;
    if (processSampling_)
        applyProcessCeilingLocked();
    const auto bytes = hasPolled_ ? cacheByteBudgetLocked(found->second) : ceiling;
    // Apply while still serialized with polls; a caller must not publish an old allowance after
    // a concurrent pressure callback has already reduced it.
    if (state_.processCacheCeilingBytes)
        applyBudgetsLocked();
    else
        found->second.applyBudget(bytes);
    return bytes;
}

std::size_t MemoryBudgetLedger::cacheByteBudgetLocked(const Cache& participant) const {
    if (participant.kind == MemoryBudgetOwnerKind::LiveWork)
        return participant.ceiling;
    if (state_.awaitingProcessSample)
        return 0;
    if (ramHeadroom_ && state_.machine.availableBytes) {
        // Usable-capacity retention. The free memory beyond headroom plus the hysteresis margin is
        // ONE pool shared by every reusable cache: each may grow by its share, so together they
        // never grow past it before the next sample (giving each pool the whole free memory let N
        // pools take N times it and push the machine into swap). This process's own swapped
        // memory is not available: a cache paged out to swap or zram keeps MemAvailable high while
        // the machine thrashes. When available falls below the headroom, every pool shrinks by its
        // share of the deficit plus the margin, below what it holds now; the caches evict to their
        // new budgets. Between headroom and headroom plus margin, and while the OS reports memory
        // stalls, pools hold and never grow.
        const auto holdings = reusableHoldingsLocked();
        std::size_t own = 0;
        std::size_t index = 0;
        long double retainedTotal = 0;
        for (std::size_t at = 0; at < holdings.size(); ++at) {
            if (holdings[at].first == &participant) {
                own = holdings[at].second.heldBytes;
                index = at;
            }
            retainedTotal += static_cast<long double>(holdings[at].second.heldBytes);
        }
        const auto pools = holdings.size();
        const auto available = poolAvailableLocked(state_.machine);
        const auto margin = memoryPressureMarginBytes(*ramHeadroom_);
        const auto restoreFloor = saturatedAdd(*ramHeadroom_, margin);
        // Pools never grow into memory already admitted to active work.
        const auto growthFloor = saturatedAdd(restoreFloor, activeWorkClaims_);
        // A severe memory stall counts only while available memory is already within the
        // headroom plus margin. Stalls, swap traffic or full swap while memory is plentiful are not
        // caused by these caches and never shrink them or block their growth.
        const bool stall = state_.machine.memoryStall && available < restoreFloor;
        std::size_t bytes = own;
        if (available >= growthFloor && !stall) {
            // During a RAM Preview fill free memory is left to the frames in flight and the
            // finished frames they become: Ordinary pools hold what they have (cycling their own
            // least recently used entries) and only FinishedPreview pools grow.
            std::size_t growing = pools;
            if (previewFills_ != 0) {
                growing = static_cast<std::size_t>(
                    std::count_if(holdings.begin(), holdings.end(), [](const auto& entry) {
                        return entry.second.priority == MemoryEvictionPriority::FinishedPreview;
                    }));
                if (participant.priority != MemoryEvictionPriority::FinishedPreview)
                    growing = 0;
            }
            if (growing != 0)
                bytes = saturatedAdd(own, (available - growthFloor) / growing);
        } else if ((available < *ramHeadroom_ || stall) && retainedTotal > 0) {
            // Only the deficit: what brings available memory back to headroom plus margin. Each
            // pool gives back its share, least recently used entries first; never a whole pool
            // (a user purge is the only full clear). During a RAM Preview fill the Ordinary pools
            // cover the deficit before finished preview frames do.
            std::vector<MemoryEvictionHolding> view;
            view.reserve(holdings.size());
            for (const auto& [cache, holding] : holdings)
                view.push_back(holding);
            const auto cuts = memoryEvictionShares(
                view, restoreFloor - available, previewFills_ != 0, MemoryEvictionRounding::Down);
            bytes = own - cuts[index];
        }
        if (state_.processCacheCeilingBytes)
            bytes = std::min(bytes, *state_.processCacheCeilingBytes);
        return bytes;
    }
    if (!state_.processCacheCeilingBytes)
        return participant.ceiling;
    const auto absoluteLimit = *state_.processCacheCeilingBytes;
    long double total = 0;
    std::size_t boundedTotal = 0;
    bool exceedsLimit = false;
    for (const auto& [owner, cache] : caches_) {
        if (cache.kind != MemoryBudgetOwnerKind::ReusableRetention)
            continue;
        total += static_cast<long double>(cache.ceiling);
        if (cache.ceiling > absoluteLimit - boundedTotal)
            exceedsLimit = true;
        boundedTotal += std::min(cache.ceiling, absoluteLimit - boundedTotal);
    }
    if (!exceedsLimit)
        return participant.ceiling;
    const auto limit = static_cast<long double>(absoluteLimit);
    // This split exists only for an observed OS limit. Registered targets remain exact when no
    // finite constraint exists; auxiliary pools never rewrite the selected preferences.
    auto remaining = absoluteLimit;
    for (const auto& [owner, cache] : caches_) {
        if (cache.kind != MemoryBudgetOwnerKind::ReusableRetention)
            continue;
        const auto scaled = static_cast<long double>(cache.ceiling) * limit / total;
        // Clamp before conversion, including platforms whose long double cannot represent every
        // size_t. Subtract each assigned share so rounding can never exceed the actual OS limit.
        const auto bytes = scaled >= static_cast<long double>(remaining)
                               ? std::min(cache.ceiling, remaining)
                               : std::min(cache.ceiling, static_cast<std::size_t>(scaled));
        if (&cache == &participant)
            return bytes;
        remaining -= bytes;
    }
    return 0;
}

std::size_t MemoryBudgetLedger::cacheByteBudget(const void* owner) const {
    const std::lock_guard lock(mutex_);
    const auto found = caches_.find(owner);
    return found == caches_.end() ? 0 : cacheByteBudgetLocked(found->second);
}

void MemoryBudgetLedger::setConfiguredTotal(const std::size_t bytes) {
    const std::lock_guard lock(mutex_);
    const EvictionCauseScope cause(EvictionCause::Ceiling);
    configuredTotalSet_ = true;
    state_.configuredBytes = bytes;
    if (!processSampling_)
        state_.effectiveBytes = bytes;
    else
        applyProcessCeilingLocked();
    applyBudgetsLocked();
}

bool MemoryBudgetLedger::observeSwapActivity(const MachineMemorySample& sample,
                                             const Clock::time_point now,
                                             const bool wasSwapPressure) noexcept {
    if (sample.swapTotalBytes == 0) {
        // No swap configured, or the platform cannot report it. Drop the baseline so a later valid
        // sample is measured from scratch instead of from a stale reading.
        lastSwapUsedBytes_.reset();
        lastSwapSampleTime_.reset();
        return false;
    }
    if (!lastSwapUsedBytes_ || !lastSwapSampleTime_ || now <= *lastSwapSampleTime_ ||
        sample.swapUsedBytes < *lastSwapUsedBytes_) {
        // The first valid sample, a clock that did not advance or moved backwards, or swap that was
        // partly released all rebaseline. Releasing from a peak means a later rise is measured from
        // the new low, not against the pre-release peak.
        lastSwapUsedBytes_ = sample.swapUsedBytes;
        lastSwapSampleTime_ = now;
        return false;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(now - *lastSwapSampleTime_);
    const auto growth = sample.swapUsedBytes - *lastSwapUsedBytes_;
    lastSwapUsedBytes_ = sample.swapUsedBytes;
    lastSwapSampleTime_ = now;
    return growth >= swapGrowthThreshold(elapsed, wasSwapPressure);
}

MemoryBudgetState MemoryBudgetLedger::pollHostLocked(MachineMemorySample sample,
                                                     const Clock::time_point now) {
    hasPolled_ = true;
    std::size_t retained = 0;
    for (const auto& [owner, cache] : caches_)
        retained = saturatedAdd(retained, cache.held().hostAllocatedBytes());
    state_.retainedBytes = retained;
    state_.memoryNotice = false;
    const bool wasSwap = state_.swapPressure;
    // Swap activity remains observable, but does not silently change selected cache budgets.
    const bool swapGrowing = observeSwapActivity(sample, now, wasSwap);
    if (sample.swapTotalBytes != 0)
        state_.swapPressure = swapGrowing;
    state_.swapNotice = state_.swapPressure && !wasSwap;
    state_.machine = sample;
    state_.machineAppliedAt = now;
    state_.retentionPercent = 100;
    if (!processSampling_) {
        state_.effectiveBytes = state_.configuredBytes;
        state_.memoryPressure = false;
        state_.memoryNotice = false;
    }
    return state_;
}

void MemoryBudgetLedger::setRamHeadroom(const std::size_t bytes) {
    const std::lock_guard lock(mutex_);
    const EvictionCauseScope cause(EvictionCause::Ceiling);
    if (ramHeadroom_ == bytes)
        return;
    ramHeadroom_ = bytes;
    resolveAutomaticTotalLocked();
    applyBudgetsLocked();
}

std::size_t MemoryBudgetLedger::reusableRetainedLocked() const {
    std::size_t retained = 0;
    for (const auto& [owner, cache] : caches_)
        if (cache.kind == MemoryBudgetOwnerKind::ReusableRetention)
            retained = saturatedAdd(retained, cache.held().hostAllocatedBytes());
    return retained;
}

std::size_t MemoryBudgetLedger::reusableRetainedBytes() const {
    const std::lock_guard lock(mutex_);
    return reusableRetainedLocked();
}

void MemoryBudgetLedger::setPoolProfile(const void* owner, MemoryPoolProfile profile) {
    const std::lock_guard lock(mutex_);
    if (const auto found = caches_.find(owner); found != caches_.end())
        found->second.profile = std::move(profile);
}

std::size_t MemoryBudgetLedger::copyProfilerPools(const std::span<ProfilerPool> out,
                                                  std::uint32_t& dropped) const {
    static constexpr std::string_view kUnnamedPool = "Unnamed pool";
    const std::lock_guard lock(mutex_);
    std::size_t count = 0;
    dropped = 0;
    for (const auto& [owner, cache] : caches_) {
        static_cast<void>(owner);
        if (cache.profile.rows) {
            const auto room = out.subspan(count);
            const auto filled = std::min(cache.profile.rows(room), room.size());
            count += filled;
            // A pool that had more rows than room reports filling all of it.
            if (room.empty())
                ++dropped;
            continue;
        }
        if (count == out.size()) {
            ++dropped;
            continue;
        }
        auto& row = out[count++];
        row = ProfilerPool{};
        setProfilerName(row.name, cache.profile.name.empty() ? kUnnamedPool : cache.profile.name);
        row.kind = cache.profile.kind;
        const auto held = cache.held();
        row.hostBytes = held.hostBytes;
        row.pendingBytes = held.hostPendingRetirementBytes;
        row.deviceBytes = held.deviceBytes;
        if (cache.ceiling != 0 && cache.ceiling != std::numeric_limits<std::size_t>::max())
            row.ceilingBytes = ProfilerBytes::of(cache.ceiling);
        if (cache.profile.counters != nullptr)
            cache.profile.counters->copyTo(row);
        if (cache.profile.entries)
            row.entries = ProfilerCount::of(cache.profile.entries());
    }
    return count;
}

MemoryPoolHolding MemoryBudgetLedger::reusableHolding() const {
    const std::lock_guard lock(mutex_);
    MemoryPoolHolding total;
    for (const auto& [owner, cache] : caches_) {
        if (cache.kind != MemoryBudgetOwnerKind::ReusableRetention)
            continue;
        const auto held = cache.held();
        total.hostBytes = saturatedAdd(total.hostBytes, held.hostBytes);
        total.hostPendingRetirementBytes =
            saturatedAdd(total.hostPendingRetirementBytes, held.hostPendingRetirementBytes);
        total.deviceBytes = saturatedAdd(total.deviceBytes, held.deviceBytes);
    }
    return total;
}

void MemoryBudgetLedger::applyBudgetsLocked() {
    // Budgets are computed from one retained view before any pool evicts, so an earlier pool's
    // eviction cannot change a later pool's share within the same application.
    std::vector<std::pair<const Cache*, std::size_t>> budgets;
    budgets.reserve(caches_.size());
    for (const auto& [owner, cache] : caches_)
        budgets.emplace_back(&cache, cacheByteBudgetLocked(cache));
    yieldTargets_.clear();
    const auto before = reusableRetainedLocked();
    for (const auto& [cache, bytes] : budgets)
        cache->applyBudget(bytes);
    const auto after = reusableRetainedLocked();
    if (after < before)
        releasedSinceTake_ = saturatedAdd(releasedSinceTake_, before - after);
}

MemoryBudgetState MemoryBudgetLedger::applyPressureSample(const MachineMemorySample& sample,
                                                          const Clock::time_point now) {
    const std::lock_guard lock(mutex_);
    const EvictionCauseScope cause(EvictionCause::MemoryPressure);
    static_cast<void>(pollHostLocked(sample, now));
    if (processSampling_)
        applyProcessCeilingLocked();
    applyBudgetsLocked();
    return state_;
}

std::size_t MemoryBudgetLedger::pressureDeficit(const MachineMemorySample& sample) const {
    const std::lock_guard lock(mutex_);
    if (!ramHeadroom_ || !sample.availableBytes)
        return 0;
    const auto available = poolAvailableLocked(sample);
    const auto deficit = available < *ramHeadroom_ ? *ramHeadroom_ - available : std::size_t{0};
    const auto restoreFloor =
        saturatedAdd(*ramHeadroom_, memoryPressureMarginBytes(*ramHeadroom_));
    return sample.memoryStall && available < restoreFloor
               ? std::max(deficit, restoreFloor - available)
               : deficit;
}

void MemoryBudgetLedger::requestHostTrim() noexcept {
    const std::lock_guard lock(mutex_);
    releasedSinceTake_ = std::numeric_limits<std::size_t>::max();
}

std::size_t MemoryBudgetLedger::takeReleasedBytes() noexcept {
    const std::lock_guard lock(mutex_);
    return std::exchange(releasedSinceTake_, 0);
}

std::size_t MemoryBudgetLedger::poolAvailableLocked(const MachineMemorySample& machine) const {
    auto available = effectiveAvailableBytes(machine);
    // Inside a memory-limited cgroup the machine can have plenty free while the process's own
    // domain is full: the room left there (limit minus usage that clean page cache cannot cover)
    // bounds what pools may hold.
    const auto& process = state_.process;
    for (std::size_t index = 0; index < process.domains.size(); ++index) {
        const auto& domain = process.domains[index];
        if (domain.kind != ProcessMemoryLimitKind::Finite || !domain.currentBytes)
            continue;
        const auto used = processMemoryDomainUnreclaimableBytes(process, index);
        available = std::min(available, domain.limitBytes - std::min(domain.limitBytes, used));
    }
    return available;
}

std::vector<std::pair<const MemoryBudgetLedger::Cache*, MemoryEvictionHolding>>
MemoryBudgetLedger::reusableHoldingsLocked() const {
    std::vector<std::pair<const Cache*, MemoryEvictionHolding>> holdings;
    holdings.reserve(caches_.size());
    for (const auto& [owner, cache] : caches_) {
        if (cache.kind != MemoryBudgetOwnerKind::ReusableRetention)
            continue;
        // Only reclaimable host bytes: pending retirement is already given back, and device
        // bytes are not host memory.
        holdings.emplace_back(&cache, MemoryEvictionHolding{.priority = cache.priority,
                                                            .heldBytes = cache.held().hostBytes});
    }
    return holdings;
}

std::size_t MemoryBudgetLedger::reclaim(const std::size_t bytes) {
    const std::lock_guard lock(mutex_);
    const EvictionCauseScope cause(EvictionCause::Admission);
    if (bytes == 0)
        return 0;
    auto holdings = reusableHoldingsLocked();
    std::erase_if(holdings, [](const auto& entry) { return !entry.first->holding; });
    std::vector<MemoryEvictionHolding> view;
    view.reserve(holdings.size());
    long double total = 0;
    for (const auto& [cache, holding] : holdings) {
        view.push_back(holding);
        total += static_cast<long double>(holding.heldBytes);
    }
    if (total <= 0)
        return 0;
    const bool fill = previewFills_ != 0;
    // Round each share up so the pools together give back at least `bytes`.
    const auto shares = memoryEvictionShares(view, bytes, fill, MemoryEvictionRounding::Up);
    const auto before = reusableRetainedLocked();
    for (std::size_t index = 0; index < holdings.size(); ++index) {
        // During a fill a pool asked for nothing keeps its budget (a finished-frame pool keeps
        // the room it was growing into); outside one every pool is bounded to what it holds.
        if (fill && shares[index] == 0)
            continue;
        holdings[index].first->applyBudget(holdings[index].second.heldBytes - shares[index]);
    }
    const auto after = reusableRetainedLocked();
    const auto released = before > after ? before - after : std::size_t{0};
    releasedSinceTake_ = saturatedAdd(releasedSinceTake_, released);
    yieldTargets_.clear();
    return released;
}

MemoryBudgetLedger::PreviewFill::PreviewFill(PreviewFill&& other) noexcept
    : ledger_(std::exchange(other.ledger_, nullptr)) {}

MemoryBudgetLedger::PreviewFill&
MemoryBudgetLedger::PreviewFill::operator=(PreviewFill&& other) noexcept {
    if (this != &other) {
        end();
        ledger_ = std::exchange(other.ledger_, nullptr);
    }
    return *this;
}

void MemoryBudgetLedger::PreviewFill::end() noexcept {
    if (auto* const ledger = std::exchange(ledger_, nullptr))
        ledger->endPreviewFill();
}

MemoryBudgetLedger::PreviewFill MemoryBudgetLedger::beginPreviewFill() {
    const std::lock_guard lock(mutex_);
    const EvictionCauseScope cause(EvictionCause::Admission);
    ++previewFills_;
    // Ordinary pools stop growing from the start of the fill, not from the next sample: a budget
    // granted before the fill would otherwise let them keep filling free memory meanwhile.
    if (previewFills_ == 1 && hasPolled_ && ramHeadroom_)
        applyBudgetsLocked();
    return PreviewFill(*this);
}

void MemoryBudgetLedger::endPreviewFill() noexcept {
    const std::lock_guard lock(mutex_);
    if (previewFills_ != 0)
        --previewFills_;
    if (previewFills_ == 0)
        yieldTargets_.clear();
}

bool MemoryBudgetLedger::previewFillActive() const {
    const std::lock_guard lock(mutex_);
    return previewFills_ != 0;
}

std::size_t MemoryBudgetLedger::yieldToPreview(const void* owner, const std::size_t bytes) {
    const std::lock_guard lock(mutex_);
    const EvictionCauseScope cause(EvictionCause::Admission);
    if (bytes == 0 || previewFills_ == 0 || !ramHeadroom_)
        return 0;
    const auto found = caches_.find(owner);
    if (found == caches_.end() || found->second.kind != MemoryBudgetOwnerKind::ReusableRetention ||
        found->second.priority != MemoryEvictionPriority::FinishedPreview)
        return 0;
    auto holdings = reusableHoldingsLocked();
    // Only Ordinary pools give here: finished frames never make room for other finished frames.
    std::erase_if(holdings, [](const auto& entry) {
        return entry.second.priority != MemoryEvictionPriority::Ordinary || !entry.first->holding;
    });
    // A pool that frees evicted entries asynchronously (the operation cache retires them after
    // their GPU work) reports them as pending retirement, outside the reclaimable bytes these
    // shares are taken from. As a second bound, since the last budget application a pool counts
    // as holding at most the budget it was last given here; the same bytes are never granted
    // twice.
    std::vector<MemoryEvictionHolding> view;
    view.reserve(holdings.size());
    std::size_t before = 0;
    std::size_t granted = 0;
    for (auto& [cache, holding] : holdings) {
        if (const auto target = yieldTargets_.find(cache); target != yieldTargets_.end())
            holding.heldBytes = std::min(holding.heldBytes, target->second);
        view.push_back(holding);
        before = saturatedAdd(before, cache->held().hostAllocatedBytes());
    }
    const auto shares = memoryEvictionShares(view, bytes, true, MemoryEvictionRounding::Up);
    for (std::size_t index = 0; index < holdings.size(); ++index) {
        if (shares[index] == 0)
            continue;
        // The pool evicts to this budget now; entries it retires asynchronously free shortly.
        const auto target = holdings[index].second.heldBytes - shares[index];
        yieldTargets_[holdings[index].first] = target;
        holdings[index].first->applyBudget(target);
        granted = saturatedAdd(granted, shares[index]);
    }
    std::size_t after = 0;
    for (const auto& [cache, holding] : holdings)
        after = saturatedAdd(after, cache->held().hostAllocatedBytes());
    if (before > after)
        releasedSinceTake_ = saturatedAdd(releasedSinceTake_, before - after);
    return granted;
}

void MemoryBudgetLedger::setActiveWorkClaims(const std::size_t bytes) {
    const std::lock_guard lock(mutex_);
    activeWorkClaims_ = bytes;
}

std::optional<std::size_t> MemoryBudgetLedger::ramHeadroom() const {
    const std::lock_guard lock(mutex_);
    return ramHeadroom_;
}

void MemoryBudgetLedger::beginProcessSampling() {
    const std::lock_guard lock(mutex_);
    const EvictionCauseScope cause(EvictionCause::MemoryPressure);
    if (processSampling_)
        return;
    // Production retention: freed cache buffers must leave the process, not stay in heap arenas.
    configureHostMemoryReturn();
    processSampling_ = true;
    hasPolled_ = true;
    state_.awaitingProcessSample = true;
    state_.effectiveBytes = 0;
    applyBudgetsLocked();
}

void MemoryBudgetLedger::updatePhysicalCapacity(const std::size_t physical) {
    if (physical == 0)
        return;
    physicalMemory_ = physical;
    usableByteBudget_ = usableByteBudgetForPhysicalMemory(physical);
    state_.processingEffectiveBytes = usableByteBudget_;
    if (!defaultsResolved_) {
        defaultsResolved_ = true;
        resolveAutomaticTotalLocked();
    }
}

void MemoryBudgetLedger::resolveAutomaticTotalLocked() noexcept {
    if (physicalMemory_ == 0) {
        defaultTotalByteBudget_ = kFallbackUsableMemoryBudget;
    } else {
        const auto headroom = ramHeadroom_.value_or(static_cast<std::size_t>(
            defaultHeadroomForCapacity(static_cast<std::uint64_t>(physicalMemory_))));
        defaultTotalByteBudget_ = physicalMemory_ - std::min(headroom, physicalMemory_);
    }
    if (!configuredTotalSet_)
        state_.configuredBytes = defaultTotalByteBudget_;
}

void MemoryBudgetLedger::applyProcessCeilingLocked() {
    const bool wasPressure = state_.memoryPressure;
    state_.effectiveBytes = state_.configuredBytes;
    if (state_.processCacheCeilingBytes)
        state_.effectiveBytes = std::min(state_.effectiveBytes, *state_.processCacheCeilingBytes);
    if (state_.awaitingProcessSample)
        state_.effectiveBytes = 0;
    state_.memoryPressure =
        !state_.awaitingProcessSample && state_.processCacheCeilingBytes &&
        (state_.configuredBytes > *state_.processCacheCeilingBytes ||
         (!ramHeadroom_ && std::any_of(caches_.begin(), caches_.end(), [this](const auto& entry) {
             const auto& cache = entry.second;
             return cache.kind == MemoryBudgetOwnerKind::ReusableRetention &&
                    cacheByteBudgetLocked(cache) < cache.ceiling;
         })));
    state_.memoryNotice = state_.memoryPressure && !wasPressure;
}

MemoryBudgetState MemoryBudgetLedger::poll(MachineMemorySample sample,
                                           const Clock::time_point now) {
    const std::lock_guard lock(mutex_);
    const EvictionCauseScope cause(EvictionCause::MemoryPressure);
    static_cast<void>(pollHostLocked(sample, now));
    applyBudgetsLocked();
    return state_;
}

MemoryBudgetState MemoryBudgetLedger::applySample(MemoryBudgetSample sample,
                                                  const Clock::time_point now) {
    const std::lock_guard lock(mutex_);
    const EvictionCauseScope cause(EvictionCause::MemoryPressure);
    updatePhysicalCapacity(sample.physicalBytes);
    if (!processSampling_) {
        static_cast<void>(pollHostLocked(sample.machine, now));
        applyBudgetsLocked();
        return state_;
    }
    const auto stamp = sample.process.sampledAt;
    // Only monotonic deliveries can authorize recovery; a superseded sample can still tighten.
    const auto ageSeconds = std::chrono::duration<long double>(now.time_since_epoch()).count() -
                            std::chrono::duration<long double>(stamp.time_since_epoch()).count();
    const bool fresh =
        ageSeconds >= 0 && ageSeconds <= 15 && (!lastProcessSample_ || stamp > *lastProcessSample_);
    state_.processSampleFresh = fresh;
    // Retained targets use the actual finite domain limit, not remaining headroom. A headroom
    // target would shrink as caches fill; retained gauges are not unique host ownership and must
    // never be added back as reclaimable credit. Live phase admission checks fresh usage
    // separately.
    std::optional<std::size_t> measured;
    for (const auto& domain : sample.process.domains) {
        if (domain.kind == ProcessMemoryLimitKind::Finite)
            measured = measured ? std::min(*measured, domain.limitBytes) : domain.limitBytes;
    }
    const bool complete = fresh && (isCompleteProcessMemorySample(sample.process) ||
                                    sample.process.status == ProcessMemoryStatus::Unsupported);
    processConstraintHistory_.recordFinite(
        sample.process, complete && sample.process.status == ProcessMemoryStatus::Finite);
    // Remove only constraints explicitly observed as unlimited in the same verified view.
    if (complete && processConstraintHistory_.verifiesRemoval(sample.process))
        state_.processCacheCeilingBytes.reset();
    if (measured) {
        if (!state_.processCacheCeilingBytes || *measured < *state_.processCacheCeilingBytes ||
            (complete && sample.process.status == ProcessMemoryStatus::Finite))
            state_.processCacheCeilingBytes = measured;
    }
    if (complete)
        state_.awaitingProcessSample = false;
    if (fresh)
        lastProcessSample_ = stamp;
    state_.process = std::move(sample.process);
    if (state_.sampleSerial != std::numeric_limits<std::uint64_t>::max())
        ++state_.sampleSerial;
    static_cast<void>(pollHostLocked(sample.machine, now));
    applyProcessCeilingLocked();
    applyBudgetsLocked();
    return state_;
}

MemoryBudgetState MemoryBudgetLedger::state() const {
    const std::lock_guard lock(mutex_);
    return state_;
}

namespace {
std::atomic<std::uint64_t> recordedDeviceLocalBytes{0};
} // namespace

void recordDeviceLocalMemoryBytes(const std::uint64_t bytes) noexcept {
    auto current = recordedDeviceLocalBytes.load(std::memory_order_relaxed);
    while (bytes > current && !recordedDeviceLocalBytes.compare_exchange_weak(
                                  current, bytes, std::memory_order_relaxed))
        ;
}

std::uint64_t defaultRamHeadroomBytes() noexcept {
    auto& ledger = processMemoryBudgetLedger();
    std::uint64_t capacity = ledger.physicalByteBudget();
    if (capacity == 0)
        capacity = physicalMemoryBytes();
    try {
        if (const auto limit = ledger.state().processCacheCeilingBytes)
            capacity = capacity == 0 ? *limit : std::min<std::uint64_t>(capacity, *limit);
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // Reading the cached state cannot fail in practice; physical memory alone remains.
    }
    return capacity == 0 ? kUnmeasuredHeadroomBytes : defaultHeadroomForCapacity(capacity);
}

std::uint64_t defaultVramHeadroomBytes() noexcept {
    const auto device = recordedDeviceLocalBytes.load(std::memory_order_relaxed);
    return device == 0 ? kUnmeasuredHeadroomBytes : defaultHeadroomForCapacity(device);
}

std::size_t memoryPressureMarginBytes(const std::size_t headroom) noexcept {
    constexpr std::size_t minimum = std::size_t{256} * 1024U * 1024U;
    return std::max(minimum, headroom / 8);
}

std::size_t effectiveAvailableBytes(const MachineMemorySample& sample) noexcept {
    const auto available = sample.availableBytes.value_or(0);
    return available - std::min(available, sample.processSwappedBytes);
}

MemoryBudgetLedger& processMemoryBudgetLedger() {
    static MemoryBudgetLedger ledger;
    return ledger;
}

std::size_t defaultOperationCacheByteBudget() noexcept {
    return processMemoryBudgetLedger().allocate().operationCacheByteBudget;
}

} // namespace bloom::runtime
