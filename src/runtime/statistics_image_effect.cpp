// #349 Image Statistics. docs/architecture/image-statistics.md is the specification.
//
// The pixel arithmetic is comparisons, subtractions, multiplications, additions and correctly rounded
// divisions of binary32 values, in one fixed order: a tile's pixels in row-major order, then the
// tiles in row-major order. The CPU reference and the GPU program therefore agree exactly.

#include "statistics_image_effect.hpp"

#include "auto_exposure_image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <bloom/document/image_math_parameters.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace bloom::runtime::detail {
namespace {

constexpr std::int64_t kExactInteger = 16777216; // 2^24
// The Sum pass carries the count as high * 2^23 + low (statisticsCountOf).
constexpr float kCountBase = 8388608.0F; // 2^23
constexpr float kCountBaseInverse = 1.0F / 8388608.0F;
// Rec. 709 luminance of scene-linear color.
constexpr float kLumaRed = 0.2126F;
constexpr float kLumaGreen = 0.7152F;
constexpr float kLumaBlue = 0.0722F;
constexpr unsigned kFlagStraight = 1;
constexpr unsigned kFlagOpaqueOnly = 2;

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic =
        EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, std::string(text), {}};
    return result;
}

[[nodiscard]] bool flag(const float* const k, const unsigned bit) noexcept {
    return (static_cast<unsigned>(k[3]) & bit) != 0U;
}

// The measured value of a stored pixel, or false when the pixel has none: it is not counted, a color
// of a straight pixel without alpha, or not a usable number.
[[nodiscard]] bool measuredValue(const std::int64_t channel, const bool straight,
                                 const bool opaqueOnly, const std::array<float, 4>& p,
                                 float& value) noexcept {
    if (opaqueOnly && !(p[3] > 0.0F))
        return false;
    if (channel == document::kStatisticsAlpha) {
        value = p[3];
    } else {
        std::array<float, 3> c{p[0], p[1], p[2]};
        if (straight) {
            if (p[3] == 0.0F)
                return false;
            for (auto& part : c)
                part = part / p[3];
        }
        if (channel == document::kStatisticsLuminance) {
            const float red = kLumaRed * c[0];
            const float green = kLumaGreen * c[1];
            const float blue = kLumaBlue * c[2];
            const float partial = red + green;
            value = partial + blue;
        } else {
            value = c[static_cast<std::size_t>(channel - document::kStatisticsRed)];
        }
    }
    return std::isfinite(value) && std::abs(value) <= kStatisticsValueLimit;
}

struct Layout final {
    AutoExposureGrid grid;
    std::int64_t tiles = 1;
};
[[nodiscard]] Layout layoutOf(const float* const k, const std::int64_t sourceWidth,
                              const std::int64_t sourceHeight) noexcept {
    Layout layout;
    layout.grid = autoExposureGrid(sourceWidth, sourceHeight, static_cast<std::int64_t>(k[1]));
    layout.tiles = layout.grid.width * layout.grid.height;
    return layout;
}

// A tile's statistics: the count, minimum and maximum, and the mean and the sum of squared
// deviations from it.
struct Tile final {
    float count = 0.0F;
    float minimum = 0.0F;
    float maximum = 0.0F;
    float mean = 0.0F;
    float squares = 0.0F;
};

} // namespace

std::optional<PreparedColorGrade> prepareStatisticsGrade(const StatisticsKernel& kernel) {
    if (kernel.channel < document::kStatisticsLuminance ||
        kernel.channel > document::kStatisticsAlpha || kernel.grid < 1 || kernel.grid > 256)
        return std::nullopt;
    PreparedColorGrade grade{ColorGradeKind::Statistics,
                             std::vector<float>(kStatisticsPreparedFloats, 0.0F), false};
    auto& d = grade.data;
    d[0] = static_cast<float>(static_cast<std::uint8_t>(kernel.role));
    d[1] = kernel.role == StatisticsRole::Reduce ? static_cast<float>(kernel.grid) : 0.0F;
    d[2] = static_cast<float>(kernel.channel);
    d[3] = (kernel.straight ? static_cast<float>(kFlagStraight) : 0.0F) +
           (kernel.opaqueOnly ? static_cast<float>(kFlagOpaqueOnly) : 0.0F);
    return grade;
}

StatisticsStage statisticsStage(const PreparedColorGrade& grade) noexcept {
    return (grade.data.size() == kStatisticsPreparedFloats ||
            grade.data.size() == kStatisticsProgramFloats) &&
                   grade.data[0] == 1.0F
               ? StatisticsStage::Sum
               : StatisticsStage::Reduce;
}

std::array<std::int64_t, 2> statisticsOutputSize(const PreparedColorGrade& grade,
                                                 const std::int64_t sourceWidth,
                                                 const std::int64_t sourceHeight) noexcept {
    if (statisticsStage(grade) == StatisticsStage::Sum)
        return {kStatisticsResultWidth, 1};
    const auto layout = layoutOf(grade.data.data(), sourceWidth, sourceHeight);
    return {kStatisticsResultWidth, layout.tiles};
}

std::optional<std::vector<float>> statisticsProgramData(const PreparedColorGrade& grade,
                                                        const std::int64_t sourceWidth,
                                                        const std::int64_t sourceHeight) {
    if (grade.kind != ColorGradeKind::Statistics ||
        grade.data.size() != kStatisticsPreparedFloats || sourceWidth < 1 || sourceHeight < 1 ||
        sourceWidth > kExactInteger || sourceHeight > kExactInteger)
        return std::nullopt;
    std::vector<float> d(kStatisticsProgramFloats, 0.0F);
    std::copy(grade.data.begin(), grade.data.end(), d.begin());
    const auto size = statisticsOutputSize(grade, sourceWidth, sourceHeight);
    // A Sum reads the rows of a Reduce, one tile each: its "tiles" are those rows.
    const bool sum = statisticsStage(grade) == StatisticsStage::Sum;
    const auto layout = layoutOf(grade.data.data(), sourceWidth, sourceHeight);
    d[32] = static_cast<float>(sourceWidth);
    d[33] = static_cast<float>(sourceHeight);
    d[34] = static_cast<float>(sum ? 1 : layout.grid.tileWidth);
    d[35] = static_cast<float>(sum ? 1 : layout.grid.tileHeight);
    d[36] = static_cast<float>(sum ? 1 : layout.grid.width);
    d[37] = static_cast<float>(sum ? 1 : layout.grid.height);
    d[38] = static_cast<float>(size[0]);
    d[39] = static_cast<float>(size[1]);
    if (!std::ranges::all_of(d, [](const float v) { return std::isfinite(v); }))
        return std::nullopt;
    return d;
}

bool validStatisticsProgramData(const std::vector<float>& d) noexcept {
    if (d.size() != kStatisticsProgramFloats ||
        !std::ranges::all_of(d, [](const float v) { return std::isfinite(v); }))
        return false;
    const auto whole = [](const float v, const float low, const float high) {
        return v >= low && v <= high && v == std::floor(v);
    };
    return whole(d[0], 0.0F, 1.0F) && whole(d[2], 0.0F, 4.0F) && whole(d[3], 0.0F, 3.0F) &&
           d[32] >= 1.0F && d[33] >= 1.0F && d[34] >= 1.0F && d[35] >= 1.0F && d[36] >= 1.0F &&
           d[37] >= 1.0F && d[38] >= 1.0F && d[39] >= 1.0F;
}

const std::string& statisticsProgramText() {
    // The same binary32 operations as applyStatistics: precise comparisons, differences, products
    // and sums, the wrapper's correctly rounded division and no fused multiply-add.
    static const std::string text = std::string(R"GLSL(
layout(set = 0, binding = 0, std430) readonly buffer BloomStatisticsData { vec4 values[]; } bloom_statistics_data;
precise float bloom_ocio_cpu_div(float a, float b);
float bsg(int i) { return bloom_statistics_data.values[i >> 2][i & 3]; }
bool bsg_value(int channel, vec4 p, bool straight, bool opaque, out float v) {
 v = 0.0;
 if (opaque && !(p.a > 0.0)) { return false; }
 if (channel == 4) {
  v = p.a;
 } else {
  vec3 c = p.rgb;
  if (straight) {
   if (p.a == 0.0) { return false; }
   c = vec3(bloom_ocio_cpu_div(c.r, p.a), bloom_ocio_cpu_div(c.g, p.a), bloom_ocio_cpu_div(c.b, p.a));
  }
  if (channel == 0) {
   precise float lr = 0.2126 * c.r;
   precise float lg = 0.7152 * c.g;
   precise float lb = 0.0722 * c.b;
   precise float partial = lr + lg;
   precise float luma = partial + lb;
   v = luma;
  } else {
   v = c[channel - 1];
  }
 }
 if (isnan(v) || isinf(v) || abs(v) > 1.0e12) { return false; }
 return true;
}
vec4 bloom_statistics(ivec2 c) {
 int stage = int(bsg(0));
 int outW = int(bsg(38));
 int outH = int(bsg(39));
 if (c.x >= outW || c.y >= outH) { return vec4(0.0, 0.0, 0.0, 1.0); }
 if (stage == 0) {
  int channel = int(bsg(2));
  bool straight = (int(bsg(3)) & 1) != 0;
  bool opaque = (int(bsg(3)) & 2) != 0;
  int tilesX = int(bsg(36));
  int tw = int(bsg(34));
  int th = int(bsg(35));
  int x0 = (c.y % tilesX) * tw;
  int y0 = (c.y / tilesX) * th;
  int x1 = min(x0 + tw, int(bsg(32)));
  int y1 = min(y0 + th, int(bsg(33)));
  int n = 0;
  float mn = 0.0;
  float mx = 0.0;
  precise float sum = 0.0;
  for (int y = y0; y < y1; ++y) {
   for (int x = x0; x < x1; ++x) {
    float v;
    if (bsg_value(channel, imageLoad(bloom_ocio_input, ivec2(x, y)), straight, opaque, v)) {
     if (n == 0) { mn = v; mx = v; } else { mn = min(mn, v); mx = max(mx, v); }
     n++;
     sum = sum + v;
    }
   }
  }
  if (n == 0) { return vec4(0.0, 0.0, 0.0, 1.0); }
  if (c.x == 0) { return vec4(float(n), mn, mx, 1.0); }
  precise float mean = bloom_ocio_cpu_div(sum, float(n));
  precise float squares = 0.0;
  for (int y = y0; y < y1; ++y) {
   for (int x = x0; x < x1; ++x) {
    float v;
    if (bsg_value(channel, imageLoad(bloom_ocio_input, ivec2(x, y)), straight, opaque, v)) {
     precise float d = v - mean;
     precise float dd = d * d;
     squares = squares + dd;
    }
   }
  }
  return vec4(mean, squares, 0.0, 1.0);
 }
 int rows = int(bsg(33));
 precise float high = 0.0;
 precise float low = 0.0;
 precise float n = 0.0;
 precise float mean = 0.0;
 precise float squares = 0.0;
 float mn = 0.0;
 float mx = 0.0;
 for (int t = 0; t < rows; ++t) {
  vec4 a = imageLoad(bloom_ocio_input, ivec2(0, t));
  vec4 b = imageLoad(bloom_ocio_input, ivec2(1, t));
  float nb = a.x;
  if (nb == 0.0) { continue; }
  precise float nbScaled = nb * (1.0 / 8388608.0);
  precise float nbHigh = floor(nbScaled);
  precise float nbHighWhole = nbHigh * 8388608.0;
  precise float nbLow = nb - nbHighWhole;
  low = low + nbLow;
  if (low >= 8388608.0) { low = low - 8388608.0; high = high + 1.0; }
  high = high + nbHigh;
  precise float highWhole = high * 8388608.0;
  precise float total = highWhole + low;
  if (n == 0.0) {
   n = total; mean = b.x; squares = b.y; mn = a.y; mx = a.z;
  } else {
   precise float delta = b.x - mean;
   precise float share = bloom_ocio_cpu_div(nb, total);
   precise float move = delta * share;
   precise float weight = n * nb;
   precise float scaled = bloom_ocio_cpu_div(weight, total);
   precise float dd = delta * delta;
   precise float between = dd * scaled;
   precise float joined = b.y + between;
   mean = mean + move;
   squares = squares + joined;
   mn = min(mn, a.y);
   mx = max(mx, a.z);
   n = total;
  }
 }
 if (c.x == 0) { return vec4(n, mn, mx, 1.0); }
 precise float variance = 0.0;
 if (n > 0.0) { variance = bloom_ocio_cpu_div(squares, n); }
 precise float highAll = high * 8388608.0;
 precise float above = highAll - n;
 precise float residual = above + low;
 return vec4(mean, variance, residual, 1.0);
}
)GLSL");
    return text;
}

ImageEffectResult applyStatistics(const PreparedColorGrade& grade,
                                  const render::Rgba32fImage* const input, const std::size_t budget,
                                  const CancellationToken& cancellation,
                                  const OperationIndex operation,
                                  const EvaluationProgressCallback& progress,
                                  CpuRowBandExecutor* const rowBands) {
    if (cancellation.isCancellationRequested())
        return ImageEffectResult{{}, {}, true};
    if (input == nullptr) {
        ImageEffectResult none;
        none.empty = true;
        return none;
    }
    if (grade.kind != ColorGradeKind::Statistics || grade.data.size() != kStatisticsPreparedFloats)
        return failed(EvaluationDiagnosticCode::InvalidParameter,
                      "Image Statistics values are out of domain");
    const auto view = input->view();
    if (!view)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Image Statistics image is invalid");
    const float* const k = grade.data.data();
    const auto stage = statisticsStage(grade);
    const auto sourceWindow = input->descriptor()->dataWindow();
    const auto sourceWidth = static_cast<std::int64_t>(sourceWindow.extent().width());
    const auto sourceHeight = static_cast<std::int64_t>(sourceWindow.extent().height());
    const auto size = statisticsOutputSize(grade, sourceWidth, sourceHeight);
    // The result sits at the origin of the composition, wherever the picture it measures is, so that
    // a route reads back just that window.
    const auto outputWindow = render::ImageWindow::create(
        0, 0, static_cast<std::uint64_t>(size[0]), static_cast<std::uint64_t>(size[1]));
    if (!outputWindow)
        return failed(EvaluationDiagnosticCode::InvalidPixel,
                      "Image Statistics output window is invalid");
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *outputWindow.value(), *outputWindow.value(), input->descriptor()->pixelAspect());
    if (!descriptor)
        return failed(EvaluationDiagnosticCode::InvalidPixel,
                      "Image Statistics output descriptor is invalid");
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Image Statistics output exceeds the pixel budget");
    auto& image = *builder.value();
    const auto channel = static_cast<std::int64_t>(k[2]);
    const bool straight = flag(k, kFlagStraight);
    const bool opaqueOnly = flag(k, kFlagOpaqueOnly);
    const auto total = static_cast<std::uint32_t>(size[1]);
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, total});

    const auto invalid = [](const char* const text) {
        return EvaluationDiagnostic{EvaluationDiagnosticCode::InvalidPixel,
                                    DiagnosticSeverity::Error,
                                    {},
                                    text,
                                    {}};
    };
    const auto put = [&](const std::int64_t x, const float a, const float b, const float c,
                         std::span<render::Rgba32f> row) -> std::optional<EvaluationDiagnostic> {
        const auto pixel = render::Rgba32f::fromPremultiplied(a, b, c, 1.0F);
        if (!pixel)
            return invalid("Image Statistics produced an invalid pixel");
        row[static_cast<std::size_t>(x)] = *pixel.value();
        return std::nullopt;
    };

    if (stage == StatisticsStage::Sum) {
        auto row = image.row(0);
        if (!row)
            return failed(EvaluationDiagnosticCode::InternalInvariant,
                          "Image Statistics row is unavailable");
        // The count is carried exactly as high * 2^23 + low, both whole binary32 numbers, beside
        // `n`, the count rounded to binary32 that weighs the mean and variance combination (equal to
        // the exact count up to 2^24). A tile's count is whole and at most 2^24.
        float high = 0.0F;
        float low = 0.0F;
        float n = 0.0F;
        float mean = 0.0F;
        float squares = 0.0F;
        float minimum = 0.0F;
        float maximum = 0.0F;
        for (auto y = sourceWindow.originY(); y < sourceWindow.maxYExclusive(); ++y) {
            if (((y - sourceWindow.originY()) & 255) == 0 && cancellation.isCancellationRequested())
                return ImageEffectResult{{}, {}, true};
            const auto source = view.value()->row(y);
            if (!source)
                return failed(EvaluationDiagnosticCode::InternalInvariant,
                              "Image Statistics row is unavailable");
            const auto& a = (*source.value())[0];
            const auto& b = (*source.value())[1];
            const float nb = a.red();
            if (nb == 0.0F)
                continue;
            const float nbHigh = std::floor(nb * kCountBaseInverse);
            const float nbLow = nb - nbHigh * kCountBase;
            low = low + nbLow;
            if (low >= kCountBase) {
                low = low - kCountBase;
                high = high + 1.0F;
            }
            high = high + nbHigh;
            const float joint = high * kCountBase + low;
            if (n == 0.0F) {
                n = joint;
                mean = b.red();
                squares = b.green();
                minimum = a.green();
                maximum = a.blue();
            } else {
                const float delta = b.red() - mean;
                const float share = nb / joint;
                const float move = delta * share;
                const float weight = n * nb;
                const float scaled = weight / joint;
                const float dd = delta * delta;
                const float between = dd * scaled;
                const float joined = b.green() + between;
                mean = mean + move;
                squares = squares + joined;
                minimum = std::min(minimum, a.green());
                maximum = std::max(maximum, a.blue());
                n = joint;
            }
        }
        float variance = 0.0F;
        if (n > 0.0F)
            variance = squares / n;
        // What the binary32 count leaves out: a whole number, exact in binary32.
        const float above = high * kCountBase - n;
        const float residual = above + low;
        if (const auto bad = put(0, n, minimum, maximum, *row.value()))
            return failed(bad->code, "Image Statistics produced an invalid result");
        if (const auto bad = put(1, mean, variance, residual, *row.value()))
            return failed(bad->code, "Image Statistics produced an invalid result");
    } else {
        const auto layout = layoutOf(k, sourceWidth, sourceHeight);
        const auto outcome = runRowBandPass(
            rowBands, cancellation, total, 0,
            [&](const std::int64_t absoluteY) -> std::optional<EvaluationDiagnostic> {
                auto row = image.row(absoluteY);
                if (!row)
                    return EvaluationDiagnostic{EvaluationDiagnosticCode::InternalInvariant,
                                                DiagnosticSeverity::Error,
                                                {},
                                                "Image Statistics row is unavailable",
                                                {}};
                // This row is one tile: its pixels in row-major order.
                const auto y = absoluteY;
                const auto x0 = (y % layout.grid.width) * layout.grid.tileWidth;
                const auto y0 = (y / layout.grid.width) * layout.grid.tileHeight;
                const auto x1 = std::min(x0 + layout.grid.tileWidth, sourceWidth);
                const auto y1 = std::min(y0 + layout.grid.tileHeight, sourceHeight);
                const auto visit = [&](auto&& each) -> std::optional<EvaluationDiagnostic> {
                    for (auto yy = y0; yy < y1; ++yy) {
                        if (((yy - y0) & 63) == 0 && cancellation.isCancellationRequested())
                            return std::nullopt;
                        const auto sourceRow = view.value()->row(sourceWindow.originY() + yy);
                        if (!sourceRow)
                            return invalid("Image Statistics image is invalid");
                        for (auto xx = x0; xx < x1; ++xx) {
                            const auto& p = (*sourceRow.value())[static_cast<std::size_t>(xx)];
                            const std::array<float, 4> pixel{p.red(), p.green(), p.blue(),
                                                             p.alpha()};
                            float value = 0.0F;
                            if (measuredValue(channel, straight, opaqueOnly, pixel, value))
                                each(value);
                        }
                    }
                    return std::nullopt;
                };
                Tile tile;
                float sum = 0.0F;
                if (const auto bad = visit([&](const float value) {
                        if (tile.count == 0.0F) {
                            tile.minimum = value;
                            tile.maximum = value;
                        } else {
                            tile.minimum = std::min(tile.minimum, value);
                            tile.maximum = std::max(tile.maximum, value);
                        }
                        tile.count = tile.count + 1.0F;
                        sum = sum + value;
                    }))
                    return bad;
                if (tile.count > 0.0F) {
                    tile.mean = sum / tile.count;
                    if (const auto bad = visit([&](const float value) {
                            const float d = value - tile.mean;
                            const float dd = d * d;
                            tile.squares = tile.squares + dd;
                        }))
                        return bad;
                }
                if (const auto bad = put(0, tile.count, tile.minimum, tile.maximum, *row.value()))
                    return bad;
                return put(1, tile.mean, tile.squares, 0.0F, *row.value());
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
                                                      "An Image Statistics row band could not complete",
                                                      {}};
            return result;
        }
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, total, total});
    auto frozen = std::move(image).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Image Statistics publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

} // namespace bloom::runtime::detail
