// Real-window captures of the maximized Assets panel (lane P), at a narrow and a wide size: with
// nothing selected, a sequence selected, the nested folder open and the search open with a filter.
// Imports generated PNG sequences through the asset controller's worker, so the rows, File Size and
// thumbnails are the real ones. Writes PNGs to BLOOM_GRAMMAR_ARTIFACT_DIR (or the directory given
// as the first argument) and checks that the shown columns span the tree at every size.
#include "await_qt.hpp"
#include "window_fixture.hpp"
#include <QDir>
#include <QHeaderView>
#include <QImage>
#include <QLineEdit>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <iostream>

namespace {
using namespace bloom::ui;

EditorArea* assetsArea(MainWindow& window) {
    for (auto* area : window.findChildren<EditorArea*>())
        if (area->editorId() == "bloom.assets")
            return area;
    return nullptr;
}

void sizeArea(MainWindow& window, EditorArea& area, const QSize target) {
    for (int pass = 0; pass < 4; ++pass) {
        QApplication::processEvents();
        const QSize delta = target - area.size();
        if (delta.isNull())
            return;
        window.resize(window.size() + delta);
        // The resize is applied by the queued layout events: deliver them.
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}

QTreeWidgetItem* named(QTreeWidget& tree, const QString& name) {
    for (QTreeWidgetItemIterator it(&tree); *it; ++it)
        if ((*it)->text(0).startsWith(name))
            return *it;
    return nullptr;
}

int run(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomGrammarTests");
    app.setApplicationName("AssetsPanelCapture");
    kit::installAfterlabTheme(app);
    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                       : QStringLiteral(BLOOM_GRAMMAR_ARTIFACT_DIR);
    QDir().mkpath(directory);
    QTemporaryDir media;
    if (!media.isValid())
        return 1;
    // Five plates like the owner's project, at the same 2.39:1 frame, plus a still.
    QStringList firsts;
    const QStringList plates{"Shot010_plate", "BG_0101", "CH_0101", "SKY_0101", "UT_0101"};
    for (int plate = 0; plate < plates.size(); ++plate) {
        QImage frame(512, 214, QImage::Format_RGBA8888);
        for (int index = 1; index <= 12; ++index) {
            frame.fill(QColor::fromHsv((plate * 61 + index * 3) % 360, 90, 150));
            const auto path = media.filePath(
                QStringLiteral("%1.%2.png").arg(plates[plate]).arg(index, 4, 10, QLatin1Char('0')));
            if (!frame.save(path))
                return 1;
            if (index == 1)
                firsts.push_back(path);
        }
    }
    test::WindowFixture fixture;
    const auto ratio = fixture.window->devicePixelRatioF();
    const QString dpr = ratio == 1.0 ? QStringLiteral("dpr1") : QStringLiteral("dpr15");
    auto& session = fixture.session;
    fixture.assets->importFiles(firsts, std::nullopt);
    if (!bloom::test_support::awaitQtCondition("plate import finishes with five assets", [&] {
            return !fixture.assets->busy() && session.snapshot().project().assets().size() >= 5;
        }))
        return 1;
    // A second composition, and a folder holding the first plate.
    bloom::commands::Transaction organise("Organise", session.snapshot().revision());
    organise.emplace<bloom::commands::AddComposition>(
        "Shot010", bloom::document::CompositionFormat{}, bloom::core::RationalTime::fromInteger(16));
    organise.emplace<bloom::commands::CreateAssetFolder>("test");
    if (!session.executeTransaction(std::move(organise)).succeeded())
        return 1;
    const auto& project = session.snapshot().project();
    if (project.assetFolders().empty())
        return 1;
    bloom::document::AssetId first;
    for (const auto& asset : project.assets())
        if (asset.name.starts_with("Shot010"))
            first = asset.id;
    bloom::commands::Transaction move("Move", session.snapshot().revision());
    move.emplace<bloom::commands::MoveAssets>(std::vector{first}, project.assetFolders().front().id,
                                              std::size_t{0});
    if (!session.executeTransaction(std::move(move)).succeeded())
        return 1;
    if (!bloom::test_support::awaitQtCondition("every asset has a thumbnail", [&] {
            bool ready = !fixture.assets->busy();
            for (const auto& asset : session.snapshot().project().assets())
                ready = ready && !fixture.assets->thumbnail(asset.id).isNull();
            return ready;
        }))
        return 1;
    auto* area = assetsArea(*fixture.window);
    if (area == nullptr) {
        std::cerr << "no Assets area\n";
        return 1;
    }
    Q_EMIT area->maximizeRequested(area);
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    auto* editor = area->findChild<AssetsEditor*>();
    auto* tree = editor ? editor->findChild<QTreeWidget*>("assetsTree") : nullptr;
    auto* search = editor ? editor->findChild<QLineEdit*>("assetsSearchField") : nullptr;
    if (tree == nullptr || search == nullptr)
        return 1;
    int failures = 0;
    const auto capture = [&](const char* state) {
        for (const auto& [name, size] :
             {std::pair{"narrow", QSize(735, 800)}, std::pair{"wide", QSize(1550, 780)}}) {
            sizeArea(*fixture.window, *area, size);
            QCoreApplication::sendPostedEvents();
            QCoreApplication::processEvents();
            int spanned = 0;
            for (int column = 0; column < tree->columnCount(); ++column)
                if (!tree->header()->isSectionHidden(column))
                    spanned += tree->header()->sectionSize(column);
            if (spanned != tree->viewport()->width()) {
                std::cerr << state << ' ' << name << ": columns span " << spanned << " of "
                          << tree->viewport()->width() << '\n';
                ++failures;
            }
            const QString label = QStringLiteral("assets-after-%1-%2-%3")
                                      .arg(QString::fromLatin1(state), QString::fromLatin1(name),
                                           dpr);
            if (!area->grab().toImage().save(directory + QLatin1Char('/') + label +
                                             QStringLiteral(".png")))
                ++failures;
            std::cout << label.toStdString() << ' ' << area->width() << 'x' << area->height()
                      << '\n';
        }
    };
    auto* folder = named(*tree, QStringLiteral("test"));
    if (folder != nullptr)
        folder->setExpanded(false);
    tree->clearSelection();
    capture("none");
    tree->setCurrentItem(named(*tree, QStringLiteral("BG_0101")));
    capture("sequence");
    if (folder != nullptr)
        folder->setExpanded(true);
    tree->setCurrentItem(named(*tree, QStringLiteral("Shot010_plate")));
    capture("nested");
    tree->clearSelection();
    if (auto* toggle = editor->window()->findChild<QWidget*>(QStringLiteral("assetsSearchButton")))
        QTest::mouseClick(toggle, Qt::LeftButton);
    search->setText(QStringLiteral("0101"));
    tree->setFocus();
    capture("search");
    return failures + bloom::test_support::awaitFailureCount() == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
