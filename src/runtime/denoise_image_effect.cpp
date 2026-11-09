#include "denoise_image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"
#include "operation_key.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bloom::runtime::detail {
namespace {

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic = EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, text, text};
    return result;
}

// The compact spatial weight at distance `i` for radius `r`: (1 - (i / (r + 1))^2)^2.
float spatialWeight(const int i, const int r) {
    const double t = static_cast<double>(i) / static_cast<double>(r + 1);
    const double base = 1.0 - t * t;
    return static_cast<float>(base * base);
}

int scaledRadius(const double radius, const double scale) {
    return std::clamp(static_cast<int>(std::lround(radius * scale)), 0, kMaximumDenoiseRadius);
}

} // namespace

std::optional<PreparedDenoise> prepareDenoise(const DenoiseKernel& kernel,
                                              const std::optional<double> radius,
                                              const std::optional<double> strength,
                                              const double horizontalScale,
                                              const double verticalScale) {
    if (!radius || !strength || !std::isfinite(*radius) || !std::isfinite(*strength) ||
        *radius < 0.0 || *radius > kMaximumDenoiseAuthoredRadius || *strength < 0.0 ||
        *strength > kMaximumDenoiseStrength || !std::isfinite(horizontalScale) ||
        !std::isfinite(verticalScale) || !(horizontalScale > 0.0) || !(verticalScale > 0.0))
        return std::nullopt;
    PreparedDenoise prepared;
    prepared.mode = kernel.mode;
    prepared.radiusX = scaledRadius(*radius, horizontalScale);
    prepared.radiusY = scaledRadius(*radius, verticalScale);
    if (prepared.identity())
        return prepared;
    if (kernel.mode == DenoiseMode::Median) {
        prepared.threshold = static_cast<float>(*strength);
    } else {
        // A zero strength still weighs identical neighborhoods, so the variance has a floor.
        const double sigma = std::max(*strength, 1.0e-6);
        prepared.inverseVariance = static_cast<float>(1.0 / (sigma * sigma));
    }
    for (int i = 0; i <= prepared.radiusX; ++i)
        prepared.weightsX[static_cast<std::size_t>(i)] = spatialWeight(i, prepared.radiusX);
    for (int i = 0; i <= prepared.radiusY; ++i)
        prepared.weightsY[static_cast<std::size_t>(i)] = spatialWeight(i, prepared.radiusY);
    return prepared;
}

std::string denoiseIdentity(const PreparedDenoise& denoise) {
    OperationKey key;
    key.add(std::string("denoise-v1"));
    key.add(static_cast<std::int64_t>(denoise.mode));
    key.add(static_cast<std::int64_t>(denoise.radiusX));
    key.add(static_cast<std::int64_t>(denoise.radiusY));
    key.add(static_cast<double>(denoise.threshold));
    key.add(static_cast<double>(denoise.inverseVariance));
    return key.bytes();
}

namespace {

using Texel = std::array<float, 4>;

struct Source final {
    const render::Rgba32fImage& image;
    render::ImageWindow window;
    [[nodiscard]] bool contains(const std::int64_t x, const std::int64_t y) const noexcept {
        return x >= window.originX() && y >= window.originY() && x < window.maxXExclusive() &&
               y < window.maxYExclusive();
    }
    [[nodiscard]] std::optional<Texel> read(const std::int64_t x, const std::int64_t y) const {
        if (!contains(x, y))
            return std::nullopt;
        const auto sample = image.read(x, y);
        if (!sample)
            return std::nullopt;
        const auto p = *sample.value();
        return Texel{p.red(), p.green(), p.blue(), p.alpha()};
    }
    [[nodiscard]] Texel clamped(const std::int64_t x, const std::int64_t y) const {
        const auto cx = std::clamp(x, window.originX(), window.maxXExclusive() - 1);
        const auto cy = std::clamp(y, window.originY(), window.maxYExclusive() - 1);
        return read(cx, cy).value_or(Texel{});
    }
};

float squaredDistance(const Texel& a, const Texel& b) {
    float sum = 0.0F;
    for (std::size_t c = 0; c < 4; ++c) {
        const float d = a[c] - b[c];
        sum = std::fma(d, d, sum);
    }
    return sum;
}

float rangeWeight(const float distance, const float inverseVariance) {
    const float t = std::max(std::fma(-distance, inverseVariance, 1.0F), 0.0F);
    return t * t;
}

// Dust & Scratches on associated pixels without moving color against coverage. Alpha is a channel
// of its own; color is taken as straight (unassociated) values from the neighbors that cover
// anything, since a premultiplied value says as much about how much the pixel covers as about its
// color. The straight color then takes the output alpha, so a soft edge keeps its color and a
// removed speck takes its coverage with it. A center that keeps both its color and its alpha is
// returned untouched, bit for bit. Where no coverage is left the pixel is transparent (an image
// stores no color where alpha is 0).
Texel median(const PreparedDenoise& d, const Source& source, const std::int64_t x,
             const std::int64_t y, const Texel& center) {
    std::array<std::array<float, 169>, 3> straights{};
    std::array<float, 169> alphas{};
    std::size_t covered = 0;
    std::size_t count = 0;
    for (int dy = -d.radiusY; dy <= d.radiusY; ++dy)
        for (int dx = -d.radiusX; dx <= d.radiusX; ++dx) {
            const auto texel = source.read(x + dx, y + dy);
            if (!texel)
                continue;
            alphas[count++] = (*texel)[3];
            if ((*texel)[3] > 0.0F) {
                for (std::size_t c = 0; c < 3; ++c)
                    straights[c][covered] = (*texel)[c] / (*texel)[3];
                ++covered;
            }
        }
    std::sort(alphas.begin(), alphas.begin() + static_cast<std::ptrdiff_t>(count));
    const float middleAlpha = alphas[count / 2];
    const float alpha = std::abs(center[3] - middleAlpha) > d.threshold ? middleAlpha : center[3];
    Texel out{0.0F, 0.0F, 0.0F, alpha};
    if (!(alpha > 0.0F))
        return out;
    const bool centerCovered = center[3] > 0.0F;
    for (std::size_t c = 0; c < 3; ++c) {
        std::sort(straights[c].begin(), straights[c].begin() + static_cast<std::ptrdiff_t>(covered));
        const float middle = straights[c][covered / 2];
        const float own = centerCovered ? center[c] / center[3] : middle;
        const bool kept = centerCovered && !(std::abs(own - middle) > d.threshold);
        // Kept color and alpha: the stored premultiplied value, exactly.
        out[c] = kept ? center[c] * (alpha / center[3]) : middle * alpha;
    }
    return out;
}

Texel weighted(const PreparedDenoise& d, const Source& source, const std::int64_t x,
               const std::int64_t y, const Texel& center) {
    Texel sum{};
    float total = 0.0F;
    const bool patches = d.mode == DenoiseMode::Spatial;
    for (int dy = -d.radiusY; dy <= d.radiusY; ++dy)
        for (int dx = -d.radiusX; dx <= d.radiusX; ++dx) {
            const auto texel = source.read(x + dx, y + dy);
            if (!texel)
                continue;
            float distance = 0.0F;
            if (patches) {
                float patch = 0.0F;
                for (int py = -1; py <= 1; ++py)
                    for (int px = -1; px <= 1; ++px)
                        patch += squaredDistance(source.clamped(x + px, y + py),
                                                 source.clamped(x + dx + px, y + dy + py));
                distance = patch * (1.0F / 36.0F);
            } else {
                distance = squaredDistance(center, *texel);
            }
            const float w =
                (d.weightsX[static_cast<std::size_t>(std::abs(dx))] *
                 d.weightsY[static_cast<std::size_t>(std::abs(dy))]) *
                rangeWeight(distance, d.inverseVariance);
            for (std::size_t c = 0; c < 4; ++c)
                sum[c] = std::fma((*texel)[c] - center[c], w, sum[c]);
            total += w;
        }
    // The mean is taken about the center pixel, so equal neighbors give the pixel back exactly.
    for (std::size_t c = 0; c < 4; ++c)
        sum[c] = center[c] + sum[c] / total;
    return sum;
}

} // namespace

ImageEffectResult applyDenoise(const PreparedDenoise& denoise, const render::Rgba32fImage& input,
                               const std::size_t budget, const CancellationToken& cancellation,
                               const OperationIndex operation,
                               const EvaluationProgressCallback& progress) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    const auto window = input.descriptor()->dataWindow();
    auto builder = render::Rgba32fImageBuilder::create(*input.descriptor(), budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Denoise output exceeds the pixel budget");
    const Source source{input, window};
    const auto rows = window.extent().height();
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, rows});
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
        if (cancellation.isCancellationRequested())
            return ImageEffectResult{{}, {}, true};
        auto row = builder.value()->row(y);
        if (!row)
            return failed(EvaluationDiagnosticCode::InternalInvariant,
                          "Denoise row is unavailable");
        for (auto x = window.originX(); x < window.maxXExclusive(); ++x) {
            if ((static_cast<std::uint64_t>(x - window.originX()) & 15U) == 0 &&
                cancellation.isCancellationRequested())
                return ImageEffectResult{{}, {}, true};
            const auto center = source.read(x, y).value_or(Texel{});
            auto out = denoise.mode == DenoiseMode::Median ? median(denoise, source, x, y, center)
                                                           : weighted(denoise, source, x, y, center);
            out[3] = std::clamp(out[3], 0.0F, 1.0F);
            const auto pixel = render::Rgba32f::fromPremultiplied(out[0], out[1], out[2], out[3]);
            if (!pixel)
                return failed(EvaluationDiagnosticCode::InvalidPixel,
                              "Denoise produced nonfinite HDR pixels");
            (*row.value())[static_cast<std::size_t>(x - window.originX())] = *pixel.value();
        }
        reportProgress(progress, {EvaluationProgressStage::Operation, operation,
                                  static_cast<std::uint64_t>(y - window.originY() + 1), rows});
    }
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Denoise image publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

std::vector<float> denoiseProgramData(const PreparedDenoise& d) {
    std::vector<float> data(24, 0.0F);
    data[0] = static_cast<float>(static_cast<int>(d.mode));
    data[1] = static_cast<float>(d.radiusX);
    data[2] = static_cast<float>(d.radiusY);
    data[4] = d.threshold;
    data[5] = d.inverseVariance;
    for (std::size_t i = 0; i < 8; ++i) {
        data[8 + i] = d.weightsX[i];
        data[16 + i] = d.weightsY[i];
    }
    return data;
}

render::OcioGpuProgramResult buildDenoiseProgram(const std::vector<float>& data) {
    if (data.size() != 24)
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    for (const auto value : data)
        if (!std::isfinite(value))
            return render::OcioGpuProgramResult::failure(
                render::OcioGpuProgramError::InvalidRequest);
    const float mode = data[0];
    if ((mode != 0.0F && mode != 1.0F && mode != 2.0F) || data[1] < 0.0F ||
        data[1] > static_cast<float>(kMaximumDenoiseRadius) || data[2] < 0.0F ||
        data[2] > static_cast<float>(kMaximumDenoiseRadius))
        return render::OcioGpuProgramResult::failure(render::OcioGpuProgramError::InvalidRequest);
    static const std::string text =
        "layout(set = 0, binding = 0, std430) readonly buffer BloomDenoiseData { vec4 values[]; } "
        "bloom_denoise_data;\n"
        "precise float bloom_ocio_cpu_div(float a, float b);\n"
        "float bloom_denoise_weight(float spread, float inverse_variance) {\n"
        " float t = max(fma(-spread, inverse_variance, 1.0), 0.0);\n"
        " return t * t;\n"
        "}\n"
        "float bloom_denoise_distance(vec4 a, vec4 b) {\n"
        " precise float sum = 0.0;\n"
        " for (int c = 0; c < 4; ++c) { float d = a[c] - b[c]; sum = fma(d, d, sum); }\n"
        " return sum;\n"
        "}\n"
        "float bloom_denoise_axis(int table, int i) {\n"
        " return bloom_denoise_data.values[table + (i >> 2)][i & 3];\n"
        "}\n"
        "vec4 bloom_denoise(ivec2 c) {\n"
        " ivec2 size = imageSize(bloom_ocio_input);\n"
        " vec4 head = bloom_denoise_data.values[0];\n"
        " vec4 knob = bloom_denoise_data.values[1];\n"
        " int mode = int(head.x);\n"
        " int rx = int(head.y);\n"
        " int ry = int(head.z);\n"
        " vec4 center = imageLoad(bloom_ocio_input, c);\n"
        " vec4 result = center;\n"
        " if (mode == 0) {\n"
        "  float s[3][169];\n"
        "  float al[169];\n"
        "  int count = 0;\n"
        "  int covered = 0;\n"
        "  for (int dy = -ry; dy <= ry; ++dy) {\n"
        "   for (int dx = -rx; dx <= rx; ++dx) {\n"
        "    ivec2 p = c + ivec2(dx, dy);\n"
        "    if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, size))) continue;\n"
        "    vec4 t = imageLoad(bloom_ocio_input, p);\n"
        "    int ja = count;\n"
        "    while (ja > 0 && al[ja - 1] > t.a) { al[ja] = al[ja - 1]; --ja; }\n"
        "    al[ja] = t.a;\n"
        "    if (t.a > 0.0) {\n"
        "     for (int ch = 0; ch < 3; ++ch) {\n"
        "      float x = bloom_ocio_cpu_div(t[ch], t.a);\n"
        "      int j = covered;\n"
        "      while (j > 0 && s[ch][j - 1] > x) { s[ch][j] = s[ch][j - 1]; --j; }\n"
        "      s[ch][j] = x;\n"
        "     }\n"
        "     ++covered;\n"
        "    }\n"
        "    ++count;\n"
        "   }\n"
        "  }\n"
        "  float middle_alpha = al[count / 2];\n"
        "  float alpha = abs(center.a - middle_alpha) > knob.x ? middle_alpha : center.a;\n"
        "  result = vec4(0.0, 0.0, 0.0, alpha);\n"
        "  if (alpha > 0.0) {\n"
        "   bool covered_center = center.a > 0.0;\n"
        "   for (int ch = 0; ch < 3; ++ch) {\n"
        "    float middle = s[ch][covered / 2];\n"
        "    float own = covered_center ? bloom_ocio_cpu_div(center[ch], center.a) : middle;\n"
        "    bool kept = covered_center && !(abs(own - middle) > knob.x);\n"
        "    result[ch] = kept ? center[ch] * bloom_ocio_cpu_div(alpha, center.a) : middle * alpha;\n"
        "   }\n"
        "  }\n"
        " } else {\n"
        "  precise vec4 sum = vec4(0.0);\n"
        "  precise float total = 0.0;\n"
        "  for (int dy = -ry; dy <= ry; ++dy) {\n"
        "   for (int dx = -rx; dx <= rx; ++dx) {\n"
        "    ivec2 p = c + ivec2(dx, dy);\n"
        "    if (any(lessThan(p, ivec2(0))) || any(greaterThanEqual(p, size))) continue;\n"
        "    vec4 t = imageLoad(bloom_ocio_input, p);\n"
        "    float spread;\n"
        "    if (mode == 2) {\n"
        "     precise float tile = 0.0;\n"
        "     for (int py = -1; py <= 1; ++py) {\n"
        "      for (int px = -1; px <= 1; ++px) {\n"
        "       ivec2 a = clamp(c + ivec2(px, py), ivec2(0), size - 1);\n"
        "       ivec2 b = clamp(p + ivec2(px, py), ivec2(0), size - 1);\n"
        "       tile += bloom_denoise_distance(imageLoad(bloom_ocio_input, a), "
        "imageLoad(bloom_ocio_input, b));\n"
        "      }\n"
        "     }\n"
        "     spread = tile * (1.0 / 36.0);\n"
        "    } else {\n"
        "     spread = bloom_denoise_distance(center, t);\n"
        "    }\n"
        "    float w = (bloom_denoise_axis(2, abs(dx)) * bloom_denoise_axis(4, abs(dy))) * "
        "bloom_denoise_weight(spread, knob.y);\n"
        "    sum = fma(t - center, vec4(w), sum);\n"
        "    total += w;\n"
        "   }\n"
        "  }\n"
        "  for (int ch = 0; ch < 4; ++ch) {\n"
        "   precise float mean = bloom_ocio_cpu_div(sum[ch], total);\n"
        "   precise float joined = center[ch] + mean;\n"
        "   result[ch] = joined;\n"
        "  }\n"
        " }\n"
        " result.a = clamp(result.a, 0.0, 1.0);\n"
        " return result;\n"
        "}\n";
    render::OcioGpuProgramDesc program;
    program.stage = render::OcioGpuProgramStage::ProcessNeighborhood;
    program.functionName = "bloom_denoise";
    program.resourcePrefix = "bloom_denoise_";
    program.shaderText = text;
    program.semanticsId = "bloom.render.denoise.v1";
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
