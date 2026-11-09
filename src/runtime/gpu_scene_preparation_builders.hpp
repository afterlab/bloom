#ifndef BLOOM_RUNTIME_GPU_SCENE_PREPARATION_BUILDERS_HPP
#define BLOOM_RUNTIME_GPU_SCENE_PREPARATION_BUILDERS_HPP

// Private to src/runtime. Small, coherent orchestration helpers shared by the scene builder
// translation unit: the prepared-subset classification and the reachable-operation pre-pass. Both
// live here so cpu_gpu_scene_preparation.cpp stays within the source-size budget without moving any
// per-leaf emission semantics out of their owning helpers.

#include "gpu_scene_nested.hpp"
#include "gpu_scene_preparation_common.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace bloom::runtime::detail {

// The reachable-operation subset the builder prepares. A new CompiledOperation alternative is not
// admitted until its preparation exists, so an unsupported graph fails closed instead of being
// silently approximated.
[[nodiscard]] inline bool isGpuSceneSubsetOperation(const CompiledOperation& operation) noexcept {
    return std::holds_alternative<CompiledSolid>(operation) ||
           std::holds_alternative<CompiledChecker>(operation) ||
           std::holds_alternative<CompiledText>(operation) ||
           std::holds_alternative<CompiledShape>(operation) ||
           std::holds_alternative<CompiledImageSource>(operation) ||
           std::holds_alternative<CompiledVideoSource>(operation) ||
           std::holds_alternative<CompiledImageEffect>(operation) ||
           std::holds_alternative<CompiledLayerOutput>(operation) ||
           std::holds_alternative<CompiledMerge>(operation) ||
           std::holds_alternative<CompiledCompositionOutput>(operation) ||
           std::holds_alternative<CompiledCompositionSource>(operation) ||
           std::holds_alternative<CompiledChannelShuffle>(operation) ||
           std::holds_alternative<CompiledCryptomatte>(operation);
}

// Computes the reachable operation mask from the terminal output, following pixel inputs and layer
// parents, and validates every reachable operation against the prepared subset. A composition
// source is admitted only when its real nested chain is present, acyclic, within the depth ceiling,
// and inside the bounded scan; the check is cancellation-aware. A failure carries the diagnostic
// the builder should publish.
[[nodiscard]] inline std::optional<GpuSceneLeafFailure> computeReachablePreparedOperations(
    const CompiledCompositionPlan& plan, const std::size_t outputIndex,
    const CancellationToken& cancellation, std::vector<bool>& reachable) {
    const std::size_t operationCount = plan.operations().size();
    reachable.assign(operationCount, false);
    std::vector<std::size_t> pending{outputIndex};
    while (!pending.empty()) {
        if (cancellation.isCancellationRequested()) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
        }
        const auto index = pending.back();
        pending.pop_back();
        if (index >= operationCount) {
            return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                        "Plan references an invalid operation");
        }
        if (reachable[index]) {
            continue;
        }
        reachable[index] = true;
        forEachInput(plan.operations()[index],
                     [&pending](const OperationIndex input) { pending.push_back(input.value()); });
        // A parent is not a pixel input, but the child's composed matrix needs the parent's matrix,
        // so the parent subtree is part of what this build must resolve. The preflight still
        // rejects a parent that is not an earlier Layer Output.
        if (const auto* layer = std::get_if<CompiledLayerOutput>(&plan.operations()[index]);
            layer && layer->parent && layer->parent->value() < operationCount) {
            pending.push_back(layer->parent->value());
        }
    }

    // One classifier is shared across every source in the plan, so a child plan several sources
    // reference is walked once.
    NestedCompositionChainClassifier nestedClassifier;
    for (std::size_t index = 0; index < operationCount; ++index) {
        if (!reachable[index]) {
            continue;
        }
        const auto& operation = plan.operations()[index];
        if (!isGpuSceneSubsetOperation(operation)) {
            return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                        "A reachable operation is outside the prepared subset");
        }
        // A composition source is inside the prepared subset only when it names a present,
        // compatible child chain. A source that does not is refused exactly as it was before nested
        // compositions were prepared at all, before any resolution or child build.
        if (const auto* source = std::get_if<CompiledCompositionSource>(&operation)) {
            switch (nestedClassifier.classify(*source, plan, cancellation)) {
            case NestedCompositionClassification::Supported:
                break;
            case NestedCompositionClassification::Cancelled:
                return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
            case NestedCompositionClassification::Unsupported:
                return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                            "A composition source without a supported nested plan is outside the "
                            "prepared subset");
            }
        }
    }
    return std::nullopt;
}

// The most image bytes the executor holds at once for this plan, each image taken as one frame (a
// decoded source as what it decodes to), walked in the executor's own order. This is what the phase
// claims on the device before it knows any window. The CPU reference evaluates every entry of a
// Merge before the Merge and so holds them all; the executor plans each foreground, composites it
// and releases it before the next (GpuSceneExecutor::Impl::planCommand), so a stack holds its
// accumulator, the replacement it is writing and one foreground's subtree, however many entries it
// has. A vector layer's fill, stroke and their composite are held only while that layer runs.
// A layer's parent lends its matrix, never its image. This is the phase's admission floor: an
// image larger than the frame (a layer's unclipped support, a gradient drawn in its own window) is
// drawn against live device capacity when the executor allocates it.
[[nodiscard]] inline std::uint64_t
gpuExecutorLivePeakBytes(const CompiledCompositionPlan& plan, const std::size_t output,
                         const std::uint64_t frameBytes) {
    const auto& operations = plan.operations();
    const auto isVectorChain = [&operations](std::size_t index) {
        while (index < operations.size()) {
            if (std::holds_alternative<CompiledShape>(operations[index]) ||
                std::holds_alternative<CompiledText>(operations[index]))
                return true;
            const auto* layer = std::get_if<CompiledLayerOutput>(&operations[index]);
            if (layer == nullptr || layer->input.value() >= index)
                return false;
            index = layer->input.value();
        }
        return false;
    };
    const auto pixelInputs = [&](const std::size_t index, const auto& visit) {
        if (const auto* layer = std::get_if<CompiledLayerOutput>(&operations[index])) {
            if (!isVectorChain(layer->input.value()))
                visit(layer->input.value());
            return;
        }
        forEachInput(operations[index],
                     [&visit](const OperationIndex input) { visit(input.value()); });
    };
    // What one operation's result holds while a later operation still reads it.
    const auto heldBytes = [&](const std::size_t index) -> std::uint64_t {
        const auto& operation = operations[index];
        if (std::holds_alternative<CompiledShape>(operation) ||
            std::holds_alternative<CompiledText>(operation))
            return 0;
        const document::AssetRecord* asset = nullptr;
        if (const auto* image = std::get_if<CompiledImageSource>(&operation))
            asset = image->asset ? &*image->asset : nullptr;
        else if (const auto* video = std::get_if<CompiledVideoSource>(&operation))
            asset = video->asset ? &*video->asset : nullptr;
        return asset != nullptr ? std::max(frameBytes, static_cast<std::uint64_t>(asset->width) *
                                                           asset->height * sizeof(render::Rgba32f))
                                : frameBytes;
    };
    // What one operation holds only while it runs, beside its result.
    const auto scratchBytes = [&](const std::size_t index) -> std::uint64_t {
        const auto& operation = operations[index];
        if (const auto* layer = std::get_if<CompiledLayerOutput>(&operation))
            return isVectorChain(layer->input.value()) ? 3 * frameBytes : 0;
        return std::holds_alternative<CompiledImageSource>(operation) ||
                       std::holds_alternative<CompiledVideoSource>(operation)
                   ? heldBytes(index)
                   : 0;
    };

    std::vector<std::uint32_t> readers(operations.size(), 0);
    std::vector<bool> counted(operations.size(), false);
    std::vector<std::size_t> pending{output};
    while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        if (index >= operations.size() || counted[index])
            continue;
        counted[index] = true;
        pixelInputs(index, [&](const std::size_t input) {
            if (input < operations.size()) {
                ++readers[input];
                pending.push_back(input);
            }
        });
    }

    std::vector<bool> planned(operations.size(), false);
    std::uint64_t alive = 0;
    std::uint64_t peak = 0;
    const auto release = [&](const std::size_t input) {
        if (input < operations.size() && --readers[input] == 0)
            alive -= heldBytes(input);
    };
    const std::function<void(std::size_t)> plan_ = [&](const std::size_t index) {
        if (index >= operations.size() || planned[index])
            return;
        planned[index] = true;
        if (const auto* merge = std::get_if<CompiledMerge>(&operations[index])) {
            bool accumulating = false;
            for (auto entry = merge->entries.rbegin(); entry != merge->entries.rend(); ++entry) {
                plan_(entry->input.value());
                if (!accumulating) {
                    alive += frameBytes;
                    accumulating = true;
                }
                peak = std::max(peak, alive + frameBytes);
                release(entry->input.value());
            }
            if (!accumulating)
                alive += frameBytes;
            peak = std::max(peak, alive);
            return;
        }
        pixelInputs(index, plan_);
        const auto held = heldBytes(index);
        peak = std::max(peak, alive + held + scratchBytes(index));
        alive += held;
        pixelInputs(index, release);
    };
    plan_(output);
    return std::max(peak, frameBytes);
}

} // namespace bloom::runtime::detail

#endif // BLOOM_RUNTIME_GPU_SCENE_PREPARATION_BUILDERS_HPP
