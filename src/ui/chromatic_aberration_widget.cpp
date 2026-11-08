#include <bloom/ui/chromatic_aberration_widget.hpp>

#include <bloom/document/chromatic_aberration_nodes.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/point_pad.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_controls_widget.hpp>

#include <QCoreApplication>

#include <algorithm>
#include <string>

namespace bloom::ui {
namespace {

constexpr std::string_view kAmountRole = "amount";
constexpr std::string_view kCenterRole = "center";

[[nodiscard]] int small() { return kit::px(kit::Spacing::XS); }

[[nodiscard]] QRectF compositionFrame(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return {0.0, 0.0, 1920.0, 1080.0};
    const auto format = composition->format();
    return {0.0, 0.0, static_cast<double>(format.width()), static_cast<double>(format.height())};
}

} // namespace

ChromaticAberrationCardWidget::ChromaticAberrationCardWidget(CompositionSession& session,
                                                             const document::NodeId node,
                                                             const bool card, QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeChromaticAberration")
                       : QStringLiteral("propertiesChromaticAberration"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    PointPadRow::Spec pad;
    pad.points = {{std::string(kCenterRole), tr("Center"), true}};
    pad.commandLabel = tr("Move Chromatic Aberration Center");
    pad.frame = [this] { return compositionFrame(session_); };
    pad_ = new PointPadRow(session_, node_, std::move(pad), this);
    pad_->pad()->setToolTip(tr("Drag the point the colours spread from"));

    amount_ = new GradingValueRow(
        session_, node_, std::string(kAmountRole),
        std::string(document::kChromaticAberrationAmountSchemaKey), tr("Amount"),
        document::kChromaticAberrationAmountDefault, this);
    amount_->setDescription(
        tr("How far red and blue are pulled apart, as a percentage of half the frame's diagonal "
           "at the corner. Red moves outward and blue inward; below zero swaps them. Zero leaves "
           "the picture as it is."));
    GradingSliderSpec slider;
    slider.minimum = document::kChromaticAberrationAmountSoftMinimum;
    slider.maximum = document::kChromaticAberrationAmountSoftMaximum;
    slider.neutral = document::kChromaticAberrationAmountDefault;
    slider.ticks = (slider.maximum - slider.minimum) / 4.0;
    slider.gradient = jogStops();
    amount_->setSlider(slider);
    amount_->setShowsSign(true);
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &ChromaticAberrationCardWidget::refresh);
    refresh();
}

GradingValueRow* ChromaticAberrationCardWidget::valueRow(const std::string_view role) const {
    return role == kAmountRole ? amount_ : nullptr;
}

bool ChromaticAberrationCardWidget::showsRole(const std::string_view role) const {
    // The centre's X/Y row is the node's own row; the amount is here.
    return role == kCenterRole;
}

std::optional<int> ChromaticAberrationCardWidget::socketY(const std::string_view role) const {
    if (const auto* row = valueRow(role))
        return row->y() + row->socketY();
    return std::nullopt;
}

int ChromaticAberrationCardWidget::heightAt(const int width) const {
    return pad_->heightAt(width) + small() + amount_->heightHint();
}

int ChromaticAberrationCardWidget::minimumWidthHint() const {
    return std::max({kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control),
                     pad_->minimumWidthHint(), amount_->minimumWidthHint()});
}

QSize ChromaticAberrationCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void ChromaticAberrationCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    place();
}

void ChromaticAberrationCardWidget::place() {
    amount_->setLabelWidth(amount_->labelWidthHint());
    const int padHeight = pad_->heightAt(width());
    pad_->setGeometry(0, 0, width(), padHeight);
    amount_->setGeometry(0, padHeight + small(), width(), amount_->heightHint());
}

void ChromaticAberrationCardWidget::refresh() {
    const auto* composition = session_.composition();
    const bool locked = composition == nullptr || composition->nodeLocked(node_);
    amount_->refresh(locked);
    pad_->refresh(locked);
}

} // namespace bloom::ui
