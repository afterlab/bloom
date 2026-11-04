#pragma once

// The house scrub's pointer wrap, shared by KValueField's scrub and KSlider's jog: where the
// windowing system lets an application move the pointer (X11, Windows, macOS), a pointer within a
// margin of the screen edge jumps to the other side and every later x is shifted back by the jump,
// so a drag never runs out of travel. Wayland gives an application no way to move the pointer, so
// there the drag ends at the screen edge.

#include <bloom/ui/kit/tokens.hpp>

#include <QCursor>
#include <QGuiApplication>
#include <QPoint>
#include <QPointF>
#include <QScreen>
#include <QWidget>

#include <cmath>

namespace bloom::ui::kit {

[[nodiscard]] inline bool pointerCanWarp() {
    static const bool warps = [] {
        const auto platform = QGuiApplication::platformName();
        return platform == QLatin1String("xcb") || platform == QLatin1String("windows") ||
               platform == QLatin1String("cocoa");
    }();
    return warps;
}

// Wraps the pointer at `global` when it nears the edge of `widget`'s screen; `offset` collects the
// jumps, to be added to every later pointer x.
inline void wrapPointerAtScreenEdge(const QWidget& widget, const QPointF& global, double& offset) {
    if (!pointerCanWarp() || widget.screen() == nullptr)
        return;
    const QRect bounds = widget.screen()->geometry();
    const int margin = px(Size::ScrubWrapMargin);
    const int x = static_cast<int>(std::lround(global.x()));
    int target = x;
    if (x <= bounds.left() + margin)
        target = bounds.right() - margin - 1;
    else if (x >= bounds.right() - margin)
        target = bounds.left() + margin + 1;
    if (target == x)
        return;
    QCursor::setPos(widget.screen(), QPoint(target, static_cast<int>(std::lround(global.y()))));
    offset -= target - x;
}

} // namespace bloom::ui::kit
