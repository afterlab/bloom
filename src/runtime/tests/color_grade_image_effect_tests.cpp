// #340 Levels, Curves and Hue/Saturation: the CPU reference against independent double-precision
// formulas over HDR, negative and partial-alpha pixels; a Raw depth pass normalised with no colour
// transform; clamp, step, identity and domain rules; windows, budget, cancellation; and the
// resident programs the GPU route prepares.

#include "image_effect.hpp"
#include <bloom/document/color_grade_nodes.hpp>
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

using Points = std::array<std::array<double, 2>, 5>;
constexpr Points kIdentityPoints{{{0.0, 0.0}, {0.25, 0.25}, {0.5, 0.5}, {0.75, 0.75}, {1.0, 1.0}}};
runtime::CurvesKernel curvesKernel(const bool clamp) {
    runtime::CurvesKernel kernel;
    std::uint64_t id = 100;
    for (auto& channel : kernel.points)
        for (auto& point : channel)
            point = {document::ParameterId::fromRaw(id++), document::Vec2d{}};
    kernel.clamp = clamp;
    return kernel;
}
std::vector<double> curvesValues(const std::array<Points, 5>& channels) {
    std::vector<double> values;
    for (const auto& points : channels)
        for (const auto& [x, y] : points) {
            values.push_back(x);
            values.push_back(y);
        }
    return values;
}
// Independent reference: the Fritsch-Carlson monotone cubic in the Hermite basis, extrapolated
// linearly along the end tangents, from points sorted by input with repeated inputs dropped.
double curvesReference(Points points, const double value) {
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
    std::vector<double> h(n - 1);
    std::vector<double> d(n - 1);
    for (std::size_t k = 0; k + 1 < n; ++k) {
        h[k] = x[k + 1] - x[k];
        d[k] = (y[k + 1] - y[k]) / h[k];
    }
    std::vector<double> m(n, d[0]);
    if (n > 2) {
        for (std::size_t k = 1; k + 1 < n; ++k)
            m[k] = d[k - 1] * d[k] <= 0.0
                       ? 0.0
                       : (3.0 * (h[k - 1] + h[k])) /
                             ((2.0 * h[k] + h[k - 1]) / d[k - 1] + (h[k] + 2.0 * h[k - 1]) / d[k]);
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
    if (value < x[0])
        return y[0] + m[0] * (value - x[0]);
    if (value >= x[n - 1])
        return y[n - 1] + m[n - 1] * (value - x[n - 1]);
    std::size_t k = 0;
    while (value >= x[k + 1])
        ++k;
    const double t = (value - x[k]) / h[k];
    const double t2 = t * t;
    const double t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * y[k] + (t3 - 2 * t2 + t) * h[k] * m[k] +
           (-2 * t3 + 3 * t2) * y[k + 1] + (t3 - t2) * h[k] * m[k + 1];
}

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
    // An S-curve master, a lifted and inverted red, an unsorted green with a repeated input, a
    // non-monotone blue and an alpha curve that lifts coverage.
    const std::array<Points, 5> channels{
        Points{{{0.0, 0.0}, {0.25, 0.15}, {0.5, 0.5}, {0.75, 0.85}, {1.0, 1.0}}},
        Points{{{0.0, 1.0}, {0.25, 0.8}, {0.5, 0.5}, {0.75, 0.3}, {1.0, 0.1}}},
        Points{{{1.0, 1.2}, {0.0, 0.1}, {0.5, 0.4}, {0.5, 0.9}, {0.2, 0.3}}},
        Points{{{0.0, 0.0}, {0.3, 0.7}, {0.6, 0.2}, {0.8, 0.9}, {1.0, 1.0}}},
        Points{{{0.0, 0.25}, {0.25, 0.4}, {0.5, 0.6}, {0.75, 0.8}, {1.0, 1.0}}}};
    for (const bool clamp : {false, true}) {
        const auto grade =
            runtime::detail::prepareColorGrade(curvesKernel(clamp), curvesValues(channels));
        require(grade && grade->kind == ColorGradeKind::Curves && !grade->identity,
                "Curves prepares");
        checkImage(
            *grade, input,
            [&](const std::array<double, 3>& s, const double a, const render::Rgba32f&) {
                const double alpha = std::clamp(curvesReference(channels[4], a), 0.0, 1.0);
                std::array<double, 4> out{};
                for (std::size_t i = 0; i < 3; ++i) {
                    double v = curvesReference(channels[i + 1], curvesReference(channels[0], s[i]));
                    if (clamp)
                        v = std::clamp(v, 0.0, 1.0);
                    out[i] = v * alpha;
                }
                out[3] = alpha;
                return out;
            },
            "Curves equals the independent monotone-cubic reference");
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
    // An identity channel is exact, even for values a cubic would round.
    std::array<float, 4> tiny{1.0e-30F, 3.0e7F, -2.5F, 1.0F};
    const auto red = runtime::detail::prepareColorGrade(
        curvesKernel(false),
        curvesValues({kIdentityPoints, kIdentityPoints, kIdentityPoints,
                      Points{{{0, 0}, {0.25, 0.25}, {0.5, 0.75}, {0.75, 0.75}, {1, 1}}},
                      kIdentityPoints}));
    require(runtime::detail::colorGradePixel(*red, tiny) && tiny[0] == 1.0e-30F &&
                tiny[1] == 3.0e7F && tiny[2] != -2.5F,
            "an identity Curves channel passes values through exactly");
    require(runtime::detail::prepareColorGrade(
                curvesKernel(false), curvesValues(std::array<Points, 5>{
                                         kIdentityPoints, kIdentityPoints, kIdentityPoints,
                                         kIdentityPoints, kIdentityPoints}))
                ->identity,
            "default Curves is an identity");
    // A shuffled point order is the same curve.
    auto shuffled = channels;
    std::reverse(shuffled[0].begin(), shuffled[0].end());
    require(
        runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(shuffled))->data ==
            runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(channels))->data,
        "point order does not change the curve");
    // One distinct input is a constant.
    auto flat = single;
    flat[0] = Points{{{0.5, 0.3}, {0.5, 0.9}, {0.5, 0.1}, {0.5, 0.2}, {0.5, 0.4}}};
    std::array<float, 4> constant{-3.0F, 0.5F, 9.0F, 1.0F};
    require(runtime::detail::colorGradePixel(
                *runtime::detail::prepareColorGrade(curvesKernel(false), curvesValues(flat)),
                constant) &&
                constant[0] == 0.3F && constant[1] == 0.3F && constant[2] == 0.3F,
            "points sharing one input are a constant at the first of them");
    auto bad = curvesValues(single);
    bad[3] = std::numeric_limits<double>::infinity();
    require(!runtime::detail::prepareColorGrade(curvesKernel(false), bad),
            "a non-finite point refuses");
    bad[3] = 0.25;
    bad[2] = bad[0] + 1.0e-44;
    require(!runtime::detail::prepareColorGrade(curvesKernel(false), bad),
            "points too close for binary32 coefficients refuse");
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
        curvesValues({Points{{{0, 0}, {0.3, 0.2}, {0.5, 0.5}, {0.7, 0.8}, {1, 1}}}, kIdentityPoints,
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
        hueSaturationTests();
        programTests();
        cancellationTest();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS: Levels, Curves and Hue/Saturation reference, Raw depth normalisation, "
                 "HDR/alpha/windows, budget, resident programs and cancellation\n";
}
