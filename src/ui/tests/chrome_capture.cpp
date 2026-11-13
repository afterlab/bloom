// Real-window captures of the window chrome (lane C2): the status bar idle, busy and narrow, the
// context bar, each workspace preset's whole window and the open Workspace dropdown. Writes PNGs to
// the directory given as the first argument (or BLOOM_GRAMMAR_ARTIFACT_DIR), suffixed with the
// device pixel ratio.
#include "window_fixture.hpp"

#include <QContextMenuEvent>
#include <QDir>
#include <QImage>
#include <QMenu>
#include <QSplitter>
#include <QSplitterHandle>
#include <bloom/ui/context_bar.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/dropdown_popup.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/window_status_bar.hpp>
#include <bloom/ui/workspace_host.hpp>
#include <bloom/ui/workspace_presets.hpp>

#include <iostream>

namespace {
using namespace bloom::ui;

QString directory;
QString suffix;

void save(const QPixmap& pixmap, const QString& name) {
    if (!pixmap.save(directory + QLatin1Char('/') + name + QLatin1Char('-') + suffix +
                     QStringLiteral(".png")))
        std::cerr << "could not write " << name.toStdString() << '\n';
}

void settle() {
    // Deliver everything already queued (layout, polish, deferred updates) until it is quiet.
    for (int pass = 0; pass < 4; ++pass) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}

EditorArea* areaFor(WorkspaceHost& host, const std::string& editorId) {
    for (auto* area : host.findChildren<EditorArea*>())
        if (area->editorId() == editorId)
            return area;
    return nullptr;
}

// Right-clicks the divider before `index` in `splitter` and grabs the Join menu it opens.
void captureDividerMenu(WorkspaceHost& host, QSplitter* splitter, const int index,
                        const QString& name) {
    auto* handle = splitter != nullptr ? splitter->handle(index) : nullptr;
    if (handle == nullptr) {
        std::cerr << "no divider for " << name.toStdString() << '\n';
        return;
    }
    const QPoint local(handle->width() / 2, handle->height() / 2);
    QContextMenuEvent event(QContextMenuEvent::Mouse, local, handle->mapToGlobal(local));
    QCoreApplication::sendEvent(handle, &event);
    settle();
    if (auto* menu = host.findChild<QMenu*>(QStringLiteral("workspaceDividerMenu"))) {
        save(menu->grab(), name);
        menu->close();
    }
    settle();
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installAfterlabTheme(application);
    directory =
        argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR);
    QDir().mkpath(directory);
    suffix = QGuiApplication::primaryScreen()->devicePixelRatio() == 1.0 ? QStringLiteral("dpr1")
                                                                         : QStringLiteral("dpr15");
    try {
        test::WindowFixture fixture;
        auto& window = *fixture.window;
        auto* status = window.statusStrip();
        auto* presets = window.workspacePresets();
        // Status bar: idle, then busy (an export notice and a RAM preview run), then narrow.
        settle();
        save(status->grab(), QStringLiteral("status-idle"));
        status->setPersistentMessage(QStringLiteral("Exporting Shot010 · frame 120 of 386"));
        fixture.preview->beginRamPreviewProgress(386);
        fixture.preview->setRamPreviewProgress(120, 120, 120);
        settle();
        save(status->grab(), QStringLiteral("status-busy"));
        // The activity line shows the persistent notice first; show the RAM preview run alone too.
        status->setPersistentMessage({});
        settle();
        save(status->grab(), QStringLiteral("status-busy-ram-preview"));
        fixture.preview->endRamPreviewProgress();
        status->setPersistentMessage({});
        const auto size = window.size();
        window.resize(900, size.height());
        settle();
        save(status->grab(), QStringLiteral("status-narrow"));
        window.resize(size);
        settle();
        // Context bar with the Select tool (no path selected: the handle strip is hidden).
        save(window.contextBar()->grab(), QStringLiteral("context-bar-select"));
        // Each preset's whole window.
        for (const auto& preset : presets->builtInPresets()) {
            presets->apply(preset.name);
            settle();
            save(window.grab(), QStringLiteral("preset-") + preset.name.toLower());
        }
        // Panels: one highlight after a preset round trip from a focused Assets panel, none while a
        // panel is maximized and back on the focused one after restoring, and the divider's Join
        // menu (enabled between two single panels, disabled where a side is split).
        presets->apply(QStringLiteral("Default"));
        settle();
        auto& host = *window.workspaceHost();
        host.setActiveArea(areaFor(host, "bloom.assets"));
        presets->apply(QStringLiteral("Export"));
        settle();
        presets->apply(QStringLiteral("Default"));
        settle();
        save(window.grab(), QStringLiteral("panels-after-preset-switch"));
        host.setActiveArea(areaFor(host, "bloom.viewer"));
        host.toggleMaximizeActiveArea();
        settle();
        save(window.grab(), QStringLiteral("panel-maximized"));
        host.toggleMaximizeActiveArea();
        settle();
        save(window.grab(), QStringLiteral("panel-restored"));
        if (auto* assets = areaFor(host, "bloom.assets")) {
            auto* row = qobject_cast<QSplitter*>(assets->parentWidget());
            captureDividerMenu(host, row, row != nullptr ? row->indexOf(assets) + 1 : -1,
                               QStringLiteral("divider-menu-side-by-side"));
        }
        if (auto* timeline = areaFor(host, "bloom.timeline")) {
            auto* column = qobject_cast<QSplitter*>(timeline->parentWidget());
            captureDividerMenu(host, column, column != nullptr ? column->indexOf(timeline) : -1,
                               QStringLiteral("divider-menu-split-side-disabled"));
        }
        presets->apply(QStringLiteral("Default"));
        settle();
        presets->saveCurrentAs(QStringLiteral("My layout"));
        settle();
        // The open Workspace dropdown, with a saved layout and the actions.
        auto* field = window.contextBar()->workspaceDropdownForTest();
        field->showPopup();
        settle();
        QPixmap popup = field->popup() != nullptr ? field->popup()->grab() : QPixmap{};
        save(popup, QStringLiteral("workspace-dropdown-popup"));
        save(window.contextBar()->grab(), QStringLiteral("context-bar-workspace"));
        field->hidePopup();
        presets->removeUser(QStringLiteral("My layout"));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
