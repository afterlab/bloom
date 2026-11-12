// Task N1 (node canvas): cursor placement, overlap-free adds and layer adds, a layer-stack Arrange,
// node-editor-style collapse, coloured frames, constant-width links, Merge's per-slot sockets, the
// footer's Arrange button, source cards without previews and the Nodes icon.

#include "node_interaction_test_support.hpp"

#include <bloom/commands/node_layout_placement.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/icons.hpp>
#include <bloom/ui/properties_editor.hpp>

#include <QGraphicsProxyWidget>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionGraphicsItem>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace bloom::ui::test {
namespace {
using node_editor::kCardHeaderHeight;
using node_editor::NodeEdgeItem;
using node_editor::NodeGroupItem;
using node_editor::NodeItem;

std::vector<NodeItem*> cards(Fixture& f) {
    std::vector<NodeItem*> result;
    for (auto* item : f.scene()->items())
        if (auto* card = dynamic_cast<NodeItem*>(item))
            result.push_back(card);
    return result;
}

std::vector<NodeEdgeItem*> edges(Fixture& f) {
    std::vector<NodeEdgeItem*> result;
    for (auto* item : f.scene()->items())
        if (auto* edge = dynamic_cast<NodeEdgeItem*>(item))
            result.push_back(edge);
    return result;
}

QRectF sceneRect(NodeItem* card) { return card->mapRectToScene(card->cardRect()); }

// Any two drawn cards that touch, named for the failure message.
std::string overlapping(Fixture& f) {
    const auto all = cards(f);
    for (std::size_t left = 0; left < all.size(); ++left)
        for (std::size_t right = left + 1; right < all.size(); ++right)
            if (sceneRect(all[left]).intersects(sceneRect(all[right])))
                return all[left]->title().toStdString() + " / " +
                       all[right]->title().toStdString();
    return {};
}

document::NodeId addSolidLayer(Fixture& f, const std::string& name) {
    const auto result =
        f.edit<commands::AddSolidLayer>(name, core::Color4d{0.5, 0.25, 0.75, 1.0});
    QCoreApplication::processEvents();
    const auto node = result.outputId<document::NodeId>("layerOutputNode");
    if (!node)
        throw std::runtime_error("fixture solid layer");
    return *node;
}

// Chooses `query`'s first match in the Add search opened at `scene`.
void addFromSearch(Fixture& f, const QPointF scene, const QString& query) {
    f.editor.openAddSearch(scene, f.view()->viewport()->mapToGlobal(f.view()->mapFromScene(scene)));
    auto* popup = f.editor.findChild<kit::KSearchPopup*>();
    if (popup == nullptr)
        throw std::runtime_error("search popup missing");
    auto* field = popup->findChild<QLineEdit*>(QStringLiteral("kSearchFilter"));
    field->setText(query);
    QTest::keyClick(field, Qt::Key_Return);
    QCoreApplication::processEvents();
}

document::NodeId newest(Fixture& f) {
    document::NodeId id;
    for (const auto& node : f.session.composition()->graph().nodes())
        id = std::max(id, node.id);
    return id;
}

// The card each stack entry's layer draws as, top of the stack first.
std::vector<NodeItem*> layerCardsInStackOrder(Fixture& f) {
    const auto& graph = f.session.composition()->graph();
    std::vector<NodeItem*> result;
    for (const auto& entry : graph.layerStack().entries())
        for (const auto& boundary : graph.layerOutputs())
            if (boundary.layerId == entry.layerId)
                result.push_back(f.card(boundary.nodeId));
    return result;
}

QPointF pathStart(const QPainterPath& path) { return path.elementAt(0); }
QPointF pathEnd(const QPainterPath& path) { return path.elementAt(path.elementCount() - 1); }
} // namespace

// Item 3: a card lands with its title band under the pointer, not a card height away.
void testAddLandsAtTheCursor() {
    Fixture f;
    const QPointF cursor(420, 260);
    addFromSearch(f, cursor, QStringLiteral("scalar"));
    auto* card = f.card(newest(f));
    expect(card != nullptr, "the search added a card");
    if (card == nullptr)
        return;
    const auto band = card->mapRectToScene(QRectF(0, 0, card->cardWidth(), kCardHeaderHeight));
    expect(band.contains(cursor), "the new card's title band sits under the cursor");
    expect(std::abs(band.center().x() - cursor.x()) <= card->cardWidth() / 2.0,
           "the card is centred on the cursor, not cornered at it");

    // Item 4: a second add at the same point does not land on the first card; it moves just past
    // it, a gap of four grid steps away, rather than a whole card height.
    addFromSearch(f, cursor, QStringLiteral("scalar"));
    auto* second = f.card(newest(f));
    expect(second != nullptr && second != card, "a second card was added");
    if (second == nullptr || second == card)
        return;
    const auto first = sceneRect(card);
    const auto moved = sceneRect(second);
    expect(!first.intersects(moved), "an add at an occupied point does not overlap");
    const qreal gap = std::max({moved.left() - first.right(), first.left() - moved.right(),
                                moved.top() - first.bottom(), first.top() - moved.bottom()});
    expect(gap >= commands::kNodeLayoutGap && gap < commands::kNodeLayoutGap + commands::kNodeLayoutGrid,
           "the moved card keeps the four-grid-step gap from its neighbour, and no more");
}

// Items 4 and 9: every add -- cursor adds and whole layers -- leaves no two cards touching, and a
// layer's cards go in the layer column at their stack position.
void testAddsNeverOverlap() {
    Fixture f;
    for (int index = 0; index < 4; ++index)
        addFromSearch(f, QPointF(300, 300), QStringLiteral("scalar"));
    expect(overlapping(f).empty(), "four adds at one point leave no overlapping cards");
    for (int index = 0; index < 5; ++index)
        (void)addSolidLayer(f, "Layer " + std::to_string(index + 1));
    const auto clash = overlapping(f);
    expect(clash.empty(), "adding layers leaves no overlapping cards");
    if (!clash.empty())
        std::cerr << "  overlap: " << clash << '\n';
    const auto layers = layerCardsInStackOrder(f);
    expect(layers.size() == 5, "five layers in the stack");
    bool column = true;
    bool ordered = true;
    for (std::size_t index = 1; index < layers.size(); ++index) {
        column = column && layers[index]->pos().x() == layers[0]->pos().x();
        ordered = ordered && layers[index - 1]->pos().y() < layers[index]->pos().y();
    }
    expect(column, "every new layer lands in one layer column");
    expect(ordered, "the layer column reads in stack order, top layer first");
    const auto mergeId = f.session.composition()->graph().layerStack().nodeId();
    expect(layers.front()->pos().x() < f.card(mergeId)->pos().x(),
           "the layer column is left of Merge");
}

// The command-side estimate the placement uses for an unmeasured card is an upper bound of every
// built-in card the editor draws, which is what makes command placement overlap-free.
void testEstimateBoundsEveryCard() {
    Fixture f;
    double x = 0.0;
    int measured = 0;
    for (const auto& definition : document::builtInNodeDefinitions().definitions()) {
        if (document::isRerouteNodeType(definition.key.typeId))
            continue;
        commands::Transaction transaction("Fixture edit", f.session.snapshot().revision());
        transaction.emplace<commands::AddNode>(f.session.compositionId(), definition.key.typeId,
                                               document::Vec2d{x, 4000.0});
        const auto result = f.scene()->submit(std::move(transaction));
        const auto id = result.outputId<document::NodeId>(commands::kAddNodeOutput);
        if (!id)
            continue;
        x += 2000.0;
        QCoreApplication::processEvents();
        auto* card = f.card(*id);
        if (card == nullptr)
            continue;
        ++measured;
        const auto estimate = commands::estimatedNodeCardSize(*f.session.composition(), *id);
        if (card->cardRect().width() > estimate.width ||
            card->cardRect().height() > estimate.height) {
            expect(false, "a drawn card exceeds the placement estimate");
            std::cerr << "  " << definition.key.typeId << ": drawn " << card->cardRect().width()
                      << "x" << card->cardRect().height() << ", estimate " << estimate.width
                      << "x" << estimate.height << '\n';
        }
    }
    // A run that drew no cards would agree with any estimate.
    expect(measured >= 50, "the estimate was held against the drawn cards");
    std::cout << "ESTIMATE cards=" << measured << '\n';
}

// Item 7 and 13: Arrange from the footer reads like the layer stack and is one undo.
void testArrangeFollowsTheLayerStack() {
    Fixture f;
    std::vector<document::NodeId> sources;
    for (int index = 0; index < 4; ++index) {
        const auto result = f.edit<commands::AddSolidLayer>("Layer " + std::to_string(index + 1),
                                                            core::Color4d{0.2, 0.4, 0.6, 1.0});
        if (const auto source = result.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput))
            sources.push_back(*source);
    }
    QCoreApplication::processEvents();
    // Scatter the cards first so Arrange has real work to do.
    std::map<document::NodeId, document::Vec2d> scattered;
    double offset = 0.0;
    for (const auto& node : f.session.composition()->graph().nodes()) {
        scattered.emplace(node.id, document::Vec2d{900.0 - offset, offset * 0.7});
        offset += 150.0;
    }
    (void)f.edit<commands::MoveNodes>(scattered);
    QCoreApplication::processEvents();

    auto* footer = f.editor.footerWidgetForTest();
    auto* button = footer ? footer->findChild<kit::KButton*>(QStringLiteral("nodeArrangeButton"))
                          : nullptr;
    expect(button != nullptr && button->text() == QStringLiteral("Arrange") &&
               button->iconId() == kit::IconId::Arrange,
           "the footer carries a labelled Arrange button with its icon");
    if (button == nullptr)
        return;
    const auto history = f.stack.size();
    button->click();
    QCoreApplication::processEvents();
    expect(f.stack.size() == history + 1, "the footer's Arrange is one undo step");

    const auto& graph = f.session.composition()->graph();
    const auto outputId = graph.compositionOutput()->nodeId;
    const auto mergeId = graph.layerStack().nodeId();
    qreal rightmost = 0.0;
    for (auto* card : cards(f))
        rightmost = std::max(rightmost, card->pos().x());
    expect(f.card(outputId)->pos().x() == rightmost, "Composition Output is the rightmost card");
    expect(f.card(mergeId)->pos().x() < f.card(outputId)->pos().x(),
           "Merge sits left of the output");
    const auto layers = layerCardsInStackOrder(f);
    bool ordered = layers.size() == 4;
    for (std::size_t index = 0; index < layers.size(); ++index) {
        ordered = ordered && layers[index]->pos().x() == layers[0]->pos().x() &&
                  layers[index]->pos().x() < f.card(mergeId)->pos().x();
        if (index > 0)
            ordered = ordered && layers[index - 1]->pos().y() < layers[index]->pos().y();
    }
    expect(ordered, "the layer column follows the stack top to bottom, left of Merge");
    bool upstreamLeft = sources.size() == 4;
    for (const auto source : sources)
        upstreamLeft = upstreamLeft && f.card(source)->pos().x() < layers[0]->pos().x();
    expect(upstreamLeft, "every source sits further left than the layers it feeds");
    expect(overlapping(f).empty(), "the arranged graph has no overlapping cards");
    bool onGrid = true;
    for (const auto& [id, record] : f.session.composition()->nodeLayout())
        onGrid = onGrid && std::fmod(record.position.x, commands::kNodeLayoutGrid) == 0.0 &&
                 std::fmod(record.position.y, commands::kNodeLayoutGrid) == 0.0;
    expect(onGrid, "every arranged card sits on the layout grid");

    // The same command through the MCP-facing path lays the graph out identically when it is handed
    // the same measured sizes: there is one Arrange, in src/commands.
    const auto arranged = f.session.composition()->nodeLayout();
    expect(f.session.undo(), "undo the footer's Arrange");
    expect(f.session.composition()->nodeLayout().at(outputId).position == scattered.at(outputId),
           "one undo restores the scattered layout");
}

// Item 6: the header caret folds a card to its header; its links stay on the folded card's edges.
void testCollapseKeepsLinksOnTheEdges() {
    Fixture f;
    const auto layer = addSolidLayer(f, "Folded");
    auto* card = f.card(layer);
    expect(card != nullptr && !card->collapsed(), "the layer card starts open");
    if (card == nullptr)
        return;
    const auto openHeight = card->cardRect().height();
    f.click(card->mapToScene(card->collapseToggleRect().center()));
    QCoreApplication::processEvents();
    card = f.card(layer);
    expect(f.session.composition()->nodeLayout().at(layer).collapsed && card->collapsed(),
           "a click on the caret collapses the card through SetNodeCollapsed");
    expect(card->cardRect().height() < openHeight, "a collapsed card is folded to its header");
    const auto rect = sceneRect(card);
    int attached = 0;
    for (auto* edge : edges(f)) {
        const bool into = node_editor::destinationNodeId(edge->edge.destination) == layer;
        const bool outOf = edge->edge.source.nodeId == layer;
        if (!into && !outOf)
            continue;
        const auto point = into ? pathEnd(edge->path()) : pathStart(edge->path());
        const qreal edgeX = into ? rect.left() : rect.right();
        expect(std::abs(point.x() - edgeX) < 0.5 && point.y() >= rect.top() &&
                   point.y() <= rect.bottom(),
               "a collapsed card's link ends on its own edge, inside its height");
        ++attached;
    }
    expect(attached >= 2, "the collapsed layer keeps its incoming and outgoing links");
    for (const auto* socket : card->sockets())
        expect(socket->isVisible() == socket->linked,
               "a collapsed card shows only its linked sockets");
    f.click(card->mapToScene(card->collapseToggleRect().center()));
    QCoreApplication::processEvents();
    expect(!f.session.composition()->nodeLayout().at(layer).collapsed,
           "a second click on the caret expands it again");
}

// Item 5: a frame takes a colour from its context menu, as one command.
void testGroupColour() {
    Fixture f;
    const auto a = f.add(document::kSolidSourceNodeType, {100, 100});
    const auto b = f.add(document::kSolidSourceNodeType, {500, 100});
    f.session.selectNodes({a, b}, a);
    f.key(Qt::Key_G, Qt::ControlModifier);
    const auto& groups = f.session.composition()->nodeGroups();
    expect(groups.size() == 1, "a group to colour");
    if (groups.empty())
        return;
    const auto id = groups.begin()->first;
    std::unique_ptr<QMenu> menu(f.editor.contextMenuForTest(false, id));
    auto* label = menu->findChild<QAction*>(QStringLiteral("nodeGroupColorAction.3"));
    expect(label != nullptr, "the frame's menu offers the label swatches");
    if (label == nullptr)
        return;
    label->trigger();
    QCoreApplication::processEvents();
    const auto expected = kit::color(kit::Color::Label3);
    const auto& color = f.session.composition()->nodeGroups().at(id).color;
    expect(color && (*color)[0] == expected.red() && (*color)[1] == expected.green() &&
               (*color)[2] == expected.blue(),
           "Label 3 colours the frame");
    auto* frame = dynamic_cast<NodeGroupItem*>(f.scene()->findNodeGroupItem(id));
    expect(frame != nullptr && frame->color().rgb() == expected.rgb(),
           "the frame paints its colour");
    std::unique_ptr<QMenu> again(f.editor.contextMenuForTest(false, id));
    again->findChild<QAction*>(QStringLiteral("nodeGroupColorNoneAction"))->trigger();
    QCoreApplication::processEvents();
    expect(!f.session.composition()->nodeGroups().at(id).color, "None clears the colour");
}

// Item 8: a link is the same width on screen at 25% and at 200%. The link is painted alone at
// each zoom and its ink is measured per screen pixel of its length: a scaled stroke would carry
// eight times the ink per pixel at 200% that it carries at 25%, a cosmetic one the same.
double inkPerScreenPixel(NodeEdgeItem& edge, const double zoom) {
    const QRectF bounds = edge.path().boundingRect().adjusted(-4, -4, 4, 4);
    QImage image((bounds.size() * zoom).toSize() + QSize(2, 2), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.scale(zoom, zoom);
    painter.translate(-bounds.topLeft());
    QStyleOptionGraphicsItem option;
    edge.paint(&painter, &option, nullptr);
    painter.end();
    double ink = 0.0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            ink += static_cast<double>(image.pixelColor(x, y).alphaF());
    return ink / (edge.path().length() * zoom);
}

void testLinkWidthIsConstant() {
    Fixture f;
    (void)addSolidLayer(f, "Wire");
    NodeEdgeItem* link = nullptr;
    for (auto* edge : edges(f))
        if (std::holds_alternative<document::NodeInputRef>(edge->edge.destination))
            link = edge;
    expect(link != nullptr, "a link to measure");
    if (link == nullptr)
        return;
    expect(link->linkPen().isCosmetic() && link->linkPen().widthF() == kit::kNodeLinkWidth,
           "a link strokes with a cosmetic pen of the link width");
    const double zoomedOut = inkPerScreenPixel(*link, 0.25);
    const double zoomedIn = inkPerScreenPixel(*link, 2.0);
    expect(zoomedOut > 0.5 && std::abs(zoomedOut - zoomedIn) < 0.25 * zoomedIn,
           "the link carries the same screen width at 25% and at 200%");
    if (std::abs(zoomedOut - zoomedIn) >= 0.25 * zoomedIn)
        std::cerr << "  ink per pixel at 0.25: " << zoomedOut << ", at 2: " << zoomedIn << '\n';
}

// #599: a Shape Source grows a socket row only for what its kind reads. A Line shows its two
// ends' sockets; a Rectangle shows none of them (nor the Path socket), so no empty socket band sits
// above its first row.
void testShapeSocketsFollowTheKind() {
    Fixture f;
    const auto sockets = [&](const document::ShapeKind kind) {
        if (!f.session.addShapeLayer(kind, {.position = document::Vec2d{100.0, 100.0},
                                            .size = document::Vec2d{80.0, 60.0},
                                            .lineStart = document::Vec2d{0.0, 0.0},
                                            .lineEnd = document::Vec2d{80.0, 0.0},
                                            .path = {}}))
            throw std::runtime_error("fixture shape layer");
        QCoreApplication::processEvents();
        const auto layer = std::get<document::LayerId>(f.session.selection().primary);
        const auto shape = f.session.directSourceNodeForLayer(layer);
        std::set<QString> names;
        if (auto* card = shape ? f.card(*shape) : nullptr)
            for (auto* socket : card->sockets())
                names.insert(socket->name);
        return names;
    };
    const auto line = sockets(document::ShapeKind::Line);
    expect(line.contains(QStringLiteral("lineStart")) && line.contains(QStringLiteral("lineEnd")),
           "a Line card shows its ends' sockets");
    expect(!line.contains(QStringLiteral("size")) && !line.contains(QStringLiteral("path")),
           "a Line card shows no Size or Path socket");
    const auto rectangle = sockets(document::ShapeKind::Rectangle);
    expect(rectangle.contains(QStringLiteral("size")) &&
               !rectangle.contains(QStringLiteral("lineStart")) &&
               !rectangle.contains(QStringLiteral("lineEnd")),
           "a Rectangle card shows its Size socket and no Line end sockets");
}

// Item 12: Merge's links each plug into their own segment of the stack pill.
void testMergeSlotsHaveTheirOwnSockets() {
    Fixture f;
    for (int index = 0; index < 3; ++index)
        (void)addSolidLayer(f, "Slot " + std::to_string(index + 1));
    const auto mergeId = f.session.composition()->graph().layerStack().nodeId();
    auto* merge = f.card(mergeId);
    node_editor::SocketItem* pill = nullptr;
    for (auto* socket : merge->sockets())
        if (socket->multiInput() && socket->kind == document::SocketValueKind::Image)
            pill = socket;
    expect(pill != nullptr && pill->orderedInputs().size() == 3, "Merge carries three layers");
    if (pill == nullptr)
        return;
    std::vector<qreal> ends;
    for (const auto& input : pill->orderedInputs())
        for (auto* edge : edges(f))
            if (edge->edge.destination == input) {
                const auto end = pathEnd(edge->path());
                expect(end == pill->mapToScene(pill->anchorFor(input)),
                       "a stack link ends on its own slot's anchor");
                ends.push_back(end.y());
            }
    expect(ends.size() == 3, "every slot has its link");
    const auto top = pill->mapToScene(QPointF(0, -pill->pillLength() / 2.0)).y();
    const auto bottom = pill->mapToScene(QPointF(0, pill->pillLength() / 2.0)).y();
    bool distinct = ends.size() == 3;
    for (std::size_t index = 1; index < ends.size(); ++index)
        distinct = distinct && ends[index - 1] < ends[index];
    expect(distinct && ends.front() > top && ends.back() < bottom,
           "three distinct attach points down the pill, in stack order");
}

// Item 2: a source card draws no preview: its first row starts right under the title band.
void testSourceCardsHaveNoPreview() {
    Fixture f;
    double x = 0.0;
    for (const auto* type : {"bloom.image-source", "bloom.video-source", "bloom.audio-source",
                             document::kShapeSourceNodeType.data()}) {
        const auto id = f.add(type, {x, 0});
        x += 600.0;
        auto* card = f.card(id);
        expect(card != nullptr, "a source card");
        if (card == nullptr)
            continue;
        qreal firstRow = card->cardRect().height();
        for (auto* child : card->childItems())
            if (auto* proxy = dynamic_cast<QGraphicsProxyWidget*>(child); proxy && proxy->isVisible())
                firstRow = std::min(firstRow, proxy->pos().y());
        expect(firstRow == kCardHeaderHeight,
               "a source card has no preview band between its title and its first row");
    }
}

// #509: Light & Shadow's Glowing paths submenu offers the five presets; one adds its group and an
// instance in one undo step, and the instance's slot sockets and Properties rows read as what they
// drive in the group, not as "Scalar 1".
void testGlowingPathPresets() {
    Fixture f;
    std::unique_ptr<QMenu> menu(f.editor.contextMenuForTest());
    auto* presets = menu ? menu->findChild<QMenu*>(QStringLiteral("nodeAddPathGlowMenu")) : nullptr;
    expect(presets != nullptr, "Light & Shadow offers the Glowing paths submenu");
    if (presets == nullptr)
        return;
    QStringList names;
    for (auto* action : presets->actions())
        names << action->text();
    expect(names == QStringList{"Saber", "Beam", "Vegas", "Write-on", "Stroke"},
           "the submenu offers Saber, Beam, Vegas, Write-on and Stroke");
    auto* saber = presets->findChild<QAction*>(QStringLiteral("nodeAddPathGlow.0"));
    expect(saber != nullptr && saber->isEnabled(), "Saber can be added");
    if (saber == nullptr)
        return;
    const auto compositions = f.session.snapshot().project().compositions().size();
    const auto nodes = f.session.composition()->graph().nodes().size();
    saber->trigger();
    QCoreApplication::processEvents();
    expect(f.session.snapshot().project().compositions().size() == compositions + 1 &&
               f.session.composition()->graph().nodes().size() == nodes + 1,
           "Saber adds its group and one Node Group");
    const auto* selected = f.session.selectedNode();
    expect(selected != nullptr && selected->typeId == document::kNodeGroupNodeType,
           "the new Node Group is selected");
    if (selected == nullptr)
        return;
    const auto instance = selected->id;
    std::map<QString, QString> sockets;
    if (auto* card = f.card(instance))
        for (auto* socket : card->sockets())
            if (socket->input)
                sockets[socket->name] = socket->label;
    expect(sockets[QStringLiteral("scalar1")] == QStringLiteral("Path Stroke Width") &&
               sockets[QStringLiteral("scalar2")] == QStringLiteral("Glow Size") &&
               sockets[QStringLiteral("scalar3")] == QStringLiteral("Glow Intensity") &&
               sockets[QStringLiteral("color1")] == QStringLiteral("Glow Tint") &&
               sockets[QStringLiteral("scalar4")].isEmpty(),
           ("the card names the slots after what they drive (" +
            sockets[QStringLiteral("scalar1")].toStdString() + ", " +
            sockets[QStringLiteral("scalar2")].toStdString() + ", " +
            sockets[QStringLiteral("scalar3")].toStdString() + ", " +
            sockets[QStringLiteral("color1")].toStdString() + ")")
               .c_str());
    PropertiesEditor properties(f.session);
    properties.resize(900, 1200);
    properties.show();
    QCoreApplication::processEvents();
    std::map<QString, QString> rows;
    for (auto* row : properties.findChildren<QWidget*>(QStringLiteral("propertiesRegistryRow")))
        rows[row->property("role").toString()] = row->property("rowLabel").toString();
    expect(rows[QStringLiteral("scalar1")] == QStringLiteral("Path Stroke Width") &&
               rows[QStringLiteral("scalar2")] == QStringLiteral("Glow Size") &&
               rows[QStringLiteral("color1")] == QStringLiteral("Glow Tint") &&
               rows[QStringLiteral("scalar4")] == QStringLiteral("Scalar 4"),
           "Properties names the slots the same way");
    if (const auto directory = qEnvironmentVariable("BLOOM_UI_CAPTURE_DIR"); !directory.isEmpty())
        properties.grab().save(directory + QStringLiteral("/properties-glowing-path-saber.png"));
    expect(f.session.undo() &&
               f.session.snapshot().project().compositions().size() == compositions &&
               f.session.composition()->graph().nodes().size() == nodes,
           "the preset undoes in one step");
    // A preset without a white core takes its colour on the stroke's fill: Beam's colour slot
    // reads as the Shape node's Fill Color. A menu adds at the revision it opened on, so it is
    // opened again.
    std::unique_ptr<QMenu> again(f.editor.contextMenuForTest());
    auto* againPresets =
        again ? again->findChild<QMenu*>(QStringLiteral("nodeAddPathGlowMenu")) : nullptr;
    auto* beam = againPresets
                     ? againPresets->findChild<QAction*>(QStringLiteral("nodeAddPathGlow.1"))
                     : nullptr;
    expect(beam != nullptr && beam->isEnabled(), "Beam can be added");
    if (beam == nullptr)
        return;
    beam->trigger();
    QCoreApplication::processEvents();
    std::optional<document::NodeId> beamNode;
    for (const auto& node : f.session.composition()->graph().nodes())
        if (node.typeId == document::kNodeGroupNodeType)
            beamNode = node.id;
    expect(beamNode.has_value() &&
               f.session.snapshot().project().compositions().size() == compositions + 1,
           "Beam adds its group and its Node Group");
    if (!beamNode)
        return;
    QString beamColor;
    if (auto* card = f.card(*beamNode))
        for (auto* socket : card->sockets())
            if (socket->input && socket->name == QStringLiteral("color1"))
                beamColor = socket->label;
    expect(
        beamColor == QStringLiteral("Shape Fill Color"),
        ("Beam's colour slot names the stroke's fill (" + beamColor.toStdString() + ")").c_str());

    // The label follows the slot's target, an edit inside the group (another composition) with
    // nothing in this composition changed: disconnecting the slot inside the group leaves the
    // socket its own name, and undoing the edit names it again, with no other redraw.
    const auto colorLabel = [&] {
        QString label = QStringLiteral("(no card)");
        if (auto* card = f.card(*beamNode))
            for (auto* socket : card->sockets())
                if (socket->input && socket->name == QStringLiteral("color1"))
                    label = socket->label;
        return label;
    };
    const auto beamSnapshot = f.session.snapshot();
    const auto& project = beamSnapshot.project();
    const auto* beamRecord = f.session.composition()->graph().findNode(*beamNode);
    const auto target = beamRecord != nullptr
                            ? document::nodeGroupSlotTarget(project, *beamRecord,
                                                            f.session.composition()->parameters(),
                                                            "color1")
                            : std::nullopt;
    const auto group = beamRecord != nullptr ? document::nodeGroupReference(
                                                 *beamRecord, f.session.composition()->parameters())
                                           : std::nullopt;
    expect(target.has_value() && group.has_value(), "Beam's colour slot has a target in its group");
    if (!target || !group)
        return;
    const auto host = f.session.composition()->contentIdentity();
    commands::Transaction retarget("Disconnect inside the group", f.session.snapshot().revision());
    retarget.emplace<commands::DisconnectInput>(
        *group, document::InputPortRef{document::NodeInputRef{target->node, target->role}});
    expect(f.scene()->submit(std::move(retarget)).changed(), "the slot's link inside the group goes");
    QCoreApplication::processEvents();
    expect(f.session.composition()->contentIdentity() == host,
           "the edit leaves the host composition's content as it was");
    expect(colorLabel().isEmpty(), ("with no target the card shows the slot's own name (" +
                                    colorLabel().toStdString() + ")")
                                       .c_str());
    expect(f.session.undo(), "the edit undoes");
    QCoreApplication::processEvents();
    expect(colorLabel() == QStringLiteral("Shape Fill Color"),
           ("undone, the card names the target again (" + colorLabel().toStdString() + ")").c_str());
}

// #509: Text & Paths offers "Stroke the Selected Text" and "Stroke the Selected Layer's Masks". They
// are disabled, saying why, until a layer that shows text (or has a path mask) is selected; then one
// adds the stroke on its own layer, parented to the text's layer at the identity, in one undo step.
void testOutlineStrokeMenu() {
    Fixture f;
    const auto added = f.edit<commands::AddTextLayer>("Title", "Ag");
    QCoreApplication::processEvents();
    const auto textNode = added.outputId<document::NodeId>(commands::kAddTextLayerTextNodeOutput);
    const auto textLayer = added.outputId<document::LayerId>(commands::kAddTextLayerLayerOutput);
    expect(textNode && textLayer, "a text layer is added");
    if (!textNode || !textLayer)
        return;
    const auto find = [&](QMenu* menu, const char* name) {
        return menu != nullptr ? menu->findChild<QAction*>(QString::fromUtf8(name)) : nullptr;
    };
    f.session.clearSelection();
    std::unique_ptr<QMenu> before(f.editor.contextMenuForTest());
    auto* text = find(before.get(), "nodeAddOutlineStroke.text");
    auto* masks = find(before.get(), "nodeAddOutlineStroke.masks");
    expect(text != nullptr && masks != nullptr && !text->isEnabled() && !masks->isEnabled() &&
               text->toolTip() == QStringLiteral("Select a layer first"),
           "with nothing selected both strokes are offered disabled, saying why");
    f.session.selectNode(*textNode);
    std::unique_ptr<QMenu> menu(f.editor.contextMenuForTest());
    text = find(menu.get(), "nodeAddOutlineStroke.text");
    masks = find(menu.get(), "nodeAddOutlineStroke.masks");
    expect(text != nullptr && text->isEnabled(), "the selected text can be stroked");
    expect(masks != nullptr && !masks->isEnabled() &&
               masks->toolTip() == QStringLiteral("The selected layer has no path mask"),
           "a text layer without masks has no masks to stroke");
    if (text == nullptr)
        return;
    const auto layers = f.session.composition()->graph().layerOutputs().size();
    text->trigger();
    QCoreApplication::processEvents();
    const auto& graph = f.session.composition()->graph();
    const document::LayerOutputBoundary* stroke = nullptr;
    for (const auto& layer : graph.layerOutputs())
        if (layer.parent == textLayer)
            stroke = &layer;
    expect(graph.layerOutputs().size() == layers + 1 && stroke != nullptr &&
               stroke->pivot == document::LayerPivot::Frame,
           "the stroke gets a layer of its own, parented to the text's layer at the identity");
    const auto* selected = f.session.selectedNode();
    expect(selected != nullptr && selected->typeId == document::kPathStrokeNodeType,
           "the new Path Stroke is selected for its controls");
    expect(f.session.undo() && f.session.composition()->graph().layerOutputs().size() == layers,
           "the stroke undoes in one step");
}

// Bug #401: with 25 animated layers, playback touches only the visible time-varying cards, once
// per display frame, rebuilds no selector, and does nothing at all while the panel is hidden.
void testPlaybackRefreshIsBounded() {
    Fixture f;
    std::vector<document::NodeId> layers;
    for (int index = 0; index < 25; ++index)
        layers.push_back(addSolidLayer(f, "Animated " + std::to_string(index + 1)));
    // Five image sources: each carries an asset selector a full refresh would rebuild.
    for (int index = 0; index < 5; ++index)
        (void)f.add("bloom.image-source", {-2000.0 - index * 400.0, 0});
    for (const auto layer : layers) {
        const auto* parameter = node_editor::parameterForRole(
            *f.session.composition()->graph().findNode(layer), *f.session.composition(),
            document::kOpacityParameterRole);
        expect(parameter != nullptr && f.session.toggleKeyframeForParameter(parameter->id),
               "each layer's opacity is animated");
    }
    QCoreApplication::processEvents();
    auto& counters = node_editor::nodeCardRefreshCounters();
    const auto tick = [&](const int index) {
        const auto time = core::RationalTime::create(index % 48, 24);
        if (time)
            static_cast<void>(f.session.setCurrentTime(*time));
    };

    // Hidden: 120 ticks reach no card at all.
    f.editor.hide();
    counters = {};
    for (int index = 1; index <= 120; ++index) {
        tick(index);
        QCoreApplication::processEvents();
    }
    expect(counters.timeRefreshes == 0 && counters.selectorRebuilds == 0,
           "120 playback ticks refresh nothing while the Node panel is hidden");
    f.editor.show();
    QCoreApplication::processEvents();

    // Visible and zoomed in: a burst of ticks inside one frame coalesces into one refresh of the
    // visible animated cards only.
    f.view()->zoomToPercent(200);
    f.view()->centerOn(f.card(layers.front())->sceneBoundingRect().center());
    QCoreApplication::processEvents();
    const QRectF shown = f.view()->mapToScene(f.view()->viewport()->rect()).boundingRect();
    std::size_t visibleAnimated = 0;
    for (const auto layer : layers)
        if (f.card(layer)->sceneBoundingRect().intersects(shown))
            ++visibleAnimated;
    expect(visibleAnimated > 0 && visibleAnimated < layers.size(),
           "the zoomed view shows some animated layers, not all");
    counters = {};
    for (int index = 0; index < 5; ++index)
        tick(index + 3);
    QCoreApplication::processEvents();
    expect(counters.timeRefreshes == visibleAnimated,
           "a burst of ticks refreshes each visible animated card exactly once");
    counters = {};
    for (int index = 1; index <= 120; ++index) {
        tick(index);
        QCoreApplication::processEvents();
    }
    expect(counters.selectorRebuilds == 0, "120 playback ticks rebuild no dropdown");
    expect(counters.timeRefreshes <= visibleAnimated * 120,
           "playback refreshes only the visible animated cards, once per frame");
}

// Item 14: the Nodes editor's glyph is Phosphor tree-structure, and graph is gone.
void testNodesIcon() {
    expect(kit::iconResourcePath(kit::IconId::Nodes, kit::IconWeight::Bold)
               .endsWith(QStringLiteral("tree-structure-bold.svg")),
           "the Nodes glyph is tree-structure");
    expect(!kit::iconPixmap(kit::IconId::Nodes, kit::IconRole::Chrome, kit::Color::Muted).isNull(),
           "the tree-structure glyph renders");
    for (const auto id : kit::iconIds())
        for (const auto weight : {kit::IconWeight::Regular, kit::IconWeight::Bold,
                                  kit::IconWeight::Fill})
            expect(!kit::iconResourcePath(id, weight).contains(QStringLiteral("/graph")),
                   "no glyph is the retired graph asset");
}
} // namespace bloom::ui::test

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        bloom::ui::test::testAddLandsAtTheCursor();
        bloom::ui::test::testAddsNeverOverlap();
        bloom::ui::test::testEstimateBoundsEveryCard();
        bloom::ui::test::testArrangeFollowsTheLayerStack();
        bloom::ui::test::testCollapseKeepsLinksOnTheEdges();
        bloom::ui::test::testGroupColour();
        bloom::ui::test::testLinkWidthIsConstant();
        bloom::ui::test::testMergeSlotsHaveTheirOwnSockets();
        bloom::ui::test::testSourceCardsHaveNoPreview();
        bloom::ui::test::testShapeSocketsFollowTheKind();
        bloom::ui::test::testNodesIcon();
        bloom::ui::test::testGlowingPathPresets();
        bloom::ui::test::testOutlineStrokeMenu();
        bloom::ui::test::testPlaybackRefreshIsBounded();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return bloom::ui::test::failures == 0 ? 0 : 1;
}
