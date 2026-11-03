#pragma once

// Process-wide, always-on telemetry counters (#423; docs/architecture/profiler-feed.md). They live
// in the core module because the code that counts them spans render, media, output and runtime,
// and only the runtime profiler sampler reads them.
//
// The always-on cost is a relaxed atomic add (two for a stage completion: its bucket and its
// summed duration) on paths that already do the work, plus the steady-clock reads that time a
// stage. Nothing here allocates, locks or formats; readers copy the values into fixed slots.

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bloom::core::telemetry {

// ---- Stage timing ---------------------------------------------------------------------------

// The six timed pipeline stages. The order matches runtime::ProfilerStage.
enum class Stage : std::uint8_t { Decode, ColourConvert, Upload, Render, Transfer, Encode };
inline constexpr std::size_t kStageCount = 6;

// Fixed log2 buckets: bucket 0 holds durations under 16 us, bucket i (1..18) holds
// [16 us * 2^(i-1), 16 us * 2^i), and bucket 19 everything from 16 us * 2^18 (about 4.19 s) up.
inline constexpr std::int64_t kHistogramFirstEdgeNs = 16'000;
inline constexpr std::size_t kHistogramEdgeCount = 19;
inline constexpr std::size_t kHistogramBucketCount = kHistogramEdgeCount + 1;

[[nodiscard]] constexpr std::size_t histogramBucket(const std::int64_t durationNs) noexcept {
    if (durationNs < kHistogramFirstEdgeNs)
        return 0;
    const auto multiple = static_cast<std::uint64_t>(durationNs / kHistogramFirstEdgeNs);
    const auto width = static_cast<std::size_t>(std::bit_width(multiple));
    return width < kHistogramEdgeCount ? width : kHistogramEdgeCount;
}

// One completed stage: one relaxed add to its bucket and one to its summed duration.
void recordStage(Stage stage, std::int64_t durationNs) noexcept;

struct StageHistogram final {
    std::array<std::uint64_t, kHistogramBucketCount> counts{};
    std::uint64_t totalNs = 0;
};
void readStage(Stage stage, StageHistogram& out) noexcept;

// Times one stage from construction to destruction (two steady-clock reads, one record).
class StageTimer final {
  public:
    explicit StageTimer(Stage stage) noexcept;
    ~StageTimer();
    StageTimer(const StageTimer&) = delete;
    StageTimer& operator=(const StageTimer&) = delete;
    // Ends the stage now instead of at destruction (a later end or the destructor does nothing).
    void end() noexcept;
    // Drops the stage: a failed or cancelled attempt records nothing.
    void cancel() noexcept { running_ = false; }

  private:
    std::int64_t startedNs_ = 0;
    Stage stage_;
    bool running_ = true;
};

// Nanoseconds on std::chrono::steady_clock.
[[nodiscard]] std::int64_t steadyNowNs() noexcept;

// ---- GPU transfers --------------------------------------------------------------------------

// Cumulative bytes copied to and from the device and queue submissions, counted where the copy
// or submit is recorded.
void addGpuUploadBytes(std::uint64_t bytes) noexcept;
void addGpuReadbackBytes(std::uint64_t bytes) noexcept;
void addGpuSubmit() noexcept;

struct GpuTransferTotals final {
    std::uint64_t uploadBytes = 0;
    std::uint64_t readbackBytes = 0;
    std::uint64_t submits = 0;
};
[[nodiscard]] GpuTransferTotals gpuTransferTotals() noexcept;

// The driver's device-local heap budget and usage (VK_EXT_memory_budget), published by a GPU
// owner thread when it queries them. Absent until the first publication.
void publishGpuHeapBudget(std::uint64_t budgetBytes, std::uint64_t usageBytes,
                          std::int64_t steadyNs) noexcept;
struct GpuHeapBudget final {
    bool present = false;
    std::uint64_t budgetBytes = 0;
    std::uint64_t usageBytes = 0;
    std::int64_t steadyNs = 0;
};
[[nodiscard]] GpuHeapBudget gpuHeapBudget() noexcept;

// ---- Host upload staging --------------------------------------------------------------------

// Host buffers that exist to move pixels to the GPU (scratch, ADR 0037): added when the buffer is
// allocated, subtracted when it is freed.
void stagingAllocated(std::uint64_t bytes) noexcept;
void stagingReleased(std::uint64_t bytes) noexcept;
struct StagingBytes final {
    std::uint64_t liveBytes = 0;
    std::uint64_t peakBytes = 0;
};
[[nodiscard]] StagingBytes stagingBytes() noexcept;
// Restarts the peak at the current live bytes (the sampler does this when it starts).
void resetStagingPeak() noexcept;

// ---- Memory pressure -------------------------------------------------------------------------

// Admission answers, counted where the admission policy answers a reservation: a request told to
// wait for capacity (a waiting request retries, so each retry that waits again counts) and a
// request refused because it can never fit the budget.
void countAdmissionWait() noexcept;
void countAdmissionRefusal() noexcept;
struct AdmissionTotals final {
    std::uint64_t waits = 0;
    std::uint64_t refusals = 0;
};
[[nodiscard]] AdmissionTotals admissionTotals() noexcept;

// Why bytes left a memory cache. The order matches runtime::ProfilerEvictionCause.
//  - Ceiling: a pool evicting to stay within its own allowance (on a store, or after its ceiling
//    or the memory preferences changed).
//  - MemoryPressure: the memory budget ledger shrinking pools after a measured memory sample.
//  - Admission: room made for live work (admission's reclaim, a RAM Preview fill).
//  - Purge: an explicit user purge.
enum class EvictionCause : std::uint8_t { Ceiling, MemoryPressure, Admission, Purge };
inline constexpr std::size_t kEvictionCauseCount = 4;

// The cause a memory pool's eviction on this thread is attributed to: the innermost live
// EvictionCauseScope, or Ceiling outside every scope (a pool evicting on its own). The memory
// budget ledger opens a scope around the budget callbacks it runs, which evict synchronously.
[[nodiscard]] EvictionCause currentEvictionCause() noexcept;
class EvictionCauseScope final {
  public:
    explicit EvictionCauseScope(EvictionCause cause) noexcept;
    ~EvictionCauseScope();
    EvictionCauseScope(const EvictionCauseScope&) = delete;
    EvictionCauseScope& operator=(const EvictionCauseScope&) = delete;

  private:
    EvictionCause previous_;
};

// Adds evicted host bytes under `cause`, or under the current cause.
void addEvictedBytes(EvictionCause cause, std::uint64_t bytes) noexcept;
void addEvictedBytes(std::uint64_t bytes) noexcept;
[[nodiscard]] std::array<std::uint64_t, kEvictionCauseCount> evictedBytesByCause() noexcept;

// ---- Profiler activity ----------------------------------------------------------------------

// Set while a profiler sampler runs. Owners of a slower query (the GPU heap budget) run it only
// while this is set; nothing else depends on it.
void setProfilerActive(bool active) noexcept;
[[nodiscard]] bool profilerActive() noexcept;

// ---- Helper processes -----------------------------------------------------------------------

inline constexpr std::size_t kHelperProcessSlots = 8;

struct HelperProcess final {
    std::int64_t pid = 0;
    // A static name ("Media worker", "Colour worker"); never null for a registered process.
    const char* name = nullptr;
    // The registering object, for an owner's own accessor (MediaWorkerPool::workerProcessIds).
    const void* owner = nullptr;
};

// A live helper process, registered for its lifetime. Claiming and releasing a slot is one CAS
// and a few stores at launch and exit; a process launched while every slot is taken is simply not
// listed. Move-only.
class HelperProcessRegistration final {
  public:
    HelperProcessRegistration() noexcept = default;
    // `name` must have static storage duration.
    HelperProcessRegistration(std::int64_t pid, const char* name, const void* owner) noexcept;
    HelperProcessRegistration(HelperProcessRegistration&& other) noexcept;
    HelperProcessRegistration& operator=(HelperProcessRegistration&& other) noexcept;
    HelperProcessRegistration(const HelperProcessRegistration&) = delete;
    HelperProcessRegistration& operator=(const HelperProcessRegistration&) = delete;
    ~HelperProcessRegistration() { reset(); }
    void reset() noexcept;
    [[nodiscard]] bool registered() const noexcept { return slot_ >= 0; }

  private:
    int slot_ = -1;
};

// Copies the registered helpers into `out` and returns how many were copied.
std::size_t readHelperProcesses(std::span<HelperProcess> out) noexcept;

} // namespace bloom::core::telemetry
