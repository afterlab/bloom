#pragma once
// Hit tests and landing-site queries for the Nodes canvas gestures (node_editor_interactions.cpp).
#include "node_editor_interactions.hpp"

#include <optional>
#include <utility>

class QGraphicsScene;

namespace bloom::ui::node_gesture {
// The socket under `point`, or within the zoom-compensated grab radius of it where no hosted field
// is under the pointer.
node_editor::SocketItem* socketAt(QGraphicsScene& scene, QPointF point);
node_editor::NodeItem* cardAt(QGraphicsScene& scene, QPointF point);
// The innermost frame under `point`: with nested frames the smallest one wins, so a drop has
// exactly one answer instead of depending on scene item order.
node_editor::NodeGroupItem* groupAt(QGraphicsScene& scene, QPointF point);
// Which frame a dropped card's CENTER lands in, by the same innermost rule.
std::optional<document::NodeGroupId> groupForDrop(QGraphicsScene& scene, QPointF center);
// True when a live hosted control -- not a label or gutter -- is under `point`.
bool fieldAt(QGraphicsScene& scene, QPointF point);
// Where an input's value comes from, whichever durable record carries it.
std::optional<document::OutputPortRef> incomingSource(const document::Composition& composition,
                                                      const document::InputPortRef& input);
node_editor::SocketItem* findOutput(QGraphicsScene& scene, const document::OutputPortRef& output);
// The image input and output a card dropped onto a link would be wired through.
std::pair<node_editor::SocketItem*, node_editor::SocketItem*>
insertionSockets(node_editor::NodeItem& card, const document::Composition& composition);
// Tells every socket on the canvas whether the link now being dragged could land on it.
void markLinkAffinity(QGraphicsScene& scene, const NodeInteraction& gesture,
                      const node_editor::SocketItem* origin);
void clearLinkAffinity(QGraphicsScene& scene);
// The Merge slot caret under the pointer during a link drag.
void updateOrderedDropIndicator(QGraphicsScene& scene, QPointF cursor);
// The link a single dragged card would be inserted into on release, emphasized while it is.
void previewInsertion(QGraphicsScene& scene, NodeInteraction& gesture,
                      const document::Composition& composition, QPointF cursor);
} // namespace bloom::ui::node_gesture
