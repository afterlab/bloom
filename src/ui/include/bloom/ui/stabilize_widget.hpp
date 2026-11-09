#pragma once

// #513: the Stabilize node's card widget, drawn and edited the same on the node card and in
// Properties: the track point (the feature as chosen, on the frame to start from) and the position
// the track found as two points on a pad over the composition's frame, and the button that runs the
// tracking job with its progress and a plain statement of how the last job ended. The node's choices
// (mode, pattern, search, filter, edges) and its X/Y rows stay beside it as the node's own rows.
//
// The button only asks the TrackController to start or cancel a job; the job runs off the UI thread
// and stores its result as one undoable edit, so the card owns no project state.

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <optional>
#include <string_view>

class QLabel;

namespace bloom::ui {

namespace kit {
class KButton;
class KProgressBar;
} // namespace kit

class PointPadRow;
class TrackController;

class StabilizeCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    StabilizeCardWidget(CompositionSession& session, document::NodeId node, bool card,
                        QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    [[nodiscard]] PointPadRow* padRow() const noexcept { return pad_; }
    [[nodiscard]] kit::KButton* trackButton() const noexcept { return button_; }
    [[nodiscard]] kit::KProgressBar* progressBar() const noexcept { return progress_; }
    [[nodiscard]] QLabel* statusLabel() const noexcept { return status_; }
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    [[nodiscard]] int controlsHeight() const;
    void layoutControls();
    void onButton();

    CompositionSession& session_;
    document::NodeId node_;
    PointPadRow* pad_ = nullptr;
    kit::KButton* button_ = nullptr;
    kit::KProgressBar* progress_ = nullptr;
    QLabel* status_ = nullptr;
    TrackController* connected_ = nullptr;
};

} // namespace bloom::ui
