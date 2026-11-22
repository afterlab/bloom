#ifndef BLOOM_RUNTIME_GPU_SCENE_SUBTITLES_EMISSION_HPP
#define BLOOM_RUNTIME_GPU_SCENE_SUBTITLES_EMISSION_HPP

// Private to src/runtime. #752: the GPU production preparation of a Subtitles node. The frame's
// host preparation (subtitles_frame.hpp, the same call the CPU reference makes) gives the box,
// outline and fill layers in drawing order. The node is a vector leaf, as Text is: it emits no
// command of its own. The Layer Output that carries it redraws every layer's paths through the
// layer matrix as native coverage over the layer window (each layer's bounded
// PathRasterCoverageGeometry rasterized on the device, never a host per-pixel mask), folds them in
// order with one source-over merge and applies the layer opacity, so a scaled or rotated layer stays
// crisp. On the integer grid, and where an image effect or a Merge reads the node as an image, the
// leaf is materialized as the CPU reference's own image: the layers over the cues' ink window at the
// frame, folded by the merge. Templated on the builder's closures, like the other leaf emissions.

#include "gpu_scene_preparation_common.hpp"
#include "gpu_scene_preparation_private.hpp"
#include "operation_key.hpp"
#include "subtitles_frame.hpp"
#include "text_styled_frame.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bloom::runtime::detail {

// Prepares the node's frame at `time` and publishes its bounds and ink window, as the CPU
// reference's Subtitles arm does. No command: the consumer draws the frame.
[[nodiscard]] inline std::optional<GpuSceneLeafFailure>
prepareSubtitlesLeaf(const CompiledSubtitles& subtitles, const CompiledCompositionPlan& plan,
                     const ResolvedEvaluation& resolved, const core::RationalTime time,
                     const std::size_t index, const render::ImageWindow fullDisplayWindow,
                     const CancellationToken& cancellation,
                     std::vector<std::optional<render::ImageWindow>>& outputWindowOf,
                     std::vector<EvaluatedOperationBounds>& bounds,
                     std::vector<std::shared_ptr<const SubtitlesFrame>>& frames) {
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
    auto frame =
        std::make_shared<const SubtitlesFrame>(std::move(std::get<SubtitlesFrame>(prepared)));
    bounds[index].local = frame->bounds;
    bounds[index].output = frame->bounds;
    // Nothing on screen: the leaf publishes no image, and its layer draws nothing.
    if (frame->window)
        outputWindowOf[index] = *frame->window;
    frames[index] = std::move(frame);
    return std::nullopt;
}

// #757: a styled Text Source's leaf: its frame prepared at the evaluation (text_styled_frame.hpp,
// the same call the CPU reference makes), its bounds and ink window published, no command.
[[nodiscard]] inline std::optional<GpuSceneLeafFailure>
prepareStyledTextLeaf(const CompiledText& text, const CompiledCompositionPlan& plan,
                      const ResolvedEvaluation& resolved, const core::RationalTime time,
                      const std::size_t index,
                      const CancellationToken& cancellation,
                      std::vector<std::optional<render::ImageWindow>>& outputWindowOf,
                      std::vector<EvaluatedOperationBounds>& bounds,
                      std::vector<std::shared_ptr<const SubtitlesFrame>>& frames) {
    auto prepared = prepareStyledTextFrame(text, plan, resolved, time, cancellation);
    if (const auto* failure = std::get_if<SubtitlesFailure>(&prepared)) {
        return fail(failure->kind == SubtitlesFailureKind::Cancelled
                        ? PreparedGpuSceneDiagnosticCode::Cancelled
                        : PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    failure->message);
    }
    auto frame =
        std::make_shared<const SubtitlesFrame>(std::move(std::get<SubtitlesFrame>(prepared)));
    bounds[index].local = frame->bounds;
    bounds[index].output = frame->bounds;
    if (frame->window)
        outputWindowOf[index] = *frame->window;
    frames[index] = std::move(frame);
    return std::nullopt;
}

// One coverage command per layer, then one source-over merge that folds them over transparent in
// drawing order (a single layer is its own result). `geometryOf` builds a layer's coverage over
// `window` and its geometry key; `opacity` is the layer opacity, applied after the fold.
template <typename Geometry, typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure> emitSubtitlesLayers(
    const SubtitlesFrame& frame, const render::ImageWindow window,
    const render::ImageWindow fullDisplayWindow, const core::PixelAspectRatio fullPixelAspect,
    const double opacity, const bool alwaysMerge, const OperationIndex operationIndex,
    Geometry&& geometryOf, Emit&& emit, Charge&& charge, ChargeCoverage&& chargeCoverage,
    GpuSceneCommandIndex& composed, std::string& semanticKey) {
    const auto width = window.extent().width();
    const auto height = window.extent().height();
    const bool folded = alwaysMerge || frame.layers.size() > 1;
    // A lone layer carries the opacity in its own command, as a Text coverage does.
    const float commandOpacity = folded ? 1.0F : static_cast<float>(opacity);
    std::vector<GpuSceneCommandIndex> foregrounds;
    OperationKey mergeKey;
    mergeKey.add(std::string{"gpu-source-over-chain-v1"});
    for (std::size_t layerIndex = 0; layerIndex < frame.layers.size(); ++layerIndex) {
        const auto& layer = frame.layers[layerIndex];
        std::shared_ptr<const render::PathRasterCoverageGeometry> geometry;
        std::string geometryKey;
        if (const auto error = geometryOf(layerIndex, layer, geometry, geometryKey))
            return error;
        if (const auto error = charge(width, height, sizeof(render::Rgba32f)))
            return error;
        OperationKey pixelKey;
        pixelKey.add(geometryKey);
        pixelKey.add(std::bit_cast<std::uint32_t>(layer.pixel.red()));
        pixelKey.add(std::bit_cast<std::uint32_t>(layer.pixel.green()));
        pixelKey.add(std::bit_cast<std::uint32_t>(layer.pixel.blue()));
        pixelKey.add(std::bit_cast<std::uint32_t>(layer.pixel.alpha()));
        pixelKey.add(std::bit_cast<std::uint32_t>(commandOpacity));
        pixelKey.add(std::string{kGpuCoveredSolidSpirvSha256});
        GpuSceneCoverageSolidCommand command{.index = kInvalidGpuSceneCommand,
                                             .sourceOperation = operationIndex,
                                             .pixel = layer.pixel,
                                             .opacity = commandOpacity,
                                             .geometry = std::move(geometry),
                                             .coverage = nullptr,
                                             .palette = nullptr,
                                             .outputWindow = window,
                                             .displayWindow = fullDisplayWindow,
                                             .pixelAspect = fullPixelAspect,
                                             .geometryKey = std::move(geometryKey),
                                             .semanticKey = pixelKey.digest()};
        if (const auto error = chargeCoverage(command))
            return error;
        mergeKey.add(command.semanticKey);
        semanticKey = command.semanticKey;
        foregrounds.push_back(emit(std::move(command)));
    }
    if (foregrounds.empty())
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "A drawn Subtitles frame has no layer");
    if (!folded) {
        composed = foregrounds.front();
        return std::nullopt;
    }
    addWindowToKey(mergeKey, window);
    addWindowToKey(mergeKey, fullDisplayWindow);
    addPixelAspectToKey(mergeKey, fullPixelAspect);
    mergeKey.add(std::string{kGpuSourceOverSpirvSha256});
    semanticKey = mergeKey.digest();
    if (const auto error = charge(width, height, sizeof(render::Rgba32f)))
        return error;
    GpuSceneMergeCommand merge{.sourceOperation = operationIndex,
                               .foregrounds = std::move(foregrounds),
                               .outputWindow = window,
                               .displayWindow = fullDisplayWindow,
                               .pixelAspect = fullPixelAspect,
                               .semanticKey = semanticKey};
    composed = emit(std::move(merge));
    if (opacity == 1.0)
        return std::nullopt;
    // The layer opacity after the fold, as the CPU reference fades the composited row.
    if (const auto error = charge(width, height, sizeof(render::Rgba32f)))
        return error;
    OperationKey key;
    key.add(std::string{"gpu-translation-opacity-v1"});
    key.add(semanticKey);
    key.add(0.0);
    key.add(0.0);
    key.add(std::bit_cast<std::uint32_t>(static_cast<float>(opacity)));
    addWindowToKey(key, window);
    addWindowToKey(key, window);
    addPixelAspectToKey(key, fullPixelAspect);
    key.add(std::string{kGpuTranslationOpacitySpirvSha256});
    semanticKey = key.digest();
    composed = emit(GpuSceneTranslationCommand{.sourceOperation = operationIndex,
                                               .input = composed,
                                               .sourceWindow = window,
                                               .outputWindow = window,
                                               .translationX = 0.0,
                                               .translationY = 0.0,
                                               .opacity = static_cast<float>(opacity),
                                               .semanticKey = semanticKey});
    return std::nullopt;
}

// The node's own image (the CPU reference's leaf): the layers rasterized at the frame over the
// cues' ink window, folded in order.
template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure> emitSubtitlesLeafImage(
    const SubtitlesFrame& frame, const render::ImageWindow fullDisplayWindow,
    const core::PixelAspectRatio fullPixelAspect, const OperationIndex operationIndex,
    const std::shared_ptr<GpuSceneCoverageCache>& coverageCache,
    const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
    ChargeCoverage&& chargeCoverage, GpuSceneCommandIndex& composed, std::string& semanticKey) {
    if (!frame.window)
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "An empty Subtitles frame was drawn");
    const auto window = *frame.window;
    const auto cancel = [&cancellation] { return cancellation.isCancellationRequested(); };
    const auto geometryOf =
        [&](const std::size_t layerIndex, const SubtitlesLayer& layer,
            std::shared_ptr<const render::PathRasterCoverageGeometry>& geometry,
            std::string& geometryKeyDigest) -> std::optional<GpuSceneLeafFailure> {
        OperationKey geometryKey;
        geometryKey.add(std::string{"gpu-subtitles-coverage-v1"});
        geometryKey.add(frame.contentKey);
        geometryKey.add(layerIndex);
        geometryKey.add(layer.stroke);
        addWindowToKey(geometryKey, window);
        addPixelAspectToKey(geometryKey, fullPixelAspect);
        geometryKeyDigest = geometryKey.digest();
        if (coverageCache != nullptr)
            geometry = coverageCache->find(geometryKeyDigest);
        if (geometry != nullptr)
            return std::nullopt;
        auto built = layer.raster->coverageGeometry(
            window.originX(), window.originY(), static_cast<std::uint32_t>(window.extent().width()),
            static_cast<std::uint32_t>(window.extent().height()), render::PathFillRule::NonZero,
            layer.stroke, cancel);
        if (!built)
            return cancellation.isCancellationRequested()
                       ? fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                              "Subtitles coverage was cancelled")
                       : fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                              "Subtitles coverage geometry is invalid");
        geometry =
            std::make_shared<const render::PathRasterCoverageGeometry>(std::move(*built.value()));
        if (coverageCache != nullptr)
            coverageCache->store(geometryKeyDigest, geometry);
        return std::nullopt;
    };
    return emitSubtitlesLayers(frame, window, fullDisplayWindow, fullPixelAspect, 1.0, true,
                               operationIndex, geometryOf, emit, charge, chargeCoverage, composed,
                               semanticKey);
}

// The vector leaf drawn by its Layer Output: every layer's author-space paths through the full
// composed `matrix` at the proxy scales, over `layerWindow`, folded in order, then `opacity`. The
// CPU reference's Layer Output vector arm draws exactly this.
template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure> emitSubtitlesVectorLayer(
    const SubtitlesFrame& frame, const LayerMatrix& matrix, const render::ImageWindow layerWindow,
    const render::ImageWindow fullDisplayWindow, const core::PixelAspectRatio fullPixelAspect,
    const double hScale, const double vScale, const double opacity,
    const OperationIndex operationIndex,
    const std::shared_ptr<GpuSceneCoverageCache>& coverageCache,
    const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
    ChargeCoverage&& chargeCoverage, GpuSceneCommandIndex& composed, std::string& semanticKey) {
    const auto cancel = [&cancellation] { return cancellation.isCancellationRequested(); };
    const auto geometryOf =
        [&](const std::size_t layerIndex, const SubtitlesLayer& layer,
            std::shared_ptr<const render::PathRasterCoverageGeometry>& geometry,
            std::string& geometryKeyDigest) -> std::optional<GpuSceneLeafFailure> {
        // Everything that changes a covered byte: the frame's content (cues, style, fade phase,
        // fonts, frame and proxy scales), which layer, the matrix and the window. Never the time.
        OperationKey geometryKey;
        geometryKey.add(std::string{"gpu-subtitles-vector-coverage-v1"});
        geometryKey.add(frame.contentKey);
        geometryKey.add(layerIndex);
        geometryKey.add(layer.stroke);
        for (const auto value : {matrix.a, matrix.b, matrix.c, matrix.d, matrix.x, matrix.y})
            geometryKey.add(value);
        geometryKey.add(hScale);
        geometryKey.add(vScale);
        addWindowToKey(geometryKey, layerWindow);
        addPixelAspectToKey(geometryKey, fullPixelAspect);
        geometryKeyDigest = geometryKey.digest();
        if (coverageCache != nullptr)
            geometry = coverageCache->find(geometryKeyDigest);
        if (geometry != nullptr)
            return std::nullopt;
        const auto failed = [&cancellation] {
            return cancellation.isCancellationRequested()
                       ? fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                              "Subtitles coverage was cancelled")
                       : fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                              "Subtitles coverage geometry is invalid");
        };
        if (layer.paths == nullptr)
            return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                        "A Subtitles layer has no paths");
        auto raster = render::PathRaster::transformed(
            std::span<const render::Path>(*layer.paths), layer.pathStroke,
            render::PathMatrix{matrix.a, matrix.b, matrix.c, matrix.d, matrix.x, matrix.y}, hScale,
            vScale, cancel, layer.clip);
        if (!raster)
            return failed();
        auto built = raster.value()->coverageGeometry(
            layerWindow.originX(), layerWindow.originY(),
            static_cast<std::uint32_t>(layerWindow.extent().width()),
            static_cast<std::uint32_t>(layerWindow.extent().height()),
            render::PathFillRule::NonZero, layer.stroke, cancel);
        if (!built)
            return failed();
        geometry =
            std::make_shared<const render::PathRasterCoverageGeometry>(std::move(*built.value()));
        if (coverageCache != nullptr)
            coverageCache->store(geometryKeyDigest, geometry);
        return std::nullopt;
    };
    return emitSubtitlesLayers(frame, layerWindow, fullDisplayWindow, fullPixelAspect, opacity,
                               false, operationIndex, geometryOf, emit, charge, chargeCoverage,
                               composed, semanticKey);
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_SUBTITLES_EMISSION_HPP
