#pragma once

// Native storage reuse (#477), private to src/render/vulkan. See NativeStorageReuse in
// gpu_device_private.hpp for the contract.

#include "gpu_device_private.hpp"

#include <cstdint>
#include <optional>

namespace bloom::render::vulkan_detail {

[[nodiscard]] NativeStorageReuse::Shape imageReuseShape(const VkImageCreateInfo& info) noexcept;
[[nodiscard]] NativeStorageReuse::Shape
bufferReuseShape(const VkBufferCreateInfo& info,
                 const VmaAllocationCreateInfo& allocation) noexcept;
// True when reuse is enabled and nothing (an extension chain, a private fault or memory-type
// control) makes this allocation differ from an ordinary one.
[[nodiscard]] bool storageReuseEligible(DeviceAllocatorState& state, const void* next) noexcept;
// The most recently parked entry of `shape`, removed from the parked set.
[[nodiscard]] std::optional<NativeStorageReuse::Entry>
takeParkedStorage(DeviceAllocatorState& state, const NativeStorageReuse::Shape& shape) noexcept;
// Returns a taken entry the current phase could not reserve to the parked set.
void restoreParkedStorage(DeviceAllocatorState& state,
                          const NativeStorageReuse::Entry& entry) noexcept;
// Records an object the allocator issued, so its release parks it.
void noteIssuedStorage(DeviceAllocatorState& state, const NativeStorageReuse::Entry& entry,
                       bool reused) noexcept;
// Release of a covered native object: parked when it was issued while reuse is enabled and
// `parkable`, otherwise destroyed with its allocation.
void destroyNativeImage(DeviceAllocatorState& state, VkImage image, VmaAllocation allocation,
                        bool parkable = true) noexcept;
void destroyNativeBuffer(DeviceAllocatorState& state, VkBuffer buffer, VmaAllocation allocation,
                         bool parkable = true) noexcept;
// Frees parked entries of the selected memory, least recently parked first, until `bytes` are
// released. Returns the bytes released.
std::uint64_t releaseParkedStorage(DeviceAllocatorState& state, std::uint64_t bytes,
                                   bool deviceLocal) noexcept;
// Frees everything parked and forgets issued objects (teardown, or reuse disabled).
void closeStorageReuse(DeviceAllocatorState& state, bool keepEnabled = false) noexcept;

} // namespace bloom::render::vulkan_detail
