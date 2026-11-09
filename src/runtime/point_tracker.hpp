#pragma once

// #513 point tracking: follow one feature through a range of frames and report where it is in each.
//
// The tracker is analysis, not rendering. It runs as a cancellable job off the render path and its
// output is plain data (a position and a score per frame) that the document stores; it never feeds
// pixels back into a frame being evaluated, so it needs no mid-frame readback. The pixels it reads
// are the evaluated picture of a node's input, taken one whole frame at a time by the caller
// (`trackNodeInput`, point_tracker_job.hpp) at the composition's evaluation resolution on the CPU
// reference path; this file only sees a grid of luminance values.
//
// Method: normalised cross-correlation of a square pattern against a square search window, frame to
// frame (the pattern is re-taken from the previous frame at the position just found, so a feature
// that changes slowly is followed), refined to a fraction of a pixel by a parabola through the
// correlation surface. Positions are in pixels of the grid, x to the right and y down, the centre of
// the first pixel at (0.5, 0.5).

#include <bloom/runtime/cancellation.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime {

struct TrackLuma final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> values; // width * height, row major, top row first
};

struct PointTrackSettings final {
    // The feature's position on the first frame, in grid pixels.
    double startX = 0.0;
    double startY = 0.0;
    // First and last frame to track (inclusive). The first frame is the feature as chosen.
    std::int64_t firstFrame = 0;
    std::int64_t lastFrame = 0;
    // Half the side of the pattern and of the search, in pixels: the pattern is (2 p + 1) square and
    // the feature may move up to `searchRadius` pixels between two frames.
    int patternRadius = 12;
    int searchRadius = 24;
    // Below this correlation the feature is lost and tracking stops.
    double minimumScore = 0.6;
};

struct PointTrackSample final {
    std::int64_t frame = 0;
    double x = 0.0;
    double y = 0.0;
    // The correlation of the match, 1 for a perfect one (the first frame is exactly 1).
    double score = 1.0;
};

enum class PointTrackEnd : std::uint8_t {
    Finished,
    Cancelled,
    // The feature was lost: the correlation fell below the minimum, or a frame could not be read, or
    // the pattern reached the edge of the grid.
    Lost,
    InvalidSettings,
};

struct PointTrackResult final {
    PointTrackEnd end = PointTrackEnd::InvalidSettings;
    // One sample per frame from firstFrame, through the last frame tracked.
    std::vector<PointTrackSample> samples;
    // The frame that failed when `end` is Lost; the text says why.
    std::int64_t lostFrame = 0;
    std::string detail;
};

// Reads one frame's luminance, or nothing when it cannot be read.
using TrackFrameSource = std::function<std::optional<TrackLuma>(std::int64_t frame)>;
// Called after each frame with the number of frames done and the total.
using TrackProgress = std::function<void(std::uint64_t done, std::uint64_t total)>;

[[nodiscard]] PointTrackResult trackPoint(const PointTrackSettings& settings,
                                          const TrackFrameSource& source,
                                          const CancellationToken& cancellation,
                                          const TrackProgress& progress = {});

} // namespace bloom::runtime
