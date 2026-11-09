#pragma once

// #340 Levels, Curves and Hue/Saturation, #494 Grade, #506 Glow's Highlight Roll-off, #503
// Alpha Association and #504 Colorize: the one CPU reference of the grades, shared by the CPU evaluator and the
// resident GPU program generator. The host prepares one packed binary32 table per grade; the CPU
// applies it here and the GPU program reads the identical table from its storage buffer and runs
// the same binary32 operations, so a keyed value changes only data, never the shader. No grade
// performs a colour transform: a Raw data pass is graded as stored.

#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/render/ocio_gpu_program.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::runtime::detail {

struct ResolvedEvaluation;

enum class ColorGradeKind : std::uint8_t {
    Levels,
    Curves,
    HueSaturation,
    Grade,
    RollOff,
    AlphaAssociation,
    Colorize,
    Vignette,
    // #525: a field raster (field_raster.hpp); the table is a field program's, its geometry
    // appended where it is applied.
    FieldRaster,
    // #483: a depth map and Z Defocus's gather (depth_effect_image_effect.hpp), each with its
    // geometry appended where it is applied.
    DepthMap,
    Defocus,
    // #507: Lens Flare's stack and its probe of the light's sample disc
    // (lens_flare_image_effect.hpp), their tables and geometry built where they are applied.
    LensFlare,
    LensFlareProbe,
    // #486: Film Grain (film_grain_image_effect.hpp), its geometry appended where it is applied.
    FilmGrain,
    // #563: one pass of Codec Degrade (codec_degrade_image_effect.hpp), the geometry of the images it
    // reads appended where it is applied.
    CodecDegrade,
    // #566: one pass of Auto Exposure (auto_exposure_image_effect.hpp), the geometry of the images
    // it reads appended where it is applied.
    AutoExposure,
    // #484: every motion kernel (motion_blur_image_effect.hpp): the pass table of one pass of the
    // motion kernel, planned where it is applied.
    Motion,
    // #486: Sharpen (sharpen_image_effect.hpp), whose program is ImageCombineV1's Unsharp mode, not a
    // grade program of its own.
    Sharpen,
    // #481: Image Math, Image Map Range and Image Clamp (pixel_math_image_effect.hpp), the table of
    // one per-pixel function, with a second image's atlas geometry appended where it is applied.
    PixelMath,
    // #349: Histogram (histogram_image_effect.hpp), the geometry of the image it follows appended
    // where it is applied.
    Histogram,
    // #347: Audio Waveform and Audio Spectrum (audio_plot_image_effect.hpp), the prepared analysis
    // of the audio in its table.
    AudioPlot
};

// The packed table, always a whole number of vec4s:
//   Levels:  [clamp, 0, 0, 0], then per channel (master, red, green, blue, alpha)
//            [pivot, scale, inverse gamma, output black] [output range, step, identity, 0]
//   Curves:  see kCurvesTableHeader below (#493: any number of points per curve, hue curves)
//   Hue/Saturation: the three rows of the RGB matrix [m0, m1, m2, 0]
//   Grade (#494): [slope r, g, b, 0] [offset r, g, b, 0] [power r, g, b, 0]
//            [contrast, contrast scale, saturation, 0] [luma weights r, g, b, 0]
//   Highlight Roll-off (#506): [amount, luma weights r, g, b]; associated colour c becomes
//            c / fma(amount, max(L, 0), 1) with L = fma(b, wb, fma(g, wg, wr * r)), alpha unchanged
//   Alpha Association (#503): [table mode, epsilon, 0, 0] [background r, g, b, 0], table mode
//            0 Unmult over black, 1 Unmult over white, 2 Premultiply, 3 Unpremultiply, 4 Remove
//            Color Matting (docs/architecture/image-filters.md, "Alpha Association")
//   Colorize (#504): [table mode, amount, source, cycle] [repetitions, phase, midpoint, 0]
//            [1 / midpoint, 1 / (1 - midpoint), 0, 0] [luma weights r, g, b, 0], then four colours
//            [r, g, b, 0] A, B, C, D and the Black & White weights [reds, greens, blues, 0]
//            (by the largest channel) and [cyans, magentas, yellows, 0] (by the smallest). Table
//            mode 0 Fill (A the colour), 1 Tint (A + L * B), 2 ramp (A + u * B below the midpoint,
//            C + u * D above it), 3 Black & White (docs/architecture/image-filters.md, "Colorize")
//   Vignette (#505): [table mode, strength, protection, hard edge] [inner, 1 / (outer - inner),
//            exponent, 1 / exponent] [luma weights r, g, b, ellipse] [protection low,
//            1 / (high - low), circle, 0] [centre offset x, y, 0, 0], table mode 0 Multiply
//            (strength the amount) or 1 Exposure (strength the stops). Where it is applied the
//            input's geometry is appended (vignetteProgramData): [centre x, centre y, scale x,
//            scale y] in the input data window's pixel indices, so the program and the CPU read
//            the same 24 floats (docs/architecture/image-filters.md, "Vignette")
// A Levels channel maps x to t = (x - pivot) * scale (or the step x >= pivot when the input range
// is empty), raises a positive t to the inverse gamma, and returns fma(t, range, black). A Grade
// channel maps the straight value x to t = fma(x, slope, offset), raises a positive t to its power,
// then a positive t to the contrast as scale * pow(t, contrast); the saturation then moves the
// three results from their luma, fma(t - L, saturation, L). A Curves
// curve selects segment s = the number of knots <= x (a binary search), measures dx from the
// segment's origin and evaluates fma(dx, fma(dx, fma(dx, d, c), b), a).
//
// Curves table: vec4 0 [clamp, hue curves active, 0, 0]; 1 the luma weights; 2 and 3 the rows
// mapping RGB to the hue plane (a, b); 4 and 5 the columns e1 and e2 mapping (a, b) back to RGB
// (bloom/runtime/grade_curve.hpp); then one header per curve in document::kGradeCurves order,
// [point count n, identity, base, ends (0 linear, 1 hold, 2 periodic)]. A curve that is not the
// identity stores at vec4 `base` its n knots, then its n + 1 segment origins, then its n + 1
// segments [a, b, c, d], each list padded to whole vec4s.
inline constexpr std::size_t kCurvesTableHeader = 6 + document::kGradeCurveCount;
struct PreparedColorGrade final {
    ColorGradeKind kind = ColorGradeKind::Levels;
    std::vector<float> data;
    // Every channel is the exact identity and nothing clamps: the input passes through unchanged.
    bool identity = false;
    friend bool operator==(const PreparedColorGrade&, const PreparedColorGrade&) = default;
};

// The kernel's operands in forEachColorGradeOperand order, a Vector 2 as (x, y); for Curves each
// curve in kGradeCurves order as its point count followed by its (x, y) pairs, sampled at the
// operation's clock time `time`. Null when one is not evaluable this frame.
[[nodiscard]] std::optional<std::vector<double>>
sampleColorGradeOperands(const ImageEffectKernel& kernel, const CompiledCompositionPlan& plan,
                         const ResolvedEvaluation& resolved,
                         std::optional<core::RationalTime> time);
// The same operands without the analysis an Audio Waveform or Spectrum then reads (nothing is
// looked up), so a worker can name the analysis to prepare. Equal to the above for every other
// kernel.
[[nodiscard]] std::optional<std::vector<double>>
sampleColorGradeOperandsOnly(const ImageEffectKernel& kernel, const CompiledCompositionPlan& plan,
                             const ResolvedEvaluation& resolved,
                             std::optional<core::RationalTime> time);
// Null for a non-grade kernel, a value outside its document domain (non-finite, |v| > 1e6, gamma
// outside [0.01, 100], saturation outside [0, 1000], a curve outside its domain) or Curves points
// too close to represent.
[[nodiscard]] std::optional<PreparedColorGrade> prepareColorGrade(const ImageEffectKernel& kernel,
                                                                  std::span<const double> values);
// One premultiplied pixel, in place. False when the result (or the straight colour Levels and
// Curves grade) is not finite.
[[nodiscard]] bool colorGradePixel(const PreparedColorGrade& grade,
                                   std::array<float, 4>& pixel) noexcept;
// #493 Curves (color_grade_curves.cpp): the table from sampled operands, one pixel, the table's
// structural check, and the fixed GPU program text that reads it.
[[nodiscard]] std::optional<PreparedColorGrade> prepareCurvesGrade(bool clamp,
                                                                   std::span<const double> values);
[[nodiscard]] bool curvesGradePixel(const std::vector<float>& data,
                                    std::array<float, 4>& pixel) noexcept;
[[nodiscard]] bool validCurvesTable(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& curvesProgramText();
// #505 Vignette (vignette_image_effect.cpp): the table from sampled operands, the full program
// data's structural check, and the fixed GPU program text that reads it.
[[nodiscard]] std::optional<PreparedColorGrade> prepareVignetteGrade(const VignetteKernel& kernel,
                                                                     std::span<const double> values);
[[nodiscard]] bool validVignetteProgramData(const std::vector<float>& data) noexcept;
[[nodiscard]] const std::string& vignetteProgramText();
// The prepared table as cache identity bytes.
[[nodiscard]] std::string colorGradeIdentity(const PreparedColorGrade& grade);

// #505 Vignette: the prepared table with the input's geometry appended, the 24 floats the CPU
// reference and the GPU program both read. `display` and `dataOriginX/Y` are at the evaluation
// resolution; `horizontalScale` and `verticalScale` scale the full-resolution centre offset to it.
// Null when the table is not a vignette's or the geometry cannot be represented.
inline constexpr std::size_t kVignetteTableFloats = 20;
inline constexpr std::size_t kVignetteProgramFloats = 24;
[[nodiscard]] std::optional<std::vector<float>>
vignetteProgramData(const PreparedColorGrade& grade, const render::ImageWindow& display,
                    std::int64_t dataOriginX, std::int64_t dataOriginY, double pixelAspect,
                    double horizontalScale, double verticalScale);
// One premultiplied pixel at the data window index (x, y), in place. False when not finite.
[[nodiscard]] bool vignettePixel(const std::vector<float>& data, std::int64_t x, std::int64_t y,
                                 std::array<float, 4>& pixel) noexcept;

struct ImageEffectResult;
// Grades every pixel of the input's data window; the windows and pixel aspect are preserved. A
// Vignette reads the input's display window and the evaluation scale.
[[nodiscard]] ImageEffectResult applyColorGrade(const PreparedColorGrade&,
                                                const render::Rgba32fImage&, std::size_t budget,
                                                const CancellationToken&, OperationIndex,
                                                const EvaluationProgressCallback&,
                                                double horizontalScale = 1.0,
                                                double verticalScale = 1.0);
// One fixed neighbourhood program per kind; the table is its storage-buffer data.
[[nodiscard]] render::OcioGpuProgramResult buildColorGradeProgram(ColorGradeKind kind,
                                                                  const std::vector<float>& data);
} // namespace bloom::runtime::detail
