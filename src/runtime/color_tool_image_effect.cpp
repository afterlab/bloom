#include "color_tool_image_effect.hpp"

#include <bloom/document/color_cleanup_nodes.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bloom::runtime::detail {
namespace {

// The operation numbers stored in the table's first float.
constexpr float kToneMapOperation = 0.0F;

[[nodiscard]] bool within(const double value, const double minimum, const double maximum) noexcept {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

// --- Tone Map ------------------------------------------------------------------------------------

// The highlight roll-off of one value. Up to the knee a value is untouched; above it the value is
// knee + r * d / (d + r) with d the distance past the knee and r the room up to the ceiling, which
// leaves the knee with slope one and approaches the ceiling without reaching it. No room (a ceiling
// at or below the knee) is a hard clip at the ceiling.
[[nodiscard]] float rollOff(const float x, const float knee, const float ceiling,
                            const float room) noexcept {
    if (room <= 0.0F)
        return std::min(x, ceiling);
    if (x <= knee)
        return x;
    const float distance = x - knee;
    const float numerator = room * distance;
    const float denominator = distance + room;
    const float shoulder = numerator / denominator;
    return knee + shoulder;
}

[[nodiscard]] bool toneMapPixel(const std::vector<float>& d, std::array<float, 4>& pixel) noexcept {
    const float a = pixel[3];
    if (a == 0.0F)
        return true;
    const int method = static_cast<int>(d[1]);
    const bool gamut = d[2] != 0.0F;
    const float knee = d[4];
    const float ceiling = d[5];
    const float room = d[6];
    const float shadows = d[8];
    const float shadowRange = d[9];
    const float threshold = d[12];
    const float scale = d[13];
    std::array<float, 3> c{pixel[0] / a, pixel[1] / a, pixel[2] / a};
    if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2]))
        return false;
    if (gamut) {
        // Distance from the brightest channel, as a fraction of its magnitude.
        const float achromatic = std::max(std::max(c[0], c[1]), c[2]);
        const float magnitude = std::abs(achromatic);
        if (magnitude != 0.0F) {
            for (auto& value : c) {
                const float distance = (achromatic - value) / magnitude;
                if (!(distance > threshold))
                    continue;
                const float excess = distance - threshold;
                const float ratio = excess / scale;
                const float divisor = 1.0F + ratio;
                const float compressed = excess / divisor;
                const float moved = threshold + compressed;
                const float offset = moved * magnitude;
                value = achromatic - offset;
            }
        }
    }
    if (shadows != 0.0F) {
        for (auto& value : c) {
            const float positive = std::max(value, 0.0F);
            const float divisor = shadowRange + positive;
            const float falloff = shadowRange / divisor;
            const float boost = shadows * falloff;
            const float gain = 1.0F + boost;
            value = value * gain;
        }
    }
    if (method == 0) {
        for (auto& value : c)
            value = rollOff(value, knee, ceiling, room);
    } else {
        const float brightest = std::max(std::max(c[0], c[1]), c[2]);
        if (brightest > 0.0F) {
            const float rolled = rollOff(brightest, knee, ceiling, room);
            if (rolled != brightest) {
                const float factor = rolled / brightest;
                for (auto& value : c)
                    value = value * factor;
            }
        }
    }
    const std::array<float, 3> out{c[0] * a, c[1] * a, c[2] * a};
    if (!std::isfinite(out[0]) || !std::isfinite(out[1]) || !std::isfinite(out[2]))
        return false;
    pixel = {out[0], out[1], out[2], a};
    return true;
}

[[nodiscard]] std::optional<PreparedColorGrade> prepareToneMap(const ColorToolKernel& kernel,
                                                               const std::span<const double> v) {
    if (v.size() != colorToolScalarCount(ColorToolOperation::ToneMap) || kernel.choices[0] < 0 ||
        kernel.choices[0] > document::kToneMapBrightestChannel || kernel.choices[1] < 0 ||
        kernel.choices[1] > 1)
        return std::nullopt;
    const double knee = v[0];
    const double ceiling = v[1];
    const double shadows = v[2];
    const double range = v[3];
    const double threshold = v[4];
    const double limit = v[5];
    constexpr double kMax = document::kMaximumToneMapValue;
    if (!within(knee, 0.0, kMax) || !within(ceiling, 0.0, kMax) || !within(shadows, -1.0, 16.0) ||
        !within(range, 1.0e-4, kMax) || !within(threshold, 0.0, 0.999) ||
        !within(limit, 1.001, 100.0))
        return std::nullopt;
    PreparedColorGrade grade{ColorGradeKind::ColorTool, std::vector<float>(kColorToolFloats, 0.0F),
                             false};
    auto& d = grade.data;
    d[0] = kToneMapOperation;
    d[1] = static_cast<float>(kernel.choices[0]);
    d[2] = static_cast<float>(kernel.choices[1]);
    d[4] = static_cast<float>(knee);
    d[5] = static_cast<float>(ceiling);
    d[6] = d[5] - d[4];
    d[8] = static_cast<float>(shadows);
    d[9] = static_cast<float>(range);
    d[12] = static_cast<float>(threshold);
    d[13] = static_cast<float>((limit - threshold) * (1.0 - threshold) / (limit - 1.0));
    // Nothing moves a value when the roll-off lies beyond any value the format can hold, no
    // shadow gain is asked for and the gamut is left alone.
    grade.identity = false;
    return grade;
}

} // namespace

std::optional<PreparedColorGrade> prepareColorToolGrade(const ColorToolKernel& kernel,
                                                        const std::span<const double> values) {
    switch (kernel.operation) {
    case ColorToolOperation::ToneMap:
        return prepareToneMap(kernel, values);
    }
    return std::nullopt;
}

bool colorToolPixel(const std::vector<float>& table, std::array<float, 4>& pixel) noexcept {
    if (table.size() != kColorToolFloats)
        return false;
    switch (static_cast<int>(table[0])) {
    case 0:
        return toneMapPixel(table, pixel);
    default:
        return false;
    }
}

bool validColorToolProgramData(const std::vector<float>& data) noexcept {
    if (data.size() != kColorToolFloats)
        return false;
    const auto whole = [](const float value, const float high) {
        return value >= 0.0F && value <= high && value == std::floor(value);
    };
    if (!whole(data[0], 0.0F))
        return false;
    return whole(data[1], 1.0F) && whole(data[2], 1.0F);
}

const std::string& colorToolProgramText() {
    // The same binary32 operations as colorToolPixel: precise sums, differences and products, the
    // wrapper's correctly rounded division, exact min, max and abs. A non-finite value is refused
    // like the CPU's.
    static const std::string text = std::string(R"GLSL(
layout(set = 0, binding = 0, std430) readonly buffer BloomColorToolData { vec4 values[]; } bloom_color_tool_data;
precise float bloom_ocio_cpu_div(float a, float b);
precise vec3 bloom_ocio_cpu_premul(vec3 rgb, float a);
float bct(int i) { return bloom_color_tool_data.values[i >> 2][i & 3]; }
bool bct_finite(float x) { return !isnan(x) && !isinf(x); }
float bct_roll_off(float x, float knee, float ceiling, float room) {
 if (room <= 0.0) { return min(x, ceiling); }
 if (x <= knee) { return x; }
 precise float dist = x - knee;
 precise float numerator = room * dist;
 precise float denominator = dist + room;
 float shoulder = bloom_ocio_cpu_div(numerator, denominator);
 precise float y = knee + shoulder;
 return y;
}
vec4 bct_tone_map(vec4 p) {
 float a = p.a;
 if (a == 0.0) { return p; }
 int method = int(bct(1));
 bool gamut = bct(2) != 0.0;
 float knee = bct(4);
 float ceiling = bct(5);
 float room = bct(6);
 float shadows = bct(8);
 float shadowRange = bct(9);
 float threshold = bct(12);
 float scale = bct(13);
 vec3 c = vec3(bloom_ocio_cpu_div(p.r, a), bloom_ocio_cpu_div(p.g, a), bloom_ocio_cpu_div(p.b, a));
 if (!bct_finite(c.r) || !bct_finite(c.g) || !bct_finite(c.b)) { return vec4(0.0, 0.0, 0.0, 2.0); }
 if (gamut) {
  float achromatic = max(max(c.r, c.g), c.b);
  float magnitude = abs(achromatic);
  if (magnitude != 0.0) {
   for (int i = 0; i < 3; ++i) {
    float dist = bloom_ocio_cpu_div(achromatic - c[i], magnitude);
    if (!(dist > threshold)) { continue; }
    precise float excess = dist - threshold;
    float ratio = bloom_ocio_cpu_div(excess, scale);
    precise float divisor = 1.0 + ratio;
    float compressed = bloom_ocio_cpu_div(excess, divisor);
    precise float moved = threshold + compressed;
    precise float offset = moved * magnitude;
    precise float value = achromatic - offset;
    c[i] = value;
   }
  }
 }
 if (shadows != 0.0) {
  for (int i = 0; i < 3; ++i) {
   float positive = max(c[i], 0.0);
   precise float divisor = shadowRange + positive;
   float falloff = bloom_ocio_cpu_div(shadowRange, divisor);
   precise float boost = shadows * falloff;
   precise float gain = 1.0 + boost;
   precise float value = c[i] * gain;
   c[i] = value;
  }
 }
 if (method == 0) {
  for (int i = 0; i < 3; ++i) { c[i] = bct_roll_off(c[i], knee, ceiling, room); }
 } else {
  float brightest = max(max(c.r, c.g), c.b);
  if (brightest > 0.0) {
   float rolled = bct_roll_off(brightest, knee, ceiling, room);
   if (rolled != brightest) {
    float factor = bloom_ocio_cpu_div(rolled, brightest);
    for (int i = 0; i < 3; ++i) {
     precise float value = c[i] * factor;
     c[i] = value;
    }
   }
  }
 }
 vec3 o = bloom_ocio_cpu_premul(c, a);
 if (!bct_finite(o.r) || !bct_finite(o.g) || !bct_finite(o.b)) { return vec4(0.0, 0.0, 0.0, 2.0); }
 return vec4(o, a);
}
vec4 bloom_color_tool(ivec2 c) {
 vec4 p = imageLoad(bloom_ocio_input, c);
 int op = int(bct(0));
 return bct_tone_map(p);
}
)GLSL");
    return text;
}

} // namespace bloom::runtime::detail
