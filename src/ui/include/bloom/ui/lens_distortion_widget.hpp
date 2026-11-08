#pragma once

// #486: the Lens Distortion node's card widget, drawn and edited the same on the node card and in
// Properties:
//
// - the lens centre on a point pad (PointPadRow, kit::KPointPad) over the composition's frame; the
//   centre's own X/Y row, with its diamond and socket, stays beside the card's other rows;
// - the radial coefficients k1 and k2 and the tangential coefficients p1 and p2, each a grading
//   slider row.
//
// The mode (Undistort or Redistort) is the node's own choice row. Every control follows the shared
// grading rules (grading_controls.hpp): relative drags, Ctrl a tenth and Shift ten times the speed,
// a double-click resets, the right-click menu offers Reset and the value's key, one gesture is one
// undo step, and every animatable value keeps its keyframe diamond and its socket beside its row.

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <optional>
#include <string_view>
#include <vector>

namespace bloom::ui {

class GradingValueRow;
class PointPadRow;

class LensDistortionCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    LensDistortionCardWidget(CompositionSession& session, document::NodeId node, bool card,
                             QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    [[nodiscard]] PointPadRow* padRow() const noexcept { return pad_; }
    [[nodiscard]] GradingValueRow* valueRow(std::string_view role) const;
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void place();
    CompositionSession& session_;
    document::NodeId node_;
    PointPadRow* pad_ = nullptr;
    std::vector<GradingValueRow*> values_;
};

} // namespace bloom::ui
