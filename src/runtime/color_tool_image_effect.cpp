#include "color_tool_image_effect.hpp"

#include <bloom/document/color_cleanup_nodes.hpp>

#include <algorithm>
#include <bit>
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


// --- Color Matrix --------------------------------------------------------------------------------

constexpr float kMatrixOperation = 1.0F;
using Triple = std::array<float, 3>;

// A cube root by the same exact steps on both routes: a bit-level first guess within about 5%, then
// four Newton steps (each step squares the error). t > 0.
[[nodiscard]] float cubeRoot(const float t) noexcept {
    std::uint32_t bits = std::bit_cast<std::uint32_t>(t);
    bits = bits / 3U + 0x2A5137A0U;
    float y = std::bit_cast<float>(bits);
    for (int step = 0; step < 4; ++step) {
        const float square = y * y;
        const float cube = square * y;
        const float error = cube - t;
        const float slope = 3.0F * square;
        const float correction = error / slope;
        y = y - correction;
    }
    return y;
}

constexpr float kLumaR = 0.2126F;
constexpr float kLumaG = 0.7152F;
constexpr float kLumaB = 0.0722F;
constexpr float kCrScale = 1.5748F;
constexpr float kCbScale = 1.8556F;
// Linear Rec. 709 to CIE XYZ (D65) and back, and the D65 white point.
constexpr float kX[3] = {0.4124564F, 0.3575761F, 0.1804375F};
constexpr float kY[3] = {0.2126729F, 0.7151522F, 0.0721750F};
constexpr float kZ[3] = {0.0193339F, 0.1191920F, 0.9503041F};
constexpr float kR[3] = {3.2404542F, -1.5371385F, -0.4985314F};
constexpr float kG[3] = {-0.9692660F, 1.8760108F, 0.0415560F};
constexpr float kB[3] = {0.0556434F, -0.2040259F, 1.0572252F};
constexpr float kWhiteX = 0.95047F;
constexpr float kWhiteZ = 1.08883F;
constexpr float kLabDelta = 0.20689655F;      // 6 / 29
constexpr float kLabDeltaCubed = 0.008856452F; // (6 / 29)^3
constexpr float kLabSlope = 7.787037F;         // 1 / (3 * (6 / 29)^2)
constexpr float kLabOffset = 0.13793103F;      // 4 / 29
constexpr float kLabDeltaSquared3 = 0.12841855F;

[[nodiscard]] float unitOf(const float x) noexcept { return std::min(std::max(x, 0.0F), 1.0F); }

[[nodiscard]] float labF(const float t) noexcept {
    if (t > kLabDeltaCubed)
        return cubeRoot(t);
    const float scaled = kLabSlope * t;
    return scaled + kLabOffset;
}
[[nodiscard]] float labInverse(const float f) noexcept {
    if (f > kLabDelta) {
        const float square = f * f;
        return square * f;
    }
    const float shifted = f - kLabOffset;
    return kLabDeltaSquared3 * shifted;
}

[[nodiscard]] float dot3(const float* m, const Triple& c) noexcept {
    const float a = m[0] * c[0];
    const float b = m[1] * c[1];
    const float c2 = m[2] * c[2];
    const float ab = a + b;
    return ab + c2;
}

[[nodiscard]] Triple rgbToHsv(const Triple& in) noexcept {
    const float r = std::max(in[0], 0.0F);
    const float g = std::max(in[1], 0.0F);
    const float b = std::max(in[2], 0.0F);
    const float mx = std::max(std::max(r, g), b);
    const float mn = std::min(std::min(r, g), b);
    const float d = mx - mn;
    float h6 = 0.0F;
    if (d != 0.0F) {
        if (mx == r) {
            const float diff = g - b;
            h6 = diff / d;
            if (h6 < 0.0F)
                h6 = h6 + 6.0F;
        } else if (mx == g) {
            const float diff = b - r;
            const float q = diff / d;
            h6 = q + 2.0F;
        } else {
            const float diff = r - g;
            const float q = diff / d;
            h6 = q + 4.0F;
        }
    }
    float h = h6 / 6.0F;
    if (h >= 1.0F)
        h = h - 1.0F;
    const float s = mx > 0.0F ? d / mx : 0.0F;
    return {h, s, mx};
}

[[nodiscard]] Triple rgbToHsl(const Triple& in) noexcept {
    const float r = unitOf(in[0]);
    const float g = unitOf(in[1]);
    const float b = unitOf(in[2]);
    const float mx = std::max(std::max(r, g), b);
    const float mn = std::min(std::min(r, g), b);
    const float d = mx - mn;
    float h6 = 0.0F;
    if (d != 0.0F) {
        if (mx == r) {
            const float diff = g - b;
            h6 = diff / d;
            if (h6 < 0.0F)
                h6 = h6 + 6.0F;
        } else if (mx == g) {
            const float diff = b - r;
            const float q = diff / d;
            h6 = q + 2.0F;
        } else {
            const float diff = r - g;
            const float q = diff / d;
            h6 = q + 4.0F;
        }
    }
    float h = h6 / 6.0F;
    if (h >= 1.0F)
        h = h - 1.0F;
    const float sum = mx + mn;
    const float l = sum * 0.5F;
    float s = 0.0F;
    if (d != 0.0F) {
        if (l > 0.5F) {
            const float two = 2.0F - mx;
            const float denominator = two - mn;
            s = d / denominator;
        } else {
            s = d / sum;
        }
    }
    return {h, s, l};
}

[[nodiscard]] Triple hsvToRgb(const Triple& in) noexcept {
    const float h = in[0] - std::floor(in[0]);
    const float s = unitOf(in[1]);
    const float v = std::max(in[2], 0.0F);
    Triple out{};
    const float offsets[3] = {5.0F, 3.0F, 1.0F};
    for (int i = 0; i < 3; ++i) {
        const float scaled = h * 6.0F;
        const float k = scaled + offsets[i];
        const float quotient = k / 6.0F;
        const float whole = 6.0F * std::floor(quotient);
        const float kk = k - whole;
        const float rise = 4.0F - kk;
        float t = std::min(std::min(kk, rise), 1.0F);
        t = std::max(t, 0.0F);
        const float p = v * s;
        const float w = p * t;
        out[static_cast<std::size_t>(i)] = v - w;
    }
    return out;
}

[[nodiscard]] Triple hslToRgb(const Triple& in) noexcept {
    const float h = in[0] - std::floor(in[0]);
    const float s = unitOf(in[1]);
    const float l = unitOf(in[2]);
    const float room = 1.0F - l;
    const float least = std::min(l, room);
    const float a = s * least;
    Triple out{};
    const float offsets[3] = {0.0F, 8.0F, 4.0F};
    for (int i = 0; i < 3; ++i) {
        const float scaled = h * 12.0F;
        const float k = scaled + offsets[i];
        const float quotient = k / 12.0F;
        const float whole = 12.0F * std::floor(quotient);
        const float kk = k - whole;
        const float low = kk - 3.0F;
        const float high = 9.0F - kk;
        float t = std::min(std::min(low, high), 1.0F);
        t = std::max(t, -1.0F);
        const float w = a * t;
        out[static_cast<std::size_t>(i)] = l - w;
    }
    return out;
}

[[nodiscard]] Triple rgbToYCbCr(const Triple& c) noexcept {
    const float a = kLumaR * c[0];
    const float b = kLumaG * c[1];
    const float d = kLumaB * c[2];
    const float ab = a + b;
    const float y = ab + d;
    const float cbDiff = c[2] - y;
    const float crDiff = c[0] - y;
    return {y, cbDiff / kCbScale, crDiff / kCrScale};
}

[[nodiscard]] Triple yCbCrToRgb(const Triple& c) noexcept {
    const float rr = kCrScale * c[2];
    const float r = c[0] + rr;
    const float bb = kCbScale * c[1];
    const float b = c[0] + bb;
    const float t1 = kLumaR * r;
    const float t2 = kLumaB * b;
    const float u = c[0] - t1;
    const float v = u - t2;
    return {r, v / kLumaG, b};
}

[[nodiscard]] Triple rgbToLab(const Triple& c) noexcept {
    const float x = dot3(kX, c);
    const float y = dot3(kY, c);
    const float z = dot3(kZ, c);
    const float fx = labF(x / kWhiteX);
    const float fy = labF(y);
    const float fz = labF(z / kWhiteZ);
    const float lScaled = 1.16F * fy;
    const float l = lScaled - 0.16F;
    const float da = fx - fy;
    const float db = fy - fz;
    return {l, 5.0F * da, 2.0F * db};
}

[[nodiscard]] Triple labToRgb(const Triple& c) noexcept {
    const float lShift = c[0] + 0.16F;
    const float fy = lShift / 1.16F;
    const float aPart = c[1] / 5.0F;
    const float bPart = c[2] / 2.0F;
    const float fx = fy + aPart;
    const float fz = fy - bPart;
    const Triple xyz{kWhiteX * labInverse(fx), labInverse(fy), kWhiteZ * labInverse(fz)};
    return {dot3(kR, xyz), dot3(kG, xyz), dot3(kB, xyz)};
}

[[nodiscard]] Triple toRgb(const int model, const Triple& c) noexcept {
    switch (model) {
    case 1:
        return hslToRgb(c);
    case 2:
        return hsvToRgb(c);
    case 3:
        return yCbCrToRgb(c);
    case 4:
        return labToRgb(c);
    default:
        return c;
    }
}
[[nodiscard]] Triple fromRgb(const int model, const Triple& c) noexcept {
    switch (model) {
    case 1:
        return rgbToHsl(c);
    case 2:
        return rgbToHsv(c);
    case 3:
        return rgbToYCbCr(c);
    case 4:
        return rgbToLab(c);
    default:
        return c;
    }
}

[[nodiscard]] bool colorMatrixPixel(const std::vector<float>& d, std::array<float, 4>& pixel) noexcept {
    const float a = pixel[3];
    if (a == 0.0F)
        return true;
    const bool four = d[1] != 0.0F;
    const int inputModel = static_cast<int>(d[2]);
    const int outputModel = static_cast<int>(d[3]);
    Triple s{pixel[0] / a, pixel[1] / a, pixel[2] / a};
    if (!std::isfinite(s[0]) || !std::isfinite(s[1]) || !std::isfinite(s[2]))
        return false;
    const Triple rgb = toRgb(inputModel, s);
    Triple out{};
    float alpha = a;
    for (std::size_t row = 0; row < 3; ++row) {
        const float* m = d.data() + 4 + row * 4;
        float value = dot3(m, rgb);
        if (four) {
            const float extra = m[3] * a;
            value = value + extra;
        }
        out[row] = value;
    }
    if (four) {
        const float* m = d.data() + 4 + 12;
        float value = dot3(m, rgb);
        const float extra = m[3] * a;
        value = value + extra;
        alpha = unitOf(value);
    }
    out = fromRgb(outputModel, out);
    if (alpha == 0.0F) {
        pixel = {0.0F, 0.0F, 0.0F, 0.0F};
        return true;
    }
    const std::array<float, 3> stored{out[0] * alpha, out[1] * alpha, out[2] * alpha};
    if (!std::isfinite(stored[0]) || !std::isfinite(stored[1]) || !std::isfinite(stored[2]))
        return false;
    pixel = {stored[0], stored[1], stored[2], alpha};
    return true;
}

// The matrix of each preset, row by row (the alpha row and column are the identity's).
[[nodiscard]] std::array<float, 16> presetMatrix(const std::int64_t preset) {
    std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const auto rows = [&m](const std::array<float, 9>& v) {
        for (std::size_t r = 0; r < 3; ++r)
            for (std::size_t c = 0; c < 3; ++c)
                m[r * 4 + c] = v[r * 3 + c];
    };
    switch (preset) {
    case 2:
        rows({kLumaR, kLumaG, kLumaB, kLumaR, kLumaG, kLumaB, kLumaR, kLumaG, kLumaB});
        break;
    case 3:
        rows({0, 0, 1, 0, 1, 0, 1, 0, 0});
        break;
    case 4:
        rows({0, 1, 0, 1, 0, 0, 0, 0, 1});
        break;
    case 5:
        rows({1, 0, 0, 0, 0, 1, 0, 1, 0});
        break;
    case 6:
        rows({0.393F, 0.769F, 0.189F, 0.349F, 0.686F, 0.168F, 0.272F, 0.534F, 0.131F});
        break;
    default:
        break;
    }
    return m;
}

[[nodiscard]] std::optional<PreparedColorGrade> prepareColorMatrix(const ColorToolKernel& kernel,
                                                                   const std::span<const double> v) {
    if (v.size() != colorToolScalarCount(ColorToolOperation::ColorMatrix) ||
        kernel.choices[0] < 0 || kernel.choices[0] >= document::kColorMatrixPresetCount ||
        kernel.choices[1] < 0 || kernel.choices[1] > document::kColorMatrixSize4 ||
        kernel.choices[2] < 0 || kernel.choices[2] >= document::kColorModelCount ||
        kernel.choices[3] < 0 || kernel.choices[3] >= document::kColorModelCount)
        return std::nullopt;
    for (const double element : v)
        if (!within(element, -document::kMaximumColorMatrixElement,
                    document::kMaximumColorMatrixElement))
            return std::nullopt;
    PreparedColorGrade grade{ColorGradeKind::ColorTool, std::vector<float>(kColorToolFloats, 0.0F),
                             false};
    auto& d = grade.data;
    d[0] = kMatrixOperation;
    d[1] = static_cast<float>(kernel.choices[1]);
    d[2] = static_cast<float>(kernel.choices[2]);
    d[3] = static_cast<float>(kernel.choices[3]);
    if (kernel.choices[0] == document::kColorMatrixCustom) {
        for (std::size_t i = 0; i < 16; ++i)
            d[4 + i] = static_cast<float>(v[i]);
    } else {
        const auto preset = presetMatrix(kernel.choices[0]);
        for (std::size_t i = 0; i < 16; ++i)
            d[4 + i] = preset[i];
    }
    // An identity matrix between RGB and RGB changes nothing at either size.
    const auto identity = presetMatrix(1);
    grade.identity = d[2] == 0.0F && d[3] == 0.0F &&
                     std::equal(identity.begin(), identity.end(), d.begin() + 4);
    return grade;
}

// --- HSL Qualifier -------------------------------------------------------------------------------

constexpr float kQualifierOperation = 2.0F;

// 1 within the range, 0 beyond `soft` past it, a straight ramp between. `dist` is how far past the
// range's edge a value lies (<= 0 inside).
[[nodiscard]] float qualifierRamp(const float dist, const float soft) noexcept {
    if (dist <= 0.0F)
        return 1.0F;
    if (soft <= 0.0F)
        return 0.0F;
    const float q = dist / soft;
    if (q >= 1.0F)
        return 0.0F;
    return 1.0F - q;
}

// A range [low, high] of a value: the distance past the nearer edge.
[[nodiscard]] float qualifierRange(const float v, const float low, const float high,
                                   const float soft) noexcept {
    const float below = low - v;
    const float above = v - high;
    return qualifierRamp(std::max(below, above), soft);
}

[[nodiscard]] std::optional<PreparedColorGrade> prepareHslQualifier(const ColorToolKernel& kernel,
                                                                    const std::span<const double> v) {
    if (v.size() != colorToolScalarCount(ColorToolOperation::HslQualifier) ||
        kernel.choices[0] < 0 || kernel.choices[0] > 1)
        return std::nullopt;
    constexpr double kL = 1.0e4;
    if (!within(v[0], 0.0, 360.0) || !within(v[1], 0.0, 360.0) || !within(v[2], 0.0, 180.0) ||
        !within(v[3], 0.0, 1.0) || !within(v[4], 0.0, 1.0) || !within(v[5], 0.0, 1.0) ||
        !within(v[6], 0.0, kL) || !within(v[7], 0.0, kL) || !within(v[8], 0.0, kL))
        return std::nullopt;
    PreparedColorGrade grade{ColorGradeKind::ColorTool, std::vector<float>(kColorToolFloats, 0.0F),
                             false};
    auto& d = grade.data;
    d[0] = kQualifierOperation;
    d[1] = static_cast<float>(kernel.choices[0]);
    d[4] = static_cast<float>(v[0]);
    d[5] = static_cast<float>(v[1]) * 0.5F;
    d[6] = static_cast<float>(v[2]);
    d[8] = static_cast<float>(v[3]);
    d[9] = static_cast<float>(v[4]);
    d[10] = static_cast<float>(v[5]);
    d[12] = static_cast<float>(v[6]);
    d[13] = static_cast<float>(v[7]);
    d[14] = static_cast<float>(v[8]);
    return grade;
}

[[nodiscard]] bool hslQualifierPixel(const std::vector<float>& d,
                                     std::array<float, 4>& pixel) noexcept {
    const float a = pixel[3];
    if (a == 0.0F) {
        pixel = {0.0F, 0.0F, 0.0F, 0.0F};
        return true;
    }
    const Triple c{pixel[0] / a, pixel[1] / a, pixel[2] / a};
    if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2]))
        return false;
    const Triple hsv = rgbToHsv(c);
    const float hue = hsv[0] * 360.0F;
    const float diff = hue - d[4];
    const float away = std::abs(diff);
    const float other = 360.0F - away;
    const float hueDist = std::min(away, other) - d[5];
    const float hueSel = qualifierRamp(hueDist, d[6]);
    const float satSel = qualifierRange(hsv[1], d[8], d[9], d[10]);
    const float ya = kLumaR * c[0];
    const float yb = kLumaG * c[1];
    const float yc = kLumaB * c[2];
    const float yab = ya + yb;
    const float luma = yab + yc;
    const float lumaSel = qualifierRange(luma, d[12], d[13], d[14]);
    const float hs = hueSel * satSel;
    float m = hs * lumaSel;
    if (d[1] != 0.0F)
        m = 1.0F - m;
    const float covered = m * a;
    pixel = {covered, covered, covered, covered};
    return true;
}

} // namespace

std::optional<PreparedColorGrade> prepareColorToolGrade(const ColorToolKernel& kernel,
                                                        const std::span<const double> values) {
    switch (kernel.operation) {
    case ColorToolOperation::ToneMap:
        return prepareToneMap(kernel, values);
    case ColorToolOperation::ColorMatrix:
        return prepareColorMatrix(kernel, values);
    case ColorToolOperation::HslQualifier:
        return prepareHslQualifier(kernel, values);
    }
    return std::nullopt;
}

bool colorToolPixel(const std::vector<float>& table, std::array<float, 4>& pixel) noexcept {
    if (table.size() != kColorToolFloats)
        return false;
    switch (static_cast<int>(table[0])) {
    case 0:
        return toneMapPixel(table, pixel);
    case 1:
        return colorMatrixPixel(table, pixel);
    case 2:
        return hslQualifierPixel(table, pixel);
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
    if (!whole(data[0], 2.0F))
        return false;
    if (data[0] == 2.0F)
        return whole(data[1], 1.0F);
    if (data[0] == 1.0F)
        return whole(data[1], 1.0F) && whole(data[2], 4.0F) && whole(data[3], 4.0F);
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
float bcm_unit(float x) { return min(max(x, 0.0), 1.0); }
float bcm_cube_root(float t) {
 uint bits = floatBitsToUint(t);
 bits = bits / 3u + 0x2A5137A0u;
 float y = uintBitsToFloat(bits);
 for (int iter = 0; iter < 4; ++iter) {
  precise float square = y * y;
  precise float cube = square * y;
  precise float error = cube - t;
  precise float slope = 3.0 * square;
  float correction = bloom_ocio_cpu_div(error, slope);
  precise float next = y - correction;
  y = next;
 }
 return y;
}
float bcm_lab_f(float t) {
 if (t > 0.008856452) { return bcm_cube_root(t); }
 precise float scaled = 7.787037 * t;
 precise float y = scaled + 0.13793103;
 return y;
}
float bcm_lab_inverse(float f) {
 if (f > 0.20689655) {
  precise float square = f * f;
  precise float cube = square * f;
  return cube;
 }
 precise float shifted = f - 0.13793103;
 precise float y = 0.12841855 * shifted;
 return y;
}
float bcm_dot3(vec3 m, vec3 c) {
 precise float a = m.x * c.x;
 precise float b = m.y * c.y;
 precise float c2 = m.z * c.z;
 precise float ab = a + b;
 precise float r = ab + c2;
 return r;
}
vec3 bcm_hue_parts(vec3 c, float mx, float mn) {
 float d = mx - mn;
 float h6 = 0.0;
 if (d != 0.0) {
  if (mx == c.r) {
   precise float diff = c.g - c.b;
   h6 = bloom_ocio_cpu_div(diff, d);
   if (h6 < 0.0) { precise float w = h6 + 6.0; h6 = w; }
  } else if (mx == c.g) {
   precise float diff = c.b - c.r;
   float q = bloom_ocio_cpu_div(diff, d);
   precise float w = q + 2.0;
   h6 = w;
  } else {
   precise float diff = c.r - c.g;
   float q = bloom_ocio_cpu_div(diff, d);
   precise float w = q + 4.0;
   h6 = w;
  }
 }
 float h = bloom_ocio_cpu_div(h6, 6.0);
 if (h >= 1.0) { precise float w = h - 1.0; h = w; }
 return vec3(h, d, 0.0);
}
vec3 bcm_rgb_to_hsv(vec3 in3) {
 vec3 c = max(in3, vec3(0.0));
 float mx = max(max(c.r, c.g), c.b);
 float mn = min(min(c.r, c.g), c.b);
 vec3 hp = bcm_hue_parts(c, mx, mn);
 float s = mx > 0.0 ? bloom_ocio_cpu_div(hp.y, mx) : 0.0;
 return vec3(hp.x, s, mx);
}
vec3 bcm_rgb_to_hsl(vec3 in3) {
 vec3 c = vec3(bcm_unit(in3.r), bcm_unit(in3.g), bcm_unit(in3.b));
 float mx = max(max(c.r, c.g), c.b);
 float mn = min(min(c.r, c.g), c.b);
 vec3 hp = bcm_hue_parts(c, mx, mn);
 float d = hp.y;
 precise float sum = mx + mn;
 precise float l = sum * 0.5;
 float s = 0.0;
 if (d != 0.0) {
  if (l > 0.5) {
   precise float two = 2.0 - mx;
   precise float denominator = two - mn;
   s = bloom_ocio_cpu_div(d, denominator);
  } else {
   s = bloom_ocio_cpu_div(d, sum);
  }
 }
 return vec3(hp.x, s, l);
}
vec3 bcm_hsv_to_rgb(vec3 in3) {
 precise float h = in3.x - floor(in3.x);
 float s = bcm_unit(in3.y);
 float v = max(in3.z, 0.0);
 vec3 o = vec3(0.0);
 float offsets[3] = float[3](5.0, 3.0, 1.0);
 for (int i = 0; i < 3; ++i) {
  precise float scaled = h * 6.0;
  precise float k = scaled + offsets[i];
  float quotient = bloom_ocio_cpu_div(k, 6.0);
  precise float whole = 6.0 * floor(quotient);
  precise float kk = k - whole;
  precise float rise = 4.0 - kk;
  float t = min(min(kk, rise), 1.0);
  t = max(t, 0.0);
  precise float p = v * s;
  precise float w = p * t;
  precise float f = v - w;
  o[i] = f;
 }
 return o;
}
vec3 bcm_hsl_to_rgb(vec3 in3) {
 precise float h = in3.x - floor(in3.x);
 float s = bcm_unit(in3.y);
 float l = bcm_unit(in3.z);
 precise float room = 1.0 - l;
 float least = min(l, room);
 precise float a = s * least;
 vec3 o = vec3(0.0);
 float offsets[3] = float[3](0.0, 8.0, 4.0);
 for (int i = 0; i < 3; ++i) {
  precise float scaled = h * 12.0;
  precise float k = scaled + offsets[i];
  float quotient = bloom_ocio_cpu_div(k, 12.0);
  precise float whole = 12.0 * floor(quotient);
  precise float kk = k - whole;
  precise float low = kk - 3.0;
  precise float high = 9.0 - kk;
  float t = min(min(low, high), 1.0);
  t = max(t, -1.0);
  precise float w = a * t;
  precise float f = l - w;
  o[i] = f;
 }
 return o;
}
vec3 bcm_rgb_to_ycbcr(vec3 c) {
 precise float a = 0.2126 * c.r;
 precise float b = 0.7152 * c.g;
 precise float d = 0.0722 * c.b;
 precise float ab = a + b;
 precise float y = ab + d;
 precise float cbDiff = c.b - y;
 precise float crDiff = c.r - y;
 return vec3(y, bloom_ocio_cpu_div(cbDiff, 1.8556), bloom_ocio_cpu_div(crDiff, 1.5748));
}
vec3 bcm_ycbcr_to_rgb(vec3 c) {
 precise float rr = 1.5748 * c.z;
 precise float r = c.x + rr;
 precise float bb = 1.8556 * c.y;
 precise float b = c.x + bb;
 precise float t1 = 0.2126 * r;
 precise float t2 = 0.0722 * b;
 precise float u = c.x - t1;
 precise float v = u - t2;
 return vec3(r, bloom_ocio_cpu_div(v, 0.7152), b);
}
vec3 bcm_rgb_to_lab(vec3 c) {
 float x = bcm_dot3(vec3(0.4124564, 0.3575761, 0.1804375), c);
 float y = bcm_dot3(vec3(0.2126729, 0.7151522, 0.0721750), c);
 float z = bcm_dot3(vec3(0.0193339, 0.1191920, 0.9503041), c);
 float fx = bcm_lab_f(bloom_ocio_cpu_div(x, 0.95047));
 float fy = bcm_lab_f(y);
 float fz = bcm_lab_f(bloom_ocio_cpu_div(z, 1.08883));
 precise float lScaled = 1.16 * fy;
 precise float l = lScaled - 0.16;
 precise float da = fx - fy;
 precise float db = fy - fz;
 precise float a5 = 5.0 * da;
 precise float b2 = 2.0 * db;
 return vec3(l, a5, b2);
}
vec3 bcm_lab_to_rgb(vec3 c) {
 precise float lShift = c.x + 0.16;
 float fy = bloom_ocio_cpu_div(lShift, 1.16);
 float aPart = bloom_ocio_cpu_div(c.y, 5.0);
 float bPart = bloom_ocio_cpu_div(c.z, 2.0);
 precise float fx = fy + aPart;
 precise float fz = fy - bPart;
 precise float xi = 0.95047 * bcm_lab_inverse(fx);
 precise float zi = 1.08883 * bcm_lab_inverse(fz);
 vec3 xyz = vec3(xi, bcm_lab_inverse(fy), zi);
 return vec3(bcm_dot3(vec3(3.2404542, -1.5371385, -0.4985314), xyz),
             bcm_dot3(vec3(-0.9692660, 1.8760108, 0.0415560), xyz),
             bcm_dot3(vec3(0.0556434, -0.2040259, 1.0572252), xyz));
}
vec3 bcm_to_rgb(int model, vec3 c) {
 if (model == 1) { return bcm_hsl_to_rgb(c); }
 if (model == 2) { return bcm_hsv_to_rgb(c); }
 if (model == 3) { return bcm_ycbcr_to_rgb(c); }
 if (model == 4) { return bcm_lab_to_rgb(c); }
 return c;
}
vec3 bcm_from_rgb(int model, vec3 c) {
 if (model == 1) { return bcm_rgb_to_hsl(c); }
 if (model == 2) { return bcm_rgb_to_hsv(c); }
 if (model == 3) { return bcm_rgb_to_ycbcr(c); }
 if (model == 4) { return bcm_rgb_to_lab(c); }
 return c;
}
vec4 bct_color_matrix(vec4 p) {
 float a = p.a;
 if (a == 0.0) { return p; }
 bool four = bct(1) != 0.0;
 int inputModel = int(bct(2));
 int outputModel = int(bct(3));
 vec3 s = vec3(bloom_ocio_cpu_div(p.r, a), bloom_ocio_cpu_div(p.g, a), bloom_ocio_cpu_div(p.b, a));
 if (!bct_finite(s.r) || !bct_finite(s.g) || !bct_finite(s.b)) { return vec4(0.0, 0.0, 0.0, 2.0); }
 vec3 rgb = bcm_to_rgb(inputModel, s);
 vec3 o = vec3(0.0);
 float alpha = a;
 for (int row = 0; row < 3; ++row) {
  int base = 4 + row * 4;
  float value = bcm_dot3(vec3(bct(base), bct(base + 1), bct(base + 2)), rgb);
  if (four) {
   precise float extra = bct(base + 3) * a;
   precise float sum = value + extra;
   value = sum;
  }
  o[row] = value;
 }
 if (four) {
  float value = bcm_dot3(vec3(bct(16), bct(17), bct(18)), rgb);
  precise float extra = bct(19) * a;
  precise float sum = value + extra;
  alpha = bcm_unit(sum);
 }
 o = bcm_from_rgb(outputModel, o);
 if (alpha == 0.0) { return vec4(0.0); }
 vec3 stored = bloom_ocio_cpu_premul(o, alpha);
 if (!bct_finite(stored.r) || !bct_finite(stored.g) || !bct_finite(stored.b)) { return vec4(0.0, 0.0, 0.0, 2.0); }
 return vec4(stored, alpha);
}
float bct_ramp(float dist, float soft) {
 if (dist <= 0.0) { return 1.0; }
 if (soft <= 0.0) { return 0.0; }
 float q = bloom_ocio_cpu_div(dist, soft);
 if (q >= 1.0) { return 0.0; }
 precise float r = 1.0 - q;
 return r;
}
float bct_range(float v, float low, float high, float soft) {
 precise float below = low - v;
 precise float above = v - high;
 return bct_ramp(max(below, above), soft);
}
vec4 bct_hsl_qualifier(vec4 p) {
 float a = p.a;
 if (a == 0.0) { return vec4(0.0); }
 vec3 c = vec3(bloom_ocio_cpu_div(p.r, a), bloom_ocio_cpu_div(p.g, a), bloom_ocio_cpu_div(p.b, a));
 if (!bct_finite(c.r) || !bct_finite(c.g) || !bct_finite(c.b)) { return vec4(0.0, 0.0, 0.0, 2.0); }
 vec3 hsv = bcm_rgb_to_hsv(c);
 precise float hue = hsv.x * 360.0;
 precise float diff = hue - bct(4);
 float away = abs(diff);
 precise float other = 360.0 - away;
 precise float hueDist = min(away, other) - bct(5);
 float hueSel = bct_ramp(hueDist, bct(6));
 float satSel = bct_range(hsv.y, bct(8), bct(9), bct(10));
 precise float ya = 0.2126 * c.r;
 precise float yb = 0.7152 * c.g;
 precise float yc = 0.0722 * c.b;
 precise float yab = ya + yb;
 precise float luma = yab + yc;
 float lumaSel = bct_range(luma, bct(12), bct(13), bct(14));
 precise float hs = hueSel * satSel;
 precise float m0 = hs * lumaSel;
 float m = m0;
 if (bct(1) != 0.0) { precise float inv = 1.0 - m0; m = inv; }
 precise float covered = m * a;
 return vec4(covered);
}
vec4 bloom_color_tool(ivec2 c) {
 vec4 p = imageLoad(bloom_ocio_input, c);
 int op = int(bct(0));
 if (op == 2) { return bct_hsl_qualifier(p); }
 if (op == 1) { return bct_color_matrix(p); }
 return bct_tone_map(p);
}
)GLSL");
    return text;
}

} // namespace bloom::runtime::detail
