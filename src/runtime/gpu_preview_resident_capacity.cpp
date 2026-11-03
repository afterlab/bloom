#include <bloom/runtime/gpu_preview_resident_capacity.hpp>

#include <algorithm>

namespace bloom::runtime {

namespace {

void finalizeEntries(GpuResidentCapacityPlan& plan) noexcept {
    if (plan.cacheBytes == 0U) {
        return;
    }
    // Pixel size is not a metadata bound. Byte/resource admission already charges actual native
    // allocations; reserve bounded lease slots for in-flight/presented/tombstoned products.
    const auto entryHeadroom = kResidentLeaseEntryCeiling / 8U;
    plan.cacheEntries =
        std::min(kResidentCacheEntryCeiling, kResidentLeaseEntryCeiling - entryHeadroom);
    plan.leaseEntries = kResidentLeaseEntryCeiling;
}

// The device pool: a live budget or the whole DEVICE_LOCAL heap, never a fraction of either.
std::uint64_t devicePoolBytes(const GpuResidentCapacity& capacity) noexcept {
    return capacity.isKnown() ? capacity.availableBytes : kUnknownResidentPoolBytes;
}

} // namespace

GpuResidentCapacity
gpuResidentCapacityFromAllocationBudget(const render::GpuAllocationBudget& allocation) noexcept {
    GpuResidentCapacity capacity;
    capacity.deviceLocalBytes = allocation.device_local_bytes;
    switch (allocation.source) {
    case render::GpuAllocationBudgetSource::MemoryBudget:
        capacity.source = GpuResidentCapacitySource::MemoryBudget;
        capacity.availableBytes = allocation.available_bytes;
        break;
    case render::GpuAllocationBudgetSource::DeviceLocalEstimate:
        capacity.source = GpuResidentCapacitySource::DeviceLocalEstimate;
        capacity.availableBytes = allocation.available_bytes;
        break;
    case render::GpuAllocationBudgetSource::Unavailable:
        capacity.source = GpuResidentCapacitySource::Unknown;
        capacity.availableBytes = 0;
        break;
    }
    return capacity;
}

GpuResidentCapacity gpuResidentConstructionCapacity(const GpuResidentCapacity& observed) noexcept {
    if (observed.deviceLocalBytes == 0) {
        return {};
    }
    return {.source = GpuResidentCapacitySource::DeviceLocalEstimate,
            .availableBytes = observed.deviceLocalBytes,
            .deviceLocalBytes = observed.deviceLocalBytes};
}

GpuResidentCapacityPlan
gpuResidentCapacityPlanConfigured(const std::uint64_t configuredCeilingBytes) noexcept {
    GpuResidentCapacityPlan plan;
    plan.poolBytes = configuredCeilingBytes;
    plan.leaseBytes = configuredCeilingBytes;
    plan.sceneCacheBytes = configuredCeilingBytes;
    plan.requestBytes = configuredCeilingBytes;
    plan.cacheBytes = configuredCeilingBytes;
    finalizeEntries(plan);
    return plan;
}

std::uint64_t gpuResidentRequestBudget(const std::uint64_t hostByteAllowance,
                                       const std::uint64_t gpuRequestCeiling) noexcept {
    if (hostByteAllowance == 0U || gpuRequestCeiling == 0U) {
        return 0U;
    }
    return std::min(hostByteAllowance, gpuRequestCeiling);
}

GpuResidentCapacityPlan gpuResidentCapacityPlanFor(const GpuResidentConfiguredBudgets& configured,
                                                   const GpuResidentCapacity& capacity) noexcept {
    GpuResidentCapacityPlan plan;
    // This is the owner-resolved path: even an Unknown device produces a valid conservative
    // fallback plan that a consumer may apply, so mark it resolved unconditionally.
    plan.resolved = true;
    plan.capacity = capacity;
    const std::uint64_t largest =
        std::max({configured.leaseBytes, configured.sceneCacheBytes, configured.requestBytes});
    if (largest == 0U) {
        return plan;
    }
    // One device pool, no partition: each ledger is its own configured ceiling clamped to the
    // pool. What the ledgers hold together is bounded by real allocation admission against usable
    // VRAM, not by splitting the pool in advance.
    const std::uint64_t pool = devicePoolBytes(capacity);
    plan.poolBytes = pool;
    // The configured lease ceiling bounds what the resident frame cache retains. The registry
    // itself only accounts leases that are real device allocations, so it takes the whole pool:
    // a lease in flight or on screen beside a full cache is never refused while VRAM is free.
    plan.cacheBytes = std::min(configured.leaseBytes, pool);
    plan.leaseBytes = configured.leaseBytes == 0U ? 0U : pool;
    plan.sceneCacheBytes = std::min(configured.sceneCacheBytes, pool);
    plan.requestBytes = std::min(configured.requestBytes, pool);
    finalizeEntries(plan);
    return plan;
}

std::uint64_t gpuResidentForegroundRequestBudget(const GpuResidentCapacity& capacity,
                                                 const std::uint64_t ownedImageBytes,
                                                 const std::uint64_t reclaimableCacheBytes) noexcept {
    if (capacity.source == GpuResidentCapacitySource::MemoryBudget) {
        // The live budget already subtracts VMA usage, including retained and in-flight images;
        // the reclaimable cache is credited back, never beyond the device.
        const auto credited = capacity.availableBytes + reclaimableCacheBytes;
        const auto bounded = credited < capacity.availableBytes ? capacity.availableBytes : credited;
        return capacity.deviceLocalBytes != 0 ? std::min(bounded, capacity.deviceLocalBytes)
                                              : bounded;
    }
    // A nominal heap size is not free space: subtract the generation's exact image ownership (a
    // pinned or retiring image remains charged; a reclaimable cache image does not). The shared
    // phase policy applies the headroom.
    const std::uint64_t planningLimit = devicePoolBytes(capacity);
    const std::uint64_t held = ownedImageBytes - std::min(ownedImageBytes, reclaimableCacheBytes);
    return held < planningLimit ? planningLimit - held : 0;
}

} // namespace bloom::runtime
