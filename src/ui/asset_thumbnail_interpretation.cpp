#include "asset_thumbnail_interpretation.hpp"

#include <bloom/media/video/colour.hpp>
#include <bloom/runtime/footage_proxy.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <variant>

namespace bloom::ui::detail {
namespace {
constexpr double kThumbnailEdge = 64.0;
} // namespace

media::ImageResult<std::shared_ptr<const render::Rgba32fImage>>
decodeInterpretedThumbnail(const document::AssetRecord& asset, const std::filesystem::path& path,
                           const media::ImageInterpretation& interpretation,
                           const core::Sha256Digest& digest, const std::int64_t firstRow,
                           const color::CpuColorSpaceProcessor* processor,
                           const std::function<bool()>& cancel) {
    // The renderer's order: stored samples, the footage pass, then the input color transform.
    auto raw = interpretation;
    raw.colorSpace = media::ImageColorSpace::Raw;
    raw.inputColorSpaceId.clear();
    raw.storedSamples = true;
    auto stored =
        media::decodeImage(path, raw, {}, cancel, {}, media::kMaxImageStorageBytes, digest);
    if (!stored.value.has_value())
        return stored;
    auto interpreted = runtime::interpretFootageProxy(asset, interpretation.alphaAssociation,
                                                      firstRow, **stored.value, processor,
                                                      media::kMaxImageStorageBytes, cancel);
    if (auto* error = std::get_if<std::string>(&interpreted)) {
        const bool cancelled = cancel && cancel();
        return {{}, cancelled ? std::string{} : *error, cancelled};
    }
    return {std::make_shared<const render::Rgba32fImage>(
                std::move(std::get<render::Rgba32fImage>(interpreted))),
            {}};
}

ThumbnailGrid sampleThumbnailGrid(const render::Rgba32fImage& image) {
    ThumbnailGrid grid;
    const auto extent = image.descriptor()->dataWindow().extent();
    if (extent.width() == 0 || extent.height() == 0)
        return grid;
    const auto scale = std::min(1.0, kThumbnailEdge / std::max(extent.width(), extent.height()));
    grid.width =
        std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(extent.width() * scale)));
    grid.height = std::clamp<std::uint32_t>(
        static_cast<std::uint32_t>(
            std::lround(static_cast<double>(extent.height()) * grid.width / extent.width())),
        1, static_cast<std::uint32_t>(kThumbnailEdge));
    grid.pixels.reserve(static_cast<std::size_t>(grid.width) * grid.height);
    for (std::uint32_t y = 0; y < grid.height; ++y)
        for (std::uint32_t x = 0; x < grid.width; ++x) {
            const auto sx = x * extent.width() / grid.width;
            const auto sy = y * extent.height() / grid.height;
            grid.pixels.push_back(
                image.pixels()[static_cast<std::size_t>(sy) * extent.width() + sx]);
        }
    return grid;
}

// #381: a video frame's stored samples through the footage pass and input color at full
// resolution, as the renderer's CPU reference, then nearest-sampled to the thumbnail size.
media::provider::Result<render::Rgba32fImage> interpretedVideoThumbnail(
    const document::AssetRecord& asset, const media::ImageAlphaAssociation association,
    const media::provider::FrameProduct& frame, const std::string& inputId,
    const color::CpuColorSpaceProcessor* processor,
    const render::Rgba32fImageDescriptor& descriptor, const std::function<bool()>& cancelled) {
    auto stored = media::video::videoToStoredSamples(
        frame, static_cast<std::uint32_t>(asset.interpretation.colorSpace),
        processor != nullptr ? std::string_view(inputId) : std::string_view{}, descriptor,
        media::kMaxImageStorageBytes, cancelled);
    if (auto* error = std::get_if<media::provider::Unavailable>(&stored))
        return *error;
    auto interpreted = runtime::interpretFootageProxy(
        asset, association, 0, std::get<render::Rgba32fImage>(stored), processor,
        media::kMaxImageStorageBytes, cancelled);
    if (auto* error = std::get_if<std::string>(&interpreted))
        return media::provider::Unavailable{media::provider::Error::InvalidValue, *error};
    const auto grid = sampleThumbnailGrid(std::get<render::Rgba32fImage>(interpreted));
    const auto invalid = media::provider::Unavailable{media::provider::Error::InvalidValue,
                                                      "The thumbnail size is invalid"};
    const auto window = render::ImageWindow::create(0, 0, grid.width, grid.height);
    if (!window)
        return invalid;
    const auto small = render::Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                              core::PixelAspectRatio::square());
    if (!small)
        return invalid;
    auto builder =
        render::Rgba32fImageBuilder::create(*small.value(), media::kMaxImageStorageBytes);
    if (!builder)
        return media::provider::Unavailable{media::provider::Error::InvalidValue,
                                            "The thumbnail exceeds the pixel budget"};
    for (std::uint32_t y = 0; y < grid.height; ++y) {
        auto row = builder.value()->row(y);
        if (!row)
            return media::provider::Unavailable{media::provider::Error::InvalidValue,
                                                "The thumbnail row is unavailable"};
        for (std::uint32_t x = 0; x < grid.width; ++x)
            (*row.value())[x] = grid.pixels[static_cast<std::size_t>(y) * grid.width + x];
    }
    auto frozen = std::move(*builder.value()).freeze();
    if (!frozen)
        return media::provider::Unavailable{media::provider::Error::InvalidValue,
                                            "The thumbnail is invalid"};
    return std::move(*frozen.value());
}
double thumbnailPixelAspectScale(const document::AssetRecord& asset) {
    const auto shown = document::footagePixelAspect(asset);
    const auto decoded = document::footageFilePixelAspect(asset);
    return (static_cast<double>(shown.numerator()) / shown.denominator()) /
           (static_cast<double>(decoded.numerator()) / decoded.denominator());
}

QImage withThumbnailPixelAspect(const QImage& image, const double scale) {
    if (image.isNull() || !std::isfinite(scale) || scale <= 0.0 || std::abs(scale - 1.0) < 1e-6)
        return image;
    double width = image.width() * scale;
    double height = image.height();
    // Kept within the 64 px edge by shrinking both sides, so the shape is the interpreted one.
    if (const auto longest = std::max(width, height); longest > kThumbnailEdge) {
        width *= kThumbnailEdge / longest;
        height *= kThumbnailEdge / longest;
    }
    return image.scaled(std::max(1, static_cast<int>(std::lround(width))),
                        std::max(1, static_cast<int>(std::lround(height))), Qt::IgnoreAspectRatio,
                        Qt::SmoothTransformation);
}

} // namespace bloom::ui::detail
