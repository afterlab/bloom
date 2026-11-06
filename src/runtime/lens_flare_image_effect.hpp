#pragma once

// #507 Lens Flare: the CPU reference of the element stack and of the light's probes, shared by the
// CPU evaluator and the resident GPU program generator. Both are grade kinds
// (color_grade_image_effect.hpp): the host prepares one table of the sampled operands; where the
// flare is applied it builds the program data from that table and the image's geometry, and the CPU
// (lensFlarePixel, lensFlareProbeValue) and the two fixed GPU programs run the same binary32
// operations over it. The stack uses only additions, multiplications, min, max and abs, each exact
// on every device, so CPU and GPU agree within 2e-6 on RGB with exact alpha.
// docs/architecture/image-filters.md, "Lens Flare", is the specification.

#include "color_grade_image_effect.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/row_band_execution.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

struct ImageEffectResult;

// The operands: position (2), colour (4), anamorphic colour (4), the Scalars, then the constants.
inline constexpr std::size_t kLensFlareOperands = 10 + document::kLensFlareScalarCount;
inline constexpr std::size_t kLensFlareTableFloats = kLensFlareOperands + 7;
// The program data: whole vec4s (lens_flare_image_effect.cpp lists them).
inline constexpr std::size_t kLensFlareProgramFloats = 122 * 4;
// A probe's data: its header, its channel weights and one vec4 per sample.
inline constexpr std::size_t kLensFlareProbeSamples = 32;
inline constexpr std::size_t kLensFlareProbeFloats = (2 + kLensFlareProbeSamples) * 4;

[[nodiscard]] std::optional<PreparedColorGrade> prepareLensFlareGrade(const LensFlareKernel&,
                                                                      std::span<const double>);

// --- The probes ---------------------------------------------------------------------------------

// What a flare reads at the light, at the evaluation resolution: the disc's window (every sample
// lies inside it, its origin the program's (0, 0)) and each probe's program data.
struct LensFlareProbes final {
    render::ImageWindow window;
    // Whether the flare reads the input's brightness and its occlusion matte.
    bool source = false;
    bool occlusion = false;
    std::vector<float> sourceData;
    std::vector<float> occlusionData;
};
// Null when the light's pixel cannot be represented. `hasOcclusion`: a matte is connected.
[[nodiscard]] std::optional<LensFlareProbes>
lensFlareProbes(const PreparedColorGrade& grade, const render::ImageWindow& display,
                double horizontalScale, double verticalScale, bool hasOcclusion);
[[nodiscard]] bool validLensFlareProbeData(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& lensFlareProbeProgramText();
// The probe of `image` (null: no pixels, every sample transparent) over `window`, as its program
// computes it over `window` cut out of the image.
[[nodiscard]] float lensFlareProbeValue(const std::vector<float>& data,
                                        const render::ImageWindow& window,
                                        const render::Rgba32fImage* image);

// --- The stack ----------------------------------------------------------------------------------

// A probe as the program reads it: its value in the dispatched image at (x, y), or `value` when x
// is negative. A probe that is not `active` is not read at all (no brightness from the source, no
// matte connected).
struct LensFlareProbeRead final {
    bool active = false;
    float x = -1.0F;
    float y = -1.0F;
    float value = 0.0F;
};
// The output window: the input's data window joined with its display window, so the light reaches
// the whole frame. Null when that overflows.
[[nodiscard]] std::optional<render::ImageWindow>
lensFlareOutputWindow(const render::ImageWindow& data, const render::ImageWindow& display);
// The program data. `output` is where index (0, 0) of the dispatched image lies; the stack draws
// over its first output-window pixels and copies the rest. Null when the geometry cannot be
// represented.
[[nodiscard]] std::optional<std::vector<float>>
lensFlareProgramData(const PreparedColorGrade& grade, const render::ImageWindow& output,
                     const render::ImageWindow& display, double pixelAspect, double horizontalScale,
                     double verticalScale, const LensFlareProbeRead& source,
                     const LensFlareProbeRead& occlusion);
[[nodiscard]] bool validLensFlareProgramData(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& lensFlareProgramText();
// What the probes make of the stack's brightness: the program's own arithmetic over the probes'
// values, the same for every pixel.
[[nodiscard]] float lensFlareLevel(const float* data, float sourceValue,
                                   float occlusionValue) noexcept;
// One premultiplied pixel at the output index (x, y), in place. False when not finite.
[[nodiscard]] bool lensFlarePixel(const float* data, float level, std::int64_t x, std::int64_t y,
                                  std::array<float, 4>& pixel) noexcept;
// The CPU reference: the flare over the input's output window. `occlusion` is the matte's image
// (null: no pixels); `hasOcclusion` whether a matte is connected at all.
[[nodiscard]] ImageEffectResult
applyLensFlare(const PreparedColorGrade& grade, const render::Rgba32fImage& input,
               const render::Rgba32fImage* occlusion, bool hasOcclusion, double horizontalScale,
               double verticalScale, std::size_t budget, const CancellationToken&, OperationIndex,
               const EvaluationProgressCallback&, CpuRowBandExecutor* rowBands);

} // namespace bloom::runtime::detail
