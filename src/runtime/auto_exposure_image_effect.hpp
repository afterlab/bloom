#pragma once

// #566 Auto Exposure: a grade kind whose table gains the geometry of the images it reads where it is
// applied. Three passes share one table and one program: a mean reduction by tile sums, the
// recursive filter over a look-back window's means, and the gain applied to the frame. The CPU
// reference (applyAutoExposure) and the GPU program (autoExposureProgramText) run the same binary32
// operations in the same order over the same table; the logarithm and the exponential are each
// backend's own, so the two agree to within their last bits. docs/architecture/image-filters.md,
// "Auto Exposure", is the specification.

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

enum class AutoExposureStage : std::uint8_t { Reduce = 0, Filter = 1, Apply = 2 };

// The prepared table without geometry (kAutoExposurePreparedFloats): the pass, the grid, and the
// Filter's amounts, limits and lag coefficients.
inline constexpr std::size_t kAutoExposurePreparedFloats = 16;
// The table with geometry: a header, then the atlas position of each filtered mean.
inline constexpr std::size_t kAutoExposureProgramFloats = 160;
// The most means one Filter reads: the look-back window (document::kMaximumAutoExposureLookBack).
inline constexpr std::size_t kAutoExposureMaxCells = 64;
// The filter removes the oldest sample's remaining weight only while that weight is below this
// (1 - 1/256); above it the lag is too long for the window to separate it. The GLSL repeats the value.
inline constexpr float kAutoExposureSeedWeightLimit = 0.99609375F;
// A channel's mean is never taken below this before it is compared, so black has a finite log.
inline constexpr float kAutoExposureFloor = 1.0e-6F;

// The grid of tile sums a Reduce of a `width` by `height` frame makes into at most `grid` by `grid`:
// every tile is `tileWidth` by `tileHeight` pixels but for the last row and column.
struct AutoExposureGrid final {
    std::int64_t tileWidth = 1;
    std::int64_t tileHeight = 1;
    std::int64_t width = 1;
    std::int64_t height = 1;
};
[[nodiscard]] AutoExposureGrid autoExposureGrid(std::int64_t width, std::int64_t height,
                                                std::int64_t grid) noexcept;

// `values`: a Filter's nine scalars in document::autoExposureScalarIndex order, then the length of
// a frame in seconds (sampleColorGradeOperands); a Reduce's or an Apply's none.
[[nodiscard]] std::optional<PreparedColorGrade>
prepareAutoExposureGrade(const AutoExposureKernel& kernel, std::span<const double> values);
[[nodiscard]] AutoExposureStage autoExposureStage(const PreparedColorGrade& grade) noexcept;

// Where the images the pass reads lie in the dispatched atlas, whose index (0, 0) is the output
// window's origin. `cells` are a Filter's means, oldest first, the last being the output's own;
// (-1, -1) is an absent one.
struct AutoExposurePlacement final {
    std::int64_t sourceWidth = 0;
    std::int64_t sourceHeight = 0;
    bool hasState = false;
    std::int64_t stateX = 0;
    std::int64_t stateY = 0;
    std::vector<std::array<std::int64_t, 2>> cells;
};
// The size of the window the pass publishes, from the size of the image it follows.
[[nodiscard]] std::array<std::int64_t, 2> autoExposureOutputSize(const PreparedColorGrade& grade,
                                                                 std::int64_t sourceWidth,
                                                                 std::int64_t sourceHeight) noexcept;
[[nodiscard]] std::optional<std::vector<float>>
autoExposureProgramData(const PreparedColorGrade& grade, const AutoExposurePlacement& placement);
[[nodiscard]] bool validAutoExposureProgramData(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& autoExposureProgramText();

// One mean colour (the premultiplied red, green and blue) of a frame, and the gains the filter holds
// in stops: the exposure, then the red and the blue against the green.
using AutoExposureMean = std::array<float, 3>;
using AutoExposureGains = std::array<float, 3>;
// The reference of the Filter: `means` oldest first, an absent mean holding the filter.
[[nodiscard]] AutoExposureGains
autoExposureFilter(const std::vector<std::optional<AutoExposureMean>>& means,
                   const std::vector<float>& table) noexcept;

// The CPU reference. A Reduce and an Apply follow `input`; a Filter follows its newest mean, `input`,
// and reads `images`, its older means oldest first (null: absent). An Apply reads its Filter's pixel
// in images[0] (null: no gain). The result is none when `input` is null.
[[nodiscard]] ImageEffectResult
applyAutoExposure(const PreparedColorGrade& grade, const render::Rgba32fImage* input,
                  std::span<const render::Rgba32fImage* const> images, std::size_t budget,
                  const CancellationToken&, OperationIndex, const EvaluationProgressCallback&,
                  CpuRowBandExecutor* rowBands);

} // namespace bloom::runtime::detail
