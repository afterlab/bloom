// #483 Depth effects. The depth map (Z Defocus's circle of confusion, Depth Fog's fog factor) and
// Z Defocus's gather. The CPU reference (depthMapPixel, defocusPixel) and the GPU programs
// (depthMapProgramText, defocusProgramText) run the same binary32 operations over the same data:
// precise fused multiply-adds, exact min, max, clamp and floor, the wrapper's correctly rounded
// division and integer addressing. Only sqrt (Mist's quadratic falloffs, the fog's inverse
// quadratic) and exp (the exponential fogs) are not correctly rounded on every device.
// docs/architecture/image-filters.md, "Depth effects", is the specification.

#include "depth_effect_image_effect.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <bloom/document/depth_effect_nodes.hpp>
#include <bloom/document/grade_curves.hpp>
#include <bloom/runtime/grade_curve.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace bloom::runtime::detail {
namespace {

namespace dp = document::depth_parameters;

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic =
        EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, std::string(text), {}};
    return result;
}

[[nodiscard]] bool finite(const float value) noexcept { return std::isfinite(value); }
[[nodiscard]] float clampf(const float x, const float lo, const float hi) noexcept {
    return std::min(std::max(x, lo), hi);
}
[[nodiscard]] bool within(const double value, const double lo, const double hi) noexcept {
    return std::isfinite(value) && value >= lo && value <= hi;
}
[[nodiscard]] bool choice(const std::int64_t value, const std::int64_t last) noexcept {
    return value >= 0 && value <= last;
}
// Every integer coordinate the programs read is an exact binary32 value.
constexpr double kExactInteger = 16777216.0;

// --- The depth map ------------------------------------------------------------------------------

// A decoded depth: the distance and its state (0 a surface, 1 infinitely far, 2 unaffected).
struct Decoded final {
    float distance = 0.0F;
    int state = 0;
};

// The stored value of the chosen channel (straight), or nothing for an empty pixel.
[[nodiscard]] std::optional<float> depthValue(const float* k, const std::array<float, 4>& p) {
    const int channel = static_cast<int>(k[1]);
    float x = 0.0F;
    if (channel < 3) {
        const float a = p[3];
        if (!(a > 0.0F))
            return std::nullopt;
        x = p[static_cast<std::size_t>(channel)] / a;
    } else {
        x = p[3];
    }
    if (!finite(x))
        return std::nullopt;
    return x;
}

[[nodiscard]] Decoded decodeDepth(const float* k, const std::array<float, 4>& p) {
    const auto value = depthValue(k, p);
    const int interpretation = static_cast<int>(k[2]);
    bool empty = !value.has_value();
    float d = 0.0F;
    if (!empty) {
        const float x = *value;
        if (interpretation == 0) {
            if (x <= 0.0F || x >= 1.0e9F)
                empty = true;
            else
                d = x;
        } else if (interpretation == 1) {
            if (x <= 0.0F) {
                empty = true;
            } else {
                d = 1.0F / x;
                if (!finite(d))
                    return {0.0F, 1};
            }
        } else {
            float t = clampf(x, 0.0F, 1.0F);
            if (k[3] != 0.0F)
                t = 1.0F - t;
            if (interpretation == 3) {
                const int falloff = static_cast<int>(k[4]);
                if (falloff == 1)
                    t = std::sqrt(t);
                else if (falloff == 2)
                    t = t * t;
            }
            d = std::fma(t, k[7] - k[6], k[6]);
        }
    }
    if (empty) {
        const int background = static_cast<int>(k[5]);
        if (background == 0)
            return {0.0F, 1};
        if (background == 1)
            return {k[7], 0};
        return {0.0F, 2};
    }
    return {d, 0};
}

// The signed circle of confusion radius in full-resolution pixels, clamped to +-R.
[[nodiscard]] float circleOfConfusion(const float* k, const Decoded v, const float focus,
                                      const float physical) {
    const int mode = static_cast<int>(k[8]);
    const float widest = k[22];
    const float size = k[14];
    float c = 0.0F;
    if (v.state == 2) {
        c = 0.0F;
    } else if (mode == 1) {
        if (v.state == 1)
            c = physical;
        else if (v.distance <= 0.0F)
            c = -widest;
        else
            c = (physical * (v.distance - focus)) / v.distance;
    } else {
        if (v.state == 1) {
            c = size;
        } else if (v.distance <= 0.0F) {
            c = -widest;
        } else {
            const float half = k[13];
            const float beyond = v.distance - (focus + half);
            const float before = (focus - half) - v.distance;
            if (beyond > 0.0F)
                c = (size * beyond) / v.distance;
            else if (before > 0.0F)
                c = -((size * before) / v.distance);
        }
    }
    const int region = static_cast<int>(k[10]);
    if (region == 1)
        c = std::max(c, 0.0F);
    else if (region == 2)
        c = std::min(c, 0.0F);
    return clampf(c, -widest, widest);
}

// A size map's radius: the value times `size`, 0 for an empty pixel.
[[nodiscard]] float sizeMapRadius(const float* k, const std::array<float, 4>& p) {
    const auto value = depthValue(k, p);
    if (!value)
        return 0.0F;
    float t = clampf(*value, 0.0F, 1.0F);
    if (k[3] != 0.0F)
        t = 1.0F - t;
    return clampf(k[14] * t, -k[22], k[22]);
}

[[nodiscard]] float fogFactor(const float* k, const Decoded v) {
    float f = 0.0F;
    if (v.state == 1) {
        f = 1.0F;
    } else if (v.state == 2) {
        f = 0.0F;
    } else {
        const int falloff = static_cast<int>(k[8]);
        if (falloff == 3 || falloff == 4) {
            const float z = std::max(v.distance - k[10], 0.0F);
            float e = k[12] * z;
            if (falloff == 4)
                e = e * e;
            f = 1.0F - std::exp(-e);
        } else {
            float s = 0.0F;
            if (k[11] > 0.0F)
                s = (v.distance - k[10]) * k[11];
            else
                s = v.distance >= k[10] ? 1.0F : 0.0F;
            const float t = clampf(s, 0.0F, 1.0F);
            if (falloff == 0) {
                f = t;
            } else if (falloff == 1) {
                f = t * t;
            } else if (falloff == 2) {
                f = std::sqrt(t);
            } else {
                const float m = t * 256.0F;
                const int j = std::min(static_cast<int>(std::floor(m)), 255);
                const float fraction = m - static_cast<float>(j);
                const float l0 = k[16 + j];
                const float l1 = k[17 + j];
                f = clampf(std::fma(fraction, l1 - l0, l0), 0.0F, 1.0F);
            }
        }
    }
    return clampf(k[9] * f, 0.0F, 1.0F);
}

// One pixel of the depth map at data window index (x, y); `fetch(x, y)` reads the input (zero
// outside it) and `inside(x, y)` says whether an index lies in it.
template <typename Fetch, typename Inside>
[[nodiscard]] std::array<float, 4> depthMapPixel(const float* k, const int x, const int y,
                                                 Fetch&& fetch, Inside&& inside) {
    if (k[0] != 0.0F) {
        const float f = fogFactor(k, decodeDepth(k, fetch(x, y)));
        return {f, f, f, f};
    }
    const int mode = static_cast<int>(k[8]);
    float focus = k[12];
    if (mode != 2 && static_cast<int>(k[9]) == 1) {
        const int fx = static_cast<int>(k[26]);
        const int fy = static_cast<int>(k[27]);
        float sum = 0.0F;
        int count = 0;
        for (int j = -2; j <= 2; ++j)
            for (int i = -2; i <= 2; ++i) {
                if (!inside(fx + i, fy + j))
                    continue;
                const auto v = decodeDepth(k, fetch(fx + i, fy + j));
                if (v.state == 0) {
                    sum = sum + v.distance;
                    count = count + 1;
                }
            }
        if (count > 0)
            focus = sum / static_cast<float>(count);
    }
    float physical = 0.0F;
    if (mode == 1) {
        const float millimetres = focus * k[18];
        const float denominator = k[17] * (millimetres - k[16]);
        if (denominator > 0.0F)
            physical = ((k[16] * k[16]) / denominator) * k[25];
        else
            physical = k[22];
    }
    const int expand = static_cast<int>(k[11]);
    float c = k[22];
    for (int j = -expand; j <= expand; ++j)
        for (int i = -expand; i <= expand; ++i) {
            if (!inside(x + i, y + j))
                continue;
            const auto p = fetch(x + i, y + j);
            const float q =
                mode == 2 ? sizeMapRadius(k, p) : circleOfConfusion(k, decodeDepth(k, p), focus, physical);
            c = std::min(c, q);
        }
    const float scaled = c * k[24];
    const float a = std::min(std::abs(scaled) * k[29], 1.0F);
    return {scaled, 0.0F, 0.0F, a};
}

const std::string kDepthMapGlsl =
    "layout(set = 0, binding = 0, std430) readonly buffer BloomDepthMapData { vec4 values[]; } "
    "bloom_depth_map_data;\n"
    "precise float bloom_ocio_cpu_div(float a, float b);\n"
    "float bdm(int i) { return bloom_depth_map_data.values[i >> 2][i & 3]; }\n"
    // (value, 1) or (0, 0) for an empty pixel.
    "vec2 bloom_depth_value(vec4 p) {\n"
    " int channel = int(bdm(1));\n"
    " precise float x = 0.0;\n"
    " if (channel < 3) {\n"
    "  float a = p.a;\n"
    "  if (!(a > 0.0)) { return vec2(0.0); }\n"
    "  x = bloom_ocio_cpu_div(p[channel], a);\n"
    " } else { x = p.a; }\n"
    " if (isnan(x) || isinf(x)) { return vec2(0.0); }\n"
    " return vec2(x, 1.0);\n"
    "}\n"
    // (distance, state): state 0 a surface, 1 infinitely far, 2 unaffected.
    "vec2 bloom_depth_decode(vec4 p) {\n"
    " vec2 value = bloom_depth_value(p);\n"
    " int interpretation = int(bdm(2));\n"
    " bool empty = value.y == 0.0;\n"
    " precise float d = 0.0;\n"
    " if (!empty) {\n"
    "  float x = value.x;\n"
    "  if (interpretation == 0) {\n"
    "   if (x <= 0.0 || x >= 1.0e9) { empty = true; } else { d = x; }\n"
    "  } else if (interpretation == 1) {\n"
    "   if (x <= 0.0) { empty = true; } else {\n"
    "    d = bloom_ocio_cpu_div(1.0, x);\n"
    "    if (isinf(d) || isnan(d)) { return vec2(0.0, 1.0); }\n"
    "   }\n"
    "  } else {\n"
    "   precise float t = clamp(x, 0.0, 1.0);\n"
    "   if (bdm(3) != 0.0) { t = 1.0 - t; }\n"
    "   if (interpretation == 3) {\n"
    "    int falloff = int(bdm(4));\n"
    "    if (falloff == 1) { t = sqrt(t); } else if (falloff == 2) { t = t * t; }\n"
    "   }\n"
    "   precise float range = bdm(7) - bdm(6);\n"
    "   d = fma(t, range, bdm(6));\n"
    "  }\n"
    " }\n"
    " if (empty) {\n"
    "  int background = int(bdm(5));\n"
    "  if (background == 0) { return vec2(0.0, 1.0); }\n"
    "  if (background == 1) { return vec2(bdm(7), 0.0); }\n"
    "  return vec2(0.0, 2.0);\n"
    " }\n"
    " return vec2(d, 0.0);\n"
    "}\n"
    "float bloom_depth_coc(vec2 v, float focus, float physical) {\n"
    " int mode = int(bdm(8));\n"
    " float widest = bdm(22);\n"
    " float size = bdm(14);\n"
    " precise float c = 0.0;\n"
    " if (v.y == 2.0) { c = 0.0; }\n"
    " else if (mode == 1) {\n"
    "  if (v.y == 1.0) { c = physical; }\n"
    "  else if (v.x <= 0.0) { c = -widest; }\n"
    "  else { precise float n = physical * (v.x - focus); c = bloom_ocio_cpu_div(n, v.x); }\n"
    " } else {\n"
    "  if (v.y == 1.0) { c = size; }\n"
    "  else if (v.x <= 0.0) { c = -widest; }\n"
    "  else {\n"
    "   float half_ = bdm(13);\n"
    "   precise float sum = focus + half_;\n"
    "   precise float beyond = v.x - sum;\n"
    "   precise float near_ = focus - half_;\n"
    "   precise float before = near_ - v.x;\n"
    "   if (beyond > 0.0) { precise float n = size * beyond; c = bloom_ocio_cpu_div(n, v.x); }\n"
    "   else if (before > 0.0) { precise float n = size * before; c = -bloom_ocio_cpu_div(n, "
    "v.x); }\n"
    "  }\n"
    " }\n"
    " int region = int(bdm(10));\n"
    " if (region == 1) { c = max(c, 0.0); } else if (region == 2) { c = min(c, 0.0); }\n"
    " return clamp(c, -widest, widest);\n"
    "}\n"
    "float bloom_depth_size(vec4 p) {\n"
    " vec2 value = bloom_depth_value(p);\n"
    " if (value.y == 0.0) { return 0.0; }\n"
    " precise float t = clamp(value.x, 0.0, 1.0);\n"
    " if (bdm(3) != 0.0) { t = 1.0 - t; }\n"
    " precise float r = bdm(14) * t;\n"
    " return clamp(r, -bdm(22), bdm(22));\n"
    "}\n"
    "float bloom_depth_fog(vec2 v) {\n"
    " precise float f = 0.0;\n"
    " if (v.y == 1.0) { f = 1.0; }\n"
    " else if (v.y == 2.0) { f = 0.0; }\n"
    " else {\n"
    "  int falloff = int(bdm(8));\n"
    "  if (falloff == 3 || falloff == 4) {\n"
    "   precise float z = max(v.x - bdm(10), 0.0);\n"
    "   precise float e = bdm(12) * z;\n"
    "   if (falloff == 4) { e = e * e; }\n"
    "   precise float g = exp(-e);\n"
    "   f = 1.0 - g;\n"
    "  } else {\n"
    "   precise float s = 0.0;\n"
    "   if (bdm(11) > 0.0) { s = (v.x - bdm(10)) * bdm(11); }\n"
    "   else { s = v.x >= bdm(10) ? 1.0 : 0.0; }\n"
    "   precise float t = clamp(s, 0.0, 1.0);\n"
    "   if (falloff == 0) { f = t; }\n"
    "   else if (falloff == 1) { f = t * t; }\n"
    "   else if (falloff == 2) { f = sqrt(t); }\n"
    "   else {\n"
    "    precise float m = t * 256.0;\n"
    "    int j = min(int(floor(m)), 255);\n"
    "    precise float fraction = m - float(j);\n"
    "    float l0 = bdm(16 + j);\n"
    "    float l1 = bdm(17 + j);\n"
    "    precise float step_ = l1 - l0;\n"
    "    f = clamp(fma(fraction, step_, l0), 0.0, 1.0);\n"
    "   }\n"
    "  }\n"
    " }\n"
    " precise float o = bdm(9) * f;\n"
    " return clamp(o, 0.0, 1.0);\n"
    "}\n"
    "bool bloom_depth_inside(ivec2 q, ivec2 size) { return q.x >= 0 && q.y >= 0 && q.x < size.x && "
    "q.y < size.y; }\n"
    "vec4 bloom_depth_map(ivec2 c) {\n"
    " if (bdm(0) != 0.0) {\n"
    "  float f = bloom_depth_fog(bloom_depth_decode(imageLoad(bloom_ocio_input, c)));\n"
    "  return vec4(f);\n"
    " }\n"
    " ivec2 size = imageSize(bloom_ocio_input);\n"
    " int mode = int(bdm(8));\n"
    " precise float focus = bdm(12);\n"
    " if (mode != 2 && int(bdm(9)) == 1) {\n"
    "  int fx = int(bdm(26));\n"
    "  int fy = int(bdm(27));\n"
    "  precise float sum = 0.0;\n"
    "  int count = 0;\n"
    "  for (int j = -2; j <= 2; ++j) {\n"
    "   for (int i = -2; i <= 2; ++i) {\n"
    "    ivec2 q = ivec2(fx + i, fy + j);\n"
    "    if (!bloom_depth_inside(q, size)) { continue; }\n"
    "    vec2 v = bloom_depth_decode(imageLoad(bloom_ocio_input, q));\n"
    "    if (v.y == 0.0) { sum = sum + v.x; count = count + 1; }\n"
    "   }\n"
    "  }\n"
    "  if (count > 0) { focus = bloom_ocio_cpu_div(sum, float(count)); }\n"
    " }\n"
    " precise float physical = 0.0;\n"
    " if (mode == 1) {\n"
    "  precise float millimetres = focus * bdm(18);\n"
    "  precise float lens = millimetres - bdm(16);\n"
    "  precise float denominator = bdm(17) * lens;\n"
    "  if (denominator > 0.0) {\n"
    "   precise float square = bdm(16) * bdm(16);\n"
    "   precise float ratio = bloom_ocio_cpu_div(square, denominator);\n"
    "   physical = ratio * bdm(25);\n"
    "  } else { physical = bdm(22); }\n"
    " }\n"
    " int expand = int(bdm(11));\n"
    " precise float m = bdm(22);\n"
    " for (int j = -expand; j <= expand; ++j) {\n"
    "  for (int i = -expand; i <= expand; ++i) {\n"
    "   ivec2 q = c + ivec2(i, j);\n"
    "   if (!bloom_depth_inside(q, size)) { continue; }\n"
    "   vec4 p = imageLoad(bloom_ocio_input, q);\n"
    "   float r = mode == 2 ? bloom_depth_size(p) : bloom_depth_coc(bloom_depth_decode(p), focus, "
    "physical);\n"
    "   m = min(m, r);\n"
    "  }\n"
    " }\n"
    " precise float scaled = m * bdm(24);\n"
    " precise float a = min(abs(scaled) * bdm(29), 1.0);\n"
    " return vec4(scaled, 0.0, 0.0, a);\n"
    "}\n";

// --- The defocus gather -------------------------------------------------------------------------

constexpr std::size_t kPatternBase = kDefocusHeaderFloats;
// The density table's step: eight entries a ring spacing.
constexpr double kDensitySteps = 8.0;

[[nodiscard]] std::size_t slotOffset(const std::size_t slot) noexcept { return 28 + 8 * slot; }

// The density lookup: (pi / 4) / A(u) at u ring spacings, linearly interpolated.
[[nodiscard]] float density(const float* k, const float u) {
    const int base = static_cast<int>(k[13]);
    const int count = static_cast<int>(k[11]);
    const float m = u * 8.0F;
    const int j = static_cast<int>(std::floor(m));
    if (j >= count - 1)
        return k[base + count - 1];
    const float fraction = m - static_cast<float>(j);
    const float t0 = k[base + j];
    const float t1 = k[base + j + 1];
    return std::fma(fraction, t1 - t0, t0);
}

// One pixel of the gather at global evaluation position (gx, gy). `fetch(slot, x, y)` reads slot
// `slot` (-1 the colour input, else a DefocusKernel slot) at global position (x, y), zero outside.
template <typename Fetch>
[[nodiscard]] std::array<float, 4> defocusPixel(const float* k, const int gx, const int gy,
                                                Fetch&& fetch) {
    const bool uniform = k[0] == 3.0F;
    // The tile maximum carries each pixel's signed circle of confusion in red.
    const bool hasTile = k[slotOffset(kDefocusTileSlot) + 6] != 0.0F;
    const bool hasPre = k[slotOffset(kDefocusPrefilterSlot) + 6] != 0.0F;
    const bool hasBokeh = k[slotOffset(kDefocusBokehSlot) + 6] != 0.0F && k[1] == 2.0F;
    const float widest = k[16];
    const float base = uniform ? k[19] : 0.0F;
    const auto tile = hasTile ? fetch(0, gx, gy) : std::array<float, 4>{};
    const float cp = hasTile ? tile[0] : base;
    float radius = std::abs(cp);
    if (hasTile)
        radius = std::max(radius, tile[3] * widest);
    const float red = k[5];
    const float reach = radius * red;
    const auto own = fetch(-1, gx, gy);
    if (!(reach >= 0.5F))
        return own;
    const float rings = k[2];
    const float spacing = std::max(1.0F, reach / rings);
    const float inverse = 1.0F / spacing;
    const int last = static_cast<int>(std::min(rings, std::floor(std::fma(reach, inverse, 0.5F))));
    const float pre = hasPre ? clampf((spacing - 1.0F) * k[18], 0.0F, 1.0F) : 0.0F;
    const bool chroma = k[7] != 0.0F;
    const std::array<float, 3> scales{chroma ? red : 1.0F, 1.0F, chroma ? k[6] : 1.0F};
    const float gain = k[8];
    const float inverseThreshold = k[9];
    const float threshold = k[15];
    std::array<float, 3> weightFront{}, weightBack{}, coverFront{};
    std::array<float, 4> sumFront{}, sumBack{};
    // The coverage is the green disk's own, unweighted by the highlight boost or the bokeh image
    // (both shape the colour only), so it depends on the depth and the input's alpha alone.
    float coverBack = 0.0F;
    const float acp = std::abs(cp);
    const auto bokeh = [&](const float ox, const float oy, const float r) {
        std::array<float, 3> w{1.0F, 1.0F, 1.0F};
        if (!(r >= 1.0F))
            return w;
        const float ux = (ox * spacing) / r;
        const float uy = (oy * spacing) / r;
        const float px = std::fma(ux, k[60], k[61]);
        const float py = std::fma(uy, k[60], k[62]);
        const float x0 = std::floor(px);
        const float y0 = std::floor(py);
        const float fx = px - x0;
        const float fy = py - y0;
        const int ix = static_cast<int>(x0);
        const int iy = static_cast<int>(y0);
        const auto t00 = fetch(2, ix, iy);
        const auto t10 = fetch(2, ix + 1, iy);
        const auto t01 = fetch(2, ix, iy + 1);
        const auto t11 = fetch(2, ix + 1, iy + 1);
        for (std::size_t c = 0; c < 3; ++c) {
            const float top = std::fma(fx, t10[c] - t00[c], t00[c]);
            const float bottom = std::fma(fx, t11[c] - t01[c], t01[c]);
            w[c] = std::max(std::fma(fy, bottom - top, top), 0.0F);
        }
        return w;
    };
    for (int ring = 0; ring <= last; ++ring) {
        const int first = ring == 0 ? 0 : 1 + 4 * ring * (ring - 1);
        const int count = ring == 0 ? 1 : 8 * ring;
        for (int i = 0; i < count; ++i) {
            const float* e = k + kPatternBase + 8 * static_cast<std::size_t>(first + i);
            const int dx = static_cast<int>(std::floor(std::fma(e[0], spacing, 0.5F)));
            const int dy = static_cast<int>(std::floor(std::fma(e[1], spacing, 0.5F)));
            const int qx = gx + dx;
            const int qy = gy + dy;
            auto color = fetch(-1, qx, qy);
            if (ring > 0 && pre > 0.0F) {
                const auto filtered = fetch(1, qx, qy);
                for (std::size_t c = 0; c < 4; ++c)
                    color[c] = std::fma(pre, filtered[c] - color[c], color[c]);
            }
            const float cq = hasTile ? fetch(0, qx, qy)[0] : base;
            const float rho = e[2] * spacing;
            const float phi = clampf(cp - cq + 0.5F, 0.0F, 1.0F);
            float boost = 1.0F;
            if (gain > 0.0F) {
                float luma = 0.0F;
                if (color[3] > 0.0F)
                    luma = std::fma(0.0722F, color[2], std::fma(0.7152F, color[1], 0.2126F * color[0])) /
                           color[3];
                boost = std::fma(gain, clampf((luma - threshold) * inverseThreshold, 0.0F, 1.0F),
                                 1.0F);
            }
            const float acq = std::abs(cq);
            const float back = std::min(acq, acp);
            std::array<float, 3> kernelFront{1.0F, 1.0F, 1.0F}, kernelBack{1.0F, 1.0F, 1.0F};
            if (hasBokeh) {
                kernelFront = bokeh(e[4], e[5], acq);
                kernelBack = bokeh(e[4], e[5], back);
            }
            // Without chromatic bokeh every channel's disk is the green one: its coverage and
            // density are worked out once and shared.
            std::array<float, 3> front{}, behind{};
            for (std::size_t c = 0; c < (chroma ? 3U : 1U); ++c) {
                const float rf = acq * scales[chroma ? c : 1];
                const float rb = back * scales[chroma ? c : 1];
                const float coverF = clampf(std::fma(rf - rho, inverse, 0.5F), 0.0F, 1.0F);
                const float coverB = clampf(std::fma(rb - rho, inverse, 0.5F), 0.0F, 1.0F);
                front[c] = (phi * coverF) * density(k, rf * inverse);
                behind[c] = (1.0F - phi) * coverB;
            }
            if (!chroma) {
                front = {front[0], front[0], front[0]};
                behind = {behind[0], behind[0], behind[0]};
            }
            for (std::size_t c = 0; c < 3; ++c) {
                const float a = front[c];
                const float wf = (a * boost) * kernelFront[c];
                const float wb = (behind[c] * boost) * kernelBack[c];
                coverFront[c] = coverFront[c] + a;
                weightFront[c] = weightFront[c] + wf;
                weightBack[c] = weightBack[c] + wb;
                sumFront[c] = std::fma(wf, color[c], sumFront[c]);
                sumBack[c] = std::fma(wb, color[c], sumBack[c]);
            }
            coverBack = coverBack + behind[1];
            sumFront[3] = std::fma(front[1], color[3], sumFront[3]);
            sumBack[3] = std::fma(behind[1], color[3], sumBack[3]);
        }
    }
    std::array<float, 4> out{};
    std::array<float, 3> alpha{};
    for (std::size_t c = 0; c < 3; ++c) {
        alpha[c] = weightBack[c] > 0.0F ? std::min(coverFront[c], 1.0F)
                                        : (weightFront[c] > 0.0F ? 1.0F : 0.0F);
        const float front = weightFront[c] > 0.0F ? sumFront[c] / weightFront[c] : 0.0F;
        const float behind = weightBack[c] > 0.0F ? sumBack[c] / weightBack[c] : 0.0F;
        out[c] = std::fma(alpha[c], front - behind, behind);
    }
    const float coverage =
        coverBack > 0.0F ? std::min(coverFront[1], 1.0F) : (coverFront[1] > 0.0F ? 1.0F : 0.0F);
    const float front = coverFront[1] > 0.0F ? sumFront[3] / coverFront[1] : 0.0F;
    const float behind = coverBack > 0.0F ? sumBack[3] / coverBack : 0.0F;
    out[3] = clampf(std::fma(coverage, front - behind, behind), 0.0F, 1.0F);
    return out;
}

const std::string kDefocusGlsl =
    "layout(set = 0, binding = 0, std430) readonly buffer BloomDefocusData { vec4 values[]; } "
    "bloom_defocus_data;\n"
    "precise float bloom_ocio_cpu_div(float a, float b);\n"
    "float bdf(int i) { return bloom_defocus_data.values[i >> 2][i & 3]; }\n"
    // A slot's window in global positions (x, y, width, height) and the shift from a global
    // position to its atlas texel, read once a pixel rather than at every fetch. Slot -1 is the
    // colour input, placed at its own position in the output window (atlas index 0 is the output
    // window's origin); a slot s at its atlas position.
    "ivec4 bloom_defocus_rect(int slot) {\n"
    " int o = slot < 0 ? 24 : 28 + 8 * slot;\n"
    " return ivec4(int(bdf(o)), int(bdf(o + 1)), int(bdf(o + 2)), int(bdf(o + 3)));\n"
    "}\n"
    "ivec2 bloom_defocus_shift(int slot) {\n"
    " if (slot < 0) { return ivec2(-int(bdf(20)), -int(bdf(21))); }\n"
    " int o = 28 + 8 * slot;\n"
    " return ivec2(int(bdf(o + 4)) - int(bdf(o)), int(bdf(o + 5)) - int(bdf(o + 1)));\n"
    "}\n"
    "vec4 bloom_defocus_load(ivec4 r, ivec2 s, int x, int y) {\n"
    " if (x < r.x || y < r.y || x >= r.x + r.z || y >= r.y + r.w) { return vec4(0.0); }\n"
    " return imageLoad(bloom_ocio_input, ivec2(x, y) + s);\n"
    "}\n"
    "float bloom_defocus_density(float u, int base, int count) {\n"
    " precise float m = u * 8.0;\n"
    " int j = int(floor(m));\n"
    " if (j >= count - 1) { return bdf(base + count - 1); }\n"
    " precise float fraction = m - float(j);\n"
    " float t0 = bdf(base + j);\n"
    " float t1 = bdf(base + j + 1);\n"
    " precise float step_ = t1 - t0;\n"
    " return fma(fraction, step_, t0);\n"
    "}\n"
    "vec3 bloom_defocus_bokeh(float ox, float oy, float r, float spacing, ivec4 br, ivec2 bs, "
    "vec3 bm) {\n"
    " if (!(r >= 1.0)) { return vec3(1.0); }\n"
    " precise float sx = ox * spacing;\n"
    " precise float sy = oy * spacing;\n"
    " precise float ux = bloom_ocio_cpu_div(sx, r);\n"
    " precise float uy = bloom_ocio_cpu_div(sy, r);\n"
    " precise float px = fma(ux, bm.x, bm.y);\n"
    " precise float py = fma(uy, bm.x, bm.z);\n"
    " float x0 = floor(px);\n"
    " float y0 = floor(py);\n"
    " precise float fx = px - x0;\n"
    " precise float fy = py - y0;\n"
    " int ix = int(x0);\n"
    " int iy = int(y0);\n"
    " vec4 t00 = bloom_defocus_load(br, bs, ix, iy);\n"
    " vec4 t10 = bloom_defocus_load(br, bs, ix + 1, iy);\n"
    " vec4 t01 = bloom_defocus_load(br, bs, ix, iy + 1);\n"
    " vec4 t11 = bloom_defocus_load(br, bs, ix + 1, iy + 1);\n"
    " vec3 w = vec3(0.0);\n"
    " for (int c = 0; c < 3; ++c) {\n"
    "  precise float a = t10[c] - t00[c];\n"
    "  precise float top = fma(fx, a, t00[c]);\n"
    "  precise float b = t11[c] - t01[c];\n"
    "  precise float bottom = fma(fx, b, t01[c]);\n"
    "  precise float d = bottom - top;\n"
    "  precise float v = fma(fy, d, top);\n"
    "  w[c] = max(v, 0.0);\n"
    " }\n"
    " return w;\n"
    "}\n"
    "vec4 bloom_defocus(ivec2 c) {\n"
    " if (c.x >= int(bdf(22)) || c.y >= int(bdf(23))) { return imageLoad(bloom_ocio_input, c); }\n"
    " int gx = c.x + int(bdf(20));\n"
    " int gy = c.y + int(bdf(21));\n"
    " bool uniform_ = bdf(0) == 3.0;\n"
    // The tile maximum (slot 0) carries each pixel's signed circle of confusion in red.
    " bool hasTile = bdf(34) != 0.0;\n"
    " bool hasPre = bdf(42) != 0.0;\n"
    " bool hasBokeh = bdf(50) != 0.0 && bdf(1) == 2.0;\n"
    " ivec4 inR = bloom_defocus_rect(-1);\n"
    " ivec2 inS = bloom_defocus_shift(-1);\n"
    " ivec4 tileR = bloom_defocus_rect(0);\n"
    " ivec2 tileS = bloom_defocus_shift(0);\n"
    " ivec4 preR = bloom_defocus_rect(1);\n"
    " ivec2 preS = bloom_defocus_shift(1);\n"
    " ivec4 bokR = bloom_defocus_rect(2);\n"
    " ivec2 bokS = bloom_defocus_shift(2);\n"
    " vec3 bokM = vec3(bdf(60), bdf(61), bdf(62));\n"
    " int densityBase = int(bdf(13));\n"
    " int densityCount = int(bdf(11));\n"
    " float widest = bdf(16);\n"
    " float base = uniform_ ? bdf(19) : 0.0;\n"
    " vec4 tile = hasTile ? bloom_defocus_load(tileR, tileS, gx, gy) : vec4(0.0);\n"
    " float cp = hasTile ? tile.r : base;\n"
    " precise float radius = abs(cp);\n"
    " if (hasTile) { precise float t = tile.a * widest; radius = max(radius, t); }\n"
    " float red = bdf(5);\n"
    " precise float reach = radius * red;\n"
    " vec4 own = bloom_defocus_load(inR, inS, gx, gy);\n"
    " if (!(reach >= 0.5)) { return own; }\n"
    " float rings = bdf(2);\n"
    " precise float spacing = max(1.0, bloom_ocio_cpu_div(reach, rings));\n"
    " precise float invSpacing = bloom_ocio_cpu_div(1.0, spacing);\n"
    " int last = int(min(rings, floor(fma(reach, invSpacing, 0.5))));\n"
    " precise float pre = 0.0;\n"
    " if (hasPre) { precise float s1 = spacing - 1.0; precise float s2 = s1 * bdf(18); pre = "
    "clamp(s2, 0.0, 1.0); }\n"
    " bool chroma = bdf(7) != 0.0;\n"
    // Without chromatic bokeh every channel's disk is the green one, so its coverage and density
    // are worked out once and shared (the same arithmetic, so the same values).
    " int lanes = chroma ? 3 : 1;\n"
    " vec3 scales = vec3(chroma ? red : 1.0, 1.0, chroma ? bdf(6) : 1.0);\n"
    " float gain = bdf(8);\n"
    " float inverseThreshold = bdf(9);\n"
    " float threshold = bdf(15);\n"
    " precise vec3 weightFront = vec3(0.0);\n"
    " precise vec3 weightBack = vec3(0.0);\n"
    " precise vec3 coverFront = vec3(0.0);\n"
    " precise vec4 sumFront = vec4(0.0);\n"
    " precise vec4 sumBack = vec4(0.0);\n"
    " precise float coverBack = 0.0;\n"
    " float acp = abs(cp);\n"
    " for (int ring = 0; ring <= last; ++ring) {\n"
    "  int first = ring == 0 ? 0 : 1 + 4 * ring * (ring - 1);\n"
    "  int count = ring == 0 ? 1 : 8 * ring;\n"
    "  for (int i = 0; i < count; ++i) {\n"
    "   int e = 64 + 8 * (first + i);\n"
    "   int dx = int(floor(fma(bdf(e), spacing, 0.5)));\n"
    "   int dy = int(floor(fma(bdf(e + 1), spacing, 0.5)));\n"
    "   int qx = gx + dx;\n"
    "   int qy = gy + dy;\n"
    "   precise vec4 color = bloom_defocus_load(inR, inS, qx, qy);\n"
    "   if (ring > 0 && pre > 0.0) {\n"
    "    vec4 filtered = bloom_defocus_load(preR, preS, qx, qy);\n"
    "    for (int ch = 0; ch < 4; ++ch) { precise float d = filtered[ch] - color[ch]; color[ch] = "
    "fma(pre, d, color[ch]); }\n"
    "   }\n"
    "   float cq = hasTile ? bloom_defocus_load(tileR, tileS, qx, qy).r : base;\n"
    "   precise float rho = bdf(e + 2) * spacing;\n"
    "   precise float difference = cp - cq;\n"
    "   precise float phi = clamp(difference + 0.5, 0.0, 1.0);\n"
    "   precise float boost = 1.0;\n"
    "   if (gain > 0.0) {\n"
    "    precise float luma = 0.0;\n"
    "    if (color.a > 0.0) {\n"
    "     precise float l0 = 0.2126 * color.r;\n"
    "     precise float l1 = fma(0.7152, color.g, l0);\n"
    "     precise float l2 = fma(0.0722, color.b, l1);\n"
    "     luma = bloom_ocio_cpu_div(l2, color.a);\n"
    "    }\n"
    "    precise float over = (luma - threshold) * inverseThreshold;\n"
    "    boost = fma(gain, clamp(over, 0.0, 1.0), 1.0);\n"
    "   }\n"
    "   float acq = abs(cq);\n"
    "   float back = min(acq, acp);\n"
    "   vec3 kernelFront = vec3(1.0);\n"
    "   vec3 kernelBack = vec3(1.0);\n"
    "   if (hasBokeh) {\n"
    "    kernelFront = bloom_defocus_bokeh(bdf(e + 4), bdf(e + 5), acq, spacing, bokR, bokS, "
    "bokM);\n"
    "    kernelBack = bloom_defocus_bokeh(bdf(e + 4), bdf(e + 5), back, spacing, bokR, bokS, "
    "bokM);\n"
    "   }\n"
    "   precise vec3 front_ = vec3(0.0);\n"
    "   precise vec3 behind_ = vec3(0.0);\n"
    "   for (int ch = 0; ch < lanes; ++ch) {\n"
    "    int lane = chroma ? ch : 1;\n"
    "    precise float rf = acq * scales[lane];\n"
    "    precise float rb = back * scales[lane];\n"
    "    precise float df = rf - rho;\n"
    "    precise float db = rb - rho;\n"
    "    precise float coverF = clamp(fma(df, invSpacing, 0.5), 0.0, 1.0);\n"
    "    precise float coverB = clamp(fma(db, invSpacing, 0.5), 0.0, 1.0);\n"
    "    precise float u = rf * invSpacing;\n"
    "    precise float pf = phi * coverF;\n"
    "    front_[ch] = pf * bloom_defocus_density(u, densityBase, densityCount);\n"
    "    precise float behind = 1.0 - phi;\n"
    "    behind_[ch] = behind * coverB;\n"
    "   }\n"
    "   if (!chroma) { front_ = vec3(front_.x); behind_ = vec3(behind_.x); }\n"
    "   for (int ch = 0; ch < 3; ++ch) {\n"
    "    precise float a = front_[ch];\n"
    "    precise float bc = behind_[ch];\n"
    "    precise float ab = a * boost;\n"
    "    precise float wf = ab * kernelFront[ch];\n"
    "    precise float bb = bc * boost;\n"
    "    precise float wb = bb * kernelBack[ch];\n"
    "    coverFront[ch] = coverFront[ch] + a;\n"
    "    weightFront[ch] = weightFront[ch] + wf;\n"
    "    weightBack[ch] = weightBack[ch] + wb;\n"
    "    sumFront[ch] = fma(wf, color[ch], sumFront[ch]);\n"
    "    sumBack[ch] = fma(wb, color[ch], sumBack[ch]);\n"
    "   }\n"
    "   coverBack = coverBack + behind_.y;\n"
    "   sumFront.a = fma(front_.y, color.a, sumFront.a);\n"
    "   sumBack.a = fma(behind_.y, color.a, sumBack.a);\n"
    "  }\n"
    " }\n"
    " precise vec4 o = vec4(0.0);\n"
    " vec3 alpha = vec3(0.0);\n"
    " for (int ch = 0; ch < 3; ++ch) {\n"
    "  alpha[ch] = weightBack[ch] > 0.0 ? min(coverFront[ch], 1.0) : (weightFront[ch] > 0.0 ? 1.0 "
    ": 0.0);\n"
    "  precise float front = weightFront[ch] > 0.0 ? bloom_ocio_cpu_div(sumFront[ch], "
    "weightFront[ch]) : 0.0;\n"
    "  precise float behind = weightBack[ch] > 0.0 ? bloom_ocio_cpu_div(sumBack[ch], "
    "weightBack[ch]) : 0.0;\n"
    "  precise float d = front - behind;\n"
    "  o[ch] = fma(alpha[ch], d, behind);\n"
    " }\n"
    " float coverage = coverBack > 0.0 ? min(coverFront.g, 1.0) : (coverFront.g > 0.0 ? 1.0 : "
    "0.0);\n"
    " precise float fa = coverFront.g > 0.0 ? bloom_ocio_cpu_div(sumFront.a, coverFront.g) : "
    "0.0;\n"
    " precise float ba = coverBack > 0.0 ? bloom_ocio_cpu_div(sumBack.a, coverBack) : 0.0;\n"
    " precise float da = fa - ba;\n"
    " o.a = clamp(fma(coverage, da, ba), 0.0, 1.0);\n"
    " return o;\n"
    "}\n";

} // namespace

// --- The depth map ------------------------------------------------------------------------------

std::optional<PreparedColorGrade> prepareDepthMapGrade(const DepthMapKernel& kernel,
                                                       const std::span<const double> values) {
    const bool coc = kernel.output == DepthMapOutput::CircleOfConfusion;
    if (static_cast<std::uint8_t>(kernel.output) > 1 || kernel.channel > 3 ||
        !choice(kernel.interpretation, document::kDepthInterpretationMist) ||
        !choice(kernel.mistFalloff, document::kMistFalloffInverseQuadratic) ||
        !choice(kernel.background, document::kDepthBackgroundUnaffected) ||
        !choice(kernel.mode, document::kDefocusModeSizeMap) ||
        !choice(kernel.focusFrom, document::kFocusFromPoint) ||
        !choice(kernel.blurRegion, document::kDefocusBlurNearOnly) ||
        !choice(kernel.units, document::kSceneUnitInches) || !choice(kernel.edgeExpand, 4) ||
        !choice(kernel.falloff, document::kFogFalloffCurve))
        return std::nullopt;
    const double distance = document::kMaximumDepthDistance;
    if (values.size() < 2 || !within(values[0], 0.0, distance) ||
        !within(values[1], 0.0, distance))
        return std::nullopt;
    PreparedColorGrade grade{ColorGradeKind::DepthMap, {}, false};
    auto& d = grade.data;
    const auto header = [&] {
        d[1] = static_cast<float>(kernel.channel);
        d[2] = static_cast<float>(kernel.interpretation);
        d[3] = kernel.invert ? 1.0F : 0.0F;
        d[4] = static_cast<float>(kernel.mistFalloff);
        d[5] = static_cast<float>(kernel.background);
        d[6] = static_cast<float>(values[0]);
        d[7] = static_cast<float>(values[1]);
    };
    if (coc) {
        if (values.size() != 11)
            return std::nullopt;
        const double size = document::kMaximumDefocusSize;
        if (!within(values[2], 0.0, distance) || !within(values[3], -1.0e6, 1.0e6) ||
            !within(values[4], -1.0e6, 1.0e6) || !within(values[5], 0.0, distance) ||
            !within(values[6], 0.0, size) || !within(values[7], 0.0, size) ||
            !within(values[8], 1.0, 5000.0) || !within(values[9], 1.0, 100.0) ||
            !within(values[10], 0.5, 128.0))
            return std::nullopt;
        d.assign(kDepthMapCocFloats, 0.0F);
        header();
        d[0] = 0.0F;
        d[8] = static_cast<float>(kernel.mode);
        d[9] = static_cast<float>(kernel.focusFrom);
        d[10] = static_cast<float>(kernel.blurRegion);
        d[11] = static_cast<float>(kernel.edgeExpand);
        d[12] = static_cast<float>(values[2]);
        d[13] = static_cast<float>(0.5 * values[5]);
        d[14] = static_cast<float>(values[6]);
        d[15] = static_cast<float>(values[7]);
        d[16] = static_cast<float>(values[8]);
        d[17] = static_cast<float>(values[10]);
        d[18] = static_cast<float>(1000.0 / document::sceneUnitsPerMetre(kernel.units));
        d[19] = static_cast<float>(1.0 / (2.0 * values[9]));
        d[20] = static_cast<float>(values[3]);
        d[21] = static_cast<float>(values[4]);
        d[22] = static_cast<float>(kernel.mode == document::kDefocusModeSizeMap ? values[6]
                                                                                 : values[7]);
        return grade;
    }
    if (values.size() < 6 || !within(values[2], 0.0, 1.0) || !within(values[3], 0.0, distance) ||
        !within(values[4], 0.0, distance) || !within(values[5], 0.0, 1.0e6))
        return std::nullopt;
    d.assign(kDepthFogFloats, 0.0F);
    header();
    d[0] = 1.0F;
    d[8] = static_cast<float>(kernel.falloff);
    d[9] = static_cast<float>(values[2]);
    d[10] = static_cast<float>(values[3]);
    const double range = values[4] - values[3];
    d[11] = range > 0.0 ? static_cast<float>(1.0 / range) : 0.0F;
    d[12] = static_cast<float>(values[5]);
    d[14] = static_cast<float>(values[4]);
    // The curve's samples: count, then (x, y) pairs (sampleColorGradeOperands).
    document::PathValue curve = document::defaultDepthFogCurve();
    if (values.size() > 6) {
        const auto count = values[6];
        if (!std::isfinite(count) || count < 0.0 ||
            count > static_cast<double>(document::kMaximumDepthCurvePoints) ||
            values.size() != 7 + 2 * static_cast<std::size_t>(count))
            return std::nullopt;
        std::vector<document::Vec2d> points;
        for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i)
            points.push_back({values[7 + 2 * i], values[8 + 2 * i]});
        curve = document::gradeCurvePath(points);
    }
    const auto shape = prepareGradeCurveShape(document::GradeCurve::Master, curve);
    if (!shape)
        return std::nullopt;
    d[13] = 1.0F;
    for (std::size_t j = 0; j < kDepthFogCurveEntries; ++j) {
        const double t = static_cast<double>(j) / 256.0;
        const double y = sampleGradeCurve(*shape, t);
        if (!std::isfinite(y))
            return std::nullopt;
        d[16 + j] = static_cast<float>(std::clamp(y, 0.0, 1.0));
    }
    return grade;
}

std::optional<std::vector<float>> depthMapProgramData(const PreparedColorGrade& grade,
                                                      const render::ImageWindow& data,
                                                      const render::ImageWindow& display,
                                                      const double horizontalScale) {
    if (grade.kind != ColorGradeKind::DepthMap || grade.data.empty() ||
        !std::isfinite(horizontalScale) || !(horizontalScale > 0.0))
        return std::nullopt;
    if (grade.data[0] != 0.0F)
        return grade.data.size() == kDepthFogFloats ? std::optional{grade.data} : std::nullopt;
    if (grade.data.size() != kDepthMapCocFloats)
        return std::nullopt;
    auto d = grade.data;
    const double width = static_cast<double>(data.extent().width());
    const double height = static_cast<double>(data.extent().height());
    const double frameWidth = static_cast<double>(display.extent().width()) / horizontalScale;
    // The focus point is a full-resolution offset from the display window's centre.
    const double focusX = std::floor(static_cast<double>(display.originX()) +
                                     0.5 * static_cast<double>(display.extent().width()) +
                                     static_cast<double>(d[20]) * horizontalScale -
                                     static_cast<double>(data.originX()));
    const double focusY = std::floor(static_cast<double>(display.originY()) +
                                     0.5 * static_cast<double>(display.extent().height()) +
                                     static_cast<double>(d[21]) * horizontalScale -
                                     static_cast<double>(data.originY()));
    const double widest = static_cast<double>(d[22]) * horizontalScale;
    if (width > kExactInteger || height > kExactInteger || !std::isfinite(frameWidth) ||
        std::abs(focusX) > kExactInteger || std::abs(focusY) > kExactInteger ||
        !std::isfinite(widest))
        return std::nullopt;
    d[24] = static_cast<float>(horizontalScale);
    d[25] = static_cast<float>(frameWidth * static_cast<double>(d[19]));
    d[26] = static_cast<float>(focusX);
    d[27] = static_cast<float>(focusY);
    d[28] = static_cast<float>(widest);
    d[29] = widest > 0.0 ? static_cast<float>(1.0 / widest) : 0.0F;
    d[30] = static_cast<float>(width);
    d[31] = static_cast<float>(height);
    if (!std::ranges::all_of(d, finite))
        return std::nullopt;
    return d;
}

bool validDepthMapProgramData(const std::vector<float>& d) noexcept {
    if (d.empty() || !std::ranges::all_of(d, finite))
        return false;
    if (d[0] == 0.0F)
        return d.size() == kDepthMapCocFloats && d[1] >= 0.0F && d[1] <= 3.0F &&
               d[11] >= 0.0F && d[11] <= 4.0F && d[22] >= 0.0F && d[24] > 0.0F;
    return d[0] == 1.0F && d.size() == kDepthFogFloats && d[1] >= 0.0F && d[1] <= 3.0F;
}

const std::string& depthMapProgramText() { return kDepthMapGlsl; }

ImageEffectResult applyDepthMap(const PreparedColorGrade& grade, const render::Rgba32fImage& input,
                                const double horizontalScale, const std::size_t budget,
                                const CancellationToken& cancellation,
                                const OperationIndex operation,
                                const EvaluationProgressCallback& progress,
                                CpuRowBandExecutor* const rowBands) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    const auto& descriptor = *input.descriptor();
    const auto window = descriptor.dataWindow();
    const auto data =
        depthMapProgramData(grade, window, descriptor.displayWindow(), horizontalScale);
    if (!data)
        return failed(EvaluationDiagnosticCode::InvalidParameter,
                      "Depth map geometry cannot be represented");
    const auto view = input.view();
    if (!view)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Depth image is invalid");
    std::vector<std::span<const render::Rgba32f>> rows;
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
        const auto row = view.value()->row(y);
        if (!row)
            return failed(EvaluationDiagnosticCode::InternalInvariant,
                          "Depth image row is unavailable");
        rows.push_back(*row.value());
    }
    const int width = static_cast<int>(window.extent().width());
    const int height = static_cast<int>(window.extent().height());
    const auto inside = [&](const int x, const int y) {
        return x >= 0 && y >= 0 && x < width && y < height;
    };
    const auto fetch = [&](const int x, const int y) -> std::array<float, 4> {
        if (!inside(x, y))
            return {};
        const auto& p = rows[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
        return {p.red(), p.green(), p.blue(), p.alpha()};
    };
    auto builder = render::Rgba32fImageBuilder::create(descriptor, budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Depth map output exceeds the pixel budget");
    auto& image = *builder.value();
    const auto total = static_cast<std::uint32_t>(height);
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, total});
    const float* k = data->data();
    const auto outcome = runRowBandPass(
        rowBands, cancellation, total, window.originY(),
        [&](const std::int64_t y) -> std::optional<EvaluationDiagnostic> {
            auto row = image.row(y);
            if (!row)
                return EvaluationDiagnostic{EvaluationDiagnosticCode::InternalInvariant,
                                            DiagnosticSeverity::Error,
                                            {},
                                            "Depth map row is unavailable",
                                            {}};
            const int j = static_cast<int>(y - window.originY());
            for (int x = 0; x < width; ++x) {
                if ((x & 1023) == 0 && cancellation.isCancellationRequested())
                    return std::nullopt;
                const auto p = depthMapPixel(k, x, j, fetch, inside);
                const auto pixel = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
                if (!pixel)
                    return EvaluationDiagnostic{EvaluationDiagnosticCode::InvalidPixel,
                                                DiagnosticSeverity::Error,
                                                {},
                                                "Depth map produced an invalid pixel",
                                                {}};
                (*row.value())[static_cast<std::size_t>(x)] = *pixel.value();
            }
            return std::nullopt;
        });
    if (outcome.cancelled || cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    if (outcome.incomplete || outcome.failure) {
        ImageEffectResult result;
        result.diagnostic = outcome.failure ? *outcome.failure
                                            : EvaluationDiagnostic{
                                                  EvaluationDiagnosticCode::AllocationFailure,
                                                  DiagnosticSeverity::Error,
                                                  {},
                                                  "A depth map row band could not complete",
                                                  {}};
        return result;
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, total, total});
    auto frozen = std::move(image).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Depth map publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

// --- The defocus gather -------------------------------------------------------------------------

std::optional<PreparedColorGrade> prepareDefocusGrade(const DefocusKernel& kernel,
                                                      const std::span<const double> values) {
    if (values.size() != 7 || !choice(kernel.mode, document::kDefocusModeUniform) ||
        !choice(kernel.shape, document::kBokehImage) || kernel.blades < 3 || kernel.blades > 16 ||
        !choice(kernel.quality, document::kDefocusQualityMaximum))
        return std::nullopt;
    const double size = document::kMaximumDefocusSize;
    if (!within(values[0], 0.0, size) || !within(values[1], 0.0, size) ||
        !within(values[2], -1.0e6, 1.0e6) || !within(values[3], 0.0, 1.0) ||
        !within(values[4], 0.0, 100.0) || !within(values[5], 0.0, 1.0e5) ||
        !within(values[6], 0.0, 1.0))
        return std::nullopt;
    const bool widestIsSize = kernel.mode == document::kDefocusModeSizeMap ||
                              kernel.mode == document::kDefocusModeUniform;
    const double widest = widestIsSize ? values[0] : values[1];
    const int rings = static_cast<int>(
        document::kDefocusQualityRings[static_cast<std::size_t>(kernel.quality)]);
    const std::size_t samples = 1 + 4 * static_cast<std::size_t>(rings) * static_cast<std::size_t>(rings + 1);
    const std::size_t entries = static_cast<std::size_t>(kDensitySteps) * static_cast<std::size_t>(rings + 1) + 1;
    const std::size_t densityBase = kPatternBase + 8 * samples;
    PreparedColorGrade grade{ColorGradeKind::Defocus,
                             std::vector<float>(densityBase + (entries + 3) / 4 * 4, 0.0F),
                             widest == 0.0};
    auto& d = grade.data;
    const double chroma = values[6];
    const bool image = kernel.shape == document::kBokehImage &&
                       kernel.slotImages[kDefocusBokehSlot] >= 0;
    d[0] = static_cast<float>(kernel.mode);
    d[1] = image ? 2.0F : static_cast<float>(std::min<std::int64_t>(kernel.shape, 1));
    d[2] = static_cast<float>(rings);
    d[3] = static_cast<float>(samples);
    d[4] = static_cast<float>(widest);
    d[5] = static_cast<float>(1.0 + 0.25 * chroma);
    d[6] = static_cast<float>(1.0 - 0.2 * chroma);
    d[7] = chroma > 0.0 ? 1.0F : 0.0F;
    d[8] = static_cast<float>(values[4]);
    d[9] = values[5] > 0.0 ? static_cast<float>(1.0 / values[5]) : 1.0e30F;
    d[10] = static_cast<float>(values[0]);
    d[11] = static_cast<float>(entries);
    d[12] = static_cast<float>(kPatternBase);
    d[13] = static_cast<float>(densityBase);
    d[15] = static_cast<float>(values[5]);
    // The ring pattern at a unit spacing: ring k holds 8k samples at radius k, odd rings turned
    // half a step. A polygon's sample stands at its distance over the polygon's radius in its
    // direction (1 at a vertex, cos(pi / blades) mid-edge), moved to the circle by the curvature.
    const double rotation = values[2] * std::numbers::pi / 180.0;
    const double roundness = values[3];
    const bool polygon = kernel.shape == document::kBokehPolygon;
    const double blades = static_cast<double>(kernel.blades);
    const double sector = 2.0 * std::numbers::pi / blades;
    std::vector<double> radii;
    radii.reserve(samples);
    std::size_t index = 0;
    for (int ring = 0; ring <= rings; ++ring) {
        const int count = ring == 0 ? 1 : 8 * ring;
        for (int i = 0; i < count; ++i, ++index) {
            const double theta =
                2.0 * std::numbers::pi * (static_cast<double>(i) + (ring % 2 == 1 ? 0.5 : 0.0)) /
                static_cast<double>(count);
            const double ux = static_cast<double>(ring) * std::cos(theta);
            const double uy = static_cast<double>(ring) * std::sin(theta);
            double rho = static_cast<double>(ring);
            if (polygon && ring > 0) {
                double local = std::fmod(theta - rotation, sector);
                if (local < 0.0)
                    local += sector;
                const double g =
                    std::cos(std::numbers::pi / blades) / std::cos(local - 0.5 * sector);
                rho = rho / (g + (1.0 - g) * roundness);
            }
            radii.push_back(rho);
            float* e = d.data() + kPatternBase + 8 * index;
            e[0] = static_cast<float>(ux);
            e[1] = static_cast<float>(uy);
            e[2] = static_cast<float>(rho);
            e[4] = static_cast<float>(std::cos(rotation) * ux + std::sin(rotation) * uy);
            e[5] = static_cast<float>(-std::sin(rotation) * ux + std::cos(rotation) * uy);
        }
    }
    // The density: (pi / 4) / A(u), A the area the pattern's samples cover at radius u.
    const double quarter = std::numbers::pi / 4.0;
    for (std::size_t j = 0; j < entries; ++j) {
        const double u = static_cast<double>(j) / kDensitySteps;
        double area = 0.0;
        for (const double rho : radii)
            area += quarter * std::clamp(u - rho + 0.5, 0.0, 1.0);
        d[densityBase + j] = static_cast<float>(quarter / area);
    }
    return grade;
}

std::int64_t defocusReach(const PreparedColorGrade& grade, const double horizontalScale) {
    if (grade.kind != ColorGradeKind::Defocus || grade.data.size() < kDefocusHeaderFloats)
        return 0;
    const double reach = static_cast<double>(grade.data[4]) * horizontalScale *
                         static_cast<double>(grade.data[5]);
    const double spacing = std::max(1.0, reach / static_cast<double>(grade.data[2]));
    return static_cast<std::int64_t>(std::ceil(reach + 0.5 * spacing));
}

bool defocusPrefilterUsed(const PreparedColorGrade& grade, const double horizontalScale) {
    if (grade.kind != ColorGradeKind::Defocus || grade.data.size() < kDefocusHeaderFloats)
        return true;
    const double reach = static_cast<double>(grade.data[4]) * horizontalScale *
                         static_cast<double>(grade.data[5]);
    return std::max(1.0, reach / static_cast<double>(grade.data[2])) > 1.0;
}

std::optional<std::vector<float>> defocusProgramData(const PreparedColorGrade& grade,
                                                     const DefocusPlacement& placement,
                                                     const double horizontalScale) {
    if (grade.kind != ColorGradeKind::Defocus || grade.data.size() < kDefocusHeaderFloats ||
        !std::isfinite(horizontalScale) || !(horizontalScale > 0.0))
        return std::nullopt;
    auto d = grade.data;
    const double widest = static_cast<double>(d[4]) * horizontalScale;
    const double reach = widest * static_cast<double>(d[5]);
    const double spacing = std::max(1.0, reach / static_cast<double>(d[2]));
    const auto grow = defocusReach(grade, horizontalScale);
    const auto exact = [](const std::int64_t value) {
        return std::abs(static_cast<double>(value)) <= kExactInteger;
    };
    const auto& color = placement.color;
    const std::int64_t outX = color.originX - grow;
    const std::int64_t outY = color.originY - grow;
    const std::int64_t outW = color.width + 2 * grow;
    const std::int64_t outH = color.height + 2 * grow;
    if (!exact(outX) || !exact(outY) || !exact(outX + outW) || !exact(outY + outH))
        return std::nullopt;
    d[16] = static_cast<float>(widest);
    d[17] = static_cast<float>(spacing);
    d[18] = spacing > 1.0 ? static_cast<float>(1.0 / (spacing - 1.0)) : 0.0F;
    d[19] = static_cast<float>(static_cast<double>(d[10]) * horizontalScale);
    d[20] = static_cast<float>(outX);
    d[21] = static_cast<float>(outY);
    d[22] = static_cast<float>(outW);
    d[23] = static_cast<float>(outH);
    d[24] = static_cast<float>(color.originX);
    d[25] = static_cast<float>(color.originY);
    d[26] = static_cast<float>(color.width);
    d[27] = static_cast<float>(color.height);
    for (std::size_t s = 0; s < kDefocusSlots; ++s) {
        const auto& slot = placement.slots[s];
        float* e = d.data() + slotOffset(s);
        if (slot.width <= 0 || slot.height <= 0) {
            std::fill(e, e + 8, 0.0F);
            continue;
        }
        for (const auto value : {slot.originX, slot.originY, slot.originX + slot.width,
                                 slot.originY + slot.height, slot.atlasX + slot.width,
                                 slot.atlasY + slot.height})
            if (!exact(value))
                return std::nullopt;
        e[0] = static_cast<float>(slot.originX);
        e[1] = static_cast<float>(slot.originY);
        e[2] = static_cast<float>(slot.width);
        e[3] = static_cast<float>(slot.height);
        e[4] = static_cast<float>(slot.atlasX);
        e[5] = static_cast<float>(slot.atlasY);
        e[6] = 1.0F;
        e[7] = 0.0F;
    }
    // The bokeh image's square: its half side and its centre less half a pixel.
    const auto& bokeh = placement.slots[kDefocusBokehSlot];
    if (bokeh.width > 0 && bokeh.height > 0) {
        const double side = static_cast<double>(std::max(bokeh.width, bokeh.height));
        d[60] = static_cast<float>(0.5 * side);
        d[61] = static_cast<float>(static_cast<double>(bokeh.originX) +
                                   0.5 * static_cast<double>(bokeh.width) - 0.5);
        d[62] = static_cast<float>(static_cast<double>(bokeh.originY) +
                                   0.5 * static_cast<double>(bokeh.height) - 0.5);
    } else {
        d[60] = d[61] = d[62] = 0.0F;
    }
    if (!std::ranges::all_of(d, finite))
        return std::nullopt;
    return d;
}

bool validDefocusProgramData(const std::vector<float>& d) noexcept {
    if (d.size() < kDefocusHeaderFloats || !std::ranges::all_of(d, finite))
        return false;
    const auto samples = static_cast<std::size_t>(d[3]);
    const auto entries = static_cast<std::size_t>(d[11]);
    return d[2] >= 1.0F && d[2] <= 16.0F && d[3] == static_cast<float>(samples) &&
           samples == 1 + 4 * static_cast<std::size_t>(d[2]) * (static_cast<std::size_t>(d[2]) + 1) &&
           d[12] == static_cast<float>(kPatternBase) &&
           d[13] == static_cast<float>(kPatternBase + 8 * samples) && entries >= 2 &&
           d.size() == kPatternBase + 8 * samples + (entries + 3) / 4 * 4 && d[22] >= 0.0F &&
           d[23] >= 0.0F && d[5] >= 1.0F;
}

const std::string& defocusProgramText() { return kDefocusGlsl; }

ImageEffectResult applyDefocus(const PreparedColorGrade& grade, const render::Rgba32fImage& color,
                               const std::span<const render::Rgba32fImage* const> slots,
                               const double horizontalScale, const std::size_t budget,
                               const CancellationToken& cancellation,
                               const OperationIndex operation,
                               const EvaluationProgressCallback& progress,
                               CpuRowBandExecutor* const rowBands) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    struct Rows final {
        std::vector<std::span<const render::Rgba32f>> rows;
        std::int64_t originX = 0, originY = 0, width = 0, height = 0;
    };
    std::vector<render::Rgba32fImageView> views;
    views.reserve(1 + kDefocusSlots);
    const auto rowsOf = [&](const render::Rgba32fImage& image, Rows& rows) -> bool {
        const auto view = image.view();
        if (!view)
            return false;
        views.push_back(*view.value());
        const auto window = image.descriptor()->dataWindow();
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
    Rows colorRows;
    std::array<Rows, kDefocusSlots> slotRows;
    DefocusPlacement placement;
    if (!rowsOf(color, colorRows))
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Defocus image is invalid");
    placement.color = {colorRows.originX, colorRows.originY, colorRows.width, colorRows.height,
                       0, 0};
    for (std::size_t s = 0; s < slots.size() && s < kDefocusSlots; ++s) {
        if (slots[s] == nullptr)
            continue;
        if (!rowsOf(*slots[s], slotRows[s]))
            return failed(EvaluationDiagnosticCode::InvalidPixel, "Defocus image is invalid");
        placement.slots[s] = {slotRows[s].originX, slotRows[s].originY, slotRows[s].width,
                              slotRows[s].height, 0, 0};
    }
    const auto data = defocusProgramData(grade, placement, horizontalScale);
    if (!data)
        return failed(EvaluationDiagnosticCode::InvalidParameter,
                      "Defocus geometry cannot be represented");
    const auto& descriptor = *color.descriptor();
    const auto grow = defocusReach(grade, horizontalScale);
    const auto window = render::ImageWindow::create(
        colorRows.originX - grow, colorRows.originY - grow,
        static_cast<std::uint64_t>(colorRows.width + 2 * grow),
        static_cast<std::uint64_t>(colorRows.height + 2 * grow));
    if (!window)
        return failed(EvaluationDiagnosticCode::InvalidParameter, "Defocus window overflows");
    const auto output = render::Rgba32fImageDescriptor::create(
        *window.value(), descriptor.displayWindow(), descriptor.pixelAspect());
    if (!output)
        return failed(EvaluationDiagnosticCode::InvalidParameter, "Defocus window is invalid");
    const auto read = [](const Rows& rows, const int x, const int y) -> std::array<float, 4> {
        const auto i = static_cast<std::int64_t>(x) - rows.originX;
        const auto j = static_cast<std::int64_t>(y) - rows.originY;
        if (i < 0 || j < 0 || i >= rows.width || j >= rows.height)
            return {};
        const auto& p = rows.rows[static_cast<std::size_t>(j)][static_cast<std::size_t>(i)];
        return {p.red(), p.green(), p.blue(), p.alpha()};
    };
    const float* k = data->data();
    const auto fetch = [&](const int slot, const int x, const int y) -> std::array<float, 4> {
        if (slot < 0)
            return read(colorRows, x, y);
        if (k[slotOffset(static_cast<std::size_t>(slot)) + 6] == 0.0F)
            return {};
        return read(slotRows[static_cast<std::size_t>(slot)], x, y);
    };
    auto builder = render::Rgba32fImageBuilder::create(*output.value(), budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Defocus output exceeds the pixel budget");
    auto& image = *builder.value();
    const auto out = *window.value();
    const auto total = static_cast<std::uint32_t>(out.extent().height());
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, total});
    const auto outcome = runRowBandPass(
        rowBands, cancellation, total, out.originY(),
        [&](const std::int64_t y) -> std::optional<EvaluationDiagnostic> {
            auto row = image.row(y);
            if (!row)
                return EvaluationDiagnostic{EvaluationDiagnosticCode::InternalInvariant,
                                            DiagnosticSeverity::Error,
                                            {},
                                            "Defocus row is unavailable",
                                            {}};
            for (std::size_t column = 0; column < row.value()->size(); ++column) {
                if ((column & 255U) == 0 && cancellation.isCancellationRequested())
                    return std::nullopt;
                const auto p = defocusPixel(
                    k, static_cast<int>(out.originX() + static_cast<std::int64_t>(column)),
                    static_cast<int>(y), fetch);
                const auto pixel = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
                if (!pixel)
                    return EvaluationDiagnostic{EvaluationDiagnosticCode::InvalidPixel,
                                                DiagnosticSeverity::Error,
                                                {},
                                                "Defocus produced an invalid pixel",
                                                {}};
                (*row.value())[column] = *pixel.value();
            }
            return std::nullopt;
        });
    if (outcome.cancelled || cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    if (outcome.incomplete || outcome.failure) {
        ImageEffectResult result;
        result.diagnostic = outcome.failure ? *outcome.failure
                                            : EvaluationDiagnostic{
                                                  EvaluationDiagnosticCode::AllocationFailure,
                                                  DiagnosticSeverity::Error,
                                                  {},
                                                  "A defocus row band could not complete",
                                                  {}};
        return result;
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, total, total});
    auto frozen = std::move(image).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Defocus publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

} // namespace bloom::runtime::detail
