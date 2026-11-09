#include <bloom/core/telemetry_counters.hpp>

#include <chrono>

namespace bloom::core::telemetry {
namespace {

constexpr auto relaxed = std::memory_order_relaxed;

struct alignas(64) StageCounters final {
    std::array<std::atomic<std::uint64_t>, kHistogramBucketCount> counts{};
    std::atomic<std::uint64_t> totalNs{0};
};

struct HelperSlot final {
    // 0 free, -1 being claimed or released, otherwise the pid.
    std::atomic<std::int64_t> pid{0};
    std::atomic<const char*> name{nullptr};
    std::atomic<const void*> owner{nullptr};
};

struct Counters final {
    std::array<StageCounters, kStageCount> stages{};
    alignas(64) std::atomic<std::uint64_t> uploadBytes{0};
    std::atomic<std::uint64_t> readbackBytes{0};
    std::atomic<std::uint64_t> submits{0};
    alignas(64) std::atomic<std::uint64_t> deviceAllocations{0};
    std::atomic<std::uint64_t> deviceAllocatedBytes{0};
    std::atomic<std::uint64_t> deviceFrees{0};
    std::atomic<std::uint64_t> deviceFreedBytes{0};
    alignas(64) std::atomic<std::uint64_t> heapBudgetBytes{0};
    std::atomic<std::uint64_t> heapUsageBytes{0};
    // 0 until the first publication.
    std::atomic<std::int64_t> heapBudgetSteadyNs{0};
    alignas(64) std::atomic<std::uint64_t> stagingLive{0};
    std::atomic<std::uint64_t> stagingPeak{0};
    alignas(64) std::atomic<std::uint64_t> admissionWaits{0};
    std::atomic<std::uint64_t> admissionRefusals{0};
    std::array<std::atomic<std::uint64_t>, kEvictionCauseCount> evictedBytes{};
    alignas(64) std::atomic<bool> profilerActive{false};
    std::atomic<bool> heapBudgetRequested{false};
    std::array<HelperSlot, kHelperProcessSlots> helpers{};
};

Counters& counters() noexcept {
    static Counters instance;
    return instance;
}

// The innermost eviction cause scope's cause on this thread; Ceiling outside every scope.
thread_local EvictionCause currentCause = EvictionCause::Ceiling;

void raisePeak(std::atomic<std::uint64_t>& peak, const std::uint64_t value) noexcept {
    auto current = peak.load(relaxed);
    while (value > current && !peak.compare_exchange_weak(current, value, relaxed))
        ;
}

} // namespace

std::int64_t steadyNowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void recordStage(const Stage stage, const std::int64_t durationNs) noexcept {
    const auto index = static_cast<std::size_t>(stage);
    if (index >= kStageCount)
        return;
    auto& target = counters().stages[index];
    target.counts[histogramBucket(durationNs)].fetch_add(1, relaxed);
    target.totalNs.fetch_add(durationNs > 0 ? static_cast<std::uint64_t>(durationNs) : 0, relaxed);
}

void readStage(const Stage stage, StageHistogram& out) noexcept {
    const auto index = static_cast<std::size_t>(stage);
    if (index >= kStageCount) {
        out = {};
        return;
    }
    const auto& source = counters().stages[index];
    for (std::size_t bucket = 0; bucket < kHistogramBucketCount; ++bucket)
        out.counts[bucket] = source.counts[bucket].load(relaxed);
    out.totalNs = source.totalNs.load(relaxed);
}

StageTimer::StageTimer(const Stage stage) noexcept : startedNs_(steadyNowNs()), stage_(stage) {}

StageTimer::~StageTimer() { end(); }

void StageTimer::end() noexcept {
    if (!running_)
        return;
    running_ = false;
    recordStage(stage_, steadyNowNs() - startedNs_);
}

void countAdmissionWait() noexcept { counters().admissionWaits.fetch_add(1, relaxed); }
void countAdmissionRefusal() noexcept { counters().admissionRefusals.fetch_add(1, relaxed); }
AdmissionTotals admissionTotals() noexcept {
    const auto& source = counters();
    return {source.admissionWaits.load(relaxed), source.admissionRefusals.load(relaxed)};
}

EvictionCause currentEvictionCause() noexcept { return currentCause; }

EvictionCauseScope::EvictionCauseScope(const EvictionCause cause) noexcept
    : previous_(currentCause) {
    currentCause = cause;
}

EvictionCauseScope::~EvictionCauseScope() { currentCause = previous_; }

void addEvictedBytes(const EvictionCause cause, const std::uint64_t bytes) noexcept {
    const auto index = static_cast<std::size_t>(cause);
    if (index < kEvictionCauseCount && bytes != 0)
        counters().evictedBytes[index].fetch_add(bytes, relaxed);
}

void addEvictedBytes(const std::uint64_t bytes) noexcept {
    addEvictedBytes(currentEvictionCause(), bytes);
}

std::array<std::uint64_t, kEvictionCauseCount> evictedBytesByCause() noexcept {
    std::array<std::uint64_t, kEvictionCauseCount> result{};
    const auto& source = counters();
    for (std::size_t cause = 0; cause < kEvictionCauseCount; ++cause)
        result[cause] = source.evictedBytes[cause].load(relaxed);
    return result;
}

void addGpuUploadBytes(const std::uint64_t bytes) noexcept {
    counters().uploadBytes.fetch_add(bytes, relaxed);
}
void addGpuReadbackBytes(const std::uint64_t bytes) noexcept {
    counters().readbackBytes.fetch_add(bytes, relaxed);
}
void addGpuSubmit() noexcept { counters().submits.fetch_add(1, relaxed); }

GpuTransferTotals gpuTransferTotals() noexcept {
    const auto& source = counters();
    return {source.uploadBytes.load(relaxed), source.readbackBytes.load(relaxed),
            source.submits.load(relaxed)};
}

void countGpuDeviceMemoryAllocation(const std::uint64_t bytes) noexcept {
    counters().deviceAllocations.fetch_add(1, relaxed);
    counters().deviceAllocatedBytes.fetch_add(bytes, relaxed);
}
void countGpuDeviceMemoryFree(const std::uint64_t bytes) noexcept {
    counters().deviceFrees.fetch_add(1, relaxed);
    counters().deviceFreedBytes.fetch_add(bytes, relaxed);
}
GpuDeviceMemoryTotals gpuDeviceMemoryTotals() noexcept {
    const auto& source = counters();
    return {source.deviceAllocations.load(relaxed), source.deviceAllocatedBytes.load(relaxed),
            source.deviceFrees.load(relaxed), source.deviceFreedBytes.load(relaxed)};
}

void publishGpuHeapBudget(const std::uint64_t budgetBytes, const std::uint64_t usageBytes,
                          const std::int64_t steadyNs) noexcept {
    auto& target = counters();
    target.heapBudgetBytes.store(budgetBytes, relaxed);
    target.heapUsageBytes.store(usageBytes, relaxed);
    // Published last: a reader that sees the time sees figures at least that new.
    target.heapBudgetSteadyNs.store(steadyNs > 0 ? steadyNs : 1, std::memory_order_release);
}

GpuHeapBudget gpuHeapBudget() noexcept {
    const auto& source = counters();
    GpuHeapBudget result;
    result.steadyNs = source.heapBudgetSteadyNs.load(std::memory_order_acquire);
    if (result.steadyNs == 0)
        return {};
    result.present = true;
    result.budgetBytes = source.heapBudgetBytes.load(relaxed);
    result.usageBytes = source.heapUsageBytes.load(relaxed);
    return result;
}

void stagingAllocated(const std::uint64_t bytes) noexcept {
    auto& target = counters();
    raisePeak(target.stagingPeak, target.stagingLive.fetch_add(bytes, relaxed) + bytes);
}

void stagingReleased(const std::uint64_t bytes) noexcept {
    counters().stagingLive.fetch_sub(bytes, relaxed);
}

StagingBytes stagingBytes() noexcept {
    const auto& source = counters();
    return {source.stagingLive.load(relaxed), source.stagingPeak.load(relaxed)};
}

void resetStagingPeak() noexcept {
    auto& target = counters();
    target.stagingPeak.store(target.stagingLive.load(relaxed), relaxed);
}

void setProfilerActive(const bool active) noexcept {
    counters().profilerActive.store(active, relaxed);
}
bool profilerActive() noexcept { return counters().profilerActive.load(relaxed); }
void requestGpuHeapBudget() noexcept { counters().heapBudgetRequested.store(true, relaxed); }
bool gpuHeapBudgetRequested() noexcept { return counters().heapBudgetRequested.load(relaxed); }
void clearGpuHeapBudgetRequest() noexcept { counters().heapBudgetRequested.store(false, relaxed); }

HelperProcessRegistration::HelperProcessRegistration(const std::int64_t pid, const char* name,
                                                     const void* owner) noexcept {
    if (pid <= 0 || name == nullptr)
        return;
    auto& slots = counters().helpers;
    for (std::size_t index = 0; index < slots.size(); ++index) {
        std::int64_t expected = 0;
        if (!slots[index].pid.compare_exchange_strong(expected, -1, std::memory_order_acquire,
                                                      relaxed))
            continue;
        slots[index].name.store(name, relaxed);
        slots[index].owner.store(owner, relaxed);
        slots[index].pid.store(pid, std::memory_order_release);
        slot_ = static_cast<int>(index);
        return;
    }
}

HelperProcessRegistration::HelperProcessRegistration(HelperProcessRegistration&& other) noexcept
    : slot_(other.slot_) {
    other.slot_ = -1;
}

HelperProcessRegistration&
HelperProcessRegistration::operator=(HelperProcessRegistration&& other) noexcept {
    if (this != &other) {
        reset();
        slot_ = other.slot_;
        other.slot_ = -1;
    }
    return *this;
}

void HelperProcessRegistration::reset() noexcept {
    if (slot_ < 0)
        return;
    auto& slot = counters().helpers[static_cast<std::size_t>(slot_)];
    slot.pid.store(-1, relaxed);
    slot.name.store(nullptr, relaxed);
    slot.owner.store(nullptr, relaxed);
    slot.pid.store(0, std::memory_order_release);
    slot_ = -1;
}

std::size_t readHelperProcesses(const std::span<HelperProcess> out) noexcept {
    std::size_t count = 0;
    for (auto& slot : counters().helpers) {
        if (count == out.size())
            break;
        const auto pid = slot.pid.load(std::memory_order_acquire);
        if (pid <= 0)
            continue;
        const auto* name = slot.name.load(relaxed);
        const auto* owner = slot.owner.load(relaxed);
        // A slot released and reclaimed while it was read shows a different pid: skip it.
        if (name == nullptr || slot.pid.load(std::memory_order_acquire) != pid)
            continue;
        out[count++] = {pid, name, owner};
    }
    return count;
}

} // namespace bloom::core::telemetry
