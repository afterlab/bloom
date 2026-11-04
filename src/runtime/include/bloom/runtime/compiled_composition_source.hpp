#pragma once

// A nested composition reference and its time mapping. Split out of compiled_plan.hpp so a
// channel source (channel_expression.hpp) can carry the Composition Source mappings it is reached
// through (#480) without including the whole plan.

#include <bloom/document/ids.hpp>
#include <bloom/runtime/compiled_curves.hpp>
#include <bloom/runtime/compiled_value_graph.hpp>

#include <cstddef>
#include <cstdint>
#include <variant>

namespace bloom::runtime {

// Each of the three typed operands gained ONE more alternative in task S7: a value-graph output,
// for a parameter a driver binding supplies per frame (see compiled_plan.hpp).
struct CompiledScalarParameter final {
    document::ParameterId id;
    std::variant<double, ScalarCurveIndex, ValueOutputIndex> source;

    friend bool operator==(const CompiledScalarParameter&,
                           const CompiledScalarParameter&) = default;
};

struct CompiledCompositionTimeMapping final {
    CompiledScalarParameter offset;
    CompiledScalarParameter scale;
    std::int64_t loopMode = 0;
    friend bool operator==(const CompiledCompositionTimeMapping&,
                           const CompiledCompositionTimeMapping&) = default;
};

// `nestedPlanIndex` indexes the nestedPlans() of the plan this reference belongs to; the mapping's
// operands index that plan's curves and value outputs.
struct CompiledCompositionSource final {
    document::NodeId sourceNodeId;
    std::size_t nestedPlanIndex = 0;
    CompiledCompositionTimeMapping timeMapping;
    friend bool operator==(const CompiledCompositionSource&,
                           const CompiledCompositionSource&) = default;
};

} // namespace bloom::runtime
