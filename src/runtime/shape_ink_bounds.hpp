#pragma once

// #774: the bounds of the ink a Shape Source's layer draws once the layer's transform has carried
// it. A layer's `output` box is the axis-aligned box of its content box's four mapped corners; that
// is the ink's box only while the transform keeps axes aligned. A rotated or sheared layer's box
// is larger than its ink (a ring, an arc, a trimmed path), so the layer also publishes the box of
// the ink itself, measured on the same flattened, stroked geometry the layer is drawn from. The CPU
// evaluator and the GPU scene preparation both call this one function, so the two routes agree.

#include <bloom/render/path_raster.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"
#include "shape_geometry.hpp"

#include <cmath>
#include <optional>
#include <span>

namespace bloom::runtime::detail {

// `map` takes a point of the shape's local space (the space its content bounds are in) to the
// composition. Nullopt when the transform keeps axes aligned (the layer's output box is then the
// ink's), when the shape draws nothing, or when its geometry cannot be measured.
template <typename Map>
[[nodiscard]] std::optional<ContentBounds>
shapeInkOutputBounds(const CompiledShape& shape, const CompiledCompositionPlan& plan,
                     const ResolvedEvaluation& sourceResolved, const Map& map,
                     const render::PathCancellation& cancelled = {}) {
    // The map is affine; read its matrix from three far-apart points to keep rounding small.
    constexpr double span = 1024.0;
    const auto origin = map(document::Vec2d{0.0, 0.0});
    const auto alongX = map(document::Vec2d{span, 0.0});
    const auto alongY = map(document::Vec2d{0.0, span});
    render::PathMatrix matrix;
    matrix.a = (alongX.x - origin.x) / span;
    matrix.b = (alongY.x - origin.x) / span;
    matrix.c = (alongX.y - origin.y) / span;
    matrix.d = (alongY.y - origin.y) / span;
    matrix.x = origin.x;
    matrix.y = origin.y;
    const double scale =
        std::abs(matrix.a) + std::abs(matrix.b) + std::abs(matrix.c) + std::abs(matrix.d);
    if (!std::isfinite(scale) || !std::isfinite(matrix.x) || !std::isfinite(matrix.y) ||
        scale == 0.0)
        return std::nullopt;
    constexpr double alignedTolerance = 1e-9;
    const bool unrotated = std::abs(matrix.b) + std::abs(matrix.c) <= alignedTolerance * scale;
    const bool quarterTurn = std::abs(matrix.a) + std::abs(matrix.d) <= alignedTolerance * scale;
    if (unrotated || quarterTurn)
        return std::nullopt;

    const auto size = resolveParameter(shape.size, plan, sourceResolved);
    const auto width = resolveParameter(shape.strokeWidth, plan, sourceResolved);
    const auto fillSwitch = resolvedFillEnabled(shape, sourceResolved);
    const auto strokeSwitch = resolvedStrokeEnabled(shape, sourceResolved);
    const auto* shapePath = resolvedShapePath(shape, sourceResolved);
    const auto line = resolvedShapeLine(shape, plan, sourceResolved);
    if (!size || !width || !fillSwitch || !strokeSwitch || shapePath == nullptr || !line)
        return std::nullopt;
    const bool fillEnabled = *fillSwitch && shape.kind != document::ShapeKind::Line;
    const bool strokeEnabled = *strokeSwitch && width->value > 0;
    if (!fillEnabled && !strokeEnabled)
        return std::nullopt;
    const auto paths = shapeRenderPaths(shape, size->value, *line, *shapePath);
    auto raster = render::PathRaster::transformed(
        std::span<const render::Path>(paths),
        {strokeEnabled ? width->value : 0, static_cast<render::PathStrokeAlign>(shape.strokeAlign),
         static_cast<render::PathStrokeJoin>(shape.strokeJoin),
         static_cast<render::PathStrokeCap>(shape.strokeCap)},
        matrix, 1.0, 1.0, cancelled);
    if (!raster)
        return std::nullopt;
    const auto extent = raster.value()->bounds(fillEnabled, strokeEnabled);
    const ContentBounds ink{extent.left, extent.top, extent.right, extent.bottom};
    if (ink.empty() || !std::isfinite(ink.left) || !std::isfinite(ink.top) ||
        !std::isfinite(ink.right) || !std::isfinite(ink.bottom))
        return std::nullopt;
    return ink;
}

} // namespace bloom::runtime::detail
