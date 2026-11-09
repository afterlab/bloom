#pragma once

// The pieces the path processing kernels share (docs/architecture/path-processing.md): refusing
// with a diagnostic, the bounds of a usable path, the trimmed window, and the conversions between a
// path value and the polygon library's polygons.

#include "value_utility_support.hpp"

#include <bloom/document/parameter.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/render/path_raster.hpp>

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::runtime::detail::pathsupport {

[[nodiscard]] inline ValueUtilityOutcome
refuse(ValueUtilityOutcome outcome, const std::string_view summary, const std::string_view detail) {
    outcome.failed = true;
    outcome.summary = summary;
    outcome.detail = detail;
    return outcome;
}

inline void publish(ValueUtilityOutcome& outcome, document::PathValue value,
                    const std::size_t slot = 0) {
    outcome.outputs[slot] =
        CompiledPathValue{std::make_shared<document::PathValue>(std::move(value))};
}

[[nodiscard]] inline bool finite(const document::Vec2d value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}
[[nodiscard]] inline bool inRange(const document::Vec2d value) noexcept {
    return finite(value) && std::abs(value.x) <= document::kMaximumPathCoordinate &&
           std::abs(value.y) <= document::kMaximumPathCoordinate;
}

// Every point and handle is a finite number a polygon operation can hold.
[[nodiscard]] inline bool coordinatesUsable(const document::PathValue& value) noexcept {
    for (const auto& contour : value.contours)
        for (const auto& anchor : contour.anchors)
            if (!inRange(anchor.point) || (anchor.inHandle && !inRange(*anchor.inHandle)) ||
                (anchor.outHandle && !inRange(*anchor.outHandle)))
                return false;
    return true;
}

// A result a path value can hold: each contour within the anchor cap and the whole within the
// value's caps.
[[nodiscard]] inline bool fitsValue(const document::PathValue& value) noexcept {
    if (value.contours.size() > document::kMaximumPathContours)
        return false;
    std::size_t total = 0;
    for (const auto& contour : value.contours) {
        if (contour.anchors.size() > document::kMaximumPathAnchors)
            return false;
        total += contour.anchors.size();
    }
    return total <= document::kMaximumPathValueAnchors;
}

[[nodiscard]] inline bool toleranceUsable(const double tolerance) noexcept {
    return std::isfinite(tolerance) && tolerance >= 0.001 && tolerance <= 1000.0;
}

// The trimmed stretch as fractions of the whole, at most two: a stretch that runs past the end of a
// looping path continues from its start.
[[nodiscard]] inline std::vector<std::pair<double, double>> windowsFor(const double startPercent,
                                                                       const double endPercent,
                                                                       const double offsetPercent,
                                                                       const bool wrap) {
    const double start = std::clamp(std::min(startPercent, endPercent) / 100.0, 0.0, 1.0);
    const double end = std::clamp(std::max(startPercent, endPercent) / 100.0, 0.0, 1.0);
    const double span = end - start;
    const double offset = offsetPercent / 100.0;
    if (!(span > 0.0))
        return {};
    if (span >= 1.0)
        return {{0.0, 1.0}};
    if (wrap) {
        double first = start + offset;
        first -= std::floor(first);
        const double last = first + span;
        if (last <= 1.0)
            return {{first, last}};
        return {{first, 1.0}, {0.0, last - 1.0}};
    }
    const double first = std::clamp(start + offset, 0.0, 1.0);
    const double last = std::clamp(end + offset, 0.0, 1.0);
    if (!(last > first))
        return {};
    return {{first, last}};
}

// A contour as the render library's path. A fill closes an open contour with a straight line; a
// stroke keeps it open.
[[nodiscard]] inline render::Path renderPath(const document::PathContour& contour,
                                             const bool closed) {
    render::Path path;
    path.closed = closed;
    path.anchors.reserve(contour.anchors.size());
    for (const auto& anchor : contour.anchors) {
        render::PathAnchor each{{anchor.point.x, anchor.point.y}, {}, {}};
        if (anchor.inHandle)
            each.inHandle = render::PathPoint{anchor.inHandle->x, anchor.inHandle->y};
        if (anchor.outHandle)
            each.outHandle = render::PathPoint{anchor.outHandle->x, anchor.outHandle->y};
        path.anchors.push_back(each);
    }
    return path;
}

// Polygons as a path value: closed corner anchors, filled by the even-odd rule, no groups.
[[nodiscard]] inline document::PathValue valueOf(const Clipper2Lib::PathsD& polygons) {
    document::PathValue value;
    value.fillRule = document::PathFillRule::EvenOdd;
    value.contours.reserve(polygons.size());
    for (const auto& polygon : polygons) {
        if (polygon.size() < 3)
            continue;
        document::PathContour contour;
        contour.closed = true;
        contour.anchors.reserve(polygon.size());
        for (const auto& point : polygon)
            contour.anchors.push_back(
                {{point.x, point.y}, std::nullopt, std::nullopt, document::PathAnchorMode::Corner});
        value.contours.push_back(std::move(contour));
    }
    return value;
}

} // namespace bloom::runtime::detail::pathsupport
