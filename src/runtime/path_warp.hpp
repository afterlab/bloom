#pragma once

// #598 Warp Path's computation, shared (docs/architecture/path-processing.md): the path matrix
// (position, anchor, scale, rotation and skew) that Transform Path applies exactly and Warp Path
// uses as the space its function runs in, the built-in warp functions, and the adaptive subdivision
// that bends straight and curved segments through a function so the drawn curve stays within a
// tolerance of the exact warp.
//
// Pure functions of their arguments: no clock, no state, no Qt. Nothing here keeps more points than
// the limits it is given: a request past them is reported, not clamped.

#include <bloom/document/parameter.hpp>
#include <bloom/document/warp_path_nodes.hpp>
#include <bloom/runtime/cancellation.hpp>

#include <cstddef>
#include <functional>
#include <optional>

namespace bloom::runtime::pathwarp {

// The matrix every path node shares: scale about the anchor, then slant (`skew` degrees along
// `skewAxis`, clockwise from screen right), then turn (clockwise on screen for a positive angle),
// then move the anchor to the position. It is a plain affine map, so a cubic maps to the cubic of
// its mapped controls and no subdivision is ever needed for it.
struct Matrix final {
    document::Vec2d position;
    document::Vec2d anchor;
    document::Vec2d scale{1.0, 1.0};
    double cosine = 1.0;
    double sine = 0.0;
    // The slant: tan(skew), and the unit direction it runs along.
    double slant = 0.0;
    document::Vec2d axis{1.0, 0.0};
    bool skewed = false;

    [[nodiscard]] static Matrix make(document::Vec2d position, document::Vec2d anchor,
                                     document::Vec2d scale, double rotationDegrees,
                                     double skewDegrees, double skewAxisDegrees) noexcept;
    // False when a scale is zero: there is no way back from a flattened plane.
    [[nodiscard]] bool invertible() const noexcept;
    [[nodiscard]] document::Vec2d forward(document::Vec2d point) const noexcept;
    [[nodiscard]] document::Vec2d inverse(document::Vec2d point) const noexcept;
};

// What a built-in warp needs: the amount (its unit is the function's), the size (a distance), the
// phase in degrees.
struct Warp final {
    document::WarpFunction function = document::WarpFunction::Wave;
    double amount = 0.0;
    double size = 100.0;
    double phase = 0.0;
};

// The warp of one point in the warp's own space. Not finite means the warp has no answer there.
[[nodiscard]] document::Vec2d apply(const Warp& warp, document::Vec2d point) noexcept;

// A map from a point of the path to where the warp puts it, both in the path's own coordinates.
// Empty means it could not answer there (a function that failed).
using PointMap = std::function<std::optional<document::Vec2d>(document::Vec2d)>;

struct Limits final {
    // The farthest, in the path's units, the drawn curve may stray from the exact warp.
    double tolerance = 0.25;
    // The most points one contour keeps and the most the rest of the value may still add.
    std::size_t maximumPoints = document::kMaximumPathAnchors;
    std::size_t remainingPoints = document::kMaximumPathValueAnchors;
    const CancellationToken* cancellation = nullptr;
};

enum class Status : std::uint8_t {
    Ok,
    // The contour needs more points than a contour or the value may hold; `requested` says how many
    // it had when it stopped.
    TooMany,
    // The map had no answer, or an answer that is not a number a path can hold.
    Unusable,
    Cancelled,
};

struct Result final {
    Status status = Status::Ok;
    document::PathContour contour;
    std::size_t requested = 0;
    // At least one stretch stopped splitting at the depth cap with the tolerance still unmet.
    bool depthReached = false;
};

// The contour through the map: a knot at every anchor and wherever a stretch deviated by more than
// the tolerance, each stretch a cubic through its two mapped ends with the mapped tangents.
[[nodiscard]] Result warpContour(const document::PathContour& contour, const PointMap& map,
                                 const Limits& limits);

} // namespace bloom::runtime::pathwarp
