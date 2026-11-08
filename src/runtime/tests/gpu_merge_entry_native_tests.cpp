// #502 Merge entry layers and Adjustment entries on the GPU. Real documents compiled by the
// production snapshot compiler are prepared by the production CpuGpuSceneBuilder (through the
// shared preparer resolved from the packaged shader tools) and executed on the production
// GpuSceneExecutor; every readback must match the unchanged CPU oracle within the native parity
// tolerance, full frame and ROI:
//   * Adjustment -> Curves over a stack, its entry layer at a partial opacity with a moved, rotated
//     and masked coverage, under an entry layer with a Screen blend;
//   * the same Adjustment at its defaults with a Difference blend (the blended Merge only);
//   * entry layers with a frame pivot, a parent and a track matte, beside a chain Layer whose blend
//     mode the Merge reads through an effect;
//   * consecutive raster Layer nodes, concatenated into one resample, the later one parented.
// Without the packaged tools or a device the test reports SKIP (77), or FAIL under
// --require-device.

#include "gpu_coverage_fixture_support.hpp"
#include "gpu_coverage_native_support.hpp"
#include "gpu_coverage_ocio_support.hpp"
#include "merge_entry_test_support.hpp"

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using bloom::merge_entry_test::Builder;
using bloom::merge_entry_test::LayerValues;

constexpr std::uint64_t kSceneBudget = 1U << 28U;
int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void mask(Builder& builder, const bloom::document::LayerId layer) {
    bloom::document::LayerMask value;
    value.id = builder.id<bloom::document::LayerMaskId>();
    value.name = "Mask";
    bloom::document::PathValue path;
    path.editableContour().closed = true;
    for (const auto& [x, y] :
         {std::pair{2.0, 1.5}, std::pair{13.0, 2.5}, std::pair{12.0, 10.5}, std::pair{3.0, 9.5}})
        path.editableContour().anchors.push_back({{x, y}, std::nullopt, std::nullopt});
    value.path = builder.constant(bloom::document::kLayerMaskPathSchemaKey, path);
    value.feather = builder.constant(bloom::document::kLayerMaskFeatherSchemaKey, 1.5);
    value.expansion = builder.constant(bloom::document::kLayerMaskExpansionSchemaKey, 0.0);
    value.opacity = builder.constant(bloom::document::kLayerMaskOpacitySchemaKey, 0.9);
    builder.boundary(layer).masks.push_back(value);
}

void below(Builder& builder) {
    const auto blue = builder.solid({0.1, 0.2, 0.9, 0.75}, 6.0, 4.0);
    const auto [blueNode, blueLayer] = builder.layerNode(
        blue,
        {.position = {6.0, 5.0}, .rotation = 10.0, .pivot = bloom::document::LayerPivot::Content});
    builder.slot(builder.root(), blueNode, blueLayer);
    builder.slot(builder.root(), builder.solid({0.8, 0.1, 0.05, 1.0}, 16.0, 12.0));
}

[[nodiscard]] bloom::document::Project partialAdjustment() {
    Builder builder;
    below(builder);
    const auto slot = builder.topSlot(builder.root(), builder.curves(builder.adjustment()));
    const auto layer = builder.entryLayer(
        builder.root(), slot, {.position = {6.5, 7.0}, .rotation = 15.0, .opacity = 0.6});
    mask(builder, layer);
    const auto green = builder.solid({0.2, 0.7, 0.3, 0.8}, 5.0, 5.0);
    const auto top = builder.topSlot(builder.root(), green);
    builder.entryLayer(builder.root(), top,
                       {.position = {10.0, 4.0}, .blendMode = bloom::core::BlendMode::Screen});
    return builder.project();
}

[[nodiscard]] bloom::document::Project blendedAdjustment() {
    Builder builder;
    below(builder);
    const auto slot = builder.topSlot(builder.root(), builder.curves(builder.adjustment()));
    builder.entryLayer(builder.root(), slot, {.blendMode = bloom::core::BlendMode::Difference});
    return builder.project();
}

[[nodiscard]] bloom::document::Project entryLayers() {
    Builder builder;
    const auto red = builder.solid({0.8, 0.1, 0.05, 1.0}, 10.0, 8.0);
    const auto blue = builder.solid({0.1, 0.2, 0.9, 1.0}, 6.0, 4.0);
    const auto green = builder.solid({0.2, 0.7, 0.3, 0.75}, 5.0, 5.0);
    const auto greenSlot = builder.slot(builder.root(), green);
    const auto blueSlot = builder.slot(builder.root(), blue);
    const auto redSlot = builder.slot(builder.root(), red);
    const auto greenLayer = builder.entryLayer(builder.root(), greenSlot,
                                               {.position = {7.5, 6.5}, .scale = {1.5, 0.75}});
    const auto blueLayer =
        builder.entryLayer(builder.root(), blueSlot, {.position = {9.0, 7.0}, .rotation = 30.0});
    const auto redLayer = builder.entryLayer(builder.root(), redSlot, {.position = {7.0, 5.0}});
    builder.boundary(greenLayer).parent = blueLayer;
    builder.boundary(greenLayer).trackMatte =
        bloom::document::TrackMatte{redLayer, bloom::document::TrackMatteChannel::Alpha, false};
    // A chain Layer: Layer (Multiply) -> Curves -> Merge.
    const auto white = builder.solid({0.9, 0.9, 0.9, 1.0}, 8.0, 6.0);
    const auto [chainNode, chainLayer] = builder.layerNode(
        white, {.position = {9.0, 5.0}, .blendMode = bloom::core::BlendMode::Multiply});
    static_cast<void>(chainLayer);
    builder.topSlot(builder.root(), builder.curves(chainNode));
    return builder.project();
}

// Consecutive raster Layer nodes (#502) concatenate into one resample on the CPU and the GPU: a
// rotated, scaled, translucent Layer over raster content, then a parented, rotated Layer.
[[nodiscard]] bloom::document::Project concatenatedLayers() {
    Builder builder;
    const auto inner = builder.merge();
    const auto blue = builder.solid({0.1, 0.2, 0.9, 1.0}, 6.0, 4.0);
    const auto [blueNode, blueLayer] = builder.layerNode(
        blue, {.position = {6.0, 5.0}, .pivot = bloom::document::LayerPivot::Content});
    builder.slot(inner, blueNode, blueLayer);
    builder.slot(inner, builder.solid({0.8, 0.1, 0.05, 1.0}, 12.0, 9.0));
    const auto [first, firstLayer] = builder.layerNode(
        inner, {.position = {7.0, 6.5}, .scale = {1.3, 0.9}, .rotation = 25.0, .opacity = 0.8});
    static_cast<void>(firstLayer);
    const auto [second, secondLayer] =
        builder.layerNode(first, {.position = {8.5, 5.5}, .rotation = -10.0, .opacity = 0.9});
    builder.slot(builder.root(), second, secondLayer);
    const auto [parentNode, parentLayer] = builder.layerNode(
        builder.solid({0.2, 0.7, 0.3, 0.5}, 4.0, 4.0), {.position = {8.5, 6.0}, .rotation = 12.0});
    builder.slot(builder.root(), parentNode, parentLayer);
    builder.boundary(secondLayer).parent = parentLayer;
    return builder.project();
}

struct Options final {
    std::filesystem::path loader;
    bool requireDevice = false;
    bool valid = true;
};

[[nodiscard]] Options parse(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc)
            options.loader = argv[++index];
        else if (argument == "--require-device")
            options.requireDevice = true;
        else
            options.valid = false;
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parse(argc, argv);
    if (!options.valid) {
        std::cerr << "invalid arguments\n";
        return 2;
    }
    const auto context = bloom::gpu_coverage_ocio::sharedContext();
    if (context == nullptr || context->preparer == nullptr) {
        if (options.requireDevice) {
            std::cerr << "FAIL: the packaged shader tools are unavailable: "
                      << bloom::gpu_coverage_ocio::unavailableReason() << '\n';
            return 1;
        }
        std::cout << "SKIP: the packaged shader tools are unavailable: "
                  << bloom::gpu_coverage_ocio::unavailableReason() << '\n';
        return 77;
    }
    const bloom::runtime::CpuGpuSceneBuilder builder(nullptr, {}, *context);
    struct Case final {
        std::string name;
        std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> plan;
        bloom::runtime::EvaluationRequest request;
        std::shared_ptr<const bloom::runtime::PreparedGpuScene> scene;
    };
    std::vector<Case> cases;
    for (const auto& [name, project] :
         {std::pair{std::string("partial adjustment"), partialAdjustment()},
          std::pair{std::string("blended adjustment"), blendedAdjustment()},
          std::pair{std::string("entry layers"), entryLayers()},
          std::pair{std::string("concatenated layers"), concatenatedLayers()}}) {
        const auto compiled = bloom::merge_entry_test::compile(project);
        expect(compiled.plan != nullptr, name + " compiles");
        if (!compiled.plan)
            continue;
        for (const bool roi : {false, true}) {
            auto request = bloom::gpu_coverage_fixtures::requestFor(*compiled.plan);
            if (roi) {
                const auto window = bloom::render::ImageWindow::create(3, 2, 9, 7);
                request.roi = *window.value();
            }
            const auto label = name + (roi ? " (ROI)" : "");
            const auto prepared = builder.build(compiled.plan, request);
            expect(static_cast<bool>(prepared),
                   label +
                       " prepares through the production builder: " + prepared.diagnostic.message);
            if (prepared)
                cases.push_back({label, compiled.plan, request, prepared.scene});
        }
    }
    std::cout << "PREPARE scenes=" << cases.size() << '\n';

    bloom::render::GpuDeviceCreationOptions deviceOptions;
    deviceOptions.loader_path = options.loader;
    auto device = bloom::render::GpuDevice::create(deviceOptions);
    if (!device) {
        if (options.requireDevice) {
            std::cerr << "FAIL: --require-device was requested but no native GPU device is "
                         "available: "
                      << device.diagnostic.message << '\n';
            return 1;
        }
        std::cout << "SKIP: no native GPU device available: " << device.diagnostic.message << '\n';
        return failures == 0 ? 77 : 1;
    }
    auto cacheResult = bloom::runtime::GpuSceneCache::create(*device.device);
    expect(static_cast<bool>(cacheResult), "the scene cache is created");
    if (!cacheResult)
        return 1;
    auto executorResult =
        bloom::runtime::GpuSceneExecutor::create(*device.device, *cacheResult.cache);
    expect(static_cast<bool>(executorResult), "the scene executor is created");
    if (!executorResult)
        return 1;
    auto& executor = *executorResult.executor;
    const bloom::runtime::CpuCompositionEvaluator oracle;
    std::size_t matched = 0;
    for (const auto& item : cases) {
        if (const auto diagnostic = executor.begin(item.scene, kSceneBudget);
            diagnostic.code != bloom::runtime::GpuSceneExecutorDiagnosticCode::None) {
            expect(false, item.name + " begins: " + diagnostic.message);
            continue;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        auto poll = executor.poll();
        while (poll == bloom::runtime::GpuSceneExecutorPollResult::Pending &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            poll = executor.poll();
        }
        expect(poll == bloom::runtime::GpuSceneExecutorPollResult::Ready,
               item.name + " reaches Ready: " + executor.diagnostic().message);
        const auto* image =
            poll == bloom::runtime::GpuSceneExecutorPollResult::Ready ? executor.image() : nullptr;
        expect(image != nullptr, item.name + " publishes a resident image");
        if (image != nullptr) {
            auto oracleRequest = item.request;
            oracleRequest.bypassOperationCache = true;
            const auto cpu = oracle.evaluate(item.plan, oracleRequest, {});
            const auto readback = bloom::render::readbackResidentImage(*image, kSceneBudget);
            expect(cpu.frame() != nullptr && static_cast<bool>(readback),
                   item.name + " has a CPU oracle and a test readback");
            if (cpu.frame() != nullptr && readback) {
                const auto& pixels = cpu.frame()->processImage().pixels();
                const bool close = readback.pixels.size() == pixels.size() &&
                                   bloom::gpu_coverage_native::pixelsClose(readback.pixels, pixels);
                expect(close,
                       item.name + " matches the CPU oracle" +
                           bloom::gpu_coverage_native::pixelParityDetail(readback.pixels, pixels));
                matched += close ? 1 : 0;
            }
        }
        (void)executor.takeImage();
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: " << matched << " merge entry and adjustment scenes match the CPU oracle\n";
    return 0;
}
