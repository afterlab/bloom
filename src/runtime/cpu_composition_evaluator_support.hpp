#pragma once

#include <bloom/runtime/cpu_composition_evaluator.hpp>

#include <bloom/render/image_types.hpp>
#include <bloom/render/text_raster.hpp>
#include <bloom/runtime/row_band_execution.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime::detail {

template <typename... Functions> struct Overloaded final : Functions... {
    using Functions::operator()...;
};
template <typename... Functions> Overloaded(Functions...) -> Overloaded<Functions...>;

template <typename Value> struct ResolvedCurveSample final {
    Value value;
    document::KeyframeId segmentStart;
};

struct ResolvedEvaluation final {
    render::Rgba32fImageDescriptor imageDescriptor;
    double horizontalScale = 1.0;
    double verticalScale = 1.0;
    std::size_t imageBytes = 0;
    std::size_t peakPixelStorageBytes = 0;
    std::vector<std::size_t> remainingConsumers;
    std::vector<ResolvedCurveSample<double>> scalarCurveValues;
    std::vector<ResolvedCurveSample<document::Vec2d>> vec2CurveValues;
    std::vector<ResolvedCurveSample<core::Color4d>> color4CurveValues;
    // The value graph, already evaluated for this frame (task S7). One entry per plan value output,
    // computed in preflight beside the curve samples and from the SAME request time -- so a Time
    // node and an animation curve in one plan can never disagree about when "now" is.
    std::vector<CompiledValue> valueOutputs;
    // ADR 0024 layer time. clockTimes[k] is clock k's time this request (nullopt = absent content)
    // and clockResolutions[k] its curve samples and value outputs; both are empty for a plan with no
    // retimed layer, and entry 0 is never read (this object is clock 0).
    std::vector<std::optional<core::RationalTime>> clockTimes{};
    std::vector<ResolvedEvaluation> clockResolutions{};
    // #343: the same live-image peak when Merges are folded entry by entry (merge_fold_order.hpp),
    // and whether this frame is evaluated that way: it is when folding holds less and the
    // plan-order peak is more than eight images or more than the request's budget.
    std::size_t foldedPeakPixelStorageBytes = 0;
    bool foldMerges = false;
};

// The resolution an operation's operands use on clock `clock`.
[[nodiscard]] inline const ResolvedEvaluation& resolvedForClock(const ResolvedEvaluation& base,
                                                               const std::uint32_t clock) noexcept {
    return clock == 0 || clock >= base.clockResolutions.size() ? base
                                                               : base.clockResolutions[clock];
}
// Clock `clock`'s time, or nullopt when its content is absent (a None out-of-extent mode).
[[nodiscard]] inline std::optional<core::RationalTime>
timeForClock(const ResolvedEvaluation& base, const core::RationalTime requestTime,
             const std::uint32_t clock) noexcept {
    return clock == 0 || clock >= base.clockTimes.size() ? std::optional{requestTime}
                                                         : base.clockTimes[clock];
}

struct PreflightOutcome final {
    std::optional<ResolvedEvaluation> resolved;
    std::optional<EvaluationDiagnostic> diagnostic;
    bool cancelled = false;

    [[nodiscard]] static PreflightOutcome success(ResolvedEvaluation value) {
        return PreflightOutcome{
            .resolved = std::move(value), .diagnostic = std::nullopt, .cancelled = false};
    }
    [[nodiscard]] static PreflightOutcome failure(EvaluationDiagnostic value) {
        return PreflightOutcome{
            .resolved = std::nullopt, .diagnostic = std::move(value), .cancelled = false};
    }
    [[nodiscard]] static PreflightOutcome cancellation() {
        return PreflightOutcome{
            .resolved = std::nullopt, .diagnostic = std::nullopt, .cancelled = true};
    }
};

// ADR 0024: resolves every layer time clock of `plan` for this request on top of the clock-0
// resolution `base`, attaching clockTimes/clockResolutions. A plan with no clocks is returned as is.
// #484: a time sample's clock time: `parent` posterized (when the sample posterizes), plus its
// offset. #563: a Codec Degrade look-back sample (gopLength) is instead a frame of the `rate` grid
// within the group of pictures `parent` lies in. nullopt when it cannot be represented.
[[nodiscard]] std::optional<core::RationalTime>
offsetTime(core::RationalTime parent, const TimeSampleKernel& sample,
           document::FrameRate rate = document::FrameRate::framesPerSecond24());
[[nodiscard]] PreflightOutcome resolveLayerClocks(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                                                  const EvaluationRequest& request,
                                                  ResolvedEvaluation base,
                                                  const CancellationToken& cancellation,
                                                  OperationCache* cache,
                                                  OperationCacheStatistics* statistics);

// The time of every clock of `plan` at `request.time` (index = clock; nullopt = absent content),
// through the same sampling and resolveLayerClocks the evaluator runs. nullopt when the clocks
// cannot be resolved. Used by worker reads that need an operation's own time (#364 eyedropper).
// #480: `plan`'s curve samples and (pre-image) value outputs at `time`, exactly as one of its
// clocks resolves them, for a channel source reached through a nested composition's mapping.
// nullopt when a curve cannot be sampled; `cancelled` is set on cancellation.
[[nodiscard]] std::optional<ResolvedEvaluation>
sampleParametersAt(const CompiledCompositionPlan& plan, core::RationalTime time,
                   const CancellationToken& cancellation, bool& cancelled);

[[nodiscard]] std::optional<std::vector<std::optional<core::RationalTime>>>
resolveClockTimes(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                  const EvaluationRequest& request, const CancellationToken& cancellation);

[[nodiscard]] EvaluationDiagnostic diagnostic(EvaluationDiagnosticCode code, std::string summary,
                                              std::string detail = {},
                                              EvaluationSubject subject = {});

[[nodiscard]] EvaluationSubject subjectFor(OperationIndex index,
                                           const CompiledOperation& operation);
void reportProgress(const EvaluationProgressCallback& callback,
                    const EvaluationProgress& progress) noexcept;

template <typename Function>
void forEachInput(const CompiledOperation& operation, Function&& function) {
    std::visit(Overloaded{
                   [](const CompiledShape&) {},
                   [](const CompiledSolid&) {},
                   [](const CompiledChecker&) {},
                   [](const CompiledText&) {},
                   [&function](const CompiledImageEffect& effect) {
                       function(effect.input);
                       if (effect.secondaryInput)
                           function(*effect.secondaryInput);
                       if (effect.maskInput)
                           function(*effect.maskInput);
                       // #525: the images a field raster's program reads.
                       for (const auto image : effect.fieldImages)
                           function(image);
                   },
                   [](const CompiledImageSource&) {},
                   [](const CompiledVideoSource&) {},
                   [](const CompiledCompositionSource&) {},
                   [](const CompiledChannelShuffle&) {},
                   [](const CompiledCryptomatte&) {},
                   [&function](const CompiledLayerOutput& layer) {
                       function(layer.input);
                       if (layer.parent)
                           function(*layer.parent);
                   },
                   [&function](const CompiledMerge& stack) {
                       for (const auto& entry : stack.entries) {
                           function(entry.input);
                       }
                   },
                   [&function](const CompiledCompositionOutput& output) { function(output.input); },
               },
               operation);
}

// One output row's worth of glyph coverage, already clipped to the frame: `coverage` is the part of
// the bitmap row that lands inside `window` (empty when none of it does), and `outputOffset` is the
// column in the row span where it starts. Separating the clipping arithmetic from the compositing
// keeps the evaluator's text arm readable and makes the off-frame cases -- text scrolled left of
// the frame, above it, wider than it -- one testable rule instead of four inline branches.
[[nodiscard]] inline ContentBounds boundsForWindow(const render::ImageWindow window,
                                                   const double sx, const double sy) noexcept {
    return {static_cast<double>(window.originX()) / sx, static_cast<double>(window.originY()) / sy,
            static_cast<double>(window.maxXExclusive()) / sx,
            static_cast<double>(window.maxYExclusive()) / sy};
}
// #429: a Composition Source is placed by its nested frame, never by where the nested content
// sits. Its local and output bounds are the nested composition's full-resolution frame, also when
// that composition is empty; the CPU evaluator and the GPU scene preparation publish exactly this.
[[nodiscard]] inline EvaluatedOperationBounds
nestedCompositionFrameBounds(const CompiledCompositionPlan& nested) noexcept {
    const ContentBounds frame{0.0, 0.0, static_cast<double>(nested.format().width()),
                              static_cast<double>(nested.format().height())};
    EvaluatedOperationBounds bounds;
    bounds.local = frame;
    bounds.output = frame;
    return bounds;
}
[[nodiscard]] inline ContentBounds unionBounds(const ContentBounds a,
                                               const ContentBounds b) noexcept {
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right),
            std::max(a.bottom, b.bottom)};
}
[[nodiscard]] inline std::array<document::Vec2d, 4> boundsCorners(const ContentBounds b) noexcept {
    return {{{b.left, b.top}, {b.right, b.top}, {b.right, b.bottom}, {b.left, b.bottom}}};
}
struct ClippedCoverageRow final {
    std::span<const std::uint8_t> coverage;
    std::size_t outputOffset = 0;
};

[[nodiscard]] inline ClippedCoverageRow clipCoverageRow(const render::TextCoverageBitmap& bitmap,
                                                        const render::ImageWindow window,
                                                        const std::int64_t outputY) noexcept {
    if (!bitmap.hasCoverage()) {
        return {};
    }
    // The text origin is the window's own origin, so a bitmap coordinate is a window coordinate
    // plus the bitmap's origin offset.
    const auto bitmapRow = outputY - bitmap.originY();
    if (bitmapRow < 0 || bitmapRow >= static_cast<std::int64_t>(bitmap.height())) {
        return {};
    }
    const auto row = bitmap.row(static_cast<std::uint32_t>(bitmapRow));
    if (row.empty()) {
        return {};
    }
    const auto width = window.maxXExclusive();
    const auto firstColumn = std::max<std::int64_t>(bitmap.originX(), window.originX());
    const auto lastColumnExclusive =
        std::min<std::int64_t>(bitmap.originX() + static_cast<std::int64_t>(bitmap.width()), width);
    if (lastColumnExclusive <= firstColumn) {
        return {};
    }
    const auto skipped = static_cast<std::size_t>(firstColumn - bitmap.originX());
    const auto count = static_cast<std::size_t>(lastColumnExclusive - firstColumn);
    return {row.subspan(skipped, count), static_cast<std::size_t>(firstColumn - window.originX())};
}

[[nodiscard]] EvaluationDiagnostic imageDiagnostic(const render::ImageError& error,
                                                   EvaluationSubject subject, std::string summary);
[[nodiscard]] inline bool checkedAdd(const std::size_t lhs, const std::size_t rhs,
                                     std::size_t& result) noexcept {
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

[[nodiscard]] inline std::optional<std::uint64_t>
checkedProduct(const std::span<const std::uint64_t> factors) noexcept {
    std::uint64_t result = 1;
    for (const auto factor : factors) {
        if (factor != 0 && result > std::numeric_limits<std::uint64_t>::max() / factor) {
            return std::nullopt;
        }
        result *= factor;
    }
    return result;
}
[[nodiscard]] std::optional<core::PixelAspectRatio>
proxyPixelAspect(const document::CompositionFormat& format,
                 render::ImageExtent proxyExtent) noexcept;
[[nodiscard]] bool isImageProducing(const CompiledOperation& operation) noexcept;
[[nodiscard]] bool hasExpectedInputKinds(const CompiledCompositionPlan& plan, std::size_t index,
                                         EvaluationDiagnostic& failure);
[[nodiscard]] PreflightOutcome preflight(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                                         const EvaluationRequest& request,
                                         const CancellationToken& cancellation,
                                         const EvaluationProgressCallback& progress);

// The REAL six-argument preflight (the definition the evaluator calls). Declared here so the
// shared GPU scene preparation can reuse the exact resolution the CPU evaluator performs,
// with optional cache/statistics. The four-argument declaration above has no definition.
[[nodiscard]] PreflightOutcome preflight(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                                         const EvaluationRequest& request,
                                         const CancellationToken& cancellation,
                                         const EvaluationProgressCallback& progress,
                                         OperationCache* cache,
                                         OperationCacheStatistics* statistics);
[[nodiscard]] EvaluationResult unexpectedAllocationFailure();

} // namespace bloom::runtime::detail
