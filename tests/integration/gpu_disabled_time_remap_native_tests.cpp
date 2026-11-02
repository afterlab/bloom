// #431: real native proof that a disabled Time Remapped nested-composition layer (which also routes
// its composition's audio) compiles and is excluded from the GPU render exactly as from the CPU
// reference, and that enabling it again brings it back.
//
// A real document is edited through commands and compiled by the production SnapshotCompiler at
// each state (enabled, disabled, enabled again). The production CpuGpuSceneBuilder prepares each
// plan, the production GpuSceneExecutor runs it on a real device, and the readback must match the
// unchanged CpuCompositionEvaluator oracle under the documented 2e-6 gate; a warm rerun of the
// same scene dispatches nothing new.
//
// Without a device the test reports an explicit SKIP (exit 77); --require-device makes that a
// failure. An explicit --loader selects the Vulkan loader.

#include "gpu_composite_native_support.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/layer_time_operations.hpp>
#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/render/gpu_image.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using render::composite_proof::Expectations;
using render::composite_proof::Options;
using render::composite_proof::parseOptions;

constexpr std::uint64_t kSceneBudget = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kWidth = 16;
constexpr std::size_t kHeight = 8;

document::FrameRate rate() { return *document::FrameRate::create(24, 1); }
document::CompositionFormat format() {
    return *document::CompositionFormat::create(kWidth, kHeight, core::PixelAspectRatio::square(),
                                                rate());
}

template <typename Operation, typename... Arguments>
void execute(document::Document& document, commands::CommandStack& commands,
             Arguments&&... arguments) {
    commands::Transaction transaction("Disabled remap", document.snapshot().revision());
    transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
    if (!commands.execute(std::move(transaction)).changed())
        throw std::runtime_error("disabled remap fixture command refused");
}

// A nested-composition layer that fills the frame with red and carries an identity Time Remap,
// under a smaller blue Solid that covers the top-left of the frame but not its last pixel.
struct Fixture {
    document::NewProject initial = document::makeNewProject(
        "Disabled remap", "Main", core::RationalTime::fromInteger(4), format());
    document::CompositionId main = initial.initialCompositionId;
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    document::LayerId nested;

    Fixture() {
        execute<commands::AddComposition>(document, commands, "Inner", format(),
                                          core::RationalTime::fromInteger(4));
        const auto inner = document.snapshot().project().compositions().back().id();
        execute<commands::AddSolidLayer>(document, commands, inner, "Red",
                                         core::Color4d{0.9, 0.1, 0.1, 1.0});
        execute<commands::AddCompositionSource>(document, commands, main, inner, document::Vec2d{},
                                                true);
        nested = document.snapshot()
                     .project()
                     .findComposition(main)
                     ->graph()
                     .layerOutputs()
                     .back()
                     .layerId;
        execute<commands::EnableLayerTimeRemap>(document, commands, main, nested);
        // A smaller blue Solid on top, which every state shows.
        execute<commands::AddSolidLayer>(document, commands, main, "Blue",
                                         core::Color4d{0.1, 0.2, 0.9, 1.0},
                                         document::Vec2d{4.0, 2.0});
    }
};

[[nodiscard]] runtime::EvaluationRequest requestFor(const runtime::CompiledCompositionPlan& plan) {
    return runtime::EvaluationRequest{.time = *core::RationalTime::create(1, 2),
                                      .output = plan.output(),
                                      .resolution = runtime::CompositionFormatResolution{},
                                      .quality = runtime::EvaluationQuality::Reference,
                                      .colorIntent =
                                          runtime::EvaluationColorIntent::LinearRec709Scene,
                                      .pixelStorageByteLimit = kSceneBudget};
}

[[nodiscard]] bool parity(const std::vector<render::Rgba32f>& actual,
                          const std::span<const render::Rgba32f> expected) {
    if (actual.size() != expected.size())
        return false;
    for (std::size_t i = 0; i < actual.size(); ++i)
        for (std::size_t c = 0; c < 4; ++c) {
            const float a = actual[i].components()[c];
            const float e = expected[i].components()[c];
            if (!std::isfinite(a) || !std::isfinite(e))
                return false;
            const float scale = std::max(std::abs(a), std::abs(e));
            if (std::abs(a - e) > 2e-6F * std::max(1.0F, scale))
                return false;
        }
    return true;
}

[[nodiscard]] std::vector<render::Rgba32f>
runScene(runtime::GpuSceneExecutor& executor,
         const std::shared_ptr<const runtime::PreparedGpuScene>& scene) {
    if (executor.begin(scene, kSceneBudget).code != runtime::GpuSceneExecutorDiagnosticCode::None)
        return {};
    auto poll = executor.poll();
    while (poll == runtime::GpuSceneExecutorPollResult::Pending)
        poll = executor.poll();
    std::vector<render::Rgba32f> pixels;
    if (poll == runtime::GpuSceneExecutorPollResult::Ready && executor.image() != nullptr)
        if (const auto readback = render::readbackResidentImage(*executor.image(), kSceneBudget))
            pixels = readback.pixels;
    static_cast<void>(executor.takeImage());
    return pixels;
}

// Compiles the current state, renders it on the CPU oracle and the GPU, and returns the oracle's
// last pixel (the nested layer's red when it shows) and its first (the blue Solid).
struct State final {
    render::Rgba32f corner;
    render::Rgba32f solid;
};

[[nodiscard]] std::optional<State> renderState(Expectations& expectations, Fixture& fixture,
                                               runtime::GpuSceneExecutor& executor,
                                               const std::string& label, const bool firstRun) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({fixture.document.snapshot(), fixture.main}, {});
    expectations.expect(
        compiled.status == runtime::SnapshotCompileStatus::Compiled && compiled.plan,
        label + ": compiles: " +
            runtime::describeCompileFailure(compiled, fixture.document.snapshot().project(),
                                            fixture.main));
    if (!compiled.plan)
        return std::nullopt;
    const auto request = requestFor(*compiled.plan);
    const auto prepared = runtime::CpuGpuSceneBuilder{}.build(compiled.plan, request);
    expectations.expect(prepared.hasValue(), label + ": prepares: " + prepared.diagnostic.message);
    if (!prepared)
        return std::nullopt;
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto frame =
        runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, oracleRequest, {});
    expectations.expect(frame.frame() != nullptr, label + ": the CPU oracle evaluates");
    if (frame.frame() == nullptr)
        return std::nullopt;
    const auto expected = frame.frame()->processImage().pixels();
    const auto before = executor.counters();
    const auto cold = runScene(executor, prepared.scene);
    // Later states may be served partly from the scene cache; the first one must dispatch.
    expectations.expect(!cold.empty() &&
                            (!firstRun || executor.counters().dispatches > before.dispatches),
                        label + ": the native run completes: " + executor.diagnostic().message);
    expectations.expect(parity(cold, expected), label + ": GPU pixels match the CPU oracle (2e-6)");
    const auto warmBefore = executor.counters();
    const auto warm = runScene(executor, prepared.scene);
    expectations.expect(parity(warm, expected) &&
                            executor.counters().dispatches == warmBefore.dispatches,
                        label + ": a warm rerun dispatches nothing new");
    return State{expected.back(), expected.front()};
}

[[nodiscard]] bool red(const render::Rgba32f pixel) {
    const auto rgba = pixel.components();
    return rgba[3] > 0.5F && rgba[0] > 0.5F && rgba[2] < 0.5F;
}

[[nodiscard]] bool blue(const render::Rgba32f pixel) {
    const auto rgba = pixel.components();
    return rgba[3] > 0.5F && rgba[2] > 0.5F && rgba[0] < 0.5F;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (!options.valid) {
            std::cerr << "invalid arguments\n";
            return 2;
        }
        render::GpuDeviceCreationOptions createOptions;
        createOptions.loader_path = options.loader_path;
        auto device = render::GpuDevice::create(createOptions);
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
        Expectations expectations;
        auto cache =
            runtime::GpuSceneCache::create(*device.device, runtime::GpuSceneCacheBudgets{});
        expectations.expect(cache.hasValue(), "the scene cache creates");
        if (!cache)
            return 1;
        auto executor = runtime::GpuSceneExecutor::create(*device.device, *cache.cache);
        expectations.expect(executor.hasValue(), "the scene executor creates");
        if (!executor)
            return 1;

        Fixture fixture;
        const auto shown = renderState(expectations, fixture, *executor.executor, "enabled", true);
        expectations.expect(shown && red(shown->corner) && blue(shown->solid),
                            "the remapped nested layer shows under the blue Solid");
        execute<commands::SetLayerEnabled>(fixture.document, fixture.commands, fixture.main,
                                           fixture.nested, false);
        const auto hidden =
            renderState(expectations, fixture, *executor.executor, "disabled", false);
        expectations.expect(hidden && hidden->corner.components()[3] == 0.0F && blue(hidden->solid),
                            "the disabled layer is excluded and the blue Solid still renders");
        execute<commands::SetLayerEnabled>(fixture.document, fixture.commands, fixture.main,
                                           fixture.nested, true);
        const auto again =
            renderState(expectations, fixture, *executor.executor, "re-enabled", false);
        expectations.expect(again && red(again->corner) && blue(again->solid),
                            "the re-enabled layer shows again");
        if (expectations.failures() != 0) {
            std::cerr << expectations.failures() << " disabled remap expectation(s) failed\n";
            return 1;
        }
        std::cout << "PASS: a disabled Time Remapped nested layer compiles and is excluded on the "
                     "GPU and the CPU reference alike\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
