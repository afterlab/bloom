#include <bloom/ui/scopes_drawing.hpp>

#include <cstring>
#include <utility>

namespace bloom::ui::scopes {
namespace {

namespace scopes_rt = bloom::runtime::scopes;

[[nodiscard]] scopes_rt::PixelSize pixelsOf(const QSize size) {
    return {size.width(), size.height()};
}

} // namespace

QImage toQImage(const scopes_rt::ScopeImage& image) {
    if (image.empty() || image.pixels.size() != static_cast<std::size_t>(image.width) *
                                                    static_cast<std::size_t>(image.height))
        return {};
    QImage result(image.width, image.height, QImage::Format_ARGB32_Premultiplied);
    const auto rowBytes = static_cast<std::size_t>(image.width) * sizeof(std::uint32_t);
    for (int y = 0; y < image.height; ++y)
        std::memcpy(result.scanLine(y),
                    image.pixels.data() +
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width),
                    rowBytes);
    return result;
}

QImage mapWaveform(const runtime::ScopeTables& tables, const WaveformChannels channels,
                   const TraceColors& colors, const float gain, const QSize size,
                   const Cancelled& cancelled) {
    return toQImage(
        scopes_rt::mapWaveform(tables, channels, colors, gain, pixelsOf(size), cancelled));
}

QImage mapParade(const runtime::ScopeTables& tables, const TraceColors& colors, const float gain,
                 const QSize size, const int gapPixels, const Cancelled& cancelled) {
    return toQImage(
        scopes_rt::mapParade(tables, colors, gain, pixelsOf(size), gapPixels, cancelled));
}

QImage mapVectorscope(const runtime::ScopeTables& tables, const runtime::ScopeLuma& luma,
                      const float gain, const QSize size, const Cancelled& cancelled) {
    return toQImage(scopes_rt::mapVectorscope(tables, luma, gain, pixelsOf(size), cancelled));
}

QImage mapChromaticity(const runtime::ScopeTables& tables, const float gain, const QSize size,
                       const Cancelled& cancelled) {
    return toQImage(scopes_rt::mapChromaticity(tables, gain, pixelsOf(size), cancelled));
}

QImage mapHistogram(const runtime::ScopeTables& tables, const HistogramChannels channels,
                    const HistogramScale scale, const TraceColors& colors, const int rowHeight,
                    const int width, const Cancelled& cancelled) {
    return toQImage(
        scopes_rt::mapHistogram(tables, channels, scale, colors, rowHeight, width, cancelled));
}

QSize chromaticityImageSize(const int side) {
    const auto size = scopes_rt::chromaticityImageSize(side);
    return {size.width, size.height};
}

ClipSummary clipSummary(const runtime::ScopeTables& tables, const Kind kind,
                        const WaveformChannels waveformChannels,
                        const HistogramChannels histogramChannels) {
    return scopes_rt::clipSummary(tables, kind, waveformChannels, histogramChannels);
}

TraceResult renderTrace(const TraceRequest& request, const Cancelled& cancelled) {
    TraceResult result;
    if (request.tables == nullptr || request.tables->empty())
        return result;
    scopes_rt::TraceRequest mapped;
    mapped.kind = request.kind;
    mapped.tables = request.tables.get();
    if (request.color != nullptr)
        mapped.luma = &request.color->luma();
    mapped.colors = request.colors;
    mapped.gain = request.gain;
    mapped.size = pixelsOf(request.size);
    mapped.gapPixels = request.gapPixels;
    mapped.waveformChannels = request.waveformChannels;
    mapped.histogramChannels = request.histogramChannels;
    mapped.histogramScale = request.histogramScale;
    auto traced = scopes_rt::renderTrace(mapped, cancelled);
    result.image = toQImage(traced.image);
    result.clip = std::move(traced.clip);
    return result;
}

} // namespace bloom::ui::scopes
