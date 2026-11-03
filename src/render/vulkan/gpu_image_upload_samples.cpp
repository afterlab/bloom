// #422: the native-sample path of GpuImageUpload. The decoded planes are copied into the staging
// buffer as they are and one SampleConvertV1 dispatch reads them there and writes the resident
// RGBA32F image, so the host never widens a frame to float. The job lifecycle (resident slot,
// fence, cancellation, retirement and quarantine) is the float path's, unchanged.

#include "gpu_image_upload_private.hpp"
#include "shaders/sample_convert_spirv.inc"

#include <bloom/core/telemetry_counters.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
#include <string>

namespace bloom::render {
namespace {

// The Vulkan maximum of minStorageBufferOffsetAlignment, so every plane binding offset is valid on
// every device without a property query.
constexpr std::uint64_t kPlaneAlignment = 256;
constexpr std::uint32_t kSampleBindingCount = 5;
// The Vulkan-guaranteed minimum of maxComputeWorkGroupCount[1].
constexpr std::uint64_t kMinWorkGroupCountY = 65535;
// Rows copied per parallel chunk.
constexpr std::size_t kCopyChunkBytes = 8U * 1024U * 1024U;

struct SamplePushConstants final {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t sampleLayout;
    std::uint32_t flags;
    std::array<std::uint32_t, 4> strides;
    std::uint32_t chromaWidth;
    std::uint32_t chromaHeight;
    std::int32_t transfer;
    float redFromCr;
    float greenFromCb;
    float greenFromCr;
    float blueFromCb;
    float wideToEight;
};
static_assert(sizeof(SamplePushConstants) == 64, "SampleConvertV1 push constants are 64 bytes");

[[nodiscard]] constexpr std::uint64_t alignUp(const std::uint64_t value,
                                              const std::uint64_t alignment) noexcept {
    return (value + alignment - 1U) / alignment * alignment;
}

// Bytes a plane occupies in staging and in its binding: whole 32-bit words.
[[nodiscard]] std::uint64_t planeRange(const NativeSamplePlane& plane) noexcept {
    return alignUp(static_cast<std::uint64_t>(plane.bytes.size()), 4U);
}

[[nodiscard]] SamplePushConstants pushConstants(const NativeSampleImage& samples) noexcept {
    const auto& conversion = samples.conversion();
    const auto planes = samples.planes();
    SamplePushConstants push{};
    push.width = samples.descriptor().dataWindow().extent().width();
    push.height = samples.descriptor().dataWindow().extent().height();
    push.sampleLayout = static_cast<std::uint32_t>(conversion.layout);
    push.flags = (conversion.storedSamples ? 1U : 0U) |
                 (static_cast<std::uint32_t>(conversion.alpha) << 1U) |
                 (conversion.limitedRange ? 8U : 0U) | (conversion.luminance ? 16U : 0U) |
                 (conversion.hasAlpha ? 32U : 0U);
    for (std::size_t index = 0; index < push.strides.size(); ++index)
        push.strides[index] = index < planes.size() ? planes[index].stride : 0U;
    const bool chroma = conversion.layout == NativeSampleLayout::Yuv420p8;
    push.chromaWidth = chroma ? planes[1].width : push.width;
    push.chromaHeight = chroma ? planes[1].height : push.height;
    push.transfer = conversion.transfer;
    push.redFromCr = conversion.redFromCr;
    push.greenFromCb = conversion.greenFromCb;
    push.greenFromCr = conversion.greenFromCr;
    push.blueFromCb = conversion.blueFromCb;
    push.wideToEight = 255.0F / 65535.0F;
    return push;
}

void copyPlane(std::byte* const destination, const NativeSamplePlane& plane,
               const std::function<void(std::size_t, const std::function<void(std::size_t)>&)>&
                   parallelFor) {
    const std::size_t bytes = plane.bytes.size();
    const std::size_t chunks = (bytes + kCopyChunkBytes - 1) / kCopyChunkBytes;
    if (!parallelFor || chunks < 2) {
        std::memcpy(destination, plane.bytes.data(), bytes);
    } else {
        parallelFor(chunks, [&](const std::size_t chunk) {
            const std::size_t offset = chunk * kCopyChunkBytes;
            std::memcpy(destination + offset, plane.bytes.data() + offset,
                        std::min(kCopyChunkBytes, bytes - offset));
        });
    }
    // The tail of the last word is padding the kernel never reads as a sample.
    const auto padded = static_cast<std::size_t>(planeRange(plane));
    if (padded > bytes)
        std::memset(destination + bytes, 0, padded - bytes);
}

} // namespace

bool GpuImageUpload::Impl::createSamplePipeline() {
    constexpr bool kBindings[kSampleBindingCount] = {false, false, false, false, true};
    std::string reason;
    if (!createCompositePipeline(*control, vulkan_detail::kSampleConvertSpirvCode,
                                 vulkan_detail::kSampleConvertSpirvByteCount, kBindings,
                                 kSampleBindingCount, sizeof(SamplePushConstants), reason,
                                 samplePipeline)) {
        createDiagnostic = uploadDiagnostic(GpuImageUploadDiagnosticCode::Unsupported,
                                            "SampleConvertV1 was rejected: " + reason);
        return false;
    }
    const std::array poolSizes{vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 4},
                               vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1}};
    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    const VkDevice rawDevice = static_cast<VkDevice>(*control->device);
    const auto* dispatcher = control->device.getDispatcher();
    VkDescriptorPool rawPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorPool(
            rawDevice, reinterpret_cast<const VkDescriptorPoolCreateInfo*>(&poolInfo), nullptr,
            &rawPool) != VK_SUCCESS) {
        createDiagnostic = uploadDiagnostic(GpuImageUploadDiagnosticCode::AllocationFailed,
                                            "the sample-convert descriptor pool failed");
        return false;
    }
    sampleDescriptorPool = vk::raii::DescriptorPool(control->device, rawPool);
    const vk::DescriptorSetLayout setLayout = *samplePipeline.descriptorSetLayout;
    vk::DescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.descriptorPool = *sampleDescriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &setLayout;
    VkDescriptorSet rawSet = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateDescriptorSets(
            rawDevice, reinterpret_cast<const VkDescriptorSetAllocateInfo*>(&allocateInfo),
            &rawSet) != VK_SUCCESS) {
        createDiagnostic = uploadDiagnostic(GpuImageUploadDiagnosticCode::AllocationFailed,
                                            "the sample-convert descriptor set failed");
        return false;
    }
    sampleDescriptorSet = vk::raii::DescriptorSet(control->device, rawSet, *sampleDescriptorPool);
    return true;
}

void GpuImageUpload::Impl::resetSamplePipeline() noexcept {
    sampleDescriptorSet = vk::raii::DescriptorSet{nullptr};
    sampleDescriptorPool = vk::raii::DescriptorPool{nullptr};
    samplePipeline.pipeline = vk::raii::Pipeline{nullptr};
    samplePipeline.pipelineLayout = vk::raii::PipelineLayout{nullptr};
    samplePipeline.descriptorSetLayout = vk::raii::DescriptorSetLayout{nullptr};
    samplePipeline.shaderModule = vk::raii::ShaderModule{nullptr};
    samplePipelineReady = false;
}

GpuImageUploadDiagnostic GpuImageUpload::Impl::beginSamples(const GpuImageUploadParameters& parameters,
                                                            const std::uint64_t byteBudget) {
    const NativeSampleImage& samples = *parameters.samples;
    const Rgba32fImageDescriptor& sampleDescriptor = samples.descriptor();
    const Rgba32fImageDescriptor* const descriptor =
        parameters.descriptorOverride ? &*parameters.descriptorOverride : &sampleDescriptor;
    if (descriptor->layout() != sampleDescriptor.layout()) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::InvalidArgument,
                                "the upload descriptor layout does not match its samples");
    }
    const std::uint32_t width = descriptor->dataWindow().extent().width();
    const std::uint32_t height = descriptor->dataWindow().extent().height();
    const auto planes = samples.planes();
    if (width == 0 || height == 0 || planes.empty()) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::InvalidArgument,
                                "the upload samples are empty");
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    const std::uint64_t imageBytes = pixels * sizeof(Rgba32f);
    std::array<std::uint64_t, kMaxNativeSamplePlanes> offsets{};
    std::uint64_t stagingBytes = 0;
    for (std::size_t index = 0; index < planes.size(); ++index) {
        const auto range = planeRange(planes[index]);
        if (range == 0 || range > control->maxStorageBufferRange) {
            return uploadDiagnostic(GpuImageUploadDiagnosticCode::Unsupported,
                                    "a sample plane exceeds the device storage-buffer range");
        }
        offsets[index] = alignUp(stagingBytes, kPlaneAlignment);
        stagingBytes = offsets[index] + range;
    }
    if (imageBytes > budgets.maxImageBytes) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::OverBudget,
                                "the resident image exceeds the configured byte budget");
    }
    if (stagingBytes > budgets.maxStagingBytes) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::OverBudget,
                                "the upload staging buffer exceeds the configured byte budget");
    }
    if (imageBytes > byteBudget || stagingBytes > byteBudget - imageBytes) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::OverBudget,
                                "the image plus its staging peak exceed the requested byte budget");
    }
    const std::uint64_t groups = (pixels + 255U) / 256U;
    const std::uint64_t maxX = std::max<std::uint32_t>(control->maxComputeWorkGroupCountX, 1U);
    const std::uint64_t groupsX = std::min(groups, maxX);
    const std::uint64_t groupsY = (groups + groupsX - 1U) / groupsX;
    if (groupsY > kMinWorkGroupCountY) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::Unsupported,
                                "the sample-convert grid exceeds the device workgroup limits");
    }
    if (control->presentationEpoch != expectedEpoch) {
        deviceLost = true;
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::DeviceLost,
                                "the device generation changed; this pipeline must not be reused");
    }
    const SolidImageSupport support = querySolidImageSupport(*control, width, height);
    if (!support.supported) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::Unsupported, support.reason);
    }
    if (imageBytes > support.maxImageBytes) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::OverBudget,
                                "the resident image exceeds the device resource limit");
    }
    if (!acquireResidentSlot()) {
        return uploadDiagnostic(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                                "the bounded upload resident pool is full; no native resources were "
                                "allocated");
    }
    if (!resourcesReady) {
        if (!createResources()) {
            resetResources();
            releaseResidentSlot();
            return createDiagnostic;
        }
        resourcesReady = true;
    }
    if (!samplePipelineReady) {
        if (!createSamplePipeline()) {
            resetSamplePipeline();
            return createDiagnostic;
        }
        samplePipelineReady = true;
    }

    clearJob();
    retainSpare = parameters.reuseStagingWithinScene;
    try {
        // The float path's spare reuse rule, for a spare that was created readable by the kernel.
        const void* const phase = control->processingPhase.get();
        bool reused = false;
        if (spareStaging.armed) {
            const auto spareBytes = spareStaging.bytes;
            constexpr std::uint64_t kRoundingMargin = 1ULL << 20U;
            if (retainSpare && spareStaging.storage && sparePhase == phase &&
                spareBytes >= stagingBytes && spareBytes <= budgets.maxStagingBytes &&
                byteBudget >= imageBytes && byteBudget - imageBytes >= spareBytes + kRoundingMargin) {
                staging = std::move(spareStaging);
                reused = true;
            } else {
                spareStaging.release();
            }
            sparePhase = nullptr;
        }
        const auto stagingOutcome =
            reused ? vulkan_detail::NativeBufferAllocationOutcome::Allocated
                   : createUploadStagingBuffer(*control, stagingBytes, budgets.maxStagingBytes,
                                               staging, /*storage=*/true);
        if (!reused && stagingOutcome == vulkan_detail::NativeBufferAllocationOutcome::Allocated)
            ++stagingAllocations;
        if (stagingOutcome != vulkan_detail::NativeBufferAllocationOutcome::Allocated) {
            const bool refused =
                stagingOutcome == vulkan_detail::NativeBufferAllocationOutcome::PhaseRefused ||
                stagingOutcome ==
                    vulkan_detail::NativeBufferAllocationOutcome::TransferBudgetRefused;
            fail(refused ? GpuImageUploadDiagnosticCode::OverBudget
                         : GpuImageUploadDiagnosticCode::AllocationFailed,
                 "the upload staging buffer could not be allocated");
            return jobDiagnostic;
        }
        VmaAllocationInfo stagingInfo{};
        vmaGetAllocationInfo(control->allocator, staging.allocation, &stagingInfo);
        if (stagingInfo.pMappedData == nullptr) {
            staging.release();
            fail(GpuImageUploadDiagnosticCode::AllocationFailed,
                 "the upload staging buffer could not be mapped");
            return jobDiagnostic;
        }
        auto* const stagingBase = static_cast<std::byte*>(stagingInfo.pMappedData);
        for (std::size_t index = 0; index < planes.size(); ++index)
            copyPlane(stagingBase + offsets[index], planes[index], parameters.parallelFor);
        if (vmaFlushAllocation(control->allocator, staging.allocation, 0, VK_WHOLE_SIZE) !=
            VK_SUCCESS) {
            staging.release();
            fail(GpuImageUploadDiagnosticCode::AllocationFailed,
                 "the upload staging buffer could not be flushed");
            return jobDiagnostic;
        }

        auto resident = std::make_unique<GpuImageImpl>();
        resident->state = control;
        resident->dataWindow = descriptor->dataWindow();
        resident->displayWindow = descriptor->displayWindow();
        resident->pixelAspect = descriptor->pixelAspect();
        resident->generation = control->generation;
        if (!createResidentImage(*control, width, height, *resident)) {
            staging.release();
            fail(control->imageStorageRefusal ? GpuImageUploadDiagnosticCode::OverBudget
                                              : GpuImageUploadDiagnosticCode::AllocationFailed,
                 "the resident image could not be allocated");
            return jobDiagnostic;
        }
        GpuImageImpl* const residentRaw = resident.get();
        const std::uint64_t actualImage = uploadAllocationBytes(*control, resident->allocation);
        const std::uint64_t actualStaging = uploadAllocationBytes(*control, staging.allocation);
        if (actualImage > budgets.maxImageBytes || actualStaging > budgets.maxStagingBytes ||
            actualImage > byteBudget || actualStaging > byteBudget - actualImage) {
            staging.release();
            fail(GpuImageUploadDiagnosticCode::OverBudget,
                 "actual VMA allocation sizes exceed the configured or requested byte budget");
            return jobDiagnostic;
        }
        lastJobBytes = actualImage + actualStaging;
        residentImage = std::make_unique<GpuImage>(makeGpuImage(std::move(resident)));

        const VkDevice rawDevice = static_cast<VkDevice>(*control->device);
        const auto* dispatcher = control->device.getDispatcher();
        // The set is only rewritten between jobs: begin() refuses while a submission is in flight.
        std::array<VkDescriptorBufferInfo, 4> bufferInfos{};
        for (std::size_t binding = 0; binding < bufferInfos.size(); ++binding) {
            // An unused binding aliases plane 0; the kernel never reads it for that layout.
            const std::size_t plane = binding < planes.size() ? binding : 0U;
            bufferInfos[binding].buffer = staging.buffer;
            bufferInfos[binding].offset = offsets[plane];
            bufferInfos[binding].range = planeRange(planes[plane]);
        }
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = residentRaw->view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        std::array<VkWriteDescriptorSet, kSampleBindingCount> writes{};
        const VkDescriptorSet rawSet = static_cast<VkDescriptorSet>(*sampleDescriptorSet);
        for (std::uint32_t binding = 0; binding < kSampleBindingCount; ++binding) {
            auto& write = writes[binding];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = rawSet;
            write.dstBinding = binding;
            write.descriptorCount = 1;
            if (binding < 4) {
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &bufferInfos[binding];
            } else {
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                write.pImageInfo = &imageInfo;
            }
        }
        dispatcher->vkUpdateDescriptorSets(rawDevice, kSampleBindingCount, writes.data(), 0,
                                           nullptr);

        const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*commandBuffer);
        const VkFence rawFence = static_cast<VkFence>(*fence);
        if (dispatcher->vkResetFences(rawDevice, 1, &rawFence) != VK_SUCCESS ||
            dispatcher->vkResetCommandBuffer(rawCommandBuffer, 0) != VK_SUCCESS) {
            releaseResident();
            staging.release();
            fail(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                 "the upload fence or command buffer could not be reset");
            return jobDiagnostic;
        }
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dispatcher->vkBeginCommandBuffer(rawCommandBuffer, &beginInfo) != VK_SUCCESS) {
            releaseResident();
            staging.release();
            fail(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                 "the upload command buffer could not begin");
            return jobDiagnostic;
        }
        VkImageMemoryBarrier toGeneral{};
        toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toGeneral.srcAccessMask = 0;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = residentRaw->image;
        toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                         nullptr, 1, &toGeneral);
        dispatcher->vkCmdBindPipeline(rawCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                      static_cast<VkPipeline>(*samplePipeline.pipeline));
        dispatcher->vkCmdBindDescriptorSets(
            rawCommandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            static_cast<VkPipelineLayout>(*samplePipeline.pipelineLayout), 0, 1, &rawSet, 0,
            nullptr);
        const SamplePushConstants push = pushConstants(samples);
        dispatcher->vkCmdPushConstants(
            rawCommandBuffer, static_cast<VkPipelineLayout>(*samplePipeline.pipelineLayout),
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        dispatcher->vkCmdDispatch(rawCommandBuffer, static_cast<std::uint32_t>(groupsX),
                                  static_cast<std::uint32_t>(groupsY), 1);
        VkImageMemoryBarrier written = toGeneral;
        written.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        written.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        written.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        dispatcher->vkCmdPipelineBarrier(rawCommandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                         nullptr, 1, &written);
        if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
            releaseResident();
            staging.release();
            fail(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                 "the upload command buffer could not end");
            return jobDiagnostic;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &rawCommandBuffer;
        core::telemetry::addGpuSubmit();
        core::telemetry::addGpuUploadBytes(stagingBytes);
        const VkResult submitted = dispatcher->vkQueueSubmit(
            static_cast<VkQueue>(*control->computeQueue), 1, &submit, rawFence);
        if (submitted == VK_ERROR_DEVICE_LOST) {
            deviceLost = true;
            releaseResident();
            staging.release();
            fail(GpuImageUploadDiagnosticCode::DeviceLost, "the device was lost during submission");
            return jobDiagnostic;
        }
        if (submitted != VK_SUCCESS) {
            releaseResident();
            staging.release();
            fail(GpuImageUploadDiagnosticCode::DeviceUnavailable,
                 "the sample conversion could not submit");
            return jobDiagnostic;
        }
        queueSubmitted = true;
        ++sampleConvertedUploads;
        jobState = GpuImageUploadJobState::Pending;
        jobDiagnostic = GpuImageUploadDiagnostic{};
        return {};
    } catch (const std::bad_alloc&) {
        releaseResident();
        staging.release();
        fail(GpuImageUploadDiagnosticCode::AllocationFailed, "the upload job could not be allocated");
        return jobDiagnostic;
    } catch (...) {
        releaseResident();
        staging.release();
        fail(GpuImageUploadDiagnosticCode::DeviceUnavailable, "the upload job failed unexpectedly");
        return jobDiagnostic;
    }
}

} // namespace bloom::render
