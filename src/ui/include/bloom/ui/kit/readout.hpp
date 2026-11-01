#pragma once

#include <bloom/ui/kit/metrics.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QString>
#include <QWidget>

#include <cstdint>
#include <optional>
#include <vector>

namespace bloom::ui::kit {

// Read-only label/value rows for live numbers (#423), one Size::PropertyRow apart: a Muted label
// (with an optional series swatch, so a row doubles as a chart legend), the value right-aligned
// in a fixed column -- numbers in the Value role, so a column of them stays aligned; a text value
// too wide for the column runs on into the note's room -- and an optional UiSmall note
// after it (an age, a rate, a share). A grid wider than two Size::ReadoutColumnWidth columns flows
// its rows into two columns.
//
// A row is set to a number in a unit, to text, or to absent. Numbers are formatted only when the
// row paints, so updating a row that is scrolled out of view costs a store and nothing else, and
// a value change repaints that row alone. Rows are declared once; setting a value never relays the
// grid out (only showing or hiding a row does).
class KReadoutGrid final : public QWidget {
    Q_OBJECT

  public:
    explicit KReadoutGrid(QWidget* parent = nullptr);

    int addRow(const QString& label, std::optional<Color> swatch = std::nullopt);
    void setLabel(int row, const QString& label);
    void setSwatch(int row, std::optional<Color> swatch);

    void setValue(int row, double value, MetricUnit unit, Color tone = Color::Foreground);
    void setText(int row, const QString& text, Color tone = Color::Foreground);
    // "Unavailable" in Faint: the metric is not measured here, which is not a zero.
    void setAbsent(int row);
    // The note after the value; an empty note clears it.
    void setNote(int row, const QString& note, Color tone = Color::Muted);
    void setNoteValue(int row, double value, MetricUnit unit, Color tone = Color::Muted);
    // "3.2 s old" -- in Warn once `stale`.
    void setAge(int row, std::int64_t ageNs, bool stale);
    void setRowVisible(int row, bool visible);

    [[nodiscard]] int rowCount() const noexcept { return static_cast<int>(rows_.size()); }
    [[nodiscard]] int visibleRowCount() const noexcept;
    [[nodiscard]] int columnCount() const;
    [[nodiscard]] QString labelText(int row) const;
    [[nodiscard]] QString valueText(int row) const;
    [[nodiscard]] QString noteText(int row) const;
    [[nodiscard]] bool isAbsent(int row) const;
    [[nodiscard]] Color valueTone(int row) const;
    [[nodiscard]] Color noteTone(int row) const;

    [[nodiscard]] bool hasHeightForWidth() const override { return true; }
    [[nodiscard]] int heightForWidth(int width) const override;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    enum class Kind : std::uint8_t { Absent, Number, Text };
    struct Part {
        Kind kind = Kind::Absent;
        double number = 0.0;
        MetricUnit unit = MetricUnit::Count;
        QString text;
        Color tone = Color::Foreground;
    };
    struct Row {
        QString label;
        std::optional<Color> swatch;
        Part value;
        Part note;
        bool visible = true;
    };
    [[nodiscard]] static QString partText(const Part& part);
    [[nodiscard]] int columnsFor(int width) const;
    [[nodiscard]] QRect rowRect(int row) const;
    void repaintRow(int row);

    std::vector<Row> rows_;
};

} // namespace bloom::ui::kit
