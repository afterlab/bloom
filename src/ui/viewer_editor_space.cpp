#include <bloom/ui/viewer_editor.hpp>

#include <bloom/core/coordinate_space.hpp>
#include <bloom/document/parameter_activity.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// #591 Coordinate Space on the Viewer: the selected space's defining points as handles over the
// frame -- its origin (Origin and scale), its rectangle's centre (Range to rectangle) or its four
// corners (Range to corners) -- with the space's axes or outline drawn through them. Each drag is one
// edit of one parameter: one command on release, one undo, and Escape confirms it as it stands. A
// space defined in another space (a connected parent) has its points in that space's units, so it
// shows no handles; nor does a point that is driven or keyed by something the drag cannot edit.
namespace bloom::ui {
namespace {

constexpr double kHandleReach = 9.0;

constexpr std::array<std::string_view, 6> kSpacePointRoles{
    "origin", "center", "topLeft", "topRight", "bottomRight", "bottomLeft"};

[[nodiscard]] bool hasParent(const CompositionSession& session, const document::NodeId node) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return false;
    for (const auto& edge : composition->graph().edges())
        if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            input != nullptr && input->nodeId == node && input->port == "parent")
            return true;
    return false;
}

[[nodiscard]] const document::NodeRecord* spaceHandleNode(const CompositionSession& session) {
    const auto* node = session.selectedNode();
    return node != nullptr && node->typeId == document::kCoordinateSpaceNodeType &&
                   !hasParent(session, node->id)
               ? node
               : nullptr;
}

[[nodiscard]] std::string_view schemaKeyOf(const std::string_view role) {
    if (role == "origin")
        return document::kSpaceOriginSchemaKey;
    if (role == "center")
        return document::kSpaceCenterSchemaKey;
    return document::kSpaceCornerSchemaKey;
}

[[nodiscard]] bool roleActive(const CompositionSession& session, const document::NodeRecord& node,
                              const std::string_view role) {
    const auto* composition = session.composition();
    return composition != nullptr &&
           !document::isParameterInactive(*composition, node, schemaKeyOf(role));
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
                              const std::string_view role, const double fallback) {
    const auto id = nodeRoleParameter(session, node, role);
    return id ? liveScalar(session, *id).value_or(fallback) : fallback;
}

[[nodiscard]] bool liveFlag(const CompositionSession& session, const document::NodeId node,
                            const std::string_view role, const bool fallback) {
    const auto id = nodeRoleParameter(session, node, role);
    const auto value = id ? session.liveValue(*id) : std::nullopt;
    const auto* stored = value ? std::get_if<bool>(&*value) : nullptr;
    return stored ? *stored : fallback;
}

[[nodiscard]] std::int64_t liveMode(const CompositionSession& session, const document::NodeId node) {
    const auto id = nodeRoleParameter(session, node, document::kModeParameterRole);
    const auto value = id ? session.liveValue(*id) : std::nullopt;
    const auto* stored = value ? std::get_if<std::int64_t>(&*value) : nullptr;
    return stored ? *stored : 0;
}

[[nodiscard]] bool editable(const CompositionSession& session, const document::NodeId node,
                            const std::string_view role) {
    const auto* composition = session.composition();
    const auto id = nodeRoleParameter(session, node, role);
    return composition != nullptr && !composition->nodeLocked(node) && id &&
           scalarEditable(session, *id);
}

[[nodiscard]] QString labelFor(const std::string_view role) {
    if (role == "origin")
        return QObject::tr("Move Space Origin");
    if (role == "center")
        return QObject::tr("Move Space Rectangle");
    return QObject::tr("Move Space Corner");
}

// The space's map from its live values, as the node computes it (core::space), for drawing.
[[nodiscard]] std::optional<core::space::Matrix4> liveMap(const CompositionSession& session,
                                                          const document::NodeRecord& node) {
    namespace space = core::space;
    const auto point = [&](const std::string_view role, const document::Vec2d fallback) {
        const auto value = liveVec2(session, node.id, role).value_or(fallback);
        return space::Point2{value.x, value.y};
    };
    const auto minimum = point("rangeMin", {-5.0, -5.0});
    const auto maximum = point("rangeMax", {5.0, 5.0});
    const double rotation = liveReal(session, node.id, "rotation", 0.0);
    const bool yUp = liveFlag(session, node.id, "yUp", true);
    std::optional<space::Matrix3> local;
    switch (liveMode(session, node.id)) {
    case 1: {
        if (!(maximum.x > minimum.x) || !(maximum.y > minimum.y))
            return std::nullopt;
        const auto size = point("size", {1000.0, 1000.0});
        local = space::placement(point("center", {}),
                                 {(minimum.x + maximum.x) * 0.5, (minimum.y + maximum.y) * 0.5},
                                 {size.x / (maximum.x - minimum.x), size.y / (maximum.y - minimum.y)},
                                 rotation, yUp);
        break;
    }
    case 2:
        local = space::rangeToQuad({minimum, maximum},
                                   {point("topLeft", {}), point("topRight", {1000.0, 0.0}),
                                    point("bottomRight", {1000.0, 1000.0}),
                                    point("bottomLeft", {0.0, 1000.0})});
        break;
    default:
        local = space::placement(point("origin", {}), {0.0, 0.0},
                                 point("pixelsPerUnit", {100.0, 100.0}), rotation, yUp);
        break;
    }
    if (!local || !space::inverse(*local))
        return std::nullopt;
    return space::embed2d(*local);
}

} // namespace

bool ViewerEditor::spaceHandlePress(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || tool() != ViewerTool::Select)
        return false;
    const auto* node = spaceHandleNode(session_);
    const auto mapping = motionHandleMapping();
    if (node == nullptr || !mapping)
        return false;
    const auto pointer = event->position();
    std::optional<std::string> role;
    for (const auto candidate : kSpacePointRoles) {
        if (!roleActive(session_, *node, candidate))
            continue;
        const auto at = liveVec2(session_, node->id, candidate);
        if (at && QLineF(mapping->toScreen(*at), pointer).length() <= kHandleReach) {
            role = std::string(candidate);
            break;
        }
    }
    if (!role || !editable(session_, node->id, *role))
        return false;
    const auto id = nodeRoleParameter(session_, node->id, *role);
    if (!id || !session_.beginValueEdits({*id}, labelFor(*role)))
        return false;
    spaceDrag_ = SpaceHandleDrag{node->id, *role};
    event->accept();
    setFocus(Qt::MouseFocusReason);
    return true;
}

bool ViewerEditor::spaceHandleMove(QMouseEvent* event) {
    if (!spaceDrag_)
        return false;
    event->accept();
    if (!session_.valueEditActive()) {
        spaceDrag_.reset();
        return true;
    }
    const auto mapping = motionHandleMapping();
    if (!mapping)
        return true;
    const auto at = mapping->toComposition(event->position());
    // A tenth of a pixel, as the other point handles round.
    const std::vector<document::ParameterValue> values{
        document::Vec2d{std::round(at.x * 10.0) / 10.0, std::round(at.y * 10.0) / 10.0}};
    if (!session_.updateValueEdits(values))
        finishSpaceHandle();
    update();
    return true;
}

bool ViewerEditor::spaceHandleRelease(QMouseEvent* event) {
    if (!spaceDrag_ || event->button() != Qt::LeftButton)
        return false;
    finishSpaceHandle();
    event->accept();
    return true;
}

void ViewerEditor::finishSpaceHandle() {
    spaceDrag_.reset();
    if (session_.valueEditActive())
        static_cast<void>(session_.commitValueEdit());
    update();
}

void ViewerEditor::paintSpaceHandles(QPainter& painter,
                                     const std::optional<ViewerMapping>& painted) const {
    const auto* node = spaceHandleNode(session_);
    if (node == nullptr || isReferenceOnly())
        return;
    const auto mapping = painted ? painted : motionHandleMapping();
    if (!mapping)
        return;
    const auto map = liveMap(session_, *node);
    const auto through = [&](const double x, const double y) -> std::optional<QPointF> {
        if (!map)
            return std::nullopt;
        const auto pixel = core::space::mapPoint(*map, {x, y});
        if (!pixel)
            return std::nullopt;
        return mapping->toScreen(document::Vec2d{pixel->x, pixel->y});
    };
    std::vector<QPointF> handles;
    for (const auto role : kSpacePointRoles)
        if (roleActive(session_, *node, role))
            if (const auto at = liveVec2(session_, node->id, role))
                handles.push_back(mapping->toScreen(*at));
    const auto mode = liveMode(session_, node->id);
    const auto minimum = liveVec2(session_, node->id, "rangeMin").value_or(document::Vec2d{-5, -5});
    const auto maximum = liveVec2(session_, node->id, "rangeMax").value_or(document::Vec2d{5, 5});
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
        // The range's outline through the map, for every mode.
        const auto corners = std::array{through(minimum.x, maximum.y), through(maximum.x, maximum.y),
                                        through(maximum.x, minimum.y), through(minimum.x, minimum.y)};
        if (corners[0] && corners[1] && corners[2] && corners[3] && minimum.x < maximum.x &&
            minimum.y < maximum.y) {
            QPen dashed = pen;
            dashed.setStyle(mode == 0 ? Qt::DashLine : Qt::SolidLine);
            painter.setPen(dashed);
            painter.drawPolygon(QPolygonF({*corners[0], *corners[1], *corners[2], *corners[3]}));
            painter.setPen(pen);
        }
        // One unit along each axis from the origin, the x axis longer-headed.
        if (const auto origin = through(0.0, 0.0)) {
            if (const auto x = through(1.0, 0.0))
                painter.drawLine(*origin, *x);
            if (const auto y = through(0.0, 1.0))
                painter.drawLine(*origin, *y);
        }
        for (const auto& point : handles) {
            const qreal half = 4.0;
            painter.drawRect(QRectF(point.x() - half, point.y() - half, 2.0 * half, 2.0 * half));
            painter.drawLine(point - QPointF(8.0, 0.0), point + QPointF(8.0, 0.0));
            painter.drawLine(point - QPointF(0.0, 8.0), point + QPointF(0.0, 8.0));
        }
    }
    painter.restore();
}

} // namespace bloom::ui
