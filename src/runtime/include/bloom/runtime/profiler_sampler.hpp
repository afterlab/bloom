#pragma once

// The runtime profiler sampler (#423; docs/architecture/profiler-feed.md). One low-priority
// thread, refcounted: the first acquire() starts it, the last release() stops and joins it, so
// nothing runs without a subscriber. It samples at 2 Hz by default (1 to 10 Hz), fills a
// preallocated ProfilerSnapshot and publishes it through a seqlock. After start it never
// allocates: OS reads use fixed buffers and every source copies into fixed slots.
//
// It only reads. It never mutates budgets, caches or scheduling and never triggers ledger process
// sampling. Values only the UI owns (the ledger's pool holding, which must be read on the thread
// that polls the ledger, and the playback, RAM Preview and export rates) arrive through
// publishHostInputs(), which the UI-side hook calls on its own thread.

#include <bloom/runtime/profiler_ledger_adapter.hpp>
#include <bloom/runtime/profiler_seqlock.hpp>
#include <bloom/runtime/profiler_snapshot.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace bloom::runtime {

class TaskScheduler;

struct ProfilerSamplerSources final {
    // Tasks and executor pools; null leaves the workload absent.
    const TaskScheduler* scheduler = nullptr;
    // Bloom's own device allocations, filled into the GPU section. Null when the build has no GPU
    // device; must not allocate or block.
    void (*readGpu)(ProfilerGpu& gpu) noexcept = nullptr;
};

// Values the UI-side hook publishes. Trivially copyable.
struct ProfilerHostInputs final {
    ProfilerLedgerReading ledger{};
    ProfilerRatio playbackFps{};
    ProfilerCount droppedFrames{};
    ProfilerCount ramPreviewCachedFrames{};
    ProfilerRatio ramPreviewFillFps{};
    ProfilerRatio exportFps{};
    // Playback, a RAM Preview fill or an export is running: the idle baseline waits for none.
    bool hostBusy = false;
};
static_assert(std::is_trivially_copyable_v<ProfilerHostInputs>);

// Whether a sample shows Bloom idle, the only time the idle baseline is captured: no queued or
// running task in the snapshot's workload and nothing busy in the host inputs.
[[nodiscard]] bool profilerSampleIdle(const ProfilerSnapshot& snapshot,
                                      const ProfilerHostInputs& inputs) noexcept;

class ProfilerSampler final {
  public:
    static constexpr std::chrono::milliseconds kDefaultInterval{500};
    static constexpr std::chrono::milliseconds kMinimumInterval{100};
    static constexpr std::chrono::milliseconds kMaximumInterval{1000};

    explicit ProfilerSampler(ProfilerSamplerSources sources = {});
    ~ProfilerSampler();
    ProfilerSampler(const ProfilerSampler&) = delete;
    ProfilerSampler& operator=(const ProfilerSampler&) = delete;

    // Refcounted subscription. The first acquire starts the thread, which samples at once; the
    // last release stops and joins it.
    void acquire();
    void release();
    [[nodiscard]] int subscribers() const;
    [[nodiscard]] bool running() const;

    // Clamped to kMinimumInterval..kMaximumInterval; takes effect at the next wait.
    void setInterval(std::chrono::milliseconds interval) noexcept;
    [[nodiscard]] std::chrono::milliseconds interval() const noexcept;

    // Any thread; read by the next sample.
    void publishHostInputs(const ProfilerHostInputs& inputs) noexcept;

    // Copies the newest published snapshot into `out` when its serial is newer than `afterSerial`.
    // Any thread; never allocates.
    bool readLatest(ProfilerSnapshot& out, std::uint64_t afterSerial) const noexcept;
    [[nodiscard]] std::uint64_t publishedSamples() const noexcept { return published_.writes(); }

    // Takes and publishes one sample on the calling thread; refused (false) while the thread runs.
    // For tests and cost measurement.
    bool sampleNow() noexcept;

  private:
    void run() noexcept;
    void sample() noexcept;
    // Stops and joins the thread; lifecycle_ held.
    void stop();

    ProfilerSamplerSources sources_;
    // Serializes acquire, release, sampleNow and destruction, held across the join.
    mutable std::mutex lifecycle_;
    // Guards stopping_ for the thread's wait.
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    int subscribers_ = 0;
    bool stopping_ = false;
    std::atomic<std::int64_t> intervalNs_;

    // Written only by the sampling thread (or sampleNow while no thread runs).
    ProfilerSnapshot work_{};
    ProfilerHostInputs inputs_{};
    std::uint64_t serial_ = 0;
    bool baselineCaptured_ = false;
    std::uint64_t baselineBytes_ = 0;

    std::mutex inputsWriter_;
    ProfilerSeqlock<ProfilerHostInputs> hostInputs_;
    ProfilerSeqlock<ProfilerSnapshot> published_;
};

} // namespace bloom::runtime
