// #347 Audio Waveform and Audio Spectrum. docs/architecture/media-io.md, "Audio analysis", is the
// specification.
//
// The pixel arithmetic is products, sums, differences, minimums and maximums of binary32 values and
// whole-number divisions, in one fixed order, so the CPU reference and the GPU program agree to the
// bit.

#include "audio_plot_image_effect.hpp"

#include "cpu_composition_evaluator_support.hpp"
#include "image_effect.hpp"

#include <algorithm>
#include <cmath>

namespace bloom::runtime::detail {
namespace {

constexpr std::size_t kFlagSeparate = 1;
constexpr double kMaximumWindowSeconds = 60.0;
constexpr double kMaximumScale = 64.0;

ImageEffectResult failed(const EvaluationDiagnosticCode code, const char* text) {
    ImageEffectResult result;
    result.diagnostic =
        EvaluationDiagnostic{code, DiagnosticSeverity::Error, {}, std::string(text), {}};
    return result;
}

[[nodiscard]] bool finite(const double value) noexcept { return std::isfinite(value); }

[[nodiscard]] float level(const float* const d, const std::size_t at) noexcept {
    return d[kAudioPlotHeaderFloats + at];
}

// One pixel's premultiplied color: the shape's coverage of the pixel over the fill behind it.
struct Pixel final {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 0.0F;
};
[[nodiscard]] Pixel compose(const float* const d, const float coverage) noexcept {
    const float sr = d[6] * coverage;
    const float sg = d[7] * coverage;
    const float sb = d[8] * coverage;
    const float sa = d[9] * coverage;
    const float inverse = 1.0F - sa;
    const float br = d[10] * inverse;
    const float bg = d[11] * inverse;
    const float bb = d[12] * inverse;
    const float ba = d[13] * inverse;
    return {sr + br, sg + bg, sb + bb, sa + ba};
}

[[nodiscard]] float waveformCoverage(const float* const d, const std::int64_t x,
                                     const std::int64_t y) noexcept {
    const auto height = static_cast<std::int64_t>(d[2]);
    const auto lanes = static_cast<std::int64_t>(d[3]);
    const auto lane = (y * lanes) / height;
    const auto top = (lane * height) / lanes;
    const auto bottom = ((lane + 1) * height) / lanes;
    const float half = 0.5F * static_cast<float>(bottom - top);
    const float mid = static_cast<float>(top) + half;
    const auto slot = static_cast<std::size_t>((x * lanes + lane) * 2);
    const float low = level(d, slot);
    const float high = level(d, slot + 1);
    const float scale = d[5];
    const float up = std::min(std::max(high * scale, -1.0F), 1.0F);
    const float down = std::min(std::max(low * scale, -1.0F), 1.0F);
    float edgeTop = mid - up * half;
    float edgeBottom = mid - down * half;
    // A flat trace is one pixel thick.
    if (edgeBottom - edgeTop < 1.0F) {
        const float centre = 0.5F * (edgeTop + edgeBottom);
        edgeTop = centre - 0.5F;
        edgeBottom = centre + 0.5F;
    }
    const float row = static_cast<float>(y);
    const float over = std::min(row + 1.0F, edgeBottom) - std::max(row, edgeTop);
    return std::min(std::max(over, 0.0F), 1.0F);
}

[[nodiscard]] float spectrumCoverage(const float* const d, const std::int64_t x,
                                     const std::int64_t y) noexcept {
    const auto height = static_cast<std::int64_t>(d[2]);
    const float fill = level(d, static_cast<std::size_t>(x)) * static_cast<float>(height);
    const float fromBottom = static_cast<float>(height - 1 - y);
    const float over = fill - fromBottom;
    return std::min(std::max(over, 0.0F), 1.0F);
}

[[nodiscard]] std::size_t analysisFloats(const AudioPlotKernel& kernel, const std::size_t lanes) {
    const auto width = static_cast<std::size_t>(kernel.width);
    return kernel.kind == AudioPlotKind::Waveform ? width * lanes * 2 : width;
}

} // namespace

std::size_t audioPlotScalarCount(const AudioPlotKernel& kernel) noexcept {
    return kernel.kind == AudioPlotKind::Waveform ? 2 : 4;
}

std::optional<AudioAnalysisQuery> audioPlotQuery(const AudioPlotKernel& kernel,
                                                 const std::span<const double> operands,
                                                 const core::RationalTime time) {
    const auto scalars = audioPlotScalarCount(kernel);
    if (operands.size() < scalars + 8 || kernel.width < kAudioPlotMinimumSide ||
        kernel.width > kAudioPlotMaximumSide || kernel.height < kAudioPlotMinimumSide ||
        kernel.height > kAudioPlotMaximumSide)
        return std::nullopt;
    AudioAnalysisQuery query;
    query.source = kernel.source;
    query.time = time;
    query.columns = static_cast<std::uint32_t>(kernel.width);
    if (kernel.kind == AudioPlotKind::Waveform) {
        query.kind = AudioAnalysisKind::Waveform;
        query.windowSeconds = operands[0];
        query.separateChannels = kernel.separate;
        if (!finite(query.windowSeconds) || !(query.windowSeconds > 0.0) ||
            query.windowSeconds > kMaximumWindowSeconds)
            return std::nullopt;
    } else {
        query.kind = AudioAnalysisKind::Spectrum;
        query.spectrum.fftSize = static_cast<std::uint32_t>(kernel.fftSize);
        query.spectrum.logarithmic = kernel.logarithmic;
        query.spectrum.lowHz = operands[0];
        query.spectrum.highHz = operands[1];
        query.spectrum.floorDb = operands[2];
        query.spectrum.ceilingDb = operands[3];
        if (!std::ranges::all_of(operands.first(4), finite) || !(query.spectrum.lowHz > 0.0) ||
            !(query.spectrum.highHz > query.spectrum.lowHz) ||
            query.spectrum.highHz > 192000.0 ||
            !(query.spectrum.ceilingDb > query.spectrum.floorDb) || query.spectrum.floorDb < -200.0 ||
            query.spectrum.ceilingDb > 60.0 ||
            query.spectrum.fftSize < media::audio::analysis::kMinimumFftSize ||
            query.spectrum.fftSize > media::audio::analysis::kMaximumFftSize ||
            (query.spectrum.fftSize & (query.spectrum.fftSize - 1)) != 0)
            return std::nullopt;
    }
    return query;
}

AudioAnalysisStatus prepareAudioPlotAnalysis(const AudioPlotKernel& kernel,
                                             const std::span<const double> operands,
                                             const std::optional<core::RationalTime> time,
                                             const std::filesystem::path& assetBase,
                                             const CancellationToken& cancellation) {
    if (!time)
        return {AudioAnalysisOutcome::Unavailable, "The plot has no time to read the audio at"};
    const auto query = audioPlotQuery(kernel, operands, *time);
    if (!query)
        return {AudioAnalysisOutcome::Unavailable, "The plot values are out of domain"};
    if (!kernel.source)
        return {AudioAnalysisOutcome::Ready, {}};
    return prepareAudioAnalysis(*query, assetBase, cancellation);
}

bool appendAudioPlotAnalysis(const AudioPlotKernel& kernel, std::vector<double>& values,
                             const core::RationalTime time) {
    const auto query = audioPlotQuery(kernel, values, time);
    if (!query)
        return false;
    if (!kernel.source) {
        // No audio wired: silence, one lane.
        const std::size_t lanes = 1;
        values.push_back(static_cast<double>(lanes));
        values.push_back(static_cast<double>(kernel.width));
        values.insert(values.end(), analysisFloats(kernel, lanes), 0.0);
        return true;
    }
    const auto analysis = findAudioAnalysis(*query);
    if (!analysis || analysis->columns != static_cast<std::uint32_t>(kernel.width))
        return false;
    values.push_back(static_cast<double>(analysis->lanes));
    values.push_back(static_cast<double>(analysis->columns));
    for (const float value : analysis->values)
        values.push_back(static_cast<double>(value));
    return true;
}

std::optional<PreparedColorGrade> prepareAudioPlotGrade(const AudioPlotKernel& kernel,
                                                        const std::span<const double> values) {
    const auto scalars = audioPlotScalarCount(kernel);
    if (kernel.width < kAudioPlotMinimumSide || kernel.width > kAudioPlotMaximumSide ||
        kernel.height < kAudioPlotMinimumSide || kernel.height > kAudioPlotMaximumSide ||
        values.size() < scalars + 8 + 2)
        return std::nullopt;
    const auto lanesValue = values[scalars + 8];
    const auto columnsValue = values[scalars + 9];
    if (!(lanesValue >= 1.0) || lanesValue > static_cast<double>(media::audio::analysis::kMaximumLanes) ||
        columnsValue != static_cast<double>(kernel.width))
        return std::nullopt;
    const auto lanes = static_cast<std::size_t>(lanesValue);
    const auto count = analysisFloats(kernel, lanes);
    if (values.size() != scalars + 10 + count)
        return std::nullopt;
    // A lane needs room for its trace.
    if (kernel.kind == AudioPlotKind::Waveform &&
        kernel.height < static_cast<std::int64_t>(lanes) * 8)
        return std::nullopt;
    const std::span<const double> colors = values.subspan(scalars, 8);
    for (const double c : colors)
        if (!finite(c) || c < 0.0 || c > 1.0e6)
            return std::nullopt;
    for (const std::size_t alpha : {std::size_t{3}, std::size_t{7}})
        if (colors[alpha] > 1.0)
            return std::nullopt;
    std::size_t total = kAudioPlotHeaderFloats + count;
    total = (total + 3) / 4 * 4;
    PreparedColorGrade grade{ColorGradeKind::AudioPlot, std::vector<float>(total, 0.0F), false};
    auto& d = grade.data;
    d[0] = kernel.kind == AudioPlotKind::Waveform ? 0.0F : 1.0F;
    d[1] = static_cast<float>(kernel.width);
    d[2] = static_cast<float>(kernel.height);
    d[3] = static_cast<float>(lanes);
    d[4] = kernel.separate ? static_cast<float>(kFlagSeparate) : 0.0F;
    if (kernel.kind == AudioPlotKind::Waveform) {
        if (!finite(values[1]) || values[1] < 0.0 || values[1] > kMaximumScale)
            return std::nullopt;
        d[5] = static_cast<float>(values[1]);
    }
    for (std::size_t i = 0; i < 2; ++i) {
        const double a = colors[i * 4 + 3];
        d[6 + i * 4] = static_cast<float>(colors[i * 4] * a);
        d[7 + i * 4] = static_cast<float>(colors[i * 4 + 1] * a);
        d[8 + i * 4] = static_cast<float>(colors[i * 4 + 2] * a);
        d[9 + i * 4] = static_cast<float>(a);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const double v = values[scalars + 10 + i];
        if (!finite(v) || std::abs(v) > 1.0)
            return std::nullopt;
        d[kAudioPlotHeaderFloats + i] = static_cast<float>(v);
    }
    return grade;
}

bool validAudioPlotProgramData(const std::vector<float>& d) noexcept {
    if (d.size() < kAudioPlotHeaderFloats || d.size() % 4 != 0 ||
        !std::ranges::all_of(d, [](const float v) { return std::isfinite(v); }))
        return false;
    const auto whole = [](const float v, const float low, const float high) {
        return v >= low && v <= high && v == std::floor(v);
    };
    if (!whole(d[0], 0.0F, 1.0F) ||
        !whole(d[1], static_cast<float>(kAudioPlotMinimumSide), static_cast<float>(kAudioPlotMaximumSide)) ||
        !whole(d[2], static_cast<float>(kAudioPlotMinimumSide), static_cast<float>(kAudioPlotMaximumSide)) ||
        !whole(d[3], 1.0F, static_cast<float>(media::audio::analysis::kMaximumLanes)) ||
        !whole(d[4], 0.0F, 1.0F))
        return false;
    const auto width = static_cast<std::size_t>(d[1]);
    const auto count = d[0] == 0.0F ? width * static_cast<std::size_t>(d[3]) * 2 : width;
    return d.size() == (kAudioPlotHeaderFloats + count + 3) / 4 * 4;
}

const std::string& audioPlotProgramText() {
    // The same binary32 operations as applyAudioPlot: precise products and sums, no fused
    // multiply-add, and whole-number divisions of non-negative integers.
    static const std::string text = std::string(R"GLSL(
layout(set = 0, binding = 0, std430) readonly buffer BloomAudioPlotData { vec4 values[]; } bloom_audio_plot_data;
float bap(int i) { return bloom_audio_plot_data.values[i >> 2][i & 3]; }
float bap_waveform(int x, int y) {
 int height = int(bap(2));
 int lanes = int(bap(3));
 int lane = (y * lanes) / height;
 int top = (lane * height) / lanes;
 int bottom = ((lane + 1) * height) / lanes;
 precise float half_height = 0.5 * float(bottom - top);
 precise float mid = float(top) + half_height;
 int slot = (x * lanes + lane) * 2;
 float low = bap(16 + slot);
 float high = bap(16 + slot + 1);
 float scale = bap(5);
 precise float high_scaled = high * scale;
 precise float low_scaled = low * scale;
 float up = min(max(high_scaled, -1.0), 1.0);
 float down = min(max(low_scaled, -1.0), 1.0);
 precise float up_span = up * half_height;
 precise float down_span = down * half_height;
 precise float edge_top = mid - up_span;
 precise float edge_bottom = mid - down_span;
 precise float thickness = edge_bottom - edge_top;
 if (thickness < 1.0) {
  precise float sum = edge_top + edge_bottom;
  precise float centre = 0.5 * sum;
  edge_top = centre - 0.5;
  edge_bottom = centre + 0.5;
 }
 float row = float(y);
 precise float row_end = row + 1.0;
 precise float over = min(row_end, edge_bottom) - max(row, edge_top);
 return min(max(over, 0.0), 1.0);
}
float bap_spectrum(int x, int y) {
 int height = int(bap(2));
 precise float fill = bap(16 + x) * float(height);
 float from_bottom = float(height - 1 - y);
 precise float over = fill - from_bottom;
 return min(max(over, 0.0), 1.0);
}
vec4 bloom_audio_plot(ivec2 c) {
 int width = int(bap(1));
 int height = int(bap(2));
 if (c.x >= width || c.y >= height) { return vec4(0.0); }
 float coverage = (int(bap(0)) == 0) ? bap_waveform(c.x, c.y) : bap_spectrum(c.x, c.y);
 precise float sr = bap(6) * coverage;
 precise float sg = bap(7) * coverage;
 precise float sb = bap(8) * coverage;
 precise float sa = bap(9) * coverage;
 precise float inverse_alpha = 1.0 - sa;
 precise float br = bap(10) * inverse_alpha;
 precise float bg = bap(11) * inverse_alpha;
 precise float bb = bap(12) * inverse_alpha;
 precise float ba = bap(13) * inverse_alpha;
 return vec4(sr + br, sg + bg, sb + bb, sa + ba);
}
)GLSL");
    return text;
}

ImageEffectResult applyAudioPlot(const PreparedColorGrade& grade,
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
    if (grade.kind != ColorGradeKind::AudioPlot || !validAudioPlotProgramData(grade.data))
        return failed(EvaluationDiagnosticCode::InvalidParameter,
                      "Audio plot values are out of domain");
    const float* const d = grade.data.data();
    const auto sourceWindow = input->descriptor()->dataWindow();
    const auto width = static_cast<std::int64_t>(d[1]);
    const auto height = static_cast<std::int64_t>(d[2]);
    if (static_cast<std::int64_t>(sourceWindow.extent().width()) != width ||
        static_cast<std::int64_t>(sourceWindow.extent().height()) != height)
        return failed(EvaluationDiagnosticCode::IncompatibleImageDescriptor,
                      "The audio plot canvas is not the size of the analysis");
    const auto descriptor = render::Rgba32fImageDescriptor::create(
        sourceWindow, input->descriptor()->displayWindow(), input->descriptor()->pixelAspect());
    if (!descriptor)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Audio plot descriptor is invalid");
    auto builder = render::Rgba32fImageBuilder::create(*descriptor.value(), budget);
    if (!builder)
        return failed(EvaluationDiagnosticCode::PixelStorageBudgetExceeded,
                      "Audio plot output exceeds the pixel budget");
    auto& image = *builder.value();
    const bool waveform = d[0] == 0.0F;
    const auto total = static_cast<std::uint32_t>(height);
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, 0, total});
    const auto outcome = runRowBandPass(
        rowBands, cancellation, total, sourceWindow.originY(),
        [&](const std::int64_t absoluteY) -> std::optional<EvaluationDiagnostic> {
            auto row = image.row(absoluteY);
            if (!row)
                return EvaluationDiagnostic{EvaluationDiagnosticCode::InternalInvariant,
                                            DiagnosticSeverity::Error,
                                            {},
                                            "Audio plot row is unavailable",
                                            {}};
            const auto y = absoluteY - sourceWindow.originY();
            for (std::int64_t x = 0; x < width; ++x) {
                const float coverage = waveform ? waveformCoverage(d, x, y) : spectrumCoverage(d, x, y);
                const auto p = compose(d, coverage);
                const auto pixel = render::Rgba32f::fromPremultiplied(p.r, p.g, p.b, p.a);
                if (!pixel)
                    return EvaluationDiagnostic{EvaluationDiagnosticCode::InvalidPixel,
                                                DiagnosticSeverity::Error,
                                                {},
                                                "Audio plot produced an invalid pixel",
                                                {}};
                (*row.value())[static_cast<std::size_t>(x)] = *pixel.value();
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
                                                  "An audio plot row band could not complete",
                                                  {}};
        return result;
    }
    reportProgress(progress, {EvaluationProgressStage::Operation, operation, total, total});
    auto frozen = std::move(image).freeze();
    if (!frozen)
        return failed(EvaluationDiagnosticCode::InvalidPixel, "Audio plot publication failed");
    return {std::make_shared<const render::Rgba32fImage>(std::move(*frozen.value())), {}, false};
}

} // namespace bloom::runtime::detail
