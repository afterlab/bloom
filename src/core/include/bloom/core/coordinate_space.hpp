#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace bloom::core::space {

// #591 Coordinate spaces (docs/architecture/coordinate-spaces.md).
//
// A space is a named map from its own units to composition pixels. The map is ONE homogeneous 4x4
// matrix for every space, so a 2D graph space, a tracker's projective plane and a 3D world or camera
// space (#605) are the same value and need no second mechanism:
//
//   rows    (pixel x, pixel y, depth, w)
//   columns (unit x, unit y, unit z, 1)
//
// A 2D space has no z: its z column and depth row are those of the identity, and its 3x3 map
// [a b c; d e f; g h i] (columns x, y, 1; rows pixel x, pixel y, w) sits in rows and columns 0, 1, 3.
// A pixel is (row0 . p) / (row3 . p). The map is affine when row 3 is (0, 0, 0, 1).
using Matrix4 = std::array<double, 16>; // Row-major.
using Matrix3 = std::array<double, 9>;  // Row-major; the 2D form.

struct Point2 final {
    double x = 0.0;
    double y = 0.0;
    friend bool operator==(const Point2&, const Point2&) = default;
};

// The extent of the space in its own units (a graph's plotted range). Optional: a space need not
// have one.
struct Range2 final {
    Point2 minimum;
    Point2 maximum;
    friend bool operator==(const Range2&, const Range2&) = default;
};

struct Space final {
    std::string name;
    // 2 for the spaces this release builds; 3 is reserved for #605's world and camera spaces.
    std::uint8_t dimensions = 2;
    // Units to composition pixels, the parent's map already applied.
    Matrix4 toPixels{1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
    std::optional<Range2> range;
    // The space this one was defined in (null for a space defined in composition pixels). Its map
    // is already part of `toPixels`; it is kept so a readout can name it.
    std::shared_ptr<const Space> parent;

    friend bool operator==(const Space& left, const Space& right) noexcept;
};

[[nodiscard]] Matrix4 identity4() noexcept;
[[nodiscard]] Matrix4 multiply(const Matrix4& left, const Matrix4& right) noexcept;
// The 2D map of a 3x3 (columns x, y, 1) embedded as described above, and back.
[[nodiscard]] Matrix4 embed2d(const Matrix3& map) noexcept;
[[nodiscard]] Matrix3 planar(const Matrix4& map) noexcept;
[[nodiscard]] bool isAffine(const Matrix4& map) noexcept;
[[nodiscard]] bool isFinite(const Matrix4& map) noexcept;

// The pixel a point of a 2D space lands on; nothing when the point maps to infinity or behind the
// projection (w not finite or not above a tiny positive bound), or the result is not finite.
[[nodiscard]] std::optional<Point2> mapPoint(const Matrix4& map, Point2 point) noexcept;
// The point of the space a pixel is; nothing when the planar map is singular.
[[nodiscard]] std::optional<Point2> unmapPoint(const Matrix4& map, Point2 pixel) noexcept;
// The planar inverse (nothing when singular or not finite).
[[nodiscard]] std::optional<Matrix3> inverse(const Matrix3& map) noexcept;
// The derivative of the 2D map at a point: column 0 is the pixel step per unit of x, column 1 per
// unit of y ({dx/du, dy/du, dx/dv, dy/dv}). Exact for an affine map at any point.
struct Jacobian2 final {
    Point2 xAxis;
    Point2 yAxis;
};
[[nodiscard]] std::optional<Jacobian2> jacobian(const Matrix4& map, Point2 point) noexcept;

// The planar map a Coordinate Space node defines in its parent's units:
//   T(pivot) * R(rotation, clockwise degrees, Y down) * S(scale.x, yUp ? -scale.y : scale.y) * T(-anchor)
// so the space's `anchor` point lands on `pivot` and one unit along x is `scale.x` parent units.
[[nodiscard]] Matrix3 placement(Point2 pivot, Point2 anchor, Point2 scale, double rotationDegrees,
                                bool yUp) noexcept;
// The projective map that takes the unit square's corners (0,0), (1,0), (1,1), (0,1) to the four
// points in that order; nothing when three of them are collinear.
[[nodiscard]] std::optional<Matrix3> squareToQuad(const std::array<Point2, 4>& corners) noexcept;
// The projective map taking the range's corners (min.x,max.y), (max.x,max.y), (max.x,min.y),
// (min.x,min.y) -- top left, top right, bottom right and bottom left of a Y-up range -- to the four
// corners in that order. Nothing for an empty range or degenerate corners.
[[nodiscard]] std::optional<Matrix3> rangeToQuad(const Range2& range,
                                                 const std::array<Point2, 4>& corners) noexcept;

// The readouts a Space Readout answers: where the space's origin lands, the pixel step of one unit
// along each axis there, the axes' lengths and the angle of the x axis (clockwise degrees, Y down).
struct Readout final {
    Point2 origin;
    Point2 xAxis;
    Point2 yAxis;
    Point2 scale;
    double rotationDegrees = 0.0;
};
[[nodiscard]] std::optional<Readout> readout(const Matrix4& map, Point2 at = {}) noexcept;

// The largest magnitude a space's numbers may have; beyond it a definition is refused.
inline constexpr double kMaximumMagnitude = 1.0e12;

} // namespace bloom::core::space
