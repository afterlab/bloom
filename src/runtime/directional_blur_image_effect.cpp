#include "directional_blur_image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"
#include "operation_key.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

namespace bloom::runtime::detail {
namespace {

// Offsets snap to this grid, so an angle whose sine or cosine is only a rounding error away from
// zero (90 degrees, say) does not leave a 1e-17 weight one texel over and grow the window for it.
constexpr double kOffsetGrid = 1048576.0; // 2^20
// Every tap offset must fit an int32 with room for the +1 bilinear texel.
constexpr double kMaximumOffset = 1073741824.0; // 2^30

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic = EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, text, text};
    return result;
}

} // namespace

std::optional<PreparedDirectionalBlur>
prepareDirectionalBlur(const DirectionalBlurKernel& kernel, const std::optional<double> angle,
                       const std::optional<double> spacing, const double horizontalScale,
                       const double verticalScale) {
    if (!angle || !spacing || !std::isfinite(*angle) || !std::isfinite(*spacing) ||
        *spacing < 0.0 || *spacing > kMaximumDirectionalBlurSpacing ||
        !std::isfinite(kernel.decay) || !(kernel.decay > 0.0) || kernel.decay > 1.0 ||
        !std::isfinite(horizontalScale) || !std::isfinite(verticalScale) ||
        !(horizontalScale > 0.0) || !(verticalScale > 0.0))
        return std::nullopt;
    PreparedDirectionalBlur prepared;
    const double radians = std::fmod(*angle, 360.0) * (std::numbers::pi / 180.0);
    const double stepX = *spacing * horizontalScale * std::cos(radians);
    const double stepY = *spacing * verticalScale * std::sin(radians);
    const auto snapped = [](const double value) { return std::round(value * kOffsetGrid) / kOffsetGrid; };
    double total = 0.0;
    std::array<double, kDirectionalBlurRayTaps> ray{};
    for (std::size_t i = 0; i < ray.size(); ++i) {
        ray[i] = std::pow(kernel.decay, static_cast<double>(i));
        total += ray[i];
    }
    bool identity = true;
    for (std::size_t i = 0; i < kDirectionalBlurRayTaps; ++i) {
        const double ox = snapped(-static_cast<double>(i) * stepX);
        const double oy = snapped(-static_cast<double>(i) * stepY);
        if (std::abs(ox) > kMaximumOffset || std::abs(oy) > kMaximumOffset)
            return std::nullopt;
        const double fx = std::floor(ox);
        const double fy = std::floor(oy);
        const double tx = ox - fx;
        const double ty = oy - fy;
        identity = identity && ox == 0.0 && oy == 0.0;
        const double weight = ray[i] / total;
        const std::array<std::array<double, 3>, 4> texels{{{fx, fy, (1.0 - tx) * (1.0 - ty)},
                                                           {fx + 1.0, fy, tx * (1.0 - ty)},
                                                           {fx, fy + 1.0, (1.0 - tx) * ty},
                                                           {fx + 1.0, fy + 1.0, tx * ty}}};
        for (std::size_t j = 0; j < texels.size(); ++j) {
            const auto k = 4 * i + j;
            prepared.dx[k] = static_cast<std::int32_t>(texels[j][0]);
            prepared.dy[k] = static_cast<std::int32_t>(texels[j][1]);
            prepared.weights[k] = static_cast<float>(weight * texels[j][2]);
        }
    }
    // Every tap at the pixel itself: the normalized weights sum to one, so the pass is the input.
    prepared.identity = identity;
    return prepared;
}

std::optional<render::ImageWindow>
directionalBlurOutputWindow(const PreparedDirectionalBlur& blur, const render::ImageWindow input) {
    std::int64_t lowX = 0;
    std::int64_t highX = 0;
    std::int64_t lowY = 0;
    std::int64_t highY = 0;
    for (std::size_t k = 0; k < kDirectionalBlurTexels; ++k) {
        if (blur.weights[k] == 0.0F)
            continue;
        // Output c reads input(c + t): c reaches the input when c is in input - t.
        lowX = std::min<std::int64_t>(lowX, -blur.dx[k]);
        highX = std::max<std::int64_t>(highX, -blur.dx[k]);
        lowY = std::min<std::int64_t>(lowY, -blur.dy[k]);
        highY = std::max<std::int64_t>(highY, -blur.dy[k]);
    }
    constexpr auto limit = std::int64_t{1} << 52;
    if (input.originX() < -limit || input.originY() < -limit || input.maxXExclusive() > limit ||
        input.maxYExclusive() > limit)
        return std::nullopt;
    const auto window = render::ImageWindow::create(
        input.originX() + lowX, input.originY() + lowY,
        input.extent().width() + static_cast<std::uint64_t>(highX - lowX),
        input.extent().height() + static_cast<std::uint64_t>(highY - lowY));
    return window ? std::optional{*window.value()} : std::nullopt;
}

std::string directionalBlurIdentity(const PreparedDirectionalBlur& blur) {
    OperationKey key;
    key.add(std::string("directional-blur-v1"));
    for (std::size_t k = 0; k < kDirectionalBlurTexels; ++k) {
        key.add(static_cast<std::int64_t>(blur.dx[k]));
        key.add(static_cast<std::int64_t>(blur.dy[k]));
        key.add(static_cast<double>(blur.weights[k]));
    }
    return key.bytes();
}

ImageEffectResult applyDirectionalBlur(const PreparedDirectionalBlur& blur,
                                       const render::Rgba32fImage& input, const std::size_t budget,
                                       const CancellationToken& cancellation,
                                       const OperationIndex operation,
                                       const EvaluationProgressCallback& progress) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    const auto sourceWindow = input.descriptor()->dataWindow();
    const auto window = directionalBlurOutputWindow(blur, sourceWindow);
    if (!window)
        return failed(EvaluationDiagnosticCode::ArithmeticOverflow,
                      "Directional blur reach overflows");
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *window, input.descriptor()->displayWindow(), input.descriptor()->pixelAspect());
    if (!descriptor)
        return failed(EvaluationDiagnosticCode::ArithmeticOverflow,
                      "Directional blur descriptor overflows");
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Directional blur output exceeds the pixel budget");
    const auto rows = window->extent().height();
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, rows});
    for (auto y = window->originY(); y < window->maxYExclusive(); ++y) {
        if (cancellation.isCancellationRequested())
            return ImageEffectResult{{}, {}, true};
        auto row = builder.value()->row(y);
        if (!row)
            return failed(EvaluationDiagnosticCode::InternalInvariant,
                          "Directional blur row is unavailable");
        for (auto x = window->originX(); x < window->maxXExclusive(); ++x) {
            if ((static_cast<std::uint64_t>(x - window->originX()) & 63U) == 0 &&
                cancellation.isCancellationRequested())
                return ImageEffectResult{{}, {}, true};
            std::array<float, 4> sum{};
            for (std::size_t k = 0; k < kDirectionalBlurTexels; ++k) {
                const auto sx = x + blur.dx[k];
                const auto sy = y + blur.dy[k];
                if (sx < sourceWindow.originX() || sy < sourceWindow.originY() ||
                    sx >= sourceWindow.maxXExclusive() || sy >= sourceWindow.maxYExclusive())
                    continue;
                const auto sample = input.read(sx, sy);
                if (!sample)
                    continue;
                const auto p = *sample.value();
                const std::array channels{p.red(), p.green(), p.blue(), p.alpha()};
                for (std::size_t channel = 0; channel < 4; ++channel)
                    sum[channel] = std::fma(channels[channel], blur.weights[k], sum[channel]);
            }
            const auto alpha = std::clamp(sum[3], 0.0F, 1.0F);
            const auto pixel = render::Rgba32f::fromPremultiplied(sum[0], sum[1], sum[2], alpha);
            if (!pixel)
                return failed(EvaluationDiagnosticCode::InvalidPixel,
                              "Directional blur produced nonfinite HDR pixels");
            (*row.value())[static_cast<std::size_t>(x - window->originX())] = *pixel.value();
        }
        reportProgress(progress, {EvaluationProgressStage::Operation, operation,
                                  static_cast<std::uint64_t>(y - window->originY() + 1), rows});
    }
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel,
                      "Directional blur image publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

std::vector<float> directionalBlurProgramData(const PreparedDirectionalBlur& blur) {
    std::vector<float> data;
    data.reserve(4 * kDirectionalBlurTexels);
    for (std::size_t k = 0; k < kDirectionalBlurTexels; ++k) {
        data.push_back(static_cast<float>(blur.dx[k]));
        data.push_back(static_cast<float>(blur.dy[k]));
        data.push_back(blur.weights[k]);
        data.push_back(0.0F);
    }
    return data;
}

render::OcioGpuProgramResult buildDirectionalBlurProgram(const std::vector<float>& data) {
    // The offsets travel as floats: every one is an integer far inside binary32's exact range.
    if (data.size() != 4 * kDirectionalBlurTexels)
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    for (std::size_t k = 0; k < kDirectionalBlurTexels; ++k) {
        const float dx = data[4 * k];
        const float dy = data[4 * k + 1];
        const float weight = data[4 * k + 2];
        if (!std::isfinite(dx) || !std::isfinite(dy) || std::trunc(dx) != dx ||
            std::trunc(dy) != dy || std::abs(dx) > 1.0e9F || std::abs(dy) > 1.0e9F ||
            !std::isfinite(weight) || weight < 0.0F || data[4 * k + 3] != 0.0F)
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::InvalidRequest);
    }
    static const std::string text =
        "layout(set = 0, binding = 0, std430) readonly buffer BloomDirectionalBlurData { vec4 "
        "values[]; } bloom_directional_blur_data;\n"
        "vec4 bloom_directional_blur(ivec2 c) {\n"
        " ivec2 size = imageSize(bloom_ocio_input);\n"
        " precise vec4 sum = vec4(0.0);\n"
        " for (int k = 0; k < 16; ++k) {\n"
        "  vec4 t = bloom_directional_blur_data.values[k];\n"
        "  ivec2 p = c + ivec2(t.xy);\n"
        "  if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, size))) continue;\n"
        "  sum = fma(imageLoad(bloom_ocio_input, p), vec4(t.z), sum);\n"
        " }\n"
        " sum.a = clamp(sum.a, 0.0, 1.0);\n"
        " return sum;\n"
        "}\n";
    render::OcioGpuProgramDesc program;
    program.stage = render::OcioGpuProgramStage::ProcessNeighborhood;
    program.functionName = "bloom_directional_blur";
    program.resourcePrefix = "bloom_directional_blur_";
    program.shaderText = text;
    program.semanticsId = "bloom.render.directional-blur.v1";
    program.uniformBufferStorage = true;
    program.uniformBufferSize = data.size() * sizeof(float);
    program.uniformBufferData.resize(data.size() * sizeof(float));
    std::memcpy(program.uniformBufferData.data(), data.data(), data.size() * sizeof(float));
    program.shaderTextDigest = render::computeOcioGpuShaderTextDigest(program.shaderText);
    program.resourceDigest = render::computeOcioGpuResourceDigest(program);
    program.contentIdentity = render::computeOcioGpuContentIdentity(
        {program.semanticsId, {}, {}, program.shaderTextDigest, program.resourceDigest});
    return render::OcioGpuProgramResult::success(std::move(program));
}

} // namespace bloom::runtime::detail
