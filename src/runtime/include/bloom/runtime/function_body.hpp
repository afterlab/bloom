#pragma once

// #590: a group definition evaluated as a function. The compiler cuts the part of a group that
// computes its `scalar1` answer out of the value graph and keeps it as a small program of its own
// (CompiledFunctionBody, compiled_value_graph.hpp). A consumer then runs that program for one
// argument (Function) or for many (Function Plot), so the one definition drives a curve, a dot on
// it and a tangent, and an edit to it reaches all of them.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace bloom::runtime {

// The most operations a function may hold. A function runs once per sample, so its size multiplies
// the cost of a plot; a group larger than this is refused when the plan is compiled.
inline constexpr std::size_t kMaximumFunctionOperations = 512;

// Whether any of `operations` can change with the time of the frame. `outputCount` is the size of
// their own output table.
[[nodiscard]] bool valueOperationsTimeDependent(std::span<const CompiledValueOperation> operations,
                                                std::size_t outputCount);

// Runs one body for any number of arguments. It keeps its own copy of the operations (the argument
// is written into one operand) and nothing else, so one evaluator serves a whole plot.
class FunctionEvaluator final {
  public:
    FunctionEvaluator(const CompiledFunctionBody& body, core::RationalTime time,
                      document::FrameRate rate, ValueGraphCurves curves,
                      const CancellationToken* cancellation = nullptr);

    // The function at `x`. Nothing when the body failed (see diagnostics()) or was cancelled.
    [[nodiscard]] std::optional<double> at(double x);
    [[nodiscard]] std::span<const ValueGraphDiagnostic> diagnostics() const noexcept {
        return diagnostics_;
    }
    void clearDiagnostics() noexcept { diagnostics_.clear(); }

  private:
    const CompiledFunctionBody& body_;
    std::vector<CompiledValueOperation> operations_;
    core::RationalTime time_;
    document::FrameRate rate_;
    ValueGraphCurves curves_;
    const CancellationToken* cancellation_;
    std::vector<ValueGraphDiagnostic> diagnostics_;
};

} // namespace bloom::runtime
