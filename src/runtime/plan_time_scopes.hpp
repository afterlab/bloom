#pragma once

// Private to src/runtime. ADR 0024 layer-owned time: assigns every compiled image operation to a
// time clock and copies an operation once per clock that reaches it.

#include <bloom/runtime/compiled_plan.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace bloom::runtime::detail {

// Bounds on the clock table and on the operation list after copying. A plan exceeding either is a
// compile diagnostic (bloom.runtime.compile.time-scope-limit), never a silent fallback.
inline constexpr std::size_t kMaximumTimeClocks = 4096;
inline constexpr std::size_t kMaximumScopedOperations = 65536;

struct TimeScopeFailure final {
    std::string summary;
};

// Rewrites `definition` in place. A plan with no reachable retimed Layer Output is left exactly as
// it was (no clocks, same operation list), so identity-timed documents compile unchanged.
//
// A retimed Layer Output placed on clock p creates a Layer clock L (parent p) and a Content clock C
// (parent L). Its own operands evaluate on L, its image input (and everything upstream) on C, and
// its transform parent on p. Every other operation evaluates on the clock its consumer needs. An
// operation needed on several clocks appears once per clock, in the original topological order.
[[nodiscard]] std::optional<TimeScopeFailure>
scopeLayerTime(CompiledCompositionPlanDefinition& definition);

// #480: run after scopeLayerTime. Each Image to Channel Set layer of the Composition Output's
// channel layers gets its own plan (the definition re-rooted at its image operation through a
// Merge, as the output places any image) in CompiledChannelSource::imagePlan, and its operation
// index is cleared. Operations nothing then reaches from the output (or a bounds readout) are
// dropped -- such as an image read only by an Image to Channel Set layer, or by a Shuffle leaf
// that names its channels statically (its reference is cleared). A plan in which every operation
// is reached is left exactly as it was.
void separateChannelLayerImages(CompiledCompositionPlanDefinition& definition);

// #484: every image input of `operation` (inputs, secondary and mask inputs, field images, layer
// inputs and parents, merge entries, output layers), read or replaced through `function`.
void forEachOperationInput(const CompiledOperation& operation,
                           const std::function<void(OperationIndex)>& function);
void remapOperationInputs(CompiledOperation& operation,
                          const std::function<OperationIndex(OperationIndex)>& function);

} // namespace bloom::runtime::detail
