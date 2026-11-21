#ifndef BLOOM_RUNTIME_GPU_SCENE_SUBTITLES_EMISSION_HPP
#define BLOOM_RUNTIME_GPU_SCENE_SUBTITLES_EMISSION_HPP

// Private to src/runtime. #752: the GPU production preparation of a Subtitles node. The frame's
// host preparation (subtitles_frame.hpp, the same call the CPU reference makes) gives the box,
// outline and fill layers in drawing order; each becomes a native coverage command over the cues'
// ink window, its bounded PathRasterCoverageGeometry rasterized on the device (never a host
// per-pixel mask), and one source-over merge folds them in order over transparent, as the CPU
// reference folds its rows. The merge is the node's image: a Layer Output, an image effect or a
// Merge consumes it like any other image. Templated on the builder's closures, like the other
// leaf emissions.

#include "gpu_scene_preparation_common.hpp"
#include "gpu_scene_preparation_private.hpp"
#include "operation_key.hpp"
#include "subtitles_frame.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime::detail {

template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure> emitSubtitlesCommands(
    const CompiledSubtitles& subtitles, const CompiledCompositionPlan& plan,
    const ResolvedEvaluation& resolved, const core::RationalTime time, const std::size_t index,
    const OperationIndex operationIndex, const render::ImageWindow fullDisplayWindow,
    const core::PixelAspectRatio fullPixelAspect,
    const std::shared_ptr<GpuSceneCoverageCache>& coverageCache,
    const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
    ChargeCoverage&& chargeCoverage, std::vector<GpuSceneCommandIndex>& commandForOperation,
    std::vector<std::string>& keyOf,
    std::vector<std::optional<render::ImageWindow>>& outputWindowOf,
    std::vector<EvaluatedOperationBounds>& bounds) {
    auto prepared =
        prepareSubtitlesFrame(subtitles, plan, resolved, time, fullDisplayWindow, cancellation);
    if (const auto* failure = std::get_if<SubtitlesFailure>(&prepared)) {
        // A frame past a subtitle limit fails on this route as on the CPU reference: the same
        // message, never a silent drop.
        return fail(failure->kind == SubtitlesFailureKind::Cancelled
                        ? PreparedGpuSceneDiagnosticCode::Cancelled
                        : PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    failure->message);
    }
    const auto& frame = std::get<SubtitlesFrame>(prepared);
    bounds[index].local = frame.bounds;
    bounds[index].output = frame.bounds;
    if (!frame.window)
        return std::nullopt; // nothing on screen: the leaf publishes no image
    const auto window = *frame.window;
    const auto width = window.extent().width();
    const auto height = window.extent().height();
    outputWindowOf[index] = window;
    const auto cancel = [&cancellation] { return cancellation.isCancellationRequested(); };
    std::vector<GpuSceneCommandIndex> foregrounds;
    OperationKey mergeKey;
    mergeKey.add(std::string{"gpu-source-over-chain-v1"});
    for (std::size_t layerIndex = 0; layerIndex < frame.layers.size(); ++layerIndex) {
        const auto& layer = frame.layers[layerIndex];
        OperationKey geometryKey;
        geometryKey.add(std::string{"gpu-subtitles-coverage-v1"});
        geometryKey.add(frame.contentKey);
        geometryKey.add(layerIndex);
        geometryKey.add(layer.stroke);
        addWindowToKey(geometryKey, window);
        addPixelAspectToKey(geometryKey, fullPixelAspect);
        const auto geometryKeyDigest = geometryKey.digest();
        std::shared_ptr<const render::PathRasterCoverageGeometry> geometry;
        if (coverageCache != nullptr)
            geometry = coverageCache->find(geometryKeyDigest);
        if (geometry == nullptr) {
            auto built = layer.raster->coverageGeometry(window.originX(), window.originY(),
                                                        static_cast<std::uint32_t>(width),
                                                        static_cast<std::uint32_t>(height),
                                                        render::PathFillRule::NonZero,
                                                        layer.stroke, cancel);
            if (!built) {
                if (cancellation.isCancellationRequested())
                    return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                                "Subtitles coverage was cancelled");
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                            "Subtitles coverage geometry is invalid");
            }
            geometry = std::make_shared<const render::PathRasterCoverageGeometry>(
                std::move(*built.value()));
            if (coverageCache != nullptr)
                coverageCache->store(geometryKeyDigest, geometry);
        }
        if (const auto error = charge(width, height, sizeof(render::Rgba32f)))
            return error;
        OperationKey pixelKey;
        pixelKey.add(geometryKeyDigest);
        pixelKey.add(std::bit_cast<std::uint32_t>(layer.pixel.red()));
        pixelKey.add(std::bit_cast<std::uint32_t>(layer.pixel.green()));
        pixelKey.add(std::bit_cast<std::uint32_t>(layer.pixel.blue()));
        pixelKey.add(std::bit_cast<std::uint32_t>(layer.pixel.alpha()));
        pixelKey.add(std::bit_cast<std::uint32_t>(1.0F));
        pixelKey.add(std::string{kGpuCoveredSolidSpirvSha256});
        GpuSceneCoverageSolidCommand command{.index = kInvalidGpuSceneCommand,
                                             .sourceOperation = operationIndex,
                                             .pixel = layer.pixel,
                                             .opacity = 1.0F,
                                             .geometry = geometry,
                                             .coverage = nullptr,
                                             .palette = nullptr,
                                             .outputWindow = window,
                                             .displayWindow = fullDisplayWindow,
                                             .pixelAspect = fullPixelAspect,
                                             .geometryKey = geometryKeyDigest,
                                             .semanticKey = pixelKey.digest()};
        if (const auto error = chargeCoverage(command))
            return error;
        mergeKey.add(command.semanticKey);
        foregrounds.push_back(emit(std::move(command)));
    }
    addWindowToKey(mergeKey, window);
    addWindowToKey(mergeKey, fullDisplayWindow);
    addPixelAspectToKey(mergeKey, fullPixelAspect);
    mergeKey.add(std::string{kGpuSourceOverSpirvSha256});
    keyOf[index] = mergeKey.digest();
    if (const auto error = charge(width, height, sizeof(render::Rgba32f)))
        return error;
    GpuSceneMergeCommand merge{.sourceOperation = operationIndex,
                               .foregrounds = std::move(foregrounds),
                               .outputWindow = window,
                               .displayWindow = fullDisplayWindow,
                               .pixelAspect = fullPixelAspect,
                               .semanticKey = keyOf[index]};
    commandForOperation[index] = emit(std::move(merge));
    return std::nullopt;
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_SUBTITLES_EMISSION_HPP
