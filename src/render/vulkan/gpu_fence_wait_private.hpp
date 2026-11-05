#ifndef BLOOM_RENDER_VULKAN_GPU_FENCE_WAIT_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_FENCE_WAIT_PRIVATE_HPP

// Private to src/render/vulkan. #386: the owner thread's event-driven wait on a native job's own
// fence. It only blocks; the job's ordinary poll() still observes the fence and decides what the
// signal proves, so no state changes here.

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <thread>

namespace bloom::render::fence_wait_detail {

// True when `fence` signalled (or the device was lost) within `timeoutNanoseconds`. Owner thread
// only; a null fence or another thread never waits.
template <typename Control>
[[nodiscard]] bool waitForNativeFence(const Control& control, const VkFence fence,
                                      const std::uint64_t timeoutNanoseconds) noexcept {
    if (fence == VK_NULL_HANDLE || control.owner != std::this_thread::get_id())
        return false;
    const VkResult waited = control.device.getDispatcher()->vkWaitForFences(
        static_cast<VkDevice>(*control.device), 1, &fence, VK_TRUE, timeoutNanoseconds);
    return waited == VK_SUCCESS || waited == VK_ERROR_DEVICE_LOST;
}

} // namespace bloom::render::fence_wait_detail

#endif // BLOOM_RENDER_VULKAN_GPU_FENCE_WAIT_PRIVATE_HPP
