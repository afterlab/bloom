#include <bloom/ui/viewer_editor.hpp>

#include <bloom/document/parameter_activity.hpp>
#include <bloom/document/warp_definition.hpp>
#include <bloom/document/warp_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QLineF>
#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// #510 Procedural Warp on the Viewer: the selected node's points and its radius as handles over the
// frame. A point (the centre, Magnify's pull point, Split's line) drags to a place; the radius is a
// ring about the centre that drags in or out. Each drag is one edit of one parameter: one command on
// release, one undo, and Escape confirms it as it stands.
namespace bloom::ui {
namespace {

// The handle's reach on screen, in pixels.
constexpr double kHandleReach = 9.0;
// How close to the ring's line a press grabs the radius.
constexpr double kRingReach = 6.0;

constexpr std::array<std::string_view, 4> kPointRoles{"center", "pullPoint", "lineStart", "lineEnd"};

[[nodiscard]] const document::NodeRecord* warpHandleNode(const CompositionSession& session) {
    const auto* node = session.selectedNode();
    return node != nullptr && node->typeId == document::kProceduralWarpNodeType ? node : nullptr;
}

[[nodiscard]] bool roleActive(const CompositionSession& session, const document::NodeRecord& node,
                              const std::string_view role) {
    const auto* composition = session.composition();
    return composition != nullptr &&
           !document::isParameterInactive(
               *composition, node, document::warpSchemaKey(document::kProceduralWarpNodeType, role));
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
                              const std::string_view role, const double fallback = 0.0) {
    const auto id = nodeRoleParameter(session, node, role);
    return id ? liveScalar(session, *id).value_or(fallback) : fallback;
}

[[nodiscard]] std::int64_t liveChoice(const CompositionSession& session, const document::NodeId node,
                                      const std::string_view role) {
    const auto id = nodeRoleParameter(session, node, role);
    const auto value = id ? session.liveValue(*id) : std::nullopt;
    const auto* stored = value ? std::get_if<std::int64_t>(&*value) : nullptr;
    return stored ? *stored : 0;
}

// The radius as the warp reads it: Magnify's grows with the magnification when it is linked to it.
[[nodiscard]] double effectiveRadius(const CompositionSession& session,
                                     const document::NodeRecord& node) {
    double radius = liveReal(session, node.id, "radius", 300.0);
    if (node.typeId == document::kProceduralWarpNodeType &&
        liveChoice(session, node.id, "mode") == document::kProceduralWarpMagnify &&
        liveChoice(session, node.id, "magnifyLink") >= document::kMagnifyLinkSize)
        radius *= liveReal(session, node.id, "magnification", 200.0) / 100.0;
    return radius;
}

// The centre's offset from the frame's centre (a Drizzle has none: its ring is about the middle).
[[nodiscard]] document::Vec2d ringCentre(const CompositionSession& session,
                                         const document::NodeRecord& node) {
    return roleActive(session, node, "center")
               ? liveVec2(session, node.id, "center").value_or(document::Vec2d{})
               : document::Vec2d{};
}

[[nodiscard]] bool editable(const CompositionSession& session, const document::NodeId node,
                            const std::string_view role) {
    const auto* composition = session.composition();
    const auto id = nodeRoleParameter(session, node, role);
    return composition != nullptr && !composition->nodeLocked(node) && id &&
           scalarEditable(session, *id);
}

[[nodiscard]] QString labelFor(const int kind, const std::string_view role) {
    if (kind == 1)
        return QObject::tr("Set Radius");
    if (role == "pullPoint")
        return QObject::tr("Move Pull Point");
    if (role == "lineStart")
        return QObject::tr("Move Line Start");
    if (role == "lineEnd")
        return QObject::tr("Move Line End");
    return QObject::tr("Move Center");
}

} // namespace

bool ViewerEditor::warpHandlePress(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || tool() != ViewerTool::Select)
        return false;
    const auto* node = warpHandleNode(session_);
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
    // The points first (the centre sits inside the ring), then the ring.
    std::optional<WarpHandleDrag> drag;
    for (const auto role : kPointRoles) {
        if (!roleActive(session_, *node, role))
            continue;
        const auto at = liveVec2(session_, node->id, role).value_or(document::Vec2d{});
        if (near(screen({at.x, at.y})))
            drag = WarpHandleDrag{node->id, 0, std::string(role)};
        if (drag)
            break;
    }
    if (!drag && roleActive(session_, *node, "radius")) {
        const auto centre = ringCentre(session_, *node);
        const auto radius = effectiveRadius(session_, *node);
        const auto at = screen({centre.x, centre.y});
        const auto rim = screen({centre.x + radius, centre.y});
        const double reach = QLineF(at, rim).length();
        const double away = QLineF(at, pointer).length();
        if (reach > 0.0 && std::abs(away - reach) <= kRingReach)
            drag = WarpHandleDrag{node->id, 1, "radius"};
    }
    if (!drag || !editable(session_, node->id, drag->role))
        return false;
    const auto id = nodeRoleParameter(session_, node->id, drag->role);
    if (!id || !session_.beginValueEdits({*id}, labelFor(drag->kind, drag->role)))
        return false;
    warpDrag_ = drag;
    event->accept();
    setFocus(Qt::MouseFocusReason);
    return true;
}

bool ViewerEditor::warpHandleMove(QMouseEvent* event) {
    if (!warpDrag_)
        return false;
    event->accept();
    if (!session_.valueEditActive()) {
        warpDrag_.reset();
        return true;
    }
    const auto* composition = session_.composition();
    const auto* node = composition != nullptr ? composition->graph().findNode(warpDrag_->node)
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
    if (warpDrag_->kind == 0) {
        values = {document::Vec2d{std::round(offset.x() * 10.0) / 10.0,
                                  std::round(offset.y() * 10.0) / 10.0}};
    } else {
        // The distance from the ring's centre; Magnify's linked ring is the radius times the
        // magnification, a square's is its larger axis.
        const auto centre = ringCentre(session_, *node);
        const double dx = offset.x() - centre.x;
        const double dy = offset.y() - centre.y;
        const bool square = liveChoice(session_, node->id, "mode") == document::kProceduralWarpMagnify &&
                            liveChoice(session_, node->id, "magnifyShape") == 1;
        double distance = square ? std::max(std::abs(dx), std::abs(dy)) : std::hypot(dx, dy);
        const double share = liveReal(session_, node->id, "radius", 300.0) /
                             std::max(effectiveRadius(session_, *node), 1.0e-9);
        distance = std::clamp(distance * share, 0.0, document::kMaximumWarpLength);
        values = {std::round(distance * 10.0) / 10.0};
    }
    if (!session_.updateValueEdits(values))
        finishWarpHandle();
    update();
    return true;
}

bool ViewerEditor::warpHandleRelease(QMouseEvent* event) {
    if (!warpDrag_ || event->button() != Qt::LeftButton)
        return false;
    finishWarpHandle();
    event->accept();
    return true;
}

void ViewerEditor::finishWarpHandle() {
    warpDrag_.reset();
    if (session_.valueEditActive())
        static_cast<void>(session_.commitValueEdit());
    update();
}

void ViewerEditor::paintWarpHandles(QPainter& painter,
                                    const std::optional<ViewerMapping>& painted) const {
    const auto* node = warpHandleNode(session_);
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
    struct Point final {
        std::string role;
        QPointF at;
    };
    std::vector<Point> points;
    for (const auto role : kPointRoles)
        if (roleActive(session_, *node, role)) {
            const auto at = liveVec2(session_, node->id, role).value_or(document::Vec2d{});
            points.push_back({std::string(role), screen({at.x, at.y})});
        }
    const bool ringShown = roleActive(session_, *node, "radius");
    if (points.empty() && !ringShown)
        return;
    const auto centre = ringCentre(session_, *node);
    const auto radius = effectiveRadius(session_, *node);
    const bool square = liveChoice(session_, node->id, "mode") == document::kProceduralWarpMagnify &&
                        liveChoice(session_, node->id, "magnifyShape") == 1;
    const bool fades = roleActive(session_, *node, "falloff");
    const double core = fades ? radius * (1.0 - liveReal(session_, node->id, "falloff", 100.0) / 100.0)
                              : 0.0;
    painter.save();
    painter.setClipRect(contentRect());
    painter.setRenderHint(QPainter::Antialiasing, true);
    const auto accent = kit::color(kit::Color::Accent);
    const auto ring = [&](const double length, const Qt::PenStyle style, const QPen& base) {
        if (!(length > 0.0))
            return;
        QPen pen = base;
        pen.setStyle(style);
        painter.setPen(pen);
        const auto at = screen({centre.x, centre.y});
        const auto rim = screen({centre.x + length, centre.y});
        const double reach = QLineF(at, rim).length();
        if (square)
            painter.drawRect(QRectF(at.x() - reach, at.y() - reach, 2.0 * reach, 2.0 * reach));
        else
            painter.drawEllipse(at, reach, reach);
    };
    for (const auto& [color, width] :
         {std::pair{kit::canvasDimColor(), 3.0}, std::pair{accent, 1.0}}) {
        QPen pen(color, width);
        pen.setCosmetic(true);
        painter.setBrush(Qt::NoBrush);
        if (ringShown) {
            // The radius, and the start of the fade inside it.
            ring(radius, Qt::SolidLine, pen);
            if (fades && core > 0.0 && core < radius)
                ring(core, Qt::DashLine, pen);
            const auto rim = screen({centre.x + radius, centre.y});
            painter.setPen(pen);
            painter.setBrush(color);
            painter.drawEllipse(rim, 4.0, 4.0);
            painter.setBrush(Qt::NoBrush);
        }
        painter.setPen(pen);
        // Magnify's pull and Split's line join their two points.
        if (points.size() == 2)
            painter.drawLine(points[0].at, points[1].at);
        for (const auto& point : points) {
            const qreal half = 4.0;
            painter.drawRect(QRectF(point.at.x() - half, point.at.y() - half, 2.0 * half, 2.0 * half));
            painter.drawLine(point.at - QPointF(8.0, 0.0), point.at + QPointF(8.0, 0.0));
            painter.drawLine(point.at - QPointF(0.0, 8.0), point.at + QPointF(0.0, 8.0));
        }
    }
    painter.restore();
}

} // namespace bloom::ui
