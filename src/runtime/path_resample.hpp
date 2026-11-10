#pragma once

// #637 Resample Path's computation, shared (docs/architecture/path-processing.md): the arc-length
// measure of a contour, the points to place along it for a count or a gap, and the contour through
// those points as straight segments or as curves that keep the shape. Resample Path is the node;
// `resampleToCount` is the shared equal-count helper for any caller that wants points spread evenly
// by length. (The morph, #597, gives two contours the same count by subdividing instead, so every
// original anchor stays: path_morph.hpp.)
//
// Pure functions of their arguments: no clock, no state, no Qt. Nothing here allocates more than
// `maximumPoints` points: a request past it is reported, not clamped.

#include <bloom/document/parameter.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bloom::runtime::pathresample {

// One contour measured along its length. Segment `i` runs from anchor `i` to anchor `i + 1` (the
// last closed segment returns to anchor 0); `start` is its distance from the contour's first anchor.
struct Measure final {
    struct Segment final {
        std::array<document::Vec2d, 4> controls{};
        bool straight = false;
        double length = 0.0;
        double start = 0.0;
        // Cumulative chord length at 64 even parameter steps; empty for a straight segment.
        std::vector<double> table;
    };
    std::vector<Segment> segments;
    double length = 0.0;
    bool closed = false;
};

[[nodiscard]] Measure measureContour(const document::PathContour& contour);

// The point at distance `along` from the first anchor. A closed contour wraps; an open one clamps.
[[nodiscard]] document::Vec2d pointAt(const Measure& measure, double along);
// The unit direction of travel at that distance; `forward` picks the segment that starts there at an
// anchor, otherwise the one that ends there. A degenerate tangent falls back to the chord.
[[nodiscard]] document::Vec2d tangentAt(const Measure& measure, double along, bool forward);

// Anchor indices that are corners: the direction of travel changes by more than half a degree
// there. The ends of an open contour are not listed (a resampled open contour always keeps them).
[[nodiscard]] std::vector<std::size_t> cornerAnchors(const document::PathContour& contour,
                                                     const Measure& measure);

// How the points of one contour are decided.
struct ContourRequest final {
    // A set number of points; otherwise `gap` is the largest distance between neighbours, in pixels.
    std::optional<std::int64_t> count;
    double gap = 0.0;
    // Existing corners stay as points.
    bool keepCorners = true;
    // Where along the contour the points fall, 0 to 1: a closed contour slides its points by this
    // share of its length (and wraps); an open contour keeps its ends and slides its inner points by
    // up to one gap, never past a neighbour.
    double phase = 0.0;
    // Curves that keep the shape of the contour (the points are on it and the pieces between them
    // follow it), or straight segments.
    bool curves = true;
};

enum class ResampleStatus : std::uint8_t {
    Ok,
    // More points than `maximumPoints` were requested; `requested` says how many.
    TooMany,
    // The contour has no length: a Fixed count answers that many copies of its point (`degenerate`).
    // A gap leaves it as it is.
    Unusable,
};

struct ContourResult final {
    ResampleStatus status = ResampleStatus::Ok;
    document::PathContour contour;
    // The count that was asked for (or that the gap works out to) when it was too many.
    double requested = 0.0;
    bool degenerate = false;
};

// The contour with its points placed by `request`. A count under the smallest a contour of its kind
// keeps (2 open, 3 closed) is raised to it. `maximumPoints` bounds the answer (a contour's own cap).
[[nodiscard]] ContourResult resampleContour(const document::PathContour& contour,
                                            const ContourRequest& request,
                                            std::size_t maximumPoints);

// The same, with a measure already made (a caller that places several contours' points).
[[nodiscard]] ContourResult resampleContour(const document::PathContour& contour,
                                            const Measure& measure,
                                            const ContourRequest& request,
                                            std::size_t maximumPoints);

// The shared equal-count helper (#598, #597): the contour with exactly `count` points, spread evenly
// by length, as curves that keep its shape. Corners are not kept, so the count is exact.
[[nodiscard]] ContourResult resampleToCount(const document::PathContour& contour,
                                            std::size_t count);

// For the tolerance the node's document states: the largest distance between a dense flattening of
// `curve` and one of `reference`, taken both ways (each one's points to the other's polyline), so a
// curve that misses a stretch of the reference is found as well as one that strays from it.
[[nodiscard]] double deviation(const document::PathContour& curve,
                               const document::PathContour& reference);

} // namespace bloom::runtime::pathresample
