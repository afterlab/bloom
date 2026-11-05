#include <bloom/ui/viewer_editor.hpp>

#include "layer_mask_support.hpp"

#include <bloom/document/depth_effect_nodes.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace bloom::ui {
namespace {

constexpr std::string_view kFocusPointRole = "focusPoint";
constexpr std::string_view kFocusFromRole = "focusFrom";
constexpr std::string_view kModeRole = "mode";

[[nodiscard]] const document::NodeRecord* zDefocusNode(const CompositionSession& session,
                                                       const std::optional<document::NodeId> id) {
    const auto* composition = session.composition();
    const auto* node =
        composition != nullptr && id ? composition->graph().findNode(*id) : nullptr;
    return node != nullptr && node->typeId == document::kZDefocusNodeType ? node : nullptr;
}

[[nodiscard]] std::optional<std::int64_t> liveInteger(const CompositionSession& session,
                                                      const document::NodeId node,
                                                      const std::string_view role) {
    const auto id = nodeRoleParameter(session, node, role);
    const auto value = id ? session.liveValue(*id) : std::nullopt;
    const auto* stored = value ? std::get_if<std::int64_t>(&*value) : nullptr;
    return stored ? std::optional(*stored) : std::nullopt;
}

[[nodiscard]] std::optional<document::Vec2d> liveVec2(const CompositionSession& session,
                                                      const document::NodeId node,
                                                      const std::string_view role) {
    const auto id = nodeRoleParameter(session, node, role);
    const auto value = id ? session.liveValue(*id) : std::nullopt;
    const auto* stored = value ? std::get_if<document::Vec2d>(&*value) : nullptr;
    return stored ? std::optional(*stored) : std::nullopt;
}

} // namespace

// #483: a Z Defocus's focus point is a full-resolution offset from its frame's centre, the frame
// its depth is read in. The Viewer maps the click into that frame the way the Cryptomatte
// eyedropper does: through the world transform of the layer that displays the node (moves,
// scales, rotations and parents included), or directly when the node's own output is inspected
// or no layer displays it. The node then reads the mean depth of the 5 x 5 pixels around the
// point when it renders, so a pick lands on the subject rather than on one noisy or edge pixel.
std::optional<QTransform> ViewerEditor::focusFrameToComposition(const document::NodeId node) const {
    const auto& inspection = previewController_->viewerChannelInspection();
    if (inspection && inspection->node == node)
        return QTransform();
    if (inspection)
        return std::nullopt;
    const auto layer = session_.layerForNode(node);
    if (!layer)
        return QTransform();
    const auto actual = actualDisplayDescriptor();
    const auto bounds = previewController_->currentLayerBounds(actual ? actual->frame : nullptr);
    const auto found = std::ranges::find(bounds, *layer, &runtime::EvaluatedOperationBounds::layerId);
    if (found == bounds.end())
        return QTransform();
    return masks::contentToComposition(*found);
}

QPointF ViewerEditor::focusFrameCentre() const {
    const auto* composition = session_.composition();
    if (composition == nullptr)
        return {};
    const auto format = composition->format();
    return {0.5 * static_cast<double>(format.width()), 0.5 * static_cast<double>(format.height())};
}

void ViewerEditor::focusPickChanged() {
    const bool active = zDefocusNode(session_, session_.focusPickNode()) != nullptr;
    if (active && !isReferenceOnly()) {
        setFocus(Qt::OtherFocusReason);
        setCursor(Qt::CrossCursor);
    } else {
        updatePanCursor();
    }
    update();
}

bool ViewerEditor::focusPickPress(QMouseEvent* event) {
    const auto* node = zDefocusNode(session_, session_.focusPickNode());
    if (node == nullptr || event->button() != Qt::LeftButton)
        return false;
    event->accept();
    setFocus(Qt::MouseFocusReason);
    const auto showMessage = [this, event](const QString& message) {
        QToolTip::showText(mapToGlobal(event->position().toPoint()), message, this);
    };
    auto mapping = currentMapping();
    if (const auto actual = actualDisplayDescriptor();
        !mapping && actual && actual->mapping && session_.composition() != nullptr &&
        actual->mapping->compositionFormat == session_.composition()->format() &&
        actual->captureRect == contentRect() && actual->devicePixelRatio == devicePixelRatioF())
        mapping = actual->mapping;
    if (!mapping || !contentRect().contains(event->position()) ||
        !mapping->displayRect.contains(event->position())) {
        showMessage(tr("Click inside the frame to set the focus point"));
        return true;
    }
    const auto toComposition = focusFrameToComposition(node->id);
    if (!toComposition || !toComposition->isInvertible()) {
        showMessage(tr("Show the composition or this Z Defocus to pick its focus"));
        return true;
    }
    const auto coordinate = mapping->toComposition(event->position());
    const auto local = toComposition->inverted().map(QPointF(coordinate.x, coordinate.y));
    const auto offset = local - focusFrameCentre();
    const auto point = nodeRoleParameter(session_, node->id, kFocusPointRole);
    const auto from = nodeRoleParameter(session_, node->id, kFocusFromRole);
    if (!point || !from) {
        showMessage(tr("This Z Defocus has no focus point"));
        return true;
    }
    std::vector<std::pair<document::ParameterId, document::ParameterValue>> values{
        {*point, document::Vec2d{std::round(offset.x() * 10.0) / 10.0,
                                 std::round(offset.y() * 10.0) / 10.0}}};
    if (liveInteger(session_, node->id, kFocusFromRole) != document::kFocusFromPoint)
        values.emplace_back(*from, document::kFocusFromPoint);
    if (!session_.setParameterValues(std::move(values), tr("Pick Focus")))
        showMessage(tr("The focus point could not be set"));
    // A Shift-click keeps picking (a rack focus: move the playhead, pick the next subject).
    if (!event->modifiers().testFlag(Qt::ShiftModifier))
        session_.cancelFocusPick();
    return true;
}

void ViewerEditor::paintFocusPoint(QPainter& painter,
                                   const std::optional<ViewerMapping>& painted) const {
    const auto* composition = session_.composition();
    const auto* node = zDefocusNode(session_, session_.focusPickNode());
    const bool picking = node != nullptr;
    if (node == nullptr)
        node = zDefocusNode(session_, session_.selectedNode() != nullptr
                                          ? std::optional(session_.selectedNode()->id)
                                          : std::nullopt);
    if (node == nullptr || composition == nullptr)
        return;
    const bool fromPoint = liveInteger(session_, node->id, kFocusFromRole) ==
                           document::kFocusFromPoint;
    const auto mode = liveInteger(session_, node->id, kModeRole).value_or(0);
    // The indicator marks the point the lens focuses on: shown while it is in use, and while a
    // pick is armed (where the next click lands is then the question).
    if (!picking && (!fromPoint || (mode != document::kDefocusModeDepth &&
                                    mode != document::kDefocusModePhysical)))
        return;
    const auto mapping = painted ? painted : currentMapping();
    const auto toComposition = focusFrameToComposition(node->id);
    if (!mapping || !toComposition)
        return;
    const auto offset = liveVec2(session_, node->id, kFocusPointRole).value_or(document::Vec2d{});
    const auto centre = focusFrameCentre();
    const auto at = [&](const double x, const double y) {
        const auto composed = toComposition->map(QPointF(centre.x() + x, centre.y() + y));
        return mapping->toScreen({composed.x(), composed.y()});
    };
    const auto point = at(offset.x, offset.y);
    // The sampled area: the 5 x 5 pixels whose mean depth the node reads, at least a few screen
    // pixels across so it reads at any zoom.
    const auto radius = static_cast<double>(document::kFocusSampleRadius) + 0.5;
    const auto corner = at(offset.x + radius, offset.y + radius);
    const double half = std::max({std::abs(corner.x() - point.x()),
                                  std::abs(corner.y() - point.y()), 3.0});
    const double reach = half + static_cast<double>(kit::px(kit::Spacing::M));
    painter.save();
    painter.setClipRect(contentRect());
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto accent = kit::color(kit::Color::Accent);
    for (const auto& [color, width] :
         {std::pair{kit::canvasDimColor(), 3.0}, std::pair{accent, 1.0}}) {
        QPen pen(color, width);
        pen.setCosmetic(true);
        if (picking && color == accent)
            pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(point.x() - half, point.y() - half, 2.0 * half, 2.0 * half));
        painter.drawEllipse(point, reach, reach);
        painter.drawLine(QPointF(point.x() - reach - 4.0, point.y()),
                         QPointF(point.x() - half - 2.0, point.y()));
        painter.drawLine(QPointF(point.x() + half + 2.0, point.y()),
                         QPointF(point.x() + reach + 4.0, point.y()));
        painter.drawLine(QPointF(point.x(), point.y() - reach - 4.0),
                         QPointF(point.x(), point.y() - half - 2.0));
        painter.drawLine(QPointF(point.x(), point.y() + half + 2.0),
                         QPointF(point.x(), point.y() + reach + 4.0));
    }
    painter.restore();
}

} // namespace bloom::ui
