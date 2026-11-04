#pragma once

// #493, #498: the Curves node's graphical curve editor, drawn and edited the same on the node card
// and in Properties (CurvesCardWidget, a NodeCardWidget). A tab bar picks the channel curve (RGB,
// R, G, B, A) or the hue curves, whose curve is picked by name; a Preset menu and the curve's
// keyframe diamond sit beside it, the graph below, and under it the selected point's input and
// output, always shown, with their units.
//
// In the graph (CurveGraphCanvas) a click on the background adds a point there and the drag that
// follows moves it; a press on a point drags it relative to where it was pressed, so it never
// jumps; Ctrl (Cmd on macOS) moves it at a tenth of the speed, and pressing or releasing Ctrl
// mid-drag continues from where the point is. Dragging a point off the graph removes it (a channel
// curve keeps two), and dragging it back restores it. Every gesture is a session value edit (ADR
// 0017): the graph and the Viewer follow it and the release commits exactly one command, or a key
// at the playhead on a keyed curve; Escape or losing focus cancels it. A double-click resets the
// curve. The right-click menu offers Delete Point, Reset Curve and the curve's key at the playhead.
// paintCurveGraph() draws the picture.
//
// The graph shows the identity diagonal (a neutral line for the hue and Luma vs Saturation curves)
// and the 0..1 box; a channel curve's view grows past 1 to hold every point and always shows some
// HDR range above 1. The hue curves and Luma vs Saturation name their axes and draw a colour band
// along each: the hue strip of bloom/runtime/grade_curve.hpp (wrapping at both edges), black to
// white for luma, grey to colour for saturation, and the hue turned by the shift for Hue vs Hue.
// Behind a channel curve and Luma vs Saturation the graph can draw the node's input luminance
// histogram, requested from ImagePreviewController only while the graph paints.

#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/grade_curves.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/ui/node_card_widget.hpp>

#include <array>
#include <optional>
#include <span>
#include <vector>

class QMenu;
class QPainter;

namespace bloom::ui {

class CompositionSession;
class KeyframeDiamond;
namespace kit {
class KButton;
class KDropdown;
class KLabel;
class KTabBar;
class KSwitch;
class KValueField;
} // namespace kit

// The value range a graph shows: input left..right, output bottom..top.
struct CurveGraphRange final {
    double left = 0.0;
    double right = 1.0;
    double bottom = 0.0;
    double top = 1.0;
    friend bool operator==(const CurveGraphRange&, const CurveGraphRange&) = default;
};

// What one graph draws.
struct CurveGraphPicture final {
    // The curve being edited, drawn with its points.
    document::GradeCurve active = document::GradeCurve::Master;
    std::array<std::optional<document::PathValue>, document::kGradeCurveCount> curves{};
    // Normalised bin heights over input 0..1; empty draws none.
    std::vector<float> histogram;
    std::optional<std::size_t> selected;
    std::optional<std::size_t> hovered;
    CurveGraphRange range;
};

// The range that holds `curve`'s domain view and every point of `values` (a channel curve always
// keeps some HDR range above 1).
[[nodiscard]] CurveGraphRange curveGraphRange(document::GradeCurve curve,
                                              std::span<const document::PathValue> values);
// Maps between values and positions inside `rect` (the plot area, y up).
[[nodiscard]] QPointF curveGraphPoint(const QRectF& rect, const CurveGraphRange& range,
                                      document::Vec2d value);
[[nodiscard]] document::Vec2d curveGraphValue(const QRectF& rect, const CurveGraphRange& range,
                                              QPointF point);
void paintCurveGraph(QPainter& painter, const QRectF& rect, const CurveGraphPicture& picture);
// The axis names a curve's graph draws (empty for a channel curve, whose axes are its values).
[[nodiscard]] QString curveGraphInputName(document::GradeCurve curve);
[[nodiscard]] QString curveGraphOutputName(document::GradeCurve curve);
// A curve's full name, and a preset's.
[[nodiscard]] QString gradeCurveName(document::GradeCurve curve);
[[nodiscard]] QString gradeCurvePresetName(document::GradeCurve curve,
                                           document::GradeCurvePreset preset);

// The node's curve values as the session sees them now (a live drag, a key at the playhead, a
// constant), in kGradeCurves order.
[[nodiscard]] std::array<std::optional<document::PathValue>, document::kGradeCurveCount>
curveGraphValues(const CompositionSession& session, document::NodeId node);

// The graph itself.
class CurveGraphCanvas final : public QWidget {
    Q_OBJECT

  public:
    CurveGraphCanvas(CompositionSession& session, document::NodeId node, QWidget* parent = nullptr);

    [[nodiscard]] document::GradeCurve curve() const noexcept { return curve_; }
    void setCurve(document::GradeCurve curve);
    [[nodiscard]] std::optional<std::size_t> selected() const noexcept { return selected_; }
    void setSelected(std::optional<std::size_t> point);
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }
    // The parameter of the shown curve, and its points now.
    [[nodiscard]] std::optional<document::ParameterId> parameter() const;
    [[nodiscard]] std::optional<document::PathValue> value() const;
    // Where the plot sits and what it shows (test seams).
    [[nodiscard]] QRectF plotRect() const;
    [[nodiscard]] CurveGraphRange range() const;
    // Removes the selected point, or `point`, as one command (a channel curve keeps two).
    bool removeSelected();
    bool removePoint(std::size_t point);
    // Sets the shown curve to `preset` (its default for Linear or Reset) as one command.
    bool applyPreset(document::GradeCurvePreset preset);
    bool resetCurve();
    void cancelDrag();
    // The right-click menu for a click at `position`; the caller owns it (a test seam: the event
    // shows it).
    [[nodiscard]] QMenu* makeContextMenu(QPointF position);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  Q_SIGNALS:
    void selectionChanged();

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
    struct Drag final {
        std::size_t point = 0;
        CurveGraphRange range;
        // The curve when the gesture began (with an added point in it).
        document::PathValue base;
        // The segment the pointer drags now: where the point and the pointer were when it began,
        // and whether it is fine. A modifier change starts a new segment, so nothing jumps.
        document::Vec2d origin;
        QPointF from;
        // Where the point is now.
        document::Vec2d at;
        bool fine = false;
        // The point is dragged off the graph, so the gesture removes it.
        bool off = false;
        // The press added the point.
        bool added = false;
    };
    [[nodiscard]] std::optional<std::size_t> pointAt(QPointF position) const;
    [[nodiscard]] document::Vec2d clamped(document::Vec2d value,
                                          const CurveGraphRange& range) const;
    [[nodiscard]] std::size_t minimumPoints() const noexcept;
    [[nodiscard]] bool offGraph(QPointF position) const;
    void dragTo(QPointF position, bool fine);

    CompositionSession& session_;
    document::NodeId node_;
    document::GradeCurve curve_ = document::GradeCurve::Master;
    std::optional<std::size_t> selected_;
    std::optional<std::size_t> hovered_;
    std::optional<Drag> drag_;
    // The revision a click that added a point committed, so the double-click it begins resets the
    // curve without leaving that point in the history.
    std::optional<document::Revision> addedRevision_;
};

class CurvesCardWidget final : public NodeCardWidget {
    Q_OBJECT

  public:
    CurvesCardWidget(CompositionSession& session, document::NodeId node, bool card,
                     QWidget* parent = nullptr);

    [[nodiscard]] bool showsRole(std::string_view role) const override;
    [[nodiscard]] int heightAt(int width) const override;
    [[nodiscard]] int minimumWidthHint() const override;
    [[nodiscard]] QSize sizeHint() const override;

    [[nodiscard]] document::GradeCurve curve() const noexcept;
    void setCurve(document::GradeCurve curve);
    [[nodiscard]] CurveGraphCanvas* canvas() const noexcept { return canvas_; }
    [[nodiscard]] kit::KTabBar* tabs() const noexcept { return tabs_; }
    [[nodiscard]] kit::KDropdown* hueCurves() const noexcept { return hueCurves_; }
    [[nodiscard]] kit::KButton* presets() const noexcept { return presets_; }
    [[nodiscard]] kit::KSwitch* histogram() const noexcept { return histogram_; }
    // The Preset menu for the shown curve; the caller owns it (a test seam: the button shows it).
    [[nodiscard]] QMenu* makePresetMenu();
    [[nodiscard]] KeyframeDiamond* diamond() const noexcept { return diamond_; }
    [[nodiscard]] kit::KValueField* input() const noexcept { return input_; }
    [[nodiscard]] kit::KValueField* output() const noexcept { return output_; }
    // Re-reads the session: the diamond, the selected point's fields, the graph.
    void refresh();

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void place();
    // Points the tabs, the diamond, the axis names and the fields at the shown curve.
    void configureCurve();
    void beginPointEdit();
    void updatePointEdit(bool input, double value);

    CompositionSession& session_;
    document::NodeId node_;
    kit::KTabBar* tabs_ = nullptr;
    kit::KDropdown* hueCurves_ = nullptr;
    kit::KButton* presets_ = nullptr;
    KeyframeDiamond* diamond_ = nullptr;
    CurveGraphCanvas* canvas_ = nullptr;
    kit::KValueField* input_ = nullptr;
    kit::KValueField* output_ = nullptr;
    kit::KLabel* inputLabel_ = nullptr;
    kit::KLabel* outputLabel_ = nullptr;
    kit::KSwitch* histogram_ = nullptr;
    // The hue curve the Hue tab shows.
    document::GradeCurve hueCurve_ = document::GradeCurve::HueVsHue;
    bool pointEdit_ = false;
};

} // namespace bloom::ui
