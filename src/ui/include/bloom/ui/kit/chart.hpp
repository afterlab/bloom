#pragma once

#include <bloom/ui/kit/metrics.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QString>
#include <QWidget>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace bloom::ui::kit {

// The categorical series palette, cycling: existing curve and label tokens, none of them Accent,
// Keyframe, Warn or Error, so a series never reads as a selection, a key or a state.
[[nodiscard]] Color chartSeriesColor(int index);
inline constexpr int kChartSeriesPaletteSize = 8;

// A time-series chart over SampleRings (#423). Bands stack bottom up as filled areas; lines draw
// over them; markers are horizontal reference values with a label (a baseline, a budget). The
// x axis is the rings' full capacity with the newest sample at the right edge, so history grows
// in from the right and a full ring spans the width. An absent sample (kAbsentSample) breaks a
// line and contributes nothing to a band. The y axis starts at zero and ends at a rounded value
// above everything visible, or at a fixed maximum; its top and middle are labelled in `unit`.
//
// The chart owns no samples and copies none: it reads the rings when it paints, and paints only
// when samplesChanged() (or a resize) asks it to. Series are declared once; nothing allocates per
// sample.
class KTimeSeriesChart final : public QWidget {
    Q_OBJECT

  public:
    explicit KTimeSeriesChart(QWidget* parent = nullptr);

    void setUnit(MetricUnit unit);
    [[nodiscard]] MetricUnit unit() const noexcept { return unit_; }
    // A fixed y maximum (a ratio's 1.0); zero or less scales to the data.
    void setScaleMaximum(double maximum);
    // How many samples the x axis spans; defaults to the first ring's capacity.
    void setSpan(int samples);

    void addBand(const SampleRing* ring, Color color);
    void addLine(const SampleRing* ring, Color color, bool dashed = false);
    // Returns the marker's index for setMarkerValue(). A NaN value hides the marker.
    int addMarker(const QString& label, Color color);
    void setMarkerValue(int marker, double value);
    // Hides a band or line without removing it (a pool slot that is not in use).
    void setBandVisible(int band, bool visible);
    void setLineVisible(int line, bool visible);
    void clearSeries();

    // The rings changed; repaint if visible.
    void samplesChanged();

    // The y maximum the last layout chose (for tests and readouts).
    [[nodiscard]] double scaleMaximum() const;
    [[nodiscard]] int bandCount() const noexcept { return static_cast<int>(bands_.size()); }
    [[nodiscard]] int lineCount() const noexcept { return static_cast<int>(lines_.size()); }

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    struct Band {
        const SampleRing* ring;
        Color color;
        bool visible;
    };
    struct Line {
        const SampleRing* ring;
        Color color;
        bool dashed;
        bool visible;
    };
    struct Marker {
        QString label;
        Color color;
        double value;
    };
    [[nodiscard]] int sampleCount() const;
    [[nodiscard]] int span() const;

    MetricUnit unit_ = MetricUnit::Bytes;
    double scaleMaximum_ = 0.0;
    int span_ = 0;
    std::vector<Band> bands_;
    std::vector<Line> lines_;
    std::vector<Marker> markers_;
    // Running band totals while painting, sized once to the span.
    mutable std::vector<double> stackScratch_;
};

// A histogram of fixed buckets (#423's stage timings): one bar per bucket on a log x axis, with
// the title, the sample count and the median and 95th-percentile bucket edges printed above the
// bars. `upperEdges[i]` is bucket i's exclusive upper edge in `unit`; the last bucket is open.
// An absent histogram (a stage nobody times) says so instead of drawing empty bars.
class KHistogramChart final : public QWidget {
    Q_OBJECT

  public:
    static constexpr int kMaximumBuckets = 32;

    explicit KHistogramChart(const QString& title, QWidget* parent = nullptr);

    void setUnit(MetricUnit unit);
    void setUpperEdges(std::span<const double> upperEdges);
    void setCounts(std::span<const std::uint64_t> counts);
    void setAbsent();

    [[nodiscard]] bool absent() const noexcept { return absent_; }
    [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
    // The bucket holding the given fraction of samples (0.5 is the median); -1 when empty.
    [[nodiscard]] int percentileBucket(double fraction) const;
    // "p50 ≤ 2.05 ms · p95 ≤ 8.19 ms · 1,204 samples", or the absent text.
    [[nodiscard]] QString summary() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    [[nodiscard]] QString edgeText(int bucket) const;

    QString title_;
    MetricUnit unit_ = MetricUnit::Duration;
    int bucketCount_ = 0;
    std::array<double, kMaximumBuckets> upperEdges_{};
    std::array<std::uint64_t, kMaximumBuckets> counts_{};
    std::uint64_t total_ = 0;
    bool absent_ = true;
};

} // namespace bloom::ui::kit
