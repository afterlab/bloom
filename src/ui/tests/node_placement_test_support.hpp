#pragma once
// #703: a node chosen from an Add surface follows the pointer until a click places it. Tests that
// add through Add (and are not about placement itself) finish that placement the artist's way: the
// pointer moves to the point Add was invoked at and clicks there.
#include "node_editor_items.hpp"

#include <bloom/ui/node_editor.hpp>

#include <QCoreApplication>
#include <QMouseEvent>

namespace bloom::ui::test {
// Moves the pointer to `scene` and clicks there; returns false when nothing was being placed.
inline bool completePlacement(NodeGraphEditor& editor, const QPointF scene) {
    if (editor.graphScene()->placementPreviewForTest() == nullptr)
        return false;
    auto* view = editor.graphView();
    const QPoint at = view->mapFromScene(scene);
    const QPoint global = view->viewport()->mapToGlobal(at);
    QMouseEvent move(QEvent::MouseMove, at, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view->viewport(), &move);
    QMouseEvent press(QEvent::MouseButtonPress, at, global, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(view->viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, global, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(view->viewport(), &release);
    QCoreApplication::processEvents();
    return true;
}

// Clicks where the preview already is: under its title band.
inline bool completePlacement(NodeGraphEditor& editor) {
    const auto* preview = editor.graphScene()->placementPreviewForTest();
    if (preview == nullptr)
        return false;
    return completePlacement(editor, preview->pos() +
                                         QPointF(node_editor::kCardMinimumWidth / 2.0,
                                                 node_editor::kCardHeaderHeight / 2.0));
}
} // namespace bloom::ui::test
