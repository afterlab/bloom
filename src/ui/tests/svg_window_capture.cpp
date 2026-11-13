// A real-window capture of the whole application with an imported SVG (#343): the Viewer, the
// Timeline with the imported layers, the Node editor with the Path nodes and Properties. Writes
// svg-window-<name>.png at 2000 logical pixels wide to the directory given as the first argument,
// from the SVG file given as the second. Not a ctest: it needs a corpus file.
#include "window_fixture.hpp"

#include <QDir>
#include <QImage>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/workspace_presets.hpp>

#include <iostream>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    bloom::ui::kit::installAfterlabTheme(application);
    if (argc < 3) {
        std::cerr << "usage: bloom_svg_window_capture <output directory> <file.svg>\n";
        return 2;
    }
    const QString directory = QString::fromLocal8Bit(argv[1]);
    const std::filesystem::path svg(argv[2]);
    QDir().mkpath(directory);
    using namespace bloom;
    try {
        ui::test::WindowFixture fixture([&](ui::test::WindowFixture& f) {
            commands::Transaction transaction("Import SVG", f.session.snapshot().revision());
            // As the desktop import does: the source's locator is made relative to the project's
            // folder (here the working directory, an unsaved project's base).
            transaction
                .emplace<commands::AddSvgArtwork>(f.session.compositionId(), nullptr, svg,
                                                  svg.stem().string(), true, commands::SvgImportMode::Shapes)
                .withProjectDirectory(std::filesystem::current_path());
            if (!f.session.executeTransaction(std::move(transaction)).succeeded())
                throw std::runtime_error("the SVG import was refused");
            const auto& outputs = f.session.composition()->graph().layerOutputs();
            if (!outputs.empty())
                f.session.selectLayer(outputs.front().layerId);
        });
        auto& window = *fixture.window;
        // The fixture re-arranges every graph it builds; undo that, so the nodes stay where the
        // import laid them (a grid per group).
        if (fixture.session.undoLabel() == QStringLiteral("Arrange sample graph"))
            static_cast<void>(fixture.session.undo());
        window.resize(2000, 1200);
        for (int pass = 0; pass < 8; ++pass) {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents();
            QCoreApplication::processEvents();
        }
        const auto name = QString::fromStdString(svg.stem().string());
        const auto settle = [] {
            for (int pass = 0; pass < 6; ++pass) {
                QCoreApplication::processEvents();
                QCoreApplication::sendPostedEvents();
                QCoreApplication::processEvents();
            }
        };
        // The imported group's layer open in the Timeline.
        if (auto* stack = window.findChild<ui::TimelineLayerStack*>()) {
            const auto& outputs = fixture.session.composition()->graph().layerOutputs();
            if (!outputs.empty())
                Q_EMIT stack->expansionRequested(outputs.front().layerId);
        }
        settle();
        if (!window.grab().save(directory + "/svg-window-" + name + ".png")) {
            std::cerr << "could not write the capture\n";
            return 1;
        }
        // Every built-in workspace, so the node editor can be seen at a useful size.
        for (const auto& preset : window.workspacePresets()->builtInPresets()) {
            window.workspacePresets()->apply(preset.name);
            settle();
            window.grab().save(directory + "/svg-window-" + name + "-" + preset.name.toLower() + ".png");
        }
        // The Node workspace at 100 percent: the fitted view above shows the whole graph as a strip.
        window.workspacePresets()->apply(QStringLiteral("Node"));
        settle();
        if (auto* nodes = window.findChild<ui::NodeGraphicsView*>()) {
            nodes->zoomToActualSize();
            settle();
            window.grab().save(directory + "/svg-window-" + name + "-node-100.png");
        }
    } catch (const std::exception& error) {
        std::cerr << "capture failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
