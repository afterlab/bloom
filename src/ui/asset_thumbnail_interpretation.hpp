#pragma once

// Interpret Footage (#381) in the Assets thumbnails: alpha (Ignore, Invert, a premultiplied matte)
// through the renderer's own CPU footage pass, and the pixel aspect ratio as the thumbnail's
// shape. Worker-only: every function here runs on the thumbnail worker.

#include <QImage>
#include <bloom/document/asset.hpp>
#include <bloom/media/image.hpp>
#include <bloom/media/provider/contract.hpp>
#include <bloom/render/image.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bloom::color {
class CpuColorSpaceProcessor;
} // namespace bloom::color

namespace bloom::ui::detail {

// A still or sequence frame decoded as its stored samples and interpreted by the footage pass,
// then `processor` (null: no conversion): the image the renderer's CPU reference makes.
[[nodiscard]] media::ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeInterpretedThumbnail(const document::AssetRecord& asset,
                           const media::ImageSourceRevision& revision,
                           const media::ImageInterpretation& interpretation, std::int64_t firstRow,
                           const color::CpuColorSpaceProcessor* processor,
                           const std::function<bool()>& cancel);

// `image` nearest-sampled to at most 64 px on its long side, row-major.
struct ThumbnailGrid final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<render::Rgba32f> pixels;
};
[[nodiscard]] ThumbnailGrid sampleThumbnailGrid(const render::Rgba32fImage& image);

// A video frame's stored samples through the footage pass and `processor` at full resolution, as
// the renderer's CPU reference, nearest-sampled to the thumbnail size.
[[nodiscard]] media::provider::Result<render::Rgba32fImage> interpretedVideoThumbnail(
    const document::AssetRecord& asset, media::ImageAlphaAssociation association,
    const media::provider::FrameProduct& frame, const std::string& inputId,
    const color::CpuColorSpaceProcessor* processor,
    const render::Rgba32fImageDescriptor& descriptor, const std::function<bool()>& cancelled);

// How much wider than its decoded picture the asset's thumbnail is drawn: the Interpret Footage
// pixel aspect ratio over the one the decode already applied (a video's coded ratio).
[[nodiscard]] double thumbnailPixelAspectScale(const document::AssetRecord& asset);
// `image` drawn `scale` times as wide, within 64 px on its long side.
[[nodiscard]] QImage withThumbnailPixelAspect(const QImage& image, double scale);

} // namespace bloom::ui::detail
