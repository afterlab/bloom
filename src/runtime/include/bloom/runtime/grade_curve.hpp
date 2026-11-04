#pragma once

// #493: the shape of a Curves curve and the colour model of its hue curves, shared by the CPU
// reference's table preparation (color_grade_image_effect.cpp), the GPU program that reads that
// table, and the curve graph that draws the same curve.
//
// Shape. The points are narrowed to binary32 (the routes compare binary32 knots), sorted by input,
// and a point whose input repeats an earlier one is dropped. The curve is the Fritsch-Carlson
// monotone cubic (PCHIP): it passes through every point, never overshoots between them and is flat
// at a local extremum. A channel curve extrapolates linearly along its end tangents (so HDR keeps
// grading); Luma vs Saturation holds its end values; a hue curve is periodic over [0, 1), its
// tangents and its segment from the last point to the first taken across the wrap. One point is a
// constant. A channel curve with no points, or whose points all lie on y = x, is the identity; a
// hue or Luma vs Saturation curve with no points, or whose outputs are all neutral (no shift, unit
// gain), changes nothing.
//
// Colour model. Every quantity is a plain function of the stored red, green and blue: no colour
// space is read or converted, so a Raw data pass is graded as stored, values above 1 and below 0
// keep their meaning, and alpha is never changed. It is the Hue/Saturation node's model (#340):
//   luma       L = 0.213 R + 0.715 G + 0.072 B, the Filter Effects weights;
//   chroma     c = RGB - L, written c = a e1 + b e2 in the plane Filter Effects hueRotate turns,
//              e1 the chroma of pure red and e2 the direction hueRotate moves it at 90 degrees
//              (its sine matrix applied to red, projected onto the plane);
//   hue        the diamond angle of (a, b) in turns: a/b ratios only, so no trigonometric function
//              is evaluated and the CPU and GPU agree; red is 0, and the hue increases through
//              yellow, green, cyan, blue and magenta. It is monotone in the true angle but not
//              proportional to it, and the graph draws its hue strip through this same mapping;
//   saturation the Filter Effects saturate operation, RGB' = L + g (RGB - L).
// Each curve reads the input pixel: Hue vs Hue turns the chroma to hue + shift(hue), keeping its
// Euclidean length in the (a, b) plane; Hue vs Saturation's and Luma vs Saturation's gains multiply
// into one saturate; Hue vs Luma scales the colour by 1 + (gain - 1) t, where t = |c| / (|c| + |L|)
// (|c| = |a| + |b|) weights the change by how colourful the pixel is. A pixel whose red, green and
// blue are equal has no hue and is never changed by these four curves. Unlike Hue/Saturation's
// hueRotate matrix, the hue curves need a per-pixel angle, which is why they use ratios rather than
// that matrix's cosine and sine.

#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/parameter.hpp>

#include <array>
#include <optional>
#include <vector>

namespace bloom::runtime {

enum class GradeCurveEnds : std::uint8_t { Linear, Hold, Periodic };

struct GradeCurveShape final {
    document::GradeCurve curve = document::GradeCurve::Master;
    GradeCurveEnds ends = GradeCurveEnds::Linear;
    // The distinct sorted binary32 knots and their outputs.
    std::vector<double> x;
    std::vector<double> y;
    // Segment s (0..x.size()) covers the inputs with s knots at or below them and evaluates
    // a + dx (b + dx (c + dx d)) with dx = input - origin[s]. Empty when there are no points.
    std::vector<double> origin;
    std::vector<std::array<double, 4>> segments;
    bool identity = true;
};

[[nodiscard]] GradeCurveEnds gradeCurveEnds(document::GradeCurve curve) noexcept;
// Null when the value is outside its curve's document domain.
[[nodiscard]] std::optional<GradeCurveShape> prepareGradeCurveShape(document::GradeCurve curve,
                                                                    const document::PathValue&);
// The curve's output at `input` (a hue input wraps into [0, 1)); the identity returns the input
// for a channel curve and the neutral output otherwise.
[[nodiscard]] double sampleGradeCurve(const GradeCurveShape& shape, double input) noexcept;

// The hue model's constants: the luma weights, the rows mapping RGB to (a, b), and e1 and e2 as
// columns mapping (a, b) back to RGB.
struct GradeHueModel final {
    std::array<double, 3> luma;
    std::array<std::array<double, 3>, 2> toOpponent;
    std::array<std::array<double, 3>, 2> fromOpponent;
};
[[nodiscard]] const GradeHueModel& gradeHueModel() noexcept;
// The hue in [0, 1) of a colour, or null when it has none (red, green and blue equal).
[[nodiscard]] std::optional<double> gradeHue(double red, double green, double blue) noexcept;
// A displayable colour of `hue` (luma 0.5, the chroma scaled into [0, 1]), for the hue strip.
[[nodiscard]] std::array<double, 3> gradeHueColor(double hue) noexcept;

} // namespace bloom::runtime
