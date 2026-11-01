#include "profiler_table_model.hpp"

#include <bloom/ui/kit/tokens.hpp>

#include <QHeaderView>
#include <QScrollBar>
#include <QTableView>

#include <utility>

namespace bloom::ui {

ProfilerTableModel::ProfilerTableModel(QStringList headers, const std::uint32_t numericColumns,
                                       RowCount rows, Cell cell, QObject* parent)
    : QAbstractTableModel(parent), headers_(std::move(headers)), numericColumns_(numericColumns),
      rows_(std::move(rows)), cell_(std::move(cell)) {}

Qt::Alignment ProfilerTableModel::alignment(const int column) const {
    const bool numeric = column >= 0 && column < 32 && (numericColumns_ >> column & 1U) != 0;
    return (numeric ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter;
}

int ProfilerTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : shownRows_;
}

int ProfilerTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(headers_.size());
}

QVariant ProfilerTableModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() >= shownRows_)
        return {};
    if (role == Qt::TextAlignmentRole)
        return QVariant::fromValue(alignment(index.column()));
    return cell_(index.row(), index.column(), role);
}

QVariant ProfilerTableModel::headerData(const int section, const Qt::Orientation orientation,
                                        const int role) const {
    if (orientation != Qt::Horizontal || section < 0 || section >= headers_.size())
        return {};
    if (role == Qt::DisplayRole)
        return headers_[section];
    if (role == Qt::TextAlignmentRole)
        return QVariant::fromValue(alignment(section));
    return {};
}

void ProfilerTableModel::refresh() {
    const int rows = rows_();
    if (rows != shownRows_) {
        beginResetModel();
        shownRows_ = rows;
        endResetModel();
        return;
    }
    if (rows > 0)
        Q_EMIT dataChanged(index(0, 0), index(rows - 1, columnCount() - 1));
}

QTableView* makeProfilerTable(ProfilerTableModel& model, const QString& objectName,
                              const QString& accessibleName, QWidget* parent) {
    auto* table = new QTableView(parent);
    table->setObjectName(objectName);
    table->setAccessibleName(accessibleName);
    table->setModel(&model);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setFocusPolicy(Qt::NoFocus);
    table->setAlternatingRowColors(true);
    table->setSortingEnabled(false);
    table->setWordWrap(false);
    table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table->verticalHeader()->setDefaultSectionSize(kit::px(kit::Size::ListRow));
    auto* header = table->horizontalHeader();
    header->setHighlightSections(false);
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(0, QHeaderView::Stretch);
    QObject::connect(&model, &QAbstractItemModel::modelReset, table,
                     [table] { fitProfilerTable(*table); });
    QObject::connect(table->horizontalScrollBar(), &QScrollBar::rangeChanged, table,
                     [table] { fitProfilerTable(*table); });
    fitProfilerTable(*table);
    return table;
}

void fitProfilerTable(QTableView& table) {
    const auto* model = table.model();
    const int rows = model != nullptr ? model->rowCount() : 0;
    const auto* bar = table.horizontalScrollBar();
    const bool scrolls = bar != nullptr && bar->maximum() > bar->minimum();
    const int height = table.frameWidth() * 2 + table.horizontalHeader()->sizeHint().height() +
                       rows * kit::px(kit::Size::ListRow) +
                       (scrolls ? kit::px(kit::Size::ScrollBarTrack) : 0);
    if (table.height() != height)
        table.setFixedHeight(height);
}

} // namespace bloom::ui
