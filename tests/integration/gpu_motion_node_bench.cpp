// #484 motion blur GPU cost benchmark (tools/bench/run_bench.py `nodes`).
//
// Each case is a real document compiled by the production SnapshotCompiler, prepared by the
// production CpuGpuSceneBuilder (the packaged shader tools) and run by the production
// GpuSceneExecutor on the device, at frame 12 of a moving texture (a Linear Gradient on an animated
// Transform 2D). For every case and resolution it reports:
//
// - coldMs: the first run on a fresh scene cache and executor (uploads, programs, every dispatch);
// - warmMs: the same frame again on that executor (the RAM-preview cache hit: no dispatch);
// - ms: the median rerun on one executor whose cache retains nothing (every pass dispatched
//   again, programs and allocations warm), the tier's cases interleaved round robin: the frame's
//   GPU cost, which the ratios use.
//
// The device runs as the preview route does: rooted, reusing the native storage of the frame before
// (#477). --final-route measures it as a final render does today, every pass allocated from the
// driver; budgets hold the preview route, and benchmarks.md records both.
//
// Cases: the cheap reference (a Grade), the closest existing nodes (a Gaussian blur of the same
// size, the Transform 2D being sampled), and every motion-blur node with its heaviest parameters
// stepped: the blur length, the quality (samples, solver iterations), Echo's count and the layered
// matte.
//
//   bloom_runtime_gpu_motion_node_bench [--tiers 1080p,2160p] [--repeats N] [--json FILE]
//                                       [--case NAME ...] [--smoke] [--final-route] [--loader PATH]
//
// Every heavy case also reports where its frame goes: `breakdown` per pass and step kind (count, host
// allocation/record/submit ms, device-and-fence ms, MB written, on an executor with batches off so one
// submission is one step), the CPU time of that frame (`breakdownCpuMs`: the host's work, the poll loop
// included), the native images that frame took back and created (`breakdownStorageReused`,
// `breakdownStorageCreated`), the production run's submissions, batches, images allocated and bytes
// written, the peak live bytes and `prepareMs` (the scene's preparation on the host, which the frame's
// `ms` leaves out; `coldPrepareMs` is the first such build, compiling programs).
//
// Without a device or the packaged shader tools it exits 77 (SKIP). --smoke runs every case once
// at 64x64 so the build keeps it working.

#include "gpu_coverage_ocio_support.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/animation_operations.hpp>
#include <bloom/document/depth_effect_parameters.hpp>
#include <bloom/document/motion_blur_parameters.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace bloom;

// A solver holds several atlases of pass buffers at once: 8 GiB at 2160p.
constexpr std::uint64_t kSceneBudget = 1ULL << 33U;

struct Doc {
    document::NewProject initial;
    document::Document document;
    commands::CommandStack commands;
    document::CompositionId composition;
    std::uint32_t width;
    std::uint32_t height;

    Doc(const std::uint32_t w, const std::uint32_t h)
        : initial(document::makeNewProject(
              "Bench", "Main", core::RationalTime::fromInteger(2),
              *document::CompositionFormat::create(w, h, core::PixelAspectRatio::square(),
                                                   *document::FrameRate::create(24, 1)))),
          document(std::move(initial.project)), commands(document),
          composition(initial.initialCompositionId), width(w), height(h) {}

    template <typename Operation, typename... Arguments>
    commands::CommandResult run(Arguments&&... arguments) {
        commands::Transaction transaction("Bench", document.snapshot().revision());
        transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded()) {
            std::string message = "bench command refused";
            for (const auto& failure : result.operationFailures)
                message += ": " + failure.issue.message;
            throw std::runtime_error(message);
        }
        return result;
    }
    const document::CanonicalGraph& graph() const {
        return document.snapshot().project().findComposition(composition)->graph();
    }
    document::ParameterId parameterOf(const document::NodeId node, const std::string_view role) {
        for (const auto& binding : graph().findNode(node)->parameters)
            if (binding.role == role)
                return binding.parameterId;
        throw std::runtime_error("missing parameter " + std::string(role));
    }
    void set(const document::NodeId node, const std::string_view role,
             document::ParameterValue value) {
        run<commands::SetParameterSource>(composition, parameterOf(node, role),
                                          document::ConstantValueSource{std::move(value)});
    }
    document::NodeId node(const std::string_view type) {
        return *run<commands::AddNode>(composition, std::string(type), document::Vec2d{})
                    .outputId<document::NodeId>(commands::kAddNodeOutput);
    }
    void connect(const document::NodeId from, const std::string& port, const document::NodeId to,
                 const std::string& input) {
        run<commands::ConnectPorts>(composition, document::OutputPortRef{from, port},
                                    document::NodeInputRef{to, input});
    }
    struct Layer {
        document::LayerId id;
        document::NodeId layerNode;
        document::NodeId solid;
    };
    // A frame-sized solid layer.
    Layer solid(const core::Color4d color) {
        const auto result = run<commands::AddSolidLayer>(composition, "Solid", color);
        const auto solidNode =
            *result.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput);
        set(solidNode, "width", static_cast<double>(width));
        set(solidNode, "height", static_cast<double>(height));
        const auto id = *result.outputId<document::LayerId>(commands::kAddSolidLayerLayerOutput);
        return {id, graph().findLayer(id)->nodeId, solidNode};
    }
    // A Grade between `layer`'s solid and its Layer node.
    document::NodeId grade(const Layer& layer) {
        const auto id = node("bloom.grade");
        set(id, "exposure", 0.5);
        connect(layer.solid, "image", id, "input");
        connect(id, "image", layer.layerNode, "image");
        return id;
    }
    // A frame-sized Linear Gradient, from `from` to `to` across the frame.
    document::NodeId gradient() {
        const auto id = node(document::kLinearGradientNodeType);
        set(id, "width", static_cast<double>(width));
        set(id, "height", static_cast<double>(height));
        set(id, "start", document::Vec2d{0.0, 0.0});
        set(id, "end", document::Vec2d{static_cast<double>(width), static_cast<double>(height)});
        return id;
    }
    // The gradient on a Transform 2D moving 8 pixels a frame through frame 12 at no offset.
    document::NodeId movingTransform() {
        const auto id = node("bloom.transform-2d");
        connect(gradient(), "image", id, "input");
        const auto keyed = [&](const std::int64_t frame, const double x) {
            const auto time = core::RationalTime::create(frame, 24);
            run<commands::SetKeyframeAtTimeForParameter>(composition, parameterOf(id, "translate"),
                                                         *time, document::Vec2d{x, 0.0});
        };
        keyed(0, -96.0);
        keyed(48, 288.0);
        return id;
    }
    // `effect` over the moving texture, shown by a frame-sized layer.
    document::NodeId overMoving(const Layer& layer, const std::string_view effect) {
        const auto id = node(effect);
        connect(movingTransform(), "image", id, "input");
        connect(id, "image", layer.layerNode, "image");
        return id;
    }
    // The vectors: a frame-sized solid moving everything right and a little down (forward RG,
    // backward BA).
    document::NodeId vectors(const double pixels) {
        const auto id = node(document::kSolidSourceNodeType);
        set(id, "color", core::Color4d{pixels, 0.25 * pixels, 0.0, 1.0});
        set(id, "width", static_cast<double>(width));
        set(id, "height", static_cast<double>(height));
        return id;
    }
    // A coverage matte over the middle of the frame.
    document::NodeId matte() {
        const auto id = node(document::kSolidSourceNodeType);
        set(id, "color", core::Color4d{1.0, 1.0, 1.0, 1.0});
        set(id, "width", 0.5 * static_cast<double>(width));
        set(id, "height", static_cast<double>(height));
        return id;
    }
};

struct Case final {
    std::string name;
    std::string node;       // the node or route measured
    std::string parameter;  // the stepped heaviest parameter, or empty
    std::int64_t value = 0; // its value
    std::string reference;  // the cheap reference case at the same resolution
    std::string comparable; // the closest existing case at the same resolution
    std::function<void(Doc&)> build;
};

std::vector<Case> cases() {
    std::vector<Case> list;
    const auto add = [&](std::string name, std::string node, std::string comparable,
                         std::function<void(Doc&)> build, std::string parameter = {},
                         std::int64_t value = 0) {
        list.push_back(Case{std::move(name), std::move(node), std::move(parameter), value, "grade",
                            std::move(comparable), std::move(build)});
    };
    const core::Color4d beauty{0.8, 0.6, 0.4, 1.0};
    const auto draft = document::kMotionQualityDraft;
    const auto standard = document::kMotionQualityStandard;
    const auto high = document::kMotionQualityHigh;
    const auto final = document::kMotionQualityFinal;
    const auto qualityName = [](const std::int64_t quality) -> std::string {
        return quality == document::kMotionQualityDraft      ? "draft"
               : quality == document::kMotionQualityStandard ? "standard"
               : quality == document::kMotionQualityHigh     ? "high"
                                                             : "final";
    };
    // References and the closest existing nodes.
    add("base", "solid layer", "", [beauty](Doc& doc) { doc.solid(beauty); });
    add("grade", "bloom.grade", "", [beauty](Doc& doc) { doc.grade(doc.solid(beauty)); });
    for (const std::int64_t radius : {8, 32, 128})
        add("gaussian-blur-" + std::to_string(radius), "bloom.gaussian-blur", "",
            [beauty, radius](Doc& doc) {
                const auto layer = doc.solid(beauty);
                const auto blur = doc.overMoving(layer, "bloom.gaussian-blur");
                doc.set(blur, "radius", static_cast<double>(radius));
            },
            "radius", radius);
    add("transform-2d", "bloom.transform-2d", "", [beauty](Doc& doc) {
        const auto layer = doc.solid(beauty);
        doc.connect(doc.movingTransform(), "image", layer.layerNode, "image");
    });
    // Directional, Spin and Zoom: the blur's reach stepped at Final (adaptive samples), held to a
    // Gaussian blur of the same reach.
    for (const std::int64_t length : {8, 32, 128})
        add("directional-" + std::to_string(length), "bloom.directional-blur",
            "gaussian-blur-" + std::to_string(length),
            [beauty, length](Doc& doc) {
                const auto node = doc.overMoving(doc.solid(beauty), document::kDirectionalBlurNodeType);
                doc.set(node, "length", static_cast<double>(length));
                doc.set(node, "angle", 30.0);
            },
            "length", length);
    for (const auto quality : {draft, standard, high})
        add("directional-32-" + qualityName(quality), "bloom.directional-blur", "gaussian-blur-32",
            [beauty, quality](Doc& doc) {
                const auto node = doc.overMoving(doc.solid(beauty), document::kDirectionalBlurNodeType);
                doc.set(node, "length", 32.0);
                doc.set(node, "quality", quality);
            },
            "quality", quality);
    for (const std::int64_t degrees : {10, 45})
        add("spin-" + std::to_string(degrees), "bloom.spin-blur", "gaussian-blur-32",
            [beauty, degrees](Doc& doc) {
                const auto node = doc.overMoving(doc.solid(beauty), document::kSpinBlurNodeType);
                doc.set(node, "amount", static_cast<double>(degrees));
                doc.set(node, "quality", document::kMotionQualityHigh);
            },
            "amount", degrees);
    for (const std::int64_t percent : {10, 50})
        add("zoom-" + std::to_string(percent), "bloom.zoom-blur", "gaussian-blur-32",
            [beauty, percent](Doc& doc) {
                const auto node = doc.overMoving(doc.solid(beauty), document::kZoomBlurNodeType);
                doc.set(node, "amount", static_cast<double>(percent) / 100.0);
                doc.set(node, "quality", document::kMotionQualityHigh);
            },
            "amount", percent);
    // Transform 2D's shutter: its one resample at each sample, held to the Transform 2D.
    for (const auto quality : {draft, standard, high, final})
        add("transform-blur-" + qualityName(quality), "bloom.transform-2d (motion blur)",
            "transform-2d",
            [beauty, quality](Doc& doc) {
                const auto layer = doc.solid(beauty);
                const auto moving = doc.movingTransform();
                doc.set(moving, "motionBlur", true);
                doc.set(moving, "quality", quality);
                doc.connect(moving, "image", layer.layerNode, "image");
            },
            "quality", quality);
    // Motion Blur: the upstream at each sub-frame, held to the shutter of Transform 2D at the same
    // sample count (the same picture for this graph).
    for (const auto quality : {draft, standard, high, final})
        add("motion-blur-" + qualityName(quality), "bloom.motion-blur",
            "transform-blur-" + qualityName(quality),
            [beauty, quality](Doc& doc) {
                const auto node = doc.overMoving(doc.solid(beauty), document::kMotionBlurNodeType);
                doc.set(node, "quality", quality);
            },
            "quality", quality);
    // Echo: the count stepped; Frame Blend and Posterize Time are its other modes.
    for (const std::int64_t echoes : {4, 16})
        add("echo-" + std::to_string(echoes), "bloom.echo", "transform-2d",
            [beauty, echoes](Doc& doc) {
                const auto node = doc.overMoving(doc.solid(beauty), document::kEchoNodeType);
                doc.set(node, "echoes", echoes);
            },
            "echoes", echoes);
    add("frame-blend-4", "bloom.echo (Frame Blend, Posterize Time)", "transform-2d",
        [beauty](Doc& doc) {
            const auto node = doc.overMoving(doc.solid(beauty), document::kEchoNodeType);
            doc.set(node, "mode", document::kEchoModeFrameBlend);
            doc.set(node, "posterize", true);
        },
        "echoes", 4);
    // Vector Motion Blur: the widest vector stepped for the gather and the splat, held to a
    // Directional Blur of the same length (one direction, no reconstruction).
    for (const std::int64_t pixels : {8, 32, 128})
        add("vector-gather-" + std::to_string(pixels), "bloom.vector-motion-blur",
            "directional-" + std::to_string(pixels),
            [beauty, pixels](Doc& doc) {
                const auto node = doc.overMoving(doc.solid(beauty), document::kVectorMotionBlurNodeType);
                doc.connect(doc.vectors(static_cast<double>(pixels)), "image", node, "vectors");
                doc.set(node, "algorithm", document::kVectorAlgorithmGather);
                doc.set(node, "quality", document::kMotionQualityStandard);
            },
            "vectorPixels", pixels);
    for (const std::int64_t limit : {32, 128, 512})
        add("vector-gather-limit-" + std::to_string(limit),
            "bloom.vector-motion-blur (maximum blur)", "directional-32",
            [beauty, limit](Doc& doc) {
                const auto node = doc.overMoving(doc.solid(beauty), document::kVectorMotionBlurNodeType);
                doc.connect(doc.vectors(32.0), "image", node, "vectors");
                doc.set(node, "algorithm", document::kVectorAlgorithmGather);
                doc.set(node, "quality", document::kMotionQualityStandard);
                doc.set(node, "maxBlur", static_cast<double>(limit));
            },
            "maxBlur", limit);
    for (const std::int64_t pixels : {8, 32, 128})
        add("vector-splat-" + std::to_string(pixels), "bloom.vector-motion-blur (layered splat)",
            "vector-gather-" + std::to_string(pixels),
            [beauty, pixels](Doc& doc) {
                const auto node = doc.overMoving(doc.solid(beauty), document::kVectorMotionBlurNodeType);
                doc.connect(doc.vectors(static_cast<double>(pixels)), "image", node, "vectors");
                doc.set(node, "algorithm", document::kVectorAlgorithmLayered);
                doc.set(node, "quality", document::kMotionQualityHigh);
            },
            "vectorPixels", pixels);
    add("vector-layered-32", "bloom.vector-motion-blur (matte layers)", "vector-splat-32",
        [beauty](Doc& doc) {
            const auto node = doc.overMoving(doc.solid(beauty), document::kVectorMotionBlurNodeType);
            doc.connect(doc.vectors(32.0), "image", node, "vectors");
            doc.connect(doc.matte(), "image", node, "matte");
            doc.set(node, "algorithm", document::kVectorAlgorithmLayered);
            doc.set(node, "quality", document::kMotionQualityHigh);
        },
        "vectorPixels", 32);
    // Optical Flow: each method at each quality's fixed pyramid and iterations; nothing existing
    // solves anything, so a Gaussian blur of the widest reach is the yardstick.
    for (const auto quality : {draft, standard, final})
        add("optical-flow-dis-" + qualityName(quality), "bloom.optical-flow (DIS)",
            "gaussian-blur-128",
            [beauty, quality](Doc& doc) {
                const auto layer = doc.solid(beauty);
                const auto node = doc.node(document::kOpticalFlowNodeType);
                doc.connect(doc.movingTransform(), "image", node, "input");
                doc.set(node, "method", document::kFlowMethodDis);
                doc.set(node, "quality", quality);
                doc.connect(node, std::string(document::kFlowForwardPort), layer.layerNode, "image");
            },
            "quality", quality);
    for (const auto quality : {draft, standard, high, final})
        add("optical-flow-tvl1-" + qualityName(quality), "bloom.optical-flow (TV-L1)",
            "gaussian-blur-128",
            [beauty, quality](Doc& doc) {
                const auto layer = doc.solid(beauty);
                const auto node = doc.node(document::kOpticalFlowNodeType);
                doc.connect(doc.movingTransform(), "image", node, "input");
                doc.set(node, "method", document::kFlowMethodTvl1);
                doc.set(node, "quality", quality);
                doc.connect(node, std::string(document::kFlowForwardPort), layer.layerNode, "image");
            },
            "quality", quality);
    // Motion Blur (Optical Flow): four solves, their consistency and the splat, held to the splat
    // of known vectors; a matte solves both layers.
    for (const auto quality : {draft, standard, high})
        add("flow-blur-" + qualityName(quality), "bloom.optical-flow-motion-blur",
            "vector-splat-32",
            [beauty, quality](Doc& doc) {
                const auto node =
                    doc.overMoving(doc.solid(beauty), document::kOpticalFlowMotionBlurNodeType);
                doc.set(node, "quality", quality);
            },
            "quality", quality);
    add("flow-blur-matte-standard", "bloom.optical-flow-motion-blur (matte)", "vector-layered-32",
        [beauty, standard](Doc& doc) {
            const auto node =
                doc.overMoving(doc.solid(beauty), document::kOpticalFlowMotionBlurNodeType);
            doc.connect(doc.matte(), "image", node, "matte");
            doc.set(node, "quality", standard);
        },
        "quality", standard);
    return list;
}

// Where a heavy frame goes, one row per pass and step kind.
struct BreakdownRow final {
    std::string step;
    std::uint64_t count = 0;
    double submitMs = 0;
    double fenceMs = 0;
    double megabytes = 0;
};

[[nodiscard]] bool heavyCase(const std::string_view name) {
    return name.starts_with("optical-flow") || name.starts_with("flow-blur") ||
           name.starts_with("vector-") || name.starts_with("motion-blur");
}

struct Timing final {
    bool ok = false;
    std::string error;
    double coldMs = 0;
    double warmMs = 0;
    double ms = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t warmDispatches = 0;
    double coldPrepareMs = 0;
    double prepareMs = 0;
    double submitMs = 0;
    double fenceMs = 0;
    std::uint64_t submissions = 0;
    std::uint64_t batches = 0;
    std::uint64_t producedImages = 0;
    double producedMegabytes = 0;
    double peakLiveMegabytes = 0;
    double breakdownCpuMs = 0;
    double breakdownMs = 0;
    std::uint64_t breakdownStorageReused = 0;
    std::uint64_t breakdownStorageCreated = 0;
    std::vector<BreakdownRow> breakdown;
};

constexpr std::array<const char*, 27> kMotionPassNames{
    "",          "accumulate", "path",      "tile",        "gather",      "splat",    "jfa-init",
    "jfa-step",  "fill",       "consistency", "grey",      "decimate",    "tv-init",  "tv-warp",
    "tv-iterate", "flow-out",  "dis-search", "dis-densify", "dis-data",   "dis-iterate", "cut-residual",
    "cut-reduce", "cut-broadcast", "temporal-fill", "dis-init", "dis-update", "neighbour"};

[[nodiscard]] std::string stepName(const runtime::GpuSceneCommand& command) {
    if (const auto* effect = std::get_if<runtime::GpuSceneOcioEffectCommand>(&command))
        return effect->diagnosticPass != 0 && effect->diagnosticPass < kMotionPassNames.size()
                   ? std::string(kMotionPassNames[effect->diagnosticPass])
                   : std::string("effect");
    if (const auto* move = std::get_if<runtime::GpuSceneTranslationCommand>(&command))
        return move->transientResult ? "atlas-move" : "output-cut";
    if (const auto* blend = std::get_if<runtime::GpuSceneBlendCommand>(&command))
        return blend->transientResult ? "atlas-blend" : "blend";
    return "other";
}

double millisecondsSince(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

// One begin -> Ready on `executor`, timed; false with `error` set when it does not complete.
// The device whose parked storage a frame keeps within kParkedDeviceBytes; null on the final route.
// A solver's many atlas shapes each keep as many images as a batch held at once, so parked storage
// reaches far past the frame's live bytes. Production samples the driver's free memory and frees
// parked storage when work waits for room; nothing here refuses room, so without this limit an
// image lands in host memory, and it and everything after it measure that instead.
render::GpuDevice* parkedDevice = nullptr;
constexpr std::uint64_t kParkedDeviceBytes = 12ULL << 30;

void limitParkedStorage() {
    if (parkedDevice == nullptr)
        return;
    const auto parked = parkedDevice->nativeStorageReuseStats().parkedDeviceBytes;
    if (parked > kParkedDeviceBytes)
        parkedDevice->releaseReusableNativeStorage(parked - kParkedDeviceBytes, true);
}

bool runOnce(runtime::GpuSceneExecutor& executor,
             const std::shared_ptr<const runtime::PreparedGpuScene>& scene, double& ms,
             std::string& error) {
    const auto start = std::chrono::steady_clock::now();
    if (const auto begin = executor.begin(scene, kSceneBudget);
        begin.code != runtime::GpuSceneExecutorDiagnosticCode::None) {
        error = "executor begin refused: " + begin.message;
        return false;
    }
    const auto deadline = start + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto poll = executor.poll();
        limitParkedStorage();
        if (poll == runtime::GpuSceneExecutorPollResult::Ready) {
            ms = millisecondsSince(start);
            (void)executor.takeImage();
            return true;
        }
        if (poll == runtime::GpuSceneExecutorPollResult::Failure ||
            poll == runtime::GpuSceneExecutorPollResult::WrongThread) {
            error = "executor failed: " + executor.diagnostic().message;
            return false;
        }
        std::this_thread::yield();
    }
    error = "executor did not finish within 60 s";
    executor.cancel();
    return false;
}

// An executor over a scene cache with the given budgets (the cache outlives the executor).
struct Runner final {
    std::unique_ptr<runtime::GpuSceneCache> cache;
    std::unique_ptr<runtime::GpuSceneExecutor> executor;
};
bool makeRunner(render::GpuDevice& device, const runtime::GpuSceneCacheBudgets budgets,
                Runner& runner, std::string& error) {
    auto cache = runtime::GpuSceneCache::create(device, budgets);
    if (!cache) {
        error = "scene cache could not be created";
        return false;
    }
    runner.cache = std::move(cache.cache);
    // A solver's pass atlases exceed the default per-image ceiling; production sets this ceiling
    // to the request budget too.
    runtime::GpuSceneExecutorBudgets executorBudgets;
    executorBudgets.maxImageBytes = kSceneBudget;
    auto executor = runtime::GpuSceneExecutor::create(device, *runner.cache, executorBudgets);
    if (!executor) {
        error = "executor could not be created: " + executor.diagnostic.message;
        return false;
    }
    runner.executor = std::move(executor.executor);
    return true;
}

// Cold, then the same frame again: the output cache hit RAM preview serves.
void measureCold(render::GpuDevice& device,
                 const std::shared_ptr<const runtime::PreparedGpuScene>& scene, Timing& timing) {
    Runner runner;
    if (!makeRunner(device, {}, runner, timing.error))
        return;
    // A cold frame owes its own allocations; the case before it parked other storage.
    device.releaseReusableNativeStorage(std::numeric_limits<std::uint64_t>::max(), true);
    const auto before = runner.executor->counters();
    if (!runOnce(*runner.executor, scene, timing.coldMs, timing.error))
        return;
    const auto after = runner.executor->counters();
    timing.dispatches = after.dispatches - before.dispatches;
    timing.submitMs = static_cast<double>(after.processingSubmitNanoseconds -
                                          before.processingSubmitNanoseconds) / 1.0e6;
    timing.fenceMs = static_cast<double>(after.processingFenceObservationNanoseconds -
                                         before.processingFenceObservationNanoseconds) / 1.0e6;
    timing.submissions = after.nativeSubmissions - before.nativeSubmissions;
    timing.batches = after.nativeBatches - before.nativeBatches;
    timing.producedImages = after.producedImages - before.producedImages;
    timing.producedMegabytes = static_cast<double>(after.cumulativeProducedImageBytes -
                                                   before.cumulativeProducedImageBytes) / 1.0e6;
    timing.peakLiveMegabytes = static_cast<double>(after.peakLiveImageBytes) / 1.0e6;
    const auto warmBefore = runner.executor->counters();
    if (!runOnce(*runner.executor, scene, timing.warmMs, timing.error))
        return;
    timing.warmDispatches = runner.executor->counters().dispatches - warmBefore.dispatches;
    timing.ok = true;
}

// One frame with batches off and programs warm: every step its own submission and fence.
void measureBreakdown(render::GpuDevice& device,
                      const std::shared_ptr<const runtime::PreparedGpuScene>& scene,
                      Timing& timing) {
    auto cache = runtime::GpuSceneCache::create(device, {.maxRetainedBytes = 0});
    if (!cache)
        return;
    runtime::GpuSceneExecutorBudgets budgets;
    budgets.maxImageBytes = kSceneBudget;
    budgets.enableNativeBatches = false;
    budgets.collectStepTimings = true;
    auto executor = runtime::GpuSceneExecutor::create(device, *cache.cache, budgets);
    if (!executor)
        return;
    std::string error;
    double ms = 0;
    if (!runOnce(*executor.executor, scene, ms, error))
        return;
    const auto storageBefore = device.nativeStorageReuseStats();
    const auto cpuBefore = std::clock();
    if (!runOnce(*executor.executor, scene, timing.breakdownMs, error))
        return;
    timing.breakdownCpuMs = 1000.0 * static_cast<double>(std::clock() - cpuBefore) / CLOCKS_PER_SEC;
    const auto storageAfter = device.nativeStorageReuseStats();
    timing.breakdownStorageReused = storageAfter.reused - storageBefore.reused;
    timing.breakdownStorageCreated = storageAfter.created - storageBefore.created;
    std::map<std::string, BreakdownRow> rows;
    const auto& commands = scene->commands();
    for (const auto& step : executor.executor->stepTimings()) {
        const auto name = step.command < commands.size()
                              ? stepName(commands[step.command])
                              : std::string("other");
        auto& row = rows[name];
        row.step = name;
        ++row.count;
        row.submitMs += static_cast<double>(step.submitNanoseconds) / 1.0e6;
        row.fenceMs += static_cast<double>(step.fenceObservationNanoseconds) / 1.0e6;
        row.megabytes += static_cast<double>(step.outputBytes) / 1.0e6;
    }
    for (auto& [name, row] : rows)
        timing.breakdown.push_back(row);
    std::ranges::sort(timing.breakdown, std::greater{}, [](const BreakdownRow& row) {
        return row.submitMs + row.fenceMs;
    });
}

// The frame's cost: one executor over a cache that retains nothing, so every run dispatches every
// pass again. The tier's cases rerun on it round robin, so a change of device clock during the run
// moves every case alike, and each case keeps the median.

struct Options final {
    std::vector<std::string> tiers{"1080p", "2160p"};
    std::vector<std::string> only;
    int repeats = 5;
    std::filesystem::path json;
    std::filesystem::path loader;
    bool smoke = false;
    bool finalRoute = false;
    bool valid = true;
};

Options parse(const int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                options.valid = false;
                return {};
            }
            return argv[++i];
        };
        if (argument == "--media-root")
            static_cast<void>(next()); // run_bench.py passes it to every bench; no media is read
        else if (argument == "--case")
            options.only.emplace_back(next());
        else if (argument == "--repeats")
            options.repeats = std::max(1, std::stoi(next()));
        else if (argument == "--json")
            options.json = next();
        else if (argument == "--loader")
            options.loader = next();
        else if (argument == "--smoke")
            options.smoke = true;
        else if (argument == "--final-route")
            options.finalRoute = true;
        else if (argument == "--tiers") {
            options.tiers.clear();
            std::stringstream list(next());
            for (std::string tier; std::getline(list, tier, ',');)
                if (!tier.empty())
                    options.tiers.push_back(tier);
        } else
            options.valid = false;
    }
    return options;
}

std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

} // namespace

int main(const int argc, char** argv) try {
    const auto options = parse(argc, argv);
    if (!options.valid) {
        std::cerr << "usage: bloom_runtime_gpu_motion_node_bench [--tiers 1080p,2160p] [--repeats N] "
                     "[--json FILE] [--smoke] [--final-route] [--loader PATH]\n";
        return 2;
    }
    const auto ocio = gpu_coverage_ocio::sharedContext();
    if (ocio == nullptr || ocio->preparer == nullptr) {
        std::cout << "SKIP: the packaged shader tools are unavailable: "
                  << gpu_coverage_ocio::unavailableReason() << '\n';
        return 77;
    }
    render::GpuDeviceCreationOptions deviceOptions;
    deviceOptions.loader_path = options.loader;
    auto device = render::GpuDevice::create(deviceOptions);
    if (!device) {
        std::cout << "SKIP: no compatible Vulkan device: " << device.diagnostic.message << '\n';
        return 77;
    }
    struct Tier {
        std::string name;
        std::uint32_t width;
        std::uint32_t height;
    };
    std::vector<Tier> tiers;
    if (options.smoke) {
        tiers.push_back({"smoke", 64, 64});
    } else {
        for (const auto& name : options.tiers) {
            if (name == "1080p")
                tiers.push_back({name, 1920, 1080});
            else if (name == "2160p")
                tiers.push_back({name, 3840, 2160});
            else if (name == "720p")
                tiers.push_back({name, 1280, 720});
            else
                throw std::runtime_error("unknown tier " + name);
        }
    }
    // As the preview route runs (#477): a processing root and a phase charge every native image, so
    // a transient image is parked on release and taken back by the next allocation of its shape,
    // and retention gives way when the device refuses room. --final-route leaves the device as a
    // final render does today, without reuse, so every pass allocates from the driver.
    render::GpuProcessingPhaseBudget phaseBudget;
    std::shared_ptr<render::GpuProcessingPhaseLease> phaseLease;
    if (!options.finalRoute) {
        phaseBudget.setHostCapacity(1ULL << 36, true);
        phaseBudget.setDeviceCapacity(1ULL << 36, true);
        if (!device.device->bindProcessingRoot(phaseBudget.rootIdentity()))
            throw std::runtime_error("the bench device would not bind its processing root");
        auto grant = phaseBudget.reserve({.fixedHostBytes = 1,
                                          .requestedExecutorBytes = kSceneBudget,
                                          .minimumExecutorBytes = kSceneBudget,
                                          .sourceFree = true});
        phaseLease = std::move(grant.lease);
        if (!phaseLease || !device.device->retainProcessingPhase(phaseLease))
            throw std::runtime_error("the bench phase was not admitted");
        parkedDevice = device.device.get();
        device.device->setNativeStorageReuse(true);
        device.device->setDeviceRoomReclaimer(
            [&native = *device.device](const std::uint64_t bytes) -> std::uint64_t {
                return native.releaseReusableNativeStorage(bytes, true);
            });
    }
    std::ostringstream json;
    json << "{\"tool\": \"gpu-motion-node-bench\", \"repeats\": " << options.repeats
         << ", \"cases\": [";
    bool first = true;
    int failures = 0;
    for (const auto& tier : tiers) {
        struct Entry final {
            Case item;
            std::shared_ptr<const runtime::PreparedGpuScene> scene;
            std::string commandCount;
            Timing timing;
            std::vector<double> samples;
        };
        std::vector<Entry> entries;
        for (auto& item : cases()) {
            if (!options.only.empty() && std::ranges::find(options.only, item.name) ==
                                             options.only.end() &&
                item.name != "grade")
                continue;
            Entry entry{std::move(item), {}, {}, {}, {}};
            Doc doc(tier.width, tier.height);
            try {
                entry.item.build(doc);
                runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
                const auto compiled =
                    compiler.compile({doc.document.snapshot(), doc.composition, {}}, {});
                if (!compiled.plan) {
                    entry.timing.error = "compile failed";
                    for (const auto& diagnostic : compiled.diagnostics)
                        entry.timing.error += ": " + diagnostic.summary;
                } else {
                    runtime::CpuCompositionEvaluator evaluator;
                    auto context = runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
                    const runtime::CpuGpuSceneBuilder builder(nullptr, context, *ocio);
                    const runtime::EvaluationRequest request{
                        .time = *core::RationalTime::create(12, 24),
                        .output = compiled.plan->output(),
                        .resolution = runtime::CompositionFormatResolution{},
                        .quality = runtime::EvaluationQuality::Reference,
                        .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                        .pixelStorageByteLimit = kSceneBudget,
                    };
                    // The first build compiles what this process has not compiled yet; the second
                    // is what every later frame costs on the host.
                    const auto coldPrepareStart = std::chrono::steady_clock::now();
                    static_cast<void>(builder.build(compiled.plan, request));
                    entry.timing.coldPrepareMs = millisecondsSince(coldPrepareStart);
                    const auto prepareStart = std::chrono::steady_clock::now();
                    const auto prepared = builder.build(compiled.plan, request);
                    entry.timing.prepareMs = millisecondsSince(prepareStart);
                    if (!prepared) {
                        entry.timing.error = "prepare failed: " + prepared.diagnostic.message;
                    } else {
                        entry.scene = prepared.scene;
                        entry.commandCount = std::to_string(prepared.scene->commands().size());
                        measureCold(*device.device, entry.scene, entry.timing);
                        if (entry.timing.ok && heavyCase(entry.item.name))
                            measureBreakdown(*device.device, entry.scene, entry.timing);
                    }
                }
            } catch (const std::exception& error) {
                entry.timing.ok = false;
                entry.timing.error = error.what();
            }
            entries.push_back(std::move(entry));
        }
        Runner steady;
        std::string steadyError;
        if (!makeRunner(*device.device, runtime::GpuSceneCacheBudgets{.maxRetainedBytes = 0},
                        steady, steadyError))
            throw std::runtime_error(steadyError);
        const int rounds = (options.smoke ? 1 : options.repeats) + 1;
        for (int round = 0; round < rounds; ++round) {
            for (auto& entry : entries) {
                if (!entry.timing.ok)
                    continue;
                auto& executor = *steady.executor;
                double ms = 0;
                // A case runs on its own parked storage, as a preview of it does from its second
                // frame, not on what the case before it left.
                if (phaseLease) {
                    device.device->releaseReusableNativeStorage(
                        std::numeric_limits<std::uint64_t>::max(), true);
                    if (!runOnce(executor, entry.scene, ms, entry.timing.error)) {
                        entry.timing.ok = false;
                        continue;
                    }
                }
                const auto before = executor.counters();
                if (!runOnce(executor, entry.scene, ms, entry.timing.error)) {
                    entry.timing.ok = false;
                } else if (executor.counters().dispatches - before.dispatches !=
                           entry.timing.dispatches) {
                    entry.timing.ok = false;
                    entry.timing.error = "a rerun did not dispatch every pass again";
                } else if (round > 0) {
                    entry.samples.push_back(ms); // the first round warms the executor
                }
            }
        }
        for (auto& entry : entries) {
            auto& timing = entry.timing;
            auto& samples = entry.samples;
            if (timing.ok && !samples.empty()) {
                std::ranges::sort(samples);
                timing.ms = samples[samples.size() / 2];
            } else {
                timing.ok = false;
            }
            const auto& item = entry.item;
            if (!timing.ok)
                ++failures;
            std::cout << (timing.ok ? "OK   " : "FAIL ") << tier.name << ' ' << item.name;
            if (timing.ok)
                std::cout << ": cold " << timing.coldMs << " ms, warm " << timing.warmMs
                          << " ms (" << timing.warmDispatches << " dispatches), " << timing.ms
                          << " ms/frame, " << timing.dispatches << " dispatches, "
                          << entry.commandCount << " commands";
            else
                std::cout << ": " << timing.error;
            std::cout << '\n';
            json << (first ? "\n" : ",\n") << "  {\"case\": " << quoted(item.name)
                 << ", \"node\": " << quoted(item.node) << ", \"tier\": " << quoted(tier.name)
                 << ", \"width\": " << tier.width << ", \"height\": " << tier.height
                 << ", \"parameter\": " << quoted(item.parameter) << ", \"value\": " << item.value
                 << ", \"reference\": " << quoted(item.reference)
                 << ", \"comparable\": " << quoted(item.comparable)
                 << ", \"ok\": " << (timing.ok ? "true" : "false")
                 << ", \"error\": " << quoted(timing.error) << ", \"coldMs\": " << timing.coldMs
                 << ", \"warmMs\": " << timing.warmMs << ", \"ms\": " << timing.ms
                 << ", \"dispatches\": " << timing.dispatches
                 << ", \"warmDispatches\": " << timing.warmDispatches
                 << ", \"coldPrepareMs\": " << timing.coldPrepareMs
                 << ", \"prepareMs\": " << timing.prepareMs
                 << ", \"coldSubmitMs\": " << timing.submitMs
                 << ", \"coldFenceMs\": " << timing.fenceMs
                 << ", \"submissions\": " << timing.submissions
                 << ", \"batches\": " << timing.batches
                 << ", \"producedImages\": " << timing.producedImages
                 << ", \"producedMegabytes\": " << timing.producedMegabytes
                 << ", \"peakLiveMegabytes\": " << timing.peakLiveMegabytes
                 << ", \"breakdownMs\": " << timing.breakdownMs
                 << ", \"breakdownCpuMs\": " << timing.breakdownCpuMs
                 << ", \"breakdownStorageReused\": " << timing.breakdownStorageReused
                 << ", \"breakdownStorageCreated\": " << timing.breakdownStorageCreated
                 << ", \"breakdown\": [";
            bool firstRow = true;
            for (const auto& row : timing.breakdown) {
                json << (firstRow ? "" : ", ") << "{\"step\": " << quoted(row.step)
                     << ", \"count\": " << row.count << ", \"submitMs\": " << row.submitMs
                     << ", \"fenceMs\": " << row.fenceMs << ", \"megabytes\": " << row.megabytes
                     << "}";
                firstRow = false;
                std::cout << "       " << row.step << ": " << row.count << " steps, submit "
                          << row.submitMs << " ms, fence " << row.fenceMs << " ms, "
                          << row.megabytes << " MB\n";
            }
            json << "]}";
            first = false;
        }
    }
    json << "\n]}\n";
    if (phaseLease) {
        parkedDevice = nullptr;
        device.device->setDeviceRoomReclaimer({});
        device.device->setNativeStorageReuse(false);
        device.device->retireProcessingPhase(phaseLease->id(), true);
    }
    if (!options.json.empty()) {
        std::ofstream out(options.json);
        out << json.str();
    }
    return failures == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "bench failed: " << error.what() << '\n';
    return 1;
}
