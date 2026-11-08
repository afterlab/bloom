// #563 Codec Degrade. docs/architecture/image-filters.md, "Codec Degrade", is the specification.
//
// The pixel arithmetic is additions, multiplications, floor, min, max and correctly rounded
// divisions only, in one fixed order, so the CPU reference and the GPU program agree exactly on
// identical inputs. The table (binary32):
//   0 pass (CodecStage)  1 block N  2 chroma step cs  3 1 / base step  4 base step
//   5 rounding offset  6 residual weight  7 1 / N  8 motion gain  9 1 / cs^2
//   10 has previous  11 has flow
//  12 colour region (x, y, width, height)  16 colour atlas position (x, y)
//  18 previous region                      22 previous atlas position
//  24 flow region                          28 flow atlas position
//  30 output window origin (x, y)          32 output window size (width, height)
//  36 DCT basis, row u of N values for u < N, at 36 + u * N + n  (at most 16 x 16)
// 292 weight of a coefficient at u + v, for u + v < 2N - 1       (32 entries)
// 324 1 / that weight                                            (32 entries)
// 356 Rec. 709 matrix: kR, kG, kB, 1 / Cb scale, 1 / Cr scale, Cr to R, Cb to B, Cb to G, Cr to G

#include "codec_degrade_image_effect.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <bloom/document/codec_degrade_nodes.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bloom::runtime::detail {
namespace {

constexpr std::size_t kBasis = 36;
constexpr std::size_t kWeight = 292;
constexpr std::size_t kInverseWeight = 324;
constexpr std::size_t kMatrix = 356;
constexpr double kExactInteger = 16777216.0; // 2^24

// The display-referred curve, e = (1 + k) v / (v + k), and its inverse, v = k e / (1 + k - e).
constexpr float kCurveK = 0.25F;
constexpr float kCurveOnePlusK = 1.25F;
// How much coarser a block's quantiser gets with its motion: 1 + kMotionGain * min(|m| / N, 1).
constexpr double kMotionGain = 3.0;
// A coefficient's weight is 1 + kWeightSlope * (u + v) / (2N - 2): 1 at the DC, 8 at the corner.
constexpr double kWeightSlope = 7.0;

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic =
        EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, std::string(text), {}};
    return result;
}

[[nodiscard]] constexpr std::int64_t floorDiv(const std::int64_t a, const std::int64_t b) noexcept {
    const auto q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

[[nodiscard]] float clamp01(const float v) noexcept { return std::min(std::max(v, 0.0F), 1.0F); }

struct Ycc final {
    float y = 0.0F;
    float cb = 0.0F;
    float cr = 0.0F;
};

[[nodiscard]] Ycc toYcc(const float* const k, const float r, const float g, const float b) noexcept {
    const float y = k[kMatrix] * r + k[kMatrix + 1] * g + k[kMatrix + 2] * b;
    return {y, (b - y) * k[kMatrix + 3], (r - y) * k[kMatrix + 4]};
}

// The region of one image a pass reads: its data window and rows. A fetch clamps to the window, and
// an absent image reads as `absent`.
struct Rows final {
    std::int64_t originX = 0;
    std::int64_t originY = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::vector<std::span<const render::Rgba32f>> rows;
    [[nodiscard]] bool present() const noexcept { return width > 0 && height > 0; }
    [[nodiscard]] std::array<float, 4> clamped(const std::int64_t x, const std::int64_t y) const {
        const auto i = std::clamp<std::int64_t>(x - originX, 0, width - 1);
        const auto j = std::clamp<std::int64_t>(y - originY, 0, height - 1);
        const auto& p = rows[static_cast<std::size_t>(j)][static_cast<std::size_t>(i)];
        return {p.red(), p.green(), p.blue(), p.alpha()};
    }
};

// One plane's block: the residuals in `blk` (N * N, row j, column i) are transformed, quantised and
// reconstructed in place of `out`. The sums run in ascending order with a separate rounding at every
// multiplication and addition. `inverse` is 1 / the base step, `step` the base step, `theta` the
// rounding offset.
void codePlane(const float* const k, const int n, float* const blk, float* const tmp,
               float* const out, const float inverse, const float step,
               const float theta) noexcept {
    const float* const basis = k + kBasis;
    for (int j = 0; j < n; ++j)
        for (int u = 0; u < n; ++u) {
            float s = 0.0F;
            for (int i = 0; i < n; ++i)
                s = s + blk[j * n + i] * basis[u * n + i];
            tmp[j * n + u] = s;
        }
    for (int v = 0; v < n; ++v)
        for (int u = 0; u < n; ++u) {
            float s = 0.0F;
            for (int j = 0; j < n; ++j)
                s = s + tmp[j * n + u] * basis[v * n + j];
            blk[v * n + u] = s;
        }
    for (int v = 0; v < n; ++v)
        for (int u = 0; u < n; ++u) {
            const auto w = static_cast<std::size_t>(u + v);
            const float c = blk[v * n + u];
            const float q = std::floor(std::fabs(c) * (inverse * k[kInverseWeight + w]) + theta);
            const float r = q * (step * k[kWeight + w]);
            blk[v * n + u] = c < 0.0F ? -r : r;
        }
    for (int iy = 0; iy < n; ++iy) {
        for (int u = 0; u < n; ++u) {
            float t = 0.0F;
            for (int v = 0; v < n; ++v)
                t = t + blk[v * n + u] * basis[v * n + iy];
            tmp[u] = t;
        }
        for (int ix = 0; ix < n; ++ix) {
            float r = 0.0F;
            for (int u = 0; u < n; ++u)
                r = r + tmp[u] * basis[u * n + ix];
            out[iy * n + ix] = r;
        }
    }
}

// The pass over one image, the same for every output pixel.
struct Pass final {
    const float* k = nullptr;
    CodecStage stage = CodecStage::Encode;
    int n = 8;
    int cs = 2;
    Rows colour;   // the image the output follows
    Rows previous; // Predicted: the chain so far
    Rows flow;
    float inverseBase = 0.0F;
    float base = 0.0F;
    float theta = 0.5F;
    float weight = 1.0F;

    // The block motion of the luma block (bx, by): the flow's mean over the block's pixels inside the
    // colour window, rounded to a whole pixel.
    [[nodiscard]] std::array<float, 2> motion(const std::int64_t bx, const std::int64_t by) const {
        if (stage != CodecStage::Predicted || !flow.present())
            return {0.0F, 0.0F};
        float sx = 0.0F;
        float sy = 0.0F;
        int count = 0;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const std::int64_t px = bx * n + i;
                const std::int64_t py = by * n + j;
                if (px < colour.originX || py < colour.originY || px >= colour.originX + colour.width ||
                    py >= colour.originY + colour.height)
                    continue;
                const auto f = flow.clamped(px, py);
                sx = sx + f[0];
                sy = sy + f[1];
                ++count;
            }
        if (count == 0)
            return {0.0F, 0.0F};
        const auto c = static_cast<float>(count);
        return {std::floor(sx / c + 0.5F), std::floor(sy / c + 0.5F)};
    }
    [[nodiscard]] float gainOf(const std::array<float, 2>& m) const noexcept {
        const float mag = std::max(std::fabs(m[0]), std::fabs(m[1]));
        const float ratio = std::min(mag * k[7], 1.0F);
        return 1.0F + k[8] * ratio;
    }

    // The residual of the Encoded colour at (x, y) against the prediction moved by `m`.
    [[nodiscard]] Ycc residual(const std::int64_t x, const std::int64_t y,
                               const std::array<float, 2>& m) const {
        const auto c = colour.clamped(x, y);
        const auto i = toYcc(k, c[0], c[1], c[2]);
        Ycc p{0.5F, 0.0F, 0.0F};
        if (stage == CodecStage::Predicted && previous.present()) {
            const auto q = previous.clamped(x + static_cast<std::int64_t>(m[0]),
                                            y + static_cast<std::int64_t>(m[1]));
            p = toYcc(k, q[0], q[1], q[2]);
        }
        return {i.y - p.y, i.cb - p.cb, i.cr - p.cr};
    }
    [[nodiscard]] Ycc prediction(const std::int64_t x, const std::int64_t y,
                                 const std::array<float, 2>& m) const {
        if (stage == CodecStage::Predicted && previous.present()) {
            const auto q = previous.clamped(x + static_cast<std::int64_t>(m[0]),
                                            y + static_cast<std::int64_t>(m[1]));
            return toYcc(k, q[0], q[1], q[2]);
        }
        return {0.5F, 0.0F, 0.0F};
    }
};

} // namespace

std::optional<PreparedColorGrade> prepareCodecDegradeGrade(const CodecDegradeKernel& kernel,
                                                           const std::span<const double> values) {
    if ((kernel.blockPixels != 8 && kernel.blockPixels != 16) || kernel.gopFrames < 1 ||
        kernel.gopFrames > 30 || kernel.step < 0 || kernel.step >= kernel.gopFrames)
        return std::nullopt;
    PreparedColorGrade grade{ColorGradeKind::CodecDegrade,
                             std::vector<float>(kCodecPreparedFloats, 0.0F), false};
    auto& d = grade.data;
    d[1] = static_cast<float>(kernel.blockPixels);
    d[2] = kernel.chromaBleed ? 1.0F : 0.0F;
    d[5] = static_cast<float>(kernel.step);
    d[6] = static_cast<float>(kernel.gopFrames);
    if (kernel.role == CodecDegradeRole::Encode || kernel.role == CodecDegradeRole::Finish) {
        if (!values.empty())
            return std::nullopt;
        // Nothing of the step: one table serves every Encode of a frame, however it is reached.
        d[5] = 0.0F;
        d[6] = 0.0F;
        d[0] = static_cast<float>(kernel.role == CodecDegradeRole::Encode ? CodecStage::Encode
                                                                         : CodecStage::Finish);
        return grade;
    }
    if (values.size() != document::kCodecDegradeScalarCount + 1)
        return std::nullopt;
    for (const auto& spec : document::codecDegradeParameters()) {
        if (spec.kind != document::CodecDegradeParameterKind::Scalar)
            continue;
        const double value = values[document::codecDegradeScalarIndex(spec.role)];
        if (!std::isfinite(value) || value < spec.minimum || value > spec.maximum)
            return std::nullopt;
    }
    const double frame = values[document::kCodecDegradeScalarCount];
    if (!std::isfinite(frame))
        return std::nullopt;
    d[3] = static_cast<float>(values[document::codecDegradeScalarIndex("quality")]);
    d[4] = static_cast<float>(values[document::codecDegradeScalarIndex("persistence")]);
    // The frame's position in its group of pictures; the step is a keyframe, a predicted frame or
    // after the current frame, the exact identity.
    const auto position = frame - std::floor(frame / static_cast<double>(kernel.gopFrames)) *
                                      static_cast<double>(kernel.gopFrames);
    if (kernel.step == 0) {
        d[0] = static_cast<float>(CodecStage::Intra);
    } else if (static_cast<double>(kernel.step) <= position) {
        d[0] = static_cast<float>(CodecStage::Predicted);
    } else {
        d[0] = static_cast<float>(CodecStage::Predicted);
        grade.identity = true;
    }
    return grade;
}

CodecStage codecDegradeStage(const PreparedColorGrade& grade) noexcept {
    return grade.data.size() == kCodecPreparedFloats ||
                   grade.data.size() == kCodecDegradeProgramFloats
               ? static_cast<CodecStage>(static_cast<int>(grade.data[0]))
               : CodecStage::Encode;
}

int codecBlock(const PreparedColorGrade& grade, const double horizontalScale) noexcept {
    const auto scaled = std::llround(static_cast<double>(grade.data[1]) * horizontalScale);
    return static_cast<int>(std::clamp<long long>(scaled, 2, kCodecMaxBlock));
}

int codecChromaStep(const PreparedColorGrade& grade, const double horizontalScale) noexcept {
    return grade.data[2] != 0.0F && horizontalScale >= 0.75 ? 2 : 1;
}
std::optional<std::vector<float>>
codecDegradeProgramData(const PreparedColorGrade& grade, const CodecPlacement& placement,
                        const double horizontalScale) {
    if (grade.kind != ColorGradeKind::CodecDegrade || grade.data.size() != kCodecPreparedFloats ||
        !std::isfinite(horizontalScale) || !(horizontalScale > 0.0))
        return std::nullopt;
    const auto& g = grade.data;
    const auto stage = codecDegradeStage(grade);
    const int n = codecBlock(grade, horizontalScale);
    const int cs = codecChromaStep(grade, horizontalScale);
    std::vector<float> d(kCodecDegradeProgramFloats, 0.0F);
    d[0] = g[0];
    d[1] = static_cast<float>(n);
    d[2] = static_cast<float>(cs);
    // The JPEG quality scale, then the base step in the unit range of the Encoded colour. An
    // orthonormal block's coefficients grow with N, so the step does too.
    const double quality = static_cast<double>(g[3]);
    const double scale = std::max(quality < 50.0 ? 5000.0 / std::max(quality, 1.0)
                                                 : 200.0 - 2.0 * quality,
                                  1.0);
    const double base = 0.0004 * scale * (static_cast<double>(n) / 8.0);
    d[3] = static_cast<float>(1.0 / base);
    d[4] = static_cast<float>(base);
    const double persistence = stage == CodecStage::Predicted ? static_cast<double>(g[4]) : 0.0;
    d[5] = static_cast<float>(0.5 - 0.4 * persistence);
    d[6] = static_cast<float>(1.0 - persistence);
    d[7] = static_cast<float>(1.0 / static_cast<double>(n));
    d[8] = static_cast<float>(kMotionGain);
    d[9] = static_cast<float>(1.0 / static_cast<double>(cs * cs));
    const auto place = [&](const std::size_t at, const std::size_t atlas, const CodecRegion& r) {
        if (r.width <= 0 || r.height <= 0)
            return true;
        for (const auto value : {r.originX, r.originY, r.originX + r.width, r.originY + r.height,
                                 r.atlasX + r.width, r.atlasY + r.height})
            if (std::abs(static_cast<double>(value)) > kExactInteger)
                return false;
        d[at] = static_cast<float>(r.originX);
        d[at + 1] = static_cast<float>(r.originY);
        d[at + 2] = static_cast<float>(r.width);
        d[at + 3] = static_cast<float>(r.height);
        d[atlas] = static_cast<float>(r.atlasX);
        d[atlas + 1] = static_cast<float>(r.atlasY);
        return true;
    };
    if (placement.colour.width <= 0 || placement.colour.height <= 0 ||
        !place(12, 16, placement.colour) || !place(18, 22, placement.previous) ||
        !place(24, 28, placement.flow))
        return std::nullopt;
    d[10] = placement.previous.width > 0 ? 1.0F : 0.0F;
    d[11] = placement.flow.width > 0 ? 1.0F : 0.0F;
    d[30] = static_cast<float>(placement.colour.originX);
    d[31] = static_cast<float>(placement.colour.originY);
    d[32] = static_cast<float>(placement.colour.width);
    d[33] = static_cast<float>(placement.colour.height);
    // The DCT-II basis, orthonormal, and the quantiser's frequency weights.
    for (int u = 0; u < n; ++u)
        for (int i = 0; i < n; ++i) {
            const double alpha = u == 0 ? std::sqrt(1.0 / n) : std::sqrt(2.0 / n);
            d[kBasis + static_cast<std::size_t>(u * n + i)] = static_cast<float>(
                alpha * std::cos((2.0 * i + 1.0) * u * std::numbers::pi / (2.0 * n)));
        }
    for (int w = 0; w < 2 * n - 1; ++w) {
        const double weight = 1.0 + kWeightSlope * w / (2.0 * n - 2.0);
        d[kWeight + static_cast<std::size_t>(w)] = static_cast<float>(weight);
        d[kInverseWeight + static_cast<std::size_t>(w)] = static_cast<float>(1.0 / weight);
    }
    // Rec. 709: Y = kR r + kG g + kB b, Cb = (B - Y) / (2 (1 - kB)), Cr = (R - Y) / (2 (1 - kR)).
    constexpr double kr = 0.2126;
    constexpr double kb = 0.0722;
    constexpr double kg = 1.0 - kr - kb;
    d[kMatrix] = static_cast<float>(kr);
    d[kMatrix + 1] = static_cast<float>(kg);
    d[kMatrix + 2] = static_cast<float>(kb);
    d[kMatrix + 3] = static_cast<float>(1.0 / (2.0 * (1.0 - kb)));
    d[kMatrix + 4] = static_cast<float>(1.0 / (2.0 * (1.0 - kr)));
    d[kMatrix + 5] = static_cast<float>(2.0 * (1.0 - kr));
    d[kMatrix + 6] = static_cast<float>(2.0 * (1.0 - kb));
    d[kMatrix + 7] = static_cast<float>(kb * 2.0 * (1.0 - kb) / kg);
    d[kMatrix + 8] = static_cast<float>(kr * 2.0 * (1.0 - kr) / kg);
    if (!std::ranges::all_of(d, [](const float v) { return std::isfinite(v); }))
        return std::nullopt;
    return d;
}

bool validCodecDegradeProgramData(const std::vector<float>& d) noexcept {
    if (d.size() != kCodecDegradeProgramFloats ||
        !std::ranges::all_of(d, [](const float v) { return std::isfinite(v); }))
        return false;
    const auto whole = [](const float v, const float low, const float high) {
        return v >= low && v <= high && v == std::floor(v);
    };
    return whole(d[0], 0.0F, 3.0F) && whole(d[1], 2.0F, static_cast<float>(kCodecMaxBlock)) &&
           whole(d[2], 1.0F, 2.0F) && d[3] > 0.0F && d[4] > 0.0F && d[5] >= 0.0F && d[5] <= 0.5F &&
           d[6] >= 0.0F && d[6] <= 1.0F && d[12 + 2] > 0.0F && d[12 + 3] > 0.0F && d[32] > 0.0F &&
           d[33] > 0.0F;
}

const std::string& codecDegradeProgramText() {
    // The same binary32 operations as applyCodecDegrade: precise additions and multiplications, the
    // wrapper's correctly rounded division and no fused multiply-add. The wrapper makes alpha 0
    // transparent black and refuses a non-finite result.
    static const std::string text = std::string(R"GLSL(
layout(set = 0, binding = 0, std430) readonly buffer BloomCodecDegradeData { vec4 values[]; } bloom_codec_degrade_data;
precise float bloom_ocio_cpu_div(float a, float b);
float bcd(int i) { return bloom_codec_degrade_data.values[i >> 2][i & 3]; }
ivec4 bcd_rect(int o) { return ivec4(int(bcd(o)), int(bcd(o + 1)), int(bcd(o + 2)), int(bcd(o + 3))); }
ivec2 bcd_shift(int o, int a) { return ivec2(int(bcd(a)) - int(bcd(o)), int(bcd(a + 1)) - int(bcd(o + 1))); }
int bcd_fdiv(int a, int b) { int q = a / b; return (a % b != 0 && a < 0) ? q - 1 : q; }
float bcd_basis[256];
float bcd_blk[256];
float bcd_tmp[256];
float bcd_cr[256];
int bcd_stage;
int bcd_n;
ivec4 bcd_colour;
ivec2 bcd_colourShift;
ivec4 bcd_previous;
ivec2 bcd_previousShift;
ivec4 bcd_flow;
ivec2 bcd_flowShift;
vec4 bcd_load(ivec4 r, ivec2 s, int x, int y) {
 int cx = clamp(x, r.x, r.x + r.z - 1);
 int cy = clamp(y, r.y, r.y + r.w - 1);
 return imageLoad(bloom_ocio_input, ivec2(cx, cy) + s);
}
vec3 bcd_ycc(vec3 e) {
 precise float y = bcd(356) * e.r + bcd(357) * e.g + bcd(358) * e.b;
 precise float cb = (e.b - y) * bcd(359);
 precise float cr = (e.r - y) * bcd(360);
 return vec3(y, cb, cr);
}
vec2 bcd_motion(int bx, int by) {
 if (bcd_stage != 2 || bcd_flow.z <= 0) { return vec2(0.0); }
 precise float sx = 0.0;
 precise float sy = 0.0;
 int count = 0;
 for (int j = 0; j < bcd_n; ++j) {
  for (int i = 0; i < bcd_n; ++i) {
   int px = bx * bcd_n + i;
   int py = by * bcd_n + j;
   if (px < bcd_colour.x || py < bcd_colour.y || px >= bcd_colour.x + bcd_colour.z || py >= bcd_colour.y + bcd_colour.w) { continue; }
   vec4 f = bcd_load(bcd_flow, bcd_flowShift, px, py);
   sx = sx + f.x;
   sy = sy + f.y;
   count = count + 1;
  }
 }
 if (count == 0) { return vec2(0.0); }
 float c = float(count);
 precise float mx = bloom_ocio_cpu_div(sx, c);
 precise float my = bloom_ocio_cpu_div(sy, c);
 precise float rx = floor(mx + 0.5);
 precise float ry = floor(my + 0.5);
 return vec2(rx, ry);
}
float bcd_gain(vec2 m) {
 float mag = max(abs(m.x), abs(m.y));
 precise float ratio = min(mag * bcd(7), 1.0);
 precise float scaled = bcd(8) * ratio;
 precise float gain = 1.0 + scaled;
 return gain;
}
vec3 bcd_prediction(int x, int y, vec2 m) {
 if (bcd_stage == 2 && bcd_previous.z > 0) {
  vec4 q = bcd_load(bcd_previous, bcd_previousShift, x + int(m.x), y + int(m.y));
  return bcd_ycc(q.rgb);
 }
 return vec3(0.5, 0.0, 0.0);
}
vec3 bcd_residual(int x, int y, vec2 m) {
 vec4 c = bcd_load(bcd_colour, bcd_colourShift, x, y);
 vec3 i = bcd_ycc(c.rgb);
 vec3 p = bcd_prediction(x, y, m);
 precise float ry = i.x - p.x;
 precise float rb = i.y - p.y;
 precise float rr = i.z - p.z;
 return vec3(ry, rb, rr);
}
float bcd_plane(float invBase, float stepBase, float theta, int ix, int iy) {
 int n = bcd_n;
 for (int j = 0; j < n; ++j) {
  for (int u = 0; u < n; ++u) {
   precise float s = 0.0;
   for (int i = 0; i < n; ++i) { s = s + bcd_blk[j * n + i] * bcd_basis[u * n + i]; }
   bcd_tmp[j * n + u] = s;
  }
 }
 for (int v = 0; v < n; ++v) {
  for (int u = 0; u < n; ++u) {
   precise float s = 0.0;
   for (int j = 0; j < n; ++j) { s = s + bcd_tmp[j * n + u] * bcd_basis[v * n + j]; }
   bcd_blk[v * n + u] = s;
  }
 }
 for (int v = 0; v < n; ++v) {
  for (int u = 0; u < n; ++u) {
   int w = u + v;
   float c = bcd_blk[v * n + u];
   precise float f = invBase * bcd(324 + w);
   precise float m = abs(c) * f;
   precise float q = floor(m + theta);
   precise float sw = stepBase * bcd(292 + w);
   precise float r = q * sw;
   bcd_blk[v * n + u] = c < 0.0 ? -r : r;
  }
 }
 for (int u = 0; u < n; ++u) {
  precise float t = 0.0;
  for (int v = 0; v < n; ++v) { t = t + bcd_blk[v * n + u] * bcd_basis[v * n + iy]; }
  bcd_tmp[u] = t;
 }
 precise float r = 0.0;
 for (int u = 0; u < n; ++u) { r = r + bcd_tmp[u] * bcd_basis[u * n + ix]; }
 return r;
}
vec4 bloom_codec_degrade(ivec2 c) {
 if (c.x >= int(bcd(32)) || c.y >= int(bcd(33))) { return imageLoad(bloom_ocio_input, c); }
 bcd_stage = int(bcd(0));
 bcd_colour = bcd_rect(12);
 bcd_colourShift = bcd_shift(12, 16);
 int gx = c.x + int(bcd(30));
 int gy = c.y + int(bcd(31));
 vec4 own = bcd_load(bcd_colour, bcd_colourShift, gx, gy);
 if (bcd_stage == 0) {
  float a = own.a;
  if (a == 0.0) { return own; }
  precise vec3 s = vec3(bloom_ocio_cpu_div(own.r, a), bloom_ocio_cpu_div(own.g, a), bloom_ocio_cpu_div(own.b, a));
  vec3 sc = min(max(s, vec3(0.0)), vec3(1.0));
  precise vec3 num = 1.25 * sc;
  precise vec3 den = sc + 0.25;
  precise vec3 e = vec3(bloom_ocio_cpu_div(num.x, den.x), bloom_ocio_cpu_div(num.y, den.y), bloom_ocio_cpu_div(num.z, den.z));
  return vec4(e, a);
 }
 if (bcd_stage == 3) {
  float a = own.a;
  if (a == 0.0) { return own; }
  vec3 e = min(max(own.rgb, vec3(0.0)), vec3(1.0));
  precise vec3 num = 0.25 * e;
  precise vec3 den = 1.25 - e;
  precise vec3 l = vec3(bloom_ocio_cpu_div(num.x, den.x), bloom_ocio_cpu_div(num.y, den.y), bloom_ocio_cpu_div(num.z, den.z));
  precise vec3 p = l * a;
  return vec4(p, a);
 }
 bcd_n = int(bcd(1));
 int n = bcd_n;
 int cs = int(bcd(2));
 bcd_previous = bcd_rect(18);
 bcd_previousShift = bcd_shift(18, 22);
 bcd_flow = bcd_rect(24);
 bcd_flowShift = bcd_shift(24, 28);
 for (int i = 0; i < n * n; ++i) { bcd_basis[i] = bcd(36 + i); }
 int span = cs * n;
 int sbx = bcd_fdiv(gx, span);
 int sby = bcd_fdiv(gy, span);
 vec2 mvs[4];
 for (int j = 0; j < cs; ++j) {
  for (int i = 0; i < cs; ++i) { mvs[j * cs + i] = bcd_motion(sbx * cs + i, sby * cs + j); }
 }
 int lbx = bcd_fdiv(gx, n);
 int lby = bcd_fdiv(gy, n);
 int own_ = clamp(lby - sby * cs, 0, cs - 1) * cs + clamp(lbx - sbx * cs, 0, cs - 1);
 vec2 mine = mvs[own_];
 float inverseStep = bcd(3);
 float base = bcd(4);
 float theta = bcd(5);
 float weight = bcd(6);
 // The luma block, coded with its own block's motion.
 float gainY = bcd_gain(mine);
 precise float invGainY = bloom_ocio_cpu_div(1.0, gainY);
 precise float invY = inverseStep * invGainY;
 precise float stepY = base * gainY;
 int x0 = lbx * n;
 int y0 = lby * n;
 for (int j = 0; j < n; ++j) {
  for (int i = 0; i < n; ++i) { bcd_blk[j * n + i] = bcd_residual(x0 + i, y0 + j, mine).x; }
 }
 float reconY = bcd_plane(invY, stepY, theta, gx - x0, gy - y0);
 // The chroma blocks of the super block, coded with the largest motion in it.
 float gainC = 1.0;
 for (int k = 0; k < cs * cs; ++k) { gainC = max(gainC, bcd_gain(mvs[k])); }
 precise float invGainC = bloom_ocio_cpu_div(1.0, gainC);
 precise float invC = inverseStep * invGainC;
 precise float stepC = base * gainC;
 int sx0 = sbx * span;
 int sy0 = sby * span;
 float invCs2 = bcd(9);
 for (int cj = 0; cj < n; ++cj) {
  for (int ci = 0; ci < n; ++ci) {
   precise float sumCb = 0.0;
   precise float sumCr = 0.0;
   for (int dj = 0; dj < cs; ++dj) {
    for (int di = 0; di < cs; ++di) {
     int px = clamp(sx0 + ci * cs + di, bcd_colour.x, bcd_colour.x + bcd_colour.z - 1);
     int py = clamp(sy0 + cj * cs + dj, bcd_colour.y, bcd_colour.y + bcd_colour.w - 1);
     int mi = clamp(bcd_fdiv(py, n) - sby * cs, 0, cs - 1) * cs + clamp(bcd_fdiv(px, n) - sbx * cs, 0, cs - 1);
     vec3 r = bcd_residual(px, py, mvs[mi]);
     sumCb = sumCb + r.y;
     sumCr = sumCr + r.z;
    }
   }
   precise float mb = sumCb * invCs2;
   precise float mr = sumCr * invCs2;
   bcd_blk[cj * n + ci] = mb;
   bcd_cr[cj * n + ci] = mr;
  }
 }
 int cx = (gx - sx0) / cs;
 int cy = (gy - sy0) / cs;
 float reconCb = bcd_plane(invC, stepC, theta, cx, cy);
 for (int i = 0; i < n * n; ++i) { bcd_blk[i] = bcd_cr[i]; }
 float reconCr = bcd_plane(invC, stepC, theta, cx, cy);
 vec3 pred = bcd_prediction(gx, gy, mine);
 precise float wy = weight * reconY;
 precise float wb = weight * reconCb;
 precise float wr = weight * reconCr;
 precise float yy = pred.x + wy;
 precise float cb = pred.y + wb;
 precise float cr = pred.z + wr;
 precise float rr = yy + bcd(361) * cr;
 precise float bb = yy + bcd(362) * cb;
 precise float g1 = yy - bcd(363) * cb;
 precise float gg = g1 - bcd(364) * cr;
 vec3 rgb = min(max(vec3(rr, gg, bb), vec3(0.0)), vec3(1.0));
 return vec4(rgb, own.a);
}
)GLSL");
    return text;
}

namespace {

// Codes the super block (sbx, sby) and writes the pixels of it inside the output window.
void codeSuperBlock(const Pass& pass, const std::int64_t sbx, const std::int64_t sby,
                    render::Rgba32fImageBuilder& image) {
    const float* const k = pass.k;
    const int n = pass.n;
    const int cs = pass.cs;
    const std::int64_t span = static_cast<std::int64_t>(cs) * n;
    const std::int64_t sx0 = sbx * span;
    const std::int64_t sy0 = sby * span;
    const std::int64_t wx0 = pass.colour.originX;
    const std::int64_t wy0 = pass.colour.originY;
    const std::int64_t wx1 = wx0 + pass.colour.width;
    const std::int64_t wy1 = wy0 + pass.colour.height;
    std::array<std::array<float, 2>, 4> mvs{};
    for (int j = 0; j < cs; ++j)
        for (int i = 0; i < cs; ++i)
            mvs[static_cast<std::size_t>(j * cs + i)] = pass.motion(sbx * cs + i, sby * cs + j);
    float gainC = 1.0F;
    for (int j = 0; j < cs * cs; ++j)
        gainC = std::max(gainC, pass.gainOf(mvs[static_cast<std::size_t>(j)]));
    const float invGainC = 1.0F / gainC;
    const float invC = k[3] * invGainC;
    const float stepC = k[4] * gainC;
    const float theta = k[5];
    const float weight = k[6];
    std::array<float, kCodecMaxBlock * kCodecMaxBlock> blkCb{}, blkCr{}, tmp{}, reconCb{}, reconCr{},
        blkY{}, reconY{};
    const auto clampX = [&](const std::int64_t x) { return std::clamp(x, wx0, wx1 - 1); };
    const auto clampY = [&](const std::int64_t y) { return std::clamp(y, wy0, wy1 - 1); };
    for (int cj = 0; cj < n; ++cj)
        for (int ci = 0; ci < n; ++ci) {
            float sumCb = 0.0F;
            float sumCr = 0.0F;
            for (int dj = 0; dj < cs; ++dj)
                for (int di = 0; di < cs; ++di) {
                    const auto px = clampX(sx0 + static_cast<std::int64_t>(ci) * cs + di);
                    const auto py = clampY(sy0 + static_cast<std::int64_t>(cj) * cs + dj);
                    const auto mi =
                        std::clamp<std::int64_t>(floorDiv(py, n) - sby * cs, 0, cs - 1) * cs +
                        std::clamp<std::int64_t>(floorDiv(px, n) - sbx * cs, 0, cs - 1);
                    const auto r = pass.residual(px, py, mvs[static_cast<std::size_t>(mi)]);
                    sumCb = sumCb + r.cb;
                    sumCr = sumCr + r.cr;
                }
            blkCb[static_cast<std::size_t>(cj * n + ci)] = sumCb * k[9];
            blkCr[static_cast<std::size_t>(cj * n + ci)] = sumCr * k[9];
        }
    codePlane(k, n, blkCb.data(), tmp.data(), reconCb.data(), invC, stepC, theta);
    codePlane(k, n, blkCr.data(), tmp.data(), reconCr.data(), invC, stepC, theta);
    for (int lj = 0; lj < cs; ++lj)
        for (int li = 0; li < cs; ++li) {
            const std::int64_t lbx = sbx * cs + li;
            const std::int64_t lby = sby * cs + lj;
            const std::int64_t x0 = lbx * n;
            const std::int64_t y0 = lby * n;
            if (x0 >= wx1 || x0 + n <= wx0 || y0 >= wy1 || y0 + n <= wy0)
                continue;
            const auto& mine = mvs[static_cast<std::size_t>(lj * cs + li)];
            const float gainY = pass.gainOf(mine);
            const float invGainY = 1.0F / gainY;
            const float invY = k[3] * invGainY;
            const float stepY = k[4] * gainY;
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i)
                    blkY[static_cast<std::size_t>(j * n + i)] = pass.residual(x0 + i, y0 + j, mine).y;
            codePlane(k, n, blkY.data(), tmp.data(), reconY.data(), invY, stepY, theta);
            for (std::int64_t gy = std::max(y0, wy0); gy < std::min(y0 + n, wy1); ++gy) {
                auto row = image.row(gy);
                for (std::int64_t gx = std::max(x0, wx0); gx < std::min(x0 + n, wx1); ++gx) {
                    const auto ix = static_cast<int>(gx - x0);
                    const auto iy = static_cast<int>(gy - y0);
                    const auto cx = static_cast<int>((gx - sx0) / cs);
                    const auto cy = static_cast<int>((gy - sy0) / cs);
                    const float ry = reconY[static_cast<std::size_t>(iy * n + ix)];
                    const float rb = reconCb[static_cast<std::size_t>(cy * n + cx)];
                    const float rr = reconCr[static_cast<std::size_t>(cy * n + cx)];
                    const auto pred = pass.prediction(gx, gy, mine);
                    const float wyv = weight * ry;
                    const float wbv = weight * rb;
                    const float wrv = weight * rr;
                    const float yy = pred.y + wyv;
                    const float cb = pred.cb + wbv;
                    const float cr = pred.cr + wrv;
                    const float red = yy + k[kMatrix + 5] * cr;
                    const float blue = yy + k[kMatrix + 6] * cb;
                    const float g1 = yy - k[kMatrix + 7] * cb;
                    const float green = g1 - k[kMatrix + 8] * cr;
                    const float alpha = pass.colour.clamped(gx, gy)[3];
                    const auto pixel = render::Rgba32f::fromPremultiplied(
                        clamp01(red), clamp01(green), clamp01(blue), alpha);
                    if (pixel)
                        (*row.value())[static_cast<std::size_t>(gx - wx0)] = *pixel.value();
                }
            }
        }
}

} // namespace

ImageEffectResult applyCodecDegrade(const PreparedColorGrade& grade,
                                    const render::Rgba32fImage* const input,
                                    const render::Rgba32fImage* const colour,
                                    const render::Rgba32fImage* const flow,
                                    const double horizontalScale, const std::size_t budget,
                                    const CancellationToken& cancellation,
                                    const OperationIndex operation,
                                    const EvaluationProgressCallback& progress,
                                    CpuRowBandExecutor* const rowBands) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    const auto stage = codecDegradeStage(grade);
    const render::Rgba32fImage* const base = stage == CodecStage::Predicted ? colour : input;
    if (base == nullptr) {
        ImageEffectResult none;
        none.empty = true;
        return none;
    }
    std::vector<render::Rgba32fImageView> views;
    views.reserve(3);
    const auto rowsOf = [&](const render::Rgba32fImage* const picture, Rows& rows) -> bool {
        if (picture == nullptr)
            return true;
        const auto view = picture->view();
        if (!view)
            return false;
        views.push_back(*view.value());
        const auto window = picture->descriptor()->dataWindow();
        rows.originX = window.originX();
        rows.originY = window.originY();
        rows.width = static_cast<std::int64_t>(window.extent().width());
        rows.height = static_cast<std::int64_t>(window.extent().height());
        for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
            const auto row = views.back().row(y);
            if (!row)
                return false;
            rows.rows.push_back(*row.value());
        }
        return true;
    };
    Pass pass;
    CodecPlacement placement;
    const bool predicted = stage == CodecStage::Predicted;
    if (!rowsOf(base, pass.colour) || (predicted && !rowsOf(input, pass.previous)) ||
        (predicted && !rowsOf(flow, pass.flow)))
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Codec Degrade image is invalid");
    placement.colour = {pass.colour.originX, pass.colour.originY, pass.colour.width,
                        pass.colour.height, 0, 0};
    placement.previous = {pass.previous.originX, pass.previous.originY, pass.previous.width,
                          pass.previous.height, 0, 0};
    placement.flow = {pass.flow.originX, pass.flow.originY, pass.flow.width, pass.flow.height, 0, 0};
    const auto data = codecDegradeProgramData(grade, placement, horizontalScale);
    if (!data)
        return failed(EvaluationDiagnosticCode::InvalidParameter,
                      "Codec Degrade geometry cannot be represented");
    pass.k = data->data();
    pass.stage = stage;
    pass.n = static_cast<int>(pass.k[1]);
    pass.cs = static_cast<int>(pass.k[2]);
    auto builder = render::Rgba32fImageBuilder::create(*base->descriptor(), budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Codec Degrade output exceeds the pixel budget");
    auto& image = *builder.value();
    const auto window = base->descriptor()->dataWindow();
    const auto invalid = [](const char* const text) {
        return EvaluationDiagnostic{EvaluationDiagnosticCode::InvalidPixel,
                                    DiagnosticSeverity::Error,
                                    {},
                                    text,
                                    {}};
    };
    const auto* const k = pass.k;
    std::uint32_t total = 0;
    std::int64_t first = 0;
    std::int64_t span = 1;
    const bool pointwise = stage == CodecStage::Encode || stage == CodecStage::Finish;
    if (pointwise) {
        total = static_cast<std::uint32_t>(window.extent().height());
        first = window.originY();
    } else {
        span = static_cast<std::int64_t>(pass.cs) * pass.n;
        first = floorDiv(window.originY(), span);
        total = static_cast<std::uint32_t>(floorDiv(window.maxYExclusive() - 1, span) - first + 1);
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, total});
    const auto outcome = runRowBandPass(
        rowBands, cancellation, total, first,
        [&](const std::int64_t y) -> std::optional<EvaluationDiagnostic> {
            if (!pointwise) {
                const auto firstBlock = floorDiv(window.originX(), span);
                const auto lastBlock = floorDiv(window.maxXExclusive() - 1, span);
                for (auto sbx = firstBlock; sbx <= lastBlock; ++sbx) {
                    if (cancellation.isCancellationRequested())
                        return std::nullopt;
                    codeSuperBlock(pass, sbx, y, image);
                }
                return std::nullopt;
            }
            auto row = image.row(y);
            if (!row)
                return EvaluationDiagnostic{EvaluationDiagnosticCode::InternalInvariant,
                                            DiagnosticSeverity::Error,
                                            {},
                                            "Codec Degrade row is unavailable",
                                            {}};
            for (std::size_t column = 0; column < row.value()->size(); ++column) {
                if ((column & 255U) == 0 && cancellation.isCancellationRequested())
                    return std::nullopt;
                const auto s = pass.colour.clamped(window.originX() + static_cast<std::int64_t>(column), y);
                const float a = s[3];
                std::array<float, 3> out{};
                if (a != 0.0F) {
                    if (stage == CodecStage::Encode) {
                        for (std::size_t c = 0; c < 3; ++c) {
                            const float straight = clamp01(s[c] / a);
                            out[c] = (kCurveOnePlusK * straight) / (straight + kCurveK);
                        }
                    } else {
                        for (std::size_t c = 0; c < 3; ++c) {
                            const float e = clamp01(s[c]);
                            const float linear = (kCurveK * e) / (kCurveOnePlusK - e);
                            out[c] = linear * a;
                        }
                    }
                }
                const auto pixel = render::Rgba32f::fromPremultiplied(out[0], out[1], out[2], a);
                if (!pixel)
                    return invalid("Codec Degrade produced an invalid pixel");
                (*row.value())[column] = *pixel.value();
            }
            return std::nullopt;
        });
    static_cast<void>(k);
    if (outcome.cancelled || cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    if (outcome.incomplete || outcome.failure) {
        ImageEffectResult result;
        result.diagnostic = outcome.failure ? *outcome.failure
                                            : EvaluationDiagnostic{
                                                  EvaluationDiagnosticCode::AllocationFailure,
                                                  DiagnosticSeverity::Error,
                                                  {},
                                                  "A Codec Degrade row band could not complete",
                                                  {}};
        return result;
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, total, total});
    auto frozen = std::move(image).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Codec Degrade publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

} // namespace bloom::runtime::detail
