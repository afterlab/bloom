#include <bloom/document/grade_curves.hpp>

#include <cmath>
#include <cstdio>

namespace bloom::document {

PathValue defaultGradeCurve(const GradeCurve curve) {
    if (!isChannelGradeCurve(curve))
        return {};
    const Vec2d points[] = {{0.0, 0.0}, {1.0, 1.0}};
    return gradeCurvePath(points);
}

PathValue gradeCurvePath(const std::span<const Vec2d> points) {
    PathValue path;
    auto& contour = path.contours.emplace_back();
    contour.anchors.reserve(points.size());
    for (const auto point : points)
        contour.anchors.push_back({point, std::nullopt, std::nullopt, PathAnchorMode::Corner});
    return path;
}

PathValue gradeCurvePreset(const GradeCurve curve, const GradeCurvePreset preset) {
    if (!isChannelGradeCurve(curve))
        return defaultGradeCurve(curve);
    switch (preset) {
    case GradeCurvePreset::Linear:
        break;
    case GradeCurvePreset::MediumContrast:
        return gradeCurvePath({{0.0, 0.0}, {0.25, 0.18}, {0.75, 0.82}, {1.0, 1.0}});
    case GradeCurvePreset::StrongContrast:
        return gradeCurvePath({{0.0, 0.0}, {0.25, 0.12}, {0.75, 0.88}, {1.0, 1.0}});
    case GradeCurvePreset::LowContrast:
        return gradeCurvePath({{0.0, 0.0}, {0.25, 0.31}, {0.75, 0.69}, {1.0, 1.0}});
    case GradeCurvePreset::Lighten:
        return gradeCurvePath({{0.0, 0.0}, {0.5, 0.62}, {1.0, 1.0}});
    case GradeCurvePreset::Darken:
        return gradeCurvePath({{0.0, 0.0}, {0.5, 0.38}, {1.0, 1.0}});
    case GradeCurvePreset::Fade:
        return gradeCurvePath({{0.0, 0.1}, {0.5, 0.52}, {1.0, 0.92}});
    case GradeCurvePreset::CrushBlacks:
        return gradeCurvePath({{0.0, 0.0}, {0.12, 0.0}, {1.0, 1.0}});
    case GradeCurvePreset::Invert:
        return gradeCurvePath({{0.0, 1.0}, {1.0, 0.0}});
    }
    return defaultGradeCurve(curve);
}

GradeCurveRange gradeCurveInputDomain(const GradeCurve curve) noexcept {
    if (isHueGradeCurve(curve))
        return {0.0, 1.0};
    return {-kMaximumGradeValue, kMaximumGradeValue};
}

GradeCurveRange gradeCurveOutputDomain(const GradeCurve curve) noexcept {
    if (isChannelGradeCurve(curve))
        return {-kMaximumGradeValue, kMaximumGradeValue};
    if (curve == GradeCurve::HueVsHue)
        return {-kMaximumHueShift, kMaximumHueShift};
    return {0.0, kMaximumCurveGain};
}

bool isGradeCurveValueValid(const GradeCurve curve, const PathValue& value) noexcept {
    // A point list is one open contour: never compound, and no fill rule or group means anything.
    const auto& points = value.firstContour();
    if (!value.isSimple() || points.closed || points.anchors.size() > kMaximumCurvePoints)
        return false;
    const auto input = gradeCurveInputDomain(curve);
    const auto output = gradeCurveOutputDomain(curve);
    for (const auto& anchor : points.anchors) {
        const auto [x, y] = anchor.point;
        if (anchor.inHandle || anchor.outHandle || anchor.mode != PathAnchorMode::Corner ||
            !std::isfinite(x) || !std::isfinite(y) || x < input.minimum || x > input.maximum ||
            y < output.minimum || y > output.maximum)
            return false;
    }
    return true;
}

PathValue normalizedGradeCurveValue(PathValue value) {
    for (auto& contour : value.contours)
        for (auto& anchor : contour.anchors)
            if (anchor.mode == PathAnchorMode::Auto && !anchor.inHandle && !anchor.outHandle)
                anchor.mode = PathAnchorMode::Corner;
    return value;
}

namespace {

std::string number(const double value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.6g", value);
    return text;
}

} // namespace

std::string gradeCurveValueProblem(const GradeCurve curve, const PathValue& value) {
    const std::string shape =
        "a curve is one open contour {closed:false, anchors:[{point:[input, output]}]} of at most " +
        std::to_string(kMaximumCurvePoints) +
        " points with no handles (an auto or corner anchor with null handles is a plain point)";
    if (!value.isSimple())
        return "this curve takes one contour with no fill rule or groups; " + shape +
               ", but it received " + std::to_string(value.contours.size()) + " contours" +
               (value.groups.empty() ? "" : " and groups");
    const auto& points = value.firstContour();
    if (points.closed)
        return "this curve is open; " + shape + ", but it received closed:true";
    if (points.anchors.size() > kMaximumCurvePoints)
        return "this curve holds at most " + std::to_string(kMaximumCurvePoints) +
               " points, but it received " + std::to_string(points.anchors.size());
    const auto input = gradeCurveInputDomain(curve);
    const auto output = gradeCurveOutputDomain(curve);
    for (std::size_t index = 0; index < points.anchors.size(); ++index) {
        const auto& anchor = points.anchors[index];
        const auto where = "anchor " + std::to_string(index);
        if (anchor.inHandle || anchor.outHandle)
            return where + " has a handle, but a curve point takes none; " + shape;
        if (anchor.mode != PathAnchorMode::Corner && anchor.mode != PathAnchorMode::Auto)
            return where + " is in " + std::string(pathAnchorModeName(anchor.mode)) +
                   " mode, which needs handles a curve point does not take; " + shape;
        const auto [x, y] = anchor.point;
        if (!std::isfinite(x) || !std::isfinite(y))
            return where + " has a point that is not finite; a point is two finite numbers";
        if (x < input.minimum || x > input.maximum)
            return where + " input " + number(x) + " is outside " + number(input.minimum) + ".." +
                   number(input.maximum);
        if (y < output.minimum || y > output.maximum)
            return where + " output " + number(y) + " is outside " + number(output.minimum) + ".." +
                   number(output.maximum);
    }
    return {};
}

std::string gradeCurveValueProblem(const std::string_view schemaKey, const PathValue& value) {
    const auto curve = gradeCurveFromSchemaKey(schemaKey);
    return curve.has_value() ? gradeCurveValueProblem(*curve, value)
                             : "this parameter is not a curve";
}

bool isGradeCurveValueValid(const std::string_view schemaKey, const PathValue& value) noexcept {
    const auto curve = gradeCurveFromSchemaKey(schemaKey);
    return curve.has_value() && isGradeCurveValueValid(*curve, value);
}

} // namespace bloom::document
