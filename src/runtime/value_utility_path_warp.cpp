// #598 Warp Path: a path moved by a non-linear function of its points, subdivided so that straight
// and curved segments bend (docs/architecture/path-processing.md). The arithmetic is path_warp.cpp's,
// shared with the matrix of Transform Path; this is the node: reading its operands, building the
// space the function runs in, and refusing with a diagnostic that names what was asked.

#include "path_warp.hpp"
#include "value_utility_path_support.hpp"

#include <bloom/document/parameter.hpp>
#include <bloom/document/warp_path_nodes.hpp>

#include <cstdint>
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
[[nodiscard]] ValueUtilityOutcome refusedWith(ValueUtilityOutcome outcome,
                                              const pathwarp::Refusal& refusal) {
    thread_local std::string summary;
    thread_local std::string detail;
    summary = refusal.summary;
    detail = refusal.detail;
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
    if (const auto refusal = runtime::pathwarp::checkOperands(amount, size, phase, tolerance,
                                                              position, anchor, scale, rotation,
                                                              skew, skewAxis))
        return refusedWith(std::move(outcome), *refusal);
    if (!coordinatesUsable(path))
        return refuse(std::move(outcome), "Warp Path path is out of range",
                      "Every coordinate must be finite and within a billion pixels.");

    const auto matrix =
        runtime::pathwarp::Matrix::make(position, anchor, scale, rotation, skew, skewAxis);
    const runtime::pathwarp::Warp warp{function, amount, size, phase};
    // The function runs in the matrix's space: into it, through the function, and back out.
    const runtime::pathwarp::PointMap map = [&](const document::Vec2d point) {
        return std::optional<document::Vec2d>(
            matrix.forward(runtime::pathwarp::apply(warp, matrix.inverse(point))));
    };
    auto warped = runtime::pathwarp::warpPath(path, map, tolerance, invocation.limits.cancellation);
    switch (warped.status) {
    case runtime::pathwarp::Status::Ok:
        break;
    case runtime::pathwarp::Status::Cancelled:
        outcome.cancelled = true;
        return shapedOutcome(invocation.operation);
    case runtime::pathwarp::Status::Unusable:
    case runtime::pathwarp::Status::TooMany:
        return refusedWith(shapedOutcome(invocation.operation),
                           runtime::pathwarp::describeFailure(warped, "Warp Path",
                                                              functionName(function), tolerance));
    }
    const bool depthReached = warped.depthReached;
    publish(outcome, std::move(warped.value));
    if (depthReached) {
        const auto notice = runtime::pathwarp::depthNotice();
        thread_local std::string summary;
        thread_local std::string detail;
        summary = notice.summary;
        detail = notice.detail;
        outcome.notice = true;
        outcome.summary = summary;
        outcome.detail = detail;
    }
    return outcome;
}

} // namespace bloom::runtime::detail
