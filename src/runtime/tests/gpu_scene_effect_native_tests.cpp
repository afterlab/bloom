// Real native proof that the production CpuGpuSceneBuilder emits genuine GPU OCIO image-effect
// commands (non-identity CST and, on Linux, a real isolated FileTransform) that the production
// GpuSceneExecutor runs on the device, with the readback compared against the unchanged
// CpuCompositionEvaluator oracle under the documented strict 2e-6 absolute-or-relative gate,
// byte-exact alpha, and the CPU process descriptor (data/display window and pixel aspect).
//
// The scene is built by the REAL production builder from a real compiled plan; no hand-built
// PreparedGpuScene is used. A warm unchanged run must perform zero new OCIO dispatch, and a changed
// effect must invalidate only the effect while its upstream solid stays cached.
//
// Without a device the test reports an explicit SKIP (exit 77); --require-device makes that a
// failure. An explicit --loader selects the Vulkan loader. Without the pinned tools the real arms
// are skipped.

#include "compositing_recipe_test_support.hpp"
#include "depth_effect_test_support.hpp"
#include "lens_flare_test_support.hpp"
#include "codec_degrade_test_support.hpp"
#include "denoise_test_support.hpp"
#include "auto_exposure_test_support.hpp"
#include "film_grain_test_support.hpp"
#include "dither_test_support.hpp"
#include "motion_blur_test_support.hpp"
#include "audio_plot_test_support.hpp"
#include "histogram_test_support.hpp"
#include "statistics_test_support.hpp"

#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/image_statistics.hpp>
#include "pixel_math_test_support.hpp"
#include "color_tool_test_support.hpp"
#include "sharpen_test_support.hpp"
#include "field_test_programs.hpp"
#include "gpu_composite_native_support.hpp"
#include "grade_curves_test_support.hpp"

#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/render/image_coverage.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/input_color_context.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using bloom::core::Color4d;
using bloom::core::RationalTime;
using bloom::document::AssetId;
using bloom::document::AssetKind;
using bloom::document::AssetRecord;
using bloom::document::CompositionFormat;
using bloom::document::CompositionId;
using bloom::document::LayerId;
using bloom::document::LayerSlotId;
using bloom::document::NodeId;
using bloom::document::ParameterId;
using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::ImageWindow;
using bloom::render::readbackResidentImage;
using bloom::render::Rgba32f;
using bloom::render::composite_proof::Expectations;
using bloom::render::composite_proof::Options;
using bloom::render::composite_proof::parseOptions;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompiledCompositionPlan;
using bloom::runtime::CompiledCompositionPlanDefinition;
using bloom::runtime::CompiledImageEffect;
using bloom::runtime::CompiledLayerOutput;
using bloom::runtime::CompiledMerge;
using bloom::runtime::CompiledMergeInput;
using bloom::runtime::CompiledOperation;
using bloom::runtime::CompiledScalarParameter;
using bloom::runtime::CompiledSolid;
using bloom::runtime::CompiledVec2Parameter;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::CstKernel;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::FileTransformKernel;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorBudgets;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneExecutorJobState;
using bloom::runtime::GpuSceneExecutorPollResult;
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::GpuSceneOcioContext;
using bloom::runtime::OperationIndex;
using bloom::runtime::PointwiseImageEffect;
using bloom::runtime::PointwiseImageKernel;

// The whole fixture/runner section prepares real OCIO programs from BLOOM_GPUSHADER_TOOLS_DIR, so
// it is compiled only when the shader tools are packaged. Without them main() reports an honest
// SKIP (exit 77); there is no silent pass.
#ifdef BLOOM_GPUSHADER_TOOLS_DIR

constexpr std::uint64_t kRevision = 7;
constexpr auto kProject = bloom::document::ProjectId::fromRaw(1);
constexpr std::uint64_t kSceneBudget = 64ULL * 1024ULL * 1024ULL;

[[nodiscard]] CompositionFormat format(const std::uint32_t width, const std::uint32_t height) {
    const auto value = CompositionFormat::create(width, height);
    if (!value.has_value()) {
        throw std::logic_error("native effect fixture format is invalid");
    }
    return *value;
}

// A solid -> OCIO effect -> layer -> Normal merge -> output plan. The solid is negative-HDR with a
// translucent alpha so the effect's HDR and alpha handling are genuinely exercised.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
effectPlan(const bloom::runtime::ImageEffectKernel& kernel, const std::uint64_t idBase,
           const std::optional<AssetRecord>& lutAsset = std::nullopt,
           const Color4d color = Color4d{-0.25, 1.75, 0.5, 0.625},
           const double sourceWidth = 12.0, const double sourceHeight = 10.0) {
    constexpr std::uint64_t layerRaw = 1;
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledSolid{NodeId::fromRaw(idBase + 1),
                                          {ParameterId::fromRaw(idBase + 2), color},
                                          {ParameterId::fromRaw(idBase + 3), sourceWidth},
                                          {ParameterId::fromRaw(idBase + 4), sourceHeight}});
    auto capturedKernel = kernel;
    if (auto* file = std::get_if<FileTransformKernel>(&capturedKernel))
        file->asset = lutAsset;
    operations.emplace_back(CompiledImageEffect{NodeId::fromRaw(idBase + 5),
                                                OperationIndex::fromRaw(0),
                                                std::move(capturedKernel), false, false});
    operations.emplace_back(CompiledLayerOutput{
        NodeId::fromRaw(idBase + 6), LayerId::fromRaw(layerRaw), OperationIndex::fromRaw(1),
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 7), bloom::document::Vec2d{6.0, 5.0}},
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 8), bloom::document::Vec2d{0.0, 0.0}},
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 9), bloom::document::Vec2d{1.0, 1.0}},
        CompiledScalarParameter{ParameterId::fromRaw(idBase + 10), 0.0},
        CompiledScalarParameter{ParameterId::fromRaw(idBase + 11), 1.0},
        ParameterId::fromRaw(idBase + 12), bloom::core::kDefaultBlendMode});
    operations.emplace_back(CompiledMerge{
        NodeId::fromRaw(idBase + 13),
        std::vector<CompiledMergeInput>{CompiledMergeInput{LayerSlotId::fromRaw(idBase + 14),
                                                           LayerId::fromRaw(layerRaw),
                                                           OperationIndex::fromRaw(2)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 15), OperationIndex::fromRaw(3)});
    CompiledCompositionPlanDefinition definition{bloom::document::Revision::fromRaw(kRevision),
                                                 kProject,
                                                 CompositionId::fromRaw(idBase + 100),
                                                 format(12, 10),
                                                 std::move(operations),
                                                 OperationIndex::fromRaw(4)};
    definition.duration = RationalTime::fromInteger(100);
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

[[nodiscard]] EvaluationRequest requestFor(const CompiledCompositionPlan& plan) {
    return EvaluationRequest{.time = RationalTime{},
                             .output = plan.output(),
                             .resolution = bloom::runtime::CompositionFormatResolution{},
                             .quality = bloom::runtime::EvaluationQuality::Reference,
                             .colorIntent =
                                 bloom::runtime::EvaluationColorIntent::LinearRec709Scene,
                             .pixelStorageByteLimit = kSceneBudget};
}

struct RunOutcome final {
    GpuSceneExecutorDiagnosticCode code = GpuSceneExecutorDiagnosticCode::None;
    GpuSceneExecutorPollResult pollResult = GpuSceneExecutorPollResult::Failure;
    std::vector<Rgba32f> pixels;
    std::optional<ImageWindow> dataWindow;
    std::optional<ImageWindow> displayWindow;
    bloom::core::PixelAspectRatio pixelAspect = bloom::core::PixelAspectRatio::square();
};

[[nodiscard]] RunOutcome
runScene(GpuSceneExecutor& executor,
         const std::shared_ptr<const bloom::runtime::PreparedGpuScene>& scene) {
    RunOutcome outcome;
    outcome.code = executor.begin(scene, kSceneBudget).code;
    if (outcome.code != GpuSceneExecutorDiagnosticCode::None) {
        return outcome;
    }
    for (;;) {
        const auto poll = executor.poll();
        if (poll == GpuSceneExecutorPollResult::Pending) {
            continue;
        }
        outcome.pollResult = poll;
        break;
    }
    if (outcome.pollResult == GpuSceneExecutorPollResult::Ready) {
        const auto* image = executor.image();
        if (image != nullptr) {
            outcome.dataWindow = image->dataWindow();
            outcome.displayWindow = image->displayWindow();
            outcome.pixelAspect = image->pixelAspect();
            const auto readback = readbackResidentImage(*image, kSceneBudget);
            if (readback.hasValue()) {
                outcome.pixels = readback.pixels;
            }
        }
        static_cast<void>(executor.takeImage());
    }
    return outcome;
}

// #545: alpha an effect derives from colour the device matches within 2e-6 rather than bit for bit
// (a Luma Key of a generated gradient or of a device blur) is held to that same tolerance. Every
// other alpha is exact.
enum class AlphaGate : std::uint8_t { Exact, KeyedFromToleranceQualifiedColour };

// RGB within the strict 2e-6 abs-or-rel gate; alpha byte-exact, or within the same 2e-6 under
// AlphaGate::KeyedFromToleranceQualifiedColour. `maxAlphaError`, when given, receives the largest
// alpha difference.
[[nodiscard]] bool parity(const std::vector<Rgba32f>& actual,
                          const std::span<const Rgba32f> expected, std::string& detail,
                          const AlphaGate alphaGate = AlphaGate::Exact,
                          float* const maxAlphaError = nullptr) {
    if (actual.size() != expected.size()) {
        detail = "size";
        return false;
    }
    const auto within = [](const float a, const float e) {
        const float scale = std::max(std::abs(a), std::abs(e));
        return std::abs(a - e) <= 2e-6F * std::max(1.0F, scale);
    };
    float largestAlpha = 0.0F;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        largestAlpha = std::max(largestAlpha, std::abs(actual[i].alpha() - expected[i].alpha()));
        if (maxAlphaError != nullptr)
            *maxAlphaError = largestAlpha;
        const bool alphaPasses = alphaGate == AlphaGate::Exact
                                     ? actual[i].alpha() == expected[i].alpha()
                                     : std::isfinite(actual[i].alpha()) &&
                                           within(actual[i].alpha(), expected[i].alpha());
        if (!alphaPasses) {
            detail = "alpha at " + std::to_string(i) + " got " + std::to_string(actual[i].alpha()) +
                     " want " + std::to_string(expected[i].alpha());
            return false;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            const float a = actual[i].components()[c];
            const float e = expected[i].components()[c];
            if (!std::isfinite(a) || !std::isfinite(e)) {
                detail = "non-finite at " + std::to_string(i);
                return false;
            }
            if (!within(a, e)) {
                detail = "rgb c" + std::to_string(c) + " at " + std::to_string(i) + " got " +
                         std::to_string(a) + " want " + std::to_string(e);
                return false;
            }
        }
    }
    return true;
}

// The Vulkan loader the run was started with, for the evaluator the device-route test creates.
std::filesystem::path& loaderPath() {
    static std::filesystem::path path;
    return path;
}

[[nodiscard]] GpuSceneOcioContext ocioContext() {
    GpuOcioCompileOptions options;
    options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    GpuSceneOcioContext context;
    context.preparer = std::make_shared<GpuOcioProgramPreparer>();
    context.compileOptions = options;
    return context;
}

void runCst(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache,
            const std::string& from, const std::string& to) {
    const auto context = ocioContext();
    const auto planA = effectPlan(CstKernel{from, to}, 1000);
    const CpuGpuSceneBuilder builder(nullptr, {}, context);
    const auto requestA = requestFor(*planA);
    const auto preparedA = builder.build(planA, requestA);
    expectations.expect(preparedA.hasValue(), "effect native: the real CST scene prepares");
    if (!preparedA) {
        std::cerr << "prepare diagnostic: " << preparedA.diagnostic.message << '\n';
        return;
    }
    const CpuCompositionEvaluator evaluator;
    auto oracleRequest = requestA;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(planA, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, "effect native: the CPU oracle evaluates");
    if (!frame.frame()) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), "effect native: the executor creates");
    if (!executor) {
        return;
    }

    const auto cold = runScene(*executor.executor, preparedA.scene);
    expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready,
                        "effect native: the cold run completes: " +
                            executor.executor->diagnostic().message);
    if (cold.pollResult != GpuSceneExecutorPollResult::Ready) {
        return;
    }
    const auto coldCounters = executor.executor->counters();
    expectations.expect(coldCounters.ocioEffectDispatches == 1,
                        "effect native: the cold run dispatches exactly one OCIO effect");
    expectations.expect(coldCounters.readbacks == 0,
                        "effect native: the executor performs no full-frame readback");
    std::string detail;
    expectations.expect(parity(cold.pixels, frame.frame()->processImage().pixels(), detail),
                        "effect native: GPU pixels match the CPU oracle (2e-6, exact alpha): " +
                            detail);
    const auto& oracleDescriptor = *frame.frame()->processImage().descriptor();
    expectations.expect(cold.dataWindow.has_value() &&
                            *cold.dataWindow == oracleDescriptor.dataWindow() &&
                            cold.displayWindow.has_value() &&
                            *cold.displayWindow == oracleDescriptor.displayWindow() &&
                            cold.pixelAspect == oracleDescriptor.pixelAspect(),
                        "effect native: the GPU descriptor matches the CPU process descriptor");

    // Warm unchanged scene: zero new OCIO dispatch.
    const auto warm = runScene(*executor.executor, preparedA.scene);
    expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready,
                        "effect native: the warm run completes");
    const auto warmCounters = executor.executor->counters();
    expectations.expect(warmCounters.ocioEffectDispatches == coldCounters.ocioEffectDispatches,
                        "effect native: a warm unchanged effect runs zero new OCIO dispatch");
    expectations.expect(parity(warm.pixels, frame.frame()->processImage().pixels(), detail),
                        "effect native: warm pixels still match the CPU oracle");

    // A changed upstream solid invalidates only its own command: the OCIO program is reused (no new
    // native program) but the effect re-dispatches over the new input.
    const auto planC =
        effectPlan(CstKernel{from, to}, 3000, std::nullopt, Color4d{0.4, -0.5, 0.9, 0.25});
    const auto requestC = requestFor(*planC);
    const auto preparedC = builder.build(planC, requestC);
    expectations.expect(preparedC.hasValue(), "effect native: the changed scene prepares");
    if (preparedC) {
        const auto changed = runScene(*executor.executor, preparedC.scene);
        expectations.expect(changed.pollResult == GpuSceneExecutorPollResult::Ready,
                            "effect native: the changed-upstream run completes");
        const auto changedCounters = executor.executor->counters();
        expectations.expect(changedCounters.ocioEffectDispatches ==
                                warmCounters.ocioEffectDispatches + 1,
                            "effect native: a changed upstream re-dispatches the effect");
        expectations.expect(changedCounters.ocioProgramCreations ==
                                coldCounters.ocioProgramCreations,
                            "effect native: a changed upstream reuses the prepared OCIO program");
        expectations.expect(changedCounters.solidDispatches > coldCounters.solidDispatches,
                            "effect native: the changed solid really re-dispatches");
    }
}

void runPointwise(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    const CpuCompositionEvaluator evaluator;
    const std::array cases{
        PointwiseImageKernel{PointwiseImageEffect::Exposure,
                             CompiledScalarParameter{ParameterId::fromRaw(900), 1.25}},
        PointwiseImageKernel{PointwiseImageEffect::Offset,
                             CompiledScalarParameter{ParameterId::fromRaw(900), -0.35}},
        PointwiseImageKernel{PointwiseImageEffect::Multiply,
                             CompiledScalarParameter{ParameterId::fromRaw(900), -2.0}},
        PointwiseImageKernel{PointwiseImageEffect::Invert, {}}};
    std::uint64_t idBase = 10000;
    for (const auto& kernel : cases) {
        const auto plan = effectPlan(kernel, idBase);
        idBase += 1000;
        const auto request = requestFor(*plan);
        const auto prepared = builder.build(plan, request);
        const auto oracle = evaluator.evaluate(plan, request, {});
        expectations.expect(prepared.hasValue() && oracle.frame(),
                            "pointwise: production GPU preparation and CPU evaluation succeed");
        if (!prepared || !oracle.frame())
            continue;
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), "pointwise: native executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        std::string detail;
        expectations.expect(
            cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail),
            "pointwise: native signed HDR and alpha match CPU: " + detail);
        const auto counters = executor.executor->counters();
        expectations.expect(counters.ocioEffectDispatches == 1 && counters.readbacks == 0,
                            "pointwise: one resident effect with no per-node CPU readback");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(
            warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                parity(warm.pixels, oracle.frame()->processImage().pixels(), detail) &&
                executor.executor->counters().ocioEffectDispatches == counters.ocioEffectDispatches,
            "pointwise: warm native pixels reuse the cached effect");
        if (kernel.amount) {
            auto changedKernel = kernel;
            std::get<double>(changedKernel.amount->source) += 0.5;
            const auto changedPlan = effectPlan(changedKernel, idBase - 1000);
            const auto changedRequest = requestFor(*changedPlan);
            const auto changed = builder.build(changedPlan, changedRequest);
            const auto changedOracle = evaluator.evaluate(changedPlan, changedRequest, {});
            expectations.expect(changed.hasValue() && changedOracle.frame(),
                                "pointwise: changed amount prepares and evaluates");
            if (changed && changedOracle.frame()) {
                const auto result = runScene(*executor.executor, changed.scene);
                const auto changedCounters = executor.executor->counters();
                expectations.expect(
                    result.pollResult == GpuSceneExecutorPollResult::Ready &&
                        parity(result.pixels, changedOracle.frame()->processImage().pixels(),
                               detail),
                    "pointwise: changed amount matches the CPU oracle: " + detail);
                // The input is a covered solid. A changed effect also invalidates the downstream
                // merge, whose transparent accumulator adds one ordinary solid dispatch.
                expectations.expect(
                    changedCounters.ocioEffectDispatches == counters.ocioEffectDispatches + 1 &&
                        changedCounters.solidDispatches == counters.solidDispatches + 1 &&
                        changedCounters.coveredSolidDispatches == counters.coveredSolidDispatches &&
                        changedCounters.coverageDispatches == counters.coverageDispatches,
                    "pointwise: changing amount invalidates the effect while retaining its input");
            }
        }
    }
}

void runPointwiseExtremeAlpha(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    const CpuCompositionEvaluator evaluator;
    // Valid finite premultiplied RGB near 1e20, but recovering straight RGB in Float32 would
    // overflow. Each affine result remains representable, including Multiply's exact zero.
    constexpr Color4d color{1e40, -1e40, 2e40, 1e-20};
    const std::array cases{
        PointwiseImageKernel{PointwiseImageEffect::Exposure,
                             CompiledScalarParameter{ParameterId::fromRaw(900), -1.0}},
        PointwiseImageKernel{PointwiseImageEffect::Offset,
                             CompiledScalarParameter{ParameterId::fromRaw(900), -3e38}},
        PointwiseImageKernel{PointwiseImageEffect::Multiply,
                             CompiledScalarParameter{ParameterId::fromRaw(900), -2.0}},
        PointwiseImageKernel{PointwiseImageEffect::Multiply,
                             CompiledScalarParameter{ParameterId::fromRaw(900), 0.0}},
        PointwiseImageKernel{PointwiseImageEffect::Invert, {}}};
    std::uint64_t idBase = 20000;
    for (const auto& kernel : cases) {
        cache.clear();
        const auto plan = effectPlan(kernel, idBase, std::nullopt, color);
        idBase += 1000;
        const auto request = requestFor(*plan);
        const auto prepared = builder.build(plan, request);
        const auto oracle = evaluator.evaluate(plan, request, {});
        if (!prepared || !oracle.frame()) {
            std::cerr << "pointwise extreme alpha effect " << static_cast<unsigned>(kernel.effect)
                      << ": GPU " << prepared.diagnostic.message << '\n';
            for (const auto& diagnostic : oracle.diagnostics())
                std::cerr << "CPU: " << diagnostic.summary << ": " << diagnostic.detail << '\n';
        }
        expectations.expect(prepared && oracle.frame(),
                            "pointwise extreme alpha: finite associated RGB prepares and evaluates");
        if (!prepared || !oracle.frame())
            continue;
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), "pointwise extreme alpha: native executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        std::string detail;
        expectations.expect(
            cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail),
            "pointwise extreme alpha: native finite HDR and exact alpha match CPU: " + detail);
        const auto counters = executor.executor->counters();
        expectations.expect(counters.ocioEffectDispatches == 1 && counters.readbacks == 0,
                            "pointwise extreme alpha: one genuine resident effect without readback");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(
            warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                parity(warm.pixels, oracle.frame()->processImage().pixels(), detail) &&
                executor.executor->counters().dispatches == counters.dispatches,
            "pointwise extreme alpha: warm cache retains parity with zero new dispatches");
    }
}

// #532 Multiply Alpha below a unit factor: base solid -> Multiply Alpha by a matte solid's alpha
// -> layer -> Normal merge -> output. Both routes round coverage x factor to Float32 first, then
// each premultiplied component times that amount once, so alpha is bit-exact.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
multiplyAlphaPlan(const std::uint64_t idBase, const Color4d base, const Color4d matte,
                  const double factor) {
    constexpr std::uint64_t layerRaw = 1;
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledSolid{NodeId::fromRaw(idBase + 1),
                                          {ParameterId::fromRaw(idBase + 2), base},
                                          {ParameterId::fromRaw(idBase + 3), 12.0},
                                          {ParameterId::fromRaw(idBase + 4), 10.0}});
    operations.emplace_back(CompiledSolid{NodeId::fromRaw(idBase + 16),
                                          {ParameterId::fromRaw(idBase + 17), matte},
                                          {ParameterId::fromRaw(idBase + 18), 12.0},
                                          {ParameterId::fromRaw(idBase + 19), 10.0}});
    operations.emplace_back(CompiledImageEffect{
        NodeId::fromRaw(idBase + 5), OperationIndex::fromRaw(0),
        bloom::runtime::ImageCombineKernel{
            bloom::runtime::ImageCombineMode::MultiplyAlpha,
            CompiledScalarParameter{ParameterId::fromRaw(idBase + 20), factor},
            bloom::render::ImageCoverageChannel::Alpha},
        false, false, OperationIndex::fromRaw(1)});
    operations.emplace_back(CompiledLayerOutput{
        NodeId::fromRaw(idBase + 6), LayerId::fromRaw(layerRaw), OperationIndex::fromRaw(2),
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 7), bloom::document::Vec2d{6.0, 5.0}},
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 8), bloom::document::Vec2d{0.0, 0.0}},
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 9), bloom::document::Vec2d{1.0, 1.0}},
        CompiledScalarParameter{ParameterId::fromRaw(idBase + 10), 0.0},
        CompiledScalarParameter{ParameterId::fromRaw(idBase + 11), 1.0},
        ParameterId::fromRaw(idBase + 12), bloom::core::kDefaultBlendMode});
    operations.emplace_back(CompiledMerge{
        NodeId::fromRaw(idBase + 13),
        std::vector<CompiledMergeInput>{CompiledMergeInput{LayerSlotId::fromRaw(idBase + 14),
                                                           LayerId::fromRaw(layerRaw),
                                                           OperationIndex::fromRaw(3)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 15), OperationIndex::fromRaw(4)});
    CompiledCompositionPlanDefinition definition{bloom::document::Revision::fromRaw(kRevision),
                                                 kProject,
                                                 CompositionId::fromRaw(idBase + 100),
                                                 format(12, 10),
                                                 std::move(operations),
                                                 OperationIndex::fromRaw(5)};
    definition.duration = RationalTime::fromInteger(100);
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

void runMultiplyAlpha(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    const CpuCompositionEvaluator evaluator;
    // Base alpha 0.7, matte coverage 0.6, factor 0.8: rounding coverage x factor first gives
    // alpha 0.335999995, one rounding of the exact triple product 0.336000025. The fixture is
    // only a regression guard while those two orders disagree.
    constexpr double baseAlpha = 0.7;
    constexpr double coverage = 0.6;
    constexpr double factor = 0.8;
    const float amount = bloom::render::multiplyAlphaAmount(static_cast<float>(coverage),
                                                            static_cast<float>(factor));
    const float twoRoundings = static_cast<float>(baseAlpha) * amount;
    const float oneRounding =
        static_cast<float>(static_cast<double>(static_cast<float>(baseAlpha)) *
                           static_cast<double>(static_cast<float>(coverage)) *
                           static_cast<double>(static_cast<float>(factor)));
    expectations.expect(twoRoundings != oneRounding,
                        "multiply alpha native: the fixture separates the two rounding orders");
    cache.clear();
    const auto plan = multiplyAlphaPlan(30000, Color4d{-0.25, 1.75, 0.5, baseAlpha},
                                        Color4d{0.75, -0.25, 2.0, coverage}, factor);
    const auto request = requestFor(*plan);
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
    const auto prepared = builder.build(plan, request);
    expectations.expect(oracle.frame() && prepared,
                        "multiply alpha native: CPU oracle and production scene prepare");
    if (!oracle.frame() || !prepared)
        return;
    const auto pixels = oracle.frame()->processImage().pixels();
    expectations.expect(std::ranges::any_of(pixels,
                                            [twoRoundings](const Rgba32f pixel) {
                                                return pixel.alpha() == twoRoundings;
                                            }),
                        "multiply alpha native: the CPU reference rounds coverage x factor first");
    auto executor = GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), "multiply alpha native: executor creates");
    if (!executor)
        return;
    const auto cold = runScene(*executor.executor, prepared.scene);
    std::string detail;
    expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                            parity(cold.pixels, pixels, detail),
                        "multiply alpha native: factor below one matches the CPU (2e-6, exact "
                        "alpha): " + detail);
    const auto counters = executor.executor->counters();
    expectations.expect(counters.blendDispatches > 0 && counters.readbacks == 0,
                        "multiply alpha native: resident combine dispatch without readback");
}

void runCanvas(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    for (const auto kind : {bloom::runtime::CanvasImageEffect::Crop,
                            bloom::runtime::CanvasImageEffect::Pad}) {
        for (const bool proxy : {false, true}) {
            const auto base = effectPlan(CstKernel{},
                                         kind == bloom::runtime::CanvasImageEffect::Crop
                                             ? 7000 : 8000);
            auto definition = base->copyDefinition();
            auto& solid = std::get<CompiledSolid>(definition.operations[0]);
            solid.width.source = 8.0;
            solid.height.source = 6.0;
            std::array<CompiledScalarParameter, 4> margins{
                CompiledScalarParameter{ParameterId::fromRaw(9001), 1.0},
                CompiledScalarParameter{ParameterId::fromRaw(9002), 1.0},
                CompiledScalarParameter{ParameterId::fromRaw(9003), 1.0},
                CompiledScalarParameter{ParameterId::fromRaw(9004), 0.0}};
            if (kind == bloom::runtime::CanvasImageEffect::Pad)
                margins = {CompiledScalarParameter{ParameterId::fromRaw(9001), 2.0},
                           CompiledScalarParameter{ParameterId::fromRaw(9002), 1.0},
                           CompiledScalarParameter{ParameterId::fromRaw(9003), 2.0},
                           CompiledScalarParameter{ParameterId::fromRaw(9004), 1.0}};
            std::get<CompiledImageEffect>(definition.operations[1]).kernel =
                bloom::runtime::CanvasImageKernel{kind, margins};
            const auto plan = std::make_shared<const CompiledCompositionPlan>(
                std::move(definition));
            auto request = requestFor(*plan);
            if (proxy) {
                const auto extent = bloom::render::ImageExtent::create(6, 5);
                request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
            }
            const auto prepared = CpuGpuSceneBuilder{}.build(plan, request);
            expectations.expect(prepared.hasValue(),
                                "canvas native: production scene prepares");
            if (!prepared)
                continue;
            const auto commandIndex = prepared.scene->commandForOperation()[1];
            const auto* command =
                std::get_if<bloom::runtime::GpuSceneTranslationCommand>(
                    &prepared.scene->commands()[commandIndex]);
            expectations.expect(command && command->outputDisplayWindow.has_value(),
                                "canvas native: resident translation changes the display window");
            if (!command)
                continue;
            auto oracleRequest = request;
            oracleRequest.bypassOperationCache = true;
            const CpuCompositionEvaluator evaluator;
            const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
            expectations.expect(oracle.frame() != nullptr,
                                "canvas native: CPU reference evaluates");
            if (!oracle.frame())
                continue;
            auto executor = GpuSceneExecutor::create(device, cache);
            expectations.expect(executor.hasValue(), "canvas native: executor creates");
            if (!executor)
                continue;
            const auto cold = runScene(*executor.executor, prepared.scene);
            expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready,
                                "canvas native: cold GPU run completes: " +
                                    executor.executor->diagnostic().message);
            if (cold.pollResult != GpuSceneExecutorPollResult::Ready)
                continue;
            std::string detail;
            expectations.expect(parity(cold.pixels, oracle.frame()->processImage().pixels(),
                                       detail),
                                "canvas native: CPU/GPU pixels agree at full/proxy: " + detail);
            const auto& descriptor = *oracle.frame()->processImage().descriptor();
            expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                    cold.displayWindow == descriptor.displayWindow() &&
                                    cold.pixelAspect == descriptor.pixelAspect(),
                                "canvas native: final descriptor matches CPU");
            const auto coldCounters = executor.executor->counters();
            expectations.expect(coldCounters.nativeBatches > 0 &&
                                    coldCounters.translationDispatches > 0 &&
                                    coldCounters.readbacks == 0,
                                "canvas native: pixels execute in a resident batch without readback");
            const auto warm = runScene(*executor.executor, prepared.scene);
            expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                    parity(warm.pixels, oracle.frame()->processImage().pixels(),
                                           detail) &&
                                    executor.executor->counters().translationDispatches ==
                                        coldCounters.translationDispatches,
                                "canvas native: warm image is reused without a new translation");
        }
    }
}

#if defined(__linux__) || defined(__APPLE__)
void runFileTransform(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache,
                      const std::string& working, const std::string& process) {
    const auto base = std::filesystem::temp_directory_path() / "bloom_gpu_effect_native";
    std::error_code directoryError;
    std::filesystem::create_directories(base, directoryError);
    expectations.expect(!directoryError, "effect native: the LUT fixture directory is creatable");
    if (directoryError) {
        return;
    }
    const auto path = base / "curve.cube";
    {
        std::ofstream file(path);
        file << "TITLE \"test\"\nLUT_1D_SIZE 4\n";
        file << "0.0 0.0 0.0\n0.2 0.3 0.4\n0.6 0.5 0.7\n1.0 1.0 1.0\n";
    }
    const auto resource = bloom::color::readLutFile(path);
    expectations.expect(resource.error == bloom::color::LutError::None,
                        "effect native: the LUT fixture reads");
    if (resource.error != bloom::color::LutError::None) {
        return;
    }
    AssetRecord asset;
    asset.id = AssetId::fromRaw(77);
    asset.kind = AssetKind::Lut;
    asset.locator.path = "curve.cube";
    asset.locator.relinkHint = "file://curve.cube";
    asset.contentDigest = resource.digest;
    FileTransformKernel file;
    file.lutAssetId = asset.id;

    const auto context = ocioContext();
    GpuSceneMediaContext media;
    media.assetBaseDirectory = base;
    const auto plan = effectPlan(file, 5000, asset);
    const CpuGpuSceneBuilder builder(nullptr, media, context);
    const auto request = requestFor(*plan);
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), "effect native: the FileTransform scene prepares");
    if (!prepared) {
        std::cerr << "FileTransform prepare diagnostic: " << prepared.diagnostic.message << '\n';
        return;
    }
    const CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(base);
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr,
                        "effect native: the FileTransform oracle evaluates");
    if (!frame.frame()) {
        return;
    }
    auto executor = GpuSceneExecutor::create(device, cache);
    expectations.expect(executor.hasValue(), "effect native: the FileTransform executor creates");
    if (!executor) {
        return;
    }
    const auto run = runScene(*executor.executor, prepared.scene);
    expectations.expect(run.pollResult == GpuSceneExecutorPollResult::Ready,
                        "effect native: the FileTransform run completes: " +
                            executor.executor->diagnostic().message);
    if (run.pollResult != GpuSceneExecutorPollResult::Ready) {
        return;
    }
    expectations.expect(executor.executor->counters().ocioEffectDispatches == 1,
                        "effect native: the FileTransform dispatches one OCIO effect");
    std::string detail;
    expectations.expect(parity(run.pixels, frame.frame()->processImage().pixels(), detail),
                        "effect native: FileTransform pixels match the CPU oracle: " + detail);
    static_cast<void>(working);
    static_cast<void>(process);
}
#endif

void runDiscrete(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::runtime::DiscreteImageKernel;
    std::uint64_t id = 20000;
    for (const auto kernel :
         {DiscreteImageKernel{true, false, 0}, DiscreteImageKernel{false, true, 0},
          DiscreteImageKernel{true, true, 0}, DiscreteImageKernel{false, false, 1},
          DiscreteImageKernel{false, false, 2}, DiscreteImageKernel{false, false, 3}}) {
        cache.clear();
        auto definition = effectPlan(CstKernel{}, id)->copyDefinition();
        bloom::runtime::CompiledShape shape{};
        shape.sourceNodeId = NodeId::fromRaw(id + 1);
        shape.kind = bloom::document::ShapeKind::Line;
        shape.size = {ParameterId::fromRaw(id + 2), bloom::document::Vec2d{6, 3}};
        shape.fillEnabled = false;
        shape.fillColor = {ParameterId::fromRaw(id + 3), Color4d{-0.25, 1.75, 0.5, 0.625}};
        shape.strokeEnabled = true;
        shape.strokeColor = {ParameterId::fromRaw(id + 4), Color4d{-0.25, 1.75, 0.5, 0.625}};
        shape.strokeWidth = {ParameterId::fromRaw(id + 5), 1.5};
        shape.lineStart = {{}, bloom::document::Vec2d{0, 0}};
        shape.lineEnd = {{}, bloom::document::Vec2d{6, 3}};
        definition.operations[0] = shape;
        std::get<CompiledImageEffect>(definition.operations[1]).kernel = kernel;
        definition.operations.resize(2);
        definition.operations.emplace_back(CompiledMerge{
            NodeId::fromRaw(id + 20),
            {{LayerSlotId::fromRaw(id + 20), LayerId{}, OperationIndex::fromRaw(1)}}});
        definition.operations.emplace_back(
            CompiledCompositionOutput{NodeId::fromRaw(id + 21), OperationIndex::fromRaw(2)});
        definition.output = OperationIndex::fromRaw(3);
        const auto plan = std::make_shared<const CompiledCompositionPlan>(std::move(definition));
        const auto request = requestFor(*plan);
        const auto prepared = CpuGpuSceneBuilder{}.build(plan, request);
        expectations.expect(prepared.hasValue(), "discrete native: production scene prepares");
        const CpuCompositionEvaluator evaluator;
        const auto oracle = evaluator.evaluate(plan, request, {});
        expectations.expect(oracle.frame() != nullptr, "discrete native: CPU oracle evaluates");
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), "discrete native: executor creates");
        if (!prepared || !oracle.frame() || !executor)
            return;
        const auto cold = runScene(*executor.executor, prepared.scene);
        expectations.expect(
            cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                std::ranges::equal(cold.pixels, oracle.frame()->processImage().pixels()),
            "discrete native: asymmetric signed/HDR/partial-alpha pixels match CPU exactly");
        const auto counters = executor.executor->counters();
        expectations.expect(
            counters.affineDispatches == 1 && counters.readbacks == 0,
            "discrete native: one real resident affine pass with no executor readback");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(
            warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                std::ranges::equal(warm.pixels, oracle.frame()->processImage().pixels()) &&
                executor.executor->counters().affineDispatches == counters.affineDispatches,
            "discrete native: warm frame reuses the resident result and preserves exact pixels");
        id += 100;
    }
}

// A Merge entry that is a shape source read directly (no layer between): the entry is composited
// like any other, so an effect chain that merges its source back (Drop Shadow, Light Wrap, Inner
// Shadow) keeps the shape on the GPU exactly as the CPU reference does.
void runShapeMergeEntry(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::runtime::DiscreteImageKernel;
    std::uint64_t id = 30000;
    for (const auto mode : {bloom::core::BlendMode::Atop, bloom::core::BlendMode::Normal}) {
        cache.clear();
        auto definition = effectPlan(CstKernel{}, id)->copyDefinition();
        bloom::runtime::CompiledShape shape{};
        shape.sourceNodeId = NodeId::fromRaw(id + 1);
        shape.kind = bloom::document::ShapeKind::Rectangle;
        shape.size = {ParameterId::fromRaw(id + 2), bloom::document::Vec2d{8, 6}};
        shape.fillEnabled = true;
        shape.fillColor = {ParameterId::fromRaw(id + 3), Color4d{0.75, 0.5, 0.25, 0.875}};
        shape.strokeEnabled = false;
        definition.operations[0] = shape;
        std::get<CompiledImageEffect>(definition.operations[1]).kernel =
            DiscreteImageKernel{true, false, 0};
        definition.operations.resize(2);
        definition.operations.emplace_back(CompiledMerge{
            NodeId::fromRaw(id + 20),
            {{LayerSlotId::fromRaw(id + 20), LayerId{}, OperationIndex::fromRaw(1), mode},
             {LayerSlotId::fromRaw(id + 21), LayerId{}, OperationIndex::fromRaw(0),
              bloom::core::BlendMode::Normal}}});
        definition.operations.emplace_back(
            CompiledCompositionOutput{NodeId::fromRaw(id + 22), OperationIndex::fromRaw(2)});
        definition.output = OperationIndex::fromRaw(3);
        const auto plan = std::make_shared<const CompiledCompositionPlan>(std::move(definition));
        const auto request = requestFor(*plan);
        const auto prepared = CpuGpuSceneBuilder{}.build(plan, request);
        expectations.expect(prepared.hasValue(), "shape merge entry: production scene prepares");
        const CpuCompositionEvaluator evaluator;
        const auto oracle = evaluator.evaluate(plan, request, {});
        expectations.expect(oracle.frame() != nullptr, "shape merge entry: CPU oracle evaluates");
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), "shape merge entry: executor creates");
        if (!prepared || !oracle.frame() || !executor)
            return;
        const auto run = runScene(*executor.executor, prepared.scene);
        std::string detail;
        expectations.expect(run.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(run.pixels, oracle.frame()->processImage().pixels(), detail),
                            "shape merge entry: the shape entry is composited and matches the CPU "
                            "reference: " + detail);
        bool anyAlpha = false;
        for (const auto& pixel : run.pixels)
            anyAlpha = anyAlpha || pixel.alpha() > 0.0F;
        expectations.expect(anyAlpha, "shape merge entry: the merged result is not empty");
        id += 100;
    }
}

void runResizeFit(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    const CpuGpuSceneBuilder builder;
    const CpuCompositionEvaluator evaluator;
    for (const bloom::runtime::ResizeFitKernel kernel :
         {bloom::runtime::ResizeFitKernel{8, 8}, bloom::runtime::ResizeFitKernel{8, 8, 1, 1, 0, 1},
          bloom::runtime::ResizeFitKernel{8, 8, 1, 1, 0, 2},
          bloom::runtime::ResizeFitKernel{16, 12, 2, 1, 0, 1},
          // #480 Nearest: the same geometry through AffineBilinearV1's nearest sampling.
          bloom::runtime::ResizeFitKernel{8, 8, 1, 1, 1},
          bloom::runtime::ResizeFitKernel{8, 8, 1, 1, 1, 1},
          bloom::runtime::ResizeFitKernel{16, 12, 2, 1, 1, 2}}) {
        auto definition = effectPlan(CstKernel{}, 25000)->copyDefinition();
        std::get<CompiledImageEffect>(definition.operations[1]).kernel = kernel;
        const auto plan = std::make_shared<const CompiledCompositionPlan>(std::move(definition));
        for (const bool proxy : {false, true}) {
            auto request = requestFor(*plan);
            if (proxy) {
                const auto extent = bloom::render::ImageExtent::create(6, 5);
                request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
            }
            const auto prepared = builder.build(plan, request);
            const auto cpu = evaluator.evaluate(plan, request, {});
            expectations.expect(prepared && cpu.frame(),
                                "Resize/Fit native: production GPU/CPU routes prepare");
            if (!prepared || !cpu.frame())
                continue;
            auto executor = GpuSceneExecutor::create(device, cache);
            expectations.expect(executor.hasValue(), "Resize/Fit native: executor creates");
            if (!executor)
                continue;
            const auto before = executor.executor->counters();
            const auto cold = runScene(*executor.executor, prepared.scene);
            const auto after = executor.executor->counters();
            expectations.expect(
                cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                    after.readbacks == before.readbacks,
                "Resize/Fit native: resident evaluation completes without readback");
            const auto pixels = cpu.frame()->processImage().pixels();
            bool matches = cold.pixels.size() == pixels.size();
            for (std::size_t i = 0; matches && i < pixels.size(); ++i)
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    const float actual = cold.pixels[i].components()[channel];
                    const float expected = pixels[i].components()[channel];
                    matches =
                        matches && std::isfinite(actual) &&
                        std::abs(actual - expected) <= 2e-6F * std::max(1.0F, std::abs(expected));
                    if (channel == 3 && (expected == 0 || expected == 1))
                        matches = matches && actual == expected;
                }
            const auto& descriptor = *cpu.frame()->processImage().descriptor();
            expectations.expect(matches && cold.dataWindow == descriptor.dataWindow() &&
                                    cold.displayWindow == descriptor.displayWindow() &&
                                    cold.pixelAspect == descriptor.pixelAspect(),
                                "Resize/Fit native: pixels, alpha/HDR and geometry match CPU");
            const auto warm = runScene(*executor.executor, prepared.scene);
            expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                    executor.executor->counters().affineDispatches ==
                                        after.affineDispatches,
                                "Resize/Fit native: unchanged warm render runs no affine dispatch");
        }
    }
}

// #336 Transform 2D: the production builder's resident AffineBilinearV1 command against the CPU
// reference at the documented 2e-6 gate, for an asymmetric stroked shape and a signed/HDR
// translucent solid, every extent, a mirror, an exact quarter turn, a degenerate scale and a
// proxy. Cold runs perform no executor readback, warm runs no new affine dispatch, and a changed
// value dispatches again.
void runTransform2D(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::document::Vec2d;
    using bloom::runtime::CompiledVec2Parameter;
    using bloom::runtime::Transform2DExtent;
    using bloom::runtime::Transform2DKernel;
    const CpuGpuSceneBuilder builder;
    const CpuCompositionEvaluator evaluator;
    struct Case final {
        Vec2d translate;
        Vec2d scale;
        double rotation;
        Vec2d pivot;
        Transform2DExtent extent;
        std::int64_t filter = bloom::runtime::kResampleFilterBilinear;
        double skew = 0.0;
        double skewAxis = 0.0;
        double opacity = 1.0;
    };
    constexpr auto kNearest = bloom::runtime::kResampleFilterNearest;
    const std::array cases{
        Case{{1.5, -0.75}, {1, 1}, 0.0, {}, Transform2DExtent::Transformed},
        Case{{0.25, 0.5}, {1.5, 0.5}, 30.0, {1, 0.5}, Transform2DExtent::Transformed},
        Case{{2, 1}, {-1, 1}, 90.0, {}, Transform2DExtent::Transformed},
        Case{{0, 0}, {1.25, 1.25}, 20.0, {}, Transform2DExtent::Display},
        Case{{3, 0.5}, {1, 1}, -12.5, {}, Transform2DExtent::Input},
        Case{{0, 0}, {0, 1}, 0.0, {}, Transform2DExtent::Transformed},
        // #480 Nearest: a fractional move (never the translation pass), a rotated scale and a
        // mirrored quarter turn copy exact source taps on both routes.
        Case{{1.25, -0.375}, {1, 1}, 0.0, {}, Transform2DExtent::Transformed, kNearest},
        Case{{0.25, 0.375}, {1.5, 0.75}, 30.0, {1, 0.5}, Transform2DExtent::Transformed,
             kNearest},
        Case{{2, 1}, {-1, 1}, 90.0, {}, Transform2DExtent::Display, kNearest},
        // Skew along the horizontal and a tilted axis, with and without rotation and scale, and an
        // opacity alone (the translation pass carries it) and with a skew and Nearest.
        Case{{0.5, 0.25}, {1, 1}, 0.0, {}, Transform2DExtent::Transformed,
             bloom::runtime::kResampleFilterBilinear, 20.0, 0.0, 1.0},
        Case{{0, 0}, {1.25, 0.75}, 15.0, {0.5, 0}, Transform2DExtent::Display,
             bloom::runtime::kResampleFilterBilinear, -30.0, 40.0, 0.75},
        Case{{1.5, -0.5}, {1, 1}, 0.0, {}, Transform2DExtent::Transformed,
             bloom::runtime::kResampleFilterBilinear, 0.0, 0.0, 0.5},
        Case{{0.25, 0.125}, {1, 1}, 0.0, {}, Transform2DExtent::Input, kNearest, 25.0, 90.0,
             0.625}};
    std::uint64_t id = 26000;
    for (const auto& item : cases) {
        for (const bool shapeSource : {false, true}) {
            for (const bool proxy : {false, true}) {
                cache.clear();
                const Transform2DKernel kernel{
                    CompiledVec2Parameter{ParameterId::fromRaw(id + 30), item.translate},
                    CompiledVec2Parameter{ParameterId::fromRaw(id + 31), item.pivot},
                    CompiledVec2Parameter{ParameterId::fromRaw(id + 32), item.scale},
                    CompiledScalarParameter{ParameterId::fromRaw(id + 33), item.rotation},
                    item.extent, item.filter,
                    CompiledScalarParameter{ParameterId::fromRaw(id + 34), item.skew},
                    CompiledScalarParameter{ParameterId::fromRaw(id + 35), item.skewAxis},
                    CompiledScalarParameter{ParameterId::fromRaw(id + 36), item.opacity}};
                auto definition = effectPlan(CstKernel{}, id)->copyDefinition();
                std::get<CompiledImageEffect>(definition.operations[1]).kernel = kernel;
                if (shapeSource) {
                    bloom::runtime::CompiledShape shape{};
                    shape.sourceNodeId = NodeId::fromRaw(id + 1);
                    shape.kind = bloom::document::ShapeKind::Line;
                    shape.size = {ParameterId::fromRaw(id + 2), Vec2d{6, 3}};
                    shape.fillEnabled = false;
                    shape.fillColor = {ParameterId::fromRaw(id + 3),
                                       Color4d{-0.25, 1.75, 0.5, 0.625}};
                    shape.strokeEnabled = true;
                    shape.strokeColor = {ParameterId::fromRaw(id + 4),
                                         Color4d{-0.25, 1.75, 0.5, 0.625}};
                    shape.strokeWidth = {ParameterId::fromRaw(id + 5), 1.5};
                    shape.lineStart = {{}, bloom::document::Vec2d{0, 0}};
                    shape.lineEnd = {{}, bloom::document::Vec2d{6, 3}};
                    definition.operations[0] = shape;
                }
                const auto plan =
                    std::make_shared<const CompiledCompositionPlan>(std::move(definition));
                auto request = requestFor(*plan);
                if (proxy) {
                    const auto extent = bloom::render::ImageExtent::create(6, 5);
                    request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
                }
                const auto prepared = builder.build(plan, request);
                const auto cpu = evaluator.evaluate(plan, request, {});
                expectations.expect(prepared && cpu.frame(),
                                    "Transform 2D native: production GPU/CPU routes prepare");
                auto executor = GpuSceneExecutor::create(device, cache);
                expectations.expect(executor.hasValue(), "Transform 2D native: executor creates");
                if (!prepared || !cpu.frame() || !executor)
                    continue;
                const auto before = executor.executor->counters();
                const auto cold = runScene(*executor.executor, prepared.scene);
                const auto after = executor.executor->counters();
                expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                        after.readbacks == before.readbacks,
                                    "Transform 2D native: resident evaluation has no readback");
                const auto pixels = cpu.frame()->processImage().pixels();
                bool matches = cold.pixels.size() == pixels.size();
                for (std::size_t i = 0; matches && i < pixels.size(); ++i)
                    for (std::size_t channel = 0; channel < 4; ++channel) {
                        const float actual = cold.pixels[i].components()[channel];
                        const float expected = pixels[i].components()[channel];
                        matches = matches && std::isfinite(actual) &&
                                  std::abs(actual - expected) <=
                                      2e-6F * std::max(1.0F, std::abs(expected));
                        if (channel == 3 && (expected == 0 || expected == 1))
                            matches = matches && actual == expected;
                    }
                const auto& descriptor = *cpu.frame()->processImage().descriptor();
                expectations.expect(matches && cold.dataWindow == descriptor.dataWindow() &&
                                        cold.displayWindow == descriptor.displayWindow() &&
                                        cold.pixelAspect == descriptor.pixelAspect(),
                                    "Transform 2D native: pixels, alpha/HDR and windows match CPU");
                const auto resamples = [&executor] {
                    const auto counters = executor.executor->counters();
                    return counters.affineDispatches + counters.translationDispatches;
                };
                const auto coldResamples = resamples();
                const auto warm = runScene(*executor.executor, prepared.scene);
                expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                        resamples() == coldResamples,
                                    "Transform 2D native: unchanged warm render dispatches nothing");
                if (item.scale.x != 0) {
                    auto changedDefinition = plan->copyDefinition();
                    std::get<Transform2DKernel>(
                        std::get<CompiledImageEffect>(changedDefinition.operations[1]).kernel)
                        .translate.source = Vec2d{item.translate.x + 0.5, item.translate.y};
                    const auto changed = std::make_shared<const CompiledCompositionPlan>(
                        std::move(changedDefinition));
                    const auto rebuilt = builder.build(changed, request);
                    const auto dispatched = resamples();
                    const auto moved = rebuilt ? runScene(*executor.executor, rebuilt.scene)
                                               : decltype(cold){};
                    expectations.expect(
                        rebuilt && moved.pollResult == GpuSceneExecutorPollResult::Ready &&
                            resamples() > dispatched,
                        "Transform 2D native: a changed value invalidates the resident result");
                }
                id += 100;
            }
        }
    }
}

// Gaussian Blur's separate vertical radius: both axes, a vertical-only blur (the horizontal pass is
// a one-tap copy and is not dispatched) and a horizontal-only one, at full scale, on a proxy with a
// region of interest, against the CPU reference.
void runSeparateBlurs(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::runtime::BlurChannels;
    using bloom::runtime::BlurEdgeMode;
    using bloom::runtime::SeparableBlurKernel;
    using bloom::runtime::SeparableBlurKind;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    std::uint64_t id = 7800;
    for (const auto [horizontal, vertical] :
         {std::pair{3.0, 1.0}, std::pair{0.0, 2.5}, std::pair{2.0, 0.0}}) {
        for (const auto edge : {BlurEdgeMode::Transparent, BlurEdgeMode::Clamp}) {
            for (const bool proxy : {false, true}) {
                cache.clear();
                SeparableBlurKernel kernel{SeparableBlurKind::Gaussian,
                                           {ParameterId::fromRaw(id + 90), horizontal}, edge,
                                           BlurChannels::Rgba};
                kernel.separate = true;
                kernel.radiusY = {ParameterId::fromRaw(id + 91), vertical};
                const auto plan = effectPlan(kernel, id);
                id += 100;
                auto request = requestFor(*plan);
                if (proxy) {
                    const auto extent = bloom::render::ImageExtent::create(7, 6);
                    request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
                    const auto roi = ImageWindow::create(1, 1, 4, 3);
                    request.roi = *roi.value();
                }
                auto oracleRequest = request;
                oracleRequest.bypassOperationCache = true;
                const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
                const auto prepared = builder.build(plan, request);
                expectations.expect(oracle.frame() && prepared,
                                    "separate blur native: CPU oracle and scene prepare");
                if (!oracle.frame() || !prepared)
                    continue;
                auto executor = GpuSceneExecutor::create(device, cache);
                if (!executor)
                    continue;
                const auto cold = runScene(*executor.executor, prepared.scene);
                const auto coldCounters = executor.executor->counters();
                std::string detail;
                expectations.expect(
                    cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                        parity(cold.pixels, oracle.frame()->processImage().pixels(), detail),
                    "separate blur native: CPU parity (2e-6, exact alpha): " + detail);
                const auto& descriptor = *oracle.frame()->processImage().descriptor();
                expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                        cold.displayWindow == descriptor.displayWindow(),
                                    "separate blur native: data and display window parity");
                const std::uint64_t legs = (horizontal > 0.0 ? 1U : 0U) + (vertical > 0.0 ? 1U : 0U);
                expectations.expect(coldCounters.ocioEffectDispatches == legs &&
                                        coldCounters.readbacks == 0,
                                    "separate blur native: one resident dispatch per non-zero "
                                    "axis and no internal readback");
                const auto warm = runScene(*executor.executor, prepared.scene);
                expectations.expect(
                    warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                        parity(warm.pixels, oracle.frame()->processImage().pixels(), detail) &&
                        executor.executor->counters().ocioEffectDispatches ==
                            coldCounters.ocioEffectDispatches,
                    "separate blur native: warm reuse has identical pixels and zero dispatches");
                // A changed vertical radius invalidates the result and keeps parity.
                auto changedDefinition = plan->copyDefinition();
                std::get<SeparableBlurKernel>(
                    std::get<CompiledImageEffect>(changedDefinition.operations[1]).kernel)
                    .radiusY.source = 1.5;
                const auto changedPlan = std::make_shared<const CompiledCompositionPlan>(
                    std::move(changedDefinition));
                const auto changed = builder.build(changedPlan, request);
                const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
                if (!changed || !changedOracle.frame())
                    continue;
                const auto before = executor.executor->counters().ocioEffectDispatches;
                const auto rerun = runScene(*executor.executor, changed.scene);
                expectations.expect(
                    rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                        parity(rerun.pixels, changedOracle.frame()->processImage().pixels(),
                               detail) &&
                        executor.executor->counters().ocioEffectDispatches > before,
                    "separate blur native: a changed vertical radius re-dispatches with CPU "
                    "parity: " + detail);
            }
        }
    }
}

// #514 Denoise & Smooth: every spatial mode, three radii and strengths, full frame and a reduced
// render scale with a region of interest, against the CPU oracle.
void runDenoise(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::runtime::DenoiseKernel;
    using bloom::runtime::DenoiseMode;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    std::uint64_t id = 7800;
    for (const auto mode : {DenoiseMode::Median, DenoiseMode::Bilateral, DenoiseMode::Spatial}) {
        for (const auto [radius, strength] : {std::pair{1.0, 0.0}, std::pair{2.0, 0.4}, std::pair{3.0, 2.0}}) {
            for (const bool proxy : {false, true}) {
                cache.clear();
                const DenoiseKernel kernel{mode, {ParameterId::fromRaw(id + 90), radius},
                                           {ParameterId::fromRaw(id + 91), strength}};
                const auto plan = effectPlan(kernel, id);
                id += 100;
                auto request = requestFor(*plan);
                if (proxy) {
                    const auto extent = bloom::render::ImageExtent::create(7, 6);
                    request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
                    const auto roi = ImageWindow::create(1, 1, 4, 3);
                    request.roi = *roi.value();
                }
                auto oracleRequest = request;
                oracleRequest.bypassOperationCache = true;
                const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
                const auto prepared = builder.build(plan, request);
                expectations.expect(oracle.frame() && prepared,
                                    "denoise native: CPU oracle and scene prepare");
                if (!oracle.frame() || !prepared)
                    continue;
                auto executor = GpuSceneExecutor::create(device, cache);
                if (!executor)
                    continue;
                const auto cold = runScene(*executor.executor, prepared.scene);
                const auto coldCounters = executor.executor->counters();
                std::string detail;
                expectations.expect(
                    cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                        parity(cold.pixels, oracle.frame()->processImage().pixels(), detail),
                    "denoise native: CPU parity: " + detail);
                const auto& descriptor = *oracle.frame()->processImage().descriptor();
                expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                        cold.displayWindow == descriptor.displayWindow(),
                                    "denoise native: data and display window parity");
                // The reduced scale (7 of 12 by 6 of 10) keeps every radius at one pixel or more.
                const std::uint64_t legs = 1U;
                expectations.expect(coldCounters.ocioEffectDispatches == legs &&
                                        coldCounters.readbacks == 0,
                                    "denoise native: one resident dispatch and no internal "
                                    "readback");
                const auto warm = runScene(*executor.executor, prepared.scene);
                expectations.expect(
                    warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                        parity(warm.pixels, oracle.frame()->processImage().pixels(), detail) &&
                        executor.executor->counters().ocioEffectDispatches ==
                            coldCounters.ocioEffectDispatches,
                    "denoise native: warm reuse has identical pixels and zero dispatches");
                // A changed strength invalidates the result and keeps parity.
                auto changedDefinition = plan->copyDefinition();
                std::get<DenoiseKernel>(
                    std::get<CompiledImageEffect>(changedDefinition.operations[1]).kernel)
                    .strength.source = strength + 0.25;
                const auto changedPlan = std::make_shared<const CompiledCompositionPlan>(
                    std::move(changedDefinition));
                const auto changed = builder.build(changedPlan, request);
                const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
                if (!changed || !changedOracle.frame())
                    continue;
                const auto before = executor.executor->counters().ocioEffectDispatches;
                const auto rerun = runScene(*executor.executor, changed.scene);
                expectations.expect(
                    rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                        parity(rerun.pixels, changedOracle.frame()->processImage().pixels(),
                               detail) &&
                        executor.executor->counters().ocioEffectDispatches > before,
                    "denoise native: a changed strength re-dispatches with CPU parity: " + detail);
            }
        }
    }
}

// #337: the Coverage channel policy (Matte Blur) runs through the same harness on the Gaussian
// kernel, so its alpha-only filter and all-channel write keep exact-alpha CPU parity too. #338
// Matte Erode/Dilate run the morphology kinds through the same cases.
void runBlurs(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::runtime::BlurChannels;
    using bloom::runtime::BlurEdgeMode;
    using bloom::runtime::SeparableBlurKernel;
    using bloom::runtime::SeparableBlurKind;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    std::uint64_t id = 7000;
    for (const auto [kind, channels] :
         {std::pair{SeparableBlurKind::Gaussian, BlurChannels::Rgba},
          std::pair{SeparableBlurKind::Box, BlurChannels::Rgba},
          std::pair{SeparableBlurKind::Gaussian, BlurChannels::Coverage},
          std::pair{SeparableBlurKind::Erode, BlurChannels::Coverage},
          std::pair{SeparableBlurKind::Dilate, BlurChannels::Coverage}}) {
        for (const auto edge : {BlurEdgeMode::Transparent, BlurEdgeMode::Clamp}) {
            for (const bool proxy : {false, true}) {
                cache.clear();
                const auto plan = effectPlan(
                    SeparableBlurKernel{kind, {ParameterId::fromRaw(id + 90), 2.5}, edge, channels},
                    id);
                id += 100;
                auto request = requestFor(*plan);
                if (proxy) {
                    const auto extent = bloom::render::ImageExtent::create(7, 6);
                    request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
                    const auto roi = ImageWindow::create(1, 1, 4, 3);
                    request.roi = *roi.value();
                }
                auto oracleRequest = request;
                oracleRequest.bypassOperationCache = true;
                const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
                const auto prepared = builder.build(plan, request);
                expectations.expect(
                    oracle.frame() && prepared,
                    "blur native: CPU oracle and production scene prepare (halo/edge/proxy/ROI)");
                if (!oracle.frame() || !prepared)
                    continue;
                auto executor = GpuSceneExecutor::create(device, cache);
                expectations.expect(executor.hasValue(), "blur native: executor creates");
                if (!executor)
                    continue;
                const auto cold = runScene(*executor.executor, prepared.scene);
                const auto coldCounters = executor.executor->counters();
                std::string detail;
                expectations.expect(
                    cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                        parity(cold.pixels, oracle.frame()->processImage().pixels(), detail),
                    "blur native: signed HDR/partial-alpha CPU parity (2e-6, exact alpha): " +
                        detail);
                const auto& descriptor = *oracle.frame()->processImage().descriptor();
                expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                        cold.displayWindow == descriptor.displayWindow() &&
                                        cold.pixelAspect == descriptor.pixelAspect(),
                                    "blur native: ROI/data/display-window and PAR parity");
                expectations.expect(coldCounters.ocioEffectDispatches == 2 &&
                                        coldCounters.readbacks == 0,
                                    "blur native: exactly two resident filter dispatches and no "
                                    "internal frame readback");
                const auto warm = runScene(*executor.executor, prepared.scene);
                expectations.expect(
                    warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                        parity(warm.pixels, oracle.frame()->processImage().pixels(), detail) &&
                        executor.executor->counters().ocioEffectDispatches ==
                            coldCounters.ocioEffectDispatches,
                    "blur native: warm RAM reuse has identical pixels and zero filter dispatches");
                // #460: a sub-pixel radius exercises the fractional Box edge taps (and a tiny
                // Gaussian) on the resident route against the CPU reference.
                std::uint64_t changedRuns = 0;
                for (const double radius : {1.0, 0.25}) {
                    auto changedDefinition = plan->copyDefinition();
                    std::get<SeparableBlurKernel>(
                        std::get<CompiledImageEffect>(changedDefinition.operations[1]).kernel)
                        .radius.source = radius;
                    const auto changedPlan = std::make_shared<const CompiledCompositionPlan>(
                        std::move(changedDefinition));
                    const auto changed = builder.build(changedPlan, request);
                    const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
                    // The reason is part of the message: a preparation that fails only on a
                    // busy machine (#623) has to say why.
                    expectations.expect(
                        changed && changedOracle.frame(),
                        "blur native: changed radius prepares: " + changed.diagnostic.message +
                            (changedOracle.frame() ? "" : " (no CPU frame)"));
                    if (!changed || !changedOracle.frame())
                        continue;
                    const auto rerun = runScene(*executor.executor, changed.scene);
                    ++changedRuns;
                    expectations.expect(
                        rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                            parity(rerun.pixels, changedOracle.frame()->processImage().pixels(),
                                   detail) &&
                            executor.executor->counters().ocioEffectDispatches ==
                                coldCounters.ocioEffectDispatches + 2 * changedRuns,
                        "blur native: changed radius " + std::to_string(radius) +
                            " invalidates both passes and retains CPU parity: " + detail);
                }
            }
        }
    }
}

// #340 Levels, Curves and Hue/Saturation (#493: and every Curves curve kind; #494 Grade; #503
// every Alpha Association mode; #504 every Colorize mode; #505 both Vignette shapes and modes) of
// signed/HDR/partial-alpha solids against the CPU oracle
// (2e-6, exact alpha), one resident dispatch, warm zero dispatch and a changed value that
// re-dispatches with parity. Alpha transfers here are linear and cubic, never a gamma, so the
// alpha stays bit-exact.
void runColorGrades(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::runtime::CurvesKernel;
    using bloom::runtime::HueSaturationKernel;
    using bloom::runtime::LevelsKernel;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    const auto levels = [](const std::uint64_t id, const bool clamp) {
        LevelsKernel kernel;
        const std::array<std::array<double, 5>, 5> values{{{-0.25, 2.0, 1.8, 0.0, 1.0},
                                                           {0.0, 1.0, 0.5, 0.1, 3.0},
                                                           {0.2, 0.8, 1.0, -1.0, 1.0},
                                                           {1.0, 0.0, 2.2, 0.0, 1.0},
                                                           {0.0, 1.0, 1.0, 0.2, 0.9}}};
        for (std::size_t c = 0; c < 5; ++c)
            kernel.channels[c] = {{ParameterId::fromRaw(id + 5 * c), values[c][0]},
                                  {ParameterId::fromRaw(id + 5 * c + 1), values[c][1]},
                                  {ParameterId::fromRaw(id + 5 * c + 2), values[c][2]},
                                  {ParameterId::fromRaw(id + 5 * c + 3), values[c][3]},
                                  {ParameterId::fromRaw(id + 5 * c + 4), values[c][4]}};
        kernel.clamp = clamp;
        return bloom::runtime::ImageEffectKernel{kernel};
    };
    const auto curves = [](const std::uint64_t id, const bool clamp) {
        using bloom::document::GradeCurve;
        using bloom::runtime::testing::legacyPoints;
        auto kernel = bloom::runtime::testing::identityCurvesKernel(id, clamp);
        bloom::runtime::testing::setCurve(kernel, GradeCurve::Master,
                                          legacyPoints({{1, {0.25, 0.15}}, {3, {0.75, 0.85}}}));
        bloom::runtime::testing::setCurve(kernel, GradeCurve::Red,
                                          legacyPoints({{2, {0.4, 0.7}}}));
        bloom::runtime::testing::setCurve(kernel, GradeCurve::Blue,
                                          legacyPoints({{4, {1.0, 1.75}}}));
        bloom::runtime::testing::setCurve(kernel, GradeCurve::Alpha,
                                          legacyPoints({{0, {0.0, 0.2}}}));
        return bloom::runtime::ImageEffectKernel{kernel};
    };
    // #493: many points on one curve and the four hue curves together (each kind on its own in
    // the coverage fixtures), with the periodic wrap between the last and first hue points.
    const auto hueCurves = [](const std::uint64_t id, const bool clamp) {
        using bloom::document::GradeCurve;
        auto kernel = bloom::runtime::testing::identityCurvesKernel(id, clamp);
        bloom::runtime::testing::setCurve(kernel, GradeCurve::Green,
                                          {{0.0, 0.0}, {0.1, 0.2}, {0.2, 0.25}, {0.35, 0.3},
                                           {0.5, 0.6}, {0.65, 0.62}, {0.8, 0.9}, {1.0, 1.0},
                                           {3.0, 2.0}});
        bloom::runtime::testing::setCurve(kernel, GradeCurve::HueVsHue,
                                          {{0.05, 0.1}, {0.4, -0.15}, {0.9, 0.2}});
        bloom::runtime::testing::setCurve(kernel, GradeCurve::HueVsSaturation,
                                          {{0.2, 1.6}, {0.6, 0.4}});
        bloom::runtime::testing::setCurve(kernel, GradeCurve::HueVsLuma,
                                          {{0.0, 1.4}, {0.5, 0.7}, {0.75, 1.1}});
        bloom::runtime::testing::setCurve(kernel, GradeCurve::LumaVsSaturation,
                                          {{0.0, 1.3}, {0.6, 0.5}, {2.0, 1.0}});
        return bloom::runtime::ImageEffectKernel{kernel};
    };
    const auto hue = [](const std::uint64_t id, const bool oversaturate) {
        return bloom::runtime::ImageEffectKernel{
            HueSaturationKernel{{ParameterId::fromRaw(id), oversaturate ? 200.0 : -70.0},
                                {ParameterId::fromRaw(id + 1), oversaturate ? 2.5 : 0.3}}};
    };
    // Vibrance at both signs with a hue turn, and Colorize with a vibrance.
    const auto vibrance = [](const std::uint64_t id, const bool colorize) {
        HueSaturationKernel kernel{{ParameterId::fromRaw(id), colorize ? 200.0 : 40.0},
                                   {ParameterId::fromRaw(id + 1), colorize ? 0.7 : 1.4},
                                   {ParameterId::fromRaw(id + 2), colorize ? -0.4 : 0.8},
                                   colorize};
        return bloom::runtime::ImageEffectKernel{kernel};
    };
    // #494: every Grade control, with contrast and saturation on or off.
    const auto grade = [](const std::uint64_t id, const bool primaries) {
        bloom::runtime::GradeKernel kernel;
        const std::array<double, 22> values{0.05,
                                            0.02,
                                            -0.01,
                                            0.0,
                                            0.2,
                                            -0.1,
                                            0.0,
                                            0.3,
                                            1.1,
                                            1.05,
                                            0.95,
                                            1.0,
                                            0.01,
                                            0.0,
                                            0.02,
                                            -0.01,
                                            primaries ? 30.0 : 0.0,
                                            primaries ? -20.0 : 0.0,
                                            primaries ? 1.4 : 1.0,
                                            0.18,
                                            primaries ? 1.3 : 1.0,
                                            primaries ? 0.5 : 0.0};
        for (std::size_t index = 0; index < values.size(); ++index)
            kernel.values[index] = {ParameterId::fromRaw(id + index), values[index]};
        return bloom::runtime::ImageEffectKernel{kernel};
    };
    // #503: every Alpha Association mode (Unmult over black and white, Premultiply,
    // Unpremultiply, Remove Color Matting against an HDR colour).
    const auto association = [](const std::uint64_t id, const int variant) {
        using Mode = bloom::runtime::AlphaAssociationMode;
        const std::array<Mode, 5> modes{Mode::Unmult, Mode::Unmult, Mode::Premultiply,
                                        Mode::Unpremultiply, Mode::RemoveMatting};
        return bloom::runtime::ImageEffectKernel{bloom::runtime::AlphaAssociationKernel{
            modes[static_cast<std::size_t>(variant)],
            variant == 1,
            {ParameterId::fromRaw(id),
             variant == 4 ? Color4d{0.25, 0.5, 1.5, 1.0} : Color4d{0.0, 0.0, 0.0, 1.0}}}};
    };
    // #504: every Colorize mode, a partial amount, both cycles and a channel source; #541 a
    // Gradient Map through a bound multi-stop gradient with a hard edge; #546 that gradient in
    // every other interpolation (variants 7 to 11) and in HSV and HSL with every hue direction
    // (variants 12 to 19).
    const auto colorize = [](const std::uint64_t id, const int variant) {
        using bloom::runtime::ColorizeCycle;
        using bloom::runtime::ColorizeMode;
        bloom::runtime::ColorizeKernel kernel;
        const std::array<ColorizeMode, 7> modes{
            ColorizeMode::Fill,        ColorizeMode::Tint,          ColorizeMode::Tritone,
            ColorizeMode::GradientMap, ColorizeMode::GradientMap,   ColorizeMode::BlackAndWhite,
            ColorizeMode::GradientMap};
        kernel.mode =
            variant >= 6 ? ColorizeMode::GradientMap : modes[static_cast<std::size_t>(variant)];
        kernel.amount = {ParameterId::fromRaw(id), variant == 2 ? 0.6 : 1.0};
        kernel.color = {ParameterId::fromRaw(id + 1), Color4d{0.9, 0.3, 1.5, 1.0}};
        kernel.shadows = {ParameterId::fromRaw(id + 2), Color4d{0.05, -0.02, 0.2, 1.0}};
        kernel.midtones = {ParameterId::fromRaw(id + 3), Color4d{0.6, 0.2, -0.1, 1.0}};
        kernel.highlights = {ParameterId::fromRaw(id + 4), Color4d{2.0, 0.9, 0.4, 1.0}};
        kernel.midpoint = {ParameterId::fromRaw(id + 5), 0.3};
        kernel.repetitions = {ParameterId::fromRaw(id + 6), 2.5};
        kernel.phase = {ParameterId::fromRaw(id + 7), -0.3};
        kernel.source = variant == 3 ? bloom::runtime::ColorizeSource::Red
                                     : bloom::runtime::ColorizeSource::Luma;
        kernel.cycle = variant == 3 ? ColorizeCycle::Mirror : ColorizeCycle::Repeat;
        const std::array<double, 6> weights{-1.5, 2.5, 0.1, 3.0, -2.0, 1.2};
        for (std::size_t i = 0; i < weights.size(); ++i)
            kernel.weights[i] = {ParameterId::fromRaw(id + 8 + i), weights[i]};
        if (variant >= 6) {
            bloom::document::GradientValue gradient{{{0.8, Color4d{1.2, 0.9, 0.1, 1.0}},
                                                     {0.1, Color4d{0.0, -0.05, 0.3, 1.0}},
                                                     {0.5, Color4d{0.1, 0.6, 0.2, 1.0}},
                                                     {0.5, Color4d{0.9, 0.1, -0.2, 1.0}},
                                                     {0.95, Color4d{3.0, 2.0, 1.5, 1.0}}}};
            using bloom::document::GradientColorMode;
            using bloom::document::GradientInterpolation;
            if (variant >= 7 && variant <= 11) {
                constexpr std::array<GradientInterpolation, 5> curves{
                    GradientInterpolation::Constant, GradientInterpolation::Ease,
                    GradientInterpolation::Cardinal, GradientInterpolation::BSpline,
                    GradientInterpolation::Exponential};
                gradient.modes.interpolation = curves[static_cast<std::size_t>(variant - 7)];
            } else if (variant >= 12) {
                gradient.modes.colorMode =
                    variant < 16 ? GradientColorMode::Hsv : GradientColorMode::Hsl;
                gradient.modes.hueDirection =
                    static_cast<bloom::document::GradientHueDirection>((variant - 12) % 4);
            } else {
                gradient.stops.front().position = 0.8;
                gradient.stops[1].position = 0.0;
                gradient.stops.back().position = 1.0;
            }
            kernel.gradient =
                bloom::runtime::CompiledGradientParameter{ParameterId::fromRaw(id + 20), gradient};
        }
        return bloom::runtime::ImageEffectKernel{kernel};
    };
    // #505: both shapes and modes, roundness both ways, a moved centre and highlight protection.
    const auto vignette = [](const std::uint64_t id, const int variant) {
        using bloom::runtime::VignetteMode;
        using bloom::runtime::VignetteShape;
        bloom::runtime::VignetteKernel kernel;
        kernel.shape = variant % 2 == 1 ? VignetteShape::Rectangular : VignetteShape::Elliptical;
        kernel.mode = variant >= 2 ? VignetteMode::Exposure : VignetteMode::Multiply;
        kernel.amount = {ParameterId::fromRaw(id), variant == 1 ? -0.6 : 0.8};
        kernel.exposure = {ParameterId::fromRaw(id + 1), variant == 3 ? 1.5 : -2.5};
        kernel.midpoint = {ParameterId::fromRaw(id + 2), 0.3};
        kernel.roundness = {ParameterId::fromRaw(id + 3), variant == 1 ? -0.5 : 0.6};
        kernel.feather = {ParameterId::fromRaw(id + 4), 0.7};
        kernel.highlightProtection = {ParameterId::fromRaw(id + 5), variant == 2 ? 0.8 : 0.0};
        kernel.center = {ParameterId::fromRaw(id + 6),
                         bloom::document::Vec2d{variant == 0 ? 0.0 : 2.5, -1.5}};
        return bloom::runtime::ImageEffectKernel{kernel};
    };
    // #518 Tone Map: each roll-off method with shadow recovery, a hard clip, and gamut compression.
    const auto toneMap = [](const std::uint64_t id, const int variant) {
        bloom::color_tool_test::ToneMapSettings settings;
        settings.knee = 0.5;
        settings.ceiling = 2.0;
        settings.shadows = 0.75;
        if (variant == 1)
            settings.method = bloom::document::kToneMapBrightestChannel;
        if (variant == 2) {
            settings.knee = 1.0;
            settings.ceiling = 0.75;
            settings.shadows = -0.5;
        }
        if (variant == 3) {
            settings.gamut = true;
            settings.method = bloom::document::kToneMapBrightestChannel;
        }
        return bloom::runtime::ImageEffectKernel{bloom::color_tool_test::toneMap(id, settings)};
    };
    // #517 Color Matrix: variants 0 to 3 write HSL, HSV, YCbCr and Lab, 4 to 7 read them back (a
    // swap preset between), 8 is a 4 x 4 mix with alpha from color and 9 the sepia preset.
    const auto colorMatrix = [](const std::uint64_t id, const int variant) {
        bloom::color_tool_test::ColorMatrixSettings settings;
        if (variant < 4) {
            settings.outputModel = variant + 1;
        } else if (variant < 8) {
            settings.inputModel = variant - 3;
            settings.preset = 3;
        } else if (variant == 8) {
            settings.size = bloom::document::kColorMatrixSize4;
            settings.matrix = {0.5, 0.5, 0, 0.25, 0, 1, 0, 0, 2,
                               0,   -1,  0, 0.2126, 0.7152, 0.0722, 0};
        } else {
            settings.preset = 6;
        }
        return bloom::runtime::ImageEffectKernel{bloom::color_tool_test::colorMatrix(id, settings)};
    };
    // #523 HSL Qualifier: 0 a soft hue window across the wrap, 1 inverted saturation and luma
    // windows, 2 hard edges.
    const auto hslQualifier = [](const std::uint64_t id, const int variant) {
        bloom::color_tool_test::HslQualifierSettings settings;
        if (variant == 0) {
            settings.hueCenter = 350.0;
            settings.hueWidth = 60.0;
            settings.hueSoftness = 40.0;
        } else if (variant == 1) {
            settings.satLow = 0.2;
            settings.satHigh = 0.9;
            settings.lumaLow = 0.1;
            settings.lumaHigh = 2.0;
            settings.lumaSoftness = 0.5;
            settings.invert = true;
        } else {
            settings.hueCenter = 120.0;
            settings.hueWidth = 90.0;
            settings.hueSoftness = 0.0;
            settings.satSoftness = 0.0;
            settings.lumaSoftness = 0.0;
        }
        return bloom::runtime::ImageEffectKernel{bloom::color_tool_test::hslQualifier(id, settings)};
    };
    // #519 Quantize: 0 a hard per-channel threshold, 1 a soft inverted luma threshold, 2 a linked
    // posterize of five levels, 3 an unlinked posterize with alpha levels, 4 256 levels. The boundary
    // colours below put a channel exactly at the level or at a half-way step and one ulp either side.
    const auto quantize = [](const std::uint64_t id, const int variant) {
        bloom::color_tool_test::QuantizeSettings settings;
        if (variant == 0) {
            settings.basis = bloom::document::kQuantizeBasisPerChannel;
            settings.level = 0.5;
        } else if (variant == 1) {
            settings.level = 0.4;
            settings.softness = 0.3;
            settings.invert = true;
        } else if (variant == 2) {
            settings.mode = bloom::document::kQuantizePosterize;
            settings.levels = 5.0;
        } else if (variant == 3) {
            settings.mode = bloom::document::kQuantizePosterize;
            settings.link = false;
            settings.red = 2.0;
            settings.green = 5.0;
            settings.blue = 256.0;
            settings.alpha = true;
            settings.alphaLevels = 4.0;
        } else {
            settings.mode = bloom::document::kQuantizePosterize;
            settings.levels = 256.0;
        }
        // The list is built once per colour while each kernel gets its own plan, so each variant owns
        // parameter identities clear of every plan's range.
        return bloom::runtime::ImageEffectKernel{bloom::color_tool_test::quantize(
            id + 900000 + static_cast<std::uint64_t>(variant) * 10, settings)};
    };
    // Changes one value of each kernel to prove invalidation re-dispatches.
    const auto change = [](bloom::runtime::ImageEffectKernel& kernel) {
        if (auto* l = std::get_if<LevelsKernel>(&kernel))
            l->channels[0].outputWhite.source = 1.5;
        if (auto* c = std::get_if<CurvesKernel>(&kernel))
            bloom::runtime::testing::setCurve(
                *c, bloom::document::GradeCurve::Master,
                bloom::runtime::testing::legacyPoints({{2, {0.5, 0.65}}}));
        if (auto* h = std::get_if<HueSaturationKernel>(&kernel))
            h->hue.source = 15.0;
        if (auto* g = std::get_if<bloom::runtime::GradeKernel>(&kernel))
            g->values[8].source = 1.25;
        // #503: a changed matte colour, or a different mode, changes the program's data.
        // #504: a changed amount changes the program's data in every mode.
        if (auto* c = std::get_if<bloom::runtime::ColorizeKernel>(&kernel))
            c->amount.source = 0.4;
        // #505: a moved centre changes the program's data in every mode.
        if (auto* v = std::get_if<bloom::runtime::VignetteKernel>(&kernel))
            v->center.source = bloom::document::Vec2d{-3.0, 2.0};
        // #518: a moved ceiling changes the program's data in every variant.
        if (auto* t = std::get_if<bloom::runtime::ColorToolKernel>(&kernel)) {
            if (t->operation == bloom::runtime::ColorToolOperation::Quantize) {
                // The level, the master levels and the red levels, whichever the variant reads.
                t->values[0].source = 0.75;
                t->values[2].source = 6.0;
                t->values[3].source = 3.0;
            } else if (t->operation == bloom::runtime::ColorToolOperation::HslQualifier) {
                t->values[1].source = 120.0;
            } else if (t->operation == bloom::runtime::ColorToolOperation::ColorMatrix) {
                t->choices[0] = 0;
                t->values[1].source = 0.75;
            } else {
                t->values[1].source = 3.5;
            }
        }
        if (auto* a = std::get_if<bloom::runtime::AlphaAssociationKernel>(&kernel)) {
            if (a->mode == bloom::runtime::AlphaAssociationMode::RemoveMatting)
                a->background.source = Color4d{0.75, 0.1, 0.3, 1.0};
            else if (a->mode == bloom::runtime::AlphaAssociationMode::Unmult)
                a->white = !a->white;
            else
                a->mode = a->mode == bloom::runtime::AlphaAssociationMode::Premultiply
                              ? bloom::runtime::AlphaAssociationMode::Unpremultiply
                              : bloom::runtime::AlphaAssociationMode::Premultiply;
        }
    };
    std::uint64_t id = 9500;
    // Straight colours with a channel exactly at the threshold level 0.5 and at a half-way step of five
    // levels (0.125), and one ulp either side: only Quantize reads them (a Quantize kernel also runs
    // over the three colours every grade runs over).
    const auto ulpUp = [](const double v) {
        return static_cast<double>(std::nextafter(static_cast<float>(v), 1.0F));
    };
    const auto ulpDown = [](const double v) {
        return static_cast<double>(std::nextafter(static_cast<float>(v), 0.0F));
    };
    const std::array<Color4d, 5> colours{
        Color4d{-0.25, 1.75, 0.5, 0.625}, Color4d{4.0, 3.0, 6.0, 1.0}, Color4d{0.3, 0.6, 0.2, 0.25},
        Color4d{0.5, ulpUp(0.5), ulpDown(0.5), 1.0}, Color4d{0.125, ulpUp(0.125), ulpDown(0.125), 1.0}};
    for (std::size_t colourIndex = 0; colourIndex < colours.size(); ++colourIndex) {
        const auto color = colours[colourIndex];
        const bool boundary = colourIndex >= 3;
        for (const auto& kernel :
             {levels(id + 50, false),  levels(id + 50, true),     curves(id + 50, false),
              curves(id + 50, true),   hueCurves(id + 50, false), hueCurves(id + 50, true),
              hue(id + 50, false),     hue(id + 50, true),        vibrance(id + 50, false),
              vibrance(id + 50, true),        grade(id + 50, false),
              grade(id + 50, true),    association(id + 50, 0),   association(id + 50, 1),
              association(id + 50, 2), association(id + 50, 3),   association(id + 50, 4),
              colorize(id + 50, 0),    colorize(id + 50, 1),      colorize(id + 50, 2),
              colorize(id + 50, 3),    colorize(id + 50, 4),      colorize(id + 50, 5),
              colorize(id + 50, 6),    colorize(id + 50, 7),      colorize(id + 50, 8),
              colorize(id + 50, 9),    colorize(id + 50, 10),     colorize(id + 50, 11),
              colorize(id + 50, 12),   colorize(id + 50, 13),     colorize(id + 50, 14),
              colorize(id + 50, 15),   colorize(id + 50, 16),     colorize(id + 50, 17),
              colorize(id + 50, 18),   colorize(id + 50, 19),     vignette(id + 50, 0),
              vignette(id + 50, 1),    vignette(id + 50, 2),      vignette(id + 50, 3),
              toneMap(id + 50, 0),     toneMap(id + 50, 1),       toneMap(id + 50, 2),
              toneMap(id + 50, 3),     colorMatrix(id + 50, 0),   colorMatrix(id + 50, 1),
              colorMatrix(id + 50, 2), colorMatrix(id + 50, 3),   colorMatrix(id + 50, 4),
              colorMatrix(id + 50, 5), colorMatrix(id + 50, 6),   colorMatrix(id + 50, 7),
              colorMatrix(id + 50, 8), colorMatrix(id + 50, 9), hslQualifier(id + 50, 0),
              hslQualifier(id + 50, 1), hslQualifier(id + 50, 2), quantize(id + 50, 0),
              quantize(id + 50, 1),    quantize(id + 50, 2),      quantize(id + 50, 3),
              quantize(id + 50, 4)}) {
            const auto* tool = std::get_if<bloom::runtime::ColorToolKernel>(&kernel);
            const bool isQuantize =
                tool != nullptr && tool->operation == bloom::runtime::ColorToolOperation::Quantize;
            if (boundary && !isQuantize)
                continue;
            cache.clear();
            const auto plan = effectPlan(kernel, id, std::nullopt, color);
            id += 100;
            const auto request = requestFor(*plan);
            auto oracleRequest = request;
            oracleRequest.bypassOperationCache = true;
            const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
            const auto prepared = builder.build(plan, request);
            expectations.expect(oracle.frame() && prepared,
                                "grade native: CPU oracle and production scene prepare");
            if (!oracle.frame() || !prepared)
                continue;
            auto executor = GpuSceneExecutor::create(device, cache);
            expectations.expect(executor.hasValue(), "grade native: executor creates");
            if (!executor)
                continue;
            const auto cold = runScene(*executor.executor, prepared.scene);
            const auto coldCounters = executor.executor->counters();
            std::string detail;
            expectations.expect(
                cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                    parity(cold.pixels, oracle.frame()->processImage().pixels(), detail),
                "grade native: HDR/partial-alpha CPU parity (2e-6, exact alpha): " + detail);
            const auto& descriptor = *oracle.frame()->processImage().descriptor();
            expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                    cold.displayWindow == descriptor.displayWindow() &&
                                    cold.pixelAspect == descriptor.pixelAspect(),
                                "grade native: data/display-window and PAR parity");
            expectations.expect(coldCounters.ocioEffectDispatches == 1 &&
                                    coldCounters.readbacks == 0,
                                "grade native: one resident dispatch and no frame readback");
            const auto warm = runScene(*executor.executor, prepared.scene);
            expectations.expect(
                warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                    parity(warm.pixels, oracle.frame()->processImage().pixels(), detail) &&
                    executor.executor->counters().ocioEffectDispatches ==
                        coldCounters.ocioEffectDispatches,
                "grade native: warm RAM reuse has identical pixels and zero dispatches");
            auto changedDefinition = plan->copyDefinition();
            change(std::get<CompiledImageEffect>(changedDefinition.operations[1]).kernel);
            const auto changedPlan =
                std::make_shared<const CompiledCompositionPlan>(std::move(changedDefinition));
            const auto changed = builder.build(changedPlan, request);
            const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
            expectations.expect(changed && changedOracle.frame(),
                                "grade native: changed value prepares");
            if (!changed || !changedOracle.frame())
                continue;
            const auto rerun = runScene(*executor.executor, changed.scene);
            expectations.expect(
                rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                    parity(rerun.pixels, changedOracle.frame()->processImage().pixels(), detail) &&
                    executor.executor->counters().ocioEffectDispatches ==
                        coldCounters.ocioEffectDispatches + 1,
                "grade native: a changed value re-dispatches with CPU parity: " + detail);
        }
    }
}

// #481 Image Math (a number, then a second image of the same, a smaller and a larger window),
// Image Map Range and Image Clamp, straight and stored, over signed/HDR/partial-alpha solids
// against the CPU oracle (2e-6, exact alpha), one resident dispatch, warm zero dispatch and a
// changed value that re-dispatches with parity. Power acts on colour only: alpha stays bit-exact.
[[nodiscard]] std::shared_ptr<const CompiledCompositionPlan>
pixelMathSecondPlan(const bloom::runtime::PixelMathKernel& kernel, const std::uint64_t idBase,
                    const Color4d base, const Color4d second, const double secondWidth,
                    const double secondHeight) {
    constexpr std::uint64_t layerRaw = 1;
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledSolid{NodeId::fromRaw(idBase + 1),
                                          {ParameterId::fromRaw(idBase + 2), base},
                                          {ParameterId::fromRaw(idBase + 3), 12.0},
                                          {ParameterId::fromRaw(idBase + 4), 10.0}});
    operations.emplace_back(CompiledSolid{NodeId::fromRaw(idBase + 16),
                                          {ParameterId::fromRaw(idBase + 17), second},
                                          {ParameterId::fromRaw(idBase + 18), secondWidth},
                                          {ParameterId::fromRaw(idBase + 19), secondHeight}});
    operations.emplace_back(CompiledImageEffect{NodeId::fromRaw(idBase + 5),
                                                OperationIndex::fromRaw(0),
                                                bloom::runtime::ImageEffectKernel{kernel}, false,
                                                false, OperationIndex::fromRaw(1)});
    operations.emplace_back(CompiledLayerOutput{
        NodeId::fromRaw(idBase + 6), LayerId::fromRaw(layerRaw), OperationIndex::fromRaw(2),
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 7), bloom::document::Vec2d{6.0, 5.0}},
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 8), bloom::document::Vec2d{0.0, 0.0}},
        CompiledVec2Parameter{ParameterId::fromRaw(idBase + 9), bloom::document::Vec2d{1.0, 1.0}},
        CompiledScalarParameter{ParameterId::fromRaw(idBase + 10), 0.0},
        CompiledScalarParameter{ParameterId::fromRaw(idBase + 11), 1.0},
        ParameterId::fromRaw(idBase + 12), bloom::core::kDefaultBlendMode});
    operations.emplace_back(CompiledMerge{
        NodeId::fromRaw(idBase + 13),
        std::vector<CompiledMergeInput>{CompiledMergeInput{LayerSlotId::fromRaw(idBase + 14),
                                                           LayerId::fromRaw(layerRaw),
                                                           OperationIndex::fromRaw(3)}}});
    operations.emplace_back(
        CompiledCompositionOutput{NodeId::fromRaw(idBase + 15), OperationIndex::fromRaw(4)});
    CompiledCompositionPlanDefinition definition{bloom::document::Revision::fromRaw(kRevision),
                                                 kProject,
                                                 CompositionId::fromRaw(idBase + 100),
                                                 format(12, 10),
                                                 std::move(operations),
                                                 OperationIndex::fromRaw(5)};
    definition.duration = RationalTime::fromInteger(100);
    return std::make_shared<const CompiledCompositionPlan>(std::move(definition));
}

void runPixelMaths(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    namespace pm = bloom::pixel_math_test;
    namespace doc = bloom::document;
    using Op = bloom::runtime::PixelMathOperation;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    struct Case final {
        bloom::runtime::PixelMathKernel kernel;
        double secondWidth = 0.0;
        double secondHeight = 0.0;
    };
    std::vector<Case> cases;
    std::uint64_t kernelId = 0;
    for (const auto op : {Op::Add, Op::Subtract, Op::Multiply, Op::Divide, Op::Power, Op::Minimum,
                          Op::Maximum, Op::Absolute})
        for (const bool straight : {true, false})
            cases.push_back({pm::math(kernelId += 100, op, op == Op::Power ? 1.5 : -0.4,
                                      op == Op::Power ? doc::kImagePixelColor
                                                      : doc::kImagePixelColorAndAlpha,
                                      straight)});
    for (const auto channels : {doc::kImagePixelRed, doc::kImagePixelGreen, doc::kImagePixelBlue,
                                doc::kImagePixelAlpha})
        cases.push_back({pm::math(kernelId += 100, Op::Multiply, 0.5, channels, true)});
    for (const auto op : {Op::Add, Op::Subtract, Op::Multiply, Op::Divide, Op::Minimum, Op::Maximum})
        for (const auto& [w, h] : {std::pair{12.0, 10.0}, std::pair{8.0, 6.0},
                                   std::pair{16.0, 14.0}}) {
            cases.push_back({pm::math(kernelId += 100, op, 0.0, doc::kImagePixelColorAndAlpha,
                                      op != Op::Maximum, true),
                             w, h});
        }
    cases.push_back({pm::math(kernelId += 100, Op::Power, 0.0, doc::kImagePixelColor, true, true),
                     8.0, 6.0});
    // #349 Image Compare: each mode against a second image of the same, a smaller and a larger
    // window, and the split at several positions and angles (the display window lays it out).
    for (const auto& [w, h] : {std::pair{12.0, 10.0}, std::pair{8.0, 6.0}, std::pair{16.0, 14.0}}) {
        cases.push_back({pm::compare(kernelId += 100, doc::kImageCompareDifference, 3.0, 0.5, 0.0, 4.0),
                         w, h});
        cases.push_back({pm::compare(kernelId += 100, doc::kImageCompareSplit, 1.0, 0.35, 25.0, 4.0),
                         w, h});
        cases.push_back({pm::compare(kernelId += 100, doc::kImageCompareSplit, 1.0, 0.8, -140.0, 4.0),
                         w, h});
        cases.push_back({pm::compare(kernelId += 100, doc::kImageCompareCheckerboard, 1.0, 0.5, 0.0, 3.0),
                         w, h});
    }
    cases.push_back({pm::mapRange(kernelId += 100, 0.0, 4.0, 0.0, 1.0, false, doc::kImagePixelColor,
                                  true)});
    cases.push_back({pm::mapRange(kernelId += 100, -1.0, 2.0, 5.0, -5.0, true,
                                  doc::kImagePixelColorAndAlpha, false)});
    cases.push_back({pm::mapRange(kernelId += 100, 1.0, 1.0, 0.25, 0.75, false,
                                  doc::kImagePixelRed, true)});
    cases.push_back({pm::clampKernel(kernelId += 100, {0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0},
                                     true)});
    cases.push_back({pm::clampKernel(kernelId += 100, {-0.5, 0.5, 0.25, 2.0, 1.0, 0.0, 0.1, 0.9},
                                     false)});
    std::uint64_t id = 11500;
    const std::array<Color4d, 3> colors{Color4d{-0.25, 1.75, 0.5, 0.625}, Color4d{4.0, 3.0, 6.0, 1.0},
                                        Color4d{0.3, 0.6, 0.2, 0.25}};
    for (const auto color : colors) {
        for (const auto& item : cases) {
            cache.clear();
            const auto plan =
                item.kernel.second
                    ? pixelMathSecondPlan(item.kernel, id, color,
                                          Color4d{0.75, -0.25, 2.0, 0.6}, item.secondWidth,
                                          item.secondHeight)
                    : effectPlan(bloom::runtime::ImageEffectKernel{item.kernel}, id, std::nullopt,
                                 color);
            id += 100;
            const auto request = requestFor(*plan);
            auto oracleRequest = request;
            oracleRequest.bypassOperationCache = true;
            const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
            const auto prepared = builder.build(plan, request);
            expectations.expect(oracle.frame() && prepared,
                                "pixel math native: CPU oracle and production scene prepare");
            if (!oracle.frame() || !prepared)
                continue;
            auto executor = GpuSceneExecutor::create(device, cache);
            expectations.expect(executor.hasValue(), "pixel math native: executor creates");
            if (!executor)
                continue;
            const auto cold = runScene(*executor.executor, prepared.scene);
            const auto coldCounters = executor.executor->counters();
            std::string detail;
            // A power's backend logarithm may differ in its last bits, which alpha never takes part
            // in; every other operation is bit-exact in alpha.
            expectations.expect(
                cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                    parity(cold.pixels, oracle.frame()->processImage().pixels(), detail),
                "pixel math native: HDR/partial-alpha CPU parity (2e-6, exact alpha): " + detail);
            const auto& descriptor = *oracle.frame()->processImage().descriptor();
            expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                    cold.displayWindow == descriptor.displayWindow() &&
                                    cold.pixelAspect == descriptor.pixelAspect(),
                                "pixel math native: data/display-window and PAR parity");
            expectations.expect(coldCounters.ocioEffectDispatches == 1 &&
                                    coldCounters.readbacks == 0,
                                "pixel math native: one resident dispatch and no frame readback");
            const auto warm = runScene(*executor.executor, prepared.scene);
            expectations.expect(
                warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                    parity(warm.pixels, oracle.frame()->processImage().pixels(), detail) &&
                    executor.executor->counters().ocioEffectDispatches ==
                        coldCounters.ocioEffectDispatches,
                "pixel math native: warm RAM reuse has identical pixels and zero dispatches");
            // A changed value changes the program's data: its first operand, or for a second image
            // the straight and stored choice.
            auto changedDefinition = plan->copyDefinition();
            auto& changedKernel = std::get<bloom::runtime::PixelMathKernel>(
                std::get<CompiledImageEffect>(changedDefinition.operations[item.kernel.second ? 2 : 1])
                    .kernel);
            if (changedKernel.operation == Op::Compare)
                changedKernel.values[1].source = 0.6;
            else if (item.kernel.second)
                changedKernel.straight = !changedKernel.straight;
            else if (changedKernel.operation == Op::Absolute)
                changedKernel.straight = !changedKernel.straight;
            else
                changedKernel.values[0].source = 0.7;
            const auto changedPlan =
                std::make_shared<const CompiledCompositionPlan>(std::move(changedDefinition));
            const auto changed = builder.build(changedPlan, request);
            const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
            expectations.expect(changed && changedOracle.frame(),
                                "pixel math native: changed value prepares");
            if (!changed || !changedOracle.frame())
                continue;
            const auto rerun = runScene(*executor.executor, changed.scene);
            expectations.expect(
                rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                    parity(rerun.pixels, changedOracle.frame()->processImage().pixels(), detail) &&
                    executor.executor->counters().ocioEffectDispatches ==
                        coldCounters.ocioEffectDispatches + 1,
                "pixel math native: a changed value re-dispatches with CPU parity: " + detail);
        }
    }
}

// #479 Channel Select (each Separate Channels output and Combine Channels' base) of
// signed/HDR/partial-alpha solids against the CPU oracle: grey stored channels, a swizzle with a
// Luma read and a premultiplied Straight alpha; one resident dispatch, warm zero dispatch and a
// changed mapping that re-dispatches with parity.
void runChannelSelects(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::runtime::ChannelSelectKernel;
    using Source = bloom::runtime::ImageChannelSource;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    const std::array<ChannelSelectKernel, 4> kernels{
        ChannelSelectKernel{{Source::Red, Source::Red, Source::Red, Source::One}, false},
        ChannelSelectKernel{{Source::Alpha, Source::Alpha, Source::Alpha, Source::One}, false},
        ChannelSelectKernel{{Source::Blue, Source::Luma, Source::Zero, Source::Alpha}, false},
        ChannelSelectKernel{{Source::Green, Source::Red, Source::Blue, Source::Luma}, true}};
    std::uint64_t id = 9800;
    for (const auto color : {Color4d{-0.25, 1.75, 0.5, 0.625}, Color4d{4.0, 3.0, 6.0, 1.0},
                             Color4d{0.3, 0.6, 0.2, 0.25}}) {
        for (const auto& kernel : kernels) {
            cache.clear();
            const auto plan = effectPlan(kernel, id, std::nullopt, color);
            id += 100;
            const auto request = requestFor(*plan);
            auto oracleRequest = request;
            oracleRequest.bypassOperationCache = true;
            const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
            const auto prepared = builder.build(plan, request);
            expectations.expect(oracle.frame() && prepared,
                                "channel native: CPU oracle and production scene prepare");
            if (!oracle.frame() || !prepared)
                continue;
            auto executor = GpuSceneExecutor::create(device, cache);
            expectations.expect(executor.hasValue(), "channel native: executor creates");
            if (!executor)
                continue;
            const auto cold = runScene(*executor.executor, prepared.scene);
            const auto coldCounters = executor.executor->counters();
            std::string detail;
            expectations.expect(
                cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                    parity(cold.pixels, oracle.frame()->processImage().pixels(), detail),
                "channel native: HDR/partial-alpha CPU parity (2e-6, exact alpha): " + detail);
            const auto& descriptor = *oracle.frame()->processImage().descriptor();
            expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                    cold.displayWindow == descriptor.displayWindow() &&
                                    cold.pixelAspect == descriptor.pixelAspect(),
                                "channel native: data/display-window and PAR parity");
            expectations.expect(coldCounters.ocioEffectDispatches == 1 &&
                                    coldCounters.readbacks == 0,
                                "channel native: one resident dispatch and no frame readback");
            const auto warm = runScene(*executor.executor, prepared.scene);
            expectations.expect(
                warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                    parity(warm.pixels, oracle.frame()->processImage().pixels(), detail) &&
                    executor.executor->counters().ocioEffectDispatches ==
                        coldCounters.ocioEffectDispatches,
                "channel native: warm RAM reuse has identical pixels and zero dispatches");
            auto changedDefinition = plan->copyDefinition();
            auto& changedKernel = std::get<ChannelSelectKernel>(
                std::get<CompiledImageEffect>(changedDefinition.operations[1]).kernel);
            changedKernel.sources[0] =
                changedKernel.sources[0] == Source::Luma ? Source::Green : Source::Luma;
            const auto changedPlan =
                std::make_shared<const CompiledCompositionPlan>(std::move(changedDefinition));
            const auto changed = builder.build(changedPlan, request);
            const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
            expectations.expect(changed && changedOracle.frame(),
                                "channel native: changed mapping prepares");
            if (!changed || !changedOracle.frame())
                continue;
            const auto rerun = runScene(*executor.executor, changed.scene);
            expectations.expect(
                rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                    parity(rerun.pixels, changedOracle.frame()->processImage().pixels(), detail) &&
                    executor.executor->counters().ocioEffectDispatches ==
                        coldCounters.ocioEffectDispatches + 1,
                "channel native: a changed mapping re-dispatches with CPU parity: " + detail);
        }
    }
}

// #338 Luma Key: ramp, inverted ramp, hard and inverted hard keys of signed/HDR/partial-alpha
// solids against the CPU oracle (2e-6, exact alpha), one resident dispatch, warm zero dispatch and
// a changed threshold that re-dispatches with parity.
void runLumaKeys(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::runtime::LumaKeyKernel;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    std::uint64_t id = 9000;
    struct Case final {
        double low;
        double high;
        bool invert;
    };
    for (const auto color : {Color4d{-0.25, 1.75, 0.5, 0.625}, Color4d{4.0, 3.0, 6.0, 1.0},
                             Color4d{0.3, 0.6, 0.2, 0.25}}) {
        for (const auto [low, high, invert] :
             {Case{0.0, 1.0, false}, Case{0.25, 2.0, true}, Case{0.5, 0.5, false},
              Case{0.5, 0.5, true}}) {
            cache.clear();
            const auto plan = effectPlan(LumaKeyKernel{{ParameterId::fromRaw(id + 90), low},
                                                       {ParameterId::fromRaw(id + 91), high},
                                                       invert},
                                         id, std::nullopt, color);
            id += 100;
            const auto request = requestFor(*plan);
            auto oracleRequest = request;
            oracleRequest.bypassOperationCache = true;
            const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
            const auto prepared = builder.build(plan, request);
            expectations.expect(oracle.frame() && prepared,
                                "luma key native: CPU oracle and production scene prepare");
            if (!oracle.frame() || !prepared)
                continue;
            auto executor = GpuSceneExecutor::create(device, cache);
            expectations.expect(executor.hasValue(), "luma key native: executor creates");
            if (!executor)
                continue;
            const auto cold = runScene(*executor.executor, prepared.scene);
            const auto coldCounters = executor.executor->counters();
            std::string detail;
            expectations.expect(
                cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                    parity(cold.pixels, oracle.frame()->processImage().pixels(), detail),
                "luma key native: HDR/partial-alpha CPU parity (2e-6, exact alpha): " + detail);
            const auto& descriptor = *oracle.frame()->processImage().descriptor();
            expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                    cold.displayWindow == descriptor.displayWindow() &&
                                    cold.pixelAspect == descriptor.pixelAspect(),
                                "luma key native: data/display-window and PAR parity");
            expectations.expect(coldCounters.ocioEffectDispatches == 1 &&
                                    coldCounters.readbacks == 0,
                                "luma key native: one resident dispatch and no frame readback");
            const auto warm = runScene(*executor.executor, prepared.scene);
            expectations.expect(
                warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                    parity(warm.pixels, oracle.frame()->processImage().pixels(), detail) &&
                    executor.executor->counters().ocioEffectDispatches ==
                        coldCounters.ocioEffectDispatches,
                "luma key native: warm RAM reuse has identical pixels and zero dispatches");
            auto changedDefinition = plan->copyDefinition();
            std::get<LumaKeyKernel>(
                std::get<CompiledImageEffect>(changedDefinition.operations[1]).kernel)
                .high.source = high + 0.375;
            const auto changedPlan =
                std::make_shared<const CompiledCompositionPlan>(std::move(changedDefinition));
            const auto changed = builder.build(changedPlan, request);
            const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
            expectations.expect(changed && changedOracle.frame(),
                                "luma key native: changed threshold prepares");
            if (!changed || !changedOracle.frame())
                continue;
            const auto rerun = runScene(*executor.executor, changed.scene);
            expectations.expect(
                rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                    parity(rerun.pixels, changedOracle.frame()->processImage().pixels(),
                           detail) &&
                    executor.executor->counters().ocioEffectDispatches ==
                        coldCounters.ocioEffectDispatches + 1,
                "luma key native: a changed threshold re-dispatches with CPU parity: " + detail);
        }
    }
}

// #339 Drop Shadow, Glow and Light Wrap (#506: and Glow's Physical and Streak modes and shared
// controls): the production recipe chains on the device against the CPU oracle at the
// 2e-6/exact-alpha gate (a Glow keyed from the gradient's colour: 2e-6 alpha, #545), with the
// data/display windows; a warm unchanged scene
// dispatches nothing; a changed opacity, intensity or amount re-runs only the chain after it (the
// softened coverage, the octave blurs, the background blur stay cached): one Levels dispatch.
void runRecipes(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::recipe_test::Recipe;
    using bloom::recipe_test::RecipeValues;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    const auto dispatches = [](const bloom::runtime::GpuSceneExecutorCounters& counters) {
        return counters.solidDispatches + counters.coveredSolidDispatches +
               counters.translationDispatches + counters.sourceOverDispatches +
               counters.affineDispatches + counters.blendDispatches + counters.ocioEffectDispatches;
    };
    struct Case final {
        Recipe recipe;
        RecipeValues values;
        RecipeValues changed;
        std::uint64_t changedOcioDispatches; // the re-run neighbourhood/Levels programs
        const char* name;
        AlphaGate alphaGate = AlphaGate::Exact;
        // Evaluate at half the composition's size (a reduced render scale).
        bool halfScale = false;
    };
    std::vector<Case> cases;
    {
        RecipeValues changed;
        changed.opacity = 0.4;
        cases.push_back({Recipe::DropShadow, {}, changed, 1, "drop shadow"});
        RecipeValues only;
        only.shadowOnly = true;
        only.direction = 135.0;
        only.distance = 2.5;
        RecipeValues onlyChanged = only;
        onlyChanged.opacity = 0.3;
        cases.push_back({Recipe::DropShadow, only, onlyChanged, 1, "drop shadow only"});
    }
    {
        // Luma Key's coverage is a function of colour: an exact-alpha device case keys a solid.
        RecipeValues glow;
        glow.solidInput = true;
        RecipeValues changed = glow;
        changed.intensity = 4.0;
        cases.push_back({Recipe::Glow, glow, changed, 1, "glow"});
        RecipeValues keyed;
        keyed.key = true;
        keyed.mask = true;
        keyed.threshold = 0.5;
        keyed.glowOnly = true;
        RecipeValues keyedChanged = keyed;
        keyedChanged.intensity = 0.5;
        cases.push_back({Recipe::Glow, keyed, keyedChanged, 1, "glow keyed"});
        // #545: the Luma Key of the generated gradient, whose colour the device matches within
        // 2e-6 rather than bit for bit: its keyed alpha is held to that same 2e-6.
        RecipeValues gradientChanged;
        gradientChanged.intensity = 4.0;
        cases.push_back({Recipe::Glow, {}, gradientChanged, 1, "glow keyed from gradient colour"});
        cases.back().alphaGate = AlphaGate::KeyedFromToleranceQualifiedColour;
    }
    {
        // #506 modes on the exact-alpha solid: a changed intensity re-runs only the gains (the
        // Physical removal's min(intensity, 1) stays 1), a changed roll-off only the roll-off.
        using bloom::runtime::detail::GlowMode;
        RecipeValues physical;
        physical.solidInput = true;
        physical.mode = GlowMode::Physical;
        RecipeValues physicalChanged = physical;
        physicalChanged.intensity = 4.0;
        cases.push_back({Recipe::Glow, physical, physicalChanged, 1, "glow physical"});
        RecipeValues keyed = physical;
        keyed.key = true;
        keyed.mask = true;
        keyed.threshold = 0.5;
        keyed.glowOnly = true;
        RecipeValues keyedChanged = keyed;
        keyedChanged.intensity = 0.5;
        cases.push_back({Recipe::Glow, keyed, keyedChanged, 1, "glow physical keyed"});
        RecipeValues star;
        star.solidInput = true;
        star.mode = GlowMode::Streak;
        RecipeValues starChanged = star;
        starChanged.intensity = 3.0;
        cases.push_back({Recipe::Glow, star, starChanged, 1, "glow streak star"});
        RecipeValues anamorphic = star;
        anamorphic.points = 2;
        anamorphic.angle = 0.0;
        anamorphic.chromatic = 0.6;
        RecipeValues anamorphicChanged = anamorphic;
        anamorphicChanged.intensity = 3.0;
        cases.push_back(
            {Recipe::Glow, anamorphic, anamorphicChanged, 1, "glow streak chromatic anamorphic"});
        RecipeValues shared;
        shared.solidInput = true;
        shared.channelRadius = {1.0, 0.5, 0.25};
        shared.gradient = true;
        shared.dirt = true;
        shared.rollOff = 0.5;
        RecipeValues sharedChanged = shared;
        sharedChanged.rollOff = 1.0;
        cases.push_back({Recipe::Glow, shared, sharedChanged, 1, "glow shared controls"});
    }
    {
        RecipeValues changed;
        changed.amount = 0.25;
        cases.push_back({Recipe::LightWrap, {}, changed, 1, "light wrap"});
        RecipeValues only;
        only.wrapOnly = true;
        RecipeValues onlyChanged = only;
        onlyChanged.amount = 0.5;
        cases.push_back({Recipe::LightWrap, only, onlyChanged, 1, "light wrap only"});
    }
    {
        // #516 Edge Extend: a changed amount re-runs only the mix, a blend dispatch (the OCIO
        // programs before it stay cached).
        RecipeValues changed;
        changed.extendAmount = 0.4;
        cases.push_back({Recipe::EdgeExtend, {}, changed, 0, "edge extend"});
        RecipeValues far;
        far.extendDistance = 2.5;
        far.extendAmount = 0.6;
        RecipeValues farChanged = far;
        farChanged.extendAmount = 0.3;
        cases.push_back({Recipe::EdgeExtend, far, farChanged, 0, "edge extend, fractional reach"});
        // #639: at a reduced render scale (half the size) the distance is half as many proxy
        // pixels; the device is held to the CPU oracle at the same gate.
        cases.push_back({Recipe::EdgeExtend, {}, changed, 0, "edge extend, half render scale",
                         AlphaGate::Exact, true});
        cases.push_back({Recipe::EdgeExtend, far, farChanged, 0,
                         "edge extend, fractional reach, half render scale", AlphaGate::Exact, true});
    }
    {
        // #528 Inner Shadow and Inner Glow: the holes of a coverage blur, shaped and painted, over
        // the HDR, partial-alpha gradient inside its own window. A changed opacity or intensity
        // re-runs the paint Levels, a changed choke the shaping Levels as well.
        RecipeValues shadow;
        shadow.centred = true;
        RecipeValues shadowChanged = shadow;
        shadowChanged.opacity = 0.4;
        cases.push_back({Recipe::InnerShadow, shadow, shadowChanged, 1, "inner shadow"});
        RecipeValues shadowOnly = shadow;
        shadowOnly.shadowOnly = true;
        shadowOnly.direction = 135.0;
        shadowOnly.distance = 2.5;
        shadowOnly.choke = 0.3;
        RecipeValues shadowOnlyChanged = shadowOnly;
        shadowOnlyChanged.choke = 0.8;
        cases.push_back(
            {Recipe::InnerShadow, shadowOnly, shadowOnlyChanged, 2, "inner shadow only, choked"});
        RecipeValues edge;
        edge.centred = true;
        edge.tint = {3.0, 1.5, 0.5, 1.0};
        RecipeValues edgeChanged = edge;
        edgeChanged.intensity = 0.5;
        cases.push_back({Recipe::InnerGlow, edge, edgeChanged, 1, "inner glow edge"});
        RecipeValues centre = edge;
        centre.centre = true;
        centre.glowOnly = true;
        centre.choke = 0.2;
        RecipeValues centreChanged = centre;
        centreChanged.choke = 0.9;
        cases.push_back({Recipe::InnerGlow, centre, centreChanged, 2, "inner glow centre only"});
    }
    {
        // #514 Denoise & Smooth Median over an HDR gradient whose coverage ramps (a soft alpha edge
        // everywhere): the straight-color rule against the CPU reference, at no threshold and at a
        // threshold that keeps some pixels. A changed strength re-runs the one neighborhood pass.
        RecipeValues plain;
        plain.denoiseStrength = 0.0;
        RecipeValues plainChanged = plain;
        plainChanged.denoiseStrength = 0.3;
        cases.push_back({Recipe::Denoise, plain, plainChanged, 1, "median at a soft alpha edge"});
        RecipeValues kept;
        kept.denoiseStrength = 0.6;
        RecipeValues keptChanged = kept;
        keptChanged.denoiseStrength = 0.05;
        cases.push_back(
            {Recipe::Denoise, kept, keptChanged, 1, "median with a threshold at a soft edge"});
    }
    {
        // #524 Gaussian Blur with a radius per channel: five blurs and four channel combines over
        // an HDR, partly transparent input, with and without separate axes. A changed share
        // re-runs the blurs it scales (with a zero vertical radius, one horizontal pass).
        RecipeValues channels;
        RecipeValues changed = channels;
        changed.channelScale = {1.0, 0.4, 0.0, 0.2};
        // The changed share re-runs that channel's blur (a horizontal and a vertical pass).
        cases.push_back({Recipe::ChannelBlur, channels, changed, 2, "per-channel blur"});
        RecipeValues separate;
        separate.blurSeparate = true;
        separate.blurRadiusY = 0.0;
        RecipeValues separateChanged = separate;
        separateChanged.channelScale = {0.5, 0.4, 0.0, 0.7};
        cases.push_back(
            {Recipe::ChannelBlur, separate, separateChanged, 1, "per-channel blur, separate axes"});
    }
    {
        // #509 Alpha Contour: a changed colour re-runs only the band's field raster (the distance
        // transform before it stays cached; the composite is a blend dispatch). The band's alpha
        // is a function of a square root of the distance, which the device rounds within an ulp
        // of the CPU (the Fields gate above), so alpha is held to the 2e-6 gate, not bit-exact.
        constexpr auto contourAlpha = AlphaGate::KeyedFromToleranceQualifiedColour;
        RecipeValues changed;
        changed.contourColor = Color4d{0.25, 1.5, 3.0, 0.6};
        cases.push_back({Recipe::AlphaContour, {}, changed, 1, "alpha contour over", contourAlpha});
        RecipeValues under;
        under.contourComposite = bloom::runtime::detail::AlphaContourComposite::Under;
        under.contourSoftness = 0.0;
        under.solidInput = true;
        RecipeValues underChanged = under;
        underChanged.contourColor = Color4d{0.25, 1.5, 3.0, 0.6};
        cases.push_back({Recipe::AlphaContour, under, underChanged, 1, "alpha contour under, hard",
                         contourAlpha});
        RecipeValues inward;
        inward.contourComposite = bloom::runtime::detail::AlphaContourComposite::ContourOnly;
        inward.contourOffset = -1.5;
        inward.contourWidth = 0.4;
        RecipeValues inwardChanged = inward;
        inwardChanged.contourColor = Color4d{6.0, 3.0, 0.5, 1.0};
        cases.push_back({Recipe::AlphaContour, inward, inwardChanged, 1,
                         "alpha contour alone, inward hairline", contourAlpha});
    }
    std::uint64_t id = 9600;
    for (const auto& item : cases) {
        cache.clear();
        const std::string name = std::string("recipe native (") + item.name + "): ";
        const auto plan = bloom::recipe_test::recipePlan(item.recipe, item.values, id).plan;
        const auto changedPlan = bloom::recipe_test::recipePlan(item.recipe, item.changed, id).plan;
        id += 100;
        auto request = requestFor(*plan);
        if (item.halfScale) {
            const auto half = bloom::render::ImageExtent::create(bloom::recipe_test::kRecipeWidth / 2,
                                                                 bloom::recipe_test::kRecipeHeight / 2);
            expectations.expect(half.hasValue(), name + "the half-scale extent is valid");
            if (!half)
                continue;
            request.resolution = bloom::runtime::ProxyResolution{*half.value()};
        }
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare");
        if (!oracle.frame() || !prepared)
            continue;
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), name + "executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        const std::string alphaGate =
            item.alphaGate == AlphaGate::Exact ? "exact alpha" : "keyed alpha within 2e-6";
        std::string detail;
        float maxAlphaError = 0.0F;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail,
                                       item.alphaGate, &maxAlphaError),
                            name + "HDR/partial-alpha CPU parity (2e-6, " + alphaGate +
                                "): " + detail);
        if (item.alphaGate != AlphaGate::Exact)
            std::cout << name << "max alpha difference " << maxAlphaError << '\n';
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            name + "data/display-window and PAR parity");
        expectations.expect(coldCounters.ocioEffectDispatches > 0 && coldCounters.readbacks == 0,
                            name + "resident dispatches and no per-operation readback");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(warm.pixels, oracle.frame()->processImage().pixels(), detail,
                                       item.alphaGate) &&
                                dispatches(executor.executor->counters()) ==
                                    dispatches(coldCounters),
                            name + "warm RAM reuse has identical pixels and zero dispatches");
        const auto changed = builder.build(changedPlan, request);
        const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
        expectations.expect(changed && changedOracle.frame(), name + "a changed value prepares");
        if (!changed || !changedOracle.frame())
            continue;
        const auto rerun = runScene(*executor.executor, changed.scene);
        expectations.expect(
            rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                parity(rerun.pixels, changedOracle.frame()->processImage().pixels(), detail,
                       item.alphaGate) &&
                executor.executor->counters().ocioEffectDispatches ==
                    coldCounters.ocioEffectDispatches + item.changedOcioDispatches,
            name + "a changed value re-runs only the chain after it, with CPU parity: " + detail +
                " (re-ran " +
                std::to_string(executor.executor->counters().ocioEffectDispatches -
                               coldCounters.ocioEffectDispatches) +
                " OCIO dispatches, expected " + std::to_string(item.changedOcioDispatches) + ")");
    }
}

// #528 Inner Shadow and Inner Glow: what the recipe-case parity above does not show. Half render
// scale against the CPU oracle at that scale, the output's data window equal to the input's (the CPU
// window of the input alone, and the GPU image's), a cancelled scene that publishes no image and
// retires every submission, a scene beyond the byte budget refused with nothing pinned, and the same
// executor running the scene afterwards with CPU parity and no unretired submission.
void runInnerLifecycle(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::recipe_test::Recipe;
    using bloom::recipe_test::RecipeValues;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    std::uint64_t id = 96000;
    for (const auto recipe : {Recipe::InnerShadow, Recipe::InnerGlow}) {
        const bool glow = recipe == Recipe::InnerGlow;
        RecipeValues values;
        values.centred = true;
        values.tint = {3.0, 1.5, 0.5, 1.0};
        values.choke = 0.25;
        const std::string name = std::string("inner native (") + (glow ? "glow" : "shadow") + "): ";
        const auto plan = bloom::recipe_test::recipePlan(recipe, values, id).plan;
        const auto inputPlan =
            bloom::recipe_test::recipePlan(recipe, values, id, bloom::recipe_test::Published::Input).plan;
        id += 100;
        for (const bool half : {false, true}) {
            cache.clear();
            const std::string where = name + (half ? "half scale: " : "full scale: ");
            auto request = requestFor(*plan);
            if (half) {
                const auto extent = bloom::render::ImageExtent::create(
                    bloom::recipe_test::kRecipeWidth / 2, bloom::recipe_test::kRecipeHeight / 2);
                request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
            }
            auto oracleRequest = request;
            oracleRequest.bypassOperationCache = true;
            const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
            auto inputRequest = oracleRequest;
            inputRequest.output = inputPlan->output();
            const auto input = evaluator.evaluate(inputPlan, inputRequest, {});
            const auto prepared = builder.build(plan, request);
            expectations.expect(oracle.frame() && input.frame() && prepared,
                                where + "CPU oracle, the input alone and the production scene prepare");
            if (!oracle.frame() || !input.frame() || !prepared)
                continue;
            auto executor = GpuSceneExecutor::create(device, cache);
            expectations.expect(executor.hasValue(), where + "executor creates");
            if (!executor)
                continue;
            const auto cold = runScene(*executor.executor, prepared.scene);
            std::string detail;
            expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                    parity(cold.pixels, oracle.frame()->processImage().pixels(), detail),
                                where + "CPU parity (2e-6, exact alpha): " + detail);
            const auto& inputWindow = input.frame()->processImage().descriptor()->dataWindow();
            expectations.expect(oracle.frame()->processImage().descriptor()->dataWindow() == inputWindow &&
                                    cold.dataWindow == inputWindow,
                                where + "the output's data window is the input's on the CPU and the device");
            expectations.expect(!executor.executor->hasUnretiredSubmission() &&
                                    !executor.executor->ownerDrainRequired(),
                                where + "every submission retired after the frame");
            const auto before = executor.executor->counters();
            const auto warm = runScene(*executor.executor, prepared.scene);
            expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                    warm.pixels == cold.pixels &&
                                    executor.executor->counters().ocioEffectDispatches ==
                                        before.ocioEffectDispatches,
                                where + "warm RAM reuse has identical pixels and zero dispatches");
            if (half)
                continue;
            // A scene beyond the byte budget is refused before any dispatch, nothing stays pinned,
            // and the executor runs the scene afterwards.
            cache.clear();
            auto budgeted = GpuSceneExecutor::create(device, cache);
            if (!budgeted)
                continue;
            const auto refused = budgeted.executor->begin(prepared.scene, 256);
            expectations.expect(refused.code == GpuSceneExecutorDiagnosticCode::OverBudget &&
                                    budgeted.executor->image() == nullptr &&
                                    budgeted.executor->counters().budgetRefusals == 1 &&
                                    budgeted.executor->counters().currentLiveImageBytes == 0,
                                where + "a scene beyond the byte budget is refused and pins nothing");
            const auto after = runScene(*budgeted.executor, prepared.scene);
            expectations.expect(after.pollResult == GpuSceneExecutorPollResult::Ready &&
                                    parity(after.pixels, oracle.frame()->processImage().pixels(), detail),
                                where + "the executor runs the scene after a budget refusal");
            // A cancelled scene publishes no image, retires every submission and pins nothing.
            cache.clear();
            auto cancelled = GpuSceneExecutor::create(device, cache);
            if (!cancelled)
                continue;
            expectations.expect(cancelled.executor->begin(prepared.scene, kSceneBudget).code ==
                                    GpuSceneExecutorDiagnosticCode::None,
                                where + "the scene to cancel begins");
            static_cast<void>(cancelled.executor->poll());
            cancelled.executor->cancel();
            GpuSceneExecutorPollResult terminal = GpuSceneExecutorPollResult::Pending;
            for (std::uint64_t i = 0; i < 1000000 && terminal == GpuSceneExecutorPollResult::Pending; ++i)
                terminal = cancelled.executor->poll();
            expectations.expect(terminal == GpuSceneExecutorPollResult::Failure &&
                                    cancelled.executor->diagnostic().code ==
                                        GpuSceneExecutorDiagnosticCode::Cancelled &&
                                    cancelled.executor->image() == nullptr &&
                                    cancelled.executor->takeImage() == nullptr,
                                where + "a cancelled scene ends Cancelled and publishes no image");
            for (std::uint64_t i = 0;
                 i < 1000000 && (cancelled.executor->hasUnretiredSubmission() ||
                                 cancelled.executor->ownerDrainRequired());
                 ++i)
                static_cast<void>(cancelled.executor->poll());
            expectations.expect(!cancelled.executor->hasUnretiredSubmission() &&
                                    !cancelled.executor->ownerDrainRequired() &&
                                    cancelled.executor->counters().scenesCancelled == 1 &&
                                    cancelled.executor->counters().currentLiveImageBytes == 0,
                                where + "a cancelled scene retires every submission and pins nothing");
            const auto reused = runScene(*cancelled.executor, prepared.scene);
            expectations.expect(reused.pollResult == GpuSceneExecutorPollResult::Ready &&
                                    parity(reused.pixels, oracle.frame()->processImage().pixels(), detail),
                                where + "the executor runs the scene after a cancellation");
        }
    }
}

// #525 Fields: every field operation (an Image field through the atlas of its slot), sub-samples
// and a mask's exact distance against the CPU oracle. The kernel is Bloom's own binary32 arithmetic
// on both sides, so the device may differ only where its sqrt rounds differently and a value then
// lands on the other side of a cell or step boundary: at most 2% of the pixels may differ, each by
// at most 1e-4 relative otherwise. One resident dispatch per pass and no readback, warm zero
// dispatches with identical pixels, and a changed operand re-dispatches with parity.
[[nodiscard]] bool fieldParity(const std::vector<Rgba32f>& actual,
                               const std::span<const Rgba32f> expected, std::string& detail) {
    if (actual.size() != expected.size() || actual.empty()) {
        detail = "size";
        return false;
    }
    std::size_t differing = 0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        bool same = true;
        for (std::size_t c = 0; c < 4; ++c) {
            const float a = actual[i].components()[c];
            const float e = expected[i].components()[c];
            if (!std::isfinite(a) || !std::isfinite(e)) {
                detail = "non-finite at " + std::to_string(i);
                return false;
            }
            same = same && std::abs(a - e) <= 1e-4F * std::max(1.0F, std::abs(e));
        }
        differing += same ? 0U : 1U;
    }
    detail = std::to_string(differing) + " of " + std::to_string(actual.size()) + " pixels differ";
    return differing * 50U <= actual.size();
}

void runFields(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::core::field::FieldOp;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    std::vector<std::pair<std::string, bloom::runtime::ImageEffectKernel>> kernels;
    std::uint64_t id = 52500;
    for (const auto op : bloom::core::field::kFieldOps)
        for (int variant = 0; variant < 2; ++variant) {
            kernels.emplace_back(std::string(bloom::core::field::fieldOpName(op)),
                                 bloom::runtime::FieldRasterKernel{
                                     bloom::field_test_programs::operationProgram(op, id, variant),
                                     variant == 1 ? 4U : 1U});
            id += 100;
        }
    // #647 Field Noise's vector output (both streams in one instruction), two noises, one at the
    // coverage sampling.
    for (const int variant : {101, 103}) {
        kernels.emplace_back("noise vector output " + std::to_string(variant),
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::operationProgram(
                                     bloom::core::field::FieldOp::Noise, id, variant),
                                 variant == 103 ? 4U : 1U});
        id += 100;
    }
    kernels.emplace_back("mask distance", bloom::runtime::MaskDistanceKernel{3.0});
    // #486 Lens Distortion: the programs its lowering builds, the warped image (premultiplied HDR
    // and partial alpha read through the atlas) and the ST map, each in both modes.
    for (const bool stMap : {false, true})
        for (const bool reverse : {false, true}) {
            kernels.emplace_back(std::string("lens distortion ") + (stMap ? "ST map " : "image ") +
                                     (reverse ? "redistort" : "undistort"),
                                 bloom::runtime::FieldRasterKernel{
                                     bloom::field_test_programs::lensDistortionProgram(id, stMap,
                                                                                       reverse),
                                     1U});
            id += 100;
        }
    // #486 Chromatic Aberration: the program its lowering builds (three premultiplied reads of an
    // HDR, partly transparent image through the atlas).
    kernels.emplace_back("chromatic aberration",
                         bloom::runtime::FieldRasterKernel{
                             bloom::field_test_programs::chromaticAberrationProgram(id), 1U});
    id += 100;
    // #568 Rolling Shutter: the program its lowering builds (a camera term and a flow image moving
    // the position a premultiplied HDR, partly transparent picture is read at).
    kernels.emplace_back("rolling shutter",
                         bloom::runtime::FieldRasterKernel{
                             bloom::field_test_programs::rollingShutterProgram(id), 1U});
    id += 100;
    // #568: the other three read directions with Extend, and every direction with Transparent edges
    // (the cases above are Top to Bottom, Extend): the programs the lowering builds for each.
    for (const std::int64_t direction : {0, 1, 2, 3})
        for (const std::int64_t edges : {0, 1}) {
            if (direction == 0 && edges == 0)
                continue;
            kernels.emplace_back(
                "rolling shutter direction " + std::to_string(direction) +
                    (edges == 0 ? " extend" : " transparent"),
                bloom::runtime::FieldRasterKernel{
                    bloom::field_test_programs::rollingShutterProgram(id, direction, edges), 1U});
            id += 100;
        }
    // #587 Test Card: every card, the programs its lowering builds, CPU and device compared.
    for (std::int64_t card = 0; card < 16; ++card) {
        kernels.emplace_back("test card " + std::to_string(card),
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::testCardProgram(
                                     id, card, card == 3 ? 2 : 14, card == 9 ? 15 : 2, card % 2 == 0),
                                 4U});
        id += 100;
    }
    // #524 Four-Color Gradient: the program its lowering builds, at a soft, the default and a sharp
    // blend (HDR colors, one partly transparent).
    for (const double blend : {0.5, 2.0, 8.0}) {
        kernels.emplace_back("four-color gradient blend " + std::to_string(blend),
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::fourColorGradientProgram(
                                     id, blend,
                                     {bloom::document::Vec2d{-5.5, 3.5}, bloom::document::Vec2d{5.5, 3.5},
                                      bloom::document::Vec2d{-5.5, -3.5}, bloom::document::Vec2d{2.0, -1.0}},
                                     {{{1.5, 0.0, 0.0, 1.0}, {0.0, 1.0, 0.25, 1.0},
                                       {0.0, 0.25, 1.0, 0.5}, {1.0, 1.0, 0.0, 1.0}}}),
                                 1U});
        id += 100;
    }
    // #524 Four-Color Gradient with every point far from the frame at the sharpest blend: the weights
    // are normalized by the nearest point's, so the device, which flushes binary32 denormals, keeps
    // the CPU's picture instead of 0 / 0.
    kernels.emplace_back("four-color gradient far points",
                         bloom::runtime::FieldRasterKernel{
                             bloom::field_test_programs::fourColorGradientProgram(
                                 id, 8.0,
                                 {bloom::document::Vec2d{-1.0e6, 1.0e6}, bloom::document::Vec2d{1.0e6, 1.0e6},
                                  bloom::document::Vec2d{-1.0e6, -1.0e6}, bloom::document::Vec2d{9.0e5, -1.0e6}},
                                 {{{1.5, 0.0, 0.0, 1.0}, {0.0, 1.0, 0.25, 1.0},
                                   {0.0, 0.25, 1.0, 0.5}, {1.0, 1.0, 0.0, 1.0}}}),
                             1U});
    id += 100;
    // #348 Corner Pin, ST Map and Displace: the programs their lowerings build, each filter.
    for (const std::int64_t filter : {0, 1, 2}) {
        kernels.emplace_back("corner pin filter " + std::to_string(filter),
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::cornerPinProgram(id, filter), 1U});
        id += 100;
        kernels.emplace_back("stabilize filter " + std::to_string(filter),
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::stabilizeProgram(id, filter, filter % 2,
                                                                              filter + 1),
                                 1U});
        id += 100;
        // #511 Tile & Mirror: each mode through each filter (Tile with Mirror Edges and Clamp to Output
        // on alternate filters, Mirror either side, Kaleidoscope at three segment counts).
        kernels.emplace_back("tile filter " + std::to_string(filter),
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::tileMirrorProgram(
                                     id, 0, filter, filter + 1, filter == 1 ? 0 : 3),
                                 1U});
        id += 100;
        kernels.emplace_back("wrap offset filter " + std::to_string(filter),
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::tileMirrorProgram(id, 1, filter), 1U});
        id += 100;
        kernels.emplace_back("mirror filter " + std::to_string(filter),
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::tileMirrorProgram(
                                     id, 2, filter, filter + 1, filter % 2),
                                 1U});
        id += 100;
        kernels.emplace_back("kaleidoscope filter " + std::to_string(filter),
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::tileMirrorProgram(
                                     id, 3, filter, filter + 1, 3 + 2 * filter),
                                 1U});
        id += 100;
        // The window of a shape or text layer is its own small box: the same programs read a 5 x 4
        // source of the 12 x 10 frame, the window laid with its centre at the frame's.
        kernels.emplace_back("small input tile filter " + std::to_string(filter),
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::tileMirrorProgram(
                                     id, 0, filter, filter + 1, filter == 1 ? 0 : 3),
                                 1U});
        id += 100;
        kernels.emplace_back("small input kaleidoscope filter " + std::to_string(filter),
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::tileMirrorProgram(
                                     id, 3, filter, filter + 1, 3 + 2 * filter),
                                 1U});
        id += 100;
        // #510 Procedural Warp: every mode through each filter on the 12 x 10 frame, the edge rule
        // and the mode's variant (wave shape and pinning, polar direction, magnify shape and link)
        // turning with the filter.
        for (const std::int64_t mode : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}) {
            kernels.emplace_back("procedural warp mode " + std::to_string(mode) + " filter " +
                                     std::to_string(filter),
                                 bloom::runtime::FieldRasterKernel{
                                     bloom::field_test_programs::proceduralWarpProgram(
                                         id, mode, filter, (filter + mode) % 4, filter + mode % 2,
                                         12, 10),
                                     1U});
            id += 100;
        }
        // Asymmetric Split through each filter: the two halves open by their own distance (the odd
        // variant also has the mirror on, which ignores the second distance).
        kernels.emplace_back("procedural warp asymmetric split filter " + std::to_string(filter),
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::proceduralWarpProgram(
                                     id, 9, filter, (filter + 1) % 4, 4 + filter % 2, 12, 10),
                                 1U});
        id += 100;
        // Every Warp style (the variant picks it) through each filter: four per filter, 0 to 15
        // wrapping onto the 13 styles.
        for (std::int64_t j = 0; j < 4; ++j) {
            kernels.emplace_back("procedural warp style " + std::to_string((filter * 4 + j) % 13) +
                                     " filter " + std::to_string(filter),
                                 bloom::runtime::FieldRasterKernel{
                                     bloom::field_test_programs::proceduralWarpProgram(
                                         id, 7, filter, (filter + j) % 4, filter * 4 + j, 12, 10),
                                     1U});
            id += 100;
        }
        kernels.emplace_back("st map filter " + std::to_string(filter),
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::stMapProgram(id, filter, filter + 1), 1U});
        id += 100;
        kernels.emplace_back("displace filter " + std::to_string(filter),
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::displaceProgram(id, filter, filter + 1), 1U});
        id += 100;
    }
    // #508 Light Rays: the program its lowering builds, rays gathered from luma and from alpha.
    for (const bool alpha : {false, true}) {
        kernels.emplace_back(alpha ? "light rays alpha" : "light rays luminance",
                             bloom::runtime::FieldRasterKernel{
                                 bloom::field_test_programs::lightRaysProgram(id, alpha), 1U});
        id += 100;
    }
    for (const auto& [name, kernel] : kernels) {
        cache.clear();
        const bool smallSource = name.starts_with("small input ");
        auto definition = effectPlan(kernel, id, std::nullopt, Color4d{-0.25, 1.75, 0.5, 0.625},
                                     smallSource ? 5.0 : 12.0, smallSource ? 4.0 : 10.0)
                              ->copyDefinition();
        id += 200;
        auto& effect = std::get<CompiledImageEffect>(definition.operations[1]);
        const auto* field = std::get_if<bloom::runtime::FieldRasterKernel>(&effect.kernel);
        if (field != nullptr) {
            // One image per slot the program reads (the same picture in each).
            int slots = 0;
            for (const auto& instruction : field->program.instructions)
                slots = std::max(slots, instruction.slot + 1);
            effect.fieldImages.assign(static_cast<std::size_t>(slots), OperationIndex::fromRaw(0));
        }
        const auto plan = std::make_shared<const CompiledCompositionPlan>(definition);
        const auto request = requestFor(*plan);
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            "field native " + name + ": CPU oracle and production scene prepare");
        if (!oracle.frame() || !prepared)
            continue;
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), "field native: executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        std::string detail;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                fieldParity(cold.pixels, oracle.frame()->processImage().pixels(),
                                            detail),
                            "field native " + name + ": CPU parity: " + detail);
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            "field native " + name + ": data/display-window and PAR parity");
        const auto passes = field != nullptr ? 1U : 2U;
        expectations.expect(coldCounters.ocioEffectDispatches == passes &&
                                coldCounters.readbacks == 0,
                            "field native " + name + ": resident dispatches and no readback");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                warm.pixels == cold.pixels &&
                                executor.executor->counters().ocioEffectDispatches ==
                                    coldCounters.ocioEffectDispatches,
                            "field native " + name +
                                ": warm RAM reuse has identical pixels and zero dispatches");
        if (field == nullptr)
            continue;
        auto changedDefinition = plan->copyDefinition();
        auto& changedProgram = std::get<bloom::runtime::FieldRasterKernel>(
                                   std::get<CompiledImageEffect>(changedDefinition.operations[1])
                                       .kernel)
                                   .program;
        bool changed = false;
        for (auto& instruction : changedProgram.instructions)
            for (auto& scalar : instruction.scalars)
                if (!changed)
                    if (auto* value = std::get_if<double>(&scalar.source)) {
                        *value += 0.5;
                        changed = true;
                    }
        if (!changed)
            continue;
        const auto changedPlan =
            std::make_shared<const CompiledCompositionPlan>(std::move(changedDefinition));
        const auto rebuilt = builder.build(changedPlan, request);
        const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
        expectations.expect(rebuilt && changedOracle.frame(),
                            "field native " + name + ": a changed operand prepares");
        if (!rebuilt || !changedOracle.frame())
            continue;
        const auto rerun = runScene(*executor.executor, rebuilt.scene);
        expectations.expect(
            rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                fieldParity(rerun.pixels, changedOracle.frame()->processImage().pixels(), detail) &&
                executor.executor->counters().ocioEffectDispatches ==
                    coldCounters.ocioEffectDispatches + 1,
            "field native " + name + ": a changed operand re-dispatches with parity: " + detail);
    }
    std::cout << "field native: " << kernels.size()
              << " programs checked for CPU parity, residency, warm reuse and a changed operand\n";
}

// #386: a whole effect chain (blurs and other OCIO effect programs, image combines and blends,
// translations and source-overs) records into shared native batches. The batched run publishes
// exactly the serial run's pixels with fewer submissions, keeps CPU parity, and each OCIO program a
// batch reads stays busy until that batch's fence is proven.
void runBatchedChains(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::recipe_test::Recipe;
    using bloom::recipe_test::RecipeValues;
    using bloom::runtime::detail::GlowMode;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    RecipeValues physical;
    physical.solidInput = true;
    physical.mode = GlowMode::Physical;
    RecipeValues streak;
    streak.solidInput = true;
    streak.mode = GlowMode::Streak;
    const std::array<std::pair<Recipe, RecipeValues>, 4> chains{
        std::pair{Recipe::Glow, physical}, std::pair{Recipe::Glow, streak},
        std::pair{Recipe::LightWrap, RecipeValues{}}, std::pair{Recipe::DropShadow, RecipeValues{}}};
    std::uint64_t id = 19600;
    for (const auto& [recipe, values] : chains) {
        const auto plan = bloom::recipe_test::recipePlan(recipe, values, id).plan;
        id += 100;
        const auto request = requestFor(*plan);
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared, "batched chain: scene and oracle prepare");
        if (!oracle.frame() || !prepared)
            continue;
        // Serial, batched, and batched with an output bound no two steps fit under (a gradient
        // still takes its required single-kernel batch in every mode).
        std::array<RunOutcome, 3> runs;
        std::array<bloom::runtime::GpuSceneExecutorCounters, 3> counters;
        for (std::size_t mode = 0; mode < 3; ++mode) {
            cache.clear();
            GpuSceneExecutorBudgets budgets;
            budgets.enableNativeBatches = mode != 0;
            if (mode == 2)
                budgets.maxNativeBatchOutputBytes = 1;
            auto executor = GpuSceneExecutor::create(device, cache, budgets);
            expectations.expect(executor.hasValue(), "batched chain: executor creates");
            if (!executor)
                return;
            runs[mode] = runScene(*executor.executor, prepared.scene);
            counters[mode] = executor.executor->counters();
        }
        const auto& serial = counters[0];
        const auto& batched = counters[1];
        std::string detail;
        expectations.expect(runs[0].pollResult == GpuSceneExecutorPollResult::Ready &&
                                runs[1].pollResult == GpuSceneExecutorPollResult::Ready &&
                                runs[0].pixels == runs[1].pixels,
                            "batched chain: batched and serial GPU pixels are byte-exact");
        expectations.expect(parity(runs[1].pixels, oracle.frame()->processImage().pixels(), detail),
                            "batched chain: batched pixels keep CPU parity: " + detail);
        expectations.expect(
            serial.nativeSubmissions == serial.dispatches && batched.nativeBatches > 0 &&
                batched.nativeSubmissions < serial.nativeSubmissions &&
                batched.dispatches == serial.dispatches &&
                batched.ocioEffectDispatches == serial.ocioEffectDispatches &&
                batched.blendDispatches == serial.blendDispatches && batched.readbacks == 0,
            "batched chain: the same kernels run in fewer submissions (" +
                std::to_string(serial.nativeSubmissions) + " -> " +
                std::to_string(batched.nativeSubmissions) + ")");
        expectations.expect(runs[2].pollResult == GpuSceneExecutorPollResult::Ready &&
                                runs[2].pixels == runs[0].pixels &&
                                counters[2].nativeBatches == serial.nativeBatches &&
                                counters[2].nativeSubmissions == serial.nativeSubmissions,
                            "batched chain: a batch never records more output bytes than its "
                            "bound, so a bound below two outputs runs every step on its own (" +
                                std::to_string(counters[2].nativeBatches) + " batches, " +
                                std::to_string(counters[2].nativeSubmissions) + " submissions)");
    }

    // A program a pending batch reads is busy: its owner can neither begin it nor see it retired.
    cache.clear();
    const auto plan = bloom::recipe_test::recipePlan(Recipe::Glow, physical, id).plan;
    const auto prepared = builder.build(plan, requestFor(*plan));
    auto executor = GpuSceneExecutor::create(device, cache);
    if (!prepared || !executor) {
        expectations.expect(false, "batched chain: ownership scene prepares");
        return;
    }
    expectations.expect(executor.executor->begin(prepared.scene, kSceneBudget).code ==
                            GpuSceneExecutorDiagnosticCode::None,
                        "batched chain: ownership scene begins");
    bool sawBatch = false;
    for (;;) {
        const auto before = executor.executor->counters().nativeBatches;
        const auto poll = executor.executor->poll();
        if (executor.executor->counters().nativeBatches > before)
            sawBatch = true;
        if (poll != GpuSceneExecutorPollResult::Pending)
            break;
        // The event-driven wait returns once the in-flight job's fence signals.
        static_cast<void>(executor.executor->waitForNative(std::chrono::milliseconds(100)));
    }
    expectations.expect(sawBatch && executor.executor->state() == GpuSceneExecutorJobState::Ready,
                        "batched chain: the event-driven fence wait drives the scene to Ready");
    static_cast<void>(executor.executor->takeImage());
    expectations.expect(!executor.executor->waitForNative(std::chrono::milliseconds(1)),
                        "batched chain: an idle executor has no fence to wait on");
}

// #483 Depth effects: Z Defocus and Depth Fog through the production chains against the CPU
// oracle at the strict gate: colour within 2e-6, alpha exact. The depth is exact solid bands, so
// the circle of confusion, the gather's weights and its coverage are the CPU's bit for bit; a fog
// layer keeps its input's alpha, so the Mix's alpha is exact whatever its factor's exp or sqrt
// rounds to (a fog that fills empty depth is shown with a linear falloff). The windows, resident
// dispatches with no readback, warm reuse with zero dispatches and a changed value re-running only
// the chain after it (the beauty and, for a lens change, the colour prefilter stay cached) hold.
void runDepthEffects(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::depth_test::DepthValues;
    using bloom::depth_test::Effect;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    struct Case final {
        DepthValues values;
        DepthValues changed;
        const char* name;
    };
    std::vector<Case> cases;
    {
        DepthValues lens;
        DepthValues lensChanged = lens;
        lensChanged.focusDistance = 16.0;
        cases.push_back({lens, lensChanged, "defocus depth disk"});
        DepthValues uniform;
        uniform.mode = bloom::document::kDefocusModeUniform;
        uniform.shape = bloom::document::kBokehPolygon;
        DepthValues uniformChanged = uniform;
        uniformChanged.highlightGain = 1.5;
        cases.push_back({uniform, uniformChanged, "defocus uniform polygon"});
        DepthValues bokeh;
        bokeh.shape = bloom::document::kBokehImage;
        bokeh.bokeh = true;
        bokeh.highlightGain = 2.0;
        bokeh.chromatic = 0.5;
        bokeh.solidInput = true;
        DepthValues bokehChanged = bokeh;
        bokehChanged.focusDistance = 4.0;
        cases.push_back({bokeh, bokehChanged, "defocus image bokeh, boost and chromatic"});
        DepthValues physical;
        physical.mode = bloom::document::kDefocusModePhysical;
        physical.quality = bloom::document::kDefocusQualityHigh;
        physical.focusFrom = bloom::document::kFocusFromPoint;
        DepthValues physicalChanged = physical;
        physicalChanged.edgeExpand = 2;
        cases.push_back({physical, physicalChanged, "defocus physical lens from a point"});
        DepthValues mist;
        mist.mist = true;
        mist.nearDepth = 1.0;
        mist.farDepth = 40.0;
        DepthValues mistChanged = mist;
        mistChanged.focusDistance = 20.0;
        cases.push_back({mist, mistChanged, "defocus from mist"});
    }
    for (const auto falloff :
         {bloom::document::kFogFalloffLinear, bloom::document::kFogFalloffQuadratic,
          bloom::document::kFogFalloffInverseQuadratic, bloom::document::kFogFalloffExponential,
          bloom::document::kFogFalloffExponentialSquared, bloom::document::kFogFalloffCurve}) {
        DepthValues fog;
        fog.effect = Effect::Fog;
        fog.falloff = falloff;
        fog.mist = falloff == bloom::document::kFogFalloffCurve;
        fog.fillEmpty = falloff == bloom::document::kFogFalloffLinear ||
                        falloff == bloom::document::kFogFalloffQuadratic;
        fog.fogImage = falloff == bloom::document::kFogFalloffQuadratic ||
                       falloff == bloom::document::kFogFalloffExponential;
        DepthValues changed = fog;
        changed.nearDepth = fog.nearDepth + 1.0;
        cases.push_back({fog, changed, "fog"});
    }
    std::uint64_t id = 48300;
    for (const auto& item : cases) {
        cache.clear();
        const std::string name = std::string("depth native (") + item.name + " " +
                                 std::to_string(item.values.falloff) + "): ";
        const auto plan = bloom::depth_test::depthPlan(item.values, id).plan;
        const auto changedPlan = bloom::depth_test::depthPlan(item.changed, id).plan;
        id += 100;
        const auto request = requestFor(*plan);
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare");
        if (!oracle.frame() || !prepared)
            continue;
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), name + "executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        std::string detail;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact),
                            name + "CPU parity: " + detail);
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            name + "data/display-window and PAR parity");
        expectations.expect(coldCounters.ocioEffectDispatches > 0 && coldCounters.readbacks == 0,
                            name + "resident dispatches and no per-operation readback");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                warm.pixels == cold.pixels &&
                                executor.executor->counters().dispatches ==
                                    coldCounters.dispatches,
                            name + "warm RAM reuse has identical pixels and zero dispatches");
        const auto changed = builder.build(changedPlan, request);
        const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
        expectations.expect(changed && changedOracle.frame(), name + "a changed value prepares");
        if (!changed || !changedOracle.frame())
            continue;
        const auto rerun = runScene(*executor.executor, changed.scene);
        const auto rerunDispatches =
            executor.executor->counters().dispatches - coldCounters.dispatches;
        expectations.expect(
            rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                parity(rerun.pixels, changedOracle.frame()->processImage().pixels(), detail,
                       AlphaGate::Exact) &&
                rerunDispatches > 0 && rerunDispatches < coldCounters.dispatches,
            name + "a changed value re-runs only the chain after it, with CPU parity: " + detail +
                ", " + std::to_string(rerunDispatches) + " of " +
                std::to_string(coldCounters.dispatches) + " dispatches");
    }
}

// #507 Lens Flare against the CPU oracle: every element, both probes through the atlas, a matte's
// edge through the light, the probes' constant cases, a small input padded to the frame and a
// projected light; warm RAM reuse with zero dispatches, and a changed value re-running with parity.
// The stack's arithmetic is exact, so its pixels and alpha agree exactly.
void runLensFlares(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::lens_flare_test::everyElement;
    using bloom::lens_flare_test::FlareValues;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    struct Case final {
        FlareValues values;
        FlareValues changed;
        const char* name;
    };
    std::vector<Case> cases;
    {
        FlareValues base;
        FlareValues baseChanged = base;
        baseChanged.position = {10.0, 6.0};
        cases.push_back({base, baseChanged, "default stack"});
        FlareValues every = everyElement();
        FlareValues everyChanged = every;
        everyChanged.scalars["chromatic"] = 0.1;
        cases.push_back({every, everyChanged, "every element"});
        FlareValues source;
        source.scalars = {{"sourceBrightness", 0.75}, {"sourceWhite", 2.0}};
        FlareValues sourceChanged = source;
        sourceChanged.position = {4.0, 2.0};
        cases.push_back({source, sourceChanged, "brightness from source"});
        FlareValues matte;
        matte.matteWidth = 20;
        matte.scalars = {{"sampleRadius", 6.0}, {"occlusionAmount", 0.8}};
        FlareValues matteChanged = matte;
        matteChanged.matteWidth = 16;
        cases.push_back({matte, matteChanged, "a matte's edge through the light"});
        FlareValues both = everyElement();
        both.scalars["sourceBrightness"] = 0.5;
        both.matteWidth = 22;
        both.integers["occlusionChannel"] = 9;
        FlareValues bothChanged = both;
        bothChanged.scalars["occlusionAmount"] = 0.25;
        cases.push_back({both, bothChanged, "both probes, inverted luma"});
        FlareValues outside;
        outside.position = {4000.0, -3000.0};
        outside.matteWidth = 20;
        outside.scalars = {{"sourceBrightness", 0.5}};
        FlareValues outsideChanged = outside;
        outsideChanged.scalars["intensity"] = 0.5;
        cases.push_back({outside, outsideChanged, "a light outside every image"});
        FlareValues small = everyElement();
        small.smallInput = true;
        FlareValues smallChanged = small;
        smallChanged.scalars["flareSize"] = 0.5;
        cases.push_back({small, smallChanged, "a small input padded to the frame"});
        FlareValues projected;
        projected.integers = {{"lightMode", bloom::document::kLensFlareLight3D}};
        projected.scalars = {{"lightDistance", 300.0}, {"cameraZoom", 700.0}};
        FlareValues projectedChanged = projected;
        projectedChanged.scalars["lightDistance"] = 100.0;
        cases.push_back({projected, projectedChanged, "a projected 3D light"});
    }
    std::uint64_t id = 50700;
    for (const auto& item : cases) {
        cache.clear();
        const std::string name = std::string("lens flare native (") + item.name + "): ";
        const auto plan = bloom::lens_flare_test::flarePlan(item.values, id);
        const auto changedPlan = bloom::lens_flare_test::flarePlan(item.changed, id);
        id += 200;
        const auto request = requestFor(*plan);
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare");
        if (!oracle.frame() || !prepared)
            continue;
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), name + "executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        std::string detail;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact),
                            name + "CPU parity: " + detail);
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            name + "data/display-window and PAR parity");
        expectations.expect(coldCounters.ocioEffectDispatches > 0 && coldCounters.readbacks == 0,
                            name + "resident dispatches and no per-operation readback");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                warm.pixels == cold.pixels &&
                                executor.executor->counters().dispatches ==
                                    coldCounters.dispatches,
                            name + "warm RAM reuse has identical pixels and zero dispatches");
        const auto changed = builder.build(changedPlan, request);
        const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
        expectations.expect(changed && changedOracle.frame(), name + "a changed value prepares");
        if (!changed || !changedOracle.frame())
            continue;
        const auto rerun = runScene(*executor.executor, changed.scene);
        const auto rerunDispatches =
            executor.executor->counters().dispatches - coldCounters.dispatches;
        expectations.expect(rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(rerun.pixels, changedOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact) &&
                                rerunDispatches > 0,
                            name + "a changed value re-runs with CPU parity: " + detail);
    }
}

// #486 Film Grain against the CPU oracle: the default, every path of the program at a larger and a
// smaller size, held grain, a full-frame proxy and a region of interest; warm RAM reuse with zero
// dispatches; the next frame new grain with parity, drawn by the same program; a changed value
// re-running with parity; Monochrome on every one of those routes; and a toggle of Monochrome
// re-running with parity while each state keeps its own cache entry.
void runFilmGrains(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::film_grain_test::everyPath;
    using bloom::film_grain_test::GrainValues;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    struct Case final {
        GrainValues values;
        const char* name;
        bool proxy = false;
        bool roi = false;
        // Every frame is the same picture: nothing is drawn anew and the next frame reuses it.
        bool frameInvariant = false;
    };
    std::vector<Case> cases;
    cases.push_back({GrainValues{}, "default"});
    cases.push_back({everyPath(), "every path"});
    {
        GrainValues fine = everyPath();
        fine.scalars["size"] = 0.25;
        cases.push_back({fine, "fine"});
        GrainValues coarse = everyPath();
        coarse.scalars["size"] = 16.0;
        cases.push_back({coarse, "coarse"});
        GrainValues held = everyPath();
        held.animation = bloom::document::kFilmGrainHeld;
        cases.push_back({held, "held"});
    }
    cases.push_back({everyPath(), "a proxy", true});
    cases.push_back({everyPath(), "a region of interest", false, true});
    // Monochrome, on every route above.
    constexpr std::array kMonochromeNames{
        "monochrome, default", "monochrome, every path", "monochrome, fine",
        "monochrome, coarse",  "monochrome, held",       "monochrome, a proxy",
        "monochrome, a region of interest"};
    for (std::size_t index = 0; index < kMonochromeNames.size(); ++index) {
        Case monochrome = cases[index];
        monochrome.values.monochrome = true;
        monochrome.name = kMonochromeNames[index];
        cases.push_back(monochrome);
    }
    // #565 Sensor Noise, on the same routes: every term, Monochrome, held, a proxy, a region of
    // interest, and a fixed pattern alone (the same picture on every frame).
    using bloom::film_grain_test::everyPathSensor;
    cases.push_back({everyPathSensor(), "sensor noise, every path"});
    {
        GrainValues mono = everyPathSensor();
        mono.monochrome = true;
        cases.push_back({mono, "sensor noise, monochrome"});
        GrainValues held = everyPathSensor();
        held.animation = bloom::document::kFilmGrainHeld;
        cases.push_back({held, "sensor noise, held"});
        GrainValues fine = everyPathSensor();
        fine.scalars["size"] = 0.25;
        fine.scalars["blotchSize"] = 4.0;
        cases.push_back({fine, "sensor noise, fine"});
        GrainValues coarse = everyPathSensor();
        coarse.scalars["size"] = 16.0;
        coarse.scalars["blotchSize"] = 256.0;
        cases.push_back({coarse, "sensor noise, coarse"});
    }
    cases.push_back({everyPathSensor(), "sensor noise, a proxy", true});
    cases.push_back({everyPathSensor(), "sensor noise, a region of interest", false, true});
    cases.push_back({bloom::film_grain_test::fixedPatternOnly(), "sensor noise, fixed pattern only",
                     false, false, true});
    std::uint64_t id = 48600;
    for (const auto& item : cases) {
        cache.clear();
        const std::string name = std::string("film grain native (") + item.name + "): ";
        const auto plan = bloom::film_grain_test::grainPlan(item.values, id);
        id += 200;
        auto request = requestFor(*plan);
        request.time = *RationalTime::create(5, 24);
        if (item.proxy) {
            const auto extent = bloom::render::ImageExtent::create(37, 29);
            request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
        }
        if (item.roi) {
            const auto window = bloom::render::ImageWindow::create(11, 7, 23, 19);
            request.roi = *window.value();
        }
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare");
        if (!oracle.frame() || !prepared)
            continue;
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), name + "executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        std::string detail;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact),
                            name + "CPU parity: " + detail);
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            name + "data/display-window and PAR parity");
        expectations.expect(coldCounters.ocioEffectDispatches > 0 && coldCounters.readbacks == 0,
                            name + "resident dispatches and no per-operation readback");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                warm.pixels == cold.pixels &&
                                executor.executor->counters().dispatches ==
                                    coldCounters.dispatches,
                            name + "warm RAM reuse has identical pixels and zero dispatches");
        // The next frame: new grain on a new dispatch, with parity; held grain the same pixels.
        auto nextRequest = request;
        nextRequest.time = *RationalTime::create(6, 24);
        auto nextOracleRequest = nextRequest;
        nextOracleRequest.bypassOperationCache = true;
        const auto next = builder.build(plan, nextRequest);
        const auto nextOracle = evaluator.evaluate(plan, nextOracleRequest, {});
        expectations.expect(next && nextOracle.frame(), name + "the next frame prepares");
        if (!next || !nextOracle.frame())
            continue;
        const auto beforeNext = executor.executor->counters();
        const auto rerun = runScene(*executor.executor, next.scene);
        const auto nextDispatches = executor.executor->counters().dispatches - beforeNext.dispatches;
        const bool held = item.values.animation == bloom::document::kFilmGrainHeld ||
                          item.frameInvariant;
        expectations.expect(rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(rerun.pixels, nextOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact) &&
                                (held ? (nextDispatches == 0 && rerun.pixels == cold.pixels)
                                      : (nextDispatches > 0 && rerun.pixels != cold.pixels)),
                            name + "the next frame draws its own grain with CPU parity: " + detail);
        // A changed value re-runs with parity.
        GrainValues changedValues = item.values;
        changedValues.scalars["intensity"] = 0.05;
        const auto changedPlan = bloom::film_grain_test::grainPlan(changedValues, id - 200);
        const auto changed = builder.build(changedPlan, request);
        const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
        expectations.expect(changed && changedOracle.frame(), name + "a changed value prepares");
        if (!changed || !changedOracle.frame())
            continue;
        const auto beforeChanged = executor.executor->counters();
        const auto again = runScene(*executor.executor, changed.scene);
        expectations.expect(again.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(again.pixels, changedOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact) &&
                                executor.executor->counters().dispatches > beforeChanged.dispatches,
                            name + "a changed value re-runs with CPU parity: " + detail);
        // A fixed pattern alone has no per-channel grain for Monochrome to share.
        if (item.frameInvariant)
            continue;
        // Toggling Monochrome re-runs with parity on the same node's ids, and neither state serves
        // the other's frame: going back finds the first state's own entry and pixels.
        GrainValues toggledValues = item.values;
        toggledValues.monochrome = !item.values.monochrome;
        const auto toggledPlan = bloom::film_grain_test::grainPlan(toggledValues, id - 200);
        const auto toggled = builder.build(toggledPlan, request);
        const auto toggledOracle = evaluator.evaluate(toggledPlan, oracleRequest, {});
        expectations.expect(toggled && toggledOracle.frame(), name + "the toggle prepares");
        if (!toggled || !toggledOracle.frame())
            continue;
        const auto backPrepared = builder.build(plan, request);
        const auto beforeToggle = executor.executor->counters();
        const auto flipped = runScene(*executor.executor, toggled.scene);
        expectations.expect(flipped.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(flipped.pixels,
                                       toggledOracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact) &&
                                flipped.pixels != cold.pixels &&
                                executor.executor->counters().dispatches > beforeToggle.dispatches,
                            name + "toggling Monochrome redraws with CPU parity: " + detail);
        const auto beforeBack = executor.executor->counters();
        const auto back = runScene(*executor.executor, backPrepared.scene);
        expectations.expect(back.pollResult == GpuSceneExecutorPollResult::Ready &&
                                back.pixels == cold.pixels &&
                                executor.executor->counters().dispatches == beforeBack.dispatches,
                            name + "toggling back finds its own entry: identical pixels, no dispatch");
    }
}

// #527 Dither against the CPU oracle (2e-6, exact alpha): the default blue noise, a strong dither,
// Bayer 2, 4, 8 and 16, 10, 12 and 16 bit, the stored value, Monochrome and Held, a half-scale proxy,
// a region of interest and 512 x 384 frames of both patterns; warm RAM reuse with zero dispatches; the
// next two frames each a new placement with parity (Held the same pixels with no dispatch); a changed
// amplitude re-running with parity; a toggle of Monochrome redrawing while each state keeps its own
// cache entry. Then the lifecycle: a scene beyond the byte budget refused with nothing pinned, a
// cancelled scene that publishes nothing and retires every submission, and the executor running the
// scene afterwards with parity.
void runDithers(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::dither_test::DitherSettings;
    using bloom::dither_test::everyPath;
    using bloom::dither_test::ordered;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    struct Case final {
        DitherSettings settings;
        const char* name;
        bool proxy = false;
        bool roi = false;
        std::uint32_t width = bloom::dither_test::kDitherWidth;
        std::uint32_t height = bloom::dither_test::kDitherHeight;
    };
    std::vector<Case> cases;
    cases.push_back({DitherSettings{}, "default"});
    cases.push_back({everyPath(), "every path"});
    for (const auto side : {2, 4, 8, 16}) {
        auto s = ordered(side);
        s.amplitude = 4.0;
        cases.push_back({s, side == 2    ? "Bayer 2"
                            : side == 4  ? "Bayer 4"
                            : side == 8  ? "Bayer 8"
                                         : "Bayer 16"});
    }
    for (const auto bits : {10, 12, 16}) {
        auto s = everyPath();
        s.bits = bits;
        s.amplitude = 12.0;
        cases.push_back({s, bits == 10 ? "10 bit" : bits == 12 ? "12 bit" : "16 bit"});
    }
    {
        auto stored = everyPath();
        stored.stored = true;
        cases.push_back({stored, "stored value"});
        auto orderedStored = ordered(8);
        orderedStored.stored = true;
        orderedStored.amplitude = 3.0;
        cases.push_back({orderedStored, "Bayer 8, stored value"});
        auto mono = everyPath();
        mono.monochrome = true;
        cases.push_back({mono, "monochrome"});
        auto held = everyPath();
        held.everyFrame = false;
        cases.push_back({held, "held"});
    }
    cases.push_back({everyPath(), "a half-scale proxy", true});
    cases.push_back({ordered(8), "Bayer 8, a half-scale proxy", true});
    cases.push_back({everyPath(), "a region of interest", false, true});
    cases.push_back({everyPath(), "512 x 384", false, false, 512, 384});
    cases.push_back({ordered(16), "Bayer 16, 512 x 384", false, false, 512, 384});
    std::uint64_t id = 52700;
    for (const auto& item : cases) {
        cache.clear();
        const std::string name = std::string("dither native (") + item.name + "): ";
        const auto plan = bloom::dither_test::ditherPlan(item.settings, id, item.width, item.height);
        id += 200;
        auto request = requestFor(*plan);
        request.time = *RationalTime::create(5, 24);
        if (item.proxy) {
            const auto extent = bloom::render::ImageExtent::create(item.width / 2, item.height / 2);
            request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
        }
        if (item.roi) {
            const auto window = bloom::render::ImageWindow::create(11, 7, 23, 19);
            request.roi = *window.value();
        }
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare");
        if (!oracle.frame() || !prepared)
            continue;
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), name + "executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        std::string detail;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact),
                            name + "CPU parity: " + detail);
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            name + "data/display-window and PAR parity");
        expectations.expect(coldCounters.ocioEffectDispatches > 0 && coldCounters.readbacks == 0,
                            name + "resident dispatches and no per-operation readback");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                warm.pixels == cold.pixels &&
                                executor.executor->counters().dispatches ==
                                    coldCounters.dispatches,
                            name + "warm RAM reuse has identical pixels and zero dispatches");
        // The next two frames: a new placement on a new dispatch, with parity; Held the same pixels.
        auto previous = cold.pixels;
        for (const std::int64_t frame : {6, 7}) {
            auto nextRequest = request;
            nextRequest.time = *RationalTime::create(frame, 24);
            auto nextOracleRequest = nextRequest;
            nextOracleRequest.bypassOperationCache = true;
            const auto next = builder.build(plan, nextRequest);
            const auto nextOracle = evaluator.evaluate(plan, nextOracleRequest, {});
            expectations.expect(next && nextOracle.frame(), name + "the next frame prepares");
            if (!next || !nextOracle.frame())
                break;
            const auto beforeNext = executor.executor->counters();
            const auto rerun = runScene(*executor.executor, next.scene);
            const auto nextDispatches =
                executor.executor->counters().dispatches - beforeNext.dispatches;
            const bool held = !item.settings.everyFrame;
            expectations.expect(
                rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                    parity(rerun.pixels, nextOracle.frame()->processImage().pixels(), detail,
                           AlphaGate::Exact) &&
                    (held ? (nextDispatches == 0 && rerun.pixels == previous)
                          : (nextDispatches > 0 && rerun.pixels != previous)),
                name + "frame " + std::to_string(frame) +
                    " draws its own placement with CPU parity: " + detail);
            previous = rerun.pixels;
        }
        // A changed amplitude re-runs with parity.
        DitherSettings changedSettings = item.settings;
        changedSettings.amplitude = item.settings.amplitude * 0.5;
        const auto changedPlan =
            bloom::dither_test::ditherPlan(changedSettings, id - 200, item.width, item.height);
        const auto changed = builder.build(changedPlan, request);
        const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
        expectations.expect(changed && changedOracle.frame(), name + "a changed amplitude prepares");
        if (!changed || !changedOracle.frame())
            continue;
        const auto beforeChanged = executor.executor->counters();
        const auto again = runScene(*executor.executor, changed.scene);
        expectations.expect(again.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(again.pixels, changedOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact) &&
                                executor.executor->counters().dispatches > beforeChanged.dispatches,
                            name + "a changed amplitude re-runs with CPU parity: " + detail);
        // Toggling Monochrome redraws with parity on the same node's ids, and going back finds the
        // first state's own entry and pixels.
        DitherSettings toggledSettings = item.settings;
        toggledSettings.monochrome = !item.settings.monochrome;
        const auto toggledPlan =
            bloom::dither_test::ditherPlan(toggledSettings, id - 200, item.width, item.height);
        const auto toggled = builder.build(toggledPlan, request);
        const auto toggledOracle = evaluator.evaluate(toggledPlan, oracleRequest, {});
        expectations.expect(toggled && toggledOracle.frame(), name + "the toggle prepares");
        if (!toggled || !toggledOracle.frame())
            continue;
        const auto backPrepared = builder.build(plan, request);
        const auto beforeToggle = executor.executor->counters();
        const auto flipped = runScene(*executor.executor, toggled.scene);
        expectations.expect(flipped.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(flipped.pixels,
                                       toggledOracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact) &&
                                flipped.pixels != cold.pixels &&
                                executor.executor->counters().dispatches > beforeToggle.dispatches,
                            name + "toggling Monochrome redraws with CPU parity: " + detail);
        const auto beforeBack = executor.executor->counters();
        const auto back = runScene(*executor.executor, backPrepared.scene);
        expectations.expect(back.pollResult == GpuSceneExecutorPollResult::Ready &&
                                back.pixels == cold.pixels &&
                                executor.executor->counters().dispatches == beforeBack.dispatches,
                            name + "toggling back finds its own entry: identical pixels, no dispatch");
    }
    std::cout << "dither native: " << cases.size()
              << " cases checked for CPU parity, warm reuse, the next frames, a changed amplitude and "
                 "a Monochrome toggle\n";
    // The lifecycle on the default dither.
    cache.clear();
    const std::string name = "dither native (lifecycle): ";
    const auto plan = bloom::dither_test::ditherPlan(everyPath(), id);
    auto request = requestFor(*plan);
    request.time = *RationalTime::create(5, 24);
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
    const auto prepared = builder.build(plan, request);
    expectations.expect(oracle.frame() && prepared, name + "CPU oracle and production scene prepare");
    if (!oracle.frame() || !prepared)
        return;
    std::string detail;
    auto budgeted = GpuSceneExecutor::create(device, cache);
    if (!budgeted)
        return;
    const auto refused = budgeted.executor->begin(prepared.scene, 256);
    expectations.expect(refused.code == GpuSceneExecutorDiagnosticCode::OverBudget &&
                            budgeted.executor->image() == nullptr &&
                            budgeted.executor->counters().budgetRefusals == 1 &&
                            budgeted.executor->counters().currentLiveImageBytes == 0,
                        name + "a scene beyond the byte budget is refused and pins nothing");
    const auto after = runScene(*budgeted.executor, prepared.scene);
    expectations.expect(after.pollResult == GpuSceneExecutorPollResult::Ready &&
                            parity(after.pixels, oracle.frame()->processImage().pixels(), detail,
                                   AlphaGate::Exact) &&
                            !budgeted.executor->hasUnretiredSubmission() &&
                            !budgeted.executor->ownerDrainRequired(),
                        name + "the executor runs the scene after a budget refusal and retires it");
    cache.clear();
    auto cancelled = GpuSceneExecutor::create(device, cache);
    if (!cancelled)
        return;
    expectations.expect(cancelled.executor->begin(prepared.scene, kSceneBudget).code ==
                            GpuSceneExecutorDiagnosticCode::None,
                        name + "the scene to cancel begins");
    static_cast<void>(cancelled.executor->poll());
    cancelled.executor->cancel();
    GpuSceneExecutorPollResult terminal = GpuSceneExecutorPollResult::Pending;
    for (std::uint64_t i = 0; i < 1000000 && terminal == GpuSceneExecutorPollResult::Pending; ++i)
        terminal = cancelled.executor->poll();
    expectations.expect(terminal == GpuSceneExecutorPollResult::Failure &&
                            cancelled.executor->diagnostic().code ==
                                GpuSceneExecutorDiagnosticCode::Cancelled &&
                            cancelled.executor->image() == nullptr &&
                            cancelled.executor->takeImage() == nullptr,
                        name + "a cancelled scene ends Cancelled and publishes no image");
    for (std::uint64_t i = 0; i < 1000000 && (cancelled.executor->hasUnretiredSubmission() ||
                                              cancelled.executor->ownerDrainRequired());
         ++i)
        static_cast<void>(cancelled.executor->poll());
    expectations.expect(!cancelled.executor->hasUnretiredSubmission() &&
                            !cancelled.executor->ownerDrainRequired() &&
                            cancelled.executor->counters().scenesCancelled == 1 &&
                            cancelled.executor->counters().currentLiveImageBytes == 0,
                        name + "a cancelled scene retires every submission and pins nothing");
    const auto reused = runScene(*cancelled.executor, prepared.scene);
    expectations.expect(reused.pollResult == GpuSceneExecutorPollResult::Ready &&
                            parity(reused.pixels, oracle.frame()->processImage().pixels(), detail,
                                   AlphaGate::Exact),
                        name + "the executor runs the scene after a cancellation");
}

// #563 Codec Degrade against the CPU oracle: a frame inside a GOP (so the chain of predicted steps
// runs), a keyframe, 16 px blocks without chroma bleed, a full-frame proxy and a region of interest;
// warm RAM reuse with zero dispatches; a later frame with parity; a changed value re-running with
// parity.
// The commands nothing reads from the scene output: a scene emits only what its output reads.
[[nodiscard]] std::size_t unreachableCommandCount(const bloom::runtime::PreparedGpuScene& scene) {
    using bloom::runtime::GpuSceneCommandIndex;
    const auto& commands = scene.commands();
    std::vector<bool> reached(commands.size(), false);
    std::vector<GpuSceneCommandIndex> pending{scene.outputCommand()};
    while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        if (index >= commands.size() || reached[index])
            continue;
        reached[index] = true;
        std::visit(
            [&](const auto& command) {
                using Command = std::decay_t<decltype(command)>;
                if constexpr (std::is_same_v<Command, bloom::runtime::GpuSceneMergeCommand>) {
                    for (const auto foreground : command.foregrounds)
                        pending.push_back(foreground);
                } else if constexpr (std::is_same_v<Command, bloom::runtime::GpuSceneBlendCommand>) {
                    pending.push_back(command.source);
                    pending.push_back(command.destination);
                    pending.push_back(command.mask);
                } else if constexpr (std::is_same_v<Command,
                                                    bloom::runtime::GpuSceneCryptomatteCommand>) {
                    pending.push_back(command.input);
                    pending.push_back(command.accumulator);
                } else if constexpr (requires { command.input; }) {
                    pending.push_back(command.input);
                }
            },
            commands[index]);
    }
    return static_cast<std::size_t>(std::count(reached.begin(), reached.end(), false));
}

// #514 Temporal denoise: the production chain (the look-back window and one weighted step per
// earlier frame) over a grained HDR picture with a moving partial-alpha block, against the CPU oracle
// at the strict gate with exact alpha: a moving block, a still one, a window reaching before the
// first frame, two frames (one step that is both first and last), a zero tolerance, a proxy and a
// region of interest. Resident dispatches and no readback; one step dispatch per earlier frame; warm
// RAM reuse with zero dispatches; the next frame with parity; a changed strength re-running with
// parity.
void runTemporalDenoise(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::denoise_test::TemporalValues;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    struct Case final {
        TemporalValues values;
        const char* name;
        std::int64_t frame = 5;
        bool proxy = false;
        bool roi = false;
    };
    std::vector<Case> cases;
    cases.push_back({TemporalValues{}, "a moving block"});
    {
        TemporalValues still;
        still.still = true;
        cases.push_back({still, "a still block"});
    }
    cases.push_back({TemporalValues{}, "a window before the first frame", 1});
    {
        TemporalValues two;
        two.frames = 2;
        cases.push_back({two, "two frames"});
        TemporalValues exact;
        exact.strength = 0.0;
        cases.push_back({exact, "a zero tolerance"});
        TemporalValues wide;
        wide.frames = 6;
        wide.strength = 0.6;
        cases.push_back({wide, "six frames, a wide tolerance"});
    }
    cases.push_back({TemporalValues{}, "a proxy", 5, true});
    cases.push_back({TemporalValues{}, "a region of interest", 5, false, true});
    std::uint64_t id = 51400;
    for (const auto& item : cases) {
        cache.clear();
        const std::string name = std::string("temporal denoise native (") + item.name + "): ";
        const auto plan = bloom::denoise_test::temporalPlan(item.values, id);
        id += 200;
        auto request = requestFor(*plan);
        request.time = *RationalTime::create(item.frame, 24);
        if (item.proxy) {
            const auto extent = bloom::render::ImageExtent::create(20, 12);
            request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
        }
        if (item.roi) {
            const auto window = bloom::render::ImageWindow::create(6, 4, 20, 14);
            request.roi = *window.value();
        }
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare: " +
                                prepared.diagnostic.message);
        if (!oracle.frame() || !prepared)
            continue;
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), name + "executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        std::string detail;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact),
                            name + "CPU parity: " + detail);
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            name + "data/display-window and PAR parity");
        // Each earlier frame of the window is one step: every step ran on the device.
        const auto steps = static_cast<std::uint64_t>(item.values.frames - 1);
        expectations.expect(coldCounters.ocioEffectDispatches >= steps &&
                                coldCounters.readbacks == 0,
                            name + "a resident dispatch per step and no per-operation readback");
        expectations.expect(unreachableCommandCount(*prepared.scene) == 0,
                            name + "the scene emits only what its output reads");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                warm.pixels == cold.pixels &&
                                executor.executor->counters().dispatches ==
                                    coldCounters.dispatches,
                            name + "warm RAM reuse has identical pixels and zero dispatches");
        auto nextRequest = request;
        nextRequest.time = *RationalTime::create(item.frame + 1, 24);
        auto nextOracleRequest = nextRequest;
        nextOracleRequest.bypassOperationCache = true;
        const auto next = builder.build(plan, nextRequest);
        const auto nextOracle = evaluator.evaluate(plan, nextOracleRequest, {});
        expectations.expect(next && nextOracle.frame(), name + "the next frame prepares");
        if (!next || !nextOracle.frame())
            continue;
        const auto rerun = runScene(*executor.executor, next.scene);
        expectations.expect(rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(rerun.pixels, nextOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact),
                            name + "the next frame matches the CPU: " + detail);
        TemporalValues changedValues = item.values;
        changedValues.strength += 0.2;
        const auto changedPlan = bloom::denoise_test::temporalPlan(changedValues, id - 200);
        const auto changed = builder.build(changedPlan, request);
        const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
        expectations.expect(changed && changedOracle.frame(), name + "a changed value prepares");
        if (!changed || !changedOracle.frame())
            continue;
        const auto beforeChanged = executor.executor->counters();
        const auto again = runScene(*executor.executor, changed.scene);
        expectations.expect(again.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(again.pixels, changedOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact) &&
                                executor.executor->counters().dispatches > beforeChanged.dispatches,
                            name + "a changed strength re-runs with CPU parity: " + detail);
    }
}

void runCodecDegrades(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::codec_test::CodecValues;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    struct Case final {
        CodecValues values;
        const char* name;
        std::int64_t frame = 6;
        bool proxy = false;
        bool roi = false;
    };
    std::vector<Case> cases;
    cases.push_back({CodecValues{}, "inside a GOP"});
    cases.push_back({CodecValues{}, "a keyframe", 8});
    {
        CodecValues wide;
        wide.blockPixels = 16;
        wide.chromaBleed = false;
        wide.gopFrames = 6;
        cases.push_back({wide, "16 px, no chroma bleed"});
    }
    {
        CodecValues proxy;
        proxy.still = true;
        proxy.aligned = true;
        cases.push_back({proxy, "a proxy", 6, true});
        CodecValues wide = proxy;
        wide.blockPixels = 16;
        cases.push_back({wide, "a proxy, 16 px", 6, true});
        CodecValues keys = proxy;
        keys.gopFrames = 1;
        cases.push_back({keys, "a proxy, every frame a keyframe", 6, true});
    }
    cases.push_back({CodecValues{}, "a region of interest", 6, false, true});
    std::uint64_t id = 56300;
    std::uint64_t insideGopOcio = 0;
    for (const auto& item : cases) {
        cache.clear();
        const std::string name = std::string("codec degrade native (") + item.name + "): ";
        const auto built = bloom::codec_test::codecPlan(item.values, id);
        const auto plan = built.plan;
        id += 200;
        auto request = requestFor(*plan);
        request.time = *RationalTime::create(item.frame, 24);
        if (item.proxy) {
            const auto extent = bloom::render::ImageExtent::create(16, 12);
            request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
        }
        if (item.roi) {
            const auto window = bloom::render::ImageWindow::create(6, 4, 20, 14);
            request.roi = *window.value();
        }
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare");
        if (!oracle.frame() || !prepared)
            continue;
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), name + "executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        std::string detail;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact),
                            name + "CPU parity: " + detail);
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            name + "data/display-window and PAR parity");
        expectations.expect(coldCounters.ocioEffectDispatches > 0 && coldCounters.readbacks == 0,
                            name + "resident dispatches and no per-operation readback");
        expectations.expect(unreachableCommandCount(*prepared.scene) == 0,
                            name + "the scene emits only what its output reads");
        if (std::string_view{item.name} == "inside a GOP")
            insideGopOcio = coldCounters.ocioEffectDispatches;
        if (std::string_view{item.name} == "a keyframe")
            expectations.expect(coldCounters.ocioEffectDispatches < insideGopOcio,
                                name + "a keyframe runs fewer codec passes than a predicted "
                                       "frame, whose Step dispatches stay");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                warm.pixels == cold.pixels &&
                                executor.executor->counters().dispatches ==
                                    coldCounters.dispatches,
                            name + "warm RAM reuse has identical pixels and zero dispatches");
        auto nextRequest = request;
        nextRequest.time = *RationalTime::create(item.frame + 1, 24);
        auto nextOracleRequest = nextRequest;
        nextOracleRequest.bypassOperationCache = true;
        const auto next = builder.build(plan, nextRequest);
        const auto nextOracle = evaluator.evaluate(plan, nextOracleRequest, {});
        expectations.expect(next && nextOracle.frame(), name + "the next frame prepares");
        if (!next || !nextOracle.frame())
            continue;
        const auto rerun = runScene(*executor.executor, next.scene);
        expectations.expect(rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(rerun.pixels, nextOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact),
                            name + "the next frame matches the CPU: " + detail);
        CodecValues changedValues = item.values;
        changedValues.quality = 15.0;
        const auto changedPlan = bloom::codec_test::codecPlan(changedValues, id - 200).plan;
        const auto changed = builder.build(changedPlan, request);
        const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
        expectations.expect(changed && changedOracle.frame(), name + "a changed value prepares");
        if (!changed || !changedOracle.frame())
            continue;
        const auto beforeChanged = executor.executor->counters();
        const auto again = runScene(*executor.executor, changed.scene);
        expectations.expect(again.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(again.pixels, changedOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact) &&
                                executor.executor->counters().dispatches > beforeChanged.dispatches,
                            name + "a changed value re-runs with CPU parity: " + detail);
    }
}

// #566 Auto Exposure against the CPU oracle: a flare mid-window (the attack), a dimming light (the
// release), a warm shot (the white balance), a full-frame proxy and a region of interest, at the
// strict gate with exact alpha; resident dispatches with no readback; only what the output reads;
// warm RAM reuse with zero dispatches; a later frame with parity, whose window shares the earlier
// frame's measurements; a changed value re-running with parity.
// #349 Histogram: the production chain (the tile counts, their sums and the plot) over the HDR,
// signed, partial-alpha gradient against the CPU oracle at the strict gate (color within 2e-6, alpha
// exact; the counts are whole numbers, so only a logarithmic plot differs by a logarithm's last
// bits), the windows, three resident dispatches and no per-operation readback, warm reuse with zero
// dispatches and a changed value that re-runs with parity.
void runHistograms(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    namespace ht = bloom::histogram_test;
    namespace doc = bloom::document;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    struct Case final {
        ht::HistogramValues values;
        const char* name;
        bool proxy = false;
        bool roi = false;
    };
    std::vector<Case> cases;
    cases.push_back({ht::HistogramValues{}, "rgb and luminance"});
    {
        ht::HistogramValues values;
        values.straight = false;
        values.channels = doc::kHistogramRgb;
        cases.push_back({values, "stored rgb"});
        values = {};
        values.channels = doc::kHistogramLuminance;
        values.logarithmic = true;
        cases.push_back({values, "luminance, logarithmic"});
        values = {};
        values.channels = doc::kHistogramAlpha;
        values.rangeMax = 1.0;
        cases.push_back({values, "alpha"});
        values = {};
        values.channels = doc::kHistogramRed;
        values.rangeMin = -1.0;
        values.rangeMax = 4.0;
        values.bins = 128;
        values.height = 32;
        cases.push_back({values, "red over an HDR range, 128 bins"});
        values = {};
        values.rangeMin = 1.0;
        values.rangeMax = 0.0;
        cases.push_back({values, "an empty range"});
        values = {};
        values.sourceWidth = 7;
        values.sourceHeight = 3;
        values.bins = 512;
        values.height = 16;
        cases.push_back({values, "a source smaller than its tile counts"});
    }
    cases.push_back({ht::HistogramValues{}, "a region of interest", false, true});
    std::uint64_t id = 34900;
    for (const auto& item : cases) {
        cache.clear();
        const std::string name = std::string("histogram native (") + item.name + "): ";
        const auto plan = ht::histogramPlan(item.values, id);
        id += 200;
        auto request = requestFor(*plan);
        if (item.roi) {
            const auto window = bloom::render::ImageWindow::create(10, 6, 30, 40);
            request.roi = *window.value();
        }
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare");
        if (!oracle.frame() || !prepared) {
            if (!prepared)
                std::cerr << name << "prepare diagnostic: " << prepared.diagnostic.message << '\n';
            continue;
        }
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), name + "executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        std::string detail;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact),
                            name + "CPU parity: " + detail);
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            name + "data/display-window and PAR parity");
        expectations.expect(coldCounters.ocioEffectDispatches == 3 && coldCounters.readbacks == 0,
                            name + "three resident dispatches and no per-operation readback");
        expectations.expect(unreachableCommandCount(*prepared.scene) == 0,
                            name + "the scene emits only what its output reads");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                warm.pixels == cold.pixels &&
                                executor.executor->counters().dispatches ==
                                    coldCounters.dispatches,
                            name + "warm RAM reuse has identical pixels and zero dispatches");
        ht::HistogramValues changedValues = item.values;
        changedValues.rangeMax = item.values.rangeMax + 0.5;
        const auto changedPlan = ht::histogramPlan(changedValues, id - 200);
        const auto changed = builder.build(changedPlan, request);
        const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
        expectations.expect(changed && changedOracle.frame(), name + "a changed value prepares");
        if (!changed || !changedOracle.frame())
            continue;
        const auto beforeChanged = executor.executor->counters();
        const auto again = runScene(*executor.executor, changed.scene);
        expectations.expect(again.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(again.pixels, changedOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact) &&
                                executor.executor->counters().dispatches > beforeChanged.dispatches,
                            name + "a changed value re-runs with CPU parity: " + detail);
    }
}

// #349 Image Statistics: the production chain (the tiles' reduction and their combination, relocated
// to the origin of the composition) over the HDR, signed, partial-alpha gradient against the CPU oracle
// at the strict gate, requested the way a statistics plan is run (the two-pixel window), two resident
// dispatches and no readback inside the scene, warm reuse with zero dispatches and a changed value
// that re-runs with parity.
void runStatistics(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    namespace stt = bloom::statistics_test;
    namespace doc = bloom::document;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    struct Case final {
        stt::StatisticsValues values;
        const char* name;
    };
    std::vector<Case> cases;
    cases.push_back({stt::StatisticsValues{}, "luminance"});
    {
        stt::StatisticsValues values;
        values.channel = doc::kStatisticsRed;
        values.straight = false;
        cases.push_back({values, "stored red"});
        values = {};
        values.channel = doc::kStatisticsGreen;
        cases.push_back({values, "straight green"});
        values = {};
        values.channel = doc::kStatisticsAlpha;
        cases.push_back({values, "alpha"});
        values = {};
        values.channel = doc::kStatisticsBlue;
        values.opaqueOnly = true;
        cases.push_back({values, "blue, only pixels with alpha"});
        values = {};
        values.gradient = false;
        values.color = bloom::core::Color4d{24.0, 0.0, 0.0, 1.0};
        values.channel = doc::kStatisticsRed;
        cases.push_back({values, "a constant picture"});
        values = {};
        values.pictureWidth = 7;
        values.pictureHeight = 3;
        cases.push_back({values, "a picture smaller than its result"});
        values = {};
        values.pictureWidth = 64;
        values.pictureHeight = 64;
        cases.push_back({values, "a picture that fills the composition"});
    }
    std::uint64_t id = 34910;
    for (const auto& item : cases) {
        cache.clear();
        const std::string name = std::string("statistics native (") + item.name + "): ";
        const auto plan = stt::statisticsPlan(item.values, id);
        id += 200;
        auto request = requestFor(*plan);
        const auto resultWindow = bloom::render::ImageWindow::create(0, 0, 2, 1);
        request.roi = *resultWindow.value();
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare");
        if (!oracle.frame() || !prepared) {
            if (!prepared)
                std::cerr << name << "prepare diagnostic: " << prepared.diagnostic.message << '\n';
            continue;
        }
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), name + "executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        std::string detail;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact),
                            name + "CPU parity: " + detail);
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            name + "data/display-window and PAR parity");
        expectations.expect(cold.dataWindow && cold.dataWindow->extent().width() == 2 &&
                                cold.dataWindow->extent().height() == 1,
                            name + "the scene's output is the two-pixel result, the only readback");
        expectations.expect(coldCounters.ocioEffectDispatches == 2 && coldCounters.readbacks == 0,
                            name + "two resident dispatches and no per-operation readback");
        expectations.expect(unreachableCommandCount(*prepared.scene) == 0,
                            name + "the scene emits only what its output reads");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                warm.pixels == cold.pixels &&
                                executor.executor->counters().dispatches ==
                                    coldCounters.dispatches,
                            name + "warm RAM reuse has identical pixels and zero dispatches");
        stt::StatisticsValues changedValues = item.values;
        changedValues.straight = !item.values.straight;
        const auto changedPlan = stt::statisticsPlan(changedValues, id - 200);
        const auto changed = builder.build(changedPlan, request);
        const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
        expectations.expect(changed && changedOracle.frame(), name + "a changed value prepares");
        if (!changed || !changedOracle.frame())
            continue;
        const auto beforeChanged = executor.executor->counters();
        const auto again = runScene(*executor.executor, changed.scene);
        expectations.expect(again.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(again.pixels, changedOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact) &&
                                executor.executor->counters().dispatches > beforeChanged.dispatches,
                            name + "a changed value re-runs with CPU parity: " + detail);
    }

    // #349: the count is exact above 2^24 on the device too. The Sum pass alone, over tables of
    // tiles whose counts total 50,331,650, 25,165,833 and 16,777,219 (none a binary32 number), gives
    // the CPU reference's bits exactly, and the count read back is exact.
    std::vector<std::pair<stt::TileTable, double>> tables;
    tables.push_back({stt::TileTable{}, 50331650.0});
    stt::TileTable split;
    split.top = bloom::core::Color4d{6.0, 0.25, 3.0, 1.0};
    split.bottom = bloom::core::Color4d{8388609.0, 0.5, 2.0, 1.0};
    tables.push_back({split, 25165833.0});
    stt::TileTable whole;
    whole.top = bloom::core::Color4d{3.0, -1.5, 7.0, 1.0};
    whole.bottom = bloom::core::Color4d{16777216.0, 1.0, 1.0, 1.0};
    whole.rows = 2;
    tables.push_back({whole, 16777219.0});
    std::uint64_t tableId = 35600;
    for (const auto& [table, exact] : tables) {
        cache.clear();
        const std::string name = "statistics native (a count of " +
                                 std::to_string(static_cast<std::int64_t>(exact)) + "): ";
        const auto plan = stt::tileTableSumPlan(table, tableId);
        tableId += 100;
        auto request = requestFor(*plan);
        const auto resultWindow = bloom::render::ImageWindow::create(0, 0, 2, 1);
        request.roi = *resultWindow.value();
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare");
        if (!oracle.frame() || !prepared)
            continue;
        auto executor = GpuSceneExecutor::create(device, cache);
        if (!executor)
            continue;
        const auto run = runScene(*executor.executor, prepared.scene);
        const auto expected = oracle.frame()->processImage().pixels();
        expectations.expect(run.pollResult == GpuSceneExecutorPollResult::Ready &&
                                run.pixels.size() == expected.size() &&
                                std::memcmp(run.pixels.data(), expected.data(),
                                            expected.size() * sizeof(Rgba32f)) == 0,
                            name + "the device gives the CPU reference's bits");
        if (run.pixels.size() != 2)
            continue;
        const auto descriptor = bloom::render::Rgba32fImageDescriptor::create(
            *resultWindow.value(), *resultWindow.value(),
            bloom::core::PixelAspectRatio::square());
        auto builderImage = bloom::render::Rgba32fImageBuilder::create(*descriptor.value(), 1U << 20U);
        if (!builderImage)
            continue;
        auto row = builderImage.value()->row(0);
        (*row.value())[0] = run.pixels[0];
        (*row.value())[1] = run.pixels[1];
        auto frozen = std::move(*builderImage.value()).freeze();
        const auto values = frozen ? bloom::runtime::readImageStatistics(
                                         *frozen.value(), bloom::runtime::ImageStatisticsRoute::Device)
                                   : std::nullopt;
        expectations.expect(values && values->count == exact,
                            name + "the count read back from the device is exact");
    }
}

// #349: an Image Statistics value in a final render's frame is measured by the device route of the
// same evaluator, for that frame's own time, and only the two-pixel result is read back: the frame
// is as high as the picture's maximum, the cache holds a result labelled with the device, the same
// frame again measures nothing and another frame measures its own.
void runStatisticsRoute(Expectations& expectations) {
    namespace stt = bloom::statistics_test;
    bloom::runtime::GpuProcessFrameEvaluatorOptions evaluatorOptions;
    evaluatorOptions.enabled = true;
    evaluatorOptions.loaderPath = loaderPath();
    evaluatorOptions.ocioContext = std::make_shared<const GpuSceneOcioContext>(ocioContext());
    auto evaluator = bloom::runtime::GpuProcessFrameEvaluator::create(evaluatorOptions);
    expectations.expect(evaluator != nullptr && evaluator->gpuAvailable(),
                        "statistics: the evaluator is constructed on the same device");
    if (evaluator == nullptr || !evaluator->gpuAvailable()) {
        return;
    }
    bloom::runtime::clearImageStatistics();
    stt::StatisticsValues red;
    red.gradient = false;
    red.color = bloom::core::Color4d{24.0, 0.0, 0.0, 1.0};
    red.channel = bloom::document::kStatisticsRed;
    const auto tap = stt::statisticsPlan(red, 36000);
    const auto driven = stt::drivenPlan(tap, 1, 36100);
    const auto request = requestFor(*driven);
    const auto height = [](const bloom::runtime::GpuProcessFrameOutcome& outcome) {
        std::int64_t rows = 0;
        if (outcome.frame == nullptr)
            return rows;
        const auto& image = outcome.frame->processImage();
        const auto window = image.descriptor()->dataWindow();
        for (std::int64_t y = window.originY();
             y < window.originY() + static_cast<std::int64_t>(window.extent().height()); ++y)
            if (const auto pixel = image.read(32, y); pixel && pixel.value()->alpha() > 0.5F)
                ++rows;
        return rows;
    };
    const auto first = evaluator->evaluate(driven, request);
    expectations.expect(first.status == bloom::runtime::GpuProcessFrameStatus::Evaluated && first.frame != nullptr,
                        "statistics: the frame evaluates on the device route: " +
                            first.diagnostic.message);
    expectations.expect(height(first) == 24,
                        "statistics: the frame is as high as the picture's maximum, 24");
    auto statistics = bloom::runtime::imageStatisticsCacheStatistics();
    expectations.expect(statistics.computed == 1 && statistics.computedOnDevice == 1,
                        "statistics: the frame's statistics were measured once, on the device");
    const auto again = evaluator->evaluate(driven, request);
    statistics = bloom::runtime::imageStatisticsCacheStatistics();
    expectations.expect(again.status == bloom::runtime::GpuProcessFrameStatus::Evaluated && height(again) == 24 &&
                            statistics.computed == 1 && statistics.hits >= 1,
                        "statistics: the same frame again reads the cache and measures nothing");
    auto later = request;
    later.time = bloom::core::RationalTime::fromInteger(3);
    const auto next = evaluator->evaluate(driven, later);
    statistics = bloom::runtime::imageStatisticsCacheStatistics();
    expectations.expect(next.status == bloom::runtime::GpuProcessFrameStatus::Evaluated && height(next) == 24 &&
                            statistics.computed == 2 && statistics.computedOnDevice == 2,
                        "statistics: another frame measures its own statistics on the device");
    // A picture that is edited gives its own statistics.
    red.color = bloom::core::Color4d{10.0, 0.0, 0.0, 1.0};
    const auto edited = stt::drivenPlan(stt::statisticsPlan(red, 36000), 1, 36100);
    const auto editedOutcome = evaluator->evaluate(edited, request);
    expectations.expect(editedOutcome.status == bloom::runtime::GpuProcessFrameStatus::Evaluated &&
                            height(editedOutcome) == 10,
                        "statistics: an edited picture is measured again, not read from the cache");
    // Cancelled before it starts: no measure is cached.
    evaluator->beginShutdown();
}

// #347 Audio Waveform and Audio Spectrum: the production chain (the prepared analysis drawn by the one
// plot pass) over a generated WAV against the CPU oracle at the strict gate, the windows, no
// per-operation readback, warm reuse with zero dispatches and a changed time that re-runs with parity.
void runAudioPlots(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    namespace apt = bloom::audio_plot_test;
    const CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(apt::sharedAudioFixture().directory);
    auto media = GpuSceneMediaContext::fromEvaluator(evaluator);
    media.assetBaseDirectory = apt::sharedAudioFixture().directory;
    const CpuGpuSceneBuilder builder(nullptr, media, ocioContext());
    struct Case final {
        apt::AudioPlotValues values;
        const char* name;
        bool roi = false;
        // A render below full scale: the proxy's width and height, or none.
        std::uint32_t proxyWidth = 0;
        std::uint32_t proxyHeight = 0;
    };
    std::vector<Case> cases;
    cases.push_back({apt::AudioPlotValues{}, "waveform"});
    {
        apt::AudioPlotValues values;
        values.separate = true;
        values.values = {1.0, 2.0, 0.0, 0.0};
        values.color = {3.0, 0.0, 0.5, 0.75};
        cases.push_back({values, "waveform, separate lanes, HDR line"});
        values = apt::spectrumValues();
        cases.push_back({values, "spectrum, linear"});
        values.logarithmic = true;
        values.fftSize = 4096;
        values.values = {30.0, 16000.0, -60.0, -6.0};
        values.background = {0.25, 0.25, 0.25, 1.0};
        cases.push_back({values, "spectrum, logarithmic"});
        values = apt::spectrumValues();
        values.withSource = false;
        cases.push_back({values, "silence"});
    }
    cases.push_back({apt::spectrumValues(), "a region of interest", true});
    // #347: below full render scale the plot draws the same picture over the scaled canvas, with the
    // CPU reference's bits.
    cases.push_back({apt::AudioPlotValues{}, "waveform at half scale", false, 32, 32});
    cases.push_back({apt::spectrumValues(), "spectrum at half scale", false, 32, 32});
    {
        apt::AudioPlotValues values;
        values.separate = true;
        values.values = {1.0, 2.0, 0.0, 0.0};
        cases.push_back({values, "waveform, separate lanes, at an odd scale", false, 37, 29});
        values = apt::spectrumValues();
        values.logarithmic = true;
        cases.push_back({values, "spectrum, logarithmic, at an odd scale", false, 37, 29});
        values = apt::spectrumValues();
        values.width = 300;
        values.height = 100;
        cases.push_back({values, "a wide spectrum folded to a narrow canvas", false, 16, 16});
    }
    std::uint64_t id = 347000;
    for (const auto& item : cases) {
        cache.clear();
        const std::string name = std::string("audio plot native (") + item.name + "): ";
        const auto plan = apt::audioPlotPlan(item.values, id);
        id += 200;
        auto request = requestFor(*plan);
        if (item.proxyWidth != 0) {
            const auto extent = bloom::render::ImageExtent::create(item.proxyWidth, item.proxyHeight);
            request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
        }
        if (item.roi) {
            const auto window = bloom::render::ImageWindow::create(10, 6, 30, 40);
            request.roi = *window.value();
        }
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare");
        if (!oracle.frame() || !prepared) {
            if (!prepared)
                std::cerr << name << "prepare diagnostic: " << prepared.diagnostic.message << '\n';
            continue;
        }
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), name + "executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        std::string detail;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact),
                            name + "CPU parity: " + detail);
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            name + "data/display-window and PAR parity");
        expectations.expect(coldCounters.ocioEffectDispatches >= 1 && coldCounters.readbacks == 0,
                            name + "a resident plot dispatch and no per-operation readback");
        expectations.expect(unreachableCommandCount(*prepared.scene) == 0,
                            name + "the scene emits only what its output reads");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                warm.pixels == cold.pixels &&
                                executor.executor->counters().dispatches ==
                                    coldCounters.dispatches,
                            name + "warm RAM reuse has identical pixels and zero dispatches");
        if (!item.values.withSource)
            continue;
        apt::AudioPlotValues changedValues = item.values;
        changedValues.startFrame = -30; // 1.25 s: the left sine has stopped
        const auto changedPlan = apt::audioPlotPlan(changedValues, id - 200);
        const auto changed = builder.build(changedPlan, request);
        const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
        expectations.expect(changed && changedOracle.frame(), name + "another time prepares");
        if (!changed || !changedOracle.frame())
            continue;
        const auto beforeChanged = executor.executor->counters();
        const auto again = runScene(*executor.executor, changed.scene);
        expectations.expect(again.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(again.pixels, changedOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact) &&
                                executor.executor->counters().dispatches > beforeChanged.dispatches,
                            name + "another time re-runs with CPU parity: " + detail);
    }
}

void runAutoExposures(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::exposure_test::ExposureValues;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    struct Case final {
        ExposureValues values;
        const char* name;
        std::int64_t frame = 15;
        bool proxy = false;
        bool roi = false;
    };
    std::vector<Case> cases;
    cases.push_back({ExposureValues{}, "a flare"});
    {
        ExposureValues dimming;
        dimming.before = {0.72, 0.72, 0.72};
        dimming.after = {0.18, 0.18, 0.18};
        cases.push_back({dimming, "a dimming light", 16});
        ExposureValues warm;
        warm.steady = true;
        warm.before = {0.3, 0.2, 0.1};
        warm.lookBack = 8;
        cases.push_back({warm, "a warm shot", 3});
        ExposureValues start;
        start.lookBack = 40;
        cases.push_back({start, "a window reaching before the first frame", 6});
    }
    cases.push_back({ExposureValues{}, "a proxy", 15, true});
    cases.push_back({ExposureValues{}, "a region of interest", 15, false, true});
    std::uint64_t id = 56600;
    for (const auto& item : cases) {
        cache.clear();
        const std::string name = std::string("auto exposure native (") + item.name + "): ";
        const auto built = bloom::exposure_test::exposurePlan(item.values, id);
        const auto plan = built.plan;
        id += 200;
        auto request = requestFor(*plan);
        request.time = *RationalTime::create(item.frame, 24);
        if (item.proxy) {
            const auto extent = bloom::render::ImageExtent::create(16, 12);
            request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
        }
        if (item.roi) {
            const auto window = bloom::render::ImageWindow::create(6, 4, 20, 14);
            request.roi = *window.value();
        }
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare");
        if (!oracle.frame() || !prepared) {
            if (!prepared)
                std::cerr << name << "prepare diagnostic: " << prepared.diagnostic.message << '\n';
            continue;
        }
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), name + "executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        std::string detail;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact),
                            name + "CPU parity: " + detail);
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            name + "data/display-window and PAR parity");
        expectations.expect(coldCounters.ocioEffectDispatches > 0 && coldCounters.readbacks == 0,
                            name + "resident dispatches and no per-operation readback");
        expectations.expect(unreachableCommandCount(*prepared.scene) == 0,
                            name + "the scene emits only what its output reads");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                warm.pixels == cold.pixels &&
                                executor.executor->counters().dispatches ==
                                    coldCounters.dispatches,
                            name + "warm RAM reuse has identical pixels and zero dispatches");
        auto nextRequest = request;
        nextRequest.time = *RationalTime::create(item.frame + 1, 24);
        auto nextOracleRequest = nextRequest;
        nextOracleRequest.bypassOperationCache = true;
        const auto next = builder.build(plan, nextRequest);
        const auto nextOracle = evaluator.evaluate(plan, nextOracleRequest, {});
        expectations.expect(next && nextOracle.frame(), name + "the next frame prepares");
        if (!next || !nextOracle.frame())
            continue;
        const auto beforeNext = executor.executor->counters();
        const auto rerun = runScene(*executor.executor, next.scene);
        const auto afterNext = executor.executor->counters();
        expectations.expect(rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(rerun.pixels, nextOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact),
                            name + "the next frame matches the CPU: " + detail);
        expectations.expect(afterNext.dispatches - beforeNext.dispatches < coldCounters.dispatches,
                            name + "the next frame shares the earlier frame's measurements");
        ExposureValues changedValues = item.values;
        changedValues.target = 0.3;
        const auto changedPlan = bloom::exposure_test::exposurePlan(changedValues, id - 200).plan;
        const auto changed = builder.build(changedPlan, request);
        const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
        expectations.expect(changed && changedOracle.frame(), name + "a changed value prepares");
        if (!changed || !changedOracle.frame())
            continue;
        const auto beforeChanged = executor.executor->counters();
        const auto again = runScene(*executor.executor, changed.scene);
        expectations.expect(again.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(again.pixels, changedOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact) &&
                                executor.executor->counters().dispatches > beforeChanged.dispatches,
                            name + "a changed value re-runs with CPU parity: " + detail);
    }
}

// #484 Motion blur: every kernel through the production chains over the moving HDR texture against
// the CPU oracle at the strict gate (colour within 2e-6, alpha exact), at frame 4. The windows,
// resident dispatches with no readback, warm reuse with zero dispatches and a changed value
// re-running only the chain after it (the texture stays cached) hold.
void runMotionBlur(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::motion_test::MotionValues;
    using bloom::motion_test::Scene;
    namespace doc = bloom::document;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    const auto draft = doc::kMotionQualityDraft;
    struct Case final {
        MotionValues values;
        const char* name;
    };
    std::vector<Case> cases{
        {{.scene = Scene::Directional, .quality = draft}, "directional"},
        {{.scene = Scene::Directional, .curve = doc::kShutterCurveGaussian}, "directional gaussian"},
        {{.scene = Scene::Spin, .quality = draft}, "spin"},
        {{.scene = Scene::Zoom, .quality = draft, .jitter = true}, "zoom jittered"},
        {{.scene = Scene::TransformBlur, .quality = draft}, "transform shutter"},
        {{.scene = Scene::TransformBlur, .curve = doc::kShutterCurveCustom}, "transform custom curve"},
        {{.scene = Scene::TransformBlur, .quality = draft, .skew = 25.0, .skewAxis = 10.0,
          .fadeOpacity = true},
         "transform shutter, skew and fading opacity"},
        {{.scene = Scene::SubFrame, .quality = draft}, "sub-frame"},
        {{.scene = Scene::SubFrame, .quality = draft, .curve = doc::kShutterCurveTriangle,
          .offset = doc::kShutterOffsetEnd},
         "sub-frame triangle, end"},
        {{.scene = Scene::Echo}, "echo add"},
        {{.scene = Scene::Echo, .op = bloom::runtime::AccumulateOperator::Maximum}, "echo maximum"},
        {{.scene = Scene::Echo, .op = bloom::runtime::AccumulateOperator::Minimum}, "echo minimum"},
        {{.scene = Scene::Echo, .op = bloom::runtime::AccumulateOperator::Screen}, "echo screen"},
        {{.scene = Scene::Echo, .op = bloom::runtime::AccumulateOperator::Over, .posterize = true},
         "echo over, posterized"},
        {{.scene = Scene::Echo, .op = bloom::runtime::AccumulateOperator::Under}, "echo under"},
        {{.scene = Scene::Echo, .layerIn = 3}, "echo, early samples absent"},
        {{.scene = Scene::Echo, .layerOut = 2}, "echo, the frame's sample absent"},
        {{.scene = Scene::VectorGather, .quality = draft}, "vector gather"},
        {{.scene = Scene::VectorGather, .jitter = true, .curved = true, .depth = true},
         "vector gather curved, depth, jitter"},
        {{.scene = Scene::VectorSplat, .quality = draft}, "vector splat"},
        {{.scene = Scene::VectorSplat, .quality = draft, .curved = true, .depth = true},
         "vector splat curved, depth"},
        {{.scene = Scene::VectorLayered, .quality = draft}, "vector layered matte"},
        {{.scene = Scene::VectorGather, .quality = draft, .alphaValid = true},
         "vector gather, alpha marks valid"},
        {{.scene = Scene::VectorSplat, .quality = draft, .alphaValid = true},
         "vector splat, alpha marks valid"},
        {{.scene = Scene::FlowDis, .quality = draft}, "flow DIS draft"},
        {{.scene = Scene::FlowDis, .quality = doc::kMotionQualityFinal}, "flow DIS final"},
        {{.scene = Scene::FlowTvl1, .quality = draft}, "flow TV-L1 draft"},
        {{.scene = Scene::FlowTvl1, .quality = doc::kMotionQualityFinal}, "flow TV-L1 final"},
        {{.scene = Scene::FlowTvl1, .quality = draft, .border = doc::kFlowBorderWrap}, "flow TV-L1 wrap"},
        {{.scene = Scene::FlowConsistency, .quality = draft}, "flow consistency"},
        {{.scene = Scene::FlowConsistency, .quality = draft, .depth = true}, "flow occlusion"},
        {{.scene = Scene::FlowCut, .quality = draft}, "flow cut"},
        {{.scene = Scene::FlowCut, .quality = draft, .depth = true}, "flow cut unrelated"},
        {{.scene = Scene::FlowBlur, .quality = draft}, "flow blur"},
        {{.scene = Scene::FlowBlur, .quality = draft, .border = doc::kFlowBorderWrap}, "flow blur wrap"},
        {{.scene = Scene::FlowTvl1, .quality = draft, .matteSolve = true}, "flow TV-L1 matte"},
        {{.scene = Scene::FlowDis, .quality = draft, .matteSolve = true}, "flow DIS matte"},
        {{.scene = Scene::FlowBlur, .quality = doc::kMotionQualityHigh}, "flow blur High, quadratic"},
        {{.scene = Scene::FlowBlurMatte, .quality = draft}, "flow blur matte"},
        {{.scene = Scene::FlowBlurTemporal, .quality = draft}, "flow blur temporal"},
    };
    std::uint64_t id = 48400;
    for (const auto& item : cases) {
        cache.clear();
        const std::string name = std::string("motion native (") + item.name + "): ";
        const auto plan = bloom::motion_test::motionPlan(item.values, id).plan;
        // A changed value: a wider shutter, or what the scene reads instead of one.
        auto changedValues = item.values;
        switch (item.values.scene) {
        case Scene::Directional:
        case Scene::Spin:
        case Scene::Zoom:
            changedValues.quality =
                item.values.quality == draft ? doc::kMotionQualityStandard : draft;
            break;
        case Scene::Echo:
            changedValues.op = item.values.op == bloom::runtime::AccumulateOperator::Add
                                   ? bloom::runtime::AccumulateOperator::Screen
                                   : bloom::runtime::AccumulateOperator::Add;
            break;
        case Scene::FlowDis:
        case Scene::FlowTvl1:
        case Scene::FlowCut:
        case Scene::FlowConsistency:
            changedValues.border = item.values.border == doc::kFlowBorderExtend
                                       ? doc::kFlowBorderBlack
                                       : doc::kFlowBorderExtend;
            break;
        default:
            changedValues.shutter = 0.75;
            break;
        }
        const auto changedPlan = bloom::motion_test::motionPlan(changedValues, id).plan;
        id += 200;
        auto request = requestFor(*plan);
        request.time = *RationalTime::create(4, 24);
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        if (!prepared)
            std::cerr << name << prepared.diagnostic.message << '\n';
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare");
        if (!oracle.frame() || !prepared)
            continue;
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), name + "executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        std::string detail;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact),
                            name + "CPU parity: " + detail);
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            name + "data/display-window and PAR parity");
        expectations.expect(coldCounters.ocioEffectDispatches > 0 && coldCounters.readbacks == 0,
                            name + "resident dispatches and no per-operation readback");
        // A pass inside the effect is not retained: the cache holds results, not solver iterations.
        expectations.expect(2 * cache.entryCount() <= coldCounters.dispatches + 16,
                            name + "the scene cache keeps results, not every pass: " +
                                std::to_string(cache.entryCount()) + " entries, " +
                                std::to_string(coldCounters.dispatches) + " dispatches");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                warm.pixels == cold.pixels &&
                                executor.executor->counters().dispatches ==
                                    coldCounters.dispatches,
                            name + "warm RAM reuse has identical pixels and zero dispatches");
        const auto changed = builder.build(changedPlan, request);
        const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
        expectations.expect(changed && changedOracle.frame(), name + "a changed value prepares");
        if (!changed || !changedOracle.frame())
            continue;
        const auto rerun = runScene(*executor.executor, changed.scene);
        const auto rerunDispatches =
            executor.executor->counters().dispatches - coldCounters.dispatches;
        expectations.expect(
            rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                parity(rerun.pixels, changedOracle.frame()->processImage().pixels(), detail,
                       AlphaGate::Exact) &&
                rerunDispatches > 0,
            name + "a changed value re-runs with CPU parity: " + detail + ", " +
                std::to_string(rerunDispatches) + " of " +
                std::to_string(coldCounters.dispatches) + " dispatches");
    }
}

// #564 Frame Rate Hold against the CPU oracle, at frame 4: a regular hold and a seeded irregular
// cadence. A time sample is the identity on its input, so the pass that runs is the plate's own
// (a translation, not an OCIO effect): the frame has resident dispatches and no readback, matches the
// CPU reference, is reused warm with zero dispatches, and another held rate re-runs.
void runFrameRateHold(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::motion_test::MotionValues;
    using bloom::motion_test::Scene;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    struct Case final {
        MotionValues values;
        const char* name;
    };
    // Held to 5 fps, frame 4 shows the plate at frame 0, where it is displaced.
    const std::vector<Case> cases{
        {{.scene = Scene::FrameRate, .holdRate = 5000}, "regular"},
        {{.scene = Scene::FrameRate, .cadenceChance = 500, .cadenceSeed = 3, .holdRate = 5000},
         "irregular cadence"},
    };
    std::uint64_t id = 48200;
    for (const auto& item : cases) {
        cache.clear();
        const std::string name = std::string("frame rate hold (") + item.name + "): ";
        const auto plan = bloom::motion_test::motionPlan(item.values, id).plan;
        auto changedValues = item.values;
        changedValues.holdRate = 8000;
        const auto changedPlan = bloom::motion_test::motionPlan(changedValues, id).plan;
        id += 200;
        auto request = requestFor(*plan);
        request.time = *RationalTime::create(4, 24);
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare");
        if (!oracle.frame() || !prepared)
            continue;
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), name + "executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        std::string detail;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact),
                            name + "CPU parity: " + detail);
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            name + "data/display-window and PAR parity");
        expectations.expect(coldCounters.dispatches > 0 && coldCounters.readbacks == 0,
                            name + "resident dispatches and no per-operation readback");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                warm.pixels == cold.pixels &&
                                executor.executor->counters().dispatches ==
                                    coldCounters.dispatches,
                            name + "warm RAM reuse has identical pixels and zero dispatches");
        // Another frame of the same hold is the same source time: the same pixels, no dispatches.
        auto sameHold = request;
        sameHold.time = *RationalTime::create(3, 24);
        const auto held = builder.build(plan, sameHold);
        expectations.expect(static_cast<bool>(held), name + "a frame of the same hold prepares");
        if (held) {
            const auto again = runScene(*executor.executor, held.scene);
            expectations.expect(again.pollResult == GpuSceneExecutorPollResult::Ready &&
                                    again.pixels == cold.pixels &&
                                    executor.executor->counters().dispatches ==
                                        coldCounters.dispatches,
                                name + "a frame holding the same source time reuses the result "
                                       "with zero dispatches");
        }
        const auto changed = builder.build(changedPlan, request);
        const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
        expectations.expect(changed && changedOracle.frame(), name + "another rate prepares");
        if (!changed || !changedOracle.frame())
            continue;
        const auto rerun = runScene(*executor.executor, changed.scene);
        expectations.expect(rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(rerun.pixels, changedOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact) &&
                                executor.executor->counters().dispatches > coldCounters.dispatches,
                            name + "another rate re-runs with CPU parity: " + detail);
    }
}

// #486 Sharpen against the CPU oracle: the default, a strong and a wide one, a threshold, the blur's
// half-resolution proxy with its blur's fractional radius and a region of interest; warm RAM reuse with
// zero dispatches; a changed amount or threshold keeping the blur and a changed radius running its two
// passes again, each with parity.
void runSharpens(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    using bloom::sharpen_test::SharpenValues;
    const CpuCompositionEvaluator evaluator;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    struct Case final {
        SharpenValues values;
        const char* name;
        bool proxy = false;
        bool roi = false;
    };
    const std::vector<Case> cases{{{}, "default"},
                                  {{3.5, 2.0, 0.0}, "strong and narrow"},
                                  {{1.5, 24.0, 0.0}, "wide"},
                                  {{2.0, 4.0, 0.25}, "a threshold"},
                                  {{1.5, 5.0, 0.05}, "a proxy", true},
                                  {{1.5, 6.0, 0.05}, "a region of interest", false, true}};
    std::uint64_t id = 49800;
    for (const auto& item : cases) {
        cache.clear();
        const std::string name = std::string("sharpen native (") + item.name + "): ";
        const auto plan = bloom::sharpen_test::sharpenPlan(item.values, id);
        id += 200;
        auto request = requestFor(*plan);
        if (item.proxy) {
            const auto extent = bloom::render::ImageExtent::create(32, 24);
            request.resolution = bloom::runtime::ProxyResolution{*extent.value()};
        }
        if (item.roi) {
            const auto window = bloom::render::ImageWindow::create(11, 7, 23, 19);
            request.roi = *window.value();
        }
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto prepared = builder.build(plan, request);
        expectations.expect(oracle.frame() && prepared,
                            name + "CPU oracle and production scene prepare: " +
                                prepared.diagnostic.message);
        if (!oracle.frame() || !prepared)
            continue;
        auto executor = GpuSceneExecutor::create(device, cache);
        expectations.expect(executor.hasValue(), name + "executor creates");
        if (!executor)
            continue;
        const auto cold = runScene(*executor.executor, prepared.scene);
        const auto coldCounters = executor.executor->counters();
        std::string detail;
        expectations.expect(cold.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(cold.pixels, oracle.frame()->processImage().pixels(), detail,
                                       AlphaGate::Exact),
                            name + "CPU parity: " + detail);
        const auto& descriptor = *oracle.frame()->processImage().descriptor();
        expectations.expect(cold.dataWindow == descriptor.dataWindow() &&
                                cold.displayWindow == descriptor.displayWindow() &&
                                cold.pixelAspect == descriptor.pixelAspect(),
                            name + "data/display-window and PAR parity");
        expectations.expect(coldCounters.dispatches > 0 && coldCounters.readbacks == 0,
                            name + "resident dispatches and no per-operation readback");
        const auto warm = runScene(*executor.executor, prepared.scene);
        expectations.expect(warm.pollResult == GpuSceneExecutorPollResult::Ready &&
                                warm.pixels == cold.pixels &&
                                executor.executor->counters().dispatches == coldCounters.dispatches,
                            name + "warm RAM reuse has identical pixels and zero dispatches");
        // A changed amount or threshold re-runs the mask and what follows it over the retained blur.
        std::uint64_t maskDispatches = 0;
        for (const auto& [changedValues, what] :
             {std::pair{SharpenValues{item.values.amount + 0.5, item.values.radius,
                                      item.values.threshold},
                        "amount"},
              std::pair{SharpenValues{item.values.amount, item.values.radius,
                                      item.values.threshold + 0.125},
                        "threshold"}}) {
            const auto changedPlan = bloom::sharpen_test::sharpenPlan(changedValues, id - 200);
            const auto changed = builder.build(changedPlan, request);
            const auto changedOracle = evaluator.evaluate(changedPlan, oracleRequest, {});
            expectations.expect(changed && changedOracle.frame(),
                                name + "a changed " + what + " prepares");
            if (!changed || !changedOracle.frame())
                continue;
            const auto before = executor.executor->counters();
            const auto again = runScene(*executor.executor, changed.scene);
            maskDispatches = executor.executor->counters().dispatches - before.dispatches;
            expectations.expect(again.pollResult == GpuSceneExecutorPollResult::Ready &&
                                    parity(again.pixels,
                                           changedOracle.frame()->processImage().pixels(), detail,
                                           AlphaGate::Exact) &&
                                    maskDispatches > 0,
                                name + "a changed " + what + " re-runs with CPU parity: " + detail);
        }
        // A changed radius also re-runs the blur's two passes: those are all that a changed amount
        // kept.
        const auto widerPlan = bloom::sharpen_test::sharpenPlan(
            {item.values.amount, item.values.radius + 2.0, item.values.threshold}, id - 200);
        const auto wider = builder.build(widerPlan, request);
        const auto widerOracle = evaluator.evaluate(widerPlan, oracleRequest, {});
        expectations.expect(wider && widerOracle.frame(), name + "a changed radius prepares");
        if (!wider || !widerOracle.frame())
            continue;
        const auto beforeWider = executor.executor->counters();
        const auto rerun = runScene(*executor.executor, wider.scene);
        expectations.expect(rerun.pollResult == GpuSceneExecutorPollResult::Ready &&
                                parity(rerun.pixels, widerOracle.frame()->processImage().pixels(),
                                       detail, AlphaGate::Exact) &&
                                executor.executor->counters().dispatches ==
                                    beforeWider.dispatches + maskDispatches + 2,
                            name + "a changed radius re-runs the blur with CPU parity: " + detail);
    }
}

#include "gpu_scene_proxy_edge_native_tests.ipp"

void runNative(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache,
               const std::string& from, const std::string& to) {
    runBatchedChains(expectations, device, cache);
    runCst(expectations, device, cache, from, to);
    runPointwise(expectations, device, cache);
    runPointwiseExtremeAlpha(expectations, device, cache);
    runMultiplyAlpha(expectations, device, cache);
    runCanvas(expectations, device, cache);
    runDiscrete(expectations, device, cache);
    runShapeMergeEntry(expectations, device, cache);
    runResizeFit(expectations, device, cache);
    runTransform2D(expectations, device, cache);
    runBlurs(expectations, device, cache);
    runDenoise(expectations, device, cache);
    runSeparateBlurs(expectations, device, cache);
    runLumaKeys(expectations, device, cache);
    runColorGrades(expectations, device, cache);
    runChannelSelects(expectations, device, cache);
    runPixelMaths(expectations, device, cache);
    runRecipes(expectations, device, cache);
    runInnerLifecycle(expectations, device, cache);
    runFields(expectations, device, cache);
    runDepthEffects(expectations, device, cache);
    runLensFlares(expectations, device, cache);
    runFilmGrains(expectations, device, cache);
    runDithers(expectations, device, cache);
    runCodecDegrades(expectations, device, cache);
    runProxyEdges(expectations, device, cache);
    runTemporalDenoise(expectations, device, cache);
    runAutoExposures(expectations, device, cache);
    runHistograms(expectations, device, cache);
    runStatistics(expectations, device, cache);
    runStatisticsRoute(expectations);
    runAudioPlots(expectations, device, cache);
    runMotionBlur(expectations, device, cache);
    runFrameRateHold(expectations, device, cache);
    runSharpens(expectations, device, cache);
#if defined(__linux__) || defined(__APPLE__)
    runFileTransform(expectations, device, cache, from, to);
#endif
}
#endif // BLOOM_GPUSHADER_TOOLS_DIR

} // namespace

int main(int argc, char** argv) {
    try {
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "invalid arguments\n";
            return 2;
        }
        if (options.require_device) {
            std::cerr << "FAIL: --require-device requested but BLOOM_GPUSHADER_TOOLS_DIR is not "
                         "set\n";
            return 1;
        }
        std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
        return 77;
#else
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "invalid arguments\n";
            return 2;
        }
        const auto config = bloom::runtime::detail::resolveInputColorConfig(
            bloom::runtime::EvaluationColorIntent::LinearRec709Scene);
        if (!config.has_value()) {
            std::cout << "SKIP: the Bloom Neutral OCIO config is unavailable\n";
            return 77;
        }
        Expectations expectations;
        GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        loaderPath() = options.loader_path;
        auto device = GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return 77;
        }
        auto cache = GpuSceneCache::create(*device.device, GpuSceneCacheBudgets{});
        expectations.expect(cache.hasValue(), "effect native: the scene cache creates");
        if (!cache) {
            return 1;
        }
        runNative(expectations, *device.device, *cache.cache,
                  std::string{config->processColorSpaceId()},
                  std::string{config->sRgbTextureColorSpaceId()});
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " effect native expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: GPU image-effect executor vs CPU oracle\n";
        return 0;
#endif
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
