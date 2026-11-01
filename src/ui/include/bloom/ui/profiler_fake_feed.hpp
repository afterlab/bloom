#pragma once

// A deterministic stand-in for the runtime profiler sampler (#423), for tests and development
// only; it is never offered in the interface. Sample n is a pure function of n: memory grows,
// plateaus and drops on an eviction in a two-minute cycle while an unattributed gap creeps up over
// the idle baseline; the ledger sample's age cycles and stalls once a cycle (stale ages); GPU
// utilization, one helper's CPU time and the encode stage stay absent; playback and export
// alternate, so their rates are absent half the time.
//
// The application uses it only when BLOOM_PROFILER_FAKE_FEED is set (see
// makeDevelopmentProfilerFeed()); otherwise the Profiler has no source until the runtime sampler
// is wired, and every metric reads absent.

#include <bloom/ui/profiler_feed.hpp>

#include <chrono>
#include <cstdint>
#include <memory>

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

// The environment variable that selects the fake feed for development.
inline constexpr const char* kProfilerFakeFeedVariable = "BLOOM_PROFILER_FAKE_FEED";

// The feed the application hands the Profiler today: the fake feed when kProfilerFakeFeedVariable
// is set to anything but empty or "0", otherwise none.
[[nodiscard]] std::unique_ptr<ProfilerFeed> makeDevelopmentProfilerFeed(QObject* parent = nullptr);

} // namespace bloom::ui
