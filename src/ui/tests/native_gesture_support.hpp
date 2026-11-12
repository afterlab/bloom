#pragma once

// Trackpad pinch events for tests and captures: a Qt::ZoomNativeGesture sent to a widget the way a
// macOS trackpad, or a Linux or Windows touchpad that reports pinch, delivers it.

#include <QCoreApplication>
#include <QNativeGestureEvent>
#include <QPointF>
#include <QPointingDevice>
#include <QWidget>

namespace bloom::ui::test {

[[nodiscard]] inline const QPointingDevice* testTrackpad() {
    static const QPointingDevice device(
        QStringLiteral("test trackpad"), 1, QInputDevice::DeviceType::TouchPad,
        QPointingDevice::PointerType::Finger,
        QInputDevice::Capability::Position | QInputDevice::Capability::Scroll, 2, 0);
    return &device;
}

// One zoom step of a pinch: `value` is the scale change since the last event (0.1 = 10% larger,
// the way Qt reports it); `local` is the gesture's position in `target`'s coordinates.
inline bool sendPinch(QWidget* target, const QPointF local, const qreal value,
                      const Qt::NativeGestureType type = Qt::ZoomNativeGesture) {
    QNativeGestureEvent event(type, testTrackpad(), 2, local, target->mapTo(target->window(), local.toPoint()),
                              target->mapToGlobal(local.toPoint()), value, QPointF(), 0);
    event.setAccepted(false);
    QCoreApplication::sendEvent(target, &event);
    return event.isAccepted();
}

} // namespace bloom::ui::test
