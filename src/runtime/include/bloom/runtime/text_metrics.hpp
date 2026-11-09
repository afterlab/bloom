#pragma once

// Text metrics: the reference text layout (the one every render uses), without rasterizing a
// frame (#390). Qt-free; heavy enough to belong on a client thread, never the UI thread.

#include <bloom/core/sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bloom::runtime {

struct TextMetricsRequest final {
    std::string text;
    // The text layer's font choice: 0 DejaVu Sans, 1 Inter Regular, 2 Inter Medium, 3 Inter
    // SemiBold (the `bloom.text.font` values).
    std::int64_t font = 0;
    // A resolved font file to measure instead of `font`; null measures the built-in face.
    std::shared_ptr<const std::vector<std::uint8_t>> fontBytes;
    core::Sha256Digest fontDigest{};
    std::uint32_t fontFaceIndex = 0;
    // Pixels per em.
    double size = 0.0;
};

struct TextMetricsLine final {
    std::size_t index = 0;
    // The line's box in layout pixels: x to the right of the text origin (the left edge of the
    // first glyph's advance), y down from the top of the first line.
    double x = 0.0;
    double y = 0.0;
    double advance = 0.0;
    double height = 0.0;
    // Half-open UTF-8 byte range of the text on this line.
    std::size_t byteBegin = 0;
    std::size_t byteEnd = 0;
};

struct TextMetricsResult final {
    bool ok = false;
    // When !ok: why (invalid UTF-8, a size out of range, a too-large block).
    std::string diagnostic;
    // The widest line's advance, in layout pixels, with kerning.
    double advance = 0.0;
    // The height of one line (the caret height).
    double lineHeight = 0.0;
    std::vector<TextMetricsLine> lines;
    // The union of every glyph's ink, in whole pixels relative to the text origin; absent for
    // text without ink (empty, or only spaces).
    bool hasInk = false;
    std::int64_t inkX = 0;
    std::int64_t inkY = 0;
    std::uint32_t inkWidth = 0;
    std::uint32_t inkHeight = 0;
};

[[nodiscard]] TextMetricsResult measureText(const TextMetricsRequest& request);

} // namespace bloom::runtime
