// #477: transient native images and buffers are parked on release and taken back by the next
// allocation of the same shape under a fresh token for the current phase; parked storage is freed
// on request, least recently parked first, and a second phase can be retained beside a parked one.

#include "gpu_image_private.hpp"
#include "gpu_native_buffer_allocation_private.hpp"
#include "gpu_native_storage_reuse_private.hpp"

#include <bloom/core/telemetry_counters.hpp>
#include <bloom/render/gpu_processing_phase_budget.hpp>

#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {
using namespace bloom::render;

void require(const bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::shared_ptr<GpuProcessingPhaseLease> phase(GpuProcessingPhaseBudget& budget) {
    constexpr std::uint64_t kBytes = 64ULL << 20;
    auto grant = budget.reserve({.fixedHostBytes = 1,
                                 .requestedExecutorBytes = kBytes,
                                 .minimumExecutorBytes = kBytes,
                                 .sourceFree = true});
    if (!grant.lease)
        throw std::runtime_error("actual test phase admission failed");
    return std::move(grant.lease);
}

std::uint64_t deviceAllocations() {
    return bloom::core::telemetry::gpuDeviceMemoryTotals().allocations;
}

struct Buffer final {
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> state;
    NativeBufferStorageToken token;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    ~Buffer() { release(); }
    void release() {
        if (buffer != VK_NULL_HANDLE)
            vulkan_detail::destroyNativeBuffer(*state, buffer, allocation);
        buffer = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        token = {};
    }
};

bool allocateStaging(const std::shared_ptr<vulkan_detail::DeviceAllocatorState>& state,
                     Buffer& out) {
    out.state = state;
    const VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  nullptr,
                                  0,
                                  1ULL << 20,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE,
                                  0,
                                  nullptr};
    VmaAllocationCreateInfo preferences{};
    preferences.usage = VMA_MEMORY_USAGE_AUTO;
    preferences.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    return vulkan_detail::allocateCoveredBuffer(*state, info, preferences, 1ULL << 30, out.buffer,
                                                out.allocation, out.info, out.token,
                                                vulkan_detail::NativeBufferPurpose::Processing) ==
           vulkan_detail::NativeBufferAllocationOutcome::Allocated;
}

void phaseSlots(GpuDevice& device, GpuProcessingPhaseBudget& budget) {
    auto first = phase(budget);
    auto second = phase(budget);
    require(!device.parkProcessingPhase(), "nothing to park without a retained phase");
    require(device.retainProcessingPhase(first), "the first phase is retained");
    require(!device.retainProcessingPhase(second), "one phase is retained at a time");
    require(device.parkProcessingPhase() && device.processingPhaseOccupied() &&
                device.retainedProcessingPhase() == nullptr,
            "a parked phase keeps the generation occupied and frees the retained slot");
    require(device.retainProcessingPhase(second) && !device.parkProcessingPhase(),
            "the next phase is retained beside the parked one, and only one is parked");
    require(device.selectProcessingPhase(first->id()) && device.retainedProcessingPhase() == first,
            "selecting the parked phase makes it the retained one");
    require(device.selectProcessingPhase(second->id()) &&
                device.retainedProcessingPhase() == second,
            "selecting swaps the phases back");
    require(!device.selectProcessingPhase(first->id() + second->id() + 1),
            "an unknown phase cannot be selected");
    device.retireProcessingPhase(second->id(), true);
    require(device.processingPhaseOccupied() && device.retainedProcessingPhase() == nullptr,
            "retiring the retained phase leaves the parked one");
    require(device.selectProcessingPhase(first->id()) && device.retainedProcessingPhase() == first,
            "the parked phase is selected into the free slot");
    device.retireProcessingPhase(first->id(), true);
    require(!device.processingPhaseOccupied(), "both phases retire");
}

void reuse(GpuDevice& device, GpuProcessingPhaseBudget& budget) {
    const auto state = GpuRendererAccess::state(device);
    require(state != nullptr, "real allocator state exists");
    const auto baseline = device.allocatorMemorySnapshot();

    // Off by default: a released image is freed.
    {
        auto admitted = phase(budget);
        require(device.retainProcessingPhase(admitted), "phase retained");
        auto image = std::make_unique<GpuImageImpl>();
        image->state = state;
        require(createResidentImage(*state, 256, 128, *image), "image allocates");
        image.reset();
        require(device.nativeStorageReuseStats().parkedEntries == 0 &&
                    device.allocatorMemorySnapshot().allocationCount == baseline.allocationCount,
                "without reuse a released image is freed");
        device.retireProcessingPhase(admitted->id(), true);
    }

    device.setNativeStorageReuse(true);
    VkImage firstHandle = VK_NULL_HANDLE;
    std::uint64_t imageBytes = 0;
    {
        auto admitted = phase(budget);
        require(device.retainProcessingPhase(admitted), "first frame phase retained");
        auto image = std::make_unique<GpuImageImpl>();
        image->state = state;
        require(createResidentImage(*state, 256, 128, *image), "first frame image allocates");
        firstHandle = image->image;
        imageBytes = image->storageToken.bytes();
        require(budget.snapshot().ownedNativeImages == 1, "the image is charged to its phase");
        image.reset();
        const auto parked = device.nativeStorageReuseStats();
        require(parked.parkedEntries == 1 && parked.created == 1 &&
                    parked.parkedDeviceBytes + parked.parkedHostBytes == imageBytes,
                "the released image is parked with its allocation");
        require(budget.snapshot().ownedNativeImages == 0 &&
                    budget.snapshot().ownedNativeImageBytes == 0,
                "a parked image is charged to no phase");
        device.retireProcessingPhase(admitted->id(), true);
    }
    {
        auto admitted = phase(budget);
        require(device.retainProcessingPhase(admitted), "next frame phase retained");
        const auto allocationsBefore = deviceAllocations();
        auto image = std::make_unique<GpuImageImpl>();
        image->state = state;
        require(createResidentImage(*state, 256, 128, *image), "next frame image is created");
        require(image->image == firstHandle && image->storageToken.bytes() == imageBytes &&
                    deviceAllocations() == allocationsBefore,
                "the same shape takes the parked image back without a device allocation");
        require(budget.snapshot().ownedNativeImages == 1 &&
                    budget.snapshot().ownedNativeImageBytes == imageBytes,
                "the reused image is charged to the current phase");
        const auto stats = device.nativeStorageReuseStats();
        require(stats.reused == 1 && stats.parkedEntries == 0, "one reuse, nothing parked");
        auto other = std::make_unique<GpuImageImpl>();
        other->state = state;
        require(createResidentImage(*state, 128, 128, *other) && other->image != firstHandle,
                "another shape is a new image");
        image.reset();
        other.reset();
        require(device.nativeStorageReuseStats().parkedEntries == 2, "both are parked");

        Buffer staging;
        require(allocateStaging(state, staging) && staging.info.pMappedData != nullptr,
                "mapped staging allocates");
        const auto bufferHandle = staging.buffer;
        staging.release();
        Buffer again;
        require(allocateStaging(state, again) && again.buffer == bufferHandle &&
                    again.info.pMappedData != nullptr,
                "mapped staging is taken back still mapped");
        again.release();
        device.retireProcessingPhase(admitted->id(), true);
    }
    const auto parked = device.nativeStorageReuseStats();
    require(parked.parkedEntries == 3, "two images and one buffer are parked");
    const auto released = device.releaseReusableNativeStorage(1, true) +
                          device.releaseReusableNativeStorage(1, false);
    require(released != 0 && device.nativeStorageReuseStats().parkedEntries < 3 &&
                device.nativeStorageReuseStats().evictedBytes == released,
            "a release frees parked storage least recently parked first");
    const auto all =
        device.releaseReusableNativeStorage(std::numeric_limits<std::uint64_t>::max(), true) +
        device.releaseReusableNativeStorage(std::numeric_limits<std::uint64_t>::max(), false);
    require(released + all == parked.parkedDeviceBytes + parked.parkedHostBytes &&
                device.nativeStorageReuseStats().parkedEntries == 0,
            "every parked byte is freed");
    require(device.allocatorMemorySnapshot().allocationCount == baseline.allocationCount,
            "freed parked storage returns the allocator to its baseline");
    {
        auto admitted = phase(budget);
        require(device.retainProcessingPhase(admitted), "phase retained");
        auto image = std::make_unique<GpuImageImpl>();
        image->state = state;
        require(createResidentImage(*state, 256, 128, *image), "image allocates");
        image.reset();
        require(device.nativeStorageReuseStats().parkedEntries == 1, "parked");
        device.setNativeStorageReuse(false);
        require(device.nativeStorageReuseStats().parkedEntries == 0 &&
                    !device.nativeStorageReuseStats().enabled &&
                    device.allocatorMemorySnapshot().allocationCount == baseline.allocationCount,
                "disabling reuse frees everything parked");
        device.retireProcessingPhase(admitted->id(), true);
    }
    require(budget.snapshot().totalDeviceClaimBytes == 0 &&
                budget.snapshot().totalHostClaimBytes == 0,
            "no claim outlives its phase");
}
} // namespace

int main(int argc, char** argv) {
    try {
        bool requireDevice = false;
        for (int index = 1; index < argc; ++index) {
            if (std::string_view(argv[index]) != "--require-device")
                return 2;
            requireDevice = true;
        }
        auto created = GpuDevice::create();
        if (!created) {
            std::cout << "Unsupported: " << created.diagnostic.message << '\n';
            return requireDevice ? 1 : 77;
        }
        GpuProcessingPhaseBudget budget;
        budget.setHostCapacity(1ULL << 32, true);
        budget.setDeviceCapacity(1ULL << 32, true);
        require(created.device->bindProcessingRoot(budget.rootIdentity()),
                "test device binds its allocation root");
        phaseSlots(*created.device, budget);
        reuse(*created.device, budget);
        std::cout << "PASS: parked native storage is reused, charged per phase and freed on "
                     "request; a second phase is retained beside a parked one\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
