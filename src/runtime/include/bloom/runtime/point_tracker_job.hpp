#pragma once

// #513 the tracking job's body: follow a feature in the picture arriving at a Stabilize node.
//
// How pixels reach the tracker. The job evaluates the node's input, one frame at a time, with the CPU
// composition evaluator (the CPU reference path, at the composition's resolution or a proxy), turns
// each finished frame into a grid of luminance and hands it to `trackPoint` (point_tracker.hpp). It
// runs as a cancellable task off the UI thread and off the render path: nothing it reads comes from a
// GPU pass, no frame being rendered waits on it, and no readback is added to any render route. Its
// output is plain data (a position and a score per frame) that the document stores as a data block
// and as keyframes. Rendering the node afterwards is the ordinary field raster on the CPU and GPU
// (docs/architecture/point-tracking.md), so the GPU coverage contract is met by the node's own
// Required fixtures; the job is analysis, and neither a pixel operation nor a render route.

#include <bloom/runtime/point_tracker.hpp>

#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/channel_inspection.hpp>
#include <bloom/runtime/compiled_plan.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace bloom::runtime {

// What to show to track the picture arriving at the input of `node` (a Stabilize node): the output
// port that feeds it, as a viewer-only inspection (the same mechanism an Image Preview uses to show
// any node's picture). The caller compiles the composition with this inspection, which gives a plan
// whose output is that picture and which lowers only what feeds it; nullopt when the input is not
// connected.
[[nodiscard]] std::optional<ChannelInspection> trackTapFor(const document::Composition& composition,
                                                           document::NodeId node);

struct NodeTrackRequest final {
    // The plan of `trackTapFor`: its output is the picture to track.
    std::shared_ptr<const CompiledCompositionPlan> plan;
    // The evaluation resolution; the composition's own format or a proxy of it.
    std::uint32_t extentWidth = 1920;
    std::uint32_t extentHeight = 1080;
    // The composition's format, in pixels, which the points are given in.
    double compositionWidth = 1920.0;
    double compositionHeight = 1080.0;
    // The frame rate: `frameRateNumerator / frameRateDenominator` frames a second.
    std::uint32_t frameRateNumerator = 24;
    std::uint32_t frameRateDenominator = 1;
    // The first and last composition frame to track, inclusive.
    std::int64_t firstFrame = 0;
    std::int64_t lastFrame = 0;
    // The feature on the first frame, pixels from the composition's centre, y down.
    double startX = 0.0;
    double startY = 0.0;
    // Half the side of the pattern and of the search, in evaluation pixels.
    int patternRadius = 16;
    int searchRadius = 32;
    double minimumScore = 0.6;
    // The pixel storage allowed for one evaluated frame.
    std::size_t pixelStorageByteLimit = std::size_t{1} << 28U;
};

// The track in the composition's own units: pixels from its centre, y down, one sample per frame.
[[nodiscard]] PointTrackResult trackNodeInput(const NodeTrackRequest& request,
                                              const CancellationToken& cancellation,
                                              const TrackProgress& progress = {});

} // namespace bloom::runtime
