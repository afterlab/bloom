#pragma once

// #504: the Colorize node's card widget, drawn and edited the same on the node card and in
// Properties. It shows the controls of the node's mode and keeps the others behind it:
//
// - Amount, a grading slider row, always;
// - Fill: the fill colour's row, a colour chip that opens the colour picker;
// - Tint and Tritone: the ramp bar, the gradient the node maps through drawn from its own
//   colours, with a stop under it for each colour (Tint: shadows and highlights; Tritone: shadows,
//   midtones at the midpoint, highlights). Dragging the midtones stop moves the midpoint; clicking
//   a stop opens its colour picker. Under the bar each stop's colour row (a chip) and, for
//   Tritone, the Midpoint row;
// - Gradient Map (#541): the node's own multi-stop gradient in a kit::KGradientEditor (any number
//   of stops, added, moved, removed and recoloured on the bar; animatable as one whole-value key),
//   the selected stop's colour chip and position under it, then Repetitions and Phase sliders and
//   the Map from and Outside the gradient choices as rows. A node saved before the gradient
//   existed shows its shadows/midtones/highlights ramp as that gradient, and takes the gradient on
//   its first edit;
// - Black & White: the channel mix, one slider row per hue (reds, yellows, greens, cyans, blues,
//   magentas) over a track that runs from black to that hue.
//
// Every control follows the shared grading rules (grading_controls.hpp): relative drags, Ctrl a
// tenth and Shift ten times the speed, a Shift-press on the ramp bar jumps the midpoint to the
// pointer, a double-click resets, the right-click menu offers Reset and the value's key, one
// gesture is one undo step, and every animatable value keeps its keyframe diamond and its socket
// beside its row.

#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <QColor>
#include <QWidget>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QAction;
class QMenu;

namespace bloom::ui {

class GradingValueRow;
class KeyframeDiamond;
namespace kit {
class KColorChip;
class KGradientEditor;
class KLabel;
class KValueField;
} // namespace kit

// One colour value on the widget: its name, its colour chip (the picker) and its diamond.
class ColorizeColorRow final : public QWidget {
    Q_OBJECT

  public:
    ColorizeColorRow(CompositionSession& session, document::NodeId node, std::string role,
                     std::string_view schemaKey, QString name, QWidget* parent = nullptr);

    [[nodiscard]] const std::string& role() const noexcept { return role_; }
    [[nodiscard]] QString name() const { return name_; }
    [[nodiscard]] kit::KColorChip* chip() const noexcept { return chip_; }
    [[nodiscard]] kit::KLabel* label() const noexcept { return label_; }
    [[nodiscard]] std::optional<document::ParameterId> parameter() const;
    // The colour now (a live edit, a key at the playhead, a constant), straight scene-linear RGBA.
    [[nodiscard]] std::optional<std::array<double, 4>> value() const;
    void setDescription(const QString& description);
    void setLabelWidth(int width);
    [[nodiscard]] int labelWidthHint() const;
    [[nodiscard]] int minimumWidthHint() const;
    [[nodiscard]] int socketY() const;
    // Sets the colour back to the node's default as one command.
    bool reset();
    // The right-click menu of the row: Reset, then the value's key. The caller owns it.
    [[nodiscard]] QMenu* makeContextMenu();
    void refresh(bool locked);

  protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void place();
    CompositionSession& session_;
    document::NodeId node_;
    std::string role_;
    std::string schemaKey_;
    QString name_;
    kit::KLabel* label_ = nullptr;
    kit::KColorChip* chip_ = nullptr;
    KeyframeDiamond* diamond_ = nullptr;
    QAction* keyAction_ = nullptr;
    int labelWidth_ = 0;
    bool refreshing_ = false;
};

// The gradient a Tint, Tritone or Gradient Map maps through, with a stop per colour under it.
class ColorizeRampBar final : public QWidget {
    Q_OBJECT

  public:
    struct Stop final {
        std::string role;
        double position = 0.0;
        // Straight scene-linear RGB, as the node reads it.
        std::array<double, 3> colour{};
    };

    ColorizeRampBar(CompositionSession& session, document::NodeId node, QWidget* parent = nullptr);

    // The stops left to right; `ramp` is true when the middle stop is the draggable midpoint.
    void setStops(std::vector<Stop> stops, bool ramp);
    [[nodiscard]] const std::vector<Stop>& stops() const noexcept { return stops_; }
    [[nodiscard]] bool ramp() const noexcept { return ramp_; }
    // Called with a stop's role when a press on it releases without a drag (opens its picker).
    void setStopClicked(std::function<void(const std::string&)> clicked);
    [[nodiscard]] std::optional<document::ParameterId> midpointParameter() const;
    [[nodiscard]] double midpoint() const;
    // The bar and the band of stop handles under it (test seams).
    [[nodiscard]] QRectF barRect() const;
    [[nodiscard]] QPointF stopCentre(std::size_t index) const;
    // The display colour of the gradient at `t` in [0, 1] (a test seam).
    [[nodiscard]] QColor colourAt(double t) const;
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }
    [[nodiscard]] int heightHint() const;
    void cancelDrag();
    bool resetMidpoint();
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
        bool moved = false;
    };
    [[nodiscard]] std::optional<std::size_t> stopAt(QPointF point) const;
    [[nodiscard]] std::array<double, 3> referenceAt(double t) const;
    CompositionSession& session_;
    document::NodeId node_;
    std::vector<Stop> stops_;
    bool ramp_ = false;
    std::function<void(const std::string&)> stopClicked_;
    std::optional<std::size_t> pressedStop_;
    std::optional<Drag> drag_;
};

// #541: the Gradient Map's gradient: its name, the gradient editor and its keyframe diamond, and
// under them the selected stop's colour chip and position. A drag on the bar, a colour pick or a
// position scrub is one session value edit (one undo step; a key at the playhead when animated).
class ColorizeGradientRow final : public QWidget {
    Q_OBJECT

  public:
    ColorizeGradientRow(CompositionSession& session, document::NodeId node,
                        QWidget* parent = nullptr);

    [[nodiscard]] kit::KGradientEditor* editor() const noexcept { return editor_; }
    [[nodiscard]] kit::KColorChip* stopChip() const noexcept { return chip_; }
    [[nodiscard]] kit::KValueField* stopPosition() const noexcept { return position_; }
    [[nodiscard]] std::optional<document::ParameterId> parameter() const;
    // The gradient now: the bound parameter's live value, or for a node saved before it existed
    // the shadows/midtones/highlights ramp it renders with.
    [[nodiscard]] std::optional<document::GradientValue> value() const;
    void setLabelWidth(int width);
    [[nodiscard]] int labelWidthHint() const;
    [[nodiscard]] int heightHint() const;
    [[nodiscard]] int minimumWidthHint() const;
    bool reset();
    [[nodiscard]] QMenu* makeContextMenu();
    void refresh(bool locked);

  protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    // The bound parameter, binding the gradient first on a node saved without it.
    [[nodiscard]] std::optional<document::ParameterId> ensureParameter();
    [[nodiscard]] document::GradientValue editorValue() const;
    void write(document::GradientValue value);
    void place();
    CompositionSession& session_;
    document::NodeId node_;
    kit::KLabel* label_ = nullptr;
    kit::KGradientEditor* editor_ = nullptr;
    KeyframeDiamond* diamond_ = nullptr;
    kit::KLabel* stopLabel_ = nullptr;
    kit::KColorChip* chip_ = nullptr;
    kit::KValueField* position_ = nullptr;
    QAction* keyAction_ = nullptr;
    int labelWidth_ = 0;
    bool refreshing_ = false;
};

class ColorizeCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    ColorizeCardWidget(CompositionSession& session, document::NodeId node, bool card,
                       QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    // The node's mode as stored (document::kColorizeFill ... kColorizeBlackAndWhite).
    [[nodiscard]] std::int64_t mode() const noexcept { return mode_; }
    [[nodiscard]] GradingValueRow* amountRow() const noexcept { return amount_; }
    [[nodiscard]] ColorizeRampBar* rampBar() const noexcept { return ramp_; }
    [[nodiscard]] ColorizeGradientRow* gradientRow() const noexcept { return gradient_; }
    [[nodiscard]] ColorizeColorRow* colorRow(std::string_view role) const;
    [[nodiscard]] GradingValueRow* valueRow(std::string_view role) const;
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    // The rows the current mode shows, top to bottom (the ramp bar as a null row).
    [[nodiscard]] std::vector<QWidget*> shownRows() const;
    [[nodiscard]] int rowHeight(const QWidget* row) const;
    void place();
    CompositionSession& session_;
    document::NodeId node_;
    std::int64_t mode_ = -1;
    GradingValueRow* amount_ = nullptr;
    ColorizeRampBar* ramp_ = nullptr;
    ColorizeGradientRow* gradient_ = nullptr;
    std::vector<ColorizeColorRow*> colours_;
    std::vector<GradingValueRow*> values_;
};

} // namespace bloom::ui
