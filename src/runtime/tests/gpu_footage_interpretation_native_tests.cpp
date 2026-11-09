// Interpret Footage (#381): real native proof of the footage interpretation pass.
//
// Real OpenEXR stills and an EXR sequence go through the production media pipeline with Ignore,
// Invert and premultiplied-matte alpha interpretations and with deinterlacing (both field orders).
// The production CpuGpuSceneBuilder emits the stored-samples upload, the footage pass as a native
// ProcessNeighborhood program and (under ACEScg) the input->working OCIO transform; the prepared
// scene runs through the production GpuSceneExecutor on a real device and must match the
// unchanged CpuCompositionEvaluator oracle (2e-6), with a warm rerun served from the cache. Every
// synthesized camera log input also runs a negative and an over-range code ramp (#412).
//
// Without the pinned shader tools the test SKIPs; without a device it SKIPs (exit 77) unless
// --require-device is passed.

#include "footage_interpretation_plan_support.hpp"
#include "gpu_media_cache_test_support.hpp"
#include "gpu_media_executor_test_support.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::readbackResidentImage;
using bloom::render::Rgba32f;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::EvaluationColorIntent;
using bloom::runtime::GpuOcioCompileOptions;
using bloom::runtime::GpuOcioProgramPreparer;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::GpuSceneOcioContext;
using bloom::runtime::GpuSceneOcioEffectCommand;
using bloom::runtime::media_executor_test::kSceneBudget;
using bloom::runtime::media_executor_test::Options;
using bloom::runtime::media_executor_test::parseOptions;
using bloom::runtime::media_executor_test::pixelsClose;
using bloom::runtime::media_executor_test::runScene;

[[nodiscard]] std::size_t passCommands(const bloom::runtime::PreparedGpuScene& scene) {
    std::size_t count = 0;
    for (const auto& command : scene.commands())
        if (const auto* ocio = std::get_if<GpuSceneOcioEffectCommand>(&command);
            ocio != nullptr && ocio->program != nullptr &&
            ocio->program->program().semanticsId == "bloom.render.footage-interpretation.v1")
            ++count;
    return count;
}

// The parity measure pixelsClose() gates: |a - b| / max(1, |a|, |b|) over every channel.
[[nodiscard]] double maxRelativeDifference(const std::span<const Rgba32f> lhs,
                                           const std::span<const Rgba32f> rhs) noexcept {
    if (lhs.size() != rhs.size())
        return std::numeric_limits<double>::infinity();
    double worst = 0.0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const std::array<float, 4> a{lhs[i].red(), lhs[i].green(), lhs[i].blue(), lhs[i].alpha()};
        const std::array<float, 4> b{rhs[i].red(), rhs[i].green(), rhs[i].blue(), rhs[i].alpha()};
        for (std::size_t c = 0; c < 4; ++c) {
            if (!std::isfinite(a[c]) || !std::isfinite(b[c])) {
                if (a[c] != b[c])
                    return std::numeric_limits<double>::infinity();
                continue;
            }
            const double scale = std::max(
                {1.0, std::abs(static_cast<double>(a[c])), std::abs(static_cast<double>(b[c]))});
            worst = std::max(
                worst, std::abs(static_cast<double>(a[c]) - static_cast<double>(b[c])) / scale);
        }
    }
    return worst;
}

void check(Expectations& expectations, GpuDevice& device, CpuCompositionEvaluator& evaluator,
           const std::filesystem::path& directory, const document::AssetRecord& asset,
           const GpuSceneOcioContext& ocioContext, const std::uint64_t idBase,
           const std::string& name, const bool aces, const bloom::core::RationalTime time = {},
           const std::size_t expectedPasses = 1, const double pixelAspect = 1.0) {
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    context.assetBaseDirectory = directory;
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    const auto plan =
        aspectPlan(format(8, 8), asset, LayerValues{.position = {4.0, 4.0}, .opacity = 1.0}, idBase,
                   pixelAspect);
    auto request = requestFor(*plan);
    request.time = time;
    if (aces)
        request.colorIntent =
            EvaluationColorIntent{.workingColorSpaceId = "ACEScg",
                                  .ocioConfigRevision = {},
                                  .ocioConfigUri = bloom::color::kAcesCgV1ConfigUri};
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), (name + ": prepares").c_str());
    if (!prepared) {
        std::cerr << name << " prepare diagnostic: " << prepared.diagnostic.message << "\n";
        return;
    }
    expectations.expect(passCommands(*prepared.scene) == expectedPasses,
                        (name + ": the footage pass count").c_str());
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, (name + ": the CPU oracle evaluates").c_str());
    if (!frame.frame())
        return;
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{});
    expectations.expect(cache.hasValue(), (name + ": the cache creates").c_str());
    if (!cache)
        return;
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), (name + ": the executor creates").c_str());
    if (!executor)
        return;
    const auto cold = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(cold.ready, (name + ": the native run completes").c_str());
    if (!cold.ready) {
        std::cerr << name << " executor diagnostic: code="
                  << static_cast<int>(executor.executor->diagnostic().code) << " "
                  << executor.executor->diagnostic().message << "\n";
        return;
    }
    const auto readback = readbackResidentImage(*cold.image, kSceneBudget);
    if (readback.hasValue())
        std::cout << name << ": max relative difference "
                  << maxRelativeDifference(readback.pixels, frame.frame()->processImage().pixels())
                  << "\n";
    expectations.expect(expectedPasses == 0 || cold.countersAtReady.ocioEffectDispatches > 0,
                        (name + ": the pass dispatched natively").c_str());
    expectations.expect(readback.hasValue() &&
                            pixelsClose(readback.pixels, frame.frame()->processImage().pixels()),
                        (name + ": native pixels match the CPU oracle (2e-6)").c_str());
    const auto before = executor.executor->counters();
    const auto warm = runScene(*executor.executor, prepared.scene, kSceneBudget);
    expectations.expect(warm.ready && executor.executor->counters().dispatches == before.dispatches,
                        (name + ": a warm rerun dispatches nothing new").c_str());
}

// #448: one executor and one scene cache, the same plan identity re-prepared after only the
// footage pixel aspect changed. The prepared output key must change and the warm cache must not
// serve the square frame for the 2:1 one; each native frame matches its own CPU oracle.
void checkAspectChange(Expectations& expectations, GpuDevice& device,
                       CpuCompositionEvaluator& evaluator, const std::filesystem::path& directory,
                       const document::AssetRecord& asset, const GpuSceneOcioContext& ocioContext,
                       const std::uint64_t idBase) {
    auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    context.assetBaseDirectory = directory;
    const CpuGpuSceneBuilder builder(nullptr, context, ocioContext);
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{});
    expectations.expect(cache.hasValue(), "aspect change: the cache creates");
    if (!cache)
        return;
    auto executor = GpuSceneExecutor::create(device, *cache.cache);
    expectations.expect(executor.hasValue(), "aspect change: the executor creates");
    if (!executor)
        return;
    const auto outputKey = [](const bloom::runtime::PreparedGpuScene& scene) {
        const auto index = scene.outputCommand();
        return std::visit([](const auto& command) { return command.semanticKey; },
                          scene.commands()[static_cast<std::size_t>(index)]);
    };
    std::vector<std::string> keys;
    std::vector<std::vector<Rgba32f>> natives;
    for (const double aspect : {1.0, 2.0}) {
        const auto plan =
            aspectPlan(format(8, 8), asset, LayerValues{.position = {4.0, 4.0}, .opacity = 1.0},
                       idBase, aspect);
        const auto request = requestFor(*plan);
        const auto prepared = builder.build(plan, request);
        expectations.expect(prepared.hasValue(), "aspect change: prepares");
        if (!prepared)
            return;
        keys.push_back(outputKey(*prepared.scene));
        auto oracleRequest = request;
        oracleRequest.bypassOperationCache = true;
        const auto oracle = evaluator.evaluate(plan, oracleRequest, {});
        const auto run = runScene(*executor.executor, prepared.scene, kSceneBudget);
        expectations.expect(oracle.frame() != nullptr && run.ready,
                            "aspect change: the oracle and the native run complete");
        if (!oracle.frame() || !run.ready)
            return;
        const auto readback = readbackResidentImage(*run.image, kSceneBudget);
        expectations.expect(
            readback.hasValue() &&
                pixelsClose(readback.pixels, oracle.frame()->processImage().pixels()),
            "aspect change: native pixels match the CPU oracle (2e-6)");
        if (!readback.hasValue())
            return;
        natives.emplace_back(readback.pixels.begin(), readback.pixels.end());
    }
    expectations.expect(keys[0] != keys[1],
                        "aspect change: the prepared output key covers the pixel aspect");
    expectations.expect(natives[0] != natives[1],
                        "aspect change: the warm cache serves a new frame for the new aspect");
}

// A 4x6 premultiplied RGBA field pattern: even rows and odd rows differ.
[[nodiscard]] std::vector<ExrPixel> fieldPixels(const float shift) {
    std::vector<ExrPixel> pixels;
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 4; ++x) {
            const float alpha = y % 3 == 2 ? 0.5F : 1.0F;
            const float base = (y % 2 == 0 ? 0.25F : 0.75F) + 0.05F * static_cast<float>(x) + shift;
            pixels.push_back({base * alpha, (1.0F - base) * alpha, 0.5F * alpha, alpha});
        }
    return pixels;
}

} // namespace

int main(const int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid)
            return 2;
#ifndef BLOOM_GPUSHADER_TOOLS_DIR
        std::cout << "SKIP: BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
        return 77;
#else
        Expectations expectations;
        auto directory =
            std::filesystem::temp_directory_path() / "bloom_gpu_footage_interpretation_test";
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        const auto still = directory / "still.exr";
        writeExrRgba(still, 3, 2, signedHdrPixels());
        const auto image = imageAsset(still, "still", 700);
        for (int frame = 1; frame <= 2; ++frame)
            writeExrRgba(directory / ("field." + std::to_string(frame) + ".exr"), 4, 6,
                         fieldPixels(0.1F * static_cast<float>(frame)));
        auto sequence = imageAsset(directory / "field.1.exr", "field", 701);
        sequence.kind = document::AssetKind::Sequence;
        sequence.manifest.pattern = "field.#.exr";
        sequence.manifest.padding = 1;
        sequence.manifest.first = 1;
        sequence.manifest.last = 2;
        for (int frame = 1; frame <= 2; ++frame)
            sequence.manifest.members.push_back(
                sequenceMember(directory / ("field." + std::to_string(frame) + ".exr"), frame, 0));

        CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(directory);
        auto preparer = std::make_shared<GpuOcioProgramPreparer>();
        GpuOcioCompileOptions compileOptions;
        compileOptions.glslangValidatorPath =
            std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
        compileOptions.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
        GpuSceneOcioContext ocioContext;
        ocioContext.preparer = preparer;
        ocioContext.compileOptions = compileOptions;

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
        auto ignore = image;
        ignore.interpretation.alphaMode = document::AssetAlphaMode::Ignore;
        check(expectations, *device.device, evaluator, directory, ignore, ocioContext, 93000,
              "ignore alpha", false);
        auto inverted = image;
        inverted.interpretation.alphaMode = document::AssetAlphaMode::Straight;
        inverted.interpretation.invertAlpha = true;
        check(expectations, *device.device, evaluator, directory, inverted, ocioContext, 93100,
              "straight inverted", false);
        auto matted = image;
        matted.interpretation.alphaMode = document::AssetAlphaMode::Premultiplied;
        matted.interpretation.matteColor = {0.25, 0.5, 0.75};
        check(expectations, *device.device, evaluator, directory, matted, ocioContext, 93200,
              "premultiplied matte", false);
        check(expectations, *device.device, evaluator, directory, matted, ocioContext, 93300,
              "premultiplied matte under ACEScg", true);
        auto upper = sequence;
        upper.interpretation.deinterlace = true;
        upper.interpretation.fieldOrder = document::AssetFieldOrder::UpperFirst;
        check(expectations, *device.device, evaluator, directory, upper, ocioContext, 93400,
              "deinterlace upper", false);
        auto lower = upper;
        lower.interpretation.fieldOrder = document::AssetFieldOrder::LowerFirst;
        lower.interpretation.invertAlpha = true;
        check(expectations, *device.device, evaluator, directory, lower, ocioContext, 93500,
              "deinterlace lower inverted, second frame", false,
              *bloom::core::RationalTime::create(1, 24));
        // A pixel aspect override widens the layer's content through the native affine.
        check(expectations, *device.device, evaluator, directory, image, ocioContext, 95000,
              "pixel aspect 2:1", false, {}, 0, 2.0);
        check(expectations, *device.device, evaluator, directory, matted, ocioContext, 95100,
              "pixel aspect 10:11 with a matte", false, {}, 1, 10.0 / 11.0);
        checkAspectChange(expectations, *device.device, evaluator, directory, image, ocioContext,
                          95200);
        // Camera log inputs Bloom synthesizes (#381) and the film-scan inputs DPX import uses (#426): the input->working OCIO transform runs as a
        // native program with CPU parity, with no footage pass.
        // Nominal log plates carry code values in [0, 1]; out-of-range codes follow below.
        const auto logPlate = directory / "log.exr";
        writeExrRgba(logPlate, 3, 2,
                     {ExrPixel{0.1F, 0.4F, 0.7F, 1.0F}, ExrPixel{0.9F, 0.5F, 0.2F, 1.0F},
                      ExrPixel{0.41F, 0.41F, 0.41F, 1.0F}, ExrPixel{0.0F, 0.05F, 0.5F, 0.5F},
                      ExrPixel{0.0F, 0.0F, 0.0F, 0.0F}, ExrPixel{0.15F, 0.075F, 0.2F, 0.25F}});
        const auto logImage = imageAsset(logPlate, "log", 702);
        std::uint64_t base = 93600;
        for (const auto* space :
             {"S-Log3 SGamut3 (Bloom)", "LogC4 AWG4 (Bloom)", "V-Log V-Gamut (Bloom)",
              "Apple Log (Bloom)", "Log3G10 RWG (Bloom)", "C-Log3 CGamut (Bloom)",
              "Cineon Log (Bloom)"}) {
            auto camera = logImage;
            camera.interpretation.inputColorSpaceId = space;
            check(expectations, *device.device, evaluator, directory, camera, ocioContext, base,
                  std::string("camera log ") + space, false, {}, 0);
            check(expectations, *device.device, evaluator, directory, camera, ocioContext,
                  base + 50, std::string("camera log under ACEScg ") + space, true, {}, 0);
            base += 100;
        }
        // Out-of-range camera code values (#412): a negative ramp (below the encoding's black)
        // and an over-range ramp up to 4.0 (beyond the encoding's white), opaque and translucent
        // (premultiplied 2.0 at alpha 0.5 is the straight code 4.0). Every synthesized camera log
        // space must render them natively with the same CPU-oracle parity as in-range codes; the
        // GPU program is never refused and never replaced by a CPU frame.
        const auto rangePlate = directory / "log_out_of_range.exr";
        writeExrRgba(rangePlate, 4, 3,
                     {ExrPixel{-0.5F, -0.25F, -1.0F, 1.0F}, ExrPixel{-0.25F, -0.1F, -0.5F, 1.0F},
                      ExrPixel{-0.1F, -0.02F, -0.05F, 1.0F}, ExrPixel{-0.02F, 0.1F, -0.25F, 1.0F},
                      ExrPixel{1.0F, 1.05F, 0.9F, 1.0F}, ExrPixel{1.5F, 1.25F, 1.1F, 1.0F},
                      ExrPixel{2.5F, 2.0F, 3.0F, 1.0F}, ExrPixel{4.0F, 3.5F, 0.5F, 1.0F},
                      ExrPixel{2.0F, -0.125F, 1.0F, 0.5F}, ExrPixel{-0.25F, 0.75F, 0.6F, 0.5F},
                      ExrPixel{4.0F, 4.0F, 4.0F, 1.0F}, ExrPixel{-1.0F, -1.0F, -1.0F, 1.0F}});
        const auto rangeImage = imageAsset(rangePlate, "log_out_of_range", 703);
        base = 96000;
        for (const auto* space :
             {"S-Log3 SGamut3 (Bloom)", "S-Log3 SG3.Cine (Bloom)", "Venice SG3 (Bloom)",
              "Venice SG3.Cine (Bloom)", "LogC3 AWG3 (Bloom)", "LogC4 AWG4 (Bloom)",
              "V-Log V-Gamut (Bloom)", "Log3G10 RWG (Bloom)", "C-Log2 CGamut (Bloom)",
              "C-Log3 CGamut (Bloom)", "Apple Log (Bloom)", "Cineon Log (Bloom)"}) {
            auto camera = rangeImage;
            camera.interpretation.inputColorSpaceId = space;
            check(expectations, *device.device, evaluator, directory, camera, ocioContext, base,
                  std::string("out-of-range camera log ") + space, false, {}, 0);
            check(expectations, *device.device, evaluator, directory, camera, ocioContext,
                  base + 50, std::string("out-of-range camera log under ACEScg ") + space, true, {},
                  0);
            base += 100;
        }
        if (!expectations.ok()) {
            std::cerr << "FAIL: native footage interpretation expectations failed\n";
            return 1;
        }
        std::cout << "PASS: native footage interpretation pass vs CPU oracle\n";
        return 0;
#endif
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
