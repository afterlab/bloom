// #486 Film Grain GPU cost benchmark (tools/bench/run_bench.py `nodes`).
//
// Each case is a real document compiled by the production SnapshotCompiler, prepared by the
// production CpuGpuSceneBuilder (the packaged shader tools) and run by the production
// GpuSceneExecutor on the device. For every case and resolution it reports:
//
// - coldMs: the first run on a fresh scene cache and executor (uploads, programs, every dispatch);
// - warmMs: the same frame again on that executor (the RAM-preview cache hit: no dispatch);
// - ms: the median rerun on one executor whose cache retains nothing (every pass dispatched
//   again, programs and allocations warm), the tier's cases interleaved round robin: the frame's
//   GPU cost, which the ratios use.
//
// Cases: the cheap reference (a Grade), the closest existing nodes (a Vignette, another per-pixel
// program that reads each pixel's position, and a procedural Noise, the other node that hashes for
// every pixel) and Film Grain over a frame-sized beauty. Its heaviest parameter is stepped: the size,
// which sets how many grains land on each pixel, every pixel always hashing the same nine cells. A
// transparent input runs no noise, which leaves the pass's own floor.
//
//   bloom_runtime_gpu_film_grain_node_bench [--tiers 1080p,2160p] [--repeats N] [--json FILE]
//                                           [--smoke] [--loader PATH]
//
// Without a device or the packaged shader tools it exits 77 (SKIP). --smoke runs every case once
// at 64x64 so the build keeps it working.

#include "gpu_coverage_ocio_support.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/film_grain_nodes.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom;

constexpr std::uint64_t kSceneBudget = 1ULL << 32U;

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
    // A Film Grain between `layer`'s solid and its Layer node.
    document::NodeId grain(const Layer& layer) {
        const auto id = node(document::kFilmGrainNodeType);
        connect(layer.solid, "image", id, "input");
        connect(id, "image", layer.layerNode, "image");
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
    // References and the closest existing nodes.
    add("base", "solid layer", "", [beauty](Doc& doc) { doc.solid(beauty); });
    add("grade", "bloom.grade", "", [beauty](Doc& doc) { doc.grade(doc.solid(beauty)); });
    add("vignette", "bloom.vignette", "", [beauty](Doc& doc) {
        const auto layer = doc.solid(beauty);
        const auto id = doc.node("bloom.vignette");
        doc.connect(layer.solid, "image", id, "input");
        doc.connect(id, "image", layer.layerNode, "image");
    });
    // A full-frame Noise, one octave: the other node that hashes for every pixel.
    add("noise", "bloom.field-noise", "", [beauty](Doc& doc) {
        const auto layer = doc.solid(beauty);
        const auto id = doc.node("bloom.field-noise");
        doc.connect(id, "image", layer.layerNode, "image");
    });
    // The default, over the Noise as the comparable.
    add("film-grain", "bloom.film-grain", "noise",
        [beauty](Doc& doc) { doc.grain(doc.solid(beauty)); });
    // The size: the fixed program's cost should not follow it.
    for (const auto& [name, size] :
         {std::pair{"0.25", 0.25}, std::pair{"4", 4.0}, std::pair{"32", 32.0}})
        add(std::string("film-grain-size-") + name, std::string("bloom.film-grain (size ") + name + ")",
            "noise",
            [beauty, value = size](Doc& doc) { doc.set(doc.grain(doc.solid(beauty)), "size", value); },
            "size", static_cast<std::int64_t>(size * 100.0));
    // Held grain draws the same pixels: its frame's cost is the same, its next frame costs nothing.
    add("film-grain-held", "bloom.film-grain (held)", "noise", [beauty](Doc& doc) {
        doc.set(doc.grain(doc.solid(beauty)), "animation", document::kFilmGrainHeld);
    });
    // Transparent, so no pixel runs the noise: the program's floor.
    add("film-grain-transparent", "bloom.film-grain (transparent input)", "noise",
        [](Doc& doc) { doc.grain(doc.solid(core::Color4d{0.8, 0.6, 0.4, 0.0})); });
    return list;
}

struct Timing final {
    bool ok = false;
    std::string error;
    double coldMs = 0;
    double warmMs = 0;
    double ms = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t warmDispatches = 0;
};

double millisecondsSince(const std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

// One begin -> Ready on `executor`, timed; false with `error` set when it does not complete.
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
    auto executor = runtime::GpuSceneExecutor::create(device, *runner.cache);
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
    const auto before = runner.executor->counters();
    if (!runOnce(*runner.executor, scene, timing.coldMs, timing.error))
        return;
    timing.dispatches = runner.executor->counters().dispatches - before.dispatches;
    const auto warmBefore = runner.executor->counters();
    if (!runOnce(*runner.executor, scene, timing.warmMs, timing.error))
        return;
    timing.warmDispatches = runner.executor->counters().dispatches - warmBefore.dispatches;
    timing.ok = true;
}

// The frame's cost: one executor over a cache that retains nothing, so every run dispatches every
// pass again. The tier's cases rerun on it round robin, so a change of device clock during the run
// moves every case alike, and each case keeps the median.

struct Options final {
    std::vector<std::string> tiers{"1080p", "2160p"};
    int repeats = 5;
    std::filesystem::path json;
    std::filesystem::path loader;
    bool smoke = false;
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
        else if (argument == "--repeats")
            options.repeats = std::max(1, std::stoi(next()));
        else if (argument == "--json")
            options.json = next();
        else if (argument == "--loader")
            options.loader = next();
        else if (argument == "--smoke")
            options.smoke = true;
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
        std::cerr << "usage: bloom_runtime_gpu_film_grain_node_bench [--tiers 1080p,2160p] [--repeats N] "
                     "[--json FILE] [--smoke] [--loader PATH]\n";
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
    std::ostringstream json;
    json << "{\"tool\": \"gpu-film-grain-node-bench\", \"repeats\": " << options.repeats
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
                        .time = core::RationalTime::fromInteger(0),
                        .output = compiled.plan->output(),
                        .resolution = runtime::CompositionFormatResolution{},
                        .quality = runtime::EvaluationQuality::Reference,
                        .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                        .pixelStorageByteLimit = kSceneBudget,
                    };
                    const auto prepared = builder.build(compiled.plan, request);
                    if (!prepared) {
                        entry.timing.error = "prepare failed: " + prepared.diagnostic.message;
                    } else {
                        entry.scene = prepared.scene;
                        entry.commandCount = std::to_string(prepared.scene->commands().size());
                        measureCold(*device.device, entry.scene, entry.timing);
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
                const auto before = executor.counters();
                double ms = 0;
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
                 << ", \"warmDispatches\": " << timing.warmDispatches << "}";
            first = false;
        }
    }
    json << "\n]}\n";
    if (!options.json.empty()) {
        std::ofstream out(options.json);
        out << json.str();
    }
    return failures == 0 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << "bench failed: " << error.what() << '\n';
    return 1;
}
