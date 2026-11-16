#pragma once

#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/render/display_buffer.hpp>
#include <bloom/render/display_pixel_packing.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/view_adjust.hpp>

namespace bloom::runtime::detail {
// `layout` is the layout of the frame this buffer shows (#733); for Rgb10A2 every pixel of
// `source` must allow it (render::rgba32fAllowsRgb10A2).
[[nodiscard]] render::ImageResult<render::PreparedReferenceDisplayBuffer> adjustedQualifiedBuffer(
    const color::PreparedCpuDisplayProcessorHandle& handle, render::Rgba32fImageView source,
    ViewAdjust adjust, std::size_t chunkPixels, std::size_t byteLimit,
    const CancellationToken& cancellation,
    render::DisplayPixelLayout layout = render::DisplayPixelLayout::Straight8888);
}
