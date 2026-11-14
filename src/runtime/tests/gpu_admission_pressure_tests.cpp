// Admission by actual capacity and actual need: pressure, waits and true exhaustion.
//
// Usable capacity is measured capacity minus the headroom preferences. A phase claims its host need
// (fixed output, 3x its minimum source bytes or the measured retention of its envelope) at
// admission and its device need (working plus
// minimum executor bytes, or the measured peak of its envelope) during its device stage; its
// requested source/executor values are unclaimed ceilings. A need that fits the usable capacity
// but not what other work holds now waits (AwaitingCapacity) and is admitted on release. Only a
// need larger than the whole usable capacity is refused (BudgetRefused).
#include <bloom/core/telemetry_counters.hpp>
#include <bloom/render/native_image_storage.hpp>
#include <bloom/render/processing_host_storage.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace bloom::runtime;
using namespace bloom::render;
using Clock = GpuProcessingPhasePolicy::Clock;
constexpr std::uint64_t kMiB = 1024ULL * 1024;
constexpr std::uint64_t kGiB = 1024 * kMiB;

void require(const bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void deliverHost(GpuProcessingPhasePolicy& policy, const std::uint64_t available,
                 const std::uint64_t serial) {
    MemoryBudgetState sample;
    sample.machine.availableBytes = available;
    sample.process.status = ProcessMemoryStatus::Unsupported;
    sample.process.source = ProcessMemorySource::OtherHostOnly;
    sample.process.sampledAt = Clock::now();
    sample.processSampleFresh = true;
    sample.sampleSerial = serial;
    policy.applyHostSample(sample, 64 * kGiB, sample.process.sampledAt);
}

void reportDevice(GpuProcessingPhasePolicy& policy, const std::uint64_t epoch,
                  const std::uint64_t available) {
    require(policy.reportDevice(epoch, {.source = GpuAllocationBudgetSource::MemoryBudget,
                                        .device_local_bytes = 24 * kGiB,
                                        .available_bytes = available}),
            "device report accepted");
}

// One 2048x858 frame: float output and metadata on the host, one float frame of source, the
// executor's live-image peak on the device.
GpuProcessingPhaseEnvelope frame(const std::uint64_t deviceWorking = 64 * kMiB) {
    const std::uint64_t pixels = 2048ULL * 858;
    return {.output = GpuProcessingPhaseOutput::FloatFinal,
            .pixels = pixels,
            .metadataBytes = 16 * kMiB,
            .requestedSourceBytes = 64 * kGiB,
            .minimumSourceBytes = pixels * 16,
            .requestedExecutorBytes = 64 * kGiB,
            .minimumExecutorBytes = pixels * 16,
            .deviceWorkingBytes = deviceWorking};
}

void headroomIsTheOnlyMargin() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 2 * kGiB, .vramBytes = 2 * kGiB});
    deliverHost(policy, 20 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    auto usable = policy.usableCapacity();
    require(usable.hostReady && usable.hostUsableBytes == 18 * kGiB,
            "usable RAM is available memory minus the 2 GiB default headroom");
    require(usable.deviceReady && usable.deviceUsableBytes == 20 * kGiB,
            "usable VRAM is the live budget minus the 2 GiB default headroom");
    policy.setHeadroom({.ramBytes = 4 * kGiB, .vramBytes = 1 * kGiB});
    usable = policy.usableCapacity();
    require(usable.hostUsableBytes == 16 * kGiB && usable.deviceUsableBytes == 21 * kGiB,
            "a headroom preference change applies at once to both domains");
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    require(policy.usableCapacity().hostUsableBytes == 20 * kGiB &&
                policy.usableCapacity().deviceUsableBytes == 22 * kGiB,
            "zero headroom exposes the whole measured capacity");
    deliverHost(policy, 10 * kGiB, 2);
    require(policy.usableCapacity().hostUsableBytes == 10 * kGiB,
            "the next measured sample replaces the capacity");
}

void phasesClaimOnlyTheirNeed() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 2 * kGiB, .vramBytes = 2 * kGiB});
    deliverHost(policy, 20 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    std::vector<GpuProcessingPhaseGrant> grants;
    // The desktop failure: Viewer, RAM fill and export frames together, each asking the whole
    // machine as a ceiling. Every one is admitted and the claims stay a small fraction.
    for (int index = 0; index < 8; ++index) {
        grants.push_back(policy.reserve(frame()));
        require(grants.back().status == GpuProcessingPhaseAdmission::Accepted,
                "concurrent 2048x858 frames are all admitted on a mostly free machine");
    }
    const auto usable = policy.usableCapacity();
    require(usable.hostClaimBytes < 3 * kGiB && usable.deviceClaimBytes < 1 * kGiB,
            "eight frames claim their need, not the whole machine");
    require(grants.front().lease->sourceByteLimit() > 1 * kGiB &&
                grants.front().lease->executorByteLimit() > 1 * kGiB,
            "the requested ceilings remain available to a phase without being claimed");
}

void contentionWaitsAndResumesOnRelease() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    deliverHost(policy, 20 * kGiB, 1);
    // An artificially small device: room for two frames' device need, not three.
    const auto perFrameDevice = 64 * kMiB + 2048ULL * 858 * 16;
    reportDevice(policy, 1, 2 * perFrameDevice + perFrameDevice / 2);
    auto first = policy.reserve(frame());
    auto second = policy.reserve(frame());
    require(first.lease && second.lease, "two frames fit the small device");
    // Both run their device stages, which claim their device needs.
    const auto now = Clock::now();
    const NativeImageStorageIdentity local{.allocatorGeneration = 7, .memoryType = 2, .heap = 1};
    auto firstStage = NativeImageStorageToken::reserve(first.lease, kMiB, local, now);
    auto secondStage = NativeImageStorageToken::reserve(second.lease, kMiB, local, now);
    require(firstStage.token && secondStage.token &&
                policy.usableCapacity().deviceClaimBytes == 2 * perFrameDevice,
            "two running device stages claim their needs");
    const auto seen = policy.usableCapacity().admissionRevision;
    auto third = policy.reserve(frame());
    require(third.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                third.domain == GpuProcessingPhaseDomain::Device && !third.lease,
            "a frame that fits the device but not the held claims waits, it is not refused");
    std::atomic_bool woke{false};
    std::thread waiter(
        [&] { woke = policy.waitForAdmissionChange(seen, {}, std::chrono::seconds(10)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    firstStage.token.reset();
    first.lease.reset();
    waiter.join();
    require(woke.load(), "a release wakes the waiting request");
    third = policy.reserve(frame());
    require(third.status == GpuProcessingPhaseAdmission::Accepted,
            "the waiting frame is admitted after the release");
    const auto start = Clock::now();
    require(!policy.waitForAdmissionChange(policy.usableCapacity().admissionRevision, {},
                                           std::chrono::milliseconds(100)) &&
                Clock::now() - start < std::chrono::seconds(2),
            "an unchanged root ends the wait at its bound so the caller re-checks cancellation");
    secondStage.token.reset();
}

void exhaustionIsRefusedWithReadableCounters() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 2 * kGiB, .vramBytes = 2 * kGiB});
    deliverHost(policy, 4 * kGiB, 1);
    reportDevice(policy, 1, 3 * kGiB);
    // Usable now: 2 GiB RAM, 1 GiB VRAM; the host ceiling is the 64 GiB machine minus headroom.
    // A 16384x16384 float frame (about 24 GiB of host need) fits the machine, so it waits for
    // memory other processes hold; a 32768x32768 frame can never fit and is refused.
    auto large = frame();
    large.pixels = 16384ULL * 16384;
    large.minimumSourceBytes = large.pixels * 16;
    large.minimumExecutorBytes = large.pixels * 16;
    // The Profiler's pressure counters (process-wide): two waits and two refusals.
    const auto countedBefore = bloom::core::telemetry::admissionTotals();
    const auto waits = policy.reserve(large);
    require(waits.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                waits.domain == GpuProcessingPhaseDomain::Host && !waits.lease,
            "a frame that fits the machine but not the memory free now waits");
    auto huge = large;
    huge.pixels = 32768ULL * 32768;
    huge.requestedSourceBytes = huge.pixels * 16;
    huge.minimumSourceBytes = huge.pixels * 16;
    huge.minimumExecutorBytes = huge.pixels * 16;
    const auto refused = policy.reserve(huge);
    require(refused.status == GpuProcessingPhaseAdmission::BudgetRefused &&
                refused.domain == GpuProcessingPhaseDomain::Host && !refused.lease,
            "a frame larger than the host ceiling is refused");
    // The device ceiling is the 24 GiB device-local heap minus headroom (#539): a device need above
    // the usable 1 GiB waits for the VRAM other programs hold; one above the heap is refused.
    const auto deviceWaits = policy.reserve(frame(2 * kGiB));
    require(deviceWaits.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                deviceWaits.domain == GpuProcessingPhaseDomain::Device,
            "a device need above the usable room but within the device waits");
    const auto deviceRefused = policy.reserve(frame(23 * kGiB));
    require(deviceRefused.status == GpuProcessingPhaseAdmission::BudgetRefused &&
                deviceRefused.domain == GpuProcessingPhaseDomain::Device,
            "a device need larger than the whole device is refused");
    const auto counted = bloom::core::telemetry::admissionTotals();
    require(counted.waits == countedBefore.waits + 2 &&
                counted.refusals == countedBefore.refusals + 2,
            "each wait and each refusal is counted for the Profiler");
    {
        const auto accepted = policy.reserve(frame());
        require(accepted.status == GpuProcessingPhaseAdmission::Accepted &&
                    bloom::core::telemetry::admissionTotals().waits == counted.waits &&
                    bloom::core::telemetry::admissionTotals().refusals == counted.refusals,
                "an accepted reservation counts neither");
    }
    const auto usable = policy.usableCapacity();
    require(usable.hostClaimBytes == 0 && usable.deviceClaimBytes == 0 &&
                usable.admittedPhases == 0,
            "a refusal leaves no claim behind");
    const auto text = describeAdmission(deviceRefused, usable);
    require(text.find("BudgetRefused") != std::string::npos &&
                text.find("domain=Device") != std::string::npos &&
                text.find("deviceUsable=1073741824") != std::string::npos &&
                text.find("phases=0") != std::string::npos,
            "the diagnostic names the status, the domain and the counters");
}

void manyConcurrentPhases() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 2 * kGiB, .vramBytes = 2 * kGiB});
    deliverHost(policy, 60 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    std::vector<std::shared_ptr<GpuProcessingPhaseLease>> leases;
    std::vector<std::thread> threads;
    std::mutex mutex;
    for (int index = 0; index < 32; ++index) {
        threads.emplace_back([&] {
            auto grant = policy.reserve(frame());
            const std::lock_guard lock(mutex);
            if (grant.lease)
                leases.push_back(std::move(grant.lease));
        });
    }
    for (auto& thread : threads)
        thread.join();
    require(leases.size() == 32, "32 concurrent frames are admitted from 32 threads");
    const auto overflow = policy.reserve(frame());
    require(overflow.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                overflow.domain == GpuProcessingPhaseDomain::RequestCount,
            "the request slots are a wait, not a refusal");
    leases.pop_back();
    require(policy.reserve(frame()).status == GpuProcessingPhaseAdmission::Accepted,
            "a released slot admits the next frame");
    leases.clear();
    require(policy.usableCapacity().hostClaimBytes == 0 &&
                policy.usableCapacity().deviceClaimBytes == 0,
            "every claim returns when the phases end");
}

void retentionIsEvictedBeforeWaiting() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    deliverHost(policy, 1 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    std::atomic<std::uint64_t> asked{0};
    std::atomic<int> calls{0};
    policy.addRetentionReclaimer([&](const GpuProcessingPhaseDomain domain,
                                     const std::uint64_t bytes) -> std::uint64_t {
        require(domain == GpuProcessingPhaseDomain::Host, "host retention is asked for host room");
        asked = bytes;
        ++calls;
        return 0;
    });
    auto held = policy.reserve(frame());
    require(held.lease != nullptr, "the first frame fits");
    auto big = frame();
    big.minimumSourceBytes = 300 * kMiB;
    const auto free = policy.usableCapacity().hostFreeBytes;
    const auto waiting = policy.reserve(big);
    const auto need = policy.lastAwaitingNeed();
    require(waiting.status == GpuProcessingPhaseAdmission::AwaitingCapacity && calls == 1 && need &&
                need->domain == GpuProcessingPhaseDomain::Host &&
                need->bytes >= 3 * big.minimumSourceBytes && asked == need->bytes - free &&
                need->shortfallBytes == asked,
            "a waiting request first asks reusable retention to release its shortfall");
}

// The owner's stuck export: reusable caches filled the memory beyond the headroom (inside a
// 24 GB cgroup cap), and an export frame waited forever because nothing evicted them. With the
// ledger's evict-on-demand registered as the policy's reclaimer, the frame is admitted at the
// next sample instead of waiting.
void cachesYieldToWork() {
    for (const bool cgroupCapped : {false, true}) {
        MemoryBudgetLedger ledger(64 * kGiB, 64 * kGiB);
        ledger.setRamHeadroom(2 * kGiB);
        std::size_t cached = 0;
        ledger.registerCache(
            &cached, 64 * kGiB, [&] { return cached; },
            [&](const std::size_t bytes) { cached = std::min(cached, bytes); });
        // What the machine (or the 24 GB cgroup) has left once the caches hold `cached`.
        const std::uint64_t room = cgroupCapped ? 24 * kGiB : 60 * kGiB;
        const auto availableNow = [&] { return room - std::min<std::uint64_t>(room, cached); };
        cached = room - 3 * kGiB; // Caches filled to headroom plus margin plus a little.
        MemoryBudgetState state;
        state.machine.availableBytes = cgroupCapped ? 40 * kGiB : availableNow();
        if (cgroupCapped) {
            state.process.status = ProcessMemoryStatus::Finite;
            state.process.source = ProcessMemorySource::LinuxCgroupV2;
            state.process.domains = {{"/owner.scope", ProcessMemoryLimitKind::Finite, room,
                                      static_cast<std::size_t>(room - availableNow())}};
            state.process.reclaimableBytes = {std::size_t{0}};
        }
        static_cast<void>(ledger.poll(state.machine));
        GpuProcessingPhasePolicy policy(32);
        policy.setHeadroom({.ramBytes = 2 * kGiB, .vramBytes = 0});
        std::uint64_t serial = 1;
        const auto sample = [&] {
            if (cgroupCapped)
                state.process.domains.front().currentBytes =
                    static_cast<std::size_t>(room - availableNow());
            else
                state.machine.availableBytes = availableNow();
            state.process.sampledAt = Clock::now();
            state.processSampleFresh = true;
            state.sampleSerial = serial++;
            if (!cgroupCapped) {
                state.process.status = ProcessMemoryStatus::Unsupported;
                state.process.source = ProcessMemorySource::OtherHostOnly;
            }
            policy.applyHostSample(state, 64 * kGiB, state.process.sampledAt);
        };
        sample();
        reportDevice(policy, 1, 22 * kGiB);
        policy.addRetentionReclaimer(
            [&](const GpuProcessingPhaseDomain domain, const std::uint64_t shortfall) {
                return domain == GpuProcessingPhaseDomain::Host
                           ? ledger.reclaim(static_cast<std::size_t>(shortfall))
                           : std::uint64_t{0};
            });
        auto exportFrame = frame();
        exportFrame.admissionClass = GpuAdmissionClass::Export;
        exportFrame.minimumSourceBytes = 512 * kMiB; // A 4K float source.
        const auto before = cached;
        const auto first = policy.reserve(exportFrame);
        require(first.status == GpuProcessingPhaseAdmission::AwaitingCapacity && cached < before,
                cgroupCapped ? "capped: a frame that does not fit makes the caches give back"
                             : "a frame that does not fit makes the caches give back");
        sample(); // The fresh sample the reclaimer requests.
        const auto admitted = policy.reserve(exportFrame);
        require(admitted.status == GpuProcessingPhaseAdmission::Accepted,
                cgroupCapped ? "capped: the export frame is admitted at the next sample"
                             : "the export frame is admitted at the next sample");
        ledger.unregisterCache(&cached);
    }
}

// #425: usable VRAM (the live budget minus the VRAM headroom) is the only device ceiling. A device
// cache may fill what fits; a frame whose need fits usable VRAM but not what the cache holds waits
// (it is never refused), the cache's owner evicts the shortfall, and the frame is admitted on the
// next device report. A live budget below the headroom (another program took VRAM) asks the
// caches for the difference.
void deviceCachesYieldToWork() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 2 * kGiB});
    deliverHost(policy, 20 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    // A scene-cache image retained from an earlier frame: 16 GiB of root-owned device memory.
    const NativeImageStorageIdentity local{.allocatorGeneration = 7, .memoryType = 2, .heap = 1};
    auto earlier = policy.reserve(frame());
    require(earlier.lease != nullptr, "the earlier frame is admitted");
    auto cached = NativeImageStorageToken::reserve(earlier.lease, 16 * kGiB, local, Clock::now());
    require(cached.token.has_value(), "the cache image is admitted against usable VRAM");
    earlier.lease.reset();
    reportDevice(policy, 1, 6 * kGiB); // The driver now sees the cache's 16 GiB in use.
    auto request = std::make_shared<GpuDeviceReclaimRequest>();
    std::atomic<int> woke{0};
    registerDeviceReclaim(policy, request, [&] { ++woke; });

    const auto big = frame(6 * kGiB);
    const auto waiting = policy.reserve(big);
    require(waiting.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                waiting.domain == GpuProcessingPhaseDomain::Device,
            "a need that fits usable VRAM but not beside the cache waits; it is not refused");
    require(request->pending() && woke == 1, "the waiting frame asks the device caches to evict");
    const auto need = policy.lastAwaitingNeed();
    const auto wanted = deviceReclaimBytes(
        *request, {.source = GpuAllocationBudgetSource::MemoryBudget,
                   .device_local_bytes = 24 * kGiB,
                   .available_bytes = 6 * kGiB},
        2 * kGiB);
    require(need && wanted == need->shortfallBytes && wanted != 0 && !request->pending(),
            "the owner takes exactly the waiting shortfall");

    cached.token.reset(); // The owner's eviction releases the cache image.
    reportDevice(policy, 1, 22 * kGiB);
    require(policy.reserve(big).status == GpuProcessingPhaseAdmission::Accepted,
            "the frame is admitted once the cache gave its memory back");

    const auto pressure = [&](const GpuAllocationBudgetSource source, const std::uint64_t free) {
        return deviceReclaimBytes(*request,
                                  {.source = source,
                                   .device_local_bytes = 24 * kGiB,
                                   .available_bytes = free},
                                  2 * kGiB);
    };
    require(pressure(GpuAllocationBudgetSource::MemoryBudget, 1 * kGiB) == 1 * kGiB &&
                pressure(GpuAllocationBudgetSource::MemoryBudget, 3 * kGiB) == 0 &&
                pressure(GpuAllocationBudgetSource::DeviceLocalEstimate, 0) == 0,
            "free VRAM below the headroom asks the caches for the difference, live budgets only");
}

// Preview first (#413) never starves the frame being prepared. During a RAM Preview fill a frame
// that waits for host room is given it by the operation cache first, leaving the finished preview
// frames alone; when the finished frames are all that is left, they still give way, so the frame
// in flight is admitted at the next sample either way.
void previewFillWorkIsNotStarved() {
    for (const bool operationHolds : {true, false}) {
        MemoryBudgetLedger ledger(64 * kGiB, 64 * kGiB);
        ledger.setRamHeadroom(2 * kGiB);
        std::size_t operation = 0;
        std::size_t preview = 0;
        std::size_t previewBudget = 0;
        ledger.registerCache(
            &operation, 64 * kGiB, [&] { return operation; },
            [&](const std::size_t bytes) { operation = std::min(operation, bytes); });
        ledger.registerCache(
            &preview, 64 * kGiB, [&] { return preview; },
            [&](const std::size_t bytes) {
                previewBudget = bytes;
                preview = std::min(preview, bytes);
            },
            MemoryBudgetOwnerKind::ReusableRetention, MemoryEvictionPriority::FinishedPreview);
        const std::uint64_t room = 24 * kGiB;
        const auto availableNow = [&] {
            return room - std::min<std::uint64_t>(room, operation + preview);
        };
        // The pools fill the room to just beyond headroom plus margin.
        preview = operationHolds ? 8 * kGiB : room - 3 * kGiB;
        operation = operationHolds ? room - 3 * kGiB - preview : 0;
        MemoryBudgetState state;
        state.machine.availableBytes = availableNow();
        static_cast<void>(ledger.poll(state.machine));
        GpuProcessingPhasePolicy policy(32);
        policy.setHeadroom({.ramBytes = 2 * kGiB, .vramBytes = 0});
        std::uint64_t serial = 1;
        const auto sample = [&] {
            state.machine.availableBytes = availableNow();
            state.process.status = ProcessMemoryStatus::Unsupported;
            state.process.source = ProcessMemorySource::OtherHostOnly;
            state.process.sampledAt = Clock::now();
            state.processSampleFresh = true;
            state.sampleSerial = serial++;
            policy.applyHostSample(state, 64 * kGiB, state.process.sampledAt);
        };
        sample();
        reportDevice(policy, 1, 22 * kGiB);
        policy.addRetentionReclaimer(
            [&](const GpuProcessingPhaseDomain domain, const std::uint64_t shortfall) {
                return domain == GpuProcessingPhaseDomain::Host
                           ? ledger.reclaim(static_cast<std::size_t>(shortfall))
                           : std::uint64_t{0};
            });
        auto fill = ledger.beginPreviewFill();
        auto inFlight = frame();
        inFlight.minimumSourceBytes = 512 * kMiB; // A 2160p float source.
        const auto previewBefore = preview;
        const auto budgetBefore = previewBudget;
        const auto operationBefore = operation;
        const auto first = policy.reserve(inFlight);
        require(first.status == GpuProcessingPhaseAdmission::AwaitingCapacity,
                "the in-flight frame does not fit before the pools give back");
        if (operationHolds) {
            require(operation < operationBefore && preview == previewBefore &&
                        previewBudget == budgetBefore,
                    "during a fill the operation cache gives the waiting frame its room and the "
                    "finished preview frames keep theirs");
        } else {
            require(preview < previewBefore,
                    "with nothing else left the finished preview frames still give way to the "
                    "frame being prepared");
        }
        sample(); // The fresh sample the reclaimer requests.
        const auto admitted = policy.reserve(inFlight);
        require(admitted.status == GpuProcessingPhaseAdmission::Accepted,
                operationHolds ? "the frame in flight is admitted at the next sample"
                               : "the frame in flight is admitted even when only preview "
                                 "frames could give");
        fill.end();
        ledger.unregisterCache(&operation);
        ledger.unregisterCache(&preview);
    }
}

void strictViewerPriority() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    deliverHost(policy, 20 * kGiB, 1);
    const auto perFrameDevice = 64 * kMiB + 2048ULL * 858 * 16;
    reportDevice(policy, 1, 2 * perFrameDevice + perFrameDevice / 2);
    auto exportFrame = frame();
    exportFrame.admissionClass = GpuAdmissionClass::Export;
    const auto start = Clock::now();
    // Idle Viewer: an export starts at once, and a second export frame too.
    auto first = policy.reserve(exportFrame, start);
    auto second = policy.reserve(exportFrame, start);
    require(first.lease && second.lease, "with an idle Viewer exports use the whole root");
    // Both export frames run their device stages, allocating their whole need (so the measured
    // peaks of this envelope keep the need at its stated size).
    const NativeImageStorageIdentity local{.allocatorGeneration = 7, .memoryType = 2, .heap = 1};
    auto firstStage = NativeImageStorageToken::reserve(first.lease, perFrameDevice, local, start);
    auto secondStage = NativeImageStorageToken::reserve(second.lease, perFrameDevice, local, start);
    require(firstStage.token && secondStage.token, "the export device stages start");
    // A Viewer frame now waits for device room; while it waits no export frame is granted.
    const auto viewerWaiting = policy.reserve(frame(), start);
    require(viewerWaiting.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                policy.viewerPriority(start).waiting,
            "the Viewer request waits for device room and is recorded as waiting");
    firstStage.token.reset();
    first.lease.reset();
    const auto yielded = policy.reserve(exportFrame, start);
    require(yielded.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                yielded.domain == GpuProcessingPhaseDomain::Viewer,
            "a released slot goes to the waiting Viewer, not to the next export frame");
    auto viewer = policy.reserve(frame(), start);
    require(viewer.lease && policy.viewerPriority(start).active &&
                !policy.viewerPriority(start).waiting,
            "the Viewer is admitted and becomes active");
    auto viewerStage =
        NativeImageStorageToken::reserve(viewer.lease, perFrameDevice, local, start);
    require(viewerStage.token.has_value(), "the Viewer device stage starts");
    // While the Viewer is active an export frame must leave the Viewer's need free.
    secondStage.token.reset();
    second.lease.reset();
    const auto spare = policy.reserve(exportFrame, start);
    require(spare.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                spare.domain == GpuProcessingPhaseDomain::Viewer,
            "an active Viewer keeps its reserve; the export uses spare capacity only");
    // Once the Viewer goes idle (no Viewer frame within the activity window) exports resume.
    const auto idle =
        start + GpuProcessingPhasePolicy::kViewerActivityWindow + std::chrono::milliseconds(1);
    deliverHost(policy, 20 * kGiB, 2);
    auto resumed = policy.reserve(exportFrame, idle);
    require(resumed.status == GpuProcessingPhaseAdmission::Accepted,
            "an idle Viewer holds nothing: the export frame is admitted");
}

// The scope measure's phase (#724): a native, source-free Background phase that needs `deviceNeed`
// bytes of working storage and a table copy on the host.
GpuProcessingPhaseEnvelope measurePhase(const std::uint64_t deviceNeed) {
    return {.output = GpuProcessingPhaseOutput::PackedPreview,
            .pixels = 1,
            .metadataBytes = kMiB,
            .requestedExecutorBytes = std::uint64_t{0},
            .deviceWorkingBytes = deviceNeed,
            .sourceFree = true,
            .native = true,
            .admissionClass = GpuAdmissionClass::Background};
}

// A Background measure phase never delays a Viewer phase: it waits while a Viewer request waits,
// and its grant leaves the Viewer's last need free, so the next Viewer frame is admitted at once
// beside it.
void backgroundMeasureYieldsToTheViewer() {
    const std::uint64_t pixels = 2048ULL * 858;
    const std::uint64_t viewerNeed = 64 * kMiB + pixels * 16;
    const std::uint64_t measureNeed = 40 * kMiB;
    const NativeImageStorageIdentity local{.allocatorGeneration = 7, .memoryType = 2, .heap = 1};
    {
        // A Viewer frame holds its device stage. The card holds it and the measure but not the
        // measure beside one more Viewer need: the measure waits, domain Viewer, taking nothing.
        GpuProcessingPhasePolicy policy(32);
        policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
        deliverHost(policy, 20 * kGiB, 1);
        const auto start = Clock::now();
        reportDevice(policy, 1, 2 * viewerNeed + measureNeed - 1);
        auto viewer = policy.reserve(frame(), start);
        require(viewer.lease != nullptr, "the Viewer frame is admitted");
        auto viewerStage = NativeImageStorageToken::reserve(viewer.lease, viewerNeed, local, start);
        require(viewerStage.token.has_value(), "the Viewer device stage starts");
        const auto before = policy.snapshot().reservations;
        const auto refused = policy.reserve(measurePhase(measureNeed), start);
        require(refused.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                    refused.domain == GpuProcessingPhaseDomain::Viewer && !refused.lease,
                "an active Viewer keeps its last need free: the Background measure waits");
        require(policy.snapshot().reservations.admittedPhases == before.admittedPhases,
                "the waiting measure holds nothing");
        // The card grows by one byte: the measure is admitted and leaves the Viewer's need free.
        reportDevice(policy, 1, 2 * viewerNeed + measureNeed);
        auto granted = policy.reserve(measurePhase(measureNeed), start);
        require(granted.status == GpuProcessingPhaseAdmission::Accepted && granted.lease,
                "with the Viewer's need free beside it the measure is admitted");
        auto measureStage =
            NativeImageStorageToken::reserve(granted.lease, measureNeed, local, start);
        require(measureStage.token.has_value(), "the measure's device stage starts");
        require(policy.usableCapacity().deviceFreeBytes >= viewerNeed,
                "a Background grant leaves the Viewer's last need free");
        // The next Viewer frame is admitted at once beside the measure's stage.
        auto next = policy.reserve(frame(), start);
        require(next.status == GpuProcessingPhaseAdmission::Accepted && next.lease,
                "the measure's phase never delays the next Viewer phase");
    }
    {
        // A Viewer request waits for device room: the measure waits too, even though its small
        // need would fit the room that is free; and when room returns the Viewer goes first.
        GpuProcessingPhasePolicy policy(32);
        policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
        deliverHost(policy, 20 * kGiB, 1);
        const auto start = Clock::now();
        reportDevice(policy, 1, viewerNeed + measureNeed);
        auto holder = policy.reserve(frame(), start);
        require(holder.lease != nullptr, "the first Viewer frame is admitted");
        auto holderStage = NativeImageStorageToken::reserve(holder.lease, viewerNeed, local, start);
        require(holderStage.token.has_value(), "its device stage starts");
        const auto waiting = policy.reserve(frame(), start);
        require(waiting.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                    policy.viewerPriority(start).waiting,
                "the second Viewer frame waits for device room");
        require(policy.usableCapacity().deviceFreeBytes >= measureNeed,
                "the measure's need alone fits the room that is free");
        const auto yielded = policy.reserve(measurePhase(measureNeed), start);
        require(yielded.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                    yielded.domain == GpuProcessingPhaseDomain::Viewer && !yielded.lease,
                "a waiting Viewer request holds the Background measure back");
        holderStage.token.reset();
        holder.lease.reset();
        const auto again = policy.reserve(measurePhase(measureNeed), start);
        require(again.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                    again.domain == GpuProcessingPhaseDomain::Viewer,
                "a released slot goes to the waiting Viewer, not to the measure");
        auto viewer = policy.reserve(frame(), start);
        require(viewer.status == GpuProcessingPhaseAdmission::Accepted && viewer.lease,
                "the waiting Viewer frame is admitted first");
    }
}

void deviceGrowthBeyondTheClaimUsesLiveRoom() {
    GpuProcessingPhaseBudget budget;
    const auto now = std::chrono::steady_clock::now();
    budget.setHostCapacity(1 * kGiB, true, now + std::chrono::seconds(10));
    budget.setDeviceCapacity(1000, true);
    auto grant = budget.reserve({.fixedHostBytes = 10,
                                 .deviceWorkingBytes = 40,
                                 .requestedExecutorBytes = 5000,
                                 .minimumExecutorBytes = 60,
                                 .sourceFree = true});
    require(grant.lease && grant.lease->deviceNeedBytes() == 100 &&
                grant.lease->deviceClaimBytes() == 0 && grant.lease->executorByteLimit() == 960,
            "the phase needs working + minimum and keeps the live room as its ceiling");
    require(budget.snapshot().totalDeviceClaimBytes == 0,
            "nothing is claimed before the device stage");
    const NativeImageStorageIdentity local{.allocatorGeneration = 7, .memoryType = 2, .heap = 1};
    auto image = NativeImageStorageToken::reserve(grant.lease, 600, local, now);
    require(image.token.has_value() && budget.snapshot().totalDeviceClaimBytes == 600,
            "an allocation beyond the claim is admitted against live room");
    auto other = budget.reserve({.fixedHostBytes = 10,
                                 .deviceWorkingBytes = 300,
                                 .minimumExecutorBytes = 0,
                                 .sourceFree = true});
    require(other.lease != nullptr, "another phase is admitted into the remaining room");
    auto otherStage = NativeImageStorageToken::reserve(other.lease, 1, local, now);
    require(otherStage.token && budget.snapshot().totalDeviceClaimBytes == 900,
            "its device stage takes the remaining room");
    auto denied = NativeImageStorageToken::reserve(grant.lease, 200, local, now);
    require(!denied.token && denied.domain == GpuProcessingPhaseDomain::Device,
            "growth beyond the live room is refused at allocation");
    image.token.reset();
    otherStage.token.reset();
    other.lease.reset();
    grant.lease.reset();
    const auto empty = budget.snapshot();
    require(empty.totalDeviceClaimBytes == 0 && empty.deviceReservedBytes == 0 &&
                empty.admittedPhases == 0,
            "growth and claims are returned exactly");
}

// #417: phases run their device stages one at a time, so device admission claims a phase's need
// only from its first native allocation until it holds no native storage. Work whose need fits
// the measured free device memory is admitted beside other admitted work; real allocations are
// still checked against the live room, so an allocation that does not fit is refused.
void deviceClaimsFollowDeviceStages() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    deliverHost(policy, 60 * kGiB, 1);
    // A 2160p preview frame: the working allowance plus one float frame of executor minimum. The
    // device has room for two and a half of those needs, not the eight frames' sum.
    const std::uint64_t pixels = 3840ULL * 2160;
    const auto need = 832 * kMiB + pixels * 16;
    reportDevice(policy, 1, 2 * need + need / 2);
    auto envelope = frame(832 * kMiB);
    envelope.pixels = pixels;
    envelope.minimumSourceBytes = pixels * 16;
    envelope.minimumExecutorBytes = pixels * 16;
    std::vector<GpuProcessingPhaseGrant> grants;
    for (int index = 0; index < 8; ++index) {
        grants.push_back(policy.reserve(envelope));
        require(grants.back().status == GpuProcessingPhaseAdmission::Accepted,
                "eight frames whose need fits the free device memory are all admitted");
    }
    require(policy.usableCapacity().deviceClaimBytes == 0,
            "admitted phases that hold no device memory claim none");
    const auto now = std::chrono::steady_clock::now();
    const NativeImageStorageIdentity local{.allocatorGeneration = 7, .memoryType = 2, .heap = 1};
    auto first = NativeImageStorageToken::reserve(grants[0].lease, 600 * kMiB, local, now);
    require(first.token && policy.usableCapacity().deviceClaimBytes == need &&
                grants[0].lease->deviceClaimBytes() == need - 600 * kMiB,
            "the first allocation of a device stage claims the phase's whole need");
    auto more = NativeImageStorageToken::reserve(grants[0].lease, 300 * kMiB, local, now);
    require(more.token && policy.usableCapacity().deviceClaimBytes == need,
            "allocations within the need come out of the stage claim");
    auto second = NativeImageStorageToken::reserve(grants[1].lease, 100 * kMiB, local, now);
    require(second.token && policy.usableCapacity().deviceClaimBytes == 2 * need,
            "a second concurrent stage claims its own need while the room holds it");
    // The third need no longer fits the live room: the stage claims nothing up front and each
    // allocation is admitted against what is actually free.
    const auto room = need / 2;
    auto third = NativeImageStorageToken::reserve(grants[2].lease, room / 2, local, now);
    require(third.token && policy.usableCapacity().deviceClaimBytes == 2 * need + room / 2,
            "without room for its whole need a stage still allocates what fits");
    auto refused = NativeImageStorageToken::reserve(grants[2].lease, room, local, now);
    require(!refused.token && refused.status == GpuProcessingPhaseAdmission::BudgetRefused &&
                refused.domain == GpuProcessingPhaseDomain::Device,
            "an allocation larger than the live device room is refused, never over-committed");
    auto late = policy.reserve(envelope);
    require(late.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                late.domain == GpuProcessingPhaseDomain::Device,
            "a new phase waits while running stages actually hold the device");
    first.token.reset();
    require(policy.usableCapacity().deviceClaimBytes == 2 * need + room / 2,
            "freed storage returns to its stage's claim while the stage holds storage");
    more.token.reset();
    require(policy.usableCapacity().deviceClaimBytes == need + room / 2 &&
                grants[0].lease->deviceClaimBytes() == 0,
            "the stage ends when the phase holds no storage and its claim is released at once");
    auto restarted = NativeImageStorageToken::reserve(grants[0].lease, 100 * kMiB, local, now);
    require(restarted.token && policy.usableCapacity().deviceClaimBytes == 2 * need + room / 2,
            "a later stage of the same phase claims its need again");
    // #539: a need above the usable room but within the 24 GiB device waits for room other
    // programs hold; only a need above the whole device is refused.
    auto whole = envelope;
    whole.deviceWorkingBytes = 3 * need;
    require(policy.reserve(whole).status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "a need larger than the usable room but within the device waits");
    whole.deviceWorkingBytes = 25 * kGiB;
    require(policy.reserve(whole).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "a need larger than the whole device is refused");
    restarted.token.reset();
    second.token.reset();
    third.token.reset();
    grants.clear();
    const auto settled = policy.snapshot().reservations;
    require(settled.totalDeviceClaimBytes == 0 && settled.deviceReservedBytes == 0 &&
                settled.releasedNativePhases == 8 && settled.peakPhaseDeviceNeedBytes == need &&
                settled.peakPhaseDeviceAllocationBytes == 900 * kMiB &&
                settled.peakOwnedNativeBytes == 1000 * kMiB + room / 2 &&
                settled.peakTotalDeviceClaimBytes == 2 * need + room / 2,
            "claims, allocations and their peaks are returned and recorded exactly");
    require(settled.peakTotalDeviceClaimBytes <=
                settled.peakOwnedNativeBytes + 2 * settled.peakPhaseDeviceNeedBytes,
            "claims stay within the real allocation plus the needs of the running stages");
}

// #417: the stated device need is a ceiling-sized estimate (the working allowance covers the OCIO
// arm's and the transfer's ceilings). Once a phase of the same stated envelope has been released,
// later phases need the largest measured peak plus a quarter, never more than the stated need; an
// allocation beyond it is still admitted against the live room.
void deviceNeedFollowsMeasuredPeaks() {
    GpuProcessingPhaseBudget budget;
    const auto now = std::chrono::steady_clock::now();
    budget.setHostCapacity(64 * kGiB, true, now + std::chrono::seconds(60));
    budget.setDeviceCapacity(2 * kGiB, true);
    const std::uint64_t frameBytes = 1920ULL * 1080 * 16;
    const GpuProcessingPhaseRequest request{.fixedHostBytes = 64 * kMiB,
                                            .deviceWorkingBytes = 832 * kMiB,
                                            .requestedExecutorBytes = 2 * kGiB,
                                            .minimumExecutorBytes = frameBytes,
                                            .sourceFree = true};
    const auto stated = 832 * kMiB + frameBytes;
    const NativeImageStorageIdentity local{.allocatorGeneration = 7, .memoryType = 2, .heap = 1};
    const auto run = [&](const GpuProcessingPhaseRequest& envelope, const std::uint64_t bytes) {
        auto grant = budget.reserve(envelope);
        require(grant.lease != nullptr, "the frame is admitted");
        const auto need = grant.lease->deviceNeedBytes();
        auto image = NativeImageStorageToken::reserve(grant.lease, bytes, local, now);
        require(image.token.has_value(), "the frame allocates what it uses");
        return need;
    };
    require(run(request, 200 * kMiB) == stated, "the first frame of an envelope needs the stated");
    require(run(request, 400 * kMiB) == 250 * kMiB,
            "the next frame needs the measured peak plus a quarter");
    require(budget.snapshot().peakTotalDeviceClaimBytes <= stated &&
                budget.snapshot().peakOwnedNativeBytes == 400 * kMiB,
            "an allocation beyond the measured need is admitted against the live room");
    require(run(request, 10 * kMiB) == 500 * kMiB, "the largest measured peak is kept");
    require(run(request, 10 * kMiB) == 500 * kMiB,
            "a frame that ended early never lowers the need");
    auto other = request;
    other.minimumExecutorBytes = 2 * frameBytes;
    require(run(other, kMiB) == 832 * kMiB + 2 * frameBytes,
            "another envelope (another plan) starts from its own stated need");
    auto large = request;
    large.deviceWorkingBytes = 2 * kGiB;
    require(budget.reserve(large).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "a stated need larger than the whole device is still refused");
    // #425: a retained device cache (root-owned storage) leaves little room beside it. A measured
    // envelope's executor ceiling still covers its measured need, so the frame is never squeezed
    // below what it is known to use; its allocations are admitted against the live room.
    auto cacheEnvelope = request; // Another plan, so its peak does not change this envelope's.
    cacheEnvelope.minimumExecutorBytes = 3 * frameBytes;
    auto cacheOwner = budget.reserve(cacheEnvelope);
    require(cacheOwner.lease != nullptr, "the cache-owning frame is admitted");
    auto cacheImage =
        NativeImageStorageToken::reserve(cacheOwner.lease, 2 * kGiB - 600 * kMiB, local, now);
    require(cacheImage.token.has_value(), "the retained cache image is admitted");
    cacheOwner.lease.reset();
    auto squeezed = budget.reserve(request);
    require(squeezed.lease && squeezed.lease->deviceNeedBytes() == 500 * kMiB &&
                squeezed.lease->executorByteLimit() >= 500 * kMiB,
            "a measured envelope's executor ceiling covers its measured need beside a full cache");
    squeezed.lease.reset();
    cacheImage.token.reset();
    const auto settled = budget.snapshot();
    require(settled.totalDeviceClaimBytes == 0 && settled.admittedPhases == 0,
            "every claim returns");
}

// #343: a prepared scene's images at their real sizes can need more than the admission's minimum
// (each image taken as one frame). The raise grows the device need by that excess only. Before an
// envelope is measured, the need becomes the stated working allowance plus the real images; once it
// is measured (#417), the measured need plus the excess, never the stated working allowance again,
// which phases of this envelope were measured not to use.
void raisedNeedKeepsTheMeasuredNeed() {
    GpuProcessingPhaseBudget budget;
    const auto now = std::chrono::steady_clock::now();
    budget.setHostCapacity(64 * kGiB, true, now + std::chrono::seconds(60));
    budget.setDeviceCapacity(2 * kGiB, true);
    const std::uint64_t frameBytes = 1920ULL * 1080 * 16;
    const GpuProcessingPhaseRequest request{.fixedHostBytes = 64 * kMiB,
                                            .deviceWorkingBytes = 832 * kMiB,
                                            .requestedExecutorBytes = 2 * kGiB,
                                            .minimumExecutorBytes = 2 * frameBytes,
                                            .sourceFree = true};
    const auto stated = 832 * kMiB + 2 * frameBytes;
    const auto real = 3 * frameBytes + 4096; // three padded images and their metadata
    const NativeImageStorageIdentity local{.allocatorGeneration = 7, .memoryType = 2, .heap = 1};

    auto first = budget.reserve(request);
    require(first.lease && first.lease->deviceNeedBytes() == stated,
            "an unmeasured envelope is admitted at its stated need");
    require(first.lease->raiseExecutorNeed(2 * frameBytes) ==
                    GpuProcessingPhaseAdmission::Accepted &&
                first.lease->deviceNeedBytes() == stated,
            "a raise to the admitted minimum changes nothing");
    require(first.lease->raiseExecutorNeed(real) == GpuProcessingPhaseAdmission::Accepted &&
                first.lease->deviceNeedBytes() == 832 * kMiB + real &&
                first.lease->executorByteLimit() >= real,
            "an unmeasured envelope needs its working allowance and the real images");
    require(first.lease->raiseExecutorNeed(real) == GpuProcessingPhaseAdmission::Accepted &&
                first.lease->deviceNeedBytes() == 832 * kMiB + real,
            "a repeated raise does not count the images twice");
    {
        auto image = NativeImageStorageToken::reserve(first.lease, 200 * kMiB, local, now);
        require(image.token.has_value(), "the first frame allocates what it uses");
    }
    first.lease.reset();

    auto measured = budget.reserve(request);
    require(measured.lease && measured.lease->deviceNeedBytes() == 250 * kMiB,
            "the next frame is admitted at the measured peak plus a quarter");
    require(
        measured.lease->raiseExecutorNeed(real) == GpuProcessingPhaseAdmission::Accepted &&
            measured.lease->deviceNeedBytes() == 250 * kMiB + (real - 2 * frameBytes) &&
            measured.lease->executorByteLimit() >= real,
        "a measured envelope grows by the images beyond its minimum, not the working allowance");
    measured.lease.reset();

    // The device itself still bounds a raise, as it bounds admission.
    auto bounded = budget.reserve(request);
    require(bounded.lease != nullptr, "a third frame is admitted");
    require(bounded.lease->raiseExecutorNeed(2 * kGiB - 832 * kMiB + 1) ==
                    GpuProcessingPhaseAdmission::BudgetRefused &&
                bounded.lease->deviceNeedBytes() == 250 * kMiB,
            "a stated need larger than the device is refused and changes nothing");
    bounded.lease.reset();
    const auto settled = budget.snapshot();
    require(settled.totalDeviceClaimBytes == 0 && settled.admittedPhases == 0,
            "every raised claim returns");
}

// #472: the many-layer RAM Preview fill. A 1080p stack of 19 footage layers cycling through six
// media (four 1080p, two 2160p) states every reference as a float frame (34 1080p frames), while
// preparation decodes each distinct source once and retains 12 such frames. Eight frames in flight
// at the stated need would claim all usable RAM; after the first frame has been measured they
// claim from what a frame retains, and the rest stays free for the preview cache.
void hostClaimsFollowRetainedSources() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    deliverHost(policy, 17 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    const std::uint64_t pixels = 1920ULL * 1080;
    const std::uint64_t frameBytes = pixels * 16;
    const auto retained = 12 * frameBytes;
    const GpuProcessingPhaseEnvelope envelope{.output = GpuProcessingPhaseOutput::PackedPreview,
                                              .pixels = pixels,
                                              .metadataBytes = 64 * kMiB,
                                              .requestedSourceBytes = 64 * kGiB,
                                              .minimumSourceBytes = 34 * frameBytes,
                                              .requestedExecutorBytes = 64 * kGiB,
                                              .minimumExecutorBytes = frameBytes,
                                              .deviceWorkingBytes = 832 * kMiB};
    std::vector<GpuProcessingPhaseGrant> grants;
    for (int index = 0; index < 8; ++index) {
        grants.push_back(policy.reserve(envelope));
        if (!grants.back().lease)
            break;
    }
    require(grants.size() < 8 &&
                grants.back().status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "at the stated source need eight frames do not fit in 17 GiB");
    const auto fixed = grants.front().lease->hostBytes() - 3 * 34 * frameBytes;
    require(grants.front().lease->finishSourcePreparation(retained),
            "the first frame finishes preparation with its distinct sources");
    grants.clear();
    for (int index = 0; index < 8; ++index) {
        grants.push_back(policy.reserve(envelope));
        require(grants.back().lease &&
                    grants.back().lease->sourceClaimBytes() == retained + retained / 4,
                "measured frames claim the retained sources plus a quarter");
    }
    const auto preparing = policy.usableCapacity();
    require(preparing.hostClaimBytes == 8 * (fixed + 3 * (retained + retained / 4)) &&
                preparing.hostFreeBytes > 3 * kGiB,
            "eight preparing frames leave room for the preview cache");
    for (auto& grant : grants)
        require(grant.lease->finishSourcePreparation(retained), "each frame finishes preparation");
    const auto prepared = policy.usableCapacity();
    require(prepared.hostClaimBytes == 8 * (fixed + retained),
            "prepared frames claim what they retain");
    const auto settled = policy.snapshot().reservations;
    require(settled.finishedSourcePhases == 9 &&
                settled.finishedPhaseRetainedSourceBytes == 9 * retained &&
                settled.finishedPhaseSourceNeedBytes ==
                    34 * frameBytes + 8 * (retained + retained / 4),
            "per-frame source needs and retention are recorded");
    // Claims against measured use: every measured frame's need covers what it retained, within a
    // quarter more (the stated need was 2.8 times it).
    const auto measuredNeed = settled.finishedPhaseSourceNeedBytes - 34 * frameBytes;
    require(measuredNeed >= 8 * retained && 4 * measuredNeed <= 5 * 8 * retained,
            "measured source needs cover the retention within a quarter");
    grants.clear();
    require(policy.usableCapacity().hostClaimBytes == 0, "every claim returns");
}
} // namespace

// Preview width is bounded by concurrency and by a quarter of the free capacity, never by how
// much memory happens to be free: a nearly empty 64 GB machine gets one frame per hardware thread.
void previewWidthIsBounded() {
    const std::uint64_t pixels = 2048ULL * 858;
    const std::uint64_t perFrameHost = pixels * (8 + 3 * 16) + 64 * kMiB;
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    deliverHost(policy, 60 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    const auto ample = previewFramesThatFit(policy, pixels, 512 * kMiB);
    const auto threads = std::max(1U, std::thread::hardware_concurrency());
    // A quarter of 60 GiB holds about ninety of these frames, so up to that many threads decide.
    require(ample <= threads && ample >= std::min<std::size_t>(threads, 64),
            "an ample machine runs one preview frame in flight per hardware thread");
    deliverHost(policy, 13 * perFrameHost, 2);
    require(previewFramesThatFit(policy, pixels, 512 * kMiB) <= 3,
            "a run sizes itself to a quarter of the free host capacity");
    deliverHost(policy, perFrameHost + perFrameHost / 2, 3);
    require(previewFramesThatFit(policy, pixels, 512 * kMiB) == 1,
            "one frame still runs when only one fits");
    deliverHost(policy, perFrameHost / 2, 4);
    require(previewFramesThatFit(policy, pixels, 512 * kMiB) == 1,
            "one frame waits for room other programs hold when none fits now (#539)");
}

// #411: claims whose backing is allocated are already in measured usage. Reusable pools were held
// back by every claim, retained preview frames included, so they froze while a RAM preview filled
// and the preview evicted its own frames. Only the unallocated part is pending.
void pendingClaimsExcludeAllocatedBacking() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    deliverHost(policy, 20 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    auto grant = policy.reserve(frame());
    require(grant.lease != nullptr, "the frame is admitted");
    auto usable = policy.usableCapacity();
    require(usable.hostClaimBytes > 0 && usable.hostPendingClaimBytes == usable.hostClaimBytes,
            "a claim with nothing allocated yet is wholly pending");
    struct Backing final {
        std::vector<std::byte> bytes = std::vector<std::byte>(kMiB);
    };
    constexpr std::uint64_t allocated = sizeof(Backing) + kMiB;
    auto backing = bloom::render::adoptProcessingHostBacking(std::make_unique<Backing>(),
                                                             grant.lease, allocated, allocated);
    require(backing != nullptr, "the phase adopts its allocated backing");
    const auto adopted = policy.usableCapacity();
    require(adopted.hostClaimBytes == usable.hostClaimBytes &&
                adopted.hostPendingClaimBytes == usable.hostClaimBytes - allocated,
            "allocated backing leaves the pending claims, not the claims");
    backing.reset();
    grant.lease.reset();
    require(policy.usableCapacity().hostPendingClaimBytes == 0, "released claims are not pending");
}

// #472: preparation decodes and converts one source at a time. A 1080p stack of 25 distinct
// layers claims its sources plus one layer's scratch while it prepares, not three times every
// layer, so eight frames fit where the 3x overlap claimed more than all usable RAM; the claim
// stays within one layer's scratch of what a frame actually holds.
void hostClaimsTrackOneSourcesScratch() {
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    deliverHost(policy, 17 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    const std::uint64_t pixels = 1920ULL * 1080;
    const std::uint64_t frameBytes = pixels * 16;
    constexpr std::uint64_t layers = 25;
    GpuProcessingPhaseEnvelope envelope{.output = GpuProcessingPhaseOutput::PackedPreview,
                                        .pixels = pixels,
                                        .metadataBytes = 64 * kMiB,
                                        .requestedSourceBytes = 64 * kGiB,
                                        .minimumSourceBytes = layers * frameBytes,
                                        .largestSourceBytes = frameBytes,
                                        .requestedExecutorBytes = 64 * kGiB,
                                        .minimumExecutorBytes = frameBytes,
                                        .deviceWorkingBytes = 832 * kMiB};
    auto wholeNeed = envelope;
    wholeNeed.largestSourceBytes = 0;
    std::vector<GpuProcessingPhaseGrant> grants;
    for (int index = 0; index < 8; ++index) {
        grants.push_back(policy.reserve(wholeNeed));
        if (!grants.back().lease)
            break;
    }
    require(grants.size() < 8, "with 3x the whole source need eight frames do not fit 17 GiB");
    grants.clear();
    for (int index = 0; index < 8; ++index) {
        grants.push_back(policy.reserve(envelope));
        require(grants.back().lease && grants.back().lease->sourceScratchBytes() == 2 * frameBytes,
                "with one source's scratch eight preparing frames fit");
    }
    const auto fixed = grants.front().lease->hostBytes() - (layers + 2) * frameBytes;
    const auto preparing = policy.usableCapacity();
    require(preparing.hostClaimBytes == 8 * (fixed + (layers + 2) * frameBytes),
            "each preparing frame claims its sources plus one source's scratch");
    // Claim against measured use: a frame retains its 25 layers and holds one layer's decoded and
    // converted copies while preparing, so its claim is what it holds, not 3x its sources.
    for (auto& grant : grants)
        require(grant.lease->finishSourcePreparation(layers * frameBytes),
                "each frame finishes preparation with its sources");
    require(policy.usableCapacity().hostClaimBytes == 8 * (fixed + layers * frameBytes),
            "prepared frames claim exactly what they retain");
    grants.clear();
    require(policy.usableCapacity().hostClaimBytes == 0, "every claim returns");
}

// #539: frames in flight follow measured per-frame needs and the usable capacity measured now.
// Once frames have run, their measured host claim and device peak size the width; when another
// program takes VRAM the width shrinks at the next frame boundary, and nothing is a fixed cap.
void previewWidthFollowsMeasuredRoom() {
    const std::uint64_t pixels = 2048ULL * 858;
    GpuProcessingPhasePolicy policy(32);
    policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
    deliverHost(policy, 60 * kGiB, 1);
    reportDevice(policy, 1, 22 * kGiB);
    auto envelope = frame(0);
    envelope.pixels = pixels;
    auto grant = policy.reserve(envelope);
    require(grant.lease != nullptr, "a frame is admitted");
    // The frame's scene held 2 GiB of native storage at its peak.
    const NativeImageStorageIdentity local{.allocatorGeneration = 7, .memoryType = 2, .heap = 1};
    auto image = NativeImageStorageToken::reserve(grant.lease, 2 * kGiB, local,
                                                  std::chrono::steady_clock::now());
    require(image.token.has_value(), "the scene allocates its live set");
    image.token.reset();
    grant.lease.reset();
    require(policy.snapshot().reservations.recentPhaseDevicePeakBytes == 2 * kGiB,
            "a released frame records its measured device peak");
    const auto threads = std::max(1U, std::thread::hardware_concurrency());
    require(previewFramesThatFit(policy, pixels, 0) == std::min<std::size_t>(threads, 11),
            "22 GiB of usable VRAM holds eleven 2 GiB frames");
    reportDevice(policy, 1, 7 * kGiB);
    require(previewFramesThatFit(policy, pixels, 0) == std::min<std::size_t>(threads, 3),
            "another program taking VRAM shrinks the width at the next boundary");
    reportDevice(policy, 1, kGiB);
    require(previewFramesThatFit(policy, pixels, 0) == 1,
            "one frame waits when another program holds the room it needs");
    reportDevice(policy, 1, 0);
    require(previewFramesThatFit(policy, pixels, 0) == 1,
            "a device another program has filled still keeps one frame waiting");
    auto now = frame(0);
    now.pixels = pixels;
    require(policy.reserve(now).status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "admission makes it wait rather than refusing it");
    reportDevice(policy, 1, 22 * kGiB);
    require(policy.reserve(now).status == GpuProcessingPhaseAdmission::Accepted,
            "it runs once the room comes back");
}

// #682 #719: a RAM fill planned for a small card (6, 8 and 12 GiB), with no device and no frames.
// Every frame leaves a cached image on the device, as the scene cache does, so the card fills.
// What a fill must hold on any card: admission never claims more than the card has and never owns
// more than it; a frame that finds the card full waits (it is not refused), asks the caches for
// exactly its shortfall, and is admitted once they gave it back; and when the owner keeps the next
// frame's need free (deviceWorkReserveShortfall), no frame waits at all. How often a frame waits
// in a real fill depends on when the owner thread runs, which is not a property to test.
void smallCardFillStaysInsideTheCard() {
    constexpr std::uint64_t pixels = 1920ULL * 1080;
    constexpr std::uint64_t imageBytes = 256 * kMiB;
    constexpr int frames = 100;
    std::size_t previousWidth = 0;
    for (const std::uint64_t card : {6 * kGiB, 8 * kGiB, 12 * kGiB}) {
        for (const bool ownerKeepsReserve : {false, true}) {
            GpuProcessingPhasePolicy policy(4);
            policy.setHeadroom({.ramBytes = 0, .vramBytes = 0});
            deliverHost(policy, 8 * kGiB, 1);
            std::deque<NativeImageStorageToken> cache;
            const auto owned = [&] {
                return static_cast<std::uint64_t>(cache.size()) * imageBytes;
            };
            // The modelled card: its size is the ceiling, and the driver's live budget is what the
            // cached images leave of it.
            const auto report = [&] {
                require(policy.reportDevice(1, {.source = GpuAllocationBudgetSource::MemoryBudget,
                                                .device_local_bytes = card,
                                                .available_bytes = card - owned()}),
                        "the modelled card reports");
            };
            const auto evict = [&](const std::uint64_t bytes) {
                std::uint64_t freed = 0;
                while (freed < bytes && !cache.empty()) {
                    cache.pop_front();
                    freed += imageBytes;
                }
                report();
                return freed;
            };
            report();
            require(policy.snapshot().reservations.deviceCapacityBytes == card &&
                        policy.snapshot().reservations.deviceCeilingBytes == card,
                    "a modelled card's capacity and ceiling are the card");
            auto request = std::make_shared<GpuDeviceReclaimRequest>();
            registerDeviceReclaim(policy, request);
            auto envelope = frame(832 * kMiB);
            envelope.output = GpuProcessingPhaseOutput::PackedPreview;
            envelope.pixels = pixels;
            envelope.minimumSourceBytes = pixels * 16;
            envelope.minimumExecutorBytes = pixels * 16;
            const NativeImageStorageIdentity local{
                .allocatorGeneration = 7, .memoryType = 2, .heap = 1};
            int waited = 0;
            for (int index = 0; index < frames; ++index) {
                auto grant = policy.reserve(envelope);
                if (grant.status == GpuProcessingPhaseAdmission::AwaitingCapacity) {
                    ++waited;
                    require(grant.domain == GpuProcessingPhaseDomain::Device && request->pending(),
                            "a frame that finds the card full waits and asks the device caches");
                    const auto need = policy.lastAwaitingNeed();
                    const auto wanted =
                        deviceReclaimBytes(*request,
                                           {.source = GpuAllocationBudgetSource::MemoryBudget,
                                            .device_local_bytes = card,
                                            .available_bytes = card - owned()},
                                           0);
                    require(need && wanted == need->shortfallBytes && wanted != 0,
                            "the owner is asked for exactly the waiting frame's shortfall");
                    require(evict(wanted) >= wanted, "the caches hold what the frame lacks");
                    grant = policy.reserve(envelope);
                }
                require(grant.status == GpuProcessingPhaseAdmission::Accepted && grant.lease,
                        "a waiting frame is admitted once the caches gave its shortfall back");
                auto image =
                    NativeImageStorageToken::reserve(grant.lease, imageBytes, local, Clock::now());
                require(image.token.has_value(), "the frame's device stage allocates its image");
                const auto during = policy.snapshot().reservations;
                require(during.totalDeviceClaimBytes <= card && during.deviceCapacityBytes == card,
                        "admission never claims more than the card has");
                policy.noteDeviceWorkNeed(grant.lease->deviceNeedBytes());
                require(image.token->promote(), "the finished image is published to the cache");
                cache.push_back(std::move(*image.token));
                grant.lease.reset();
                report();
                if (ownerKeepsReserve) {
                    // The owner's pass after a frame: give back what the next frame of the same
                    // work lacks, before it asks.
                    const auto shortfall = deviceWorkReserveShortfall(policy);
                    if (shortfall != 0)
                        require(evict(shortfall) >= shortfall, "the caches cover the reserve");
                    require(deviceWorkReserveShortfall(policy) == 0,
                            "the next frame's need is free after the owner's pass");
                }
            }
            const auto filled = policy.snapshot().reservations;
            require(filled.peakOwnedNativeBytes != 0 && filled.peakOwnedNativeBytes <= card &&
                        filled.peakTotalDeviceClaimBytes <= card,
                    "a whole fill never owns or claims more device memory than the card has");
            require(owned() + imageBytes > card - 2 * 832 * kMiB,
                    "the fill really filled the modelled card");
            if (ownerKeepsReserve)
                require(waited == 0, "no frame waits when the owner keeps the next need free");
            else
                require(waited != 0, "without the owner's pass the full card makes frames wait");
            if (ownerKeepsReserve) {
                // Frames in flight shrink to fit: as many measured frames as the card's usable
                // memory holds, never more, and no fewer on a larger card.
                const auto width = previewFramesThatFit(policy, pixels, 832 * kMiB);
                const auto perFrame = 832 * kMiB + filled.recentPhaseDevicePeakBytes;
                require(width >= 1 && width * perFrame <= std::max(card, perFrame) &&
                            width >= previousWidth,
                        "frames in flight fit the card and shrink with it");
                previousWidth = width;
            }
            cache.clear();
            require(policy.usableCapacity().deviceClaimBytes == 0, "every device claim returns");
        }
    }
}

int main() {
    try {
        headroomIsTheOnlyMargin();
        previewWidthIsBounded();
        phasesClaimOnlyTheirNeed();
        contentionWaitsAndResumesOnRelease();
        exhaustionIsRefusedWithReadableCounters();
        manyConcurrentPhases();
        strictViewerPriority();
        backgroundMeasureYieldsToTheViewer();
        retentionIsEvictedBeforeWaiting();
        cachesYieldToWork();
        previewFillWorkIsNotStarved();
        deviceCachesYieldToWork();
        deviceGrowthBeyondTheClaimUsesLiveRoom();
        deviceClaimsFollowDeviceStages();
        deviceNeedFollowsMeasuredPeaks();
        raisedNeedKeepsTheMeasuredNeed();
        hostClaimsFollowRetainedSources();
        hostClaimsTrackOneSourcesScratch();
        previewWidthFollowsMeasuredRoom();
        pendingClaimsExcludeAllocatedBacking();
        smallCardFillStaysInsideTheCard();
    } catch (const std::exception& error) {
        std::cerr << "admission pressure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "admission pressure: all expectations passed\n";
    return EXIT_SUCCESS;
}
