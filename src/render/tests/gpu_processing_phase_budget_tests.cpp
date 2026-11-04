#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/render/native_buffer_storage.hpp>
#include <bloom/render/native_image_storage.hpp>

#include <array>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace bloom::render;
void require(const bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
constexpr NativeBufferStorageIdentity kDeviceLocal{
    .allocatorGeneration = 1, .memoryType = 0, .heap = 0, .deviceLocal = true};
// Starts a phase's device stage with one device-local byte, as its first native allocation does.
NativeBufferStorageToken startDeviceStage(const std::shared_ptr<GpuProcessingPhaseLease>& lease) {
    auto grant = NativeBufferStorageToken::reserve(lease, 1, kDeviceLocal);
    require(grant.token.has_value(), "a device stage starts with its first native allocation");
    return std::move(*grant.token);
}
GpuProcessingPhaseRequest request() {
    return {.fixedHostBytes = 10,
            .requestedSourceBytes = 20,
            .minimumSourceBytes = 20,
            .deviceWorkingBytes = 10,
            .requestedExecutorBytes = 20,
            .minimumExecutorBytes = 20};
}
void readinessAndRollback() {
    GpuProcessingPhaseBudget budget;
    require(budget.reserve(request()).status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "unknown host must await capacity");
    budget.setHostCapacity(70, true);
    require(budget.reserve(request()).domain == GpuProcessingPhaseDomain::Device,
            "native request must await device independently");
    budget.setDeviceCapacity(29, true);
    require(budget.reserve(request()).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "failed device half must refuse");
    require(budget.snapshot().hostReservedBytes == 0 && budget.snapshot().admittedPhases == 0,
            "failed paired admission must leave host/count untouched");
    budget.setDeviceCapacity(30, true);
    auto grant = budget.reserve(request());
    require(grant.lease && grant.lease->hostBytes() == 70 && grant.lease->deviceBytes() == 30,
            "exact paired minimum must fit");
    budget.setHostCapacity(0, true);
    require(budget.reserve(request()).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "measured zero must refuse instead of await");
    require(budget.snapshot().hostReservedBytes == 70,
            "shrinking capacity must not free live phase");
    budget.setHostCapacity(std::nullopt, false);
    require(budget.snapshot().hostCapacityBytes == 0 &&
                budget.reserve(request()).status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "unknown closes new admission without losing prior cap");
    grant.lease.reset();
    require(budget.snapshot().hostReservedBytes == 0 && budget.snapshot().deviceReservedBytes == 0,
            "release returns both halves after shrink");
    budget.setHostCapacity(10, true);
    auto hostOnly = request();
    hostOnly.native = false;
    hostOnly.sourceFree = true;
    hostOnly.minimumSourceBytes = 0;
    hostOnly.deviceWorkingBytes = 0;
    hostOnly.requestedExecutorBytes = 0;
    hostOnly.minimumExecutorBytes = 0;
    budget.setDeviceCapacity(std::nullopt, false);
    require(budget.reserve(hostOnly).status == GpuProcessingPhaseAdmission::Accepted,
            "admitted CPU route does not need device readiness");
}
void emptyAndZeroGrant() {
    GpuProcessingPhaseBudget budget;
    GpuProcessingPhaseRequest empty;
    empty.sourceFree = true;
    budget.setHostCapacity(0, true);
    require(budget.reserve(empty).domain == GpuProcessingPhaseDomain::Host,
            "measured zero host wins even while device capacity is unknown");
    budget.setDeviceCapacity(0, true);
    require(budget.reserve(empty).domain == GpuProcessingPhaseDomain::Host,
            "zero host refuses even an empty preparation request");
    budget.setHostCapacity(std::nullopt, false);
    require(budget.reserve(empty).domain == GpuProcessingPhaseDomain::Device,
            "measured zero device wins even while host capacity is unknown");
    budget.setHostCapacity(10, true);
    require(budget.reserve(empty).domain == GpuProcessingPhaseDomain::Device,
            "zero applicable device refuses even an empty preparation request");
    budget.setDeviceCapacity(10, true);
    empty.sourceFree = false;
    empty.fixedHostBytes = 10;
    empty.requestedSourceBytes = 1;
    require(budget.reserve(empty).domain == GpuProcessingPhaseDomain::Host,
            "positive source request cannot receive a zero cap");
    empty.sourceFree = true;
    empty.deviceWorkingBytes = 10;
    empty.requestedExecutorBytes = 1;
    require(budget.reserve(empty).domain == GpuProcessingPhaseDomain::Device,
            "positive executor request cannot receive a zero cap");
    require(budget.snapshot().admittedPhases == 0, "zero-grant refusals consume no request slots");
}
void sizingAndLifetime() {
    GpuProcessingPhaseBudget budget(1);
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    budget.setHostCapacity(gib, true);
    budget.setDeviceCapacity(gib, true);
    constexpr std::uint64_t fullPixels = 1920ULL * 1080;
    auto full = request();
    full.fixedHostBytes = 2 * fullPixels * 4 + 4096;
    full.requestedSourceBytes = gib;
    full.minimumSourceBytes = 0;
    full.sourceFree = true;
    full.deviceWorkingBytes = 2 * fullPixels * 16;
    full.requestedExecutorBytes = gib;
    full.minimumExecutorBytes = 4 * fullPixels * 16;
    auto grant = budget.reserve(full);
    // The executor ceiling is the live room; only working + minimum executor bytes are the need,
    // and that need is claimed during the device stage only.
    require(
        grant.lease && grant.lease->sourceByteLimit() == 0 &&
            grant.lease->hostBytes() == full.fixedHostBytes && grant.lease->deviceBytes() == gib &&
            grant.lease->deviceNeedBytes() ==
                full.deviceWorkingBytes + full.minimumExecutorBytes &&
            grant.lease->deviceClaimBytes() == 0 && budget.snapshot().totalDeviceClaimBytes == 0,
        "1080p source-free envelope states its need and keeps the room as an unclaimed ceiling");
    {
        auto stage = startDeviceStage(grant.lease);
        require(budget.snapshot().totalDeviceClaimBytes ==
                    full.deviceWorkingBytes + full.minimumExecutorBytes,
                "the device stage claims exactly the stated need");
    }
    require(budget.snapshot().totalDeviceClaimBytes == 0,
            "the device stage releases its claim when the phase holds no native storage");
    auto alias = grant.lease;
    grant.lease->markUnproven();
    alias->markUnproven();
    grant.lease.reset();
    require(budget.snapshot().admittedPhases == 1 && budget.snapshot().unprovenPhases == 1,
            "unproven aliases charge once until last owner retires");
    require(budget.reserve(full).domain == GpuProcessingPhaseDomain::RequestCount,
            "generation retention cannot grow beyond bounded requests");
    alias.reset();
    require(budget.snapshot().admittedPhases == 0 && budget.snapshot().unprovenPhases == 0,
            "last alias releases retained unproven reservation");
    full.sourceFree = false;
    auto media = budget.reserve(full);
    require(media.lease && media.lease->sourceByteLimit() == gib - full.fixedHostBytes &&
                media.lease->hostBytes() == full.fixedHostBytes,
            "the media source ceiling is the whole remaining host room and is not claimed");
    media.lease.reset();
    GpuProcessingPhaseBudget concurrent(2);
    concurrent.setHostCapacity(1000, true);
    concurrent.setDeviceCapacity(1000, true);
    GpuProcessingPhaseRequest fitting{.fixedHostBytes = 40,
                                      .requestedSourceBytes = 100,
                                      .minimumSourceBytes = 100,
                                      .deviceWorkingBytes = 100,
                                      .requestedExecutorBytes = 500,
                                      .minimumExecutorBytes = 500};
    auto first = concurrent.reserve(fitting);
    require(first.lease && first.lease->executorByteLimit() == 500,
            "executor beyond a quarter admits when actual device capacity fits");
    auto firstStage = startDeviceStage(first.lease);
    auto held = concurrent.reserve(fitting);
    require(!held.lease && held.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                held.domain == GpuProcessingPhaseDomain::Device,
            "a running device stage's claim makes a second phase that would overcommit wait");
    firstStage = {};
    first.lease.reset();
    held = concurrent.reserve(fitting);
    require(held.lease != nullptr, "retired claims restore paired admission");
    held.lease.reset();
    const auto max = std::numeric_limits<std::uint64_t>::max();
    budget.setHostCapacity(max, true);
    budget.setDeviceCapacity(max, true);
    auto huge = request();
    huge.fixedHostBytes = max - 2;
    huge.minimumSourceBytes = 1;
    require(budget.reserve(huge).domain == GpuProcessingPhaseDomain::Host,
            "overflow-sized minimum refuses before multiplication");
    huge.minimumSourceBytes = 0;
    huge.requestedSourceBytes = 0;
    huge.requestedExecutorBytes = max;
    huge.minimumExecutorBytes = 0;
    auto bounded = budget.reserve(huge);
    require(bounded.lease && bounded.lease->hostBytes() == max - 2,
            "wide valid envelope remains bounded without overflow");
    bounded.lease.reset();
    std::shared_ptr<GpuProcessingPhaseLease> surviving;
    {
        GpuProcessingPhaseBudget temporary;
        temporary.setHostCapacity(70, true);
        temporary.setDeviceCapacity(30, true);
        surviving = temporary.reserve(request()).lease;
    }
    require(surviving && surviving->hostBytes() == 70,
            "native owner lease survives composition-root destruction");
    surviving.reset();
}
void retryWatermarks() {
    GpuProcessingPhaseBudget budget(4);
    budget.setHostCapacity(1000, true);
    budget.setDeviceCapacity(1000, true);
    const auto baseline = budget.snapshot().admissionRevision;
    budget.setHostCapacity(1000, true);
    require(budget.snapshot().admissionRevision == baseline,
            "identical samples do not create readiness events");
    auto own = budget.reserve(request());
    require(own.lease && own.admissionRevision == baseline,
            "atomic admission captures exact refusal baseline");
    const auto ignoredOwnRelease = own.lease->revisionAfterOwnRelease();
    auto alias = own.lease;
    own.lease.reset();
    require(budget.snapshot().admissionRevision == baseline,
            "nonlast alias is not a release event");
    alias.reset();
    require(budget.snapshot().admissionRevision == ignoredOwnRelease,
            "failed attempt's own last release alone cannot exceed its retry watermark");
    own = budget.reserve(request());
    const auto watermark = own.lease->revisionAfterOwnRelease();
    auto external = budget.reserve(request());
    require(external.lease != nullptr, "competing phase admitted");
    external.lease.reset(); // Genuine external release before own refusal delivery.
    own.lease.reset();
    require(budget.snapshot().admissionRevision > watermark,
            "external release before failed delivery remains visible beyond excluded own release");
    own = budget.reserve(request());
    const auto capacityWatermark = own.lease->revisionAfterOwnRelease();
    budget.setHostCapacity(1100, true);
    own.lease.reset();
    require(budget.snapshot().admissionRevision > capacityWatermark,
            "capacity change is not erased by failed attempt disposal");
}
void finishedPreparationReleasesDecodeScratch() {
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(1000, true);
    budget.setDeviceCapacity(1000, true);
    auto grant = budget.reserve({.fixedHostBytes = 100,
                                 .requestedSourceBytes = 300,
                                 .minimumSourceBytes = 300,
                                 .deviceWorkingBytes = 1000});
    require(grant.lease && budget.snapshot().totalHostClaimBytes == 1000,
            "preparation initially protects its full decode/conversion/upload overlap");
    const NativeBufferStorageIdentity identity{
        .allocatorGeneration = 1, .memoryType = 1, .heap = 0, .deviceLocal = false};
    auto blocked = NativeBufferStorageToken::reserveProcessing(grant.lease, 64, identity);
    require(!blocked.token, "unneeded decode reservation previously starved native scratch");
    auto earlyUpload = NativeBufferStorageToken::reserveUpload(grant.lease, 10, identity);
    require(earlyUpload.token && !grant.lease->finishSourcePreparation(20),
            "preparation cannot change host accounting while an earlier upload is live");
    earlyUpload.token.reset();
    require(!grant.lease->finishSourcePreparation(301),
            "final source storage cannot exceed its admitted allowance");
    require(grant.lease->finishSourcePreparation(20) &&
                budget.snapshot().totalHostClaimBytes == 120,
            "finished preparation retains actual sources, not unused decode or upload storage");
    require(!grant.lease->finishSourcePreparation(20) &&
                budget.snapshot().totalHostClaimBytes == 120,
            "completion cannot release the same reservation twice");
    auto scratch = NativeBufferStorageToken::reserveProcessing(grant.lease, 64, identity);
    require(scratch.token && budget.snapshot().totalHostClaimBytes == 184,
            "native scratch can use released headroom without changing the device limit");
    budget.setHostCapacity(184, true);
    auto exhausted = NativeBufferStorageToken::reserveUpload(grant.lease, 1, identity);
    require(!exhausted.token && budget.snapshot().totalHostClaimBytes == 184,
            "deferred upload admission still refuses real host exhaustion without a partial claim");
    budget.setHostCapacity(1000, true);
    auto upload = NativeBufferStorageToken::reserveUpload(grant.lease, 300, identity);
    require(upload.token && budget.snapshot().totalHostClaimBytes == 484,
            "native staging acquires its real host allocation within the original upload ceiling");
    require(upload.token->reconcile(280, identity) && budget.snapshot().totalHostClaimBytes == 464,
            "actual smaller upload backing returns only its own supplemental host claim");
    scratch.token.reset();
    upload.token.reset();
    grant.lease.reset();
    require(budget.snapshot().totalHostClaimBytes == 0 &&
                budget.snapshot().totalDeviceClaimBytes == 0 &&
                budget.snapshot().admittedPhases == 0,
            "trimmed phase and actual allocations retire with exact conservation");
    grant = budget.reserve({.fixedHostBytes = 100,
                            .requestedSourceBytes = 300,
                            .minimumSourceBytes = 300,
                            .deviceWorkingBytes = 1000});
    require(grant.lease && grant.lease->finishSourcePreparation(20),
            "a second preparation can release unused scratch");
    const auto watermark = grant.lease->revisionAfterOwnRelease();
    grant.lease.reset();
    require(budget.snapshot().admissionRevision == watermark,
            "own scratch release followed by teardown cannot create a self-retry loop");
}
// #472: the stated source need sums every reachable source reference as a float frame, while
// preparation decodes each distinct source once. After a phase of the same envelope finished
// preparation, later phases claim three times the largest retention plus a quarter (never more
// than the stated need). Retention beyond the claim is still charged, a phase's sources stay
// bounded by the live room at its admission, and a need that cannot fit is still refused.
void sourceNeedFollowsMeasuredRetention() {
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(10000, true);
    budget.setDeviceCapacity(1000, true);
    const GpuProcessingPhaseRequest stated{.fixedHostBytes = 100,
                                           .requestedSourceBytes = 5000,
                                           .minimumSourceBytes = 1000,
                                           .deviceWorkingBytes = 10};
    require(budget.hostNeedBytes(stated) == 3100,
            "an unmeasured envelope needs its stated sources");
    auto first = budget.reserve(stated);
    require(first.lease && first.lease->sourceClaimBytes() == 1000 &&
                budget.snapshot().totalHostClaimBytes == 3100,
            "the first phase of an envelope claims 3x its stated source need");
    require(first.lease->finishSourcePreparation(200) &&
                budget.snapshot().totalHostClaimBytes == 300,
            "finishing keeps only the retained sources");
    require(budget.hostNeedBytes(stated) == 100 + 3 * 250,
            "later phases need the measured retention plus a quarter");
    auto second = budget.reserve(stated);
    require(second.lease && second.lease->sourceClaimBytes() == 250 &&
                second.lease->hostBytes() == 850 && second.lease->sourceByteLimit() == 5000 &&
                budget.snapshot().totalHostClaimBytes == 300 + 850,
            "a measured phase claims from the retention; its ceiling stays the live room");
    require(second.lease->finishSourcePreparation(780) &&
                budget.snapshot().totalHostClaimBytes == 300 + 100 + 780,
            "retention beyond the claim is charged as it is");
    require(budget.hostNeedBytes(stated) == 100 + 3 * 975, "the largest retention is kept");
    first.lease.reset();
    second.lease.reset();
    auto light = budget.reserve(stated);
    require(light.lease && light.lease->finishSourcePreparation(10) &&
                budget.hostNeedBytes(stated) == 100 + 3 * 975,
            "a phase that retained less never lowers the need");
    light.lease.reset();
    auto other = stated;
    other.minimumSourceBytes = 2000;
    require(budget.hostNeedBytes(other) == 6100,
            "another envelope (another plan) starts from its own stated need");
    auto huge = stated;
    huge.minimumSourceBytes = huge.requestedSourceBytes = 5000;
    require(budget.reserve(huge).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "a need larger than the whole capacity is still refused");
    budget.setHostCapacity(4000, true);
    auto bounded = budget.reserve(stated);
    require(bounded.lease && bounded.lease->sourceByteLimit() == 4000 - 3025 + 975,
            "the sources a phase may retain are bounded by the live room at its admission");
    require(budget.reserve(stated).status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "a measured need that does not fit what is free now waits");
    require(!bounded.lease->finishSourcePreparation(1951),
            "retention beyond the admitted room is refused, never over-committed");
    bounded.lease.reset();
    const auto settled = budget.snapshot();
    require(settled.totalHostClaimBytes == 0 && settled.admittedPhases == 0 &&
                settled.finishedSourcePhases == 3 &&
                settled.finishedPhaseSourceNeedBytes == 1000 + 250 + 975 &&
                settled.finishedPhaseRetainedSourceBytes == 200 + 780 + 10 &&
                settled.peakPhaseSourceNeedBytes == 1000 &&
                settled.peakPhaseRetainedSourceBytes == 780,
            "claims return and per-phase need and retention are recorded exactly");
}
void rootIdentity() {
    GpuProcessingPhaseBudget first;
    GpuProcessingPhaseBudget second;
    for (auto* budget : {&first, &second}) {
        budget->setHostCapacity(70, true);
        budget->setDeviceCapacity(30, true);
    }
    auto a = first.reserve(request());
    auto b = second.reserve(request());
    require(a.lease && b.lease && a.lease->id() == b.lease->id(),
            "distinct roots can have the same numeric phase id");
    const auto identity = a.lease->rootIdentity();
    require(identity && identity != b.lease->rootIdentity(),
            "root identity must distinguish equal phase ids from different roots");
    const std::weak_ptr<GpuProcessingPhaseLease> weak = a.lease;
    a.lease.reset();
    require(weak.expired() && first.snapshot().admittedPhases == 0,
            "retaining neutral root identity must not retain a phase or its claims");
    require(!b.lease->isUnproven(), "new authority begins proven");
    b.lease->markUnproven();
    require(b.lease->isUnproven(), "authority exposes the sticky unproven latch");
    auto next = first.reserve(request());
    require(next.lease && next.lease->rootIdentity() == identity,
            "later phases preserve the exact neutral root identity");
}
// Owner-reported regression: GPU scene preparation asks for the whole device as its executor
// CEILING. Granting the whole room to the first phase refused every concurrent native phase (a
// Viewer request beside a RAM-preview fill) with "does not fit the current device-memory
// capacity" while most of the device was free. Concurrent ceiling requests must all fit while
// their stated minimum fits, and a minimum that does not fit is still refused.
void ceilingRequestsShareDevice() {
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    constexpr std::uint64_t mib = 1024ULL * 1024;
    GpuProcessingPhaseBudget budget(8);
    budget.setHostCapacity(16 * gib, true);
    budget.setDeviceCapacity(gib, true);
    GpuProcessingPhaseRequest ceiling{.fixedHostBytes = 64 * mib,
                                      .requestedSourceBytes = gib,
                                      .minimumSourceBytes = 0,
                                      .deviceWorkingBytes = 0,
                                      .requestedExecutorBytes = gib,
                                      .minimumExecutorBytes = 112 * mib,
                                      .sourceFree = true};
    auto viewer = budget.reserve(ceiling);
    auto fill = budget.reserve(ceiling);
    auto prefetch = budget.reserve(ceiling);
    require(viewer.lease && fill.lease && prefetch.lease,
            "concurrent whole-device ceiling requests all admit while their minimum fits");
    auto viewerStage = startDeviceStage(viewer.lease);
    auto fillStage = startDeviceStage(fill.lease);
    auto prefetchStage = startDeviceStage(prefetch.lease);
    require(viewer.lease->deviceBytes() >= ceiling.minimumExecutorBytes &&
                fill.lease->deviceBytes() >= ceiling.minimumExecutorBytes &&
                budget.snapshot().deviceReservedBytes <= gib,
            "every grant covers its minimum and the device is never over-reserved");
    auto heavy = ceiling;
    heavy.minimumExecutorBytes = gib;
    heavy.requestedExecutorBytes = gib;
    const auto waiting = budget.reserve(heavy);
    require(waiting.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                waiting.domain == GpuProcessingPhaseDomain::Device,
            "a minimum that fits the device but not the held claims waits for a release");
    heavy.minimumExecutorBytes = gib + 1;
    heavy.requestedExecutorBytes = gib + 1;
    require(budget.reserve(heavy).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "a minimum larger than the whole device is refused");
}

void contention() {
    constexpr std::size_t contenders = 32;
    GpuProcessingPhaseBudget budget(2);
    budget.setHostCapacity(140, true);
    budget.setDeviceCapacity(60, true);
    std::array<GpuProcessingPhaseGrant, contenders> grants;
    std::mutex startMutex;
    std::condition_variable_any start;
    bool ready = false;
    std::vector<std::jthread> workers;
    workers.reserve(contenders);
    for (std::size_t index = 0; index < contenders; ++index) {
        workers.emplace_back([&, index](const std::stop_token& stop) {
            {
                std::unique_lock lock(startMutex);
                if (!start.wait(lock, stop, [&] { return ready; })) {
                    return;
                }
            }
            grants[index] = budget.reserve(request());
        });
    }
    {
        const std::lock_guard lock(startMutex);
        ready = true;
    }
    start.notify_all();
    for (auto& worker : workers) {
        worker.join();
    }
    workers.clear();
    std::size_t accepted = 0;
    for (const auto& grant : grants) {
        if (grant.status == GpuProcessingPhaseAdmission::Accepted) {
            ++accepted;
        }
    }
    const auto snapshot = budget.snapshot();
    std::size_t waiting = 0;
    for (const auto& grant : grants) {
        if (grant.status == GpuProcessingPhaseAdmission::AwaitingCapacity) {
            ++waiting;
        }
    }
    require(accepted == 2 && waiting == contenders - 2 && snapshot.hostReservedBytes == 140 &&
                snapshot.deviceReservedBytes == 0 && snapshot.budgetRefusals == 0,
            "competing requests never acquire a partial or over-capacity pair; the rest wait "
            "(admitted phases claim device memory only in their device stage)");
    for (auto& grant : grants) {
        grant.lease.reset();
    }
    require(budget.snapshot().admittedPhases == 0 && budget.snapshot().hostReservedBytes == 0 &&
                budget.snapshot().deviceReservedBytes == 0,
            "all contending aliases retire exactly once");
}
} // namespace
// #472: preparation decodes and converts one source at a time, so its scratch is twice the largest
// source, not twice the whole source need. The claim then tracks what a many-layer frame holds.
void sourceScratchFollowsLargestSource() {
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(100000, true);
    budget.setDeviceCapacity(1000, true);
    // Twenty-five layers of 100 bytes each: the stated need sums them, the scratch is one layer's.
    const GpuProcessingPhaseRequest layers{.fixedHostBytes = 100,
                                           .requestedSourceBytes = 50000,
                                           .minimumSourceBytes = 2500,
                                           .largestSourceBytes = 100,
                                           .deviceWorkingBytes = 10};
    require(budget.hostNeedBytes(layers) == 100 + 2500 + 2 * 100,
            "the need is the sources plus one source's decode and conversion scratch");
    auto first = budget.reserve(layers);
    require(first.lease && first.lease->sourceScratchBytes() == 200 &&
                budget.snapshot().totalHostClaimBytes == 2800,
            "the phase claims its sources and one source's scratch, not 3x the whole need");
    require(first.lease->finishSourcePreparation(2500) &&
                budget.snapshot().totalHostClaimBytes == 2600,
            "finishing returns the scratch and keeps the retained sources");
    first.lease.reset();
    require(budget.hostNeedBytes(layers) == 100 + 2500 + 200,
            "a measured retention above the stated sources stays capped at them");
    auto unknown = layers;
    unknown.largestSourceBytes = 0;
    require(budget.hostNeedBytes(unknown) == 100 + 3 * 2500,
            "an unknown largest source keeps the whole-need scratch bound");
    auto oversized = layers;
    oversized.largestSourceBytes = 9000;
    require(budget.hostNeedBytes(oversized) == 100 + 2500 + 2 * 2500,
            "one source is never larger than all of them");
    budget.setHostCapacity(2700, true);
    require(budget.reserve(layers).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "a need above the whole capacity is still refused");
}
// #539: a phase's device claim follows the peak of what it holds at once, not the sum of what it
// allocated. Storage it publishes (a cache entry) leaves its live set, and the claim is held only
// while its scene executes.
void deviceClaimFollowsPeakLiveSet() {
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(10000, true);
    budget.setDeviceCapacity(2000, true);
    const GpuProcessingPhaseRequest scene{.fixedHostBytes = 10,
                                          .deviceWorkingBytes = 50,
                                          .requestedExecutorBytes = 900,
                                          .minimumExecutorBytes = 250,
                                          .sourceFree = true};
    auto grant = budget.reserve(scene);
    require(grant.lease && budget.snapshot().totalDeviceClaimBytes == 0,
            "a phase preparing or waiting for the device claims no device memory");
    // A chain of intermediates: each step's output is published to the cache as the next runs.
    constexpr NativeImageStorageIdentity local{.allocatorGeneration = 1, .memoryType = 0, .heap = 0};
    std::vector<NativeImageStorageToken> published;
    for (int step = 0; step < 8; ++step) {
        auto image = NativeImageStorageToken::reserve(grant.lease, 100, local);
        require(image.token.has_value(), "each intermediate fits the live room");
        require(image.token->promote(), "a finished intermediate is published to the cache");
        published.push_back(std::move(*image.token));
    }
    require(grant.lease->peakDeviceAllocationBytes() == 100,
            "published intermediates leave the phase: its peak is one live image, not the sum");
    require(budget.snapshot().totalDeviceClaimBytes == 800,
            "published storage stays charged to the root as retention");
    auto output = NativeImageStorageToken::reserve(grant.lease, 120, local);
    require(output.token.has_value(), "the output is allocated");
    require(budget.snapshot().totalDeviceClaimBytes == 800 + 300,
            "while executing the phase holds its whole need");
    grant.lease->endDeviceExecution();
    require(budget.snapshot().totalDeviceClaimBytes == 800 + 120 + 50 &&
                budget.snapshot().deviceReservedBytes == 50,
            "after execution the phase keeps its output and only its working allowance unallocated");
    published.clear();
    require(budget.snapshot().totalDeviceClaimBytes == 120 + 50,
            "evicted cache entries return to the root, not to the phase");
    output.token.reset();
    grant.lease.reset();
    require(budget.snapshot().totalDeviceClaimBytes == 0 &&
                budget.snapshot().recentPhaseDevicePeakBytes == 120,
            "the released phase records its own live peak for sizing frames in flight");
}
// #539: device memory another program holds can come back. A need above the measured free
// capacity but within the device's ceiling waits; only a need the whole device could never hold is
// refused.
void deviceRoomOthersHoldIsAWait() {
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(10000, true);
    budget.setDeviceCapacity(0, true);
    const GpuProcessingPhaseRequest frame{.fixedHostBytes = 100,
                                          .deviceWorkingBytes = 10,
                                          .requestedExecutorBytes = 400,
                                          .minimumExecutorBytes = 200,
                                          .native = true};
    require(budget.reserve(frame).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "a measured zero with no ceiling is exhaustion");
    budget.setDeviceCeiling(1000);
    auto zero = budget.reserve(frame);
    require(zero.status == GpuProcessingPhaseAdmission::AwaitingCapacity &&
                zero.domain == GpuProcessingPhaseDomain::Device,
            "a measured zero within the ceiling waits for the room to come back");
    budget.setDeviceCapacity(100, true);
    require(budget.reserve(frame).status == GpuProcessingPhaseAdmission::AwaitingCapacity,
            "a need above the free room but within the ceiling waits");
    auto huge = frame;
    huge.requestedExecutorBytes = 2000;
    huge.minimumExecutorBytes = 2000;
    require(budget.reserve(huge).status == GpuProcessingPhaseAdmission::BudgetRefused,
            "a need above the whole device is still refused");
    budget.setDeviceCapacity(800, true);
    auto grant = budget.reserve(frame);
    require(grant.lease && grant.lease->deviceCapacityAtAdmission() == 800,
            "room that returns admits the frame, which records the capacity it was admitted with");
}
int main() {
    try {
        readinessAndRollback();
        emptyAndZeroGrant();
        sizingAndLifetime();
        rootIdentity();
        contention();
        ceilingRequestsShareDevice();
        retryWatermarks();
        finishedPreparationReleasesDecodeScratch();
        sourceNeedFollowsMeasuredRetention();
        sourceScratchFollowsLargestSource();
        deviceClaimFollowsPeakLiveSet();
        deviceRoomOthersHoldIsAWait();
        std::cout << "paired phase readiness/sizing/lifetime/32-way contention PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
