#pragma once
#include "node_editor_items.hpp"
#include <QGraphicsRectItem>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <functional>
#include <string>
#include <vector>

namespace bloom::ui {
// This is transient canvas state. Frozen revision and stable IDs prevent a gesture from editing
// replacement project content. No draft, mutable document or command stack is retained by UI.
struct NodeInteraction final {
    enum class Mode { Idle, Move, Resize, Box, Link, Cut, Reroute, Place };
    Mode mode = Mode::Idle;
    document::Revision revision;
    QPointF origin;
    std::map<document::NodeId, QPointF> positions;
    std::set<document::NodeId> baseSelection;
    document::NodeId resized;
    qreal width = 0;
    bool floating = false;
    std::optional<document::InputPortRef> input;
    std::optional<document::OutputPortRef> output;
    std::optional<document::InputPortRef> pickedInput;
    // Set when the gesture picked up a GROUP frame rather than a card. Such a move carries every
    // member at once, so the frame travels with them and no membership can change -- which is
    // exactly why the drop test is skipped for it.
    std::optional<document::NodeGroupId> movedGroup;
    // The transport kind the link being dragged carries (task S1, item 6): what every other
    // socket's compatibility is judged against, and what inks the preview wire.
    document::SocketValueKind linkKind = document::SocketValueKind::Image;
    // #490: the drag started on an inspection input that also takes a Mask or a ChannelSet.
    bool linkAcceptsPixelKinds = false;
    // #525: the drag started on an any-field input that also takes the other field kinds.
    bool linkAcceptsFieldKinds = false;
    bool linkAcceptsImageMatte = false;
    QGraphicsPathItem* line = nullptr;
    QGraphicsRectItem* box = nullptr;
    node_editor::NodeEdgeItem* insertEdge = nullptr;
    // #703: the node type Add is placing and its translucent card.
    std::string placeType;
    QGraphicsItem* placement = nullptr;
    // The Merge pills a link drag is showing its slot caret on: the only ones a move has to clear.
    std::vector<node_editor::SocketItem*> indicated;
};

// Whether the link `gesture` drags may land on `target` by KIND: the shared
// isAcceptedSocketConnection() rule, plus the pixel kinds an inspection input takes (#490) and the
// field kinds an any-field input takes (#525). The orientation and reroute questions stay with the
// callers.
[[nodiscard]] inline bool linkKindAccepted(const NodeInteraction& gesture,
                                           const node_editor::SocketItem& target) {
    if (gesture.output.has_value())
        return document::isAcceptedSocketConnection(gesture.linkKind, target.kind) ||
               (target.acceptsPixelKinds() && document::isPixelSocketKind(gesture.linkKind)) ||
               (target.acceptsFieldKinds() && document::isFieldSocketKind(gesture.linkKind)) ||
               (target.acceptsImageMatte() && gesture.linkKind == document::SocketValueKind::Image);
    return document::isAcceptedSocketConnection(target.kind, gesture.linkKind) ||
           (gesture.linkAcceptsPixelKinds && document::isPixelSocketKind(target.kind)) ||
           (gesture.linkAcceptsFieldKinds && document::isFieldSocketKind(target.kind)) ||
           (gesture.linkAcceptsImageMatte && target.kind == document::SocketValueKind::Image);
}
} // namespace bloom::ui
