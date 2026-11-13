#include <bloom/ui/scopes_view.hpp>

#include <bloom/render/scope_table_layout.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QFontMetricsF>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace bloom::ui {
namespace {

namespace render = bloom::render;
namespace scopes_rt = bloom::runtime::scopes;
using kit::Color;

constexpr qreal kBehindOpacity = 0.5;

[[nodiscard]] QFont labelFont() { return kit::font(kit::TypeRole::Value); }
[[nodiscard]] QFont nameFont() { return kit::font(kit::TypeRole::UiSmall); }

[[nodiscard]] QColor qcolor(const std::uint32_t argb) { return QColor::fromRgb(argb); }

[[nodiscard]] QPen hairline(const QColor& color, const qreal width) {
    QPen pen(color, width);
    pen.setCapStyle(Qt::FlatCap);
    return pen;
}

constexpr std::array<const char*, 4> kChannelLetters{"R", "G", "B", "Y"};

[[nodiscard]] QString percentText(const double value) { return QString::number(value, 'f', 1); }

[[nodiscard]] QRectF toQRectF(const scopes_rt::Rect& rect) {
    return {rect.x, rect.y, rect.width, rect.height};
}
[[nodiscard]] QPointF toQPointF(const scopes_rt::Point& point) { return {point.x, point.y}; }

} // namespace

scopes_rt::ScopePalette scopePanelPalette() {
    scopes_rt::ScopePalette palette;
    palette.canvas = kit::color(Color::Canvas).rgb();
    palette.quiet = kit::color(Color::Border).rgb();
    palette.stronger = kit::color(Color::BorderHover).rgb();
    palette.strongest = kit::color(Color::Foreground).rgb();
    palette.label = kit::color(Color::Muted).rgb();
    palette.faint = kit::color(Color::Faint).rgb();
    palette.warning = kit::color(Color::Warn).rgb();
    palette.channel = {kit::color(Color::ComponentX).rgb(), kit::color(Color::ComponentY).rgb(),
                       kit::color(Color::ComponentZ).rgb(), kit::color(Color::Foreground).rgb()};
    return palette;
}

scopes_rt::ScopeMetrics scopePanelMetrics() {
    scopes_rt::ScopeMetrics metrics;
    metrics.padding = kit::px(kit::Spacing::S);
    metrics.small = kit::px(kit::Spacing::XS);
    metrics.tiny = kit::px(kit::Spacing::XXS);
    metrics.majorTick = kit::px(kit::Size::MajorTick);
    metrics.minorTick = kit::px(kit::Size::MinorTick);
    return metrics;
}

ScopeView::ScopeView(const scopes::Kind kind, ScopesTraceRenderer* const renderer,
                     QWidget* const parent)
    : QWidget(parent), kind_(kind), renderer_(renderer) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    static constexpr std::array<const char*, scopes::kKindCount> names{
        "scopesWaveform", "scopesParade", "scopesVectorscope", "scopesHistogram",
        "scopesChromaticity"};
    setObjectName(QString::fromLatin1(names[static_cast<std::size_t>(kind)]));
    if (renderer_ != nullptr)
        connect(renderer_, &ScopesTraceRenderer::traceReady, this, [this](const int ready) {
            if (ready == static_cast<int>(kind_))
                adoptFinished();
        });
}

ScopeView::~ScopeView() = default;

bool ScopeView::measured() const {
    if (data_.scopes == nullptr || data_.scopes->tables.empty())
        return false;
    const auto& layout = data_.scopes->tables.layout();
    switch (kind_) {
    case scopes::Kind::Waveform:
    case scopes::Kind::Parade:
        return layout.has(render::kScopeTableWaveform);
    case scopes::Kind::Vectorscope:
        return layout.has(render::kScopeTableVectorscope);
    case scopes::Kind::Histogram:
        return layout.has(render::kScopeTableHistogram);
    case scopes::Kind::Chromaticity:
        return layout.has(render::kScopeTableChromaticity);
    }
    return false;
}

bool ScopeView::showsBehind() const {
    if (trace_.isNull())
        return false;
    const auto* current = data_.scopes != nullptr ? &data_.scopes->tables : nullptr;
    return data_.behind || traceTables_ != current;
}

void ScopeView::setData(const ScopeViewData& data) {
    data_ = data;
    if (!measured()) {
        trace_ = {};
        clip_ = {};
        traceTables_ = nullptr;
        requestedSize_ = {};
        if (renderer_ != nullptr)
            renderer_->clear(kind_);
    } else {
        requestTrace();
    }
    update();
}

void ScopeView::setGain(const float gain) {
    if (gain_ == gain)
        return;
    gain_ = gain;
    requestTrace();
}

void ScopeView::setWaveformChannels(const scopes::WaveformChannels channels) {
    if (waveformChannels_ == channels)
        return;
    waveformChannels_ = channels;
    requestTrace();
    update();
}

void ScopeView::setHistogramChannels(const scopes::HistogramChannels channels) {
    if (histogramChannels_ == channels)
        return;
    histogramChannels_ = channels;
    updateGeometry();
    requestTrace();
    update();
}

void ScopeView::setHistogramScale(const scopes::HistogramScale scale) {
    if (histogramScale_ == scale)
        return;
    histogramScale_ = scale;
    requestTrace();
}

void ScopeView::setActive(const bool active) {
    if (active_ == active)
        return;
    active_ = active;
    if (active_)
        requestTrace();
    else if (renderer_ != nullptr) {
        requestedSize_ = {};
        renderer_->clear(kind_);
    }
}

scopes_rt::GraticuleRequest ScopeView::planRequest(const int width, const int height,
                                                   const bool withClip) const {
    scopes_rt::GraticuleRequest request;
    request.kind = kind_;
    request.width = width;
    request.height = height;
    request.devicePixelRatio = devicePixelRatioF();
    request.signal = data_.signal;
    request.axisMaximum = data_.axisMaximum;
    request.waveformChannels = waveformChannels_;
    request.histogramChannels = histogramChannels_;
    request.histogramScale = histogramScale_;
    if (data_.color != nullptr) {
        request.luma = &data_.color->luma();
        request.colorimetry = &data_.color->colorimetry();
    }
    if (withClip)
        request.clip = &clip_;
    request.metrics = scopePanelMetrics();
    request.measure = [numeric = QFontMetricsF(labelFont()), name = QFontMetricsF(nameFont())](
                          const std::string_view text, const scopes_rt::LabelFont font) {
        const auto& metrics = font == scopes_rt::LabelFont::Numeric ? numeric : name;
        return scopes_rt::TextSize{metrics.horizontalAdvance(QString::fromUtf8(
                                       text.data(), static_cast<qsizetype>(text.size()))),
                                   metrics.height()};
    };
    return request;
}

scopes_rt::ScopeGraticule ScopeView::plan(const int width, const int height,
                                          const bool withClip) const {
    return scopes_rt::buildScopeGraticule(planRequest(width, height, withClip));
}

scopes_rt::ScopeGraticule ScopeView::graticule() const { return plan(width(), height(), true); }

QRectF ScopeView::plotRect() const { return toQRectF(plan(width(), height()).plot); }

scopes::TraceRequest ScopeView::currentRequest() const {
    scopes::TraceRequest request;
    request.kind = kind_;
    if (data_.scopes != nullptr)
        request.tables =
            std::shared_ptr<const runtime::ScopeTables>(data_.scopes, &data_.scopes->tables);
    request.color = data_.color;
    request.colors = scopePanelPalette().traceColors();
    request.gain = gain_;
    const auto traceSize = plan(width(), height()).traceRequestSize;
    request.size = QSize(traceSize.width, traceSize.height);
    request.gapPixels = static_cast<int>(std::lround(scopes_rt::kParadeGap * devicePixelRatioF()));
    request.waveformChannels = waveformChannels_;
    request.histogramChannels = histogramChannels_;
    request.histogramScale = histogramScale_;
    return request;
}

void ScopeView::requestTrace() {
    if (renderer_ == nullptr || !active_ || !measured() || !isVisible() || width() <= 0 ||
        height() <= 0)
        return;
    const auto request = currentRequest();
    requestedSize_ = request.size;
    renderer_->request(request);
}

void ScopeView::adoptFinished() {
    const auto finished = renderer_->finished(kind_);
    if (finished == nullptr || data_.scopes == nullptr ||
        finished->request.tables.get() != &data_.scopes->tables)
        return;
    trace_ = finished->trace.image;
    clip_ = finished->trace.clip;
    traceTables_ = finished->request.tables.get();
    update();
}

void ScopeView::resizeEvent(QResizeEvent* const event) {
    QWidget::resizeEvent(event);
    const auto traceSize = plan(width(), height()).traceRequestSize;
    if (measured() && QSize(traceSize.width, traceSize.height) != requestedSize_)
        requestTrace();
}

void ScopeView::showEvent(QShowEvent* const event) {
    QWidget::showEvent(event);
    requestTrace();
}

int ScopeView::heightForWidth(const int width) const {
    return scopes_rt::scopeHeightForWidth(planRequest(width, 0, false), width);
}

QSize ScopeView::sizeHint() const {
    const int width = kit::px(kit::Size::PanelMinWidth);
    return {width, heightForWidth(width)};
}

QSize ScopeView::minimumSizeHint() const {
    const int width = kit::px(kit::Size::PanelMinWidth) - 2 * kit::px(kit::Spacing::SectionPadding);
    return {width, heightForWidth(width)};
}

QString ScopeView::axisLabel(const int code) const {
    return QString::fromStdString(scopes_rt::scopeAxisLabel(code, data_.signal, data_.axisMaximum));
}

void ScopeView::paintTrace(QPainter& painter, const QRectF& target, const bool additive) const {
    if (trace_.isNull())
        return;
    painter.save();
    if (showsBehind())
        painter.setOpacity(kBehindOpacity);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    if (additive)
        painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.drawImage(target, trace_);
    painter.restore();
}

void ScopeView::paintEvent(QPaintEvent* const event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const auto palette = scopePanelPalette();
    painter.fillRect(rect(), qcolor(palette.canvas));
    const auto graticule = plan(width(), height(), true);
    paintPlan(painter, graticule, palette);
    if (showsBehind()) {
        painter.setFont(nameFont());
        painter.setPen(qcolor(palette.warning));
        painter.drawText(toQRectF(graticule.plot)
                             .adjusted(kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XS),
                                       -kit::px(kit::Spacing::XS), 0),
                         Qt::AlignLeft | Qt::AlignTop, tr("Behind"));
    }
}

// Paints the plan with the vector painter and the interface fonts: the graticule under the trace,
// the trace, then the lines, polylines, circles, fills and labels over it.
void ScopeView::paintPlan(QPainter& painter, const scopes_rt::ScopeGraticule& graticule,
                          const scopes_rt::ScopePalette& palette) const {
    const auto paintShapes = [&](const scopes_rt::Layer layer) {
        painter.setBrush(Qt::NoBrush);
        for (const auto& line : graticule.lines) {
            if (line.layer != layer)
                continue;
            painter.setRenderHint(QPainter::Antialiasing, line.antialiased);
            QPen pen = hairline(qcolor(palette.color(line.color)), graticule.hairlineWidth);
            if (line.dashed)
                pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            painter.drawLine(toQPointF(line.from), toQPointF(line.to));
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
        for (const auto& polyline : graticule.polylines) {
            if (polyline.layer != layer || polyline.points.empty())
                continue;
            QPainterPath path;
            path.moveTo(toQPointF(polyline.points.front()));
            for (std::size_t index = 1; index < polyline.points.size(); ++index)
                path.lineTo(toQPointF(polyline.points[index]));
            if (polyline.closed)
                path.closeSubpath();
            painter.setPen(
                hairline(qcolor(palette.color(polyline.color)), graticule.hairlineWidth));
            painter.drawPath(path);
        }
        for (const auto& circle : graticule.circles) {
            if (circle.layer != layer)
                continue;
            painter.setPen(hairline(qcolor(palette.color(circle.color)), graticule.hairlineWidth));
            painter.drawEllipse(toQPointF(circle.center), circle.radius, circle.radius);
        }
        painter.setRenderHint(QPainter::Antialiasing, false);
    };
    paintShapes(scopes_rt::Layer::UnderTrace);
    paintTrace(painter, toQRectF(graticule.trace),
               kind_ == scopes::Kind::Waveform || kind_ == scopes::Kind::Parade);
    paintShapes(scopes_rt::Layer::OverTrace);

    painter.setPen(Qt::NoPen);
    for (const auto& fill : graticule.fills) {
        painter.setBrush(qcolor(palette.color(fill.color)));
        painter.drawRect(toQRectF(fill.rect));
    }
    painter.setBrush(Qt::NoBrush);

    for (const auto& label : graticule.labels) {
        const auto font = label.font == scopes_rt::LabelFont::Numeric ? labelFont() : nameFont();
        const QFontMetricsF metrics(font);
        const auto text = QString::fromStdString(label.text);
        const qreal textWidth = metrics.horizontalAdvance(text);
        const qreal textHeight = metrics.height();
        const qreal left = label.anchor.x -
                           (label.horizontal == scopes_rt::HorizontalAlign::Left     ? 0.0
                            : label.horizontal == scopes_rt::HorizontalAlign::Center ? textWidth / 2
                                                                                     : textWidth);
        const qreal top = label.anchor.y -
                          (label.vertical == scopes_rt::VerticalAlign::Top ? 0.0 : textHeight / 2);
        painter.setFont(font);
        painter.setPen(qcolor(palette.color(label.color)));
        painter.drawText(QRectF(left, top, textWidth, textHeight),
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextDontClip, text);
    }
}

QString ScopeView::readoutAt(const QPointF& position) const {
    if (!measured())
        return {};
    const auto g = plan(width(), height());
    const auto& tables = data_.scopes->tables;
    const auto plot = toQRectF(g.plot);
    switch (kind_) {
    case scopes::Kind::Waveform:
    case scopes::Kind::Parade: {
        if (!plot.contains(position))
            return {};
        const bool parade = kind_ == scopes::Kind::Parade;
        const qreal gap = scopes_rt::kParadeGap;
        const qreal paneWidth = parade ? (plot.width() - 2 * gap) / 3 : plot.width();
        int pane = 0;
        qreal local = position.x() - plot.left();
        if (parade) {
            pane = std::clamp(static_cast<int>(local / (paneWidth + gap)), 0, 2);
            local -= pane * (paneWidth + gap);
            if (local < 0 || local > paneWidth)
                return {};
        }
        const auto columns = tables.layout().columns;
        const auto column = std::min<std::uint32_t>(
            columns - 1,
            static_cast<std::uint32_t>(std::max<qreal>(0, local) / paneWidth * columns));
        const int code = std::clamp(
            static_cast<int>((plot.bottom() - position.y()) / plot.height() * 1024.0), 0, 1023);
        const auto level = static_cast<std::uint32_t>(code >> 1);
        QStringList counts;
        if (parade) {
            counts << tr("%1 %2")
                          .arg(QString::fromLatin1(kChannelLetters[static_cast<std::size_t>(pane)]))
                          .arg(tables.waveform(static_cast<std::uint32_t>(pane), column, level));
        } else {
            for (const auto channel : scopes::waveformChannelList(waveformChannels_))
                counts << tr("%1 %2")
                              .arg(QString::fromLatin1(kChannelLetters[channel]))
                              .arg(tables.waveform(channel, column, level));
        }
        return tr("Column %1 · Code %2 (%3) · Count %4")
            .arg(column)
            .arg(code)
            .arg(axisLabel(code))
            .arg(counts.join(QStringLiteral(" · ")));
    }
    case scopes::Kind::Histogram: {
        if (!plot.contains(position))
            return {};
        const auto channels = scopes::histogramChannelList(histogramChannels_);
        const auto row = std::min<std::size_t>(
            channels.size() - 1,
            static_cast<std::size_t>((position.y() - plot.top()) / g.rowHeight));
        const auto channel = channels[row];
        const QString letter = QString::fromLatin1(kChannelLetters[channel]);
        const qreal local = position.x() - plot.left();
        if (local < scopes_rt::kClipMarkerWidth && clip_.under[channel] != 0)
            return tr("%1 under (below code 0) · Count %2").arg(letter).arg(clip_.under[channel]);
        if (local > plot.width() - scopes_rt::kClipMarkerWidth && clip_.over[channel] != 0)
            return tr("%1 over (above code 1023) · Count %2").arg(letter).arg(clip_.over[channel]);
        const int code = std::clamp(static_cast<int>(local / plot.width() * 1024.0), 0, 1023);
        return tr("%1 · Code %2 (%3) · Count %4")
            .arg(letter)
            .arg(code)
            .arg(axisLabel(code))
            .arg(tables.histogram(channel, static_cast<std::uint32_t>(code)));
    }
    case scopes::Kind::Vectorscope: {
        if (!plot.contains(position))
            return {};
        const double fx = (position.x() - plot.left()) / plot.width();
        const double fy = (position.y() - plot.top()) / plot.height();
        const double cb = fx - 0.5;
        const double cr = 0.5 - fy;
        const auto cell = [](const double value) {
            return static_cast<std::uint32_t>(
                std::clamp(std::floor((value + 0.5) * 512.0), 0.0, 511.0));
        };
        double angle = std::atan2(cr, cb) * 180.0 / std::numbers::pi;
        if (angle < 0)
            angle += 360.0;
        const double saturation = std::hypot(cb, cr) / 0.5 * 100.0;
        return tr("Cb %1 · Cr %2 · %3° · %4% saturation · Count %5")
            .arg(cb, 0, 'f', 3)
            .arg(cr, 0, 'f', 3)
            .arg(angle, 0, 'f', 1)
            .arg(percentText(saturation))
            .arg(tables.vectorscope(cell(cb), cell(cr)));
    }
    case scopes::Kind::Chromaticity: {
        if (!plot.contains(position))
            return {};
        const double x = (position.x() - plot.left()) / plot.width();
        const double y = (plot.bottom() - position.y()) / plot.height();
        std::uint32_t count = 0;
        if (x < scopes::kChromaticityXMaximum && y < scopes::kChromaticityYMaximum) {
            const auto xCell =
                static_cast<std::uint32_t>(x / scopes::kChromaticityXMaximum * 512.0);
            const auto yCell =
                static_cast<std::uint32_t>(y / scopes::kChromaticityYMaximum * 512.0);
            count = tables.chromaticity(std::min(xCell, 511U), std::min(yCell, 511U));
        }
        return tr("x %1 · y %2 · Count %3").arg(x, 0, 'f', 4).arg(y, 0, 'f', 4).arg(count);
    }
    }
    return {};
}

bool ScopeView::event(QEvent* const event) {
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const auto text = readoutAt(help->pos());
        if (text.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(help->globalPos(), text, this);
        return true;
    }
    return QWidget::event(event);
}

// The readout follows the pointer: it is the value under it, not a hint that waits for a pause.
void ScopeView::mouseMoveEvent(QMouseEvent* const event) {
    QWidget::mouseMoveEvent(event);
    const auto text = readoutAt(event->position());
    if (text.isEmpty())
        QToolTip::hideText();
    else
        QToolTip::showText(event->globalPosition().toPoint(), text, this);
}

void ScopeView::leaveEvent(QEvent* const event) {
    QWidget::leaveEvent(event);
    QToolTip::hideText();
}

} // namespace bloom::ui
