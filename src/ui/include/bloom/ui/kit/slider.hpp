#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QGradientStops>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <optional>

namespace bloom::ui::kit {

// A single-value slider with an accent fill from the track's start to the handle.
//
// The handle follows the pointer with no easing at all. Scrubbing is direct editor feedback, and
// an eased handle lies about where the value is -- the same reason the playhead is never eased
// (Motion::None in the token vocabulary).
//
// It keeps KValueField's edit-session contract, so properties::bindValueEdit() binds it unchanged:
// a drag is one edit (editStarted() on the press, editFinished() on the release, or when the
// pointer grab or the window is lost), and a run of arrow/Page/Home/End presses is one edit that
// ends on the key release, Escape (which confirms) or focus loss. Pressing the handle keeps the
// pointer's offset from its centre, so grabbing it never jumps the value; pressing the bare track
// moves the handle there and drags on from it. A projection (setValue) arriving mid-drag is refused.
//
// #501: the grading variant (DragMode::Relative and DragMode::Jog) follows the grading widgets'
// shared rules (ui-grammar.md, "Live Value Editing"): a press never jumps the value, a Shift-press
// on the track jumps it to the pointer and then follows the drag, Ctrl (Cmd on macOS) moves it at a
// tenth of the speed and Shift held mid-drag at ten times, and changing a modifier mid-drag starts
// a new segment from the value reached, so nothing jumps. A bipolar control fills from its neutral
// value (setNeutral), with a tick there and a detent a drag at full speed snaps to; ticks
// (setTickInterval) and a gradient track (setTrackGradient) show what the value means. A value
// past the track (typed, or a jog's) is drawn capped with an overflow chevron, never pinned. A
// double-click resets it to its default and the right-click menu offers Reset and the widget's
// own actions(), as KValueField's does.
class KSlider final : public QWidget {
    Q_OBJECT

  public:
    enum class DragMode : std::uint8_t {
        // The handle follows the pointer and a press on the bare track takes the value there.
        Absolute,
        // The value moves by the pointer's travel along the track, from where it was pressed.
        Relative,
        // A jog strip: no handle and no end stops, jogStep() per pixel, the pointer wrapping at the
        // screen edge as a value field's scrub does; the track shows the range.
        Jog,
    };

    explicit KSlider(QWidget* parent = nullptr);

    void setRange(double minimum, double maximum);
    [[nodiscard]] double minimum() const noexcept;
    [[nodiscard]] double maximum() const noexcept;

    [[nodiscard]] double value() const noexcept;
    void setValue(double value);

    void setDragMode(DragMode mode);
    [[nodiscard]] DragMode dragMode() const noexcept { return mode_; }
    // The hard limits a Relative or Jog value is held to (the range when unset); the track still
    // spans the range.
    void setLimits(double minimum, double maximum);
    // The value one pixel of jog travel is worth.
    void setJogStep(double step);
    [[nodiscard]] double jogStep() const noexcept { return jogStep_; }
    // The value a bipolar control fills from, ticks and snaps to; none fills from the minimum.
    void setNeutral(std::optional<double> neutral);
    [[nodiscard]] std::optional<double> neutral() const noexcept { return neutral_; }
    // A tick every `interval` along the track; 0 draws none.
    void setTickInterval(double interval);
    [[nodiscard]] double tickInterval() const noexcept { return tickInterval_; }
    // The track painted with these stops (0 at the minimum, 1 at the maximum) instead of the
    // Field well; a gradient track draws no fill.
    void setTrackGradient(const QGradientStops& stops);
    // The value a double-click or the menu's Reset returns to.
    void setDefaultValue(std::optional<double> value);
    [[nodiscard]] std::optional<double> defaultValue() const noexcept { return default_; }
    void setResetText(const QString& text);
    // Resets the value to its default as one complete edit.
    void resetToDefault();
    // True while the value sits past the track's range (drawn capped with a chevron).
    [[nodiscard]] bool overflows() const noexcept;
    // What one pixel of a Relative or Jog drag is worth, as a multiple: a tenth with Ctrl, ten
    // times with Shift, as KValueField::scrubScale().
    [[nodiscard]] static double dragScale(Qt::KeyboardModifiers modifiers) noexcept;

    [[nodiscard]] bool hasLiveEdit() const noexcept { return interactionActive_; }
    // Programmatic cancellation: the value the edit started from comes back.
    void cancelEdit();

    // Where the value sits in the range, 0..1. The geometry below is derived from this, so a test
    // can assert position without measuring pixels.
    [[nodiscard]] double normalizedValue() const;

    [[nodiscard]] QRectF trackRect() const;
    [[nodiscard]] QRectF fillRect() const;
    [[nodiscard]] QRectF handleRect() const;
    // Where `value` sits along the track (clamped to it).
    [[nodiscard]] double positionOf(double value) const;

    [[nodiscard]] State visualState() const;

    // The single border the handle paints, by the kit-wide focus/hover rule
    // (kit::borderForInteraction).
    [[nodiscard]] Color borderToken() const;
    [[nodiscard]] bool isDragging() const noexcept;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void valueChanged(double value);
    void editStarted();
    void editFinished();
    void editCancelled();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool event(QEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    [[nodiscard]] double valueForPosition(double x) const;
    [[nodiscard]] double lowLimit() const noexcept;
    [[nodiscard]] double highLimit() const noexcept;
    void commitValue(double value);
    void beginInteraction();
    void finishInteraction(bool keep);
    void startSegment(double x, Qt::KeyboardModifiers modifiers, double scale);
    void paintTicks(QPainter& painter, const QRectF& track) const;
    void paintOverflow(QPainter& painter, const QRectF& track) const;

    double minimum_ = 0.0;
    double maximum_ = 1.0;
    double value_ = 0.0;
    DragMode mode_ = DragMode::Absolute;
    std::optional<double> limitMinimum_;
    std::optional<double> limitMaximum_;
    double jogStep_ = 0.01;
    std::optional<double> neutral_;
    double tickInterval_ = 0.0;
    QGradientStops gradient_;
    std::optional<double> default_;
    QString resetText_;
    bool dragging_ = false;
    bool keyStepping_ = false;
    bool hovered_ = false;
    bool interactionActive_ = false;
    double interactionBase_ = 0.0;
    // The press's distance from the handle centre, kept for the whole drag.
    double grabOffset_ = 0.0;
    // A Relative or Jog drag's current segment: where the pointer and the value were when it began,
    // its modifiers and its speed. A Shift-press segment follows the pointer 1:1 until a modifier
    // changes.
    double segmentX_ = 0.0;
    double segmentBase_ = 0.0;
    double segmentScale_ = 1.0;
    Qt::KeyboardModifiers segmentModifiers_;
    // Added to every pointer x while a jog wraps the pointer around the screen.
    double wrapOffset_ = 0.0;
};

} // namespace bloom::ui::kit
