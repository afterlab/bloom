// Review captures for the SVG import modes (#642): the chooser, Preferences > Import, the one-layer
// artwork in the whole window, the Timeline's context menu offering Break Apart SVG, and the window
// after Break Apart. Writes PNGs to the directory given as the first argument; the scale factor is
// the environment's QT_SCALE_FACTOR. Not a ctest.
#include "svg_import_mode_dialog.hpp"
#include "window_fixture.hpp"

#include <QContextMenuEvent>
#include <QDir>
#include <QListWidget>
#include <QMenu>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/theme.hpp>
#include "preferences_window.hpp"
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    bloom::ui::kit::installKinetikTheme(application);
    if (argc < 2) {
        std::cerr << "usage: bloom_svg_import_capture <output directory>\n";
        return 2;
    }
    const QString directory = QString::fromLocal8Bit(argv[1]);
    QDir().mkpath(directory);
    const QString ratio = application.devicePixelRatio() > 1.25 ? "dpr15" : "dpr1";
    using namespace bloom;
    const auto save = [&](QWidget& widget, const QString& name) {
        if (!widget.grab().save(directory + "/" + name + "-" + ratio + ".png"))
            throw std::runtime_error("could not write a capture");
    };
    const auto settle = [] {
        for (int pass = 0; pass < 6; ++pass) {
            QCoreApplication::processEvents();
            QTest::qWait(60);
        }
    };
    try {
        const auto svg = std::filesystem::temp_directory_path() / "logo.svg";
        std::ofstream(svg, std::ios::binary)
            << R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120"><defs><linearGradient id="g" x1="0" x2="1"><stop offset="0" stop-color="#ffcc33"/><stop offset="1" stop-color="#ee3377"/></linearGradient></defs><circle cx="60" cy="60" r="44" fill="url(#g)"/><rect x="110" y="26" width="72" height="68" rx="10" fill="#3377dd"/><path d="M120 80 L146 40 L172 80 Z" fill="#ffffff"/></svg>)svg";
        ui::test::WindowFixture fixture([&](ui::test::WindowFixture& f) {
            commands::Transaction transaction("Import SVG", f.session.snapshot().revision());
            transaction
                .emplace<commands::AddSvgArtwork>(f.session.compositionId(), nullptr, svg, "logo", true,
                                                  commands::SvgImportMode::Layer)
                .withProjectDirectory(svg.parent_path());
            if (!f.session.executeTransaction(std::move(transaction)).succeeded())
                throw std::runtime_error("the SVG import was refused");
            f.session.selectLayer(f.session.composition()->graph().layerOutputs().front().layerId);
        });
        auto& window = *fixture.window;
        if (fixture.session.undoLabel() == QStringLiteral("Arrange sample graph"))
            static_cast<void>(fixture.session.undo());
        window.resize(1700, 1000);
        settle();

        for (const int index : {0, 1}) {
            ui::SvgImportModeDialog dialog(2, commands::kDefaultSvgImportMode, window);
            dialog.choices()->setCurrentIndex(index);
            dialog.show();
            settle();
            save(dialog, index == 0 ? "chooser-one-layer" : "chooser-shapes");
        }
        {
            ui::PreferencesWindow preferences(window, ui::ApplicationPreferences{});
            if (auto* rail = preferences.findChild<QListWidget*>(QStringLiteral("preferencesCategoryRail")))
                for (int row = 0; row < rail->count(); ++row)
                    if (rail->item(row)->text() == QStringLiteral("Import"))
                        rail->setCurrentRow(row);
            preferences.show();
            settle();
            save(preferences, "preferences-import");
        }
        save(window, "window-one-layer");
        if (auto* stack = window.findChild<ui::TimelineLayerStack*>()) {
            const QPoint point(stack->width() / 2, ui::kTimelineRowHeight / 2);
            QContextMenuEvent event(QContextMenuEvent::Mouse, point, stack->mapToGlobal(point));
            QApplication::sendEvent(stack, &event);
            settle();
            if (auto* menu = stack->findChild<QMenu*>(QStringLiteral("timelineLayerContextMenu"))) {
                save(*menu, "timeline-context-menu");
                menu->hide();
            }
        }
        const auto layer = fixture.session.composition()->graph().layerOutputs().front().layerId;
        fixture.assets->breakApartSvg(fixture.session.compositionId(), layer);
        for (int pass = 0; pass < 200 && fixture.assets->busy(); ++pass)
            QTest::qWait(25);
        settle();
        save(window, "window-after-break-apart");
        std::filesystem::remove(svg);
    } catch (const std::exception& error) {
        std::cerr << "capture failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
