#pragma once

// #501: the Hue/Saturation node's card widget, drawn and edited the same on the node card and in
// Properties. The Hue row (its number in degrees, its keyframe diamond, its socket beside it) sits
// over the hue strip: the input spectrum above the output spectrum, each input hue drawn in the
// colour the node turns it into (the node's own matrix, runtime::hueSaturationMatrix), so the shift
// is seen rather than read. Dragging the strip slides the output spectrum with the pointer and
// turns the hue, wrapping at +-180 degrees. The Saturation row sits over its slider, grey at 0,
// the colour as it is at 1 (ticked, with a detent) and more vivid toward 2. Both follow the shared
// grading rules (grading_controls.hpp): relative drags, Ctrl a tenth and Shift ten times the
// speed, a double-click resets, the right-click menu offers Reset and the value's key.

#include <bloom/document/ids.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <QWidget>

#include <optional>
#include <string_view>

class QMenu;

namespace bloom::ui {

class GradingValueRow;

// The hue a strip drag of `pixels` (positive to the right) reaches from `from`, over a strip
// `width` pixels wide at `scale` times the speed: the output spectrum follows the pointer, and the
// result wraps into (-180, 180].
[[nodiscard]] double hueStripDragValue(double from, double pixels, double width, double scale);

class HueShiftStrip final : public QWidget {
    Q_OBJECT

  public:
    HueShiftStrip(CompositionSession& session, document::NodeId node, QWidget* parent = nullptr);

    [[nodiscard]] std::optional<document::ParameterId> parameter() const;
    [[nodiscard]] double hue() const;
    // Where the two spectra are drawn (test seams).
    [[nodiscard]] QRectF inputBand() const;
    [[nodiscard]] QRectF outputBand() const;
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }
    void cancelDrag();
    bool reset();
    // The right-click menu; the caller owns it (a test seam: the event shows it).
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
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    struct Drag final {
        document::ParameterId parameter;
        // The segment the pointer drags now; a modifier change starts a new one.
        double fromX = 0.0;
        double base = 0.0;
        double scale = 1.0;
        Qt::KeyboardModifiers modifiers;
    };
    [[nodiscard]] qreal captionWidth() const;
    CompositionSession& session_;
    document::NodeId node_;
    std::optional<Drag> drag_;
    bool hovered_ = false;
};

class HueSaturationCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    HueSaturationCardWidget(CompositionSession& session, document::NodeId node, bool card,
                            QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] std::optional<int> socketY(std::string_view role) const override;
    [[nodiscard]] QSize sizeHint() const override;

    [[nodiscard]] GradingValueRow* hueRow() const noexcept { return hue_; }
    [[nodiscard]] GradingValueRow* saturationRow() const noexcept { return saturation_; }
    [[nodiscard]] HueShiftStrip* strip() const noexcept { return strip_; }
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void place();
    CompositionSession& session_;
    document::NodeId node_;
    GradingValueRow* hue_ = nullptr;
    HueShiftStrip* strip_ = nullptr;
    GradingValueRow* saturation_ = nullptr;
};

} // namespace bloom::ui
