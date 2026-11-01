// The process-wide telemetry counters (#423): histogram bucketing at every edge, stage recording,
// staging live and peak, GPU transfer totals and heap budget publication, and the helper-process
// registry's slot lifecycle.
#include <bloom/core/telemetry_counters.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace telemetry = bloom::core::telemetry;

int failures = 0;
void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr std::int64_t edge(const std::size_t index) {
    return telemetry::kHistogramFirstEdgeNs << index;
}

static_assert(telemetry::histogramBucket(0) == 0);
static_assert(telemetry::histogramBucket(-5) == 0);
static_assert(telemetry::histogramBucket(edge(0) - 1) == 0);
static_assert(telemetry::histogramBucket(edge(0)) == 1);
static_assert(telemetry::histogramBucket(edge(1) - 1) == 1);
static_assert(telemetry::histogramBucket(edge(1)) == 2);
static_assert(telemetry::histogramBucket(edge(18) - 1) == 18);
static_assert(telemetry::histogramBucket(edge(18)) == 19);
static_assert(telemetry::histogramBucket(INT64_MAX) == 19);

void testBucketEdges() {
    // Every edge starts its own bucket, and the value just below it stays in the previous one.
    for (std::size_t index = 0; index < telemetry::kHistogramEdgeCount; ++index) {
        expect(telemetry::histogramBucket(edge(index)) == index + 1, "edge starts its bucket");
        expect(telemetry::histogramBucket(edge(index) - 1) == index, "below an edge stays");
    }
    // About 4.19 s is the last edge; 16 us is the first.
    expect(edge(18) == 4'194'304'000, "the last edge is 16 us * 2^18");
}

void testRecordStage() {
    telemetry::StageHistogram before;
    telemetry::readStage(telemetry::Stage::Upload, before);
    telemetry::recordStage(telemetry::Stage::Upload, 10'000);
    telemetry::recordStage(telemetry::Stage::Upload, 40'000);
    telemetry::recordStage(telemetry::Stage::Upload, 5'000'000'000);
    telemetry::StageHistogram after;
    telemetry::readStage(telemetry::Stage::Upload, after);
    expect(after.counts[0] == before.counts[0] + 1, "10 us lands in bucket 0");
    expect(after.counts[2] == before.counts[2] + 1, "40 us lands in bucket 2");
    expect(after.counts[19] == before.counts[19] + 1, "5 s lands in the last bucket");
    expect(after.totalNs == before.totalNs + 5'000'050'000, "durations are summed");
    telemetry::StageHistogram decode;
    telemetry::readStage(telemetry::Stage::Decode, decode);
    std::uint64_t decodeCount = 0;
    for (const auto count : decode.counts)
        decodeCount += count;
    expect(decodeCount == 0, "another stage is untouched");

    {
        telemetry::StageTimer timer(telemetry::Stage::Encode);
    }
    {
        telemetry::StageTimer cancelled(telemetry::Stage::Encode);
        cancelled.cancel();
    }
    telemetry::StageHistogram encode;
    telemetry::readStage(telemetry::Stage::Encode, encode);
    std::uint64_t encodeCount = 0;
    for (const auto count : encode.counts)
        encodeCount += count;
    expect(encodeCount == 1, "a timer records once; a cancelled timer records nothing");
}

void testRecordStageConcurrently() {
    telemetry::StageHistogram before;
    telemetry::readStage(telemetry::Stage::Render, before);
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread)
        threads.emplace_back([] {
            for (int sample = 0; sample < 10'000; ++sample)
                telemetry::recordStage(telemetry::Stage::Render, 100'000);
        });
    for (auto& thread : threads)
        thread.join();
    telemetry::StageHistogram after;
    telemetry::readStage(telemetry::Stage::Render, after);
    const auto bucket = telemetry::histogramBucket(100'000);
    expect(after.counts[bucket] - before.counts[bucket] == 40'000, "no lost increments");
}

void testStaging() {
    const auto base = telemetry::stagingBytes().liveBytes;
    telemetry::resetStagingPeak();
    telemetry::stagingAllocated(1000);
    telemetry::stagingAllocated(500);
    expect(telemetry::stagingBytes().liveBytes == base + 1500, "live adds");
    telemetry::stagingReleased(1000);
    auto bytes = telemetry::stagingBytes();
    expect(bytes.liveBytes == base + 500, "live subtracts");
    expect(bytes.peakBytes == base + 1500, "the peak holds the high point");
    telemetry::resetStagingPeak();
    expect(telemetry::stagingBytes().peakBytes == base + 500, "reset restarts the peak at live");
    telemetry::stagingReleased(500);
    expect(telemetry::stagingBytes().liveBytes == base, "everything released");
}

void testGpu() {
    const auto before = telemetry::gpuTransferTotals();
    telemetry::addGpuUploadBytes(100);
    telemetry::addGpuReadbackBytes(30);
    telemetry::addGpuSubmit();
    const auto after = telemetry::gpuTransferTotals();
    expect(after.uploadBytes == before.uploadBytes + 100, "upload bytes");
    expect(after.readbackBytes == before.readbackBytes + 30, "readback bytes");
    expect(after.submits == before.submits + 1, "submits");

    expect(!telemetry::gpuHeapBudget().present, "the heap budget is absent until published");
    telemetry::publishGpuHeapBudget(8'000, 2'000, 123);
    const auto heap = telemetry::gpuHeapBudget();
    expect(heap.present && heap.budgetBytes == 8'000 && heap.usageBytes == 2'000 &&
               heap.steadyNs == 123,
           "the published heap budget");
}

void testHelpers() {
    std::array<telemetry::HelperProcess, telemetry::kHelperProcessSlots> listed{};
    expect(telemetry::readHelperProcesses(listed) == 0, "no helpers at start");
    int owner = 0;
    {
        telemetry::HelperProcessRegistration worker(4242, "Media worker", &owner);
        expect(worker.registered(), "registered");
        telemetry::HelperProcessRegistration invalid(0, "Bad", nullptr);
        expect(!invalid.registered(), "a zero pid is not registered");
        auto count = telemetry::readHelperProcesses(listed);
        expect(count == 1 && listed[0].pid == 4242 && listed[0].owner == &owner &&
                   std::string_view(listed[0].name) == "Media worker",
               "the helper is listed");
        telemetry::HelperProcessRegistration moved(std::move(worker));
        expect(!worker.registered() && moved.registered(), "moving keeps one registration");
        expect(telemetry::readHelperProcesses(listed) == 1, "still one helper after a move");
    }
    expect(telemetry::readHelperProcesses(listed) == 0, "released at destruction");

    // More helpers than slots: the extra ones are not listed, and nothing breaks.
    std::vector<telemetry::HelperProcessRegistration> many;
    for (std::int64_t pid = 1; pid <= 10; ++pid)
        many.emplace_back(pid, "Colour worker", nullptr);
    expect(telemetry::readHelperProcesses(listed) == telemetry::kHelperProcessSlots,
           "every slot fills");
    many.clear();
    expect(telemetry::readHelperProcesses(listed) == 0, "every slot frees");
}

// The always-on cost: what one stage record, one staging add/sub pair and one transfer count take
// on this machine (single thread, uncontended).
void alwaysOnCost() {
    constexpr int kIterations = 1'000'000;
    const auto time = [](auto&& body) {
        const auto start = std::chrono::steady_clock::now();
        for (int index = 0; index < kIterations; ++index)
            body(index);
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                   .count() /
               kIterations;
    };
    const auto stage = time([](const int index) {
        telemetry::recordStage(telemetry::Stage::Transfer, 20'000 + index);
    });
    const auto timer = time([](int) { telemetry::StageTimer timed(telemetry::Stage::Transfer); });
    const auto staging = time([](int) {
        telemetry::stagingAllocated(4096);
        telemetry::stagingReleased(4096);
    });
    const auto transfer = time([](int) { telemetry::addGpuUploadBytes(4096); });
    std::cout << "always-on cost: recordStage " << stage << " ns, StageTimer (two clock reads) "
              << timer << " ns, staging add+sub " << staging << " ns, transfer add " << transfer
              << " ns\n";
    expect(stage < 1000 && staging < 1000 && transfer < 1000, "counters cost nanoseconds");
}

} // namespace

int main() {
    testBucketEdges();
    testRecordStage();
    testRecordStageConcurrently();
    testStaging();
    testGpu();
    testHelpers();
    alwaysOnCost();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "telemetry counters: all passed\n";
    return 0;
}
