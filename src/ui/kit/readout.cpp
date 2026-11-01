#include <bloom/ui/kit/readout.hpp>

#include <bloom/ui/kit/accessibility.hpp>
#include <bloom/ui/kit/painting.hpp>

#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <utility>

namespace bloom::ui::kit {
namespace {

// Two readout columns at most: a third would push related numbers too far apart to compare.
constexpr int kMaximumColumns = 2;

} // namespace

KReadoutGrid::KReadoutGrid(QWidget* parent) : QWidget(parent) {
    installKitAccessibility();
    setObjectName(QStringLiteral("kReadoutGrid"));
    setFocusPolicy(Qt::NoFocus);
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

int KReadoutGrid::addRow(const QString& label, const std::optional<Color> swatch) {
    Row entry{label, swatch, {}, {}, true};
    entry.value.tone = Color::Faint;
    entry.note = {Kind::Text, 0.0, MetricUnit::Count, {}, Color::Muted};
    rows_.push_back(std::move(entry));
    updateGeometry();
    return static_cast<int>(rows_.size()) - 1;
}

void KReadoutGrid::setLabel(const int row, const QString& label) {
    if (row < 0 || row >= rowCount() || rows_[static_cast<std::size_t>(row)].label == label)
        return;
    rows_[static_cast<std::size_t>(row)].label = label;
    repaintRow(row);
}

void KReadoutGrid::setSwatch(const int row, const std::optional<Color> swatch) {
    if (row < 0 || row >= rowCount())
        return;
    rows_[static_cast<std::size_t>(row)].swatch = swatch;
    repaintRow(row);
}

void KReadoutGrid::setValue(const int row, const double value, const MetricUnit unit,
                            const Color tone) {
    if (row < 0 || row >= rowCount())
        return;
    auto& part = rows_[static_cast<std::size_t>(row)].value;
    if (std::isnan(value)) {
        setAbsent(row);
        return;
    }
    if (part.kind == Kind::Number && part.number == value && part.unit == unit && part.tone == tone)
        return;
    part.kind = Kind::Number;
    part.number = value;
    part.unit = unit;
    part.tone = tone;
    part.text.clear();
    repaintRow(row);
}

void KReadoutGrid::setText(const int row, const QString& text, const Color tone) {
    if (row < 0 || row >= rowCount())
        return;
    auto& part = rows_[static_cast<std::size_t>(row)].value;
    if (part.kind == Kind::Text && part.text == text && part.tone == tone)
        return;
    part.kind = Kind::Text;
    part.text = text;
    part.tone = tone;
    repaintRow(row);
}

void KReadoutGrid::setAbsent(const int row) {
    if (row < 0 || row >= rowCount())
        return;
    auto& entry = rows_[static_cast<std::size_t>(row)];
    if (entry.value.kind == Kind::Absent && entry.note.kind == Kind::Text &&
        entry.note.text.isEmpty())
        return;
    entry.value = {};
    entry.value.tone = Color::Faint;
    entry.note = {Kind::Text, 0.0, MetricUnit::Count, {}, Color::Muted};
    repaintRow(row);
}

void KReadoutGrid::setNote(const int row, const QString& note, const Color tone) {
    if (row < 0 || row >= rowCount())
        return;
    auto& part = rows_[static_cast<std::size_t>(row)].note;
    if (part.kind == Kind::Text && part.text == note && part.tone == tone)
        return;
    part = {Kind::Text, 0.0, MetricUnit::Count, note, tone};
    repaintRow(row);
}

void KReadoutGrid::setNoteValue(const int row, const double value, const MetricUnit unit,
                                const Color tone) {
    if (row < 0 || row >= rowCount())
        return;
    auto& part = rows_[static_cast<std::size_t>(row)].note;
    if (part.kind == Kind::Number && part.number == value && part.unit == unit && part.tone == tone)
        return;
    part = {std::isnan(value) ? Kind::Absent : Kind::Number, value, unit, {}, tone};
    repaintRow(row);
}

void KReadoutGrid::setAge(const int row, const std::int64_t ageNs, const bool stale) {
    if (row < 0 || row >= rowCount())
        return;
    setNote(row, formatMetricAge(ageNs), stale ? Color::Warn : Color::Muted);
}

void KReadoutGrid::setRowVisible(const int row, const bool visible) {
    if (row < 0 || row >= rowCount() || rows_[static_cast<std::size_t>(row)].visible == visible)
        return;
    rows_[static_cast<std::size_t>(row)].visible = visible;
    updateGeometry();
    update();
}

int KReadoutGrid::visibleRowCount() const noexcept {
    return static_cast<int>(std::ranges::count_if(rows_, &Row::visible));
}

int KReadoutGrid::columnsFor(const int width) const {
    return std::clamp(width / px(Size::ReadoutColumnWidth), 1, kMaximumColumns);
}

int KReadoutGrid::columnCount() const { return columnsFor(width()); }

QString KReadoutGrid::partText(const Part& part) {
    switch (part.kind) {
    case Kind::Absent:
        return absentMetricText();
    case Kind::Number:
        return formatMetric(part.number, part.unit);
    case Kind::Text:
        return part.text;
    }
    return {};
}

QString KReadoutGrid::labelText(const int row) const {
    return row >= 0 && row < rowCount() ? rows_[static_cast<std::size_t>(row)].label : QString{};
}

QString KReadoutGrid::valueText(const int row) const {
    return row >= 0 && row < rowCount() ? partText(rows_[static_cast<std::size_t>(row)].value)
                                        : QString{};
}

QString KReadoutGrid::noteText(const int row) const {
    return row >= 0 && row < rowCount() ? partText(rows_[static_cast<std::size_t>(row)].note)
                                        : QString{};
}

bool KReadoutGrid::isAbsent(const int row) const {
    return row >= 0 && row < rowCount() &&
           rows_[static_cast<std::size_t>(row)].value.kind == Kind::Absent;
}

Color KReadoutGrid::valueTone(const int row) const {
    return row >= 0 && row < rowCount() ? rows_[static_cast<std::size_t>(row)].value.tone
                                        : Color::Foreground;
}

Color KReadoutGrid::noteTone(const int row) const {
    return row >= 0 && row < rowCount() ? rows_[static_cast<std::size_t>(row)].note.tone
                                        : Color::Muted;
}

int KReadoutGrid::heightForWidth(const int width) const {
    const int columns = columnsFor(width);
    const int perColumn = (visibleRowCount() + columns - 1) / columns;
    return perColumn * px(Size::PropertyRow);
}

QSize KReadoutGrid::sizeHint() const {
    return {px(Size::ReadoutColumnWidth), heightForWidth(px(Size::ReadoutColumnWidth))};
}

QSize KReadoutGrid::minimumSizeHint() const {
    return {px(Size::ReadoutLabelWidth) + px(Size::ReadoutValueWidth),
            heightForWidth(px(Size::ReadoutLabelWidth) + px(Size::ReadoutValueWidth))};
}

QRect KReadoutGrid::rowRect(const int row) const {
    if (row < 0 || row >= rowCount() || !rows_[static_cast<std::size_t>(row)].visible)
        return {};
    int visibleIndex = 0;
    for (int index = 0; index < row; ++index)
        visibleIndex += rows_[static_cast<std::size_t>(index)].visible ? 1 : 0;
    const int columns = columnCount();
    const int perColumn = std::max(1, (visibleRowCount() + columns - 1) / columns);
    const int columnWidth = width() / columns;
    const int pitch = px(Size::PropertyRow);
    return {(visibleIndex / perColumn) * columnWidth, (visibleIndex % perColumn) * pitch,
            columnWidth, pitch};
}

void KReadoutGrid::repaintRow(const int row) {
    const QRect area = rowRect(row);
    if (!area.isEmpty())
        update(area);
}

void KReadoutGrid::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    update();
}

void KReadoutGrid::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QFont labelFont = kit::font(TypeRole::Ui);
    const QFont valueFont = kit::font(TypeRole::Value);
    const QFont noteFont = kit::font(TypeRole::UiSmall);
    const int padding = px(Spacing::RowPadding);
    const int gap = px(Spacing::S);
    const int swatchExtent = px(Size::IconSmall);
    for (int row = 0; row < rowCount(); ++row) {
        const QRect area = rowRect(row);
        if (area.isEmpty() || !event->rect().intersects(area))
            continue;
        const auto& entry = rows_[static_cast<std::size_t>(row)];
        QRect label(area.left() + padding, area.top(), px(Size::ReadoutLabelWidth), area.height());
        if (entry.swatch.has_value()) {
            const QRectF swatch(label.left(), area.center().y() - swatchExtent / 2.0 + 0.5,
                                swatchExtent, swatchExtent);
            fillRoundedSurface(painter, swatch, color(*entry.swatch),
                               edgeInkFor(color(*entry.swatch)), Radius::Small);
            label.setLeft(label.left() + swatchExtent + px(Spacing::XS));
        }
        painter.setFont(labelFont);
        painter.setPen(color(Color::Muted));
        painter.drawText(
            label, Qt::AlignLeft | Qt::AlignVCenter,
            QFontMetrics(labelFont).elidedText(entry.label, Qt::ElideRight, label.width() - gap));
        const QRect value(area.left() + padding + px(Size::ReadoutLabelWidth), area.top(),
                          px(Size::ReadoutValueWidth), area.height());
        // Numbers take the Value role; words (Yes, a list of names) stay interface text.
        const QFont& valueFace = entry.value.kind == Kind::Text ? labelFont : valueFont;
        painter.setFont(valueFace);
        painter.setPen(color(entry.value.tone));
        const QString valueString = partText(entry.value);
        const QFontMetrics valueMetrics(valueFace);
        if (entry.value.kind == Kind::Text &&
            valueMetrics.horizontalAdvance(valueString) > value.width()) {
            // A text value wider than the number column (a list of names) starts at the column
            // and takes the rest of the row instead of a note.
            const QRect wide(value.left(), area.top(),
                             std::max(0, area.right() - value.left() - padding), area.height());
            painter.drawText(wide, Qt::AlignLeft | Qt::AlignVCenter,
                             valueMetrics.elidedText(valueString, Qt::ElideRight, wide.width()));
            continue;
        }
        painter.drawText(value, Qt::AlignRight | Qt::AlignVCenter, valueString);
        const QString noteString = entry.note.kind == Kind::Text && entry.note.text.isEmpty()
                                       ? QString{}
                                       : partText(entry.note);
        if (!noteString.isEmpty()) {
            const QRect note(value.right() + gap, area.top(),
                             std::max(0, area.right() - value.right() - gap - padding),
                             area.height());
            painter.setFont(noteFont);
            painter.setPen(color(entry.note.tone));
            painter.drawText(
                note, Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(noteFont).elidedText(noteString, Qt::ElideRight, note.width()));
        }
    }
}

} // namespace bloom::ui::kit
