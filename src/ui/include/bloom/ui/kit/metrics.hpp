#pragma once

#include <QString>

#include <cstdint>
#include <limits>

namespace bloom::ui::kit {

// What a measured number is, so a chart axis, a readout and a table cell print it the same way.
// Bytes use binary units (KiB, MiB, GiB); a Ratio is 0..1 and prints as a percentage; a Duration
// is in nanoseconds.
enum class MetricUnit : std::uint8_t {
    Bytes,
    BytesPerSecond,
    Count,
    CountPerSecond,
    Ratio,
    Cores,
    FramesPerSecond,
    Duration,
};

// One value in its unit, in American English ("1.2 GiB", "1,204", "45%", "23.9 fps", "850 µs").
// A negative Bytes value keeps its sign. NaN prints as absentMetricText().
[[nodiscard]] QString formatMetric(double value, MetricUnit unit);

// The one word for a metric that is not measured here: never a zero.
[[nodiscard]] QString absentMetricText();

// How old a value from a slower source is ("3.2 s old").
[[nodiscard]] QString formatMetricAge(std::int64_t ageNs);

// An absent sample in a SampleRing: a gap, never a zero.
inline constexpr float kAbsentSample = std::numeric_limits<float>::quiet_NaN();

// A fixed-capacity ring of float samples over storage someone else owns, so one allocation can
// back many rings (a panel's whole history). Pushing never allocates; once full, each push
// overwrites the oldest sample. at(0) is the oldest sample held, at(size() - 1) the newest.
class SampleRing final {
  public:
    SampleRing() = default;
    SampleRing(float* storage, int capacity) noexcept;

    void push(float sample) noexcept;
    void clear() noexcept;

    [[nodiscard]] int capacity() const noexcept { return capacity_; }
    [[nodiscard]] int size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] float at(int index) const noexcept;
    // The newest sample, or kAbsentSample when empty.
    [[nodiscard]] float latest() const noexcept;
    // The storage this ring writes into (for tests that check nothing moved).
    [[nodiscard]] const float* storage() const noexcept { return storage_; }

  private:
    float* storage_ = nullptr;
    int capacity_ = 0;
    int head_ = 0;
    int size_ = 0;
};

} // namespace bloom::ui::kit
