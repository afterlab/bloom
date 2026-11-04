#pragma once

// #493: Curves kernels for the runtime tests -- the document's default curves (the identity) with
// any curve replaced by a point list.

#include <bloom/document/grade_curves.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace bloom::runtime::testing {

[[nodiscard]] inline CurvesKernel identityCurvesKernel(const std::uint64_t idBase,
                                                       const bool clamp = false) {
    CurvesKernel kernel;
    for (std::size_t index = 0; index < document::kGradeCurveCount; ++index)
        kernel.curves[index] = {document::ParameterId::fromRaw(idBase + index),
                                document::defaultGradeCurve(document::kGradeCurves[index])};
    kernel.clamp = clamp;
    return kernel;
}

inline void setCurve(CurvesKernel& kernel, const document::GradeCurve curve,
                     const std::vector<document::Vec2d>& points) {
    kernel.curves[document::gradeCurveIndex(curve)].source = document::gradeCurvePath(points);
}

// The five schema 1 points of a channel (0, 0.25, 0.5, 0.75, 1 on the identity) with some moved.
[[nodiscard]] inline std::vector<document::Vec2d>
legacyPoints(std::initializer_list<std::pair<std::size_t, document::Vec2d>> moved) {
    std::vector<document::Vec2d> points;
    for (std::size_t point = 0; point < 5; ++point) {
        const double x = static_cast<double>(point) / 4.0;
        points.push_back({x, x});
    }
    for (const auto& [index, value] : moved)
        points[index] = value;
    return points;
}

} // namespace bloom::runtime::testing
