#include <bloom/render/painted_coverage.hpp>

#include <bloom/render/cpu_image_primitives.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace bloom::render {
namespace {

[[nodiscard]] ImageError invalid() noexcept {
    return ImageError::codeOnly(ImageErrorCode::InvalidParameter);
}

// The largest component magnitude of a pixel.
[[nodiscard]] double magnitude(const Rgba32f& pixel) noexcept {
    return std::max({std::abs(static_cast<double>(pixel.red())),
                     std::abs(static_cast<double>(pixel.green())),
                     std::abs(static_cast<double>(pixel.blue())),
                     std::abs(static_cast<double>(pixel.alpha()))});
}

} // namespace

ImageResult<PathPaintedCoverage> paintCoverageLayers(const std::span<const PaintedCoverageLayer> layers) {
    using Result = ImageResult<PathPaintedCoverage>;
    if (layers.empty() || layers.front().geometry == nullptr)
        return Result::failure(invalid());
    const auto& first = *layers.front().geometry;
    const std::size_t ranges = static_cast<std::size_t>(first.height) * 4U;
    std::size_t spans = 0;
    for (const auto& layer : layers) {
        if (layer.geometry == nullptr || layer.geometry->width != first.width ||
            layer.geometry->height != first.height || layer.geometry->rows.size() != ranges)
            return Result::failure(invalid());
        spans += layer.geometry->spans.size();
    }
    constexpr auto kIndexLimit = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    if (spans > kIndexLimit || layers.size() > kIndexLimit / kPaintedCoverageLevels)
        return Result::failure(invalid());

    PathPaintedCoverage painted;
    painted.width = first.width;
    painted.height = first.height;
    painted.rows.resize(ranges);
    painted.spans.reserve(spans);
    painted.palette.reserve(layers.size() * kPaintedCoverageLevels);
    // Each paint's 17 results through the CPU arm itself, and the bound of the fold: every step
    // keeps |d| at most |s| + |d|, so the sum of the paints' largest magnitudes bounds it.
    std::array<std::uint8_t, kPaintedCoverageLevels> samples{};
    for (std::uint32_t count = 0; count < kPaintedCoverageLevels; ++count)
        samples[count] = static_cast<std::uint8_t>((count * 255U + 8U) / 16U);
    std::vector<Rgba32f> results(kPaintedCoverageLevels, Rgba32f::transparent());
    double bound = 0.0;
    for (const auto& layer : layers) {
        if (coverageSolidRow(samples, layer.pixel, results))
            return Result::failure(ImageError::codeOnly(ImageErrorCode::NonFiniteResult));
        double largest = 0.0;
        for (const auto& result : results)
            largest = std::max(largest, magnitude(result));
        bound += largest;
        painted.palette.insert(painted.palette.end(), results.begin(), results.end());
    }
    if (!(bound < static_cast<double>(std::numeric_limits<float>::max()) / 2.0))
        return Result::failure(ImageError::codeOnly(ImageErrorCode::NonFiniteResult));
    for (std::size_t range = 0; range < ranges; ++range) {
        painted.rows[range].offset = static_cast<std::uint32_t>(painted.spans.size());
        for (std::size_t paint = 0; paint < layers.size(); ++paint) {
            const auto& geometry = *layers[paint].geometry;
            const auto& source = geometry.rows[range];
            if (static_cast<std::size_t>(source.offset) + source.count > geometry.spans.size())
                return Result::failure(invalid());
            for (std::uint32_t index = 0; index < source.count; ++index) {
                const auto& span = geometry.spans[source.offset + index];
                painted.spans.push_back({span.first, span.last, static_cast<std::uint32_t>(paint)});
            }
        }
        painted.rows[range].count =
            static_cast<std::uint32_t>(painted.spans.size()) - painted.rows[range].offset;
    }
    return Result::success(std::move(painted));
}

ImageStatus paintedCoverageRow(const PathPaintedCoverage& coverage, const std::uint32_t row,
                               const std::span<Rgba32f> output) {
    if (row >= coverage.height || output.size() != coverage.width ||
        coverage.rows.size() != static_cast<std::size_t>(coverage.height) * 4U)
        return invalid();
    std::vector<std::uint8_t> samples(coverage.width);
    std::vector<Rgba32f> layer(coverage.width, Rgba32f::transparent());
    std::fill(output.begin(), output.end(), Rgba32f::transparent());
    for (std::size_t paint = 0; paint < coverage.paints(); ++paint) {
        // This paint's covered samples per pixel over the row's four sub-scanlines.
        std::vector<std::uint32_t> counts(coverage.width, 0U);
        bool any = false;
        for (std::uint32_t sy = 0; sy < 4U; ++sy) {
            const auto& range = coverage.rows[static_cast<std::size_t>(row) * 4U + sy];
            for (std::uint32_t index = 0; index < range.count; ++index) {
                const auto& span = coverage.spans[range.offset + index];
                if (span.paint != paint || span.last < span.first)
                    continue;
                for (auto sample = span.first; sample <= span.last; ++sample) {
                    const auto pixel = sample / 4U;
                    if (pixel >= coverage.width)
                        break;
                    ++counts[pixel];
                    any = true;
                }
            }
        }
        if (!any)
            continue;
        for (std::uint32_t x = 0; x < coverage.width; ++x)
            layer[x] = coverage.palette[paint * kPaintedCoverageLevels + std::min(counts[x], 16U)];
        if (const auto failure = sourceOverLinearRec709SceneRow(layer, output))
            return failure;
    }
    return std::nullopt;
}

} // namespace bloom::render
