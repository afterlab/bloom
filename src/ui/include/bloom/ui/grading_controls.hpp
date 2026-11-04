#pragma once

// #501: the pieces every grading widget shares, so the Grade, Levels, Curves, Hue/Saturation,
// Exposure, Offset and Multiply cards behave the same (docs/ux/ui-grammar.md, "Grading widgets"):
//
// - a press never jumps a value; a Shift-press on a wheel disc or a slider track jumps it to the
//   pointer, which it then follows;
// - Ctrl (Cmd on macOS) is a tenth of the speed on every drag and Shift held during a 1-D drag ten
//   times; a modifier change mid-drag starts a new segment from the value reached;
// - a double-click resets the control it lands on, a group's Reset button the group, and the
//   right-click menu of every control offers Reset and its key at the playhead;
// - every animatable value shows its keyframe diamond on the widget's own row, and its number with
//   its unit, always;
// - one gesture is one undo step (a session value edit, ADR 0017).
//
// GradingValueRow is one value on such a widget: its name, its number, its diamond, and under them
// the slider that drags it (kit::KSlider's grading variant). GradingSliderSpec says what the slider
// shows: its span, its neutral value, ticks and the colours of its track.

#include <bloom/document/ids.hpp>
#include <bloom/ui/kit/slider.hpp>

#include <QGradientStops>
#include <QString>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

class QAction;
class QMenu;

namespace bloom::ui {

class CompositionSession;
class KeyframeDiamond;
namespace kit {
class KLabel;
class KValueField;
} // namespace kit

// The parameter bound to `role` on `node`, its scalar value now (a live drag, a key at the
// playhead, a constant), and whether a widget may write it (not locked, not driven).
[[nodiscard]] std::optional<document::ParameterId>
nodeRoleParameter(const CompositionSession& session, document::NodeId node, std::string_view role);
[[nodiscard]] std::optional<double> liveScalar(const CompositionSession& session,
                                               document::ParameterId parameter);
[[nodiscard]] bool scalarEditable(const CompositionSession& session, document::ParameterId parameter);

// The track colours of the grading sliders: dark to light (exposure, offset, a wheel's level),
// grey to colour (saturation), blue to amber (temperature) and green to magenta (tint).
[[nodiscard]] QGradientStops darkToLightStops();
// A jog strip's track: dark to light, quieter than a slider's, behind the knurl.
[[nodiscard]] QGradientStops jogStops();
[[nodiscard]] QGradientStops greyToColourStops(double neutralShare);
[[nodiscard]] QGradientStops temperatureStops();
[[nodiscard]] QGradientStops tintStops();

struct GradingSliderSpec final {
    kit::KSlider::DragMode mode = kit::KSlider::DragMode::Relative;
    // The span the track shows, in stored units.
    double minimum = 0.0;
    double maximum = 1.0;
    std::optional<double> neutral;
    // A tick every `ticks` (0 none).
    double ticks = 0.0;
    QGradientStops gradient;
    // A jog's value per pixel.
    double jogStep = 0.01;
    // The share of the row's width the slider takes, centred.
    double widthShare = 1.0;
};

class GradingValueRow final : public QWidget {
    Q_OBJECT

  public:
    // `name` is the row's label; `neutral` the value a reset returns to.
    GradingValueRow(CompositionSession& session, document::NodeId node, std::string role,
                    std::string_view schemaKey, QString name, double neutral,
                    QWidget* parent = nullptr);

    // Adds the slider under the name and number.
    void setSlider(const GradingSliderSpec& spec);
    void setShowsSign(bool shows);
    // The row's own diamond; a widget that keys this value with a group's diamond hides it.
    void setDiamondShown(bool shown);
    // The label's tooltip and accessible description: what the value does.
    void setDescription(const QString& description);
    // A tooltip for the number that depends on it (a gamma's resulting power).
    void setValueToolTip(std::function<QString(double)> tooltip);

    [[nodiscard]] QString name() const { return name_; }
    [[nodiscard]] const std::string& role() const noexcept { return role_; }
    [[nodiscard]] kit::KLabel* label() const noexcept { return label_; }
    [[nodiscard]] kit::KValueField* field() const noexcept { return field_; }
    [[nodiscard]] kit::KSlider* slider() const noexcept { return slider_; }
    [[nodiscard]] KeyframeDiamond* diamond() const noexcept { return diamond_; }
    [[nodiscard]] QAction* keyAction() const noexcept { return keyAction_; }
    [[nodiscard]] std::optional<document::ParameterId> parameter() const;

    // The label column (the widest label of the rows it lines up with) and the row's height.
    void setLabelWidth(int width);
    [[nodiscard]] int labelWidthHint() const;
    [[nodiscard]] int heightHint() const;
    [[nodiscard]] int minimumWidthHint() const;
    // Where its socket belongs, in the row's coordinates: the middle of its first line.
    [[nodiscard]] int socketY() const;

    // Sets the value back to neutral as one command.
    bool reset();
    // The right-click menu of the row's controls: Reset, then the value's key at the playhead.
    // The caller owns it (a test seam).
    [[nodiscard]] QMenu* makeContextMenu();
    // Re-reads the session.
    void refresh(bool locked);

  protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void place();
    void write(double value);

    CompositionSession& session_;
    document::NodeId node_;
    std::string role_;
    QString name_;
    double neutral_ = 0.0;
    kit::KLabel* label_ = nullptr;
    kit::KValueField* field_ = nullptr;
    kit::KSlider* slider_ = nullptr;
    KeyframeDiamond* diamond_ = nullptr;
    QAction* keyAction_ = nullptr;
    QAction* resetAction_ = nullptr;
    std::function<QString(double)> valueToolTip_;
    double widthShare_ = 1.0;
    int labelWidth_ = 0;
    bool diamondShown_ = true;
};

} // namespace bloom::ui
