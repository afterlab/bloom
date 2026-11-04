#include <bloom/runtime/grade_curve.hpp>

#include <bloom/document/grade_curves.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace bloom::runtime {
namespace {

using document::GradeCurve;

// Fritsch-Carlson monotone cubic (PCHIP) end tangent, as SciPy's PchipInterpolator computes it.
[[nodiscard]] double pchipEnd(const double h0, const double h1, const double d0,
                              const double d1) noexcept {
    double m = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
    const auto sign = [](const double x) { return (x > 0.0) - (x < 0.0); };
    if (sign(m) != sign(d0))
        m = 0.0;
    else if (sign(d0) != sign(d1) && std::abs(m) > 3.0 * std::abs(d0))
        m = 3.0 * d0;
    return m;
}

// The interior PCHIP tangent between a previous interval (length h0, slope d0) and the next one.
[[nodiscard]] double pchipInterior(const double h0, const double h1, const double d0,
                                   const double d1) noexcept {
    if (d0 == 0.0 || d1 == 0.0 || (d0 > 0.0) != (d1 > 0.0))
        return 0.0;
    const double w1 = 2.0 * h1 + h0;
    const double w2 = h1 + 2.0 * h0;
    return (w1 + w2) / (w1 / d0 + w2 / d1);
}

[[nodiscard]] std::array<double, 4> hermite(const double h, const double y0, const double y1,
                                            const double m0, const double m1) noexcept {
    const double delta = (y1 - y0) / h;
    return {y0, m0, (3.0 * delta - 2.0 * m0 - m1) / h, (m0 + m1 - 2.0 * delta) / (h * h)};
}

[[nodiscard]] GradeHueModel makeHueModel() noexcept {
    GradeHueModel model{};
    model.luma = {0.213, 0.715, 0.072};
    const auto& w = model.luma;
    // e1: pure red's chroma. e2: the Filter Effects hueRotate sine matrix applied to red (its
    // first column), projected onto the chroma plane w . c = 0 (the weights sum to one).
    const std::array<double, 3> e1{1.0 - w[0], -w[0], -w[0]};
    std::array<double, 3> e2{-0.213, 0.143, -0.787};
    const double offPlane = w[0] * e2[0] + w[1] * e2[1] + w[2] * e2[2];
    for (auto& value : e2)
        value -= offPlane;
    const auto dot = [](const std::array<double, 3>& p, const std::array<double, 3>& q) {
        return p[0] * q[0] + p[1] * q[1] + p[2] * q[2];
    };
    const double g11 = dot(e1, e1);
    const double g12 = dot(e1, e2);
    const double g22 = dot(e2, e2);
    const double determinant = g11 * g22 - g12 * g12;
    // (a, b) = G^-1 [e1; e2] c with c = (I - 1 w^T) RGB, folded into one row per coordinate.
    std::array<std::array<double, 3>, 2> rows{};
    for (std::size_t j = 0; j < 3; ++j) {
        rows[0][j] = (g22 * e1[j] - g12 * e2[j]) / determinant;
        rows[1][j] = (g11 * e2[j] - g12 * e1[j]) / determinant;
    }
    for (std::size_t r = 0; r < 2; ++r) {
        const double sum = rows[r][0] + rows[r][1] + rows[r][2];
        for (std::size_t j = 0; j < 3; ++j)
            model.toOpponent[r][j] = rows[r][j] - sum * w[j];
    }
    model.fromOpponent = {e1, e2};
    return model;
}

// The direction (a, b) of `hue` on the unit diamond |a| + |b| = 1.
[[nodiscard]] std::array<double, 2> hueDirection(double hue) noexcept {
    hue -= std::floor(hue);
    const double p = hue * 4.0;
    const double quadrant = std::floor(p);
    const double f = p - quadrant;
    switch (static_cast<int>(quadrant)) {
    case 0:
        return {1.0 - f, f};
    case 1:
        return {-f, 1.0 - f};
    case 2:
        return {f - 1.0, -f};
    default:
        return {f, f - 1.0};
    }
}

} // namespace

GradeCurveEnds gradeCurveEnds(const GradeCurve curve) noexcept {
    if (document::isHueGradeCurve(curve))
        return GradeCurveEnds::Periodic;
    return curve == GradeCurve::LumaVsSaturation ? GradeCurveEnds::Hold : GradeCurveEnds::Linear;
}

std::optional<GradeCurveShape> prepareGradeCurveShape(const GradeCurve curve,
                                                      const document::PathValue& value) {
    if (!document::isGradeCurveValueValid(curve, value))
        return std::nullopt;
    GradeCurveShape shape;
    shape.curve = curve;
    shape.ends = gradeCurveEnds(curve);
    const bool periodic = shape.ends == GradeCurveEnds::Periodic;
    std::vector<std::pair<float, float>> points;
    points.reserve(value.anchors.size());
    for (const auto& anchor : value.anchors) {
        auto x = static_cast<float>(anchor.point.x);
        // A hue of one is the hue of zero.
        if (periodic && x >= 1.0F)
            x = 0.0F;
        points.emplace_back(x, static_cast<float>(anchor.point.y));
    }
    std::ranges::stable_sort(points, {}, &std::pair<float, float>::first);
    for (const auto& [px, py] : points) {
        if (!shape.x.empty() && static_cast<float>(shape.x.back()) == px)
            continue;
        shape.x.push_back(static_cast<double>(px));
        shape.y.push_back(static_cast<double>(py));
    }
    const auto& x = shape.x;
    const auto& y = shape.y;
    const auto n = x.size();
    if (document::isChannelGradeCurve(curve)) {
        shape.identity = n != 1;
        for (std::size_t k = 0; k < n; ++k)
            shape.identity = shape.identity && x[k] == y[k];
    } else {
        const double neutral = document::neutralGradeCurveOutput(curve);
        shape.identity = std::ranges::all_of(y, [neutral](const double v) { return v == neutral; });
    }
    if (n == 0)
        return shape;
    std::vector<double> m(n, 0.0);
    if (periodic && n >= 2) {
        for (std::size_t k = 0; k < n; ++k) {
            const auto previous = (k + n - 1) % n;
            const auto next = (k + 1) % n;
            const double h0 = x[k] - x[previous] + (k == 0 ? 1.0 : 0.0);
            const double h1 = x[next] - x[k] + (k + 1 == n ? 1.0 : 0.0);
            m[k] = pchipInterior(h0, h1, (y[k] - y[previous]) / h0, (y[next] - y[k]) / h1);
        }
    } else if (n == 2) {
        m[0] = m[1] = (y[1] - y[0]) / (x[1] - x[0]);
    } else if (n > 2) {
        std::vector<double> h(n - 1);
        std::vector<double> delta(n - 1);
        for (std::size_t k = 0; k + 1 < n; ++k) {
            h[k] = x[k + 1] - x[k];
            delta[k] = (y[k + 1] - y[k]) / h[k];
        }
        for (std::size_t k = 1; k + 1 < n; ++k)
            m[k] = pchipInterior(h[k - 1], h[k], delta[k - 1], delta[k]);
        m[0] = pchipEnd(h[0], h[1], delta[0], delta[1]);
        m[n - 1] = pchipEnd(h[n - 2], h[n - 3], delta[n - 2], delta[n - 3]);
    }
    shape.origin.resize(n + 1);
    shape.segments.resize(n + 1);
    for (std::size_t k = 0; k + 1 < n; ++k) {
        shape.origin[k + 1] = x[k];
        shape.segments[k + 1] = hermite(x[k + 1] - x[k], y[k], y[k + 1], m[k], m[k + 1]);
    }
    if (periodic) {
        // Below the first knot and above the last: the one segment across the wrap.
        const auto wrap =
            n == 1 ? std::array<double, 4>{y[0], 0.0, 0.0, 0.0}
                   : hermite(x[0] + 1.0 - x[n - 1], y[n - 1], y[0], m[n - 1], m[0]);
        shape.origin[0] = x[n - 1] - 1.0;
        shape.segments[0] = wrap;
        shape.origin[n] = x[n - 1];
        shape.segments[n] = wrap;
    } else {
        const bool hold = shape.ends == GradeCurveEnds::Hold;
        shape.origin[0] = x[0];
        shape.segments[0] = {y[0], hold ? 0.0 : m[0], 0.0, 0.0};
        shape.origin[n] = x[n - 1];
        shape.segments[n] = {y[n - 1], hold ? 0.0 : m[n - 1], 0.0, 0.0};
    }
    return shape;
}

double sampleGradeCurve(const GradeCurveShape& shape, double input) noexcept {
    if (shape.identity || shape.segments.empty())
        return document::isChannelGradeCurve(shape.curve)
                   ? input
                   : document::neutralGradeCurveOutput(shape.curve);
    if (shape.ends == GradeCurveEnds::Periodic)
        input -= std::floor(input);
    const auto segment = static_cast<std::size_t>(
        std::ranges::upper_bound(shape.x, input) - shape.x.begin());
    const double dx = input - shape.origin[segment];
    const auto& q = shape.segments[segment];
    return q[0] + dx * (q[1] + dx * (q[2] + dx * q[3]));
}

const GradeHueModel& gradeHueModel() noexcept {
    static const GradeHueModel model = makeHueModel();
    return model;
}

std::optional<double> gradeHue(const double red, const double green, const double blue) noexcept {
    if (red == green && green == blue)
        return std::nullopt;
    const auto& rows = gradeHueModel().toOpponent;
    const double a = rows[0][0] * red + rows[0][1] * green + rows[0][2] * blue;
    const double b = rows[1][0] * red + rows[1][1] * green + rows[1][2] * blue;
    const double sum = std::abs(a) + std::abs(b);
    if (!(sum > 0.0))
        return std::nullopt;
    double p = 0.0;
    if (a >= 0.0 && b >= 0.0)
        p = b / sum;
    else if (b >= 0.0)
        p = 1.0 + -a / sum;
    else if (a < 0.0)
        p = 2.0 + -b / sum;
    else
        p = 3.0 + a / sum;
    const double hue = p * 0.25;
    return hue >= 1.0 ? 0.0 : hue;
}

std::array<double, 3> gradeHueColor(const double hue) noexcept {
    const auto direction = hueDirection(hue);
    const auto& columns = gradeHueModel().fromOpponent;
    std::array<double, 3> chroma{};
    double largest = 0.0;
    for (std::size_t j = 0; j < 3; ++j) {
        chroma[j] = direction[0] * columns[0][j] + direction[1] * columns[1][j];
        largest = std::max(largest, std::abs(chroma[j]));
    }
    const double scale = largest > 0.0 ? 0.5 / largest : 0.0;
    return {0.5 + chroma[0] * scale, 0.5 + chroma[1] * scale, 0.5 + chroma[2] * scale};
}

} // namespace bloom::runtime
