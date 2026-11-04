#pragma once

// #493: the durable value of a Curves curve. Each of the nine curves (color_grade_nodes.hpp) is a
// PathValue used as a point list: an open path whose anchors are the (input, output) points, with
// no handles and the Corner mode. Storing it as a Path reuses the Path value, its persistence, its
// whole-value Path curves for animation (two keys with the same point count interpolate point by
// point; otherwise the earlier key holds) and its request-scoped override, rather than adding a
// second variable-length value kind. The points need not be sorted; the curve sorts them and
// counts a repeated input once.
//
// Domains (every coordinate finite):
//   channel curves     input and output within +-kMaximumGradeValue
//   Luma vs Saturation input within +-kMaximumGradeValue, output (a gain) within [0, 1000]
//   hue curves         input within [0, 1] (1 is the same hue as 0); Hue vs Hue's output (a shift
//                      in turns) within [-0.5, 0.5], a gain within [0, 1000]
// A curve holds at most kMaximumCurvePoints points. A channel curve defaults to the two points
// (0, 0) and (1, 1); the other four default to no points, which changes nothing.

#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/parameter.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace bloom::document {

[[nodiscard]] PathValue defaultGradeCurve(GradeCurve curve);
// A point list holding exactly `points` (input, output) pairs.
[[nodiscard]] PathValue gradeCurvePath(std::span<const Vec2d> points);
[[nodiscard]] inline PathValue gradeCurvePath(const std::initializer_list<Vec2d> points) {
    return gradeCurvePath(std::span<const Vec2d>(points.begin(), points.size()));
}
[[nodiscard]] bool isGradeCurveValueValid(GradeCurve curve, const PathValue& value) noexcept;
// The same check by schema key; false for a key that is not a Curves curve.
[[nodiscard]] bool isGradeCurveValueValid(std::string_view schemaKey,
                                          const PathValue& value) noexcept;
// The output range drawn by the graph and the domain a dragged point is kept inside.
struct GradeCurveRange final {
    double minimum = 0.0;
    double maximum = 1.0;
};
[[nodiscard]] GradeCurveRange gradeCurveOutputDomain(GradeCurve curve) noexcept;
[[nodiscard]] GradeCurveRange gradeCurveInputDomain(GradeCurve curve) noexcept;

// #498: the curve presets, in menu order. Each is a point list over 0..1 in the usual photo and
// grading convention; past the end points a channel curve extends in a straight line, as any
// curve does. A channel curve offers all of them; a hue or Luma vs Saturation curve offers only
// Linear, as Reset, which is its empty default.
enum class GradeCurvePreset : std::uint8_t {
    Linear,
    MediumContrast,
    StrongContrast,
    LowContrast,
    Lighten,
    Darken,
    Fade,
    CrushBlacks,
    Invert,
};
inline constexpr std::array<GradeCurvePreset, 9> kGradeCurvePresets{
    GradeCurvePreset::Linear,         GradeCurvePreset::MediumContrast,
    GradeCurvePreset::StrongContrast, GradeCurvePreset::LowContrast,
    GradeCurvePreset::Lighten,        GradeCurvePreset::Darken,
    GradeCurvePreset::Fade,           GradeCurvePreset::CrushBlacks,
    GradeCurvePreset::Invert};
// `preset` as `curve`'s points; a curve that is not a channel curve takes its default for every
// preset.
[[nodiscard]] PathValue gradeCurvePreset(GradeCurve curve, GradeCurvePreset preset);

} // namespace bloom::document
