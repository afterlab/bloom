#pragma once

// #349 Image Statistics: prepared statistics of a picture. An Image Statistics node gives the value
// graph the minimum, maximum, mean, deviation and count of the pixels of one frame of the picture it
// reads. The picture has a plan of its own (CompiledValueImageStatistics::plan: the picture followed
// by its reduction to two pixels). The frame's evaluator runs that plan for the frame's own time and
// resolution before it samples the node, through a provider (the CPU reference evaluates it, the
// device route runs it on the device and reads back only those two pixels), keeps the result in a
// bounded cache under an exact identity, and the value graph reads it back with no pixel work. A
// frame whose statistics are not prepared is reported as not ready; it never takes the statistics of
// another frame. docs/architecture/image-statistics.md is the specification.

#include <bloom/core/rational_time.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/cancellation.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/evaluation.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bloom::runtime {

// Which route produced the statistics: the CPU reference evaluator or a device. A result is always
// labelled with the route that computed it.
enum class ImageStatisticsRoute : std::uint8_t { CpuReference, Device };

struct ImageStatisticsValues final {
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    double deviation = 0.0;
    // The pixels counted, exactly (the passes carry the count as two whole binary32 numbers).
    double count = 0.0;
    ImageStatisticsRoute route = ImageStatisticsRoute::CpuReference;

    friend bool operator==(const ImageStatisticsValues&, const ImageStatisticsValues&) = default;
};

struct ImageStatisticsQuery final {
    document::NodeId node;
    std::shared_ptr<const CompiledCompositionPlan> plan;
    // The frame: the evaluation time the node is sampled at.
    core::RationalTime time{};
};

enum class ImageStatisticsOutcome : std::uint8_t { Ready, Cancelled, Unavailable };
struct ImageStatisticsStatus final {
    ImageStatisticsOutcome outcome = ImageStatisticsOutcome::Unavailable;
    // For Unavailable: the reason, in words the artist can read.
    std::string detail;
};

struct ImageStatisticsRun final {
    ImageStatisticsStatus status;
    std::optional<ImageStatisticsValues> values;
};

// The statistics a two-pixel result image holds (the output of a statistics plan), or none when it
// is not one. Pixel 0 is (count rounded to binary32, minimum, maximum) and pixel 1 is (mean,
// variance, the whole number the rounded count leaves out); the exact count is their sum and the
// deviation the square root of the variance, both taken here, the same for every route.
[[nodiscard]] std::optional<ImageStatisticsValues>
readImageStatistics(const render::Rgba32fImage& result, ImageStatisticsRoute route);

// Runs the query's plan for the frame and returns its statistics. It runs on a worker and honors
// cancellation; it never runs on the interface thread.
using ImageStatisticsProvider =
    std::function<ImageStatisticsRun(const ImageStatisticsQuery&, const CancellationToken&)>;

// Measures `query` for the frame `frame` is part of on a device route, from a worker that is not
// evaluating on that device itself (the interactive viewer and RAM preview prepare their frames on
// their own workers and hand the picture to the application's device route). The result is labelled
// Device; Unavailable says why the device could not measure, and the frame then takes the CPU
// reference path, which measures the statistics itself.
using ImageStatisticsDeviceMeasure = std::function<ImageStatisticsRun(
    const ImageStatisticsQuery&, const EvaluationRequest& frame, const CancellationToken&)>;

// The cache identity of `query` within the frame a preparation stands for: the same string for the
// same node of the same plan revision at the same time and resolution, whatever thread asks.
[[nodiscard]] std::string imageStatisticsKey(const std::string& frameKey,
                                             const ImageStatisticsQuery& query);

// What names a frame's statistics besides its time and plan: the route, resolution, quality and
// color of `request`. Two requests with the same key and plan measure the same pixels.
[[nodiscard]] std::string imageStatisticsFrameKey(const EvaluationRequest& request,
                                                  ImageStatisticsRoute route);

// The request that evaluates a statistics plan for the frame `frame` is part of: the frame's own
// time (the query's) and resolution, the plan's own output, the whole picture measured and only the
// two-pixel result asked for, and no state of the frame's pass.
[[nodiscard]] EvaluationRequest imageStatisticsRequest(const EvaluationRequest& frame,
                                                       const ImageStatisticsQuery& query);

// Whether a plan's value graph has an Image Statistics operation.
[[nodiscard]] bool planReadsImageStatistics(const CompiledCompositionPlan& plan) noexcept;

// While one is alive on a thread, a value-graph evaluation on that thread may prepare the statistics
// it needs (the evaluators install one on their worker for the duration of a frame). `frameKey`
// names everything about the frame but its time and plan: its resolution, quality and color. An
// evaluation on a thread without one only reads what is already prepared, and with no frame to read
// it for it finds nothing.
class ImageStatisticsPreparation final {
  public:
    ImageStatisticsPreparation(std::string frameKey, ImageStatisticsProvider provider,
                               CancellationToken cancellation);
    ~ImageStatisticsPreparation();
    ImageStatisticsPreparation(const ImageStatisticsPreparation&) = delete;
    ImageStatisticsPreparation& operator=(const ImageStatisticsPreparation&) = delete;
    // Prepares `query` under this thread's innermost preparation, or none when there is none. A
    // result already cached is not computed again.
    [[nodiscard]] static std::optional<ImageStatisticsStatus> prepareHere(const ImageStatisticsQuery&);
    // The prepared statistics of `query` for this thread's frame, or null when they are not ready.
    // Never blocks and never computes.
    [[nodiscard]] static std::shared_ptr<const ImageStatisticsValues>
    findHere(const ImageStatisticsQuery&);
    // Whether this thread has a preparation that can compute statistics.
    [[nodiscard]] static bool installedHere() noexcept;

  private:
    std::string frameKey_;
    ImageStatisticsProvider provider_;
    CancellationToken cancellation_;
    const ImageStatisticsPreparation* previous_ = nullptr;
};

struct ImageStatisticsCacheStatistics final {
    std::size_t entries = 0;
    std::uint64_t hits = 0;
    std::uint64_t computed = 0;
    // Of `computed`, the results measured on a device route (the rest by the CPU reference).
    std::uint64_t computedOnDevice = 0;
    std::uint64_t evicted = 0;
    std::uint64_t failed = 0;
    std::uint64_t cancelled = 0;
};
[[nodiscard]] ImageStatisticsCacheStatistics imageStatisticsCacheStatistics();
// Drops every cached result (cache purge, tests).
void clearImageStatistics();
// The most results kept; the least recently used retire when it is exceeded. A result is a few
// dozen bytes.
inline constexpr std::size_t kImageStatisticsCacheEntries = 4096;

} // namespace bloom::runtime
