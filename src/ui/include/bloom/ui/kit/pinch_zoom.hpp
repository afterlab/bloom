#pragma once

#include <QObject>
#include <QPointF>

#include <functional>

class QWidget;

namespace bloom::ui::kit {

// Trackpad pinch to zoom (#696). A pinch reaches a widget as a Qt::ZoomNativeGesture: on macOS from
// any trackpad, and on Wayland and on Windows and X11 touchpads whose driver reports pinch. This
// filter turns each step of it into one zoom call, `factor` being the scale change of that step
// (1.0 = none, above 1 zooms in) and `position` where the fingers are, in the filtered widget's
// coordinates, so the surface zooms about the gesture rather than about its centre. The surface's
// own limits stay the surface's: the callback goes through the same zoom function the wheel uses.
//
// The filter is parented to the widget it filters and is gone with it. A QGraphicsView filters its
// viewport(), the widget the events are delivered to.
class PinchZoomFilter final : public QObject {
    Q_OBJECT

  public:
    using Zoom = std::function<void(QPointF position, double factor)>;

    PinchZoomFilter(QWidget& target, Zoom zoom);

    // The scale factor of a native zoom gesture whose `value` is the change since the last event;
    // clamped so one event can never zoom more than a factor of two either way.
    [[nodiscard]] static double factorForValue(double value) noexcept;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    Zoom zoom_;
};

// Installs a PinchZoomFilter on `target`.
inline PinchZoomFilter* installPinchZoom(QWidget& target, PinchZoomFilter::Zoom zoom) {
    return new PinchZoomFilter(target, std::move(zoom));
}

} // namespace bloom::ui::kit
