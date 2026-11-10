#include "path_resample.hpp"

#include <bloom/document/path_value.hpp>
#include <bloom/render/path_raster.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>

namespace bloom::runtime::pathresample {
namespace {

using document::PathAnchor;
using document::PathContour;
using document::Vec2d;

constexpr std::size_t kLengthSamples = 64;
// Two distances closer than this are the same place.
constexpr double kEpsilon = 1.0e-9;
// The turn at an anchor, as the cosine of half a degree, past which it is a corner.
constexpr double kCornerCosine = 0.99996192306417128;

[[nodiscard]] Vec2d lerp(const Vec2d a, const Vec2d b, const double t) noexcept {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}
[[nodiscard]] double distance(const Vec2d a, const Vec2d b) noexcept {
    return std::hypot(b.x - a.x, b.y - a.y);
}
[[nodiscard]] double dot(const Vec2d a, const Vec2d b) noexcept { return a.x * b.x + a.y * b.y; }

[[nodiscard]] Vec2d evaluate(const std::array<Vec2d, 4>& c, const double t) noexcept {
    const auto p01 = lerp(c[0], c[1], t);
    const auto p12 = lerp(c[1], c[2], t);
    const auto p23 = lerp(c[2], c[3], t);
    return lerp(lerp(p01, p12, t), lerp(p12, p23, t), t);
}

// The sub-curve of a cubic from parameter t0 to t1, by de Casteljau (the shape is unchanged).
[[nodiscard]] std::array<Vec2d, 4> subcurve(const std::array<Vec2d, 4>& c, const double t0,
                                            const double t1) noexcept {
    const auto left = [](const std::array<Vec2d, 4>& k, const double t) {
        const auto p01 = lerp(k[0], k[1], t);
        const auto p12 = lerp(k[1], k[2], t);
        const auto p23 = lerp(k[2], k[3], t);
        const auto p012 = lerp(p01, p12, t);
        const auto p123 = lerp(p12, p23, t);
        return std::array<Vec2d, 4>{k[0], p01, p012, lerp(p012, p123, t)};
    };
    const auto right = [](const std::array<Vec2d, 4>& k, const double t) {
        const auto p01 = lerp(k[0], k[1], t);
        const auto p12 = lerp(k[1], k[2], t);
        const auto p23 = lerp(k[2], k[3], t);
        const auto p012 = lerp(p01, p12, t);
        const auto p123 = lerp(p12, p23, t);
        return std::array<Vec2d, 4>{lerp(p012, p123, t), p123, p23, k[3]};
    };
    const auto head = t1 >= 1.0 ? c : left(c, t1);
    if (t0 <= 0.0)
        return head;
    return right(head, t1 > 0.0 ? t0 / t1 : 0.0);
}

// The parameter of a segment at a distance along it, from its chord-length table.
[[nodiscard]] double parameterAt(const Measure::Segment& segment, const double along) noexcept {
    if (!(segment.length > 0.0) || along <= 0.0)
        return 0.0;
    if (along >= segment.length)
        return 1.0;
    if (segment.straight)
        return along / segment.length;
    const auto upper = std::upper_bound(segment.table.begin(), segment.table.end(), along);
    const auto high = static_cast<std::size_t>(upper - segment.table.begin());
    const auto low = high == 0 ? 0 : high - 1;
    const double span = segment.table[high] - segment.table[low];
    const double fraction = span > 0.0 ? (along - segment.table[low]) / span : 0.0;
    return (static_cast<double>(low) + fraction) / static_cast<double>(kLengthSamples);
}

// Where a distance falls: the segment and the distance into it. `forward` prefers the segment that
// starts at an exact boundary; otherwise the one that ends there. Zero-length segments are skipped.
struct Location final {
    std::size_t segment = 0;
    double along = 0.0;
};

[[nodiscard]] Location locate(const Measure& measure, double along, const bool forward) {
    Location where;
    const auto count = measure.segments.size();
    if (count == 0)
        return where;
    if (measure.closed && measure.length > 0.0) {
        along = std::fmod(along, measure.length);
        if (along < 0.0)
            along += measure.length;
        // A distance at the very end of a closed contour that was asked "backward" is its end.
        if (!forward && along <= kEpsilon)
            along = measure.length;
    }
    along = std::clamp(along, 0.0, measure.length);
    std::size_t index = 0;
    if (forward) {
        // The last segment that starts at or before `along` and has length.
        for (std::size_t each = 0; each < count; ++each) {
            if (!(measure.segments[each].length > 0.0))
                continue;
            if (measure.segments[each].start <= along + kEpsilon)
                index = each;
            else
                break;
        }
    } else {
        // The first segment that ends at or after `along` and has length.
        index = count - 1;
        for (std::size_t each = 0; each < count; ++each) {
            const auto& segment = measure.segments[each];
            if (segment.length > 0.0 && segment.start + segment.length >= along - kEpsilon) {
                index = each;
                break;
            }
        }
    }
    where.segment = index;
    where.along = std::clamp(along - measure.segments[index].start, 0.0,
                             measure.segments[index].length);
    return where;
}

[[nodiscard]] Vec2d derivative(const std::array<Vec2d, 4>& c, const double t) noexcept {
    const double u = 1.0 - t;
    return {3.0 * (u * u * (c[1].x - c[0].x) + 2.0 * u * t * (c[2].x - c[1].x) +
                   t * t * (c[3].x - c[2].x)),
            3.0 * (u * u * (c[1].y - c[0].y) + 2.0 * u * t * (c[2].y - c[1].y) +
                   t * t * (c[3].y - c[2].y))};
}

[[nodiscard]] Vec2d unit(const Vec2d v) noexcept {
    const double length = std::hypot(v.x, v.y);
    return length > 0.0 ? Vec2d{v.x / length, v.y / length} : Vec2d{};
}

[[nodiscard]] Vec2d tangentOf(const Measure::Segment& segment, const double t,
                              const bool atStart) noexcept {
    const auto& c = segment.controls;
    auto direction = segment.straight ? Vec2d{c[3].x - c[0].x, c[3].y - c[0].y} : derivative(c, t);
    if (std::hypot(direction.x, direction.y) <= kEpsilon) {
        // A handle on top of its anchor: the next control point gives the direction.
        direction = atStart ? Vec2d{c[2].x - c[0].x, c[2].y - c[0].y}
                            : Vec2d{c[3].x - c[1].x, c[3].y - c[1].y};
        if (std::hypot(direction.x, direction.y) <= kEpsilon)
            direction = {c[3].x - c[0].x, c[3].y - c[0].y};
    }
    return unit(direction);
}

[[nodiscard]] PathAnchor anchorAt(const Vec2d point) {
    PathAnchor anchor;
    anchor.point = point;
    return anchor;
}

// One piece of the answer between two placed points.
struct Piece final {
    Vec2d p0, c1, c2, p3;
    bool straight = false;
};

// The curve between the distances `a` and `b` (a < b; a closed contour may wrap past its end) as one
// cubic. Inside a single original segment it is that segment's own sub-curve, exactly; across
// anchors it is the cubic through the end points whose end directions are the original's and whose
// handle lengths best fit the original curve between them (a least-squares fit on even arc steps).
[[nodiscard]] Piece pieceBetween(const Measure& measure, const double a, const double b) {
    const auto first = locate(measure, a, true);
    const auto last = locate(measure, b, false);
    const auto& startSegment = measure.segments[first.segment];
    const auto& endSegment = measure.segments[last.segment];
    Piece piece;
    piece.p0 = pointAt(measure, a);
    piece.p3 = pointAt(measure, b);
    if (first.segment == last.segment && last.along + kEpsilon >= first.along && b - a <= endSegment.length + kEpsilon) {
        const double t0 = parameterAt(startSegment, first.along);
        const double t1 = parameterAt(startSegment, last.along);
        if (startSegment.straight) {
            piece.straight = true;
            piece.c1 = piece.p0;
            piece.c2 = piece.p3;
            return piece;
        }
        const auto controls = subcurve(startSegment.controls, t0, t1);
        piece.p0 = controls[0];
        piece.c1 = controls[1];
        piece.c2 = controls[2];
        piece.p3 = controls[3];
        return piece;
    }
    const double arc = b - a;
    const double chord = distance(piece.p0, piece.p3);
    const auto t0 = tangentOf(startSegment, parameterAt(startSegment, first.along), true);
    const auto t3 = tangentOf(endSegment, parameterAt(endSegment, last.along), false);
    const std::size_t steps = 24;
    double a11 = 0.0, a12 = 0.0, a22 = 0.0, r1 = 0.0, r2 = 0.0;
    for (std::size_t step = 1; step < steps; ++step) {
        const double t = static_cast<double>(step) / static_cast<double>(steps);
        const double u = 1.0 - t;
        const double b0 = u * u * u, b1 = 3.0 * u * u * t, b2 = 3.0 * u * t * t, b3 = t * t * t;
        const auto sample = pointAt(measure, a + arc * t);
        const Vec2d residual{sample.x - (piece.p0.x * (b0 + b1) + piece.p3.x * (b2 + b3)),
                             sample.y - (piece.p0.y * (b0 + b1) + piece.p3.y * (b2 + b3))};
        // residual ~ alpha * t0 * b1 - beta * t3 * b2
        a11 += b1 * b1;
        a22 += b2 * b2;
        a12 += -dot(t0, t3) * b1 * b2;
        r1 += b1 * dot(t0, residual);
        r2 += -b2 * dot(t3, residual);
    }
    double alpha = chord / 3.0, beta = chord / 3.0;
    const double determinant = a11 * a22 - a12 * a12;
    if (std::abs(determinant) > 1.0e-12) {
        const double solvedAlpha = (r1 * a22 - r2 * a12) / determinant;
        const double solvedBeta = (a11 * r2 - a12 * r1) / determinant;
        // A fit that folds back or runs far past the arc is a bad fit: keep the chord's third.
        if (solvedAlpha > 0.0 && solvedBeta > 0.0 && solvedAlpha < arc && solvedBeta < arc) {
            alpha = solvedAlpha;
            beta = solvedBeta;
        }
    }
    piece.c1 = {piece.p0.x + t0.x * alpha, piece.p0.y + t0.y * alpha};
    piece.c2 = {piece.p3.x - t3.x * beta, piece.p3.y - t3.y * beta};
    return piece;
}

// Inner points of the stretch [from, to], `inner` of them, evenly spaced and slid by `phase` of one
// gap towards the far end, the slide shrinking to nothing there so no point reaches or passes it.
void innerPoints(const double from, const double to, const std::size_t inner, const double phase,
                 std::vector<double>& out) {
    const double span = to - from;
    const double step = span / static_cast<double>(inner + 1);
    for (std::size_t index = 1; index <= inner; ++index) {
        const double j = static_cast<double>(index);
        out.push_back(from + step * (j + phase * (1.0 - j / static_cast<double>(inner + 1))));
    }
}

// How many points of `total` go to each span, in proportion to the spans' lengths, by largest
// remainder; ties go to the earlier span.
[[nodiscard]] std::vector<std::size_t> shares(const std::vector<double>& lengths,
                                              const std::size_t total) {
    std::vector<std::size_t> result(lengths.size(), 0);
    const double sum = std::accumulate(lengths.begin(), lengths.end(), 0.0);
    if (lengths.empty() || !(sum > 0.0)) {
        if (!result.empty())
            result.front() = total;
        return result;
    }
    std::vector<double> remainders(lengths.size(), 0.0);
    std::size_t given = 0;
    for (std::size_t index = 0; index < lengths.size(); ++index) {
        const double exact = static_cast<double>(total) * lengths[index] / sum;
        result[index] = static_cast<std::size_t>(std::floor(exact));
        remainders[index] = exact - std::floor(exact);
        given += result[index];
    }
    while (given < total) {
        std::size_t best = 0;
        for (std::size_t index = 1; index < remainders.size(); ++index)
            if (remainders[index] > remainders[best] + 1.0e-12)
                best = index;
        ++result[best];
        remainders[best] = -1.0;
        ++given;
    }
    return result;
}

[[nodiscard]] render::Path renderPathOf(const PathContour& contour) {
    render::Path path;
    path.closed = contour.closed;
    for (const auto& anchor : contour.anchors) {
        render::PathAnchor each{{anchor.point.x, anchor.point.y}, {}, {}};
        if (anchor.inHandle)
            each.inHandle = render::PathPoint{anchor.inHandle->x, anchor.inHandle->y};
        if (anchor.outHandle)
            each.outHandle = render::PathPoint{anchor.outHandle->x, anchor.outHandle->y};
        path.anchors.push_back(each);
    }
    return path;
}

[[nodiscard]] double distanceToPolyline(const render::PathPoint point,
                                        const std::vector<render::PathPoint>& line,
                                        const bool closed) {
    double best = std::numeric_limits<double>::infinity();
    const auto count = line.size();
    const auto segments = closed ? count : (count > 0 ? count - 1 : 0);
    for (std::size_t index = 0; index < segments; ++index) {
        const auto a = line[index];
        const auto b = line[(index + 1) % count];
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double lengthSquared = dx * dx + dy * dy;
        double t = lengthSquared > 0.0 ? ((point.x - a.x) * dx + (point.y - a.y) * dy) / lengthSquared
                                       : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        best = std::min(best, std::hypot(point.x - (a.x + dx * t), point.y - (a.y + dy * t)));
    }
    if (segments == 0 && count == 1)
        best = std::hypot(point.x - line[0].x, point.y - line[0].y);
    return best;
}

} // namespace

Measure measureContour(const PathContour& contour) {
    Measure measure;
    measure.closed = contour.closed;
    const auto count = document::contourSegmentCount(contour);
    measure.segments.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        Measure::Segment segment;
        segment.controls = document::contourSegmentControls(contour, index);
        const auto& c = segment.controls;
        segment.straight = c[1] == c[0] && c[2] == c[3];
        if (segment.straight) {
            segment.length = distance(c[0], c[3]);
        } else {
            segment.table.assign(kLengthSamples + 1, 0.0);
            auto previous = c[0];
            for (std::size_t sample = 1; sample <= kLengthSamples; ++sample) {
                const auto point =
                    evaluate(c, static_cast<double>(sample) / static_cast<double>(kLengthSamples));
                segment.table[sample] = segment.table[sample - 1] + distance(previous, point);
                previous = point;
            }
            segment.length = segment.table.back();
        }
        segment.start = measure.length;
        measure.length += segment.length;
        measure.segments.push_back(std::move(segment));
    }
    return measure;
}

Vec2d pointAt(const Measure& measure, const double along) {
    if (measure.segments.empty())
        return {};
    const auto where = locate(measure, along, true);
    const auto& segment = measure.segments[where.segment];
    if (segment.straight) {
        const double t = segment.length > 0.0 ? where.along / segment.length : 0.0;
        return lerp(segment.controls[0], segment.controls[3], t);
    }
    return evaluate(segment.controls, parameterAt(segment, where.along));
}

Vec2d tangentAt(const Measure& measure, const double along, const bool forward) {
    if (measure.segments.empty())
        return {};
    const auto where = locate(measure, along, forward);
    const auto& segment = measure.segments[where.segment];
    return tangentOf(segment, parameterAt(segment, where.along), forward);
}

std::vector<std::size_t> cornerAnchors(const PathContour& contour, const Measure& measure) {
    std::vector<std::size_t> corners;
    const auto anchors = contour.anchors.size();
    const auto segments = measure.segments.size();
    if (anchors < 3 || segments == 0)
        return corners;
    for (std::size_t index = contour.closed ? 0 : 1; index < (contour.closed ? anchors : anchors - 1);
         ++index) {
        const std::size_t before = (index + segments - 1) % segments;
        const auto& incoming = measure.segments[before];
        const auto& outgoing = measure.segments[index % segments];
        if (!(incoming.length > 0.0) || !(outgoing.length > 0.0))
            continue;
        const auto arriving = tangentOf(incoming, 1.0, false);
        const auto leaving = tangentOf(outgoing, 0.0, true);
        if (dot(arriving, leaving) < kCornerCosine)
            corners.push_back(index);
    }
    return corners;
}

ContourResult resampleContour(const PathContour& contour, const ContourRequest& request,
                              const std::size_t maximumPoints) {
    return resampleContour(contour, measureContour(contour), request, maximumPoints);
}

ContourResult resampleContour(const PathContour& contour, const Measure& measure,
                              const ContourRequest& request, const std::size_t maximumPoints) {
    ContourResult result;
    result.contour.closed = contour.closed;
    const bool closed = contour.closed;
    const auto minimum = static_cast<std::size_t>(closed ? 3 : 2);
    const double phase = std::clamp(request.phase, 0.0, 1.0);

    // A contour with no length has no place to put points along.
    if (contour.anchors.empty() || !(measure.length > 0.0) || measure.segments.empty()) {
        result.status = ResampleStatus::Unusable;
        result.contour = contour;
        if (request.count && !contour.anchors.empty()) {
            const auto wanted = std::max<std::int64_t>(*request.count, static_cast<std::int64_t>(minimum));
            if (static_cast<double>(wanted) > static_cast<double>(maximumPoints)) {
                result.status = ResampleStatus::TooMany;
                result.requested = static_cast<double>(wanted);
                result.contour = {};
                return result;
            }
            result.contour.anchors.assign(static_cast<std::size_t>(wanted),
                                          anchorAt(contour.anchors.front().point));
            result.degenerate = true;
        }
        return result;
    }

    const double length = measure.length;
    std::vector<std::size_t> corners;
    if (request.keepCorners)
        corners = cornerAnchors(contour, measure);

    // The fixed points: an open contour's ends and its corners; a closed contour's corners.
    std::vector<double> fixed;
    if (!closed)
        fixed.push_back(0.0);
    for (const auto index : corners)
        fixed.push_back(measure.segments[index].start);
    if (!closed)
        fixed.push_back(length);

    // The stretches between fixed points, each with the number of inner points it gets.
    struct Span final {
        double from = 0.0;
        double to = 0.0;
    };
    std::vector<Span> spans;
    bool cyclic = false;
    if (closed && fixed.empty()) {
        cyclic = true;
    } else if (closed) {
        for (std::size_t index = 0; index < fixed.size(); ++index)
            spans.push_back({fixed[index],
                             index + 1 < fixed.size() ? fixed[index + 1] : fixed.front() + length});
    } else {
        for (std::size_t index = 0; index + 1 < fixed.size(); ++index)
            spans.push_back({fixed[index], fixed[index + 1]});
    }
    const std::size_t fixedCount = closed ? fixed.size() : fixed.size();

    std::vector<std::size_t> inner(spans.size(), 0);
    std::size_t cyclicCount = 0;
    double requested = 0.0;
    if (request.count) {
        auto wanted = *request.count;
        wanted = std::max<std::int64_t>(wanted, static_cast<std::int64_t>(minimum));
        requested = static_cast<double>(wanted);
        if (requested > static_cast<double>(maximumPoints)) {
            result.status = ResampleStatus::TooMany;
            result.requested = requested;
            result.contour = {};
            return result;
        }
        const auto total = static_cast<std::size_t>(wanted);
        if (cyclic) {
            cyclicCount = total;
        } else if (total < fixedCount) {
            // Not room for every corner: the points are spread evenly instead.
            corners.clear();
            fixed.clear();
            spans.clear();
            inner.clear();
            if (closed) {
                cyclic = true;
                cyclicCount = total;
            } else {
                spans.push_back({0.0, length});
                fixed = {0.0, length};
                inner.assign(1, total - 2);
            }
        } else {
            std::vector<double> lengths;
            for (const auto& span : spans)
                lengths.push_back(span.to - span.from);
            inner = shares(lengths, total - fixedCount);
        }
    } else {
        const double gap = request.gap;
        const auto intervals = [&](const double span) {
            return std::max(1.0, std::ceil(span / gap - 1.0e-9));
        };
        if (cyclic) {
            requested = std::max<double>(static_cast<double>(minimum), intervals(length));
            cyclicCount = requested <= static_cast<double>(maximumPoints)
                              ? static_cast<std::size_t>(requested)
                              : 0;
        } else {
            requested = static_cast<double>(fixedCount);
            std::vector<double> counts;
            for (const auto& span : spans) {
                const double stretch = intervals(span.to - span.from) - 1.0;
                counts.push_back(stretch);
                requested += stretch;
            }
            // An open contour of one stretch keeps its ends only when the gap is the whole length.
            if (requested <= static_cast<double>(maximumPoints))
                for (std::size_t index = 0; index < counts.size(); ++index)
                    inner[index] = static_cast<std::size_t>(counts[index]);
        }
        if (requested > static_cast<double>(maximumPoints)) {
            result.status = ResampleStatus::TooMany;
            result.requested = requested;
            result.contour = {};
            return result;
        }
    }

    // The distance of every placed point, in order.
    std::vector<double> positions;
    if (cyclic) {
        const double step = length / static_cast<double>(cyclicCount);
        for (std::size_t index = 0; index < cyclicCount; ++index) {
            double along = std::fmod(phase * length + step * static_cast<double>(index), length);
            positions.push_back(along);
        }
    } else {
        for (std::size_t index = 0; index < spans.size(); ++index) {
            positions.push_back(spans[index].from);
            innerPoints(spans[index].from, spans[index].to, inner[index], phase, positions);
        }
        if (!closed)
            positions.push_back(length);
    }
    if (positions.size() > maximumPoints) {
        result.status = ResampleStatus::TooMany;
        result.requested = static_cast<double>(positions.size());
        result.contour = {};
        return result;
    }

    auto& out = result.contour;
    out.anchors.reserve(positions.size());
    for (const auto along : positions)
        out.anchors.push_back(anchorAt(pointAt(measure, along)));
    // A kept corner is the original anchor itself, not a point computed to near it.
    if (!closed)
        out.anchors.front().point = contour.anchors.front().point;
    if (!closed)
        out.anchors.back().point = contour.anchors.back().point;
    if (!request.curves)
        return result;

    const auto count = positions.size();
    const auto pieces = closed ? count : count - 1;
    for (std::size_t index = 0; index < pieces; ++index) {
        const double a = positions[index];
        double b = index + 1 < count ? positions[index + 1] : positions.front() + length;
        if (closed && b <= a)
            b += length;
        if (!(b - a > kEpsilon))
            continue;
        const auto piece = pieceBetween(measure, a, b);
        if (piece.straight)
            continue;
        out.anchors[index].outHandle = piece.c1;
        out.anchors[(index + 1) % count].inHandle = piece.c2;
    }
    for (auto& anchor : out.anchors)
        anchor.mode = document::inferPathAnchorMode(anchor);
    return result;
}

ContourResult resampleToCount(const PathContour& contour, const std::size_t count) {
    ContourRequest request;
    request.count = static_cast<std::int64_t>(count);
    request.keepCorners = false;
    request.curves = true;
    return resampleContour(contour, request, document::kMaximumPathAnchors);
}

double deviation(const PathContour& curve, const PathContour& reference) {
    const auto flatCurve = render::flattenPathContour(renderPathOf(curve), 0.01);
    const auto flatReference = render::flattenPathContour(renderPathOf(reference), 0.01);
    if (!flatCurve || !flatReference)
        return std::numeric_limits<double>::infinity();
    double worst = 0.0;
    for (const auto& point : *flatCurve)
        worst = std::max(worst, distanceToPolyline(point, *flatReference, reference.closed));
    for (const auto& point : *flatReference)
        worst = std::max(worst, distanceToPolyline(point, *flatCurve, curve.closed));
    return worst;
}

} // namespace bloom::runtime::pathresample
