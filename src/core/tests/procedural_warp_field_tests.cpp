// #510 Procedural Warp: the field kernel's Procedural Warp instruction (field_kernel.glsl,
// "Procedural warp") against an independent binary64 reading of each mode's documented inverse
// map (docs/architecture/fields.md, "Procedural warp"), written from the specification with the
// standard library's own sine, arc tangent, square root and power. The kernel evaluates the same
// table on the CPU and on the GPU with the same binary32 operations, so this is the CPU reference's
// own accuracy; the native harness holds the device to it.
//
// Also: each mode is the identity at zero strength, exactly; nothing is NaN at the centre, at a
// radius of 0 or at a far position; the table validates and an unknown mode does not.

#include "procedural_warp_reference.hpp"

#include <bloom/core/field_program.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <numbers>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace field = bloom::core::field;
using field::FieldOp;
using field::ProceduralWarpMode;
using field::ProceduralWarpParameters;

class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

using bloom::procedural_warp_reference::kPi;
constexpr field::FieldImages kNoImages{};

// r0 = coordinate, r1 = Procedural Warp(r0); the result is r1, a vector.
std::optional<std::vector<float>> warpTable(const ProceduralWarpParameters& p) {
    const auto block = field::encodeProceduralWarp(p);
    if (!block)
        return std::nullopt;
    field::FieldProgramBuilder builder;
    builder.emit(FieldOp::Coordinate, 0, field::kNoFieldInputs, {});
    builder.emit(FieldOp::ProceduralWarp, 1, {0, -1, -1, -1}, *block);
    return builder.finish(1, field::FieldResultKind::Vector, field::FieldOutputMode::Raw);
}

// Turbulent: the warp's own table (the instruction carries the field Noise operation's two streams)
// and, to read the noise the warp sees, a program of the same two Noise operations as a vector.
struct TurbulentTables final {
    std::vector<float> warp;
    std::vector<float> noise;
};

std::optional<TurbulentTables> turbulentTables(const ProceduralWarpParameters& p) {
    const auto block = field::encodeProceduralWarp(p);
    const auto a = field::encodeNoise(field::turbulentNoise(p, 0));
    const auto b = field::encodeNoise(field::turbulentNoise(p, 1));
    if (!block || !a || !b)
        return std::nullopt;
    TurbulentTables out;
    {
        field::FieldProgramBuilder builder;
        builder.emit(FieldOp::Coordinate, 0, field::kNoFieldInputs, {});
        builder.emit(FieldOp::ProceduralWarp, 1, {0, -1, -1, -1}, *block);
        auto table = builder.finish(1, field::FieldResultKind::Vector, field::FieldOutputMode::Raw);
        if (!table)
            return std::nullopt;
        out.warp = std::move(*table);
    }
    {
        field::FieldProgramBuilder builder;
        builder.emit(FieldOp::Coordinate, 0, field::kNoFieldInputs, {});
        builder.emit(FieldOp::Noise, 1, {0, -1, -1, -1}, *a);
        builder.emit(FieldOp::Noise, 2, {0, -1, -1, -1}, *b);
        builder.emit(FieldOp::Combine, 3, {1, 2, -1, -1}, field::encodeConstant({0.0, 0.0, 0.0, 0.0}));
        auto table = builder.finish(3, field::FieldResultKind::Vector, field::FieldOutputMode::Raw);
        if (!table)
            return std::nullopt;
        out.noise = std::move(*table);
    }
    return out;
}

std::array<double, 3> kernelAt(const std::vector<float>& table, const double x, const double y) {
    const auto v =
        field::evaluateField(table, kNoImages, static_cast<float>(x), static_cast<float>(y));
    return {static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2])};
}

using bloom::procedural_warp_reference::reference;

// --- The checks ---------------------------------------------------------------------------------

// The worst position error of the kernel against the reading over a grid of the 1920 x 1080 frame
// and a ring of points close to the centre (and the centre itself); every value finite.
struct Compare final {
    double worst = 0.0;
    double worstWeight = 0.0;
    std::size_t compared = 0;
    bool finite = true;
};

Compare compare(const ProceduralWarpParameters& p) {
    Compare out;
    const auto table = warpTable(p);
    if (!table || !field::validFieldTable(*table)) {
        out.finite = false;
        return out;
    }
    std::vector<std::array<double, 2>> points;
    for (int j = 0; j <= 54; ++j)
        for (int i = 0; i <= 96; ++i)
            points.push_back({-960.0 + 20.0 * i + 0.37, -540.0 + 20.0 * j + 0.61});
    for (int k = 0; k < 64; ++k) {
        const auto a = 2.0 * kPi * k / 64.0;
        for (const double radius : {0.0, 1.0e-3, 0.5, 3.0, 17.0})
            points.push_back({p.centreX + radius * std::cos(a), p.centreY + radius * std::sin(a)});
    }
    for (const auto& [x, y] : points) {
        // The reading of the binary32 position the kernel sees.
        const auto fx = static_cast<double>(static_cast<float>(x));
        const auto fy = static_cast<double>(static_cast<float>(y));
        const auto got = kernelAt(*table, fx, fy);
        out.finite = out.finite && std::isfinite(got[0]) && std::isfinite(got[1]) &&
                     std::isfinite(got[2]);
        const auto want = reference(p, fx, fy);
        if (want.step)
            continue;
        ++out.compared;
        out.worst = std::max({out.worst, std::abs(got[0] - want.x), std::abs(got[1] - want.y)});
        out.worstWeight = std::max(out.worstWeight, std::abs(got[2] - want.weight));
    }
    return out;
}

void expectMatches(Expectations& e, const ProceduralWarpParameters& p, const std::string& what,
                   const double tolerance = 2.0e-3) {
    const auto result = compare(p);
    e.expect(result.finite, what + ": the table encodes and every value is finite");
    e.expect(result.compared > 3000, what + ": the positions were compared");
    e.expect(result.worst <= tolerance,
             what + ": the kernel reads the documented position (worst " +
                 std::to_string(result.worst) + " pixels)");
    if (p.mode == ProceduralWarpMode::Magnify || p.mode == ProceduralWarpMode::Split)
        e.expect(result.worstWeight <= 1.0e-4,
                 what + ": the Magnify weight is the documented one (worst " +
                     std::to_string(result.worstWeight) + ")");
}

ProceduralWarpParameters base(const ProceduralWarpMode mode) {
    ProceduralWarpParameters p;
    p.mode = mode;
    p.centreX = 83.0;
    p.centreY = -41.0;
    p.radius = 420.0;
    return p;
}

void testTwirl(Expectations& e) {
    for (const double angle : {90.0, -270.0, 1080.0})
        for (const double falloff : {1.0, 0.4}) {
            auto p = base(ProceduralWarpMode::Twirl);
            p.angleDegrees = angle;
            p.falloff = falloff;
            // Three turns over 40% of the radius: the turn changes by a pixel's worth of position
            // every 0.02 pixel of radius, so binary32's own radius sets the error.
            expectMatches(e, p,
                          "Twirl " + std::to_string(angle) + " falloff " + std::to_string(falloff),
                          std::abs(angle) > 360.0 ? 5.0e-3 : 2.0e-3);
        }
    auto hard = base(ProceduralWarpMode::Twirl);
    hard.angleDegrees = 45.0;
    hard.falloff = 0.0;
    expectMatches(e, hard, "Twirl with no falloff (a turned disc)");
}

void testBulgePinch(Expectations& e) {
    for (const double amount : {1.0, 0.5, -0.5, -1.0})
        for (const double falloff : {0.0, 1.0}) {
            auto p = base(ProceduralWarpMode::BulgePinch);
            p.amount = amount;
            p.falloff = falloff;
            // A full pinch reads the centre's ring from a ring 2 / pi of the radius out: its
            // positions next to the centre are far from it, steep, and binary32 places them to a
            // few thousandths of a pixel only.
            expectMatches(e, p, "Bulge / Pinch " + std::to_string(amount) + " falloff " +
                                    std::to_string(falloff),
                          amount <= -1.0 ? 2.0e-2 : 2.0e-3);
        }
}

void testSpherize(Expectations& e) {
    for (const double amount : {1.0, 0.6, -0.6, -1.0})
        for (const double index : {1.0, 1.5, 4.0}) {
            auto p = base(ProceduralWarpMode::Spherize);
            p.amount = amount;
            p.refraction = index;
            expectMatches(e, p, "Spherize " + std::to_string(amount) + " index " +
                                    std::to_string(index));
        }
    // The two signs are inverses: the position a negative amount reads, read again by the
    // positive one, is the pixel itself (to the bisection's precision).
    auto forward = base(ProceduralWarpMode::Spherize);
    forward.amount = 0.8;
    auto inverse = forward;
    inverse.amount = -0.8;
    const auto a = warpTable(forward);
    const auto b = warpTable(inverse);
    double worst = 0.0;
    if (a && b)
        for (int k = 0; k < 200; ++k) {
            const auto x = forward.centreX + 2.0 * (k - 100);
            const auto y = forward.centreY + 1.3 * (k - 100);
            const auto q = kernelAt(*b, x, y);
            const auto back = kernelAt(*a, q[0], q[1]);
            worst = std::max({worst, std::abs(back[0] - x), std::abs(back[1] - y)});
        }
    e.expect(a && b && worst < 5.0e-3,
             "Spherize: a negative amount is the inverse of the positive one (worst " +
                 std::to_string(worst) + " pixels)");
}

void testRipple(Expectations& e) {
    for (const double falloff : {1.0, 0.0})
        for (const double speed : {0.0, 0.75}) {
            auto p = base(ProceduralWarpMode::Ripple);
            p.wavelength = 37.0;
            p.amplitude = 9.0;
            p.phaseDegrees = 40.0;
            p.speed = speed;
            p.seconds = 1.6;
            p.falloff = falloff;
            expectMatches(e, p, "Ripple falloff " + std::to_string(falloff) + " speed " +
                                    std::to_string(speed));
        }
}

void testWave(Expectations& e) {
    for (int kind = 0; kind < 4; ++kind)
        for (const double direction : {90.0, 0.0, 225.0}) {
            auto p = base(ProceduralWarpMode::Wave);
            p.wave = static_cast<field::ProceduralWarpWave>(kind);
            p.directionDegrees = direction;
            p.wavelength = 120.0;
            p.amplitude = 14.0;
            p.phaseDegrees = 30.0;
            p.speed = 0.5;
            p.seconds = 0.84;
            expectMatches(e, p, "Wave " + std::to_string(kind) + " direction " +
                                    std::to_string(direction));
        }
    for (const std::uint8_t pins : {std::uint8_t{15}, std::uint8_t{1}, std::uint8_t{2},
                                    std::uint8_t{4}, std::uint8_t{8}, std::uint8_t{3},
                                    std::uint8_t{12}}) {
        auto p = base(ProceduralWarpMode::Wave);
        p.wavelength = 150.0;
        p.amplitude = 20.0;
        p.pinnedEdges = pins;
        expectMatches(e, p, "Wave pinned " + std::to_string(pins));
        // A pinned edge does not move.
        const auto table = warpTable(p);
        if (!table)
            continue;
        double edge = 0.0;
        for (int k = -20; k <= 20; ++k) {
            if ((pins & field::kWarpPinLeft) != 0) {
                const auto q = kernelAt(*table, -960.0, 20.0 * k);
                edge = std::max({edge, std::abs(q[0] + 960.0), std::abs(q[1] - 20.0 * k)});
            }
            if ((pins & field::kWarpPinTop) != 0) {
                const auto q = kernelAt(*table, 40.0 * k, 540.0);
                edge = std::max({edge, std::abs(q[0] - 40.0 * k), std::abs(q[1] - 540.0)});
            }
        }
        e.expect(edge == 0.0, "Wave: a pinned edge does not move");
    }
}

void testPolar(Expectations& e) {
    for (const bool back : {false, true})
        for (const double amount : {1.0, 0.35}) {
            auto p = base(ProceduralWarpMode::Polar);
            p.polarToRect = back;
            p.interpolation = amount;
            expectMatches(e, p, std::string(back ? "Polar to rectangular" : "Rectangular to polar") +
                                    " amount " + std::to_string(amount),
                          4.0e-3);
        }
    // The two directions are inverses inside the polar disc.
    auto toPolar = base(ProceduralWarpMode::Polar);
    auto toRect = toPolar;
    toRect.polarToRect = true;
    const auto a = warpTable(toPolar);
    const auto b = warpTable(toRect);
    double worst = 0.0;
    if (a && b)
        for (int k = 0; k < 300; ++k) {
            // Inside the rectangle, away from its left and right edges (the polar seam).
            const auto x = -900.0 + 6.0 * k;
            const auto y = 500.0 - 3.3 * k;
            const auto q = kernelAt(*b, x, y);
            const auto back = kernelAt(*a, q[0], q[1]);
            worst = std::max({worst, std::abs(back[0] - x), std::abs(back[1] - y)});
        }
    e.expect(a && b && worst < 2.0e-2,
             "Polar: the two directions are inverses (worst " + std::to_string(worst) + " pixels)");
}

void testMagnify(Expectations& e) {
    for (const bool square : {false, true})
        for (const double feather : {0.0, 60.0}) {
            auto p = base(ProceduralWarpMode::Magnify);
            p.radius = 230.0;
            p.magnification = 2.5;
            p.squareShape = square;
            p.feather = feather;
            expectMatches(e, p, std::string("Magnify ") + (square ? "square" : "circle") +
                                    " feather " + std::to_string(feather));
        }
}


void testMagnifyPull(Expectations& e) {
    for (const bool square : {false, true})
        for (const double pull : {0.6, -0.4}) {
            auto p = base(ProceduralWarpMode::Magnify);
            p.radius = 230.0;
            p.magnification = 2.0;
            p.squareShape = square;
            p.feather = 40.0;
            p.pull = pull;
            p.secondX = 410.0;
            p.secondY = 150.0;
            expectMatches(e, p, std::string("Magnify pull ") + (square ? "square " : "circle ") +
                                    std::to_string(pull));
        }
    // The pull is strongest at the centre: the centre reads the pulled position, an edge pixel not.
    auto p = base(ProceduralWarpMode::Magnify);
    p.radius = 200.0;
    p.pull = 1.0;
    p.secondX = p.centreX + 100.0;
    p.secondY = p.centreY;
    const auto table = warpTable(p);
    if (table) {
        const auto centre = kernelAt(*table, p.centreX, p.centreY);
        e.expect(std::abs(centre[0] - (p.centreX - 100.0)) < 1.0e-3 &&
                     std::abs(centre[1] - p.centreY) < 1.0e-3,
                 "Magnify: a full pull reads the picture the whole distance back at the centre");
    }
}

void testRipplePulse(Expectations& e) {
    for (const double speed : {0.0, 80.0}) {
        auto p = base(ProceduralWarpMode::Ripple);
        p.wavelength = 31.0;
        p.amplitude = 8.0;
        p.pulseRadius = 150.0;
        p.pulseSpeed = speed;
        p.seconds = 1.25;
        expectMatches(e, p, "Ripple pulse speed " + std::to_string(speed));
        // Outside the wavefront the ripple does nothing.
        const auto table = warpTable(p);
        const auto front = 150.0 + speed * 1.25;
        if (table && front < p.radius - 5.0) {
            const auto q = kernelAt(*table, p.centreX + front + 3.0, p.centreY + 1.5);
            e.expect(q[0] == p.centreX + front + 3.0 && q[1] == p.centreY + 1.5,
                     "Ripple pulse: nothing moves beyond the wavefront");
        }
    }
}

void testWarpStyles(Expectations& e) {
    for (int style = 0; style < field::kProceduralWarpStyleCount; ++style)
        for (const double bend : {0.7, -0.5})
            for (const std::array<double, 2> skew : {std::array{0.0, 0.0}, std::array{0.6, -0.4}}) {
                auto p = base(ProceduralWarpMode::Warp);
                p.warpStyle = static_cast<std::uint8_t>(style);
                p.bend = bend;
                p.horizontalDistortion = skew[0];
                p.verticalDistortion = skew[1];
                expectMatches(e, p, "Warp style " + std::to_string(style) + " bend " +
                                        std::to_string(bend) + " skew " +
                                        std::to_string(skew[0]));
            }
}

void testBend(Expectations& e) {
    for (const double direction : {90.0, 0.0, 215.0})
        for (const double bend : {0.8, -0.35}) {
            auto p = base(ProceduralWarpMode::Bend);
            p.directionDegrees = direction;
            p.bend = bend;
            p.height = 520.0;
            expectMatches(e, p, "Bend direction " + std::to_string(direction) + " bend " +
                                    std::to_string(bend));
        }
    // The stretch ends smoothly: nothing moves at and beyond half the length from the centre.
    auto p = base(ProceduralWarpMode::Bend);
    p.bend = 1.0;
    p.height = 400.0;
    const auto table = warpTable(p);
    if (table) {
        const auto q = kernelAt(*table, p.centreX + 200.0, p.centreY + 30.0);
        const auto r = kernelAt(*table, p.centreX + 340.0, p.centreY + 30.0);
        e.expect(q[0] == p.centreX + 200.0 && q[1] == p.centreY + 30.0 && r[1] == p.centreY + 30.0,
                 "Bend: nothing moves at the ends of the bent stretch");
    }
}

void testSplit(Expectations& e) {
    for (const bool mirror : {false, true})
        for (const double distance : {0.0, 60.0, 140.0}) {
            auto p = base(ProceduralWarpMode::Split);
            p.secondX = p.centreX + 120.0;
            p.secondY = p.centreY + 280.0;
            p.splitDistance = distance;
            p.mirror = mirror;
            expectMatches(e, p, std::string("Split ") + (mirror ? "mirror " : "gap ") +
                                    std::to_string(distance));
        }
    // The gap is empty, and two equal points are no line.
    auto p = base(ProceduralWarpMode::Split);
    p.secondX = p.centreX;
    p.secondY = p.centreY + 100.0;
    p.splitDistance = 50.0;
    auto table = warpTable(p);
    if (table) {
        const auto in = kernelAt(*table, p.centreX + 10.0, p.centreY + 20.0);
        const auto out = kernelAt(*table, p.centreX + 40.0, p.centreY + 20.0);
        e.expect(in[2] == 0.0, "Split: the gap has the weight 0");
        e.expect(out[2] == 1.0 && std::abs(out[0] - (p.centreX + 15.0)) < 1.0e-3,
                 "Split: the picture beside the gap is drawn half the distance away");
    }
    p.secondY = p.centreY;
    table = warpTable(p);
    if (table) {
        const auto q = kernelAt(*table, p.centreX + 3.0, p.centreY + 4.0);
        e.expect(q[0] == p.centreX + 3.0 && q[1] == p.centreY + 4.0 && q[2] == 1.0,
                 "Split: two equal points do nothing");
    }
}

void testTurbulent(Expectations& e) {
    for (const std::uint8_t pins : {std::uint8_t{0}, std::uint8_t{15}, std::uint8_t{5}}) {
        auto p = base(ProceduralWarpMode::Turbulent);
        p.displacement = 48.0;
        p.noiseSizeX = 130.0;
        p.noiseSizeY = 90.0;
        p.noiseOffsetX = 21.0;
        p.noiseOffsetY = -13.0;
        p.seed = 7;
        p.evolution = 0.35;
        p.pinnedEdges = pins;
        const auto tables = turbulentTables(p);
        e.expect(tables.has_value() && field::validFieldTable(tables->warp), "Turbulent encodes");
        if (!tables)
            continue;
        double worst = 0.0;
        std::size_t compared = 0;
        for (int j = 0; j <= 27; ++j)
            for (int i = 0; i <= 48; ++i) {
                const auto x = static_cast<double>(static_cast<float>(-960.0 + 40.0 * i + 0.37));
                const auto y = static_cast<double>(static_cast<float>(-540.0 + 40.0 * j + 0.61));
                const auto got = kernelAt(tables->warp, x, y);
                const auto noise = kernelAt(tables->noise, x, y);
                const auto want = reference(p, x, y, noise[0], noise[1]);
                worst = std::max({worst, std::abs(got[0] - want.x), std::abs(got[1] - want.y)});
                ++compared;
            }
        e.expect(compared > 1000 && worst <= 2.0e-3,
                 "Turbulent pins " + std::to_string(pins) + ": the kernel adds the pinned noise "
                 "(worst " + std::to_string(worst) + " pixels)");
        // A pinned edge does not move.
        if ((pins & field::kWarpPinLeft) != 0) {
            const auto q = kernelAt(tables->warp, -960.0, 100.0);
            e.expect(q[0] == -960.0 && q[1] == 100.0, "Turbulent: a pinned edge does not move");
        }
    }
    // The same seed and evolution read the same; another seed or another evolution does not.
    auto p = base(ProceduralWarpMode::Turbulent);
    p.displacement = 40.0;
    p.seed = 7;
    p.evolution = 0.35;
    const auto a = turbulentTables(p);
    const auto same = turbulentTables(p);
    auto otherSeed = p;
    otherSeed.seed = 8;
    const auto seed = turbulentTables(otherSeed);
    auto otherEvolution = p;
    otherEvolution.evolution = 0.60;
    const auto evolved = turbulentTables(otherEvolution);
    if (a && same && seed && evolved) {
        double dSame = 0.0;
        double dSeed = 0.0;
        double dEvolution = 0.0;
        for (int k = 0; k < 200; ++k) {
            const auto x = -400.0 + 4.1 * k;
            const auto y = 130.0 - 2.3 * k;
            const auto q = kernelAt(a->warp, x, y);
            dSame = std::max(dSame, std::abs(q[0] - kernelAt(same->warp, x, y)[0]));
            dSeed = std::max(dSeed, std::abs(q[0] - kernelAt(seed->warp, x, y)[0]));
            dEvolution = std::max(dEvolution, std::abs(q[0] - kernelAt(evolved->warp, x, y)[0]));
        }
        e.expect(dSame == 0.0, "Turbulent: the same seed and evolution read the same positions");
        e.expect(dSeed > 0.5, "Turbulent: another seed moves the pixels differently");
        e.expect(dEvolution > 0.5, "Turbulent: another evolution moves the pixels differently");
    }
}

void testDrizzle(Expectations& e) {
    for (const std::int64_t seed : {std::int64_t{1}, std::int64_t{99991}, std::int64_t{16777215}})
        for (const std::int64_t drops : {std::int64_t{1}, std::int64_t{12}, std::int64_t{64}}) {
            auto p = base(ProceduralWarpMode::Drizzle);
            p.radius = 260.0;
            p.wavelength = 28.0;
            p.amplitude = 7.0;
            p.phaseDegrees = 77.0;
            p.speed = 0.4;
            p.seconds = 0.9;
            p.seed = seed;
            p.drops = drops;
            expectMatches(e, p, "Drizzle seed " + std::to_string(seed) + " drops " +
                                    std::to_string(drops));
        }
    // Another seed puts the drops elsewhere.
    auto a = base(ProceduralWarpMode::Drizzle);
    a.amplitude = 9.0;
    a.phaseDegrees = 140.0;
    auto b = a;
    b.seed = 2;
    const auto ta = warpTable(a);
    const auto tb = warpTable(b);
    double different = 0.0;
    if (ta && tb)
        for (int j = -20; j <= 20; ++j)
            for (int i = -30; i <= 30; ++i) {
                const auto x = 30.0 * i + 0.3;
                const auto y = 25.0 * j + 0.2;
                different = std::max(different, std::abs(kernelAt(*ta, x, y)[0] -
                                                         kernelAt(*tb, x, y)[0]));
            }
    e.expect(different > 0.1, "Drizzle: another seed places the drops differently");
}

// Zero strength is the identity exactly: the move is added to the pixel's own position.
void testIdentity(Expectations& e) {
    std::vector<ProceduralWarpParameters> cases;
    auto twirl = base(ProceduralWarpMode::Twirl);
    twirl.angleDegrees = 0.0;
    cases.push_back(twirl);
    auto bulge = base(ProceduralWarpMode::BulgePinch);
    bulge.amount = 0.0;
    cases.push_back(bulge);
    auto sphere = base(ProceduralWarpMode::Spherize);
    sphere.amount = 0.0;
    cases.push_back(sphere);
    auto ripple = base(ProceduralWarpMode::Ripple);
    ripple.amplitude = 0.0;
    cases.push_back(ripple);
    auto wave = base(ProceduralWarpMode::Wave);
    wave.amplitude = 0.0;
    cases.push_back(wave);
    auto polar = base(ProceduralWarpMode::Polar);
    polar.interpolation = 0.0;
    cases.push_back(polar);
    auto magnify = base(ProceduralWarpMode::Magnify);
    magnify.magnification = 1.0;
    cases.push_back(magnify);
    auto warp = base(ProceduralWarpMode::Warp);
    warp.warpStyle = 9;
    warp.bend = 0.0;
    cases.push_back(warp);
    auto bend = base(ProceduralWarpMode::Bend);
    bend.bend = 0.0;
    cases.push_back(bend);
    auto split = base(ProceduralWarpMode::Split);
    split.secondX = split.centreX + 50.0;
    split.secondY = split.centreY + 30.0;
    split.splitDistance = 0.0;
    cases.push_back(split);
    auto turbulent = base(ProceduralWarpMode::Turbulent);
    turbulent.displacement = 0.0;
    cases.push_back(turbulent);
    auto drizzle = base(ProceduralWarpMode::Drizzle);
    drizzle.amplitude = 0.0;
    cases.push_back(drizzle);
    for (const auto& p : cases) {
        const auto table = warpTable(p);
        bool same = table.has_value();
        for (int j = -27; j <= 27 && same; ++j)
            for (int i = -48; i <= 48 && same; ++i) {
                const auto x = 20.0 * i + 0.37;
                const auto y = 20.0 * j - 0.21;
                const auto q = kernelAt(*table, static_cast<double>(static_cast<float>(x)),
                                        static_cast<double>(static_cast<float>(y)));
                same = q[0] == static_cast<double>(static_cast<float>(x)) &&
                       q[1] == static_cast<double>(static_cast<float>(y));
            }
        e.expect(same, "mode " + std::to_string(static_cast<int>(p.mode)) +
                           ": zero strength reads the pixel itself, exactly");
    }
}

// The centre, a radius of 0 and far positions are finite.
void testGuards(Expectations& e) {
    for (std::uint8_t mode = 0; mode < field::kProceduralWarpModeCount; ++mode) {
        auto p = base(static_cast<ProceduralWarpMode>(mode));
        p.angleDegrees = 300.0;
        p.amount = mode == 1 ? -1.0 : 1.0;
        p.amplitude = 30.0;
        for (const double radius : {420.0, 0.0}) {
            p.radius = radius;
            const auto table = warpTable(p);
            e.expect(table.has_value(), "the table encodes");
            if (!table)
                continue;
            bool finite = true;
            for (const auto& [x, y] : std::array<std::array<double, 2>, 6>{
                     {{p.centreX, p.centreY},
                      {p.centreX + 1.0e-30, p.centreY},
                      {1.0e30, -1.0e30},
                      {-1.0e20, 3.0},
                      {5.0e15, 5.0e15},
                      {p.centreX + radius, p.centreY}}}) {
                const auto q = kernelAt(*table, x, y);
                finite = finite && std::isfinite(q[0]) && std::isfinite(q[1]) &&
                         std::isfinite(q[2]);
            }
            e.expect(finite, "mode " + std::to_string(mode) + " radius " +
                                 std::to_string(radius) +
                                 ": the centre, the edge and far positions are finite");
            if (radius == 0.0 && p.mode != ProceduralWarpMode::Wave &&
                p.mode != ProceduralWarpMode::Polar && p.mode != ProceduralWarpMode::Warp &&
                p.mode != ProceduralWarpMode::Bend && p.mode != ProceduralWarpMode::Split &&
                p.mode != ProceduralWarpMode::Turbulent) {
                const auto q = kernelAt(*table, 12.5, -7.25);
                e.expect(q[0] == 12.5 && q[1] == -7.25,
                         "mode " + std::to_string(mode) + ": a radius of 0 changes nothing");
            }
        }
    }
}

void testEncoding(Expectations& e) {
    auto p = base(ProceduralWarpMode::Ripple);
    e.expect(field::encodeProceduralWarp(p).has_value(), "a Ripple encodes");
    const auto refuse = [&](ProceduralWarpParameters bad, const char* what) {
        e.expect(!field::encodeProceduralWarp(bad).has_value(), what);
    };
    auto bad = p;
    bad.wavelength = 0.0;
    refuse(bad, "a wavelength of 0 is refused");
    bad = p;
    bad.refraction = 0.5;
    refuse(bad, "an index below 1 is refused");
    bad = p;
    bad.magnification = 0.0;
    refuse(bad, "a magnification of 0 is refused");
    bad = p;
    bad.centreX = std::nan("");
    refuse(bad, "a value that is not finite is refused");
    bad = p;
    bad.mode = static_cast<ProceduralWarpMode>(field::kProceduralWarpModeCount);
    refuse(bad, "an unknown mode is refused");
    bad = p;
    bad.frameWidth = 0.0;
    refuse(bad, "a frame under a pixel is refused");
    bad = p;
    bad.warpStyle = field::kProceduralWarpStyleCount;
    refuse(bad, "an unknown Warp style is refused");
    bad = base(ProceduralWarpMode::Turbulent);
    e.expect(field::encodeProceduralWarp(bad).has_value(), "a Turbulent encodes");
    bad.noiseSizeX = 0.0;
    refuse(bad, "a Turbulent noise size of 0 is refused");
    bad = p;
    bad.drops = 65;
    refuse(bad, "more than 64 drops are refused");
    bad = p;
    bad.drops = 0;
    refuse(bad, "no drops is refused");
    bad = p;
    bad.seed = 16777216;
    refuse(bad, "a seed beyond 2^24 is refused");
    // A table whose block names an unknown mode does not validate.
    auto table = warpTable(p);
    e.expect(table && field::validFieldTable(*table), "a Procedural Warp table validates");
    if (table) {
        const auto parameterBase = field::kFieldTableCode + 2U * 2U;
        (*table)[parameterBase * 4U] = static_cast<float>(field::kProceduralWarpModeCount);
        e.expect(!field::validFieldTable(*table), "a table with an unknown mode does not validate");
    }
    e.expect(field::fieldOpName(FieldOp::ProceduralWarp) == "procedural-warp",
             "the operation's coverage name");
}

} // namespace

int main() {
    Expectations e;
    testTwirl(e);
    testBulgePinch(e);
    testSpherize(e);
    testRipple(e);
    testWave(e);
    testPolar(e);
    testMagnify(e);
    testMagnifyPull(e);
    testRipplePulse(e);
    testWarpStyles(e);
    testBend(e);
    testSplit(e);
    testTurbulent(e);
    testDrizzle(e);
    testIdentity(e);
    testGuards(e);
    testEncoding(e);
    if (e.failures() != 0) {
        std::cerr << e.failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "procedural warp field: all checks passed\n";
    return 0;
}
