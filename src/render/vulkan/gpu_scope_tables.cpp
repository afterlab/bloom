#include <bloom/core/telemetry_counters.hpp>
#include <bloom/render/gpu_scope_tables.hpp>

#include "gpu_composite_private.hpp"
#include "gpu_device_private.hpp"
#include "gpu_image_private.hpp"
#include "shaders/scope_tables_spirv.inc"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// ScopeTablesV1 (#724): the scope count tables measured on the device and read back. The lifetime
// rules are GpuProcessReadback's: one bounded process-wide slot, every native resource retained
// until the fence is proven, quarantine instead of destruction when it is not.

namespace bloom::render {
namespace {

using vulkan_detail::DeviceAllocatorState;

constexpr std::uint64_t kScopeTablesDeadlineNanoseconds = 5ULL * 1000ULL * 1000ULL * 1000ULL;
constexpr std::uint32_t kScopeWorkgroupSize = 256;
constexpr std::uint32_t kScopeBindingCount = 3;
// column * width stays below 2^32 in the column pass.
constexpr std::uint32_t kScopeMaxAreaWidth = 1U << 22U;
constexpr std::uint32_t kScopeMaxGroupCount = 65535U;

struct ScopeTablesPush final {
    std::uint32_t pass;
    std::uint32_t scopes;
    std::uint32_t flags;
    std::uint32_t columns;
    std::int32_t areaX;
    std::int32_t areaY;
    std::uint32_t areaWidth;
    std::uint32_t areaHeight;
    float inverseMaximum;
    float lumaRed;
    float lumaGreen;
    float lumaBlue;
    float cbDenominator;
    float crDenominator;
    std::uint32_t histogramOffset;
    std::uint32_t waveformOffset;
    std::uint32_t vectorscopeOffset;
    std::uint32_t chromaticityOffset;
    std::uint32_t groupWidth;
    std::uint32_t detail;
};
static_assert(sizeof(ScopeTablesPush) == 80);

std::atomic<std::uint64_t>& scopeDispatchCounter() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

struct ScopeBuffer final {
    ScopeBuffer() = default;
    ScopeBuffer(const ScopeBuffer&) = delete;
    ScopeBuffer& operator=(const ScopeBuffer&) = delete;
    ScopeBuffer(ScopeBuffer&& other) noexcept { *this = std::move(other); }
    ScopeBuffer& operator=(ScopeBuffer&& other) noexcept {
        if (this != &other) {
            release();
            state = other.state;
            buffer = other.buffer;
            allocation = other.allocation;
            armed = other.armed;
            other.state = nullptr;
            other.buffer = VK_NULL_HANDLE;
            other.allocation = VK_NULL_HANDLE;
            other.armed = false;
        }
        return *this;
    }
    ~ScopeBuffer() { release(); }

    void release() noexcept {
        if (armed && state != nullptr) {
            vmaDestroyBuffer(state->allocator, buffer, allocation);
        }
        state = nullptr;
        buffer = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        armed = false;
    }

    DeviceAllocatorState* state = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    bool armed = false;
};

// The pipeline and its one descriptor set, built once per device generation. A submission keeps a
// reference, so they outlive it even when the measuring object is destroyed first.
struct ScopePipelineSet final {
    std::shared_ptr<DeviceAllocatorState> state;
    CompositePipeline pipeline;
    vk::raii::DescriptorPool descriptorPool{nullptr};
    vk::raii::DescriptorSet descriptorSet{nullptr};

    ~ScopePipelineSet() {
        descriptorSet = vk::raii::DescriptorSet{nullptr};
        descriptorPool = vk::raii::DescriptorPool{nullptr};
        pipeline = CompositePipeline{};
    }
};

enum class ScopeSlotState : std::uint8_t { Free, Reserved, Quarantined };

// The bounded process-wide slot, as GpuProcessReadback's: Reserved while a live Impl owns the
// submission, Quarantined when a possibly in-flight submission was handed over with everything it
// references. Only the owner thread that proves the fence retired may free it.
struct ScopeSlot final {
    ScopeSlotState slotState = ScopeSlotState::Free;
    std::thread::id ownerThread{};
    std::uint64_t token = 0;

    std::shared_ptr<DeviceAllocatorState> state;
    std::shared_ptr<ScopePipelineSet> pipelines;
    std::shared_ptr<const GpuImage> signal;
    std::shared_ptr<const GpuImage> xyz;
    ScopeBuffer tables;
    ScopeBuffer staging;
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer buffer{nullptr};
    vk::raii::Fence fence{nullptr};
};

ScopeSlot& scopeSlot() {
    static auto* const slot = new ScopeSlot();
    return *slot;
}

std::mutex& scopeSlotMutex() {
    static auto* const mutex = new std::mutex();
    return *mutex;
}

void freeQuarantinedSlotLocked(ScopeSlot& slot) noexcept {
    slot.tables.release();
    slot.staging.release();
    slot.fence = vk::raii::Fence{nullptr};
    slot.buffer = vk::raii::CommandBuffer{nullptr};
    slot.pool = vk::raii::CommandPool{nullptr};
    slot.signal.reset();
    slot.xyz.reset();
    slot.pipelines.reset();
    slot.state.reset();
    slot.slotState = ScopeSlotState::Free;
    slot.ownerThread = std::thread::id{};
}

[[nodiscard]] bool isSlotOwnerLocked(const ScopeSlot& slot) noexcept {
    return slot.slotState != ScopeSlotState::Free && slot.ownerThread == std::this_thread::get_id();
}

[[nodiscard]] bool retireQuarantinedSlotLocked(ScopeSlot& slot) noexcept {
    if (slot.slotState != ScopeSlotState::Quarantined || slot.state == nullptr ||
        static_cast<VkFence>(*slot.fence) == VK_NULL_HANDLE) {
        return false;
    }
    const VkFence rawFence = static_cast<VkFence>(*slot.fence);
    const VkResult status = slot.state->device.getDispatcher()->vkGetFenceStatus(
        static_cast<VkDevice>(*slot.state->device), rawFence);
    if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) {
        return false;
    }
    freeQuarantinedSlotLocked(slot);
    return true;
}

struct ScopeDispatch final {
    std::uint32_t groupsX = 0;
    std::uint32_t groupsY = 0;
};

// Flattens `invocations` into a grid of 256-wide workgroups within the device's limits.
[[nodiscard]] bool planScopeDispatch(const std::uint64_t invocations,
                                     const std::uint32_t deviceMaxX, ScopeDispatch& out) noexcept {
    if (invocations == 0) {
        return false;
    }
    const std::uint64_t groups = (invocations + kScopeWorkgroupSize - 1U) / kScopeWorkgroupSize;
    const std::uint64_t maxX =
        deviceMaxX == 0 ? kScopeMaxGroupCount
                        : (deviceMaxX < kScopeMaxGroupCount ? deviceMaxX : kScopeMaxGroupCount);
    const std::uint64_t groupsX = groups < maxX ? groups : maxX;
    const std::uint64_t groupsY = (groups + groupsX - 1U) / groupsX;
    if (groupsY > kScopeMaxGroupCount) {
        return false;
    }
    out.groupsX = static_cast<std::uint32_t>(groupsX);
    out.groupsY = static_cast<std::uint32_t>(groupsY);
    return true;
}

[[nodiscard]] bool createScopeBuffer(DeviceAllocatorState& state, const std::uint64_t bytes,
                                     const VkBufferUsageFlags usage, const bool hostVisible,
                                     ScopeBuffer& out, std::uint64_t& actualBytes) noexcept {
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage =
        hostVisible ? VMA_MEMORY_USAGE_AUTO : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (hostVisible) {
        allocationInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VmaAllocationInfo info{};
    out.state = &state;
    if (vmaCreateBuffer(state.allocator, &bufferInfo, &allocationInfo, &out.buffer, &out.allocation,
                        &info) != VK_SUCCESS) {
        out.state = nullptr;
        return false;
    }
    out.armed = true;
    actualBytes = static_cast<std::uint64_t>(info.size);
    return true;
}

[[nodiscard]] std::shared_ptr<ScopePipelineSet>
createScopePipelineSet(const std::shared_ptr<DeviceAllocatorState>& state, std::string& reason) {
    auto set = std::make_shared<ScopePipelineSet>();
    set->state = state;
    constexpr bool kBindings[kScopeBindingCount] = {true, true, false};
    if (!createCompositePipeline(
            *state, vulkan_detail::kScopeTablesSpirvCode, vulkan_detail::kScopeTablesSpirvByteCount,
            kBindings, kScopeBindingCount, sizeof(ScopeTablesPush), reason, set->pipeline)) {
        return nullptr;
    }
    const VkDevice device = static_cast<VkDevice>(*state->device);
    const auto* dispatcher = state->device.getDispatcher();
    const std::array poolSizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    VkDescriptorPool rawPool = VK_NULL_HANDLE;
    if (dispatcher->vkCreateDescriptorPool(device, &poolInfo, nullptr, &rawPool) != VK_SUCCESS) {
        reason = "the scope-tables descriptor pool could not be created";
        return nullptr;
    }
    set->descriptorPool = vk::raii::DescriptorPool(state->device, rawPool);
    const VkDescriptorSetLayout layout =
        static_cast<VkDescriptorSetLayout>(*set->pipeline.descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = rawPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &layout;
    VkDescriptorSet rawSet = VK_NULL_HANDLE;
    if (dispatcher->vkAllocateDescriptorSets(device, &allocateInfo, &rawSet) != VK_SUCCESS) {
        reason = "the scope-tables descriptor set could not be allocated";
        return nullptr;
    }
    set->descriptorSet = vk::raii::DescriptorSet(state->device, rawSet, *set->descriptorPool);
    return set;
}

} // namespace

struct GpuScopeTables::Impl final {
    GpuScopeTablesState state = GpuScopeTablesState::Idle;
    GpuScopeTablesDiagnostic diagnostic;

    std::shared_ptr<ScopePipelineSet> pipelines;
    std::shared_ptr<DeviceAllocatorState> deviceState;
    std::shared_ptr<const GpuImage> signal;
    std::shared_ptr<const GpuImage> xyz;
    ScopeBuffer tables;
    ScopeBuffer staging;
    vk::raii::CommandPool pool{nullptr};
    vk::raii::CommandBuffer buffer{nullptr};
    vk::raii::Fence fence{nullptr};
    ScopeTableLayout layout;
    std::uint64_t bytes = 0;
    std::vector<std::uint32_t> words;
    bool cancelRequested = false;
    bool submitted = false;
    std::uint64_t slotToken = 0;
    std::thread::id ownerThread{};
    std::chrono::steady_clock::time_point submittedAt{};

    void releaseRetired() noexcept {
        tables.release();
        staging.release();
        fence = vk::raii::Fence{nullptr};
        buffer = vk::raii::CommandBuffer{nullptr};
        pool = vk::raii::CommandPool{nullptr};
        signal.reset();
        xyz.reset();
        releaseClaim();
        deviceState.reset();
        submitted = false;
    }

    void releaseClaim() noexcept {
        if (slotToken == 0) {
            return;
        }
        std::lock_guard lock(scopeSlotMutex());
        ScopeSlot& slot = scopeSlot();
        if (isSlotOwnerLocked(slot) && slot.token == slotToken) {
            slot.slotState = ScopeSlotState::Free;
            slot.ownerThread = std::thread::id{};
        }
        slotToken = 0;
    }

    // Hands a possibly in-flight submission, with everything it references, to the process slot.
    // False when the claim is not this Impl's: the resources then stay here, never destroyed.
    bool quarantine() noexcept {
        if (!submitted) {
            releaseRetired();
            return true;
        }
        std::lock_guard lock(scopeSlotMutex());
        ScopeSlot& slot = scopeSlot();
        if (!isSlotOwnerLocked(slot) || slot.token != slotToken) {
            return false;
        }
        slot.state = std::move(deviceState);
        slot.pipelines = pipelines;
        slot.signal = std::move(signal);
        slot.xyz = std::move(xyz);
        slot.tables = std::move(tables);
        slot.staging = std::move(staging);
        slot.pool = std::move(pool);
        slot.buffer = std::move(buffer);
        slot.fence = std::move(fence);
        slot.slotState = ScopeSlotState::Quarantined;
        deviceState.reset();
        signal.reset();
        xyz.reset();
        pool = vk::raii::CommandPool{nullptr};
        buffer = vk::raii::CommandBuffer{nullptr};
        fence = vk::raii::Fence{nullptr};
        slotToken = 0;
        submitted = false;
        return true;
    }

    void setFailure(const GpuScopeTablesCode code, const std::string_view message) noexcept {
        state = GpuScopeTablesState::Failure;
        diagnostic.code = code;
        try {
            diagnostic.message.assign(message);
        } catch (...) {
            diagnostic.message.clear();
        }
    }

    // A failure before submission: nothing is in flight, so the job's resources go at once.
    void fail(const GpuScopeTablesCode code, const std::string_view message) noexcept {
        setFailure(code, message);
        if (!submitted) {
            tables.release();
            staging.release();
            fence = vk::raii::Fence{nullptr};
            buffer = vk::raii::CommandBuffer{nullptr};
            pool = vk::raii::CommandPool{nullptr};
            releaseClaim();
        }
    }
};

GpuScopeTables::GpuScopeTables() : impl_(std::make_unique<Impl>()) {}

GpuScopeTables::~GpuScopeTables() {
    if (impl_ == nullptr) {
        return;
    }
    // Native objects are owner-thread only. A foreign-thread destruction retains the Impl (a
    // bounded, intentional leak) instead of touching the device from the wrong thread.
    const bool ownsNative = impl_->submitted || impl_->pipelines != nullptr;
    if (ownsNative && impl_->ownerThread != std::this_thread::get_id()) {
        [[maybe_unused]] const auto* const retained = impl_.release();
        return;
    }
    if (impl_->submitted) {
        cancel();
        const auto deadline =
            impl_->submittedAt + std::chrono::nanoseconds(kScopeTablesDeadlineNanoseconds);
        while (impl_->submitted && impl_->state == GpuScopeTablesState::Pending &&
               std::chrono::steady_clock::now() < deadline) {
            static_cast<void>(poll());
            if (impl_->submitted && impl_->state == GpuScopeTablesState::Pending) {
                std::this_thread::yield();
            }
        }
        if (impl_->submitted && !impl_->quarantine()) {
            [[maybe_unused]] const auto* const retained = impl_.release();
        }
    }
}

GpuScopeTablesState GpuScopeTables::state() const noexcept {
    return impl_ == nullptr ? GpuScopeTablesState::Idle : impl_->state;
}

const GpuScopeTablesDiagnostic& GpuScopeTables::diagnostic() const noexcept {
    static const GpuScopeTablesDiagnostic empty{};
    return impl_ == nullptr ? empty : impl_->diagnostic;
}

bool GpuScopeTables::hasUnretiredSubmission() noexcept {
    if (impl_ != nullptr && impl_->submitted) {
        return true;
    }
    std::lock_guard lock(scopeSlotMutex());
    ScopeSlot& slot = scopeSlot();
    if (slot.slotState != ScopeSlotState::Quarantined) {
        return false;
    }
    if (slot.ownerThread == std::this_thread::get_id()) {
        static_cast<void>(retireQuarantinedSlotLocked(slot));
    }
    return slot.slotState == ScopeSlotState::Quarantined;
}

std::uint64_t GpuScopeTables::nativeDispatchCount() noexcept {
    return scopeDispatchCounter().load();
}

bool GpuScopeTables::begin(const GpuScopeTablesRequest& request,
                           const std::uint64_t byteBudget) noexcept {
    try {
        if (impl_ == nullptr || impl_->state == GpuScopeTablesState::Pending ||
            impl_->state == GpuScopeTablesState::Ready || impl_->submitted) {
            return false;
        }
        Impl& impl = *impl_;
        impl.state = GpuScopeTablesState::Idle;
        impl.diagnostic = {};

        const std::uint32_t scopes = request.scopes & kScopeTableAll;
        if (request.signal == nullptr || gpuImageImpl(*request.signal) == nullptr ||
            gpuImageImpl(*request.signal)->image == VK_NULL_HANDLE) {
            impl.setFailure(GpuScopeTablesCode::DeviceUnavailable, "no resident signal picture");
            return false;
        }
        const GpuImageImpl* const signal = gpuImageImpl(*request.signal);
        if (!signal->onOwnerThread()) {
            impl.setFailure(GpuScopeTablesCode::WrongThread,
                            "the scope measure must run on the device owner thread");
            return false;
        }
        if (signal->deviceLost) {
            impl.setFailure(GpuScopeTablesCode::DeviceLost, "the device was lost");
            return false;
        }
        if (signal->submissionUnretired) {
            impl.setFailure(GpuScopeTablesCode::DeviceUnavailable,
                            "a submission that reads the signal picture is not retired");
            return false;
        }
        const GpuImageImpl* xyz = signal;
        if ((scopes & kScopeTableChromaticity) != 0U) {
            xyz = request.xyz == nullptr ? nullptr : gpuImageImpl(*request.xyz);
            if (xyz == nullptr || xyz->image == VK_NULL_HANDLE || xyz->state != signal->state ||
                xyz->width != signal->width || xyz->height != signal->height) {
                impl.setFailure(GpuScopeTablesCode::InvalidArgument,
                                "the Chromaticity table needs an XYZ picture of the signal's "
                                "extent on the same device");
                return false;
            }
            if (xyz->deviceLost || xyz->submissionUnretired) {
                impl.setFailure(GpuScopeTablesCode::DeviceUnavailable,
                                "the XYZ picture is not available");
                return false;
            }
        }
        const std::uint64_t pixels =
            static_cast<std::uint64_t>(request.areaWidth) * request.areaHeight;
        if (scopes != request.scopes || request.areaWidth == 0 || request.areaHeight == 0 ||
            request.areaWidth > kScopeMaxAreaWidth || !validScopeDetail(request.detail) ||
            pixels > std::numeric_limits<std::uint32_t>::max()) {
            impl.setFailure(GpuScopeTablesCode::InvalidArgument,
                            "the scope measure's scopes, detail or area are not valid");
            return false;
        }

        auto state = signal->state;
        const ScopeTableLayout layout = scopeTableLayout(scopes, request.areaWidth, request.detail);
        const std::uint64_t bytes = static_cast<std::uint64_t>(layout.wordCount) * 4U;
        if (bytes > byteBudget || bytes > state->maxStorageBufferRange) {
            impl.setFailure(GpuScopeTablesCode::OverBudget,
                            "the scope tables exceed the byte budget or the device's storage "
                            "buffer range");
            return false;
        }
        ScopeDispatch pixelDispatch{};
        ScopeDispatch columnDispatch{};
        if (!planScopeDispatch(pixels, state->maxComputeWorkGroupCountX, pixelDispatch) ||
            (layout.columns != 0 &&
             !planScopeDispatch(layout.columns, state->maxComputeWorkGroupCountX,
                                columnDispatch))) {
            impl.setFailure(GpuScopeTablesCode::OverBudget,
                            "the scope measure exceeds the device's workgroup grid");
            return false;
        }

        const std::thread::id self = std::this_thread::get_id();
        {
            std::lock_guard lock(scopeSlotMutex());
            ScopeSlot& slot = scopeSlot();
            if (slot.slotState == ScopeSlotState::Reserved) {
                impl.setFailure(GpuScopeTablesCode::Busy, "another scope measure is in flight");
                return false;
            }
            if (slot.slotState == ScopeSlotState::Quarantined &&
                (slot.ownerThread != self || !retireQuarantinedSlotLocked(slot))) {
                impl.setFailure(GpuScopeTablesCode::Busy,
                                "an earlier scope measure is not retired");
                return false;
            }
            slot.slotState = ScopeSlotState::Reserved;
            slot.ownerThread = self;
            ++slot.token;
            if (slot.token == 0) {
                ++slot.token;
            }
            impl.slotToken = slot.token;
        }
        impl.ownerThread = self;

        if (impl.pipelines == nullptr || impl.pipelines->state != state) {
            std::string reason;
            auto created = createScopePipelineSet(state, reason);
            if (created == nullptr) {
                impl.fail(GpuScopeTablesCode::ShaderRejected, reason);
                return false;
            }
            impl.pipelines = std::move(created);
        }

        const VkDevice device = static_cast<VkDevice>(*state->device);
        const auto* dispatcher = state->device.getDispatcher();

        std::uint64_t tablesActual = 0;
        std::uint64_t stagingActual = 0;
        if (!createScopeBuffer(*state, bytes,
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               false, impl.tables, tablesActual) ||
            !createScopeBuffer(*state, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, impl.staging,
                               stagingActual)) {
            impl.fail(GpuScopeTablesCode::MeasureFailed,
                      "the scope table buffers could not be created");
            return false;
        }
        // The submission holds both buffers and, once retired, the host copy.
        if (tablesActual > byteBudget || stagingActual > byteBudget - tablesActual ||
            bytes > byteBudget - tablesActual - stagingActual) {
            impl.fail(GpuScopeTablesCode::OverBudget,
                      "the scope table buffers and their host copy exceed the byte budget");
            return false;
        }

        {
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = state->computeQueueFamily;
            VkCommandPool raw = VK_NULL_HANDLE;
            if (dispatcher->vkCreateCommandPool(device, &poolInfo, nullptr, &raw) != VK_SUCCESS) {
                impl.fail(GpuScopeTablesCode::MeasureFailed,
                          "the scope measure command pool could not be created");
                return false;
            }
            impl.pool = vk::raii::CommandPool(state->device, raw);
        }
        {
            VkCommandBufferAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocateInfo.commandPool = *impl.pool;
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = 1;
            VkCommandBuffer raw = VK_NULL_HANDLE;
            if (dispatcher->vkAllocateCommandBuffers(device, &allocateInfo, &raw) != VK_SUCCESS) {
                impl.fail(GpuScopeTablesCode::MeasureFailed,
                          "the scope measure command buffer could not be allocated");
                return false;
            }
            impl.buffer = vk::raii::CommandBuffer(state->device, raw, *impl.pool);
        }
        {
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            VkFence raw = VK_NULL_HANDLE;
            if (dispatcher->vkCreateFence(device, &fenceInfo, nullptr, &raw) != VK_SUCCESS) {
                impl.fail(GpuScopeTablesCode::MeasureFailed,
                          "the scope measure fence could not be created");
                return false;
            }
            impl.fence = vk::raii::Fence(state->device, raw);
        }

        const VkDescriptorSet descriptorSet =
            static_cast<VkDescriptorSet>(*impl.pipelines->descriptorSet);
        VkDescriptorImageInfo signalInfo{};
        signalInfo.imageView = signal->view;
        signalInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo xyzInfo{};
        xyzInfo.imageView = xyz->view;
        xyzInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorBufferInfo tablesInfo{};
        tablesInfo.buffer = impl.tables.buffer;
        tablesInfo.range = bytes;
        std::array<VkWriteDescriptorSet, kScopeBindingCount> writes{};
        for (std::uint32_t binding = 0; binding < kScopeBindingCount; ++binding) {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = descriptorSet;
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &signalInfo;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &xyzInfo;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &tablesInfo;
        dispatcher->vkUpdateDescriptorSets(device, kScopeBindingCount, writes.data(), 0, nullptr);

        const VkCommandBuffer commands = static_cast<VkCommandBuffer>(*impl.buffer);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dispatcher->vkBeginCommandBuffer(commands, &beginInfo) != VK_SUCCESS) {
            impl.fail(GpuScopeTablesCode::MeasureFailed,
                      "the scope measure command buffer could not begin");
            return false;
        }

        dispatcher->vkCmdFillBuffer(commands, impl.tables.buffer, 0, bytes, 0);
        VkBufferMemoryBarrier tableBarrier{};
        tableBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        tableBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        tableBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        tableBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        tableBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        tableBarrier.buffer = impl.tables.buffer;
        tableBarrier.size = bytes;
        dispatcher->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                         &tableBarrier, 0, nullptr);

        const VkPipelineLayout pipelineLayout =
            static_cast<VkPipelineLayout>(*impl.pipelines->pipeline.pipelineLayout);
        dispatcher->vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE,
                                      static_cast<VkPipeline>(*impl.pipelines->pipeline.pipeline));
        dispatcher->vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE,
                                            pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        ScopeTablesPush push{};
        push.pass = 0;
        push.scopes = scopes;
        push.flags = request.ignoreTransparent ? 1U : 0U;
        push.columns = layout.columns;
        push.areaX = request.areaX;
        push.areaY = request.areaY;
        push.areaWidth = request.areaWidth;
        push.areaHeight = request.areaHeight;
        push.inverseMaximum = request.inverseAxisMaximum;
        push.lumaRed = request.lumaRed;
        push.lumaGreen = request.lumaGreen;
        push.lumaBlue = request.lumaBlue;
        push.cbDenominator = request.cbDenominator;
        push.crDenominator = request.crDenominator;
        push.histogramOffset = layout.histogramOffset;
        push.waveformOffset = layout.waveformOffset;
        push.vectorscopeOffset = layout.vectorscopeOffset;
        push.chromaticityOffset = layout.chromaticityOffset;
        push.groupWidth = pixelDispatch.groupsX * kScopeWorkgroupSize;
        push.detail = layout.detail;
        dispatcher->vkCmdPushConstants(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                       sizeof(ScopeTablesPush), &push);
        dispatcher->vkCmdDispatch(commands, pixelDispatch.groupsX, pixelDispatch.groupsY, 1);
        std::uint64_t dispatches = 1;

        if (layout.columns != 0) {
            VkBufferMemoryBarrier betweenPasses = tableBarrier;
            betweenPasses.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            dispatcher->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                                             &betweenPasses, 0, nullptr);
            push.pass = 1;
            push.groupWidth = columnDispatch.groupsX * kScopeWorkgroupSize;
            dispatcher->vkCmdPushConstants(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                           sizeof(ScopeTablesPush), &push);
            dispatcher->vkCmdDispatch(commands, columnDispatch.groupsX, columnDispatch.groupsY, 1);
            ++dispatches;
        }

        VkBufferMemoryBarrier toCopy = tableBarrier;
        toCopy.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        dispatcher->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &toCopy,
                                         0, nullptr);
        VkBufferCopy copy{};
        copy.size = bytes;
        dispatcher->vkCmdCopyBuffer(commands, impl.tables.buffer, impl.staging.buffer, 1, &copy);
        VkBufferMemoryBarrier toHost = tableBarrier;
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        toHost.buffer = impl.staging.buffer;
        dispatcher->vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0,
                                         nullptr);

        if (dispatcher->vkEndCommandBuffer(commands) != VK_SUCCESS) {
            impl.fail(GpuScopeTablesCode::MeasureFailed,
                      "the scope measure command buffer could not end");
            return false;
        }

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        core::telemetry::addGpuSubmit();
        core::telemetry::addGpuReadbackBytes(bytes);
        const VkResult submitted =
            dispatcher->vkQueueSubmit(static_cast<VkQueue>(*state->computeQueue), 1, &submit,
                                      static_cast<VkFence>(*impl.fence));
        if (submitted == VK_ERROR_DEVICE_LOST) {
            impl.fail(GpuScopeTablesCode::DeviceLost,
                      "the device was lost during the scope measure submit");
            return false;
        }
        if (submitted != VK_SUCCESS) {
            impl.fail(GpuScopeTablesCode::MeasureFailed, "the scope measure submission failed");
            return false;
        }
        scopeDispatchCounter().fetch_add(dispatches);

        impl.deviceState = std::move(state);
        impl.signal = request.signal;
        impl.xyz = request.xyz;
        impl.layout = layout;
        impl.bytes = bytes;
        impl.cancelRequested = false;
        impl.submitted = true;
        impl.submittedAt = std::chrono::steady_clock::now();
        impl.state = GpuScopeTablesState::Pending;
        return true;
    } catch (const std::bad_alloc&) {
        impl_->fail(GpuScopeTablesCode::MeasureFailed,
                    "the scope measure state could not be allocated");
        return false;
    } catch (...) {
        impl_->fail(GpuScopeTablesCode::MeasureFailed, "the scope measure submission failed");
        return false;
    }
}

GpuScopeTablesState GpuScopeTables::poll() noexcept {
    if (impl_ == nullptr || impl_->state != GpuScopeTablesState::Pending) {
        return impl_ == nullptr ? GpuScopeTablesState::Idle : impl_->state;
    }
    Impl& impl = *impl_;
    // A wrong-thread poll changes neither the job nor who retires it.
    if (impl.ownerThread != std::this_thread::get_id()) {
        impl.diagnostic = {GpuScopeTablesCode::WrongThread,
                           "the scope measure poll must run on the device owner thread"};
        return GpuScopeTablesState::Pending;
    }
    if (impl.deviceState == nullptr) {
        impl.fail(GpuScopeTablesCode::DeviceUnavailable, "the scope measure lost its device");
        return impl.state;
    }
    const auto* dispatcher = impl.deviceState->device.getDispatcher();
    const VkResult status = dispatcher->vkGetFenceStatus(
        static_cast<VkDevice>(*impl.deviceState->device), static_cast<VkFence>(*impl.fence));
    if (status == VK_NOT_READY) {
        if (std::chrono::steady_clock::now() >=
            impl.submittedAt + std::chrono::nanoseconds(kScopeTablesDeadlineNanoseconds)) {
            const bool cancelled = impl.cancelRequested;
            static_cast<void>(impl.quarantine());
            impl.setFailure(cancelled ? GpuScopeTablesCode::Cancelled
                                      : GpuScopeTablesCode::MeasureFailed,
                            "the scope measure did not retire within the deadline; the "
                            "submission is retained");
        }
        return impl.state;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
        impl.releaseRetired();
        impl.setFailure(GpuScopeTablesCode::DeviceLost,
                        "the device was lost during the scope measure");
        return impl.state;
    }
    if (status != VK_SUCCESS) {
        static_cast<void>(impl.quarantine());
        impl.setFailure(GpuScopeTablesCode::MeasureFailed,
                        "the scope measure's completion is unknown; the submission is retained");
        return impl.state;
    }

    const bool cancelled = impl.cancelRequested;
    if (!cancelled) {
        VmaAllocationInfo info{};
        if (vmaInvalidateAllocation(impl.deviceState->allocator, impl.staging.allocation, 0,
                                    impl.bytes) == VK_SUCCESS) {
            vmaGetAllocationInfo(impl.deviceState->allocator, impl.staging.allocation, &info);
        }
        if (info.pMappedData == nullptr) {
            impl.releaseRetired();
            impl.setFailure(GpuScopeTablesCode::MeasureFailed,
                            "the scope tables could not be read from the device");
            return impl.state;
        }
        try {
            impl.words.assign(static_cast<std::size_t>(impl.bytes / 4U), 0U);
            std::memcpy(impl.words.data(), info.pMappedData, static_cast<std::size_t>(impl.bytes));
        } catch (const std::bad_alloc&) {
            impl.releaseRetired();
            impl.setFailure(GpuScopeTablesCode::MeasureFailed,
                            "the scope tables' host copy could not be allocated");
            return impl.state;
        }
    }
    impl.releaseRetired();
    if (cancelled) {
        impl.setFailure(GpuScopeTablesCode::Cancelled,
                        "the scope measure was cancelled; no table was published");
    } else {
        impl.state = GpuScopeTablesState::Ready;
        impl.diagnostic = {};
    }
    return impl.state;
}

GpuScopeTablesResult GpuScopeTables::take() noexcept {
    if (impl_ == nullptr || impl_->state != GpuScopeTablesState::Ready) {
        return {};
    }
    GpuScopeTablesResult result;
    result.layout = impl_->layout;
    result.words = std::move(impl_->words);
    impl_->words.clear();
    impl_->state = GpuScopeTablesState::Idle;
    impl_->diagnostic = {};
    return result;
}

void GpuScopeTables::cancel() noexcept {
    if (impl_ != nullptr && impl_->state == GpuScopeTablesState::Pending) {
        impl_->cancelRequested = true;
    }
}

} // namespace bloom::render
