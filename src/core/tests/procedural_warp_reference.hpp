#pragma once

// #510 Procedural Warp: an independent binary64 reading of each mode's documented inverse map
// (docs/architecture/fields.md, "Procedural warp"), written from the specification with the
// standard library's own sine, arc tangent, square root and power. Shared by the field kernel's own
// test and the snapshot compiler's pixel tests; it never reads the kernel.

#include <bloom/core/field_program.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace bloom::procedural_warp_reference {

using core::field::ProceduralWarpMode;
using core::field::ProceduralWarpParameters;

inline constexpr double kPi = std::numbers::pi;

inline double smooth01(const double t) {
    const auto c = std::clamp(t, 0.0, 1.0);
    return c * c * (3.0 - 2.0 * c);
}

// The falloff weight at the fraction t of the radius: 1 inside, fading to 0 over the outer
// `falloff` share of the radius.
inline double fade(const double t, const double falloff) {
    return falloff > 0.0 ? smooth01((1.0 - t) / falloff) : 1.0;
}

// The radius (a fraction) that light through a refracting sphere cap of index n at fraction t
// comes from: t - sqrt(1 - t^2) tan(asin t - asin(t / n)).
inline double sphereSource(const double t, const double n) {
    return t - std::sqrt(1.0 - t * t) * std::tan(std::asin(t) - std::asin(t / n));
}

// The first word of the 4D permuted congruential generator of Jarzynski and Olano, "Hash Functions
// for GPU Rendering" (JCGT 9(3), 2020), written from the paper in integers, and its top 24 bits as a
// unit value: the stream Drizzle places its drops with.
inline std::uint32_t pcg4dFirst(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t w) {
    x = x * 1664525u + 1013904223u;
    y = y * 1664525u + 1013904223u;
    z = z * 1664525u + 1013904223u;
    w = w * 1664525u + 1013904223u;
    x += y * w;
    y += z * x;
    z += x * y;
    w += y * z;
    x ^= x >> 16u;
    y ^= y >> 16u;
    z ^= z >> 16u;
    w ^= w >> 16u;
    x += y * w;
    y += z * x;
    z += x * y;
    w += y * z;
    return x;
}

inline double unitOf(const std::uint32_t h) { return static_cast<double>(h >> 8u) / 16777216.0; }

// The pinned edges' product of eases at a position, as Wave and Turbulent pin: each pinned edge of
// the frame multiplies by smooth01(distance to it / length).
inline double pinWeight(const std::uint8_t pins, const double px, const double py, const double halfW,
                        const double halfH, const double length) {
    double w = 1.0;
    if ((pins & core::field::kWarpPinLeft) != 0)
        w *= smooth01((px + halfW) / length);
    if ((pins & core::field::kWarpPinRight) != 0)
        w *= smooth01((halfW - px) / length);
    if ((pins & core::field::kWarpPinTop) != 0)
        w *= smooth01((halfH - py) / length);
    if ((pins & core::field::kWarpPinBottom) != 0)
        w *= smooth01((py + halfH) / length);
    return w;
}

// Warp's style on the perspective-corrected normalised position (u, v).
inline void warpStyle(const int style, const double b, const double u, const double v,
                      const double halfW, const double halfH, const double circle, double& u2,
                      double& v2) {
    const auto cu = std::clamp(u, -1.0, 1.0);
    const auto cv = std::clamp(v, -1.0, 1.0);
    const auto arch = 1.0 - cu * cu;
    const auto side = 1.0 - cv * cv;
    u2 = u;
    v2 = v;
    switch (style) {
    case 0:
        v2 = v - b * arch;
        break;
    case 1:
        v2 = v - b * arch * (1.0 - cv) / 2.0;
        break;
    case 2:
        v2 = v - b * arch * (1.0 + cv) / 2.0;
        break;
    case 3:
        v2 = v / (1.0 + b * arch / 2.0);
        break;
    case 4: {
        const auto s = 1.0 + b / 2.0 * std::max(0.0, 1.0 - (cu * cu + cv * cv) / 2.0);
        u2 = u / s;
        v2 = v / s;
        break;
    }
    case 5:
        v2 = v - b * std::sin(kPi * cu) / 2.0;
        break;
    case 6:
        v2 = v - b * std::sin(kPi * cu) * cv / 2.0;
        break;
    case 7: {
        const auto t = (cu + 0.5) / 1.5;
        v2 = v / (1.0 + b / 2.0 * std::max(0.0, 1.0 - t * t));
        break;
    }
    case 8:
        u2 = u * (1.0 + b * cv / 2.0);
        break;
    case 9: {
        const auto x = u * halfW / circle;
        const auto y = v * halfH / circle;
        const auto k = 1.0 - b / 2.0 * std::max(0.0, 1.0 - (x * x + y * y));
        u2 = u * k;
        v2 = v * k;
        break;
    }
    case 10:
        u2 = u / (1.0 + b * side / 2.0);
        break;
    case 11:
        u2 = u * (1.0 + b * side / 2.0);
        v2 = v * (1.0 + b * arch / 2.0);
        break;
    default:
        u2 = u - b * cv / 2.0;
        v2 = v + b * cu / 2.0;
        break;
    }
}

struct Reading final {
    double x = 0.0;
    double y = 0.0;
    double weight = 1.0;
    // The position lies on a discontinuity the map has by definition (an edge with no falloff, a
    // square or saw wave's step, the polar seam): a binary32 and a binary64 reading may fall on
    // either side of it.
    bool step = false;
};

// `noiseX`, `noiseY`: Turbulent's noise vector at the position (its second input).
inline Reading reference(const ProceduralWarpParameters& p, const double px, const double py,
                         const double noiseX = 0.0, const double noiseY = 0.0) {
    Reading out{px, py, 1.0, false};
    const auto dx = px - p.centreX;
    const auto dy = py - p.centreY;
    const auto r = std::hypot(dx, dy);
    const auto falloff = std::clamp(p.falloff, 0.0, 1.0);
    const auto near = [](const double a, const double b) { return std::abs(a - b) < 1.0e-3; };
    switch (p.mode) {
    case ProceduralWarpMode::Twirl:
    case ProceduralWarpMode::BulgePinch:
    case ProceduralWarpMode::Spherize:
    case ProceduralWarpMode::Ripple: {
        if (!(p.radius > 0.0) || r >= p.radius) {
            out.step = falloff == 0.0 && near(r, p.radius);
            return out;
        }
        const auto t = r / p.radius;
        out.step = falloff == 0.0 && near(r, p.radius);
        if (p.mode == ProceduralWarpMode::Twirl) {
            // Turned counter-clockwise (+y up) by the angle times the falloff: the picture turns
            // clockwise on screen.
            const auto a = p.angleDegrees * kPi / 180.0 * fade(t, falloff);
            out.x = p.centreX + dx * std::cos(a) - dy * std::sin(a);
            out.y = p.centreY + dx * std::sin(a) + dy * std::cos(a);
        } else if (p.mode == ProceduralWarpMode::BulgePinch) {
            const auto e = std::clamp(p.amount, -1.0, 1.0) * fade(t, falloff);
            const auto s = std::sin(kPi * t / 2.0);
            const auto k = s > 0.0 ? std::pow(s, e) : 0.0;
            out.x = p.centreX + dx * k;
            out.y = p.centreY + dy * k;
        } else if (p.mode == ProceduralWarpMode::Spherize) {
            const auto a = std::clamp(p.amount, -1.0, 1.0);
            double source = t;
            if (a >= 0.0) {
                source = t - a * (t - sphereSource(t, p.refraction));
            } else {
                // The inverse of the positive map at |a|, to binary64 precision.
                double lo = 0.0;
                double hi = 1.0;
                for (int i = 0; i < 80; ++i) {
                    const auto mid = 0.5 * (lo + hi);
                    const auto at = mid + a * (mid - sphereSource(mid, p.refraction));
                    (at < t ? lo : hi) = mid;
                }
                source = 0.5 * (lo + hi);
            }
            const auto k = r > 0.0 ? source / t : 1.0;
            out.x = p.centreX + dx * k;
            out.y = p.centreY + dy * k;
        } else {
            const auto phase = p.phaseDegrees / 360.0 + p.speed * p.seconds;
            auto move = p.amplitude * std::sin(2.0 * kPi * (r / p.wavelength - phase)) *
                        fade(t, falloff) * smooth01(2.0 * r / p.wavelength);
            if (p.pulseRadius != 0.0 || p.pulseSpeed != 0.0) {
                const auto front = std::max(0.0, p.pulseRadius + p.pulseSpeed * p.seconds);
                move *= smooth01((front - r) / p.wavelength);
            }
            out.x = r > 0.0 ? px + dx / r * move : px;
            out.y = r > 0.0 ? py + dy / r * move : py;
        }
        return out;
    }
    case ProceduralWarpMode::Wave: {
        const auto alpha = p.directionDegrees * kPi / 180.0;
        // The direction it travels (clockwise from up, +y up) and the move a quarter turn
        // counter-clockwise from it.
        const double tx = std::sin(alpha);
        const double ty = std::cos(alpha);
        const double nx = -ty;
        const double ny = tx;
        const auto u = (px * tx + py * ty) / p.wavelength -
                       (p.phaseDegrees / 360.0 + p.speed * p.seconds);
        const auto f = u - std::floor(u);
        double w = 0.0;
        switch (p.wave) {
        case core::field::ProceduralWarpWave::Sine:
            w = std::sin(2.0 * kPi * u);
            break;
        case core::field::ProceduralWarpWave::Triangle:
            // 0 at a whole cycle, 1 a quarter in, -1 three quarters in.
            w = f < 0.25 ? 4.0 * f : (f < 0.75 ? 2.0 - 4.0 * f : 4.0 * f - 4.0);
            break;
        case core::field::ProceduralWarpWave::Square:
            w = f < 0.5 ? 1.0 : -1.0;
            out.step = near(f, 0.5) || near(f, 0.0) || near(f, 1.0);
            break;
        case core::field::ProceduralWarpWave::Saw:
            w = f < 0.5 ? 2.0 * f : 2.0 * f - 2.0;
            out.step = near(f, 0.5);
            break;
        }
        double move = p.amplitude * w;
        const auto halfW = p.frameWidth / 2.0;
        const auto halfH = p.frameHeight / 2.0;
        if ((p.pinnedEdges & core::field::kWarpPinLeft) != 0)
            move *= smooth01((px + halfW) / p.wavelength);
        if ((p.pinnedEdges & core::field::kWarpPinRight) != 0)
            move *= smooth01((halfW - px) / p.wavelength);
        if ((p.pinnedEdges & core::field::kWarpPinTop) != 0)
            move *= smooth01((halfH - py) / p.wavelength);
        if ((p.pinnedEdges & core::field::kWarpPinBottom) != 0)
            move *= smooth01((py + halfH) / p.wavelength);
        // The picture moves along the normal, so the pixel reads where it came from.
        out.x = px - nx * move;
        out.y = py - ny * move;
        return out;
    }
    case ProceduralWarpMode::Polar: {
        const auto w = p.frameWidth;
        const auto h = p.frameHeight;
        const auto pole = std::min(w, h) / 2.0;
        double cx = 0.0;
        double cy = 0.0;
        if (!p.polarToRect) {
            // The angle clockwise from up as a fraction of a turn, the left edge at 0, the top row
            // at the pole and the bottom row at the polar radius.
            auto a = std::atan2(px, py) / (2.0 * kPi);
            if (a < 0.0)
                a += 1.0;
            const auto distance = std::hypot(px, py);
            out.step = near(a, 0.0) || near(a, 1.0) || distance < 1.0e-3;
            cx = -w / 2.0 + a * w;
            cy = h / 2.0 - distance / pole * h;
        } else {
            const auto u = (px + w / 2.0) / w;
            const auto v = (h / 2.0 - py) / h;
            cx = v * pole * std::sin(2.0 * kPi * u);
            cy = v * pole * std::cos(2.0 * kPi * u);
        }
        const auto k = std::clamp(p.interpolation, 0.0, 1.0);
        out.x = px + k * (cx - px);
        out.y = py + k * (cy - py);
        return out;
    }
    case ProceduralWarpMode::Magnify: {
        const auto distance = p.squareShape ? std::max(std::abs(dx), std::abs(dy)) : r;
        out.weight = 0.0;
        out.step = near(distance, p.radius) && !(p.feather > 0.0);
        if (!(p.radius > 0.0) || distance >= p.radius)
            return out;
        const auto pull = std::clamp(p.pull, -1.0, 1.0) * smooth01((p.radius - distance) / p.radius);
        out.x = p.centreX + dx / p.magnification - pull * (p.secondX - p.centreX);
        out.y = p.centreY + dy / p.magnification - pull * (p.secondY - p.centreY);
        out.weight = p.feather > 0.0 ? smooth01((p.radius - distance) / p.feather) : 1.0;
        return out;
    }
    case ProceduralWarpMode::Warp: {
        const auto halfW = p.frameWidth / 2.0;
        const auto halfH = p.frameHeight / 2.0;
        const auto circle = std::min(p.frameWidth, p.frameHeight) / 2.0;
        const auto b = std::clamp(p.bend, -1.0, 1.0);
        const auto h = std::clamp(p.horizontalDistortion, -1.0, 1.0);
        const auto vd = std::clamp(p.verticalDistortion, -1.0, 1.0);
        const auto u = px / halfW;
        const auto v = py / halfH;
        const auto u1 = u / (1.0 + 0.5 * h * std::clamp(v, -1.0, 1.0));
        const auto v1 = v / (1.0 + 0.5 * vd * std::clamp(u, -1.0, 1.0));
        double u2 = 0.0;
        double v2 = 0.0;
        warpStyle(p.warpStyle, b, u1, v1, halfW, halfH, circle, u2, v2);
        out.x = px + (u2 - u) * halfW;
        out.y = py + (v2 - v) * halfH;
        return out;
    }
    case ProceduralWarpMode::Bend: {
        const auto alpha = p.directionDegrees * kPi / 180.0;
        const double tx = std::sin(alpha);
        const double ty = std::cos(alpha);
        const auto length = std::max(p.height, 0.0);
        if (!(length > 0.0))
            return out;
        const auto w = (dx * tx + dy * ty) / (length / 2.0);
        if (std::abs(w) >= 1.0)
            return out;
        const auto move = std::clamp(p.bend, -1.0, 1.0) * length / 2.0 * (1.0 - w * w) * (1.0 - w * w);
        // The picture moves along the normal (-ty, tx), so the pixel reads where it came from.
        out.x = px - (-ty) * move;
        out.y = py - tx * move;
        return out;
    }
    case ProceduralWarpMode::Split: {
        const auto lx = p.secondX - p.centreX;
        const auto ly = p.secondY - p.centreY;
        const auto length = std::hypot(lx, ly);
        if (!(length > 0.0))
            return out;
        const auto nx = -ly / length;
        const auto ny = lx / length;
        const auto sd = dx * nx + dy * ny;
        const auto distance = std::max(p.splitDistance, 0.0);
        const auto gap = distance / 2.0;
        const auto gapRight = p.asymmetric ? std::max(p.oppositeDistance, 0.0) / 2.0 : gap;
        out.step = near(sd, gap) || near(-sd, gapRight) || near(sd, distance);
        if (p.mirror) {
            if (sd < distance) {
                out.x = px - 2.0 * (sd - distance) * nx;
                out.y = py - 2.0 * (sd - distance) * ny;
            }
            return out;
        }
        if (sd < gap && -sd < gapRight) {
            out.weight = 0.0;
        } else if (sd > gap) {
            out.x = px - gap * nx;
            out.y = py - gap * ny;
        } else if (sd < -gapRight) {
            out.x = px + gapRight * nx;
            out.y = py + gapRight * ny;
        }
        return out;
    }
    case ProceduralWarpMode::Turbulent: {
        const auto length = std::max({std::abs(p.noiseSizeX), std::abs(p.noiseSizeY), 1.0e-6});
        const auto w = pinWeight(p.pinnedEdges, px, py, p.frameWidth / 2.0, p.frameHeight / 2.0, length);
        out.x = px + p.displacement * w * noiseX;
        out.y = py + p.displacement * w * noiseY;
        return out;
    }
    case ProceduralWarpMode::Drizzle: {
        if (!(p.radius > 0.0))
            return out;
        const auto seed = static_cast<std::uint32_t>(p.seed);
        const auto phase = p.phaseDegrees / 360.0 + p.speed * p.seconds;
        double mx = 0.0;
        double my = 0.0;
        for (std::int64_t i = 0; i < p.drops; ++i) {
            const auto n = static_cast<std::uint32_t>(i);
            const auto cx = (2.0 * unitOf(pcg4dFirst(n, 0u, 0u, seed)) - 1.0) * p.frameWidth / 2.0;
            const auto cy = (2.0 * unitOf(pcg4dFirst(n, 1u, 0u, seed)) - 1.0) * p.frameHeight / 2.0;
            const auto offset = unitOf(pcg4dFirst(n, 2u, 0u, seed));
            const auto age = std::fmod(std::fmod(phase, 1.0) + 1.0 + offset, 1.0);
            const auto front = age * p.radius;
            const auto ddx = px - cx;
            const auto ddy = py - cy;
            const auto ring = std::hypot(ddx, ddy);
            if (!(ring < front) || !(ring > 0.0))
                continue;
            const auto behind = (front - ring) / p.wavelength;
            const auto move = p.amplitude * (1.0 - age) * smooth01(behind) * std::sin(-2.0 * kPi * behind);
            mx += ddx / ring * move;
            my += ddy / ring * move;
        }
        out.x = px + mx;
        out.y = py + my;
        return out;
    }
    }
    return out;
}

} // namespace bloom::procedural_warp_reference
