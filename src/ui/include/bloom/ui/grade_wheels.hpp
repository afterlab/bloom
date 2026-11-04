#pragma once

// #494: the Grade node's card widget: Lift, Gamma, Gain and Offset colour wheels and the primaries
// bar, drawn and edited the same on the node card and in Properties.
//
// A wheel's colour puck shows the part of its red, green and blue values that is not grey (their
// mean is kept): the chroma plane perpendicular to grey, red at the vectorscope's 103 degrees, then
// yellow, green, cyan, blue and magenta anticlockwise. The rim is a chroma of
// gradeWheelRange(wheel). Dragging the puck moves it by the pointer (Shift for a tenth of the
// speed) and edits the three values as one session value edit, one command on release;
// double-clicking a wheel resets it (master and colour) as one command; Alt-click opens its Values
// group. Under each wheel a slider drags its master and a button resets it.

#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <QPointF>
#include <QRectF>

#include <array>
#include <optional>
#include <string_view>

namespace bloom::ui {

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
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }
    void cancelDrag();
    // Sets the wheel's master and colour to neutral as one command.
    bool reset();

  Q_SIGNALS:
    void alternateClicked();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    struct Drag final {
        QPointF pressed;
        QPointF puck;
        std::array<double, 3> base{};
        document::ParameterId primary;
    };
    CompositionSession& session_;
    document::NodeId node_;
    document::GradeWheel wheel_;
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
    [[nodiscard]] QSize sizeHint() const override;

    [[nodiscard]] GradeWheelCanvas* canvas(document::GradeWheel wheel) const;
    [[nodiscard]] kit::KSlider* master(document::GradeWheel wheel) const;
    [[nodiscard]] kit::KButton* resetButton(document::GradeWheel wheel) const;
    [[nodiscard]] kit::KValueField* primary(document::GradePrimary primary) const;
    // Opens or closes a wheel's Values group (its master, red, green and blue rows); the primaries'
    // group is opened with setPrimariesExpanded().
    void setValuesExpanded(document::GradeWheel wheel, bool expanded);
    [[nodiscard]] bool valuesExpanded(document::GradeWheel wheel) const;
    void setPrimariesExpanded(bool expanded);
    [[nodiscard]] bool primariesExpanded() const noexcept { return expanded_.back(); }
    // Re-reads the sliders and fields from the session.
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct Cell final {
        GradeWheelCanvas* canvas = nullptr;
        kit::KSlider* master = nullptr;
        kit::KButton* reset = nullptr;
        kit::KButton* values = nullptr;
    };
    void place();
    void setExpanded(std::size_t group, bool expanded);
    [[nodiscard]] std::optional<document::ParameterId> parameter(std::string_view role) const;

    CompositionSession& session_;
    document::NodeId node_;
    std::array<Cell, document::kGradeWheelCount> cells_{};
    kit::KLabel* primariesTitle_ = nullptr;
    kit::KButton* primariesValues_ = nullptr;
    std::array<kit::KValueField*, document::kGradePrimaryCount> primaries_{};
    // The four wheels' Values groups, then the primaries'.
    std::array<bool, document::kGradeWheelCount + 1> expanded_{};
};

} // namespace bloom::ui
