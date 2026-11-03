// Focused, bounded tests for the capacity-aware resident preview admission policy.
//
// The pure cases run with no device: the unpartitioned configured ceiling (#425), the
// device-fits/device-pressure/unknown clamp, overflow saturation, and the 24x6000x4000 RGBA8
// retention arithmetic. The optional native
// owner case creates one real device on this thread, resolves its allocation budget, and proves the
// effective lease/cache plan stays aligned and never overcommits the resolved budget. It uses
// a compact arithmetic simulation (no 24 large GPU allocations, no simultaneous worker pressure).

#include <bloom/render/gpu_device.hpp>
#include <bloom/runtime/gpu_memory_budget.hpp>
#include <bloom/runtime/gpu_preview_resident_capacity.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <thread>

namespace {

using bloom::render::GpuAllocationBudgetSource;
using bloom::runtime::GpuResidentCapacity;
using bloom::runtime::GpuResidentCapacitySource;
using bloom::runtime::GpuResidentConfiguredBudgets;

constexpr std::uint64_t kMib = 1024ULL * 1024ULL;
constexpr std::uint64_t kGib = 1024ULL * kMib;

int failures = 0;

void expect(const bool condition, const std::string& label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

GpuResidentCapacity knownCapacity(const std::uint64_t bytes) {
    GpuResidentCapacity capacity;
    capacity.source = GpuResidentCapacitySource::MemoryBudget;
    capacity.availableBytes = bytes;
    capacity.deviceLocalBytes = bytes;
    return capacity;
}

// #425: usable VRAM is the only device ceiling. A configured ceiling applies to every ledger as
// it is; nothing is split into lease/scene/request/cache fractions.
void testConfiguredCeilingIsNotPartitioned() {
    const auto plan = bloom::runtime::gpuResidentCapacityPlanConfigured(2ULL * kGib);
    expect(plan.poolBytes == 2ULL * kGib, "the configured pool is the explicit ceiling");
    expect(plan.leaseBytes == 2ULL * kGib && plan.sceneCacheBytes == 2ULL * kGib &&
               plan.requestBytes == 2ULL * kGib && plan.cacheBytes == 2ULL * kGib,
           "every ledger is the whole ceiling, not a fixed fraction of it");
    expect(plan.cacheEntries >= 8 && plan.leaseEntries >= plan.cacheEntries,
           "the entry bounds are positive and aligned");
    expect(!plan.resolved, "a host-configured plan is not an owner-resolved plan");

    const auto tiny = bloom::runtime::gpuResidentCapacityPlanConfigured(5);
    expect(tiny.poolBytes == 5 && tiny.leaseBytes == 5 && tiny.sceneCacheBytes == 5 &&
               tiny.requestBytes == 5,
           "a tiny explicit ceiling is honored as it is, without a floor");

    const auto zero = bloom::runtime::gpuResidentCapacityPlanConfigured(0);
    expect(zero.poolBytes == 0 && zero.cacheBytes == 0 && zero.leaseBytes == 0 &&
               zero.sceneCacheBytes == 0 && zero.requestBytes == 0,
           "a zero ceiling yields an all-zero plan with no floor");
}

void testCapacityClamp() {
    GpuResidentConfiguredBudgets configured;
    configured.leaseBytes = 3ULL * kGib;
    configured.sceneCacheBytes = kGib;
    configured.requestBytes = kGib;

    // Plenty of room: the configured retention ceilings are preserved exactly, never raised; the
    // lease registry, which only accounts real allocations, takes the device pool.
    const auto roomy =
        bloom::runtime::gpuResidentCapacityPlanFor(configured, knownCapacity(16ULL * kGib));
    expect(roomy.cacheBytes == configured.leaseBytes &&
               roomy.sceneCacheBytes == configured.sceneCacheBytes &&
               roomy.requestBytes == configured.requestBytes,
           "a device with room preserves the configured ceilings exactly");
    expect(roomy.leaseBytes == 16ULL * kGib && roomy.leaseBytes >= roomy.cacheBytes,
           "the lease registry is bounded by the device, never below the cache");

    // Tight device: each ledger is clamped to the pool on its own, with no proportional share.
    const auto tight =
        bloom::runtime::gpuResidentCapacityPlanFor(configured, knownCapacity(2ULL * kGib));
    expect(tight.poolBytes == 2ULL * kGib, "a tight live budget caps the pool at available VRAM");
    expect(tight.cacheBytes == 2ULL * kGib && tight.leaseBytes == 2ULL * kGib &&
               tight.sceneCacheBytes == kGib && tight.requestBytes == kGib,
           "each ledger is min(configured, pool): no 1/2, 1/5, 4/5 or remainder split");

    // Unknown device: the small safe fallback, never a host-RAM assumption.
    const auto unknown =
        bloom::runtime::gpuResidentCapacityPlanFor(configured, GpuResidentCapacity{});
    expect(unknown.resolved, "the owner-resolved path is always marked resolved");
    expect(unknown.poolBytes == 256ULL * kMib, "an unknown device uses the safe fallback pool");
    expect(!unknown.capacity.isKnown(), "an unresolved device source is not known");

    // A KNOWN live budget reporting zero free bytes is not an unresolved device.
    const auto knownZero = bloom::runtime::gpuResidentCapacityPlanFor(configured, knownCapacity(0));
    expect(knownZero.capacity.isKnown(), "a known-zero budget is still a resolved device");
    expect(knownZero.poolBytes == 0 && knownZero.cacheBytes == 0,
           "a known-zero budget yields no resident subset, never the unknown fallback");

    // An explicit zero configured ceiling stays zero even on a large device.
    GpuResidentConfiguredBudgets none;
    const auto configuredZero =
        bloom::runtime::gpuResidentCapacityPlanFor(none, knownCapacity(16ULL * kGib));
    expect(configuredZero.resolved, "an explicit-zero owner plan is still resolved");
    expect(configuredZero.poolBytes == 0 && configuredZero.cacheBytes == 0 &&
               configuredZero.leaseBytes == 0,
           "an explicit zero ceiling is respected, never floored upward");
}

void testNominalHeapHasNoDivisor() {
    // Without VK_EXT_memory_budget the fallback is the DEVICE_LOCAL heap itself: the retired
    // nominal-heap divisor of 2 is gone, and the phase policy subtracts the VRAM headroom.
    GpuResidentCapacity nominal;
    nominal.source = GpuResidentCapacitySource::DeviceLocalEstimate;
    nominal.availableBytes = 16ULL * kGib;
    nominal.deviceLocalBytes = 16ULL * kGib;
    const GpuResidentConfiguredBudgets unlimited{
        .leaseBytes = std::numeric_limits<std::uint64_t>::max(),
        .sceneCacheBytes = std::numeric_limits<std::uint64_t>::max(),
        .requestBytes = 0};
    const auto plan = bloom::runtime::gpuResidentCapacityPlanFor(unlimited, nominal);
    expect(plan.poolBytes == 16ULL * kGib && plan.sceneCacheBytes == 16ULL * kGib &&
               plan.cacheBytes == 16ULL * kGib && plan.leaseBytes == 16ULL * kGib,
           "a nominal heap is the whole pool for every retention ledger");
    expect(bloom::runtime::gpuResidentForegroundRequestBudget(nominal, 3ULL * kGib) ==
               13ULL * kGib,
           "nominal foreground capacity is the heap minus actual image ownership");
}

void testStartupPressureDoesNotFreezeConstruction() {
    auto pressured = knownCapacity(0);
    pressured.deviceLocalBytes = 8 * kGib;
    auto recovered = pressured;
    recovered.availableBytes = 6 * kGib;
    const auto cold = bloom::runtime::gpuResidentConstructionCapacity(pressured);
    const auto warm = bloom::runtime::gpuResidentConstructionCapacity(recovered);
    expect(cold == warm && cold.source == GpuResidentCapacitySource::DeviceLocalEstimate &&
               cold.availableBytes == 8 * kGib,
           "transient zero free bytes do not become a lifetime construction ceiling");
    const GpuResidentConfiguredBudgets retention{.leaseBytes = kGib, .sceneCacheBytes = kGib / 2};
    const auto plan = bloom::runtime::gpuResidentCapacityPlanFor(retention, cold);
    expect(plan.leaseBytes != 0 && plan.sceneCacheBytes == kGib / 2 && plan.cacheBytes == kGib &&
               plan.cacheEntries != 0 && plan.requestBytes == 0 && plan.poolBytes == 8 * kGib,
           "rooted retention construction stays finite and usable under startup pressure");
    expect(bloom::runtime::gpuResidentForegroundRequestBudget(pressured, 0) == 0 &&
               bloom::runtime::gpuResidentForegroundRequestBudget(recovered, 0) == 6 * kGib,
           "live foreground admission still refuses zero and recovers with free space");
    expect(bloom::runtime::gpuResidentConstructionCapacity({}) == GpuResidentCapacity{},
           "unknown physical capacity retains the bounded unknown fallback");
}

void testRootedForegroundUsesOwnedCapacity() {
    // Large retention ceilings never reduce the foreground request's live device allowance.
    GpuResidentConfiguredBudgets retention;
    retention.leaseBytes = 32ULL * kGib;
    retention.sceneCacheBytes = 12ULL * kGib;
    retention.requestBytes = 8ULL * kGib;
    const auto available = knownCapacity(8ULL * kGib);
    const auto plan = bloom::runtime::gpuResidentCapacityPlanFor(retention, available);
    expect(plan.requestBytes == 8ULL * kGib,
           "a large cache ceiling no longer starves the active request");
    expect(bloom::runtime::gpuResidentForegroundRequestBudget(available, 0) == 8ULL * kGib,
           "empty retention cannot reduce a rooted foreground request below live free VRAM");
    expect(bloom::runtime::gpuResidentForegroundRequestBudget(knownCapacity(5ULL * kGib),
                                                              3ULL * kGib) == 5ULL * kGib,
           "live MemoryBudget already subtracts owned VMA images exactly once");
    expect(bloom::runtime::gpuResidentForegroundRequestBudget(knownCapacity(0), 0) == 0,
           "known-zero free VRAM refuses foreground admission");
    // #425: a scene cache that fills the device gives way to work, so its bytes are room for the
    // request rather than a reason to refuse it; the credit never exceeds the device.
    expect(bloom::runtime::gpuResidentForegroundRequestBudget(knownCapacity(1ULL * kGib), 0,
                                                              6ULL * kGib) == 1ULL * kGib,
           "the credit never exceeds the device-local size");
    auto filled = knownCapacity(1ULL * kGib);
    filled.deviceLocalBytes = 24ULL * kGib;
    expect(bloom::runtime::gpuResidentForegroundRequestBudget(filled, 0, 6ULL * kGib) ==
               7ULL * kGib,
           "a live budget credits the reclaimable scene cache back to the request");

    GpuResidentCapacity nominal;
    nominal.source = GpuResidentCapacitySource::DeviceLocalEstimate;
    nominal.availableBytes = 16ULL * kGib;
    expect(bloom::runtime::gpuResidentForegroundRequestBudget(nominal, 17ULL * kGib) == 0,
           "nominal ownership beyond the heap saturates at zero");
    expect(bloom::runtime::gpuResidentForegroundRequestBudget(nominal, 10ULL * kGib, 4ULL * kGib) ==
               10ULL * kGib,
           "a nominal heap charges owned images except the reclaimable cache");
    expect(bloom::runtime::gpuResidentForegroundRequestBudget(GpuResidentCapacity{},
                                                              32ULL * kMib) == 224ULL * kMib,
           "unresolved capacity retains its bounded fallback after owned-image charges");
}

void testOverflowSaturation() {
    GpuResidentConfiguredBudgets saturated;
    saturated.leaseBytes = std::numeric_limits<std::uint64_t>::max();
    saturated.sceneCacheBytes = std::numeric_limits<std::uint64_t>::max();
    saturated.requestBytes = std::numeric_limits<std::uint64_t>::max();
    const auto plan =
        bloom::runtime::gpuResidentCapacityPlanFor(saturated, knownCapacity(16ULL * kGib));
    expect(plan.poolBytes == 16ULL * kGib && plan.leaseBytes == 16ULL * kGib &&
               plan.sceneCacheBytes == 16ULL * kGib && plan.requestBytes == 16ULL * kGib,
           "unlimited ceilings resolve to the device pool without overflow");
    expect(plan.cacheBytes <= plan.leaseBytes, "unlimited ceilings keep cache <= lease");
}

// Deterministic default for the scheduler's aggregate GPU request-owned ceiling. Pure: no ledger,
// no device, no resource dependence.
void testSchedulerRequestCapacityDefault() {
    using bloom::runtime::defaultGpuRequestOwnedByteCapacityFor;
    expect(defaultGpuRequestOwnedByteCapacityFor(8ULL * kGib, 4ULL * kGib) == 4ULL * kGib,
           "a positive operation-cache split is the default aggregate ceiling");
    expect(defaultGpuRequestOwnedByteCapacityFor(8ULL * kGib, 0) == 8ULL * kGib,
           "a zero operation-cache split falls back to the usable host budget");
    expect(defaultGpuRequestOwnedByteCapacityFor(8ULL * kGib, 16ULL * kGib) == 8ULL * kGib,
           "the default is clamped to the usable host budget");
    expect(defaultGpuRequestOwnedByteCapacityFor(0, 0) > 0,
           "an unknown/low-memory host still yields a valid positive default");
    expect(defaultGpuRequestOwnedByteCapacityFor(0, 4ULL * kGib) > 0,
           "a zero usable budget still yields a valid positive default");
}

void testAllocationBudgetMapping() {
    bloom::render::GpuAllocationBudget actual;
    actual.source = GpuAllocationBudgetSource::MemoryBudget;
    actual.available_bytes = 5ULL * kGib;
    actual.device_local_bytes = 16ULL * kGib;
    const auto mapped = bloom::runtime::gpuResidentCapacityFromAllocationBudget(actual);
    expect(mapped.source == GpuResidentCapacitySource::MemoryBudget &&
               mapped.availableBytes == 5ULL * kGib && mapped.deviceLocalBytes == 16ULL * kGib,
           "a live memory budget maps to the MemoryBudget source");

    bloom::render::GpuAllocationBudget estimated;
    estimated.source = GpuAllocationBudgetSource::DeviceLocalEstimate;
    estimated.available_bytes = 16ULL * kGib;
    const auto estimate = bloom::runtime::gpuResidentCapacityFromAllocationBudget(estimated);
    expect(estimate.source == GpuResidentCapacitySource::DeviceLocalEstimate,
           "a nominal heap estimate is labelled as an estimate");

    const auto missing = bloom::runtime::gpuResidentCapacityFromAllocationBudget(
        bloom::render::GpuAllocationBudget{});
    expect(missing.source == GpuResidentCapacitySource::Unknown && !missing.isKnown(),
           "an unavailable budget maps to Unknown with zero availability");
}

void testSixKRetention() {
    const std::uint64_t frame6k = 6000ULL * 4000ULL * 4ULL;
    const std::uint64_t retention24 = 24ULL * frame6k;
    GpuResidentConfiguredBudgets configured;
    configured.leaseBytes = 12ULL * kGib;
    configured.sceneCacheBytes = 12ULL * kGib;
    configured.requestBytes = 12ULL * kGib;

    const auto plan =
        bloom::runtime::gpuResidentCapacityPlanFor(configured, knownCapacity(16ULL * kGib));
    expect(plan.cacheBytes >= retention24,
           "24 6K RGBA8 frames are retained when the device budget allows it");
    // Compact simulation: the entry count the cache would need for 24 real frames is admitted.
    const std::uint64_t framesFitting = plan.cacheBytes / frame6k;
    expect(framesFitting >= 24, "the compact 6K frame simulation retains at least 24 frames");

    // A device that cannot host even the configured route still refuses honestly (LRU/typed).
    const auto tiny = bloom::runtime::gpuResidentCapacityPlanFor(configured, knownCapacity(kGib));
    expect(tiny.cacheBytes < retention24, "a small device does not admit the 24-frame retention");
}

struct TestOptions final {
    std::filesystem::path loaderPath;
    bool requireDevice = false;
};

TestOptions parseOptions(const int argc, char** argv) {
    TestOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            options.loaderPath = argv[++index];
        } else if (argument == "--require-device") {
            options.requireDevice = true;
        }
    }
    return options;
}

// Native owner bootstrap: resolve the real device budget and prove the effective lease/cache plan
// is aligned and never overcommits it. Compact arithmetic only; no large allocations.
void testNativeOwnerBootstrap(const TestOptions& options) {
    bloom::render::GpuDeviceCreationOptions createOptions;
    createOptions.loader_path = options.loaderPath;
    auto created = bloom::render::GpuDevice::create(createOptions);
    if (!created) {
        if (options.requireDevice) {
            expect(false, "a device was required but the probe returned Unavailable");
            return;
        }
        std::cout << "SKIP: no compatible Vulkan device available: " << created.diagnostic.message
                  << '\n';
        return;
    }
    auto& device = *created.device;
    expect(device.state() == bloom::render::GpuDeviceState::Ready,
           "the native owner device is Ready");

    const auto budget = device.availableAllocationBudget();
    expect(budget.source != GpuAllocationBudgetSource::Unavailable,
           "the owner resolved a real allocation budget");
    expect(budget.available_bytes <= budget.device_local_bytes,
           "the owner available budget never exceeds DEVICE_LOCAL capacity");
    std::cout << "GPU allocation budget: source=" << static_cast<int>(budget.source)
              << " available_bytes=" << budget.available_bytes
              << " device_local_bytes=" << budget.device_local_bytes << '\n';

    const auto capacity = bloom::runtime::gpuResidentCapacityFromAllocationBudget(budget);
    GpuResidentConfiguredBudgets configured;
    configured.leaseBytes = 6ULL * kGib;
    configured.sceneCacheBytes = 2ULL * kGib;
    configured.requestBytes = 2ULL * kGib;
    const auto plan = bloom::runtime::gpuResidentCapacityPlanFor(configured, capacity);
    expect(plan.leaseBytes >= plan.cacheBytes,
           "the native effective lease ledger covers the cache sublimit");
    expect(plan.poolBytes <= capacity.availableBytes && plan.leaseBytes <= plan.poolBytes &&
               plan.sceneCacheBytes <= plan.poolBytes && plan.requestBytes <= plan.poolBytes,
           "every native ledger is at most the resolved device pool");

    // A foreign-thread query is rejected without touching the driver.
    GpuResidentCapacity foreignCapacity;
    std::thread worker([&device, &foreignCapacity]() {
        foreignCapacity = bloom::runtime::gpuResidentCapacityFromAllocationBudget(
            device.availableAllocationBudget());
    });
    worker.join();
    expect(foreignCapacity.source == GpuResidentCapacitySource::Unknown &&
               foreignCapacity.availableBytes == 0,
           "a foreign-thread budget query fails closed to Unknown");
}

} // namespace

int main(int argc, char** argv) {
    const auto halfSequence = bloom::runtime::gpuResidentCapacityPlanConfigured(1ULL << 30);
    if (halfSequence.cacheEntries < 100 || halfSequence.cacheEntries >= halfSequence.leaseEntries ||
        halfSequence.leaseEntries > bloom::runtime::kResidentLeaseEntryCeiling)
        return 1;

    const TestOptions options = parseOptions(argc, argv);
    testConfiguredCeilingIsNotPartitioned();
    testCapacityClamp();
    testNominalHeapHasNoDivisor();
    testStartupPressureDoesNotFreezeConstruction();
    testRootedForegroundUsesOwnedCapacity();
    testOverflowSaturation();
    testSchedulerRequestCapacityDefault();
    testAllocationBudgetMapping();
    testSixKRetention();
    testNativeOwnerBootstrap(options);

    if (failures != 0) {
        std::cerr << "FAIL: " << failures << " resident capacity expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: resident preview capacity policy\n";
    return 0;
}
