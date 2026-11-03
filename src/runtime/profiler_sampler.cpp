#include <bloom/core/telemetry_counters.hpp>
#include <bloom/runtime/profiler_process_reads.hpp>
#include <bloom/runtime/profiler_sampler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>

namespace bloom::runtime {
namespace {

namespace telemetry = core::telemetry;
static_assert(telemetry::kStageCount == kProfilerStageCount);
static_assert(telemetry::kHistogramBucketCount == kProfilerHistogramBucketCount);
static_assert(telemetry::kHistogramFirstEdgeNs == kProfilerHistogramFirstEdgeNs);
static_assert(static_cast<std::size_t>(telemetry::Stage::Encode) ==
              static_cast<std::size_t>(ProfilerStage::Encode));
static_assert(telemetry::histogramBucket(profilerHistogramEdgeNs(18)) ==
              profilerHistogramBucket(profilerHistogramEdgeNs(18)));
static_assert(telemetry::kHelperProcessSlots == kProfilerMaximumHelpers);
static_assert(telemetry::kEvictionCauseCount == kProfilerEvictionCauseCount);
static_assert(static_cast<std::size_t>(telemetry::EvictionCause::MemoryPressure) ==
                  static_cast<std::size_t>(ProfilerEvictionCause::MemoryPressure) &&
              static_cast<std::size_t>(telemetry::EvictionCause::Admission) ==
                  static_cast<std::size_t>(ProfilerEvictionCause::Admission) &&
              static_cast<std::size_t>(telemetry::EvictionCause::Purge) ==
                  static_cast<std::size_t>(ProfilerEvictionCause::Purge));

std::int64_t clampedIntervalNs(const std::chrono::milliseconds interval) noexcept {
    const auto clamped =
        std::clamp(interval, ProfilerSampler::kMinimumInterval, ProfilerSampler::kMaximumInterval);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clamped).count();
}

} // namespace

bool profilerSampleIdle(const ProfilerSnapshot& snapshot,
                        const ProfilerHostInputs& inputs) noexcept {
    const auto& tasks = snapshot.workload.tasksByState;
    return !inputs.hostBusy && tasks[static_cast<std::size_t>(TaskState::Queued)] == 0 &&
           tasks[static_cast<std::size_t>(TaskState::Running)] == 0;
}

ProfilerSampler::ProfilerSampler(const ProfilerSamplerSources sources)
    : sources_(sources), intervalNs_(clampedIntervalNs(kDefaultInterval)) {}

ProfilerSampler::~ProfilerSampler() {
    std::lock_guard lifecycle(lifecycle_);
    stop();
}

void ProfilerSampler::stop() {
    if (!thread_.joinable())
        return;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
    telemetry::setProfilerActive(false);
}

void ProfilerSampler::acquire() {
    std::lock_guard lifecycle(lifecycle_);
    if (subscribers_++ > 0)
        return;
    {
        std::lock_guard lock(mutex_);
        stopping_ = false;
    }
    // The staging peak is the peak since sampler start.
    telemetry::resetStagingPeak();
    telemetry::setProfilerActive(true);
    thread_ = std::thread([this] { run(); });
}

void ProfilerSampler::release() {
    std::lock_guard lifecycle(lifecycle_);
    if (subscribers_ == 0 || --subscribers_ > 0)
        return;
    stop();
}

int ProfilerSampler::subscribers() const {
    std::lock_guard lifecycle(lifecycle_);
    return subscribers_;
}

bool ProfilerSampler::running() const {
    std::lock_guard lifecycle(lifecycle_);
    return thread_.joinable();
}

void ProfilerSampler::setInterval(const std::chrono::milliseconds interval) noexcept {
    intervalNs_.store(clampedIntervalNs(interval), std::memory_order_relaxed);
    wake_.notify_all();
}

std::chrono::milliseconds ProfilerSampler::interval() const noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds(intervalNs_.load(std::memory_order_relaxed)));
}

void ProfilerSampler::publishHostInputs(const ProfilerHostInputs& inputs) noexcept {
    // The seqlock takes one writer at a time.
    std::lock_guard lock(inputsWriter_);
    hostInputs_.store(inputs);
}

bool ProfilerSampler::readLatest(ProfilerSnapshot& out,
                                 const std::uint64_t afterSerial) const noexcept {
    if (published_.writes() == 0)
        return false;
    published_.load(out);
    return out.serial > afterSerial;
}

bool ProfilerSampler::sampleNow() noexcept {
    std::lock_guard lifecycle(lifecycle_);
    if (thread_.joinable())
        return false;
    sample();
    return true;
}

void ProfilerSampler::run() noexcept {
    lowerProfilerThreadPriority();
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        lock.unlock();
        sample();
        lock.lock();
        const auto wait = std::chrono::nanoseconds(intervalNs_.load(std::memory_order_relaxed));
        wake_.wait_for(lock, wait, [this] { return stopping_; });
    }
}

void ProfilerSampler::sample() noexcept {
    const auto now = std::chrono::steady_clock::now();
    auto& snapshot = work_;
    snapshot = ProfilerSnapshot{};
    snapshot.serial = ++serial_;
    snapshot.steadyTimestampNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    snapshot.intervalNs = intervalNs_.load(std::memory_order_relaxed);
    hostInputs_.load(inputs_);

    // ---- Memory: RSS (and on Linux the anonymous part) from statm; the rest from the ledger.
    auto& memory = snapshot.memory;
    if (const auto resident = readProfilerResidentMemory()) {
        memory.rssBytes = ProfilerBytes::of(resident->residentBytes);
        // statm's resident is anonymous + file + shmem pages and its shared field is file +
        // shmem, so the difference is exactly the kernel's anonymous resident count.
        if (resident->fileAndSharedBytes &&
            *resident->fileAndSharedBytes <= resident->residentBytes)
            memory.anonBytes =
                ProfilerBytes::of(resident->residentBytes - *resident->fileAndSharedBytes);
    }
    // The split by kind and this process's swap (Linux /proc/self/status).
    const auto breakdown = readProfilerMemoryBreakdown();
    if (!memory.anonBytes.present && breakdown.anonBytes)
        memory.anonBytes = ProfilerBytes::of(*breakdown.anonBytes);
    if (breakdown.fileBytes)
        memory.fileBytes = ProfilerBytes::of(*breakdown.fileBytes);
    if (breakdown.shmemBytes)
        memory.shmemBytes = ProfilerBytes::of(*breakdown.shmemBytes);
    if (breakdown.swappedBytes)
        memory.processSwappedBytes = ProfilerBytes::of(*breakdown.swappedBytes);
    applyProfilerLedgerReading(inputs_.ledger, memory);
    if (inputs_.ledger.sampleSteadyNs.present)
        snapshot.ledgerSampleAgeNs = ProfilerNanoseconds::of(std::max<std::int64_t>(
            snapshot.steadyTimestampNs - inputs_.ledger.sampleSteadyNs.value, 0));
    // The live host-staging scratch line (always counted).
    const auto staging = telemetry::stagingBytes();
    memory.stagingLiveBytes = ProfilerBytes::of(staging.liveBytes);
    memory.stagingPeakBytes = ProfilerBytes::of(staging.peakBytes);
    memory.unattributedBytes = profilerUnattributedBytes(memory);

    // ---- GPU: Bloom's own allocations from the injected reader; transfers and the driver heap
    // only in a process that has a GPU device (the reader reports its live bytes).
    if (sources_.readGpu != nullptr)
        sources_.readGpu(snapshot.gpu);
    if (snapshot.gpu.liveBytes.present) {
        const auto transfers = telemetry::gpuTransferTotals();
        snapshot.gpu.uploadBytes = ProfilerBytes::of(transfers.uploadBytes);
        snapshot.gpu.readbackBytes = ProfilerBytes::of(transfers.readbackBytes);
        snapshot.gpu.submits = ProfilerCount::of(transfers.submits);
    }
    if (const auto heap = telemetry::gpuHeapBudget(); heap.present) {
        snapshot.gpu.heapBudgetBytes = ProfilerBytes::of(heap.budgetBytes);
        snapshot.gpu.heapUsageBytes = ProfilerBytes::of(heap.usageBytes);
        snapshot.gpuBudgetQueryAgeNs = ProfilerNanoseconds::of(
            std::max<std::int64_t>(snapshot.steadyTimestampNs - heap.steadyNs, 0));
    }

    // ---- Stage histograms (always counted).
    for (std::size_t stage = 0; stage < kProfilerStageCount; ++stage) {
        auto& out = snapshot.workload.stages[stage];
        telemetry::StageHistogram histogram;
        telemetry::readStage(static_cast<telemetry::Stage>(stage), histogram);
        out.value.counts = histogram.counts;
        out.value.totalNs = histogram.totalNs;
        out.present = true;
    }

    // ---- Helper processes: the registered pids, read through /proc or the platform calls.
    std::array<telemetry::HelperProcess, kProfilerMaximumHelpers> helpers{};
    const auto helperCount = telemetry::readHelperProcesses(helpers);
    auto& processes = snapshot.processes;
    processes.helperCount = static_cast<std::uint32_t>(helperCount);
    for (std::size_t index = 0; index < helperCount; ++index) {
        auto& out = processes.helpers[index];
        out.pid = helpers[index].pid;
        setProfilerName(out.name, helpers[index].name);
        const auto reading = readProfilerHelperProcess(out.pid);
        out.alive = reading.alive;
        if (reading.residentBytes)
            out.rssBytes = ProfilerBytes::of(*reading.residentBytes);
        if (reading.cpuNs)
            out.cpuNs = ProfilerMetric<std::uint64_t>::of(*reading.cpuNs);
    }

    // ---- CPU and tasks.
    if (const auto cpuNs = readProfilerProcessCpuNs())
        snapshot.cpu.processCpuNs = ProfilerMetric<std::uint64_t>::of(*cpuNs);
    if (sources_.scheduler != nullptr)
        sources_.scheduler->copyProfilerTasks(snapshot.workload, snapshot.cpu, now);

    // ---- Playback, RAM Preview and export, as the UI-side hook last published them.
    auto& workload = snapshot.workload;
    workload.playbackFps = inputs_.playbackFps;
    workload.droppedFrames = inputs_.droppedFrames;
    workload.ramPreviewCachedFrames = inputs_.ramPreviewCachedFrames;
    workload.ramPreviewFillFps = inputs_.ramPreviewFillFps;
    workload.exportFps = inputs_.exportFps;

    // ---- Pressure: admission answers and evicted bytes by cause (always counted).
    const auto admission = telemetry::admissionTotals();
    snapshot.pressure.admissionWaits = ProfilerCount::of(admission.waits);
    snapshot.pressure.admissionRefusals = ProfilerCount::of(admission.refusals);
    const auto evicted = telemetry::evictedBytesByCause();
    for (std::size_t cause = 0; cause < kProfilerEvictionCauseCount; ++cause)
        snapshot.pressure.evictionBytes[cause] = ProfilerBytes::of(evicted[cause]);

    // ---- The idle baseline: the unattributed gap at the first sample that has both sides while
    // Bloom is idle (no queued or running task, no playback, RAM Preview fill or export).
    if (!baselineCaptured_ && memory.unattributedBytes.present && inputs_.ledger.holdingPresent &&
        profilerSampleIdle(snapshot, inputs_)) {
        baselineCaptured_ = true;
        baselineBytes_ =
            static_cast<std::uint64_t>(std::max<std::int64_t>(memory.unattributedBytes.value, 0));
    }
    if (baselineCaptured_)
        memory.idleBaselineBytes = ProfilerBytes::of(baselineBytes_);

    published_.store(snapshot);
}

} // namespace bloom::runtime
