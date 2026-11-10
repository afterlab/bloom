#pragma once

// #510 Procedural Warp: an independent binary64 reading of each mode's documented inverse map
// (docs/architecture/fields.md, "Procedural warp"), written from the specification with the
// standard library's own sine, arc tangent, square root and power. Shared by the field kernel's own
// test and the snapshot compiler's pixel tests; it never reads the kernel.

#include <bloom/core/field_program.hpp>

#include <algorithm>
#include <cmath>
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

struct Reading final {
    double x = 0.0;
    double y = 0.0;
    double weight = 1.0;
    // The position lies on a discontinuity the map has by definition (an edge with no falloff, a
    // square or saw wave's step, the polar seam): a binary32 and a binary64 reading may fall on
    // either side of it.
    bool step = false;
};

inline Reading reference(const ProceduralWarpParameters& p, const double px, const double py) {
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
            const auto move = p.amplitude * std::sin(2.0 * kPi * (r / p.wavelength - phase)) *
                              fade(t, falloff) * smooth01(2.0 * r / p.wavelength);
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
        out.x = p.centreX + dx / p.magnification;
        out.y = p.centreY + dy / p.magnification;
        out.weight = p.feather > 0.0 ? smooth01((p.radius - distance) / p.feather) : 1.0;
        return out;
    }
    }
    return out;
}

} // namespace bloom::procedural_warp_reference
