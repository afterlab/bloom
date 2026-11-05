#include "gpu_composite_offsets_private.hpp"
#include "gpu_blend_plan_private.hpp"
#include "gpu_ocio_program_dispatch_plan.hpp"
#include "gpu_operation_batch_private.hpp"
#include "gpu_translation_axis_private.hpp"
#include <bloom/render/processing_host_storage.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace bloom::render::operation_batch_detail {
namespace {
template <typename T> void pack(Job& job, const T& push) noexcept {
    static_assert(sizeof(T) <= sizeof(job.push));
    std::memcpy(job.push.data(), &push, sizeof(T));
    job.pushBytes = sizeof(T);
}
bool finite(const GpuAffineMatrix& matrix) noexcept {
    return std::isfinite(matrix.a) && std::isfinite(matrix.b) && std::isfinite(matrix.c) &&
           std::isfinite(matrix.d) && std::isfinite(matrix.tx) && std::isfinite(matrix.ty);
}
bool opacityValid(const float opacity) noexcept {
    return std::isfinite(opacity) && opacity >= 0 && opacity <= 1;
}
} // namespace
bool State::initialize() {
    if (initialized)
        return true;
    if (std::exchange(initializationAttempted, true))
        return false;
    if (control->maxComputeWorkGroupInvocations < kCompositeWorkgroupSizeX ||
        control->maxComputeWorkGroupSizeX < kCompositeWorkgroupSizeX) {
        fail(GpuOperationBatchCode::Unsupported, "batch compute workgroup size is unsupported");
        return false;
    }
    if (!control->operationBatchPipelines) {
        constexpr bool solidBindings[] = {true};
        constexpr bool gradientBindings[] = {true, false};
        constexpr bool translationBindings[] = {true, true, false, false, false};
        constexpr bool affineBindings[] = {true, true, false, false};
        constexpr bool overBindings[] = {true, true, true, false};
        constexpr bool blendBindings[kBlendBindingCount] = {true, true, true, false};
        constexpr bool combineBindings[kImageCombineBindingCount] = {true, true, true, false, true};
        auto created = std::make_shared<Pipelines>();
        auto& base = created->base;
        // GpuBlend's Auto policy: the general modes use the exact Float64 companion when the
        // device enabled shaderFloat64, and the portable compensated-Float32 kernel otherwise.
        created->generalUsesF64 = control->shaderFloat64;
        std::string reason;
        if (!createCompositePipeline(*control, vulkan_detail::kSolidSpirvCode,
                                     vulkan_detail::kSolidSpirvByteCount, solidBindings, 1, 24,
                                     reason, base[0]) ||
            !createCompositePipeline(*control, vulkan_detail::kTranslationOpacitySpirvCode,
                                     vulkan_detail::kTranslationOpacitySpirvByteCount,
                                     translationBindings, 5, 20, reason, base[1]) ||
            !createCompositePipeline(*control, vulkan_detail::kAffineBilinearSpirvCode,
                                     vulkan_detail::kAffineBilinearSpirvByteCount, affineBindings,
                                     4, kAffinePushBytes, reason, base[2]) ||
            !createCompositePipeline(*control, vulkan_detail::kSourceOverSpirvCode,
                                     vulkan_detail::kSourceOverSpirvByteCount, overBindings, 4, 24,
                                     reason, base[3]) ||
            !createCompositePipeline(*control, vulkan_detail::kGradientSpirvCode,
                                     vulkan_detail::kGradientSpirvByteCount, gradientBindings, 2,
                                     80, reason, base[4]) ||
            !createCompositePipeline(*control, vulkan_detail::kBlendSpirvCode,
                                     vulkan_detail::kBlendSpirvByteCount, blendBindings,
                                     kBlendBindingCount, kBlendPushBytes, reason,
                                     created->blend) ||
            !createCompositePipeline(*control, vulkan_detail::kBlendPortableSpirvCode,
                                     vulkan_detail::kBlendPortableSpirvByteCount, blendBindings,
                                     kBlendBindingCount, kBlendPushBytes, reason,
                                     created->blendPortable) ||
            !createCompositePipeline(*control, vulkan_detail::kImageCombineSpirvCode,
                                     vulkan_detail::kImageCombineSpirvByteCount, combineBindings,
                                     kImageCombineBindingCount, kImageCombinePushBytes, reason,
                                     created->imageCombine) ||
            (created->generalUsesF64 &&
             !createCompositePipeline(*control, vulkan_detail::kBlendF64SpirvCode,
                                      vulkan_detail::kBlendF64SpirvByteCount, blendBindings,
                                      kBlendBindingCount, kBlendPushBytes, reason,
                                      created->blendF64)))
            return false;
        control->operationBatchPipelines = std::move(created);
    }
    pipelines = control->operationBatchPipelines;
    const VkDevice device = static_cast<VkDevice>(*control->device);
    const auto* api = control->device.getDispatcher();
    const std::array poolSizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * kGpuOperationBatchCapacity},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kGpuOperationBatchCapacity}};
    const VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              nullptr,
                                              0,
                                              kGpuOperationBatchCapacity,
                                              static_cast<std::uint32_t>(poolSizes.size()),
                                              poolSizes.data()};
    const VkCommandPoolCreateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                              VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                              control->computeQueueFamily};
    const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
    if (api->vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptors) != VK_SUCCESS ||
        api->vkCreateCommandPool(device, &commandInfo, nullptr, &commandPool) != VK_SUCCESS ||
        api->vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
        return false;
    const VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                   nullptr, commandPool,
                                                   VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    initialized = api->vkAllocateCommandBuffers(device, &allocateInfo, &command) == VK_SUCCESS;
    return initialized;
}

bool State::prepare(const std::span<const GpuBatchOperation> operations,
                    const GpuOperationBatchBudgets& budgets, const std::uint64_t byteBudget) {
    if (operations.empty() || operations.size() > kGpuOperationBatchCapacity ||
        (operations.size() == 1 && !std::holds_alternative<GpuBatchGradient>(operations.front()))) {
        fail(GpuOperationBatchCode::InvalidRequest,
             "native batch requires two to sixty-four operations, or one gradient");
        return false;
    }
    const auto resolve = [this](const GpuBatchImageRef& ref, const std::uint32_t index,
                                std::shared_ptr<const GpuImage>& pin) -> const GpuImageImpl* {
        if (ref.external) {
            if (ref.operation != kGpuBatchExternalImage || !ref.external->isValid())
                return nullptr;
            pin = ref.external;
            const auto* image = gpuImageImpl(*pin);
            return compositeImageBelongsTo(image, control) &&
                           image->generation == expectedGeneration &&
                           !image->submissionUnretired.load(std::memory_order_acquire)
                       ? image
                       : nullptr;
        }
        return ref.operation < index ? jobs[ref.operation].image.get() : nullptr;
    };
    for (std::uint32_t index = 0; index < operations.size(); ++index) {
        auto& job = jobs[index];
        count = index + 1; // Cleanup includes a partially prepared current operation.
        job.kind = operations[index].index();
        std::optional<ImageWindow> window;
        std::optional<ImageWindow> display;
        core::PixelAspectRatio aspect = core::PixelAspectRatio::square();
        std::uint64_t requestedMetadata = 0;
        if (const auto* solid = std::get_if<GpuBatchSolid>(&operations[index])) {
            window = solid->parameters.dataWindow;
            display = solid->parameters.displayWindow;
            aspect = solid->parameters.pixelAspect;
            const auto& pixel = solid->parameters.pixel;
            pack(job, SolidPushConstants{{pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()},
                                         window->extent().width(),
                                         window->extent().height()});
        } else if (const auto* gradient = std::get_if<GpuBatchGradient>(&operations[index])) {
            window = gradient->dataWindow;
            display = gradient->displayWindow;
            aspect = gradient->pixelAspect;
            struct alignas(16) GradientPush final {
                std::array<float, 4> endpoints, transform, extent;
                std::array<std::int32_t, 4> flags, table;
            };
            static_assert(sizeof(GradientPush) == 80);
            constexpr float arithmeticLimit = std::numeric_limits<float>::max() / 2.0F;
            const auto finiteArray = [](const auto& values) {
                return std::ranges::all_of(values, [](const float v) {
                    return std::isfinite(v) &&
                           std::fabs(v) <= std::numeric_limits<float>::max() / 2.0F;
                });
            };
            const auto aspectValue = static_cast<float>(aspect.numerator()) /
                                     static_cast<float>(aspect.denominator());
            const auto safeGeometry = [&] {
                if (!std::isfinite(aspectValue) || aspectValue <= 0) return false;
                if (gradient->normalizedCoordinates) {
                    const auto& values = *gradient->normalizedCoordinates;
                    if (!finiteArray(values)) return false;
                    const auto pair = [&](const std::size_t index) {
                        return std::abs(static_cast<long double>(values[index * 2])) +
                               std::abs(static_cast<long double>(values[index * 2 + 1]));
                    };
                    const auto width = static_cast<long double>(window->extent().width() - 1);
                    const auto height = static_cast<long double>(window->extent().height() - 1);
                    const auto x = pair(0) + width * pair(gradient->radial ? 2 : 1);
                    const auto y = gradient->radial ? pair(1) + height * pair(3)
                                                    : height * pair(2);
                    if (gradient->radial) {
                        const auto minimumSquareRoot = std::sqrt(
                            static_cast<long double>(std::numeric_limits<float>::min()));
                        for (std::size_t coordinate = 0; coordinate < 4; ++coordinate)
                            if (pair(coordinate) > 0 && pair(coordinate) < minimumSquareRoot)
                                return false;
                    }
                    const auto magnitude = gradient->radial ? x * x + y * y : x + y;
                    return magnitude <= static_cast<long double>(arithmeticLimit);
                }
                const long double limit = static_cast<long double>(arithmeticLimit);
                const long double minNormal =
                    static_cast<long double>(std::numeric_limits<float>::min());
                const long double physicalAspect = static_cast<long double>(aspectValue);
                const long double scaleX =
                    static_cast<long double>(gradient->inverseHorizontalScale);
                const long double scaleY =
                    static_cast<long double>(gradient->inverseVerticalScale);
                const long double sx = static_cast<long double>(gradient->endpoints[0]);
                const long double sy = static_cast<long double>(gradient->endpoints[1]);
                const long double dx = (static_cast<long double>(gradient->endpoints[2]) - sx) *
                                       physicalAspect;
                const long double dy = static_cast<long double>(gradient->endpoints[3]) - sy;
                const long double span2 = dx * dx + dy * dy;
                if (span2 > limit || (span2 > 0 && span2 < minNormal)) return false;
                const long double farX =
                    (static_cast<long double>(window->extent().width()) - 0.5L) * scaleX;
                const long double farY =
                    (static_cast<long double>(window->extent().height()) - 0.5L) * scaleY;
                const long double px = std::max(
                    std::abs((0.5L * scaleX - sx) * physicalAspect),
                    std::abs((farX - sx) * physicalAspect));
                const long double py = std::max(
                    std::abs(0.5L * scaleY - sy), std::abs(farY - sy));
                const long double offset2 = px * px + py * py;
                const long double projection = px * std::abs(dx) + py * std::abs(dy);
                return offset2 <= limit && projection <= limit &&
                       (span2 == 0 ||
                        (gradient->radial ? std::sqrt(offset2 / span2) : projection / span2) <=
                            limit);
            };
            // The stop table: sorted positions in [0, 1], finite spans and colours, alpha in [0, 1].
            const auto stopsValid = [&] {
                const auto& stops = gradient->stops;
                if (stops.empty() || stops.size() > kGpuBatchGradientMaximumStops)
                    return false;
                for (std::size_t stop = 0; stop < stops.size(); ++stop) {
                    const auto& value = stops[stop];
                    if (!std::isfinite(value.position) || value.position < 0 ||
                        value.position > 1 || !std::isfinite(value.inverseSpan) ||
                        value.inverseSpan < 0 || !finiteArray(value.color) ||
                        value.color[3] < 0 || value.color[3] > 1 ||
                        (stop > 0 && value.position < stops[stop - 1].position))
                        return false;
                }
                return true;
            };
            if (!stopsValid() ||
                (!gradient->normalizedCoordinates &&
                 !std::ranges::all_of(gradient->endpoints,
                                      [](const float value) { return std::isfinite(value); })) ||
                !std::isfinite(gradient->inverseHorizontalScale) ||
                !std::isfinite(gradient->inverseVerticalScale) ||
                gradient->inverseHorizontalScale <= 0 || gradient->inverseVerticalScale <= 0 ||
                !std::isfinite(gradient->exactWidth) || gradient->exactWidth <= 0 ||
                !std::isfinite(gradient->exactHeight) || gradient->exactHeight <= 0 ||
                gradient->repeat > 2 || gradient->interpolation > 5 || gradient->colorMode > 2 ||
                gradient->hueDirection > 3 || !std::isfinite(gradient->leadIn) ||
                gradient->leadIn < 0 || !std::isfinite(gradient->leadOut) ||
                gradient->leadOut < 0 || window->originX() != 0 || window->originY() != 0 ||
                !safeGeometry()) {
                fail(GpuOperationBatchCode::Unsupported,
                     "gradient values exceed the native Float32 arithmetic capability");
                return false;
            }
            auto endpoints = gradient->endpoints;
            std::array<float, 4> transform{gradient->inverseHorizontalScale,
                                           gradient->inverseVerticalScale, aspectValue,
                                           static_cast<float>(gradient->repeat)};
            if (gradient->normalizedCoordinates) {
                std::copy_n(gradient->normalizedCoordinates->begin(), 4, endpoints.begin());
                std::copy_n(gradient->normalizedCoordinates->begin() + 4, 4, transform.begin());
            }
            pack(job, GradientPush{endpoints,
                                   transform,
                                   {static_cast<float>(window->extent().width()),
                                    static_cast<float>(window->extent().height()),
                                    gradient->exactWidth, gradient->exactHeight},
                                   {gradient->normalizedCoordinates ? (gradient->radial ? 3 : 2)
                                                                    : (gradient->radial ? 1 : 0),
                                    0, 0, static_cast<std::int32_t>(gradient->repeat)},
                                   {static_cast<std::int32_t>(gradient->stops.size()),
                                    static_cast<std::int32_t>(gradient->interpolation),
                                    static_cast<std::int32_t>(gradient->colorMode),
                                    static_cast<std::int32_t>(gradient->hueDirection)}});
            requestedMetadata = gradient->stops.size() * 8 * sizeof(float);
            if (requestedMetadata > budgets.maxMetadataBytes) {
                fail(GpuOperationBatchCode::OverBudget,
                     "batch metadata exceeds the operation ceiling");
                return false;
            }

        } else if (const auto* blend = std::get_if<GpuBatchBlend>(&operations[index])) {
            job.source = resolve(blend->source, index, job.sourcePin);
            job.destination = resolve(blend->destination, index, job.destinationPin);
            if (blend->mask)
                job.mask = resolve(*blend->mask, index, job.maskPin);
            if (!job.source || !job.destination || !job.source->dataWindow ||
                !job.destination->dataWindow || (blend->mask && !job.mask)) {
                fail(GpuOperationBatchCode::InvalidRequest,
                     "batch blend input is non-topological, foreign, pending, or has no window");
                return false;
            }
            blend_plan_detail::BlendPlan plan;
            const auto planned = blend_plan_detail::planBlend(
                *job.source->dataWindow, *job.destination->dataWindow,
                job.mask ? job.mask->dataWindow : std::nullopt, blend->mode, blend->imageCombine,
                blend->mask.has_value(), plan);
            if (planned.code != GpuBlendDiagnosticCode::None) {
                fail(planned.code == GpuBlendDiagnosticCode::OverBudget
                         ? GpuOperationBatchCode::OverBudget
                     : planned.code == GpuBlendDiagnosticCode::Unsupported
                         ? GpuOperationBatchCode::Unsupported
                         : GpuOperationBatchCode::InvalidRequest,
                     "batch blend parameters were refused");
                return false;
            }
            job.pipeline = plan.kernel == blend_plan_detail::BlendKernel::ImageCombine
                               ? &pipelines->imageCombine
                           : plan.kernel == blend_plan_detail::BlendKernel::General
                               ? (pipelines->generalUsesF64 ? &pipelines->blendF64
                                                            : &pipelines->blendPortable)
                               : &pipelines->blend;
            if (blend->imageCombine)
                pack(job, plan.combine);
            else
                pack(job, plan.push);
            window = job.destination->dataWindow;
            display = job.destination->displayWindow;
            aspect = job.destination->pixelAspect;
            requestedMetadata = sizeof(std::uint32_t);
        } else if (const auto* effect = std::get_if<GpuBatchOcioEffect>(&operations[index])) {
            auto* program =
                effect->program ? GpuOcioProgramBatchAccess::impl(*effect->program) : nullptr;
            if (program == nullptr || !program->onOwnerThread() || program->control != control ||
                program->displayArm || program->queueSubmitted || program->deviceLost ||
                program->jobState != GpuOcioProgramJobState::Idle ||
                program->expectedGeneration != expectedGeneration ||
                program->authorityDiagnostic().code != GpuOcioProgramDiagnosticCode::None) {
                fail(GpuOperationBatchCode::InvalidRequest,
                     "batch OCIO program is busy, foreign or not an effect program");
                return false;
            }
            for (std::uint32_t earlier = 0; earlier < index; ++earlier) {
                if (jobs[earlier].ocio == program) {
                    fail(GpuOperationBatchCode::InvalidRequest,
                         "a batch may dispatch each OCIO program once");
                    return false;
                }
            }
            const auto& bytes = effect->uniformBytes.empty() ? program->desc.uniformBufferData
                                                             : effect->uniformBytes;
            if (bytes.size() != program->desc.uniformBufferSize) {
                fail(GpuOperationBatchCode::InvalidRequest,
                     "batch OCIO uniform bytes do not match the declared UBO size");
                return false;
            }
            job.ocio = program;
            job.uniform.assign(bytes.begin(), bytes.end());
            job.source = resolve(effect->source, index, job.sourcePin);
            if (!job.source || !job.source->dataWindow || job.source->width == 0 ||
                job.source->height == 0) {
                fail(GpuOperationBatchCode::InvalidRequest,
                     "batch OCIO input is non-topological, foreign, pending, or has no window");
                return false;
            }
            const std::uint64_t pixels =
                static_cast<std::uint64_t>(job.source->width) * job.source->height;
            const auto grid = ocio_program_detail::planOcioDispatch(
                pixels, ocio_program_detail::kWorkgroupSizeX, program->maxWorkGroupCountX,
                program->maxWorkGroupCountY);
            if (!grid.valid() || pixels > std::numeric_limits<std::uint32_t>::max()) {
                fail(GpuOperationBatchCode::Unsupported,
                     "batch OCIO geometry cannot be covered by a capacity-safe dispatch");
                return false;
            }
            job.groupsX = grid.groupsX;
            job.groupsY = grid.groupsY;
            const std::array<std::uint32_t, 3> push{static_cast<std::uint32_t>(pixels),
                                                     job.source->width, job.source->height};
            pack(job, push);
            window = job.source->dataWindow;
            display = job.source->displayWindow;
            aspect = job.source->pixelAspect;
        } else {
            const GpuBatchImageRef* source = nullptr;
            float opacity = 1;
            std::uint32_t sampling = 0;
            if (const auto* translation = std::get_if<GpuBatchTranslation>(&operations[index])) {
                source = &translation->source;
                window = translation->outputWindow;
                opacity = translation->opacity;
                if (!std::isfinite(translation->translationX) ||
                    !std::isfinite(translation->translationY))
                    window.reset();
            } else if (const auto* affine = std::get_if<GpuBatchAffine>(&operations[index])) {
                source = &affine->source;
                window = affine->outputWindow;
                opacity = affine->opacity;
                sampling = static_cast<std::uint32_t>(affine->filter);
                if (!finite(affine->matrix) || sampling > 1)
                    window.reset();
            } else if (const auto* over = std::get_if<GpuBatchSourceOver>(&operations[index])) {
                source = &over->source;
                job.destination = resolve(over->destination, index, job.destinationPin);
                if (job.destination) {
                    window = job.destination->dataWindow;
                    display = job.destination->displayWindow;
                    aspect = job.destination->pixelAspect;
                }
            }
            job.source = source ? resolve(*source, index, job.sourcePin) : nullptr;
            if (!job.source || !window || !job.source->dataWindow || !opacityValid(opacity) ||
                (job.kind == 3 && !job.destination)) {
                fail(GpuOperationBatchCode::InvalidRequest,
                     "batch input is non-topological, foreign, pending, or has invalid fields");
                return false;
            }
            if (job.kind != 3) {
                display = job.source->displayWindow;
                aspect = job.source->pixelAspect;
            }
            if (const auto* translation = std::get_if<GpuBatchTranslation>(&operations[index]);
                translation && translation->outputDisplayWindow)
                display = translation->outputDisplayWindow;
            if (const auto* affine = std::get_if<GpuBatchAffine>(&operations[index]);
                affine && affine->outputDescriptor) {
                if (affine->outputDescriptor->dataWindow() != *window) {
                    fail(GpuOperationBatchCode::InvalidRequest,
                         "batch affine output descriptor disagrees with its data window");
                    return false;
                }
                display = affine->outputDescriptor->displayWindow();
                aspect = affine->outputDescriptor->pixelAspect();
            }
            requestedMetadata = sizeof(std::uint32_t);
            const auto width = window->extent().width();
            const auto height = window->extent().height();
            if (job.kind == 1)
                requestedMetadata +=
                    (static_cast<std::uint64_t>(width) + height) * sizeof(GpuAxisSample);
            else if (job.kind == 2)
                requestedMetadata += sizeof(GpuAffineMapGpu);
            if (requestedMetadata > budgets.maxMetadataBytes) {
                fail(GpuOperationBatchCode::OverBudget,
                     "batch metadata exceeds the operation ceiling");
                return false;
            }
            if (job.kind == 3) {
                const auto sourceWindow = *job.source->dataWindow;
                std::int32_t offsetX = 0;
                std::int32_t offsetY = 0;
                if (!compositeSourceOffset(window->originX(), sourceWindow.originX(), width,
                                           offsetX) ||
                    !compositeSourceOffset(window->originY(), sourceWindow.originY(), height,
                                           offsetY)) {
                    fail(GpuOperationBatchCode::InvalidRequest,
                         "source-over offset is not representable");
                    return false;
                }
                pack(job, CompositeSourceOverPush{width, height, job.source->width,
                                                  job.source->height, offsetX, offsetY});
            } else if (job.kind == 2) {
                pack(job, AffinePush{width, height, job.source->width, job.source->height, opacity,
                                     sampling});
            } else {
                pack(job, CompositeTranslationPush{width, height, job.source->width,
                                                   job.source->height, opacity});
            }
        }
        std::uint64_t imageBytes = 0;
        const auto width = window->extent().width();
        const auto height = window->extent().height();
        const auto support = querySolidImageSupport(*control, width, height);
        const auto limits = control->physicalDevice.getProperties().limits;
        if (!support.supported ||
            compositeDispatchGroupCount(width) > limits.maxComputeWorkGroupCount[0] ||
            height > limits.maxComputeWorkGroupCount[1]) {
            fail(GpuOperationBatchCode::Unsupported,
                 "batch image or dispatch extent is unsupported");
            return false;
        }
        const auto remaining = byteBudget - std::min(byteBudget, statistics.peakAllocationBytes);
        if (!display || compositeMultiplyOverflows(width, height, imageBytes) ||
            compositeMultiplyOverflows(imageBytes, sizeof(Rgba32f), imageBytes) ||
            imageBytes > budgets.maxImageBytes || imageBytes > support.maxImageBytes ||
            imageBytes > remaining || requestedMetadata > remaining - imageBytes) {
            fail(GpuOperationBatchCode::OverBudget,
                 "batch image extent or requested bytes exceed capacity");
            return false;
        }
        job.image = std::make_unique<GpuImageImpl>();
        job.image->state = control;
        job.image->dataWindow = window;
        job.image->displayWindow = display;
        job.image->pixelAspect = aspect;
        job.image->generation = expectedGeneration;
        if (!createResidentImage(*control, width, height, *job.image, true)) {
            fail(GpuOperationBatchCode::AllocationFailed, "batch output backing was refused");
            return false;
        }
        const auto buffer = [this, &job](const std::size_t slot, const std::uint64_t bytes,
                                         const bool host, const void* data) {
            return createCompositeBuffer(control, bytes, host, data, job.metadata[slot], true);
        };
        const std::uint32_t zero = 0;
        if (job.kind != 0 && job.kind != 4 && job.kind != kOcioEffectKind &&
            !buffer(2, sizeof(zero), true, &zero)) {
            fail(GpuOperationBatchCode::AllocationFailed, "batch status backing was refused");
            return false;
        }
        if (const auto* translation = std::get_if<GpuBatchTranslation>(&operations[index])) {
            const auto axisX = composite_detail::prepareProcessingTranslationAxis(
                width, job.source->width, translation->translationX, control->processingPhase);
            const auto axisY = composite_detail::prepareProcessingTranslationAxis(
                height, job.source->height, translation->translationY, control->processingPhase);
            if (!buffer(0, axisX.size() * sizeof(GpuAxisSample), false, axisX.data()) ||
                !buffer(1, axisY.size() * sizeof(GpuAxisSample), false, axisY.data())) {
                fail(GpuOperationBatchCode::AllocationFailed,
                     "batch translation metadata was refused");
                return false;
            }
        } else if (const auto* gradient = std::get_if<GpuBatchGradient>(&operations[index])) {
            std::vector<float> table;
            table.reserve(gradient->stops.size() * 8);
            for (const auto& stop : gradient->stops) {
                // #546: the first stop's spare pair carries the spans before and after the table.
                const bool first = table.empty();
                table.insert(table.end(),
                             {stop.position, stop.inverseSpan, first ? gradient->leadIn : 0.0F,
                              first ? gradient->leadOut : 0.0F});
                table.insert(table.end(), stop.color.begin(), stop.color.end());
            }
            if (!buffer(0, table.size() * sizeof(float), false, table.data())) {
                fail(GpuOperationBatchCode::AllocationFailed, "batch gradient stops were refused");
                return false;
            }
        } else if (const auto* affine = std::get_if<GpuBatchAffine>(&operations[index])) {
            const auto map = packAffineMap(prepareAffineMatrixMap(affine->matrix, *window));
            if (!buffer(0, sizeof(map), false, &map)) {
                fail(GpuOperationBatchCode::AllocationFailed, "batch affine metadata was refused");
                return false;
            }
        }
        const auto actualImage = compositeImageAllocationBytes(*job.image);
        if (compositeAddOverflows(statistics.imageBytes, actualImage, statistics.imageBytes) ||
            compositeAddOverflows(statistics.peakAllocationBytes, actualImage,
                                  statistics.peakAllocationBytes)) {
            fail(GpuOperationBatchCode::OverBudget, "batch allocation accounting overflowed");
            return false;
        }
        for (const auto& metadata : job.metadata) {
            std::uint64_t bytes = 0;
            if (compositeAddOverflows(compositeBufferAllocationBytes(metadata),
                                      metadata.stagingBytes, bytes) ||
                compositeAddOverflows(statistics.peakAllocationBytes, bytes,
                                      statistics.peakAllocationBytes)) {
                fail(GpuOperationBatchCode::OverBudget, "batch metadata accounting overflowed");
                return false;
            }
        }
        if (statistics.peakAllocationBytes > byteBudget) {
            fail(GpuOperationBatchCode::OverBudget,
                 "actual whole-batch backing exceeds granted headroom");
            return false;
        }
    }
    return true;
}
} // namespace bloom::render::operation_batch_detail
