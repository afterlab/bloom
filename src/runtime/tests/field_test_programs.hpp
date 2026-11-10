#pragma once

// #525: compiled field programs for the runtime tests and the GPU coverage fixtures, one per field
// operation, each the program a Fields node lowers to (runtime/field_raster.hpp has the operand
// orders). Every program reads a constant operand set; its parameter ids start at `idBase`.

#include <bloom/core/field_program.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom::field_test_programs {

using core::field::FieldOp;
using core::field::FieldOutputMode;
using core::field::FieldResultKind;
using document::ParameterId;
using document::Vec2d;
using runtime::CompiledScalarParameter;
using runtime::CompiledVec2Parameter;
using runtime::FieldInstruction;
using runtime::FieldProgram;

class Builder final {
  public:
    explicit Builder(const std::uint64_t idBase) : next_(idBase) {}

    // Appends `op` writing a fresh register from `inputs`, and returns that register.
    std::int8_t add(const FieldOp op, std::initializer_list<int> inputs = {},
                    std::vector<std::int64_t> choices = {}, std::initializer_list<double> scalars = {},
                    std::initializer_list<Vec2d> vectors = {}) {
        FieldInstruction instruction;
        instruction.op = static_cast<std::uint8_t>(op);
        instruction.output = static_cast<std::int8_t>(registers_++);
        std::size_t lane = 0;
        for (const auto input : inputs)
            instruction.inputs[lane++] = static_cast<std::int8_t>(input);
        instruction.choices = std::move(choices);
        for (const auto value : scalars)
            instruction.scalars.push_back({ParameterId::fromRaw(next_++), value});
        for (const auto value : vectors)
            instruction.vectors.push_back({ParameterId::fromRaw(next_++), value});
        program_.instructions.push_back(std::move(instruction));
        return program_.instructions.back().output;
    }
    FieldInstruction& last() { return program_.instructions.back(); }
    [[nodiscard]] ParameterId nextId() { return ParameterId::fromRaw(next_++); }

    FieldProgram finish(const int result, const FieldResultKind kind,
                        const FieldOutputMode mode = FieldOutputMode::Raw) {
        program_.result = static_cast<std::uint8_t>(result);
        program_.kind = static_cast<std::uint8_t>(kind);
        program_.mode = static_cast<std::uint8_t>(mode);
        program_.compositionSeconds = 5.0;
        return std::move(program_);
    }

  private:
    FieldProgram program_;
    std::uint64_t next_ = 0;
    int registers_ = 0;
};

// A Test Card program: the TestCard instruction of `card` over the Coordinate, in a `width` x `height`
// frame centerd on the composition's center, with numbers that make every card vary across it, as the
// premultiplied raster its lowering builds.
inline void addTestCard(Builder& b, const int coordinate, const std::int64_t card,
                        const double width, const double height, const std::int64_t variant = 0,
                        const std::int64_t option = 0, const bool label = false) {
    double v0 = 0.0;
    double v1 = 0.0;
    switch (card) {
    case 7:
        v0 = 4.0;
        v1 = 1.0;
        break;
    case 10:
        v0 = 4.0;
        break;
    case 12:
        v0 = 8.0;
        break;
    case 13:
        v0 = 3.0;
        v1 = 2.0;
        break;
    case 14:
        v0 = 5.0;
        break;
    default:
        break;
    }
    std::vector<std::int64_t> choices{card, variant, option, label ? 1 + (6 << 4) : 0};
    if (label)
        for (const std::int64_t glyph : {17, 5, 15, 17, 1, 3})
            choices.push_back(glyph);
    b.add(FieldOp::TestCard, {coordinate}, std::move(choices),
          {width, height, 0.0, 0.0, v0, v1, 0.0, 0.0, 24.0});
}

[[nodiscard]] inline FieldProgram testCardProgram(const std::uint64_t idBase,
                                                  const std::int64_t card,
                                                  const std::int64_t variant = 0,
                                                  const std::int64_t option = 0,
                                                  const bool label = false) {
    Builder b(idBase);
    const auto c = b.add(FieldOp::Coordinate);
    addTestCard(b, c, card, 16.0, 12.0, variant, option, label);
    return b.finish(b.last().output, FieldResultKind::Premultiplied);
}

// #524 Four-Color Gradient: the program its lowering builds over the 16 x 12 test composition,
// mirroring snapshot_compiler_four_color_gradient.ipp (the snapshot compiler test holds the two to
// the same instruction sequence). Four colors at four points, weights (d / nearest d)^-blend.
[[nodiscard]] inline FieldProgram fourColorGradientProgram(const std::uint64_t idBase,
                                                           const double blend,
                                                           const std::array<Vec2d, 4>& points,
                                                           const std::array<std::array<double, 4>, 4>& colors) {
    Builder b(idBase);
    const auto c = b.add(FieldOp::Coordinate);
    const auto math = [&](const std::int64_t op, const int a, const int x) {
        return b.add(FieldOp::Math, {a, x}, {op}, {0.1, 0.0});
    };
    const double squared = 16.0 * 16.0 + 12.0 * 12.0;
    const auto inverse = b.add(FieldOp::Constant, {}, {}, {1.0 / squared, 1.0 / squared, 1.0 / squared, 1.0 / squared});
    const auto half = b.add(FieldOp::Constant, {}, {}, {0.25 / squared, 0.25 / squared, 0.25 / squared, 0.25 / squared});
    const auto once = b.add(FieldOp::Constant, {}, {}, {blend, 0.0, 0.0, 0.0});
    const auto negative = b.add(FieldOp::Swizzle, {once}, {0, 0, 0, 0}, {-0.5, -0.5, -0.5, -0.5});
    std::array<int, 4> padded{};
    int nearest = -1;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto offset = b.add(FieldOp::Transform, {c}, {0, 6}, {0.0, 0.0}, {points[i]});
        const auto distance = math(13, offset, offset);
        const auto scaled = math(2, distance, inverse);
        padded[i] = math(0, scaled, half);
        nearest = nearest < 0 ? padded[i] : math(4, nearest, padded[i]);
    }
    int weightSum = -1;
    int colorSum = -1;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto relative = math(3, padded[i], nearest);
        const auto weight = math(6, relative, negative);
        const auto lanes = b.add(FieldOp::Swizzle, {weight}, {0, 0, 0, 0}, {1.0, 1.0, 1.0, 1.0});
        const auto color = b.add(FieldOp::Constant, {}, {}, {colors[i][0], colors[i][1], colors[i][2], colors[i][3]});
        const auto weighted = math(2, color, lanes);
        weightSum = weightSum < 0 ? lanes : math(0, weightSum, lanes);
        colorSum = colorSum < 0 ? weighted : math(0, colorSum, weighted);
    }
    const auto straight = math(3, colorSum, weightSum);
    const auto alpha = b.add(FieldOp::Swizzle, {straight}, {3, 3, 3, 5}, {1.0, 1.0, 1.0, 1.0});
    auto program = b.finish(math(2, straight, alpha), FieldResultKind::Premultiplied);
    // The lowering's register allocation: a register is free after its last read, so the sixteen
    // registers of a program suffice.
    auto& steps = program.instructions;
    std::vector<std::size_t> lastUse(64, 0);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        lastUse[static_cast<std::size_t>(steps[i].output)] =
            std::max(lastUse[static_cast<std::size_t>(steps[i].output)], i);
        for (const auto input : steps[i].inputs)
            if (input >= 0)
                lastUse[static_cast<std::size_t>(input)] = i;
    }
    lastUse[program.result] = steps.size();
    std::vector<int> physical(64, -1);
    std::vector<bool> busy(16, false);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto virtualOutput = static_cast<std::size_t>(steps[i].output);
        const auto inputs = steps[i].inputs;
        for (auto& input : steps[i].inputs)
            input = input < 0 ? std::int8_t{-1}
                              : static_cast<std::int8_t>(physical[static_cast<std::size_t>(input)]);
        for (const auto input : inputs)
            if (input >= 0 && lastUse[static_cast<std::size_t>(input)] == i &&
                physical[static_cast<std::size_t>(input)] >= 0)
                busy[static_cast<std::size_t>(physical[static_cast<std::size_t>(input)])] = false;
        const auto free = std::find(busy.begin(), busy.end(), false);
        *free = true;
        const auto reg = static_cast<int>(free - busy.begin());
        physical[virtualOutput] = reg;
        steps[i].output = static_cast<std::int8_t>(reg);
        if (lastUse[virtualOutput] <= i)
            *free = false;
    }
    program.result = static_cast<std::uint8_t>(physical[program.result]);
    return program;
}

[[nodiscard]] inline document::PathValue curvePoints(std::initializer_list<Vec2d> points) {
    document::PathValue path;
    for (const auto point : points)
        path.editableContour().anchors.push_back(
            document::PathAnchor{point, std::nullopt, std::nullopt});
    return path;
}

// A program for each field operation over a small frame (sizes of a few pixels, so every one
// varies across a 16 x 12 composition).
[[nodiscard]] inline FieldProgram operationProgram(const FieldOp op, const std::uint64_t idBase,
                                                   const int variant = 0) {
    Builder b(idBase);
    const auto c = b.add(FieldOp::Coordinate);
    const auto noise = [&](const int at, const std::int64_t type, const std::int64_t fractal) {
        return b.add(FieldOp::Noise, {at}, {type, fractal, 7, 0, 0, 0, 3, 0, 0},
                     {15.0, 4.0, 2.0, 0.5, 30.0, 1.0, 2.0, 0.25, 0.0, 1.0, 0.0},
                     {Vec2d{6.0, 5.0}, Vec2d{1.5, -2.0}, Vec2d{0.0, 0.0}});
    };
    switch (op) {
    case FieldOp::Coordinate:
        return b.finish(c, FieldResultKind::Vector);
    case FieldOp::Constant:
        return b.finish(b.add(FieldOp::Constant, {}, {}, {1.5, -0.25, 0.5, 0.75}),
                        FieldResultKind::Color);
    case FieldOp::Noise:
        return b.finish(noise(c, variant % 6, variant % 8), FieldResultKind::Scalar);
    case FieldOp::Voronoi:
        return b.finish(b.add(FieldOp::Voronoi, {c}, {variant % 5, variant % 3, 11, 2, 0, 0},
                              {5.0, 0.9, 0.3, 0.4}),
                        FieldResultKind::Scalar);
    case FieldOp::Random:
        return b.finish(b.add(FieldOp::Random, {c}, {3, variant % 2, 0, 2}, {0.5},
                              {Vec2d{3.0, 2.0}}),
                        FieldResultKind::Color);
    case FieldOp::Pattern:
        return b.finish(b.add(FieldOp::Pattern, {c}, {variant % 4, variant % 3},
                              {6.0, 0.4, 25.0, 0.1, 0.0, 1.0}, {Vec2d{1.0, -1.0}}),
                        FieldResultKind::Scalar);
    case FieldOp::Gradient:
        return b.finish(b.add(FieldOp::Gradient, {c}, {variant % 4, 1}, {0.25},
                              {Vec2d{-6.0, -2.0}, Vec2d{6.0, 3.0}}),
                        FieldResultKind::Scalar);
    case FieldOp::Shape:
        return b.finish(b.add(FieldOp::Shape, {c}, {0, variant % 6, 1, 0, 5}, {20.0, 1.0, 0.5, 4.0},
                              {Vec2d{0.5, -0.5}, Vec2d{9.0, 6.0}}),
                        FieldResultKind::Scalar);
    case FieldOp::Image: {
        const auto at = b.add(FieldOp::Image, {c}, {variant % 2, variant % 3, 2, 4, 0, 1});
        b.last().slot = 0;
        return b.finish(at, FieldResultKind::Color);
    }
    case FieldOp::Force: {
        const auto force = b.add(FieldOp::Force, {c}, {variant % 4, 3, 1}, {1.5, 7.0, 30.0},
                                 {Vec2d{1.0, 0.5}});
        b.last().curve = runtime::CompiledPathParameter{
            b.nextId(), curvePoints({{0.0, 1.0}, {0.5, 0.2}, {1.0, 0.0}})};
        return b.finish(force, FieldResultKind::Vector);
    }
    case FieldOp::Math: {
        const auto a = noise(c, 1, 1);
        const auto g = b.add(FieldOp::Gradient, {c}, {0, 0}, {0.0},
                             {Vec2d{-8.0, 0.0}, Vec2d{8.0, 0.0}});
        return b.finish(b.add(FieldOp::Math, {a, g, g}, {variant % 18}, {0.2, 45.0}),
                        FieldResultKind::Scalar);
    }
    case FieldOp::Remap: {
        const auto n = noise(c, 3, 1);
        const auto r = b.add(FieldOp::Remap, {n}, {1, variant % 2, variant == 2 ? 4 : 0, 0},
                             {0.2, 0.8, -0.5, 1.5, 0.4, 0.6});
        b.last().curve = runtime::CompiledPathParameter{
            b.nextId(), curvePoints({{0.0, 0.1}, {0.5, 0.7}, {1.0, 0.9}})};
        return b.finish(r, FieldResultKind::Scalar);
    }
    case FieldOp::Transform: {
        const auto moved = b.add(FieldOp::Transform, {c, -1}, {variant % 5, 6}, {30.0, 0.0},
                                 {Vec2d{1.0, 2.0}, Vec2d{1.5, 0.75}, Vec2d{0.5, 0.5},
                                  Vec2d{4.0, 3.0}});
        return b.finish(noise(moved, 1, 1), FieldResultKind::Scalar);
    }
    case FieldOp::Swizzle:
        return b.finish(b.add(FieldOp::Swizzle, {c}, {1, 0, 4, 5}, {0.5, -0.25, 1.0, 1.0}),
                        FieldResultKind::Color);
    case FieldOp::Combine: {
        const auto n = noise(c, 1, 0);
        return b.finish(b.add(FieldOp::Combine, {n, -1, n, -1}, {}, {0.0, 0.3, 0.0, 0.8}),
                        FieldResultKind::Color);
    }
    case FieldOp::Offset: {
        const auto moved = b.add(FieldOp::Offset, {c}, {1, -1}, {1.5});
        return b.finish(noise(moved, 1, 1), FieldResultKind::Scalar);
    }
    case FieldOp::Derivative: {
        std::array<int, 4> taps{};
        const std::array<std::array<std::int64_t, 2>, 4> signs{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
        for (std::size_t i = 0; i < 4; ++i)
            taps[i] = noise(b.add(FieldOp::Offset, {c}, {signs[i][0], signs[i][1]}, {1.0}), 1, 1);
        return b.finish(b.add(FieldOp::Derivative, {taps[0], taps[1], taps[2], taps[3]},
                              {variant % 4}, {1.0, 2.0}),
                        FieldResultKind::Vector);
    }
    case FieldOp::CellCentre:
        return b.finish(b.add(FieldOp::CellCentre, {c}, {}, {4.0}), FieldResultKind::Vector);
    case FieldOp::Arrow: {
        const auto centre = b.add(FieldOp::CellCentre, {c}, {}, {6.0});
        const auto v = b.add(FieldOp::Force, {centre}, {1, 0, 0}, {1.0, 20.0, 0.0},
                             {Vec2d{0.0, 0.0}});
        return b.finish(b.add(FieldOp::Arrow, {c, centre, v}, {}, {6.0, 4.0, 1.0}),
                        FieldResultKind::Scalar, FieldOutputMode::Mask);
    }
    case FieldOp::Streak: {
        const auto v = b.add(FieldOp::Force, {c}, {1, 0, 0}, {1.0, 20.0, 0.0}, {Vec2d{0.0, 0.0}});
        return b.finish(b.add(FieldOp::Streak, {c, v}, {}, {1.5, 2.0}), FieldResultKind::Scalar,
                        FieldOutputMode::Mask);
    }
    case FieldOp::LensWarp: {
        // The source position of an Undistort (variant 0) or a Redistort (variant 1) about a centre
        // off the middle, as a vector raster.
        const auto warped = b.add(FieldOp::LensWarp, {c}, {variant % 2},
                                  {-0.18, 0.04, 0.012, -0.008, 14.0}, {Vec2d{1.5, -1.0}});
        return b.finish(warped, FieldResultKind::Vector);
    }
    case FieldOp::TestCard:
        return testCardProgram(idBase, variant % 16, variant % 3, variant % 4, variant % 2 == 0);
    case FieldOp::Homography: {
        // The source position of a Corner Pin of a 16 x 12 frame with its corners moved off the
        // frame, as a vector raster.
        const auto pinned = b.add(FieldOp::Homography, {c}, {0}, {},
                                  {Vec2d{-2.0, 1.0}, Vec2d{1.0, -1.5}, Vec2d{-1.0, 2.0},
                                   Vec2d{2.5, 0.5}});
        b.last().slot = 0;
        return b.finish(pinned, FieldResultKind::Vector);
    }
    case FieldOp::Rays: {
        // The rays gathered from an image's luma towards a centre
        // off the middle, as a premultiplied raster.
        const auto rays = b.add(FieldOp::Rays, {c}, {0}, {0.4, 1.0, 2.0},
                                {Vec2d{1.5, -1.0}});
        b.last().slot = 0;
        return b.finish(rays, FieldResultKind::Premultiplied);
    }
    case FieldOp::ProceduralWarp: {
        // The source position of a chain of procedural warps, as a vector raster: variant 0 Twirl,
        // Bulge and Spherize; 1 Ripple, a pinned triangle Wave and Rectangular to Polar; 2 a
        // feathered square Magnify, a square Wave, Polar to Rectangular and a negative (inverse)
        // Spherize.
        const auto warp = [&](const int at, const std::int64_t mode, const std::int64_t shape,
                              const std::int64_t pins, const std::int64_t back,
                              const double amount) {
            return b.add(FieldOp::ProceduralWarp, {at}, {mode, shape, pins, back, back, back, 16, 12},
                         {6.0, 140.0, 100.0, amount, 1.6, 3.5, 1.2, 30.0, 0.5, 60.0, 80.0, 180.0,
                          1.5},
                         {Vec2d{1.5, -1.0}});
        };
        int where = c;
        if (variant % 3 == 0) {
            where = warp(where, 0, 0, 0, 0, 60.0);
            where = warp(where, 1, 0, 0, 0, -45.0);
            where = warp(where, 2, 0, 0, 0, 70.0);
        } else if (variant % 3 == 1) {
            where = warp(where, 3, 0, 0, 0, 0.0);
            where = warp(where, 4, 1, 15, 0, 0.0);
            where = warp(where, 5, 0, 0, 0, 0.0);
        } else {
            where = warp(where, 6, 0, 0, 1, 0.0);
            where = warp(where, 4, 2, 0, 0, 0.0);
            where = warp(where, 5, 0, 0, 1, 0.0);
            where = warp(where, 2, 0, 0, 0, -70.0);
        }
        return b.finish(where, FieldResultKind::Vector);
    }
    case FieldOp::Ramp: {
        const auto n = noise(c, 3, 1);
        const auto ramp = b.add(FieldOp::Ramp, {n}, {}, {0.1, 0.9});
        b.last().ramp = runtime::CompiledGradientParameter{
            b.nextId(), document::GradientValue{{{0.0, core::Color4d{0.0, 0.05, 0.3, 1.0}},
                                                 {0.6, core::Color4d{1.2, 0.4, 0.1, 1.0}},
                                                 {1.0, core::Color4d{3.0, 2.5, 1.0, 1.0}}}}};
        return b.finish(ramp, FieldResultKind::Color, FieldOutputMode::Color);
    }
    }
    return b.finish(c, FieldResultKind::Vector);
}

// #486 Lens Distortion: the program its image output (`stMap` false) or its ST map output lowers to
// over the 16 x 12 test composition, mirroring snapshot_compiler_fields.ipp's lowerLensDistortion
// (the snapshot compiler test holds the two to the same instruction sequence).
[[nodiscard]] inline FieldProgram lensDistortionProgram(const std::uint64_t idBase,
                                                        const bool stMap, const bool reverse) {
    Builder b(idBase);
    const auto c = b.add(FieldOp::Coordinate);
    const auto where = b.add(FieldOp::LensWarp, {c}, {reverse ? 1 : 0},
                             {-0.18, 0.04, 0.012, -0.008, 10.0}, {Vec2d{1.5, -1.0}});
    if (stMap) {
        const auto scaled = b.add(FieldOp::Swizzle, {where}, {0, 1, 4, 4},
                                  {1.0 / 16.0, 1.0 / 12.0, 1.0, 1.0});
        const auto half = b.add(FieldOp::Constant, {}, {}, {0.5, 0.5, 0.0, 0.0});
        const auto position = b.add(FieldOp::Math, {scaled, half}, {0}, {0.1, 0.0});
        return b.finish(position, FieldResultKind::Vector);
    }
    const auto read = b.add(FieldOp::Image, {where}, {0, 1, 3, 4, 0, 1});
    b.last().slot = 0;
    return b.finish(read, FieldResultKind::Premultiplied);
}

// #486 Chromatic Aberration: the program its lowering builds over the 16 x 12 test composition,
// mirroring snapshot_compiler_fields.ipp's lowerChromaticAberration (the snapshot compiler test holds
// the two to the same instruction sequence).
[[nodiscard]] inline FieldProgram chromaticAberrationProgram(const std::uint64_t idBase) {
    Builder b(idBase);
    const auto c = b.add(FieldOp::Coordinate);
    const auto red = b.add(FieldOp::LensWarp, {c}, {0}, {0.04, 0.0, 0.0, 0.0, 10.0},
                           {Vec2d{1.5, -1.0}});
    const auto twice = b.add(FieldOp::Swizzle, {c}, {0, 1, 4, 4}, {2.0, 2.0, 1.0, 1.0});
    const auto blue = b.add(FieldOp::Math, {twice, red}, {1}, {0.1, 0.0});
    const auto read = [&](const int where) {
        const auto texel = b.add(FieldOp::Image, {where}, {0, 1, 3, 4, 0, 1});
        b.last().slot = 0;
        return texel;
    };
    const auto texelRed = read(red);
    const auto texelGreen = read(c);
    const auto texelBlue = read(blue);
    const auto green = b.add(FieldOp::Swizzle, {texelGreen}, {1, 4, 4, 4});
    const auto blueChannel = b.add(FieldOp::Swizzle, {texelBlue}, {2, 4, 4, 4});
    const auto widest = b.add(FieldOp::Math, {texelRed, texelGreen}, {5}, {0.1, 0.0});
    const auto widest3 = b.add(FieldOp::Math, {widest, texelBlue}, {5}, {0.1, 0.0});
    const auto alpha = b.add(FieldOp::Swizzle, {widest3}, {3, 4, 4, 4});
    const auto joined = b.add(FieldOp::Combine, {texelRed, green, blueChannel, alpha}, {},
                              {0.0, 0.0, 0.0, 0.0});
    return b.finish(joined, FieldResultKind::Premultiplied);
}

// #508 Light Rays: the program its lowering builds over the 16 x 12 test composition, mirroring
// snapshot_compiler_fields.ipp's lowerLightRays (the snapshot compiler test holds the two to the
// same instruction sequence). `alpha` gathers the input's alpha instead of its luma.
[[nodiscard]] inline FieldProgram lightRaysProgram(const std::uint64_t idBase,
                                                   const bool alpha = false) {
    Builder b(idBase);
    const auto c = b.add(FieldOp::Coordinate);
    const auto rays = b.add(FieldOp::Rays, {c}, {alpha ? 1 : 0}, {0.4, 1.5, 2.0}, {Vec2d{1.5, -1.0}});
    b.last().slot = 0;
    const auto tint = b.add(FieldOp::Constant, {}, {}, {1.0, 0.8, 0.6, 1.0});
    const auto tinted = b.add(FieldOp::Math, {rays, tint}, {2}, {0.1, 0.0});
    const auto texel = b.add(FieldOp::Image, {c}, {0, 1, 3, 4, 0, 1});
    b.last().slot = 0;
    const auto joined = b.add(FieldOp::Math, {texel, tinted}, {0}, {0.1, 0.0});
    return b.finish(joined, FieldResultKind::Premultiplied);
}

// #510 Procedural Warp: the program its lowering builds for `mode`, mirroring
// snapshot_compiler_warp.ipp's lowerProceduralWarp (the snapshot compiler test holds the two to the
// same instruction sequence): the instruction's choices [mode, wave, pinned edges, polar direction,
// shape, link, frame width, frame height], its scalars in operand order, the centre, the input read
// at the moved position through `filter` and `edge`; Magnify also reads the plain picture and blends
// the magnified one over it by the instruction's weight. `variant` picks the mode's own choices (a
// wave shape, pinned edges, the polar direction, a square, the link), at values scaled to a
// `width` x `height` frame (the 16 x 12 test composition by default).
[[nodiscard]] inline FieldProgram proceduralWarpProgram(const std::uint64_t idBase,
                                                        const std::int64_t mode,
                                                        const std::int64_t filter = 0,
                                                        const std::int64_t edge = 1,
                                                        const std::int64_t variant = 0,
                                                        const std::int64_t width = 16,
                                                        const std::int64_t height = 12) {
    Builder b(idBase);
    const auto c = b.add(FieldOp::Coordinate);
    const double k = static_cast<double>(width) / 16.0;
    std::vector<std::int64_t> choices{mode, 0, 0, 0, 0, 0, width, height};
    if (mode == 4) {
        choices[1] = variant % 4;
        constexpr std::array<std::int64_t, 4> pins{0, 15, 3, 12};
        choices[2] = pins[static_cast<std::size_t>(variant % 4)];
    } else if (mode == 5) {
        choices[3] = variant % 2;
    } else if (mode == 6) {
        choices[4] = variant % 2;
        choices[5] = variant % 3;
    }
    const double amount = variant % 2 == 0 ? 65.0 : -55.0;
    const auto where = b.add(FieldOp::ProceduralWarp, {c}, choices,
                             {6.0 * k, 160.0, variant % 2 == 0 ? 100.0 : 45.0, amount,
                              1.4 + 0.3 * static_cast<double>(variant), 3.5 * k, 1.1 * k, 25.0, 0.0,
                              70.0, variant % 2 == 0 ? 100.0 : 40.0, 220.0,
                              variant % 2 == 0 ? 0.0 : 1.5 * k},
                             {Vec2d{1.5 * k, -1.0 * k}});
    const auto read = b.add(FieldOp::Image, {where}, {filter, edge, 3, 4, 0, 1});
    b.last().slot = 0;
    if (mode != 6)
        return b.finish(read, FieldResultKind::Premultiplied);
    const auto plain = b.add(FieldOp::Image, {c}, {filter, edge, 3, 4, 0, 1});
    b.last().slot = 0;
    const auto weight = b.add(FieldOp::Swizzle, {where}, {2, 4, 4, 4}, {1.0, 1.0, 1.0, 1.0});
    const auto blended = b.add(FieldOp::Math, {plain, read, weight}, {9}, {0.1, 0.0});
    return b.finish(blended, FieldResultKind::Premultiplied);
}

// Packs the virtual registers into the sixteen physical ones by last use, as the lowering does.
inline void packRegisters(FieldProgram& program) {
    const auto count = program.instructions.size();
    std::vector<std::size_t> lastUse(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        lastUse[static_cast<std::size_t>(program.instructions[i].output)] = i;
        for (const auto input : program.instructions[i].inputs)
            if (input >= 0)
                lastUse[static_cast<std::size_t>(input)] = i;
    }
    lastUse[program.result] = count;
    std::vector<int> physicalOf(count, -1);
    std::array<std::size_t, 16> freeAfter{};
    std::array<bool, 16> used{};
    for (std::size_t i = 0; i < count; ++i) {
        auto& instruction = program.instructions[i];
        const auto virtualOutput = static_cast<std::size_t>(instruction.output);
        for (auto& input : instruction.inputs)
            if (input >= 0)
                input = static_cast<std::int8_t>(physicalOf[static_cast<std::size_t>(input)]);
        // A register whose last read is this very instruction may be written by it.
        for (std::size_t r = 0; r < 16; ++r)
            if (used[r] && freeAfter[r] <= i)
                used[r] = false;
        std::size_t chosen = 0;
        while (chosen < 16 && used[chosen])
            ++chosen;
        physicalOf[virtualOutput] = static_cast<int>(chosen);
        used[chosen] = true;
        freeAfter[chosen] = lastUse[virtualOutput];
        instruction.output = static_cast<std::int8_t>(chosen);
    }
    program.result = static_cast<std::uint8_t>(physicalOf[program.result]);
}

// #568 Rolling Shutter: the program its lowering builds for a top-to-bottom readout of half a frame
// over the 16 x 12 test composition, a Camera Shake behind both camera sockets and a flow image
// connected, mirroring snapshot_compiler_fields.ipp's lowerRollingShutter (the snapshot compiler
// test holds the two to the same instruction sequence). Slot 0 is the picture, slot 1 the flow.
//
// `direction` is the node's read direction (0 Top to Bottom, 1 Bottom to Top, 2 Left to Right, 3 Right
// to Left) and `edges` its edge mode (0 Extend, 1 Transparent), as the lowering builds each.
[[nodiscard]] inline FieldProgram rollingShutterProgram(const std::uint64_t idBase,
                                                        const std::int64_t direction = 0,
                                                        const std::int64_t edges = 0) {
    Builder b(idBase);
    // The phase in every lane: the row (y, lane 1) or the column (x, lane 0), scaled by 1 / extent
    // with the direction's sign, over the 16 x 12 composition.
    const std::int64_t lane = direction >= 2 ? 0 : 1;
    const double scale = direction == 0   ? -1.0 / 12.0
                         : direction == 1 ? 1.0 / 12.0
                         : direction == 2 ? 1.0 / 16.0
                                          : -1.0 / 16.0;
    constexpr double kRadians = 0.017453292519943295;
    const auto math = [&](const std::int64_t operation, const int x, const int y) {
        return b.add(FieldOp::Math, {x, y}, {operation}, {0.1, 0.0});
    };
    const auto c = b.add(FieldOp::Coordinate);
    const auto phase = b.add(FieldOp::Swizzle, {c}, {lane, lane, lane, lane}, {scale, scale, scale, scale});
    const auto amountOnce = b.add(FieldOp::Constant, {}, {}, {1.0, 0.0, 0.0, 0.0});
    const auto amount = b.add(FieldOp::Swizzle, {amountOnce}, {0, 0, 0, 0}, {1.0, 1.0, 1.0, 1.0});
    const auto a = b.add(FieldOp::Constant, {}, {}, {3.0, 1.0, 0.5, 0.0});
    const auto mid = b.add(FieldOp::Constant, {}, {}, {4.0, 2.0, 1.0, 0.0});
    const auto cc = b.add(FieldOp::Constant, {}, {}, {5.0, 3.0, 1.5, 0.0});
    const auto linear = math(1, cc, a);
    const auto twiceB = b.add(FieldOp::Swizzle, {mid}, {0, 1, 2, 3}, {2.0, 2.0, 2.0, 2.0});
    const auto sum = math(0, a, cc);
    const auto curve = math(1, sum, twiceB);
    const auto twiceCurve = b.add(FieldOp::Swizzle, {curve}, {0, 1, 2, 3}, {2.0, 2.0, 2.0, 2.0});
    const auto weighted = math(2, twiceCurve, phase);
    const auto shifted = math(0, linear, weighted);
    const auto change = math(2, shifted, phase);
    const auto scaled = math(2, change, amount);
    const auto moved = b.add(FieldOp::Swizzle, {scaled}, {0, 1, 4, 4}, {1.0, -1.0, 0.0, 0.0});
    const auto angle = b.add(FieldOp::Swizzle, {scaled}, {2, 2, 2, 2}, {kRadians, kRadians, kRadians, kRadians});
    const auto rest = b.add(FieldOp::Swizzle, {mid}, {0, 1, 4, 4}, {1.0, -1.0, 0.0, 0.0});
    const auto fromRest = math(1, c, rest);
    const auto v = math(1, fromRest, moved);
    const auto turned = b.add(FieldOp::Swizzle, {v}, {1, 0, 4, 4}, {-1.0, 1.0, 0.0, 0.0});
    const auto swung = math(2, turned, angle);
    const auto sway = math(0, v, swung);
    const auto where0 = math(0, sway, rest);
    const auto flow = b.add(FieldOp::Image, {c}, {0, 0, 1, 0, 0, 1});
    b.last().slot = 1;
    const auto flowUp = b.add(FieldOp::Swizzle, {flow}, {0, 1, 4, 4}, {1.0, -1.0, 0.0, 0.0});
    const auto frames = b.add(FieldOp::Swizzle, {c}, {lane, lane, lane, lane}, {0.5 * scale, 0.5 * scale, 0.5 * scale, 0.5 * scale});
    const auto times = math(2, frames, amount);
    const auto shift = math(2, flowUp, times);
    const auto where = math(1, where0, shift);
    const auto read = b.add(FieldOp::Image, {where}, {0, edges, 3, 4, 0, 1});
    b.last().slot = 0;
    auto program = b.finish(read, FieldResultKind::Premultiplied);
    packRegisters(program);
    return program;
}

// #348 Corner Pin, ST Map and Displace: the programs their lowerings build over the 16 x 12 test
// composition, mirroring snapshot_compiler_warp.ipp (the snapshot compiler test holds each to the
// same instruction sequence).
[[nodiscard]] inline FieldProgram cornerPinProgram(const std::uint64_t idBase,
                                                   const std::int64_t filter = 0) {
    Builder b(idBase);
    const auto c = b.add(FieldOp::Coordinate);
    const auto where = b.add(FieldOp::Homography, {c}, {0}, {},
                             {Vec2d{-2.0, 1.0}, Vec2d{1.0, -1.5}, Vec2d{-1.0, 2.0}, Vec2d{2.5, 0.5}});
    b.last().slot = 0;
    const auto read = b.add(FieldOp::Image, {where}, {filter, 1, 3, 4, 0, 1});
    b.last().slot = 0;
    return b.finish(read, FieldResultKind::Premultiplied);
}

// #513 Stabilize and Match Move: the picture moved by the difference of the held point and the
// tracked position (the Homography instruction's translation mode), `mode` 0 holding the picture
// still and 1 carrying it along.
[[nodiscard]] inline FieldProgram stabilizeProgram(const std::uint64_t idBase,
                                                   const std::int64_t filter = 0,
                                                   const std::int64_t mode = 0,
                                                   const std::int64_t edge = 1) {
    Builder b(idBase);
    const auto c = b.add(FieldOp::Coordinate);
    const auto where = b.add(FieldOp::Homography, {c}, {1, mode}, {}, {Vec2d{3.0, -2.0}, Vec2d{-4.5, 1.5}});
    const auto read = b.add(FieldOp::Image, {where}, {filter, edge, 3, 4, 0, 1});
    b.last().slot = 0;
    return b.finish(read, FieldResultKind::Premultiplied);
}

// The map is slot 0 and the picture slot 1; `u` and `v` the map's channels.
[[nodiscard]] inline FieldProgram stMapProgram(const std::uint64_t idBase,
                                               const std::int64_t filter = 0,
                                               const std::int64_t edge = 1,
                                               const std::int64_t u = 0, const std::int64_t v = 1) {
    Builder b(idBase);
    const auto c = b.add(FieldOp::Coordinate);
    const auto uv = b.add(FieldOp::Image, {c}, {1, 0, 1, 0, u, v});
    b.last().slot = 0;
    const auto scaled = b.add(FieldOp::Swizzle, {uv}, {0, 1, 4, 4}, {16.0, 12.0, 1.0, 1.0});
    const auto half = b.add(FieldOp::Constant, {}, {}, {8.0, 6.0, 0.0, 0.0});
    const auto where = b.add(FieldOp::Math, {scaled, half}, {1}, {0.1, 0.0});
    const auto read = b.add(FieldOp::Image, {where}, {filter, edge, 3, 4, 0, 1});
    b.last().slot = 1;
    return b.finish(read, FieldResultKind::Premultiplied);
}

// The picture is slot 0 and the map slot 1.
[[nodiscard]] inline FieldProgram displaceProgram(const std::uint64_t idBase,
                                                  const std::int64_t filter = 0,
                                                  const std::int64_t edge = 1,
                                                  const std::int64_t first = 0,
                                                  const std::int64_t second = 1) {
    Builder b(idBase);
    const auto c = b.add(FieldOp::Coordinate);
    const auto value = b.add(FieldOp::Image, {c}, {0, 0, 1, 0, first, second});
    b.last().slot = 1;
    const auto midOnce = b.add(FieldOp::Constant, {}, {}, {0.5, 0.0, 0.0, 0.0});
    const auto mid = b.add(FieldOp::Swizzle, {midOnce}, {0, 0, 4, 4}, {1.0, 1.0, 1.0, 1.0});
    const auto relative = b.add(FieldOp::Math, {value, mid}, {1}, {0.1, 0.0});
    const auto amounts = b.add(FieldOp::Constant, {}, {}, {3.0, 2.0, 0.0, 0.0});
    const auto moved = b.add(FieldOp::Math, {relative, amounts}, {2}, {0.1, 0.0});
    const auto shift = b.add(FieldOp::Swizzle, {moved}, {0, 1, 4, 4}, {1.0, -1.0, 0.0, 0.0});
    const auto where = b.add(FieldOp::Math, {c, shift}, {1}, {0.1, 0.0});
    const auto read = b.add(FieldOp::Image, {where}, {filter, edge, 3, 4, 0, 1});
    b.last().slot = 0;
    return b.finish(read, FieldResultKind::Premultiplied);
}

// #511 Tile & Mirror: the programs its lowering builds over the 16 x 12 test composition, mirroring
// snapshot_compiler_warp.ipp (the snapshot compiler test holds each to the same instruction sequence).
// `mode` 0 Tile (`flags` bit 0 Mirror Edges, bit 1 Clamp to Output), 1 Wrap Offset, 2 Mirror (`flags`
// bit 0 the upper side), 3 Kaleidoscope (`flags` the segment count). Tile and Kaleidoscope end with the
// window-centre translation of the input's slot.
[[nodiscard]] inline FieldProgram tileMirrorProgram(const std::uint64_t idBase, const std::int64_t mode,
                                                    const std::int64_t filter = 0,
                                                    const std::int64_t edge = 1,
                                                    const std::int64_t flags = 0) {
    Builder b(idBase);
    const auto c = b.add(FieldOp::Coordinate);
    const Vec2d zero{};
    const Vec2d unit{1.0, 1.0};
    const auto translate = [&b](const int where, const std::int64_t direction, const Vec2d point) {
        return b.add(FieldOp::Homography, {where}, {1, direction}, {}, {point, Vec2d{}});
    };
    int where = c;
    std::int64_t readEdge = edge;
    switch (mode) {
    case 0: {
        const auto sizes = b.add(FieldOp::Constant, {}, {}, {37.5, 62.5, 0.0, 0.0});
        const auto tile = b.add(FieldOp::Swizzle, {sizes}, {0, 1, 4, 4}, {0.16, 0.12, 1.0, 1.0});
        const auto phases = b.add(FieldOp::Constant, {}, {}, {90.0, -45.0, 0.0, 0.0});
        const auto shift = b.add(FieldOp::Swizzle, {phases}, {0, 1, 4, 4},
                                 {1.0 / 360.0, -1.0 / 360.0, 1.0, 1.0});
        const auto tiles = b.add(FieldOp::Math, {c, tile}, {3}, {0.1, 0.0});
        const auto phased = b.add(FieldOp::Math, {tiles, shift}, {1}, {0.1, 0.0});
        const auto cell = b.add(FieldOp::Transform, {phased}, {(flags & 1) != 0 ? 3 : 2, 6},
                                {0.0, 0.0}, {zero, unit, zero, unit});
        const auto pixels = b.add(FieldOp::Math, {cell, tile}, {2}, {0.1, 0.0});
        where = translate(pixels, 1, Vec2d{1.5, -1.0});
        if ((flags & 2) != 0) {
            const auto extent = b.add(FieldOp::Constant, {}, {}, {80.0, 90.0, 0.0, 0.0});
            const auto half = b.add(FieldOp::Swizzle, {extent}, {0, 1, 4, 4}, {0.08, 0.06, 1.0, 1.0});
            const auto distance = b.add(FieldOp::Math, {c, c}, {7}, {0.1, 0.0});
            const auto beyond = b.add(FieldOp::Math, {distance, half}, {1}, {0.1, 0.0});
            const auto none = b.add(FieldOp::Constant, {}, {}, {0.0, 0.0, 0.0, 0.0});
            const auto outside = b.add(FieldOp::Math, {beyond, none}, {5}, {0.1, 0.0});
            const auto far = b.add(FieldOp::Swizzle, {outside}, {0, 1, 4, 4}, {1.0e19, 1.0e19, 1.0, 1.0});
            where = b.add(FieldOp::Math, {where, far}, {0}, {0.1, 0.0});
        }
        where = b.add(FieldOp::Homography, {where}, {2});
        b.last().slot = 0;
        break;
    }
    case 1:
        where = translate(c, 0, Vec2d{2.0, -1.0});
        readEdge = 2;
        break;
    case 2: {
        const auto nothing = b.add(FieldOp::Constant, {}, {}, {0.0, 0.0, 0.0, 0.0});
        const auto origin = translate(nothing, 1, Vec2d{1.0, 0.5});
        const auto local = b.add(FieldOp::Math, {c, origin}, {1}, {0.1, 0.0});
        const auto basis = b.add(FieldOp::Constant, {}, {}, {1.0, 0.0, 0.0, 0.0});
        const auto turned = b.add(FieldOp::Math, {basis}, {17}, {0.1, 30.0});
        const auto normal = b.add(FieldOp::Swizzle, {turned}, {1, 0, 4, 4}, {1.0, 1.0, 1.0, 1.0});
        const auto distance = b.add(FieldOp::Math, {local, normal}, {13}, {0.1, 0.0});
        const auto none = b.add(FieldOp::Constant, {}, {}, {0.0, 0.0, 0.0, 0.0});
        const auto far = b.add(FieldOp::Math, {distance, none}, {(flags & 1) != 0 ? 4 : 5}, {0.1, 0.0});
        const auto along = b.add(FieldOp::Math, {far, normal}, {2}, {0.1, 0.0});
        const auto twice = b.add(FieldOp::Swizzle, {along}, {0, 1, 4, 4}, {2.0, 2.0, 1.0, 1.0});
        const auto reflected = b.add(FieldOp::Math, {local, twice}, {1}, {0.1, 0.0});
        where = b.add(FieldOp::Math, {reflected, origin}, {0}, {0.1, 0.0});
        break;
    }
    default: {
        const auto nothing = b.add(FieldOp::Constant, {}, {}, {0.0, 0.0, 0.0, 0.0});
        const auto origin = translate(nothing, 1, Vec2d{-1.0, 1.0});
        const auto local = b.add(FieldOp::Math, {c, origin}, {1}, {0.1, 0.0});
        const auto turned = b.add(FieldOp::Math, {local}, {17}, {0.1, 15.0});
        const auto once = b.add(FieldOp::Constant, {}, {}, {150.0, 0.0, 0.0, 0.0});
        const auto size = b.add(FieldOp::Swizzle, {once}, {0, 0, 4, 4}, {1.0, 1.0, 1.0, 1.0});
        const auto divided = b.add(FieldOp::Math, {turned, size}, {3}, {0.1, 0.0});
        const auto scaled = b.add(FieldOp::Swizzle, {divided}, {0, 1, 4, 4}, {100.0, 100.0, 1.0, 1.0});
        const auto wedge = b.add(FieldOp::Transform, {scaled}, {4, flags}, {0.0, 0.0});
        where = b.add(FieldOp::Math, {wedge, origin}, {0}, {0.1, 0.0});
        where = b.add(FieldOp::Homography, {where}, {2});
        b.last().slot = 0;
        break;
    }
    }
    const auto read = b.add(FieldOp::Image, {where}, {filter, readEdge, 3, 4, 0, 1});
    b.last().slot = 0;
    auto program = b.finish(read, FieldResultKind::Premultiplied);
    packRegisters(program);
    return program;
}

} // namespace bloom::field_test_programs
