// The Assets panel layout (lane P): the table fills the panel at every width, the default and
// optional columns, monochrome row glyphs, the nested-row geometry, the fixed summary strip, the
// collapsible search and the worker-measured File Size column.
#include "assets_editor_internal.hpp"
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QImage>
#include <QMenu>
#include <QMouseEvent>
#include <QJsonObject>
#include <QLineEdit>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidgetItemIterator>
#include <bloom/commands/asset_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/mnemonic_style.hpp>
#include <bloom/ui/kit/painting.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace bloom;
namespace assets = ui::assets;
namespace kit = ui::kit;

int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

document::AssetRecord sequenceAsset(const std::uint64_t id, const std::string& name,
                                    const std::optional<document::AssetFolderId> folder = {}) {
    document::AssetRecord asset;
    asset.id = document::AssetId::fromRaw(id);
    asset.name = name;
    asset.kind = document::AssetKind::Sequence;
    asset.locator = {"file", "project-relative", name + ".0001.png",
                     "file:///media/" + name + ".0001.png"};
    asset.width = 2048;
    asset.height = 858;
    asset.manifest.pattern = name + ".####.png";
    asset.manifest.padding = 4;
    asset.manifest.first = 1;
    asset.manifest.last = 24;
    for (std::int64_t frame = 1; frame <= 24; ++frame)
        asset.manifest.members.push_back(
            {frame,
             {"file", "project-relative",
              name + "." + std::to_string(1000 + frame).substr(1) + ".png",
              "file:///media/" + name + ".png"},
             bloom::core::Sha256Digest{},
             std::nullopt});
    asset.folder = folder;
    return asset;
}

// A project with a folder holding one sequence, two sequences at the root and the composition.
struct Fixture {
    document::NewProject seed =
        document::makeNewProject("Panel", "Main", core::RationalTime::fromInteger(2));
    std::unique_ptr<document::Document> document;
    std::unique_ptr<commands::CommandStack> stack;
    std::unique_ptr<ui::CompositionSession> session;
    Fixture() {
        const auto folder = document::AssetFolderId::fromRaw(1);
        expect(seed.project.addAssetFolder({folder, "test", {}}), "folder fixture");
        expect(seed.project.addAsset(sequenceAsset(1, "Shot010_plate_0001", folder)) &&
                   seed.project.addAsset(sequenceAsset(2, "BG_0101")) &&
                   seed.project.addAsset(sequenceAsset(3, "Shot_0101")),
               "sequence fixtures");
        document = std::make_unique<document::Document>(std::move(seed.project));
        stack = std::make_unique<commands::CommandStack>(*document);
        session = std::make_unique<ui::CompositionSession>(*document, *stack,
                                                           seed.initialCompositionId);
    }
};

QTreeWidgetItem* named(QTreeWidget& tree, const QString& name) {
    for (QTreeWidgetItemIterator it(&tree); *it; ++it)
        if ((*it)->text(assets::NameColumn) == name)
            return *it;
    return nullptr;
}

QModelIndex indexOf(QTreeWidget& tree, QTreeWidgetItem* item) {
    if (item == nullptr)
        return {};
    if (item->parent() == nullptr)
        return tree.model()->index(tree.indexOfTopLevelItem(item), 0);
    return tree.model()->index(item->parent()->indexOfChild(item), 0,
                               indexOf(tree, item->parent()));
}

void settle() {
    for (int pass = 0; pass < 3; ++pass)
        QApplication::processEvents();
}

QImage grabViewport(QTreeWidget& tree) {
    settle();
    return tree.viewport()->grab().toImage();
}

// The ink a glyph was drawn in: the most opaque, brightest pixel of its box.
QColor glyphInk(const QImage& image, const QRect& box) {
    const qreal ratio = image.devicePixelRatio();
    QColor best;
    int bestValue = -1;
    for (int y = qRound(box.top() * ratio); y < qRound((box.bottom() + 1) * ratio); ++y)
        for (int x = qRound(box.left() * ratio); x < qRound((box.right() + 1) * ratio); ++x) {
            if (x < 0 || y < 0 || x >= image.width() || y >= image.height())
                continue;
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.value() > bestValue) {
                bestValue = pixel.value();
                best = pixel;
            }
        }
    return best;
}

bool grey(const QColor& value) {
    return std::abs(value.red() - value.green()) <= 2 && std::abs(value.green() - value.blue()) <= 2;
}

void tableFillsThePanel() {
    Fixture fixture;
    QSettings().remove(QString::fromLatin1(assets::kHeaderStateKey));
    QSettings().remove(QString::fromLatin1(assets::kSizedColumnsKey));
    ui::AssetsEditor editor(*fixture.session);
    editor.show();
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    expect(tree != nullptr, "assets tree");
    if (tree == nullptr)
        return;
    auto* header = tree->header();
    expect(header->sectionResizeMode(assets::NameColumn) == QHeaderView::Interactive,
           "#692: Name is resized by its header edge like every other column");
    expect(tree->headerItem()->text(assets::DimensionsColumn) == "Dimensions",
           "the W × H column is Dimensions");
    auto* bg = named(*tree, "BG_0101");
    for (const int width : {300, 600, 1500}) {
        editor.resize(width, 500);
        settle();
        int sections = 0;
        for (int column = 0; column < assets::ColumnCount; ++column)
            if (!header->isSectionHidden(column))
                sections += header->sectionSize(column);
        expect(sections == tree->viewport()->width(),
               ("the shown columns span the viewport exactly: Name takes the remainder (" +
                std::to_string(sections) + " of " + std::to_string(tree->viewport()->width()) +
                " at " + std::to_string(width) + ")")
                   .c_str());
        expect(header->sectionSize(assets::NameColumn) >
                   header->sectionSize(assets::TypeColumn),
               "Name keeps the widest column");
        // The selected row and an alternate row paint to the viewport's right edge.
        tree->clearSelection();
        bg->setSelected(true);
        const auto image = grabViewport(*tree);
        const QRect row = tree->visualRect(indexOf(*tree, bg));
        const qreal ratio = image.devicePixelRatio();
        const int edge = qRound((tree->viewport()->width() - 2) * ratio);
        const int middle = qRound(row.center().y() * ratio);
        const QColor right = image.pixelColor(edge, middle);
        const bool alternate = (row.top() / row.height()) % 2 == 1;
        const auto wash = kit::selectionWash(alternate ? kit::Color::Surface
                                                       : kit::Color::Background);
        expect(right == wash, "the selected row's wash reaches the viewport's right edge");
        expect(image.pixelColor(qRound(kit::px(kit::Size::SelectionEdge) / 2.0 * ratio),
                                middle) == kit::color(kit::Color::Accent),
               "the Accent edge sits at the row's left");
    }
    // Every row is one dense Assets row; Ui text keeps its kit size.
    expect(tree->visualRect(indexOf(*tree, bg)).height() == kit::px(kit::Size::AssetRow),
           "rows are one AssetRow tall");
}

void columnsAndPersistence() {
    Fixture fixture;
    QSettings().remove(QString::fromLatin1(assets::kHeaderStateKey));
    QSettings().remove(QString::fromLatin1(assets::kSizedColumnsKey));
    {
        ui::AssetsEditor editor(*fixture.session);
        auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
        auto* header = tree->header();
        QStringList shown;
        for (int column = 0; column < assets::ColumnCount; ++column)
            if (!header->isSectionHidden(column))
                shown.push_back(tree->headerItem()->text(column));
        expect(shown == QStringList({"Name", "Type", "Dimensions"}),
               "the default columns are Name, Type and Dimensions");
        auto* columns = editor.findChild<QMenu*>("assetsColumnMenu");
        QAction* duration = nullptr;
        for (auto* action : columns ? columns->actions() : QList<QAction*>{})
            if (action->objectName() ==
                QStringLiteral("assetsColumnAction%1").arg(assets::DurationColumn))
                duration = action;
        expect(duration != nullptr && !duration->isChecked(), "Duration is offered, unchecked");
        if (duration != nullptr)
            duration->setChecked(true);
        expect(!header->isSectionHidden(assets::DurationColumn), "toggling shows Duration");
        auto* item = named(*tree, "BG_0101");
        expect(item && item->text(assets::DurationColumn) == "0:00:01:00",
               "the Duration cell is the timecode alone");
        expect(item && item->toolTip(assets::DurationColumn).contains("(24 frames)"),
               "frames and rate are the Duration tooltip");
    }
    ui::AssetsEditor restored(*fixture.session);
    auto* header = restored.findChild<QTreeWidget*>("assetsTree")->header();
    expect(!header->isSectionHidden(assets::DurationColumn) &&
               header->sectionResizeMode(assets::NameColumn) == QHeaderView::Interactive,
           "column visibility persists per user and Name is still resizable");
    QSettings().remove(QString::fromLatin1(assets::kHeaderStateKey));
}

// #692: every column is resized by dragging its header edge, and the widths are the panel's own
// state, which the workspace layout keeps: a panel reopened with that state has them again.
void columnsResizeAndStayWithThePanel() {
    Fixture fixture;
    QSettings().remove(QString::fromLatin1(assets::kHeaderStateKey));
    QJsonObject state;
    std::map<int, int> dragged;
    std::map<int, int> fitted;
    {
        ui::AssetsEditor editor(*fixture.session);
        editor.resize(700, 500);
        editor.show();
        settle();
        auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
        expect(tree != nullptr, "assets tree");
        if (tree == nullptr)
            return;
        auto* header = tree->header();
        const auto drag = [&](const int column, const int delta) {
            const int edge = header->sectionViewportPosition(column) + header->sectionSize(column);
            const QPoint press(edge - 1, header->height() / 2);
            QTest::mousePress(header->viewport(), Qt::LeftButton, Qt::NoModifier, press);
            QTest::mouseMove(header->viewport(), press + QPoint(delta / 2, 0));
            QTest::mouseMove(header->viewport(), press + QPoint(delta, 0));
            QTest::mouseRelease(header->viewport(), Qt::LeftButton, Qt::NoModifier,
                                press + QPoint(delta, 0));
            settle();
        };
        for (int column = 0; column < assets::ColumnCount; ++column)
            if (!header->isSectionHidden(column))
                fitted[column] = header->sectionSize(column);
        expect(fitted.size() == 3, "Name, Type and Dimensions are shown");
        for (const auto& [column, before] : fitted) {
            const int delta = column == assets::NameColumn ? -60 : 40;
            drag(column, delta);
            const int after = header->sectionSize(column);
            expect(std::abs(after - (before + delta)) <= 2,
                   ("#692: dragging the header edge resizes " +
                    tree->headerItem()->text(column).toStdString() + " (" + std::to_string(before) +
                    " to " + std::to_string(after) + ")")
                       .c_str());
            dragged[column] = after;
        }
        for (const auto& [column, width] : dragged)
            expect(header->sectionSize(column) == width,
                   "#692: a dragged width stays while another column is dragged");
        editor.resize(900, 500);
        settle();
        for (const auto& [column, width] : dragged)
            expect(header->sectionSize(column) == width,
                   "#692: a dragged width stays when the panel is resized");
        state = editor.saveEditorSessionState();
        expect(state.value("columnWidths").toObject().size() == 3,
               "#692: the panel's state carries the three dragged widths");
    }
    // The panel reopened: fitted without its state, the dragged widths with it.
    ui::AssetsEditor reopened(*fixture.session);
    reopened.resize(700, 500);
    reopened.show();
    settle();
    auto* header = reopened.findChild<QTreeWidget*>("assetsTree")->header();
    for (const auto& [column, width] : fitted)
        expect(header->sectionSize(column) == width,
               "a panel opened without a state has fitted columns");
    reopened.restoreEditorSessionState(state);
    settle();
    for (const auto& [column, width] : dragged)
        expect(header->sectionSize(column) == width,
               ("#692: the reopened panel has the dragged width of column " + std::to_string(column) +
                " (" + std::to_string(header->sectionSize(column)) + ", expected " +
                std::to_string(width) + ")")
                   .c_str());
    expect(reopened.saveEditorSessionState() == state, "#692: and saves the same state again");
    // Reset Column Widths goes back to fitted columns.
    auto* columns = reopened.findChild<QMenu*>("assetsColumnMenu");
    if (columns != nullptr)
        emit columns->aboutToShow();
    auto* reset = columns != nullptr
                      ? columns->findChild<QAction*>("assetsColumnResetWidthsAction")
                      : nullptr;
    expect(reset != nullptr && reset->isEnabled(), "Reset Column Widths is offered");
    if (reset != nullptr)
        reset->trigger();
    settle();
    for (const auto& [column, width] : fitted)
        expect(header->sectionSize(column) == width, "Reset Column Widths fits the columns again");
    QSettings().remove(QString::fromLatin1(assets::kHeaderStateKey));
}

// #689: a double-click on the empty space below the rows opens Import, into the top level; a
// double-click on a row keeps its meaning and the header's is its own.
void doubleClickOnEmptySpaceImports() {
    Fixture fixture;
    QSettings().remove(QString::fromLatin1(assets::kHeaderStateKey));
    ui::AssetsEditor editor(*fixture.session);
    editor.resize(700, 600);
    editor.show();
    settle();
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    expect(tree != nullptr, "assets tree");
    if (tree == nullptr)
        return;
    std::vector<std::optional<document::AssetFolderId>> imports;
    editor.setImportRequestForTest(
        [&](const std::optional<document::AssetFolderId> folder) { imports.push_back(folder); });
    // The last row's bottom edge, then well below it.
    int bottom = 0;
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if (!(*it)->isHidden())
            bottom = std::max(bottom, tree->visualItemRect(*it).bottom());
    const QPoint empty(tree->viewport()->width() / 2, bottom + (tree->viewport()->height() - bottom) / 2);
    expect(bottom > 0 && empty.y() < tree->viewport()->height() && tree->itemAt(empty) == nullptr,
           "the panel has empty space below its rows");
    // With a folder's row selected, the empty space is still the top level.
    if (auto* folder = named(*tree, "test"))
        tree->setCurrentItem(folder);
    QTest::mouseDClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, empty);
    settle();
    expect(imports.size() == 1 && !imports.front().has_value(),
           "#689: a double-click on empty space opens Import into the top level");
    // A row's double-click is still the row's: the tree reports it and no import opens.
    int rowDoubleClicks = 0;
    const auto rowConnection = QObject::connect(tree, &QTreeWidget::itemDoubleClicked,
                                                [&rowDoubleClicks] { ++rowDoubleClicks; });
    int rows = 0;
    for (const char* name : {"BG_0101", "test", "Main"}) {
        auto* row = named(*tree, name);
        expect(row != nullptr, "a fixture row");
        if (row == nullptr)
            continue;
        ++rows;
        // The four events of a double-click, as the window system delivers them to the view.
        const QPointF at = tree->visualItemRect(row).center();
        const QPointF global = tree->viewport()->mapToGlobal(at);
        const auto send = [&](const QEvent::Type type, const Qt::MouseButtons buttons) {
            QMouseEvent event(type, at, global, Qt::LeftButton, buttons, Qt::NoModifier);
            QCoreApplication::sendEvent(tree->viewport(), &event);
        };
        send(QEvent::MouseButtonPress, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, Qt::NoButton);
        send(QEvent::MouseButtonDblClick, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, Qt::NoButton);
        settle();
    }
    QObject::disconnect(rowConnection);
    expect(imports.size() == 1 && rowDoubleClicks == rows && rows == 3,
           ("#689: a double-click on a footage, folder or composition row keeps its own meaning "
            "and opens no Import (" +
            std::to_string(rowDoubleClicks) + " of " + std::to_string(rows) + ")")
               .c_str());
    QTest::mouseDClick(tree->header()->viewport(), Qt::LeftButton, Qt::NoModifier,
                       QPoint(tree->header()->sectionViewportPosition(assets::TypeColumn) + 8,
                              tree->header()->height() / 2));
    settle();
    expect(imports.size() == 1, "#689: a double-click on the header does not open Import");
}

void monochromeGlyphsAndNesting() {
    Fixture fixture;
    QSettings().remove(QString::fromLatin1(assets::kHeaderStateKey));
    ui::AssetsEditor editor(*fixture.session);
    editor.resize(700, 500);
    editor.show();
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    auto* folder = named(*tree, "test");
    auto* child = named(*tree, "Shot010_plate_0001");
    auto* root = named(*tree, "BG_0101");
    auto* composition = named(*tree, "Main");
    expect(folder && child && root && composition, "fixture rows");
    if (!folder || !child || !root || !composition)
        return;
    folder->setExpanded(true);
    tree->clearSelection();
    settle();
    // Nesting: the child's glyph starts where its parent's name does; a root row has exactly one
    // caret column in front of its glyph, never an extra gutter.
    const QRect folderCell = tree->visualRect(indexOf(*tree, folder));
    const QRect childCell = tree->visualRect(indexOf(*tree, child));
    const QRect rootCell = tree->visualRect(indexOf(*tree, root));
    expect(assets::AssetRowDelegate::glyphRect(childCell).left() ==
               assets::AssetRowDelegate::textLeft(folderCell, assets::NameColumn),
           "a child's glyph x equals its parent's text x");
    expect(assets::AssetRowDelegate::glyphRect(rootCell).left() ==
               kit::px(kit::Size::AssetIndent),
           "a root row's glyph sits one caret column in");
    expect(childCell.left() - folderCell.left() == kit::px(kit::Size::AssetIndent),
           "one indent token per level");
    expect(folder->data(assets::NameColumn, assets::kSecondaryTextRole).toString() == "1" &&
               folder->text(assets::TypeColumn) == "Folder",
           "a folder row carries its muted child count and the Folder type");
    // Monochrome: each kind's glyph is drawn in the row's ink, never a per-kind colour.
    auto image = grabViewport(*tree);
    for (auto* item : {folder, child, root, composition}) {
        const auto ink = glyphInk(image, assets::AssetRowDelegate::glyphRect(
                                             tree->visualRect(indexOf(*tree, item))));
        expect(grey(ink) && ink.value() >= 0xF0, "an unselected glyph is the Foreground ink");
        expect(!item->data(assets::NameColumn, assets::kGlyphToneRole).isValid(),
               "a kind glyph carries no tone of its own");
    }
    root->setSelected(true);
    image = grabViewport(*tree);
    const auto selected =
        glyphInk(image, assets::AssetRowDelegate::glyphRect(tree->visualRect(indexOf(*tree, root))));
    expect(grey(selected) && selected == kit::color(kit::Color::OnAccent),
           "a selected row's glyph is OnAccent");
    expect(assets::AssetRowDelegate::glyphInk(indexOf(*tree, root), true) == kit::Color::OnAccent &&
               assets::AssetRowDelegate::glyphInk(indexOf(*tree, root), false) ==
                   kit::Color::Foreground,
           "the glyph ink follows the row");
}

void summaryStrip() {
    Fixture fixture;
    ui::AssetsEditor editor(*fixture.session);
    editor.resize(420, 500);
    editor.show();
    settle();
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    auto* strip = editor.findChild<QWidget*>("assetsSelectionHeader");
    auto* name = editor.findChild<kit::KLabel*>("assetsSelectionName");
    auto* kind = editor.findChild<kit::KLabel*>("assetsSelectionKind");
    expect(strip && name && kind, "summary strip parts");
    if (!strip || !name || !kind)
        return;
    const int height = kit::px(kit::Size::SummaryStrip);
    tree->clearSelection();
    settle();
    expect(strip->height() == height, "the strip keeps its height with nothing selected");
    expect(name->isHidden() && kind->text() == "4 items · 3 sequences · 1 composition · 1 folder",
           "no selection: one line of what the project holds");
    tree->setCurrentItem(named(*tree, "BG_0101"));
    settle();
    expect(strip->height() == height, "the strip keeps its height with one item");
    expect(kind->toolTip() == "Sequence · 2048 × 858 · 0:00:01:00 (24 frames), 24 fps" &&
               kind->text().startsWith("Sequence · 2048 × 858 · 0:00:01:00 · 24 f · 24 fps"),
           "one item: skimmable facts, the sentence in the tooltip");
    tree->selectAll();
    settle();
    expect(strip->height() == height, "the strip keeps its height with several items");
    expect(name->toolTip() == "5 items selected", "several: their count");
    // A long name elides; the whole name is the tooltip.
    commands::Transaction rename("Rename", fixture.session->snapshot().revision());
    rename.emplace<commands::RenameAsset>(
        document::AssetId::fromRaw(2),
        "An_extremely_long_plate_name_that_cannot_possibly_fit_in_a_narrow_panel_0101");
    expect(fixture.session->executeTransaction(std::move(rename)).succeeded(), "rename");
    editor.resize(320, 500);
    tree->clearSelection();
    tree->setCurrentItem(named(
        *tree, "An_extremely_long_plate_name_that_cannot_possibly_fit_in_a_narrow_panel_0101"));
    settle();
    expect(name->text() != name->toolTip() && name->text().endsWith(QChar(0x2026)) &&
               name->toolTip().endsWith("panel_0101"),
           "a long name elides and keeps the whole name in its tooltip");
    expect(strip->height() == height, "elision never grows the strip");
    // View > Show Info hides the strip, and the choice persists.
    auto* showInfo = editor.findChild<QAction*>("assetsShowInfoAction");
    expect(showInfo && showInfo->isChecked(), "Show Info starts on");
    if (showInfo != nullptr)
        showInfo->setChecked(false);
    expect(strip->isHidden(), "Show Info off hides the strip");
    {
        ui::AssetsEditor again(*fixture.session);
        expect(again.findChild<QWidget*>("assetsSelectionHeader")->isHidden(),
               "Show Info persists");
    }
    if (showInfo != nullptr)
        showInfo->setChecked(true);
}

void collapsibleSearch() {
    Fixture fixture;
    ui::AssetsEditor editor(*fixture.session);
    editor.resize(600, 500);
    editor.show();
    editor.activateWindow();
    expect(QTest::qWaitForWindowActive(&editor), "editor window active");
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    auto* bar = editor.findChild<QWidget*>("assetsSearchBar");
    auto* field = editor.findChild<QLineEdit*>("assetsSearchField");
    auto* count = editor.findChild<kit::KLabel*>("assetsSearchCount");
    auto* button = editor.findChild<kit::KIconButton*>("assetsSearchButton");
    expect(tree && bar && field && count && button, "search parts");
    if (!tree || !bar || !field || !count || !button)
        return;
    expect(bar->isHidden() && !button->isChecked(), "search starts collapsed");
    expect(button->toolTip().startsWith("Search ("), "the toggle names its shortcut");
    // Typing in the tree is type-ahead to an item, never search.
    tree->setFocus();
    QTest::keyClick(tree, Qt::Key_S);
    settle();
    expect(bar->isHidden() && tree->currentItem() &&
               tree->currentItem()->text(assets::NameColumn).startsWith("S"),
           "typing in the tree moves to a matching item");
    QTest::keyClick(tree, Qt::Key_F, Qt::ControlModifier);
    settle();
    expect(!bar->isHidden() && QApplication::focusWidget() == field && button->isChecked(),
           "Ctrl+F opens the search row and focuses it");
    QTest::keyClicks(field, "Shot");
    settle();
    expect(!count->isHidden() && count->text() == "2 of 4", "the match count names the matches");
    tree->setFocus();
    settle();
    expect(!bar->isHidden() && !count->isHidden(),
           "an active filter stays visible after focus leaves");
    field->setFocus();
    QTest::keyClick(field, Qt::Key_Escape);
    settle();
    expect(field->text().isEmpty() && !bar->isHidden(), "Escape clears the text first");
    QTest::keyClick(field, Qt::Key_Escape);
    settle();
    expect(bar->isHidden() && !button->isChecked() && QApplication::focusWidget() == tree,
           "a second Escape collapses the row and returns focus to the tree");
    button->click();
    settle();
    expect(!bar->isHidden() && button->isChecked(), "the toggle opens the row");
    button->click();
    settle();
    expect(bar->isHidden(), "the toggle closes an empty row");
    // Text set from anywhere opens the row, so a filter never applies out of sight.
    field->setText(QStringLiteral("tag:hero"));
    settle();
    expect(!bar->isHidden() && button->isChecked(), "a filter set while closed opens the row");
    button->click();
    settle();
    expect(!bar->isHidden(), "the toggle never closes a filtering row");
    field->clear();
}

void fileSizeColumn() {
    QTemporaryDir directory;
    expect(directory.isValid(), "temporary media directory");
    QImage image(32, 16, QImage::Format_RGBA8888);
    for (const auto* name : {"still.png", "clip.0001.png", "clip.0002.png"}) {
        image.fill(QString(name).contains("0002") ? Qt::black : Qt::white);
        expect(image.save(directory.filePath(name)), "write PNG fixture");
    }
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host(scheduler);
    ui::CompositionSession session(*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId());
    ui::TaskUiBridge bridge(scheduler);
    ui::AssetController controller(session, host, scheduler, bridge);
    ui::AssetsEditor editor(session);
    controller.importFiles(
        {directory.filePath("still.png"), directory.filePath("clip.0001.png")}, std::nullopt);
    QElapsedTimer timer;
    timer.start();
    const auto& project = [&]() -> const document::Project& {
        return session.snapshot().project();
    };
    const auto measured = [&] {
        return project().assets().size() == 2 &&
               std::ranges::all_of(project().assets(),
                                   [&](const auto& asset) { return controller.fileSize(asset.id).has_value(); });
    };
    while (!measured() && timer.elapsed() < 20000)
        QTest::qWait(10);
    expect(measured(), "the worker measures every imported source");
    const auto bytes = [&](const char* name) {
        return static_cast<std::uint64_t>(std::filesystem::file_size(
            std::filesystem::path(directory.filePath(name).toStdString())));
    };
    auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
    for (const auto& asset : project().assets()) {
        const auto expected = asset.kind == document::AssetKind::Sequence
                                  ? bytes("clip.0001.png") + bytes("clip.0002.png")
                                  : bytes("still.png");
        expect(controller.fileSize(asset.id) == expected,
               "File Size is the file's bytes, a sequence's members summed");
        auto* row = named(*tree, QString::fromStdString(asset.name));
        expect(row && row->text(assets::FileSizeColumn) == assets::fileSizeText(expected) &&
                   row->data(assets::FileSizeColumn, assets::kSortRole).toDouble() ==
                       static_cast<double>(expected),
               "the File Size cell shows and sorts by the bytes");
    }
    expect(tree->header()->isSectionHidden(assets::FileSizeColumn), "File Size is optional");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomAssetsPanelTests"));
    QApplication::setApplicationName(QStringLiteral("AssetsPanel"));
    QStandardPaths::setTestModeEnabled(true);
    // A settings file of its own, so the DPR variants of this test can run side by side.
    QTemporaryDir settingsDirectory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QApplication::setStyle(new kit::AltUnderlineProxyStyle());
    kit::installAfterlabTheme(application);
    tableFillsThePanel();
    columnsAndPersistence();
    columnsResizeAndStayWithThePanel();
    doubleClickOnEmptySpaceImports();
    monochromeGlyphsAndNesting();
    summaryStrip();
    collapsibleSearch();
    fileSizeColumn();
    return failures == 0 ? 0 : 1;
}
