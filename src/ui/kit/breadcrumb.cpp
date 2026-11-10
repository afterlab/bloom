#include <bloom/ui/kit/breadcrumb.hpp>

#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/typography.hpp>

#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <utility>

namespace bloom::ui::kit {
namespace {

const QString kSeparator = QStringLiteral(" ▸ ");
const QString kEllipsis = QStringLiteral("…");

} // namespace

KBreadcrumb::KBreadcrumb(QWidget* parent) : QWidget(parent) {
    ensureKeyboardFocusTracking(*this);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setFont(kit::font(TypeRole::Ui));
    setFixedHeight(px(Size::Control));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void KBreadcrumb::setCrumbs(std::vector<Crumb> crumbs, QString suffix) {
    crumbs_ = std::move(crumbs);
    suffix_ = std::move(suffix);
    hovered_ = -1;
    pressed_ = -1;
    if (focused_ >= static_cast<int>(crumbs_.size()) || focused_ < 0)
        focused_ = crumbs_.empty() ? -1 : static_cast<int>(crumbs_.size()) - 1;
    const auto path = pathText();
    setToolTip(path);
    setAccessibleDescription(path);
    updateGeometry();
    relayout();
}

QString KBreadcrumb::pathText() const {
    QStringList parts;
    for (const auto& crumb : crumbs_)
        parts.push_back(crumb.text);
    auto text = parts.join(kSeparator);
    if (!suffix_.isEmpty())
        text += QStringLiteral(" · ") + suffix_;
    return text;
}

int KBreadcrumb::textWidth(const QString& text) const {
    return QFontMetrics(font()).horizontalAdvance(text);
}

int KBreadcrumb::crumbWidth(const QString& text) const {
    return textWidth(text) + 2 * px(Spacing::S);
}

int KBreadcrumb::chevronWidth() const { return px(Size::IconSmall) + px(Spacing::XXS); }

int KBreadcrumb::suffixWidth() const {
    return suffix_.isEmpty() ? 0 : textWidth(QStringLiteral("· ") + suffix_) + px(Spacing::S);
}

std::vector<KBreadcrumb::Shown> KBreadcrumb::layoutFor(const int width) const {
    std::vector<Shown> entries;
    const int count = static_cast<int>(crumbs_.size());
    if (count == 0)
        return entries;
    for (int index = 0; index < count; ++index)
        entries.push_back({crumbs_[static_cast<std::size_t>(index)].text, index, {}, false});
    const auto total = [&] {
        int sum = suffixWidth();
        for (const auto& entry : entries)
            sum += crumbWidth(entry.text);
        return sum + std::max(0, static_cast<int>(entries.size()) - 1) * chevronWidth();
    };
    // Middle crumbs first, outermost middle first, into one "…" that goes to the innermost crumb
    // it stands for.
    while (total() > width && entries.size() > 2) {
        if (entries[1].text == kEllipsis && entries[1].elided) {
            if (entries.size() <= 3)
                break;
            // Fold the next middle crumb into the ellipsis.
            entries[1].target = entries[2].target;
            entries.erase(entries.begin() + 2);
            continue;
        }
        entries[1] = {kEllipsis, entries[1].target, {}, true};
    }
    // Then the first and last crumbs' own text, the outermost first.
    for (const auto which : {std::size_t{0}, entries.size() - 1}) {
        if (total() <= width)
            break;
        auto& entry = entries[which];
        const int others = total() - crumbWidth(entry.text);
        const int room = std::max(textWidth(kEllipsis), width - others - 2 * px(Spacing::S));
        const auto elided = QFontMetrics(font()).elidedText(
            crumbs_[static_cast<std::size_t>(entry.target)].text, Qt::ElideRight, room);
        if (elided != entry.text) {
            entry.text = elided;
            entry.elided = true;
        }
    }
    int x = 0;
    const int height = this->height() > 0 ? this->height() : px(Size::Control);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const int crumb = crumbWidth(entries[index].text);
        entries[index].rect = QRect(x, 0, crumb, height);
        x += crumb + (index + 1 < entries.size() ? chevronWidth() : 0);
    }
    return entries;
}

void KBreadcrumb::relayout() {
    layoutWidth_ = width();
    shown_ = layoutFor(layoutWidth_);
    update();
}

void KBreadcrumb::relayoutIfResized() {
    if (layoutWidth_ != width())
        relayout();
}

std::vector<KBreadcrumb::Shown> KBreadcrumb::shown() const {
    return layoutWidth_ == width() ? shown_ : layoutFor(width());
}

QSize KBreadcrumb::sizeHint() const {
    int width = suffixWidth();
    for (const auto& crumb : crumbs_)
        width += crumbWidth(crumb.text);
    width += std::max(0, static_cast<int>(crumbs_.size()) - 1) * chevronWidth();
    return {width, px(Size::Control)};
}

QSize KBreadcrumb::minimumSizeHint() const {
    // The first crumb's ellipsis, the middle "…" and the last crumb's ellipsis.
    const int crumbs = std::min<int>(3, static_cast<int>(crumbs_.size()));
    const int width = crumbs * crumbWidth(kEllipsis) + std::max(0, crumbs - 1) * chevronWidth();
    return {width, px(Size::Control)};
}

int KBreadcrumb::shownIndexAt(const QPoint& point) const {
    for (std::size_t index = 0; index < shown_.size(); ++index)
        if (shown_[index].rect.contains(point))
            return static_cast<int>(index);
    return -1;
}

int KBreadcrumb::shownIndexOfTarget(const int target) const {
    for (std::size_t index = 0; index < shown_.size(); ++index)
        if (shown_[index].target == target)
            return static_cast<int>(index);
    return -1;
}

void KBreadcrumb::activate(const int target) {
    // The last crumb is where the artist already is.
    if (target < 0 || target + 1 >= static_cast<int>(crumbs_.size()))
        return;
    emit crumbActivated(target);
}

void KBreadcrumb::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(font());
    const bool focus = hasKeyboardFocus(*this);
    const auto last = static_cast<int>(crumbs_.size()) - 1;
    const auto inset = static_cast<qreal>(px(Spacing::ControlInset));
    for (std::size_t index = 0; index < shown_.size(); ++index) {
        const auto& entry = shown_[index];
        const bool current = entry.target == last;
        State state = State::Normal;
        if (!isEnabled())
            state = State::Disabled;
        else if (!current && pressed_ == static_cast<int>(index))
            state = State::Pressed;
        else if (!current && hovered_ == static_cast<int>(index))
            state = State::Hover;
        const bool focused = focus && entry.target == focused_;
        const auto colors = buttonColors(ButtonKind::Ghost, state, focused);
        const QRectF body = QRectF(entry.rect).adjusted(0.0, inset, 0.0, -inset);
        fillRoundedSurface(painter, body, colors.fill, colors.border, Radius::Small);
        // Where you are reads in full ink; the way back reads muted until the pointer is on it.
        QColor ink = colors.ink;
        if (!current && state == State::Normal && isEnabled())
            ink = color(Color::Muted);
        painter.setPen(ink);
        painter.drawText(QRectF(entry.rect).adjusted(px(Spacing::S), 0.0, -px(Spacing::S), 0.0),
                         Qt::AlignVCenter | Qt::AlignLeft, entry.text);
        if (index + 1 < shown_.size()) {
            const auto box = static_cast<qreal>(px(Size::IconSmall));
            const QRectF chevron(entry.rect.right() + 1.0 + px(Spacing::XXS) / 2.0,
                                 entry.rect.center().y() - box / 2.0 + 0.5, box, box);
            painter.drawPixmap(snappedToDevicePixels(painter, chevron).topLeft(),
                               iconPixmap(IconId::CaretRight, Size::IconSmall,
                                          color(Color::Muted), devicePixelRatioF(),
                                          iconWeight(IconRole::Chrome)));
        }
    }
    if (!suffix_.isEmpty() && !shown_.empty()) {
        const auto left = shown_.back().rect.right() + 1;
        painter.setPen(color(isEnabled() ? Color::Muted : Color::Muted));
        painter.drawText(QRect(left, 0, std::max(0, width() - left), height()),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         QFontMetrics(font()).elidedText(QStringLiteral("· ") + suffix_,
                                                         Qt::ElideRight,
                                                         std::max(0, width() - left)));
    }
}

void KBreadcrumb::mousePressEvent(QMouseEvent* event) {
    relayoutIfResized();
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    pressed_ = shownIndexAt(event->position().toPoint());
    update();
    event->accept();
}

void KBreadcrumb::mouseReleaseEvent(QMouseEvent* event) {
    relayoutIfResized();
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const int released = shownIndexAt(event->position().toPoint());
    const int pressed = std::exchange(pressed_, -1);
    update();
    event->accept();
    if (released >= 0 && released == pressed)
        activate(shown_[static_cast<std::size_t>(released)].target);
}

void KBreadcrumb::mouseMoveEvent(QMouseEvent* event) {
    relayoutIfResized();
    const int hovered = shownIndexAt(event->position().toPoint());
    if (hovered != hovered_) {
        hovered_ = hovered;
        const bool link = hovered >= 0 && shown_[static_cast<std::size_t>(hovered)].target + 1 <
                                              static_cast<int>(crumbs_.size());
        setCursor(link ? Qt::PointingHandCursor : Qt::ArrowCursor);
        // A crumb shows its own name in full while the pointer is on it.
        if (hovered >= 0) {
            const auto& crumb = crumbs_[static_cast<std::size_t>(
                shown_[static_cast<std::size_t>(hovered)].target)];
            setToolTip(crumb.toolTip.isEmpty() ? pathText() : crumb.toolTip);
        } else {
            setToolTip(pathText());
        }
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void KBreadcrumb::moveFocus(const int step) {
    if (shown_.empty())
        return;
    int index = shownIndexOfTarget(focused_);
    if (index < 0)
        index = static_cast<int>(shown_.size()) - 1;
    index = std::clamp(index + step, 0, static_cast<int>(shown_.size()) - 1);
    focused_ = shown_[static_cast<std::size_t>(index)].target;
    update();
}

void KBreadcrumb::keyPressEvent(QKeyEvent* event) {
    relayoutIfResized();
    switch (event->key()) {
    case Qt::Key_Left:
        moveFocus(-1);
        event->accept();
        return;
    case Qt::Key_Right:
        moveFocus(1);
        event->accept();
        return;
    case Qt::Key_Home:
        moveFocus(-static_cast<int>(shown_.size()));
        event->accept();
        return;
    case Qt::Key_End:
        moveFocus(static_cast<int>(shown_.size()));
        event->accept();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        activate(focused_);
        event->accept();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void KBreadcrumb::focusInEvent(QFocusEvent* event) {
    relayoutIfResized();
    // Keyboard focus lands on the innermost place to go back to.
    if (event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason) {
        const int count = static_cast<int>(crumbs_.size());
        focused_ = count >= 2 ? count - 2 : count - 1;
        if (shownIndexOfTarget(focused_) < 0 && !shown_.empty())
            focused_ = shown_.size() >= 2 ? shown_[shown_.size() - 2].target : shown_.back().target;
    }
    QWidget::focusInEvent(event);
    update();
}

void KBreadcrumb::leaveEvent(QEvent* event) {
    hovered_ = -1;
    setToolTip(pathText());
    update();
    QWidget::leaveEvent(event);
}

void KBreadcrumb::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayout();
}

void KBreadcrumb::changeEvent(QEvent* event) {
    if (event->type() == QEvent::FontChange)
        relayout();
    QWidget::changeEvent(event);
}

} // namespace bloom::ui::kit
