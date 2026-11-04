// #493 Curves: the packed table (color_grade_image_effect.hpp), the CPU reference that applies it
// and the one fixed GPU program that reads the same table, with the same binary32 operations in the
// same order. The curve shapes and the hue colour model are bloom/runtime/grade_curve.hpp's.

#include "color_grade_image_effect.hpp"

#include <bloom/document/grade_curves.hpp>
#include <bloom/runtime/grade_curve.hpp>

#include <cmath>
#include <limits>

namespace bloom::runtime::detail {
namespace {

using document::GradeCurve;
using document::kGradeCurveCount;

constexpr std::size_t kControl = 0;
constexpr std::size_t kLuma = 4;
constexpr std::size_t kToOpponent = 8;
constexpr std::size_t kFromOpponent = 16;
constexpr std::size_t kCurveHeaders = 24;
constexpr std::size_t kFirstHueCurve = document::gradeCurveIndex(GradeCurve::HueVsHue);
static_assert(kCurvesTableHeader == 15 && kCurveHeaders == 4 * 6);

[[nodiscard]] std::size_t vec4s(const std::size_t floats) noexcept { return (floats + 3) / 4; }

[[nodiscard]] bool representable(const double value) noexcept {
    return std::isfinite(value) &&
           std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max());
}

[[nodiscard]] float ends(const GradeCurveEnds value) noexcept {
    return value == GradeCurveEnds::Linear ? 0.0F : value == GradeCurveEnds::Hold ? 1.0F : 2.0F;
}

// Appends a curve's knots, origins and segments at the next whole vec4; false when a value is not
// a finite binary32 number (points too close together).
[[nodiscard]] bool packCurve(const GradeCurveShape& shape, std::vector<float>& data) {
    const auto n = shape.x.size();
    const auto pad = [&data] { data.resize(4 * vec4s(data.size()), 0.0F); };
    for (const auto x : shape.x)
        data.push_back(static_cast<float>(x));
    pad();
    for (const auto origin : shape.origin) {
        if (!representable(origin))
            return false;
        data.push_back(static_cast<float>(origin));
    }
    pad();
    for (const auto& segment : shape.segments)
        for (const auto coefficient : segment) {
            if (!representable(coefficient))
                return false;
            data.push_back(static_cast<float>(coefficient));
        }
    return shape.segments.size() == n + 1;
}

// A curve that is not the identity, at `x`.
[[nodiscard]] float curveAt(const float* table, const std::size_t curve, const float x) noexcept {
    const float* header = table + kCurveHeaders + 4 * curve;
    const auto n = static_cast<int>(header[0]);
    const auto base = 4 * static_cast<int>(header[2]);
    const int origins = base + 4 * ((n + 3) / 4);
    const int segments = origins + 4 * ((n + 4) / 4);
    int low = 0;
    int high = n;
    while (low < high) {
        const int middle = (low + high) / 2;
        if (x >= table[base + middle])
            low = middle + 1;
        else
            high = middle;
    }
    const float dx = x - table[origins + low];
    const float* q = table + segments + 4 * low;
    return std::fma(dx, std::fma(dx, std::fma(dx, q[3], q[2]), q[1]), q[0]);
}

[[nodiscard]] bool curveIdentity(const float* table, const std::size_t curve) noexcept {
    return table[kCurveHeaders + 4 * curve + 1] != 0.0F;
}

[[nodiscard]] float channel(const float* table, const std::size_t curve, const float x) noexcept {
    return curveIdentity(table, curve) ? x : curveAt(table, curve, x);
}

// The four hue curves on straight colour `rgb`, in place (grade_curve.hpp's colour model).
void hueCurves(const float* t, std::array<float, 3>& rgb) noexcept {
    float r = rgb[0];
    float g = rgb[1];
    float b = rgb[2];
    if (r == g && g == b)
        return;
    const float* w = t + kLuma;
    const float* a0 = t + kToOpponent;
    const float* a1 = t + kToOpponent + 4;
    const float* e1 = t + kFromOpponent;
    const float* e2 = t + kFromOpponent + 4;
    const float luma = std::fma(w[2], b, std::fma(w[1], g, w[0] * r));
    const float ca = std::fma(a0[2], b, std::fma(a0[1], g, a0[0] * r));
    const float cb = std::fma(a1[2], b, std::fma(a1[1], g, a1[0] * r));
    if (ca == 0.0F && cb == 0.0F)
        return;
    const float sum = std::abs(ca) + std::abs(cb);
    float hue = 0.0F;
    if (ca >= 0.0F && cb >= 0.0F)
        hue = (cb / sum) * 0.25F;
    else if (cb >= 0.0F)
        hue = (1.0F + -ca / sum) * 0.25F;
    else if (ca < 0.0F)
        hue = (2.0F + -cb / sum) * 0.25F;
    else
        hue = (3.0F + ca / sum) * 0.25F;
    if (hue >= 1.0F)
        hue = 0.0F;
    const auto at = [t](const GradeCurve curve, const float x, const float neutral) {
        const auto index = document::gradeCurveIndex(curve);
        return curveIdentity(t, index) ? neutral : curveAt(t, index, x);
    };
    const float shift = at(GradeCurve::HueVsHue, hue, 0.0F);
    const float hueSaturation = at(GradeCurve::HueVsSaturation, hue, 1.0F);
    const float hueLuma = at(GradeCurve::HueVsLuma, hue, 1.0F);
    const float lumaSaturation = at(GradeCurve::LumaVsSaturation, luma, 1.0F);
    if (shift != 0.0F) {
        float turned = hue + shift;
        turned = turned - std::floor(turned);
        if (turned >= 1.0F)
            turned = 0.0F;
        const float p = turned * 4.0F;
        const float quadrant = std::floor(p);
        const float f = p - quadrant;
        float dx = f;
        float dy = f - 1.0F;
        if (quadrant == 0.0F) {
            dx = 1.0F - f;
            dy = f;
        } else if (quadrant == 1.0F) {
            dx = -f;
            dy = 1.0F - f;
        } else if (quadrant == 2.0F) {
            dx = f - 1.0F;
            dy = -f;
        }
        const float length = std::fma(cb, cb, ca * ca);
        const float direction = std::fma(dy, dy, dx * dx);
        const float k = std::sqrt(length / direction);
        const float na = k * dx;
        const float nb = k * dy;
        const float da = na - ca;
        const float db = nb - cb;
        r = std::fma(e2[0], db, std::fma(e1[0], da, r));
        g = std::fma(e2[1], db, std::fma(e1[1], da, g));
        b = std::fma(e2[2], db, std::fma(e1[2], da, b));
    }
    const float gain = hueSaturation * lumaSaturation;
    if (gain != 1.0F) {
        const float current = std::fma(w[2], b, std::fma(w[1], g, w[0] * r));
        r = std::fma(gain, r - current, current);
        g = std::fma(gain, g - current, current);
        b = std::fma(gain, b - current, current);
    }
    if (hueLuma != 1.0F) {
        const float weight = sum / (sum + std::abs(luma));
        const float factor = std::fma(hueLuma - 1.0F, weight, 1.0F);
        r = r * factor;
        g = g * factor;
        b = b * factor;
    }
    rgb = {r, g, b};
}

} // namespace

std::optional<PreparedColorGrade> prepareCurvesGrade(const bool clamp,
                                                     const std::span<const double> values) {
    PreparedColorGrade grade{ColorGradeKind::Curves,
                             std::vector<float>(4 * kCurvesTableHeader, 0.0F), !clamp};
    const auto& model = gradeHueModel();
    grade.data[kControl] = clamp ? 1.0F : 0.0F;
    for (std::size_t j = 0; j < 3; ++j) {
        grade.data[kLuma + j] = static_cast<float>(model.luma[j]);
        for (std::size_t row = 0; row < 2; ++row) {
            grade.data[kToOpponent + 4 * row + j] = static_cast<float>(model.toOpponent[row][j]);
            grade.data[kFromOpponent + 4 * row + j] =
                static_cast<float>(model.fromOpponent[row][j]);
        }
    }
    std::size_t cursor = 0;
    bool hueActive = false;
    for (std::size_t index = 0; index < kGradeCurveCount; ++index) {
        if (cursor >= values.size())
            return std::nullopt;
        const double count = values[cursor++];
        if (!(count >= 0.0) || count > static_cast<double>(document::kMaximumCurvePoints) ||
            count != std::floor(count) ||
            values.size() - cursor < 2 * static_cast<std::size_t>(count))
            return std::nullopt;
        std::vector<document::Vec2d> points(static_cast<std::size_t>(count));
        for (auto& point : points) {
            point = {values[cursor], values[cursor + 1]};
            cursor += 2;
        }
        const auto curve = document::kGradeCurves[index];
        const auto shape = prepareGradeCurveShape(curve, document::gradeCurvePath(points));
        if (!shape)
            return std::nullopt;
        float* header = grade.data.data() + kCurveHeaders + 4 * index;
        header[0] = static_cast<float>(shape->x.size());
        header[1] = shape->identity ? 1.0F : 0.0F;
        header[3] = ends(shape->ends);
        grade.identity = grade.identity && shape->identity;
        if (shape->identity)
            continue;
        hueActive = hueActive || index >= kFirstHueCurve;
        header[2] = static_cast<float>(grade.data.size() / 4);
        if (!packCurve(*shape, grade.data))
            return std::nullopt;
        grade.data.resize(4 * vec4s(grade.data.size()), 0.0F);
    }
    if (cursor != values.size())
        return std::nullopt;
    grade.data[kControl + 1] = hueActive ? 1.0F : 0.0F;
    return grade;
}

bool validCurvesTable(const std::vector<float>& data) noexcept {
    if (data.size() < 4 * kCurvesTableHeader || data.size() % 4 != 0)
        return false;
    for (std::size_t index = 0; index < kGradeCurveCount; ++index) {
        const float* header = data.data() + kCurveHeaders + 4 * index;
        const float n = header[0];
        if (!(n >= 0.0F) || n > static_cast<float>(document::kMaximumCurvePoints) ||
            n != std::floor(n) || (header[1] != 0.0F && header[1] != 1.0F) ||
            (header[3] != 0.0F && header[3] != 1.0F && header[3] != 2.0F))
            return false;
        if (header[1] != 0.0F)
            continue;
        const auto points = static_cast<std::size_t>(n);
        const float base = header[2];
        if (points == 0 || base != std::floor(base) ||
            base < static_cast<float>(kCurvesTableHeader) ||
            static_cast<std::size_t>(base) + vec4s(points) + vec4s(points + 1) + points + 1 >
                data.size() / 4)
            return false;
    }
    return true;
}

bool curvesGradePixel(const std::vector<float>& data, std::array<float, 4>& pixel) noexcept {
    const auto finite = [](const float x) { return std::isfinite(x); };
    const float* table = data.data();
    const bool clamp = table[kControl] != 0.0F;
    const float a = pixel[3];
    std::array<float, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float straight = a == 0.0F ? 0.0F : pixel[i] / a;
        if (!finite(straight))
            return false;
        out[i] = channel(table, i + 1, channel(table, 0, straight));
        if (!finite(out[i]))
            return false;
    }
    if (table[kControl + 1] != 0.0F) {
        hueCurves(table, out);
        if (!finite(out[0]) || !finite(out[1]) || !finite(out[2]))
            return false;
    }
    if (clamp)
        for (auto& value : out)
            value = std::min(std::max(value, 0.0F), 1.0F);
    float alpha = channel(table, 4, a);
    if (!finite(alpha))
        return false;
    alpha = std::min(std::max(alpha, 0.0F), 1.0F);
    if (alpha == 0.0F) {
        pixel = {0.0F, 0.0F, 0.0F, 0.0F};
        return true;
    }
    pixel = {out[0] * alpha, out[1] * alpha, out[2] * alpha, alpha};
    return finite(pixel[0]) && finite(pixel[1]) && finite(pixel[2]);
}

const std::string& curvesProgramText() {
    // The straight colour is divided and re-associated through the wrapper's correctly rounded
    // division, as the CPU does; every intermediate is `precise`, so nothing is contracted.
    static const std::string text =
        "layout(set = 0, binding = 0, std430) readonly buffer BloomCurvesData { vec4 values[]; "
        "} bloom_curves_data;\n"
        "precise float bloom_ocio_cpu_div(float a, float b);\n"
        "precise vec3 bloom_ocio_cpu_premul(vec3 rgb, float a);\n"
        "bool bloom_grade_finite(float x) { return !isnan(x) && !isinf(x); }\n"
        "float bloom_curves_scalar(int i) { return bloom_curves_data.values[i / 4][i % 4]; }\n"
        "bool bloom_curves_identity(int curve) {\n"
        " return bloom_curves_data.values[6 + curve].y != 0.0;\n"
        "}\n"
        "float bloom_curves_at(int curve, float x) {\n"
        " vec4 header = bloom_curves_data.values[6 + curve];\n"
        " int n = int(header.x);\n"
        " int base = 4 * int(header.z);\n"
        " int origins = base + 4 * ((n + 3) / 4);\n"
        " int segments = origins + 4 * ((n + 4) / 4);\n"
        " int low = 0;\n"
        " int high = n;\n"
        " while (low < high) {\n"
        "  int middle = (low + high) / 2;\n"
        "  if (x >= bloom_curves_scalar(base + middle)) { low = middle + 1; } else { high = "
        "middle; }\n"
        " }\n"
        " precise float dx = x - bloom_curves_scalar(origins + low);\n"
        " vec4 q = bloom_curves_data.values[segments / 4 + low];\n"
        " precise float y = fma(dx, fma(dx, fma(dx, q.w, q.z), q.y), q.x);\n"
        " return y;\n"
        "}\n"
        "float bloom_curves_channel(int c, float x) {\n"
        " return bloom_curves_identity(c) ? x : bloom_curves_at(c, x);\n"
        "}\n"
        "float bloom_curves_or(int curve, float x, float neutral) {\n"
        " return bloom_curves_identity(curve) ? neutral : bloom_curves_at(curve, x);\n"
        "}\n"
        "vec3 bloom_curves_hue(vec3 o) {\n"
        " precise float r = o.x;\n"
        " precise float g = o.y;\n"
        " precise float b = o.z;\n"
        " if (r == g && g == b) { return o; }\n"
        " vec4 w = bloom_curves_data.values[1];\n"
        " vec4 a0 = bloom_curves_data.values[2];\n"
        " vec4 a1 = bloom_curves_data.values[3];\n"
        " vec4 e1 = bloom_curves_data.values[4];\n"
        " vec4 e2 = bloom_curves_data.values[5];\n"
        " precise float luma = fma(w.z, b, fma(w.y, g, w.x * r));\n"
        " precise float ca = fma(a0.z, b, fma(a0.y, g, a0.x * r));\n"
        " precise float cb = fma(a1.z, b, fma(a1.y, g, a1.x * r));\n"
        " if (ca == 0.0 && cb == 0.0) { return o; }\n"
        " precise float s = abs(ca) + abs(cb);\n"
        " precise float hue = 0.0;\n"
        " if (ca >= 0.0 && cb >= 0.0) { hue = bloom_ocio_cpu_div(cb, s) * 0.25; }\n"
        " else if (cb >= 0.0) { hue = (1.0 + bloom_ocio_cpu_div(-ca, s)) * 0.25; }\n"
        " else if (ca < 0.0) { hue = (2.0 + bloom_ocio_cpu_div(-cb, s)) * 0.25; }\n"
        " else { hue = (3.0 + bloom_ocio_cpu_div(ca, s)) * 0.25; }\n"
        " if (hue >= 1.0) { hue = 0.0; }\n"
        " float shift = bloom_curves_or(5, hue, 0.0);\n"
        " float hueSaturation = bloom_curves_or(6, hue, 1.0);\n"
        " float hueLuma = bloom_curves_or(7, hue, 1.0);\n"
        " float lumaSaturation = bloom_curves_or(8, luma, 1.0);\n"
        " if (shift != 0.0) {\n"
        "  precise float turned = hue + shift;\n"
        "  turned = turned - floor(turned);\n"
        "  if (turned >= 1.0) { turned = 0.0; }\n"
        "  precise float p = turned * 4.0;\n"
        "  precise float quadrant = floor(p);\n"
        "  precise float f = p - quadrant;\n"
        "  precise float dx = f;\n"
        "  precise float dy = f - 1.0;\n"
        "  if (quadrant == 0.0) { dx = 1.0 - f; dy = f; }\n"
        "  else if (quadrant == 1.0) { dx = -f; dy = 1.0 - f; }\n"
        "  else if (quadrant == 2.0) { dx = f - 1.0; dy = -f; }\n"
        "  precise float len = fma(cb, cb, ca * ca);\n"
        "  precise float dir = fma(dy, dy, dx * dx);\n"
        "  precise float k = sqrt(bloom_ocio_cpu_div(len, dir));\n"
        "  precise float na = k * dx;\n"
        "  precise float nb = k * dy;\n"
        "  precise float da = na - ca;\n"
        "  precise float db = nb - cb;\n"
        "  r = fma(e2.x, db, fma(e1.x, da, r));\n"
        "  g = fma(e2.y, db, fma(e1.y, da, g));\n"
        "  b = fma(e2.z, db, fma(e1.z, da, b));\n"
        " }\n"
        " precise float gain = hueSaturation * lumaSaturation;\n"
        " if (gain != 1.0) {\n"
        "  precise float current = fma(w.z, b, fma(w.y, g, w.x * r));\n"
        "  r = fma(gain, r - current, current);\n"
        "  g = fma(gain, g - current, current);\n"
        "  b = fma(gain, b - current, current);\n"
        " }\n"
        " if (hueLuma != 1.0) {\n"
        "  precise float weight = bloom_ocio_cpu_div(s, s + abs(luma));\n"
        "  precise float factor = fma(hueLuma - 1.0, weight, 1.0);\n"
        "  r = r * factor;\n"
        "  g = g * factor;\n"
        "  b = b * factor;\n"
        " }\n"
        " return vec3(r, g, b);\n"
        "}\n"
        "vec4 bloom_curves(ivec2 c) {\n"
        " vec4 p = imageLoad(bloom_ocio_input, c);\n"
        " bool clampResult = bloom_curves_data.values[0].x != 0.0;\n"
        " float a = p.a;\n"
        " vec3 s = a == 0.0 ? vec3(0.0) : vec3(bloom_ocio_cpu_div(p.r, a), "
        "bloom_ocio_cpu_div(p.g, a), bloom_ocio_cpu_div(p.b, a));\n"
        " vec3 o = vec3(0.0);\n"
        " for (int i = 0; i < 3; ++i) {\n"
        "  if (!bloom_grade_finite(s[i])) { return vec4(0.0, 0.0, 0.0, 2.0); }\n"
        "  float g = bloom_curves_channel(i + 1, bloom_curves_channel(0, s[i]));\n"
        "  if (!bloom_grade_finite(g)) { return vec4(0.0, 0.0, 0.0, 2.0); }\n"
        "  o[i] = g;\n"
        " }\n"
        " if (bloom_curves_data.values[0].y != 0.0) {\n"
        "  o = bloom_curves_hue(o);\n"
        "  if (!bloom_grade_finite(o.x) || !bloom_grade_finite(o.y) || !bloom_grade_finite(o.z)) "
        "{ return vec4(0.0, 0.0, 0.0, 2.0); }\n"
        " }\n"
        " if (clampResult) { o = min(max(o, vec3(0.0)), vec3(1.0)); }\n"
        " float na = bloom_curves_channel(4, a);\n"
        " if (!bloom_grade_finite(na)) { return vec4(0.0, 0.0, 0.0, 2.0); }\n"
        " na = min(max(na, 0.0), 1.0);\n"
        " return vec4(bloom_ocio_cpu_premul(o, na), na);\n"
        "}\n";
    return text;
}

} // namespace bloom::runtime::detail
