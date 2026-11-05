#pragma once

#include "gpu_device_private.hpp"
#include "gpu_native_storage_reuse_private.hpp"

namespace bloom::render::vulkan_detail {

// The caller's impl owns the admitted token from the first successful backing allocation. On
// every failure backing is destroyed before its debit folds back. No native owner enters the token.
[[nodiscard]] inline bool allocateCoveredImage(DeviceAllocatorState& state,
                                               const VkImageCreateInfo& imageInfo, VkImage& image,
                                               VmaAllocation& allocation,
                                               NativeImageStorageToken& token,
                                               const bool presentation = false,
                                               const bool restoreFailedPrefix = false) noexcept {
    const auto release = [&] {
        if (restoreFailedPrefix && !presentation)
            static_cast<void>(
                token.releaseFailedAttemptAfterBackingDestroyed(state.processingPhase));
        else
            token = {};
    };
    state.imageStorageRefusal.reset();
    if (presentation)
        state.presentationStorageRefusal.reset();
    VmaAllocationCreateInfo preferred{};
    preferred.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (presentation ? !state.processingRoot : !state.processingPhase) {
        // Explicit legacy/untracked resources retain the established VMA AUTO route.
        return vmaCreateImage(state.allocator, &imageInfo, &preferred, &image, &allocation,
                              nullptr) == VK_SUCCESS;
    }
    // #477: a parked image of the identical shape is taken back under a fresh token for this
    // phase; a refused reservation leaves it parked and takes the ordinary path, which reports
    // the same refusal.
    const bool reuse = !presentation && storageReuseEligible(state, imageInfo.pNext);
    const auto shape = reuse ? imageReuseShape(imageInfo) : NativeStorageReuse::Shape{};
    if (reuse) {
        if (const auto parked = takeParkedStorage(state, shape)) {
            const NativeImageStorageIdentity identity{state.presentationEpoch, parked->memoryType,
                                                      parked->heap, parked->deviceLocal};
            auto grant = NativeImageStorageToken::reserve(state.processingPhase,
                                                          parked->requirementBytes, identity);
            if (grant.token && grant.token->reconcile(parked->allocationBytes, identity)) {
                token = std::move(*grant.token);
                image = handleFromBits<VkImage>(parked->handle);
                allocation = parked->allocation;
                noteIssuedStorage(state, *parked, true);
                return true;
            }
            grant.token.reset();
            restoreParkedStorage(state, *parked);
        }
    }
    const auto* dispatch = state.device.getDispatcher();
    const VkDevice device = static_cast<VkDevice>(*state.device);
    if (dispatch->vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS)
        return false;
    VkMemoryDedicatedRequirements dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
    VkMemoryRequirements2 requirements{};
    requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    requirements.pNext = &dedicated;
    VkImageMemoryRequirementsInfo2 query{};
    query.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
    query.image = image;
    dispatch->vkGetImageMemoryRequirements2(device, &query, &requirements);
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(state.allocator, &memory);
    auto remaining = requirements.memoryRequirements.memoryTypeBits;
    if (state.nativeImageMemoryTypeMaskForTests != 0)
        remaining &= state.nativeImageMemoryTypeMaskForTests;
    bool allocationAttempted = false;
    while (remaining != 0) {
        preferred.memoryTypeBits = remaining;
        std::uint32_t selected = 0;
        if (vmaFindMemoryTypeIndexForImageInfo(state.allocator, &imageInfo, &preferred,
                                               &selected) != VK_SUCCESS ||
            selected >= memory->memoryTypeCount || selected >= 32)
            break;
        const auto bit = std::uint32_t{1} << selected;
        if ((remaining & bit) == 0)
            break;
        remaining &= ~bit;
        const auto heap = memory->memoryTypes[selected].heapIndex;
        const NativeImageStorageIdentity identity{
            state.presentationEpoch, selected, heap,
            (memory->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0};
        auto grant = reserveGivingWayToRetention(
            state, identity.deviceLocal, requirements.memoryRequirements.size, [&] {
                return presentation
                           ? NativeImageStorageToken::reservePresentation(
                                 state.processingRoot, requirements.memoryRequirements.size,
                                 identity)
                           : NativeImageStorageToken::reserve(state.processingPhase,
                                                              requirements.memoryRequirements.size,
                                                              identity);
            });
        if (!grant.token) {
            if (grant.status == GpuProcessingPhaseAdmission::AwaitingCapacity ||
                grant.status == GpuProcessingPhaseAdmission::BudgetRefused)
                state.imageStorageRefusal = {grant.status, grant.domain,
                                             presentation ? 0 : state.processingPhase->id(),
                                             grant.revisionAfterOwnRelease};
            if (presentation && (grant.status == GpuProcessingPhaseAdmission::BudgetRefused ||
                                 grant.status == GpuProcessingPhaseAdmission::AwaitingCapacity))
                state.presentationStorageRefusal = {grant.status, grant.domain,
                                                    grant.revisionAfterOwnRelease, true};
            continue;
        } // A denied domain must not hide a covered compatible fallback.
        token = std::move(*grant.token);
        VmaAllocationCreateInfo pinned = preferred;
        pinned.usage = VMA_MEMORY_USAGE_UNKNOWN;
        pinned.memoryTypeBits = bit;
        if (dedicated.requiresDedicatedAllocation)
            pinned.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (state.nativeImageAllocationFault == NativeImageAllocationFault::Allocation) {
            state.nativeImageAllocationFault = NativeImageAllocationFault::None;
            dispatch->vkDestroyImage(device, image, nullptr);
            image = VK_NULL_HANDLE;
            release();
            state.imageStorageRefusal.reset();
            if (presentation)
                state.presentationStorageRefusal.reset();
            return false; // Injected branch failure, not an observed allocator OOM.
        }
        VmaAllocationInfo actual{};
        allocationAttempted = true;
        if (state.nativeImageBackingAttempts != UINT64_MAX)
            ++state.nativeImageBackingAttempts;
        if (vmaAllocateMemoryForImage(state.allocator, image, &pinned, &allocation, &actual) !=
            VK_SUCCESS) {
            release();
            continue;
        }
        // The same actual image remains unbound until its selected allocation is reconciled.
        const bool reconcileFault =
            state.nativeImageAllocationFault == NativeImageAllocationFault::Reconcile;
        const bool bindFault = state.nativeImageAllocationFault == NativeImageAllocationFault::Bind;
        if (reconcileFault || bindFault)
            state.nativeImageAllocationFault = NativeImageAllocationFault::None;
        const bool exactType = actual.memoryType == selected;
        if (!exactType || !token.reconcile(reconcileFault ? UINT64_MAX : actual.size, identity) ||
            bindFault || vmaBindImageMemory(state.allocator, allocation, image) != VK_SUCCESS) {
            dispatch->vkDestroyImage(device, image, nullptr);
            image = VK_NULL_HANDLE;
            vmaFreeMemory(state.allocator, allocation);
            if (state.nativeImageBackingsDestroyed != UINT64_MAX)
                ++state.nativeImageBackingsDestroyed;
            allocation = VK_NULL_HANDLE;
            release();
            state.imageStorageRefusal.reset();
            if (presentation)
                state.presentationStorageRefusal.reset();
            return false;
        }
        state.imageStorageRefusal.reset();
        if (presentation)
            state.presentationStorageRefusal.reset();
        if (presentation)
            static_cast<void>(token.promote());
        if (reuse)
            noteIssuedStorage(state,
                              {.shape = shape,
                               .handle = handleBits(image),
                               .allocation = allocation,
                               .requirementBytes = requirements.memoryRequirements.size,
                               .allocationBytes = actual.size,
                               .memoryType = selected,
                               .heap = heap,
                               .deviceLocal = identity.deviceLocal},
                              false);
        return true;
    }
    if (allocationAttempted)
        state.imageStorageRefusal.reset(); // Real allocator failure remains terminal, not capacity.
    if (presentation && allocationAttempted)
        state.presentationStorageRefusal.reset();
    dispatch->vkDestroyImage(device, image, nullptr);
    image = VK_NULL_HANDLE;
    return false;
}
} // namespace bloom::render::vulkan_detail
