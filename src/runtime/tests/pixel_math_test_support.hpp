#pragma once

// #481 Image Math, Image Map Range and Image Clamp: kernels shared by the CPU tests, the GPU
// coverage fixtures and the native parity runs.

#include <bloom/runtime/compiled_plan.hpp>

#include <array>
#include <cstdint>

namespace bloom::pixel_math_test {

using bloom::document::ParameterId;
using bloom::runtime::CompiledScalarParameter;
using bloom::runtime::ImageEffectKernel;
using bloom::runtime::PixelMathKernel;
using bloom::runtime::PixelMathOperation;

[[nodiscard]] inline CompiledScalarParameter scalar(const std::uint64_t id, const double value) {
    return {ParameterId::fromRaw(id), value};
}

// An arithmetic operation with a number as its operand (none for Absolute), or `second` for the
// pixel of a second image.
[[nodiscard]] inline PixelMathKernel math(const std::uint64_t idBase, const PixelMathOperation operation,
                                          const double value, const std::int64_t channels,
                                          const bool straight, const bool second = false) {
    PixelMathKernel kernel;
    kernel.operation = operation;
    kernel.channels = channels;
    kernel.straight = straight;
    kernel.second = second;
    if (!second && operation != PixelMathOperation::Absolute)
        kernel.values[0] = scalar(idBase + 60, value);
    return kernel;
}

[[nodiscard]] inline PixelMathKernel mapRange(const std::uint64_t idBase, const double inputMin,
                                              const double inputMax, const double outputMin,
                                              const double outputMax, const bool clamp,
                                              const std::int64_t channels, const bool straight) {
    PixelMathKernel kernel;
    kernel.operation = PixelMathOperation::MapRange;
    kernel.channels = channels;
    kernel.straight = straight;
    kernel.clamp = clamp;
    kernel.values[0] = scalar(idBase + 60, inputMin);
    kernel.values[1] = scalar(idBase + 61, inputMax);
    kernel.values[2] = scalar(idBase + 62, outputMin);
    kernel.values[3] = scalar(idBase + 63, outputMax);
    return kernel;
}

[[nodiscard]] inline PixelMathKernel clampKernel(const std::uint64_t idBase,
                                                 const std::array<double, 8>& limits,
                                                 const bool straight) {
    PixelMathKernel kernel;
    kernel.operation = PixelMathOperation::Clamp;
    kernel.channels = document::kImagePixelColorAndAlpha;
    kernel.straight = straight;
    for (std::size_t index = 0; index < limits.size(); ++index)
        kernel.values[index] = scalar(idBase + 60 + index, limits[index]);
    return kernel;
}

// #349 Image Compare (the second image is required): a mode (document::kImageCompareDifference ...),
// the difference gain, the split position and angle in degrees and the checkerboard cell in pixels.
[[nodiscard]] inline PixelMathKernel compare(const std::uint64_t idBase, const std::int64_t mode,
                                             const double gain, const double position,
                                             const double angle, const double cell) {
    PixelMathKernel kernel;
    kernel.operation = PixelMathOperation::Compare;
    kernel.straight = false;
    kernel.second = true;
    kernel.compareMode = mode;
    kernel.values[0] = scalar(idBase + 60, gain);
    kernel.values[1] = scalar(idBase + 61, position);
    kernel.values[2] = scalar(idBase + 62, angle);
    kernel.values[3] = scalar(idBase + 63, cell);
    return kernel;
}

} // namespace bloom::pixel_math_test
