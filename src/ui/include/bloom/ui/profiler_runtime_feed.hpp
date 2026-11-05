#pragma once

// The Profiler's real source (#423; docs/architecture/profiler-feed.md): a ProfilerFeed over the
// runtime ProfilerSampler. Activation acquires the sampler, deactivation releases it, and
// readLatest() copies the sampler's published snapshot.
//
// It is also the UI-side hook for what only the UI owns. On every feed tick, on the UI thread, it
// reads the memory budget ledger through readProfilerLedger() (the pool holding must be read on
// the thread that polls the ledger; the ledger state only after the ledger applied a sample) and
// the playback, RAM Preview and export controllers, the Viewer's displayed frame and the GPU
// preview service's counters, and publishes those values into the sampler with
// publishHostInputs(). It never reads a Qt object off the UI thread and never mutates
// budgets, caches or scheduling.

#include <bloom/runtime/profiler_sampler.hpp>
#include <bloom/ui/profiler_feed.hpp>

#include <QMetaObject>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

namespace bloom::runtime {
class GpuPreviewDisplayService;
class GpuProcessingPhasePolicy;
class MemoryBudgetLedger;
} // namespace bloom::runtime

namespace bloom::ui {

class CompositionPreviewController;
class CompositionSession;
class ExportQueueAdapter;
class MemoryBudgetController;
class RamPreviewController;

// Every source is optional; a missing one leaves its metrics absent.
struct ProfilerUiSources final {
    const runtime::MemoryBudgetLedger* ledger = nullptr;
    // Its sampleApplied and pressureApplied signals mark the ledger state as worth re-reading.
    MemoryBudgetController* memoryBudget = nullptr;
    CompositionSession* session = nullptr;
    CompositionPreviewController* preview = nullptr;
    RamPreviewController* ramPreview = nullptr;
    ExportQueueAdapter* exportQueue = nullptr;
    // Host bytes admission has granted to running work (its claims minus the preview retention
    // they include); read with the ledger on each tick.
    const runtime::GpuProcessingPhasePolicy* admission = nullptr;
    // The GPU preview route's CPU-fallback, admission and completion counters (schema 5).
    const runtime::GpuPreviewDisplayService* previewService = nullptr;
};

// The sampler's GPU source: Bloom's own device images from the render module's process-wide
// reclamation counters (live and retiring bytes, refusals, reclaims), through
// runtime::readProfilerGpuDevice. Absent in a build without a GPU device.
void readProfilerGpuImageStats(runtime::ProfilerGpu& gpu) noexcept;

class RuntimeProfilerFeed final : public ProfilerFeed {
    Q_OBJECT

  public:
    RuntimeProfilerFeed(runtime::ProfilerSampler& sampler, ProfilerUiSources sources,
                        QObject* parent = nullptr);
    ~RuntimeProfilerFeed() override;

    // Reads the UI-owned values now and publishes them into the sampler (the feed tick does this;
    // tests call it to step).
    void publishUiInputs();

  protected:
    void activate() override;
    void deactivate() override;
    bool readLatest(runtime::ProfilerSnapshot& out) override;

  private:
    struct Rate final {
        std::optional<std::uint64_t> count;
        std::chrono::steady_clock::time_point at;
    };
    // Frames per second since the previous call while `active`; absent otherwise.
    static runtime::ProfilerRatio rate(Rate& previous, bool active, std::uint64_t count,
                                       std::chrono::steady_clock::time_point now);

    runtime::ProfilerSampler& sampler_;
    ProfilerUiSources sources_;
    runtime::ProfilerHostInputs inputs_{};
    bool ledgerStateDirty_ = true;
    // Playhead advances while playing (the transport's delivered frames).
    std::uint64_t playbackFrames_ = 0;
    Rate playbackRate_{};
    Rate fillRate_{};
    Rate exportRate_{};
    std::array<QMetaObject::Connection, 3> connections_{};
};

} // namespace bloom::ui
