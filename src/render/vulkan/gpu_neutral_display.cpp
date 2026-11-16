#include "gpu_neutral_display_private.hpp"

#include "gpu_neutral_display_kernel.hpp"
#include <bloom/core/telemetry_counters.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace bloom::render {
namespace {

using neutral_display_detail::kInputBytesPerPixel;
using neutral_display_detail::kOutputBytesPerPixel;
using neutral_display_detail::kStatusBytes;
using neutral_display_detail::kWorkgroupSizeX;

[[nodiscard]] GpuNeutralDisplayDiagnostic makeDiagnostic(const GpuNeutralDisplayDiagnosticCode code,
                                                         std::string message) {
    return GpuNeutralDisplayDiagnostic{code, std::move(message)};
}

} // namespace

GpuNeutralDisplay::GpuNeutralDisplay(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

GpuNeutralDisplay::GpuNeutralDisplay(GpuNeutralDisplay&& other) noexcept = default;

GpuNeutralDisplay& GpuNeutralDisplay::operator=(GpuNeutralDisplay&& other) noexcept {
    if (this != &other) {
        releaseImpl();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

GpuNeutralDisplay::~GpuNeutralDisplay() { releaseImpl(); }

void GpuNeutralDisplay::releaseImpl() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->onOwnerThread()) {
        // The device owner thread is the only place a queue submission can be drained. A foreign
        // thread must not race it, so report the limitation and retain the generation rather than
        // risk destroying live Vulkan objects. The process-wide fuse bounds repeated leaks.
        neutral_display_detail::noteQuarantine();
        [[maybe_unused]] const auto* const quarantined = impl_.release();
        return;
    }
    if (!impl_->drainAndRetire()) {
        // Retirement could not be proved within the bounded drain; leak deliberately.
        if (impl_->state->processingPhase) {
            impl_->state->processingPhaseUnproven = true;
            impl_->state->processingPhase->markUnproven();
        }
        neutral_display_detail::noteQuarantine();
        [[maybe_unused]] const auto* const quarantined = impl_.release();
        return;
    }
    impl_.reset();
}

GpuNeutralDisplayCreateResult GpuNeutralDisplay::create(GpuDevice& device,
                                                        const GpuNeutralDisplayBudgets& budgets) {
    if (budgets.maxInputBytes < kInputBytesPerPixel ||
        budgets.maxOwnedBytes < kInputBytesPerPixel + kOutputBytesPerPixel + kStatusBytes) {
        return {nullptr, makeDiagnostic(GpuNeutralDisplayDiagnosticCode::InvalidArgument,
                                        "the neutral display budgets are out of range")};
    }
    if (device.state() != GpuDeviceState::Ready) {
        return {nullptr, makeDiagnostic(GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
                                        "the GPU device is not Ready")};
    }
    // The pipeline's Vulkan objects are created on the device's owner thread and used only there;
    // creating them from a different thread would race the device, so fail closed before touching
    // any Vulkan state.
    if (GpuRendererAccess::owner(device) != std::this_thread::get_id()) {
        return {nullptr,
                makeDiagnostic(GpuNeutralDisplayDiagnosticCode::WrongThread,
                               "the neutral display pipeline must be created on the device "
                               "owner thread")};
    }
    auto state = GpuRendererAccess::state(device);
    if (state == nullptr) {
        return {nullptr, makeDiagnostic(GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
                                        "the GPU device exposes no renderer state")};
    }
    if (!neutral_display_detail::quarantineAllowed()) {
        return {nullptr,
                makeDiagnostic(GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
                               "the process has quarantined too many undrained GPU generations; no "
                               "further display pipeline is created")};
    }

    auto impl = std::make_unique<Impl>();
    impl->owner = std::this_thread::get_id();
    impl->state = std::move(state);
    // A permissive configured maximum is an upper bound, not an allocation: clamp the effective
    // ceiling instead of refusing the pipeline. begin() validates the actual requested bytes.
    impl->budgets = budgets;
    impl->budgets.maxInputBytes = budgets.maxInputBytes < neutral_display_detail::kMaxBudgetBytes
                                      ? budgets.maxInputBytes
                                      : neutral_display_detail::kMaxBudgetBytes;
    impl->budgets.maxOwnedBytes = budgets.maxOwnedBytes < neutral_display_detail::kMaxBudgetBytes
                                      ? budgets.maxOwnedBytes
                                      : neutral_display_detail::kMaxBudgetBytes;
    impl->expectedGeneration = impl->state->generation;
    if (!impl->createPipeline()) {
        return {nullptr, impl->createDiagnostic};
    }
    return {std::unique_ptr<GpuNeutralDisplay>(new GpuNeutralDisplay(std::move(impl))),
            GpuNeutralDisplayDiagnostic{}};
}

GpuNeutralDisplayJobState GpuNeutralDisplay::state() const noexcept {
    return impl_ != nullptr ? impl_->jobState : GpuNeutralDisplayJobState::Idle;
}

bool GpuNeutralDisplay::hasUnretiredSubmission() const noexcept {
    return impl_ && (!impl_->onOwnerThread() || impl_->queueSubmitted || impl_->deviceLost);
}

bool GpuNeutralDisplay::isBoundTo(GpuDevice& device) const noexcept {
    // Owner-only query: a foreign thread must not read device renderer state, and the answer is
    // false rather than racing.
    if (impl_ == nullptr || !impl_->onOwnerThread()) {
        return false;
    }
    const auto deviceState = GpuRendererAccess::state(device);
    return deviceState != nullptr && deviceState == impl_->state;
}

const GpuNeutralDisplayDiagnostic& GpuNeutralDisplay::diagnostic() const noexcept {
    static const GpuNeutralDisplayDiagnostic none{};
    if (impl_ != nullptr) {
        return impl_->jobDiagnostic;
    }
    return none;
}

GpuNeutralDisplayDiagnostic GpuNeutralDisplay::begin(const std::span<const Rgba32f> source,
                                                     const std::uint64_t byteBudget,
                                                     const GpuDisplayPackRequest& pack) {
    if (impl_ == nullptr) {
        return makeDiagnostic(GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
                              "the neutral display pipeline is not initialized");
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return makeDiagnostic(GpuNeutralDisplayDiagnosticCode::WrongThread,
                              "begin must run on the device owner thread");
    }
    if (impl.deviceLost) {
        return makeDiagnostic(GpuNeutralDisplayDiagnosticCode::DeviceLost,
                              "the device was lost; this generation must not be reused");
    }
    // Any live submission owns the command buffer and buffers, independent of the API-facing job
    // state. An unexpected fence result can leave jobState Failure while queueSubmitted stays true;
    // begin must still refuse in that case so it never resets or frees a busy generation.
    if (impl.queueSubmitted || impl.jobState == GpuNeutralDisplayJobState::Pending) {
        return makeDiagnostic(GpuNeutralDisplayDiagnosticCode::Busy,
                              "one job is already in flight");
    }
    if (source.empty()) {
        return makeDiagnostic(GpuNeutralDisplayDiagnosticCode::InvalidArgument,
                              "the source span is empty");
    }
    if (source.size() > static_cast<std::size_t>(UINT32_MAX)) {
        return makeDiagnostic(GpuNeutralDisplayDiagnosticCode::Unsupported,
                              "the pixel count exceeds the shader's 32-bit index");
    }
    const auto pixelCount = static_cast<std::uint32_t>(source.size());
    const std::uint64_t inputBytes = static_cast<std::uint64_t>(pixelCount) * kInputBytesPerPixel;
    const std::uint64_t outputBytes = static_cast<std::uint64_t>(pixelCount) * kOutputBytesPerPixel;
    const std::uint64_t required = neutral_display_detail::gpuBytesForPixels(pixelCount);
    if (inputBytes > impl.budgets.maxInputBytes) {
        return makeDiagnostic(GpuNeutralDisplayDiagnosticCode::OverBudget,
                              "the source exceeds the configured input byte ceiling");
    }
    // The per-call byteBudget is only an upper allowance. Clamp it to the configured hard ceiling
    // rather than rejecting a small, otherwise valid job just because the allowance is generous;
    // the actual required and retained bytes are still bounded by the hard ceiling below.
    const std::uint64_t effectiveBudget =
        byteBudget < impl.budgets.maxOwnedBytes ? byteBudget : impl.budgets.maxOwnedBytes;
    if (required > impl.budgets.maxOwnedBytes || required > effectiveBudget) {
        return makeDiagnostic(GpuNeutralDisplayDiagnosticCode::OverBudget,
                              "the frame exceeds the configured or requested byte budget");
    }
    if (inputBytes > impl.state->maxStorageBufferRange ||
        outputBytes > impl.state->maxStorageBufferRange) {
        return makeDiagnostic(GpuNeutralDisplayDiagnosticCode::Unsupported,
                              "the frame exceeds maxStorageBufferRange");
    }
    const std::uint32_t groupCount = (pixelCount + kWorkgroupSizeX - 1U) / kWorkgroupSizeX;
    if (groupCount > impl.state->maxComputeWorkGroupCountX ||
        impl.state->maxComputeWorkGroupInvocations < kWorkgroupSizeX ||
        impl.state->maxComputeWorkGroupSizeX < kWorkgroupSizeX) {
        return makeDiagnostic(GpuNeutralDisplayDiagnosticCode::Unsupported,
                              "the dispatch exceeds the device's compute workgroup limits");
    }
    if (impl.state->generation != impl.expectedGeneration) {
        impl.deviceLost = true;
        return makeDiagnostic(GpuNeutralDisplayDiagnosticCode::DeviceLost,
                              "the device generation changed; this pipeline must not be reused");
    }

    impl.clearJob();
    if (!impl.ensureCapacity(pixelCount, effectiveBudget)) {
        impl.fail(GpuNeutralDisplayDiagnosticCode::AllocationFailed,
                  "the display buffers could not be sized within budget");
        return impl.jobDiagnostic;
    }
    impl.jobPixelCount = pixelCount;
    impl.jobPack = pack;
    impl.frameByteBudget = effectiveBudget;

    const VkDevice rawDevice = static_cast<VkDevice>(*impl.state->device);
    const auto* dispatcher = impl.state->device.getDispatcher();
    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    if (dispatcher->vkResetFences(rawDevice, 1, &rawFence) != VK_SUCCESS) {
        impl.fail(GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
                  "the display fence could not be reset");
        return impl.jobDiagnostic;
    }
    const VkCommandBuffer rawCommandBuffer = static_cast<VkCommandBuffer>(*impl.commandBuffer);
    if (dispatcher->vkResetCommandBuffer(rawCommandBuffer, 0) != VK_SUCCESS) {
        impl.fail(GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
                  "the display command buffer could not be reset");
        return impl.jobDiagnostic;
    }

    std::memcpy(impl.input.info.pMappedData, source.data(), inputBytes);
    if (vmaFlushAllocation(impl.state->allocator, impl.input.allocation, 0, inputBytes) !=
        VK_SUCCESS) {
        impl.fail(GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
                  "the input buffer flush failed");
        return impl.jobDiagnostic;
    }
    std::memset(impl.status.info.pMappedData, 0, kStatusBytes);
    if (vmaFlushAllocation(impl.state->allocator, impl.status.allocation, 0, kStatusBytes) !=
        VK_SUCCESS) {
        impl.fail(GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
                  "the status buffer flush failed");
        return impl.jobDiagnostic;
    }

    vk::CommandBufferBeginInfo beginInfo{};
    beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    if (dispatcher->vkBeginCommandBuffer(
            rawCommandBuffer, reinterpret_cast<const VkCommandBufferBeginInfo*>(&beginInfo)) !=
        VK_SUCCESS) {
        impl.fail(GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
                  "the display command buffer could not begin");
        return impl.jobDiagnostic;
    }

    impl.updateDescriptors();

    std::array<vk::BufferMemoryBarrier, 3> hostToCompute{};
    const auto setBarrier = [](vk::BufferMemoryBarrier& barrier, const VkBuffer buffer,
                               const std::uint64_t size, const vk::AccessFlags source,
                               const vk::AccessFlags destination) {
        barrier.srcAccessMask = source;
        barrier.dstAccessMask = destination;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer;
        barrier.offset = 0;
        barrier.size = size;
    };
    setBarrier(hostToCompute[0], impl.input.buffer, inputBytes, vk::AccessFlagBits::eHostWrite,
               vk::AccessFlagBits::eShaderRead);
    setBarrier(hostToCompute[1], impl.output.buffer, outputBytes, vk::AccessFlagBits::eHostRead,
               vk::AccessFlagBits::eShaderWrite);
    setBarrier(hostToCompute[2], impl.status.buffer, kStatusBytes, vk::AccessFlagBits::eHostWrite,
               vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);

    impl.commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, *impl.pipeline);
    impl.commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eHost,
                                       vk::PipelineStageFlagBits::eComputeShader, {}, {},
                                       hostToCompute, {});
    impl.commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *impl.pipelineLayout, 0,
                                          {*impl.descriptorSet}, {});
    const auto push = vulkan_detail::neutralDisplayPush(pixelCount, pack);
    impl.commandBuffer.pushConstants(*impl.pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0,
                                     static_cast<std::uint32_t>(sizeof(push)), &push);
    impl.commandBuffer.dispatch(groupCount, 1, 1);

    std::array<vk::BufferMemoryBarrier, 2> computeToHost{};
    setBarrier(computeToHost[0], impl.output.buffer, outputBytes, vk::AccessFlagBits::eShaderWrite,
               vk::AccessFlagBits::eHostRead);
    setBarrier(computeToHost[1], impl.status.buffer, kStatusBytes, vk::AccessFlagBits::eShaderWrite,
               vk::AccessFlagBits::eHostRead);
    impl.commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                       vk::PipelineStageFlagBits::eHost, {}, {}, computeToHost, {});

    if (dispatcher->vkEndCommandBuffer(rawCommandBuffer) != VK_SUCCESS) {
        impl.fail(GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
                  "the display command buffer could not end");
        return impl.jobDiagnostic;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &rawCommandBuffer;
    core::telemetry::addGpuSubmit();
    const VkResult submitResult = dispatcher->vkQueueSubmit(
        static_cast<VkQueue>(*impl.state->computeQueue), 1, &submitInfo, rawFence);
    if (submitResult == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.fail(GpuNeutralDisplayDiagnosticCode::DeviceLost,
                  "the device was lost while submitting the display dispatch");
        return impl.jobDiagnostic;
    }
    if (submitResult != VK_SUCCESS) {
        impl.fail(GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
                  "the display dispatch could not be submitted");
        return impl.jobDiagnostic;
    }

    // From here the queue owns the command buffer and buffers. Retirement is tracked independently
    // of the API-facing jobState.
    impl.queueSubmitted = true;
    impl.jobState = GpuNeutralDisplayJobState::Pending;
    impl.jobDiagnostic = GpuNeutralDisplayDiagnostic{};
    return {};
}

GpuNeutralDisplayPollResult GpuNeutralDisplay::poll() {
    if (impl_ == nullptr) {
        return GpuNeutralDisplayPollResult::Failure;
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        // Fail closed WITHOUT touching owned state: a foreign thread must not mutate a live job.
        // The caller can read the returned Failure; the owner's job is untouched.
        return GpuNeutralDisplayPollResult::Failure;
    }
    if (impl.jobState == GpuNeutralDisplayJobState::Ready) {
        return GpuNeutralDisplayPollResult::Ready;
    }
    const bool pending = impl.jobState == GpuNeutralDisplayJobState::Pending;
    // A live submission can outlive a Failure job state: an unknown fence result leaves
    // queueSubmitted set. Keep querying the fence in that case so a later successful owner poll
    // retires the submission (never republishing a frame), which lets destruction proceed without a
    // drain and lets begin succeed again.
    if (!pending && !impl.queueSubmitted) {
        return GpuNeutralDisplayPollResult::Failure;
    }

    const VkFence rawFence = static_cast<VkFence>(*impl.fence);
    const VkResult status = neutral_display_detail::effectiveFenceStatus(
        impl.state->device.getDispatcher()->vkGetFenceStatus(
            static_cast<VkDevice>(*impl.state->device), rawFence));
    if (status == VK_NOT_READY) {
        return pending ? GpuNeutralDisplayPollResult::Pending
                       : GpuNeutralDisplayPollResult::Failure;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.deviceLost = true;
        impl.queueSubmitted = false; // a lost device's submission is no longer executing
        if (pending) {
            impl.fail(GpuNeutralDisplayDiagnosticCode::DeviceLost,
                      "the device was lost while polling the display dispatch");
        }
        return GpuNeutralDisplayPollResult::Failure;
    }
    if (status != VK_SUCCESS) {
        // Unknown status: the fence is NOT proved signalled, so the submission stays unretired.
        // Do not clear queueSubmitted and do not free anything; a later owner poll or the
        // destructor's bounded drain still covers it. Keep the error unpublished until then.
        if (pending) {
            impl.fail(
                GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
                "the display fence returned an unexpected status; the submission is not retired");
        }
        return GpuNeutralDisplayPollResult::Failure;
    }
    impl.queueSubmitted = false;
    if (!pending) {
        // The submission retired after an already-published Failure. There is nothing to publish;
        // the pipeline is now idle and reusable.
        return GpuNeutralDisplayPollResult::Failure;
    }

    if (impl.discardRequested.load()) {
        impl.clearJob();
        impl.fail(GpuNeutralDisplayDiagnosticCode::Cancelled,
                  "the display job was cancelled before readback");
        return GpuNeutralDisplayPollResult::Failure;
    }

    const std::uint64_t outputBytes =
        static_cast<std::uint64_t>(impl.jobPixelCount) * kOutputBytesPerPixel;
    const VkResult invalidateOutput =
        vmaInvalidateAllocation(impl.state->allocator, impl.output.allocation, 0, outputBytes);
    const VkResult invalidateStatus =
        vmaInvalidateAllocation(impl.state->allocator, impl.status.allocation, 0, kStatusBytes);
    if (invalidateOutput != VK_SUCCESS || invalidateStatus != VK_SUCCESS) {
        // Never read possibly-stale mapped memory after a failed invalidate.
        impl.fail(GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
                  "the readback buffers could not be invalidated; no frame is published");
        return GpuNeutralDisplayPollResult::Failure;
    }
    std::array<std::uint32_t, kGpuDisplayPackStatusWords> words{};
    std::memcpy(words.data(), impl.status.info.pMappedData, kGpuDisplayPackStatusBytes);
    const auto flags = words[kGpuDisplayPackStatusErrors];
    if (flags != 0U) {
        impl.fail(GpuNeutralDisplayDiagnosticCode::ShaderRejected,
                  neutral_display_detail::errorFlagMessage(flags));
        return GpuNeutralDisplayPollResult::Failure;
    }
    // #733: the status block also says whether the frame allows the layout it was packed in.
    // Words packed as 10:10:10:2 from a frame that does not allow it are never handed on.
    const auto report =
        gpuDisplayPackReport(words, impl.jobPack.layout, impl.jobPack.luma.has_value());
    impl.jobReport = report;
    if (!report.wordsValid()) {
        impl.fail(GpuNeutralDisplayDiagnosticCode::LayoutRefused,
                  "the frame holds a pixel that does not allow the 10:10:10:2 layout");
        return GpuNeutralDisplayPollResult::Failure;
    }
    impl.jobState = GpuNeutralDisplayJobState::Ready;
    impl.jobDiagnostic = GpuNeutralDisplayDiagnostic{};
    return GpuNeutralDisplayPollResult::Ready;
}

GpuNeutralDisplayReadback GpuNeutralDisplay::readback() {
    if (impl_ == nullptr) {
        return {std::vector<Rgba8>{},
                makeDiagnostic(GpuNeutralDisplayDiagnosticCode::DeviceUnavailable,
                               "the neutral display pipeline is not initialized")};
    }
    Impl& impl = *impl_;
    if (!impl.onOwnerThread()) {
        return {std::vector<Rgba8>{},
                makeDiagnostic(GpuNeutralDisplayDiagnosticCode::WrongThread,
                               "readback must run on the device owner thread")};
    }
    if (impl.jobState != GpuNeutralDisplayJobState::Ready) {
        return {std::vector<Rgba8>{},
                makeDiagnostic(GpuNeutralDisplayDiagnosticCode::InvalidArgument,
                               "readback requires a completed, uncancelled frame")};
    }

    const auto* words = static_cast<const std::uint32_t*>(impl.output.info.pMappedData);
    try {
        impl.frame.resize(impl.jobPixelCount);
    } catch (const std::exception&) {
        impl.fail(GpuNeutralDisplayDiagnosticCode::AllocationFailed,
                  "the host readback buffer could not be allocated");
        return {std::vector<Rgba8>{}, impl.jobDiagnostic};
    }
    // The cells are the words' bytes in either layout.
    for (std::uint32_t index = 0; index < impl.jobPixelCount; ++index) {
        impl.frame[index] = displayCell(words[index]);
    }
    GpuNeutralDisplayReadback result;
    result.pixels = std::move(impl.frame);
    result.diagnostic = GpuNeutralDisplayDiagnostic{};
    result.pixelLayout = impl.jobPack.layout;
    result.report = impl.jobReport;
    impl.clearJob();
    return result;
}

std::optional<GpuDisplayPackReport> GpuNeutralDisplay::packReport() const noexcept {
    return impl_ != nullptr ? impl_->jobReport : std::nullopt;
}

void GpuNeutralDisplay::cancel() noexcept {
    if (impl_ != nullptr) {
        impl_->discardRequested.store(true);
    }
}

bool GpuNeutralDisplay::teardownDrainIncomplete() noexcept {
    return neutral_display_detail::teardownIncomplete();
}

} // namespace bloom::render
