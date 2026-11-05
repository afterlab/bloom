#pragma once

// #505: the Vignette node's card widget, drawn and edited the same on the node card and in
// Properties:
//
// - the centre on a point pad (PointPadRow, kit::KPointPad) over a picture of the frame that shows
//   the vignette's falloff as the node computes it (its shape, roundness, midpoint, feather and
//   strength), so dragging the centre or any slider redraws the darkening where it will fall. The
//   centre's own X/Y row, with its diamond and socket, stays beside the card's other rows;
// - Amount (Multiply) or Exposure in stops (Exposure), whichever the mode reads;
// - Midpoint, Roundness, Feather and Highlight protection, each a grading slider row.
//
// The shape and the mode are the node's own choice rows. Every control follows the shared grading
// rules (grading_controls.hpp): relative drags, Ctrl a tenth and Shift ten times the speed, a
// double-click resets, the right-click menu offers Reset and the value's key, one gesture is one
// undo step, and every animatable value keeps its keyframe diamond and its socket beside its row.

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <QImage>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace bloom::ui {

class GradingValueRow;
class PointPadRow;

class VignetteCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    VignetteCardWidget(CompositionSession& session, document::NodeId node, bool card,
                       QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    // The node's mode as stored (document::kVignetteMultiply or kVignetteExposure).
    [[nodiscard]] std::int64_t mode() const noexcept { return mode_; }
    [[nodiscard]] PointPadRow* padRow() const noexcept { return pad_; }
    [[nodiscard]] GradingValueRow* valueRow(std::string_view role) const;
    // The falloff picture the pad draws, at `width` pixels across (a test seam).
    [[nodiscard]] QImage preview(int width) const;
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    // The rows the current mode shows, top to bottom.
    [[nodiscard]] std::vector<GradingValueRow*> shownRows() const;
    void place();
    CompositionSession& session_;
    document::NodeId node_;
    std::int64_t mode_ = -1;
    PointPadRow* pad_ = nullptr;
    std::vector<GradingValueRow*> values_;
};

} // namespace bloom::ui
