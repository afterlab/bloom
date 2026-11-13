// Multi-monitor workspace windows (#536, docs/architecture/workspace-layout.md "Secondary
// windows"): Window > New Window, shortcuts from a secondary window, one active highlight across
// windows, persistence with the missing-monitor rule, tear-off and re-dock, pinned and following
// Viewers, a window's own display transform in the preview cache key, Fit without padding, and
// (#550) reopening a project on its last viewed composition and pinned targets.

#include "await_qt.hpp"
#include "window_fixture.hpp"

#include <bloom/ui/context_bar.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/project_view_memory.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/workspace_host.hpp>
#include <bloom/ui/workspace_window.hpp>

#include <bloom/host/project_color_choice.hpp>

#include <QAction>
#include <QDropEvent>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QScreen>
#include <QSettings>
#include <QTest>

#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <source_location>
#include <string>

namespace {

using namespace bloom;
using namespace bloom::ui;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

// Delivers the queued events, including deferred deletes (layout, rebuilt rows, closed windows).
void settle() {
    QCoreApplication::sendPostedEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

EditorArea* areaFor(WorkspaceHost& host, const std::string& editorId) {
    for (auto* area : host.areas())
        if (area->editorId() == editorId)
            return area;
    return nullptr;
}

ViewerEditor* viewerIn(WorkspaceHost& host) {
    auto* area = areaFor(host, "bloom.viewer");
    return area != nullptr ? area->findChild<ViewerEditor*>() : nullptr;
}

int highlightedAreas(MainWindow& window) {
    int count = 0;
    for (auto* host : window.workspaceHosts())
        for (auto* area : host->areas())
            count += area->isActiveHighlightShown() ? 1 : 0;
    return count;
}

void activate(QWidget& window) {
    window.activateWindow();
    (void)QTest::qWaitForWindowActive(&window);
    settle();
}

document::CompositionId addComposition(test::WindowFixture& fixture, const std::string& name) {
    commands::Transaction add("Add " + name, fixture.session.snapshot().revision());
    add.emplace<commands::AddComposition>(name, document::CompositionFormat{},
                                          core::RationalTime::fromInteger(10));
    const auto result = fixture.session.executeTransaction(std::move(add));
    return result.succeeded()
               ? result.outputId<document::CompositionId>(commands::kAddCompositionOutput)
                     .value_or(document::CompositionId{})
               : document::CompositionId{};
}

void testNewWindow(Expectations& expectations, test::WindowFixture& fixture) {
    auto& window = *fixture.window;
    auto* action = window.newWindowAction();
    expectations.expect(action != nullptr && action->text() == QStringLiteral("New Window"),
                        "Window > New Window exists");
    action->trigger();
    settle();
    const auto windows = window.workspaceWindows();
    expectations.expect(windows.size() == 1, "New Window opens one secondary window");
    if (windows.size() != 1)
        return;
    auto* secondary = windows.front();
    expectations.expect(secondary->isVisible() && secondary->isWindow(),
                        "the secondary window is a visible top-level window");
    expectations.expect(secondary->findChild<QMenuBar*>() == nullptr &&
                            secondary->findChild<ContextBar*>() == nullptr,
                        "the secondary window has no menu bar and no toolbar");
    auto* host = secondary->workspaceHost();
    expectations.expect(host->areaCount() == 1 && host->areas().front()->editorId() ==
                                                      "bloom.viewer",
                        "a new window opens with a single Viewer");
    expectations.expect(window.activeWorkspaceHost() == host,
                        "the new window's workspace becomes the active one");
    // A full workspace host: split, any editor, close.
    auto* split = host->splitArea(*host->areas().front(), Qt::Horizontal, "bloom.timeline");
    expectations.expect(split != nullptr && host->areaCount() == 2,
                        "the secondary window splits like the main one");
    expectations.expect(host->closeArea(*split) && host->areaCount() == 1,
                        "and closes a panel like the main one");

    secondary->close();
    settle();
    expectations.expect(window.workspaceWindows().isEmpty(),
                        "closing a secondary window closes only that window");
    expectations.expect(window.isVisible(), "the main window stays open");
    expectations.expect(window.activeWorkspaceHost() == window.workspaceHost(),
                        "the main workspace is active again");
}

void testShortcutsFromSecondaryWindow(Expectations& expectations, test::WindowFixture& fixture) {
    auto& window = *fixture.window;
    auto* secondary = window.openWorkspaceWindow();
    activate(*secondary);
    expectations.expect(QApplication::activeWindow() == secondary,
                        "the secondary window is the active window");
    expectations.expect(fixture.session.canUndo(), "the fixture has an edit to undo");
    const bool couldRedo = fixture.session.canRedo();
    QTest::keyClick(secondary, Qt::Key_Z, Qt::ControlModifier);
    settle();
    expectations.expect(!couldRedo && fixture.session.canRedo(),
                        "Ctrl+Z in a secondary window undoes through the shared command stack");
    QTest::keySequence(secondary,
                       window.findChild<QAction*>(QStringLiteral("redoAction"))->shortcut());
    settle();
    expectations.expect(!fixture.session.canRedo(), "and the redo shortcut redoes it");

    // F11 makes the window it was pressed in full screen, on its own monitor.
    QTest::keyClick(secondary, Qt::Key_F11);
    settle();
    expectations.expect(secondary->isFullScreen() && !window.isFullScreen(),
                        "F11 in a secondary window makes that window full screen");
    QTest::keyClick(secondary, Qt::Key_F11);
    settle();
    expectations.expect(!secondary->isFullScreen(), "and F11 again restores it");

    // The application's actions are this window's own, for viewer commands that look them up.
    bool carriesUndo = false;
    for (auto* action : secondary->actions())
        carriesUndo = carriesUndo || action->objectName() == QStringLiteral("undoAction");
    expectations.expect(carriesUndo, "the secondary window carries the application commands");
    secondary->close();
    settle();
}

void testOneHighlightAcrossWindows(Expectations& expectations, test::WindowFixture& fixture) {
    auto& window = *fixture.window;
    activate(window);
    auto* secondary = window.openWorkspaceWindow();
    activate(*secondary);
    expectations.expect(highlightedAreas(window) == 1,
                        "exactly one panel is highlighted across both windows");
    expectations.expect(secondary->workspaceHost()->activeArea() != nullptr &&
                            secondary->workspaceHost()->activeArea()->isActiveHighlightShown(),
                        "the highlight is on the active window's active panel");
    expectations.expect(!window.workspaceHost()->activeArea()->isActiveHighlightShown(),
                        "the main window paints none while the secondary one is active");
    // Panel commands act on the active workspace.
    const int before = secondary->workspaceHost()->areaCount();
    window.findChild<QAction*>(QStringLiteral("splitAreaRightAction"))->trigger();
    settle();
    expectations.expect(secondary->workspaceHost()->areaCount() == before + 1,
                        "Split Active Area Right splits the active window's panel");
    activate(window);
    expectations.expect(highlightedAreas(window) == 1 &&
                            window.workspaceHost()->activeArea()->isActiveHighlightShown(),
                        "activating the main window moves the one highlight back");
    secondary->close();
    settle();
}

void testPlacementRule(Expectations& expectations) {
    const std::vector<WorkspaceScreen> screens{
        {QStringLiteral("Left"), QRect(0, 0, 1920, 1080), QRect(0, 0, 1920, 1040)},
        {QStringLiteral("Right"), QRect(1920, 0, 2560, 1440), QRect(1920, 0, 2560, 1400)}};
    auto kept = resolveWindowPlacement(
        {.screen = QStringLiteral("Right"), .geometry = QRect(2000, 100, 1200, 800)}, screens, 0);
    expectations.expect(kept.screen == 1 && kept.geometry == QRect(2000, 100, 1200, 800) &&
                            !kept.relocated,
                        "a window on a present monitor keeps its place");
    auto moved = resolveWindowPlacement(
        {.screen = QStringLiteral("Right"), .geometry = QRect(3900, 900, 1200, 800)}, screens, 0);
    expectations.expect(moved.screen == 1 && screens[1].available.contains(moved.geometry) &&
                            moved.geometry.size() == QSize(1200, 800),
                        "a frame hanging off its monitor is moved fully onto it");
    auto missing = resolveWindowPlacement(
        {.screen = QStringLiteral("Gone"), .geometry = QRect(9000, 9000, 3000, 2000)}, screens, 0);
    expectations.expect(missing.relocated && missing.screen == 0 &&
                            screens[0].available.contains(missing.geometry) &&
                            missing.geometry.center() == screens[0].available.center(),
                        "a window whose monitor is gone opens centred on the primary screen, "
                        "shrunk to fit");
    auto renamed = resolveWindowPlacement(
        {.screen = QStringLiteral("Gone"), .geometry = QRect(2100, 200, 800, 600)}, screens, 0);
    expectations.expect(renamed.relocated && renamed.screen == 1 &&
                            renamed.geometry == QRect(2100, 200, 800, 600),
                        "a frame still on a visible monitor stays there when its name changed");
}

void testPersistenceRoundTrip(Expectations& expectations, test::WindowFixture& fixture) {
    auto& window = *fixture.window;
    auto* secondary = window.openWorkspaceWindow();
    auto* host = secondary->workspaceHost();
    (void)host->splitArea(*host->areas().front(), Qt::Vertical, "bloom.timeline");
    settle();
    const QString path = fixture.settingsDirectory.filePath(QStringLiteral("windows.ini"));
    {
        QSettings settings(path, QSettings::IniFormat);
        window.saveApplicationState(settings);
        settings.sync();
    }
    QSettings settings(path, QSettings::IniFormat);
    const auto document = QJsonDocument::fromJson(
                              settings.value(QStringLiteral("workspace/compositing/layout"))
                                  .toByteArray())
                              .object();
    expectations.expect(document.value("schema").toInt() == 3, "the workspace schema is 3");
    const auto windows = document.value("windows").toArray();
    expectations.expect(windows.size() == 1, "the secondary window is saved");
    const auto placement = windows.at(0).toObject().value("placement").toObject();
    expectations.expect(placement.value("width").toInt() > 0 &&
                            placement.contains("screen") && placement.contains("fullScreen"),
                        "with its monitor, position and size");
    secondary->close();
    settle();

    // A fresh main window restores it: the same panels, on a visible screen.
    MainWindow restored(fixture.registry, fixture.session, fixture.projectHost, nullptr,
                        fixture.preview.get());
    restored.resize(1600, 1000);
    restored.show();
    (void)restored.restoreApplicationState(settings);
    settle();
    const auto reopened = restored.workspaceWindows();
    expectations.expect(reopened.size() == 1, "the secondary window is restored on relaunch");
    if (reopened.size() == 1) {
        auto* reopenedHost = reopened.front()->workspaceHost();
        expectations.expect(reopenedHost->areaCount() == 2 &&
                                areaFor(*reopenedHost, "bloom.viewer") != nullptr &&
                                areaFor(*reopenedHost, "bloom.timeline") != nullptr,
                            "with its own layout");
        reopened.front()->close();
        settle();
    }

    // A saved window on a monitor that is gone opens on a visible one.
    auto missing = document;
    auto entry = windows.at(0).toObject();
    entry.insert("placement", QJsonObject{{"screen", "Disconnected Monitor"},
                                          {"x", 60000},
                                          {"y", 60000},
                                          {"width", 900},
                                          {"height", 600},
                                          {"fullScreen", false},
                                          {"maximized", false}});
    missing.insert("windows", QJsonArray{entry});
    settings.setValue(QStringLiteral("workspace/compositing/layout"),
                      QJsonDocument(missing).toJson(QJsonDocument::Compact));
    (void)restored.restoreApplicationState(settings);
    settle();
    const auto relocated = restored.workspaceWindows();
    expectations.expect(relocated.size() == 1, "the window of a missing monitor still opens");
    if (relocated.size() == 1) {
        const auto* screen = QGuiApplication::primaryScreen();
        const QRect placed = relocated.front()->geometry();
        expectations.expect(screen->availableGeometry().contains(placed.topLeft()) &&
                                screen->availableGeometry().intersects(placed),
                            "brought onto a visible screen");
        relocated.front()->close();
        settle();
    }
    // A malformed window entry is skipped without stopping the restore.
    missing.insert("windows", QJsonArray{QJsonObject{{"layout", 3}}, QJsonValue("nonsense")});
    settings.setValue(QStringLiteral("workspace/compositing/layout"),
                      QJsonDocument(missing).toJson(QJsonDocument::Compact));
    expectations.expect(restored.restoreApplicationState(settings) ==
                                WorkspaceLayoutRestoreResult::Restored &&
                            restored.workspaceWindows().isEmpty(),
                        "malformed windows are skipped and the main layout still restores");
    restored.hide();
}

void testSchema(Expectations& expectations, test::WindowFixture& fixture) {
    WorkspaceHost host(fixture.registry);
    const QByteArray version2 = R"({"format":"bloom.workspace-layout","schema":2,"root":{
        "type":"area","id":"0f8a3b7e-1c2d-4e5f-8a9b-0c1d2e3f4a5b","editor":"bloom.timeline",
        "active":true}})";
    expectations.expect(host.restoreLayoutState(version2) == WorkspaceLayoutRestoreResult::Restored,
                        "a schema-2 layout still restores");
    const QByteArray badState = R"({"format":"bloom.workspace-layout","schema":3,"root":{
        "type":"area","id":"0f8a3b7e-1c2d-4e5f-8a9b-0c1d2e3f4a5c","editor":"bloom.timeline",
        "active":true,"state":7}})";
    expectations.expect(host.restoreLayoutState(badState) == WorkspaceLayoutRestoreResult::Invalid,
                        "an area state that is not an object is invalid");
    const QByteArray future = R"({"format":"bloom.workspace-layout","schema":4,"root":{}})";
    expectations.expect(host.restoreLayoutState(future) ==
                            WorkspaceLayoutRestoreResult::UnsupportedVersion,
                        "a future schema is not read");
}

void testTearOffAndRedock(Expectations& expectations, test::WindowFixture& fixture) {
    auto& window = *fixture.window;
    auto& main = *window.workspaceHost();
    const int mainAreas = main.areaCount();
    auto* properties = areaFor(main, "bloom.properties");
    expectations.expect(properties != nullptr, "the main window has a Properties panel");
    if (properties == nullptr)
        return;

    // The panel menu's Move to New Window, the same request a drag released outside makes.
    auto* move = properties->findChild<QAction*>(QStringLiteral("panelMoveToNewWindowAction"));
    expectations.expect(move != nullptr && move->isEnabled(), "Move to New Window is offered");
    if (move == nullptr)
        return;
    move->trigger();
    settle();
    auto windows = window.workspaceWindows();
    expectations.expect(windows.size() == 1 && main.areaCount() == mainAreas - 1,
                        "tearing a panel off moves it into a new window");
    if (windows.size() != 1)
        return;
    auto* torn = windows.front()->workspaceHost();
    expectations.expect(torn->areaCount() == 1 && torn->areas().front()->editorId() ==
                                                      "bloom.properties",
                        "the new window holds that panel");
    auto* onlyPanel =
        torn->areas().front()->findChild<QAction*>(QStringLiteral("panelMoveToNewWindowAction"));
    expectations.expect(onlyPanel != nullptr && !onlyPanel->isEnabled(),
                        "a window's only panel cannot tear off again");

    // Dragging it back: a drop on the right half of the Timeline docks it there.
    auto* timeline = areaFor(main, "bloom.timeline");
    expectations.expect(timeline != nullptr && timeline->dockEdgeAt(QPointF(
                                                   timeline->width() - 2.0,
                                                   timeline->height() / 2.0)) ==
                                                   PanelDockEdge::Right &&
                            timeline->dockEdgeAt(QPointF(timeline->width() / 2.0, 2.0)) ==
                                PanelDockEdge::Top,
                        "the dock edge is the nearest edge of the area");
    QMimeData mime;
    mime.setData(QString::fromLatin1(kPanelDragMimeType),
                 torn->areas().front()->areaId().toUtf8());
    // Qt delivers a drop to the widget that accepted the drag's enter, so the drag enters first.
    const QPoint rightEdge(timeline->width() - 2, timeline->height() / 2);
    QDragEnterEvent enter(rightEdge, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &enter);
    QDragMoveEvent over(rightEdge, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &over);
    expectations.expect(enter.isAccepted() && over.isAccepted() &&
                            timeline->dockIndicatorEdge() == PanelDockEdge::Right,
                        "dragging over the Timeline's right edge shows where it docks");
    QDropEvent drop(QPointF(rightEdge), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(timeline, &drop);
    settle();
    settle();
    expectations.expect(drop.isAccepted(), "the Timeline takes the dropped panel");
    expectations.expect(main.areaCount() == mainAreas && areaFor(main, "bloom.properties"),
                        "re-docking puts the panel back in the main window");
    expectations.expect(window.workspaceWindows().isEmpty(),
                        "the window it left, now empty, closes");
    auto* docked = areaFor(main, "bloom.properties");
    auto* splitter = docked != nullptr ? qobject_cast<QSplitter*>(docked->parentWidget()) : nullptr;
    expectations.expect(splitter != nullptr && splitter->orientation() == Qt::Horizontal &&
                            splitter->indexOf(docked) == splitter->indexOf(timeline) + 1,
                        "it docks to the right of the Timeline");

    // The main window's last panel never leaves it.
    WorkspaceHost single(fixture.registry);
    expectations.expect(window.tearOffPanel(*single.areas().front()) == nullptr,
                        "a panel of no window of this application does not tear off");
}

void testPinnedAndFollowingViewers(Expectations& expectations, test::WindowFixture& fixture) {
    auto& window = *fixture.window;
    auto& session = fixture.session;
    const auto first = session.compositionId();
    const auto second = addComposition(fixture, "Second");
    expectations.expect(second.isValid(), "a second composition exists");

    auto* mainViewer = viewerIn(*window.workspaceHost());
    auto* secondary = window.openWorkspaceWindow();
    settle();
    auto* windowViewer = viewerIn(*secondary->workspaceHost());
    expectations.expect(mainViewer != nullptr && windowViewer != nullptr, "both have a Viewer");
    if (mainViewer == nullptr || windowViewer == nullptr)
        return;
    expectations.expect(!mainViewer->hasOwnPreview() && windowViewer->hasOwnPreview(),
                        "a secondary window's Viewer prepares through its own controller");
    expectations.expect(windowViewer->pin().mode == ViewerPinMode::Follow &&
                            windowViewer->boundPreviewForTest().targetCompositionId() == first,
                        "it follows the active composition by default");
    expectations.expect(windowViewer->pinDescription() ==
                            QStringLiteral("Following the active composition"),
                        "and says so");

    expectations.expect(windowViewer->pinToActiveComposition(), "pinning to the active one works");
    auto* label = windowViewer->window()->findChild<kit::KLabel*>(QStringLiteral("viewerPinLabel"));
    expectations.expect(label != nullptr && label->isVisible() &&
                            windowViewer->pinDescription().contains(QStringLiteral("Pinned")),
                        "a pinned Viewer shows that it is pinned");
    (void)session.setComposition(second);
    settle();
    expectations.expect(mainViewer->boundPreviewForTest().targetCompositionId() == second,
                        "the following Viewer shows the newly active composition");
    expectations.expect(windowViewer->boundPreviewForTest().targetCompositionId() == first,
                        "the pinned Viewer keeps showing its composition");
    expectations.expect(bloom::test_support::awaitQtCondition(
                            "preview requests are for the pinned composition",
                            [&] {
                                const auto& state = windowViewer->boundPreviewForTest().state();
                                return state.desiredIdentity &&
                                       state.desiredIdentity->compositionId == first;
                            }),
                        "its preview requests are for the pinned composition");
    expectations.expect(windowViewer->isReferenceOnly() && !mainViewer->isReferenceOnly(),
                        "a Viewer pinned elsewhere is a reference view");

    // The pin rides with the panel when it moves to another window.
    auto* windowArea = areaFor(*secondary->workspaceHost(), "bloom.viewer");
    const auto state = windowArea->editorSessionState();
    expectations.expect(state.value("pin").toObject().value("mode").toString() ==
                            QStringLiteral("composition"),
                        "the pin is the Viewer's session state");

    windowViewer->followActiveComposition();
    expectations.expect(windowViewer->boundPreviewForTest().targetCompositionId() == second &&
                            !windowViewer->isReferenceOnly(),
                        "following again shows the active composition");

    // A node pin shows that node's output.
    (void)session.setComposition(first);
    settle();
    std::optional<document::NodeId> node;
    for (const auto& layer : session.composition()->graph().layerOutputs())
        node = layer.nodeId;
    if (node)
        session.selectNode(*node);
    expectations.expect(node && windowViewer->pinToSelectedNode(),
                        "pinning to the selected node works");
    const auto& inspection = windowViewer->boundPreviewForTest().viewerChannelInspection();
    expectations.expect(windowViewer->pin().mode == ViewerPinMode::Node && inspection &&
                            inspection->node == *node && inspection->channels.empty(),
                        "a node pin inspects that node's own output");
    windowViewer->followActiveComposition();

    // A Viewer in the main window owns a controller only while pinned.
    expectations.expect(mainViewer->pinToActiveComposition() && mainViewer->hasOwnPreview(),
                        "pinning a main-window Viewer gives it its own controller");
    mainViewer->followActiveComposition();
    expectations.expect(!mainViewer->hasOwnPreview() &&
                            &mainViewer->boundPreviewForTest() == fixture.preview.get(),
                        "following again returns it to the shared controller");
    secondary->close();
    settle();
}

void testDisplayTransformIdentity(Expectations& expectations, test::WindowFixture& fixture) {
    auto& window = *fixture.window;
    // ACES offers several displays and views; the neutral default offers one.
    auto aces = host::resolveProjectColorSettings(fixture.session.colorSettings(),
                                                  host::ProjectColorConfig::AcesCg, std::nullopt);
    expectations.expect(aces.settings.has_value(), "the ACES configuration resolves");
    if (!aces.settings)
        return;
    fixture.session.setColorSettings(*aces.settings);
    settle();
    auto* secondary = window.openWorkspaceWindow();
    settle();
    auto* area = areaFor(*secondary->workspaceHost(), "bloom.viewer");
    auto* viewer = area != nullptr ? area->findChild<ViewerEditor*>() : nullptr;
    auto* displayView = area != nullptr
                            ? area->findChild<kit::KDropdown*>(QStringLiteral("viewerDisplayView"))
                            : nullptr;
    expectations.expect(viewer != nullptr && displayView != nullptr && displayView->count() > 1,
                        "the window's Viewer offers several displays and views");
    if (viewer == nullptr || displayView == nullptr || displayView->count() < 2)
        return;
    const auto mainSettings = fixture.preview->settings();
    const int other = displayView->currentIndex() == 0 ? 1 : 0;
    displayView->setCurrentIndex(other);
    settle();
    const auto chosen = displayView->itemData(other).toStringList();
    const auto& own = viewer->boundPreviewForTest();
    expectations.expect(chosen.size() == 2 &&
                            own.settings().displayName == chosen[0].toStdString() &&
                            own.settings().viewName == chosen[1].toStdString(),
                        "the window's Viewer uses its own display and view");
    expectations.expect(fixture.preview->settings().displayName == mainSettings.displayName &&
                            fixture.preview->settings().viewName == mainSettings.viewName,
                        "the main window's display transform is unchanged");
    const auto time = fixture.session.currentTime();
    const auto ownKey = own.cacheKeyForTime(time);
    const auto mainKey = fixture.preview->cacheKeyForTime(time);
    expectations.expect(ownKey && mainKey && ownKey->displayName == chosen[0].toStdString() &&
                            ownKey->viewName == chosen[1].toStdString() && !(*ownKey == *mainKey),
                        "the display transform is part of the preview cache key");
    expectations.expect(
        bloom::test_support::awaitQtCondition(
            "every request the window's Viewer makes is for its pinned composition",
            [&] {
                const auto& state = own.state();
                return state.desiredIdentity &&
                       PreviewFrameCacheKey::forIdentity(*state.desiredIdentity).displayName ==
                           chosen[0].toStdString();
            }),
        "and of every request the window's Viewer makes");
    expectations.expect(area->editorSessionState().value("displayView").toArray().size() == 2,
                        "the choice is saved with the window's workspace");
    secondary->close();
    settle();
}

// The fitted frame touches the limiting edges of the Viewer's content area: no padding.
bool fitTouchesEdges(ViewerEditor& viewer, std::string& detail) {
    const auto descriptor = viewer.actualDisplayDescriptor();
    if (!descriptor || !descriptor->mapping) {
        detail = "no displayed frame";
        return false;
    }
    const QRectF frame = descriptor->mapping->displayRect;
    const QRectF content = viewer.contentRectForTest();
    const bool widthLimits =
        content.width() / content.height() <= frame.width() / std::max(1e-9, frame.height()) + 1e-6;
    const auto near = [](const qreal a, const qreal b) { return std::abs(a - b) <= 0.5; };
    detail = "frame " + std::to_string(frame.left()) + "," + std::to_string(frame.top()) + " " +
             std::to_string(frame.width()) + "x" + std::to_string(frame.height()) + " in " +
             std::to_string(content.width()) + "x" + std::to_string(content.height());
    return widthLimits ? near(frame.left(), content.left()) && near(frame.right(), content.right())
                       : near(frame.top(), content.top()) && near(frame.bottom(), content.bottom());
}

void testFitHasNoPadding(Expectations& expectations, test::WindowFixture& fixture) {
    auto& window = *fixture.window;
    auto* mainViewer = viewerIn(*window.workspaceHost());
    std::string detail;
    expectations.expect(mainViewer != nullptr &&
                            bloom::test_support::awaitQtCondition(
                                "Fit fills a docked Viewer to its limiting edges",
                                [&] { return fitTouchesEdges(*mainViewer, detail); }),
                        "Fit fills a docked Viewer to its limiting edges: " + detail);
    window.resize(1500, 1100);
    settle();
    expectations.expect(
        bloom::test_support::awaitQtCondition("Fit stays exact when the Viewer resizes",
                                              [&] { return fitTouchesEdges(*mainViewer, detail); }),
        "and stays exact when the Viewer resizes: " + detail);

    auto* secondary = window.openWorkspaceWindow();
    secondary->resize(1100, 500);
    settle();
    auto* windowViewer = viewerIn(*secondary->workspaceHost());
    expectations.expect(windowViewer != nullptr &&
                            bloom::test_support::awaitQtCondition(
                                "Fit fills a Viewer in a separate window",
                                [&] { return fitTouchesEdges(*windowViewer, detail); }),
                        "Fit fills a Viewer in a separate window: " + detail);
    secondary->resize(600, 900);
    settle();
    expectations.expect(bloom::test_support::awaitQtCondition(
                            "Fit has the other axis limiting after a resize",
                            [&] { return fitTouchesEdges(*windowViewer, detail); }),
                        "with the other axis limiting after a resize: " + detail);
    secondary->showFullScreen();
    settle();
    expectations.expect(
        bloom::test_support::awaitQtCondition(
            "Fit is exact in full screen", [&] { return fitTouchesEdges(*windowViewer, detail); }),
        "and full screen: " + detail);
    if (const auto directory = qEnvironmentVariable("BLOOM_WORKSPACE_WINDOW_CAPTURE");
        !directory.isEmpty()) {
        (void)window.grab().save(directory + QStringLiteral("/main-window.png"));
        secondary->showNormal();
        secondary->resize(1100, 640);
        settle();
        (void)windowViewer->pinToActiveComposition();
        settle();
        (void)secondary->grab().save(directory + QStringLiteral("/secondary-window.png"));
    }
    secondary->close();
    settle();
}

// #550: a project reopens on the composition last viewed in it, and each pinned Viewer on its own
// target, as session state that never dirties the project or touches its file.
void testReopenLastViewedComposition(Expectations& expectations, test::WindowFixture& fixture) {
    auto& host = fixture.projectHost;
    auto& session = fixture.session;
    // The application's own wiring (apps/bloom/main.cpp).
    const auto replaced = QObject::connect(&host, &ProjectHost::sessionReplaced, &session, [&] {
        auto [document, stack] = host.liveDocumentAndStack();
        if (document == nullptr || stack == nullptr)
            return;
        session.projectViewMemory().beginProject(host.displayPath());
        session.rebindToRememberedComposition(*document, *stack, host.lowestCompositionId());
    });
    int saves = 0;
    int opens = 0;
    const auto savedConnection =
        QObject::connect(&host, &ProjectHost::saveFinished, &session,
                         [&](const ProjectHostOperationOutcome outcome, const QString&) {
                             ++saves;
                             if (outcome == ProjectHostOperationOutcome::Published ||
                                 outcome == ProjectHostOperationOutcome::PublishedWithWarning)
                                 session.projectViewMemory().moveProject(host.displayPath());
                         });
    const auto openedConnection =
        QObject::connect(&host, &ProjectHost::openFinished, &session,
                         [&](const ProjectHostOperationOutcome, const QString&) { ++opens; });
    const auto open = [&](const std::filesystem::path& path) {
        const int before = opens;
        host.beginOpen(path);
        return bloom::test_support::awaitQtCondition(
                   "reopen of the second project finishes",
                   [&] { return opens > before && !host.isBusy(); }) &&
               session.composition() != nullptr;
    };
    const auto fileBytes = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };

    auto& window = *fixture.window;
    const auto first = session.compositionId();
    const auto second = addComposition(fixture, "Reopen Second");
    expectations.expect(second.isValid() && session.setComposition(first),
                        "reopen: a second composition exists");
    auto* secondary = window.openWorkspaceWindow();
    settle();
    auto* windowViewer = viewerIn(*secondary->workspaceHost());
    auto* mainViewer = viewerIn(*window.workspaceHost());
    if (windowViewer == nullptr || mainViewer == nullptr) {
        expectations.expect(false, "reopen: both windows have a Viewer");
        return;
    }
    const auto windowArea = areaFor(*secondary->workspaceHost(), "bloom.viewer")->areaId();
    expectations.expect(windowViewer->pinToActiveComposition(),
                        "reopen: the secondary window's Viewer pins to the first composition");
    expectations.expect(session.setComposition(second), "reopen: the artist views the second one");

    const auto path = fixture.settingsDirectory.filePath(QStringLiteral("reopen-last.bloom"));
    host.beginSaveAs(path.toStdString());
    expectations.expect(
        bloom::test_support::awaitQtCondition("project save finishes",
                                              [&] { return saves == 1 && !host.isBusy(); }) &&
            !host.isDirty(),
        "reopen: the project is saved");
    const auto savedBytes = fileBytes(path);
    const auto savedRevision = session.snapshot().revision();
    expectations.expect(session.setComposition(first) && session.setComposition(second) &&
                            !host.isDirty() && session.snapshot().revision() == savedRevision,
                        "reopen: switching compositions never dirties the project");

    // Close it (a blank project replaces it), then open it again.
    expectations.expect(host.replaceWithBlankProject() && session.composition() == nullptr &&
                            windowViewer->pin().mode == ViewerPinMode::Follow,
                        "reopen: the blank project has no composition and nothing pinned");
    expectations.expect(open(path.toStdString()), "reopen: the project opens again");
    expectations.expect(session.compositionId() == second,
                        "reopen: it opens on the composition last viewed, not the first");
    expectations.expect(mainViewer->boundPreviewForTest().targetCompositionId() == second,
                        "reopen: the main Viewer shows that composition");
    expectations.expect(windowViewer->pin().mode == ViewerPinMode::Composition &&
                            windowViewer->pin().composition == first &&
                            windowViewer->boundPreviewForTest().targetCompositionId() == first,
                        "reopen: the pinned Viewer in the other window restores its own target");
    expectations.expect(!host.isDirty() && fileBytes(path) == savedBytes,
                        "reopen: remembering changes neither the document nor the saved file");

    // A relaunch: nothing in memory, everything read back from the application settings.
    {
        ProjectViewMemory relaunched;
        relaunched.beginProject(std::filesystem::path(path.toStdString()));
        expectations.expect(relaunched.recallComposition() == second,
                            "relaunch: the last viewed composition comes from the settings");
        const auto pin = relaunched.recallViewerPin(windowArea);
        expectations.expect(pin && pin->value("composition").toString() ==
                                       QString::number(first.value()),
                            "relaunch: so does the pinned Viewer's target");
        ProjectViewMemory other;
        other.beginProject(std::filesystem::path(
            fixture.settingsDirectory.filePath(QStringLiteral("other.bloom")).toStdString()));
        expectations.expect(!other.recallComposition() && !other.recallViewerPin(windowArea),
                            "relaunch: another project remembers nothing of it");
    }
    // A relaunch with the last project: the windows come back from the saved layout before the
    // project opens, and opening it restores each window's target.
    const QString settingsPath = fixture.settingsDirectory.filePath(QStringLiteral("reopen.ini"));
    {
        QSettings settings(settingsPath, QSettings::IniFormat);
        window.saveApplicationState(settings);
        settings.sync();
    }
    secondary->close();
    settle();
    expectations.expect(host.replaceWithBlankProject(), "relaunch: Bloom starts blank");
    {
        MainWindow restored(fixture.registry, session, host, nullptr, fixture.preview.get());
        restored.resize(1600, 1000);
        restored.show();
        QSettings settings(settingsPath, QSettings::IniFormat);
        (void)restored.restoreApplicationState(settings);
        settle();
        const auto windows = restored.workspaceWindows();
        auto* restoredViewer =
            windows.size() == 1 ? viewerIn(*windows.front()->workspaceHost()) : nullptr;
        expectations.expect(restoredViewer != nullptr,
                            "relaunch: the secondary window and its Viewer are restored");
        expectations.expect(open(path.toStdString()) && session.compositionId() == second,
                            "relaunch: opening the last project shows its last viewed composition");
        if (restoredViewer != nullptr)
            expectations.expect(restoredViewer->pin().mode == ViewerPinMode::Composition &&
                                    restoredViewer->pin().composition == first,
                                "relaunch: the restored window's Viewer is pinned to its target");

        // The remembered composition and pin no longer exist: an unsaved composition is viewed
        // and pinned, then discarded with the project.
        const auto discarded = addComposition(fixture, "Reopen Discarded");
        expectations.expect(discarded.isValid() && session.setComposition(discarded) &&
                                restoredViewer != nullptr &&
                                restoredViewer->pinToActiveComposition(),
                            "fallback: an unsaved composition is viewed and pinned");
        expectations.expect(host.replaceWithBlankProject() && open(path.toStdString()),
                            "fallback: the project opens again without it");
        expectations.expect(session.compositionId() == host.lowestCompositionId(),
                            "fallback: a composition that is gone falls back to the first one");
        if (restoredViewer != nullptr)
            expectations.expect(restoredViewer->pin().mode == ViewerPinMode::Follow,
                                "fallback: a pin whose composition is gone follows");
        for (auto* opened : restored.workspaceWindows())
            opened->close();
        settle();
        restored.hide();
    }
    QObject::disconnect(replaced);
    QObject::disconnect(savedConnection);
    QObject::disconnect(openedConnection);
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    Expectations expectations;
    try {
        testPlacementRule(expectations);
        test::WindowFixture fixture;
        testNewWindow(expectations, fixture);
        testShortcutsFromSecondaryWindow(expectations, fixture);
        testOneHighlightAcrossWindows(expectations, fixture);
        testSchema(expectations, fixture);
        testPersistenceRoundTrip(expectations, fixture);
        testTearOffAndRedock(expectations, fixture);
        testDisplayTransformIdentity(expectations, fixture);
        testFitHasNoPadding(expectations, fixture);
        testPinnedAndFollowingViewers(expectations, fixture);
        testReopenLastViewedComposition(expectations, fixture);
    } catch (const std::exception& error) {
        std::cerr << "fixture failed: " << error.what() << '\n';
        return 2;
    }
    if (expectations.failures() + bloom::test_support::awaitFailureCount() != 0) {
        std::cerr << expectations.failures() << " expectation(s) failed\n";
        return 1;
    }
    std::cout << "workspace window tests passed\n";
    return 0;
}
