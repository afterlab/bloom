#pragma once

// Interpret Footage (#381) for proxies outside the renderer (the Assets thumbnails): the
// renderer's own CPU reference of the footage interpretation pass (Ignore, Invert, a premultiplied
// matte, deinterlace) and the input color transform after it, so a thumbnail shows what a layer
// renders. Runs on the caller's worker thread, never the interface thread.

#include <bloom/document/asset.hpp>
#include <bloom/media/image.hpp>
#include <bloom/render/image.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <variant>

namespace bloom::color {
class CpuColorSpaceProcessor;
} // namespace bloom::color

namespace bloom::runtime {

// The identity of the pass `asset` needs when read with `association` (its first data row's frame
// row is `firstRow`), for cache keys; empty when an ordinary decode already is the interpretation.
[[nodiscard]] std::string footageProxyPassIdentity(const document::AssetRecord& asset,
                                                   media::ImageAlphaAssociation association,
                                                   std::int64_t firstRow = 0);

// A stored-samples image (media::ImageInterpretation::storedSamples) through the pass and then
// `processor` (null: no input conversion): premultiplied pixels in the working space, as the
// renderer's CPU reference makes them. An empty error string means cancelled.
[[nodiscard]] std::variant<render::Rgba32fImage, std::string>
interpretFootageProxy(const document::AssetRecord& asset, media::ImageAlphaAssociation association,
                      std::int64_t firstRow, const render::Rgba32fImage& stored,
                      const color::CpuColorSpaceProcessor* processor, std::size_t budget,
                      const std::function<bool()>& cancelled);

} // namespace bloom::runtime
