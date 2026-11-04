#pragma once

// #494, #501: the Grade node's card widget: Lift, Gamma, Gain and Offset colour wheels and the
// primaries, drawn and edited the same on the node card and in Properties.
//
// A wheel's colour puck shows the part of its red, green and blue values that is not grey (their
// mean is kept): the chroma plane perpendicular to grey, red at the vectorscope's 103 degrees, then
// yellow, green, cyan, blue and magenta anticlockwise. The rim is a chroma of
// gradeWheelRange(wheel). The wheel is a donut: the hue ring around an empty centre while the
// colour is neutral, the interior filled with the faded hues once it is not, and nothing else but
// the puck and its line from the centre.
//
// Each wheel follows the shared grading rules (grading_controls.hpp). A press inside the disc
// drags the puck by the pointer's travel, so it never jumps; a Shift-press jumps it to the pointer,
// which it then follows; Ctrl (Cmd) moves it at a tenth of the speed, and a modifier change mid-drag
// continues from where the puck is. The arrow keys nudge it. A drag edits the three values as one
// session value edit, one command on release. A double-click on the disc resets its colour (the
// level stays); the wheel's Reset button resets colour and level. Under the disc the wheel's level
// ("Lift level") is always readable, with its unit, over a jog strip that drags it with no end
// stops (darker to the left, lighter to the right); under that, the red, green and blue values.
// The wheel's keyframe diamond keys its four values as one; its Values button (or Alt-click)
// shows their rows, with their sockets, to link them.
//
// The primaries follow, one row each with its diamond and its socket: Temperature and Tint over
// blue-to-amber and green-to-magenta tracks, Contrast with a detent at 1, Pivot over a small
// picture of the contrast curve whose pivot marker drags it, Saturation over grey-to-colour and
// Exposure over whole-stop ticks. Their Reset button resets the six.

#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <QPointF>
#include <QRectF>

#include <array>
#include <optional>
#include <string_view>

class QMenu;

namespace bloom::ui {

class GradingValueRow;
class KeyframeGroupDiamond;
namespace kit {
class KButton;
class KLabel;
class KSlider;
class KValueField;
} // namespace kit

// The chroma length at a wheel's rim.
[[nodiscard]] double gradeWheelRange(document::GradeWheel wheel) noexcept;
// Where the puck of `rgb` sits, in units of the wheel's radius with y up (beyond 1 past the rim).
[[nodiscard]] QPointF gradeWheelPuck(document::GradeWheel wheel, const std::array<double, 3>& rgb);
// The red, green and blue that put the puck at `puck`, keeping the grey part of `current`. Each is
// limited to the wheel's domain.
[[nodiscard]] std::array<double, 3> gradeWheelColour(document::GradeWheel wheel, QPointF puck,
                                                     const std::array<double, 3>& current);
// The hue a wheel shows at angle `degrees` (anticlockwise from the right), as display RGB.
[[nodiscard]] std::array<double, 3> gradeWheelHueColour(double degrees);
[[nodiscard]] QString gradeWheelName(document::GradeWheel wheel);
// What a wheel's level does, for its label's tooltip and accessible description.
[[nodiscard]] QString gradeWheelLevelDescription(document::GradeWheel wheel);

class GradeWheelCanvas final : public QWidget {
    Q_OBJECT

  public:
    GradeWheelCanvas(CompositionSession& session, document::NodeId node, document::GradeWheel wheel,
                     QWidget* parent = nullptr);

    [[nodiscard]] document::GradeWheel wheel() const noexcept { return wheel_; }
    // The wheel's master, red, green and blue parameters, and their values now.
    [[nodiscard]] std::optional<std::array<document::ParameterId, 4>> parameters() const;
    [[nodiscard]] std::optional<std::array<double, 4>> values() const;
    // Where the disc and the puck are drawn (widget coordinates).
    [[nodiscard]] QRectF discRect() const;
    [[nodiscard]] QPointF puckPosition() const;
    // Whether a press at `position` lands on the disc (its circle, the puck's reach included).
    [[nodiscard]] bool onDisc(QPointF position) const;
    // Whether the colour is neutral (the donut's centre is empty).
    [[nodiscard]] bool neutral() const;
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }
    void cancelDrag();
    // Sets the wheel's red, green and blue to neutral as one command (the level stays).
    bool resetColour();
    // Sets the wheel's level and colour to neutral as one command.
    bool reset();
    // The right-click menu; the caller owns it (a test seam: the event shows it).
    [[nodiscard]] QMenu* makeContextMenu();

    [[nodiscard]] QSize sizeHint() const override;

  Q_SIGNALS:
    void alternateClicked();
    // The menu's key action: the card keys the wheel's four values as one.
    void keyRequested();

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
    void hideEvent(QHideEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    struct Drag final {
        // The segment the pointer drags now: where the pointer and the puck were when it began,
        // and its speed. A modifier change starts a new segment, so nothing jumps.
        QPointF from;
        QPointF puck;
        double scale = 1.0;
        Qt::KeyboardModifiers modifiers;
        // Where the puck is now, and how far past the rim it may go (it stops at the rim unless
        // the values already sat past it).
        QPointF at;
        double limit = 1.0;
        std::array<double, 3> base{};
        document::ParameterId primary;
        // A run of arrow keys, not the pointer.
        bool keys = false;
    };
    [[nodiscard]] bool beginDrag(bool keys);
    void moveTo(QPointF puck);
    CompositionSession& session_;
    document::NodeId node_;
    document::GradeWheel wheel_;
    std::optional<Drag> drag_;
    bool hovered_ = false;
};

// #501, D7: the Pivot row's picture: the contrast curve about the pivot over 0..1, with the pivot's
// marker on its foot. Dragging the marker (or the picture) moves the pivot by the pointer's travel;
// a Shift-press jumps it to the pointer; Ctrl a tenth, Shift ten times the speed; a double-click
// resets it to 0.18.
class GradeContrastCanvas final : public QWidget {
    Q_OBJECT

  public:
    GradeContrastCanvas(CompositionSession& session, document::NodeId node,
                        QWidget* parent = nullptr);

    [[nodiscard]] double contrast() const;
    [[nodiscard]] double pivot() const;
    [[nodiscard]] QRectF plotRect() const;
    [[nodiscard]] double pivotX() const;
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }
    void cancelDrag();
    bool resetPivot();
    // The right-click menu; the caller owns it.
    [[nodiscard]] QMenu* makeContextMenu();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    struct Drag final {
        document::ParameterId parameter;
        double fromX = 0.0;
        double base = 0.0;
        double scale = 1.0;
        Qt::KeyboardModifiers modifiers;
    };
    [[nodiscard]] std::optional<document::ParameterId> parameter(std::string_view role) const;
    CompositionSession& session_;
    document::NodeId node_;
    std::optional<Drag> drag_;
};

class GradeCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    GradeCardWidget(CompositionSession& session, document::NodeId node, bool card,
                    QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    [[nodiscard]] GradeWheelCanvas* canvas(document::GradeWheel wheel) const;
    // The wheel's level: its row (label, value) and the jog strip under it.
    [[nodiscard]] GradingValueRow* level(document::GradeWheel wheel) const;
    [[nodiscard]] kit::KSlider* master(document::GradeWheel wheel) const;
    // The wheel's red (0), green (1) and blue (2) values.
    [[nodiscard]] kit::KValueField* colour(document::GradeWheel wheel, std::size_t channel) const;
    [[nodiscard]] kit::KButton* resetButton(document::GradeWheel wheel) const;
    [[nodiscard]] kit::KButton* valuesButton(document::GradeWheel wheel) const;
    [[nodiscard]] KeyframeGroupDiamond* diamond(document::GradeWheel wheel) const;
    [[nodiscard]] GradingValueRow* primaryRow(document::GradePrimary primary) const;
    [[nodiscard]] kit::KValueField* primary(document::GradePrimary primary) const;
    [[nodiscard]] kit::KButton* primariesReset() const noexcept { return primariesReset_; }
    [[nodiscard]] GradeContrastCanvas* contrastCanvas() const noexcept { return contrast_; }
    // Opens or closes a wheel's Values group (its master, red, green and blue rows, with their
    // sockets).
    void setValuesExpanded(document::GradeWheel wheel, bool expanded);
    [[nodiscard]] bool valuesExpanded(document::GradeWheel wheel) const;
    // Resets the six primaries as one command.
    bool resetPrimaries();
    // Re-reads the fields and diamonds from the session.
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct Cell final {
        kit::KLabel* title = nullptr;
        KeyframeGroupDiamond* diamond = nullptr;
        kit::KButton* reset = nullptr;
        kit::KButton* values = nullptr;
        GradeWheelCanvas* canvas = nullptr;
        GradingValueRow* level = nullptr;
        std::array<kit::KValueField*, 3> colour{};
    };
    struct Metrics final {
        int cell = 0;
        int disc = 0;
        int cellHeight = 0;
        int primariesTop = 0;
    };
    [[nodiscard]] Metrics metrics(int width) const;
    [[nodiscard]] int cellMinimumWidth() const;
    void place();
    void setExpanded(std::size_t group, bool expanded);
    [[nodiscard]] std::optional<document::ParameterId> parameter(std::string_view role) const;

    CompositionSession& session_;
    document::NodeId node_;
    std::array<Cell, document::kGradeWheelCount> cells_{};
    kit::KLabel* primariesTitle_ = nullptr;
    kit::KButton* primariesReset_ = nullptr;
    std::array<GradingValueRow*, document::kGradePrimaryCount> primaries_{};
    GradeContrastCanvas* contrast_ = nullptr;
    // The four wheels' Values groups.
    std::array<bool, document::kGradeWheelCount> expanded_{};
};

} // namespace bloom::ui
