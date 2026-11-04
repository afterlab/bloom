// #494: the Grade wheels and the graphical Levels, on the node card and in Properties. A puck
// drag edits a wheel's red, green and blue as one live edit and one command; a master slider and a
// Levels handle drag are one command each; a press that moves nothing commits nothing; Escape
// cancels; a double-click resets; Alt-click opens the values behind the widget; the card shows the
// widget instead of its value rows, and the Levels parameters are the numeric ones.

#include "node_editor_items.hpp"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QGraphicsProxyWidget>
#include <QLabel>
#include <QStyleOptionGraphicsItem>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grade_wheels.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/kit/tab_bar.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/levels_graph.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>

#include <cmath>
#include <iostream>

using namespace bloom;
using document::GradeWheel;
using ui::LevelsHandle;

namespace {
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

document::NodeId addNode(ui::CompositionSession& session, const std::string_view type) {
    commands::Transaction transaction("Add", session.snapshot().revision());
    transaction.emplace<commands::AddNode>(session.compositionId(), std::string(type),
                                           document::Vec2d{});
    const auto result = session.executeNodeTransaction(std::move(transaction));
    return result.outputId<document::NodeId>(commands::kAddNodeOutput).value_or(document::NodeId{});
}

void mouse(QWidget& widget, const QEvent::Type type, const QPointF position,
           const Qt::KeyboardModifiers modifiers = Qt::NoModifier,
           const Qt::MouseButton button = Qt::LeftButton) {
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : button;
    QMouseEvent event(type, position, widget.mapToGlobal(position), button,
                      type == QEvent::MouseMove ? Qt::MouseButtons(Qt::LeftButton) : held,
                      modifiers);
    QApplication::sendEvent(&widget, &event);
}

void key(QWidget& widget, const int which) {
    QKeyEvent event(QEvent::KeyPress, which, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

// #499: a card paints nothing outside its own rectangle (a hidden row's socket name included).
bool paintsInsideCard(ui::node_editor::NodeItem& item) {
    const QRectF card = item.cardRect();
    constexpr int margin = 48;
    QImage image(static_cast<int>(card.width()) + 2 * margin,
                 static_cast<int>(card.height()) + 2 * margin, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.translate(margin, margin);
        QStyleOptionGraphicsItem option;
        option.rect = card.toRect();
        option.exposedRect = card.adjusted(-margin, -margin, margin, margin);
        item.paint(&painter, &option, nullptr);
    }
    const QRectF inside = card.translated(margin, margin).adjusted(-1.0, -1.0, 1.0, 1.0);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (!inside.contains(QPointF(x + 0.5, y + 0.5)) && qAlpha(image.pixel(x, y)) != 0)
                return false;
    return true;
}

// #499: every shown widget of a card (its card widget and its rows) sits inside the card, and no
// text the card shows is elided: no label is narrower than its text, and no tab is clipped. The
// sockets sit on the card's edges by design.
bool cardContentFits(ui::node_editor::NodeItem& item) {
    const QRectF bounds = item.cardRect().adjusted(-0.5, -0.5, 0.5, 0.5);
    bool fits = true;
    for (auto* child : item.childItems()) {
        auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child);
        if (!child->isVisible() || proxy == nullptr || proxy->widget() == nullptr)
            continue;
        if (!bounds.contains(child->mapRectToParent(child->boundingRect()))) {
            std::cerr << "a card widget lies outside the card\n";
            fits = false;
        }
        for (auto* label : proxy->widget()->findChildren<QLabel*>()) {
            if (!label->isVisible() || label->text().isEmpty())
                continue;
            if (label->text().contains(QChar(0x2026)) ||
                label->width() < label->fontMetrics().horizontalAdvance(label->text())) {
                std::cerr << "label '" << label->text().toStdString() << "' is cut short\n";
                fits = false;
            }
        }
        for (auto* tabs : proxy->widget()->findChildren<ui::kit::KTabBar*>())
            if (tabs->isVisible() && tabs->tabRect(tabs->count() - 1).right() >= tabs->width()) {
                std::cerr << "a tab bar is clipped\n";
                fits = false;
            }
    }
    return fits;
}

std::optional<double> stored(ui::CompositionSession& session, const document::NodeId node,
                             const std::string& role) {
    const auto* composition = session.composition();
    const auto* record = composition->graph().findNode(node);
    for (const auto& binding : record->parameters)
        if (binding.role == role) {
            const auto* parameter = composition->parameters().find(binding.parameterId);
            const auto* constant = std::get_if<document::ConstantValueSource>(&parameter->source);
            const auto* value = constant ? std::get_if<double>(&constant->value) : nullptr;
            return value ? std::optional(*value) : std::nullopt;
        }
    return std::nullopt;
}

void wheelMaths() {
    // Red sits at the vectorscope's 103 degrees, and the puck and the colour invert each other.
    const auto red = ui::gradeWheelPuck(GradeWheel::Lift, {0.1, -0.05, -0.05});
    const double angle = std::atan2(red.y(), red.x()) * 180.0 / 3.14159265358979323846;
    expect(std::abs(angle - 103.0) < 1e-9, "a red lift points at the vectorscope's red");
    const std::array<double, 3> colour{1.2, 0.95, 1.05};
    const auto puck = ui::gradeWheelPuck(GradeWheel::Gain, colour);
    const auto back = ui::gradeWheelColour(GradeWheel::Gain, puck, colour);
    expect(std::abs(back[0] - colour[0]) < 1e-12 && std::abs(back[1] - colour[1]) < 1e-12 &&
               std::abs(back[2] - colour[2]) < 1e-12,
           "a gain colour round-trips through its puck");
    const auto grey = ui::gradeWheelColour(GradeWheel::Gain, {}, colour);
    expect(std::abs(grey[0] - grey[1]) < 1e-12 && std::abs(grey[1] - grey[2]) < 1e-12 &&
               std::abs((grey[0] + grey[1] + grey[2]) - (colour[0] + colour[1] + colour[2])) <
                   1e-12,
           "the centre keeps the colour's grey part");
    const auto rim = ui::gradeWheelPuck(GradeWheel::Offset,
                                        ui::gradeWheelColour(GradeWheel::Offset, {0.0, 1.0}, {}));
    expect(std::abs(std::hypot(rim.x(), rim.y()) - 1.0) < 1e-12, "the rim is the wheel's range");
}

void gradeWheels() {
    auto project = document::makeNewProject("Grade", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    const auto node = addNode(session, document::kGradeNodeType);
    expect(node.isValid(), "Grade is added");
    QWidget window;
    auto* properties = new ui::PropertiesEditor(session, &window);
    auto* editor = new ui::NodeGraphEditor(session, &window);
    properties->setGeometry(0, 0, 420, 900);
    editor->setGeometry(420, 0, 1000, 900);
    window.resize(1420, 900);
    window.show();
    session.selectNode(node);
    QApplication::processEvents();

    auto* item = ui::node_editor::nodeItemAncestor(editor->graphScene()->findNodeItem(node));
    auto* card = item ? qobject_cast<ui::GradeCardWidget*>(item->cardWidget()) : nullptr;
    expect(card != nullptr, "the Grade card draws its wheels");
    auto* panel = properties->findChild<ui::GradeCardWidget*>("propertiesGradeWheels");
    expect(panel != nullptr, "Properties draws the same wheels");
    if (!card || !panel)
        return;
    QApplication::processEvents();
    expect(card->canvas(GradeWheel::Lift)->width() >= 100,
           "a card wheel is large enough to read and drag");
    // A card row is a proxied property row carrying its parameter's role.
    const auto rowShown = [&](const char* role) {
        bool shown = false;
        for (auto* child : item->childItems())
            if (auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child);
                proxy && proxy->isVisible() && proxy->widget() &&
                proxy->widget()->property("nodeParameterRole").toString() ==
                    QString::fromLatin1(role))
                shown = true;
        return shown;
    };
    expect(!rowShown("liftRed") && !rowShown("temperature"),
           "the card shows the widget, not the value rows");
    expect(paintsInsideCard(*item), "the Grade card paints no hidden row's label outside it");
    expect(cardContentFits(*item), "the Grade card's content fits it, nothing elided");
    // #499: at the default card width every primary's name and value is whole.
    for (const auto which : document::kGradePrimaries) {
        const auto role = QString::fromStdString(std::string(document::gradePrimaryRole(which)));
        auto* label =
            card->findChild<ui::kit::KLabel*>(QStringLiteral("gradePrimaryLabel.%1").arg(role));
        auto* field = card->primary(which);
        const bool whole =
            label && field &&
            label->width() >= QFontMetrics(label->font()).horizontalAdvance(label->text()) &&
            !field->paintedText().contains(QChar(0x2026)) && !field->isDisplayRounded() &&
            field->width() >= field->sizeHint().width() &&
            field->geometry().right() <= card->width() && label->geometry().right() < field->x();
        if (!whole)
            std::cerr << "primary " << role.toStdString() << " is cut short\n";
        expect(whole, "a primary's name and value fit the card");
    }
    const auto collapsedHeight = item->boundingRect().height();

    // A puck drag: live, then one command for the three colour values.
    auto& wheel = *card->canvas(GradeWheel::Gain);
    const auto centre = wheel.puckPosition();
    const auto commands = stack.size();
    mouse(wheel, QEvent::MouseButtonPress, centre);
    mouse(wheel, QEvent::MouseMove, centre + QPointF(20.0, -10.0));
    const auto live = wheel.values();
    expect(live && ((*live)[1] != 1.0 || (*live)[2] != 1.0 || (*live)[3] != 1.0) &&
               stack.size() == commands && stored(session, node, "gainRed") == 1.0,
           "a puck drag is live and commits nothing yet");
    expect(panel->canvas(GradeWheel::Gain)->values() == live,
           "Properties follows the card's drag before it commits");
    mouse(wheel, QEvent::MouseButtonRelease, centre + QPointF(20.0, -10.0));
    expect(stack.size() == commands + 1, "a puck drag is one command");
    const auto red = stored(session, node, "gainRed");
    const auto green = stored(session, node, "gainGreen");
    const auto blue = stored(session, node, "gainBlue");
    expect(red && green && blue && (*red != 1.0 || *green != 1.0 || *blue != 1.0) &&
               std::abs((*red + *green + *blue) - 3.0) < 1e-9,
           "the drag shifts the colour and keeps its grey part");
    expect(session.undo() && stored(session, node, "gainRed") == 1.0 &&
               stored(session, node, "gainBlue") == 1.0 && session.redo() &&
               stored(session, node, "gainRed") == red,
           "the gesture undoes and redoes as one step");

    // A press that moves nothing commits nothing; Escape cancels.
    mouse(wheel, QEvent::MouseButtonPress, centre);
    mouse(wheel, QEvent::MouseButtonRelease, centre);
    expect(stack.size() == commands + 1, "a press that does not move commits nothing");
    mouse(wheel, QEvent::MouseButtonPress, centre);
    mouse(wheel, QEvent::MouseMove, centre + QPointF(-30.0, 0.0));
    key(wheel, Qt::Key_Escape);
    mouse(wheel, QEvent::MouseButtonRelease, centre + QPointF(-30.0, 0.0));
    expect(!wheel.dragging() && stack.size() == commands + 1 &&
               stored(session, node, "gainRed") == red,
           "Escape cancels the drag and commits nothing");

    // The master slider is one command per drag.
    auto& master = *card->master(GradeWheel::Lift);
    const auto handle = master.handleRect().center();
    mouse(master, QEvent::MouseButtonPress, handle);
    mouse(master, QEvent::MouseMove, handle + QPointF(15.0, 0.0));
    mouse(master, QEvent::MouseMove, handle + QPointF(25.0, 0.0));
    mouse(master, QEvent::MouseButtonRelease, handle + QPointF(25.0, 0.0));
    expect(stack.size() == commands + 2 && stored(session, node, "liftMaster") > 0.0,
           "a master slider drag is one command");

    // A double-click resets the wheel, master and colour, as one command.
    mouse(wheel, QEvent::MouseButtonDblClick, centre);
    expect(stack.size() == commands + 3 && stored(session, node, "gainRed") == 1.0 &&
               stored(session, node, "gainGreen") == 1.0 &&
               stored(session, node, "gainBlue") == 1.0,
           "a double-click resets the wheel as one command");

    // Alt-click opens the wheel's values on the card.
    mouse(*card->canvas(GradeWheel::Lift), QEvent::MouseButtonPress,
          card->canvas(GradeWheel::Lift)->puckPosition(), Qt::AltModifier);
    mouse(*card->canvas(GradeWheel::Lift), QEvent::MouseButtonRelease,
          card->canvas(GradeWheel::Lift)->puckPosition(), Qt::AltModifier);
    QApplication::processEvents();
    expect(card->valuesExpanded(GradeWheel::Lift) && card->showsRole("liftRed") &&
               !card->showsRole("gainRed") && rowShown("liftMaster") && rowShown("liftBlue") &&
               !rowShown("gainRed") && item->boundingRect().height() > collapsedHeight &&
               stack.size() == commands + 3,
           "Alt-click opens the wheel's value rows and commits nothing");
    panel->setPrimariesExpanded(true);
    expect(panel->showsRole("exposure") && !panel->showsRole("liftRed"),
           "Properties opens the primaries' rows on its own");

    // A primaries field edits its parameter as one command.
    auto* exposure = card->primary(document::GradePrimary::Exposure);
    exposure->setValue(1.5);
    QApplication::processEvents();
    expect(stack.size() == commands + 4 && stored(session, node, "exposure") == 1.5,
           "a primaries field writes its value");
    expect(panel->primary(document::GradePrimary::Exposure)->value() == 1.5,
           "the Properties primaries follow the card");
}

void levelsGraph() {
    auto project = document::makeNewProject("Levels", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    const auto node = addNode(session, document::kLevelsNodeType);
    QWidget window;
    auto* editor = new ui::NodeGraphEditor(session, &window);
    editor->setGeometry(0, 0, 1000, 900);
    window.resize(1000, 900);
    window.show();
    QApplication::processEvents();
    auto* item = ui::node_editor::nodeItemAncestor(editor->graphScene()->findNodeItem(node));
    auto* card = item ? qobject_cast<ui::LevelsCardWidget*>(item->cardWidget()) : nullptr;
    expect(card != nullptr, "the Levels card draws its graph");
    if (!card)
        return;
    expect(card->showsRole("clamp") && !card->showsRole("masterGamma"),
           "the card keeps the clamp row and hides the values");
    expect(paintsInsideCard(*item), "the Levels card paints no hidden row's label outside it");
    expect(cardContentFits(*item), "the Levels card's content fits it, nothing elided");
    // #499: the five channel tabs fit beside the Values caret.
    auto* tabs = card->tabs();
    expect(tabs->count() == 5 && tabs->width() >= tabs->sizeHint().width() &&
               tabs->tabRect(4).right() < tabs->width() &&
               tabs->geometry().right() < card->width() - ui::kit::px(ui::kit::Size::Control) + 1,
           "every channel tab fits on the card");
    auto& canvas = *card->canvas();
    expect(canvas.handlePosition(LevelsHandle::InputBlack) &&
               canvas.handlePosition(LevelsHandle::Gamma) &&
               canvas.handlePosition(LevelsHandle::OutputWhite),
           "the graph draws every handle");
    if (!canvas.handlePosition(LevelsHandle::InputWhite))
        return;
    const auto commands = stack.size();
    // Input white moves left: one command.
    const auto white = *canvas.handlePosition(LevelsHandle::InputWhite);
    const auto plot = canvas.plotRect();
    mouse(canvas, QEvent::MouseButtonPress, white);
    mouse(canvas, QEvent::MouseMove, QPointF(plot.left() + plot.width() * 0.8, white.y()));
    expect(stack.size() == commands && stored(session, node, "masterInputWhite") == 1.0 &&
               canvas.values() && std::abs((*canvas.values())[1] - 0.8) < 0.02,
           "a handle drag is live and commits nothing yet");
    mouse(canvas, QEvent::MouseButtonRelease, QPointF(plot.left() + plot.width() * 0.8, white.y()));
    const auto inputWhite = stored(session, node, "masterInputWhite");
    expect(stack.size() == commands + 1 && inputWhite && std::abs(*inputWhite - 0.8) < 0.02,
           "an input white drag is one command");
    // The gamma handle dragged left raises gamma (brightens the middle).
    const auto gamma = *canvas.handlePosition(LevelsHandle::Gamma);
    mouse(canvas, QEvent::MouseButtonPress, gamma);
    mouse(canvas, QEvent::MouseMove, gamma - QPointF(plot.width() * 0.15, 0.0));
    mouse(canvas, QEvent::MouseButtonRelease, gamma - QPointF(plot.width() * 0.15, 0.0));
    const auto raised = stored(session, node, "masterGamma");
    expect(stack.size() == commands + 2 && raised && *raised > 1.0,
           "dragging the gamma handle left raises gamma, as one command");
    // Output black.
    const auto outputBlack = *canvas.handlePosition(LevelsHandle::OutputBlack);
    mouse(canvas, QEvent::MouseButtonPress, outputBlack);
    mouse(canvas, QEvent::MouseMove, outputBlack + QPointF(plot.width() * 0.1, 0.0));
    key(canvas, Qt::Key_Escape);
    mouse(canvas, QEvent::MouseButtonRelease, outputBlack + QPointF(plot.width() * 0.1, 0.0));
    expect(stack.size() == commands + 2 && stored(session, node, "masterOutputBlack") == 0.0,
           "Escape cancels an output handle drag");
    mouse(canvas, QEvent::MouseButtonPress, outputBlack);
    mouse(canvas, QEvent::MouseMove, outputBlack + QPointF(plot.width() * 0.1, 0.0));
    mouse(canvas, QEvent::MouseButtonRelease, outputBlack + QPointF(plot.width() * 0.1, 0.0));
    expect(stack.size() == commands + 3 && stored(session, node, "masterOutputBlack") > 0.05,
           "an output black drag is one command");
    // A double-click resets a handle.
    mouse(canvas, QEvent::MouseButtonDblClick, *canvas.handlePosition(LevelsHandle::Gamma));
    expect(stack.size() == commands + 4 && stored(session, node, "masterGamma") == 1.0,
           "a double-click resets the handle as one command");
    // Each tab edits its own channel.
    card->tabs()->setCurrentIndex(1);
    expect(canvas.channel() == 1, "the R tab shows the red channel");
    const auto redWhite = *canvas.handlePosition(LevelsHandle::InputWhite);
    mouse(canvas, QEvent::MouseButtonPress, redWhite);
    mouse(canvas, QEvent::MouseMove, QPointF(plot.left() + plot.width() * 0.5, redWhite.y()));
    mouse(canvas, QEvent::MouseButtonRelease,
          QPointF(plot.left() + plot.width() * 0.5, redWhite.y()));
    expect(stack.size() == commands + 5 && stored(session, node, "redInputWhite") < 0.6 &&
               stored(session, node, "masterInputWhite") == inputWhite,
           "a red handle edits the red channel only");
    // Alt-click opens the channel's values.
    mouse(canvas, QEvent::MouseButtonPress, plot.center(), Qt::AltModifier);
    mouse(canvas, QEvent::MouseButtonRelease, plot.center(), Qt::AltModifier);
    expect(card->valuesExpanded() && card->showsRole("redGamma") && !card->showsRole("masterGamma"),
           "Alt-click opens the channel's values");
    expect(std::abs(ui::levelsGammaAt(0.0, 1.0, ui::levelsGammaHandle(0.0, 1.0, 2.0)) - 2.0) < 1e-9,
           "the gamma handle and its gamma invert each other");
    QImage image(card->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    card->render(&image);
    bool drawn = false;
    for (int y = 0; y < image.height() && !drawn; y += 4)
        for (int x = 0; x < image.width() && !drawn; x += 4)
            drawn = qAlpha(image.pixel(x, y)) != 0;
    expect(drawn, "the Levels graph paints");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    ui::kit::installKinetikTheme(application);
    wheelMaths();
    gradeWheels();
    levelsGraph();
    if (failures != 0)
        return 1;
    std::cout << "PASS: grade wheels and levels graph\n";
    return 0;
}
