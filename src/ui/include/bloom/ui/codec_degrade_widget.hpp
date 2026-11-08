#pragma once

// #563: the Codec Degrade node's card widget, drawn and edited the same on the node card and in
// Properties: Quality and P-frame persistence, each a grading slider row.
//
// The block size, the GOP length and chroma bleed are the node's own rows (a choice, a choice
// and a checkbox). Every slider follows the shared grading rules (grading_controls.hpp): relative
// drags, Ctrl a tenth and Shift ten times the speed, a double-click resets, one gesture is one undo
// step, and every value keeps its keyframe diamond and its socket beside its row.

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <optional>
#include <string_view>
#include <vector>

namespace bloom::ui {

class GradingValueRow;

class CodecDegradeCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    CodecDegradeCardWidget(CompositionSession& session, document::NodeId node, bool card,
                           QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    [[nodiscard]] GradingValueRow* valueRow(std::string_view role) const;
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void place();
    CompositionSession& session_;
    document::NodeId node_;
    std::vector<GradingValueRow*> values_;
};

} // namespace bloom::ui
