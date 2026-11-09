#include "pixel_math_image_effect.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <bloom/document/image_math_parameters.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace bloom::runtime::detail {
namespace {

constexpr std::size_t kChannelBase = 4;
constexpr std::size_t kChannelFloats = 8;
// A Map Range's scale is limited so an extreme output span over a tiny input span stays a finite
// binary32 factor; the products it makes are then finite for every finite input within the domain.
constexpr double kMaximumScale = 1.0e30;

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic = EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, text, text};
    return result;
}

[[nodiscard]] float finiteOrZero(const float value) noexcept {
    return std::isfinite(value) ? value : 0.0F;
}

[[nodiscard]] float unit(const float value) noexcept {
    return std::min(std::max(value, 0.0F), 1.0F);
}

// A power, defined for every finite pair: 0^0 is 1; a zero base gives 0 (its negative powers are a
// pole, which is 0 like every non-finite result); a negative base needs an integral exponent, and an
// odd one keeps the sign.
[[nodiscard]] float powerOf(const float x, const float y) noexcept {
    if (y == 0.0F)
        return 1.0F;
    if (x == 0.0F)
        return 0.0F;
    if (x < 0.0F) {
        if (y != std::floor(y))
            return 0.0F;
        const float magnitude = std::pow(-x, y);
        return std::fmod(y, 2.0F) != 0.0F ? -magnitude : magnitude;
    }
    return std::pow(x, y);
}

// One channel through the operation: `p` is the channel's [p0..p3], `limits` its [low, high].
[[nodiscard]] float channelOf(const int operation, const unsigned flags, const float* p,
                              const float* limits, float x, float b) noexcept {
    x = finiteOrZero(x);
    b = finiteOrZero(b);
    float result = 0.0F;
    switch (static_cast<PixelMathOperation>(operation)) {
    case PixelMathOperation::Add:
        result = x + b;
        break;
    case PixelMathOperation::Subtract:
        result = x - b;
        break;
    case PixelMathOperation::Multiply:
        result = x * b;
        break;
    case PixelMathOperation::Divide:
        result = b == 0.0F ? 0.0F : x / b;
        break;
    case PixelMathOperation::Power:
        result = powerOf(x, b);
        break;
    case PixelMathOperation::Minimum:
        result = std::min(x, b);
        break;
    case PixelMathOperation::Maximum:
        result = std::max(x, b);
        break;
    case PixelMathOperation::Absolute:
        result = std::abs(x);
        break;
    case PixelMathOperation::MapRange: {
        const float difference = x - p[0];
        result = std::fma(difference, p[1], p[2]);
        if ((flags & 2U) != 0U)
            result = std::min(std::max(result, limits[0]), limits[1]);
        break;
    }
    case PixelMathOperation::Clamp:
        result = std::min(std::max(x, limits[0]), limits[1]);
        break;
    }
    return finiteOrZero(result);
}

[[nodiscard]] unsigned channelMask(const std::int64_t channels) noexcept {
    switch (channels) {
    case document::kImagePixelColor:
        return 7U;
    case document::kImagePixelRed:
        return 1U;
    case document::kImagePixelGreen:
        return 2U;
    case document::kImagePixelBlue:
        return 4U;
    case document::kImagePixelAlpha:
        return 8U;
    case document::kImagePixelColorAndAlpha:
        return 15U;
    default:
        return 0U;
    }
}

[[nodiscard]] bool operand(const double value) noexcept {
    return std::isfinite(value) && std::abs(value) <= document::kMaximumImagePixelValue;
}

} // namespace

std::optional<PreparedColorGrade> preparePixelMathGrade(const PixelMathKernel& kernel,
                                                        const std::span<const double> values) {
    const auto operation = kernel.operation;
    if (static_cast<std::uint8_t>(operation) > static_cast<std::uint8_t>(PixelMathOperation::Clamp))
        return std::nullopt;
    const bool arithmetic = operation <= PixelMathOperation::Absolute;
    const bool clampNode = operation == PixelMathOperation::Clamp;
    std::size_t expected = 0;
    if (arithmetic)
        expected = kernel.second || operation == PixelMathOperation::Absolute ? 0 : 1;
    else
        expected = clampNode ? 8 : 4;
    const unsigned mask = clampNode ? 15U : channelMask(kernel.channels);
    if (values.size() != expected || mask == 0U || (kernel.second && !arithmetic))
        return std::nullopt;
    if (!std::ranges::all_of(values, operand))
        return std::nullopt;
    PreparedColorGrade grade{ColorGradeKind::PixelMath, std::vector<float>(kPixelMathFloats, 0.0F),
                             false};
    auto& d = grade.data;
    float flags = (kernel.straight ? kPixelMathStraight : 0.0F) +
                  (kernel.clamp && operation == PixelMathOperation::MapRange ? kPixelMathClampFlag
                                                                             : 0.0F) +
                  (kernel.second ? kPixelMathSecondFlag : 0.0F);
    d[0] = static_cast<float>(static_cast<std::uint8_t>(operation));
    d[1] = flags;
    d[2] = static_cast<float>(mask);
    for (std::size_t channel = 0; channel < 4; ++channel) {
        float* p = d.data() + kChannelBase + channel * kChannelFloats;
        float* limits = p + 4;
        if (arithmetic) {
            p[0] = expected == 1 ? static_cast<float>(values[0]) : 0.0F;
        } else if (clampNode) {
            limits[0] = static_cast<float>(values[channel * 2]);
            limits[1] = static_cast<float>(values[channel * 2 + 1]);
            // The alpha channel's limits are within [0, 1].
            if (channel == 3 && !(limits[0] >= 0.0F && limits[1] <= 1.0F && limits[0] <= 1.0F &&
                                  limits[1] >= 0.0F))
                return std::nullopt;
        } else {
            const double inputMin = values[0];
            const double inputMax = values[1];
            const double outputMin = values[2];
            const double outputMax = values[3];
            const double span = inputMax - inputMin;
            const double scale =
                span == 0.0 ? 0.0
                            : std::clamp((outputMax - outputMin) / span, -kMaximumScale,
                                         kMaximumScale);
            p[0] = static_cast<float>(inputMin);
            p[1] = static_cast<float>(scale);
            p[2] = static_cast<float>(outputMin);
            limits[0] = static_cast<float>(std::min(outputMin, outputMax));
            limits[1] = static_cast<float>(std::max(outputMin, outputMax));
        }
    }
    return grade;
}

bool pixelMathReadsSecond(const std::vector<float>& table) noexcept {
    return table.size() >= kPixelMathFloats &&
           (static_cast<unsigned>(table[1]) & static_cast<unsigned>(kPixelMathSecondFlag)) != 0U;
}

bool pixelMathPixel(const std::vector<float>& table, std::array<float, 4>& pixel,
                    const std::array<float, 4>* second) noexcept {
    const int operation = static_cast<int>(table[0]);
    const unsigned flags = static_cast<unsigned>(table[1]);
    const unsigned mask = static_cast<unsigned>(table[2]);
    const bool straight = (flags & 1U) != 0U;
    const float alpha = pixel[3];
    // The operand of each channel: the second image's pixel (straight or stored as the node is
    // told), or the number in the channel's p0.
    std::array<float, 4> operands{};
    for (std::size_t channel = 0; channel < 4; ++channel)
        operands[channel] = table[kChannelBase + channel * kChannelFloats];
    if ((flags & 4U) != 0U) {
        const std::array<float, 4> none{};
        const auto& other = second != nullptr ? *second : none;
        const float otherAlpha = other[3];
        for (std::size_t channel = 0; channel < 3; ++channel)
            operands[channel] = straight ? (otherAlpha == 0.0F ? 0.0F : other[channel] / otherAlpha)
                                         : other[channel];
        operands[3] = otherAlpha;
    }
    const auto channelParameters = [&](const std::size_t channel) {
        return table.data() + kChannelBase + channel * kChannelFloats;
    };
    const bool alphaSelected = (mask & 8U) != 0U;
    float resultAlpha = finiteOrZero(alpha);
    if (alphaSelected)
        resultAlpha = unit(channelOf(operation, flags, channelParameters(3),
                                     channelParameters(3) + 4, alpha, operands[3]));
    std::array<float, 3> out{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const bool selected = (mask & (1U << channel)) != 0U;
        const float* p = channelParameters(channel);
        if (!straight) {
            out[channel] = selected ? channelOf(operation, flags, p, p + 4, pixel[channel],
                                                operands[channel])
                                    : finiteOrZero(pixel[channel]);
            continue;
        }
        if (!selected && !alphaSelected) {
            out[channel] = finiteOrZero(pixel[channel]);
            continue;
        }
        const float s = alpha == 0.0F ? 0.0F : pixel[channel] / alpha;
        const float y = selected ? channelOf(operation, flags, p, p + 4, s, operands[channel])
                                 : finiteOrZero(s);
        out[channel] = resultAlpha == 0.0F ? 0.0F : finiteOrZero(y * resultAlpha);
    }
    pixel = {out[0], out[1], out[2], resultAlpha};
    return true;
}

std::optional<std::vector<float>> pixelMathProgramData(const PreparedColorGrade& grade,
                                                       const std::int64_t outputWidth) {
    if (grade.kind != ColorGradeKind::PixelMath || grade.data.size() != kPixelMathFloats)
        return std::nullopt;
    auto data = grade.data;
    if (pixelMathReadsSecond(data)) {
        // A width of 0 says the second image has no pixels: its operand is transparent black.
        if (outputWidth < 0 || outputWidth > (std::int64_t{1} << 24))
            return std::nullopt;
        data.insert(data.end(), {static_cast<float>(outputWidth), 0.0F, 0.0F, 0.0F});
    }
    return data;
}

bool validPixelMathProgramData(const std::vector<float>& data) noexcept {
    if (data.size() != kPixelMathFloats && data.size() != kPixelMathProgramFloats)
        return false;
    const auto closed = [&](const float value, const float high) {
        return value >= 0.0F && value <= high && value == std::floor(value);
    };
    if (!closed(data[0], 9.0F) || !closed(data[1], 7.0F) || !closed(data[2], 15.0F))
        return false;
    const bool second = (static_cast<unsigned>(data[1]) & 4U) != 0U;
    if (second != (data.size() == kPixelMathProgramFloats))
        return false;
    return !second || (data[36] >= 0.0F && data[36] == std::floor(data[36]));
}

const std::string& pixelMathProgramText() {
    // The same binary32 operations as pixelMathPixel: exact additions, subtractions, products,
    // minima and maxima, the wrapper's correctly rounded division, a fused multiply-add for the map
    // and the backend's power. A non-finite value is 0 at every step the CPU makes it so.
    static const std::string text = std::string(R"GLSL(
layout(set = 0, binding = 0, std430) readonly buffer BloomPixelMathData { vec4 values[]; } bloom_pixel_math_data;
precise float bloom_ocio_cpu_div(float a, float b);
float bpm(int i) { return bloom_pixel_math_data.values[i >> 2][i & 3]; }
float bpm_finite(float x) { return (isnan(x) || isinf(x)) ? 0.0 : x; }
float bpm_unit(float x) { return min(max(x, 0.0), 1.0); }
float bpm_power(float x, float y) {
 if (y == 0.0) { return 1.0; }
 if (x == 0.0) { return 0.0; }
 if (x < 0.0) {
  if (y != floor(y)) { return 0.0; }
  float m = pow(-x, y);
  return mod(y, 2.0) != 0.0 ? -m : m;
 }
 return pow(x, y);
}
float bpm_channel(int op, int flags, int channel, float x, float b) {
 int base = 4 + channel * 8;
 x = bpm_finite(x);
 b = bpm_finite(b);
 float r = 0.0;
 if (op == 0) {
  precise float t = x + b;
  r = t;
 } else if (op == 1) {
  precise float t = x - b;
  r = t;
 } else if (op == 2) {
  precise float t = x * b;
  r = t;
 } else if (op == 3) {
  r = b == 0.0 ? 0.0 : bloom_ocio_cpu_div(x, b);
 } else if (op == 4) {
  r = bpm_power(x, b);
 } else if (op == 5) {
  r = min(x, b);
 } else if (op == 6) {
  r = max(x, b);
 } else if (op == 7) {
  r = abs(x);
 } else if (op == 8) {
  precise float d = x - bpm(base);
  precise float t = fma(d, bpm(base + 1), bpm(base + 2));
  r = t;
  if ((flags & 2) != 0) { r = min(max(r, bpm(base + 4)), bpm(base + 5)); }
 } else {
  r = min(max(x, bpm(base + 4)), bpm(base + 5));
 }
 return bpm_finite(r);
}
vec4 bloom_pixel_math(ivec2 c) {
 int op = int(bpm(0));
 int flags = int(bpm(1));
 int mask = int(bpm(2));
 bool straight = (flags & 1) != 0;
 int width = (flags & 4) != 0 ? int(bpm(36)) : 0;
 if (width > 0 && c.x >= width) { return imageLoad(bloom_ocio_input, c); }
 vec4 p = imageLoad(bloom_ocio_input, c);
 float alpha = p.a;
 float operands[4];
 for (int i = 0; i < 4; ++i) { operands[i] = bpm(4 + i * 8); }
 if ((flags & 4) != 0) {
  vec4 o = width > 0 ? imageLoad(bloom_ocio_input, c + ivec2(width, 0)) : vec4(0.0);
  for (int i = 0; i < 3; ++i) {
   operands[i] = straight ? (o.a == 0.0 ? 0.0 : bloom_ocio_cpu_div(o[i], o.a)) : o[i];
  }
  operands[3] = o.a;
 }
 bool alphaSelected = (mask & 8) != 0;
 float resultAlpha = bpm_finite(alpha);
 if (alphaSelected) { resultAlpha = bpm_unit(bpm_channel(op, flags, 3, alpha, operands[3])); }
 vec3 o3 = vec3(0.0);
 for (int i = 0; i < 3; ++i) {
  bool selected = (mask & (1 << i)) != 0;
  if (!straight) {
   o3[i] = selected ? bpm_channel(op, flags, i, p[i], operands[i]) : bpm_finite(p[i]);
   continue;
  }
  if (!selected && !alphaSelected) {
   o3[i] = bpm_finite(p[i]);
   continue;
  }
  float s = alpha == 0.0 ? 0.0 : bloom_ocio_cpu_div(p[i], alpha);
  float y = selected ? bpm_channel(op, flags, i, s, operands[i]) : bpm_finite(s);
  precise float m = y * resultAlpha;
  o3[i] = resultAlpha == 0.0 ? 0.0 : bpm_finite(m);
 }
 return vec4(o3, resultAlpha);
}
)GLSL");
    return text;
}

ImageEffectResult applyPixelMath(const PreparedColorGrade& grade, const render::Rgba32fImage& input,
                                 const render::Rgba32fImage* second, const std::size_t budget,
                                 const CancellationToken& cancellation,
                                 const OperationIndex operation,
                                 const EvaluationProgressCallback& progress) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    if (grade.kind != ColorGradeKind::PixelMath || grade.data.size() != kPixelMathFloats)
        return failed(EvaluationDiagnosticCode::InvalidParameter,
                      "Image math values are out of domain");
    if (second != nullptr && second->descriptor() == nullptr)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Image math second input is unavailable");
    auto builder = render::Rgba32fImageBuilder::create(*input.descriptor(), budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Image math output exceeds the pixel budget");
    const auto view = input.view();
    if (!view)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Image math input is invalid");
    std::optional<render::Rgba32fImageView> secondView;
    std::optional<render::ImageWindow> secondWindow;
    if (second != nullptr && pixelMathReadsSecond(grade.data)) {
        auto fetched = second->view();
        if (!fetched)
            return failed(EvaluationDiagnosticCode::InvalidPixel,
                          "Image math second input is invalid");
        secondView.emplace(*fetched.value());
        secondWindow = second->descriptor()->dataWindow();
    }
    const auto window = input.descriptor()->dataWindow();
    const auto rows = window.extent().height();
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, rows});
    for (auto y = window.originY(); y < window.maxYExclusive(); ++y) {
        if (cancellation.isCancellationRequested())
            return ImageEffectResult{{}, {}, true};
        const auto source = view.value()->row(y);
        auto target = builder.value()->row(y);
        if (!source || !target)
            return failed(EvaluationDiagnosticCode::InternalInvariant,
                          "Image math row is unavailable");
        std::optional<std::span<const render::Rgba32f>> secondRow;
        if (secondWindow && y >= secondWindow->originY() && y < secondWindow->maxYExclusive()) {
            const auto fetched = secondView->row(y);
            if (!fetched)
                return failed(EvaluationDiagnosticCode::InternalInvariant,
                              "Image math second row is unavailable");
            secondRow = *fetched.value();
        }
        for (std::size_t column = 0; column < source.value()->size(); ++column) {
            if ((column & 4095U) == 0 && cancellation.isCancellationRequested())
                return ImageEffectResult{{}, {}, true};
            const auto p = (*source.value())[column];
            std::array<float, 4> pixel{p.red(), p.green(), p.blue(), p.alpha()};
            std::array<float, 4> other{};
            const auto x = window.originX() + static_cast<std::int64_t>(column);
            if (secondRow && x >= secondWindow->originX() && x < secondWindow->maxXExclusive()) {
                const auto tap = (*secondRow)[static_cast<std::size_t>(x - secondWindow->originX())];
                other = {tap.red(), tap.green(), tap.blue(), tap.alpha()};
            }
            if (!pixelMathPixel(grade.data, pixel, &other))
                return failed(EvaluationDiagnosticCode::InvalidPixel,
                              "Image math produced a non-finite value");
            const auto result =
                render::Rgba32f::fromPremultiplied(pixel[0], pixel[1], pixel[2], pixel[3]);
            if (!result)
                return failed(EvaluationDiagnosticCode::InvalidPixel,
                              "Image math produced an invalid pixel");
            (*target.value())[column] = *result.value();
        }
        reportProgress(progress, {EvaluationProgressStage::Operation, operation,
                                  static_cast<std::uint64_t>(y - window.originY() + 1), rows});
    }
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Image math image publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

} // namespace bloom::runtime::detail
