#pragma once

// #486: the Film Grain node's card widget, drawn and edited the same on the node card and in
// Properties:
//
// - the response on a graph (FilmGrainResponseGraph): a grey ramp from black to white under the grain
//   as the node adds it there, and the share of the intensity across that ramp as a curve with one
//   handle each for the shadows, the midtones and the highlights, dragged up and down;
// - Intensity, Size, the red, green and blue amounts and the three responses, each a grading slider
//   row.
//
// The animation, the seed and Monochrome are the node's own rows. Every control follows the shared grading rules
// (grading_controls.hpp): relative drags, Ctrl a tenth and Shift ten times the speed, a double-click
// resets, one gesture is one undo step, and every value keeps its keyframe diamond and its socket
// beside its row.

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace bloom::ui {

class GradingValueRow;

class FilmGrainResponseGraph final : public QWidget {
    Q_OBJECT

  public:
    FilmGrainResponseGraph(CompositionSession& session, document::NodeId node,
                           QWidget* parent = nullptr);

    // The handles, in the order of their luma: 0 shadows, 1 midtones, 2 highlights.
    static constexpr std::size_t kHandles = 3;
    [[nodiscard]] static int fixedHeight();
    // The ramp's area and where a handle is drawn (test seams).
    [[nodiscard]] QRectF plotRect() const;
    [[nodiscard]] QPointF handlePosition(std::size_t handle) const;
    // The picture under the curve, at `width` by `height` pixels (a test seam).
    [[nodiscard]] QImage preview(int width, int height) const;
    void cancelDrag();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    struct Drag final {
        std::size_t handle = 0;
        document::ParameterId parameter;
        // The value the gesture has reached and the pointer's last height.
        double value = 0.0;
        qreal lastY = 0.0;
    };
    [[nodiscard]] double scalar(std::string_view role) const;
    [[nodiscard]] std::optional<std::size_t> handleAt(QPointF position) const;

    CompositionSession& session_;
    document::NodeId node_;
    std::optional<Drag> drag_;
    std::optional<std::size_t> hovered_;
};

class FilmGrainCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    FilmGrainCardWidget(CompositionSession& session, document::NodeId node, bool card,
                        QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    [[nodiscard]] FilmGrainResponseGraph* graph() const noexcept { return graph_; }
    [[nodiscard]] GradingValueRow* valueRow(std::string_view role) const;
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void place();
    CompositionSession& session_;
    document::NodeId node_;
    FilmGrainResponseGraph* graph_ = nullptr;
    std::vector<GradingValueRow*> values_;
};

} // namespace bloom::ui
