#include <bloom/ui/stabilize_widget.hpp>

#include <bloom/document/project.hpp>
#include <bloom/document/warp_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/point_pad.hpp>
#include <bloom/ui/kit/progress_bar.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/node_controls_widget.hpp>
#include <bloom/ui/track_controller.hpp>

#include <QCoreApplication>
#include <QFontMetrics>
#include <QLabel>

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

StabilizeCardWidget::StabilizeCardWidget(CompositionSession& session, const document::NodeId node,
                                         const bool card, QWidget* parent)
    : NodeCardWidget(parent), session_(session), node_(node) {
    setObjectName(card ? QStringLiteral("nodeStabilize") : QStringLiteral("propertiesStabilize"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    PointPadRow::Spec pad;
    const auto roles = document::stabilizePointRoles();
    const auto labels = document::stabilizePointLabels();
    for (std::size_t index = 0; index < roles.size(); ++index)
        pad.points.push_back({std::string(roles[index]),
                              QCoreApplication::translate("Stabilize",
                                                          std::string(labels[index]).c_str()),
                              true, false});
    pad.connected = false;
    pad.commandLabel = tr("Move Tracked Point");
    pad.frame = [this] { return compositionFrame(session_); };
    pad_ = new PointPadRow(session_, node_, std::move(pad), this);
    pad_->pad()->setToolTip(tr("Drag the Track Point onto the feature to follow, on the frame to "
                               "start from"));

    button_ = new kit::KButton(tr("Track"), this);
    button_->setObjectName(QStringLiteral("stabilizeTrackButton"));
    button_->setToolTip(tr("Follow the Track Point from this frame to the end"));
    progress_ = new kit::KProgressBar(this);
    progress_->setObjectName(QStringLiteral("stabilizeTrackProgress"));
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("stabilizeTrackStatus"));
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::NoTextInteraction);
    connect(button_, &QAbstractButton::clicked, this, &StabilizeCardWidget::onButton);

    for (const auto signal :
         {&CompositionSession::snapshotChanged, &CompositionSession::liveValueChanged,
          &CompositionSession::currentTimeChanged})
        connect(&session_, signal, this, &StabilizeCardWidget::refresh);
    refresh();
}

bool StabilizeCardWidget::showsRole(const std::string_view) const { return true; }

std::optional<int> StabilizeCardWidget::socketY(const std::string_view) const { return std::nullopt; }

int StabilizeCardWidget::controlsHeight() const {
    const QFontMetrics metrics(font());
    return kit::px(kit::Size::Control) + kit::px(kit::Spacing::S) +
           2 * metrics.lineSpacing() + kit::px(kit::Spacing::S);
}

int StabilizeCardWidget::heightAt(const int width) const {
    return pad_->heightAt(width) + controlsHeight();
}

int StabilizeCardWidget::minimumWidthHint() const {
    return std::max(kit::px(kit::Size::DropdownWidthExpanded) - kit::px(kit::Size::Control),
                    pad_->minimumWidthHint());
}

QSize StabilizeCardWidget::sizeHint() const {
    const int width = minimumWidthHint();
    return {width, heightAt(width)};
}

void StabilizeCardWidget::layoutControls() {
    const int top = pad_->heightAt(width());
    const int row = kit::px(kit::Size::Control);
    const int gap = kit::px(kit::Spacing::S);
    const int buttonWidth = std::max(button_->sizeHint().width(), kit::px(kit::Size::Control) * 2);
    button_->setGeometry(0, top, buttonWidth, row);
    const int barHeight = progress_->sizeHint().height();
    progress_->setGeometry(buttonWidth + gap, top + (row - barHeight) / 2,
                           std::max(0, width() - buttonWidth - gap), barHeight);
    status_->setGeometry(0, top + row + gap, width(), height() - top - row - gap);
}

void StabilizeCardWidget::resizeEvent(QResizeEvent* event) {
    NodeCardWidget::resizeEvent(event);
    pad_->setGeometry(0, 0, width(), pad_->heightAt(width()));
    layoutControls();
}

void StabilizeCardWidget::onButton() {
    auto* controller = session_.trackController();
    if (controller == nullptr)
        return;
    const auto status = controller->status();
    if (status.state == TrackController::State::Running && status.node == node_)
        controller->cancel();
    else
        controller->start(node_);
}

void StabilizeCardWidget::refresh() {
    const auto* composition = session_.composition();
    pad_->refresh(composition == nullptr || composition->nodeLocked(node_));
    auto* controller = session_.trackController();
    if (controller != connected_) {
        connected_ = controller;
        if (controller != nullptr)
            connect(controller, &TrackController::changed, this, &StabilizeCardWidget::refresh,
                    Qt::UniqueConnection);
    }
    if (controller == nullptr) {
        button_->setEnabled(false);
        progress_->setProgress(0, 1);
        status_->setText(tr("Tracking is not available here."));
        return;
    }
    const auto status = controller->status();
    const bool mine = status.node == node_;
    const bool running = mine && status.state == TrackController::State::Running;
    button_->setText(running ? tr("Cancel") : tr("Track"));
    button_->setEnabled(running || !controller->running());
    if (running) {
        if (status.total == 0)
            progress_->setBusy();
        else
            progress_->setProgress(status.done, status.total);
    } else {
        progress_->setProgress(0, 1);
    }
    status_->setText(mine ? status.message : QString());
}

} // namespace bloom::ui
