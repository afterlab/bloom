// #480: a Composition Source's `channels` output on real documents, so render passes survive a
// precomp.
//
// A four-frame multilayer OpenEXR sequence (root RGBA, depth.Z) is precomped in "Render" (4 fps):
// its Image source's channels plus an Image to Channel Set layer `proc` (the image's red) reach the
// Render Composition Output's channels. The main composition (2 fps) reads the precomp's channels:
//   - a Pass of depth.Z is the production channel gather of the nested file layer, read at the
//     mapped nested time (offset, scale, loop and an animated offset, at the nested frame rate);
//   - a Pass of proc and a default Shuffle (the composite) are Channel Select effects over nested
//     Composition Source images;
//   - two levels of precomps chain their mappings; the layered output, the channel listing, a
//     cancelled read and a small budget behave like the image path; a nesting cycle is refused.
// The CPU assertions always run. Each state is then prepared by the production CpuGpuSceneBuilder
// (with the media context), run by the production GpuSceneExecutor and must match the CPU
// reference (2e-6) and dispatch nothing new warm. Without a device the GPU half reports SKIP (77);
// --require-device makes that a failure.

#include "channel_source_time.hpp"
#include "document_render_parity_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/channel_inspection_listing.hpp>
#include <bloom/runtime/channel_layers.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/task_types.hpp>

#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfOutputFile.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

constexpr std::uint32_t kWidth = 4;
constexpr std::uint32_t kHeight = 2;
constexpr std::size_t kSamples = kWidth * kHeight;

document::CompositionFormat format(const std::uint32_t fps) {
    return *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                                *document::FrameRate::create(fps, 1));
}
core::RationalTime seconds(const std::int64_t numerator, const std::int64_t denominator = 1) {
    return *core::RationalTime::create(numerator, denominator);
}

// Sequence member k (1-4): R = 0.01 k, G = 0.5, B = 0.25, A = 1, depth.Z = 100 k + pixel.
void writeFrame(const std::filesystem::path& path, const int member) {
    const Imath::Box2i window(Imath::V2i(0, 0), Imath::V2i(kWidth - 1, kHeight - 1));
    std::vector<std::pair<std::string, std::vector<float>>> planes;
    const auto plane = [](std::string name, auto&& sample) {
        std::vector<float> values;
        for (std::size_t index = 0; index < kSamples; ++index)
            values.push_back(sample(index));
        return std::pair{std::move(name), std::move(values)};
    };
    planes.push_back(plane("R", [&](std::size_t) { return 0.01F * static_cast<float>(member); }));
    planes.push_back(plane("G", [](std::size_t) { return 0.5F; }));
    planes.push_back(plane("B", [](std::size_t) { return 0.25F; }));
    planes.push_back(plane("A", [](std::size_t) { return 1.0F; }));
    planes.push_back(plane("depth.Z", [&](std::size_t index) {
        return 100.0F * static_cast<float>(member) + static_cast<float>(index);
    }));
    Imf::Header header(window, window);
    for (const auto& [name, values] : planes)
        header.channels().insert(name, Imf::Channel(Imf::FLOAT));
    Imf::FrameBuffer frameBuffer;
    for (auto& [name, values] : planes)
        frameBuffer.insert(name, Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(values.data()),
                                            sizeof(float), kWidth * sizeof(float)));
    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(frameBuffer);
    file.writePixels(static_cast<int>(kHeight));
}

// A token whose task was cancelled (the scheduler is the only producer of tokens).
[[nodiscard]] runtime::CancellationToken makeCancelledToken() {
    using namespace bloom::runtime;
    TaskSchedulerConfig config = TaskSchedulerConfig::defaults();
    config.cpuWorkerCount = 1;
    config.blockingIoWorkerCount = 1;
    TaskScheduler scheduler(config);
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    CancellationToken token;
    auto submission = scheduler.submit<void>(
        TaskRequest("precomp channels cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(99)}),
        [&](TaskContext& context) {
            {
                std::lock_guard lock(mutex);
                token = context.cancellation();
                entered = true;
            }
            condition.notify_all();
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] { return release; });
            return TaskResult<void>::succeeded();
        });
    if (!submission.accepted())
        throw std::logic_error("cancel token task was refused");
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return entered; });
    }
    submission.handle.cancel();
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!submission.handle.tryTakeResult().has_value() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return token;
}

[[nodiscard]] bool near(const float value, const float expected, const float tolerance = 1e-4F) {
    return std::fabs(value - expected) <= tolerance;
}

struct Fixture {
    std::filesystem::path folder;
    document::NewProject initial =
        document::makeNewProject("Precomp passes", "Main", seconds(2), format(2));
    document::CompositionId main = initial.initialCompositionId;
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::CompositionId render;
    document::NodeId renderSource;
    document::NodeId precomp;
    document::NodeId mainLayer;

    Fixture() {
        std::mt19937_64 random{std::random_device{}()};
        folder = std::filesystem::temp_directory_path() /
                 ("bloom-precomp-channels-" + std::to_string(random()));
        std::filesystem::create_directories(folder);
        for (int member = 1; member <= 4; ++member)
            writeFrame(folder / ("render.000" + std::to_string(member) + ".exr"), member);
        execute<commands::ImportAssets>(document, commands,
                                        std::vector{folder / "render.0001.exr"}, folder);
        const auto asset = document.snapshot().project().assets().front();
        if (asset.kind != document::AssetKind::Sequence)
            throw std::runtime_error("the render did not import as a sequence");
        execute<commands::AddComposition>(document, commands, "Render", format(4), seconds(1));
        render = document.snapshot().project().compositions().back().id();
        execute<commands::AddImageLayer>(document, commands, render, asset.id);
        renderSource = lastNode(document, render, document::kImageSourceNodeType);
        // Render's channels: the file's layers plus `proc`, an Image to Channel Set of its image.
        const auto pack = add(render, document::kImageToChannelSetNodeType);
        connect(render, renderSource, "image", pack, document::kImageToChannelSetImagePort);
        set(render, pack, document::kImageToChannelSetLayerParameterRole, std::string("proc"));
        set(render, pack, document::kImageToChannelSetChannelsParameterRole, std::string("Z"));
        const auto copy = add(render, document::kChannelCopyNodeType);
        connect(render, renderSource, document::kImageSourceChannelsOutputPort, copy,
                document::kChannelSetPort);
        connect(render, pack, document::kChannelSetPort, copy, document::kChannelCopyFromPort);
        set(render, copy, "select", std::string("proc"));
        connect(render, copy, document::kChannelSetPort, outputNode(render),
                document::kCompositionOutputChannelsInputPort);
        execute<commands::AddCompositionSource>(document, commands, main, render,
                                                document::Vec2d{}, true);
        precomp = lastNode(document, main, document::kCompositionSourceNodeType);
        mainLayer = layerNode(document, main, lastLayer(document, main));
        // The precomp layer spans the whole main composition, past the precomp's own 1 s.
        execute<commands::SetLayerRange>(document, commands, main, lastLayer(document, main),
                                         seconds(0), seconds(2));
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    document::NodeId outputNode(const document::CompositionId target) {
        return composition(document, target).graph().compositionOutput()->nodeId;
    }
    document::NodeId add(const document::CompositionId target, const std::string_view type) {
        commands::Transaction transaction("Fixture", document.snapshot().revision());
        transaction.emplace<commands::AddNode>(target, std::string(type), document::Vec2d{});
        auto result = commands.execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("AddNode refused");
        return *result.outputId<document::NodeId>(commands::kAddNodeOutput);
    }
    void connect(const document::CompositionId target, const document::NodeId from,
                 const std::string_view output, const document::NodeId to,
                 const std::string_view input) {
        execute<commands::ConnectPorts>(document, commands, target,
                                        document::OutputPortRef{from, std::string(output)},
                                        document::NodeInputRef{to, std::string(input)});
    }
    void set(const document::CompositionId target, const document::NodeId node,
             const std::string_view role, document::ParameterValue value) {
        execute<commands::SetParameterSource>(document, commands, target,
                                              parameter(document, target, node, role),
                                              document::ConstantValueSource{std::move(value)});
    }
    // A Pass of the precomp's channels feeding the main layer.
    document::NodeId pass(const std::string& picked) {
        const auto node = add(main, document::kChannelPassNodeType);
        connect(main, precomp, document::kCompositionSourceChannelsOutputPort, node,
                document::kChannelSetPort);
        connect(main, node, document::kChannelPassImagePort, mainLayer,
                document::kLayerOutputContentInputPort);
        set(main, node, document::kChannelPassParameterRole, picked);
        return node;
    }

    [[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> compile() {
        const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
        const auto compiled = compiler.compile({document.snapshot(), main}, {});
        if (!compiled.plan)
            throw std::runtime_error(
                "compile failed: " +
                runtime::describeCompileFailure(compiled, document.snapshot().project(), main));
        return compiled.plan;
    }
    [[nodiscard]] runtime::EvaluationResult
    evaluate(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
             const core::RationalTime time, const std::size_t budget = kSceneBudget,
             const runtime::CancellationToken& cancellation = {}) const {
        runtime::CpuCompositionEvaluator evaluator;
        evaluator.setAssetBaseDirectory(folder);
        auto request = requestFor(*plan);
        request.time = time;
        request.bypassOperationCache = true;
        request.pixelStorageByteLimit = budget;
        return evaluator.evaluate(plan, request, cancellation);
    }
    // The CPU reference at `time` (main composition pixel 0..7 in row order).
    [[nodiscard]] std::optional<Reference> reference(Expectations& expectations,
                                                     const core::RationalTime time,
                                                     const std::string& label) {
        const auto plan = compile();
        const auto result = evaluate(plan, time);
        std::string why;
        for (const auto& diagnostic : result.diagnostics())
            why += " [" + diagnostic.summary + ": " + diagnostic.detail + "]";
        expectations.expect(result.frame() != nullptr,
                            label + ": the CPU reference evaluates" + why);
        if (result.frame() == nullptr)
            return std::nullopt;
        auto request = requestFor(*plan);
        request.time = time;
        return Reference{plan, request, result.frame()};
    }
};

[[nodiscard]] float redAt(const Reference& reference, const std::size_t pixel) {
    const auto pixels = reference.frame->processImage().pixels();
    return pixel < pixels.size() ? pixels[pixel].red() : std::numeric_limits<float>::quiet_NaN();
}

template <typename Kind> [[nodiscard]] std::size_t count(const runtime::CompiledCompositionPlan& plan) {
    return static_cast<std::size_t>(std::ranges::count_if(
        plan.operations(), [](const auto& item) { return std::holds_alternative<Kind>(item); }));
}

[[nodiscard]] bool hasChannelSelect(const runtime::CompiledCompositionPlan& plan) {
    return std::ranges::any_of(plan.operations(), [](const auto& item) {
        const auto* effect = std::get_if<runtime::CompiledImageEffect>(&item);
        return effect != nullptr && std::holds_alternative<runtime::ChannelSelectKernel>(effect->kernel);
    });
}

#ifdef BLOOM_GPUSHADER_TOOLS_DIR
// The pinned shader tools prepare the Channel Select / Channel Combine programs.
[[nodiscard]] runtime::GpuSceneOcioContext ocioContext() {
    runtime::GpuOcioCompileOptions options;
    options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    runtime::GpuSceneOcioContext context;
    context.preparer = std::make_shared<runtime::GpuOcioProgramPreparer>();
    context.compileOptions = options;
    return context;
}

// GPU parity with the media context the EXR sources need.
void mediaParity(Expectations& expectations, runtime::GpuSceneExecutor& executor,
                 const std::filesystem::path& folder, const Reference& reference,
                 const std::string& label) {
    runtime::CpuCompositionEvaluator evaluator;
    evaluator.setAssetBaseDirectory(folder);
    auto context = runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
    context.assetBaseDirectory = folder;
    const runtime::CpuGpuSceneBuilder builder(nullptr, context, ocioContext());
    const auto prepared = builder.build(reference.plan, reference.request);
    expectations.expect(prepared.hasValue(), label + ": prepares: " + prepared.diagnostic.message);
    if (!prepared)
        return;
    const auto expected = reference.frame->processImage().pixels();
    const auto cold = runScene(executor, prepared.scene);
    expectations.expect(!cold.empty(),
                        label + ": the native run completes: " + executor.diagnostic().message);
    expectations.expect(parity(cold, expected), label + ": GPU pixels match the CPU oracle (2e-6)");
    const auto before = executor.counters();
    const auto warm = runScene(executor, prepared.scene);
    expectations.expect(parity(warm, expected) &&
                            executor.counters().dispatches == before.dispatches,
                        label + ": a warm rerun matches and dispatches nothing new");
}
#endif

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "invalid arguments\n";
            return 2;
        }
        Expectations expectations;
        std::vector<std::tuple<std::string, std::filesystem::path, Reference>> references;
        std::vector<std::unique_ptr<Fixture>> fixtures;
        const auto keep = [&](const Fixture& fixture, const std::string& label,
                              const std::optional<Reference>& reference) {
            if (reference)
                references.emplace_back(label, fixture.folder, *reference);
            return reference;
        };

        // The nested file layer at the mapped nested time. Render runs at 4 fps for 1 s, so
        // nested time n/4 reads member n + 1 (depth 100 (n + 1) + pixel).
        {
            auto& fixture = *fixtures.emplace_back(std::make_unique<Fixture>());
            fixture.pass("depth");
            const auto plan = fixture.compile();
            const auto* shuffle = [&]() -> const runtime::CompiledChannelShuffle* {
                for (const auto& item : plan->operations())
                    if (const auto* found = std::get_if<runtime::CompiledChannelShuffle>(&item))
                        return found;
                return nullptr;
            }();
            expectations.expect(
                shuffle != nullptr &&
                    std::ranges::any_of(shuffle->channels.sources,
                                        [&](const auto& source) {
                                            return !source.imageBacked &&
                                                   source.nesting.size() == 1 &&
                                                   source.nesting[0].sourceNodeId ==
                                                       fixture.precomp;
                                        }),
                "a Pass of a precomp's file layer is the channel gather through the precomp");
            const auto depthAt = [&](const std::string& label, const core::RationalTime time,
                                     const float member) {
                if (const auto reference =
                        keep(fixture, label, fixture.reference(expectations, time, label)))
                    expectations.expect(near(redAt(*reference, 0), 100.0F * member) &&
                                            near(redAt(*reference, 5), 100.0F * member + 5.0F),
                                        label + ": reads member " + std::to_string(member) +
                                            ", not " + std::to_string(redAt(*reference, 0)));
            };
            // 0.5 s at the nested 4 fps is nested frame 2 (the parent's 2 fps would read 1).
            depthAt("nested rate", seconds(1, 2), 3);
            fixture.set(fixture.main, fixture.precomp, "timeOffset", -0.25);
            depthAt("offset", seconds(1, 2), 4);
            fixture.set(fixture.main, fixture.precomp, "timeOffset", 0.0);
            fixture.set(fixture.main, fixture.precomp, "timeScale", 0.5);
            depthAt("scale", seconds(1), 3);
            fixture.set(fixture.main, fixture.precomp, "timeScale", 1.0);
            // Past the precomp's end Hold reads its last frame; Loop wraps.
            fixture.set(fixture.main, fixture.precomp, "timeOffset", -0.5);
            depthAt("hold", seconds(1, 2), 4);
            fixture.set(fixture.main, fixture.precomp, "timeOffset", 0.0);
            fixture.set(fixture.main, fixture.precomp, "loopMode", std::int64_t{1});
            depthAt("loop", seconds(3, 2), 3);
            fixture.set(fixture.main, fixture.precomp, "loopMode", std::int64_t{0});
            // An animated offset: 0 at 0 s, 0.5 at 1 s, so 1 s reads nested 0.5 s (member 3);
            // the unanimated offset would read nested 1 s, held at member 4.
            const auto offset = parameter(fixture.document, fixture.main, fixture.precomp,
                                          "timeOffset");
            execute<commands::CreateAnimationForParameter>(fixture.document, fixture.commands,
                                                           fixture.main, offset, seconds(0));
            execute<commands::SetKeyframeAtTimeForParameter>(fixture.document, fixture.commands,
                                                             fixture.main, offset, seconds(1),
                                                             0.5);
            depthAt("animated offset", seconds(1), 3);

            // The pickers list the precomp's composite and its layers.
            const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
            const auto listing = runtime::listChannelInspectionChoices(
                compiler, fixture.document.snapshot(), fixture.main, fixture.precomp,
                std::string(document::kCompositionSourceChannelsOutputPort), fixture.folder,
                seconds(0), runtime::EvaluationColorIntent::LinearRec709Scene, {});
            const auto listed = [&](const std::string& name) {
                return std::ranges::find(listing.channels, name) != listing.channels.end();
            };
            expectations.expect(listing.diagnostic.empty() && listed("R") && listed("A") &&
                                    listed("depth.Z") && listed("proc.Z"),
                                "the channel pickers list the precomp's composite and layers: " +
                                    listing.diagnostic);

            // A cancelled read of the nested clock reports cancellation, never a frame.
            const auto cancelled = makeCancelledToken();
            expectations.expect(cancelled.isCancellationRequested(), "the token is cancelled");
            if (shuffle != nullptr) {
                const auto times = runtime::detail::resolveChannelSourceTimes(
                    shuffle->channels, seconds(0), plan->format().frameRate(),
                    runtime::detail::ChannelSourceClock{plan.get(), nullptr}, cancelled);
                expectations.expect(times.cancelled,
                                    "a cancelled nested channel time is cancelled");
            }
            const auto stopped = fixture.evaluate(plan, seconds(0), kSceneBudget, cancelled);
            expectations.expect(stopped.frame() == nullptr,
                                "a cancelled precomp Pass produces no frame");
        }

        // The precomp's Image to Channel Set layer and its composite are image channels: Channel
        // Select over nested Composition Source images, at the mapped time.
        {
            auto& fixture = *fixtures.emplace_back(std::make_unique<Fixture>());
            fixture.pass("proc");
            fixture.set(fixture.main, fixture.precomp, "timeOffset", -0.25);
            const auto plan = fixture.compile();
            expectations.expect(count<runtime::CompiledChannelShuffle>(*plan) == 0 &&
                                    hasChannelSelect(*plan) &&
                                    count<runtime::CompiledCompositionSource>(*plan) == 1,
                                "a Pass of a precomp's packed layer is a Channel Select over a "
                                "nested Composition Source");
            if (const auto reference = keep(
                    fixture, "packed layer",
                    fixture.reference(expectations, seconds(1, 2), "packed layer")))
                expectations.expect(near(redAt(*reference, 3), 0.04F),
                                    "packed layer: reads the nested image at the mapped time");
            // A default Shuffle of the precomp's channels is its composite.
            const auto shuffle = fixture.add(fixture.main, document::kChannelShuffleNodeType);
            fixture.connect(fixture.main, fixture.precomp,
                            document::kCompositionSourceChannelsOutputPort, shuffle,
                            document::kChannelSetPort);
            fixture.connect(fixture.main, shuffle, document::kChannelShuffleImagePort,
                            fixture.mainLayer, document::kLayerOutputContentInputPort);
            const auto composite = fixture.compile();
            expectations.expect(count<runtime::CompiledChannelShuffle>(*composite) == 0 &&
                                    hasChannelSelect(*composite),
                                "a default Shuffle of a precomp reads its composite statically");
            if (const auto reference =
                    keep(fixture, "composite",
                         fixture.reference(expectations, seconds(1, 2), "composite")))
                expectations.expect(near(redAt(*reference, 0), 0.04F) &&
                                        near(reference->frame->processImage().pixels()[0].green(),
                                             0.5F),
                                    "composite: the precomp's own image at the mapped time");
            // Mixing the composite with a file layer is refused like any image/file mix.
            fixture.set(fixture.main, shuffle, "green", std::string("depth.Z"));
            const auto mixed = fixture.evaluate(fixture.compile(), seconds(1, 2));
            expectations.expect(
                std::ranges::any_of(mixed.diagnostics(),
                                    [](const auto& item) {
                                        return item.summary.find("image channel R") !=
                                               std::string::npos;
                                    }),
                "a Shuffle mixing the composite and a file layer is refused with a diagnostic");
        }

        // Two levels: Main -> Mid (offset -0.5 s) -> Render (offset -0.25 s).
        {
            auto& fixture = *fixtures.emplace_back(std::make_unique<Fixture>());
            execute<commands::AddComposition>(fixture.document, fixture.commands, "Mid",
                                              format(2), seconds(2));
            const auto mid = fixture.document.snapshot().project().compositions().back().id();
            execute<commands::AddCompositionSource>(fixture.document, fixture.commands, mid,
                                                    fixture.render, document::Vec2d{}, true);
            const auto inner = lastNode(fixture.document, mid, document::kCompositionSourceNodeType);
            fixture.connect(mid, inner, document::kCompositionSourceChannelsOutputPort,
                            fixture.outputNode(mid), document::kCompositionOutputChannelsInputPort);
            fixture.set(mid, inner, "timeOffset", -0.25);
            fixture.set(fixture.main, fixture.precomp, "composition",
                        static_cast<std::int64_t>(mid.value()));
            fixture.set(fixture.main, fixture.precomp, "timeOffset", -0.5);
            fixture.pass("depth");
            // Main 0 s -> Mid 0.5 s -> Render 0.75 s: member 4.
            if (const auto reference =
                    keep(fixture, "two levels",
                         fixture.reference(expectations, seconds(0), "two levels")))
                expectations.expect(near(redAt(*reference, 2), 402.0F),
                                    "two levels: each precomp's mapping applies in turn");
            fixture.set(fixture.main, fixture.precomp, "timeOffset", 0.0);
            fixture.pass("proc");
            if (const auto reference =
                    keep(fixture, "two levels packed",
                         fixture.reference(expectations, seconds(0), "two levels packed")))
                expectations.expect(near(redAt(*reference, 1), 0.02F),
                                    "two levels: a packed layer chains both mappings");

            // A nesting cycle (Render containing Main) is refused.
            commands::Transaction transaction("Cycle", fixture.document.snapshot().revision());
            transaction.emplace<commands::AddCompositionSource>(fixture.render, fixture.main,
                                                                document::Vec2d{}, true);
            const auto refused = fixture.commands.execute(std::move(transaction));
            expectations.expect(!refused.succeeded(), "a precomp nesting cycle is refused");
        }

        // The layered output writes the precomp's layers, read at the mapped time, but never its
        // composite RGBA root as layers; a small budget refuses like a direct read.
        {
            auto& fixture = *fixtures.emplace_back(std::make_unique<Fixture>());
            fixture.connect(fixture.main, fixture.precomp,
                            document::kCompositionSourceChannelsOutputPort,
                            fixture.outputNode(fixture.main),
                            document::kCompositionOutputChannelsInputPort);
            fixture.set(fixture.main, fixture.precomp, "timeOffset", -0.25);
            const auto plan = fixture.compile();
            const auto created = render::ImageWindow::create(0, 0, kWidth, kHeight);
            const auto window = *created.value();
            const auto images =
                [&](const std::shared_ptr<const runtime::CompiledCompositionPlan>& layerPlan) {
                    runtime::ChannelLayerImage layer;
                    const auto result = fixture.evaluate(layerPlan, seconds(0));
                    if (!result.frame()) {
                        layer.failed = true;
                        return layer;
                    }
                    layer.image = std::shared_ptr<const render::Rgba32fImage>(
                        result.frame(), &result.frame()->processImage());
                    return layer;
                };
            const auto assembled = runtime::assembleOutputChannelLayers(
                *plan, seconds(0), fixture.folder,
                runtime::EvaluationColorIntent::LinearRec709Scene, window, window, kSceneBudget, {},
                images);
            const auto depth = assembled.layers ? assembled.layers->find("depth.Z") : std::nullopt;
            const auto proc = assembled.layers ? assembled.layers->find("proc.Z") : std::nullopt;
            expectations.expect(assembled.ok() && depth && proc && !assembled.layers->find("R"),
                                "the layered output writes the precomp's layers: " +
                                    assembled.diagnostic);
            if (depth && proc)
                expectations.expect(near(assembled.layers->plane(*depth)[1], 201.0F) &&
                                        near(assembled.layers->plane(*proc)[1], 0.02F),
                                    "the precomp's layers are written at the mapped time");

            fixture.pass("depth");
            const auto tiny = fixture.evaluate(fixture.compile(), seconds(0), 64);
            Fixture direct;
            const auto directPass = direct.add(direct.render, document::kChannelPassNodeType);
            direct.connect(direct.render, direct.renderSource,
                           document::kImageSourceChannelsOutputPort, directPass,
                           document::kChannelSetPort);
            direct.connect(direct.render, directPass, document::kChannelPassImagePort,
                           layerNode(direct.document, direct.render,
                                     lastLayer(direct.document, direct.render)),
                           document::kLayerOutputContentInputPort);
            direct.set(direct.render, directPass, document::kChannelPassParameterRole,
                       std::string("depth"));
            const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
            const auto directPlan = compiler.compile({direct.document.snapshot(), direct.render}, {});
            const auto directTiny = direct.evaluate(directPlan.plan, seconds(0), 64);
            expectations.expect(tiny.frame() == nullptr && directTiny.frame() == nullptr,
                                "a precomp Pass over budget is refused like a direct Pass");
        }

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " precomp channel expectation(s) failed\n";
            return 1;
        }

#ifndef BLOOM_GPUSHADER_TOOLS_DIR
        if (options.require_device) {
            std::cerr << "FAIL: --require-device requested but BLOOM_GPUSHADER_TOOLS_DIR is not "
                         "set\n";
            return 1;
        }
        std::cout << "SKIP: CPU precomp channels passed; BLOOM_GPUSHADER_TOOLS_DIR is not set\n";
        return 77;
#else
        render::GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = render::GpuDevice::create(createOptions);
        if (!device) {
            if (options.require_device) {
                std::cerr << "FAIL: required device unavailable: " << device.diagnostic.message
                          << '\n';
                return 1;
            }
            std::cout << "SKIP: CPU precomp channels passed; no compatible Vulkan device available: "
                      << device.diagnostic.message << '\n';
            return 77;
        }
        auto cache =
            runtime::GpuSceneCache::create(*device.device, runtime::GpuSceneCacheBudgets{});
        expectations.expect(cache.hasValue(), "the scene cache creates");
        if (!cache)
            return 1;
        auto executor = runtime::GpuSceneExecutor::create(*device.device, *cache.cache);
        expectations.expect(executor.hasValue(), "the scene executor creates");
        if (!executor)
            return 1;
        for (const auto& [label, folder, reference] : references)
            mediaParity(expectations, *executor.executor, folder, reference, label);
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " precomp channel expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: precomp channel layers match on the GPU and the CPU reference\n";
        return 0;
#endif
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
