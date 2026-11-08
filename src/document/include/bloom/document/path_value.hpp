#pragma once

// #589: pure operations over a path value (docs/architecture/path-value.md).

#include <bloom/document/parameter.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace bloom::document {

// The number of segments in a contour: a closed one has a closing segment; fewer than two anchors
// have none.
[[nodiscard]] std::size_t contourSegmentCount(const PathContour& contour) noexcept;

// A segment's four Bezier control points. A missing handle coincides with its anchor.
[[nodiscard]] std::array<Vec2d, 4> contourSegmentControls(const PathContour& contour,
                                                          std::size_t segment);

// Splits one segment at parameter `t` (clamped to [0, 1]) without changing the shape: de Casteljau
// on the segment's own control points, so the two new segments are the old curve's two parts, and
// a straight segment stays straight with no handles. Answers the new anchor's index, or nullopt
// when the segment does not exist, `t` is not finite, or the contour is at its anchor cap.
std::optional<std::size_t> subdivideContourSegment(PathContour& contour, std::size_t segment,
                                                   double t);

// The groups two different paths share, as (index in `from`, index in `to`) pairs in `from`'s
// order. Keys compare as bytes; a group whose key the other path lacks has no partner.
[[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>>
matchPathGroups(const PathValue& from, const PathValue& to);

// True when a value is written in the original flat shape, {anchors, closed}: at most one contour,
// no fill rule, no groups. One open contour with no anchors is the one simple value that is not,
// because the flat empty path reads back as no contours at all. Every value has one spelling.
[[nodiscard]] bool pathValueHasFlatForm(const PathValue& value) noexcept;

// True when two keys may interpolate: the same contour count, and each contour's anchor count and
// closed flag match. Anything else holds the earlier key.
[[nodiscard]] bool pathKeysInterpolate(const PathValue& from, const PathValue& to) noexcept;

} // namespace bloom::document
