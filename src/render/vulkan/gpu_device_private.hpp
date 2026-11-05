#ifndef BLOOM_RENDER_VULKAN_GPU_DEVICE_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_DEVICE_PRIVATE_HPP

// Private to src/render/vulkan. Never included by the public render surface, the CPU stub, or the
// portable test smoke. Vulkan-Hpp supplies typed handles, structs, and RAII owners; Bloom opens the
// platform loader privately at runtime and defines VMA's implementation in its own translation
// unit, so no translation unit needs static Vulkan prototypes.

#if !defined(VK_NO_PROTOTYPES)
#define VK_NO_PROTOTYPES 1
#endif
#if !defined(VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL)
#define VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL 0
#endif
#if !defined(VULKAN_HPP_NO_EXCEPTIONS)
#define VULKAN_HPP_NO_EXCEPTIONS 1
#endif

#if !defined(VMA_STATIC_VULKAN_FUNCTIONS)
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#endif
#if !defined(VMA_DYNAMIC_VULKAN_FUNCTIONS)
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#endif

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_presentation_types.hpp>
#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/render/native_image_storage.hpp>

#include <vulkan/vulkan_raii.hpp>

#include <vk_mem_alloc.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bloom::render::queue_timing_detail {
struct State;
}
namespace bloom::render::operation_batch_detail {
struct State;
struct Pipelines;
}

namespace bloom::render::ocio_program_detail {
struct ProgramRetirementNode;
}

namespace bloom::render::vulkan_detail {

// Owns one runtime-opened platform loader. The loader must outlive every function pointer,
// instance, and device resolved through it, so it is destroyed after the Vulkan handles below.
class DynamicLibrary final {
  public:
    DynamicLibrary() noexcept = default;
    explicit DynamicLibrary(const char* path) noexcept;
    ~DynamicLibrary();

    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

    [[nodiscard]] void* symbol(const char* name) const noexcept;
    [[nodiscard]] bool isOpen() const noexcept { return handle_ != nullptr; }

  private:
    void close() noexcept;

    void* handle_ = nullptr;
};

[[nodiscard]] std::uint64_t deviceOwnerLifetimeToken() noexcept;

// Owns one logical device generation and its memory allocator. Destruction tears the allocator
// down in the body, then lets the RAII members release in reverse declaration order: queue, device,
// physical device, instance, and finally the loader. The owner thread is recorded so misuse can
// fail closed rather than race a device.
// Private one-shot owner-thread fault control; None is the production path.
enum class NativeImageAllocationFault : std::uint8_t { None, Allocation, Reconcile, Bind, View };
enum class NativeBufferAllocationFault : std::uint8_t { None, Allocation, Reconcile, Bind, Map };

// #477: retired transient native storage kept for the next frame of the same shape. A frame's
// processing images and buffers (uploads, intermediates, the display image and the packed
// readback staging) are created and destroyed per frame; each creation is a driver allocation and
// mapping. With reuse enabled, an image or buffer the allocator issued is parked instead of freed
// when its owner releases it (after its submission is proven retired, as before), and the next
// allocation with the identical create info and allocator flags takes it back under a fresh
// storage token for the current phase. Parked storage holds no content anyone reads (a reused
// image starts in an undefined layout exactly like a new one) and is evicted least recently
// parked first whenever work waits for memory or the device's free memory falls below the
// headroom. Off by default; an owner opts in. All members are guarded by `mutex`.
struct NativeStorageReuse final {
    struct Shape final {
        bool image = false;
        std::uint32_t flags = 0;
        std::uint32_t kind = 0; // image type, or buffer sharing mode
        std::uint32_t format = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t depth = 0;
        std::uint32_t levels = 0;
        std::uint32_t layers = 0;
        std::uint32_t samples = 0;
        std::uint32_t tiling = 0;
        std::uint32_t usage = 0;
        std::uint64_t bytes = 0; // buffer size
        std::uint32_t allocationFlags = 0;
        std::uint32_t requiredFlags = 0;
        std::uint32_t preferredFlags = 0;
        std::uint32_t memoryUsage = 0;
        friend bool operator==(const Shape&, const Shape&) = default;
    };
    struct Entry final {
        Shape shape;
        std::uint64_t handle = 0; // VkImage or VkBuffer bits
        VmaAllocation allocation = VK_NULL_HANDLE;
        std::uint64_t requirementBytes = 0;
        std::uint64_t allocationBytes = 0;
        std::uint32_t memoryType = 0;
        std::uint32_t heap = 0;
        bool deviceLocal = false;
    };
    std::mutex mutex;
    bool enabled = false;
    // Issued by the allocator while enabled, keyed by handle bits (images and buffers apart).
    std::unordered_map<std::uint64_t, Entry> issuedImages;
    std::unordered_map<std::uint64_t, Entry> issuedBuffers;
    // Least recently parked first.
    std::vector<Entry> parked;
    std::uint64_t parkedDeviceBytes = 0;
    std::uint64_t parkedHostBytes = 0;
    std::uint64_t reused = 0;
    std::uint64_t created = 0;
    std::uint64_t evictedBytes = 0;
};

struct DeviceAllocatorState final {
    DeviceAllocatorState() = default;
    DeviceAllocatorState(const DeviceAllocatorState&) = delete;
    DeviceAllocatorState& operator=(const DeviceAllocatorState&) = delete;
    DeviceAllocatorState(DeviceAllocatorState&&) = delete;
    DeviceAllocatorState& operator=(DeviceAllocatorState&&) = delete;
    ~DeviceAllocatorState();

    // One serial timestamp owner. Final foreign/unproven release preserves this exact native
    // owner for an allocation-free owner drain; repeated timing requests cannot grow quarantine.
    std::atomic<bool> queueTimingOccupied{false};
    std::weak_ptr<queue_timing_detail::State> queueTiming;
    std::atomic<queue_timing_detail::State*> retiredQueueTiming{nullptr};
    // One bounded multi-kernel command owner. Unproven/foreign final release preserves this exact
    // owner, including all intermediate backing, until an owner-thread fence proof drains it.
    std::atomic<bool> operationBatchOccupied{false};
    std::atomic<operation_batch_detail::State*> retiredOperationBatch{nullptr};

    std::mutex programRetirementMutex;
    ocio_program_detail::ProgramRetirementNode* programRetirementHead = nullptr;
    std::uint64_t registeredPrograms = 0;
    std::uint64_t pendingPrograms = 0;
    bool programDrainActive = false;
    bool programOwnerClosed = false;

    std::thread::id owner;
    std::uint64_t ownerLifetimeToken = 0;
    // Final-image release publication is allocation-free, including after facade shutdown.
    std::atomic<std::uint64_t> pendingImageReclaims{0};
    std::atomic<std::uint64_t> pendingOracleReadbacks{0};
    std::atomic<std::uint64_t> imageReclamationScans{0};
    // Owner-only backing-call witness, distinct from unbound-image/requirements queries.
    std::uint64_t nativeQueueTimingBackingAttempts = 0;
    std::uint64_t nativeImageBackingAttempts = 0;
    std::uint64_t nativeImageBackingsDestroyed = 0;
    std::uint64_t nativeBufferBackingAttempts = 0;
    std::uint64_t nativeBufferBackingsDestroyed = 0;
    std::uint64_t presentationEpoch = 0;
    std::uint64_t borrowedInstanceBits = 0;
    std::uint64_t maxStorageBufferRange = 0;
    // Summed DEVICE_LOCAL heap size, not committed allocator bytes.
    std::uint64_t deviceLocalBytes = 0;
    // Driver-reported physical device UUID, copied into every live allocation budget.
    std::array<std::uint8_t, 16> deviceUuid{};
    // Owner-only VMA frame index. Advancing it refreshes VK_EXT_memory_budget even when this
    // allocator has made no allocations while other processes changed device-memory usage.
    std::uint32_t budgetFrameIndex = 0;

    // Declared before native RAII fields: all native teardown precedes final quota release. The
    // lease points only to a neutral ledger, never back to this generation (no reference cycle).
    std::shared_ptr<GpuProcessingPhaseLease> processingPhase;
    // #477: a proven phase moved aside while the next frame's phase is retained; see
    // GpuDevice::parkProcessingPhase.
    std::shared_ptr<GpuProcessingPhaseLease> parkedProcessingPhase;
    GpuProcessingRootIdentity processingRoot;
    std::optional<NativePresentationStorageRefusal> presentationStorageRefusal;
    std::optional<NativeImageStorageRefusal> imageStorageRefusal;
    std::optional<NativeBufferStorageRefusal> bufferStorageRefusal;
    // #544: the owner's reusable device retention (its scene cache, parked native storage), asked
    // to give way when a phase's device-local reservation is refused for device room; the
    // reservation is then tried once more. Owner thread only; returns the bytes it released.
    std::function<std::uint64_t(std::uint64_t)> deviceRoomReclaimer;
    bool reclaimingDeviceRoom = false;
    // Native RAII declaration order is deliberate: queues/device/instance die before the loader.
    DynamicLibrary loader;
    vk::raii::Instance instance{nullptr};
    vk::raii::PhysicalDevice physicalDevice{nullptr};
    vk::raii::Device device{nullptr};
    vk::raii::Queue computeQueue{nullptr};
    // Non-owning aliases of the same VkDevice queues (vk::raii::Queue has no destroy). presentQueue
    // is only a distinct queue when the presentation family differs from the compute family.
    vk::raii::Queue presentQueue{nullptr};
    VmaAllocator allocator = VK_NULL_HANDLE;
    NativeStorageReuse storageReuse;
    // #386: the batch owner's compute pipelines, created once per device generation and shared by
    // every batch owner it creates (an executor releases its owner after each scene). The
    // destructor releases them before it clears `device`. Owner thread only.
    std::shared_ptr<operation_batch_detail::Pipelines> operationBatchPipelines;

    // Nonzero private domain-control mask intersects real image requirements; 0 is unmodified.
    std::uint32_t nativeImageMemoryTypeMaskForTests = 0;
    std::uint32_t nativeBufferMemoryTypeMaskForTests = 0;
    std::uint32_t computeQueueFamily = 0;
    std::uint32_t presentQueueFamily = 0;
    std::uint32_t generation = 0;
    std::uint32_t maxComputeWorkGroupCountX = 0;
    std::uint32_t maxComputeWorkGroupInvocations = 0;
    std::uint32_t maxComputeWorkGroupSizeX = 0;

    // Owner-only lifecycle flag; surviving images retain this allocator and remain valid.
    bool imageOwnerClosed = false;
    // Scoped private format-fallback control; production retains actual capability selection.
    bool processingPhaseUnproven = false;
    NativeImageAllocationFault nativeImageAllocationFault = NativeImageAllocationFault::None;
    NativeBufferAllocationFault nativeBufferAllocationFault = NativeBufferAllocationFault::None;
    // Immutable presentation bootstrap facts, resolved only on the owner thread.
    bool presentationRequested = false;
    bool presentationReady = false;
    bool surfaceExtensionEnabled = false;
    bool platformSurfaceExtensionEnabled = false;
    bool swapchainExtensionEnabled = false;
    bool presentFencesEnabled = false;
    bool presentWaitEnabled = false;
    // Enabled only when advertised and requested for Float64 kernels.
    bool shaderFloat64 = false;
    // True only when VK_EXT_memory_budget and the matching VMA flag are both enabled.
    bool memoryBudgetEnabled = false;
    bool calibratedTimestampsEnabled = false;
};

// #544: reserves a phase's native storage, and when the root refuses a device-local reservation
// for device room, lets the owner's reusable retention give way first and tries once more. A
// running scene publishes its finished intermediates to the scene cache as retention; without
// this, a 4K scene filled the device with them and its next allocation failed the whole frame,
// which then waited, re-prepared and refilled the same cache (or, on the final render route,
// fell back to the CPU). Owner thread only; never nested.
template <typename Reserve>
[[nodiscard]] auto reserveGivingWayToRetention(DeviceAllocatorState& state, const bool deviceLocal,
                                               const std::uint64_t bytes, Reserve&& reserve) {
    auto grant = reserve();
    if (grant.token || !deviceLocal || grant.domain != GpuProcessingPhaseDomain::Device ||
        (grant.status != GpuProcessingPhaseAdmission::AwaitingCapacity &&
         grant.status != GpuProcessingPhaseAdmission::BudgetRefused) ||
        !state.deviceRoomReclaimer || state.reclaimingDeviceRoom)
        return grant;
    state.reclaimingDeviceRoom = true;
    std::uint64_t released = 0;
    try {
        released = state.deviceRoomReclaimer(bytes);
    } catch (...) {
        released = 0;
    }
    state.reclaimingDeviceRoom = false;
    if (released != 0)
        grant = reserve();
    return grant;
}

} // namespace bloom::render::vulkan_detail

namespace bloom::render {

// Native handles cross the public boundary as integer bits only; no Vk* type is ever public.
// Defined here so the bootstrap and resource translation units share one conversion without a new
// header.
template <typename Handle>
[[nodiscard]] inline std::uint64_t handleBits(const Handle handle) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
}

template <typename Handle>
[[nodiscard]] inline Handle handleFromBits(const std::uint64_t bits) noexcept {
    // The public contract carries native handles as opaque integer bits only. Reconstructing the
    // typed handle is the single necessary int-to-pointer boundary and has no safer equivalent.
    // NOLINTNEXTLINE(performance-no-int-to-ptr): documented native-handle reconstruction boundary.
    return reinterpret_cast<Handle>(static_cast<std::uintptr_t>(bits));
}

[[nodiscard]] inline GpuDiagnostic diagnostic(const GpuDiagnosticCode code, std::string message) {
    return GpuDiagnostic{code, std::move(message)};
}

// The single device-generation state behind GpuDevice. Defined here so the bootstrap translation
// unit can construct it during create() while the resource translation unit implements the
// remaining members, with no second definition of the pimpl.
struct GpuDevice::Impl final {
    std::thread::id owner;
    GpuDeviceState state = GpuDeviceState::Unavailable;
    GpuCapabilityReport report;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;

    // Immutable after create(): safe for borrowedInstanceView() reads from the UI thread.
    bool presentationReady = false;
    std::uint64_t presentationEpoch = 0;
    std::uint32_t presentQueueFamily = 0;
    std::uint64_t borrowedInstanceBits = 0;
};

// The only non-public bridge from a render operation to the device's Vulkan allocator and queue.
// GpuDevice befriends this class; the CPU stub never defines or references it, so a build without
// Vulkan dependencies links the stub API unchanged.
class GpuRendererAccess final {
  public:
    [[nodiscard]] static std::shared_ptr<vulkan_detail::DeviceAllocatorState>
    state(GpuDevice& device) noexcept;
    [[nodiscard]] static std::thread::id owner(const GpuDevice& device) noexcept;
};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_DEVICE_PRIVATE_HPP
