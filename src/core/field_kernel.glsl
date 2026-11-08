// #525 Fields: the one field kernel. This exact text is compiled twice: as C++ by
// src/core/field_program.cpp (the CPU reference) and as GLSL 4.60 by the GPU program the runtime
// builds around it (bloom/core/field_program.hpp, fieldKernelGlsl()). Each host defines the dialect
// first:
//   BFK_FN          a function's qualifiers ("static inline" in C++, nothing in GLSL)
//   BFK_P           GLSL `precise` (nothing in C++, built without contraction or fast math)
//   BFK_OUT(T)      an out parameter; BFK_INOUT(T) an inout one
//   BFK_CTX         the context parameter every data or image read takes; BFK_CTXA passes it on
//   vec2/vec3/vec4  the vector types, built only from their components
//   bfk_data(BFK_CTXA, i)                 the i-th vec4 of the prepared table
//   bfk_texel(BFK_CTXA, slot, x, y)       a stored premultiplied texel of image slot `slot`
//   bfk_div(a, b)                         correctly rounded binary32 division
// The dialect is a common subset: binary32 arithmetic, fma, floor, sqrt, abs, min, max, ldexp,
// frexp, integer and bit operations. Every intermediate is stored in a precise variable, so no
// compiler can fuse or reorder it, and every transcendental is Bloom's own polynomial; the only
// operation a device may round differently from the CPU is sqrt. docs/architecture/fields.md is the
// specification of every operation here.

const float BFK_HALF_PI = 1.57079637f;

// --- Elementary functions ------------------------------------------------------------------------

BFK_FN BFK_P float bfk_clamp(float v, float lo, float hi) {
    BFK_P float r = min(max(v, lo), hi);
    return r;
}

BFK_FN BFK_P float bfk_sqrt(float v) {
    BFK_P float r = sqrt(max(v, 0.0f));
    return r;
}

// 2^x for x in [-126, 126] (clamped): 2^floor(x) times a degree-9 polynomial of the fraction.
BFK_FN BFK_P float bfk_exp2(float x) {
    BFK_P float c = bfk_clamp(x, -126.0f, 126.0f);
    BFK_P float whole = floor(c);
    BFK_P float f = c - whole;
    BFK_P float p = 1.01780863e-07f;
    p = fma(p, f, 1.32154867e-06f);
    p = fma(p, f, 1.52527336e-05f);
    p = fma(p, f, 0.000154035297f);
    p = fma(p, f, 0.00133335579f);
    p = fma(p, f, 0.00961812865f);
    p = fma(p, f, 0.0555041097f);
    p = fma(p, f, 0.240226507f);
    p = fma(p, f, 0.693147182f);
    p = fma(p, f, 1.0f);
    BFK_P float r = ldexp(p, int(whole));
    return r;
}

BFK_FN BFK_P float bfk_exp(float x) {
    BFK_P float t = x * 1.44269502f;
    BFK_P float r = bfk_exp2(t);
    return r;
}

// log2(x) for x > 0 (-126 otherwise): the exponent plus 2 atanh(s) / ln 2 with s = (m - 1) / (m + 1)
// and the mantissa m in [sqrt(1/2), sqrt(2)).
BFK_FN BFK_P float bfk_log2(float x) {
    if (!(x > 0.0f))
        return -126.0f;
    int e = 0;
    BFK_P float m = frexp(x, e);
    if (m < 0.707106769f) {
        m = m * 2.0f;
        e = e - 1;
    }
    BFK_P float num = m - 1.0f;
    BFK_P float den = m + 1.0f;
    BFK_P float s = bfk_div(num, den);
    BFK_P float s2 = s * s;
    BFK_P float q = 0.0769230798f;
    q = fma(q, s2, 0.0909090936f);
    q = fma(q, s2, 0.111111112f);
    q = fma(q, s2, 0.142857149f);
    q = fma(q, s2, 0.200000003f);
    q = fma(q, s2, 0.333333343f);
    q = fma(q, s2, 1.0f);
    BFK_P float sq = s * q;
    BFK_P float ln = sq * 2.0f;
    BFK_P float r = fma(ln, 1.44269502f, float(e));
    return r;
}

BFK_FN BFK_P float bfk_log(float x) {
    BFK_P float l = bfk_log2(x);
    BFK_P float r = l * 0.693147182f;
    return r;
}

// a^b for a > 0 (0 otherwise).
BFK_FN BFK_P float bfk_pow(float a, float b) {
    if (!(a > 0.0f))
        return 0.0f;
    BFK_P float l = bfk_log2(a);
    BFK_P float t = b * l;
    BFK_P float r = bfk_exp2(t);
    return r;
}

// The sine and cosine of `turns` whole turns: reduced to a quarter turn, then Taylor polynomials
// of degree 11 and 12 on [-pi/4, pi/4].
BFK_FN void bfk_sincos(float turns, BFK_OUT(float) s, BFK_OUT(float) c) {
    BFK_P float r = turns - floor(turns);
    BFK_P float q4 = r * 4.0f;
    BFK_P float qh = q4 + 0.5f;
    BFK_P float qf = floor(qh);
    BFK_P float f = q4 - qf;
    BFK_P float x = f * BFK_HALF_PI;
    BFK_P float x2 = x * x;
    BFK_P float ps = -2.50521079e-08f;
    ps = fma(ps, x2, 2.75573188e-06f);
    ps = fma(ps, x2, -0.000198412701f);
    ps = fma(ps, x2, 0.00833333377f);
    ps = fma(ps, x2, -0.166666672f);
    ps = fma(ps, x2, 1.0f);
    BFK_P float sv = x * ps;
    BFK_P float pc = 2.08767559e-09f;
    pc = fma(pc, x2, -2.755732e-07f);
    pc = fma(pc, x2, 2.48015876e-05f);
    pc = fma(pc, x2, -0.00138888892f);
    pc = fma(pc, x2, 0.0416666679f);
    pc = fma(pc, x2, -0.5f);
    pc = fma(pc, x2, 1.0f);
    int k = int(qf) & 3;
    if (k == 0) {
        s = sv;
        c = pc;
    } else if (k == 1) {
        s = pc;
        c = -sv;
    } else if (k == 2) {
        s = -sv;
        c = -pc;
    } else {
        s = -pc;
        c = sv;
    }
}

// atan2(y, x) in turns, in [-1/2, 1/2]: reduced to [0, tan 15 degrees] and a Taylor series.
BFK_FN BFK_P float bfk_atan2(float y, float x) {
    BFK_P float ax = abs(x);
    BFK_P float ay = abs(y);
    if (ax == 0.0f && ay == 0.0f)
        return 0.0f;
    BFK_P float mn = min(ax, ay);
    BFK_P float mx = max(ax, ay);
    BFK_P float a = bfk_div(mn, mx);
    BFK_P float off = 0.0f;
    if (a > 0.267949194f) {
        BFK_P float num = fma(a, 1.73205078f, -1.0f);
        BFK_P float den = a + 1.73205078f;
        a = bfk_div(num, den);
        off = 0.0833333358f;
    }
    BFK_P float a2 = a * a;
    BFK_P float p = -0.0666666701f;
    p = fma(p, a2, 0.0769230798f);
    p = fma(p, a2, -0.0909090936f);
    p = fma(p, a2, 0.111111112f);
    p = fma(p, a2, -0.142857149f);
    p = fma(p, a2, 0.200000003f);
    p = fma(p, a2, -0.333333343f);
    p = fma(p, a2, 1.0f);
    BFK_P float at = a * p;
    BFK_P float t = fma(at, 0.159154937f, off);
    if (ay > ax)
        t = 0.25f - t;
    if (x < 0.0f)
        t = 0.5f - t;
    if (y < 0.0f)
        t = -t;
    return t;
}

BFK_FN BFK_P float bfk_length(float x, float y) {
    BFK_P float xx = x * x;
    BFK_P float s = fma(y, y, xx);
    BFK_P float r = bfk_sqrt(s);
    return r;
}

BFK_FN BFK_P float bfk_smooth01(float t) {
    BFK_P float c = bfk_clamp(t, 0.0f, 1.0f);
    BFK_P float k = fma(-2.0f, c, 3.0f);
    BFK_P float c2 = c * c;
    BFK_P float r = c2 * k;
    return r;
}

// --- Table access ---------------------------------------------------------------------------------

// A whole number in [0, 2^32) carried as two exact binary32 halves of 16 bits.
BFK_FN uint bfk_word(float hi, float lo) {
    uint h = uint(int(hi));
    uint l = uint(int(lo));
    uint r = h * 65536u + l;
    return r;
}

// --- Hashing (Jarzynski and Olano, "Hash Functions for GPU Rendering", JCGT 9(3) 2020: pcg4d) ----

BFK_FN void bfk_pcg4d(BFK_INOUT(uint) x, BFK_INOUT(uint) y, BFK_INOUT(uint) z, BFK_INOUT(uint) w) {
    x = x * 1664525u + 1013904223u;
    y = y * 1664525u + 1013904223u;
    z = z * 1664525u + 1013904223u;
    w = w * 1664525u + 1013904223u;
    x = x + y * w;
    y = y + z * x;
    z = z + x * y;
    w = w + y * z;
    x = x ^ (x >> 16u);
    y = y ^ (y >> 16u);
    z = z ^ (z >> 16u);
    w = w ^ (w >> 16u);
    x = x + y * w;
    y = y + z * x;
    z = z + x * y;
    w = w + y * z;
}

// The top 24 bits as a binary32 in [0, 1), exactly.
BFK_FN BFK_P float bfk_unit(uint h) {
    BFK_P float r = float(h >> 8u) * 5.96046448e-08f;
    return r;
}

// The first word of pcg4d(x, y, z, seed).
BFK_FN uint bfk_hash(uint x, uint y, uint z, uint seed) {
    uint a = x;
    uint b = y;
    uint c = z;
    uint d = seed;
    bfk_pcg4d(a, b, c, d);
    return a;
}

// Floor division by a positive whole number with no negative operand.
BFK_FN int bfk_floordiv(int a, int b) {
    if (a >= 0)
        return a / b;
    int n = -a;
    int q = (n + b - 1) / b;
    return -q;
}

// --- Dual numbers: (value, d/dx, d/dy) ------------------------------------------------------------

BFK_FN vec3 bfk_dual(float v, float dx, float dy) {
    return vec3(v, dx, dy);
}

// a + u (b - a) where u depends on x (du = du/dx).
BFK_FN vec3 bfk_lerp_x(vec3 a, vec3 b, float u, float du) {
    BFK_P float dv = b.x - a.x;
    BFK_P float v = fma(u, dv, a.x);
    BFK_P float ddx = b.y - a.y;
    BFK_P float t = fma(u, ddx, a.y);
    BFK_P float gx = fma(du, dv, t);
    BFK_P float ddy = b.z - a.z;
    BFK_P float gy = fma(u, ddy, a.z);
    return vec3(v, gx, gy);
}

BFK_FN vec3 bfk_lerp_y(vec3 a, vec3 b, float u, float du) {
    BFK_P float dv = b.x - a.x;
    BFK_P float v = fma(u, dv, a.x);
    BFK_P float ddx = b.y - a.y;
    BFK_P float gx = fma(u, ddx, a.y);
    BFK_P float ddy = b.z - a.z;
    BFK_P float t = fma(u, ddy, a.z);
    BFK_P float gy = fma(du, dv, t);
    return vec3(v, gx, gy);
}

// A blend whose weight does not depend on x or y (the evolution axis).
BFK_FN vec3 bfk_lerp_c(vec3 a, vec3 b, float u) {
    BFK_P float dv = b.x - a.x;
    BFK_P float v = fma(u, dv, a.x);
    BFK_P float ddx = b.y - a.y;
    BFK_P float gx = fma(u, ddx, a.y);
    BFK_P float ddy = b.z - a.z;
    BFK_P float gy = fma(u, ddy, a.z);
    return vec3(v, gx, gy);
}

BFK_FN vec3 bfk_dual_scale(vec3 a, float k) {
    BFK_P float v = a.x * k;
    BFK_P float gx = a.y * k;
    BFK_P float gy = a.z * k;
    return vec3(v, gx, gy);
}

BFK_FN vec3 bfk_dual_mul(vec3 a, vec3 b) {
    BFK_P float v = a.x * b.x;
    BFK_P float t0 = a.y * b.x;
    BFK_P float gx = fma(a.x, b.y, t0);
    BFK_P float t1 = a.z * b.x;
    BFK_P float gy = fma(a.x, b.z, t1);
    return vec3(v, gx, gy);
}

BFK_FN vec3 bfk_dual_abs(vec3 a) {
    if (a.x < 0.0f)
        return vec3(-a.x, -a.y, -a.z);
    return a;
}

// --- Gradient noise ---------------------------------------------------------------------------------

// Perlin, "Improving Noise" (SIGGRAPH 2002): the quintic fade and its derivative.
BFK_FN BFK_P float bfk_fade(float t) {
    BFK_P float a = fma(t, 6.0f, -15.0f);
    BFK_P float b = fma(t, a, 10.0f);
    BFK_P float t2 = t * t;
    BFK_P float t3 = t2 * t;
    BFK_P float r = t3 * b;
    return r;
}

BFK_FN BFK_P float bfk_fade_d(float t) {
    BFK_P float a = t - 2.0f;
    BFK_P float b = fma(t, a, 1.0f);
    BFK_P float t2 = t * t;
    BFK_P float t30 = t2 * 30.0f;
    BFK_P float r = t30 * b;
    return r;
}

// Perlin 2002's gradient of a corner: the twelve cube edges (four repeated) chosen by hash & 15.
// Returns the dual of g . (x, y, z) in x and y.
BFK_FN vec3 bfk_grad3(uint h, float x, float y, float z) {
    int hh = int(h & 15u);
    BFK_P float su = ((hh & 1) == 0) ? 1.0f : -1.0f;
    BFK_P float sv = ((hh & 2) == 0) ? 1.0f : -1.0f;
    BFK_P float gx = 0.0f;
    BFK_P float gy = 0.0f;
    BFK_P float gz = 0.0f;
    if (hh < 8)
        gx = su;
    else
        gy = su;
    if (hh < 4)
        gy = sv;
    else if (hh == 12 || hh == 14)
        gx = sv;
    else
        gz = sv;
    BFK_P float v0 = gx * x;
    BFK_P float v1 = fma(gy, y, v0);
    BFK_P float v = fma(gz, z, v1);
    return vec3(v, gx, gy);
}

// A whole lattice coordinate in the evolution axis, reduced to the period (pa, pb, pc) when pc > 0:
// the corner (i, j, k) becomes (i - q pa, j - q pb, k - q pc) with q = floor(k / pc).
BFK_FN void bfk_periodic(BFK_INOUT(int) i, BFK_INOUT(int) j, BFK_INOUT(int) k, int pa, int pb, int pc) {
    if (pc <= 0)
        return;
    int q = bfk_floordiv(k, pc);
    i = i - q * pa;
    j = j - q * pb;
    k = k - q * pc;
}

// The improved Perlin noise in 3D at (x, y, z) of the lattice whose integer corner offset is
// (ox, oy, oz), with the z axis periodic every `period` cells (0: none). Dual in x and y.
BFK_FN vec3 bfk_perlin3(float x, float y, float z, int ox, int oy, int oz, int period, uint seed) {
    BFK_P float fx0 = floor(x);
    BFK_P float fy0 = floor(y);
    BFK_P float fz0 = floor(z);
    BFK_P float fx = x - fx0;
    BFK_P float fy = y - fy0;
    BFK_P float fz = z - fz0;
    int ix = int(fx0) + ox;
    int iy = int(fy0) + oy;
    int iz = int(fz0) + oz;
    vec3 n[8];
    for (int c = 0; c < 8; ++c) {
        int dx = c & 1;
        int dy = (c >> 1) & 1;
        int dz = (c >> 2) & 1;
        int cx = ix + dx;
        int cy = iy + dy;
        int cz = iz + dz;
        bfk_periodic(cx, cy, cz, 0, 0, period);
        uint h = bfk_hash(uint(cx), uint(cy), uint(cz), seed);
        BFK_P float px = fx - float(dx);
        BFK_P float py = fy - float(dy);
        BFK_P float pz = fz - float(dz);
        n[c] = bfk_grad3(h, px, py, pz);
    }
    BFK_P float u = bfk_fade(fx);
    BFK_P float du = bfk_fade_d(fx);
    BFK_P float v = bfk_fade(fy);
    BFK_P float dv = bfk_fade_d(fy);
    BFK_P float w = bfk_fade(fz);
    vec3 x00 = bfk_lerp_x(n[0], n[1], u, du);
    vec3 x10 = bfk_lerp_x(n[2], n[3], u, du);
    vec3 x01 = bfk_lerp_x(n[4], n[5], u, du);
    vec3 x11 = bfk_lerp_x(n[6], n[7], u, du);
    vec3 y0 = bfk_lerp_y(x00, x10, v, dv);
    vec3 y1 = bfk_lerp_y(x01, x11, v, dv);
    vec3 r = bfk_lerp_c(y0, y1, w);
    return r;
}

// 2D Perlin noise whose unit gradients turn by `alpha` turns (Perlin and Neyret, "Flow Noise",
// 2001): periodic in alpha with period 1. Scaled by sqrt(2) to about [-1, 1].
BFK_FN vec3 bfk_perlin2_flow(float x, float y, int ox, int oy, float alpha, uint seed) {
    BFK_P float fx0 = floor(x);
    BFK_P float fy0 = floor(y);
    BFK_P float fx = x - fx0;
    BFK_P float fy = y - fy0;
    int ix = int(fx0) + ox;
    int iy = int(fy0) + oy;
    vec3 n[4];
    for (int c = 0; c < 4; ++c) {
        int dx = c & 1;
        int dy = (c >> 1) & 1;
        uint h = bfk_hash(uint(ix + dx), uint(iy + dy), 0u, seed);
        BFK_P float a = bfk_unit(h) + alpha;
        BFK_P float gs = 0.0f;
        BFK_P float gc = 0.0f;
        bfk_sincos(a, gs, gc);
        BFK_P float px = fx - float(dx);
        BFK_P float py = fy - float(dy);
        BFK_P float t = gc * px;
        BFK_P float val = fma(gs, py, t);
        n[c] = vec3(val, gc, gs);
    }
    BFK_P float u = bfk_fade(fx);
    BFK_P float du = bfk_fade_d(fx);
    BFK_P float v = bfk_fade(fy);
    BFK_P float dv = bfk_fade_d(fy);
    vec3 x0 = bfk_lerp_x(n[0], n[1], u, du);
    vec3 x1 = bfk_lerp_x(n[2], n[3], u, du);
    vec3 ny = bfk_lerp_y(x0, x1, v, dv);
    vec3 r = bfk_dual_scale(ny, 1.41421354f);
    return r;
}

// One simplex corner's contribution t^4 (g . d) with t = r2 - |d|^2, as a dual in x and y.
BFK_FN vec3 bfk_simplex_corner(float r2, float dx, float dy, float dz, float gx, float gy, float gz) {
    BFK_P float dd0 = dx * dx;
    BFK_P float dd1 = fma(dy, dy, dd0);
    BFK_P float dd = fma(dz, dz, dd1);
    BFK_P float t = r2 - dd;
    if (!(t > 0.0f))
        return vec3(0.0f, 0.0f, 0.0f);
    BFK_P float g0 = gx * dx;
    BFK_P float g1 = fma(gy, dy, g0);
    BFK_P float gd = fma(gz, dz, g1);
    BFK_P float t2 = t * t;
    BFK_P float t4 = t2 * t2;
    BFK_P float v = t4 * gd;
    // d/dx t^4 gd = t^4 gx - 8 t^3 dx gd
    BFK_P float t3 = t2 * t;
    BFK_P float k = t3 * gd;
    BFK_P float k8 = k * -8.0f;
    BFK_P float ax = t4 * gx;
    BFK_P float ddx = fma(k8, dx, ax);
    BFK_P float ay = t4 * gy;
    BFK_P float ddy = fma(k8, dy, ay);
    return vec3(v, ddx, ddy);
}

// Gustavson, "Simplex noise demystified" (2005): 3D simplex noise at (x, y, z) whose skewed integer
// corner offset is (oi, oj, ok), with Perlin 2002's twelve gradients and the final factor 32. With
// period N > 0 the noise repeats along z every 3N: the skewed lattice translation (N, N, 4N).
BFK_FN vec3 bfk_simplex3(float x, float y, float z, int oi, int oj, int ok, int period, uint seed) {
    BFK_P float sxy = x + y;
    BFK_P float sxyz = sxy + z;
    BFK_P float s = sxyz * 0.333333343f;
    BFK_P float xs = x + s;
    BFK_P float ys = y + s;
    BFK_P float zs = z + s;
    BFK_P float fi = floor(xs);
    BFK_P float fj = floor(ys);
    BFK_P float fk = floor(zs);
    BFK_P float ij = fi + fj;
    BFK_P float ijk = ij + fk;
    BFK_P float t = ijk * 0.166666672f;
    BFK_P float ux = fi - t;
    BFK_P float uy = fj - t;
    BFK_P float uz = fk - t;
    BFK_P float x0 = x - ux;
    BFK_P float y0 = y - uy;
    BFK_P float z0 = z - uz;
    int i1 = 0;
    int j1 = 0;
    int k1 = 0;
    int i2 = 0;
    int j2 = 0;
    int k2 = 0;
    if (x0 >= y0) {
        if (y0 >= z0) {
            i1 = 1;
            i2 = 1;
            j2 = 1;
        } else if (x0 >= z0) {
            i1 = 1;
            i2 = 1;
            k2 = 1;
        } else {
            k1 = 1;
            i2 = 1;
            k2 = 1;
        }
    } else {
        if (y0 < z0) {
            k1 = 1;
            j2 = 1;
            k2 = 1;
        } else if (x0 < z0) {
            j1 = 1;
            j2 = 1;
            k2 = 1;
        } else {
            j1 = 1;
            i2 = 1;
            j2 = 1;
        }
    }
    int bi = int(fi) + oi;
    int bj = int(fj) + oj;
    int bk = int(fk) + ok;
    vec3 sum = vec3(0.0f, 0.0f, 0.0f);
    for (int c = 0; c < 4; ++c) {
        int ci = c == 0 ? 0 : (c == 1 ? i1 : (c == 2 ? i2 : 1));
        int cj = c == 0 ? 0 : (c == 1 ? j1 : (c == 2 ? j2 : 1));
        int ck = c == 0 ? 0 : (c == 1 ? k1 : (c == 2 ? k2 : 1));
        BFK_P float g = float(c) * 0.166666672f;
        BFK_P float ex = x0 - float(ci);
        BFK_P float dx = ex + g;
        BFK_P float ey = y0 - float(cj);
        BFK_P float dy = ey + g;
        BFK_P float ez = z0 - float(ck);
        BFK_P float dz = ez + g;
        int li = bi + ci;
        int lj = bj + cj;
        int lk = bk + ck;
        bfk_periodic(li, lj, lk, period, period, 4 * period);
        uint h = bfk_hash(uint(li), uint(lj), uint(lk), seed);
        vec3 grad = bfk_grad3(h, 1.0f, 1.0f, 1.0f);
        // bfk_grad3's value at (1, 1, 1) is gx + gy + gz; recover gz from it.
        BFK_P float gxy = grad.y + grad.z;
        BFK_P float gz = grad.x - gxy;
        vec3 part = bfk_simplex_corner(0.6f, dx, dy, dz, grad.y, grad.z, gz);
        BFK_P float sv = sum.x + part.x;
        BFK_P float sx = sum.y + part.y;
        BFK_P float sy = sum.z + part.z;
        sum = vec3(sv, sx, sy);
    }
    vec3 r = bfk_dual_scale(sum, 32.0f);
    return r;
}

// 2D simplex noise (Gustavson 2005's skew) whose unit gradients turn by `alpha` turns, after
// Gustavson and McEwan's psrdnoise (JCGT 11(1), 2022): periodic in alpha with period 1.
BFK_FN vec3 bfk_simplex2_flow(float x, float y, int oi, int oj, float alpha, uint seed) {
    BFK_P float sxy = x + y;
    BFK_P float s = sxy * 0.366025418f;
    BFK_P float xs = x + s;
    BFK_P float ys = y + s;
    BFK_P float fi = floor(xs);
    BFK_P float fj = floor(ys);
    BFK_P float ij = fi + fj;
    BFK_P float t = ij * 0.211324871f;
    BFK_P float ux = fi - t;
    BFK_P float uy = fj - t;
    BFK_P float x0 = x - ux;
    BFK_P float y0 = y - uy;
    int i1 = x0 > y0 ? 1 : 0;
    int j1 = 1 - i1;
    int bi = int(fi) + oi;
    int bj = int(fj) + oj;
    vec3 sum = vec3(0.0f, 0.0f, 0.0f);
    for (int c = 0; c < 3; ++c) {
        int ci = c == 0 ? 0 : (c == 1 ? i1 : 1);
        int cj = c == 0 ? 0 : (c == 1 ? j1 : 1);
        BFK_P float g = float(c) * 0.211324871f;
        BFK_P float ex = x0 - float(ci);
        BFK_P float dx = ex + g;
        BFK_P float ey = y0 - float(cj);
        BFK_P float dy = ey + g;
        uint h = bfk_hash(uint(bi + ci), uint(bj + cj), 0u, seed);
        BFK_P float a = bfk_unit(h) + alpha;
        BFK_P float gs = 0.0f;
        BFK_P float gc = 0.0f;
        bfk_sincos(a, gs, gc);
        vec3 part = bfk_simplex_corner(0.5f, dx, dy, 0.0f, gc, gs, 0.0f);
        BFK_P float sv = sum.x + part.x;
        BFK_P float sx = sum.y + part.y;
        BFK_P float sy = sum.z + part.z;
        sum = vec3(sv, sx, sy);
    }
    vec3 r = bfk_dual_scale(sum, 99.2043381f);
    return r;
}

// --- Value noise (Block, Linear, Soft Linear and Spline lattice interpolation) -----------------

// A lattice value in [-1, 1): 2h - 1, or with flow cos(2 pi (h + alpha)).
BFK_FN BFK_P float bfk_lattice(uint h, bool flow, float alpha) {
    BFK_P float u = bfk_unit(h);
    if (flow) {
        BFK_P float a = u + alpha;
        BFK_P float s = 0.0f;
        BFK_P float c = 0.0f;
        bfk_sincos(a, s, c);
        return c;
    }
    BFK_P float r = fma(u, 2.0f, -1.0f);
    return r;
}

// The interpolation weights of one axis at t in [0, 1): kind 1 linear, 2 smoothstep (Soft Linear);
// the derivative in dw.
BFK_FN BFK_P float bfk_weight(int kind, float t, BFK_OUT(float) dw) {
    if (kind == 2) {
        BFK_P float a = fma(-2.0f, t, 3.0f);
        BFK_P float t2 = t * t;
        BFK_P float w = t2 * a;
        BFK_P float b = 1.0f - t;
        BFK_P float tb = t * b;
        dw = tb * 6.0f;
        return w;
    }
    dw = 1.0f;
    return t;
}

// Catmull-Rom weight of tap k (0..3, at offsets -1..2) at t, and its derivative.
BFK_FN BFK_P float bfk_spline_weight(int k, float t, BFK_OUT(float) dw) {
    BFK_P float t2 = t * t;
    BFK_P float t3 = t2 * t;
    BFK_P float w = 0.0f;
    BFK_P float d = 0.0f;
    if (k == 0) {
        BFK_P float a = fma(2.0f, t2, -t3);
        w = a - t;
        BFK_P float b = fma(-3.0f, t2, 4.0f * t);
        d = b - 1.0f;
    } else if (k == 1) {
        BFK_P float a = fma(3.0f, t3, -5.0f * t2);
        w = a + 2.0f;
        BFK_P float b = 9.0f * t2;
        d = fma(-10.0f, t, b);
    } else if (k == 2) {
        BFK_P float a = fma(-3.0f, t3, 4.0f * t2);
        w = a + t;
        BFK_P float b = -9.0f * t2;
        BFK_P float c = fma(8.0f, t, b);
        d = c + 1.0f;
    } else {
        w = t3 - t2;
        BFK_P float b = 3.0f * t2;
        d = fma(-2.0f, t, b);
    }
    BFK_P float hw = w * 0.5f;
    dw = d * 0.5f;
    return hw;
}

// One plane (fixed lattice z) of value noise at the cell fraction (fx, fy) of corner (ix, iy).
BFK_FN vec3 bfk_value_plane(float fx, float fy, int ix, int iy, int iz, int kind, bool flow,
                            float alpha, uint seed) {
    if (kind == 0) {
        uint h = bfk_hash(uint(ix), uint(iy), uint(iz), seed);
        BFK_P float v = bfk_lattice(h, flow, alpha);
        return vec3(v, 0.0f, 0.0f);
    }
    if (kind == 3) {
        BFK_P float sv = 0.0f;
        BFK_P float sx = 0.0f;
        BFK_P float sy = 0.0f;
        for (int j = 0; j < 4; ++j) {
            BFK_P float dwy = 0.0f;
            BFK_P float wy = bfk_spline_weight(j, fy, dwy);
            BFK_P float rv = 0.0f;
            BFK_P float rx = 0.0f;
            for (int i = 0; i < 4; ++i) {
                BFK_P float dwx = 0.0f;
                BFK_P float wx = bfk_spline_weight(i, fx, dwx);
                uint h = bfk_hash(uint(ix + i - 1), uint(iy + j - 1), uint(iz), seed);
                BFK_P float l = bfk_lattice(h, flow, alpha);
                rv = fma(wx, l, rv);
                rx = fma(dwx, l, rx);
            }
            sv = fma(wy, rv, sv);
            sx = fma(wy, rx, sx);
            sy = fma(dwy, rv, sy);
        }
        return vec3(sv, sx, sy);
    }
    BFK_P float dwx = 0.0f;
    BFK_P float wx = bfk_weight(kind, fx, dwx);
    BFK_P float dwy = 0.0f;
    BFK_P float wy = bfk_weight(kind, fy, dwy);
    vec3 c[4];
    for (int k = 0; k < 4; ++k) {
        int dx = k & 1;
        int dy = (k >> 1) & 1;
        uint h = bfk_hash(uint(ix + dx), uint(iy + dy), uint(iz), seed);
        BFK_P float l = bfk_lattice(h, flow, alpha);
        c[k] = vec3(l, 0.0f, 0.0f);
    }
    vec3 x0 = bfk_lerp_x(c[0], c[1], wx, dwx);
    vec3 x1 = bfk_lerp_x(c[2], c[3], wx, dwx);
    vec3 r = bfk_lerp_y(x0, x1, wy, dwy);
    return r;
}

// Value noise: kind 0 Block, 1 Linear, 2 Soft Linear, 3 Spline. Slice evolution (flow false) reads z
// through the same kernel (Block: linearly, so evolving Block noise stays continuous in time).
BFK_FN vec3 bfk_value(float x, float y, float z, int ox, int oy, int oz, int period, int kind,
                      bool flow, float alpha, uint seed) {
    BFK_P float fx0 = floor(x);
    BFK_P float fy0 = floor(y);
    BFK_P float fx = x - fx0;
    BFK_P float fy = y - fy0;
    int ix = int(fx0) + ox;
    int iy = int(fy0) + oy;
    if (flow)
        return bfk_value_plane(fx, fy, ix, iy, 0, kind, true, alpha, seed);
    BFK_P float fz0 = floor(z);
    BFK_P float fz = z - fz0;
    int iz = int(fz0) + oz;
    if (kind == 3) {
        vec3 sum = vec3(0.0f, 0.0f, 0.0f);
        for (int k = 0; k < 4; ++k) {
            BFK_P float dwz = 0.0f;
            BFK_P float wz = bfk_spline_weight(k, fz, dwz);
            int a = 0;
            int b = 0;
            int lz = iz + k - 1;
            bfk_periodic(a, b, lz, 0, 0, period);
            vec3 p = bfk_value_plane(fx, fy, ix, iy, lz, 3, false, 0.0f, seed);
            BFK_P float sv = fma(wz, p.x, sum.x);
            BFK_P float sx = fma(wz, p.y, sum.y);
            BFK_P float sy = fma(wz, p.z, sum.z);
            sum = vec3(sv, sx, sy);
        }
        return sum;
    }
    int a0 = 0;
    int b0 = 0;
    int z0 = iz;
    bfk_periodic(a0, b0, z0, 0, 0, period);
    int a1 = 0;
    int b1 = 0;
    int z1 = iz + 1;
    bfk_periodic(a1, b1, z1, 0, 0, period);
    vec3 p0 = bfk_value_plane(fx, fy, ix, iy, z0, kind, false, 0.0f, seed);
    vec3 p1 = bfk_value_plane(fx, fy, ix, iy, z1, kind, false, 0.0f, seed);
    BFK_P float dwz = 0.0f;
    BFK_P float wz = kind == 2 ? bfk_weight(2, fz, dwz) : fz;
    vec3 r = bfk_lerp_c(p0, p1, wz);
    return r;
}

// --- The prepared table ---------------------------------------------------------------------------
// vec4 0      [magic 525, semantics version, instruction count, result register]
// vec4 1      [ax, bx, ay, by]: pixel (i, j) of the target is at field (fma(i + ox, ax, bx),
//             fma(j + oy, ay, by)), (ox, oy) a sub-sample offset
// vec4 2      [target x, target y, target width, target height] in the dispatched image
// vec4 3      [sample count, footprint (field pixels per sample), output mode, result kind]
// vec4 4..11  sixteen sub-sample offsets (ox, oy), two per vec4
// vec4 12..19 four image slots: [kx, cx, ky, cy] (texel u = fma(x, kx, cx), v = fma(y, ky, cy)),
//             [origin x, origin y, width, height] in the dispatched image (width 0: no image)
// vec4 24...  the instructions, two vec4 each: [op, output, in0, in1] [in2, in3, parameters, 0];
//             an absent input is -1
const int BFK_HEADER = 0;
const int BFK_MAPPING = 1;
const int BFK_TARGET = 2;
const int BFK_SAMPLING = 3;
const int BFK_SAMPLES = 4;
const int BFK_SLOTS = 12;
const int BFK_CODE = 24;

// --- Images -----------------------------------------------------------------------------------------

// Wraps texel index i of an axis of n texels by edge mode (0 Clamp, 1 Transparent, 2 Repeat,
// 3 Mirror); -1 when Transparent leaves the image.
BFK_FN int bfk_edge(int i, int n, int edge) {
    if (n <= 0)
        return -1;
    if (edge == 0)
        return min(max(i, 0), n - 1);
    if (edge == 1)
        return (i < 0 || i >= n) ? -1 : i;
    if (edge == 2) {
        int q = bfk_floordiv(i, n);
        return i - q * n;
    }
    int period = 2 * n;
    int q = bfk_floordiv(i, period);
    int m = i - q * period;
    return m < n ? m : period - 1 - m;
}

BFK_FN vec4 bfk_tap(BFK_CTX, int slot, int x, int y, int edge) {
    vec4 place = bfk_data(BFK_CTXA, BFK_SLOTS + 2 * slot + 1);
    int w = int(place.z);
    int h = int(place.w);
    int ix = bfk_edge(x, w, edge);
    int iy = bfk_edge(y, h, edge);
    if (ix < 0 || iy < 0)
        return vec4(0.0f, 0.0f, 0.0f, 0.0f);
    return bfk_texel(BFK_CTXA, slot, int(place.x) + ix, int(place.y) + iy);
}

BFK_FN vec4 bfk_mix4(vec4 a, vec4 b, float t) {
    BFK_P float dx = b.x - a.x;
    BFK_P float x = fma(t, dx, a.x);
    BFK_P float dy = b.y - a.y;
    BFK_P float y = fma(t, dy, a.y);
    BFK_P float dz = b.z - a.z;
    BFK_P float z = fma(t, dz, a.z);
    BFK_P float dw = b.w - a.w;
    BFK_P float w = fma(t, dw, a.w);
    return vec4(x, y, z, w);
}

BFK_FN vec4 bfk_madd4(vec4 acc, vec4 v, float k) {
    BFK_P float x = fma(v.x, k, acc.x);
    BFK_P float y = fma(v.y, k, acc.y);
    BFK_P float z = fma(v.z, k, acc.z);
    BFK_P float w = fma(v.w, k, acc.w);
    return vec4(x, y, z, w);
}

// A filtered sample of slot `slot` at field position (x, y): filter 0 Bilinear, 1 Nearest,
// 2 Bicubic (Catmull-Rom); texel i's centre is at u = i + 1/2. A slot with no image is transparent.
BFK_FN vec4 bfk_sample(BFK_CTX, int slot, float x, float y, int filt, int edge) {
    vec4 map = bfk_data(BFK_CTXA, BFK_SLOTS + 2 * slot);
    vec4 place = bfk_data(BFK_CTXA, BFK_SLOTS + 2 * slot + 1);
    if (!(place.z > 0.0f) || !(place.w > 0.0f))
        return vec4(0.0f, 0.0f, 0.0f, 0.0f);
    BFK_P float u = fma(x, map.x, map.y);
    BFK_P float v = fma(y, map.z, map.w);
    if (filt == 1) {
        BFK_P float fu = floor(u);
        BFK_P float fv = floor(v);
        return bfk_tap(BFK_CTXA, slot, int(fu), int(fv), edge);
    }
    BFK_P float su = u - 0.5f;
    BFK_P float sv = v - 0.5f;
    BFK_P float iu = floor(su);
    BFK_P float iv = floor(sv);
    BFK_P float tu = su - iu;
    BFK_P float tv = sv - iv;
    int x0 = int(iu);
    int y0 = int(iv);
    if (filt == 2) {
        vec4 acc = vec4(0.0f, 0.0f, 0.0f, 0.0f);
        for (int j = 0; j < 4; ++j) {
            BFK_P float dwy = 0.0f;
            BFK_P float wy = bfk_spline_weight(j, tv, dwy);
            vec4 row = vec4(0.0f, 0.0f, 0.0f, 0.0f);
            for (int i = 0; i < 4; ++i) {
                BFK_P float dwx = 0.0f;
                BFK_P float wx = bfk_spline_weight(i, tu, dwx);
                vec4 t = bfk_tap(BFK_CTXA, slot, x0 + i - 1, y0 + j - 1, edge);
                row = bfk_madd4(row, t, wx);
            }
            acc = bfk_madd4(acc, row, wy);
        }
        return acc;
    }
    vec4 a = bfk_tap(BFK_CTXA, slot, x0, y0, edge);
    vec4 b = bfk_tap(BFK_CTXA, slot, x0 + 1, y0, edge);
    vec4 c = bfk_tap(BFK_CTXA, slot, x0, y0 + 1, edge);
    vec4 d = bfk_tap(BFK_CTXA, slot, x0 + 1, y0 + 1, edge);
    vec4 top = bfk_mix4(a, b, tu);
    vec4 bottom = bfk_mix4(c, d, tu);
    vec4 r = bfk_mix4(top, bottom, tv);
    return r;
}

// One stored channel of a premultiplied texel: 0 red, 1 green, 2 blue, 3 alpha, 4 Rec. 709 luma.
BFK_FN BFK_P float bfk_channel(vec4 t, int channel) {
    if (channel == 0)
        return t.x;
    if (channel == 1)
        return t.y;
    if (channel == 2)
        return t.z;
    if (channel == 3)
        return t.w;
    BFK_P float a = t.x * 0.212599993f;
    BFK_P float b = fma(t.y, 0.715200007f, a);
    BFK_P float c = fma(t.z, 0.0722000003f, b);
    return c;
}

// --- Curves (the Fritsch-Carlson monotone cubic of the Curves node's master curve) -----------------
// At vec4 `base`: [knot count n (at most 16), 0, 0, 0], the n knots, then the n + 1 segment origins,
// each packed four to a vec4, then the n + 1 segments [a, b, c, d]. Segment s covers the inputs with
// s knots at or below them: a + dx (b + dx (c + dx d)) with dx = input - origin[s].
BFK_FN BFK_P float bfk_curve(BFK_CTX, int base, float x) {
    vec4 head = bfk_data(BFK_CTXA, base);
    int n = int(head.x);
    if (n <= 0)
        return x;
    int knots = base + 1;
    int origins = knots + (n + 3) / 4;
    int segments = origins + (n + 4) / 4;
    int s = 0;
    for (int k = 0; k < 16; ++k) {
        if (k >= n)
            break;
        vec4 q = bfk_data(BFK_CTXA, knots + k / 4);
        int lane = k - 4 * (k / 4);
        BFK_P float kx = lane == 0 ? q.x : (lane == 1 ? q.y : (lane == 2 ? q.z : q.w));
        if (kx <= x)
            s = k + 1;
    }
    vec4 o = bfk_data(BFK_CTXA, origins + s / 4);
    int lane = s - 4 * (s / 4);
    BFK_P float origin = lane == 0 ? o.x : (lane == 1 ? o.y : (lane == 2 ? o.z : o.w));
    vec4 c = bfk_data(BFK_CTXA, segments + s);
    BFK_P float dx = x - origin;
    BFK_P float p = fma(dx, c.w, c.z);
    p = fma(dx, p, c.y);
    p = fma(dx, p, c.x);
    return p;
}

// --- Noise ----------------------------------------------------------------------------------------------
// [type, fractal, octaves, flow] [ridge offset, ridge gain, normalization, evolution period]
// [contrast, brightness, invert, overflow] [bipolar, 0, 0, 0], then per octave five vec4:
// [m00, m01, m10, m11] [fx, fy, fz, weight] [ox hi, ox lo, oy hi, oy lo] [oz hi, oz lo, seed hi,
// seed lo] [flow turn, cells per field pixel, 0, 0]. The lattice point of field position p is
// (m00 px + m01 py + fx, m10 px + m11 py + fy, fz) plus the whole offset (ox, oy, oz).
// Types: 0 Perlin, 1 Simplex, 2 Linear, 3 Soft Linear, 4 Block, 5 Spline. Fractals: 0 None, 1 fBm,
// 2 Turbulence, 3 Ridged, 4 Billow, 5-7 Dynamic, Dynamic Progressive, Dynamic Twist (fBm sums whose
// per-octave evolution and rotation the host prepares). Overflow: 0 Clip, 1 Soft Clamp, 2 Wrap
// Back, 3 Allow HDR Results.

BFK_FN vec3 bfk_noise_octave(BFK_CTX, int base, int type, bool flow, int period, float px, float py,
                             float footprint, BFK_OUT(float) amplitude) {
    vec4 m = bfk_data(BFK_CTXA, base);
    vec4 f = bfk_data(BFK_CTXA, base + 1);
    vec4 io = bfk_data(BFK_CTXA, base + 2);
    vec4 iz = bfk_data(BFK_CTXA, base + 3);
    vec4 extra = bfk_data(BFK_CTXA, base + 4);
    BFK_P float aaRaw = fma(-extra.y, footprint, 0.5f);
    BFK_P float aaScaled = aaRaw * 4.0f;
    BFK_P float aa = bfk_clamp(aaScaled, 0.0f, 1.0f);
    amplitude = f.w * aa;
    if (!(amplitude > 0.0f))
        return vec3(0.0f, 0.0f, 0.0f);
    BFK_P float tx = fma(m.y, py, f.x);
    BFK_P float qx = fma(m.x, px, tx);
    BFK_P float ty = fma(m.w, py, f.y);
    BFK_P float qy = fma(m.z, px, ty);
    int ox = int(bfk_word(io.x, io.y));
    int oy = int(bfk_word(io.z, io.w));
    int oz = int(bfk_word(iz.x, iz.y));
    uint seed = bfk_word(iz.z, iz.w);
    vec3 n = vec3(0.0f, 0.0f, 0.0f);
    if (type == 0)
        n = flow ? bfk_perlin2_flow(qx, qy, ox, oy, extra.x, seed)
                 : bfk_perlin3(qx, qy, f.z, ox, oy, oz, period, seed);
    else if (type == 1)
        n = flow ? bfk_simplex2_flow(qx, qy, ox, oy, extra.x, seed)
                 : bfk_simplex3(qx, qy, f.z, ox, oy, oz, period, seed);
    else {
        int kind = type == 2 ? 1 : (type == 3 ? 2 : (type == 4 ? 0 : 3));
        n = bfk_value(qx, qy, f.z, ox, oy, oz, period, kind, flow, extra.x, seed);
    }
    // The gradient in field pixels: M^T times the lattice gradient.
    BFK_P float a0 = m.z * n.z;
    BFK_P float gx = fma(m.x, n.y, a0);
    BFK_P float a1 = m.w * n.z;
    BFK_P float gy = fma(m.y, n.y, a1);
    return vec3(n.x, gx, gy);
}

BFK_FN vec4 bfk_op_noise(BFK_CTX, int base, float px, float py) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    vec4 h2 = bfk_data(BFK_CTXA, base + 2);
    vec4 h3 = bfk_data(BFK_CTXA, base + 3);
    vec4 sampling = bfk_data(BFK_CTXA, BFK_SAMPLING);
    int type = int(h0.x);
    int fractal = int(h0.y);
    int octaves = int(h0.z);
    bool flow = h0.w != 0.0f;
    int period = int(h1.w);
    vec3 acc = vec3(0.0f, 0.0f, 0.0f);
    vec3 weight = vec3(1.0f, 0.0f, 0.0f);
    for (int k = 0; k < 10; ++k) {
        if (k >= octaves)
            break;
        BFK_P float amplitude = 0.0f;
        vec3 n = bfk_noise_octave(BFK_CTXA, base + 4 + 5 * k, type, flow, period, px, py,
                                  sampling.y, amplitude);
        if (fractal == 2) {
            n = bfk_dual_abs(n);
        } else if (fractal == 4) {
            vec3 a = bfk_dual_abs(n);
            BFK_P float v = fma(a.x, 2.0f, -1.0f);
            BFK_P float dx = a.y * 2.0f;
            BFK_P float dy = a.z * 2.0f;
            n = vec3(v, dx, dy);
        } else if (fractal == 3) {
            // Musgrave's ridged multifractal (Texturing & Modeling, 3rd ed.): the signal
            // (offset - |n|)^2 weighted by the previous octave's clamped signal times the gain.
            vec3 a = bfk_dual_abs(n);
            vec3 s = vec3(h1.x - a.x, -a.y, -a.z);
            s = bfk_dual_mul(s, s);
            s = bfk_dual_mul(s, weight);
            BFK_P float wv = s.x * h1.y;
            if (wv <= 0.0f)
                weight = vec3(0.0f, 0.0f, 0.0f);
            else if (wv >= 1.0f)
                weight = vec3(1.0f, 0.0f, 0.0f);
            else {
                BFK_P float wx = s.y * h1.y;
                BFK_P float wy = s.z * h1.y;
                weight = vec3(wv, wx, wy);
            }
            n = s;
        }
        if (amplitude > 0.0f) {
            BFK_P float av = fma(n.x, amplitude, acc.x);
            BFK_P float ax = fma(n.y, amplitude, acc.y);
            BFK_P float ay = fma(n.z, amplitude, acc.z);
            acc = vec3(av, ax, ay);
        }
    }
    vec3 u = bfk_dual_scale(acc, h1.z);
    if (fractal != 2 && fractal != 3) {
        BFK_P float v = fma(u.x, 0.5f, 0.5f);
        BFK_P float dx = u.y * 0.5f;
        BFK_P float dy = u.z * 0.5f;
        u = vec3(v, dx, dy);
    }
    if (h2.z != 0.0f) {
        BFK_P float v = 1.0f - u.x;
        u = vec3(v, -u.y, -u.z);
    }
    BFK_P float centred = u.x - 0.5f;
    BFK_P float lifted = 0.5f + h2.y;
    BFK_P float cv = fma(centred, h2.x, lifted);
    BFK_P float cdx = u.y * h2.x;
    BFK_P float cdy = u.z * h2.x;
    u = vec3(cv, cdx, cdy);
    int overflow = int(h2.w);
    if (overflow == 0) {
        if (u.x <= 0.0f)
            u = vec3(0.0f, 0.0f, 0.0f);
        else if (u.x >= 1.0f)
            u = vec3(1.0f, 0.0f, 0.0f);
    } else if (overflow == 1) {
        // A soft knee a quarter wide at each end: identity on [1/4, 3/4], exponential towards 0
        // and 1 beyond it, continuous with its slope.
        if (u.x > 0.75f) {
            BFK_P float d = u.x - 0.75f;
            BFK_P float e = d * -4.0f;
            BFK_P float k = bfk_exp(e);
            BFK_P float v = fma(-0.25f, k, 1.0f);
            BFK_P float dx = u.y * k;
            BFK_P float dy = u.z * k;
            u = vec3(v, dx, dy);
        } else if (u.x < 0.25f) {
            BFK_P float d = u.x - 0.25f;
            BFK_P float e = d * 4.0f;
            BFK_P float k = bfk_exp(e);
            BFK_P float v = k * 0.25f;
            BFK_P float dx = u.y * k;
            BFK_P float dy = u.z * k;
            u = vec3(v, dx, dy);
        }
    } else if (overflow == 2) {
        BFK_P float halfv = u.x * 0.5f;
        BFK_P float fl = floor(halfv);
        BFK_P float m = fma(fl, -2.0f, u.x);
        if (m > 1.0f) {
            BFK_P float v = 2.0f - m;
            u = vec3(v, -u.y, -u.z);
        } else {
            u = vec3(m, u.y, u.z);
        }
    }
    if (h3.x != 0.0f) {
        BFK_P float v = fma(u.x, 2.0f, -1.0f);
        BFK_P float dx = u.y * 2.0f;
        BFK_P float dy = u.z * 2.0f;
        u = vec3(v, dx, dy);
    }
    return vec4(u.x, u.y, u.z, 0.0f);
}

// --- Voronoi (Worley, "A Cellular Texture Basis Function", SIGGRAPH 1996) ---------------------------
// [mode, metric, jitter, smoothness (cells)] [1 / size, size, octaves, selector] [seed hi, seed lo,
// orbit turn, edges]. Modes 0 F1, 1 F2, 2 F2 - F1, 3 Edge, 4 Smooth F1; metrics 0 Euclidean,
// 1 Manhattan, 2 Chebyshev. Each cell's feature point lies in the disc of radius jitter / 2 about its
// centre, at a hashed radius and angle, the angle advanced by the orbit turn: a 5 x 5 search is then
// exact for F1 and F2. Selector 0: [distance, cell id, edge distance, 0] (cells); 1: [feature x,
// feature y, direction x, direction y] (field pixels, unit direction to the feature point).

BFK_FN void bfk_feature(int cx, int cy, int octave, uint seed, float jitter, float orbit,
                        BFK_OUT(float) fx, BFK_OUT(float) fy, BFK_OUT(float) id) {
    uint a = uint(cx);
    uint b = uint(cy);
    uint c = uint(octave);
    uint d = seed;
    bfk_pcg4d(a, b, c, d);
    BFK_P float rho = bfk_sqrt(bfk_unit(a));
    BFK_P float turn = bfk_unit(b) + orbit;
    BFK_P float s = 0.0f;
    BFK_P float co = 0.0f;
    bfk_sincos(turn, s, co);
    BFK_P float r = rho * jitter;
    BFK_P float hr = r * 0.5f;
    fx = fma(hr, co, 0.5f);
    fy = fma(hr, s, 0.5f);
    id = bfk_unit(c);
}

BFK_FN BFK_P float bfk_metric(float dx, float dy, int metric) {
    if (metric == 1) {
        BFK_P float r = abs(dx) + abs(dy);
        return r;
    }
    if (metric == 2)
        return max(abs(dx), abs(dy));
    BFK_P float xx = dx * dx;
    BFK_P float r = fma(dy, dy, xx);
    return r;
}

BFK_FN vec4 bfk_voronoi_octave(float qx, float qy, int octave, uint seed, float jitter, float orbit,
                               int metric, float smoothness, int mode, bool edges, BFK_OUT(vec4) feature) {
    BFK_P float flx = floor(qx);
    BFK_P float fly = floor(qy);
    BFK_P float fx = qx - flx;
    BFK_P float fy = qy - fly;
    int cx = int(flx);
    int cy = int(fly);
    BFK_P float d1 = 1.0e30f;
    BFK_P float d2 = 1.0e30f;
    BFK_P float r1x = 0.0f;
    BFK_P float r1y = 0.0f;
    BFK_P float id1 = 0.0f;
    int b1x = 0;
    int b1y = 0;
    BFK_P float sum = 0.0f;
    BFK_P float invSmooth = smoothness > 0.0f ? bfk_div(1.0f, smoothness) : 0.0f;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            BFK_P float px = 0.0f;
            BFK_P float py = 0.0f;
            BFK_P float id = 0.0f;
            bfk_feature(cx + i, cy + j, octave, seed, jitter, orbit, px, py, id);
            BFK_P float ox = px + float(i);
            BFK_P float rx = ox - fx;
            BFK_P float oy = py + float(j);
            BFK_P float ry = oy - fy;
            BFK_P float d = bfk_metric(rx, ry, metric);
            if (mode == 4) {
                BFK_P float e = metric == 0 ? bfk_sqrt(d) : d;
                BFK_P float a = e * -invSmooth;
                sum = sum + bfk_exp(a);
            }
            if (d < d1) {
                d2 = d1;
                d1 = d;
                r1x = rx;
                r1y = ry;
                id1 = id;
                b1x = i;
                b1y = j;
            } else if (d < d2) {
                d2 = d;
            }
        }
    }
    BFK_P float e1 = metric == 0 ? bfk_sqrt(d1) : d1;
    BFK_P float e2 = metric == 0 ? bfk_sqrt(d2) : d2;
    BFK_P float edge = 0.0f;
    if (edges) {
        // Quilez, "Voronoi edges": the distance to the nearest bisector of the F1 point and any
        // other feature point, searched 5 x 5 about the F1 cell.
        edge = 1.0e30f;
        for (int j = -2; j <= 2; ++j) {
            for (int i = -2; i <= 2; ++i) {
                BFK_P float px = 0.0f;
                BFK_P float py = 0.0f;
                BFK_P float id = 0.0f;
                int gx = b1x + i;
                int gy = b1y + j;
                bfk_feature(cx + gx, cy + gy, octave, seed, jitter, orbit, px, py, id);
                BFK_P float ox = px + float(gx);
                BFK_P float rx = ox - fx;
                BFK_P float oy = py + float(gy);
                BFK_P float ry = oy - fy;
                BFK_P float ddx = rx - r1x;
                BFK_P float ddy = ry - r1y;
                BFK_P float dd0 = ddx * ddx;
                BFK_P float dd = fma(ddy, ddy, dd0);
                if (dd > 1.0e-10f) {
                    BFK_P float len = bfk_sqrt(dd);
                    BFK_P float mx = r1x + rx;
                    BFK_P float my = r1y + ry;
                    BFK_P float dot0 = mx * ddx;
                    BFK_P float dp = fma(my, ddy, dot0);
                    BFK_P float halfv = dp * 0.5f;
                    BFK_P float dist = bfk_div(halfv, len);
                    edge = min(edge, dist);
                }
            }
        }
    }
    BFK_P float value = e1;
    if (mode == 1)
        value = e2;
    else if (mode == 2)
        value = e2 - e1;
    else if (mode == 3)
        value = edge;
    else if (mode == 4) {
        if (smoothness > 0.0f) {
            BFK_P float l = bfk_log(max(sum, 1.0e-30f));
            value = -l * smoothness;
        }
    }
    BFK_P float len1 = bfk_length(r1x, r1y);
    BFK_P float dirx = len1 > 0.0f ? bfk_div(r1x, len1) : 0.0f;
    BFK_P float diry = len1 > 0.0f ? bfk_div(r1y, len1) : 0.0f;
    BFK_P float fpx = qx + r1x;
    BFK_P float fpy = qy + r1y;
    feature = vec4(fpx, fpy, dirx, diry);
    return vec4(value, id1, edge, 0.0f);
}

BFK_FN vec4 bfk_op_voronoi(BFK_CTX, int base, float px, float py) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    vec4 h2 = bfk_data(BFK_CTXA, base + 2);
    uint seed = bfk_word(h2.x, h2.y);
    int mode = int(h0.x);
    int metric = int(h0.y);
    int octaves = int(h1.z);
    BFK_P float qx = px * h1.x;
    BFK_P float qy = py * h1.x;
    vec4 feature = vec4(0.0f, 0.0f, 0.0f, 0.0f);
    vec4 first = bfk_voronoi_octave(qx, qy, 0, seed, h0.z, h2.z, metric, h0.w, mode,
                                    h2.w != 0.0f || mode == 3, feature);
    if (int(h1.w) == 1) {
        BFK_P float fx = feature.x * h1.y;
        BFK_P float fy = feature.y * h1.y;
        return vec4(fx, fy, feature.z, feature.w);
    }
    BFK_P float total = first.x;
    BFK_P float norm = 1.0f;
    BFK_P float amp = 1.0f;
    BFK_P float scale = 1.0f;
    for (int k = 1; k < 4; ++k) {
        if (k >= octaves)
            break;
        amp = amp * 0.5f;
        scale = scale * 2.0f;
        BFK_P float sx = qx * scale;
        BFK_P float sy = qy * scale;
        vec4 unused = vec4(0.0f, 0.0f, 0.0f, 0.0f);
        vec4 o = bfk_voronoi_octave(sx, sy, k, seed, h0.z, h2.z, metric, h0.w, mode, mode == 3,
                                    unused);
        total = fma(o.x, amp, total);
        norm = norm + amp;
    }
    BFK_P float dist = bfk_div(total, norm);
    return vec4(dist, first.y, first.z, 0.0f);
}

// --- Random ------------------------------------------------------------------------------------------
// [1 / cell x, 1 / cell y, seed hi, seed lo] [step hi, step lo, crossfade, smooth] [gaussian,
// selector, 0, 0]. One pcg4d per cell and evolution step; Gaussian pairs by Box-Muller about 1/2
// with standard deviation 1/6. Selector 0 [v, 0, 0, 0], 1 [v, w, 0, 0], 2 [r, g, b, 1].

BFK_FN vec4 bfk_random_step(int cx, int cy, uint stp, uint seed, bool gaussian) {
    uint a = uint(cx);
    uint b = uint(cy);
    uint c = stp;
    uint d = seed;
    bfk_pcg4d(a, b, c, d);
    BFK_P float u0 = bfk_unit(a);
    BFK_P float u1 = bfk_unit(b);
    BFK_P float u2 = bfk_unit(c);
    BFK_P float u3 = bfk_unit(d);
    if (!gaussian)
        return vec4(u0, u1, u2, u3);
    BFK_P float l0 = bfk_log(max(u0, 5.96046448e-08f));
    BFK_P float m0 = l0 * -2.0f;
    BFK_P float r0 = bfk_sqrt(m0);
    BFK_P float s0 = 0.0f;
    BFK_P float c0 = 0.0f;
    bfk_sincos(u1, s0, c0);
    BFK_P float l1 = bfk_log(max(u2, 5.96046448e-08f));
    BFK_P float m1 = l1 * -2.0f;
    BFK_P float r1 = bfk_sqrt(m1);
    BFK_P float s1 = 0.0f;
    BFK_P float c1 = 0.0f;
    bfk_sincos(u3, s1, c1);
    BFK_P float k = 0.166666672f;
    BFK_P float z0 = r0 * c0;
    BFK_P float z1 = r0 * s0;
    BFK_P float z2 = r1 * c1;
    BFK_P float z3 = r1 * s1;
    BFK_P float g0 = fma(z0, k, 0.5f);
    BFK_P float g1 = fma(z1, k, 0.5f);
    BFK_P float g2 = fma(z2, k, 0.5f);
    BFK_P float g3 = fma(z3, k, 0.5f);
    return vec4(g0, g1, g2, g3);
}

BFK_FN vec4 bfk_op_random(BFK_CTX, int base, float px, float py) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    vec4 h2 = bfk_data(BFK_CTXA, base + 2);
    BFK_P float sx = px * h0.x;
    BFK_P float sy = py * h0.y;
    BFK_P float fx = floor(sx);
    BFK_P float fy = floor(sy);
    int cx = int(fx);
    int cy = int(fy);
    uint seed = bfk_word(h0.z, h0.w);
    uint stp = bfk_word(h1.x, h1.y);
    bool gaussian = h2.x != 0.0f;
    vec4 v = bfk_random_step(cx, cy, stp, seed, gaussian);
    if (h1.w != 0.0f && h1.z > 0.0f) {
        vec4 next = bfk_random_step(cx, cy, stp + 1u, seed, gaussian);
        v = bfk_mix4(v, next, h1.z);
    }
    int selector = int(h2.y);
    if (selector == 0)
        return vec4(v.x, 0.0f, 0.0f, 0.0f);
    if (selector == 1)
        return vec4(v.x, v.y, 0.0f, 0.0f);
    return vec4(v.x, v.y, v.z, 1.0f);
}

// --- Pattern (box-filtered after Quilez, "Filterable procedurals") ---------------------------------
// [shape, waveform, width, phase] [cos, sin, centre x, centre y] [1 / size, softness (cells), 0, 0].
// Shapes 0 Stripes, 1 Checker, 2 Rings, 3 Dots, 4 Grid lines, 5 Hex, 6 Wave; waveforms 0 Sine,
// 1 Triangle, 2 Square, 3 Saw. Each periodic profile is the exact box filter of width `softness`
// cells, the difference of its integral at the two ends.

// The integral of one period-1 profile from 0 to u, less its whole periods (returned in `periods`).
BFK_FN BFK_P float bfk_profile_integral(int waveform, float u, float duty, BFK_OUT(float) periods) {
    BFK_P float fl = floor(u);
    BFK_P float f = u - fl;
    periods = fl;
    if (waveform == 2)
        return min(f, duty);
    if (waveform == 3) {
        BFK_P float ff = f * f;
        BFK_P float r = ff * 0.5f;
        return r;
    }
    if (f < 0.5f) {
        BFK_P float r = f * f;
        return r;
    }
    BFK_P float twice = f * 2.0f;
    BFK_P float ff = f * f;
    BFK_P float a = twice - ff;
    BFK_P float r = a - 0.5f;
    return r;
}

BFK_FN BFK_P float bfk_profile_point(int waveform, float u, float duty) {
    BFK_P float fl = floor(u);
    BFK_P float f = u - fl;
    if (waveform == 0) {
        BFK_P float s = 0.0f;
        BFK_P float c = 0.0f;
        bfk_sincos(u, s, c);
        BFK_P float r = fma(s, 0.5f, 0.5f);
        return r;
    }
    if (waveform == 2)
        return f < duty ? 1.0f : 0.0f;
    if (waveform == 3)
        return f;
    BFK_P float t = fma(f, 2.0f, -1.0f);
    BFK_P float r = 1.0f - abs(t);
    return r;
}

BFK_FN BFK_P float bfk_profile(int waveform, float u, float w, float duty) {
    if (!(w > 1.0e-6f))
        return bfk_profile_point(waveform, u, duty);
    if (waveform == 0) {
        BFK_P float s = 0.0f;
        BFK_P float c = 0.0f;
        bfk_sincos(u, s, c);
        BFK_P float hw = w * 0.5f;
        BFK_P float ss = 0.0f;
        BFK_P float cc = 0.0f;
        bfk_sincos(hw, ss, cc);
        BFK_P float pw = w * 3.14159274f;
        BFK_P float sinc = bfk_div(ss, pw);
        BFK_P float a = s * sinc;
        BFK_P float r = fma(a, 0.5f, 0.5f);
        return r;
    }
    BFK_P float area = waveform == 2 ? duty : 0.5f;
    BFK_P float hw = w * 0.5f;
    BFK_P float ua = u - hw;
    BFK_P float ub = u + hw;
    BFK_P float pa = 0.0f;
    BFK_P float pb = 0.0f;
    BFK_P float ia = bfk_profile_integral(waveform, ua, duty, pa);
    BFK_P float ib = bfk_profile_integral(waveform, ub, duty, pb);
    BFK_P float dp = pb - pa;
    BFK_P float di = ib - ia;
    BFK_P float total = fma(dp, area, di);
    BFK_P float r = bfk_div(total, w);
    return r;
}

BFK_FN vec4 bfk_op_pattern(BFK_CTX, int base, float px, float py) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    vec4 h2 = bfk_data(BFK_CTXA, base + 2);
    int shape = int(h0.x);
    int waveform = int(h0.y);
    BFK_P float width = h0.z;
    BFK_P float phase = h0.w;
    BFK_P float dx = px - h1.z;
    BFK_P float dy = py - h1.w;
    BFK_P float a0 = h1.y * dy;
    BFK_P float lx = fma(h1.x, dx, a0);
    BFK_P float a1 = h1.x * dy;
    BFK_P float ly = fma(-h1.y, dx, a1);
    BFK_P float u = lx * h2.x;
    BFK_P float v = ly * h2.x;
    BFK_P float w = h2.y;
    BFK_P float r = 0.0f;
    if (shape == 0) {
        BFK_P float s = u + phase;
        r = bfk_profile(waveform, s, w, width);
    } else if (shape == 1) {
        BFK_P float s = u + phase;
        BFK_P float sx = bfk_profile(2, s, w, 0.5f);
        BFK_P float sy = bfk_profile(2, v, w, 0.5f);
        BFK_P float ax = fma(sx, 2.0f, -1.0f);
        BFK_P float ay = fma(sy, 2.0f, -1.0f);
        BFK_P float prod = ax * ay;
        r = fma(prod, -0.5f, 0.5f);
    } else if (shape == 2) {
        BFK_P float radius = bfk_length(u, v);
        BFK_P float s = radius + phase;
        r = bfk_profile(waveform, s, w, width);
    } else if (shape == 3) {
        BFK_P float s = u + phase;
        BFK_P float cu = s - floor(s);
        BFK_P float cv = v - floor(v);
        BFK_P float ox = cu - 0.5f;
        BFK_P float oy = cv - 0.5f;
        BFK_P float d = bfk_length(ox, oy);
        BFK_P float rad = width * 0.5f;
        BFK_P float e = d - rad;
        if (w > 1.0e-6f) {
            BFK_P float t = bfk_div(e, w);
            BFK_P float c = 0.5f - t;
            r = bfk_clamp(c, 0.0f, 1.0f);
        } else {
            r = e < 0.0f ? 1.0f : 0.0f;
        }
    } else if (shape == 4) {
        BFK_P float hw = width * 0.5f;
        BFK_P float s = u + phase;
        BFK_P float su = s + hw;
        BFK_P float sv = v + hw;
        BFK_P float gx = bfk_profile(2, su, w, width);
        BFK_P float gy = bfk_profile(2, sv, w, width);
        r = max(gx, gy);
    } else if (shape == 5) {
        // A hexagonal lattice of unit spacing: the nearer of two rectangular grids of centres.
        BFK_P float s = u + phase;
        BFK_P float ry = v * 0.577350259f;
        BFK_P float ax = s - floor(s + 0.5f);
        BFK_P float ayr = ry - floor(ry + 0.5f);
        BFK_P float ay = ayr * 1.73205078f;
        BFK_P float bxs = s - 0.5f;
        BFK_P float bys = ry - 0.5f;
        BFK_P float bx = bxs - floor(bxs + 0.5f);
        BFK_P float byr = bys - floor(bys + 0.5f);
        BFK_P float by = byr * 1.73205078f;
        BFK_P float da0 = ax * ax;
        BFK_P float da = fma(ay, ay, da0);
        BFK_P float db0 = bx * bx;
        BFK_P float db = fma(by, by, db0);
        BFK_P float hx = da <= db ? abs(ax) : abs(bx);
        BFK_P float hy = da <= db ? abs(ay) : abs(by);
        BFK_P float k0 = hx * 0.5f;
        BFK_P float k = fma(hy, 0.866025388f, k0);
        BFK_P float hd = max(hx, k);
        BFK_P float line = 0.5f - hd;
        BFK_P float hw = width * 0.5f;
        BFK_P float e = line - hw;
        if (w > 1.0e-6f) {
            BFK_P float t = bfk_div(e, w);
            BFK_P float c = 0.5f - t;
            r = bfk_clamp(c, 0.0f, 1.0f);
        } else {
            r = e < 0.0f ? 1.0f : 0.0f;
        }
    } else {
        BFK_P float s = 0.0f;
        BFK_P float c = 0.0f;
        bfk_sincos(v, s, c);
        BFK_P float amp = width * 0.25f;
        BFK_P float bend = fma(s, amp, u);
        BFK_P float t = bend + phase;
        r = bfk_profile(waveform, t, w, 0.5f);
    }
    return vec4(r, 0.0f, 0.0f, 0.0f);
}

// --- Gradient field (the GradientV1 positions of the gradient sources) ----------------------------
// [mode, repeat, cycle, 0] [start x, start y, end x - start x, end y - start y] [1 / |d|^2,
// 1 / |d|, 0, 0]. Modes 0 Linear, 1 Radial, 2 Angular, 3 Diamond; repeat 0 Clamp, 1 Repeat,
// 2 Mirror.

BFK_FN BFK_P float bfk_cycle(float t, int repeat) {
    if (repeat == 0)
        return bfk_clamp(t, 0.0f, 1.0f);
    if (repeat == 1) {
        BFK_P float r = t - floor(t);
        return r;
    }
    BFK_P float halfv = t * 0.5f;
    BFK_P float fl = floor(halfv);
    BFK_P float m = fma(fl, -2.0f, t);
    BFK_P float r = m > 1.0f ? 2.0f - m : m;
    return r;
}

BFK_FN vec4 bfk_op_gradient(BFK_CTX, int base, float px, float py) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    vec4 h2 = bfk_data(BFK_CTXA, base + 2);
    int mode = int(h0.x);
    BFK_P float vx = px - h1.x;
    BFK_P float vy = py - h1.y;
    BFK_P float d0 = vx * h1.z;
    BFK_P float along = fma(vy, h1.w, d0);
    BFK_P float c0 = vy * h1.z;
    BFK_P float across = fma(-vx, h1.w, c0);
    BFK_P float t = 0.0f;
    if (mode == 0)
        t = along * h2.x;
    else if (mode == 1) {
        BFK_P float l = bfk_length(vx, vy);
        t = l * h2.y;
    } else if (mode == 2) {
        BFK_P float a = bfk_atan2(across, along);
        t = a - floor(a);
    } else {
        BFK_P float s = abs(along) + abs(across);
        t = s * h2.x;
    }
    BFK_P float shifted = t + h0.z;
    BFK_P float r = bfk_cycle(shifted, int(h0.y));
    return vec4(r, 0.0f, 0.0f, 0.0f);
}

// --- Shape distance (Quilez, "2D distance functions"; a mask's exact distance transform) ----------
// [source, primitive, falloff, curve] [centre x, centre y, cos, sin] [half x, half y, corner,
// sector turn] [vertex A x, vertex B x, vertex B y, invert] [slot, field pixels per texel, 0, 0].
// Source 0 primitive, 1 the distance image of a mask in slot `slot` (signed texels in red, +far
// outside it). Primitives 0 Circle, 1 Box, 2 Rounded Box, 3 Segment, 4 Polygon, 5 Star, 6 Ring.
// Curves 0 Linear, 1 Smooth, 2 Exponential, 3 Step. Result [signed distance (field pixels),
// falloff, direction x, direction y].

BFK_FN BFK_P float bfk_box(float x, float y, float bx, float by) {
    BFK_P float qx = abs(x) - bx;
    BFK_P float qy = abs(y) - by;
    BFK_P float ox = max(qx, 0.0f);
    BFK_P float oy = max(qy, 0.0f);
    BFK_P float outside = bfk_length(ox, oy);
    BFK_P float inside = min(max(qx, qy), 0.0f);
    BFK_P float r = outside + inside;
    return r;
}

BFK_FN BFK_P float bfk_primitive(BFK_CTX, int base, float px, float py) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    vec4 h2 = bfk_data(BFK_CTXA, base + 2);
    vec4 h3 = bfk_data(BFK_CTXA, base + 3);
    int primitive = int(h0.y);
    BFK_P float dx = px - h1.x;
    BFK_P float dy = py - h1.y;
    BFK_P float a0 = h1.w * dy;
    BFK_P float x = fma(h1.z, dx, a0);
    BFK_P float a1 = h1.z * dy;
    BFK_P float y = fma(-h1.w, dx, a1);
    if (primitive == 0) {
        BFK_P float l = bfk_length(x, y);
        BFK_P float r = l - h2.x;
        return r;
    }
    if (primitive == 1)
        return bfk_box(x, y, h2.x, h2.y);
    if (primitive == 2) {
        BFK_P float bx = h2.x - h2.z;
        BFK_P float by = h2.y - h2.z;
        BFK_P float b = bfk_box(x, y, bx, by);
        BFK_P float r = b - h2.z;
        return r;
    }
    if (primitive == 3) {
        BFK_P float cx = bfk_clamp(x, -h2.x, h2.x);
        BFK_P float ex = x - cx;
        BFK_P float l = bfk_length(ex, y);
        BFK_P float r = l - h2.z;
        return r;
    }
    if (primitive == 6) {
        BFK_P float l = bfk_length(x, y);
        BFK_P float e = l - h2.x;
        BFK_P float r = abs(e) - h2.z;
        return r;
    }
    // Polygon and Star: the point folded into the half sector [0, sector / 2], then the signed
    // distance to the edge from the outer vertex A = (h3.x, 0) to the inner vertex B = (h3.y, h3.z).
    BFK_P float radius = bfk_length(x, y);
    BFK_P float angle = bfk_atan2(y, x);
    BFK_P float sector = h2.w;
    BFK_P float ratio = bfk_div(angle, sector);
    BFK_P float shifted = ratio + 0.5f;
    BFK_P float whole = floor(shifted);
    BFK_P float reduced = fma(-whole, sector, angle);
    BFK_P float folded = abs(reduced);
    BFK_P float fs = 0.0f;
    BFK_P float fc = 0.0f;
    bfk_sincos(folded, fs, fc);
    BFK_P float qx = radius * fc;
    BFK_P float qy = radius * fs;
    BFK_P float ex = h3.y - h3.x;
    BFK_P float ey = h3.z;
    BFK_P float wx = qx - h3.x;
    BFK_P float wy = qy;
    BFK_P float ee0 = ex * ex;
    BFK_P float ee = fma(ey, ey, ee0);
    BFK_P float we0 = wx * ex;
    BFK_P float we = fma(wy, ey, we0);
    BFK_P float hq = bfk_div(we, ee);
    BFK_P float h = bfk_clamp(hq, 0.0f, 1.0f);
    BFK_P float nx = fma(-h, ex, wx);
    BFK_P float ny = fma(-h, ey, wy);
    BFK_P float d = bfk_length(nx, ny);
    BFK_P float c0 = ey * wx;
    BFK_P float crs = fma(ex, wy, -c0);
    BFK_P float sd = crs > 0.0f ? -d : d;
    BFK_P float r = sd - h2.z;
    return r;
}

BFK_FN BFK_P float bfk_shape_distance(BFK_CTX, int base, float px, float py) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    if (int(h0.x) == 0)
        return bfk_primitive(BFK_CTXA, base, px, py);
    vec4 h4 = bfk_data(BFK_CTXA, base + 4);
    int slot = int(h4.x);
    vec4 place = bfk_data(BFK_CTXA, BFK_SLOTS + 2 * slot + 1);
    if (!(place.z > 0.0f) || !(place.w > 0.0f))
        return 1.0e6f;
    vec4 map = bfk_data(BFK_CTXA, BFK_SLOTS + 2 * slot);
    BFK_P float u = fma(px, map.x, map.y);
    BFK_P float v = fma(py, map.z, map.w);
    BFK_P float su = u - 0.5f;
    BFK_P float sv = v - 0.5f;
    BFK_P float iu = floor(su);
    BFK_P float iv = floor(sv);
    BFK_P float tu = su - iu;
    BFK_P float tv = sv - iv;
    int x0 = int(iu);
    int y0 = int(iv);
    int w = int(place.z);
    int h = int(place.w);
    if (x0 < 0 || y0 < 0 || x0 + 1 >= w || y0 + 1 >= h) {
        // Outside the distance image, which reaches past the mask by its full falloff: far.
        BFK_P float c = bfk_clamp(su, 0.0f, float(w - 1));
        BFK_P float d = bfk_clamp(sv, 0.0f, float(h - 1));
        vec4 t = bfk_tap(BFK_CTXA, slot, int(c), int(d), 0);
        BFK_P float far = t.x * h4.y;
        return max(far, 0.0f);
    }
    vec4 a = bfk_tap(BFK_CTXA, slot, x0, y0, 0);
    vec4 b = bfk_tap(BFK_CTXA, slot, x0 + 1, y0, 0);
    vec4 c = bfk_tap(BFK_CTXA, slot, x0, y0 + 1, 0);
    vec4 d = bfk_tap(BFK_CTXA, slot, x0 + 1, y0 + 1, 0);
    vec4 top = bfk_mix4(a, b, tu);
    vec4 bottom = bfk_mix4(c, d, tu);
    vec4 m = bfk_mix4(top, bottom, tv);
    BFK_P float r = m.x * h4.y;
    return r;
}

BFK_FN vec4 bfk_op_shape(BFK_CTX, int base, float px, float py) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h3 = bfk_data(BFK_CTXA, base + 3);
    BFK_P float sd = bfk_shape_distance(BFK_CTXA, base, px, py);
    BFK_P float xp = px + 0.5f;
    BFK_P float xm = px - 0.5f;
    BFK_P float yp = py + 0.5f;
    BFK_P float ym = py - 0.5f;
    BFK_P float sxp = bfk_shape_distance(BFK_CTXA, base, xp, py);
    BFK_P float sxm = bfk_shape_distance(BFK_CTXA, base, xm, py);
    BFK_P float syp = bfk_shape_distance(BFK_CTXA, base, px, yp);
    BFK_P float sym = bfk_shape_distance(BFK_CTXA, base, px, ym);
    BFK_P float gx = sxp - sxm;
    BFK_P float gy = syp - sym;
    BFK_P float gl = bfk_length(gx, gy);
    BFK_P float dirx = gl > 0.0f ? bfk_div(gx, gl) : 0.0f;
    BFK_P float diry = gl > 0.0f ? bfk_div(gy, gl) : 0.0f;
    BFK_P float outside = max(sd, 0.0f);
    BFK_P float ratio = h0.z > 0.0f ? bfk_div(outside, h0.z) : (outside > 0.0f ? 1.0f : 0.0f);
    BFK_P float linear = 1.0f - ratio;
    BFK_P float f = bfk_clamp(linear, 0.0f, 1.0f);
    int curve = int(h0.w);
    if (curve == 1)
        f = bfk_smooth01(f);
    else if (curve == 2) {
        BFK_P float e0 = f - 1.0f;
        BFK_P float e = e0 * 4.0f;
        BFK_P float k = bfk_exp(e);
        BFK_P float num = k - 0.0183156393f;
        f = num * 1.01865733f;
        f = bfk_clamp(f, 0.0f, 1.0f);
    } else if (curve == 3)
        f = f > 0.0f ? 1.0f : 0.0f;
    if (h3.w != 0.0f)
        f = 1.0f - f;
    return vec4(sd, f, dirx, diry);
}

// --- Image field ------------------------------------------------------------------------------------
// [slot, filter, edge, selector] [channel, pair first, pair second, 0]. Selector 0 [stored channel,
// 0, 0, 0], 1 [first, second, 0, 0], 2 the straight colour [r, g, b, a], 3 the filtered texel as
// stored, premultiplied [r a, g a, b a, a] (a result of kind 3 keeps it whole, even where alpha is 0).

BFK_FN vec4 bfk_op_image(BFK_CTX, int base, float px, float py) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    vec4 t = bfk_sample(BFK_CTXA, int(h0.x), px, py, int(h0.y), int(h0.z));
    int selector = int(h0.w);
    if (selector == 0)
        return vec4(bfk_channel(t, int(h1.x)), 0.0f, 0.0f, 0.0f);
    if (selector == 1)
        return vec4(bfk_channel(t, int(h1.y)), bfk_channel(t, int(h1.z)), 0.0f, 0.0f);
    if (selector == 3)
        return t;
    BFK_P float a = bfk_clamp(t.w, 0.0f, 1.0f);
    if (!(a > 0.0f))
        return vec4(0.0f, 0.0f, 0.0f, 0.0f);
    BFK_P float r = bfk_div(t.x, a);
    BFK_P float g = bfk_div(t.y, a);
    BFK_P float b = bfk_div(t.z, a);
    return vec4(r, g, b, a);
}

// --- Force ----------------------------------------------------------------------------------------------
// [mode, falloff, strength, 1 / radius] [centre x, centre y, spin, curve] [cos, sin, 0, 0]. Modes
// 0 Vortex, 1 Radial, 2 Uniform; falloffs 0 Linear, 1 Smooth, 2 Inverse Square, 3 Curve (at vec4
// base + `curve`); spin +1 counter-clockwise. Result [vx, vy, magnitude, 0].

BFK_FN vec4 bfk_op_force(BFK_CTX, int base, float px, float py) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    vec4 h2 = bfk_data(BFK_CTXA, base + 2);
    int mode = int(h0.x);
    if (mode == 2) {
        BFK_P float vx = h0.z * h2.x;
        BFK_P float vy = h0.z * h2.y;
        return vec4(vx, vy, abs(h0.z), 0.0f);
    }
    BFK_P float dx = px - h1.x;
    BFK_P float dy = py - h1.y;
    BFK_P float r = bfk_length(dx, dy);
    if (!(r > 0.0f))
        return vec4(0.0f, 0.0f, 0.0f, 0.0f);
    BFK_P float x = r * h0.w;
    int falloff = int(h0.y);
    BFK_P float k = 0.0f;
    if (falloff == 0)
        k = max(1.0f - x, 0.0f);
    else if (falloff == 1) {
        BFK_P float t = 1.0f - x;
        k = bfk_smooth01(t);
    } else if (falloff == 2) {
        BFK_P float d = fma(x, x, 1.0f);
        k = bfk_div(1.0f, d);
    } else {
        BFK_P float c = bfk_curve(BFK_CTXA, base + int(h1.w), x);
        k = c;
    }
    BFK_P float s = h0.z * k;
    BFK_P float inv = bfk_div(s, r);
    BFK_P float ux = dx * inv;
    BFK_P float uy = dy * inv;
    if (mode == 0) {
        BFK_P float vx = -uy * h1.z;
        BFK_P float vy = ux * h1.z;
        return vec4(vx, vy, abs(s), 0.0f);
    }
    return vec4(ux, uy, abs(s), 0.0f);
}

// --- Field math -------------------------------------------------------------------------------------
// [operation, k, cos, sin]. Componentwise: 0 Add, 1 Subtract, 2 Multiply, 3 Divide (0 below 1e-20),
// 4 Min, 5 Max, 6 Power, 7 Abs, 8 Difference, 9 Lerp (t = in2.x), 10 Screen, 11 Smooth Min,
// 12 Smooth Max (Quilez's polynomial smooth minimum of width k). Of the xy vector: 13 Dot, 14
// Length, 15 Normalize, 16 Scale (by b.x), 17 Rotate (by the angle whose cos and sin are given).

BFK_FN BFK_P float bfk_math1(int op, float a, float b, float t, float k) {
    if (op == 0) {
        BFK_P float r = a + b;
        return r;
    }
    if (op == 1) {
        BFK_P float r = a - b;
        return r;
    }
    if (op == 2) {
        BFK_P float r = a * b;
        return r;
    }
    if (op == 3)
        return abs(b) < 1.0e-20f ? 0.0f : bfk_div(a, b);
    if (op == 4)
        return min(a, b);
    if (op == 5)
        return max(a, b);
    if (op == 6)
        return bfk_pow(a, b);
    if (op == 7)
        return abs(a);
    if (op == 8) {
        BFK_P float d = a - b;
        return abs(d);
    }
    if (op == 9) {
        BFK_P float d = b - a;
        BFK_P float r = fma(t, d, a);
        return r;
    }
    if (op == 10) {
        BFK_P float ia = 1.0f - a;
        BFK_P float ib = 1.0f - b;
        BFK_P float p = ia * ib;
        BFK_P float r = 1.0f - p;
        return r;
    }
    if (op == 11 || op == 12) {
        BFK_P float sa = op == 12 ? -a : a;
        BFK_P float sb = op == 12 ? -b : b;
        BFK_P float m = min(sa, sb);
        if (k > 0.0f) {
            BFK_P float d = sa - sb;
            BFK_P float gap = k - abs(d);
            BFK_P float hk = max(gap, 0.0f);
            BFK_P float h = bfk_div(hk, k);
            BFK_P float hh = h * h;
            BFK_P float q = hh * k;
            m = fma(q, -0.25f, m);
        }
        BFK_P float r = op == 12 ? -m : m;
        return r;
    }
    return a;
}

BFK_FN vec4 bfk_op_math(BFK_CTX, int base, vec4 a, vec4 b, vec4 t) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    int op = int(h0.x);
    if (op <= 12) {
        BFK_P float x = bfk_math1(op, a.x, b.x, t.x, h0.y);
        BFK_P float y = bfk_math1(op, a.y, b.y, t.x, h0.y);
        BFK_P float z = bfk_math1(op, a.z, b.z, t.x, h0.y);
        BFK_P float w = bfk_math1(op, a.w, b.w, t.x, h0.y);
        return vec4(x, y, z, w);
    }
    if (op == 13) {
        BFK_P float p = a.x * b.x;
        BFK_P float d = fma(a.y, b.y, p);
        return vec4(d, d, 0.0f, 0.0f);
    }
    BFK_P float l = bfk_length(a.x, a.y);
    if (op == 14)
        return vec4(l, l, 0.0f, 0.0f);
    if (op == 15) {
        if (!(l > 0.0f))
            return vec4(0.0f, 0.0f, 0.0f, 0.0f);
        BFK_P float x = bfk_div(a.x, l);
        BFK_P float y = bfk_div(a.y, l);
        return vec4(x, y, 0.0f, 0.0f);
    }
    if (op == 16) {
        BFK_P float x = a.x * b.x;
        BFK_P float y = a.y * b.x;
        return vec4(x, y, 0.0f, 0.0f);
    }
    BFK_P float r0 = h0.w * a.y;
    BFK_P float x = fma(h0.z, a.x, -r0);
    BFK_P float r1 = h0.z * a.y;
    BFK_P float y = fma(h0.w, a.x, r1);
    return vec4(x, y, 0.0f, 0.0f);
}

// --- Remap ------------------------------------------------------------------------------------------
// [in low, 1 / (in high - in low), out low, out high - out low] [clamp, invert, steps, vector]
// [bias, gain, curve (vec4 offset from the block, -1 none), 0]. Bias and gain are Schlick's rational functions
// (Graphics Gems IV, 1994) on t in [0, 1]; outside it t passes unchanged, so HDR values keep their
// meaning. A vector is remapped by its length and keeps its direction.

BFK_FN BFK_P float bfk_bias(float t, float b) {
    if (b == 0.5f || t <= 0.0f || t >= 1.0f)
        return t;
    BFK_P float ib = bfk_div(1.0f, b);
    BFK_P float k = ib - 2.0f;
    BFK_P float u = 1.0f - t;
    BFK_P float d = fma(k, u, 1.0f);
    BFK_P float r = bfk_div(t, d);
    return r;
}

BFK_FN BFK_P float bfk_gain(float t, float g) {
    if (g == 0.5f || t <= 0.0f || t >= 1.0f)
        return t;
    if (t < 0.5f) {
        BFK_P float t2 = t * 2.0f;
        BFK_P float b = bfk_bias(t2, g);
        BFK_P float r = b * 0.5f;
        return r;
    }
    BFK_P float t2 = fma(t, -2.0f, 2.0f);
    BFK_P float b = bfk_bias(t2, g);
    BFK_P float r = fma(b, -0.5f, 1.0f);
    return r;
}

BFK_FN BFK_P float bfk_remap1(BFK_CTX, int base, float v) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    vec4 h2 = bfk_data(BFK_CTXA, base + 2);
    BFK_P float d = v - h0.x;
    BFK_P float t = d * h0.y;
    if (h1.y != 0.0f)
        t = 1.0f - t;
    if (h2.z >= 0.0f)
        t = bfk_curve(BFK_CTXA, base + int(h2.z), t);
    t = bfk_bias(t, h2.x);
    t = bfk_gain(t, h2.y);
    if (h1.z >= 2.0f) {
        BFK_P float s = t * h1.z;
        BFK_P float fl = floor(s);
        BFK_P float top = h1.z - 1.0f;
        BFK_P float c = bfk_clamp(fl, 0.0f, top);
        t = bfk_div(c, top);
    }
    BFK_P float r = fma(t, h0.w, h0.z);
    if (h1.x != 0.0f) {
        BFK_P float hi = h0.z + h0.w;
        BFK_P float lo = min(h0.z, hi);
        BFK_P float up = max(h0.z, hi);
        r = bfk_clamp(r, lo, up);
    }
    return r;
}

BFK_FN vec4 bfk_op_remap(BFK_CTX, int base, vec4 a) {
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    if (h1.w == 0.0f) {
        BFK_P float r = bfk_remap1(BFK_CTXA, base, a.x);
        return vec4(r, r, 0.0f, 0.0f);
    }
    BFK_P float l = bfk_length(a.x, a.y);
    BFK_P float m = bfk_remap1(BFK_CTXA, base, l);
    if (!(l > 0.0f))
        return vec4(0.0f, 0.0f, 0.0f, 0.0f);
    BFK_P float k = bfk_div(m, l);
    BFK_P float x = a.x * k;
    BFK_P float y = a.y * k;
    return vec4(x, y, 0.0f, 0.0f);
}

// --- Transform / warp of a coordinate ---------------------------------------------------------------
// [a00, a01, a10, a11] [b x, b y, mode, segments] [tile x, tile y, 1 / tile x, 1 / tile y]
// [warp amount, 0, 0, 0]: the source coordinate A p + b, then the coordinate mode (0 None, 1 Polar,
// 2 Tile, 3 Mirror, 4 Kaleidoscope), then plus amount times the warp field at p (Quilez, "Domain
// warping").

BFK_FN BFK_P float bfk_mirror_axis(float x, float tile, float inv) {
    BFK_P float u0 = x * inv;
    BFK_P float u = u0 + 0.5f;
    BFK_P float halfv = u * 0.5f;
    BFK_P float fl = floor(halfv);
    BFK_P float m = fma(fl, -2.0f, u);
    BFK_P float g = m > 1.0f ? 2.0f - m : m;
    BFK_P float c = g - 0.5f;
    BFK_P float r = c * tile;
    return r;
}

BFK_FN vec4 bfk_op_transform(BFK_CTX, int base, vec4 p, vec4 warp, bool warped) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    vec4 h2 = bfk_data(BFK_CTXA, base + 2);
    vec4 h3 = bfk_data(BFK_CTXA, base + 3);
    BFK_P float t0 = fma(h0.y, p.y, h1.x);
    BFK_P float x = fma(h0.x, p.x, t0);
    BFK_P float t1 = fma(h0.w, p.y, h1.y);
    BFK_P float y = fma(h0.z, p.x, t1);
    int mode = int(h1.z);
    if (mode == 1) {
        BFK_P float a = bfk_atan2(y, x);
        BFK_P float l = bfk_length(x, y);
        BFK_P float ax = a * h2.x;
        x = ax;
        y = l;
    } else if (mode == 2) {
        BFK_P float ux = fma(x, h2.z, 0.5f);
        BFK_P float uy = fma(y, h2.w, 0.5f);
        BFK_P float fx = ux - floor(ux);
        BFK_P float fy = uy - floor(uy);
        BFK_P float cx = fx - 0.5f;
        BFK_P float cy = fy - 0.5f;
        x = cx * h2.x;
        y = cy * h2.y;
    } else if (mode == 3) {
        BFK_P float mx = bfk_mirror_axis(x, h2.x, h2.z);
        BFK_P float my = bfk_mirror_axis(y, h2.y, h2.w);
        x = mx;
        y = my;
    } else if (mode == 4) {
        BFK_P float a = bfk_atan2(y, x);
        BFK_P float l = bfk_length(x, y);
        BFK_P float seg = bfk_div(1.0f, h1.w);
        BFK_P float ratio = a * h1.w;
        BFK_P float whole = floor(ratio);
        BFK_P float rem = fma(-whole, seg, a);
        BFK_P float halfSeg = seg * 0.5f;
        BFK_P float folded = rem > halfSeg ? seg - rem : rem;
        BFK_P float s = 0.0f;
        BFK_P float c = 0.0f;
        bfk_sincos(folded, s, c);
        BFK_P float kx = l * c;
        BFK_P float ky = l * s;
        x = kx;
        y = ky;
    }
    if (warped) {
        x = fma(warp.x, h3.x, x);
        y = fma(warp.y, h3.x, y);
    }
    return vec4(x, y, 0.0f, 0.0f);
}

// --- Lens warp ------------------------------------------------------------------------------------
// [k1, k2, p1, p2] [centre x, centre y, scale, mode]: the Brown-Conrady lens model (Brown, "Decentering
// Distortion of Lenses", Photogrammetric Engineering 32(3), 1966; Conrady, "Decentred Lens-Systems",
// Monthly Notices of the Royal Astronomical Society 79, 1919) in normalised coordinates
// (x, y) = ((p - centre) / scale), with r2 = x x + y y:
//   D(x, y) = (x s + 2 p1 x y + p2 (r2 + 2 x x), y s + p1 (r2 + 2 y y) + 2 p2 x y), s = 1 + k1 r2 + k2 r2 r2,
// the distorted position of an undistorted one. The result is the position to read the source at,
// back in field positions: mode 0 reads D(p) (an Undistort output pixel reads the distorted image
// where the lens put it), mode 1 reads D^-1(p), found by twelve Newton steps from p over D's
// Jacobian (Redistort). The two are inverses wherever D is monotonic from the centre. The centre is
// in field positions (+y up) and the scale a length in field pixels.

BFK_FN vec4 bfk_lens_model(float k1, float k2, float p1, float p2, float x, float y) {
    BFK_P float xx = x * x;
    BFK_P float yy = y * y;
    BFK_P float xy = x * y;
    BFK_P float r2 = xx + yy;
    BFK_P float r4 = r2 * r2;
    BFK_P float rad0 = fma(k1, r2, 1.0f);
    BFK_P float rad = fma(k2, r4, rad0);
    BFK_P float p1x2 = p1 * 2.0f;
    BFK_P float p2x2 = p2 * 2.0f;
    BFK_P float r2x = fma(xx, 2.0f, r2);
    BFK_P float r2y = fma(yy, 2.0f, r2);
    BFK_P float tx0 = p1x2 * xy;
    BFK_P float tx = fma(p2, r2x, tx0);
    BFK_P float ty0 = p2x2 * xy;
    BFK_P float ty = fma(p1, r2y, ty0);
    BFK_P float xd = fma(x, rad, tx);
    BFK_P float yd = fma(y, rad, ty);
    return vec4(xd, yd, 0.0f, 0.0f);
}

// D's Jacobian [dx/dx, dx/dy (= dy/dx), dy/dy, 0].
BFK_FN vec4 bfk_lens_jacobian(float k1, float k2, float p1, float p2, float x, float y) {
    BFK_P float xx = x * x;
    BFK_P float yy = y * y;
    BFK_P float xy = x * y;
    BFK_P float r2 = xx + yy;
    BFK_P float rad0 = fma(k1, r2, 1.0f);
    BFK_P float r4 = r2 * r2;
    BFK_P float rad = fma(k2, r4, rad0);
    BFK_P float k24 = k2 * 4.0f;
    BFK_P float k12 = k1 * 2.0f;
    BFK_P float g = fma(k24, r2, k12);
    BFK_P float p1x2 = p1 * 2.0f;
    BFK_P float p2x2 = p2 * 2.0f;
    BFK_P float p1x6 = p1 * 6.0f;
    BFK_P float p2x6 = p2 * 6.0f;
    BFK_P float a0 = fma(g, xx, rad);
    BFK_P float a1 = fma(p1x2, y, a0);
    BFK_P float dxx = fma(p2x6, x, a1);
    BFK_P float b0 = g * xy;
    BFK_P float b1 = fma(p1x2, x, b0);
    BFK_P float dxy = fma(p2x2, y, b1);
    BFK_P float c0 = fma(g, yy, rad);
    BFK_P float c1 = fma(p1x6, y, c0);
    BFK_P float dyy = fma(p2x2, x, c1);
    return vec4(dxx, dxy, dyy, 0.0f);
}

BFK_FN vec4 bfk_op_lens_warp(BFK_CTX, int base, vec4 p) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    BFK_P float inv = bfk_div(1.0f, h1.z);
    BFK_P float ox = p.x - h1.x;
    BFK_P float oy = p.y - h1.y;
    BFK_P float x = ox * inv;
    BFK_P float y = oy * inv;
    BFK_P float sx = x;
    BFK_P float sy = y;
    if (int(h1.w) == 0) {
        vec4 d = bfk_lens_model(h0.x, h0.y, h0.z, h0.w, x, y);
        sx = d.x;
        sy = d.y;
    } else {
        BFK_P float ux = x;
        BFK_P float uy = y;
        for (int i = 0; i < 12; ++i) {
            vec4 d = bfk_lens_model(h0.x, h0.y, h0.z, h0.w, ux, uy);
            vec4 j = bfk_lens_jacobian(h0.x, h0.y, h0.z, h0.w, ux, uy);
            BFK_P float fx = d.x - x;
            BFK_P float fy = d.y - y;
            BFK_P float det0 = j.x * j.z;
            BFK_P float det = fma(-j.y, j.y, det0);
            if (abs(det) > 1.0e-12f) {
                BFK_P float nx0 = j.y * fy;
                BFK_P float nx = fma(j.z, fx, -nx0);
                BFK_P float ny0 = j.y * fx;
                BFK_P float ny = fma(j.x, fy, -ny0);
                BFK_P float qx = bfk_div(nx, det);
                BFK_P float qy = bfk_div(ny, det);
                BFK_P float stepX = bfk_clamp(qx, -1.0f, 1.0f);
                BFK_P float stepY = bfk_clamp(qy, -1.0f, 1.0f);
                ux = ux - stepX;
                uy = uy - stepY;
            }
        }
        sx = ux;
        sy = uy;
    }
    BFK_P float rx = fma(sx, h1.z, h1.x);
    BFK_P float ry = fma(sy, h1.z, h1.y);
    return vec4(rx, ry, 0.0f, 0.0f);
}

// --- Test card ----------------------------------------------------------------------------------------
// [card, variant, option, flags] [width, height, center x, center y] [s0, s1, s2, s3] [phase, a1, a2, a3]
// then the label's characters, three six-bit codes to a lane in two vec4 (docs/user-guide/test-card.md).
// A card is drawn procedurally from the pixel's position in the card's own frame (x right and y down
// from its top left, in full-resolution composition pixels, so a circle is round and a cell square at
// any size) and returns the straight RGBA signal of that point. The raster's own sub-samples antialias
// it. Signal levels are the card's own numbers (0 black, 1 white); the node's color space turns them
// into working-space values downstream. Cards: 0 SD Bars, 1 HD Bars, 2 EBU Bars, 3 HDR Bars, 4 Patch,
// 5 Color Chart, 6 Gray Ramp, 7 Grid, 8 Circle, 9 Safe Areas, 10 UV Grid, 11 Zone Plate, 12 Siemens
// Star, 13 Moving Bar, 14 Countdown, 15 Alpha Ramp.

BFK_FN int bfk_tc_glyph_a(int ch) {
    switch (ch) {
    case 1:
        return 476789;
    case 2:
        return 143492;
    case 3:
        return 476194;
    case 4:
        return 476198;
    case 5:
        return 72018;
    case 6:
        return 1033153;
    case 7:
        return 205342;
    case 8:
        return 1016900;
    case 9:
        return 476718;
    case 10:
        return 476719;
    case 11:
        return 476735;
    case 12:
        return 1001022;
    case 13:
        return 476688;
    case 14:
        return 936497;
    case 15:
        return 1032734;
    case 16:
        return 1032734;
    case 17:
        return 476695;
    case 18:
        return 575039;
    case 19:
        return 462980;
    case 20:
        return 231490;
    case 21:
        return 576152;
    case 22:
        return 541200;
    case 23:
        return 585397;
    case 24:
        return 583347;
    case 25:
        return 476721;
    case 26:
        return 1001022;
    case 27:
        return 476721;
    case 28:
        return 1001022;
    case 29:
        return 508430;
    case 30:
        return 1020036;
    case 31:
        return 575025;
    case 32:
        return 575025;
    case 33:
        return 575029;
    case 34:
        return 574788;
    case 35:
        return 574788;
    case 36:
        return 1016900;
    case 38:
        return 12672;
    case 40:
        return 31;
    case 41:
        return 33860;
    case 42:
        return 844868;
    default:
        return 0;
    }
}

BFK_FN int bfk_tc_glyph_b(int ch) {
    switch (ch) {
    case 1:
        return 26158;
    case 2:
        return 4238;
    case 3:
        return 4383;
    case 4:
        return 1582;
    case 5:
        return 31810;
    case 6:
        return 1582;
    case 7:
        return 17966;
    case 8:
        return 8456;
    case 9:
        return 17966;
    case 10:
        return 1100;
    case 11:
        return 17969;
    case 12:
        return 17982;
    case 13:
        return 16942;
    case 14:
        return 18012;
    case 15:
        return 16927;
    case 16:
        return 16912;
    case 17:
        return 17967;
    case 18:
        return 17969;
    case 19:
        return 4238;
    case 20:
        return 2636;
    case 21:
        return 21073;
    case 22:
        return 16927;
    case 23:
        return 17969;
    case 24:
        return 17969;
    case 25:
        return 17966;
    case 26:
        return 16912;
    case 27:
        return 22093;
    case 28:
        return 21073;
    case 29:
        return 1086;
    case 30:
        return 4228;
    case 31:
        return 17966;
    case 32:
        return 17732;
    case 33:
        return 22385;
    case 34:
        return 10801;
    case 35:
        return 4228;
    case 36:
        return 8735;
    case 37:
        return 396;
    case 38:
        return 12672;
    case 39:
        return 12424;
    case 41:
        return 8720;
    case 42:
        return 8819;
    default:
        return 0;
    }
}

BFK_FN int bfk_tc_glyph_bit(int ch, int gx, int gy) {
    if (gx < 0 || gx > 4 || gy < 0 || gy > 6)
        return 0;
    int bits = 0;
    if (gy < 4)
        bits = (bfk_tc_glyph_a(ch) >> (5 * (3 - gy))) & 31;
    else
        bits = (bfk_tc_glyph_b(ch) >> (5 * (6 - gy))) & 31;
    return (bits >> (4 - gx)) & 1;
}

// 1 where (x, y), measured from the top left of one glyph cell, lies on a lit pixel of a glyph drawn
// with squares of g pixels.
BFK_FN int bfk_tc_hit(int ch, float x, float y, float g) {
    BFK_P float gx = bfk_div(x, g);
    BFK_P float gy = bfk_div(y, g);
    if (gx < 0.0f || gy < 0.0f)
        return 0;
    return bfk_tc_glyph_bit(ch, int(floor(gx)), int(floor(gy)));
}

BFK_FN int bfk_tc_label_char(vec4 l0, vec4 l1, int i) {
    int lane = i / 3;
    int shift = 6 * (i % 3);
    vec4 v = lane < 4 ? l0 : l1;
    int k = lane & 3;
    BFK_P float f = k == 0 ? v.x : (k == 1 ? v.y : (k == 2 ? v.z : v.w));
    return (int(f) >> shift) & 63;
}

BFK_FN int bfk_tc_digits(int n) {
    int d = 1;
    int m = n;
    for (int i = 0; i < 9; ++i) {
        if (m < 10)
            break;
        m = m / 10;
        d = d + 1;
    }
    return d;
}

// The decimal digit of n whose place value is 10^place.
BFK_FN int bfk_tc_digit(int n, int place) {
    int m = n;
    for (int i = 0; i < 9; ++i) {
        if (i >= place)
            break;
        m = m / 10;
    }
    return m % 10;
}

BFK_FN vec4 bfk_tc_rgb(int bits, float level) {
    BFK_P float r = (bits & 1) != 0 ? level : 0.0f;
    BFK_P float g = (bits & 2) != 0 ? level : 0.0f;
    BFK_P float b = (bits & 4) != 0 ? level : 0.0f;
    return vec4(r, g, b, 1.0f);
}

BFK_FN vec4 bfk_tc_gray(float v) { return vec4(v, v, v, 1.0f); }

// Bar color bits (R 1, G 2, B 4) of the seven bars white, yellow, cyan, green, magenta, red, blue.
BFK_FN int bfk_tc_bar_bits(int k) {
    switch (k) {
    case 0:
        return 7;
    case 1:
        return 3;
    case 2:
        return 6;
    case 3:
        return 2;
    case 4:
        return 5;
    case 5:
        return 1;
    default:
        return 4;
    }
}

// The largest rectangle of the given aspect (width over height; 0 the whole frame) centerd in the
// frame: the point's position in it, 0 to 1, and whether it is inside.
BFK_FN bool bfk_tc_fit(float px, float py, float w, float h, float aspect, BFK_OUT(float) u,
                       BFK_OUT(float) v) {
    BFK_P float rw = w;
    BFK_P float rh = h;
    if (aspect > 0.0f) {
        BFK_P float wide = h * aspect;
        if (wide <= w) {
            rw = wide;
        } else {
            rh = bfk_div(w, aspect);
        }
    }
    BFK_P float ox = (w - rw) * 0.5f;
    BFK_P float oy = (h - rh) * 0.5f;
    u = bfk_div(px - ox, rw);
    v = bfk_div(py - oy, rh);
    return u >= 0.0f && u < 1.0f && v >= 0.0f && v < 1.0f;
}

// Cards of color and level ------------------------------------------------------------------------

// SD Bars: seven 75% bars over two thirds of the height, the reversed castellation row, and the
// bottom row of -I, 100% white, +Q, black and the PLUGE steps below, at and above black.
BFK_FN vec4 bfk_tc_sd(float u, float v) {
    int col = min(max(int(floor(u * 7.0f)), 0), 6);
    if (v < 0.666666687f)
        return bfk_tc_rgb(bfk_tc_bar_bits(col), 0.75f);
    if (v < 0.75f) {
        int bits = 0;
        if (col == 0)
            bits = 4;
        else if (col == 2)
            bits = 5;
        else if (col == 4)
            bits = 6;
        else if (col == 6)
            bits = 7;
        return bfk_tc_rgb(bits, 0.75f);
    }
    if (u < 0.142857149f)
        return vec4(0.0f, 0.246575341f, 0.410958916f, 1.0f);
    if (u < 0.428571433f)
        return bfk_tc_gray(1.0f);
    if (u < 0.714285731f)
        return vec4(0.255707771f, 0.0f, 0.465753436f, 1.0f);
    if (u >= 0.714285731f && u < 0.857142866f) {
        int pluge = int(floor((u - 0.714285731f) * 21.0f));
        if (pluge <= 0)
            return bfk_tc_gray(-0.04f);
        if (pluge == 1)
            return bfk_tc_gray(0.0f);
        return bfk_tc_gray(0.04f);
    }
    return bfk_tc_gray(0.0f);
}

// The seven bar starts of the HD bars in 1920ths of the width, and the end.
BFK_FN float bfk_tc_hd_start(int k) {
    switch (k) {
    case 0:
        return 240.0f;
    case 1:
        return 445.714294f;
    case 2:
        return 651.428589f;
    case 3:
        return 857.142883f;
    case 4:
        return 1062.85718f;
    case 5:
        return 1268.57141f;
    case 6:
        return 1474.28577f;
    default:
        return 1680.0f;
    }
}

BFK_FN vec4 bfk_tc_hd_bar(float x, float level) {
    int k = 0;
    for (int i = 1; i < 7; ++i)
        if (x >= bfk_tc_hd_start(i))
            k = i;
    return bfk_tc_rgb(bfk_tc_bar_bits(k), level);
}

// HD Bars (SMPTE RP 219-1:2014, figure 1 and annex C with its default sub-patterns): four patterns of
// 7, 1, 1 and 3 twelfths of the height, widths in 1920ths of the width.
BFK_FN vec4 bfk_tc_hd(float u, float v) {
    BFK_P float x = u * 1920.0f;
    if (v < 0.583333313f) {
        if (x < 240.0f || x >= 1680.0f)
            return bfk_tc_gray(0.4f);
        return bfk_tc_hd_bar(x, 0.75f);
    }
    if (v < 0.666666687f) {
        if (x < 240.0f)
            return bfk_tc_rgb(6, 1.0f);
        if (x >= 1680.0f)
            return bfk_tc_rgb(4, 1.0f);
        return bfk_tc_gray(0.75f);
    }
    if (v < 0.75f) {
        if (x < 240.0f)
            return bfk_tc_rgb(3, 1.0f);
        if (x >= 1680.0f)
            return bfk_tc_rgb(1, 1.0f);
        if (x < 445.714294f)
            return bfk_tc_gray(0.0f);
        if (x >= 1474.28577f)
            return bfk_tc_gray(1.0f);
        return bfk_tc_gray(bfk_div(x - 445.714294f, 1028.57141f));
    }
    if (x < 240.0f || x >= 1680.0f)
        return bfk_tc_gray(0.15f);
    if (x < 549.0f)
        return bfk_tc_gray(0.0f);
    if (x < 960.0f)
        return bfk_tc_gray(1.0f);
    if (x < 1131.0f)
        return bfk_tc_gray(0.0f);
    if (x < 1200.0f)
        return bfk_tc_gray(-0.02f);
    if (x < 1268.0f)
        return bfk_tc_gray(0.0f);
    if (x < 1337.0f)
        return bfk_tc_gray(0.02f);
    if (x < 1405.0f)
        return bfk_tc_gray(0.0f);
    if (x < 1474.0f)
        return bfk_tc_gray(0.04f);
    return bfk_tc_gray(0.0f);
}

// EBU Bars (ITU-R BT.471, 100/0/75/0): white at 100%, the six colors at 75%, black; eight equal bars.
BFK_FN vec4 bfk_tc_ebu(float u) {
    int col = min(max(int(floor(u * 8.0f)), 0), 7);
    if (col == 0)
        return bfk_tc_gray(1.0f);
    if (col == 7)
        return bfk_tc_gray(0.0f);
    return bfk_tc_rgb(bfk_tc_bar_bits(col), 0.75f);
}

// HDR Bars (ITU-R BT.2111-3, figures 1 to 3 and tables 1 to 6): variant 0 HLG narrow range, 1 PQ narrow
// range, 2 PQ full range; the signal is the code value as a fraction of its range.
BFK_FN BFK_P float bfk_tc_hdr_sig(int code, int variant) {
    BFK_P float c = float(code);
    if (variant == 2)
        return bfk_div(c, 1023.0f);
    return bfk_div(c - 64.0f, 876.0f);
}

// The code values (10 bits each, red, green, blue) of the 75% (58% for PQ) BT.709 color bars, packed.
BFK_FN int bfk_tc_hdr_709(int variant, int k) {
    int key = variant * 6 + k;
    switch (key) {
    case 0:
        return (713 << 20) | (719 << 10) | 316;
    case 1:
        return (538 << 20) | (709 << 10) | 718;
    case 2:
        return (512 << 20) | (706 << 10) | 296;
    case 3:
        return (651 << 20) | (286 << 10) | 705;
    case 4:
        return (639 << 20) | (269 << 10) | 164;
    case 5:
        return (227 << 20) | (147 << 10) | 702;
    case 6:
        return (569 << 20) | (572 << 10) | 381;
    case 7:
        return (485 << 20) | (566 << 10) | 571;
    case 8:
        return (474 << 20) | (565 << 10) | 368;
    case 9:
        return (537 << 20) | (362 << 10) | 564;
    case 10:
        return (531 << 20) | (351 << 10) | 257;
    case 11:
        return (318 << 20) | (236 << 10) | 563;
    case 12:
        return (589 << 20) | (593 << 10) | 370;
    case 13:
        return (491 << 20) | (586 << 10) | 592;
    case 14:
        return (479 << 20) | (585 << 10) | 355;
    case 15:
        return (552 << 20) | (348 << 10) | 584;
    case 16:
        return (545 << 20) | (335 << 10) | 225;
    default:
        return (296 << 20) | (201 << 10) | 582;
    }
}

// The code of step j (0 to 12) of the stair: -7% (0% full range), 0, 10 ... 100% and 109% (100%).
BFK_FN int bfk_tc_hdr_step(int variant, int j) {
    if (variant == 2) {
        switch (j) {
        case 0:
            return 0;
        case 1:
            return 0;
        case 2:
            return 102;
        case 3:
            return 205;
        case 4:
            return 307;
        case 5:
            return 409;
        case 6:
            return 512;
        case 7:
            return 614;
        case 8:
            return 716;
        case 9:
            return 818;
        case 10:
            return 921;
        default:
            return 1023;
        }
    }
    switch (j) {
    case 0:
        return 4;
    case 1:
        return 64;
    case 2:
        return 152;
    case 3:
        return 239;
    case 4:
        return 327;
    case 5:
        return 414;
    case 6:
        return 502;
    case 7:
        return 590;
    case 8:
        return 677;
    case 9:
        return 765;
    case 10:
        return 852;
    case 11:
        return 940;
    default:
        return 1019;
    }
}

BFK_FN vec4 bfk_tc_hdr_code_rgb(int codes, int variant) {
    BFK_P float r = bfk_tc_hdr_sig((codes >> 20) & 1023, variant);
    BFK_P float g = bfk_tc_hdr_sig((codes >> 10) & 1023, variant);
    BFK_P float b = bfk_tc_hdr_sig(codes & 1023, variant);
    return vec4(r, g, b, 1.0f);
}

BFK_FN float bfk_tc_hdr_start(int k) {
    switch (k) {
    case 0:
        return 240.0f;
    case 1:
        return 446.0f;
    case 2:
        return 652.0f;
    case 3:
        return 858.0f;
    case 4:
        return 1062.0f;
    case 5:
        return 1268.0f;
    case 6:
        return 1474.0f;
    default:
        return 1680.0f;
    }
}

BFK_FN vec4 bfk_tc_hdr_bar(float x, float level) {
    int k = 0;
    for (int i = 1; i < 7; ++i)
        if (x >= bfk_tc_hdr_start(i))
            k = i;
    return bfk_tc_rgb(bfk_tc_bar_bits(k), level);
}

BFK_FN vec4 bfk_tc_hdr(float u, float v, int variant) {
    BFK_P float x = u * 1920.0f;
    int white = variant == 0 ? 721 : (variant == 1 ? 573 : 594);
    int gray = variant == 2 ? 409 : 414;
    int black = variant == 2 ? 0 : 64;
    if (v < 0.583333313f) {
        if (x < 240.0f || x >= 1680.0f)
            return bfk_tc_gray(bfk_tc_hdr_sig(gray, variant));
        BFK_P float bar = v < 0.0833333358f ? 1.0f : bfk_tc_hdr_sig(white, variant);
        return bfk_tc_hdr_bar(x, bar);
    }
    if (v < 0.666666687f) {
        if (x < 240.0f || x >= 1680.0f)
            return bfk_tc_gray(bfk_tc_hdr_sig(white, variant));
        if (x < 446.0f)
            return bfk_tc_gray(bfk_tc_hdr_sig(bfk_tc_hdr_step(variant, 0), variant));
        int bar = 1;
        for (int i = 2; i < 7; ++i)
            if (x >= bfk_tc_hdr_start(i))
                bar = i;
        BFK_P float mid = (bfk_tc_hdr_start(bar) + bfk_tc_hdr_start(bar + 1)) * 0.5f;
        int j = 1 + 2 * (bar - 1) + (x >= mid ? 1 : 0);
        return bfk_tc_gray(bfk_tc_hdr_sig(bfk_tc_hdr_step(variant, j), variant));
    }
    if (v < 0.75f) {
        if (x < 240.0f)
            return bfk_tc_gray(bfk_tc_hdr_sig(black, variant));
        BFK_P float ramp = 0.0f;
        if (variant == 2) {
            ramp = bfk_clamp(x - 858.0f, 0.0f, 1023.0f);
            return bfk_tc_gray(bfk_div(ramp, 1023.0f));
        }
        ramp = bfk_clamp(4.0f + (x - 799.0f), 4.0f, 1019.0f);
        return bfk_tc_gray(bfk_div(ramp - 64.0f, 876.0f));
    }
    if (x < 240.0f) {
        int k = int(floor(bfk_div(x, 80.0f)));
        return bfk_tc_hdr_code_rgb(bfk_tc_hdr_709(variant, min(k, 2)), variant);
    }
    if (x >= 1680.0f) {
        int k = 3 + int(floor(bfk_div(x - 1680.0f, 80.0f)));
        return bfk_tc_hdr_code_rgb(bfk_tc_hdr_709(variant, min(k, 5)), variant);
    }
    int code = black;
    if (x >= 376.0f && x < 446.0f)
        code = variant == 2 ? 0 : 48;
    else if (x >= 514.0f && x < 584.0f)
        code = variant == 2 ? 19 : 80;
    else if (x >= 652.0f && x < 722.0f)
        code = variant == 2 ? 41 : 99;
    else if (x >= 960.0f && x < 1398.0f)
        code = white;
    return bfk_tc_gray(bfk_tc_hdr_sig(code, variant));
}

// Patch: the 27 values, black and the gray steps first, then the primaries and secondaries at 75% and
// 100%.
BFK_FN vec4 bfk_tc_patch_value(int index) {
    if (index == 0)
        return bfk_tc_gray(0.0f);
    if (index == 1)
        return bfk_tc_gray(0.02f);
    if (index == 2)
        return bfk_tc_gray(0.04f);
    if (index <= 11)
        return bfk_tc_gray(float(index - 2) * 0.1f);
    if (index == 12)
        return bfk_tc_gray(0.95f);
    if (index == 13)
        return bfk_tc_gray(0.98f);
    if (index == 14)
        return bfk_tc_gray(1.0f);
    int k = (index - 15) % 6;
    BFK_P float level = index < 21 ? 0.75f : 1.0f;
    // red, green, blue, cyan, magenta, yellow
    int bits = k == 0 ? 1 : (k == 1 ? 2 : (k == 2 ? 4 : (k == 3 ? 6 : (k == 4 ? 5 : 3))));
    return bfk_tc_rgb(bits, level);
}

BFK_FN vec4 bfk_tc_patch(float u, float v, int index, int window) {
    BFK_P float area = window == 1 ? 0.5f : (window == 2 ? 0.25f : (window == 3 ? 0.1f : 1.0f));
    if (window != 0) {
        BFK_P float reach = bfk_sqrt(area) * 0.5f;
        BFK_P float dx = abs(u - 0.5f);
        BFK_P float dy = abs(v - 0.5f);
        if (dx >= reach || dy >= reach)
            return bfk_tc_gray(0.0f);
    }
    return bfk_tc_patch_value(index);
}

// Color Chart: 6 by 4 patches of our own values: neutrals, the primaries and secondaries at 100% and
// 50%, and six skin tones.
BFK_FN vec4 bfk_tc_chart(float u, float v) {
    BFK_P float fx = u * 6.0f;
    BFK_P float fy = v * 4.0f;
    int col = min(max(int(floor(fx)), 0), 5);
    int row = min(max(int(floor(fy)), 0), 3);
    BFK_P float cx = fx - float(col);
    BFK_P float cy = fy - float(row);
    if (cx < 0.04f || cx >= 0.96f || cy < 0.04f || cy >= 0.96f)
        return bfk_tc_gray(0.12f);
    if (row == 0)
        return bfk_tc_gray(1.0f - float(col) * 0.2f);
    if (row < 3) {
        int bits = col == 0 ? 1 : (col == 1 ? 2 : (col == 2 ? 4 : (col == 3 ? 6 : (col == 4 ? 5 : 3))));
        return bfk_tc_rgb(bits, row == 1 ? 1.0f : 0.5f);
    }
    switch (col) {
    case 0:
        return vec4(0.90f, 0.74f, 0.64f, 1.0f);
    case 1:
        return vec4(0.82f, 0.62f, 0.50f, 1.0f);
    case 2:
        return vec4(0.72f, 0.52f, 0.40f, 1.0f);
    case 3:
        return vec4(0.58f, 0.40f, 0.30f, 1.0f);
    case 4:
        return vec4(0.42f, 0.28f, 0.21f, 1.0f);
    default:
        return vec4(0.28f, 0.18f, 0.14f, 1.0f);
    }
}

// Gray Ramp: a smooth ramp from 0 to 1 over the top four fifths, PLUGE steps below.
BFK_FN vec4 bfk_tc_ramp(float u, float v) {
    if (v < 0.8f)
        return bfk_tc_gray(u);
    int k = min(max(int(floor(u * 9.0f)), 0), 8);
    BFK_P float level = 0.0f;
    if (k == 1)
        level = -0.04f;
    else if (k == 3)
        level = -0.02f;
    else if (k == 5)
        level = 0.02f;
    else if (k == 7)
        level = 0.04f;
    return bfk_tc_gray(level);
}

// Cards of geometry and framing --------------------------------------------------------------------

// 1 within half a line width of the nearest multiple of `spacing` of d.
BFK_FN int bfk_tc_on_line(float d, float spacing, float lw) {
    BFK_P float k = floor(bfk_div(d, spacing) + 0.5f);
    BFK_P float off = abs(d - k * spacing);
    return off <= lw * 0.5f ? 1 : 0;
}

BFK_FN vec4 bfk_tc_grid(float px, float py, float w, float h, float spacing, float lw) {
    BFK_P float dx = px - w * 0.5f;
    BFK_P float dy = py - h * 0.5f;
    if (abs(dx) <= lw * 0.5f || abs(dy) <= lw * 0.5f)
        return vec4(0.9f, 0.8f, 0.1f, 1.0f);
    if (bfk_tc_on_line(dx, spacing, lw) != 0 || bfk_tc_on_line(dy, spacing, lw) != 0)
        return bfk_tc_gray(0.85f);
    return bfk_tc_gray(0.08f);
}

BFK_FN int bfk_tc_outline(float px, float py, float x0, float y0, float x1, float y1, float lw) {
    BFK_P float hw = lw * 0.5f;
    if (px < x0 - hw || px > x1 + hw || py < y0 - hw || py > y1 + hw)
        return 0;
    if (px > x0 + hw && px < x1 - hw && py > y0 + hw && py < y1 - hw)
        return 0;
    return 1;
}

BFK_FN vec4 bfk_tc_circle(float px, float py, float w, float h) {
    BFK_P float s = min(w, h);
    BFK_P float lw = max(1.0f, bfk_div(s, 540.0f));
    BFK_P float dx = px - w * 0.5f;
    BFK_P float dy = py - h * 0.5f;
    if (px < lw * 2.0f || py < lw * 2.0f || px > w - lw * 2.0f || py > h - lw * 2.0f)
        return bfk_tc_gray(0.95f);
    if (abs(dx) <= lw || abs(dy) <= lw)
        return bfk_tc_gray(0.95f);
    BFK_P float r = bfk_sqrt(dx * dx + dy * dy);
    BFK_P float ring = s * 0.45f;
    if (abs(r - ring) <= lw)
        return bfk_tc_gray(0.95f);
    if (abs(r - ring * 0.5f) <= lw * 0.5f)
        return vec4(0.9f, 0.8f, 0.1f, 1.0f);
    BFK_P float cell = bfk_div(s, 20.0f);
    if (bfk_tc_on_line(dx, cell * 5.0f, lw) != 0 || bfk_tc_on_line(dy, cell * 5.0f, lw) != 0)
        return bfk_tc_gray(0.45f);
    if (bfk_tc_on_line(dx, cell, lw * 0.5f) != 0 || bfk_tc_on_line(dy, cell, lw * 0.5f) != 0)
        return bfk_tc_gray(0.24f);
    return bfk_tc_gray(0.08f);
}

// The largest rectangle of the given aspect centerd in the frame, as its corners.
BFK_FN vec4 bfk_tc_crop(float w, float h, float aspect) {
    BFK_P float cw = w;
    BFK_P float ch = h;
    BFK_P float wide = h * aspect;
    if (wide <= w)
        cw = wide;
    else
        ch = bfk_div(w, aspect);
    return vec4((w - cw) * 0.5f, (h - ch) * 0.5f, (w + cw) * 0.5f, (h + ch) * 0.5f);
}

// Safe Areas: action safe 93% and title safe 90% of the width and the height, the center, and the crop
// guides selected in the option's bits (1 16:9, 2 9:16, 4 1:1, 8 2.39:1).
BFK_FN vec4 bfk_tc_safe(float px, float py, float w, float h, int guides) {
    BFK_P float lw = max(2.0f, bfk_div(min(w, h), 360.0f));
    BFK_P float cx = w * 0.5f;
    BFK_P float cy = h * 0.5f;
    if (bfk_tc_outline(px, py, cx - w * 0.465f, cy - h * 0.465f, cx + w * 0.465f, cy + h * 0.465f,
                       lw) != 0)
        return vec4(0.9f, 0.8f, 0.1f, 1.0f);
    if (bfk_tc_outline(px, py, cx - w * 0.45f, cy - h * 0.45f, cx + w * 0.45f, cy + h * 0.45f,
                       lw) != 0)
        return vec4(0.1f, 0.8f, 0.9f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        if ((guides & (1 << i)) == 0)
            continue;
        BFK_P float aspect = i == 0 ? 1.77777779f : (i == 1 ? 0.5625f : (i == 2 ? 1.0f : 2.39f));
        vec4 r = bfk_tc_crop(w, h, aspect);
        if (bfk_tc_outline(px, py, r.x, r.y, r.z, r.w, lw) != 0) {
            if (i == 0)
                return vec4(0.95f, 0.45f, 0.1f, 1.0f);
            if (i == 1)
                return vec4(0.5f, 0.9f, 0.2f, 1.0f);
            if (i == 2)
                return vec4(0.95f, 0.3f, 0.7f, 1.0f);
            return vec4(0.4f, 0.5f, 1.0f, 1.0f);
        }
    }
    BFK_P float dx = abs(px - cx);
    BFK_P float dy = abs(py - cy);
    BFK_P float arm = min(w, h) * 0.03f;
    if ((dx <= lw * 0.5f && dy <= arm) || (dy <= lw * 0.5f && dx <= arm))
        return bfk_tc_gray(0.95f);
    return bfk_tc_gray(0.12f);
}

// UV Grid: square cells `cell` pixels across, colored by their position (red across, green up, a
// little blue), a dark line between them, and each cell's column and row written in it.
BFK_FN vec4 bfk_tc_uv(float px, float py, float w, float h, float cell) {
    int col = int(floor(bfk_div(px, cell)));
    int row = int(floor(bfk_div(py, cell)));
    BFK_P float fx = px - float(col) * cell;
    BFK_P float fy = py - float(row) * cell;
    BFK_P float lw = max(1.0f, bfk_div(cell, 32.0f));
    if (fx < lw * 0.5f || fy < lw * 0.5f || fx > cell - lw * 0.5f || fy > cell - lw * 0.5f)
        return bfk_tc_gray(0.0f);
    BFK_P float g = floor(bfk_div(cell, 36.0f));
    if (g >= 1.0f) {
        int nc = bfk_tc_digits(col);
        int nr = bfk_tc_digits(row);
        int total = nc + 1 + nr;
        BFK_P float tw = float(total) * 6.0f * g - g;
        BFK_P float ox = (cell - tw) * 0.5f;
        BFK_P float oy = (cell - 7.0f * g) * 0.5f;
        for (int pass = 0; pass < 2; ++pass) {
            BFK_P float shift = pass == 0 ? g : 0.0f;
            BFK_P float tx = fx - ox - shift;
            BFK_P float ty = fy - oy - shift;
            if (tx < 0.0f || ty < 0.0f || tx >= tw || ty >= 7.0f * g)
                continue;
            int k = int(floor(bfk_div(tx, 6.0f * g)));
            BFK_P float inner = tx - float(k) * 6.0f * g;
            int ch = 0;
            if (k < nc)
                ch = 1 + bfk_tc_digit(col, nc - 1 - k);
            else if (k == nc)
                ch = 39;
            else
                ch = 1 + bfk_tc_digit(row, nr - 1 - (k - nc - 1));
            if (bfk_tc_hit(ch, inner, ty, g) != 0)
                return pass == 0 ? bfk_tc_gray(0.0f) : bfk_tc_gray(1.0f);
        }
    }
    BFK_P float u = bfk_div(px, w);
    BFK_P float v = 1.0f - bfk_div(py, h);
    return vec4(bfk_clamp(u, 0.0f, 1.0f), bfk_clamp(v, 0.0f, 1.0f), 0.25f, 1.0f);
}

// Cards of sharpness and motion --------------------------------------------------------------------

// Zone Plate: 0.5 + 0.5 cos(2 pi (r^2 / (4 R) + phase)), whose local frequency reaches the Nyquist
// limit (0.5 cycle per pixel) at R, half the frame's diagonal.
BFK_FN vec4 bfk_tc_zone(float px, float py, float w, float h, float phase) {
    BFK_P float dx = px - w * 0.5f;
    BFK_P float dy = py - h * 0.5f;
    BFK_P float rmax = bfk_sqrt(w * w + h * h) * 0.5f;
    BFK_P float r2 = dx * dx + dy * dy;
    BFK_P float turns = bfk_div(r2, rmax * 4.0f) + phase;
    BFK_P float s = 0.0f;
    BFK_P float c = 0.0f;
    bfk_sincos(turns, s, c);
    BFK_P float value = c * 0.5f + 0.5f;
    return bfk_tc_gray(value);
}

// Siemens Star: `spokes` alternating black and white spokes inside a circle of 45% of the shorter
// side.
BFK_FN vec4 bfk_tc_star(float px, float py, float w, float h, float spokes) {
    BFK_P float dx = px - w * 0.5f;
    BFK_P float dy = py - h * 0.5f;
    BFK_P float r = bfk_sqrt(dx * dx + dy * dy);
    if (r > min(w, h) * 0.45f)
        return bfk_tc_gray(0.18f);
    BFK_P float turns = bfk_atan2(dy, dx);
    BFK_P float s = turns * spokes;
    BFK_P float f = s - floor(s);
    return bfk_tc_gray(f < 0.5f ? 1.0f : 0.0f);
}

// Moving Bar: a white bar `bar` pixels wide at `phase` (0 to 1) of its way from off the left (top)
// edge to off the right (bottom) edge.
BFK_FN vec4 bfk_tc_bar(float px, float py, float w, float h, float bar, float phase, int vertical) {
    BFK_P float extent = vertical != 0 ? h : w;
    BFK_P float at = vertical != 0 ? py : px;
    BFK_P float start = phase * (extent + bar) - bar;
    if (at >= start && at < start + bar)
        return bfk_tc_gray(1.0f);
    return bfk_tc_gray(0.1f);
}

// Countdown: the seconds left in large digits, the frame within the second under them, and a white
// flash over the whole frame on the first frame of each second (a3).
BFK_FN vec4 bfk_tc_countdown(float px, float py, float w, float h, int seconds, int frame, int fps,
                             bool flash) {
    if (flash)
        return bfk_tc_gray(1.0f);
    int nd = bfk_tc_digits(seconds);
    BFK_P float g = max(1.0f, floor(min(bfk_div(h, 18.0f), bfk_div(w, float(nd) * 6.0f))));
    BFK_P float tw = float(nd) * 6.0f * g - g;
    BFK_P float tx = px - (w - tw) * 0.5f;
    BFK_P float ty = py - (h * 0.5f - 7.5f * g);
    if (tx >= 0.0f && tx < tw && ty >= 0.0f && ty < 7.0f * g) {
        int k = int(floor(bfk_div(tx, 6.0f * g)));
        BFK_P float inner = tx - float(k) * 6.0f * g;
        if (bfk_tc_hit(1 + bfk_tc_digit(seconds, nd - 1 - k), inner, ty, g) != 0)
            return bfk_tc_gray(0.95f);
    }
    int fd = fps >= 100 ? 3 : 2;
    int total = 6 + fd;
    BFK_P float g2 = max(1.0f, floor(min(bfk_div(h, 60.0f), bfk_div(w, float(total) * 6.0f))));
    BFK_P float tw2 = float(total) * 6.0f * g2 - g2;
    BFK_P float ux = px - (w - tw2) * 0.5f;
    BFK_P float uy = py - (h * 0.5f + 2.0f * g2);
    if (ux >= 0.0f && ux < tw2 && uy >= 0.0f && uy < 7.0f * g2) {
        int k = int(floor(bfk_div(ux, 6.0f * g2)));
        BFK_P float inner = ux - float(k) * 6.0f * g2;
        int ch = 0;
        if (k == 0)
            ch = 16;
        else if (k == 1)
            ch = 28;
        else if (k == 2)
            ch = 11;
        else if (k == 3)
            ch = 23;
        else if (k == 4)
            ch = 15;
        else if (k >= 6)
            ch = 1 + bfk_tc_digit(frame, fd - 1 - (k - 6));
        if (bfk_tc_hit(ch, inner, uy, g2) != 0)
            return bfk_tc_gray(0.7f);
    }
    return bfk_tc_gray(0.1f);
}

// Alpha Ramp: six bands of color, each with an alpha rising from 0 to 1 across the width. Straight
// color; the caller premultiplies.
BFK_FN vec4 bfk_tc_alpha(float u, float v) {
    int band = min(max(int(floor(v * 6.0f)), 0), 5);
    vec4 c = bfk_tc_gray(0.0f);
    if (band == 0)
        c = bfk_tc_gray(1.0f);
    else if (band == 1)
        c = bfk_tc_rgb(1, 1.0f);
    else if (band == 2)
        c = bfk_tc_rgb(2, 1.0f);
    else if (band == 3)
        c = bfk_tc_rgb(4, 1.0f);
    else if (band == 5)
        c = bfk_tc_gray(0.5f);
    return vec4(c.x, c.y, c.z, bfk_clamp(u, 0.0f, 1.0f));
}

// The label: the card's name and size on a dark plate in the top left corner.
BFK_FN vec4 bfk_tc_label(vec4 under, vec4 l0, vec4 l1, float px, float py, float w, float h, int n) {
    BFK_P float g = max(1.0f, floor(bfk_div(min(w, h), 270.0f)));
    BFK_P float m = g * 3.0f;
    BFK_P float pad = g * 2.0f;
    BFK_P float tw = float(n) * 6.0f * g - g;
    if (px < m || py < m || px >= m + pad * 2.0f + tw || py >= m + pad * 2.0f + 7.0f * g)
        return under;
    BFK_P float tx = px - m - pad;
    BFK_P float ty = py - m - pad;
    if (tx >= 0.0f && tx < tw && ty >= 0.0f && ty < 7.0f * g) {
        int k = int(floor(bfk_div(tx, 6.0f * g)));
        BFK_P float inner = tx - float(k) * 6.0f * g;
        if (bfk_tc_hit(bfk_tc_label_char(l0, l1, k), inner, ty, g) != 0)
            return bfk_tc_gray(0.95f);
    }
    return bfk_tc_gray(0.04f);
}

BFK_FN vec4 bfk_op_test_card(BFK_CTX, int base, float qx, float qy) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    vec4 h2 = bfk_data(BFK_CTXA, base + 2);
    vec4 h3 = bfk_data(BFK_CTXA, base + 3);
    vec4 l0 = bfk_data(BFK_CTXA, base + 4);
    vec4 l1 = bfk_data(BFK_CTXA, base + 5);
    int card = int(h0.x);
    int variant = int(h0.y);
    int option = int(h0.z);
    int flags = int(h0.w);
    BFK_P float w = h1.x;
    BFK_P float h = h1.y;
    BFK_P float px = (qx - h1.z) + w * 0.5f;
    BFK_P float py = h * 0.5f - (qy - h1.w);
    BFK_P float u = 0.0f;
    BFK_P float v = 0.0f;
    vec4 c = bfk_tc_gray(0.0f);
    bool inside = true;
    switch (card) {
    case 0:
        inside = bfk_tc_fit(px, py, w, h, 1.77777779f, u, v);
        c = bfk_tc_sd(u, v);
        break;
    case 1:
        inside = bfk_tc_fit(px, py, w, h, 1.77777779f, u, v);
        c = bfk_tc_hd(u, v);
        break;
    case 2:
        inside = bfk_tc_fit(px, py, w, h, 1.77777779f, u, v);
        c = bfk_tc_ebu(u);
        break;
    case 3:
        inside = bfk_tc_fit(px, py, w, h, 1.77777779f, u, v);
        c = bfk_tc_hdr(u, v, variant);
        break;
    case 4:
        inside = bfk_tc_fit(px, py, w, h, 0.0f, u, v);
        c = bfk_tc_patch(u, v, variant, option);
        break;
    case 5:
        inside = bfk_tc_fit(px, py, w, h, 1.5f, u, v);
        c = bfk_tc_chart(u, v);
        break;
    case 6:
        inside = bfk_tc_fit(px, py, w, h, 0.0f, u, v);
        c = bfk_tc_ramp(u, v);
        break;
    case 7:
        c = bfk_tc_grid(px, py, w, h, h2.x, h2.y);
        break;
    case 8:
        c = bfk_tc_circle(px, py, w, h);
        break;
    case 9:
        c = bfk_tc_safe(px, py, w, h, option);
        break;
    case 10:
        c = bfk_tc_uv(px, py, w, h, h2.x);
        break;
    case 11:
        c = bfk_tc_zone(px, py, w, h, h3.x);
        break;
    case 12:
        c = bfk_tc_star(px, py, w, h, h2.x);
        break;
    case 13:
        c = bfk_tc_bar(px, py, w, h, h2.x, h3.x, option);
        break;
    case 14:
        c = bfk_tc_countdown(px, py, w, h, int(h3.y), int(h3.z), int(h2.x), h3.w > 0.5f);
        break;
    default:
        inside = bfk_tc_fit(px, py, w, h, 0.0f, u, v);
        c = bfk_tc_alpha(u, v);
        break;
    }
    if (!inside)
        c = bfk_tc_gray(0.0f);
    if ((flags & 1) != 0)
        c = bfk_tc_label(c, l0, l1, px, py, w, h, (flags >> 4) & 31);
    BFK_P float a = bfk_clamp(c.w, 0.0f, 1.0f);
    BFK_P float r = c.x * a;
    BFK_P float g = c.y * a;
    BFK_P float b = c.z * a;
    return vec4(r, g, b, a);
}

// --- Light rays ----------------------------------------------------------------------------------
// [slot, source, centre x, centre y] [length, falloff, boost, 0]: the rays seen from the position p
// of a pixel. The pixel gathers slot `slot` along the segment from p towards the centre that covers
// `length` of the way, in one sample per two pixels of it (at least one, at most 96), each sample the
// filtered premultiplied texel's luma (source 0) or alpha (source 1), weighed (1 - t)^falloff at the
// fraction t of the segment it lies at; the result is the weighted mean times the boost, as
// [m, m, m, min(m, 1)]. A pixel at the centre, a zero length or a zero boost gathers nothing.

BFK_FN vec4 bfk_op_rays(BFK_CTX, int base, vec4 p) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    BFK_P float dx = h0.z - p.x;
    BFK_P float dy = h0.w - p.y;
    BFK_P float dist = bfk_length(dx, dy);
    BFK_P float reach = dist * h1.x;
    if (!(reach > 0.0f) || !(h1.z > 0.0f))
        return vec4(0.0f, 0.0f, 0.0f, 0.0f);
    BFK_P float samples = reach * 0.5f;
    int n = min(int(floor(samples)) + 1, 96);
    BFK_P float invN = bfk_div(1.0f, float(n));
    int slot = int(h0.x);
    int source = int(h0.y);
    BFK_P float sum = 0.0f;
    BFK_P float weights = 0.0f;
    for (int i = 0; i < 96; ++i) {
        if (i >= n)
            break;
        BFK_P float t = (float(i) + 0.5f) * invN;
        BFK_P float along = t * h1.x;
        BFK_P float qx = fma(dx, along, p.x);
        BFK_P float qy = fma(dy, along, p.y);
        vec4 texel = bfk_sample(BFK_CTXA, slot, qx, qy, 0, 1);
        BFK_P float value = source == 1 ? texel.w : bfk_channel(texel, 4);
        BFK_P float rest = 1.0f - t;
        BFK_P float w = bfk_pow(rest, h1.y);
        sum = fma(value, w, sum);
        weights = weights + w;
    }
    if (!(weights > 0.0f))
        return vec4(0.0f, 0.0f, 0.0f, 0.0f);
    BFK_P float mean = bfk_div(sum, weights);
    BFK_P float m = mean * h1.z;
    BFK_P float a = min(m, 1.0f);
    return vec4(m, m, m, a);
}

// --- Plumbing -----------------------------------------------------------------------------------------

BFK_FN BFK_P float bfk_lane(vec4 v, float selector) {
    int s = int(selector);
    if (s == 0)
        return v.x;
    if (s == 1)
        return v.y;
    if (s == 2)
        return v.z;
    if (s == 3)
        return v.w;
    return s == 5 ? 1.0f : 0.0f;
}

// [lane 0..3 (4 zero, 5 one) for x, y, z, w] [scale x, y, z, w].
BFK_FN vec4 bfk_op_swizzle(BFK_CTX, int base, vec4 a) {
    vec4 l = bfk_data(BFK_CTXA, base);
    vec4 s = bfk_data(BFK_CTXA, base + 1);
    BFK_P float x = bfk_lane(a, l.x) * s.x;
    BFK_P float y = bfk_lane(a, l.y) * s.y;
    BFK_P float z = bfk_lane(a, l.z) * s.z;
    BFK_P float w = bfk_lane(a, l.w) * s.w;
    return vec4(x, y, z, w);
}

// [mode, 1 / (2 step), strength, 0], inputs f(p + h x), f(p - h x), f(p + h y), f(p - h y).
// Modes 0 Gradient of a scalar, 1 Curl of a scalar (Bridson et al. 2007: (d/dy, -d/dx)),
// 2 Curl of a vector (d vy/dx - d vx/dy), 3 Divergence (d vx/dx + d vy/dy).
BFK_FN vec4 bfk_op_derivative(BFK_CTX, int base, vec4 xp, vec4 xm, vec4 yp, vec4 ym) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    BFK_P float k = h0.y * h0.z;
    int mode = int(h0.x);
    if (mode == 0 || mode == 1) {
        BFK_P float ex = xp.x - xm.x;
        BFK_P float ey = yp.x - ym.x;
        BFK_P float gx = ex * k;
        BFK_P float gy = ey * k;
        if (mode == 0)
            return vec4(gx, gy, 0.0f, 0.0f);
        return vec4(gy, -gx, 0.0f, 0.0f);
    }
    BFK_P float vyx = xp.y - xm.y;
    BFK_P float vxy = yp.x - ym.x;
    BFK_P float vxx = xp.x - xm.x;
    BFK_P float vyy = yp.y - ym.y;
    BFK_P float c0 = vyx - vxy;
    BFK_P float d0 = vxx + vyy;
    BFK_P float r = mode == 2 ? c0 * k : d0 * k;
    return vec4(r, r, 0.0f, 0.0f);
}

// --- Inspection -------------------------------------------------------------------------------------

// The centre of the grid cell of `spacing` field pixels holding p: [spacing, 1 / spacing, 0, 0].
BFK_FN vec4 bfk_op_cell_centre(BFK_CTX, int base, vec4 p) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    BFK_P float ux = p.x * h0.y;
    BFK_P float uy = p.y * h0.y;
    BFK_P float fx = floor(ux) + 0.5f;
    BFK_P float fy = floor(uy) + 0.5f;
    BFK_P float x = fx * h0.x;
    BFK_P float y = fy * h0.x;
    return vec4(x, y, 0.0f, 0.0f);
}

// An arrow of the vector v drawn from its cell centre c: [spacing, length per unit, half width
// (field pixels), 0], antialiased over the sampling footprint. Coverage in x.
BFK_FN vec4 bfk_op_arrow(BFK_CTX, int base, vec4 p, vec4 c, vec4 v) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    BFK_P float l = bfk_length(v.x, v.y);
    if (!(l > 0.0f))
        return vec4(0.0f, 0.0f, 0.0f, 0.0f);
    BFK_P float ux = bfk_div(v.x, l);
    BFK_P float uy = bfk_div(v.y, l);
    BFK_P float wanted = l * h0.y;
    BFK_P float most = h0.x * 0.45f;
    BFK_P float len = min(wanted, most);
    BFK_P float dx = p.x - c.x;
    BFK_P float dy = p.y - c.y;
    BFK_P float a0 = uy * dy;
    BFK_P float along = fma(ux, dx, a0);
    BFK_P float a1 = uy * dx;
    BFK_P float across = fma(ux, dy, -a1);
    BFK_P float halfv = len * 0.5f;
    BFK_P float head = min(len * 0.35f, h0.x * 0.2f);
    BFK_P float shaftEnd = halfv - head;
    BFK_P float clamped = bfk_clamp(along, -halfv, shaftEnd);
    BFK_P float sx = along - clamped;
    BFK_P float shaft = bfk_length(sx, across);
    BFK_P float sd = shaft - h0.z;
    // The head: a triangle from the shaft's end to the tip, half as wide as it is long.
    BFK_P float into = along - shaftEnd;
    BFK_P float t = head > 0.0f ? bfk_div(into, head) : 1.0f;
    BFK_P float halfWide = (1.0f - t) * head * 0.5f;
    BFK_P float hd = abs(across) - halfWide;
    BFK_P float inHead = (t >= 0.0f && t <= 1.0f) ? hd : 1.0e6f;
    BFK_P float d = min(sd, inHead);
    vec4 sampling = bfk_data(BFK_CTXA, BFK_SAMPLING);
    BFK_P float pixel = max(sampling.y, 0.5f);
    BFK_P float k = bfk_div(d, pixel);
    BFK_P float cover = bfk_clamp(0.5f - k, 0.0f, 1.0f);
    return vec4(cover, 0.0f, 0.0f, 0.0f);
}

// A line integral convolution of hashed noise along the local direction of v (Cabral and Leedom,
// SIGGRAPH 1993), seventeen taps: [tap step, 1 / noise cell, 0, 0].
BFK_FN vec4 bfk_op_streak(BFK_CTX, int base, vec4 p, vec4 v) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    BFK_P float l = bfk_length(v.x, v.y);
    BFK_P float ux = l > 0.0f ? bfk_div(v.x, l) : 0.0f;
    BFK_P float uy = l > 0.0f ? bfk_div(v.y, l) : 0.0f;
    BFK_P float sum = 0.0f;
    for (int k = -8; k <= 8; ++k) {
        BFK_P float s = float(k) * h0.x;
        BFK_P float x = fma(ux, s, p.x);
        BFK_P float y = fma(uy, s, p.y);
        BFK_P float cx = floor(x * h0.y);
        BFK_P float cy = floor(y * h0.y);
        uint h = bfk_hash(uint(int(cx)), uint(int(cy)), 7u, 525u);
        sum = sum + bfk_unit(h);
    }
    BFK_P float r = sum * 0.0588235296f;
    return vec4(r, 0.0f, 0.0f, 0.0f);
}

// A colour from a ramp of 256 straight colours over [low, low + 1 / scale]: [low, scale, 0, 0], then
// the 256 vec4.
BFK_FN vec4 bfk_op_ramp(BFK_CTX, int base, vec4 a) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    BFK_P float d = a.x - h0.x;
    BFK_P float t0 = d * h0.y;
    BFK_P float t = bfk_clamp(t0, 0.0f, 1.0f);
    BFK_P float s = t * 255.0f;
    BFK_P float fl = floor(s);
    int i = min(int(fl), 254);
    BFK_P float f = s - float(i);
    vec4 lo = bfk_data(BFK_CTXA, base + 1 + i);
    vec4 hi = bfk_data(BFK_CTXA, base + 2 + i);
    vec4 r = bfk_mix4(lo, hi, f);
    return r;
}

// --- The interpreter ----------------------------------------------------------------------------------
// Operations (bloom/core/field_program.hpp, FieldOp):
//   0 Coordinate  1 Constant [value]  2 Noise  3 Voronoi  4 Random  5 Pattern  6 Gradient  7 Shape
//   8 Image  9 Force  10 Math (in0, in1, in2)  11 Remap  12 Transform (in0 coordinate, in1 warp)
//   13 Swizzle  14 Combine (in0..in3 x; an absent input is the parameter's lane)  15 Offset [dx, dy]
//   16 Derivative (in0..in3)  17 Cell Centre  18 Arrow (in0 p, in1 centre, in2 v)
//   19 Streak (in0 p, in1 v)  20 Ramp  21 Lens Warp (in0 p)  22 Test Card (in0 p)  23 Light Rays (in0 p)
// Every register write is limited to [-1e30, 1e30], so no later operation can see an infinity.

BFK_FN vec4 bfk_limit(vec4 v) {
    BFK_P float x = bfk_clamp(v.x, -1.0e30f, 1.0e30f);
    BFK_P float y = bfk_clamp(v.y, -1.0e30f, 1.0e30f);
    BFK_P float z = bfk_clamp(v.z, -1.0e30f, 1.0e30f);
    BFK_P float w = bfk_clamp(v.w, -1.0e30f, 1.0e30f);
    return vec4(x, y, z, w);
}

BFK_FN vec4 bfk_run(BFK_CTX, float px, float py) {
    vec4 head = bfk_data(BFK_CTXA, BFK_HEADER);
    int count = int(head.z);
    vec4 r[16];
    for (int k = 0; k < 16; ++k)
        r[k] = vec4(0.0f, 0.0f, 0.0f, 0.0f);
    vec4 zero = vec4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 64; ++i) {
        if (i >= count)
            break;
        vec4 a = bfk_data(BFK_CTXA, BFK_CODE + 2 * i);
        vec4 b = bfk_data(BFK_CTXA, BFK_CODE + 2 * i + 1);
        int op = int(a.x);
        int dst = int(a.y);
        int i0 = int(a.z);
        int i1 = int(a.w);
        int i2 = int(b.x);
        int i3 = int(b.y);
        int base = int(b.z);
        vec4 x0 = i0 >= 0 ? r[i0] : zero;
        vec4 x1 = i1 >= 0 ? r[i1] : zero;
        vec4 x2 = i2 >= 0 ? r[i2] : zero;
        vec4 x3 = i3 >= 0 ? r[i3] : zero;
        vec4 v = zero;
        switch (op) {
        case 0:
            v = vec4(px, py, 0.0f, 0.0f);
            break;
        case 1:
            v = bfk_data(BFK_CTXA, base);
            break;
        case 2:
            v = bfk_op_noise(BFK_CTXA, base, x0.x, x0.y);
            break;
        case 3:
            v = bfk_op_voronoi(BFK_CTXA, base, x0.x, x0.y);
            break;
        case 4:
            v = bfk_op_random(BFK_CTXA, base, x0.x, x0.y);
            break;
        case 5:
            v = bfk_op_pattern(BFK_CTXA, base, x0.x, x0.y);
            break;
        case 6:
            v = bfk_op_gradient(BFK_CTXA, base, x0.x, x0.y);
            break;
        case 7:
            v = bfk_op_shape(BFK_CTXA, base, x0.x, x0.y);
            break;
        case 8:
            v = bfk_op_image(BFK_CTXA, base, x0.x, x0.y);
            break;
        case 9:
            v = bfk_op_force(BFK_CTXA, base, x0.x, x0.y);
            break;
        case 10:
            v = bfk_op_math(BFK_CTXA, base, x0, x1, x2);
            break;
        case 11:
            v = bfk_op_remap(BFK_CTXA, base, x0);
            break;
        case 12:
            v = bfk_op_transform(BFK_CTXA, base, x0, x1, i1 >= 0);
            break;
        case 13:
            v = bfk_op_swizzle(BFK_CTXA, base, x0);
            break;
        case 14: {
            vec4 d = bfk_data(BFK_CTXA, base);
            BFK_P float cx = i0 >= 0 ? x0.x : d.x;
            BFK_P float cy = i1 >= 0 ? x1.x : d.y;
            BFK_P float cz = i2 >= 0 ? x2.x : d.z;
            BFK_P float cw = i3 >= 0 ? x3.x : d.w;
            v = vec4(cx, cy, cz, cw);
            break;
        }
        case 15: {
            vec4 d = bfk_data(BFK_CTXA, base);
            BFK_P float ox = x0.x + d.x;
            BFK_P float oy = x0.y + d.y;
            v = vec4(ox, oy, 0.0f, 0.0f);
            break;
        }
        case 16:
            v = bfk_op_derivative(BFK_CTXA, base, x0, x1, x2, x3);
            break;
        case 17:
            v = bfk_op_cell_centre(BFK_CTXA, base, x0);
            break;
        case 18:
            v = bfk_op_arrow(BFK_CTXA, base, x0, x1, x2);
            break;
        case 19:
            v = bfk_op_streak(BFK_CTXA, base, x0, x1);
            break;
        case 20:
            v = bfk_op_ramp(BFK_CTXA, base, x0);
            break;
        case 21:
            v = bfk_op_lens_warp(BFK_CTXA, base, x0);
            break;
        case 22:
            v = bfk_op_test_card(BFK_CTXA, base, x0.x, x0.y);
            break;
        case 23:
            v = bfk_op_rays(BFK_CTXA, base, x0);
            break;
        default:
            v = zero;
            break;
        }
        if (dst >= 0 && dst < 16)
            r[dst] = bfk_limit(v);
    }
    int result = int(head.w);
    return (result >= 0 && result < 16) ? r[result] : zero;
}

// One output pixel (i, j) of the target: the mean of the sub-samples, each converted to
// premultiplied RGBA by the output mode (0 Raw: a scalar grey [v, v, v, 1], a vector [x, y, 0, 1],
// a colour premultiplied; 1 Mask: coverage clamp(v, 0, 1) in every channel; 2 Colour: the straight
// colour premultiplied by its clamped alpha). Result kinds 0 Scalar, 1 Vector, 2 Colour.
BFK_FN vec4 bfk_pixel(BFK_CTX, int i, int j) {
    vec4 map = bfk_data(BFK_CTXA, BFK_MAPPING);
    vec4 sampling = bfk_data(BFK_CTXA, BFK_SAMPLING);
    int samples = int(sampling.x);
    int mode = int(sampling.z);
    int kind = int(sampling.w);
    vec4 sum = vec4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int s = 0; s < 16; ++s) {
        if (s >= samples)
            break;
        vec4 pair = bfk_data(BFK_CTXA, BFK_SAMPLES + s / 2);
        BFK_P float ox = (s & 1) == 0 ? pair.x : pair.z;
        BFK_P float oy = (s & 1) == 0 ? pair.y : pair.w;
        BFK_P float fi = float(i) + ox;
        BFK_P float fj = float(j) + oy;
        BFK_P float px = fma(fi, map.x, map.y);
        BFK_P float py = fma(fj, map.z, map.w);
        vec4 v = bfk_run(BFK_CTXA, px, py);
        vec4 c = vec4(0.0f, 0.0f, 0.0f, 0.0f);
        if (mode == 1) {
            BFK_P float k = bfk_clamp(v.x, 0.0f, 1.0f);
            c = vec4(k, k, k, k);
        } else if (kind == 3) {
            BFK_P float a = bfk_clamp(v.w, 0.0f, 1.0f);
            c = vec4(v.x, v.y, v.z, a);
        } else if (kind == 2) {
            BFK_P float a = bfk_clamp(v.w, 0.0f, 1.0f);
            BFK_P float cr = v.x * a;
            BFK_P float cg = v.y * a;
            BFK_P float cb = v.z * a;
            c = vec4(cr, cg, cb, a);
        } else if (kind == 1) {
            c = vec4(v.x, v.y, 0.0f, 1.0f);
        } else {
            c = vec4(v.x, v.x, v.x, 1.0f);
        }
        BFK_P float ax = sum.x + c.x;
        BFK_P float ay = sum.y + c.y;
        BFK_P float az = sum.z + c.z;
        BFK_P float aw = sum.w + c.w;
        sum = vec4(ax, ay, az, aw);
    }
    BFK_P float inv = samples > 1 ? bfk_div(1.0f, float(samples)) : 1.0f;
    BFK_P float rx = sum.x * inv;
    BFK_P float ry = sum.y * inv;
    BFK_P float rz = sum.z * inv;
    BFK_P float rw = sum.w * inv;
    BFK_P float a = bfk_clamp(rw, 0.0f, 1.0f);
    if (kind != 3 && !(a > 0.0f))
        return vec4(0.0f, 0.0f, 0.0f, 0.0f);
    return vec4(rx, ry, rz, a);
}

// A pixel (x, y) of the dispatched image: the target pixel it is, or transparent outside the target
// (the GPU dispatches over an image that also holds the image slots beside the target).
BFK_FN vec4 bfk_dispatch_pixel(BFK_CTX, int x, int y) {
    vec4 target = bfk_data(BFK_CTXA, BFK_TARGET);
    int i = x - int(target.x);
    int j = y - int(target.y);
    if (i < 0 || j < 0 || i >= int(target.z) || j >= int(target.w))
        return vec4(0.0f, 0.0f, 0.0f, 0.0f);
    return bfk_pixel(BFK_CTXA, i, j);
}
