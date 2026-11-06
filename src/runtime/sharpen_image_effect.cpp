// #486 Sharpen. docs/architecture/image-filters.md, "Sharpen", is the specification.

#include "sharpen_image_effect.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <bloom/document/sharpen_nodes.hpp>
#include <bloom/render/image_coverage.hpp>

#include <array>
#include <cmath>
#include <string>

namespace bloom::runtime::detail {
namespace {

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic =
        EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, std::string(text), {}};
    return result;
}

} // namespace

std::optional<PreparedColorGrade> prepareSharpenGrade(const SharpenKernel& kernel,
                                                      const std::span<const double> values) {
    static_cast<void>(kernel);
    if (values.size() != 3)
        return std::nullopt;
    constexpr std::array roles{"amount", "threshold", "radius"};
    for (std::size_t index = 0; index < roles.size(); ++index) {
        const auto* spec = document::sharpenParameterSpec(std::string("bloom.sharpen.") + roles[index]);
        if (spec == nullptr || !std::isfinite(values[index]) || values[index] < spec->minimum ||
            values[index] > spec->maximum)
            return std::nullopt;
    }
    PreparedColorGrade grade{
        ColorGradeKind::Sharpen,
        {static_cast<float>(values[0]), static_cast<float>(values[1])},
        false};
    grade.identity = grade.data[0] == 0.0F || values[2] == 0.0;
    return grade;
}

float sharpenAmount(const PreparedColorGrade& grade) noexcept { return grade.data[0]; }
float sharpenThreshold(const PreparedColorGrade& grade) noexcept { return grade.data[1]; }

ImageEffectResult applySharpen(const PreparedColorGrade& grade, const render::Rgba32fImage& input,
                               const render::Rgba32fImage* const blurred, const std::size_t budget,
                               const CancellationToken& cancellation,
                               const OperationIndex operation,
                               const EvaluationProgressCallback& progress,
                               CpuRowBandExecutor* const rowBands) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    const auto& descriptor = *input.descriptor();
    const auto window = descriptor.dataWindow();
    auto builder = render::Rgba32fImageBuilder::create(descriptor, budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Sharpen output exceeds the pixel budget");
    const auto view = input.view();
    if (!view)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Sharpen image is invalid");
    std::optional<render::Rgba32fImageView> blurredView;
    std::optional<render::ImageWindow> blurredWindow;
    if (blurred != nullptr) {
        const auto fetched = blurred->view();
        if (!fetched)
            return failed(EvaluationDiagnosticCode::InvalidPixel, "Sharpen blur is invalid");
        blurredView.emplace(*fetched.value());
        blurredWindow = blurred->descriptor()->dataWindow();
    }
    auto& image = *builder.value();
    const auto& sourceView = *view.value();
    const float amount = sharpenAmount(grade);
    const float threshold = sharpenThreshold(grade);
    const auto total = static_cast<std::uint32_t>(window.extent().height());
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, total});
    const auto outcome = runRowBandPass(
        rowBands, cancellation, total, window.originY(),
        [&](const std::int64_t y) -> std::optional<EvaluationDiagnostic> {
            const auto source = sourceView.row(y);
            auto target = image.row(y);
            if (!source || !target)
                return EvaluationDiagnostic{EvaluationDiagnosticCode::InternalInvariant,
                                            DiagnosticSeverity::Error,
                                            {},
                                            "Sharpen row is unavailable",
                                            {}};
            std::optional<std::span<const render::Rgba32f>> blurredRow;
            if (blurredWindow && y >= blurredWindow->originY() &&
                y < blurredWindow->maxYExclusive()) {
                const auto fetched = blurredView->row(y);
                if (!fetched)
                    return EvaluationDiagnostic{EvaluationDiagnosticCode::InternalInvariant,
                                                DiagnosticSeverity::Error,
                                                {},
                                                "Sharpen blur row is unavailable",
                                                {}};
                blurredRow = *fetched.value();
            }
            for (std::size_t column = 0; column < source.value()->size(); ++column) {
                if ((column & 255U) == 0 && cancellation.isCancellationRequested())
                    return std::nullopt;
                const auto x = window.originX() + static_cast<std::int64_t>(column);
                const auto tap =
                    blurredRow && x >= blurredWindow->originX() && x < blurredWindow->maxXExclusive()
                        ? (*blurredRow)[static_cast<std::size_t>(x - blurredWindow->originX())]
                        : render::Rgba32f::transparent();
                const auto pixel =
                    render::unsharpImagePixel((*source.value())[column], tap, amount, threshold);
                if (!pixel)
                    return EvaluationDiagnostic{EvaluationDiagnosticCode::InvalidPixel,
                                                DiagnosticSeverity::Error,
                                                {},
                                                "Sharpen produced a non-finite value",
                                                {}};
                (*target.value())[column] = *pixel.value();
            }
            return std::nullopt;
        });
    if (outcome.cancelled || cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    if (outcome.incomplete || outcome.failure) {
        ImageEffectResult result;
        result.diagnostic = outcome.failure ? *outcome.failure
                                            : EvaluationDiagnostic{
                                                  EvaluationDiagnosticCode::AllocationFailure,
                                                  DiagnosticSeverity::Error,
                                                  {},
                                                  "A Sharpen row band could not complete",
                                                  {}};
        return result;
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, total, total});
    auto frozen = std::move(image).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Sharpen publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

} // namespace bloom::runtime::detail
