#include <bloom/core/coordinate_space.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bloom::core::space {

namespace {

// w at or below this is a point at (or beyond) infinity.
constexpr double kMinimumW = 1.0e-12;

[[nodiscard]] bool finite(const Point2 point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

[[nodiscard]] Matrix3 multiply3(const Matrix3& left, const Matrix3& right) noexcept {
    Matrix3 result{};
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column) {
            double sum = 0.0;
            for (std::size_t k = 0; k < 3; ++k)
                sum += left[row * 3 + k] * right[k * 3 + column];
            result[row * 3 + column] = sum;
        }
    return result;
}

} // namespace

bool operator==(const Space& left, const Space& right) noexcept {
    if (left.name != right.name || left.dimensions != right.dimensions ||
        left.toPixels != right.toPixels || left.range != right.range)
        return false;
    if (left.parent == right.parent)
        return true;
    return left.parent != nullptr && right.parent != nullptr && *left.parent == *right.parent;
}

Matrix4 identity4() noexcept {
    return {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
}

Matrix4 multiply(const Matrix4& left, const Matrix4& right) noexcept {
    Matrix4 result{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column) {
            double sum = 0.0;
            for (std::size_t k = 0; k < 4; ++k)
                sum += left[row * 4 + k] * right[k * 4 + column];
            result[row * 4 + column] = sum;
        }
    return result;
}

Matrix4 embed2d(const Matrix3& map) noexcept {
    return {map[0], map[1], 0.0, map[2], map[3], map[4], 0.0, map[5],
            0.0,    0.0,    1.0, 0.0,    map[6], map[7], 0.0, map[8]};
}

Matrix3 planar(const Matrix4& map) noexcept {
    return {map[0], map[1], map[3], map[4], map[5], map[7], map[12], map[13], map[15]};
}

bool isAffine(const Matrix4& map) noexcept {
    return map[12] == 0.0 && map[13] == 0.0 && map[14] == 0.0 && map[15] == 1.0;
}

bool isFinite(const Matrix4& map) noexcept {
    for (const double entry : map)
        if (!std::isfinite(entry) || std::abs(entry) > kMaximumMagnitude)
            return false;
    return true;
}

std::optional<Point2> mapPoint(const Matrix4& map, const Point2 point) noexcept {
    const double x = map[0] * point.x + map[1] * point.y + map[3];
    const double y = map[4] * point.x + map[5] * point.y + map[7];
    const double w = map[12] * point.x + map[13] * point.y + map[15];
    if (!std::isfinite(w) || !(w > kMinimumW))
        return std::nullopt;
    // An affine map divides by exactly 1, so its pixels are the plain affine arithmetic.
    const Point2 result = w == 1.0 ? Point2{x, y} : Point2{x / w, y / w};
    if (!finite(result))
        return std::nullopt;
    return result;
}

std::optional<Matrix3> inverse(const Matrix3& m) noexcept {
    const double c00 = m[4] * m[8] - m[5] * m[7];
    const double c01 = m[5] * m[6] - m[3] * m[8];
    const double c02 = m[3] * m[7] - m[4] * m[6];
    const double determinant = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (!std::isfinite(determinant) || determinant == 0.0)
        return std::nullopt;
    // A map whose determinant is negligible against its entries is singular for every purpose.
    double magnitude = 0.0;
    for (const double entry : m)
        magnitude = std::max(magnitude, std::abs(entry));
    if (std::abs(determinant) <= 1.0e-14 * magnitude * magnitude * magnitude)
        return std::nullopt;
    const double r = 1.0 / determinant;
    Matrix3 result{c00 * r,
                   (m[2] * m[7] - m[1] * m[8]) * r,
                   (m[1] * m[5] - m[2] * m[4]) * r,
                   c01 * r,
                   (m[0] * m[8] - m[2] * m[6]) * r,
                   (m[2] * m[3] - m[0] * m[5]) * r,
                   c02 * r,
                   (m[1] * m[6] - m[0] * m[7]) * r,
                   (m[0] * m[4] - m[1] * m[3]) * r};
    for (const double entry : result)
        if (!std::isfinite(entry))
            return std::nullopt;
    return result;
}

std::optional<Point2> unmapPoint(const Matrix4& map, const Point2 pixel) noexcept {
    const auto inverted = inverse(planar(map));
    if (!inverted)
        return std::nullopt;
    const auto& m = *inverted;
    const double x = m[0] * pixel.x + m[1] * pixel.y + m[2];
    const double y = m[3] * pixel.x + m[4] * pixel.y + m[5];
    const double w = m[6] * pixel.x + m[7] * pixel.y + m[8];
    if (!std::isfinite(w) || w == 0.0)
        return std::nullopt;
    // The inverse of a projective map is defined up to scale, so only its sign-free ratio counts;
    // the pixel must also be in front of the projection, which mapPoint() checks on the way back.
    const Point2 result = w == 1.0 ? Point2{x, y} : Point2{x / w, y / w};
    if (!finite(result))
        return std::nullopt;
    if (!isAffine(map)) {
        const double forward = map[12] * result.x + map[13] * result.y + map[15];
        if (!(forward > kMinimumW))
            return std::nullopt;
    }
    return result;
}

std::optional<Jacobian2> jacobian(const Matrix4& map, const Point2 point) noexcept {
    const double x = map[0] * point.x + map[1] * point.y + map[3];
    const double y = map[4] * point.x + map[5] * point.y + map[7];
    const double w = map[12] * point.x + map[13] * point.y + map[15];
    if (!std::isfinite(w) || !(w > kMinimumW))
        return std::nullopt;
    Jacobian2 result;
    if (w == 1.0 && map[12] == 0.0 && map[13] == 0.0) {
        result.xAxis = {map[0], map[4]};
        result.yAxis = {map[1], map[5]};
    } else {
        const double w2 = w * w;
        result.xAxis = {(map[0] * w - x * map[12]) / w2, (map[4] * w - y * map[12]) / w2};
        result.yAxis = {(map[1] * w - x * map[13]) / w2, (map[5] * w - y * map[13]) / w2};
    }
    if (!finite(result.xAxis) || !finite(result.yAxis))
        return std::nullopt;
    return result;
}

Matrix3 placement(const Point2 pivot, const Point2 anchor, const Point2 scale,
                  const double rotationDegrees, const bool yUp) noexcept {
    // Whole quarter turns are exact, so an unrotated space is exactly its scale and offset.
    const double turns = rotationDegrees / 90.0;
    double cosine = 0.0;
    double sine = 0.0;
    if (turns == std::floor(turns) && std::abs(turns) < 1.0e9) {
        const auto quarter = static_cast<long long>(std::fmod(std::fmod(turns, 4.0) + 4.0, 4.0));
        cosine = quarter == 0 ? 1.0 : quarter == 2 ? -1.0 : 0.0;
        sine = quarter == 1 ? 1.0 : quarter == 3 ? -1.0 : 0.0;
    } else {
        const double radians = rotationDegrees * std::numbers::pi / 180.0;
        cosine = std::cos(radians);
        sine = std::sin(radians);
    }
    const double sx = scale.x;
    const double sy = yUp ? -scale.y : scale.y;
    // R * S, then the anchor moved to the pivot.
    const double a = cosine * sx;
    const double b = -sine * sy;
    const double d = sine * sx;
    const double e = cosine * sy;
    return {a,   b,   pivot.x - (a * anchor.x + b * anchor.y),
            d,   e,   pivot.y - (d * anchor.x + e * anchor.y),
            0.0, 0.0, 1.0};
}

std::optional<Matrix3> squareToQuad(const std::array<Point2, 4>& corners) noexcept {
    // Heckbert's closed form: (0,0), (1,0), (1,1), (0,1) to the corners in that order.
    for (const auto& corner : corners)
        if (!finite(corner))
            return std::nullopt;
    const auto& [p0, p1, p2, p3] = corners;
    const double dx1 = p1.x - p2.x;
    const double dx2 = p3.x - p2.x;
    const double dx3 = p0.x - p1.x + p2.x - p3.x;
    const double dy1 = p1.y - p2.y;
    const double dy2 = p3.y - p2.y;
    const double dy3 = p0.y - p1.y + p2.y - p3.y;
    Matrix3 map{};
    if (dx3 == 0.0 && dy3 == 0.0) {
        map = {p1.x - p0.x, p2.x - p1.x, p0.x, p1.y - p0.y, p2.y - p1.y, p0.y, 0.0, 0.0, 1.0};
    } else {
        const double determinant = dx1 * dy2 - dx2 * dy1;
        if (determinant == 0.0 || !std::isfinite(determinant))
            return std::nullopt;
        const double g = (dx3 * dy2 - dx2 * dy3) / determinant;
        const double h = (dx1 * dy3 - dx3 * dy1) / determinant;
        map = {p1.x - p0.x + g * p1.x, p3.x - p0.x + h * p3.x, p0.x,
               p1.y - p0.y + g * p1.y, p3.y - p0.y + h * p3.y, p0.y,
               g,                      h,                      1.0};
    }
    if (!inverse(map))
        return std::nullopt;
    // Every corner must be in front of the projection (a quad that folds through infinity is not a
    // plane seen from one side).
    const auto embedded = embed2d(map);
    for (const Point2 unit : {Point2{0, 0}, Point2{1, 0}, Point2{1, 1}, Point2{0, 1}})
        if (!mapPoint(embedded, unit))
            return std::nullopt;
    return map;
}

std::optional<Matrix3> rangeToQuad(const Range2& range,
                                   const std::array<Point2, 4>& corners) noexcept {
    const double width = range.maximum.x - range.minimum.x;
    const double height = range.maximum.y - range.minimum.y;
    if (!std::isfinite(width) || !std::isfinite(height) || !(width > 0.0) || !(height > 0.0))
        return std::nullopt;
    const auto quad = squareToQuad(corners);
    if (!quad)
        return std::nullopt;
    // The range to the unit square: u from min.x to max.x, v from max.y (top) to min.y.
    const Matrix3 normalize{1.0 / width,  0.0, -range.minimum.x / width,
                            0.0, -1.0 / height, range.maximum.y / height,
                            0.0, 0.0,           1.0};
    return multiply3(*quad, normalize);
}

std::optional<Readout> readout(const Matrix4& map, const Point2 at) noexcept {
    const auto origin = mapPoint(map, at);
    const auto axes = jacobian(map, at);
    if (!origin || !axes)
        return std::nullopt;
    Readout result;
    result.origin = *origin;
    result.xAxis = axes->xAxis;
    result.yAxis = axes->yAxis;
    result.scale = {std::hypot(axes->xAxis.x, axes->xAxis.y),
                    std::hypot(axes->yAxis.x, axes->yAxis.y)};
    result.rotationDegrees = result.scale.x > 0.0
                                 ? std::atan2(axes->xAxis.y, axes->xAxis.x) * 180.0 /
                                       std::numbers::pi
                                 : 0.0;
    return result;
}

} // namespace bloom::core::space
