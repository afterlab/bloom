#include <bloom/ui/viewer_editor.hpp>

#include <bloom/document/motion_blur_parameters.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <variant>

namespace bloom::ui {
namespace {

constexpr double kDegrees = 180.0 / std::numbers::pi;
// The handle's reach on screen from its centre, in pixels.
constexpr double kHandleReach = 9.0;
// Spin's dial and Zoom's unit ring in full-resolution pixels, as a share of the shorter frame side.
constexpr double kRingShare = 0.2;

[[nodiscard]] const document::NodeRecord* motionHandleNode(const CompositionSession& session) {
    const auto* node = session.selectedNode();
    if (node == nullptr)
        return nullptr;
    return node->typeId == document::kDirectionalBlurNodeType ||
                   node->typeId == document::kSpinBlurNodeType ||
                   node->typeId == document::kZoomBlurNodeType
               ? node
               : nullptr;
}

[[nodiscard]] std::optional<document::Vec2d> liveVec2(const CompositionSession& session,
                                                      const document::NodeId node,
                                                      const std::string_view role) {
    const auto id = nodeRoleParameter(session, node, role);
    const auto value = id ? session.liveValue(*id) : std::nullopt;
    const auto* stored = value ? std::get_if<document::Vec2d>(&*value) : nullptr;
    return stored ? std::optional(*stored) : std::nullopt;
}

[[nodiscard]] double liveReal(const CompositionSession& session, const document::NodeId node,
                              const std::string_view role) {
    const auto id = nodeRoleParameter(session, node, role);
    return id ? liveScalar(session, *id).value_or(0.0) : 0.0;
}

[[nodiscard]] double frameRing(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return 216.0;
    const auto format = composition->format();
    return kRingShare * static_cast<double>(std::min(format.width(), format.height()));
}

// A frame-space point as the tip of `node`'s handle: Directional's end of the blur from the frame's
// centre, Spin's turn on its dial (zero up, clockwise), Zoom's scale along the diagonal ray.
[[nodiscard]] QPointF tipOffset(const CompositionSession& session, const document::NodeRecord& node) {
    const auto centre = liveVec2(session, node.id, "centre").value_or(document::Vec2d{});
    const double ring = frameRing(session);
    if (node.typeId == document::kDirectionalBlurNodeType) {
        const double angle = liveReal(session, node.id, "angle") / kDegrees;
        const double length = liveReal(session, node.id, "length");
        return {length * std::cos(angle), length * std::sin(angle)};
    }
    const double amount = liveReal(session, node.id, "amount");
    if (node.typeId == document::kSpinBlurNodeType) {
        const double turn = amount / kDegrees;
        return {centre.x + ring * std::sin(turn), centre.y - ring * std::cos(turn)};
    }
    const double reach = ring * (1.0 + amount) * std::numbers::sqrt2 * 0.5;
    return {centre.x + reach, centre.y - reach};
}

[[nodiscard]] bool editable(const CompositionSession& session, const document::NodeId node,
                            const std::string_view role) {
    const auto* composition = session.composition();
    const auto id = nodeRoleParameter(session, node, role);
    return composition != nullptr && !composition->nodeLocked(node) && id &&
           scalarEditable(session, *id);
}

} // namespace

std::optional<ViewerMapping> ViewerEditor::motionHandleMapping() const {
    // A blur's frame can take a while: its handles keep working from the frame still on screen, as
    // the focus pick does. The mapping is geometry; it does not depend on the frame's pixels.
    auto mapping = currentMapping();
    if (const auto actual = actualDisplayDescriptor();
        !mapping && actual && actual->mapping && session_.composition() != nullptr &&
        actual->mapping->compositionFormat == session_.composition()->format() &&
        actual->captureRect == contentRect() && actual->devicePixelRatio == devicePixelRatioF())
        mapping = actual->mapping;
    return mapping;
}

bool ViewerEditor::motionHandlePress(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || tool() != ViewerTool::Select)
        return false;
    const auto* node = motionHandleNode(session_);
    const auto mapping = motionHandleMapping();
    if (node == nullptr || !mapping)
        return false;
    const auto toComposition = focusFrameToComposition(node->id);
    if (!toComposition)
        return false;
    const auto frame = focusFrameCentre();
    const auto screen = [&](const QPointF offset) {
        const auto composed = toComposition->map(frame + offset);
        return mapping->toScreen({composed.x(), composed.y()});
    };
    const auto pointer = event->position();
    const auto near = [&](const QPointF point) {
        return QLineF(point, pointer).length() <= kHandleReach;
    };
    const bool directional = node->typeId == document::kDirectionalBlurNodeType;
    const auto tip = tipOffset(session_, *node);
    std::optional<MotionHandleDrag> drag;
    if (near(screen(tip)))
        drag = MotionHandleDrag{node->id, 1, 0.0, liveReal(session_, node->id, "amount")};
    else if (!directional) {
        const auto centre = liveVec2(session_, node->id, "centre").value_or(document::Vec2d{});
        if (near(screen({centre.x, centre.y})))
            drag = MotionHandleDrag{node->id, 0, 0.0, 0.0};
    }
    if (!drag)
        return false;
    std::vector<document::ParameterId> parameters;
    QString label;
    const auto add = [&](const std::string_view role) {
        const auto id = nodeRoleParameter(session_, node->id, role);
        if (id && editable(session_, node->id, role))
            parameters.push_back(*id);
    };
    std::size_t expected = 1;
    if (directional) {
        add("angle");
        add("length");
        expected = 2;
        label = tr("Set Blur Direction");
    } else if (drag->handle == 0) {
        add("centre");
        label = tr("Move Centre");
    } else {
        add("amount");
        label = node->typeId == document::kSpinBlurNodeType ? tr("Set Spin") : tr("Set Zoom");
    }
    if (parameters.size() != expected || !session_.beginValueEdits(parameters, label))
        return false;
    if (node->typeId == document::kSpinBlurNodeType && drag->handle == 1) {
        const auto centre = liveVec2(session_, node->id, "centre").value_or(document::Vec2d{});
        const auto at = screen({centre.x, centre.y});
        drag->bearing = std::atan2(pointer.x() - at.x(), at.y() - pointer.y());
    }
    motionDrag_ = drag;
    event->accept();
    setFocus(Qt::MouseFocusReason);
    return true;
}

bool ViewerEditor::motionHandleMove(QMouseEvent* event) {
    if (!motionDrag_)
        return false;
    event->accept();
    if (!session_.valueEditActive()) {
        motionDrag_.reset();
        return true;
    }
    const auto* composition = session_.composition();
    const auto* node = composition != nullptr ? composition->graph().findNode(motionDrag_->node)
                                              : nullptr;
    const auto mapping = motionHandleMapping();
    const auto toComposition =
        node != nullptr ? focusFrameToComposition(node->id) : std::optional<QTransform>{};
    if (node == nullptr || !mapping || !toComposition || !toComposition->isInvertible())
        return true;
    const auto coordinate = mapping->toComposition(event->position());
    const auto local = toComposition->inverted().map(QPointF(coordinate.x, coordinate.y));
    const auto offset = local - focusFrameCentre();
    std::vector<document::ParameterValue> values;
    if (node->typeId == document::kDirectionalBlurNodeType) {
        const double length = std::hypot(offset.x(), offset.y());
        const double angle = length > 0.0 ? std::atan2(offset.y(), offset.x()) * kDegrees
                                          : liveReal(session_, node->id, "angle");
        values = {std::round(angle * 10.0) / 10.0,
                  std::min(std::round(length * 10.0) / 10.0, document::kMaximumMotionBlurPixels)};
    } else if (motionDrag_->handle == 0) {
        values = {document::Vec2d{std::round(offset.x() * 10.0) / 10.0,
                                  std::round(offset.y() * 10.0) / 10.0}};
    } else {
        const auto centre = liveVec2(session_, node->id, "centre").value_or(document::Vec2d{});
        const double dx = offset.x() - centre.x;
        const double dy = offset.y() - centre.y;
        if (node->typeId == document::kSpinBlurNodeType) {
            // The dial's bearing, unwrapped: a drag may turn past a whole revolution.
            const double bearing = std::atan2(dx, -dy);
            double step = bearing - motionDrag_->bearing;
            if (step > std::numbers::pi)
                step -= 2.0 * std::numbers::pi;
            else if (step < -std::numbers::pi)
                step += 2.0 * std::numbers::pi;
            motionDrag_->bearing = bearing;
            motionDrag_->amount =
                std::clamp(motionDrag_->amount + step * kDegrees, -3600.0, 3600.0);
            values = {std::round(motionDrag_->amount * 10.0) / 10.0};
        } else {
            // Along the handle's diagonal ray: the unit ring is no zoom.
            const double along = (dx - dy) * std::numbers::sqrt2 * 0.5;
            const double amount = std::clamp(along / frameRing(session_) - 1.0, -0.95, 16.0);
            values = {std::round(amount * 100.0) / 100.0};
        }
    }
    if (!session_.updateValueEdits(values))
        finishMotionHandle();
    update();
    return true;
}

bool ViewerEditor::motionHandleRelease(QMouseEvent* event) {
    if (!motionDrag_ || event->button() != Qt::LeftButton)
        return false;
    finishMotionHandle();
    event->accept();
    return true;
}

void ViewerEditor::finishMotionHandle() {
    motionDrag_.reset();
    if (session_.valueEditActive())
        static_cast<void>(session_.commitValueEdit());
    update();
}

void ViewerEditor::paintMotionHandles(QPainter& painter,
                                      const std::optional<ViewerMapping>& painted) const {
    const auto* node = motionHandleNode(session_);
    if (node == nullptr || isReferenceOnly())
        return;
    const auto mapping = painted ? painted : motionHandleMapping();
    const auto toComposition = focusFrameToComposition(node->id);
    if (!mapping || !toComposition)
        return;
    const auto frame = focusFrameCentre();
    const auto screen = [&](const QPointF offset) {
        const auto composed = toComposition->map(frame + offset);
        return mapping->toScreen({composed.x(), composed.y()});
    };
    const bool directional = node->typeId == document::kDirectionalBlurNodeType;
    const bool spin = node->typeId == document::kSpinBlurNodeType;
    const auto centreValue = directional
                                 ? document::Vec2d{}
                                 : liveVec2(session_, node->id, "centre").value_or(document::Vec2d{});
    const QPointF centreOffset(centreValue.x, centreValue.y);
    const auto centre = screen(centreOffset);
    const auto tip = screen(tipOffset(session_, *node));
    const double ring = frameRing(session_);
    painter.save();
    painter.setClipRect(contentRect());
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto accent = kit::color(kit::Color::Accent);
    for (const auto& [color, width] :
         {std::pair{kit::canvasDimColor(), 3.0}, std::pair{accent, 1.0}}) {
        QPen pen(color, width);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        if (directional) {
            // The blur's path from the frame's centre, its direction an arrowhead at the tip.
            painter.drawLine(centre, tip);
            const QLineF shaft(tip, centre);
            for (const double turn : {28.0, -28.0}) {
                QLineF barb(tip, tip + QPointF(kHandleReach * 1.4, 0.0));
                barb.setAngle(shaft.angle() + turn);
                painter.drawLine(barb);
            }
        } else if (spin) {
            // The dial, and the arc the spin turns through.
            const auto rim = screen(centreOffset + QPointF(0.0, -ring));
            const double radius = QLineF(centre, rim).length();
            painter.drawEllipse(centre, radius, radius);
            const double amount = liveReal(session_, node->id, "amount");
            QPen arc = pen;
            arc.setWidthF(width + 1.5);
            painter.setPen(arc);
            const QRectF bounds(centre.x() - radius, centre.y() - radius, 2.0 * radius,
                                2.0 * radius);
            painter.drawArc(bounds, 90 * 16,
                            static_cast<int>(std::lround(-std::clamp(amount, -360.0, 360.0) * 16.0)));
            painter.setPen(pen);
        } else {
            // The unit ring (no zoom) and the ray the zoom reads along.
            const auto rim = screen(centreOffset + QPointF(ring, 0.0));
            const double radius = QLineF(centre, rim).length();
            painter.drawEllipse(centre, radius, radius);
            painter.drawLine(centre, tip);
        }
        // The handles.
        painter.setBrush(color);
        painter.drawEllipse(tip, 4.0, 4.0);
        if (!directional) {
            painter.setBrush(Qt::NoBrush);
            const qreal half = 4.0;
            painter.drawRect(QRectF(centre.x() - half, centre.y() - half, 2.0 * half, 2.0 * half));
            painter.drawLine(centre - QPointF(8.0, 0.0), centre + QPointF(8.0, 0.0));
            painter.drawLine(centre - QPointF(0.0, 8.0), centre + QPointF(0.0, 8.0));
        }
    }
    painter.restore();
}

} // namespace bloom::ui
