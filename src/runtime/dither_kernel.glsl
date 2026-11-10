// #527 Dither: the one dither kernel. This exact text is compiled twice: as C++ by
// src/runtime/dither_image_effect.cpp (the CPU reference) and as GLSL 4.60 by the GPU program the
// runtime builds around it (ditherProgramText()). Each host defines the dialect first:
//   BDK_FN          a function's qualifiers ("inline" in C++, nothing in GLSL)
//   BDK_P           GLSL `precise` (nothing in C++, built without contraction or fast math)
//   BDK_INOUT(T)    an inout parameter
//   bdk_data(i)     the i-th float of the program table
//   bdk_div(a, b)   correctly rounded binary32 division
// The dialect is a common subset: binary32 arithmetic, fma, floor, abs, frexp, ldexp and integer
// operations. Every intermediate that could be fused or reordered is a precise variable and every
// transcendental is the field kernel's polynomial, so the CPU and every device evaluate the same
// binary32 operations. docs/architecture/image-filters.md, "Dither", is the specification.
//
// The table, in floats:
//   0  the amplitude in the domain's units (amount / (2^bits - 1)), the domain (0 Display, 1 Stored),
//      the pattern's side n, 1 / (n * n)
//   4  red's shift x, shift y and rank rotation, 0; 8 green's; 12 blue's (Monochrome: red's three times)
//  16  the data window's origin x + 0.5 and y + 0.5, 1 / scale x and 1 / scale y (the geometry)
//  20  the pattern's n * n ranks, row major

const int BDK_RANKS = 20;
// A straight value beyond this (the largest half float) passes unchanged in the Display domain.
const float BDK_DISPLAY_LIMIT = 65504.0f;

// 2^x for x in [-126, 126] (clamped): 2^floor(x) times a degree-9 polynomial of the fraction
// (field_kernel.glsl, bfk_exp2).
BDK_FN BDK_P float bdk_exp2(float x) {
    BDK_P float c = min(max(x, -126.0f), 126.0f);
    BDK_P float whole = floor(c);
    BDK_P float f = c - whole;
    BDK_P float p = 1.01780863e-07f;
    p = fma(p, f, 1.32154867e-06f);
    p = fma(p, f, 1.52527336e-05f);
    p = fma(p, f, 0.000154035297f);
    p = fma(p, f, 0.00133335579f);
    p = fma(p, f, 0.00961812865f);
    p = fma(p, f, 0.0555041097f);
    p = fma(p, f, 0.240226507f);
    p = fma(p, f, 0.693147182f);
    p = fma(p, f, 1.0f);
    BDK_P float r = ldexp(p, int(whole));
    return r;
}

// log2(x) for x > 0 (-126 otherwise) (field_kernel.glsl, bfk_log2).
BDK_FN BDK_P float bdk_log2(float x) {
    if (!(x > 0.0f))
        return -126.0f;
    int e = 0;
    BDK_P float m = frexp(x, e);
    if (m < 0.707106769f) {
        m = m * 2.0f;
        e = e - 1;
    }
    BDK_P float num = m - 1.0f;
    BDK_P float den = m + 1.0f;
    BDK_P float s = bdk_div(num, den);
    BDK_P float s2 = s * s;
    BDK_P float q = 0.0769230798f;
    q = fma(q, s2, 0.0909090936f);
    q = fma(q, s2, 0.111111112f);
    q = fma(q, s2, 0.142857149f);
    q = fma(q, s2, 0.200000003f);
    q = fma(q, s2, 0.333333343f);
    q = fma(q, s2, 1.0f);
    BDK_P float sq = s * q;
    BDK_P float ln = sq * 2.0f;
    BDK_P float r = fma(ln, 1.44269502f, float(e));
    return r;
}

// a^b for a > 0 (0 otherwise).
BDK_FN BDK_P float bdk_pow(float a, float b) {
    if (!(a > 0.0f))
        return 0.0f;
    BDK_P float l = bdk_log2(a);
    BDK_P float t = b * l;
    BDK_P float r = bdk_exp2(t);
    return r;
}

// The sRGB transfer (IEC 61966-2-1) of a straight value, odd about zero: the display-encoded value
// the amplitude is measured in.
BDK_FN BDK_P float bdk_encode(float v) {
    BDK_P float a = abs(v);
    BDK_P float e = 0.0f;
    if (a <= 0.0031308f) {
        e = a * 12.9200001f;
    } else {
        BDK_P float p = bdk_pow(a, 0.416666657f);
        BDK_P float q = p * 1.05499995f;
        e = q - 0.0549999997f;
    }
    BDK_P float r = v < 0.0f ? -e : e;
    return r;
}

// The inverse of bdk_encode.
BDK_FN BDK_P float bdk_decode(float v) {
    BDK_P float a = abs(v);
    BDK_P float d = 0.0f;
    if (a <= 0.0404499993f) {
        d = bdk_div(a, 12.9200001f);
    } else {
        BDK_P float s = a + 0.0549999997f;
        BDK_P float t = bdk_div(s, 1.05499995f);
        d = bdk_pow(t, 2.4000001f);
    }
    BDK_P float r = v < 0.0f ? -d : d;
    return r;
}

// The centred threshold in (-1/2, 1/2) of the full-resolution pixel (x, y) for `channel`: the
// pattern moved by the channel's shift, its rank rotated, as (rank + 0.5) / (n * n) - 0.5.
BDK_FN BDK_P float bdk_threshold(int x, int y, int channel) {
    int side = int(bdk_data(2));
    int mask = side - 1;
    int cells = side * side;
    int at = 4 + 4 * channel;
    int sx = int(bdk_data(at));
    int sy = int(bdk_data(at + 1));
    int rotation = int(bdk_data(at + 2));
    int cx = (x + sx) & mask;
    int cy = (y + sy) & mask;
    int rank = int(bdk_data(BDK_RANKS + cy * side + cx));
    int rotated = (rank + rotation) & (cells - 1);
    BDK_P float t = float(rotated) + 0.5f;
    BDK_P float u = t * bdk_data(3);
    BDK_P float r = u - 0.5f;
    return r;
}

// One premultiplied colour channel `v` of a pixel with alpha `a` and centred threshold `n`.
BDK_FN BDK_P float bdk_channel(float v, float a, float n) {
    BDK_P float amount = n * bdk_data(0);
    // Nothing to add leaves the value exactly as it is (amplitude 0 is the identity).
    if (amount == 0.0f)
        return v;
    if (bdk_data(1) != 0.0f) {
        // Stored: the amplitude in the stored (working-space) value, in proportion to alpha.
        BDK_P float scaled = amount * a;
        BDK_P float r = v + scaled;
        return r;
    }
    // Display: added to the straight value's sRGB encoding, then decoded and premultiplied again.
    BDK_P float s = bdk_div(v, a);
    if (!(abs(s) <= BDK_DISPLAY_LIMIT))
        return v;
    BDK_P float e = bdk_encode(s);
    BDK_P float moved = e + amount;
    BDK_P float d = bdk_decode(moved);
    BDK_P float r = d * a;
    return r;
}

// The pixel at the data window's index (column, row), in place. A pixel with alpha 0 is left as
// it is; alpha never changes.
BDK_FN void bdk_dither(int column, int row, BDK_INOUT(float) red, BDK_INOUT(float) green,
                       BDK_INOUT(float) blue, float alpha) {
    if (alpha == 0.0f)
        return;
    // The full-resolution pixel under this one's centre, wherever the data window, a region of
    // interest or a proxy scale put it.
    BDK_P float fx = float(column) + bdk_data(16);
    BDK_P float fy = float(row) + bdk_data(17);
    BDK_P float px = fx * bdk_data(18);
    BDK_P float py = fy * bdk_data(19);
    int x = int(floor(px));
    int y = int(floor(py));
    BDK_P float nr = bdk_threshold(x, y, 0);
    BDK_P float ng = bdk_threshold(x, y, 1);
    BDK_P float nb = bdk_threshold(x, y, 2);
    red = bdk_channel(red, alpha, nr);
    green = bdk_channel(green, alpha, ng);
    blue = bdk_channel(blue, alpha, nb);
}
