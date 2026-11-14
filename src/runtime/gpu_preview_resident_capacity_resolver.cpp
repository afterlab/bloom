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
    // #682: retention ceilings are planned against the usable device memory, never the VRAM
    // headroom the preference keeps free of Bloom's work and caches.
    if (core->options.processingPhasePolicy && capacity.isKnown()) {
        const auto headroom = core->options.processingPhasePolicy->headroom().vramBytes;
        capacity.availableBytes =
            capacity.availableBytes > headroom ? capacity.availableBytes - headroom : 0;
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

// #539: idle means small. Once the owner has had no preview work for the idle trim delay, parked
// native storage and the reusable scene cache go back to the device, leaving the resident working
// set (what is on screen and the finished frames the UI holds). Owner thread only.
static void trimIdleDeviceRetention(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                    const std::chrono::steady_clock::time_point now) noexcept {
    const bool busy = !core->stages.empty() || !core->residentNativeReady.empty() ||
                      core->residentNativeInFlight || core->residentAhead != nullptr ||
                      !core->nativeReady.empty() || core->nativeInFlight;
    if (busy) {
        core->residentBusyAt = now;
        core->idleTrimmed = false;
        return;
    }
    if (core->idleTrimmed || now - core->residentBusyAt < core->options.idleTrimDelay)
        return;
    core->idleTrimmed = true;
    // A tap matters only while frames are being shown: it goes with the rest of the idle
    // retention, first, so the scene cache's entries are the only owners of what they cache.
    releaseScopeTapsIdle(core);
    std::uint64_t released = 0;
    if (core->residentSceneCache != nullptr)
        released += core->residentSceneCache->reclaim(core->residentSceneCache->retainedBytes());
    // What the cache let go of is parked first; all parked storage is freed.
    const auto parked = core->device->nativeStorageReuseStats().parkedDeviceBytes;
    const auto freed = parked != 0 ? core->device->releaseReusableNativeStorage(parked, true) : 0;
    released = std::max(released, freed);
    core->counterIdleTrims.fetch_add(1, std::memory_order_relaxed);
    core->counterIdleTrimmedBytes.fetch_add(released, std::memory_order_relaxed);
    // The next device report sees the returned room.
    core->foregroundCapacitySampledAt = {};
}

std::uint64_t giveWayToDeviceRoom(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                  const std::uint64_t bytes) noexcept {
    if (core == nullptr || core->device == nullptr || !core->device->isOwnerThread() ||
        !core->options.processingPhasePolicy)
        return 0;
    // The measure's own allocation never makes anything give device room back for it (#724): it
    // is refused and the measure yields.
    if (core->scope.stepping)
        return 0;
    // The scope taps and the measure's memory are given back before anything else (#724): a
    // frame's allocation was refused for device room. What they free counts toward the room.
    const auto scopeGiven = giveScopeMemoryBack(core);
    // Parked storage holds nothing anyone reads, so it goes first; then the scene cache's
    // unpinned entries, least recently used first (the running scene's own inputs stay pinned).
    // What the cache lets go of is parked on release, and is freed too.
    const auto wanted = deviceRoomGiveWay(
        *core->options.processingPhasePolicy, bytes,
        core->device->nativeStorageReuseStats().parkedDeviceBytes +
            (core->residentSceneCache != nullptr ? core->residentSceneCache->retainedBytes() : 0));
    std::uint64_t released = scopeGiven.freedBytes;
    if (released < wanted)
        released += core->device->releaseReusableNativeStorage(wanted - released, true);
    if (released < wanted && core->residentSceneCache != nullptr) {
        const auto parkedBefore = core->device->nativeStorageReuseStats().parkedDeviceBytes;
        released += core->residentSceneCache->reclaim(wanted - released);
        const auto parkedAfter = core->device->nativeStorageReuseStats().parkedDeviceBytes;
        if (parkedAfter > parkedBefore)
            static_cast<void>(
                core->device->releaseReusableNativeStorage(parkedAfter - parkedBefore, true));
    }
    if (released != 0) {
        core->counterDeviceRoomReclaims.fetch_add(1, std::memory_order_relaxed);
        core->counterDeviceRoomReclaimedBytes.fetch_add(released, std::memory_order_relaxed);
        // Report the returned room now, so the retried allocation is admitted against it.
        core->foregroundCapacitySampledAt = {};
        refreshResidentForegroundCapacity(core);
    }
    return released;
}

void refreshResidentForegroundCapacity(
    const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core == nullptr || core->device == nullptr || !core->device->isOwnerThread() ||
        (!usesLiveForegroundDefault(*core) && !core->options.processingPhasePolicy)) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    trimIdleDeviceRetention(core, now);
    // Parked host-visible storage (#477) goes back as soon as work waits for host memory.
    if (core->hostStorageReclaim->pending())
        static_cast<void>(
            core->device->releaseReusableNativeStorage(core->hostStorageReclaim->take(), false));
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
        const auto& policy = *core->options.processingPhasePolicy;
        const auto headroom = policy.headroom().vramBytes;
        const auto wanted = deviceReclaimBytes(*core->deviceReclaim, allocation, headroom);
        // #682: the caches also keep room for one more phase of the current work, so a RAM fill
        // never waits for them to shrink. Only reusable retention gives this back.
        const auto reserve = deviceWorkReserveShortfall(policy);
        // #724: the scope taps and the measure's memory are the first thing given back for a
        // frame short of device memory, before parked storage, the scene cache, the OCIO programs
        // or a request to the UI to evict finished frames.
        std::uint64_t scopeFreed = 0;
        if (wanted != 0 || reserve != 0) {
            scopeFreed = giveScopeMemoryBack(core).freedBytes;
            if (scopeFreed != 0)
                allocation = sample();
        }
        if (reserve > wanted && reserve > scopeFreed) {
            auto released = core->device->releaseReusableNativeStorage(reserve - scopeFreed, true);
            const auto parkedBefore = core->device->nativeStorageReuseStats().parkedDeviceBytes;
            if (released + scopeFreed < reserve && core->residentSceneCache != nullptr)
                released += core->residentSceneCache->reclaim(reserve - scopeFreed - released);
            const auto parkedAfter = core->device->nativeStorageReuseStats().parkedDeviceBytes;
            if (parkedAfter > parkedBefore)
                static_cast<void>(
                    core->device->releaseReusableNativeStorage(parkedAfter - parkedBefore, true));
            if (released != 0) {
                core->counterDeviceRoomReclaims.fetch_add(1, std::memory_order_relaxed);
                core->counterDeviceRoomReclaimedBytes.fetch_add(released,
                                                                std::memory_order_relaxed);
                allocation = sample();
            }
        }
        if (wanted != 0 && wanted > scopeFreed) {
            const auto stillWanted = wanted - scopeFreed;
            // Parked native storage (#477) holds nothing anyone reads, so it goes first.
            auto released = core->device->releaseReusableNativeStorage(stillWanted, true);
            const auto parkedBefore = core->device->nativeStorageReuseStats().parkedDeviceBytes;
            if (released < stillWanted && core->residentSceneCache != nullptr)
                released += core->residentSceneCache->reclaim(stillWanted - released);
            // Then the retained OCIO programs, which have no fixed byte ceiling of their own
            // (#474): the scene executor's effect programs and the display arm's, and the scope
            // measure's color programs.
            if (released < stillWanted && core->residentExecutor != nullptr)
                released += core->residentExecutor->reclaimOcioPrograms(stillWanted - released);
            if (released < stillWanted && core->residentGeneralDisplay != nullptr)
                released += core->residentGeneralDisplay->executor().reclaim(stillWanted - released);
            if (released < stillWanted)
                released += reclaimScopePrograms(core, stillWanted - released);
            // Storage those releases parked is freed too, so what they report is given back.
            const auto parkedAfter = core->device->nativeStorageReuseStats().parkedDeviceBytes;
            if (parkedAfter > parkedBefore)
                static_cast<void>(
                    core->device->releaseReusableNativeStorage(parkedAfter - parkedBefore, true));
            if (released < stillWanted) {
                std::lock_guard lock(core->stateMutex);
                ++core->publishedResidentFrameEvictionSerial;
                core->publishedResidentFrameEvictionBytes = stillWanted - released;
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
        // Parked native storage (#477) is allocated too, outside the image registry.
        const auto parked = core->device->nativeStorageReuseStats().parkedDeviceBytes;
        ownedImageBytes = parked > std::numeric_limits<std::uint64_t>::max() - ownedImageBytes
                              ? std::numeric_limits<std::uint64_t>::max()
                              : ownedImageBytes + parked;
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
