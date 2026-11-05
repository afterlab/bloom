// #505 Vignette: a grade kind whose table gains the input's geometry where it is applied. The CPU
// reference (vignettePixel) and the GPU program (vignetteProgramText) run the same binary32
// operations over the same 24 floats; only sqrt, pow and exp2 are not correctly rounded on every
// device, so the two agree to a few units in the last place.

#include "color_grade_image_effect.hpp"

#include <bloom/document/vignette_nodes.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace bloom::runtime::detail {
namespace {

// The operands: amount, exposure, midpoint, roundness, feather, highlight protection, centre x, y.
constexpr std::size_t kVignetteOperands = 8;
// Rec. 709 luma, the weights of the track-matte Luma, Luma Key and the Glow roll-off.
constexpr float kLumaRed = 0.2126F;
constexpr float kLumaGreen = 0.7152F;
constexpr float kLumaBlue = 0.0722F;

[[nodiscard]] bool within(const std::string_view schemaKey, const double value) noexcept {
    const auto domain = document::vignetteScalarDomain(schemaKey);
    return domain && std::isfinite(value) && value >= domain->minimum && value <= domain->maximum;
}

// a^b for a >= 0 and b > 0, with an exact zero for a zero base on every device.
[[nodiscard]] float power(const float a, const float b) noexcept {
    return a > 0.0F ? std::pow(a, b) : 0.0F;
}

} // namespace

std::optional<PreparedColorGrade> prepareVignetteGrade(const VignetteKernel& kernel,
                                                       const std::span<const double> values) {
    if (values.size() != kVignetteOperands ||
        static_cast<std::uint8_t>(kernel.shape) >
            static_cast<std::uint8_t>(VignetteShape::Rectangular) ||
        static_cast<std::uint8_t>(kernel.mode) > static_cast<std::uint8_t>(VignetteMode::Exposure))
        return std::nullopt;
    if (!within(document::kVignetteAmountSchemaKey, values[0]) ||
        !within(document::kVignetteExposureSchemaKey, values[1]) ||
        !within(document::kVignetteMidpointSchemaKey, values[2]) ||
        !within(document::kVignetteRoundnessSchemaKey, values[3]) ||
        !within(document::kVignetteFeatherSchemaKey, values[4]) ||
        !within(document::kVignetteHighlightProtectionSchemaKey, values[5]))
        return std::nullopt;
    for (const double offset : {values[6], values[7]})
        if (!std::isfinite(offset) || std::abs(offset) > document::kMaximumVignetteCenter)
            return std::nullopt;
    const bool exposure = kernel.mode == VignetteMode::Exposure;
    const auto falloff = document::vignetteFalloff(
        kernel.shape == VignetteShape::Rectangular ? document::kVignetteRectangular
                                                   : document::kVignetteElliptical,
        values[2], values[3], values[4]);
    const float inner = static_cast<float>(falloff.inner);
    const float outer = static_cast<float>(falloff.outer);
    const bool hard = !(outer > inner);
    PreparedColorGrade grade{ColorGradeKind::Vignette,
                             std::vector<float>(kVignetteTableFloats, 0.0F), false};
    auto& d = grade.data;
    d[0] = exposure ? 1.0F : 0.0F;
    d[1] = static_cast<float>(exposure ? values[1] : values[0]);
    d[2] = static_cast<float>(values[5]);
    d[3] = hard ? 1.0F : 0.0F;
    d[4] = inner;
    d[5] = hard ? 0.0F : static_cast<float>(1.0 / (static_cast<double>(outer) - static_cast<double>(inner)));
    d[6] = static_cast<float>(falloff.exponent);
    d[7] = static_cast<float>(1.0 / falloff.exponent);
    d[8] = kLumaRed;
    d[9] = kLumaGreen;
    d[10] = kLumaBlue;
    d[11] = falloff.exponent == 2.0 ? 1.0F : 0.0F;
    d[12] = static_cast<float>(document::kVignetteProtectionLow);
    d[13] = static_cast<float>(
        1.0 / (document::kVignetteProtectionHigh - document::kVignetteProtectionLow));
    d[14] = static_cast<float>(falloff.circle);
    d[16] = static_cast<float>(values[6]);
    d[17] = static_cast<float>(values[7]);
    // No strength is no vignette, whatever the shape: the gain is one everywhere.
    grade.identity = d[1] == 0.0F;
    return grade;
}

std::optional<std::vector<float>>
vignetteProgramData(const PreparedColorGrade& grade, const render::ImageWindow& display,
                    const std::int64_t dataOriginX, const std::int64_t dataOriginY,
                    const double pixelAspect, const double horizontalScale,
                    const double verticalScale) {
    if (grade.kind != ColorGradeKind::Vignette || grade.data.size() != kVignetteTableFloats ||
        !std::isfinite(pixelAspect) || !(pixelAspect > 0.0) || !std::isfinite(horizontalScale) ||
        !std::isfinite(verticalScale) || !(horizontalScale > 0.0) || !(verticalScale > 0.0))
        return std::nullopt;
    const auto width = static_cast<double>(display.extent().width());
    const auto height = static_cast<double>(display.extent().height());
    if (!(width > 0.0) || !(height > 0.0))
        return std::nullopt;
    // The centre and the half-axes. Physical extents are measured in full-resolution pixel heights
    // so a non-uniform proxy scale keeps the shape a circle is drawn as.
    const double centreX = static_cast<double>(display.originX()) + 0.5 * width +
                           static_cast<double>(grade.data[16]) * horizontalScale;
    const double centreY = static_cast<double>(display.originY()) + 0.5 * height +
                           static_cast<double>(grade.data[17]) * verticalScale;
    const auto axes = document::vignetteHalfAxes(width / horizontalScale * pixelAspect,
                                                 height / verticalScale,
                                                 static_cast<double>(grade.data[14]));
    // Pixel index i of the data window has its centre at origin + i + 0.5.
    const double localX = centreX - static_cast<double>(dataOriginX) - 0.5;
    const double localY = centreY - static_cast<double>(dataOriginY) - 0.5;
    const double scaleX = pixelAspect / (horizontalScale * axes.x);
    const double scaleY = 1.0 / (verticalScale * axes.y);
    constexpr double kLimit = 16777216.0; // 2^24: every pixel index is an exact binary32 value
    if (!std::isfinite(localX) || !std::isfinite(localY) || std::abs(localX) > 64.0 * kLimit ||
        std::abs(localY) > 64.0 * kLimit || !std::isfinite(scaleX) || !std::isfinite(scaleY) ||
        !(scaleX > 0.0) || !(scaleY > 0.0) ||
        scaleX > static_cast<double>(std::numeric_limits<float>::max()) ||
        scaleY > static_cast<double>(std::numeric_limits<float>::max()))
        return std::nullopt;
    auto data = grade.data;
    data.insert(data.end(), {static_cast<float>(localX), static_cast<float>(localY),
                             static_cast<float>(scaleX), static_cast<float>(scaleY)});
    if (!(data[22] > 0.0F) || !(data[23] > 0.0F))
        return std::nullopt;
    return data;
}

bool vignettePixel(const std::vector<float>& k, const std::int64_t x, const std::int64_t y,
                   std::array<float, 4>& pixel) noexcept {
    const float a = pixel[3];
    if (a == 0.0F)
        return true;
    const float u = (static_cast<float>(x) - k[20]) * k[22];
    const float v = (static_cast<float>(y) - k[21]) * k[23];
    // Beyond four half-axes every falloff is complete; the clamp keeps the powers finite.
    const float ax = std::min(std::abs(u), 4.0F);
    const float ay = std::min(std::abs(v), 4.0F);
    const float d = k[11] != 0.0F ? std::sqrt(std::fma(ax, ax, ay * ay))
                                  : power(power(ax, k[6]) + power(ay, k[6]), k[7]);
    float mask = 0.0F;
    if (k[3] != 0.0F) {
        mask = d >= k[4] ? 1.0F : 0.0F;
    } else {
        const float t = std::min(std::max((d - k[4]) * k[5], 0.0F), 1.0F);
        mask = t * t * std::fma(-2.0F, t, 3.0F);
    }
    float gain = k[0] == 0.0F ? std::fma(-k[1], mask, 1.0F) : std::exp2(k[1] * mask);
    if (k[2] != 0.0F) {
        const float luma = std::fma(k[10], pixel[2], std::fma(k[9], pixel[1], k[8] * pixel[0]));
        const float straight = luma / a;
        if (!std::isfinite(straight))
            return false;
        float h = std::min(std::max((straight - k[12]) * k[13], 0.0F), 1.0F);
        h = h * h * std::fma(-2.0F, h, 3.0F);
        gain = std::fma(k[2] * h, 1.0F - gain, gain);
    }
    for (std::size_t i = 0; i < 3; ++i)
        pixel[i] = pixel[i] * gain;
    return std::isfinite(pixel[0]) && std::isfinite(pixel[1]) && std::isfinite(pixel[2]);
}

bool validVignetteProgramData(const std::vector<float>& d) noexcept {
    if (d.size() != kVignetteProgramFloats ||
        !std::ranges::all_of(d, [](const float value) { return std::isfinite(value); }))
        return false;
    const auto flag = [](const float value) { return value == 0.0F || value == 1.0F; };
    return flag(d[0]) && flag(d[3]) && flag(d[11]) && d[2] >= 0.0F && d[2] <= 1.0F &&
           d[6] >= 2.0F && d[7] > 0.0F && d[22] > 0.0F && d[23] > 0.0F;
}

const std::string& vignetteProgramText() {
    // The same binary32 operations as vignettePixel: precise fused multiply-adds, exact min, max
    // and clamp, the wrapper's correctly rounded division; sqrt, pow and exp2 to the device's
    // precision. The wrapper makes alpha 0 transparent black and refuses a non-finite result.
    static const std::string text =
        "layout(set = 0, binding = 0, std430) readonly buffer BloomVignetteData { vec4 values[]; "
        "} bloom_vignette_data;\n"
        "precise float bloom_ocio_cpu_div(float a, float b);\n"
        "float bloom_vignette_power(float a, float b) { return a > 0.0 ? pow(a, b) : 0.0; }\n"
        "vec4 bloom_vignette(ivec2 c) {\n"
        " vec4 p = imageLoad(bloom_ocio_input, c);\n"
        " float a = p.a;\n"
        " if (a == 0.0) { return p; }\n"
        " vec4 k0 = bloom_vignette_data.values[0];\n"
        " vec4 k1 = bloom_vignette_data.values[1];\n"
        " vec4 k2 = bloom_vignette_data.values[2];\n"
        " vec4 k3 = bloom_vignette_data.values[3];\n"
        " vec4 g = bloom_vignette_data.values[5];\n"
        " precise float u = (float(c.x) - g.x) * g.z;\n"
        " precise float v = (float(c.y) - g.y) * g.w;\n"
        " float ax = min(abs(u), 4.0);\n"
        " float ay = min(abs(v), 4.0);\n"
        " precise float d = 0.0;\n"
        " if (k2.w != 0.0) {\n"
        "  precise float s = fma(ax, ax, ay * ay);\n"
        "  d = sqrt(s);\n"
        " } else {\n"
        "  precise float s = bloom_vignette_power(ax, k1.z) + bloom_vignette_power(ay, k1.z);\n"
        "  d = bloom_vignette_power(s, k1.w);\n"
        " }\n"
        " precise float mask = 0.0;\n"
        " if (k0.w != 0.0) {\n"
        "  mask = d >= k1.x ? 1.0 : 0.0;\n"
        " } else {\n"
        "  precise float t = min(max((d - k1.x) * k1.y, 0.0), 1.0);\n"
        "  mask = t * t * fma(-2.0, t, 3.0);\n"
        " }\n"
        " precise float gain = 0.0;\n"
        " if (k0.x == 0.0) {\n"
        "  gain = fma(-k0.y, mask, 1.0);\n"
        " } else {\n"
        "  precise float e = k0.y * mask;\n"
        "  gain = exp2(e);\n"
        " }\n"
        " if (k0.z != 0.0) {\n"
        "  precise float luma = fma(k2.z, p.b, fma(k2.y, p.g, k2.x * p.r));\n"
        "  precise float straight = bloom_ocio_cpu_div(luma, a);\n"
        "  precise float h = min(max((straight - k3.x) * k3.y, 0.0), 1.0);\n"
        "  h = h * h * fma(-2.0, h, 3.0);\n"
        "  gain = fma(k0.z * h, 1.0 - gain, gain);\n"
        " }\n"
        " precise vec3 o = vec3(p.r * gain, p.g * gain, p.b * gain);\n"
        " return vec4(o, a);\n"
        "}\n";
    return text;
}

} // namespace bloom::runtime::detail
