#pragma once

// A clock that moves only when the test moves it. Code under test takes its time source as a
// callable (`std::function<std::chrono::steady_clock::time_point()>` or similar); a test passes
// `clock.source()` and calls `advance()` to cross a deadline, instead of sleeping and asserting on
// elapsed wall time.
//
//   bloom::test_support::ManualClock clock;
//   Monitor monitor{{.now = clock.source()}};
//   clock.advance(2s);
//   monitor.poll();

#include <atomic>
#include <chrono>
#include <functional>

namespace bloom::test_support {

class ManualClock final {
  public:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] Clock::time_point now() const noexcept {
        return Clock::time_point{Clock::duration{ticks_.load(std::memory_order_acquire)}};
    }

    void advance(const Clock::duration by) noexcept {
        ticks_.fetch_add(by.count(), std::memory_order_acq_rel);
    }

    // A time source bound to this clock. The clock must outlive every copy.
    [[nodiscard]] std::function<Clock::time_point()> source() const {
        return [this] { return now(); };
    }

  private:
    // Starts away from the epoch so "no time recorded yet" defaults never compare equal to now.
    std::atomic<Clock::rep> ticks_{std::chrono::duration_cast<Clock::duration>(
                                       std::chrono::hours{1})
                                       .count()};
};

} // namespace bloom::test_support
