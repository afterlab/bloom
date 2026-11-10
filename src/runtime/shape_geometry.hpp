#pragma once

// A Shape Source's geometry and fill rule at one frame, shared by the CPU evaluator and GPU scene
// preparation so the two routes cannot disagree about what a shape is.

#include <bloom/runtime/compiled_plan.hpp>

#include <bloom/render/path_raster.hpp>

#include "cpu_composition_resolution.hpp"

#include <cstdint>
#include <vector>

namespace bloom::runtime::detail {

// One render path per contour, in order. A Path shape reads `path` (the value it resolved to this
// frame); the empty path answers one empty render path, so every kind answers at least one.
[[nodiscard]] inline std::vector<render::Path> shapeRenderPaths(const CompiledShape& shape,
                                                                const document::Vec2d size,
                                                                const ResolvedShapeLine& line,
                                                                const document::PathValue& path) {
    const auto w = size.x, h = size.y;
    std::vector<render::Path> paths;
    switch (shape.kind) {
    case document::ShapeKind::Rectangle:
        paths.push_back(render::rectanglePath(w, h, shape.cornerRadius));
        break;
    case document::ShapeKind::Ellipse:
        paths.push_back(render::ellipsePath(w, h));
        break;
    case document::ShapeKind::Triangle:
        paths.push_back(render::polygonPath(w, h, 3));
        break;
    case document::ShapeKind::Polygon:
        paths.push_back(render::polygonPath(w, h, static_cast<std::uint32_t>(shape.points),
                                            shape.cornerRadius));
        break;
    case document::ShapeKind::Star:
        paths.push_back(
            render::starPath(w, h, static_cast<std::uint32_t>(shape.points), shape.innerRatio));
        break;
    case document::ShapeKind::Line:
        paths.push_back(render::linePath({line.start.x, line.start.y}, {line.end.x, line.end.y}));
        break;
    case document::ShapeKind::Path:
        paths.reserve(path.contours.size());
        for (const auto& contour : path.contours) {
            auto& next = paths.emplace_back();
            next.closed = contour.closed;
            next.anchors.reserve(contour.anchors.size());
            for (const auto& anchor : contour.anchors) {
                render::PathAnchor a{{anchor.point.x, anchor.point.y}, {}, {}};
                if (anchor.inHandle)
                    a.inHandle = render::PathPoint{anchor.inHandle->x, anchor.inHandle->y};
                if (anchor.outHandle)
                    a.outHandle = render::PathPoint{anchor.outHandle->x, anchor.outHandle->y};
                next.anchors.push_back(a);
            }
        }
        if (paths.empty())
            paths.emplace_back();
        break;
    }
    return paths;
}

// #589: a path value's explicit fill rule wins over the Shape's `fillRule` parameter; a value that
// inherits, and every shape kind but Path, uses the parameter.
[[nodiscard]] inline render::PathFillRule shapeFillRule(const CompiledShape& shape,
                                                        const document::PathValue& path) noexcept {
    if (shape.kind == document::ShapeKind::Path) {
        if (path.fillRule == document::PathFillRule::NonZero)
            return render::PathFillRule::NonZero;
        if (path.fillRule == document::PathFillRule::EvenOdd)
            return render::PathFillRule::EvenOdd;
    }
    return static_cast<render::PathFillRule>(shape.fillRule);
}

} // namespace bloom::runtime::detail
