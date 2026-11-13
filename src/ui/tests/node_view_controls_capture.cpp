// Review captures of the node editor's view controls (#695, #721): the footer with its zoom
// dropdown and selection count, and the minimap corner with its zoom row.
// A tool, not a test:
//   bloom_node_view_controls_capture <directory> <1|1.5> <label>
// writes <label>-editor-, <label>-minimap- and <label>-footer-<dpr1|dpr15>.png.
#include "window_fixture.hpp"

#include <QDir>
#include <QImage>
#include <bloom/commands/operations.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/application_preferences.hpp>

#include <iostream>
#include <set>

namespace {
using namespace bloom;
using namespace bloom::ui;

// Delivers the queued events (layout, repaint requests); a grab renders synchronously after this.
void settle() {
    QCoreApplication::sendPostedEvents();
    QApplication::processEvents();
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
    const QString label = QString::fromLocal8Bit(argv[3]);
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
    const QString stem = directory + QLatin1Char('/') + label;
    // Zoomed in so the viewport rectangle is smaller than the graph, with one node selected so the
    // footer shows its count.
    view->zoomToPercent(200);
    if (const auto* shown = session.composition();
        shown != nullptr && !shown->graph().nodes().empty()) {
        const auto node = shown->graph().nodes().front().id;
        session.selectNodes({node}, node);
    }
    settle();
    const QImage whole = area->grab().toImage();
    ok = save(whole, stem + QStringLiteral("-editor-") + ratio + QStringLiteral(".png")) && ok;
    // The minimap corner, cropped from the same picture.
    const qreal scale = whole.devicePixelRatio();
    const QSize corner(static_cast<int>(2 * kit::px(kit::Size::NodeMinimapWidth) * scale),
                       static_cast<int>(2 * kit::px(kit::Size::NodeMinimapHeight) * scale));
    ok = save(whole.copy(whole.width() - corner.width(), whole.height() - corner.height(),
                         corner.width(), corner.height()),
              stem + QStringLiteral("-minimap-") + ratio + QStringLiteral(".png")) &&
         ok;
    // The footer alone.
    if (auto* footer = editor->footerWidgetForTest())
        ok = save(footer->grab().toImage(),
                  stem + QStringLiteral("-footer-") + ratio + QStringLiteral(".png")) &&
             ok;
    return ok ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: bloom_node_view_controls_capture <directory> <1|1.5> <label>\n";
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
