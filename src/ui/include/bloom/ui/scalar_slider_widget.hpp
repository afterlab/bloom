#pragma once

// #501: the card widget of a one-value colour operation -- Exposure (stops), Offset and Multiply
// (a factor) -- drawn the same on the node card and in Properties: the value's row (its name, its
// number with its unit and its keyframe diamond, its socket beside it) and under it the grading
// slider that drags it, with a tick at neutral that a drag snaps to and ticks along its soft range
// (whole stops for Exposure). The slider follows the shared grading rules (grading_controls.hpp).

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <string_view>

namespace bloom::ui {

class GradingValueRow;

// Exposure, Offset and Multiply.
[[nodiscard]] bool isScalarSliderNodeType(std::string_view typeId) noexcept;

class ScalarSliderCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    ScalarSliderCardWidget(CompositionSession& session, document::NodeId node,
                           std::string_view typeId, bool card, QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    [[nodiscard]] GradingValueRow* row() const noexcept { return row_; }
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    CompositionSession& session_;
    document::NodeId node_;
    GradingValueRow* row_ = nullptr;
};

} // namespace bloom::ui
