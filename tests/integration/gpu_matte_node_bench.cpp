// #383 matte workflow GPU cost benchmark (tools/bench/run_bench.py `nodes`).
//
// Each case is a real document compiled by the production SnapshotCompiler, prepared by the
// production CpuGpuSceneBuilder (the packaged shader tools, the media context) and run by the
// production GpuSceneExecutor on the device. For every case and resolution it reports:
//
// - coldMs: the first run on a fresh scene cache and executor (uploads, programs, every dispatch);
// - warmMs: the same frame again on that executor (the RAM-preview cache hit: no dispatch);
// - ms: the median rerun on one executor whose cache retains nothing (every pass dispatched
//   again, programs and allocations warm), the tier's cases interleaved round robin: the frame's
//   GPU cost, which the ratios use.
//
// Cases: the cheap reference (a Grade), the closest existing nodes (Luma Key into Image Mix, a
// path layer mask, a layer track matte, the Cryptomatte Image output) and every matte route
// (Image to Mask, Mask Combine, the effect mask, matte layer masks, node track mattes, Cryptomatte
// selections as a matte mask), with their heaviest parameters stepped (masks per layer, IDs
// picked). The Cryptomatte cases read the bench media's multilayer EXR (--media-root).
//
//   bloom_runtime_gpu_matte_node_bench [--media-root DIR] [--tiers 1080p,2160p] [--repeats N]
//                                      [--json FILE] [--smoke] [--loader PATH]
//
// Without a device or the packaged shader tools it exits 77 (SKIP). --smoke runs every
// non-media case once at 64x64 so the build keeps it working.

#include "gpu_coverage_ocio_support.hpp"

#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/layer_masks.hpp>
#include <bloom/document/matte_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/cryptomatte_matte_list.hpp>
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
    document::PathValue ellipseish(const double inset) const {
        document::PathValue path;
        path.closed = true;
        const double w = width;
        const double h = height;
        path.anchors = {{{w * inset, h * 0.5}, std::nullopt, std::nullopt},
                        {{w * 0.5, h * inset}, std::nullopt, std::nullopt},
                        {{w * (1 - inset), h * 0.5}, std::nullopt, std::nullopt},
                        {{w * 0.5, h * (1 - inset)}, std::nullopt, std::nullopt}};
        return path;
    }
    document::NodeId mask(const double inset) {
        const auto id = node(document::kMaskNodeType);
        set(id, "path", ellipseish(inset));
        return id;
    }
    // A Grade between `layer`'s solid and its Layer node.
    document::NodeId grade(const Layer& layer) {
        const auto id = node("bloom.grade");
        set(id, "exposure", 0.5);
        connect(layer.solid, "image", id, "input");
        connect(id, "image", layer.layerNode, "image");
        return id;
    }
    // Image Mix of `layer`'s solid towards a second solid by `mask`'s coverage.
    void mixBy(const Layer& layer, const document::NodeId maskNode, const std::string& port) {
        const auto second = node(document::kSolidSourceNodeType);
        set(second, "color", core::Color4d{0.1, 0.3, 0.8, 1.0});
        set(second, "width", static_cast<double>(width));
        set(second, "height", static_cast<double>(height));
        const auto mix = node("bloom.image-mix");
        connect(layer.solid, "image", mix, "input");
        connect(second, "image", mix, "second");
        connect(maskNode, port, mix, "mask");
        connect(mix, "image", layer.layerNode, "image");
    }
};

struct Media final {
    std::filesystem::path directory;
    std::filesystem::path file;
};

// The Cryptomatte objects of the bench scene (tools/bench/blender_scene.py), most visible first.
std::vector<std::string> objectNames(const std::size_t count) {
    std::vector<std::string> names;
    for (int i = 0; i < 12; ++i)
        names.push_back("Pillar" + std::to_string(i));
    for (int i = 0; i < 5; ++i)
        names.push_back("Card" + std::to_string(i));
    names.emplace_back("Veil");
    for (int i = 0; i < 360; ++i) {
        std::string index = std::to_string(i);
        names.push_back("Bit" + std::string(3 - index.size(), '0') + index);
    }
    names.resize(std::min(count, names.size()));
    return names;
}

struct ExrLayer final {
    Doc::Layer layer;
    document::NodeId source;
};

// The multilayer EXR's beauty as a layer, its source's channel set available.
ExrLayer exrLayer(Doc& doc, const Media& media) {
    doc.run<commands::ImportAssets>(std::vector{media.file}, media.directory);
    doc.run<commands::AddImageLayer>(doc.composition,
                                     doc.document.snapshot().project().assets().front().id);
    ExrLayer result{};
    for (const auto& node : doc.graph().nodes()) {
        if (node.typeId == document::kImageSourceNodeType)
            result.source = node.id;
    }
    result.layer.id = doc.graph().layerOutputs().front().layerId;
    result.layer.layerNode = doc.graph().layerOutputs().front().nodeId;
    result.layer.solid = result.source;
    doc.run<commands::SetNodeParameterValue>(doc.composition, result.source, "layer",
                                             document::ParameterValue{std::string("Composite.Combined")});
    return result;
}

document::NodeId cryptomatte(Doc& doc, const document::NodeId source, const std::size_t ids) {
    const auto node = doc.node(document::kCryptomatteNodeType);
    doc.connect(source, std::string(document::kImageSourceChannelsOutputPort), node,
                std::string(document::kChannelSetPort));
    doc.run<commands::SetNodeParameterValue>(
        doc.composition, node, std::string(document::kCryptomatteMattesParameterRole),
        document::ParameterValue{runtime::joinCryptomatteMatteList(objectNames(ids))});
    return node;
}

struct Case final {
    std::string name;
    std::string node;       // the node or route measured
    std::string parameter;  // the stepped heaviest parameter, or empty
    std::int64_t value = 0; // its value
    std::string reference;  // the cheap reference case at the same resolution
    std::string comparable; // the closest existing case at the same resolution
    bool media = false;
    std::function<void(Doc&, const Media*)> build;
};

std::vector<Case> cases() {
    using document::LayerMaskMode;
    std::vector<Case> list;
    const auto solidCase = [&](std::string name, std::string node, std::string comparable,
                               std::function<void(Doc&)> build, std::string parameter = {},
                               std::int64_t value = 0) {
        list.push_back(Case{std::move(name), std::move(node), std::move(parameter), value, "grade",
                            std::move(comparable), false,
                            [build = std::move(build)](Doc& doc, const Media*) { build(doc); }});
    };
    // References and the closest existing routes.
    solidCase("base", "solid layer", "", [](Doc& doc) { doc.solid({0.8, 0.6, 0.4, 1.0}); });
    solidCase("grade", "bloom.grade", "", [](Doc& doc) {
        doc.grade(doc.solid({0.8, 0.6, 0.4, 1.0}));
    });
    solidCase("luma-key-mix", "bloom.luma-key", "", [](Doc& doc) {
        const auto layer = doc.solid({0.8, 0.6, 0.4, 1.0});
        const auto key = doc.node("bloom.luma-key");
        doc.connect(layer.solid, "image", key, "input");
        doc.mixBy(layer, key, "mask");
    });
    for (const std::int64_t count : {1, 4, 16})
        solidCase("path-layer-masks-" + std::to_string(count), "layer mask (path)", "",
                  [count](Doc& doc) {
                      const auto layer = doc.solid({0.8, 0.6, 0.4, 1.0});
                      for (std::int64_t i = 0; i < count; ++i)
                          doc.run<commands::AddLayerMask>(
                              doc.composition, layer.id,
                              doc.ellipseish(0.02 * static_cast<double>(i % 10)),
                              i == 0 ? LayerMaskMode::Add : LayerMaskMode::Intersect);
                  },
                  "masks", count);
    solidCase("layer-track-matte", "track matte (layer)", "", [](Doc& doc) {
        const auto matte = doc.solid({1, 1, 1, 0.5});
        const auto target = doc.solid({0.8, 0.6, 0.4, 1.0});
        doc.run<commands::SetTrackMatte>(
            doc.composition, target.id,
            document::TrackMatte{matte.id, document::TrackMatteChannel::Alpha, false});
    });
    // The matte routes.
    solidCase("image-to-mask-mix", "bloom.image-to-mask", "luma-key-mix", [](Doc& doc) {
        const auto layer = doc.solid({0.8, 0.6, 0.4, 1.0});
        const auto toMask = doc.node(document::kImageToMaskNodeType);
        doc.set(toMask, "channel", std::int64_t{4}); // Luma
        doc.connect(layer.solid, "image", toMask, "input");
        doc.mixBy(layer, toMask, "mask");
    });
    solidCase("mask-combine-mix", "bloom.mask-combine", "luma-key-mix", [](Doc& doc) {
        const auto layer = doc.solid({0.8, 0.6, 0.4, 1.0});
        const auto combine = doc.node(document::kMaskCombineNodeType);
        doc.set(combine, "mode", std::int64_t{3}); // Difference: the longest chain
        doc.connect(doc.mask(0.1), "mask", combine, "a");
        doc.connect(doc.mask(0.3), "mask", combine, "b");
        doc.mixBy(layer, combine, "mask");
    });
    solidCase("grade-effect-mask", "effect mask (Grade)", "grade", [](Doc& doc) {
        const auto layer = doc.solid({0.8, 0.6, 0.4, 1.0});
        const auto grade = doc.grade(layer);
        doc.connect(doc.mask(0.1), "mask", grade, "mask");
    });
    // The existing way to apply N node Masks to an image: N Image Mix nodes, each by its Mask.
    for (const std::int64_t count : {1, 4, 16})
        solidCase("mask-mix-chain-" + std::to_string(count), "bloom.image-mix (Mask)", "",
                  [count](Doc& doc) {
                      const auto layer = doc.solid({0.8, 0.6, 0.4, 1.0});
                      const auto clear = doc.node(document::kSolidSourceNodeType);
                      doc.set(clear, "color", core::Color4d{0.0, 0.0, 0.0, 0.0});
                      doc.set(clear, "width", static_cast<double>(doc.width));
                      doc.set(clear, "height", static_cast<double>(doc.height));
                      auto previous = layer.solid;
                      for (std::int64_t i = 0; i < count; ++i) {
                          const auto mix = doc.node("bloom.image-mix");
                          doc.connect(previous, "image", mix, "input");
                          doc.connect(clear, "image", mix, "second");
                          doc.connect(doc.mask(0.02 * static_cast<double>(i % 10)), "mask", mix,
                                      "mask");
                          previous = mix;
                      }
                      doc.connect(previous, "image", layer.layerNode, "image");
                  },
                  "masks", count);
    for (const std::int64_t count : {1, 4, 16})
        solidCase("matte-layer-masks-" + std::to_string(count), "layer mask (node Mask)",
                  "mask-mix-chain-" + std::to_string(count),
                  [count](Doc& doc) {
                      const auto layer = doc.solid({0.8, 0.6, 0.4, 1.0});
                      for (std::int64_t i = 0; i < count; ++i)
                          doc.run<commands::AddLayerMask>(
                              doc.composition, layer.id, document::PathValue{},
                              i == 0 ? LayerMaskMode::Add : LayerMaskMode::Intersect,
                              std::string{},
                              document::OutputPortRef{
                                  doc.mask(0.02 * static_cast<double>(i % 10)), "mask"});
                  },
                  "masks", count);
    solidCase("node-track-matte", "track matte (node Mask)", "layer-track-matte", [](Doc& doc) {
        const auto target = doc.solid({0.8, 0.6, 0.4, 1.0});
        doc.run<commands::SetTrackMatte>(
            doc.composition, target.id,
            document::TrackMatte{document::LayerId{}, document::TrackMatteChannel::Alpha, false,
                                 document::OutputPortRef{doc.mask(0.1), "mask"}});
    });
    // Cryptomatte on the bench EXR: the existing Image output (the comparable), and a selection
    // of 1 to 256 objects as the beauty's matte mask and as a Grade's effect mask.
    const auto mediaCase = [&](std::string name, std::string node, std::string comparable,
                               std::function<void(Doc&, const Media&)> build,
                               std::string parameter = {}, std::int64_t value = 0) {
        list.push_back(Case{std::move(name), std::move(node), std::move(parameter), value,
                            "exr-grade", std::move(comparable), true,
                            [build = std::move(build)](Doc& doc, const Media* media) {
                                build(doc, *media);
                            }});
    };
    mediaCase("exr-base", "EXR beauty layer", "", [](Doc& doc, const Media& media) {
        (void)exrLayer(doc, media);
    });
    mediaCase("exr-grade", "bloom.grade (EXR)", "", [](Doc& doc, const Media& media) {
        doc.grade(exrLayer(doc, media).layer);
    });
    mediaCase("cryptomatte-image-16", "bloom.cryptomatte (Image output)", "",
              [](Doc& doc, const Media& media) {
                  const auto exr = exrLayer(doc, media);
                  const auto node = cryptomatte(doc, exr.source, 16);
                  doc.connect(exr.source, "image", node, "image");
                  doc.connect(node, "image", exr.layer.layerNode, "image");
              });
    for (const std::int64_t ids : {1, 16, 64, 256}) {
        mediaCase("cryptomatte-layer-mask-" + std::to_string(ids), "layer mask (Cryptomatte)",
                  "cryptomatte-image-16",
                  [ids](Doc& doc, const Media& media) {
                      const auto exr = exrLayer(doc, media);
                      const auto node = cryptomatte(doc, exr.source, static_cast<std::size_t>(ids));
                      doc.run<commands::AddLayerMask>(
                          doc.composition, exr.layer.id, document::PathValue{},
                          LayerMaskMode::Add, std::string{},
                          document::OutputPortRef{node, "mask"});
                  },
                  "ids", ids);
    }
    mediaCase("cryptomatte-effect-mask-16", "effect mask (Cryptomatte)", "cryptomatte-image-16",
              [](Doc& doc, const Media& media) {
                  const auto exr = exrLayer(doc, media);
                  const auto grade = doc.grade(exr.layer);
                  doc.connect(cryptomatte(doc, exr.source, 16), "mask", grade, "mask");
              },
              "ids", 16);
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
    std::filesystem::path mediaRoot;
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
            options.mediaRoot = next();
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
        std::cerr << "usage: bloom_runtime_gpu_matte_node_bench [--media-root DIR] [--tiers "
                     "1080p,2160p] [--repeats N] [--json FILE] [--smoke] [--loader PATH]\n";
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
    json << "{\"tool\": \"gpu-matte-node-bench\", \"repeats\": " << options.repeats
         << ", \"cases\": [";
    bool first = true;
    int failures = 0;
    for (const auto& tier : tiers) {
        std::optional<Media> media;
        if (!options.smoke && !options.mediaRoot.empty()) {
            const auto directory = options.mediaRoot / tier.name / "exr_multilayer";
            const auto file = directory / ("bench_" + tier.name + "_multilayer.0001.exr");
            if (std::filesystem::is_regular_file(file))
                media = Media{directory, file};
            else
                std::cerr << "NOTE: no multilayer EXR at " << file
                          << "; the Cryptomatte cases are skipped\n";
        }
        struct Entry final {
            Case item;
            std::shared_ptr<const runtime::PreparedGpuScene> scene;
            std::string commandCount;
            Timing timing;
            std::vector<double> samples;
        };
        std::vector<Entry> entries;
        for (auto& item : cases()) {
            if (item.media && !media)
                continue;
            Entry entry{std::move(item), {}, {}, {}, {}};
            Doc doc(tier.width, tier.height);
            try {
                entry.item.build(doc, media ? &*media : nullptr);
                runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
                const auto compiled =
                    compiler.compile({doc.document.snapshot(), doc.composition, {}}, {});
                if (!compiled.plan) {
                    entry.timing.error = "compile failed";
                    for (const auto& diagnostic : compiled.diagnostics)
                        entry.timing.error += ": " + diagnostic.summary;
                } else {
                    runtime::CpuCompositionEvaluator evaluator;
                    if (media)
                        evaluator.setAssetBaseDirectory(media->directory);
                    auto context = runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
                    if (media)
                        context.assetBaseDirectory = media->directory;
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
