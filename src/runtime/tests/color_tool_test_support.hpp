#pragma once

// #518 Tone Map (and, in later changes, #517 Color Matrix and #523 HSL Qualifier): kernels shared by
// the CPU tests, the GPU coverage fixtures and the native parity runs.

#include <bloom/document/color_cleanup_nodes.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace bloom::color_tool_test {

using bloom::document::ParameterId;
using bloom::runtime::ColorToolKernel;
using bloom::runtime::ColorToolOperation;
using bloom::runtime::CompiledScalarParameter;
using bloom::runtime::ImageEffectKernel;

[[nodiscard]] inline CompiledScalarParameter scalar(const std::uint64_t id, const double value) {
    return {ParameterId::fromRaw(id), value};
}

struct ToneMapSettings final {
    std::int64_t method = document::kToneMapEachChannel;
    bool gamut = false;
    double knee = 0.8;
    double ceiling = 1.0;
    double shadows = 0.0;
    double shadowRange = 0.18;
    double gamutThreshold = 0.8;
    double gamutLimit = 1.2;
};

[[nodiscard]] inline ColorToolKernel toneMap(const std::uint64_t idBase,
                                             const ToneMapSettings& settings) {
    ColorToolKernel kernel;
    kernel.operation = ColorToolOperation::ToneMap;
    kernel.choices[0] = settings.method;
    kernel.choices[1] = settings.gamut ? 1 : 0;
    const std::array<double, 6> values{settings.knee,           settings.ceiling,
                                       settings.shadows,        settings.shadowRange,
                                       settings.gamutThreshold, settings.gamutLimit};
    for (std::size_t index = 0; index < values.size(); ++index)
        kernel.values[index] = scalar(idBase + 60 + index, values[index]);
    return kernel;
}

struct ColorMatrixSettings final {
    std::int64_t preset = document::kColorMatrixCustom;
    std::int64_t size = document::kColorMatrixSize3;
    std::int64_t inputModel = document::kColorModelRgb;
    std::int64_t outputModel = document::kColorModelRgb;
    std::array<double, 16> matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

[[nodiscard]] inline ColorToolKernel colorMatrix(const std::uint64_t idBase,
                                                 const ColorMatrixSettings& settings) {
    ColorToolKernel kernel;
    kernel.operation = ColorToolOperation::ColorMatrix;
    kernel.choices = {settings.preset, settings.size, settings.inputModel, settings.outputModel};
    for (std::size_t index = 0; index < settings.matrix.size(); ++index)
        kernel.values[index] = scalar(idBase + 60 + index, settings.matrix[index]);
    return kernel;
}

struct HslQualifierSettings final {
    bool invert = false;
    double hueCenter = 0.0;
    double hueWidth = 360.0;
    double hueSoftness = 20.0;
    double satLow = 0.0;
    double satHigh = 1.0;
    double satSoftness = 0.1;
    double lumaLow = 0.0;
    double lumaHigh = 1.0;
    double lumaSoftness = 0.1;
};

[[nodiscard]] inline ColorToolKernel hslQualifier(const std::uint64_t idBase,
                                                  const HslQualifierSettings& settings) {
    ColorToolKernel kernel;
    kernel.operation = ColorToolOperation::HslQualifier;
    kernel.choices[0] = settings.invert ? 1 : 0;
    const std::array<double, 9> values{settings.hueCenter, settings.hueWidth,  settings.hueSoftness,
                                       settings.satLow,    settings.satHigh,   settings.satSoftness,
                                       settings.lumaLow,   settings.lumaHigh,  settings.lumaSoftness};
    for (std::size_t index = 0; index < values.size(); ++index)
        kernel.values[index] = scalar(idBase + 60 + index, values[index]);
    return kernel;
}

// #519 Quantize: every value of the node; a kernel built from these is what the lowering builds.
struct QuantizeSettings final {
    std::int64_t mode = document::kQuantizeThreshold;
    std::int64_t basis = document::kQuantizeBasisLuma;
    bool invert = false;
    bool link = true;
    bool alpha = false;
    double level = 0.5;
    double softness = 0.0;
    double levels = 8.0;
    double red = 8.0;
    double green = 8.0;
    double blue = 8.0;
    double alphaLevels = 8.0;
};

[[nodiscard]] inline ColorToolKernel quantize(const std::uint64_t idBase,
                                              const QuantizeSettings& settings) {
    ColorToolKernel kernel;
    kernel.operation = ColorToolOperation::Quantize;
    kernel.choices = {settings.mode, settings.basis, settings.invert ? 1 : 0,
                      (settings.link ? 1 : 0) | (settings.alpha ? 2 : 0)};
    const std::array<double, 7> values{settings.level, settings.softness, settings.levels,
                                       settings.red,   settings.green,    settings.blue,
                                       settings.alphaLevels};
    for (std::size_t index = 0; index < values.size(); ++index)
        kernel.values[index] = scalar(idBase + 60 + index, values[index]);
    return kernel;
}

} // namespace bloom::color_tool_test
