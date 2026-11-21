#ifndef BLOOM_RENDER_VULKAN_GPU_AFFINE_PRIVATE_HPP
#define BLOOM_RENDER_VULKAN_GPU_AFFINE_PRIVATE_HPP

// Private to src/render/vulkan. Shared by gpu_affine.cpp (public API) and gpu_affine_resources.cpp
// (pipeline/buffer helpers). Vulkan-Hpp typed RAII stays here; the public header exposes no native
// type. It reuses the generic CompositeBuffer/CompositePipeline helpers declared by
// gpu_composite_private.hpp (no edit to gpu_composite.* is made) and the
// GpuImageImpl/GpuRendererAccess seams from gpu_image_private.hpp/gpu_device_private.hpp.

#include <bloom/render/gpu_affine.hpp>

#include "gpu_composite_private.hpp"
#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"
#include "shaders/affine_bilinear_spirv.inc"

#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
#include "gpu_scene_executor_fault_injection.hpp"
#endif

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>

namespace bloom::render {

// 5 bindings: source image (the lower mip level when filtered), output image, map buffer,
// error-flag buffer and (#742) the upper mip level image.
inline constexpr std::uint32_t kAffineBindingCount = 5;
inline constexpr bool kAffineBindingIsImage[kAffineBindingCount] = {true, true, false, false, true};
// The translation push-constant layout (output width/height, source width/height, opacity), the
// #480 sampling word (0 bilinear, 1 nearest, #742 2 trilinear, 3 anisotropic) and the #742
// filtered-sampling plan: 84 bytes of tightly packed uint32/float32.
inline constexpr std::uint32_t kAffinePushBytes = 84;

struct AffinePush final {
    std::uint32_t outputWidth;
    std::uint32_t outputHeight;
    std::uint32_t sourceWidth;
    std::uint32_t sourceHeight;
    float opacity;
    std::uint32_t sampling;
    std::uint32_t upperWidth = 0;
    std::uint32_t upperHeight = 0;
    float lowerScale = 1.0F;
    float upperScale = 1.0F;
    float upperWeight = 0.0F;
    std::uint32_t taps = 1;
    float tapWeight = 1.0F;
    float tapStart[4] = {};
    float tapStep[4] = {};
};
static_assert(sizeof(AffinePush) == kAffinePushBytes);

// The push words of one resample: the plan's levels and taps for a filtered (#742) sample, the
// plain fields otherwise. `sourceWidth`/`sourceHeight` are the bound lower image's extent.
[[nodiscard]] inline AffinePush
makeAffinePush(const std::uint32_t outputWidth, const std::uint32_t outputHeight,
               const std::uint32_t sourceWidth, const std::uint32_t sourceHeight,
               const float opacity, const GpuAffineFilter filter, const GpuAffineFilterPlan& plan,
               const std::uint32_t upperWidth, const std::uint32_t upperHeight) noexcept {
    AffinePush push{outputWidth,  outputHeight, sourceWidth,
                    sourceHeight, opacity,      static_cast<std::uint32_t>(filter)};
    push.upperWidth = upperWidth;
    push.upperHeight = upperHeight;
    if (filter == GpuAffineFilter::Trilinear || filter == GpuAffineFilter::Anisotropic) {
        push.lowerScale = std::ldexp(1.0F, -static_cast<int>(plan.lowerLevel));
        push.upperScale = std::ldexp(1.0F, -static_cast<int>(plan.upperLevel));
        push.upperWeight = plan.upperWeight;
        push.taps = plan.taps;
        push.tapWeight = plan.tapWeight;
        const auto split = [](const double value, float* pair) noexcept {
            const auto high = static_cast<float>(value);
            pair[0] = high;
            pair[1] = static_cast<float>(value - static_cast<double>(high));
        };
        split(plan.tapStartX, &push.tapStart[0]);
        split(plan.tapStartY, &push.tapStart[2]);
        split(plan.tapStepX, &push.tapStep[0]);
        split(plan.tapStepY, &push.tapStep[2]);
    }
    return push;
}

// Whether `parameters` names a filter and, for a filtered one, a plan that is the
// planAffineFilter() of its matrix with level images of the plan's extents; the reason otherwise.
[[nodiscard]] inline const char* affineFilterRefusal(const GpuAffineFilter filter,
                                                     const GpuAffineMatrix& matrix,
                                                     const GpuAffineFilterPlan& plan,
                                                     const ImageWindow lowerWindow,
                                                     const std::optional<ImageWindow> upperWindow) {
    if (filter == GpuAffineFilter::Bilinear || filter == GpuAffineFilter::Nearest)
        return nullptr;
    if (filter != GpuAffineFilter::Trilinear && filter != GpuAffineFilter::Anisotropic)
        return "the affine filter is unknown";
    if (plan.filter != filter ||
        planAffineFilter(matrix, filter, plan.sourceWidth, plan.sourceHeight) != plan)
        return "the filtered affine plan is not the plan of its matrix";
    if (!upperWindow)
        return "the filtered affine upper level image is missing";
    const auto lower = affineMipLevelExtent(plan.sourceWidth, plan.sourceHeight, plan.lowerLevel);
    const auto upper = affineMipLevelExtent(plan.sourceWidth, plan.sourceHeight, plan.upperLevel);
    if (lower != std::pair{lowerWindow.extent().width(), lowerWindow.extent().height()} ||
        upper != std::pair{upperWindow->extent().width(), upperWindow->extent().height()})
        return "the filtered affine level images do not have the plan's extents";
    return nullptr;
}

static_assert(sizeof(GpuAffineSample) == 16);

// Packed GPU layout of the compact O(1) inverse-affine map: six binary64 coefficients, each split
// into an error-free (hi, lo) Float32 pair, matching the shader's `vec2 map[6]` std430 buffer. The
// order is localX@origin, localY@origin, d(localX)/d(column), d(localX)/d(row),
// d(localY)/d(column), d(localY)/d(row).
struct GpuAffineMapGpu final {
    float coefficients[12] = {};

    friend bool operator==(const GpuAffineMapGpu&, const GpuAffineMapGpu&) noexcept = default;
};
static_assert(sizeof(GpuAffineMapGpu) == 48);

// The split is splitAffineMap(), the one the CPU reference (affineMatrixResampleRow) reconstructs
// from, so the device and the reference start from the same twelve Float32 values.
[[nodiscard]] inline GpuAffineMapGpu packAffineMap(const GpuAffineMap& map) noexcept {
    const auto split = splitAffineMap(map);
    GpuAffineMapGpu packed;
    for (std::size_t index = 0; index < split.size(); ++index) {
        packed.coefficients[index] = split[index];
    }
    return packed;
}

[[nodiscard]] inline GpuAffineDiagnostic affineDiagnostic(const GpuAffineDiagnosticCode code,
                                                          std::string message) {
    return GpuAffineDiagnostic{code, std::move(message)};
}

// The single job state machine behind GpuAffine, defined here so both translation units can
// implement its member operations.
//
// Sentinel for an Impl that owns no bounded resident-pool slot.
inline constexpr std::size_t kAffineNoResidentSlot = static_cast<std::size_t>(-1);

struct GpuAffine::Impl final {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    [[nodiscard]] bool onOwnerThread() const noexcept {
        return control != nullptr && control->owner == std::this_thread::get_id();
    }
    void fail(GpuAffineDiagnosticCode code, std::string message);
    void clearJob();
    [[nodiscard]] bool createPipelines();
    [[nodiscard]] bool drainAndRetire() noexcept;
    [[nodiscard]] bool checkStatusFlag();
    // Cheap owner/busy/device-generation gate run BEFORE any host metadata allocation so a foreign
    // thread or a stale generation is rejected without O(width*height) work.
    [[nodiscard]] GpuAffineDiagnostic preflightCheap();
    // Shared validation/upload/submit half used by both beginAffine() and beginAffineMatrix().
    [[nodiscard]] GpuAffineDiagnostic
    beginPrepared(const std::shared_ptr<const GpuImage>& source, ImageWindow outputWindow,
                  const GpuAffineMap& affineMap, float opacity, std::uint64_t byteBudget,
                  const Rgba32fImageDescriptor* output = nullptr,
                  GpuAffineFilter filter = GpuAffineFilter::Bilinear,
                  const std::shared_ptr<const GpuImage>& upper = nullptr,
                  const GpuAffineFilterPlan& plan = {});
    // Bounded resident-slot management, defined in gpu_affine_retirement.cpp. Acquire is called
    // before the first native allocation; release is owner-thread retirement; orphan is the
    // foreign-thread or unproven owner path that preserves the already-owned slot for the owner
    // drain. The drain is a static member because it names the private Impl type.
    [[nodiscard]] bool acquireResidentSlot() noexcept;
    void releaseResidentSlot() noexcept;
    void orphanResidentSlot() noexcept;
    static void drainResidentOrphansOnOwnerThread() noexcept;
    // Acquires the bounded slot and lazily builds the pipeline/layout/descriptor set/command
    // resources under it on the first begin. A full pool refuses before any native allocation; a
    // creation failure resets the partial native state and returns the slot. Returns false with
    // createDiagnostic set. Defined in gpu_affine.cpp.
    [[nodiscard]] bool ensureResidentReady();
    // Frees every pipeline/descriptor/command/fence native resource. Called only on the owner
    // thread after a failed ensureResidentReady() so an Impl that then holds no resident slot owns
    // no Vulkan object and may be destroyed from any thread.
    void resetPipelineResources() noexcept;

    std::thread::id owner;
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    GpuAffineBudgets budgets;
    std::uint32_t expectedGeneration = 0;
    // Bounded resident-pool slot owned by this Impl from before its first native allocation until
    // owner-thread release. kAffineNoResidentSlot means this Impl owns no native resources.
    std::size_t residentSlot = kAffineNoResidentSlot;
    bool pipelinesReady = false;

    CompositePipeline affine;
    vk::raii::DescriptorPool descriptorPool{nullptr};
    vk::raii::DescriptorSet affineSet{nullptr};
    vk::raii::CommandPool commandPool{nullptr};
    vk::raii::CommandBuffer commandBuffer{nullptr};
    vk::raii::Fence fence{nullptr};

    std::unique_ptr<GpuImage> residentImage;
    std::shared_ptr<const GpuImage> retainedSource;
    // #742: the upper mip level a filtered resample reads (binding 4), retained like the source.
    std::shared_ptr<const GpuImage> retainedUpper;
    CompositeBuffer map;
    CompositeBuffer status;
    void* statusMapped = nullptr;

    GpuAffineJobState jobState = GpuAffineJobState::Idle;
    bool queueSubmitted = false;
    std::uint64_t lastJobBytes = 0;
    bool deviceLost = false;
    std::atomic<bool> discardRequested{false};
    GpuAffineDiagnostic jobDiagnostic;
    GpuAffineDiagnostic createDiagnostic;
#ifdef BLOOM_GPU_SCENE_EXECUTOR_TEST_FAULT_INJECTION
    // TEST-ONLY. Compiles only in the fault closure; production builds never see the hook. It lives
    // here so the production translation unit stays within the line budget with identical semantics
    // to the composite poll hook.
    [[nodiscard]] std::optional<GpuAffinePollResult> injectedPollFault() {
        const auto injected = gpu_scene_executor_fault::take();
        if (injected == gpu_scene_executor_fault::PollFault::None) {
            return std::nullopt;
        }
        if (injected == gpu_scene_executor_fault::PollFault::StallPending) {
            return GpuAffinePollResult::Pending;
        }
        if (injected == gpu_scene_executor_fault::PollFault::DeviceLost) {
            const VkFence faultFence = static_cast<VkFence>(*fence);
            const VkResult faultWait = control->device.getDispatcher()->vkWaitForFences(
                static_cast<VkDevice>(*control->device), 1, &faultFence, VK_TRUE, 1'000'000'000ULL);
            if (faultWait == VK_SUCCESS) {
                deviceLost = true;
                queueSubmitted = false;
                fail(GpuAffineDiagnosticCode::DeviceLost,
                     "injected device loss after proven retirement");
            } else {
                fail(GpuAffineDiagnosticCode::DeviceUnavailable,
                     "injected device loss could not prove fence retirement; the submission is "
                     "retained");
            }
            return GpuAffinePollResult::Failure;
        }
        fail(GpuAffineDiagnosticCode::DeviceUnavailable,
             "injected unknown fence status; the submission is not retired");
        return GpuAffinePollResult::Failure;
    }
#endif
};

} // namespace bloom::render

#endif // BLOOM_RENDER_VULKAN_GPU_AFFINE_PRIVATE_HPP
