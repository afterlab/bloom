#pragma once

#include <QPointer>
#include <QRect>
#include <QString>
#include <QWidget>

#include <vector>

class QCloseEvent;
class QStackedWidget;

namespace bloom::ui {

class EditorRegistry;
class WorkspaceHost;

// #536: where a secondary window sits. Logical (device-independent) virtual-desktop coordinates
// of the normal, un-maximized frame, the screen it was on, and its full-screen/maximized state.
struct WorkspaceWindowPlacement final {
    QString screen;
    QRect geometry;
    bool fullScreen = false;
    bool maximized = false;
    friend bool operator==(const WorkspaceWindowPlacement&,
                           const WorkspaceWindowPlacement&) = default;
};

// One screen as the placement rule sees it.
struct WorkspaceScreen final {
    QString name;
    QRect geometry;
    QRect available;
};

struct ResolvedWindowPlacement final {
    // Index into the screens the rule was given.
    int screen = 0;
    QRect geometry;
    // The saved monitor is gone, or the saved frame is no longer on any screen: the window was
    // brought onto a visible screen.
    bool relocated = false;
};

// The missing-monitor rule. A saved frame keeps its place on its own screen (found by name, else
// whichever screen it still overlaps), shrunk to that screen's available area and moved fully
// onto it. A frame on no visible screen opens centred on `primary`, shrunk to fit. Pure: no
// QScreen, so it is testable with any monitor arrangement.
[[nodiscard]] ResolvedWindowPlacement
resolveWindowPlacement(const WorkspaceWindowPlacement& saved,
                       const std::vector<WorkspaceScreen>& screens, int primary);

// False where clients cannot position their windows (Wayland): there the compositor places a
// window, and Bloom restores its size, its monitor (as a hint) and its full-screen state.
[[nodiscard]] bool platformPositionsWindows();

// #536: a secondary workspace window, made by Window > New Window or by tearing a panel off. It
// shares the application's session -- the same project, playhead, selection, active composition,
// undo stack and commands -- and hosts its own split-area tree of any editors. It has no menu bar
// and no toolbar; the main window keeps those, and its shortcuts reach this window too. A Viewer
// in it keeps its own Display / View (kIndependentDisplay). Closing it closes only it.
class WorkspaceWindow final : public QWidget {
    Q_OBJECT

  public:
    WorkspaceWindow(const EditorRegistry& registry, QString windowId = {},
                    QWidget* parent = nullptr);
    ~WorkspaceWindow() override;

    [[nodiscard]] WorkspaceHost* workspaceHost() const noexcept { return host_; }
    [[nodiscard]] const QString& windowId() const noexcept { return windowId_; }

    // F11, and on Windows a double-click on the title: borderless full screen on this window's
    // own monitor, and back.
    void toggleFullScreen();

    [[nodiscard]] WorkspaceWindowPlacement placement() const;
    // Shows the window where `placement` (resolved against the live screens) puts it.
    void showWithPlacement(const WorkspaceWindowPlacement& placement);

    // The main window's read-only placeholder: the workspace is not shown while the project is
    // read-only.
    void setReadOnly(bool readOnly);

    // Application shutdown: native surfaces have already retired, so a close is accepted at once.
    void setClosingForShutdown() { closingForShutdown_ = true; }

  signals:
    // The window was closed by the artist and is about to be deleted.
    void closed(bloom::ui::WorkspaceWindow* window);
    // The window became the active window.
    void activated(bloom::ui::WorkspaceWindow* window);

  protected:
    void closeEvent(QCloseEvent* event) override;
    bool event(QEvent* event) override;
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
#endif

  private:
    QString windowId_;
    QStackedWidget* stack_ = nullptr;
    WorkspaceHost* host_ = nullptr;
    QWidget* readOnlyPage_ = nullptr;
    bool closing_ = false;
    bool closingForShutdown_ = false;
};

} // namespace bloom::ui
