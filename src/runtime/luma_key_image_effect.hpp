#pragma once

// #338 Luma Key: the one CPU reference of the key, shared by the CPU evaluator and the resident GPU
// program generator, which runs the same Float32 operations over the same prepared values.

#include <bloom/document/parameter.hpp>
#include <bloom/render/ocio_gpu_program.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <array>
#include <cstddef>
#include <optional>

namespace bloom::runtime::detail {

// The key per pixel, in premultiplied scene-linear RGBA with alpha `a`:
//   luma  = fma(0.0722, b, fma(0.7152, g, 0.2126 * r))   (Rec. 709, the track-matte Luma weights)
//   d     = fma(-pivot, a, luma)                          (straight luma minus pivot, times a)
//   Ramp:     coverage = clamp(d * slope, 0, a)
//   Hard:     coverage = d >= 0 ? a : 0     HardInverted: coverage = d < 0 ? a : 0
// Ramp uses pivot = low and slope = 1/(high - low), or pivot = high and slope = -1/(high - low)
// when inverted, so the coverage is alpha times the straight-colour key and a transparent pixel has
// none under either polarity. high <= low (or a ramp too steep for binary32) is a hard key at low.
enum class LumaKeyMode : std::uint8_t { Ramp, Hard, HardInverted };
struct PreparedLumaKey final {
    float pivot = 0.0F;
    float slope = 1.0F;
    LumaKeyMode mode = LumaKeyMode::Ramp;
    friend bool operator==(const PreparedLumaKey&, const PreparedLumaKey&) = default;
};

// Null for a non-finite threshold or one outside document::kMaximumLumaKeyThreshold.
[[nodiscard]] std::optional<PreparedLumaKey> prepareLumaKey(const LumaKeyKernel& kernel,
                                                            std::optional<double> low,
                                                            std::optional<double> high);
[[nodiscard]] float lumaKeyCoverage(const PreparedLumaKey& key, float red, float green, float blue,
                                    float alpha) noexcept;

struct ImageEffectResult;
// Writes the coverage to every channel (the ADR 0028 Mask transport) over the input's windows.
[[nodiscard]] ImageEffectResult applyLumaKey(const PreparedLumaKey&, const render::Rgba32fImage&,
                                             std::size_t budget, const CancellationToken&,
                                             OperationIndex, const EvaluationProgressCallback&);
// The prepared values as the program's storage-buffer bytes: one vec4 (pivot, slope, mode, 0).
[[nodiscard]] std::array<float, 4> lumaKeyProgramData(const PreparedLumaKey&) noexcept;
// One fixed neighbourhood program; keyed thresholds change only its data, never its shader.
[[nodiscard]] render::OcioGpuProgramResult buildLumaKeyProgram(std::array<float, 4> data);
} // namespace bloom::runtime::detail
