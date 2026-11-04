#pragma once

// #494: the Levels node's card widget, drawn and edited the same on the node card and in
// Properties. A tab per channel (RGB master, R, G, B, A) shows that channel over the node input's
// luminance histogram (the #493 histogram: requested only while the graph paints, prepared off the
// interface thread). Under the histogram, handles drag the input black, gamma and input white
// points; under the output bar, handles drag the output black and white. The parameters are the
// numeric Levels ones (#340), unchanged.
//
// The gamma handle sits where the output is half way between black and white:
// black + (white - black) * 0.5^gamma, so dragging it left raises gamma and brightens the middle.
// Each drag is one session value edit and one command on release; double-clicking a handle resets
// it; Escape, the right button or losing focus cancels. Alt-click opens the channel's Values rows.

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
class KTabBar;
} // namespace kit

// The five handles, in the order of the Levels fields.
enum class LevelsHandle : std::uint8_t { InputBlack, InputWhite, Gamma, OutputBlack, OutputWhite };

// Where the gamma handle sits for an input range and gamma, and the gamma a handle at `x` means
// (limited to the Levels gamma domain).
[[nodiscard]] double levelsGammaHandle(double black, double white, double gamma) noexcept;
[[nodiscard]] double levelsGammaAt(double black, double white, double x) noexcept;

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
    };
    [[nodiscard]] Range inputRange() const;
    [[nodiscard]] Range outputRange() const;
    [[nodiscard]] std::optional<LevelsHandle> handleAt(QPointF position) const;
    [[nodiscard]] std::optional<QPointF> handlePosition(LevelsHandle handle, const Range& input,
                                                        const Range& output) const;

    CompositionSession& session_;
    document::NodeId node_;
    std::size_t channel_ = 0;
    std::optional<Drag> drag_;
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
    void setValuesExpanded(bool expanded);
    [[nodiscard]] bool valuesExpanded() const noexcept { return expanded_; }

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    CompositionSession& session_;
    document::NodeId node_;
    kit::KTabBar* tabs_ = nullptr;
    kit::KButton* values_ = nullptr;
    LevelsGraphCanvas* canvas_ = nullptr;
    bool expanded_ = false;
};

} // namespace bloom::ui
