// CPU-side acceptance for the production image-effect scene preparation: identity/bypass/look
// aliasing, the missing-LUT CPU refusal passthrough, the fail-closed non-identity CST, the
// fail-closed real file transform, and (with the pinned tools) the genuine GPU OCIO command a
// non-identity CST emits, including the explicit materialization of a text leaf input. It needs no
// device and no loader; the native builder -> executor -> CPU oracle parity is a separate harness.

#include "compositing_recipe_test_support.hpp"
#include "gpu_scene_preparation_test_support.hpp"
#include "grade_curves_test_support.hpp"

#include <bloom/color/ocio_gpu_program.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/input_color_context.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace {

using bloom::document::AssetId;
using bloom::document::AssetKind;
using bloom::document::AssetRecord;
using bloom::document::LayerId;
using bloom::document::LayerSlotId;
using bloom::document::NodeId;
using bloom::document::ParameterId;
using bloom::runtime::CompiledCompositionOutput;
using bloom::runtime::CompiledImageEffect;
using bloom::runtime::CompiledMerge;
using bloom::runtime::CompiledMergeInput;
using bloom::runtime::CompiledOperation;
using bloom::runtime::CompiledText;
using bloom::runtime::CstKernel;
using bloom::runtime::FileTransformKernel;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioOutputEncoding;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuSceneOcioContext;
using bloom::runtime::GpuSceneOcioEffectCommand;
using bloom::runtime::IdentityImageKernel;
using bloom::runtime::ImageEffectKernel;

[[nodiscard]] std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>
effectPlan(const std::vector<CompiledOperation>& leading, const ImageEffectKernel& kernel,
           const std::uint64_t idBase) {
    const LayerIds ids{ParameterId::fromRaw(idBase + 0), ParameterId::fromRaw(idBase + 1),
                       ParameterId::fromRaw(idBase + 2), ParameterId::fromRaw(idBase + 3),
                       ParameterId::fromRaw(idBase + 4), ParameterId::fromRaw(idBase + 5)};
    std::vector<CompiledOperation> operations = leading;
    const auto effectIndex = static_cast<std::size_t>(leading.size());
    operations.emplace_back(CompiledImageEffect{NodeId::fromRaw(idBase + 10),
                                                OperationIndex::fromRaw(effectIndex - 1), kernel,
                                                false, false});
    operations.emplace_back(layerOutput(NodeId::fromRaw(idBase + 11), LayerId::fromRaw(idBase + 12),
                                        OperationIndex::fromRaw(effectIndex), ids, LayerValues{}));
    operations.emplace_back(
        CompiledMerge{NodeId::fromRaw(idBase + 13),
                      std::vector<CompiledMergeInput>{CompiledMergeInput{
                          LayerSlotId::fromRaw(idBase + 14), LayerId::fromRaw(idBase + 12),
                          OperationIndex::fromRaw(effectIndex + 1)}}});
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 15),
                                                      OperationIndex::fromRaw(effectIndex + 2)});
    return publish(bloom::runtime::CompiledCompositionPlanDefinition{
        bloom::document::Revision::fromRaw(7), kProjectId, kCompositionId, format(8, 6),
        std::move(operations), OperationIndex::fromRaw(effectIndex + 3)});
}

[[nodiscard]] CompiledOperation solidLeaf(const std::uint64_t idBase) {
    return bloom::runtime::CompiledSolid{
        NodeId::fromRaw(idBase),
        {ParameterId::fromRaw(idBase + 1), Color4d{0.5, 0.25, 0.125, 1.0}},
        {ParameterId::fromRaw(idBase + 2), 8.0},
        {ParameterId::fromRaw(idBase + 3), 6.0}};
}

#ifdef BLOOM_GPUSHADER_TOOLS_DIR
[[nodiscard]] CompiledOperation textLeaf(const std::uint64_t idBase) {
    return CompiledText{
        NodeId::fromRaw(idBase),
        ParameterId::fromRaw(idBase + 1),
        "BLOOM",
        {ParameterId::fromRaw(idBase + 2), 10.0},
        {ParameterId::fromRaw(idBase + 3), Color4d{0.8, 0.4, 0.2, 1.0}},
        bloom::runtime::CompiledTextLayout{ParameterId::fromRaw(idBase + 4),
                                           0,
                                           {ParameterId::fromRaw(idBase + 5), 1.0},
                                           {ParameterId::fromRaw(idBase + 6), 0.0}}};
}
#endif

[[nodiscard]] const GpuSceneOcioEffectCommand*
findOcioCommand(const bloom::runtime::PreparedGpuScene& scene) {
    for (const auto& command : scene.commands()) {
        if (const auto* ocio = std::get_if<GpuSceneOcioEffectCommand>(&command)) {
            return ocio;
        }
    }
    return nullptr;
}

void testCanvasPreparation(Expectations& expectations) {
    const auto window = [](const std::int64_t x, const std::int64_t y,
                           const std::uint64_t width, const std::uint64_t height) {
        const auto created = bloom::render::ImageWindow::create(x, y, width, height);
        return *created.value();
    };
    // The margin ids stay clear of effectPlan's layer parameters (idBase + 0..5): one id with two
    // values is a non-canonical plan and fails preflight.
    const auto kernel = [](const bloom::runtime::CanvasImageEffect effect,
                           const std::array<double, 4> sides) {
        std::array<bloom::runtime::CompiledScalarParameter, 4> parameters{
            bloom::runtime::CompiledScalarParameter{ParameterId::fromRaw(8101), sides[0]},
            bloom::runtime::CompiledScalarParameter{ParameterId::fromRaw(8102), sides[1]},
            bloom::runtime::CompiledScalarParameter{ParameterId::fromRaw(8103), sides[2]},
            bloom::runtime::CompiledScalarParameter{ParameterId::fromRaw(8104), sides[3]}};
        return bloom::runtime::CanvasImageKernel{effect, parameters};
    };
    for (const auto kind : {bloom::runtime::CanvasImageEffect::Crop,
                            bloom::runtime::CanvasImageEffect::Pad}) {
        const auto plan = effectPlan({solidLeaf(800)},
                                     kernel(kind, kind == bloom::runtime::CanvasImageEffect::Crop
                                                      ? std::array<double, 4>{1, 0, 1, 0}
                                                      : std::array<double, 4>{2, 1, 0, 0}),
                                     8000);
        const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
        expectations.expect(prepared.hasValue(), "canvas effect prepares through the GPU scene");
        if (!prepared)
            continue;
        const auto index = prepared.scene->commandForOperation()[1];
        const auto* command = std::get_if<bloom::runtime::GpuSceneTranslationCommand>(
            &prepared.scene->commands()[index]);
        expectations.expect(command != nullptr,
                            "canvas effect emits a real resident GPU translation pass");
        if (!command)
            continue;
        const auto expected = kind == bloom::runtime::CanvasImageEffect::Crop
                                  ? window(1, 0, 6, 6)
                                  : window(-2, -1, 10, 7);
        expectations.expect(command->outputWindow == expected &&
                                command->outputDisplayWindow == expected,
                            "GPU canvas data and display windows match the authored margins");
        expectations.expect(command->translationX ==
                                (kind == bloom::runtime::CanvasImageEffect::Crop ? -1.0 : 2.0) &&
                                command->translationY ==
                                    (kind == bloom::runtime::CanvasImageEffect::Crop ? 0.0 : 1.0),
                            "GPU canvas maps unchanged source coordinates by exact integer axes");
    }
}

void testDiscretePreparation(Expectations& expectations) {
    for (const auto kernel : {bloom::runtime::DiscreteImageKernel{true, false, 0},
                              bloom::runtime::DiscreteImageKernel{false, true, 0},
                              bloom::runtime::DiscreteImageKernel{false, false, 1},
                              bloom::runtime::DiscreteImageKernel{false, false, 2},
                              bloom::runtime::DiscreteImageKernel{false, false, 3}}) {
        const auto plan = effectPlan({solidLeaf(400)}, kernel, 4000);
        const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
        expectations.expect(prepared.hasValue(), "discrete production scene prepares without OCIO");
        if (!prepared)
            continue;
        const auto index = prepared.scene->commandForOperation()[1];
        const auto* affine =
            std::get_if<bloom::runtime::GpuSceneAffineCommand>(&prepared.scene->commands()[index]);
        expectations.expect(
            affine && affine->opacity == 1.0F &&
                affine->outputWindow.extent().width() == ((kernel.quarterTurns & 1U) ? 6U : 8U) &&
                affine->outputWindow.extent().height() == ((kernel.quarterTurns & 1U) ? 8U : 6U),
            "discrete effect emits a genuine resident affine pass and exact data extents");
    }
}

void testResizeFitPreparation(Expectations& expectations) {
    for (const std::int64_t policy : {0, 1, 2}) {
        const bloom::runtime::ResizeFitKernel kernel{10, 10, 2, 1, 0, policy};
        const auto plan = effectPlan({solidLeaf(8000)}, kernel, 8100);
        auto request = requestFor(*plan);
        const auto prepared = CpuGpuSceneBuilder{}.build(plan, request);
        expectations.expect(prepared.hasValue(),
                            "Resize/Fit prepares through the production builder");
        if (!prepared)
            continue;
        const auto command = prepared.scene->commandForOperation()[1];
        const auto* affine = std::get_if<bloom::runtime::GpuSceneAffineCommand>(
            &prepared.scene->commands()[command]);
        expectations.expect(affine && affine->outputDescriptor &&
                                affine->outputDescriptor->displayWindow().extent().width() == 10 &&
                                affine->outputDescriptor->displayWindow().extent().height() == 10 &&
                                affine->pixelAspect == *bloom::core::PixelAspectRatio::create(2, 1),
                            "Resize/Fit emits a real affine with the authored extent and PAR");
        CpuCompositionEvaluator evaluator;
        const auto cpu = evaluator.evaluate(plan, request, {});
        std::vector<std::shared_ptr<const Rgba32fImage>> images;
        const bool replayed = replayScene(*prepared.scene, images);
        bool matches = cpu.frame() && replayed && images[prepared.scene->outputCommand()];
        if (matches) {
            const auto expected = cpu.frame()->processImage().pixels();
            const auto actual = images[prepared.scene->outputCommand()]->pixels();
            matches = expected.size() == actual.size();
            for (std::size_t i = 0; matches && i < expected.size(); ++i)
                for (std::size_t channel = 0; channel < 4; ++channel)
                    matches =
                        matches &&
                        std::abs(expected[i].components()[channel] -
                                 actual[i].components()[channel]) <=
                            2e-6F * std::max(1.0F, std::abs(expected[i].components()[channel]));
        }
        expectations.expect(matches,
                            "prepared Resize/Fit command replay matches the CPU reference");
        const auto roi = ImageWindow::create(2, 1, 3, 3);
        request.roi = *roi.value();
        const auto clipped = CpuGpuSceneBuilder{}.build(plan, request);
        const auto cpuRoi = evaluator.evaluate(plan, request, {});
        expectations.expect(clipped && cpuRoi.frame() &&
                                clipped.scene->outputDescriptor().dataWindow() ==
                                    cpuRoi.frame()->processImage().descriptor()->dataWindow(),
                            "Resize/Fit preserves the production ROI route");
    }
}

// #336 Transform 2D: the production builder emits one real affine (no output descriptor: the
// display window and PAR follow the input), its replay matches the CPU reference, the ROI route
// agrees, identity aliases, and a degenerate scale publishes no command and a transparent frame.
void testTransform2DPreparation(Expectations& expectations) {
    using bloom::document::Vec2d;
    using bloom::runtime::CompiledScalarParameter;
    using bloom::runtime::CompiledVec2Parameter;
    using bloom::runtime::Transform2DExtent;
    using bloom::runtime::Transform2DKernel;
    const auto kernelOf = [](const std::uint64_t base, const Vec2d translate, const Vec2d scale,
                             const double rotation, const Transform2DExtent extent) {
        return Transform2DKernel{CompiledVec2Parameter{ParameterId::fromRaw(base + 1), translate},
                                 CompiledVec2Parameter{ParameterId::fromRaw(base + 2), Vec2d{1, -1}},
                                 CompiledVec2Parameter{ParameterId::fromRaw(base + 3), scale},
                                 CompiledScalarParameter{ParameterId::fromRaw(base + 4), rotation},
                                 extent};
    };
    std::uint64_t base = 8300;
    for (const auto extent : {Transform2DExtent::Transformed, Transform2DExtent::Input,
                              Transform2DExtent::Display}) {
        for (const auto& [translate, scale, rotation] :
             {std::tuple{Vec2d{1.5, -0.75}, Vec2d{1, 1}, 0.0},
              std::tuple{Vec2d{0.25, 0.5}, Vec2d{1.5, 0.5}, 30.0},
              std::tuple{Vec2d{2, 1}, Vec2d{-1, 1}, 90.0}}) {
            const auto plan = effectPlan({solidLeaf(base)},
                                         kernelOf(base + 10, translate, scale, rotation, extent),
                                         base + 50);
            auto request = requestFor(*plan);
            const auto prepared = CpuGpuSceneBuilder{}.build(plan, request);
            expectations.expect(prepared.hasValue(),
                                "Transform 2D prepares through the production builder");
            if (!prepared) {
                base += 100;
                continue;
            }
            const auto command = prepared.scene->commandForOperation()[1];
            const auto* emitted =
                command < prepared.scene->commands().size() ? &prepared.scene->commands()[command]
                                                            : nullptr;
            const auto* affine =
                emitted ? std::get_if<bloom::runtime::GpuSceneAffineCommand>(emitted) : nullptr;
            const auto* translation =
                emitted ? std::get_if<bloom::runtime::GpuSceneTranslationCommand>(emitted)
                        : nullptr;
            const bool moveOnly = scale == Vec2d{1, 1} && rotation == 0.0;
            expectations.expect(
                moveOnly ? translation && !translation->outputDisplayWindow &&
                               translation->opacity == 1.0F
                         : affine && !affine->outputDescriptor && affine->opacity == 1.0F,
                "Transform 2D emits the Layer raster command (translation for a pure move, "
                "affine otherwise) that keeps the input display window and pixel aspect");
            CpuCompositionEvaluator evaluator;
            const auto cpu = evaluator.evaluate(plan, request, {});
            std::vector<std::shared_ptr<const Rgba32fImage>> images;
            const bool replayed = replayScene(*prepared.scene, images);
            bool matches = cpu.frame() && replayed && images[prepared.scene->outputCommand()];
            if (matches) {
                const auto expected = cpu.frame()->processImage().pixels();
                const auto actual = images[prepared.scene->outputCommand()]->pixels();
                matches = expected.size() == actual.size();
                for (std::size_t i = 0; matches && i < expected.size(); ++i)
                    for (std::size_t channel = 0; channel < 4; ++channel)
                        matches =
                            matches &&
                            std::abs(expected[i].components()[channel] -
                                     actual[i].components()[channel]) <=
                                2e-6F * std::max(1.0F, std::abs(expected[i].components()[channel]));
            }
            expectations.expect(matches,
                                "prepared Transform 2D command replay matches the CPU reference");
            const auto roi = ImageWindow::create(2, 1, 3, 3);
            request.roi = *roi.value();
            const auto clipped = CpuGpuSceneBuilder{}.build(plan, request);
            const auto cpuRoi = evaluator.evaluate(plan, request, {});
            expectations.expect(clipped && cpuRoi.frame() &&
                                    clipped.scene->outputDescriptor().dataWindow() ==
                                        cpuRoi.frame()->processImage().descriptor()->dataWindow(),
                                "Transform 2D preserves the production ROI route");
            base += 100;
        }
    }
    // Identity aliases the input; a degenerate scale publishes nothing on either route.
    const auto identityPlan = effectPlan(
        {solidLeaf(base)},
        kernelOf(base + 10, Vec2d{}, Vec2d{1, 1}, 360.0, Transform2DExtent::Transformed),
        base + 50);
    const auto identity = CpuGpuSceneBuilder{}.build(identityPlan, requestFor(*identityPlan));
    expectations.expect(identity && identity.scene->commandForOperation()[1] ==
                                        identity.scene->commandForOperation()[0],
                        "an identity Transform 2D aliases its input command");
    base += 100;
    const auto collapsedPlan = effectPlan(
        {solidLeaf(base)},
        kernelOf(base + 10, Vec2d{}, Vec2d{0, 1}, 0.0, Transform2DExtent::Transformed),
        base + 50);
    const auto collapsed = CpuGpuSceneBuilder{}.build(collapsedPlan, requestFor(*collapsedPlan));
    const auto collapsedCpu =
        CpuCompositionEvaluator{}.evaluate(collapsedPlan, requestFor(*collapsedPlan), {});
    bool transparent = collapsedCpu.frame() != nullptr;
    if (collapsedCpu.frame())
        for (const auto pixel : collapsedCpu.frame()->processImage().pixels())
            transparent = transparent && pixel.alpha() == 0.0F;
    expectations.expect(collapsed && collapsed.scene->commandForOperation()[1] ==
                                         bloom::runtime::kInvalidGpuSceneCommand &&
                            transparent,
                        "a degenerate Transform 2D publishes no command and no CPU pixels");
}

void testIdentityAndRefusals(Expectations& expectations) {
    // An identity CST aliases the input command exactly.
    const auto identity =
        effectPlan({solidLeaf(100)}, CstKernel{"lin_rec709_scene", "lin_rec709_scene"}, 1000);
    const auto identityPrepared = CpuGpuSceneBuilder{}.build(identity, requestFor(*identity));
    expectations.expect(identityPrepared.hasValue(), "an identity CST prepares");
    if (identityPrepared) {
        const auto& map = identityPrepared.scene->commandForOperation();
        expectations.expect(map[1] == map[0], "an identity CST aliases its input command");
        expectations.expect(findOcioCommand(*identityPrepared.scene) == nullptr,
                            "an identity CST emits no GPU OCIO command");
    }

    // IdentityImageKernel and bypass alias too.
    const auto identityKernel = effectPlan({solidLeaf(200)}, IdentityImageKernel{}, 2000);
    const auto kernelPrepared =
        CpuGpuSceneBuilder{}.build(identityKernel, requestFor(*identityKernel));
    expectations.expect(kernelPrepared.hasValue(), "an IdentityImageKernel prepares");
    if (kernelPrepared) {
        expectations.expect(kernelPrepared.scene->commandForOperation()[1] ==
                                kernelPrepared.scene->commandForOperation()[0],
                            "an IdentityImageKernel aliases its input command");
    }

    // A missing-LUT file transform is the CPU refusal-warning passthrough: exact identity.
    const auto missingLut = effectPlan(
        {solidLeaf(300)},
        FileTransformKernel{AssetId::fromRaw(0), 0, 0, "lin_rec709_scene", std::nullopt}, 3000);
    const auto missingPrepared = CpuGpuSceneBuilder{}.build(missingLut, requestFor(*missingLut));
    expectations.expect(missingPrepared.hasValue(), "a missing-LUT file transform prepares");
    if (missingPrepared) {
        expectations.expect(missingPrepared.scene->commandForOperation()[1] ==
                                missingPrepared.scene->commandForOperation()[0],
                            "a missing-LUT file transform aliases its input command");
    }
}

void testFailClosed(Expectations& expectations, const std::string& from, const std::string& to) {
    // A non-identity CST with no injected preparer must fail closed (never a false identity).
    const auto plan = effectPlan({solidLeaf(400)}, CstKernel{from, to}, 4000);
    const auto prepared = CpuGpuSceneBuilder{}.build(plan, requestFor(*plan));
    expectations.expect(!prepared.hasValue(), "a non-identity CST without a preparer fails closed");
    if (!prepared) {
        expectations.expect(prepared.diagnostic.code ==
                                PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "the fail-closed diagnostic is UnsupportedOperation");
    }

    // A valid LUT asset whose bytes are unreadable is the CPU refusal-warning passthrough: exact
    // identity, not a failure and not a false transform.
    FileTransformKernel file;
    file.lutAssetId = AssetId::fromRaw(42);
    AssetRecord asset;
    asset.id = AssetId::fromRaw(42);
    asset.kind = AssetKind::Lut;
    asset.locator.path = "missing.cube";
    asset.locator.relinkHint = "file://missing.cube";
    file.asset = asset;
    const auto filePlan = effectPlan({solidLeaf(500)}, file, 5000);
    const auto filePrepared = CpuGpuSceneBuilder{}.build(filePlan, requestFor(*filePlan));
    expectations.expect(filePrepared.hasValue(), "an unreadable LUT prepares as identity");
    if (filePrepared) {
        expectations.expect(filePrepared.scene->commandForOperation()[1] ==
                                filePrepared.scene->commandForOperation()[0],
                            "an unreadable LUT aliases its input command");
    }
}

// Used by the file-based LUT cases, which run where the LUT helper process exists.
#if defined(BLOOM_GPUSHADER_TOOLS_DIR) && defined(__linux__)
[[nodiscard]] std::size_t countOcioCommands(const bloom::runtime::PreparedGpuScene& scene) {
    std::size_t count = 0;
    for (const auto& command : scene.commands()) {
        count += std::holds_alternative<GpuSceneOcioEffectCommand>(command) ? 1U : 0U;
    }
    return count;
}
#endif

#ifdef BLOOM_GPUSHADER_TOOLS_DIR
[[nodiscard]] GpuSceneOcioContext ocioContext() {
    GpuOcioCompileOptions options;
    options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    GpuSceneOcioContext context;
    context.preparer = std::make_shared<GpuOcioProgramPreparer>();
    context.compileOptions = options;
    return context;
}

void testRealCst(Expectations& expectations, const std::string& from, const std::string& to) {
    const auto context = ocioContext();

    // A raster (solid) input emits a real ProcessEffect command whose input is the solid command.
    const auto rasterPlan = effectPlan({solidLeaf(600)}, CstKernel{from, to}, 6000);
    const CpuGpuSceneBuilder rasterBuilder(nullptr, {}, context);
    const auto rasterPrepared = rasterBuilder.build(rasterPlan, requestFor(*rasterPlan));
    expectations.expect(rasterPrepared.hasValue(), "a real non-identity CST prepares");
    if (rasterPrepared) {
        const auto* ocio = findOcioCommand(*rasterPrepared.scene);
        expectations.expect(ocio != nullptr, "a real CST emits a GPU OCIO command");
        if (ocio != nullptr) {
            expectations.expect(ocio->program != nullptr && ocio->program->encoding() ==
                                                                GpuOcioOutputEncoding::FinalRgba32f,
                                "the emitted command is a ProcessEffect RGBA32F program");
            expectations.expect(ocio->input == rasterPrepared.scene->commandForOperation()[0],
                                "the OCIO command consumes the solid command");
        }
    }

    // A vector leaf input (text) is materialized as a coverage command before the OCIO command.
    const auto textPlanValue = effectPlan({textLeaf(700)}, CstKernel{from, to}, 7000);
    const CpuGpuSceneBuilder textBuilder(nullptr, {}, context);
    const auto textPrepared = textBuilder.build(textPlanValue, requestFor(*textPlanValue));
    expectations.expect(textPrepared.hasValue(), "a non-identity CST over text prepares");
    if (textPrepared) {
        const auto* ocio = findOcioCommand(*textPrepared.scene);
        expectations.expect(ocio != nullptr, "a text-fed CST emits a GPU OCIO command");
        if (ocio != nullptr) {
            // A text leaf read as an image is the CPU leaf's own form: the glyph coverage over its
            // own bitmap window, then one identity-linear translation into the effect's window
            // (a83f5f7db). The effect therefore reads the translation, whose input is the accepted
            // coverage GPU command.
            const auto& inputCommand = textPrepared.scene->commands()[ocio->input];
            const auto* translation =
                std::get_if<bloom::runtime::GpuSceneTranslationCommand>(&inputCommand);
            expectations.expect(translation != nullptr,
                                "the text leaf lands in place through the leaf translation");
            if (translation != nullptr) {
                expectations.expect(
                    std::holds_alternative<bloom::runtime::GpuSceneCoverageSolidCommand>(
                        textPrepared.scene->commands()[translation->input]),
                    "the text leaf was materialized as an accepted coverage GPU command");
            }
        }
    }
}

// #337: Matte Blur prepares two resident Coverage neighborhood commands (horizontal, vertical) over
// the transparent halo, never the Rgba blur's programs: same windows, distinct program identities.
void testRealMatteBlur(Expectations& expectations) {
    using bloom::runtime::BlurChannels;
    using bloom::runtime::BlurEdgeMode;
    using bloom::runtime::SeparableBlurKernel;
    using bloom::runtime::SeparableBlurKind;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    const auto commandsOf = [&](const BlurChannels channels) {
        const auto plan = effectPlan({solidLeaf(800)},
                                     SeparableBlurKernel{SeparableBlurKind::Gaussian,
                                                         {ParameterId::fromRaw(8090), 2.0},
                                                         BlurEdgeMode::Transparent,
                                                         channels},
                                     8000);
        std::vector<GpuSceneOcioEffectCommand> commands;
        const auto prepared = builder.build(plan, requestFor(*plan));
        if (prepared)
            for (const auto& command : prepared.scene->commands())
                if (const auto* ocio = std::get_if<GpuSceneOcioEffectCommand>(&command))
                    commands.push_back(*ocio);
        return commands;
    };
    const auto coverage = commandsOf(BlurChannels::Coverage);
    const auto rgba = commandsOf(BlurChannels::Rgba);
    expectations.expect(coverage.size() == 2 && rgba.size() == 2,
                        "Matte Blur prepares two resident neighborhood passes");
    if (coverage.size() != 2 || rgba.size() != 2)
        return;
    for (std::size_t pass = 0; pass < 2; ++pass) {
        expectations.expect(coverage[pass].program && rgba[pass].program &&
                                coverage[pass].program->identity() !=
                                    rgba[pass].program->identity() &&
                                coverage[pass].semanticKey != rgba[pass].semanticKey,
                            "Matte Blur programs and command keys differ from the Rgba blur's");
        expectations.expect(coverage[pass].outputWindow == rgba[pass].outputWindow &&
                                coverage[pass].outputWindow.originX() == -2 &&
                                coverage[pass].outputWindow.extent().width() == 12,
                            "Matte Blur keeps the Gaussian blur's transparent-halo window");
    }
}

// #338: Matte Dilate prepares two resident morphology passes over the transparent halo; Matte Erode
// keeps the input window (no halo padding) because it never grows coverage. Both programs differ
// from the Coverage blur's and from each other.
void testRealMatteMorphology(Expectations& expectations) {
    using bloom::runtime::BlurChannels;
    using bloom::runtime::BlurEdgeMode;
    using bloom::runtime::SeparableBlurKernel;
    using bloom::runtime::SeparableBlurKind;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    const auto build = [&](const SeparableBlurKind kind) {
        const auto plan = effectPlan({solidLeaf(810)},
                                     SeparableBlurKernel{kind,
                                                         {ParameterId::fromRaw(8190), 2.0},
                                                         BlurEdgeMode::Transparent,
                                                         BlurChannels::Coverage},
                                     8100);
        std::vector<GpuSceneOcioEffectCommand> commands;
        std::size_t translations = 0;
        const auto prepared = builder.build(plan, requestFor(*plan));
        if (prepared)
            for (const auto& command : prepared.scene->commands()) {
                if (const auto* ocio = std::get_if<GpuSceneOcioEffectCommand>(&command))
                    commands.push_back(*ocio);
                if (const auto* translation = std::get_if<bloom::runtime::GpuSceneTranslationCommand>(&command);
                    translation && translation->sourceOperation.value() == 1)
                    ++translations;
            }
        return std::pair{commands, translations};
    };
    const auto [erode, erodePadding] = build(SeparableBlurKind::Erode);
    const auto [dilate, dilatePadding] = build(SeparableBlurKind::Dilate);
    const auto [blur, blurPadding] = build(SeparableBlurKind::Gaussian);
    expectations.expect(erode.size() == 2 && dilate.size() == 2 && blur.size() == 2,
                        "Matte Erode/Dilate prepare two resident neighborhood passes");
    if (erode.size() != 2 || dilate.size() != 2 || blur.size() != 2)
        return;
    expectations.expect(erodePadding == 0 && dilatePadding == 1 && blurPadding == 1,
                        "only Dilate pads a transparent halo; Erode keeps its input window");
    for (std::size_t pass = 0; pass < 2; ++pass) {
        expectations.expect(erode[pass].outputWindow.originX() == 0 &&
                                erode[pass].outputWindow.extent().width() == 8 &&
                                dilate[pass].outputWindow == blur[pass].outputWindow &&
                                dilate[pass].outputWindow.originX() == -2 &&
                                dilate[pass].outputWindow.extent().width() == 12,
                            "Matte Erode keeps the input window and Dilate grows by the halo");
        expectations.expect(
            erode[pass].program && dilate[pass].program && blur[pass].program &&
                erode[pass].program->identity() != dilate[pass].program->identity() &&
                dilate[pass].program->identity() != blur[pass].program->identity() &&
                erode[pass].program->program().semanticsId ==
                    "bloom.render.separable-erode.coverage.v1" &&
                dilate[pass].program->program().semanticsId ==
                    "bloom.render.separable-dilate.coverage.v1",
            "Matte Erode/Dilate programs are distinct from each other and from the blur");
    }
}

// #340 and #479: each grade and each channel select prepares one resident neighborhood command over
// the input window from its kind's fixed program. A changed value changes the command identity and
// data but reuses the compiled shader and native program key; an identity grade aliases its input
// with no command.
void testRealColorGrades(Expectations& expectations) {
    auto context = ocioContext();
    const CpuGpuSceneBuilder builder(nullptr, {}, context);
    const auto commandsOf = [&](const bloom::runtime::ImageEffectKernel& kernel) {
        const auto plan = effectPlan({solidLeaf(830)}, kernel, 8300);
        std::vector<GpuSceneOcioEffectCommand> commands;
        const auto prepared = builder.build(plan, requestFor(*plan));
        expectations.expect(static_cast<bool>(prepared), "grade scene prepares");
        if (prepared)
            for (const auto& command : prepared.scene->commands())
                if (const auto* ocio = std::get_if<GpuSceneOcioEffectCommand>(&command))
                    commands.push_back(*ocio);
        return commands;
    };
    const auto levels = [](const double gamma) {
        bloom::runtime::LevelsKernel kernel;
        std::uint64_t id = 8390;
        for (auto& channel : kernel.channels) {
            channel = {{ParameterId::fromRaw(id), 0.0},
                       {ParameterId::fromRaw(id + 1), 1.0},
                       {ParameterId::fromRaw(id + 2), 1.0},
                       {ParameterId::fromRaw(id + 3), 0.0},
                       {ParameterId::fromRaw(id + 4), 1.0}};
            id += 5;
        }
        kernel.channels[0].gamma.source = gamma;
        return kernel;
    };
    const auto curves = [](const double middle) {
        auto kernel = bloom::runtime::testing::identityCurvesKernel(8420);
        bloom::runtime::testing::setCurve(kernel, bloom::document::GradeCurve::Red,
                                          bloom::runtime::testing::legacyPoints(
                                              {{2, bloom::document::Vec2d{0.5, middle}}}));
        return kernel;
    };
    const auto hue = [](const double degrees) {
        return bloom::runtime::HueSaturationKernel{{ParameterId::fromRaw(8450), degrees},
                                                   {ParameterId::fromRaw(8451), 1.2}};
    };
    using Source = bloom::runtime::ImageChannelSource;
    const auto select = [](const std::array<Source, 4> sources, const bool premultiply) {
        return bloom::runtime::ChannelSelectKernel{sources, premultiply};
    };
    struct Case final {
        bloom::runtime::ImageEffectKernel first;
        bloom::runtime::ImageEffectKernel changed;
        bloom::runtime::ImageEffectKernel identity;
        const char* semantics;
    };
    for (const auto& item :
         {Case{levels(2.2), levels(0.5), levels(1.0), "bloom.render.levels.v1"},
          Case{curves(0.7), curves(0.2), curves(0.5), "bloom.render.curves.v1"},
          Case{hue(30.0), hue(-80.0),
               bloom::runtime::HueSaturationKernel{{ParameterId::fromRaw(8450), 0.0},
                                                   {ParameterId::fromRaw(8451), 1.0}},
               "bloom.render.hue-saturation.v2"},
          // #479 Channel Select (Separate Channels' outputs, Combine Channels' base): a changed
          // mapping is new data for the same program; the red, green, blue, alpha mapping
          // without premultiply aliases its input.
          Case{select({Source::Blue, Source::Green, Source::Red, Source::Alpha}, false),
               select({Source::Luma, Source::Luma, Source::Luma, Source::One}, true),
               select({Source::Red, Source::Green, Source::Blue, Source::Alpha}, false),
               "bloom.render.channel-select.v1"}}) {
        const auto first = commandsOf(item.first);
        const auto compiles = context.preparer->counters().shaderCompilerRuns;
        const auto changed = commandsOf(item.changed);
        const auto repeated = commandsOf(item.first);
        expectations.expect(first.size() == 1 && changed.size() == 1 && repeated.size() == 1,
                            "a grade prepares one resident command");
        if (first.size() != 1 || changed.size() != 1 || repeated.size() != 1)
            continue;
        expectations.expect(first[0].program &&
                                first[0].program->program().semanticsId == item.semantics &&
                                first[0].outputWindow.originX() == 0 &&
                                first[0].outputWindow.extent().width() == 8 &&
                                first[0].outputWindow.extent().height() == 6,
                            "a grade keeps its input window");
        expectations.expect(changed[0].program->identity() != first[0].program->identity() &&
                                changed[0].semanticKey != first[0].semanticKey &&
                                changed[0].program->nativeProgramKey() ==
                                    first[0].program->nativeProgramKey() &&
                                context.preparer->counters().shaderCompilerRuns == compiles,
                            "a changed grade value changes data, never the compiled shader");
        expectations.expect(repeated[0].semanticKey == first[0].semanticKey &&
                                repeated[0].program->identity() == first[0].program->identity(),
                            "an unchanged grade reuses its command identity");
        expectations.expect(commandsOf(item.identity).empty(),
                            "an identity grade aliases its input with no command");
    }
}

// #338: Luma Key prepares exactly one resident neighborhood command over the input window. A
// changed threshold changes the command identity and data but reuses the compiled shader and the
// native program key, so keyed thresholds never recompile.
void testRealLumaKey(Expectations& expectations) {
    auto context = ocioContext();
    const CpuGpuSceneBuilder builder(nullptr, {}, context);
    const auto commandOf = [&](const double low, const double high, const bool invert) {
        const auto plan =
            effectPlan({solidLeaf(820)},
                       bloom::runtime::LumaKeyKernel{{ParameterId::fromRaw(8291), low},
                                                     {ParameterId::fromRaw(8292), high},
                                                     invert},
                       8200);
        std::vector<GpuSceneOcioEffectCommand> commands;
        const auto prepared = builder.build(plan, requestFor(*plan));
        if (prepared)
            for (const auto& command : prepared.scene->commands())
                if (const auto* ocio = std::get_if<GpuSceneOcioEffectCommand>(&command))
                    commands.push_back(*ocio);
        return commands;
    };
    const auto first = commandOf(0.0, 0.5, false);
    const auto compiles = context.preparer->counters().shaderCompilerRuns;
    const auto changed = commandOf(0.1, 0.9, true);
    const auto repeated = commandOf(0.0, 0.5, false);
    expectations.expect(first.size() == 1 && changed.size() == 1 && repeated.size() == 1,
                        "Luma Key prepares one resident command");
    if (first.size() != 1 || changed.size() != 1 || repeated.size() != 1)
        return;
    expectations.expect(first[0].program &&
                            first[0].program->program().semanticsId == "bloom.render.luma-key.v1" &&
                            first[0].outputWindow.originX() == 0 &&
                            first[0].outputWindow.extent().width() == 8 &&
                            first[0].outputWindow.extent().height() == 6,
                        "Luma Key keeps its input window");
    expectations.expect(changed[0].program->identity() != first[0].program->identity() &&
                            changed[0].semanticKey != first[0].semanticKey &&
                            changed[0].program->nativeProgramKey() ==
                                first[0].program->nativeProgramKey() &&
                            context.preparer->counters().shaderCompilerRuns == compiles,
                        "a changed Luma Key threshold changes data, never the compiled shader");
    expectations.expect(repeated[0].semanticKey == first[0].semanticKey &&
                            repeated[0].program->identity() == first[0].program->identity(),
                        "an unchanged Luma Key reuses its command identity");
}

// #339: every recipe prepares entirely on the production GPU route. Its Merge reaches the
// resident BlendV1 path with the recipe's explicit mode (Add for Glow, Atop and In for Light Wrap),
// never the Normal source-over chain or a CPU frame; Drop Shadow's Normal Merge stays on the
// source-over chain.
void testRealRecipes(Expectations& expectations) {
    using bloom::recipe_test::Recipe;
    using bloom::recipe_test::RecipeValues;
    const CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
    const auto modesOf = [&](const Recipe recipe, const RecipeValues& values) {
        const auto plan = bloom::recipe_test::recipePlan(recipe, values).plan;
        const auto prepared = builder.build(plan, requestFor(*plan));
        std::vector<bloom::core::BlendMode> modes;
        std::size_t merges = 0;
        expectations.expect(static_cast<bool>(prepared),
                            "a recipe prepares on the production GPU route");
        if (prepared)
            for (const auto& command : prepared.scene->commands()) {
                if (const auto* blend = std::get_if<bloom::runtime::GpuSceneBlendCommand>(&command);
                    blend && !blend->imageCombine)
                    modes.push_back(blend->mode);
                merges += std::holds_alternative<bloom::runtime::GpuSceneMergeCommand>(command);
            }
        return std::pair{modes, merges};
    };
    const auto has = [](const std::vector<bloom::core::BlendMode>& modes,
                        const bloom::core::BlendMode mode) {
        return std::ranges::find(modes, mode) != modes.end();
    };
    const auto [glow, glowMerges] = modesOf(Recipe::Glow, {});
    expectations.expect(has(glow, bloom::core::BlendMode::Add),
                        "Glow adds its glow through a resident Add blend");
    const auto [wrap, wrapMerges] = modesOf(Recipe::LightWrap, {});
    expectations.expect(has(wrap, bloom::core::BlendMode::Atop),
                        "Light Wrap composites through a resident Atop blend");
    RecipeValues wrapOnly;
    wrapOnly.wrapOnly = true;
    const auto [only, onlyMerges] = modesOf(Recipe::LightWrap, wrapOnly);
    expectations.expect(has(only, bloom::core::BlendMode::In),
                        "Light Wrap's wrap only composites through a resident In blend");
    const auto [shadow, shadowMerges] = modesOf(Recipe::DropShadow, {});
    expectations.expect(shadowMerges >= 2 && !has(shadow, bloom::core::BlendMode::Add),
                        "Drop Shadow's Normal Merge stays on the source-over chain");
    // #506: Streak's rays and Highlight Roll-off reach their resident programs; nothing in the
    // chain is left for a CPU frame.
    RecipeValues streak;
    streak.mode = bloom::runtime::detail::GlowMode::Streak;
    streak.rollOff = 0.5;
    const auto streakPlan = bloom::recipe_test::recipePlan(Recipe::Glow, streak).plan;
    const auto streakPrepared = builder.build(streakPlan, requestFor(*streakPlan));
    std::size_t effects = 0;
    if (streakPrepared)
        for (const auto& command : streakPrepared.scene->commands())
            effects += std::holds_alternative<bloom::runtime::GpuSceneOcioEffectCommand>(command);
    // Four rays of five passes, each ray's 1/4 Levels, the gains, the roll-off and the key.
    expectations.expect(static_cast<bool>(streakPrepared) && effects >= 4 * 5 + 4 + 3,
                        "a Streak Glow with roll-off prepares every ray pass on the GPU route");
    static_cast<void>(glowMerges);
    static_cast<void>(wrapMerges);
    static_cast<void>(onlyMerges);
}

#if defined(__linux__)
void testRealFileTransform(Expectations& expectations, const std::string& working,
                           const std::string& process) {
    const auto base = std::filesystem::temp_directory_path() / "bloom_gpu_effect_test";
    std::error_code directoryError;
    std::filesystem::create_directories(base, directoryError);
    expectations.expect(!directoryError, "the LUT fixture directory is creatable");
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
                        "the LUT fixture reads for its digest");
    if (resource.error != bloom::color::LutError::None) {
        return;
    }
    const auto makeAsset = [&]() {
        AssetRecord asset;
        asset.id = AssetId::fromRaw(77);
        asset.kind = AssetKind::Lut;
        asset.locator.path = "curve.cube";
        asset.locator.relinkHint = "file://curve.cube";
        asset.contentDigest = resource.digest;
        return asset;
    };
    const auto context = ocioContext();
    bloom::runtime::GpuSceneMediaContext media;
    media.assetBaseDirectory = base;

    // The default process space is the working space: one real FileTransform command.
    FileTransformKernel sameSpace;
    sameSpace.lutAssetId = AssetId::fromRaw(77);
    sameSpace.asset = makeAsset();
    const auto samePlan = effectPlan({solidLeaf(800)}, sameSpace, 8000);
    const CpuGpuSceneBuilder sameBuilder(nullptr, media, context);
    const auto samePrepared = sameBuilder.build(samePlan, requestFor(*samePlan));
    expectations.expect(samePrepared.hasValue(), "a working-space LUT prepares");
    if (samePrepared) {
        expectations.expect(countOcioCommands(*samePrepared.scene) == 1,
                            "a working-space LUT emits exactly one OCIO command");
        const auto* ocio = findOcioCommand(*samePrepared.scene);
        expectations.expect(ocio != nullptr && ocio->program != nullptr &&
                                ocio->program->program().semanticsId ==
                                    bloom::color::kOcioGpuFileTransformSemanticsId,
                            "the emitted command is the FileTransform program");
    }

    // An explicit non-working process space emits the CPU's CST/LUT/CST chain.
    FileTransformKernel crossSpace;
    crossSpace.lutAssetId = AssetId::fromRaw(77);
    crossSpace.asset = makeAsset();
    crossSpace.processSpaceId = process;
    const auto crossPlan = effectPlan({solidLeaf(900)}, crossSpace, 9000);
    const CpuGpuSceneBuilder crossBuilder(nullptr, media, context);
    const auto crossPrepared = crossBuilder.build(crossPlan, requestFor(*crossPlan));
    expectations.expect(crossPrepared.hasValue(), "a cross-space LUT prepares");
    if (crossPrepared) {
        expectations.expect(countOcioCommands(*crossPrepared.scene) == 3,
                            "a cross-space LUT emits the working/process CST chain");
        const auto& map = crossPrepared.scene->commandForOperation();
        expectations.expect(map[1] != map[0],
                            "a cross-space LUT publishes a distinct chained command");
    }
    static_cast<void>(working);
}
#endif
#endif

} // namespace

int main() {
    try {
        Expectations expectations;
        testResizeFitPreparation(expectations);
        testTransform2DPreparation(expectations);
        testIdentityAndRefusals(expectations);
        testCanvasPreparation(expectations);
        testDiscretePreparation(expectations);

        const auto config = bloom::runtime::detail::resolveInputColorConfig(
            bloom::runtime::EvaluationColorIntent::LinearRec709Scene);
        if (!config.has_value()) {
            std::cerr << "SKIP: the Bloom Neutral OCIO config is unavailable\n";
            return 77;
        }
        const std::string from{config->processColorSpaceId()};
        const std::string to{config->sRgbTextureColorSpaceId()};
        testFailClosed(expectations, from, to);
#ifdef BLOOM_GPUSHADER_TOOLS_DIR
        testRealCst(expectations, from, to);
        testRealMatteBlur(expectations);
        testRealMatteMorphology(expectations);
        testRealLumaKey(expectations);
        testRealColorGrades(expectations);
        testRealRecipes(expectations);
#if defined(__linux__)
        testRealFileTransform(expectations, from, to);
#endif
#else
        std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set (real arms skipped)\n";
#endif

        if (!expectations.ok()) {
            std::cerr << "image-effect scene preparation expectations failed\n";
            return 1;
        }
        std::cout << "PASS: image-effect GPU scene preparation\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
