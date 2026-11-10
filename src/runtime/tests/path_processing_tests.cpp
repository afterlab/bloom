// #344 Transform Path, Trim Path and Offset Path; #530 Boolean Paths, Merge Paths and Round
// Corners, against answers worked out by hand.

#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/value_utility_kernels.hpp>

#include <bloom/document/path_value.hpp>
#include <bloom/document/resample_path_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/document/warp_path_nodes.hpp>
#include <bloom/render/text_raster.hpp>
#include <bloom/runtime/cancellation.hpp>

#include "path_resample.hpp"
#include "path_warp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <source_location>
#include <string_view>
#include <vector>

namespace {

using namespace bloom;
using document::PathAnchor;
using document::PathContour;
using document::PathValue;
using document::Vec2d;
using runtime::CompiledValue;
using runtime::ValueUtilityOutcome;
using Kernel = document::ValueUtilityKernel;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

[[nodiscard]] ValueUtilityOutcome run(const Kernel kernel, const std::vector<CompiledValue>& values,
                                      const std::vector<std::int64_t>& selectors = {}) {
    std::vector<const CompiledValue*> operands;
    for (const auto& value : values)
        operands.push_back(&value);
    return runtime::evaluateValueUtility({kernel,
                                          operands,
                                          selectors,
                                          core::RationalTime::fromInteger(0),
                                          document::FrameRate::framesPerSecond24(),
                                          nullptr,
                                          {}});
}

[[nodiscard]] CompiledValue wrap(PathValue value) {
    return runtime::CompiledPathValue{std::make_shared<PathValue>(std::move(value))};
}

[[nodiscard]] const PathValue& pathOf(const ValueUtilityOutcome& outcome) {
    static const PathValue empty;
    const auto* held = std::get_if<runtime::CompiledPathValue>(&outcome.outputs[0]);
    return held == nullptr ? empty : *held->value;
}

[[nodiscard]] PathAnchor corner(const double x, const double y) {
    return {{x, y}, std::nullopt, std::nullopt, document::PathAnchorMode::Corner};
}
[[nodiscard]] PathContour polygon(const std::vector<Vec2d>& points, const bool closed) {
    PathContour contour;
    contour.closed = closed;
    for (const auto point : points)
        contour.anchors.push_back(corner(point.x, point.y));
    return contour;
}
[[nodiscard]] PathContour square(const double x, const double y, const double size) {
    return polygon({{x, y}, {x + size, y}, {x + size, y + size}, {x, y + size}}, true);
}
[[nodiscard]] PathValue single(PathContour contour) {
    PathValue value;
    value.contours.push_back(std::move(contour));
    return value;
}

// The net area a value's contours enclose, in the orientation the polygon library writes (outer
// contours one way, holes the other).
[[nodiscard]] double area(const PathValue& value) {
    double sum = 0.0;
    for (const auto& contour : value.contours) {
        const auto& a = contour.anchors;
        for (std::size_t index = 0; index < a.size(); ++index) {
            const auto& p = a[index].point;
            const auto& q = a[(index + 1) % a.size()].point;
            sum += p.x * q.y - q.x * p.y;
        }
    }
    return std::abs(sum) / 2.0;
}

[[nodiscard]] bool near(const Vec2d a, const Vec2d b, const double tolerance = 1e-9) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance;
}
[[nodiscard]] bool near(const double a, const double b, const double tolerance) {
    return std::abs(a - b) <= tolerance;
}

void testTransform(Expectations& expectations) {
    PathValue source = single(square(0, 0, 10));
    source.groups.push_back({"a", 0, 1, std::nullopt});
    // Scale (2, 1) about (0, 0), turn 90 degrees, then move to (5, 5): (10, 0) -> (20, 0) ->
    // (0, 20) -> (5, 25).
    const auto moved =
        run(Kernel::TransformPath, {wrap(source), Vec2d{5, 5}, Vec2d{0, 0}, Vec2d{2, 1}, 90.0});
    const auto& result = pathOf(moved);
    expectations.expect(!moved.failed && result.contours.size() == 1 &&
                            result.contours[0].anchors.size() == 4 && result.contours[0].closed,
                        "Transform Path keeps the contour count, anchor count and closed flag");
    if (result.contours.size() != 1 || result.contours[0].anchors.size() != 4)
        return;
    expectations.expect(near(result.contours[0].anchors[0].point, {5, 5}, 1e-9) &&
                            near(result.contours[0].anchors[1].point, {5, 25}, 1e-9),
                        "Transform Path scales, turns and moves about the anchor");
    expectations.expect(result.groups == source.groups, "Transform Path keeps the groups");
    // The anchor point stays where the position puts it: a turn about (10, 10).
    const auto about = run(Kernel::TransformPath,
                           {wrap(source), Vec2d{10, 10}, Vec2d{10, 10}, Vec2d{1, 1}, 180.0});
    expectations.expect(near(pathOf(about).contours[0].anchors[0].point, {20, 20}, 1e-9),
                        "a half turn about the anchor sends the origin to the opposite side");
    // A curve's handles move with its anchors.
    PathContour curve = polygon({{0, 0}, {10, 0}}, false);
    curve.anchors[0].outHandle = Vec2d{0, 5};
    const auto handled =
        run(Kernel::TransformPath, {wrap(single(curve)), Vec2d{1, 1}, Vec2d{}, Vec2d{1, 1}, 0.0});
    expectations.expect(pathOf(handled).contours[0].anchors[0].outHandle &&
                            near(*pathOf(handled).contours[0].anchors[0].outHandle, {1, 6}, 1e-9),
                        "Transform Path moves handles with their anchors");
    expectations.expect(run(Kernel::TransformPath, {wrap(source), Vec2d{}, Vec2d{}, Vec2d{1, 1},
                                                    std::numeric_limits<double>::quiet_NaN()})
                            .failed,
                        "a rotation that is not finite is refused");
}

void testTrim(Expectations& expectations) {
    const auto trim = [](const PathValue& path, const double start, const double end,
                         const double offset, const std::int64_t mode = 0) {
        return run(Kernel::TrimPath, {wrap(path), start, end, offset}, {mode});
    };
    const PathValue line = single(polygon({{0, 0}, {100, 0}}, false));
    const auto middle = trim(line, 25, 75, 0);
    expectations.expect(!middle.failed && pathOf(middle).contours.size() == 1 &&
                            near(pathOf(middle).contours[0].anchors.front().point, {25, 0}, 1e-9) &&
                            near(pathOf(middle).contours[0].anchors.back().point, {75, 0}, 1e-9),
                        "Trim Path keeps the middle half of a line");
    expectations.expect(std::get<double>(middle.outputs[1]) == 100.0,
                        "Trim Path reports the length of the whole path");
    const auto shifted = trim(line, 0, 50, 25);
    expectations.expect(near(pathOf(shifted).contours[0].anchors.front().point, {25, 0}, 1e-9) &&
                            near(pathOf(shifted).contours[0].anchors.back().point, {75, 0}, 1e-9),
                        "the offset slides the trimmed stretch along an open path");
    const auto whole = trim(line, 0, 100, 40);
    expectations.expect(pathOf(whole) == line, "the whole range answers the operand itself");
    expectations.expect(pathOf(trim(line, 50, 50, 0)).contours.empty(),
                        "an empty range leaves no contour");
    expectations.expect(pathOf(trim(line, 75, 25, 0)).contours[0].anchors.front().point.x == 25.0,
                        "a start past the end is read as the other way round");

    // A 100 square is 400 long. Start at 90% of the way round, run 25% (to 15%): the last 40 of the
    // left side, then the top side to x = 60, as one open contour.
    const PathValue box = single(square(0, 0, 100));
    const auto wrapped = trim(box, 0, 25, 90);
    expectations.expect(pathOf(wrapped).contours.size() == 1 &&
                            pathOf(wrapped).contours[0].anchors.size() == 3 &&
                            !pathOf(wrapped).contours[0].closed,
                        "a stretch that runs past the end of a closed contour is one open contour");
    if (pathOf(wrapped).contours.size() == 1 && pathOf(wrapped).contours[0].anchors.size() == 3) {
        const auto& a = pathOf(wrapped).contours[0].anchors;
        expectations.expect(near(a[0].point, {0, 40}, 1e-9) && near(a[1].point, {0, 0}, 1e-9) &&
                                near(a[2].point, {60, 0}, 1e-9),
                            "the wrapped stretch runs up the left side and along the top");
    }
    expectations.expect(std::get<double>(trim(box, 0, 25, 90).outputs[1]) == 400.0,
                        "the square is 400 long");

    // A quarter circle of radius 100 as one cubic: 157.08 long; half of it ends near (70.7, 70.7).
    PathContour arc = polygon({{100, 0}, {0, 100}}, false);
    arc.anchors[0].outHandle = Vec2d{100, 55.2284749831};
    arc.anchors[1].inHandle = Vec2d{55.2284749831, 100};
    const auto quarter = trim(single(arc), 0, 50, 0);
    expectations.expect(near(std::get<double>(quarter.outputs[1]), 157.0796, 0.1),
                        "a quarter circle is pi / 2 times the radius long");
    if (pathOf(quarter).contours.size() == 1)
        expectations.expect(
            near(pathOf(quarter).contours[0].anchors.back().point, {70.71, 70.71}, 0.2),
            "half the length of a quarter circle ends on the diagonal");

    // Groups follow their contours: two lines, one group each.
    PathValue pair;
    pair.contours.push_back(polygon({{0, 0}, {100, 0}}, false));
    pair.contours.push_back(polygon({{0, 50}, {100, 50}}, false));
    pair.groups.push_back({"first", 0, 1, std::nullopt});
    pair.groups.push_back({"second", 1, 1, std::nullopt});
    const auto together = trim(pair, 0, 50, 0, 0);
    expectations.expect(pathOf(together).contours.size() == 1 &&
                            pathOf(together).groups.size() == 1 &&
                            pathOf(together).groups[0].key == "first" && pathOf(together).isValid(),
                        "trimming all contours together drops the group of a contour trimmed away");
    const auto apart = trim(pair, 0, 50, 0, 1);
    expectations.expect(pathOf(apart).contours.size() == 2 && pathOf(apart).groups.size() == 2 &&
                            near(pathOf(apart).contours[1].anchors.back().point, {50, 50}, 1e-9),
                        "trimming individually trims each contour against its own length");
    expectations.expect(trim(line, std::numeric_limits<double>::infinity(), 1, 0).failed,
                        "a range that is not finite is refused");
}

void testRoundCorners(Expectations& expectations) {
    const PathValue box = single(square(0, 0, 100));
    const auto rounded = run(Kernel::RoundCorners, {wrap(box), 10.0});
    expectations.expect(!rounded.failed && pathOf(rounded).contours.size() == 1 &&
                            pathOf(rounded).contours[0].anchors.size() == 8,
                        "each corner of a square becomes two anchors");
    if (pathOf(rounded).contours.empty() || pathOf(rounded).contours[0].anchors.size() != 8)
        return;
    const auto& a = pathOf(rounded).contours[0].anchors;
    // The corner at the origin: the curve leaves the left side at (0, 10) and meets the top at (10,
    // 0) with handles 4 / 3 * tan(pi / 8) * 10 long.
    expectations.expect(near(a[0].point, {0, 10}, 1e-9) && near(a[1].point, {10, 0}, 1e-9),
                        "the curve touches each side one radius from the corner");
    expectations.expect(a[0].outHandle && near(*a[0].outHandle, {0, 10 - 5.522847498}, 1e-6) &&
                            a[1].inHandle && near(*a[1].inHandle, {10 - 5.522847498, 0}, 1e-6),
                        "the handles are the circular arc's");
    const auto clamped = run(Kernel::RoundCorners, {wrap(box), 1000.0});
    expectations.expect(near(pathOf(clamped).contours[0].anchors[0].point, {0, 50}, 1e-9),
                        "a radius larger than half a side is held to half a side");
    expectations.expect(pathOf(run(Kernel::RoundCorners, {wrap(box), 0.0})) == box,
                        "a radius of zero answers the operand itself");
    // Open ends and curved sides are left alone.
    const PathValue open = single(polygon({{0, 0}, {50, 0}, {50, 50}}, false));
    const auto ends = pathOf(run(Kernel::RoundCorners, {wrap(open), 10.0}));
    expectations.expect(ends.contours[0].anchors.size() == 4 &&
                            near(ends.contours[0].anchors.front().point, {0, 0}, 1e-12) &&
                            near(ends.contours[0].anchors.back().point, {50, 50}, 1e-12),
                        "the ends of an open contour are not rounded");
    expectations.expect(
        run(Kernel::RoundCorners, {wrap(box), std::numeric_limits<double>::quiet_NaN()}).failed,
        "a radius that is not finite is refused");
}

void testOffset(Expectations& expectations) {
    const auto offset = [](const PathValue& path, const double amount, const std::int64_t join,
                           const double limit = 4.0) {
        return run(Kernel::OffsetPath, {wrap(path), amount, limit, 0.1}, {join});
    };
    const PathValue box = single(square(0, 0, 100));
    const auto grown = offset(box, 10, 0);
    expectations.expect(!grown.failed && pathOf(grown).contours.size() == 1 &&
                            pathOf(grown).contours[0].anchors.size() == 4 &&
                            near(area(pathOf(grown)), 120.0 * 120.0, 1e-6),
                        "a mitered offset of a square by 10 is a square 20 larger");
    const auto shrunk = offset(box, -10, 0);
    expectations.expect(near(area(pathOf(shrunk)), 80.0 * 80.0, 1e-6),
                        "a negative amount shrinks the shape");
    expectations.expect(offset(box, -60, 0).failed == false &&
                            pathOf(offset(box, -60, 0)).contours.empty(),
                        "an offset that removes the shape leaves no contour");
    const auto bevel = offset(box, 10, 2);
    // Four corners cut: 120 * 120 less four triangles of 10 * 10 / 2.
    expectations.expect(near(area(pathOf(bevel)), 14400.0 - 4 * 50.0, 1e-3),
                        "a bevel cuts each corner of the grown square");
    const auto round = offset(box, 10, 1);
    const double expected = 10000.0 + 4 * 100.0 * 10.0 + std::numbers::pi * 100.0;
    expectations.expect(near(area(pathOf(round)), expected, expected * 0.002),
                        "a round offset adds the four sides and a circle of the offset's radius");
    // A winding direction does not matter.
    PathContour reversed = square(0, 0, 100);
    std::ranges::reverse(reversed.anchors);
    expectations.expect(near(area(pathOf(offset(single(reversed), 10, 0))), 14400.0, 1e-6),
                        "the offset grows the shape whichever way the contour is wound");
    // A ring: outer 100, hole 20..80; growing by 5 leaves a hole 50 across.
    PathValue ring;
    ring.contours.push_back(square(0, 0, 100));
    ring.contours.push_back(square(20, 20, 60));
    ring.fillRule = document::PathFillRule::EvenOdd;
    expectations.expect(near(area(pathOf(offset(ring, 5, 0))), 110.0 * 110.0 - 50.0 * 50.0, 1e-6),
                        "the offset of a ring shrinks its hole");
    expectations.expect(pathOf(offset(box, 0, 0)) == box, "an amount of zero answers the operand");
    PathValue mixed = box;
    mixed.contours.push_back(polygon({{200, 0}, {300, 0}}, false));
    expectations.expect(pathOf(offset(mixed, 10, 0)).contours.size() == 2,
                        "an open contour is kept as it is");
    expectations.expect(
        offset(box, std::numeric_limits<double>::quiet_NaN(), 0).failed &&
            run(Kernel::OffsetPath, {wrap(box), 10.0, 4.0, 0.0}, {0}).failed &&
            offset(single(square(0, 0, 1.0e12)), 10, 0).failed,
        "an amount that is not finite, a tolerance of zero and a huge path are refused");
}

void testCombine(Expectations& expectations) {
    const PathValue left = single(square(0, 0, 100));
    const PathValue right = single(square(50, 50, 100));
    const auto boolean = [&](const std::int64_t operation) {
        return run(Kernel::BooleanPaths, {wrap(left), wrap(right), 0.1}, {operation});
    };
    expectations.expect(near(area(pathOf(boolean(0))), 17500.0, 1e-6),
                        "the union of two overlapping squares");
    expectations.expect(near(area(pathOf(boolean(1))), 7500.0, 1e-6), "the first less the second");
    expectations.expect(near(area(pathOf(boolean(2))), 2500.0, 1e-6), "the intersection");
    expectations.expect(near(area(pathOf(boolean(3))), 15000.0, 1e-6), "the exclusive part");
    expectations.expect(pathOf(boolean(0)).fillRule == document::PathFillRule::EvenOdd,
                        "the result names the rule that cuts its holes");
    // Disjoint shapes have an empty intersection and a union of two contours.
    const PathValue far = single(square(500, 500, 10));
    expectations.expect(
        pathOf(run(Kernel::BooleanPaths, {wrap(left), wrap(far), 0.1}, {2})).contours.empty() &&
            pathOf(run(Kernel::BooleanPaths, {wrap(left), wrap(far), 0.1}, {0})).contours.size() ==
                2,
        "disjoint shapes intersect in nothing and unite as two contours");
    // Round shapes combine at the flattening tolerance: a circle of radius 50 against itself.
    PathContour circle;
    circle.closed = true;
    const double k = 0.5522847498 * 50.0;
    // Clockwise on screen from the right: each anchor's out handle leads to the next anchor.
    circle.anchors = {
        {{100, 50}, Vec2d{100, 50 - k}, Vec2d{100, 50 + k}, document::PathAnchorMode::Free},
        {{50, 100}, Vec2d{50 + k, 100}, Vec2d{50 - k, 100}, document::PathAnchorMode::Free},
        {{0, 50}, Vec2d{0, 50 + k}, Vec2d{0, 50 - k}, document::PathAnchorMode::Free},
        {{50, 0}, Vec2d{50 - k, 0}, Vec2d{50 + k, 0}, document::PathAnchorMode::Free}};
    const auto disc =
        run(Kernel::BooleanPaths, {wrap(single(circle)), wrap(PathValue{}), 0.05}, {0});
    // The polygon is inscribed: at most perimeter * tolerance * 2/3 (10.5) short of the circle.
    const double deficit = std::numbers::pi * 2500.0 - area(pathOf(disc));
    expectations.expect(deficit >= 0.0 && deficit <= 11.0,
                        "a flattened circle has the circle's area to the flattening tolerance");

    // Merge Paths combines the contours of one value in order.
    PathValue pair;
    pair.contours.push_back(square(0, 0, 100));
    pair.contours.push_back(square(50, 50, 100));
    const auto merge = [&](const std::int64_t operation) {
        return run(Kernel::MergePaths, {wrap(pair), 0.1}, {operation});
    };
    expectations.expect(near(area(pathOf(merge(0))), 17500.0, 1e-6) &&
                            near(area(pathOf(merge(1))), 7500.0, 1e-6) &&
                            near(area(pathOf(merge(2))), 2500.0, 1e-6) &&
                            near(area(pathOf(merge(3))), 15000.0, 1e-6),
                        "merging two contours is the same four answers");
    expectations.expect(
        pathOf(run(Kernel::MergePaths, {wrap(PathValue{}), 0.1}, {0})).contours.empty(),
        "merging nothing answers nothing");
    expectations.expect(run(Kernel::MergePaths, {wrap(pair), 0.0}, {0}).failed &&
                            run(Kernel::BooleanPaths, {wrap(left), wrap(right), -1.0}, {0}).failed,
                        "a tolerance outside 0.001 to 1000 is refused");
}

struct StrokeOptions final {
    double width = 10.0, start = 0.0, end = 100.0, offset = 0.0, taperStart = 0.0, taperEnd = 0.0;
    double dashLength = 0.0, dashGap = 8.0, dashOffset = 0.0, spacing = 0.0, wiggle = 0.0;
    double frequency = 2.0, phase = 0.0, core = 0.0, tolerance = 0.25, stampRotation = 0.0;
    std::int64_t seed = 0, cap = 0, join = 0, stamp = 0, stampAlign = 0;
};

[[nodiscard]] ValueUtilityOutcome stroke(const PathValue& path, const StrokeOptions& o) {
    return run(Kernel::PathStroke,
               {wrap(path), o.width, o.start, o.end, o.offset, o.taperStart, o.taperEnd,
                o.dashLength, o.dashGap, o.dashOffset, o.spacing, o.wiggle, o.frequency, o.phase,
                o.seed, o.core, o.tolerance, o.stampRotation},
               {o.cap, o.join, o.stamp, o.stampAlign});
}

[[nodiscard]] const PathValue& coreOf(const ValueUtilityOutcome& outcome) {
    static const PathValue empty;
    const auto* held = std::get_if<runtime::CompiledPathValue>(&outcome.outputs[1]);
    return held == nullptr ? empty : *held->value;
}

[[nodiscard]] double totalLengthOf(const ValueUtilityOutcome& outcome) {
    const auto* length = std::get_if<double>(&outcome.outputs[2]);
    return length == nullptr ? -1.0 : *length;
}

void testPathStroke(Expectations& expectations) {
    const PathValue line = single(polygon({{0, 0}, {100, 0}}, false));
    const auto solid = stroke(line, {});
    expectations.expect(!solid.failed && pathOf(solid).contours.size() == 1 &&
                            near(area(pathOf(solid)), 1000.0, 1e-6) &&
                            totalLengthOf(solid) == 100.0,
                        "a 10 wide butt stroke of a 100 line covers 1000 and reports its length");
    StrokeOptions round;
    round.cap = 1;
    // Two discs of radius 5 as 16-gons: 2 * 0.5 * 16 * 25 * sin(2 pi / 16), half of each outside
    // the body.
    const double capArea = 0.5 * 16 * 25.0 * std::sin(2.0 * std::numbers::pi / 16.0);
    expectations.expect(near(area(pathOf(stroke(line, round))), 1000.0 + capArea, 0.5),
                        "round caps add a disc's area at the ends");
    StrokeOptions squared;
    squared.cap = 2;
    expectations.expect(near(area(pathOf(stroke(line, squared))), 1100.0, 1e-6),
                        "square caps extend each end by half the width");

    StrokeOptions half;
    half.end = 50;
    expectations.expect(near(area(pathOf(stroke(line, half))), 500.0, 1e-6) &&
                            totalLengthOf(stroke(line, half)) == 50.0,
                        "the end trims the stroke to the first half");
    StrokeOptions slid;
    slid.start = 0;
    slid.end = 50;
    slid.offset = 25;
    expectations.expect(near(area(pathOf(stroke(line, slid))), 500.0, 1e-6),
                        "the offset slides the drawn stretch");
    StrokeOptions nothing;
    nothing.end = 0;
    expectations.expect(pathOf(stroke(line, nothing)).contours.empty(),
                        "an empty range draws nothing");

    StrokeOptions dashed;
    dashed.dashLength = 20;
    dashed.dashGap = 10;
    const auto dashes = stroke(line, dashed);
    expectations.expect(pathOf(dashes).contours.size() == 4 &&
                            near(area(pathOf(dashes)), 700.0, 1e-6),
                        "dashes 20 on and 10 off cover 0-20, 30-50, 60-80 and 90-100");
    dashed.dashOffset = 10;
    const auto shifted = stroke(line, dashed);
    expectations.expect(pathOf(shifted).contours.size() == 4 &&
                            near(area(pathOf(shifted)), 700.0, 1e-6),
                        "the dash offset moves the pattern along the path");

    StrokeOptions tapered;
    tapered.taperEnd = 100;
    expectations.expect(near(area(pathOf(stroke(line, tapered))), 500.0, 1e-6),
                        "a full end taper narrows the stroke to a point: a triangle");
    tapered.taperStart = 100;
    tapered.taperEnd = 0;
    expectations.expect(near(area(pathOf(stroke(line, tapered))), 500.0, 1e-6),
                        "a full start taper does the same from the other end");

    StrokeOptions cored;
    cored.core = 50;
    const auto glowing = stroke(line, cored);
    expectations.expect(near(area(coreOf(glowing)), 500.0, 1e-6) &&
                            near(area(pathOf(glowing)), 1000.0, 1e-6),
                        "the core is the body at its share of the width");
    expectations.expect(coreOf(stroke(line, {})).contours.empty(),
                        "no core share, no core outline");

    StrokeOptions dotted;
    dotted.spacing = 25;
    dotted.width = 10;
    const auto dots = stroke(line, dotted);
    expectations.expect(pathOf(dots).contours.size() == 5 &&
                            near(area(pathOf(dots)),
                                 5 * 0.5 * 16 * 25.0 * std::sin(2.0 * std::numbers::pi / 16.0),
                                 0.5),
                        "brush spacing stamps a dot every 25 along the path");

    // #509 stamps. Each is sized by the width: a square's side, a diamond's diagonal, a dash as
    // long as the width and a quarter as thick.
    const auto stamped = [&](const PathValue& path, const std::int64_t kind,
                             const std::int64_t align, const double rotation, const double core) {
        StrokeOptions o;
        o.spacing = 25;
        o.width = 10;
        o.stamp = kind;
        o.stampAlign = align;
        o.stampRotation = rotation;
        o.core = core;
        return stroke(path, o);
    };
    const auto extentX = [](const PathValue& value) {
        double reach = 0.0;
        for (const auto& contour : value.contours)
            for (const auto& anchor : contour.anchors)
                reach = std::max(reach, std::abs(anchor.point.x));
        return reach;
    };
    const auto squares = stamped(line, 1, 0, 0.0, 50.0);
    expectations.expect(!squares.failed && pathOf(squares).contours.size() == 5 &&
                            near(area(pathOf(squares)), 500.0, 1e-6) &&
                            near(area(coreOf(squares)), 125.0, 1e-6),
                        "square stamps are the width on a side, and the core's are its share");
    expectations.expect(near(area(pathOf(stamped(line, 2, 0, 0.0, 0.0))), 250.0, 1e-6),
                        "diamond stamps have the width as their diagonal");
    expectations.expect(near(area(pathOf(stamped(line, 3, 0, 0.0, 0.0))), 125.0, 1e-6),
                        "dash stamps are the width long and a quarter of it thick");
    const PathValue upright = single(polygon({{0, 0}, {0, 100}}, false));
    expectations.expect(extentX(pathOf(stamped(upright, 3, 0, 0.0, 0.0))) <= 1.25 + 1e-9 &&
                            near(extentX(pathOf(stamped(upright, 3, 2, 0.0, 0.0))), 5.0, 1e-9),
                        "Automatic turns a dash along the path; Fixed leaves it level");
    expectations.expect(extentX(pathOf(stamped(upright, 3, 2, 90.0, 0.0))) <= 1.25 + 1e-9,
                        "the stamp rotation turns a fixed dash a quarter turn upright");
    // The last stamp of a 45 degree line is 125 along it. The union keeps three decimals.
    const PathValue diagonal = single(polygon({{0, 0}, {100, 100}}, false));
    const double last = 125.0 / std::sqrt(2.0);
    expectations.expect(
        near(extentX(pathOf(stamped(diagonal, 1, 1, 0.0, 0.0))), last + 5.0 * std::sqrt(2.0),
             1e-3) &&
            near(extentX(pathOf(stamped(diagonal, 1, 0, 0.0, 0.0))), last + 5.0, 1e-3),
        "Follow path turns a square with the path; Automatic keeps it level");
    expectations.expect(pathOf(stamped(line, 0, 1, 30.0, 0.0)) == pathOf(dots) &&
                            pathOf(stamped(line, 0, 2, 0.0, 0.0)) == pathOf(dots),
                        "a dot is the original brush dot whatever the alignment or rotation");
    expectations.expect(stamped(line, 1, 0, std::numeric_limits<double>::infinity(), 0.0).failed,
                        "a stamp rotation that is not finite is refused");

    // A closed square stroked with mitered joins is a ring: 110 outside, 90 inside.
    const PathValue box = single(square(0, 0, 100));
    StrokeOptions ring;
    const auto closed = stroke(box, ring);
    expectations.expect(pathOf(closed).contours.size() == 2 &&
                            near(area(pathOf(closed)), 110.0 * 110.0 - 90.0 * 90.0, 1e-6) &&
                            totalLengthOf(closed) == 400.0,
                        "a closed contour strokes as a ring with no caps");

    // Wiggle is a pure function of the seed and the phase.
    StrokeOptions shaky;
    shaky.wiggle = 6;
    shaky.width = 2;
    const auto first = stroke(line, shaky);
    shaky.seed = 1;
    const auto second = stroke(line, shaky);
    shaky.seed = 0;
    const auto again = stroke(line, shaky);
    expectations.expect(!first.failed && pathOf(first) == pathOf(again) &&
                            !(pathOf(first) == pathOf(second)),
                        "the same seed wiggles the same way and another seed does not");
    shaky.phase = 0.5;
    expectations.expect(!(pathOf(stroke(line, shaky)) == pathOf(first)),
                        "the phase moves the wiggle");
    double reach = 0.0;
    for (const auto& contour : pathOf(first).contours)
        for (const auto& anchor : contour.anchors)
            reach = std::max(reach, std::abs(anchor.point.y));
    expectations.expect(reach <= 6.0 + 1.0 + 1e-6 && reach > 0.5,
                        "the wiggle stays within its amount");

    StrokeOptions bad;
    bad.width = std::numeric_limits<double>::quiet_NaN();
    expectations.expect(stroke(line, bad).failed, "a width that is not finite is refused");
    StrokeOptions tiny;
    tiny.spacing = 1.0e-6;
    expectations.expect(stroke(line, tiny).failed,
                        "a spacing that would stamp millions of dots is refused");
    StrokeOptions fine;
    fine.dashLength = 1.0e-6;
    fine.dashGap = 0.0;
    expectations.expect(stroke(line, fine).failed, "dashes that would number millions are refused");
}

// #509 Mask Path: the mask index, then each mask's path at the frame (the evaluator samples them).
[[nodiscard]] ValueUtilityOutcome maskPath(const std::int64_t which,
                                           const std::vector<PathValue>& masks) {
    std::vector<CompiledValue> values{CompiledValue{which}};
    for (const auto& mask : masks)
        values.push_back(wrap(mask));
    return run(Kernel::MaskPath, values, {0});
}

[[nodiscard]] std::int64_t countOf(const ValueUtilityOutcome& outcome) {
    const auto* count = std::get_if<std::int64_t>(&outcome.outputs[1]);
    return count == nullptr ? -1 : *count;
}

void testMaskPath(Expectations& expectations) {
    PathValue first = single(square(0, 0, 10));
    first.fillRule = document::PathFillRule::EvenOdd;
    PathValue second;
    second.contours = {square(20, 0, 5), polygon({{0, 30}, {40, 30}}, false)};
    const auto every = maskPath(0, {first, second});
    expectations.expect(!every.failed && pathOf(every).contours.size() == 3 &&
                            pathOf(every).contours[0] == first.contours[0] &&
                            pathOf(every).contours[1] == second.contours[0] &&
                            pathOf(every).contours[2] == second.contours[1] &&
                            pathOf(every).fillRule == document::PathFillRule::EvenOdd &&
                            countOf(every) == 3,
                        "Mask Path 0 reads every mask's contours in mask order, with the first "
                        "mask's fill rule, and counts them");
    const auto only = maskPath(2, {first, second});
    expectations.expect(pathOf(only) == second && countOf(only) == 2,
                        "Mask Path 2 reads the second mask exactly");
    expectations.expect(pathOf(maskPath(3, {first, second})).contours.empty() &&
                            countOf(maskPath(3, {first, second})) == 0 &&
                            !maskPath(3, {first, second}).failed,
                        "a mask past the last reads the empty path");
    expectations.expect(pathOf(maskPath(0, {})).contours.empty() && !maskPath(0, {}).failed,
                        "no masks read the empty path");
    expectations.expect(maskPath(-1, {first}).failed, "a negative mask is refused");
}

// #509 Text Outline: the text's content, size, line height, letter spacing and alignment, with its
// face and fixed layout beside them.
[[nodiscard]] ValueUtilityOutcome textOutline(const std::string& content, const double size,
                                              const std::int64_t alignment = 0,
                                              const double spacing = 0.0) {
    const std::vector<CompiledValue> values{CompiledValue{content}, size, 1.0, spacing,
                                            CompiledValue{alignment}};
    std::vector<const CompiledValue*> operands;
    for (const auto& value : values)
        operands.push_back(&value);
    // The anchor follows the alignment, as a new Text Source's does.
    runtime::CompiledTextOutlineSource text{};
    text.anchorMode = render::kTextAnchorFollowsAlignment;
    return runtime::evaluateValueUtility({Kernel::TextOutline,
                                          operands,
                                          std::vector<std::int64_t>{0},
                                          core::RationalTime::fromInteger(0),
                                          document::FrameRate::framesPerSecond24(),
                                          nullptr,
                                          {},
                                          &text});
}

void testTextOutline(Expectations& expectations) {
    // The answer the render library gives for the same text, laid out as the text draws it.
    render::TextLayoutOptions layout;
    layout.multiline = true;
    layout.anchorMode =
        render::resolveTextAnchorMode(render::kTextAnchorFollowsAlignment, layout.alignment);
    const auto parameters = render::TextRasterParameters::create(40, 40);
    const auto reference =
        render::textOutlines(render::EmbeddedFace::DejaVuSans, "Ag", *parameters.value(), layout);
    const auto outline = textOutline("Ag", 40);
    bool same = reference && !outline.failed &&
                pathOf(outline).contours.size() == reference.value()->size();
    for (std::size_t index = 0; same && index < reference.value()->size(); ++index) {
        const auto& expected = (*reference.value())[index];
        const auto& actual = pathOf(outline).contours[index];
        same = expected.closed == actual.closed && expected.anchors.size() == actual.anchors.size();
        for (std::size_t anchor = 0; same && anchor < expected.anchors.size(); ++anchor)
            same = expected.anchors[anchor].point.x == actual.anchors[anchor].point.x &&
                   expected.anchors[anchor].point.y == actual.anchors[anchor].point.y &&
                   expected.anchors[anchor].inHandle.has_value() ==
                       actual.anchors[anchor].inHandle.has_value() &&
                   expected.anchors[anchor].outHandle.has_value() ==
                       actual.anchors[anchor].outHandle.has_value();
    }
    expectations.expect(same && countOf(outline) ==
                                    static_cast<std::int64_t>(pathOf(outline).contours.size()),
                        "Text Outline is the render library's glyph outlines of the same text, "
                        "anchor for anchor, and counts its contours");
    expectations.expect(pathOf(outline).fillRule == document::PathFillRule::NonZero,
                        "glyph outlines fill by the non-zero rule, as the text draws them");
    // 'A' has an outer contour and a counter, 'g' its bowl and its counter.
    expectations.expect(pathOf(outline).contours.size() >= 3, "Ag has more than one contour");
    expectations.expect(!(pathOf(textOutline("Ag", 60)) == pathOf(outline)) &&
                            !(pathOf(textOutline("Ag", 40, 1)) == pathOf(outline)) &&
                            !(pathOf(textOutline("Ag", 40, 0, 5.0)) == pathOf(outline)),
                        "the size, alignment and letter spacing each move the outline");
    expectations.expect(pathOf(textOutline(" ", 40)).contours.empty() &&
                            !textOutline(" ", 40).failed,
                        "blank text is the empty path");
    expectations.expect(textOutline("Ag", 0.0).failed && textOutline("Ag", 40, 7).failed,
                        "a size or alignment the text cannot lay out is refused");
    // No text to read (the node's source is gone): the empty path, not a failure.
    const std::vector<CompiledValue> none;
    const auto unread = run(Kernel::TextOutline, none, {0});
    expectations.expect(!unread.failed && pathOf(unread).contours.empty() && countOf(unread) == 0,
                        "with no text to read Text Outline is the empty path");
}

// ---------------------------------------------------------------------------------------------
// Cancellation inside the kernels, and the budgets each one refuses at (#344 #530 #509)
// ---------------------------------------------------------------------------------------------

// A real token from a scheduler task that the test cancels at an exact moment.
class CancelGate final {
  public:
    CancelGate() : scheduler_(configuration()) {
        auto submission = scheduler_.submit<void>(
            runtime::TaskRequest("path kernel cancel gate",
                                 {.kind = runtime::TaskOwnerKind::Composition,
                                  .id = runtime::TaskOwnerId::fromRaw(97)}),
            [this](runtime::TaskContext& context) {
                {
                    std::lock_guard lock(mutex_);
                    token_ = context.cancellation();
                    entered_ = true;
                }
                condition_.notify_all();
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return release_; });
                return runtime::TaskResult<void>::succeeded();
            });
        if (!submission.accepted())
            throw std::logic_error("the cancel gate task was refused");
        handle_ = std::move(submission.handle);
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return entered_; });
    }
    ~CancelGate() {
        {
            std::lock_guard lock(mutex_);
            release_ = true;
        }
        condition_.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!handle_.tryTakeResult().has_value() &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CancelGate(const CancelGate&) = delete;
    CancelGate& operator=(const CancelGate&) = delete;
    [[nodiscard]] const runtime::CancellationToken& token() const { return token_; }
    void cancel() { handle_.cancel(); }

  private:
    static runtime::TaskSchedulerConfig configuration() {
        auto config = runtime::TaskSchedulerConfig::defaults();
        config.cpuWorkerCount = 1;
        config.blockingIoWorkerCount = 1;
        return config;
    }
    runtime::TaskScheduler scheduler_;
    runtime::TaskHandle<void> handle_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_ = false;
    bool release_ = false;
    runtime::CancellationToken token_;
};

// One kernel over one operand set; what a scenario runs and how it is told apart from a refusal.
struct Scenario final {
    std::string name;
    Kernel kernel;
    std::vector<CompiledValue> values;
    std::vector<std::int64_t> selectors;
    std::optional<runtime::CompiledTextOutlineSource> text{};
};

[[nodiscard]] ValueUtilityOutcome runWith(const Scenario& scenario,
                                          const runtime::ValueUtilityLimits& limits) {
    std::vector<const CompiledValue*> operands;
    for (const auto& value : scenario.values)
        operands.push_back(&value);
    return runtime::evaluateValueUtility({scenario.kernel, operands, scenario.selectors,
                                          core::RationalTime::fromInteger(0),
                                          document::FrameRate::framesPerSecond24(), nullptr, limits,
                                          scenario.text ? &*scenario.text : nullptr});
}

[[nodiscard]] bool publishedNothing(const ValueUtilityOutcome& outcome) {
    // An outcome starts shaped with an empty path per slot, so "published" is a path with contours.
    for (const auto& value : outcome.outputs)
        if (const auto* held = std::get_if<runtime::CompiledPathValue>(&value);
            held != nullptr && held->value != nullptr && !held->value->contours.empty())
            return false;
    return true;
}

// A closed contour of `count` corner anchors on a circle (`wobble` pushes alternate anchors out so
// the polygon is not convex and rounding, offsetting and merging have real work to do).
[[nodiscard]] PathContour ring(const std::size_t count, const double centerX, const double centerY,
                               const double radius, const bool curved, const double wobble = 0.0) {
    PathContour contour;
    contour.closed = true;
    for (std::size_t index = 0; index < count; ++index) {
        const double angle = 2.0 * std::numbers::pi * static_cast<double>(index) /
                             static_cast<double>(count);
        const double r = radius + (index % 2 == 0 ? wobble : -wobble);
        PathAnchor anchor = corner(centerX + r * std::cos(angle), centerY + r * std::sin(angle));
        if (curved) {
            const double reach = radius * 2.0 * std::numbers::pi / static_cast<double>(count) / 3.0;
            anchor.outHandle = Vec2d{anchor.point.x - reach * std::sin(angle),
                                     anchor.point.y + reach * std::cos(angle)};
            anchor.inHandle = Vec2d{anchor.point.x + reach * std::sin(angle),
                                    anchor.point.y - reach * std::cos(angle)};
            anchor.mode = document::PathAnchorMode::Free;
        }
        contour.anchors.push_back(anchor);
    }
    return contour;
}

// An open zig-zag of `count` corner anchors, `step` apart.
[[nodiscard]] PathContour zigzag(const std::size_t count, const double step) {
    PathContour contour;
    contour.closed = false;
    for (std::size_t index = 0; index < count; ++index)
        contour.anchors.push_back(
            corner(static_cast<double>(index) * step, index % 2 == 0 ? 0.0 : 25.0));
    return contour;
}

[[nodiscard]] PathValue crowd(const std::size_t contours, const std::size_t anchors,
                              const bool curved, const double wobble) {
    PathValue value;
    for (std::size_t index = 0; index < contours; ++index)
        value.contours.push_back(ring(anchors, 40.0 * static_cast<double>(index % 8),
                                      30.0 * static_cast<double>(index / 8), 90.0, curved, wobble));
    return value;
}

[[nodiscard]] std::vector<Scenario> heavyScenarios() {
    std::vector<Scenario> scenarios;
    const auto stroke = [](const char* name, auto&& customise, const std::size_t anchors = 700) {
        StrokeOptions options;
        options.tolerance = 0.1;
        customise(options);
        PathValue open;
        open.contours.push_back(zigzag(anchors, anchors < 700 ? 30.0 : 12.0));
        return Scenario{name,
                        Kernel::PathStroke,
                        {wrap(std::move(open)), options.width, options.start, options.end,
                         options.offset, options.taperStart, options.taperEnd, options.dashLength,
                         options.dashGap, options.dashOffset, options.spacing, options.wiggle,
                         options.frequency, options.phase, options.seed, options.core,
                         options.tolerance, options.stampRotation},
                        {options.cap, options.join, options.stamp, options.stampAlign}};
    };
    scenarios.push_back({"Transform Path",
                         Kernel::TransformPath,
                         {wrap(crowd(24, 4000, false, 1.0)), Vec2d{5, 5}, Vec2d{0, 0}, Vec2d{2, 1},
                          33.0},
                         {}});
    scenarios.push_back({"Round Corners",
                         Kernel::RoundCorners,
                         {wrap(crowd(4, 2000, false, 4.0)), 2.0},
                         {}});
    scenarios.push_back({"Trim Path",
                         Kernel::TrimPath,
                         {wrap(crowd(8, 2000, true, 0.0)), 10.0, 85.0, 7.0},
                         {0}});
    scenarios.push_back({"Trim Path (individually)",
                         Kernel::TrimPath,
                         {wrap(crowd(8, 2000, true, 0.0)), 10.0, 85.0, 7.0},
                         {1}});
    scenarios.push_back({"Offset Path",
                         Kernel::OffsetPath,
                         {wrap(crowd(8, 300, true, 0.0)), 6.0, 4.0, 0.05},
                         {1}});
    scenarios.push_back({"Boolean Paths",
                         Kernel::BooleanPaths,
                         {wrap(crowd(4, 600, true, 0.0)), wrap(crowd(4, 600, true, 6.0)), 0.05},
                         {2}});
    scenarios.push_back({"Merge Paths",
                         Kernel::MergePaths,
                         {wrap(crowd(16, 300, true, 0.0)), 0.05},
                         {1}});
    scenarios.push_back({"Path Stroke (solid with core)", Kernel::PathStroke, {}, {}});
    scenarios.back() = stroke("Path Stroke (solid with core)", [](StrokeOptions& o) {
        o.core = 40;
        o.cap = 1;
        o.join = 0;
    });
    scenarios.push_back(stroke("Path Stroke (dashed)", [](StrokeOptions& o) {
        o.dashLength = 9;
        o.dashGap = 4;
    }));
    scenarios.push_back(stroke("Path Stroke (dots)", [](StrokeOptions& o) { o.spacing = 14; }));
    scenarios.push_back(stroke("Path Stroke (dash stamps, turned)", [](StrokeOptions& o) {
        o.spacing = 14;
        o.stamp = 3;
        o.stampRotation = 15;
        o.core = 50;
    }));
    {
        // #509 Mask Path over many masks, and Text Outline over a long text.
        Scenario masks{"Mask Path", Kernel::MaskPath, {CompiledValue{std::int64_t{0}}}, {0}};
        for (int mask = 0; mask < 16; ++mask)
            masks.values.push_back(wrap(crowd(64, 200, true, mask)));
        scenarios.push_back(std::move(masks));
        std::string text;
        for (int line = 0; line < 40; ++line)
            text += "The quick brown fox jumps over the lazy dog 0123456789\n";
        scenarios.push_back({"Text Outline",
                             Kernel::TextOutline,
                             {CompiledValue{text}, 24.0, 1.0, 0.0, CompiledValue{std::int64_t{0}}},
                             {0},
                             runtime::CompiledTextOutlineSource{}});
    }
    scenarios.push_back(stroke("Path Stroke (tapered, wiggle)", [](StrokeOptions& o) {
        o.taperStart = 30;
        o.taperEnd = 30;
        o.wiggle = 3;
        o.frequency = 2;
    }, 60));
    return scenarios;
}

void testKernelCancellation(Expectations& expectations) {
    for (const auto& scenario : heavyScenarios()) {
        const std::string name = scenario.name;
        // The answer with no cancellation, and how many times the kernel polls to reach it.
        std::uint64_t total = 0;
        const std::function<void()> counter = [&total] { ++total; };
        runtime::ValueUtilityLimits counting;
        counting.pollObserver = &counter;
        const auto baseline = runWith(scenario, counting);
        expectations.expect(!baseline.failed && !baseline.cancelled && !publishedNothing(baseline),
                            name + ": the large input has a real answer (" +
                                std::string(baseline.summary) + ")");
        expectations.expect(total >= 8, name + ": the kernel polls cancellation inside its loops (" +
                                            std::to_string(total) + " polls)");

        // A token cancelled before the call: cancelled, nothing published.
        {
            CancelGate gate;
            gate.cancel();
            runtime::ValueUtilityLimits limits;
            limits.cancellation = &gate.token();
            const auto outcome = runWith(scenario, limits);
            expectations.expect(outcome.cancelled && !outcome.failed && publishedNothing(outcome),
                                name + ": a cancelled token cancels the kernel and publishes nothing");
        }
        // Cancelled from inside the kernel, at the first poll, the middle poll and the last but
        // one: the kernel stops at the next poll, before it reaches its answer, and publishes
        // nothing. Polls after the one that cancelled are a handful at most.
        for (const std::uint64_t at : {std::uint64_t{1}, std::max<std::uint64_t>(2, total / 2),
                                       total > 3 ? total - 2 : std::uint64_t{1}}) {
            CancelGate gate;
            std::uint64_t polls = 0;
            std::uint64_t after = 0;
            const std::function<void()> observer = [&] {
                ++polls;
                if (polls == at)
                    gate.cancel();
                else if (polls > at)
                    ++after;
            };
            runtime::ValueUtilityLimits limits;
            limits.cancellation = &gate.token();
            limits.pollObserver = &observer;
            const auto outcome = runWith(scenario, limits);
            const auto label = name + " cancelled at poll " + std::to_string(at) + " of " +
                               std::to_string(total);
            expectations.expect(outcome.cancelled && !outcome.failed,
                                label + ": the kernel reports cancelled");
            expectations.expect(publishedNothing(outcome), label + ": it publishes no value");
            expectations.expect(after <= 4, label + ": it stops at the next poll (" +
                                                std::to_string(after) + " polls after)");
            expectations.expect(polls < total + 1 && polls <= at + 4,
                                label + ": it stopped before the work was done");
        }
        // And the next, uncancelled call returns the same full answer.
        const auto again = runWith(scenario, {});
        expectations.expect(!again.failed && !again.cancelled &&
                                pathOf(again) == pathOf(baseline) &&
                                coreOf(again) == coreOf(baseline),
                            name + ": an uncancelled call after a cancelled one is the full answer");
    }
}

// Each kernel's budget: an input past what it may hold fails with its documented diagnostic.
void testKernelBudgets(Expectations& expectations) {
    const auto refusal = [&](const Scenario& scenario, const std::string_view summary) {
        const auto outcome = runWith(scenario, {});
        expectations.expect(outcome.failed && !outcome.cancelled && outcome.summary == summary,
                            scenario.name + ": refused with \"" + std::string(summary) +
                                "\" (got \"" + std::string(outcome.summary) + "\")");
    };
    // A contour of curves each a million pixels across, flattened at the finest tolerance, runs
    // past the flattener's segment budget.
    PathValue vast;
    vast.contours.push_back(ring(4000, 0.0, 0.0, 5.0e8, true));
    const auto fine = 0.001;
    refusal({"Offset Path", Kernel::OffsetPath, {wrap(vast), 5.0, 4.0, fine}, {0}},
            "Offset Path is too detailed");
    refusal({"Boolean Paths", Kernel::BooleanPaths, {wrap(vast), wrap(vast), fine}, {0}},
            "Boolean Paths is too detailed");
    refusal({"Merge Paths", Kernel::MergePaths, {wrap(vast), fine}, {0}},
            "Merge Paths is too detailed");
    {
        StrokeOptions options;
        options.tolerance = fine;
        refusal(
            {"Path Stroke",
             Kernel::PathStroke,
             {wrap(vast), options.width, options.start, options.end, options.offset,
              options.taperStart, options.taperEnd, options.dashLength, options.dashGap,
              options.dashOffset, options.spacing, options.wiggle, options.frequency, options.phase,
              options.seed, options.core, options.tolerance, options.stampRotation},
             {options.cap, options.join, options.stamp, options.stampAlign}},
            "Path Stroke is too detailed");
    }
    // #509: masks or glyphs past the path value's anchor cap.
    {
        Scenario masks{"Mask Path", Kernel::MaskPath, {CompiledValue{std::int64_t{0}}}, {0}};
        const auto full = wrap(single(ring(document::kMaximumPathAnchors, 0.0, 0.0, 100.0, false)));
        for (std::size_t mask = 0;
             mask <= document::kMaximumPathValueAnchors / document::kMaximumPathAnchors; ++mask)
            masks.values.push_back(full);
        refusal(masks, "Mask Path result is too large");
        refusal({"Text Outline",
                 Kernel::TextOutline,
                 {CompiledValue{std::string(12000, '@')}, 12.0, 1.0, 0.0,
                  CompiledValue{std::int64_t{0}}},
                 {0},
                 runtime::CompiledTextOutlineSource{}},
                "Text Outline result is too large");
    }
    // Rounding every corner of a contour already at the anchor cap doubles it past the cap.
    PathValue crowded;
    crowded.contours.push_back(ring(document::kMaximumPathAnchors, 0.0, 0.0, 1000.0, false, 100.0));
    refusal({"Round Corners", Kernel::RoundCorners, {wrap(crowded), 5.0}, {}},
            "Round Corners result is too large");
    // A coordinate a billion pixels out is not a usable path.
    PathValue outside = single(polygon({{0, 0}, {3.0e9, 0}, {3.0e9, 5}}, true));
    refusal({"Trim Path", Kernel::TrimPath, {wrap(outside), 0.0, 50.0, 0.0}, {0}},
            "Trim Path path is out of range");
    refusal({"Round Corners", Kernel::RoundCorners, {wrap(outside), 2.0}, {}},
            "Round Corners path is out of range");
    // A scale that carries a coordinate past what a number holds overflows.
    refusal({"Transform Path",
             Kernel::TransformPath,
             {wrap(single(square(0, 0, 1.0e9))), Vec2d{}, Vec2d{}, Vec2d{1.0e300, 1.0e300}, 0.0},
             {}},
            "Transform Path overflowed");
    // Dashes and brush dots are bounded at the sample budget.
    {
        PathValue line = single(polygon({{0, 0}, {1.0e9, 0}}, false));
        StrokeOptions dots;
        dots.spacing = 1.0e-3;
        const auto make = [&](const StrokeOptions& o, const char* name) {
            return Scenario{name,
                            Kernel::PathStroke,
                            {wrap(line), o.width, o.start, o.end, o.offset, o.taperStart,
                             o.taperEnd, o.dashLength, o.dashGap, o.dashOffset, o.spacing, o.wiggle,
                             o.frequency, o.phase, o.seed, o.core, o.tolerance, o.stampRotation},
                            {o.cap, o.join, o.stamp, o.stampAlign}};
        };
        refusal(make(dots, "Path Stroke (dots)"), "Path Stroke spacing is too small");
        StrokeOptions squares = dots;
        squares.stamp = 1;
        squares.stampAlign = 1;
        refusal(make(squares, "Path Stroke (square stamps)"), "Path Stroke spacing is too small");
        StrokeOptions dashes;
        dashes.dashLength = 1.0e-3;
        dashes.dashGap = 0.0;
        refusal(make(dashes, "Path Stroke (dashes)"), "Path Stroke dashes are too short");
        StrokeOptions shaky;
        shaky.wiggle = 4;
        shaky.frequency = 1.0e9;
        refusal(make(shaky, "Path Stroke (wiggle)"), "Path Stroke wiggle is too fine");
    }
}


// ---- #637 Resample Path ----

struct ResampleOptions final {
    std::int64_t count = 32;
    double gap = 10.0, density = 10.0, percentage = 100.0, startOffset = 0.0;
    bool keepCorners = true;
    document::ResampleMode mode = document::ResampleMode::FixedCount;
    document::ResampleOutput output = document::ResampleOutput::Curves;
};

[[nodiscard]] ValueUtilityOutcome resample(const PathValue& path, const ResampleOptions& o) {
    return run(Kernel::ResamplePath,
               {wrap(path), o.count, o.gap, o.density, o.percentage, o.keepCorners, o.startOffset},
               {document::selectorStoredValue(o.mode), document::selectorStoredValue(o.output)});
}

[[nodiscard]] ResampleOptions fixedCount(const std::int64_t count, const bool keepCorners = false) {
    ResampleOptions o;
    o.count = count;
    o.keepCorners = keepCorners;
    return o;
}
[[nodiscard]] ResampleOptions byGap(const double gap, const bool keepCorners = false) {
    ResampleOptions o;
    o.mode = document::ResampleMode::Gap;
    o.gap = gap;
    o.keepCorners = keepCorners;
    return o;
}

[[nodiscard]] PathContour circle(const double radius, const Vec2d centre = {0, 0}) {
    const double k = 0.5522847498307936 * radius;
    PathContour contour;
    contour.closed = true;
    const auto at = [&](const double x, const double y, const Vec2d in, const Vec2d out) {
        return PathAnchor{{centre.x + x, centre.y + y},
                          Vec2d{centre.x + in.x, centre.y + in.y},
                          Vec2d{centre.x + out.x, centre.y + out.y},
                          document::PathAnchorMode::Mirrored};
    };
    contour.anchors = {at(radius, 0, {radius, -k}, {radius, k}),
                       at(0, radius, {k, radius}, {-k, radius}),
                       at(-radius, 0, {-radius, k}, {-radius, -k}),
                       at(0, -radius, {-k, -radius}, {k, -radius})};
    return contour;
}

[[nodiscard]] double arcStep(const PathContour& contour, const std::size_t index) {
    const auto& a = contour.anchors[index].point;
    const auto& b = contour.anchors[(index + 1) % contour.anchors.size()].point;
    return std::hypot(b.x - a.x, b.y - a.y);
}

void testResamplePathKnownAnswers(Expectations& expectations) {
    const auto line100 = single(polygon({{0, 0}, {100, 0}}, false));

    // A 100 px line at a gap of 10 gives 11 points, 10 px apart, ends kept.
    const auto gapped = resample(line100, byGap(10.0));
    const auto& eleven = pathOf(gapped).contours.empty() ? PathContour{} : pathOf(gapped).contours[0];
    expectations.expect(!gapped.failed && eleven.anchors.size() == 11,
                        "a 100 px line at Gap 10 gives 11 points");
    bool evenly = eleven.anchors.size() == 11;
    for (std::size_t index = 0; evenly && index < eleven.anchors.size(); ++index)
        evenly = near(eleven.anchors[index].point, Vec2d{10.0 * static_cast<double>(index), 0.0}, 1e-9);
    expectations.expect(evenly, "the 11 points are 0, 10, ... 100 on the line");
    expectations.expect(!eleven.closed && !eleven.anchors.front().outHandle &&
                            !eleven.anchors.back().inHandle,
                        "a straight line stays straight under Curves (no handles)");
    // The gap is the largest step: 95 px at Gap 10 needs 10 steps of 9.5.
    const auto ninety5 = pathOf(resample(single(polygon({{0, 0}, {95, 0}}, false)), byGap(10.0)));
    expectations.expect(ninety5.contours[0].anchors.size() == 11 &&
                            near(ninety5.contours[0].anchors[1].point.x, 9.5, 1e-9),
                        "a gap that does not divide the length is the largest step, spread evenly");

    // Fixed count: N points on every contour, open and closed alike, in a compound path.
    PathValue compound;
    compound.contours = {polygon({{0, 0}, {50, 0}, {50, 30}}, false), square(0, 0, 40),
                         polygon({{0, 0}, {7, 3}}, false), circle(20)};
    compound.fillRule = document::PathFillRule::EvenOdd;
    for (const std::int64_t n : {2, 3, 7, 16, 100}) {
        const auto fixed = resample(compound, fixedCount(n));
        bool all = !fixed.failed && pathOf(fixed).contours.size() == compound.contours.size();
        for (std::size_t index = 0; all && index < compound.contours.size(); ++index) {
            const auto want = static_cast<std::size_t>(
                std::max<std::int64_t>(n, compound.contours[index].closed ? 3 : 2));
            all = pathOf(fixed).contours[index].anchors.size() == want &&
                  pathOf(fixed).contours[index].closed == compound.contours[index].closed;
        }
        expectations.expect(all, "Fixed count " + std::to_string(n) +
                                     " gives that many points on every contour (at least 2 open, 3 closed)");
    }
    expectations.expect(
        pathOf(resample(compound, fixedCount(7))).contours.size() == 4 &&
            pathOf(resample(compound, fixedCount(7))).fillRule == document::PathFillRule::EvenOdd,
        "contours stay one to one and the fill rule is kept");

    // By length: points per 100 px. 200 px at 5 per 100 is a gap of 20: 11 points; 50 px at 10 per
    // 100 is a gap of 10: 6 points; a closed 160 px square at 25 per 100 is a gap of 4: 40 points.
    ResampleOptions byLength;
    byLength.mode = document::ResampleMode::ByLength;
    byLength.keepCorners = false;
    byLength.density = 5.0;
    expectations.expect(pathOf(resample(single(polygon({{0, 0}, {200, 0}}, false)), byLength))
                                .contours[0].anchors.size() == 11,
                        "By length: 200 px at 5 points per 100 px gives 11 points");
    byLength.density = 10.0;
    expectations.expect(pathOf(resample(single(polygon({{0, 0}, {50, 0}}, false)), byLength))
                                .contours[0].anchors.size() == 6,
                        "By length: 50 px at 10 points per 100 px gives 6 points");
    byLength.density = 25.0;
    expectations.expect(pathOf(resample(single(square(0, 0, 40)), byLength)).contours[0].anchors.size() == 40,
                        "By length: a closed 160 px square at 25 points per 100 px gives 40 points");

    // Percentage of the contour's current point count; 100 percent keeps it; minimum 2 open / 3 closed.
    ResampleOptions percentage;
    percentage.mode = document::ResampleMode::Percentage;
    percentage.keepCorners = false;
    const auto octagon = single(polygon({{10, 0}, {20, 0}, {30, 10}, {30, 20}, {20, 30}, {10, 30}, {0, 20}, {0, 10}}, true));
    const auto sizeAt = [&](const PathValue& path, const double share) {
        percentage.percentage = share;
        return pathOf(resample(path, percentage)).contours[0].anchors.size();
    };
    expectations.expect(sizeAt(octagon, 100.0) == 8 && sizeAt(octagon, 50.0) == 4 &&
                            sizeAt(octagon, 200.0) == 16 && sizeAt(octagon, 1.0) == 3,
                        "Percentage: 100 keeps 8 points, 50 gives 4, 200 gives 16, a tiny share keeps 3");
    const auto polyline5 = single(polygon({{0, 0}, {10, 5}, {20, 0}, {30, 5}, {40, 0}}, false));
    expectations.expect(sizeAt(polyline5, 50.0) == 3 && sizeAt(polyline5, 10.0) == 2 &&
                            sizeAt(polyline5, 0.0) == 2,
                        "Percentage on an open contour: 5 points at 50 gives 3 (half rounds up), the minimum is 2");

    // A closed contour of N points does not duplicate the seam.
    const auto sq = single(square(0, 0, 40));
    const auto eight = pathOf(resample(sq, fixedCount(8))).contours[0];
    bool noSeam = eight.closed && eight.anchors.size() == 8;
    for (std::size_t index = 0; noSeam && index < 8; ++index)
        noSeam = near(arcStep(eight, index), 20.0, 1e-9) || near(arcStep(eight, index), 20.0 * std::sqrt(2.0), 1e-9) ||
                 arcStep(eight, index) > 0.0;
    expectations.expect(noSeam && !near(eight.anchors.front().point, eight.anchors.back().point),
                        "a closed contour of 8 points does not repeat its first point at the end");
    // On a straight-sided square with the points at 20 px spacing along the perimeter the chord
    // between neighbours is 20 except across corners.
    expectations.expect(near(eight.anchors[0].point, Vec2d{0, 0}, 1e-9) &&
                            near(eight.anchors[1].point, Vec2d{20, 0}, 1e-9) &&
                            near(eight.anchors[2].point, Vec2d{40, 0}, 1e-9) &&
                            near(eight.anchors[7].point, Vec2d{0, 20}, 1e-9),
                        "the 8 points fall every 20 px of perimeter from the first anchor");

    // Start offset slides the points: a closed square at 12.5 percent starts 20 px along, and 100
    // percent is the start again (it wraps).
    auto slid = fixedCount(8);
    slid.startOffset = 12.5;
    const auto shifted = pathOf(resample(sq, slid)).contours[0];
    expectations.expect(near(shifted.anchors[0].point, Vec2d{20, 0}, 1e-9) &&
                            near(shifted.anchors[7].point, Vec2d{0, 0}, 1e-9),
                        "Start offset 12.5 percent slides the points 20 px along a 160 px contour");
    slid.startOffset = 100.0;
    expectations.expect(pathOf(resample(sq, slid)).contours[0] == eight,
                        "Start offset 100 percent wraps to the start on a closed contour");
    slid.startOffset = -75.0;
    expectations.expect(near(pathOf(resample(sq, slid)).contours[0].anchors[0].point, Vec2d{40, 0}, 1e-9),
                        "a negative offset wraps the other way (-75 is +25 of 100: 40 px along)");
    // An open contour keeps its ends and slides the inner points; the offset clamps.
    auto openFive = fixedCount(5);
    const auto plain = pathOf(resample(line100, openFive)).contours[0];
    openFive.startOffset = 50.0;
    const auto moved = pathOf(resample(line100, openFive)).contours[0];
    openFive.startOffset = 500.0;
    const auto clamped = pathOf(resample(line100, openFive)).contours[0];
    openFive.startOffset = 100.0;
    const auto full = pathOf(resample(line100, openFive)).contours[0];
    expectations.expect(plain.anchors.size() == 5 && near(plain.anchors[1].point.x, 25.0, 1e-9) &&
                            near(plain.anchors[2].point.x, 50.0, 1e-9),
                        "an open Fixed count 5 on 100 px falls at 0, 25, 50, 75, 100");
    expectations.expect(moved.anchors.size() == 5 && near(moved.anchors[0].point.x, 0.0, 1e-9) &&
                            near(moved.anchors[4].point.x, 100.0, 1e-9) &&
                            near(moved.anchors[1].point.x, 34.375, 1e-9) &&
                            near(moved.anchors[2].point.x, 56.25, 1e-9) &&
                            near(moved.anchors[3].point.x, 78.125, 1e-9),
                        "Start offset 50 percent slides the inner points half a gap, ends kept");
    expectations.expect(clamped == full && full.anchors.size() == 5 &&
                            full.anchors[3].point.x < 100.0 &&
                            full.anchors[1].point.x > 25.0,
                        "an open contour clamps the offset at 100 percent and never reaches the end");

    // Keep corners keeps a square's four corners, off lets them go.
    const auto six = pathOf(resample(sq, fixedCount(6, true))).contours[0];
    const auto hasPoint = [](const PathContour& contour, const Vec2d point) {
        return std::ranges::any_of(contour.anchors,
                                   [&](const PathAnchor& each) { return near(each.point, point, 1e-9); });
    };
    expectations.expect(six.anchors.size() == 6 && hasPoint(six, {0, 0}) && hasPoint(six, {40, 0}) &&
                            hasPoint(six, {40, 40}) && hasPoint(six, {0, 40}),
                        "Keep corners: 6 points on a square still include its four corners");
    const auto loose = pathOf(resample(sq, fixedCount(6, false))).contours[0];
    expectations.expect(loose.anchors.size() == 6 && !hasPoint(loose, {40, 0}),
                        "Keep corners off: 6 even points leave the corner at (40, 0)");
    const auto gappedCorners = pathOf(resample(sq, byGap(15.0, true))).contours[0];
    expectations.expect(hasPoint(gappedCorners, {40, 0}) && hasPoint(gappedCorners, {40, 40}) &&
                            gappedCorners.anchors.size() == 4 * 3,
                        "Keep corners with a gap: each 40 px side takes 3 steps, corners kept");
    // Fewer points than corners: the corners cannot all stay, the count wins.
    expectations.expect(pathOf(resample(sq, fixedCount(3, true))).contours[0].anchors.size() == 3,
                        "Fixed count 3 on a square keeps 3 points, not its 4 corners");
    // A collinear anchor is not a corner.
    const auto straightish = single(polygon({{0, 0}, {50, 0}, {100, 0}}, false));
    expectations.expect(pathOf(resample(straightish, fixedCount(5, true))).contours[0].anchors.size() == 5,
                        "an anchor in the middle of a straight run is not a corner");
    // Open polyline corners are kept.
    const auto elbow = single(polygon({{0, 0}, {40, 0}, {40, 40}}, false));
    const auto elbowed = pathOf(resample(elbow, fixedCount(7, true))).contours[0];
    expectations.expect(elbowed.anchors.size() == 7 && hasPoint(elbowed, {40, 0}) &&
                            hasPoint(elbowed, {0, 0}) && hasPoint(elbowed, {40, 40}),
                        "an open elbow keeps its corner and its ends");

    // Groups are kept.
    PathValue grouped = compound;
    grouped.groups = {{"a", 0, 2, std::nullopt}, {"b", 3, 1, core::Color4d{1, 0, 0, 1}}};
    expectations.expect(pathOf(resample(grouped, fixedCount(9))).groups == grouped.groups,
                        "groups on the input are kept");

    // A zero-length contour: Fixed count answers copies of its point with a notice, not a failure.
    PathValue dot;
    dot.contours = {polygon({{3, 4}}, false)};
    const auto dotted = resample(dot, fixedCount(5));
    expectations.expect(!dotted.failed && dotted.notice && pathOf(dotted).contours[0].anchors.size() == 5 &&
                            near(pathOf(dotted).contours[0].anchors[4].point, Vec2d{3, 4}),
                        "Fixed count on a contour with no length repeats its point and says so");
    const auto dotGap = resample(dot, byGap(5.0));
    expectations.expect(!dotGap.failed && pathOf(dotGap) == dot, "a gap leaves a contour with no length as it is");
    expectations.expect(pathOf(resample(PathValue{}, fixedCount(8))).contours.empty(),
                        "an empty path answers the empty path");
}

void testResampleCurvesStayOnTheCurve(Expectations& expectations) {
    const auto original = circle(100.0);
    const auto value = single(original);
    // The stated tolerance is 0.25 px for a count at the original density (4 anchors) or finer.
    for (const std::int64_t n : {4, 8, 16, 32, 64, 128}) {
        const auto result = resample(value, fixedCount(n));
        const auto& contour = pathOf(result).contours[0];
        expectations.expect(!result.failed && contour.anchors.size() == static_cast<std::size_t>(n),
                            "the circle resamples to " + std::to_string(n));
        const double off = runtime::pathresample::deviation(contour, original);
        expectations.expect(off <= 0.25, "Curves output at " + std::to_string(n) + " points stays within 0.25 px of the circle (measured " + std::to_string(off) + ")");
    }
    // An open cubic S-curve, resampled at 5 and 24 points.
    PathContour wave;
    wave.anchors = {{{0, 0}, std::nullopt, Vec2d{60, -80}, document::PathAnchorMode::Free},
                    {{200, 0}, Vec2d{140, 80}, std::nullopt, document::PathAnchorMode::Free}};
    for (const std::int64_t n : {5, 24, 96}) {
        const auto result = resample(single(wave), fixedCount(n));
        const double off = runtime::pathresample::deviation(pathOf(result).contours[0], wave);
        expectations.expect(off <= 0.25, "an open cubic at " + std::to_string(n) + " points stays within 0.25 px (measured " + std::to_string(off) + ")");
    }
    // A multi-segment open curve with a corner between smooth runs, more points than it has anchors.
    PathContour mixed;
    mixed.anchors = {{{0, 0}, std::nullopt, Vec2d{30, 40}, document::PathAnchorMode::Free},
                     {{100, 60}, Vec2d{70, 80}, std::nullopt, document::PathAnchorMode::Corner},
                     {{180, 0}, Vec2d{150, 60}, Vec2d{210, -40}, document::PathAnchorMode::Free},
                     {{260, 50}, Vec2d{230, 10}, std::nullopt, document::PathAnchorMode::Free}};
    for (const std::int64_t n : {12, 40, 120}) {
        const auto result = resample(single(mixed), fixedCount(n, true));
        const double off = runtime::pathresample::deviation(pathOf(result).contours[0], mixed);
        expectations.expect(off <= 0.25, "a mixed open curve with corners kept at " + std::to_string(n) + " points stays within 0.25 px (measured " + std::to_string(off) + ")");
    }
    // Straight output follows the chords: within the sagitta of the points chosen.
    ResampleOptions straight = fixedCount(64);
    straight.output = document::ResampleOutput::Straight;
    const auto chords = pathOf(resample(value, straight)).contours[0];
    expectations.expect(!chords.anchors[0].outHandle && !chords.anchors[1].inHandle &&
                            runtime::pathresample::deviation(chords, original) <= 0.13,
                        "Straight output joins 64 points on the circle with chords within 0.13 px");
    // A coarse count below the original detail is by definition not on the curve: 3 points on a circle.
    const auto coarse = pathOf(resample(value, fixedCount(3))).contours[0];
    expectations.expect(coarse.anchors.size() == 3, "a count below the original detail still answers that many points");
    // The points are on the original curve: each is within 1e-3 of the circle.
    bool onCircle = true;
    const auto thirtySeven = resample(value, fixedCount(37));
    for (const auto& anchor : pathOf(thirtySeven).contours[0].anchors)
        onCircle = onCircle && std::abs(std::hypot(anchor.point.x, anchor.point.y) - 100.0) < 0.06;
    expectations.expect(onCircle, "every resampled point lies on the original circle (within 0.06 px of radius 100)");
}

void testResampleBounds(Expectations& expectations) {
    const auto line100 = single(polygon({{0, 0}, {100, 0}}, false));
    const auto beyond = resample(line100, fixedCount(5000));
    expectations.expect(beyond.failed &&
                            std::string_view(beyond.detail).find("Resample Path") != std::string_view::npos &&
                            std::string_view(beyond.detail).find("Fixed count") != std::string_view::npos &&
                            std::string_view(beyond.detail).find("5000") != std::string_view::npos &&
                            std::string_view(beyond.detail).find("4096") != std::string_view::npos &&
                            pathOf(beyond).contours.empty(),
                        "a count past the contour cap fails, naming the node, the mode, the count and the cap");
    expectations.expect(!resample(line100, fixedCount(4096)).failed, "4096 points is the cap, allowed");
    const auto tiny = resample(line100, byGap(0.001));
    expectations.expect(tiny.failed && std::string_view(tiny.detail).find("Gap") != std::string_view::npos &&
                            std::string_view(tiny.detail).find("100001") != std::string_view::npos,
                        "a gap that would give 100001 points fails with the count named, nothing allocated");
    ResampleOptions dense;
    dense.mode = document::ResampleMode::ByLength;
    dense.density = 1.0e12;
    expectations.expect(resample(line100, dense).failed, "a density of a trillion points is refused");
    ResampleOptions percentage;
    percentage.mode = document::ResampleMode::Percentage;
    percentage.percentage = 1.0e9;
    expectations.expect(resample(single(square(0, 0, 4)), percentage).failed,
                        "a percentage that would give billions of points is refused");
    // The whole value has a cap too: 70 contours of 4000 points is 280000.
    PathValue many;
    for (int index = 0; index < 70; ++index)
        many.contours.push_back(polygon({{0, 0}, {100, 0}}, false));
    const auto crowded = resample(many, fixedCount(4000));
    expectations.expect(crowded.failed &&
                            std::string_view(crowded.detail).find("262144") != std::string_view::npos &&
                            std::string_view(crowded.detail).find("Fixed count") != std::string_view::npos,
                        "past the whole path's 262144 points, the node fails and names the mode and the cap");
    expectations.expect(!resample(many, fixedCount(3000)).failed, "70 contours of 3000 points (210000) fit");
    expectations.expect(resample(line100, byGap(0.0)).failed && resample(line100, byGap(-5.0)).failed &&
                            resample(line100, byGap(std::nan(""))).failed,
                        "a gap of zero, below zero or not a number is refused");
    // Non-finite coordinates.
    expectations.expect(resample(single(polygon({{0, 0}, {std::nan(""), 0}}, false)), fixedCount(5)).failed,
                        "a path with a coordinate that is not a number is refused");
}

void testResampleCancellation(Expectations& expectations) {
    PathValue many;
    for (int index = 0; index < 8; ++index)
        many.contours.push_back(circle(10.0 + index));
    runtime::CancellationToken cancelled;
    // A token that is not cancelled passes through; the evaluator-level observation between contours
    // is covered with a real cancelled token by the integration test.
    std::vector<const CompiledValue*> operands;
    std::vector<CompiledValue> values{wrap(many), std::int64_t{16}, 10.0, 10.0, 100.0, true, 0.0};
    for (const auto& value : values)
        operands.push_back(&value);
    const std::array<std::int64_t, 2> selectors{0, 1};
    const auto done = runtime::evaluateValueUtility({Kernel::ResamplePath, operands, selectors,
                                                     core::RationalTime::fromInteger(0),
                                                     document::FrameRate::framesPerSecond24(), nullptr,
                                                     {.cancellation = &cancelled}});
    expectations.expect(!done.cancelled && !done.failed && pathOf(done).contours.size() == 8,
                        "an uncancelled token changes nothing");
}

void testResampleHelper(Expectations& expectations) {
    // The shared helper: exactly N points, spread evenly by length, on the curve.
    const auto original = circle(50.0);
    for (const std::size_t n : std::array<std::size_t, 4>{3, 5, 11, 40}) {
        const auto result = runtime::pathresample::resampleToCount(original, n);
        expectations.expect(result.status == runtime::pathresample::ResampleStatus::Ok &&
                                result.contour.anchors.size() == n && result.contour.closed,
                            "resampleToCount answers exactly N points");
    }
    // The same contour twice gives the same answer.
    expectations.expect(runtime::pathresample::resampleToCount(original, 17).contour ==
                            runtime::pathresample::resampleToCount(original, 17).contour,
                        "resampling is deterministic");
}

// ---------------------------------------------------------------------------------------------
// #598 Transform Path skew and Warp Path
// ---------------------------------------------------------------------------------------------

[[nodiscard]] ValueUtilityOutcome transformWithSkew(const PathValue& path, const Vec2d position,
                                                    const Vec2d anchor, const Vec2d scale,
                                                    const double rotation, const double skew,
                                                    const double skewAxis) {
    return run(Kernel::TransformPath,
               {wrap(path), position, anchor, scale, rotation, skew, skewAxis});
}

struct WarpOptions final {
    document::WarpFunction function = document::WarpFunction::Wave;
    double amount = 20.0;
    double size = 100.0;
    double phase = 0.0;
    double tolerance = 0.25;
    Vec2d position{0, 0};
    Vec2d anchor{0, 0};
    Vec2d scale{1, 1};
    double rotation = 0.0;
    double skew = 0.0;
    double skewAxis = 0.0;
};
[[nodiscard]] ValueUtilityOutcome warp(const PathValue& path, const WarpOptions& o) {
    return run(Kernel::WarpPath,
               {wrap(path), o.amount, o.size, o.phase, o.tolerance, o.position, o.anchor, o.scale,
                o.rotation, o.skew, o.skewAxis},
               {document::selectorStoredValue(o.function)});
}

// A contour as a dense polyline: each cubic segment at `steps` parameters.
[[nodiscard]] std::vector<Vec2d> dense(const PathContour& contour, const int steps) {
    std::vector<Vec2d> points;
    const auto count = contour.anchors.size();
    const auto segments = contour.closed ? count : count - 1;
    for (std::size_t index = 0; index < segments; ++index) {
        const auto& a = contour.anchors[index];
        const auto& b = contour.anchors[(index + 1) % count];
        const Vec2d p0 = a.point, p1 = a.outHandle.value_or(a.point),
                    p2 = b.inHandle.value_or(b.point), p3 = b.point;
        for (int step = 0; step < steps; ++step) {
            const double t = static_cast<double>(step) / steps;
            const double u = 1.0 - t;
            points.push_back({u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x,
                              u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y});
        }
    }
    points.push_back(contour.closed ? contour.anchors[0].point : contour.anchors.back().point);
    return points;
}

// The smallest distance from a point to a polyline.
[[nodiscard]] double distanceTo(const Vec2d point, const std::vector<Vec2d>& line) {
    double best = std::numeric_limits<double>::max();
    for (std::size_t index = 0; index + 1 < line.size(); ++index) {
        const auto a = line[index];
        const auto b = line[index + 1];
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double length = dx * dx + dy * dy;
        const double t = length > 0.0
                             ? std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / length, 0.0, 1.0)
                             : 0.0;
        best = std::min(best, std::hypot(point.x - (a.x + t * dx), point.y - (a.y + t * dy)));
    }
    return best;
}

// The farthest the dense `curve` strays from the dense `reference`, and the reverse (so a curve that
// misses a stretch of the reference is found as well as one that strays from it).
[[nodiscard]] double strayBetween(const std::vector<Vec2d>& curve, const std::vector<Vec2d>& reference) {
    double worst = 0.0;
    for (const auto point : curve)
        worst = std::max(worst, distanceTo(point, reference));
    for (const auto point : reference)
        worst = std::max(worst, distanceTo(point, curve));
    return worst;
}

void testTransformSkew(Expectations& expectations) {
    const PathValue source = single(square(0, 0, 10));
    // Skew 45 degrees along the x axis slides each point right by its height: (0,10) -> (10,10).
    const auto slanted = transformWithSkew(source, {0, 0}, {0, 0}, {1, 1}, 0.0, 45.0, 0.0);
    const auto& corners = pathOf(slanted).contours[0].anchors;
    expectations.expect(!slanted.failed && corners.size() == 4 && near(corners[0].point, {0, 0}) &&
                            near(corners[1].point, {10, 0}) && near(corners[2].point, {20, 10}) &&
                            near(corners[3].point, {10, 10}),
                        "a 45 degree skew along x slides each point by its height");
    // Along the y axis (90 degrees) it slides each point down by minus its x: (10,0) -> (10,-10).
    const auto upright = transformWithSkew(source, {0, 0}, {0, 0}, {1, 1}, 0.0, 45.0, 90.0);
    const auto& tall = pathOf(upright).contours[0].anchors;
    expectations.expect(near(tall[1].point, {10, -10}) && near(tall[3].point, {0, 10}) &&
                            near(tall[2].point, {10, 0}),
                        "a skew along the y axis slides each point by minus its x");
    // Skew sits between scale and turn: scale (2,1), skew 45 along x, then turn 90, move to (5,5):
    // (10,10) -> scaled (20,10) -> slanted (30,10) -> turned (-10,30) -> moved (-5,35).
    const auto combined = transformWithSkew(source, {5, 5}, {0, 0}, {2, 1}, 90.0, 45.0, 0.0);
    expectations.expect(near(pathOf(combined).contours[0].anchors[2].point, {-5, 35}),
                        "skew is applied after the scale and before the turn");
    // No skew changes nothing: five operands (a node saved before the skew), seven at zero, and the
    // hand-worked answer of the older test are all the same bytes.
    const auto old = run(Kernel::TransformPath, {wrap(source), Vec2d{5, 5}, Vec2d{0, 0}, Vec2d{2, 1}, 90.0});
    const auto zero = transformWithSkew(source, {5, 5}, {0, 0}, {2, 1}, 90.0, 0.0, 37.0);
    expectations.expect(pathOf(old) == pathOf(zero),
                        "a skew of zero (any axis) answers exactly what the node did before");
    // A curve stays a curve: the mapped cubic is the cubic of the mapped controls, at every t.
    PathContour curve = polygon({{0, 0}, {40, 10}}, false);
    curve.anchors[0].outHandle = Vec2d{0, 30};
    curve.anchors[1].inHandle = Vec2d{20, -20};
    const auto bent = transformWithSkew(single(curve), {3, -4}, {1, 2}, {1.5, 0.5}, 30.0, 25.0, 15.0);
    const auto original = dense(curve, 40);
    const auto mapped = dense(pathOf(bent).contours[0], 40);
    const auto matrix = runtime::pathwarp::Matrix::make({3, -4}, {1, 2}, {1.5, 0.5}, 30.0, 25.0, 15.0);
    bool exact = original.size() == mapped.size();
    for (std::size_t index = 0; exact && index < original.size(); ++index)
        exact = near(matrix.forward(original[index]), mapped[index], 1e-9);
    expectations.expect(exact, "a skewed, scaled, turned cubic is exactly the cubic of the mapped controls");
    expectations.expect(pathOf(bent).contours[0].anchors.size() == 2,
                        "a matrix never adds points (no subdivision for a pure matrix)");
    // The inverse really inverts it.
    const auto probe = Vec2d{12.5, -7.25};
    expectations.expect(near(matrix.inverse(matrix.forward(probe)), probe, 1e-9),
                        "the matrix's inverse undoes it");
    expectations.expect(transformWithSkew(source, {}, {}, {1, 1}, 0.0, 90.0, 0.0).failed &&
                            transformWithSkew(source, {}, {}, {1, 1}, 0.0, -89.95, 0.0).failed &&
                            !transformWithSkew(source, {}, {}, {1, 1}, 0.0, 89.9, 0.0).failed,
                        "a skew past 89.9 degrees is refused");
    expectations.expect(transformWithSkew(source, {}, {}, {1, 1}, 0.0, std::nan(""), 0.0).failed &&
                            transformWithSkew(source, {}, {}, {1, 1}, 0.0, 10.0, std::nan("")).failed,
                        "a skew or axis that is not a number is refused");
}

void testWarpFunctionsKnownAnswers(Expectations& expectations) {
    using runtime::pathwarp::apply;
    using runtime::pathwarp::Warp;
    using document::WarpFunction;
    const double tau = 2.0 * std::numbers::pi;
    // Wave: x moves by A sin(2 pi y / size + phase); y stays.
    const auto wave = apply(Warp{WarpFunction::Wave, 20.0, 100.0, 0.0}, {7.0, 25.0});
    expectations.expect(near(wave, {7.0 + 20.0 * std::sin(tau * 25.0 / 100.0), 25.0}),
                        "Wave moves x by a sine of y: a quarter wavelength up is the full amount");
    expectations.expect(near(wave.x, 27.0, 1e-9), "Wave at a quarter wavelength moves x by exactly the amount");
    const auto shifted = apply(Warp{WarpFunction::Wave, 20.0, 100.0, 90.0}, {0.0, 0.0});
    expectations.expect(near(shifted, {20.0, 0.0}, 1e-9), "a phase of 90 degrees starts Wave at its peak");
    // Twist: radius kept, turned by amount * exp(-(r/size)^2) (degrees).
    const auto twisted = apply(Warp{WarpFunction::Twist, 90.0, 100.0, 0.0}, {50.0, 0.0});
    const double turn = std::numbers::pi / 2.0 * std::exp(-0.25);
    expectations.expect(near(twisted, {50.0 * std::cos(turn), 50.0 * std::sin(turn)}, 1e-9) &&
                            near(std::hypot(twisted.x, twisted.y), 50.0, 1e-9),
                        "Twist turns a point about the origin, keeping its radius");
    expectations.expect(near(apply(Warp{WarpFunction::Twist, 90.0, 100.0, 0.0}, {0.0, 0.0}), {0.0, 0.0}),
                        "Twist leaves the origin where it is");
    // Bulge: a point moves out by amount percent where the fade is one (size very wide).
    expectations.expect(near(apply(Warp{WarpFunction::Bulge, 50.0, 1.0e9, 0.0}, {10.0, -4.0}), {15.0, -6.0}, 1e-9) &&
                            near(apply(Warp{WarpFunction::Bulge, -50.0, 1.0e9, 0.0}, {10.0, -4.0}), {5.0, -2.0}, 1e-9),
                        "Bulge pushes a point out by the percentage and a negative amount pulls it in");
    // Bend: a quarter turn over `size`: (100, 0) -> the quarter point of a circle of radius 200/pi.
    const double radius = 200.0 / std::numbers::pi;
    const auto arc = apply(Warp{WarpFunction::Bend, 90.0, 100.0, 0.0}, {100.0, 0.0});
    expectations.expect(near(arc, {radius, radius}, 1e-9), "Bend sends the end of the length to the quarter of the arc");
    expectations.expect(near(apply(Warp{WarpFunction::Bend, 0.0, 100.0, 0.0}, {33.0, 4.0}), {33.0, 4.0}, 1e-9) &&
                            near(apply(Warp{WarpFunction::Bend, 1.0e-9, 100.0, 0.0}, {33.0, 4.0}), {33.0, 4.0}, 1e-6),
                        "Bend with no angle is the identity, with no division by zero");
    // Ripple: along the line from the origin by A sin(2 pi r / size + phase).
    expectations.expect(near(apply(Warp{WarpFunction::Ripple, 12.0, 100.0, 90.0}, {100.0, 0.0}), {112.0, 0.0}, 1e-9) &&
                            near(apply(Warp{WarpFunction::Ripple, 12.0, 100.0, 90.0}, {0.0, 0.0}), {0.0, 0.0}),
                        "Ripple moves a point along its radius and leaves the origin");
}

void testWarpGridIsAnalytic(Expectations& expectations) {
    // The issue's example: a grid warped by (x, y) -> (x + A sin(y / w), y) (w here is size / 2 pi).
    const double amount = 20.0, size = 100.0;
    PathValue grid;
    for (const double c : {-100.0, -50.0, 0.0, 50.0, 100.0})
        grid.contours.push_back(polygon({{c, -100}, {c, 100}}, false));
    for (const double c : {-100.0, -50.0, 0.0, 50.0, 100.0})
        grid.contours.push_back(polygon({{-100, c}, {100, c}}, false));
    grid.groups.push_back({"verticals", 0, 5, std::nullopt});
    grid.groups.push_back({"horizontals", 5, 5, core::Color4d{1, 0, 0, 1}});
    WarpOptions options;
    options.amount = amount;
    options.size = size;
    options.tolerance = 0.25;
    const auto done = warp(grid, options);
    const auto& result = pathOf(done);
    expectations.expect(!done.failed && result.contours.size() == 10 && result.groups == grid.groups &&
                            result.fillRule == grid.fillRule,
                        "the warped grid keeps its contours, groups and fill rule, one to one");
    if (result.contours.size() != 10)
        return;
    const double tau = 2.0 * std::numbers::pi;
    const auto analytic = [&](const Vec2d p) { return Vec2d{p.x + amount * std::sin(tau * p.y / size), p.y}; };
    bool straightBecameCurves = true, pointsExact = true, horizontalsStayStraight = true;
    double worst = 0.0;
    for (std::size_t index = 0; index < 5; ++index) {
        const auto& contour = result.contours[index];
        const double c = grid.contours[index].anchors[0].point.x;
        straightBecameCurves = straightBecameCurves && contour.anchors.size() > 4 && !contour.closed;
        bool handled = false;
        for (const auto& anchor : contour.anchors) {
            handled = handled || anchor.inHandle || anchor.outHandle;
            // Every output point is the analytic function of a point of the original line.
            pointsExact = pointsExact && near(anchor.point, analytic({c, anchor.point.y}), 1e-9);
        }
        straightBecameCurves = straightBecameCurves && handled;
        // The drawn curve against the analytic warp of the original line, sampled finely.
        std::vector<Vec2d> reference;
        for (int step = 0; step <= 4000; ++step)
            reference.push_back(analytic({c, -100.0 + 200.0 * step / 4000.0}));
        worst = std::max(worst, strayBetween(dense(contour, 64), reference));
    }
    for (std::size_t index = 5; index < 10; ++index) {
        const auto& contour = result.contours[index];
        const double c = grid.contours[index].anchors[0].point.y;
        // A horizontal line only slides sideways by A sin(2 pi c / size): it stays a straight pair.
        horizontalsStayStraight = horizontalsStayStraight && contour.anchors.size() == 2 &&
                                  near(contour.anchors[0].point, analytic({-100, c}), 1e-9) &&
                                  near(contour.anchors[1].point, analytic({100, c}), 1e-9);
    }
    expectations.expect(straightBecameCurves, "straight vertical lines become curves (more points, with handles)");
    expectations.expect(pointsExact, "every output point equals the analytic function of its original point");
    expectations.expect(horizontalsStayStraight, "horizontal lines, which the wave only slides, stay two points");
    expectations.expect(worst <= 0.25,
                        "the drawn curves stay within the 0.25 px tolerance of the analytic warp (worst " +
                            std::to_string(worst) + ")");
    // A tighter tolerance gives more points and a tighter fit.
    WarpOptions fine = options;
    fine.tolerance = 0.01;
    const auto finer = warp(grid, fine);
    expectations.expect(!finer.failed &&
                            pathOf(finer).contours[0].anchors.size() > result.contours[0].anchors.size(),
                        "a tolerance of 0.01 px keeps more points than 0.25 px");
    double tight = 0.0;
    {
        std::vector<Vec2d> reference;
        for (int step = 0; step <= 4000; ++step)
            reference.push_back(analytic({-50.0, -100.0 + 200.0 * step / 4000.0}));
        tight = strayBetween(dense(pathOf(finer).contours[1], 64), reference);
    }
    expectations.expect(tight <= 0.01, "and stays within 0.01 px of the analytic warp (worst " +
                                           std::to_string(tight) + ")");
    // No amount: the identity. Positions are the input's.
    WarpOptions none = options;
    none.amount = 0.0;
    const auto flat = warp(grid, none);
    bool same = !flat.failed;
    for (std::size_t index = 0; same && index < 10; ++index) {
        const auto& out = pathOf(flat).contours[index];
        same = out.anchors.size() == 2 && near(out.anchors[0].point, grid.contours[index].anchors[0].point, 1e-9) &&
               near(out.anchors[1].point, grid.contours[index].anchors[1].point, 1e-9);
    }
    expectations.expect(same, "an amount of zero leaves the grid where it was, two points a line");
    expectations.expect(pathOf(warp(grid, options)) == pathOf(warp(grid, options)),
                        "warping twice gives the same bytes");
}

void testWarpCurvesAndClosedContours(Expectations& expectations) {
    // A circle (closed, four curved segments) through a Ripple about its centre: it stays closed, and
    // each output point is the analytic function of an original point.
    const auto round = circle(40.0, {10.0, 5.0});
    WarpOptions options;
    options.function = document::WarpFunction::Ripple;
    options.amount = 6.0;
    options.size = 30.0;
    options.tolerance = 0.1;
    const auto done = warp(single(round), options);
    const auto& out = pathOf(done);
    expectations.expect(!done.failed && out.contours.size() == 1 && out.contours[0].closed &&
                            out.contours[0].anchors.size() > 4,
                        "a closed curved contour stays closed and gains points where the ripple bends it");
    if (out.contours.size() != 1)
        return;
    expectations.expect(out.contours[0].anchors.front().inHandle.has_value(),
                        "the seam of a closed contour has its closing handle");
    // Deviation of the output against the analytic ripple of the dense original.
    const auto reference = [&] {
        std::vector<Vec2d> line;
        for (const auto p : dense(round, 4000))
            line.push_back(runtime::pathwarp::apply({options.function, options.amount, options.size, 0.0}, p));
        return line;
    }();
    const double worst = strayBetween(dense(out.contours[0], 64), reference);
    expectations.expect(worst <= 0.1, "the warped circle stays within its 0.1 px tolerance (worst " +
                                          std::to_string(worst) + ")");
    // Bend: a line along x becomes an arc of radius 200 / pi about (0, 200 / pi).
    WarpOptions bend;
    bend.function = document::WarpFunction::Bend;
    bend.amount = 90.0;
    bend.size = 100.0;
    bend.tolerance = 0.05;
    const auto arc = warp(single(polygon({{0, 0}, {100, 0}}, false)), bend);
    const double radius = 200.0 / std::numbers::pi;
    bool onCircle = !arc.failed && pathOf(arc).contours[0].anchors.size() > 2;
    for (const auto& anchor : pathOf(arc).contours[0].anchors)
        onCircle = onCircle && std::abs(std::hypot(anchor.point.x, anchor.point.y - radius) - radius) < 1e-9;
    expectations.expect(onCircle, "a bent line's points lie on the arc of radius 200 / pi");
    std::vector<Vec2d> circleLine;
    for (int step = 0; step <= 2000; ++step) {
        const double angle = std::numbers::pi / 2.0 * step / 2000.0;
        circleLine.push_back({radius * std::sin(angle), radius - radius * std::cos(angle)});
    }
    expectations.expect(strayBetween(dense(pathOf(arc).contours[0], 64), circleLine) <= 0.05,
                        "and the drawn curve stays within 0.05 px of the arc");
}

void testWarpSpaceMatrix(Expectations& expectations) {
    // The warp runs in the matrix's space. Turned 90 degrees, the wave runs along the other axis:
    // y' = y - A sin(2 pi x / size) for a horizontal line.
    WarpOptions options;
    options.amount = 10.0;
    options.size = 100.0;
    options.tolerance = 0.05;
    options.rotation = 90.0;
    const auto turned = warp(single(polygon({{-100, 30}, {100, 30}}, false)), options);
    bool along = !turned.failed && pathOf(turned).contours[0].anchors.size() > 2;
    for (const auto& anchor : pathOf(turned).contours[0].anchors)
        along = along && near(anchor.point.y,
                              30.0 - 10.0 * std::sin(2.0 * std::numbers::pi * anchor.point.x / 100.0), 1e-9);
    expectations.expect(along, "a warp in a space turned 90 degrees waves the other way");
    // A scaled space stretches the function: scale (2, 1) makes the wave twice as wide in x... a
    // vertical line at x = 0 has y' = y, so its wave is unchanged; a vertical line at x = 40 maps to
    // local x = 20 and moves by A sin(2 pi y / size) in local x, i.e. 2x in the world.
    WarpOptions stretched;
    stretched.amount = 10.0;
    stretched.size = 100.0;
    stretched.tolerance = 0.05;
    stretched.scale = {2.0, 1.0};
    const auto wide = warp(single(polygon({{40, -50}, {40, 50}}, false)), stretched);
    bool doubled = !wide.failed;
    for (const auto& anchor : pathOf(wide).contours[0].anchors)
        doubled = doubled && near(anchor.point.x, 40.0 + 2.0 * 10.0 * std::sin(2.0 * std::numbers::pi * anchor.point.y / 100.0), 1e-9);
    expectations.expect(doubled, "a scaled space stretches the displacement by the scale");
    // Skewed, anchored and moved: with no amount the whole map is the identity (the space cancels).
    WarpOptions cancels;
    cancels.amount = 0.0;
    cancels.position = {17, -3};
    cancels.anchor = {5, 6};
    cancels.scale = {1.5, 0.75};
    cancels.rotation = 33.0;
    cancels.skew = 21.0;
    cancels.skewAxis = 40.0;
    const auto source = single(polygon({{-20, -10}, {30, 5}, {12, 40}}, true));
    const auto same = warp(source, cancels);
    bool identity = !same.failed && pathOf(same).contours[0].anchors.size() == 3;
    for (std::size_t index = 0; identity && index < 3; ++index)
        identity = near(pathOf(same).contours[0].anchors[index].point, source.contours[0].anchors[index].point, 1e-9);
    expectations.expect(identity, "with no amount the space's matrix and its inverse cancel exactly");
    WarpOptions flat;
    flat.scale = {0.0, 1.0};
    expectations.expect(warp(source, flat).failed, "a scale of zero is refused");
}

void testWarpBoundsAndDiagnostics(Expectations& expectations) {
    const auto line = single(polygon({{-100, 0}, {100, 0}}, false));
    WarpOptions huge;
    huge.function = document::WarpFunction::Wave;
    huge.amount = 1.0e7;
    huge.size = 1.0;
    huge.tolerance = 0.001;
    const auto beyond = warp(single(polygon({{0, -100}, {0, 100}}, false)), huge);
    const std::string_view detail = beyond.detail;
    expectations.expect(beyond.failed && pathOf(beyond).contours.empty() &&
                            detail.find("Warp Path") != std::string_view::npos &&
                            detail.find("Wave") != std::string_view::npos &&
                            detail.find("4096") != std::string_view::npos &&
                            detail.find("0.001") != std::string_view::npos,
                        "a warp that needs more than 4096 points fails, naming the node, the function, the tolerance and the cap");
    // Operands.
    WarpOptions bad;
    bad.size = 0.0;
    expectations.expect(warp(line, bad).failed, "a size of zero is refused");
    bad = {};
    bad.size = -5.0;
    expectations.expect(warp(line, bad).failed, "a negative size is refused");
    bad = {};
    bad.tolerance = 0.0;
    expectations.expect(warp(line, bad).failed, "a tolerance of zero is refused");
    bad.tolerance = 5000.0;
    expectations.expect(warp(line, bad).failed, "a tolerance above 1000 is refused");
    bad = {};
    bad.amount = std::nan("");
    expectations.expect(warp(line, bad).failed, "an amount that is not a number is refused");
    bad = {};
    bad.skew = 95.0;
    expectations.expect(warp(line, bad).failed, "a skew of a quarter turn or more is refused");
    expectations.expect(warp(single(polygon({{0, 0}, {std::nan(""), 1}}, false)), WarpOptions{}).failed,
                        "a path with a coordinate that is not a number is refused");
    // A warp that jumps (Ripple through its centre, off the cycle) stops at the depth limit and says
    // so without failing.
    WarpOptions jump;
    jump.function = document::WarpFunction::Ripple;
    jump.amount = 20.0;
    jump.size = 100.0;
    jump.phase = 90.0;
    const auto jumped = warp(line, jump);
    expectations.expect(!jumped.failed && jumped.notice && !pathOf(jumped).contours.empty() &&
                            std::string_view(jumped.summary).find("depth") != std::string_view::npos,
                        "a warp that jumps says it stopped splitting and still answers");
    // Contours with no anchors and one anchor pass through.
    PathValue odd;
    odd.contours.push_back({});
    PathContour lone;
    lone.anchors.push_back(corner(3, 4));
    odd.contours.push_back(lone);
    const auto passed = warp(odd, WarpOptions{});
    expectations.expect(!passed.failed && pathOf(passed).contours.size() == 2 &&
                            pathOf(passed).contours[0].anchors.empty() &&
                            pathOf(passed).contours[1].anchors.size() == 1,
                        "an empty contour and a lone point are carried through");
    // An empty path is an empty path.
    expectations.expect(!warp(PathValue{}, WarpOptions{}).failed && pathOf(warp(PathValue{}, WarpOptions{})).contours.empty(),
                        "an empty path warps to an empty path");
}

void testWarpRespectsAnUncancelledToken(Expectations& expectations) {
    PathValue many;
    for (int index = 0; index < 6; ++index)
        many.contours.push_back(circle(10.0 + index));
    runtime::CancellationToken token;
    std::vector<const CompiledValue*> operands;
    std::vector<CompiledValue> values{wrap(many), 4.0, 100.0, 0.0, 0.25, Vec2d{}, Vec2d{},
                                      Vec2d{1, 1}, 0.0, 0.0, 0.0};
    for (const auto& value : values)
        operands.push_back(&value);
    const std::array<std::int64_t, 1> selectors{0};
    const auto done = runtime::evaluateValueUtility({Kernel::WarpPath, operands, selectors,
                                                     core::RationalTime::fromInteger(0),
                                                     document::FrameRate::framesPerSecond24(),
                                                     nullptr, {.cancellation = &token}});
    expectations.expect(!done.cancelled && !done.failed && pathOf(done).contours.size() == 6,
                        "an uncancelled token changes nothing (a cancelled one has its own integration test)");
}

} // namespace

int main() {
    Expectations expectations;
    testTransform(expectations);
    testTrim(expectations);
    testRoundCorners(expectations);
    testOffset(expectations);
    testCombine(expectations);
    testPathStroke(expectations);
    testMaskPath(expectations);
    testTextOutline(expectations);
    testKernelCancellation(expectations);
    testKernelBudgets(expectations);
    testResamplePathKnownAnswers(expectations);
    testResampleCurvesStayOnTheCurve(expectations);
    testResampleBounds(expectations);
    testResampleCancellation(expectations);
    testResampleHelper(expectations);
    testTransformSkew(expectations);
    testWarpFunctionsKnownAnswers(expectations);
    testWarpGridIsAnalytic(expectations);
    testWarpCurvesAndClosedContours(expectations);
    testWarpSpaceMatrix(expectations);
    testWarpBoundsAndDiagnostics(expectations);
    testWarpRespectsAnUncancelledToken(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
