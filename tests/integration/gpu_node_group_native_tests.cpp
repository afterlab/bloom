// #342 Node Group on the production GPU route (docs/architecture/layer-graph-model.md, Group
// Definitions).
//
// A grouped composition (a translucent HDR plate through an Exposure and an Image Mix inside a
// group definition, with a second plate and the Mask a Luma Key cuts from it entering at the
// group's pixel slots, and the exposure read from a keyed Scalar value node through a value slot)
// and the flat graph of the same nodes are built into real documents through commands, compiled by
// the production SnapshotCompiler and rendered by the CPU reference. The group renders the flat
// graph's CPU pixels exactly; each is prepared by the production CpuGpuSceneBuilder, run on the
// production GpuSceneExecutor and held to the CPU reference at 2e-6, alpha exact, cold then warm,
// at three times, over a region of interest and at half resolution. The prepared scenes of the
// group and the flat graph hold the same commands and dispatch the same number of times. Editing
// an instance slot or the definition changes the pixels and is dispatched; an unrelated edit is
// not. Preparation and the native job cancel, and the budget refuses.
//
// A ChannelSet through a group (Image to Channel Set outside, Channel Pass inside at `channels1`,
// and the reverse) is held to the flat chain's CPU pixels and, on the device, to the CPU reference.
//
// The CPU assertions always run. Without a device the GPU half reports an explicit SKIP (exit 77)
// after the CPU half passed; --require-device makes that a failure.

#include "document_render_parity_support.hpp"
#include "node_group_document_support.hpp"

#include <bloom/commands/animation_operations.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

using namespace bloom;
using namespace bloom::test::document_render;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;
using bloom::test::node_group::buildChannelChain;
using bloom::test::node_group::buildTone;
using bloom::test::node_group::Fixture;
using bloom::test::node_group::ToneOptions;

// A CPU reference at a quarter second, optionally over a region of interest or at reduced size.
[[nodiscard]] std::optional<Reference>
referenceAt(Expectations& expectations, const Fixture& fixture, const int quarter,
            const std::string& label, const std::optional<render::ImageWindow>& roi = {},
            const std::optional<runtime::EvaluationResolution>& resolution = {}) {
    auto reference = cpuReference(expectations, fixture.document, fixture.main, label);
    if (!reference)
        return std::nullopt;
    reference->request.time = *core::RationalTime::create(quarter, 4);
    reference->request.roi = roi;
    if (resolution)
        reference->request.resolution = *resolution;
    auto oracle = reference->request;
    oracle.bypassOperationCache = true;
    const auto result = runtime::CpuCompositionEvaluator{}.evaluate(reference->plan, oracle, {});
    expectations.expect(result.frame() != nullptr, label + ": the CPU reference evaluates");
    if (result.frame() == nullptr)
        return std::nullopt;
    reference->frame = result.frame();
    return reference;
}

[[nodiscard]] bool samePixels(const Reference& a, const Reference& b) {
    const auto left = a.frame->processImage().pixels();
    const auto right = b.frame->processImage().pixels();
    return left.size_bytes() == right.size_bytes() &&
           std::memcmp(left.data(), right.data(), left.size_bytes()) == 0;
}

// Operations by kind, in order: the shape of a plan with the node ids taken out.
[[nodiscard]] std::vector<std::size_t> shapeOf(const runtime::CompiledCompositionPlan& plan) {
    std::vector<std::size_t> shape;
    for (const auto& operation : plan.operations()) {
        shape.push_back(operation.index());
        if (const auto* effect = std::get_if<runtime::CompiledImageEffect>(&operation))
            shape.push_back(effect->kernel.index());
    }
    return shape;
}

void keyStops(Fixture& fixture) {
    const auto driver = fixture.parts.driver;
    for (const auto& [quarter, value] : {std::pair{std::int64_t{0}, 0.2}, std::pair{std::int64_t{4}, 1.4}})
        fixture.run<commands::SetKeyframeAtTimeForParameter>(
            fixture.main, fixture.parameter(fixture.main, driver, document::kValueParameterRole),
            *core::RationalTime::create(quarter, 4), value);
}

// A genuinely cancelled token, taken from a scheduler task that is cancelled while it runs.
[[nodiscard]] runtime::CancellationToken cancelledToken() {
    using namespace runtime;
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
        TaskRequest("gpu node group cancel token",
                    {.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(97)}),
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
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return token;
}

#ifdef BLOOM_GPUSHADER_TOOLS_DIR
// The pinned shader tools prepare the image effects' working-space programs.
[[nodiscard]] runtime::GpuSceneOcioContext ocioContext() {
    runtime::GpuOcioCompileOptions options;
    options.glslangValidatorPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/glslangValidator";
    options.spirvValPath = std::string(BLOOM_GPUSHADER_TOOLS_DIR) + "/spirv-val";
    runtime::GpuSceneOcioContext context;
    context.preparer = std::make_shared<runtime::GpuOcioProgramPreparer>();
    context.compileOptions = options;
    return context;
}

// Dispatches one cold run of `scene` takes on a cache and executor of its own.
[[nodiscard]] std::optional<std::uint64_t>
coldDispatches(render::GpuDevice& device,
               const std::shared_ptr<const runtime::PreparedGpuScene>& scene) {
    auto cache = runtime::GpuSceneCache::create(device, runtime::GpuSceneCacheBudgets{});
    if (!cache)
        return std::nullopt;
    auto executor = runtime::GpuSceneExecutor::create(device, *cache.cache);
    if (!executor)
        return std::nullopt;
    if (runScene(*executor.executor, scene).empty())
        return std::nullopt;
    return executor.executor->counters().dispatches;
}

void cancellationAndBudget(Expectations& expectations, runtime::GpuSceneExecutor& executor,
                           const runtime::CpuGpuSceneBuilder& builder, const Reference& reference,
                           const std::string& label) {
    const auto cancelled = builder.build(reference.plan, reference.request, cancelledToken());
    expectations.expect(!cancelled.hasValue() &&
                            cancelled.diagnostic.code ==
                                runtime::PreparedGpuSceneDiagnosticCode::Cancelled,
                        label + ": a cancelled preparation reports Cancelled");
    // Nothing in this scene is a host raster, so the request's host pixel-storage limit has nothing
    // to refuse at preparation; the budget that applies is the executor's own, checked below.

    auto fresh = reference.request;
    fresh.time = core::RationalTime::fromInteger(3);
    const auto prepared = builder.build(reference.plan, fresh);
    expectations.expect(prepared.hasValue(), label + ": native cancel prepares");
    if (!prepared)
        return;
    const auto begun = executor.begin(prepared.scene, kSceneBudget);
    expectations.expect(begun.code == runtime::GpuSceneExecutorDiagnosticCode::None,
                        label + ": native cancel begins");
    executor.cancel();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (executor.state() == runtime::GpuSceneExecutorJobState::Pending &&
           std::chrono::steady_clock::now() < deadline) {
        (void)executor.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expectations.expect(executor.state() != runtime::GpuSceneExecutorJobState::Ready &&
                            executor.image() == nullptr,
                        label + ": no image is published after cancel");
    while (executor.hasUnretiredSubmission() && std::chrono::steady_clock::now() < deadline) {
        (void)executor.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto after = runScene(executor, prepared.scene);
    auto oracle = fresh;
    oracle.bypassOperationCache = true;
    const auto expected = runtime::CpuCompositionEvaluator{}.evaluate(reference.plan, oracle, {});
    expectations.expect(expected.frame() != nullptr &&
                            parity(after, expected.frame()->processImage().pixels()),
                        label + ": the same executor completes the scene afterwards");
    const auto refused = executor.begin(prepared.scene, 64);
    expectations.expect(refused.code != runtime::GpuSceneExecutorDiagnosticCode::None,
                        label + ": the executor refuses a scene over its byte budget");
    if (refused.code == runtime::GpuSceneExecutorDiagnosticCode::None) {
        auto settle = executor.poll();
        while (settle == runtime::GpuSceneExecutorPollResult::Pending)
            settle = executor.poll();
        static_cast<void>(executor.takeImage());
    }
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

        // 1. The grouped and the flat document, keyed exposure, at three times.
        auto grouped = buildTone({.grouped = true, .drivenStops = true});
        auto flat = buildTone({.grouped = false, .drivenStops = true});
        keyStops(*grouped);
        keyStops(*flat);
        struct Case final {
            std::string label;
            std::optional<Reference> group;
            std::optional<Reference> flat;
        };
        std::vector<Case> cases;
        const auto window = render::ImageWindow::create(20, 16, 40, 34);
        const auto halfExtent = render::ImageExtent::create(48, 40);
        const auto add = [&](std::string label, const int quarter,
                             const std::optional<render::ImageWindow>& roi = {},
                             const std::optional<runtime::EvaluationResolution>& resolution = {}) {
            Case entry{label, referenceAt(expectations, *grouped, quarter, label + " (group)", roi, resolution),
                       referenceAt(expectations, *flat, quarter, label + " (flat)", roi, resolution)};
            expectations.expect(entry.group && entry.flat &&
                                    shapeOf(*entry.group->plan) == shapeOf(*entry.flat->plan),
                                label + ": the group compiles to the flat graph's operations");
            expectations.expect(entry.group && entry.flat && samePixels(*entry.group, *entry.flat),
                                label + ": the group renders the flat graph's CPU pixels exactly");
            cases.push_back(std::move(entry));
        };
        add("frame 0", 0);
        add("frame 2", 2);
        add("frame 4", 4);
        add("region of interest", 2, window ? std::optional(*window.value()) : std::nullopt);
        add("half resolution", 2, std::nullopt,
            runtime::ProxyResolution{*halfExtent.value()});
        if (cases.size() >= 3 && cases[0].group && cases[1].group && cases[2].group)
            expectations.expect(!samePixels(*cases[0].group, *cases[1].group) &&
                                    !samePixels(*cases[1].group, *cases[2].group),
                                "the keyed exposure moves the picture between frames");
        // Alpha: the plate is translucent, so the frame carries partial coverage.
        if (!cases.empty() && cases[0].group) {
            const auto pixels = cases[0].group->frame->processImage().pixels();
            expectations.expect(std::ranges::any_of(pixels,
                                                    [](const render::Rgba32f& pixel) {
                                                        return pixel.alpha() > 0.0F &&
                                                               pixel.alpha() < 1.0F;
                                                    }),
                                "the fixture carries partial alpha");
        }
        if (cases.size() > 2 && cases[2].group) {
            const auto pixels = cases[2].group->frame->processImage().pixels();
            expectations.expect(std::ranges::any_of(pixels,
                                                    [](const render::Rgba32f& pixel) {
                                                        return pixel.red() > 1.0F;
                                                    }),
                                "the fixture carries a value above one at the brightest key");
        }

        // 2. Edits: one instance slot, then something unrelated, then the definition itself.
        grouped->set(grouped->main, grouped->parts.instance, "scalar2", 0.3);
        const auto slotEdit = referenceAt(expectations, *grouped, 2, "instance slot edited");
        const auto unrelatedNode = grouped->addNode(grouped->main, document::kScalarValueNodeType);
        grouped->set(grouped->main, unrelatedNode, document::kValueParameterRole, 42.0);
        const auto unrelated = referenceAt(expectations, *grouped, 2, "unrelated edit");
        grouped->set(grouped->parts.group, grouped->parts.mix, "factor", 0.45);
        const auto definitionEdit = referenceAt(expectations, *grouped, 2, "definition edited");
        flat->set(flat->main, flat->parts.mix, "factor", 0.45);
        const auto flatDefinitionEdit = referenceAt(expectations, *flat, 2, "definition edit (flat)");
        expectations.expect(slotEdit && cases.size() > 1 && cases[1].group &&
                                !samePixels(*slotEdit, *cases[1].group),
                            "editing an instance slot changes the CPU reference");
        expectations.expect(slotEdit && unrelated && samePixels(*slotEdit, *unrelated),
                            "an unrelated edit changes no pixel");
        expectations.expect(definitionEdit && unrelated && !samePixels(*definitionEdit, *unrelated),
                            "editing the definition changes the CPU reference");
        expectations.expect(definitionEdit && flatDefinitionEdit &&
                                samePixels(*definitionEdit, *flatDefinitionEdit),
                            "an edit inside the definition matches the flat edit");

        // 2b. A ChannelSet through a group (entering and leaving) renders the flat chain's pixels.
        auto flatChannels = buildChannelChain(false);
        auto enterChannels = buildChannelChain(true, false);
        auto leaveChannels = buildChannelChain(true, true);
        struct ChannelCase final {
            std::string label;
            std::optional<Reference> group;
            std::optional<Reference> flat;
        };
        std::vector<ChannelCase> channelCases;
        for (const auto& [label, fixture] :
             {std::pair<const char*, Fixture*>{"ChannelSet entering", enterChannels.get()},
              std::pair<const char*, Fixture*>{"ChannelSet leaving", leaveChannels.get()}}) {
            ChannelCase entry{label, referenceAt(expectations, *fixture, 0, std::string(label) + " (group)"),
                              referenceAt(expectations, *flatChannels, 0, std::string(label) + " (flat)")};
            expectations.expect(entry.group && entry.flat &&
                                    shapeOf(*entry.group->plan) == shapeOf(*entry.flat->plan),
                                entry.label + ": the group compiles to the flat chain's operations");
            expectations.expect(entry.group && entry.flat && samePixels(*entry.group, *entry.flat),
                                entry.label + ": the group renders the flat chain's CPU pixels exactly");
            if (entry.flat) {
                const auto pixels = entry.flat->frame->processImage().pixels();
                expectations.expect(std::ranges::any_of(pixels,
                                                        [](const render::Rgba32f& pixel) {
                                                            return pixel.red() > 0.0F &&
                                                                   pixel.alpha() > 0.0F;
                                                        }),
                                    entry.label + ": the Channel Pass reads the plate's colour back");
            }
            channelCases.push_back(std::move(entry));
        }

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " node group expectation(s) failed\n";
            return 1;
        }

#ifndef BLOOM_GPUSHADER_TOOLS_DIR
        std::cout << "NOTRUN: the grouped image effects need the pinned shader tools\n";
        if (options.require_device) {
            std::cerr << "FAIL: the node group cases need the pinned shader tools\n";
            return 1;
        }
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
            std::cout << "SKIP: CPU half passed; no compatible Vulkan device available: "
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
        const runtime::CpuGpuSceneBuilder builder(nullptr, {}, ocioContext());
        auto& run = *executor.executor;

        // 3. Parity of the group and of the flat graph, cold then warm, for every case.
        for (const auto& entry : cases) {
            if (entry.group)
                gpuParity(expectations, run, *entry.group, entry.label + " (group)", builder);
            if (entry.flat)
                gpuParity(expectations, run, *entry.flat, entry.label + " (flat)", builder);
        }

        // 3b. The ChannelSet chain through channels1, held to the CPU reference on the device.
        for (const auto& entry : channelCases) {
            if (entry.group)
                gpuParity(expectations, run, *entry.group, entry.label + " (group)", builder);
            if (entry.flat)
                gpuParity(expectations, run, *entry.flat, entry.label + " (flat)", builder);
            if (entry.group && entry.flat) {
                const auto groupScene = builder.build(entry.group->plan, entry.group->request);
                const auto flatScene = builder.build(entry.flat->plan, entry.flat->request);
                expectations.expect(groupScene.hasValue() && flatScene.hasValue() &&
                                        groupScene.scene->commands().size() ==
                                            flatScene.scene->commands().size(),
                                    entry.label + ": the group's prepared scene holds the flat "
                                                  "chain's number of commands");
            }
        }

        // 4. The prepared scenes hold the same commands and dispatch the same number of times.
        for (const auto& entry : cases) {
            if (!entry.group || !entry.flat)
                continue;
            const auto groupScene = builder.build(entry.group->plan, entry.group->request);
            const auto flatScene = builder.build(entry.flat->plan, entry.flat->request);
            expectations.expect(groupScene.hasValue() && flatScene.hasValue(),
                                entry.label + ": both scenes prepare");
            if (!groupScene || !flatScene)
                continue;
            expectations.expect(groupScene.scene->commands().size() ==
                                    flatScene.scene->commands().size(),
                                entry.label + ": the group's prepared scene holds the flat "
                                              "graph's number of commands");
            const auto groupDispatches = coldDispatches(*device.device, groupScene.scene);
            const auto flatDispatches = coldDispatches(*device.device, flatScene.scene);
            expectations.expect(groupDispatches && flatDispatches &&
                                    *groupDispatches == *flatDispatches && *groupDispatches > 0,
                                entry.label + ": the group dispatches as often as the flat graph "
                                              "on a cold cache");
        }

        // 5. Cache identity: the slot edit is a new scene and is dispatched; an unrelated edit is
        // the same scene and dispatches nothing; a definition edit is dispatched and matches.
        if (slotEdit && unrelated && definitionEdit) {
            const auto edited = builder.build(slotEdit->plan, slotEdit->request);
            expectations.expect(edited.hasValue(), "the slot-edited state prepares");
            if (edited) {
                const auto before = run.counters();
                const auto pixels = runScene(run, edited.scene);
                expectations.expect(parity(pixels, slotEdit->frame->processImage().pixels()) &&
                                        run.counters().dispatches > before.dispatches,
                                    "the slot-edited frame matches and is dispatched, not served stale");
            }
            const auto same = builder.build(unrelated->plan, unrelated->request);
            expectations.expect(same.hasValue(), "the unrelated-edit state prepares");
            if (same) {
                const auto before = run.counters();
                const auto pixels = runScene(run, same.scene);
                expectations.expect(parity(pixels, unrelated->frame->processImage().pixels()) &&
                                        run.counters().dispatches == before.dispatches,
                                    "an unrelated edit matches and dispatches nothing new");
            }
            const auto redefined = builder.build(definitionEdit->plan, definitionEdit->request);
            expectations.expect(redefined.hasValue(), "the definition-edited state prepares");
            if (redefined) {
                const auto before = run.counters();
                const auto pixels = runScene(run, redefined.scene);
                expectations.expect(parity(pixels, definitionEdit->frame->processImage().pixels()) &&
                                        run.counters().dispatches > before.dispatches,
                                    "the definition-edited frame matches and is dispatched");
            }
        }

        // 6. Cancellation and budget on the grouped scene.
        if (!cases.empty() && cases[0].group)
            cancellationAndBudget(expectations, run, builder, *cases[0].group, "node group");

        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " node group expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: a Node Group with an image, a second image, a Mask and a keyed value "
                     "slot (and a ChannelSet entering and leaving at channels1) matches the flat graph and the CPU reference on the device, cold and "
                     "warm, at three frames, over a region of interest and at half resolution, "
                     "with the same prepared commands and dispatches, cache identity and "
                     "invalidation, cancellation and budget\n";
        return 0;
#endif
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
