#pragma once

// #638: the card widget of Color Matrix, drawn the same on the node card and in Properties: the
// matrix as a grid of numbers, a row for each result channel and a column for each channel it is
// made from. Only the rows and columns the chosen size reads are drawn: 3 x 3 has no alpha row and
// no alpha column, 4 x 4 has both.
//
// Each cell is the element's own value: it reads the document (a key at the playhead, a live drag,
// "Linked" for a driven element) and writes through the session, so dragging a number is one undo
// step. With a preset chosen the node does not read the elements, and the grid says so and is not
// editable.
//
// The elements' own rows, with their keyframe diamonds and sockets, stay behind the grid and open
// with its Values button, as a Grade wheel's do.

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <array>
#include <string>
#include <string_view>

class QAction;

namespace bloom::ui {

namespace kit {
class KButton;
class KLabel;
class KValueField;
} // namespace kit

class ColorMatrixCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    ColorMatrixCardWidget(CompositionSession& session, document::NodeId node, bool card,
                          QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] QSize sizeHint() const override;

    // The grid's side now: 3 or 4.
    [[nodiscard]] int side() const noexcept { return side_; }
    // The cell of result channel `row` made from channel `column` (0 red to 3 alpha).
    [[nodiscard]] kit::KValueField* cell(int row, int column) const;
    [[nodiscard]] kit::KButton* valuesButton() const noexcept { return values_; }
    void setValuesExpanded(bool expanded);
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct Cell final {
        std::string role;
        kit::KValueField* field = nullptr;
        QAction* key = nullptr;
    };
    void place();
    [[nodiscard]] bool elementRole(std::string_view role) const;

    CompositionSession& session_;
    document::NodeId node_;
    std::array<Cell, 16> cells_{};
    std::array<kit::KLabel*, 4> columnLabels_{};
    std::array<kit::KLabel*, 4> rowLabels_{};
    kit::KLabel* note_ = nullptr;
    kit::KButton* values_ = nullptr;
    int side_ = 3;
    bool preset_ = false;
    bool expanded_ = false;
};

} // namespace bloom::ui
