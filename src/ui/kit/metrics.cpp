#include <bloom/ui/kit/metrics.hpp>

#include <QCoreApplication>
#include <QLocale>

#include <array>
#include <cmath>

namespace bloom::ui::kit {
namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("bloom::ui::kit::Metrics", text);
}

// American English digits and grouping whatever the process locale is.
const QLocale& usEnglish() {
    static const QLocale locale(QLocale::English, QLocale::UnitedStates);
    return locale;
}

// Three significant figures, at most one decimal above 100 ("512", "34.5", "1.23").
QString significant(const double value) {
    const double magnitude = std::fabs(value);
    const int decimals = magnitude >= 100.0 ? 0 : magnitude >= 10.0 ? 1 : 2;
    return usEnglish().toString(value, 'f', decimals);
}

QString bytes(const double value) {
    static const std::array<const char*, 6> units{"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double scaled = std::fabs(value);
    std::size_t unit = 0;
    while (scaled >= 1024.0 && unit + 1 < units.size()) {
        scaled /= 1024.0;
        ++unit;
    }
    const QString number =
        unit == 0 ? usEnglish().toString(static_cast<qlonglong>(scaled)) : significant(scaled);
    return (value < 0.0 ? QStringLiteral("−") : QString{}) + number + QLatin1Char(' ') +
           QLatin1StringView(units[unit]);
}

QString duration(const double nanoseconds) {
    const double magnitude = std::fabs(nanoseconds);
    if (magnitude < 1'000.0)
        return tr("%1 ns").arg(usEnglish().toString(nanoseconds, 'f', 0));
    if (magnitude < 1'000'000.0)
        return tr("%1 µs").arg(significant(nanoseconds / 1'000.0));
    if (magnitude < 1'000'000'000.0)
        return tr("%1 ms").arg(significant(nanoseconds / 1'000'000.0));
    return tr("%1 s").arg(significant(nanoseconds / 1'000'000'000.0));
}

} // namespace

QString absentMetricText() { return tr("Unavailable"); }

QString formatMetric(const double value, const MetricUnit unit) {
    if (std::isnan(value))
        return absentMetricText();
    switch (unit) {
    case MetricUnit::Bytes:
        return bytes(value);
    case MetricUnit::BytesPerSecond:
        return tr("%1/s").arg(bytes(value));
    case MetricUnit::Count:
        return usEnglish().toString(static_cast<qlonglong>(std::llround(value)));
    case MetricUnit::CountPerSecond:
        return tr("%1/s").arg(significant(value));
    case MetricUnit::Ratio:
        return tr("%1%").arg(usEnglish().toString(value * 100.0, 'f', 0));
    case MetricUnit::Cores:
        return tr("%1 cores").arg(usEnglish().toString(value, 'f', 2));
    case MetricUnit::FramesPerSecond:
        return tr("%1 fps").arg(usEnglish().toString(value, 'f', 1));
    case MetricUnit::Duration:
        return duration(value);
    }
    return {};
}

QString formatMetricAge(const std::int64_t ageNs) {
    return tr("%1 s old").arg(usEnglish().toString(static_cast<double>(ageNs) / 1e9, 'f', 1));
}

SampleRing::SampleRing(float* const storage, const int capacity) noexcept
    : storage_(storage), capacity_(storage != nullptr && capacity > 0 ? capacity : 0) {}

void SampleRing::push(const float sample) noexcept {
    if (capacity_ == 0)
        return;
    storage_[head_] = sample;
    head_ = head_ + 1 == capacity_ ? 0 : head_ + 1;
    if (size_ < capacity_)
        ++size_;
}

void SampleRing::clear() noexcept {
    head_ = 0;
    size_ = 0;
}

float SampleRing::at(const int index) const noexcept {
    if (index < 0 || index >= size_)
        return kAbsentSample;
    // The oldest sample sits at head_ once the ring is full, at 0 before.
    const int oldest = size_ == capacity_ ? head_ : 0;
    const int slot = oldest + index;
    return storage_[slot >= capacity_ ? slot - capacity_ : slot];
}

float SampleRing::latest() const noexcept { return at(size_ - 1); }

} // namespace bloom::ui::kit
