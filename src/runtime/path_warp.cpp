#include "path_warp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace bloom::runtime::pathwarp {
namespace {

using document::PathAnchor;
using document::PathContour;
using document::Vec2d;

constexpr double kRadiansPerDegree = std::numbers::pi / 180.0;
// The step, in a segment's own parameter, of the tangent's central difference.
constexpr double kTangentStep = 1.0e-4;

[[nodiscard]] bool finite(const Vec2d value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::abs(value.x) <= document::kMaximumPathCoordinate &&
           std::abs(value.y) <= document::kMaximumPathCoordinate;
}

// One segment of the original contour as a cubic. A segment with no handle at either end is a
// straight line with an even parameter, so the knots that split it are evenly spaced.
struct Segment final {
    std::array<Vec2d, 4> c;
    bool straight = false;

    [[nodiscard]] Vec2d at(const double t) const noexcept {
        if (straight)
            return {(1.0 - t) * c[0].x + t * c[3].x, (1.0 - t) * c[0].y + t * c[3].y};
        const double u = 1.0 - t;
        const double b0 = u * u * u, b1 = 3.0 * u * u * t, b2 = 3.0 * u * t * t, b3 = t * t * t;
        return {b0 * c[0].x + b1 * c[1].x + b2 * c[2].x + b3 * c[3].x,
                b0 * c[0].y + b1 * c[1].y + b2 * c[2].y + b3 * c[3].y};
    }
};

[[nodiscard]] Segment segmentBetween(const PathAnchor& from, const PathAnchor& to) noexcept {
    const bool noOut = !from.outHandle || *from.outHandle == from.point;
    const bool noIn = !to.inHandle || *to.inHandle == to.point;
    Segment segment;
    segment.straight = noOut && noIn;
    segment.c = {from.point, from.outHandle.value_or(from.point), to.inHandle.value_or(to.point),
                 to.point};
    return segment;
}

// A knot: a parameter on the segment, where the warp puts it and the warp's tangent there.
struct Knot final {
    double t = 0.0;
    Vec2d point;
    Vec2d tangent;
};

class Walker final {
  public:
    Walker(const PointMap& map, const Limits& limits) : map_(map), limits_(limits) {}

    // The warped curve's point at parameter t of the segment.
    [[nodiscard]] std::optional<Vec2d> sample(const Segment& segment, const double t) {
        const auto warped = map_(segment.at(t));
        if (!warped || !finite(*warped))
            return std::nullopt;
        return warped;
    }

    // The tangent by a central difference. The cubic continues past its ends, so one at an end of
    // the segment reads the segment's own direction, not its neighbour's.
    [[nodiscard]] std::optional<Knot> knot(const Segment& segment, const double t,
                                           const Vec2d known, const bool haveKnown) {
        Knot result;
        result.t = t;
        if (haveKnown) {
            result.point = known;
        } else {
            const auto here = sample(segment, t);
            if (!here)
                return std::nullopt;
            result.point = *here;
        }
        const auto before = sample(segment, t - kTangentStep);
        const auto after = sample(segment, t + kTangentStep);
        if (!before || !after)
            return std::nullopt;
        result.tangent = {(after->x - before->x) / (2.0 * kTangentStep),
                          (after->y - before->y) / (2.0 * kTangentStep)};
        // A tangent is a rate, not a place: it only has to be a number.
        if (!std::isfinite(result.tangent.x) || !std::isfinite(result.tangent.y))
            return std::nullopt;
        return result;
    }

    // The largest distance between the cubic through two knots and the exact warp at three
    // parameters inside it.
    [[nodiscard]] std::optional<double> deviation(const Segment& segment, const Knot& from,
                                                  const Knot& to, Vec2d& middle) {
        const double span = to.t - from.t;
        const Vec2d p0 = from.point;
        const Vec2d p1{from.point.x + from.tangent.x * span / 3.0,
                       from.point.y + from.tangent.y * span / 3.0};
        const Vec2d p2{to.point.x - to.tangent.x * span / 3.0,
                       to.point.y - to.tangent.y * span / 3.0};
        const Vec2d p3 = to.point;
        double worst = 0.0;
        for (const double s : {0.25, 0.5, 0.75}) {
            const auto exact = sample(segment, from.t + span * s);
            if (!exact)
                return std::nullopt;
            if (s == 0.5)
                middle = *exact;
            const double u = 1.0 - s;
            const double b0 = u * u * u, b1 = 3.0 * u * u * s, b2 = 3.0 * u * s * s, b3 = s * s * s;
            const Vec2d drawn{b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x,
                              b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y};
            worst = std::max(worst, std::hypot(drawn.x - exact->x, drawn.y - exact->y));
        }
        return worst;
    }

    [[nodiscard]] bool cancelled() {
        return limits_.cancellation != nullptr && ((++polls_ & 63U) == 0U) &&
               limits_.cancellation->isCancellationRequested();
    }

    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }

  private:
    const PointMap& map_;
    const Limits& limits_;
    std::uint32_t polls_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------------------------
// The matrix
// ---------------------------------------------------------------------------------------------

Matrix Matrix::make(const Vec2d position, const Vec2d anchor, const Vec2d scale,
                    const double rotationDegrees, const double skewDegrees,
                    const double skewAxisDegrees) noexcept {
    Matrix matrix;
    matrix.position = position;
    matrix.anchor = anchor;
    matrix.scale = scale;
    const double radians = rotationDegrees * std::numbers::pi / 180.0;
    matrix.cosine = std::cos(radians);
    matrix.sine = std::sin(radians);
    matrix.skewed = skewDegrees != 0.0;
    if (matrix.skewed) {
        matrix.slant = std::tan(skewDegrees * kRadiansPerDegree);
        const double axis = skewAxisDegrees * kRadiansPerDegree;
        matrix.axis = {std::cos(axis), std::sin(axis)};
    }
    return matrix;
}

bool Matrix::invertible() const noexcept { return scale.x != 0.0 && scale.y != 0.0; }

Vec2d Matrix::forward(const Vec2d point) const noexcept {
    double dx = (point.x - anchor.x) * scale.x;
    double dy = (point.y - anchor.y) * scale.y;
    if (skewed) {
        // Slide along the axis by the slant times the distance across it.
        const double across = -dx * axis.y + dy * axis.x;
        dx += slant * across * axis.x;
        dy += slant * across * axis.y;
    }
    return {position.x + dx * cosine - dy * sine, position.y + dx * sine + dy * cosine};
}

Vec2d Matrix::inverse(const Vec2d point) const noexcept {
    const double ox = point.x - position.x;
    const double oy = point.y - position.y;
    double dx = ox * cosine + oy * sine;
    double dy = -ox * sine + oy * cosine;
    if (skewed) {
        const double across = -dx * axis.y + dy * axis.x;
        dx -= slant * across * axis.x;
        dy -= slant * across * axis.y;
    }
    return {dx / scale.x + anchor.x, dy / scale.y + anchor.y};
}

// ---------------------------------------------------------------------------------------------
// The built-in warps
// ---------------------------------------------------------------------------------------------

namespace {

// sin(t) / t, whole through zero.
[[nodiscard]] double sinc(const double t) noexcept {
    return std::abs(t) < 1.0e-8 ? 1.0 - t * t / 6.0 : std::sin(t) / t;
}
// (1 - cos(t)) / t, whole through zero.
[[nodiscard]] double versineOver(const double t) noexcept {
    if (std::abs(t) < 1.0e-8)
        return t / 2.0;
    const double half = std::sin(t / 2.0);
    return 2.0 * half * half / t;
}

} // namespace

Vec2d apply(const Warp& warp, const Vec2d point) noexcept {
    const double x = point.x;
    const double y = point.y;
    const double phase = warp.phase * kRadiansPerDegree;
    const double tau = 2.0 * std::numbers::pi;
    switch (warp.function) {
    case document::WarpFunction::Wave:
        // Vertical lines become waves: x moves by a sine of y.
        return {x + warp.amount * std::sin(tau * y / warp.size + phase), y};
    case document::WarpFunction::Twist: {
        // The plane turns about the origin, by the amount in degrees at the middle, fading with the
        // distance (a Gaussian of width `size`).
        const double fade = std::exp(-(x * x + y * y) / (warp.size * warp.size));
        const double angle = warp.amount * kRadiansPerDegree * fade;
        const double c = std::cos(angle);
        const double s = std::sin(angle);
        return {x * c - y * s, x * s + y * c};
    }
    case document::WarpFunction::Bulge: {
        // Points move out from (positive) or in toward (negative) the origin by `amount` percent at
        // the middle, fading with the distance.
        const double fade = std::exp(-(x * x + y * y) / (warp.size * warp.size));
        const double factor = 1.0 + warp.amount / 100.0 * fade;
        return {x * factor, y * factor};
    }
    case document::WarpFunction::Bend: {
        // The x axis curls into an arc that turns `amount` degrees over a length of `size`.
        const double curvature = warp.amount * kRadiansPerDegree / warp.size;
        const double t = curvature * x;
        return {x * sinc(t) - y * std::sin(t), y * std::cos(t) + x * versineOver(t)};
    }
    case document::WarpFunction::Ripple: {
        const double radius = std::hypot(x, y);
        if (!(radius > 0.0))
            return point;
        const double move = warp.amount * std::sin(tau * radius / warp.size + phase);
        return {x + move * x / radius, y + move * y / radius};
    }
    }
    return point;
}

// ---------------------------------------------------------------------------------------------
// The adaptive subdivision
// ---------------------------------------------------------------------------------------------

Result warpContour(const PathContour& contour, const PointMap& map, const Limits& limits) {
    Result result;
    result.contour.closed = contour.closed;
    const auto count = contour.anchors.size();
    if (count == 0)
        return result;
    const std::size_t maximum = std::min(limits.maximumPoints, limits.remainingPoints);
    Walker walker(map, limits);
    auto fail = [&](const Status status) {
        result.status = status;
        result.contour.anchors.clear();
        return result;
    };

    // A lone anchor has no segment: it is mapped as a point.
    if (count == 1) {
        const auto each = map(contour.anchors[0].point);
        if (!each || !finite(*each))
            return fail(Status::Unusable);
        result.contour.anchors.push_back(
            {*each, std::nullopt, std::nullopt, document::PathAnchorMode::Corner});
        return result;
    }

    const std::size_t segments = contour.closed ? count : count - 1;
    // The knots of every segment, kept apart until the handles are known.
    struct Piece final {
        std::vector<Knot> knots;
    };
    std::vector<Piece> pieces(segments);
    std::size_t total = 0;
    for (std::size_t index = 0; index < segments; ++index) {
        const auto& from = contour.anchors[index];
        const auto& to = contour.anchors[(index + 1) % count];
        const Segment segment = segmentBetween(from, to);
        auto start = walker.knot(segment, 0.0, {}, false);
        auto end = walker.knot(segment, 1.0, {}, false);
        if (!start || !end)
            return fail(Status::Unusable);
        // Depth-first, so the knots come out in order. Each entry is an interval still to settle.
        struct Interval final {
            Knot from, to;
            int depth = 0;
        };
        std::vector<Interval> stack;
        stack.push_back({*start, *end, 0});
        auto& knots = pieces[index].knots;
        knots.push_back(*start);
        while (!stack.empty()) {
            if (walker.cancelled())
                return fail(Status::Cancelled);
            const Interval interval = stack.back();
            stack.pop_back();
            Vec2d middle{};
            const auto worst = walker.deviation(segment, interval.from, interval.to, middle);
            if (!worst)
                return fail(Status::Unusable);
            const bool settled = *worst <= limits.tolerance;
            if (settled || interval.depth >= document::kMaximumWarpDepth) {
                if (!settled)
                    result.depthReached = true;
                knots.push_back(interval.to);
                continue;
            }
            // Every stretch still to settle ends in at least one more point, and this one splits
            // into two: a floor on the contour's points. Past the budget it is too many, and the
            // walk stops before it allocates more.
            const std::size_t atLeast = total + (knots.size() - 1) + stack.size() + 2 +
                                        (segments - index - 1) + (contour.closed ? 0 : 1);
            if (atLeast > maximum) {
                result.requested = atLeast;
                return fail(Status::TooMany);
            }
            const double centre = (interval.from.t + interval.to.t) / 2.0;
            const auto split = walker.knot(segment, centre, middle, true);
            if (!split)
                return fail(Status::Unusable);
            // Push the later half first so the earlier half settles first.
            stack.push_back({*split, interval.to, interval.depth + 1});
            stack.push_back({interval.from, *split, interval.depth + 1});
        }
        total += knots.size() - 1;
    }

    // The anchors: a knot is shared by the two intervals around it, whose handles come from the
    // length of each side. A segment's last knot is the next segment's first, so it is written once
    // with the incoming handle from this one and the outgoing from the next.
    const std::size_t anchors = total + (contour.closed ? 0 : 1);
    if (anchors > maximum) {
        result.requested = anchors;
        return fail(Status::TooMany);
    }
    result.contour.anchors.reserve(anchors);
    const auto handle = [](const Knot& knot, const double span, const double sign) {
        return Vec2d{knot.point.x + sign * knot.tangent.x * span / 3.0,
                     knot.point.y + sign * knot.tangent.y * span / 3.0};
    };
    std::optional<Vec2d> carriedIn;
    for (std::size_t index = 0; index < segments; ++index) {
        const auto& knots = pieces[index].knots;
        for (std::size_t at = 0; at + 1 < knots.size(); ++at) {
            const double span = knots[at + 1].t - knots[at].t;
            PathAnchor anchor;
            anchor.point = knots[at].point;
            anchor.mode = document::PathAnchorMode::Free;
            anchor.outHandle = handle(knots[at], span, 1.0);
            if (at == 0) {
                // The incoming handle of an original anchor comes from the segment before.
                anchor.inHandle = carriedIn;
            } else {
                const double before = knots[at].t - knots[at - 1].t;
                anchor.inHandle = handle(knots[at], before, -1.0);
            }
            result.contour.anchors.push_back(anchor);
        }
        const double before = knots.size() >= 2 ? knots.back().t - knots[knots.size() - 2].t : 0.0;
        carriedIn = handle(knots.back(), before, -1.0);
    }
    if (contour.closed) {
        // The first anchor takes the last segment's closing handle.
        result.contour.anchors.front().inHandle = carriedIn;
    } else {
        PathAnchor last;
        last.point = pieces.back().knots.back().point;
        last.mode = document::PathAnchorMode::Free;
        last.inHandle = carriedIn;
        result.contour.anchors.push_back(last);
    }
    // A handle that lies on its anchor draws nothing: leave it out.
    for (auto& anchor : result.contour.anchors) {
        if (anchor.inHandle && *anchor.inHandle == anchor.point)
            anchor.inHandle.reset();
        if (anchor.outHandle && *anchor.outHandle == anchor.point)
            anchor.outHandle.reset();
        if (!anchor.inHandle && !anchor.outHandle)
            anchor.mode = document::PathAnchorMode::Corner;
    }
    for (const auto& anchor : result.contour.anchors)
        if (!finite(anchor.point) || (anchor.inHandle && !finite(*anchor.inHandle)) ||
            (anchor.outHandle && !finite(*anchor.outHandle)))
            return fail(Status::Unusable);
    return result;
}

} // namespace bloom::runtime::pathwarp
