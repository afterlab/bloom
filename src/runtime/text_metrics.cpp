// Text metrics: the reference text layout (the one every render uses), without rasterizing a
// frame.

#include <bloom/render/embedded_fonts.hpp>
#include <bloom/render/text_raster.hpp>
#include <bloom/runtime/text_metrics.hpp>

#include <algorithm>
#include <cmath>

namespace bloom::runtime {

namespace {
// A coverage bitmap larger than this is refused (about 8 million pixels).
constexpr std::size_t kInkByteLimit = std::size_t{8} * 1024 * 1024;
} // namespace

TextMetricsResult measureText(const TextMetricsRequest& request) {
    TextMetricsResult result;
    if (request.font < 0 || request.font > 3) {
        result.diagnostic = "font is a choice from 0 to 3; query kind=enums lists the labels";
        return result;
    }
    if (!(request.size > 0.0) || request.size > render::kMaximumTextPixelSize) {
        result.diagnostic = "size is pixels per em, above 0 and at most 4096";
        return result;
    }
    const auto face = static_cast<render::EmbeddedFace>(request.font);
    const auto parameters = render::TextRasterParameters::create(request.size, request.size);
    if (!parameters) {
        result.diagnostic = "size cannot be used for text";
        return result;
    }
    const render::TextFont font = face;
    // Point text: no box, no wrapping; a newline starts a new line.
    const auto layout = render::layoutText(font, request.text, *parameters.value());
    if (!layout) {
        result.diagnostic = "the text is not well-formed UTF-8 or is too large to lay out";
        return result;
    }
    const auto& value = *layout.value();
    for (const auto& line : value.lines) {
        TextMetricsLine row;
        row.index = line.index;
        row.x = line.box.x;
        row.y = line.box.y;
        row.advance = line.box.width;
        row.height = line.box.height;
        row.byteBegin = line.byteRange.begin;
        row.byteEnd = line.byteRange.end;
        result.advance = std::max(result.advance, row.advance);
        result.lines.push_back(row);
    }
    result.lineHeight = value.caretHeight;
    const auto ink = render::TextCoverageBitmap::rasterizeText(font, request.text, *parameters.value(),
                                                               kInkByteLimit);
    if (!ink) {
        result.diagnostic = "the text is too large to measure its ink";
        return result;
    }
    const auto& bitmap = *ink.value();
    if (bitmap.hasCoverage()) {
        result.hasInk = true;
        result.inkX = bitmap.originX();
        result.inkY = bitmap.originY();
        result.inkWidth = bitmap.width();
        result.inkHeight = bitmap.height();
    }
    result.ok = true;
    return result;
}

} // namespace bloom::runtime
