#include <bloom/ui/hue_saturation_widget.hpp>

#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/grade_curve.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/typography.hpp>

#include <QContextMenuEvent>
#include <QEnterEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <memory>

namespace bloom::ui {
namespace {

constexpr std::string_view kHueRole = "hue";
constexpr std::string_view kSaturationRole = "saturation";
constexpr std::string_view kVibranceRole = "vibrance";
constexpr int kSpectrumStops = 48;

[[nodiscard]] int control() { return kit::px(kit::Size::Control); }
[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }

[[nodiscard]] QColor display(const std::array<double, 3>& rgb) {
    return QColor::fromRgbF(static_cast<float>(std::clamp(rgb[0], 0.0, 1.0)),
                            static_cast<float>(std::clamp(rgb[1], 0.0, 1.0)),
                            static_cast<float>(std::clamp(rgb[2], 0.0, 1.0)));
}

[[nodiscard]] double wrapHue(const double degrees) {
    double wrapped = std::fmod(degrees + 180.0, 360.0);
    if (wrapped <= 0.0)
        wrapped += 360.0;
    return wrapped - 180.0;
}

} // namespace

double hueStripDragValue(const double from, const double pixels, const double width,
                         const double scale) {
    // Turning the hue forward moves each colour toward the next hue to its right, so the output
    // spectrum slides left: a drag to the right turns it back, and the spectrum follows the pointer.
    const double degrees = from - pixels * scale * 360.0 / std::max(1.0, width);
    return wrapHue(degrees);
}

// --- HueShiftStrip ------------------------------------------------------------------------------

HueShiftStrip::HueShiftStrip(CompositionSession& session, const document::NodeId node,
                             QWidget* parent)
    : QWidget(parent), session_(session), node_(node) {
    setObjectName(QStringLiteral("hueShiftStrip"));
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::SizeHorCursor);
    setAttribute(Qt::WA_Hover, true);
    setAccessibleName(tr("Hue shift"));
    setToolTip(tr("Hue shift: each input hue above, the colour it becomes below"));
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, qOverload<>(&QWidget::update));
}

std::optional<document::ParameterId> HueShiftStrip::parameter() const {
    return nodeRoleParameter(session_, node_, kHueRole);
}

double HueShiftStrip::hue() const {
    const auto id = parameter();
    return id ? liveScalar(session_, *id).value_or(0.0) : 0.0;
}

qreal HueShiftStrip::captionWidth() const {
    const QFontMetricsF metrics(kit::font(kit::TypeRole::UiSmall));
    return std::max(metrics.horizontalAdvance(tr("In")), metrics.horizontalAdvance(tr("Out"))) +
           small();
}

QRectF HueShiftStrip::inputBand() const {
    const qreal left = captionWidth();
    const qreal band = (height() - kit::px(kit::Spacing::XXS)) / 2.0;
    return {left, 0.0, std::max<qreal>(0.0, width() - left), band};
}

QRectF HueShiftStrip::outputBand() const {
    const auto input = inputBand();
    return input.translated(0.0, input.height() + kit::px(kit::Spacing::XXS));
}

void HueShiftStrip::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto input = inputBand();
    const auto output = outputBand();
    const auto matrix = runtime::hueSaturationMatrix(hue(), 1.0);
    QLinearGradient before(input.topLeft(), input.topRight());
    QLinearGradient after(output.topLeft(), output.topRight());
    for (int stop = 0; stop <= kSpectrumStops; ++stop) {
        const double t = static_cast<double>(stop) / kSpectrumStops;
        const auto colour = runtime::gradeHueColor(t - std::floor(t));
        std::array<double, 3> turned{};
        for (std::size_t row = 0; row < 3; ++row)
            turned[row] = matrix[row][0] * colour[0] + matrix[row][1] * colour[1] +
                          matrix[row][2] * colour[2];
        before.setColorAt(t, display(colour));
        after.setColorAt(t, display(turned));
    }
    const qreal radius = kit::px(kit::Spacing::XXS);
    const bool enabled = isEnabled();
    painter.setOpacity(enabled ? 1.0 : kit::kDisabledOpacity);
    painter.setPen(Qt::NoPen);
    painter.setBrush(before);
    painter.drawRoundedRect(input, radius, radius);
    painter.setBrush(after);
    painter.drawRoundedRect(output, radius, radius);
    painter.setOpacity(1.0);
    painter.setBrush(Qt::NoBrush);
    const auto edge = drag_ ? kit::Color::Accent
                      : hasFocus() ? kit::Color::Accent
                      : hovered_   ? kit::Color::BorderHover
                                   : kit::Color::Border;
    kit::applyHairlinePen(painter, kit::color(edge));
    painter.drawRoundedRect(output, radius, radius);
    painter.setFont(kit::font(kit::TypeRole::UiSmall));
    painter.setPen(kit::color(enabled ? kit::Color::Muted : kit::Color::Faint));
    painter.drawText(QRectF(0.0, input.top(), captionWidth(), input.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, tr("In"));
    painter.drawText(QRectF(0.0, output.top(), captionWidth(), output.height()),
                     Qt::AlignLeft | Qt::AlignVCenter, tr("Out"));
}

void HueShiftStrip::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::RightButton && drag_) {
        cancelDrag();
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton || !isEnabled()) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    event->accept();
    const auto id = parameter();
    if (!id || !session_.beginValueEdit(*id))
        return;
    const auto modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    drag_ = Drag{*id, event->position().x(), hue(), kit::KSlider::dragScale(modifiers), modifiers};
    update();
}

void HueShiftStrip::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_)
        return;
    if (!session_.isValueEditing(drag_->parameter)) {
        drag_.reset();
        update();
        return;
    }
    const auto modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    if (modifiers != drag_->modifiers) {
        // A modifier change continues from the hue reached, so nothing jumps.
        drag_->base = hue();
        drag_->fromX = event->position().x();
        drag_->scale = kit::KSlider::dragScale(modifiers);
        drag_->modifiers = modifiers;
    }
    const double next = hueStripDragValue(drag_->base, event->position().x() - drag_->fromX,
                                          inputBand().width(), drag_->scale);
    if (!session_.updateValueEdit(next))
        cancelDrag();
    event->accept();
}

void HueShiftStrip::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !drag_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool owned = session_.isValueEditing(drag_->parameter);
    drag_.reset();
    // One command for the gesture; a press that moved nothing commits nothing.
    if (owned)
        static_cast<void>(session_.commitValueEdit());
    update();
    event->accept();
}

void HueShiftStrip::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    cancelDrag();
    static_cast<void>(reset());
    event->accept();
}

QMenu* HueShiftStrip::makeContextMenu() {
    auto* menu = kit::makeMenu(this);
    const auto id = parameter();
    auto* reset = menu->addAction(tr("Reset Hue"));
    reset->setObjectName(QStringLiteral("hueShiftStripReset"));
    reset->setEnabled(isEnabled() && hue() != 0.0);
    connect(reset, &QAction::triggered, this, [this] { static_cast<void>(this->reset()); });
    const auto state =
        id ? session_.keyframeDiamondStateForParameter(*id) : KeyframeDiamondState::Unsupported;
    if (state != KeyframeDiamondState::Unsupported) {
        auto* key = menu->addAction(state == KeyframeDiamondState::AnimatedWithKey ? tr("Remove Key")
                                                                                    : tr("Add Key"));
        key->setObjectName(QStringLiteral("hueShiftStripKey"));
        connect(key, &QAction::triggered, this, [this, parameter = *id] {
            static_cast<void>(session_.toggleKeyframeForParameter(parameter));
        });
    }
    return menu;
}

void HueShiftStrip::contextMenuEvent(QContextMenuEvent* event) {
    if (drag_) {
        event->accept();
        return;
    }
    const std::unique_ptr<QMenu> menu(makeContextMenu());
    menu->exec(event->globalPos());
    event->accept();
}

void HueShiftStrip::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && drag_) {
        cancelDrag();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void HueShiftStrip::focusOutEvent(QFocusEvent* event) {
    cancelDrag();
    QWidget::focusOutEvent(event);
    update();
}

void HueShiftStrip::hideEvent(QHideEvent* event) {
    cancelDrag();
    QWidget::hideEvent(event);
}

void HueShiftStrip::enterEvent(QEnterEvent* event) {
    hovered_ = true;
    update();
    QWidget::enterEvent(event);
}

void HueShiftStrip::leaveEvent(QEvent* event) {
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void HueShiftStrip::cancelDrag() {
    if (!drag_)
        return;
    const auto id = drag_->parameter;
    drag_.reset();
    if (session_.isValueEditing(id))
        session_.cancelValueEdit();
    update();
}

bool HueShiftStrip::reset() {
    const auto id = parameter();
    if (!id)
        return false;
    if (hue() == 0.0)
        return true;
    return session_.setParameterValue(*id, 0.0, tr("Reset Hue"));
}

// --- HueSaturationCardWidget --------------------------------------------------------------------

HueSaturationCardWidget::HueSaturationCardWidget(CompositionSession& session,
                                                 const document::NodeId node, const bool card,
                                                 QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeHueSaturation")
                       : QStringLiteral("propertiesHueSaturation"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    hue_ = new GradingValueRow(session_, node_, std::string(kHueRole),
                               document::kHueSaturationHueSchemaKey, tr("Hue"), 0.0, this);
    hue_->setShowsSign(true);
    hue_->setDescription(tr("Turns every hue around the colour wheel; grey stays grey."));
    strip_ = new HueShiftStrip(session_, node_, this);
    strip_->addAction(hue_->keyAction());
    saturation_ =
        new GradingValueRow(session_, node_, std::string(kSaturationRole),
                            document::kHueSaturationSaturationSchemaKey, tr("Saturation"), 1.0, this);
    saturation_->setDescription(tr("0 is grey, 1 leaves the colour as it is, above 1 is more vivid."));
    GradingSliderSpec slider;
    slider.minimum = 0.0;
    slider.maximum = 2.0;
    slider.neutral = 1.0;
    slider.gradient = greyToColourStops(0.5);
    saturation_->setSlider(slider);
    vibrance_ = new GradingValueRow(session_, node_, std::string(kVibranceRole),
                                    document::kHueSaturationVibranceSchemaKey, tr("Vibrance"), 0.0,
                                    this);
    vibrance_->setShowsSign(true);
    vibrance_->setDescription(
        tr("Saturates dull colors more than strong ones; negative values mute the dull ones."));
    GradingSliderSpec vibranceSlider;
    vibranceSlider.minimum = -1.0;
    vibranceSlider.maximum = 1.0;
    vibranceSlider.neutral = 0.0;
    vibranceSlider.ticks = 0.5;
    vibrance_->setSlider(vibranceSlider);
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &HueSaturationCardWidget::refresh);
    refresh();
}

bool HueSaturationCardWidget::showsRole(const std::string_view role) const {
    return role != kHueRole && role != kSaturationRole && role != kVibranceRole;
}

std::optional<int> HueSaturationCardWidget::socketY(const std::string_view role) const {
    if (role == kHueRole)
        return hue_->y() + hue_->socketY();
    if (role == kSaturationRole)
        return control() + small() + control() + small() + saturation_->socketY();
    if (role == kVibranceRole)
        return control() + small() + control() + small() + saturation_->heightHint() + small() +
               vibrance_->socketY();
    return std::nullopt;
}

int HueSaturationCardWidget::heightAt(const int) const {
    return control() + small() + control() + small() + saturation_->heightHint() + small() +
           vibrance_->heightHint();
}

int HueSaturationCardWidget::minimumWidthHint() const {
    return std::max({hue_->minimumWidthHint(), saturation_->minimumWidthHint(),
                     vibrance_->minimumWidthHint(),
                     kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control)});
}

QSize HueSaturationCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void HueSaturationCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void HueSaturationCardWidget::place() {
    const int w = width();
    const int labels = std::max({hue_->labelWidthHint(), saturation_->labelWidthHint(),
                                 vibrance_->labelWidthHint()});
    hue_->setLabelWidth(labels);
    saturation_->setLabelWidth(labels);
    vibrance_->setLabelWidth(labels);
    hue_->setGeometry(0, 0, w, hue_->heightHint());
    int y = control() + small();
    strip_->setGeometry(0, y, w, control());
    y += control() + small();
    saturation_->setGeometry(0, y, w, saturation_->heightHint());
    y += saturation_->heightHint() + small();
    vibrance_->setGeometry(0, y, w, vibrance_->heightHint());
}

void HueSaturationCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    hue_->refresh(locked);
    saturation_->refresh(locked);
    vibrance_->refresh(locked);
    const auto id = strip_->parameter();
    strip_->setEnabled(!locked && id && scalarEditable(session_, *id));
    strip_->update();
}

} // namespace bloom::ui
