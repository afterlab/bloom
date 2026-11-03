// Real-device proof for the scene executor's upload lookahead (#405).
//
// A three-source media plan (three distinct EXR stills, each through its own transformed layer into
// one Normal merge) plans three Upload steps with GPU work between them. With the lookahead on, the
// next upload is begun on the executor's second upload pipeline while the job in front of it runs,
// and completes at its own step. The proofs:
//  * ordering: an upload is begun before its own step (while the first job is in flight), every
//    upload still completes, publishes and is cached at its step, and the output is bit-identical
//    to the same executor with the lookahead off and within the 2e-6 gate of the uncached CPU
//    frame;
//  * cancellation mid-upload: a cancel while a lookahead upload is in flight publishes nothing,
//    drops the lookahead, retires every submission on owner polls alone, and leaves the executor
//    reusable with no reserved bytes left behind;
//  * fallback: with the lookahead off, or with a request budget that is exactly the serial live
//    peak (so a lookahead beside the in-flight job is refused or dropped and retried), the scene
//    runs one native job at a time with the identical output. Both submissions use the device's
//    one queue, so this is also the path of a device without a separate transfer queue.

#include "gpu_media_scene_preparation_test_support.hpp"

#include "gpu_media_executor_test_support.hpp"

#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using bloom::render::GpuDevice;
using bloom::render::GpuDeviceCreationOptions;
using bloom::render::readbackResidentImage;
using bloom::runtime::CpuCompositionEvaluator;
using bloom::runtime::CpuGpuSceneBuilder;
using bloom::runtime::GpuSceneCache;
using bloom::runtime::GpuSceneCacheBudgets;
using bloom::runtime::GpuSceneExecutor;
using bloom::runtime::GpuSceneExecutorBudgets;
using bloom::runtime::GpuSceneExecutorDiagnosticCode;
using bloom::runtime::GpuSceneExecutorJobState;
using bloom::runtime::GpuSceneExecutorPollResult;
using bloom::runtime::GpuSceneMediaContext;
using bloom::runtime::PreparedGpuScene;

using bloom::runtime::media_executor_test::drainToTerminal;
using bloom::runtime::media_executor_test::kCacheBudget;
using bloom::runtime::media_executor_test::kMaxPollIterations;
using bloom::runtime::media_executor_test::kReadbackBudget;
using bloom::runtime::media_executor_test::kSceneBudget;
using bloom::runtime::media_executor_test::Options;
using bloom::runtime::media_executor_test::parseOptions;
using bloom::runtime::media_executor_test::pixelsClose;
using bloom::runtime::media_executor_test::pixelsExact;
using bloom::runtime::media_executor_test::runScene;

using PlanPtr = std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>;

constexpr std::size_t kSources = 3;

struct Fixture final {
    std::filesystem::path directory;
    std::vector<bloom::document::AssetRecord> assets;
};

[[nodiscard]] std::vector<ExrPixel> sourcePixels(const std::size_t source, const int width,
                                                 const int height) {
    std::vector<ExrPixel> pixels;
    pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float s = static_cast<float>(source + 1);
            pixels.push_back(ExrPixel{0.125F * s + 0.0625F * static_cast<float>(x),
                                      1.5F - 0.25F * static_cast<float>(y) * s, 0.5F * s - 0.75F,
                                      (x + y + static_cast<int>(source)) % 2 == 0 ? 1.0F : 0.5F});
        }
    }
    return pixels;
}

[[nodiscard]] Fixture makeFixture() {
    Fixture fixture;
    fixture.directory = std::filesystem::temp_directory_path() / "bloom_gpu_upload_lookahead_test";
    std::filesystem::remove_all(fixture.directory);
    std::filesystem::create_directories(fixture.directory);
    for (std::size_t source = 0; source < kSources; ++source) {
        const int width = 3 + static_cast<int>(source);
        const int height = 2 + static_cast<int>(source % 2);
        const auto path = fixture.directory / ("source_" + std::to_string(source) + ".exr");
        writeExrRgba(path, width, height, sourcePixels(source, width, height));
        fixture.assets.push_back(
            imageAsset(path, "source_" + std::to_string(source), 4100 + source));
    }
    return fixture;
}

// kSources ImageSource -> transformed Layer Output pairs -> one Normal Merge -> Composition Output.
[[nodiscard]] PlanPtr threeSourcePlan(const Fixture& fixture, const std::uint64_t idBase) {
    using bloom::document::LayerId;
    using bloom::document::LayerSlotId;
    using bloom::document::NodeId;
    using bloom::document::ParameterId;
    std::vector<CompiledOperation> operations;
    std::vector<bloom::runtime::CompiledMergeInput> inputs;
    for (std::size_t source = 0; source < kSources; ++source) {
        const std::uint64_t base = idBase + 20 * source;
        const LayerIds ids{ParameterId::fromRaw(base + 0), ParameterId::fromRaw(base + 1),
                           ParameterId::fromRaw(base + 2), ParameterId::fromRaw(base + 3),
                           ParameterId::fromRaw(base + 4), ParameterId::fromRaw(base + 5)};
        const auto sourceIndex = OperationIndex::fromRaw(operations.size());
        operations.emplace_back(bloom::runtime::CompiledImageSource{
            NodeId::fromRaw(base + 10), fixture.assets[source], 0, 0, 0, std::string{}, false});
        const LayerValues values{.position = {1.3 + 2.0 * static_cast<double>(source),
                                              1.1 + static_cast<double>(source)},
                                 .rotation = 5.0 * static_cast<double>(source),
                                 .opacity = 1.0 - 0.125 * static_cast<double>(source)};
        const auto layerIndex = OperationIndex::fromRaw(operations.size());
        operations.emplace_back(layerOutput(NodeId::fromRaw(base + 11), LayerId::fromRaw(base + 12),
                                            sourceIndex, ids, values));
        inputs.push_back(bloom::runtime::CompiledMergeInput{
            LayerSlotId::fromRaw(base + 14), LayerId::fromRaw(base + 12), layerIndex});
    }
    const auto mergeIndex = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledMerge{NodeId::fromRaw(idBase + 90), std::move(inputs)});
    const auto outputIndex = OperationIndex::fromRaw(operations.size());
    operations.emplace_back(CompiledCompositionOutput{NodeId::fromRaw(idBase + 91), mergeIndex});
    return publish(CompiledCompositionPlanDefinition{bloom::document::Revision::fromRaw(7),
                                                     kProjectId, kCompositionId, format(12, 8),
                                                     std::move(operations), outputIndex});
}

struct Host final {
    std::unique_ptr<GpuSceneCache> cache;
    std::unique_ptr<GpuSceneExecutor> executor;
};

[[nodiscard]] Host makeHost(GpuDevice& device, const bool lookahead) {
    Host host;
    auto cache = GpuSceneCache::create(device, GpuSceneCacheBudgets{kCacheBudget});
    if (!cache)
        return host;
    host.cache = std::move(cache.cache);
    GpuSceneExecutorBudgets budgets;
    budgets.enableUploadLookahead = lookahead;
    auto executor = GpuSceneExecutor::create(device, *host.cache, budgets);
    if (executor)
        host.executor = std::move(executor.executor);
    return host;
}

[[nodiscard]] std::vector<bloom::render::Rgba32f>
readPixels(const std::shared_ptr<const bloom::render::GpuImage>& image) {
    if (image == nullptr)
        return {};
    auto readback = readbackResidentImage(*image, kReadbackBudget);
    return readback.hasValue() ? std::move(readback.pixels) : std::vector<bloom::render::Rgba32f>{};
}

struct Scene final {
    std::shared_ptr<const PreparedGpuScene> scene;
    std::vector<bloom::render::Rgba32f> cpuPixels;
};

[[nodiscard]] Scene prepare(Expectations& expectations, const CpuCompositionEvaluator& evaluator,
                            const PlanPtr& plan) {
    Scene result;
    const auto context = GpuSceneMediaContext::fromEvaluator(evaluator);
    const CpuGpuSceneBuilder builder(nullptr, context);
    const auto request = requestFor(*plan);
    const auto prepared = builder.build(plan, request);
    expectations.expect(prepared.hasValue(), "the three-source scene prepares");
    if (!prepared)
        return result;
    std::size_t uploads = 0;
    for (const auto& command : prepared.scene->commands())
        uploads += std::holds_alternative<bloom::runtime::GpuSceneUploadCommand>(command) ? 1 : 0;
    expectations.expect(uploads == kSources, "the scene plans one upload per distinct source");
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame = evaluator.evaluate(plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, "the CPU oracle frame evaluates");
    if (frame.frame() == nullptr)
        return result;
    const auto pixels = frame.frame()->processImage().pixels();
    result.cpuPixels.assign(pixels.begin(), pixels.end());
    result.scene = prepared.scene;
    return result;
}

// Ordering and parity: lookahead on versus off on fresh caches.
void testOrderingAndParity(Expectations& expectations, GpuDevice& device, const Scene& scene,
                           std::vector<bloom::render::Rgba32f>& serialPixels,
                           std::uint64_t& serialPeak) {
    auto serial = makeHost(device, false);
    auto overlapped = makeHost(device, true);
    expectations.expect(serial.executor && overlapped.executor, "ordering: both executors build");
    if (!serial.executor || !overlapped.executor)
        return;

    const auto serialRun = runScene(*serial.executor, scene.scene, kSceneBudget);
    expectations.expect(serialRun.ready, "ordering: the serial executor reaches Ready");
    const auto serialCounters = serial.executor->counters();
    expectations.expect(serialCounters.uploads == kSources && serialCounters.uploadsAhead == 0,
                        "ordering: with the lookahead off every upload runs at its own step");
    serialPixels = readPixels(serialRun.image);
    serialPeak = serialCounters.peakLiveImageBytes;

    // The first poll starts the first job and, beside it, the next planned upload: an upload is
    // begun before its own step has been reached.
    auto& executor = *overlapped.executor;
    const auto begun = executor.begin(scene.scene, kSceneBudget);
    expectations.expect(begun.code == GpuSceneExecutorDiagnosticCode::None,
                        "ordering: the overlapped executor accepts the scene");
    if (begun.code != GpuSceneExecutorDiagnosticCode::None)
        return;
    bool sawAhead = false;
    GpuSceneExecutorPollResult result = GpuSceneExecutorPollResult::Pending;
    std::uint32_t lastCompleted = 0;
    bool monotonic = true;
    for (std::uint64_t i = 0; i < kMaxPollIterations; ++i) {
        result = executor.poll();
        const auto counters = executor.counters();
        const auto progress = executor.progress();
        // An upload begun ahead is counted before the cursor reaches its step.
        if (counters.uploadsAhead > 0 && progress.completedSteps < progress.totalSteps)
            sawAhead = true;
        monotonic = monotonic && progress.completedSteps >= lastCompleted;
        lastCompleted = progress.completedSteps;
        if (result != GpuSceneExecutorPollResult::Pending)
            break;
        std::this_thread::yield();
    }
    expectations.expect(result == GpuSceneExecutorPollResult::Ready,
                        "ordering: the overlapped executor reaches Ready");
    expectations.expect(sawAhead, "ordering: an upload was begun ahead of its own step");
    expectations.expect(monotonic, "ordering: steps complete in plan order");
    const auto counters = executor.counters();
    expectations.expect(counters.uploads == kSources && counters.uploadsAhead >= 1 &&
                            counters.uploadsAheadDropped == 0,
                        "ordering: each source uploads once, at least one of them ahead");
    expectations.expect(counters.cacheInsertions == serialCounters.cacheInsertions &&
                            counters.commandsExecuted == serialCounters.commandsExecuted &&
                            counters.producedImages == serialCounters.producedImages,
                        "ordering: the same steps complete, publish and cache as the serial run");
    const auto image = result == GpuSceneExecutorPollResult::Ready ? executor.takeImage() : nullptr;
    const auto pixels = readPixels(image);
    expectations.expect(!pixels.empty() && pixelsExact(pixels, serialPixels),
                        "ordering: the output is bit-identical to the serial executor");
    expectations.expect(pixelsClose(pixels, scene.cpuPixels),
                        "ordering: every pixel is within the 2e-6 gate of the CPU frame");
    expectations.expect(!executor.hasUnretiredSubmission() &&
                            executor.counters().currentLiveImageBytes == 0,
                        "ordering: nothing stays live or unretired after the output is taken");
}

// Cancellation while the lookahead upload is in flight.
void testCancelMidUpload(Expectations& expectations, GpuDevice& device, const Scene& scene,
                         const std::vector<bloom::render::Rgba32f>& serialPixels) {
    auto host = makeHost(device, true);
    expectations.expect(host.executor != nullptr, "cancel: the executor builds");
    if (!host.executor)
        return;
    auto& executor = *host.executor;
    const auto begun = executor.begin(scene.scene, kSceneBudget);
    expectations.expect(begun.code == GpuSceneExecutorDiagnosticCode::None,
                        "cancel: the scene is accepted");
    if (begun.code != GpuSceneExecutorDiagnosticCode::None)
        return;
    const auto first = executor.poll();
    const auto atCancel = executor.counters();
    expectations.expect(first == GpuSceneExecutorPollResult::Pending && atCancel.uploadsAhead == 1,
                        "cancel: the first poll leaves a lookahead upload outstanding");
    executor.cancel();
    const auto terminal = drainToTerminal(executor);
    expectations.expect(terminal == GpuSceneExecutorPollResult::Failure &&
                            executor.diagnostic().code == GpuSceneExecutorDiagnosticCode::Cancelled,
                        "cancel: the job ends Cancelled");
    if (executor.diagnostic().code != GpuSceneExecutorDiagnosticCode::Cancelled)
        std::cerr << "cancel: terminal=" << static_cast<int>(terminal)
                  << " code=" << static_cast<int>(executor.diagnostic().code)
                  << " message=" << executor.diagnostic().message << '\n';
    expectations.expect(executor.image() == nullptr && executor.takeImage() == nullptr,
                        "cancel: no image is published after the cancel");
    // Owner polls alone retire the dropped lookahead (an unproven one keeps the job draining).
    for (std::uint64_t i = 0; i < kMaxPollIterations &&
                              (executor.hasUnretiredSubmission() || executor.ownerDrainRequired());
         ++i) {
        static_cast<void>(executor.poll());
        std::this_thread::yield();
    }
    const auto afterCancel = executor.counters();
    expectations.expect(!executor.hasUnretiredSubmission() && !executor.ownerDrainRequired(),
                        "cancel: every submission, the lookahead included, retires");
    expectations.expect(afterCancel.uploadsAheadDropped == 1 && afterCancel.scenesCancelled == 1 &&
                            afterCancel.currentLiveImageBytes == 0,
                        "cancel: the lookahead is dropped and nothing stays pinned");
    if (afterCancel.uploadsAheadDropped != 1 || afterCancel.scenesCancelled != 1)
        std::cerr << "cancel: dropped=" << afterCancel.uploadsAheadDropped
                  << " cancelled=" << afterCancel.scenesCancelled
                  << " completed=" << afterCancel.scenesCompleted
                  << " live=" << afterCancel.currentLiveImageBytes << '\n';
    expectations.expect(executor.state() != GpuSceneExecutorJobState::Pending,
                        "cancel: the executor is not left pending");

    // Reusable afterwards, with no reservation left behind: the cancelled sources were never
    // cached, so the scene uploads all of them again and matches the serial output.
    const auto again = runScene(executor, scene.scene, kSceneBudget);
    expectations.expect(again.ready, "cancel: the executor runs the scene again");
    const auto pixels = readPixels(again.image);
    expectations.expect(!pixels.empty() && pixelsExact(pixels, serialPixels),
                        "cancel: the rerun is bit-identical to the serial output");
}

// Fallback: one native job at a time when the lookahead cannot (or may not) run beside a job.
void testFallback(Expectations& expectations, GpuDevice& device, const Scene& scene,
                  const std::vector<bloom::render::Rgba32f>& serialPixels,
                  const std::uint64_t serialPeak) {
    // A budget of exactly the serial live peak: the lookahead may begin only where it fits beside
    // the in-flight job, and a later step refused beside it drops it and runs serially. The scene
    // must still complete with the same output and never exceed the serial peak.
    auto tight = makeHost(device, true);
    expectations.expect(tight.executor != nullptr && serialPeak > 0,
                        "fallback: the tight-budget executor builds");
    if (tight.executor && serialPeak > 0) {
        const auto run = runScene(*tight.executor, scene.scene, serialPeak);
        expectations.expect(run.ready, "fallback: the serial live peak still runs the scene");
        const auto counters = tight.executor->counters();
        expectations.expect(counters.uploads == kSources &&
                                counters.peakLiveImageBytes <= serialPeak,
                            "fallback: every source uploads once within the serial peak");
        const auto pixels = readPixels(run.image);
        expectations.expect(!pixels.empty() && pixelsExact(pixels, serialPixels),
                            "fallback: the tight-budget output is bit-identical");
        expectations.expect(!tight.executor->hasUnretiredSubmission(),
                            "fallback: nothing stays unretired");
    }

    // A budget below the serial peak still refuses as before: the lookahead adds no new failure
    // mode and no success the serial executor would not have.
    auto refused = makeHost(device, true);
    if (refused.executor && serialPeak > 1) {
        const auto run = runScene(*refused.executor, scene.scene, serialPeak / 2);
        expectations.expect(!run.ready, "fallback: half the serial peak is still refused");
        for (std::uint64_t i = 0;
             i < kMaxPollIterations &&
             (refused.executor->hasUnretiredSubmission() || refused.executor->ownerDrainRequired());
             ++i) {
            static_cast<void>(refused.executor->poll());
            std::this_thread::yield();
        }
        expectations.expect(!refused.executor->hasUnretiredSubmission() &&
                                !refused.executor->ownerDrainRequired(),
                            "fallback: a refused scene retires every submission");
    }
}

} // namespace

int main(const int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            return 2;
        }
        Expectations expectations;
        CpuCompositionEvaluator evaluator;
        const auto fixture = makeFixture();
        evaluator.setAssetBaseDirectory(fixture.directory);

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
            return 0;
        }
        const auto plan = threeSourcePlan(fixture, 41000);
        const auto scene = prepare(expectations, evaluator, plan);
        if (scene.scene == nullptr) {
            std::cerr << "FAIL: the three-source scene did not prepare\n";
            return 1;
        }
        std::vector<bloom::render::Rgba32f> serialPixels;
        std::uint64_t serialPeak = 0;
        testOrderingAndParity(expectations, *device.device, scene, serialPixels, serialPeak);
        testCancelMidUpload(expectations, *device.device, scene, serialPixels);
        testFallback(expectations, *device.device, scene, serialPixels, serialPeak);

        if (!expectations.ok()) {
            std::cerr << "FAIL: GPU upload lookahead expectations failed\n";
            return 1;
        }
        std::cout << "PASS: GPU upload lookahead\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
