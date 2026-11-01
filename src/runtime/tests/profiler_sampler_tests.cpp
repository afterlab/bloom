// The runtime profiler sampler (#423): the refcounted thread (none while idle), 10,000 samples
// without an allocation, seqlock consistency under concurrent readers, statm parsing, the
// scheduler's fixed-slot copy, the ledger adapter, absent fields staying absent, and the cost of
// one sample.
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/profiler_ledger_adapter.hpp>
#include <bloom/runtime/profiler_process_reads.hpp>
#include <bloom/runtime/profiler_sampler.hpp>
#include <bloom/runtime/profiler_seqlock.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// ---- Allocation counting: the global operator new, counted only while `gCounting` is set. -----
namespace {
std::atomic<bool> gCounting{false};
std::atomic<long> gAllocations{0};
} // namespace

void* operator new(std::size_t size) {
    if (gCounting.load(std::memory_order_relaxed))
        gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* block = std::malloc(size == 0 ? 1 : size))
        return block;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* block) noexcept { std::free(block); }
void operator delete[](void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }
void operator delete[](void* block, std::size_t) noexcept { std::free(block); }

namespace {

using namespace bloom::runtime;
using namespace std::chrono_literals;

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;

int failures = 0;
void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// Counts allocations made by `body` on any thread.
template <typename Body> long allocationsDuring(Body&& body) {
    gAllocations.store(0);
    gCounting.store(true);
    body();
    gCounting.store(false);
    return gAllocations.load();
}

bool waitFor(const auto& condition) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition())
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

TaskOwner owner(const std::uint64_t value, const TaskOwnerKind kind = TaskOwnerKind::Composition) {
    return {.kind = kind, .id = TaskOwnerId::fromRaw(value)};
}

TaskSchedulerConfig singleWorkers() {
    auto config = TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    config.rowBandWorkerCount = kSerialRowBandWorkers;
    return config;
}

// A scheduler with one blocked CPU task, one blocked I/O task and `queued` CPU tasks behind them,
// all with names longer than a profiler slot.
struct BusyScheduler final {
    explicit BusyScheduler(const int queued) : scheduler(singleWorkers()) {
        const auto block = [this](TaskContext& context) {
            entered.fetch_add(1);
            while (!released.load() && !context.isCancellationRequested())
                std::this_thread::sleep_for(1ms);
            return TaskResult<void>::succeeded();
        };
        handles.push_back(
            scheduler.submit<void>(TaskRequest(longName("Running CPU"), owner(1)), block).handle);
        handles.push_back(
            scheduler
                .submit<void>(TaskRequest(longName("Running I/O"), owner(2, TaskOwnerKind::Export),
                                          TaskPriority::Background, TaskExecutor::BlockingIo),
                              block)
                .handle);
        expect(waitFor([this] { return entered.load() == 2; }), "both blockers start");
        for (int index = 0; index < queued; ++index) {
            handles.push_back(
                scheduler
                    .submit<void>(
                        TaskRequest(longName("Queued " + std::to_string(index)), owner(3)),
                        [](TaskContext&) { return TaskResult<void>::succeeded(); })
                    .handle);
            std::this_thread::sleep_for(50us);
        }
    }
    ~BusyScheduler() {
        released.store(true);
        scheduler.beginShutdown();
        waitFor([this] { return scheduler.isQuiescent(); });
    }
    BusyScheduler(const BusyScheduler&) = delete;
    BusyScheduler& operator=(const BusyScheduler&) = delete;

    static std::string longName(const std::string& head) {
        return head + " -- a task name far longer than the fixed profiler name slot holds";
    }

    TaskScheduler scheduler;
    std::atomic<bool> released{false};
    std::atomic<int> entered{0};
    std::vector<TaskHandle<void>> handles;
};

void fakeGpu(ProfilerGpu& gpu) noexcept {
    gpu.liveBytes = ProfilerBytes::of(3 * kGiB);
    gpu.retiringBytes = ProfilerBytes::of(64 * kMiB);
    gpu.refusals = ProfilerCount::of(2);
    gpu.reclaims = ProfilerCount::of(7);
}

ProfilerHostInputs someInputs() {
    ProfilerHostInputs inputs;
    inputs.ledger.physicalBytes = ProfilerBytes::of(64 * kGiB);
    inputs.ledger.holdingPresent = true;
    inputs.ledger.holdingHostBytes = 100 * kMiB;
    inputs.ledger.holdingPendingBytes = 4 * kMiB;
    inputs.playbackFps = ProfilerRatio::of(23.9F);
    return inputs;
}

void lifecycleStartsOnFirstAndStopsOnLast() {
    ProfilerSampler sampler;
    expect(!sampler.running() && sampler.subscribers() == 0, "no thread before a subscriber");
    sampler.release();
    expect(sampler.subscribers() == 0, "an unmatched release is ignored");
    expect(sampler.sampleNow(), "sampleNow runs while idle");
    const auto afterManual = sampler.publishedSamples();
    sampler.acquire();
    expect(sampler.running(), "the first subscriber starts the thread");
    expect(waitFor([&] { return sampler.publishedSamples() > afterManual; }),
           "the thread samples at once");
    expect(!sampler.sampleNow(), "sampleNow is refused while the thread runs");
    sampler.acquire();
    expect(sampler.subscribers() == 2 && sampler.running(), "a second subscriber shares it");
    sampler.release();
    expect(sampler.running(), "the thread keeps running for the remaining subscriber");
    sampler.release();
    expect(!sampler.running() && sampler.subscribers() == 0, "the last release stops and joins");
    const auto stopped = sampler.publishedSamples();
    std::this_thread::sleep_for(150ms);
    expect(sampler.publishedSamples() == stopped, "nothing samples without a subscriber");

    // Restart at the fastest rate: about ten samples a second.
    sampler.setInterval(1ms);
    expect(sampler.interval() == ProfilerSampler::kMinimumInterval, "the interval is clamped");
    sampler.setInterval(5s);
    expect(sampler.interval() == ProfilerSampler::kMaximumInterval, "the interval is clamped");
    sampler.setInterval(100ms);
    sampler.acquire();
    std::this_thread::sleep_for(450ms);
    sampler.release();
    const auto taken = sampler.publishedSamples() - stopped;
    expect(taken >= 2 && taken <= 8, "the thread samples at its interval");
}

void tenThousandSamplesAllocateNothing() {
    BusyScheduler busy(24);
    ProfilerSampler sampler({.scheduler = &busy.scheduler, .readGpu = &fakeGpu});
    sampler.publishHostInputs(someInputs());
    expect(sampler.sampleNow(), "a warm-up sample");
    ProfilerSnapshot out;
    const auto allocations = allocationsDuring([&] {
        for (int index = 0; index < 10'000; ++index) {
            static_cast<void>(sampler.sampleNow());
            static_cast<void>(sampler.readLatest(out, 0));
        }
        sampler.publishHostInputs(someInputs());
    });
    expect(allocations == 0, "10,000 samples, reads and input publishes allocate nothing");
    if (allocations != 0)
        std::cerr << "  allocations: " << allocations << '\n';
    expect(out.serial == 10'001, "every sample was published");
    expect(out.workload.taskCount == kProfilerMaximumTasks, "the sample carries the tasks");
}

void seqlockReadersSeeWholeWrites() {
    struct Payload final {
        std::array<std::uint64_t, 129> words{};
        std::uint8_t tail = 0; // An odd size, so the last word is partial.
    };
    ProfilerSeqlock<Payload> lock;
    std::atomic<bool> stop{false};
    std::atomic<long> torn{0};
    std::atomic<long> reads{0};
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 3; ++reader)
        readers.emplace_back([&] {
            Payload seen;
            while (!stop.load(std::memory_order_relaxed)) {
                lock.load(seen);
                const auto first = seen.words[0];
                if (!std::ranges::all_of(seen.words,
                                         [first](auto word) { return word == first; }) ||
                    seen.tail != static_cast<std::uint8_t>(first))
                    torn.fetch_add(1);
                reads.fetch_add(1);
            }
        });
    Payload value;
    const auto deadline = std::chrono::steady_clock::now() + 300ms;
    for (std::uint64_t write = 1; std::chrono::steady_clock::now() < deadline; ++write) {
        value.words.fill(write);
        value.tail = static_cast<std::uint8_t>(write);
        lock.store(value);
    }
    stop.store(true);
    for (auto& reader : readers)
        reader.join();
    expect(torn.load() == 0, "no reader sees a torn write");
    expect(reads.load() > 100, "readers ran concurrently with the writer");

    // The sampler's own buffer under concurrent readers while its thread publishes at 10 Hz.
    ProfilerSampler sampler;
    sampler.setInterval(100ms);
    sampler.acquire();
    std::atomic<long> inconsistent{0};
    std::atomic<bool> done{false};
    std::vector<std::thread> snapshotReaders;
    for (int reader = 0; reader < 2; ++reader)
        snapshotReaders.emplace_back([&] {
            ProfilerSnapshot snapshot;
            std::uint64_t last = 0;
            while (!done.load()) {
                if (sampler.readLatest(snapshot, 0)) {
                    if (snapshot.schemaVersion != kProfilerSnapshotSchemaVersion ||
                        snapshot.serial < last)
                        inconsistent.fetch_add(1);
                    last = snapshot.serial;
                }
            }
        });
    std::this_thread::sleep_for(350ms);
    done.store(true);
    for (auto& reader : snapshotReaders)
        reader.join();
    sampler.release();
    expect(inconsistent.load() == 0, "snapshot readers see whole, ordered samples");
}

void statmParsing() {
    const auto parsed = parseProfilerStatm("1000 250 100 10 0 300 0\n", 4096);
    expect(parsed && parsed->residentBytes == 250 * 4096ULL &&
               parsed->fileAndSharedBytes == 100 * 4096ULL,
           "statm resident and shared are pages");
    expect(parseProfilerStatm("  7 3 1", 1).has_value(), "leading spaces and three fields suffice");
    expect(!parseProfilerStatm("1000 250", 4096), "two fields are not enough");
    expect(!parseProfilerStatm("abc 1 2", 4096), "a non-numeric field is rejected");
    expect(!parseProfilerStatm("", 4096), "empty text is rejected");
    expect(!parseProfilerStatm("1 2 3", 0), "a zero page size is rejected");
#if defined(__linux__)
    const auto live = readProfilerResidentMemory();
    expect(live && live->residentBytes > 0 && live->fileAndSharedBytes &&
               *live->fileAndSharedBytes <= live->residentBytes,
           "this process's statm reads");
#endif
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    expect(readProfilerResidentMemory().has_value(), "RSS is read on every desktop platform");
    expect(readProfilerProcessCpuNs().has_value(), "process CPU time is read");
#endif
}

void schedulerCopyRespectsSlots() {
    BusyScheduler busy(30);
    ProfilerWorkload workload;
    ProfilerCpu cpu;
    const auto now = std::chrono::steady_clock::now();
    const auto allocations =
        allocationsDuring([&] { busy.scheduler.copyProfilerTasks(workload, cpu, now); });
    expect(allocations == 0, "the scheduler copy allocates nothing");
    expect(workload.taskCount == kProfilerMaximumTasks, "the copy fills only the fixed slots");
    expect(workload.tasksByState[static_cast<std::size_t>(TaskState::Running)] == 2 &&
               workload.tasksByState[static_cast<std::size_t>(TaskState::Queued)] == 30,
           "counts cover every task, not only the slots");
    expect(workload.tasksByExecutor[static_cast<std::size_t>(TaskExecutor::BlockingIo)] == 1,
           "tasks are counted by executor");
    expect(workload.tasksByOwner[static_cast<std::size_t>(TaskOwnerKind::Export)] == 1,
           "tasks are counted by owner kind");
    expect(workload.tasks[0].state == TaskState::Running &&
               workload.tasks[1].state == TaskState::Running &&
               workload.tasks[2].state == TaskState::Queued,
           "running tasks come first");
    expect(profilerNameView(workload.tasks[2].name).starts_with("Queued 0 ") &&
               profilerNameView(workload.tasks[15].name).starts_with("Queued 13 "),
           "queued tasks follow oldest first");
    expect(profilerNameView(workload.tasks[0].name).size() == kProfilerNameCapacity - 1,
           "long names are truncated into the slot");
    expect(workload.tasks[2].ageNs > workload.tasks[3].ageNs && workload.tasks[2].ageNs > 0,
           "ages are measured from queueing");
    expect(cpu.executorPoolCount == 3, "CPU, blocking I/O and GPU executors are reported");
    expect(profilerNameView(cpu.executorPools[0].name) == "CPU" &&
               cpu.executorPools[0].workers == 1 && cpu.executorPools[0].queueDepth == 30,
           "the CPU pool has its workers and queue depth");
    expect(cpu.executorPools[1].workers == 1 && cpu.executorPools[1].queueDepth == 0,
           "the blocking I/O pool has its workers and queue depth");
    expect(std::ranges::none_of(cpu.executorPools,
                                [](const auto& pool) { return pool.busyNs.present; }),
           "executor busy time stays absent");
}

void ledgerAdapterReadsAccessors() {
    MemoryBudgetLedger ledger(16 * kGiB);
    const int cacheOwner = 0;
    ledger.registerCache(
        &cacheOwner, 2 * kGiB,
        [] {
            return MemoryPoolHolding{.hostBytes = 300 * kMiB,
                                     .hostPendingRetirementBytes = 20 * kMiB,
                                     .deviceBytes = 1 * kGiB};
        },
        [](std::size_t) {});
    ledger.setRamHeadroom(2 * kGiB);
    ProfilerLedgerReading reading;
    readProfilerLedger(ledger, reading, true);
    expect(reading.physicalBytes.present && reading.physicalBytes.value == 16 * kGiB,
           "physical capacity comes from the ledger");
    expect(reading.headroomBytes.value == 2 * kGiB && reading.usableBytes.value == 14 * kGiB,
           "usable is capacity minus the headroom");
    expect(reading.configuredBytes.present && reading.effectiveBytes.present &&
               reading.memoryPressure.present && reading.swapPressure.present,
           "the ledger state is read when refreshed");
    expect(reading.holdingPresent && reading.holdingHostBytes == 300 * kMiB &&
               reading.holdingPendingBytes == 20 * kMiB && reading.holdingDeviceBytes == 1 * kGiB,
           "the reusable holding is read");
    const auto allocations = allocationsDuring([&] { readProfilerLedger(ledger, reading, false); });
    expect(allocations == 0, "a holding-only read allocates nothing");

    ProfilerMemory memory;
    applyProfilerLedgerReading(reading, memory);
    expect(memory.poolCount == 1 && memory.pools[0].hostBytes == 300 * kMiB &&
               profilerNameView(memory.pools[0].name) == kProfilerReusablePoolName,
           "the holding becomes one aggregate pool row");
    const auto& pool = memory.pools[0];
    expect(!pool.entries.present && !pool.hits.present && !pool.misses.present &&
               !pool.stores.present && !pool.evictions.present && !pool.refusals.present &&
               !pool.ceilingBytes.present,
           "pool counters the ledger does not expose stay absent");
    ledger.unregisterCache(&cacheOwner);
}

void absentFieldsStayAbsent() {
    ProfilerSampler sampler;
    ProfilerSnapshot snapshot;
    expect(!sampler.readLatest(snapshot, 0), "nothing reads before the first sample");
    expect(sampler.sampleNow() && sampler.readLatest(snapshot, 0), "a bare sample publishes");
    const auto& memory = snapshot.memory;
    expect(snapshot.serial == 1 && snapshot.intervalNs == 500'000'000, "serial and interval");
#if defined(__linux__)
    expect(memory.rssBytes.present && memory.anonBytes.present, "RSS and anonymous from statm");
    expect(memory.unattributedBytes.present, "unattributed follows anonymous memory");
#endif
    expect(!memory.fileBytes.present && !memory.shmemBytes.present,
           "file-backed and shared memory are not split at the sampler rate");
    expect(!snapshot.ledgerSampleAgeNs.present && !snapshot.gpuBudgetQueryAgeNs.present,
           "no slower source is read");
    expect(!memory.physicalBytes.present && !memory.ledgerConfiguredBytes.present &&
               !memory.memoryPressure.present && memory.poolCount == 0,
           "ledger figures are absent until the hook publishes them");
    expect(!memory.idleBaselineBytes.present, "no baseline without the pool holding");
    expect(!memory.stagingLiveBytes.present && !memory.stagingPeakBytes.present &&
               !memory.admissionClaimBytes.present,
           "staging and claims stay absent");
    expect(!snapshot.gpu.liveBytes.present && !snapshot.gpu.heapBudgetBytes.present &&
               !snapshot.gpu.uploadBytes.present && !snapshot.gpu.submits.present &&
               !snapshot.gpu.utilization.present,
           "GPU figures are absent without a GPU source");
    expect(snapshot.cpu.executorPoolCount == 0 && snapshot.workload.taskCount == 0,
           "no scheduler, no executors or tasks");
    expect(std::ranges::none_of(snapshot.workload.stages,
                                [](const auto& stage) { return stage.present; }),
           "stage timings stay absent");
    expect(!snapshot.workload.playbackFps.present && !snapshot.workload.exportFps.present &&
               !snapshot.workload.ramPreviewCachedFrames.present,
           "workload rates are absent without the hook");
    expect(!snapshot.pressure.admissionWaits.present &&
               !snapshot.pressure.admissionRefusals.present &&
               std::ranges::none_of(snapshot.pressure.evictionBytes,
                                    [](const auto& bytes) { return bytes.present; }),
           "pressure counters stay absent");
    expect(snapshot.processes.helperCount == 0, "no helper processes are reported");

    // With the hook's values: the pool row, the baseline and the rates arrive; the rest stays.
    sampler.publishHostInputs(someInputs());
    expect(sampler.sampleNow() && sampler.readLatest(snapshot, 1), "a second sample publishes");
    expect(snapshot.memory.poolCount == 1 && snapshot.memory.physicalBytes.present &&
               snapshot.workload.playbackFps.present && !snapshot.workload.exportFps.present,
           "the hook's values reach the snapshot, and only those");
#if defined(__linux__)
    const auto expected = static_cast<std::int64_t>(snapshot.memory.anonBytes.value) -
                          static_cast<std::int64_t>(104 * kMiB);
    expect(snapshot.memory.unattributedBytes.value == expected,
           "unattributed is anonymous minus pool host and pending bytes");
    expect(snapshot.memory.idleBaselineBytes.present &&
               static_cast<std::int64_t>(snapshot.memory.idleBaselineBytes.value) ==
                   std::max<std::int64_t>(expected, 0),
           "the baseline is the first full sample's unattributed gap");
    const auto baseline = snapshot.memory.idleBaselineBytes.value;
    expect(sampler.sampleNow() && sampler.readLatest(snapshot, 2) &&
               snapshot.memory.idleBaselineBytes.value == baseline,
           "the baseline is captured once");
#endif
}

void sampleCost() {
    BusyScheduler busy(24);
    ProfilerSampler sampler({.scheduler = &busy.scheduler, .readGpu = &fakeGpu});
    sampler.publishHostInputs(someInputs());
    std::vector<double> microseconds;
    microseconds.reserve(2000);
    for (int index = 0; index < 2000; ++index) {
        const auto start = std::chrono::steady_clock::now();
        static_cast<void>(sampler.sampleNow());
        microseconds.push_back(
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count());
    }
    std::ranges::sort(microseconds);
    const auto median = microseconds[microseconds.size() / 2];
    const auto p99 = microseconds[microseconds.size() * 99 / 100];
    double total = 0;
    for (const auto value : microseconds)
        total += value;
    std::cout << "profiler sample cost (26 tasks): median " << median << " us, mean "
              << total / static_cast<double>(microseconds.size()) << " us, p99 " << p99 << " us\n";
#if defined(__linux__)
    expect(median < 1000.0, "one sample takes well under a millisecond");
#endif
}

} // namespace

int main() try {
    lifecycleStartsOnFirstAndStopsOnLast();
    tenThousandSamplesAllocateNothing();
    seqlockReadersSeeWholeWrites();
    statmParsing();
    schedulerCopyRespectsSlots();
    ledgerAdapterReadsAccessors();
    absentFieldsStayAbsent();
    sampleCost();
    if (failures == 0)
        std::cout << "profiler sampler: all checks passed\n";
    return failures == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
