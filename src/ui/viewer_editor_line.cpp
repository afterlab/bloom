// #599 Line Shape Source ends as Viewer point handles. A selected Line draws a handle at each end;
// dragging one edits `lineStart` or `lineEnd` at the playhead through the session's value edit, so
// a keyed end gets a key at the playhead and a constant end is set, as one undoable command. The
// ends are source coordinates, so the layer's transform carries them to the composition. A Line
// pivots on its content centre, the midpoint of its ends, so the anchor takes up half an end's move
// to keep the other end still; for a keyed end that compensation is keyed too (a key at the
// playhead, the anchor pinned at the end's other key times), so a drag changes only that frame.
#include <bloom/ui/viewer_editor.hpp>

#include <bloom/document/shape.hpp>
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
#include <variant>
#include <vector>

namespace bloom::ui {
namespace {

// The handle's reach on screen from its centre, in pixels.
constexpr double kLineHandleReach = 9.0;

struct LineTarget final {
    document::NodeId node;
    std::array<document::ParameterId, 2> ends;
    // The Layer Output's anchor, when the layer pivots on its content: the pivot is the content's
    // centre plus the anchor, and a Line's centre is the midpoint of its ends, so an end that moves
    // carries the pivot with it unless the anchor takes up half the move (the way a path anchor's
    // edit does). Invalid for a layer that pivots on the frame.
    document::ParameterId anchor;
};

// The Line Shape Source of the selection: the selected node, or the selected layer's source.
[[nodiscard]] std::optional<LineTarget> lineTarget(const CompositionSession& session) {
    const auto* composition = session.composition();
    if (composition == nullptr)
        return std::nullopt;
    const document::NodeRecord* node = session.selectedNode();
    if (node == nullptr || node->typeId != document::kShapeSourceNodeType) {
        const auto* layer = std::get_if<document::LayerId>(&session.selection().primary);
        const auto source = layer != nullptr ? session.directSourceNodeForLayer(*layer)
                                             : std::optional<document::NodeId>{};
        node = source ? composition->graph().findNode(*source) : nullptr;
    }
    if (node == nullptr || node->typeId != document::kShapeSourceNodeType)
        return std::nullopt;
    const auto kind = nodeRoleParameter(session, node->id, "kind");
    const auto kindValue = kind ? session.liveValue(*kind) : std::nullopt;
    const auto* stored = kindValue ? std::get_if<std::int64_t>(&*kindValue) : nullptr;
    const auto start = nodeRoleParameter(session, node->id, "lineStart");
    const auto end = nodeRoleParameter(session, node->id, "lineEnd");
    if (stored == nullptr || *stored != static_cast<std::int64_t>(document::ShapeKind::Line) ||
        !start || !end)
        return std::nullopt;
    LineTarget target{node->id, {*start, *end}, {}};
    if (const auto layer = session.layerForNode(node->id)) {
        const auto* boundary = composition->graph().findLayer(*layer);
        if (boundary != nullptr && boundary->pivot != document::LayerPivot::Frame) {
            const auto owner = session.boundaryNodeForLayer(*layer);
            const auto anchor = owner ? nodeRoleParameter(session, *owner, document::kAnchorParameterRole)
                                      : std::nullopt;
            if (!anchor)
                return std::nullopt;
            target.anchor = *anchor;
        }
    }
    return target;
}

[[nodiscard]] std::optional<document::Vec2d> liveEnd(const CompositionSession& session,
                                                     const document::ParameterId id) {
    const auto value = session.liveValue(id);
    const auto* stored = value ? std::get_if<document::Vec2d>(&*value) : nullptr;
    return stored ? std::optional(*stored) : std::nullopt;
}

// Whether a drag may write the end: keyed or constant, not driven, not locked.
[[nodiscard]] bool endEditable(const CompositionSession& session, const LineTarget& target,
                               const document::ParameterId id) {
    const auto* composition = session.composition();
    const auto* parameter = composition != nullptr ? composition->parameters().find(id) : nullptr;
    return parameter != nullptr && !composition->nodeLocked(target.node) &&
           !composition->parameterLocked(id) &&
           !std::holds_alternative<document::DriverBindingSource>(parameter->source);
}

} // namespace

bool ViewerEditor::lineHandlePress(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || tool() != ViewerTool::Select)
        return false;
    const auto target = lineTarget(session_);
    const auto mapping = motionHandleMapping();
    if (!target || !mapping)
        return false;
    const auto toComposition = focusFrameToComposition(target->node);
    if (!toComposition || !toComposition->isInvertible())
        return false;
    const auto pointer = event->position();
    // The nearer end wins when both are within reach (a very short line).
    std::optional<int> hit;
    double nearest = kLineHandleReach;
    for (int end = 0; end < 2; ++end) {
        const auto value = liveEnd(session_, target->ends[static_cast<std::size_t>(end)]);
        if (!value)
            continue;
        const auto composed = toComposition->map(QPointF(value->x, value->y));
        const double distance =
            QLineF(mapping->toScreen({composed.x(), composed.y()}), pointer).length();
        if (distance <= nearest) {
            nearest = distance;
            hit = end;
        }
    }
    if (!hit)
        return false;
    const auto id = target->ends[static_cast<std::size_t>(*hit)];
    if (!endEditable(session_, *target, id))
        return false;
    std::vector<document::ParameterId> edited{id};
    std::optional<document::Vec2d> anchorBase;
    if (target->anchor.isValid()) {
        anchorBase = liveEnd(session_, target->anchor);
        if (!anchorBase || !endEditable(session_, *target, target->anchor))
            return false;
        edited.push_back(target->anchor);
    }
    const auto endBase = liveEnd(session_, id);
    if (!endBase || !session_.beginValueEdits(edited, tr("Move Line End")))
        return false;
    // A keyed end moves the pivot only at the playhead: the anchor takes up the move there and is
    // pinned to its present value at every other key time of the end, so the other frames of the
    // line, and the other end at this frame, stay where they were.
    if (anchorBase) {
        const auto* parameter = session_.composition()->parameters().find(id);
        const auto* source = parameter != nullptr
                                 ? std::get_if<document::AnimationCurveSource>(&parameter->source)
                                 : nullptr;
        const auto* curve = source != nullptr
                                ? session_.composition()->animationCurves().findVec2(source->curveId)
                                : nullptr;
        if (curve != nullptr) {
            std::vector<core::RationalTime> times;
            for (const auto& component : curve->components)
                for (const auto& key : component.keyframes)
                    if (std::ranges::find(times, key.time) == times.end())
                        times.push_back(key.time);
            session_.holdKeysDuringValueEdits(target->anchor, std::move(times));
        }
    }
    lineDrag_ = LineHandleDrag{target->node, id, *toComposition, *endBase, anchorBase};
    event->accept();
    setFocus(Qt::MouseFocusReason);
    update();
    return true;
}

bool ViewerEditor::lineHandleMove(QMouseEvent* event) {
    if (!lineDrag_)
        return false;
    event->accept();
    if (!session_.valueEditActive()) {
        lineDrag_.reset();
        return true;
    }
    const auto mapping = motionHandleMapping();
    if (!mapping || !lineDrag_->toComposition.isInvertible())
        return true;
    const auto composed = mapping->toComposition(event->position());
    const auto local = lineDrag_->toComposition.inverted().map(QPointF(composed.x, composed.y));
    // Tenth-pixel steps, as the other Viewer point handles: a dragged end is a clean number.
    document::Vec2d next{std::round(local.x() * 10.0) / 10.0, std::round(local.y() * 10.0) / 10.0};
    // Shift keeps the line on the 45 degree steps about its other end.
    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        const auto target = lineTarget(session_);
        if (target) {
            const auto other = target->ends[0] == lineDrag_->parameter ? target->ends[1]
                                                                       : target->ends[0];
            if (const auto fixed = liveEnd(session_, other)) {
                const double dx = next.x - fixed->x, dy = next.y - fixed->y;
                const double length = std::hypot(dx, dy);
                constexpr double kStep = 3.14159265358979323846 / 4.0;
                const double angle = std::round(std::atan2(dy, dx) / kStep) * kStep;
                next = {std::round((fixed->x + length * std::cos(angle)) * 10.0) / 10.0,
                        std::round((fixed->y + length * std::sin(angle)) * 10.0) / 10.0};
            }
        }
    }
    std::vector<document::ParameterValue> values{next};
    // The centre moves by half the end's move; the anchor takes it up so the other end stays put.
    if (lineDrag_->anchorBase)
        values.emplace_back(document::Vec2d{
            lineDrag_->anchorBase->x - (next.x - lineDrag_->endBase.x) / 2.0,
            lineDrag_->anchorBase->y - (next.y - lineDrag_->endBase.y) / 2.0});
    if (!session_.updateValueEdits(values))
        finishLineHandle();
    update();
    return true;
}

bool ViewerEditor::lineHandleRelease(QMouseEvent* event) {
    if (!lineDrag_ || event->button() != Qt::LeftButton)
        return false;
    finishLineHandle();
    event->accept();
    return true;
}

void ViewerEditor::finishLineHandle() {
    lineDrag_.reset();
    if (session_.valueEditActive())
        static_cast<void>(session_.commitValueEdit());
    update();
}

void ViewerEditor::paintLineHandles(QPainter& painter,
                                    const std::optional<ViewerMapping>& painted) const {
    const auto target = lineTarget(session_);
    if (!target || isReferenceOnly() || tool() != ViewerTool::Select)
        return;
    const auto mapping = painted ? painted : motionHandleMapping();
    const auto toComposition = focusFrameToComposition(target->node);
    if (!mapping || !toComposition)
        return;
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
        for (std::size_t end = 0; end < 2; ++end) {
            const auto value = liveEnd(session_, target->ends[end]);
            if (!value)
                continue;
            const auto composed = toComposition->map(QPointF(value->x, value->y));
            const auto at = mapping->toScreen({composed.x(), composed.y()});
            // The start is a ring, the end a filled dot, so the two read apart.
            painter.setBrush(end == 1 ? QBrush(color) : QBrush(Qt::NoBrush));
            painter.drawEllipse(at, 5.0, 5.0);
        }
    }
    painter.restore();
}

} // namespace bloom::ui
