#include <bloom/output/frame_readback.hpp>

#include <bloom/media/image.hpp>
#include <bloom/render/image.hpp>

namespace bloom::output {

FrameReadback visitExportedFrame(const std::filesystem::path& path, const FrameRegion& region,
                                 const std::function<void(float, float, float, float)>& visit) {
    media::ImageInterpretation raw;
    raw.colorSpace = media::ImageColorSpace::Raw;
    auto decoded = media::decodeImage(path, raw);
    if (!decoded)
        return {.succeeded = false, .diagnostic = decoded.diagnostic, .width = 0, .height = 0};
    const auto& image = **decoded.value;
    const auto* descriptor = image.descriptor();
    if (descriptor == nullptr)
        return {.succeeded = false, .diagnostic = "The frame has no pixels", .width = 0,
                .height = 0};
    const auto width = descriptor->dataWindow().extent().width();
    const auto height = descriptor->dataWindow().extent().height();
    if (region.width == 0 || region.height == 0 || region.x + region.width > width ||
        region.y + region.height > height)
        return {.succeeded = false,
                .diagnostic = "The region is outside the " + std::to_string(width) + " x " +
                              std::to_string(height) + " frame",
                .width = width,
                .height = height};
    const auto pixels = image.pixels();
    for (auto y = region.y; y < region.y + region.height; ++y) {
        for (auto x = region.x; x < region.x + region.width; ++x) {
            const auto& pixel = pixels[y * width + x];
            visit(pixel.red(), pixel.green(), pixel.blue(), pixel.alpha());
        }
    }
    return {.succeeded = true, .diagnostic = {}, .width = width, .height = height};
}

} // namespace bloom::output
