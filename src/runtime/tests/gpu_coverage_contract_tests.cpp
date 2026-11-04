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

#include <bloom/document/graph.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/runtime/gpu_coverage_contract.hpp>
#include <bloom/runtime/gpu_coverage_route_proof_contract.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/input_color_context.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

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

    for (const std::string_view id :
         {"operation.CompiledSolid", "operation.CompiledLayerOutput", "operation.CompiledMerge",
          "operation.CompiledCompositionOutput"}) {
        add(std::string{id}, GpuCoverageFixtureCriterion::Prepared,
            "src/runtime gpu scene preparation tests", [] { return runBuilder(basePlan()); });
    }
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
    // the translation pass). This gate requires bit-exact alpha everywhere, while the shared affine
    // backend guarantees bit-exact alpha only at the 0/1 endpoints (gpu_affine.hpp), so these
    // fixtures use dyadic coefficients whose inverse map is exact on both routes. Arbitrary
    // rotations are verified at the documented 2e-6 gate by gpu_scene_effect_native.
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
    // #364: Cryptomatte through the production rank gather, stacked upload, CryptomatteCoverageV1
    // and proxy resample: names resolved through the manifest, names resolved by hash without one,
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
            bloom::runtime::CurvesKernel kernel;
            std::uint64_t id = idBase + 60;
            for (auto& channel : kernel.points)
                for (std::size_t point = 0; point < channel.size(); ++point) {
                    const double x = static_cast<double>(point) / 4.0;
                    channel[point] = {ParameterId::fromRaw(id++), bloom::document::Vec2d{x, x}};
                }
            kernel.points[0][1].source = bloom::document::Vec2d{0.25, 0.1};
            kernel.points[0][2].source = bloom::document::Vec2d{0.5, middle};
            kernel.points[0][3].source = bloom::document::Vec2d{0.75, 0.9};
            kernel.points[2][4].source = bloom::document::Vec2d{1.0, 1.6};
            kernel.points[4][0].source = bloom::document::Vec2d{0.0, alphaLift};
            kernel.clamp = clamp;
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
