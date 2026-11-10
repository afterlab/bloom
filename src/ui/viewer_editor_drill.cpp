// #645 Drilling into grouped and SVG artwork in the Viewer (docs/user-guide/viewer.md, "Drilling
// into groups"; docs/ux/interaction-model.md, Viewer). The level lives in the session
// (CompositionSession::viewerLevel); the Viewer turns clicks, double-clicks, Enter and Escape into
// level changes and selections. Inside a group a click is answered by runtime::ViewerPicker on a
// worker: the UI thread maps the click, submits the task and applies its result.

#include <bloom/ui/viewer_editor.hpp>

#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/document/drill_levels.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/project.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/kit/dialog_prompts.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPointer>
#include <QTimer>

#include <algorithm>

namespace bloom::ui {

namespace {
[[nodiscard]] bool isTextLayer(const CompositionSession& session, const document::LayerId layer) {
    const auto* composition = session.composition();
    const auto source =
        composition ? document::layerContentSource(*composition, layer) : std::nullopt;
    const auto* node = source ? composition->graph().findNode(*source) : nullptr;
    return node != nullptr && node->typeId == document::kTextSourceNodeType;
}
} // namespace

std::optional<ViewerMapping> ViewerEditor::drillMapping() const {
    const auto actual = actualDisplayDescriptor();
    if (!actual || !actual->frame || !actual->mapping || !frameInCurrentDomain(actual->frame) ||
        actual->frame->desiredIdentity().time != session_.currentTime() ||
        actual->captureRect != contentRect() || actual->devicePixelRatio != devicePixelRatioF())
        return std::nullopt;
    const auto* shown = shownComposition();
    if (shown == nullptr || actual->mapping->compositionFormat != shown->format())
        return std::nullopt;
    return actual->mapping;
}

void ViewerEditor::cancelDrillPick() {
    if (drillPickTask_)
        drillPickTask_->cancel();
    drillPickTask_.reset();
    drillPick_.reset();
}

void ViewerEditor::submitDrillPick(const DrillPick::Intent intent, const QPointF screen,
                                   const ViewerMapping& mapping, const bool extend,
                                   const Qt::KeyboardModifiers modifiers) {
    cancelDrillPick();
    const auto actual = actualDisplayDescriptor();
    const auto frame = actual ? actual->frame : nullptr;
    if (!frameInCurrentDomain(frame) || frame->desiredIdentity().time != session_.currentTime())
        return;
    // Half a gizmo handle on screen, in composition pixels.
    const auto width = static_cast<double>(mapping.compositionFormat.width());
    const auto scale = width > 0.0 ? mapping.displayRect.width() / width : 0.0;
    runtime::ViewerPickRequest request{
        .snapshot = session_.snapshot(),
        .composition = session_.compositionId(),
        .time = session_.currentTime(),
        .level = session_.viewerLevel().groups,
        .point = mapping.toComposition(screen),
        .tolerance = scale > 0.0 ? kit::px(kit::Size::GizmoHandle) / 2.0 / scale : 0.0,
        .bounds = previewController_->currentLayerBounds(frame),
        .plan = frame->processIdentity().plan};
    DrillPick pick{intent,        screen,
                   mapping,       request.snapshot.revision(),
                   request.time,  request.composition,
                   request.level, extend,
                   modifiers};
    auto submission = previewController_->submitViewerPick(std::move(request));
    if (!submission.accepted())
        return;
    drillPickTask_ = std::move(submission.handle);
    drillPick_ = std::move(pick);
}

void ViewerEditor::consumeDrillPick() {
    if (!drillPickTask_)
        return;
    auto result = drillPickTask_->tryTakeResult();
    if (!result)
        return;
    drillPickTask_.reset();
    const auto pick = std::exchange(drillPick_, std::nullopt);
    const auto& value = result->value();
    if (!pick || result->state() != runtime::TaskState::Succeeded || !value || !*value)
        return;
    // A result for a frame, level or document the Viewer no longer shows is dropped.
    if (pick->revision != session_.snapshot().revision() || pick->time != session_.currentTime() ||
        pick->composition != session_.compositionId() ||
        pick->level != session_.viewerLevel().groups)
        return;
    applyDrillPick(*pick, **value);
}

void ViewerEditor::applyDrillPick(const DrillPick& pick, const runtime::ViewerPickResult& result) {
    if (result.status != runtime::ViewerPickStatus::Picked)
        return;
    switch (pick.intent) {
    case DrillPick::Intent::Select:
        if (!result.child) {
            if (!pick.extend)
                session_.clearSelectionFromViewer();
            break;
        }
        session_.selectLayerFromViewer(*result.child, pick.extend);
        // The button is still down: the move starts from the press point, so nothing jumps.
        if (drillPressDown_ && !pick.extend)
            (void)beginSelectionMove(*result.child, pick.mapping, pick.screen, pick.modifiers);
        break;
    case DrillPick::Intent::Reveal:
        if (result.child)
            session_.selectLayerFromViewer(*result.child);
        break;
    case DrillPick::Intent::Drill:
        if (!result.child) {
            (void)session_.leaveViewerLevel();
            break;
        }
        (void)drillInto(*result.child, pick.screen);
        break;
    }
    update();
}

bool ViewerEditor::beginSelectionMove(const document::LayerId layer, const ViewerMapping& mapping,
                                      const QPointF pressScreen,
                                      const Qt::KeyboardModifiers modifiers) {
    const auto current = drillMapping();
    const auto actual = actualDisplayDescriptor();
    if (!current || *current != mapping || !actual || !actual->frame || dragActive_ || panActive_ ||
        gpuNativeRetirePending_ || gpuHostMutationPending_)
        return false;
    const auto selected = previewController_->selectedLayerBounds(actual->frame);
    const auto bounds =
        std::ranges::find(selected, layer, &runtime::EvaluatedOperationBounds::layerId);
    if (bounds == selected.end())
        return false;
    if (session_.beginTransformInteraction(
            {TransformGesture::Kind::Move, 0, pressScreen, *bounds}, mapping,
            {modifiers.testFlag(Qt::ShiftModifier), modifiers.testFlag(Qt::AltModifier)}))
        return false;
    dragActive_ = true;
    dragPointer_ = pressScreen;
    activeMapping_ = mapping;
    activeGestureIdentity_ = actual->frame->desiredIdentity();
    activeGestureViewport_ = contentRect();
    activeGestureTransform_ = transform_;
    activeGestureSelectionRevision_ = gpuOverlayRevision_;
    previewController_->beginInteractiveScrub();
    return true;
}

bool ViewerEditor::drillPress(QMouseEvent* event, const ViewerMapping& mapping,
                              const ViewerHit& hit) {
    if (session_.viewerLevel().groups.empty())
        return false;
    drillRepickPress_.reset();
    if (hit.region != ViewerHitRegion::Empty) {
        // The selection's own handles and box answer at once; a click without a drag picks again,
        // so the topmost child under the pointer is selected.
        if (hit.region == ViewerHitRegion::Move)
            drillRepickPress_ = event->position();
        return false;
    }
    drillPressDown_ = true;
    submitDrillPick(DrillPick::Intent::Select, event->position(), mapping,
                    event->modifiers().testFlag(Qt::ShiftModifier), event->modifiers());
    event->accept();
    return true;
}

bool ViewerEditor::drillDoubleClick(QMouseEvent* event) {
    if (tool() != ViewerTool::Select || event->button() != Qt::LeftButton || isReferenceOnly() ||
        creation_ || textEdit_)
        return false;
    const auto mapping = drillMapping();
    if (!mapping || !contentRect().contains(event->position()))
        return false;
    endDrag(false);
    if (!session_.viewerLevel().groups.empty()) {
        drillPressDown_ = false;
        submitDrillPick(DrillPick::Intent::Drill, event->position(), *mapping, false,
                        event->modifiers());
        event->accept();
        return true;
    }
    // The top level picks synchronously, as a click there does.
    const auto hit = hitAt(*mapping, event->position());
    if (!hit.layerId.isValid()) {
        if (session_.leaveViewerLevel()) {
            event->accept();
            return true;
        }
        return false;
    }
    if (isTextLayer(session_, hit.layerId))
        return false; // Text editing, unchanged.
    if (drillInto(hit.layerId, event->position())) {
        event->accept();
        return true;
    }
    return false;
}

bool ViewerEditor::drillInto(const document::LayerId layer, const std::optional<QPointF> screen) {
    const auto* composition = session_.composition();
    if (composition == nullptr)
        return false;
    if (commands::findLayerVectorArtwork(*composition, layer) != nullptr) {
        session_.selectLayerFromViewer(layer);
        offerBreakApart(layer, screen);
        return true;
    }
    if (document::isDrillGroup(*composition, layer)) {
        enterGroup(layer, screen);
        return true;
    }
    if (document::drillShape(*composition, layer)) {
        if (session_.enterViewerPoints(layer)) {
            notifyAnchorModeState();
            return true;
        }
        return false;
    }
    session_.selectLayerFromViewer(layer);
    if (isTextLayer(session_, layer))
        return beginTextEditing(screen);
    return true;
}

void ViewerEditor::enterGroup(const document::LayerId group, const std::optional<QPointF> screen) {
    const auto mapping = drillMapping();
    auto level = session_.viewerLevel();
    level.groups.push_back(group);
    level.points.reset();
    if (!session_.setViewerLevel(level).ok())
        return;
    session_.clearSelectionFromViewer();
    if (screen) {
        if (mapping)
            submitDrillPick(DrillPick::Intent::Reveal, *screen, *mapping, false, Qt::NoModifier);
        return;
    }
    // Enter: the topmost child.
    if (const auto* composition = session_.composition())
        if (const auto merge = document::layerContentMerge(*composition, group)) {
            const auto children = document::drillChildren(*composition, *merge);
            if (!children.empty())
                session_.selectLayerFromViewer(children.front());
        }
}

bool ViewerEditor::drillKey(QKeyEvent* event) {
    if (tool() != ViewerTool::Select || creation_ || textEdit_ || dragActive_ || panActive_ ||
        anchorGesture_ || isReferenceOnly())
        return false;
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) {
        const auto* primary = std::get_if<document::LayerId>(&session_.selection().primary);
        const auto* composition = session_.composition();
        if (primary == nullptr || composition == nullptr || isTextLayer(session_, *primary))
            return false;
        if (session_.viewerLevel().points)
            return false; // Already editing points.
        const bool group = document::isDrillGroup(*composition, *primary);
        const bool shape = !group && document::drillShape(*composition, *primary).has_value();
        const bool artwork = commands::findLayerVectorArtwork(*composition, *primary) != nullptr;
        if (!group && !shape && !artwork)
            return false;
        // The selection may sit anywhere (the Timeline selected it): its own level first.
        const auto layer = *primary;
        if (session_.viewerLevel().groups != session_.enclosingGroups(layer)) {
            if (!session_.setViewerLevel({session_.enclosingGroups(layer), std::nullopt}).ok())
                return false;
        }
        (void)drillInto(layer, std::nullopt);
        event->accept();
        return true;
    }
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
        if (drillPickTask_) {
            cancelDrillPick();
            drillPressDown_ = false;
        }
        if (session_.leaveViewerLevel()) {
            notifyAnchorModeState();
            event->accept();
            return true;
        }
    }
    return false;
}

void ViewerEditor::offerBreakApart(const document::LayerId layer,
                                   const std::optional<QPointF> screen) {
    auto* assets = session_.assetController();
    const auto* composition = session_.composition();
    const auto* boundary = composition ? composition->graph().findLayer(layer) : nullptr;
    if (assets == nullptr || boundary == nullptr)
        return;
    kit::KConfirmSpec spec;
    spec.title = tr("Break Apart SVG");
    spec.objectName = QStringLiteral("viewerBreakApartOffer");
    spec.tone = kit::KPromptTone::Question;
    spec.heading = tr("Break apart “%1” into editable shapes to edit its parts?")
                       .arg(QString::fromStdString(boundary->name));
    spec.detail = tr("Each element becomes a shape layer with its own path node, nested as in the "
                     "file. The layer keeps its transform and keys; Undo restores the one-layer "
                     "SVG.");
    spec.acceptText = tr("Break Apart");
    spec.rejectText = tr("Cancel");
    const BreakApartDrill pending{session_.compositionId(), layer, session_.viewerLevel().groups,
                                  screen, false};
    QPointer<ViewerEditor> self(this);
    kit::confirm(*this, spec, [self, pending](const kit::KDialog::Role role) {
        if (!self || role != kit::KDialog::Role::Accept)
            return;
        auto* controller = self->session_.assetController();
        if (controller == nullptr || self->session_.compositionId() != pending.composition)
            return;
        self->breakApartDrill_ = pending;
        QObject::disconnect(self->brokenApartConnection_);
        self->brokenApartConnection_ =
            QObject::connect(controller, &AssetController::brokenApart, self,
                             [view = self.data()](const document::CompositionId composition,
                                                  const std::vector<document::LayerId>& layers) {
                                 view->onBrokenApart(composition, layers);
                             });
        controller->breakApartSvg(pending.composition, std::vector{pending.layer});
    });
}

void ViewerEditor::onBrokenApart(const document::CompositionId composition,
                                 const std::vector<document::LayerId>& layers) {
    QObject::disconnect(brokenApartConnection_);
    if (!breakApartDrill_ || breakApartDrill_->composition != composition ||
        std::ranges::find(layers, breakApartDrill_->layer) == layers.end() ||
        session_.compositionId() != composition) {
        breakApartDrill_.reset();
        return;
    }
    breakApartDrill_->landed = true;
    auto level = ViewerLevel{breakApartDrill_->level, std::nullopt};
    level.groups.push_back(breakApartDrill_->layer);
    if (!session_.setViewerLevel(level).ok()) {
        breakApartDrill_.reset();
        return;
    }
    session_.clearSelectionFromViewer();
    if (!breakApartDrill_->screen) {
        // Enter: the topmost part is selected.
        const auto layer = breakApartDrill_->layer;
        breakApartDrill_.reset();
        if (const auto* current = session_.composition())
            if (const auto merge = document::layerContentMerge(*current, layer)) {
                const auto children = document::drillChildren(*current, *merge);
                if (!children.empty())
                    session_.selectLayerFromViewer(children.front());
            }
        return;
    }
    continueBreakApartDrill();
}

// The point is picked once the frame of the broken-apart document is on screen. A frame that is
// ready but not yet painted is checked again shortly, for a bounded time.
void ViewerEditor::continueBreakApartDrill() {
    if (!breakApartDrill_ || !breakApartDrill_->landed)
        return;
    const auto mapping = drillMapping();
    const auto actual = actualDisplayDescriptor();
    const auto revision = actual && actual->frame
                              ? std::optional(actual->frame->desiredIdentity().sourceRevision)
                              : std::nullopt;
    if (!mapping || !revision ||
        (*revision != session_.snapshot().revision() &&
         *revision != session_.evaluationSnapshotForTime(session_.currentTime()).revision())) {
        constexpr int kRetries = 200;
        constexpr int kRetryMs = 15;
        if (breakApartDrill_->retries++ < kRetries) {
            QPointer<ViewerEditor> self(this);
            QTimer::singleShot(kRetryMs, this, [self] {
                if (self)
                    self->continueBreakApartDrill();
            });
        } else {
            breakApartDrill_.reset();
        }
        return;
    }
    const auto screen = *breakApartDrill_->screen;
    breakApartDrill_.reset();
    submitDrillPick(DrillPick::Intent::Reveal, screen, *mapping, false, Qt::NoModifier);
}

} // namespace bloom::ui
