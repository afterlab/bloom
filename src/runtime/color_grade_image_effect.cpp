#include "color_grade_image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "cpu_composition_resolution.hpp"
#include "image_effect.hpp"
#include "operation_key.hpp"

#include <bloom/runtime/animation_sampling.hpp>

#include <bloom/document/color_grade_nodes.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <string>
#include <utility>

namespace bloom::runtime::detail {
namespace {
using document::kGradeChannelCount;

constexpr std::size_t kLevelsChannelFloats = 8;
constexpr std::size_t kLevelsFloats = 4 + kGradeChannelCount * kLevelsChannelFloats;
constexpr std::size_t kHueSaturationFloats = 12;
static_assert(kLevelsChannelFloats % 4 == 0);

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
    const double radians = std::fmod(hue, 360.0) * (std::numbers::pi / 180.0);
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    // Filter Effects Module Level 1, feColorMatrix hueRotate and saturate.
    const std::array<std::array<double, 3>, 3> rotate{{
        {kLumaRed + cosine * (1.0 - kLumaRed) - sine * kLumaRed,
         kLumaGreen - cosine * kLumaGreen - sine * kLumaGreen,
         kLumaBlue - cosine * kLumaBlue + sine * (1.0 - kLumaBlue)},
        {kLumaRed - cosine * kLumaRed + sine * 0.143,
         kLumaGreen + cosine * (1.0 - kLumaGreen) + sine * 0.140,
         kLumaBlue - cosine * kLumaBlue - sine * 0.283},
        {kLumaRed - cosine * kLumaRed - sine * (1.0 - kLumaRed),
         kLumaGreen - cosine * kLumaGreen + sine * kLumaGreen,
         kLumaBlue + cosine * (1.0 - kLumaBlue) + sine * kLumaBlue},
    }};
    const std::array<std::array<double, 3>, 3> saturate{{
        {kLumaRed + (1.0 - kLumaRed) * s, kLumaGreen - kLumaGreen * s, kLumaBlue - kLumaBlue * s},
        {kLumaRed - kLumaRed * s, kLumaGreen + (1.0 - kLumaGreen) * s, kLumaBlue - kLumaBlue * s},
        {kLumaRed - kLumaRed * s, kLumaGreen - kLumaGreen * s, kLumaBlue + (1.0 - kLumaBlue) * s},
    }};
    // Hue first, then saturation: M = saturate * rotate.
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t column = 0; column < 3; ++column) {
            double sum = 0.0;
            for (std::size_t k = 0; k < 3; ++k)
                sum += saturate[row][k] * rotate[k][column];
            grade.data[4 * row + column] = static_cast<float>(sum);
        }
    return grade;
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
                                                         : data.size() == kHueSaturationFloats;
    if (static_cast<std::uint8_t>(kind) >
            static_cast<std::uint8_t>(ColorGradeKind::HueSaturation) ||
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
