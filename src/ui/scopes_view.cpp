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
#include <vector>

namespace bloom::ui {
namespace {

namespace render = bloom::render;
namespace scopes_rt = bloom::runtime::scopes;
using kit::Color;

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
    // A black canvas, hairlines in BorderHover, labels in Muted (about twice the lines'
    // brightness).
    palette.canvas = kit::color(Color::CanvasBlack).rgb();
    palette.quiet = kit::color(Color::BorderHover).rgb();
    palette.stronger = kit::color(Color::BorderHover).rgb();
    palette.strongest = kit::color(Color::Foreground).rgb();
    palette.label = kit::color(Color::Muted).rgb();
    palette.faint = kit::color(Color::Faint).rgb();
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

bool ScopeView::measures(const runtime::PreviewFrameScopes* const scopes) const {
    if (scopes == nullptr || scopes->status != runtime::PreviewFrameScopesStatus::Measured ||
        scopes->tables.empty())
        return false;
    const auto& layout = scopes->tables.layout();
    switch (kind_) {
    case scopes::Kind::Waveform:
    case scopes::Kind::Parade:
        return layout.has(render::kScopeTableWaveform);
    case scopes::Kind::Vectorscope:
        return layout.has(render::kScopeTableVectorscope) && scopes->color != nullptr;
    case scopes::Kind::Histogram:
        return layout.has(render::kScopeTableHistogram);
    case scopes::Kind::Chromaticity:
        return layout.has(render::kScopeTableChromaticity);
    }
    return false;
}

void ScopeView::setScopes(std::shared_ptr<const runtime::PreviewFrameScopes> scopes) {
    if (scopes_ == scopes)
        return;
    scopes_ = std::move(scopes);
    if (measures(scopes_.get())) {
        // The trace on show stays, at full strength, until the new one replaces it.
        requestTrace();
        if (trace_.isNull() && shown_ == nullptr) {
            // Nothing on show yet: the graticule follows the new tables at once.
            shown_ = scopes_;
            update();
        }
        return;
    }
    // Nothing to count for this frame (no frame, an empty region, tables that could not be
    // counted): the empty graticule, at once and silently.
    if (renderer_ != nullptr)
        renderer_->clear(kind_);
    requestedSize_ = {};
    trace_ = {};
    shown_ = scopes_;
    shownDrawing_ = drawing_;
    update();
}

void ScopeView::setDrawing(const ScopesRegionStore::Drawing& drawing) {
    if (drawing_ == drawing)
        return;
    const bool rows = drawing_.histogramChannels != drawing.histogramChannels;
    drawing_ = drawing;
    if (rows)
        updateGeometry();
    if (measures(scopes_.get()))
        requestTrace();
    else {
        shownDrawing_ = drawing_;
        update();
    }
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

scopes_rt::GraticuleRequest
ScopeView::planRequest(const int width, const int height,
                       const runtime::PreviewFrameScopes* const scopes) const {
    scopes_rt::GraticuleRequest request;
    request.kind = kind_;
    request.width = width;
    request.height = height;
    request.devicePixelRatio = devicePixelRatioF();
    if (scopes != nullptr) {
        request.signal = scopes->settings.signal;
        request.axisMaximum = scopes->settings.axisMaximum;
        if (scopes->color != nullptr) {
            request.luma = &scopes->color->luma();
            request.colorimetry = &scopes->color->colorimetry();
        }
    }
    request.waveformChannels = shownDrawing_.waveformChannels;
    request.histogramChannels = shownDrawing_.histogramChannels;
    request.histogramScale = shownDrawing_.histogramScale;
    request.metrics = scopePanelMetrics();
    request.measure = textMeasure();
    return request;
}

scopes_rt::MeasureText ScopeView::textMeasure() {
    return [numeric = QFontMetricsF(labelFont()), name = QFontMetricsF(nameFont())](
               const std::string_view text, const scopes_rt::LabelFont font) {
        const auto& metrics = font == scopes_rt::LabelFont::Numeric ? numeric : name;
        return scopes_rt::TextSize{metrics.horizontalAdvance(QString::fromUtf8(
                                       text.data(), static_cast<qsizetype>(text.size()))),
                                   metrics.height()};
    };
}

scopes_rt::ScopeGraticule ScopeView::plan(const int width, const int height) const {
    return scopes_rt::buildScopeGraticule(planRequest(width, height, shown_.get()));
}

scopes_rt::ScopeGraticule ScopeView::graticule() const { return plan(width(), height()); }

QRectF ScopeView::plotRect() const { return toQRectF(plan(width(), height()).plot); }

scopes::TraceRequest ScopeView::currentRequest() const {
    scopes::TraceRequest request;
    request.kind = kind_;
    if (scopes_ != nullptr) {
        request.tables = std::shared_ptr<const runtime::ScopeTables>(scopes_, &scopes_->tables);
        request.color = scopes_->color;
    }
    // The size of the plot the new tables will be drawn in: the graticule of the target drawing.
    auto target = planRequest(width(), height(), scopes_.get());
    target.waveformChannels = drawing_.waveformChannels;
    target.histogramChannels = drawing_.histogramChannels;
    target.histogramScale = drawing_.histogramScale;
    const auto traceSize = scopes_rt::buildScopeGraticule(target).traceRequestSize;
    request.size = QSize(traceSize.width, traceSize.height);
    request.gapPixels = static_cast<int>(std::lround(scopes_rt::kParadeGap * devicePixelRatioF()));
    request.waveformChannels = drawing_.waveformChannels;
    request.histogramChannels = drawing_.histogramChannels;
    request.histogramScale = drawing_.histogramScale;
    return request;
}

void ScopeView::requestTrace() {
    if (renderer_ == nullptr || !active_ || !measures(scopes_.get()) || !isVisible() ||
        width() <= 0 || height() <= 0)
        return;
    const auto request = currentRequest();
    if (request.size.isEmpty())
        return;
    requestedSize_ = request.size;
    renderer_->request(request);
}

void ScopeView::adoptFinished() {
    const auto finished = renderer_->finished(kind_);
    if (finished == nullptr || finished->image.isNull())
        return;
    // One swap: the image, the tables it was mapped from and the modes it was mapped with.
    const auto& request = finished->request;
    if (request.tables == nullptr)
        return;
    std::shared_ptr<const runtime::PreviewFrameScopes> owner;
    if (scopes_ != nullptr && request.tables.get() == &scopes_->tables)
        owner = scopes_;
    else if (shown_ != nullptr && request.tables.get() == &shown_->tables)
        owner = shown_;
    else
        return;
    trace_ = finished->image;
    shown_ = std::move(owner);
    shownDrawing_ = {request.waveformChannels, request.histogramChannels, request.histogramScale};
    update();
}

void ScopeView::resizeEvent(QResizeEvent* const event) {
    QWidget::resizeEvent(event);
    const auto request = currentRequest();
    if (measures(scopes_.get()) && request.size != requestedSize_)
        requestTrace();
}

void ScopeView::showEvent(QShowEvent* const event) {
    QWidget::showEvent(event);
    requestTrace();
}

int ScopeView::heightForWidth(const int width) const {
    auto request = planRequest(width, 0, nullptr);
    request.histogramChannels = drawing_.histogramChannels;
    return scopes_rt::scopeHeightForWidth(request, width);
}

QSize ScopeView::sizeHint() const {
    const int width = kit::px(kit::Size::PanelMinWidth);
    return {width, heightForWidth(width)};
}

QSize ScopeView::minimumSizeHint() const {
    const int width = kit::px(kit::Size::PanelMinWidth) - kit::px(kit::Spacing::SectionPadding) -
                      kit::px(kit::Spacing::SectionPadding);
    return {width, heightForWidth(width)};
}

QString ScopeView::axisLabel(const int code) const {
    const auto signal = shown_ != nullptr ? shown_->settings.signal : runtime::ScopeSignal::Display;
    const auto maximum = shown_ != nullptr ? shown_->settings.axisMaximum : 1U;
    return QString::fromStdString(scopes_rt::scopeAxisLabel(code, signal, maximum));
}

void ScopeView::paintEvent(QPaintEvent* const event) {
    Q_UNUSED(event)
    QPainter painter(this);
    const auto palette = scopePanelPalette();
    painter.fillRect(rect(), qcolor(palette.canvas));
    paintPlan(painter, plan(width(), height()), palette);
}

// Paints the plan with the vector painter and the interface fonts: the graticule under the trace,
// the trace, then the lines, polylines, circles and labels over it.
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
    if (!trace_.isNull()) {
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        if (scopes::additiveTrace(kind_))
            painter.setCompositionMode(QPainter::CompositionMode_Plus);
        painter.drawImage(toQRectF(graticule.trace), trace_);
        painter.restore();
    }
    paintShapes(scopes_rt::Layer::OverTrace);

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
    if (!measures(shown_.get()) || trace_.isNull())
        return {};
    const auto g = plan(width(), height());
    const auto& tables = shown_->tables;
    const auto plot = toQRectF(g.plot);
    if (!plot.contains(position))
        return {};
    switch (kind_) {
    case scopes::Kind::Waveform:
    case scopes::Kind::Parade: {
        const bool parade = kind_ == scopes::Kind::Parade;
        const qreal gap = scopes_rt::kParadeGap;
        const qreal paneWidth = parade ? (plot.width() - gap - gap) / 3 : plot.width();
        int pane = 0;
        qreal local = position.x() - plot.left();
        if (parade) {
            pane = std::clamp(static_cast<int>(local / (paneWidth + gap)), 0, 2);
            local -= pane * (paneWidth + gap);
            if (local < 0 || local > paneWidth)
                return {};
        }
        const auto columns = tables.layout().columns;
        if (columns == 0)
            return {};
        const auto column = std::min<std::uint32_t>(
            columns - 1,
            static_cast<std::uint32_t>(std::max<qreal>(0, local) / paneWidth * columns));
        const int code = std::clamp(
            static_cast<int>((plot.bottom() - position.y()) / plot.height() * 1024.0), 0, 1023);
        const auto level = static_cast<std::uint32_t>(code);
        QStringList counts;
        const auto channels = parade ? std::vector<std::uint32_t>{static_cast<std::uint32_t>(pane)}
                                     : scopes::waveformChannelList(shownDrawing_.waveformChannels);
        for (const auto channel : channels)
            counts << tr("%1 %2")
                          .arg(QString::fromLatin1(kChannelLetters[channel]))
                          .arg(tables.waveform(channel, column, level));
        return tr("Column %1 · Code %2 (%3) · Count %4")
            .arg(column)
            .arg(code)
            .arg(axisLabel(code))
            .arg(counts.join(QStringLiteral(" · ")));
    }
    case scopes::Kind::Histogram: {
        const auto channels = scopes::histogramChannelList(shownDrawing_.histogramChannels);
        if (channels.empty() || !(g.rowHeight > 0.0))
            return {};
        const auto row = std::min<std::size_t>(
            channels.size() - 1,
            static_cast<std::size_t>((position.y() - plot.top()) / g.rowHeight));
        const auto channel = channels[row];
        const QString letter = QString::fromLatin1(kChannelLetters[channel]);
        const qreal local = position.x() - plot.left();
        const int code = std::clamp(static_cast<int>(local / plot.width() * 1024.0), 0, 1023);
        auto text = tr("%1 · Code %2 (%3) · Count %4")
                        .arg(letter)
                        .arg(code)
                        .arg(axisLabel(code))
                        .arg(tables.histogram(channel, static_cast<std::uint32_t>(code)));
        // Values outside the axis are drawn in its end bins; the readout names them.
        if (code == 0 && tables.histogram(channel, render::kScopeCodeUnder) != 0)
            text += tr(" · Under %1").arg(tables.histogram(channel, render::kScopeCodeUnder));
        if (code == 1023 && tables.histogram(channel, render::kScopeCodeOver) != 0)
            text += tr(" · Over %1").arg(tables.histogram(channel, render::kScopeCodeOver));
        return text;
    }
    case scopes::Kind::Vectorscope: {
        const double extent = scopes_rt::kVectorscopeExtent;
        const double cb = ((position.x() - plot.left()) / plot.width() * 2.0 - 1.0) * extent;
        const double cr = (1.0 - (position.y() - plot.top()) / plot.height() * 2.0) * extent;
        const double side = render::kScopeVectorscopeCells;
        const auto cell = [side](const double value) {
            return static_cast<std::uint32_t>(
                std::clamp(std::floor((value + scopes_rt::kVectorscopeExtent) *
                                      render::kScopeVectorscopeCellsPerUnit),
                           0.0, side - 1.0));
        };
        double angle = std::atan2(cr, cb) * 180.0 / std::numbers::pi;
        if (angle < 0)
            angle += 360.0;
        const double saturation = std::hypot(cb, cr) / runtime::kScopeVectorscopeRing * 100.0;
        return tr("Cb %1 · Cr %2 · %3° · %4% saturation · Count %5")
            .arg(cb, 0, 'f', 3)
            .arg(cr, 0, 'f', 3)
            .arg(angle, 0, 'f', 1)
            .arg(percentText(saturation))
            .arg(tables.vectorscope(cell(cb), cell(cr)));
    }
    case scopes::Kind::Chromaticity: {
        const double x = (position.x() - plot.left()) / plot.width();
        const double y = (plot.bottom() - position.y()) / plot.height();
        std::uint32_t count = 0;
        if (x < scopes::kChromaticityXMaximum && y < scopes::kChromaticityYMaximum) {
            constexpr auto cells = render::kScopeChromaticityCells;
            const double side = static_cast<double>(cells);
            const auto xCell = static_cast<std::uint32_t>(x / scopes::kChromaticityXMaximum * side);
            const auto yCell = static_cast<std::uint32_t>(y / scopes::kChromaticityYMaximum * side);
            count = tables.chromaticity(std::min(xCell, cells - 1U), std::min(yCell, cells - 1U));
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
