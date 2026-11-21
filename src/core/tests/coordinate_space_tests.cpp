#include <bloom/core/coordinate_space.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <source_location>
#include <string_view>

namespace {

using namespace bloom::core::space;

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

bool near(const double a, const double b, const double tolerance = 1e-9) {
    return std::abs(a - b) <= tolerance;
}

bool near(const std::optional<Point2>& a, const Point2 b, const double tolerance = 1e-9) {
    return a && near(a->x, b.x, tolerance) && near(a->y, b.y, tolerance);
}

void testPlacement(Expectations& e) {
    // A graph space: origin at (960, 540), 100 px a unit, Y up.
    const auto map = embed2d(placement({960, 540}, {0, 0}, {100, 100}, 0.0, true));
    e.expect(isAffine(map), "a placement is affine");
    e.expect(mapPoint(map, {0, 0}) == Point2{960, 540}, "the origin lands on the pivot exactly");
    e.expect(mapPoint(map, {2, 3}) == Point2{1160, 240}, "Y up: a positive y rises");
    e.expect(near(unmapPoint(map, {1160, 240}), {2, 3}), "the inverse returns the point");
    // The same numbers as Function Plot's origin + (x * ppu.x, -y * ppu.y), bit for bit.
    for (const double x : {-4.75, -1.0 / 3.0, 0.1, 2.5})
        for (const double y : {-2.0, 0.3, 7.125}) {
            const auto pixel = mapPoint(map, {x, y});
            e.expect(pixel && pixel->x == 960 + x * 100 && pixel->y == 540 - y * 100,
                     "an unrotated placement is exactly origin plus scaled units");
        }
    // Y down keeps the sign.
    const auto down = embed2d(placement({10, 20}, {0, 0}, {2, 3}, 0.0, false));
    e.expect(mapPoint(down, {1, 1}) == Point2{12, 23}, "Y down");
    // Quarter turns are exact; 90 degrees clockwise (Y down) takes +x to +y on screen.
    const auto turned = embed2d(placement({0, 0}, {0, 0}, {10, 10}, 90.0, false));
    e.expect(mapPoint(turned, {1, 0}) == Point2{0, 10}, "a quarter turn is exact and clockwise");
    // An anchor lands on the pivot.
    const auto anchored = embed2d(placement({500, 300}, {2, 1}, {50, 50}, 30.0, true));
    e.expect(near(mapPoint(anchored, {2, 1}), {500, 300}), "the anchor lands on the pivot");
    // Independent reference: rotation by 30 degrees of (dx, -dy) * 50.
    const double c = std::cos(std::numbers::pi / 6.0);
    const double s = std::sin(std::numbers::pi / 6.0);
    const double dx = (3.0 - 2.0) * 50.0;
    const double dy = -(4.0 - 1.0) * 50.0;
    e.expect(near(mapPoint(anchored, {3, 4}), {500 + c * dx - s * dy, 300 + s * dx + c * dy}),
             "a rotated placement matches the reference");
    const auto read = readout(anchored, {2, 1});
    e.expect(read && near(read->rotationDegrees, 30.0) && near(read->scale.x, 50.0) &&
                 near(read->scale.y, 50.0),
             "the readout's rotation and scale are the definition's");
}

void testParentAndInverse(Expectations& e) {
    const auto parent = embed2d(placement({960, 540}, {0, 0}, {100, 100}, 0.0, true));
    // A child space in the parent's units: its origin at (2, 1) of the parent, half a unit a unit.
    const auto local = embed2d(placement({2, 1}, {0, 0}, {0.5, 0.5}, 0.0, false));
    const auto composed = multiply(parent, local);
    const auto expected = mapPoint(parent, *mapPoint(local, {3, -2}));
    e.expect(near(mapPoint(composed, {3, -2}), *expected), "a parented map composes the two");
    for (const Point2 pixel : {Point2{0, 0}, Point2{1920, 1080}, Point2{17.25, -400.5}})
        e.expect(near(mapPoint(composed, *unmapPoint(composed, pixel)), pixel, 1e-7),
                 "unmap then map returns the pixel");
    e.expect(!unmapPoint(embed2d(placement({0, 0}, {0, 0}, {0, 1}, 0, false)), {1, 1}),
             "a singular space has no inverse");
}

void testProjective(Expectations& e) {
    const Range2 range{{-5, -5}, {5, 5}};
    // A plane seen in perspective: the top edge narrower than the bottom.
    const std::array<Point2, 4> corners{Point2{700, 200}, Point2{1220, 200}, Point2{1500, 900},
                                        Point2{420, 900}};
    const auto quad = rangeToQuad(range, corners);
    e.expect(quad.has_value(), "a convex quad has a homography");
    if (!quad)
        return;
    const auto map = embed2d(*quad);
    e.expect(!isAffine(map), "a perspective quad is projective");
    e.expect(near(mapPoint(map, {-5, 5}), corners[0], 1e-7), "top left");
    e.expect(near(mapPoint(map, {5, 5}), corners[1], 1e-7), "top right");
    e.expect(near(mapPoint(map, {5, -5}), corners[2], 1e-7), "bottom right");
    e.expect(near(mapPoint(map, {-5, -5}), corners[3], 1e-7), "bottom left");
    // Straight lines stay straight: the midpoint of the left edge in units lies on the edge.
    const auto middle = mapPoint(map, {-5, 0});
    const double cross = (corners[3].x - corners[0].x) * (middle->y - corners[0].y) -
                         (corners[3].y - corners[0].y) * (middle->x - corners[0].x);
    e.expect(near(cross, 0.0, 1e-6), "a projective map keeps lines straight");
    // Perspective foreshortening: the unit midpoint lands above the quad's pixel midpoint.
    e.expect(middle->y < 550.0, "the far half is foreshortened");
    for (const Point2 pixel : {Point2{960, 500}, Point2{800, 700}})
        e.expect(near(mapPoint(map, *unmapPoint(map, pixel)), pixel, 1e-6),
                 "the projective inverse round-trips");
    // The derivative against a central difference.
    const auto axes = jacobian(map, {1, 2});
    const double h = 1e-6;
    const auto px = mapPoint(map, {1 + h, 2});
    const auto mx = mapPoint(map, {1 - h, 2});
    e.expect(axes && near(axes->xAxis.x, (px->x - mx->x) / (2 * h), 1e-3) &&
                 near(axes->xAxis.y, (px->y - mx->y) / (2 * h), 1e-3),
             "the projective derivative matches a difference");
    // Degenerate quads are refused.
    e.expect(!rangeToQuad(range, {Point2{0, 0}, Point2{1, 1}, Point2{2, 2}, Point2{3, 3}}),
             "collinear corners have no map");
    e.expect(!rangeToQuad(Range2{{1, 0}, {1, 1}}, corners), "an empty range has no map");
}

void testInfinity(Expectations& e) {
    Matrix3 behind{1, 0, 0, 0, 1, 0, 0, -1, 1};
    const auto map = embed2d(behind);
    e.expect(mapPoint(map, {0, 0.5}).has_value(), "in front of the projection");
    e.expect(!mapPoint(map, {0, 1}).has_value(), "w = 0 is at infinity");
    e.expect(!mapPoint(map, {0, 2}).has_value(), "w < 0 is behind the projection");
}

} // namespace

int main() {
    Expectations e;
    testPlacement(e);
    testParentAndInverse(e);
    testProjective(e);
    testInfinity(e);
    if (e.failures() != 0) {
        std::cerr << e.failures() << " coordinate space expectation(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "coordinate space: all expectations passed\n";
    return EXIT_SUCCESS;
}
