#pragma once

// #494, #501: the Levels node's card widget, drawn and edited the same on the node card and in
// Properties. A tab per channel (RGB master, R, G, B, A; a dot marks a channel that is not at its
// defaults) shows that channel over the node input's luminance histogram (the #493 histogram:
// requested only while the graph paints, prepared off the interface thread) when the Histogram
// switch is on -- the same switch, and the same look, as the Curves card: one grey under the whole
// plot, a grid every quarter, the 0 and 1 marked on its foot. Under the plot, handles drag the
// input black, gamma and input white points; under the output bar, handles drag the output black
// and white. The parameters are the numeric Levels ones (#340), unchanged, and the five values are
// always shown under the graph ("Input": black, gamma, white; "Output": black, white).
//
// The gamma handle sits where the output is half way between black and white:
// black + (white - black) * 0.5^gamma, so dragging it left raises gamma and brightens the middle.
// The handles follow the shared grading rules (grading_controls.hpp): a press keeps the pointer's
// offset, so a handle never jumps; a Shift-press on a handle's track jumps the nearest handle to the
// pointer; Ctrl drags at a tenth of the speed and Shift at ten times, a modifier change starting a
// new segment. Each drag is one session value edit and one command on release; a double-click on a
// handle resets it and on the plot the channel; the right-click menu offers those resets and the
// channel's key. Escape, the right button or losing focus cancels. The channel's keyframe diamond
// keys its five values as one; its Values button (or Alt-click) shows their rows, with their
// sockets, to link them.

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

class KeyframeGroupDiamond;
namespace kit {
class KButton;
class KLabel;
class KSwitch;
class KTabBar;
class KValueField;
} // namespace kit

// The five handles, in the order of the Levels fields.
enum class LevelsHandle : std::uint8_t { InputBlack, InputWhite, Gamma, OutputBlack, OutputWhite };

// Where the gamma handle sits for an input range and gamma, and the gamma a handle at `x` means
// (limited to the Levels gamma domain).
[[nodiscard]] double levelsGammaHandle(double black, double white, double gamma) noexcept;
[[nodiscard]] double levelsGammaAt(double black, double white, double x) noexcept;
[[nodiscard]] QString levelsHandleName(LevelsHandle handle);

class LevelsGraphCanvas final : public QWidget {
    Q_OBJECT

  public:
    LevelsGraphCanvas(CompositionSession& session, document::NodeId node,
                      QWidget* parent = nullptr);

    // 0 the RGB master, then red, green, blue and alpha (document::kGradeChannelNames).
    [[nodiscard]] std::size_t channel() const noexcept { return channel_; }
    void setChannel(std::size_t channel);
    // The channel's five values now, in field order.
    [[nodiscard]] std::optional<std::array<double, document::kLevelsFieldCount>> values() const;
    [[nodiscard]] std::optional<document::ParameterId> parameter(LevelsHandle handle) const;
    // Test seams: the plot, the output bar and where a handle is drawn.
    [[nodiscard]] QRectF plotRect() const;
    [[nodiscard]] QRectF outputRect() const;
    [[nodiscard]] std::optional<QPointF> handlePosition(LevelsHandle handle) const;
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }
    void cancelDrag();
    bool resetHandle(LevelsHandle handle);
    // Sets the channel's five values to their defaults as one command.
    bool resetChannel();
    // The right-click menu for a click at `position`; the caller owns it (a test seam).
    [[nodiscard]] QMenu* makeContextMenu(QPointF position);

  Q_SIGNALS:
    void alternateClicked();
    void keyRequested();

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
    void leaveEvent(QEvent* event) override;

  private:
    struct Range final {
        double low = 0.0;
        double high = 1.0;
    };
    struct Drag final {
        LevelsHandle handle = LevelsHandle::InputBlack;
        document::ParameterId parameter;
        Range input;
        Range output;
        std::array<double, document::kLevelsFieldCount> base{};
        // The segment the pointer drags now: where the pointer and the handle were when it began
        // (in the handle's range), and its speed. A modifier change starts a new one.
        double fromX = 0.0;
        double origin = 0.0;
        double at = 0.0;
        double scale = 1.0;
        Qt::KeyboardModifiers modifiers;
    };
    [[nodiscard]] Range inputRange() const;
    [[nodiscard]] Range outputRange() const;
    [[nodiscard]] std::optional<LevelsHandle> handleAt(QPointF position) const;
    [[nodiscard]] std::optional<QPointF> handlePosition(LevelsHandle handle, const Range& input,
                                                        const Range& output) const;
    // The handle row under the pointer, for a Shift-press jump: the nearest handle of that row.
    [[nodiscard]] std::optional<LevelsHandle> nearestInRow(QPointF position) const;
    // The value a handle at x stands for (gamma's at the handle position).
    [[nodiscard]] double handleValueAt(LevelsHandle handle, double x) const;

    CompositionSession& session_;
    document::NodeId node_;
    std::size_t channel_ = 0;
    std::optional<Drag> drag_;
    std::optional<LevelsHandle> hovered_;
};

class LevelsCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    LevelsCardWidget(CompositionSession& session, document::NodeId node, bool card,
                     QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] QSize sizeHint() const override;

    [[nodiscard]] LevelsGraphCanvas* canvas() const noexcept { return canvas_; }
    [[nodiscard]] kit::KTabBar* tabs() const noexcept { return tabs_; }
    [[nodiscard]] KeyframeGroupDiamond* diamond() const noexcept { return diamond_; }
    [[nodiscard]] kit::KButton* valuesButton() const noexcept { return values_; }
    [[nodiscard]] kit::KButton* resetButton() const noexcept { return reset_; }
    [[nodiscard]] kit::KSwitch* histogram() const noexcept { return histogram_; }
    // The always-shown value of a handle.
    [[nodiscard]] kit::KValueField* field(LevelsHandle handle) const;
    void setValuesExpanded(bool expanded);
    [[nodiscard]] bool valuesExpanded() const noexcept { return expanded_; }
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void place();
    CompositionSession& session_;
    document::NodeId node_;
    kit::KTabBar* tabs_ = nullptr;
    KeyframeGroupDiamond* diamond_ = nullptr;
    kit::KButton* values_ = nullptr;
    kit::KSwitch* histogram_ = nullptr;
    kit::KButton* reset_ = nullptr;
    LevelsGraphCanvas* canvas_ = nullptr;
    kit::KLabel* inputLabel_ = nullptr;
    kit::KLabel* outputLabel_ = nullptr;
    std::array<kit::KValueField*, document::kLevelsFieldCount> fields_{};
    bool expanded_ = false;
};

} // namespace bloom::ui
