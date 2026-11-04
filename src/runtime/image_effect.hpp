#pragma once

#include "canvas_image_effect.hpp"
#include "channel_image_effect.hpp"
#include "color_grade_image_effect.hpp"
#include "directional_blur_image_effect.hpp"
#include "layer_mask_coverage.hpp"
#include "luma_key_image_effect.hpp"
#include "pointwise_image_effect.hpp"
#include "separable_image_effect.hpp"
#include "transform_2d_geometry.hpp"

#include <bloom/color/ocio_cpu_color_space_processor.hpp>
#include <bloom/color/ocio_cpu_file_transform_processor.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <mutex>
#include <unordered_map>

namespace bloom::runtime::detail {

struct PreparedImageEffect final {
    struct Combine final {
        ImageCombineMode mode = ImageCombineMode::Mix;
        float factor = 1.0F;
        render::MatteSelection matte{};
    };
    std::optional<Combine> combine;
    std::optional<PointwiseAffine> pointwise;
    std::optional<CanvasMargins> canvas;
    std::optional<DiscreteImageKernel> discrete;
    std::optional<ResizeFitKernel> resizeFit;
    std::optional<PreparedSeparableBlur> blur;
    std::optional<PreparedTransform2D> transform2D;
    // #338 Luma Key: never an identity, since it turns an Image into a Mask.
    std::optional<PreparedLumaKey> lumaKey;
    // #340 Levels, Curves and Hue/Saturation (absent for an identity grade).
    std::optional<PreparedColorGrade> colorGrade;
    // #479 Separate Channels and Combine Channels (absent for an identity Channel Select).
    std::optional<PreparedChannelSelect> channelSelect;
    std::optional<PreparedChannelCombine> channelCombine;
    // #506 Directional Blur (absent for a zero spacing, which aliases its input).
    std::optional<PreparedDirectionalBlur> directionalBlur;
    // ADR 0028: prepared layer-mask geometry (null for every other kernel).
    std::shared_ptr<const PreparedLayerMasks> masks;
    std::shared_ptr<const color::CpuColorSpaceProcessor> processor;
    std::shared_ptr<const color::CpuColorSpaceProcessor> afterProcessor;
    std::shared_ptr<const color::CpuFileTransformProcessor> fileProcessor;
    std::optional<EvaluationDiagnostic> diagnostic;
    std::string cacheIdentity;
    bool cancelled = false;
    [[nodiscard]] bool identity() const noexcept {
        if (resizeFit || lumaKey || colorGrade || channelSelect || channelCombine ||
            directionalBlur)
            return false;
        return !diagnostic && !cancelled
                   ? (!combine || (combine->mode == ImageCombineMode::Mix &&
                                   combine->factor == 0.0F)) &&
                         (!canvas || canvas->identity()) && (!masks || masks->masks.empty()) &&
                         (!discrete || discrete->identity()) &&
                         (!pointwise || pointwise->identity()) && (!blur || blur->identity()) &&
                         (!transform2D || transform2D->identity()) &&
                         (!processor || processor->isIdentity()) &&
                         (!afterProcessor || afterProcessor->isIdentity()) &&
                         (!fileProcessor || fileProcessor->isIdentity())
                   : true;
    }
};

// Prepared built-in transforms are shared across nodes and frames by exact config/from/to.
// No OCIO work runs on the UI thread. The bounded cache belongs to the evaluator session.
class ImageEffectContext final {
  public:
    [[nodiscard]] PreparedImageEffect
    prepare(const CompiledImageEffect& effect, const EvaluationColorIntent& intent,
            const std::filesystem::path& base = {}, const CancellationToken& cancellation = {},
            std::optional<double> sampledAmount = {},
            std::optional<std::array<double, 4>> canvasMargins = {}, double horizontalScale = 1.0,
            double verticalScale = 1.0, std::optional<std::int64_t> sampledChannel = {},
            std::shared_ptr<const PreparedLayerMasks> layerMasks = {},
            std::optional<Transform2DValues> transform2D = {},
            std::optional<std::array<double, 2>> lumaKeyRange = {},
            std::optional<std::vector<double>> gradeValues = {},
            std::optional<std::array<double, 2>> directionalValues = {});

  private:
    std::mutex mutex_;
    std::unordered_map<std::string, PreparedImageEffect> processors_;
};

struct ImageEffectResult final {
    std::shared_ptr<const render::Rgba32fImage> image;
    std::optional<EvaluationDiagnostic> diagnostic;
    bool cancelled = false;
    // A successful evaluation that publishes no image (a degenerate or fully clipped Transform 2D).
    bool empty = false;
};

[[nodiscard]] ImageEffectResult
applyImageEffect(const PreparedImageEffect& effect, const render::Rgba32fImage& input,
                 const render::Rgba32fImage* secondary,
                 std::size_t pixelBudget, const CancellationToken& cancellation,
                 OperationIndex operation, const EvaluationProgressCallback& progress,
                 double horizontalScale = 1.0, double verticalScale = 1.0,
                 // ADR 0028 Image Mix mask: absent = no mask; engaged null = a zero (absent) mask.
                 std::optional<const render::Rgba32fImage*> mask = std::nullopt);
} // namespace bloom::runtime::detail
