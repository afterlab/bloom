#pragma once

// #546: the GLSL of the gradient interpolation, colour and hue modes (the operations of
// document::gradient_math::sample in the same order), shared by the native GradientV1 shader,
// which carries this exact text (the build checks it), and Colorize's Gradient Map program. The
// including shader first defines
//   struct BloomGradientStop { float position; float inverseSpan; vec4 color; };
//   BloomGradientStop bloom_gradient_stop(int index);
// over its sorted stop table.

#include <string_view>

namespace bloom::render {

inline constexpr std::string_view kGradientModesGlsl = R"glsl(// #546: gradient interpolation, colour and hue modes, the operations of
// document::gradient_math::sample in the same order. The including shader defines
// BloomGradientStop bloom_gradient_stop(int index) over its sorted stop table.
float bloom_gm_unit(float v) { return min(max(v, 0.0), 1.0); }

float bloom_gm_exp2_unit(float x) {
    float whole = floor(x);
    precise float f = x - whole;
    precise float p = 1.01780863e-07;
    p = fma(p, f, 1.32154867e-06);
    p = fma(p, f, 1.52527336e-05);
    p = fma(p, f, 0.000154035297);
    p = fma(p, f, 0.00133335579);
    p = fma(p, f, 0.00961812865);
    p = fma(p, f, 0.0555041097);
    p = fma(p, f, 0.240226507);
    p = fma(p, f, 0.693147182);
    p = fma(p, f, 1.0);
    return ldexp(p, int(whole));
}

float bloom_gm_shape(int interpolation, float u) {
    if (interpolation == 2) {
        precise float u2 = u * u;
        precise float three = 3.0 * u2;
        precise float two = (2.0 * u2) * u;
        return bloom_gm_unit(three - two);
    }
    if (interpolation == 5) {
        if (u == 0.0) return 0.0;
        precise float scaled = 10.0 * u;
        precise float x = scaled - 10.0;
        precise float shifted = bloom_gm_exp2_unit(x) - 0.0009765625;
        precise float r = shifted * 1.00097752;
        return bloom_gm_unit(r);
    }
    return u;
}

float bloom_gm_weighted(float wa, float a, float wb, float b) {
    precise float first = wa * a;
    precise float second = wb * b;
    precise float r = first + second;
    return r;
}

vec3 bloom_gm_rgb_to_hsv(vec3 c) {
    float r = c.r;
    float g = c.g;
    float b = c.b;
    precise float k = 0.0;
    if (g < b) { float s = g; g = b; b = s; k = -1.0; }
    float minimum = b;
    if (r < g) { float s = r; r = g; g = s; k = -0.333333343 - k; minimum = min(g, b); }
    precise float chroma = r - minimum;
    precise float scaled = 6.0 * chroma;
    precise float denominator = scaled + 1e-20;
    precise float q = (g - b) / denominator;
    precise float hue = abs(k + q);
    precise float divisor = r + 1e-20;
    precise float saturation = divisor != 0.0 ? chroma / divisor : 0.0;
    return vec3(hue, saturation, r);
}

vec3 bloom_gm_rgb_to_hsl(vec3 c) {
    float high = max(max(c.r, c.g), c.b);
    float low = min(min(c.r, c.g), c.b);
    precise float sum = high + low;
    precise float mean = sum * 0.5;
    float lightness = min(1.0, mean);
    if (high == low) return vec3(0.0, 0.0, lightness);
    precise float d = high - low;
    precise float divisor = lightness > 0.5 ? (2.0 - high) - low : high + low;
    precise float saturation = divisor != 0.0 ? d / divisor : 0.0;
    precise float hue;
    if (high == c.r) hue = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
    else if (high == c.g) hue = (c.b - c.r) / d + 2.0;
    else hue = (c.r - c.g) / d + 4.0;
    precise float sixth = hue / 6.0;
    return vec3(sixth, saturation, lightness);
}

vec3 bloom_gm_hue_weights(float hue) {
    precise float six = hue * 6.0;
    precise float r = abs(six - 3.0) - 1.0;
    precise float g = 2.0 - abs(six - 2.0);
    precise float b = 2.0 - abs(six - 4.0);
    return vec3(bloom_gm_unit(r), bloom_gm_unit(g), bloom_gm_unit(b));
}

vec3 bloom_gm_hsv_to_rgb(vec3 hsv) {
    vec3 n = bloom_gm_hue_weights(hsv.x);
    vec3 o;
    for (int i = 0; i < 3; ++i) {
        precise float scaled = (n[i] - 1.0) * hsv.y;
        precise float r = (scaled + 1.0) * hsv.z;
        o[i] = r;
    }
    return o;
}

vec3 bloom_gm_hsl_to_rgb(vec3 hsl) {
    vec3 n = bloom_gm_hue_weights(hsl.x);
    precise float twice = 2.0 * hsl.z;
    precise float chroma = (1.0 - abs(twice - 1.0)) * hsl.y;
    vec3 o;
    for (int i = 0; i < 3; ++i) {
        precise float scaled = (n[i] - 0.5) * chroma;
        precise float r = scaled + hsl.z;
        o[i] = r;
    }
    return o;
}

float bloom_gm_wrap(float h) {
    precise float lower = h - 1.0;
    return h < 1.0 ? h : lower;
}

float bloom_gm_hue(int direction, float mfac, float fac, float h1, float h2) {
    h1 = bloom_gm_wrap(h1);
    h2 = bloom_gm_wrap(h2);
    precise float gap = h2 - h1;
    int mode = 0;
    if (direction == 0) {
        if (h1 < h2 && gap > 0.5) mode = 1;
        else if (h1 > h2 && gap < -0.5) mode = 2;
    } else if (direction == 1) {
        if (h1 == h2) mode = 1;
        else if (h1 < h2 && gap < 0.5) mode = 1;
        else if (h1 > h2 && gap > -0.5) mode = 2;
    } else if (direction == 3) {
        mode = h1 > h2 ? 2 : 0;
    } else {
        mode = h1 < h2 ? 1 : 0;
    }
    precise float h1Up = h1 + 1.0;
    precise float h2Up = h2 + 1.0;
    if (mode == 1) return bloom_gm_wrap(bloom_gm_weighted(mfac, h1Up, fac, h2));
    if (mode == 2) return bloom_gm_wrap(bloom_gm_weighted(mfac, h1, fac, h2Up));
    return bloom_gm_weighted(mfac, h1, fac, h2);
}

vec4 bloom_gm_spline(int interpolation, float f) {
    precise float f2 = f * f;
    precise float f3 = f2 * f;
    if (interpolation == 3) {
        precise float a0 = -0.709999979 * f3;
        precise float b0 = 1.41999996 * f2;
        precise float c0 = 0.709999979 * f;
        precise float a1 = 1.28999996 * f3;
        precise float b1 = -2.28999996 * f2;
        precise float a2 = -1.28999996 * f3;
        precise float b2 = 1.58000004 * f2;
        precise float c2 = 0.709999979 * f;
        precise float a3 = 0.709999979 * f3;
        precise float b3 = 0.709999979 * f2;
        precise float w0 = (a0 + b0) - c0;
        precise float w1 = (a1 + b1) + 1.0;
        precise float w2 = (a2 + b2) + c2;
        precise float w3 = a3 - b3;
        return vec4(w0, w1, w2, w3);
    }
    precise float a0 = -0.166666657 * f3;
    precise float b0 = 0.5 * f2;
    precise float c0 = 0.5 * f;
    precise float a1 = 0.5 * f3;
    precise float a2 = -0.5 * f3;
    precise float b2 = 0.5 * f2;
    precise float c2 = 0.5 * f;
    precise float a3 = 0.166666657 * f3;
    precise float w0 = ((a0 + b0) - c0) + 0.166666657;
    precise float w1 = (a1 - f2) + 0.666666687;
    precise float w2 = ((a2 + b2) + c2) + 0.166666657;
    return vec4(w0, w1, w2, a3);
}

// The straight RGBA at t of n sorted stops; interpolation 0 Linear, 1 Constant, 2 Ease,
// 3 Cardinal, 4 B-Spline, 5 Exponential; colour mode 0 RGB, 1 HSV, 2 HSL; hue 0 Near, 1 Far,
// 2 Clockwise, 3 Counter-clockwise.
vec4 bloom_gradient_sample(int n, float t, float leadIn, float leadOut, int interpolation,
                           int colorMode, int hue) {
    BloomGradientStop first = bloom_gradient_stop(0);
    if (n <= 1) return first.color;
    BloomGradientStop last = bloom_gradient_stop(n - 1);
    bool rgb = colorMode == 0;
    bool spline = rgb && (interpolation == 3 || interpolation == 4);
    if (!spline) {
        if (!(t > first.position)) return first.color;
        if (t >= last.position) return last.color;
        int i = 0;
        for (int j = 0; j < 64; ++j) {
            if (i + 2 >= n || !(bloom_gradient_stop(i + 1).position <= t)) break;
            ++i;
        }
        BloomGradientStop from = bloom_gradient_stop(i);
        BloomGradientStop to = bloom_gradient_stop(i + 1);
        if (rgb) {
            if (interpolation == 1) return from.color;
            precise float local = (t - from.position) * from.inverseSpan;
            float u = bloom_gm_shape(interpolation, local);
            precise float remaining = 1.0 - u;
            vec4 o;
            for (int c = 0; c < 4; ++c)
                o[c] = bloom_gm_weighted(remaining, from.color[c], u, to.color[c]);
            o.a = bloom_gm_unit(o.a);
            return o;
        }
        precise float fac = (to.position - t) * from.inverseSpan;
        precise float mfac = 1.0 - fac;
        bool hsv = colorMode == 1;
        vec3 later = hsv ? bloom_gm_rgb_to_hsv(to.color.rgb) : bloom_gm_rgb_to_hsl(to.color.rgb);
        vec3 earlier =
            hsv ? bloom_gm_rgb_to_hsv(from.color.rgb) : bloom_gm_rgb_to_hsl(from.color.rgb);
        float h = bloom_gm_hue(hue, mfac, fac, later.x, earlier.x);
        float s = bloom_gm_weighted(mfac, later.y, fac, earlier.y);
        float v = bloom_gm_weighted(mfac, later.z, fac, earlier.z);
        float a = bloom_gm_unit(bloom_gm_weighted(mfac, to.color.a, fac, from.color.a));
        vec3 blended = hsv ? bloom_gm_hsv_to_rgb(vec3(h, s, v)) : bloom_gm_hsl_to_rgb(vec3(h, s, v));
        return vec4(blended, a);
    }
    int after = 0;
    for (int j = 0; j < 64; ++j) {
        if (after >= n || bloom_gradient_stop(after).position > t) break;
        ++after;
    }
    BloomGradientStop earlier;
    BloomGradientStop later;
    precise float fac;
    if (after == n) {
        earlier = last;
        later = last;
        later.position = 1.0;
        fac = leadOut == 0.0 ? 1.0 : (1.0 - t) * leadOut;
    } else if (after == 0) {
        later = first;
        earlier = first;
        earlier.position = 0.0;
        fac = (first.position - t) * leadIn;
    } else {
        earlier = bloom_gradient_stop(after - 1);
        later = bloom_gradient_stop(after);
        fac = (later.position - t) * earlier.inverseSpan;
    }
    BloomGradientStop next = after + 1 >= n ? later : bloom_gradient_stop(after + 1);
    BloomGradientStop previous = after < 2 ? earlier : bloom_gradient_stop(after - 2);
    vec4 w = bloom_gm_spline(interpolation, bloom_gm_unit(fac));
    vec4 o;
    for (int c = 0; c < 4; ++c) {
        precise float a = w.w * previous.color[c];
        precise float b = w.z * earlier.color[c];
        precise float d = w.y * later.color[c];
        precise float e = w.x * next.color[c];
        precise float r = ((a + b) + d) + e;
        o[c] = r;
    }
    o.a = bloom_gm_unit(o.a);
    return o;
}
)glsl";

} // namespace bloom::render
