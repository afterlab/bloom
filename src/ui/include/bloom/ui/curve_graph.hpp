#pragma once

// #493: the Curves node's graphical curve editor. CurveGraphField is the Properties widget: a tab
// per curve over one graph, where a click adds a point, a drag moves one, and Delete or a
// double-click removes one. A drag is a session value edit (ADR 0017): the graph and the Viewer
// follow it through the request-scoped override and its release commits exactly one command; a
// press that does not move commits nothing, and Escape, the right button or losing focus cancels
// it. paintCurveGraph() draws the same picture for the field and for the Curves node card.
//
// The graph shows the identity diagonal (a neutral line for the hue and Luma vs Saturation curves)
// and the 0..1 box; a channel curve's view grows past 1 to hold every point and always shows some
// HDR range above 1. The hue curves draw the hue strip of bloom/runtime/grade_curve.hpp behind
// them, wrapping at both edges. Behind a channel curve and Luma vs Saturation the graph can draw
// the node's input luminance histogram, requested from ImagePreviewController only while the graph
// paints.

#include <QPointF>
#include <QRectF>
#include <QWidget>

#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/ids.hpp>
#include <bloom/document/parameter.hpp>

#include <array>
#include <optional>
#include <span>
#include <vector>

class QPainter;

namespace bloom::ui {

class CompositionSession;
class KeyframeDiamond;
namespace kit {
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
    // The curve being edited, drawn with its points; the card's overview draws every channel curve
    // instead, without points.
    document::GradeCurve active = document::GradeCurve::Master;
    bool overview = false;
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

// The node's curve values as the session sees them now (a live drag, a key at the playhead, a
// constant), in kGradeCurves order.
[[nodiscard]] std::array<std::optional<document::PathValue>, document::kGradeCurveCount>
curveGraphValues(const CompositionSession& session, document::NodeId node);

class CurveGraphCanvas;

class CurveGraphField final : public QWidget {
    Q_OBJECT

  public:
    // `curves` are the tabs this field offers, in order: the channel curves or the hue curves.
    CurveGraphField(CompositionSession& session, document::NodeId node,
                    std::vector<document::GradeCurve> curves, QWidget* parent = nullptr);

    [[nodiscard]] document::GradeCurve curve() const noexcept;
    void setCurve(document::GradeCurve curve);
    // Re-reads the session: the tab's diamond, the selected point's fields, the graph.
    void refresh();
    [[nodiscard]] CurveGraphCanvas* canvas() const noexcept { return canvas_; }
    [[nodiscard]] kit::KTabBar* tabs() const noexcept { return tabs_; }

  private:
    void beginPointEdit();
    void updatePointEdit(bool input, double value);

    CompositionSession& session_;
    document::NodeId node_;
    std::vector<document::GradeCurve> curves_;
    kit::KTabBar* tabs_ = nullptr;
    KeyframeDiamond* diamond_ = nullptr;
    CurveGraphCanvas* canvas_ = nullptr;
    kit::KValueField* input_ = nullptr;
    kit::KValueField* output_ = nullptr;
    kit::KSwitch* histogram_ = nullptr;
    bool pointEdit_ = false;
};

// The graph itself, public for the field's tests.
class CurveGraphCanvas final : public QWidget {
    Q_OBJECT

  public:
    CurveGraphCanvas(CompositionSession& session, document::NodeId node,
                     QWidget* parent = nullptr);

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
    // Removes the selected point as one command (a channel curve keeps two).
    bool removeSelected();
    void cancelDrag();

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
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    struct Drag final {
        std::size_t point = 0;
        CurveGraphRange range;
        bool moved = false;
        QPointF pressed;
    };
    [[nodiscard]] std::optional<std::size_t> pointAt(QPointF position) const;
    [[nodiscard]] document::Vec2d clamped(document::Vec2d value, const CurveGraphRange& range) const;
    [[nodiscard]] bool removePoint(std::size_t point);

    CompositionSession& session_;
    document::NodeId node_;
    document::GradeCurve curve_ = document::GradeCurve::Master;
    std::optional<std::size_t> selected_;
    std::optional<std::size_t> hovered_;
    std::optional<Drag> drag_;
};

} // namespace bloom::ui
