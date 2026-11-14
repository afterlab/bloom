#pragma once

// #724 Scopes: the scope measure of one frame, through the device route or the CPU reference.
// docs/architecture/scopes.md is the specification.
//
// The measured signal of a frame is
//   Working: the frame's scene-linear picture as stored (its color over black and its alpha);
//   Display: every pixel's straight color through the Viewer's display and view, then the
//            Viewer's exposure and gamma when they are not neutral (which also clamps to 0..1, as
//            the Viewer shows it), then multiplied by the pixel's alpha again. With a neutral
//            adjustment the display transform's float result is kept as it is, so values the
//            display encoding leaves outside 0..1 are counted as under and over.
// The Chromaticity table places every pixel by the CIE XYZ (D65) of that signal, converted by the
// config's own transform from the signal's color space (color/scope_colorimetry.hpp).
//
// The device route runs the frame as a job of the final-render evaluator
// (runtime/gpu_scope_measure.hpp) and reads back only the tables. The CPU reference evaluates the
// frame with the CPU evaluator and counts with measureScopesCpu. A result always names the route
// that counted it; a frame the device could not measure is counted by the CPU reference and says
// why.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/color/scope_colorimetry.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/runtime/view_adjust.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::runtime {

class GpuProcessFrameEvaluator;
struct GpuSceneOcioContext;

struct ScopeSignalColorRequest final {
    std::shared_ptr<const color::ResolvedBloomNeutralConfig> config;
    ScopeSignal signal = ScopeSignal::Display;
    // The Viewer's display and view; empty selects the config's own. Display signal only.
    std::string display;
    std::string view;
    // The Viewer's exposure and gamma. Display signal only.
    ViewAdjust viewAdjust{};
};

enum class ScopeSignalColorError : std::uint8_t {
    None,
    InvalidRequest,
    // The config has no such display and view.
    DisplayUnavailable,
    // The signal's color space has no route to CIE XYZ in this config.
    XyzUnavailable,
};

class ScopeSignalColor;

struct [[nodiscard]] ScopeSignalColorResult final {
    std::shared_ptr<const ScopeSignalColor> color;
    ScopeSignalColorError error = ScopeSignalColorError::None;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept { return color != nullptr; }
};

// The color objects of one signal: the CPU display processor (Display), the transform to CIE XYZ,
// the luma coefficients and the colorimetry the graticules draw. Blocking to prepare (OCIO), so
// never prepared on the UI thread; immutable afterwards and shared by every frame of the signal.
class ScopeSignalColor final {
  public:
    [[nodiscard]] static ScopeSignalColorResult prepare(const ScopeSignalColorRequest& request);

    [[nodiscard]] ScopeSignal signal() const noexcept { return signal_; }
    [[nodiscard]] const color::ResolvedBloomNeutralConfig& config() const& noexcept {
        return *config_;
    }
    [[nodiscard]] const std::string& display() const& noexcept { return display_; }
    [[nodiscard]] const std::string& view() const& noexcept { return view_; }
    [[nodiscard]] const ViewAdjust& viewAdjust() const& noexcept { return viewAdjust_; }
    // The signal's color space in the config: the display's color space, or the working space.
    [[nodiscard]] const std::string& colorSpaceId() const& noexcept { return colorSpaceId_; }
    [[nodiscard]] const ScopeLuma& luma() const& noexcept { return luma_; }
    [[nodiscard]] const color::ScopeColorimetry& colorimetry() const& noexcept {
        return xyz_->colorimetry();
    }
    [[nodiscard]] const color::ScopeXyzProcessor& xyz() const& noexcept { return *xyz_; }
    // Null for the Working signal.
    [[nodiscard]] const color::PreparedCpuDisplayProcessorHandle*
    displayProcessor() const noexcept {
        return displayProcessor_.get();
    }
    // Everything of this object that decides a count: part of a scope sidecar's identity.
    [[nodiscard]] const std::string& identity() const& noexcept { return identity_; }

  private:
    ScopeSignalColor() = default;

    std::shared_ptr<const color::ResolvedBloomNeutralConfig> config_;
    ScopeSignal signal_ = ScopeSignal::Display;
    std::string display_;
    std::string view_;
    ViewAdjust viewAdjust_{};
    std::string colorSpaceId_;
    ScopeLuma luma_;
    std::shared_ptr<const color::ScopeXyzProcessor> xyz_;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> displayProcessor_;
    std::string identity_;
};

enum class ScopeFrameStatus : std::uint8_t {
    Measured,
    // The region lies outside the picture.
    EmptyArea,
    // The device route cannot measure this frame; `message` says why. Only from
    // measureScopeFrameOnDevice: measureScopeFrame counts such a frame with the CPU reference.
    Unavailable,
    Cancelled,
    Failed,
};

struct ScopeFrameMeasure final {
    ScopeFrameStatus status = ScopeFrameStatus::Failed;
    // Failed and Unavailable: the reason. Measured by the CPU reference after the device route
    // could not: why the device did not count it. Empty otherwise.
    std::string message;
    ScopeRoute route = ScopeRoute::Cpu;
    ScopeCounts counts;
    ScopeMeasuredArea area;
    // The native work of a device measure (all 0 for the CPU reference, which reads nothing back):
    // color program dispatches, table readbacks, the bytes those readbacks moved (the layout's
    // bytes at the measure's detail) and picture readbacks (always 0: no picture leaves the
    // device).
    std::uint64_t colorDispatches = 0;
    std::uint64_t tableReadbacks = 0;
    std::uint64_t tableReadbackBytes = 0;
    std::uint64_t pictureReadbacks = 0;
};

// The CPU reference: counts `frame`, an evaluated process frame of `plan`'s composition.
// `settings.signal` must be `color`'s signal.
[[nodiscard]] ScopeFrameMeasure measureScopeFrameCpu(const ProcessFrame& frame,
                                                     const ScopeMeasureSettings& settings,
                                                     const ScopeSignalColor& color,
                                                     const CancellationToken& cancellation = {});

// The device route. Called on a CPU worker; blocks until the device has counted the frame.
[[nodiscard]] ScopeFrameMeasure
measureScopeFrameOnDevice(GpuProcessFrameEvaluator& evaluator, const GpuSceneOcioContext& ocio,
                          std::shared_ptr<const CompiledCompositionPlan> plan,
                          const EvaluationRequest& request, const ScopeMeasureSettings& settings,
                          const ScopeSignalColor& color, const CancellationToken& cancellation = {},
                          const std::optional<std::filesystem::path>& assetBaseDirectory = {});

// Where a frame is measured. `device` and `ocio` are the application's final-render evaluator and
// its shared OCIO context (either may be null: the CPU reference then counts every frame);
// `evaluateCpu` evaluates a frame with the CPU reference evaluator.
struct ScopeFrameRoutes final {
    std::function<std::shared_ptr<GpuProcessFrameEvaluator>()> device;
    std::function<std::shared_ptr<const GpuSceneOcioContext>()> ocio;
    std::function<EvaluationResult(std::shared_ptr<const CompiledCompositionPlan> plan,
                                   const EvaluationRequest& request,
                                   const CancellationToken& cancellation)>
        evaluateCpu;
};

// Measures one frame: on the device when `preferDevice` and the device can, else with the CPU
// reference. Called on a CPU worker, never the UI thread.
[[nodiscard]] ScopeFrameMeasure measureScopeFrame(
    const ScopeFrameRoutes& routes, const std::shared_ptr<const CompiledCompositionPlan>& plan,
    const EvaluationRequest& request, const ScopeMeasureSettings& settings,
    const ScopeSignalColor& color, bool preferDevice, const CancellationToken& cancellation = {},
    const std::optional<std::filesystem::path>& assetBaseDirectory = {});

} // namespace bloom::runtime
