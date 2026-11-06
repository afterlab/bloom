// #486 Film Grain. The table holds each channel's amplitude, the size, the response and the stream;
// the geometry appended where it is applied. docs/architecture/image-filters.md, "Film Grain", is
// the specification. The pixel uses additions, multiplications, min, max and one correctly rounded
// division over the integer-hashed noise (core::film_grain).

#include "film_grain_image_effect.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <bloom/core/film_grain_noise.hpp>
#include <bloom/document/film_grain_nodes.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace bloom::runtime::detail {
namespace {

// The table:
//  0  red, green and blue amplitude (intensity * amount * the noise's normalisation), size
//  4  shadows, midtones, highlights, (unused)
//  8  Rec. 709 luma weights, 1 / mid grey
// 12  1 / (white - mid grey), the stream's low and high 16 bits, mid grey
// 16  pixel origin x and y, scale x and y (filmGrainProgramData)
constexpr std::size_t kSize = 3;
constexpr std::size_t kShadows = 4;
constexpr std::size_t kMidtones = 5;
constexpr std::size_t kHighlights = 6;
constexpr std::size_t kStreamLow = 13;
constexpr std::size_t kStreamHigh = 14;
constexpr std::size_t kMidGrey = 15;
constexpr std::size_t kOriginX = 16;
constexpr std::size_t kOriginY = 17;
constexpr std::size_t kScaleX = 18;
constexpr std::size_t kScaleY = 19;

// Rec. 709 luma, the weights of the track-matte Luma, Luma Key and the Glow roll-off.
constexpr float kLumaRed = 0.2126F;
constexpr float kLumaGreen = 0.7152F;
constexpr float kLumaBlue = 0.0722F;

// A cell coordinate must fit an int on both devices.
constexpr double kCoordinateLimit = 1073741824.0; // 2^30

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic =
        EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, std::string(text), {}};
    return result;
}

[[nodiscard]] std::uint32_t streamOf(const float* const k) noexcept {
    return static_cast<std::uint32_t>(k[kStreamLow]) |
           (static_cast<std::uint32_t>(k[kStreamHigh]) << 16U);
}

} // namespace

std::optional<PreparedColorGrade> prepareFilmGrainGrade(const FilmGrainKernel& kernel,
                                                        const std::span<const double> values) {
    if (values.size() != document::kFilmGrainScalarCount + 1 ||
        kernel.seed < 0 || kernel.seed > 2147483647 ||
        (kernel.animation != document::kFilmGrainEveryFrame &&
         kernel.animation != document::kFilmGrainHeld))
        return std::nullopt;
    std::size_t scalar = 0;
    for (const auto& spec : document::filmGrainParameters()) {
        if (spec.kind != document::FilmGrainParameterKind::Scalar)
            continue;
        const double value = values[scalar++];
        if (!std::isfinite(value) || value < spec.minimum || value > spec.maximum)
            return std::nullopt;
    }
    const double frame = values[document::kFilmGrainScalarCount];
    if (!std::isfinite(frame))
        return std::nullopt;
    const auto at = [&](const std::string_view role) {
        return values[document::filmGrainScalarIndex(role)];
    };
    PreparedColorGrade grade{ColorGradeKind::FilmGrain,
                             std::vector<float>(kFilmGrainTableFloats, 0.0F), false};
    auto& d = grade.data;
    const double intensity = at("intensity") * core::film_grain::kNormalise;
    d[0] = static_cast<float>(intensity * at("redAmount"));
    d[1] = static_cast<float>(intensity * at("greenAmount"));
    d[2] = static_cast<float>(intensity * at("blueAmount"));
    d[kSize] = static_cast<float>(at("size"));
    d[kShadows] = static_cast<float>(at("shadows"));
    d[kMidtones] = static_cast<float>(at("midtones"));
    d[kHighlights] = static_cast<float>(at("highlights"));
    d[8] = kLumaRed;
    d[9] = kLumaGreen;
    d[10] = kLumaBlue;
    d[11] = static_cast<float>(1.0 / document::kFilmGrainMidGrey);
    d[12] = static_cast<float>(1.0 / (document::kFilmGrainWhite - document::kFilmGrainMidGrey));
    // Each half of the stream is exact in a binary32.
    const auto stream = core::film_grain::stream(kernel.seed, static_cast<std::int64_t>(frame));
    d[kStreamLow] = static_cast<float>(stream & 0xFFFFU);
    d[kStreamHigh] = static_cast<float>(stream >> 16U);
    d[kMidGrey] = static_cast<float>(document::kFilmGrainMidGrey);
    // No channel or no response anywhere adds nothing, on any frame.
    grade.identity = (d[0] == 0.0F && d[1] == 0.0F && d[2] == 0.0F) ||
                     (d[kShadows] == 0.0F && d[kMidtones] == 0.0F && d[kHighlights] == 0.0F);
    return grade;
}

std::optional<std::vector<float>>
filmGrainProgramData(const PreparedColorGrade& grade, const render::ImageWindow& dataWindow,
                     const double pixelAspect, const double horizontalScale,
                     const double verticalScale) {
    if (grade.kind != ColorGradeKind::FilmGrain || grade.data.size() != kFilmGrainTableFloats ||
        !std::isfinite(pixelAspect) || !(pixelAspect > 0.0) || !std::isfinite(horizontalScale) ||
        !std::isfinite(verticalScale) || !(horizontalScale > 0.0) || !(verticalScale > 0.0))
        return std::nullopt;
    const auto size = static_cast<double>(grade.data[kSize]);
    const double scaleX = pixelAspect / (horizontalScale * size);
    const double scaleY = 1.0 / (verticalScale * size);
    const double originX = static_cast<double>(dataWindow.originX()) + 0.5;
    const double originY = static_cast<double>(dataWindow.originY()) + 0.5;
    const auto lastX = originX + static_cast<double>(dataWindow.extent().width());
    const auto lastY = originY + static_cast<double>(dataWindow.extent().height());
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || !(scaleX > 0.0) || !(scaleY > 0.0) ||
        scaleX > static_cast<double>(std::numeric_limits<float>::max()) ||
        scaleY > static_cast<double>(std::numeric_limits<float>::max()) ||
        std::abs(originX) * scaleX > kCoordinateLimit || std::abs(lastX) * scaleX > kCoordinateLimit ||
        std::abs(originY) * scaleY > kCoordinateLimit || std::abs(lastY) * scaleY > kCoordinateLimit)
        return std::nullopt;
    auto data = grade.data;
    data.insert(data.end(), {static_cast<float>(originX), static_cast<float>(originY),
                             static_cast<float>(scaleX), static_cast<float>(scaleY)});
    return data;
}

bool validFilmGrainProgramData(const std::vector<float>& d) noexcept {
    if (d.size() != kFilmGrainProgramFloats ||
        !std::ranges::all_of(d, [](const float value) { return std::isfinite(value); }))
        return false;
    const auto half = [](const float value) {
        return value >= 0.0F && value <= 65535.0F && value == std::floor(value);
    };
    return half(d[kStreamLow]) && half(d[kStreamHigh]) && d[kScaleX] > 0.0F && d[kScaleY] > 0.0F;
}

bool filmGrainPixel(const float* const k, const std::int64_t x, const std::int64_t y,
                    std::array<float, 4>& pixel) noexcept {
    const float a = pixel[3];
    if (a == 0.0F)
        return true;
    const float luma = k[8] * pixel[0] + k[9] * pixel[1] + k[10] * pixel[2];
    const float straight = luma / a;
    if (!std::isfinite(straight))
        return false;
    const float l = std::max(straight, 0.0F);
    float response = 0.0F;
    if (l >= 1.0F)
        response = k[kHighlights] * l;
    else if (l >= k[kMidGrey])
        response = k[kMidtones] + (k[kHighlights] - k[kMidtones]) * ((l - k[kMidGrey]) * k[12]);
    else
        response = k[kShadows] + (k[kMidtones] - k[kShadows]) * (l * k[11]);
    const float scale = a * response;
    if (scale == 0.0F)
        return true;
    const float px = (static_cast<float>(x) + k[kOriginX]) * k[kScaleX];
    const float py = (static_cast<float>(y) + k[kOriginY]) * k[kScaleY];
    const auto grain = core::film_grain::noise(px, py, streamOf(k));
    for (std::size_t i = 0; i < 3; ++i)
        pixel[i] = pixel[i] + scale * (k[i] * grain[i]);
    return std::isfinite(pixel[0]) && std::isfinite(pixel[1]) && std::isfinite(pixel[2]);
}

const std::string& filmGrainProgramText() {
    // The same binary32 operations as filmGrainPixel: precise additions and multiplications, the
    // wrapper's correctly rounded division. The wrapper makes alpha 0 transparent black and refuses
    // a non-finite result.
    static const std::string text =
        std::string(
            "layout(set = 0, binding = 0, std430) readonly buffer BloomFilmGrainData { vec4 "
            "values[]; } bloom_film_grain_data;\n"
            "precise float bloom_ocio_cpu_div(float a, float b);\n") +
        std::string(core::film_grain::kernelGlsl()) +
        "vec4 bloom_film_grain(ivec2 c) {\n"
        " vec4 p = imageLoad(bloom_ocio_input, c);\n"
        " float a = p.a;\n"
        " if (a == 0.0) { return p; }\n"
        " vec4 k0 = bloom_film_grain_data.values[0];\n"
        " vec4 k1 = bloom_film_grain_data.values[1];\n"
        " vec4 k2 = bloom_film_grain_data.values[2];\n"
        " vec4 k3 = bloom_film_grain_data.values[3];\n"
        " vec4 g = bloom_film_grain_data.values[4];\n"
        " precise float luma = k2.x * p.r + k2.y * p.g + k2.z * p.b;\n"
        " precise float straight = bloom_ocio_cpu_div(luma, a);\n"
        " float l = max(straight, 0.0);\n"
        " precise float response = 0.0;\n"
        " if (l >= 1.0) {\n"
        "  response = k1.z * l;\n"
        " } else if (l >= k3.w) {\n"
        "  response = k1.y + (k1.z - k1.y) * ((l - k3.w) * k3.x);\n"
        " } else {\n"
        "  response = k1.x + (k1.y - k1.x) * (l * k2.w);\n"
        " }\n"
        " precise float scale = a * response;\n"
        " if (scale == 0.0) { return p; }\n"
        " precise float px = (float(c.x) + g.x) * g.z;\n"
        " precise float py = (float(c.y) + g.y) * g.w;\n"
        " uint stream = uint(k3.y) | (uint(k3.z) << 16u);\n"
        " vec3 grain = bloom_film_grain_noise(px, py, stream);\n"
        " precise float r = p.r + scale * (k0.x * grain.x);\n"
        " precise float gr = p.g + scale * (k0.y * grain.y);\n"
        " precise float b = p.b + scale * (k0.z * grain.z);\n"
        " return vec4(r, gr, b, a);\n"
        "}\n";
    return text;
}

ImageEffectResult applyFilmGrain(const PreparedColorGrade& grade, const render::Rgba32fImage& input,
                                 const double horizontalScale, const double verticalScale,
                                 const std::size_t budget, const CancellationToken& cancellation,
                                 const OperationIndex operation,
                                 const EvaluationProgressCallback& progress,
                                 CpuRowBandExecutor* const rowBands) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    const auto& descriptor = *input.descriptor();
    const auto window = descriptor.dataWindow();
    const auto aspect = descriptor.pixelAspect();
    const auto table = filmGrainProgramData(
        grade, window,
        static_cast<double>(aspect.numerator()) / static_cast<double>(aspect.denominator()),
        horizontalScale, verticalScale);
    if (!table)
        return failed(EvaluationDiagnosticCode::InvalidParameter,
                      "Film Grain geometry cannot be represented");
    auto builder = render::Rgba32fImageBuilder::create(descriptor, budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Film Grain output exceeds the pixel budget");
    const auto view = input.view();
    if (!view)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Film Grain image is invalid");
    auto& image = *builder.value();
    const auto& sourceView = *view.value();
    const float* const k = table->data();
    const auto originY = window.originY();
    const auto total = static_cast<std::uint32_t>(window.extent().height());
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, total});
    const auto invalid = [](const char* const text) {
        return EvaluationDiagnostic{EvaluationDiagnosticCode::InvalidPixel,
                                    DiagnosticSeverity::Error,
                                    {},
                                    text,
                                    {}};
    };
    const auto outcome = runRowBandPass(
        rowBands, cancellation, total, originY,
        [&](const std::int64_t y) -> std::optional<EvaluationDiagnostic> {
            const auto source = sourceView.row(y);
            auto target = image.row(y);
            if (!source || !target)
                return EvaluationDiagnostic{EvaluationDiagnosticCode::InternalInvariant,
                                            DiagnosticSeverity::Error,
                                            {},
                                            "Film Grain row is unavailable",
                                            {}};
            for (std::size_t column = 0; column < source.value()->size(); ++column) {
                if ((column & 255U) == 0 && cancellation.isCancellationRequested())
                    return std::nullopt;
                const auto& s = (*source.value())[column];
                std::array<float, 4> p{s.red(), s.green(), s.blue(), s.alpha()};
                if (!filmGrainPixel(k, static_cast<std::int64_t>(column), y - originY, p))
                    return invalid("Film Grain produced a non-finite value");
                const auto pixel = render::Rgba32f::fromPremultiplied(p[0], p[1], p[2], p[3]);
                if (!pixel)
                    return invalid("Film Grain produced an invalid pixel");
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
                                                  "A Film Grain row band could not complete",
                                                  {}};
        return result;
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, total, total});
    auto frozen = std::move(image).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Film Grain publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

} // namespace bloom::runtime::detail
