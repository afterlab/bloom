// Workspace panel behaviour (docs/architecture/workspace-layout.md): the active highlight while a
// panel is maximized (#537) and after a workspace preset switch (#496), and joining two panels
// from the divider between them (#538).

#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/surfaces.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/workspace_host.hpp>
#include <bloom/ui/workspace_presets.hpp>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPointer>
#include <QSplitter>
#include <QSplitterHandle>
#include <QWidget>

#include <iostream>
#include <source_location>
#include <string>

namespace {

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

EditorRegistry makeRegistry() {
    EditorRegistry registry;
    for (const auto* id : {"bloom.viewer", "bloom.assets", "bloom.nodes", "bloom.properties",
                           "bloom.timeline", "bloom.export"}) {
        (void)registry.registerEditor({id, QString::fromLatin1(id), [](QWidget* parent) {
                                           auto* editor = new QWidget(parent);
                                           editor->setObjectName(QStringLiteral("probeEditor"));
                                           return editor;
                                       }});
    }
    return registry;
}

void show(WorkspaceHost& host) {
    host.resize(1400, 900);
    host.show();
    host.activateWindow();
    QCoreApplication::processEvents();
}

void settle() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

bool highlighted(EditorArea* area) {
    // KPanelFrame has no Q_OBJECT, so it is found as a QWidget by its object name.
    auto* frame = dynamic_cast<kit::KPanelFrame*>(
        area->findChild<QWidget*>(QStringLiteral("panelFrame"), Qt::FindDirectChildrenOnly));
    return frame != nullptr && frame->isActive() && area->isActiveHighlightShown();
}

int highlightedCount(WorkspaceHost& host) {
    int count = 0;
    for (auto* area : host.findChildren<EditorArea*>())
        count += highlighted(area) ? 1 : 0;
    return count;
}

EditorArea* areaFor(WorkspaceHost& host, const std::string& id) {
    for (auto* area : host.findChildren<EditorArea*>())
        if (area->editorId() == id)
            return area;
    return nullptr;
}

QSplitter* rootSplitter(WorkspaceHost& host) {
    for (auto* splitter : host.findChildren<QSplitter*>())
        if (splitter->parentWidget() == &host)
            return splitter;
    return nullptr;
}

// Right-clicks the divider before `index` and returns the Join menu it opened.
QMenu* openDividerMenu(WorkspaceHost& host, QSplitter& splitter, const int index) {
    auto* handle = splitter.handle(index);
    if (handle == nullptr)
        return nullptr;
    const QPoint local(handle->width() / 2, handle->height() / 2);
    QContextMenuEvent event(QContextMenuEvent::Mouse, local, handle->mapToGlobal(local));
    QCoreApplication::sendEvent(handle, &event);
    QCoreApplication::processEvents();
    auto* menu = host.findChild<QMenu*>(QStringLiteral("workspaceDividerMenu"));
    return menu != nullptr && menu->isVisible() ? menu : nullptr;
}

void testMaximizeHidesTheActiveHighlight(Expectations& expectations) {
    const auto registry = makeRegistry();
    WorkspaceHost host(registry);
    auto* viewer = host.activeArea();
    auto* nodes = host.splitArea(*viewer, Qt::Horizontal, "bloom.nodes");
    auto* timeline = host.splitArea(*viewer, Qt::Vertical, "bloom.timeline");
    show(host);
    host.setActiveArea(nodes);
    expectations.expect(highlighted(nodes) && highlightedCount(host) == 1,
                        "before maximizing, only the active panel is highlighted");

    host.toggleMaximizeActiveArea();
    expectations.expect(host.isAreaMaximized() && host.activeArea() == nodes &&
                            nodes->isAreaActive(),
                        "maximizing keeps the panel active");
    expectations.expect(highlightedCount(host) == 0,
                        "a maximized panel shows no active highlight, and nor does any other");

    host.setActiveArea(timeline);
    expectations.expect(host.activeArea() == nodes && highlightedCount(host) == 0,
                        "while maximized, focus stays on the maximized panel");

    host.toggleMaximizeActiveArea();
    expectations.expect(!host.isAreaMaximized() && highlighted(nodes) &&
                            highlightedCount(host) == 1,
                        "restoring brings the highlight back on the focused panel only");
    expectations.expect(!highlighted(viewer) && !highlighted(timeline),
                        "no other panel gains the highlight on restore");
    host.hide();
}

void testAPresetSwitchLeavesOneHighlight(Expectations& expectations) {
    const auto registry = makeRegistry();
    WorkspaceHost host(registry);
    host.resetToDefaultLayout("bloom.viewer", "bloom.nodes", "bloom.assets", "bloom.timeline",
                              "bloom.properties");
    show(host);
    WorkspacePresets presets(host);
    expectations.expect(presets.apply(QStringLiteral("Default")), "Default applies");
    auto* assets = areaFor(host, "bloom.assets");
    host.setActiveArea(assets);
    expectations.expect(highlighted(assets) && highlightedCount(host) == 1,
                        "Assets is the one highlighted panel");

    for (const auto* name : {"Export", "Default", "Node", "Default"}) {
        expectations.expect(presets.apply(QString::fromLatin1(name)), "a preset applies");
        settle();
        expectations.expect(highlightedCount(host) == 1 && host.activeArea() != nullptr &&
                                highlighted(host.activeArea()),
                            std::string{"after switching to "} + name +
                                " exactly one panel, the active one, is highlighted");
    }

    auto* viewer = areaFor(host, "bloom.viewer");
    assets = areaFor(host, "bloom.assets");
    host.setActiveArea(viewer);
    expectations.expect(highlighted(viewer) && !highlighted(assets) && highlightedCount(host) == 1,
                        "clicking another panel moves the one highlight; none stays behind");
    host.hide();
}

void testTheDividerShowsAResizeCursor(Expectations& expectations) {
    const auto registry = makeRegistry();
    WorkspaceHost host(registry);
    auto* viewer = host.activeArea();
    (void)host.splitArea(*viewer, Qt::Horizontal, "bloom.nodes");
    show(host);
    auto* root = rootSplitter(host);
    auto* handle = root != nullptr ? root->handle(1) : nullptr;
    expectations.expect(handle != nullptr && handle->cursor().shape() == Qt::SplitHCursor,
                        "a vertical divider shows the horizontal resize cursor");
    (void)host.splitArea(*viewer, Qt::Vertical, "bloom.timeline");
    QSplitter* stacked = nullptr;
    for (auto* splitter : host.findChildren<QSplitter*>())
        if (splitter->orientation() == Qt::Vertical)
            stacked = splitter;
    handle = stacked != nullptr ? stacked->handle(1) : nullptr;
    expectations.expect(handle != nullptr && handle->cursor().shape() == Qt::SplitVCursor,
                        "a horizontal divider shows the vertical resize cursor");
    host.hide();
}

struct JoinCase {
    Qt::Orientation orientation;
    const char* actionName;
    const char* otherActionName;
    const char* text;
    bool leadingSurvives;
};

void testAllFourJoins(Expectations& expectations) {
    const auto registry = makeRegistry();
    for (const auto& join : {JoinCase{Qt::Horizontal, "workspaceJoinLeftAction",
                                      "workspaceJoinRightAction", "Join Left", false},
                             JoinCase{Qt::Horizontal, "workspaceJoinRightAction",
                                      "workspaceJoinLeftAction", "Join Right", true},
                             JoinCase{Qt::Vertical, "workspaceJoinUpAction",
                                      "workspaceJoinDownAction", "Join Up", false},
                             JoinCase{Qt::Vertical, "workspaceJoinDownAction",
                                      "workspaceJoinUpAction", "Join Down", true}}) {
        const std::string label = join.text;
        WorkspaceHost host(registry);
        auto* leading = host.activeArea();
        auto* trailing = host.splitArea(*leading, join.orientation, "bloom.nodes");
        show(host);
        auto* root = rootSplitter(host);
        auto* menu = root != nullptr ? openDividerMenu(host, *root, 1) : nullptr;
        expectations.expect(menu != nullptr, label + ": a right-click on the divider opens a menu");
        if (menu == nullptr)
            continue;
        auto* action = menu->findChild<QAction*>(QString::fromLatin1(join.actionName));
        expectations.expect(
            action != nullptr && action->text() == QString::fromLatin1(join.text) &&
                action->isEnabled() && menu->actions().size() == 2 &&
                menu->findChild<QAction*>(QString::fromLatin1(join.otherActionName)) != nullptr,
            label + ": the divider offers both joins of its orientation, enabled");
        if (action == nullptr)
            continue;

        EditorArea* survivor = join.leadingSurvives ? leading : trailing;
        const QPointer<EditorArea> absorbed(join.leadingSurvives ? trailing : leading);
        // The survivor's editor and its state stay: the same panel and the same editor widget.
        auto* survivorEditor = survivor->findChild<QWidget*>(QStringLiteral("probeEditor"));
        const auto survivorEditorId = survivor->editorId();
        // The closing panel holds focus, so the join itself must hand focus to the survivor.
        host.setActiveArea(absorbed);
        absorbed->setFocus(Qt::OtherFocusReason);
        action->trigger();
        menu->close();
        settle();

        expectations.expect(absorbed.isNull() && host.areaCount() == 1,
                            label + ": the other panel closes");
        expectations.expect(host.findChildren<EditorArea*>().size() == 1 &&
                                host.findChildren<EditorArea*>().front() == survivor &&
                                survivor->editorId() == survivorEditorId &&
                                survivor->findChild<QWidget*>(QStringLiteral("probeEditor")) ==
                                    survivorEditor,
                            label + ": the surviving panel keeps its editor and state");
        expectations.expect(host.activeArea() == survivor && highlighted(survivor) &&
                                survivor->hasFocus(),
                            label + ": focus goes to the surviving panel");
        host.hide();
    }
}

void testAJoinInAWiderSplitGivesTheRoomToTheSurvivor(Expectations& expectations) {
    const auto registry = makeRegistry();
    WorkspaceHost host(registry);
    const QByteArray state = R"({"format":"bloom.workspace-layout","schema":2,"root":{
        "type":"split","orientation":"horizontal","weights":[0.3,0.3,0.4],"children":[
        {"type":"area","id":"00000000-0000-4000-8000-000000000001","editor":"bloom.viewer",
         "active":true},
        {"type":"area","id":"00000000-0000-4000-8000-000000000002","editor":"bloom.nodes",
         "active":false},
        {"type":"area","id":"00000000-0000-4000-8000-000000000003","editor":"bloom.assets",
         "active":false}]}})";
    expectations.expect(host.restoreLayoutState(state) == WorkspaceLayoutRestoreResult::Restored,
                        "a three-panel split restores");
    show(host);
    auto* root = rootSplitter(host);
    if (root == nullptr || root->count() != 3) {
        expectations.expect(false, "the root split holds three panels");
        return;
    }
    const auto before = root->sizes();
    auto* middle = qobject_cast<EditorArea*>(root->widget(1));
    auto* menu = openDividerMenu(host, *root, 2);
    auto* joinLeft = menu != nullptr
                         ? menu->findChild<QAction*>(QStringLiteral("workspaceJoinLeftAction"))
                         : nullptr;
    expectations.expect(joinLeft != nullptr && joinLeft->isEnabled(),
                        "the second divider offers Join Left");
    if (joinLeft == nullptr)
        return;
    joinLeft->trigger();
    menu->close();
    settle();
    const auto after = root->sizes();
    expectations.expect(root->count() == 2 && host.areaCount() == 2 && middle != nullptr &&
                            root->widget(1) == areaFor(host, "bloom.assets") &&
                            areaFor(host, "bloom.nodes") == nullptr,
                        "Join Left on the second divider closes the middle panel");
    expectations.expect(after.size() == 2 && after[0] == before[0] &&
                            after[1] >= before[1] + before[2],
                        "the first panel keeps its width and the survivor takes the closed room");
    host.hide();
}

void testJoinIsDisabledWhenASideIsSplit(Expectations& expectations) {
    const auto registry = makeRegistry();
    WorkspaceHost host(registry);
    auto* viewer = host.activeArea();
    auto* nodes = host.splitArea(*viewer, Qt::Horizontal, "bloom.nodes");
    (void)host.splitArea(*nodes, Qt::Vertical, "bloom.timeline");
    show(host);
    auto* root = rootSplitter(host);
    auto* menu = root != nullptr ? openDividerMenu(host, *root, 1) : nullptr;
    expectations.expect(menu != nullptr, "the root divider opens its menu");
    if (menu == nullptr)
        return;
    for (const auto* name : {"workspaceJoinLeftAction", "workspaceJoinRightAction"}) {
        auto* action = menu->findChild<QAction*>(QString::fromLatin1(name));
        expectations.expect(action != nullptr && !action->isEnabled() &&
                                action->toolTip().contains(QStringLiteral("split")),
                            std::string{name} + " is disabled with the reason in its tooltip");
        if (action != nullptr)
            action->trigger();
    }
    menu->close();
    settle();
    expectations.expect(host.areaCount() == 3, "a disabled join changes nothing");
    expectations.expect(!host.joinAreas(*viewer, *nodes),
                        "joinAreas refuses a pair that does not share a divider as two panels");
    host.hide();
}

void testAJoinedLayoutPersists(Expectations& expectations) {
    const auto registry = makeRegistry();
    WorkspaceHost host(registry);
    auto* viewer = host.activeArea();
    auto* nodes = host.splitArea(*viewer, Qt::Horizontal, "bloom.nodes");
    auto* timeline = host.splitArea(*nodes, Qt::Vertical, "bloom.timeline");
    show(host);
    expectations.expect(host.joinAreas(*timeline, *nodes), "Join Up over Nodes");
    settle();
    const auto saved = host.saveLayoutState();
    const auto root = QJsonDocument::fromJson(saved).object().value("root").toObject();
    const auto children = root.value("children").toArray();
    expectations.expect(root.value("type") == "split" && children.size() == 2 &&
                            children.at(0).toObject().value("editor") == "bloom.viewer" &&
                            children.at(1).toObject().value("editor") == "bloom.timeline" &&
                            children.at(1).toObject().value("active").toBool(),
                        "the saved layout holds the joined arrangement, Timeline active");

    WorkspaceHost restored(registry);
    expectations.expect(restored.restoreLayoutState(saved) ==
                            WorkspaceLayoutRestoreResult::Restored,
                        "the joined layout restores");
    expectations.expect(restored.areaCount() == 2 && restored.activeArea() != nullptr &&
                            restored.activeArea()->areaId() == timeline->areaId() &&
                            restored.activeArea()->editorId() == "bloom.timeline",
                        "the restored layout round-trips unchanged");
    host.hide();
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    bloom::ui::kit::installKinetikTheme(application);
    Expectations expectations;
    testMaximizeHidesTheActiveHighlight(expectations);
    testAPresetSwitchLeavesOneHighlight(expectations);
    testTheDividerShowsAResizeCursor(expectations);
    testAllFourJoins(expectations);
    testAJoinInAWiderSplitGivesTheRoomToTheSurvivor(expectations);
    testJoinIsDisabledWhenASideIsSplit(expectations);
    testAJoinedLayoutPersists(expectations);
    return expectations.failures() == 0 ? 0 : 1;
}
