#pragma once

// #506 Directional Blur: one pass of a ray blur, the primitive Glow's Streak mode chains into long
// exponential streaks. The one CPU reference, shared by the CPU evaluator and the resident GPU
// program generator, which runs the same Float32 operations over the same prepared taps.
//
// The host prepares the pass as SIXTEEN weighted integer taps: each of the four ray taps
// i = 0..3, offset o_i = -i * spacing * (cos a, sin a) (scaled to the evaluation resolution and
// snapped to 2^-20 pixel), is a bilinear sample split into its four texels, weighted by
// decay^i / sum(decay^j) times the bilinear weight. Output pixel c is then
//   sum = fma(input(c + t_k), w_k, sum)   for k = 0..15 in order, skipping a texel outside the input,
// with alpha limited to [0, 1], exactly as the separable blur folds its taps. Integer addressing
// and host weights make the device fold the CPU fold operation for operation. The data window
// grows to every pixel a nonzero tap can reach; spacing 0 is the exact identity.

#include <bloom/render/ocio_gpu_program.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

inline constexpr std::size_t kDirectionalBlurRayTaps = 4;
inline constexpr std::size_t kDirectionalBlurTexels = 4 * kDirectionalBlurRayTaps;
// Full-resolution pixels between taps. Glow's longest streak (1024 pixels) chains five passes with
// spacings length * 4^p / 1023, the widest 256.25 pixels.
inline constexpr double kMaximumDirectionalBlurSpacing = 1024.0;

struct PreparedDirectionalBlur final {
    // Texel k reads input(c + (dx[k], dy[k])) with weight weights[k].
    std::array<std::int32_t, kDirectionalBlurTexels> dx{};
    std::array<std::int32_t, kDirectionalBlurTexels> dy{};
    std::array<float, kDirectionalBlurTexels> weights{};
    bool identity = false;
    friend bool operator==(const PreparedDirectionalBlur&,
                           const PreparedDirectionalBlur&) = default;
};

// Null for a non-finite angle, a spacing outside [0, kMaximumDirectionalBlurSpacing], a decay
// outside (0, 1] or a non-positive scale.
[[nodiscard]] std::optional<PreparedDirectionalBlur>
prepareDirectionalBlur(const DirectionalBlurKernel& kernel, std::optional<double> angle,
                       std::optional<double> spacing, double horizontalScale,
                       double verticalScale);
// The input window grown to every pixel whose nonzero taps reach it.
[[nodiscard]] std::optional<render::ImageWindow>
directionalBlurOutputWindow(const PreparedDirectionalBlur&, render::ImageWindow input);
[[nodiscard]] std::string directionalBlurIdentity(const PreparedDirectionalBlur&);

struct ImageEffectResult;
[[nodiscard]] ImageEffectResult applyDirectionalBlur(const PreparedDirectionalBlur&,
                                                     const render::Rgba32fImage&,
                                                     std::size_t budget, const CancellationToken&,
                                                     OperationIndex,
                                                     const EvaluationProgressCallback&);
// The prepared taps as the program's storage-buffer data: sixteen vec4 (dx, dy, weight, 0).
[[nodiscard]] std::vector<float> directionalBlurProgramData(const PreparedDirectionalBlur&);
// One fixed neighbourhood program over the input padded to the output window; a keyed angle or
// spacing changes only its data, never its shader.
[[nodiscard]] render::OcioGpuProgramResult
buildDirectionalBlurProgram(const std::vector<float>& data);

} // namespace bloom::runtime::detail
