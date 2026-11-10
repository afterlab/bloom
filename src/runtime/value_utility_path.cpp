// #344 Transform Path, Trim Path and Offset Path; #530 Boolean Paths, Merge Paths and Round
// Corners. Path in, Path out, as pure functions of their operands
// (docs/architecture/path-processing.md).
//
// These are host value computations: the path they answer is the same for the preview, a still, a
// sequence and a scripted render, and whatever draws it (Shape Source) is the production fill that
// is already covered on every route. Nothing here reads the time.

#include "value_utility_path_support.hpp"

#include <bloom/document/parameter.hpp>
#include <bloom/document/path_value.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/render/path_raster.hpp>

#include <clipper2/clipper.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using document::PathAnchor;
using document::PathAnchorMode;
using document::PathContour;
using document::PathValue;
using document::Vec2d;
using runtime::ValueUtilityInvocation;
using runtime::ValueUtilityOutcome;
using runtime::detail::shapedOutcome;
using runtime::detail::ValueUtilityReader;
using Kernel = document::ValueUtilityKernel;

constexpr int kClipPrecision = 3;
constexpr std::size_t kLengthSamples = 64;

using namespace runtime::detail::pathsupport;

// Groups follow the contours that descend from them: `produced[i]` is how many output contours
// input contour `i` became, in order. A group keeps its key over the contours its inputs became and
// loses it when they became none.
void carryGroups(const PathValue& from, const std::span<const std::uint32_t> produced,
                 PathValue& to) {
    to.groups.clear();
    std::vector<std::uint32_t> start(from.contours.size() + 1, 0);
    for (std::size_t index = 0; index < from.contours.size(); ++index)
        start[index + 1] = start[index] + (index < produced.size() ? produced[index] : 1U);
    for (auto group : from.groups) {
        if (group.firstContour + group.contourCount > from.contours.size())
            continue;
        const auto low = start[group.firstContour];
        const auto high = start[group.firstContour + group.contourCount];
        if (high <= low)
            continue;
        group.firstContour = low;
        group.contourCount = high - low;
        to.groups.push_back(std::move(group));
    }
}

[[nodiscard]] Vec2d lerp(const Vec2d a, const Vec2d b, const double t) noexcept {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}
[[nodiscard]] double distance(const Vec2d a, const Vec2d b) noexcept {
    return std::hypot(b.x - a.x, b.y - a.y);
}

// ---------------------------------------------------------------------------------------------
// Transform Path
// ---------------------------------------------------------------------------------------------

ValueUtilityOutcome transformPath(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto& path = reader.path(0);
    const auto position = reader.vector2(1);
    const auto anchor = reader.vector2(2);
    const auto scale = reader.vector2(3);
    const double rotation = reader.scalar(4);
    if (reader.malformed())
        return outcome;
    if (!finite(position) || !finite(anchor) || !finite(scale) || !std::isfinite(rotation))
        return refuse(std::move(outcome), "Transform Path operand is not finite",
                      "The position, anchor, scale and rotation must be finite numbers.");
    const double radians = rotation * std::numbers::pi / 180.0;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    const double scaleX = scale.x;
    const double scaleY = scale.y;
    // Scale about the anchor, then turn about it (clockwise on screen for a positive angle), then
    // move the anchor to the position. A cubic curve maps to the cubic of the mapped controls, so
    // the transform is exact for every segment.
    const auto map = [&](const Vec2d point) {
        const double dx = (point.x - anchor.x) * scaleX;
        const double dy = (point.y - anchor.y) * scaleY;
        return Vec2d{position.x + dx * cosine - dy * sine, position.y + dx * sine + dy * cosine};
    };
    PathValue result = path;
    for (auto& contour : result.contours) {
        if (stopRequested(invocation))
            return cancelledOutcome(invocation);
        for (auto& each : contour.anchors) {
            each.point = map(each.point);
            if (each.inHandle)
                each.inHandle = map(*each.inHandle);
            if (each.outHandle)
                each.outHandle = map(*each.outHandle);
            const bool usable = finite(each.point) && (!each.inHandle || finite(*each.inHandle)) &&
                                (!each.outHandle || finite(*each.outHandle));
            if (!usable)
                return refuse(shapedOutcome(invocation.operation), "Transform Path overflowed",
                              "The transformed path has a coordinate that is not a finite number.");
        }
    }
    publish(outcome, std::move(result));
    return outcome;
}

// ---------------------------------------------------------------------------------------------
// Round Corners
// ---------------------------------------------------------------------------------------------

[[nodiscard]] bool straightBetween(const PathAnchor& from, const PathAnchor& to) noexcept {
    return (!from.outHandle || *from.outHandle == from.point) &&
           (!to.inHandle || *to.inHandle == to.point);
}

// nullopt when cancelled part way through the contour's anchors.
std::optional<PathContour> roundContour(const PathContour& source, const double radius,
                                        const ValueUtilityInvocation& invocation) {
    const auto count = source.anchors.size();
    if (count < 3)
        return source;
    PathContour result;
    result.closed = source.closed;
    result.anchors.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (stopRequested(invocation))
            return std::nullopt;
        const auto& corner = source.anchors[index];
        const bool endpoint = !source.closed && (index == 0 || index + 1 == count);
        const auto& before = source.anchors[(index + count - 1) % count];
        const auto& after = source.anchors[(index + 1) % count];
        const bool bare = !corner.inHandle && !corner.outHandle;
        if (endpoint || !bare || !straightBetween(before, corner) ||
            !straightBetween(corner, after)) {
            result.anchors.push_back(corner);
            continue;
        }
        const double lengthBefore = distance(corner.point, before.point);
        const double lengthAfter = distance(corner.point, after.point);
        if (!(lengthBefore > 0.0) || !(lengthAfter > 0.0)) {
            result.anchors.push_back(corner);
            continue;
        }
        const Vec2d toBefore{(before.point.x - corner.point.x) / lengthBefore,
                             (before.point.y - corner.point.y) / lengthBefore};
        const Vec2d toAfter{(after.point.x - corner.point.x) / lengthAfter,
                            (after.point.y - corner.point.y) / lengthAfter};
        const double cosine =
            std::clamp(toBefore.x * toAfter.x + toBefore.y * toAfter.y, -1.0, 1.0);
        // The interior angle between the two sides, and the turn a traveller makes at the corner.
        const double interior = std::acos(cosine);
        const double turn = std::numbers::pi - interior;
        if (turn < 1e-6 || interior < 1e-6) {
            result.anchors.push_back(corner);
            continue;
        }
        // The curve touches each side `reach` from the corner; no more than half of a side, so two
        // rounded corners on one side never overlap.
        const double wanted = radius * std::tan(turn / 2.0);
        const double reach = std::min({wanted, lengthBefore / 2.0, lengthAfter / 2.0});
        const double effective = reach / std::tan(turn / 2.0);
        const double handle = 4.0 / 3.0 * std::tan(turn / 4.0) * effective;
        PathAnchor first;
        first.point = {corner.point.x + toBefore.x * reach, corner.point.y + toBefore.y * reach};
        first.outHandle =
            Vec2d{first.point.x - toBefore.x * handle, first.point.y - toBefore.y * handle};
        first.mode = PathAnchorMode::Free;
        PathAnchor second;
        second.point = {corner.point.x + toAfter.x * reach, corner.point.y + toAfter.y * reach};
        second.inHandle =
            Vec2d{second.point.x - toAfter.x * handle, second.point.y - toAfter.y * handle};
        second.mode = PathAnchorMode::Free;
        result.anchors.push_back(first);
        result.anchors.push_back(second);
    }
    return result;
}

ValueUtilityOutcome roundCorners(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto& path = reader.path(0);
    const double radius = reader.scalar(1);
    if (reader.malformed())
        return outcome;
    if (!std::isfinite(radius))
        return refuse(std::move(outcome), "Round Corners radius is not finite",
                      "The radius must be a finite number.");
    if (!coordinatesUsable(path))
        return refuse(std::move(outcome), "Round Corners path is out of range",
                      "Every coordinate must be finite and within a billion pixels.");
    if (radius <= 0.0) {
        publish(outcome, path);
        return outcome;
    }
    PathValue result;
    result.fillRule = path.fillRule;
    result.contours.reserve(path.contours.size());
    for (const auto& contour : path.contours) {
        auto rounded = roundContour(contour, radius, invocation);
        if (!rounded)
            return cancelledOutcome(invocation);
        result.contours.push_back(std::move(*rounded));
    }
    if (!fitsValue(result))
        return refuse(std::move(outcome), "Round Corners result is too large",
                      "Rounding every corner would exceed the anchors a path may hold.");
    result.groups = path.groups;
    publish(outcome, std::move(result));
    return outcome;
}

// ---------------------------------------------------------------------------------------------
// Trim Path
// ---------------------------------------------------------------------------------------------

struct Segment {
    std::array<Vec2d, 4> controls{};
    bool straight = true;
    double length = 0.0;
    // Chord length up to parameter i / kLengthSamples, for a curve.
    std::vector<double> table;
};

[[nodiscard]] Vec2d evaluate(const std::array<Vec2d, 4>& c, const double t) noexcept {
    const auto p01 = lerp(c[0], c[1], t);
    const auto p12 = lerp(c[1], c[2], t);
    const auto p23 = lerp(c[2], c[3], t);
    return lerp(lerp(p01, p12, t), lerp(p12, p23, t), t);
}

// The curve from parameter `t0` to `t1`, exactly: the two de Casteljau splits.
[[nodiscard]] std::array<Vec2d, 4> subcurve(const std::array<Vec2d, 4>& c, const double t0,
                                            const double t1) noexcept {
    const auto splitLeft = [](const std::array<Vec2d, 4>& k, const double t) {
        const auto p01 = lerp(k[0], k[1], t);
        const auto p12 = lerp(k[1], k[2], t);
        const auto p23 = lerp(k[2], k[3], t);
        const auto p012 = lerp(p01, p12, t);
        const auto p123 = lerp(p12, p23, t);
        return std::array<Vec2d, 4>{k[0], p01, p012, lerp(p012, p123, t)};
    };
    const auto splitRight = [](const std::array<Vec2d, 4>& k, const double t) {
        const auto p01 = lerp(k[0], k[1], t);
        const auto p12 = lerp(k[1], k[2], t);
        const auto p23 = lerp(k[2], k[3], t);
        const auto p012 = lerp(p01, p12, t);
        const auto p123 = lerp(p12, p23, t);
        return std::array<Vec2d, 4>{lerp(p012, p123, t), p123, p23, k[3]};
    };
    const auto head = t1 >= 1.0 ? c : splitLeft(c, t1);
    if (t0 <= 0.0)
        return head;
    return splitRight(head, t1 > 0.0 ? t0 / t1 : 0.0);
}

// Empty when cancelled (a contour with no segment is never measured).
[[nodiscard]] std::vector<Segment> measure(const PathContour& contour,
                                           const ValueUtilityInvocation& invocation) {
    std::vector<Segment> segments;
    const auto count = document::contourSegmentCount(contour);
    segments.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (stopRequested(invocation))
            return {};
        Segment segment;
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
        segments.push_back(std::move(segment));
    }
    return segments;
}

[[nodiscard]] double parameterAt(const Segment& segment, const double along) noexcept {
    if (!(segment.length > 0.0))
        return 0.0;
    if (along <= 0.0)
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

[[nodiscard]] PathAnchorMode derivedMode(const PathAnchor& anchor) noexcept {
    return anchor.inHandle || anchor.outHandle ? PathAnchorMode::Free : PathAnchorMode::Corner;
}

struct Piece {
    PathContour contour;
    bool atStart = false; // begins at the contour's first anchor
    bool atEnd = false;   // ends at the contour's closing point
};

// The open piece of a contour from distance `from` to distance `to` along it.
[[nodiscard]] Piece extract(const PathContour& source, const std::vector<Segment>& segments,
                            const double from, const double to,
                            const ValueUtilityInvocation& invocation) {
    Piece piece;
    piece.contour.closed = false;
    auto& anchors = piece.contour.anchors;
    double base = 0.0;
    double total = 0.0;
    for (const auto& segment : segments)
        total += segment.length;
    piece.atStart = from <= 0.0;
    piece.atEnd = to >= total;
    for (std::size_t index = 0; index < segments.size(); ++index) {
        if (stopRequested(invocation))
            return {};
        const auto& segment = segments[index];
        const double low = base;
        const double high = base + segment.length;
        base = high;
        if (!(segment.length > 0.0) || high <= from || low >= to)
            continue;
        const double t0 = parameterAt(segment, std::max(from, low) - low);
        const double t1 = parameterAt(segment, std::min(to, high) - low);
        if (!(t1 > t0))
            continue;
        // A straight segment is cut where its length says; its controls would not be uniform.
        auto controls =
            (t0 <= 0.0 && t1 >= 1.0) ? segment.controls : subcurve(segment.controls, t0, t1);
        if (segment.straight) {
            const auto from_ = segment.controls[0];
            const auto to_ = segment.controls[3];
            controls = {lerp(from_, to_, t0), lerp(from_, to_, t0), lerp(from_, to_, t1),
                        lerp(from_, to_, t1)};
        }
        const auto& startSource = source.anchors[index];
        const auto& endSource = source.anchors[(index + 1) % source.anchors.size()];
        if (anchors.empty()) {
            PathAnchor start;
            start.point = controls[0];
            start.mode = t0 <= 0.0 ? startSource.mode : PathAnchorMode::Corner;
            anchors.push_back(start);
        }
        if (!segment.straight)
            anchors.back().outHandle = controls[1];
        PathAnchor end;
        end.point = controls[3];
        if (!segment.straight)
            end.inHandle = controls[2];
        end.mode = t1 >= 1.0 ? endSource.mode : derivedMode(end);
        anchors.push_back(end);
    }
    if (anchors.empty())
        piece.contour.anchors.clear();
    return piece;
}

// Two pieces that meet at a closed contour's first anchor, as one.
[[nodiscard]] PathContour joinPieces(PathContour tail, const PathContour& head) {
    tail.anchors.back().outHandle = head.anchors.front().outHandle;
    for (std::size_t index = 1; index < head.anchors.size(); ++index)
        tail.anchors.push_back(head.anchors[index]);
    return tail;
}

ValueUtilityOutcome trimPath(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto& path = reader.path(0);
    const double start = reader.scalar(1);
    const double end = reader.scalar(2);
    const double offset = reader.scalar(3);
    const auto mode = runtime::detail::selectorAt(invocation, 0, document::kTrimModes,
                                                  document::TrimMode::Simultaneously);
    if (reader.malformed())
        return outcome;
    if (!std::isfinite(start) || !std::isfinite(end) || !std::isfinite(offset))
        return refuse(std::move(outcome), "Trim Path operand is not finite",
                      "The start, end and offset must be finite numbers.");
    if (!coordinatesUsable(path))
        return refuse(std::move(outcome), "Trim Path path is out of range",
                      "Every coordinate must be finite and within a billion pixels.");

    const auto count = path.contours.size();
    std::vector<std::vector<Segment>> measured;
    std::vector<double> lengths;
    measured.reserve(count);
    lengths.reserve(count);
    double total = 0.0;
    for (const auto& contour : path.contours) {
        measured.push_back(measure(contour, invocation));
        if (stopRequested(invocation))
            return cancelledOutcome(invocation);
        double length = 0.0;
        for (const auto& segment : measured.back())
            length += segment.length;
        lengths.push_back(length);
        total += length;
    }
    outcome.outputs[1] = total;

    PathValue result;
    result.fillRule = path.fillRule;
    std::vector<std::uint32_t> produced(count, 0);
    bool unchanged = true;
    const auto keep = [&](const std::size_t index) {
        result.contours.push_back(path.contours[index]);
        produced[index] = 1;
    };
    const auto addPiece = [&](const std::size_t index, PathContour contour) {
        if (contour.anchors.size() < 2)
            return;
        result.contours.push_back(std::move(contour));
        ++produced[index];
        unchanged = false;
    };

    if (mode == document::TrimMode::Individually) {
        for (std::size_t index = 0; index < count; ++index) {
            const auto& contour = path.contours[index];
            const auto windows = windowsFor(start, end, offset, contour.closed);
            if (windows.size() == 1 && windows[0].first <= 0.0 && windows[0].second >= 1.0) {
                keep(index);
                continue;
            }
            unchanged = false;
            if (!(lengths[index] > 0.0))
                continue;
            std::vector<Piece> pieces;
            for (const auto& [low, high] : windows)
                pieces.push_back(
                    extract(contour, measured[index], low * lengths[index], high * lengths[index],
                            invocation));
            if (stopRequested(invocation))
                return cancelledOutcome(invocation);
            if (pieces.size() == 2 && contour.closed && pieces[0].contour.anchors.size() >= 2 &&
                pieces[1].contour.anchors.size() >= 2 && pieces[0].atEnd && pieces[1].atStart) {
                addPiece(index, joinPieces(pieces[0].contour, pieces[1].contour));
            } else {
                for (auto& piece : pieces)
                    addPiece(index, std::move(piece.contour));
            }
        }
    } else {
        const bool wrap = count > 0 && std::ranges::all_of(path.contours, [](const auto& contour) {
                              return contour.closed;
                          });
        const auto windows = windowsFor(start, end, offset, wrap);
        if (windows.size() == 1 && windows[0].first <= 0.0 && windows[0].second >= 1.0) {
            for (std::size_t index = 0; index < count; ++index)
                keep(index);
        } else {
            unchanged = false;
            double base = 0.0;
            for (std::size_t index = 0; index < count; ++index) {
                const double length = lengths[index];
                const double from = base;
                base += length;
                if (!(length > 0.0))
                    continue;
                std::vector<Piece> pieces;
                for (const auto& [low, high] : windows) {
                    const double a = std::max(low * total, from);
                    const double b = std::min(high * total, from + length);
                    if (!(b > a))
                        continue;
                    pieces.push_back(extract(path.contours[index], measured[index], a - from,
                                             b - from, invocation));
                }
                if (stopRequested(invocation))
                    return cancelledOutcome(invocation);
                if (pieces.size() == 2 && path.contours[index].closed &&
                    pieces[0].contour.anchors.size() >= 2 &&
                    pieces[1].contour.anchors.size() >= 2 && pieces[0].atEnd && pieces[1].atStart) {
                    addPiece(index, joinPieces(pieces[0].contour, pieces[1].contour));
                } else {
                    for (auto& piece : pieces)
                        addPiece(index, std::move(piece.contour));
                }
            }
        }
    }
    if (unchanged) {
        // Nothing was cut: the answer is the operand itself, groups included.
        publish(outcome, path);
        return outcome;
    }
    if (!fitsValue(result))
        return refuse(shapedOutcome(invocation.operation), "Trim Path result is too large",
                      "Trimming would exceed the contours or anchors a path may hold.");
    carryGroups(path, produced, result);
    publish(outcome, std::move(result));
    return outcome;
}

// ---------------------------------------------------------------------------------------------
// Polygon operations: Offset Path, Boolean Paths, Merge Paths
// ---------------------------------------------------------------------------------------------

// One contour as a polygon, flattened at `tolerance`. Fewer than three points is no area.
// nullopt when flattening ran past its segment budget or the kernel was cancelled inside it (the
// caller asks stopRequested() to tell which).
[[nodiscard]] std::optional<Clipper2Lib::PathD> polygonOf(const PathContour& contour,
                                                          const double tolerance,
                                                          const ValueUtilityInvocation& invocation) {
    auto flattened = render::flattenPathContour(renderPath(contour, true), tolerance,
                                                [&invocation] { return stopRequested(invocation); });
    if (!flattened)
        return std::nullopt;
    Clipper2Lib::PathD polygon;
    polygon.reserve(flattened->size());
    for (const auto& point : *flattened)
        polygon.emplace_back(point.x, point.y);
    if (polygon.size() < 3)
        polygon.clear();
    return polygon;
}

[[nodiscard]] Clipper2Lib::FillRule clipRule(const document::PathFillRule rule) noexcept {
    // A value that names no rule is read as non-zero.
    return rule == document::PathFillRule::EvenOdd ? Clipper2Lib::FillRule::EvenOdd
                                                   : Clipper2Lib::FillRule::NonZero;
}

// Every contour of a value as polygons, or nullopt when one is too detailed to flatten or the
// kernel was cancelled (`cancelled` says which).
[[nodiscard]] std::optional<Clipper2Lib::PathsD>
polygonsOf(const PathValue& value, const double tolerance, const bool closedOnly,
           const ValueUtilityInvocation& invocation, bool& cancelled) {
    Clipper2Lib::PathsD polygons;
    for (const auto& contour : value.contours) {
        if (stopRequested(invocation)) {
            cancelled = true;
            return std::nullopt;
        }
        if (closedOnly && !contour.closed)
            continue;
        auto polygon = polygonOf(contour, tolerance, invocation);
        if (!polygon) {
            cancelled = stopRequested(invocation);
            return std::nullopt;
        }
        if (!polygon->empty())
            polygons.push_back(std::move(*polygon));
    }
    return polygons;
}

ValueUtilityOutcome offsetPath(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto& path = reader.path(0);
    const double amount = reader.scalar(1);
    const double miterLimit = reader.scalar(2);
    const double tolerance = reader.scalar(3);
    const auto join = runtime::detail::selectorAt(invocation, 0, document::kOffsetJoins,
                                                  document::OffsetJoin::Miter);
    if (reader.malformed())
        return outcome;
    if (!std::isfinite(amount) || !std::isfinite(miterLimit) ||
        std::abs(amount) > document::kMaximumPathCoordinate)
        return refuse(std::move(outcome), "Offset Path amount is not usable",
                      "The amount must be a finite number within a billion pixels.");
    if (!toleranceUsable(tolerance))
        return refuse(std::move(outcome), "Offset Path tolerance is not usable",
                      "The tolerance must be between 0.001 and 1000 pixels.");
    if (!coordinatesUsable(path))
        return refuse(std::move(outcome), "Offset Path path is out of range",
                      "Every coordinate must be finite and within a billion pixels.");
    if (amount == 0.0) {
        publish(outcome, path);
        return outcome;
    }
    try {
        bool cancelled = false;
        auto polygons = polygonsOf(path, tolerance, true, invocation, cancelled);
        if (cancelled)
            return cancelledOutcome(invocation);
        if (!polygons)
            return refuse(std::move(outcome), "Offset Path is too detailed",
                          "The path has more flattened segments than can be offset; raise the "
                          "tolerance.");
        // Orientation and overlap are settled first, so the offset grows the filled area whichever
        // way the contours were wound.
        const auto settled = Clipper2Lib::Union(*polygons, clipRule(path.fillRule), kClipPrecision);
        if (stopRequested(invocation))
            return cancelledOutcome(invocation);
        const auto joinType = join == document::OffsetJoin::Round   ? Clipper2Lib::JoinType::Round
                              : join == document::OffsetJoin::Bevel ? Clipper2Lib::JoinType::Bevel
                                                                    : Clipper2Lib::JoinType::Miter;
        const auto grown = Clipper2Lib::InflatePaths(
            settled, amount, joinType, Clipper2Lib::EndType::Polygon,
            std::clamp(miterLimit, 1.0, 100.0), kClipPrecision, tolerance);
        if (stopRequested(invocation))
            return cancelledOutcome(invocation);
        auto converted = valueOf(grown, invocation);
        if (!converted)
            return cancelledOutcome(invocation);
        auto result = std::move(*converted);
        // An open contour has no inside to offset: it is kept as it is.
        for (const auto& contour : path.contours)
            if (!contour.closed)
                result.contours.push_back(contour);
        if (!fitsValue(result))
            return refuse(std::move(outcome), "Offset Path result is too large",
                          "The offset path has a contour with more anchors than a path may hold; "
                          "raise the tolerance.");
        publish(outcome, std::move(result));
    } catch (...) {
        return refuse(shapedOutcome(invocation.operation), "Offset Path could not be computed",
                      "The path could not be offset at this amount.");
    }
    return outcome;
}

[[nodiscard]] Clipper2Lib::PathsD combine(const Clipper2Lib::PathsD& left,
                                          const Clipper2Lib::PathsD& right,
                                          const document::PathCombine operation) {
    constexpr auto rule = Clipper2Lib::FillRule::NonZero;
    switch (operation) {
    case document::PathCombine::Union:
        return Clipper2Lib::Union(left, right, rule, kClipPrecision);
    case document::PathCombine::Subtract:
        return Clipper2Lib::Difference(left, right, rule, kClipPrecision);
    case document::PathCombine::Intersect:
        return Clipper2Lib::Intersect(left, right, rule, kClipPrecision);
    case document::PathCombine::Exclude:
        return Clipper2Lib::Xor(left, right, rule, kClipPrecision);
    }
    return left;
}

ValueUtilityOutcome booleanPaths(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto& first = reader.path(0);
    const auto& second = reader.path(1);
    const double tolerance = reader.scalar(2);
    const auto operation = runtime::detail::selectorAt(invocation, 0, document::kPathCombines,
                                                       document::PathCombine::Union);
    if (reader.malformed())
        return outcome;
    if (!toleranceUsable(tolerance))
        return refuse(std::move(outcome), "Boolean Paths tolerance is not usable",
                      "The tolerance must be between 0.001 and 1000 pixels.");
    if (!coordinatesUsable(first) || !coordinatesUsable(second))
        return refuse(std::move(outcome), "Boolean Paths path is out of range",
                      "Every coordinate must be finite and within a billion pixels.");
    try {
        bool cancelled = false;
        const auto left = polygonsOf(first, tolerance, false, invocation, cancelled);
        std::optional<Clipper2Lib::PathsD> right;
        if (!cancelled)
            right = polygonsOf(second, tolerance, false, invocation, cancelled);
        if (cancelled)
            return cancelledOutcome(invocation);
        if (!left || !right)
            return refuse(std::move(outcome), "Boolean Paths is too detailed",
                          "A path has more flattened segments than can be combined; raise the "
                          "tolerance.");
        // Each side is a filled shape under its own fill rule; settle each, then combine.
        const auto settledFirst =
            Clipper2Lib::Union(*left, clipRule(first.fillRule), kClipPrecision);
        const auto settledSecond =
            Clipper2Lib::Union(*right, clipRule(second.fillRule), kClipPrecision);
        if (stopRequested(invocation))
            return cancelledOutcome(invocation);
        const auto combined = combine(settledFirst, settledSecond, operation);
        if (stopRequested(invocation))
            return cancelledOutcome(invocation);
        auto converted = valueOf(combined, invocation);
        if (!converted)
            return cancelledOutcome(invocation);
        auto result = std::move(*converted);
        if (!fitsValue(result))
            return refuse(std::move(outcome), "Boolean Paths result is too large",
                          "The result has a contour with more anchors than a path may hold; raise "
                          "the tolerance.");
        publish(outcome, std::move(result));
    } catch (...) {
        return refuse(shapedOutcome(invocation.operation), "Boolean Paths could not be computed",
                      "The paths could not be combined.");
    }
    return outcome;
}

ValueUtilityOutcome mergePaths(const ValueUtilityInvocation& invocation) {
    auto outcome = shapedOutcome(invocation.operation);
    ValueUtilityReader reader(invocation);
    const auto& path = reader.path(0);
    const double tolerance = reader.scalar(1);
    const auto operation = runtime::detail::selectorAt(invocation, 0, document::kPathCombines,
                                                       document::PathCombine::Union);
    if (reader.malformed())
        return outcome;
    if (!toleranceUsable(tolerance))
        return refuse(std::move(outcome), "Merge Paths tolerance is not usable",
                      "The tolerance must be between 0.001 and 1000 pixels.");
    if (!coordinatesUsable(path))
        return refuse(std::move(outcome), "Merge Paths path is out of range",
                      "Every coordinate must be finite and within a billion pixels.");
    try {
        // Each contour is one shape; the first is combined with each later one in order.
        std::vector<Clipper2Lib::PathsD> shapes;
        for (const auto& contour : path.contours) {
            if (stopRequested(invocation))
                return cancelledOutcome(invocation);
            auto polygon = polygonOf(contour, tolerance, invocation);
            if (!polygon && stopRequested(invocation))
                return cancelledOutcome(invocation);
            if (!polygon)
                return refuse(std::move(outcome), "Merge Paths is too detailed",
                              "The path has more flattened segments than can be merged; raise the "
                              "tolerance.");
            if (polygon->empty())
                continue;
            shapes.push_back(Clipper2Lib::Union(Clipper2Lib::PathsD{std::move(*polygon)},
                                                Clipper2Lib::FillRule::NonZero, kClipPrecision));
        }
        Clipper2Lib::PathsD merged;
        if (stopRequested(invocation))
            return cancelledOutcome(invocation);
        if (operation == document::PathCombine::Union) {
            Clipper2Lib::PathsD all;
            for (auto& shape : shapes)
                all.insert(all.end(), shape.begin(), shape.end());
            merged = Clipper2Lib::Union(all, Clipper2Lib::FillRule::NonZero, kClipPrecision);
        } else if (!shapes.empty()) {
            merged = shapes.front();
            for (std::size_t index = 1; index < shapes.size(); ++index) {
                if (stopRequested(invocation))
                    return cancelledOutcome(invocation);
                merged = combine(merged, shapes[index], operation);
            }
        }
        if (stopRequested(invocation))
            return cancelledOutcome(invocation);
        auto converted = valueOf(merged, invocation);
        if (!converted)
            return cancelledOutcome(invocation);
        auto result = std::move(*converted);
        if (!fitsValue(result))
            return refuse(std::move(outcome), "Merge Paths result is too large",
                          "The result has a contour with more anchors than a path may hold; raise "
                          "the tolerance.");
        publish(outcome, std::move(result));
    } catch (...) {
        return refuse(shapedOutcome(invocation.operation), "Merge Paths could not be computed",
                      "The contours could not be merged.");
    }
    return outcome;
}

} // namespace

namespace bloom::runtime::detail {

ValueUtilityOutcome evaluateValuePath(const ValueUtilityInvocation& invocation) {
    switch (invocation.operation) {
    case Kernel::TransformPath:
        return transformPath(invocation);
    case Kernel::TrimPath:
        return trimPath(invocation);
    case Kernel::OffsetPath:
        return offsetPath(invocation);
    case Kernel::BooleanPaths:
        return booleanPaths(invocation);
    case Kernel::MergePaths:
        return mergePaths(invocation);
    case Kernel::RoundCorners:
        return roundCorners(invocation);
    case Kernel::PathStroke:
        return evaluateValuePathStroke(invocation);
    case Kernel::MaskPath:
        return evaluateValueMaskPath(invocation);
    case Kernel::TextOutline:
        return evaluateValueTextOutline(invocation);
    case Kernel::ResamplePath:
        return evaluateValuePathResample(invocation);
    default:
        break;
    }
    return shapedOutcome(invocation.operation);
}

} // namespace bloom::runtime::detail
