#pragma once

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_frame.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/sha256.hpp>

#include <optional>
#include <span>
#include <string>

namespace bloom::ui::detail {

// Display pixels are straight RGBA8. The qualified OCIO processor owns the working primaries,
// display/view and transfer; a fixed sRGB curve is valid only for one particular working space.
[[nodiscard]] inline bool
packAssetThumbnailRow(const color::PreparedCpuDisplayProcessorHandle& processor,
                      const std::span<const render::Rgba32f> source, const std::span<float> scratch,
                      const std::span<render::Rgba8> destination) {
    return !color::applyBloomNeutralDisplayChunk(processor, source, scratch, destination);
}

// The processor asset (and node card) thumbnails display through: the Viewer's default
// display/view, never the config's own default view (a tone map under ACES CG), so a thumbnail
// looks like the Viewer showing the same frame (issue #409). Absent when no pair is marked or the
// processor cannot be built.
[[nodiscard]] inline std::optional<color::PreparedCpuDisplayProcessorHandle>
buildAssetThumbnailDisplayProcessor(const color::ResolvedBloomNeutralConfig& config) {
    const auto* pair = color::viewerDefaultDisplayView(config.displays());
    if (pair == nullptr)
        return std::nullopt;
    return color::buildCpuDisplayProcessorForView(config, pair->display, pair->view).takeHandle();
}

// The canonical display identity includes config revision, effective working space, view/look,
// output space and packing. Hashing it keeps proxy cache keys bounded without losing any axis.
[[nodiscard]] inline std::optional<std::string>
assetThumbnailDisplayCacheIdentity(const color::PreparedCpuDisplayProcessorHandle& processor) {
    const auto digest = core::Sha256Hasher::hash(processor.identity().canonicalBytes());
    if (!digest)
        return std::nullopt;
    const auto hex = digest->toLowercaseHex();
    return "thumbnail-display-v1:" + std::string(hex.begin(), hex.end());
}

} // namespace bloom::ui::detail
