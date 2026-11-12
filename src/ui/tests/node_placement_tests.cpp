// #703: Add Node places by cursor. Choosing a node shows a translucent card that follows the
// pointer and creates nothing; Escape or a right-click cancels it; a click on empty canvas places it
// unconnected; a click on a link it fits inserts it between the two nodes through its best matching
// sockets; every add is one undo step.
#include "node_production_harness.hpp"

#include <QAction>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QMenu>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::production_test;

namespace {
[[nodiscard]] node_editor::NodeEdgeItem* linkTo(App& a, const document::InputPortRef& destination) {
    for (auto* item : a.editor.graphScene()->items())
        if (auto* link = dynamic_cast<node_editor::NodeEdgeItem*>(item);
            link != nullptr && link->edge.destination == destination)
            return link;
    return nullptr;
}

[[nodiscard]] std::optional<document::OutputPortRef> feeding(App& a,
                                                             const document::InputPortRef& input) {
    for (const auto& edge : a.session.composition()->graph().edges())
        if (edge.destination == input)
            return edge.source;
    return std::nullopt;
}

// The Add menu's entry named `text`, at any depth of its categories.
[[nodiscard]] QAction* findIn(QMenu* menu, const QString& text) {
    for (auto* action : menu->actions()) {
        if (action->menu() != nullptr) {
            if (auto* found = findIn(action->menu(), text))
                return found;
        } else if (action->text() == text) {
            return action;
        }
    }
    return nullptr;
}

[[nodiscard]] QAction* addAction(App& a, const QString& text) {
    auto* menu = a.editor.headerMenuForTest("add");
    return menu == nullptr ? nullptr : findIn(menu, text);
}

// Pointer over the canvas without a button, as the hover that drives the preview.
void hover(App& a, const QPointF point) {
    a.mouse(QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
    QCoreApplication::processEvents();
}

void rightClick(App& a, const QPointF point) {
    a.mouse(QEvent::MouseButtonPress, point, Qt::RightButton, Qt::RightButton);
    a.mouse(QEvent::MouseButtonRelease, point, Qt::RightButton, Qt::NoButton);
    auto* view = a.editor.graphView();
    const QPoint at = view->mapFromScene(point);
    QContextMenuEvent menu(QContextMenuEvent::Mouse, at, view->viewport()->mapToGlobal(at));
    QCoreApplication::sendEvent(view->viewport(), &menu);
    QCoreApplication::processEvents();
}

[[nodiscard]] QPointF titleCenter(const QGraphicsItem& preview) {
    return preview.pos() +
           QPointF(node_editor::kCardMinimumWidth / 2.0, node_editor::kCardHeaderHeight / 2.0);
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        App a;
        auto* scene = a.editor.graphScene();
        const auto solid = a.addByType(document::kSolidSourceNodeType, {-600, 0});
        const auto layer = a.addByType(document::kLayerOutputNodeType, {200, 0});
        const document::InputPortRef image =
            document::NodeInputRef{layer, std::string(document::kLayerOutputContentInputPort)};
        a.drag(a.outputSocket(solid)->scenePos(),
               a.namedSocket(layer, QString::fromUtf8(document::kLayerOutputContentInputPort), true)
                   ->scenePos());
        QCoreApplication::processEvents();
        const auto original = feeding(a, image);
        expect(original.has_value(), "the fixture has a link from the solid into the layer");
        auto* blur = addAction(a, QStringLiteral("Gaussian Blur"));
        expect(blur != nullptr, "Add offers Gaussian Blur");
        if (blur == nullptr || !original)
            return 1;

        // --- Choosing a node creates nothing yet; the translucent card follows the pointer. ---
        const auto revision = a.session.snapshot().revision();
        const auto nodes = a.session.composition()->graph().nodes().size();
        blur->trigger();
        QCoreApplication::processEvents();
        auto* preview = scene->placementPreviewForTest();
        expect(scene->placementActive() && preview != nullptr && preview->isVisible(),
               "choosing a node starts its placement with a visible preview card");
        expect(preview != nullptr && preview->opacity() < 1.0,
               "the preview card is translucent");
        expect(a.session.snapshot().revision() == revision &&
                   a.session.composition()->graph().nodes().size() == nodes,
               "choosing a node does not change the document");
        // The Add surface takes focus and gives it back as it closes; that does not end it.
        QFocusEvent focusOut(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
        QCoreApplication::sendEvent(a.editor.graphView(), &focusOut);
        expect(scene->placementActive(), "a focus change does not end the placement");
        const QPointF empty(-400, 700);
        hover(a, empty);
        // The pointer is a whole viewport pixel, so the point it maps back to is within a pixel.
        expect(preview != nullptr &&
                   QLineF(titleCenter(*preview), empty).length() <= 1.0,
               "the preview's title band follows the pointer");
        expect(!scene->placementTargetForTest().has_value(),
               "over empty canvas the preview targets no link");

        // --- Escape cancels. ---
        QTest::keyClick(a.editor.graphView(), Qt::Key_Escape);
        QCoreApplication::processEvents();
        expect(!scene->placementActive() && scene->placementPreviewForTest() == nullptr,
               "Escape cancels the placement and removes the preview");
        expect(a.session.snapshot().revision() == revision &&
                   a.session.composition()->graph().nodes().size() == nodes,
               "a cancelled placement adds nothing");

        // --- A right-click cancels, and opens no menu. ---
        blur->trigger();
        QCoreApplication::processEvents();
        hover(a, empty);
        rightClick(a, empty);
        expect(!scene->placementActive() && scene->placementPreviewForTest() == nullptr,
               "a right-click cancels the placement");
        expect(a.editor.findChild<QMenu*>(QString(), Qt::FindDirectChildrenOnly) == nullptr ||
                   !a.editor.findChild<QMenu*>(QString(), Qt::FindDirectChildrenOnly)->isVisible(),
               "the cancelling right-click opens no context menu");
        expect(a.session.composition()->graph().nodes().size() == nodes,
               "a right-click cancel adds nothing");

        // --- A click on empty canvas places the node there, unconnected, in one undo step. ---
        blur->trigger();
        QCoreApplication::processEvents();
        hover(a, empty);
        const auto origin = scene->placementPreviewForTest()->pos();
        a.press(empty);
        a.release(empty);
        QCoreApplication::processEvents();
        const auto* placed = a.session.selectedNode();
        expect(!scene->placementActive() && placed != nullptr &&
                   placed->typeId == "bloom.gaussian-blur" &&
                   a.session.composition()->graph().nodes().size() == nodes + 1,
               "a click on empty canvas adds the node and selects it");
        if (placed != nullptr) {
            const auto id = placed->id;
            const auto& layout = a.session.composition()->nodeLayout();
            expect(layout.contains(id) &&
                       layout.at(id).position == document::Vec2d{origin.x(), origin.y()},
                   "the card lands where its preview was");
            bool connected = false;
            for (const auto& edge : a.session.composition()->graph().edges())
                connected = connected || edge.source.nodeId == id ||
                            node_editor::destinationNodeId(edge.destination) == id;
            expect(!connected, "a node placed on empty canvas has no connections");
            expect(a.session.undo() && a.session.composition()->graph().nodes().size() == nodes,
                   "one undo removes the placed node");
            QCoreApplication::processEvents();
        }

        // --- Over a link it fits: the link is targeted and a click inserts the node into it. ---
        blur->trigger();
        QCoreApplication::processEvents();
        auto* link = linkTo(a, image);
        expect(link != nullptr, "the link is on the canvas");
        if (link == nullptr)
            return 1;
        const QPointF onLink = link->path().pointAtPercent(0.5);
        hover(a, onLink);
        expect(scene->placementTargetForTest() == image,
               "over a link the node can go into, the preview targets that link");
        a.press(onLink);
        a.release(onLink);
        QCoreApplication::processEvents();
        const auto* inserted = a.session.selectedNode();
        expect(inserted != nullptr && inserted->typeId == "bloom.gaussian-blur",
               "the click on the link adds the node");
        if (inserted != nullptr) {
            const auto id = inserted->id;
            expect(feeding(a, image) == document::OutputPortRef{id, "image"},
                   "the node's image output now feeds the layer");
            expect(feeding(a, document::NodeInputRef{id, "input"}) == original,
                   "the solid now feeds the node's image input, not its mask or a parameter");
            expect(a.session.undo() && feeding(a, image) == original &&
                       a.session.composition()->graph().nodes().size() == nodes,
                   "one undo takes the node out and restores the original link");
            QCoreApplication::processEvents();
        }

        // --- A node with no matching sockets is never targeted at a link. ---
        auto* scalar = addAction(a, QStringLiteral("Scalar"));
        expect(scalar != nullptr, "Add offers Scalar");
        if (scalar != nullptr) {
            scalar->trigger();
            QCoreApplication::processEvents();
            link = linkTo(a, image);
            hover(a, link->path().pointAtPercent(0.5));
            expect(scene->placementActive() && !scene->placementTargetForTest().has_value(),
                   "a node with no input for the link's image does not target it");
            const auto point = link->path().pointAtPercent(0.5);
            a.press(point);
            a.release(point);
            QCoreApplication::processEvents();
            expect(feeding(a, image) == original &&
                       a.session.composition()->graph().nodes().size() == nodes + 1,
                   "it is placed unconnected and the link stays as it was");
        }

        // --- A document change while placing (another edit's publication) does not end it. ---
        blur->trigger();
        QCoreApplication::processEvents();
        hover(a, empty);
        const auto before = a.session.composition()->graph().nodes().size();
        static_cast<void>(a.addByType(document::kSolidSourceNodeType, {-1400, 1400}));
        expect(scene->placementActive() && scene->placementPreviewForTest() != nullptr,
               "a projection of another edit keeps the placement and its preview");
        a.press(empty);
        a.release(empty);
        QCoreApplication::processEvents();
        expect(!scene->placementActive() &&
                   a.session.composition()->graph().nodes().size() == before + 2,
               "and the click still places the node");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " placement check(s) failed\n";
        return 1;
    }
    std::cout << "PASS: node placement\n";
    return 0;
}
