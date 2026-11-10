// #637 Resample Path: a path with more or fewer points, spaced along each contour
// (docs/architecture/path-processing.md). The arithmetic is path_resample.cpp's, shared with the
// warp and the morph; this is the node: reading its operands, choosing each contour's request from
// the mode, and refusing with a diagnostic that names what was asked.

#include "path_resample.hpp"
#include "value_utility_path_support.hpp"

#include <bloom/document/parameter.hpp>
#include <bloom/document/resample_path_nodes.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace bloom::runtime::detail {
namespace {

using document::ResampleMode;
using document::ResampleOutput;

[[nodiscard]] std::string_view modeName(const ResampleMode mode) noexcept {
    switch (mode) {
    case ResampleMode::FixedCount:
        return "Fixed count";
    case ResampleMode::Gap:
        return "Gap";
    case ResampleMode::ByLength:
        return "By length";
    case ResampleMode::Percentage:
        return "Percentage";
    }
    return "Fixed count";
}

// A diagnostic's text is built per failure and kept until the evaluator has copied it (it does so
// as soon as the kernel returns, on the same thread).
[[nodiscard]] ValueUtilityOutcome tooMany(ValueUtilityOutcome outcome, const ResampleMode mode,
                                          const double requested, const std::size_t contour,
                                          const std::size_t limit, const bool wholeValue) {
    thread_local std::string summary;
    thread_local std::string detail;
    summary = wholeValue ? "Resample Path result is too large" : "Resample Path asks for too many points";
    char buffer[384];
    if (wholeValue)
        std::snprintf(buffer, sizeof buffer,
                      "Resample Path, mode %s: the contours so far hold %.0f points and a path holds "
                      "at most %zu. Lower the count, widen the gap or lower the density.",
                      std::string(modeName(mode)).c_str(), requested, limit);
    else
        std::snprintf(buffer, sizeof buffer,
                      "Resample Path, mode %s: contour %zu would hold %.0f points and a contour "
                      "holds at most %zu. Lower the count, widen the gap or lower the density.",
                      std::string(modeName(mode)).c_str(), contour, requested, limit);
    detail = buffer;
    outcome.failed = true;
    outcome.summary = summary;
    outcome.detail = detail;
    return outcome;
}

} // namespace

ValueUtilityOutcome evaluateValuePathResample(const ValueUtilityInvocation& invocation) {
    using namespace pathsupport;
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto& path = reader.path(0);
    const std::int64_t count = reader.integer(1);
    const double gapOperand = reader.scalar(2);
    const double density = reader.scalar(3);
    const double percentage = reader.scalar(4);
    const bool keepCorners = reader.boolean(5);
    const double startOffset = reader.scalar(6);
    const auto mode = selectorAt(invocation, 0, document::kResampleModes, ResampleMode::FixedCount);
    const auto output = selectorAt(invocation, 1, document::kResampleOutputs, ResampleOutput::Curves);
    if (reader.malformed())
        return outcome;
    if (!std::isfinite(gapOperand) || !std::isfinite(density) || !std::isfinite(percentage) ||
        !std::isfinite(startOffset))
        return refuse(std::move(outcome), "Resample Path operand is not finite",
                      "The gap, density, percentage and start offset must be finite numbers.");
    if (!coordinatesUsable(path))
        return refuse(std::move(outcome), "Resample Path path is out of range",
                      "Every coordinate must be finite and within a billion pixels.");

    double gap = gapOperand;
    switch (mode) {
    case ResampleMode::FixedCount:
        if (count < 0)
            return refuse(std::move(outcome), "Resample Path count is negative",
                          "Mode Fixed count needs a count of 2 or more.");
        break;
    case ResampleMode::Gap:
        if (!(gap > 0.0))
            return refuse(std::move(outcome), "Resample Path gap is not above zero",
                          "Mode Gap needs a gap above 0 pixels.");
        break;
    case ResampleMode::ByLength:
        if (!(density > 0.0))
            return refuse(std::move(outcome), "Resample Path density is not above zero",
                          "Mode By length needs more than 0 points per 100 pixels.");
        gap = 100.0 / density;
        break;
    case ResampleMode::Percentage:
        if (percentage < 0.0)
            return refuse(std::move(outcome), "Resample Path percentage is negative",
                          "Mode Percentage needs a percentage of 0 or more.");
        break;
    }

    document::PathValue result = path;
    const auto* cancellation = invocation.limits.cancellation;
    std::size_t total = 0;
    bool degenerate = false;
    for (std::size_t index = 0; index < path.contours.size(); ++index) {
        // Cancellation is observed between contours; a cancelled evaluation keeps nothing.
        if (cancellation != nullptr && cancellation->isCancellationRequested()) {
            outcome.cancelled = true;
            return shapedOutcome(invocation.operation);
        }
        const auto& contour = path.contours[index];
        pathresample::ContourRequest request;
        request.keepCorners = keepCorners;
        request.curves = output == ResampleOutput::Curves;
        // A closed contour wraps the offset; an open one clamps it to its length.
        const double share = startOffset / 100.0;
        request.phase = contour.closed ? share - std::floor(share) : std::clamp(share, 0.0, 1.0);
        switch (mode) {
        case ResampleMode::FixedCount:
            request.count = count;
            break;
        case ResampleMode::Percentage: {
            const double wanted =
                std::round(static_cast<double>(contour.anchors.size()) * percentage / 100.0);
            request.count = wanted >= 1.0e15 ? std::int64_t{1000000000000000}
                                             : static_cast<std::int64_t>(wanted);
            break;
        }
        case ResampleMode::Gap:
        case ResampleMode::ByLength:
            request.gap = gap;
            break;
        }
        auto resampled =
            pathresample::resampleContour(contour, request, document::kMaximumPathAnchors);
        if (resampled.status == pathresample::ResampleStatus::TooMany)
            return tooMany(shapedOutcome(invocation.operation), mode, resampled.requested, index,
                           document::kMaximumPathAnchors, false);
        degenerate = degenerate || resampled.degenerate;
        total += resampled.contour.anchors.size();
        if (total > document::kMaximumPathValueAnchors)
            return tooMany(shapedOutcome(invocation.operation), mode, static_cast<double>(total),
                           index, document::kMaximumPathValueAnchors, true);
        result.contours[index] = std::move(resampled.contour);
    }
    publish(outcome, std::move(result));
    if (degenerate) {
        outcome.notice = true;
        outcome.summary = "Resample Path found a contour with no length";
        outcome.detail = "A contour of length zero has no place to put points; Fixed count answers "
                         "that many copies of its point.";
    }
    return outcome;
}

} // namespace bloom::runtime::detail
