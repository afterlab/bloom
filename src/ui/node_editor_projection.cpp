// The Nodes canvas projection (#359): reconciles the scene against one composition by stable id,
// touching only the cards, sockets, links and frames an edit changed, and skipping the whole pass
// when the composition's content is the content already on screen.
#include "node_editor_projection.hpp"

#include "node_editor_interactions.hpp"
#include "properties_registry_row.hpp"

#include <bloom/document/node_group_nodes.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::ui {
using namespace node_editor;

namespace node_editor {
NodeInputKey nodeInputKey(const document::InputPortRef& input) {
    if (const auto* fixed = std::get_if<document::NodeInputRef>(&input))
        return {0, fixed->nodeId.value(), 0, fixed->port};
    const auto& slot = std::get<document::LayerStackInputRef>(input);
    return {1, slot.stackNodeId.value(), slot.slotId.value(), slot.role};
}

NodeLinkKey nodeLinkKey(const document::EdgeRecord& link) {
    return {nodeInputKey(link.destination), link.source.nodeId.value(), link.source.port,
            link.id.value()};
}

NodeProjectionIndex buildProjectionIndex(const document::Composition& composition) {
    // Task FIX1, item A: a driven operand's link is recorded as its parameter's driver binding
    // rather than as an edge, so the canvas's one link list is the graph's edges FOLLOWED BY every
    // driver binding, drawn as the same NodeEdgeItem so hover, selection emphasis, cutting and the
    // pick-up gesture all reach them without a second code path. A driver link carries no EdgeId
    // (there is no edge to carry one), so it is addressed by its destination, which is what
    // DisconnectInput already takes.
    NodeProjectionIndex index;
    const auto edges = composition.graph().edges();
    index.links.assign(edges.begin(), edges.end());
    for (const auto& edge : edges) {
        index.edgeInputs.insert(nodeInputKey(edge.destination));
        index.linkedOutputs[edge.source.nodeId.value()].insert(edge.source.port);
    }
    for (const auto& node : composition.graph().nodes()) {
        for (const auto& binding : node.parameters) {
            const auto* parameter = composition.parameters().find(binding.parameterId);
            const auto* driver =
                parameter == nullptr
                    ? nullptr
                    : std::get_if<document::DriverBindingSource>(&parameter->source);
            if (driver == nullptr)
                continue;
            index.linkedOutputs[driver->sourceNodeId.value()].insert(driver->outputPort);
            index.links.push_back({document::EdgeId{},
                                   document::OutputPortRef{driver->sourceNodeId, driver->outputPort},
                                   document::NodeInputRef{node.id, binding.role}});
        }
    }
    return index;
}

bool NodeProjectionInputs::sameContent(const NodeProjectionInputs& other) const {
    return snapshot.has_value() && other.snapshot.has_value() && composition == other.composition &&
           content == other.content && assets == other.assets && assetCount == other.assetCount &&
           compositions == other.compositions &&
           compositionColorSpace == other.compositionColorSpace &&
           colorSettings == other.colorSettings && workingColorSpace == other.workingColorSpace &&
           colorConverterReady == other.colorConverterReady && slotLabels == other.slotLabels;
}
} // namespace node_editor

namespace {
// What a projection of `composition` would read. A card reads more than its own composition's
// content -- an asset's name and kind, a nested composition's name, the colour settings its
// colour-space selectors list, whether its colour chips have a converter yet -- so all of those
// are part of "unchanged".
NodeProjectionInputs projectionInputs(const document::Snapshot& snapshot,
                                      const document::Composition& composition,
                                      const CompositionSession* session) {
    NodeProjectionInputs inputs;
    inputs.snapshot = snapshot;
    inputs.composition = composition.id();
    inputs.content = composition.contentIdentity();
    const auto assets = snapshot.project().assets();
    inputs.assets = assets.data();
    inputs.assetCount = assets.size();
    for (const auto& candidate : snapshot.project().compositions())
        inputs.compositions.emplace_back(candidate.id().value(), candidate.name());
    inputs.compositionColorSpace = composition.workingColorSpaceId();
    if (session != nullptr) {
        inputs.colorSettings = session->colorSettings();
        inputs.workingColorSpace = std::string(session->colorIntent().workingColorSpaceId);
        inputs.colorConverterReady = session->colorConverterReady();
    }
    for (const auto& node : composition.graph().nodes()) {
        if (node.typeId != document::kNodeGroupNodeType)
            continue;
        for (const auto& slot : document::kNodeGroupSlots)
            if (const auto label =
                    nodeGroupSlotLabel(snapshot.project(), composition, node.id, slot.name))
                inputs.slotLabels[{node.id.value(), std::string(slot.name)}] = *label;
    }
    return inputs;
}
} // namespace

void NodeGraphicsScene::setProjection(const document::Snapshot& snapshot,
                                      const document::CompositionId compositionId) {
    auto& counters = nodeCanvasWorkCounters();
    const auto* composition = snapshot.project().findComposition(compositionId);
    if (composition == nullptr) {
        cancelGesture();
        ++counters.projections;
        index_->clear();
        clear();
        setSceneRect({});
        return;
    }
    auto inputs = projectionInputs(snapshot, *composition, session_);
    if (index_->projected.sameContent(inputs)) {
        ++counters.projectionsSkipped;
        // A gesture's commit is checked against the revision it froze, so one the publication
        // moved past could only be refused; it ends here exactly as a real projection would end it.
        // #703: a placement never froze a revision; its click adds to whatever is current.
        if (gestureActive() && !placementActive() &&
            interaction_->revision != snapshot.revision())
            cancelGesture();
        // The new snapshot shares this content, so holding it instead releases the older one.
        index_->projected = std::move(inputs);
        return;
    }
    // #703: a placement outlives a projection (an asynchronous publication must not snatch the
    // card from under the pointer); only the link it targeted is let go, as links are rebuilt.
    if (placementActive())
        releasePlacementTarget();
    else
        cancelGesture();
    ++counters.projections;

    index_->links = buildProjectionIndex(*composition);
    index_->links.socketLabels = inputs.slotLabels;
    const auto& links = index_->links;
    // Every link re-registers with its two cards below, kept or new.
    for (const auto& [id, card] : index_->nodes)
        card->clearEdges();

    const auto defaults = document::defaultNodeLayout(composition->graph().nodes());
    std::unordered_map<std::uint64_t, NodeItem*> present;
    present.reserve(composition->graph().nodes().size());
    for (const auto& node : composition->graph().nodes()) {
        NodeItem* item = index_->node(node.id);
        if (item == nullptr) {
            item = new NodeItem(node.id, session_);
            ++counters.cardsCreated;
            addItem(item);
        }
        const auto foundLayout = composition->nodeLayout().find(node.id);
        const auto& layout = foundLayout == composition->nodeLayout().end() ? defaults.at(node.id)
                                                                            : foundLayout->second;
        item->refresh(node, *composition, layout, links);
        item->setAuthoringEnabled(canSubmit());
        item->setPos(layout.position.x, layout.position.y);
        present.emplace(node.id.value(), item);
    }
    for (const auto& [id, item] : index_->nodes) {
        if (present.contains(id))
            continue;
        removeItem(item);
        // deleteLater(), not delete: a card can be dropped from inside one of its own field
        // widgets' signal emission (an edit that removes the node), and unwinding through a
        // freed widget is not something a projection may risk.
        item->deleteLater();
    }
    index_->nodes = std::move(present);

    // Only nodes without artist-authored layout use this measured presentation grid.
    // Preserve the document's topological row/column grouping, but allow the real kit rows
    // to determine cell extents so newly visible controls cannot overlap adjacent cards.
    std::map<double, qreal> columnWidths, rowHeights;
    std::vector<std::pair<NodeItem*, document::NodeLayoutRecord>> unplaced;
    for (const auto& node : composition->graph().nodes()) {
        if (composition->nodeLayout().contains(node.id))
            continue;
        auto* card = index_->node(node.id);
        const auto& position = defaults.at(node.id);
        columnWidths[position.position.x] =
            std::max(columnWidths[position.position.x], card->cardRect().width());
        rowHeights[position.position.y] =
            std::max(rowHeights[position.position.y], card->cardRect().height());
        unplaced.emplace_back(card, position);
    }
    qreal offset = 0;
    for (auto& [key, extent] : columnWidths) {
        const auto next = offset + extent + kit::px(kit::Size::NodeColumnGap);
        extent = offset;
        offset = next;
    }
    offset = 0;
    for (auto& [key, extent] : rowHeights) {
        const auto next = offset + extent + kit::px(kit::Size::NodeRowGap);
        extent = offset;
        offset = next;
    }
    for (const auto& [card, position] : unplaced)
        card->setPos(columnWidths.at(position.position.x), rowHeights.at(position.position.y));

    rebuildGroups(*composition);
    reconcileEdges(links);
    updateGroupGeometry();
    index_->timeVarying.clear();
    for (const auto& node : composition->graph().nodes())
        if (auto* card = index_->node(node.id); card->timeVarying())
            index_->timeVarying.push_back(card);
    index_->projected = std::move(inputs);
    if (placementActive())
        updatePlacement(interaction_->origin);
    const QRectF bounds = itemsBoundingRect();
    setSceneRect(bounds.isEmpty() ? QRectF(-kNodeSceneMargin, -kNodeSceneMargin,
                                           (kNodeSceneMargin + kNodeSceneMargin),
                                           (kNodeSceneMargin + kNodeSceneMargin))
                                  : bounds.adjusted(-kNodeSceneMargin, -kNodeSceneMargin,
                                                    kNodeSceneMargin, kNodeSceneMargin));
}

std::vector<NodeItem*> NodeGraphicsScene::nodeItems() const {
    std::vector<NodeItem*> cards;
    cards.reserve(index_->nodes.size());
    for (const auto& [id, card] : index_->nodes)
        cards.push_back(card);
    return cards;
}

void NodeGraphicsScene::refreshValues() {
    const auto* composition =
        session_ == nullptr
            ? nullptr
            : session_->snapshot().project().findComposition(session_->compositionId());
    if (composition == nullptr) {
        return;
    }
    for (const auto& node : composition->graph().nodes())
        if (auto* card = index_->node(node.id))
            card->refreshCurrentValues(node, *composition);
}

void NodeGraphicsScene::refreshTimeValues(const bool staleOnly) {
    const auto* composition =
        session_ == nullptr
            ? nullptr
            : session_->snapshot().project().findComposition(session_->compositionId());
    if (composition == nullptr)
        return;
    QRectF shown;
    for (const auto* view : views())
        if (view != nullptr && view->isVisible())
            shown = shown.united(view->mapToScene(view->viewport()->rect()).boundingRect());
    for (auto* card : index_->timeVarying) {
        if (staleOnly && !card->timeStale())
            continue;
        if (!shown.intersects(card->sceneBoundingRect())) {
            card->setTimeStale(true);
            continue;
        }
        if (const auto* node = composition->graph().findNode(card->id()))
            card->refreshTimeValues(*node, *composition);
    }
}

QGraphicsItem* NodeGraphicsScene::findNodeItem(const document::NodeId nodeId) const {
    ++nodeCanvasWorkCounters().itemLookups;
    return index_->node(nodeId);
}

QGraphicsItem* NodeGraphicsScene::findNodeGroupItem(const document::NodeGroupId groupId) const {
    ++nodeCanvasWorkCounters().itemLookups;
    return index_->group(groupId);
}

// Frames are reconciled by their stable NodeGroupId exactly as cards are, because an inline title
// edit in flight has to survive the snapshot change its own rename produced.
void NodeGraphicsScene::rebuildGroups(const document::Composition& composition) {
    std::unordered_map<std::uint64_t, NodeGroupItem*> present;
    index_->memberGroup.clear();
    for (const auto& [id, record] : composition.nodeGroups()) {
        auto* item = index_->group(id);
        if (item == nullptr) {
            item = new NodeGroupItem(id, session_);
            addItem(item);
        }
        item->refresh(record);
        item->setAuthoringEnabled(canSubmit());
        present.emplace(id.value(), item);
        for (const auto member : record.members)
            index_->memberGroup.emplace(member.value(), item);
    }
    for (const auto& [id, item] : index_->groups) {
        if (present.contains(id))
            continue;
        removeItem(item);
        // deleteLater(), not delete: a frame can be dropped from inside its own title editor's
        // signal emission -- the rename that ungrouped it -- and unwinding through a freed widget
        // is not something a projection may risk.
        item->deleteLater();
    }
    index_->groups = std::move(present);
}

// The frame is the bounding rectangle of its member cards plus the record's own padding, with room
// for the title strip above. A member the gesture in flight is dragging OUT is excluded, so the
// frame holds still and the artist can see where the card is landing; a gesture that carries the
// whole frame excludes nothing and the frame travels with its members.
void NodeGraphicsScene::updateGroupFrame(NodeGroupItem& group) {
    const bool frameDrag = interaction_->movedGroup.has_value();
    std::optional<QRectF> settled;
    std::optional<QRectF> every;
    for (const auto id : group.members()) {
        const auto* card = dynamic_cast<NodeItem*>(findNodeItem(id));
        if (card == nullptr)
            continue;
        const QRectF rect = card->mapRectToScene(card->cardRect());
        every = every ? every->united(rect) : rect;
        if (!frameDrag && interaction_->positions.contains(id))
            continue;
        settled = settled ? settled->united(rect) : rect;
    }
    const auto bounds = settled ? settled : every;
    group.setVisible(bounds.has_value());
    if (!bounds)
        return;
    const auto padding = group.padding();
    group.setFrameRect(
        bounds->adjusted(-padding.x, -padding.y - kGroupTitleHeight, padding.x, padding.y));
}

void NodeGraphicsScene::updateGroupGeometry() {
    for (const auto& [id, group] : index_->groups)
        updateGroupFrame(*group);
}

// Only a frame that holds a card the gesture moves can change shape, so a move reshapes those and
// no others: the cost follows the selection, never the size of the graph.
void NodeGraphicsScene::updateGroupGeometry(const std::map<document::NodeId, QPointF>& moved) {
    std::set<NodeGroupItem*> affected;
    for (const auto& [id, position] : moved)
        if (const auto found = index_->memberGroup.find(id.value());
            found != index_->memberGroup.end())
            affected.insert(found->second);
    for (auto* group : affected)
        updateGroupFrame(*group);
}

QWidget* NodeGraphicsScene::nodeFieldForTest(const document::NodeId nodeId,
                                             const QString& fieldObjectName) const {
    auto* item = index_->node(nodeId);
    return item == nullptr ? nullptr : item->fieldWidget(fieldObjectName);
}

QList<QWidget*> NodeGraphicsScene::nodeControlsForRoleForTest(const document::NodeId nodeId,
                                                              const QString& role) const {
    const auto* item = index_->node(nodeId);
    return item == nullptr ? QList<QWidget*>{} : item->controlsForRole(role);
}

NodeSockets NodeGraphicsScene::nodeSocketsForTest(const document::NodeId nodeId) const {
    const auto* item = index_->node(nodeId);
    return item == nullptr ? NodeSockets{}
                           : NodeSockets{item->hasInputSocket(), item->hasOutputSocket()};
}

void NodeGraphicsScene::setLinkStyle(const LinkStyle style) {
    if (linkStyle_ == style) {
        return;
    }
    linkStyle_ = style;
    for (const auto& [key, edge] : index_->edges)
        edge->setLinkStyle(style);
}

// Links are reconciled by what they connect. A link whose two cards and two sockets are the very
// items already drawing it is kept and only re-registered with its cards; one whose end changed is
// replaced, one that left the graph is removed, and only a link the edit created is allocated.
void NodeGraphicsScene::reconcileEdges(const NodeProjectionIndex& links) {
    auto& counters = nodeCanvasWorkCounters();
    std::map<NodeLinkKey, NodeEdgeItem*> present;
    for (const auto& link : links.links) {
        auto* source = index_->node(link.source.nodeId);
        auto* destination = index_->node(destinationNodeId(link.destination));
        if (source == nullptr || destination == nullptr)
            continue;
        SocketItem* output = nullptr;
        SocketItem* input = nullptr;
        for (auto* socket : source->sockets())
            if (socket->output == link.source)
                output = socket;
        for (auto* socket : destination->sockets())
            // accepts(), not an equality test: the Merge node's one ordered multi-input stands
            // for every stack slot, so every edge terminating on any of them terminates here.
            if (socket->accepts(link.destination))
                input = socket;
        if (output == nullptr || input == nullptr)
            continue;
        auto key = nodeLinkKey(link);
        if (present.contains(key))
            continue;
        NodeEdgeItem* edge = nullptr;
        // joins() compares addresses only: a stale link's ends may already be gone, and it is
        // never dereferenced before it is deleted below.
        if (const auto found = index_->edges.find(key);
            found != index_->edges.end() &&
            found->second->joins(*source, *destination, *output, *input)) {
            edge = found->second;
            index_->edges.erase(found);
            edge->reattach();
        } else {
            // Task FIX1, item C: no link is structural any more. Task NODES-1: constructed with
            // THIS scene's own current link style, so a fresh link can never disagree with
            // setLinkStyle()'s own live repaint of every link already in the scene.
            edge = new NodeEdgeItem(*source, *destination, *output, *input, link, false,
                                    linkStyle_);
            ++counters.edgesCreated;
            addItem(edge);
        }
        present.emplace(std::move(key), edge);
    }
    for (const auto& [key, edge] : index_->edges) {
        ++counters.edgesDeleted;
        removeItem(edge);
        delete edge;
    }
    index_->edges = std::move(present);
}
} // namespace bloom::ui
