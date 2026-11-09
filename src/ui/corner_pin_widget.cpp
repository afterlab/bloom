#include <bloom/ui/corner_pin_widget.hpp>

#include <bloom/document/project.hpp>
#include <bloom/document/warp_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/point_pad.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_controls_widget.hpp>

#include <QCoreApplication>

#include <algorithm>
#include <string>

namespace bloom::ui {
namespace {

[[nodiscard]] QRectF compositionFrame(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return {0.0, 0.0, 1920.0, 1080.0};
    const auto format = composition->format();
    return {0.0, 0.0, static_cast<double>(format.width()), static_cast<double>(format.height())};
}

} // namespace

CornerPinCardWidget::CornerPinCardWidget(CompositionSession& session, const document::NodeId node,
                                         const bool card, QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeCornerPin") : QStringLiteral("propertiesCornerPin"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    // The corners' place on the frame, as a fraction from its centre, y down.
    const std::array<QPointF, 4> corners{QPointF(-0.5, -0.5), QPointF(0.5, -0.5),
                                         QPointF(0.5, 0.5), QPointF(-0.5, 0.5)};
    PointPadRow::Spec pad;
    const auto roles = document::cornerPinCornerRoles();
    const auto labels = document::cornerPinCornerLabels();
    for (std::size_t index = 0; index < roles.size(); ++index) {
        PointPadRow::Point point{std::string(roles[index]),
                                 QCoreApplication::translate("CornerPin",
                                                             std::string(labels[index]).c_str()),
                                 false, false, corners[index]};
        pad.points.push_back(std::move(point));
    }
    pad.connected = true;
    pad.commandLabel = tr("Move Pinned Corner");
    pad.frame = [this] { return compositionFrame(session_); };
    pad_ = new PointPadRow(session_, node_, std::move(pad), this);
    pad_->pad()->setToolTip(tr("Drag a corner to where that corner of the picture should land"));
    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &CornerPinCardWidget::refresh);
    refresh();
}

bool CornerPinCardWidget::showsRole(const std::string_view) const {
    // The corners' X/Y rows and the filter are the node's own rows; the pad is here.
    return true;
}

std::optional<int> CornerPinCardWidget::socketY(const std::string_view) const { return std::nullopt; }

int CornerPinCardWidget::heightAt(const int width) const { return pad_->heightAt(width); }

int CornerPinCardWidget::minimumWidthHint() const {
    return std::max(kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control),
                    pad_->minimumWidthHint());
}

QSize CornerPinCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void CornerPinCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    pad_->setGeometry(0, 0, width(), pad_->heightAt(width()));
}

void CornerPinCardWidget::refresh() {
    const auto* composition = session_.composition();
    pad_->refresh(composition == nullptr || composition->nodeLocked(node_));
}

} // namespace bloom::ui
