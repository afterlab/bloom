#pragma once

// #525: compiled field programs for the runtime tests and the GPU coverage fixtures, one per field
// operation, each the program a Fields node lowers to (runtime/field_raster.hpp has the operand
// orders). Every program reads a constant operand set; its parameter ids start at `idBase`.

#include <bloom/core/field_program.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <array>
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

[[nodiscard]] inline document::PathValue curvePoints(std::initializer_list<Vec2d> points) {
    document::PathValue path;
    for (const auto point : points)
        path.anchors.push_back(document::PathAnchor{point, std::nullopt, std::nullopt});
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

} // namespace bloom::field_test_programs
