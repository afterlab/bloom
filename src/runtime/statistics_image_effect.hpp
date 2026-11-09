#pragma once

// #349 Image Statistics: a grade kind whose table gains the geometry of the image it reads where it
// is applied. Two passes share one table and one program: a reduction of every tile of the frame to
// its count, minimum, maximum, mean and sum of squared deviations, and the combination of the tiles
// in a fixed order into the frame's statistics. The CPU reference (applyStatistics) and the GPU
// program (statisticsProgramText) run the same binary32 operations in the same order over the same
// table, so the two agree exactly; the standard deviation's square root is taken on the host from the
// binary32 variance. docs/architecture/image-statistics.md is the specification.

#include "color_grade_image_effect.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/row_band_execution.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

struct ImageEffectResult;

enum class StatisticsStage : std::uint8_t { Reduce = 0, Sum = 1 };

// The table without geometry (kStatisticsPreparedFloats):
//   0 stage  1 grid (Reduce)  2 channel (document::kStatistics*)  3 flags (1 straight, 2 only
//   pixels with alpha)
// and, with the geometry of the image the pass follows (kStatisticsProgramFloats),
//   32 source width  33 source height  34 tile width  35 tile height  36 tiles across
//   37 tiles down  38 output width  39 output height
inline constexpr std::size_t kStatisticsPreparedFloats = 16;
inline constexpr std::size_t kStatisticsProgramFloats = 48;
// A value whose magnitude is above this counts as not finite, so that no square or sum of squares
// overflows binary32 on any frame: the sentinel a data pass uses for "no data" is not data.
inline constexpr float kStatisticsValueLimit = 1.0e12F;

// The result image is this wide: texel 0 holds (count, minimum, maximum) and texel 1 holds (mean,
// variance, 0) of the whole frame. Between the passes, a tile is one row of this width.
inline constexpr std::int64_t kStatisticsResultWidth = 2;

[[nodiscard]] std::optional<PreparedColorGrade> prepareStatisticsGrade(const StatisticsKernel& kernel);
[[nodiscard]] StatisticsStage statisticsStage(const PreparedColorGrade& grade) noexcept;

// The size of the image the pass publishes, from the size of the one it follows.
[[nodiscard]] std::array<std::int64_t, 2> statisticsOutputSize(const PreparedColorGrade& grade,
                                                               std::int64_t sourceWidth,
                                                               std::int64_t sourceHeight) noexcept;
[[nodiscard]] std::optional<std::vector<float>>
statisticsProgramData(const PreparedColorGrade& grade, std::int64_t sourceWidth,
                      std::int64_t sourceHeight);
[[nodiscard]] bool validStatisticsProgramData(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& statisticsProgramText();

// The CPU reference. Every pass follows `input`; the result is none when it is null.
[[nodiscard]] ImageEffectResult applyStatistics(const PreparedColorGrade& grade,
                                                const render::Rgba32fImage* input,
                                                std::size_t budget, const CancellationToken&,
                                                OperationIndex, const EvaluationProgressCallback&,
                                                CpuRowBandExecutor* rowBands);

} // namespace bloom::runtime::detail
