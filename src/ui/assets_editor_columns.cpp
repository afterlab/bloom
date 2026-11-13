// The Assets column controller: which columns show, their order and the sort, kept as a per-user
// setting (never project data), and their widths, kept with the panel's own state in the workspace
// layout (#692). Every column is resized by dragging its header edge. Until the user drags one,
// Name takes what the other columns leave and every other shown column fits its content.
#include "assets_editor_internal.hpp"
#include <QAction>
#include <QEvent>
#include <QHeaderView>
#include <QJsonObject>
#include <QMenu>
#include <QSettings>
#include <QTreeWidgetItemIterator>
#include <QVariant>
#include <algorithm>
#include <bloom/ui/kit/tokens.hpp>

namespace bloom::ui::assets {

QString columnTitle(const int column) {
    switch (column) {
    case NameColumn:
        return QObject::tr("Name");
    case TypeColumn:
        return QObject::tr("Type");
    case DimensionsColumn:
        return QObject::tr("Dimensions");
    case DurationColumn:
        return QObject::tr("Duration");
    case FrameRateColumn:
        return QObject::tr("Frame Rate");
    case FileSizeColumn:
        return QObject::tr("File Size");
    case TagsColumn:
        return QObject::tr("Tags");
    case LocationColumn:
        return QObject::tr("File Location");
    default:
        return {};
    }
}

ColumnController::ColumnController(QTreeWidget& tree) : QObject(&tree), tree_(tree) {
    tree_.setColumnCount(ColumnCount);
    QStringList titles;
    for (int column = 0; column < ColumnCount; ++column)
        titles.push_back(columnTitle(column));
    tree_.setHeaderLabels(titles);
    auto* header = tree_.header();
    header->setObjectName(QStringLiteral("assetsTreeHeader"));
    header->setSectionsMovable(true);
    header->setFirstSectionMovable(false);
    header->setStretchLastSection(false);
    header->setHighlightSections(false);
    // #692: every column is dragged by its header edge, Name included. Name is given what the
    // other columns leave (fitName) until the user sizes it.
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setContextMenuPolicy(Qt::CustomContextMenu);
    header->setToolTip(
        QObject::tr("Click to sort, drag to resize or reorder; right-click to choose columns"));
    for (int column = TypeColumn; column < ColumnCount; ++column)
        header->setSectionHidden(column, !defaultColumnVisible(column));
    tree_.sortByColumn(NameColumn, Qt::AscendingOrder);
    connect(header, &QHeaderView::sectionResized, this, [this](const int column, int, int) {
        // A width the user dragged stays (#692: Name's too). Only a resize made with the mouse
        // on the header is the user's; the view's own (a column shown or hidden) is not.
        if (fitting_ || restoring_ || !dragging_ || tree_.header()->isSectionHidden(column))
            return;
        sized_.insert(column);
        if (column != NameColumn)
            fitName();
    });
    header->viewport()->installEventFilter(this);
    // Name follows the panel's width until the user sizes it.
    tree_.viewport()->installEventFilter(this);
    connect(header, &QHeaderView::sortIndicatorChanged, this, [this] { save(); });
    connect(header, &QHeaderView::sectionMoved, this, [this] { save(); });
}

bool ColumnController::eventFilter(QObject* watched, QEvent* event) {
    if (watched == tree_.viewport() && event->type() == QEvent::Resize)
        fitName();
    if (watched == tree_.header()->viewport()) {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)
            dragging_ = true;
        else if (event->type() == QEvent::MouseButtonRelease)
            dragging_ = false;
    }
    return QObject::eventFilter(watched, event);
}

void ColumnController::restore() {
    QSettings settings;
    // #692: the widths belong to the panel's workspace state (restoreWidths), not to this
    // setting; every column starts fitted.
    sized_.clear();
    const auto state = settings.value(QString::fromLatin1(kHeaderStateKey)).toByteArray();
    auto* header = tree_.header();
    // Restoring re-emits the sort and resize signals; none of them is the user's, so nothing is
    // saved or marked as dragged while it runs.
    restoring_ = true;
    if (!state.isEmpty() && header->restoreState(state)) {
        // Name is never hidden and every column is dragged, whatever a saved state says.
        header->setSectionHidden(NameColumn, false);
        header->setSectionResizeMode(QHeaderView::Interactive);
        header->setStretchLastSection(false);
        tree_.setSortingEnabled(true);
    }
    restoring_ = false;
}

void ColumnController::save() const {
    if (restoring_)
        return;
    QSettings settings;
    settings.setValue(QString::fromLatin1(kHeaderStateKey), tree_.header()->saveState());
}

// #692: the widths the user dragged, by column, for the panel's state in the workspace layout.
// A column that is not named fits its content (Name: what the others leave).
QJsonObject ColumnController::saveWidths() const {
    QJsonObject widths;
    for (const int column : sized_)
        widths.insert(QString::number(column), tree_.header()->sectionSize(column));
    return widths;
}

void ColumnController::restoreWidths(const QJsonObject& widths) {
    sized_.clear();
    restoring_ = true;
    for (auto entry = widths.begin(); entry != widths.end(); ++entry) {
        bool ok = false;
        const int column = entry.key().toInt(&ok);
        const int width = entry.value().toInt();
        if (!ok || column < NameColumn || column >= ColumnCount ||
            width < tree_.header()->minimumSectionSize())
            continue;
        tree_.header()->resizeSection(column, width);
        sized_.insert(column);
    }
    restoring_ = false;
    fit();
}

void ColumnController::resetWidths() {
    sized_.clear();
    fit();
}

// Name takes what the other shown columns leave of the panel until the user sizes it.
void ColumnController::fitName() {
    if (sized_.contains(NameColumn) || fitting_ || restoring_)
        return;
    int others = 0;
    for (int column = TypeColumn; column < ColumnCount; ++column)
        if (!tree_.header()->isSectionHidden(column))
            others += tree_.header()->sectionSize(column);
    const int width =
        std::max(tree_.viewport()->width() - others, tree_.header()->minimumSectionSize());
    if (tree_.header()->sectionSize(NameColumn) == width)
        return;
    fitting_ = true;
    tree_.header()->resizeSection(NameColumn, width);
    fitting_ = false;
}

void ColumnController::setVisible(const int column, const bool visible) {
    if (column <= NameColumn || column >= ColumnCount)
        return;
    tree_.header()->setSectionHidden(column, !visible);
    if (visible && !sized_.contains(column))
        fitColumn(column);
    fitName();
    save();
}

void ColumnController::fill(QMenu& menu) {
    menu.clear();
    for (int index = TypeColumn; index < ColumnCount; ++index) {
        auto* action = menu.addAction(columnTitle(index));
        action->setObjectName(QStringLiteral("assetsColumnAction%1").arg(index));
        action->setCheckable(true);
        action->setChecked(!tree_.header()->isSectionHidden(index));
        connect(action, &QAction::toggled, this,
                [this, index](const bool visible) { setVisible(index, visible); });
    }
    // #692: back to fitted widths.
    menu.addSeparator();
    auto* reset = menu.addAction(QObject::tr("Reset Column Widths"));
    reset->setObjectName(QStringLiteral("assetsColumnResetWidthsAction"));
    reset->setEnabled(!sized_.isEmpty());
    connect(reset, &QAction::triggered, this, [this] { resetWidths(); });
}

void ColumnController::fitColumn(const int column) {
    if (column == NameColumn || tree_.header()->isSectionHidden(column))
        return;
    // The widest cell of every row, collapsed or scrolled away included (the view's own
    // sizeHintForColumn() reads only the rows on screen), or the title, whichever is wider; never
    // below one value cell, so an empty column still reads as a column.
    // One header padding each side, and one pixel for a fractional advance rounding down.
    const auto padding =
        kit::px(kit::Spacing::S) + kit::px(kit::Spacing::S) + kit::px(kit::Size::Hairline);
    const auto metrics = tree_.fontMetrics();
    int width = tree_.header()->fontMetrics().horizontalAdvance(columnTitle(column)) + padding;
    for (QTreeWidgetItemIterator it(&tree_); *it; ++it)
        width = std::max(width, metrics.horizontalAdvance((*it)->text(column)) + padding);
    width = std::max(width, kit::px(kit::Size::ValueCellMin));
    fitting_ = true;
    tree_.header()->resizeSection(column, width);
    fitting_ = false;
}

void ColumnController::fit() {
    for (int column = TypeColumn; column < ColumnCount; ++column)
        if (!sized_.contains(column))
            fitColumn(column);
    fitName();
}

} // namespace bloom::ui::assets
