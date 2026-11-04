#include "gpu_native_storage_reuse_private.hpp"

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace bloom::render::vulkan_detail {
namespace {

void freeEntry(DeviceAllocatorState& state, const NativeStorageReuse::Entry& entry) noexcept {
    if (entry.shape.image)
        vmaDestroyImage(state.allocator, handleFromBits<VkImage>(entry.handle), entry.allocation);
    else
        vmaDestroyBuffer(state.allocator, handleFromBits<VkBuffer>(entry.handle), entry.allocation);
}

void unpark(NativeStorageReuse& reuse, const NativeStorageReuse::Entry& entry) noexcept {
    (entry.deviceLocal ? reuse.parkedDeviceBytes : reuse.parkedHostBytes) -= std::min(
        entry.allocationBytes, entry.deviceLocal ? reuse.parkedDeviceBytes : reuse.parkedHostBytes);
}

void park(NativeStorageReuse& reuse, const NativeStorageReuse::Entry& entry) {
    reuse.parked.push_back(entry);
    (entry.deviceLocal ? reuse.parkedDeviceBytes : reuse.parkedHostBytes) += entry.allocationBytes;
}

// Parks an issued object; false when it must be destroyed instead.
bool parkIssued(DeviceAllocatorState& state,
                std::unordered_map<std::uint64_t, NativeStorageReuse::Entry>& issued,
                const std::uint64_t handle, const bool parkable) noexcept {
    auto& reuse = state.storageReuse;
    const std::lock_guard lock(reuse.mutex);
    const auto found = issued.find(handle);
    if (found == issued.end())
        return false;
    const auto entry = found->second;
    issued.erase(found);
    if (!reuse.enabled || !parkable)
        return false;
    try {
        park(reuse, entry);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

NativeStorageReuse::Shape imageReuseShape(const VkImageCreateInfo& info) noexcept {
    NativeStorageReuse::Shape shape;
    shape.image = true;
    shape.flags = info.flags;
    shape.kind = static_cast<std::uint32_t>(info.imageType);
    shape.format = static_cast<std::uint32_t>(info.format);
    shape.width = info.extent.width;
    shape.height = info.extent.height;
    shape.depth = info.extent.depth;
    shape.levels = info.mipLevels;
    shape.layers = info.arrayLayers;
    shape.samples = static_cast<std::uint32_t>(info.samples);
    shape.tiling = static_cast<std::uint32_t>(info.tiling);
    shape.usage = info.usage;
    // A new image starts undefined; only an undefined initial layout matches a reused one.
    shape.memoryUsage = static_cast<std::uint32_t>(info.initialLayout) +
                        (static_cast<std::uint32_t>(info.sharingMode) << 16U);
    return shape;
}

NativeStorageReuse::Shape bufferReuseShape(const VkBufferCreateInfo& info,
                                           const VmaAllocationCreateInfo& allocation) noexcept {
    NativeStorageReuse::Shape shape;
    shape.flags = info.flags;
    shape.kind = static_cast<std::uint32_t>(info.sharingMode);
    shape.usage = info.usage;
    shape.bytes = info.size;
    shape.allocationFlags = allocation.flags;
    shape.requiredFlags = allocation.requiredFlags;
    shape.preferredFlags = allocation.preferredFlags;
    shape.memoryUsage = static_cast<std::uint32_t>(allocation.usage);
    return shape;
}

bool storageReuseEligible(DeviceAllocatorState& state, const void* const next) noexcept {
    if (next != nullptr || state.nativeImageMemoryTypeMaskForTests != 0 ||
        state.nativeBufferMemoryTypeMaskForTests != 0 ||
        state.nativeImageAllocationFault != NativeImageAllocationFault::None ||
        state.nativeBufferAllocationFault != NativeBufferAllocationFault::None)
        return false;
    const std::lock_guard lock(state.storageReuse.mutex);
    return state.storageReuse.enabled;
}

std::optional<NativeStorageReuse::Entry>
takeParkedStorage(DeviceAllocatorState& state, const NativeStorageReuse::Shape& shape) noexcept {
    auto& reuse = state.storageReuse;
    const std::lock_guard lock(reuse.mutex);
    if (!reuse.enabled)
        return std::nullopt;
    for (auto entry = reuse.parked.rbegin(); entry != reuse.parked.rend(); ++entry) {
        if (entry->shape != shape)
            continue;
        auto taken = *entry;
        reuse.parked.erase(std::next(entry).base());
        unpark(reuse, taken);
        return taken;
    }
    return std::nullopt;
}

void restoreParkedStorage(DeviceAllocatorState& state,
                          const NativeStorageReuse::Entry& entry) noexcept {
    auto& reuse = state.storageReuse;
    {
        const std::lock_guard lock(reuse.mutex);
        try {
            if (reuse.enabled) {
                park(reuse, entry);
                return;
            }
        } catch (...) {
        }
    }
    freeEntry(state, entry);
}

void noteIssuedStorage(DeviceAllocatorState& state, const NativeStorageReuse::Entry& entry,
                       const bool reused) noexcept {
    auto& reuse = state.storageReuse;
    const std::lock_guard lock(reuse.mutex);
    if (!reuse.enabled)
        return;
    try {
        (entry.shape.image ? reuse.issuedImages : reuse.issuedBuffers)[entry.handle] = entry;
        ++(reused ? reuse.reused : reuse.created);
    } catch (...) {
        // Untracked: the release destroys it as before.
    }
}

void destroyNativeImage(DeviceAllocatorState& state, const VkImage image,
                        const VmaAllocation allocation, const bool parkable) noexcept {
    if (image != VK_NULL_HANDLE &&
        parkIssued(state, state.storageReuse.issuedImages, handleBits(image), parkable))
        return;
    vmaDestroyImage(state.allocator, image, allocation);
}

void destroyNativeBuffer(DeviceAllocatorState& state, const VkBuffer buffer,
                         const VmaAllocation allocation, const bool parkable) noexcept {
    if (buffer != VK_NULL_HANDLE &&
        parkIssued(state, state.storageReuse.issuedBuffers, handleBits(buffer), parkable))
        return;
    vmaDestroyBuffer(state.allocator, buffer, allocation);
}

std::uint64_t releaseParkedStorage(DeviceAllocatorState& state, const std::uint64_t bytes,
                                   const bool deviceLocal) noexcept {
    auto& reuse = state.storageReuse;
    std::vector<NativeStorageReuse::Entry> released;
    std::uint64_t total = 0;
    {
        const std::lock_guard lock(reuse.mutex);
        auto entry = reuse.parked.begin();
        while (entry != reuse.parked.end() && total < bytes) {
            if (entry->deviceLocal != deviceLocal) {
                ++entry;
                continue;
            }
            try {
                released.push_back(*entry);
            } catch (...) {
                break;
            }
            total += entry->allocationBytes;
            unpark(reuse, *entry);
            entry = reuse.parked.erase(entry);
        }
        reuse.evictedBytes += total;
    }
    for (const auto& entry : released)
        freeEntry(state, entry);
    return total;
}

void closeStorageReuse(DeviceAllocatorState& state, const bool keepEnabled) noexcept {
    auto& reuse = state.storageReuse;
    std::vector<NativeStorageReuse::Entry> parked;
    {
        const std::lock_guard lock(reuse.mutex);
        reuse.enabled = keepEnabled;
        parked.swap(reuse.parked);
        reuse.parkedDeviceBytes = 0;
        reuse.parkedHostBytes = 0;
        if (!keepEnabled) {
            // Issued objects are destroyed by their owners as before.
            reuse.issuedImages.clear();
            reuse.issuedBuffers.clear();
        }
    }
    for (const auto& entry : parked)
        freeEntry(state, entry);
}

} // namespace bloom::render::vulkan_detail
