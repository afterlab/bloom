#include "color_grade_image_effect.hpp"
#include "motion_blur_image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"
#include "depth_effect_image_effect.hpp"
#include "lens_flare_image_effect.hpp"
#include "film_grain_image_effect.hpp"
#include "dither_image_effect.hpp"
#include "codec_degrade_image_effect.hpp"
#include "auto_exposure_image_effect.hpp"
#include "pixel_math_image_effect.hpp"
#include "color_tool_image_effect.hpp"
#include "histogram_image_effect.hpp"
#include "statistics_image_effect.hpp"
#include "audio_plot_image_effect.hpp"
#include "sharpen_image_effect.hpp"
#include "field_raster.hpp"
#include "gradient_source.hpp"
#include "image_effect.hpp"
#include "operation_key.hpp"

#include <bloom/runtime/animation_sampling.hpp>
#include <bloom/runtime/grade_curve.hpp>
#include <bloom/runtime/value_graph_evaluation.hpp>

#include <bloom/render/gradient_modes_glsl.hpp>

#include <bloom/document/alpha_association_nodes.hpp>
#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/film_grain_nodes.hpp>

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
constexpr std::size_t kHueSaturationFloats = 16;
constexpr std::size_t kGradeFloats = 20;
constexpr std::size_t kRollOffFloats = 4;
constexpr std::size_t kAlphaAssociationFloats = 8;
constexpr std::size_t kColorizeFloats = 40;
constexpr std::size_t kColorizeOperands = 26;
// #541: a Gradient Map's stops follow the fixed table, two vec4s each.
constexpr std::size_t kColorizeStopFloats = 8;
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
prepareHueSaturation(const HueSaturationKernel& kernel, const std::span<const double> values) {
    // The operands are hue, saturation and (when the kernel supplies it) vibrance.
    if ((values.size() != 2 && values.size() != 3) || !withinGradeDomain(values[0]) ||
        !std::isfinite(values[1]) || values[1] < 0.0 ||
        values[1] > document::kMaximumGradeSaturation)
        return std::nullopt;
    const double hue = values[0];
    const double s = values[1];
    const double vibrance = values.size() == 3 ? values[2] : 0.0;
    if (!std::isfinite(vibrance) || std::abs(vibrance) > document::kMaximumVibrance)
        return std::nullopt;
    PreparedColorGrade grade{ColorGradeKind::HueSaturation,
                             std::vector<float>(kHueSaturationFloats, 0.0F),
                             !kernel.colorize && hue == 0.0 && s == 1.0 && vibrance == 0.0};
    grade.data[12] = static_cast<float>(vibrance);
    if (kernel.colorize) {
        // Colorize: every output is the input's luma (the weights below) times one tint, so the
        // matrix has rank one. The tint is the hue at full chroma blended toward white by the
        // saturation (1 is the full hue; above 1 is the same), scaled to a luma of one.
        const double turns = std::fmod(hue, 360.0) / 60.0;
        const double sector = turns < 0.0 ? turns + 6.0 : turns;
        const auto ramp = [&](const double offset) {
            return std::clamp(std::abs(std::fmod(sector + offset, 6.0) - 3.0) - 1.0, 0.0, 1.0);
        };
        const double amount = std::min(s, 1.0);
        const std::array<double, 3> tint{1.0 + amount * (ramp(0.0) - 1.0),
                                         1.0 + amount * (ramp(4.0) - 1.0),
                                         1.0 + amount * (ramp(2.0) - 1.0)};
        constexpr std::array<double, 3> weights{0.213, 0.715, 0.072};
        const double luma = weights[0] * tint[0] + weights[1] * tint[1] + weights[2] * tint[2];
        for (std::size_t row = 0; row < 3; ++row)
            for (std::size_t column = 0; column < 3; ++column)
                grade.data[4 * row + column] =
                    static_cast<float>(tint[row] / luma * weights[column]);
        return grade;
    }
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

// #504 Colorize. The operands are amount, the colour, shadows, midtones and highlights (each
// r, g, b, a), midpoint, repetitions, phase and the six weights (ColorizeKernel order). Each is
// checked against its document domain; only what the mode reads is written, so an unused value
// never changes the table or the cache identity. Tritone is the ramp over luma, clamped, once.
[[nodiscard]] std::optional<PreparedColorGrade>
prepareColorize(const ColorizeKernel& kernel, const std::span<const double> values) {
    // #541: a lowered gradient follows the fixed operands as its stop count, then (position, r,
    // g, b, a) per stop, then (#546) its interpolation, colour mode and hue direction.
    std::vector<document::GradientStop> stops;
    document::GradientModes modes;
    if (kernel.gradient) {
        if (values.size() <= kColorizeOperands)
            return std::nullopt;
        const double count = values[kColorizeOperands];
        if (!(count >= 1.0 && count <= static_cast<double>(document::kMaximumGradientStops)) ||
            count != std::floor(count) ||
            values.size() != kColorizeOperands + 1 + 5 * static_cast<std::size_t>(count) + 3)
            return std::nullopt;
        document::GradientValue gradient;
        for (std::size_t stop = 0; stop < static_cast<std::size_t>(count); ++stop) {
            const auto* v = &values[kColorizeOperands + 1 + 5 * stop];
            gradient.stops.push_back({v[0], core::Color4d{v[1], v[2], v[3], v[4]}});
        }
        const auto* word = &values[values.size() - 3];
        const auto closed = [](const double value, const double maximum) {
            return value >= 0.0 && value <= maximum && value == std::floor(value);
        };
        if (!closed(word[0], 5.0) || !closed(word[1], 2.0) || !closed(word[2], 3.0))
            return std::nullopt;
        gradient.modes = {static_cast<document::GradientInterpolation>(word[0]),
                          static_cast<document::GradientColorMode>(word[1]),
                          static_cast<document::GradientHueDirection>(word[2])};
        modes = document::effectiveGradientModes(gradient.modes);
        if (!gradient.isValid() ||
            !std::ranges::all_of(gradient.stops, [](const document::GradientStop& stop) {
                return std::abs(stop.color.red) <= document::kMaximumColorizeColor &&
                       std::abs(stop.color.green) <= document::kMaximumColorizeColor &&
                       std::abs(stop.color.blue) <= document::kMaximumColorizeColor;
            }))
            return std::nullopt;
        stops = document::sortedGradientStops(gradient);
    } else if (values.size() != kColorizeOperands) {
        return std::nullopt;
    }
    if (values.size() < kColorizeOperands ||
        static_cast<std::uint8_t>(kernel.mode) >
            static_cast<std::uint8_t>(ColorizeMode::BlackAndWhite) ||
        static_cast<std::uint8_t>(kernel.source) >
            static_cast<std::uint8_t>(ColorizeSource::Alpha) ||
        static_cast<std::uint8_t>(kernel.cycle) > static_cast<std::uint8_t>(ColorizeCycle::Mirror))
        return std::nullopt;
    const auto within = [&](const std::size_t index, const std::string_view schemaKey) {
        const auto domain = document::colorizeScalarDomain(schemaKey);
        return std::isfinite(values[index]) && values[index] >= domain->minimum &&
               values[index] <= domain->maximum;
    };
    if (!within(0, document::kColorizeAmountSchemaKey) ||
        !within(17, document::kColorizeMidpointSchemaKey) ||
        !within(18, document::kColorizeRepetitionsSchemaKey) ||
        !within(19, document::kColorizePhaseSchemaKey) ||
        !within(20, document::kColorizeRedsSchemaKey) ||
        !within(21, document::kColorizeYellowsSchemaKey) ||
        !within(22, document::kColorizeGreensSchemaKey) ||
        !within(23, document::kColorizeCyansSchemaKey) ||
        !within(24, document::kColorizeBluesSchemaKey) ||
        !within(25, document::kColorizeMagentasSchemaKey))
        return std::nullopt;
    for (std::size_t colour = 0; colour < 4; ++colour)
        for (std::size_t i = 0; i < 3; ++i) {
            const double value = values[1 + 4 * colour + i];
            if (!std::isfinite(value) || std::abs(value) > document::kMaximumColorizeColor)
                return std::nullopt;
        }
    const auto rgb = [&](const std::size_t colour, const std::size_t i) {
        return values[1 + 4 * colour + i];
    };
    PreparedColorGrade grade{ColorGradeKind::Colorize, std::vector<float>(kColorizeFloats, 0.0F),
                             false};
    auto& d = grade.data;
    d[1] = static_cast<float>(values[0]);
    d[12] = 0.2126F;
    d[13] = 0.7152F;
    d[14] = 0.0722F;
    const auto store = [&](const std::size_t vec, const std::size_t i, const double value) {
        d[4 * vec + i] = static_cast<float>(value);
    };
    switch (kernel.mode) {
    case ColorizeMode::Fill:
        d[0] = 0.0F;
        for (std::size_t i = 0; i < 3; ++i)
            store(4, i, rgb(0, i));
        break;
    case ColorizeMode::Tint:
        d[0] = 1.0F;
        for (std::size_t i = 0; i < 3; ++i) {
            store(4, i, rgb(1, i));
            store(5, i, rgb(3, i) - rgb(1, i));
        }
        break;
    case ColorizeMode::GradientMap:
        if (!stops.empty()) {
            // #541: table mode 4, the stops by position: [position, r, g, b] then
            // [1 / (next position - position) or 0, next colour - colour], each in binary32.
            d[0] = 4.0F;
            d[2] = static_cast<float>(kernel.source);
            d[3] = static_cast<float>(kernel.cycle);
            d[4] = static_cast<float>(values[18]);
            d[5] = static_cast<float>(values[19]);
            d[7] = static_cast<float>(stops.size());
            // #546: the blend as [interpolation, colour mode, hue, 0] [lead in, lead out, 0, 0]
            // where the fixed table's colours would be (Gradient Map reads none of them).
            d[16] = static_cast<float>(modes.interpolation);
            d[17] = static_cast<float>(modes.colorMode);
            d[18] = static_cast<float>(modes.hueDirection);
            if (document::gradientModesReachPastEnds(modes)) {
                const float first = static_cast<float>(stops.front().position);
                const float after = 1.0F - static_cast<float>(stops.back().position);
                const float in = first > 0.0F ? 1.0F / first : 0.0F;
                const float out = after > 0.0F ? 1.0F / after : 0.0F;
                d[20] = std::isfinite(in) ? in : 0.0F;
                d[21] = std::isfinite(out) ? out : 0.0F;
            }
            for (std::size_t index = 0; index < stops.size(); ++index) {
                const auto& stop = stops[index];
                const float position = static_cast<float>(stop.position);
                const std::array colour{static_cast<float>(stop.color.red),
                                        static_cast<float>(stop.color.green),
                                        static_cast<float>(stop.color.blue)};
                d.push_back(position);
                d.insert(d.end(), colour.begin(), colour.end());
                float inverse = 0.0F;
                std::array<float, 3> change{};
                if (index + 1 < stops.size()) {
                    const auto& next = stops[index + 1];
                    const float span = static_cast<float>(next.position) - position;
                    inverse = span > 0.0F ? 1.0F / span : 0.0F;
                    if (!std::isfinite(inverse))
                        inverse = 0.0F;
                    change = {static_cast<float>(next.color.red) - colour[0],
                              static_cast<float>(next.color.green) - colour[1],
                              static_cast<float>(next.color.blue) - colour[2]};
                }
                d.push_back(inverse);
                d.insert(d.end(), change.begin(), change.end());
            }
            break;
        }
        [[fallthrough]];
    case ColorizeMode::Tritone: {
        const bool map = kernel.mode == ColorizeMode::GradientMap;
        const double midpoint = values[17];
        d[0] = 2.0F;
        d[2] = map ? static_cast<float>(kernel.source) : 0.0F;
        d[3] = map ? static_cast<float>(kernel.cycle) : 0.0F;
        d[4] = map ? static_cast<float>(values[18]) : 1.0F;
        d[5] = map ? static_cast<float>(values[19]) : 0.0F;
        d[6] = static_cast<float>(midpoint);
        d[8] = static_cast<float>(1.0 / midpoint);
        d[9] = static_cast<float>(1.0 / (1.0 - midpoint));
        for (std::size_t i = 0; i < 3; ++i) {
            store(4, i, rgb(1, i));
            store(5, i, rgb(2, i) - rgb(1, i));
            store(6, i, rgb(2, i));
            store(7, i, rgb(3, i) - rgb(2, i));
        }
        break;
    }
    case ColorizeMode::BlackAndWhite:
        d[0] = 3.0F;
        // Primary weights by the largest channel, secondary by the smallest.
        store(8, 0, values[20]);
        store(8, 1, values[22]);
        store(8, 2, values[24]);
        store(9, 0, values[23]);
        store(9, 1, values[25]);
        store(9, 2, values[21]);
        break;
    }
    // A zero amount keeps the original colour everywhere.
    grade.identity = d[1] == 0.0F;
    return grade;
}

// One premultiplied pixel: its straight colour is colorized and mixed back by the amount, the
// alpha kept. Alpha 0 has no colour and passes unchanged.
[[nodiscard]] bool colorizePixel(const std::vector<float>& k, std::array<float, 4>& pixel) noexcept {
    const auto finite = [](const float x) { return std::isfinite(x); };
    const float a = pixel[3];
    if (a == 0.0F)
        return true;
    std::array<float, 3> s{};
    for (std::size_t i = 0; i < 3; ++i) {
        s[i] = pixel[i] / a;
        if (!finite(s[i]))
            return false;
    }
    const float luma = std::fma(k[14], s[2], std::fma(k[13], s[1], k[12] * s[0]));
    const auto mode = static_cast<int>(k[0]);
    std::array<float, 3> c{};
    if (mode == 0) {
        c = {k[16], k[17], k[18]};
    } else if (mode == 1) {
        for (std::size_t i = 0; i < 3; ++i)
            c[i] = std::fma(luma, k[20 + i], k[16 + i]);
    } else if (mode == 2 || mode == 4) {
        const auto source = static_cast<int>(k[2]);
        const float x = source == 0   ? luma
                        : source == 4 ? a
                                      : s[static_cast<std::size_t>(source - 1)];
        float t = std::fma(x, k[4], k[5]);
        if (!finite(t))
            return false;
        const auto cycle = static_cast<int>(k[3]);
        if (cycle == 0) {
            t = std::min(std::max(t, 0.0F), 1.0F);
        } else if (cycle == 1) {
            t = t - std::floor(t);
        } else {
            const float u = t - 2.0F * std::floor(t * 0.5F);
            t = u > 1.0F ? 2.0F - u : u;
        }
        if (mode == 4) {
            // #541: the last stop at or before t, interpolated towards the next one.
            const auto n = static_cast<std::size_t>(k[7]);
            const auto stop = [&](const std::size_t index) {
                return &k[kColorizeFloats + kColorizeStopFloats * index];
            };
            if (k[16] != 0.0F || k[17] != 0.0F) {
                // #546: any other mode than Linear in RGB, as native Colorize does.
                const auto sampled = document::gradient_math::sample<float>(
                    n,
                    [&](const std::size_t index) {
                        const float* at = stop(index);
                        return document::gradient_math::Stop<float>{
                            at[0], at[4], {at[1], at[2], at[3], 1.0F}};
                    },
                    k[20], k[21], t,
                    document::GradientModes{static_cast<document::GradientInterpolation>(k[16]),
                                            static_cast<document::GradientColorMode>(k[17]),
                                            static_cast<document::GradientHueDirection>(k[18])});
                c = {sampled[0], sampled[1], sampled[2]};
            } else if (!(t > stop(0)[0])) {
                c = {stop(0)[1], stop(0)[2], stop(0)[3]};
            } else if (t >= stop(n - 1)[0]) {
                c = {stop(n - 1)[1], stop(n - 1)[2], stop(n - 1)[3]};
            } else {
                std::size_t index = 0;
                while (index + 2 < n && stop(index + 1)[0] <= t)
                    ++index;
                const float* from = stop(index);
                const float u = (t - from[0]) * from[4];
                for (std::size_t i = 0; i < 3; ++i)
                    c[i] = std::fma(u, from[5 + i], from[1 + i]);
            }
        } else if (t <= k[6]) {
            const float u = t * k[8];
            for (std::size_t i = 0; i < 3; ++i)
                c[i] = std::fma(u, k[20 + i], k[16 + i]);
        } else {
            const float u = (t - k[6]) * k[9];
            for (std::size_t i = 0; i < 3; ++i)
                c[i] = std::fma(u, k[28 + i], k[24 + i]);
        }
    } else {
        const std::size_t largest = s[0] >= s[1] && s[0] >= s[2] ? 0 : s[1] >= s[2] ? 1 : 2;
        const std::size_t smallest = s[0] <= s[1] && s[0] <= s[2] ? 0 : s[1] <= s[2] ? 1 : 2;
        const float high = s[largest];
        const float low = s[smallest];
        const float middle = std::max(std::min(s[0], s[1]), std::min(std::max(s[0], s[1]), s[2]));
        const float grey =
            std::fma(high - middle, k[32 + largest], std::fma(middle - low, k[36 + smallest], low));
        c = {grey, grey, grey};
    }
    if (k[1] != 1.0F)
        for (std::size_t i = 0; i < 3; ++i)
            c[i] = std::fma(k[1], c[i] - s[i], s[i]);
    if (!finite(c[0]) || !finite(c[1]) || !finite(c[2]))
        return false;
    pixel = {c[0] * a, c[1] * a, c[2] * a, a};
    return finite(pixel[0]) && finite(pixel[1]) && finite(pixel[2]);
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

// The table's closed fields are whole numbers in range and the ramp's midpoint is inside (0, 1).
[[nodiscard]] bool validColorizeTable(const std::vector<float>& d) noexcept {
    const auto closed = [](const float value, const float maximum) {
        return value >= 0.0F && value <= maximum && value == std::floor(value);
    };
    if (d.size() < kColorizeFloats || !closed(d[0], 4.0F) || !closed(d[2], 4.0F) ||
        !closed(d[3], 2.0F))
        return false;
    // #541: table mode 4 carries one to 64 stops after the fixed table.
    if (d[0] == 4.0F)
        return closed(d[7], static_cast<float>(document::kMaximumGradientStops)) && d[7] >= 1.0F &&
               closed(d[16], 5.0F) && closed(d[17], 2.0F) && closed(d[18], 3.0F) && d[20] >= 0.0F &&
               d[21] >= 0.0F &&
               d.size() == kColorizeFloats + kColorizeStopFloats * static_cast<std::size_t>(d[7]);
    return d.size() == kColorizeFloats && (d[0] != 2.0F || (d[6] > 0.0F && d[6] < 1.0F));
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
    case ColorGradeKind::Colorize: {
        // The same binary32 operations as colorizePixel: the wrapper's correctly rounded division
        // and premultiply, precise fused multiply-adds, exact min/max/floor and the same
        // channel-order ties. A non-finite colour or position is refused like the CPU's.
        static const std::string text =
            "layout(set = 0, binding = 0, std430) readonly buffer BloomColorizeData { vec4 "
            "values[]; } bloom_colorize_data;\n" +
            straight + finite +
            // #546: the Gradient Map's stops for the shared mode functions: [position, colour]
            // then [reciprocal span, colour change].
            "struct BloomGradientStop { float position; float inverseSpan; vec4 color; };\n"
            "BloomGradientStop bloom_gradient_stop(int index) {\n"
            " vec4 a = bloom_colorize_data.values[10 + 2 * index];\n"
            " vec4 b = bloom_colorize_data.values[11 + 2 * index];\n"
            " return BloomGradientStop(a.x, b.x, vec4(a.yzw, 1.0));\n"
            "}\n" +
            std::string(render::kGradientModesGlsl) +
            "vec4 bloom_colorize(ivec2 c) {\n"
            " vec4 p = imageLoad(bloom_ocio_input, c);\n"
            " float a = p.a;\n"
            " if (a == 0.0) { return p; }\n"
            " vec4 k = bloom_colorize_data.values[0];\n"
            " vec4 ramp = bloom_colorize_data.values[1];\n"
            " vec4 inv = bloom_colorize_data.values[2];\n"
            " vec4 w = bloom_colorize_data.values[3];\n"
            " vec4 c0 = bloom_colorize_data.values[4];\n"
            " vec4 c1 = bloom_colorize_data.values[5];\n"
            " vec4 c2 = bloom_colorize_data.values[6];\n"
            " vec4 c3 = bloom_colorize_data.values[7];\n"
            " vec4 wp = bloom_colorize_data.values[8];\n"
            " vec4 ws = bloom_colorize_data.values[9];\n"
            " vec3 s = vec3(bloom_ocio_cpu_div(p.r, a), bloom_ocio_cpu_div(p.g, a), "
            "bloom_ocio_cpu_div(p.b, a));\n"
            " for (int i = 0; i < 3; ++i) {\n"
            "  if (!bloom_grade_finite(s[i])) {" +
            failure +
            "  }\n"
            " }\n"
            " precise float luma = fma(w.z, s.z, fma(w.y, s.y, w.x * s.x));\n"
            " int mode = int(k.x);\n"
            " vec3 o = vec3(0.0);\n"
            " if (mode == 0) {\n"
            "  o = c0.rgb;\n"
            " } else if (mode == 1) {\n"
            "  precise vec3 r = vec3(fma(luma, c1.x, c0.x), fma(luma, c1.y, c0.y), "
            "fma(luma, c1.z, c0.z));\n"
            "  o = r;\n"
            " } else if (mode == 2 || mode == 4) {\n"
            "  int source = int(k.z);\n"
            "  float x = source == 0 ? luma : (source == 4 ? a : s[source - 1]);\n"
            "  precise float t = fma(x, ramp.x, ramp.y);\n"
            "  if (!bloom_grade_finite(t)) {" +
            failure +
            "  }\n"
            "  int cycle = int(k.w);\n"
            "  if (cycle == 0) {\n"
            "   t = min(max(t, 0.0), 1.0);\n"
            "  } else if (cycle == 1) {\n"
            "   precise float f = t - floor(t);\n"
            "   t = f;\n"
            "  } else {\n"
            "   precise float u = t - 2.0 * floor(t * 0.5);\n"
            "   t = u > 1.0 ? 2.0 - u : u;\n"
            "  }\n"
            "  if (mode == 4) {\n"
            "   int n = int(ramp.w);\n"
            "   vec4 first = bloom_colorize_data.values[10];\n"
            "   vec4 last = bloom_colorize_data.values[10 + 2 * (n - 1)];\n"
            "   if (c0.x != 0.0 || c0.y != 0.0) {\n"
            "    o = bloom_gradient_sample(n, t, c1.x, c1.y, int(c0.x), int(c0.y), "
            "int(c0.z)).rgb;\n"
            "   } else if (!(t > first.x)) {\n"
            "    o = first.yzw;\n"
            "   } else if (t >= last.x) {\n"
            "    o = last.yzw;\n"
            "   } else {\n"
            "    int i = 0;\n"
            "    for (int j = 0; j < 64; ++j) {\n"
            "     if (i + 2 >= n || !(bloom_colorize_data.values[10 + 2 * (i + 1)].x <= t)) {\n"
            "      break;\n"
            "     }\n"
            "     ++i;\n"
            "    }\n"
            "    vec4 a0 = bloom_colorize_data.values[10 + 2 * i];\n"
            "    vec4 a1 = bloom_colorize_data.values[11 + 2 * i];\n"
            "    precise float u = (t - a0.x) * a1.x;\n"
            "    precise vec3 r = vec3(fma(u, a1.y, a0.y), fma(u, a1.z, a0.z), fma(u, a1.w, "
            "a0.w));\n"
            "    o = r;\n"
            "   }\n"
            "  } else if (t <= ramp.z) {\n"
            "   precise float u = t * inv.x;\n"
            "   precise vec3 r = vec3(fma(u, c1.x, c0.x), fma(u, c1.y, c0.y), fma(u, c1.z, "
            "c0.z));\n"
            "   o = r;\n"
            "  } else {\n"
            "   precise float u = (t - ramp.z) * inv.y;\n"
            "   precise vec3 r = vec3(fma(u, c3.x, c2.x), fma(u, c3.y, c2.y), fma(u, c3.z, "
            "c2.z));\n"
            "   o = r;\n"
            "  }\n"
            " } else {\n"
            "  int hi = (s.x >= s.y && s.x >= s.z) ? 0 : (s.y >= s.z ? 1 : 2);\n"
            "  int lo = (s.x <= s.y && s.x <= s.z) ? 0 : (s.y <= s.z ? 1 : 2);\n"
            "  float mid = max(min(s.x, s.y), min(max(s.x, s.y), s.z));\n"
            "  precise float g = fma(s[hi] - mid, wp[hi], fma(mid - s[lo], ws[lo], s[lo]));\n"
            "  o = vec3(g);\n"
            " }\n"
            " if (k.y != 1.0) {\n"
            "  precise vec3 r = vec3(fma(k.y, o.x - s.x, s.x), fma(k.y, o.y - s.y, s.y), "
            "fma(k.y, o.z - s.z, s.z));\n"
            "  o = r;\n"
            " }\n"
            " for (int i = 0; i < 3; ++i) {\n"
            "  if (!bloom_grade_finite(o[i])) {" +
            failure +
            "  }\n"
            " }\n"
            " return vec4(bloom_ocio_cpu_premul(o, a), a);\n"
            "}\n";
        return text;
    }
    case ColorGradeKind::Vignette:
        return vignetteProgramText();
    case ColorGradeKind::FieldRaster:
        return fieldRasterProgramText();
    case ColorGradeKind::DepthMap:
        return depthMapProgramText();
    case ColorGradeKind::Defocus:
        return defocusProgramText();
    case ColorGradeKind::LensFlare:
        return lensFlareProgramText();
    case ColorGradeKind::LensFlareProbe:
        return lensFlareProbeProgramText();
    case ColorGradeKind::FilmGrain:
        return filmGrainProgramText();
    case ColorGradeKind::Dither:
        return ditherProgramText();
    case ColorGradeKind::CodecDegrade:
        return codecDegradeProgramText();
    case ColorGradeKind::AutoExposure:
        return autoExposureProgramText();
    case ColorGradeKind::Motion:
        return {}; // One program per pass: the caller asks with the pass (motionProgramText).
    case ColorGradeKind::PixelMath:
        return pixelMathProgramText();
    case ColorGradeKind::Histogram:
        return histogramProgramText();
    case ColorGradeKind::Statistics:
        return statisticsProgramText();
    case ColorGradeKind::AudioPlot:
        return audioPlotProgramText();
    case ColorGradeKind::ColorTool:
        return colorToolProgramText();
    case ColorGradeKind::Sharpen: {
        // ImageCombineV1's Unsharp mode; buildColorGradeProgram refuses this kind.
        static const std::string none;
        return none;
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
            " float mx = max(p.r, max(p.g, p.b));\n"
            " float mn = min(p.r, min(p.g, p.b));\n"
            " float v = bloom_hue_saturation_data.values[3].x;\n"
            " if (v != 0.0) {\n"
            "  precise float y = fma(0.072, o.b, fma(0.715, o.g, 0.213 * o.r));\n"
            "  precise float f = fma(v, 1.0 - clamp(mx - mn, 0.0, 1.0), 1.0);\n"
            "  o = vec3(fma(f, o.r - y, y), fma(f, o.g - y, y), fma(f, o.b - y, y));\n"
            " }\n"
            " return vec4(o, p.a);\n"
            "}\n";
        return text;
    }
    }
    return {};
}
} // namespace

namespace {
std::optional<std::vector<double>>
sampleOperandsImpl(const ImageEffectKernel& kernel, const CompiledCompositionPlan& plan,
                   const ResolvedEvaluation& resolved, const std::optional<core::RationalTime> time,
                   const bool operandsOnly) {
    // #484: a motion kernel's operands include the shutter's sub-frame samples.
    if (isMotionKernel(kernel))
        return sampleMotionOperands(kernel, plan, resolved, time);
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
            values.push_back(static_cast<double>(sampled->firstContour().anchors.size()));
            for (const auto& anchor : sampled->firstContour().anchors) {
                values.push_back(anchor.point.x);
                values.push_back(anchor.point.y);
            }
        } else if constexpr (std::is_same_v<Parameter, CompiledGradientParameter>) {
            // #541: a gradient's stop count, then each stop's position and straight colour.
            std::optional<document::GradientValue> sampled;
            if (const auto* constant = std::get_if<document::GradientValue>(&parameter.source))
                sampled = *constant;
            else if (time)
                sampled =
                    sampleGradientCurve(std::get<CompiledGradientCurve>(parameter.source), *time);
            // #543: then the stops its value nodes drive this frame.
            sampled = detail::applyGradientStopDrivers(
                std::move(sampled), parameter.drivers,
                [&](const CompiledGradientStopDriver& driver) -> const CompiledValue* {
                    const auto index = driver.value.value();
                    return index < resolved.valueOutputs.size() ? &resolved.valueOutputs[index]
                                                                : nullptr;
                });
            if (!sampled) {
                evaluable = false;
                return;
            }
            values.push_back(static_cast<double>(sampled->stops.size()));
            for (const auto& stop : sampled->stops) {
                values.push_back(stop.position);
                values.push_back(stop.color.red);
                values.push_back(stop.color.green);
                values.push_back(stop.color.blue);
                values.push_back(stop.color.alpha);
            }
            // #546: then its interpolation, colour mode and hue direction.
            values.push_back(static_cast<double>(sampled->modes.interpolation));
            values.push_back(static_cast<double>(sampled->modes.colorMode));
            values.push_back(static_cast<double>(sampled->modes.hueDirection));
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
    // #347: then the prepared analysis the pass draws, which a worker prepared for this exact time.
    if (const auto* plot = std::get_if<AudioPlotKernel>(&kernel); plot != nullptr && !operandsOnly) {
        if (!time || !appendAudioPlotAnalysis(*plot, values, *time))
            return std::nullopt;
    }
    // #525: a field program then reads the clock time itself (a looping evolution, a pattern's
    // speed), in exact seconds converted once here.
    if (std::holds_alternative<FieldRasterKernel>(kernel)) {
        if (!time)
            return std::nullopt;
        values.push_back(time->toSeconds());
    }
    // #563: a Codec Degrade step reads the frame, to place itself in its group of pictures.
    if (const auto* codec = std::get_if<CodecDegradeKernel>(&kernel);
        codec != nullptr && codec->role == CodecDegradeRole::Step) {
        if (!time)
            return std::nullopt;
        const auto frame = valueGraphFrameIndex(*time, plan.format().frameRate());
        if (!frame)
            return std::nullopt;
        values.push_back(static_cast<double>(*frame));
    }
    // #566: the filter converts its lags in seconds to per-frame coefficients with the length of a
    // frame of the composition, in seconds.
    if (const auto* autoExposure = std::get_if<AutoExposureKernel>(&kernel);
        autoExposure != nullptr && autoExposure->role == AutoExposureRole::Filter) {
        const auto rate = plan.format().frameRate();
        if (rate.numerator() == 0)
            return std::nullopt;
        values.push_back(static_cast<double>(rate.denominator()) /
                         static_cast<double>(rate.numerator()));
    }
    // #486: Film Grain then reads the frame it is drawn for, by the same exact arithmetic as a Time
    // node's frame output, so one instant always names one frame's grain. Held grain reads none.
    if (const auto* grain = std::get_if<FilmGrainKernel>(&kernel)) {
        if (!grain->drawsPerFrame()) {
            values.push_back(0.0);
        } else {
            if (!time)
                return std::nullopt;
            const auto frame = valueGraphFrameIndex(*time, plan.format().frameRate());
            if (!frame)
                return std::nullopt;
            values.push_back(static_cast<double>(*frame));
        }
    }
    // #527: Dither reads its frame the same way; Held reads none.
    if (const auto* dither = std::get_if<DitherKernel>(&kernel)) {
        if (!dither->drawsPerFrame()) {
            values.push_back(0.0);
        } else {
            if (!time)
                return std::nullopt;
            const auto frame = valueGraphFrameIndex(*time, plan.format().frameRate());
            if (!frame)
                return std::nullopt;
            values.push_back(static_cast<double>(*frame));
        }
    }
    return values;
}
} // namespace

std::optional<std::vector<double>>
sampleColorGradeOperands(const ImageEffectKernel& kernel, const CompiledCompositionPlan& plan,
                         const ResolvedEvaluation& resolved,
                         const std::optional<core::RationalTime> time) {
    return sampleOperandsImpl(kernel, plan, resolved, time, false);
}

std::optional<std::vector<double>>
sampleColorGradeOperandsOnly(const ImageEffectKernel& kernel, const CompiledCompositionPlan& plan,
                             const ResolvedEvaluation& resolved,
                             const std::optional<core::RationalTime> time) {
    return sampleOperandsImpl(kernel, plan, resolved, time, true);
}

std::optional<PreparedColorGrade> prepareColorGrade(const ImageEffectKernel& kernel,
                                                    const std::span<const double> values) {
    if (const auto* levels = std::get_if<LevelsKernel>(&kernel))
        return prepareLevels(*levels, values);
    if (const auto* curves = std::get_if<CurvesKernel>(&kernel))
        return prepareCurvesGrade(curves->clamp, values);
    if (const auto* hueSaturation = std::get_if<HueSaturationKernel>(&kernel))
        return prepareHueSaturation(*hueSaturation, values);
    if (std::holds_alternative<GradeKernel>(kernel))
        return prepareGrade(values);
    if (std::holds_alternative<HighlightRollOffKernel>(kernel))
        return prepareRollOff(values);
    if (const auto* association = std::get_if<AlphaAssociationKernel>(&kernel))
        return prepareAlphaAssociation(*association, values);
    if (const auto* colorize = std::get_if<ColorizeKernel>(&kernel))
        return prepareColorize(*colorize, values);
    if (const auto* vignette = std::get_if<VignetteKernel>(&kernel))
        return prepareVignetteGrade(*vignette, values);
    if (const auto* field = std::get_if<FieldRasterKernel>(&kernel))
        return prepareFieldRasterGrade(*field, values);
    if (const auto* depth = std::get_if<DepthMapKernel>(&kernel))
        return prepareDepthMapGrade(*depth, values);
    if (const auto* defocus = std::get_if<DefocusKernel>(&kernel))
        return prepareDefocusGrade(*defocus, values);
    if (const auto* flare = std::get_if<LensFlareKernel>(&kernel))
        return prepareLensFlareGrade(*flare, values);
    if (const auto* grain = std::get_if<FilmGrainKernel>(&kernel))
        return prepareFilmGrainGrade(*grain, values);
    if (const auto* dither = std::get_if<DitherKernel>(&kernel))
        return prepareDitherGrade(*dither, values);
    if (const auto* codec = std::get_if<CodecDegradeKernel>(&kernel))
        return prepareCodecDegradeGrade(*codec, values);
    if (const auto* autoExposure = std::get_if<AutoExposureKernel>(&kernel))
        return prepareAutoExposureGrade(*autoExposure, values);
    if (isMotionKernel(kernel))
        return prepareMotionGrade(kernel, values);
    if (const auto* pixelMath = std::get_if<PixelMathKernel>(&kernel))
        return preparePixelMathGrade(*pixelMath, values);
    if (const auto* tool = std::get_if<ColorToolKernel>(&kernel))
        return prepareColorToolGrade(*tool, values);
    if (const auto* histogram = std::get_if<HistogramKernel>(&kernel))
        return prepareHistogramGrade(*histogram, values);
    if (const auto* statistics = std::get_if<StatisticsKernel>(&kernel))
        return values.empty() ? prepareStatisticsGrade(*statistics) : std::nullopt;
    if (const auto* plot = std::get_if<AudioPlotKernel>(&kernel))
        return prepareAudioPlotGrade(*plot, values);
    if (const auto* sharpen = std::get_if<SharpenKernel>(&kernel))
        return prepareSharpenGrade(*sharpen, values);
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
        const float vibrance = grade.data[12];
        if (vibrance != 0.0F) {
            const float chroma = std::clamp(std::max(pixel[0], std::max(pixel[1], pixel[2])) -
                                                std::min(pixel[0], std::min(pixel[1], pixel[2])),
                                            0.0F, 1.0F);
            const float y = std::fma(0.072F, out[2], std::fma(0.715F, out[1], 0.213F * out[0]));
            const float f = std::fma(vibrance, 1.0F - chroma, 1.0F);
            for (auto& channel : out) {
                channel = std::fma(f, channel - y, y);
                if (!finite(channel))
                    return false;
            }
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
    if (grade.kind == ColorGradeKind::Colorize)
        return colorizePixel(grade.data, pixel);
    if (grade.kind == ColorGradeKind::ColorTool)
        return colorToolPixel(grade.data, pixel);
    // #505: a vignette depends on where the pixel is (vignettePixel); #525 a field raster replaces
    // every pixel (applyFieldRaster).
    if (grade.kind == ColorGradeKind::Vignette || grade.kind == ColorGradeKind::FieldRaster ||
        grade.kind == ColorGradeKind::DepthMap || grade.kind == ColorGradeKind::Defocus ||
        grade.kind == ColorGradeKind::LensFlare || grade.kind == ColorGradeKind::LensFlareProbe ||
        grade.kind == ColorGradeKind::FilmGrain || grade.kind == ColorGradeKind::Dither ||
        grade.kind == ColorGradeKind::CodecDegrade ||
        grade.kind == ColorGradeKind::AutoExposure || grade.kind == ColorGradeKind::Motion ||
        grade.kind == ColorGradeKind::Sharpen || grade.kind == ColorGradeKind::PixelMath ||
        grade.kind == ColorGradeKind::Histogram || grade.kind == ColorGradeKind::AudioPlot ||
        grade.kind == ColorGradeKind::Statistics)
        return false;
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
                                  const EvaluationProgressCallback& progress,
                                  const double horizontalScale, const double verticalScale) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    // #505: a vignette's table gains the input's geometry; every pixel reads its own index.
    std::optional<std::vector<float>> vignette;
    if (grade.kind == ColorGradeKind::Vignette) {
        const auto& descriptor = *input.descriptor();
        const auto aspect = descriptor.pixelAspect();
        vignette = vignetteProgramData(
            grade, descriptor.displayWindow(), descriptor.dataWindow().originX(),
            descriptor.dataWindow().originY(),
            static_cast<double>(aspect.numerator()) / static_cast<double>(aspect.denominator()),
            horizontalScale, verticalScale);
        if (!vignette)
            return failed(EvaluationDiagnosticCode::InvalidParameter,
                          "Vignette geometry cannot be represented");
    }
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
            if (vignette ? !vignettePixel(*vignette, static_cast<std::int64_t>(column),
                                          y - window.originY(), graded)
                         : !colorGradePixel(grade, graded))
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
                        : kind == ColorGradeKind::Colorize ? validColorizeTable(data)
                        : kind == ColorGradeKind::Vignette ? validVignetteProgramData(data)
                        : kind == ColorGradeKind::FieldRaster
                            ? validFieldProgramData(data)
                        : kind == ColorGradeKind::DepthMap ? validDepthMapProgramData(data)
                        : kind == ColorGradeKind::Defocus  ? validDefocusProgramData(data)
                        : kind == ColorGradeKind::LensFlare ? validLensFlareProgramData(data)
                        : kind == ColorGradeKind::LensFlareProbe
                            ? validLensFlareProbeData(data)
                        : kind == ColorGradeKind::FilmGrain ? validFilmGrainProgramData(data)
                        : kind == ColorGradeKind::Dither ? validDitherProgramData(data)
                        : kind == ColorGradeKind::CodecDegrade
                            ? validCodecDegradeProgramData(data)
                        : kind == ColorGradeKind::AutoExposure
                            ? validAutoExposureProgramData(data)
                        : kind == ColorGradeKind::Motion ? validMotionProgramData(data)
                        : kind == ColorGradeKind::PixelMath ? validPixelMathProgramData(data)
                        : kind == ColorGradeKind::Histogram ? validHistogramProgramData(data)
                        : kind == ColorGradeKind::Statistics ? validStatisticsProgramData(data)
                        : kind == ColorGradeKind::AudioPlot ? validAudioPlotProgramData(data)
                        : kind == ColorGradeKind::ColorTool ? validColorToolProgramData(data)
                            : data.size() == kHueSaturationFloats;
    if ((static_cast<std::uint8_t>(kind) > static_cast<std::uint8_t>(ColorGradeKind::Motion) &&
         kind != ColorGradeKind::PixelMath && kind != ColorGradeKind::Histogram &&
         kind != ColorGradeKind::Statistics && kind != ColorGradeKind::AudioPlot &&
         kind != ColorGradeKind::ColorTool && kind != ColorGradeKind::Dither) ||
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
        program.semanticsId = "bloom.render.hue-saturation.v2";
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
    case ColorGradeKind::Colorize:
        program.functionName = "bloom_colorize";
        program.resourcePrefix = "bloom_colorize_";
        program.semanticsId = "bloom.render.colorize.v1";
        break;
    case ColorGradeKind::Vignette:
        program.functionName = "bloom_vignette";
        program.resourcePrefix = "bloom_vignette_";
        program.semanticsId = "bloom.render.vignette.v1";
        break;
    case ColorGradeKind::FieldRaster:
        program.functionName = "bloom_field";
        program.resourcePrefix = "bloom_field_";
        program.semanticsId = "bloom.render.field-raster.v1";
        break;
    case ColorGradeKind::DepthMap:
        program.functionName = "bloom_depth_map";
        program.resourcePrefix = "bloom_depth_map_";
        program.semanticsId = "bloom.render.depth-map.v1";
        break;
    case ColorGradeKind::Defocus:
        program.functionName = "bloom_defocus";
        program.resourcePrefix = "bloom_defocus_";
        program.semanticsId = "bloom.render.z-defocus.v1";
        break;
    case ColorGradeKind::LensFlare:
        program.functionName = "bloom_lens_flare";
        program.resourcePrefix = "bloom_lens_flare_";
        program.semanticsId = "bloom.render.lens-flare.v1";
        break;
    case ColorGradeKind::LensFlareProbe:
        program.functionName = "bloom_lens_flare_probe";
        program.resourcePrefix = "bloom_lens_flare_probe_";
        program.semanticsId = "bloom.render.lens-flare-probe.v1";
        break;
    case ColorGradeKind::FilmGrain:
        program.functionName = "bloom_film_grain";
        program.resourcePrefix = "bloom_film_grain_";
        program.semanticsId = "bloom.render.film-grain.v1";
        break;
    case ColorGradeKind::CodecDegrade:
        program.functionName = "bloom_codec_degrade";
        program.resourcePrefix = "bloom_codec_degrade_";
        program.semanticsId = "bloom.render.codec-degrade.v1";
        break;
    case ColorGradeKind::AutoExposure:
        program.functionName = "bloom_auto_exposure";
        program.resourcePrefix = "bloom_auto_exposure_";
        program.semanticsId = "bloom.render.auto-exposure.v1";
        break;
    case ColorGradeKind::Motion:
        program.functionName = "bloom_motion";
        program.resourcePrefix = "bloom_motion_";
        program.semanticsId = "bloom.render.motion-blur.v1";
        break;
    case ColorGradeKind::Sharpen:
        break; // Refused above.
    case ColorGradeKind::PixelMath:
        program.functionName = "bloom_pixel_math";
        program.resourcePrefix = "bloom_pixel_math_";
        program.semanticsId = "bloom.render.pixel-math.v1";
        break;
    case ColorGradeKind::Histogram:
        program.functionName = "bloom_histogram";
        program.resourcePrefix = "bloom_histogram_";
        program.semanticsId = "bloom.render.histogram.v2";
        break;
    case ColorGradeKind::Statistics:
        program.functionName = "bloom_statistics";
        program.resourcePrefix = "bloom_statistics_";
        program.semanticsId = "bloom.render.statistics.v1";
        break;
    case ColorGradeKind::Dither:
        program.functionName = "bloom_dither";
        program.resourcePrefix = "bloom_dither_";
        program.semanticsId = "bloom.render.dither.v2";
        break;
    case ColorGradeKind::ColorTool:
        program.functionName = "bloom_color_tool";
        program.resourcePrefix = "bloom_color_tool_";
        program.semanticsId = "bloom.render.color-tool.v1";
        break;
    case ColorGradeKind::AudioPlot:
        program.functionName = "bloom_audio_plot";
        program.resourcePrefix = "bloom_audio_plot_";
        program.semanticsId = "bloom.render.audio-plot.v1";
        break;
    }
    program.shaderText = kind == ColorGradeKind::Statistics
                             ? statisticsProgramText()
                         : kind == ColorGradeKind::AudioPlot
                             ? audioPlotProgramText()
                         : kind == ColorGradeKind::Histogram
                             ? histogramProgramText()
                         : kind == ColorGradeKind::PixelMath
                             ? pixelMathProgramText()
                         : kind == ColorGradeKind::Motion
                             ? motionProgramText(static_cast<int>(data[0]))
                             : glslProgramText(kind);
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
