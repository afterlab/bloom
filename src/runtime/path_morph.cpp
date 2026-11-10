#include "path_morph.hpp"

#include <bloom/document/path_value.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace bloom::runtime::pathmorph {
namespace {

using document::PathAnchor;
using document::PathContour;
using document::PathValue;
using document::Vec2d;

// Chords per segment when a contour is measured for pairing and subdivision: enough to tell which
// segment is longest and where a contour's middle is, and the same on every platform.
constexpr int kMeasureSteps = 16;
// Up to this many anchors the start of a closed contour is chosen by summing every candidate
// exactly; past it, by the same sums through a circular correlation (an FFT), so the cost stays
// near N log N.
constexpr std::size_t kExactShiftLimit = 1024;

[[nodiscard]] Vec2d lerp(const Vec2d a, const Vec2d b, const double t) noexcept {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}
[[nodiscard]] double squared(const Vec2d a, const Vec2d b) noexcept {
    const double x = b.x - a.x;
    const double y = b.y - a.y;
    return x * x + y * y;
}
[[nodiscard]] Vec2d cubicAt(const std::array<Vec2d, 4>& c, const double t) noexcept {
    const auto p01 = lerp(c[0], c[1], t);
    const auto p12 = lerp(c[1], c[2], t);
    const auto p23 = lerp(c[2], c[3], t);
    return lerp(lerp(p01, p12, t), lerp(p12, p23, t), t);
}

[[nodiscard]] double segmentLength(const PathContour& contour, const std::size_t segment) {
    const auto controls = document::contourSegmentControls(contour, segment);
    const auto& a = contour.anchors[segment];
    const auto& b = contour.anchors[(segment + 1) % contour.anchors.size()];
    if (!a.outHandle && !b.inHandle)
        return std::sqrt(squared(controls[0], controls[3]));
    double length = 0.0;
    Vec2d previous = controls[0];
    for (int step = 1; step <= kMeasureSteps; ++step) {
        const auto point = cubicAt(controls, static_cast<double>(step) / kMeasureSteps);
        length += std::sqrt(squared(previous, point));
        previous = point;
    }
    return length;
}

// What pairing weighs of a contour: the centroid of its outline (each piece of the flattened
// outline weighted by its length), its length and the size of the area it encloses (as though
// closed).
struct Shape final {
    Vec2d centroid;
    double length = 0.0;
    double size = 0.0;
};

[[nodiscard]] Shape shapeOf(const PathContour& contour) {
    Shape shape;
    if (contour.anchors.empty())
        return shape;
    std::vector<Vec2d> outline{contour.anchors.front().point};
    const auto segments = document::contourSegmentCount(contour);
    for (std::size_t segment = 0; segment < segments; ++segment) {
        const auto controls = document::contourSegmentControls(contour, segment);
        for (int step = 1; step <= kMeasureSteps; ++step)
            outline.push_back(cubicAt(controls, static_cast<double>(step) / kMeasureSteps));
    }
    double weight = 0.0;
    Vec2d sum{0.0, 0.0};
    double area = 0.0;
    for (std::size_t index = 0; index + 1 < outline.size(); ++index) {
        const auto a = outline[index];
        const auto b = outline[index + 1];
        const double piece = std::sqrt(squared(a, b));
        weight += piece;
        sum.x += piece * (a.x + b.x) * 0.5;
        sum.y += piece * (a.y + b.y) * 0.5;
        area += a.x * b.y - b.x * a.y;
    }
    const auto first = outline.front();
    const auto last = outline.back();
    area += last.x * first.y - first.x * last.y;
    if (weight > 0.0) {
        shape.centroid = {sum.x / weight, sum.y / weight};
    } else {
        // Every anchor in one place: that place.
        shape.centroid = first;
    }
    shape.length = weight;
    shape.size = std::sqrt(std::abs(area) * 0.5);
    return shape;
}

[[nodiscard]] double pairCost(const Shape& a, const Shape& b) noexcept {
    const double size = a.size - b.size;
    const double length = (a.length - b.length) * 0.25;
    return squared(a.centroid, b.centroid) + size * size + length * length;
}

// The contour with every anchor (and no handle) at `point`: what a contour with no partner shrinks
// to or grows from. Its anchor count, closed flag and modes are the contour's own.
[[nodiscard]] PathContour collapsed(const PathContour& contour, const Vec2d point) {
    PathContour out;
    out.closed = contour.closed;
    out.anchors.reserve(contour.anchors.size());
    for (const auto& anchor : contour.anchors)
        out.anchors.push_back({point, std::nullopt, std::nullopt, anchor.mode});
    return out;
}

// The contour with `count` anchors: its longest segment split at its middle until there are that
// many, so the drawn shape is unchanged. A contour with no segment repeats its anchor.
[[nodiscard]] PathContour subdivided(PathContour contour, const std::size_t count) {
    if (contour.anchors.size() >= count || contour.anchors.empty())
        return contour;
    if (document::contourSegmentCount(contour) == 0) {
        auto anchor = contour.anchors.back();
        anchor.inHandle.reset();
        anchor.outHandle.reset();
        contour.anchors.resize(count, anchor);
        return contour;
    }
    std::vector<double> lengths;
    lengths.reserve(count);
    for (std::size_t segment = 0; segment < document::contourSegmentCount(contour); ++segment)
        lengths.push_back(segmentLength(contour, segment));
    while (contour.anchors.size() < count) {
        // The longest; the first of equals, so the answer never depends on anything but the input.
        const auto first =
            static_cast<std::size_t>(std::ranges::max_element(lengths) - lengths.begin());
        if (!document::subdivideContourSegment(contour, first, 0.5).has_value())
            break;
        lengths[first] = segmentLength(contour, first);
        lengths.insert(lengths.begin() + static_cast<std::ptrdiff_t>(first) + 1,
                       segmentLength(contour, first + 1));
    }
    return contour;
}

// The same contour drawn the other way: anchors in reverse order, each anchor's handles swapped.
[[nodiscard]] PathContour reversed(PathContour contour) {
    std::ranges::reverse(contour.anchors);
    for (auto& anchor : contour.anchors)
        std::swap(anchor.inHandle, anchor.outHandle);
    return contour;
}

// A closed contour started at anchor `shift`: the same shape.
[[nodiscard]] PathContour rotated(PathContour contour, const std::size_t shift) {
    std::ranges::rotate(contour.anchors,
                        contour.anchors.begin() + static_cast<std::ptrdiff_t>(shift));
    return contour;
}

// A closed contour opened at its first anchor, which is repeated at the end with the closing
// segment's handle: the same outline, now open.
[[nodiscard]] PathContour cutOpen(PathContour contour) {
    if (contour.anchors.empty())
        return contour;
    auto end = contour.anchors.front();
    end.outHandle.reset();
    contour.anchors.front().inHandle.reset();
    contour.anchors.push_back(end);
    contour.closed = false;
    return contour;
}

[[nodiscard]] std::vector<Vec2d> pointsOf(const PathContour& contour, const std::size_t count) {
    std::vector<Vec2d> points;
    points.reserve(count);
    for (std::size_t index = 0; index < count && index < contour.anchors.size(); ++index)
        points.push_back(contour.anchors[index].point);
    return points;
}

void fft(std::vector<std::complex<double>>& values, const bool inverse) {
    const std::size_t size = values.size();
    for (std::size_t index = 1, bit = 0; index < size; ++index) {
        std::size_t mask = size >> 1U;
        for (; (bit & mask) != 0; mask >>= 1U)
            bit ^= mask;
        bit ^= mask;
        if (index < bit)
            std::swap(values[index], values[bit]);
    }
    for (std::size_t length = 2; length <= size; length <<= 1U) {
        const double angle =
            2.0 * std::numbers::pi / static_cast<double>(length) * (inverse ? 1.0 : -1.0);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t start = 0; start < size; start += length) {
            std::complex<double> twiddle(1.0, 0.0);
            for (std::size_t offset = 0; offset < length / 2; ++offset) {
                const auto even = values[start + offset];
                const auto odd = values[start + offset + length / 2] * twiddle;
                values[start + offset] = even + odd;
                values[start + offset + length / 2] = even - odd;
                twiddle *= step;
            }
        }
    }
    if (inverse)
        for (auto& value : values)
            value /= static_cast<double>(size);
}

[[nodiscard]] bool cancelled(const CancellationToken* cancellation) noexcept {
    return cancellation != nullptr && cancellation->isCancellationRequested();
}

// cost[k] = sum over i of |a[i] - b[(i + k) mod n]|^2, for every shift k (a and b both n long).
[[nodiscard]] std::vector<double> shiftCosts(std::span<const Vec2d> a, std::span<const Vec2d> b,
                                             const CancellationToken* cancellation) {
    const std::size_t n = a.size();
    std::vector<double> costs(n, 0.0);
    if (n == 0)
        return costs;
    if (n <= kExactShiftLimit) {
        for (std::size_t shift = 0; shift < n; ++shift) {
            if (cancelled(cancellation))
                return costs;
            double sum = 0.0;
            for (std::size_t index = 0; index < n; ++index)
                sum += squared(a[index], b[(index + shift) % n]);
            costs[shift] = sum;
        }
        return costs;
    }
    // |a - b|^2 = |a|^2 + |b|^2 - 2 Re(conj(a) b), and the last term over every shift is the
    // circular correlation of a with b, read off one product of transforms.
    double norms = 0.0;
    for (std::size_t index = 0; index < n; ++index)
        norms += a[index].x * a[index].x + a[index].y * a[index].y + b[index].x * b[index].x +
                 b[index].y * b[index].y;
    std::size_t size = 1;
    while (size < 2 * n)
        size <<= 1U;
    std::vector<std::complex<double>> left(size), right(size);
    for (std::size_t index = 0; index < n; ++index) {
        left[index] = {a[index].x, a[index].y};
        right[index] = {b[index].x, b[index].y};
        right[index + n] = right[index];
    }
    // Correlation: reverse a, convolve with b repeated once.
    std::reverse(left.begin(), left.begin() + static_cast<std::ptrdiff_t>(n));
    for (auto& value : left)
        value = std::conj(value);
    // The convolution reads up to index 3n - 2; size >= 2n is enough only with b's copy wrapped, so
    // grow to 4n when needed.
    if (size < 3 * n) {
        size <<= 1U;
        left.resize(size);
        right.resize(size);
    }
    if (cancelled(cancellation))
        return costs;
    fft(left, false);
    fft(right, false);
    for (std::size_t index = 0; index < size; ++index)
        left[index] *= right[index];
    fft(left, true);
    for (std::size_t shift = 0; shift < n; ++shift)
        costs[shift] = norms - 2.0 * left[n - 1 + shift].real();
    return costs;
}

struct Choice final {
    double cost = std::numeric_limits<double>::infinity();
    std::size_t shift = 0;
    bool reverse = false;
};

// Two closed contours of the same count: the start and direction of `to` that make the summed
// squared distance between corresponding anchors least (forward first, then the earliest start).
[[nodiscard]] PathContour alignClosed(const PathContour& from, const PathContour& to,
                                      const CancellationToken* cancellation) {
    const auto n = from.anchors.size();
    const auto a = pointsOf(from, n);
    Choice best;
    for (const bool reverse : {false, true}) {
        const auto candidate = reverse ? reversed(to) : to;
        const auto costs = shiftCosts(a, pointsOf(candidate, n), cancellation);
        for (std::size_t shift = 0; shift < n; ++shift)
            if (costs[shift] < best.cost)
                best = {costs[shift], shift, reverse};
    }
    return rotated(best.reverse ? reversed(to) : to, best.shift);
}

// A closed contour of m anchors against an open one of m + 1: where to cut the closed one open (and
// in which direction) so its anchors lie nearest the open one's, the cut repeated at the end.
[[nodiscard]] PathContour cutNearest(const PathContour& closed, const PathContour& open,
                                     const CancellationToken* cancellation) {
    const auto m = closed.anchors.size();
    const auto a = pointsOf(open, m);
    const auto end = open.anchors.back().point;
    Choice best;
    for (const bool reverse : {false, true}) {
        const auto candidate = reverse ? reversed(closed) : closed;
        const auto b = pointsOf(candidate, m);
        auto costs = shiftCosts(a, b, cancellation);
        for (std::size_t shift = 0; shift < m; ++shift) {
            const double cost = costs[shift] + squared(end, b[shift]);
            if (cost < best.cost)
                best = {cost, shift, reverse};
        }
    }
    return cutOpen(rotated(best.reverse ? reversed(closed) : closed, best.shift));
}

// Two contours with the same anchor count and closed flag that draw `from` and `to`.
[[nodiscard]] std::pair<PathContour, PathContour> alignContours(const PathContour& from,
                                                                const PathContour& to,
                                                                const CancellationToken* cancellation) {
    if (from.closed == to.closed) {
        const auto count = std::max(from.anchors.size(), to.anchors.size());
        auto a = subdivided(from, count);
        auto b = subdivided(to, count);
        if (from.closed) {
            auto started = alignClosed(a, b, cancellation);
            return {std::move(a), std::move(started)};
        }
        // Open: the ends stay ends; `to` turns round when its ends lie nearer the opposite ones.
        const auto& first = a.anchors.front().point;
        const auto& last = a.anchors.back().point;
        const double along =
            squared(first, b.anchors.front().point) + squared(last, b.anchors.back().point);
        const double across =
            squared(first, b.anchors.back().point) + squared(last, b.anchors.front().point);
        return {std::move(a), across < along ? reversed(std::move(b)) : std::move(b)};
    }
    // One open, one closed: the closed one is cut open where it best meets the open one's anchors.
    const auto& open = from.closed ? to : from;
    const auto& closed = from.closed ? from : to;
    const auto count = std::max(open.anchors.size(), closed.anchors.size() + 1);
    auto o = subdivided(open, count);
    auto c = cutNearest(subdivided(closed, count - 1), o, cancellation);
    if (from.closed)
        return {std::move(c), std::move(o)};
    return {std::move(o), std::move(c)};
}

// The pairs of one set of contours (indices into `from` and `to`), greedily by cost, or by order.
void pairSet(const std::vector<std::size_t>& fromSet, const std::vector<std::size_t>& toSet,
             const std::vector<Shape>& fromShapes, const std::vector<Shape>& toShapes,
             const Pairing pairing, std::vector<std::optional<std::size_t>>& partner,
             const CancellationToken* cancellation) {
    if (pairing == Pairing::ByOrder || fromSet.size() * toSet.size() > kMaximumGeometricPairs) {
        for (std::size_t index = 0; index < std::min(fromSet.size(), toSet.size()); ++index)
            partner[fromSet[index]] = toSet[index];
        return;
    }
    std::vector<std::tuple<double, std::size_t, std::size_t>> candidates;
    candidates.reserve(fromSet.size() * toSet.size());
    for (const auto i : fromSet) {
        if (cancelled(cancellation))
            return;
        for (const auto j : toSet)
            candidates.emplace_back(pairCost(fromShapes[i], toShapes[j]), i, j);
    }
    if (cancelled(cancellation))
        return;
    std::ranges::sort(candidates);
    std::vector<bool> fromTaken(fromShapes.size(), false), toTaken(toShapes.size(), false);
    for (const auto& [cost, i, j] : candidates) {
        if (fromTaken[i] || toTaken[j])
            continue;
        fromTaken[i] = toTaken[j] = true;
        partner[i] = j;
    }
}

} // namespace

Alignment alignPaths(const PathValue& from, const PathValue& to, const AlignOptions& options) {
    const auto n = from.contours.size();
    const auto m = to.contours.size();
    std::vector<Shape> fromShapes, toShapes;
    fromShapes.reserve(n);
    toShapes.reserve(m);
    for (const auto& contour : from.contours)
        fromShapes.push_back(shapeOf(contour));
    for (const auto& contour : to.contours)
        toShapes.push_back(shapeOf(contour));

    // Which group each contour is in, when that group's key is in the other value too.
    std::vector<std::optional<std::size_t>> fromGroup(n), toGroup(m);
    std::vector<std::pair<std::size_t, std::size_t>> matches;
    if (options.pairing == Pairing::ByGroupThenGeometry)
        matches = document::matchPathGroups(from, to);
    const auto mark = [](const document::PathGroup& group, const std::size_t match,
                         std::vector<std::optional<std::size_t>>& groupOf) {
        const auto end = std::min<std::size_t>(group.firstContour + std::size_t{group.contourCount},
                                               groupOf.size());
        for (std::size_t index = group.firstContour; index < end; ++index)
            groupOf[index] = match;
    };
    for (std::size_t match = 0; match < matches.size(); ++match) {
        mark(from.groups[matches[match].first], match, fromGroup);
        mark(to.groups[matches[match].second], match, toGroup);
    }

    // A contour with no anchors pairs with nothing: it stays empty, and one in `to` adds nothing.
    std::vector<std::optional<std::size_t>> partner(n);
    std::vector<std::vector<std::size_t>> fromSets(matches.size() + 1), toSets(matches.size() + 1);
    for (std::size_t index = 0; index < n; ++index)
        if (!from.contours[index].anchors.empty())
            fromSets[fromGroup[index].value_or(matches.size())].push_back(index);
    for (std::size_t index = 0; index < m; ++index)
        if (!to.contours[index].anchors.empty())
            toSets[toGroup[index].value_or(matches.size())].push_back(index);
    Alignment result;
    for (std::size_t set = 0; set < fromSets.size(); ++set) {
        pairSet(fromSets[set], toSets[set], fromShapes, toShapes, options.pairing, partner,
                options.cancellation);
        if (cancelled(options.cancellation)) {
            result.status = AlignStatus::Cancelled;
            return result;
        }
    }

    std::vector<bool> toPaired(m, false);
    for (const auto& each : partner)
        if (each)
            toPaired[*each] = true;
    std::size_t contours = n;
    for (std::size_t index = 0; index < m; ++index)
        contours += !toPaired[index] && !to.contours[index].anchors.empty() ? 1U : 0U;
    if (contours > document::kMaximumPathContours) {
        result.status = AlignStatus::TooMany;
        result.requested = contours;
        return result;
    }
    // The anchors the aligned form will hold, checked before any is made.
    std::size_t anchors = 0;
    for (std::size_t index = 0; index < n; ++index) {
        const auto& a = from.contours[index];
        std::size_t count = a.anchors.size();
        if (partner[index]) {
            const auto& b = to.contours[*partner[index]];
            count = a.closed == b.closed
                        ? std::max(a.anchors.size(), b.anchors.size())
                        : std::max(a.closed ? a.anchors.size() + 1 : a.anchors.size(),
                                   b.closed ? b.anchors.size() + 1 : b.anchors.size());
        }
        if (count > document::kMaximumPathAnchors) {
            result.status = AlignStatus::TooMany;
            result.requested = count;
            result.contour = true;
            return result;
        }
        anchors += count;
    }
    for (std::size_t index = 0; index < m; ++index)
        if (!toPaired[index])
            anchors += to.contours[index].anchors.size();
    if (anchors > document::kMaximumPathValueAnchors) {
        result.status = AlignStatus::TooMany;
        result.requested = anchors;
        return result;
    }

    result.from.fillRule = from.fillRule;
    result.from.groups = from.groups;
    result.to.fillRule = to.fillRule;
    result.to.groups = from.groups;
    result.from.contours.reserve(contours);
    result.to.contours.reserve(contours);
    for (std::size_t index = 0; index < n; ++index) {
        const auto& a = from.contours[index];
        if (partner[index]) {
            // The costly step: one start and direction search per pair, so cancellation is looked
            // at before each and the answer after it is discarded if the token fired inside it.
            if (cancelled(options.cancellation)) {
                result.status = AlignStatus::Cancelled;
                result.from = {};
                result.to = {};
                return result;
            }
            auto [first, second] =
                alignContours(a, to.contours[*partner[index]], options.cancellation);
            if (cancelled(options.cancellation)) {
                result.status = AlignStatus::Cancelled;
                result.from = {};
                result.to = {};
                return result;
            }
            result.from.contours.push_back(std::move(first));
            result.to.contours.push_back(std::move(second));
        } else {
            result.from.contours.push_back(a);
            result.to.contours.push_back(
                a.anchors.empty() ? a : collapsed(a, fromShapes[index].centroid));
        }
    }
    for (std::size_t index = 0; index < m; ++index) {
        const auto& b = to.contours[index];
        if (toPaired[index] || b.anchors.empty())
            continue;
        result.from.contours.push_back(collapsed(b, toShapes[index].centroid));
        result.to.contours.push_back(b);
    }
    return result;
}

PathValue mixAligned(const PathValue& from, const PathValue& to, const double amount) {
    const auto mix = [amount](const Vec2d a, const Vec2d b) {
        return Vec2d{a.x + (b.x - a.x) * amount, a.y + (b.y - a.y) * amount};
    };
    PathValue result;
    result.fillRule = from.fillRule;
    result.groups = from.groups;
    result.contours.reserve(from.contours.size());
    for (std::size_t contour = 0; contour < from.contours.size() && contour < to.contours.size();
         ++contour) {
        const auto& a = from.contours[contour];
        const auto& b = to.contours[contour];
        auto& out = result.contours.emplace_back();
        out.closed = a.closed;
        out.anchors.reserve(a.anchors.size());
        for (std::size_t index = 0; index < a.anchors.size() && index < b.anchors.size(); ++index) {
            const auto& p = a.anchors[index];
            const auto& q = b.anchors[index];
            PathAnchor mixed;
            mixed.point = mix(p.point, q.point);
            if (p.inHandle || q.inHandle)
                mixed.inHandle = mix(p.inHandle.value_or(p.point), q.inHandle.value_or(q.point));
            if (p.outHandle || q.outHandle)
                mixed.outHandle = mix(p.outHandle.value_or(p.point), q.outHandle.value_or(q.point));
            mixed.mode = p.mode;
            out.anchors.push_back(mixed);
        }
    }
    return result;
}

Morphed morphPathsChecked(const PathValue& from, const PathValue& to, const double amount,
                          const AlignOptions& options) {
    Morphed result;
    if (amount == 0.0 || amount == 1.0) {
        result.value = amount == 0.0 ? from : to;
        return result;
    }
    const auto aligned = alignPaths(from, to, options);
    result.status = aligned.status;
    result.requested = aligned.requested;
    result.contour = aligned.contour;
    if (aligned.status != AlignStatus::Ok)
        return result;
    result.value = mixAligned(aligned.from, aligned.to, amount);
    result.value.fillRule = from.fillRule;
    result.value.groups = from.groups;
    return result;
}

std::optional<PathValue> morphPaths(const PathValue& from, const PathValue& to, const double amount,
                                    const AlignOptions& options) {
    auto morphed = morphPathsChecked(from, to, amount, options);
    if (morphed.status != AlignStatus::Ok)
        return std::nullopt;
    return std::move(morphed.value);
}

} // namespace bloom::runtime::pathmorph
