#include "color_grade_image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"
#include "image_effect.hpp"
#include "operation_key.hpp"

#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/grade_curve.hpp>

#include <bloom/document/alpha_association_nodes.hpp>
#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <string>
#include <tuple>
#include <utility>

namespace bloom::runtime::detail {
namespace {
using document::kGradeChannelCount;

constexpr std::size_t kLevelsChannelFloats = 8;
constexpr std::size_t kLevelsFloats = 4 + kGradeChannelCount * kLevelsChannelFloats;
constexpr std::size_t kHueSaturationFloats = 12;
constexpr std::size_t kGradeFloats = 20;
constexpr std::size_t kRollOffFloats = 4;
constexpr std::size_t kAlphaAssociationFloats = 8;
static_assert(kLevelsChannelFloats % 4 == 0);
static_assert(std::tuple_size_v<decltype(GradeKernel::values)> == document::kGradeParameterCount);

// The CSS/SVG Filter Effects luminance weights its saturate and hue-rotate matrices are built on.
constexpr double kLumaRed = 0.213;
constexpr double kLumaGreen = 0.715;
constexpr double kLumaBlue = 0.072;

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic = EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, text, text};
    return result;
}

[[nodiscard]] bool representable(const double value) noexcept {
    return std::isfinite(value) &&
           std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max());
}

[[nodiscard]] bool withinGradeDomain(const double value) noexcept {
    return std::isfinite(value) && std::abs(value) <= document::kMaximumGradeValue;
}

[[nodiscard]] float levelsChannel(const float* c, const float x) noexcept {
    if (c[6] != 0.0F)
        return x;
    float t = 0.0F;
    if (c[5] != 0.0F) {
        t = x >= c[0] ? 1.0F : 0.0F;
    } else {
        const float d = x - c[0];
        t = d * c[1];
    }
    if (c[2] != 1.0F && t > 0.0F)
        t = std::pow(t, c[2]);
    return std::fma(t, c[4], c[3]);
}

[[nodiscard]] std::optional<PreparedColorGrade>
prepareLevels(const LevelsKernel& kernel, const std::span<const double> values) {
    if (values.size() != kGradeChannelCount * document::kLevelsFieldCount)
        return std::nullopt;
    PreparedColorGrade grade{ColorGradeKind::Levels, std::vector<float>(kLevelsFloats, 0.0F),
                             !kernel.clamp};
    grade.data[0] = kernel.clamp ? 1.0F : 0.0F;
    for (std::size_t channel = 0; channel < kGradeChannelCount; ++channel) {
        const auto* v = values.data() + channel * document::kLevelsFieldCount;
        const double inBlack = v[0];
        const double inWhite = v[1];
        const double gamma = v[2];
        const double outBlack = v[3];
        const double outWhite = v[4];
        if (!withinGradeDomain(inBlack) || !withinGradeDomain(inWhite) ||
            !withinGradeDomain(outBlack) || !withinGradeDomain(outWhite) || !std::isfinite(gamma) ||
            gamma < document::kMinimumLevelsGamma || gamma > document::kMaximumLevelsGamma)
            return std::nullopt;
        float* c = grade.data.data() + 4 + channel * kLevelsChannelFloats;
        const bool identity =
            inBlack == 0.0 && inWhite == 1.0 && gamma == 1.0 && outBlack == 0.0 && outWhite == 1.0;
        grade.identity = grade.identity && identity;
        c[0] = static_cast<float>(inBlack);
        // An input range too narrow for binary32 (or empty) is a hard step at input black.
        const double scale = 1.0 / (inWhite - inBlack);
        const bool step = !(inWhite != inBlack) || !representable(scale);
        c[1] = step ? 0.0F : static_cast<float>(scale);
        c[2] = static_cast<float>(1.0 / gamma);
        c[3] = static_cast<float>(outBlack);
        c[4] = static_cast<float>(outWhite - outBlack);
        c[5] = step ? 1.0F : 0.0F;
        c[6] = identity ? 1.0F : 0.0F;
    }
    return grade;
}

[[nodiscard]] std::optional<PreparedColorGrade>
prepareHueSaturation(const std::span<const double> values) {
    if (values.size() != 2 || !withinGradeDomain(values[0]) || !std::isfinite(values[1]) ||
        values[1] < 0.0 || values[1] > document::kMaximumGradeSaturation)
        return std::nullopt;
    const double hue = values[0];
    const double s = values[1];
    PreparedColorGrade grade{ColorGradeKind::HueSaturation,
                             std::vector<float>(kHueSaturationFloats, 0.0F),
                             hue == 0.0 && s == 1.0};
    // Filter Effects Module Level 1, feColorMatrix hueRotate and saturate (one definition, shared
    // with the Hue/Saturation card's spectrum).
    const auto matrix = hueSaturationMatrix(hue, s);
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column)
            grade.data[4 * row + column] = static_cast<float>(matrix[row][column]);
    return grade;
}

// #494 Grade. Every value is checked against its document domain, then the wheels and the
// primaries fold into one slope, offset and power per channel (the ASC CDL form), the contrast and
// its scale, and the saturation; see docs/architecture/image-filters.md for the derivation.
[[nodiscard]] std::optional<PreparedColorGrade> prepareGrade(const std::span<const double> values) {
    using document::GradePrimary;
    using document::GradeWheel;
    if (values.size() != document::kGradeParameterCount)
        return std::nullopt;
    std::size_t index = 0;
    for (const auto wheel : document::kGradeWheels) {
        const auto domain = document::gradeWheelDomain(wheel);
        for (std::size_t component = 0; component < document::kGradeWheelComponentCount;
             ++component, ++index)
            if (!std::isfinite(values[index]) || values[index] < domain.minimum ||
                values[index] > domain.maximum)
                return std::nullopt;
    }
    for (const auto primary : document::kGradePrimaries) {
        const auto domain = document::gradePrimaryDomain(primary);
        if (!std::isfinite(values[index]) || values[index] < domain.minimum ||
            values[index] > domain.maximum)
            return std::nullopt;
        ++index;
    }
    const auto wheel = [&](const GradeWheel which, const std::size_t component) {
        return values[static_cast<std::size_t>(which) * document::kGradeWheelComponentCount +
                      component];
    };
    const auto primary = [&](const GradePrimary which) {
        return values[document::kGradeWheelCount * document::kGradeWheelComponentCount +
                      static_cast<std::size_t>(which)];
    };
    const double temperature = primary(GradePrimary::Temperature);
    const double tint = primary(GradePrimary::Tint);
    const double contrast = primary(GradePrimary::Contrast);
    const double pivot = primary(GradePrimary::Pivot);
    const double saturation = primary(GradePrimary::Saturation);
    const double exposure = std::exp2(primary(GradePrimary::Exposure));
    // White balance as channel gains in stops: Temperature trades red against blue and Tint trades
    // green against red and blue, each keeping the product of the three gains at one.
    const std::array<double, 3> balance{std::exp2(temperature / 200.0 + tint / 400.0),
                                        std::exp2(-tint / 200.0),
                                        std::exp2(-temperature / 200.0 + tint / 400.0)};
    PreparedColorGrade grade{ColorGradeKind::Grade, std::vector<float>(kGradeFloats, 0.0F), false};
    auto& d = grade.data;
    bool identity = contrast == 1.0 && saturation == 1.0;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const double lift = wheel(GradeWheel::Lift, 0) + wheel(GradeWheel::Lift, channel + 1);
        const double gamma = wheel(GradeWheel::Gamma, 0) + wheel(GradeWheel::Gamma, channel + 1);
        const double gain = wheel(GradeWheel::Gain, 0) * wheel(GradeWheel::Gain, channel + 1);
        const double offset = wheel(GradeWheel::Offset, 0) + wheel(GradeWheel::Offset, channel + 1);
        const double slope = exposure * balance[channel] * gain * (1.0 - lift);
        const double shift = gain * lift + offset;
        const double power = std::exp2(-gamma);
        if (!representable(slope) || !representable(shift) || !representable(power))
            return std::nullopt;
        d[channel] = static_cast<float>(slope);
        d[4 + channel] = static_cast<float>(shift);
        d[8 + channel] = static_cast<float>(power);
        identity =
            identity && d[channel] == 1.0F && d[4 + channel] == 0.0F && d[8 + channel] == 1.0F;
    }
    // Contrast about the pivot: pivot * (t / pivot)^contrast = pivot^(1 - contrast) * t^contrast.
    const double scale = std::pow(pivot, 1.0 - contrast);
    if (!representable(scale) || static_cast<float>(scale) == 0.0F)
        return std::nullopt;
    d[12] = static_cast<float>(contrast);
    d[13] = static_cast<float>(scale);
    d[14] = static_cast<float>(saturation);
    d[16] = static_cast<float>(kLumaRed);
    d[17] = static_cast<float>(kLumaGreen);
    d[18] = static_cast<float>(kLumaBlue);
    grade.identity = identity;
    return grade;
}

// #506 Highlight Roll-off: [amount, luma red, luma green, luma blue], the Rec. 709 weights of the
// track-matte Luma (render::imageCoverage) and Luma Key.
[[nodiscard]] std::optional<PreparedColorGrade>
prepareRollOff(const std::span<const double> values) {
    if (values.size() != 1 || !std::isfinite(values[0]) || values[0] < 0.0 ||
        values[0] > document::kMaximumGlowRollOff)
        return std::nullopt;
    PreparedColorGrade grade{ColorGradeKind::RollOff, std::vector<float>(kRollOffFloats, 0.0F),
                             values[0] == 0.0};
    grade.data = {static_cast<float>(values[0]), 0.2126F, 0.7152F, 0.0722F};
    grade.identity = grade.data[0] == 0.0F;
    return grade;
}

// c / (1 + amount * max(L, 0)) on associated colour; the divisor is at least one, so the result is
// finite and never larger in magnitude than the input. Alpha is unchanged.
[[nodiscard]] bool rollOffPixel(const std::vector<float>& d, std::array<float, 4>& pixel) noexcept {
    const float luma = std::fma(d[3], pixel[2], std::fma(d[2], pixel[1], d[1] * pixel[0]));
    const float divisor = std::fma(d[0], std::max(luma, 0.0F), 1.0F);
    for (std::size_t i = 0; i < 3; ++i)
        pixel[i] = pixel[i] / divisor;
    return std::isfinite(pixel[0]) && std::isfinite(pixel[1]) && std::isfinite(pixel[2]);
}

// #503 Alpha Association: [table mode, epsilon, 0, 0] [background r, g, b, 0]. The table mode is
// 0 Unmult over black, 1 Unmult over white, 2 Premultiply, 3 Unpremultiply, 4 Remove Color Matting;
// the background is zero except in Remove Color Matting, so an unused colour never changes the
// table or the cache identity.
[[nodiscard]] std::optional<PreparedColorGrade>
prepareAlphaAssociation(const AlphaAssociationKernel& kernel,
                        const std::span<const double> values) {
    if (values.size() != 4 || static_cast<std::uint8_t>(kernel.mode) >
                                  static_cast<std::uint8_t>(AlphaAssociationMode::RemoveMatting))
        return std::nullopt;
    for (std::size_t i = 0; i < 3; ++i)
        if (!std::isfinite(values[i]) ||
            std::abs(values[i]) > document::kMaximumAlphaAssociationBackground)
            return std::nullopt;
    float mode = 0.0F;
    switch (kernel.mode) {
    case AlphaAssociationMode::Unmult:
        mode = kernel.white ? 1.0F : 0.0F;
        break;
    case AlphaAssociationMode::Premultiply:
        mode = 2.0F;
        break;
    case AlphaAssociationMode::Unpremultiply:
        mode = 3.0F;
        break;
    case AlphaAssociationMode::RemoveMatting:
        mode = 4.0F;
        break;
    }
    PreparedColorGrade grade{ColorGradeKind::AlphaAssociation,
                             std::vector<float>(kAlphaAssociationFloats, 0.0F), false};
    grade.data[0] = mode;
    grade.data[1] = document::kAlphaAssociationEpsilon;
    if (kernel.mode == AlphaAssociationMode::RemoveMatting)
        for (std::size_t i = 0; i < 3; ++i)
            grade.data[4 + i] = static_cast<float>(values[i]);
    // Subtracting (1 - alpha) * 0 changes nothing: matting against black is the identity.
    grade.identity =
        mode == 4.0F && grade.data[4] == 0.0F && grade.data[5] == 0.0F && grade.data[6] == 0.0F;
    return grade;
}

// One premultiplied pixel. Alpha is a clamped max or the input, except over white where it is
// one binary32 subtraction from one; alpha 0 is transparent black. Only Unpremultiply divides, and
// only above the epsilon.
[[nodiscard]] bool alphaAssociationPixel(const std::vector<float>& d,
                                         std::array<float, 4>& pixel) noexcept {
    const float a = pixel[3];
    auto out = pixel;
    const auto mode = static_cast<int>(d[0]);
    if (mode == 0) {
        // The image over black is its premultiplied colour; its brightest channel is the alpha.
        const float m = std::max(std::max(pixel[0], pixel[1]), pixel[2]);
        out[3] = std::min(std::max(m, 0.0F), 1.0F);
    } else if (mode == 1) {
        // The image over white is c + (1 - a); one minus its darkest channel is the alpha, and
        // the colour is what remains once (1 - alpha) of white is taken away.
        const float t = 1.0F - a;
        const std::array<float, 3> w{pixel[0] + t, pixel[1] + t, pixel[2] + t};
        const float n = 1.0F - std::min(std::min(w[0], w[1]), w[2]);
        out[3] = std::min(std::max(n, 0.0F), 1.0F);
        const float s = out[3] - 1.0F;
        for (std::size_t i = 0; i < 3; ++i)
            out[i] = w[i] + s;
    } else if (mode == 2) {
        for (std::size_t i = 0; i < 3; ++i)
            out[i] = pixel[i] * a;
    } else if (mode == 3) {
        if (a > d[1])
            for (std::size_t i = 0; i < 3; ++i)
                out[i] = pixel[i] / a;
    } else {
        const float t = 1.0F - a;
        for (std::size_t i = 0; i < 3; ++i)
            out[i] = std::fma(-t, d[4 + i], pixel[i]);
    }
    if (out[3] == 0.0F)
        out = {0.0F, 0.0F, 0.0F, 0.0F};
    pixel = out;
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

[[nodiscard]] bool gradePixel(const std::vector<float>& d, std::array<float, 4>& pixel) noexcept {
    const auto finite = [](const float x) { return std::isfinite(x); };
    const float a = pixel[3];
    // Alpha is left alone, and a pixel with no coverage has no straight colour to grade.
    if (a == 0.0F)
        return true;
    std::array<float, 3> y{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float straight = pixel[i] / a;
        if (!finite(straight))
            return false;
        float t = std::fma(straight, d[i], d[4 + i]);
        if (d[8 + i] != 1.0F && t > 0.0F)
            t = std::pow(t, d[8 + i]);
        if (d[12] != 1.0F && t > 0.0F)
            t = d[13] * std::pow(t, d[12]);
        y[i] = t;
    }
    if (d[14] != 1.0F) {
        const float luma = std::fma(d[18], y[2], std::fma(d[17], y[1], d[16] * y[0]));
        for (auto& value : y)
            value = std::fma(value - luma, d[14], luma);
    }
    pixel = {y[0] * a, y[1] * a, y[2] * a, a};
    return finite(y[0]) && finite(y[1]) && finite(y[2]) && finite(pixel[0]) && finite(pixel[1]) &&
           finite(pixel[2]);
}

[[nodiscard]] std::string glslProgramText(const ColorGradeKind kind) {
    // The straight-colour grades divide and re-associate exactly as the CPU reference does, through
    // the wrapper's correctly rounded division (defined after this body, hence the prototypes).
    static const std::string straight = "precise float bloom_ocio_cpu_div(float a, float b);\n"
                                        "precise vec3 bloom_ocio_cpu_premul(vec3 rgb, float a);\n";
    // An alpha above one, which the wrapper refuses like any non-finite result.
    static const std::string failure = " return vec4(0.0, 0.0, 0.0, 2.0);\n";
    static const std::string finite =
        "bool bloom_grade_finite(float x) { return !isnan(x) && !isinf(x); }\n";
    switch (kind) {
    case ColorGradeKind::Levels: {
        static const std::string text =
            "layout(set = 0, binding = 0, std430) readonly buffer BloomLevelsData { vec4 values[]; "
            "} bloom_levels_data;\n" +
            straight + finite +
            "float bloom_levels_channel(int c, float x) {\n"
            " vec4 u = bloom_levels_data.values[1 + 2 * c];\n"
            " vec4 v = bloom_levels_data.values[2 + 2 * c];\n"
            " if (v.z != 0.0) { return x; }\n"
            " precise float t = 0.0;\n"
            " if (v.y != 0.0) {\n"
            "  t = x >= u.x ? 1.0 : 0.0;\n"
            " } else {\n"
            "  precise float d = x - u.x;\n"
            "  t = d * u.y;\n"
            " }\n"
            " if (u.z != 1.0 && t > 0.0) { t = pow(t, u.z); }\n"
            " precise float y = fma(t, v.x, u.w);\n"
            " return y;\n"
            "}\n"
            "vec4 bloom_levels(ivec2 c) {\n"
            " vec4 p = imageLoad(bloom_ocio_input, c);\n"
            " bool clampResult = bloom_levels_data.values[0].x != 0.0;\n"
            " float a = p.a;\n"
            " vec3 s = a == 0.0 ? vec3(0.0) : vec3(bloom_ocio_cpu_div(p.r, a), "
            "bloom_ocio_cpu_div(p.g, a), bloom_ocio_cpu_div(p.b, a));\n"
            " vec3 o = vec3(0.0);\n"
            " for (int i = 0; i < 3; ++i) {\n"
            "  if (!bloom_grade_finite(s[i])) {" +
            failure +
            "  }\n"
            "  float g = bloom_levels_channel(i + 1, bloom_levels_channel(0, s[i]));\n"
            "  if (!bloom_grade_finite(g)) {" +
            failure +
            "  }\n"
            "  o[i] = clampResult ? min(max(g, 0.0), 1.0) : g;\n"
            " }\n"
            " float na = bloom_levels_channel(4, a);\n"
            " if (!bloom_grade_finite(na)) {" +
            failure +
            " }\n"
            " na = min(max(na, 0.0), 1.0);\n"
            " return vec4(bloom_ocio_cpu_premul(o, na), na);\n"
            "}\n";
        return text;
    }
    case ColorGradeKind::Curves:
        return curvesProgramText();
    case ColorGradeKind::Grade: {
        static const std::string text =
            "layout(set = 0, binding = 0, std430) readonly buffer BloomGradeData { vec4 values[]; "
            "} bloom_grade_data;\n" +
            straight + finite +
            "vec4 bloom_grade(ivec2 c) {\n"
            " vec4 p = imageLoad(bloom_ocio_input, c);\n"
            " float a = p.a;\n"
            " if (a == 0.0) { return p; }\n"
            " vec4 slope = bloom_grade_data.values[0];\n"
            " vec4 shift = bloom_grade_data.values[1];\n"
            " vec4 power = bloom_grade_data.values[2];\n"
            " vec4 shape = bloom_grade_data.values[3];\n"
            " vec4 w = bloom_grade_data.values[4];\n"
            " vec3 s = vec3(bloom_ocio_cpu_div(p.r, a), bloom_ocio_cpu_div(p.g, a), "
            "bloom_ocio_cpu_div(p.b, a));\n"
            " vec3 y = vec3(0.0);\n"
            " for (int i = 0; i < 3; ++i) {\n"
            "  if (!bloom_grade_finite(s[i])) {" +
            failure +
            "  }\n"
            "  precise float t = fma(s[i], slope[i], shift[i]);\n"
            "  if (power[i] != 1.0 && t > 0.0) { t = pow(t, power[i]); }\n"
            "  if (shape.x != 1.0 && t > 0.0) { precise float r = pow(t, shape.x); t = shape.y * "
            "r; "
            "}\n"
            "  y[i] = t;\n"
            " }\n"
            " if (shape.z != 1.0) {\n"
            "  precise float luma = fma(w.z, y.z, fma(w.y, y.y, w.x * y.x));\n"
            "  for (int i = 0; i < 3; ++i) {\n"
            "   precise float d = y[i] - luma;\n"
            "   precise float m = fma(d, shape.z, luma);\n"
            "   y[i] = m;\n"
            "  }\n"
            " }\n"
            " for (int i = 0; i < 3; ++i) {\n"
            "  if (!bloom_grade_finite(y[i])) {" +
            failure +
            "  }\n"
            " }\n"
            " return vec4(bloom_ocio_cpu_premul(y, a), a);\n"
            "}\n";
        return text;
    }
    case ColorGradeKind::RollOff: {
        // The divisor is at least one; the wrapper's correctly rounded division matches the CPU's.
        static const std::string text =
            "layout(set = 0, binding = 0, std430) readonly buffer BloomRollOffData { vec4 "
            "values[]; } bloom_roll_off_data;\n" +
            straight +
            "vec4 bloom_roll_off(ivec2 c) {\n"
            " vec4 p = imageLoad(bloom_ocio_input, c);\n"
            " vec4 k = bloom_roll_off_data.values[0];\n"
            " precise float luma = fma(k.w, p.b, fma(k.z, p.g, k.y * p.r));\n"
            " precise float d = fma(k.x, max(luma, 0.0), 1.0);\n"
            " return vec4(bloom_ocio_cpu_div(p.r, d), bloom_ocio_cpu_div(p.g, d), "
            "bloom_ocio_cpu_div(p.b, d), p.a);\n"
            "}\n";
        return text;
    }
    case ColorGradeKind::AlphaAssociation: {
        // The same binary32 operations as alphaAssociationPixel: exact max/min/clamp alphas,
        // precise sums, fused multiply-adds, the wrapper's premultiply and correctly rounded
        // division. The wrapper makes alpha 0 transparent black and refuses a non-finite result.
        static const std::string text =
            "layout(set = 0, binding = 0, std430) readonly buffer BloomAlphaAssociationData { "
            "vec4 values[]; } bloom_alpha_association_data;\n" +
            straight +
            "vec4 bloom_alpha_association(ivec2 c) {\n"
            " vec4 p = imageLoad(bloom_ocio_input, c);\n"
            " vec4 k = bloom_alpha_association_data.values[0];\n"
            " vec4 b = bloom_alpha_association_data.values[1];\n"
            " int mode = int(k.x);\n"
            " vec4 o = p;\n"
            " if (mode == 0) {\n"
            "  o.a = min(max(max(max(p.r, p.g), p.b), 0.0), 1.0);\n"
            " } else if (mode == 1) {\n"
            "  precise float t = 1.0 - p.a;\n"
            "  precise vec3 w = vec3(p.r + t, p.g + t, p.b + t);\n"
            "  precise float n = 1.0 - min(min(w.r, w.g), w.b);\n"
            "  o.a = min(max(n, 0.0), 1.0);\n"
            "  precise float s = o.a - 1.0;\n"
            "  precise vec3 r = vec3(w.r + s, w.g + s, w.b + s);\n"
            "  o.rgb = r;\n"
            " } else if (mode == 2) {\n"
            "  o.rgb = bloom_ocio_cpu_premul(p.rgb, p.a);\n"
            " } else if (mode == 3) {\n"
            "  if (p.a > k.y) { o.rgb = vec3(bloom_ocio_cpu_div(p.r, p.a), "
            "bloom_ocio_cpu_div(p.g, p.a), bloom_ocio_cpu_div(p.b, p.a)); }\n"
            " } else {\n"
            "  precise float t = 1.0 - p.a;\n"
            "  precise vec3 r = vec3(fma(-t, b.x, p.r), fma(-t, b.y, p.g), fma(-t, b.z, p.b));\n"
            "  o.rgb = r;\n"
            " }\n"
            " return o;\n"
            "}\n";
        return text;
    }
    case ColorGradeKind::HueSaturation: {
        static const std::string text =
            "layout(set = 0, binding = 0, std430) readonly buffer BloomHueSaturationData { vec4 "
            "values[]; } bloom_hue_saturation_data;\n"
            "vec4 bloom_hue_saturation(ivec2 c) {\n"
            " vec4 p = imageLoad(bloom_ocio_input, c);\n"
            " vec3 o = vec3(0.0);\n"
            " for (int i = 0; i < 3; ++i) {\n"
            "  vec4 m = bloom_hue_saturation_data.values[i];\n"
            "  precise float y = fma(m.z, p.b, fma(m.y, p.g, m.x * p.r));\n"
            "  o[i] = y;\n"
            " }\n"
            " return vec4(o, p.a);\n"
            "}\n";
        return text;
    }
    }
    return {};
}
} // namespace

std::optional<std::vector<double>>
sampleColorGradeOperands(const ImageEffectKernel& kernel, const CompiledCompositionPlan& plan,
                         const ResolvedEvaluation& resolved,
                         const std::optional<core::RationalTime> time) {
    std::vector<double> values;
    bool evaluable = true;
    const bool grade = forEachColorGradeOperand(kernel, [&](const auto& parameter) {
        using Parameter = std::decay_t<decltype(parameter)>;
        if constexpr (std::is_same_v<Parameter, CompiledPathParameter>) {
            // #493: a curve's point count, then its points, at the operation's clock time.
            std::optional<document::PathValue> sampled;
            if (const auto* constant = std::get_if<document::PathValue>(&parameter.source))
                sampled = *constant;
            else if (time)
                sampled = samplePathCurve(std::get<CompiledPathCurve>(parameter.source), *time);
            if (!sampled) {
                evaluable = false;
                return;
            }
            values.push_back(static_cast<double>(sampled->anchors.size()));
            for (const auto& anchor : sampled->anchors) {
                values.push_back(anchor.point.x);
                values.push_back(anchor.point.y);
            }
        } else {
            const auto sampled = resolveParameter(parameter, plan, resolved);
            if (!sampled) {
                evaluable = false;
                return;
            }
            using Value = std::decay_t<decltype(sampled->value)>;
            if constexpr (std::is_same_v<Value, document::Vec2d>) {
                values.push_back(sampled->value.x);
                values.push_back(sampled->value.y);
            } else if constexpr (std::is_same_v<Value, core::Color4d>) {
                // #503: a colour as its straight red, green, blue and alpha.
                values.push_back(sampled->value.red);
                values.push_back(sampled->value.green);
                values.push_back(sampled->value.blue);
                values.push_back(sampled->value.alpha);
            } else {
                values.push_back(sampled->value);
            }
        }
    });
    if (!grade || !evaluable)
        return std::nullopt;
    return values;
}

std::optional<PreparedColorGrade> prepareColorGrade(const ImageEffectKernel& kernel,
                                                    const std::span<const double> values) {
    if (const auto* levels = std::get_if<LevelsKernel>(&kernel))
        return prepareLevels(*levels, values);
    if (const auto* curves = std::get_if<CurvesKernel>(&kernel))
        return prepareCurvesGrade(curves->clamp, values);
    if (std::holds_alternative<HueSaturationKernel>(kernel))
        return prepareHueSaturation(values);
    if (std::holds_alternative<GradeKernel>(kernel))
        return prepareGrade(values);
    if (std::holds_alternative<HighlightRollOffKernel>(kernel))
        return prepareRollOff(values);
    if (const auto* association = std::get_if<AlphaAssociationKernel>(&kernel))
        return prepareAlphaAssociation(*association, values);
    return std::nullopt;
}

bool colorGradePixel(const PreparedColorGrade& grade, std::array<float, 4>& pixel) noexcept {
    const auto finite = [](const float x) { return std::isfinite(x); };
    if (grade.kind == ColorGradeKind::HueSaturation) {
        std::array<float, 3> out{};
        for (std::size_t row = 0; row < 3; ++row) {
            const float* m = grade.data.data() + 4 * row;
            out[row] = std::fma(m[2], pixel[2], std::fma(m[1], pixel[1], m[0] * pixel[0]));
            if (!finite(out[row]))
                return false;
        }
        pixel = {out[0], out[1], out[2], pixel[3]};
        return true;
    }
    if (grade.kind == ColorGradeKind::Curves)
        return curvesGradePixel(grade.data, pixel);
    if (grade.kind == ColorGradeKind::Grade)
        return gradePixel(grade.data, pixel);
    if (grade.kind == ColorGradeKind::RollOff)
        return rollOffPixel(grade.data, pixel);
    if (grade.kind == ColorGradeKind::AlphaAssociation)
        return alphaAssociationPixel(grade.data, pixel);
    const auto channel = [&](const std::size_t index, const float x) {
        return levelsChannel(grade.data.data() + 4 + index * kLevelsChannelFloats, x);
    };
    const bool clamp = grade.data[0] != 0.0F;
    const float a = pixel[3];
    std::array<float, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float straight = a == 0.0F ? 0.0F : pixel[i] / a;
        if (!finite(straight))
            return false;
        const float graded = channel(i + 1, channel(0, straight));
        if (!finite(graded))
            return false;
        out[i] = clamp ? std::min(std::max(graded, 0.0F), 1.0F) : graded;
    }
    float alpha = channel(4, a);
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

std::string colorGradeIdentity(const PreparedColorGrade& grade) {
    OperationKey key;
    key.add(std::string("color-grade-v1"));
    key.add(grade.kind);
    for (const auto value : grade.data)
        key.add(static_cast<double>(value));
    return key.bytes();
}

ImageEffectResult applyColorGrade(const PreparedColorGrade& grade,
                                  const render::Rgba32fImage& input, const std::size_t budget,
                                  const CancellationToken& cancellation,
                                  const OperationIndex operation,
                                  const EvaluationProgressCallback& progress) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    auto builder = render::Rgba32fImageBuilder::create(*input.descriptor(), budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Color grade output exceeds the pixel budget");
    const auto view = input.view();
    if (!view)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Color grade input is invalid");
    const auto window = input.descriptor()->dataWindow();
    const auto rows = window.extent().height();
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, rows});
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
        if (cancellation.isCancellationRequested())
            return ImageEffectResult{{}, {}, true};
        const auto source = view.value()->row(y);
        auto target = builder.value()->row(y);
        if (!source || !target)
            return failed(EvaluationDiagnosticCode::InternalInvariant,
                          "Color grade row is unavailable");
        for (std::size_t column = 0; column < source.value()->size(); ++column) {
            if ((column & 4095U) == 0 && cancellation.isCancellationRequested())
                return ImageEffectResult{{}, {}, true};
            const auto p = (*source.value())[column];
            std::array<float, 4> graded{p.red(), p.green(), p.blue(), p.alpha()};
            if (!colorGradePixel(grade, graded))
                return failed(EvaluationDiagnosticCode::InvalidPixel,
                              "Color grade produced a non-finite value");
            const auto pixel =
                render::Rgba32f::fromPremultiplied(graded[0], graded[1], graded[2], graded[3]);
            if (!pixel)
                return failed(EvaluationDiagnosticCode::InvalidPixel,
                              "Color grade produced an invalid pixel");
            (*target.value())[column] = *pixel.value();
        }
        reportProgress(progress, {EvaluationProgressStage::Operation, operation,
                                  static_cast<std::uint64_t>(y - window.originY() + 1), rows});
    }
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel,
                      "Color grade image publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

render::OcioGpuProgramResult buildColorGradeProgram(const ColorGradeKind kind,
                                                    const std::vector<float>& data) {
    const bool shaped = kind == ColorGradeKind::Levels   ? data.size() == kLevelsFloats
                        : kind == ColorGradeKind::Curves ? validCurvesTable(data)
                        : kind == ColorGradeKind::Grade  ? data.size() == kGradeFloats
                        : kind == ColorGradeKind::RollOff
                            ? data.size() == kRollOffFloats && data[0] >= 0.0F
                        : kind == ColorGradeKind::AlphaAssociation
                            ? data.size() == kAlphaAssociationFloats && data[0] >= 0.0F &&
                                  data[0] <= 4.0F && data[0] == std::floor(data[0])
                            : data.size() == kHueSaturationFloats;
    if (static_cast<std::uint8_t>(kind) >
            static_cast<std::uint8_t>(ColorGradeKind::AlphaAssociation) ||
        !shaped ||
        !std::ranges::all_of(data, [](const float value) { return std::isfinite(value); }))
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    render::OcioGpuProgramDesc program;
    program.stage = render::OcioGpuProgramStage::ProcessNeighborhood;
    switch (kind) {
    case ColorGradeKind::Levels:
        program.functionName = "bloom_levels";
        program.resourcePrefix = "bloom_levels_";
        program.semanticsId = "bloom.render.levels.v1";
        break;
    case ColorGradeKind::Curves:
        program.functionName = "bloom_curves";
        program.resourcePrefix = "bloom_curves_";
        program.semanticsId = "bloom.render.curves.v1";
        break;
    case ColorGradeKind::HueSaturation:
        program.functionName = "bloom_hue_saturation";
        program.resourcePrefix = "bloom_hue_saturation_";
        program.semanticsId = "bloom.render.hue-saturation.v1";
        break;
    case ColorGradeKind::Grade:
        program.functionName = "bloom_grade";
        program.resourcePrefix = "bloom_grade_";
        program.semanticsId = "bloom.render.grade.v1";
        break;
    case ColorGradeKind::RollOff:
        program.functionName = "bloom_roll_off";
        program.resourcePrefix = "bloom_roll_off_";
        program.semanticsId = "bloom.render.highlight-roll-off.v1";
        break;
    case ColorGradeKind::AlphaAssociation:
        program.functionName = "bloom_alpha_association";
        program.resourcePrefix = "bloom_alpha_association_";
        program.semanticsId = "bloom.render.alpha-association.v1";
        break;
    }
    program.shaderText = glslProgramText(kind);
    program.uniformBufferStorage = true;
    program.uniformBufferSize = data.size() * sizeof(float);
    program.uniformBufferData.resize(data.size() * sizeof(float));
    std::memcpy(program.uniformBufferData.data(), data.data(), data.size() * sizeof(float));
    program.shaderTextDigest = render::computeOcioGpuShaderTextDigest(program.shaderText);
    program.resourceDigest = render::computeOcioGpuResourceDigest(program);
    program.contentIdentity = render::computeOcioGpuContentIdentity(
        {program.semanticsId, {}, {}, program.shaderTextDigest, program.resourceDigest});
    return render::OcioGpuProgramResult::success(std::move(program));
}
} // namespace bloom::runtime::detail
