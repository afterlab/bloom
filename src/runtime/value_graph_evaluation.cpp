#include <bloom/document/node_group_nodes.hpp>
#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/function_body.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>
#include <bloom/runtime/value_utility_kernels.hpp>

#include "field_raster.hpp"
#include <bloom/runtime/audio_analysis.hpp>
#include <bloom/runtime/image_statistics.hpp>
#include "gradient_source.hpp"
#include "operation_key.hpp"
#include <deque>

#include <bloom/core/scalar_primitives.hpp>
#include <bloom/core/value_primitives.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using namespace bloom;
using runtime::Color4CurveIndex;
using runtime::CompiledValue;
using runtime::CompiledValueOperand;
using runtime::sampleAnimationCurve;
using runtime::ScalarCurveIndex;
using runtime::ValueGraphCurves;
using runtime::ValueGraphDiagnostic;
using runtime::ValueOutputIndex;
using runtime::Vec2CurveIndex;
using runtime::Vec3CurveIndex;

using Scalar = core::primitives::ScalarPrimitive;

[[nodiscard]] std::optional<std::int64_t> checkedMultiply(const std::int64_t left,
                                                          const std::int64_t right) noexcept {
    if (left == 0 || right == 0) {
        return std::int64_t{0};
    }
    constexpr auto limit = std::numeric_limits<std::int64_t>::max();
    // Magnitudes only: both operands are already known non-zero, and the sign is reattached by the
    // multiplication itself. The most negative value has no positive magnitude, so it is refused
    // rather than negated.
    constexpr auto lowest = std::numeric_limits<std::int64_t>::min();
    if (left == lowest || right == lowest) {
        return std::nullopt;
    }
    const auto leftMagnitude = left < 0 ? -left : left;
    const auto rightMagnitude = right < 0 ? -right : right;
    if (leftMagnitude > limit / rightMagnitude) {
        return std::nullopt;
    }
    return left * right;
}

// Floor division over exact integers. std::int64_t division truncates toward zero, which would put
// the frame before time zero one frame too late.
[[nodiscard]] std::int64_t floorDivide(const std::int64_t numerator,
                                       const std::int64_t denominator) noexcept {
    const auto quotient = numerator / denominator;
    const auto remainder = numerator % denominator;
    const bool negative = (remainder != 0) && ((remainder < 0) != (denominator < 0));
    return negative ? quotient - 1 : quotient;
}

// The one place a value graph's operand is read. A constant answers with itself; an output
// reference answers with the entry already written for it. An index past what has been written
// cannot happen in a well-formed plan -- the operations are topologically ordered -- so nullptr
// here means the plan is malformed and the caller reports it.
// Samples one curve-backed operand into `sampled`, which is a deque so the pointer this returns
// stays valid for the whole of the operation that asked for it. A curve index the plan's own tables
// do not carry, or a sample the curve refuses, answers nothing -- and the caller reports it as a
// malformed operand exactly as it reports a bad output index.
[[nodiscard]] const CompiledValue* sample(const CompiledValueOperand& operand,
                                          const ValueGraphCurves& curves,
                                          const core::RationalTime time,
                                          std::deque<CompiledValue>& sampled) {
    if (const auto* index = std::get_if<ScalarCurveIndex>(&operand.source)) {
        if (index->value() >= curves.scalar.size()) {
            return nullptr;
        }
        const auto value = sampleAnimationCurve(curves.scalar[index->value()], time);
        if (!value || !value.value.has_value()) {
            return nullptr;
        }
        return &sampled.emplace_back(value.value.value());
    }
    if (const auto* index = std::get_if<Vec2CurveIndex>(&operand.source)) {
        if (index->value() >= curves.vec2.size()) {
            return nullptr;
        }
        const auto value = sampleAnimationCurve(curves.vec2[index->value()], time);
        if (!value || !value.value.has_value()) {
            return nullptr;
        }
        return &sampled.emplace_back(value.value.value());
    }
    if (const auto* index = std::get_if<Vec3CurveIndex>(&operand.source)) {
        if (index->value() >= curves.vec3.size()) {
            return nullptr;
        }
        const auto value = sampleAnimationCurve(curves.vec3[index->value()], time);
        if (!value || !value.value.has_value()) {
            return nullptr;
        }
        return &sampled.emplace_back(value.value.value());
    }
    const auto* index = std::get_if<Color4CurveIndex>(&operand.source);
    if (index == nullptr || index->value() >= curves.color4.size()) {
        return nullptr;
    }
    const auto value = sampleAnimationCurve(curves.color4[index->value()], time);
    if (!value || !value.value.has_value()) {
        return nullptr;
    }
    return &sampled.emplace_back(value.value.value());
}

[[nodiscard]] const CompiledValue*
read(const CompiledValueOperand& operand, const std::vector<CompiledValue>& outputs,
     const std::span<const std::uint8_t> available, const ValueGraphCurves& curves,
     const core::RationalTime time, std::deque<CompiledValue>& sampled) {
    if (const auto* constant = std::get_if<CompiledValue>(&operand.source)) {
        return constant;
    }
    if (const auto* output = std::get_if<ValueOutputIndex>(&operand.source)) {
        const auto index = output->value();
        if (index >= available.size() || index >= outputs.size() || available[index] == 0) {
            return nullptr;
        }
        return &outputs[index];
    }
    return sample(operand, curves, time, sampled);
}

// Strict readers. A Boolean is NOT silently a number and an Integer is NOT silently a Scalar: every
// widening the connect-time whitelist allows became its own CompiledValuePromotion operation, so
// anything arriving here with the wrong alternative means the plan disagrees with the document's
// own typing and is reported rather than coerced.
[[nodiscard]] const double* asScalar(const CompiledValue* value) {
    return value == nullptr ? nullptr : std::get_if<double>(value);
}
[[nodiscard]] const std::int64_t* asInteger(const CompiledValue* value) {
    return value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
}
[[nodiscard]] const bool* asBoolean(const CompiledValue* value) {
    return value == nullptr ? nullptr : std::get_if<bool>(value);
}
[[nodiscard]] const core::Color4d* asColor(const CompiledValue* value) {
    return value == nullptr ? nullptr : std::get_if<core::Color4d>(value);
}

// A vector operand of either width, as its components.
[[nodiscard]] std::optional<std::array<double, 3>> asVector(const CompiledValue* value,
                                                            const std::uint8_t components) {
    if (value == nullptr) {
        return std::nullopt;
    }
    if (components == 2) {
        const auto* vector = std::get_if<document::Vec2d>(value);
        return vector == nullptr ? std::nullopt
                                 : std::optional(std::array<double, 3>{vector->x, vector->y, 0.0});
    }
    const auto* vector = std::get_if<document::Vec3d>(value);
    return vector == nullptr
               ? std::nullopt
               : std::optional(std::array<double, 3>{vector->x, vector->y, vector->z});
}

[[nodiscard]] CompiledValue vectorValue(const std::array<double, 3>& components,
                                        const std::uint8_t width) {
    if (width == 2) {
        return document::Vec2d{components[0], components[1]};
    }
    return document::Vec3d{components[0], components[1], components[2]};
}

// The result of one scalar kernel call, with the tranche's domain failure already turned into this
// node's documented fallback.
struct ScalarOutcome final {
    double value = 0.0;
    bool failed = false;
};

[[nodiscard]] ScalarOutcome evaluate(const Scalar primitive, const std::span<const double> inputs,
                                     const double fallback) {
    const auto result = core::primitives::evaluateScalar(primitive, inputs);
    if (!result.hasValue()) {
        return {fallback, true};
    }
    return {*result.value(), false};
}

class Evaluator final {
  public:
    Evaluator(const std::size_t outputCount, const core::RationalTime time,
              const document::FrameRate rate, const ValueGraphCurves curves,
              runtime::ValueGraphMemoization memoization)
        : time_(time), rate_(rate), curves_(curves), memoization_(memoization) {
        outputs_.assign(outputCount, CompiledValue{0.0});
        available_.assign(outputCount, 0);
        if (!memoization_.initialOutputs.empty()) {
            const auto count = std::min(outputCount, memoization_.initialOutputs.size());
            for (std::size_t index = 0; index < count; ++index) {
                const auto& value = memoization_.initialOutputs[index];
                if (const auto* string = std::get_if<std::string>(&value)) {
                    if (string->size() > memoization_.maxStringBytes ||
                        string->size() >
                            memoization_.maxRetainedStringBytes - retainedStringBytes_) {
                        initialBudgetFailures_.push_back(index);
                        continue;
                    }
                    retainedStringBytes_ += string->size();
                }
                if (const auto* path = std::get_if<runtime::CompiledPathValue>(&value)) {
                    const auto anchors = path->value->anchorCount();
                    if (anchors > memoization_.maxRetainedPathAnchors - retainedPathAnchors_) {
                        initialBudgetFailures_.push_back(index);
                        continue;
                    }
                    retainedPathAnchors_ += anchors;
                }
                outputs_[index] = value;
            }
        }
    }

    void run(const std::span<const runtime::CompiledValueOperation> operations) {
        std::size_t operationIndex = 0;
        for (const auto& operation : operations) {
            if (memoization_.cancellation && memoization_.cancellation->isCancellationRequested())
                return;
            if (std::ranges::find_if(initialBudgetFailures_, [&](const auto index) {
                    return index >= operation.firstOutput.value() &&
                           index - operation.firstOutput.value() < operation.outputCount;
                }) != initialBudgetFailures_.end()) {
                fail(operation, {}, "Value graph string budget exceeded",
                     "An initial value exceeds this evaluation's string budget.");
                return;
            }
            const bool postOperation = operation.requiresPostImage;
            if (memoization_.pass == runtime::ValueGraphPass::PreImage && postOperation) {
                ++operationIndex;
                continue;
            }
            if (memoization_.pass == runtime::ValueGraphPass::PostImage && !postOperation) {
                markAvailable(operation);
                ++operationIndex;
                continue;
            }
            const auto first = operation.firstOutput.value();
            const auto count = static_cast<std::size_t>(operation.outputCount);
            if (count == 0 || first > outputs_.size() || count > outputs_.size() - first) {
                // The two passes may skip disjoint output runs, so contiguity is not an invariant.
                // Each operation must still own a valid range in the flat output table.
                fail(operation, {}, "Value operation claims an invalid output range",
                     "Value operations must claim a valid output range in the plan table.");
                return;
            }
            if (std::ranges::any_of(available_.begin() + static_cast<std::ptrdiff_t>(first),
                                    available_.begin() + static_cast<std::ptrdiff_t>(first + count),
                                    [](const auto value) { return value != 0; })) {
                fail(operation, {}, "Value operation overlaps an earlier output",
                     "A value output may be written by only one compiled operation.");
                return;
            }
            runtime::detail::OperationKey key;
            bool valid = true;
            std::optional<document::ParameterId> oversizedStringOperand;
            const bool cacheable =
                memoization_.cache != nullptr &&
                !std::holds_alternative<runtime::CompiledBoundsReadout>(operation.kernel) &&
                // #347: the level is read from the analysis cache under its own identity (source
                // revision and exact time), which a memoized value's key would not carry.
                !std::holds_alternative<runtime::CompiledValueAudioAmplitude>(operation.kernel) &&
                // #349: the statistics are read from their cache under the frame's own identity.
                !std::holds_alternative<runtime::CompiledValueImageStatistics>(operation.kernel);
            const auto currentOperation = operationIndex++;
            if (cacheable) {
                key.add(std::string("value"));
                key.add(memoization_.project);
                key.add(memoization_.composition);
                key.add(operation.sourceNodeId);
                key.add(operation.kernel.index());
                key.add(count);
                const bool dependent = currentOperation >= memoization_.timeDependence.size() ||
                                       memoization_.timeDependence[currentOperation] != 0;
                key.add(dependent);
                if (dependent)
                    key.add(time_);
                key.add(rate_);
                // A cache hit copies its retained values before write() can inspect them. Bind
                // the entry to the same byte and scratch allowances that admitted it, including
                // the room left by earlier output slots in this evaluation.
                key.add(memoization_.maxStringBytes);
                key.add(memoization_.maxRetainedStringBytes);
                key.add(memoization_.maxStringScratchBytes);
                key.add(memoization_.maxRetainedStringBytes - retainedStringBytes_);
                std::visit(
                    [&](const auto& kernel) {
                        if constexpr (requires { kernel.operation; })
                            key.add(kernel.operation);
                        if constexpr (requires { kernel.clampResult; })
                            key.add(kernel.clampResult);
                        if constexpr (requires { kernel.reduction; })
                            key.add(kernel.reduction);
                        if constexpr (requires { kernel.interpolation; })
                            key.add(kernel.interpolation);
                        if constexpr (requires { kernel.color; })
                            key.add(kernel.color);
                        if constexpr (requires { kernel.componentCount; })
                            key.add(kernel.componentCount);
                        if constexpr (requires { kernel.promotion; })
                            key.add(kernel.promotion);
                        if constexpr (requires { kernel.selectors; }) {
                            key.add(kernel.selectors.size());
                            for (const auto selector : kernel.selectors)
                                key.add(selector);
                        }
                        if constexpr (requires { kernel.components; }) {
                            if constexpr (std::is_integral_v<decltype(kernel.components)>)
                                key.add(kernel.components);
                            else
                                key.add(kernel.components.size());
                        }
                    },
                    operation.kernel);
                runtime::forEachValueOperand(operation.kernel,
                                             [&](const CompiledValueOperand& operand) {
                                                 const auto* value = operandOf(operand);
                                                 if (value == nullptr) {
                                                     valid = false;
                                                     return;
                                                 }
                                                 // OperationKey serializes the operand value. Do
                                                 // not copy an oversized literal into its key
                                                 // before the evaluator's output or utility-input
                                                 // budget checks can run.
                                                 const auto* string =
                                                     std::get_if<std::string>(value);
                                                 if (string != nullptr &&
                                                     string->size() > memoization_.maxStringBytes) {
                                                     oversizedStringOperand = operand.id;
                                                     valid = false;
                                                     return;
                                                 }
                                                 key.add(*value);
                                             });
            }
            const auto hit = cacheable && valid
                                 ? memoization_.cache->find(key.bytes(), memoization_.revision)
                                 : std::nullopt;
            if (hit) {
                for (std::size_t slot = 0; slot < std::min(count, hit->values.size()); ++slot)
                    write(operation, slot, hit->values[slot]);
                if (memoization_.statistics)
                    ++memoization_.statistics->hits;
            } else {
                if (memoization_.statistics) {
                    ++memoization_.statistics->misses;
                    memoization_.statistics->evaluatedNodes.push_back(operation.sourceNodeId);
                }
                const auto diagnosticsBefore = diagnostics_.size();
                evaluateOperation(operation);
                if (memoization_.cancellation &&
                    memoization_.cancellation->isCancellationRequested())
                    return;
                if (oversizedStringOperand && diagnosticsBefore == diagnostics_.size()) {
                    fail(operation, *oversizedStringOperand, "Value graph string budget exceeded",
                         "An input string exceeds the per-value byte limit.");
                }
                if (cacheable && valid && diagnosticsBefore == diagnostics_.size()) {
                    memoization_.cache->store(
                        key.bytes(), memoization_.revision,
                        {.image = {},
                         .values = {outputs_.begin() + static_cast<std::ptrdiff_t>(first),
                                    outputs_.begin() + static_cast<std::ptrdiff_t>(first + count)}},
                        runtime::OperationCacheEntryKind::Operation, {}, memoization_.linearPass);
                }
            }
            sampled_.clear();
            markAvailable(operation);
        }
    }

    [[nodiscard]] runtime::ValueGraphEvaluation release() {
        return {std::move(outputs_), std::move(diagnostics_)};
    }

  private:
    void fail(const runtime::CompiledValueOperation& operation,
              const document::ParameterId parameterId, std::string summary, std::string detail) {
        diagnostics_.push_back({operation.sourceNodeId, parameterId, operation.firstOutput,
                                std::move(summary), std::move(detail)});
    }

    [[nodiscard]] const CompiledValue* operandOf(const CompiledValueOperand& operand) {
        return read(operand, outputs_, available_, curves_, time_, sampled_);
    }

    // Bounds-checked against the operation's OWN run, not just against the table: a kernel's own
    // output count and the operation's declared one agree in every plan the compiler emits, but
    // this is the boundary where a malformed plan would otherwise write past a neighbour's slot. A
    // refused write is silently dropped rather than diagnosed twice -- run() already reported the
    // operation whose range is wrong, and every slot in the table starts at a usable zero.
    void write(const runtime::CompiledValueOperation& operation, const std::size_t slot,
               const CompiledValue& value) {
        if (slot >= static_cast<std::size_t>(operation.outputCount) ||
            operation.firstOutput.value() >= outputs_.size() ||
            slot >= outputs_.size() - operation.firstOutput.value()) {
            return;
        }
        const auto index = operation.firstOutput.value() + slot;
        const auto* oldString = std::get_if<std::string>(&outputs_[index]);
        const auto oldBytes = oldString == nullptr ? std::size_t{0} : oldString->size();
        if (const auto* string = std::get_if<std::string>(&value)) {
            if (string->size() > memoization_.maxStringBytes ||
                string->size() >
                    memoization_.maxRetainedStringBytes - (retainedStringBytes_ - oldBytes)) {
                fail(operation, {}, "Value graph string budget exceeded",
                     "The string output exceeds the per-value or retained-output byte limit.");
                outputs_[index] = std::string{};
                retainedStringBytes_ -= oldBytes;
                return;
            }
        }
        // #589: a shared path is charged once per output that holds it, against the same kind of
        // retained allowance strings have; a frame that exceeds it reads the empty path and says
        // so.
        const auto anchorsOf = [](const CompiledValue& held) -> std::size_t {
            const auto* path = std::get_if<runtime::CompiledPathValue>(&held);
            return path == nullptr ? 0 : path->value->anchorCount();
        };
        const auto oldAnchors = anchorsOf(outputs_[index]);
        const auto newAnchors = anchorsOf(value);
        if (newAnchors >
            memoization_.maxRetainedPathAnchors - (retainedPathAnchors_ - oldAnchors)) {
            fail(operation, {}, "Value graph path budget exceeded",
                 "The path output exceeds the retained-output anchor limit.");
            outputs_[index] = runtime::CompiledPathValue{};
            retainedPathAnchors_ -= oldAnchors;
            retainedStringBytes_ -= oldBytes;
            return;
        }
        const auto* newString = std::get_if<std::string>(&value);
        outputs_[index] = value;
        retainedPathAnchors_ = retainedPathAnchors_ - oldAnchors + newAnchors;
        retainedStringBytes_ = retainedStringBytes_ - oldBytes +
                               (newString == nullptr ? std::size_t{0} : newString->size());
    }

    void markAvailable(const runtime::CompiledValueOperation& operation) {
        const auto first = operation.firstOutput.value();
        const auto count = static_cast<std::size_t>(operation.outputCount);
        if (first > available_.size() || count > available_.size() - first)
            return;
        std::fill(available_.begin() + static_cast<std::ptrdiff_t>(first),
                  available_.begin() + static_cast<std::ptrdiff_t>(first + count), 1);
    }

    void evaluateOperation(const runtime::CompiledValueOperation& operation);
    void evaluatePassthrough(const runtime::CompiledValueOperation& operation,
                             const runtime::CompiledValuePassthrough& kernel);
    void evaluateTime(const runtime::CompiledValueOperation& operation);
    void evaluateScalarMath(const runtime::CompiledValueOperation& operation,
                            const runtime::CompiledValueScalarMath& kernel);
    void evaluateVectorMath(const runtime::CompiledValueOperation& operation,
                            const runtime::CompiledValueVectorMath& kernel);
    void evaluateVectorReduce(const runtime::CompiledValueOperation& operation,
                              const runtime::CompiledValueVectorReduce& kernel);
    void evaluateMapRange(const runtime::CompiledValueOperation& operation,
                          const runtime::CompiledValueMapRange& kernel);
    void evaluateClamp(const runtime::CompiledValueOperation& operation,
                       const runtime::CompiledValueClamp& kernel);
    void evaluateMix(const runtime::CompiledValueOperation& operation,
                     const runtime::CompiledValueMix& kernel);
    void evaluateCompare(const runtime::CompiledValueOperation& operation,
                         const runtime::CompiledValueCompare& kernel);
    void evaluateSwitch(const runtime::CompiledValueOperation& operation,
                        const runtime::CompiledValueSwitch& kernel);
    void evaluateSeparate(const runtime::CompiledValueOperation& operation,
                          const runtime::CompiledValueSeparate& kernel);
    void evaluateCombine(const runtime::CompiledValueOperation& operation,
                         const runtime::CompiledValueCombine& kernel);
    void evaluateRandom(const runtime::CompiledValueOperation& operation,
                        const runtime::CompiledValueRandom& kernel);
    void evaluateShake(const runtime::CompiledValueOperation& operation,
                       const runtime::CompiledValueShake& kernel);
    void evaluatePromotion(const runtime::CompiledValueOperation& operation,
                           const runtime::CompiledValuePromotion& kernel);
    void evaluateUtility(const runtime::CompiledValueOperation& operation,
                         const runtime::CompiledValueUtility& kernel);
    void evaluateBoundsReadout(const runtime::CompiledValueOperation& operation,
                               const runtime::CompiledBoundsReadout& kernel);
    void evaluateGradientSample(const runtime::CompiledValueOperation& operation,
                                const runtime::CompiledValueGradientSample& kernel);
    void evaluateFieldSample(const runtime::CompiledValueOperation& operation,
                             const runtime::CompiledValueFieldSample& kernel);
    void evaluatePathCurve(const runtime::CompiledValueOperation& operation,
                           const runtime::CompiledValuePathCurve& kernel);
    void evaluateGroupBoundary(const runtime::CompiledValueOperation& operation,
                               const runtime::CompiledValueGroupBoundary& kernel);
    void evaluateFunction(const runtime::CompiledValueOperation& operation,
                          const runtime::CompiledValueFunction& kernel);
    void evaluateFunctionPlot(const runtime::CompiledValueOperation& operation,
                              const runtime::CompiledValueFunctionPlot& kernel);
    void evaluateAudioAmplitude(const runtime::CompiledValueOperation& operation,
                                const runtime::CompiledValueAudioAmplitude& kernel);
    void evaluateImageStatistics(const runtime::CompiledValueOperation& operation,
                                 const runtime::CompiledValueImageStatistics& kernel);

    core::RationalTime time_;
    document::FrameRate rate_;
    ValueGraphCurves curves_;
    runtime::ValueGraphMemoization memoization_;
    // A deque, not a vector: every sampled curve value must keep a stable address for as long as
    // the operation that read it is running, and a vector would move them on the next sample.
    std::deque<CompiledValue> sampled_;
    std::vector<CompiledValue> outputs_;
    std::vector<std::uint8_t> available_;
    std::vector<ValueGraphDiagnostic> diagnostics_;
    std::size_t retainedStringBytes_ = 0;
    std::size_t retainedPathAnchors_ = 0;
    std::vector<std::size_t> initialBudgetFailures_;
};

void Evaluator::evaluateOperation(const runtime::CompiledValueOperation& operation) {
    std::visit(
        [&](const auto& kernel) {
            using Kernel = std::decay_t<decltype(kernel)>;
            if constexpr (std::is_same_v<Kernel, runtime::CompiledValuePassthrough>) {
                evaluatePassthrough(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueTime>) {
                evaluateTime(operation);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueScalarMath>) {
                evaluateScalarMath(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueVectorMath>) {
                evaluateVectorMath(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueVectorReduce>) {
                evaluateVectorReduce(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueMapRange>) {
                evaluateMapRange(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueClamp>) {
                evaluateClamp(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueMix>) {
                evaluateMix(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueCompare>) {
                evaluateCompare(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueSwitch>) {
                evaluateSwitch(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueSeparate>) {
                evaluateSeparate(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueCombine>) {
                evaluateCombine(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueRandom>) {
                evaluateRandom(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueShake>) {
                evaluateShake(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueUtility>) {
                evaluateUtility(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledBoundsReadout>) {
                evaluateBoundsReadout(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueGradientSample>) {
                evaluateGradientSample(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueFieldSample>) {
                evaluateFieldSample(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValuePathCurve>) {
                evaluatePathCurve(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueGroupBoundary>) {
                evaluateGroupBoundary(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueFunction>) {
                evaluateFunction(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueFunctionPlot>) {
                evaluateFunctionPlot(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueAudioAmplitude>) {
                evaluateAudioAmplitude(operation, kernel);
            } else if constexpr (std::is_same_v<Kernel, runtime::CompiledValueImageStatistics>) {
                evaluateImageStatistics(operation, kernel);
            } else {
                evaluatePromotion(operation, kernel);
            }
        },
        operation.kernel);
}

// #347: the level of the source's audio over the window around this frame (plus the offset). The
// measurement was prepared on a worker; it is read from the cache here and never decoded, so a frame
// evaluated on the interface thread never waits on a file. A frame whose analysis is not prepared
// reads silence and says so.
void Evaluator::evaluateAudioAmplitude(const runtime::CompiledValueOperation& operation,
                                       const runtime::CompiledValueAudioAmplitude& kernel) {
    constexpr double kSilenceDecibels = -120.0;
    const auto silence = [&] {
        write(operation, 0, kernel.unit == 1 ? kSilenceDecibels : 0.0);
    };
    const auto* window = asScalar(operandOf(kernel.window));
    const auto* offset = asScalar(operandOf(kernel.offset));
    if (window == nullptr || offset == nullptr) {
        fail(operation, kernel.window.id, "Audio Amplitude operand has the wrong kind",
             "The window and the offset must reach the node as Scalars.");
        silence();
        return;
    }
    if (!kernel.source) {
        silence();
        return;
    }
    const auto query = runtime::audioAmplitudeQuery(kernel, time_, *window, *offset);
    if (!query) {
        fail(operation, kernel.window.id, "Audio Amplitude values are outside their range",
             "The window is 1 to 60000 milliseconds and the offset within a day.");
        silence();
        return;
    }
    auto found = runtime::findAudioAnalysis(*query);
    // A worker that evaluates the frame prepares the analysis here, with the operand values this
    // evaluation resolved; an evaluation outside a worker's preparation only reads what is prepared.
    if (!found) {
        if (const auto status = runtime::AudioAnalysisPreparation::prepareHere(*query)) {
            if (status->outcome == runtime::AudioAnalysisOutcome::Ready) {
                found = runtime::findAudioAnalysis(*query);
            } else {
                if (status->outcome == runtime::AudioAnalysisOutcome::Unavailable)
                    fail(operation, {}, "Audio Amplitude audio is unavailable", status->detail);
                silence();
                return;
            }
        }
    }
    if (!found) {
        fail(operation, {}, "Audio Amplitude is not prepared",
             "The audio at this time has not been analyzed yet; the level reads as silence until it is.");
        silence();
        return;
    }
    const double linear = found->amplitude;
    write(operation, 0,
          kernel.unit == 1 ? std::max(kSilenceDecibels, 20.0 * std::log10(std::max(linear, 1.0e-6)))
                           : linear);
}

// #349: the statistics of this frame's picture. They were prepared on a worker before the value was
// sampled; they are read from the cache here and never computed, so a frame sampled on the
// interface thread never waits on pixels. A frame whose statistics are not prepared reads 0 for
// every output and says so: it never reads another frame's statistics.
void Evaluator::evaluateImageStatistics(const runtime::CompiledValueOperation& operation,
                                        const runtime::CompiledValueImageStatistics& kernel) {
    const auto zero = [&] {
        for (std::size_t slot = 0; slot < 5; ++slot)
            write(operation, slot, 0.0);
    };
    if (!kernel.plan) {
        zero();
        return;
    }
    const runtime::ImageStatisticsQuery query{kernel.node, kernel.plan, time_};
    auto found = runtime::ImageStatisticsPreparation::findHere(query);
    // A worker that evaluates the frame prepares the statistics here, for this frame; an evaluation
    // outside a worker's preparation only reads what is prepared.
    if (!found) {
        if (const auto status = runtime::ImageStatisticsPreparation::prepareHere(query)) {
            if (status->outcome == runtime::ImageStatisticsOutcome::Ready) {
                found = runtime::ImageStatisticsPreparation::findHere(query);
            } else {
                if (status->outcome == runtime::ImageStatisticsOutcome::Unavailable)
                    fail(operation, {}, "Image Statistics are unavailable", status->detail);
                zero();
                return;
            }
        }
    }
    if (!found) {
        fail(operation, {}, "Image Statistics are not ready",
             "The statistics of this frame's picture have not been computed yet; every output "
             "reads 0 and is not a measured value.");
        zero();
        return;
    }
    write(operation, 0, found->minimum);
    write(operation, 1, found->maximum);
    write(operation, 2, found->mean);
    write(operation, 3, found->deviation);
    write(operation, 4, found->count);
}

void Evaluator::evaluateBoundsReadout(const runtime::CompiledValueOperation& operation,
                                      const runtime::CompiledBoundsReadout& kernel) {
    const auto index = kernel.operationIndex.value();
    if (index >= memoization_.evaluatedBounds.size()) {
        fail(operation, {}, "Layer Bounds input is unavailable",
             "The connected image operation did not publish evaluated bounds for this frame.");
        for (std::size_t slot = 0; slot < 4; ++slot)
            write(operation, slot, document::Vec2d{});
        return;
    }
    const auto& bounds = memoization_.evaluatedBounds[index];
    write(operation, 0,
          document::Vec2d{std::max(0.0, bounds.output.right - bounds.output.left),
                          std::max(0.0, bounds.output.bottom - bounds.output.top)});
    write(operation, 1, document::Vec2d{bounds.output.left, bounds.output.top});
    write(operation, 2, bounds.anchor);
    write(operation, 3, bounds.output.centre());
}

// #541: the gradient at this frame, then its colour at the kernel's position.
void Evaluator::evaluateGradientSample(const runtime::CompiledValueOperation& operation,
                                       const runtime::CompiledValueGradientSample& kernel) {
    // #543: with the stops its value nodes drive this frame.
    const auto sampled = runtime::detail::applyGradientStopDrivers(
        runtime::detail::sampleGradientSource(kernel.gradient, time_), kernel.drivers,
        [this](const runtime::CompiledValueGradientStopOperand& driver) {
            return operandOf(driver.value);
        });
    if (!sampled) {
        fail(operation, {}, "Gradient is not evaluable at this time",
             "The gradient's keys or driven stops could not be sampled; its colour falls back to "
             "black.");
        write(operation, 0, core::Color4d{0.0, 0.0, 0.0, 1.0});
        return;
    }
    write(operation, 0, runtime::detail::gradientValueTintAt(*sampled, kernel.position));
}

// #589: a keyed Path literal at this frame. A curve that cannot sample falls back to the empty
// path.
void Evaluator::evaluatePathCurve(const runtime::CompiledValueOperation& operation,
                                  const runtime::CompiledValuePathCurve& kernel) {
    auto sampled = runtime::samplePathCurve(kernel.curve, time_);
    if (!sampled) {
        fail(operation, {}, "Path is not evaluable at this time",
             "The path's keys could not be sampled; it falls back to the empty path.");
        write(operation, 0, runtime::CompiledPathValue{});
        return;
    }
    write(operation, 0,
          runtime::CompiledPathValue{std::make_shared<document::PathValue>(std::move(*sampled))});
}

// #525: the field program at this frame, read at the kernel's position. Its Scalar and Vector 2
// operands are the kernel's operands in program order; its curves are sampled here.
void Evaluator::evaluateFieldSample(const runtime::CompiledValueOperation& operation,
                                    const runtime::CompiledValueFieldSample& kernel) {
    const auto publish = [&](const std::array<float, 4>& value) {
        write(operation, 0, static_cast<double>(value[0]));
        write(operation, 1,
              document::Vec2d{static_cast<double>(value[0]), static_cast<double>(value[1])});
        write(operation, 2,
              core::Color4d{static_cast<double>(value[0]), static_cast<double>(value[1]),
                            static_cast<double>(value[2]), static_cast<double>(value[3])});
    };
    std::vector<double> values;
    std::size_t next = 0;
    bool evaluable = kernel.program != nullptr;
    if (evaluable)
        runtime::forEachFieldOperand(*kernel.program, [&](const auto& parameter) {
            using Parameter = std::decay_t<decltype(parameter)>;
            if constexpr (std::is_same_v<Parameter, runtime::CompiledPathParameter>) {
                std::optional<document::PathValue> sampled;
                if (const auto* constant = std::get_if<document::PathValue>(&parameter.source))
                    sampled = *constant;
                else
                    sampled = runtime::samplePathCurve(
                        std::get<runtime::CompiledPathCurve>(parameter.source), time_);
                if (!sampled) {
                    evaluable = false;
                    return;
                }
                values.push_back(static_cast<double>(sampled->firstContour().anchors.size()));
                for (const auto& anchor : sampled->firstContour().anchors) {
                    values.push_back(anchor.point.x);
                    values.push_back(anchor.point.y);
                }
            } else if constexpr (std::is_same_v<Parameter, runtime::CompiledGradientParameter>) {
                // A ramp is a raster's colouring; a Field Sample's program has none.
                evaluable = false;
            } else {
                const auto* value =
                    next < kernel.operands.size() ? operandOf(kernel.operands[next]) : nullptr;
                ++next;
                if (const auto* scalar = asScalar(value)) {
                    values.push_back(*scalar);
                } else if (const auto* vector =
                               value == nullptr ? nullptr : std::get_if<document::Vec2d>(value)) {
                    values.push_back(vector->x);
                    values.push_back(vector->y);
                } else {
                    evaluable = false;
                }
            }
        });
    values.push_back(time_.toSeconds());
    const auto* positionValue = operandOf(kernel.position);
    const auto* position =
        positionValue == nullptr ? nullptr : std::get_if<document::Vec2d>(positionValue);
    const auto sampled =
        evaluable && position != nullptr && next == kernel.operands.size()
            ? runtime::detail::sampleFieldProgram(*kernel.program, values, position->x,
                                                  position->y)
            : std::nullopt;
    if (!sampled) {
        fail(operation, {}, "Field is not evaluable at this time",
             "The field's operands could not be sampled or lie outside the domain its operations "
             "represent; the sample falls back to zero.");
        publish({0.0F, 0.0F, 0.0F, 0.0F});
        return;
    }
    publish(*sampled);
}

void Evaluator::evaluatePassthrough(const runtime::CompiledValueOperation& operation,
                                    const runtime::CompiledValuePassthrough& kernel) {
    const auto* value = operandOf(kernel.value);
    if (value == nullptr) {
        fail(operation, kernel.value.id, "Value operand is unavailable",
             "The operand names a value-graph output that has not been computed.");
        return;
    }
    write(operation, 0, *value);
}

void Evaluator::evaluateGroupBoundary(const runtime::CompiledValueOperation& operation,
                                      const runtime::CompiledValueGroupBoundary& kernel) {
    for (std::size_t slot = 0; slot < kernel.operands.size(); ++slot) {
        const auto* value = operandOf(kernel.operands[slot]);
        if (value == nullptr) {
            fail(operation, kernel.operands[slot].id, "Value operand is unavailable",
                 "The operand names a value-graph output that has not been computed.");
            continue;
        }
        write(operation, slot, *value);
    }
}

void Evaluator::evaluateFunction(const runtime::CompiledValueOperation& operation,
                                 const runtime::CompiledValueFunction& kernel) {
    const auto* argument = operandOf(kernel.argument);
    const auto* x = argument == nullptr ? nullptr : std::get_if<double>(argument);
    if (x == nullptr || kernel.body == nullptr) {
        fail(operation, kernel.argument.id, "Function argument is unavailable",
             "The function's argument is not a number the value graph computed.");
        write(operation, 0, 0.0);
        return;
    }
    runtime::FunctionEvaluator body(*kernel.body, time_, rate_, curves_, memoization_.cancellation);
    const auto answer = body.at(*x);
    if (!answer.has_value()) {
        // Cancelled evaluations carry no diagnostic: the frame is being abandoned.
        if (!(memoization_.cancellation && memoization_.cancellation->isCancellationRequested()))
            fail(operation, {}, "Function could not be evaluated",
                 body.diagnostics().empty() ? "The group's answer is not a number."
                                            : body.diagnostics().front().detail);
        write(operation, 0, 0.0);
        return;
    }
    write(operation, 0, *answer);
}

// #595: the body sampled over [start, end] into one open contour, in pixels with Y up on the plot
// (so Y is subtracted from the origin). A sample that fails or is not finite ends the contour, so a
// pole or a domain edge is a gap rather than a line drawn across it.
void Evaluator::evaluateFunctionPlot(const runtime::CompiledValueOperation& operation,
                                     const runtime::CompiledValueFunctionPlot& kernel) {
    const auto scalar = [&](const runtime::CompiledValueOperand& operand) {
        const auto* value = operandOf(operand);
        return value == nullptr ? nullptr : std::get_if<double>(value);
    };
    const auto vector = [&](const runtime::CompiledValueOperand& operand) {
        const auto* value = operandOf(operand);
        return value == nullptr ? nullptr : std::get_if<document::Vec2d>(value);
    };
    const auto* start = scalar(kernel.start);
    const auto* end = scalar(kernel.end);
    const auto* samples = [&] {
        const auto* value = operandOf(kernel.samples);
        return value == nullptr ? nullptr : std::get_if<std::int64_t>(value);
    }();
    const auto* origin = vector(kernel.origin);
    const auto* scale = vector(kernel.scale);
    const auto* smooth = [&] {
        const auto* value = operandOf(kernel.smooth);
        return value == nullptr ? nullptr : std::get_if<bool>(value);
    }();
    const auto empty = [&] { write(operation, 0, runtime::CompiledPathValue{}); };
    if (start == nullptr || end == nullptr || samples == nullptr || origin == nullptr ||
        scale == nullptr || smooth == nullptr || kernel.body == nullptr) {
        fail(operation, {}, "Function Plot input is unavailable",
             "An input of the plot is not a value the value graph computed.");
        empty();
        return;
    }
    if (!std::isfinite(*start) || !std::isfinite(*end) || !std::isfinite(origin->x) ||
        !std::isfinite(origin->y) || !std::isfinite(scale->x) || !std::isfinite(scale->y)) {
        fail(operation, {}, "Function Plot range is not finite",
             "The range, the origin and the scale must be finite numbers.");
        empty();
        return;
    }
    const auto count = static_cast<std::size_t>(
        std::clamp(*samples, document::kMinimumFunctionPlotSamples,
                   document::kMaximumFunctionPlotSamples));
    runtime::FunctionEvaluator body(*kernel.body, time_, rate_, curves_, memoization_.cancellation);
    document::PathValue value;
    document::PathContour contour;
    std::size_t valid = 0;
    const auto flush = [&] {
        if (contour.anchors.size() >= 2)
            value.contours.push_back(std::move(contour));
        contour = {};
    };
    const auto mode = *smooth ? document::PathAnchorMode::Auto : document::PathAnchorMode::Corner;
    for (std::size_t index = 0; index < count; ++index) {
        if (memoization_.cancellation && memoization_.cancellation->isCancellationRequested())
            return;
        const double x = index + 1 == count
                             ? *end
                             : *start + (*end - *start) * static_cast<double>(index) /
                                            static_cast<double>(count - 1);
        const auto y = body.at(x);
        body.clearDiagnostics();
        const double px = origin->x + x * scale->x;
        const double py = y.has_value() ? origin->y - *y * scale->y : 0.0;
        if (!y.has_value() || !std::isfinite(px) || !std::isfinite(py)) {
            flush();
            continue;
        }
        ++valid;
        contour.anchors.push_back({document::Vec2d{px, py}, std::nullopt, std::nullopt, mode});
    }
    flush();
    if (valid == 0)
        fail(operation, {}, "Function Plot has no valid samples",
             "The function gave no finite answer anywhere in the range.");
    write(operation, 0,
          runtime::CompiledPathValue{std::make_shared<document::PathValue>(std::move(value))});
}

void Evaluator::evaluateTime(const runtime::CompiledValueOperation& operation) {
    write(operation, 0, time_.toSeconds());
    const auto frame = runtime::valueGraphFrameIndex(time_, rate_);
    if (!frame.has_value()) {
        // Fallback zero plus a diagnostic, not a frame number nobody can name: the product
        // overflowed the exact integer range, which means the request time is outside anything a
        // frame index could address.
        fail(operation, {}, "Frame number is not representable at this time",
             "The request time multiplied by the frame rate overflows an exact integer.");
        write(operation, 1, std::int64_t{0});
        return;
    }
    write(operation, 1, *frame);
}

void Evaluator::evaluateScalarMath(const runtime::CompiledValueOperation& operation,
                                   const runtime::CompiledValueScalarMath& kernel) {
    std::array<double, document::kMaximumScalarOperationOperands> inputs{};
    if (kernel.operands.size() > inputs.size()) {
        fail(operation, {}, "Math operation has too many operands",
             "No scalar primitive reads more operands than the vocabulary's widest.");
        write(operation, 0, 0.0);
        return;
    }
    for (std::size_t index = 0; index < kernel.operands.size(); ++index) {
        const auto* value = asScalar(operandOf(kernel.operands[index]));
        if (value == nullptr) {
            fail(operation, kernel.operands[index].id, "Math operand is not a scalar",
                 "Every Math operand must reach the kernel as a Scalar.");
            write(operation, 0, 0.0);
            return;
        }
        inputs[index] = *value;
    }
    // Zero is the documented fallback for every scalar domain failure, which is also the usual
    // node-graph divide-by-zero answer -- so the most common failure an artist will actually hit
    // behaves the way they expect, and the rest behave consistently with it.
    auto outcome =
        evaluate(kernel.operation, std::span(inputs.data(), kernel.operands.size()), 0.0);
    if (outcome.failed) {
        fail(operation, {}, "Math operation is outside its domain",
             "The operation's fallback value of zero was substituted.");
    }
    if (kernel.clampResult) {
        const std::array<double, 3> clampInputs{outcome.value, 0.0, 1.0};
        outcome = evaluate(Scalar::Clamp, clampInputs, 0.0);
    }
    write(operation, 0, outcome.value);
}

void Evaluator::evaluateVectorMath(const runtime::CompiledValueOperation& operation,
                                   const runtime::CompiledValueVectorMath& kernel) {
    const auto width = kernel.components;
    const auto left = asVector(operandOf(kernel.left), width);
    const auto right = asVector(operandOf(kernel.right), width);
    const std::array<double, 3> zero{};
    if (!left.has_value() || !right.has_value()) {
        fail(operation, kernel.left.id, "Vector Math operand has the wrong width",
             "Both operands must reach the kernel as vectors of the node's own width.");
        write(operation, 0, vectorValue(zero, width));
        return;
    }
    std::array<double, 3> result{};
    bool failed = false;
    const auto componentwise = [&](const Scalar primitive) {
        for (std::uint8_t component = 0; component < width; ++component) {
            const std::array<double, 2> inputs{(*left)[component], (*right)[component]};
            const auto outcome = evaluate(primitive, inputs, 0.0);
            result[component] = outcome.value;
            failed = failed || outcome.failed;
        }
    };
    switch (kernel.operation) {
    case document::VectorOperation::Add:
        componentwise(Scalar::Add);
        break;
    case document::VectorOperation::Subtract:
        componentwise(Scalar::Subtract);
        break;
    case document::VectorOperation::Multiply:
        componentwise(Scalar::Multiply);
        break;
    case document::VectorOperation::Divide:
        componentwise(Scalar::Divide);
        break;
    case document::VectorOperation::Scale: {
        const auto* factor = asScalar(operandOf(kernel.factor));
        if (factor == nullptr) {
            fail(operation, kernel.factor.id, "Scale factor is not a scalar",
                 "The scale operand must reach the kernel as a Scalar.");
            write(operation, 0, vectorValue(zero, width));
            return;
        }
        for (std::uint8_t component = 0; component < width; ++component) {
            const std::array<double, 2> inputs{(*left)[component], *factor};
            const auto outcome = evaluate(Scalar::Multiply, inputs, 0.0);
            result[component] = outcome.value;
            failed = failed || outcome.failed;
        }
        break;
    }
    case document::VectorOperation::Normalize:
    case document::VectorOperation::CrossProduct: {
        const auto primitive = kernel.operation == document::VectorOperation::Normalize
                                   ? core::primitives::VectorPrimitive::Normalize
                                   : core::primitives::VectorPrimitive::CrossProduct;
        std::array<double, 6> inputs{};
        std::copy_n(left->begin(), width, inputs.begin());
        std::copy_n(right->begin(), width, inputs.begin() + width);
        const auto operandCount =
            static_cast<std::size_t>(width) *
            (primitive == core::primitives::VectorPrimitive::Normalize ? 1U : 2U);
        const auto evaluated = core::primitives::evaluateVector(
            primitive, width, std::span(inputs.data(), operandCount));
        if (!evaluated.hasValue()) {
            // A zero-length vector has no direction, so the documented fallback is the zero vector
            // itself rather than an arbitrary axis.
            failed = true;
        } else {
            std::copy_n(evaluated.components().begin(), width, result.begin());
        }
        break;
    }
    }
    if (failed) {
        fail(operation, {}, "Vector Math operation is outside its domain",
             "The operation's fallback of zero in the affected components was substituted.");
    }
    write(operation, 0, vectorValue(result, width));
}

void Evaluator::evaluateVectorReduce(const runtime::CompiledValueOperation& operation,
                                     const runtime::CompiledValueVectorReduce& kernel) {
    const auto width = kernel.components;
    const auto left = asVector(operandOf(kernel.left), width);
    const auto right = asVector(operandOf(kernel.right), width);
    if (!left.has_value() || !right.has_value()) {
        fail(operation, kernel.left.id, "Vector operand has the wrong width",
             "Both operands must reach the kernel as vectors of the node's own width.");
        write(operation, 0, 0.0);
        return;
    }
    const auto primitive = document::vectorReductionPrimitive(kernel.reduction);
    const auto* signature = core::primitives::vectorPrimitiveSignature(primitive);
    std::array<double, 6> inputs{};
    std::copy_n(left->begin(), width, inputs.begin());
    std::copy_n(right->begin(), width, inputs.begin() + width);
    const auto operandCount =
        signature == nullptr ? 0 : static_cast<std::size_t>(width) * signature->vectorOperandCount;
    const auto evaluated =
        core::primitives::evaluateVector(primitive, width, std::span(inputs.data(), operandCount));
    if (!evaluated.hasValue()) {
        fail(operation, {}, "Vector reduction is outside its domain",
             "The reduction's fallback value of zero was substituted.");
        write(operation, 0, 0.0);
        return;
    }
    write(operation, 0, evaluated.components()[0]);
}

void Evaluator::evaluateMapRange(const runtime::CompiledValueOperation& operation,
                                 const runtime::CompiledValueMapRange& kernel) {
    const auto* value = asScalar(operandOf(kernel.value));
    const auto* fromMinimum = asScalar(operandOf(kernel.fromMinimum));
    const auto* fromMaximum = asScalar(operandOf(kernel.fromMaximum));
    const auto* toMinimum = asScalar(operandOf(kernel.toMinimum));
    const auto* toMaximum = asScalar(operandOf(kernel.toMaximum));
    if (value == nullptr || fromMinimum == nullptr || fromMaximum == nullptr ||
        toMinimum == nullptr || toMaximum == nullptr) {
        fail(operation, kernel.value.id, "Map Range operand is not a scalar",
             "Every Map Range operand must reach the kernel as a Scalar.");
        write(operation, 0, 0.0);
        return;
    }
    // A degenerate source range has no factor at all, so the documented fallback is the destination
    // minimum: the answer every value in that range would map to if the range had any width.
    const double fallback = *toMinimum;
    ScalarOutcome outcome{fallback, false};
    if (kernel.interpolation == document::RangeInterpolation::Linear) {
        const std::array<double, 5> inputs{*value, *fromMinimum, *fromMaximum, *toMinimum,
                                           *toMaximum};
        outcome = evaluate(Scalar::Remap, inputs, fallback);
    } else {
        const auto primitive = kernel.interpolation == document::RangeInterpolation::Smoothstep
                                   ? Scalar::Smoothstep
                                   : Scalar::Smootherstep;
        const std::array<double, 3> edges{*fromMinimum, *fromMaximum, *value};
        const auto factor = evaluate(primitive, edges, 0.0);
        if (factor.failed) {
            outcome = {fallback, true};
        } else {
            const std::array<double, 3> inputs{*toMinimum, *toMaximum, factor.value};
            outcome = evaluate(Scalar::Mix, inputs, fallback);
        }
    }
    if (outcome.failed) {
        fail(operation, {}, "Map Range is outside its domain",
             "The destination minimum was substituted for a degenerate source range.");
    }
    if (kernel.clampResult) {
        // Clamped to the DESTINATION range, in whichever order it was authored: a reversed To range
        // is a legitimate inversion, not a mistake to be corrected.
        const std::array<double, 3> inputs{outcome.value, std::min(*toMinimum, *toMaximum),
                                           std::max(*toMinimum, *toMaximum)};
        outcome = evaluate(Scalar::Clamp, inputs, outcome.value);
    }
    write(operation, 0, outcome.value);
}

void Evaluator::evaluateClamp(const runtime::CompiledValueOperation& operation,
                              const runtime::CompiledValueClamp& kernel) {
    const auto* value = asScalar(operandOf(kernel.value));
    const auto* minimum = asScalar(operandOf(kernel.minimum));
    const auto* maximum = asScalar(operandOf(kernel.maximum));
    if (value == nullptr || minimum == nullptr || maximum == nullptr) {
        fail(operation, kernel.value.id, "Clamp operand is not a scalar",
             "Every Clamp operand must reach the kernel as a Scalar.");
        write(operation, 0, 0.0);
        return;
    }
    const std::array<double, 3> inputs{*value, *minimum, *maximum};
    // Reversed bounds name no interval, so the documented fallback is the value UNCLAMPED: passing
    // it through is visibly wrong in the picture, where silently swapping the bounds would look
    // correct and hide the authoring mistake.
    const auto outcome = evaluate(Scalar::Clamp, inputs, *value);
    if (outcome.failed) {
        fail(operation, {}, "Clamp bounds are reversed",
             "The unclamped value was substituted for an inverted bound pair.");
    }
    write(operation, 0, outcome.value);
}

void Evaluator::evaluateMix(const runtime::CompiledValueOperation& operation,
                            const runtime::CompiledValueMix& kernel) {
    const auto* factor = asScalar(operandOf(kernel.factor));
    if (factor == nullptr) {
        fail(operation, kernel.factor.id, "Mix factor is not a scalar",
             "The factor operand must reach the kernel as a Scalar.");
        write(operation, 0, kernel.color ? CompiledValue{core::Color4d{}} : CompiledValue{0.0});
        return;
    }
    if (!kernel.color) {
        const auto* start = asScalar(operandOf(kernel.start));
        const auto* end = asScalar(operandOf(kernel.end));
        if (start == nullptr || end == nullptr) {
            fail(operation, kernel.start.id, "Mix operand is not a scalar",
                 "Both Mix endpoints must reach the kernel as Scalars.");
            write(operation, 0, 0.0);
            return;
        }
        const std::array<double, 3> inputs{*start, *end, *factor};
        const auto outcome = evaluate(Scalar::Mix, inputs, *start);
        if (outcome.failed) {
            fail(operation, {}, "Mix is outside its domain", "The start value was substituted.");
        }
        write(operation, 0, outcome.value);
        return;
    }
    const auto* start = asColor(operandOf(kernel.start));
    const auto* end = asColor(operandOf(kernel.end));
    if (start == nullptr || end == nullptr) {
        fail(operation, kernel.start.id, "Mix operand is not a color",
             "Both Mix endpoints must reach the kernel as Colors.");
        write(operation, 0, core::Color4d{});
        return;
    }
    // Four independent straight-value interpolations, alpha included and alpha LAST -- not a vector
    // mix of four floats. A Color is not a Vec4, and nothing here implies a gamut or OCIO step.
    const std::array<std::pair<double, double>, 4> channels{
        std::pair{start->red, end->red}, std::pair{start->green, end->green},
        std::pair{start->blue, end->blue}, std::pair{start->alpha, end->alpha}};
    std::array<double, 4> mixed{};
    bool failed = false;
    for (std::size_t channel = 0; channel < channels.size(); ++channel) {
        const std::array<double, 3> inputs{channels[channel].first, channels[channel].second,
                                           *factor};
        const auto outcome = evaluate(Scalar::Mix, inputs, channels[channel].first);
        mixed[channel] = outcome.value;
        failed = failed || outcome.failed;
    }
    if (failed) {
        fail(operation, {}, "Color Mix is outside its domain",
             "The start color's affected channels were substituted.");
    }
    write(operation, 0, core::Color4d{mixed[0], mixed[1], mixed[2], mixed[3]});
}

void Evaluator::evaluateCompare(const runtime::CompiledValueOperation& operation,
                                const runtime::CompiledValueCompare& kernel) {
    const auto* left = asScalar(operandOf(kernel.left));
    const auto* right = asScalar(operandOf(kernel.right));
    const auto* epsilon = asScalar(operandOf(kernel.epsilon));
    if (left == nullptr || right == nullptr || epsilon == nullptr) {
        fail(operation, kernel.left.id, "Compare operand is not a scalar",
             "Every Compare operand must reach the kernel as a Scalar.");
        write(operation, 0, false);
        return;
    }
    const double difference = std::abs(*left - *right);
    const bool within = difference <= *epsilon;
    bool result = false;
    switch (kernel.operation) {
    case document::CompareOperation::Equal:
        result = within;
        break;
    case document::CompareOperation::NotEqual:
        result = !within;
        break;
    case document::CompareOperation::Less:
        result = *left < *right;
        break;
    case document::CompareOperation::LessOrEqual:
        result = *left <= *right;
        break;
    case document::CompareOperation::Greater:
        result = *left > *right;
        break;
    case document::CompareOperation::GreaterOrEqual:
        result = *left >= *right;
        break;
    }
    write(operation, 0, result);
}

void Evaluator::evaluateSwitch(const runtime::CompiledValueOperation& operation,
                               const runtime::CompiledValueSwitch& kernel) {
    const auto* condition = asBoolean(operandOf(kernel.condition));
    if (condition == nullptr) {
        fail(operation, kernel.condition.id, "Switch condition is not a boolean",
             "The condition operand must reach the kernel as a Boolean.");
    }
    const auto& chosen = (condition != nullptr && *condition) ? kernel.ifTrue : kernel.ifFalse;
    const auto* value = operandOf(chosen);
    if (value == nullptr) {
        fail(operation, chosen.id, "Switch branch is unavailable",
             "The selected branch names a value-graph output that has not been computed.");
        return;
    }
    write(operation, 0, *value);
}

void Evaluator::evaluateSeparate(const runtime::CompiledValueOperation& operation,
                                 const runtime::CompiledValueSeparate& kernel) {
    const auto* value = operandOf(kernel.value);
    if (kernel.color) {
        const auto* color = asColor(value);
        if (color == nullptr) {
            fail(operation, kernel.value.id, "Separate operand is not a color",
                 "The operand must reach the kernel as a Color.");
            for (std::size_t slot = 0; slot < kernel.componentCount; ++slot) {
                write(operation, slot, 0.0);
            }
            return;
        }
        const std::array<double, 4> channels{color->red, color->green, color->blue, color->alpha};
        const auto written = std::min<std::size_t>(kernel.componentCount, channels.size());
        for (std::size_t slot = 0; slot < written; ++slot) {
            write(operation, slot, channels[slot]);
        }
        return;
    }
    const auto components = asVector(value, kernel.componentCount);
    if (!components.has_value()) {
        fail(operation, kernel.value.id, "Separate operand has the wrong width",
             "The operand must reach the kernel as a vector of the node's own width.");
        for (std::size_t slot = 0; slot < kernel.componentCount; ++slot) {
            write(operation, slot, 0.0);
        }
        return;
    }
    const auto written = std::min<std::size_t>(kernel.componentCount, components->size());
    for (std::size_t slot = 0; slot < written; ++slot) {
        write(operation, slot, (*components)[slot]);
    }
}

void Evaluator::evaluateCombine(const runtime::CompiledValueOperation& operation,
                                const runtime::CompiledValueCombine& kernel) {
    std::array<double, 4> components{};
    for (std::size_t index = 0; index < kernel.components.size() && index < components.size();
         ++index) {
        const auto* value = asScalar(operandOf(kernel.components[index]));
        if (value == nullptr) {
            fail(operation, kernel.components[index].id, "Combine operand is not a scalar",
                 "Every Combine component must reach the kernel as a Scalar.");
            const std::array<double, 3> zero{};
            write(operation, 0,
                  kernel.color
                      ? CompiledValue{core::Color4d{}}
                      : vectorValue(zero, static_cast<std::uint8_t>(kernel.components.size())));
            return;
        }
        components[index] = *value;
    }
    if (kernel.color) {
        write(operation, 0,
              core::Color4d{components[0], components[1], components[2], components[3]});
        return;
    }
    const std::array<double, 3> assembled{components[0], components[1], components[2]};
    write(operation, 0,
          vectorValue(assembled, static_cast<std::uint8_t>(kernel.components.size())));
}

void Evaluator::evaluateRandom(const runtime::CompiledValueOperation& operation,
                               const runtime::CompiledValueRandom& kernel) {
    const auto* seed = asInteger(operandOf(kernel.seed));
    const auto* minimum = asScalar(operandOf(kernel.minimum));
    const auto* maximum = asScalar(operandOf(kernel.maximum));
    if (seed == nullptr || minimum == nullptr || maximum == nullptr) {
        fail(operation, kernel.seed.id, "Random operand has the wrong kind",
             "The seed must reach the kernel as an Integer and the bounds as Scalars.");
        write(operation, 0, 0.0);
        return;
    }
    const double unit = core::primitives::hashUnitValue(*seed);
    const std::array<double, 3> inputs{*minimum, *maximum, unit};
    // The hashed position is in [0, 1), so the result is in the half-open span [min, max) -- the
    // bound convention every sampling API uses, so a seeded value never lands exactly on the
    // maximum.
    const auto outcome = evaluate(Scalar::Mix, inputs, *minimum);
    if (outcome.failed) {
        fail(operation, {}, "Random bounds are outside the representable range",
             "The minimum bound was substituted.");
    }
    write(operation, 0, outcome.value);
}

void Evaluator::evaluateShake(const runtime::CompiledValueOperation& operation,
                              const runtime::CompiledValueShake& kernel) {
    const auto* amplitudeX = asScalar(operandOf(kernel.amplitudeX));
    const auto* amplitudeY = asScalar(operandOf(kernel.amplitudeY));
    const auto* roll = asScalar(operandOf(kernel.roll));
    const auto* frequency = asScalar(operandOf(kernel.frequency));
    const auto* roughness = asScalar(operandOf(kernel.roughness));
    const auto* smoothing = asScalar(operandOf(kernel.smoothing));
    if (amplitudeX == nullptr || amplitudeY == nullptr || roll == nullptr || frequency == nullptr ||
        roughness == nullptr || smoothing == nullptr) {
        fail(operation, kernel.amplitudeX.id, "Camera Shake operand has the wrong kind",
             "The amplitudes, the roll, the frequency, the roughness and the smoothing must "
             "reach the kernel as Scalars.");
        if (kernel.components) {
            write(operation, 0, 0.0);
            write(operation, 1, 0.0);
            write(operation, 2, 0.0);
            return;
        }
        write(operation, 0, document::Vec2d{});
        write(operation, 1, 0.0);
        write(operation, 2, document::Vec2d{1.0, 1.0});
        return;
    }
    const double frameRate = static_cast<double>(rate_.numerator()) /
                             static_cast<double>(rate_.denominator());
    // A driven frequency may leave its domain; the shake needs a positive one, the other two are
    // clamped by the motion itself.
    auto settings = kernel.settings;
    settings.frequency = std::isfinite(*frequency) ? std::clamp(*frequency, 0.05, 30.0) : 1.0;
    settings.roughness = *roughness;
    settings.smoothing = *smoothing;
    // #568: a clone of the shake read at another moment of the frame.
    const double seconds = time_.toSeconds() + kernel.timeOffsetFrames / frameRate;
    const auto sample = core::camera_shake::evaluate(settings, seconds, frameRate);
    if (kernel.components) {
        write(operation, 0, *amplitudeX * sample.x);
        write(operation, 1, *amplitudeY * sample.y);
        write(operation, 2, *roll * sample.roll);
        return;
    }
    const double scale =
        kernel.overscan ? core::camera_shake::fillScale(*amplitudeX, *amplitudeY, *roll,
                                                        kernel.width, kernel.height)
                        : 1.0;
    write(operation, 0, document::Vec2d{*amplitudeX * sample.x, *amplitudeY * sample.y});
    write(operation, 1, *roll * sample.roll);
    write(operation, 2, document::Vec2d{scale, scale});
}

void Evaluator::evaluatePromotion(const runtime::CompiledValueOperation& operation,
                                  const runtime::CompiledValuePromotion& kernel) {
    const auto* value = operandOf(kernel.value);
    const auto reject = [&](const CompiledValue& fallback) {
        fail(operation, kernel.value.id, "Promotion operand has the wrong kind",
             "A promotion widens exactly one kind into exactly one other.");
        write(operation, 0, fallback);
    };
    switch (kernel.promotion) {
    case runtime::ValuePromotion::IntegerToScalar: {
        const auto* integer = asInteger(value);
        if (integer == nullptr) {
            reject(0.0);
            return;
        }
        // Exact for every magnitude a document can hold below 2^53; beyond that the nearest double
        // is the only answer binary64 has, and the widening is still total rather than failing.
        write(operation, 0, static_cast<double>(*integer));
        return;
    }
    case runtime::ValuePromotion::BooleanToInteger: {
        const auto* boolean = asBoolean(value);
        if (boolean == nullptr) {
            reject(std::int64_t{0});
            return;
        }
        write(operation, 0, *boolean ? std::int64_t{1} : std::int64_t{0});
        return;
    }
    case runtime::ValuePromotion::BooleanToScalar: {
        const auto* boolean = asBoolean(value);
        if (boolean == nullptr) {
            reject(0.0);
            return;
        }
        write(operation, 0, *boolean ? 1.0 : 0.0);
        return;
    }
    case runtime::ValuePromotion::ScalarToVector2:
    case runtime::ValuePromotion::ScalarToVector3: {
        const bool wide = kernel.promotion == runtime::ValuePromotion::ScalarToVector3;
        const auto* scalar = asScalar(value);
        if (scalar == nullptr) {
            reject(wide ? CompiledValue{document::Vec3d{}} : CompiledValue{document::Vec2d{}});
            return;
        }
        // Splat: the one value in every component. A vector built from a single number has no other
        // defensible reading.
        if (wide) {
            write(operation, 0, document::Vec3d{*scalar, *scalar, *scalar});
        } else {
            write(operation, 0, document::Vec2d{*scalar, *scalar});
        }
        return;
    }
    }
}

// Task UTIL-1's library. The Evaluator's part is exactly the plan's bookkeeping -- read each
// operand, hand the values to the pure kernel, write what comes back -- and none of the operations
// themselves live here.
void Evaluator::evaluateUtility(const runtime::CompiledValueOperation& operation,
                                const runtime::CompiledValueUtility& kernel) {
    std::vector<const CompiledValue*> operands;
    operands.reserve(kernel.operands.size() + kernel.pathSources.size());
    for (const auto& operand : kernel.operands) {
        operands.push_back(operandOf(operand));
    }
    // #509 Mask Path: each mask's path at this frame, after the operands. A keyed mask that cannot
    // sample is the empty path, diagnosed, as a keyed Path node's is.
    std::vector<CompiledValue> sampledPaths;
    sampledPaths.reserve(kernel.pathSources.size());
    for (const auto& source : kernel.pathSources) {
        if (const auto* constant = std::get_if<runtime::CompiledPathValue>(&source)) {
            sampledPaths.emplace_back(*constant);
            continue;
        }
        auto sampled =
            runtime::samplePathCurve(std::get<runtime::CompiledPathCurve>(source), time_);
        if (!sampled) {
            fail(operation, {}, "Mask path is not evaluable at this time",
                 "A mask's keys could not be sampled; it reads as the empty path.");
            sampledPaths.emplace_back(runtime::CompiledPathValue{});
            continue;
        }
        sampledPaths.emplace_back(
            runtime::CompiledPathValue{std::make_shared<document::PathValue>(std::move(*sampled))});
    }
    for (const auto& sampled : sampledPaths)
        operands.push_back(&sampled);
    const auto outcome = runtime::evaluateValueUtility(
        {kernel.operation,
         operands,
         kernel.selectors,
         time_,
         rate_,
         kernel.dataBlock.has_value() ? &*kernel.dataBlock : nullptr,
         {.maxStringBytes = memoization_.maxStringBytes,
          .availableStringBytes = memoization_.maxRetainedStringBytes - retainedStringBytes_,
          .maxScratchBytes = memoization_.maxStringScratchBytes,
          .cancellation = memoization_.cancellation},
         kernel.textSource.has_value() ? &*kernel.textSource : nullptr});
    if (outcome.cancelled)
        return;
    if (outcome.failed || outcome.notice) {
        const auto& blamed = outcome.failedOperand < kernel.operands.size()
                                 ? kernel.operands[outcome.failedOperand]
                                 : CompiledValueOperand{};
        fail(operation, blamed.id, std::string(outcome.summary), std::string(outcome.detail));
    }
    for (std::size_t slot = 0; slot < outcome.outputCount; ++slot) {
        write(operation, slot, outcome.outputs[slot]);
    }
}

} // namespace

namespace bloom::runtime {

std::optional<std::int64_t> valueGraphFrameIndex(const core::RationalTime time,
                                                 const document::FrameRate rate) noexcept {
    // Reduce across the fraction bar first: it is what keeps an ordinary frame-aligned time at an
    // ordinary rate far away from the overflow check below, rather than relying on the magnitudes
    // happening to be small.
    auto numerator = time.numerator();
    auto rateNumerator = static_cast<std::int64_t>(rate.numerator());
    auto denominator = time.denominator();
    auto rateDenominator = static_cast<std::int64_t>(rate.denominator());
    if (denominator == 0 || rateDenominator == 0) {
        return std::nullopt;
    }
    const auto reduce = [](std::int64_t& left, std::int64_t& right) noexcept {
        const auto magnitude = [](const std::int64_t value) noexcept {
            return value < 0 ? -static_cast<std::uint64_t>(value)
                             : static_cast<std::uint64_t>(value);
        };
        const auto divisor = std::gcd(magnitude(left), magnitude(right));
        if (divisor > 1) {
            left /= static_cast<std::int64_t>(divisor);
            right /= static_cast<std::int64_t>(divisor);
        }
    };
    reduce(numerator, rateDenominator);
    reduce(rateNumerator, denominator);
    const auto scaled = checkedMultiply(numerator, rateNumerator);
    const auto divisor = checkedMultiply(denominator, rateDenominator);
    if (!scaled.has_value() || !divisor.has_value() || *divisor == 0) {
        return std::nullopt;
    }
    return floorDivide(*scaled, *divisor);
}

ValueGraphEvaluation evaluateValueGraph(const std::span<const CompiledValueOperation> operations,
                                        const std::size_t outputCount,
                                        const core::RationalTime time,
                                        const document::FrameRate rate,
                                        const ValueGraphCurves curves,
                                        ValueGraphMemoization memoization) {
    Evaluator evaluator(outputCount, time, rate, curves, memoization);
    evaluator.run(operations);
    return evaluator.release();
}

} // namespace bloom::runtime
