// #344 Transform Path, Trim Path and Offset Path; #530 Boolean Paths, Merge Paths and Round
// Corners, against answers worked out by hand.

#include <bloom/runtime/value_utility_kernels.hpp>

#include <bloom/document/path_value.hpp>
#include <bloom/document/value_utility_nodes.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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

} // namespace

int main() {
    Expectations expectations;
    testTransform(expectations);
    testTrim(expectations);
    testRoundCorners(expectations);
    testOffset(expectations);
    testCombine(expectations);
    return expectations.failures() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
