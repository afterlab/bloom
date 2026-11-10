// CPU-CI gate for the bounded GPU coverage contract. It drives the REAL production
// CpuGpuSceneBuilder over one genuine fixture per Required operation/feature/route/node-type, so a
// pixel operation that only reaches Unsupported is reported by name as a hole. A requirement with
// no genuine fixture is a missing-fixture failure, never a pass.
//
// This is a strict acceptance gate, not an expected-RED report. On the CPU path every Required
// operation/effect/feature/node-type id must have a fixture the production builder genuinely
// prepares. On the native path (`--native-acceptance`, a distinct CTest) every such fixture must
// genuinely execute on a real device and match the unchanged CPU oracle, and every Required render
// route must carry a fresh, run-nonce-matched genuine proof from its owning harness. Without a
// device the native CTest SKIPs (exit 77); `--require-device` makes an unavailable device a hard
// failure. The gate never passes a requirement it did not earn and never adds an opt-out to turn a
// hole green. "Green" means every Required id was genuinely prepared or genuinely executed on the
// hardware present in this run; it is not a claim that every platform or GPU has been qualified.
//
// A fixture may declare an explicit PackagedShaderTools prerequisite. When this build stages the
// packaged tools the fixture runs normally and a broken resolver is a hard failure; when the build
// deliberately packages no tools (CPU stub, unqualified mode, missing prefix) the fixture is an
// explicit NotRun and the CPU gate returns CTest 77, but only after every structural and
// independent check has passed. A missing required fixture is a structural failure that the
// prerequisite NotRun can never absorb, and no independent fixture is ever masked by tools.

#include "compositing_recipe_test_support.hpp"
#include "gpu_coverage_contract_plans.hpp"
#include "gpu_coverage_display_support.hpp"
#include "gpu_coverage_fixture_support.hpp"
#include "gpu_coverage_gate_support.hpp"
#include "gpu_coverage_media_support.hpp"
#include "gpu_coverage_mutation_support.hpp"
#include "gpu_coverage_native_acceptance.hpp"
#include "gpu_coverage_native_support.hpp"
#include "gpu_coverage_ocio_support.hpp"
#include "gpu_coverage_route_proof_support.hpp"
#include "gpu_coverage_video_support.hpp"
#include "gpu_coverage_working_space_support.hpp"
#include "gpu_route_proof_io.hpp"
#include "depth_effect_test_support.hpp"
#include "lens_flare_test_support.hpp"
#include "codec_degrade_test_support.hpp"
#include "denoise_test_support.hpp"
#include "auto_exposure_test_support.hpp"
#include "audio_plot_test_support.hpp"
#include "histogram_test_support.hpp"
#include "statistics_test_support.hpp"
#include "pixel_math_test_support.hpp"
#include "color_tool_test_support.hpp"
#include "film_grain_test_support.hpp"
#include "motion_blur_test_support.hpp"
#include "sharpen_test_support.hpp"
#include "field_test_programs.hpp"
#include "grade_curves_test_support.hpp"
#include "merge_entry_test_support.hpp"

#include <bloom/document/field_parameters.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/runtime/gpu_coverage_contract.hpp>
#include <bloom/runtime/gpu_coverage_route_proof_contract.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/input_color_context.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using bloom::document::NodeId;
using bloom::document::ParameterId;
using bloom::document::ShapeKind;
using bloom::document::Vec2d;
using bloom::gpu_coverage_fixtures::codeName;
using bloom::gpu_coverage_fixtures::format;
using bloom::gpu_coverage_fixtures::LayerValues;
using bloom::gpu_coverage_fixtures::publish;
using bloom::gpu_coverage_plans::imageCombinePlan;
using bloom::gpu_coverage_plans::matteCombinePlan;
using bloom::gpu_coverage_fixtures::requestFor;
using bloom::gpu_coverage_fixtures::twoLayerPlan;
using bloom::runtime::CompiledColorParameter;
using bloom::runtime::CompiledChecker;
using bloom::runtime::CompiledCompositionSource;
using bloom::runtime::CompiledImageEffect;
using bloom::runtime::CompiledImageSource;
using bloom::runtime::CompiledOperation;
using bloom::runtime::CompiledScalarParameter;
using bloom::runtime::CompiledShape;
using bloom::runtime::CompiledText;
using bloom::runtime::CompiledTextLayout;
using bloom::runtime::CompiledVec2Parameter;
using bloom::runtime::CompiledVideoSource;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::GpuCoverageFixtureCriterion;
using bloom::runtime::ImageEffectKernel;
using bloom::runtime::OperationIndex;
using bloom::runtime::PointwiseImageEffect;
using bloom::runtime::PointwiseImageKernel;
using bloom::runtime::PreparedGpuSceneDiagnosticCode;

using bloom::gpu_coverage_gate::classifyFixtureExecution;
using bloom::gpu_coverage_gate::Fixture;
using bloom::gpu_coverage_gate::FixtureExecution;
using bloom::gpu_coverage_gate::FixturePrerequisite;
using bloom::gpu_coverage_gate::FixtureRun;
using bloom::gpu_coverage_gate::FrameRun;
using bloom::gpu_coverage_gate::PrerequisiteAvailability;
using bloom::gpu_coverage_gate::prerequisiteDecisionRegression;
using bloom::gpu_coverage_gate::requiredCoverageIds;
using bloom::gpu_coverage_gate::routeOwner;
using bloom::gpu_coverage_gate::structuralFixtureFailures;
using bloom::gpu_coverage_plans::AffineAxis;
using bloom::gpu_coverage_plans::basePlan;
using bloom::gpu_coverage_plans::effectPlan;
using bloom::gpu_coverage_plans::gradient;
using bloom::gpu_coverage_plans::layerOnLayerPlan;
using bloom::gpu_coverage_plans::modifiedLayer;
using bloom::gpu_coverage_plans::nestedBranchReusePlans;
using bloom::gpu_coverage_plans::nestedChildPlan;
using bloom::gpu_coverage_plans::nestedParentPlan;
using bloom::gpu_coverage_plans::parentedPlan;
using bloom::gpu_coverage_plans::rasterAffinePlan;
using bloom::gpu_coverage_plans::shape;
using bloom::gpu_coverage_plans::text;
using bloom::gpu_coverage_plans::withSource;

[[nodiscard]] FixtureRun
runBuilder(const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan) {
    const CpuGpuSceneBuilder builder(nullptr, {}, bloom::gpu_coverage_ocio::context());
    const auto request = requestFor(*plan);
    FixtureRun result;
    const auto prepared = builder.build(plan, request);
    if (prepared) {
        result.prepared = true;
        result.frames.push_back(FrameRun{plan, request, {}, prepared.scene});
        result.evidence =
            "prepared " + std::to_string(prepared.scene->commands().size()) + " commands";
        return result;
    }
    result.evidence = codeName(prepared.diagnostic.code) + ": " + prepared.diagnostic.message;
    return result;
}

// Runs the production builder with an explicit request (for the ROI feature axis, whose request
// carries a region of interest). The genuine prepared scene is retained for the native acceptance
// pass, which evaluates the CPU oracle with the same ROI and checks the native descriptor.
[[nodiscard]] FixtureRun
runBuilderWithRequest(const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
                      const bloom::runtime::EvaluationRequest& request) {
    const CpuGpuSceneBuilder builder(nullptr, {}, bloom::gpu_coverage_ocio::context());
    FixtureRun result;
    const auto prepared = builder.build(plan, request);
    if (prepared) {
        result.prepared = true;
        result.frames.push_back(FrameRun{plan, request, {}, prepared.scene});
        result.evidence =
            "prepared " + std::to_string(prepared.scene->commands().size()) + " commands";
        return result;
    }
    result.evidence = codeName(prepared.diagnostic.code) + ": " + prepared.diagnostic.message;
    return result;
}

[[nodiscard]] FixtureRun
runMediaBuilder(const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
                const bloom::runtime::CpuCompositionEvaluator& evaluator,
                const std::filesystem::path& baseDirectory) {
    auto context = bloom::runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
    context.assetBaseDirectory = baseDirectory;
    const CpuGpuSceneBuilder builder(nullptr, context, bloom::gpu_coverage_ocio::context());
    const auto request = requestFor(*plan);
    FixtureRun result;
    const auto prepared = builder.build(plan, request);
    if (prepared) {
        result.prepared = true;
        result.frames.push_back(FrameRun{plan, request, baseDirectory, prepared.scene});
        result.evidence =
            "prepared media " + std::to_string(prepared.scene->commands().size()) + " commands";
        return result;
    }
    result.evidence = codeName(prepared.diagnostic.code) + ": " + prepared.diagnostic.message;
    return result;
}

// Two genuine video frames at distinct composition times, sharing the probed asset/interpretation.
[[nodiscard]] FixtureRun runVideoFixture(const bloom::gpu_coverage_video::VideoFixture& fixture,
                                         const std::uint64_t idBase) {
    FixtureRun result;
    for (std::int64_t frame = 0; frame < 2; ++frame) {
        const bloom::runtime::CpuCompositionEvaluator evaluator;
        auto plan = bloom::gpu_coverage_video::videoPlan(
            fixture.asset, LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 0,
            idBase + static_cast<std::uint64_t>(frame) * 100);
        auto request = requestFor(*plan);
        request.time = bloom::core::RationalTime::fromInteger(frame);
        auto context = bloom::runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
        context.assetBaseDirectory = bloom::gpu_coverage_video::mediaFixturesDirectory();
        const CpuGpuSceneBuilder builder(nullptr, context, bloom::gpu_coverage_ocio::context());
        const auto prepared = builder.build(plan, request);
        if (!prepared) {
            result.prepared = false;
            result.evidence = "frame " + std::to_string(frame) + ": " +
                              codeName(prepared.diagnostic.code) + ": " +
                              prepared.diagnostic.message;
            return result;
        }
        result.frames.push_back(FrameRun{
            plan, request, bloom::gpu_coverage_video::mediaFixturesDirectory(), prepared.scene});
    }
    result.prepared = true;
    result.evidence = "prepared video 2 frames";
    return result;
}

// A genuine non-identity CST effect: the resolved Bloom Neutral process space -> its sRGB texture
// space. The shared OCIO context compiles the real ProcessEffect command, so the native gate
// dispatches an OCIO kernel instead of the identity alias a same-from/to CST would produce.
[[nodiscard]] FixtureRun runCstEffectFixture(const std::uint64_t idBase) {
    const auto config = bloom::runtime::detail::resolveInputColorConfig(
        bloom::runtime::EvaluationColorIntent::LinearRec709Scene);
    if (!config.has_value()) {
        FixtureRun result;
        result.evidence = "the Bloom Neutral OCIO config is unavailable";
        return result;
    }
    return runBuilder(
        effectPlan(bloom::runtime::CstKernel{std::string{config->processColorSpaceId()},
                                             std::string{config->sRgbTextureColorSpaceId()}},
                   idBase));
}

// A genuine non-identity FileTransform: a real 1D .cube read back through color::readLutFile. The
// builder emits the accepted LUT chain and the CPU oracle consumes the identical digest.
[[nodiscard]] FixtureRun runFileTransformFixture(const std::uint64_t idBase) {
    const bloom::runtime::CpuCompositionEvaluator evaluator;
    const auto& lut = bloom::gpu_coverage_media::syntheticLut();
    const bloom::runtime::FileTransformKernel kernel{lut.asset.id, 0, 0, "lin_rec709_scene",
                                                     lut.asset};
    return runMediaBuilder(effectPlan(kernel, idBase), evaluator,
                           bloom::gpu_coverage_media::gateDirectory());
}

// The working-space colour transform axis: a mixed solid/text/media/effect ACEScg composition. It
// uses the production media context (so the real EXR is decoded) plus the shared OCIO context (so
// the CST legs and the media input transform are genuine GPU ProcessEffects). The request carries
// the exact ACES built-in identity, and the native gate compares the GPU result to the unchanged
// CPU evaluator at 2e-6.
[[nodiscard]] FixtureRun runWorkingSpaceFixture() {
    const bloom::runtime::CpuCompositionEvaluator evaluator;
    const auto asset = bloom::gpu_coverage_working_space::textureInputAsset();
    const auto plan = bloom::gpu_coverage_working_space::mixedPlan(
        format(8, 8), asset, bloom::core::Color4d{-0.2, 1.6, 0.35, 0.5}, 130000);
    auto context = bloom::runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
    context.assetBaseDirectory = bloom::gpu_coverage_media::gateDirectory();
    const CpuGpuSceneBuilder builder(nullptr, context, bloom::gpu_coverage_ocio::context());
    const auto request = bloom::gpu_coverage_working_space::acesRequest(*plan);
    FixtureRun result;
    const auto prepared = builder.build(plan, request);
    if (prepared) {
        result.prepared = true;
        result.frames.push_back(
            FrameRun{plan, request, bloom::gpu_coverage_media::gateDirectory(), prepared.scene});
        result.evidence = "prepared working-space " +
                          std::to_string(prepared.scene->commands().size()) + " commands";
        return result;
    }
    result.evidence = codeName(prepared.diagnostic.code) + ": " + prepared.diagnostic.message;
    return result;
}

// A native-only display proof runner: the display support function reports its own genuine
// evidence.
[[nodiscard]] bloom::gpu_coverage_gate::NativeProofRunner
displayProofRunner(const bool customView) {
    return [customView](bloom::render::GpuDevice& device,
                        const bloom::runtime::GpuSceneOcioContext& ocioContext,
                        std::string& evidence) {
        const auto outcome =
            bloom::gpu_coverage_display::runDisplayProof(device, ocioContext, customView);
        evidence = outcome.evidence;
        return outcome.passed;
    };
}

[[nodiscard]] std::vector<Fixture> fixtures() {
    std::vector<Fixture> list;
    const auto add = [&list](std::string id, GpuCoverageFixtureCriterion criterion,
                             std::string owner, std::function<FixtureRun()> run,
                             bloom::gpu_coverage_gate::NativeProofRunner nativeProof = {},
                             FixturePrerequisite prerequisite = FixturePrerequisite::None) {
        list.push_back(Fixture{std::move(id), criterion, std::move(owner), std::move(run),
                               std::move(nativeProof), prerequisite});
    };
    const auto addImageEffectPlan = [&add](std::string id, const ImageEffectKernel& kernel,
                                           const std::uint64_t idBase, std::string owner) {
        if (owner.empty()) {
            owner = "src/color OCIO image effect tests";
        }
        add(std::move(id), GpuCoverageFixtureCriterion::Prepared, std::move(owner),
            [kernel, idBase] { return runBuilder(effectPlan(kernel, idBase)); });
    };

    for (const std::string_view id : {"operation.CompiledSolid", "operation.CompiledMerge",
                                      "operation.CompiledCompositionOutput"}) {
        add(std::string{id}, GpuCoverageFixtureCriterion::Prepared,
            "src/runtime gpu scene preparation tests", [] { return runBuilder(basePlan()); });
    }
    // #502: a Merge entry's own layer is a CompiledLayerOutput with a frame pivot; the second
    // variant pivots both layers about the frame centre (off-centre content, scaled and rotated).
    add("operation.CompiledLayerOutput", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime gpu scene preparation tests + merge entry layer tests", [] {
            FixtureRun combined;
            combined.prepared = true;
            auto definition = basePlan()->copyDefinition();
            for (auto& operation : definition.operations)
                if (auto* layer = std::get_if<bloom::runtime::CompiledLayerOutput>(&operation)) {
                    layer->framePivot = true;
                    layer->scale = CompiledVec2Parameter{layer->scale.id, Vec2d{1.25, 0.75}};
                    layer->rotation = CompiledScalarParameter{layer->rotation.id, 20.0};
                }
            for (const auto& plan : {basePlan(), publish(std::move(definition))}) {
                auto frame = runBuilder(plan);
                combined.prepared = combined.prepared && frame.prepared;
                combined.evidence += frame.evidence + "; ";
                for (auto& value : frame.frames)
                    combined.frames.push_back(std::move(value));
            }
            // #545: translucent footage under non-dyadic scales, a rotation and an opacity: sparse
            // highlights on transparent black at a full-frame-sized composition, and partial
            // alpha everywhere. Their near-zero and partial alphas resample through the
            // double-float coordinate rule on both routes, so the gate's exact alpha holds without
            // dyadic coefficients.
            const bloom::runtime::CpuCompositionEvaluator evaluator;
            const auto translucentLayer = [&](const bloom::gpu_coverage_media::ImageFixture& image,
                                              const LayerValues values,
                                              const std::uint64_t idBase, const std::uint32_t width,
                                              const std::uint32_t height) {
                auto definition =
                    bloom::gpu_coverage_media::imageSourcePlan(image.asset, values, idBase)
                        ->copyDefinition();
                definition.format = format(width, height);
                auto frame = runMediaBuilder(publish(std::move(definition)), evaluator,
                                             bloom::gpu_coverage_media::gateDirectory());
                combined.prepared = combined.prepared && frame.prepared;
                combined.evidence += frame.evidence + "; ";
                for (auto& value : frame.frames)
                    combined.frames.push_back(std::move(value));
            };
            const auto& highlights = bloom::gpu_coverage_media::sparseHighlightsExrImage();
            translucentLayer(highlights, LayerValues{.position = {30.5, 18.5}, .scale = {0.8, 0.8}},
                             152000, 61, 37);
            translucentLayer(highlights,
                             LayerValues{.position = {29.3, 17.9}, .scale = {0.7, 0.9}}, 152100,
                             61, 37);
            translucentLayer(bloom::gpu_coverage_media::translucentExrImage(),
                             LayerValues{.position = {3.9, 4.2},
                                         .anchor = {0.35, -0.6},
                                         .scale = {0.83, 1.17},
                                         .rotation = 17.0,
                                         .opacity = 0.6},
                             152200, 8, 8);
            return combined;
        });
    const auto checkerPlan = [] {
        return withSource(
            CompiledChecker{NodeId::fromRaw(94700),
                            CompiledColorParameter{ParameterId::fromRaw(94701),
                                                   bloom::core::Color4d{-2.0, 0.5, 4.0, 0.5}},
                            CompiledColorParameter{ParameterId::fromRaw(94702),
                                                   bloom::core::Color4d{3.0, -1.0, 0.25, 0.75}},
                            3, 2, 6, 5},
            94700);
    };
    add("operation.CompiledChecker", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime Checker CPU and native parity tests", [checkerPlan] {
            return runBuilder(checkerPlan());
        });
    add("operation.CompiledText", GpuCoverageFixtureCriterion::Prepared,
        "src/render text_raster + text source tests",
        [] { return runBuilder(withSource(text(95000), 95000, nullptr, 0.6)); });
    add("operation.CompiledShape", GpuCoverageFixtureCriterion::Prepared,
        "src/render PathRaster + shape source tests", [] {
            return runBuilder(
                withSource(shape(ShapeKind::Rectangle, 96000, true), 96000, nullptr, 0.7));
        });
    add("operation.CompiledCompositionSource", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime gpu nested scene preparation tests", [] {
            const auto child =
                nestedChildPlan(120000, 201, bloom::core::Color4d{0.125, 0.375, 0.75, 0.5});
            return runBuilder(nestedParentPlan(child, 121000, 100));
        });
    addImageEffectPlan("operation.CompiledImageEffect", bloom::runtime::IdentityImageKernel{},
                       98000, "src/color OCIO image effect tests");
    // Real still-image fixture: a synthetic signed/HDR EXR resolved through the production media
    // pipeline. VideoSource has no genuine fixture in this gate yet, so it stays explicitly absent
    // (missing-fixture RED) rather than counting InvalidPlan/MediaUnavailable as a GPU pass.
    add("operation.CompiledImageSource", GpuCoverageFixtureCriterion::Prepared,
        "src/media image source tests", [] {
            const bloom::runtime::CpuCompositionEvaluator evaluator;
            const auto& image = bloom::gpu_coverage_media::syntheticImage();
            const auto plan = bloom::gpu_coverage_media::imageSourcePlan(
                image.asset, LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 99000);
            return runMediaBuilder(plan, evaluator, bloom::gpu_coverage_media::gateDirectory());
        });
    add(
        "operation.CompiledVideoSource", GpuCoverageFixtureCriterion::Prepared,
        "src/media video source tests",
        [] {
            try {
                return runVideoFixture(bloom::gpu_coverage_video::probeVideoFixture(), 111000);
            } catch (const std::exception& error) {
                FixtureRun result;
                result.evidence = error.what();
                return result;
            }
        },
        {}, FixturePrerequisite::MediaFixtures);

    addImageEffectPlan("effect.IdentityImageKernel", bloom::runtime::IdentityImageKernel{}, 100000,
                       {});
    addImageEffectPlan("effect.ResizeFitKernel", bloom::runtime::ResizeFitKernel{10, 7}, 103000,
                       "src/runtime Resize/Fit production affine preparation");
    // #336 Transform 2D: a genuine quarter-turned, nonuniformly scaled, sub-pixel translated
    // resample through the production resident affine preparation (never the identity alias or
    // the translation pass). This gate requires bit-exact alpha everywhere; since #545 the CPU
    // reference shares the kernel's double-float coordinate rule, so any coefficients meet it.
    const auto transform2D = [](const std::uint64_t base, const bloom::document::Vec2d translate,
                                const bloom::document::Vec2d scale, const double rotation,
                                const bloom::runtime::Transform2DExtent extent) {
        return bloom::runtime::Transform2DKernel{
            CompiledVec2Parameter{ParameterId::fromRaw(base + 1), translate},
            CompiledVec2Parameter{ParameterId::fromRaw(base + 2), bloom::document::Vec2d{0.5, 0.25}},
            CompiledVec2Parameter{ParameterId::fromRaw(base + 3), scale},
            CompiledScalarParameter{ParameterId::fromRaw(base + 4), rotation}, extent};
    };
    addImageEffectPlan("effect.Transform2DKernel",
                       transform2D(150000, {1.25, -0.5}, {2.0, 0.5}, 90.0,
                                   bloom::runtime::Transform2DExtent::Transformed),
                       150000, "src/runtime Transform 2D production affine preparation");
    addImageEffectPlan("node.bloom.transform-2d",
                       transform2D(151000, {-0.75, 1.5}, {0.5, 2.0}, -90.0,
                                   bloom::runtime::Transform2DExtent::Display),
                       151000, "src/runtime Transform 2D production affine preparation");
    // #512 Camera Shake lowers to this same Transform 2D (the shake's translation, roll and
    // overscan scale arrive as resolved values): a small rolled, overscanned, sub-pixel move.
    addImageEffectPlan("node.bloom.camera-shake",
                       transform2D(158000, {3.25, -1.75}, {1.03, 1.03}, 0.75,
                                   bloom::runtime::Transform2DExtent::Input),
                       158000, "src/runtime Transform 2D production affine preparation");
    add(
        "effect.CstKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/color OCIO image effect tests", [] { return runCstEffectFixture(101000); }, {},
        FixturePrerequisite::PackagedShaderTools);
    add(
        "effect.FileTransformKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/color OCIO image effect tests", [] { return runFileTransformFixture(102000); }, {},
        FixturePrerequisite::PackagedShaderTools);
    const auto pointwise = [](const PointwiseImageEffect kind, const double value,
                              const std::uint64_t idBase) {
        return PointwiseImageKernel{
            kind, CompiledScalarParameter{ParameterId::fromRaw(idBase + 1), value}};
    };
    add("effect.PointwiseImageKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime pointwise image effect tests", [pointwise] {
            return runBuilder(effectPlan(pointwise(PointwiseImageEffect::Exposure, 1.25, 103100),
                                         103100));
        }, {}, FixturePrerequisite::PackagedShaderTools);
    add("effect.ImageCombineKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime image combine production scene preparation", [] {
            return runBuilder(imageCombinePlan(bloom::runtime::ImageCombineMode::Mix, 103200));
        });
    const auto canvas = [](const bloom::runtime::CanvasImageEffect kind,
                           const std::uint64_t base) {
        std::array<CompiledScalarParameter, 4> margins{
            CompiledScalarParameter{ParameterId::fromRaw(base + 1), 1.0},
            CompiledScalarParameter{ParameterId::fromRaw(base + 2), 0.0},
            CompiledScalarParameter{ParameterId::fromRaw(base + 3), 1.0},
            CompiledScalarParameter{ParameterId::fromRaw(base + 4), 0.0}};
        return bloom::runtime::CanvasImageKernel{kind, margins};
    };
    add("effect.CanvasImageKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime canvas image effect tests", [canvas] {
            return runBuilder(effectPlan(canvas(bloom::runtime::CanvasImageEffect::Crop, 104100),
                                         104100));
        });

    addImageEffectPlan("effect.DiscreteImageKernel",
                       bloom::runtime::DiscreteImageKernel{false, false, 1}, 103200,
                       "src/runtime exact discrete image tests");
    addImageEffectPlan("node.bloom.flip", bloom::runtime::DiscreteImageKernel{true, false, 0},
                       103300, "src/runtime exact discrete image tests");
    addImageEffectPlan("node.bloom.rotate-90", bloom::runtime::DiscreteImageKernel{false, false, 1},
                       103400, "src/runtime exact discrete image tests");

    // Every one of the eight blend modes is emitted by the real production builder now: Normal
    // stays the retained SourceOver merge, every other mode an explicit BlendV1 fold.
    for (const auto mode : bloom::core::kBlendModes) {
        const auto id =
            std::string{"feature.blend."} + std::to_string(bloom::core::blendModeStoredValue(mode));
        add(id, GpuCoverageFixtureCriterion::Prepared,
            "src/render GpuComposite + gpu scene blend integration tests", [mode] {
                return runBuilder(modifiedLayer([mode](bloom::runtime::CompiledLayerOutput& layer) {
                    layer.blendMode = mode;
                }));
            });
    }
    constexpr std::array<ShapeKind, 7> kShapeKinds{
        ShapeKind::Rectangle, ShapeKind::Ellipse, ShapeKind::Triangle, ShapeKind::Polygon,
        ShapeKind::Star,      ShapeKind::Line,    ShapeKind::Path};
    for (const auto kind : kShapeKinds) {
        const auto id =
            std::string{"feature.shape."} + std::to_string(static_cast<std::int64_t>(kind));
        add(id, GpuCoverageFixtureCriterion::Prepared, "src/render PathRaster + shape source tests",
            [kind] { return runBuilder(withSource(shape(kind, 103000), 103000)); });
    }
    // The native vector-coverage axis: the production builder emits immutable PathRaster coverage
    // geometry (never a host mask) and the native gate must observe a real GpuPathCoverage compute
    // dispatch for it, cold and zero warm. A fill+stroke shape exercises both coverage passes.
    add("feature.geometry.vector_coverage", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime GpuSceneExecutor native coverage tests",
        [] { return runBuilder(withSource(shape(ShapeKind::Ellipse, 107500, true), 107500)); });
    // #589: the compound value and the driven value reach the same production coverage producer.
    add("feature.path.compound", GpuCoverageFixtureCriterion::Prepared,
        "tests/integration gpu path value native tests", [] {
            auto source = shape(ShapeKind::Path, 107600, true);
            source.path = bloom::gpu_coverage_plans::compoundPath();
            return runBuilder(withSource(source, 107600));
        });
    add("feature.path.driven", GpuCoverageFixtureCriterion::Prepared,
        "tests/integration gpu path value native tests", [] {
            auto source = shape(ShapeKind::Path, 107700, true);
            // The compiler keeps no path on a driven shape; the frame's value is its only source.
            source.path = bloom::document::PathValue{};
            source.drivenPath = bloom::runtime::ValueOutputIndex::fromRaw(0);
            auto definition = withSource(source, 107700)->copyDefinition();
            definition.valueOperations = {bloom::runtime::CompiledValueOperation{
                NodeId::fromRaw(107710), bloom::runtime::ValueOutputIndex::fromRaw(0), 1,
                bloom::runtime::CompiledValuePathCurve{bloom::runtime::CompiledPathCurve{
                    bloom::document::AnimationCurveId::fromRaw(107711),
                    {bloom::runtime::CompiledPathKeyframe{
                         bloom::document::KeyframeId::fromRaw(107712), bloom::core::RationalTime{},
                         bloom::gpu_coverage_plans::compoundPath()},
                     bloom::runtime::CompiledPathKeyframe{
                         bloom::document::KeyframeId::fromRaw(107713),
                         bloom::core::RationalTime::fromInteger(1),
                         bloom::gpu_coverage_plans::compoundPath(3.0)}}}}}};
            definition.valueOutputCount = 1;
            return runBuilder(publish(std::move(definition)));
        });
    // #599: both ends of a Line read keyed curves, resolved per frame into the same coverage producer.
    add("feature.shape.line.animated", GpuCoverageFixtureCriterion::Prepared,
        "tests/integration gpu shape line ends native tests", [] {
            auto source = shape(ShapeKind::Line, 107800);
            const auto key = [](const std::uint64_t id, const std::int64_t frame,
                                const double value) {
                return bloom::runtime::CompiledScalarKeyframe{
                    bloom::document::KeyframeId::fromRaw(id),
                    *bloom::core::RationalTime::create(frame, 24), value,
                    bloom::runtime::CompiledKeyframeInterpolation::Linear};
            };
            const auto curve = [&](const std::uint64_t id, const double from, const double to,
                                   const double otherFrom, const double otherTo) {
                bloom::runtime::CompiledVec2Curve out;
                out.id = bloom::document::AnimationCurveId::fromRaw(id);
                out.components[0] = {key(id + 1, 0, from), key(id + 2, 24, to)};
                out.components[1] = {key(id + 3, 0, otherFrom), key(id + 4, 24, otherTo)};
                return out;
            };
            source.lineStart = bloom::runtime::CompiledVec2Parameter{
                source.lineStart.id, bloom::runtime::Vec2CurveIndex::fromRaw(0)};
            source.lineEnd = bloom::runtime::CompiledVec2Parameter{
                source.lineEnd.id, bloom::runtime::Vec2CurveIndex::fromRaw(1)};
            auto definition = withSource(source, 107800)->copyDefinition();
            definition.vec2Curves = {curve(107810, 0.0, 1.0, 0.0, 1.0),
                                     curve(107820, 6.0, 2.0, 5.0, 4.0)};
            return runBuilder(publish(std::move(definition)));
        });
    // A layer over a raster merge result is a raster input, so the production builder must emit the
    // accepted GpuAffine command for each transform axis (scale, rotation, anchor).
    add("feature.layer.affine.scale", GpuCoverageFixtureCriterion::Prepared,
        "src/render LayerTransform + gpu scene affine integration tests",
        [] { return runBuilder(rasterAffinePlan(AffineAxis::Scale, 112000)); });
    add("feature.layer.affine.rotation", GpuCoverageFixtureCriterion::Prepared,
        "src/render LayerTransform + gpu scene affine integration tests",
        [] { return runBuilder(rasterAffinePlan(AffineAxis::Rotation, 112100)); });
    add("feature.layer.affine.anchor", GpuCoverageFixtureCriterion::Prepared,
        "src/render LayerTransform + gpu scene affine integration tests",
        [] { return runBuilder(rasterAffinePlan(AffineAxis::Anchor, 112200)); });
    // A real parented layer: layer B composes layer A's matrix (its parent), so the parented shear
    // is prepared through the production builder.
    add("feature.layer.parent", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime layer_parent_transform tests",
        [] { return runBuilder(parentedPlan(112300)); });
    add("feature.layer.generic_input", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime GpuSceneExecutor graph tests",
        [] { return runBuilder(layerOnLayerPlan(104000)); });
    // The request ROI axis: a nonzero-origin region smaller than the composition. The production
    // builder must clip the terminal output to the ROI while keeping every native source/output
    // window, and the native gate must execute the real GPU scene and match the CPU oracle's
    // ROI-sized process image. The dedicated ROI suites own the vector/affine/nested/media proofs.
    add("feature.roi", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime GpuSceneExecutor ROI native tests", [] {
            const auto plan = basePlan();
            const auto window = bloom::render::ImageWindow::create(3, 2, 5, 6);
            if (!window) {
                FixtureRun result;
                result.evidence = "the ROI fixture window is invalid";
                return result;
            }
            auto request = requestFor(*plan);
            request.roi = *window.value();
            return runBuilderWithRequest(plan, request);
        });
    // Working-space colour conversion is a pixel transformation: a mixed solid/text/media/effect
    // ACEScg composition prepared by the production builder with the shared OCIO context, then
    // executed and compared to the unchanged CPU evaluator at 2e-6. The owner is this gate's
    // native working-space proof, never an external test name.
    add(
        "feature.color.working_space_transform", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime GpuSceneBuilder working-space effect native proof",
        [] { return runWorkingSpaceFixture(); }, {}, FixturePrerequisite::PackagedShaderTools);

    // Display features are native-only. Each owns a genuine native proof in this gate: a real OCIO
    // display program compiled with the shared context and dispatched through the production
    // executor, compared to the CPU display oracle at one RGBA8 code with exact alpha. The
    // view-adjust proof uses a non-neutral exposure/gamma; the custom-view proof uses a non-default
    // display/view pair.
    add(
        "feature.display.view_adjust", GpuCoverageFixtureCriterion::NativeRequired,
        "src/runtime GpuSceneExecutor display view-adjust native proof",
        [] { return FixtureRun{}; }, displayProofRunner(false));
    add(
        "feature.display.custom_view_transform", GpuCoverageFixtureCriterion::NativeRequired,
        "src/runtime GpuSceneExecutor custom display/view native proof",
        [] { return FixtureRun{}; }, displayProofRunner(true));

    // One fixture identity per built-in authoring node type that produces pixels. A new node type
    // registered against an existing lowering gets a new `node.<typeId>` requirement and this
    // explicit list cannot satisfy it, so it cannot reuse another node's fixture.
    add("node.bloom.solid-source", GpuCoverageFixtureCriterion::Prepared,
        "src/document node_definition_registry tests", [] { return runBuilder(basePlan()); });
    addImageEffectPlan("node.bloom.resize", bloom::runtime::ResizeFitKernel{10, 7}, 104000,
                       "src/runtime Resize production affine preparation");
    addImageEffectPlan("node.bloom.fit", bloom::runtime::ResizeFitKernel{10, 7, 1, 1, 0, 1}, 105000,
                       "src/runtime Fit production affine preparation");
    // #480: Resize, Fit and Transform 2D Nearest through the production affine command.
    add("feature.resample.nearest", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime Resize/Fit and Transform 2D production affine preparation", [] {
            FixtureRun combined;
            combined.prepared = true;
            const auto nearest = bloom::runtime::kResampleFilterNearest;
            const std::array<ImageEffectKernel, 3> kernels{
                bloom::runtime::ResizeFitKernel{10, 7, 1, 1, nearest},
                bloom::runtime::ResizeFitKernel{10, 7, 1, 1, nearest, 1},
                bloom::runtime::Transform2DKernel{
                    CompiledVec2Parameter{ParameterId::fromRaw(105530), Vec2d{1.25, -0.5}},
                    CompiledVec2Parameter{ParameterId::fromRaw(105531), Vec2d{}},
                    CompiledVec2Parameter{ParameterId::fromRaw(105532), Vec2d{1.5, 0.75}},
                    CompiledScalarParameter{ParameterId::fromRaw(105533), 30.0},
                    bloom::runtime::Transform2DExtent::Transformed, nearest}};
            std::uint64_t idBase = 105500;
            for (const auto& kernel : kernels) {
                auto frame = runBuilder(effectPlan(kernel, idBase));
                idBase += 100;
                combined.prepared = combined.prepared && frame.prepared;
                combined.evidence += frame.evidence + "; ";
                for (auto& value : frame.frames)
                    combined.frames.push_back(std::move(value));
            }
            return combined;
        });
    add("node.bloom.linear-gradient", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime gradient source and native batch tests",
        [] { return runBuilder(withSource(gradient(false, 140000), 140000)); });
    add("node.bloom.radial-gradient", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime gradient source and native batch tests",
        [] { return runBuilder(withSource(gradient(true, 141000), 141000)); });
    add("node.bloom.layer-output", GpuCoverageFixtureCriterion::Prepared,
        "src/document node_definition_registry tests", [] { return runBuilder(basePlan()); });
    add("node.bloom.layer-stack", GpuCoverageFixtureCriterion::Prepared,
        "src/document node_definition_registry tests", [] { return runBuilder(basePlan()); });
    add("node.bloom.composition-output", GpuCoverageFixtureCriterion::Prepared,
        "src/document node_definition_registry tests", [] { return runBuilder(basePlan()); });
    // ADR 0026: Shuffle (Colour), Remove and Channel Copy (Raw data with NaN repair) through the
    // production channel gather, upload, proxy resample and OCIO input pass.
    const auto runChannels = [](const bloom::gpu_coverage_media::ChannelFixture kind,
                                const std::uint64_t idBase) {
        const bloom::runtime::CpuCompositionEvaluator evaluator;
        const auto& image = bloom::gpu_coverage_media::layeredImage();
        const auto plan = bloom::gpu_coverage_media::channelShufflePlan(
            image.asset, LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, idBase, kind);
        return runMediaBuilder(plan, evaluator, bloom::gpu_coverage_media::gateDirectory());
    };
    const auto runAllChannels = [runChannels](const std::uint64_t idBase) {
        using bloom::gpu_coverage_media::ChannelFixture;
        FixtureRun combined;
        combined.prepared = true;
        std::uint64_t offset = 0;
        for (const auto kind :
             {ChannelFixture::Shuffle, ChannelFixture::Remove, ChannelFixture::Copy}) {
            auto frame = runChannels(kind, idBase + offset);
            offset += 100;
            combined.prepared = combined.prepared && frame.prepared;
            combined.evidence += frame.evidence + "; ";
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
        }
        return combined;
    };
    // A Colour Shuffle whose input space differs from an ACEScg working space: the gather is the raw
    // upload and the input->working conversion is a genuine GPU OCIO ProcessEffect.
    const auto runColourChannels = [] {
        const bloom::runtime::CpuCompositionEvaluator evaluator;
        auto asset = bloom::gpu_coverage_media::layeredImage().asset;
        asset.interpretation.inputColorSpaceId =
            std::string{bloom::gpu_coverage_working_space::kAcesTextureInput};
        const auto plan = bloom::gpu_coverage_media::channelShufflePlan(
            asset, LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 113300,
            bloom::gpu_coverage_media::ChannelFixture::Shuffle);
        auto context = bloom::runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
        context.assetBaseDirectory = bloom::gpu_coverage_media::gateDirectory();
        const CpuGpuSceneBuilder builder(nullptr, context, bloom::gpu_coverage_ocio::context());
        const auto request = bloom::gpu_coverage_working_space::acesRequest(*plan);
        FixtureRun result;
        const auto prepared = builder.build(plan, request);
        if (prepared) {
            result.prepared = true;
            result.frames.push_back(FrameRun{plan, request,
                                             bloom::gpu_coverage_media::gateDirectory(),
                                             prepared.scene});
            result.evidence = "prepared color shuffle " +
                              std::to_string(prepared.scene->commands().size()) + " commands";
            return result;
        }
        result.evidence = codeName(prepared.diagnostic.code) + ": " + prepared.diagnostic.message;
        return result;
    };
    add(
        "operation.CompiledChannelShuffle", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime channel shuffle production scene preparation",
        [runAllChannels, runColourChannels] {
            auto combined = runAllChannels(113000);
            auto colour = runColourChannels();
            combined.prepared = combined.prepared && colour.prepared;
            combined.evidence += colour.evidence;
            for (auto& frame : colour.frames)
                combined.frames.push_back(std::move(frame));
            return combined;
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add("node.bloom.channel-shuffle", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime channel shuffle production scene preparation", [runChannels] {
            return runChannels(bloom::gpu_coverage_media::ChannelFixture::Shuffle, 113400);
        });
    add("node.bloom.channel-remove", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime channel shuffle production scene preparation", [runChannels] {
            return runChannels(bloom::gpu_coverage_media::ChannelFixture::Remove, 113500);
        });
    add("node.bloom.channel-copy", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime channel shuffle production scene preparation", [runChannels] {
            return runChannels(bloom::gpu_coverage_media::ChannelFixture::Copy, 113600);
        });
    // #480: a Pass over file channels is the production channel gather with a derived mapping.
    add("node.bloom.channel-pass", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime channel shuffle production scene preparation", [runChannels] {
            return runChannels(bloom::gpu_coverage_media::ChannelFixture::Pass, 168000);
        });
    // #480 Composition Source `channels`: a precomp's file channel layer read through the
    // production channel gather at the mapped nested time (offset, scale and Loop), and the
    // precomp's composite and an Image to Channel Set layer read as Channel Select / Channel
    // Combine over nested Composition Source images -- the operations the compiler emits.
    add("feature.channels.composition_source", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime precomp channel production scene preparation + native document parity", [] {
            using bloom::runtime::CompiledCompositionSource;
            using bloom::runtime::CompiledCompositionTimeMapping;
            FixtureRun combined;
            combined.prepared = true;
            const auto keep = [&](FixtureRun frame) {
                combined.prepared = combined.prepared && frame.prepared;
                combined.evidence += frame.evidence + "; ";
                for (auto& value : frame.frames)
                    combined.frames.push_back(std::move(value));
            };
            const auto child =
                nestedChildPlan(169000, 211, bloom::core::Color4d{0.2, 0.6, 0.9, 0.75});
            const auto mapping = [](const std::uint64_t id, const double offset,
                                    const double scale, const std::int64_t loop) {
                return CompiledCompositionTimeMapping{{ParameterId::fromRaw(id), offset},
                                                      {ParameterId::fromRaw(id + 1), scale},
                                                      loop};
            };
            // File channels: a Pass of the precomp's depth layer at a mapped, looped time.
            {
                const bloom::runtime::CpuCompositionEvaluator evaluator;
                auto definition =
                    bloom::gpu_coverage_media::channelShufflePlan(
                        bloom::gpu_coverage_media::layeredImage().asset,
                        LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 169100,
                        bloom::gpu_coverage_media::ChannelFixture::Pass)
                        ->copyDefinition();
                auto& shuffle =
                    std::get<bloom::runtime::CompiledChannelShuffle>(definition.operations[0]);
                shuffle.channels.sources[0].nesting.push_back(CompiledCompositionSource{
                    NodeId::fromRaw(169190), 0, mapping(169191, -1.5, 2.0, 1)});
                definition.nestedPlans.push_back(child);
                definition.duration = bloom::core::RationalTime::fromInteger(100);
                keep(runMediaBuilder(
                    std::make_shared<const bloom::runtime::CompiledCompositionPlan>(
                        std::move(definition)),
                    evaluator, bloom::gpu_coverage_media::gateDirectory()));
            }
            // Image channels: the composite's R, G, B with alpha 1, and the alpha of a second
            // nested image (as an Image to Channel Set layer's plan is read) joined in, each on
            // its own mapped time (offset, PingPong).
            {
                auto definition = nestedParentPlan(child, 169200, 212)->copyDefinition();
                auto& operations = definition.operations;
                operations.insert(
                    operations.begin() + 1,
                    {CompiledCompositionSource{NodeId::fromRaw(169290), 0,
                                               mapping(169291, 0.5, 1.0, 2)},
                     CompiledImageEffect{NodeId::fromRaw(169292), OperationIndex::fromRaw(0),
                                         bloom::runtime::ChannelSelectKernel{
                                             {bloom::runtime::ImageChannelSource::Red,
                                              bloom::runtime::ImageChannelSource::Green,
                                              bloom::runtime::ImageChannelSource::Blue,
                                              bloom::runtime::ImageChannelSource::One},
                                             false}},
                     CompiledImageEffect{NodeId::fromRaw(169292), OperationIndex::fromRaw(2),
                                         bloom::runtime::ChannelCombineKernel{
                                             3, bloom::runtime::ImageChannelSource::Alpha, false},
                                         false, false, OperationIndex::fromRaw(1)}});
                // The parent's Layer Output now reads the joined image; its Merge and output
                // move down by the three inserted operations.
                std::get<bloom::runtime::CompiledLayerOutput>(operations[4]).input =
                    OperationIndex::fromRaw(3);
                std::get<bloom::runtime::CompiledMerge>(operations[5]).entries[0].input =
                    OperationIndex::fromRaw(4);
                std::get<bloom::runtime::CompiledCompositionOutput>(operations[6]).input =
                    OperationIndex::fromRaw(5);
                definition.output = OperationIndex::fromRaw(6);
                keep(runBuilder(std::make_shared<const bloom::runtime::CompiledCompositionPlan>(
                    std::move(definition))));
            }
            return combined;
        });
    // #364: Cryptomatte through the production rank gather, one data upload and one
    // CryptomatteCoverageV1 pass per rank layer (#549) and proxy resample: names resolved through the manifest, names resolved by hash without one,
    // a raw ID, and the Image output (beauty x matte through the MultiplyAlpha combine).
    const auto runCryptomatte = [](const bool manifest, std::vector<std::string> mattes,
                                   const bool imageOutput, const std::uint64_t idBase,
                                   const bool preview = false) {
        const bloom::runtime::CpuCompositionEvaluator evaluator;
        const auto& image = bloom::gpu_coverage_media::cryptomatteImage(manifest);
        const auto plan = bloom::gpu_coverage_media::cryptomattePlan(
            image.asset, LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, idBase,
            std::move(mattes), imageOutput, preview);
        return runMediaBuilder(plan, evaluator, bloom::gpu_coverage_media::gateDirectory());
    };
    const auto runAllCryptomatte = [runCryptomatte](const std::uint64_t idBase) {
        FixtureRun combined;
        combined.prepared = true;
        const auto sky =
            bloom::runtime::formatCryptomatteRawId(bloom::runtime::cryptomatteNameId("sky"));
        std::vector<FixtureRun> runs;
        runs.push_back(runCryptomatte(true, {"cube", "floor"}, false, idBase));
        runs.push_back(runCryptomatte(false, {"cube", "floor", sky}, false, idBase + 100));
        runs.push_back(runCryptomatte(true, {"sphere"}, true, idBase + 200));
        // The Viewer's ID-colour preview (the same kernel in its preview mode).
        runs.push_back(runCryptomatte(true, {"cube"}, false, idBase + 300, true));
        for (auto& frame : runs) {
            combined.prepared = combined.prepared && frame.prepared;
            combined.evidence += frame.evidence + "; ";
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
        }
        return combined;
    };
    add("operation.CompiledCryptomatte", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime Cryptomatte production scene preparation",
        [runAllCryptomatte] { return runAllCryptomatte(113700); });
    add("node.bloom.cryptomatte", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime Cryptomatte production scene preparation",
        [runAllCryptomatte] { return runAllCryptomatte(114100); });
    // #381: an interpreted still (alpha ignored, then inverted straight, then a premultiplied
    // matte) prepares the stored-samples upload and the native footage pass.
    add(
        "feature.footage.interpretation", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime footage interpretation native tests + production media preparation",
        [] {
            FixtureRun combined;
            combined.prepared = true;
            const auto& image = bloom::gpu_coverage_media::syntheticImage();
            std::uint64_t base = 114500;
            for (const auto mode : {bloom::document::AssetAlphaMode::Ignore,
                                    bloom::document::AssetAlphaMode::Straight,
                                    bloom::document::AssetAlphaMode::Premultiplied}) {
                auto asset = image.asset;
                asset.interpretation.alphaMode = mode;
                asset.interpretation.invertAlpha =
                    mode == bloom::document::AssetAlphaMode::Straight;
                if (mode == bloom::document::AssetAlphaMode::Premultiplied)
                    asset.interpretation.matteColor = {0.25, 0.5, 0.75};
                const bloom::runtime::CpuCompositionEvaluator evaluator;
                const auto plan = bloom::gpu_coverage_media::imageSourcePlan(
                    asset, LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, base);
                base += 100;
                auto run =
                    runMediaBuilder(plan, evaluator, bloom::gpu_coverage_media::gateDirectory());
                combined.prepared = combined.prepared && run.prepared;
                combined.evidence = run.evidence;
                for (auto& frame : run.frames)
                    combined.frames.push_back(std::move(frame));
            }
            return combined;
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add("node.bloom.image-source", GpuCoverageFixtureCriterion::Prepared,
        "src/media image source tests", [] {
            const bloom::runtime::CpuCompositionEvaluator evaluator;
            const auto& image = bloom::gpu_coverage_media::syntheticImage();
            const auto plan = bloom::gpu_coverage_media::imageSourcePlan(
                image.asset, LayerValues{.position = {4.3, 3.1}, .opacity = 1.0}, 110000);
            return runMediaBuilder(plan, evaluator, bloom::gpu_coverage_media::gateDirectory());
        });
    add(
        "node.bloom.video-source", GpuCoverageFixtureCriterion::Prepared,
        "src/media video source tests",
        [] {
            try {
                return runVideoFixture(bloom::gpu_coverage_video::probeVideoFixture(), 113000);
            } catch (const std::exception& error) {
                FixtureRun result;
                result.evidence = error.what();
                return result;
            }
        },
        {}, FixturePrerequisite::MediaFixtures);
    // #422: one fixture per decoded native-sample layout. Each is prepared by the production
    // builder on the GPU colour route, must upload that layout's samples (never a host float
    // frame), and the native gate executes it and compares it with the CPU oracle.
    const auto nativeSamples = [&add](const bloom::render::NativeSampleLayout layout,
                                      std::function<FixtureRun()> run,
                                      const FixturePrerequisite prerequisite) {
        add(std::string{"feature.media.native_samples."} +
                std::string{bloom::render::nativeSampleLayoutName(layout)},
            GpuCoverageFixtureCriterion::Prepared,
            "src/render SampleConvertV1 parity + production media preparation",
            [layout, run = std::move(run)] {
                auto result = run();
                if (!result.prepared)
                    return result;
                for (const auto& frame : result.frames) {
                    bool native = false;
                    for (const auto& command : frame.scene->commands())
                        if (const auto* upload =
                                std::get_if<bloom::runtime::GpuSceneUploadCommand>(&command))
                            native = native ||
                                     (upload->image == nullptr && upload->samples != nullptr &&
                                      upload->samples->conversion().layout == layout);
                    if (!native) {
                        result.prepared = false;
                        result.evidence = "the prepared scene uploads no " +
                                          std::string{bloom::render::nativeSampleLayoutName(layout)} +
                                          " native samples";
                        return result;
                    }
                }
                result.evidence += " (native " +
                                   std::string{bloom::render::nativeSampleLayoutName(layout)} +
                                   " upload)";
                return result;
            },
            {}, prerequisite);
    };
    const auto nativeVideo = [](const std::string& name, const std::uint64_t rawId,
                                const std::uint64_t idBase) {
        return [name, rawId, idBase] {
            try {
                return runVideoFixture(bloom::gpu_coverage_video::probeVideoFixture(name, rawId),
                                       idBase);
            } catch (const std::exception& error) {
                FixtureRun result;
                result.evidence = error.what();
                return result;
            }
        };
    };
    const auto nativeStill = [](const bloom::gpu_coverage_media::ImageFixture& (*image)(),
                                const std::uint64_t idBase) {
        return [image, idBase] {
            const bloom::runtime::CpuCompositionEvaluator evaluator;
            const auto plan = bloom::gpu_coverage_media::imageSourcePlan(
                image().asset, LayerValues{.position = {2.3, 1.1}, .opacity = 1.0}, idBase);
            return runMediaBuilder(plan, evaluator, bloom::gpu_coverage_media::gateDirectory());
        };
    };
    nativeSamples(bloom::render::NativeSampleLayout::Yuv420p8,
                  nativeVideo("numbered-h264.mp4", 910, 115000),
                  FixturePrerequisite::MediaFixturesAndShaderTools);
    nativeSamples(bloom::render::NativeSampleLayout::Yuva444p16,
                  nativeVideo("alpha-prores.mov", 911, 115300),
                  FixturePrerequisite::MediaFixturesAndShaderTools);
    nativeSamples(bloom::render::NativeSampleLayout::Rgba16,
                  nativeStill(&bloom::gpu_coverage_media::png16Image, 115600),
                  FixturePrerequisite::PackagedShaderTools);
    nativeSamples(bloom::render::NativeSampleLayout::RgbaHalf,
                  nativeStill(&bloom::gpu_coverage_media::halfExrImage, 115700),
                  FixturePrerequisite::PackagedShaderTools);
    add("node.bloom.text-source", GpuCoverageFixtureCriterion::Prepared,
        "src/render text_raster + text source tests",
        [] { return runBuilder(withSource(text(105000), 105000, nullptr, 0.6)); });
    add("node.bloom.shape-source", GpuCoverageFixtureCriterion::Prepared,
        "src/render PathRaster + shape source tests",
        [] { return runBuilder(withSource(shape(ShapeKind::Rectangle, 106000, true), 106000)); });
    add("node.bloom.composition-source", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime gpu nested scene preparation tests", [] {
            const auto child =
                nestedChildPlan(122000, 203, bloom::core::Color4d{0.2, 0.6, 0.9, 1.0});
            return runBuilder(nestedParentPlan(child, 123000, 101));
        });
    add(
        "node.bloom.ocio-colour-space-transform", GpuCoverageFixtureCriterion::Prepared,
        "src/color OCIO image effect tests", [] { return runCstEffectFixture(108000); }, {},
        FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.ocio-file-transform", GpuCoverageFixtureCriterion::Prepared,
        "src/color OCIO image effect tests", [] { return runFileTransformFixture(109000); }, {},
        FixturePrerequisite::PackagedShaderTools);
    add("node.bloom.exposure", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime pointwise image effect tests", [pointwise] {
            return runBuilder(effectPlan(pointwise(PointwiseImageEffect::Exposure, 1.25, 110100),
                                         110100));
        }, {}, FixturePrerequisite::PackagedShaderTools);
    add("node.bloom.offset", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime pointwise image effect tests", [pointwise] {
            return runBuilder(effectPlan(pointwise(PointwiseImageEffect::Offset, -0.35, 110200),
                                         110200));
        }, {}, FixturePrerequisite::PackagedShaderTools);
    add("node.bloom.multiply", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime pointwise image effect parity tests", [pointwise] {
            return runBuilder(effectPlan(pointwise(PointwiseImageEffect::Multiply, -2.0, 114000),
                                         114000));
        }, {}, FixturePrerequisite::PackagedShaderTools);
    add("node.bloom.invert", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime pointwise image effect parity tests", [] {
            return runBuilder(effectPlan(
                bloom::runtime::PointwiseImageKernel{PointwiseImageEffect::Invert, {}}, 114100));
        }, {}, FixturePrerequisite::PackagedShaderTools);
    add("node.bloom.image-mix", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime image combine production scene preparation", [] {
            return runBuilder(imageCombinePlan(bloom::runtime::ImageCombineMode::Mix, 110300));
        });
    add("node.bloom.set-alpha", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime image combine production scene preparation", [] {
            return runBuilder(imageCombinePlan(bloom::runtime::ImageCombineMode::SetAlpha,
                                               110400));
        });
    add("node.bloom.crop", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime canvas image effect tests", [canvas] {
            return runBuilder(effectPlan(canvas(bloom::runtime::CanvasImageEffect::Crop, 110500),
                                         110500));
        });
    add("node.bloom.pad", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime canvas image effect tests", [canvas] {
            return runBuilder(effectPlan(canvas(bloom::runtime::CanvasImageEffect::Pad, 110600),
                                         110600));
        });
    // #361 matte axes: the inverted selector and an absent matte (a trimmed matte Layer with no
    // image) both prepare through the production builder's resident combine dispatch.
    // ADR 0024 layer time axes: start/speed, Time Remap, out-of-extent modes and an upstream shared
    // by two layers on different clocks, all prepared through the production builder.
    using bloom::gpu_coverage_plans::LayerTimeAxis;
    for (const auto& [id, axis, base] :
         {std::tuple{"feature.layer.time.offset_speed", LayerTimeAxis::OffsetSpeed, 111000},
          std::tuple{"feature.layer.time.remap", LayerTimeAxis::Remap, 111100},
          std::tuple{"feature.layer.time.extent_modes", LayerTimeAxis::ExtentModes, 111200},
          std::tuple{"feature.layer.time.shared_upstream", LayerTimeAxis::SharedUpstream, 111300}})
        add(id, GpuCoverageFixtureCriterion::Prepared,
            "src/runtime layer time production scene preparation",
            [axis = axis, base = base] {
                return runBuilder(bloom::gpu_coverage_plans::layerTimePlan(
                    axis, static_cast<std::uint64_t>(base)));
            });
    add("feature.matte.invert", GpuCoverageFixtureCriterion::Prepared,
        "src/render ImageCombineV1 native + image combine production scene preparation", [] {
            return runBuilder(matteCombinePlan(bloom::runtime::ImageCombineMode::SetAlpha, 110800,
                                               true, false));
        });
    add("feature.matte.absent", GpuCoverageFixtureCriterion::Prepared,
        "src/render ImageCombineV1 native + image combine production scene preparation", [] {
            return runBuilder(matteCombinePlan(bloom::runtime::ImageCombineMode::MultiplyAlpha,
                                               110900, true, true));
        });
    // ADR 0028: layer masks (every mode and control, constant and keyed paths), the Mask value
    // through Image Mix (present and absent) and track mattes (Alpha, Luma, inverted, absent).
    const auto runAll = [](std::vector<std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>>
                               plans,
                           std::string evidence) {
        FixtureRun combined;
        combined.prepared = true;
        for (const auto& plan : plans) {
            auto frame = runBuilder(plan);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
        }
        combined.evidence = std::move(evidence);
        return combined;
    };
    const auto maskPlans = [] {
        return std::vector{effectPlan(bloom::gpu_coverage_plans::layerMaskKernel(112000), 112000),
                           effectPlan(bloom::gpu_coverage_plans::layerMaskKernel(112100, true),
                                      112100)};
    };
    add(
        "effect.LayerMaskKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime layer mask production scene preparation",
        [runAll, maskPlans] { return runAll(maskPlans(), "production layer masks"); }, {},
        FixturePrerequisite::PackagedShaderTools);
    add(
        "feature.mask.layer", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime layer mask production scene preparation",
        [runAll, maskPlans] { return runAll(maskPlans(), "production layer masks"); }, {},
        FixturePrerequisite::PackagedShaderTools);
    const auto valuePlans = [] {
        return std::vector{bloom::gpu_coverage_plans::maskValuePlan(112200, false),
                           bloom::gpu_coverage_plans::maskValuePlan(112300, true)};
    };
    add(
        "feature.mask.value", GpuCoverageFixtureCriterion::Prepared,
        "src/render ImageCombineV1 mask binding + image combine production scene preparation",
        [runAll, valuePlans] { return runAll(valuePlans(), "production Image Mix mask"); }, {},
        FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.mask", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime layer mask production scene preparation",
        [runAll, valuePlans] { return runAll(valuePlans(), "production Mask node"); }, {},
        FixturePrerequisite::PackagedShaderTools);
    add("feature.matte.track", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime track matte production scene preparation", [runAll] {
            using bloom::render::ImageCoverageChannel;
            return runAll({bloom::gpu_coverage_plans::trackMattePlan(
                               112400, ImageCoverageChannel::Alpha, false, false),
                           bloom::gpu_coverage_plans::trackMattePlan(
                               112500, ImageCoverageChannel::Luma, true, false),
                           bloom::gpu_coverage_plans::trackMattePlan(
                               112600, ImageCoverageChannel::Alpha, false, true)},
                          "production track mattes");
        });
    // #383 matte workflow: Mask Combine, Image to Mask, the effect mask, matte layer masks and
    // node track mattes as the production lowering's chains of existing kernels.
    constexpr auto matteEvidence = "src/runtime matte workflow production scene preparation";
    const auto combinePlans = [] {
        using bloom::document::LayerMaskMode;
        return std::vector{
            bloom::gpu_coverage_plans::maskCombinePlan(23830000, LayerMaskMode::Add),
            bloom::gpu_coverage_plans::maskCombinePlan(23830100, LayerMaskMode::Subtract),
            bloom::gpu_coverage_plans::maskCombinePlan(23830200, LayerMaskMode::Intersect),
            bloom::gpu_coverage_plans::maskCombinePlan(23830300, LayerMaskMode::Difference)};
    };
    const auto imageToMaskPlans = [] {
        using bloom::render::ImageCoverageChannel;
        return std::vector{
            bloom::gpu_coverage_plans::imageToMaskPlan(23830400, ImageCoverageChannel::Alpha, false),
            bloom::gpu_coverage_plans::imageToMaskPlan(23830500, ImageCoverageChannel::Luma, true),
            bloom::gpu_coverage_plans::imageToMaskPlan(23830600, ImageCoverageChannel::Green,
                                                       false)};
    };
    for (const auto* id : {"feature.mask.combine", "node.bloom.mask-combine"})
        add(
            id, GpuCoverageFixtureCriterion::Prepared, matteEvidence,
            [runAll, combinePlans] { return runAll(combinePlans(), "production Mask Combine"); },
            {}, FixturePrerequisite::PackagedShaderTools);
    add("node.bloom.image-to-mask", GpuCoverageFixtureCriterion::Prepared, matteEvidence,
        [runAll, imageToMaskPlans] {
            return runAll(imageToMaskPlans(), "production Image to Mask");
        });
    add(
        "feature.mask.effect_input", GpuCoverageFixtureCriterion::Prepared, matteEvidence,
        [runAll] {
            return runAll({bloom::gpu_coverage_plans::effectMaskPlan(23830700, false, false),
                           bloom::gpu_coverage_plans::effectMaskPlan(23830800, true, false),
                           bloom::gpu_coverage_plans::effectMaskPlan(23830900, false, true),
                           bloom::gpu_coverage_plans::effectMaskPlan(23830950, true, false, true),
                           bloom::gpu_coverage_plans::effectMaskPlan(23830980, false, true,
                                                                     true)},
                          "production effect mask");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "feature.mask.layer_matte", GpuCoverageFixtureCriterion::Prepared, matteEvidence,
        [runAll] {
            return runAll({bloom::gpu_coverage_plans::layerMatteMaskPlan(23831000)},
                          "production matte layer masks");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "feature.matte.track_node", GpuCoverageFixtureCriterion::Prepared, matteEvidence,
        [runAll] {
            using bloom::render::ImageCoverageChannel;
            return runAll({bloom::gpu_coverage_plans::nodeTrackMattePlan(
                               23831100, ImageCoverageChannel::Alpha, false),
                           bloom::gpu_coverage_plans::nodeTrackMattePlan(
                               23831200, ImageCoverageChannel::Alpha, true)},
                          "production node track mattes");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add("node.bloom.multiply-alpha", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime image combine production scene preparation", [] {
            return runBuilder(imageCombinePlan(bloom::runtime::ImageCombineMode::MultiplyAlpha,
                                               110700));
        });
    add("node.bloom.checker-source", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime Checker CPU and native parity tests", [checkerPlan] {
            return runBuilder(checkerPlan());
        });
    const auto runBlur = [](const bloom::runtime::SeparableBlurKind kind) {
        FixtureRun combined;
        combined.prepared = true;
        for (const auto edge :
             {bloom::runtime::BlurEdgeMode::Transparent, bloom::runtime::BlurEdgeMode::Clamp}) {
            const auto plan = effectPlan(
                bloom::runtime::SeparableBlurKernel{
                    kind, {ParameterId::fromRaw(111101), 2.0}, edge},
                111100);
            auto request = requestFor(*plan);
            auto frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
            const auto roiWindow = bloom::render::ImageWindow::create(1, 1, 3, 2);
            request.roi = *roiWindow.value();
            frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
        }
        combined.evidence = "production separable blur: transparent/clamp and ROI";
        return combined;
    };
    add(
        "effect.SeparableBlurKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime separable image effect tests",
        [runBlur] { return runBlur(bloom::runtime::SeparableBlurKind::Gaussian); }, {},
        FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.gaussian-blur", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime separable image effect tests",
        [runBlur] { return runBlur(bloom::runtime::SeparableBlurKind::Gaussian); }, {},
        FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.box-blur", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime separable image effect tests",
        [runBlur] { return runBlur(bloom::runtime::SeparableBlurKind::Box); }, {},
        FixturePrerequisite::PackagedShaderTools);
    // #337 Matte Blur, #338 Matte Erode/Dilate and Luma Key: a Mask node's coverage (or, for Luma
    // Key, the masked canvas keyed into a Mask) filtered and read by Image Mix's mask, for each
    // kernel and through an ROI request. Never the Rgba blur or an identity.
    const auto runMaskFilters =
        [](const std::vector<std::function<ImageEffectKernel(std::uint64_t)>>& kernels,
           std::uint64_t idBase, std::string evidence) {
            FixtureRun combined;
            combined.prepared = true;
            for (const auto& kernel : kernels) {
                const auto plan =
                    bloom::gpu_coverage_plans::maskValuePlan(idBase, false, kernel(idBase));
                idBase += 100;
                auto request = requestFor(*plan);
                auto frame = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && frame.prepared;
                for (auto& value : frame.frames)
                    combined.frames.push_back(std::move(value));
                const auto roiWindow = bloom::render::ImageWindow::create(2, 1, 5, 4);
                request.roi = *roiWindow.value();
                frame = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && frame.prepared;
                for (auto& value : frame.frames)
                    combined.frames.push_back(std::move(value));
            }
            combined.evidence = std::move(evidence);
            return combined;
        };
    const auto matteFilter = [](const bloom::runtime::SeparableBlurKind kind) {
        std::vector<std::function<ImageEffectKernel(std::uint64_t)>> kernels;
        for (const auto edge :
             {bloom::runtime::BlurEdgeMode::Transparent, bloom::runtime::BlurEdgeMode::Clamp})
            kernels.emplace_back([kind, edge](const std::uint64_t idBase) -> ImageEffectKernel {
                return bloom::runtime::SeparableBlurKernel{
                    kind, {ParameterId::fromRaw(idBase + 71), 1.5}, edge,
                    bloom::runtime::BlurChannels::Coverage};
            });
        return kernels;
    };
    add(
        "node.bloom.matte-blur", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime separable image effect tests",
        [runMaskFilters, matteFilter] {
            return runMaskFilters(matteFilter(bloom::runtime::SeparableBlurKind::Gaussian), 160000,
                                  "production Matte Blur: Mask coverage, transparent/clamp and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.matte-erode", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime separable image effect tests",
        [runMaskFilters, matteFilter] {
            return runMaskFilters(matteFilter(bloom::runtime::SeparableBlurKind::Erode), 161000,
                                  "production Matte Erode: Mask minimum, transparent/clamp and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.matte-dilate", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime separable image effect tests",
        [runMaskFilters, matteFilter] {
            return runMaskFilters(matteFilter(bloom::runtime::SeparableBlurKind::Dilate), 162000,
                                  "production Matte Dilate: Mask maximum, transparent/clamp and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // Luma Key: a ramp and an inverted ramp across the white canvas's luma (so its partial mask
    // coverage scales a fractional key), and a hard key.
    const auto lumaKeys = [] {
        std::vector<std::function<ImageEffectKernel(std::uint64_t)>> kernels;
        for (const auto [low, high, invert] :
             {std::tuple{0.5, 1.5, false}, std::tuple{0.25, 2.0, true},
              std::tuple{0.75, 0.75, false}})
            kernels.emplace_back([low, high, invert](const std::uint64_t idBase) -> ImageEffectKernel {
                return bloom::runtime::LumaKeyKernel{{ParameterId::fromRaw(idBase + 71), low},
                                                     {ParameterId::fromRaw(idBase + 72), high},
                                                     invert};
            });
        return kernels;
    };
    add(
        "effect.LumaKeyKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime Luma Key image effect tests",
        [] {
            return runBuilder(effectPlan(
                bloom::runtime::LumaKeyKernel{{ParameterId::fromRaw(163001), 0.0},
                                              {ParameterId::fromRaw(163002), 0.5},
                                              false},
                163000));
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.luma-key", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime Luma Key image effect tests",
        [runMaskFilters, lumaKeys] {
            return runMaskFilters(lumaKeys(), 164000,
                                  "production Luma Key: ramp, inverted and hard keys into Image "
                                  "Mix's mask, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #340 Levels, Curves and Hue/Saturation over an HDR, signed, partial-alpha linear gradient,
    // full frame and through an ROI, each through the production builder's resident program.
    const auto runGrades =
        [](const std::vector<std::function<ImageEffectKernel(std::uint64_t)>>& kernels,
           std::uint64_t idBase, std::string evidence) {
            FixtureRun combined;
            combined.prepared = true;
            for (const auto& kernel : kernels) {
                auto definition =
                    bloom::gpu_coverage_plans::effectPlan(kernel(idBase), idBase)->copyDefinition();
                definition.operations[0] = bloom::gpu_coverage_plans::gradient(false, idBase + 50);
                const auto plan = std::make_shared<const bloom::runtime::CompiledCompositionPlan>(
                    std::move(definition));
                idBase += 100;
                auto request = requestFor(*plan);
                auto frame = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && frame.prepared;
                for (auto& value : frame.frames)
                    combined.frames.push_back(std::move(value));
                const auto roiWindow = bloom::render::ImageWindow::create(5, 3, 6, 4);
                request.roi = *roiWindow.value();
                frame = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && frame.prepared;
                for (auto& value : frame.frames)
                    combined.frames.push_back(std::move(value));
            }
            combined.evidence = std::move(evidence);
            return combined;
        };
    const auto levels = [](const std::array<double, 5> master, const std::array<double, 5> red,
                           const std::array<double, 5> alpha, const bool clamp) {
        return [=](const std::uint64_t idBase) -> ImageEffectKernel {
            bloom::runtime::LevelsKernel kernel;
            std::uint64_t id = idBase + 60;
            for (std::size_t channel = 0; channel < kernel.channels.size(); ++channel) {
                const auto& values = channel == 0   ? master
                                     : channel == 1 ? red
                                     : channel == 4 ? alpha
                                                    : std::array{0.0, 1.0, 1.0, 0.0, 1.0};
                auto& c = kernel.channels[channel];
                c = {{ParameterId::fromRaw(id), values[0]},
                     {ParameterId::fromRaw(id + 1), values[1]},
                     {ParameterId::fromRaw(id + 2), values[2]},
                     {ParameterId::fromRaw(id + 3), values[3]},
                     {ParameterId::fromRaw(id + 4), values[4]}};
                id += 5;
            }
            kernel.clamp = clamp;
            return kernel;
        };
    };
    const auto curves = [](const double middle, const double alphaLift, const bool clamp) {
        return [=](const std::uint64_t idBase) -> ImageEffectKernel {
            using bloom::document::GradeCurve;
            using bloom::runtime::testing::legacyPoints;
            auto kernel = bloom::runtime::testing::identityCurvesKernel(idBase + 60, clamp);
            bloom::runtime::testing::setCurve(
                kernel, GradeCurve::Master,
                legacyPoints({{1, {0.25, 0.1}}, {2, {0.5, middle}}, {3, {0.75, 0.9}}}));
            bloom::runtime::testing::setCurve(kernel, GradeCurve::Green,
                                              legacyPoints({{4, {1.0, 1.6}}}));
            bloom::runtime::testing::setCurve(kernel, GradeCurve::Alpha,
                                              legacyPoints({{0, {0.0, alphaLift}}}));
            return kernel;
        };
    };
    // #493: one curve kind at a time over the identity, so each fixture claims its own axis.
    const auto curveKind = [](const bloom::document::GradeCurve curve,
                              const std::vector<bloom::document::Vec2d> points) {
        return [=](const std::uint64_t idBase) -> ImageEffectKernel {
            auto kernel = bloom::runtime::testing::identityCurvesKernel(idBase + 60);
            bloom::runtime::testing::setCurve(kernel, curve, points);
            return kernel;
        };
    };
    const auto hueSaturation = [](const double hue, const double saturation) {
        return [=](const std::uint64_t idBase) -> ImageEffectKernel {
            return bloom::runtime::HueSaturationKernel{
                {ParameterId::fromRaw(idBase + 60), hue},
                {ParameterId::fromRaw(idBase + 61), saturation}};
        };
    };
    add(
        "effect.LevelsKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime color grade image effect tests",
        [runGrades, levels] {
            return runGrades({levels({-0.25, 2.0, 1.8, 0.0, 1.0}, {0.0, 1.0, 1.0, 0.0, 1.0},
                                     {0.0, 1.0, 1.0, 0.0, 1.0}, false)},
                             165000, "production Levels: gamma over HDR, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.levels", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime color grade image effect tests",
        [runGrades, levels] {
            return runGrades(
                {levels({-0.25, 2.0, 2.2, 0.0, 1.0}, {0.0, 1.0, 0.5, 0.1, 3.0},
                        {0.0, 1.0, 1.0, 0.2, 0.9}, false),
                 levels({0.5, 0.5, 1.0, 0.0, 1.0}, {1.0, 0.0, 1.0, 0.0, 1.0},
                        {0.0, 1.0, 1.0, 0.0, 1.0}, true)},
                165200,
                "production Levels: per-channel gamma, alpha lift, step and clamp, full frame and "
                "ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "effect.CurvesKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime color grade image effect tests",
        [runGrades, curves] {
            return runGrades({curves(0.6, 0.0, false)}, 165600,
                             "production Curves: monotone cubic with HDR extrapolation");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.curves", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime color grade image effect tests",
        [runGrades, curves] {
            return runGrades({curves(0.4, 0.3, false), curves(0.7, 0.0, true)}, 165800,
                             "production Curves: S-curve, HDR extrapolation, alpha lift and clamp, "
                             "full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    {
        using bloom::document::GradeCurve;
        const std::vector<std::tuple<const char*, GradeCurve, std::vector<bloom::document::Vec2d>,
                                     std::uint64_t>>
            kinds{{"feature.curves.points", GradeCurve::Master,
                   {{0.0, 0.05}, {0.1, 0.06}, {0.2, 0.2}, {0.3, 0.35}, {0.4, 0.38}, {0.5, 0.55},
                    {0.6, 0.62}, {0.7, 0.8}, {0.8, 0.82}, {0.9, 0.95}, {1.0, 1.0}, {2.0, 1.5}},
                   166600},
                  {"feature.curves.hue_vs_hue", GradeCurve::HueVsHue,
                   {{0.0, 0.1}, {0.3, -0.2}, {0.6, 0.05}}, 166800},
                  {"feature.curves.hue_vs_saturation", GradeCurve::HueVsSaturation,
                   {{0.1, 1.8}, {0.5, 0.3}, {0.8, 1.2}}, 167000},
                  {"feature.curves.hue_vs_luma", GradeCurve::HueVsLuma,
                   {{0.05, 1.5}, {0.4, 0.6}, {0.7, 1.3}}, 167200},
                  {"feature.curves.luma_vs_saturation", GradeCurve::LumaVsSaturation,
                   {{0.0, 1.5}, {0.5, 0.4}, {1.0, 1.0}}, 167400}};
        for (const auto& [id, curve, points, base] : kinds)
            add(
                id, GpuCoverageFixtureCriterion::Prepared,
                "src/runtime color grade image effect tests",
                [runGrades, curveKind, curve = curve, points = points, base = base] {
                    return runGrades({curveKind(curve, points)}, base,
                                     "production Curves: one curve kind over an HDR, signed, "
                                     "partial-alpha gradient, full frame and ROI");
                },
                {}, FixturePrerequisite::PackagedShaderTools);
    }
    add(
        "effect.HueSaturationKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime color grade image effect tests",
        [runGrades, hueSaturation] {
            return runGrades({hueSaturation(60.0, 1.3)}, 166200,
                             "production Hue/Saturation: linear RGB matrix over associated colour");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.hue-saturation", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime color grade image effect tests",
        [runGrades, hueSaturation] {
            return runGrades({hueSaturation(-120.0, 0.0), hueSaturation(200.0, 2.5)}, 166400,
                             "production Hue/Saturation: desaturate and oversaturated rotation, "
                             "full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #503 Alpha Association over the HDR, signed, partial-alpha gradient: every mode through the
    // production builder's one resident program.
    const auto association = [](const bloom::runtime::AlphaAssociationMode mode, const bool white,
                                const bloom::core::Color4d background) {
        return [=](const std::uint64_t idBase) -> ImageEffectKernel {
            return bloom::runtime::AlphaAssociationKernel{
                mode, white, {ParameterId::fromRaw(idBase + 60), background}};
        };
    };
    add(
        "effect.AlphaAssociationKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime alpha association tests",
        [runGrades, association] {
            using Mode = bloom::runtime::AlphaAssociationMode;
            return runGrades({association(Mode::Unmult, false, {}),
                              association(Mode::Premultiply, false, {}),
                              association(Mode::Unpremultiply, false, {}),
                              association(Mode::RemoveMatting, false, {0.25, 0.5, 1.5, 1.0})},
                             503000,
                             "production Alpha Association: Unmult, Premultiply, Unpremultiply "
                             "and Remove Color Matting over HDR and signed values, full frame "
                             "and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.alpha-association", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime alpha association tests",
        [runGrades, association] {
            using Mode = bloom::runtime::AlphaAssociationMode;
            return runGrades({association(Mode::Unmult, true, {}),
                              association(Mode::RemoveMatting, false, {-0.5, 0.75, 2.0, 1.0})},
                             503600,
                             "production Alpha Association node: white-backed Unmult and a "
                             "signed HDR matte colour, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #518 Tone Map over the HDR, signed, partial-alpha gradient: each method, shadow recovery, a
    // hard clip and gamut compression through the production builder's one resident program.
    const auto toneMap = [](const bloom::color_tool_test::ToneMapSettings settings) {
        return [=](const std::uint64_t idBase) -> ImageEffectKernel {
            return bloom::color_tool_test::toneMap(idBase, settings);
        };
    };
    add(
        "effect.ColorToolKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime color tool tests",
        [runGrades, toneMap] {
            bloom::color_tool_test::ToneMapSettings brightest;
            brightest.method = 1;
            bloom::color_tool_test::ToneMapSettings shadows;
            shadows.shadows = 1.5;
            shadows.knee = 2.0;
            shadows.ceiling = 6.0;
            bloom::color_tool_test::ColorMatrixSettings gray;
            gray.preset = 2;
            return runGrades({toneMap({}), toneMap(brightest), toneMap(shadows),
                              [gray](const std::uint64_t idBase) -> ImageEffectKernel {
                                  return bloom::color_tool_test::colorMatrix(idBase, gray);
                              }},
                             518000,
                             "production Tone Map and Color Matrix: both roll-off methods, shadow "
                             "recovery and a gray mix over HDR and signed values, full frame and "
                             "ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.tone-map", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime color tool tests",
        [runGrades, toneMap] {
            bloom::color_tool_test::ToneMapSettings hard;
            hard.knee = 0.8;
            hard.ceiling = 0.5;
            bloom::color_tool_test::ToneMapSettings gamut;
            gamut.gamut = true;
            gamut.method = 1;
            gamut.shadows = -0.5;
            return runGrades({toneMap(hard), toneMap(gamut)}, 518600,
                             "production Tone Map node: a hard clip, and gamut compression with a "
                             "brightest-channel roll-off and darkened shadows, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #517 Color Matrix: a 3 x 3 mix, a 4 x 4 mix that makes alpha from color, and every color model
    // in and out through the production builder.
    const auto colorMatrix = [](const bloom::color_tool_test::ColorMatrixSettings settings) {
        return [=](const std::uint64_t idBase) -> ImageEffectKernel {
            return bloom::color_tool_test::colorMatrix(idBase, settings);
        };
    };
    const auto matrixModels = [colorMatrix](const std::int64_t first, const std::int64_t last) {
        std::vector<std::function<ImageEffectKernel(std::uint64_t)>> kernels;
        for (std::int64_t model = first; model <= last; ++model) {
            bloom::color_tool_test::ColorMatrixSettings out;
            out.outputModel = model;
            bloom::color_tool_test::ColorMatrixSettings in;
            in.inputModel = model;
            in.preset = 3;
            kernels.push_back(colorMatrix(out));
            kernels.push_back(colorMatrix(in));
        }
        return kernels;
    };
    add(
        "node.bloom.color-matrix", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime color tool tests",
        [runGrades, colorMatrix, matrixModels] {
            bloom::color_tool_test::ColorMatrixSettings four;
            four.size = bloom::document::kColorMatrixSize4;
            four.matrix = {0.5, 0.5, 0, 0.25, 0, 1, 0, 0, 2, 0, -1, 0, 0.2126, 0.7152, 0.0722, 0};
            auto kernels = matrixModels(1, 4);
            kernels.push_back(colorMatrix(four));
            return runGrades(kernels, 517000,
                             "production Color Matrix node: a 4 x 4 mix with alpha from color and "
                             "every color model read and written, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #523 HSL Qualifier: a soft hue window across the wrap, a saturation and a luma window, and an
    // inverted selection, as mask coverage through the production builder.
    add(
        "node.bloom.hsl-qualifier", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime color tool tests",
        [runGrades] {
            bloom::color_tool_test::HslQualifierSettings hue;
            hue.hueCenter = 350.0;
            hue.hueWidth = 60.0;
            hue.hueSoftness = 40.0;
            bloom::color_tool_test::HslQualifierSettings ranges;
            ranges.satLow = 0.2;
            ranges.satHigh = 0.9;
            ranges.lumaLow = 0.1;
            ranges.lumaHigh = 2.0;
            ranges.lumaSoftness = 0.5;
            ranges.invert = true;
            bloom::color_tool_test::HslQualifierSettings hard;
            hard.hueCenter = 120.0;
            hard.hueWidth = 90.0;
            hard.hueSoftness = 0.0;
            hard.satSoftness = 0.0;
            hard.lumaSoftness = 0.0;
            const auto make = [](const bloom::color_tool_test::HslQualifierSettings settings) {
                return [=](const std::uint64_t idBase) -> ImageEffectKernel {
                    return bloom::color_tool_test::hslQualifier(idBase, settings);
                };
            };
            return runGrades({make(hue), make(ranges), make(hard)}, 523000,
                             "production HSL Qualifier node: a soft hue window across the wrap, "
                             "inverted saturation and luma windows and hard edges over HDR and "
                             "signed values, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #519 Quantize: a hard luma threshold, a soft inverted per-channel threshold, a linked posterize
    // and an unlinked posterize that also snaps alpha, over the HDR, signed, partial-alpha gradient.
    add(
        "node.bloom.quantize", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime quantize tests",
        [runGrades] {
            const auto make = [](const bloom::color_tool_test::QuantizeSettings settings) {
                return [=](const std::uint64_t idBase) -> ImageEffectKernel {
                    return bloom::color_tool_test::quantize(idBase, settings);
                };
            };
            bloom::color_tool_test::QuantizeSettings hardLuma;
            hardLuma.level = 0.4;
            bloom::color_tool_test::QuantizeSettings softChannel;
            softChannel.basis = bloom::document::kQuantizeBasisPerChannel;
            softChannel.level = 0.6;
            softChannel.softness = 0.5;
            softChannel.invert = true;
            bloom::color_tool_test::QuantizeSettings linked;
            linked.mode = bloom::document::kQuantizePosterize;
            linked.levels = 4.0;
            bloom::color_tool_test::QuantizeSettings unlinked;
            unlinked.mode = bloom::document::kQuantizePosterize;
            unlinked.link = false;
            unlinked.red = 2.0;
            unlinked.green = 7.0;
            unlinked.blue = 256.0;
            unlinked.alpha = true;
            unlinked.alphaLevels = 3.0;
            return runGrades({make(hardLuma), make(softChannel), make(linked), make(unlinked)}, 519000,
                             "production Quantize node: a hard luma threshold, a soft inverted "
                             "per-channel threshold, a linked posterize and an unlinked posterize "
                             "with alpha levels over HDR and signed values, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #504 Colorize: Fill, Tint, a partial Tritone, a mirrored and a repeating Gradient Map from
    // a channel and luma, and a custom Black & White mix; #541 a repeating Gradient Map through a
    // bound multi-stop gradient (unsorted stops, a hard edge, HDR and signed colours).
    const auto colorize = [](const int variant, const bloom::document::GradientModes blend = {}) {
        return [=](const std::uint64_t idBase) -> ImageEffectKernel {
            using bloom::runtime::ColorizeCycle;
            using bloom::runtime::ColorizeMode;
            using bloom::runtime::ColorizeSource;
            bloom::runtime::ColorizeKernel kernel;
            const std::array<ColorizeMode, 7> modes{
                ColorizeMode::Fill,        ColorizeMode::Tint,          ColorizeMode::Tritone,
                ColorizeMode::GradientMap, ColorizeMode::GradientMap,   ColorizeMode::BlackAndWhite,
                ColorizeMode::GradientMap};
            kernel.mode = modes[static_cast<std::size_t>(variant)];
            kernel.amount = {ParameterId::fromRaw(idBase + 60), variant == 2 ? 0.6 : 1.0};
            kernel.color = {ParameterId::fromRaw(idBase + 61),
                            bloom::core::Color4d{0.9, 0.3, 1.5, 1.0}};
            kernel.shadows = {ParameterId::fromRaw(idBase + 62),
                              bloom::core::Color4d{0.05, -0.02, 0.2, 1.0}};
            kernel.midtones = {ParameterId::fromRaw(idBase + 63),
                               bloom::core::Color4d{0.6, 0.2, -0.1, 1.0}};
            kernel.highlights = {ParameterId::fromRaw(idBase + 64),
                                 bloom::core::Color4d{2.0, 0.9, 0.4, 1.0}};
            kernel.midpoint = {ParameterId::fromRaw(idBase + 65), 0.3};
            if (variant == 3) {
                kernel.source = ColorizeSource::Green;
                kernel.cycle = ColorizeCycle::Mirror;
                kernel.repetitions = {ParameterId::fromRaw(idBase + 66), 2.5};
                kernel.phase = {ParameterId::fromRaw(idBase + 67), -0.3};
            }
            if (variant == 4 || variant == 6) {
                kernel.cycle = ColorizeCycle::Repeat;
                kernel.repetitions = {ParameterId::fromRaw(idBase + 66), 3.0};
                kernel.phase = {ParameterId::fromRaw(idBase + 67), 0.125};
            }
            if (variant == 6)
                kernel.gradient = bloom::runtime::CompiledGradientParameter{
                    ParameterId::fromRaw(idBase + 75),
                    bloom::document::GradientValue{
                        {{0.8, bloom::core::Color4d{1.2, 0.9, 0.1, 1.0}},
                         {0.0, bloom::core::Color4d{0.0, -0.05, 0.3, 1.0}},
                         {0.5, bloom::core::Color4d{0.1, 0.6, 0.2, 1.0}},
                         {0.5, bloom::core::Color4d{0.9, 0.1, -0.2, 1.0}},
                         {1.0, bloom::core::Color4d{3.0, 2.0, 1.5, 1.0}}},
                        blend}};
            const std::array<double, 6> weights{-1.5, 2.5, 0.1, 3.0, -2.0, 1.2};
            if (variant == 5)
                for (std::size_t i = 0; i < weights.size(); ++i)
                    kernel.weights[i] = {ParameterId::fromRaw(idBase + 68 + i), weights[i]};
            return ImageEffectKernel{kernel};
        };
    };
    add(
        "effect.ColorizeKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime colorize tests",
        [runGrades, colorize] {
            return runGrades({colorize(0), colorize(1), colorize(2), colorize(3), colorize(4),
                              colorize(5), colorize(6)},
                             504000,
                             "production Colorize: Fill, Tint, Tritone, Gradient Map (mirrored, "
                             "repeating and through a multi-stop gradient) and Black & White over "
                             "HDR and signed values, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.colorize", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime colorize tests",
        [runGrades, colorize] {
            return runGrades({colorize(3), colorize(5), colorize(6)}, 504600,
                             "production Colorize node: a channel Gradient Map, a Black & White "
                             "mix and a multi-stop Gradient Map, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #525 Fields: field programs over the HDR, signed, partial-alpha gradient canvas (an Image
    // field reads that gradient through its slot, so the atlas route is taken), a mask's exact
    // distance, every field operation, sub-samples, output modes and each Fields node's lowering.
    const auto runFields = [](const std::vector<bloom::runtime::ImageEffectKernel>& kernels,
                              std::uint64_t idBase, std::string evidence) {
        FixtureRun combined;
        combined.prepared = true;
        for (const auto& kernel : kernels) {
            auto definition =
                bloom::gpu_coverage_plans::effectPlan(kernel, idBase)->copyDefinition();
            definition.operations[0] = bloom::gpu_coverage_plans::gradient(false, idBase + 50);
            if (const auto* field = std::get_if<bloom::runtime::FieldRasterKernel>(&kernel);
                field != nullptr) {
                // One image per slot the program reads (the same picture in each).
                int slots = 0;
                for (const auto& instruction : field->program.instructions)
                    slots = std::max(slots, instruction.slot + 1);
                std::get<bloom::runtime::CompiledImageEffect>(definition.operations[1])
                    .fieldImages.assign(static_cast<std::size_t>(slots),
                                        bloom::runtime::OperationIndex::fromRaw(0));
            }
            const auto plan =
                std::make_shared<const bloom::runtime::CompiledCompositionPlan>(std::move(definition));
            idBase += 100;
            auto request = requestFor(*plan);
            auto frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
            const auto roiWindow = bloom::render::ImageWindow::create(5, 3, 6, 4);
            request.roi = *roiWindow.value();
            frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
        }
        combined.evidence = std::move(evidence);
        return combined;
    };
    const auto fieldKernel = [](const bloom::core::field::FieldOp op, const std::uint64_t idBase,
                                const int variant = 0, const std::uint32_t samples = 1) {
        return bloom::runtime::ImageEffectKernel{bloom::runtime::FieldRasterKernel{
            bloom::field_test_programs::operationProgram(op, idBase, variant), samples}};
    };
    const auto addField = [&add, runFields](std::string id,
                                            std::vector<bloom::runtime::ImageEffectKernel> kernels,
                                            const std::uint64_t idBase, std::string evidence) {
        add(
            std::move(id), GpuCoverageFixtureCriterion::Prepared, "src/runtime field raster tests",
            [runFields, kernels = std::move(kernels), idBase, evidence = std::move(evidence)] {
                return runFields(kernels, idBase, evidence);
            },
            {}, FixturePrerequisite::PackagedShaderTools);
    };
    {
        using bloom::core::field::FieldOp;
        addField("effect.FieldRasterKernel",
                 {fieldKernel(FieldOp::Noise, 525000, 1), fieldKernel(FieldOp::Image, 525100),
                  fieldKernel(FieldOp::Ramp, 525200, 0, 4)},
                 525300,
                 "production field raster: a fractal noise, an Image field through the atlas and "
                 "a 4-sample ramp, full frame and ROI");
        addField("effect.MaskDistanceKernel",
                 {bloom::runtime::MaskDistanceKernel{3.0}, bloom::runtime::MaskDistanceKernel{0.5}},
                 526000, "production mask distance: both exact passes over the padded mask");
        std::uint64_t base = 527000;
        for (const auto op : bloom::core::field::kFieldOps) {
            addField(std::string{"feature.field.op."} +
                         std::string{bloom::core::field::fieldOpName(op)},
                     {fieldKernel(op, base, 0), fieldKernel(op, base + 100, 1),
                      fieldKernel(op, base + 200, 2)},
                     base + 300,
                     "production field operation " +
                         std::string{bloom::core::field::fieldOpName(op)} +
                         ": three variants, full frame and ROI");
            base += 1000;
        }
        addField("feature.field.images",
                 {fieldKernel(FieldOp::Image, base, 0), fieldKernel(FieldOp::Image, base + 100, 1),
                  fieldKernel(FieldOp::Image, base + 200, 2)},
                 base + 300, "production field image slots: the atlas with every filter and edge");
        base += 1000;
        addField("feature.field.supersample",
                 {fieldKernel(FieldOp::Noise, base, 2, 4), fieldKernel(FieldOp::Pattern, base + 100, 1, 16)},
                 base + 300, "production field sub-samples: 4 and 16 per pixel");
        base += 1000;
        addField("feature.field.output_modes",
                 {fieldKernel(FieldOp::Noise, base, 1), fieldKernel(FieldOp::Arrow, base + 100),
                  fieldKernel(FieldOp::Ramp, base + 200), fieldKernel(FieldOp::Force, base + 300)},
                 base + 400, "production field output modes: Raw scalar, Mask, Colour and vector");
        base += 1000;
        const std::vector<std::pair<std::string_view, std::vector<FieldOp>>> nodes{
            {bloom::document::kFieldNoiseNodeType, {FieldOp::Noise}},
            {bloom::document::kFieldVoronoiNodeType, {FieldOp::Voronoi}},
            {bloom::document::kFieldRandomNodeType, {FieldOp::Random}},
            {bloom::document::kFieldPatternNodeType, {FieldOp::Pattern}},
            {bloom::document::kFieldGradientNodeType, {FieldOp::Gradient}},
            {bloom::document::kFieldShapeDistanceNodeType, {FieldOp::Shape}},
            {bloom::document::kFieldFromImageNodeType, {FieldOp::Image}},
            {bloom::document::kFieldForceNodeType, {FieldOp::Force}},
            {bloom::document::kFieldMathNodeType, {FieldOp::Math}},
            {bloom::document::kFieldRemapNodeType, {FieldOp::Remap}},
            {bloom::document::kFieldTransformNodeType, {FieldOp::Transform}},
            {bloom::document::kFieldDerivativeNodeType, {FieldOp::Derivative}},
            {bloom::document::kFieldCombineNodeType, {FieldOp::Combine}},
            {bloom::document::kFieldSeparateNodeType, {FieldOp::Swizzle}},
            {bloom::document::kFieldRasterizeNodeType,
             {FieldOp::Ramp, FieldOp::Arrow, FieldOp::Streak}}};
        // #486 Lens Distortion: its image output undistorting and redistorting, and its ST map in
        // both modes, the programs its lowering builds.
        addField("node.bloom.lens-distortion",
                 {bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::lensDistortionProgram(base, false, false), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::lensDistortionProgram(base + 100, false, true), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::lensDistortionProgram(base + 200, true, false), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::lensDistortionProgram(base + 300, true, true), 1}},
                 base + 400,
                 "production Lens Distortion lowering: the warped image in both modes and its ST "
                 "map in both modes, full frame and ROI");
        base += 1000;
        // #486 Chromatic Aberration: the one program its lowering builds, three premultiplied image
        // reads at the red, green and blue positions.
        addField("node.bloom.chromatic-aberration",
                 {bloom::runtime::FieldRasterKernel{
                     bloom::field_test_programs::chromaticAberrationProgram(base), 1}},
                 base + 100,
                 "production Chromatic Aberration lowering: red and blue read at opposite radial "
                 "offsets, full frame and ROI");
        base += 1000;
        // #568 Rolling Shutter: the one program its lowering builds, a picture read at a position
        // moved by a quadratic camera term and a flow image.
        addField("node.bloom.rolling-shutter",
                 {bloom::runtime::FieldRasterKernel{
                     bloom::field_test_programs::rollingShutterProgram(base), 1}},
                 base + 100,
                 "production Rolling Shutter lowering: camera and flow terms moving the read "
                 "position, full frame and ROI");
        base += 1000;
        // #348 Corner Pin, ST Map and Displace: the programs their lowerings build, every filter
        // and edge rule and channel choice, the picture read through the atlas beside a map.
        addField("node.bloom.corner-pin",
                 {bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::cornerPinProgram(base, 0), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::cornerPinProgram(base + 100, 1), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::cornerPinProgram(base + 200, 2), 1}},
                 base + 300,
                 "production Corner Pin lowering: the data window pinned to a moved quad, "
                 "bilinear, nearest and bicubic, full frame and ROI");
        base += 1000;
        // #513 Stabilize and Match Move: the picture moved by two points, both modes, every filter
        // and edge rule, full frame and ROI.
        addField("node.bloom.stabilize",
                 {bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::stabilizeProgram(base, 0, 0, 1), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::stabilizeProgram(base + 100, 1, 1, 0), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::stabilizeProgram(base + 200, 2, 0, 3), 1}},
                 base + 300,
                 "production Stabilize lowering: the picture moved by a tracked point, held "
                 "still or carried along, every filter and edge rule, full frame and ROI");
        base += 1000;
        // #511 Tile & Mirror: the programs its four modes lower to, every filter and edge rule,
        // Mirror Edges and Clamp to Output, either mirror side and several segment counts, full frame
        // and ROI.
        addField("node.bloom.tile-mirror",
                 {bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::tileMirrorProgram(base, 0, 0, 1, 3), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::tileMirrorProgram(base + 100, 0, 1, 2, 0), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::tileMirrorProgram(base + 200, 1, 2), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::tileMirrorProgram(base + 300, 2, 0, 3, 0), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::tileMirrorProgram(base + 400, 2, 1, 0, 1), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::tileMirrorProgram(base + 500, 3, 0, 2, 6), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::tileMirrorProgram(base + 600, 3, 2, 3, 3), 1}},
                 base + 700,
                 "production Tile & Mirror lowering: tiles, wrap offset, mirror line and "
                 "kaleidoscope, every filter and edge rule, full frame and ROI");
        base += 1000;
        addField("node.bloom.st-map",
                 {bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::stMapProgram(base, 0, 1, 0, 1), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::stMapProgram(base + 100, 1, 2, 2, 3), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::stMapProgram(base + 200, 2, 3, 1, 0), 1}},
                 base + 300,
                 "production ST Map lowering: a map read as data and the picture read at its "
                 "positions, every filter and edge rule, full frame and ROI");
        base += 1000;
        addField("node.bloom.displace",
                 {bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::displaceProgram(base, 0, 1), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::displaceProgram(base + 100, 1, 2), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::displaceProgram(base + 200, 2, 3), 1}},
                 base + 300,
                 "production Displace lowering: the picture read where a map moved it, every "
                 "filter and edge rule, full frame and ROI");
        base += 1000;
        // #587 Test Card: every card, as the raster its lowering builds (four sub-samples, a variant
        // and option where the card has them, the label on every other card).
        {
            std::vector<bloom::runtime::ImageEffectKernel> cards;
            for (std::int64_t card = 0; card < 16; ++card)
                cards.emplace_back(bloom::runtime::FieldRasterKernel{
                    bloom::field_test_programs::testCardProgram(
                        base + static_cast<std::uint64_t>(card) * 100, card, card == 3 ? 2 : 14,
                        card == 9 ? 15 : (card == 4 ? 2 : 0), card % 2 == 0),
                    4});
            addField("node.bloom.test-card", std::move(cards), base + 1700,
                     "production Test Card lowering: all sixteen cards, full frame and ROI");
            base += 2000;
        }
        // #524 Four-Color Gradient: the one program its lowering builds, at a soft, the default
        // and a sharp blend.
        addField("node.bloom.four-color-gradient",
                 {bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::fourColorGradientProgram(
                          base, 0.5,
                          {bloom::document::Vec2d{-5.5, 3.5}, bloom::document::Vec2d{5.5, 3.5},
                           bloom::document::Vec2d{-5.5, -3.5}, bloom::document::Vec2d{2.0, -1.0}},
                          {{{1.5, 0.0, 0.0, 1.0}, {0.0, 1.0, 0.25, 1.0}, {0.0, 0.25, 1.0, 0.5},
                            {1.0, 1.0, 0.0, 1.0}}}),
                      1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::fourColorGradientProgram(
                          base + 100, 2.0,
                          {bloom::document::Vec2d{-5.5, 3.5}, bloom::document::Vec2d{5.5, 3.5},
                           bloom::document::Vec2d{-5.5, -3.5}, bloom::document::Vec2d{2.0, -1.0}},
                          {{{1.5, 0.0, 0.0, 1.0}, {0.0, 1.0, 0.25, 1.0}, {0.0, 0.25, 1.0, 0.5},
                            {1.0, 1.0, 0.0, 1.0}}}),
                      1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::fourColorGradientProgram(
                          base + 200, 8.0,
                          {bloom::document::Vec2d{-5.5, 3.5}, bloom::document::Vec2d{5.5, 3.5},
                           bloom::document::Vec2d{-5.5, -3.5}, bloom::document::Vec2d{2.0, -1.0}},
                          {{{1.5, 0.0, 0.0, 1.0}, {0.0, 1.0, 0.25, 1.0}, {0.0, 0.25, 1.0, 0.5},
                            {1.0, 1.0, 0.0, 1.0}}}),
                      1}},
                 base + 300,
                 "production Four-Color Gradient lowering: soft, default and sharp blends, full "
                 "frame and ROI");
        base += 1000;
        // #508 Light Rays: the one program its lowering builds, rays gathered from the input's
        // luma and from its alpha and added to the input's premultiplied texel.
        addField("node.bloom.light-rays",
                 {bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::lightRaysProgram(base, false), 1},
                  bloom::runtime::FieldRasterKernel{
                      bloom::field_test_programs::lightRaysProgram(base + 50, true), 1}},
                 base + 100,
                 "production Light Rays lowering: rays from luminance and from alpha, full frame "
                 "and ROI");
        base += 1000;
        for (const auto& [type, ops] : nodes) {
            std::vector<bloom::runtime::ImageEffectKernel> kernels;
            for (const auto op : ops) {
                kernels.push_back(fieldKernel(op, base, 1));
                base += 100;
            }
            addField("node." + std::string{type}, std::move(kernels), base,
                     "production " + std::string{type} + " lowering, full frame and ROI");
            base += 1000;
        }
    }
    // #546: gradient blends through native GradientV1 (linear and radial generators) and the
    // resident Colorize program.
    {
        using bloom::document::GradientColorMode;
        using bloom::document::GradientHueDirection;
        using bloom::document::GradientInterpolation;
        using bloom::document::GradientModes;
        const auto runBlends = [runGrades, colorize](const std::vector<GradientModes>& blends,
                                                     const std::uint64_t idBase,
                                                     std::string evidence) {
            FixtureRun combined;
            combined.prepared = true;
            std::uint64_t id = idBase;
            std::vector<std::function<ImageEffectKernel(std::uint64_t)>> maps;
            for (const auto& blend : blends) {
                for (const bool radial : {false, true}) {
                    auto source = gradient(radial, id);
                    std::get<bloom::document::GradientValue>(source.gradient->stops.source).modes =
                        blend;
                    auto frame = runBuilder(withSource(source, id));
                    id += 100;
                    combined.prepared = combined.prepared && frame.prepared;
                    for (auto& value : frame.frames)
                        combined.frames.push_back(std::move(value));
                }
                maps.push_back(colorize(6, blend));
            }
            auto mapped = runGrades(maps, id, {});
            combined.prepared = combined.prepared && mapped.prepared;
            for (auto& value : mapped.frames)
                combined.frames.push_back(std::move(value));
            combined.evidence = std::move(evidence);
            return combined;
        };
        add(
            "feature.gradient.interpolation", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime gradient native batch and colorize tests",
            [runBlends] {
                std::vector<GradientModes> blends;
                for (const auto interpolation : bloom::document::kGradientInterpolations)
                    blends.push_back(
                        {interpolation, GradientColorMode::Rgb, GradientHueDirection::Near});
                return runBlends(blends, 546000,
                                 "production gradient interpolations: Constant, Linear, Ease, "
                                 "Cardinal, B-Spline and Exponential on Linear and Radial "
                                 "Gradient and a Colorize Gradient Map, full frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
        add(
            "feature.gradient.color_mode", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime gradient native batch and colorize tests",
            [runBlends] {
                std::vector<GradientModes> blends;
                for (const auto colorMode : {GradientColorMode::Hsv, GradientColorMode::Hsl})
                    for (const auto hue : bloom::document::kGradientHueDirections)
                        blends.push_back({GradientInterpolation::Linear, colorMode, hue});
                return runBlends(blends, 550000,
                                 "production gradient colour modes: HSV and HSL with Near, Far, "
                                 "Clockwise and Counter-clockwise hues on Linear and Radial "
                                 "Gradient and a Colorize Gradient Map, full frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
    }
    // #505 Vignette: both shapes, both modes, roundness both ways, a moved centre and highlight
    // protection.
    const auto vignette = [](const int variant) {
        return [=](const std::uint64_t idBase) -> ImageEffectKernel {
            using bloom::runtime::VignetteMode;
            using bloom::runtime::VignetteShape;
            bloom::runtime::VignetteKernel kernel;
            kernel.shape = variant % 2 == 1 ? VignetteShape::Rectangular : VignetteShape::Elliptical;
            kernel.mode = variant >= 2 ? VignetteMode::Exposure : VignetteMode::Multiply;
            kernel.amount = {ParameterId::fromRaw(idBase + 60), variant == 1 ? -0.5 : 0.9};
            kernel.exposure = {ParameterId::fromRaw(idBase + 61), variant == 3 ? 1.25 : -3.0};
            kernel.midpoint = {ParameterId::fromRaw(idBase + 62), 0.25};
            kernel.roundness = {ParameterId::fromRaw(idBase + 63), variant == 1 ? -0.4 : 0.7};
            kernel.feather = {ParameterId::fromRaw(idBase + 64), 0.6};
            kernel.highlightProtection = {ParameterId::fromRaw(idBase + 65),
                                          variant == 2 ? 0.75 : 0.0};
            kernel.center = {ParameterId::fromRaw(idBase + 66),
                             bloom::document::Vec2d{variant == 0 ? 0.0 : 3.0, -2.0}};
            return ImageEffectKernel{kernel};
        };
    };
    add(
        "effect.VignetteKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime vignette tests",
        [runGrades, vignette] {
            return runGrades({vignette(0), vignette(1), vignette(2), vignette(3)}, 505000,
                             "production Vignette: elliptical and rectangular, multiply and "
                             "exposure, a moved centre and highlight protection over HDR and "
                             "signed values, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.vignette", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime vignette tests",
        [runGrades, vignette] {
            return runGrades({vignette(0), vignette(2)}, 505600,
                             "production Vignette node: an elliptical multiply and a protected "
                             "exposure falloff, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #481 Image Math, Image Map Range and Image Clamp over the HDR, signed, partial-alpha linear
    // gradient, full frame and through an ROI: the one resident program over its own table, and
    // Image Math with a second image (a signed HDR solid) through the atlas of both.
    const auto runPixelMath = [](const std::vector<bloom::runtime::PixelMathKernel>& kernels,
                                 std::uint64_t idBase, std::string evidence) {
        FixtureRun combined;
        combined.prepared = true;
        for (const auto& kernel : kernels) {
            auto definition = (kernel.second ? imageCombinePlan(bloom::runtime::ImageCombineMode::Mix,
                                                                idBase)
                                             : bloom::gpu_coverage_plans::effectPlan(
                                                   ImageEffectKernel{kernel}, idBase))
                                  ->copyDefinition();
            definition.operations[0] = bloom::gpu_coverage_plans::gradient(false, idBase + 50);
            if (kernel.second)
                std::get<CompiledImageEffect>(definition.operations[2]).kernel = kernel;
            const auto plan = std::make_shared<const bloom::runtime::CompiledCompositionPlan>(
                std::move(definition));
            idBase += 100;
            auto request = requestFor(*plan);
            auto frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
            const auto roiWindow = bloom::render::ImageWindow::create(5, 3, 6, 4);
            request.roi = *roiWindow.value();
            frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
        }
        combined.evidence = std::move(evidence);
        return combined;
    };
    {
        namespace pm = bloom::pixel_math_test;
        namespace doc = bloom::document;
        using Op = bloom::runtime::PixelMathOperation;
        const auto mathKernels = [] {
            std::vector<bloom::runtime::PixelMathKernel> kernels;
            std::uint64_t id = 481000;
            for (const auto op : {Op::Add, Op::Subtract, Op::Multiply, Op::Divide, Op::Power,
                                  Op::Minimum, Op::Maximum, Op::Absolute}) {
                kernels.push_back(
                    pm::math(id, op, op == Op::Power ? 2.0 : 0.5, doc::kImagePixelColor, true));
                id += 100;
            }
            kernels.push_back(pm::math(id, Op::Multiply, 0.5, doc::kImagePixelColorAndAlpha, false));
            kernels.push_back(pm::math(id + 100, Op::Add, 0.25, doc::kImagePixelAlpha, true));
            kernels.push_back(pm::math(id + 200, Op::Subtract, 0.0, doc::kImagePixelGreen, true,
                                       true));
            kernels.push_back(pm::math(id + 300, Op::Divide, 0.0, doc::kImagePixelColor, false,
                                       true));
            return kernels;
        };
        const auto rangeKernels = [] {
            return std::vector<bloom::runtime::PixelMathKernel>{
                pm::mapRange(481800, 0.0, 4.0, 0.0, 1.0, false, doc::kImagePixelColor, true),
                pm::mapRange(481900, -1.0, 2.0, 5.0, -5.0, true, doc::kImagePixelColorAndAlpha,
                             false),
                pm::mapRange(482000, 1.0, 1.0, 0.25, 0.75, false, doc::kImagePixelRed, true)};
        };
        const auto clampKernels = [] {
            return std::vector<bloom::runtime::PixelMathKernel>{
                pm::clampKernel(482100, {0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0}, true),
                pm::clampKernel(482200, {-0.5, 0.5, 0.25, 2.0, 1.0, 0.0, 0.1, 0.9}, false)};
        };
        // #349 Image Compare: each mode, and the split at an angle, over the second image.
        const auto compareKernels = [] {
            return std::vector<bloom::runtime::PixelMathKernel>{
                pm::compare(349000, doc::kImageCompareDifference, 3.0, 0.5, 0.0, 4.0),
                pm::compare(349100, doc::kImageCompareSplit, 1.0, 0.35, 25.0, 4.0),
                pm::compare(349200, doc::kImageCompareSplit, 1.0, 0.8, -140.0, 4.0),
                pm::compare(349300, doc::kImageCompareCheckerboard, 1.0, 0.5, 0.0, 3.0)};
        };
        add(
            "effect.PixelMathKernel", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime pixel math tests",
            [runPixelMath, mathKernels, rangeKernels, clampKernels, compareKernels] {
                auto kernels = mathKernels();
                for (auto& kernel : rangeKernels())
                    kernels.push_back(std::move(kernel));
                for (auto& kernel : clampKernels())
                    kernels.push_back(std::move(kernel));
                for (auto& kernel : compareKernels())
                    kernels.push_back(std::move(kernel));
                return runPixelMath(kernels, 481000,
                                    "production Image Math, Map Range, Clamp and Compare: every "
                                    "operation, straight and stored values, each channel choice "
                                    "and a second image over HDR and signed values, full frame and "
                                    "ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
        add(
            "node.bloom.image-math", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime pixel math tests",
            [runPixelMath, mathKernels] {
                return runPixelMath(mathKernels(), 483000,
                                    "production Image Math node: every operation with a number "
                                    "and with a second image, full frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
        add(
            "node.bloom.image-map-range", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime pixel math tests",
            [runPixelMath, rangeKernels] {
                return runPixelMath(rangeKernels(), 484000,
                                    "production Image Map Range node: a normalising map, a "
                                    "reversed clamped map and an empty input range, full frame and "
                                    "ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
        add(
            "node.bloom.image-clamp", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime pixel math tests",
            [runPixelMath, clampKernels] {
                return runPixelMath(clampKernels(), 485000,
                                    "production Image Clamp node: the unit range and per-channel "
                                    "limits, full frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
    }
    {
        namespace pm = bloom::pixel_math_test;
        namespace doc = bloom::document;
        add(
            "node.bloom.image-compare", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime histogram and compare tests",
            [runPixelMath] {
                return runPixelMath(
                    {pm::compare(349400, doc::kImageCompareDifference, 3.0, 0.5, 0.0, 4.0),
                     pm::compare(349500, doc::kImageCompareSplit, 1.0, 0.35, 25.0, 4.0),
                     pm::compare(349600, doc::kImageCompareCheckerboard, 1.0, 0.5, 0.0, 3.0)},
                    349400,
                    "production Image Compare node: the difference, a split at an angle and the "
                    "checkerboard of two images, full frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
    }
    // #349 Histogram: the tile counts, their sums and the plot through the production chain over
    // the HDR, signed, partial-alpha gradient, full frame and through an ROI.
    {
        namespace ht = bloom::histogram_test;
        namespace doc = bloom::document;
        const auto runHistogram = [](const std::vector<ht::HistogramValues>& variants,
                                     std::uint64_t idBase, std::string evidence) {
            FixtureRun combined;
            combined.prepared = true;
            for (const auto& values : variants) {
                const auto plan = ht::histogramPlan(values, idBase);
                idBase += 200;
                auto request = requestFor(*plan);
                auto run = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && run.prepared;
                for (auto& value : run.frames)
                    combined.frames.push_back(std::move(value));
                const auto roiWindow = bloom::render::ImageWindow::create(10, 6, 30, 40);
                request.roi = *roiWindow.value();
                run = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && run.prepared;
                for (auto& value : run.frames)
                    combined.frames.push_back(std::move(value));
            }
            combined.evidence = std::move(evidence);
            return combined;
        };
        ht::HistogramValues stored;
        stored.straight = false;
        stored.channels = doc::kHistogramRgb;
        ht::HistogramValues luma;
        luma.channels = doc::kHistogramLuminance;
        luma.logarithmic = true;
        ht::HistogramValues alpha;
        alpha.channels = doc::kHistogramAlpha;
        ht::HistogramValues hdr;
        hdr.channels = doc::kHistogramRed;
        hdr.rangeMin = -1.0;
        hdr.rangeMax = 4.0;
        hdr.bins = 128;
        add(
            "effect.HistogramKernel", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime histogram and compare tests",
            [runHistogram, stored, luma, alpha, hdr] {
                return runHistogram({ht::HistogramValues{}, stored, luma, alpha, hdr}, 349700,
                                    "production Histogram: every channel choice, straight and "
                                    "stored values, linear and logarithmic graphs and an HDR "
                                    "range over HDR and partial-alpha values, full frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
        add(
            "node.bloom.histogram", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime histogram and compare tests",
            [runHistogram] {
                return runHistogram({ht::HistogramValues{}}, 349900,
                                    "production Histogram node: the defaults over the gradient, "
                                    "full frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
    }
    // #349 Image Statistics: the tiles' reduction and their combination through the production chain
    // over the HDR, signed, partial-alpha gradient and a constant picture, requested whole and as a
    // statistics plan is run (the two-pixel result window). The node itself is a value lowering, so
    // it has no pixel-operation fixture of its own: the pixel work is this kernel.
    {
        namespace stt = bloom::statistics_test;
        namespace doc = bloom::document;
        const auto runStatistics = [](const std::vector<stt::StatisticsValues>& variants,
                                      std::uint64_t idBase, std::string evidence) {
            FixtureRun combined;
            combined.prepared = true;
            for (const auto& values : variants) {
                const auto plan = stt::statisticsPlan(values, idBase);
                idBase += 200;
                auto request = requestFor(*plan);
                auto run = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && run.prepared;
                for (auto& value : run.frames)
                    combined.frames.push_back(std::move(value));
                const auto resultWindow = bloom::render::ImageWindow::create(0, 0, 2, 1);
                request.roi = *resultWindow.value();
                run = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && run.prepared;
                for (auto& value : run.frames)
                    combined.frames.push_back(std::move(value));
            }
            combined.evidence = std::move(evidence);
            return combined;
        };
        stt::StatisticsValues stored;
        stored.channel = doc::kStatisticsRed;
        stored.straight = false;
        stt::StatisticsValues alpha;
        alpha.channel = doc::kStatisticsAlpha;
        stt::StatisticsValues opaque;
        opaque.channel = doc::kStatisticsBlue;
        opaque.opaqueOnly = true;
        stt::StatisticsValues flat;
        flat.gradient = false;
        flat.channel = doc::kStatisticsGreen;
        add(
            "effect.StatisticsKernel", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime image statistics tests",
            [runStatistics, stored, alpha, opaque, flat] {
                return runStatistics({stt::StatisticsValues{}, stored, alpha, opaque, flat}, 349950,
                                     "production Image Statistics: luminance, stored red, alpha, "
                                     "blue over the pixels with alpha and a constant picture, over "
                                     "HDR and partial-alpha values, whole and as the two-pixel "
                                     "result");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
    }
    // #347 Audio Waveform and Audio Spectrum: the prepared analysis of a generated WAV drawn by the
    // one plot pass, the HDR/partial-alpha colors, both axes, separate lanes, silence and an ROI, on
    // the production preparation chain and the CPU reference.
    {
        namespace apt = bloom::audio_plot_test;
        const auto runAudioPlot = [](const std::vector<apt::AudioPlotValues>& variants,
                                     std::uint64_t idBase, std::string evidence) {
            FixtureRun combined;
            combined.prepared = true;
            const bloom::runtime::CpuCompositionEvaluator evaluator;
            const auto& directory = apt::sharedAudioFixture().directory;
            for (const auto& values : variants) {
                const auto plan = apt::audioPlotPlan(values, idBase);
                idBase += 200;
                auto run = runMediaBuilder(plan, evaluator, directory);
                combined.prepared = combined.prepared && run.prepared;
                for (auto& value : run.frames)
                    combined.frames.push_back(std::move(value));
                if (!run.prepared)
                    combined.evidence += run.evidence + "; ";
            }
            if (combined.prepared)
                combined.evidence = std::move(evidence);
            return combined;
        };
        apt::AudioPlotValues waveform;
        apt::AudioPlotValues separate;
        separate.separate = true;
        separate.values = {1.0, 2.0, 0.0, 0.0};
        separate.color = {3.0, 0.0, 0.5, 0.75};
        apt::AudioPlotValues silent;
        silent.withSource = false;
        apt::AudioPlotValues linear = apt::spectrumValues();
        apt::AudioPlotValues logarithmic = apt::spectrumValues();
        logarithmic.logarithmic = true;
        logarithmic.fftSize = 4096;
        logarithmic.values = {30.0, 16000.0, -60.0, -6.0};
        logarithmic.background = {0.25, 0.25, 0.25, 1.0};
        add(
            "effect.AudioPlotKernel", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime audio analysis tests",
            [runAudioPlot, waveform, separate, silent, linear, logarithmic] {
                return runAudioPlot({waveform, separate, silent, linear, logarithmic}, 347800,
                                    "production Audio Waveform and Audio Spectrum: mixed and "
                                    "separate lanes, linear and logarithmic axes, an HDR "
                                    "partial-alpha line and silence");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
        add(
            "node.bloom.audio-waveform", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime audio analysis tests",
            [runAudioPlot, waveform] {
                return runAudioPlot({waveform}, 347900,
                                    "production Audio Waveform node: the defaults over a generated "
                                    "stereo WAV");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
        add(
            "node.bloom.audio-spectrum", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime audio analysis tests",
            [runAudioPlot, linear] {
                return runAudioPlot({linear}, 348100,
                                    "production Audio Spectrum node: the defaults over a generated "
                                    "stereo WAV");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
    }
    // #483 Depth effects: Z Defocus and Depth Fog through the production chains (the depth map,
    // the tile maximum, the prefilter, the bokeh atlas and the gather; the fog factor and its Mix)
    // over the HDR, partial-alpha gradient beauty and three exact depth bands, full frame and ROI.
    const auto runDepth = [](const std::vector<bloom::depth_test::DepthValues>& variants,
                             std::uint64_t idBase, std::string evidence) {
        FixtureRun combined;
        combined.prepared = true;
        for (const auto& values : variants) {
            const auto plan = bloom::depth_test::depthPlan(values, idBase).plan;
            idBase += 100;
            auto request = requestFor(*plan);
            auto frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
            const auto roiWindow = bloom::render::ImageWindow::create(5, 3, 9, 7);
            request.roi = *roiWindow.value();
            frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
        }
        combined.evidence = std::move(evidence);
        return combined;
    };
    {
        using bloom::depth_test::DepthValues;
        using bloom::depth_test::Effect;
        DepthValues lens;
        DepthValues uniform;
        uniform.mode = bloom::document::kDefocusModeUniform;
        uniform.shape = bloom::document::kBokehPolygon;
        DepthValues bokeh;
        bokeh.shape = bloom::document::kBokehImage;
        bokeh.bokeh = true;
        bokeh.highlightGain = 2.0;
        bokeh.chromatic = 0.5;
        DepthValues physical;
        physical.mode = bloom::document::kDefocusModePhysical;
        physical.quality = bloom::document::kDefocusQualityHigh;
        physical.focusFrom = bloom::document::kFocusFromPoint;
        DepthValues mistLens;
        mistLens.mist = true;
        mistLens.nearDepth = 1.0;
        mistLens.farDepth = 40.0;
        DepthValues fog;
        fog.effect = Effect::Fog;
        DepthValues fogCurve = fog;
        fogCurve.falloff = bloom::document::kFogFalloffCurve;
        fogCurve.mist = true;
        DepthValues fogImage = fog;
        fogImage.fogImage = true;
        fogImage.fillEmpty = true;
        const auto addDepth = [&add, runDepth](std::string id, std::vector<DepthValues> variants,
                                               const std::uint64_t idBase, std::string evidence) {
            add(
                std::move(id), GpuCoverageFixtureCriterion::Prepared,
                "src/runtime depth effect tests",
                [runDepth, variants = std::move(variants), idBase,
                 evidence = std::move(evidence)] { return runDepth(variants, idBase, evidence); },
                {}, FixturePrerequisite::PackagedShaderTools);
        };
        addDepth("effect.DepthMapKernel", {lens, physical, mistLens, fog, fogCurve}, 483000,
                 "production depth map: Z, mist and physical circles of confusion, a picked "
                 "focus point, linear and curve fog factors, full frame and ROI");
        addDepth("effect.DefocusKernel", {lens, uniform, bokeh, physical}, 483600,
                 "production defocus gather: a depth disk, a uniform polygon, an image bokeh with "
                 "highlight boost and chromatic bokeh through the atlas and a high-quality "
                 "physical lens, full frame and ROI");
        addDepth("node.bloom.z-defocus", {lens, bokeh}, 484200,
                 "production Z Defocus node: a depth disk and an image bokeh, full frame and ROI");
        addDepth("node.bloom.depth-fog", {fog, fogImage}, 484600,
                 "production Depth Fog node: a colour fog and a fog image with empty depth "
                 "filled, full frame and ROI");
    }
    // #507 Lens Flare: every element, the probes through the atlas and a small input padded to the
    // frame, full frame and ROI.
    const auto runFlare = [](const std::vector<bloom::lens_flare_test::FlareValues>& variants,
                             std::uint64_t idBase, std::string evidence) {
        FixtureRun combined;
        combined.prepared = true;
        for (const auto& values : variants) {
            const auto plan = bloom::lens_flare_test::flarePlan(values, idBase);
            idBase += 200;
            auto request = requestFor(*plan);
            auto frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
            const auto roiWindow = bloom::render::ImageWindow::create(5, 3, 9, 7);
            request.roi = *roiWindow.value();
            frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
        }
        combined.evidence = std::move(evidence);
        return combined;
    };
    {
        using bloom::lens_flare_test::everyElement;
        using bloom::lens_flare_test::FlareValues;
        FlareValues probed = everyElement();
        probed.scalars["sourceBrightness"] = 0.5;
        probed.matteWidth = 10;
        FlareValues padded;
        padded.smallInput = true;
        FlareValues projected;
        projected.integers = {{"lightMode", bloom::document::kLensFlareLight3D}};
        projected.scalars = {{"lightDistance", 200.0}, {"cameraZoom", 800.0}};
        add(
            "effect.LensFlareKernel", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime lens flare tests",
            [runFlare, probed, padded, projected] {
                return runFlare({FlareValues{}, everyElement(), probed, padded, projected}, 507000,
                                "production Lens Flare: the default stack, every element at its "
                                "largest counts, the source and occlusion probes through the atlas, "
                                "a small input padded to the frame and a projected 3D light, full "
                                "frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
        add(
            "node.bloom.lens-flare", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime lens flare tests",
            [runFlare, probed] {
                return runFlare({FlareValues{}, probed}, 508200,
                                "production Lens Flare node: the default stack and a flare reading "
                                "its source and an occlusion matte, full frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
    }
    // #486 Film Grain: every path of the program, full frame and ROI, on a moved frame.
    const auto runGrain = [](const std::vector<bloom::film_grain_test::GrainValues>& variants,
                             std::uint64_t idBase, std::string evidence) {
        FixtureRun combined;
        combined.prepared = true;
        for (const auto& values : variants) {
            const auto plan = bloom::film_grain_test::grainPlan(values, idBase);
            idBase += 200;
            auto request = requestFor(*plan);
            request.time = *bloom::core::RationalTime::create(7, 24);
            auto frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
            const auto roiWindow = bloom::render::ImageWindow::create(5, 3, 9, 7);
            request.roi = *roiWindow.value();
            frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
        }
        combined.evidence = std::move(evidence);
        return combined;
    };
    {
        using bloom::film_grain_test::everyPath;
        using bloom::film_grain_test::GrainValues;
        GrainValues held = everyPath();
        held.animation = bloom::document::kFilmGrainHeld;
        add(
            "effect.FilmGrainKernel", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime film grain tests",
            [runGrain, held] {
                return runGrain({GrainValues{}, bloom::film_grain_test::everyPath(), held,
                                 bloom::film_grain_test::everyPathMonochrome(),
                                 bloom::film_grain_test::everyPathSensor(),
                                 bloom::film_grain_test::fixedPatternOnly()},
                                486000,
                                "production Film Grain: the default, every channel and response "
                                "distinct at a larger size, held grain and Monochrome, and Sensor "
                                "Noise with every term and with a fixed pattern alone, over HDR "
                                "and partial alpha, on a moved frame, full frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
        add(
            "node.bloom.film-grain", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime film grain tests",
            [runGrain] {
                return runGrain({GrainValues{}, bloom::film_grain_test::everyPath(),
                                 bloom::film_grain_test::everyPathMonochrome(),
                                 bloom::film_grain_test::everyPathSensor()},
                                487000,
                                "production Film Grain node: the default, strong distinct "
                                "channels, Monochrome and Sensor Noise, on a moved frame, full "
                                "frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
    }
    // #563 Codec Degrade: a frame inside a GOP (the chain of predicted steps), a keyframe, 16 px
    // blocks without chroma bleed, full frame and ROI.
    {
        using bloom::codec_test::CodecValues;
        const auto runCodec = [](const std::vector<std::pair<CodecValues, std::int64_t>>& variants,
                                 std::uint64_t idBase, std::string evidence) {
            FixtureRun combined;
            combined.prepared = true;
            for (const auto& [values, frame] : variants) {
                const auto plan = bloom::codec_test::codecPlan(values, idBase).plan;
                idBase += 200;
                auto request = requestFor(*plan);
                request.time = *bloom::core::RationalTime::create(frame, 24);
                auto run = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && run.prepared;
                for (auto& value : run.frames)
                    combined.frames.push_back(std::move(value));
                const auto roiWindow = bloom::render::ImageWindow::create(6, 4, 20, 14);
                request.roi = *roiWindow.value();
                run = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && run.prepared;
                for (auto& value : run.frames)
                    combined.frames.push_back(std::move(value));
            }
            combined.evidence = std::move(evidence);
            return combined;
        };
        CodecValues wide;
        wide.blockPixels = 16;
        wide.chromaBleed = false;
        wide.gopFrames = 6;
        add(
            "effect.CodecDegradeKernel", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime codec degrade tests",
            [runCodec, wide] {
                return runCodec({{CodecValues{}, 6}, {CodecValues{}, 8}, {wide, 7}}, 563000,
                                "production Codec Degrade: a frame inside a GOP, a keyframe and "
                                "16 px blocks without chroma bleed, over an HDR moving picture, "
                                "full frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
        add(
            "node.bloom.codec-degrade", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime codec degrade tests",
            [runCodec] {
                return runCodec({{CodecValues{}, 6}}, 564000,
                                "production Codec Degrade node: the defaults on a moving frame "
                                "inside a GOP, full frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
    }
    // #566 Auto Exposure: a flare mid-window (the reduction, the filter over the window's means and
    // the gain), a warm shot and a window reaching before the first frame, full frame and ROI.
    {
        using bloom::exposure_test::ExposureValues;
        const auto runExposure = [](const std::vector<std::pair<ExposureValues, std::int64_t>>& variants,
                                    std::uint64_t idBase, std::string evidence) {
            FixtureRun combined;
            combined.prepared = true;
            for (const auto& [values, frame] : variants) {
                const auto plan = bloom::exposure_test::exposurePlan(values, idBase).plan;
                idBase += 200;
                auto request = requestFor(*plan);
                request.time = *bloom::core::RationalTime::create(frame, 24);
                auto run = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && run.prepared;
                for (auto& value : run.frames)
                    combined.frames.push_back(std::move(value));
                const auto roiWindow = bloom::render::ImageWindow::create(6, 4, 20, 14);
                request.roi = *roiWindow.value();
                run = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && run.prepared;
                for (auto& value : run.frames)
                    combined.frames.push_back(std::move(value));
            }
            combined.evidence = std::move(evidence);
            return combined;
        };
        ExposureValues warm;
        warm.steady = true;
        warm.before = {0.3, 0.2, 0.1};
        warm.lookBack = 8;
        ExposureValues start;
        start.lookBack = 40;
        add(
            "effect.AutoExposureKernel", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime auto exposure tests",
            [runExposure, warm, start] {
                return runExposure({{ExposureValues{}, 15}, {warm, 3}, {start, 6}}, 566000,
                                   "production Auto Exposure: a flare mid-window, a warm shot and "
                                   "a window reaching before the first frame, full frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
        add(
            "node.bloom.auto-exposure", GpuCoverageFixtureCriterion::Prepared,
            "src/runtime auto exposure tests",
            [runExposure] {
                return runExposure({{ExposureValues{}, 15}}, 567000,
                                   "production Auto Exposure node: the defaults on a flare "
                                   "mid-window, full frame and ROI");
            },
            {}, FixturePrerequisite::PackagedShaderTools);
    }
    // #484 Motion blur: every kernel, node and feature axis through the production chains
    // (motion_blur_test_support.hpp) over the moving HDR texture at frame 4, full frame and ROI.
    {
        using bloom::motion_test::MotionValues;
        using bloom::motion_test::Scene;
        namespace doc = bloom::document;
        const auto draft = doc::kMotionQualityDraft;
        const auto runMotion = [](const std::vector<MotionValues>& variants, std::uint64_t idBase,
                                  std::string evidence) {
            FixtureRun combined;
            combined.prepared = true;
            for (const auto& values : variants) {
                const auto plan = bloom::motion_test::motionPlan(values, idBase).plan;
                idBase += 100;
                auto request = requestFor(*plan);
                request.time = *bloom::core::RationalTime::create(4, 24);
                auto frame = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && frame.prepared;
                for (auto& value : frame.frames)
                    combined.frames.push_back(std::move(value));
                const auto roiWindow = bloom::render::ImageWindow::create(5, 3, 12, 9);
                request.roi = *roiWindow.value();
                frame = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && frame.prepared;
                for (auto& value : frame.frames)
                    combined.frames.push_back(std::move(value));
            }
            combined.evidence = std::move(evidence);
            return combined;
        };
        const auto addMotion = [&add, runMotion](std::string id, std::vector<MotionValues> variants,
                                                 const std::uint64_t idBase, std::string evidence) {
            add(
                std::move(id), GpuCoverageFixtureCriterion::Prepared,
                "src/runtime motion blur tests",
                [runMotion, variants = std::move(variants), idBase,
                 evidence = std::move(evidence)] { return runMotion(variants, idBase, evidence); },
                {}, FixturePrerequisite::PackagedShaderTools);
        };
        const MotionValues subFrame{.scene = Scene::SubFrame, .quality = draft};
        const MotionValues echo{.scene = Scene::Echo};
        const MotionValues directional{.scene = Scene::Directional, .quality = draft};
        const MotionValues spin{.scene = Scene::Spin, .quality = draft};
        const MotionValues zoom{.scene = Scene::Zoom, .quality = draft};
        const MotionValues shutter{.scene = Scene::TransformBlur, .quality = draft};
        const MotionValues skewedShutter{.scene = Scene::TransformBlur, .quality = draft,
                                         .skew = 25.0, .skewAxis = 10.0, .fadeOpacity = true};
        const MotionValues gather{.scene = Scene::VectorGather, .quality = draft};
        const MotionValues splat{.scene = Scene::VectorSplat, .quality = draft};
        const MotionValues layered{.scene = Scene::VectorLayered, .quality = draft};
        const MotionValues dis{.scene = Scene::FlowDis, .quality = draft};
        const MotionValues tvl1{.scene = Scene::FlowTvl1, .quality = draft};
        const MotionValues cut{.scene = Scene::FlowCut, .quality = draft};
        const MotionValues check{.scene = Scene::FlowConsistency, .quality = draft};
        const MotionValues flowBlur{.scene = Scene::FlowBlur, .quality = draft};
        const MotionValues flowMatte{.scene = Scene::FlowBlurMatte, .quality = draft};
        const MotionValues flowTemporal{.scene = Scene::FlowBlurTemporal, .quality = draft};
        // Held to 5 fps, frame 4 shows the plate at frame 0, where it is displaced.
        const MotionValues frameRate{.scene = Scene::FrameRate, .holdRate = 5000};
        auto irregularRate = frameRate;
        irregularRate.cadenceChance = 500;
        irregularRate.cadenceSeed = 3;
        auto posterized = echo;
        posterized.posterize = true;
        auto over = echo;
        over.op = bloom::runtime::AccumulateOperator::Over;
        auto screen = echo;
        screen.op = bloom::runtime::AccumulateOperator::Screen;
        auto maximum = echo;
        maximum.op = bloom::runtime::AccumulateOperator::Maximum;
        auto triangle = subFrame;
        triangle.curve = doc::kShutterCurveTriangle;
        auto customCurve = shutter;
        customCurve.curve = doc::kShutterCurveCustom;
        auto jittered = zoom;
        jittered.jitter = true;
        auto jitteredGather = gather;
        jitteredGather.jitter = true;
        jitteredGather.curved = true;
        jitteredGather.depth = true;
        auto depthSplat = splat;
        depthSplat.depth = true;
        auto validGather = gather;
        validGather.alphaValid = true;
        auto disMatte = dis;
        disMatte.matteSolve = true;
        auto tvl1Matte = tvl1;
        tvl1Matte.matteSolve = true;
        auto flowQuadratic = flowBlur;
        flowQuadratic.quality = doc::kMotionQualityHigh;
        addMotion("effect.TimeSampleKernel", {subFrame, echo}, 490000,
                  "production time samples: sub-frame and neighbour-frame Offset clocks of a "
                  "moving Transform 2D, full frame and ROI");
        addMotion("effect.TemporalAccumulateKernel", {subFrame, over}, 490400,
                  "production accumulator: a weighted add of sub-frames and Echo's over, full "
                  "frame and ROI");
        addMotion("effect.MotionPathBlurKernel", {directional, spin, zoom, shutter}, 490800,
                  "production path sampler: Directional, Spin, Zoom and the Transform 2D shutter, "
                  "full frame and ROI");
        addMotion("effect.VectorBlurKernel", {gather, splat, layered, check}, 491400,
                  "production vector stages: gather, splat, matte fill and consistency, full "
                  "frame and ROI");
        addMotion("effect.OpticalFlowKernel", {dis, tvl1, cut}, 492000,
                  "production optical flow: DIS, TV-L1 and the cut diagnostic, full frame and ROI");
        addMotion("feature.motion.time.sample", {echo}, 492400, "an upstream graph on Offset clocks");
        addMotion("feature.motion.time.posterize", {posterized}, 492600,
                  "Echo with Posterize Time on its Offset clocks");
        addMotion("feature.motion.time.cadence", {irregularRate}, 492700,
                  "Frame Rate Hold with a seeded irregular cadence on its Offset clock");
        addMotion("feature.motion.time.hold", {subFrame}, 492800,
                  "Motion Blur's held time samples");
        addMotion("feature.motion.accumulate.operators", {over, screen, maximum}, 493000,
                  "Echo's over, screen and maximum operators");
        addMotion("feature.motion.shutter.curve", {triangle, customCurve}, 493400,
                  "triangle and custom shutter curves");
        addMotion("feature.motion.shutter.jitter", {jittered, jitteredGather}, 493800,
                  "jittered path and gather samples");
        addMotion("feature.motion.path.directional", {directional}, 494200, "Directional path");
        addMotion("feature.motion.path.spin", {spin}, 494400, "Spin path");
        addMotion("feature.motion.path.zoom", {zoom}, 494600, "Zoom path");
        addMotion("feature.motion.path.transform", {shutter, customCurve, skewedShutter}, 494800,
                  "the Transform 2D shutter through animated rows, with skew and a fading opacity");
        addMotion("feature.motion.vector.gather", {gather, jitteredGather, validGather}, 495200,
                  "the reconstruction gather, curved, with depth, jitter and valid vectors");
        addMotion("feature.motion.vector.splat", {splat, depthSplat, flowBlur, flowQuadratic}, 495600,
                  "the soft splat, depth ordered, with neighbour frames and the quadratic path");
        addMotion("feature.motion.vector.fill", {layered, flowMatte}, 496000,
                  "the outside-in fill behind a matte");
        addMotion("feature.motion.vector.temporal_fill", {flowTemporal}, 496400,
                  "the temporal background fill");
        addMotion("feature.motion.vector.consistency", {check}, 496600,
                  "the forward-backward consistency check");
        addMotion("feature.motion.flow.dis", {dis, disMatte}, 496800, "DIS optical flow");
        addMotion("feature.motion.flow.tvl1", {tvl1, tvl1Matte}, 497000, "TV-L1 optical flow");
        addMotion("feature.motion.flow.cut", {cut}, 497200, "the cut diagnostic");
        addMotion("node.bloom.motion-blur", {subFrame, triangle}, 497400,
                  "production Motion Blur: sub-frame accumulation with its shutter curve");
        addMotion("node.bloom.vector-motion-blur", {gather, depthSplat, layered}, 497800,
                  "production Vector Motion Blur: gather, depth splat and matte layers");
        addMotion("node.bloom.optical-flow", {dis, tvl1, check, cut}, 498200,
                  "production Optical Flow: the solvers, consistency and the cut diagnostic");
        addMotion("node.bloom.optical-flow-motion-blur", {flowBlur, flowMatte, flowTemporal}, 498800,
                  "production Motion Blur (Optical Flow): no matte, matte, temporal fill");
        addMotion("node.bloom.echo", {echo, over, posterized}, 499400,
                  "production Echo: add, over and Posterize Time");
        addMotion("node.bloom.frame-rate-hold", {frameRate, irregularRate}, 499600,
                  "production Frame Rate Hold: a regular hold and a seeded irregular cadence");
        addMotion("node.bloom.directional-blur", {directional}, 499800, "production Directional");
        addMotion("node.bloom.spin-blur", {spin}, 500000, "production Spin");
        addMotion("node.bloom.zoom-blur", {zoom, jittered}, 500200, "production Zoom");
    }
    // #486 Sharpen: the blur and the unsharp dispatch through the production chain, full frame and ROI.
    const auto runSharpen = [](const std::vector<bloom::sharpen_test::SharpenValues>& variants,
                               std::uint64_t idBase, std::string evidence) {
        FixtureRun combined;
        combined.prepared = true;
        for (const auto& values : variants) {
            const auto plan = bloom::sharpen_test::sharpenPlan(values, idBase);
            idBase += 200;
            auto request = requestFor(*plan);
            auto frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
            const auto roiWindow = bloom::render::ImageWindow::create(5, 3, 9, 7);
            request.roi = *roiWindow.value();
            frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
        }
        combined.evidence = std::move(evidence);
        return combined;
    };
    add(
        "effect.SharpenKernel", GpuCoverageFixtureCriterion::Prepared, "src/runtime sharpen tests",
        [runSharpen] {
            return runSharpen({{}, {3.0, 12.0, 0.0}, {2.0, 3.0, 0.25}}, 488000,
                              "production Sharpen: the default, a strong wide one and a threshold, "
                              "over HDR and partial alpha, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.sharpen", GpuCoverageFixtureCriterion::Prepared, "src/runtime sharpen tests",
        [runSharpen] {
            return runSharpen({{}, {2.0, 3.0, 0.25}}, 489000,
                              "production Sharpen node: the default and a threshold, full frame and "
                              "ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #494 Grade: the wheels and primaries through the production builder's resident program.
    const auto grade = [](const std::array<double, 22> values) {
        return [=](const std::uint64_t idBase) -> ImageEffectKernel {
            bloom::runtime::GradeKernel kernel;
            for (std::size_t index = 0; index < values.size(); ++index)
                kernel.values[index] = {ParameterId::fromRaw(idBase + 60 + index), values[index]};
            return kernel;
        };
    };
    // lift, gamma, gain, offset (master, red, green, blue), then temperature, tint, contrast,
    // pivot, saturation, exposure.
    static constexpr std::array<double, 22> kWheels{
        0.05, 0.02, -0.01, 0.0,  0.2,   -0.1, 0.0, 0.3, 1.1,  1.05, 0.95,
        1.0,  0.01, 0.0,   0.02, -0.01, 0.0,  0.0, 1.0, 0.18, 1.0,  0.0};
    static constexpr std::array<double, 22> kPrimaries{0.0,  0.0,   0.0, 0.0,  0.0, 0.0, 0.0, 0.0,
                                                       1.0,  1.0,   1.0, 1.0,  0.0, 0.0, 0.0, 0.0,
                                                       30.0, -20.0, 1.4, 0.18, 1.3, 0.5};
    static constexpr std::array<double, 22> kEverything{
        -0.05, 0.0, 0.03, 0.0, -0.3, 0.1,   0.0,  -0.2, 0.9, 1.0, 1.1,
        0.9,   0.0, 0.01, 0.0, 0.0,  -45.0, 10.0, 0.7,  0.4, 0.0, -1.0};
    add(
        "effect.GradeKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime color grade image effect tests",
        [runGrades, grade] {
            return runGrades({grade(kWheels)}, 167600,
                             "production Grade: lift, gamma, gain and offset wheels over HDR");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.grade", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime color grade image effect tests",
        [runGrades, grade] {
            return runGrades({grade(kPrimaries), grade(kEverything)}, 167800,
                             "production Grade: primaries (white balance, exposure, contrast, "
                             "saturation) and every control together, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #479 Separate Channels and Combine Channels over the HDR, signed, partial-alpha linear
    // gradient, full frame and through an ROI: a Channel Select through the production builder's
    // resident program, and a Channel Combine (the gradient as the base, a signed HDR solid as
    // the second image) through the resident ImageCombineV1 Set Channel dispatch.
    const auto runImageChannels = [](const std::vector<ImageEffectKernel>& kernels,
                                     std::uint64_t idBase, std::string evidence) {
        FixtureRun combined;
        combined.prepared = true;
        for (const auto& kernel : kernels) {
            const bool twoInputs =
                std::holds_alternative<bloom::runtime::ChannelCombineKernel>(kernel);
            auto definition =
                (twoInputs ? imageCombinePlan(bloom::runtime::ImageCombineMode::Mix, idBase)
                           : bloom::gpu_coverage_plans::effectPlan(kernel, idBase))
                    ->copyDefinition();
            definition.operations[0] = bloom::gpu_coverage_plans::gradient(false, idBase + 50);
            if (twoInputs)
                std::get<CompiledImageEffect>(definition.operations[2]).kernel = kernel;
            const auto plan = std::make_shared<const bloom::runtime::CompiledCompositionPlan>(
                std::move(definition));
            idBase += 100;
            auto request = requestFor(*plan);
            auto frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
            const auto roiWindow = bloom::render::ImageWindow::create(5, 3, 6, 4);
            request.roi = *roiWindow.value();
            frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
        }
        combined.evidence = std::move(evidence);
        return combined;
    };
    using bloom::runtime::ChannelCombineKernel;
    using bloom::runtime::ChannelSelectKernel;
    using bloom::runtime::ImageChannelSource;
    const auto greySelect = [](const ImageChannelSource c) -> ImageEffectKernel {
        return ChannelSelectKernel{{c, c, c, ImageChannelSource::One}, false};
    };
    add(
        "effect.ChannelSelectKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime channel image effect tests",
        [runImageChannels] {
            return runImageChannels(
                {ChannelSelectKernel{{ImageChannelSource::Blue, ImageChannelSource::Luma,
                                      ImageChannelSource::Zero, ImageChannelSource::Alpha},
                                     true}},
                166600, "production Channel Select: swizzle, Luma and premultiply");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.separate-channels", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime channel image effect tests",
        [runImageChannels, greySelect] {
            return runImageChannels(
                {greySelect(ImageChannelSource::Red), greySelect(ImageChannelSource::Green),
                 greySelect(ImageChannelSource::Blue), greySelect(ImageChannelSource::Alpha)},
                166800,
                "production Separate Channels: the four grey stored-channel "
                "outputs, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "effect.ChannelCombineKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime channel image effect tests",
        [runImageChannels] {
            return runImageChannels({ChannelCombineKernel{1, ImageChannelSource::Red, false}},
                                    167400,
                                    "production Channel Combine: one slot from a second image");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.combine-channels", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime channel image effect tests",
        [runImageChannels] {
            return runImageChannels(
                {ChannelSelectKernel{{ImageChannelSource::Red, ImageChannelSource::Zero,
                                      ImageChannelSource::Zero, ImageChannelSource::One},
                                     false},
                 ChannelCombineKernel{2, ImageChannelSource::Luma, false},
                 ChannelCombineKernel{3, ImageChannelSource::Alpha, true}},
                167600,
                "production Combine Channels: base select, a Luma slot and a Straight alpha, "
                "full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #480: an Image to Channel Set layer read by a Shuffle or Pass lowers to these effects over the
    // image operation: a Pass over the packed layer (its stored R, G, B, alpha 1, associated off)
    // and a Shuffle joining a second packed image's lane into the first.
    add(
        "node.bloom.image-to-channel-set", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime snapshot compiler Image to Channel Set lowering",
        [runImageChannels] {
            return runImageChannels(
                {ChannelSelectKernel{{ImageChannelSource::Red, ImageChannelSource::Green,
                                      ImageChannelSource::Blue, ImageChannelSource::One},
                                     true},
                 ChannelCombineKernel{1, ImageChannelSource::Alpha, true}},
                168200,
                "production Image to Channel Set reads: a packed layer's Pass and a lane joined "
                "from a second packed image, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #339 Drop Shadow, Glow and Light Wrap: the production recipe chains (compositing_recipes.hpp)
    // over an HDR, partial-alpha gradient (Glow: solid), each variant full frame and ROI. Their
    // Merges carry explicit Normal, Add, Atop and In modes through the per-entry BlendV1 path.
    const auto runRecipes = [](const bloom::recipe_test::Recipe recipe,
                               const std::vector<bloom::recipe_test::RecipeValues>& variants,
                               std::uint64_t idBase, std::string evidence) {
        FixtureRun combined;
        combined.prepared = true;
        for (const auto& values : variants) {
            const auto plan = bloom::recipe_test::recipePlan(recipe, values, idBase).plan;
            idBase += 100;
            auto request = requestFor(*plan);
            auto frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
            const auto roi = bloom::render::ImageWindow::create(5, 4, 9, 7);
            request.roi = *roi.value();
            frame = runBuilderWithRequest(plan, request);
            combined.prepared = combined.prepared && frame.prepared;
            for (auto& value : frame.frames)
                combined.frames.push_back(std::move(value));
        }
        combined.evidence = std::move(evidence);
        return combined;
    };
    // A recipe keyed from real footage: the plan's input solid replaced by an image source.
    const auto runMediaGlow = [](const bloom::recipe_test::RecipeValues& values,
                                 const std::uint64_t idBase) {
        const bloom::runtime::CpuCompositionEvaluator evaluator;
        const auto recipe =
            bloom::recipe_test::recipePlan(bloom::recipe_test::Recipe::Glow, values, idBase);
        auto definition = recipe.plan->copyDefinition();
        definition.operations[recipe.input.value()] = CompiledImageSource{
            NodeId::fromRaw(idBase + 90), bloom::gpu_coverage_media::translucentExrImage().asset,
            0, 0, 0, std::string{}, false};
        return runMediaBuilder(publish(std::move(definition)), evaluator,
                               bloom::gpu_coverage_media::gateDirectory());
    };
    add(
        "node.bloom.drop-shadow", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime compositing recipe tests",
        [runRecipes] {
            bloom::recipe_test::RecipeValues shadowOnly;
            shadowOnly.shadowOnly = true;
            shadowOnly.direction = 135.0;
            shadowOnly.distance = 2.5;
            return runRecipes(bloom::recipe_test::Recipe::DropShadow, {{}, shadowOnly}, 169000,
                              "production Drop Shadow: softened, moved, coloured coverage under "
                              "the input and shadow only, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.glow", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime compositing recipe tests",
        [runRecipes, runMediaGlow] {
            bloom::recipe_test::RecipeValues keyed;
            keyed.key = true;
            keyed.mask = true;
            keyed.threshold = 0.5;
            keyed.glowOnly = true;
            // Luma Key's coverage is a function of colour: the exact-alpha gate keys a solid.
            bloom::recipe_test::RecipeValues glow;
            glow.solidInput = true;
            auto combined = runRecipes(bloom::recipe_test::Recipe::Glow, {glow, keyed}, 169400,
                                       "production Glow: keyed octaves added to the input, and a "
                                       "keyed, masked glow only, full frame and ROI, and a "
                                       "Physical glow keyed from translucent working-space EXR "
                                       "footage");
            // #545: decoded footage in the working space keys the same colour on both routes, so
            // a Physical glow of a translucent premultiplied EXR meets the exact-alpha gate.
            bloom::recipe_test::RecipeValues physical;
            physical.solidInput = true;
            physical.mode = bloom::runtime::detail::GlowMode::Physical;
            physical.threshold = 0.8;
            physical.intensity = 1.5;
            auto footage = runMediaGlow(physical, 169700);
            combined.prepared = combined.prepared && footage.prepared;
            for (auto& frame : footage.frames)
                combined.frames.push_back(std::move(frame));
            return combined;
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.light-wrap", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime compositing recipe tests",
        [runRecipes] {
            bloom::recipe_test::RecipeValues wrapOnly;
            wrapOnly.wrapOnly = true;
            return runRecipes(bloom::recipe_test::Recipe::LightWrap, {{}, wrapOnly}, 169800,
                              "production Light Wrap: the held-out background Atop and In the "
                              "foreground, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #516 Edge Extend: the production chain (alpha-weighted blur, unpremultiply, re-association
    // and a masked mix) over the HDR, partial-alpha gradient at two reaches and amounts.
    add(
        "node.bloom.edge-extend", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime compositing recipe tests",
        [runRecipes] {
            bloom::recipe_test::RecipeValues partial;
            partial.extendDistance = 2.5;
            partial.extendAmount = 0.6;
            return runRecipes(bloom::recipe_test::Recipe::EdgeExtend, {{}, partial}, 169900,
                              "production Edge Extend: the alpha-weighted colour of the nearby "
                              "solid pixels mixed under a soft edge, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #509 Alpha Contour: the production chain (the mask distance transform and the field raster
    // of the band over it, composited over, under or alone) over the HDR, partial-alpha gradient,
    // outward and inward, soft and hard, with an HDR colour.
    add(
        "node.bloom.alpha-contour", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime compositing recipe tests",
        [runRecipes] {
            bloom::recipe_test::RecipeValues under;
            under.contourComposite = bloom::runtime::detail::AlphaContourComposite::Under;
            under.contourSoftness = 0.0;
            bloom::recipe_test::RecipeValues inward;
            inward.contourComposite = bloom::runtime::detail::AlphaContourComposite::ContourOnly;
            inward.contourOffset = -1.5;
            inward.contourWidth = 0.4;
            return runRecipes(bloom::recipe_test::Recipe::AlphaContour, {{}, under, inward}, 509000,
                              "production Alpha Contour: the band of the input's alpha edge over, "
                              "under and alone, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #528 Inner Shadow and Inner Glow: the production chains (a coverage blur, an offset, a white
    // held out by the inverted coverage, two Levels and an Atop or In composite) over the HDR,
    // partial-alpha gradient inside its own window, in both sources and with the Only switches.
    add(
        "node.bloom.inner-shadow", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime compositing recipe tests",
        [runRecipes] {
            bloom::recipe_test::RecipeValues shadow;
            shadow.centred = true;
            bloom::recipe_test::RecipeValues only;
            only.centred = true;
            only.shadowOnly = true;
            only.direction = 135.0;
            only.distance = 2.5;
            only.choke = 0.4;
            return runRecipes(bloom::recipe_test::Recipe::InnerShadow, {shadow, only}, 171600,
                              "production Inner Shadow: the blurred, moved coverage held out of "
                              "a white, choked, coloured and composited Atop and In the input, "
                              "full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.inner-glow", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime compositing recipe tests",
        [runRecipes] {
            bloom::recipe_test::RecipeValues edge;
            edge.centred = true;
            edge.tint = {3.0, 1.5, 0.5, 1.0};
            bloom::recipe_test::RecipeValues centre = edge;
            centre.centre = true;
            centre.glowOnly = true;
            centre.choke = 0.3;
            return runRecipes(bloom::recipe_test::Recipe::InnerGlow, {edge, centre}, 171800,
                              "production Inner Glow: the Edge and Centre sources, choked, "
                              "scaled and composited Atop and In the input, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #506 Glow modes and their two primitives. The primitives run over the HDR, signed,
    // partial-alpha gradient (full frame and ROI); the modes run the production recipe chains.
    add(
        "effect.DirectionalBlurKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime glow primitive tests",
        [runGrades] {
            return runGrades(
                {[](const std::uint64_t idBase) -> ImageEffectKernel {
                     return bloom::runtime::DirectionalBlurKernel{
                         {ParameterId::fromRaw(idBase + 60), 30.0},
                         {ParameterId::fromRaw(idBase + 61), 1.7},
                         0.8};
                 },
                 [](const std::uint64_t idBase) -> ImageEffectKernel {
                     return bloom::runtime::DirectionalBlurKernel{
                         {ParameterId::fromRaw(idBase + 60), 200.0},
                         {ParameterId::fromRaw(idBase + 61), 3.0},
                         0.5};
                 }},
                170000,
                "production Directional Blur: bilinear ray taps growing the window along the ray, "
                "full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #514 Temporal denoise: the production chain (the look-back window and its weighted steps)
    // over a grained HDR picture with a moving partial-alpha block, full frame and ROI.
    const auto runTemporalDenoise =
        [](const std::vector<std::pair<bloom::denoise_test::TemporalValues, std::int64_t>>& variants,
           std::uint64_t idBase) {
            FixtureRun combined;
            combined.prepared = true;
            for (const auto& [values, frame] : variants) {
                const auto plan = bloom::denoise_test::temporalPlan(values, idBase);
                idBase += 200;
                auto request = requestFor(*plan);
                request.time = *bloom::core::RationalTime::create(frame, 24);
                auto run = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && run.prepared;
                for (auto& value : run.frames)
                    combined.frames.push_back(std::move(value));
                const auto roiWindow = bloom::render::ImageWindow::create(6, 4, 20, 14);
                request.roi = *roiWindow.value();
                run = runBuilderWithRequest(plan, request);
                combined.prepared = combined.prepared && run.prepared;
                for (auto& value : run.frames)
                    combined.frames.push_back(std::move(value));
            }
            return combined;
        };
    // A spatial run and the temporal chain, as one fixture.
    const auto withTemporal = [](FixtureRun spatial, FixtureRun temporal, std::string evidence) {
        spatial.prepared = spatial.prepared && temporal.prepared;
        for (auto& value : temporal.frames)
            spatial.frames.push_back(std::move(value));
        spatial.evidence = std::move(evidence);
        return spatial;
    };
    add(
        "effect.DenoiseKernel", GpuCoverageFixtureCriterion::Prepared, "src/runtime denoise tests",
        [runGrades, runTemporalDenoise, withTemporal] {
            bloom::denoise_test::TemporalValues two;
            two.frames = 2;
            bloom::denoise_test::TemporalValues still;
            still.still = true;
            return withTemporal(runGrades(
                {
                 [](const std::uint64_t idBase) -> ImageEffectKernel {
                     return bloom::runtime::DenoiseKernel{
                         bloom::runtime::DenoiseMode::Median,
                         {ParameterId::fromRaw(idBase + 60), 2.0},
                         {ParameterId::fromRaw(idBase + 61), 0.0}};
                 },
                 [](const std::uint64_t idBase) -> ImageEffectKernel {
                     return bloom::runtime::DenoiseKernel{
                         bloom::runtime::DenoiseMode::Median,
                         {ParameterId::fromRaw(idBase + 60), 3.0},
                         {ParameterId::fromRaw(idBase + 61), 0.5}};
                 },
                 [](const std::uint64_t idBase) -> ImageEffectKernel {
                     return bloom::runtime::DenoiseKernel{
                         bloom::runtime::DenoiseMode::Bilateral,
                         {ParameterId::fromRaw(idBase + 60), 2.0},
                         {ParameterId::fromRaw(idBase + 61), 0.4}};
                 },
                 [](const std::uint64_t idBase) -> ImageEffectKernel {
                     return bloom::runtime::DenoiseKernel{
                         bloom::runtime::DenoiseMode::Spatial,
                         {ParameterId::fromRaw(idBase + 60), 2.0},
                         {ParameterId::fromRaw(idBase + 61), 0.6}};
                 }},
                501000,
                "production Denoise: Median with and without a threshold, Bilateral and Spatial passes over HDR and partial alpha, full frame and ROI"),
                runTemporalDenoise({{{}, 5}, {two, 5}, {still, 4}, {{}, 1}}, 501400),
                "production Denoise: Median with and without a threshold, Bilateral and Spatial "
                "passes over HDR and partial alpha, and the Temporal steps over a moving, a still "
                "and an early window (a first step, middle steps and a last step), full frame and "
                "ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "node.bloom.denoise", GpuCoverageFixtureCriterion::Prepared, "src/runtime denoise tests",
        [runGrades, runTemporalDenoise, withTemporal] {
            return withTemporal(runGrades(
                {
                 [](const std::uint64_t idBase) -> ImageEffectKernel {
                     return bloom::runtime::DenoiseKernel{
                         bloom::runtime::DenoiseMode::Median,
                         {ParameterId::fromRaw(idBase + 60), 2.0},
                         {ParameterId::fromRaw(idBase + 61), 0.0}};
                 },
                 [](const std::uint64_t idBase) -> ImageEffectKernel {
                     return bloom::runtime::DenoiseKernel{
                         bloom::runtime::DenoiseMode::Median,
                         {ParameterId::fromRaw(idBase + 60), 3.0},
                         {ParameterId::fromRaw(idBase + 61), 0.5}};
                 },
                 [](const std::uint64_t idBase) -> ImageEffectKernel {
                     return bloom::runtime::DenoiseKernel{
                         bloom::runtime::DenoiseMode::Bilateral,
                         {ParameterId::fromRaw(idBase + 60), 2.0},
                         {ParameterId::fromRaw(idBase + 61), 0.4}};
                 },
                 [](const std::uint64_t idBase) -> ImageEffectKernel {
                     return bloom::runtime::DenoiseKernel{
                         bloom::runtime::DenoiseMode::Spatial,
                         {ParameterId::fromRaw(idBase + 60), 2.0},
                         {ParameterId::fromRaw(idBase + 61), 0.6}};
                 }},
                502000,
                "production Denoise node: the neighborhood pass its lowering builds in every spatial mode, full frame and ROI"),
                runTemporalDenoise({{{}, 5}}, 502400),
                "production Denoise node: the neighborhood pass its lowering builds in every "
                "spatial mode and the Temporal chain its lowering builds (the look-back window "
                "and its weighted steps), full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "effect.HighlightRollOffKernel", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime glow primitive tests",
        [runGrades] {
            return runGrades({[](const std::uint64_t idBase) -> ImageEffectKernel {
                                 return bloom::runtime::HighlightRollOffKernel{
                                     {ParameterId::fromRaw(idBase + 60), 0.6}};
                             }},
                             170400,
                             "production Highlight Roll-off: associated colour over HDR and "
                             "signed values, alpha kept, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "feature.glow.physical", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime compositing recipe tests",
        [runRecipes] {
            bloom::recipe_test::RecipeValues physical;
            physical.solidInput = true;
            physical.mode = bloom::runtime::detail::GlowMode::Physical;
            auto keyed = physical;
            keyed.key = true;
            keyed.mask = true;
            keyed.threshold = 0.5;
            keyed.glowOnly = true;
            return runRecipes(bloom::recipe_test::Recipe::Glow, {physical, keyed}, 170600,
                              "production Physical Glow: energy-keyed inverse-power octaves with "
                              "the scattered light removed under the key, and a keyed, masked "
                              "glow only, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "feature.glow.streak", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime compositing recipe tests",
        [runRecipes] {
            bloom::recipe_test::RecipeValues star;
            star.solidInput = true;
            star.mode = bloom::runtime::detail::GlowMode::Streak;
            auto anamorphic = star;
            anamorphic.points = 2;
            anamorphic.angle = 0.0;
            anamorphic.chromatic = 0.6;
            return runRecipes(bloom::recipe_test::Recipe::Glow, {star, anamorphic}, 171000,
                              "production Streak Glow: a four-point star and a chromatic "
                              "anamorphic streak of chained Directional Blur rays, full frame and "
                              "ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    add(
        "feature.glow.shared_controls", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime compositing recipe tests",
        [runRecipes] {
            bloom::recipe_test::RecipeValues shared;
            shared.solidInput = true;
            shared.channelRadius = {1.0, 0.5, 0.25};
            shared.gradient = true;
            shared.dirt = true;
            shared.rollOff = 0.5;
            return runRecipes(bloom::recipe_test::Recipe::Glow, {shared}, 171400,
                              "production Glow shared controls: per-channel radius, octave "
                              "gradient, lens dirt and highlight roll-off, full frame and ROI");
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    // #502 Adjustment: production compiles of real documents. Adjustment -> Curves at the top of a
    // Merge over two layers, its entry layer at a partial opacity with its coverage moved (the
    // white-frame coverage, Stencil inverse and masked Image Mix), and at its defaults with a
    // Screen blend (the explicitly blended Merge), each full frame and ROI.
    add(
        "node.bloom.adjustment", GpuCoverageFixtureCriterion::Prepared,
        "src/runtime merge entry layer tests (snapshot compiler Adjustment lowering)",
        [] {
            using bloom::merge_entry_test::Builder;
            FixtureRun combined;
            combined.prepared = true;
            for (const bool moved : {true, false}) {
                Builder builder;
                const auto blue = builder.solid({0.1, 0.2, 0.9, 0.75}, 6.0, 4.0);
                const auto [blueNode, blueLayer] = builder.layerNode(
                    blue, {.position = {6.0, 5.0}, .pivot = bloom::document::LayerPivot::Content});
                builder.slot(builder.root(), blueNode, blueLayer);
                builder.slot(builder.root(), builder.solid({0.8, 0.1, 0.05, 1.0}, 16.0, 12.0));
                const auto slot =
                    builder.topSlot(builder.root(), builder.curves(builder.adjustment()));
                builder.entryLayer(
                    builder.root(), slot,
                    moved ? bloom::merge_entry_test::LayerValues{.position = {5.0, 7.0},
                                                                 .rotation = 15.0,
                                                                 .opacity = 0.6}
                          : bloom::merge_entry_test::LayerValues{
                                .blendMode = bloom::core::BlendMode::Screen});
                const auto compiled = bloom::merge_entry_test::compile(builder.project());
                if (!compiled.plan) {
                    combined.prepared = false;
                    combined.evidence = "the Adjustment document did not compile";
                    return combined;
                }
                auto request = requestFor(*compiled.plan);
                for (const bool roi : {false, true}) {
                    if (roi) {
                        const auto window = bloom::render::ImageWindow::create(5, 4, 9, 7);
                        request.roi = *window.value();
                    }
                    auto frame = runBuilderWithRequest(compiled.plan, request);
                    combined.prepared = combined.prepared && frame.prepared;
                    for (auto& value : frame.frames)
                        combined.frames.push_back(std::move(value));
                }
            }
            combined.evidence = "production Adjustment: Curves of the stack below, a moved partial "
                                "coverage and a Screen blend, full frame and ROI";
            return combined;
        },
        {}, FixturePrerequisite::PackagedShaderTools);
    return list;
}

int run(int argc, char** argv) {
    bool requireDevice = false;
    bool nativeAcceptance = false;
    std::filesystem::path loader;
    std::filesystem::path routeProofDirectory;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--require-device") {
            requireDevice = true;
        } else if (argument == "--native-acceptance") {
            nativeAcceptance = true;
        } else if (argument == "--loader" && i + 1 < argc) {
            loader = argv[++i];
        } else if (argument == "--media-fixtures" && i + 1 < argc) {
            bloom::gpu_coverage_video::mediaFixturesDirectory() = argv[++i];
        } else if (argument == "--route-proof-dir" && i + 1 < argc) {
            routeProofDirectory = argv[++i];
        }
    }
    if (nativeAcceptance || requireDevice) {
        return bloom::gpu_coverage_gate::runNativeAcceptance(fixtures(), loader, requireDevice,
                                                             routeProofDirectory);
    }

    const auto list = fixtures();
    std::vector<std::string> fixtureIds;
    fixtureIds.reserve(list.size());
    for (const auto& fixture : list) {
        fixtureIds.push_back(fixture.id);
    }

    std::vector<std::string> failures;
    for (const auto& issue : bloom::runtime::validateGpuCoverageContract()) {
        failures.push_back("contract: " + issue.detail);
    }
    // #349 (owner decision, 2026-11-09): the statistics readback is one narrow host-preparation
    // entry with an id, a rationale and its owning document, and it is the only addition: the
    // statistics kernel itself is a Required pixel operation.
    {
        const auto preparation = bloom::runtime::gpuApprovedCpuPreparation();
        const auto readback = std::ranges::find_if(preparation, [](const auto& entry) {
            return entry.id == "prep.statistics.result_readback";
        });
        if (readback == preparation.end() ||
            readback->disposition != bloom::runtime::GpuCoverageDisposition::ApprovedCpuHostPreparation ||
            readback->approvalReference != "docs/architecture/image-statistics.md" ||
            readback->rationale.empty() || readback->label.empty() ||
            std::ranges::count_if(preparation, [](const auto& entry) {
                return entry.id.starts_with("prep.statistics.");
            }) != 1)
            failures.push_back("the statistics readback is not one narrow typed entry naming its "
                               "owning document");
        const auto effects = bloom::runtime::gpuImageEffectCoverage();
        const auto kernel = std::ranges::find_if(effects, [](const auto& entry) {
            return entry.id == "effect.StatisticsKernel";
        });
        if (kernel == effects.end() ||
            kernel->disposition != bloom::runtime::GpuCoverageDisposition::Required ||
            !kernel->approvalReference.empty())
            failures.push_back("the statistics kernel is not a Required pixel operation");
    }
    bloom::gpu_coverage_mutation::nodeTypeMutationProof(fixtureIds, failures);
    bloom::gpu_coverage_route_proof::routeProofSinkRejectsFakes(failures);
    std::string nativeNegativeEvidence;
    if (bloom::gpu_coverage_native::nativeNegativeFixturesDetectMissedGpu(nativeNegativeEvidence)) {
        std::cout << "NEGATIVE native: " << nativeNegativeEvidence << '\n';
    } else {
        failures.push_back("native acceptance negative proof: " + nativeNegativeEvidence);
    }

    const auto requiredIds = requiredCoverageIds();
    // The explicit fixture list must cover every Required id and list no unrequired fixture. This
    // is a structural check independent of any optional prerequisite: a missing fixture stays a
    // failure even when the packaged shader tools are absent.
    for (const auto& failure : structuralFixtureFailures(list, requiredIds)) {
        failures.push_back(failure);
    }
    // Focused regression proving the environmental NotRun path can never absorb a missing fixture.
    std::string regressionEvidence;
    if (prerequisiteDecisionRegression(regressionEvidence)) {
        std::cout << "REGRESSION prerequisite: " << regressionEvidence << '\n';
    } else {
        failures.push_back("prerequisite regression: " + regressionEvidence);
    }

    const bool toolsAvailable = bloom::gpu_coverage_ocio::toolsAvailable();
    const bool mediaFixturesAvailable =
        !bloom::gpu_coverage_video::mediaFixturesDirectory().empty();
    const PrerequisiteAvailability prerequisiteAvailability{toolsAvailable, mediaFixturesAvailable};
    // Routes are proven only by the genuine external route harnesses (viewer/RAM/export/headless),
    // which the CPU-only gate cannot run. It reports them as externally owned and never relabels an
    // executor-prepared scene as a route proof; the distinct native acceptance CTest still requires
    // an actual proof for every route.
    for (const auto& id : requiredIds) {
        if (id.rfind("route.", 0) == 0) {
            std::cout << "NOTRUN(route) " << id << " (externally-owned genuine proof; owner "
                      << routeOwner(id) << ")\n";
        }
    }

    std::size_t passed = 0;
    std::size_t red = 0;
    std::size_t notRun = 0;
    std::size_t prerequisiteNotRun = 0;
    std::size_t mediaNotRun = 0;
    for (const auto& fixture : list) {
        const auto execution = classifyFixtureExecution(fixture, prerequisiteAvailability);
        if (execution == FixtureExecution::NativeNotRun) {
            std::cout << "NOTRUN " << fixture.id << " (native fixture owned by " << fixture.owner
                      << ")\n";
            ++notRun;
            continue;
        }
        if (execution == FixtureExecution::PrerequisiteNotRun) {
            if (fixture.prerequisite == FixturePrerequisite::MediaFixtures ||
                (fixture.prerequisite == FixturePrerequisite::MediaFixturesAndShaderTools &&
                 !prerequisiteAvailability.mediaFixtures)) {
                std::cout << "NOTRUN(media) " << fixture.id
                          << " (generated media fixtures not supplied)\n";
                ++mediaNotRun;
            } else {
                std::cout << "NOTRUN(tools) " << fixture.id
                          << " (packaged GPU shader tools unavailable: "
                          << bloom::gpu_coverage_ocio::unavailableReason() << ")\n";
                ++prerequisiteNotRun;
            }
            continue;
        }
        const auto result = fixture.run();
        if (result.prepared) {
            std::cout << "PASS " << fixture.id << ": " << result.evidence << '\n';
            ++passed;
        } else {
            std::cout << "FAIL " << fixture.id << ": " << result.evidence << '\n';
            failures.push_back("required coverage hole '" + fixture.id + "': " + result.evidence);
            ++red;
        }
    }

    std::cout << "\nGPU coverage contract: " << passed << " pass, " << red << " required holes, "
              << prerequisiteNotRun << " not-run (unavailable tools), " << mediaNotRun
              << " not-run (media fixtures absent), " << notRun << " native-not-run\n";
    if (!failures.empty()) {
        std::cerr << "\nGPU COVERAGE GATE RED (" << failures.size() << " failures):\n";
        for (const auto& failure : failures) {
            std::cerr << "  - " << failure << '\n';
        }
        return 1;
    }
    if (prerequisiteNotRun > 0 || mediaNotRun > 0) {
        // Every structural and independent check passed; the required fixture could not execute
        // because this build/environment does not supply its explicit prerequisite. This is an
        // explicit environmental SKIP (CTest 77), never a red gate and never a fabricated pass. A
        // qualified build that supplies the prerequisite executes these fixtures normally and fails
        // if the production path is broken.
        std::cout << "SKIP: " << (prerequisiteNotRun + mediaNotRun)
                  << " required fixtures were not executable in this environment ("
                  << prerequisiteNotRun << " need the packaged GPU shader tools";
        if (prerequisiteNotRun > 0) {
            std::cout << " (" << bloom::gpu_coverage_ocio::unavailableReason() << ")";
        }
        std::cout << ", " << mediaNotRun << " need the generated media fixtures)\n";
        return 77;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
