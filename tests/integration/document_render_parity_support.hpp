#pragma once

// Shared support for native tests that build a real document through commands, compile it with
// the production SnapshotCompiler, and compare the production GPU scene (CpuGpuSceneBuilder ->
// GpuSceneExecutor -> one readback) against the unchanged CpuCompositionEvaluator oracle under
// the documented 2e-6 gate.

#include "gpu_composite_native_support.hpp"

#include <bloom/commands/command_stack.hpp>
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
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::test::document_render {

using render::composite_proof::Expectations;

inline constexpr std::uint64_t kSceneBudget = 256ULL * 1024ULL * 1024ULL;

template <typename Operation, typename... Arguments>
void execute(document::Document& document, commands::CommandStack& commands,
             Arguments&&... arguments) {
    commands::Transaction transaction("Fixture", document.snapshot().revision());
    transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
    if (!commands.execute(std::move(transaction)).changed())
        throw std::runtime_error("document render fixture command refused");
}

[[nodiscard]] inline const document::Composition& composition(const document::Document& document,
                                                              const document::CompositionId id) {
    const auto* found = document.snapshot().project().findComposition(id);
    if (found == nullptr)
        throw std::runtime_error("fixture composition is missing");
    return *found;
}

// The last node of a type in a composition (fixtures add one node at a time).
[[nodiscard]] inline document::NodeId lastNode(const document::Document& document,
                                               const document::CompositionId id,
                                               const std::string_view type) {
    const auto nodes = composition(document, id).graph().nodes();
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
        if (it->typeId == type)
            return it->id;
    throw std::runtime_error("fixture node type is missing");
}

[[nodiscard]] inline document::ParameterId parameter(const document::Document& document,
                                                     const document::CompositionId id,
                                                     const document::NodeId node,
                                                     const std::string_view role) {
    const auto* record = composition(document, id).graph().findNode(node);
    if (record != nullptr)
        for (const auto& binding : record->parameters)
            if (binding.role == role)
                return binding.parameterId;
    throw std::runtime_error("fixture parameter is missing");
}

[[nodiscard]] inline document::LayerId lastLayer(const document::Document& document,
                                                 const document::CompositionId id) {
    return composition(document, id).graph().layerOutputs().back().layerId;
}

[[nodiscard]] inline document::NodeId layerNode(const document::Document& document,
                                                const document::CompositionId id,
                                                const document::LayerId layer) {
    for (const auto& boundary : composition(document, id).graph().layerOutputs())
        if (boundary.layerId == layer)
            return boundary.nodeId;
    throw std::runtime_error("fixture layer is missing");
}

[[nodiscard]] inline bool parity(const std::vector<render::Rgba32f>& actual,
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

[[nodiscard]] inline runtime::EvaluationRequest
requestFor(const runtime::CompiledCompositionPlan& plan) {
    return runtime::EvaluationRequest{.time = core::RationalTime{},
                                      .output = plan.output(),
                                      .resolution = runtime::CompositionFormatResolution{},
                                      .quality = runtime::EvaluationQuality::Reference,
                                      .colorIntent =
                                          runtime::EvaluationColorIntent::LinearRec709Scene,
                                      .pixelStorageByteLimit = kSceneBudget};
}

// One compiled state and its CPU reference frame.
struct Reference final {
    std::shared_ptr<const runtime::CompiledCompositionPlan> plan;
    runtime::EvaluationRequest request;
    std::shared_ptr<const runtime::ProcessFrame> frame;
};

[[nodiscard]] inline std::optional<Reference> cpuReference(Expectations& expectations,
                                                           const document::Document& document,
                                                           const document::CompositionId id,
                                                           const std::string& label) {
    const runtime::SnapshotCompiler compiler(document::builtInNodeDefinitions());
    const auto compiled = compiler.compile({document.snapshot(), id}, {});
    expectations.expect(
        compiled.status == runtime::SnapshotCompileStatus::Compiled && compiled.plan,
        label + ": compiles: " +
            runtime::describeCompileFailure(compiled, document.snapshot().project(), id));
    if (!compiled.plan)
        return std::nullopt;
    auto request = requestFor(*compiled.plan);
    auto oracleRequest = request;
    oracleRequest.bypassOperationCache = true;
    const auto result =
        runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, oracleRequest, {});
    expectations.expect(result.frame() != nullptr, label + ": the CPU reference evaluates");
    if (result.frame() == nullptr)
        return std::nullopt;
    return Reference{compiled.plan, request, result.frame()};
}

[[nodiscard]] inline std::vector<render::Rgba32f>
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

// Prepares the production GPU scene for a CPU reference, runs it cold and warm, and requires
// pixel parity and the same prepared bounds as the CPU evaluator.
inline void gpuParity(Expectations& expectations, runtime::GpuSceneExecutor& executor,
                      const Reference& reference, const std::string& label) {
    const auto prepared = runtime::CpuGpuSceneBuilder{}.build(reference.plan, reference.request);
    expectations.expect(prepared.hasValue(), label + ": prepares: " + prepared.diagnostic.message);
    if (!prepared)
        return;
    const auto& cpuBounds = reference.frame->evaluatedBounds();
    const auto& gpuBounds = prepared.scene->bounds();
    bool boundsMatch = cpuBounds.size() == gpuBounds.size();
    for (std::size_t index = 0; boundsMatch && index < cpuBounds.size(); ++index)
        boundsMatch = cpuBounds[index].local == gpuBounds[index].local &&
                      cpuBounds[index].output == gpuBounds[index].output;
    expectations.expect(boundsMatch, label + ": GPU preparation publishes the CPU bounds");
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

// Alpha of the CPU reference at a composition pixel (full resolution, origin at the frame).
[[nodiscard]] inline float alphaAt(const Reference& reference, const std::int64_t x,
                                   const std::int64_t y) {
    const auto& image = reference.frame->processImage();
    const auto window = image.descriptor()->dataWindow();
    if (x < window.originX() || y < window.originY() || x >= window.maxXExclusive() ||
        y >= window.maxYExclusive())
        return 0.0F;
    const auto width = static_cast<std::int64_t>(window.extent().width());
    return image
        .pixels()[static_cast<std::size_t>((y - window.originY()) * width + (x - window.originX()))]
        .alpha();
}

// The tight rectangle of pixels with nonzero alpha in the CPU reference, as
// {left, top, rightExclusive, bottomExclusive}; empty when nothing is covered.
struct PixelRect final {
    std::int64_t left = 0, top = 0, right = 0, bottom = 0;
    [[nodiscard]] bool empty() const noexcept { return right <= left || bottom <= top; }
    friend bool operator==(const PixelRect&, const PixelRect&) = default;
};

[[nodiscard]] inline PixelRect coverage(const Reference& reference) {
    const auto& image = reference.frame->processImage();
    const auto window = image.descriptor()->dataWindow();
    const auto width = static_cast<std::int64_t>(window.extent().width());
    const auto height = static_cast<std::int64_t>(window.extent().height());
    constexpr auto most = std::numeric_limits<std::int64_t>::max();
    constexpr auto least = std::numeric_limits<std::int64_t>::min();
    PixelRect rect{most, most, least, least};
    for (std::int64_t y = 0; y < height; ++y)
        for (std::int64_t x = 0; x < width; ++x)
            if (image.pixels()[static_cast<std::size_t>(y * width + x)].alpha() > 0.0F) {
                rect.left = std::min(rect.left, x + window.originX());
                rect.top = std::min(rect.top, y + window.originY());
                rect.right = std::max(rect.right, x + window.originX() + 1);
                rect.bottom = std::max(rect.bottom, y + window.originY() + 1);
            }
    return rect.right < rect.left ? PixelRect{} : rect;
}

} // namespace bloom::test::document_render
