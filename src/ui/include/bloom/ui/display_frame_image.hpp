#pragma once

// #733: a packed display frame as a QImage. A cached display frame is four bytes a pixel in one of
// two layouts (render/display_pixel_packing.hpp): straight RGBA8, or 10:10:10:2 words for a frame
// whose every pixel is opaque or exactly transparent. Every reader in the interface that hands a
// frame to Qt goes through this header, so no reader assumes the 8-bit layout.

#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/render/image_types.hpp>

#include <QImage>

#include <span>

namespace bloom::ui {

// The QImage format whose pixels are the frame's cells as they are stored. A 10:10:10:2 word (red
// in the low ten bits, alpha in the top two) is Qt's A2BGR30. Every pixel of such a frame is
// opaque or the word 0, so its straight color is also its premultiplied color.
[[nodiscard]] constexpr QImage::Format
displayFrameImageFormat(const render::DisplayPixelLayout layout) noexcept {
    return layout == render::DisplayPixelLayout::Rgb10A2 ? QImage::Format_A2BGR30_Premultiplied
                                                         : QImage::Format_RGBA8888;
}

// A QImage over the frame's cells without a copy; the caller keeps the cells alive while the image
// is used. Drawn at its own size on an 8-bit target, a 10:10:10:2 pixel shows
// render::presentedCode8 of each code (bloom.ui.viewer-presentation-depth holds Qt to that for
// every code); a deeper target keeps the codes; a smooth-scaled draw interpolates and stays within
// one 8-bit code of the rule.
[[nodiscard]] inline QImage borrowDisplayFrameImage(const std::span<const render::Rgba8> cells,
                                                    const int width, const int height,
                                                    const qsizetype rowStrideBytes,
                                                    const render::DisplayPixelLayout layout) {
    return QImage(reinterpret_cast<const uchar*>(cells.data()), width, height, rowStrideBytes,
                  displayFrameImageFormat(layout));
}

// An owned straight RGBA8 image of the frame: the pixels an 8-bit output shows
// (render::presentedRgba8). It copies or converts the whole frame, so it is for worker threads.
[[nodiscard]] inline QImage copyDisplayFrameImage8(const std::span<const render::Rgba8> cells,
                                                   const int width, const int height,
                                                   const qsizetype rowStrideBytes,
                                                   const render::DisplayPixelLayout layout) {
    const QImage borrowed = borrowDisplayFrameImage(cells, width, height, rowStrideBytes, layout);
    return layout == render::DisplayPixelLayout::Rgb10A2
               ? borrowed.convertToFormat(QImage::Format_RGBA8888)
               : borrowed.copy();
}

} // namespace bloom::ui
