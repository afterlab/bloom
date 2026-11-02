#pragma once

// A deterministic stand-in for the runtime profiler sampler (#423), for tests and development
// only; it is never offered in the interface. Sample n is a pure function of n: memory grows,
// plateaus and drops on an eviction in a two-minute cycle while an unattributed gap creeps up over
// the idle baseline; the ledger sample's age cycles and stalls once a cycle (stale ages); GPU
// utilization and one helper's CPU time stay absent, and the encode stage has no samples until
// the first export; playback and export alternate, so their rates are absent half the time.
//
// Tests and captures use it; the application's source is RuntimeProfilerFeed
// (profiler_runtime_feed.hpp) over the runtime sampler.

#include <bloom/ui/profiler_feed.hpp>

#include <chrono>
#include <cstdint>

namespace bloom::ui {

// Fills `out` with fake sample `serial` (1-based) taken `interval` apart.
void fillFakeProfilerSnapshot(std::uint64_t serial, std::chrono::milliseconds interval,
                              runtime::ProfilerSnapshot& out) noexcept;

class FakeProfilerFeed final : public ProfilerFeed {
    Q_OBJECT

  public:
    explicit FakeProfilerFeed(QObject* parent = nullptr);
    ~FakeProfilerFeed() override;

    // The next sample the feed will publish (1 before the first).
    [[nodiscard]] std::uint64_t nextSerial() const noexcept { return next_; }
    // Skips ahead, so a capture can show a full history without waiting for it.
    void setNextSerial(std::uint64_t serial) noexcept { next_ = serial == 0 ? 1 : serial; }

  protected:
    bool readLatest(runtime::ProfilerSnapshot& out) override;

  private:
    std::uint64_t next_ = 1;
};

} // namespace bloom::ui
