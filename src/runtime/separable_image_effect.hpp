#pragma once

#include <bloom/render/ocio_gpu_program.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

namespace bloom::runtime::detail {
// A proxy-upsampled kernel has at most 8193 coefficients (32,772 bytes) per axis. This is
// an explicit shader-source/loop-work bound; every larger request returns Unsupported/failure.
inline constexpr std::size_t kMaximumSeparableBlurSupport = 4096;
// The closed kernel shape both routes accept: a known kind, edge and channel policy, and the
// #338 morphology kinds only with the Coverage policy (they reduce Mask coverage, never RGB), or
// (#483) Dilate with the Rgba policy: the alpha dilated, each pixel's own colour carried unchanged
// (Z Defocus's tile maximum carries its signed circle of confusion in red).
[[nodiscard]] constexpr bool
validSeparableBlurKernelShape(const SeparableBlurKernel& kernel) noexcept {
    const auto kind = static_cast<unsigned>(kernel.kind);
    return kind <= static_cast<unsigned>(SeparableBlurKind::Dilate) &&
           static_cast<unsigned>(kernel.edge) <= 1 && static_cast<unsigned>(kernel.channels) <= 1 &&
           (kind <= static_cast<unsigned>(SeparableBlurKind::Box) ||
            kernel.channels == BlurChannels::Coverage ||
            kernel.kind == SeparableBlurKind::Dilate);
}
struct PreparedSeparableBlur final {
    std::vector<float> horizontal;
    std::vector<float> vertical;
    BlurEdgeMode edge = BlurEdgeMode::Transparent;
    BlurChannels channels = BlurChannels::Rgba;
    SeparableBlurKind kind = SeparableBlurKind::Gaussian;
    [[nodiscard]] bool identity() const noexcept {
        return horizontal.size() == 1 && vertical.size() == 1;
    }
};
struct ImageEffectResult;
[[nodiscard]] std::optional<PreparedSeparableBlur>
prepareSeparableBlur(const SeparableBlurKernel&, std::optional<double> radius,
                     double horizontalScale, double verticalScale,
                     std::optional<double> verticalRadius = std::nullopt);
[[nodiscard]] std::optional<render::ImageWindow> blurOutputWindow(const PreparedSeparableBlur&,
                                                                  render::ImageWindow);
[[nodiscard]] ImageEffectResult applySeparableBlur(const PreparedSeparableBlur&,
                                                   const render::Rgba32fImage&, std::size_t budget,
                                                   const CancellationToken&, OperationIndex,
                                                   const EvaluationProgressCallback&);
// Host preparation emits only normalized coefficient GLSL. Pixels stay resident on the device.
// Coverage filters alpha alone and writes it to every channel, exactly as the CPU pass does.
// Erode/Dilate (#338) reduce Coverage by minimum/maximum; Gaussian and Box share one sum program.
[[nodiscard]] render::OcioGpuProgramResult
buildSeparableBlurProgram(std::span<const float> weights, bool vertical, BlurEdgeMode edge,
                          BlurChannels channels = BlurChannels::Rgba,
                          SeparableBlurKind kind = SeparableBlurKind::Gaussian);
} // namespace bloom::runtime::detail
