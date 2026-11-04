#pragma once

#include <bloom/ui/kit/tokens.hpp>

#include <QPointF>
#include <QSize>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <optional>

namespace bloom::ui::kit {

// #541: an angle edited by turning a dial. The needle points along the angle; a tick on the rim
// marks zero. The value is in degrees, positive clockwise on screen (the Viewer's y-down
// convention), and it winds: turning past a full revolution keeps counting (370 is not 10), so an
// animated rotation can spin.
//
// It follows the grading widgets' rules (ui-grammar.md, "Live Value Editing"): a press never jumps
// the value -- dragging turns the needle by the pointer's own turn around the centre -- and a
// Shift-press jumps it to the pointer, after which Shift held snaps to kSnapDegrees. Ctrl (Cmd on
// macOS) turns it at a tenth of the speed. A double-click resets it to its default, and the
// right-click menu offers Reset and the widget's own actions().
//
// Keyboard: Left/Down and Right/Up step one degree, Page Up/Page Down kSnapDegrees, Home resets.
// It keeps KSlider's edit-session contract, so properties::bindValueEdit() binds it unchanged: a
// drag or a run of keys is one edit.
class KAngleDial final : public QWidget {
    Q_OBJECT

  public:
    // Where zero points on screen.
    enum class Zero : std::uint8_t {
        Up,
        Right,
    };
    static constexpr double kSnapDegrees = 15.0;

    explicit KAngleDial(QWidget* parent = nullptr);

    [[nodiscard]] double value() const noexcept { return value_; }
    // A projection arriving mid-drag is refused, as KSlider's is.
    void setValue(double degrees);
    void setZero(Zero zero);
    [[nodiscard]] Zero zero() const noexcept { return zero_; }
    // A value that turns counter-clockwise for a positive angle (the mathematical convention of a
    // value graph's vector rotation) turns the needle that way too.
    void setCounterClockwise(bool counterClockwise);
    [[nodiscard]] bool counterClockwise() const noexcept { return counterClockwise_; }
    void setDefaultValue(std::optional<double> degrees);
    [[nodiscard]] std::optional<double> defaultValue() const noexcept { return default_; }
    void setResetText(const QString& text);
    void resetToDefault();

    [[nodiscard]] bool hasLiveEdit() const noexcept { return interactionActive_; }
    void cancelEdit();
    [[nodiscard]] bool isDragging() const noexcept { return dragging_; }

    // The dial's disc and the point the needle ends at (test seams).
    [[nodiscard]] QRectF discRect() const;
    [[nodiscard]] QPointF needleTip() const;
    // The screen angle of `point` around the centre, in the dial's own degrees (0 at zero(),
    // clockwise positive unless counterClockwise(), in [0, 360)).
    [[nodiscard]] double angleAt(QPointF point) const;

    [[nodiscard]] State visualState() const;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] bool hasHeightForWidth() const override { return true; }
    [[nodiscard]] int heightForWidth(int width) const override { return width; }

  Q_SIGNALS:
    void valueChanged(double degrees);
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
    void commitValue(double degrees);
    void beginInteraction();
    void finishInteraction(bool keep);
    void startSegment(QPointF point, Qt::KeyboardModifiers modifiers);

    double value_ = 0.0;
    Zero zero_ = Zero::Up;
    bool counterClockwise_ = false;
    std::optional<double> default_;
    QString resetText_;
    bool dragging_ = false;
    bool keyStepping_ = false;
    bool hovered_ = false;
    bool interactionActive_ = false;
    double interactionBase_ = 0.0;
    // The current drag segment: the pointer's unwrapped angle and the value when it began.
    double segmentPointer_ = 0.0;
    double segmentBase_ = 0.0;
    double lastPointer_ = 0.0;
    double unwrapped_ = 0.0;
    Qt::KeyboardModifiers segmentModifiers_;
    bool jumped_ = false;
};

} // namespace bloom::ui::kit
