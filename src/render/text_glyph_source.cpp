#include "text_glyph_source.hpp"

#include <bloom/render/embedded_fonts.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

// The ONE translation unit in this repository that instantiates stb_truetype. Everything in this
// file exists to keep that instantiation here:
//
//   * STBTT_STATIC gives every stb symbol internal linkage, so nothing from the vendored header
//     reaches bloom_render's symbol table, any Bloom public header, or any other translation unit.
//     It is also why the implementation and its only caller must share this file.
//   * STBTT_assert is defined away. Upstream's asserts fire on malformed font data; Bloom's font
//     bytes are a digest-recorded compile-time constant (dependencies/licenses/stb_truetype/
//     security.md), a release build would compile the asserts out anyway, and aborting inside a
//     render task is not an acceptable failure mode for work that must return a diagnostic.
//   * Allocation stays on stb's defaults (malloc/free via <cstdlib>), which is why <cstdlib> and
//     <cstring> are included before the header rather than relying on its own includes.
//
// This file is compiled as the bloom_render_stb_truetype object library: no
// bloom_enable_warnings(), so no -Wall/-Werror and no clang-tidy, plus an explicit -w. The object
// library exists only to scope that suppression -- its objects link straight into bloom_render, and
// the strict floating-point flags the rest of src/render uses are applied to it too so the
// rasterizer's output stays reproducible.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_assert(x) ((void)0)
#include "third_party/stb_truetype/stb_truetype.h"

namespace bloom::render::detail {
namespace {

struct ParsedFont final {
    std::once_flag once;
    stbtt_fontinfo info{};
    bool parsed = false;
};

[[nodiscard]] constexpr std::size_t faceIndex(const EmbeddedFace face) noexcept {
    switch (face) {
    case EmbeddedFace::DejaVuSans:
        return 0;
    case EmbeddedFace::InterRegular:
        return 1;
    case EmbeddedFace::InterMedium:
        return 2;
    case EmbeddedFace::InterSemiBold:
        return 3;
    }
    return 4;
}

// Each registry entry is parsed once, on first use, from its embedded constant. call_once supplies
// the synchronization for the info and parsed flag; after it returns, readers never mutate either.
[[nodiscard]] const ParsedFont* parsedFont(const EmbeddedFace face) noexcept {
    const auto index = faceIndex(face);
    if (index >= 4) {
        return nullptr;
    }
    static std::array<ParsedFont, 4> fonts;
    auto& font = fonts[index];
    std::call_once(font.once, [&font, face] {
        const auto bytes = embeddedFaceBytes(face);
        if (bytes.empty()) {
            return;
        }
        const auto* data = bytes.data();
        const int offset = stbtt_GetFontOffsetForIndex(data, 0);
        if (offset >= 0) {
            font.parsed = stbtt_InitFont(&font.info, data, offset) != 0;
        }
    });
    return &font;
}

} // namespace

struct ParsedExternalFont final {
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    stbtt_fontinfo info{};
    bool parsed = false;
};

namespace {
// The parse when it succeeded, else null.
[[nodiscard]] const ParsedExternalFont* parsedExternalFont(const ExternalFontFace& font) noexcept {
    return font.parsed != nullptr && font.parsed->parsed ? font.parsed.get() : nullptr;
}
} // namespace

ExternalFontFace openExternalFont(const ExternalFontFile& font) noexcept {
    if (font.bytes == nullptr || font.bytes->empty())
        return {};
    try {
        auto parsed = std::make_shared<ParsedExternalFont>();
        parsed->bytes = font.bytes;
        const auto* data = parsed->bytes->data();
        const auto offset = stbtt_GetFontOffsetForIndex(data, static_cast<int>(font.faceIndex));
        if (offset >= 0)
            parsed->parsed = stbtt_InitFont(&parsed->info, data, offset) != 0;
        return {std::move(parsed)};
    } catch (...) {
        return {};
    }
}

bool embeddedFontIsParsed(const EmbeddedFace face) noexcept {
    const auto* font = parsedFont(face);
    return font != nullptr && font->parsed;
}

float embeddedFontScaleForEmPixelSize(const EmbeddedFace face, const double pixelSize) noexcept {
    const auto* font = parsedFont(face);
    if (font == nullptr || !font->parsed || !std::isfinite(pixelSize) || pixelSize <= 0.0) {
        return 0.0F;
    }
    return stbtt_ScaleForMappingEmToPixels(&font->info, static_cast<float>(pixelSize));
}

FontVerticalMetrics embeddedFontVerticalMetrics(const EmbeddedFace face) noexcept {
    const auto* font = parsedFont(face);
    FontVerticalMetrics metrics;
    if (font == nullptr || !font->parsed) {
        return metrics;
    }
    stbtt_GetFontVMetrics(&font->info, &metrics.ascent, &metrics.descent, &metrics.lineGap);
    return metrics;
}

int embeddedFontGlyphIndex(const EmbeddedFace face, const char32_t codepoint) noexcept {
    const auto* font = parsedFont(face);
    if (font == nullptr || !font->parsed) {
        return 0;
    }
    return stbtt_FindGlyphIndex(&font->info, static_cast<int>(codepoint));
}

GlyphHorizontalMetrics embeddedFontGlyphHorizontalMetrics(const EmbeddedFace face,
                                                          const int glyph) noexcept {
    const auto* font = parsedFont(face);
    GlyphHorizontalMetrics metrics;
    if (font == nullptr || !font->parsed) {
        return metrics;
    }
    stbtt_GetGlyphHMetrics(&font->info, glyph, &metrics.advanceWidth, &metrics.leftSideBearing);
    return metrics;
}

int embeddedFontGlyphKernAdvance(const EmbeddedFace face, const int leftGlyph,
                                 const int rightGlyph) noexcept {
    const auto* font = parsedFont(face);
    if (font == nullptr || !font->parsed) {
        return 0;
    }
    return stbtt_GetGlyphKernAdvance(&font->info, leftGlyph, rightGlyph);
}

GlyphBitmapBox embeddedFontGlyphBitmapBox(const EmbeddedFace face, const int glyph,
                                          const float scaleX, const float scaleY,
                                          const float shiftX, const float shiftY) noexcept {
    const auto* font = parsedFont(face);
    GlyphBitmapBox box;
    if (font == nullptr || !font->parsed) {
        return box;
    }
    stbtt_GetGlyphBitmapBoxSubpixel(&font->info, glyph, scaleX, scaleY, shiftX, shiftY, &box.left,
                                    &box.top, &box.right, &box.bottom);
    return box;
}

void embeddedFontRasterizeGlyph(const EmbeddedFace face, const std::span<std::uint8_t> output,
                                const int width, const int height, const int strideBytes,
                                const float scaleX, const float scaleY, const float shiftX,
                                const float shiftY, const int glyph) noexcept {
    const auto* font = parsedFont(face);
    if (font == nullptr || !font->parsed || width <= 0 || height <= 0 || strideBytes < width) {
        return;
    }
    const auto required = static_cast<std::size_t>(strideBytes) * static_cast<std::size_t>(height);
    if (output.size() < required) {
        return;
    }
    stbtt_MakeGlyphBitmapSubpixel(&font->info, output.data(), width, height, strideBytes, scaleX,
                                  scaleY, shiftX, shiftY, glyph);
}

bool externalFontIsParsed(const ExternalFontFace& font) noexcept {
    const auto* parsed = parsedExternalFont(font);
    return parsed != nullptr;
}

float externalFontScaleForEmPixelSize(const ExternalFontFace& font,
                                      const double pixelSize) noexcept {
    const auto* parsed = parsedExternalFont(font);
    if (parsed == nullptr || !std::isfinite(pixelSize) || pixelSize <= 0.0)
        return 0.0F;
    return stbtt_ScaleForMappingEmToPixels(&parsed->info, static_cast<float>(pixelSize));
}

FontVerticalMetrics externalFontVerticalMetrics(const ExternalFontFace& font) noexcept {
    FontVerticalMetrics metrics;
    const auto* parsed = parsedExternalFont(font);
    if (parsed != nullptr)
        stbtt_GetFontVMetrics(&parsed->info, &metrics.ascent, &metrics.descent, &metrics.lineGap);
    return metrics;
}

int externalFontGlyphIndex(const ExternalFontFace& font, const char32_t codepoint) noexcept {
    const auto* parsed = parsedExternalFont(font);
    return parsed != nullptr ? stbtt_FindGlyphIndex(&parsed->info, static_cast<int>(codepoint)) : 0;
}

GlyphHorizontalMetrics externalFontGlyphHorizontalMetrics(const ExternalFontFace& font,
                                                          const int glyph) noexcept {
    GlyphHorizontalMetrics metrics;
    const auto* parsed = parsedExternalFont(font);
    if (parsed != nullptr)
        stbtt_GetGlyphHMetrics(&parsed->info, glyph, &metrics.advanceWidth,
                               &metrics.leftSideBearing);
    return metrics;
}

int externalFontGlyphKernAdvance(const ExternalFontFace& font, const int leftGlyph,
                                 const int rightGlyph) noexcept {
    const auto* parsed = parsedExternalFont(font);
    return parsed != nullptr ? stbtt_GetGlyphKernAdvance(&parsed->info, leftGlyph, rightGlyph) : 0;
}

GlyphBitmapBox externalFontGlyphBitmapBox(const ExternalFontFace& font, const int glyph,
                                          const float scaleX, const float scaleY,
                                          const float shiftX, const float shiftY) noexcept {
    GlyphBitmapBox box;
    const auto* parsed = parsedExternalFont(font);
    if (parsed != nullptr)
        stbtt_GetGlyphBitmapBoxSubpixel(&parsed->info, glyph, scaleX, scaleY, shiftX, shiftY,
                                        &box.left, &box.top, &box.right, &box.bottom);
    return box;
}

void externalFontRasterizeGlyph(const ExternalFontFace& font, const std::span<std::uint8_t> output,
                                const int width, const int height, const int strideBytes,
                                const float scaleX, const float scaleY, const float shiftX,
                                const float shiftY, const int glyph) noexcept {
    const auto* parsed = parsedExternalFont(font);
    if (parsed == nullptr || width <= 0 || height <= 0 || strideBytes < width)
        return;
    const auto required = static_cast<std::size_t>(strideBytes) * static_cast<std::size_t>(height);
    if (output.size() < required)
        return;
    stbtt_MakeGlyphBitmapSubpixel(&parsed->info, output.data(), width, height, strideBytes, scaleX,
                                  scaleY, shiftX, shiftY, glyph);
}

namespace {
std::vector<Path> glyphOutlines(const stbtt_fontinfo& font, int glyph) {
    stbtt_vertex* vertices = nullptr;
    const int count = stbtt_GetGlyphShape(&font, glyph, &vertices);
    const auto release = [&](stbtt_vertex* p) { stbtt_FreeShape(&font, p); };
    std::unique_ptr<stbtt_vertex, decltype(release)> owned(vertices, release);
    std::vector<Path> paths;
    for (int i = 0; i < count; ++i) {
        const auto& v = vertices[i];
        const PathPoint end{double(v.x), double(v.y)};
        if (v.type == STBTT_vmove) {
            paths.push_back({{{end, {}, {}}}, true});
            continue;
        }
        if (paths.empty())
            continue;
        auto& path = paths.back();
        PathAnchor next{end, {}, {}};
        auto& before = path.anchors.back();
        if (v.type == STBTT_vcurve) {
            before.outHandle = PathPoint{before.point.x + (double(v.cx) - before.point.x) * 2 / 3,
                                         before.point.y + (double(v.cy) - before.point.y) * 2 / 3};
            next.inHandle = PathPoint{end.x + (double(v.cx) - end.x) * 2 / 3,
                                      end.y + (double(v.cy) - end.y) * 2 / 3};
        } else if (v.type == STBTT_vcubic) {
            before.outHandle = PathPoint{double(v.cx), double(v.cy)};
            next.inHandle = PathPoint{double(v.cx1), double(v.cy1)};
        }
        path.anchors.push_back(next);
    }
    return paths;
}
} // namespace
std::vector<Path> embeddedFontGlyphOutlines(EmbeddedFace face, int glyph) {
    const auto* font = parsedFont(face);
    return font && font->parsed ? glyphOutlines(font->info, glyph) : std::vector<Path>{};
}
std::vector<Path> externalFontGlyphOutlines(const ExternalFontFace& face, int glyph) {
    const auto* font = parsedExternalFont(face);
    return font ? glyphOutlines(font->info, glyph) : std::vector<Path>{};
}
} // namespace bloom::render::detail
