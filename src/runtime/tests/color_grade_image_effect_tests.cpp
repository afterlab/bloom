// #340 Levels, Curves and Hue/Saturation (#493: Curves with any number of points and the four hue
// curves): the CPU reference against independent double-precision
// formulas over HDR, negative and partial-alpha pixels; a Raw depth pass normalised with no colour
// transform; clamp, step, identity and domain rules; windows, budget, cancellation; and the
// resident programs the GPU route prepares.

#include "grade_curves_test_support.hpp"
#include "image_effect.hpp"
#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/runtime/grade_curve.hpp>
#include <bloom/runtime/gpu_ocio_wrapper.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace bloom;
using runtime::detail::ColorGradeKind;
void require(const bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
template <typename Actual, typename Expected>
bool near(const Actual actualValue, const Expected expectedValue, const double tolerance = 2e-6) {
    const auto actual = static_cast<double>(actualValue);
    const auto expected = static_cast<double>(expectedValue);
    return std::abs(actual - expected) <= tolerance * std::max(1.0, std::abs(expected));
}

struct Levels final {
    double inBlack = 0.0;
    double inWhite = 1.0;
    double gamma = 1.0;
    double outBlack = 0.0;
    double outWhite = 1.0;
};
runtime::LevelsKernel levelsKernel(const bool clamp) {
    runtime::LevelsKernel kernel;
    std::uint64_t id = 1;
    for (auto& channel : kernel.channels)
        for (auto* field : {&channel.inputBlack, &channel.inputWhite, &channel.gamma,
                            &channel.outputBlack, &channel.outputWhite})
            *field = {document::ParameterId::fromRaw(id++), 0.0};
    kernel.clamp = clamp;
    return kernel;
}
std::vector<double> levelsValues(const std::array<Levels, 5>& channels) {
    std::vector<double> values;
    for (const auto& c : channels)
        for (const double v : {c.inBlack, c.inWhite, c.gamma, c.outBlack, c.outWhite})
            values.push_back(v);
    return values;
}
// Independent reference: Levels on the straight value, gamma only for a positive normalised value.
double levelsReference(const Levels& c, const double x) {
    double t = c.inWhite == c.inBlack ? (x >= c.inBlack ? 1.0 : 0.0)
                                      : (x - c.inBlack) / (c.inWhite - c.inBlack);
    if (t > 0.0)
        t = std::pow(t, 1.0 / c.gamma);
    return c.outBlack + t * (c.outWhite - c.outBlack);
}

using Points = std::vector<std::array<double, 2>>;
const Points kIdentityPoints{{0.0, 0.0}, {0.25, 0.25}, {0.5, 0.5}, {0.75, 0.75}, {1.0, 1.0}};
runtime::CurvesKernel curvesKernel(const bool clamp) {
    return runtime::testing::identityCurvesKernel(100, clamp);
}
// Every curve in kGradeCurves order: the five channel curves, then the four hue curves (empty
// unless given).
using CurveSet = std::array<Points, document::kGradeCurveCount>;
CurveSet channelCurves(const std::array<Points, 5>& channels) {
    CurveSet curves{};
    std::copy(channels.begin(), channels.end(), curves.begin());
    return curves;
}
std::vector<double> curvesValues(const CurveSet& curves) {
    std::vector<double> values;
    for (const auto& points : curves) {
        values.push_back(static_cast<double>(points.size()));
        for (const auto& [x, y] : points) {
            values.push_back(x);
            values.push_back(y);
        }
    }
    return values;
}
std::vector<double> curvesValues(const std::array<Points, 5>& channels) {
    return curvesValues(channelCurves(channels));
}
enum class Ends { Linear, Hold, Periodic };
// Independent reference: the Fritsch-Carlson monotone cubic in the Hermite basis from points
// (narrowed to binary32 as the document stores them) sorted by input with repeated inputs dropped.
// Linear ends extrapolate along the end tangents, Hold ends keep the end values, and a periodic
// curve over 0..1 takes its tangents and its segment across the wrap from its neighbours mod 1.
double curvesReference(Points points, const double value, const Ends ends = Ends::Linear) {
    for (auto& p : points) {
        p = {static_cast<double>(static_cast<float>(p[0])),
             static_cast<double>(static_cast<float>(p[1]))};
        if (ends == Ends::Periodic && p[0] >= 1.0)
            p[0] = 0.0;
    }
    std::stable_sort(points.begin(), points.end(),
                     [](const auto& a, const auto& b) { return a[0] < b[0]; });
    std::vector<double> x;
    std::vector<double> y;
    for (const auto& p : points)
        if (x.empty() || p[0] != x.back()) {
            x.push_back(p[0]);
            y.push_back(p[1]);
        }
    const std::size_t n = x.size();
    if (n == 1)
        return y[0];
    const auto interior = [](const double h0, const double h1, const double d0, const double d1) {
        return d0 * d1 <= 0.0 ? 0.0
                              : (3.0 * (h0 + h1)) / ((2.0 * h1 + h0) / d0 + (h1 + 2.0 * h0) / d1);
    };
    const auto hermite = [](const double x0, const double h, const double y0, const double y1,
                            const double m0, const double m1, const double at) {
        const double t = (at - x0) / h;
        const double t2 = t * t;
        const double t3 = t2 * t;
        return (2 * t3 - 3 * t2 + 1) * y0 + (t3 - 2 * t2 + t) * h * m0 + (-2 * t3 + 3 * t2) * y1 +
               (t3 - t2) * h * m1;
    };
    if (ends == Ends::Periodic) {
        const double at = value - std::floor(value);
        std::vector<double> m(n);
        for (std::size_t k = 0; k < n; ++k) {
            const auto p = (k + n - 1) % n;
            const auto q = (k + 1) % n;
            const double h0 = k == 0 ? x[0] + 1.0 - x[n - 1] : x[k] - x[p];
            const double h1 = k + 1 == n ? x[0] + 1.0 - x[k] : x[q] - x[k];
            m[k] = interior(h0, h1, (y[k] - y[p]) / h0, (y[q] - y[k]) / h1);
        }
        const double wrap = x[0] + 1.0 - x[n - 1];
        if (at < x[0])
            return hermite(x[n - 1] - 1.0, wrap, y[n - 1], y[0], m[n - 1], m[0], at);
        if (at >= x[n - 1])
            return hermite(x[n - 1], wrap, y[n - 1], y[0], m[n - 1], m[0], at);
        std::size_t k = 0;
        while (at >= x[k + 1])
            ++k;
        return hermite(x[k], x[k + 1] - x[k], y[k], y[k + 1], m[k], m[k + 1], at);
    }
    std::vector<double> h(n - 1);
    std::vector<double> d(n - 1);
    for (std::size_t k = 0; k + 1 < n; ++k) {
        h[k] = x[k + 1] - x[k];
        d[k] = (y[k + 1] - y[k]) / h[k];
    }
    std::vector<double> m(n, d[0]);
    if (n > 2) {
        for (std::size_t k = 1; k + 1 < n; ++k)
            m[k] = interior(h[k - 1], h[k], d[k - 1], d[k]);
        const auto sign = [](const double v) { return v > 0.0 ? 1 : v < 0.0 ? -1 : 0; };
        const auto end = [&](const double h0, const double h1, const double d0, const double d1) {
            const double t = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
            if (sign(t) != sign(d0))
                return 0.0;
            if (sign(d0) != sign(d1) && std::abs(t) > 3.0 * std::abs(d0))
                return 3.0 * d0;
            return t;
        };
        m[0] = end(h[0], h[1], d[0], d[1]);
        m[n - 1] = end(h[n - 2], h[n - 3], d[n - 2], d[n - 3]);
    }
    const double slope0 = ends == Ends::Hold ? 0.0 : m[0];
    const double slope1 = ends == Ends::Hold ? 0.0 : m[n - 1];
    if (value < x[0])
        return y[0] + slope0 * (value - x[0]);
    if (value >= x[n - 1])
        return y[n - 1] + slope1 * (value - x[n - 1]);
    std::size_t k = 0;
    while (value >= x[k + 1])
        ++k;
    return hermite(x[k], h[k], y[k], y[k + 1], m[k], m[k + 1], value);
}

// Independent double reference of the hue colour model: Filter Effects luma; the chroma plane
// spanned by red's chroma e1 and the hueRotate sine column e2 (projected onto the plane), with
// (a, b) the least-squares coordinates of a colour's chroma in that basis; hue the diamond angle
// of (a, b), 0 at red, a quarter turn at +e2.
struct HueReference final {
    std::array<double, 3> w{0.213, 0.715, 0.072};
    std::array<double, 3> e1{};
    std::array<double, 3> e2{};
    HueReference() {
        e1 = {1.0 - w[0], -w[0], -w[0]};
        e2 = {-0.213, 0.143, -0.787};
        const double off = w[0] * e2[0] + w[1] * e2[1] + w[2] * e2[2];
        for (auto& v : e2)
            v -= off;
    }
    [[nodiscard]] double luma(const std::array<double, 3>& c) const {
        return w[0] * c[0] + w[1] * c[1] + w[2] * c[2];
    }
    [[nodiscard]] std::array<double, 2> opponent(const std::array<double, 3>& c) const {
        const double l = luma(c);
        const std::array<double, 3> chroma{c[0] - l, c[1] - l, c[2] - l};
        const auto dot = [](const std::array<double, 3>& p, const std::array<double, 3>& q) {
            return p[0] * q[0] + p[1] * q[1] + p[2] * q[2];
        };
        const double g11 = dot(e1, e1);
        const double g12 = dot(e1, e2);
        const double g22 = dot(e2, e2);
        const double r1 = dot(e1, chroma);
        const double r2 = dot(e2, chroma);
        const double det = g11 * g22 - g12 * g12;
        return {(g22 * r1 - g12 * r2) / det, (g11 * r2 - g12 * r1) / det};
    }
    [[nodiscard]] static double hue(const std::array<double, 2>& ab) {
        const double s = std::abs(ab[0]) + std::abs(ab[1]);
        double q = 0.0;
        if (ab[0] >= 0.0 && ab[1] >= 0.0)
            q = ab[1] / s;
        else if (ab[1] >= 0.0)
            q = 1.0 - ab[0] / s;
        else if (ab[0] < 0.0)
            q = 2.0 - ab[1] / s;
        else
            q = 3.0 + ab[0] / s;
        return q / 4.0 >= 1.0 ? 0.0 : q / 4.0;
    }
    [[nodiscard]] static std::array<double, 2> direction(double hue) {
        hue -= std::floor(hue);
        const double p = hue * 4.0;
        const double f = p - std::floor(p);
        switch (static_cast<int>(std::floor(p))) {
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
    // The four hue curves on straight `c`, in the documented order.
    [[nodiscard]] std::array<double, 3> apply(const CurveSet& curves,
                                              std::array<double, 3> c) const {
        if (c[0] == c[1] && c[1] == c[2])
            return c;
        const auto at = [&](const document::GradeCurve curve, const double x, const Ends ends) {
            const auto& points = curves[document::gradeCurveIndex(curve)];
            const double neutral = document::neutralGradeCurveOutput(curve);
            if (points.empty() || std::ranges::all_of(points, [&](const auto& p) {
                    return p[1] == neutral;
                }))
                return neutral;
            return curvesReference(points, x, ends);
        };
        const auto ab = opponent(c);
        const double l = luma(c);
        const double h = hue(ab);
        const double shift = at(document::GradeCurve::HueVsHue, h, Ends::Periodic);
        const double hueSat = at(document::GradeCurve::HueVsSaturation, h, Ends::Periodic);
        const double hueLuma = at(document::GradeCurve::HueVsLuma, h, Ends::Periodic);
        const double lumaSat = at(document::GradeCurve::LumaVsSaturation, l, Ends::Hold);
        if (shift != 0.0) {
            const auto d = direction(h + shift);
            const double k = std::hypot(ab[0], ab[1]) / std::hypot(d[0], d[1]);
            const double da = k * d[0] - ab[0];
            const double db = k * d[1] - ab[1];
            for (std::size_t j = 0; j < 3; ++j)
                c[j] += e1[j] * da + e2[j] * db;
        }
        const double gain = hueSat * lumaSat;
        const double current = luma(c);
        for (auto& v : c)
            v = current + gain * (v - current);
        const double s = std::abs(ab[0]) + std::abs(ab[1]);
        const double factor = 1.0 + (hueLuma - 1.0) * s / (s + std::abs(l));
        for (auto& v : c)
            v *= factor;
        return c;
    }
};

// Straight RGBA authoring values premultiplied into the resident representation: SDR, HDR,
// negative, partial and zero alpha.
constexpr std::array<std::array<float, 4>, 12> kPixels{{{0.0F, 0.0F, 0.0F, 1.0F},
                                                        {1.0F, 1.0F, 1.0F, 1.0F},
                                                        {0.5F, 0.5F, 0.5F, 1.0F},
                                                        {0.25F, 0.75F, 0.1F, 0.5F},
                                                        {4.0F, 3.0F, 6.0F, 1.0F},
                                                        {-0.5F, 0.2F, -1.0F, 1.0F},
                                                        {0.8F, 0.1F, 0.9F, 0.25F},
                                                        {0.0F, 0.0F, 0.0F, 0.0F},
                                                        {2.0F, 2.0F, 2.0F, 0.125F},
                                                        {0.3F, 0.6F, 0.2F, 0.75F},
                                                        {0.6F, 0.4F, 0.5F, 1.0F},
                                                        {0.1F, 0.1F, 0.1F, 0.0625F}}};
render::Rgba32fImage image(const std::array<std::array<float, 4>, 12>& straight) {
    const auto data = render::ImageWindow::create(-2, 3, 4, 3);
    const auto display = render::ImageWindow::create(0, 0, 8, 8);
    auto descriptor = render::Rgba32fImageDescriptor::create(*data.value(), *display.value(),
                                                             core::PixelAspectRatio::square());
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), 4096);
    for (std::size_t i = 0; i < straight.size(); ++i) {
        const auto& s = straight[i];
        const auto p =
            render::Rgba32f::fromPremultiplied(s[0] * s[3], s[1] * s[3], s[2] * s[3], s[3]);
        require(p.hasValue() &&
                    !builder.value()->write(-2 + static_cast<std::int64_t>(i % 4),
                                            3 + static_cast<std::int64_t>(i / 4), *p.value()),
                "grade input writes");
    }
    auto frozen = std::move(*builder.value()).freeze();
    require(frozen.hasValue(), "grade input freezes");
    return std::move(*frozen.value());
}

// Applies the grade and checks every pixel against `expected(straight rgb, alpha) -> premultiplied
// rgba`, plus the preserved windows and the budget refusal.
template <typename Expected>
void checkImage(const runtime::detail::PreparedColorGrade& grade, const render::Rgba32fImage& input,
                Expected&& expected, const char* message) {
    const auto result = runtime::detail::applyColorGrade(grade, input, 65536, {},
                                                         runtime::OperationIndex::fromRaw(0), {});
    require(result.image && !result.diagnostic, "grade publishes");
    require(result.image->descriptor()->dataWindow() == input.descriptor()->dataWindow() &&
                result.image->descriptor()->displayWindow() == input.descriptor()->displayWindow(),
            "grade keeps the input data and display windows");
    const auto window = input.descriptor()->dataWindow();
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y)
        for (auto x = window.originX(); x < window.maxXExclusive(); ++x) {
            const auto sourceSample = input.read(x, y);
            const auto actualSample = result.image->read(x, y);
            require(sourceSample.hasValue() && actualSample.hasValue(), "grade samples");
            const auto s = *sourceSample.value();
            const auto actual = *actualSample.value();
            const double a = static_cast<double>(s.alpha());
            const std::array<double, 3> straight{
                a == 0.0 ? 0.0 : static_cast<double>(s.red()) / a,
                a == 0.0 ? 0.0 : static_cast<double>(s.green()) / a,
                a == 0.0 ? 0.0 : static_cast<double>(s.blue()) / a};
            const std::array<double, 4> want = expected(straight, a, s);
            require(near(actual.red(), want[0]) && near(actual.green(), want[1]) &&
                        near(actual.blue(), want[2]) && near(actual.alpha(), want[3]),
                    message);
        }
    require(!runtime::detail::applyColorGrade(grade, input, 1, {},
                                              runtime::OperationIndex::fromRaw(0), {})
                 .image,
            "grade budget refuses before publication");
}

void levelsTests() {
    const auto input = image(kPixels);
    // Per-channel Levels with HDR/negative ranges, gamma on the master and each colour channel,
    // and an alpha transfer that lifts coverage.
    const std::array<Levels, 5> channels{
        Levels{-0.25, 2.0, 1.8, 0.0, 1.0}, Levels{0.0, 1.0, 0.5, 0.1, 3.0},
        Levels{0.2, 0.8, 1.0, -1.0, 1.0}, Levels{1.0, 0.0, 2.2, 0.0, 1.0},
        Levels{0.0, 1.0, 1.0, 0.2, 0.9}};
    for (const bool clamp : {false, true}) {
        const auto grade =
            runtime::detail::prepareColorGrade(levelsKernel(clamp), levelsValues(channels));
        require(grade && grade->kind == ColorGradeKind::Levels && !grade->identity,
                "Levels prepares");
        checkImage(
            *grade, input,
            [&](const std::array<double, 3>& s, const double a, const render::Rgba32f&) {
                const double alpha = std::clamp(levelsReference(channels[4], a), 0.0, 1.0);
                std::array<double, 4> out{};
                for (std::size_t i = 0; i < 3; ++i) {
                    double v = levelsReference(channels[i + 1], levelsReference(channels[0], s[i]));
                    if (clamp)
                        v = std::clamp(v, 0.0, 1.0);
                    out[i] = v * alpha;
                }
                out[3] = alpha;
                return out;
            },
            "Levels equals the independent per-channel reference");
    }
    // A Raw depth pass (alpha one, values far above one): input 10..1000 normalises to 0..1 with
    // no colour transform, and out-of-range depths extrapolate unless clamped.
    constexpr std::array<std::array<float, 4>, 12> depth{{{10.0F, 10.0F, 10.0F, 1.0F},
                                                          {1000.0F, 1000.0F, 1000.0F, 1.0F},
                                                          {505.0F, 505.0F, 505.0F, 1.0F},
                                                          {2000.0F, 2000.0F, 2000.0F, 1.0F},
                                                          {0.0F, 0.0F, 0.0F, 1.0F},
                                                          {257.5F, 257.5F, 257.5F, 1.0F},
                                                          {10.0F, 10.0F, 10.0F, 1.0F},
                                                          {10.0F, 10.0F, 10.0F, 1.0F},
                                                          {10.0F, 10.0F, 10.0F, 1.0F},
                                                          {10.0F, 10.0F, 10.0F, 1.0F},
                                                          {10.0F, 10.0F, 10.0F, 1.0F},
                                                          {10.0F, 10.0F, 10.0F, 1.0F}}};
    std::array<Levels, 5> normalise{};
    normalise[0] = Levels{10.0, 1000.0, 1.0, 0.0, 1.0};
    const auto depthImage = image(depth);
    for (const bool clamp : {false, true}) {
        const auto grade =
            runtime::detail::prepareColorGrade(levelsKernel(clamp), levelsValues(normalise));
        const auto result = runtime::detail::applyColorGrade(
            *grade, depthImage, 65536, {}, runtime::OperationIndex::fromRaw(0), {});
        require(result.image != nullptr, "depth normalisation publishes");
        const auto sample = [&](const std::int64_t x, const std::int64_t y) {
            const auto pixel = result.image->read(x, y);
            require(pixel.hasValue(), "depth sample");
            return static_cast<double>(pixel.value()->red());
        };
        require(sample(-2, 3) == 0.0 && near(sample(-1, 3), 1.0) && near(sample(0, 3), 0.5) &&
                    near(sample(-1, 4), 0.25),
                "a Raw depth pass normalises from its near/far range");
        require(clamp ? sample(1, 3) == 1.0 && sample(-2, 4) == 0.0
                      : near(sample(1, 3), 1990.0 / 990.0) && near(sample(-2, 4), -10.0 / 990.0),
                "out-of-range depth extrapolates unless clamped");
    }
    // Identity: the defaults alias the input; clamp on is never an identity.
    std::array<Levels, 5> defaults{};
    require(
        runtime::detail::prepareColorGrade(levelsKernel(false), levelsValues(defaults))->identity,
        "default Levels is an identity");
    require(
        !runtime::detail::prepareColorGrade(levelsKernel(true), levelsValues(defaults))->identity,
        "clamped default Levels is not an identity");
    // An empty input range is a hard step at input black.
    std::array<Levels, 5> step{};
    step[0] = Levels{0.5, 0.5, 1.0, 0.0, 1.0};
    const auto stepGrade =
        runtime::detail::prepareColorGrade(levelsKernel(false), levelsValues(step));
    std::array<float, 4> below{0.25F, 0.5F, 0.75F, 1.0F};
    require(runtime::detail::colorGradePixel(*stepGrade, below) && below[0] == 0.0F &&
                below[1] == 1.0F && below[2] == 1.0F && below[3] == 1.0F,
            "an empty input range is a step at input black");
    // Domain refusals.
    auto bad = levelsValues(defaults);
    bad[2] = 0.0;
    require(!runtime::detail::prepareColorGrade(levelsKernel(false), bad), "gamma 0 refuses");
    bad[2] = 1.0;
    bad[0] = std::numeric_limits<double>::quiet_NaN();
    require(!runtime::detail::prepareColorGrade(levelsKernel(false), bad), "NaN refuses");
    bad[0] = 2.0e6;
    require(!runtime::detail::prepareColorGrade(levelsKernel(false), bad),
            "an out-of-domain value refuses");
    require(!runtime::detail::prepareColorGrade(levelsKernel(false), std::vector<double>(3, 0.0)),
            "a short operand list refuses");
}

void curvesTests() {
    const auto input = image(kPixels);
    // A seven-point S-curve master, a lifted and inverted red, an unsorted three-point green with
    // a repeated input, a non-monotone blue and an alpha curve that lifts coverage.
    const std::array<Points, 5> channels{
        Points{{0.0, 0.0},
               {0.1, 0.05},
               {0.25, 0.15},
               {0.5, 0.5},
               {0.75, 0.85},
               {0.9, 0.96},
               {1.0, 1.0}},
        Points{{0.0, 1.0}, {0.25, 0.8}, {0.5, 0.5}, {0.75, 0.3}, {1.0, 0.1}},
        Points{{1.0, 1.2}, {0.0, 0.1}, {0.5, 0.4}, {0.5, 0.9}},
        Points{{0.0, 0.0}, {0.3, 0.7}, {0.6, 0.2}, {0.8, 0.9}, {1.0, 1.0}},
        Points{{0.0, 0.25}, {0.25, 0.4}, {0.5, 0.6}, {0.75, 0.8}, {1.0, 1.0}}};
    const auto channelReference = [](const std::array<Points, 5>& curves,
                                     const std::array<double, 3>& s) {
        std::array<double, 3> out{};
        for (std::size_t i = 0; i < 3; ++i)
            out[i] = curvesReference(curves[i + 1], curvesReference(curves[0], s[i]));
        return out;
    };
    for (const bool clamp : {false, true}) {
        const auto grade =
            runtime::detail::prepareColorGrade(curvesKernel(clamp), curvesValues(channels));
        require(grade && grade->kind == ColorGradeKind::Curves && !grade->identity,
                "Curves prepares");
        checkImage(
            *grade, input,
            [&](const std::array<double, 3>& s, const double a, const render::Rgba32f&) {
                const double alpha = std::clamp(curvesReference(channels[4], a), 0.0, 1.0);
                const auto rgb = channelReference(channels, s);
                std::array<double, 4> out{};
                for (std::size_t i = 0; i < 3; ++i)
                    out[i] = (clamp ? std::clamp(rgb[i], 0.0, 1.0) : rgb[i]) * alpha;
                out[3] = alpha;
                return out;
            },
            "Curves equals the independent monotone-cubic reference for any number of points");
    }
    // Every point is interpolated exactly and HDR extrapolates along the end tangent.
    std::array<Points, 5> single{kIdentityPoints, kIdentityPoints, kIdentityPoints, kIdentityPoints,
                                 kIdentityPoints};
    single[0] = channels[0];
    const auto grade =
        runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(single));
    for (const auto& [x, y] : channels[0]) {
        std::array<float, 4> p{static_cast<float>(x), static_cast<float>(x), static_cast<float>(x),
                               1.0F};
        require(runtime::detail::colorGradePixel(*grade, p) && near(p[0], y),
                "Curves passes through its points");
    }
    std::array<float, 4> hdr{4.0F, 1.0F, 1.5F, 1.0F};
    require(runtime::detail::colorGradePixel(*grade, hdr) && hdr[0] > hdr[2] && hdr[2] > hdr[1] &&
                hdr[1] == 1.0F && near(hdr[0], curvesReference(channels[0], 4.0)),
            "Curves extrapolates above one for HDR");
    // An identity channel is exact, even for values a cubic would round; the default (0,0),(1,1)
    // channel, an empty one and the schema 1 five-point identity are all the identity.
    std::array<float, 4> tiny{1.0e-30F, 3.0e7F, -2.5F, 1.0F};
    const auto red = runtime::detail::prepareColorGrade(
        curvesKernel(false),
        curvesValues(std::array<Points, 5>{kIdentityPoints, kIdentityPoints, kIdentityPoints,
                      Points{{0, 0}, {0.25, 0.25}, {0.5, 0.75}, {0.75, 0.75}, {1, 1}},
                      kIdentityPoints}));
    require(runtime::detail::colorGradePixel(*red, tiny) && tiny[0] == 1.0e-30F &&
                tiny[1] == 3.0e7F && tiny[2] != -2.5F,
            "an identity Curves channel passes values through exactly");
    const Points diagonal{{0.0, 0.0}, {1.0, 1.0}};
    for (const auto& identity :
         {std::array<Points, 5>{kIdentityPoints, kIdentityPoints, kIdentityPoints, kIdentityPoints,
                                kIdentityPoints},
          std::array<Points, 5>{diagonal, diagonal, diagonal, diagonal, diagonal},
          std::array<Points, 5>{}, std::array<Points, 5>{Points{{2.0, 2.0}, {-3.0, -3.0}}}})
        require(runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(identity))
                    ->identity,
                "default, empty and diagonal Curves are the identity");
    const auto defaults = curvesKernel(false);
    std::vector<double> defaultValues;
    for (const auto& curve : defaults.curves) {
        const auto& path = std::get<document::PathValue>(curve.source);
        defaultValues.push_back(static_cast<double>(path.anchors.size()));
        for (const auto& anchor : path.anchors) {
            defaultValues.push_back(anchor.point.x);
            defaultValues.push_back(anchor.point.y);
        }
    }
    require(runtime::detail::prepareColorGrade(defaults, defaultValues)->identity,
            "the document's default curves are the identity");
    // A shuffled point order is the same curve.
    auto shuffled = channels;
    std::reverse(shuffled[0].begin(), shuffled[0].end());
    require(
        runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(shuffled))->data ==
            runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(channels))->data,
        "point order does not change the curve");
    // One distinct input is a constant.
    for (const auto& flatPoints : {Points{{0.5, 0.3}, {0.5, 0.9}, {0.5, 0.1}}, Points{{0.5, 0.3}}}) {
        auto flat = single;
        flat[0] = flatPoints;
        std::array<float, 4> constant{-3.0F, 0.5F, 9.0F, 1.0F};
        require(runtime::detail::colorGradePixel(
                    *runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(flat)),
                    constant) &&
                    constant[0] == 0.3F && constant[1] == 0.3F && constant[2] == 0.3F,
                "points sharing one input are a constant at the first of them");
    }
    auto bad = curvesValues(single);
    bad[2] = std::numeric_limits<double>::infinity();
    require(!runtime::detail::prepareColorGrade(curvesKernel(false), bad),
            "a non-finite point refuses");
    bad[2] = 0.0;
    bad[3] = bad[1] + 1.0e-44;
    require(!runtime::detail::prepareColorGrade(curvesKernel(false), bad),
            "points too close for binary32 coefficients refuse");
    bad = curvesValues(single);
    bad[0] += 1.0;
    require(!runtime::detail::prepareColorGrade(curvesKernel(false), bad) &&
                !runtime::detail::prepareColorGrade(curvesKernel(false),
                                                    std::vector<double>(4, 0.0)),
            "a point count that does not match its points refuses");
    Points many;
    for (std::size_t point = 0; point <= document::kMaximumCurvePoints; ++point) {
        const double x = static_cast<double>(point) / 256.0;
        many.push_back({x, x * x});
    }
    auto tooMany = single;
    tooMany[1] = many;
    require(!runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(tooMany)),
            "more than the maximum number of points refuses");
    many.pop_back();
    tooMany[1] = many;
    require(runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(tooMany)) &&
                runtime::detail::validCurvesTable(
                    runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(tooMany))
                        ->data),
            "the maximum number of points prepares a valid table");
}

// #493: the colour model the hue curves read their hue from, against the independent reference.
bool hueNear(const double a, const double b, const double tolerance) {
    const double d = std::abs(a - b);
    return std::min(d, 1.0 - d) <= tolerance;
}

void hueModelTests() {
    const HueReference reference;
    const auto red = runtime::gradeHue(1.0, 0.0, 0.0);
    require(red && hueNear(*red, 0.0, 1e-12) && !runtime::gradeHue(0.4, 0.4, 0.4) &&
                !runtime::gradeHue(-2.0, -2.0, -2.0),
            "red is hue zero and greys have no hue");
    for (const auto& colour :
         {std::array<double, 3>{0.2, 0.9, 0.1}, std::array<double, 3>{0.1, 0.2, 0.9},
          std::array<double, 3>{4.0, 3.0, 6.0}, std::array<double, 3>{-0.5, 0.2, -1.0},
          std::array<double, 3>{0.8, 0.1, 0.9}, std::array<double, 3>{0.6, 0.4, 0.5},
          std::array<double, 3>{1.0, 1.0, 0.0}, std::array<double, 3>{0.0, 1.0, 1.0}}) {
        const auto hue = runtime::gradeHue(colour[0], colour[1], colour[2]);
        require(hue && *hue >= 0.0 && *hue < 1.0 &&
                    hueNear(*hue, HueReference::hue(reference.opponent(colour)), 1e-12),
                "the hue is the diamond angle of the chroma-plane coordinates");
        const auto scaled = runtime::gradeHue(colour[0] * 3.0 + 1.0, colour[1] * 3.0 + 1.0,
                                              colour[2] * 3.0 + 1.0);
        require(scaled && hueNear(*scaled, *hue, 1e-9),
                "the hue ignores a neutral offset and a positive scale");
    }
    for (int step = 0; step < 24; ++step) {
        const double hue = static_cast<double>(step) / 24.0;
        const auto strip = runtime::gradeHueColor(hue);
        const auto back = runtime::gradeHue(strip[0], strip[1], strip[2]);
        require(back && hueNear(*back, hue, 1e-9) &&
                    std::ranges::all_of(strip, [](const double v) { return v >= 0.0 && v <= 1.0; }),
                "the hue strip shows each hue as a displayable colour");
    }
}

// #493: Hue vs Hue, Hue vs Saturation, Hue vs Luma and Luma vs Saturation against the independent
// reference over HDR, negative and partial-alpha pixels; neutrals, alpha, periodicity, identities
// and domains.
void hueCurvesTests() {
    const auto input = image(kPixels);
    std::array<Points, 5> channels{};
    channels[0] = Points{{0.0, 0.0}, {0.5, 0.55}, {1.0, 1.0}};
    auto curves = channelCurves(channels);
    const auto at = [&curves](const document::GradeCurve curve) -> Points& {
        return curves[document::gradeCurveIndex(curve)];
    };
    at(document::GradeCurve::HueVsHue) = {{0.05, 0.1}, {0.4, -0.15}, {0.7, 0.05}, {0.95, 0.0}};
    at(document::GradeCurve::HueVsSaturation) = {{0.0, 1.6}, {0.33, 0.4}, {0.66, 1.2}};
    at(document::GradeCurve::HueVsLuma) = {{0.1, 1.3}, {0.6, 0.7}};
    at(document::GradeCurve::LumaVsSaturation) = {{0.0, 0.5}, {0.5, 1.4}, {1.0, 0.8}};
    const HueReference reference;
    for (const bool clamp : {false, true}) {
        const auto grade =
            runtime::detail::prepareColorGrade(curvesKernel(clamp), curvesValues(curves));
        require(grade && !grade->identity &&
                    runtime::detail::validCurvesTable(grade->data),
                "hue curves prepare");
        checkImage(
            *grade, input,
            [&](const std::array<double, 3>& s, const double a, const render::Rgba32f&) {
                std::array<double, 3> c{};
                for (std::size_t i = 0; i < 3; ++i)
                    c[i] = curvesReference(channels[0], s[i]);
                c = reference.apply(curves, c);
                std::array<double, 4> out{0.0, 0.0, 0.0, a};
                for (std::size_t i = 0; i < 3; ++i)
                    out[i] = (clamp ? std::clamp(c[i], 0.0, 1.0) : c[i]) * a;
                return out;
            },
            "hue curves equal the independent colour-model reference");
    }
    // Each curve alone: greys and alpha untouched, and a hue curve at hue one is its value at zero.
    for (const auto curve : {document::GradeCurve::HueVsHue, document::GradeCurve::HueVsSaturation,
                             document::GradeCurve::HueVsLuma,
                             document::GradeCurve::LumaVsSaturation}) {
        CurveSet alone{};
        alone[document::gradeCurveIndex(curve)] = curves[document::gradeCurveIndex(curve)];
        const auto grade =
            runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(alone));
        require(grade && !grade->identity, "one hue curve prepares");
        std::array<float, 4> grey{2.5F, 2.5F, 2.5F, 0.5F};
        std::array<float, 4> colour{0.9F, 0.3F, 0.2F, 0.25F};
        require(runtime::detail::colorGradePixel(*grade, grey) && grey[0] == 2.5F &&
                    grey[1] == 2.5F && grey[2] == 2.5F && grey[3] == 0.5F &&
                    runtime::detail::colorGradePixel(*grade, colour) && colour[3] == 0.25F,
                "hue curves leave greys and alpha exactly unchanged");
        auto neutral = alone;
        for (auto& point : neutral[document::gradeCurveIndex(curve)])
            point[1] = document::neutralGradeCurveOutput(curve);
        require(runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(neutral))
                    ->identity,
                "a hue curve at its neutral value is the identity");
    }
    const auto& hueSat = at(document::GradeCurve::HueVsSaturation);
    require(near(curvesReference(hueSat, 0.0, Ends::Periodic),
                 curvesReference(hueSat, 1.0, Ends::Periodic), 1e-12) &&
                near(curvesReference(hueSat, 0.999999, Ends::Periodic),
                     curvesReference(hueSat, 0.0, Ends::Periodic), 1e-4),
            "a hue curve is periodic over the hue strip");
    auto wrapped = curves;
    wrapped[document::gradeCurveIndex(document::GradeCurve::HueVsSaturation)] = {
        {1.0, 1.6}, {0.33, 0.4}, {0.66, 1.2}};
    require(runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(wrapped))->data ==
                runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(curves))->data,
            "a hue point at one is the point at zero");
    // Domains: a hue input in 0..1, a hue shift within half a turn, gains within 0..1000.
    for (const auto& [curve, point] :
         {std::pair{document::GradeCurve::HueVsHue, std::array<double, 2>{0.5, 0.6}},
          std::pair{document::GradeCurve::HueVsHue, std::array<double, 2>{1.5, 0.0}},
          std::pair{document::GradeCurve::HueVsSaturation, std::array<double, 2>{0.5, -0.1}},
          std::pair{document::GradeCurve::HueVsLuma, std::array<double, 2>{-0.1, 1.0}},
          std::pair{document::GradeCurve::LumaVsSaturation, std::array<double, 2>{0.5, 1001.0}}}) {
        CurveSet outside{};
        outside[document::gradeCurveIndex(curve)] = {point};
        require(!runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(outside)),
                "an out-of-domain hue curve point refuses");
    }
}

void hueSaturationTests() {
    const auto input = image(kPixels);
    const auto kernel = runtime::HueSaturationKernel{{document::ParameterId::fromRaw(300), 0.0},
                                                     {document::ParameterId::fromRaw(301), 1.0}};
    const auto matrix = [](const double hue, const double s) {
        // Filter Effects Module Level 1 saturate * hueRotate, in double.
        const double c = std::cos(hue * std::acos(-1.0) / 180.0);
        const double n = std::sin(hue * std::acos(-1.0) / 180.0);
        const double r[3][3] = {{0.213 + c * 0.787 - n * 0.213, 0.715 - c * 0.715 - n * 0.715,
                                 0.072 - c * 0.072 + n * 0.928},
                                {0.213 - c * 0.213 + n * 0.143, 0.715 + c * 0.285 + n * 0.140,
                                 0.072 - c * 0.072 - n * 0.283},
                                {0.213 - c * 0.213 - n * 0.787, 0.715 - c * 0.715 + n * 0.715,
                                 0.072 + c * 0.928 + n * 0.072}};
        const double t[3][3] = {{0.213 + 0.787 * s, 0.715 - 0.715 * s, 0.072 - 0.072 * s},
                                {0.213 - 0.213 * s, 0.715 + 0.285 * s, 0.072 - 0.072 * s},
                                {0.213 - 0.213 * s, 0.715 - 0.715 * s, 0.072 + 0.928 * s}};
        std::array<std::array<double, 3>, 3> m{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    m[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] +=
                        t[i][k] * r[k][j];
        return m;
    };
    for (const auto [hue, saturation] :
         {std::pair{0.0, 0.0}, std::pair{90.0, 1.0}, std::pair{-135.0, 1.6}, std::pair{400.0, 0.5},
          std::pair{180.0, 3.0}}) {
        const std::vector<double> values{hue, saturation};
        const auto grade = runtime::detail::prepareColorGrade(kernel, values);
        require(grade && grade->kind == ColorGradeKind::HueSaturation && !grade->identity,
                "Hue/Saturation prepares");
        const auto m = matrix(hue, saturation);
        checkImage(
            *grade, input,
            [&](const std::array<double, 3>&, const double a, const render::Rgba32f& p) {
                const std::array<double, 3> rgb{static_cast<double>(p.red()),
                                                static_cast<double>(p.green()),
                                                static_cast<double>(p.blue())};
                std::array<double, 4> out{0.0, 0.0, 0.0, a};
                for (std::size_t i = 0; i < 3; ++i)
                    out[i] = m[i][0] * rgb[0] + m[i][1] * rgb[1] + m[i][2] * rgb[2];
                return out;
            },
            "Hue/Saturation equals the Filter Effects matrices over associated RGB");
    }
    // Saturation zero is the Filter Effects luminance. Hue rotation preserves neutrals exactly and
    // that luminance to within the standard's rounded coefficients (about 0.1%).
    const auto grey = runtime::detail::prepareColorGrade(kernel, std::vector<double>{0.0, 0.0});
    std::array<float, 4> p{2.0F, 0.5F, -1.0F, 1.0F};
    require(runtime::detail::colorGradePixel(*grey, p) &&
                near(p[0], 0.213 * 2 + 0.715 * 0.5 - 0.072) && p[0] == p[1] && p[1] == p[2],
            "zero saturation is the HDR luminance on every channel");
    const auto rotate = runtime::detail::prepareColorGrade(kernel, std::vector<double>{120.0, 1.0});
    std::array<float, 4> neutral{3.0F, 3.0F, 3.0F, 1.0F};
    std::array<float, 4> colour{1.5F, 0.25F, 0.5F, 0.5F};
    const double luma = 0.213 * 1.5 + 0.715 * 0.25 + 0.072 * 0.5;
    require(runtime::detail::colorGradePixel(*rotate, neutral) && near(neutral[0], 3.0) &&
                near(neutral[1], 3.0) && near(neutral[2], 3.0) &&
                runtime::detail::colorGradePixel(*rotate, colour) &&
                near(0.213 * static_cast<double>(colour[0]) +
                         0.715 * static_cast<double>(colour[1]) +
                         0.072 * static_cast<double>(colour[2]),
                     luma, 1e-3) &&
                colour[3] == 0.5F,
            "hue rotation preserves neutrals and alpha, and luminance to the standard's rounding");
    require(runtime::detail::prepareColorGrade(kernel, std::vector<double>{0.0, 1.0})->identity,
            "zero hue and unit saturation is an identity");
    require(!runtime::detail::prepareColorGrade(kernel, std::vector<double>{0.0, -0.5}) &&
                !runtime::detail::prepareColorGrade(kernel, std::vector<double>{0.0, 2000.0}) &&
                !runtime::detail::prepareColorGrade(
                    kernel, std::vector<double>{std::numeric_limits<double>::infinity(), 1.0}),
            "Hue/Saturation refuses out-of-domain values");
}

void programTests() {
    std::array<Levels, 5> channels{};
    channels[0] = Levels{0.1, 0.9, 2.0, 0.0, 1.0};
    const auto levels =
        runtime::detail::prepareColorGrade(levelsKernel(false), levelsValues(channels));
    const auto curves = runtime::detail::prepareColorGrade(
        curvesKernel(true),
        curvesValues(std::array<Points, 5>{Points{{0, 0}, {0.3, 0.2}, {0.5, 0.5}, {0.7, 0.8}, {1, 1}}, kIdentityPoints,
                      kIdentityPoints, kIdentityPoints, kIdentityPoints}));
    const auto hue = runtime::detail::prepareColorGrade(
        runtime::HueSaturationKernel{{document::ParameterId::fromRaw(1), 0.0},
                                     {document::ParameterId::fromRaw(2), 1.0}},
        std::vector<double>{45.0, 1.25});
    struct Case final {
        const runtime::detail::PreparedColorGrade* grade;
        const char* semantics;
        const char* function;
    };
    for (const auto& [grade, semantics, function] :
         {Case{&*levels, "bloom.render.levels.v1", "bloom_levels"},
          Case{&*curves, "bloom.render.curves.v1", "bloom_curves"},
          Case{&*hue, "bloom.render.hue-saturation.v1", "bloom_hue_saturation"}}) {
        auto program = runtime::detail::buildColorGradeProgram(grade->kind, grade->data);
        require(program.succeeded() && render::validateOcioGpuProgram(*program.program(), {}) ==
                                           render::OcioGpuProgramError::None,
                "grade program prepares and validates");
        const auto& desc = *program.program();
        require(desc.semanticsId == semantics && desc.functionName == function &&
                    desc.stage == render::OcioGpuProgramStage::ProcessNeighborhood &&
                    desc.uniformBufferStorage &&
                    desc.uniformBufferSize == grade->data.size() * sizeof(float) &&
                    std::memcmp(desc.uniformBufferData.data(), grade->data.data(),
                                grade->data.size() * sizeof(float)) == 0,
                "grade program carries the prepared table as its storage data");
        auto changed = grade->data;
        changed[changed.size() - 2] += 0.5F;
        const auto other = runtime::detail::buildColorGradeProgram(grade->kind, changed);
        require(other.succeeded() && other.program()->shaderText == desc.shaderText &&
                    other.program()->uniformBufferData != desc.uniformBufferData,
                "changed grade values change only the program data, never the shader");
        const auto wrapper = runtime::buildGpuOcioWrapperGlsl(desc);
        require(wrapper.succeeded() && wrapper.source.find(std::string("vec4 p = ") + function +
                                                           "(c)") != std::string::npos,
                "grade runs in the resident neighborhood wrapper");
        auto bad = grade->data;
        bad[0] = std::numeric_limits<float>::quiet_NaN();
        require(
            !runtime::detail::buildColorGradeProgram(grade->kind, bad).succeeded() &&
                !runtime::detail::buildColorGradeProgram(grade->kind, std::vector<float>(4, 0.0F))
                     .succeeded(),
            "non-finite or mis-sized grade data refuses");
    }
    CurveSet hueOnly{};
    hueOnly[document::gradeCurveIndex(document::GradeCurve::HueVsLuma)] = {{0.2, 1.5}};
    const auto hueCurves =
        runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(hueOnly));
    const auto hueProgram = runtime::detail::buildColorGradeProgram(hueCurves->kind,
                                                                    hueCurves->data);
    const auto curvesProgram = runtime::detail::buildColorGradeProgram(curves->kind, curves->data);
    require(hueProgram.succeeded() && curvesProgram.succeeded() &&
                hueProgram.program()->shaderText == curvesProgram.program()->shaderText,
            "every curve set, hue curves included, runs the one fixed Curves program");
    require(levels->data != curves->data && runtime::detail::colorGradeIdentity(*levels) !=
                                                runtime::detail::colorGradeIdentity(*curves),
            "grades have distinct cache identities");
}

void cancellationTest() {
    const auto input = image(kPixels);
    std::array<Levels, 5> channels{};
    channels[1] = Levels{0.0, 0.5, 1.0, 0.0, 1.0};
    const auto grade =
        runtime::detail::prepareColorGrade(levelsKernel(false), levelsValues(channels));
    runtime::TaskScheduler scheduler(runtime::TaskSchedulerConfig::defaults());
    const runtime::TaskOwner owner{.kind = runtime::TaskOwnerKind::Composition,
                                   .id = runtime::TaskOwnerId::fromRaw(85)};
    auto submission = scheduler.submit<void>(
        runtime::TaskRequest("Color grade cancellation", owner),
        [&](runtime::TaskContext& context) {
            const auto result = runtime::detail::applyColorGrade(
                *grade, input, 65536, context.cancellation(), runtime::OperationIndex::fromRaw(0),
                [&](const runtime::EvaluationProgress& progress) {
                    if (progress.completed == 1)
                        static_cast<void>(scheduler.cancelOwner(owner));
                });
            require(result.cancelled && !result.image, "row cancellation publishes no image");
            return runtime::TaskResult<void>::cancelled();
        });
    require(submission.accepted(), "cancellation task admitted");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<runtime::TaskResult<void>> result;
    while (!result && std::chrono::steady_clock::now() < deadline) {
        result = submission.handle.tryTakeResult();
        std::this_thread::yield();
    }
    require(result && result->state() == runtime::TaskState::Cancelled,
            "row cancellation observed");
    scheduler.beginShutdown();
    while (!scheduler.isQuiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    require(scheduler.isQuiescent(), "grade cancellation shuts down safely");
}
} // namespace
int main() {
    try {
        levelsTests();
        curvesTests();
        hueModelTests();
        hueCurvesTests();
        hueSaturationTests();
        programTests();
        cancellationTest();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS: Levels, Curves (any number of points, hue curves) and Hue/Saturation reference, Raw depth normalisation, "
                 "HDR/alpha/windows, budget, resident programs and cancellation\n";
}
