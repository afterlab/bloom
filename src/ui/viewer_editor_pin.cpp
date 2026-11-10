// #536 Viewer pin/follow and the Viewer's own preview controller.
#include <bloom/ui/viewer_editor.hpp>

#include "node_editor_items.hpp"
#include "viewer_editor_internal.hpp"

#include <bloom/ui/editor_area.hpp>

#include <bloom/document/node_definition_registry.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/viewer_preview_pool.hpp>

#include <QAction>
#include <QActionGroup>
#include <QJsonArray>
#include <QMenu>
#include <QSignalBlocker>
#include <QToolButton>

#include <utility>

namespace bloom::ui {
using viewer_detail::viewerResolutionText;
namespace {

// The first Image or Mask output of `node`: what a node pin shows (#490's pixel inspection).
std::optional<std::string> pixelOutputOf(const document::NodeRecord& node) {
    const auto* definition =
        document::builtInNodeDefinitions().find(node.typeId, node.schemaVersion);
    if (definition == nullptr)
        return std::nullopt;
    for (const auto& output : definition->outputs)
        if (output.valueKind == document::SocketValueKind::Image ||
            output.valueKind == document::SocketValueKind::Mask)
            return output.name;
    return std::nullopt;
}

QString compositionName(const document::Composition* composition) {
    return composition != nullptr ? QString::fromStdString(composition->name()) : QString{};
}

// A pin as session JSON: {mode, composition, node?, port?}; empty while the Viewer follows.
QJsonObject pinObject(const ViewerPin& pin) {
    if (pin.mode == ViewerPinMode::Follow)
        return {};
    QJsonObject object{
        {"mode", pin.mode == ViewerPinMode::Node ? "node" : "composition"},
        {"composition", QString::number(pin.composition.value())},
    };
    if (pin.mode == ViewerPinMode::Node) {
        object.insert("node", QString::number(pin.node.value()));
        object.insert("port", QString::fromStdString(pin.port));
    }
    return object;
}

std::optional<ViewerPin> pinFromObject(const QJsonObject& object) {
    bool compositionOk = false;
    const auto composition = object.value("composition").toString().toULongLong(&compositionOk);
    if (!compositionOk)
        return std::nullopt;
    ViewerPin pin{.mode = ViewerPinMode::Composition,
                  .composition = document::CompositionId::fromRaw(composition),
                  .node = {},
                  .port = {}};
    const auto mode = object.value("mode").toString();
    if (mode == QStringLiteral("node")) {
        bool nodeOk = false;
        const auto node = object.value("node").toString().toULongLong(&nodeOk);
        if (!nodeOk)
            return std::nullopt;
        pin.mode = ViewerPinMode::Node;
        pin.node = document::NodeId::fromRaw(node);
        pin.port = object.value("port").toString().toStdString();
    } else if (mode != QStringLiteral("composition")) {
        return std::nullopt;
    }
    return pin;
}

} // namespace

const document::Composition* ViewerEditor::shownComposition() const {
    return session_.snapshot().project().findComposition(previewController_->targetCompositionId());
}

void ViewerEditor::setPreviewPool(ViewerPreviewPool* const pool) {
    previewPool_ = pool;
    updatePinControl();
    if (isVisible())
        refreshPreviewBinding();
}

bool ViewerEditor::setPin(ViewerPin pin) {
    if (pin.mode == ViewerPinMode::Follow) {
        followActiveComposition();
        return true;
    }
    const auto* composition = session_.snapshot().project().findComposition(pin.composition);
    if (composition == nullptr)
        return false;
    if (pin.mode == ViewerPinMode::Node) {
        const auto* node = composition->graph().findNode(pin.node);
        if (node == nullptr)
            return false;
        if (pin.port.empty()) {
            const auto output = pixelOutputOf(*node);
            if (!output)
                return false;
            pin.port = *output;
        }
    } else {
        pin.node = {};
        pin.port.clear();
    }
    if (pin == pin_)
        return true;
    if (ownPreview_ == nullptr && (previewPool_ == nullptr || packedHostShutdown_))
        return false;
    const ViewerPin previous = pin_;
    pin_ = std::move(pin);
    refreshPreviewBinding();
    if (ownPreview_ == nullptr) {
        // No controller of its own could be made: the Viewer keeps what it showed.
        pin_ = previous;
        updatePinControl();
        return false;
    }
    rememberPinForProject();
    return true;
}

bool ViewerEditor::pinToActiveComposition() {
    if (session_.composition() == nullptr)
        return false;
    return setPin({.mode = ViewerPinMode::Composition,
                   .composition = session_.compositionId(),
                   .node = {},
                   .port = {}});
}

bool ViewerEditor::pinToSelectedNode() {
    const auto* node = session_.selectedNode();
    if (node == nullptr || session_.composition() == nullptr)
        return false;
    const auto output = pixelOutputOf(*node);
    if (!output)
        return false;
    return setPin({.mode = ViewerPinMode::Node,
                   .composition = session_.compositionId(),
                   .node = node->id,
                   .port = *output});
}

void ViewerEditor::followActiveComposition() {
    if (pin_.mode == ViewerPinMode::Follow)
        return;
    pin_ = {};
    refreshPreviewBinding();
    rememberPinForProject();
}

QString ViewerEditor::workspaceAreaId() const {
    for (const auto* widget = parentWidget(); widget != nullptr; widget = widget->parentWidget())
        if (const auto* area = qobject_cast<const EditorArea*>(widget))
            return area->areaId();
    return {};
}

void ViewerEditor::rememberPinForProject() {
    session_.projectViewMemory().rememberViewerPin(workspaceAreaId(), pinObject(pin_));
}

void ViewerEditor::restoreProjectPin() {
    // Another project is bound: the old project's pin never applies to it. This project's own
    // remembered pin does, while its composition and node still exist; otherwise follow.
    const auto areaId = workspaceAreaId();
    const auto remembered = session_.projectViewMemory().recallViewerPin(areaId);
    const auto pin = remembered ? pinFromObject(*remembered) : std::nullopt;
    if (pin && setPin(*pin))
        return;
    followActiveComposition();
    if (remembered)
        session_.projectViewMemory().rememberViewerPin(areaId, {});
}

bool ViewerEditor::isReferenceOnly() const {
    return pin_.mode == ViewerPinMode::Node ||
           (pin_.mode == ViewerPinMode::Composition && pin_.composition != session_.compositionId());
}

QString ViewerEditor::pinDescription() const {
    const auto* composition = session_.snapshot().project().findComposition(pin_.composition);
    switch (pin_.mode) {
    case ViewerPinMode::Follow:
        break;
    case ViewerPinMode::Composition:
        return tr("Pinned to composition “%1”").arg(compositionName(composition));
    case ViewerPinMode::Node: {
        const auto* node = composition != nullptr ? composition->graph().findNode(pin_.node)
                                                  : nullptr;
        return tr("Pinned to node “%1” in “%2”")
            .arg(node != nullptr ? node_editor::nodeDisplayName(*composition, *node) : QString{},
                 compositionName(composition));
    }
    }
    return tr("Following the active composition");
}

void ViewerEditor::dropStalePin() {
    if (pin_.mode == ViewerPinMode::Follow)
        return;
    const auto* composition = session_.snapshot().project().findComposition(pin_.composition);
    if (composition == nullptr ||
        (pin_.mode == ViewerPinMode::Node && composition->graph().findNode(pin_.node) == nullptr))
        followActiveComposition();
    else
        updatePinControl();
}

void ViewerEditor::refreshPreviewBinding() {
    if (packedHostShutdown_)
        return;
    const QWidget* host = window();
    independentDisplay_ =
        host != nullptr && host != this && host->property(kIndependentDisplayProperty).toBool();
    const bool wantOwn = pin_.mode != ViewerPinMode::Follow || independentDisplay_;
    if (wantOwn && ownPreview_ == nullptr && previewPool_ != nullptr) {
        auto created = previewPool_->create();
        if (created != nullptr) {
            ownPreview_ = std::move(created);
            ownPreview_->setCompositionTarget(pin_.mode == ViewerPinMode::Follow
                                                  ? std::nullopt
                                                  : std::optional(pin_.composition));
            bindPreviewController(*ownPreview_);
        }
    } else if (!wantOwn && ownPreview_ != nullptr) {
        bindPreviewController(sharedPreview_);
        ownPreview_.reset();
        ownDisplayView_.reset();
        rebuildDisplayViewControl();
    }
    if (ownPreview_ != nullptr) {
        ownPreview_->setCompositionTarget(pin_.mode == ViewerPinMode::Follow
                                              ? std::nullopt
                                              : std::optional(pin_.composition));
        ownPreview_->setViewerChannelInspection(
            pin_.mode == ViewerPinMode::Node
                ? std::optional(runtime::ChannelInspection{pin_.node, pin_.port, {}})
                : std::nullopt);
    }
    rebuildCompositionSelector();
    // #645: a Viewer pinned to another composition is not where the level is.
    rebuildBreadcrumb();
    updatePinControl();
    updateEmptyState();
    update();
}

void ViewerEditor::bindPreviewController(CompositionPreviewController& controller) {
    if (previewController_ == &controller)
        return;
    disconnect(previewController_, nullptr, this, nullptr);
    previewController_ = &controller;
    connectPreviewController();
    resetPreviewDerivedState();
    applyViewerChoicesToPreview();
}

void ViewerEditor::applyViewerChoicesToPreview() {
    // The newly bound controller shows what this Viewer's own controls say.
    if (resolutionDropdown_ != nullptr && resolutionDropdown_->currentIndex() >= 0)
        previewController_->setResolutionPolicy(
            static_cast<runtime::PreviewResolutionPolicy>(resolutionDropdown_->currentIndex()));
    if (lookAction_ != nullptr)
        previewController_->setViewerLookEnabled(lookAction_->isChecked());
    rebuildDisplayViewControl();
    publishRoi();
    updatePreviewResolution();
    refreshViewAdjustment();
}

void ViewerEditor::connectPreviewController() {
    auto* controller = previewController_;
    connect(controller, &CompositionPreviewController::stateChanged, this, [this] {
        if (probePosition_)
            refreshProbe(*probePosition_);
    });
    connect(controller, &CompositionPreviewController::stateChanged, this,
            [this] { notifyAnchorModeState(); });
    connect(controller, &CompositionPreviewController::stateChanged, this, [this] {
        updatePreviewAccessibility();
        resolutionReadout_->setText(viewerResolutionText(*previewController_));
        resolutionDropdown_->setToolTip(viewerResolutionText(*previewController_));
        // A newly delivered frame may carry a format/proxy/pixel-aspect/display-descriptor change
        // (docs/architecture/animation-and-time.md); a mid-drag mapping change cancels the gesture
        // rather than silently mis-mapping the rest of it.
        if (dragActive_ && !mappingStillValid()) {
            endDrag(false);
        }
        update();
        if (statusBarFooter_ != nullptr) {
            statusBarFooter_->update();
        }
    });
    connect(controller, &CompositionPreviewController::stateChanged, this,
            &ViewerEditor::refreshViewAdjustment);
    connect(controller, &CompositionPreviewController::viewerResultsAvailable, this,
            &ViewerEditor::consumeChannelDisplay);
    connect(controller, &CompositionPreviewController::viewerResultsAvailable, this,
            &ViewerEditor::consumeChannelListing);
    connect(controller, &CompositionPreviewController::viewerResultsAvailable, this,
            &ViewerEditor::consumeViewAdjustment);
    connect(controller, &CompositionPreviewController::viewerResultsAvailable, this,
            &ViewerEditor::consumeProbe);
    connect(controller, &CompositionPreviewController::viewerResultsAvailable, this,
            &ViewerEditor::consumeDrillPick);
    connect(controller, &CompositionPreviewController::stateChanged, this,
            &ViewerEditor::continueBreakApartDrill);
    connect(controller, &CompositionPreviewController::viewerResultsAvailable, this,
            &ViewerEditor::pollResidentCpuFallback);
    connect(controller, &CompositionPreviewController::resolutionChanged, this, [this] {
        if (resolutionDropdown_ == nullptr)
            return;
        const QSignalBlocker blocker(resolutionDropdown_);
        resolutionDropdown_->setCurrentIndex(
            static_cast<int>(previewController_->settings().resolutionPolicy));
        resolutionReadout_->setText(viewerResolutionText(*previewController_));
        resolutionDropdown_->setToolTip(
            tr("Resolution: %1").arg(viewerResolutionText(*previewController_)));
    });
}

void ViewerEditor::resetPreviewDerivedState() {
    resetChannelDisplay();
    // Different document owners may have colliding numeric project/composition/revision IDs.
    // Detach cancelled per-view work and every local retained display before re-evaluation;
    // no late completion can become a candidate for the newly bound document.
    if (adjustTask_)
        adjustTask_->cancel();
    adjustTask_.reset();
    adjustIdentity_.reset();
    adjustedFrame_.reset();
    adjustmentFailure_.clear();
    if (cpuFallbackTask_)
        cpuFallbackTask_->cancel();
    cpuFallbackTask_.reset();
    cpuFallbackIdentity_.reset();
    cpuFallbackFailedIdentity_.reset();
    cpuFallbackFrame_.reset();
    lastCpuFrame_.reset();
    gpuRequestedFrame_.reset();
    cpuPaintedDisplay_.reset();
    cpuCoverDisplay_.reset();
    coverCaptureDisplay_.reset();
    clearProbe();
    probeTask_.reset();
    probeIdentity_.reset();
    probeTaskFrame_.reset();
    probeCacheFrame_.reset();
    probeCacheReference_.reset();
    probeCacheDisplayLinear_.reset();
    probeCacheDisplay_.reset();
    probeFailure_.clear();
    ++gpuOverlayRevision_;
    gpuPublicationDomain_.reset();
    updateGpuResidentPresentation();
    updatePreviewAccessibility();
    update();
}

void ViewerEditor::buildPinControl() {
    pinButton_ = new kit::KIconButton(this);
    pinButton_->setObjectName(QStringLiteral("viewerPinButton"));
    pinButton_->setCheckable(true);
    pinButton_->setPopupMode(QToolButton::InstantPopup);
    const int extent = kit::px(kit::iconSize(kit::IconRole::Chrome));
    pinButton_->setIconSize(QSize(extent, extent));
    pinMenu_ = kit::makeMenu(this);
    pinMenu_->setObjectName(QStringLiteral("viewerPinMenu"));
    pinMenu_->setToolTipsVisible(true);
    auto* group = new QActionGroup(pinMenu_);
    group->setExclusive(true);
    const auto add = [this, group](const QString& name, auto handler) {
        auto* action = pinMenu_->addAction(QString{});
        action->setObjectName(name);
        action->setCheckable(true);
        group->addAction(action);
        connect(action, &QAction::triggered, this, handler);
        return action;
    };
    add(QStringLiteral("viewerFollowAction"), [this] { followActiveComposition(); });
    add(QStringLiteral("viewerPinCompositionAction"), [this] { (void)pinToActiveComposition(); });
    add(QStringLiteral("viewerPinNodeAction"), [this] { (void)pinToSelectedNode(); });
    connect(pinMenu_, &QMenu::aboutToShow, this, &ViewerEditor::updatePinControl);
    pinButton_->setMenu(pinMenu_);

    pinLabel_ = new kit::KLabel(this);
    pinLabel_->setObjectName(QStringLiteral("viewerPinLabel"));
    pinLabel_->setTone(kit::Color::Accent);
    pinLabel_->hide();
    updatePinControl();
}

void ViewerEditor::updatePinControl() {
    if (pinButton_ == nullptr)
        return;
    const bool pinned = pin_.mode != ViewerPinMode::Follow;
    const bool available = previewPool_ != nullptr && !packedHostShutdown_;
    {
        const QSignalBlocker blocker(pinButton_);
        pinButton_->setChecked(pinned);
    }
    pinButton_->setIcon(pinned ? kit::icon(kit::IconId::Pin, kit::iconSize(kit::IconRole::Chrome),
                                           kit::Color::Accent, kit::IconWeight::Fill)
                               : kit::icon(kit::IconId::Pin, kit::IconRole::Chrome));
    const QString description = pinDescription();
    pinButton_->setToolTip(available || pinned
                               ? description
                               : tr("%1. Pinning is unavailable in this window.").arg(description));
    pinButton_->setAccessibleName(tr("Follow or pin: %1").arg(description));
    pinButton_->setEnabled(available || pinned);

    const auto* pinnedComposition =
        session_.snapshot().project().findComposition(pin_.composition);
    QString label;
    if (pin_.mode == ViewerPinMode::Composition) {
        label = tr("Pinned");
    } else if (pin_.mode == ViewerPinMode::Node && pinnedComposition != nullptr) {
        if (const auto* node = pinnedComposition->graph().findNode(pin_.node))
            label = tr("Pinned: %1").arg(node_editor::nodeDisplayName(*pinnedComposition, *node));
    }
    pinLabel_->setText(label);
    pinLabel_->setToolTip(description);
    pinLabel_->setVisible(pinned);

    const auto actions = pinMenu_->actions();
    auto* follow = pinMenu_->findChild<QAction*>(QStringLiteral("viewerFollowAction"));
    auto* composition = pinMenu_->findChild<QAction*>(QStringLiteral("viewerPinCompositionAction"));
    auto* nodeAction = pinMenu_->findChild<QAction*>(QStringLiteral("viewerPinNodeAction"));
    if (follow == nullptr || composition == nullptr || nodeAction == nullptr) {
        for (auto* action : actions) {
            if (action->objectName() == QStringLiteral("viewerFollowAction"))
                follow = action;
            else if (action->objectName() == QStringLiteral("viewerPinCompositionAction"))
                composition = action;
            else if (action->objectName() == QStringLiteral("viewerPinNodeAction"))
                nodeAction = action;
        }
    }
    if (follow == nullptr || composition == nullptr || nodeAction == nullptr)
        return;
    follow->setText(tr("Follow Active Composition"));
    follow->setToolTip(tr("Show whichever composition is active"));
    follow->setChecked(!pinned);

    const auto* active = session_.composition();
    composition->setText(active != nullptr
                             ? tr("Pin to Composition “%1”").arg(compositionName(active))
                             : tr("Pin to Composition"));
    composition->setEnabled(available && active != nullptr);
    composition->setToolTip(
        !available        ? tr("Pinning is unavailable in this window")
        : active == nullptr ? tr("There is no active composition to pin")
                            : tr("Keep showing this composition whatever becomes active"));
    composition->setChecked(pin_.mode == ViewerPinMode::Composition);

    const auto* selected = session_.selectedNode();
    const bool pixelNode = selected != nullptr && active != nullptr && pixelOutputOf(*selected);
    nodeAction->setText(pixelNode ? tr("Pin to Node “%1”").arg(node_editor::nodeDisplayName(*active, *selected))
                                  : tr("Pin to Selected Node"));
    nodeAction->setEnabled(available && pixelNode);
    nodeAction->setToolTip(!available ? tr("Pinning is unavailable in this window")
                           : selected == nullptr
                               ? tr("Select a node to show its output here")
                           : !pixelNode ? tr("The selected node has no image or mask output")
                                        : tr("Keep showing this node's output"));
    nodeAction->setChecked(pin_.mode == ViewerPinMode::Node);
}

QJsonObject ViewerEditor::saveEditorSessionState() const {
    QJsonObject state;
    if (auto pin = pinObject(pin_); !pin.isEmpty()) {
        // A pin names this project's composition and node; another project ignores it.
        pin.insert("project", QString::number(session_.snapshot().project().id().value()));
        state.insert("pin", pin);
    }
    if (ownPreview_ != nullptr && ownDisplayView_ && ownDisplayView_->size() == 2)
        state.insert("displayView", QJsonArray::fromStringList(*ownDisplayView_));
    return state;
}

void ViewerEditor::restoreEditorSessionState(const QJsonObject& state) {
    if (const auto displayView = state.value("displayView").toArray(); displayView.size() == 2 &&
                                                                       displayView[0].isString() &&
                                                                       displayView[1].isString())
        ownDisplayView_ = QStringList{displayView[0].toString(), displayView[1].toString()};
    const auto pin = state.value("pin").toObject();
    bool projectOk = false;
    const auto project = pin.value("project").toString().toULongLong(&projectOk);
    if (pin.isEmpty() || !projectOk || project != session_.snapshot().project().id().value())
        return;
    if (auto restored = pinFromObject(pin))
        (void)setPin(std::move(*restored));
}

} // namespace bloom::ui
