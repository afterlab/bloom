#pragma once

// Bloom-owned, Qt-free and Vulkan-free GPU device surface. Nothing in this header exposes a
// Vulkan/native handle, a Vulkan flag, or a catch-all backend interface: callers receive typed
// Bloom values, an immutable capability report, ordered structured diagnostics, and opaque
// move-only resource ownership. src/render owns the concrete Vulkan bootstrap behind a pimpl, so a
// build without Vulkan dependencies compiles the same API against an explicit Unavailable stub.
//
// A dedicated runtime GPU service will call create() only on its service thread; the created
// device records that thread and fails closed when an exposed operation is called from another
// thread. This slice does not integrate with the runtime, UI, or task system.

#include <bloom/render/gpu_allocator_memory.hpp>
#include <bloom/render/gpu_presentation_types.hpp>
#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/render/native_buffer_storage.hpp>
#include <bloom/render/native_image_storage.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bloom::render {
class GpuProcessingPhaseLease;

// Per-operation-and-precision outcome. It is never a device-wide marketing tier: a Ready device
// grants no operation a qualification outcome.
enum class GpuQualification : std::uint8_t {
    Unavailable,
    PreviewOnly,
    ReferenceParity,
};

enum class GpuDeviceState : std::uint8_t {
    Initializing,
    Ready,
    Lost,
    Recovering,
    Unavailable,
    ShuttingDown,
    Stopped,
};

// The initial closed operation vocabulary from docs/architecture/gpu-backend.md. Declaring the
// vocabulary does not claim any implementation; the report's operation list grows one tested
// capability at a time instead of inventing every operation up front.
enum class GpuOperationId : std::uint8_t {
    SolidV1,
    TranslationOpacityBilinearV1,
    SourceOverV1,
    OcioDisplayV1,
    PackedDisplayV1,
};

enum class GpuPrecision : std::uint8_t {
    Rgba32f,
    Rgba16f,
    PackedRgba8,
};

// Typed reason a device or resource operation could not be completed. The code is stable and
// machine-checkable; the message is a bounded, human-readable diagnostic.
enum class GpuDiagnosticCode : std::uint8_t {
    None,
    BackendNotBuilt,
    LoaderUnavailable,
    InstanceCreationFailed,
    NoPhysicalDevice,
    DeviceIncompatible,
    DeviceCreationFailed,
    AllocatorUnavailable,
    AllocationLimitExceeded,
    BufferAllocationFailed,
    WrongThread,
    InvalidArgument,
    DeviceUnavailable,
};

struct GpuDiagnostic final {
    GpuDiagnosticCode code = GpuDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuDiagnostic&, const GpuDiagnostic&) = default;
};

// Selected backend, physical device, and driver identity. This is diagnostic evidence and cache
// identity, not project authoring state.
struct GpuDeviceIdentity final {
    std::string backend;
    std::string device_name;
    std::string driver;
    std::uint32_t driver_version = 0;
    std::uint32_t api_version_major = 0;
    std::uint32_t api_version_minor = 0;
    std::uint32_t vendor_id = 0;
    std::string device_type;
    // The physical device's driver-reported UUID (VkPhysicalDeviceIDProperties::deviceUUID); all
    // zero when unknown. Two owners of one physical GPU report the same value.
    std::array<std::uint8_t, 16> device_uuid{};

    friend bool operator==(const GpuDeviceIdentity&, const GpuDeviceIdentity&) = default;
};

// One operation-and-precision entry. A missing entry means Unavailable; unsupported operations do
// not inherit another operation's outcome. `qualification` stays Unavailable until the frozen
// fixture gate passes, and the ordered `diagnostics` explain why it has not.
struct GpuOperationCapability final {
    GpuOperationId operation = GpuOperationId::SolidV1;
    GpuPrecision precision = GpuPrecision::Rgba32f;
    GpuQualification qualification = GpuQualification::Unavailable;
    std::vector<std::string> required_features;
    std::string tested_revision;
    std::string numeric_contract;
    std::string fixture_set_digest;
    std::vector<GpuDiagnostic> diagnostics;
};

enum class GpuPhaseRecovery : std::uint8_t {
    // No unproven phase is latched on this generation.
    NotNeeded,
    // The device went idle; the latched phase was proven retired and released.
    Recovered,
    // The device was lost; the generation must be replaced.
    DeviceLost,
    // Not the owner thread, or the device could not be waited on.
    Failed,
};

// Typed source of a resolved device allocation budget. `MemoryBudget` is an actual available
// figure the driver reported (VK_EXT_memory_budget through VMA); `DeviceLocalEstimate` is a
// conservative nominal DEVICE_LOCAL heap figure used when no live budget is available (integrated
// GPUs share host memory and drivers migrate blocks, so it is never a claim of free VRAM);
// `Unavailable` means no budget could be resolved (null/stub device, foreign thread, no heap, or a
// live query the driver did not answer, which is never reported as zero room).
enum class GpuAllocationBudgetSource : std::uint8_t {
    Unavailable,
    MemoryBudget,
    DeviceLocalEstimate,
};

// Immutable, owner-thread-resolved device allocation budget. `available_bytes` is the conservative
// allocation budget a planner may build on; `device_local_bytes` is the summed DEVICE_LOCAL heap
// size and is a diagnostic fact only, never an availability claim.
struct GpuAllocationBudget final {
    GpuAllocationBudgetSource source = GpuAllocationBudgetSource::Unavailable;
    std::uint64_t device_local_bytes = 0;
    std::uint64_t available_bytes = 0;
    // Unified memory: every memory heap of the device is DEVICE_LOCAL, so the device has no memory
    // apart from system memory (Apple silicon, integrated GPUs). What the driver calls available
    // then says nothing about the memory the rest of the system is using; the planner bounds this
    // budget by the measured system memory as well (docs/decisions/0039). False for a device with
    // its own memory, whose budget stands alone.
    bool unified_memory = false;
    // Which physical GPU this live budget describes (all zero when unknown). Owners that share a
    // GPU share its process-wide budget, so ownership credit is applied once per physical device.
    std::array<std::uint8_t, 16> device_uuid{};
    // The device's allocation sizes (null when unknown); shared by every budget it resolves.
    std::shared_ptr<const GpuAllocationSizes> allocation_sizes = nullptr;

    friend bool operator==(const GpuAllocationBudget&, const GpuAllocationBudget&) = default;
};

// Immutable, generation-scoped report. `Ready` means the bootstrap requirements needed to probe
// and submit work passed; it grants no operation qualification.
struct GpuCapabilityReport final {
    std::uint32_t generation = 0;
    GpuDeviceState state = GpuDeviceState::Unavailable;
    GpuDeviceIdentity identity;
    bool compute_queue = false;
    bool timeline_semaphore = false;
    bool memory_budget_supported = false;
    // An actually enabled calibrated-timestamp extension guarantees cross-submit monotonicity.
    bool cross_submission_timestamps = false;
    // Sum of DEVICE_LOCAL memory heap sizes. A bounded diagnostic fact, not a VRAM capacity or a
    // usable allocation budget: integrated GPUs share host memory and drivers may migrate blocks.
    std::uint64_t device_memory_bytes = 0;
    // Presentation bootstrap facts for this generation. NotRequested unless create() was asked for
    // presentation; Ready only when every required capability was actually enabled.
    GpuPresentationStatus presentation;
    std::vector<GpuOperationCapability> operations;
};

// Facts about one opaque allocation. No native handle is exposed.
struct GpuBufferInfo final {
    std::uint64_t size_bytes = 0;
    bool host_visible = false;
};

// Image ownership diagnostics count native allocation bytes, not aggregate VRAM or process RAM.
// Retiring includes final releases awaiting their exact owner. Closed-owner retiring images remain
// bounded and explicitly charged when that owner can no longer pump; no off-thread teardown occurs.
struct GpuImageReclamationStats final {
    std::uint64_t liveImages = 0;
    std::uint64_t liveAllocationBytes = 0;
    std::uint64_t retiringImages = 0;
    std::uint64_t retiringAllocationBytes = 0;
    std::uint64_t closedOwnerRetiringImages = 0;
    std::uint64_t closedOwnerRetiringAllocationBytes = 0;
    std::uint64_t capacityPerFamily = 0;
    std::uint64_t admissionRefusals = 0;
    std::uint64_t ownerReclaims = 0;
};

// Native storage reuse (#477): retired transient images and buffers parked for the next frame of
// the same shape instead of being freed. Bytes are the parked allocations: device-local ones, and
// host-visible system memory (staging) apart. `reused` counts allocations served from the parked
// set, `created` the ones that needed a new native allocation while reuse was enabled.
struct GpuNativeStorageReuseStats final {
    bool enabled = false;
    std::uint64_t parkedEntries = 0;
    std::uint64_t parkedDeviceBytes = 0;
    std::uint64_t parkedHostBytes = 0;
    std::uint64_t reused = 0;
    std::uint64_t created = 0;
    std::uint64_t evictedBytes = 0;
};

// Exact-generation program ownership, including live aliases that have not requested retirement.
// Counts are lifetime diagnostics, not owned allocation bytes.
struct GpuOcioProgramRetirementStats final {
    std::uint64_t registeredPrograms = 0;
    std::uint64_t pendingPrograms = 0;
    std::uint64_t closedOwnerPendingPrograms = 0;
    std::uint64_t pendingUploads = 0;
};

class GpuDevice;
class GpuRendererAccess;

// Opaque, move-only ownership of one allocation. An allocation co-owns the device's allocator
// generation, so the underlying Vulkan allocator and device handles stay alive until the last
// buffer is released even if the GpuDevice object is destroyed first. The GpuDevice object itself
// must still be destroyed on its creating owner thread, and releasing an allocation on any other
// thread trips that same owner-thread assertion.
class GpuBufferAllocation final {
  public:
    GpuBufferAllocation() noexcept;
    GpuBufferAllocation(const GpuBufferAllocation&) = delete;
    GpuBufferAllocation& operator=(const GpuBufferAllocation&) = delete;
    GpuBufferAllocation(GpuBufferAllocation&& other) noexcept;
    GpuBufferAllocation& operator=(GpuBufferAllocation&& other) noexcept;
    ~GpuBufferAllocation();

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] GpuBufferInfo info() const noexcept;

  private:
    friend class GpuDevice;

    struct Impl;
    explicit GpuBufferAllocation(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuBufferAllocationResult final {
    GpuBufferAllocation allocation;
    GpuDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return allocation.isValid(); }
    explicit operator bool() const noexcept { return hasValue(); }
};

// The Vulkan loader packaged with this executable on macOS, which has no system loader: the one
// GpuDevice::create opens when no loader is named. Empty when none is packaged, in a build without
// the Vulkan backend, and on every other platform. For callers that hand a loader path to another
// component.
[[nodiscard]] std::filesystem::path packagedVulkanLoaderPath();

// Explicit loader override for controlled tests and diagnostics. Production callers leave this
// empty and receive the platform loader (on macOS, the packaged one); no workspace or build path
// is ever hardcoded.
struct GpuDeviceCreationOptions final {
    std::filesystem::path loader_path;
    // Optional presentation bootstrap. The default (false / None) reproduces the compute-only
    // device exactly as before. When requested, only actually-supported surface extensions are
    // enabled; a missing capability leaves the compute path intact and reports a typed Unavailable
    // status.
    bool request_presentation = false;
    GpuPresentationPlatform presentation_platform = GpuPresentationPlatform::None;
    // Per-user directory for the persisted pipeline cache (#457): derived runtime cache state,
    // never project state. Empty keeps the device's pipeline cache in memory only (tests, tools).
    // The directory is created on the first write.
    std::filesystem::path pipeline_cache_directory{};
    // Models a card with this much device-local memory (#682): every device-local heap is capped
    // at it in the allocator, so allocations beyond it fail, and the allocation budget is this
    // size less the device's own allocations. The driver's live figure is not read, so what other
    // programs hold on the real card does not change what a test is admitted against. 0 (the
    // default) is the whole device. Tests use it to plan for a stated card on any machine;
    // nothing else sets it.
    std::uint64_t device_memory_limit_bytes = 0;
};

// What the device's pipeline cache started from (#457). `NotPersistent`: no cache directory was
// given, so the cache lives in memory for this device generation only. `Missing`: no cache file for
// this device yet. `Loaded`: a file whose Bloom header, checksum, device, driver and Vulkan
// pipeline-cache header all matched seeded the cache. `Rejected`: a file existed but was stale,
// foreign, truncated or corrupt; the cache started empty and the next write replaces the file.
enum class GpuPipelineCacheLoad : std::uint8_t { NotPersistent, Missing, Loaded, Rejected };

struct GpuPipelineCacheStats final {
    // A native VkPipelineCache exists and every pipeline creation of this generation uses it.
    bool active = false;
    GpuPipelineCacheLoad load = GpuPipelineCacheLoad::NotPersistent;
    // Why a file was rejected, or why the last write was skipped; empty otherwise.
    std::string detail;
    std::uint64_t loadedBytes = 0;
    // Pipelines this generation created through the cache.
    std::uint64_t pipelinesCreated = 0;
    std::uint64_t writes = 0;
    std::uint64_t lastWriteBytes = 0;
    std::filesystem::path file;
};

struct GpuDeviceCreationResult;

class GpuDevice final {
  public:
    GpuDevice(const GpuDevice&) = delete;
    GpuDevice& operator=(const GpuDevice&) = delete;
    GpuDevice(GpuDevice&& other) noexcept;
    GpuDevice& operator=(GpuDevice&& other) noexcept;
    ~GpuDevice();

    // Probe and bootstrap one logical device generation on the calling thread. A missing loader,
    // incompatible or absent device, or allocator failure produces a typed Unavailable diagnostic
    // and a null device instead of a crash.
    [[nodiscard]] static GpuDeviceCreationResult
    create(const GpuDeviceCreationOptions& options = {});

    [[nodiscard]] GpuDeviceState state() const noexcept;
    // True only on the thread that created this device generation. False for a null/moved-from or
    // portable-stub device. No allocation or native call is involved.
    [[nodiscard]] bool isOwnerThread() const noexcept;
    // Non-blocking, owner-thread-only reclaim of final image releases for this exact generation.
    // Submission-unproved images stay charged. Compute retirement is never a WSI retirement proof.
    void reclaimReleasedImages() noexcept;
    [[nodiscard]] GpuImageReclamationStats imageReclamationStats() const noexcept;
    [[nodiscard]] GpuOcioProgramRetirementStats ocioProgramRetirementStats() const noexcept;
    // Owner-thread only (#477). While enabled, a transient processing image or buffer this
    // generation's allocator issued is parked when its owner releases it, and the next allocation
    // of the identical shape takes it back instead of allocating; nothing is reused while a parked
    // object could still be referenced, because owners release only after their submissions are
    // proven retired. Disabling frees everything parked. Off by default.
    void setNativeStorageReuse(bool enabled) noexcept;
    [[nodiscard]] GpuNativeStorageReuseStats nativeStorageReuseStats() const noexcept;
    // Owner-thread only. Frees parked storage, least recently parked first, until at least `bytes`
    // are released: device-local storage when `deviceLocal`, host-visible system memory otherwise.
    // Returns the bytes released.
    std::uint64_t releaseReusableNativeStorage(std::uint64_t bytes, bool deviceLocal) noexcept;
    // Owner-thread only (#544). When the processing root refuses a phase's device-local native
    // allocation for device room, `reclaimer` is asked for at least that allocation's bytes (the
    // owner's reusable retention: its scene cache, parked storage) and the allocation is tried
    // once more if it released anything. It runs on the owner thread inside the allocation, must
    // not allocate native storage itself, and returns the bytes it released. Empty clears it.
    void setDeviceRoomReclaimer(std::function<std::uint64_t(std::uint64_t)> reclaimer) noexcept;
    // Pipeline cache (#457). `persistPipelineCache` is owner-thread only and writes the cache file
    // when this generation created pipelines since the last write (the device also writes it when
    // it is destroyed); it never runs on the UI thread because only the device owner may call it.
    // Returns true when a file was written.
    [[nodiscard]] GpuPipelineCacheStats pipelineCacheStats() const noexcept;
    bool persistPipelineCache() noexcept;
    // Includes closed generations after their GpuDevice facade has been destroyed.
    [[nodiscard]] static GpuImageReclamationStats processImageReclamationStats() noexcept;
    [[nodiscard]] const GpuCapabilityReport& capabilityReport() const noexcept;

    // Owner-thread only. Resolves this generation's conservative allocation budget: the live VMA
    // heap budget when VK_EXT_memory_budget was actually enabled, otherwise a nominal DEVICE_LOCAL
    // estimate. Returns Unavailable for a non-owner thread, a null/moved-from or stub device, or a
    // device that reported no memory. Unlike `device_memory_bytes` in the capability report (a
    // summed heap-size diagnostic), this is a distinct permission to plan allocations.
    [[nodiscard]] GpuAllocationBudget availableAllocationBudget() const noexcept;

    // Unique ownership identity for this exact GpuDevice generation. It is minted once per created
    // device from the same per-process monotonic counter that backs the presentation epoch, so two
    // GpuDevice instances -- even two created from the same physical GPU -- never share it. The
    // bootstrap capability report's generation is always 1 and cannot distinguish two instances;
    // qualification identity must use this instead. Zero only for a null/moved-from or
    // portable-stub device. No native handle is exposed.
    [[nodiscard]] std::uint64_t ownershipEpoch() const noexcept;

    // Owner-only, one-slot conservative phase retention. Arm before any native work, including
    // synchronous preparation uploads. Only an explicit whole-phase retirement proof releases it;
    // unknown retirement closes this generation and keeps the quota through native quarantines.
    // Explicit persistent allocation root for presentation and other phase-independent native
    // ownership. A bound generation cannot switch roots or retain a phase from a different root.
    [[nodiscard]] bool bindProcessingRoot(GpuProcessingRootIdentity) noexcept;
    [[nodiscard]] GpuProcessingRootIdentity processingRootIdentity() const noexcept;
    [[nodiscard]] bool retainProcessingPhase(std::shared_ptr<GpuProcessingPhaseLease>) noexcept;
    void retireProcessingPhase(std::uint64_t leaseId, bool retirementProven) noexcept;
    // True while a phase is retained or parked.
    [[nodiscard]] bool processingPhaseOccupied() const noexcept;
    // Owner-only (#477): one frame's display and transfer may finish while the next frame's
    // processing runs. `parkProcessingPhase` moves the retained phase aside (only a proven one,
    // and only when none is parked yet) so the next frame's phase can be retained;
    // `selectProcessingPhase` makes the retained or parked phase with `leaseId` the retained one,
    // parking the other. Native storage and retirement always apply to the retained phase, so an
    // owner selects a frame's phase before each native step of that frame.
    [[nodiscard]] bool parkProcessingPhase() noexcept;
    [[nodiscard]] bool selectProcessingPhase(std::uint64_t leaseId) noexcept;
    // Owner-only recovery from an unproven retirement (a failed, timed-out or cancelled frame whose
    // native work could not be proven retired). Waits for the whole device to go idle, which proves
    // every submission on every queue of this generation has completed, then proves and releases
    // the latched phase so the generation admits work again. The fence-before-free guard holds:
    // nothing is released before the idle proof. A lost device is reported, never released.
    [[nodiscard]] GpuPhaseRecovery recoverUnprovenProcessingPhase() noexcept;
    // Owner-only authority read for synchronous native resource preparation. Returns the exact
    // already-retained lease; callers must not arm a second phase. Null for non-owner/stub devices.
    [[nodiscard]] std::shared_ptr<GpuProcessingPhaseLease> retainedProcessingPhase() const noexcept;
    // Owner-only actual native-image capacity refusal for the currently retained phase. Native
    // allocation/bind/view faults have no retry metadata. Cleared on the next phase/allocation.
    [[nodiscard]] std::optional<NativeImageStorageRefusal>
    nativeImageStorageRefusal() const noexcept;
    [[nodiscard]] std::optional<NativeBufferStorageRefusal>
    nativeBufferStorageRefusal() const noexcept;
    [[nodiscard]] std::optional<NativePresentationStorageRefusal>
    nativePresentationStorageRefusal() const noexcept;
    // Exact-generation live reservation or handed-off staging; neither permits native owner exit.
    [[nodiscard]] bool displayTransferPending() const noexcept;
    [[nodiscard]] bool outputColorReadbackPending() const noexcept;
    [[nodiscard]] bool imageUploadPending() const noexcept;
    // Fresh owner-only VMA/driver diagnostics. Foreign/stub calls return unavailable without
    // driver work; services publish cached copies to UI consumers at a bounded sampling rate.
    [[nodiscard]] GpuAllocatorMemorySnapshot allocatorMemorySnapshot() const noexcept;

    // Missing operation/precision entries are Unavailable, matching the architecture contract.
    [[nodiscard]] GpuQualification qualificationFor(GpuOperationId operation,
                                                    GpuPrecision precision) const noexcept;

    // Presentation bootstrap facts for this device generation. Ready means the requested platform's
    // surface extension, VK_KHR_surface, VK_KHR_swapchain, and a present-capable queue were all
    // actually enabled; it never claims support for any specific surface.
    [[nodiscard]] GpuPresentationStatus presentationStatus() const noexcept;

    // Non-owning borrowed view of the live instance, for handing the native Vulkan instance to the
    // UI layer.
    // The returned fields are immutable after create(), so this is safe to call from the UI thread.
    // `valid` is false unless presentationStatus() is Ready. The caller must keep this GpuDevice
    // alive on its owner thread until every borrowed surface is retired and acknowledged; this view
    // never owns or destroys the instance.
    [[nodiscard]] GpuBorrowedInstanceView borrowedInstanceView() const noexcept;

    // Owner-thread validation of a surface the UI created against the borrowed instance. Device
    // state, presentation epoch, and calling thread are checked BEFORE any driver call; a stale
    // epoch or a non-owner thread is rejected without touching the driver. Only then is
    // vkGetPhysicalDeviceSurfaceSupportKHR queried on the preselected present queue family.
    [[nodiscard]] GpuSurfaceSupportResult
    validateBorrowedSurface(const GpuBorrowedSurface& surface) const;

    // Bounded host-visible buffer allocation. Exposed here as the minimal owned resource that
    // proves the allocator is initialized with explicit dynamic functions.
    [[nodiscard]] GpuBufferAllocationResult allocateHostBuffer(std::uint64_t size_bytes);

  private:
    friend class GpuRendererAccess;

    struct Impl;
    explicit GpuDevice(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuDeviceCreationResult final {
    std::unique_ptr<GpuDevice> device;
    GpuDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return device != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

// Upper bound checked before any allocator call. Deployment policy may lower it later; the
// allocation path must never silently clamp or overflow.
inline constexpr std::uint64_t kMaxGpuHostBufferBytes = 64ULL * 1024ULL * 1024ULL;

} // namespace bloom::render
