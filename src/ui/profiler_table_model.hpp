#pragma once

// A read-only table over the Profiler's current snapshot (#423): pools, tasks, helper processes
// and pressure events. The model holds no copy of the data; it asks its owner for the row count
// and for each cell, so a cell is formatted only when the view paints it. refresh() after a new
// sample signals a reset only when the row count changed and otherwise one dataChanged over the
// whole table, which repaints only the visible cells.

#include <QAbstractTableModel>
#include <QStringList>

#include <cstdint>
#include <functional>

class QTableView;

namespace bloom::ui {

class ProfilerTableModel final : public QAbstractTableModel {
    Q_OBJECT

  public:
    using RowCount = std::function<int()>;
    using Cell = std::function<QVariant(int row, int column, int role)>;

    // Bit c of `numericColumns` right-aligns column c (its header and its cells).
    ProfilerTableModel(QStringList headers, std::uint32_t numericColumns, RowCount rows, Cell cell,
                       QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role) const override;

    void refresh();

  private:
    [[nodiscard]] Qt::Alignment alignment(int column) const;

    QStringList headers_;
    std::uint32_t numericColumns_;
    RowCount rows_;
    Cell cell_;
    int shownRows_ = 0;
};

// A read-only Profiler table view: the kit item-view look at the ListRow pitch, no selection or
// focus, the first column stretching, and a height that fits every row so the panel scrolls
// instead of the table.
[[nodiscard]] QTableView* makeProfilerTable(ProfilerTableModel& model, const QString& objectName,
                                            const QString& accessibleName, QWidget* parent);
void fitProfilerTable(QTableView& table);

} // namespace bloom::ui
