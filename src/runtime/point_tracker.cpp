#include "point_tracker.hpp"

#include <algorithm>
#include <cmath>

namespace bloom::runtime {
namespace {

[[nodiscard]] float at(const TrackLuma& grid, const std::int64_t x, const std::int64_t y) {
    return grid.values[static_cast<std::size_t>(y) * grid.width + static_cast<std::size_t>(x)];
}

[[nodiscard]] bool valid(const TrackLuma& grid) {
    return grid.width > 0 && grid.height > 0 &&
           grid.values.size() == static_cast<std::size_t>(grid.width) * grid.height;
}

// The pattern of the square of half side `r` whose centre pixel is (cx, cy), with its mean removed
// and its energy. False when the square leaves the grid or has no variation to follow.
struct Pattern final {
    std::vector<float> values;
    double energy = 0.0;
};

[[nodiscard]] bool takePattern(const TrackLuma& grid, const std::int64_t cx, const std::int64_t cy,
                               const int r, Pattern& out) {
    if (cx - r < 0 || cy - r < 0 || cx + r >= static_cast<std::int64_t>(grid.width) ||
        cy + r >= static_cast<std::int64_t>(grid.height))
        return false;
    const auto side = static_cast<std::size_t>(2 * r + 1);
    out.values.assign(side * side, 0.0F);
    double mean = 0.0;
    for (int j = -r; j <= r; ++j)
        for (int i = -r; i <= r; ++i)
            mean += static_cast<double>(at(grid, cx + i, cy + j));
    mean /= static_cast<double>(side * side);
    double energy = 0.0;
    std::size_t k = 0;
    for (int j = -r; j <= r; ++j)
        for (int i = -r; i <= r; ++i) {
            const auto v = static_cast<double>(at(grid, cx + i, cy + j)) - mean;
            out.values[k++] = static_cast<float>(v);
            energy += v * v;
        }
    out.energy = energy;
    return energy > 1.0e-12;
}

// The normalised correlation of `pattern` against the square centred on (cx, cy) of `grid`.
[[nodiscard]] double correlate(const TrackLuma& grid, const Pattern& pattern, const std::int64_t cx,
                               const std::int64_t cy, const int r) {
    Pattern candidate;
    if (!takePattern(grid, cx, cy, r, candidate))
        return -2.0;
    double dot = 0.0;
    for (std::size_t k = 0; k < pattern.values.size(); ++k)
        dot += static_cast<double>(pattern.values[k]) * static_cast<double>(candidate.values[k]);
    return dot / std::sqrt(pattern.energy * candidate.energy);
}

// `grid` read bilinearly at (x, y), where pixel (i, j) has its centre at (i, j).
[[nodiscard]] double bilinear(const TrackLuma& grid, const double x, const double y) {
    const auto x0 = static_cast<std::int64_t>(std::floor(x));
    const auto y0 = static_cast<std::int64_t>(std::floor(y));
    const auto fx = x - static_cast<double>(x0);
    const auto fy = y - static_cast<double>(y0);
    const auto get = [&](const std::int64_t i, const std::int64_t j) {
        return static_cast<double>(at(grid, std::clamp<std::int64_t>(i, 0, grid.width - 1),
                                      std::clamp<std::int64_t>(j, 0, grid.height - 1)));
    };
    return (get(x0, y0) * (1.0 - fx) + get(x0 + 1, y0) * fx) * (1.0 - fy) +
           (get(x0, y0 + 1) * (1.0 - fx) + get(x0 + 1, y0 + 1) * fx) * fy;
}

// Refines the whole-pixel move (`dx`, `dy`) of `pattern` (centred on pixel (cx, cy) of the previous
// frame) to a fraction of a pixel: a few Gauss-Newton steps of the translation that best lays the
// pattern over the current frame, means removed so a change of brightness does not move the match.
// Identical patches stay exactly where they are. Unchanged when the system is ill conditioned or the
// step runs more than a pixel from the whole-pixel match.
void refine(const TrackLuma& current, const Pattern& pattern, const std::int64_t cx,
            const std::int64_t cy, const int r, double& dx, double& dy) {
    const double startX = dx;
    const double startY = dy;
    const auto side = static_cast<std::size_t>(2 * r + 1);
    std::vector<double> values(side * side);
    std::vector<double> gx(side * side);
    std::vector<double> gy(side * side);
    for (int iteration = 0; iteration < 8; ++iteration) {
        double mean = 0.0;
        std::size_t k = 0;
        for (int j = -r; j <= r; ++j)
            for (int i = -r; i <= r; ++i) {
                const auto x = static_cast<double>(cx + i) + dx;
                const auto y = static_cast<double>(cy + j) + dy;
                values[k] = bilinear(current, x, y);
                gx[k] = 0.5 * (bilinear(current, x + 1.0, y) - bilinear(current, x - 1.0, y));
                gy[k] = 0.5 * (bilinear(current, x, y + 1.0) - bilinear(current, x, y - 1.0));
                mean += values[k++];
            }
        mean /= static_cast<double>(side * side);
        double a = 0.0;
        double b = 0.0;
        double c = 0.0;
        double ex = 0.0;
        double ey = 0.0;
        for (std::size_t q = 0; q < values.size(); ++q) {
            const auto error = static_cast<double>(pattern.values[q]) - (values[q] - mean);
            a += gx[q] * gx[q];
            b += gx[q] * gy[q];
            c += gy[q] * gy[q];
            ex += gx[q] * error;
            ey += gy[q] * error;
        }
        const auto det = a * c - b * b;
        if (!(det > 1.0e-9 * static_cast<double>(values.size())))
            break;
        const auto stepX = (c * ex - b * ey) / det;
        const auto stepY = (a * ey - b * ex) / det;
        if (!std::isfinite(stepX) || !std::isfinite(stepY))
            break;
        dx += stepX;
        dy += stepY;
        if (std::abs(dx - startX) > 1.0 || std::abs(dy - startY) > 1.0) {
            dx = startX;
            dy = startY;
            return;
        }
        if (std::abs(stepX) < 1.0e-4 && std::abs(stepY) < 1.0e-4)
            break;
    }
}

} // namespace

PointTrackResult trackPoint(const PointTrackSettings& settings, const TrackFrameSource& source,
                            const CancellationToken& cancellation, const TrackProgress& progress) {
    PointTrackResult result;
    if (!source || settings.lastFrame < settings.firstFrame || settings.patternRadius < 2 ||
        settings.searchRadius < 1 || !std::isfinite(settings.startX) ||
        !std::isfinite(settings.startY) || !(settings.minimumScore > -1.0)) {
        result.detail = "The track settings are not valid.";
        return result;
    }
    const auto total = static_cast<std::uint64_t>(settings.lastFrame - settings.firstFrame + 1);
    const int r = settings.patternRadius;
    const int search = settings.searchRadius;

    const auto first = source(settings.firstFrame);
    if (!first || !valid(*first)) {
        result.end = PointTrackEnd::Lost;
        result.lostFrame = settings.firstFrame;
        result.detail = "The first frame could not be read.";
        return result;
    }
    double x = settings.startX;
    double y = settings.startY;
    result.samples.push_back({settings.firstFrame, x, y, 1.0});
    if (progress)
        progress(1, total);

    // The pattern is the first frame around the feature and stays so while it keeps matching well;
    // it is retaken from the current frame when the match weakens, so a feature that changes slowly
    // is followed without the error of each frame adding to the last.
    Pattern pattern;
    std::int64_t templateX = 0;
    std::int64_t templateY = 0;
    double offsetX = 0.0; // the feature's position less the pattern centre's
    double offsetY = 0.0;
    const auto retake = [&](const TrackLuma& grid) {
        templateX = static_cast<std::int64_t>(std::floor(x));
        templateY = static_cast<std::int64_t>(std::floor(y));
        offsetX = x - static_cast<double>(templateX);
        offsetY = y - static_cast<double>(templateY);
        return takePattern(grid, templateX, templateY, r, pattern);
    };
    if (!retake(*first)) {
        result.end = PointTrackEnd::Lost;
        result.lostFrame = settings.firstFrame;
        result.detail = "The pattern has no detail or reaches the edge of the picture.";
        return result;
    }

    for (std::int64_t frame = settings.firstFrame + 1; frame <= settings.lastFrame; ++frame) {
        if (cancellation.isCancellationRequested()) {
            result.end = PointTrackEnd::Cancelled;
            return result;
        }
        const auto lost = [&](const char* why) {
            result.end = PointTrackEnd::Lost;
            result.lostFrame = frame;
            result.detail = why;
            return result;
        };
        auto current = source(frame);
        if (!current || !valid(*current))
            return lost("A frame could not be read.");
        if (cancellation.isCancellationRequested()) {
            result.end = PointTrackEnd::Cancelled;
            return result;
        }
        // The search is every whole pixel position of the pattern's centre within the radius of where
        // the feature was last found.
        const auto px = static_cast<std::int64_t>(std::llround(x - offsetX));
        const auto py = static_cast<std::int64_t>(std::llround(y - offsetY));
        double best = -2.0;
        std::int64_t bestX = px;
        std::int64_t bestY = py;
        for (int j = -search; j <= search; ++j)
            for (int i = -search; i <= search; ++i) {
                const auto score = correlate(*current, pattern, px + i, py + j, r);
                if (score > best) {
                    best = score;
                    bestX = px + i;
                    bestY = py + j;
                }
            }
        if (best < settings.minimumScore)
            return lost("The correlation fell below the minimum or the feature left the picture.");
        double dx = static_cast<double>(bestX - templateX);
        double dy = static_cast<double>(bestY - templateY);
        refine(*current, pattern, templateX, templateY, r, dx, dy);
        x = static_cast<double>(templateX) + dx + offsetX;
        y = static_cast<double>(templateY) + dy + offsetY;
        result.samples.push_back({frame, x, y, best});
        if (best < 0.9 && !retake(*current))
            return lost("The pattern has no detail or reaches the edge of the picture.");
        if (progress)
            progress(static_cast<std::uint64_t>(frame - settings.firstFrame + 1), total);
    }
    result.end = PointTrackEnd::Finished;
    return result;
}

} // namespace bloom::runtime
