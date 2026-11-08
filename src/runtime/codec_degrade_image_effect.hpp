#pragma once

// #563 Codec Degrade: a grade kind whose table gains the geometry of the images it reads where it
// is applied. The CPU reference (applyCodecDegrade) and the GPU program (codecDegradeProgramText)
// run the same binary32 operations in the same order over the same table, so they agree bit for
// bit on identical inputs. The step of the look-back chain codes one DCT block at a time; the GPU
// program codes the block its pixel lies in for every pixel, the CPU once for the block, by the
// same sums. docs/architecture/image-filters.md, "Codec Degrade", is the specification.

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

// What a pass does (the first float of every table).
enum class CodecStage : std::uint8_t {
    Encode = 0,    // premultiplied scene-linear to straight, clamped, display-referred colour
    Intra = 1,     // a keyframe: the Encoded input coded against mid grey
    Predicted = 2, // a predicted frame: the Encoded colour coded against the moved input
    Finish = 3     // display-referred straight colour to premultiplied scene-linear
};

// The table: a header, the DCT basis, the quantiser weights by u + v and their inverses, the
// colour matrix. Always a whole number of vec4s.
inline constexpr std::size_t kCodecDegradeProgramFloats = 368;
// The widest block the program codes.
inline constexpr int kCodecMaxBlock = 16;

// The prepared table without geometry (kCodecPreparedFloats): the pass, the block and chroma
// settings and the quality and persistence the pass reads, or nothing for a pass that is the exact
// identity (a step after the current frame).
inline constexpr std::size_t kCodecPreparedFloats = 8;
// `values`: a Step's quality and persistence and then the frame the pass is drawn for
// (sampleColorGradeOperands); an Encode's or Finish's none.
[[nodiscard]] std::optional<PreparedColorGrade>
prepareCodecDegradeGrade(const CodecDegradeKernel& kernel, std::span<const double> values);
[[nodiscard]] CodecStage codecDegradeStage(const PreparedColorGrade& grade) noexcept;

// Where the images the pass reads lie, at the evaluation resolution, and (on the GPU) where each
// is in the dispatched atlas, whose index (0, 0) is the output window's origin. Width 0: absent.
struct CodecRegion final {
    std::int64_t originX = 0;
    std::int64_t originY = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::int64_t atlasX = 0;
    std::int64_t atlasY = 0;
};
struct CodecPlacement final {
    // The image the output follows: the Encoded colour of a Predicted pass, otherwise the input.
    CodecRegion colour;
    // A Predicted pass's input, the chain so far, and its optical flow.
    CodecRegion previous;
    CodecRegion flow;
};
// The block, and the chroma step, the pass codes at this resolution scale.
[[nodiscard]] int codecBlock(const PreparedColorGrade& grade, double horizontalScale) noexcept;
[[nodiscard]] int codecChromaStep(const PreparedColorGrade& grade, double horizontalScale) noexcept;
[[nodiscard]] std::optional<std::vector<float>>
codecDegradeProgramData(const PreparedColorGrade& grade, const CodecPlacement& placement,
                        double horizontalScale);
[[nodiscard]] bool validCodecDegradeProgramData(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& codecDegradeProgramText();

// The CPU reference. `input` is the pass's input (null: absent); `colour` and `flow` are a
// Predicted pass's Encoded colour and flow (null: absent). The result follows `colour` for a
// Predicted pass and `input` otherwise; none when that image is absent.
[[nodiscard]] ImageEffectResult
applyCodecDegrade(const PreparedColorGrade& grade, const render::Rgba32fImage* input,
                  const render::Rgba32fImage* colour, const render::Rgba32fImage* flow,
                  double horizontalScale, std::size_t budget, const CancellationToken&,
                  OperationIndex, const EvaluationProgressCallback&, CpuRowBandExecutor* rowBands);

} // namespace bloom::runtime::detail
