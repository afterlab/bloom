// #541: the kit's interactive value controls -- the gradient editor, the angle dial, the point pad
// and the range selector's crossing ends -- edit by direct manipulation, keep the kit's edit-session
// contract (one gesture is one editStarted/editFinished pair; a cancel restores), and never jump a
// value on a plain press.

#include <bloom/ui/kit/angle_dial.hpp>
#include <bloom/ui/kit/gradient_editor.hpp>
#include <bloom/ui/kit/point_pad.hpp>
#include <bloom/ui/kit/range_selector.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QWidget>

#include <cmath>
#include <iostream>
#include <source_location>
#include <string>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (condition)
        return;
    ++failures;
    std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
}

using namespace bloom::ui;

void mouse(QWidget& widget, const QEvent::Type type, const QPointF& point,
           const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const Qt::MouseButtons held =
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton);
    QMouseEvent event(type, point, widget.mapToGlobal(point), Qt::LeftButton, held, modifiers);
    QCoreApplication::sendEvent(&widget, &event);
    QCoreApplication::processEvents();
}

void key(QWidget& widget, const int code, const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent press(QEvent::KeyPress, code, modifiers);
    QCoreApplication::sendEvent(&widget, &press);
    QKeyEvent release(QEvent::KeyRelease, code, modifiers);
    QCoreApplication::sendEvent(&widget, &release);
    QCoreApplication::processEvents();
}

kit::KColor reference(const float r, const float g, const float b) {
    return kit::KColor::fromRgba(r, g, b, 1.0F, kit::ColorSpace::Reference);
}

void testGradientEditorEditsStops() {
    QWidget host;
    auto& editor = *new kit::KGradientEditor(&host);
    editor.setGeometry(0, 0, 300, editor.sizeHint().height());
    editor.setStops({{1.0, reference(1.0F, 1.0F, 1.0F)}, {0.0, reference(0.0F, 0.0F, 0.0F)}});
    host.show();
    QCoreApplication::processEvents();
    expect(editor.stops().size() == 2 && editor.stops().front().position == 0.0,
           "stops are kept sorted by position");
    const auto mid = editor.colorAt(0.5);
    expect(std::abs(mid.red - 0.5F) < 1e-6F && mid.space == kit::ColorSpace::Reference,
           "the gradient interpolates Reference colour linearly between stops");
    expect(editor.colorAt(-1.0).red == 0.0F && editor.colorAt(2.0).red == 1.0F,
           "the ends hold past the bar");

    QSignalSpy started(&editor, &kit::KGradientEditor::editStarted);
    QSignalSpy finished(&editor, &kit::KGradientEditor::editFinished);
    QSignalSpy changed(&editor, &kit::KGradientEditor::stopsChanged);
    // A press on the bare bar adds a stop there, coloured as the gradient is, and is one edit.
    const auto bar = editor.barRect();
    const QPointF quarter(bar.left() + bar.width() * 0.25, bar.center().y());
    mouse(editor, QEvent::MouseButtonPress, quarter);
    mouse(editor, QEvent::MouseButtonRelease, quarter);
    expect(editor.stops().size() == 3 && std::abs(editor.stops()[1].position - 0.25) < 1e-6 &&
               std::abs(editor.stops()[1].color.red - 0.25F) < 1e-3F,
           "a press on the bar adds a stop with the colour already there");
    expect(started.size() == 1 && finished.size() == 1 && !changed.isEmpty(),
           "adding a stop is one edit");
    expect(editor.selectedStop() == std::optional<std::size_t>{1}, "the added stop is selected");

    // Dragging a stop's handle moves it by the pointer's travel; passing a neighbour reorders.
    const QPointF handle = editor.stopCentre(1);
    mouse(editor, QEvent::MouseButtonPress, handle);
    expect(std::abs(editor.stops()[1].position - 0.25) < 1e-9, "a press on a stop never moves it");
    mouse(editor, QEvent::MouseMove, handle + QPointF(bar.width() * 0.5, 0.0));
    expect(std::abs(editor.stops()[1].position - 0.75) < 1e-6, "a drag moves the stop");
    mouse(editor, QEvent::MouseMove, handle + QPointF(bar.width() * 0.5, 0.0), Qt::ControlModifier);
    mouse(editor, QEvent::MouseMove, handle + QPointF(bar.width() * 0.6, 0.0), Qt::ControlModifier);
    expect(std::abs(editor.stops()[1].position - 0.76) < 1e-6,
           "Ctrl moves a stop at a tenth of the speed, from where it is");
    mouse(editor, QEvent::MouseButtonRelease, handle + QPointF(bar.width() * 0.6, 0.0));
    expect(started.size() == finished.size(), "every drag is one complete edit");

    // Pulled well off the bar a stop is removed, and back over it comes back.
    const auto count = editor.stops().size();
    const QPointF stop = editor.stopCentre(1);
    const QPointF away = stop + QPointF(0.0, kit::px(kit::Size::DropdownWidth));
    mouse(editor, QEvent::MouseButtonPress, stop);
    mouse(editor, QEvent::MouseMove, away);
    expect(editor.stops().size() == count - 1 && editor.removalPending(),
           "a stop pulled off the bar is removed");
    mouse(editor, QEvent::MouseMove, stop);
    expect(editor.stops().size() == count && !editor.removalPending(),
           "brought back before the release it returns");
    mouse(editor, QEvent::MouseMove, away);
    mouse(editor, QEvent::MouseButtonRelease, away);
    expect(editor.stops().size() == count - 1, "released off the bar it stays removed");

    // The editor keeps its minimum: the last two stops cannot be pulled off.
    editor.setStopLimits(2, 64);
    const auto two = editor.stops().size();
    expect(two == 2 && !editor.removeStop(0), "the last stops cannot be removed");

    // A cancelled gesture restores the stops it started from.
    const auto before = editor.stops();
    const QPointF first = editor.stopCentre(0);
    QSignalSpy cancelled(&editor, &kit::KGradientEditor::editCancelled);
    mouse(editor, QEvent::MouseButtonPress, first);
    mouse(editor, QEvent::MouseMove, first + QPointF(bar.width() * 0.3, 0.0));
    editor.cancelEdit();
    expect(editor.stops() == before && cancelled.size() == 1 && !editor.hasLiveEdit(),
           "a cancelled drag restores the stops");
    mouse(editor, QEvent::MouseButtonRelease, first);

    // Reverse and distribute are one edit each.
    editor.setStops({{0.0, reference(0.0F, 0.0F, 0.0F)},
                     {0.1, reference(1.0F, 0.0F, 0.0F)},
                     {1.0, reference(1.0F, 1.0F, 1.0F)}});
    editor.distributeStops();
    expect(std::abs(editor.stops()[1].position - 0.5) < 1e-12, "distribute spaces stops evenly");
    editor.reverseStops();
    expect(editor.stops().front().color.red == 1.0F && editor.stops().front().color.green == 1.0F &&
               editor.stops().back().color.red == 0.0F,
           "reverse mirrors the gradient");

    // The keyboard moves the selected stop; Tab walks the stops.
    editor.setSelectedStop(1);
    editor.setFocus();
    key(editor, Qt::Key_Right);
    expect(std::abs(editor.stops()[1].position - (0.5 + kit::kKeyStepFraction)) < 1e-9,
           "an arrow key steps the selected stop");
    key(editor, Qt::Key_Delete);
    expect(editor.stops().size() == 2, "Delete removes the selected stop");
    // A double-click on a stop asks its owner for the colour picker.
    QSignalSpy requested(&editor, &kit::KGradientEditor::stopColorRequested);
    mouse(editor, QEvent::MouseButtonDblClick, editor.stopCentre(0));
    expect(requested.size() == 1, "a double-click on a stop asks for its colour");
    mouse(editor, QEvent::MouseButtonRelease, editor.stopCentre(0));
}

void testAngleDialTurnsAndWinds() {
    QWidget host;
    auto& dial = *new kit::KAngleDial(&host);
    dial.setGeometry(0, 0, 100, 100);
    dial.setDefaultValue(0.0);
    host.show();
    QCoreApplication::processEvents();
    const auto centre = dial.discRect().center();
    const double radius = dial.discRect().width() / 2.0;
    expect(std::abs(dial.angleAt(centre + QPointF(radius, 0.0)) - 90.0) < 1e-9,
           "zero is up and a positive angle turns clockwise");
    dial.setZero(kit::KAngleDial::Zero::Right);
    expect(std::abs(dial.angleAt(centre + QPointF(0.0, radius)) - 90.0) < 1e-9,
           "zero may point right");
    dial.setCounterClockwise(true);
    expect(std::abs(dial.angleAt(centre + QPointF(0.0, -radius)) - 90.0) < 1e-9,
           "a counter-clockwise dial turns the other way");
    dial.setCounterClockwise(false);
    dial.setZero(kit::KAngleDial::Zero::Up);

    QSignalSpy started(&dial, &kit::KAngleDial::editStarted);
    QSignalSpy finished(&dial, &kit::KAngleDial::editFinished);
    // A press never jumps: the needle turns by the pointer's turn around the centre.
    const QPointF right = centre + QPointF(radius * 0.8, 0.0);
    const QPointF down = centre + QPointF(0.0, radius * 0.8);
    const QPointF left = centre + QPointF(-radius * 0.8, 0.0);
    const QPointF up = centre + QPointF(0.0, -radius * 0.8);
    mouse(dial, QEvent::MouseButtonPress, right);
    expect(dial.value() == 0.0, "a press on the dial does not jump");
    mouse(dial, QEvent::MouseMove, down);
    expect(std::abs(dial.value() - 90.0) < 1e-6, "a quarter turn of the pointer is 90 degrees");
    // It winds: a full turn keeps counting.
    mouse(dial, QEvent::MouseMove, left);
    mouse(dial, QEvent::MouseMove, up);
    mouse(dial, QEvent::MouseMove, right);
    mouse(dial, QEvent::MouseMove, down);
    expect(std::abs(dial.value() - 450.0) < 1e-6, "turning past a revolution winds the value");
    mouse(dial, QEvent::MouseButtonRelease, down);
    expect(started.size() == 1 && finished.size() == 1, "a drag is one edit");
    // Shift snaps; a Shift-press jumps to the pointer by the shorter way round.
    mouse(dial, QEvent::MouseButtonPress, left, Qt::ShiftModifier);
    expect(std::abs(dial.value() - 630.0) < 1e-6,
           "a Shift-press jumps the needle to the pointer, keeping its winding");
    mouse(dial, QEvent::MouseButtonRelease, left);
    // A double-click resets it.
    mouse(dial, QEvent::MouseButtonDblClick, up);
    mouse(dial, QEvent::MouseButtonRelease, up);
    expect(dial.value() == 0.0, "a double-click resets the dial");
    // Keys step a degree, Page Up a snap step.
    dial.setFocus();
    key(dial, Qt::Key_Right);
    key(dial, Qt::Key_PageUp);
    expect(std::abs(dial.value() - 16.0) < 1e-9, "keys step the dial");
    // A cancelled drag restores the value.
    mouse(dial, QEvent::MouseButtonPress, right);
    mouse(dial, QEvent::MouseMove, down);
    dial.cancelEdit();
    expect(std::abs(dial.value() - 16.0) < 1e-9 && !dial.hasLiveEdit(),
           "a cancelled drag restores the angle");
    mouse(dial, QEvent::MouseButtonRelease, down);
}

void testPointPadDragsPoints() {
    QWidget host;
    auto& pad = *new kit::KPointPad(&host);
    pad.setGeometry(0, 0, 320, 200);
    pad.setFrame(QRectF(0.0, 0.0, 1920.0, 1080.0));
    pad.setPoints({{QStringLiteral("Start"), QPointF(0.0, 540.0), QPointF(0.0, 540.0)},
                   {QStringLiteral("End"), QPointF(1920.0, 540.0), QPointF(1920.0, 540.0)}});
    pad.setConnected(true);
    host.show();
    QCoreApplication::processEvents();
    const auto frame = pad.frameRect();
    expect(std::abs(frame.width() / frame.height() - 1920.0 / 1080.0) < 1e-6,
           "the frame is drawn at its aspect ratio");
    const auto start = pad.screenPoint(pad.value(0));
    expect(std::abs(pad.valueAt(start).x() - 0.0) < 1e-6 &&
               std::abs(pad.valueAt(start).y() - 540.0) < 1e-6,
           "screen and value positions agree");
    QSignalSpy started(&pad, &kit::KPointPad::editStarted);
    QSignalSpy finished(&pad, &kit::KPointPad::editFinished);
    mouse(pad, QEvent::MouseButtonPress, start + QPointF(2.0, 1.0));
    expect(pad.value(0) == QPointF(0.0, 540.0), "a press on a point never moves it");
    const double units = 1920.0 / frame.width();
    mouse(pad, QEvent::MouseMove, start + QPointF(12.0, 1.0));
    expect(std::abs(pad.value(0).x() - 10.0 * units) < 1e-6 && pad.value(0).y() == 540.0,
           "a point moves by the pointer's travel in the frame's units");
    mouse(pad, QEvent::MouseButtonRelease, start + QPointF(12.0, 1.0));
    expect(started.size() == 1 && finished.size() == 1 && pad.activePoint() == 0,
           "a drag is one edit and leaves its point active");
    // A Shift-press on bare pad jumps the active point to the pointer.
    const QPointF centre = frame.center();
    mouse(pad, QEvent::MouseButtonPress, centre, Qt::ShiftModifier);
    mouse(pad, QEvent::MouseButtonRelease, centre);
    expect(std::abs(pad.value(0).x() - 960.0) < 1.0 && std::abs(pad.value(0).y() - 540.0) < 1.0,
           "a Shift-press jumps the active point");
    // Keys move the active point; Tab walks to the next.
    pad.setFocus();
    key(pad, Qt::Key_Right);
    expect(std::abs(pad.value(0).x() - 961.0) < 1.0, "an arrow moves the active point a unit");
    // A double-click on a point resets every point.
    mouse(pad, QEvent::MouseButtonDblClick, pad.screenPoint(pad.value(1)));
    mouse(pad, QEvent::MouseButtonRelease, pad.screenPoint(pad.value(1)));
    expect(pad.value(0) == QPointF(0.0, 540.0) && pad.value(1) == QPointF(1920.0, 540.0),
           "a double-click resets the points");
}

void testRangeEndsMayCross() {
    kit::KRangeSelector range;
    range.setRange(0.0, 1.0);
    range.setValues(0.2, 0.8);
    range.setLowerValue(0.9);
    expect(range.lowerValue() == 0.8, "by default the ends cannot cross");
    range.setHandlesMayCross(true);
    range.setLowerValue(0.9);
    expect(range.lowerValue() == 0.9 && range.upperValue() == 0.8,
           "crossing ends move independently");
    range.setValues(0.7, 0.3);
    expect(range.lowerValue() == 0.7 && range.upperValue() == 0.3, "a reversed pair is kept");
    range.resize(200, kit::px(kit::Size::Control));
    const auto span = range.spanRect();
    const auto track = range.trackRect();
    expect(std::abs(span.left() - (track.left() + track.width() * 0.3)) < 1e-6 &&
               std::abs(span.right() - (track.left() + track.width() * 0.7)) < 1e-6,
           "the span fills between the ends whichever is greater");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    kit::installKinetikTheme(application);
    testGradientEditorEditsStops();
    testAngleDialTurnsAndWinds();
    testPointPadDragsPoints();
    testRangeEndsMayCross();
    if (failures != 0)
        return 1;
    std::cout << "PASS: kit interactive controls\n";
    return 0;
}
