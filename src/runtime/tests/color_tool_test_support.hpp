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

} // namespace bloom::color_tool_test
