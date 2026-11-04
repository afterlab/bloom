#include <bloom/document/grade_curves.hpp>

#include <cmath>

namespace bloom::document {

PathValue defaultGradeCurve(const GradeCurve curve) {
    if (!isChannelGradeCurve(curve))
        return {};
    const Vec2d points[] = {{0.0, 0.0}, {1.0, 1.0}};
    return gradeCurvePath(points);
}

PathValue gradeCurvePath(const std::span<const Vec2d> points) {
    PathValue path;
    path.anchors.reserve(points.size());
    for (const auto point : points)
        path.anchors.push_back({point, std::nullopt, std::nullopt, PathAnchorMode::Corner});
    return path;
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
    if (value.closed || value.anchors.size() > kMaximumCurvePoints)
        return false;
    const auto input = gradeCurveInputDomain(curve);
    const auto output = gradeCurveOutputDomain(curve);
    for (const auto& anchor : value.anchors) {
        const auto [x, y] = anchor.point;
        if (anchor.inHandle || anchor.outHandle || anchor.mode != PathAnchorMode::Corner ||
            !std::isfinite(x) || !std::isfinite(y) || x < input.minimum || x > input.maximum ||
            y < output.minimum || y > output.maximum)
            return false;
    }
    return true;
}

bool isGradeCurveValueValid(const std::string_view schemaKey, const PathValue& value) noexcept {
    const auto curve = gradeCurveFromSchemaKey(schemaKey);
    return curve.has_value() && isGradeCurveValueValid(*curve, value);
}

} // namespace bloom::document
