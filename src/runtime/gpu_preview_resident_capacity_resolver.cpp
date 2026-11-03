// Owner-thread bridge between the render device allocation budget and the resident admission
// policy. Kept in its own translation unit so the service startup and resident-route files stay
// focused. Nothing here allocates, blocks, or touches a device off its owner thread.

#include "gpu_preview_display_service_private.hpp"

#include <bloom/runtime/gpu_preview_resident_capacity.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace bloom::runtime::detail {

namespace {

[[nodiscard]] bool usesLiveForegroundDefault(const PreviewDisplayServiceCore& core) noexcept {
    return !core.options.previewByteAllowance && core.options.processingPhasePolicy &&
           core.options.processingPhasePolicy->budgetView();
}

} // namespace

void resolveResidentCapacity(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core == nullptr || core->gpuStageFunction == nullptr || core->device == nullptr) {
        return;
    }
    if (!core->device->isOwnerThread()) {
        return;
    }

    GpuResidentCapacity capacity;
    try {
        capacity =
            gpuResidentCapacityFromAllocationBudget(core->device->availableAllocationBudget());
    } catch (...) {
        capacity = GpuResidentCapacity{};
    }

    const GpuResidentCapacity observedCapacity = capacity;
    GpuResidentConfiguredBudgets configured;
    configured.leaseBytes = core->options.residentLeaseBudgets.maxBytes;
    configured.sceneCacheBytes = core->options.residentSceneCacheBudgets.maxRetainedBytes;
    // A live default must not freeze the first (or absent) host sample into the owner ceiling.
    // Resolve the finite physical retention pool once; each request applies live admission.
    const bool liveDefault = usesLiveForegroundDefault(*core);
    configured.requestBytes =
        liveDefault ? 0
                    : core->options.previewByteAllowance.value_or(gpuPreviewRequestByteAllowance());
    if (liveDefault) {
        // These are lifetime retention ceilings, not allocated bytes. A transient low/zero free
        // sample must not permanently disable the lease registry, scene cache, or Viewer cache.
        // The nominal construction pool is finite; every real native allocation is separately
        // checked against the refreshed shared-root capacity and the device allocator.
        capacity = gpuResidentConstructionCapacity(capacity);
    }
    const GpuResidentCapacityPlan plan = gpuResidentCapacityPlanFor(configured, capacity);

    // Retention sub-budgets never exceed their configured values. Rooted foreground work borrows
    // unclaimed device space independently; explicit/unrooted requests keep the additive share.
    core->effectiveResidentLeaseBudgets.maxBytes = plan.leaseBytes;
    const std::uint64_t configuredEntries =
        static_cast<std::uint64_t>(core->options.residentLeaseBudgets.maxEntries);
    core->effectiveResidentLeaseBudgets.maxEntries =
        static_cast<std::size_t>(std::min(configuredEntries, plan.leaseEntries));
    core->effectiveResidentSceneCacheBudgets.maxRetainedBytes = plan.sceneCacheBytes;
    core->effectivePreviewByteAllowance.store(
        liveDefault ? gpuResidentForegroundRequestBudget(observedCapacity, 0) : plan.requestBytes,
        std::memory_order_relaxed);

    {
        std::lock_guard lock(core->stateMutex);
        core->publishedResidentCapacityPlan = plan;
    }
    core->notify();
}

void refreshResidentForegroundCapacity(
    const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core == nullptr || core->device == nullptr || !core->device->isOwnerThread() ||
        (!usesLiveForegroundDefault(*core) && !core->options.processingPhasePolicy)) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    // Work waiting for device memory is answered at once, not at the next sampling slot.
    if (!core->deviceReclaim->pending() &&
        core->foregroundCapacitySampledAt != std::chrono::steady_clock::time_point{} &&
        now - core->foregroundCapacitySampledAt < std::chrono::milliseconds(50)) {
        return;
    }
    core->foregroundCapacitySampledAt = now;
    const auto sample = [&core] {
        try {
            return core->device->availableAllocationBudget();
        } catch (...) {
            return render::GpuAllocationBudget{};
        }
    };
    render::GpuAllocationBudget allocation = sample();
    if (core->options.processingPhasePolicy) {
        // Evict on demand (#425): what waiting work is short of, or what the driver's free memory
        // is below the VRAM headroom. The reusable scene cache gives it back first; what it cannot
        // cover goes to the UI as a finished-frame eviction, so finished frames go last.
        const auto wanted = deviceReclaimBytes(
            *core->deviceReclaim, allocation,
            core->options.processingPhasePolicy->headroom().vramBytes);
        if (wanted != 0) {
            const auto released =
                core->residentSceneCache != nullptr ? core->residentSceneCache->reclaim(wanted) : 0;
            if (released < wanted) {
                std::lock_guard lock(core->stateMutex);
                ++core->publishedResidentFrameEvictionSerial;
                core->publishedResidentFrameEvictionBytes = wanted - released;
            }
            if (released != 0)
                allocation = sample();
        }
    }
    const GpuResidentCapacity capacity = gpuResidentCapacityFromAllocationBudget(allocation);
    std::uint64_t ownedImageBytes = 0;
    if (capacity.source != GpuResidentCapacitySource::MemoryBudget) {
        // A nominal heap estimate is static. The generation-scoped image registry includes scene
        // cache, presented lease, external pin and retiring images, even after a cache evicts its
        // own reference. The live MemoryBudget branch already subtracts these VMA allocations.
        const auto images = core->device->imageReclamationStats();
        ownedImageBytes = images.liveAllocationBytes;
        ownedImageBytes = images.retiringAllocationBytes >
                                  std::numeric_limits<std::uint64_t>::max() - ownedImageBytes
                              ? std::numeric_limits<std::uint64_t>::max()
                              : ownedImageBytes + images.retiringAllocationBytes;
    }
    if (core->options.processingPhasePolicy) {
        if (allocation.source == render::GpuAllocationBudgetSource::DeviceLocalEstimate) {
            // The phase policy already subtracts root-owned native image tokens as claims.
            // Account separately for only the images outside that root, which nominal heap size
            // cannot see. A multi-device root has no per-owner split, so retain the conservative
            // full image charge there.
            const auto phase = core->options.processingPhasePolicy->snapshot();
            std::uint64_t untrackedImages = ownedImageBytes;
            if (phase.deviceReports == 1) {
                const auto& claims = phase.reservations;
                const auto localRootImages =
                    claims.ownedNativeImageBytes -
                    std::min(claims.ownedNativeImageBytes, claims.ownedNonlocalNativeHostBytes);
                untrackedImages -= std::min(untrackedImages, localRootImages);
            }
            allocation.available_bytes -= std::min(untrackedImages, allocation.available_bytes);
        }
        // MemoryBudget is already free after VMA allocations. Do not add held phase claims here:
        // some are unallocated promises, and treating them as free could over-admit new work.
        static_cast<void>(core->options.processingPhasePolicy->reportDevice(
            core->device->ownershipEpoch(), allocation));
    }
    if (core->gpuStageFunction != nullptr && usesLiveForegroundDefault(*core)) {
        core->effectivePreviewByteAllowance.store(
            gpuResidentForegroundRequestBudget(
                capacity, ownedImageBytes,
                core->residentSceneCache != nullptr ? core->residentSceneCache->retainedBytes()
                                                    : 0),
            std::memory_order_relaxed);
    }
}

} // namespace bloom::runtime::detail
