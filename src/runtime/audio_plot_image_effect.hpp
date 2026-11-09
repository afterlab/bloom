#pragma once

// #347 Audio Waveform and Audio Spectrum: a grade kind whose table carries the prepared analysis of
// a window of audio (audio_analysis.hpp) and whose one pass draws it over the canvas it follows. The
// CPU reference (applyAudioPlot) and the GPU program (audioPlotProgramText) run the same binary32
// operations, in the same order, over the same table, so the two backends agree to the bit: no
// transcendental function is used by the pass, and every product is rounded before it is added.
// docs/architecture/media-io.md, "Audio analysis", is the specification.

#include "color_grade_image_effect.hpp"

#include <bloom/render/image.hpp>
#include <bloom/runtime/audio_analysis.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/row_band_execution.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

struct ImageEffectResult;

// The table (a whole number of vec4s):
//   0 kind (0 Waveform, 1 Spectrum)  1 width  2 height  3 lanes  4 flags (1 separate)  5 scale
//   6..9 the line or bars, premultiplied r g b a   10..13 the fill behind, premultiplied r g b a
//   14, 15 reserved, then from kAudioPlotHeaderFloats the analysis:
//   Waveform [column][lane][minimum, maximum]   Spectrum [column] one level in [0, 1]
inline constexpr std::size_t kAudioPlotHeaderFloats = 16;

// The longest side of a plot the node draws.
inline constexpr std::int64_t kAudioPlotMaximumSide = 4096;
inline constexpr std::int64_t kAudioPlotMinimumSide = 16;

// The operands the kernel reads from its sampled values, before the two colors: 2 for a Waveform
// (window seconds, scale), 4 for a Spectrum (low and high hertz, floor and ceiling decibels).
[[nodiscard]] std::size_t audioPlotScalarCount(const AudioPlotKernel& kernel) noexcept;

// The analysis the pass reads, from the operands sampled for it (the kernel's scalars, then its two
// colors as straight r g b a), or none when an operand is outside its domain.
[[nodiscard]] std::optional<AudioAnalysisQuery>
audioPlotQuery(const AudioPlotKernel& kernel, std::span<const double> operands,
               core::RationalTime time);

// Prepares (on the calling worker) the analysis the pass reads at `time`: `operands` are its
// sampled operands without the analysis (sampleColorGradeOperandsOnly). Ready for an input with no
// audio wired, Unavailable with the reason for a source that cannot be read, and Cancelled.
[[nodiscard]] AudioAnalysisStatus prepareAudioPlotAnalysis(const AudioPlotKernel& kernel,
                                                           std::span<const double> operands,
                                                           std::optional<core::RationalTime> time,
                                                           const std::filesystem::path& assetBase,
                                                           const CancellationToken& cancellation);

// Appends [lanes, columns, values...] of the prepared analysis to the sampled operands. False when
// the analysis is not prepared (the caller prepares it on a worker with prepareAudioAnalysis).
// An input with no audio wired appends silence.
[[nodiscard]] bool appendAudioPlotAnalysis(const AudioPlotKernel& kernel,
                                           std::vector<double>& values, core::RationalTime time);

[[nodiscard]] std::optional<PreparedColorGrade> prepareAudioPlotGrade(const AudioPlotKernel& kernel,
                                                                      std::span<const double> values);
[[nodiscard]] bool validAudioPlotProgramData(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& audioPlotProgramText();

// The CPU reference. The result is none when `input` is null.
[[nodiscard]] ImageEffectResult applyAudioPlot(const PreparedColorGrade& grade,
                                               const render::Rgba32fImage* input, std::size_t budget,
                                               const CancellationToken&, OperationIndex,
                                               const EvaluationProgressCallback&,
                                               CpuRowBandExecutor* rowBands);

} // namespace bloom::runtime::detail
