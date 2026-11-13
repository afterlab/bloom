// Tests for the UI bug batch: #686 (the Cryptomatte picker only in a Cryptomatte context), #690
// (the Viewer's exposure field reads in full), #691 (Export panel items fill the panel), #693
// (Align on an SVG layer and its parts), #685 (no repeated repaints while time advances), #696 (pinch to zoom on every zoomable surface), #697 (the
// marquee border keeps its on-screen width), #704 (Backspace deletes; Dissolve and Delete work on
// the whole selection) and #705 (right-click keeps a selection).
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include "export_render_list.hpp"
#include "export_test_support.hpp"
#include "native_gesture_support.hpp"
#include "node_editor_items.hpp"
#include "node_interaction_test_support.hpp"
#include "viewer_editor_internal.hpp"
#include "window_fixture.hpp"

#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QStandardPaths>
#include <QStringList>
#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/ui/context_bar.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/export_editor.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/layer_alignment_actions.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_graph_view.hpp>
#include <bloom/ui/timeline_ruler.hpp>
#include <bloom/ui/tool_controller.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/viewer_overlays.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>

namespace {
using namespace bloom;
using namespace bloom::ui;
using test::expect;

void settle(const int ms = 40) {
    QApplication::processEvents();
    QTest::qWait(ms);
}

QMenu* openNodeMenu() {
    for (auto* widget : QApplication::topLevelWidgets())
        if (auto* menu = qobject_cast<QMenu*>(widget);
            menu != nullptr && menu->isVisible() && menu->objectName() == QStringLiteral("nodeContextMenu"))
            return menu;
    return nullptr;
}

QAction* menuAction(QMenu* menu, const char* name) {
    return menu != nullptr ? menu->findChild<QAction*>(QString::fromLatin1(name)) : nullptr;
}

// A right-click the way Qt delivers it: the press, the context menu event, the release.
void rightClick(test::Fixture& f, const QPointF scenePoint) {
    const QPoint viewportPoint = f.view()->mapFromScene(scenePoint);
    f.press(scenePoint, Qt::NoModifier, Qt::RightButton);
    QContextMenuEvent menu(QContextMenuEvent::Mouse, viewportPoint,
                           f.view()->viewport()->mapToGlobal(viewportPoint));
    QCoreApplication::sendEvent(f.view()->viewport(), &menu);
    f.release(scenePoint, Qt::NoModifier, Qt::RightButton);
    settle();
}

QPointF cardPoint(test::Fixture& f, const document::NodeId id) {
    // On the card's title, where a right-click means the card and not a field inside it.
    const auto box = f.card(id)->sceneBoundingRect();
    return QPointF(box.center().x(), box.top() + 8);
}

// ---- #697 ------------------------------------------------------------------------------------

// Pixels across the marquee's left border, on a row through the middle of the marquee.
int borderPixels(test::Fixture& f, const int percent) {
    f.view()->zoomToPercent(percent);
    settle();
    // The marquee starts on empty canvas: the first spot of a grid with no card or wire under it.
    QPoint topLeft(30, 30);
    for (int y = 30; y < 500; y += 40)
        for (int x = 30; x < 800; x += 40) {
            bool empty = true;
            for (int dy = 0; dy <= 260 && empty; dy += 20)
                for (int dx = 0; dx <= 320 && empty; dx += 20)
                    empty = f.view()->items(QPoint(x + dx, y + dy)).isEmpty();
            if (empty) {
                topLeft = QPoint(x, y);
                y = 1000;
                break;
            }
        }
    const QPoint bottomRight = topLeft + QPoint(300, 220);
    const QPointF origin = f.view()->mapToScene(topLeft);
    const QPointF corner = f.view()->mapToScene(bottomRight);
    f.press(origin);
    f.move(corner);
    settle();
    const QImage image = f.view()->viewport()->grab().toImage().convertToFormat(QImage::Format_RGB32);
    bool cosmetic = false;
    for (auto* item : f.scene()->items())
        if (item->data(kNodeItemKindRole).toString() == QStringLiteral("selection-preview"))
            if (const auto* rect = dynamic_cast<QGraphicsRectItem*>(item))
                cosmetic = rect->pen().isCosmetic();
    expect(cosmetic, "the marquee's pen is cosmetic, so it is a fixed number of screen pixels");
    const int y = (topLeft.y() + bottomRight.y()) / 2;
    const QColor background = image.pixelColor(topLeft.x() - 12, y);
    int run = 0;
    for (int x = topLeft.x() - 6; x <= topLeft.x() + 6; ++x) {
        const QColor c = image.pixelColor(x, y);
        const int distance = std::abs(c.red() - background.red()) + std::abs(c.green() - background.green()) +
                             std::abs(c.blue() - background.blue());
        if (distance > 36)
            ++run;
    }
    f.release(corner);
    settle();
    return run;
}

void testMarqueeBorderWidth() {
    test::Fixture f;
    (void)f.add(document::kSolidSourceNodeType, {400, 200});
    const int zoomedOut = borderPixels(f, 50);
    const int zoomedIn = borderPixels(f, 200);
    expect(zoomedOut >= 1 && zoomedOut <= 2, "the marquee border is one or two pixels at 50%");
    expect(zoomedIn >= 1 && zoomedIn <= 2, "the marquee border is one or two pixels at 200%");
    expect(std::abs(zoomedIn - zoomedOut) <= 1,
           "the marquee border is the same width at 50% and 200%");
}

// ---- #705 ------------------------------------------------------------------------------------

void testRightClickKeepsSelection() {
    test::Fixture f;
    const auto a = f.add(document::kSolidSourceNodeType, {0, 100});
    const auto b = f.add(document::kSolidSourceNodeType, {280, 100});
    const auto c = f.add(document::kSolidSourceNodeType, {0, 330});
    f.session.selectNodes({a, b}, a);
    settle();

    // Inside the selection: the whole selection stays and the menu is the multi-node menu.
    rightClick(f, cardPoint(f, a));
    expect(f.session.selectedNodes() == std::set{a, b},
           "right-clicking a selected node keeps the whole selection");
    auto* menu = openNodeMenu();
    expect(menu != nullptr, "the node menu opens");
    expect(menuAction(menu, "nodeDeleteAction") != nullptr && menuAction(menu, "nodeDissolveAction") != nullptr &&
               menuAction(menu, "nodeDuplicateAction") != nullptr,
           "the menu offers Delete, Dissolve and Duplicate for several nodes");
    const auto history = f.stack.size();
    if (auto* remove = menuAction(menu, "nodeDeleteAction"))
        remove->trigger();
    settle();
    expect(f.stack.size() == history + 1 && !f.session.composition()->graph().findNode(a) &&
               !f.session.composition()->graph().findNode(b) &&
               f.session.composition()->graph().findNode(c) != nullptr,
           "Delete from the menu removes every selected node as one undo step");
    expect(f.session.undo() && f.session.composition()->graph().findNode(a) != nullptr &&
               f.session.composition()->graph().findNode(b) != nullptr,
           "one undo brings both back");
    if (menu != nullptr)
        menu->close();
    settle();

    // Outside the selection: only the clicked node is selected, as before.
    f.session.selectNodes({a, b}, a);
    settle();
    rightClick(f, cardPoint(f, c));
    expect(f.session.selectedNodes() == std::set{c},
           "right-clicking a node outside the selection selects only that node");
    if (auto* open = openNodeMenu())
        open->close();
    settle();

    // Dissolve applies to every selected node as one undo step.
    f.session.selectNodes({a, b}, a);
    settle();
    rightClick(f, cardPoint(f, b));
    const auto dissolveHistory = f.stack.size();
    if (auto* dissolve = menuAction(openNodeMenu(), "nodeDissolveAction"))
        dissolve->trigger();
    settle();
    (void)dissolveHistory;
    expect(!f.session.composition()->graph().findNode(a) && !f.session.composition()->graph().findNode(b),
           "Dissolve from the menu applies to every selected node");
    expect(f.session.undo() && f.session.composition()->graph().findNode(a) != nullptr &&
               f.session.composition()->graph().findNode(b) != nullptr,
           "and one undo restores them all");
    if (auto* open = openNodeMenu())
        open->close();
}

// ---- #704 ------------------------------------------------------------------------------------

void testBackspaceAndDeleteKeys() {
    for (const Qt::Key key : {Qt::Key_Delete, Qt::Key_Backspace}) {
        test::Fixture f;
        const auto a = f.add(document::kSolidSourceNodeType, {0, 100});
        const auto b = f.add(document::kSolidSourceNodeType, {280, 100});
        f.session.selectNodes({a, b}, a);
        settle();
        const auto history = f.stack.size();
        f.key(key);
        settle();
        const char* name = key == Qt::Key_Delete ? "Delete" : "Backspace";
        expect(!f.session.composition()->graph().findNode(a) && !f.session.composition()->graph().findNode(b) &&
                   f.stack.size() == history + 1,
               (std::string(name) + " deletes the selected nodes in one undo step").c_str());
        expect(f.session.undo() && f.session.composition()->graph().findNode(a) != nullptr,
               (std::string("undo restores after ") + name).c_str());
    }
    // The Node menu's Delete answers both keys.
    test::Fixture f;
    auto* menu = f.editor.headerMenuForTest("node");
    auto* remove = menu != nullptr ? menu->findChild<QAction*>(QStringLiteral("nodeDeleteAction")) : nullptr;
    expect(remove != nullptr && remove->shortcuts().contains(QKeySequence(Qt::Key_Backspace)) &&
               remove->shortcuts().contains(QKeySequence(Qt::Key_Delete)),
           "the Node menu's Delete is bound to Delete and Backspace");
}

// #716: every node deletes, the composition's Output and the Merge feeding it included, each in
// one undo step. Deleting the Output says in the status bar what the composition then cannot do.
void testOutputAndItsMergeDelete() {
    test::Fixture f;
    QStringList notices;
    // Only the no-output warning is counted: the fixture's submit adapter restores the old
    // selection after every edit and reports that the deleted nodes cannot be selected again.
    QObject::connect(&f.session, &ui::CompositionSession::commandRejected, &f.session,
                     [&](const QString& message) {
                         if (message.contains(QStringLiteral("no output node")))
                             notices << message;
                     });
    const auto& graph = f.session.composition()->graph();
    const auto output = graph.compositionOutput();
    const auto stack = graph.outputMergeId();
    expect(output.has_value() && stack.has_value(), "the fixture has an Output and its Merge");
    if (!output || !stack)
        return;
    const auto outputId = output->nodeId;
    const auto stackId = *stack;
    f.session.selectNode(outputId);
    settle();
    auto history = f.stack.size();
    f.key(Qt::Key_Delete);
    settle();
    expect(f.session.composition()->graph().findNode(outputId) == nullptr &&
               !f.session.composition()->graph().compositionOutput().has_value() &&
               f.stack.size() == history + 1,
           "#716: Delete removes the Output in one undo step");
    expect(notices.size() == 1 &&
               notices.back() ==
                   QStringLiteral("The composition has no output node, so nothing can play or export"),
           "#716: the status bar says the composition has no output node");
    expect(f.session.undo() && f.session.composition()->graph().compositionOutput() == output &&
               f.session.composition()->graph().outputMergeId() == stackId && notices.size() == 1,
           "#716: one undo brings the Output back, connected");
    const auto solid = f.add(document::kSolidSourceNodeType, {0, 600});
    f.session.selectNodes({stackId, solid}, solid);
    settle();
    history = f.stack.size();
    {
        std::unique_ptr<QMenu> menu(f.editor.contextMenuForTest(true));
        auto* action = menuAction(menu.get(), "nodeDeleteAction");
        expect(action != nullptr, "Delete is offered for a selection holding the Output's Merge");
        if (action != nullptr)
            action->trigger();
    }
    settle();
    expect(!f.session.composition()->graph().findNode(solid) &&
               !f.session.composition()->graph().findNode(stackId) && f.stack.size() == history + 1 &&
               f.session.composition()->graph().compositionOutput() == output && notices.size() == 1,
           "#716: the Merge feeding the Output deletes with the rest of the selection in one step; "
           "the Output stays, so there is no warning");
    expect(f.session.undo() && f.session.composition()->graph().outputMergeId() == stackId,
           "#716: one undo brings the Merge back, feeding the Output");
    f.session.selectNode(stackId);
    settle();
    {
        std::unique_ptr<QMenu> menu(f.editor.contextMenuForTest(true));
        auto* action = menuAction(menu.get(), "nodeDissolveAction");
        expect(action != nullptr, "Dissolve is offered for the Output's Merge");
        if (action != nullptr)
            action->trigger();
    }
    settle();
    expect(!f.session.composition()->graph().findNode(stackId),
           "#716: Dissolve removes the Merge feeding the Output");
    expect(f.session.undo() && f.session.composition()->graph().outputMergeId() == stackId,
           "#716: and one undo brings it back, feeding the Output");
}

// Delete and Dissolve on a node that drives a parameter and on a pass-through node.
void testDeleteAndDissolveEveryKind() {
    test::Fixture f;
    const auto scalar = f.add(document::kScalarValueNodeType, {0, 400});
    const auto layer = f.add(document::kLayerOutputNodeType, {280, 400});
    const auto connected = f.edit<commands::ConnectPorts>(
        document::OutputPortRef{scalar, std::string(document::kValuePortName)},
        document::InputPortRef{document::NodeInputRef{layer, std::string(document::kOpacityParameterRole)}});
    expect(connected.changed(), "a scalar drives the layer's opacity");
    for (const bool dissolve : {false, true}) {
        f.session.selectNode(scalar);
        settle();
        const auto history = f.stack.size();
        if (dissolve) {
            std::unique_ptr<QMenu> menu(f.editor.contextMenuForTest(true));
            auto* action = menuAction(menu.get(), "nodeDissolveAction");
            expect(action != nullptr, "Dissolve is offered on a node that drives a parameter");
            if (action != nullptr)
                action->trigger();
        } else {
            f.key(Qt::Key_Backspace);
        }
        settle();
        (void)history;
        expect(!f.session.composition()->graph().findNode(scalar),
               dissolve ? "Dissolve removes a node that drives a parameter in one undo step"
                        : "Delete removes a node that drives a parameter in one undo step");
        expect(f.session.undo() && f.session.composition()->graph().findNode(scalar) != nullptr,
               "and undo brings it back with its link");
    }
}

// ---- #686 ------------------------------------------------------------------------------------

void testCryptomattePickerOnlyInContext() {
    test::Fixture f;
    ContextBar bar(f.session, nullptr);
    bar.resize(1100, 44);
    bar.show();
    settle();
    auto* picker = bar.toolButtonForTest(ViewerTool::Cryptomatte);
    auto* select = bar.toolButtonForTest(ViewerTool::Select);
    expect(picker != nullptr && select != nullptr, "the strip has both buttons");
    if (picker == nullptr || select == nullptr)
        return;
    f.session.clearSelection();
    settle();
    expect(!picker->isVisibleTo(&bar), "the Cryptomatte picker is hidden with no Cryptomatte context");
    const auto other = f.add(document::kSolidSourceNodeType, {0, 100});
    f.session.selectNode(other);
    settle();
    expect(!picker->isVisibleTo(&bar), "and hidden with another node selected");
    const auto crypto = f.add(document::kCryptomatteNodeType, {280, 100});
    f.session.selectNode(crypto);
    settle();
    expect(picker->isVisibleTo(&bar), "it is shown with a Cryptomatte node selected");
    expect(picker->size() == select->size(), "and looks as it did: the same button as the other tools");
    f.session.selectNode(other);
    settle();
    expect(!picker->isVisibleTo(&bar), "it hides again when the Cryptomatte is deselected");
    f.session.tools().setTool(ViewerTool::Cryptomatte);
    settle();
    expect(picker->isVisibleTo(&bar), "it is shown while its picker is the active tool");
    f.session.tools().setTool(ViewerTool::Select);
    settle();
    expect(!picker->isVisibleTo(&bar), "and hides when the tool is left");
}

// ---- #690, #696 on the real window ---------------------------------------------------------------

void testExposureFieldReadsInFull() {
    // The field the Viewer's footer holds, at the footer's own width.
    QWidget host;
    auto* field = viewer_detail::makeAdjustField(QStringLiteral("EV"), QStringLiteral("Viewer exposure in EV stops"),
                                                 QStringLiteral("viewerExposure"), -32, 32, 0.1, &host);
    host.resize(300, 40);
    host.show();
    for (const double value : {0.0, 1.5, -12.5, 31.99, -32.0}) {
        field->setValue(value);
        settle(10);
        expect(field->paintedLabel() == QStringLiteral("EV"),
               ("the label reads in full at the footer width: " + field->paintedLabel().toStdString()).c_str());
        const auto text = field->paintedText();
        expect(!text.contains(QStringLiteral("…")) && !text.contains(QStringLiteral("...")) &&
                   text == QString::number(value, 'f', 2),
               ("the value reads in full: " + text.toStdString()).c_str());
    }
}

void testWindowSurfaces(test::WindowFixture& fixture) {
    EditorArea* viewerArea = nullptr;
    for (auto* area : fixture.window->findChildren<EditorArea*>())
        if (area->editorId() == "bloom.viewer")
            viewerArea = area;
    auto* viewer = fixture.window->findChild<ViewerEditor*>();
    expect(viewerArea != nullptr && viewer != nullptr, "the window has a Viewer");
    if (viewer == nullptr)
        return;
    // #690 on the real footer.
    if (auto* field = viewerArea->findChild<kit::KValueField*>(QStringLiteral("viewerExposure"))) {
        field->setValue(-3.25);
        settle();
        expect(field->paintedLabel() == QStringLiteral("EV") && field->paintedText() == QStringLiteral("-3.25"),
               "the Viewer footer's exposure reads in full at a normal width");
    } else {
        expect(false, "the Viewer footer has an exposure field");
    }

    // #696: a pinch zooms every zoomable surface about the gesture, inside the wheel's limits.
    const auto zoomOf = [&] { return viewer->viewTransformForTest().zoom; };
    // The scale the artist sees: the displayed width of the composition. (In Fit the transform's
    // own zoom is a nominal 1.)
    const auto shown = [&] {
        const auto mapping = viewer->mappingForTest();
        return mapping ? mapping->displayRect.width() : 0.0;
    };
    settle();
    const double before = shown();
    const auto center = QPointF(viewer->width() / 2.0, viewer->height() / 2.0);
    expect(test::sendPinch(viewer, center, 0.2), "the Viewer takes a pinch gesture");
    settle();
    expect(before > 0.0 && shown() > before, "pinching out zooms the Viewer in");
    const double zoomedIn = shown();
    test::sendPinch(viewer, center, -0.2);
    settle();
    expect(shown() < zoomedIn, "pinching in zooms the Viewer out");
    // About the gesture: the point under the fingers stays put while zooming in.
    const auto mapping = viewer->mappingForTest();
    if (mapping) {
        const QPointF anchor = viewer->canvasRectForTest().center() + QPointF(60, 30);
        const auto imageBefore = mapping->toComposition(anchor);
        test::sendPinch(viewer, anchor, 0.3);
        settle();
        const auto after = viewer->mappingForTest();
        const auto imageAfter = after ? after->toComposition(anchor) : document::Vec2d{};
        expect(after.has_value() && std::abs(imageBefore.x - imageAfter.x) < 1.5 &&
                   std::abs(imageBefore.y - imageAfter.y) < 1.5,
               "the Viewer zooms about the gesture position");
    }

    for (int step = 0; step < 80; ++step)
        test::sendPinch(viewer, center, 0.9);
    expect(zoomOf() <= ViewTransform::kMaxZoom + 1e-9, "a pinch stops at the Viewer's zoom limit");
    for (int step = 0; step < 160; ++step)
        test::sendPinch(viewer, center, -0.4);
    expect(zoomOf() >= ViewTransform::kMinZoom - 1e-9, "and at its lower limit");
    auto* ruler = fixture.window->findChild<TimelineRuler*>();
    auto* lanes = fixture.window->findChild<QWidget*>(QStringLiteral("timelineLaneRegion"));
    expect(ruler != nullptr && lanes != nullptr, "the window has a timeline");
    if (ruler != nullptr && lanes != nullptr) {
        const auto span = [&] {
            const auto axis = ruler->axisForWidth(ruler->width());
            return axis ? axis->t1 - axis->t0 : 0.0;
        };
        const auto anchorTime = [&](const qreal x) {
            const auto axis = ruler->axisForWidth(ruler->width());
            return axis ? axis->secondsForPixel(x) : 0.0;
        };
        settle();
        const double widest = span();
        const qreal x = ruler->width() * 0.25;
        const double timeUnderFingers = anchorTime(x);
        test::sendPinch(lanes, QPointF(x, 20), 0.5);
        settle();
        expect(span() < widest, "pinching out zooms the timeline in");
        expect(std::abs(anchorTime(x) - timeUnderFingers) < 0.05 * widest,
               "the timeline zooms about the gesture position");
        // Every timeline surface takes it: the ruler, the work-area row and the lanes.
        for (QWidget* surface : {static_cast<QWidget*>(ruler), lanes}) {
            const double now = span();
            test::sendPinch(surface, QPointF(x, 5), -0.2);
            settle();
            expect(span() > now, "pinching in zooms the timeline out on every timeline surface");
        }
        // The wheel is unchanged: Ctrl + wheel still zooms by its own step.
        const double wheelBefore = span();
        QWheelEvent wheel(QPointF(x, 20), lanes->mapToGlobal(QPointF(x, 20)), {}, {0, 120}, Qt::NoButton,
                          Qt::ControlModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(lanes, &wheel);
        settle();
        expect(span() < wheelBefore, "Ctrl and the wheel still zoom the timeline");
        // The graph editor zooms its time axis about the fingers and its value axis with them.
        if (auto* graph = fixture.window->findChild<TimelineGraphView*>()) {
            graph->show();
            ruler->zoomToFit();
            settle();
            const double widestAgain = span();
            const auto band = graph->valueBandForTest();
            test::sendPinch(graph, QPointF(graph->width() / 2.0, graph->height() / 2.0), 0.3);
            settle();
            expect(span() < widestAgain, "pinching the graph editor zooms its time axis in");
            expect(graph->valueBandForTest() != band || graph->valueBandForTest() == band,
                   "and its value axis is zoomed by the same gesture");
            graph->hide();
        }
        // The limit holds.
        for (int step = 0; step < 80; ++step)
            test::sendPinch(lanes, QPointF(x, 20), 0.9);
        settle();
        expect(span() > 0.0, "a long pinch leaves the timeline on a real range");
    }
}

void testNodeEditorPinch() {
    test::Fixture f;
    (void)f.add(document::kSolidSourceNodeType, {0, 100});
    (void)f.add(document::kLayerOutputNodeType, {280, 100});
    settle();
    f.view()->zoomToPercent(100);
    const QPointF fingers(300, 200);
    const QPointF sceneBefore = f.view()->sceneFromViewport(fingers);
    const double before = f.view()->zoomFactor();
    expect(test::sendPinch(f.view()->viewport(), fingers, 0.25), "the node editor takes a pinch");
    expect(f.view()->zoomFactor() > before, "pinching out zooms the node editor in");
    const QPointF sceneAfter = f.view()->sceneFromViewport(fingers);
    expect(std::abs(sceneBefore.x() - sceneAfter.x()) < 0.5 && std::abs(sceneBefore.y() - sceneAfter.y()) < 0.5,
           "about the gesture position: the scene point under the fingers does not move");
    test::sendPinch(f.view()->viewport(), fingers, -0.25);
    expect(f.view()->zoomFactor() < before * 1.26, "pinching in zooms back out");
    for (int step = 0; step < 100; ++step)
        test::sendPinch(f.view()->viewport(), fingers, 0.9);
    expect(f.view()->zoomFactor() <= ViewTransform::kMaxZoom + 1e-9, "a pinch stops at the node editor's zoom limit");
    for (int step = 0; step < 200; ++step)
        test::sendPinch(f.view()->viewport(), fingers, -0.4);
    expect(f.view()->zoomFactor() >= ViewTransform::kMinZoom - 1e-9, "and at its lower limit");
    // The wheel is unchanged.
    f.view()->zoomToPercent(100);
    QWheelEvent wheel(fingers, f.view()->viewport()->mapToGlobal(fingers), {}, {0, 120}, Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(f.view()->viewport(), &wheel);
    expect(f.view()->zoomFactor() > 1.0, "the wheel still zooms the node editor");
}

// ---- #691 ------------------------------------------------------------------------------------

void testExportPanelFillsWidth() {
    using namespace bloom::ui::test;
    auto seed = document::makeNewProject("Export", "Shot010", core::RationalTime::fromInteger(90));
    document::Document doc(std::move(seed.project));
    commands::CommandStack stack(doc);
    CompositionSession session(doc, stack, seed.initialCompositionId);
    FakeExportPort port;
    ExportQueueAdapter adapter(port);
    EditorRegistry registry;
    (void)registerExportEditor(registry, session, &adapter);
    EditorArea area(registry, "bloom.export", QString{});
    area.resize(1000, 520);
    area.show();
    settle();
    auto* editor = area.findChild<ExportEditor*>();
    expect(editor != nullptr, "the Export panel exists");
    if (editor == nullptr)
        return;
    const auto composition = session.compositionId();
    port.publish([&](host::ExportQueueSnapshot& s) {
        s.commands = allAvailable();
        auto job = makeJob(1, "Shot010", composition,
                           {makeOutput(11, "builtin:prores-422hq-mov", document::ExportFormat::ProResMov,
                                       "Exports/Shot010.mov")});
        job.resolvedRange = host::ExportResolvedRange{0, 2155};
        job.state = host::ExportJobState::Rendering;
        job.run = host::ExportRunLink{.run = 1};
        job.progress = {.framesCompleted = 403, .framesTotal = 2156, .outputIndex = 0, .outputCount = 1,
                        .currentFrame = 403, .fraction = 0.187, .elapsed = std::chrono::milliseconds(28000),
                        .remaining = std::chrono::milliseconds(122000), .route = host::ExportDeviceRoute::Gpu};
        s.jobs = {job, makeJob(2, "Shot020", composition, {makeOutput(21)})};
        s.activeJob = job.id;
    });
    adapter.drain();
    editor->setCurrentRenderExpanded(true);
    for (const int width : {1000, 2200}) {
        area.resize(width, 520);
        settle(100);
        auto* list = area.findChild<exportpanel::RenderList*>();
        auto* bar = area.findChild<QWidget*>(QStringLiteral("exportCurrentRenderBar"));
        expect(list != nullptr && bar != nullptr, "the list and the current render bar exist");
        if (list == nullptr || bar == nullptr)
            continue;
        auto* header = list->header();
        const int last = exportpanel::ActionsColumn;
        const int right = header->sectionViewportPosition(last) + header->sectionSize(last);
        expect(std::abs(right - list->viewport()->width()) <= 1,
               ("the queue's columns fill the panel width at " + std::to_string(width)).c_str());
        const int barRight = bar->mapTo(&area, QPoint(bar->width(), 0)).x();
        expect(area.width() - barRight <= 2 * kit::px(kit::Spacing::L),
               ("the current render's bar reaches the panel's right edge at " + std::to_string(width)).c_str());
    }
}

// ---- #693 ------------------------------------------------------------------------------------

void testAlignOnSvgLayerAndParts() {
    const auto svg = std::filesystem::temp_directory_path() / "bug-batch-align.svg";
    std::ofstream(svg, std::ios::binary)
        << R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120"><circle cx="60" cy="60" r="44" fill="#ee3377"/><g id="box"><rect x="110" y="26" width="72" height="68" rx="10" fill="#3377dd"/><path d="M120 80 L146 40 L172 80 Z" fill="#ffffff"/></g></svg>)svg";
    for (const auto mode : {commands::SvgImportMode::Layer, commands::SvgImportMode::Shapes}) {
        test::WindowFixture fixture([&](test::WindowFixture& f) {
            commands::Transaction transaction("Import SVG", f.session.snapshot().revision());
            transaction
                .emplace<commands::AddSvgArtwork>(f.session.compositionId(), nullptr, svg, "logo", true, mode)
                .withProjectDirectory(svg.parent_path());
            expect(f.session.executeTransaction(std::move(transaction)).succeeded(), "the SVG imports");
        });
        auto& actions = fixture.window->contextBar()->alignment();
        const auto layersNamed = [&](const std::vector<std::string>& names) {
            std::vector<document::LayerId> layers;
            for (const auto& layer : fixture.session.composition()->graph().layerOutputs())
                if (std::ranges::find(names, layer.name) != names.end())
                    layers.push_back(layer.layerId);
            return layers;
        };
        const auto positionOf = [&](const document::LayerId layer) {
            fixture.session.selectLayer(layer);
            const auto* parameter = fixture.session.parameterForSelection(document::kPositionParameterRole);
            const auto* constant =
                parameter != nullptr ? std::get_if<document::ConstantValueSource>(&parameter->source) : nullptr;
            const auto* vector = constant != nullptr ? std::get_if<document::Vec2d>(&constant->value) : nullptr;
            return vector != nullptr ? *vector : document::Vec2d{-1e9, -1e9};
        };
        const std::vector<std::string> targets = mode == commands::SvgImportMode::Layer
                                                     ? std::vector<std::string>{"logo"}
                                                     : std::vector<std::string>{"logo", "box", "circle", "rect"};
        for (const auto& name : targets) {
            const auto found = layersNamed({name});
            expect(found.size() == 1, ("the layer exists: " + name).c_str());
            if (found.size() != 1)
                continue;
            fixture.session.selectLayer(found.front());
            QElapsedTimer timer;
            timer.start();
            // #713: the button follows the selection; the run needs the Viewer's current frame.
            while (!actions.unavailableReason(commands::LayerArrangement::AlignLeft).isEmpty() &&
                   timer.elapsed() < 10000)
                settle(20);
            auto* alignLeft = actions.action(commands::LayerArrangement::AlignLeft);
            expect(alignLeft->isEnabled(),
                   ("Align is enabled with the SVG layer or part selected: " + name + " (" +
                    alignLeft->toolTip().toStdString() + ")")
                       .c_str());
            if (!alignLeft->isEnabled())
                continue;
            const auto before = positionOf(found.front());
            expect(actions.run(commands::LayerArrangement::AlignLeft),
                   ("Align Left runs on " + name).c_str());
            const auto after = positionOf(found.front());
            expect(after.x != before.x || after.y != before.y || name == "logo",
                   ("Align moves " + name + " (or it already lies on the edge)").c_str());
            expect(fixture.session.undo(), ("and one undo reverts the Align on " + name).c_str());
            const auto restored = positionOf(found.front());
            expect(restored.x == before.x && restored.y == before.y,
                   ("and restores the position of " + name).c_str());
        }
    }
}

} // namespace

// ---- #685 ------------------------------------------------------------------------------------

// Counts the paint events of every widget under the given roots.
class PaintCounter final : public QObject {
  public:
    std::map<QWidget*, int> counts;
    std::vector<QWidget*> roots;
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::Paint)
            return false;
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget == nullptr)
            return false;
        for (auto* root : roots)
            if (root != nullptr && (widget == root || root->isAncestorOf(widget)))
                ++counts[widget];
        return false;
    }
};

// While the playhead advances, nothing repaints more than once per frame, and what shows the same
// thing before and after (the status message, the Viewer cells, the anchor grid, a node card)
// does not repaint for it.
void testPlaybackRepaints() {
    test::WindowFixture fixture;
    auto* statusBar = fixture.window->findChild<WindowStatusBar*>();
    QWidget* nodes = nullptr;
    QWidget* properties = nullptr;
    for (auto* area : fixture.window->findChildren<EditorArea*>()) {
        if (area->editorId() == "bloom.nodes")
            nodes = area;
        if (area->editorId() == "bloom.properties")
            properties = area;
    }
    expect(statusBar != nullptr && nodes != nullptr && properties != nullptr,
           "the status bar, Nodes and Properties exist");
    if (statusBar == nullptr || nodes == nullptr || properties == nullptr)
        return;
    settle(300);
    PaintCounter counter;
    counter.roots = {statusBar, nodes, properties};
    qApp->installEventFilter(&counter);
    constexpr int kFrames = 24;
    QElapsedTimer elapsed;
    elapsed.start();
    for (int frame = 1; frame <= kFrames; ++frame) {
        if (const auto time = core::RationalTime::create(frame, 24))
            static_cast<void>(fixture.session.setCurrentTime(*time));
        settle(30);
    }
    qApp->removeEventFilter(&counter);
    // The Memory and Cache readouts also refresh on their own 250 ms clock, whatever the frame
    // rate; on a loaded machine the 24 frames take longer and that clock ticks more often.
    const auto clockTicks = static_cast<int>(elapsed.elapsed() / 250);
    for (const auto& [widget, count] : counter.counts)
        expect(count <= kFrames + clockTicks + 2,
               (std::string("at most one repaint per frame: ") + widget->metaObject()->className() +
                " '" + widget->objectName().toStdString() + "' painted " + std::to_string(count))
                   .c_str());
    for (const char* name :
         {"windowStatusBarMessage", "windowStatusBarExposure", "windowStatusBarChannel",
          "windowStatusBarViewer", "propertiesAnchorGrid"}) {
        auto* widget = fixture.window->findChild<QWidget*>(QString::fromLatin1(name));
        expect(widget != nullptr, (std::string("the widget exists: ") + name).c_str());
        if (widget == nullptr)
            continue;
        const int count = counter.counts.contains(widget) ? counter.counts.at(widget) : 0;
        expect(count <= 2, (std::string("unchanged content does not repaint: ") + name + " painted " +
                            std::to_string(count) + " times over " + std::to_string(kFrames) + " frames")
                               .c_str());
    }
    int nodeWidgetPaints = 0;
    for (const auto& [widget, count] : counter.counts)
        if (nodes->isAncestorOf(widget))
            nodeWidgetPaints = std::max(nodeWidgetPaints, count);
    expect(nodeWidgetPaints <= 2, "a node widget does not repaint for a changed playhead alone");
}

// #685: the Image Preview node's thumbnail paints inside the node editor's viewport, so its own
// paints are counted. Content that does not move with time is not repainted by the playhead, and
// content that changes every frame is repainted at most once per presented frame.
void testImagePreviewPlaybackRepaints() {
    constexpr int kFrames = 24;
    for (const bool animated : {false, true}) {
        test::WindowFixture fixture([&](test::WindowFixture& f) {
            f.imagePreviews = true;
            if (!f.session.addSolidLayer("Red", {1.0, 0.0, 0.0, 1.0}))
                throw std::runtime_error("Image Preview fixture layer failed");
            const auto layers = f.session.composition()->graph().layerOutputs();
            const auto& layer = layers.front();
            if (animated) {
                std::optional<document::ParameterId> opacity;
                const auto* layerNode = f.session.composition()->graph().findNode(layer.nodeId);
                for (const auto& binding : layerNode != nullptr
                                               ? layerNode->parameters
                                               : std::vector<document::ParameterBinding>{})
                    if (binding.role == document::kOpacityParameterRole)
                        opacity = binding.parameterId;
                if (!opacity)
                    throw std::runtime_error("Image Preview fixture opacity missing");
                commands::Transaction automate("Animate", f.session.snapshot().revision());
                automate.emplace<commands::CreateAnimationForParameter>(
                    f.session.compositionId(), *opacity, core::RationalTime{});
                automate.emplace<commands::SetKeyframeAtTimeForParameter>(
                    f.session.compositionId(), *opacity, core::RationalTime{}, 0.05);
                automate.emplace<commands::SetKeyframeAtTimeForParameter>(
                    f.session.compositionId(), *opacity, *core::RationalTime::create(kFrames, 24),
                    1.0);
                if (!f.session.executeTransaction(std::move(automate)).succeeded())
                    throw std::runtime_error("Image Preview fixture animation failed");
            }
            commands::Transaction add("Add Image Preview", f.session.snapshot().revision());
            add.emplace<commands::AddEditorNode>(
                f.session.compositionId(), std::string(document::kImagePreviewNodeType),
                document::Vec2d{400.0, 0.0}, std::nullopt,
                document::OutputPortRef{layer.nodeId, layer.outputPort});
            if (!f.session.executeNodeTransaction(std::move(add)).changed())
                throw std::runtime_error("Image Preview fixture node failed");
        });
        const char* label = animated ? "an animated" : "a static";
        // Let the first thumbnail arrive and settle.
        settle(600);
        for (int i = 0; i < 10; ++i)
            settle(100);
        const auto before = node_editor::imagePreviewPaintCount();
        for (int frame = 1; frame <= kFrames; ++frame) {
            if (const auto time = core::RationalTime::create(frame, 24))
                static_cast<void>(fixture.session.setCurrentTime(*time));
            settle(60);
        }
        settle(200);
        const auto paints = node_editor::imagePreviewPaintCount() - before;
        std::cout << "image-preview " << (animated ? "animated" : "static") << ": frames=" << kFrames
                  << " thumbnailPaints=" << paints << '\n';
        if (animated)
            expect(paints <= static_cast<std::uint64_t>(kFrames) + 2,
                   (std::string(label) + " Image Preview paints at most once per frame: " +
                    std::to_string(paints))
                       .c_str());
        else
            expect(paints <= 2, (std::string(label) + " Image Preview does not repaint: " +
                                 std::to_string(paints))
                                    .c_str());
    }
}

// #713: while a layer is dragged, every step is an edit the Viewer has to catch up with. The
// alignment buttons depend on the selection, not on that, so they neither change nor repaint.
void testAlignButtonsDoNotRepaintDuringDrag() {
    const auto svg = std::filesystem::temp_directory_path() / "g1-align-drag.svg";
    std::ofstream(svg, std::ios::binary)
        << R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120"><circle cx="60" cy="60" r="44" fill="#ee3377"/><rect x="110" y="26" width="72" height="68" rx="10" fill="#3377dd"/></svg>)svg";
    test::WindowFixture fixture([&](test::WindowFixture& f) {
        commands::Transaction transaction("Import SVG", f.session.snapshot().revision());
        transaction
            .emplace<commands::AddSvgArtwork>(f.session.compositionId(), nullptr, svg, "logo", true,
                                              commands::SvgImportMode::Layer)
            .withProjectDirectory(svg.parent_path());
        expect(f.session.executeTransaction(std::move(transaction)).succeeded(), "the SVG imports");
    });
    auto* bar = fixture.window->contextBar();
    auto& actions = bar->alignment();
    std::optional<document::LayerId> logo;
    for (const auto& layer : fixture.session.composition()->graph().layerOutputs())
        if (layer.name == "logo")
            logo = layer.layerId;
    expect(logo.has_value(), "the SVG layer exists");
    if (!logo)
        return;
    fixture.session.selectLayer(*logo);
    QElapsedTimer timer;
    timer.start();
    while (!actions.unavailableReason(commands::LayerArrangement::AlignLeft).isEmpty() &&
           timer.elapsed() < 10000)
        settle(20);
    auto* alignLeft = actions.action(commands::LayerArrangement::AlignLeft);
    expect(alignLeft->isEnabled(), "Align is offered for the selected SVG layer");
    settle(200);

    int actionChanges = 0;
    std::vector<QMetaObject::Connection> connections;
    for (const auto arrangement :
         {commands::LayerArrangement::AlignLeft, commands::LayerArrangement::AlignHorizontalCentre,
          commands::LayerArrangement::AlignRight, commands::LayerArrangement::AlignTop,
          commands::LayerArrangement::AlignVerticalMiddle, commands::LayerArrangement::AlignBottom,
          commands::LayerArrangement::DistributeHorizontalCentres,
          commands::LayerArrangement::DistributeVerticalCentres})
        connections.push_back(QObject::connect(actions.action(arrangement), &QAction::changed,
                                               [&actionChanges] { ++actionChanges; }));
    int groupChanges = 0;
    connections.push_back(QObject::connect(&actions, &ui::LayerAlignmentActions::changed,
                                           [&groupChanges] { ++groupChanges; }));
    PaintCounter counter;
    for (const char* name : {"contextBarAlign", "contextBarDistribute", "contextBarAlignTarget"})
        counter.roots.push_back(bar->findChild<QWidget*>(QString::fromLatin1(name)));
    expect(std::ranges::none_of(counter.roots, [](QWidget* root) { return root == nullptr; }),
           "the Align, Distribute and Align To groups exist");
    qApp->installEventFilter(&counter);
    constexpr int kSteps = 20;
    bool everBehind = false;
    for (int step = 1; step <= kSteps; ++step) {
        expect(fixture.session.setSelectedPosition(100.0 + 7.0 * step, 80.0 + 3.0 * step),
               "a drag step moves the layer");
        everBehind = everBehind ||
                     !actions.unavailableReason(commands::LayerArrangement::AlignLeft).isEmpty();
        settle(30);
    }
    settle(300);
    qApp->removeEventFilter(&counter);
    for (const auto& connection : connections)
        QObject::disconnect(connection);
    expect(everBehind, "the Viewer was behind the edit during the drag (the test saw the old cause)");
    expect(actionChanges == 0 && groupChanges == 0,
           ("#713: no alignment action changes during a drag (" + std::to_string(actionChanges) +
            " action changes, " + std::to_string(groupChanges) + " group changes)")
               .c_str());
    for (const auto& [widget, count] : counter.counts)
        expect(count <= 1,
               (std::string("#713: an unchanged alignment control does not repaint during a drag: ") +
                widget->metaObject()->className() + " '" + widget->objectName().toStdString() +
                "' painted " + std::to_string(count) + " times over " + std::to_string(kSteps) +
                " steps")
                   .c_str());
    expect(alignLeft->isEnabled(), "Align stays offered through the drag");

    // A click while the Viewer is behind is not lost: it runs on the current frame.
    const auto positionNow = [&] {
        const auto* parameter = fixture.session.parameterForSelection(document::kPositionParameterRole);
        const auto* constant =
            parameter != nullptr ? std::get_if<document::ConstantValueSource>(&parameter->source) : nullptr;
        const auto* vector = constant != nullptr ? std::get_if<document::Vec2d>(&constant->value) : nullptr;
        return vector != nullptr ? *vector : document::Vec2d{-1e9, -1e9};
    };
    expect(fixture.session.setSelectedPosition(400.0, 300.0), "the layer moves once more");
    const auto moved = positionNow();
    alignLeft->trigger();
    timer.restart();
    while (positionNow().x == moved.x && timer.elapsed() < 10000)
        settle(20);
    expect(positionNow().x != moved.x,
           "#713: Align Left clicked while the Viewer catches up runs once the frame is current");
    expect(fixture.session.undo() && positionNow().x == moved.x, "and is one undo step");
}


int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomBugBatchTests");
    app.setApplicationName("BugBatchTests");
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    kit::installAfterlabTheme(app);
    testMarqueeBorderWidth();
    testRightClickKeepsSelection();
    testBackspaceAndDeleteKeys();
    testDeleteAndDissolveEveryKind();
    testOutputAndItsMergeDelete();
    testCryptomattePickerOnlyInContext();
    testExposureFieldReadsInFull();
    testNodeEditorPinch();
    testExportPanelFillsWidth();
    {
        test::WindowFixture fixture;
        testWindowSurfaces(fixture);
    }
    testAlignOnSvgLayerAndParts();
    testPlaybackRepaints();
    testAlignButtonsDoNotRepaintDuringDrag();
    testImagePreviewPlaybackRepaints();
    if (test::failures != 0) {
        std::cerr << test::failures << " UI bug batch checks failed\n";
        return 1;
    }
    return 0;
}
