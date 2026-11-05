#ifndef BLOOM_RENDER_VULKAN_GPU_COMMAND_REUSE_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_COMMAND_REUSE_PRIVATE_HPP

// Private to src/render/vulkan. #457: the single-submission native jobs (packed display transfer,
// path coverage, OCIO LUT upload) used to create a command pool, a command buffer and a fence for
// every job, which on the preview route meant every frame and every shape. They now take a set
// from the device generation and give it back when the job's own fence proved its submission
// retired. A set whose submission is unproven never comes back: it follows the job into its
// quarantine exactly as before, and the device creates a new one for the next job.

#include "gpu_device_private.hpp"

#include <cstddef>

namespace bloom::render::vulkan_detail {

// Parked sets per device generation; a return beyond it destroys the set.
inline constexpr std::size_t kMaxRecycledCommandSets = 8;

// Owner thread: a parked set with its pool reset (the buffer back in the initial state) and its
// fence unsignalled, or a new one (a TRANSIENT pool on the compute queue family, one primary
// buffer, an unsignalled fence). Any other thread always gets a new set. False when a new set could
// not be created; `out` is then empty.
[[nodiscard]] bool acquireCommands(DeviceAllocatorState& state, ReusableCommands& out) noexcept;

// Only after the set's last submission is proven retired (its fence signalled) or when it was
// never submitted. Parks the set on the owner thread; destroys it on any other thread or when the
// park is full. Leaves `commands` empty.
void recycleCommands(DeviceAllocatorState& state, ReusableCommands& commands) noexcept;

// Owner thread, before the device is cleared.
void releaseRecycledCommands(DeviceAllocatorState& state) noexcept;

} // namespace bloom::render::vulkan_detail

#endif // BLOOM_RENDER_VULKAN_GPU_COMMAND_REUSE_PRIVATE_HPP
