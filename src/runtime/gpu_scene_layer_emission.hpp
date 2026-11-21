#ifndef BLOOM_RUNTIME_GPU_SCENE_LAYER_EMISSION_HPP
#define BLOOM_RUNTIME_GPU_SCENE_LAYER_EMISSION_HPP

// Private to src/runtime. The shared builder's raster-layer emission: the exact translation path
// and the accepted GpuAffine placement. Header-only and templated on the builder's emit/charge
// closures so the split adds no std::function and no shared state; the builder still owns the
// command vector, the per-operation maps, the budget and the coverage cache.

#include "gpu_scene_preparation_common.hpp"
#include "gpu_scene_preparation_private.hpp"
#include "layer_parent_transform.hpp"

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace bloom::runtime::detail {

// toGpuAffineMatrix() lives in layer_parent_transform.hpp beside the CPU row that shares it.

// The output-window-local device translation for a translation-only LayerTransform, or disengaged
// when the transform is not an exact translation.
[[nodiscard]] inline std::optional<std::pair<double, double>>
translationOnlyDeviceDelta(const render::LayerTransform& transform,
                           const render::ImageWindow sourceWindow,
                           const render::ImageWindow outputWindow) {
    const auto device = transform.translationOnlyDeviceTranslation();
    if (!device.has_value()) {
        return std::nullopt;
    }
    return std::make_pair(device->x - (static_cast<double>(outputWindow.originX()) -
                                       static_cast<double>(sourceWindow.originX())),
                          device->y - (static_cast<double>(outputWindow.originY()) -
                                       static_cast<double>(sourceWindow.originY())));
}

template <typename Charge, typename Emit>
[[nodiscard]] inline std::optional<GpuSceneLeafFailure> emitTranslationOpacityCommand(
    const GpuSceneCommandIndex input, const std::string& inputKey,
    const render::ImageWindow sourceWindow, const render::ImageWindow outputWindow,
    const double translationX, const double translationY, const double opacity,
    const core::PixelAspectRatio fullPixelAspect, const OperationIndex operationIndex,
    Charge&& charge, Emit&& emit, GpuSceneCommandIndex& out, std::string& key) {
    OperationKey commandKey;
    commandKey.add(std::string{"gpu-translation-opacity-v1"});
    commandKey.add(inputKey);
    commandKey.add(translationX);
    commandKey.add(translationY);
    commandKey.add(std::bit_cast<std::uint32_t>(static_cast<float>(opacity)));
    addWindowToKey(commandKey, sourceWindow);
    addWindowToKey(commandKey, outputWindow);
    addPixelAspectToKey(commandKey, fullPixelAspect);
    commandKey.add(std::string{kGpuTranslationOpacitySpirvSha256});
    key = commandKey.digest();
    if (const auto error = charge(outputWindow.extent().width(), outputWindow.extent().height(),
                                  sizeof(render::Rgba32f))) {
        return error;
    }
    GpuSceneTranslationCommand command{.sourceOperation = operationIndex,
                                       .input = input,
                                       .sourceWindow = sourceWindow,
                                       .outputWindow = outputWindow,
                                       .translationX = translationX,
                                       .translationY = translationY,
                                       .opacity = static_cast<float>(opacity),
                                       .semanticKey = key};
    out = emit(std::move(command));
    return std::nullopt;
}

// #742: one composed-matrix resample of `input`. Bilinear and Nearest, and a Trilinear or
// Anisotropic map that does not shrink the image (planAffineFilter resolves those to Bilinear),
// emit the one affine command. A shrinking filtered map first emits the mip levels it reads, each
// a Bilinear affine command halving the previous level through affineMipReductionMatrix() and
// keyed by the source's key, so the scene cache retains the pyramid per source identity and an
// unchanged source reuses it on the next frame; then the filtered command reads the plan's lower
// and upper levels. Every level is charged to the frame budget.
template <typename Charge, typename Emit>
[[nodiscard]] inline std::optional<GpuSceneLeafFailure> emitAffineMatrixCommand(
    const GpuSceneCommandIndex input, const std::string& inputKey,
    const render::ImageWindow sourceWindow, const render::ImageWindow outputWindow,
    const render::GpuAffineMatrix& affineMatrix, const float opacity,
    const core::PixelAspectRatio pixelAspect,
    const std::optional<render::Rgba32fImageDescriptor>& outputDescriptor,
    const render::GpuAffineFilter requestedFilter, const OperationIndex operationIndex,
    Charge&& charge, Emit&& emit, GpuSceneCommandIndex& out, std::string& key,
    const std::optional<core::PixelAspectRatio> sourcePixelAspect = std::nullopt) {
    // A level inherits its source's display window and pixel aspect.
    const auto levelPixelAspect = sourcePixelAspect.value_or(pixelAspect);
    const auto plan =
        render::planAffineFilter(affineMatrix, requestedFilter, sourceWindow.extent().width(),
                                 sourceWindow.extent().height());
    GpuSceneCommandIndex lower = input;
    std::string lowerKey = inputKey;
    GpuSceneCommandIndex upper = kInvalidGpuSceneCommand;
    std::string upperKey;
    if (plan.filtered()) {
        GpuSceneCommandIndex level = input;
        std::string levelKey = inputKey;
        render::ImageWindow levelWindow = sourceWindow;
        const auto reduction = render::affineMipReductionMatrix();
        for (std::uint32_t index = 1; index <= plan.upperLevel; ++index) {
            const auto [width, height] = render::affineMipLevelExtent(
                sourceWindow.extent().width(), sourceWindow.extent().height(), index);
            const auto next = render::ImageWindow::create(0, 0, width, height);
            if (!next)
                return fail(PreparedGpuSceneDiagnosticCode::UnsupportedTransform,
                            "A mip level window is not representable");
            if (const auto error = charge(width, height, sizeof(render::Rgba32f)))
                return error;
            auto nextKey =
                makeGpuSceneAffineSemanticKey(levelKey, levelWindow, reduction, 1.0F, *next.value(),
                                              levelPixelAspect, std::string{kGpuAffineSpirvSha256});
            level = emit(GpuSceneAffineCommand{.sourceOperation = operationIndex,
                                               .input = level,
                                               .inputKey = levelKey,
                                               .sourceWindow = levelWindow,
                                               .outputWindow = *next.value(),
                                               .matrix = reduction,
                                               .opacity = 1.0F,
                                               .pixelAspect = levelPixelAspect,
                                               .semanticKey = nextKey});
            levelKey = std::move(nextKey);
            levelWindow = *next.value();
            if (index == plan.lowerLevel) {
                lower = level;
                lowerKey = levelKey;
            }
        }
        upper = plan.upperLevel == plan.lowerLevel ? lower : level;
        upperKey = plan.upperLevel == plan.lowerLevel ? lowerKey : levelKey;
    }
    const auto filter = plan.filter;
    key = makeGpuSceneAffineSemanticKey(lowerKey, sourceWindow, affineMatrix, opacity, outputWindow,
                                        pixelAspect, std::string{kGpuAffineSpirvSha256},
                                        outputDescriptor, filter, &plan, upperKey);
    if (const auto error = charge(outputWindow.extent().width(), outputWindow.extent().height(),
                                  sizeof(render::Rgba32f))) {
        return error;
    }
    out = emit(GpuSceneAffineCommand{.sourceOperation = operationIndex,
                                     .input = lower,
                                     .inputKey = lowerKey,
                                     .sourceWindow = sourceWindow,
                                     .outputWindow = outputWindow,
                                     .matrix = affineMatrix,
                                     .opacity = opacity,
                                     .pixelAspect = pixelAspect,
                                     .semanticKey = key,
                                     .outputDescriptor = outputDescriptor,
                                     .filter = filter,
                                     .upperInput = upper,
                                     .upperInputKey = upperKey,
                                     .plan = plan});
    return std::nullopt;
}

template <typename Charge, typename Emit>
[[nodiscard]] inline std::optional<GpuSceneLeafFailure> emitAffineLayerCommand(
    const GpuSceneCommandIndex input, const std::string& inputKey,
    const render::ImageWindow sourceWindow, const render::ImageWindow outputWindow,
    const LayerMatrix& matrix, const double scaleX, const double scaleY, const double opacity,
    const core::PixelAspectRatio fullPixelAspect, const OperationIndex operationIndex,
    Charge&& charge, Emit&& emit, GpuSceneCommandIndex& out, std::string& key,
    const render::GpuAffineFilter filter = render::GpuAffineFilter::Bilinear) {
    const auto affineMatrix = toGpuAffineMatrix(matrix, sourceWindow, scaleX, scaleY);
    return emitAffineMatrixCommand(input, inputKey, sourceWindow, outputWindow, affineMatrix,
                                   static_cast<float>(opacity), fullPixelAspect, std::nullopt,
                                   filter, operationIndex, charge, emit, out, key);
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_LAYER_EMISSION_HPP
