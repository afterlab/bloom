// #349 Histogram. docs/architecture/image-filters.md, "Histogram", is the specification.
//
// The pixel arithmetic is subtractions, multiplications, floors, comparisons, correctly rounded
// divisions, additions of whole numbers and, for a logarithmic graph, bloom::core::specifiedLog2
// (a logarithm of base two made of the same operations on every processor and GPU), in one fixed
// order. The counts are whole numbers below 2^24 in every pass but the sum, which adds them
// in a fixed order, so the CPU reference and the GPU program count identically.

#include "histogram_image_effect.hpp"

#include "auto_exposure_image_effect.hpp"
#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <bloom/core/specified_log2.hpp>
#include <bloom/document/image_math_parameters.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace bloom::runtime::detail {
namespace {

constexpr std::int64_t kExactInteger = 16777216; // 2^24
// Rec. 709 luminance of scene-linear color.
constexpr float kLumaRed = 0.2126F;
constexpr float kLumaGreen = 0.7152F;
constexpr float kLumaBlue = 0.0722F;
// The graph's background and the colors of what it plots, in scene-linear color.
constexpr float kBackground = 0.02F;
constexpr std::array<std::array<float, 3>, 5> kSourceColors{{{1.0F, 0.0F, 0.0F},
                                                              {0.0F, 1.0F, 0.0F},
                                                              {0.0F, 0.0F, 1.0F},
                                                              {0.75F, 0.75F, 0.75F},
                                                              {0.75F, 0.75F, 0.75F}}};
constexpr std::size_t kSources = 9;
constexpr std::size_t kColors = 16;
constexpr std::size_t kFlagStraight = 1;
// 2 was the logarithmic graph drawn with each backend's own log2. It is retired and refused, so a
// table made for it never reads as one made for the specified logarithm.
constexpr std::size_t kFlagRetiredLogarithmic = 2;
constexpr std::size_t kFlagLogarithmic = 8;
constexpr std::size_t kFlagEmpty = 4;

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic =
        EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, std::string(text), {}};
    return result;
}

[[nodiscard]] std::vector<HistogramSource> sourcesOf(const std::int64_t channels) {
    using Source = HistogramSource;
    switch (channels) {
    case document::kHistogramRgb:
        return {Source::Red, Source::Green, Source::Blue};
    case document::kHistogramLuminance:
        return {Source::Luminance};
    case document::kHistogramRgbLuminance:
        return {Source::Red, Source::Green, Source::Blue, Source::Luminance};
    case document::kHistogramRed:
        return {Source::Red};
    case document::kHistogramGreen:
        return {Source::Green};
    case document::kHistogramBlue:
        return {Source::Blue};
    case document::kHistogramAlpha:
        return {Source::Alpha};
    default:
        return {};
    }
}

[[nodiscard]] bool flag(const float* const k, const std::size_t bit) noexcept {
    return (static_cast<unsigned>(k[4]) & static_cast<unsigned>(bit)) != 0U;
}

// A counted channel's value for a stored pixel, or false when the pixel has none: a color channel of
// a straight pixel without alpha.
[[nodiscard]] bool channelValue(const HistogramSource source, const bool straight,
                                const std::array<float, 4>& p, float& value) noexcept {
    if (source == HistogramSource::Alpha) {
        value = p[3];
        return true;
    }
    std::array<float, 3> c{p[0], p[1], p[2]};
    if (straight) {
        if (p[3] == 0.0F)
            return false;
        for (auto& channel : c)
            channel = channel / p[3];
    }
    if (source == HistogramSource::Luminance) {
        const float red = kLumaRed * c[0];
        const float green = kLumaGreen * c[1];
        const float blue = kLumaBlue * c[2];
        const float partial = red + green;
        value = partial + blue;
        return true;
    }
    value = c[static_cast<std::size_t>(source)];
    return true;
}

[[nodiscard]] std::int64_t ceilDiv(const std::int64_t a, const std::int64_t b) noexcept {
    return (a + b - 1) / b;
}

[[nodiscard]] std::int64_t channelCount(const float* const k) noexcept {
    return static_cast<std::int64_t>(k[8]);
}

// The tiles a frame is cut into and the width of the Bin pass's output.
struct Layout final {
    AutoExposureGrid grid;
    std::int64_t groups = 1;
    std::int64_t width = 1;
    std::int64_t tiles = 1;
};
[[nodiscard]] Layout layoutOf(const float* const k, const std::int64_t sourceWidth,
                              const std::int64_t sourceHeight) noexcept {
    Layout layout;
    layout.grid = autoExposureGrid(sourceWidth, sourceHeight, static_cast<std::int64_t>(k[1]));
    layout.groups = histogramGroups(static_cast<std::int64_t>(k[2]));
    layout.width = layout.groups * channelCount(k);
    layout.tiles = layout.grid.width * layout.grid.height;
    return layout;
}

} // namespace

std::int64_t histogramGroups(const std::int64_t bins) noexcept { return ceilDiv(bins, 3); }

int histogramBin(const float value, const float low, const float high, const float scale,
                 const int bins) noexcept {
    if (!std::isfinite(value) || !(value >= low) || !(value <= high))
        return -1;
    const float shifted = value - low;
    const float position = shifted * scale;
    return std::min(static_cast<int>(std::floor(position)), bins - 1);
}

std::optional<PreparedColorGrade> prepareHistogramGrade(const HistogramKernel& kernel,
                                                        const std::span<const double> values) {
    const auto sources = sourcesOf(kernel.channels);
    if (sources.empty() || kernel.bins < 3 || kernel.bins > 4096 || kernel.height < 1 ||
        kernel.height > 4096 || kernel.grid < 1 || kernel.grid > 256)
        return std::nullopt;
    PreparedColorGrade grade{ColorGradeKind::Histogram,
                             std::vector<float>(kHistogramPreparedFloats, 0.0F), false};
    auto& d = grade.data;
    const bool bin = kernel.role == HistogramRole::Bin;
    if (values.size() != (bin ? 2U : 0U))
        return std::nullopt;
    d[0] = static_cast<float>(static_cast<std::uint8_t>(kernel.role));
    d[1] = bin ? static_cast<float>(kernel.grid) : 0.0F;
    d[2] = static_cast<float>(kernel.bins);
    d[3] = static_cast<float>(kernel.height);
    float flags = (kernel.straight ? static_cast<float>(kFlagStraight) : 0.0F) +
                  (kernel.logarithmic ? static_cast<float>(kFlagLogarithmic) : 0.0F);
    if (bin) {
        for (const double value : values)
            if (!std::isfinite(value) || std::abs(value) > document::kMaximumImagePixelValue)
                return std::nullopt;
        const double low = values[0];
        const double high = values[1];
        const double scale = (high > low) ? static_cast<double>(kernel.bins) / (high - low) : 0.0;
        // A range too narrow to scale to the bins in binary32 counts nothing.
        if (!(high > low) || !std::isfinite(static_cast<float>(scale)) ||
            static_cast<float>(scale) <= 0.0F)
            flags += static_cast<float>(kFlagEmpty);
        else
            d[7] = static_cast<float>(scale);
        d[5] = static_cast<float>(low);
        d[6] = static_cast<float>(high);
    }
    d[4] = flags;
    d[8] = static_cast<float>(sources.size());
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const auto source = static_cast<std::size_t>(sources[i]);
        d[kSources + i] = static_cast<float>(source);
        for (std::size_t c = 0; c < 3; ++c)
            d[kColors + 3 * i + c] = kSourceColors[source][c];
    }
    return grade;
}

HistogramStage histogramStage(const PreparedColorGrade& grade) noexcept {
    return (grade.data.size() == kHistogramPreparedFloats ||
            grade.data.size() == kHistogramProgramFloats) &&
                   grade.data[0] >= 0.0F && grade.data[0] <= 2.0F
               ? static_cast<HistogramStage>(static_cast<int>(grade.data[0]))
               : HistogramStage::Bin;
}

std::array<std::int64_t, 2> histogramOutputSize(const PreparedColorGrade& grade,
                                                const std::int64_t sourceWidth,
                                                const std::int64_t sourceHeight) noexcept {
    const float* const k = grade.data.data();
    switch (histogramStage(grade)) {
    case HistogramStage::Bin: {
        const auto layout = layoutOf(k, sourceWidth, sourceHeight);
        return {layout.width, layout.tiles};
    }
    case HistogramStage::Sum:
        return {sourceWidth, 1};
    case HistogramStage::Plot:
        break;
    }
    return {static_cast<std::int64_t>(k[2]), static_cast<std::int64_t>(k[3])};
}

std::optional<std::vector<float>> histogramProgramData(const PreparedColorGrade& grade,
                                                       const std::int64_t sourceWidth,
                                                       const std::int64_t sourceHeight) {
    if (grade.kind != ColorGradeKind::Histogram || grade.data.size() != kHistogramPreparedFloats ||
        sourceWidth < 1 || sourceHeight < 1 || sourceWidth > kExactInteger ||
        sourceHeight > kExactInteger)
        return std::nullopt;
    std::vector<float> d(kHistogramProgramFloats, 0.0F);
    std::copy(grade.data.begin(), grade.data.end(), d.begin());
    const float* const k = grade.data.data();
    const auto layout = layoutOf(k, sourceWidth, sourceHeight);
    const auto size = histogramOutputSize(grade, sourceWidth, sourceHeight);
    d[32] = static_cast<float>(sourceWidth);
    d[33] = static_cast<float>(sourceHeight);
    d[34] = static_cast<float>(layout.grid.tileWidth);
    d[35] = static_cast<float>(layout.grid.tileHeight);
    d[36] = static_cast<float>(layout.grid.width);
    d[37] = static_cast<float>(layout.grid.height);
    d[38] = static_cast<float>(size[0]);
    d[39] = static_cast<float>(size[1]);
    d[40] = static_cast<float>(layout.groups);
    if (!std::ranges::all_of(d, [](const float v) { return std::isfinite(v); }))
        return std::nullopt;
    return d;
}

bool validHistogramProgramData(const std::vector<float>& d) noexcept {
    if (d.size() != kHistogramProgramFloats ||
        !std::ranges::all_of(d, [](const float v) { return std::isfinite(v); }))
        return false;
    const auto whole = [](const float v, const float low, const float high) {
        return v >= low && v <= high && v == std::floor(v);
    };
    if (!whole(d[0], 0.0F, 2.0F) || !whole(d[2], 3.0F, 4096.0F) || !whole(d[3], 1.0F, 4096.0F) ||
        !whole(d[4], 0.0F, 15.0F) || flag(d.data(), kFlagRetiredLogarithmic) ||
        !whole(d[8], 1.0F, 4.0F))
        return false;
    for (std::size_t i = 0; i < static_cast<std::size_t>(d[8]); ++i)
        if (!whole(d[kSources + i], 0.0F, 4.0F))
            return false;
    return d[32] >= 1.0F && d[33] >= 1.0F && d[34] >= 1.0F && d[35] >= 1.0F && d[36] >= 1.0F &&
           d[37] >= 1.0F && d[38] >= 1.0F && d[39] >= 1.0F && d[40] >= 1.0F;
}

const std::string& histogramProgramText() {
    // The same binary32 operations as applyHistogram: precise subtractions, products and sums, the
    // wrapper's correctly rounded division, the specified logarithm and no fused multiply-add. The
    // counts are whole numbers.
    static const std::string text = std::string(R"GLSL(
layout(set = 0, binding = 0, std430) readonly buffer BloomHistogramData { vec4 values[]; } bloom_histogram_data;
precise float bloom_ocio_cpu_div(float a, float b);
precise float bloom_specified_log2(float x);
float bhg(int i) { return bloom_histogram_data.values[i >> 2][i & 3]; }
int bhg_bin(float v) {
 if (isnan(v) || isinf(v)) { return -1; }
 if (!(v >= bhg(5)) || !(v <= bhg(6))) { return -1; }
 precise float shifted = v - bhg(5);
 precise float position = shifted * bhg(7);
 return min(int(floor(position)), int(bhg(2)) - 1);
}
bool bhg_value(int source, vec4 p, bool straight, out float v) {
 v = 0.0;
 if (source == 3) { v = p.a; return true; }
 vec3 c = p.rgb;
 if (straight) {
  if (p.a == 0.0) { return false; }
  c = vec3(bloom_ocio_cpu_div(c.r, p.a), bloom_ocio_cpu_div(c.g, p.a), bloom_ocio_cpu_div(c.b, p.a));
 }
 if (source == 4) {
  precise float lr = 0.2126 * c.r;
  precise float lg = 0.7152 * c.g;
  precise float lb = 0.0722 * c.b;
  precise float partial = lr + lg;
  precise float luma = partial + lb;
  v = luma;
  return true;
 }
 v = c[source];
 return true;
}
vec4 bloom_histogram(ivec2 c) {
 int stage = int(bhg(0));
 int outW = int(bhg(38));
 int outH = int(bhg(39));
 int groups = int(bhg(40));
 int channels = int(bhg(8));
 if (stage == 0) {
  if (c.x >= outW || c.y >= outH) { return vec4(0.0, 0.0, 0.0, 1.0); }
  int channel = c.x / groups;
  int base = (c.x - channel * groups) * 3;
  int source = int(bhg(9 + channel));
  bool straight = (int(bhg(4)) & 1) != 0;
  bool empty = (int(bhg(4)) & 4) != 0;
  int tilesX = int(bhg(36));
  int tw = int(bhg(34));
  int th = int(bhg(35));
  int x0 = (c.y % tilesX) * tw;
  int y0 = (c.y / tilesX) * th;
  int x1 = min(x0 + tw, int(bhg(32)));
  int y1 = min(y0 + th, int(bhg(33)));
  int n0 = 0;
  int n1 = 0;
  int n2 = 0;
  if (!empty) {
   for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
     float v;
     if (bhg_value(source, imageLoad(bloom_ocio_input, ivec2(x, y)), straight, v)) {
      int b = bhg_bin(v);
      if (b == base) { n0++; }
      else if (b == base + 1) { n1++; }
      else if (b == base + 2) { n2++; }
     }
    }
   }
  }
  return vec4(float(n0), float(n1), float(n2), 1.0);
 }
 if (stage == 1) {
  if (c.y != 0 || c.x >= outW) { return imageLoad(bloom_ocio_input, c); }
  int rows = int(bhg(33));
  precise float s0 = 0.0;
  precise float s1 = 0.0;
  precise float s2 = 0.0;
  for (int t = 0; t < rows; ++t) {
   vec3 n = imageLoad(bloom_ocio_input, ivec2(c.x, t)).rgb;
   s0 = s0 + n.x;
   s1 = s1 + n.y;
   s2 = s2 + n.z;
  }
  return vec4(s0, s1, s2, 1.0);
 }
 if (c.x >= outW || c.y >= outH) { return vec4(0.0, 0.0, 0.0, 1.0); }
 bool logarithmic = (int(bhg(4)) & 8) != 0;
 float peak = 0.0;
 for (int channel = 0; channel < channels; ++channel) {
  for (int g = 0; g < groups; ++g) {
   vec3 n = imageLoad(bloom_ocio_input, ivec2(channel * groups + g, 0)).rgb;
   peak = max(peak, max(n.x, max(n.y, n.z)));
  }
 }
 int group = c.x / 3;
 int part = c.x - group * 3;
 float fromBottom = float(outH - 1 - c.y);
 precise vec3 color = vec3(0.02);
 for (int channel = 0; channel < channels; ++channel) {
  float count = imageLoad(bloom_ocio_input, ivec2(channel * groups + group, 0))[part];
  float share = 0.0;
  if (peak > 0.0) {
   if (logarithmic) {
    precise float above = 1.0 + count;
    precise float top = 1.0 + peak;
    share = bloom_ocio_cpu_div(bloom_specified_log2(above), bloom_specified_log2(top));
   } else {
    share = bloom_ocio_cpu_div(count, peak);
   }
  }
  precise float fill = share * float(outH);
  precise float over = fill - fromBottom;
  float coverage = min(max(over, 0.0), 1.0);
  precise float cr = coverage * bhg(16 + 3 * channel);
  precise float cg = coverage * bhg(17 + 3 * channel);
  precise float cb = coverage * bhg(18 + 3 * channel);
  color = vec3(color.r + cr, color.g + cg, color.b + cb);
 }
 return vec4(color, 1.0);
}
)GLSL") + std::string(core::specifiedLog2Glsl());
    return text;
}

ImageEffectResult applyHistogram(const PreparedColorGrade& grade,
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
    if (grade.kind != ColorGradeKind::Histogram || grade.data.size() != kHistogramPreparedFloats)
        return failed(EvaluationDiagnosticCode::InvalidParameter, "Histogram values are out of domain");
    const auto view = input->view();
    if (!view)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Histogram image is invalid");
    const float* const k = grade.data.data();
    const auto stage = histogramStage(grade);
    const auto sourceWindow = input->descriptor()->dataWindow();
    const auto sourceWidth = static_cast<std::int64_t>(sourceWindow.extent().width());
    const auto sourceHeight = static_cast<std::int64_t>(sourceWindow.extent().height());
    const auto size = histogramOutputSize(grade, sourceWidth, sourceHeight);
    const auto outputWindow = render::ImageWindow::create(
        sourceWindow.originX(), sourceWindow.originY(), static_cast<std::uint64_t>(size[0]),
        static_cast<std::uint64_t>(size[1]));
    if (!outputWindow)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Histogram output window is invalid");
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        *outputWindow.value(), *outputWindow.value(), input->descriptor()->pixelAspect());
    if (!descriptor)
        return failed(EvaluationDiagnosticCode::InvalidPixel,
                      "Histogram output descriptor is invalid");
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Histogram output exceeds the pixel budget");
    auto& image = *builder.value();
    const auto channels = static_cast<std::size_t>(channelCount(k));
    const auto bins = static_cast<int>(k[2]);
    const auto groups = histogramGroups(bins);
    const bool straight = flag(k, kFlagStraight);
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
                         const float alpha,
                         std::span<render::Rgba32f> row) -> std::optional<EvaluationDiagnostic> {
        const auto pixel = render::Rgba32f::fromPremultiplied(a, b, c, alpha);
        if (!pixel)
            return invalid("Histogram produced an invalid pixel");
        row[static_cast<std::size_t>(x)] = *pixel.value();
        return std::nullopt;
    };
    const auto rowOf = [&](const std::int64_t y) { return image.row(sourceWindow.originY() + y); };

    // The counts a Sum or a Plot reads: a pixel per group of three bins of each channel.
    std::vector<float> counts;
    std::vector<std::span<const render::Rgba32f>> sourceRows;
    if (stage != HistogramStage::Bin) {
        sourceRows.reserve(static_cast<std::size_t>(sourceHeight));
        for (auto y = sourceWindow.originY(); y < sourceWindow.maxYExclusive(); ++y) {
            const auto row = view.value()->row(y);
            if (!row)
                return failed(EvaluationDiagnosticCode::InternalInvariant,
                              "Histogram row is unavailable");
            sourceRows.push_back(*row.value());
        }
    }

    if (stage == HistogramStage::Sum) {
        auto row = rowOf(0);
        if (!row)
            return failed(EvaluationDiagnosticCode::InternalInvariant,
                          "Histogram row is unavailable");
        for (std::int64_t x = 0; x < size[0]; ++x) {
            if ((x & 255) == 0 && cancellation.isCancellationRequested())
                return ImageEffectResult{{}, {}, true};
            float s0 = 0.0F;
            float s1 = 0.0F;
            float s2 = 0.0F;
            for (const auto& source : sourceRows) {
                const auto& n = source[static_cast<std::size_t>(x)];
                s0 = s0 + n.red();
                s1 = s1 + n.green();
                s2 = s2 + n.blue();
            }
            if (const auto bad = put(x, s0, s1, s2, 1.0F, *row.value()))
                return failed(bad->code, "Histogram produced an invalid count");
        }
    } else {
        if (stage == HistogramStage::Plot) {
            // The bins' counts, channel after channel, and the tallest.
            counts.assign(channels * static_cast<std::size_t>(bins), 0.0F);
            for (std::size_t channel = 0; channel < channels; ++channel)
                for (std::int64_t b = 0; b < bins; ++b) {
                    const auto& n = sourceRows[0][static_cast<std::size_t>(
                        static_cast<std::int64_t>(channel) * groups + b / 3)];
                    counts[channel * static_cast<std::size_t>(bins) + static_cast<std::size_t>(b)] =
                        b % 3 == 0 ? n.red() : (b % 3 == 1 ? n.green() : n.blue());
                }
        }
        float peak = 0.0F;
        for (const float count : counts)
            peak = std::max(peak, count);
        const bool logarithmic = flag(k, kFlagLogarithmic);
        const bool empty = flag(k, kFlagEmpty);
        const auto layout = layoutOf(k, sourceWidth, sourceHeight);
        const auto outcome = runRowBandPass(
            rowBands, cancellation, total, sourceWindow.originY(),
            [&](const std::int64_t absoluteY) -> std::optional<EvaluationDiagnostic> {
                auto row = image.row(absoluteY);
                if (!row)
                    return EvaluationDiagnostic{EvaluationDiagnosticCode::InternalInvariant,
                                                DiagnosticSeverity::Error,
                                                {},
                                                "Histogram row is unavailable",
                                                {}};
                const auto y = absoluteY - sourceWindow.originY();
                if (stage == HistogramStage::Bin) {
                    // This row's tile: count every pixel of it into its bin, once per channel.
                    const auto x0 = (y % layout.grid.width) * layout.grid.tileWidth;
                    const auto y0 = (y / layout.grid.width) * layout.grid.tileHeight;
                    const auto x1 = std::min(x0 + layout.grid.tileWidth, sourceWidth);
                    const auto y1 = std::min(y0 + layout.grid.tileHeight, sourceHeight);
                    std::vector<std::int64_t> tally(
                        channels * static_cast<std::size_t>(groups) * 3, 0);
                    for (auto yy = y0; yy < y1; ++yy) {
                        const auto sourceRow = view.value()->row(sourceWindow.originY() + yy);
                        if (!sourceRow)
                            return invalid("Histogram image is invalid");
                        for (auto xx = x0; xx < x1; ++xx) {
                            const auto& p = (*sourceRow.value())[static_cast<std::size_t>(xx)];
                            const std::array<float, 4> pixel{p.red(), p.green(), p.blue(),
                                                             p.alpha()};
                            for (std::size_t channel = 0; channel < channels; ++channel) {
                                float value = 0.0F;
                                if (empty ||
                                    !channelValue(static_cast<HistogramSource>(
                                                      static_cast<int>(k[kSources + channel])),
                                                  straight, pixel, value))
                                    continue;
                                const int bin = histogramBin(value, k[5], k[6], k[7], bins);
                                if (bin >= 0)
                                    ++tally[channel * static_cast<std::size_t>(groups) * 3 +
                                            static_cast<std::size_t>(bin)];
                            }
                        }
                    }
                    for (std::int64_t x = 0; x < size[0]; ++x) {
                        const auto at = static_cast<std::size_t>(x) * 3;
                        if (const auto bad =
                                put(x, static_cast<float>(tally[at]),
                                    static_cast<float>(tally[at + 1]),
                                    static_cast<float>(tally[at + 2]), 1.0F, *row.value()))
                            return bad;
                    }
                    return std::nullopt;
                }
                // Plot: the bars, from the bottom of the graph.
                const float fromBottom = static_cast<float>(size[1] - 1 - y);
                for (std::int64_t x = 0; x < size[0]; ++x) {
                    if ((x & 255) == 0 && cancellation.isCancellationRequested())
                        return std::nullopt;
                    float r = kBackground;
                    float g = kBackground;
                    float b = kBackground;
                    for (std::size_t channel = 0; channel < channels; ++channel) {
                        const float count = counts[channel * static_cast<std::size_t>(bins) +
                                                   static_cast<std::size_t>(x)];
                        float share = 0.0F;
                        if (peak > 0.0F) {
                            if (logarithmic) {
                                const float above = 1.0F + count;
                                const float top = 1.0F + peak;
                                share = core::specifiedLog2(above) / core::specifiedLog2(top);
                            } else {
                                share = count / peak;
                            }
                        }
                        const float fill = share * static_cast<float>(size[1]);
                        const float over = fill - fromBottom;
                        const float coverage = std::min(std::max(over, 0.0F), 1.0F);
                        const float cr = coverage * k[kColors + 3 * channel];
                        const float cg = coverage * k[kColors + 3 * channel + 1];
                        const float cb = coverage * k[kColors + 3 * channel + 2];
                        r = r + cr;
                        g = g + cg;
                        b = b + cb;
                    }
                    if (const auto bad = put(x, r, g, b, 1.0F, *row.value()))
                        return bad;
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
                                                      "A Histogram row band could not complete",
                                                      {}};
            return result;
        }
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, total, total});
    auto frozen = std::move(image).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Histogram publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

} // namespace bloom::runtime::detail
