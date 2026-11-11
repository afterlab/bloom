#pragma once

// #349 Histogram: a grade kind whose table gains the geometry of the image it reads where it is
// applied. Three passes share one table and one program: a count of the pixels of each tile of the
// frame in each bin, the sum of those tile counts, and the plot of the sums. The CPU reference
// (applyHistogram) and the GPU program (histogramProgramText) run the same binary32 operations in
// the same order over the same table, so the counts, which are whole numbers, agree exactly, and so
// does the plot: a logarithmic graph takes bloom::core::specifiedLog2 on both, not a library's
// log2. docs/architecture/image-filters.md, "Histogram", is the specification.

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

enum class HistogramStage : std::uint8_t { Bin = 0, Sum = 1, Plot = 2 };

// The table without geometry (kHistogramPreparedFloats):
//   0 stage  1 grid (Bin)  2 bins  3 graph height  4 flags (1 straight, 8 logarithmic, 4 empty range)
//   5 range minimum  6 range maximum  7 bins per unit of the range  8 counted channels (1..4)
//   9..12 the source of each counted channel (HistogramSource)  16..27 the color of each, r g b
// and, with the geometry of the image the pass follows (kHistogramProgramFloats),
//   32 source width  33 source height  34 tile width  35 tile height  36 tiles across  37 tiles down
//   38 output width  39 output height  40 groups of three bins per channel
// Flag 2 was the logarithmic graph drawn with each backend's own log2; it is retired and refused.
inline constexpr std::size_t kHistogramPreparedFloats = 32;
inline constexpr std::size_t kHistogramProgramFloats = 48;
inline constexpr std::int64_t kHistogramMaxChannels = 4;

// What a counted channel reads.
enum class HistogramSource : std::uint8_t { Red = 0, Green = 1, Blue = 2, Alpha = 3, Luminance = 4 };

// Three bins fit one pixel's red, green and blue (an image's alpha is limited to [0, 1]), so a
// channel's bins are `histogramGroups(bins)` pixels wide.
[[nodiscard]] std::int64_t histogramGroups(std::int64_t bins) noexcept;

// The bin a value is counted in, or -1 when it is not counted: not finite, or outside the range
// (both ends are in).
[[nodiscard]] int histogramBin(float value, float low, float high, float scale, int bins) noexcept;

[[nodiscard]] std::optional<PreparedColorGrade> prepareHistogramGrade(const HistogramKernel& kernel,
                                                                      std::span<const double> values);
[[nodiscard]] HistogramStage histogramStage(const PreparedColorGrade& grade) noexcept;

// The size of the image the pass publishes, from the size of the one it follows.
[[nodiscard]] std::array<std::int64_t, 2> histogramOutputSize(const PreparedColorGrade& grade,
                                                              std::int64_t sourceWidth,
                                                              std::int64_t sourceHeight) noexcept;
// The table with the geometry of the followed image appended.
[[nodiscard]] std::optional<std::vector<float>>
histogramProgramData(const PreparedColorGrade& grade, std::int64_t sourceWidth,
                     std::int64_t sourceHeight);
[[nodiscard]] bool validHistogramProgramData(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& histogramProgramText();

// The CPU reference. Every pass follows `input`; the result is none when it is null.
[[nodiscard]] ImageEffectResult applyHistogram(const PreparedColorGrade& grade,
                                               const render::Rgba32fImage* input,
                                               std::size_t budget, const CancellationToken&,
                                               OperationIndex, const EvaluationProgressCallback&,
                                               CpuRowBandExecutor* rowBands);

} // namespace bloom::runtime::detail
