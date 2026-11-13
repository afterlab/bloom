#pragma once

#include <QPointF>
#include <QRectF>
#include <QWidget>
#include <vector>

namespace bloom::ui {

namespace kit {
class KIconButton;
class KSlider;
} // namespace kit
class NodeGraphicsView;

// #695: the node editor's minimap, an overlay in the canvas's bottom-right corner. A child of the
// view's viewport, so it never reaches outside the canvas and a press inside it never reaches the
// canvas gestures beneath. It paints a scaled picture of the whole graph (every card and group
// frame) plus the rectangle the canvas currently shows; pressing or dragging in it centers the
// canvas on the pointer. It reads the scene and the view and edits nothing in the document.
//
// #721: a zoom row sits directly under the map, map-application style: zoom out, a horizontal
// zoom slider over the canvas's whole zoom range, zoom in. The slider is logarithmic (each step of
// travel is the same zoom ratio), follows every zoom change of the canvas and zooms about the
// view's center; the buttons step like the Zoom In and Zoom Out commands. The row shows and hides
// with the map, and has a preference of its own.
//
// The card rectangles are cached and refreshed only when the scene reports a change, so a repaint
// is drawing alone.
class NodeMinimap final : public QWidget {
    Q_OBJECT

  public:
    explicit NodeMinimap(NodeGraphicsView& view);

    // The scene rectangle the canvas shows right now.
    [[nodiscard]] QRectF visibleSceneRect() const;
    // The cached card and group rectangles, in scene coordinates.
    [[nodiscard]] const std::vector<QRectF>& cardRects() const noexcept { return cards_; }
    // What the minimap draws: every cached rectangle united with the visible scene rectangle.
    [[nodiscard]] QRectF contentRect() const;
    // The mapping the painter and the pointer share, between scene and minimap coordinates.
    [[nodiscard]] QRectF toMinimap(const QRectF& scene) const;
    [[nodiscard]] QPointF toScene(const QPointF& minimapPoint) const;
    // The visible-viewport rectangle as painted, in minimap coordinates.
    [[nodiscard]] QRectF viewportRect() const;
    // Whether the minimap is on: the preference, a non-empty graph and a canvas big enough to
    // leave room around it.
    void setEnabledByPreference(bool enabled);
    [[nodiscard]] bool enabledByPreference() const noexcept { return enabled_; }
    // The zoom row under the map: shown with the map unless its own preference turns it off.
    void setZoomRowEnabledByPreference(bool enabled);
    // The map alone, in the minimap's coordinates: the plate the graph is painted on.
    [[nodiscard]] QRectF mapRect() const { return plate(); }
    // Re-reads the cards from the scene; a no-op when nothing moved.
    void refreshCards();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void place();
    void updateVisibility();
    void scheduleRefresh();
    void buildZoomRow();
    void layoutZoomRow();
    void refreshZoomRow();
    void panTo(const QPointF& minimapPoint);
    [[nodiscard]] QRectF plate() const;
    [[nodiscard]] QRectF inner() const;

    NodeGraphicsView& view_;
    std::vector<QRectF> cards_;
    std::vector<QRectF> frames_;
    bool enabled_ = true;
    bool dragging_ = false;
    bool refreshPending_ = false;
    bool zoomRowEnabled_ = true;
    bool zoomSyncing_ = false;
    kit::KIconButton* zoomOut_ = nullptr;
    kit::KSlider* zoomSlider_ = nullptr;
    kit::KIconButton* zoomIn_ = nullptr;
    // The scene rectangle the mapping covers while a drag is in progress.
    QRectF frozenContent_;
};

} // namespace bloom::ui
