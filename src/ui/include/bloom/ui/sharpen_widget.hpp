#pragma once

// #486: the Sharpen node's card widget, drawn and edited the same on the node card and in Properties:
//
// - a step edge as the node sharpens it (SharpenProfileGraph): the edge's picture under its profile
//   across the edge, the input's as a thin line and the result's as a curve, with one handle on the
//   overshoot, dragged up and down for the amount, and one where the halo ends, dragged sideways for
//   the radius;
// - Amount, Radius and Threshold, each a grading slider row.
//
// Every control follows the shared grading rules (grading_controls.hpp): relative drags, Ctrl a tenth
// and Shift ten times the speed, a double-click resets, one gesture is one undo step, and every value
// keeps its keyframe diamond and its socket beside its row.

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace bloom::ui {

class GradingValueRow;

class SharpenProfileGraph final : public QWidget {
    Q_OBJECT

  public:
    SharpenProfileGraph(CompositionSession& session, document::NodeId node,
                        QWidget* parent = nullptr);

    // The handles: 0 the amount, 1 the radius.
    static constexpr std::size_t kHandles = 2;
    [[nodiscard]] static int fixedHeight();
    // The drawing's area and where a handle is drawn (test seams).
    [[nodiscard]] QRectF plotRect() const;
    [[nodiscard]] QPointF handlePosition(std::size_t handle) const;
    // The edge's brightness in linear light at the pixel `offset` pixels past the edge (a test seam).
    [[nodiscard]] double sharpenedAt(int offset) const;
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
        // The value the gesture has reached and the pointer's last position.
        double value = 0.0;
        QPointF last;
    };
    [[nodiscard]] double scalar(std::string_view role) const;
    [[nodiscard]] std::optional<std::size_t> handleAt(QPointF position) const;

    CompositionSession& session_;
    document::NodeId node_;
    std::optional<Drag> drag_;
    std::optional<std::size_t> hovered_;
};

class SharpenCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    SharpenCardWidget(CompositionSession& session, document::NodeId node, bool card,
                      QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    [[nodiscard]] SharpenProfileGraph* graph() const noexcept { return graph_; }
    [[nodiscard]] GradingValueRow* valueRow(std::string_view role) const;
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void place();
    CompositionSession& session_;
    document::NodeId node_;
    SharpenProfileGraph* graph_ = nullptr;
    std::vector<GradingValueRow*> values_;
};

} // namespace bloom::ui
