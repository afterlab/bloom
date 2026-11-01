#include <bloom/ui/kit/chart.hpp>

#include <bloom/ui/kit/accessibility.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace bloom::ui::kit {
namespace {

constexpr std::array<Color, kChartSeriesPaletteSize> kSeriesPalette{
    Color::GraphCurve3, Color::GraphCurve2, Color::ComponentZ, Color::GraphCurve5,
    Color::ComponentY,  Color::Label2,      Color::Label8,     Color::Label6,
};

// The quarter grid lines a chart draws across its plot.
constexpr int kGridDivisions = 4;

// The next "round" value at or above `value`: 1, 1.5, 2, 3, 4, 5, 6 or 8 times a power of ten --
// of 1024 first for bytes, so a byte axis ends on 3 GiB rather than 3.22 GB.
double niceCeiling(const double value, const MetricUnit unit) {
    if (!(value > 0.0))
        return 1.0;
    const bool binary = unit == MetricUnit::Bytes || unit == MetricUnit::BytesPerSecond;
    double base = 1.0;
    double rest = value;
    if (binary) {
        while (rest >= 1024.0) {
            rest /= 1024.0;
            base *= 1024.0;
        }
    }
    const double decade = std::pow(10.0, std::floor(std::log10(rest)));
    const double fraction = rest / decade;
    static constexpr std::array<double, 9> steps{1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0};
    const auto step = std::ranges::find_if(steps, [fraction](double s) { return s >= fraction; });
    return base * decade * (step == steps.end() ? 10.0 : *step);
}

QRectF plotRect(const QWidget& widget) {
    const qreal inset = px(Spacing::XS);
    return QRectF(widget.rect()).adjusted(inset, inset, -inset, -inset);
}

void paintFrame(QPainter& painter, const QWidget& widget) {
    fillRoundedSurface(painter, QRectF(widget.rect()), color(Color::SurfaceSunken),
                       color(Color::Border), Radius::Small);
}

} // namespace

Color chartSeriesColor(const int index) {
    const int slot =
        ((index % kChartSeriesPaletteSize) + kChartSeriesPaletteSize) % kChartSeriesPaletteSize;
    return kSeriesPalette[static_cast<std::size_t>(slot)];
}

// ---- KTimeSeriesChart -------------------------------------------------------------------------

KTimeSeriesChart::KTimeSeriesChart(QWidget* parent) : QWidget(parent) {
    installKitAccessibility();
    setObjectName(QStringLiteral("kTimeSeriesChart"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(px(Size::ChartHeight));
}

void KTimeSeriesChart::setUnit(const MetricUnit unit) {
    unit_ = unit;
    update();
}

void KTimeSeriesChart::setScaleMaximum(const double maximum) {
    scaleMaximum_ = maximum;
    update();
}

void KTimeSeriesChart::setSpan(const int samples) {
    span_ = std::max(0, samples);
    stackScratch_.reserve(static_cast<std::size_t>(span()));
    update();
}

void KTimeSeriesChart::addBand(const SampleRing* const ring, const Color color) {
    bands_.push_back({ring, color, true});
    stackScratch_.reserve(static_cast<std::size_t>(span()));
}

void KTimeSeriesChart::addLine(const SampleRing* const ring, const Color color, const bool dashed) {
    lines_.push_back({ring, color, dashed, true});
}

int KTimeSeriesChart::addMarker(const QString& label, const Color color) {
    markers_.push_back({label, color, std::nan("")});
    return static_cast<int>(markers_.size()) - 1;
}

void KTimeSeriesChart::setMarkerValue(const int marker, const double value) {
    if (marker < 0 || marker >= static_cast<int>(markers_.size()))
        return;
    markers_[static_cast<std::size_t>(marker)].value = value;
}

void KTimeSeriesChart::setBandVisible(const int band, const bool visible) {
    if (band >= 0 && band < static_cast<int>(bands_.size()))
        bands_[static_cast<std::size_t>(band)].visible = visible;
}

void KTimeSeriesChart::setLineVisible(const int line, const bool visible) {
    if (line >= 0 && line < static_cast<int>(lines_.size()))
        lines_[static_cast<std::size_t>(line)].visible = visible;
}

void KTimeSeriesChart::clearSeries() {
    bands_.clear();
    lines_.clear();
    markers_.clear();
    update();
}

void KTimeSeriesChart::samplesChanged() { update(); }

int KTimeSeriesChart::sampleCount() const {
    int count = 0;
    for (const auto& band : bands_)
        count = std::max(count, band.ring != nullptr ? band.ring->size() : 0);
    for (const auto& line : lines_)
        count = std::max(count, line.ring != nullptr ? line.ring->size() : 0);
    return count;
}

int KTimeSeriesChart::span() const {
    if (span_ > 0)
        return span_;
    for (const auto& band : bands_)
        if (band.ring != nullptr)
            return band.ring->capacity();
    for (const auto& line : lines_)
        if (line.ring != nullptr)
            return line.ring->capacity();
    return 1;
}

double KTimeSeriesChart::scaleMaximum() const {
    if (scaleMaximum_ > 0.0)
        return scaleMaximum_;
    double highest = 0.0;
    const int count = sampleCount();
    for (int index = 0; index < count; ++index) {
        double stacked = 0.0;
        for (const auto& band : bands_) {
            if (!band.visible || band.ring == nullptr || index >= band.ring->size())
                continue;
            const float sample = band.ring->at(index - (count - band.ring->size()));
            if (!std::isnan(sample))
                stacked += std::max(0.0, static_cast<double>(sample));
        }
        highest = std::max(highest, stacked);
        for (const auto& line : lines_) {
            if (!line.visible || line.ring == nullptr)
                continue;
            const float sample = line.ring->at(index - (count - line.ring->size()));
            if (!std::isnan(sample))
                highest = std::max(highest, static_cast<double>(sample));
        }
    }
    for (const auto& marker : markers_)
        if (!std::isnan(marker.value))
            highest = std::max(highest, marker.value);
    return niceCeiling(highest, unit_);
}

QSize KTimeSeriesChart::sizeHint() const {
    return {px(Size::ReadoutColumnWidth), px(Size::ChartHeight)};
}

QSize KTimeSeriesChart::minimumSizeHint() const {
    return {px(Size::ReadoutValueWidth), px(Size::ChartHeight)};
}

void KTimeSeriesChart::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), color(Color::Background));
    painter.setRenderHint(QPainter::Antialiasing);
    paintFrame(painter, *this);
    const QRectF plot = plotRect(*this);
    const double maximum = scaleMaximum();
    const auto yFor = [&](const double value) {
        const double share = std::clamp(value / maximum, 0.0, 1.0);
        return plot.bottom() - plot.height() * share;
    };

    applyHairlinePen(painter, color(Color::Border));
    for (int division = 1; division < kGridDivisions; ++division) {
        const qreal y = plot.bottom() - plot.height() * division / kGridDivisions;
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    const int count = sampleCount();
    const double step = plot.width() / std::max(1, span() - 1);
    const auto xFor = [&](const int index) { return plot.right() - (count - 1 - index) * step; };
    painter.save();
    painter.setClipRect(plot);
    if (count > 0) {
        stackScratch_.assign(static_cast<std::size_t>(count), 0.0);
        for (const auto& band : bands_) {
            if (!band.visible || band.ring == nullptr || band.ring->empty())
                continue;
            const int offset = count - band.ring->size();
            QPainterPath top;
            QPainterPath area;
            for (int index = 0; index < count; ++index) {
                const float sample =
                    index >= offset ? band.ring->at(index - offset) : kAbsentSample;
                const double below = stackScratch_[static_cast<std::size_t>(index)];
                const double above =
                    below + (std::isnan(sample) ? 0.0 : std::max(0.0, static_cast<double>(sample)));
                const QPointF point(xFor(index), yFor(above));
                if (index == 0) {
                    top.moveTo(point);
                    area.moveTo(point);
                } else {
                    top.lineTo(point);
                    area.lineTo(point);
                }
                stackScratch_[static_cast<std::size_t>(index)] = above;
            }
            // Back along the band's floor: the previous running total, rebuilt from this band.
            for (int index = count - 1; index >= 0; --index) {
                const float sample =
                    index >= offset ? band.ring->at(index - offset) : kAbsentSample;
                const double above = stackScratch_[static_cast<std::size_t>(index)];
                const double below =
                    above - (std::isnan(sample) ? 0.0 : std::max(0.0, static_cast<double>(sample)));
                area.lineTo(QPointF(xFor(index), yFor(below)));
            }
            area.closeSubpath();
            painter.fillPath(area, withOpacity(color(band.color), kChartBandOpacity));
            applyHairlinePen(painter, color(band.color));
            painter.drawPath(top);
        }
        for (const auto& line : lines_) {
            if (!line.visible || line.ring == nullptr || line.ring->empty())
                continue;
            const int offset = count - line.ring->size();
            QPainterPath path;
            bool drawing = false;
            for (int index = offset; index < count; ++index) {
                const float sample = line.ring->at(index - offset);
                if (std::isnan(sample)) {
                    drawing = false;
                    continue;
                }
                const QPointF point(xFor(index), yFor(static_cast<double>(sample)));
                if (drawing)
                    path.lineTo(point);
                else
                    path.moveTo(point);
                drawing = true;
            }
            QPen pen(color(line.color), kChartLineStroke);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            if (line.dashed)
                pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
        }
    }
    painter.restore();

    const QFont small = kit::font(TypeRole::UiSmall);
    painter.setFont(small);
    const QFontMetricsF metrics(small);
    for (const auto& marker : markers_) {
        if (std::isnan(marker.value) || marker.value > maximum)
            continue;
        const qreal y = yFor(marker.value);
        QPen pen(color(marker.color), snappedHairlineWidth(devicePixelRatioF()));
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        const qreal width = metrics.horizontalAdvance(marker.label);
        const qreal top = std::max(plot.top(), y - metrics.height());
        painter.drawText(QRectF(plot.right() - width, top, width, metrics.height()),
                         Qt::AlignRight | Qt::AlignVCenter, marker.label);
    }
    // The axis labels sit on a plate of the chart's own ground so a series under them never
    // makes them unreadable.
    const auto axisLabel = [&](const qreal top, const double value) {
        const QString label = formatMetric(value, unit_);
        const QRectF box(plot.left(), top, metrics.horizontalAdvance(label) + px(Spacing::XXS) * 2,
                         metrics.height());
        painter.fillRect(box, color(Color::SurfaceSunken));
        painter.setPen(color(Color::Muted));
        painter.drawText(box, Qt::AlignCenter, label);
    };
    axisLabel(plot.top(), maximum);
    axisLabel(plot.center().y() - metrics.height(), maximum / 2.0);
}

// ---- KHistogramChart --------------------------------------------------------------------------

KHistogramChart::KHistogramChart(const QString& title, QWidget* parent)
    : QWidget(parent), title_(title) {
    installKitAccessibility();
    setObjectName(QStringLiteral("kHistogramChart"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(px(Size::HistogramHeight));
    setAccessibleName(title);
}

void KHistogramChart::setUnit(const MetricUnit unit) {
    unit_ = unit;
    update();
}

void KHistogramChart::setUpperEdges(const std::span<const double> upperEdges) {
    const auto count = std::min<std::size_t>(upperEdges.size(), upperEdges_.size() - 1);
    std::copy_n(upperEdges.begin(), count, upperEdges_.begin());
    bucketCount_ = static_cast<int>(count) + 1;
    update();
}

void KHistogramChart::setCounts(const std::span<const std::uint64_t> counts) {
    const auto count = std::min<std::size_t>(counts.size(), static_cast<std::size_t>(bucketCount_));
    counts_.fill(0);
    std::copy_n(counts.begin(), count, counts_.begin());
    total_ = 0;
    for (std::size_t index = 0; index < count; ++index)
        total_ += counts_[index];
    absent_ = false;
    setAccessibleDescription(summary());
    update();
}

void KHistogramChart::setAbsent() {
    if (absent_)
        return;
    absent_ = true;
    counts_.fill(0);
    total_ = 0;
    setAccessibleDescription(summary());
    update();
}

int KHistogramChart::percentileBucket(const double fraction) const {
    if (absent_ || total_ == 0)
        return -1;
    const auto threshold = static_cast<double>(total_) * std::clamp(fraction, 0.0, 1.0);
    std::uint64_t running = 0;
    for (int bucket = 0; bucket < bucketCount_; ++bucket) {
        running += counts_[static_cast<std::size_t>(bucket)];
        if (static_cast<double>(running) >= threshold && running > 0)
            return bucket;
    }
    return bucketCount_ - 1;
}

QString KHistogramChart::edgeText(const int bucket) const {
    if (bucket < 0)
        return absentMetricText();
    if (bucket >= bucketCount_ - 1)
        return tr("≥ %1").arg(
            formatMetric(upperEdges_[static_cast<std::size_t>(bucketCount_ - 2)], unit_));
    return tr("≤ %1").arg(formatMetric(upperEdges_[static_cast<std::size_t>(bucket)], unit_));
}

QString KHistogramChart::summary() const {
    if (absent_)
        return absentMetricText();
    if (total_ == 0)
        return tr("No samples");
    return tr("p50 %1 · p95 %2 · %3 samples")
        .arg(edgeText(percentileBucket(0.5)), edgeText(percentileBucket(0.95)),
             formatMetric(static_cast<double>(total_), MetricUnit::Count));
}

QSize KHistogramChart::sizeHint() const {
    return {px(Size::ReadoutColumnWidth), px(Size::HistogramHeight)};
}

QSize KHistogramChart::minimumSizeHint() const {
    return {px(Size::ReadoutValueWidth), px(Size::HistogramHeight)};
}

void KHistogramChart::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), color(Color::Background));
    painter.setRenderHint(QPainter::Antialiasing);
    paintFrame(painter, *this);
    const QRectF plot = plotRect(*this);
    const QFont small = kit::font(TypeRole::UiSmall);
    const QFontMetricsF metrics(small);
    const qreal line = metrics.height();
    painter.setFont(small);
    const QRectF header(plot.left(), plot.top(), plot.width(), line);
    painter.setPen(color(Color::Foreground));
    painter.drawText(header, Qt::AlignLeft | Qt::AlignVCenter, title_);
    painter.setPen(color(absent_ ? Color::Faint : Color::Muted));
    painter.drawText(
        header, Qt::AlignRight | Qt::AlignVCenter,
        metrics.elidedText(summary(), Qt::ElideLeft,
                           plot.width() - metrics.horizontalAdvance(title_) - px(Spacing::S)));
    const QRectF bars(plot.left(), header.bottom() + px(Spacing::XXS), plot.width(),
                      plot.height() - line * 2 - px(Spacing::XXS) * 2);
    const QRectF footer(plot.left(), bars.bottom() + px(Spacing::XXS), plot.width(), line);
    if (absent_ || bucketCount_ == 0) {
        painter.setPen(color(Color::Faint));
        painter.drawText(bars, Qt::AlignCenter, absentMetricText());
        return;
    }
    std::uint64_t highest = 0;
    for (int bucket = 0; bucket < bucketCount_; ++bucket)
        highest = std::max(highest, counts_[static_cast<std::size_t>(bucket)]);
    const qreal pitch = bars.width() / bucketCount_;
    const qreal gap = std::min<qreal>(px(Spacing::XXS), pitch / 4.0);
    const int median = percentileBucket(0.5);
    applyHairlinePen(painter, color(Color::Border));
    painter.drawLine(bars.bottomLeft(), bars.bottomRight());
    painter.setPen(Qt::NoPen);
    for (int bucket = 0; bucket < bucketCount_; ++bucket) {
        const auto count = counts_[static_cast<std::size_t>(bucket)];
        if (count == 0 || highest == 0)
            continue;
        const qreal height =
            bars.height() * static_cast<double>(count) / static_cast<double>(highest);
        const QRectF bar(bars.left() + pitch * bucket + gap / 2.0, bars.bottom() - height,
                         pitch - gap, height);
        painter.fillRect(snappedToDevicePixels(painter, bar),
                         color(bucket == median ? Color::Foreground : Color::Muted));
    }
    painter.setPen(color(Color::Faint));
    const auto edgeLabel = [&](const int edge) {
        return formatMetric(upperEdges_[static_cast<std::size_t>(edge)], unit_);
    };
    painter.drawText(footer, Qt::AlignLeft | Qt::AlignVCenter, edgeLabel(0));
    painter.drawText(footer, Qt::AlignHCenter | Qt::AlignVCenter,
                     edgeLabel((bucketCount_ - 2) / 2));
    painter.drawText(footer, Qt::AlignRight | Qt::AlignVCenter, edgeLabel(bucketCount_ - 2));
}

} // namespace bloom::ui::kit
