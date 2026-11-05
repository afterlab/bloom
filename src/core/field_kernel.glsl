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
// 0, 0, 0], 1 [first, second, 0, 0], 2 the straight colour [r, g, b, a].

BFK_FN vec4 bfk_op_image(BFK_CTX, int base, float px, float py) {
    vec4 h0 = bfk_data(BFK_CTXA, base);
    vec4 h1 = bfk_data(BFK_CTXA, base + 1);
    vec4 t = bfk_sample(BFK_CTXA, int(h0.x), px, py, int(h0.y), int(h0.z));
    int selector = int(h0.w);
    if (selector == 0)
        return vec4(bfk_channel(t, int(h1.x)), 0.0f, 0.0f, 0.0f);
    if (selector == 1)
        return vec4(bfk_channel(t, int(h1.y)), bfk_channel(t, int(h1.z)), 0.0f, 0.0f);
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
//   19 Streak (in0 p, in1 v)  20 Ramp
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
    if (!(a > 0.0f))
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
