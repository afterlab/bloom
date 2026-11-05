#pragma once

#include "gpu_affine_private.hpp"
#include "gpu_blend_private.hpp"
#include "gpu_ocio_program_private.hpp"
#include "gpu_solid_private.hpp"
#include "shaders/solid_spirv.inc"
#include "shaders/gradient_spirv.inc"

#include <bloom/render/gpu_operation_batch.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace bloom::render::operation_batch_detail {
enum class PollFault : std::uint8_t { None, Pending, Unknown, LostAfterProof, StatusAfterProof };
inline std::atomic<std::size_t> orphanCount{0};
struct Job final {
    std::unique_ptr<GpuImageImpl> image;
    std::array<CompositeBuffer, 3> metadata;
    std::shared_ptr<const GpuImage> sourcePin;
    std::shared_ptr<const GpuImage> destinationPin;
    std::shared_ptr<const GpuImage> maskPin;
    const GpuImageImpl* source = nullptr;
    const GpuImageImpl* destination = nullptr;
    const GpuImageImpl* mask = nullptr;
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    std::array<std::uint32_t, 24> push{};
    std::uint32_t pushBytes = 0;
    std::size_t kind = 0;
    // #386 blend: the selected kernel (one of Pipelines' blend kernels).
    const CompositePipeline* pipeline = nullptr;
    // #386 OCIO effect: the borrowed program, whether this batch's submission marked it, the
    // uniform bytes written before submission and the capacity-safe dispatch grid.
    GpuOcioProgramBatchAccess::ProgramImpl* ocio = nullptr;
    bool ocioMarked = false;
    std::vector<std::byte> uniform;
    std::uint32_t groupsX = 0;
    std::uint32_t groupsY = 0;
};
inline constexpr std::size_t kBlendKind = 5;
inline constexpr std::size_t kOcioEffectKind = 6;
// The batch kernels: solid, translation, affine, source-over and gradient by operation index, then
// GpuBlend's exact, Float64, portable and image-combine kernels (#386).
struct Pipelines final {
    std::array<CompositePipeline, 5> base;
    CompositePipeline blend;
    CompositePipeline blendF64;
    CompositePipeline blendPortable;
    CompositePipeline imageCombine;
    bool generalUsesF64 = false;
};
struct State final {
    std::shared_ptr<vulkan_detail::DeviceAllocatorState> control;
    std::shared_ptr<Pipelines> pipelines;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::array<Job, kGpuOperationBatchCapacity> jobs;
    std::uint32_t count = 0;
    std::uint32_t expectedGeneration = 0;
    GpuOperationBatchState state = GpuOperationBatchState::Idle;
    GpuOperationBatchDiagnostic diagnostic;
    GpuOperationBatchStatistics statistics;
    bool ownsSlot = false;
    bool submitted = false;
    bool cancelled = false;
    bool lost = false;
    bool initialized = false;
    bool initializationAttempted = false;
    bool orphaned = false;
    PollFault fault = PollFault::None;

    ~State();
    [[nodiscard]] bool owner() const noexcept;
    [[nodiscard]] bool initialize();
    void clear(bool restorePrefix) noexcept;
    void fail(GpuOperationBatchCode, const char*) noexcept;
    [[nodiscard]] bool prepare(std::span<const GpuBatchOperation>, const GpuOperationBatchBudgets&,
                               std::uint64_t byteBudget);
    [[nodiscard]] bool record();
    [[nodiscard]] bool recordOcioEffect(Job& job);
    // Clears the #386 OCIO program marks once the batch's submission is proven retired or lost.
    void releaseOcioPrograms() noexcept;
    void markOcioPrograms() noexcept;
    [[nodiscard]] bool submit();
    void poll() noexcept;
};
void retire(State*) noexcept;
void drainRetired(vulkan_detail::DeviceAllocatorState&) noexcept;
} // namespace bloom::render::operation_batch_detail
