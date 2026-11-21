#pragma once

// Private to src/render. The glyph access the Text layout (text_raster.cpp) and the styled layout
// (styled_text.cpp, #752) share, over an embedded face or a parsed external font: one place that
// reads glyph indices, metrics, kerning and outlines, so both layouts measure text identically.

#include "text_glyph_source.hpp"

#include <bloom/render/embedded_fonts.hpp>

#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace bloom::render::font_access {

using bloom::render::EmbeddedFace;
namespace detail = bloom::render::detail;

template <typename Face> [[nodiscard]] bool fontIsParsed(const Face& face) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontIsParsed(face);
    else
        return detail::externalFontIsParsed(face);
}
template <typename Face>
[[nodiscard]] float fontScaleForEmPixelSize(const Face& face, const double size) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontScaleForEmPixelSize(face, size);
    else
        return detail::externalFontScaleForEmPixelSize(face, size);
}
template <typename Face> [[nodiscard]] auto fontVerticalMetrics(const Face& face) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontVerticalMetrics(face);
    else
        return detail::externalFontVerticalMetrics(face);
}
template <typename Face>
[[nodiscard]] int fontGlyphIndex(const Face& face, const char32_t scalar) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontGlyphIndex(face, scalar);
    else
        return detail::externalFontGlyphIndex(face, scalar);
}
template <typename Face>
[[nodiscard]] int fontGlyphKernAdvance(const Face& face, const int left, const int right) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontGlyphKernAdvance(face, left, right);
    else
        return detail::externalFontGlyphKernAdvance(face, left, right);
}
template <typename Face>
[[nodiscard]] auto fontGlyphHorizontalMetrics(const Face& face, const int glyph) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontGlyphHorizontalMetrics(face, glyph);
    else
        return detail::externalFontGlyphHorizontalMetrics(face, glyph);
}
template <typename Face>
[[nodiscard]] auto fontGlyphBitmapBox(const Face& face, const int glyph, const float scaleX,
                                      const float scaleY, const float shiftX,
                                      const float shiftY) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontGlyphBitmapBox(face, glyph, scaleX, scaleY, shiftX, shiftY);
    else
        return detail::externalFontGlyphBitmapBox(face, glyph, scaleX, scaleY, shiftX, shiftY);
}
template <typename Face>
void fontRasterizeGlyph(const Face& face, const std::span<std::uint8_t> output, const int width,
                        const int height, const int stride, const float scaleX, const float scaleY,
                        const float shiftX, const float shiftY, const int glyph) noexcept {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        detail::embeddedFontRasterizeGlyph(face, output, width, height, stride, scaleX, scaleY,
                                           shiftX, shiftY, glyph);
    else
        detail::externalFontRasterizeGlyph(face, output, width, height, stride, scaleX, scaleY,
                                           shiftX, shiftY, glyph);
}


template <typename Face>
[[nodiscard]] std::vector<Path> fontGlyphOutlines(const Face& face, const int glyph) {
    if constexpr (std::is_same_v<Face, EmbeddedFace>)
        return detail::embeddedFontGlyphOutlines(face, glyph);
    else
        return detail::externalFontGlyphOutlines(face, glyph);
}

} // namespace bloom::render::font_access
