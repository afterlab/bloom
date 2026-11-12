#include <bloom/ui/kit/pinch_zoom.hpp>

#include <QEvent>
#include <QNativeGestureEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace bloom::ui::kit {

PinchZoomFilter::PinchZoomFilter(QWidget& target, Zoom zoom)
    : QObject(&target), zoom_(std::move(zoom)) {
    target.installEventFilter(this);
}

double PinchZoomFilter::factorForValue(const double value) noexcept {
    if (!std::isfinite(value))
        return 1.0;
    return std::clamp(1.0 + value, 0.5, 2.0);
}

bool PinchZoomFilter::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::NativeGesture)
        return QObject::eventFilter(watched, event);
    const auto* gesture = static_cast<QNativeGestureEvent*>(event);
    if (gesture->gestureType() != Qt::ZoomNativeGesture || !zoom_)
        return QObject::eventFilter(watched, event);
    if (const double factor = factorForValue(gesture->value()); factor != 1.0)
        zoom_(gesture->position(), factor);
    event->accept();
    return true;
}

} // namespace bloom::ui::kit
