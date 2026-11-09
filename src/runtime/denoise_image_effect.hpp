#pragma once

// #514 Denoise & Smooth, the spatial modes: Median (with Dust & Scratches' threshold), Bilateral
// (edge-preserving) and Spatial (a non-local means search over 3 x 3 patches). One CPU reference,
// shared by the CPU evaluator and the resident GPU program generator, which runs the same Float32
// operations over the same prepared table.
//
// The output has the input's data window. Every mode reads the square of radius (rx, ry) around the
// pixel, skipping texels outside the window (a Spatial patch clamps to the window's edge), over the
// stored (associated) channels, so the host table is the whole interface:
//   Median: per channel the middle of the sorted values present (the upper middle on an even
//     count); a channel keeps its own value unless it differs from the median by more than the
//     threshold, so threshold 0 is the plain median and a larger one removes only outliers.
//   Bilateral: p + sum(w (q - p)) / sum(w) in scan order (dy then dx, fma folds), w = ws(dx) ws(dy) wr, the
//     spatial weights the table's compact biweights 1 - (d / (r + 1))^2 squared, wr the same shape
//     over the squared distance of the four channels to the center pixel's divided by strength^2.
//   Spatial: the same fold with wr taken from the mean squared difference of the 3 x 3 patch around
//     the center and around the candidate, over the four channels, divided by strength^2.
// Weights are polynomials, not exponentials, so the device repeats the host's arithmetic. A pixel
// always weighs itself 1, so a sum is never empty. Alpha is limited to [0, 1].
//
// The radius is authored in full-resolution pixels and scaled to the evaluation resolution; a
// radius that rounds to zero is the identity.

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

// The largest radius, in evaluation pixels, a mode reads: 13 x 13 texels, 169 values per channel.
inline constexpr int kMaximumDenoiseRadius = 6;
inline constexpr double kMaximumDenoiseAuthoredRadius = 6.0;
inline constexpr double kMaximumDenoiseStrength = 10.0;

struct PreparedDenoise final {
    DenoiseMode mode = DenoiseMode::Median;
    int radiusX = 0;
    int radiusY = 0;
    // The Median threshold, or the Bilateral and Spatial 1 / strength^2.
    float threshold = 0.0F;
    float inverseVariance = 0.0F;
    std::array<float, kMaximumDenoiseRadius + 2> weightsX{};
    std::array<float, kMaximumDenoiseRadius + 2> weightsY{};
    [[nodiscard]] bool identity() const noexcept { return radiusX == 0 && radiusY == 0; }
    friend bool operator==(const PreparedDenoise&, const PreparedDenoise&) = default;
};

// Null for a non-finite or out-of-domain radius or strength, or a non-positive scale.
[[nodiscard]] std::optional<PreparedDenoise>
prepareDenoise(const DenoiseKernel& kernel, std::optional<double> radius,
               std::optional<double> strength, double horizontalScale, double verticalScale);
[[nodiscard]] std::string denoiseIdentity(const PreparedDenoise&);

struct ImageEffectResult;
[[nodiscard]] ImageEffectResult applyDenoise(const PreparedDenoise&, const render::Rgba32fImage&,
                                             std::size_t budget, const CancellationToken&,
                                             OperationIndex, const EvaluationProgressCallback&);
// The table as the program's storage-buffer data: six vec4.
[[nodiscard]] std::vector<float> denoiseProgramData(const PreparedDenoise&);
[[nodiscard]] render::OcioGpuProgramResult buildDenoiseProgram(const std::vector<float>& data);

} // namespace bloom::runtime::detail
