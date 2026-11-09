// Issue #359: the Nodes canvas at scale. A pointer move costs the same on a 50-node graph as on a
// 600-node one, an edit allocates only the items it touches, and a projection whose composition
// content is unchanged is skipped outright. Every number here is a work counter -- lookups, linear
// item walks, item allocations -- never wall-clock time, so the suite cannot flake under load.

#include "node_interaction_test_support.hpp"

#include <QGraphicsItem>

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bloom::ui::test {
namespace {
using node_editor::NodeCanvasWorkCounters;
using node_editor::NodeEdgeItem;
using node_editor::NodeItem;
using node_editor::nodeCanvasWorkCounters;
using node_editor::SocketItem;

constexpr int kGroupSize = 5;

// The synthetic graph: `count` blur cards on a grid, chained image-to-image, framed five to a
// group. Three transactions, so building it costs three projections however large it is.
struct Graph final {
    std::vector<document::NodeId> nodes;
};

Graph populate(Fixture& f, const int count) {
    std::set<document::NodeId> before;
    for (const auto& node : f.session.composition()->graph().nodes())
        before.insert(node.id);
    {
        commands::Transaction transaction("Populate", f.session.snapshot().revision());
        for (int index = 0; index < count; ++index)
            transaction.emplace<commands::AddNode>(
                f.session.compositionId(), "bloom.gaussian-blur",
                // Rows apart by more than the tallest blur card (Gaussian Blur has ten rows of values).
                document::Vec2d{1000.0 + (index % 25) * 400.0, (index / 25) * 700.0});
        expect(f.scene()->submit(std::move(transaction)).succeeded(), "the cards are added");
    }
    Graph graph;
    for (const auto& node : f.session.composition()->graph().nodes())
        if (!before.contains(node.id))
            graph.nodes.push_back(node.id);
    expect(graph.nodes.size() == static_cast<std::size_t>(count), "every card was added");
    {
        commands::Transaction transaction("Chain", f.session.snapshot().revision());
        for (std::size_t index = 1; index < graph.nodes.size(); ++index)
            transaction.emplace<commands::ConnectPorts>(
                f.session.compositionId(), document::OutputPortRef{graph.nodes[index - 1], "image"},
                document::NodeInputRef{graph.nodes[index], "input"});
        expect(f.scene()->submit(std::move(transaction)).succeeded(), "the cards are chained");
    }
    {
        commands::Transaction transaction("Frame", f.session.snapshot().revision());
        for (std::size_t first = 0; first < graph.nodes.size(); first += kGroupSize) {
            std::set<document::NodeId> members;
            for (std::size_t index = first;
                 index < std::min(graph.nodes.size(), first + kGroupSize); ++index)
                members.insert(graph.nodes[index]);
            transaction.emplace<commands::GroupNodes>(f.session.compositionId(), members);
        }
        expect(f.scene()->submit(std::move(transaction)).succeeded(), "the cards are framed");
    }
    QCoreApplication::processEvents();
    return graph;
}

// Brings `card` to a fixed place in the viewport at 100%, so a gesture's events land on it.
void showCard(Fixture& f, NodeItem& card) {
    const QPointF top = card.scenePos();
    f.view()->setTransform(QTransform::fromTranslate(200.0 - top.x(), 200.0 - top.y()));
    QCoreApplication::processEvents();
}

QPointF headerPoint(NodeItem& card) {
    return card.mapToScene(QPointF(card.cardWidth() / 2.0, node_editor::kCardHeaderHeight / 2.0));
}

struct MoveCost final {
    std::size_t lookups = 0;
    std::size_t scanned = 0;
    std::size_t created = 0;
};

constexpr int kMoves = 20;

// The per-move work of one drag: everything the moves themselves do, divided by their count. The
// press and the release are outside the measurement; they are not per-move paths.
template <class Press, class Release>
MoveCost measureMoves(Fixture& f, const QPointF start, Press press, Release release) {
    press(start);
    auto& counters = nodeCanvasWorkCounters();
    counters = {};
    for (int step = 1; step <= kMoves; ++step)
        f.move(start + QPointF(step * 7.0, step * 3.0));
    const MoveCost cost{counters.itemLookups / kMoves, counters.itemsScanned / kMoves,
                        (counters.cardsCreated + counters.socketsCreated + counters.edgesCreated) /
                            kMoves};
    release(start + QPointF(kMoves * 7.0, kMoves * 3.0));
    return cost;
}

struct ScaleCosts final {
    MoveCost cardDrag;
    MoveCost selectionDrag;
    MoveCost frameDrag;
    MoveCost linkDrag;
};

ScaleCosts measureAt(const int count) {
    Fixture f;
    const auto graph = populate(f, count);
    ScaleCosts costs;
    // Card 15 sits in the middle of its frame and the chain, so a drag moves two links and
    // reshapes one frame.
    auto* card = f.card(graph.nodes[15]);
    expect(card != nullptr, "the dragged card exists");
    if (card == nullptr)
        return costs;
    showCard(f, *card);
    const auto press = [&f](const QPointF point) { f.press(point); };
    const auto release = [&f](const QPointF point) { f.release(point); };
    costs.cardDrag = measureMoves(f, headerPoint(*card), press, release);

    // Three selected cards, from two frames, travel together.
    f.session.selectNodes({graph.nodes[15], graph.nodes[16], graph.nodes[25]}, graph.nodes[15]);
    QCoreApplication::processEvents();
    card = f.card(graph.nodes[15]);
    showCard(f, *card);
    costs.selectionDrag = measureMoves(f, headerPoint(*card), press, release);

    // A frame's own drag carries its five members.
    f.session.clearSelection();
    QCoreApplication::processEvents();
    card = f.card(graph.nodes[33]);
    showCard(f, *card);
    auto* frame = dynamic_cast<node_editor::NodeGroupItem*>(
        f.scene()->findNodeGroupItem(document::findNodeGroupOf(
                                         f.session.composition()->nodeGroups(), graph.nodes[33])
                                         ->id));
    expect(frame != nullptr, "the dragged frame exists");
    if (frame != nullptr)
        costs.frameDrag = measureMoves(
            f, frame->mapToScene(frame->titleRect().center() + QPointF(40.0, 0.0)), press,
            release);

    // A link dragged out of an output and dropped back on empty canvas (Escape-equivalent: a
    // right press cancels it, so no Add search opens).
    card = f.card(graph.nodes[40]);
    showCard(f, *card);
    SocketItem* output = nullptr;
    for (auto* socket : card->sockets())
        if (socket->output)
            output = socket;
    expect(output != nullptr, "the dragged output exists");
    if (output != nullptr)
        costs.linkDrag = measureMoves(f, output->scenePos(), press, [&f](const QPointF point) {
            f.press(point, Qt::NoModifier, Qt::RightButton);
            f.release(point, Qt::NoModifier, Qt::RightButton);
        });
    return costs;
}

void report(const char* name, const MoveCost& small, const MoveCost& large) {
    std::cerr << name << ": per move at 50 nodes " << small.lookups << " lookups, "
              << small.scanned << " items scanned; at 600 nodes " << large.lookups
              << " lookups, " << large.scanned << " items scanned\n";
}

void expectFlat(const char* name, const MoveCost& small, const MoveCost& large) {
    report(name, small, large);
    expect(small.lookups == large.lookups, "per-move lookups do not depend on the node count");
    expect(large.scanned == 0, "a move walks no list of scene items");
    expect(large.created == 0 && small.created == 0, "a move allocates no canvas item");
}

// Issue #359, acceptance: per-move cost is independent of node count.
void testPerMoveCostIsIndependentOfNodeCount() {
    const auto small = measureAt(50);
    const auto large = measureAt(600);
    expectFlat("card drag", small.cardDrag, large.cardDrag);
    expectFlat("selection drag", small.selectionDrag, large.selectionDrag);
    expectFlat("frame drag", small.frameDrag, large.frameDrag);
    expectFlat("link drag", small.linkDrag, large.linkDrag);
    expect(large.cardDrag.lookups > 0, "a card drag looks its card up by id");
}

struct ItemIdentity final {
    std::map<document::NodeId, NodeItem*> cards;
    std::map<document::NodeId, std::vector<SocketItem*>> sockets;
    std::set<NodeEdgeItem*> edges;
};

ItemIdentity identity(Fixture& f) {
    ItemIdentity result;
    for (auto* item : f.scene()->items()) {
        if (auto* card = dynamic_cast<NodeItem*>(item)) {
            result.cards.emplace(card->id(), card);
            result.sockets.emplace(card->id(), card->sockets());
        } else if (auto* edge = dynamic_cast<NodeEdgeItem*>(item)) {
            result.edges.insert(edge);
        }
    }
    return result;
}

std::size_t sharedEdges(const ItemIdentity& left, const ItemIdentity& right) {
    std::size_t shared = 0;
    for (auto* edge : left.edges)
        shared += right.edges.contains(edge) ? 1U : 0U;
    return shared;
}

bool sameCardsAndSockets(const ItemIdentity& left, const ItemIdentity& right,
                         const std::set<document::NodeId>& except = {}) {
    for (const auto& [id, card] : left.cards) {
        if (except.contains(id))
            continue;
        const auto found = right.cards.find(id);
        if (found == right.cards.end() || found->second != card ||
            right.sockets.at(id) != left.sockets.at(id))
            return false;
    }
    return true;
}

// Issue #359, acceptance: one edit changes only the items it touches.
void testEditTouchesOnlyItsItems() {
    Fixture f;
    const auto graph = populate(f, 200);
    auto& counters = nodeCanvasWorkCounters();

    // A layout edit -- one card moves -- allocates nothing and keeps every item.
    auto before = identity(f);
    counters = {};
    expect(f.edit<commands::MoveNodes>(
                std::map<document::NodeId, document::Vec2d>{{graph.nodes[7], {-900.0, -900.0}}})
               .succeeded(),
           "the card moves");
    QCoreApplication::processEvents();
    auto after = identity(f);
    std::cerr << "move edit: " << counters.cardsCreated << " cards, " << counters.socketsCreated
              << " sockets, " << counters.edgesCreated << " edges created; "
              << counters.edgesDeleted << " edges deleted\n";
    expect(counters.projections == 1, "the edit is projected once");
    expect(counters.cardsCreated == 0 && counters.socketsCreated == 0 &&
               counters.edgesCreated == 0 && counters.edgesDeleted == 0,
           "a layout edit allocates and frees no card, socket or link");
    expect(sameCardsAndSockets(before, after), "every card and socket keeps its identity");
    expect(after.edges == before.edges, "every link keeps its identity");

    // A value edit on one card does the same.
    before = after;
    counters = {};
    expect(f.edit<commands::SetNodeParameterValue>(graph.nodes[3], std::string("radius"),
                                                   document::ParameterValue{12.5})
               .succeeded(),
           "the radius changes");
    QCoreApplication::processEvents();
    after = identity(f);
    expect(counters.cardsCreated == 0 && counters.socketsCreated == 0 &&
               counters.edgesCreated == 0 && counters.edgesDeleted == 0,
           "a value edit allocates and frees no card, socket or link");
    expect(sameCardsAndSockets(before, after) && after.edges == before.edges,
           "a value edit keeps every item's identity");

    // Disconnecting one link frees exactly that link; its two ends keep their sockets.
    before = after;
    counters = {};
    expect(f.edit<commands::DisconnectInput>(document::NodeInputRef{graph.nodes[50], "input"})
               .succeeded(),
           "the link is disconnected");
    QCoreApplication::processEvents();
    after = identity(f);
    std::cerr << "disconnect edit: " << counters.socketsCreated << " sockets, "
              << counters.edgesCreated << " edges created; " << counters.edgesDeleted
              << " edges deleted\n";
    expect(counters.edgesDeleted == 1 && counters.edgesCreated == 0 &&
               counters.socketsCreated == 0 && counters.cardsCreated == 0,
           "a disconnect frees one link and allocates nothing");
    expect(after.edges.size() + 1 == before.edges.size() &&
               sharedEdges(before, after) == after.edges.size(),
           "every other link keeps its identity");
    expect(sameCardsAndSockets(before, after), "both ends keep their sockets");

    // Reconnecting it allocates exactly one link.
    before = after;
    counters = {};
    expect(f.edit<commands::ConnectPorts>(document::OutputPortRef{graph.nodes[49], "image"},
                                          document::NodeInputRef{graph.nodes[50], "input"})
               .succeeded(),
           "the link is reconnected");
    QCoreApplication::processEvents();
    after = identity(f);
    expect(counters.edgesCreated == 1 && counters.edgesDeleted == 0 &&
               counters.socketsCreated == 0 && counters.cardsCreated == 0,
           "a connect allocates one link and nothing else");
    expect(sharedEdges(before, after) == before.edges.size(), "every other link is kept");
    expect(sameCardsAndSockets(before, after), "both ends keep their sockets");

    // Adding a card allocates that card and its own sockets only.
    before = after;
    counters = {};
    const auto added = f.add("bloom.gaussian-blur", {-3000.0, -3000.0});
    QCoreApplication::processEvents();
    after = identity(f);
    expect(counters.cardsCreated == 1 && counters.edgesCreated == 0 &&
               counters.edgesDeleted == 0 &&
               counters.socketsCreated == after.sockets.at(added).size(),
           "an add allocates one card and its own sockets");
    expect(sameCardsAndSockets(before, after) && after.edges == before.edges,
           "an add keeps every existing item");
}

// Issue #359: a projection whose composition content is unchanged is skipped, and the playhead
// still rebuilds nothing (#401).
void testUnchangedContentSkipsTheProjection() {
    Fixture f;
    const auto graph = populate(f, 60);
    auto& counters = nodeCanvasWorkCounters();
    const auto before = identity(f);
    counters = {};
    Q_EMIT f.session.snapshotChanged();
    Q_EMIT f.session.compositionChanged();
    QCoreApplication::processEvents();
    expect(counters.projectionsSkipped == 2 && counters.projections == 0,
           "republishing the same content skips the projection");
    expect(counters.cardRefreshes == 0 && counters.itemsScanned == 0,
           "a skipped projection refreshes no card and walks no item list");
    expect(sameCardsAndSockets(before, identity(f)), "a skipped projection keeps every item");

    counters = {};
    expect(f.edit<commands::MoveNodes>(
                std::map<document::NodeId, document::Vec2d>{{graph.nodes[2], {-700.0, -700.0}}})
               .succeeded(),
           "a layout edit");
    QCoreApplication::processEvents();
    // The session publishes an edit as snapshotChanged then compositionChanged: the first
    // projects the new content and the second, carrying the same content, is skipped.
    expect(counters.projections == 1, "changed content is projected once");

    counters = {};
    for (int frame = 1; frame <= 48; ++frame) {
        if (const auto time = core::RationalTime::create(frame, 24))
            static_cast<void>(f.session.setCurrentTime(*time));
        QCoreApplication::processEvents();
    }
    expect(counters.projections == 0 && counters.projectionsSkipped == 0 &&
               counters.cardRefreshes == 0 && counters.cardsCreated == 0 &&
               counters.socketsCreated == 0 && counters.edgesCreated == 0,
           "playback projects nothing and rebuilds no card");
    expect(counters.itemsScanned == 0, "playback walks no list of scene items");
}
} // namespace
} // namespace bloom::ui::test

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    using namespace bloom::ui::test;
    testPerMoveCostIsIndependentOfNodeCount();
    testEditTouchesOnlyItsItems();
    testUnchangedContentSkipsTheProjection();
    if (failures != 0) {
        std::cerr << failures << " node canvas scale expectation(s) failed\n";
        return 1;
    }
    return 0;
}
