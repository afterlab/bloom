#include <bloom/runtime/function_body.hpp>

#include <variant>

namespace bloom::runtime {

FunctionEvaluator::FunctionEvaluator(const CompiledFunctionBody& body,
                                     const core::RationalTime time, const document::FrameRate rate,
                                     const ValueGraphCurves curves,
                                     const CancellationToken* cancellation)
    : body_(body), operations_(body.operations), time_(time), rate_(rate), curves_(curves),
      cancellation_(cancellation) {}

std::optional<double> FunctionEvaluator::at(const double x) {
    if (body_.inputOperation.has_value() && *body_.inputOperation < operations_.size()) {
        if (auto* boundary =
                std::get_if<CompiledValueGroupBoundary>(&operations_[*body_.inputOperation].kernel);
            boundary != nullptr && body_.inputSlot < boundary->operands.size())
            boundary->operands[body_.inputSlot].source = CompiledValue{x};
    }
    // No memoization: the body is small, runs once per argument, and its values depend on the
    // argument, which the value cache's keys do not carry for a nested program.
    ValueGraphMemoization memoization;
    memoization.cancellation = cancellation_;
    auto evaluation =
        evaluateValueGraph(operations_, body_.outputCount, time_, rate_, curves_, memoization);
    if (cancellation_ != nullptr && cancellation_->isCancellationRequested())
        return std::nullopt;
    if (!evaluation.diagnostics.empty()) {
        diagnostics_.insert(diagnostics_.end(), evaluation.diagnostics.begin(),
                            evaluation.diagnostics.end());
        return std::nullopt;
    }
    if (body_.result.value() >= evaluation.outputs.size())
        return std::nullopt;
    const auto* value = std::get_if<double>(&evaluation.outputs[body_.result.value()]);
    return value == nullptr ? std::nullopt : std::optional<double>(*value);
}

} // namespace bloom::runtime
