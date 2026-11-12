// Review captures of the node editor's view controls (#695): the footer and the minimap.
// A tool, not a test:
//   bloom_node_view_controls_capture <directory> <1|1.5> <before|after>
// `before` reconstructs the footer the controls replaced (the zoom dropdown with Fit and the
// presets) over the same editor, so the two states can be compared; `after` is the real footer.
#include "window_fixture.hpp"

#include <QDir>
#include <QImage>
#include <bloom/commands/operations.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/application_preferences.hpp>

#include <iostream>
#include <set>

namespace {
using namespace bloom;
using namespace bloom::ui;

void settle(const int ms = 80) {
    QApplication::processEvents();
    QTest::qWait(ms);
}

bool save(const QImage& image, const QString& path) {
    if (image.isNull() || !image.save(path)) {
        std::cerr << "could not write " << path.toStdString() << '\n';
        return false;
    }
    std::cout << path.toStdString() << ' ' << image.width() << 'x' << image.height() << '\n';
    return true;
}

int capture(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomGrammarTests");
    app.setApplicationName("NodeViewControlsCapture");
    kit::installAfterlabTheme(app);
    const QString directory = QString::fromLocal8Bit(argv[1]);
    const QString ratio = QString::fromLatin1(argv[2]) == QStringLiteral("1")
                              ? QStringLiteral("dpr1")
                              : QStringLiteral("dpr15");
    const bool before = QString::fromLatin1(argv[3]) == QStringLiteral("before");
    QDir().mkpath(directory);
    test::WindowFixture fixture;
    auto& session = fixture.session;
    const auto composition = session.compositionId();
    for (const auto* name : {"Background", "Plate", "Title", "Lower third", "Logo", "Glow"}) {
        commands::Transaction add("Add Layer", session.snapshot().revision());
        add.emplace<commands::AddSolidLayer>(composition, name, core::Color4d{0.3, 0.5, 0.7, 1.0});
        (void)session.executeNodeTransaction(std::move(add));
    }
    EditorArea* area = nullptr;
    for (auto* candidate : fixture.window->findChildren<EditorArea*>())
        if (candidate->editorId() == "bloom.nodes")
            area = candidate;
    if (area == nullptr)
        return 1;
    Q_EMIT area->maximizeRequested(area);
    settle();
    auto* editor = area->findChild<NodeGraphEditor*>();
    if (editor == nullptr)
        return 1;
    fixture.clearInteraction();
    session.clearSelection();
    settle();
    auto* view = editor->graphView();
    view->frameGraph();
    settle();
    bool ok = true;
    if (before) {
        ApplicationPreferences preferences;
        preferences.nodeShowZoomControls = false;
        preferences.nodeShowMinimap = false;
        editor->applyApplicationPreferences(preferences);
        settle();
        auto* host = editor->footerWidgetForTest();
        auto* dropdown = new kit::KDropdown(host);
        dropdown->setControlSize(kit::KDropdown::ControlSize::Dense);
        dropdown->addItem(QStringLiteral("Fit"), 0);
        for (const int percent : {25, 50, 100, 200, 400})
            dropdown->addItem(QStringLiteral("%1%").arg(percent), percent);
        dropdown->move(kit::px(kit::Spacing::ChromePadding), kit::px(kit::Spacing::ChromePadding));
        dropdown->show();
        settle();
        ok = save(area->grab().toImage(), directory + QStringLiteral("/695-before-footer-") + ratio + QStringLiteral(".png"));
        return ok ? 0 : 1;
    }
    ok = save(area->grab().toImage(), directory + QStringLiteral("/695-after-footer-full-") + ratio +
                                          QStringLiteral(".png")) && ok;
    // Zoomed in so the viewport rectangle is smaller than the graph.
    view->zoomToPercent(200);
    settle();
    ok = save(area->grab().toImage(),
              directory + QStringLiteral("/695-after-minimap-") + ratio + QStringLiteral(".png")) &&
         ok;
    // The footer alone, cropped.
    if (auto* footer = editor->footerWidgetForTest())
        ok = save(footer->grab().toImage(), directory + QStringLiteral("/695-after-footer-") +
                                                ratio + QStringLiteral(".png")) &&
             ok;
    return ok ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: bloom_node_view_controls_capture <directory> <1|1.5> <before|after>\n";
        return 2;
    }
    qputenv("QT_SCALE_FACTOR", argv[2]);
    try {
        return capture(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
