#ifndef BLOOM_RUNTIME_GPU_SCENE_EFFECT_EMISSION_HPP
#define BLOOM_RUNTIME_GPU_SCENE_EFFECT_EMISSION_HPP

// Private to src/runtime. The image-effect scene-command emission the shared builder in
// cpu_gpu_scene_preparation.cpp delegates to.
//
// CPU semantics reproduced here (see detail::ImageEffectContext / applyImageEffect):
//  * bypass, an IdentityImageKernel, a look-tagged effect under request.bypassLookNodes, a CST
//  whose
//    resolved from/to ids are equal, a file transform the CPU would refuse (missing/invalid LUT
//    asset, unreadable bytes, a changed digest, or an unbuildable/identity CPU LUT processor), and
//    an effect whose input publishes no image are all EXACT identity: the output aliases the input
//    command, key, window and bounds, and a deferred vector chain propagates unchanged.
//  * a non-identity CST emits one real GPU OCIO ProcessEffect command. A file transform emits the
//    CPU's exact chain -- working -> process CST, the LUT, process -> working CST -- with the CST
//    legs omitted when the process space is the working space. If its input is a deferred vector
//    leaf, that leaf is first materialized as the accepted coverage command the CPU leaf arm would
//    rasterize, so no vector leaf is ever silently dropped.
//  * any preparation refusal other than a genuine identity fails closed (Unsupported) so the caller
//    takes the existing CPU reference path; it is never substituted with an identity result.

#include "canvas_image_effect.hpp"
#include "channel_image_effect.hpp"
#include "color_grade_image_effect.hpp"
#include "directional_blur_image_effect.hpp"
#include "depth_effect_image_effect.hpp"
#include "lens_flare_image_effect.hpp"
#include "film_grain_image_effect.hpp"
#include "motion_blur_image_effect.hpp"
#include "sharpen_image_effect.hpp"
#include "codec_degrade_image_effect.hpp"
#include "auto_exposure_image_effect.hpp"
#include "pixel_math_image_effect.hpp"
#include "histogram_image_effect.hpp"
#include "statistics_image_effect.hpp"
#include "audio_plot_image_effect.hpp"
#include "discrete_image_effect.hpp"
#include "field_raster.hpp"
#include "gpu_scene_layer_emission.hpp"
#include "gpu_scene_preparation_common.hpp"
#include "gpu_scene_vector_emission.hpp"
#include "input_color_context.hpp"
#include "layer_parent_transform.hpp"
#include "luma_key_image_effect.hpp"
#include "pointwise_image_effect.hpp"
#include "resize_fit_geometry.hpp"
#include "separable_image_effect.hpp"
#include "transform_2d_geometry.hpp"

#include "layer_mask_sampling.hpp"
#include <bloom/media/image.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace bloom::runtime::detail {

// The evaluator's vector chain: a solid/text/shape leaf, the composed author-space matrix every
// enclosing layer multiplied into it, and the accumulated layer opacity. Defined here so the effect
// emission can materialize a leaf and the builder orchestrator can propagate a chain through an
// identity effect.
struct GpuSceneVectorChain final {
    std::size_t source = 0;
    LayerMatrix matrix;
    double opacity = 1.0;
};

inline constexpr std::size_t kNoVectorLeaf = std::numeric_limits<std::size_t>::max();

// The descriptor a command's output image carries, derived from the already-emitted scene alone.
// Mirrors the executor's expectedDescriptorOf so a produced OCIO effect command is validated
// against the exact input geometry at execution time.
struct GpuSceneCommandDescriptor final {
    render::ImageWindow data;
    render::ImageWindow display;
    core::PixelAspectRatio pixelAspect = core::PixelAspectRatio::square();
};

[[nodiscard]] inline std::optional<GpuSceneCommandDescriptor>
gpuSceneCommandDescriptorOf(const std::vector<GpuSceneCommand>& commands,
                            const GpuSceneCommandIndex index, const std::size_t depth) {
    if (index == kInvalidGpuSceneCommand || static_cast<std::size_t>(index) >= commands.size() ||
        depth > 64) {
        return std::nullopt;
    }
    return std::visit(
        [&](const auto& item) -> std::optional<GpuSceneCommandDescriptor> {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, GpuSceneAffineCommand>) {
                if (item.outputDescriptor)
                    return GpuSceneCommandDescriptor{item.outputWindow,
                                                     item.outputDescriptor->displayWindow(),
                                                     item.outputDescriptor->pixelAspect()};
                const auto source = gpuSceneCommandDescriptorOf(commands, item.input, depth + 1);
                if (!source)
                    return std::nullopt;
                return GpuSceneCommandDescriptor{item.outputWindow, source->display,
                                                 source->pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneTranslationCommand>) {
                const auto source = gpuSceneCommandDescriptorOf(commands, item.input, depth + 1);
                if (!source.has_value()) {
                    return std::nullopt;
                }
                return GpuSceneCommandDescriptor{
                    item.outputWindow,
                    [&]() {
                        if constexpr (std::is_same_v<T, GpuSceneTranslationCommand>)
                            return item.outputDisplayWindow.value_or(source->display);
                        return source->display;
                    }(),
                    source->pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneBlendCommand>) {
                const auto backdrop =
                    gpuSceneCommandDescriptorOf(commands, item.destination, depth + 1);
                if (!backdrop.has_value()) {
                    return std::nullopt;
                }
                return GpuSceneCommandDescriptor{item.outputWindow, backdrop->display,
                                                 backdrop->pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneOcioEffectCommand> ||
                                 std::is_same_v<T, GpuScenePointResampleCommand> ||
                                 std::is_same_v<T, GpuSceneCryptomatteCommand> ||
                                 std::is_same_v<T, GpuSceneMergeCommand> ||
                                 std::is_same_v<T, GpuSceneCoverageSolidCommand>) {
                return GpuSceneCommandDescriptor{item.outputWindow, item.displayWindow,
                                                 item.pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneCompositionOutputCommand> ||
                                 std::is_same_v<T, GpuSceneSolidCommand>) {
                return GpuSceneCommandDescriptor{item.dataWindow, item.displayWindow,
                                                 item.pixelAspect};
            } else if constexpr (std::is_same_v<T, GpuSceneUploadCommand>) {
                return GpuSceneCommandDescriptor{item.descriptor.dataWindow(),
                                                 item.descriptor.displayWindow(),
                                                 item.descriptor.pixelAspect()};
            } else {
                return std::nullopt;
            }
        },
        commands[index]);
}

// The builder state a single effect emission reads. Every pointer refers to the live build a
// cpu_gpu_scene_preparation.cpp loop owns; nothing here is shared or retained.
struct GpuSceneEffectEmissionContext final {
    const CompiledCompositionPlan* plan = nullptr;
    const EvaluationRequest* request = nullptr;
    const ResolvedEvaluation* resolved = nullptr;
    // ADR 0024: the frame (clock 0) resolution, so a vector leaf resolves on its own clock.
    const ResolvedEvaluation* frameResolved = nullptr;
    const std::vector<std::optional<GpuSceneVectorChain>>* vectors = nullptr;
    const std::vector<GpuSceneCommandIndex>* commandForOperation = nullptr;
    const std::vector<std::string>* keyOf = nullptr;
    const std::vector<std::optional<render::ImageWindow>>* outputWindowOf = nullptr;
    const std::vector<EvaluatedOperationBounds>* bounds = nullptr;
    const std::vector<GpuSceneCommand>* commands = nullptr;
    render::ImageWindow fullDisplayWindow;
    core::PixelAspectRatio fullPixelAspect = core::PixelAspectRatio::square();
    double hScale = 1.0;
    double vScale = 1.0;
    std::uint64_t allowance = 0;
    std::filesystem::path assetBaseDirectory;
    std::shared_ptr<GpuSceneCoverageCache> coverageCache;
};

struct GpuSceneEffectEmission final {
    // True when the CPU evaluation of this effect is exact identity: the caller aliases the input
    // command/key/window/bounds and propagates the deferred vector chain.
    bool identity = false;
    GpuSceneCommandIndex command = kInvalidGpuSceneCommand;
    std::string key;
    std::optional<render::ImageWindow> window;
    ContentBounds local;
    ContentBounds output;
    // The vector leaf a materialization consumed, or kNoVectorLeaf when the input was already a
    // resident command.
    std::size_t consumedLeaf = kNoVectorLeaf;
    bool consumedText = false;
    bool consumedShape = false;
    std::size_t consumedSecondLeaf = kNoVectorLeaf;
    bool consumedSecondText = false;
    bool consumedSecondShape = false;
    // #525: the further leaves a field raster's images materialized.
    struct ConsumedLeaf final {
        std::size_t leaf = kNoVectorLeaf;
        bool text = false;
        bool shape = false;
    };
    std::vector<ConsumedLeaf> consumedFieldLeaves;
};

#include "gpu_scene_resize_fit_emission.ipp"
#include "gpu_scene_transform_2d_emission.ipp"
#include "gpu_scene_field_emission.ipp"
#include "gpu_scene_depth_emission.ipp"
#include "gpu_scene_lens_flare_emission.ipp"
#include "gpu_scene_motion_emission.ipp"
#include "gpu_scene_sharpen_emission.ipp"
#include "gpu_scene_codec_degrade_emission.ipp"
#include "gpu_scene_auto_exposure_emission.ipp"
#include "gpu_scene_pixel_math_emission.ipp"
#include "gpu_scene_histogram_emission.ipp"
#include "gpu_scene_statistics_emission.ipp"
#include "gpu_scene_audio_plot_emission.ipp"

// Emits the scene command(s) for one reachable CompiledImageEffect. Returns nullopt on success and
// a fail-closed failure otherwise.
template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure>
emitImageEffectCommand(const CompiledImageEffect& effect, const OperationIndex operationIndex,
                       const GpuSceneEffectEmissionContext& ctx,
                       const GpuSceneOcioContext& ocioContext,
                       const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
                       ChargeCoverage&& chargeCoverage, GpuSceneEffectEmission& result) {
    const auto& plan = *ctx.plan;
    const auto& request = *ctx.request;
    const auto& resolved = *ctx.resolved;
    const auto leafResolution = [&](const std::size_t source) -> const ResolvedEvaluation& {
        return ctx.frameResolved == nullptr
                   ? resolved
                   : resolvedForClock(*ctx.frameResolved,
                                      plan.operationClock(OperationIndex::fromRaw(source)));
    };
    const auto& vectors = *ctx.vectors;
    const auto& commandForOperation = *ctx.commandForOperation;
    const auto& keyOf = *ctx.keyOf;
    const auto& outputWindowOf = *ctx.outputWindowOf;
    const auto& bounds = *ctx.bounds;
    const auto& commands = *ctx.commands;

    if (effect.input.value() >= plan.operations().size()) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Image effect input is invalid");
    }
    const std::size_t inputValue = effect.input.value();
    const auto identity = [&result]() {
        result.identity = true;
        return std::optional<GpuSceneLeafFailure>{};
    };

    // Exact CPU identity cases.
    // #484: a time sample's pixels are its input's (evaluated on its own clock).
    if (effect.bypass || (request.bypassLookNodes && effect.look) ||
        std::holds_alternative<IdentityImageKernel>(effect.kernel) ||
        std::holds_alternative<TimeSampleKernel>(effect.kernel)) {
        return identity();
    }
    if (const auto* discrete = std::get_if<DiscreteImageKernel>(&effect.kernel)) {
        if (discrete->quarterTurns > 3)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Invalid quarter turn");
        const auto inputWindow = outputWindowOf[inputValue];
        if (discrete->identity() || !inputWindow)
            return identity();
        // The CPU lattice supports int64 origins. The affine map is Float64 host metadata;
        // outside this exact-integer domain, use the explicit CPU fallback rather than round.
        constexpr std::int64_t exactOriginLimit = std::int64_t{1} << 52;
        if (inputWindow->originX() < -exactOriginLimit ||
            inputWindow->originY() < -exactOriginLimit ||
            inputWindow->maxXExclusive() > exactOriginLimit ||
            inputWindow->maxYExclusive() > exactOriginLimit)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedTransform,
                        "Discrete image origin exceeds exact GPU affine coordinates");
        const auto window = discreteOutputWindow(*discrete, *inputWindow, ctx.hScale, ctx.vScale);
        if (!window)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedTransform,
                        "Discrete image output window overflows");
        GpuSceneCommandIndex inputCommand = commandForOperation[inputValue];
        std::string inputKey = keyOf[inputValue];
        if (inputCommand == kInvalidGpuSceneCommand) {
            const auto& chain = vectors[inputValue];
            if (!chain || chain->source >= plan.operations().size())
                return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "Discrete image input cannot be materialized");
            if (const auto error = emitVectorLeafLayer(
                    plan.operations()[chain->source], plan, leafResolution(chain->source),
                    chain->matrix, *inputWindow, ctx.fullDisplayWindow, ctx.fullPixelAspect,
                    ctx.hScale, ctx.vScale, chain->opacity, false, ctx.allowance, operationIndex,
                    ctx.coverageCache, cancellation, emit, charge, chargeCoverage, inputCommand,
                    inputKey, result.consumedText, result.consumedShape))
                return error;
            result.consumedLeaf = chain->source;
        }
        const auto matrix = discreteAffineMatrix(*discrete, *inputWindow, ctx.hScale, ctx.vScale);
        result.key =
            makeGpuSceneAffineSemanticKey(inputKey, *inputWindow, matrix, 1.0F, *window.value(),
                                          ctx.fullPixelAspect, std::string{kGpuAffineSpirvSha256});
        if (const auto error = charge(window.value()->extent().width(),
                                      window.value()->extent().height(), sizeof(render::Rgba32f)))
            return error;
        result.command = emit(GpuSceneAffineCommand{.sourceOperation = operationIndex,
                                                    .input = inputCommand,
                                                    .inputKey = inputKey,
                                                    .sourceWindow = *inputWindow,
                                                    .outputWindow = *window.value(),
                                                    .matrix = matrix,
                                                    .opacity = 1.0F,
                                                    .pixelAspect = ctx.fullPixelAspect,
                                                    .semanticKey = result.key});
        result.window = *window.value();
        result.local = {static_cast<double>(window.value()->originX()) / ctx.hScale,
                        static_cast<double>(window.value()->originY()) / ctx.vScale,
                        static_cast<double>(window.value()->maxXExclusive()) / ctx.hScale,
                        static_cast<double>(window.value()->maxYExclusive()) / ctx.vScale};
        result.output = result.local;
        return std::nullopt;
    }
    if (const auto* kernel = std::get_if<ResizeFitKernel>(&effect.kernel))
        return emitResizeFitCommand(*kernel, effect.input, operationIndex, ctx, cancellation, emit,
                                    charge, chargeCoverage, result);
    if (const auto* kernel = std::get_if<Transform2DKernel>(&effect.kernel))
        return emitTransform2DCommand(*kernel, effect.input, operationIndex, ctx, cancellation,
                                      emit, charge, chargeCoverage, result);
    // #525 Fields: a mask's signed distance, and a field raster with its images.
    if (const auto* distance = std::get_if<MaskDistanceKernel>(&effect.kernel))
        return emitMaskDistanceCommand(effect, *distance, operationIndex, ctx, ocioContext,
                                       cancellation, emit, charge, chargeCoverage, result);
    if (std::holds_alternative<FieldRasterKernel>(effect.kernel)) {
        const auto values = sampleColorGradeOperands(
            effect.kernel, plan, resolved,
            timeForClock(ctx.frameResolved != nullptr ? *ctx.frameResolved : resolved,
                         request.time, plan.operationClock(operationIndex)));
        const auto grade = values ? prepareColorGrade(effect.kernel, *values) : std::nullopt;
        if (!grade)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Field values are out of domain");
        return emitFieldRasterCommand(effect, *grade, operationIndex, ctx, ocioContext,
                                      cancellation, emit, charge, chargeCoverage, result);
    }
    // #483 Depth effects: a depth map over its depth image, a defocus gather over its atlas.
    if (std::holds_alternative<DepthMapKernel>(effect.kernel) ||
        std::holds_alternative<DefocusKernel>(effect.kernel)) {
        const auto values = sampleColorGradeOperands(
            effect.kernel, plan, resolved,
            timeForClock(ctx.frameResolved != nullptr ? *ctx.frameResolved : resolved,
                         request.time, plan.operationClock(operationIndex)));
        const auto grade = values ? prepareColorGrade(effect.kernel, *values) : std::nullopt;
        if (!grade)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Depth effect values are out of domain");
        if (const auto* defocus = std::get_if<DefocusKernel>(&effect.kernel)) {
            // No blur anywhere aliases the input, as on the CPU reference.
            if (grade->identity) {
                result.identity = true;
                return std::nullopt;
            }
            return emitDefocusCommand(effect, *defocus, *grade, operationIndex, ctx, ocioContext,
                                      cancellation, emit, charge, chargeCoverage, result);
        }
        return emitDepthMapCommand(effect, *grade, operationIndex, ctx, ocioContext, cancellation,
                                   emit, charge, chargeCoverage, result);
    }
    // #563 Codec Degrade: one pass of the codec imitation over its images.
    if (std::holds_alternative<CodecDegradeKernel>(effect.kernel)) {
        const auto values = sampleColorGradeOperands(
            effect.kernel, plan, resolved,
            timeForClock(ctx.frameResolved != nullptr ? *ctx.frameResolved : resolved,
                         request.time, plan.operationClock(operationIndex)));
        const auto grade = values ? prepareColorGrade(effect.kernel, *values) : std::nullopt;
        if (!grade)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Codec Degrade values are out of domain");
        // A step after the current frame aliases its input, as on the CPU reference.
        if (grade->identity) {
            result.identity = true;
            return std::nullopt;
        }
        return emitCodecDegradeCommand(effect, *grade, operationIndex, ctx, ocioContext,
                                       cancellation, emit, charge, chargeCoverage, result);
    }
    // #566 Auto Exposure: one pass of the reduction, the filter or the gain.
    if (std::holds_alternative<AutoExposureKernel>(effect.kernel)) {
        const auto values = sampleColorGradeOperands(
            effect.kernel, plan, resolved,
            timeForClock(ctx.frameResolved != nullptr ? *ctx.frameResolved : resolved,
                         request.time, plan.operationClock(operationIndex)));
        const auto grade = values ? prepareColorGrade(effect.kernel, *values) : std::nullopt;
        if (!grade)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Auto Exposure values are out of domain");
        return emitAutoExposureCommand(effect, *grade, operationIndex, ctx, ocioContext,
                                       cancellation, emit, charge, chargeCoverage, result);
    }
    // #349 Image Statistics: one pass of the tiles' reductions or their combination.
    if (std::holds_alternative<StatisticsKernel>(effect.kernel)) {
        const auto grade = prepareColorGrade(effect.kernel, {});
        if (!grade)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Image Statistics values are out of domain");
        return emitStatisticsCommand(effect, *grade, operationIndex, ctx, ocioContext, cancellation,
                                     emit, charge, chargeCoverage, result);
    }
    // #349 Histogram: one pass of the tile counts, their sums or the plot.
    if (std::holds_alternative<HistogramKernel>(effect.kernel)) {
        const auto values = sampleColorGradeOperands(
            effect.kernel, plan, resolved,
            timeForClock(ctx.frameResolved != nullptr ? *ctx.frameResolved : resolved,
                         request.time, plan.operationClock(operationIndex)));
        const auto grade = values ? prepareColorGrade(effect.kernel, *values) : std::nullopt;
        if (!grade)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Histogram values are out of domain");
        return emitHistogramCommand(effect, *grade, operationIndex, ctx, ocioContext, cancellation,
                                    emit, charge, chargeCoverage, result);
    }
    // #347 Audio Waveform and Spectrum: the analysis is read and reduced here, on the worker, then
    // one pass draws it over the canvas.
    if (const auto* plot = std::get_if<AudioPlotKernel>(&effect.kernel)) {
        const auto plotTime =
            timeForClock(ctx.frameResolved != nullptr ? *ctx.frameResolved : resolved,
                         request.time, plan.operationClock(operationIndex));
        const auto operands = sampleColorGradeOperandsOnly(effect.kernel, plan, resolved, plotTime);
        if (!operands)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Audio plot values are out of domain");
        const auto prepared = prepareAudioPlotAnalysis(*plot, *operands, plotTime,
                                                       ctx.assetBaseDirectory, cancellation);
        if (prepared.outcome == AudioAnalysisOutcome::Cancelled)
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                        "Audio plot preparation was cancelled");
        if (prepared.outcome == AudioAnalysisOutcome::Unavailable)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "The audio this plot draws is unavailable: " + prepared.detail);
        const auto values = sampleColorGradeOperands(effect.kernel, plan, resolved, plotTime);
        const auto grade = values ? prepareColorGrade(effect.kernel, *values) : std::nullopt;
        if (!grade)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Audio plot values are out of domain");
        return emitAudioPlotCommand(effect, *grade, operationIndex, ctx, ocioContext, cancellation,
                                    emit, charge, chargeCoverage, result);
    }
    // #481 Image Math with a second image: one dispatch over an atlas of the input and the second.
    if (const auto* pixelMath = std::get_if<PixelMathKernel>(&effect.kernel);
        pixelMath != nullptr && pixelMath->second) {
        const auto values = sampleColorGradeOperands(
            effect.kernel, plan, resolved,
            timeForClock(ctx.frameResolved != nullptr ? *ctx.frameResolved : resolved,
                         request.time, plan.operationClock(operationIndex)));
        const auto grade = values ? prepareColorGrade(effect.kernel, *values) : std::nullopt;
        if (!grade)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Image pixel math values are out of domain");
        return emitPixelMathSecondCommand(effect, *grade, operationIndex, ctx, ocioContext,
                                          cancellation, emit, charge, chargeCoverage, result);
    }
    // #486 Sharpen: one two-input dispatch over the input and its blur.
    if (std::holds_alternative<SharpenKernel>(effect.kernel)) {
        const auto values = sampleColorGradeOperands(
            effect.kernel, plan, resolved,
            timeForClock(ctx.frameResolved != nullptr ? *ctx.frameResolved : resolved,
                         request.time, plan.operationClock(operationIndex)));
        const auto grade = values ? prepareColorGrade(effect.kernel, *values) : std::nullopt;
        if (!grade)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Sharpen values are out of domain");
        // No amount or no radius aliases the input, as on the CPU reference.
        if (grade->identity) {
            result.identity = true;
            return std::nullopt;
        }
        return emitSharpenCommand(effect, *grade, operationIndex, ctx, cancellation, emit, charge,
                                  chargeCoverage, result);
    }
    // #507 Lens Flare: its probes and its stack over the input padded to the frame.
    if (std::holds_alternative<LensFlareKernel>(effect.kernel)) {
        const auto values = sampleColorGradeOperands(
            effect.kernel, plan, resolved,
            timeForClock(ctx.frameResolved != nullptr ? *ctx.frameResolved : resolved,
                         request.time, plan.operationClock(operationIndex)));
        const auto grade = values ? prepareColorGrade(effect.kernel, *values) : std::nullopt;
        if (!grade)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Lens Flare values are out of domain");
        // No light anywhere aliases the input, as on the CPU reference.
        if (grade->identity) {
            result.identity = true;
            return std::nullopt;
        }
        return emitLensFlareCommand(effect, *grade, operationIndex, ctx, ocioContext, cancellation,
                                    emit, charge, chargeCoverage, result);
    }
    // #484 Motion blur: every motion kernel's pass plan.
    if (isMotionKernel(effect.kernel)) {
        const auto values = sampleColorGradeOperands(
            effect.kernel, plan, resolved,
            timeForClock(ctx.frameResolved != nullptr ? *ctx.frameResolved : resolved,
                         request.time, plan.operationClock(operationIndex)));
        const auto grade = values ? prepareColorGrade(effect.kernel, *values) : std::nullopt;
        if (!grade)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Motion blur values are out of domain");
        // No blur anywhere aliases the input, as on the CPU reference.
        if (grade->identity)
            return identity();
        return emitMotionCommand(effect, *grade, operationIndex, ctx, ocioContext, cancellation,
                                 emit, charge, chargeCoverage, result);
    }
    const auto* cst = std::get_if<CstKernel>(&effect.kernel);
    const auto* file = std::get_if<FileTransformKernel>(&effect.kernel);
    const auto* pointwise = std::get_if<PointwiseImageKernel>(&effect.kernel);
    const auto* combine = std::get_if<ImageCombineKernel>(&effect.kernel);
    // #479 Combine Channels: the base with one slot set from the second image, on the same
    // resident two-input ImageCombineV1 dispatch (its Set Channel mode).
    const auto* channelCombine = std::get_if<ChannelCombineKernel>(&effect.kernel);
    const auto preparedChannel =
        channelCombine ? prepareChannelCombine(*channelCombine) : std::nullopt;
    if (channelCombine != nullptr && !preparedChannel)
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Channel combine slot or source is out of domain");
    if (combine != nullptr || channelCombine != nullptr) {
        // The same closed selector the CPU reference prepares (image_effect.cpp): a driver samples
        // the whole stored enumeration, a constant arrives split into channel and invert.
        std::optional<render::MatteSelection> selection;
        if (preparedChannel) {
            selection = render::MatteSelection{preparedChannel->source, false};
        } else if (combine->drivenChannel) {
            const auto* sampled = resolvedValue<std::int64_t>(*combine->drivenChannel, resolved);
            if (sampled == nullptr)
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                            "Image combine channel driver is not an Integer");
            selection = render::matteSelectionFromStoredValue(*sampled);
        } else if (static_cast<std::uint8_t>(combine->channel) <=
                   static_cast<std::uint8_t>(render::ImageCoverageChannel::Luma)) {
            selection = render::MatteSelection{combine->channel, combine->invert};
        }
        if (!effect.secondaryInput || effect.secondaryInput->value() >= plan.operations().size() ||
            (combine != nullptr &&
             static_cast<std::uint8_t>(combine->mode) >
                 static_cast<std::uint8_t>(ImageCombineMode::MultiplyAlpha)) ||
            !selection)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Image combine kernel or second input is invalid");
        const auto secondValue = effect.secondaryInput->value();
        if (!outputWindowOf[inputValue])
            return identity();
        // #361: a second input with no image (a matte Layer outside its range, an unconnected or
        // empty matte) is a transparent matte, as on the CPU reference. The kernel then binds the
        // base image in the matte slot and declares an empty matte extent, so every tap is
        // transparent black; it never reads a matte texel and never falls back to the CPU.
        const bool matteAbsent = !outputWindowOf[secondValue];
        float factor = 1.0F;
        if (combine == nullptr) {
            // Set Channel has no factor.
        } else if (combine->factor) {
            const auto sampled = resolveParameter(*combine->factor, plan, resolved);
            if (!sampled || !std::isfinite(sampled->value) || sampled->value < 0.0 ||
                sampled->value > 1.0)
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                            "Image combine factor is outside its unit domain");
            factor = static_cast<float>(sampled->value);
        } else if (combine->mode != ImageCombineMode::SetAlpha) {
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Image combine factor is missing");
        }
        if (combine != nullptr && combine->mode == ImageCombineMode::Mix && factor == 0.0F)
            return identity();

        // Both image inputs may be deferred vector leaves. Materialize each through the same
        // native coverage route the ordinary image-effect path uses before the two-input dispatch.
        const auto residentInput = [&](const std::size_t value, GpuSceneCommandIndex& command,
                                       std::string& key, std::size_t& consumedLeaf,
                                       bool& consumedText,
                                       bool& consumedShape) -> std::optional<GpuSceneLeafFailure> {
            command = commandForOperation[value];
            key = keyOf[value];
            if (command != kInvalidGpuSceneCommand)
                return std::nullopt;
            const auto& chain = vectors[value];
            if (!chain || chain->source >= plan.operations().size())
                return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "Image combine vector input is not materializable");
            const auto error = emitVectorLeafLayer(
                plan.operations()[chain->source], plan, leafResolution(chain->source),
                chain->matrix, *outputWindowOf[value], ctx.fullDisplayWindow, ctx.fullPixelAspect,
                ctx.hScale, ctx.vScale, chain->opacity, false, ctx.allowance, operationIndex,
                ctx.coverageCache, cancellation, emit, charge, chargeCoverage, command, key,
                consumedText, consumedShape);
            if (!error)
                consumedLeaf = chain->source;
            return error;
        };
        GpuSceneCommandIndex baseCommand = kInvalidGpuSceneCommand;
        GpuSceneCommandIndex secondCommand = kInvalidGpuSceneCommand;
        std::string baseKey;
        std::string secondKey;
        if (const auto error = residentInput(inputValue, baseCommand, baseKey, result.consumedLeaf,
                                             result.consumedText, result.consumedShape))
            return error;
        if (matteAbsent) {
            secondCommand = baseCommand;
            secondKey = baseKey;
        } else if (const auto error = residentInput(
                       secondValue, secondCommand, secondKey, result.consumedSecondLeaf,
                       result.consumedSecondText, result.consumedSecondShape)) {
            return error;
        }
        // ADR 0028: Image Mix's Mask is a third resident input (the Mask node's masked canvas); an
        // input with no image is a zero mask, as on the CPU reference.
        auto maskMode = render::GpuImageCombineParameters::Mask::None;
        GpuSceneCommandIndex maskCommand = kInvalidGpuSceneCommand;
        std::string maskKey;
        std::optional<render::ImageWindow> maskWindow;
        if (effect.maskInput) {
            const auto maskValue = effect.maskInput->value();
            if (combine == nullptr || combine->mode != ImageCombineMode::Mix ||
                maskValue >= plan.operations().size())
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                            "Image combine mask is invalid");
            if (!outputWindowOf[maskValue]) {
                maskMode = render::GpuImageCombineParameters::Mask::Absent;
            } else {
                maskCommand = commandForOperation[maskValue];
                maskKey = keyOf[maskValue];
                if (maskCommand == kInvalidGpuSceneCommand)
                    return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                                "Image Mix mask is not a resident image");
                const auto maskDescriptor =
                    gpuSceneCommandDescriptorOf(*ctx.commands, maskCommand, 0);
                if (!maskDescriptor)
                    return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                                "Image Mix mask descriptor is not derivable");
                maskWindow = maskDescriptor->data;
                maskMode = render::GpuImageCombineParameters::Mask::Present;
            }
        }
        const auto baseDescriptor = gpuSceneCommandDescriptorOf(*ctx.commands, baseCommand, 0);
        const auto secondDescriptor = gpuSceneCommandDescriptorOf(*ctx.commands, secondCommand, 0);
        if (!baseDescriptor || !secondDescriptor)
            return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                        "Image combine resident descriptors are not derivable");
        const auto outputWindow = baseDescriptor->data;
        if (const auto error = charge(outputWindow.extent().width(), outputWindow.extent().height(),
                                      sizeof(render::Rgba32f)))
            return error;
        render::GpuImageCombineParameters parameters{
            combine != nullptr ? static_cast<render::GpuImageCombineKind>(combine->mode)
                               : render::GpuImageCombineKind::SetChannel,
            factor,
            selection->channel,
            selection->invert,
            matteAbsent,
            maskMode};
        if (preparedChannel) {
            parameters.slot = preparedChannel->slot;
            parameters.premultiply = preparedChannel->premultiply;
        }
        GpuSceneBlendCommand command{.sourceOperation = operationIndex,
                                     .source = secondCommand,
                                     .destination = baseCommand,
                                     .sourceKey = secondKey,
                                     .destinationKey = baseKey,
                                     .mode = core::BlendMode::Normal,
                                     .imageCombine = parameters,
                                     .mask = maskCommand,
                                     .maskKey = maskKey,
                                     .maskWindow = maskWindow,
                                     .sourceWindow = secondDescriptor->data,
                                     .outputWindow = outputWindow,
                                     .pixelAspect = baseDescriptor->pixelAspect,
                                     .artifactDigest =
                                         std::string(render::gpuImageCombineShaderIdentity()),
                                     .semanticKey = {}};
        command.semanticKey = makeGpuSceneBlendSemanticKey(
            secondKey, baseKey, command.mode, command.sourceWindow, command.outputWindow,
            command.pixelAspect, command.artifactDigest, command.imageCombine, command.maskKey,
            command.maskWindow);
        result.command = emit(std::move(command));
        result.key = makeGpuSceneBlendSemanticKey(
            secondKey, baseKey, core::BlendMode::Normal, secondDescriptor->data, outputWindow,
            baseDescriptor->pixelAspect, std::string(render::gpuImageCombineShaderIdentity()),
            parameters, maskKey, maskWindow);
        result.window = outputWindow;
        // Mix can add pixels where only the second image has content; Set Alpha can assign
        // coverage to a transparent base pixel. Use the whole base data window as conservative
        // content bounds so downstream ROI never silently drops those pixels.
        result.local = boundsForWindow(outputWindow, ctx.hScale, ctx.vScale);
        result.output = result.local;
        return std::nullopt;
    }
    const auto* canvas = std::get_if<CanvasImageKernel>(&effect.kernel);
    const auto* blurKernel = std::get_if<SeparableBlurKernel>(&effect.kernel);
    const auto* masksKernel = std::get_if<LayerMaskKernel>(&effect.kernel);
    const auto* lumaKeyKernel = std::get_if<LumaKeyKernel>(&effect.kernel);
    const auto* selectKernel = std::get_if<ChannelSelectKernel>(&effect.kernel);
    const auto* directionalKernel = std::get_if<DirectionalBlurKernel>(&effect.kernel);
    std::optional<PreparedSeparableBlur> blur;
    std::optional<PreparedDirectionalBlur> directional;
    // #505, #486: a Vignette's or Film Grain's table, whose geometry is the input's: its leg is
    // prepared once the input's descriptor is known (below).
    std::optional<PreparedColorGrade> positioned;
    if (cst == nullptr && file == nullptr && pointwise == nullptr && canvas == nullptr &&
        blurKernel == nullptr && masksKernel == nullptr && lumaKeyKernel == nullptr &&
        selectKernel == nullptr && directionalKernel == nullptr &&
        !isColorGradeKernel(effect.kernel)) {
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "An image effect kernel is not supported");
    }

    if (canvas != nullptr) {
        std::array<double, 4> sides{};
        for (std::size_t i = 0; i < sides.size(); ++i) {
            const auto sampled = resolveParameter(canvas->margins[i], plan, resolved);
            if (!sampled)
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                            "Canvas margin is not evaluable");
            sides[i] = sampled->value;
        }
        const CanvasMargins margins{canvas->effect, sides};
        if (margins.identity() || !outputWindowOf[inputValue])
            return identity();
        const auto sourceWindow = *outputWindowOf[inputValue];
        const auto outputWindow = canvasOutputWindow(sourceWindow, margins, ctx.hScale, ctx.vScale);
        if (!outputWindow)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "Canvas margins leave an invalid data window");
        GpuSceneCommandIndex inputCommand = commandForOperation[inputValue];
        std::string inputKey = keyOf[inputValue];
        if (inputCommand == kInvalidGpuSceneCommand) {
            const auto& chain = vectors[inputValue];
            if (!chain || chain->source >= plan.operations().size())
                return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "Canvas input vector chain is not materializable");
            if (const auto error = emitVectorLeafLayer(
                    plan.operations()[chain->source], plan, leafResolution(chain->source),
                    chain->matrix, sourceWindow, ctx.fullDisplayWindow, ctx.fullPixelAspect,
                    ctx.hScale, ctx.vScale, chain->opacity, false, ctx.allowance, operationIndex,
                    ctx.coverageCache, cancellation, emit, charge, chargeCoverage, inputCommand,
                    inputKey, result.consumedText, result.consumedShape))
                return error;
            result.consumedLeaf = chain->source;
        }
        const auto descriptor = gpuSceneCommandDescriptorOf(commands, inputCommand, 0);
        if (!descriptor || descriptor->data != sourceWindow)
            return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                        "Canvas input descriptor does not match its data window");
        const auto displayWindow =
            canvasOutputWindow(descriptor->display, margins, ctx.hScale, ctx.vScale);
        if (!displayWindow)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "Canvas margins leave an invalid display window");
        const double deltaX = static_cast<double>(sourceWindow.originX() - outputWindow->originX());
        const double deltaY = static_cast<double>(sourceWindow.originY() - outputWindow->originY());
        OperationKey commandKey;
        commandKey.add(std::string{"gpu-canvas-translation-v1"});
        commandKey.add(inputKey);
        addWindowToKey(commandKey, sourceWindow);
        addWindowToKey(commandKey, *outputWindow);
        addWindowToKey(commandKey, *displayWindow);
        addPixelAspectToKey(commandKey, descriptor->pixelAspect);
        commandKey.add(deltaX);
        commandKey.add(deltaY);
        commandKey.add(std::string{kGpuTranslationOpacitySpirvSha256});
        result.key = commandKey.digest();
        if (const auto error = charge(outputWindow->extent().width(),
                                      outputWindow->extent().height(), sizeof(render::Rgba32f)))
            return error;
        GpuSceneTranslationCommand command{.sourceOperation = operationIndex,
                                           .input = inputCommand,
                                           .sourceWindow = sourceWindow,
                                           .outputWindow = *outputWindow,
                                           .outputDisplayWindow = *displayWindow,
                                           .translationX = deltaX,
                                           .translationY = deltaY,
                                           .opacity = 1.0F,
                                           .semanticKey = result.key};
        result.command = emit(std::move(command));
        result.window = *outputWindow;
        result.local = boundsForWindow(*outputWindow, ctx.hScale, ctx.vScale);
        result.output = result.local;
        return std::nullopt;
    }

    const std::string working{request.colorIntent.workingColorSpaceId};
    const auto cancellationProbe = [&cancellation] {
        return cancellation.isCancellationRequested();
    };

    // Resolve the exact CPU chain for this kernel, short-circuiting every case the CPU treats as an
    // identity passthrough before any geometry or GPU work happens.
    std::vector<GpuOcioTransformSpec> legs;
    if (blurKernel != nullptr) {
        const auto sampled = resolveParameter(blurKernel->radius, plan, resolved);
        blur = prepareSeparableBlur(*blurKernel,
                                    sampled ? std::optional{sampled->value} : std::nullopt,
                                    ctx.hScale, ctx.vScale);
        if (!blur)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Blur radius is out of domain");
        if (blur->identity())
            return identity();
        for (unsigned axis = 0; axis < 2; ++axis) {
            GpuOcioTransformSpec leg;
            leg.kind = GpuOcioTransformKind::SeparableBlur;
            leg.blurWeights = axis == 0 ? blur->horizontal : blur->vertical;
            leg.blurVertical = axis == 1;
            leg.blurEdge = static_cast<std::uint8_t>(blur->edge);
            leg.blurCoverage = blur->channels == BlurChannels::Coverage;
            leg.blurKind = static_cast<std::uint8_t>(blur->kind);
            legs.push_back(std::move(leg));
        }
    } else if (directionalKernel != nullptr) {
        // #506: the CPU reference's prepared taps as the one fixed program's data, over the input
        // padded to the grown window (below). A zero spacing aliases its input, as on the CPU.
        const auto angle = resolveParameter(directionalKernel->angle, plan, resolved);
        const auto spacing = resolveParameter(directionalKernel->spacing, plan, resolved);
        directional = prepareDirectionalBlur(
            *directionalKernel, angle ? std::optional{angle->value} : std::nullopt,
            spacing ? std::optional{spacing->value} : std::nullopt, ctx.hScale, ctx.vScale);
        if (!directional)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Directional blur angle or spacing is out of domain");
        if (directional->identity)
            return identity();
        GpuOcioTransformSpec leg;
        leg.kind = GpuOcioTransformKind::DirectionalBlur;
        leg.directionalBlur = directionalBlurProgramData(*directional);
        legs.push_back(std::move(leg));
    } else if (isColorGradeKernel(effect.kernel)) {
        // #340: the CPU reference's prepared table as the kind's one fixed program's data; an
        // identity grade aliases its input exactly, as on the CPU.
        const auto values = sampleColorGradeOperands(
            effect.kernel, plan, resolved,
            timeForClock(ctx.frameResolved != nullptr ? *ctx.frameResolved : resolved,
                         request.time, plan.operationClock(operationIndex)));
        const auto grade = values ? prepareColorGrade(effect.kernel, *values) : std::nullopt;
        if (!grade)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Color grade values are out of domain");
        if (grade->identity)
            return identity();
        if (grade->kind == ColorGradeKind::Vignette || grade->kind == ColorGradeKind::FilmGrain) {
            positioned = std::move(*grade);
        } else {
            GpuOcioTransformSpec leg;
            leg.kind = GpuOcioTransformKind::ColorGrade;
            leg.gradeKind = static_cast<std::uint8_t>(grade->kind);
            leg.gradeData = grade->data;
            legs.push_back(std::move(leg));
        }
    } else if (selectKernel != nullptr) {
        // #479: the CPU reference's prepared mapping as the one fixed program's data; the identity
        // mapping aliases its input exactly, as on the CPU.
        const auto select = prepareChannelSelect(*selectKernel);
        if (!select)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Channel select sources are out of domain");
        if (select->identity)
            return identity();
        GpuOcioTransformSpec leg;
        leg.kind = GpuOcioTransformKind::ChannelSelect;
        leg.channelSelect = channelSelectProgramData(*select);
        legs.push_back(std::move(leg));
    } else if (lumaKeyKernel != nullptr) {
        // #338: the CPU reference's prepared key as the one fixed program's data. Never an
        // identity for an input with pixels; an input with no image stays empty (below).
        const auto low = resolveParameter(lumaKeyKernel->low, plan, resolved);
        const auto high = resolveParameter(lumaKeyKernel->high, plan, resolved);
        const auto key =
            prepareLumaKey(*lumaKeyKernel, low ? std::optional{low->value} : std::nullopt,
                           high ? std::optional{high->value} : std::nullopt);
        if (!key)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Luma Key thresholds are out of domain");
        GpuOcioTransformSpec leg;
        leg.kind = GpuOcioTransformKind::LumaKey;
        leg.lumaKey = lumaKeyProgramData(*key);
        legs.push_back(std::move(leg));
    } else if (masksKernel != nullptr) {
        // ADR 0028: the CPU reference's prepared geometry, uploaded as data to the one fixed
        // neighbourhood program that runs the same per-pixel coverage math on the resident input.
        // A keyed path changes only these bytes, so playback never recompiles.
        const auto time = timeForClock(ctx.frameResolved != nullptr ? *ctx.frameResolved : resolved,
                                       request.time, plan.operationClock(operationIndex));
        const auto prepared =
            sampleLayerMasks(*masksKernel, plan, resolved, time, ctx.hScale, ctx.vScale);
        if (!prepared)
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Layer mask parameters are not evaluable");
        if (prepared->masks.empty() || !outputWindowOf[inputValue])
            return identity();
        auto data = layerMaskProgramData(*prepared, outputWindowOf[inputValue]->originX(),
                                         outputWindowOf[inputValue]->originY());
        if (!data)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "Layer mask geometry exceeds the GPU program limits");
        GpuOcioTransformSpec leg;
        leg.kind = GpuOcioTransformKind::LayerMask;
        leg.maskData = std::move(*data);
        legs.push_back(std::move(leg));
    } else if (pointwise != nullptr) {
        std::optional<double> amount;
        if (pointwise->amount.has_value()) {
            const auto sampled = resolveParameter(*pointwise->amount, plan, resolved);
            if (!sampled) {
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                            "Pointwise effect amount is not evaluable");
            }
            amount = sampled->value;
        }
        const auto affine = pointwiseAffine(*pointwise, amount);
        if (!affine) {
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "Pointwise effect amount cannot be represented");
        }
        if (affine->identity()) {
            return identity();
        }
        GpuOcioTransformSpec leg;
        leg.kind = GpuOcioTransformKind::PointwiseAffine;
        leg.affineScale = affine->scale;
        leg.affineOffset = affine->offset;
        legs.push_back(std::move(leg));
    } else if (cst != nullptr) {
        const std::string from = cst->fromId.empty() ? working : cst->fromId;
        const std::string to = cst->toId.empty() ? working : cst->toId;
        if (from == to) {
            return identity();
        }
        GpuOcioTransformSpec leg;
        leg.kind = GpuOcioTransformKind::Cst;
        leg.fromId = from;
        leg.toId = to;
        legs.push_back(std::move(leg));
    } else {
        if (!file->asset || file->asset->kind != document::AssetKind::Lut ||
            file->asset->id != file->lutAssetId) {
            // The CPU refuses a missing or mismatched LUT asset with a warning passthrough.
            return identity();
        }
        const auto path = media::resolveImagePath(
            file->asset->locator.path, file->asset->locator.relinkHint, ctx.assetBaseDirectory);
        auto resource = color::readLutFile(path, cancellationProbe);
        if (resource.error == color::LutError::HelperCancelled) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                        "Image effect LUT read was cancelled");
        }
        if (resource.error != color::LutError::None ||
            resource.digest != file->asset->contentDigest) {
            // Unreadable bytes or a changed digest: the CPU refuses and passes the input through.
            return identity();
        }
        const auto interpolation = static_cast<color::LutInterpolation>(file->interpolation);
        const auto direction = static_cast<color::LutDirection>(file->direction);
        auto cpuLut = color::CpuFileTransformProcessor::prepare(resource, interpolation, direction,
                                                                cancellationProbe);
        if (cpuLut.error == color::LutError::HelperCancelled) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                        "Image effect LUT preparation was cancelled");
        }
        if (cpuLut.error != color::LutError::None || cpuLut.processor == nullptr) {
            // The CPU cannot build the LUT processor and passes the input through.
            return identity();
        }
        if (cpuLut.processor->isIdentity()) {
            // The CPU resets every surrounding processor for a proven identity LUT: exact identity.
            return identity();
        }
        const std::string process = file->processSpaceId.empty() ? working : file->processSpaceId;
        if (process != working) {
            GpuOcioTransformSpec inLeg;
            inLeg.kind = GpuOcioTransformKind::Cst;
            inLeg.fromId = working;
            inLeg.toId = process;
            legs.push_back(std::move(inLeg));
        }
        GpuOcioTransformSpec fileLeg;
        fileLeg.kind = GpuOcioTransformKind::FileTransform;
        fileLeg.lutFile = std::make_shared<const color::LutFile>(std::move(resource));
        fileLeg.interpolation = interpolation;
        fileLeg.direction = direction;
        fileLeg.processSpaceId = process;
        fileLeg.workingSpaceId = working;
        legs.push_back(std::move(fileLeg));
        if (process != working) {
            GpuOcioTransformSpec outLeg;
            outLeg.kind = GpuOcioTransformKind::Cst;
            outLeg.fromId = process;
            outLeg.toId = working;
            legs.push_back(std::move(outLeg));
        }
    }

    // A residual input publishes no image and the CPU effect is a no-op (identity).
    const auto inputWindow = outputWindowOf[inputValue];
    if (!inputWindow.has_value()) {
        return identity();
    }
    if (ocioContext.preparer == nullptr) {
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "No GPU OCIO preparer is configured for image effects");
    }
    const auto config = resolveInputColorConfig(request.colorIntent);
    if (!config.has_value()) {
        // The CPU refuses the transform and passes the input through; fail closed so the CPU path
        // reproduces that frame rather than substituting identity here.
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "The OCIO configuration for the image effect is unavailable");
    }
    const auto filterWindow = blur          ? blurOutputWindow(*blur, *inputWindow)
                              : directional ? directionalBlurOutputWindow(*directional, *inputWindow)
                                            : inputWindow;
    const bool grows = blur || directional;
    if (!filterWindow)
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation, "Blur halo overflows");
    const GpuOcioCommandGeometry geometry{
        .width = static_cast<std::uint32_t>(filterWindow->extent().width()),
        .height = static_cast<std::uint32_t>(filterWindow->extent().height())};
    std::vector<std::shared_ptr<const PreparedGpuOcioCommand>> programs;
    programs.reserve(legs.size() + 1);
    const auto prepareLeg =
        [&](const GpuOcioTransformSpec& leg) -> std::optional<GpuSceneLeafFailure> {
        auto prepared = ocioContext.preparer->prepare(
            *config, leg, geometry, ocioContext.compileOptions, cancellationProbe);
        if (!prepared) {
            if (prepared.error == GpuOcioPreparationError::CompileCancelled ||
                cancellation.isCancellationRequested()) {
                return fail(PreparedGpuSceneDiagnosticCode::Cancelled,
                            "GPU image-effect preparation was cancelled");
            }
            // A compiler that was busy, slow or could not start is not an unsupported operation:
            // the frame waits and retries on the GPU (#539).
            if (prepared.error == GpuOcioPreparationError::CompileUnavailable)
                return fail(PreparedGpuSceneDiagnosticCode::AwaitingCapacity,
                            "GPU image-effect program compile is unavailable; the frame waits");
            // Never substitute identity for a valid transform whose GPU program could not be built.
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "GPU image-effect preparation failed: " +
                            std::string{gpuOcioPreparationErrorName(prepared.error)});
        }
        programs.push_back(std::move(prepared.command));
        return std::nullopt;
    };
    for (const auto& leg : legs)
        if (const auto error = prepareLeg(leg))
            return error;
    if (programs.empty() && !positioned) {
        return identity();
    }

    // The exact resident input command: an already-emitted raster, or a deferred vector leaf that
    // must be materialized through the accepted coverage commands first.
    GpuSceneCommandIndex inputCommand = commandForOperation[inputValue];
    std::string inputKey = keyOf[inputValue];
    if (inputCommand == kInvalidGpuSceneCommand) {
        const auto& vectorChain = vectors[inputValue];
        if (!vectorChain.has_value()) {
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "A non-identity image effect input is not materializable");
        }
        const auto& chain = *vectorChain;
        if (chain.source >= plan.operations().size()) {
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "An image effect vector chain is invalid");
        }
        // Materialize the leaf at its own window with its chain matrix/opacity; nativeGrid is left
        // false so the general coverage arm handles every leaf kind without a layer transform.
        GpuSceneCommandIndex materialized = kInvalidGpuSceneCommand;
        std::string materializedKey;
        if (const auto error = emitVectorLeafLayer(
                plan.operations()[chain.source], plan, leafResolution(chain.source), chain.matrix,
                *inputWindow, ctx.fullDisplayWindow, ctx.fullPixelAspect, ctx.hScale, ctx.vScale,
                chain.opacity, false, ctx.allowance, operationIndex, ctx.coverageCache,
                cancellation, emit, charge, chargeCoverage, materialized, materializedKey,
                result.consumedText, result.consumedShape)) {
            return error;
        }
        result.consumedLeaf = chain.source;
        inputCommand = materialized;
        inputKey = materializedKey;
    }

    const auto descriptor = gpuSceneCommandDescriptorOf(commands, inputCommand, 0);
    if (!descriptor.has_value()) {
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "The image effect input descriptor is not derivable");
    }
    // #505, #486: the table with the input's own windows and pixel aspect, exactly as the CPU
    // reference reads them from its input image.
    if (positioned) {
        const auto aspect = descriptor->pixelAspect;
        const double pixelAspect =
            static_cast<double>(aspect.numerator()) / static_cast<double>(aspect.denominator());
        auto data = positioned->kind == ColorGradeKind::FilmGrain
                        ? filmGrainProgramData(*positioned, descriptor->data, pixelAspect,
                                               ctx.hScale, ctx.vScale)
                        : vignetteProgramData(*positioned, descriptor->display,
                                              descriptor->data.originX(),
                                              descriptor->data.originY(), pixelAspect, ctx.hScale,
                                              ctx.vScale);
        if (!data)
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        positioned->kind == ColorGradeKind::FilmGrain
                            ? "Film Grain geometry cannot be represented"
                            : "Vignette geometry cannot be represented");
        GpuOcioTransformSpec leg;
        leg.kind = GpuOcioTransformKind::ColorGrade;
        leg.gradeKind = static_cast<std::uint8_t>(positioned->kind);
        leg.gradeData = std::move(*data);
        if (const auto error = prepareLeg(leg))
            return error;
    }
    const auto outputWindow = grows ? *filterWindow : descriptor->data;
    if (grows && outputWindow != descriptor->data) {
        if (const auto error = charge(outputWindow.extent().width(), outputWindow.extent().height(),
                                      sizeof(render::Rgba32f)))
            return error;
        OperationKey paddingKey;
        paddingKey.add(inputKey);
        paddingKey.add(std::string("blur-transparent-halo-v1"));
        paddingKey.add(outputWindow.originX());
        paddingKey.add(outputWindow.originY());
        paddingKey.add(outputWindow.extent().width());
        paddingKey.add(outputWindow.extent().height());
        inputKey = paddingKey.bytes();
        inputCommand = emit(GpuSceneTranslationCommand{
            .index = kInvalidGpuSceneCommand,
            .sourceOperation = operationIndex,
            .input = inputCommand,
            .sourceWindow = descriptor->data,
            .outputWindow = outputWindow,
            .translationX =
                static_cast<double>(descriptor->data.originX() - outputWindow.originX()),
            .translationY =
                static_cast<double>(descriptor->data.originY() - outputWindow.originY()),
            .opacity = 1.0F,
            .semanticKey = inputKey});
    }

    // Emit the prepared legs as one resident RGBA32F chain. Each command's descriptor equals the
    // original input's, so every leg reuses `descriptor`.
    GpuSceneCommandIndex currentCommand = inputCommand;
    std::string currentKey = std::move(inputKey);
    for (auto& program : programs) {
        if (const auto error = charge(outputWindow.extent().width(), outputWindow.extent().height(),
                                      sizeof(render::Rgba32f))) {
            return error;
        }
        GpuSceneOcioEffectCommand command{.index = kInvalidGpuSceneCommand,
                                          .sourceOperation = operationIndex,
                                          .input = currentCommand,
                                          .inputKey = currentKey,
                                          .program = std::move(program),
                                          .outputWindow = outputWindow,
                                          .displayWindow = descriptor->display,
                                          .pixelAspect = descriptor->pixelAspect,
                                          .semanticKey = {}};
        command.semanticKey = makeGpuSceneOcioEffectSemanticKey(
            currentKey, command.program->identity(), command.outputWindow, command.pixelAspect);
        currentKey = command.semanticKey;
        currentCommand = emit(std::move(command));
    }
    result.command = currentCommand;
    result.key = std::move(currentKey);
    result.window = outputWindow;
    // A grade can lift transparent pixels inside the data window (an alpha transfer), so its
    // content bounds are that whole window, as on the CPU reference.
    // A Channel Select (#479) makes every pixel of the window opaque or reads alpha from colour.
    result.local = grows || isColorGradeKernel(effect.kernel) || selectKernel != nullptr
                       ? boundsForWindow(outputWindow, ctx.hScale, ctx.vScale)
                       : bounds[inputValue].output;
    result.output = result.local;
    return std::nullopt;
}

// Emits and wires one reachable CompiledImageEffect into the builder's per-operation state.
// Identity effects alias the input command/key/window/bounds and keep the deferred vector chain; a
// non-identity effect publishes its OCIO command chain and marks any materialized leaf consumed.
// Keeping this here leaves the builder orchestrator within its size budget.
template <typename Emit, typename Charge, typename ChargeCoverage>
[[nodiscard]] std::optional<GpuSceneLeafFailure> emitImageEffectOperation(
    const CompiledImageEffect& effect, const OperationIndex operationIndex,
    const GpuSceneEffectEmissionContext& ctx, const GpuSceneOcioContext& ocioContext,
    const CancellationToken& cancellation, Emit&& emit, Charge&& charge,
    ChargeCoverage&& chargeCoverage, std::vector<GpuSceneCommandIndex>& commandForOperation,
    std::vector<std::string>& keyOf,
    std::vector<std::optional<render::ImageWindow>>& outputWindowOf,
    std::vector<EvaluatedOperationBounds>& bounds,
    std::vector<std::optional<GpuSceneVectorChain>>& vectors, std::vector<bool>& textConsumed,
    std::vector<bool>& shapeConsumed) {
    GpuSceneEffectEmission emitted;
    if (const auto error =
            emitImageEffectCommand(effect, operationIndex, ctx, ocioContext, cancellation, emit,
                                   charge, chargeCoverage, emitted)) {
        return error;
    }
    const std::size_t index = operationIndex.value();
    const std::size_t input = effect.input.value();
    if (emitted.identity) {
        commandForOperation[index] = commandForOperation[input];
        keyOf[index] = keyOf[input];
        outputWindowOf[index] = outputWindowOf[input];
        bounds[index].local = bounds[input].output;
        bounds[index].output = bounds[index].local;
        vectors[index] = vectors[input];
        return std::nullopt;
    }
    commandForOperation[index] = emitted.command;
    keyOf[index] = emitted.key;
    outputWindowOf[index] = emitted.window;
    bounds[index].local = emitted.local;
    bounds[index].output = emitted.output;
    if (emitted.consumedLeaf != kNoVectorLeaf) {
        if (emitted.consumedText) {
            textConsumed[emitted.consumedLeaf] = true;
        }
        if (emitted.consumedShape) {
            shapeConsumed[emitted.consumedLeaf] = true;
        }
    }
    if (emitted.consumedSecondLeaf != kNoVectorLeaf) {
        if (emitted.consumedSecondText)
            textConsumed[emitted.consumedSecondLeaf] = true;
        if (emitted.consumedSecondShape)
            shapeConsumed[emitted.consumedSecondLeaf] = true;
    }
    for (const auto& consumed : emitted.consumedFieldLeaves) {
        if (consumed.text)
            textConsumed[consumed.leaf] = true;
        if (consumed.shape)
            shapeConsumed[consumed.leaf] = true;
    }
    return std::nullopt;
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_EFFECT_EMISSION_HPP
