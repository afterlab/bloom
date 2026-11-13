#pragma once

#include <bloom/host/export_job_runner.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

// Test support only (never linked into production): a deterministic, scriptable ExportRunBackend
// for ExportQueueService. Each poll() of a run advances it by exactly one step, so a test (or a
// src/ui panel test) can drive a job through Waiting -> Preparing -> Rendering with progress ->
// WaitingForViewer -> Done / Failed with a verification result / Stopped without evaluating or
// encoding anything. Destination samples are answered from an in-memory set, so a test can
// raise a DestinationExists NeedsConfirmation preflight without touching the filesystem.
namespace bloom::host::testing {

struct FakeExportScript final {
    enum class Outcome : std::uint8_t {
        Succeed,
        SucceedWithWarnings,
        // The output publishes but its verification fails (a ColourTags mismatch).
        FailVerification,
        // The output fails while rendering (EncoderFailed) on `failAtFrame`.
        FailRendering,
    };
    // Frames completed per poll while rendering.
    std::uint64_t framesPerPoll = 1;
    // After this many completed frames the route reports WaitingForViewer...
    std::uint64_t waitingForViewerAfterFrames = 0;
    // ...for this many polls (0: never).
    std::uint32_t waitingForViewerPolls = 0;
    Outcome outcome = Outcome::Succeed;
    std::uint64_t failAtFrame = 0;
    // Writes a small placeholder file at each output's first path when it publishes, for a
    // caller that reads the published file back (MCP capture).
    bool writeDestinations = false;
    // The stalled run reports waiting on GPU preparation instead of a media worker.
    bool waitingOnGpuPreparation = false;
    // When nonzero, each output's verification reports a measured media worker peak of this many
    // bytes, with the cap the run was started under (issue #581).
    std::uint64_t workerPeakBytes = 0;
};

class FakeExportRunBackend final : public ExportRunBackend {
  public:
    // The script the next started run uses (FIFO); runs without one succeed.
    void push(FakeExportScript script);
    // Destinations reported as existing (by first path), with a fingerprint derived from the
    // path and `generation`; bump the generation to simulate an external change.
    void setExisting(const std::filesystem::path& firstPath, std::uint64_t generation = 1);
    void clearExisting(const std::filesystem::path& firstPath);
    // Every destination check takes this long from now on; zero (the default) also releases the
    // checks that are waiting. Simulates a slow volume.
    void setDestinationCheckDelay(std::chrono::milliseconds delay);
    // Destination checks blocked by the delay right now.
    [[nodiscard]] std::size_t waitingDestinationChecks() const;
    [[nodiscard]] std::size_t runsStarted() const;
    // The worker limits each started run was given, in start order.
    [[nodiscard]] std::vector<ExportWorkerLimits> startedLimits() const;

    std::unique_ptr<ExportRun> start(ExportJobRunnerDependencies dependencies,
                                     ExportJobRunRequest request) override;
    ExportDestinationSample
    sampleDestination(const ExportDestinationSampleRequest& request) override;

  private:
    mutable std::mutex mutex_;
    std::deque<FakeExportScript> scripts_;
    std::map<std::filesystem::path, std::uint64_t> existing_;
    std::chrono::milliseconds checkDelay_{0};
    std::condition_variable checkDelayChanged_;
    std::size_t waitingChecks_ = 0;
    std::size_t started_ = 0;
    std::vector<ExportWorkerLimits> limits_;
};

} // namespace bloom::host::testing
