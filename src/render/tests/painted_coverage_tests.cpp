// #758: PaintedCoverageV1's host builder and CPU oracle (render/painted_coverage.hpp). No device.
//
// Overlapping fills and a stroke in different premultiplied pixels (opaque, translucent and HDR)
// are concatenated into one painted coverage. Every row must equal, bit for bit:
// - the CPU reference's per-layer fold (coverageRow, coverageSolidRow, then
//   sourceOverLinearRec709SceneRow, layer after layer), through paintedCoverageRow();
// - an emulation of solid_painted.comp's own walk (the four sub-scanline cursors merged by
//   ascending paint, the integer sample overlap, the palette lookup and source_over.comp's endpoints
//   and fma), so the shader's algorithm is the reference's.
// The builder refuses an empty layer list, a missing geometry and mismatched windows.

#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/painted_coverage.hpp>
#include <bloom/render/path_raster.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace bloom::render;

namespace {

int failures = 0;

void expect(const bool ok, const char* message) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] Rgba32f pixel(const float r, const float g, const float b, const float a) {
    const auto made = Rgba32f::fromPremultiplied(r, g, b, a);
    if (!made)
        throw std::runtime_error("an invalid test pixel");
    return *made.value();
}

[[nodiscard]] bool same(const Rgba32f& a, const Rgba32f& b) {
    return std::bit_cast<std::uint32_t>(a.red()) == std::bit_cast<std::uint32_t>(b.red()) &&
           std::bit_cast<std::uint32_t>(a.green()) == std::bit_cast<std::uint32_t>(b.green()) &&
           std::bit_cast<std::uint32_t>(a.blue()) == std::bit_cast<std::uint32_t>(b.blue()) &&
           std::bit_cast<std::uint32_t>(a.alpha()) == std::bit_cast<std::uint32_t>(b.alpha());
}

struct Layer final {
    PathRaster raster;
    bool stroke = false;
    Rgba32f color;
};

// solid_painted.comp, line for line, for one pixel.
[[nodiscard]] std::array<float, 4> shaderPixel(const PathPaintedCoverage& coverage,
                                               const std::uint32_t x, const std::uint32_t y) {
    const std::uint32_t lo = x * 4U;
    const std::uint32_t hi = lo + 3U;
    std::array<std::uint32_t, 4> cursor{};
    std::array<std::uint32_t, 4> end{};
    for (std::uint32_t sy = 0; sy < 4U; ++sy) {
        const auto& range = coverage.rows[y * 4U + sy];
        cursor[sy] = range.offset;
        end[sy] = range.offset + range.count;
    }
    std::array<float, 4> destination{0.0F, 0.0F, 0.0F, 0.0F};
    for (;;) {
        std::uint32_t paint = 0xFFFFFFFFU;
        for (std::uint32_t sy = 0; sy < 4U; ++sy)
            if (cursor[sy] < end[sy])
                paint = std::min(paint, coverage.spans[cursor[sy]].paint);
        if (paint == 0xFFFFFFFFU)
            break;
        std::uint32_t count = 0;
        for (std::uint32_t sy = 0; sy < 4U; ++sy)
            while (cursor[sy] < end[sy] && coverage.spans[cursor[sy]].paint == paint) {
                const auto& span = coverage.spans[cursor[sy]];
                const auto first = std::max(span.first, lo);
                const auto last = std::min(span.last, hi);
                if (first <= last)
                    count += last - first + 1U;
                ++cursor[sy];
            }
        const auto& source = coverage.palette[paint * kPaintedCoverageLevels + std::min(count, 16U)];
        const std::array<float, 4> s{source.red(), source.green(), source.blue(), source.alpha()};
        if (s[3] == 0.0F)
            continue;
        if (s[3] == 1.0F || destination[3] == 0.0F) {
            destination = s;
            continue;
        }
        const float inverse = 1.0F - s[3];
        for (std::size_t c = 0; c < 4; ++c)
            destination[c] = std::fma(inverse, destination[c], s[c]);
    }
    return destination;
}

void foldTests() {
    // A 37 x 23 window at a fractional offset over overlapping shapes.
    constexpr std::int64_t originX = -3;
    constexpr std::int64_t originY = 2;
    constexpr std::uint32_t width = 37;
    constexpr std::uint32_t height = 23;
    std::vector<Layer> layers;
    const auto add = [&](const Path& path, const PathMatrix matrix, const PathStroke stroke,
                         const Rgba32f color) {
        auto raster = PathRaster::transformed(std::span<const Path>(&path, 1), stroke, matrix,
                                              1.0, 1.0);
        expect(static_cast<bool>(raster), "a test raster builds");
        if (raster)
            layers.push_back({std::move(*raster.value()), stroke.width > 0.0, color});
    };
    add(rectanglePath(18.3, 11.7), {1, 0, 0, 1, 0.4, 3.2}, {}, pixel(0.9F, 0.1F, 0.1F, 1.0F));
    add(ellipsePath(20.0, 16.0), {1, 0, 0, 1, 6.25, 5.5}, {}, pixel(0.05F, 0.2F, 0.3F, 0.5F));
    add(rectanglePath(9.0, 14.0), {0.8, 0.6, -0.6, 0.8, 22.0, 2.0}, {},
        pixel(2.5F, 1.5F, 0.25F, 0.75F));
    add(ellipsePath(12.0, 12.0), {1, 0, 0, 1, 1.0, 9.0}, {.width = 1.75},
        pixel(0.0F, 0.6F, 0.0F, 0.6F));
    add(rectanglePath(30.0, 2.5), {1, 0, 0, 1, -1.0, 12.125}, {}, pixel(0.0F, 0.0F, 0.0F, 0.0F));
    std::vector<PathRasterCoverageGeometry> geometries;
    for (const auto& layer : layers) {
        auto geometry = layer.raster.coverageGeometry(originX, originY, width, height,
                                                      PathFillRule::NonZero, layer.stroke);
        expect(static_cast<bool>(geometry), "a layer's coverage geometry builds");
        if (!geometry)
            return;
        geometries.push_back(std::move(*geometry.value()));
    }
    std::vector<PaintedCoverageLayer> painted;
    for (std::size_t index = 0; index < layers.size(); ++index)
        painted.push_back({&geometries[index], layers[index].color});
    const auto built = paintCoverageLayers(painted);
    expect(static_cast<bool>(built), "the painted coverage builds");
    if (!built)
        return;
    const auto& coverage = *built.value();
    expect(coverage.paints() == layers.size() && coverage.width == width &&
               coverage.height == height && coverage.rows.size() == height * 4U,
           "one paint per layer over the window");
    std::size_t covered = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        // The CPU reference's fold.
        std::vector<Rgba32f> reference(width, Rgba32f::transparent());
        std::vector<std::uint8_t> samples(width);
        std::vector<Rgba32f> layerRow(width, Rgba32f::transparent());
        for (const auto& layer : layers) {
            expect(layer.raster.coverageRow(originX, originY + y, samples, PathFillRule::NonZero,
                                            layer.stroke),
                   "a reference coverage row");
            expect(!coverageSolidRow(samples, layer.color, layerRow), "a reference fill row");
            expect(!sourceOverLinearRec709SceneRow(layerRow, reference), "a reference fold row");
        }
        std::vector<Rgba32f> oracle(width, Rgba32f::transparent());
        expect(!paintedCoverageRow(coverage, y, oracle), "the painted oracle row");
        for (std::uint32_t x = 0; x < width; ++x) {
            if (reference[x].alpha() > 0.0F)
                ++covered;
            if (!same(oracle[x], reference[x])) {
                std::cerr << "oracle differs at " << x << ',' << y << '\n';
                ++failures;
            }
            const auto shader = shaderPixel(coverage, x, y);
            const auto emulated = Rgba32f::fromPremultiplied(shader[0], shader[1], shader[2],
                                                             shader[3]);
            if (!emulated || !same(*emulated.value(), reference[x])) {
                std::cerr << "shader walk differs at " << x << ',' << y << '\n';
                ++failures;
            }
        }
    }
    expect(covered > 300, "the shapes cover the window");
}

void refusalTests() {
    expect(!paintCoverageLayers({}), "no layer is refused");
    const std::array<PaintedCoverageLayer, 1> missing{{{nullptr, Rgba32f::transparent()}}};
    expect(!paintCoverageLayers(missing), "a layer without geometry is refused");
    const auto raster = PathRaster::create(rectanglePath(4, 4), {}, 1, 1);
    expect(static_cast<bool>(raster), "a refusal raster builds");
    if (!raster)
        return;
    const auto a = raster.value()->coverageGeometry(0, 0, 8, 8, PathFillRule::NonZero, false);
    const auto b = raster.value()->coverageGeometry(0, 0, 9, 8, PathFillRule::NonZero, false);
    expect(a && b, "refusal geometries build");
    if (!a || !b)
        return;
    const std::array<PaintedCoverageLayer, 2> mismatched{
        {{&*a.value(), Rgba32f::transparent()}, {&*b.value(), Rgba32f::transparent()}}};
    expect(!paintCoverageLayers(mismatched), "geometries over different windows are refused");
}

} // namespace

int main() {
    try {
        foldTests();
        refusalTests();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected test exception: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " painted coverage expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: painted coverage folds its layers as the CPU reference does\n";
    return 0;
}
