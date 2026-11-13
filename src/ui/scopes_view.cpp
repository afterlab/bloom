#include <bloom/ui/scopes_view.hpp>

#include <bloom/color/scope_colorimetry.hpp>
#include <bloom/render/scope_table_layout.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QHelpEvent>
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
using kit::Color;

constexpr std::array<int, 9> kLabelCodes{0, 128, 256, 384, 512, 640, 768, 896, 1023};
// Logical pixels of one Histogram row, a Parade gap, and the clip markers.
constexpr int kHistogramRowHeight = 56;
constexpr int kParadeGap = 4;
constexpr int kClipMarkerWidth = 3;
constexpr qreal kBehindOpacity = 0.5;

[[nodiscard]] QFont labelFont() { return kit::font(kit::TypeRole::Value); }
[[nodiscard]] QFont nameFont() { return kit::font(kit::TypeRole::UiSmall); }

// The position of a code on an axis: a code covers 1/1024 of it, so its center is (code + 0.5).
[[nodiscard]] qreal codeFraction(const int code) {
    return (static_cast<qreal>(code) + 0.5) / 1024.0;
}

[[nodiscard]] qreal snap(const qreal value, const qreal dpr) {
    return (std::floor(value * dpr) + 0.5) / dpr;
}

[[nodiscard]] QPen hairline(const QColor& color, const qreal dpr) {
    QPen pen(color, kit::snappedHairlineWidth(dpr));
    pen.setCapStyle(Qt::FlatCap);
    return pen;
}

struct Tint final {
    Color token;
    const char* letter;
};
constexpr std::array<Tint, 4> kChannelTints{{{Color::ComponentX, "R"},
                                             {Color::ComponentY, "G"},
                                             {Color::ComponentZ, "B"},
                                             {Color::Foreground, "Y"}}};

[[nodiscard]] QString percentText(const double value) { return QString::number(value, 'f', 1); }

// The 75% color bars' positions on the Vectorscope with the signal's own coefficients.
struct Target final {
    const char* name;
    float red;
    float green;
    float blue;
};
constexpr std::array<Target, 6> kTargets{{{"R", 0.75F, 0.0F, 0.0F},
                                          {"Mg", 0.75F, 0.0F, 0.75F},
                                          {"B", 0.0F, 0.0F, 0.75F},
                                          {"Cy", 0.0F, 0.75F, 0.75F},
                                          {"G", 0.0F, 0.75F, 0.0F},
                                          {"Yl", 0.75F, 0.75F, 0.0F}}};
constexpr double kSkinToneDegrees = 123.0;

[[nodiscard]] QPointF vectorscopePoint(const runtime::ScopeLuma& luma, const Target& target) {
    const float y = (luma.red * target.red + luma.green * target.green) + luma.blue * target.blue;
    const float cb = (target.blue - y) / luma.cbDenominator;
    const float cr = (target.red - y) / luma.crDenominator;
    return {static_cast<qreal>(cb), static_cast<qreal>(cr)};
}

} // namespace

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

ScopeView::Geometry ScopeView::geometry(const int width, const int height) const {
    const QFontMetricsF metrics(labelFont());
    const qreal pad = kit::px(kit::Spacing::S);
    const qreal small = kit::px(kit::Spacing::XS);
    const qreal tick = kit::px(kit::Size::MajorTick);
    Geometry result;
    switch (kind_) {
    case scopes::Kind::Waveform:
    case scopes::Kind::Parade: {
        const qreal left = metrics.horizontalAdvance(QStringLiteral("1023")) + small * 2;
        const qreal top = std::ceil(metrics.height() / 2) + kit::px(kit::Spacing::XXS);
        result.plot = QRectF(left, top, std::max<qreal>(1, width - left - pad),
                             std::max<qreal>(1, height - top - tick - small));
        result.image = result.plot;
        break;
    }
    case scopes::Kind::Histogram: {
        result.rows = static_cast<int>(scopes::histogramChannelList(histogramChannels_).size());
        result.rowHeight = kHistogramRowHeight;
        const qreal top = std::ceil(metrics.height()) + small * 2;
        result.plot =
            QRectF(pad, top, std::max<qreal>(1, width - pad * 2), result.rowHeight * result.rows);
        result.image = result.plot;
        break;
    }
    case scopes::Kind::Vectorscope: {
        const qreal side = std::max<qreal>(1, std::min(width, height) - pad * 2);
        result.plot = QRectF((width - side) / 2, (height - side) / 2, side, side);
        result.image = result.plot;
        break;
    }
    case scopes::Kind::Chromaticity: {
        const qreal left = metrics.horizontalAdvance(QStringLiteral("1.0")) + small * 2;
        const qreal bottom = std::ceil(metrics.height()) + small * 2;
        const qreal side = std::max<qreal>(1, std::min(width - left - pad, height - bottom - pad));
        result.plot = QRectF(left + (width - left - pad - side) / 2, pad, side, side);
        result.image =
            QRectF(result.plot.left(), result.plot.bottom() - side * scopes::kChromaticityYMaximum,
                   side * scopes::kChromaticityXMaximum, side * scopes::kChromaticityYMaximum);
        break;
    }
    }
    return result;
}

QRectF ScopeView::plotRect() const { return geometry(width(), height()).plot; }

QSize ScopeView::traceSizeFor(const Geometry& geometry) const {
    const qreal dpr = devicePixelRatioF();
    const auto pixels = [dpr](const qreal logical) {
        return std::max(1, static_cast<int>(std::lround(logical * dpr)));
    };
    switch (kind_) {
    case scopes::Kind::Histogram:
        return {pixels(geometry.plot.width()), pixels(geometry.rowHeight)};
    case scopes::Kind::Chromaticity:
        return {pixels(geometry.plot.width()), pixels(geometry.plot.height())};
    case scopes::Kind::Waveform:
    case scopes::Kind::Parade:
    case scopes::Kind::Vectorscope:
        break;
    }
    return {pixels(geometry.plot.width()), pixels(geometry.plot.height())};
}

scopes::TraceRequest ScopeView::currentRequest() const {
    scopes::TraceRequest request;
    request.kind = kind_;
    if (data_.scopes != nullptr)
        request.tables =
            std::shared_ptr<const runtime::ScopeTables>(data_.scopes, &data_.scopes->tables);
    request.color = data_.color;
    for (std::size_t channel = 0; channel < 4; ++channel)
        request.colors.channel[channel] = kit::color(kChannelTints[channel].token).rgb();
    request.gain = gain_;
    request.size = traceSizeFor(geometry(width(), height()));
    request.gapPixels = static_cast<int>(std::lround(kParadeGap * devicePixelRatioF()));
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
    if (measured() && traceSizeFor(geometry(width(), height())) != requestedSize_)
        requestTrace();
}

void ScopeView::showEvent(QShowEvent* const event) {
    QWidget::showEvent(event);
    requestTrace();
}

int ScopeView::heightForWidth(const int width) const {
    switch (kind_) {
    case scopes::Kind::Waveform:
    case scopes::Kind::Parade:
        return std::clamp(width / 2, 170, 380);
    case scopes::Kind::Vectorscope:
        return std::clamp(width, 240, 480);
    case scopes::Kind::Chromaticity:
        return std::clamp(width, 280, 480);
    case scopes::Kind::Histogram:
        return static_cast<int>(std::ceil(geometry(width, 10000).plot.bottom())) +
               kit::px(kit::Spacing::S);
    }
    return 200;
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
    if (data_.signal == runtime::ScopeSignal::Display)
        return QString::number(code);
    return QString::number(static_cast<double>(code) / 1023.0 * data_.axisMaximum, 'g', 3);
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
    painter.fillRect(rect(), kit::color(Color::Canvas));
    const auto g = geometry(width(), height());
    painter.setFont(labelFont());
    switch (kind_) {
    case scopes::Kind::Waveform:
    case scopes::Kind::Parade:
        paintWaveform(painter, g);
        break;
    case scopes::Kind::Vectorscope:
        paintVectorscope(painter, g);
        break;
    case scopes::Kind::Histogram:
        paintHistogram(painter, g);
        break;
    case scopes::Kind::Chromaticity:
        paintChromaticity(painter, g);
        break;
    }
    if (showsBehind()) {
        painter.setFont(nameFont());
        painter.setPen(kit::color(Color::Warn));
        painter.drawText(g.plot.adjusted(kit::px(kit::Spacing::XS), kit::px(kit::Spacing::XS),
                                         -kit::px(kit::Spacing::XS), 0),
                         Qt::AlignLeft | Qt::AlignTop, tr("Behind"));
    }
}

void ScopeView::paintWaveform(QPainter& painter, const Geometry& g) const {
    const qreal dpr = devicePixelRatioF();
    const QFontMetricsF metrics(labelFont());
    const auto& plot = g.plot;
    const bool parade = kind_ == scopes::Kind::Parade;
    const qreal gap = kParadeGap;
    const qreal paneWidth = parade ? (plot.width() - 2 * gap) / 3 : plot.width();
    std::array<QRectF, 3> panes;
    const int paneCount = parade ? 3 : 1;
    for (int pane = 0; pane < paneCount; ++pane)
        panes[static_cast<std::size_t>(pane)] =
            QRectF(plot.left() + pane * (paneWidth + gap), plot.top(), paneWidth, plot.height());

    // Graticule under the trace: a quiet line at every labelled code.
    painter.setPen(hairline(kit::color(Color::Border), dpr));
    for (const int code : kLabelCodes) {
        const qreal y = snap(plot.bottom() - codeFraction(code) * plot.height(), dpr);
        for (int pane = 0; pane < paneCount; ++pane)
            painter.drawLine(QPointF(panes[static_cast<std::size_t>(pane)].left(), y),
                             QPointF(panes[static_cast<std::size_t>(pane)].right(), y));
    }
    // Minor ticks between the labelled lines, down the left edge.
    painter.setPen(hairline(kit::color(Color::BorderHover), dpr));
    for (int code = 0; code <= 1023; code += 32) {
        if (code % 128 == 0)
            continue;
        const qreal y = snap(plot.bottom() - codeFraction(code) * plot.height(), dpr);
        painter.drawLine(QPointF(plot.left() - kit::px(kit::Size::MinorTick), y),
                         QPointF(plot.left(), y));
    }
    // And along the bottom, sixteen steps of each pane.
    for (int pane = 0; pane < paneCount; ++pane) {
        const auto& rect = panes[static_cast<std::size_t>(pane)];
        for (int step = 0; step <= 16; ++step) {
            const qreal x = snap(rect.left() + rect.width() * step / 16.0, dpr);
            const int length =
                step % 4 == 0 ? kit::px(kit::Size::MajorTick) : kit::px(kit::Size::MinorTick);
            painter.drawLine(QPointF(x, plot.bottom()), QPointF(x, plot.bottom() + length));
        }
    }

    paintTrace(painter, plot, true);

    // Clip ticks: a column with an under or over count marks the bottom or top edge.
    if (!clip_.columns.empty()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(kit::color(Color::Warn));
        const auto columns = static_cast<qreal>(clip_.columns.size());
        for (int pane = 0; pane < paneCount; ++pane) {
            const auto& rect = panes[static_cast<std::size_t>(pane)];
            const qreal width = std::max<qreal>(1.0 / dpr, rect.width() / columns);
            for (std::size_t column = 0; column < clip_.columns.size(); ++column) {
                const qreal x = rect.left() + rect.width() * static_cast<qreal>(column) / columns;
                if ((clip_.columns[column] & 1U) != 0)
                    painter.drawRect(
                        QRectF(x, rect.bottom() - kClipMarkerWidth, width, kClipMarkerWidth));
                if ((clip_.columns[column] & 2U) != 0)
                    painter.drawRect(QRectF(x, rect.top(), width, kClipMarkerWidth));
            }
        }
        painter.setBrush(Qt::NoBrush);
    }

    // Labels never overlap the trace area: they sit in the left gutter.
    painter.setPen(kit::color(Color::Muted));
    painter.setFont(labelFont());
    for (const int code : kLabelCodes) {
        const qreal y = plot.bottom() - codeFraction(code) * plot.height();
        painter.drawText(QRectF(0, y - metrics.height() / 2,
                                plot.left() - kit::px(kit::Spacing::XS), metrics.height()),
                         Qt::AlignRight | Qt::AlignVCenter, axisLabel(code));
    }
    if (parade) {
        painter.setFont(nameFont());
        for (int pane = 0; pane < 3; ++pane) {
            painter.setPen(kit::color(kChannelTints[static_cast<std::size_t>(pane)].token));
            painter.drawText(
                panes[static_cast<std::size_t>(pane)].adjusted(kit::px(kit::Spacing::XS),
                                                               kit::px(kit::Spacing::XS), 0, 0),
                Qt::AlignRight | Qt::AlignTop,
                QString::fromLatin1(kChannelTints[static_cast<std::size_t>(pane)].letter) +
                    QString(QChar(0x2009)));
        }
    }
}

void ScopeView::paintVectorscope(QPainter& painter, const Geometry& g) const {
    const qreal dpr = devicePixelRatioF();
    const auto& plot = g.plot;
    const QPointF center = plot.center();
    const qreal radius = plot.width() / 2;
    painter.setRenderHint(QPainter::Antialiasing, false);
    // Cross hairs.
    painter.setPen(hairline(kit::color(Color::Border), dpr));
    painter.drawLine(QPointF(plot.left(), snap(center.y(), dpr)),
                     QPointF(plot.right(), snap(center.y(), dpr)));
    painter.drawLine(QPointF(snap(center.x(), dpr), plot.top()),
                     QPointF(snap(center.x(), dpr), plot.bottom()));
    painter.setRenderHint(QPainter::Antialiasing, true);
    // The circle of radius 0.5 and the tick marks around it, every 10 degrees and longer every 30.
    painter.setPen(hairline(kit::color(Color::BorderHover), dpr));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(center, radius, radius);
    painter.setPen(hairline(kit::color(Color::Muted), dpr));
    for (int degrees = 0; degrees < 360; degrees += 10) {
        const qreal angle = degrees * std::numbers::pi / 180.0;
        const qreal length =
            degrees % 30 == 0 ? kit::px(kit::Size::MajorTick) : kit::px(kit::Size::MinorTick);
        const QPointF direction(std::cos(angle), -std::sin(angle));
        painter.drawLine(center + direction * radius, center + direction * (radius - length));
    }
    // The skin tone line, thin, from the center at 123 degrees counter-clockwise from +Cb.
    {
        const qreal angle = kSkinToneDegrees * std::numbers::pi / 180.0;
        QPen pen = hairline(kit::color(Color::Faint), dpr);
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.drawLine(center, center + QPointF(std::cos(angle), -std::sin(angle)) * radius);
    }

    paintTrace(painter, plot, false);

    // The targets: four corner brackets each, with the letter beside, at the 75% bars.
    if (data_.color != nullptr) {
        const auto& luma = data_.color->luma();
        const qreal half = std::max<qreal>(5.0, plot.width() * 0.025);
        const qreal arm = half * 0.8;
        painter.setPen(hairline(kit::color(Color::Muted), dpr));
        painter.setRenderHint(QPainter::Antialiasing, false);
        for (const auto& target : kTargets) {
            const auto position = vectorscopePoint(luma, target);
            const QPointF at(snap(center.x() + position.x() * plot.width(), dpr),
                             snap(center.y() - position.y() * plot.height(), dpr));
            for (const int sx : {-1, 1})
                for (const int sy : {-1, 1}) {
                    const QPointF corner(at.x() + sx * half, at.y() + sy * half);
                    painter.drawLine(corner, QPointF(corner.x() - sx * arm, corner.y()));
                    painter.drawLine(corner, QPointF(corner.x(), corner.y() - sy * arm));
                }
        }
        painter.setFont(nameFont());
        painter.setPen(kit::color(Color::Muted));
        for (const auto& target : kTargets) {
            const auto position = vectorscopePoint(luma, target);
            const QPointF at(center.x() + position.x() * plot.width(),
                             center.y() - position.y() * plot.height());
            painter.drawText(QRectF(at.x() + half + kit::px(kit::Spacing::XXS),
                                    at.y() - half - kit::px(kit::Spacing::XS), 40, 16),
                             Qt::AlignLeft | Qt::AlignTop, QString::fromLatin1(target.name));
        }
    }
}

void ScopeView::paintHistogram(QPainter& painter, const Geometry& g) const {
    const qreal dpr = devicePixelRatioF();
    const QFontMetricsF metrics(labelFont());
    const auto& plot = g.plot;
    const auto channels = scopes::histogramChannelList(histogramChannels_);
    // Row frames, then the vertical graticule through every row.
    painter.setPen(hairline(kit::color(Color::Border), dpr));
    for (int row = 0; row <= g.rows; ++row) {
        const qreal y = snap(plot.top() + row * g.rowHeight, dpr);
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    for (const int code : kLabelCodes) {
        const qreal x = snap(plot.left() + codeFraction(code) * plot.width(), dpr);
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }
    // Minor ticks between the labelled lines along the top.
    painter.setPen(hairline(kit::color(Color::BorderHover), dpr));
    for (int code = 0; code <= 1023; code += 32) {
        if (code % 128 == 0)
            continue;
        const qreal x = snap(plot.left() + codeFraction(code) * plot.width(), dpr);
        painter.drawLine(QPointF(x, plot.top() - kit::px(kit::Size::MinorTick)),
                         QPointF(x, plot.top()));
    }

    paintTrace(painter, plot, false);

    // Clip markers at either end of a row whose channel has an under or over count.
    painter.setPen(Qt::NoPen);
    painter.setBrush(kit::color(Color::Warn));
    for (std::size_t row = 0; row < channels.size(); ++row) {
        const auto channel = channels[row];
        const qreal top = plot.top() + static_cast<qreal>(row) * g.rowHeight;
        if (clip_.under[channel] != 0)
            painter.drawRect(QRectF(plot.left(), top, kClipMarkerWidth, g.rowHeight));
        if (clip_.over[channel] != 0)
            painter.drawRect(
                QRectF(plot.right() - kClipMarkerWidth, top, kClipMarkerWidth, g.rowHeight));
    }
    painter.setBrush(Qt::NoBrush);

    // Code labels across the top, in the gutter above the rows.
    painter.setFont(labelFont());
    painter.setPen(kit::color(Color::Muted));
    const qreal labelTop = plot.top() - kit::px(kit::Size::MinorTick) - metrics.height();
    for (const int code : kLabelCodes) {
        const qreal x = plot.left() + codeFraction(code) * plot.width();
        const auto text = axisLabel(code);
        const qreal width = metrics.horizontalAdvance(text) + 2;
        const qreal left = std::clamp(x - width / 2, 0.0, std::max(0.0, this->width() - width));
        painter.drawText(QRectF(left, labelTop, width, metrics.height()), Qt::AlignCenter, text);
    }
    painter.setFont(nameFont());
    for (std::size_t row = 0; row < channels.size(); ++row) {
        const auto& tint = kChannelTints[channels[row]];
        painter.setPen(kit::color(tint.token));
        painter.drawText(
            QRectF(plot.left() + kit::px(kit::Spacing::XS),
                   plot.top() + static_cast<qreal>(row) * g.rowHeight + kit::px(kit::Spacing::XXS),
                   24, 16),
            Qt::AlignLeft | Qt::AlignTop, QString::fromLatin1(tint.letter));
    }
}

void ScopeView::paintChromaticity(QPainter& painter, const Geometry& g) const {
    const qreal dpr = devicePixelRatioF();
    const QFontMetricsF metrics(labelFont());
    const auto& plot = g.plot;
    const qreal side = plot.width();
    const auto map = [&plot, side](const double x, const double y) {
        return QPointF(plot.left() + x * side, plot.bottom() - y * side);
    };
    painter.setRenderHint(QPainter::Antialiasing, false);
    // Grid every 0.1, labelled every 0.2.
    for (int step = 0; step <= 10; ++step) {
        const bool edge = step == 0 || step == 10;
        painter.setPen(hairline(kit::color(edge ? Color::BorderHover : Color::Border), dpr));
        const qreal x = snap(plot.left() + side * step / 10.0, dpr);
        const qreal y = snap(plot.bottom() - side * step / 10.0, dpr);
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    paintTrace(painter, g.image, false);

    painter.setRenderHint(QPainter::Antialiasing, true);
    // The spectral locus: a thin curve in the strongest token with a tick every 10 nm.
    static const auto locus = color::cie1931SpectralLocus();
    {
        QPainterPath path;
        for (std::size_t index = 0; index < locus.size(); ++index) {
            const auto point = map(locus[index].x, locus[index].y);
            if (index == 0)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        painter.setPen(hairline(kit::color(Color::Foreground), dpr));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        // The line of purples closes the shape, quieter than the locus itself.
        if (locus.size() > 1)
            painter.setPen(hairline(kit::color(Color::Muted), dpr));
        painter.drawLine(map(locus.back().x, locus.back().y),
                         map(locus.front().x, locus.front().y));
        painter.setPen(hairline(kit::color(Color::Foreground), dpr));
        for (std::size_t index = 1; index + 1 < locus.size(); ++index) {
            if (locus[index].wavelengthNm % 10 != 0 || locus[index].wavelengthNm < 460 ||
                locus[index].wavelengthNm > 620)
                continue;
            const auto before = map(locus[index - 1].x, locus[index - 1].y);
            const auto after = map(locus[index + 1].x, locus[index + 1].y);
            const QPointF along = after - before;
            const qreal length = std::hypot(along.x(), along.y());
            if (length <= 0)
                continue;
            // The normal points away from the inside of the horseshoe (toward lower x, higher y
            // along the upper curve, which is a good enough outside for a tick).
            const QPointF normal(-along.y() / length, along.x() / length);
            const auto point = map(locus[index].x, locus[index].y);
            const QPointF outward = (point.x() - plot.center().x()) * normal.x() +
                                                (point.y() - plot.center().y()) * normal.y() >=
                                            0
                                        ? normal
                                        : -normal;
            painter.drawLine(point, point + outward * kit::px(kit::Size::MinorTick));
        }
    }
    if (data_.color != nullptr) {
        const auto& colorimetry = data_.color->colorimetry();
        QPolygonF triangle;
        triangle << map(colorimetry.red.x, colorimetry.red.y)
                 << map(colorimetry.green.x, colorimetry.green.y)
                 << map(colorimetry.blue.x, colorimetry.blue.y);
        painter.setPen(hairline(kit::color(Color::Muted), dpr));
        painter.drawPolygon(triangle);
        const auto white = map(colorimetry.white.x, colorimetry.white.y);
        painter.setPen(hairline(kit::color(Color::Foreground), dpr));
        painter.drawEllipse(white, 4.0, 4.0);
        painter.setFont(nameFont());
        painter.setPen(kit::color(Color::Muted));
        const auto name = QString::fromStdString(colorimetry.colorSpaceName);
        const auto greenCorner = map(colorimetry.green.x, colorimetry.green.y);
        painter.drawText(QRectF(greenCorner.x() + kit::px(kit::Spacing::XS),
                                greenCorner.y() - kit::px(kit::Spacing::S), 220, 16),
                         Qt::AlignLeft | Qt::AlignVCenter, name);
        // The space's name and R, G, B and W to four decimals in the top-right corner.
        painter.setFont(labelFont());
        const auto line = [](const char* letter, const color::ScopeChromaticity value) {
            return QString::fromLatin1(letter) + QLatin1Char(' ') +
                   QString::fromStdString(color::formatChromaticityXy(value));
        };
        const QStringList lines{name, line("R", colorimetry.red), line("G", colorimetry.green),
                                line("B", colorimetry.blue), line("W", colorimetry.white)};
        painter.setPen(kit::color(Color::Muted));
        const qreal lineHeight = metrics.height();
        for (int index = 0; index < lines.size(); ++index)
            painter.drawText(QRectF(plot.left(),
                                    plot.top() + kit::px(kit::Spacing::XS) + index * lineHeight,
                                    side - kit::px(kit::Spacing::S), lineHeight),
                             Qt::AlignRight | Qt::AlignVCenter, lines[index]);
    }
    // Axis labels every 0.2, with the axis names at the ends.
    painter.setFont(labelFont());
    painter.setPen(kit::color(Color::Muted));
    for (int step = 0; step <= 10; step += 2) {
        const auto text = step == 0    ? QStringLiteral("0")
                          : step == 10 ? QStringLiteral("1.0")
                                       : QString::number(step / 10.0, 'f', 1);
        const qreal x = plot.left() + side * step / 10.0;
        const qreal y = plot.bottom() - side * step / 10.0;
        const qreal width = metrics.horizontalAdvance(text) + 2;
        painter.drawText(QRectF(x - width / 2, plot.bottom() + kit::px(kit::Spacing::XXS), width,
                                metrics.height()),
                         Qt::AlignCenter, text);
        if (step != 0)
            painter.drawText(QRectF(0, y - metrics.height() / 2,
                                    plot.left() - kit::px(kit::Spacing::XS), metrics.height()),
                             Qt::AlignRight | Qt::AlignVCenter, text);
    }
    painter.setFont(nameFont());
    painter.drawText(
        QRectF(plot.right() - 20, plot.bottom() + kit::px(kit::Spacing::XXS), 20, metrics.height()),
        Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("x"));
    painter.drawText(QRectF(0, plot.top() - kit::px(kit::Spacing::XXS),
                            plot.left() - kit::px(kit::Spacing::XS), metrics.height()),
                     Qt::AlignRight | Qt::AlignTop, QStringLiteral("y"));
}

QString ScopeView::readoutAt(const QPointF& position) const {
    if (!measured())
        return {};
    const auto g = geometry(width(), height());
    const auto& tables = data_.scopes->tables;
    const auto& plot = g.plot;
    switch (kind_) {
    case scopes::Kind::Waveform:
    case scopes::Kind::Parade: {
        if (!plot.contains(position))
            return {};
        const bool parade = kind_ == scopes::Kind::Parade;
        const qreal gap = kParadeGap;
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
                          .arg(QString::fromLatin1(
                              kChannelTints[static_cast<std::size_t>(pane)].letter))
                          .arg(tables.waveform(static_cast<std::uint32_t>(pane), column, level));
        } else {
            for (const auto channel : scopes::waveformChannelList(waveformChannels_))
                counts << tr("%1 %2")
                              .arg(QString::fromLatin1(kChannelTints[channel].letter))
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
        const QString letter = QString::fromLatin1(kChannelTints[channel].letter);
        const qreal local = position.x() - plot.left();
        if (local < kClipMarkerWidth && clip_.under[channel] != 0)
            return tr("%1 under (below code 0) · Count %2").arg(letter).arg(clip_.under[channel]);
        if (local > plot.width() - kClipMarkerWidth && clip_.over[channel] != 0)
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

} // namespace bloom::ui
