// #695, #721: the node editor's view controls -- the footer's zoom preset dropdown, the minimap
// overlay with its zoom row, and the two Preferences that switch them. The canvas's own zoom and pan live in
// node_editor_canvas.cpp; everything here goes through that view's functions, so the buttons, the
// value, the minimap, the wheel and the View menu can never disagree about a limit.
#include "node_editor_minimap.hpp"

#include <QAction>
#include <QSettings>
#include <QSignalBlocker>
#include <algorithm>
#include <array>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <cmath>

namespace bloom::ui {
namespace {
constexpr auto kShowZoomControlsKey = "nodes/show-zoom-controls";
constexpr auto kShowMinimapKey = "nodes/show-minimap";
constexpr double kPercent = 100.0;
} // namespace

// --- NodeGraphicsView --------------------------------------------------------------------------

void NodeGraphicsView::zoomToFactor(const double target) {
    const double current = zoomFactor();
    if (!(current > 0.0) || !(target > 0.0))
        return;
    zoomAboutViewportPoint(QRectF(viewport()->rect()).center(), target / current);
}

void NodeGraphicsView::centerOnScenePoint(const QPointF scenePoint) {
    const double scale = zoomFactor();
    const QPointF center = QRectF(viewport()->rect()).center();
    // The mapping is scene * scale + offset (no scroll offset is folded in, see the constructor),
    // so putting `scenePoint` under the viewport's center is one translation.
    setTransform(QTransform::fromScale(scale, scale) *
                 QTransform::fromTranslate(center.x() - scenePoint.x() * scale,
                                           center.y() - scenePoint.y() * scale));
    // A deliberate placement: the view is the artist's from here on, as after any pan.
    viewAdjusted_ = true;
    framedOnce_ = true;
    Q_EMIT viewChanged();
}

// --- NodeGraphEditor ---------------------------------------------------------------------------

namespace {
// #721: the footer's zoom dropdown, as before #695: Fit and these presets, largest first.
constexpr std::array<int, 4> kZoomPresets{100, 75, 50, 25};
constexpr int kFixedZoomItems = static_cast<int>(kZoomPresets.size()) + 1;
} // namespace

void NodeGraphEditor::buildViewControls() {
    footerZoomDropdown_ = new kit::KDropdown(this);
    footerZoomDropdown_->setObjectName(QStringLiteral("nodeZoomDropdown"));
    footerZoomDropdown_->setAccessibleName(tr("Zoom"));
    footerZoomDropdown_->setToolTip(tr("Zoom: Fit or a preset"));
    footerZoomDropdown_->setControlSize(kit::KDropdown::ControlSize::Dense);
    footerZoomDropdown_->setWidthFloor(kit::px(kit::Size::ViewerZoomWidth));
    footerZoomDropdown_->addItem(tr("Fit"), 0);
    for (const int percent : kZoomPresets)
        footerZoomDropdown_->addItem(tr("%1%").arg(percent), percent);
    // activated(), not currentIndexChanged(): only the artist's choice zooms; showing the current
    // zoom after a wheel step or Fit never writes back to the view.
    connect(footerZoomDropdown_, &kit::KDropdown::activated, this, [this](const int index) {
        if (index <= 0)
            view_->frameGraph();
        else
            view_->zoomToPercent(index < kFixedZoomItems
                                     ? footerZoomDropdown_->itemData(index).toInt()
                                     : footerCustomZoomPercent_);
        // Fit holds no zoom of its own: the field goes back to the zoom the view ended at.
        refreshZoomControls();
    });
    chrome_.footer.addWidget(footerZoomDropdown_);

    minimap_ = new NodeMinimap(*view_);
    connect(view_, &NodeGraphicsView::viewChanged, this, &NodeGraphEditor::refreshZoomControls);
    refreshZoomControls();

    const QSettings settings;
    applyViewControls(settings.value(QLatin1String(kShowZoomControlsKey), true).toBool(),
                      settings.value(QLatin1String(kShowMinimapKey), true).toBool());
}

void NodeGraphEditor::refreshZoomControls() {
    if (footerZoomDropdown_ == nullptr)
        return;
    // The closed field always shows the current zoom: a preset when the view sits on one, and
    // otherwise one trailing item carrying the current percentage, renamed in place.
    const int percent = static_cast<int>(std::lround(view_->zoomFactor() * kPercent));
    int target = -1;
    for (int index = 1; index < kFixedZoomItems; ++index)
        if (footerZoomDropdown_->itemData(index).toInt() == percent)
            target = index;
    const QSignalBlocker blocker(footerZoomDropdown_);
    if (target < 0) {
        const QString label = tr("%1%").arg(percent);
        if (footerZoomDropdown_->count() > kFixedZoomItems) {
            footerZoomDropdown_->setItemText(kFixedZoomItems, label);
        } else {
            footerZoomDropdown_->addItem(label, percent);
        }
        footerCustomZoomPercent_ = percent;
        target = kFixedZoomItems;
    }
    if (footerZoomDropdown_->currentIndex() != target)
        footerZoomDropdown_->setCurrentIndex(target);
    footerZoomDropdown_->update();
}

void NodeGraphEditor::applyViewControls(const bool zoomControls, const bool minimap) {
    // #721: the minimap preference switches the minimap together with its zoom row; the zoom
    // controls preference switches the zoom row alone.
    if (minimap_ != nullptr) {
        minimap_->setZoomRowEnabledByPreference(zoomControls);
        minimap_->setEnabledByPreference(minimap);
    }
}

NodeMinimap* NodeGraphEditor::minimapForTest() const noexcept { return minimap_; }

} // namespace bloom::ui
