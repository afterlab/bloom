// What a Nodes canvas gesture is over and what it may land on: hit tests, the link a pick-up
// lifts, the sockets a drag can reach, and the Merge slot caret. Split from the gesture handlers in
// node_editor_interactions.cpp so each stays readable. What a pointer move asks here reads the
// scene's spatial index or the canvas's id index, never a walk of every item (#359).
#include "node_editor_gesture_targets.hpp"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QGraphicsView>
#include <QLineF>

#include <algorithm>
#include <cmath>

namespace bloom::ui::node_gesture {
using namespace node_editor;
namespace {
// The pointer slop a socket gets, measured where the artist actually aims: on SCREEN. The socket's
// own hit shape is fixed in scene units (SocketItem::shape()), so at a zoomed-out canvas -- which
// is what Fit leaves the artist looking at -- kSocketHitSlop's 12 scene px shrink to three or four
// device px and a socket becomes something to aim at rather than something to grab. This converts
// the same slop back into scene units through the view's own scale, so the grab radius is constant
// in the artist's hand at every zoom.
qreal sceneGrabRadius(const QGraphicsScene& scene) {
    qreal scale = 1.0;
    for (const auto* view : scene.views())
        if (view != nullptr && view->transform().m11() > 0.0)
            scale = view->transform().m11();
    // Never TIGHTER than the painted hit shape: zooming in does not make a socket harder to hit.
    // Pointer coordinates are integer viewport pixels. Include half a pixel so a point
    // exactly on the radius remains inside after viewport rounding at fractional zoom.
    return std::max(kSocketHitSlop, (kSocketHitSlop + kit::px(kit::Size::Hairline) / 2.0) / scale);
}
} // namespace

SocketItem* socketAt(QGraphicsScene& scene, const QPointF point) {
    SocketItem* closest = nullptr;
    qreal distance = 1e30;
    for (auto* item : scene.items(point)) {
        auto* socket = dynamic_cast<SocketItem*>(item);
        if (!socket)
            continue;
        const qreal candidate = QLineF(point, socket->scenePos()).length();
        if (candidate < distance) {
            closest = socket;
            distance = candidate;
        }
    }
    if (closest != nullptr)
        return closest;
    // No socket's own shape held the point. Widen to the zoom-compensated radius, but only where a
    // hosted field is not already under the pointer -- a field keeps its own clicks, exactly as it
    // does when a socket shape does contain the point.
    if (fieldAt(scene, point)) {
        return nullptr;
    }
    const qreal radius = sceneGrabRadius(scene);
    const QRectF region(point.x() - radius, point.y() - radius, radius * 2.0, radius * 2.0);
    for (auto* item : scene.items(region, Qt::IntersectsItemBoundingRect)) {
        auto* socket = dynamic_cast<SocketItem*>(item);
        if (socket == nullptr || !socket->isVisible())
            continue;
        // Measured against the socket's own BODY, so a long multi-input pill is grabbable along its
        // whole length rather than only near its centre.
        const QPointF origin = socket->scenePos();
        const qreal half = socket->pillLength() / 2.0;
        const qreal dy = std::max(0.0, std::abs(point.y() - origin.y()) - half);
        const qreal reach = std::hypot(point.x() - origin.x(), dy);
        if (reach <= radius && reach < distance) {
            closest = socket;
            distance = reach;
        }
    }
    return closest;
}
NodeItem* cardAt(QGraphicsScene& scene, const QPointF point) {
    for (auto* item : scene.items(point))
        if (auto* card = nodeItemAncestor(item))
            return card;
    return nullptr;
}
// The innermost frame under `point`: with nested frames the smallest one wins, so a drop has
// exactly one answer instead of depending on scene item order.
NodeGroupItem* groupAt(QGraphicsScene& scene, const QPointF point) {
    NodeGroupItem* innermost = nullptr;
    for (auto* item : scene.items(point)) {
        auto* group = groupItemAncestor(item);
        if (group == nullptr || !group->isVisible())
            continue;
        const auto area = group->frameRect().width() * group->frameRect().height();
        if (innermost == nullptr ||
            area < innermost->frameRect().width() * innermost->frameRect().height())
            innermost = group;
    }
    return innermost;
}
// Which frame a dropped card's CENTER lands in, by the same innermost rule. Membership is judged on
// the center rather than on any overlap, so one card has one landing.
std::optional<document::NodeGroupId> groupForDrop(QGraphicsScene& scene, const QPointF center) {
    auto* group = groupAt(scene, center);
    return group == nullptr ? std::nullopt : std::optional(group->id());
}
bool fieldAt(QGraphicsScene& scene, const QPointF point) {
    for (auto* item : scene.items(point))
        for (auto* parent = item; parent; parent = parent->parentItem())
            if (auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(parent)) {
                auto* root = proxy->widget();
                if (!root)
                    continue;
                auto* child = root->childAt(proxy->mapFromScene(point).toPoint());
                // Property labels and gutters belong to the card. Only a live control owns
                // the click, so its row proxy does not consume the socket's screen-space slop.
                for (auto* target = child ? child : root; target; target = target->parentWidget()) {
                    if (target->focusPolicy() != Qt::NoFocus ||
                        qobject_cast<QAbstractButton*>(target) ||
                        dynamic_cast<kit::KDiamond*>(target))
                        return true;
                    if (target == root)
                        break;
                }
            }
    return false;
}
namespace {
const document::EdgeRecord* inputEdge(const document::Composition& composition,
                                      const document::InputPortRef& input) {
    const auto edges = composition.graph().edges();
    const auto found = std::ranges::find(edges, input, &document::EdgeRecord::destination);
    return found == edges.end() ? nullptr : &*found;
}
} // namespace
// Where an input's value comes from, whichever durable record carries it: an edge for image
// transport, the parameter's own driver binding for an operand (task FIX1, item A). The pick-up
// gesture asks this one question rather than only looking for an edge, which is why an operand's
// link can now be dragged off and dropped the way an image link always could.
std::optional<document::OutputPortRef> incomingSource(const document::Composition& composition,
                                                      const document::InputPortRef& input) {
    if (const auto* edge = inputEdge(composition, input))
        return edge->source;
    const auto* fixed = std::get_if<document::NodeInputRef>(&input);
    if (fixed == nullptr)
        return std::nullopt;
    const auto* node = composition.graph().findNode(fixed->nodeId);
    if (node == nullptr)
        return std::nullopt;
    const auto binding =
        std::ranges::find(node->parameters, fixed->port, &document::ParameterBinding::role);
    if (binding == node->parameters.end())
        return std::nullopt;
    const auto* parameter = composition.parameters().find(binding->parameterId);
    const auto* driver = parameter == nullptr
                             ? nullptr
                             : std::get_if<document::DriverBindingSource>(&parameter->source);
    return driver == nullptr
               ? std::nullopt
               : std::optional(document::OutputPortRef{driver->sourceNodeId, driver->outputPort});
}
SocketItem* findOutput(const NodeCanvasIndex& index, const document::OutputPortRef& output) {
    if (const auto* card = index.node(output.nodeId))
        for (auto* socket : card->sockets())
            if (socket->output == output)
                return socket;
    return nullptr;
}
std::pair<SocketItem*, SocketItem*> insertionSockets(NodeItem& card,
                                                     const NodeProjectionIndex& links) {
    SocketItem* input = nullptr;
    SocketItem* output = nullptr;
    for (auto* socket : card.sockets()) {
        if (socket->kind != document::SocketValueKind::Image || socket->multiInput())
            continue;
        if (socket->input && !input)
            input = socket;
        if (socket->output && !output)
            output = socket;
    }
    if (!input || !output || !input->draggable() || !output->draggable() || !input->input ||
        !output->output || links.edgeInto(*input->input))
        return {};
    return {input, output};
}
namespace {
// The one line a dimmed socket carries while a drag it cannot take is in flight (task FIX1,
// item A.3): which way round the link would have to go, or which two kinds do not meet.
QString refusalFor(const NodeInteraction& gesture, const SocketItem& socket,
                   const bool opposite) {
    if (!socket.draggable())
        return QCoreApplication::translate(
            "node_editor", "Cannot be unlinked here; remove the layer to remove this connection");
    if (!opposite)
        return gesture.output.has_value()
                   ? QCoreApplication::translate("node_editor",
                                                 "A link from an output must land on an input")
                   : QCoreApplication::translate("node_editor",
                                                 "A link from an input must land on an output");
    const auto from = gesture.output.has_value() ? gesture.linkKind : socket.kind;
    const auto to = gesture.output.has_value() ? socket.kind : gesture.linkKind;
    return QCoreApplication::translate("node_editor", "%1 does not connect to %2")
        .arg(socketKindName(from), socketKindName(to));
}

// Tells every socket on the canvas whether the link now being dragged could land on it (task S1,
// item 6). The socket the drag STARTED from keeps its resting ink: it is the thing in the artist's
// hand, not a candidate to aim at.
void markSocketAffinity(SocketItem& socket, const NodeInteraction& gesture,
                        const SocketItem* origin) {
    if (&socket == origin) {
        socket.setDragAffinity(SocketItem::DragAffinity::Idle);
        return;
    }
    const bool fromOutput = gesture.output.has_value();
    const bool opposite = fromOutput ? socket.input.has_value() : socket.output.has_value();
    // The shared promotion whitelist, not kind equality: a socket an Integer output can widen
    // into is a socket the drag can land on, so the affinity an artist SEES is the rule
    // ConnectPorts will actually apply.
    const bool compatible = socket.draggable() && opposite &&
                            (socket.acceptsAnyKind() || linkKindAccepted(gesture, socket));
    socket.setDragAffinity(compatible ? SocketItem::DragAffinity::Compatible
                                      : SocketItem::DragAffinity::Incompatible,
                           compatible ? QString{} : refusalFor(gesture, socket, opposite));
}
} // namespace

void markLinkAffinity(const NodeCanvasIndex& index, const NodeInteraction& gesture,
                      const SocketItem* origin) {
    for (const auto& [id, card] : index.nodes)
        for (auto* socket : card->sockets())
            markSocketAffinity(*socket, gesture, origin);
}

// Marks, on the Merge node's ordered multi-input, which position in the stack order the pointer is
// currently at (task S1, item 7). The pill is simultaneously dimmed as incompatible by
// markLinkAffinity() above -- a stack slot is structural and accepts no drop -- so the caret
// reports the pointer's position in the order and never promises a landing. Only a pill under
// the pointer can carry a caret, so a move asks the scene's index for those and clears the ones
// the previous move marked (#359), rather than visiting every socket on the canvas.
void updateOrderedDropIndicator(QGraphicsScene& scene, NodeInteraction& gesture,
                                const QPointF cursor) {
    std::vector<SocketItem*> marked;
    for (auto* item : scene.items(cursor)) {
        auto* socket = dynamic_cast<SocketItem*>(item);
        if (socket == nullptr || !socket->multiInput())
            continue;
        socket->setDropIndicator(socket->slotIndexAt(socket->mapFromScene(cursor)));
        marked.push_back(socket);
    }
    for (auto* socket : gesture.indicated)
        if (std::ranges::find(marked, socket) == marked.end())
            socket->setDropIndicator(std::nullopt);
    gesture.indicated = std::move(marked);
}

void previewInsertion(QGraphicsScene& scene, NodeInteraction& gesture,
                      const NodeProjectionIndex& links, const QPointF cursor) {
    if (gesture.insertEdge)
        gesture.insertEdge->emphasize(false);
    gesture.insertEdge = nullptr;
    if (gesture.positions.size() != 1)
        return;
    auto* card = cardAt(scene, cursor);
    if (!card || !gesture.positions.contains(card->id()))
        return;
    if (!insertionSockets(*card, links).first)
        return;
    for (auto* item : scene.items(cursor)) {
        auto* edge = dynamic_cast<NodeEdgeItem*>(item);
        if (!edge || edge->structural || edge->edge.source.nodeId == card->id() ||
            destinationNodeId(edge->edge.destination) == card->id())
            continue;
        gesture.insertEdge = edge;
        edge->emphasize(true);
        break;
    }
}
} // namespace bloom::ui::node_gesture
