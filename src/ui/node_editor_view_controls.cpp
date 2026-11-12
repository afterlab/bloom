// #695: the node editor's view controls -- the footer's zoom out / zoom value / zoom in group, the
// minimap overlay, and the two Preferences that switch them. The canvas's own zoom and pan live in
// node_editor_canvas.cpp; everything here goes through that view's functions, so the buttons, the
// value, the minimap, the wheel and the View menu can never disagree about a limit.
#include "node_editor_minimap.hpp"

#include <QAction>
#include <QSettings>
#include <QSignalBlocker>
#include <algorithm>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/value_field.hpp>
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

void NodeGraphEditor::buildViewControls() {
    constexpr std::array<int, 5> kZoomPresets{25, 50, 100, 200, 400};
    const auto button = [this](const kit::IconId icon, const QString& name, const QString& text) {
        auto* control = new kit::KIconButton(this);
        control->setObjectName(name);
        control->setIcon(kit::icon(icon, kit::IconRole::Chrome));
        control->setToolTip(text);
        control->setAccessibleName(text);
        return control;
    };

    footerZoomOutButton_ = button(kit::IconId::Subtract, QStringLiteral("nodeZoomOutButton"),
                                  tr("Zoom Out"));
    connect(footerZoomOutButton_, &QAbstractButton::clicked, this, [this] { view_->zoomStep(-1); });

    // The value: a kit value field that scrubs along the vertical axis (drag up to zoom in), takes
    // a typed percentage, and resets to 100%. Fit and the presets are its context menu.
    footerZoomValue_ = new kit::KValueField(this);
    auto* field = footerZoomValue_;
    field->setObjectName(QStringLiteral("nodeZoomValue"));
    field->setAccessibleName(tr("Zoom"));
    field->setToolTip(tr("Zoom. Drag up or down, or type a percentage."));
    field->setCompact(true);
    field->setOutlinedAtRest(true);
    field->setScrubAxis(kit::KValueField::ScrubAxis::Vertical);
    field->setRange(ViewTransform::kMinZoom * kPercent, ViewTransform::kMaxZoom * kPercent);
    field->setSingleStep(1.0);
    field->setDecimals(0);
    field->setUnit(QStringLiteral("%"));
    field->setDefaultValue(kPercent);
    field->setResetText(tr("Actual Size"));
    field->setFixedWidth(kit::px(kit::Size::ViewerZoomWidth));
    connect(field, &kit::KValueField::valueChanged, this, [this](const double percent) {
        if (!zoomSyncing_)
            view_->zoomToFactor(percent / kPercent);
    });
    auto* fit = new QAction(tr("Fit"), field);
    fit->setObjectName(QStringLiteral("nodeZoomFitPresetAction"));
    connect(fit, &QAction::triggered, this, [this] { view_->frameGraph(); });
    field->addAction(fit);
    for (const int percent : kZoomPresets) {
        auto* preset = new QAction(tr("%1%").arg(percent), field);
        preset->setObjectName(QStringLiteral("nodeZoomPreset%1Action").arg(percent));
        connect(preset, &QAction::triggered, this,
                [this, percent] { view_->zoomToPercent(percent); });
        field->addAction(preset);
    }

    footerZoomInButton_ =
        button(kit::IconId::Add, QStringLiteral("nodeZoomInButton"), tr("Zoom In"));
    connect(footerZoomInButton_, &QAbstractButton::clicked, this, [this] { view_->zoomStep(1); });

    chrome_.footer.addWidget(footerZoomOutButton_);
    chrome_.footer.addWidget(footerZoomValue_);
    chrome_.footer.addWidget(footerZoomInButton_);

    minimap_ = new NodeMinimap(*view_);
    connect(view_, &NodeGraphicsView::viewChanged, this, &NodeGraphEditor::refreshZoomControls);
    refreshZoomControls();

    const QSettings settings;
    applyViewControls(settings.value(QLatin1String(kShowZoomControlsKey), true).toBool(),
                      settings.value(QLatin1String(kShowMinimapKey), true).toBool());
}

void NodeGraphEditor::refreshZoomControls() {
    if (footerZoomValue_ == nullptr)
        return;
    const double zoom = view_->zoomFactor();
    zoomSyncing_ = true;
    // A projection, never an edit: a field with a live scrub keeps the value it is scrubbing.
    footerZoomValue_->setValue(zoom * kPercent);
    zoomSyncing_ = false;
    footerZoomOutButton_->setEnabled(zoom > ViewTransform::kMinZoom * (1.0 + 1e-9));
    footerZoomInButton_->setEnabled(zoom < ViewTransform::kMaxZoom * (1.0 - 1e-9));
}

void NodeGraphEditor::applyViewControls(const bool zoomControls, const bool minimap) {
    // The footer row owns visibility: a suppressed control leaves the row and the rest re-flows.
    for (QWidget* control : {static_cast<QWidget*>(footerZoomOutButton_),
                             static_cast<QWidget*>(footerZoomValue_),
                             static_cast<QWidget*>(footerZoomInButton_)})
        if (control != nullptr && control->property("chromeSuppressed").toBool() == zoomControls)
            control->setProperty("chromeSuppressed", !zoomControls);
    if (minimap_ != nullptr)
        minimap_->setEnabledByPreference(minimap);
}

NodeMinimap* NodeGraphEditor::minimapForTest() const noexcept { return minimap_; }

} // namespace bloom::ui
