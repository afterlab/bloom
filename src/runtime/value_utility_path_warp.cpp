// #598 Warp Path: a path moved by a non-linear function of its points, subdivided so that straight
// and curved segments bend (docs/architecture/path-processing.md). The arithmetic is path_warp.cpp's,
// shared with the matrix of Transform Path; this is the node: reading its operands, building the
// space the function runs in, and refusing with a diagnostic that names what was asked.

#include "path_warp.hpp"
#include "value_utility_path_support.hpp"

#include <bloom/document/parameter.hpp>
#include <bloom/document/warp_path_nodes.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace bloom::runtime::detail {
namespace {

using document::WarpFunction;

[[nodiscard]] std::string_view functionName(const WarpFunction function) noexcept {
    switch (function) {
    case WarpFunction::Wave:
        return "Wave";
    case WarpFunction::Twist:
        return "Twist";
    case WarpFunction::Bulge:
        return "Bulge / Pinch";
    case WarpFunction::Bend:
        return "Bend";
    case WarpFunction::Ripple:
        return "Ripple";
    }
    return "Wave";
}

// A diagnostic's text is built per failure and kept until the evaluator has copied it (it does so
// as soon as the kernel returns, on the same thread).
[[nodiscard]] ValueUtilityOutcome tooMany(ValueUtilityOutcome outcome, const WarpFunction function,
                                          const double tolerance, const std::size_t contour,
                                          const std::size_t needed, const std::size_t limit) {
    thread_local std::string summary;
    thread_local std::string detail;
    summary = "Warp Path needs too many points";
    char buffer[384];
    std::snprintf(buffer, sizeof buffer,
                  "Warp Path, function %s: contour %zu needs at least %zu points to stay within "
                  "%g px of the warp and a contour holds at most %zu. Raise the tolerance, lower "
                  "the amount or widen the size.",
                  std::string(functionName(function)).c_str(), contour, needed, tolerance, limit);
    detail = buffer;
    outcome.failed = true;
    outcome.summary = summary;
    outcome.detail = detail;
    return outcome;
}

} // namespace

ValueUtilityOutcome evaluateValuePathWarp(const ValueUtilityInvocation& invocation) {
    using namespace pathsupport;
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto& path = reader.path(0);
    const double amount = reader.scalar(1);
    const double size = reader.scalar(2);
    const double phase = reader.scalar(3);
    const double tolerance = reader.scalar(4);
    const auto position = reader.vector2(5);
    const auto anchor = reader.vector2(6);
    const auto scale = reader.vector2(7);
    const double rotation = reader.scalar(8);
    const double skew = reader.scalar(9);
    const double skewAxis = reader.scalar(10);
    const auto function = selectorAt(invocation, 0, document::kWarpFunctions, WarpFunction::Wave);
    if (reader.malformed())
        return outcome;
    if (!std::isfinite(amount) || !std::isfinite(size) || !std::isfinite(phase) ||
        !std::isfinite(rotation) || !std::isfinite(skew) || !std::isfinite(skewAxis) ||
        !finite(position) || !finite(anchor) || !finite(scale))
        return refuse(std::move(outcome), "Warp Path operand is not finite",
                      "The amount, size, phase, position, anchor, scale, rotation, skew and skew "
                      "axis must be finite numbers.");
    if (!(size > 0.0))
        return refuse(std::move(outcome), "Warp Path size is not above zero",
                      "The size is the width the function works over and must be above 0 pixels.");
    if (!toleranceUsable(tolerance))
        return refuse(std::move(outcome), "Warp Path tolerance is out of range",
                      "The tolerance must be between 0.001 and 1000 pixels.");
    if (std::abs(skew) > document::kMaximumPathSkewDegrees)
        return refuse(std::move(outcome), "Warp Path skew is out of range",
                      "The skew must be within 89.9 degrees either way: a quarter turn is no "
                      "slant.");
    const auto matrix =
        runtime::pathwarp::Matrix::make(position, anchor, scale, rotation, skew, skewAxis);
    if (!matrix.invertible())
        return refuse(std::move(outcome), "Warp Path scale is zero",
                      "The warp runs in the space the scale describes, and a scale of zero "
                      "flattens it.");
    if (!coordinatesUsable(path))
        return refuse(std::move(outcome), "Warp Path path is out of range",
                      "Every coordinate must be finite and within a billion pixels.");

    const runtime::pathwarp::Warp warp{function, amount, size, phase};
    // The function runs in the matrix's space: into it, through the function, and back out.
    const runtime::pathwarp::PointMap map = [&](const document::Vec2d point) {
        return std::optional<document::Vec2d>(
            matrix.forward(runtime::pathwarp::apply(warp, matrix.inverse(point))));
    };

    document::PathValue result = path;
    const auto* cancellation = invocation.limits.cancellation;
    std::size_t total = 0;
    bool depthReached = false;
    for (std::size_t index = 0; index < path.contours.size(); ++index) {
        // Cancellation is observed between contours (and inside a long one); a cancelled
        // evaluation keeps nothing.
        if (cancellation != nullptr && cancellation->isCancellationRequested()) {
            outcome.cancelled = true;
            return shapedOutcome(invocation.operation);
        }
        runtime::pathwarp::Limits limits;
        limits.tolerance = tolerance;
        limits.maximumPoints = document::kMaximumPathAnchors;
        limits.remainingPoints = document::kMaximumPathValueAnchors - total;
        limits.cancellation = cancellation;
        auto warped = runtime::pathwarp::warpContour(path.contours[index], map, limits);
        switch (warped.status) {
        case runtime::pathwarp::Status::Ok:
            break;
        case runtime::pathwarp::Status::Cancelled:
            outcome.cancelled = true;
            return shapedOutcome(invocation.operation);
        case runtime::pathwarp::Status::Unusable:
            return refuse(shapedOutcome(invocation.operation), "Warp Path has no answer here",
                          "The warp gave a point that is not a finite number a path can hold. "
                          "Lower the amount or change the size.");
        case runtime::pathwarp::Status::TooMany: {
            const bool wholeValue = warped.requested + total > document::kMaximumPathValueAnchors &&
                                    warped.requested <= document::kMaximumPathAnchors;
            return tooMany(shapedOutcome(invocation.operation), function, tolerance, index,
                           wholeValue ? warped.requested + total : warped.requested,
                           wholeValue ? document::kMaximumPathValueAnchors
                                      : document::kMaximumPathAnchors);
        }
        }
        depthReached = depthReached || warped.depthReached;
        total += warped.contour.anchors.size();
        result.contours[index] = std::move(warped.contour);
    }
    publish(outcome, std::move(result));
    if (depthReached) {
        outcome.notice = true;
        outcome.summary = "Warp Path stopped splitting at its depth limit";
        outcome.detail = "Near a place where the warp jumps, a segment was split as far as it "
                         "goes and the curve there may stray from the warp by more than the "
                         "tolerance.";
    }
    return outcome;
}

} // namespace bloom::runtime::detail
