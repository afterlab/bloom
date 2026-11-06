// #484 Motion blur: the one motion kernel. This exact text is compiled twice: as C++ by
// src/runtime/motion_blur_kernel.cpp (the CPU reference) and as GLSL 4.60 by the GPU program the
// runtime builds around it (motionKernelGlsl()). Each host defines the dialect first:
//   BMK_FN          a function's qualifiers ("inline" in C++, nothing in GLSL)
//   BMK_P           GLSL `precise` (nothing in C++, built without contraction or fast math)
//   BMK_OUT(T)      an out parameter; BMK_INOUT(T) an inout one
//   vec2/vec4       the vector types with GLSL's componentwise + - * and constructors
//   bmk_data(i)     the i-th vec4 of the pass table
//   bmk_texel(x, y) a stored texel of the pass atlas (zero outside it)
//   bmk_div(a, b)   correctly rounded binary32 division
//   bmk_bits(f)     the bits of a float (uint); bmk_float(u) the float of bits
//   BMK_IS_PASS(p)  whether the dispatch runs pass p: the table's pass in C++, a constant in GLSL
//                   (each pass is its own GPU program, so a device compiles only what a frame
//                   uses: the whole kernel as one program took 13 s on a cold driver cache)
// The dialect is a common subset: binary32 arithmetic, fma, floor, sqrt, abs, min, max, clamp,
// integer and bit operations. Every intermediate that could be fused or reordered is a precise
// variable. sqrt goes through bmk_sqrt, which corrects the device's root to the correctly rounded
// one, and exp through Bloom's own polynomial, so the CPU and every device evaluate the same
// binary32 operations. docs/architecture/motion-blur.md is the specification of every pass.
//
// Every pass runs over one atlas: the images it reads placed side by side as regions. Table
// vec4 0 is [pass, active x, active y, active width], vec4 1 [active height, 0, 0, 0]; a pixel
// outside the active rectangle is copied through. Regions are vec4 2.. 15 as [x, y, width,
// height] in atlas pixels; each pass's own parameters start at vec4 16. A result pixel must have
// alpha in [0, 1] (data regions keep alpha 1), and a result with alpha 0 is stored as zero, as the
// GPU wrapper stores it.

const int BMK_PASS_ACCUMULATE = 1;
const int BMK_PASS_PATH = 2;
const int BMK_PASS_TILE = 3;
const int BMK_PASS_GATHER = 4;
const int BMK_PASS_SPLAT = 5;
const int BMK_PASS_JFA_INIT = 6;
const int BMK_PASS_JFA_STEP = 7;
const int BMK_PASS_FILL = 8;
const int BMK_PASS_CONSISTENCY = 9;
const int BMK_PASS_GREY = 10;
const int BMK_PASS_DECIMATE = 11;
const int BMK_PASS_TV_INIT = 12;
const int BMK_PASS_TV_WARP = 13;
const int BMK_PASS_TV_ITERATE = 14;
const int BMK_PASS_FLOW_OUT = 15;
const int BMK_PASS_DIS_SEARCH = 16;
const int BMK_PASS_DIS_DENSIFY = 17;
const int BMK_PASS_DIS_DATA = 18;
const int BMK_PASS_DIS_ITERATE = 19;
const int BMK_PASS_CUT_RESIDUAL = 20;
const int BMK_PASS_CUT_REDUCE = 21;
const int BMK_PASS_CUT_BROADCAST = 22;
const int BMK_PASS_TEMPORAL_FILL = 23;
const int BMK_PASS_DIS_INIT = 24;
const int BMK_PASS_DIS_UPDATE = 25;
const int BMK_PASS_NEIGHBOUR = 26;

const int BMK_PARAMS = 16;

// --- Elementary functions ------------------------------------------------------------------------

BMK_FN float bmk_clampf(float v, float lo, float hi) {
    BMK_P float r = min(max(v, lo), hi);
    return r;
}

BMK_FN int bmk_clampi(int v, int lo, int hi) { return min(max(v, lo), hi); }

// The correctly rounded square root: the device's root, then whichever of it and its two
// neighbours squares closest to v (fma gives each residual with one rounding).
BMK_FN float bmk_sqrt(float v) {
    BMK_P float x = max(v, 0.0f);
    BMK_P float s = sqrt(x);
    if (!(s > 0.0f) || s > 3.0e38f)
        return s;
    BMK_P float lo = bmk_float(bmk_bits(s) - 1u);
    BMK_P float hi = bmk_float(bmk_bits(s) + 1u);
    BMK_P float rs = abs(fma(s, s, -x));
    BMK_P float rl = abs(fma(lo, lo, -x));
    BMK_P float rh = abs(fma(hi, hi, -x));
    if (rl < rs)
        return lo;
    if (rh < rs)
        return hi;
    return s;
}

// 2^x for x in [-126, 126] (clamped): 2^floor(x) times a degree-9 polynomial of the fraction, the
// field kernel's polynomial (docs/architecture/fields.md).
BMK_FN float bmk_exp2(float x) {
    BMK_P float c = bmk_clampf(x, -126.0f, 126.0f);
    BMK_P float whole = floor(c);
    BMK_P float f = c - whole;
    BMK_P float p = 1.01780863e-07f;
    p = fma(p, f, 1.32154867e-06f);
    p = fma(p, f, 1.52527336e-05f);
    p = fma(p, f, 0.000154035297f);
    p = fma(p, f, 0.00133335579f);
    p = fma(p, f, 0.00961812865f);
    p = fma(p, f, 0.0555041097f);
    p = fma(p, f, 0.240226507f);
    p = fma(p, f, 0.693147182f);
    p = fma(p, f, 1.0f);
    int e = int(whole);
    // 2^e as bits: the biased exponent in place (e is within [-126, 126]).
    BMK_P float scale = bmk_float(uint(e + 127) << 23);
    BMK_P float r = p * scale;
    return r;
}

BMK_FN float bmk_exp(float x) {
    BMK_P float t = x * 1.44269502f;
    return bmk_exp2(t);
}

BMK_FN vec4 bmk_lerp4(vec4 a, vec4 b, float t) {
    BMK_P vec4 d = b - a;
    BMK_P vec4 r = fma(d, vec4(t), a);
    return r;
}

BMK_FN float bmk_length2(float x, float y) {
    BMK_P float xx = x * x;
    BMK_P float s = fma(y, y, xx);
    return s;
}

BMK_FN float bmk_length(float x, float y) { return bmk_sqrt(bmk_length2(x, y)); }

// Rec. 709 luma of associated colour.
BMK_FN float bmk_luma(vec4 p) {
    BMK_P float a = 0.2126f * p.x;
    BMK_P float b = fma(0.7152f, p.y, a);
    BMK_P float c = fma(0.0722f, p.z, b);
    return c;
}

// --- Integer hash (the seeded jitter) -----------------------------------------------------------

// Wellons' lowbias32 integer hash.
BMK_FN uint bmk_hash(uint v) {
    uint x = v;
    x = x ^ (x >> 16);
    x = x * 0x7feb352du;
    x = x ^ (x >> 15);
    x = x * 0x846ca68bu;
    x = x ^ (x >> 16);
    return x;
}

// A uniform value in [0, 1) from the node's seed, the frame and the global pixel.
BMK_FN float bmk_jitter(float seed, float frame, int gx, int gy) {
    uint h = bmk_hash(uint(int(seed)) ^ bmk_hash(uint(int(frame)) ^
                                                bmk_hash(uint(gx) ^ bmk_hash(uint(gy) + 0x9e3779b9u))));
    BMK_P float j = float(h >> 8) * 5.96046448e-08f;
    return j;
}

// --- The table and the atlas --------------------------------------------------------------------

BMK_FN int bmk_int(float v) { return int(floor(v + 0.5f)); }

BMK_FN vec4 bmk_param(int i) { return bmk_data(BMK_PARAMS + i); }

BMK_FN vec4 bmk_region(int r) { return bmk_data(2 + r); }

BMK_FN bool bmk_has(int r) { return bmk_region(r).z > 0.0f; }

BMK_FN int bmk_wrap(int x, int w) {
    int v = x;
    if (v < 0)
        v = v + w * ((w - 1 - v) / w);
    return v - w * (v / w);
}

// A texel of region r at local (x, y). Border 0 is transparent outside, 1 extends the edge, 2
// wraps horizontally (a 360 frame) and extends vertically.
BMK_FN vec4 bmk_fetch(int r, int x, int y, int border) {
    vec4 d = bmk_region(r);
    int w = int(d.z);
    int h = int(d.w);
    if (w <= 0 || h <= 0)
        return vec4(0.0f);
    int lx = x;
    int ly = y;
    if (border == 0) {
        if (lx < 0 || ly < 0 || lx >= w || ly >= h)
            return vec4(0.0f);
    } else if (border == 1) {
        lx = bmk_clampi(lx, 0, w - 1);
        ly = bmk_clampi(ly, 0, h - 1);
    } else {
        lx = bmk_wrap(lx, w);
        ly = bmk_clampi(ly, 0, h - 1);
    }
    return bmk_texel(int(d.x) + lx, int(d.y) + ly);
}

// Bilinear at local (x, y), texel centres at integers.
BMK_FN vec4 bmk_bilinear(int r, float x, float y, int border) {
    BMK_P float cx = bmk_clampf(x, -16777216.0f, 16777216.0f);
    BMK_P float cy = bmk_clampf(y, -16777216.0f, 16777216.0f);
    BMK_P float fx = floor(cx);
    BMK_P float fy = floor(cy);
    BMK_P float tx = cx - fx;
    BMK_P float ty = cy - fy;
    int ix = int(fx);
    int iy = int(fy);
    vec4 a = bmk_fetch(r, ix, iy, border);
    vec4 b = bmk_fetch(r, ix + 1, iy, border);
    vec4 c = bmk_fetch(r, ix, iy + 1, border);
    vec4 d = bmk_fetch(r, ix + 1, iy + 1, border);
    BMK_P vec4 top = bmk_lerp4(a, b, tx);
    BMK_P vec4 bottom = bmk_lerp4(c, d, tx);
    return bmk_lerp4(top, bottom, ty);
}

BMK_FN vec4 bmk_nearest(int r, float x, float y, int border) {
    BMK_P float cx = bmk_clampf(x, -16777216.0f, 16777216.0f);
    BMK_P float cy = bmk_clampf(y, -16777216.0f, 16777216.0f);
    BMK_P float rx = floor(cx + 0.5f);
    BMK_P float ry = floor(cy + 0.5f);
    return bmk_fetch(r, int(rx), int(ry), border);
}

// --- Accumulate (Motion Blur sub-frames, Echo) -----------------------------------------------------

// Regions 0 (the accumulator, active) and 1 (the sample, same size); [operator, accumulator
// weight, sample weight, 0]. Operators: 0 add, 1 maximum, 2 minimum, 3 screen, 4 sample over the
// accumulator, 5 sample under it.
BMK_FN vec4 bmk_accumulate(int lx, int ly) {
    vec4 q = bmk_param(0);
    int op = int(q.x);
    BMK_P vec4 a = bmk_fetch(0, lx, ly, 0) * q.y;
    BMK_P vec4 s = bmk_fetch(1, lx, ly, 0) * q.z;
    BMK_P vec4 r = a + s;
    if (op == 1) {
        r = max(a, s);
    } else if (op == 2) {
        r = min(a, s);
    } else if (op == 3) {
        BMK_P vec4 m = a * s;
        r = (a + s) - m;
    } else if (op == 4) {
        BMK_P float k = 1.0f - s.w;
        BMK_P vec4 m = a * k;
        r = s + m;
    } else if (op == 5) {
        BMK_P float k = 1.0f - a.w;
        BMK_P vec4 m = s * k;
        r = a + m;
    }
    r.w = bmk_clampf(r.w, 0.0f, 1.0f);
    return r;
}

// --- The path sampler (Directional, Spin, Zoom, Transform 2D) -----------------------------------

// Regions 0 (the output, active) and 1 (the input). [count, jitter, filter, 0], [seed, frame,
// output global x, output global y]; sample k at vec4 20 + 4k: [A row 0, weight], [A row 1, 0],
// [B row 0, 0], [B row 1, 0]. A and B map an output-local texel index to input-local coordinates
// at the start and end of the sample's stratum; without jitter both are its centre's map.
BMK_FN vec4 bmk_path(int lx, int ly) {
    vec4 h = bmk_param(0);
    vec4 g = bmk_param(1);
    int count = int(h.x);
    bool jitter = h.y != 0.0f;
    bool nearest = h.z != 0.0f;
    BMK_P float j = 0.5f;
    if (jitter)
        j = bmk_jitter(g.x, g.y, lx + int(g.z), ly + int(g.w));
    BMK_P float px = float(lx);
    BMK_P float py = float(ly);
    BMK_P vec4 sum = vec4(0.0f);
    for (int k = 0; k < count; ++k) {
        int base = BMK_PARAMS + 4 + 4 * k;
        vec4 a0 = bmk_data(base);
        vec4 a1 = bmk_data(base + 1);
        vec4 b0 = bmk_data(base + 2);
        vec4 b1 = bmk_data(base + 3);
        BMK_P float ax = fma(a0.x, px, fma(a0.y, py, a0.z));
        BMK_P float ay = fma(a1.x, px, fma(a1.y, py, a1.z));
        BMK_P float bx = fma(b0.x, px, fma(b0.y, py, b0.z));
        BMK_P float by = fma(b1.x, px, fma(b1.y, py, b1.z));
        BMK_P float qx = fma(bx - ax, j, ax);
        BMK_P float qy = fma(by - ay, j, ay);
        vec4 s = nearest ? bmk_nearest(1, qx, qy, 0) : bmk_bilinear(1, qx, qy, 0);
        sum = fma(s, vec4(a0.w), sum);
    }
    sum.w = bmk_clampf(sum.w, 0.0f, 1.0f);
    return sum;
}

// --- Motion vectors -------------------------------------------------------------------------------

// The decode block at parameters d, d + 1, d + 2: [scale, offset, flip y, arrangement], [units x,
// units y, alpha is validity, widest extent], [shutter open t0, shutter close t1, curved, 0].
// Layout 0: one vector (RG of F), the path linear; 1: F's RG to the next frame and B's RG to the
// previous; 2: F's RG the motion from the previous frame and B's RG the motion to the next one
// (Blender's order). The result is [forward x, forward y, backward x, backward y] in evaluation
// pixels with y down (forward: the displacement to the next frame; backward: to the previous),
// zero for an invalid or non-finite vector, scaled so its blur extent is at most the widest.
BMK_FN float bmk_decode1(float v, float scale, float offset, float units) {
    BMK_P float a = fma(v, scale, offset);
    BMK_P float b = a * units;
    return b;
}

// The displacement at shutter time t (frames) of a pixel with forward f and backward b.
BMK_FN vec2 bmk_displace(vec4 v, float t, bool curved) {
    if (curved) {
        // The quadratic through b (t = -1), 0 and f (t = 1): t (f - b) / 2 + t^2 (f + b) / 2.
        BMK_P float vx = (v.x - v.z) * 0.5f;
        BMK_P float vy = (v.y - v.w) * 0.5f;
        BMK_P float ax = (v.x + v.z) * 0.5f;
        BMK_P float ay = (v.y + v.w) * 0.5f;
        BMK_P float tt = t * t;
        BMK_P float dx = fma(ax, tt, vx * t);
        BMK_P float dy = fma(ay, tt, vy * t);
        return vec2(dx, dy);
    }
    if (t >= 0.0f) {
        BMK_P float dx = v.x * t;
        BMK_P float dy = v.y * t;
        return vec2(dx, dy);
    }
    BMK_P float u = -t;
    BMK_P float dx = v.z * u;
    BMK_P float dy = v.w * u;
    return vec2(dx, dy);
}

// The longest displacement over the shutter (its ends, and the vertex of a curved path).
BMK_FN float bmk_extent(vec4 v, float t0, float t1, bool curved) {
    vec2 a = bmk_displace(v, t0, curved);
    vec2 b = bmk_displace(v, t1, curved);
    BMK_P float e = max(bmk_length(a.x, a.y), bmk_length(b.x, b.y));
    if (curved) {
        vec2 m = bmk_displace(v, (t0 + t1) * 0.5f, curved);
        e = max(e, bmk_length(m.x, m.y));
    } else if (t0 < 0.0f && t1 > 0.0f) {
        e = max(e, 0.0f);
    }
    return e;
}

BMK_FN vec4 bmk_decode(int d, vec4 f, vec4 b) {
    vec4 p0 = bmk_param(d);
    vec4 p1 = bmk_param(d + 1);
    vec4 p2 = bmk_param(d + 2);
    int arrangement = int(p0.w);
    BMK_P float sy = p0.z != 0.0f ? -p1.y : p1.y;
    BMK_P float fx = bmk_decode1(f.x, p0.x, p0.y, p1.x);
    BMK_P float fy = bmk_decode1(f.y, p0.x, p0.y, sy);
    BMK_P float gx = bmk_decode1(b.x, p0.x, p0.y, p1.x);
    BMK_P float gy = bmk_decode1(b.y, p0.x, p0.y, sy);
    // A valid vector's coverage is full on (RE:Vision's rule); the third channel carries it.
    bool valid = p1.z == 0.0f || f.z >= 1.0f;
    BMK_P vec4 v = vec4(fx, fy, -fx, -fy);
    if (arrangement == 1)
        v = vec4(fx, fy, gx, gy);
    else if (arrangement == 2)
        v = vec4(gx, gy, -fx, -fy);
    if (!valid || isnan(v.x) || isnan(v.y) || isnan(v.z) || isnan(v.w) || isinf(v.x) ||
        isinf(v.y) || isinf(v.z) || isinf(v.w))
        return vec4(0.0f);
    BMK_P float e = bmk_extent(v, p2.x, p2.y, p2.z != 0.0f);
    if (e > p1.w && e > 0.0f) {
        BMK_P float k = bmk_div(p1.w, e);
        v = v * k;
    }
    return v;
}

// The decoded vectors at global pixel (gx, gy). Region rf holds F and rb B (absent: none), each
// placed with its global origin at parameters o (F) and o + 1 (B) as [x, y, 0, 0].
BMK_FN vec4 bmk_vectors_at(int rf, int rb, int o, int d, int gx, int gy) {
    vec4 of = bmk_param(o);
    vec4 ob = bmk_param(o + 1);
    vec4 f = bmk_fetch(rf, gx - int(of.x), gy - int(of.y), 0);
    vec4 b = bmk_has(rb) ? bmk_fetch(rb, gx - int(ob.x), gy - int(ob.y), 0) : vec4(0.0f);
    return bmk_decode(d, f, b);
}

BMK_FN vec4 bmk_vectors_bilinear(int rf, int rb, int o, int d, float x, float y) {
    vec4 of = bmk_param(o);
    vec4 ob = bmk_param(o + 1);
    vec4 f = bmk_bilinear(rf, x - of.x, y - of.y, 0);
    vec4 b = bmk_has(rb) ? bmk_bilinear(rb, x - ob.x, y - ob.y, 0) : vec4(0.0f);
    return bmk_decode(d, f, b);
}

// Tile maximum. Regions 0 (the tiles, active), 1 (F), 2 (B). [tile size K, 0, 0, 0], the decode
// block at 1, the origins at 4 (F) and 5 (B), [tile grid global x, y, 0, 0] at 6. A tile holds the
// global position of the pixel whose blur reaches farthest inside it and that extent: [x, y,
// extent, 1] (McGuire et al. 2012's TileMax, carrying the pixel instead of its velocity).
BMK_FN vec4 bmk_tile(int lx, int ly) {
    int k = int(bmk_param(0).x);
    vec4 grid = bmk_param(6);
    vec4 p2 = bmk_param(3);
    bool curved = p2.z != 0.0f;
    int x0 = int(grid.x) + lx * k;
    int y0 = int(grid.y) + ly * k;
    BMK_P float best = -1.0f;
    int bx = x0;
    int by = y0;
    for (int j = 0; j < k; ++j) {
        for (int i = 0; i < k; ++i) {
            vec4 v = bmk_vectors_at(1, 2, 4, 1, x0 + i, y0 + j);
            BMK_P float e = bmk_extent(v, p2.x, p2.y, curved);
            if (e > best) {
                best = e;
                bx = x0 + i;
                by = y0 + j;
            }
        }
    }
    return vec4(float(bx), float(by), max(best, 0.0f), 1.0f);
}

// NeighborMax (McGuire et al. 2012; Guertin et al. 2014). Regions 0 (the tiles, active), 1 (the tile
// maxima). [tile size K, radius in tiles, grid width, grid height], [grid global x, y, 0, 0]. A tile
// holds the farthest-reaching dominant pixel among the tiles around it whose blur actually reaches
// it, so a wide Maximum blur costs no quality where the vectors are short. [x, y, extent, 1], or
// w = 0 where nothing moves here.
BMK_FN vec4 bmk_neighbour(int lx, int ly) {
    vec4 p = bmk_param(0);
    vec4 grid = bmk_param(1);
    int k = int(p.x);
    int radius = int(p.y);
    int gw = int(p.z);
    int gh = int(p.w);
    int x0 = int(grid.x) + lx * k;
    int y0 = int(grid.y) + ly * k;
    int x1 = x0 + k - 1;
    int y1 = y0 + k - 1;
    vec4 best = vec4(float(x0), float(y0), 0.0f, 0.0f);
    int jl = max(-radius, -ly);
    int jh = min(radius, gh - 1 - ly);
    int il = max(-radius, -lx);
    int ih = min(radius, gw - 1 - lx);
    for (int j = jl; j <= jh; ++j)
        for (int i = il; i <= ih; ++i) {
            vec4 t = bmk_fetch(1, lx + i, ly + j, 0);
            if (t.w <= 0.0f || t.z <= best.z)
                continue;
            BMK_P float dx = max(max(float(x0) - t.x, t.x - float(x1)), 0.0f);
            BMK_P float dy = max(max(float(y0) - t.y, t.y - float(y1)), 0.0f);
            if (bmk_length(dx, dy) <= t.z + 1.0f)
                best = t;
        }
    return best;
}

// The dominant pixel over global pixel (gx, gy): its tile's NeighborMax. [x, y, extent].
BMK_FN vec4 bmk_neighbour_max(int rt, int k, vec4 grid, int gx, int gy) {
    int tx = (gx - int(grid.x)) >= 0 ? (gx - int(grid.x)) / k : -((int(grid.x) - gx + k - 1) / k);
    int ty = (gy - int(grid.y)) >= 0 ? (gy - int(grid.y)) / k : -((int(grid.y) - gy + k - 1) / k);
    vec4 t = bmk_fetch(rt, tx, ty, 0);
    if (t.w > 0.0f && t.z > 0.0f)
        return t;
    return vec4(float(gx), float(gy), 0.0f, 0.0f);
}

BMK_FN float bmk_cone(float dist, float extent) {
    BMK_P float r = 1.0f - bmk_div(dist, max(extent, 0.5f));
    return bmk_clampf(r, 0.0f, 1.0f);
}

BMK_FN float bmk_cylinder(float dist, float extent) {
    BMK_P float e = max(extent, 0.5f);
    BMK_P float lo = 0.95f * e;
    BMK_P float hi = 1.05f * e;
    BMK_P float t = bmk_clampf(bmk_div(dist - lo, hi - lo), 0.0f, 1.0f);
    BMK_P float s = t * t * (3.0f - 2.0f * t);
    return 1.0f - s;
}

// McGuire et al. 2012's softDepthCompare for distances (nearer is smaller): 1 when b is in front
// of a, falling to 0 over `soft` scene units behind it.
BMK_FN float bmk_depth_compare(float a, float b, float soft) {
    BMK_P float r = 1.0f - bmk_div(b - a, soft);
    return bmk_clampf(r, 0.0f, 1.0f);
}

BMK_FN float bmk_depth_at(int rd, vec4 od, int channel, int gx, int gy) {
    if (!bmk_has(rd))
        return 0.0f;
    vec4 p = bmk_fetch(rd, gx - int(od.x), gy - int(od.y), 1);
    BMK_P float z = channel == 0 ? p.x : channel == 1 ? p.y : channel == 2 ? p.z : p.w;
    if (isnan(z) || isinf(z))
        return 0.0f;
    return z;
}

BMK_FN float bmk_weight(int base, int i) {
    vec4 w = bmk_data(base + i / 4);
    int c = i - 4 * (i / 4);
    return c == 0 ? w.x : c == 1 ? w.y : c == 2 ? w.z : w.w;
}

// The reconstruction filter. Regions 0 (output, active), 1 (colour), 2 (F), 3 (B), 4 (tiles), 5
// (depth). [samples, jitter, tile size, soft depth extent], the decode block at 1, [output global
// x, y, colour global x, y] at 4, the F and B origins at 5 and 6, [depth global x, y, depth
// channel, 0] at 7, [tile grid global x, y, seed, frame] at 8; the shutter curve's per-sample
// weights from vec4 48.
BMK_FN vec4 bmk_gather(int lx, int ly) {
    vec4 h = bmk_param(0);
    vec4 p2 = bmk_param(3);
    vec4 o = bmk_param(4);
    vec4 od = bmk_param(7);
    vec4 grid = bmk_param(8);
    int count = int(h.x);
    int k = int(h.z);
    bool curved = p2.z != 0.0f;
    int gx = lx + int(o.x);
    int gy = ly + int(o.y);
    int cx = int(o.z);
    int cy = int(o.w);
    vec4 cp = bmk_fetch(1, gx - cx, gy - cy, 0);
    vec4 n = bmk_neighbour_max(4, k, grid, gx, gy);
    if (n.z < 0.5f)
        return cp;
    vec4 vn = bmk_vectors_at(2, 3, 5, 1, int(n.x), int(n.y));
    vec4 vp = bmk_vectors_at(2, 3, 5, 1, gx, gy);
    BMK_P float ep = max(bmk_extent(vp, p2.x, p2.y, curved), 0.5f);
    bool own = ep >= 1.0f;
    int channel = int(od.z);
    BMK_P float zp = bmk_depth_at(5, od, channel, gx, gy);
    BMK_P float weight = bmk_div(1.0f, ep);
    BMK_P vec4 sum = cp * weight;
    BMK_P float j = 0.5f;
    if (h.y != 0.0f)
        j = bmk_jitter(grid.z, grid.w, gx, gy);
    for (int i = 0; i < count; ++i) {
        BMK_P float u = bmk_div(float(i) + j, float(count));
        BMK_P float t = fma(p2.y - p2.x, u, p2.x);
        // Guertin et al. 2014: alternate samples follow the pixel's own vector, so a pixel with its
        // own motion keeps it beside a faster neighbour.
        vec2 dn = bmk_displace((i & 1) == 1 && own ? vp : vn, t, curved);
        BMK_P float yx = float(gx) - dn.x;
        BMK_P float yy = float(gy) - dn.y;
        int ix = int(floor(yx + 0.5f));
        int iy = int(floor(yy + 0.5f));
        vec4 cy4 = bmk_fetch(1, ix - cx, iy - cy, 0);
        vec4 vy = bmk_vectors_at(2, 3, 5, 1, ix, iy);
        BMK_P float ey = bmk_extent(vy, p2.x, p2.y, curved);
        BMK_P float zy = bmk_depth_at(5, od, channel, ix, iy);
        BMK_P float dist = bmk_length(float(ix - gx), float(iy - gy));
        BMK_P float f = bmk_depth_compare(zp, zy, h.w);
        BMK_P float b = bmk_depth_compare(zy, zp, h.w);
        BMK_P float c0 = f * bmk_cone(dist, ey);
        BMK_P float c1 = b * bmk_cone(dist, ep);
        BMK_P float c2 = bmk_cylinder(dist, ey) * bmk_cylinder(dist, ep) * 2.0f;
        BMK_P float a = (c0 + c1 + c2) * bmk_weight(BMK_PARAMS + 32, i);
        weight = weight + a;
        sum = fma(cy4, vec4(a), sum);
    }
    BMK_P vec4 r = sum * bmk_div(1.0f, weight);
    r.w = bmk_clampf(r.w, 0.0f, 1.0f);
    return r;
}

// --- The soft z-ordered splat ---------------------------------------------------------------------

// One source of frame `frame` (0 the current, 1 the next, 2 the previous) reaching global pixel
// (px, py) at shutter time t: the fixed point q of q = p - D(q) from `seed`, D the source's
// displacement, accepted when it lands within the tent of p. Returns [weight, q x, q y, 0].
BMK_FN vec4 bmk_splat_source(int frame, float px, float py, float sx, float sy, float t,
                             int iterations) {
    vec4 p2 = bmk_param(3);
    bool curved = p2.z != 0.0f;
    BMK_P float qx = sx;
    BMK_P float qy = sy;
    BMK_P float dx = 0.0f;
    BMK_P float dy = 0.0f;
    for (int it = 0; it <= iterations; ++it) {
        if (frame == 0) {
            vec4 v = bmk_vectors_bilinear(2, 3, 5, 1, qx, qy);
            vec2 d = bmk_displace(v, t, curved);
            dx = d.x;
            dy = d.y;
        } else {
            // A neighbour frame's flow to the current frame, scaled to the part of the frame step
            // left: (1 - |t|).
            int rr = frame == 1 ? 8 : 10;
            vec4 orr = bmk_param(frame == 1 ? 13 : 14);
            vec4 r = bmk_bilinear(rr, qx - orr.x, qy - orr.y, 1);
            BMK_P float k = 1.0f - abs(t);
            dx = r.x * k;
            dy = r.y * k;
        }
        if (it < iterations) {
            qx = px - dx;
            qy = py - dy;
        }
    }
    BMK_P float rx = abs((qx + dx) - px);
    BMK_P float ry = abs((qy + dy) - py);
    BMK_P float w = max(1.0f - rx, 0.0f) * max(1.0f - ry, 0.0f);
    return vec4(w, qx, qy, 0.0f);
}

// Regions 0 (output, active), 1 (colour), 2 (F), 3 (B), 4 (tiles), 5 (depth), 6 (matte), 7 (next
// colour), 8 (next frame's flow to this one), 9 (previous colour), 10 (previous frame's flow to
// this one), 11 (consistency of F in R and of B in G), 12 (consistency of the next frame's flow),
// 13 (of the previous frame's flow). [samples, jitter, tile size, soft z], the decode block at 1,
// origins as [x, y, 0, 0]: output 4, F 5, B 6, colour 7, depth 8, matte 9, next colour 10, previous
// colour 11, consistency 12, next flow 13, previous flow 14, next consistency 15 (x, y) and
// previous consistency 15 (z, w); [layer (0 all, 1 foreground, 2 background), z (0 none, 1 depth,
// 2 brightness constancy), brightness scale, border] at 16, [seed, frame, depth channel, fixed
// point iterations] at 17, [tile grid x, y, 0, 0] at 18; per-sample weights from vec4 48.
// Softmax splatting (Niklaus and Liu 2020) evaluated as a gather; each neighbour frame's warp is
// blended by its visibility as Super SloMo (Jiang et al. 2018) blends them.
BMK_FN vec4 bmk_splat(int lx, int ly) {
    vec4 h = bmk_param(0);
    vec4 p2 = bmk_param(3);
    vec4 o = bmk_param(4);
    vec4 oc = bmk_param(7);
    vec4 od = bmk_param(8);
    vec4 om = bmk_param(9);
    vec4 on = bmk_param(10);
    vec4 op = bmk_param(11);
    vec4 oq = bmk_param(12);
    vec4 oqn = bmk_param(15);
    vec4 mode = bmk_param(16);
    vec4 s17 = bmk_param(17);
    vec4 grid = bmk_param(18);
    int count = int(h.x);
    int k = int(h.z);
    bool curved = p2.z != 0.0f;
    int layer = int(mode.x);
    int zmode = int(mode.y);
    int border = int(mode.w);
    int iterations = int(s17.w);
    int channel = int(s17.z);
    int gx = lx + int(o.x);
    int gy = ly + int(o.y);
    BMK_P float px = float(gx);
    BMK_P float py = float(gy);
    vec4 n = bmk_neighbour_max(4, k, grid, gx, gy);
    vec4 vn = bmk_vectors_at(2, 3, 5, 1, int(n.x), int(n.y));
    vec4 vp = bmk_vectors_at(2, 3, 5, 1, gx, gy);
    BMK_P float zp = bmk_depth_at(5, od, channel, gx, gy);
    bool flow = bmk_has(7) || bmk_has(9);
    BMK_P float j = 0.5f;
    if (h.y != 0.0f)
        j = bmk_jitter(s17.x, s17.y, gx, gy);
    BMK_P vec4 sum = vec4(0.0f);
    BMK_P float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        BMK_P float u = bmk_div(float(i) + j, float(count));
        BMK_P float t = fma(p2.y - p2.x, u, p2.x);
        BMK_P float ws = bmk_weight(BMK_PARAMS + 32, i);
        // Frame 0, then (with neighbour frames) the frame on t's side.
        for (int frame = 0; frame < 2; ++frame) {
            int which = frame == 0 ? 0 : (t >= 0.0f ? 1 : 2);
            if (frame == 1 && (!flow || !bmk_has(which == 1 ? 7 : 9)))
                continue;
            // Super SloMo's temporal weight: (1 - |t|) for this frame, |t| for the neighbour.
            BMK_P float blend = flow ? (frame == 0 ? 1.0f - abs(t) : abs(t)) : 1.0f;
            if (!(blend > 0.0f))
                continue;
            // Seeds: this pixel's own motion and the neighbourhood's dominant motion.
            vec2 d0 = bmk_displace(vp, t, curved);
            vec2 d1 = bmk_displace(vn, t, curved);
            BMK_P float s0x = px - d0.x;
            BMK_P float s0y = py - d0.y;
            BMK_P float s1x = px - d1.x;
            BMK_P float s1y = py - d1.y;
            if (which != 0) {
                // A neighbour frame's pixel moves back by its own flow: seed with the opposite of
                // this frame's motion.
                BMK_P float k1 = 1.0f - abs(t);
                vec2 e0 = bmk_displace(vp, which == 1 ? 1.0f : -1.0f, curved);
                vec2 e1 = bmk_displace(vn, which == 1 ? 1.0f : -1.0f, curved);
                s0x = fma(e0.x, k1, px);
                s0y = fma(e0.y, k1, py);
                s1x = fma(e1.x, k1, px);
                s1y = fma(e1.y, k1, py);
            }
            vec4 r0 = bmk_splat_source(which, px, py, s0x, s0y, t, iterations);
            vec4 r1 = bmk_splat_source(which, px, py, s1x, s1y, t, iterations);
            // Two seeds converging to one source count it once.
            if (abs(r1.y - r0.y) < 0.5f && abs(r1.z - r0.z) < 0.5f)
                r1.x = 0.0f;
            for (int s = 0; s < 2; ++s) {
                vec4 r = s == 0 ? r0 : r1;
                if (!(r.x > 0.0f))
                    continue;
                int rc = which == 0 ? 1 : (which == 1 ? 7 : 9);
                vec4 orc = which == 0 ? oc : (which == 1 ? on : op);
                vec4 c = bmk_bilinear(rc, r.y - orc.x, r.z - orc.y, border);
                BMK_P float w = r.x * blend;
                if (bmk_has(6) && which == 0) {
                    BMK_P float m = bmk_bilinear(6, r.y - om.x, r.z - om.y, 1).w;
                    if (layer == 1)
                        w = w * m;
                    else if (layer == 2)
                        w = w * (1.0f - m);
                }
                // Visibility: the consistency of the flow the source moved by.
                int rq = which == 0 ? 11 : (which == 1 ? 12 : 13);
                if (bmk_has(rq)) {
                    BMK_P float qxo = which == 0 ? oq.x : (which == 1 ? oqn.x : oqn.z);
                    BMK_P float qyo = which == 0 ? oq.y : (which == 1 ? oqn.y : oqn.w);
                    vec4 q = bmk_bilinear(rq, r.y - qxo, r.z - qyo, 1);
                    BMK_P float vis = which == 0 ? (t >= 0.0f ? q.x : q.y) : q.x;
                    w = w * max(vis, 0.0625f);
                }
                // Soft z-ordering: exp(Z) of the importance metric.
                if (zmode == 1 && which == 0) {
                    BMK_P float zq =
                        bmk_depth_at(5, od, channel, int(floor(r.y + 0.5f)), int(floor(r.z + 0.5f)));
                    BMK_P float z = bmk_clampf(bmk_div(zp - zq, h.w), -60.0f, 60.0f);
                    w = w * bmk_exp(z);
                } else if (zmode == 2 && which == 0 && bmk_has(7)) {
                    // Brightness constancy (Niklaus and Liu 2020, eq. 7): -alpha |I0 - I1(q + F)|.
                    vec4 vq = bmk_vectors_bilinear(2, 3, 5, 1, r.y, r.z);
                    vec4 c1 = bmk_bilinear(7, r.y + vq.x - on.x, r.z + vq.y - on.y, border);
                    BMK_P float e = abs(bmk_luma(c) - bmk_luma(c1));
                    BMK_P float z = bmk_clampf(-mode.z * e, -60.0f, 0.0f);
                    w = w * bmk_exp(z);
                }
                w = w * ws;
                total = total + w;
                sum = fma(c, vec4(w), sum);
            }
        }
    }
    if (!(total > 1.0e-6f)) {
        // Nothing reached the pixel: it keeps its own value (a static estimate).
        vec4 cp = bmk_fetch(1, gx - int(oc.x), gy - int(oc.y), border);
        if (bmk_has(6) && layer != 0) {
            BMK_P float m = bmk_fetch(6, gx - int(om.x), gy - int(om.y), 1).w;
            BMK_P float keep = layer == 1 ? m : 1.0f - m;
            cp = cp * keep;
        }
        return cp;
    }
    BMK_P vec4 r = sum * bmk_div(1.0f, total);
    r.w = bmk_clampf(r.w, 0.0f, 1.0f);
    return r;
}

// --- Outside-in hole filling (Baker et al. 2011) --------------------------------------------------

// The nearest known pixel by jump flooding (Rong and Tan 2006). Regions 0 (the seeds, active), 1
// (the image), 2 (the hole mask, absent: the image's own alpha), [image global x, y, mask global
// x, y], [threshold, 0, 0, 0] at 1. A seed pixel is [x, y, 1, 1] (local), a hole [0, 0, 0, 1].
BMK_FN vec4 bmk_jfa_init(int lx, int ly) {
    vec4 o = bmk_param(0);
    vec4 q = bmk_param(1);
    bool hole = false;
    if (bmk_has(2)) {
        vec4 m = bmk_fetch(2, lx + int(o.x) - int(o.z), ly + int(o.y) - int(o.w), 0);
        hole = m.w >= q.x;
    }
    if (hole)
        return vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return vec4(float(lx), float(ly), 1.0f, 1.0f);
}

// One jump: the nearest of the seeds the 3 x 3 pixels `step` apart have found. [step, 0, 0, 0].
BMK_FN vec4 bmk_jfa_step(int lx, int ly) {
    int jump = int(bmk_param(0).x);
    vec4 best = bmk_fetch(0, lx, ly, 1);
    BMK_P float bestDistance = 3.0e38f;
    if (best.z > 0.0f)
        bestDistance = bmk_length2(best.x - float(lx), best.y - float(ly));
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            if (i == 0 && j == 0)
                continue;
            vec4 s = bmk_fetch(0, lx + i * jump, ly + j * jump, 0);
            if (!(s.z > 0.0f))
                continue;
            BMK_P float d = bmk_length2(s.x - float(lx), s.y - float(ly));
            if (d < bestDistance) {
                bestDistance = d;
                best = s;
            }
        }
    return best;
}

// The fill. Regions 0 (output, active, the image's size), 1 (the image), 2 (the seeds). A known
// pixel keeps its value; a hole takes the average of the known pixels nearest it and its eight
// neighbours, the outside-in average of Baker et al.
BMK_FN vec4 bmk_fill(int lx, int ly) {
    vec4 s = bmk_fetch(2, lx, ly, 1);
    if (s.z > 0.0f && int(s.x) == lx && int(s.y) == ly)
        return bmk_fetch(1, lx, ly, 0);
    BMK_P vec4 sum = vec4(0.0f);
    BMK_P float count = 0.0f;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            vec4 n = bmk_fetch(2, lx + i, ly + j, 1);
            if (!(n.z > 0.0f))
                continue;
            sum = sum + bmk_fetch(1, int(n.x), int(n.y), 0);
            count = count + 1.0f;
        }
    if (!(count > 0.0f))
        return vec4(0.0f);
    BMK_P vec4 r = sum * bmk_div(1.0f, count);
    r.w = bmk_clampf(r.w, 0.0f, 1.0f);
    return r;
}

// --- Forward-backward consistency (Sundaram et al. 2010) -------------------------------------------

// Regions 0 (output, active, the flow's size), 1 (the flow w, RG), 2 (the other frame's flow
// back, RG). [flow global x, y, back global x, y], [border, invert, 0, 0]. Consistent when
// |w + w'|^2 < 0.01 (|w|^2 + |w'|^2) + 0.5, w' the back flow at x + w: the Mask transport of 1
// there and 0 at an occlusion (inverted: the occlusion). With regions 3 (a second flow) and 4 (its
// flow back) at [second global x, y, its back global x, y] (parameter 2), the pair as data:
// [first consistent, second consistent, 0, 1].
BMK_FN float bmk_consistent(int rf, int rb, vec4 o, int border, int lx, int ly) {
    vec4 w = bmk_fetch(rf, lx, ly, 0);
    BMK_P float x = float(lx) + o.x + w.x - o.z;
    BMK_P float y = float(ly) + o.y + w.y - o.w;
    vec4 b = bmk_bilinear(rb, x, y, border);
    BMK_P float sx = w.x + b.x;
    BMK_P float sy = w.y + b.y;
    BMK_P float lhs = bmk_length2(sx, sy);
    BMK_P float rhs = fma(0.01f, bmk_length2(w.x, w.y) + bmk_length2(b.x, b.y), 0.5f);
    return lhs < rhs ? 1.0f : 0.0f;
}

BMK_FN vec4 bmk_consistency(int lx, int ly) {
    vec4 q = bmk_param(1);
    int border = int(q.x);
    float c = bmk_consistent(1, 2, bmk_param(0), border, lx, ly);
    if (bmk_has(3)) {
        vec4 o2 = bmk_param(2);
        // The second flow shares the first's grid: its local pixel is the same global pixel.
        vec4 o = bmk_param(0);
        int sx = lx + int(o.x) - int(o2.x);
        int sy = ly + int(o.y) - int(o2.y);
        float d = bmk_consistent(3, 4, o2, border, sx, sy);
        return vec4(c, d, 0.0f, 1.0f);
    }
    if (q.y != 0.0f)
        c = 1.0f - c;
    return vec4(c);
}

// --- Optical flow: the pyramid --------------------------------------------------------------------

// Grey level for the flow: 255 L / (1 + L), L the luma of the associated colour (at least 0), so
// HDR stays bounded on the scale TV-L1's lambda is defined for. Regions 0 and 1 (the two frames'
// grey over the flow domain, active over both), 4 and 5 (the two frames' colour); [border, 0, 0,
// 0] at 1, [domain global x, y, 0, 0] at 2, the colour regions' global origins at 3 and 4. Writes
// [grey, 0, 0, 1].
BMK_FN float bmk_grey(vec4 c) {
    BMK_P float l = max(bmk_luma(c), 0.0f);
    BMK_P float n = 255.0f * l;
    BMK_P float d = 1.0f + l;
    BMK_P float g = bmk_div(n, d);
    return g;
}

BMK_FN vec4 bmk_grey_pass(int ax, int ay) {
    vec4 dom = bmk_param(2);
    int border = int(bmk_param(1).x);
    for (int r = 0; r < 2; ++r) {
        vec4 d = bmk_region(r);
        int lx = ax - int(d.x);
        int ly = ay - int(d.y);
        if (lx < 0 || ly < 0 || lx >= int(d.z) || ly >= int(d.w))
            continue;
        vec4 src = bmk_param(3 + r);
        int gx = lx + int(dom.x);
        int gy = ly + int(dom.y);
        vec4 c = bmk_fetch(4 + r, gx - int(src.x), gy - int(src.y), border);
        return vec4(bmk_grey(c), 0.0f, 0.0f, 1.0f);
    }
    return bmk_texel(ax, ay);
}

// 2x decimation by the 5 x 5 binomial filter [1 4 6 4 1] / 16 (Burt and Adelson). Regions 0
// (output, active), 1 (the finer level). Reads the finer level around 2x + 0.5 with the edge
// extended.
BMK_FN vec4 bmk_decimate(int lx, int ly) {
    BMK_P float sum = 0.0f;
    for (int j = -2; j <= 2; ++j) {
        BMK_P float wy = (j == 0) ? 6.0f : ((j == -1 || j == 1) ? 4.0f : 1.0f);
        for (int i = -2; i <= 2; ++i) {
            BMK_P float wx = (i == 0) ? 6.0f : ((i == -1 || i == 1) ? 4.0f : 1.0f);
            vec4 p = bmk_fetch(1, 2 * lx + i, 2 * ly + j, 1);
            sum = fma(p.x, wx * wy, sum);
        }
    }
    BMK_P float g = sum * 0.00390625f;
    return vec4(g, 0.0f, 0.0f, 1.0f);
}

// Central differences of a grey region with the edge extended.
BMK_FN vec2 bmk_gradient(int r, int x, int y) {
    BMK_P float gx = (bmk_fetch(r, x + 1, y, 1).x - bmk_fetch(r, x - 1, y, 1).x) * 0.5f;
    BMK_P float gy = (bmk_fetch(r, x, y + 1, 1).x - bmk_fetch(r, x, y - 1, 1).x) * 0.5f;
    return vec2(gx, gy);
}

// --- Optical flow: TV-L1 (Zach et al. 2007; Sanchez Perez et al., IPOL 2013) -----------------------

// The level atlas: regions 0 S1 [u1, p11, p12], 1 S2 [u2, p21, p22], 2 W [I1(x + u0), its x and y
// derivatives], 3 E [rho_c, data weight, I0], 4 G [I1, I1x, I1y], then for the level's start 5
// (the coarser S1), 6 (the coarser S2), 7 (I0 grey), 8 (I1 grey), 9 (the layer weight mask, w = 0
// none: weight 1). [lambda, theta, tau, border], [mask global offset x, y relative to the level's
// region origin, mask scale (level to mask pixels), 0] at 1.

// Level start: the regions from the pyramid and the coarser solution (u doubled, p zero).
BMK_FN vec4 bmk_tv_init(int ax, int ay) {
    vec4 mk = bmk_param(1);
    for (int r = 0; r < 5; ++r) {
        vec4 d = bmk_region(r);
        int lx = ax - int(d.x);
        int ly = ay - int(d.y);
        if (lx < 0 || ly < 0 || lx >= int(d.z) || ly >= int(d.w))
            continue;
        if (r == 0 || r == 1) {
            BMK_P float u = 0.0f;
            if (bmk_has(5)) {
                vec4 cd = bmk_region(5);
                BMK_P float sx = bmk_div(float(cd.z), float(d.z));
                BMK_P float sy = bmk_div(float(cd.w), float(d.w));
                BMK_P float x = fma(float(lx) + 0.5f, sx, -0.5f);
                BMK_P float y = fma(float(ly) + 0.5f, sy, -0.5f);
                vec4 c = bmk_bilinear(r == 0 ? 5 : 6, x, y, 1);
                BMK_P float scale = r == 0 ? bmk_div(1.0f, sx) : bmk_div(1.0f, sy);
                u = c.x * scale;
            }
            return vec4(u, 0.0f, 0.0f, 1.0f);
        }
        if (r == 2)
            return vec4(0.0f, 0.0f, 0.0f, 1.0f);
        if (r == 3) {
            BMK_P float w = 1.0f;
            if (bmk_has(9)) {
                BMK_P float mx = fma(float(lx) + 0.5f, mk.z, mk.x - 0.5f);
                BMK_P float my = fma(float(ly) + 0.5f, mk.z, mk.y - 0.5f);
                w = bmk_bilinear(9, mx, my, 1).w;
                if (mk.w != 0.0f)
                    w = 1.0f - w;
            }
            return vec4(0.0f, w, bmk_fetch(7, lx, ly, 1).x, 1.0f);
        }
        vec2 g = bmk_gradient(8, lx, ly);
        return vec4(bmk_fetch(8, lx, ly, 1).x, g.x, g.y, 1.0f);
    }
    return bmk_texel(ax, ay);
}

// Warp: I1, its derivatives at x + u0 and rho_c = I1w - grad(I1w) . u0 - I0 (IPOL 2013, eq. 21).
BMK_FN vec4 bmk_tv_warp(int ax, int ay) {
    vec4 h = bmk_param(0);
    int border = int(h.w);
    vec4 dw = bmk_region(2);
    vec4 de = bmk_region(3);
    bool inW = ax >= int(dw.x) && ax < int(dw.x) + int(dw.z) && ay >= int(dw.y) &&
               ay < int(dw.y) + int(dw.w);
    bool inE = ax >= int(de.x) && ax < int(de.x) + int(de.z) && ay >= int(de.y) &&
               ay < int(de.y) + int(de.w);
    if (!inW && !inE)
        return bmk_texel(ax, ay);
    int lx = inW ? ax - int(dw.x) : ax - int(de.x);
    int ly = inW ? ay - int(dw.y) : ay - int(de.y);
    vec4 s1 = bmk_fetch(0, lx, ly, 1);
    vec4 s2 = bmk_fetch(1, lx, ly, 1);
    BMK_P float x = float(lx) + s1.x;
    BMK_P float y = float(ly) + s2.x;
    vec4 g = bmk_bilinear(4, x, y, border);
    if (inW)
        return vec4(g.x, g.y, g.z, 1.0f);
    vec4 e = bmk_fetch(3, lx, ly, 1);
    BMK_P float gu = fma(g.y, s1.x, g.z * s2.x);
    BMK_P float rho = (g.x - gu) - e.z;
    return vec4(rho, e.y, e.z, 1.0f);
}

// v at local (x, y) for component `c` (0 or 1): the thresholding step (IPOL 2013, eq. 15) from u.
BMK_FN float bmk_tv_v(int x, int y, int c) {
    vec4 h = bmk_param(0);
    vec4 s1 = bmk_fetch(0, x, y, 1);
    vec4 s2 = bmk_fetch(1, x, y, 1);
    vec4 w = bmk_fetch(2, x, y, 1);
    vec4 e = bmk_fetch(3, x, y, 1);
    BMK_P float grad = bmk_length2(w.y, w.z);
    BMK_P float rho = e.x + fma(w.y, s1.x, w.z * s2.x);
    BMK_P float lt = h.x * h.y * e.y;
    BMK_P float gc = c == 0 ? w.y : w.z;
    BMK_P float uc = c == 0 ? s1.x : s2.x;
    BMK_P float d = 0.0f;
    BMK_P float bound = lt * grad;
    if (rho < -bound) {
        d = lt * gc;
    } else if (rho > bound) {
        d = -(lt * gc);
    } else if (grad > 1.0e-10f) {
        d = -bmk_div(rho * gc, grad);
    }
    BMK_P float v = uc + d;
    return v;
}

// div p at (x, y) for component c, the adjoint of the forward gradient (zero past the last column
// or row).
BMK_FN float bmk_tv_div(int x, int y, int c, int w, int h) {
    vec4 s = bmk_fetch(c, x, y, 1);
    BMK_P float px = x < w - 1 ? s.y : 0.0f;
    BMK_P float py = y < h - 1 ? s.z : 0.0f;
    BMK_P float lx = x > 0 ? bmk_fetch(c, x - 1, y, 1).y : 0.0f;
    BMK_P float ly = y > 0 ? bmk_fetch(c, x, y - 1, 1).z : 0.0f;
    BMK_P float r = (px - lx) + (py - ly);
    return r;
}

BMK_FN float bmk_tv_u(int x, int y, int c, int w, int h) {
    vec4 q = bmk_param(0);
    BMK_P float v = bmk_tv_v(x, y, c);
    BMK_P float u = fma(q.y, bmk_tv_div(x, y, c, w, h), v);
    return u;
}

// One iteration: u = v + theta div p, then p = (p + tau / theta grad u) / (1 + tau / theta |grad u|)
// with the new u (IPOL 2013, Algorithm 1), each component in its own region.
BMK_FN vec4 bmk_tv_iterate(int ax, int ay) {
    vec4 q = bmk_param(0);
    for (int c = 0; c < 2; ++c) {
        vec4 d = bmk_region(c);
        int w = int(d.z);
        int h = int(d.w);
        int lx = ax - int(d.x);
        int ly = ay - int(d.y);
        if (lx < 0 || ly < 0 || lx >= w || ly >= h)
            continue;
        BMK_P float u = bmk_tv_u(lx, ly, c, w, h);
        BMK_P float ux = lx < w - 1 ? bmk_tv_u(lx + 1, ly, c, w, h) - u : 0.0f;
        BMK_P float uy = ly < h - 1 ? bmk_tv_u(lx, ly + 1, c, w, h) - u : 0.0f;
        vec4 s = bmk_fetch(c, lx, ly, 1);
        BMK_P float k = bmk_div(q.z, q.y);
        BMK_P float g = bmk_length(ux, uy);
        BMK_P float den = fma(k, g, 1.0f);
        BMK_P float p1 = bmk_div(fma(k, ux, s.y), den);
        BMK_P float p2 = bmk_div(fma(k, uy, s.z), den);
        return vec4(u, p1, p2, 1.0f);
    }
    return bmk_texel(ax, ay);
}

// The flow out of a level atlas: [u1, u2, 0, 1] from regions 0 and 1 over the output (region 2).
BMK_FN vec4 bmk_flow_out(int lx, int ly) {
    vec4 a = bmk_fetch(0, lx, ly, 1);
    vec4 b = bmk_fetch(1, lx, ly, 1);
    return vec4(a.x, b.x, 0.0f, 1.0f);
}

// --- Optical flow: DIS (Kroeger et al. 2016) -------------------------------------------------------

// The level atlas: regions 0 G0 [I0, I0x, I0y], 1 G1 [I1, I1x, I1y], 2 the patch grid [u, v],
// 3 U [u, v] (dense), 4 J [a11, a12, a22], 5 B [b1, b2, smoothness weight], 6 DU [du, dv], 7 the
// coarser U, 8 I0 grey, 9 I1 grey, 10 the layer weight mask. [patch size, stride, iterations,
// border], [delta, gamma, alpha, refinement iterations], [mask offset x, y, scale, invert] at 2.

BMK_FN float bmk_layer_weight(int lx, int ly, int region, int params) {
    if (!bmk_has(region))
        return 1.0f;
    vec4 mk = bmk_param(params);
    BMK_P float mx = fma(float(lx) + 0.5f, mk.z, mk.x - 0.5f);
    BMK_P float my = fma(float(ly) + 0.5f, mk.z, mk.y - 0.5f);
    BMK_P float w = bmk_bilinear(region, mx, my, 1).w;
    if (mk.w != 0.0f)
        w = 1.0f - w;
    return w;
}

// Level start: G0 and G1 from the pyramid, U from the coarser level (doubled), DU zero.
BMK_FN vec4 bmk_dis_init(int ax, int ay) {
    for (int r = 0; r < 7; ++r) {
        vec4 d = bmk_region(r);
        int lx = ax - int(d.x);
        int ly = ay - int(d.y);
        if (lx < 0 || ly < 0 || lx >= int(d.z) || ly >= int(d.w))
            continue;
        if (r == 0 || r == 1) {
            int g = r == 0 ? 8 : 9;
            vec2 gr = bmk_gradient(g, lx, ly);
            return vec4(bmk_fetch(g, lx, ly, 1).x, gr.x, gr.y, 1.0f);
        }
        if (r == 3) {
            if (!bmk_has(7))
                return vec4(0.0f, 0.0f, 0.0f, 1.0f);
            vec4 cd = bmk_region(7);
            BMK_P float sx = bmk_div(float(cd.z), float(d.z));
            BMK_P float sy = bmk_div(float(cd.w), float(d.w));
            BMK_P float x = fma(float(lx) + 0.5f, sx, -0.5f);
            BMK_P float y = fma(float(ly) + 0.5f, sy, -0.5f);
            vec4 c = bmk_bilinear(7, x, y, 1);
            return vec4(c.x * bmk_div(1.0f, sx), c.y * bmk_div(1.0f, sy), 0.0f, 1.0f);
        }
        return vec4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    return bmk_texel(ax, ay);
}

// The inverse search of one patch (Kroeger et al. 2016, section 2.1): the patch's template from
// I0, zero-mean normalised, and iterations of inverse compositional Lucas-Kanade over I1 from the
// dense flow at the patch centre; a patch that moves farther than its size from its start keeps
// its start.
BMK_FN vec4 bmk_dis_search(int lx, int ly) {
    vec4 h = bmk_param(0);
    int size = int(h.x);
    int stride = int(h.y);
    int iterations = int(h.z);
    int border = int(h.w);
    vec4 d0 = bmk_region(0);
    int w = int(d0.z);
    int ht = int(d0.w);
    int x0 = min(lx * stride, max(w - size, 0));
    int y0 = min(ly * stride, max(ht - size, 0));
    // The start: the dense flow at the patch centre.
    vec4 start = bmk_fetch(3, x0 + size / 2, y0 + size / 2, 1);
    BMK_P float ux = start.x;
    BMK_P float uy = start.y;
    // The template's mean, the Hessian of its gradients (weighted by the layer weight).
    BMK_P float mean0 = 0.0f;
    BMK_P float h11 = 0.0f;
    BMK_P float h12 = 0.0f;
    BMK_P float h22 = 0.0f;
    BMK_P float wsum = 0.0f;
    for (int j = 0; j < size; ++j)
        for (int i = 0; i < size; ++i) {
            vec4 g = bmk_fetch(0, x0 + i, y0 + j, 1);
            BMK_P float lw = bmk_layer_weight(x0 + i, y0 + j, 10, 2);
            mean0 = fma(g.x, lw, mean0);
            wsum = wsum + lw;
            h11 = fma(g.y * g.y, lw, h11);
            h12 = fma(g.y * g.z, lw, h12);
            h22 = fma(g.z * g.z, lw, h22);
        }
    if (!(wsum > 0.0f))
        return vec4(ux, uy, 0.0f, 1.0f);
    mean0 = bmk_div(mean0, wsum);
    h11 = h11 + 1.0e-3f;
    h22 = h22 + 1.0e-3f;
    BMK_P float det = fma(h11, h22, -(h12 * h12));
    if (!(det > 1.0e-12f))
        return vec4(ux, uy, 0.0f, 1.0f);
    for (int it = 0; it < iterations; ++it) {
        BMK_P float mean1 = 0.0f;
        for (int j = 0; j < size; ++j)
            for (int i = 0; i < size; ++i) {
                BMK_P float lw = bmk_layer_weight(x0 + i, y0 + j, 10, 2);
                vec4 s = bmk_bilinear(1, float(x0 + i) + ux, float(y0 + j) + uy, border);
                mean1 = fma(s.x, lw, mean1);
            }
        mean1 = bmk_div(mean1, wsum);
        BMK_P float bx = 0.0f;
        BMK_P float by = 0.0f;
        for (int j = 0; j < size; ++j)
            for (int i = 0; i < size; ++i) {
                vec4 g = bmk_fetch(0, x0 + i, y0 + j, 1);
                BMK_P float lw = bmk_layer_weight(x0 + i, y0 + j, 10, 2);
                vec4 s = bmk_bilinear(1, float(x0 + i) + ux, float(y0 + j) + uy, border);
                BMK_P float e = ((s.x - mean1) - (g.x - mean0)) * lw;
                bx = fma(g.y, e, bx);
                by = fma(g.z, e, by);
            }
        // du = H^-1 b; u <- u - du (the inverse compositional update for a translation).
        BMK_P float dx = bmk_div(fma(h22, bx, -(h12 * by)), det);
        BMK_P float dy = bmk_div(fma(h11, by, -(h12 * bx)), det);
        ux = ux - dx;
        uy = uy - dy;
    }
    if (bmk_length(ux - start.x, uy - start.y) > float(size)) {
        ux = start.x;
        uy = start.y;
    }
    return vec4(ux, uy, 0.0f, 1.0f);
}

// Densification (Kroeger et al. 2016, eq. 3): the patches over a pixel weighted by
// 1 / max(1, |I1(x + u_i) - I0(x)|).
BMK_FN vec4 bmk_dis_densify(int lx, int ly) {
    vec4 h = bmk_param(0);
    int size = int(h.x);
    int stride = int(h.y);
    int border = int(h.w);
    vec4 d0 = bmk_region(0);
    vec4 dp = bmk_region(2);
    int w = int(d0.z);
    int ht = int(d0.w);
    int pw = int(dp.z);
    int ph = int(dp.w);
    BMK_P float i0 = bmk_fetch(0, lx, ly, 1).x;
    BMK_P float sx = 0.0f;
    BMK_P float sy = 0.0f;
    BMK_P float sw = 0.0f;
    int nx = lx - size + stride;
    int ny = ly - size + stride;
    int first = nx > 0 ? nx / stride : 0;
    int firstY = ny > 0 ? ny / stride : 0;
    for (int py = firstY; py < ph; ++py) {
        int y0 = min(py * stride, max(ht - size, 0));
        if (y0 > ly)
            break;
        if (ly >= y0 + size)
            continue;
        for (int px = first; px < pw; ++px) {
            int x0 = min(px * stride, max(w - size, 0));
            if (x0 > lx)
                break;
            if (lx >= x0 + size)
                continue;
            vec4 u = bmk_fetch(2, px, py, 1);
            vec4 s = bmk_bilinear(1, float(lx) + u.x, float(ly) + u.y, border);
            BMK_P float e = abs(s.x - i0);
            BMK_P float lw = bmk_div(1.0f, max(1.0f, e));
            sx = fma(u.x, lw, sx);
            sy = fma(u.y, lw, sy);
            sw = sw + lw;
        }
    }
    if (!(sw > 0.0f))
        return bmk_fetch(3, lx, ly, 1);
    return vec4(bmk_div(sx, sw), bmk_div(sy, sw), 0.0f, 1.0f);
}

// The variational refinement's data (Kroeger et al. 2016, section 2.3, after Brox et al. 2004):
// intensity and gradient constancy linearised at u, robustly weighted (Charbonnier), as the 2 x 2
// system J and right side B, and the smoothness weight of u.
BMK_FN vec4 bmk_dis_data(int ax, int ay) {
    vec4 h = bmk_param(0);
    vec4 r = bmk_param(1);
    int border = int(h.w);
    vec4 dj = bmk_region(4);
    vec4 db = bmk_region(5);
    vec4 du = bmk_region(6);
    bool inJ = ax >= int(dj.x) && ax < int(dj.x) + int(dj.z) && ay >= int(dj.y) &&
               ay < int(dj.y) + int(dj.w);
    bool inB = ax >= int(db.x) && ax < int(db.x) + int(db.z) && ay >= int(db.y) &&
               ay < int(db.y) + int(db.w);
    bool inD = ax >= int(du.x) && ax < int(du.x) + int(du.z) && ay >= int(du.y) &&
               ay < int(du.y) + int(du.w);
    if (!inJ && !inB && !inD)
        return bmk_texel(ax, ay);
    if (inD)
        return vec4(0.0f, 0.0f, 0.0f, 1.0f);
    int lx = inJ ? ax - int(dj.x) : ax - int(db.x);
    int ly = inJ ? ay - int(dj.y) : ay - int(db.y);
    vec4 u = bmk_fetch(3, lx, ly, 1);
    vec4 g0 = bmk_fetch(0, lx, ly, 1);
    BMK_P float x = float(lx) + u.x;
    BMK_P float y = float(ly) + u.y;
    vec4 g1 = bmk_bilinear(1, x, y, border);
    vec4 gxp = bmk_bilinear(1, x + 1.0f, y, border);
    vec4 gxm = bmk_bilinear(1, x - 1.0f, y, border);
    vec4 gyp = bmk_bilinear(1, x, y + 1.0f, border);
    vec4 gym = bmk_bilinear(1, x, y - 1.0f, border);
    BMK_P float lw = bmk_layer_weight(lx, ly, 10, 2);
    // Intensity: Iz + Ix du + Iy dv.
    BMK_P float iz = g1.x - g0.x;
    BMK_P float ix = g1.y;
    BMK_P float iy = g1.z;
    // Gradient: (I1x - I0x) + I1xx du + I1xy dv and (I1y - I0y) + I1xy du + I1yy dv.
    BMK_P float ixx = (gxp.y - gxm.y) * 0.5f;
    BMK_P float ixy = (gyp.y - gym.y) * 0.5f;
    BMK_P float iyy = (gyp.z - gym.z) * 0.5f;
    BMK_P float gzx = g1.y - g0.y;
    BMK_P float gzy = g1.z - g0.z;
    BMK_P float eps = 1.0e-3f;
    BMK_P float ei = bmk_div(r.x * lw, bmk_sqrt(fma(iz, iz, eps * eps)));
    BMK_P float eg = bmk_div(r.y * lw, bmk_sqrt(fma(gzx, gzx, fma(gzy, gzy, eps * eps))));
    if (inJ) {
        BMK_P float a11 = fma(ei, ix * ix, eg * fma(ixx, ixx, ixy * ixy));
        BMK_P float a12 = fma(ei, ix * iy, eg * fma(ixx, ixy, ixy * iyy));
        BMK_P float a22 = fma(ei, iy * iy, eg * fma(ixy, ixy, iyy * iyy));
        return vec4(a11, a12, a22, 1.0f);
    }
    BMK_P float b1 = -fma(ei, ix * iz, eg * fma(ixx, gzx, ixy * gzy));
    BMK_P float b2 = -fma(ei, iy * iz, eg * fma(ixy, gzx, iyy * gzy));
    // Smoothness weight psi'(|grad u|^2) from forward differences.
    vec4 ur = bmk_fetch(3, lx + 1, ly, 1);
    vec4 ud = bmk_fetch(3, lx, ly + 1, 1);
    BMK_P float s = bmk_length2(ur.x - u.x, ud.x - u.x) + bmk_length2(ur.y - u.y, ud.y - u.y);
    BMK_P float ws = bmk_div(r.z, bmk_sqrt(s + eps * eps));
    return vec4(b1, b2, ws, 1.0f);
}

// One Jacobi iteration of the refinement's linear system for du: (J + sum w_n) du = B +
// sum w_n (u_n + du_n - u), over the four neighbours.
BMK_FN vec4 bmk_dis_iterate(int ax, int ay) {
    vec4 du = bmk_region(6);
    int lx = ax - int(du.x);
    int ly = ay - int(du.y);
    if (lx < 0 || ly < 0 || lx >= int(du.z) || ly >= int(du.w))
        return bmk_texel(ax, ay);
    int w = int(du.z);
    int h = int(du.w);
    vec4 j = bmk_fetch(4, lx, ly, 1);
    vec4 b = bmk_fetch(5, lx, ly, 1);
    vec4 u = bmk_fetch(3, lx, ly, 1);
    BMK_P float sw = 0.0f;
    BMK_P float rx = b.x;
    BMK_P float ry = b.y;
    for (int n = 0; n < 4; ++n) {
        int nx = lx + (n == 0 ? 1 : (n == 1 ? -1 : 0));
        int ny = ly + (n == 2 ? 1 : (n == 3 ? -1 : 0));
        if (nx < 0 || ny < 0 || nx >= w || ny >= h)
            continue;
        vec4 wn = n == 0 || n == 2 ? b : bmk_fetch(5, nx, ny, 1);
        BMK_P float ws = wn.z;
        vec4 un = bmk_fetch(3, nx, ny, 1);
        vec4 dn = bmk_fetch(6, nx, ny, 1);
        sw = sw + ws;
        rx = fma(ws, (un.x + dn.x) - u.x, rx);
        ry = fma(ws, (un.y + dn.y) - u.y, ry);
    }
    BMK_P float a11 = j.x + sw;
    BMK_P float a22 = j.z + sw;
    BMK_P float det = fma(a11, a22, -(j.y * j.y));
    if (!(abs(det) > 1.0e-12f))
        return vec4(0.0f, 0.0f, 0.0f, 1.0f);
    BMK_P float dx = bmk_div(fma(a22, rx, -(j.y * ry)), det);
    BMK_P float dy = bmk_div(fma(a11, ry, -(j.y * rx)), det);
    return vec4(dx, dy, 0.0f, 1.0f);
}

// u <- u + du after the refinement's iterations.
BMK_FN vec4 bmk_dis_update(int ax, int ay) {
    vec4 d = bmk_region(3);
    int lx = ax - int(d.x);
    int ly = ay - int(d.y);
    if (lx < 0 || ly < 0 || lx >= int(d.z) || ly >= int(d.w))
        return bmk_texel(ax, ay);
    vec4 u = bmk_fetch(3, lx, ly, 1);
    vec4 du = bmk_fetch(6, lx, ly, 1);
    return vec4(u.x + du.x, u.y + du.y, 0.0f, 1.0f);
}

// --- Cut detection (diagnostic only) -------------------------------------------------------------

// The flow residual |I1(x + w) - I0(x)| (grey). Regions 0 (output, active), 1 (I0 colour), 2 (I1
// colour), 3 (the flow); [I0 global x, y, I1 global x, y], [flow global x, y, border, 0] at 1.
BMK_FN vec4 bmk_cut_residual(int lx, int ly) {
    vec4 o = bmk_param(0);
    vec4 of = bmk_param(1);
    vec4 out0 = bmk_param(2);
    int border = int(of.z);
    int gx = lx + int(out0.x);
    int gy = ly + int(out0.y);
    vec4 w = bmk_fetch(3, gx - int(of.x), gy - int(of.y), 1);
    vec4 a = bmk_fetch(1, gx - int(o.x), gy - int(o.y), border);
    vec4 b = bmk_bilinear(2, float(gx) + w.x - o.z, float(gy) + w.y - o.w, border);
    BMK_P float e = abs(bmk_grey(b) - bmk_grey(a));
    return vec4(e, 1.0f, 0.0f, 1.0f);
}

// Sum of a 16 x 16 block: [sum of residuals, sum of counts]. Regions 0 (output, active), 1 (the
// finer sums).
BMK_FN vec4 bmk_cut_reduce(int lx, int ly) {
    BMK_P float s = 0.0f;
    BMK_P float n = 0.0f;
    for (int j = 0; j < 16; ++j)
        for (int i = 0; i < 16; ++i) {
            vec4 p = bmk_fetch(1, lx * 16 + i, ly * 16 + j, 0);
            s = s + p.x;
            n = n + p.y;
        }
    return vec4(s, n, 0.0f, 1.0f);
}

// The cut flag over the output: the Mask transport of 1 when the mean residual exceeds the
// threshold. Regions 0 (output, active), 1 (the one-pixel sum); [threshold, 0, 0, 0].
BMK_FN vec4 bmk_cut_broadcast() {
    vec4 p = bmk_fetch(1, 0, 0, 0);
    BMK_P float mean = p.y > 0.0f ? bmk_div(p.x, p.y) : 0.0f;
    float c = mean > bmk_param(0).x ? 1.0f : 0.0f;
    return vec4(c);
}

// --- Temporal background fill (opt-in) -------------------------------------------------------------

// The background behind the foreground from the neighbour frames along the background's flow,
// where they see background. Regions 0 (output, active, the hole image's size), 1 (the holed
// background), 2 (this frame's matte), 3 (the forward flow), 4 (the backward flow), 5 (the next
// colour), 6 (the previous colour), 7 (the next matte), 8 (the previous matte). Origins as [x, y,
// 0, 0]: background 0, matte 1, forward 2, backward 3, next colour 4, previous colour 5, next matte
// 6, previous matte 7; [threshold, mask output, border, 0] at 8. With `mask output` the pass gives
// the Mask of the pixels still unfilled instead of the colour.
BMK_FN vec4 bmk_temporal_fill(int lx, int ly) {
    vec4 ob = bmk_param(0);
    vec4 q = bmk_param(8);
    int border = int(q.z);
    int gx = lx + int(ob.x);
    int gy = ly + int(ob.y);
    vec4 om = bmk_param(1);
    BMK_P float m = bmk_fetch(2, gx - int(om.x), gy - int(om.y), 0).w;
    vec4 own = bmk_fetch(1, lx, ly, 0);
    if (m < q.x)
        return q.y != 0.0f ? vec4(0.0f) : own;
    BMK_P vec4 sum = vec4(0.0f);
    BMK_P float n = 0.0f;
    for (int s = 0; s < 2; ++s) {
        vec4 of = bmk_param(s == 0 ? 2 : 3);
        vec4 w = bmk_fetch(s == 0 ? 3 : 4, gx - int(of.x), gy - int(of.y), 1);
        if (!bmk_has(s == 0 ? 5 : 6))
            continue;
        BMK_P float x = float(gx) + w.x;
        BMK_P float y = float(gy) + w.y;
        vec4 onm = bmk_param(s == 0 ? 6 : 7);
        BMK_P float nm = bmk_bilinear(s == 0 ? 7 : 8, x - onm.x, y - onm.y, 1).w;
        if (nm >= q.x)
            continue;
        vec4 oc = bmk_param(s == 0 ? 4 : 5);
        sum = sum + bmk_bilinear(s == 0 ? 5 : 6, x - oc.x, y - oc.y, border);
        n = n + 1.0f;
    }
    if (q.y != 0.0f)
        return n > 0.0f ? vec4(0.0f) : vec4(1.0f);
    if (!(n > 0.0f))
        return own;
    BMK_P vec4 r = sum * bmk_div(1.0f, n);
    r.w = bmk_clampf(r.w, 0.0f, 1.0f);
    return r;
}

// --- The pass dispatch ------------------------------------------------------------------------------

BMK_FN vec4 bmk_main(int ax, int ay) {
    vec4 t0 = bmk_data(0);
    vec4 t1 = bmk_data(1);
    int pass = int(t0.x);
    int x0 = int(t0.y);
    int y0 = int(t0.z);
    int w = int(t0.w);
    int h = int(t1.x);
    int lx = ax - x0;
    int ly = ay - y0;
    if (lx < 0 || ly < 0 || lx >= w || ly >= h)
        return bmk_texel(ax, ay);
    if (BMK_IS_PASS(BMK_PASS_ACCUMULATE))
        return bmk_accumulate(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_PATH))
        return bmk_path(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_TILE))
        return bmk_tile(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_NEIGHBOUR))
        return bmk_neighbour(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_GATHER))
        return bmk_gather(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_SPLAT))
        return bmk_splat(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_JFA_INIT))
        return bmk_jfa_init(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_JFA_STEP))
        return bmk_jfa_step(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_FILL))
        return bmk_fill(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_CONSISTENCY))
        return bmk_consistency(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_GREY))
        return bmk_grey_pass(ax, ay);
    if (BMK_IS_PASS(BMK_PASS_DECIMATE))
        return bmk_decimate(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_TV_INIT))
        return bmk_tv_init(ax, ay);
    if (BMK_IS_PASS(BMK_PASS_TV_WARP))
        return bmk_tv_warp(ax, ay);
    if (BMK_IS_PASS(BMK_PASS_TV_ITERATE))
        return bmk_tv_iterate(ax, ay);
    if (BMK_IS_PASS(BMK_PASS_FLOW_OUT))
        return bmk_flow_out(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_DIS_INIT))
        return bmk_dis_init(ax, ay);
    if (BMK_IS_PASS(BMK_PASS_DIS_SEARCH))
        return bmk_dis_search(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_DIS_DENSIFY))
        return bmk_dis_densify(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_DIS_DATA))
        return bmk_dis_data(ax, ay);
    if (BMK_IS_PASS(BMK_PASS_DIS_ITERATE))
        return bmk_dis_iterate(ax, ay);
    if (BMK_IS_PASS(BMK_PASS_DIS_UPDATE))
        return bmk_dis_update(ax, ay);
    if (BMK_IS_PASS(BMK_PASS_CUT_RESIDUAL))
        return bmk_cut_residual(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_CUT_REDUCE))
        return bmk_cut_reduce(lx, ly);
    if (BMK_IS_PASS(BMK_PASS_CUT_BROADCAST))
        return bmk_cut_broadcast();
    if (BMK_IS_PASS(BMK_PASS_TEMPORAL_FILL))
        return bmk_temporal_fill(lx, ly);
    return bmk_texel(ax, ay);
}
