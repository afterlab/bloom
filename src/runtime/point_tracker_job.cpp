#include "point_tracker_job.hpp"

#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace bloom::runtime {
std::optional<ChannelInspection> trackTapFor(const document::Composition& composition,
                                             const document::NodeId node) {
    for (const auto& edge : composition.graph().edges())
        if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            input != nullptr && input->nodeId == node && input->port == "input")
            return ChannelInspection{edge.source.nodeId, edge.source.port, {}};
    return std::nullopt;
}

PointTrackResult trackNodeInput(const NodeTrackRequest& request, const CancellationToken& cancellation,
                                const TrackProgress& progress) {
    PointTrackResult failure;
    if (!request.plan || request.lastFrame < request.firstFrame || request.compositionWidth <= 0.0 ||
        request.compositionHeight <= 0.0 || request.frameRateNumerator == 0 || request.frameRateDenominator == 0 ||
        request.extentWidth == 0 || request.extentHeight == 0) {
        failure.detail = "The track request is not valid.";
        return failure;
    }
    const auto extent = render::ImageExtent::create(request.extentWidth, request.extentHeight);
    if (!extent.value()) {
        failure.detail = "The evaluation resolution is not valid.";
        return failure;
    }
    const double width = static_cast<double>(request.extentWidth);
    const double height = static_cast<double>(request.extentHeight);
    const double toGridX = width / request.compositionWidth;
    const double toGridY = height / request.compositionHeight;

    const CpuCompositionEvaluator evaluator;
    std::string unreadable; // the evaluator's reason for the last frame it could not give
    const TrackFrameSource frames = [&](const std::int64_t frame) -> std::optional<TrackLuma> {
        const auto time = core::RationalTime::create(
            frame * static_cast<std::int64_t>(request.frameRateDenominator),
            static_cast<std::int64_t>(request.frameRateNumerator));
        if (!time)
            return std::nullopt;
        EvaluationRequest evaluation{*time, request.plan->output(), ProxyResolution{*extent.value()}};
        evaluation.pixelStorageByteLimit = request.pixelStorageByteLimit;
        evaluation.bypassOperationCache = true;
        const auto result = evaluator.evaluate(request.plan, evaluation, cancellation);
        if (result.frame() == nullptr) {
            unreadable = result.status() == EvaluationStatus::Cancelled ? "The job was cancelled."
                                                                       : "The evaluator reported";
            for (const auto& diagnostic : result.diagnostics())
                unreadable += " " + diagnostic.summary + (diagnostic.detail.empty() ? "" : ": " + diagnostic.detail) + ".";
            return std::nullopt;
        }
        const auto& image = result.frame()->processImage();
        TrackLuma grid;
        grid.width = request.extentWidth;
        grid.height = request.extentHeight;
        grid.values.assign(static_cast<std::size_t>(grid.width) * grid.height, 0.0F);
        for (std::uint32_t y = 0; y < grid.height; ++y)
            for (std::uint32_t x = 0; x < grid.width; ++x)
                if (const auto pixel = image.read(x, y)) {
                    // Rec. 709 luminance of the premultiplied picture: what is seen, over black.
                    const auto c = pixel.value()->components();
                    grid.values[static_cast<std::size_t>(y) * grid.width + x] =
                        0.2126F * c[0] + 0.7152F * c[1] + 0.0722F * c[2];
                }
        return grid;
    };

    PointTrackSettings settings;
    settings.startX = (request.compositionWidth * 0.5 + request.startX) * toGridX;
    settings.startY = (request.compositionHeight * 0.5 + request.startY) * toGridY;
    settings.firstFrame = request.firstFrame;
    settings.lastFrame = request.lastFrame;
    settings.patternRadius = request.patternRadius;
    settings.searchRadius = request.searchRadius;
    settings.minimumScore = request.minimumScore;
    auto result = trackPoint(settings, frames, cancellation, progress);
    if (result.end == PointTrackEnd::Lost && !unreadable.empty())
        result.detail += " " + unreadable;
    for (auto& sample : result.samples) {
        sample.x = sample.x / toGridX - request.compositionWidth * 0.5;
        sample.y = sample.y / toGridY - request.compositionHeight * 0.5;
    }
    return result;
}

} // namespace bloom::runtime
