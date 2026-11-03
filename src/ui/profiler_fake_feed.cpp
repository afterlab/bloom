#include <bloom/ui/profiler_fake_feed.hpp>

#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace bloom::ui {
namespace {

using runtime::ProfilerBytes;
using runtime::ProfilerCount;
using runtime::ProfilerSnapshot;

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;
// One fake cycle: fill (playback), plateau (export), an eviction drop, then refill.
constexpr std::uint64_t kCycle = 240;
constexpr std::uint64_t kFillEnd = 120;
constexpr std::uint64_t kEvictionAt = 200;

std::uint64_t mib(const double value) { return static_cast<std::uint64_t>(value * kMiB); }

// A small deterministic wobble in -1..1.
double wobble(const std::uint64_t t, const double period) {
    return std::sin(static_cast<double>(t) * 6.283185307179586 / period);
}

template <typename T> runtime::ProfilerMetric<T> of(const T value) {
    return runtime::ProfilerMetric<T>::of(value);
}

void addPool(runtime::ProfilerMemory& memory, const std::string_view name,
             const runtime::ProfilerPoolKind kind, const std::uint64_t host,
             const std::uint64_t pending, const std::uint64_t device, const std::uint64_t entries,
             const ProfilerBytes ceiling, const std::uint64_t t, const std::uint64_t evictions,
             const std::uint64_t bytesPerEviction) {
    auto& pool = memory.pools[memory.poolCount++];
    runtime::setProfilerName(pool.name, name);
    pool.kind = kind;
    pool.hostBytes = host;
    pool.pendingBytes = pending;
    pool.deviceBytes = device;
    pool.entries = of(entries);
    pool.ceilingBytes = ceiling;
    pool.hits = of(t * (12 + memory.poolCount));
    pool.misses = of(t * 2 + t / 7);
    pool.stores = of(t * 3);
    pool.evictions = of(evictions);
    pool.evictedBytes = of(evictions * bytesPerEviction);
    pool.refusals = of(t / 90);
}

void addTask(runtime::ProfilerWorkload& workload, const std::string_view name,
             const runtime::TaskOwnerKind owner, const runtime::TaskExecutor executor,
             const runtime::TaskState state, const runtime::ProfilerRatio progress,
             const std::int64_t ageNs) {
    workload.tasksByState[static_cast<std::size_t>(state)] += 1;
    workload.tasksByOwner[static_cast<std::size_t>(owner)] += 1;
    workload.tasksByExecutor[static_cast<std::size_t>(executor)] += 1;
    if (workload.taskCount >= workload.tasks.size())
        return;
    auto& task = workload.tasks[workload.taskCount++];
    runtime::setProfilerName(task.name, name);
    task.ownerKind = owner;
    task.ownerId = 1;
    task.executor = executor;
    task.state = state;
    task.progress = progress;
    task.ageNs = ageNs;
}

// Cumulative stage counts: a bell of five buckets around `centre` per sample, plus a slow tail.
runtime::ProfilerHistogram stage(const std::uint64_t samples, const std::size_t centre) {
    static constexpr std::array<std::uint64_t, 5> bell{1, 3, 6, 3, 1};
    runtime::ProfilerHistogram histogram;
    for (std::size_t offset = 0; offset < bell.size(); ++offset)
        histogram.counts[centre - 2 + offset] = samples * bell[offset];
    histogram.counts[std::min<std::size_t>(centre + 5, histogram.counts.size() - 1)] = samples / 17;
    for (std::size_t bucket = 0; bucket < histogram.counts.size(); ++bucket) {
        const auto edge = runtime::profilerHistogramEdgeNs(
            std::min<std::size_t>(bucket, runtime::kProfilerHistogramEdgeCount - 1));
        histogram.totalNs += histogram.counts[bucket] * static_cast<std::uint64_t>(edge) / 2;
    }
    return histogram;
}

} // namespace

void fillFakeProfilerSnapshot(const std::uint64_t serial, const std::chrono::milliseconds interval,
                              ProfilerSnapshot& out) noexcept {
    out = ProfilerSnapshot{};
    const std::uint64_t t = std::max<std::uint64_t>(serial, 1);
    const std::uint64_t phase = t % kCycle;
    const std::uint64_t cycles = t / kCycle;
    const std::int64_t intervalNs = std::chrono::nanoseconds(interval).count();
    const bool playing = phase < kFillEnd;
    const bool exporting = phase >= kFillEnd && phase < kEvictionAt;
    const bool pressure = phase >= kEvictionAt - 5 && phase < kEvictionAt + 5;
    const bool rendering = t % 40 < 24;
    const std::uint64_t exportedSamples =
        cycles * (kEvictionAt - kFillEnd) +
        (phase >= kFillEnd ? std::min(phase, kEvictionAt) - kFillEnd : 0);

    out.serial = t;
    out.steadyTimestampNs = static_cast<std::int64_t>(t) * intervalNs;
    out.intervalNs = intervalNs;
    // The ledger samples every five seconds, and stalls for fifteen seconds once a cycle.
    const bool stalled = phase >= 150 && phase < 180;
    out.ledgerSampleAgeNs =
        of<std::int64_t>(static_cast<std::int64_t>(stalled ? phase - 150 : t % 10) * intervalNs);
    out.gpuBudgetQueryAgeNs = of<std::int64_t>(static_cast<std::int64_t>(t % 4) * intervalNs);

    // ---- Memory: pools fill, plateau and drop on an eviction.
    auto& memory = out.memory;
    const double fill = phase < kFillEnd ? static_cast<double>(phase) / kFillEnd : 1.0;
    const std::uint64_t preview =
        phase < kEvictionAt ? static_cast<std::uint64_t>(fill * 1600.0 * kMiB) : mib(640.0);
    const std::uint64_t retiring = phase >= kEvictionAt && phase < kEvictionAt + 12
                                       ? mib(96.0 - static_cast<double>(phase - kEvictionAt) * 8.0)
                                       : 0;
    const std::uint64_t evictions = cycles * 4 + (phase >= kEvictionAt ? 4 : 0);
    // The pool rows the application registers (docs/architecture/profiler-feed.md).
    addPool(memory, "Preview frames", runtime::ProfilerPoolKind::Cache, preview, retiring,
            preview / 2, preview / (6 * kMiB), of(2 * kGiB), t, evictions, mib(240.0));
    addPool(memory, "Operation cache: operations", runtime::ProfilerPoolKind::Cache,
            mib(300.0 + 90.0 * wobble(t, 74.0)), 0, 0, 140 + t % 9, of(mib(512.0)), t, cycles,
            mib(18.0));
    addPool(memory, "Operation cache: decoded media", runtime::ProfilerPoolKind::Cache,
            mib(180.0 + 24.0 * wobble(t, 31.0)), 0, mib(64.0), 36, of(mib(512.0)), t, cycles * 2,
            mib(24.0));
    addPool(memory, "Decoded video", runtime::ProfilerPoolKind::Cache,
            pressure ? mib(40.0) : mib(220.0), 0, 0, pressure ? 4 : 22, of(mib(1024.0)), t, cycles,
            mib(12.0));
    addPool(memory, "Proxies", runtime::ProfilerPoolKind::Cache, mib(48.0), 0, 0, 64,
            of(mib(256.0)), t, 0, 0);
    addPool(memory, "Decoded audio", runtime::ProfilerPoolKind::Cache,
            mib(24.0 + 4.0 * wobble(t, 23.0)), 0, 0, 6, of(mib(128.0)), t, cycles, mib(4.0));
    addPool(memory, "Media disk cache write queue", runtime::ProfilerPoolKind::Scratch,
            rendering ? mib(96.0) : 0, 0, 0, rendering ? 3 : 0, {}, t, 0, 0);

    memory.stagingLiveBytes = of(rendering ? mib(48.0 + 16.0 * wobble(t, 6.0)) : mib(8.0));
    memory.stagingPeakBytes = of(t >= 3 ? mib(96.0) : mib(64.0));
    memory.admissionClaimBytes = of(exporting || rendering ? mib(64.0) : 0);
    memory.idleBaselineBytes = of(mib(420.0));
    // The leak: the unattributed gap creeps up a quarter MiB a sample over the idle baseline.
    const std::uint64_t unattributed = mib(420.0 + 0.25 * static_cast<double>(t % 1200));
    std::uint64_t attributed = runtime::profilerPoolHostBytes(memory) +
                               memory.stagingLiveBytes.value + memory.admissionClaimBytes.value;
    memory.anonBytes = of(attributed + unattributed);
    memory.fileBytes = of(mib(182.0));
    memory.shmemBytes = of(mib(64.0));
    memory.rssBytes = of(memory.anonBytes.value + memory.fileBytes.value + memory.shmemBytes.value +
                         mib(8.0 * wobble(t, 5.0) + 8.0));
    memory.unattributedBytes = runtime::profilerUnattributedBytes(memory);
    memory.physicalBytes = of(64 * kGiB);
    memory.availableBytes = of(64 * kGiB - 18 * kGiB - memory.rssBytes.value);
    memory.swapTotalBytes = of(8 * kGiB);
    memory.swapUsedBytes = of(mib(12.0));
    memory.processSwappedBytes = of(mib(3.0));
    memory.headroomBytes = of(2 * kGiB);
    memory.usableBytes = of(memory.availableBytes.value - memory.headroomBytes.value);
    memory.ledgerConfiguredBytes = of(8 * kGiB);
    memory.ledgerEffectiveBytes = of(8 * kGiB);
    attributed = 0;
    for (std::size_t index = 0; index < memory.poolCount; ++index)
        if (memory.pools[index].kind == runtime::ProfilerPoolKind::Cache)
            attributed += memory.pools[index].hostBytes + memory.pools[index].pendingBytes;
    memory.ledgerRetainedBytes = of(attributed);
    memory.memoryPressure = of(pressure);
    memory.swapPressure = of(false);

    // ---- GPU: utilization stays absent (no platform figure).
    auto& gpu = out.gpu;
    gpu.liveBytes = of(mib(640.0) + preview / 2 + (exporting ? mib(128.0) : 0));
    gpu.retiringBytes = of(retiring / 2);
    gpu.refusals = of(t / 120);
    gpu.reclaims = of(evictions);
    gpu.heapBudgetBytes = of(mib(7680.0));
    gpu.heapUsageBytes = of(gpu.liveBytes.value + gpu.retiringBytes.value + mib(512.0));
    gpu.uploadBytes = of(t * mib(36.0));
    gpu.readbackBytes = of(t * mib(6.0) + exportedSamples * mib(12.0));
    gpu.submits = of(t * 28);

    // ---- CPU: cumulative times are the integrals of the utilization shapes, so the rates a
    // reader derives from two samples follow those shapes.
    auto& cpu = out.cpu;
    const double ns = static_cast<double>(intervalNs);
    const double tt = static_cast<double>(t);
    const double sinePeriod = 40.0;
    const double sineIntegral =
        sinePeriod / 6.283185307179586 * (1.0 - std::cos(tt * 6.283185307179586 / sinePeriod));
    const auto exported = static_cast<double>(exportedSamples);
    const auto played = static_cast<double>(cycles * kFillEnd + std::min(phase, kFillEnd));
    const auto rendered = static_cast<double>((t / 40) * 24 + std::min<std::uint64_t>(t % 40, 24));
    cpu.processCpuNs =
        of(static_cast<std::uint64_t>(ns * (2.4 * tt + 1.4 * sineIntegral + 3.0 * exported)));
    struct Executor {
        std::string_view name;
        std::uint32_t workers;
        double busy;
        double busyIntegral;
    };
    const std::array<Executor, 4> executors{{
        {"CPU", 12, 0.35 + 0.2 * wobble(t, sinePeriod) + (exporting ? 0.3 : 0.0),
         0.35 * tt + 0.2 * sineIntegral + 0.3 * exported},
        {"Blocking I/O", 4, playing ? 0.55 : 0.15, 0.15 * tt + 0.4 * played},
        {"GPU submit", 1, rendering ? 0.8 : 0.3, 0.3 * tt + 0.5 * rendered},
        {"Row bands", 8, rendering ? 0.6 : 0.05, 0.05 * tt + 0.55 * rendered},
    }};
    for (const auto& executor : executors) {
        auto& pool = cpu.executorPools[cpu.executorPoolCount++];
        runtime::setProfilerName(pool.name, executor.name);
        pool.workers = executor.workers;
        pool.queueDepth = static_cast<std::uint32_t>(executor.busy * 10.0 + (pressure ? 14 : 0));
        pool.busyNs = of(static_cast<std::uint64_t>(ns * executor.workers * executor.busyIntegral));
    }

    // ---- Workload.
    auto& workload = out.workload;
    using runtime::TaskExecutor;
    using runtime::TaskOwnerKind;
    using runtime::TaskState;
    const auto age = [&](const std::uint64_t samples) {
        return static_cast<std::int64_t>(samples) * intervalNs;
    };
    addTask(workload, "Preview frame", TaskOwnerKind::Composition, TaskExecutor::Gpu,
            TaskState::Running, of(static_cast<float>(t % 4) / 4.0F), age(1));
    if (playing)
        addTask(workload, "RAM Preview fill", TaskOwnerKind::Composition, TaskExecutor::Gpu,
                TaskState::Running, of(static_cast<float>(fill)), age(phase));
    addTask(workload, "Decode clip_03.mov", TaskOwnerKind::Project, TaskExecutor::BlockingIo,
            TaskState::Running, {}, age(t % 30));
    if (exporting)
        addTask(
            workload, "Export shot_010", TaskOwnerKind::Export, TaskExecutor::Cpu,
            TaskState::Running,
            of(static_cast<float>(phase - kFillEnd) / static_cast<float>(kEvictionAt - kFillEnd)),
            age(phase - kFillEnd));
    addTask(workload, "Thumbnail Assets", TaskOwnerKind::PanelRequest, TaskExecutor::Cpu,
            TaskState::Queued, {}, age(t % 5));
    for (std::uint64_t queued = 0; queued < 2 + t % 4; ++queued)
        addTask(workload, "Preview frame", TaskOwnerKind::Composition, TaskExecutor::Gpu,
                TaskState::Queued, {}, age(queued));
    addTask(workload, "Waveform music.wav", TaskOwnerKind::Project, TaskExecutor::BlockingIo,
            TaskState::Queued, {}, age(t % 12));

    workload.stages[0] = of(stage(t, 9));
    workload.stages[1] = of(stage(t, 5));
    workload.stages[2] = of(stage(t, 7));
    workload.stages[3] = of(stage(t, 10));
    workload.stages[4] = of(stage(t, 6));
    // Every stage is counted; encode has no samples before the first export.
    workload.stages[5] =
        of(exportedSamples > 0 ? stage(exportedSamples, 11) : runtime::ProfilerHistogram{});
    if (playing)
        workload.playbackFps =
            of(static_cast<float>(t % 53 == 0 ? 19.5 : 23.9 + 0.1 * wobble(t, 9.0)));
    workload.droppedFrames = of(t / 53 * 3);
    workload.ramPreviewCachedFrames = of<std::uint64_t>(preview / (6 * kMiB));
    workload.ramPreviewFillFps = of(static_cast<float>(playing ? 4.0 : 0.0));
    if (exporting)
        workload.exportFps = of(static_cast<float>(11.5 + 1.5 * wobble(t, 13.0)));

    // ---- Pressure.
    auto& pressureCounters = out.pressure;
    // A steady trickle of waits, and a burst while memory pressure lasts.
    const std::uint64_t pressureStart = kEvictionAt - 5;
    pressureCounters.admissionWaits =
        of(t + cycles * 60 +
           (phase >= pressureStart ? std::min<std::uint64_t>(phase - pressureStart, 10) * 6 : 0));
    pressureCounters.admissionRefusals = of(t / 120);
    pressureCounters.evictionBytes[0] = of(evictions * mib(240.0));
    pressureCounters.evictionBytes[1] =
        of(cycles * mib(180.0) + (phase >= kEvictionAt ? mib(180.0) : 0));
    pressureCounters.evictionBytes[2] = of(t / 40 * mib(32.0));
    pressureCounters.evictionBytes[3] = of<std::uint64_t>(0);

    // ---- Helper processes: the color worker's CPU time and the old worker's RSS are absent.
    auto& processes = out.processes;
    const auto helper = [&](const std::int64_t pid, const std::string_view name,
                            const ProfilerBytes rss,
                            const runtime::ProfilerMetric<std::uint64_t> cpuNs, const bool alive) {
        auto& entry = processes.helpers[processes.helperCount++];
        entry.pid = pid;
        runtime::setProfilerName(entry.name, name);
        entry.rssBytes = rss;
        entry.cpuNs = cpuNs;
        entry.alive = alive;
    };
    helper(41201, "Media worker", of(mib(210.0 + 30.0 * wobble(t, 50.0))),
           of(t * static_cast<std::uint64_t>(intervalNs) / 3), true);
    helper(41202, "Color worker", of(mib(96.0)), {}, true);
    helper(40877, "Media worker", {}, of(static_cast<std::uint64_t>(intervalNs) * 40), false);
}

FakeProfilerFeed::FakeProfilerFeed(QObject* parent) : ProfilerFeed(parent) {}

FakeProfilerFeed::~FakeProfilerFeed() { shutdown(); }

bool FakeProfilerFeed::readLatest(runtime::ProfilerSnapshot& out) {
    fillFakeProfilerSnapshot(next_++, interval(), out);
    return true;
}

} // namespace bloom::ui
