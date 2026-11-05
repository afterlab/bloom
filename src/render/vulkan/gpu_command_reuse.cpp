#include "gpu_command_reuse_private.hpp"

#include <thread>
#include <utility>

namespace bloom::render::vulkan_detail {

namespace {

[[nodiscard]] bool createCommands(DeviceAllocatorState& state, ReusableCommands& out) noexcept {
    try {
        const VkDevice device = static_cast<VkDevice>(*state.device);
        const auto* dispatcher = state.device.getDispatcher();
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = state.computeQueueFamily;
        VkCommandPool rawPool = VK_NULL_HANDLE;
        if (dispatcher->vkCreateCommandPool(device, &poolInfo, nullptr, &rawPool) != VK_SUCCESS)
            return false;
        ReusableCommands created;
        created.pool = vk::raii::CommandPool(state.device, rawPool);
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = rawPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        VkCommandBuffer rawBuffer = VK_NULL_HANDLE;
        if (dispatcher->vkAllocateCommandBuffers(device, &allocateInfo, &rawBuffer) != VK_SUCCESS)
            return false;
        created.buffer = vk::raii::CommandBuffer(state.device, rawBuffer, rawPool);
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence rawFence = VK_NULL_HANDLE;
        if (dispatcher->vkCreateFence(device, &fenceInfo, nullptr, &rawFence) != VK_SUCCESS)
            return false;
        created.fence = vk::raii::Fence(state.device, rawFence);
        out = std::move(created);
        return true;
    } catch (...) {
        return false;
    }
}

void destroy(ReusableCommands& commands) noexcept {
    // The buffer is freed into its pool before the pool is destroyed.
    commands.buffer = vk::raii::CommandBuffer{nullptr};
    commands.pool = vk::raii::CommandPool{nullptr};
    commands.fence = vk::raii::Fence{nullptr};
}

} // namespace

bool acquireCommands(DeviceAllocatorState& state, ReusableCommands& out) noexcept {
    destroy(out);
    if (state.owner == std::this_thread::get_id()) {
        while (!state.recycledCommands.empty()) {
            ReusableCommands parked = std::move(state.recycledCommands.back());
            state.recycledCommands.pop_back();
            const VkDevice device = static_cast<VkDevice>(*state.device);
            const auto* dispatcher = state.device.getDispatcher();
            const VkFence fence = static_cast<VkFence>(*parked.fence);
            if (dispatcher->vkResetCommandPool(device, static_cast<VkCommandPool>(*parked.pool),
                                               0) != VK_SUCCESS ||
                dispatcher->vkResetFences(device, 1, &fence) != VK_SUCCESS) {
                destroy(parked);
                continue;
            }
            out = std::move(parked);
            ++state.commandSetsReused;
            return true;
        }
    }
    if (!createCommands(state, out)) {
        destroy(out);
        return false;
    }
    ++state.commandSetsCreated;
    return true;
}

void recycleCommands(DeviceAllocatorState& state, ReusableCommands& commands) noexcept {
    const bool complete = *commands.pool != vk::CommandPool{} &&
                          *commands.buffer != vk::CommandBuffer{} && *commands.fence != vk::Fence{};
    if (complete && state.owner == std::this_thread::get_id() &&
        state.recycledCommands.size() < kMaxRecycledCommandSets) {
        try {
            state.recycledCommands.push_back(std::move(commands));
        } catch (...) { // NOLINT(bugprone-empty-catch): the set is destroyed below instead
        }
    }
    destroy(commands);
}

void releaseRecycledCommands(DeviceAllocatorState& state) noexcept {
    for (auto& commands : state.recycledCommands)
        destroy(commands);
    state.recycledCommands.clear();
}

} // namespace bloom::render::vulkan_detail
