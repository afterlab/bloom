#pragma once

// The Profiler panel's history (#423): every graphed series as a fixed 600-sample ring, all backed
// by ONE float block allocated when the history is created and freed when it is destroyed. Pushing
// a snapshot derives rates from the previous one (kept as a copy) and writes one float per series;
// it never allocates. Absent values and rates that cannot be derived (no previous sample, an absent
// counter, a counter that went backwards) are pushed as kit::kAbsentSample, so a graph shows a gap,
// never a zero. Pressure events are derived the same way into a fixed ring of events.

#include <bloom/runtime/profiler_snapshot.hpp>
#include <bloom/ui/kit/metrics.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace bloom::ui {

class ProfilerHistory final {
  public:
    // Five minutes at the default 2 Hz.
    static constexpr int kCapacity = 600;
    static constexpr int kEventCapacity = 16;

    enum class Series : int {
        Rss,
        Anon,
        Staging,
        Claims,
        // Positive unattributed bytes (the stacked band); the signed value is in the snapshot.
        Unattributed,
        IdleBaseline,
        GpuLive,
        GpuRetiring,
        GpuHeapUsage,
        GpuHeapBudget,
        GpuUtilization,
        GpuUploadRate,
        GpuReadbackRate,
        GpuSubmitRate,
        ProcessCores,
        PlaybackFps,
        ExportFps,
        RamPreviewFillFps,
        AdmissionWaitRate,
        AdmissionRefusalRate,
        EvictionRate,
        Count,
    };
    static constexpr int kSeriesCount = static_cast<int>(Series::Count);
    static constexpr int kPoolSeries = static_cast<int>(runtime::kProfilerMaximumPools);
    static constexpr int kExecutorSeries = static_cast<int>(runtime::kProfilerMaximumExecutorPools);
    static constexpr int kRingCount = kSeriesCount + kPoolSeries + kExecutorSeries;

    enum class EventKind : std::uint8_t {
        MemoryPressureBegan,
        MemoryPressureEnded,
        SwapPressureBegan,
        SwapPressureEnded,
        AdmissionRefused,
        GpuRefused,
        Evicted,
        HelperExited,
    };
    struct Event {
        std::int64_t timestampNs = 0;
        EventKind kind = EventKind::Evicted;
        // A count, bytes, or a helper pid, by kind.
        double value = 0.0;
        // An eviction's runtime::ProfilerEvictionCause, or a helper's slot.
        std::uint8_t detail = 0;
    };

    ProfilerHistory();
    ~ProfilerHistory();
    ProfilerHistory(const ProfilerHistory&) = delete;
    ProfilerHistory& operator=(const ProfilerHistory&) = delete;

    void push(const runtime::ProfilerSnapshot& snapshot) noexcept;
    void clear() noexcept;

    [[nodiscard]] const kit::SampleRing& series(Series series) const noexcept;
    // A pool slot's host plus pending bytes, and an executor pool slot's utilization (0..1).
    [[nodiscard]] const kit::SampleRing& pool(int slot) const noexcept;
    [[nodiscard]] const kit::SampleRing& executor(int slot) const noexcept;
    [[nodiscard]] int size() const noexcept { return rings_[0].size(); }

    // Newest first: event(0) is the latest.
    [[nodiscard]] int eventCount() const noexcept { return eventCount_; }
    [[nodiscard]] const Event& event(int newest) const noexcept;

    [[nodiscard]] bool hasPrevious() const noexcept { return previous_.serial != 0; }
    // Bytes the history allocated, once (tests).
    [[nodiscard]] static constexpr std::size_t storageBytes() noexcept {
        return sizeof(float) * kCapacity * kRingCount;
    }

  private:
    void record(EventKind kind, std::int64_t timestampNs, double value, std::uint8_t detail = 0);
    kit::SampleRing& ring(Series series) noexcept;

    std::unique_ptr<float[]> storage_;
    std::array<kit::SampleRing, kRingCount> rings_{};
    runtime::ProfilerSnapshot previous_{};
    std::array<Event, kEventCapacity> events_{};
    int eventHead_ = 0;
    int eventCount_ = 0;
};

} // namespace bloom::ui
