#include "asset_drop.hpp"
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/channel_set_controller.hpp>
#include <bloom/ui/cryptomatte_controller.hpp>
#include <bloom/ui/image_preview_controller.hpp>
#include <bloom/ui/node_editor.hpp>

#include <bloom/ui/composition_editors.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <bloom/ui/kit/color.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/value_field.hpp>

#include <bloom/core/color.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/project.hpp>

#include <QAction>
#include <QBrush>
#include <QColor>
#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointer>
#include <QResizeEvent>
#include <QSettings>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "node_editor_interactions.hpp"

namespace bloom::ui {
using namespace node_editor;
namespace {
// Task NODES-1, deliverable 3: the grid's own pitch is now the artist's setting
// (NodeGraphicsScene::gridSize(), QSettings "nodes/grid-size", default 16 design px) rather than a
// fixed token; kGridMinimumDeviceSpacing is still the floor below which the dots are hidden rather
// than painted as a solid smear.
constexpr qreal kGridMinimumDeviceSpacing = kit::px(kit::Spacing::M);
constexpr qreal kGridDotRadius = kit::kHairlineWidth;
} // namespace

// --- NodeGraphicsScene -------------------------------------------------------------------------

NodeGraphicsScene::NodeGraphicsScene(QObject* parent)
    : QGraphicsScene(parent), interaction_(std::make_unique<NodeInteraction>()),
      index_(std::make_unique<NodeCanvasIndex>()) {
    setObjectName("nodeGraphicsScene");
    setBackgroundBrush(kit::color(kit::Color::Background));
    setItemIndexMethod(QGraphicsScene::BspTreeIndex);
}

void NodeGraphicsScene::setSession(CompositionSession* session) { session_ = session; }

void NodeGraphicsScene::drawBackground(QPainter* painter, const QRectF& rect) {
    // The canvas surface itself stays the scene's own Background brush; the grid is drawn on top of
    // it, one step up the surface ladder, so it reads as texture rather than as a second color.
    QGraphicsScene::drawBackground(painter, rect);

    const qreal scale = painter->worldTransform().m11();
    if (!(scale > 0.0) || gridSize_ * scale < kGridMinimumDeviceSpacing) {
        return;
    }
    // Integer step counts, not a floating-point loop variable: repeatedly adding a pitch to a
    // double accumulates error across a wide exposed rectangle, and the dots would slowly drift off
    // the lattice the far side of the canvas is drawn on.
    const qreal first = std::floor(rect.left() / gridSize_) * gridSize_;
    const qreal top = std::floor(rect.top() / gridSize_) * gridSize_;
    const auto columns = static_cast<int>(std::floor((rect.right() - first) / gridSize_)) + 1;
    const auto lines = static_cast<int>(std::floor((rect.bottom() - top) / gridSize_)) + 1;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(kit::color(kit::surfaceStep(kit::Color::Background, 2)));
    for (int column = 0; column < columns; ++column) {
        const qreal x = first + static_cast<qreal>(column) * gridSize_;
        for (int line = 0; line < lines; ++line) {
            const qreal y = top + static_cast<qreal>(line) * gridSize_;
            painter->drawEllipse(QPointF(x, y), kGridDotRadius, kGridDotRadius);
        }
    }
    painter->restore();
}

void NodeGraphicsScene::setGridSize(const qreal size) {
    const qreal clamped = std::max(1.0, size);
    if (qFuzzyCompare(gridSize_ + 1.0, clamped + 1.0)) {
        return;
    }
    gridSize_ = clamped;
    update();
}

// --- NodeGraphEditor ---------------------------------------------------------------------------

NodeGraphEditor::NodeGraphEditor(CompositionSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
    setObjectName("nodeGraphEditor");
    setAccessibleName(tr("Nodes editor"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    scene_ = new NodeGraphicsScene(this);
    scene_->setSession(&session_);
    // Production submission (task N3): every gesture transaction goes through the session's one
    // public node-transaction seam, so publication and refusal reporting match Properties and the
    // Timeline. Tests may replace this with an adapter through setSubmit().
    scene_->setSubmit([this](commands::Transaction&& transaction) {
        return session_.executeNodeTransaction(std::move(transaction));
    });
    view_ = new NodeGraphicsView(this);
    view_->setScene(scene_);
    installAssetDropTarget(*view_->viewport(), session_, view_);
    if (auto* controller = session_.assetController()) {
        // A worker result (a thumbnail, an image's probed parts) also refills the cards' probe
        // selectors.
        connect(controller, &AssetController::changed, this, [this] {
            scene_->refreshValues();
            scene_->update();
        });
    }
    if (auto* cryptomatte = session_.cryptomatteController())
        connect(cryptomatte, &CryptomatteController::changed, scene_,
                &NodeGraphicsScene::refreshValues);
    // #487: the Shuffle slot pickers and channel checklists refill when a listing arrives.
    if (auto* channels = session_.channelSetController())
        connect(channels, &ChannelSetController::changed, scene_,
                &NodeGraphicsScene::refreshValues);
    // #490: an Image Preview card repaints when its prepared image arrives or what it shows
    // moves; the background repaints with it. Painting only reads what is prepared.
    if (auto* previews = session_.imagePreviews()) {
        view_->setImagePreviews(previews);
        // #685: the playhead alone does not repaint a card: its picture is requested here, and
        // the card repaints once, when a different picture arrives.
        connect(&session_, &CompositionSession::currentTimeChanged, this, [this, previews] {
            if (!isVisible())
                return;
            for (auto* item : scene_->nodeItems())
                if (item->isImagePreview())
                    previews->refresh(item->id());
            if (const auto background = previews->backgroundNode())
                previews->refresh(*background);
        });
        connect(previews, &ImagePreviewController::previewChanged, this, [this] {
            if (!isVisible())
                return;
            for (auto* item : scene_->nodeItems())
                if (item->isImagePreview())
                    item->update();
            if (!view_->backgroundRect().isEmpty())
                view_->viewport()->update();
        });
        // A newly chosen background starts fitted; hiding or showing it keeps its view.
        connect(previews, &ImagePreviewController::backgroundChanged, this,
                [this, previews, shown = std::optional<document::NodeId>{}]() mutable {
                    if (previews->backgroundNode() != shown) {
                        shown = previews->backgroundNode();
                        view_->fitBackground();
                    }
                    view_->viewport()->update();
                });
    }
    layout->addWidget(view_);

    connect(&session_, &CompositionSession::snapshotChanged, this, &NodeGraphEditor::rebuild);
    // #720: a projection that waited for a gesture to end.
    connect(scene_, &NodeGraphicsScene::projectionDue, this, &NodeGraphEditor::rebuild);
    // #694: another composition (or project) opens at the opening frame, as a fresh panel does.
    connect(&session_, &CompositionSession::compositionChanged, this, [this] {
        // A placement belongs to the composition it was started in.
        scene_->cancelGesture();
        view_->resetFraming();
        rebuild();
    });
    connect(&session_, &CompositionSession::selectionChanged, this,
            &NodeGraphEditor::updateSelection);
    // The playhead is not a document change, so it is deliberately NOT a projection rebuild -- but
    // an animated cell shows the curve's value AT the session time and its diamond says whether a
    // key sits there, so both have to follow the time or the canvas quietly disagrees with the
    // Properties panel and the timeline about what the frame currently holds.
    // ... but only the cards whose values move with time, coalesced into one refresh per display
    // frame, and none at all while the panel is hidden (bug #401).
    timeRefresh_ = new QTimer(this);
    timeRefresh_->setSingleShot(true);
    timeRefresh_->setInterval(0);
    connect(timeRefresh_, &QTimer::timeout, scene_, [this] { scene_->refreshTimeValues(); });
    connect(&session_, &CompositionSession::currentTimeChanged, this,
            &NodeGraphEditor::scheduleTimeRefresh);
    connect(scene_, &QGraphicsScene::selectionChanged, this,
            &NodeGraphEditor::sceneSelectionChanged);
    connect(view_, &NodeGraphicsView::contextMenuRequested, this,
            &NodeGraphEditor::showContextMenu);

    connect(view_, &NodeGraphicsView::viewChanged, scene_,
            [this] { scene_->refreshTimeValues(true); });
    connect(view_, &NodeGraphicsView::canvasKeyPressed, this, &NodeGraphEditor::handleCanvasKey);
    // A held gesture ends when the canvas loses focus. #703: a placement does not: no button is
    // held, the Add surface that started it takes and returns focus as it closes, and the artist
    // ends it themselves -- a click places it, Escape or a right-click cancels it.
    connect(view_, &NodeGraphicsView::canvasFocusLost, scene_, [this] {
        if (!scene_->placementActive())
            scene_->cancelGesture();
    });
    connect(scene_, &NodeGraphicsScene::addSearchRequested, this, &NodeGraphEditor::openAddSearch);
    connect(scene_, &NodeGraphicsScene::rerouteRequested, this, &NodeGraphEditor::insertReroute);

    // Task NODES-1: persisted chrome settings, read once here -- before buildHeaderMenus() and
    // buildFooter() below, so every control they build already reflects the artist's own last
    // choice instead of flashing the defaults for one frame. A short-lived QSettings, matching
    // apps/bloom/main.cpp's own ramPreviewByteBudget precedent for a leaf widget that has no reason
    // to carry a QSettings reference of its own.
    {
        const QSettings settings;
        scene_->setLinkStyle(NodeGraphEditor::linkStyleFromSettingsValue(
            settings.value(QStringLiteral("nodes/link-style")).toString()));
        scene_->setGridSnapEnabled(settings.value(QStringLiteral("nodes/snap"), false).toBool());
        scene_->setGridSize(settings.value(QStringLiteral("nodes/grid-size"), 16.0).toDouble());
    }
    buildHeaderMenus();
    buildFooter();

    rebuild();
}

NodeGraphEditor::~NodeGraphEditor() { scene_->blockSignals(true); }

void NodeGraphEditor::scheduleTimeRefresh() {
    if (!isVisible()) {
        hiddenTimeChange_ = true;
        return;
    }
    if (!timeRefresh_->isActive())
        timeRefresh_->start();
}

void NodeGraphEditor::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (hiddenTimeChange_) {
        hiddenTimeChange_ = false;
        scene_->refreshTimeValues();
    }
}

NodeGraphicsScene* NodeGraphEditor::graphScene() const noexcept { return scene_; }

void NodeGraphEditor::applyApplicationPreferences(const ApplicationPreferences& preferences) {
    if (scene_ == nullptr) {
        return;
    }
    // Reuse the same string mapping the constructor and the View menu use, so a preference applied
    // here and one chosen from the menu can never disagree.
    scene_->setLinkStyle(
        linkStyleFromSettingsValue(QLatin1String(nodeLinkStyleValue(preferences.nodeLinkStyle))));
    scene_->setGridSnapEnabled(preferences.nodeSnap);
    scene_->setGridSize(preferences.nodeGridSize);
    applyViewControls(preferences.nodeShowZoomControls, preferences.nodeShowMinimap);
}

NodeGraphicsView* NodeGraphEditor::graphView() const noexcept { return view_; }

void NodeGraphEditor::rebuild() {
    rebuilding_ = true;
    scene_->setProjection(session_.snapshot(), session_.compositionId());
    updateSelection();
    rebuilding_ = false;
    if (!view_->viewAdjusted()) {
        view_->reframe();
    }
}

void NodeGraphEditor::updateSelection() {
    const bool wasRebuilding = rebuilding_;
    rebuilding_ = true;
    for (auto* item : scene_->nodeItems()) {
        item->setSelected(session_.selectedNodes().contains(item->id()));
        const auto* selected = session_.selectedNode();
        item->setPrimary(selected && selected->id == item->id());
    }
    rebuilding_ = wasRebuilding;
}

void NodeGraphEditor::sceneSelectionChanged() {
    if (rebuilding_) {
        return;
    }
    std::set<document::NodeId> nodes;
    for (auto* candidate : scene_->selectedItems())
        if (auto* item = dynamic_cast<NodeItem*>(candidate))
            nodes.insert(item->id());
    if (nodes.empty()) {
        session_.clearSelection();
        return;
    }
    const auto* primary = session_.selectedNode();
    session_.selectNodes(nodes,
                         primary && nodes.contains(primary->id) ? primary->id : *nodes.begin());
}

} // namespace bloom::ui
