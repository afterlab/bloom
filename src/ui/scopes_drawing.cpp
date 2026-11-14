#include <bloom/ui/scopes_drawing.hpp>

#include <cstring>

namespace bloom::ui::scopes {
namespace {

namespace scopes_rt = bloom::runtime::scopes;

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

QImage renderTrace(const TraceRequest& request, const Cancelled& cancelled) {
    if (request.tables == nullptr || request.tables->empty())
        return {};
    scopes_rt::TraceRequest mapped;
    mapped.kind = request.kind;
    mapped.tables = request.tables.get();
    if (request.color != nullptr)
        mapped.luma = &request.color->luma();
    mapped.size = {request.size.width(), request.size.height()};
    mapped.gapPixels = request.gapPixels;
    mapped.waveformChannels = request.waveformChannels;
    mapped.histogramChannels = request.histogramChannels;
    mapped.histogramScale = request.histogramScale;
    return toQImage(scopes_rt::renderTrace(mapped, cancelled));
}

} // namespace bloom::ui::scopes
