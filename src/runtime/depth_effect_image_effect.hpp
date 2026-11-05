#pragma once

// #483 Depth effects: the CPU reference of the depth map (Z Defocus's circle of confusion, Depth
// Fog's fog factor) and of Z Defocus's gather, shared by the CPU evaluator and the resident GPU
// program generator. Both are grade kinds (color_grade_image_effect.hpp): the host prepares one
// packed binary32 table from the sampled operands, appends the geometry where it is applied, and
// the CPU (depthMapPixel, defocusPixel) and the fixed GPU programs run the same binary32 operations
// over it. docs/architecture/image-filters.md, "Depth effects", is the specification.

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

// --- The depth map ------------------------------------------------------------------------------

// The circle of confusion table: 24 floats of operands and 8 of geometry (depthMapProgramData).
inline constexpr std::size_t kDepthMapCocFloats = 32;
// The fog table: 16 floats, then the 257-entry curve table padded to whole vec4s.
inline constexpr std::size_t kDepthFogCurveEntries = 257;
inline constexpr std::size_t kDepthFogFloats = 16 + 260;

[[nodiscard]] std::optional<PreparedColorGrade> prepareDepthMapGrade(const DepthMapKernel&,
                                                                     std::span<const double>);
// The table with the input's geometry: its data window, display window and the evaluation scale.
// A fog table has none and is returned as it is. Null when the geometry cannot be represented.
[[nodiscard]] std::optional<std::vector<float>>
depthMapProgramData(const PreparedColorGrade& grade, const render::ImageWindow& data,
                    const render::ImageWindow& display, double horizontalScale);
[[nodiscard]] bool validDepthMapProgramData(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& depthMapProgramText();
// The CPU reference over every pixel of the input's data window (windows and aspect kept).
[[nodiscard]] ImageEffectResult applyDepthMap(const PreparedColorGrade& grade,
                                              const render::Rgba32fImage& input,
                                              double horizontalScale, std::size_t budget,
                                              const CancellationToken&, OperationIndex,
                                              const EvaluationProgressCallback&,
                                              CpuRowBandExecutor* rowBands);

// --- The defocus gather -------------------------------------------------------------------------

// The header (operands, then the geometry depthMapProgramData fills), then per sample of the ring
// pattern two vec4s, then the coverage density table.
inline constexpr std::size_t kDefocusHeaderFloats = 64;

[[nodiscard]] std::optional<PreparedColorGrade> prepareDefocusGrade(const DefocusKernel&,
                                                                    std::span<const double>);
// Where the gather's images lie: the colour input's data window, each slot's (width 0: absent)
// and, on the GPU, each slot's position in the dispatched atlas (whose index (0, 0) is the output
// window's origin). Windows are at the evaluation resolution.
struct DefocusPlacement final {
    struct Region final {
        std::int64_t originX = 0;
        std::int64_t originY = 0;
        std::int64_t width = 0;
        std::int64_t height = 0;
        std::int64_t atlasX = 0;
        std::int64_t atlasY = 0;
    };
    Region color;
    std::array<Region, kDefocusSlots> slots{};
};
// The widest reach of the gather in evaluation pixels: the output is the colour's data window
// grown by it on every side.
[[nodiscard]] std::int64_t defocusReach(const PreparedColorGrade& grade, double horizontalScale);
// Whether the prefiltered colour can contribute: only when the widest ring spacing is over one
// pixel (otherwise its mix is zero at every pixel, defocusProgramData's prefilter rate).
[[nodiscard]] bool defocusPrefilterUsed(const PreparedColorGrade& grade, double horizontalScale);
[[nodiscard]] std::optional<std::vector<float>>
defocusProgramData(const PreparedColorGrade& grade, const DefocusPlacement& placement,
                   double horizontalScale);
[[nodiscard]] bool validDefocusProgramData(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& defocusProgramText();
// The CPU reference: the colour input gathered over its grown data window; `slots` in
// DefocusKernel::slotImages order (null: absent).
[[nodiscard]] ImageEffectResult applyDefocus(const PreparedColorGrade& grade,
                                             const render::Rgba32fImage& color,
                                             std::span<const render::Rgba32fImage* const> slots,
                                             double horizontalScale, std::size_t budget,
                                             const CancellationToken&, OperationIndex,
                                             const EvaluationProgressCallback&,
                                             CpuRowBandExecutor* rowBands);

} // namespace bloom::runtime::detail
