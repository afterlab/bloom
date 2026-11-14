// #724 Scopes: the scope measure of a frame through the production device route.
//
// The real GpuProcessFrameEvaluator runs a two-layer scene and, as a scope measure job, counts the
// resident picture on the device and reads back only the tables. The OCIO context is qualified
// through the real GpuOcioContextResolver from this target's own packaging, the seam the desktop,
// CLI and MCP roots use.
//
// What is asserted:
//   * Working signal: the device's tables equal the CPU reference's, cell for cell, for the same
//     float picture (the picture the same evaluator reads back for the same request), with and
//     without a region, a region of interest, a proxy resolution and ignored transparent pixels.
//   * Display signal and the Chromaticity table: OCIO's CPU and GPU processors round differently,
//     so a pixel may land in the neighboring cell. Every pixel is counted, and along every table
//     axis no pixel is more than one cell from where the CPU reference puts it.
//   * The job reads back one set of tables and no picture.
//   * measureScopeFrame names the route that counted, and says why when the device did not.
//
// Requires the Vulkan backend and the pinned shader tools; skips (77) otherwise.

#include "final_host_test_support.hpp"
#include "gpu_composite_native_support.hpp"
#include "gpu_scene_executor_test_support.hpp"
#include "scope_tables_compare_support.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/gpu_scope_measure.hpp>
#include <bloom/runtime/scope_frame_measure.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using bloom::core::Color4d;
using bloom::render::ImageWindow;
using bloom::render::kScopeChannels;
using bloom::render::kScopeCodeOver;
using bloom::render::kScopeCodeUnder;
using bloom::render::kScopeTableAll;
using bloom::render::kScopeTableChromaticity;
using bloom::render::kScopeTableHistogram;
using bloom::render::kScopeTableVectorscope;
using bloom::render::kScopeTableWaveform;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::EvaluationRequest;
using bloom::runtime::GpuOcioContextRequest;
using bloom::runtime::GpuOcioContextResolver;
using bloom::runtime::GpuProcessFrameEvaluator;
using bloom::runtime::GpuProcessFrameEvaluatorOptions;
using bloom::runtime::GpuProcessFrameStatus;
using bloom::runtime::ScopeCounts;
using bloom::runtime::ScopeFrameMeasure;
using bloom::runtime::ScopeFrameRoutes;
using bloom::runtime::ScopeFrameStatus;
using bloom::runtime::ScopeMeasuredArea;
using bloom::runtime::ScopeMeasureSettings;
using bloom::runtime::ScopeRegion;
using bloom::runtime::ScopeRoute;
using bloom::runtime::ScopeSignal;
using bloom::runtime::ScopeSignalColor;
using bloom::runtime::ViewAdjust;

using namespace bloom::runtime::executor_test;

#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE

[[nodiscard]] std::optional<bloom::core::Sha256Digest> parseDigest(const std::string_view text) {
    constexpr std::string_view prefix = "sha256:";
    if (text.size() != prefix.size() + bloom::core::kSha256HexCharacters ||
        text.substr(0, prefix.size()) != prefix) {
        return std::nullopt;
    }
    return bloom::core::Sha256Digest::fromLowercaseHex(text.substr(prefix.size()));
}

[[nodiscard]] bloom::color::GpuShaderToolPackage packagedToolsDescriptor() {
    bloom::color::GpuShaderToolPackage package;
    package.toolsDirectory = BLOOM_GPU_TOOLS_DIR;
    package.inventoryName = BLOOM_GPU_TOOLS_INVENTORY_NAME;
    package.glslangValidatorName = BLOOM_GPU_TOOLS_GLSLANG_NAME;
    package.spirvValName = BLOOM_GPU_TOOLS_SPIRV_VAL_NAME;
#if defined(BLOOM_GPU_TOOLS_RELOCATED)
    package.relocated = BLOOM_GPU_TOOLS_RELOCATED != 0;
#endif
#if defined(BLOOM_GPU_TOOLS_BUNDLE_RELATIVE)
    package.bundleRelative = true;
#endif
#if defined(BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256)
    package.glslangStagedDigest = parseDigest(BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256);
#endif
#if defined(BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256)
    package.spirvValStagedDigest = parseDigest(BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256);
#endif
    return package;
}

using bloom::runtime::scope_test::histogramAxis;
using bloom::runtime::scope_test::tablesWithinOneCell;

int run(int argc, char** argv) {
    const auto options = bloom::render::composite_proof::parseOptions(argc, argv);
    if (!options.valid) {
        std::cerr << "invalid arguments\n";
        return 2;
    }
    Expectations expectations;

    GpuOcioContextResolver resolver{
        GpuOcioContextRequest{std::filesystem::path{BLOOM_SCOPE_FRAME_MEASURE_TEST_EXECUTABLE},
                              packagedToolsDescriptor(),
                              {},
                              {}}};
    const auto resolved = resolver.resolve();
    expectations.expect(resolved.hasValue(), "the packaged tools resolve to a shared context");
    if (!resolved.hasValue()) {
        std::cerr << "resolver diagnostic: " << resolved.diagnostic << '\n';
        return 1;
    }
    const auto ocioContext = resolved.context;

    // A fixed host sample: the test does not depend on the memory this machine has free.
    bloom::render::test::FinalHostRetirementScope host;
    auto phasePolicy = std::make_shared<bloom::runtime::GpuProcessingPhasePolicy>(32, host.owner);
    {
        constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
        bloom::runtime::MemoryBudgetState sample;
        sample.machine.availableBytes = 60 * gib;
        sample.process.status = bloom::runtime::ProcessMemoryStatus::Finite;
        sample.process.source = bloom::runtime::ProcessMemorySource::LinuxCgroupV2;
        sample.process.sampledAt = bloom::runtime::GpuProcessingPhasePolicy::Clock::now();
        sample.process.domains.push_back({.path = "/fixture",
                                          .kind = bloom::runtime::ProcessMemoryLimitKind::Finite,
                                          .limitBytes = 8 * gib,
                                          .currentBytes = 6 * gib});
        sample.processSampleFresh = true;
        sample.sampleSerial = 1;
        phasePolicy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    }

    CpuCompositionEvaluator cpuEvaluator;
    GpuProcessFrameEvaluatorOptions evaluatorOptions;
    evaluatorOptions.processingPhasePolicy = phasePolicy;
    evaluatorOptions.enabled = true;
    evaluatorOptions.loaderPath = options.loader_path;
    evaluatorOptions.ocioContext = ocioContext;
    evaluatorOptions.mediaContext =
        bloom::runtime::GpuSceneMediaContext::fromEvaluator(cpuEvaluator);
    std::shared_ptr<GpuProcessFrameEvaluator> evaluator =
        GpuProcessFrameEvaluator::create(evaluatorOptions);
    if (evaluator == nullptr || !evaluator->gpuAvailable()) {
        if (options.require_device) {
            std::cerr << "FAIL: required device unavailable: "
                      << (evaluator != nullptr ? evaluator->availabilityDiagnostic().message
                                               : std::string{"no evaluator"})
                      << '\n';
            return 1;
        }
        std::cout << "SKIP: no compatible Vulkan device available\n";
        return 77;
    }

    auto neutral =
        bloom::color::resolveOcioBuiltIn(bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                         bloom::color::kBloomNeutralV1ConfigUri,
                                         bloom::color::ocioBuiltInContentRevision(
                                             bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                                             bloom::color::kBloomNeutralV1ConfigUri)
                                             .value_or(bloom::color::kBloomNeutralV1ConfigDigest),
                                         "lin_rec709_scene")
            .takeResolved();
    expectations.expect(neutral.has_value(), "Bloom Neutral resolves");
    if (!neutral) {
        evaluator->beginShutdown();
        return 1;
    }
    const auto config =
        std::make_shared<const bloom::color::ResolvedBloomNeutralConfig>(std::move(*neutral));

    // An opaque solid under a half-transparent one with a value above 1 and one below 0, in a
    // 256 x 256 composition they do not fill: flat areas, an overlap, and transparent pixels.
    const auto plan =
        twoSolidPlan(format(256, 256), Color4d{0.43, 0.27, 0.13, 1.0},
                     LayerValues{.position = {10.0, 20.0}}, Color4d{-0.2, 3.2, 0.35, 0.5},
                     LayerValues{.position = {60.0, 50.0}, .opacity = 0.8}, 180.0, 150.0, 7000);
    const auto request = requestFor(*plan);

    const auto colorFor = [&](const ScopeSignal signal, const ViewAdjust adjust = {}) {
        auto prepared = ScopeSignalColor::prepare(
            {.config = config, .signal = signal, .display = {}, .view = {}, .viewAdjust = adjust});
        expectations.expect(static_cast<bool>(prepared),
                            "the scope signal's color objects prepare: " + prepared.message);
        return prepared.color;
    };
    const auto working = colorFor(ScopeSignal::Working);
    const auto display = colorFor(ScopeSignal::Display);
    const auto adjusted = colorFor(ScopeSignal::Display, ViewAdjust{.exposure = 0.5, .gamma = 1.2});
    if (working == nullptr || display == nullptr || adjusted == nullptr) {
        evaluator->beginShutdown();
        return 1;
    }
    expectations.expect(working->identity() != display->identity() &&
                            display->identity() != adjusted->identity(),
                        "a signal, and an adjustment, change the color identity");

    // The picture the device computes for a request, read back: the float input both routes of
    // the Working signal count.
    const auto devicePicture = [&](const EvaluationRequest& frameRequest) {
        auto outcome = evaluator->evaluate(plan, frameRequest);
        expectations.expect(outcome.status == GpuProcessFrameStatus::Evaluated &&
                                outcome.frame != nullptr,
                            "the device evaluates the frame: " + outcome.diagnostic.message);
        return outcome.frame;
    };
    const auto onDevice = [&](const EvaluationRequest& frameRequest,
                              const ScopeMeasureSettings& settings, const ScopeSignalColor& color) {
        return bloom::runtime::measureScopeFrameOnDevice(*evaluator, *ocioContext, plan,
                                                         frameRequest, settings, color);
    };
    const auto settingsFor = [](const ScopeSignal signal, const std::uint32_t scopes) {
        ScopeMeasureSettings settings;
        settings.signal = signal;
        settings.scopes = scopes;
        return settings;
    };
    constexpr std::uint32_t kNoChromaticity =
        kScopeTableHistogram | kScopeTableWaveform | kScopeTableVectorscope;

    const auto fullPicture = devicePicture(request);
    if (fullPicture == nullptr) {
        evaluator->beginShutdown();
        return 1;
    }

    // Working, exact, and the native work of one measure.
    {
        const auto settings = settingsFor(ScopeSignal::Working, kNoChromaticity);
        bloom::runtime::GpuScopeMeasureJob job;
        job.scopes = settings.scopes;
        job.luma = working->luma();
        const auto outcome = evaluator->measureScopes(plan, request, job);
        expectations.expect(outcome.status == bloom::runtime::GpuScopeMeasureStatus::Measured,
                            "the device measures the Working signal: " +
                                outcome.diagnostic.message);
        expectations.expect(outcome.tableReadbacks == 1 && outcome.pictureReadbacks == 0 &&
                                outcome.colorDispatches == 0,
                            "the Working measure reads back its tables and no picture");
        const auto reference =
            bloom::runtime::measureScopeFrameCpu(*fullPicture, settings, *working);
        expectations.expect(reference.status == ScopeFrameStatus::Measured &&
                                reference.route == ScopeRoute::Cpu,
                            "the CPU reference measures the device's picture");
        expectations.expect(outcome.counts == reference.counts,
                            "Working: the device's tables equal the CPU reference's");
        expectations.expect(
            (outcome.area == ScopeMeasuredArea{.x = 0, .y = 0, .width = 256, .height = 256}) &&
                outcome.counts.counted() == 256U * 256U,
            "the whole display window is measured, transparent pixels included");
        expectations.expect(outcome.counts.histogram(1, kScopeCodeOver) != 0 &&
                                outcome.counts.histogram(0, kScopeCodeUnder) != 0,
                            "values above the axis and below zero are counted over and under");
    }

    // Working with ignored transparent pixels, a region, a region of interest and a proxy.
    {
        auto settings = settingsFor(ScopeSignal::Working, kNoChromaticity);
        settings.ignoreTransparent = true;
        const auto measured = onDevice(request, settings, *working);
        const auto reference =
            bloom::runtime::measureScopeFrameCpu(*fullPicture, settings, *working);
        expectations.expect(measured.status == ScopeFrameStatus::Measured &&
                                measured.route == ScopeRoute::Gpu &&
                                measured.counts == reference.counts,
                            "ignored transparent pixels: the routes agree exactly");
        expectations.expect(measured.counts.counted() < 256U * 256U &&
                                measured.counts.counted() != 0,
                            "transparent pixels are not counted when ignored");
    }
    {
        auto settings = settingsFor(ScopeSignal::Working, kNoChromaticity);
        settings.region = ScopeRegion{.x = 40, .y = 30, .width = 100, .height = 90};
        const auto measured = onDevice(request, settings, *working);
        const auto reference =
            bloom::runtime::measureScopeFrameCpu(*fullPicture, settings, *working);
        expectations.expect(measured.status == ScopeFrameStatus::Measured &&
                                measured.counts == reference.counts &&
                                measured.area == reference.area,
                            "a region: the routes agree exactly");
        expectations.expect(
            (measured.area == ScopeMeasuredArea{.x = 40, .y = 30, .width = 100, .height = 90}) &&
                measured.counts.counted() == 100U * 90U && measured.counts.layout.columns == 100,
            "a region measures its own pixels in its own columns");

        settings.region = ScopeRegion{.x = 400, .y = 30, .width = 20, .height = 20};
        const auto outside = onDevice(request, settings, *working);
        expectations.expect(outside.status == ScopeFrameStatus::EmptyArea,
                            "a region outside the picture has nothing to count");
        expectations.expect(
            bloom::runtime::measureScopeFrameCpu(*fullPicture, settings, *working).status ==
                ScopeFrameStatus::EmptyArea,
            "the CPU reference reports the same empty area");
    }
    {
        auto roiRequest = request;
        const auto roiWindow = ImageWindow::create(32, 48, 96, 64);
        roiRequest.roi = *roiWindow.value();
        const auto picture = devicePicture(roiRequest);
        auto settings = settingsFor(ScopeSignal::Working, kNoChromaticity);
        settings.region = ScopeRegion{.x = 0, .y = 0, .width = 100, .height = 100};
        const auto measured = onDevice(roiRequest, settings, *working);
        expectations.expect(measured.status == ScopeFrameStatus::Measured,
                            "a region of interest is measured: " + measured.message);
        if (picture != nullptr) {
            const auto reference =
                bloom::runtime::measureScopeFrameCpu(*picture, settings, *working);
            expectations.expect(measured.counts == reference.counts &&
                                    measured.area == reference.area,
                                "a region of interest: the routes agree exactly");
        }
        expectations.expect(measured.counts.counted() == 68U * 52U,
                            "the region is cut to the region of interest");
    }
    {
        auto proxyRequest = request;
        const auto proxyExtent = bloom::render::ImageExtent::create(128, 128);
        proxyRequest.resolution = bloom::runtime::ProxyResolution{*proxyExtent.value()};
        const auto picture = devicePicture(proxyRequest);
        auto settings = settingsFor(ScopeSignal::Working, kNoChromaticity);
        settings.region = ScopeRegion{.x = 40, .y = 30, .width = 100, .height = 90};
        const auto measured = onDevice(proxyRequest, settings, *working);
        expectations.expect(measured.status == ScopeFrameStatus::Measured,
                            "a proxy frame is measured: " + measured.message);
        if (picture != nullptr) {
            const auto reference =
                bloom::runtime::measureScopeFrameCpu(*picture, settings, *working);
            expectations.expect(measured.counts == reference.counts,
                                "a proxy frame: the routes agree exactly");
        }
        expectations.expect(
            (measured.area == ScopeMeasuredArea{.x = 20, .y = 15, .width = 50, .height = 45}),
            "a region in composition pixels measures the same part of a proxy picture");
    }

    // The detail: a measure at detail 1 through the production device route reads back the
    // detail's bytes, once, and no picture; the tables are exact for Working without CIE and
    // within one cell of the detail's cells through the OCIO steps.
    {
        constexpr std::uint32_t kDetail = 1;
        auto settings = settingsFor(ScopeSignal::Working, kNoChromaticity);
        settings.detail = kDetail;
        const auto measured = onDevice(request, settings, *working);
        const auto reference =
            bloom::runtime::measureScopeFrameCpu(*fullPicture, settings, *working);
        expectations.expect(
            measured.status == ScopeFrameStatus::Measured && measured.route == ScopeRoute::Gpu &&
                reference.status == ScopeFrameStatus::Measured,
            "detail 1: the Working signal is measured on the device: " + measured.message);
        expectations.expect(measured.counts == reference.counts &&
                                measured.counts.layout.detail == kDetail,
                            "detail 1, Working: the device's tables equal the CPU reference's");
        const auto layout =
            bloom::render::scopeTableLayout(settings.scopes, measured.area.width, kDetail);
        expectations.expect(measured.counts.layout == layout &&
                                measured.tableReadbackBytes == layout.byteCount() &&
                                measured.tableReadbacks == 1 && measured.pictureReadbacks == 0 &&
                                measured.colorDispatches == 0,
                            "detail 1, Working: the layout's bytes, one table readback, no "
                            "picture readback and no color program");
        expectations.expect(reference.tableReadbackBytes == 0 && reference.tableReadbacks == 0,
                            "the CPU reference reads nothing back");
        // The coarser tables are the full ones pooled, and a smaller readback.
        const auto fullMeasure =
            onDevice(request, settingsFor(ScopeSignal::Working, kNoChromaticity), *working);
        expectations.expect(
            fullMeasure.status == ScopeFrameStatus::Measured &&
                fullMeasure.tableReadbackBytes ==
                    bloom::render::scopeTableLayout(kNoChromaticity, fullMeasure.area.width, 0)
                        .byteCount() &&
                fullMeasure.tableReadbackBytes > measured.tableReadbackBytes,
            "full detail reads back the full layout's bytes, more than detail 1");
        bool pooled = true;
        for (std::uint32_t row = 0; row < layout.cells() && pooled; ++row)
            for (std::uint32_t column = 0; column < layout.cells() && pooled; ++column)
                pooled = measured.counts.vectorscope(column, row) ==
                         fullMeasure.counts.vectorscope(column * 2, row * 2) +
                             fullMeasure.counts.vectorscope(column * 2 + 1, row * 2) +
                             fullMeasure.counts.vectorscope(column * 2, row * 2 + 1) +
                             fullMeasure.counts.vectorscope(column * 2 + 1, row * 2 + 1);
        expectations.expect(pooled, "detail 1 on the device is the full detail's cells pooled");
    }
    {
        // Display with the Chromaticity table at detail 1: OCIO's processors, within one cell of
        // the detail's cells.
        auto settings = settingsFor(ScopeSignal::Display, kScopeTableAll);
        settings.detail = 1;
        const auto measured = onDevice(request, settings, *display);
        const auto reference =
            bloom::runtime::measureScopeFrameCpu(*fullPicture, settings, *display);
        expectations.expect(measured.status == ScopeFrameStatus::Measured &&
                                reference.status == ScopeFrameStatus::Measured,
                            "detail 1: Display with Chromaticity is measured: " + measured.message);
        expectations.expect(tablesWithinOneCell(measured.counts, reference.counts),
                            "detail 1, Display: no pixel is more than one detail cell away");
        expectations.expect(
            measured.tableReadbackBytes ==
                    bloom::render::scopeTableLayout(kScopeTableAll, measured.area.width, 1)
                        .byteCount() &&
                measured.tableReadbacks == 1 && measured.pictureReadbacks == 0,
            "detail 1, Display: the layout's bytes in one table readback and no picture");
        // The color programs follow the tables: with the Chromaticity table the Display
        // measure also runs the XYZ program, without it only the display program.
        settings.scopes = kNoChromaticity;
        const auto noXyz = onDevice(request, settings, *display);
        expectations.expect(noXyz.status == ScopeFrameStatus::Measured &&
                                noXyz.colorDispatches != 0 &&
                                measured.colorDispatches > noXyz.colorDispatches,
                            "a measure without the Chromaticity table runs no XYZ program");
    }
    {
        // One scope: only its table is allocated, counted and read back.
        for (const std::uint32_t scope : {kScopeTableHistogram, kScopeTableWaveform,
                                          kScopeTableVectorscope, kScopeTableChromaticity}) {
            auto settings = settingsFor(ScopeSignal::Working, scope);
            const auto measured = onDevice(request, settings, *working);
            expectations.expect(
                measured.status == ScopeFrameStatus::Measured &&
                    measured.tableReadbackBytes ==
                        bloom::render::scopeTableLayout(scope, measured.area.width, 0)
                            .byteCount() &&
                    measured.tableReadbacks == 1 && measured.pictureReadbacks == 0,
                "one scope reads back its own table: " + std::to_string(scope));
            if (scope == kScopeTableHistogram) {
                expectations.expect(measured.tableReadbackBytes == (8U + 4U * 1026U) * 4U &&
                                        measured.colorDispatches == 0,
                                    "a Histogram-only measure reads 8 counters and 4104 words "
                                    "and runs no color program");
            }
        }
        const auto displayHistogram =
            onDevice(request, settingsFor(ScopeSignal::Display, kScopeTableHistogram), *display);
        expectations.expect(displayHistogram.status == ScopeFrameStatus::Measured &&
                                displayHistogram.tableReadbackBytes == (8U + 4U * 1026U) * 4U,
                            "a Display Histogram-only measure reads its small tables");
    }

    // Bytes per measure at the live detail (docs/architecture/scopes.md, "Real-time cost"): a
    // measure reads back only the open sections' tables and the layout's bytes at its detail. The
    // picture is 256 wide, so the Waveform has 256 columns at both details: all tables are
    // 1,597,504 bytes at the live detail (the same as a picture 512 or more wide) and 4,218,944 at
    // full detail (6,324,288 for a picture 512 or more wide).
    {
        constexpr std::uint32_t kLive = 1;
        auto histogram = settingsFor(ScopeSignal::Working, kScopeTableHistogram);
        histogram.detail = kLive;
        const auto one = onDevice(request, histogram, *working);
        expectations.expect(
            one.status == ScopeFrameStatus::Measured &&
                one.tableReadbackBytes ==
                    bloom::render::scopeTableLayout(kScopeTableHistogram, one.area.width, kLive)
                        .byteCount() &&
                one.tableReadbackBytes == (8U + 4U * 1026U) * 4U && one.tableReadbacks == 1 &&
                one.pictureReadbacks == 0,
            "live detail, one section (Histogram): exactly its layout's 16,448 bytes");

        auto all = settingsFor(ScopeSignal::Working, kScopeTableAll);
        all.detail = kLive;
        const auto live = onDevice(request, all, *working);
        expectations.expect(
            live.status == ScopeFrameStatus::Measured &&
                live.tableReadbackBytes ==
                    bloom::render::scopeTableLayout(kScopeTableAll, live.area.width, kLive)
                        .byteCount() &&
                live.tableReadbackBytes == 1'597'504U && live.tableReadbacks == 1 &&
                live.pictureReadbacks == 0,
            "live detail, every section: exactly the layout's 1,597,504 bytes");

        all.detail = 0;
        const auto full = onDevice(request, all, *working);
        expectations.expect(full.status == ScopeFrameStatus::Measured &&
                                full.tableReadbackBytes == bloom::render::scopeTableLayout(
                                                               kScopeTableAll, full.area.width, 0)
                                                               .byteCount() &&
                                full.tableReadbackBytes == 4'218'944U && full.tableReadbacks == 1 &&
                                full.pictureReadbacks == 0,
                            "full detail, every section: exactly the layout's 4,218,944 bytes");
    }

    // The Chromaticity table and the Display signal: OCIO's two processors, within one cell.
    {
        const auto settings = settingsFor(ScopeSignal::Working, kScopeTableAll);
        const auto measured = onDevice(request, settings, *working);
        const auto reference =
            bloom::runtime::measureScopeFrameCpu(*fullPicture, settings, *working);
        expectations.expect(measured.status == ScopeFrameStatus::Measured &&
                                reference.status == ScopeFrameStatus::Measured,
                            "Working with Chromaticity is measured: " + measured.message);
        expectations.expect(tablesWithinOneCell(measured.counts, reference.counts),
                            "Working with Chromaticity: no pixel is more than one cell away");
        bool exact = true;
        for (std::uint32_t channel = 0; channel < kScopeChannels && exact; ++channel)
            exact =
                histogramAxis(measured.counts, channel) == histogramAxis(reference.counts, channel);
        expectations.expect(exact, "the Histogram does not depend on the XYZ step");
    }
    for (const auto& [color, name] :
         {std::pair{display, std::string_view("Display")},
          std::pair{adjusted, std::string_view("Display with exposure and gamma")}}) {
        const auto settings = settingsFor(ScopeSignal::Display, kScopeTableAll);
        const auto measured = onDevice(request, settings, *color);
        const auto reference = bloom::runtime::measureScopeFrameCpu(*fullPicture, settings, *color);
        expectations.expect(measured.status == ScopeFrameStatus::Measured &&
                                measured.route == ScopeRoute::Gpu,
                            std::string(name) + " is measured on the device: " + measured.message);
        expectations.expect(reference.status == ScopeFrameStatus::Measured,
                            std::string(name) +
                                " is measured by the CPU reference: " + reference.message);
        expectations.expect(tablesWithinOneCell(measured.counts, reference.counts),
                            std::string(name) + ": no pixel is more than one cell away");
        expectations.expect(measured.counts.counted() == 256U * 256U,
                            std::string(name) + ": every pixel is counted");
    }
    {
        const auto neutralCounts =
            onDevice(request, settingsFor(ScopeSignal::Display, kScopeTableHistogram), *display);
        const auto adjustedCounts =
            onDevice(request, settingsFor(ScopeSignal::Display, kScopeTableHistogram), *adjusted);
        expectations.expect(neutralCounts.counts != adjustedCounts.counts,
                            "the Viewer's exposure and gamma change the Display signal");
        const auto workingCounts =
            onDevice(request, settingsFor(ScopeSignal::Working, kScopeTableHistogram), *working);
        expectations.expect(neutralCounts.counts != workingCounts.counts,
                            "the Display and Working signals differ");
    }

    // measureScopeFrame: the route is named, and the CPU reference says why it counted.
    {
        const auto settings = settingsFor(ScopeSignal::Working, kNoChromaticity);
        ScopeFrameRoutes routes;
        routes.device = [&evaluator] { return evaluator; };
        routes.ocio = [&ocioContext] { return ocioContext; };
        routes.evaluateCpu = [&cpuEvaluator](auto framePlan, const EvaluationRequest& frameRequest,
                                             const auto& cancellation) {
            return cpuEvaluator.evaluate(std::move(framePlan), frameRequest, cancellation);
        };
        const auto device =
            bloom::runtime::measureScopeFrame(routes, plan, request, settings, *working, true);
        expectations.expect(device.status == ScopeFrameStatus::Measured &&
                                device.route == ScopeRoute::Gpu && device.message.empty(),
                            "a device frame is measured on the device");
        const auto cpu =
            bloom::runtime::measureScopeFrame(routes, plan, request, settings, *working, false);
        expectations.expect(cpu.status == ScopeFrameStatus::Measured &&
                                cpu.route == ScopeRoute::Cpu && cpu.message.empty(),
                            "a CPU frame is measured by the CPU reference");
        expectations.expect(tablesWithinOneCell(device.counts, cpu.counts),
                            "the two routes' frames measure within one cell of each other");
        routes.device = [] { return std::shared_ptr<GpuProcessFrameEvaluator>(); };
        const auto fallback =
            bloom::runtime::measureScopeFrame(routes, plan, request, settings, *working, true);
        expectations.expect(fallback.status == ScopeFrameStatus::Measured &&
                                fallback.route == ScopeRoute::Cpu && !fallback.message.empty() &&
                                fallback.counts == cpu.counts,
                            "without a device the CPU reference counts and says why");
    }

    evaluator->beginShutdown();
    evaluator.reset();
    if (!expectations.ok())
        return 1;
    std::cout << "PASS: scope frame measure on the device route\n";
    return 0;
}

#else

int run(int argc, char** argv) {
    const auto options = bloom::render::composite_proof::parseOptions(argc, argv);
    if (options.require_device) {
        std::cerr << "FAIL: required device unavailable: packaged GPU shader tools unavailable\n";
        return 1;
    }
    std::cout << "SKIP: packaged GPU shader tools unavailable\n";
    return 77;
}

#endif

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
