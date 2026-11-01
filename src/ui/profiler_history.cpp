#include <bloom/ui/profiler_history.hpp>

#include <algorithm>
#include <cmath>

namespace bloom::ui {
namespace {

using runtime::ProfilerSnapshot;

template <typename T> float sample(const runtime::ProfilerMetric<T>& metric) noexcept {
    return metric.present ? static_cast<float>(metric.value) : kit::kAbsentSample;
}

double elapsedSeconds(const ProfilerSnapshot& previous, const ProfilerSnapshot& current) noexcept {
    if (previous.serial == 0 || current.steadyTimestampNs <= previous.steadyTimestampNs)
        return 0.0;
    return static_cast<double>(current.steadyTimestampNs - previous.steadyTimestampNs) / 1e9;
}

// The rate of a cumulative counter between two samples: absent when either side is, when there is
// no previous sample, or when the counter went backwards (the sampler restarted).
template <typename T>
float rate(const runtime::ProfilerMetric<T>& previous, const runtime::ProfilerMetric<T>& current,
           const double seconds) noexcept {
    if (seconds <= 0.0 || !previous.present || !current.present || current.value < previous.value)
        return kit::kAbsentSample;
    return static_cast<float>(static_cast<double>(current.value - previous.value) / seconds);
}

} // namespace

ProfilerHistory::ProfilerHistory()
    : storage_(std::make_unique<float[]>(static_cast<std::size_t>(kCapacity) * kRingCount)) {
    for (int index = 0; index < kRingCount; ++index)
        rings_[static_cast<std::size_t>(index)] = kit::SampleRing(
            storage_.get() + static_cast<std::ptrdiff_t>(index) * kCapacity, kCapacity);
}

ProfilerHistory::~ProfilerHistory() = default;

kit::SampleRing& ProfilerHistory::ring(const Series series) noexcept {
    return rings_[static_cast<std::size_t>(series)];
}

const kit::SampleRing& ProfilerHistory::series(const Series series) const noexcept {
    return rings_[static_cast<std::size_t>(series)];
}

const kit::SampleRing& ProfilerHistory::pool(const int slot) const noexcept {
    return rings_[static_cast<std::size_t>(kSeriesCount + std::clamp(slot, 0, kPoolSeries - 1))];
}

const kit::SampleRing& ProfilerHistory::executor(const int slot) const noexcept {
    return rings_[static_cast<std::size_t>(kSeriesCount + kPoolSeries +
                                           std::clamp(slot, 0, kExecutorSeries - 1))];
}

const ProfilerHistory::Event& ProfilerHistory::event(const int newest) const noexcept {
    const int index =
        ((eventHead_ - 1 - newest) % kEventCapacity + kEventCapacity) % kEventCapacity;
    return events_[static_cast<std::size_t>(index)];
}

void ProfilerHistory::record(const EventKind kind, const std::int64_t timestampNs,
                             const double value, const std::uint8_t detail) {
    events_[static_cast<std::size_t>(eventHead_)] = {timestampNs, kind, value, detail};
    eventHead_ = (eventHead_ + 1) % kEventCapacity;
    eventCount_ = std::min(eventCount_ + 1, kEventCapacity);
}

void ProfilerHistory::clear() noexcept {
    for (auto& entry : rings_)
        entry.clear();
    previous_ = {};
    eventHead_ = 0;
    eventCount_ = 0;
}

void ProfilerHistory::push(const ProfilerSnapshot& snapshot) noexcept {
    const auto& memory = snapshot.memory;
    const auto& old = previous_;
    const double seconds = elapsedSeconds(old, snapshot);

    ring(Series::Rss).push(sample(memory.rssBytes));
    ring(Series::Anon).push(sample(memory.anonBytes));
    ring(Series::Staging).push(sample(memory.stagingLiveBytes));
    ring(Series::Claims).push(sample(memory.admissionClaimBytes));
    const auto unattributed = runtime::profilerUnattributedBytes(memory);
    ring(Series::Unattributed)
        .push(unattributed.present
                  ? static_cast<float>(std::max<std::int64_t>(0, unattributed.value))
                  : kit::kAbsentSample);
    ring(Series::IdleBaseline).push(sample(memory.idleBaselineBytes));
    const auto poolCount = std::min<std::size_t>(memory.poolCount, memory.pools.size());
    for (int slot = 0; slot < kPoolSeries; ++slot) {
        const auto index = static_cast<std::size_t>(slot);
        rings_[static_cast<std::size_t>(kSeriesCount + slot)].push(
            index < poolCount ? static_cast<float>(memory.pools[index].hostBytes +
                                                   memory.pools[index].pendingBytes)
                              : kit::kAbsentSample);
    }

    const auto& gpu = snapshot.gpu;
    ring(Series::GpuLive).push(sample(gpu.liveBytes));
    ring(Series::GpuRetiring).push(sample(gpu.retiringBytes));
    ring(Series::GpuHeapUsage).push(sample(gpu.heapUsageBytes));
    ring(Series::GpuHeapBudget).push(sample(gpu.heapBudgetBytes));
    ring(Series::GpuUtilization).push(sample(gpu.utilization));
    ring(Series::GpuUploadRate).push(rate(old.gpu.uploadBytes, gpu.uploadBytes, seconds));
    ring(Series::GpuReadbackRate).push(rate(old.gpu.readbackBytes, gpu.readbackBytes, seconds));
    ring(Series::GpuSubmitRate).push(rate(old.gpu.submits, gpu.submits, seconds));

    const auto& cpu = snapshot.cpu;
    const float cpuNsPerSecond = rate(old.cpu.processCpuNs, cpu.processCpuNs, seconds);
    ring(Series::ProcessCores)
        .push(std::isnan(cpuNsPerSecond) ? kit::kAbsentSample : cpuNsPerSecond / 1e9F);
    const auto executorCount =
        std::min<std::size_t>(cpu.executorPoolCount, cpu.executorPools.size());
    const auto oldExecutors =
        std::min<std::size_t>(old.cpu.executorPoolCount, old.cpu.executorPools.size());
    for (int slot = 0; slot < kExecutorSeries; ++slot) {
        const auto index = static_cast<std::size_t>(slot);
        float utilization = kit::kAbsentSample;
        if (index < executorCount && index < oldExecutors && seconds > 0.0) {
            const auto& now = cpu.executorPools[index];
            const auto& then = old.cpu.executorPools[index];
            if (now.workers > 0 && now.busyNs.present && then.busyNs.present &&
                now.busyNs.value >= then.busyNs.value)
                utilization =
                    static_cast<float>(static_cast<double>(now.busyNs.value - then.busyNs.value) /
                                       (seconds * 1e9 * now.workers));
        }
        rings_[static_cast<std::size_t>(kSeriesCount + kPoolSeries + slot)].push(utilization);
    }

    const auto& workload = snapshot.workload;
    ring(Series::PlaybackFps).push(sample(workload.playbackFps));
    ring(Series::ExportFps).push(sample(workload.exportFps));
    ring(Series::RamPreviewFillFps).push(sample(workload.ramPreviewFillFps));

    const auto& pressure = snapshot.pressure;
    ring(Series::AdmissionWaitRate)
        .push(rate(old.pressure.admissionWaits, pressure.admissionWaits, seconds));
    ring(Series::AdmissionRefusalRate)
        .push(rate(old.pressure.admissionRefusals, pressure.admissionRefusals, seconds));
    float evicted = seconds > 0.0 ? 0.0F : kit::kAbsentSample;
    bool anyEviction = false;
    for (std::size_t cause = 0; cause < runtime::kProfilerEvictionCauseCount; ++cause) {
        const float bytes =
            rate(old.pressure.evictionBytes[cause], pressure.evictionBytes[cause], seconds);
        if (!std::isnan(bytes)) {
            evicted += bytes;
            anyEviction = true;
        }
    }
    ring(Series::EvictionRate).push(anyEviction ? evicted : kit::kAbsentSample);

    // Events: transitions and counter steps between the previous sample and this one.
    if (old.serial != 0) {
        const auto at = snapshot.steadyTimestampNs;
        const auto edge = [&](const runtime::ProfilerMetric<bool>& before,
                              const runtime::ProfilerMetric<bool>& after, const EventKind began,
                              const EventKind ended) {
            if (before.present && after.present && before.value != after.value)
                record(after.value ? began : ended, at, 0.0);
        };
        edge(old.memory.memoryPressure, memory.memoryPressure, EventKind::MemoryPressureBegan,
             EventKind::MemoryPressureEnded);
        edge(old.memory.swapPressure, memory.swapPressure, EventKind::SwapPressureBegan,
             EventKind::SwapPressureEnded);
        const auto step = [](const runtime::ProfilerCount& before,
                             const runtime::ProfilerCount& after) {
            return before.present && after.present && after.value > before.value
                       ? after.value - before.value
                       : std::uint64_t{0};
        };
        if (const auto refused = step(old.pressure.admissionRefusals, pressure.admissionRefusals))
            record(EventKind::AdmissionRefused, at, static_cast<double>(refused));
        if (const auto refused = step(old.gpu.refusals, gpu.refusals))
            record(EventKind::GpuRefused, at, static_cast<double>(refused));
        for (std::size_t cause = 0; cause < runtime::kProfilerEvictionCauseCount; ++cause)
            if (const auto bytes =
                    step(old.pressure.evictionBytes[cause], pressure.evictionBytes[cause]))
                record(EventKind::Evicted, at, static_cast<double>(bytes),
                       static_cast<std::uint8_t>(cause));
        const auto& helpers = snapshot.processes;
        const auto oldHelpers =
            std::min<std::size_t>(old.processes.helperCount, old.processes.helpers.size());
        for (std::size_t slot = 0;
             slot < std::min<std::size_t>(helpers.helperCount, helpers.helpers.size()); ++slot)
            if (slot < oldHelpers && old.processes.helpers[slot].alive &&
                !helpers.helpers[slot].alive &&
                old.processes.helpers[slot].pid == helpers.helpers[slot].pid)
                record(EventKind::HelperExited, at, static_cast<double>(helpers.helpers[slot].pid),
                       static_cast<std::uint8_t>(slot));
    }
    previous_ = snapshot;
}

} // namespace bloom::ui
