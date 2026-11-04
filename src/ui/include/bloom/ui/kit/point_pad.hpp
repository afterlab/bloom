#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <optional>
#include <vector>

namespace bloom::ui::kit {

// #541: positions edited by dragging points over a picture of the frame they live in -- a
// gradient's start and end, a transform's pivot and offset. The frame (a rectangle in the values'
// own units, y down as in the Viewer) is drawn at its aspect ratio with room around it, so a point
// can sit somewhat outside it; a point further out is drawn pinned to the edge, hollow.
//
// A point is dragged by the pointer's travel, never jumped to it: pressing one never moves it.
// Ctrl (Cmd on macOS) moves it at a tenth of the speed. A Shift-press on bare pad jumps the active
// point to the pointer, which it then follows. A double-click on a point resets every point to its
// default. Two points can be joined by a line (setConnected), the way a gradient's start and end
// are.
//
// Keyboard: one point is active and carries the focus ring; the arrow keys move it by one unit
// (Shift ten), and Tab and Shift+Tab move to the next and previous point before focus leaves the
// control. It keeps KSlider's edit-session contract (editStarted, pointsChanged per step,
// editFinished or editCancelled), so a drag or a run of keys is one edit.
class KPointPad final : public QWidget {
    Q_OBJECT

  public:
    struct Point final {
        QString label;
        QPointF value;
        std::optional<QPointF> defaultValue;
    };

    explicit KPointPad(QWidget* parent = nullptr);

    // The frame the values are drawn against.
    void setFrame(const QRectF& frame);
    [[nodiscard]] QRectF frame() const noexcept { return frame_; }
    // The points; their count and labels set the pad's shape. A projection mid-edit is refused.
    void setPoints(std::vector<Point> points);
    [[nodiscard]] const std::vector<Point>& points() const noexcept { return points_; }
    [[nodiscard]] QPointF value(std::size_t index) const;
    void setConnected(bool connected);
    [[nodiscard]] bool connected() const noexcept { return connected_; }

    [[nodiscard]] std::size_t activePoint() const noexcept { return active_; }
    void setActivePoint(std::size_t index);
    // The point being dragged, if any.
    [[nodiscard]] std::optional<std::size_t> grabbedPoint() const noexcept { return grabbed_; }
    void resetToDefaults();

    [[nodiscard]] bool hasLiveEdit() const noexcept { return interactionActive_; }
    void cancelEdit();

    // Geometry (test seams): the frame as drawn, a value's place on screen and back.
    [[nodiscard]] QRectF frameRect() const;
    [[nodiscard]] QPointF screenPoint(QPointF value) const;
    [[nodiscard]] QPointF valueAt(QPointF screen) const;
    [[nodiscard]] std::optional<std::size_t> pointAt(QPointF screen) const;

    [[nodiscard]] State visualState() const;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] bool hasHeightForWidth() const override { return true; }
    [[nodiscard]] int heightForWidth(int width) const override;

  Q_SIGNALS:
    void pointsChanged();
    void activePointChanged(std::size_t index);
    void editStarted();
    void editFinished();
    void editCancelled();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool focusNextPrevChild(bool next) override;
    bool event(QEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    // Value units per screen pixel.
    [[nodiscard]] double unitsPerPixel() const;
    [[nodiscard]] QRectF padRect() const;
    void movePoint(std::size_t index, QPointF value);
    void beginInteraction();
    void finishInteraction(bool keep);

    QRectF frame_{0.0, 0.0, 1920.0, 1080.0};
    std::vector<Point> points_;
    std::vector<QPointF> base_;
    bool connected_ = false;
    std::size_t active_ = 0;
    std::optional<std::size_t> grabbed_;
    QPointF segmentPointer_;
    QPointF segmentBase_;
    Qt::KeyboardModifiers segmentModifiers_;
    bool keyStepping_ = false;
    bool hovered_ = false;
    bool interactionActive_ = false;
};

} // namespace bloom::ui::kit
