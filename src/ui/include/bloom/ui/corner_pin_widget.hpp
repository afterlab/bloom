#pragma once

// #348: the Corner Pin node's card widget, drawn and edited the same on the node card and in
// Properties: the four corners of the pinned quad as points on a pad over the composition's frame,
// joined into the quad. Each point starts on a corner of the frame (the offset is zero) and is
// dragged to where that corner of the picture should land. The corners' own X/Y rows, with their
// diamonds and sockets, and the filter choice stay beside the pad as the node's own rows.
//
// The pad follows the shared interaction rules (kit::KPointPad): relative drags, Ctrl a tenth of the
// speed, a double-click resets, one gesture is one undo step.

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <optional>
#include <string_view>

namespace bloom::ui {

class PointPadRow;

class CornerPinCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    CornerPinCardWidget(CompositionSession& session, document::NodeId node, bool card,
                        QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    [[nodiscard]] PointPadRow* padRow() const noexcept { return pad_; }
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    CompositionSession& session_;
    document::NodeId node_;
    PointPadRow* pad_ = nullptr;
};

} // namespace bloom::ui
