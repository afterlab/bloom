#include <bloom/ui/workspace_window.hpp>

#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/workspace_host.hpp>

#include <QCloseEvent>
#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QStackedWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cstdint>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace bloom::ui {
namespace {

std::int64_t overlapArea(const QRect& a, const QRect& b) {
    const QRect shared = a.intersected(b);
    return shared.isEmpty() ? 0 : std::int64_t{shared.width()} * shared.height();
}

// `frame` shrunk to `area` and moved fully inside it.
QRect fitInside(QRect frame, const QRect& area) {
    frame.setWidth(std::min(frame.width(), area.width()));
    frame.setHeight(std::min(frame.height(), area.height()));
    frame.moveLeft(std::clamp(frame.left(), area.left(), area.left() + area.width() - frame.width()));
    frame.moveTop(std::clamp(frame.top(), area.top(), area.top() + area.height() - frame.height()));
    return frame;
}

// A new window's frame when nothing was saved: half the main window's authored size.
QSize defaultWindowSize() {
    return {kit::px(kit::Size::MainWindowWidth) / 2, kit::px(kit::Size::MainWindowHeight) / 2};
}

} // namespace

ResolvedWindowPlacement resolveWindowPlacement(const WorkspaceWindowPlacement& saved,
                                               const std::vector<WorkspaceScreen>& screens,
                                               const int primary) {
    if (screens.empty())
        return {.screen = 0, .geometry = saved.geometry, .relocated = false};
    const int count = static_cast<int>(screens.size());
    const auto at = [&screens](const int index) -> const WorkspaceScreen& {
        return screens[static_cast<std::size_t>(index)];
    };
    const int fallback = std::clamp(primary, 0, count - 1);
    const QRect frame = saved.geometry.isValid() && !saved.geometry.isEmpty()
                            ? saved.geometry
                            : QRect(at(fallback).available.topLeft(), defaultWindowSize());

    int named = -1;
    for (int index = 0; index < count; ++index)
        if (!saved.screen.isEmpty() && at(index).name == saved.screen)
            named = index;
    int chosen = named >= 0 && overlapArea(frame, at(named).available) > 0 ? named : -1;
    if (chosen < 0) {
        // The saved monitor is gone (or the frame left it): keep the frame on whichever visible
        // screen it still overlaps most.
        std::int64_t best = 0;
        for (int index = 0; index < count; ++index) {
            const auto shared = overlapArea(frame, at(index).available);
            if (shared > best) {
                best = shared;
                chosen = index;
            }
        }
    }
    if (chosen >= 0)
        return {.screen = chosen,
                .geometry = fitInside(frame, at(chosen).available),
                .relocated = named < 0 && !saved.screen.isEmpty()};

    // On no visible screen at all: centred on the primary screen.
    const QRect& area = at(fallback).available;
    QRect placed(QPoint{}, frame.size().boundedTo(area.size()));
    placed.moveCenter(area.center());
    return {.screen = fallback, .geometry = fitInside(placed, area), .relocated = true};
}

bool platformPositionsWindows() {
    return !QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
}

WorkspaceWindow::WorkspaceWindow(const EditorRegistry& registry, QString windowId, QWidget* parent)
    : QWidget(parent, Qt::Window), windowId_(std::move(windowId)) {
    if (QUuid(windowId_).isNull())
        windowId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    setObjectName(QStringLiteral("bloomWorkspaceWindow"));
    // Every Viewer here keeps its own Display / View, so a calibrated reference monitor can show
    // a different display transform than the UI monitor.
    setProperty(ViewerEditor::kIndependentDisplayProperty, true);
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(defaultWindowSize());

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    stack_ = new QStackedWidget(this);
    host_ = new WorkspaceHost(registry, stack_);
    stack_->addWidget(host_);
    readOnlyPage_ = new kit::KLabel(tr("This project is read-only. The main window shows why."),
                                    stack_);
    readOnlyPage_->setObjectName(QStringLiteral("workspaceWindowReadOnlyPage"));
    static_cast<kit::KLabel*>(readOnlyPage_)->setAlignment(Qt::AlignCenter);
    stack_->addWidget(readOnlyPage_);
    stack_->setCurrentWidget(host_);
    layout->addWidget(stack_);
}

WorkspaceWindow::~WorkspaceWindow() = default;

void WorkspaceWindow::toggleFullScreen() {
    if (isFullScreen())
        showNormal();
    else
        showFullScreen();
}

WorkspaceWindowPlacement WorkspaceWindow::placement() const {
    const bool fullScreen = isFullScreen();
    const bool maximized = isMaximized();
    const QRect frame = fullScreen || maximized ? normalGeometry() : geometry();
    return {.screen = screen() != nullptr ? screen()->name() : QString{},
            .geometry = frame.isValid() ? frame : geometry(),
            .fullScreen = fullScreen,
            .maximized = maximized};
}

void WorkspaceWindow::showWithPlacement(const WorkspaceWindowPlacement& placement) {
    std::vector<WorkspaceScreen> screens;
    const auto live = QGuiApplication::screens();
    int primary = 0;
    for (int index = 0; index < live.size(); ++index) {
        screens.push_back({live[index]->name(), live[index]->geometry(),
                           live[index]->availableGeometry()});
        if (live[index] == QGuiApplication::primaryScreen())
            primary = index;
    }
    const auto resolved = resolveWindowPlacement(placement, screens, primary);
    if (resolved.screen >= 0 && resolved.screen < live.size()) {
        // The screen goes to the platform window before it is shown: Wayland compositors may honour
        // it as a hint (full screen on that output), every other platform places the window there.
        create();
        if (auto* handle = windowHandle())
            handle->setScreen(live[resolved.screen]);
    }
    if (platformPositionsWindows())
        setGeometry(resolved.geometry);
    else
        resize(resolved.geometry.size());
    if (placement.fullScreen)
        showFullScreen();
    else if (placement.maximized)
        showMaximized();
    else
        show();
}

void WorkspaceWindow::setReadOnly(const bool readOnly) {
    stack_->setCurrentWidget(readOnly ? readOnlyPage_ : static_cast<QWidget*>(host_));
}

void WorkspaceWindow::closeEvent(QCloseEvent* event) {
    if (closingForShutdown_) {
        event->accept();
        return;
    }
    event->ignore();
    if (closing_)
        return;
    closing_ = true;
    // Live native surfaces retire before the window's tree goes away; a CPU-only tree closes at
    // once.
    const QPointer<WorkspaceWindow> self(this);
    host_->closeAll([self](const bool closed) {
        if (self == nullptr)
            return;
        if (!closed) {
            self->closing_ = false;
            return;
        }
        emit self->closed(self.data());
        self->hide();
        self->deleteLater();
    });
}

bool WorkspaceWindow::event(QEvent* event) {
    if (event->type() == QEvent::WindowActivate)
        emit activated(this);
    return QWidget::event(event);
}

#ifdef Q_OS_WIN
bool WorkspaceWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    // A double-click on the native title bar makes this window full screen, as F11 does.
    if (eventType == "windows_generic_MSG" && message != nullptr) {
        const auto* msg = static_cast<const MSG*>(message);
        if (msg->message == WM_NCLBUTTONDBLCLK && msg->wParam == HTCAPTION) {
            toggleFullScreen();
            if (result != nullptr)
                *result = 0;
            return true;
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}
#endif

} // namespace bloom::ui
