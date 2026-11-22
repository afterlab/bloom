#pragma once

// #758: PaintedCoverageV1, many coverage layers drawn in one pass. A frame whose layers differ only
// in paint (an animated text's per-cluster fill and outline colors and opacities) resolves each
// layer's PathRasterCoverageGeometry as usual and concatenates them, per sub-scanline and in drawing
// order, with every span tagged by its layer's paint. Each paint carries its 17 possible pixels, one
// per number of covered samples of the 4 x 4 grid, built with coverageSolidRow(). The result is the
// CPU reference's per-layer fold (coverage row, coverageSolidRow, source-over, layer after layer):
// GpuSolid::beginPainted draws it in one dispatch, and paintedCoverageRow() is its CPU oracle. The
// pass count is one, however many paints the layers have.

#include <bloom/render/image_types.hpp>
#include <bloom/render/path_raster.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bloom::render {

// The results a paint can produce: 0 to 16 covered samples.
inline constexpr std::uint32_t kPaintedCoverageLevels = 17;

struct PathPaintedCoverageSpan final {
    std::uint32_t first = 0; // inclusive sample index within the row
    std::uint32_t last = 0;  // inclusive sample index within the row
    std::uint32_t paint = 0;
    friend bool operator==(const PathPaintedCoverageSpan&, const PathPaintedCoverageSpan&) = default;
};

struct PathPaintedCoverage final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Exactly height * 4 entries, the row-major (row * 4 + sy) sub-scanline range. Within one range
    // the spans ascend by paint.
    std::vector<PathRasterCoverageRange> rows;
    std::vector<PathPaintedCoverageSpan> spans;
    // paints * kPaintedCoverageLevels premultiplied pixels: palette[paint * 17 + n] is what
    // coverageSolidRow() gives the paint's pixel at the coverage byte (n * 255 + 8) / 16.
    std::vector<Rgba32f> palette;

    [[nodiscard]] std::size_t paints() const noexcept {
        return palette.size() / kPaintedCoverageLevels;
    }
    // The host bytes the coverage retains (its rows, spans and palette).
    [[nodiscard]] std::uint64_t bytes() const noexcept {
        return static_cast<std::uint64_t>(rows.size()) * sizeof(PathRasterCoverageRange) +
               static_cast<std::uint64_t>(spans.size()) * sizeof(PathPaintedCoverageSpan) +
               static_cast<std::uint64_t>(palette.size()) * sizeof(Rgba32f);
    }
};

// One layer in drawing order: its coverage over the shared window and its premultiplied pixel.
struct PaintedCoverageLayer final {
    const PathRasterCoverageGeometry* geometry = nullptr;
    Rgba32f pixel = Rgba32f::transparent();
};

// Concatenates the layers (paint i is layer i). InvalidParameter when there is no layer, a layer
// has no geometry, the geometries differ in size, or an index would pass 32 bits; NonFiniteResult
// when a palette pixel is not representable or the summed magnitude of the paints could overflow
// the fold.
[[nodiscard]] ImageResult<PathPaintedCoverage>
paintCoverageLayers(std::span<const PaintedCoverageLayer> layers);

// The CPU oracle of PaintedCoverageV1 for one row: `output` (exactly width pixels) receives the
// layers folded source-over in order, as sourceOverLinearRec709SceneRow() folds them.
[[nodiscard]] ImageStatus paintedCoverageRow(const PathPaintedCoverage& coverage, std::uint32_t row,
                                             std::span<Rgba32f> output);

} // namespace bloom::render
