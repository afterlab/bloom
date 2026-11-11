#include "gpu_image_private.hpp"
#include "gpu_image_reclamation_private.hpp"
#include "gpu_resident_display_private.hpp"

#include <bloom/render/gpu_processing_phase_budget.hpp>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace bloom::render;
bool createResidentImageForTest(vulkan_detail::DeviceAllocatorState& owner,
                                const std::uint32_t width, const std::uint32_t height,
                                GpuImageImpl& image) {
    return createResidentImage(owner, width, height, image);
}
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::shared_ptr<GpuProcessingPhaseLease> phase(GpuProcessingPhaseBudget& budget,
                                               std::uint64_t bytes) {
    auto grant = budget.reserve({.fixedHostBytes = 1,
                                 .requestedExecutorBytes = bytes,
                                 .minimumExecutorBytes = bytes,
                                 .sourceFree = true});
    if (!grant.lease)
        throw std::runtime_error("actual test phase admission failed");
    return std::move(grant.lease);
}
void actualAllocation(GpuDevice& device) {
    auto state = GpuRendererAccess::state(device);
    require(state != nullptr, "real allocator state exists");
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(1ULL << 30, true);
    budget.setDeviceCapacity(1ULL << 30, true);
    auto admitted = phase(budget, 1ULL << 20);
    require(device.retainProcessingPhase(admitted), "real device retains same root phase");
    auto process = std::make_unique<GpuImageImpl>();
    process->state = state;
    require(createResidentImage(*state, 64, 64, *process), "real RGBA32F allocation succeeds");
    const auto actualProcess = process->storageToken.bytes();
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(state->allocator, process->allocation, &info);
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(state->allocator, &memory);
    const auto identityMatches = [&](const NativeImageStorageToken& token,
                                     const VmaAllocationInfo& actual) {
        const auto heap = memory->memoryTypes[actual.memoryType].heapIndex;
        const bool local = (memory->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        return token.bytes() == actual.size && token.identity().memoryType == actual.memoryType &&
               token.identity().heap == heap && token.identity().deviceLocal == local &&
               token.identity().allocatorGeneration == device.ownershipEpoch();
    };
    require(identityMatches(process->storageToken, info),
            "RGBA32F actual selected heap/domain are charged");
    require(actualProcess == info.size &&
                process->storageToken.identity().memoryType == info.memoryType &&
                process->storageToken.identity().allocatorGeneration == device.ownershipEpoch(),
            "actual VMA size/type/unique generation are charged");
    auto display = std::make_unique<GpuDisplayImageImpl>();
    display->state = state;
    require(createDisplayImage(*state, 64, 64, *display), "real RGBA8 allocation succeeds");
    const auto actualDisplay = display->storageToken.bytes();
    vmaGetAllocationInfo(state->allocator, display->allocation, &info);
    require(identityMatches(display->storageToken, info),
            "RGBA8 actual VMA size/type/heap/domain/unique generation are charged");
    require(budget.snapshot().ownedNativeImageBytes == actualProcess + actualDisplay &&
                budget.snapshot().ownedNativeImages == 2,
            "two actual image families transfer exactly one charge each");
    display.reset();
    process.reset();
    require(budget.snapshot().ownedNativeImages == 0 &&
                budget.snapshot().deviceReservedBytes == 0 &&
                budget.snapshot().totalDeviceClaimBytes == 0,
            "actual backing destruction folds all provisional debits and ends the device stage");
    device.retireProcessingPhase(admitted->id(), true);
    admitted.reset();
    for (const bool packed : {false, true}) {
        const auto actual = packed ? actualDisplay : actualProcess;
        admitted = phase(budget, actual - 1);
        require(device.retainProcessingPhase(admitted), "tight phase admitted");
        const auto attempts = state->nativeImageBackingAttempts;
        if (packed) {
            GpuDisplayImageImpl denied;
            denied.state = state;
            require(!createDisplayImage(*state, 64, 64, denied) && denied.image == VK_NULL_HANDLE &&
                        denied.allocation == VK_NULL_HANDLE && denied.storageToken.bytes() == 0,
                    "RGBA8 one-byte-short phase denies actual image backing");
        } else {
            GpuImageImpl denied;
            denied.state = state;
            require(!createResidentImage(*state, 64, 64, denied) &&
                        denied.image == VK_NULL_HANDLE && denied.allocation == VK_NULL_HANDLE &&
                        denied.storageToken.bytes() == 0,
                    "RGBA32F one-byte-short phase denies actual image backing");
        }
        require(state->nativeImageBackingAttempts == attempts,
                "one-byte-short actual preadmission issues zero VMA backing calls");
        const auto refused = device.nativeImageStorageRefusal();
        if (!refused)
            throw std::runtime_error("native refusal is missing exact capacity metadata");
        require(refused->status == GpuProcessingPhaseAdmission::BudgetRefused &&
                    refused->phaseId == admitted->id(),
                "real image capacity denial emits current-phase typed refusal");
        device.retireProcessingPhase(admitted->id(), true);
        admitted.reset();
    }
    // A peak-based phase must support successive nonoverlapping retained allocations. This
    // exercises the same actual backing/promotion/free boundary used by cache turnover.
    admitted = phase(budget, actualProcess);
    require(device.retainProcessingPhase(admitted), "turnover phase admitted");
    for (int iteration = 0; iteration != 3; ++iteration) {
        auto image = std::make_unique<GpuImageImpl>();
        image->state = state;
        require(createResidentImage(*state, 64, 64, *image),
                "one-image peak grant admits successive genuinely freed allocations");
        require(image->storageToken.promote(), "actual retained image promotes independently");
        image.reset();
    }
    device.retireProcessingPhase(admitted->id(), true);
    admitted.reset();
    require(budget.snapshot().totalDeviceClaimBytes == 0 &&
                budget.snapshot().totalHostClaimBytes == 0,
            "real family and turnover ownership conserves all claims");
}
void backingFaults(GpuDevice& device) {
    auto state = GpuRendererAccess::state(device);
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(1ULL << 30, true);
    budget.setDeviceCapacity(1ULL << 30, true);
    using Fault = vulkan_detail::NativeImageAllocationFault;
    for (bool packed : {false, true}) {
        for (Fault fault : {Fault::Allocation, Fault::Reconcile, Fault::Bind, Fault::View}) {
            auto admitted = phase(budget, 1ULL << 20);
            require(device.retainProcessingPhase(admitted), "fault phase retained");
            const auto attempts = state->nativeImageBackingAttempts;
            const auto destroyed = state->nativeImageBackingsDestroyed;
            state->nativeImageAllocationFault = fault;
            if (packed) {
                GpuDisplayImageImpl image;
                image.state = state;
                require(!createDisplayImage(*state, 64, 64, image) &&
                            image.image == VK_NULL_HANDLE && image.allocation == VK_NULL_HANDLE,
                        "injected packed backing branch returns no image");
            } else {
                GpuImageImpl image;
                image.state = state;
                require(!createResidentImage(*state, 64, 64, image) &&
                            image.image == VK_NULL_HANDLE && image.allocation == VK_NULL_HANDLE,
                        "injected process backing branch returns no image");
            }
            const bool backed = fault != Fault::Allocation;
            require(state->nativeImageAllocationFault == Fault::None &&
                        state->nativeImageBackingAttempts == attempts + (backed ? 1 : 0) &&
                        state->nativeImageBackingsDestroyed == destroyed + (backed ? 1 : 0),
                    "post-backing faults destroy exactly the actual successful backing");
            const auto remaining = budget.snapshot();
            require(remaining.ownedNativeImages == 0 && remaining.ownedNativeImageBytes == 0 &&
                        remaining.ownedNonlocalNativeHostBytes == 0 &&
                        remaining.deviceReservedBytes == 0 &&
                        remaining.totalDeviceClaimBytes == 0 &&
                        !device.nativeImageStorageRefusal(),
                    "injected noncapacity faults return debit without fabricated retry metadata");
            device.retireProcessingPhase(admitted->id(), true);
            admitted.reset();
        }
    }
}

// This private unknown flag is a conservative orphan-control witness, not simulated fence proof.
// The test removes its own flag only after verifying that the owner refuses to destroy the image.
template <class Impl, class Image, class Create, class Wrap>
void foreignAliases(GpuDevice& device, Create create, Wrap wrap) {
    auto state = GpuRendererAccess::state(device);
    auto budget = std::make_unique<GpuProcessingPhaseBudget>();
    budget->setHostCapacity(1ULL << 30, true);
    budget->setDeviceCapacity(1ULL << 30, true);
    auto admitted = phase(*budget, 1ULL << 20);
    require(device.retainProcessingPhase(admitted), "foreign alias phase retained");
    auto impl = std::make_unique<Impl>();
    impl->state = state;
    require(create(*state, 64, 64, *impl), "actual foreign alias backing exists");
    auto* raw = impl.get();
    const auto bytes = raw->storageToken.bytes();
    const auto nonlocal = raw->storageToken.identity().deviceLocal ? 0 : bytes;
    auto image = std::make_shared<Image>(wrap(std::move(impl)));
    require(image->promoteStorageOwnership(), "actual foreign image promoted");
    std::vector<std::shared_ptr<Image>> aliases(256, image);
    require(budget->snapshot().ownedNativeImages == 1 &&
                budget->snapshot().ownedNativeImageBytes == bytes &&
                budget->snapshot().ownedNonlocalNativeHostBytes == nonlocal,
            "256 aliases retain exactly one actual image and its host-domain subset");
    aliases.clear();
    device.retireProcessingPhase(admitted->id(), true);
    admitted.reset();
    const auto revision = budget->snapshot().admissionRevision;
    const auto destroyed = state->nativeImageBackingsDestroyed;
    raw->submissionUnretired.store(true);
    std::thread foreign([held = std::move(image)]() mutable { held.reset(); });
    foreign.join();
    image_detail::drainImages(*state);
    require(
        budget->snapshot().ownedNativeImages == 1 &&
            budget->snapshot().ownedNativeImageBytes == bytes &&
            budget->snapshot().ownedNonlocalNativeHostBytes == nonlocal &&
            budget->snapshot().admissionRevision == revision &&
            state->nativeImageBackingsDestroyed == destroyed &&
            device.imageReclamationStats().retiringImages >= 1,
        "foreign unknown orphan retains complete native backing/token without host-worker free");
    raw->submissionUnretired.store(false); // Only remove this test's explicit conservative flag.
    image_detail::drainImages(*state);
    require(
        budget->snapshot().ownedNativeImages == 0 &&
            budget->snapshot().ownedNativeImageBytes == 0 &&
            budget->snapshot().ownedNonlocalNativeHostBytes == 0 &&
            budget->snapshot().admissionRevision == revision + 1 &&
            state->nativeImageBackingsDestroyed == destroyed + 1,
        "native owner destroys actual orphan backing then releases exactly one independent claim");
}

void nonlocalDomains(GpuDevice& device) {
    auto state = GpuRendererAccess::state(device);
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(state->allocator, &memory);
    std::uint32_t mask = 0;
    for (std::uint32_t type = 0; type < memory->memoryTypeCount; ++type) {
        const auto heap = memory->memoryTypes[type].heapIndex;
        if ((memory->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0)
            mask |= std::uint32_t{1} << type;
    }
    if (mask == 0) {
        // A unified-memory device has no memory apart from system memory, so there is no nonlocal
        // image domain to cover; its device-local domain is covered by the cases above.
        if (device.availableAllocationBudget().unified_memory) {
            std::cout << "NotApplicable: a unified-memory device has no nonlocal image domain\n";
            return;
        }
        std::cout << "NotRun: actual device has no nonlocal image domain\n";
        return;
    }
    state->nativeImageMemoryTypeMaskForTests = mask;
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(1ULL << 30, true);
    budget.setDeviceCapacity(1ULL << 30, true);
    auto admitted = phase(budget, 1ULL << 20);
    require(device.retainProcessingPhase(admitted), "nonlocal capability probe phase retained");
    GpuImageImpl probe;
    probe.state = state;
    const bool compatible = createResidentImage(*state, 64, 64, probe);
    if (compatible) {
        require(!probe.storageToken.identity().deviceLocal &&
                    budget.snapshot().ownedNonlocalNativeHostBytes == probe.storageToken.bytes(),
                "actual nonlocal candidate admits additive host-domain ownership");
    }
    probe.destroy();
    device.retireProcessingPhase(admitted->id(), true);
    admitted.reset();
    if (!compatible) {
        state->nativeImageMemoryTypeMaskForTests = 0;
        std::cout << "NotRun: nonlocal types incompatible with actual image requirements\n";
        return;
    }
    foreignAliases<GpuImageImpl, GpuImage>(device, createResidentImageForTest, makeGpuImage);
    foreignAliases<GpuDisplayImageImpl, GpuDisplayImage>(device, createDisplayImage,
                                                         makeGpuDisplayImage);
    state->nativeImageMemoryTypeMaskForTests = 0;
    std::cout << "PASS: actual nonlocal image host-subset foreign/unknown-control conservation\n";
}

void downstreamRefusal(GpuDevice& device) {
    auto state = GpuRendererAccess::state(device);
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(1ULL << 30, true);
    budget.setDeviceCapacity(1ULL << 30, true);
    auto created = GpuResidentDisplay::create(device);
    require(bool(created), "actual neutral display pipeline exists");
    // Preserve this fixture's output-image refusal after processing buffers gained ownership.
    // Probe real requirements without an active phase, then admit that exact transient prefix.
    std::uint64_t workingBytes = 0;
    struct BufferSpec {
        std::uint64_t bytes;
        VkBufferUsageFlags usage;
        VmaAllocationCreateFlags flags;
        bool mapped;
    };
    const std::array specs{
        BufferSpec{64ULL * 64ULL * sizeof(Rgba32f),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, false},
        BufferSpec{64ULL * 64ULL * sizeof(Rgba8),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0, false},
        BufferSpec{4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                   true}};
    for (const auto& spec : specs) {
        ResidentBuffer buffer;
        require(createResidentDisplayBuffer(*state, spec.bytes, spec.usage, spec.flags, spec.mapped,
                                            buffer),
                "real downstream buffer requirement probe succeeds");
        workingBytes += buffer.info.size;
        destroyResidentBuffer(*state, buffer);
    }
    for (bool externalHolder : {false, true}) {
        // The published input leaves the phase's live set (#539), so the phase is sized to hold
        // the input while it is produced, then the display's buffers, but not its output image.
        auto admitted = phase(budget, workingBytes + 4096);
        require(device.retainProcessingPhase(admitted), "display refusal phase retained");
        auto impl = std::make_unique<GpuImageImpl>();
        impl->state = state;
        impl->generation = state->generation;
        const auto window = ImageWindow::create(0, 0, 64, 64);
        require(bool(window), "actual input geometry exists");
        impl->dataWindow = *window.value();
        impl->displayWindow = impl->dataWindow;
        require(createResidentImage(*state, 64, 64, *impl), "actual display input backing exists");
        std::shared_ptr<const GpuImage> input =
            std::make_shared<GpuImage>(makeGpuImage(std::move(impl)));
        require(input->promoteStorageOwnership(), "input handoff is independently published");
        auto external = externalHolder ? input : std::shared_ptr<const GpuImage>{};
        const auto attempts = state->nativeImageBackingAttempts;
        const auto diagnostic = created.display->begin(input, 1ULL << 20);
        require(diagnostic.code == GpuResidentDisplayDiagnosticCode::AllocationFailed &&
                    !created.display->hasUnretiredSubmission(),
                "actual downstream native output is refused before dispatch");
        const auto refused = device.nativeImageStorageRefusal();
        if (!refused)
            throw std::runtime_error(
                "downstream refusal is missing exact native capacity metadata");
        const auto refusalRevision = refused->revisionAfterOwnRelease;
        require(refused->status == GpuProcessingPhaseAdmission::BudgetRefused &&
                    state->nativeImageBackingAttempts == attempts,
                "downstream output has typed capacity metadata and zero backing attempts");
        require(created.display->discardRetiredFailure(device),
                "actual proven failed producer disposes its internal input alias");
        const bool owned = retireFailedAttemptGpuImage(input, device);
        require(owned != externalHolder, "only exact last internal input alias folds own cleanup");
        input.reset();
        if (externalHolder) {
            require(budget.snapshot().ownedNativeImages == 1,
                    "external published input remains charged after failed producer cleanup");
            external.reset(); // Independent release AFTER refusal, BEFORE terminal delivery.
        }
        device.retireProcessingPhase(admitted->id(), true);
        admitted.reset();
        require(budget.snapshot().ownedNativeImages == 0 &&
                    budget.snapshot().totalDeviceClaimBytes == 0 &&
                    budget.snapshot().admissionRevision ==
                        refusalRevision + (externalHolder ? 1 : 0),
                "own cleanup cannot self-retry and external pre-delivery release stays visible");
    }
}
} // namespace
// #544: a device-local reservation the root refuses for room asks the owner's reusable retention
// to give way, inside the allocation, and is tried once more; without a reclaimer, or when nothing
// gives way, the refusal stands with its capacity metadata.
void retentionGivesWayToRefusedAllocation(GpuDevice& device) {
    auto state = GpuRendererAccess::state(device);
    // Device-local types only: a compatible system-memory type would take the refused image.
    const VkPhysicalDeviceMemoryProperties* memory = nullptr;
    vmaGetMemoryProperties(state->allocator, &memory);
    std::uint32_t localTypes = 0;
    for (std::uint32_t type = 0; type < memory->memoryTypeCount; ++type)
        if ((memory->memoryHeaps[memory->memoryTypes[type].heapIndex].flags &
             VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            localTypes |= std::uint32_t{1} << type;
    state->nativeImageMemoryTypeMaskForTests = localTypes;
    GpuProcessingPhaseBudget budget;
    budget.setHostCapacity(1ULL << 30, true);
    budget.setDeviceCapacity(1ULL << 30, true);
    auto admitted = phase(budget, 1ULL << 20);
    require(device.retainProcessingPhase(admitted), "retention phase retained");
    const auto window = ImageWindow::create(0, 0, 64, 64);
    require(bool(window), "actual image geometry exists");
    // One published intermediate, standing in for a scene cache entry.
    auto first = std::make_unique<GpuImageImpl>();
    first->state = state;
    first->generation = state->generation;
    first->dataWindow = *window.value();
    first->displayWindow = first->dataWindow;
    require(createResidentImage(*state, 64, 64, *first), "the first image is allocated");
    std::shared_ptr<const GpuImage> retained =
        std::make_shared<GpuImage>(makeGpuImage(std::move(first)));
    require(retained->promoteStorageOwnership(), "the first image is published as retention");
    const auto held = budget.snapshot().totalDeviceClaimBytes;
    require(held != 0 && budget.snapshot().retainedNativeImageBytes == held,
            "the published image is the root's only device claim, as retention");
    // Room for half an image more: the next allocation does not fit beside the retention.
    budget.setDeviceCapacity(held + held / 2, true);
    {
        GpuImageImpl refused;
        refused.state = state;
        require(!createResidentImage(*state, 64, 64, refused),
                "without a reclaimer the allocation is refused for room");
        const auto refusal = device.nativeImageStorageRefusal();
        require(refusal && refusal->domain == GpuProcessingPhaseDomain::Device,
                "the refusal keeps its device capacity metadata");
    }
    int calls = 0;
    std::uint64_t asked = 0;
    device.setDeviceRoomReclaimer([&](const std::uint64_t bytes) -> std::uint64_t {
        ++calls;
        asked = bytes;
        if (!retained)
            return 0;
        const auto released = budget.snapshot().retainedNativeImageBytes;
        retained.reset();
        return released;
    });
    GpuImageImpl second;
    second.state = state;
    require(createResidentImage(*state, 64, 64, second),
            "the retention gives way inside the allocation, which then succeeds");
    require(calls == 1 && asked != 0 && !retained,
            "the reclaimer is asked once for the refused allocation's bytes");
    require(!device.nativeImageStorageRefusal(), "a successful retry leaves no refusal");
    second.destroy();
    // Nothing left to give way: the refusal stands after one reclaimer call.
    budget.setDeviceCapacity(held / 2, true);
    {
        GpuImageImpl refused;
        refused.state = state;
        require(!createResidentImage(*state, 64, 64, refused) && calls == 2,
                "with nothing to give way the reclaimer is asked once and the refusal stands");
        require(device.nativeImageStorageRefusal().has_value(),
                "the standing refusal keeps its capacity metadata");
    }
    device.setDeviceRoomReclaimer({});
    state->nativeImageMemoryTypeMaskForTests = 0;
    device.retireProcessingPhase(admitted->id(), true);
    admitted.reset();
    std::cout << "PASS: retention gives way inside a native allocation refused for room\n";
}

int main(int argc, char** argv) {
    try {
        GpuDeviceCreationOptions options;
        bool requireDevice = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--require-device")
                requireDevice = true;
            else if (std::string_view(argv[i]) == "--loader" && i + 1 < argc)
                options.loader_path = argv[++i];
            else
                return 2;
        }
        auto device = GpuDevice::create(options);
        if (!device) {
            std::cout << "Unsupported: " << device.diagnostic.message << '\n';
            return requireDevice ? 1 : 77;
        }
        actualAllocation(*device.device);
        downstreamRefusal(*device.device);
        backingFaults(*device.device);
        retentionGivesWayToRefusedAllocation(*device.device);
        nonlocalDomains(*device.device);
        foreignAliases<GpuImageImpl, GpuImage>(*device.device, createResidentImageForTest,
                                               makeGpuImage);
        foreignAliases<GpuDisplayImageImpl, GpuDisplayImage>(*device.device, createDisplayImage,
                                                             makeGpuDisplayImage);
        std::cout << "PASS: actual two-family native image backing/accounting/turnover\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
