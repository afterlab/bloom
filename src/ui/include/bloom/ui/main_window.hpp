#pragma once

#include <bloom/ui/workspace_window.hpp>

#include <QList>
#include <QMainWindow>
#include <QPoint>
#include <QPointer>
#include <QTimer>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class QAction;
class QCloseEvent;
class QLabel;
class QMenu;
class QMenuBar;
class QJsonArray;
class QJsonValue;
class QSettings;
class QStackedWidget;

namespace bloom::runtime {
class OperationCache;
}

namespace bloom::media::cache {
class MediaDiskCache;
} // namespace bloom::media::cache

namespace bloom::ui {

struct ApplicationPreferences;
class AccelerationStatusProvider;
class CachePurgeController;
class CompositionPreviewController;
class McpServerAdapter;
class CompositionSession;
class EditorRegistry;
class ExportQueueAdapter;
class LicensesWindow;
class ProjectHost;
class RamPreviewController;
class PlaybackController;
class WindowStatusBar;
class ContextBar;
class WorkspaceHost;
class WorkspacePresets;
class ViewerEditor;
class EditorArea;
class EditorNativeSurface;
class WorkspaceWindow;
enum class PanelDockEdge : std::uint8_t;
enum class WorkspaceLayoutRestoreResult;

// Historical custom-chrome setting (task U2, issue #118). Superseded by task C1 ("let OS handle
// the native window chrome for now"): MainWindow now always builds native (server-side) window
// decorations and never reads or writes this setting. ChromeMode and the two free functions below
// stay compiled and independently tested (main_window_chrome_tests.cpp's own
// chromeModeFromSettings coverage) alongside kit::TitleBar (kit_title_bar_tests.cpp) and
// FramelessEdgeResizer (frameless_window_support_tests.cpp) purely so a future custom-chrome/CSD
// return has working, tested pieces to build on -- nothing in the live application constructs or
// consults any of them today.
enum class ChromeMode : std::uint8_t {
    Custom,
    Native,
};

// "appearance/chrome" = "custom" (default) | "native". Any other or missing value reads as
// Custom. No in-app settings UI writes this key anymore (task C1 removed the View menu's "Use
// Native Window Frame" toggle); kept only for the round trip these two functions have always had.
[[nodiscard]] ChromeMode chromeModeFromSettings(const QSettings& settings);
void setChromeModeInSettings(QSettings& settings, ChromeMode mode);

class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    // `ramPreview` is the Composition menu's RAM Preview command (task PERF1, item 3), the same
    // controller the Timeline's transport button reaches. Null leaves the menu item present and
    // disabled, which is what a window built without one should show.
    // `previewController` feeds the window status bar's preview cells (task VIEW-1) -- the colour
    // state, readiness, dropped frames and cache progress. Null builds the strip with those cells
    // empty rather than absent, which is what a window without a preview pipeline should show.
    // `operationCache` feeds the window status bar's combined cache cell with hit/miss/retained-
    // byte statistics for the evaluator's shared operation cache, appended beside the RAM preview
    // text. Null leaves that cell showing only the RAM preview account.
    // `mediaDiskCache` feeds the window status bar's disk-cache cell (docs/architecture/media-io.md
    // "Disk cache"). Null reports the cell as off.
    // `cachePurgeController` owns the Edit | Purge… commands' asynchronous, off-UI purge. Null
    // leaves both commands present but reporting that no purge task system is available, rather
    // than doing the heavy media clear on the UI thread. `accelerationStatus` feeds the Preferences
    // window's read-only Performance page. Null reports the CPU-only truth. Borrowed and must
    // outlive the window.
    MainWindow(const EditorRegistry& editorRegistry, CompositionSession& compositionSession,
               ProjectHost& projectHost,
               RamPreviewController* ramPreview = nullptr,
               CompositionPreviewController* previewController = nullptr, QWidget* parent = nullptr,
               PlaybackController* playbackController = nullptr,
               runtime::OperationCache* operationCache = nullptr,
               media::cache::MediaDiskCache* mediaDiskCache = nullptr,
               const AccelerationStatusProvider* accelerationStatus = nullptr,
               CachePurgeController* cachePurgeController = nullptr);
    ~MainWindow() override;

    // ADR 0033: the application's export queue. Enables File > Add to Export Queue (Ctrl+M) and
    // shows the Export panel whenever a job is added. Null leaves the item present and disabled
    // with the reason. Borrowed; must outlive the window.
    void setExportQueue(ExportQueueAdapter* adapter);
    [[nodiscard]] QAction* addToExportQueueAction() const noexcept { return addToExportQueueAction_; }

    [[nodiscard]] WorkspaceHost* workspaceHost() const noexcept;

    // --- #536: secondary workspace windows ----------------------------------------------------
    // Window > New Window: another full workspace window on the same session, with no menu bar
    // and no toolbar. `layoutState` (WorkspaceHost::saveLayoutState()) is its arrangement, a
    // single Viewer when empty; `placement` where it opens, beside this window when absent.
    WorkspaceWindow* openWorkspaceWindow(const QByteArray& layoutState = {},
                                         const std::optional<WorkspaceWindowPlacement>& placement = {});
    [[nodiscard]] QList<WorkspaceWindow*> workspaceWindows() const;
    // The main workspace first, then every secondary window's.
    [[nodiscard]] QList<WorkspaceHost*> workspaceHosts() const;
    // The workspace whose window was activated last: the one panel commands act on, and the one
    // that paints the single active highlight.
    [[nodiscard]] WorkspaceHost* activeWorkspaceHost() const noexcept;
    // Tear-off: the panel leaves for a new window of its own (at `globalPosition` when given).
    // Returns its area there, or nullptr (it is the only panel of its window, a panel is
    // maximized, or a native surface is still retiring). One undo-free UI action.
    EditorArea* tearOffPanel(EditorArea& area, QPoint globalPosition = {});
    // Re-dock: the panel moves next to `target` (any window), taking the half of it on `edge`. A
    // secondary window left without panels closes. Returns the panel's new area, or nullptr.
    EditorArea* movePanel(EditorArea& source, EditorArea& target, PanelDockEdge edge);
    // Every live editor native surface in every window, for the shutdown retirement gate.
    [[nodiscard]] std::vector<EditorNativeSurface*> liveNativeSurfaces() const;
    void releasePackedFramesForShutdown();
    [[nodiscard]] QAction* newWindowAction() const noexcept { return newWindowAction_; }
    // The Viewer the status bar and context bar follow: the active area's, else the last active
    // one, else the first visible. Null with no visible Viewer.
    [[nodiscard]] ViewerEditor* displayedViewer();
    [[nodiscard]] WorkspaceLayoutRestoreResult restoreApplicationState(QSettings& settings);
    void saveApplicationState(QSettings& settings) const;
    // Called only after the shutdown coordinator has observed runtime quiescence. Earlier close
    // requests must leave the window alive while unsaved decisions and worker draining finish.
    void completeShutdown();
    // Presentation-level read-only surface (task R1, issue #74): true exactly when the central
    // QStackedWidget's current page is the read-only placeholder instead of the editor workspace
    // -- i.e. the live ProjectHost content kind is PreservedReadOnly. Exposed so an offscreen test
    // can assert the switch without depending on QWidget::isVisible(), which only reports
    // correctly once the top-level window itself has been shown.
    [[nodiscard]] bool isShowingReadOnlyPlaceholder() const noexcept;
    // The one persistent reporting surface (task VIEW-1). Exposed so a test can read what the
    // window is currently saying without grabbing pixels.
    [[nodiscard]] WindowStatusBar* statusStrip() const noexcept { return statusStrip_; }
    // The application's menu bar: this window's own on Linux and Windows, the one global menu
    // bar on macOS (#536). It hides QMainWindow::menuBar(), which on macOS would create a second,
    // empty menu bar for this window and show that instead of the global one.
    [[nodiscard]] QMenuBar* menuBar() const;
    // ADR 0031: the fixed contextual bar under the menu bar, and Window > Context Bar.
    [[nodiscard]] ContextBar* contextBar() const noexcept { return contextBar_; }
    [[nodiscard]] QAction* contextBarAction() const noexcept { return contextBarAction_; }
    // The workspace presets behind the context bar's Workspace field.
    [[nodiscard]] WorkspacePresets* workspacePresets() const noexcept { return workspacePresets_; }
    // The application's MCP server adapter (ADR 0034): enables the status bar indicator, and reveals the MCP Server panel when either asks. Borrowed; null removes it.
    void setMcpServer(McpServerAdapter* adapter);
    // Test-only seam (same terms as the other test/diagnostic accessors): applies committed
    // preferences exactly as the Preferences dialog's approved path does, without presenting the
    // modal dialog, so a test can assert the View action mirror and the open-editor propagation.
    void applyCommittedPreferencesForTest(const ApplicationPreferences& preferences);
    // The Preferences window's OK, without the window (MCP preferences.set): saves `preferences`,
    // applies them to the open editors and emits preferencesChanged().
    void commitPreferences(const ApplicationPreferences& preferences);
    // Edit | Purge… command bodies (also MCP cache.purge). Neither touches project truth, the
    // command stack, or undo. False when a purge is already running.
    bool purgePreviewCache();
    bool purgeMediaCache();

  signals:
    void shutdownRequested();
    // Emitted after the Preferences window commits and persists a new value, so the composition
    // root can re-apply the preferences it owns (for example the audio engine).
    void preferencesChanged();
    // The Preferences Sounds page asked to hear a sample at its unsaved volume.
    void soundTestRequested(const bloom::ui::ApplicationPreferences& draft);
    // #536: a secondary workspace window opened (New Window, tear-off or a restored session).
    void workspaceWindowOpened(bloom::ui::WorkspaceWindow* window);

  protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void createChrome();
    void createMenus(QMenuBar& menuBar);
    // macOS: makes the global menu bar's menus children of this window (see the definition).
    void adoptMenus();
    void createFileMenu(QMenu& fileMenu);
    void createCompositionMenu(QMenu& compositionMenu);
    void createViewMenu(QMenu& viewMenu);
    void revealMcpServerPanel();
    void createHelpMenu(QMenu& helpMenu);
    void updatePurgeActions();
    void createEditorLayout(const EditorRegistry& editorRegistry);
    void connectWorkspaceHost(WorkspaceHost& host);
    void setActiveWorkspaceHost(WorkspaceHost* host);
    [[nodiscard]] WorkspaceHost* hostOf(const QWidget* widget) const;
    [[nodiscard]] EditorArea* findAreaAnywhere(const QString& areaId) const;
    [[nodiscard]] QList<QAction*> applicationActions() const;
    [[nodiscard]] bool belongsToWorkspace(const QWidget* widget) const;
    void restoreWorkspaceWindows(const QJsonValue& windows);
    [[nodiscard]] QJsonArray saveWorkspaceWindows() const;
    [[nodiscard]] WorkspaceWindowPlacement placementBesideMainWindow() const;
    void createCentralStack();
    QWidget* createReadOnlyPlaceholderPage();
    void createWorkspaceActions();
    void resetCompositingLayout(bool persist = false);
    void updateEditActions();
    void updateWorkspaceActions();
    void updateDisplayedViewer();
    void scheduleDisplayedViewerUpdate();
    QPointer<ViewerEditor> lastActiveViewer_;
    QTimer displayedViewerUpdateTimer_;
    void updateFileActions();
    void updateExportQueueActions();
    void revealExportPanel();
    void updateWindowTitle();
    void updateContentSurface();
    void updateCompositionActions();
    void toggleFullScreen();
    void showProjectColorSettings();
    void showPreferences();
    void applyPreferencesToOpenEditors(const ApplicationPreferences& preferences);

    const EditorRegistry& editorRegistry_;
    CompositionSession& compositionSession_;
    ProjectHost& projectHost_;
    std::vector<QPointer<WorkspaceWindow>> workspaceWindows_;
    QPointer<WorkspaceHost> activeHost_;
    QAction* newWindowAction_ = nullptr;
    // macOS: the one parentless menu bar every window shares (Qt's documented route to one global
    // menu bar for all windows). Elsewhere the menu bar is this window's own.
    std::unique_ptr<QMenuBar> globalMenuBar_;
    // Borrowed, may be null; the command itself is owned by the application composition root.
    RamPreviewController* ramPreview_ = nullptr;
    // Borrowed, may be null; owned by the application composition root, and read only by the
    // window status bar's preview cells.
    CompositionPreviewController* previewController_ = nullptr;
    PlaybackController* playbackController_ = nullptr;
    // Borrowed, may be null; owned by the application composition root.
    runtime::OperationCache* operationCache_ = nullptr;
    // Borrowed, may be null; owned by the application composition root.
    media::cache::MediaDiskCache* mediaDiskCache_ = nullptr;
    // Borrowed, may be null; owned by the application composition root.
    CachePurgeController* cachePurgeController_ = nullptr;
    // Borrowed, may be null; null means the Performance page reports the CPU-only truth.
    const AccelerationStatusProvider* accelerationStatus_ = nullptr;
    QMenuBar* menuBar_ = nullptr;
    QMenu* windowMenu_ = nullptr;
    QMenu* viewMenu_ = nullptr;
    QMenu* compositionMenu_ = nullptr;
    QMenu* purgeMenu_ = nullptr;
    QStackedWidget* centralStack_ = nullptr;
    // A kit strip under the workspace, NOT QMainWindow::statusBar(): that brings its own chrome,
    // size grip and item model, none of which the Afterlab language wants.
    WindowStatusBar* statusStrip_ = nullptr;
    ContextBar* contextBar_ = nullptr;
    QAction* contextBarAction_ = nullptr;
    QPointer<McpServerAdapter> mcpServer_;
    WorkspaceHost* workspaceHost_ = nullptr;
    WorkspacePresets* workspacePresets_ = nullptr;
    QWidget* readOnlyPlaceholderPage_ = nullptr;
    QLabel* readOnlyPlaceholderFileNameLabel_ = nullptr;
    QLabel* readOnlyPlaceholderBodyLabel_ = nullptr;
    QAction* newProjectAction_ = nullptr;
    QAction* openProjectAction_ = nullptr;
    QAction* cancelOpenAction_ = nullptr;
    QAction* saveProjectAction_ = nullptr;
    QAction* saveProjectAsAction_ = nullptr;
    QAction* saveProjectCopyAction_ = nullptr;
    QAction* projectColorSettingsAction_ = nullptr;
    QAction* preferencesAction_ = nullptr;
    QAction* addToExportQueueAction_ = nullptr;
    ExportQueueAdapter* exportQueue_ = nullptr;
    QAction* quitAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* splitRightAction_ = nullptr;
    QAction* splitDownAction_ = nullptr;
    QAction* closeAreaAction_ = nullptr;
    QAction* maximizeAreaAction_ = nullptr;
    QAction* ramPreviewAction_ = nullptr;
    QAction* cancelRamPreviewAction_ = nullptr;
    QAction* newCompositionAction_ = nullptr;
    QAction* duplicateCompositionAction_ = nullptr;
    QAction* breakApartSvgAction_ = nullptr;
    QAction* renameCompositionAction_ = nullptr;
    QAction* compositionSettingsAction_ = nullptr;
    QAction* deleteCompositionAction_ = nullptr;
    QAction* viewFullScreenAction_ = nullptr;
    QAction* viewMaximizePanelAction_ = nullptr;
    QAction* viewAudioEnabledAction_ = nullptr;
    QAction* reportIssueAction_ = nullptr;
    QAction* openSourceLicensesAction_ = nullptr;
    QPointer<LicensesWindow> licensesWindow_;
    QAction* purgePreviewCacheAction_ = nullptr;
    QAction* purgeMediaCacheAction_ = nullptr;
    bool workspaceLayoutWritable_ = true;
    bool shutdownRequested_ = false;
    bool shutdownComplete_ = false;
};

} // namespace bloom::ui
