#pragma once

// The one runtime telemetry contract (issue #423): a Qt-free, fixed-capacity snapshot of Bloom's
// memory, GPU, CPU, workload, pressure and helper-process state. The Profiler panel, the benchmark
// harness (BENCH_MEMORY) and the agent surfaces (MCP/CLI `profiler.snapshot`) all read this type,
// so a number means the same thing everywhere.
//
// Data only. The type owns no heap memory (no std::string, std::vector or pointers) and is
// trivially copyable, so a sampler can publish it through a seqlock or triple buffer and a reader
// copies it with one memcpy, with no allocation per sample. Capacities are fixed; a sampler that
// sees more items than a capacity reports the first ones and counts the rest as dropped.
//
// Absent is not zero. Every value the platform or an unwired counter cannot supply is a
// ProfilerMetric whose `present` flag is false; readers show it as absent, never as 0. Values that
// come from a slower source (the ledger's machine sample, the GPU budget query) carry that source's
// age so a reader can show how old they are.
//
// Units: bytes, nanoseconds and counts are unsigned 64-bit; "cumulative" counters only grow from
// sampler start and a reader derives rates from two snapshots. Steady-clock values are nanoseconds
// on std::chrono::steady_clock.

#include <bloom/runtime/task_types.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace bloom::runtime {

// 2: pool counters and executor busy time can be absent (a source that does not count them).
// 3: pools report cumulative evicted bytes (ProfilerPool::evictedBytes).
// 4: this process's memory in swap (ProfilerMemory::processSwappedBytes).
inline constexpr std::uint32_t kProfilerSnapshotSchemaVersion = 4;

inline constexpr std::size_t kProfilerNameCapacity = 48;
inline constexpr std::size_t kProfilerMaximumPools = 32;
inline constexpr std::size_t kProfilerMaximumExecutorPools = 8;
inline constexpr std::size_t kProfilerMaximumTasks = 16;
inline constexpr std::size_t kProfilerMaximumHelpers = 8;

// A fixed, NUL-terminated name. Longer names are truncated at a byte boundary by setProfilerName.
using ProfilerName = std::array<char, kProfilerNameCapacity>;

inline void setProfilerName(ProfilerName& name, const std::string_view text) noexcept {
    const auto length = std::min(text.size(), name.size() - 1);
    std::copy_n(text.data(), length, name.data());
    std::fill(name.begin() + static_cast<std::ptrdiff_t>(length), name.end(), '\0');
}

[[nodiscard]] inline std::string_view profilerNameView(const ProfilerName& name) noexcept {
    const auto end = std::find(name.begin(), name.end(), '\0');
    return {name.data(), static_cast<std::size_t>(end - name.begin())};
}

// A value that may be absent. `present == false` means "not measured here", which is different
// from a measured zero.
template <typename T> struct ProfilerMetric final {
    T value{};
    bool present = false;

    [[nodiscard]] static constexpr ProfilerMetric of(const T measured) noexcept {
        return {measured, true};
    }
    [[nodiscard]] constexpr T valueOr(const T fallback) const noexcept {
        return present ? value : fallback;
    }
};

using ProfilerBytes = ProfilerMetric<std::uint64_t>;
using ProfilerCount = ProfilerMetric<std::uint64_t>;
using ProfilerNanoseconds = ProfilerMetric<std::int64_t>;
using ProfilerRatio = ProfilerMetric<float>;

// ---- Memory ---------------------------------------------------------------------------------

// What a ledger pool is for. Scratch pools hold memory only while work runs; cache pools retain
// reusable results; live pools hold data a running playback or export needs right now.
enum class ProfilerPoolKind : std::uint8_t { Scratch, Cache, Live };
inline constexpr std::size_t kProfilerPoolKindCount = 3;

struct ProfilerPool final {
    ProfilerName name{};
    ProfilerPoolKind kind = ProfilerPoolKind::Cache;
    // Host bytes the pool retains, host bytes evicted but still awaiting asynchronous retirement,
    // and device-resident bytes (attribution only; never charged to host budgets).
    std::uint64_t hostBytes = 0;
    std::uint64_t pendingBytes = 0;
    std::uint64_t deviceBytes = 0;
    // Absent when the pool's owner does not count entries.
    ProfilerCount entries{};
    // The pool's selected allowance; absent for a pool without a ceiling.
    ProfilerBytes ceilingBytes{};
    // Cumulative; absent when the pool's owner does not count them.
    ProfilerCount hits{};
    ProfilerCount misses{};
    ProfilerCount stores{};
    ProfilerCount evictions{};
    ProfilerBytes evictedBytes{};
    ProfilerCount refusals{};
};

struct ProfilerMemory final {
    // Resident set size, read at the sampler rate (statm or the platform equivalent).
    ProfilerBytes rssBytes{};
    // Resident anonymous, file-backed and shared memory, read at the sampler rate where the
    // platform splits them (Linux /proc/self/statm and /proc/self/status).
    ProfilerBytes anonBytes{};
    ProfilerBytes fileBytes{};
    ProfilerBytes shmemBytes{};
    // The machine.
    ProfilerBytes physicalBytes{};
    ProfilerBytes availableBytes{};
    ProfilerBytes swapTotalBytes{};
    ProfilerBytes swapUsedBytes{};
    // This process's own memory in swap (Linux VmSwap), read at the sampler rate.
    ProfilerBytes processSwappedBytes{};
    // Measured capacity minus the RAM headroom preference, and the headroom itself.
    ProfilerBytes headroomBytes{};
    ProfilerBytes usableBytes{};
    // The ledger: configured and effective reusable totals, and the host bytes every pool holds.
    ProfilerBytes ledgerConfiguredBytes{};
    ProfilerBytes ledgerEffectiveBytes{};
    ProfilerBytes ledgerRetainedBytes{};
    ProfilerMetric<bool> memoryPressure{};
    ProfilerMetric<bool> swapPressure{};

    std::uint32_t poolCount = 0;
    std::uint32_t droppedPools = 0;
    std::array<ProfilerPool, kProfilerMaximumPools> pools{};

    // The live host-staging scratch line: bytes staged right now and the peak since sampler start.
    ProfilerBytes stagingLiveBytes{};
    ProfilerBytes stagingPeakBytes{};
    // Bytes admission has granted to running work that are not yet in any pool.
    ProfilerBytes admissionClaimBytes{};
    // Anonymous bytes not explained by pools, pending retirement, staging and claims. The sampler
    // fills it with profilerUnattributedBytes(); signed, because attribution can overcount.
    ProfilerMetric<std::int64_t> unattributedBytes{};
    // Anonymous memory of an idle Bloom with an empty project: the floor unattributed is read
    // against.
    ProfilerBytes idleBaselineBytes{};
};

// ---- GPU ------------------------------------------------------------------------------------

struct ProfilerGpu final {
    // Bloom's own device allocations: live, and retired but awaiting the GPU work that reads them.
    ProfilerBytes liveBytes{};
    ProfilerBytes retiringBytes{};
    // Cumulative device admission refusals and reclaims.
    ProfilerCount refusals{};
    ProfilerCount reclaims{};
    // The driver's heap figures (VK_EXT_memory_budget); see gpuBudgetQueryAgeNs for their age.
    ProfilerBytes heapBudgetBytes{};
    ProfilerBytes heapUsageBytes{};
    // Cumulative transfer bytes and queue submissions.
    ProfilerBytes uploadBytes{};
    ProfilerBytes readbackBytes{};
    ProfilerCount submits{};
    // Device utilization in 0..1, where the platform reports one.
    ProfilerRatio utilization{};
};

// ---- CPU ------------------------------------------------------------------------------------

struct ProfilerExecutorPool final {
    ProfilerName name{};
    std::uint32_t workers = 0;
    std::uint32_t queueDepth = 0;
    // Cumulative nanoseconds the pool's workers spent running tasks, summed over workers; absent
    // when the executor does not measure it.
    ProfilerMetric<std::uint64_t> busyNs{};
};

struct ProfilerCpu final {
    // Cumulative user + system CPU time of the Bloom process.
    ProfilerMetric<std::uint64_t> processCpuNs{};
    std::uint32_t executorPoolCount = 0;
    std::array<ProfilerExecutorPool, kProfilerMaximumExecutorPools> executorPools{};
};

// ---- Workload -------------------------------------------------------------------------------

inline constexpr std::size_t kProfilerTaskStateCount = 5;
inline constexpr std::size_t kProfilerTaskOwnerKindCount = 5;
inline constexpr std::size_t kProfilerTaskExecutorCount = 3;
static_assert(static_cast<std::size_t>(TaskState::Failed) + 1 == kProfilerTaskStateCount);
static_assert(static_cast<std::size_t>(TaskOwnerKind::Export) + 1 == kProfilerTaskOwnerKindCount);
static_assert(static_cast<std::size_t>(TaskExecutor::Gpu) + 1 == kProfilerTaskExecutorCount);

struct ProfilerTask final {
    ProfilerName name{};
    TaskOwnerKind ownerKind = TaskOwnerKind::Application;
    std::uint64_t ownerId = 0;
    TaskExecutor executor = TaskExecutor::Cpu;
    TaskState state = TaskState::Queued;
    // 0..1; absent while the task's progress is indeterminate.
    ProfilerRatio progress{};
    // Time since the task was queued.
    std::int64_t ageNs = 0;
};

// The six timed pipeline stages.
enum class ProfilerStage : std::uint8_t {
    Decode,
    ColourConvert,
    Upload,
    Render,
    Transfer,
    Encode,
};
inline constexpr std::size_t kProfilerStageCount = 6;
static_assert(static_cast<std::size_t>(ProfilerStage::Encode) + 1 == kProfilerStageCount);

// Fixed log2 buckets. The edges are 16 us, 32 us, ... 16 us * 2^18 (about 4.19 s): bucket 0 holds
// durations under 16 us, bucket i (1..18) holds [edge(i-1), edge(i)), and the last bucket holds
// everything from about 4.19 s up.
inline constexpr std::int64_t kProfilerHistogramFirstEdgeNs = 16'000;
inline constexpr std::size_t kProfilerHistogramEdgeCount = 19;
inline constexpr std::size_t kProfilerHistogramBucketCount = kProfilerHistogramEdgeCount + 1;

[[nodiscard]] constexpr std::int64_t profilerHistogramEdgeNs(const std::size_t edge) noexcept {
    return kProfilerHistogramFirstEdgeNs << edge;
}

[[nodiscard]] constexpr std::size_t
profilerHistogramBucket(const std::int64_t durationNs) noexcept {
    std::size_t bucket = 0;
    while (bucket < kProfilerHistogramEdgeCount && durationNs >= profilerHistogramEdgeNs(bucket))
        ++bucket;
    return bucket;
}

struct ProfilerHistogram final {
    // Cumulative sample counts per bucket and the summed duration of every sample.
    std::array<std::uint64_t, kProfilerHistogramBucketCount> counts{};
    std::uint64_t totalNs = 0;
};

struct ProfilerWorkload final {
    // Every live task, counted by state, owner kind and executor (indexed by the enum values).
    std::array<std::uint32_t, kProfilerTaskStateCount> tasksByState{};
    std::array<std::uint32_t, kProfilerTaskOwnerKindCount> tasksByOwner{};
    std::array<std::uint32_t, kProfilerTaskExecutorCount> tasksByExecutor{};
    // The top tasks: running first, then queued, oldest first within each.
    std::uint32_t taskCount = 0;
    std::array<ProfilerTask, kProfilerMaximumTasks> tasks{};
    // Stage timings, indexed by ProfilerStage. A stage that is not instrumented is absent.
    std::array<ProfilerMetric<ProfilerHistogram>, kProfilerStageCount> stages{};
    // Playback: the painted rate, and dropped frames (cumulative). Absent while not playing.
    ProfilerRatio playbackFps{};
    ProfilerCount droppedFrames{};
    // RAM Preview: frames cached now and the fill rate in frames per second.
    ProfilerCount ramPreviewCachedFrames{};
    ProfilerRatio ramPreviewFillFps{};
    // Export throughput in frames per second; absent while nothing exports.
    ProfilerRatio exportFps{};
};

// ---- Pressure -------------------------------------------------------------------------------

// Why bytes left a cache: a pool over its ceiling, an OS memory-pressure trim, room made for
// admitted live work, or an explicit purge.
enum class ProfilerEvictionCause : std::uint8_t { Ceiling, MemoryPressure, Admission, Purge };
inline constexpr std::size_t kProfilerEvictionCauseCount = 4;
static_assert(static_cast<std::size_t>(ProfilerEvictionCause::Purge) + 1 ==
              kProfilerEvictionCauseCount);

struct ProfilerPressure final {
    // Cumulative admission waits and refusals.
    ProfilerCount admissionWaits{};
    ProfilerCount admissionRefusals{};
    // Cumulative evicted bytes, indexed by ProfilerEvictionCause.
    std::array<ProfilerBytes, kProfilerEvictionCauseCount> evictionBytes{};
};

// ---- Processes ------------------------------------------------------------------------------

struct ProfilerHelperProcess final {
    std::int64_t pid = 0;
    ProfilerName name{};
    ProfilerBytes rssBytes{};
    // Cumulative user + system CPU time.
    ProfilerMetric<std::uint64_t> cpuNs{};
    bool alive = false;
};

struct ProfilerProcesses final {
    std::uint32_t helperCount = 0;
    std::array<ProfilerHelperProcess, kProfilerMaximumHelpers> helpers{};
};

// ---- Snapshot -------------------------------------------------------------------------------

struct ProfilerSnapshot final {
    std::uint32_t schemaVersion = kProfilerSnapshotSchemaVersion;
    // Increments once per published sample; 0 means "never sampled".
    std::uint64_t serial = 0;
    // When the sample was taken, and the sampler's interval at that time.
    std::int64_t steadyTimestampNs = 0;
    std::int64_t intervalNs = 0;
    // Ages of the slower sources at sample time: the memory budget ledger's machine sample
    // (available memory, swap, the pressure flags and the ledger totals) and the GPU budget query.
    ProfilerNanoseconds ledgerSampleAgeNs{};
    ProfilerNanoseconds gpuBudgetQueryAgeNs{};

    ProfilerMemory memory{};
    ProfilerGpu gpu{};
    ProfilerCpu cpu{};
    ProfilerWorkload workload{};
    ProfilerPressure pressure{};
    ProfilerProcesses processes{};
};

static_assert(std::is_trivially_copyable_v<ProfilerSnapshot>,
              "ProfilerSnapshot is published by memcpy through a seqlock or triple buffer");
static_assert(std::is_standard_layout_v<ProfilerSnapshot>);

// Host bytes every reported pool holds, retained plus pending retirement.
[[nodiscard]] inline std::uint64_t profilerPoolHostBytes(const ProfilerMemory& memory) noexcept {
    std::uint64_t total = 0;
    const auto count = std::min<std::size_t>(memory.poolCount, memory.pools.size());
    for (std::size_t index = 0; index < count; ++index)
        total += memory.pools[index].hostBytes + memory.pools[index].pendingBytes;
    return total;
}

// Unattributed = anon - (pools + pending + staging + claims). Absent when anonymous memory is
// absent; an absent staging or claim counter contributes nothing. Negative when attribution
// overcounts anonymous memory.
[[nodiscard]] inline ProfilerMetric<std::int64_t>
profilerUnattributedBytes(const ProfilerMemory& memory) noexcept {
    if (!memory.anonBytes.present)
        return {};
    const auto attributed = profilerPoolHostBytes(memory) + memory.stagingLiveBytes.valueOr(0) +
                            memory.admissionClaimBytes.valueOr(0);
    return ProfilerMetric<std::int64_t>::of(static_cast<std::int64_t>(memory.anonBytes.value) -
                                            static_cast<std::int64_t>(attributed));
}

// Unattributed bytes above the idle baseline: the part of the gap an idle Bloom does not explain.
// Absent when either side is absent.
[[nodiscard]] inline ProfilerMetric<std::int64_t>
profilerUnattributedOverBaseline(const ProfilerMemory& memory) noexcept {
    const auto unattributed = profilerUnattributedBytes(memory);
    if (!unattributed.present || !memory.idleBaselineBytes.present)
        return {};
    return ProfilerMetric<std::int64_t>::of(
        unattributed.value - static_cast<std::int64_t>(memory.idleBaselineBytes.value));
}

} // namespace bloom::runtime
