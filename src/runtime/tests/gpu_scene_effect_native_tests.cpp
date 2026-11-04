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

#include "gpu_composite_native_support.hpp"

#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/input_color_context.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
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
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
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
           const Color4d color = Color4d{-0.25, 1.75, 0.5, 0.625}) {
    constexpr std::uint64_t layerRaw = 1;
    std::vector<CompiledOperation> operations;
    operations.emplace_back(CompiledSolid{NodeId::fromRaw(idBase + 1),
                                          {ParameterId::fromRaw(idBase + 2), color},
                                          {ParameterId::fromRaw(idBase + 3), 12.0},
                                          {ParameterId::fromRaw(idBase + 4), 10.0}});
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

// RGB within the strict 2e-6 abs-or-rel gate; alpha byte-exact.
[[nodiscard]] bool parity(const std::vector<Rgba32f>& actual,
                          const std::span<const Rgba32f> expected, std::string& detail) {
    if (actual.size() != expected.size()) {
        detail = "size";
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i].alpha() != expected[i].alpha()) {
            detail = "alpha at " + std::to_string(i);
            return false;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            const float a = actual[i].components()[c];
            const float e = expected[i].components()[c];
            if (!std::isfinite(a) || !std::isfinite(e)) {
                detail = "non-finite at " + std::to_string(i);
                return false;
            }
            const float scale = std::max(std::abs(a), std::abs(e));
            if (std::abs(a - e) > 2e-6F * std::max(1.0F, scale)) {
                detail = "rgb c" + std::to_string(c) + " at " + std::to_string(i) + " got " +
                         std::to_string(a) + " want " + std::to_string(e);
                return false;
            }
        }
    }
    return true;
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

#if defined(__linux__)
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
        shape.lineStart = {0, 0};
        shape.lineEnd = {6, 3};
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

void runResizeFit(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache) {
    const CpuGpuSceneBuilder builder;
    const CpuCompositionEvaluator evaluator;
    for (const bloom::runtime::ResizeFitKernel kernel :
         {bloom::runtime::ResizeFitKernel{8, 8}, bloom::runtime::ResizeFitKernel{8, 8, 1, 1, 0, 1},
          bloom::runtime::ResizeFitKernel{8, 8, 1, 1, 0, 2},
          bloom::runtime::ResizeFitKernel{16, 12, 2, 1, 0, 1}}) {
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
    };
    const std::array cases{
        Case{{1.5, -0.75}, {1, 1}, 0.0, {}, Transform2DExtent::Transformed},
        Case{{0.25, 0.5}, {1.5, 0.5}, 30.0, {1, 0.5}, Transform2DExtent::Transformed},
        Case{{2, 1}, {-1, 1}, 90.0, {}, Transform2DExtent::Transformed},
        Case{{0, 0}, {1.25, 1.25}, 20.0, {}, Transform2DExtent::Display},
        Case{{3, 0.5}, {1, 1}, -12.5, {}, Transform2DExtent::Input},
        Case{{0, 0}, {0, 1}, 0.0, {}, Transform2DExtent::Transformed}};
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
                    item.extent};
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
                    shape.lineStart = {0, 0};
                    shape.lineEnd = {6, 3};
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
                    expectations.expect(changed && changedOracle.frame(),
                                        "blur native: changed radius prepares");
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

// #340 Levels, Curves and Hue/Saturation of signed/HDR/partial-alpha solids against the CPU oracle
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
        CurvesKernel kernel;
        std::uint64_t next = id;
        for (auto& channel : kernel.points)
            for (std::size_t point = 0; point < channel.size(); ++point) {
                const double x = static_cast<double>(point) / 4.0;
                channel[point] = {ParameterId::fromRaw(next++), bloom::document::Vec2d{x, x}};
            }
        kernel.points[0][1].source = bloom::document::Vec2d{0.25, 0.15};
        kernel.points[0][3].source = bloom::document::Vec2d{0.75, 0.85};
        kernel.points[1][2].source = bloom::document::Vec2d{0.4, 0.7};
        kernel.points[3][4].source = bloom::document::Vec2d{1.0, 1.75};
        kernel.points[4][0].source = bloom::document::Vec2d{0.0, 0.2};
        kernel.clamp = clamp;
        return bloom::runtime::ImageEffectKernel{kernel};
    };
    const auto hue = [](const std::uint64_t id, const bool oversaturate) {
        return bloom::runtime::ImageEffectKernel{
            HueSaturationKernel{{ParameterId::fromRaw(id), oversaturate ? 200.0 : -70.0},
                                {ParameterId::fromRaw(id + 1), oversaturate ? 2.5 : 0.3}}};
    };
    // Changes one value of each kernel to prove invalidation re-dispatches.
    const auto change = [](bloom::runtime::ImageEffectKernel& kernel) {
        if (auto* l = std::get_if<LevelsKernel>(&kernel))
            l->channels[0].outputWhite.source = 1.5;
        if (auto* c = std::get_if<CurvesKernel>(&kernel))
            c->points[0][2].source = bloom::document::Vec2d{0.5, 0.65};
        if (auto* h = std::get_if<HueSaturationKernel>(&kernel))
            h->hue.source = 15.0;
    };
    std::uint64_t id = 9500;
    for (const auto color : {Color4d{-0.25, 1.75, 0.5, 0.625}, Color4d{4.0, 3.0, 6.0, 1.0},
                             Color4d{0.3, 0.6, 0.2, 0.25}}) {
        for (const auto& kernel :
             {levels(id + 50, false), levels(id + 50, true), curves(id + 50, false),
              curves(id + 50, true), hue(id + 50, false), hue(id + 50, true)}) {
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

void runNative(Expectations& expectations, GpuDevice& device, GpuSceneCache& cache,
               const std::string& from, const std::string& to) {
    runCst(expectations, device, cache, from, to);
    runPointwise(expectations, device, cache);
    runPointwiseExtremeAlpha(expectations, device, cache);
    runCanvas(expectations, device, cache);
    runDiscrete(expectations, device, cache);
    runResizeFit(expectations, device, cache);
    runTransform2D(expectations, device, cache);
    runBlurs(expectations, device, cache);
    runLumaKeys(expectations, device, cache);
    runColorGrades(expectations, device, cache);
#if defined(__linux__)
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
