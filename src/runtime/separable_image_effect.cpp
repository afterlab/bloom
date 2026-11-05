#include "separable_image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <span>

namespace bloom::runtime::detail {
namespace {
std::vector<float> weights(const SeparableBlurKind kind, const double radius) {
    const int support = static_cast<int>(std::ceil(radius));
    std::vector<float> result(static_cast<std::size_t>(support * 2 + 1), 1.0F);
    if (support == 0)
        return result;
    // Box covers [-radius, radius] continuously: whole taps weigh 1 and the outermost taps of a
    // fractional radius weigh its fraction, so an animated radius grows smoothly between integers.
    const double boxEdge = radius - std::floor(radius);
    // #338 morphology: the Box taps unnormalized. Whole taps weigh exactly 1, so an integer
    // radius is a plain minimum/maximum over the square.
    if (kind == SeparableBlurKind::Erode || kind == SeparableBlurKind::Dilate) {
        if (boxEdge > 0.0)
            result.front() = result.back() = static_cast<float>(boxEdge);
        return result;
    }
    double total = 0.0;
    for (int i = -support; i <= support; ++i) {
        const double value = kind == SeparableBlurKind::Box
                                 ? (std::abs(i) == support && boxEdge > 0.0 ? boxEdge : 1.0)
                             : i == 0 ? 1.0
                                      : std::exp(-0.5 * std::pow(double(i) / (radius / 3.0), 2.0));
        result[static_cast<std::size_t>(i + support)] = static_cast<float>(value);
        total += static_cast<double>(static_cast<float>(value));
    }
    for (auto& value : result)
        value = static_cast<float>(static_cast<double>(value) / total);
    return result;
}
ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic = EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, text, text};
    return result;
}
} // namespace
std::optional<PreparedSeparableBlur> prepareSeparableBlur(const SeparableBlurKernel& kernel,
                                                          const std::optional<double> radius,
                                                          const double horizontalScale,
                                                          const double verticalScale) {
    if (!radius || !std::isfinite(*radius) || *radius < 0.0 ||
        *radius > document::kMaximumBlurRadiusPixels || !std::isfinite(horizontalScale) ||
        !std::isfinite(verticalScale) || horizontalScale <= 0.0 || verticalScale <= 0.0 ||
        *radius * horizontalScale > static_cast<double>(kMaximumSeparableBlurSupport) ||
        *radius * verticalScale > static_cast<double>(kMaximumSeparableBlurSupport) ||
        !validSeparableBlurKernelShape(kernel))
        return {};
    return PreparedSeparableBlur{weights(kernel.kind, *radius * horizontalScale),
                                 weights(kernel.kind, *radius * verticalScale), kernel.edge,
                                 kernel.channels, kernel.kind};
}
std::optional<render::ImageWindow> blurOutputWindow(const PreparedSeparableBlur& blur,
                                                    const render::ImageWindow input) {
    // Erode never grows coverage: outside its input every whole centre tap reads zero, so the
    // halo would hold only zeros and the window stays the input's.
    if (blur.edge == BlurEdgeMode::Clamp || blur.kind == SeparableBlurKind::Erode)
        return input;
    const auto x = static_cast<std::int64_t>(blur.horizontal.size() / 2);
    const auto y = static_cast<std::int64_t>(blur.vertical.size() / 2);
    if (input.originX() < std::numeric_limits<std::int64_t>::min() + x ||
        input.originY() < std::numeric_limits<std::int64_t>::min() + y)
        return {};
    auto window =
        render::ImageWindow::create(input.originX() - x, input.originY() - y,
                                    input.extent().width() + static_cast<std::uint64_t>(x * 2),
                                    input.extent().height() + static_cast<std::uint64_t>(y * 2));
    return window ? std::optional{*window.value()} : std::nullopt;
}
ImageEffectResult applySeparableBlur(const PreparedSeparableBlur& blur,
                                     const render::Rgba32fImage& input, const std::size_t budget,
                                     const CancellationToken& cancellation,
                                     const OperationIndex operation,
                                     const EvaluationProgressCallback& progress) {
    const auto window = blurOutputWindow(blur, input.descriptor()->dataWindow());
    if (!window)
        return failed(EvaluationDiagnosticCode::ArithmeticOverflow, "Blur halo overflows");
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window, input.descriptor()->displayWindow(), input.descriptor()->pixelAspect());
    if (!descriptor)
        return failed(EvaluationDiagnosticCode::ArithmeticOverflow, "Blur descriptor overflows");
    const auto bytes = descriptor.value()->layout().pixelStorageBytes;
    if (bytes > budget / 2)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Blur intermediate and output exceed the pixel budget");
    const bool coverage = blur.channels == BlurChannels::Coverage;
    const bool erode = blur.kind == SeparableBlurKind::Erode;
    const bool morphology = erode || blur.kind == SeparableBlurKind::Dilate;
    std::shared_ptr<const render::Rgba32fImage> intermediate;
    for (unsigned axis = 0; axis < 2; ++axis) {
        if (cancellation.isCancellationRequested())
            return ImageEffectResult{{}, {}, true};
        const auto& source = axis == 0 ? input : *intermediate;
        const auto sourceWindow = source.descriptor()->dataWindow();
        const auto& coefficients = axis == 0 ? blur.horizontal : blur.vertical;
        const auto support = static_cast<std::int64_t>(coefficients.size() / 2);
        auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), budget - bytes);
        if (!builder)
            return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                          "Blur allocation refused");
        for (auto y = window->originY(); y < window->maxYExclusive(); ++y) {
            if (cancellation.isCancellationRequested())
                return ImageEffectResult{{}, {}, true};
            auto row = builder.value()->row(y);
            for (auto x = window->originX(); x < window->maxXExclusive(); ++x) {
                if ((static_cast<std::uint64_t>(x - window->originX()) & 63U) == 0 &&
                    cancellation.isCancellationRequested())
                    return ImageEffectResult{{}, {}, true};
                std::array<float, 4> sum{};
                // #338: Erode starts from full coverage and takes the minimum, Dilate starts from
                // zero and takes the maximum. The device program performs the same operations.
                if (erode)
                    sum[3] = 1.0F;
                // #483: an Rgba Dilate carries the centre tap's colour (zero outside the input).
                std::array<float, 3> centre{};
                for (std::int64_t tap = -support; tap <= support; ++tap) {
                    const auto shifted = [](const std::int64_t value, const std::int64_t delta) {
                        if (delta < 0 && value < std::numeric_limits<std::int64_t>::min() - delta)
                            return std::numeric_limits<std::int64_t>::min();
                        if (delta > 0 && value > std::numeric_limits<std::int64_t>::max() - delta)
                            return std::numeric_limits<std::int64_t>::max();
                        return value + delta;
                    };
                    auto sx = shifted(x, axis == 0 ? tap : 0);
                    auto sy = shifted(y, axis == 1 ? tap : 0);
                    if (blur.edge == BlurEdgeMode::Clamp) {
                        sx = std::clamp(sx, sourceWindow.originX(),
                                        sourceWindow.maxXExclusive() - 1);
                        sy = std::clamp(sy, sourceWindow.originY(),
                                        sourceWindow.maxYExclusive() - 1);
                    }
                    const auto sample = source.read(sx, sy);
                    const auto weight = coefficients[static_cast<std::size_t>(tap + support)];
                    if (erode) {
                        // A tap outside the source is transparent: zero coverage. The fraction
                        // of an outer tap blends its coverage toward the neutral 1.
                        const float a = sample ? sample.value()->alpha() : 0.0F;
                        sum[3] = std::min(sum[3], std::fma(weight, a, 1.0F - weight));
                        continue;
                    }
                    if (!sample)
                        continue;
                    const auto p = *sample.value();
                    if (morphology) {
                        sum[3] = std::max(sum[3], weight * p.alpha());
                        if (tap == 0)
                            centre = {p.red(), p.green(), p.blue()};
                        continue;
                    }
                    if (coverage) {
                        sum[3] = std::fma(p.alpha(), weight, sum[3]);
                        continue;
                    }
                    const std::array channels{p.red(), p.green(), p.blue(), p.alpha()};
                    for (std::size_t channel = 0; channel < 4; ++channel)
                        sum[channel] = std::fma(channels[channel], weight, sum[channel]);
                }
                // Coverage writes the filtered alpha to every channel (premultiplied white).
                const auto alpha = std::clamp(sum[3], 0.0F, 1.0F);
                auto pixel =
                    coverage ? render::Rgba32f::fromPremultiplied(alpha, alpha, alpha, alpha)
                    : morphology
                        ? render::Rgba32f::fromPremultiplied(centre[0], centre[1], centre[2], alpha)
                        : render::Rgba32f::fromPremultiplied(sum[0], sum[1], sum[2], alpha);
                if (!pixel)
                    return failed(EvaluationDiagnosticCode::InvalidPixel,
                                  "Blur produced nonfinite HDR pixels");
                (*row.value())[static_cast<std::size_t>(x - window->originX())] = *pixel.value();
            }
            reportProgress(progress, {EvaluationProgressStage::Operation, operation,
                                      axis * window->extent().height() +
                                          static_cast<std::uint64_t>(y - window->originY()) + 1,
                                      2 * window->extent().height()});
        }
        if (cancellation.isCancellationRequested())
            return ImageEffectResult{{}, {}, true};
        auto frozen = std::move(*builder.value()).freeze();
        if (!frozen)
            return failed(EvaluationDiagnosticCode::InvalidPixel, "Blur image publication failed");
        intermediate = std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value()));
    }
    return {std::move(intermediate), {}, false};
}
render::OcioGpuProgramResult buildSeparableBlurProgram(const std::span<const float> coefficients,
                                                       const bool vertical, const BlurEdgeMode edge,
                                                       const BlurChannels channels,
                                                       const SeparableBlurKind kind) {
    const bool coverage = channels == BlurChannels::Coverage;
    const bool erode = kind == SeparableBlurKind::Erode;
    const bool dilate = kind == SeparableBlurKind::Dilate;
    const bool carry = dilate && !coverage;
    if (coefficients.empty() || coefficients.size() > 2 * kMaximumSeparableBlurSupport + 1 ||
        coefficients.size() % 2 != 1 ||
        !validSeparableBlurKernelShape(SeparableBlurKernel{kind, {}, edge, channels}))
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    std::string shader = "const float bloom_blur_weights[] = float[](";
    for (std::size_t i = 0; i < coefficients.size(); ++i) {
        if (!std::isfinite(coefficients[i]) || coefficients[i] < 0.0F)
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::InvalidRequest);
        char literal[64];
        const auto text = std::to_chars(literal, literal + sizeof(literal), coefficients[i],
                                        std::chars_format::scientific, 9);
        if (i)
            shader += ',';
        shader.append(literal, text.ptr);
    }
    const auto support = std::to_string(coefficients.size() / 2);
    if (erode || dilate) {
        // #338: the CPU pass operation for operation. Erode reads a tap outside the input as zero
        // coverage (Transparent) and folds fma(w, a, 1 - w) by minimum from 1; Dilate skips it and
        // folds w * a by maximum from 0.
        shader += ");\nvec4 bloom_separable_blur(ivec2 c) {\n"
                  " ivec2 size = imageSize(bloom_ocio_input);\n precise float m = " +
                  std::string(erode ? "1.0" : "0.0") + ";\n" +
                  std::string(carry ? " vec3 centre = vec3(0.0);\n" : "") + " for (int i = -" + support +
                  "; i <= " + support + "; ++i) {\n  ivec2 p = c + ivec2(" +
                  (vertical ? std::string("0, i") : std::string("i, 0")) +
                  ");\n  float w = bloom_blur_weights[i + " + support + "];\n";
        if (edge == BlurEdgeMode::Clamp)
            shader += "  p = clamp(p, ivec2(0), size - ivec2(1));\n";
        if (erode) {
            shader += edge == BlurEdgeMode::Clamp
                          ? "  float a = imageLoad(bloom_ocio_input, p).a;\n"
                          : "  float a = (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, "
                            "size))) ? 0.0 : imageLoad(bloom_ocio_input, p).a;\n";
            shader += "  precise float t = fma(w, a, 1.0 - w);\n  m = min(m, t);\n";
        } else {
            if (edge != BlurEdgeMode::Clamp)
                shader += "  if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, size))) "
                          "continue;\n";
            // #483: an Rgba Dilate carries the centre tap's colour (zero outside the input).
            shader += carry ? "  vec4 s = imageLoad(bloom_ocio_input, p);\n"
                              "  precise float t = w * s.a;\n"
                              "  m = max(m, t);\n"
                              "  if (i == 0) { centre = s.rgb; }\n"
                            : "  precise float t = w * imageLoad(bloom_ocio_input, p).a;\n"
                              "  m = max(m, t);\n";
        }
        shader += carry ? " }\n return vec4(centre, clamp(m, 0.0, 1.0));\n}\n"
                        : " }\n return vec4(clamp(m, 0.0, 1.0));\n}\n";
    } else {
        shader += ");\nvec4 bloom_separable_blur(ivec2 c) {\n"
                  " ivec2 size = imageSize(bloom_ocio_input);\n precise vec4 sum = vec4(0.0);\n"
                  " for (int i = -" +
                  support + "; i <= " + support +
                  "; ++i) {\n"
                  "  ivec2 p = c + ivec2(" +
                  (vertical ? std::string("0, i") : std::string("i, 0")) + ");\n";
        if (edge == BlurEdgeMode::Clamp)
            shader += "  p = clamp(p, ivec2(0), size - ivec2(1));\n";
        else
            shader +=
                "  if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, size))) continue;\n";
        if (coverage)
            shader += "  sum.a = fma(imageLoad(bloom_ocio_input, p).a, bloom_blur_weights[i + " +
                      support +
                      "], sum.a);\n"
                      " }\n return vec4(clamp(sum.a, 0.0, 1.0));\n}\n";
        else
            shader += "  sum = fma(imageLoad(bloom_ocio_input, p), vec4(bloom_blur_weights[i + " +
                      support +
                      "]), sum);\n"
                      " }\n sum.a = clamp(sum.a, 0.0, 1.0);\n return sum;\n}\n";
    }
    render::OcioGpuProgramDesc program;
    program.stage = render::OcioGpuProgramStage::ProcessNeighborhood;
    program.functionName = "bloom_separable_blur";
    program.resourcePrefix = "bloom_blur_";
    program.shaderText = std::move(shader);
    program.semanticsId = erode      ? "bloom.render.separable-erode.coverage.v1"
                          : dilate   ? (coverage ? "bloom.render.separable-dilate.coverage.v1"
                                                 : "bloom.render.separable-dilate.carry.v1")
                          : coverage ? "bloom.render.separable-blur.coverage.v1"
                                     : "bloom.render.separable-blur.v1";
    program.shaderTextDigest = render::computeOcioGpuShaderTextDigest(program.shaderText);
    program.resourceDigest = render::computeOcioGpuResourceDigest(program);
    program.contentIdentity = render::computeOcioGpuContentIdentity(
        {program.semanticsId, {}, {}, program.shaderTextDigest, program.resourceDigest});
    return render::OcioGpuProgramResult::success(std::move(program));
}
} // namespace bloom::runtime::detail
