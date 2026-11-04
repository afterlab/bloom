// #501: every grading card at the default width -- Grade, Levels, Curves, Hue/Saturation,
// Exposure, Offset and Multiply (#504: and Colorize in each of its modes) -- paints only inside its card, keeps every widget inside it and
// cuts no label short (the #499 rules), shows its numbers with their units, and has no lighter
// sub-rectangle or dark band in its graphs. With BLOOM_GRAMMAR_ARTIFACT_DIR set it also writes each
// card, as the node editor draws it at a device pixel ratio of 2, to <dir>/<name>-card.png.
//
// It also holds the shared grading rules on the kit slider's grading variant, the Hue/Saturation
// strip, the Exposure slider and a group keyframe diamond: a press never jumps, a Shift-press
// jumps, Ctrl is a tenth of the speed and a modifier change starts a new segment, a double-click
// resets, and each gesture is one command.

#include "node_editor_items.hpp"

#include <QApplication>
#include <QDir>
#include <QGraphicsProxyWidget>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/colorize_widget.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grade_wheels.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/hue_saturation_widget.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/tab_bar.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/scalar_slider_widget.hpp>

#include <cmath>
#include <iostream>

using namespace bloom;

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
           const Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const Qt::MouseButtons held =
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton);
    QMouseEvent event(type, position, widget.mapToGlobal(position), Qt::LeftButton, held,
                      modifiers);
    QApplication::sendEvent(&widget, &event);
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

// #499: a card paints nothing outside its own rectangle.
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

// #499: every shown widget of a card sits inside it, no label is narrower than its text, no tab
// is clipped, and no value field drops its digits or its unit.
bool cardContentFits(ui::node_editor::NodeItem& item, const char* name) {
    const QRectF bounds = item.cardRect().adjusted(-0.5, -0.5, 0.5, 0.5);
    bool fits = true;
    for (auto* child : item.childItems()) {
        auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child);
        if (!child->isVisible() || proxy == nullptr || proxy->widget() == nullptr)
            continue;
        if (!bounds.contains(child->mapRectToParent(child->boundingRect()))) {
            std::cerr << name << ": a card widget lies outside the card\n";
            fits = false;
        }
        auto* root = proxy->widget();
        for (auto* widget : root->findChildren<QWidget*>()) {
            if (!widget->isVisibleTo(root))
                continue;
            const QRect inRoot(widget->mapTo(root, QPoint(0, 0)), widget->size());
            if (!root->rect().adjusted(0, 0, 1, 1).contains(inRoot)) {
                std::cerr << name << ": " << widget->objectName().toStdString()
                          << " lies outside its card widget\n";
                fits = false;
            }
        }
        for (auto* label : root->findChildren<QLabel*>()) {
            if (!label->isVisibleTo(root) || label->text().isEmpty())
                continue;
            if (label->text().contains(QChar(0x2026)) ||
                label->width() < label->fontMetrics().horizontalAdvance(label->text())) {
                std::cerr << name << ": label '" << label->text().toStdString()
                          << "' is cut short\n";
                fits = false;
            }
        }
        for (auto* tabs : root->findChildren<ui::kit::KTabBar*>())
            if (tabs->isVisibleTo(root) && tabs->tabRect(tabs->count() - 1).right() >= tabs->width()) {
                std::cerr << name << ": a tab bar is clipped\n";
                fits = false;
            }
        for (auto* field : root->findChildren<ui::kit::KValueField*>()) {
            if (!field->isVisibleTo(root))
                continue;
            const auto painted = field->paintedText();
            if (painted.contains(QChar(0x2026)) || field->isDisplayRounded() ||
                (!field->unit().isEmpty() && field->specialValueText().isEmpty() &&
                 !painted.endsWith(field->unit()))) {
                std::cerr << name << ": " << field->objectName().toStdString() << " shows '"
                          << painted.toStdString() << "', not its whole value and unit\n";
                fits = false;
            }
        }
    }
    return fits;
}

struct CardFixture final {
    explicit CardFixture(const std::string_view type)
        : project(document::makeNewProject("Cards", "Main", core::RationalTime::fromInteger(10))),
          document(std::move(project.project)), stack(document),
          session(document, stack, project.initialCompositionId) {
        node = addNode(session, type);
        editor = new ui::NodeGraphEditor(session, &window);
        editor->setGeometry(0, 0, 1400, 1400);
        window.resize(1400, 1400);
        window.show();
        QApplication::processEvents();
        item = ui::node_editor::nodeItemAncestor(editor->graphScene()->findNodeItem(node));
    }
    document::NewProject project;
    document::Document document;
    commands::CommandStack stack;
    ui::CompositionSession session;
    document::NodeId node;
    QWidget window;
    ui::NodeGraphEditor* editor = nullptr;
    ui::node_editor::NodeItem* item = nullptr;
};

void capture(ui::node_editor::NodeItem& item, ui::NodeGraphEditor& editor, const QString& name) {
    const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    const QRectF source = item.mapRectToScene(item.cardRect()).adjusted(-24, -24, 24, 24);
    constexpr qreal scale = 2.0;
    QImage image((source.size() * scale).toSize(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    image.fill(ui::kit::color(ui::kit::Color::Canvas));
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        editor.graphScene()->render(&painter, QRectF(QPointF(), source.size()), source);
    }
    if (!image.save(directory + QLatin1Char('/') + name + QStringLiteral("-card.png")))
        std::cerr << "could not write " << name.toStdString() << '\n';
}

// The cards at their default width.
void everyCard() {
    const std::array<std::pair<std::string_view, const char*>, 7> cards{{
        {document::kGradeNodeType, "grade"},
        {document::kLevelsNodeType, "levels"},
        {document::kCurvesNodeType, "curves"},
        {document::kHueSaturationNodeType, "hue-saturation"},
        {"bloom.exposure", "exposure"},
        {"bloom.offset", "offset"},
        {"bloom.multiply", "multiply"},
    }};
    for (const auto& [type, name] : cards) {
        CardFixture fixture(type);
        expect(fixture.item != nullptr && fixture.item->cardWidget() != nullptr,
               "every grading node draws its card widget");
        if (fixture.item == nullptr)
            continue;
        QApplication::processEvents();
        if (!paintsInsideCard(*fixture.item)) {
            std::cerr << name << " paints outside its card\n";
            ++failures;
        }
        if (!cardContentFits(*fixture.item, name))
            ++failures;
        capture(*fixture.item, *fixture.editor, QString::fromLatin1(name));
    }
}

// The kit slider's grading variant.
void sliderVariant() {
    QWidget host;
    auto& slider = *new ui::kit::KSlider(&host);
    slider.setGeometry(0, 0, 212, ui::kit::px(ui::kit::Size::Control));
    slider.setDragMode(ui::kit::KSlider::DragMode::Relative);
    slider.setRange(-4.0, 4.0);
    slider.setLimits(-32.0, 32.0);
    slider.setNeutral(0.0);
    slider.setDefaultValue(0.0);
    slider.setValue(1.0);
    host.show();
    QApplication::processEvents();
    const double perPixel = 8.0 / slider.trackRect().width();
    // A press on the bare track does not jump; the drag moves by the pointer's travel.
    const QPointF far(slider.trackRect().left() + 10.0, slider.height() / 2.0);
    mouse(slider, QEvent::MouseButtonPress, far);
    expect(slider.value() == 1.0, "a press on a grading slider's track does not jump");
    mouse(slider, QEvent::MouseMove, far + QPointF(20.0, 0.0));
    expect(std::abs(slider.value() - (1.0 + 20.0 * perPixel)) < 1e-9,
           "a grading slider moves by the pointer's travel");
    // Ctrl is a tenth of the speed, from where the value is: no jump.
    const double reached = slider.value();
    mouse(slider, QEvent::MouseMove, far + QPointF(20.0, 0.0), Qt::ControlModifier);
    expect(slider.value() == reached, "pressing Ctrl mid-drag does not jump");
    mouse(slider, QEvent::MouseMove, far + QPointF(40.0, 0.0), Qt::ControlModifier);
    expect(std::abs(slider.value() - (reached + 2.0 * perPixel)) < 1e-9,
           "Ctrl drags at a tenth of the speed");
    mouse(slider, QEvent::MouseButtonRelease, far + QPointF(40.0, 0.0));
    // A Shift-press jumps to the pointer, which the value then follows.
    const QPointF middle(slider.positionOf(2.0), slider.height() / 2.0);
    mouse(slider, QEvent::MouseButtonPress, middle, Qt::ShiftModifier);
    expect(std::abs(slider.value() - 2.0) < 1e-9, "a Shift-press jumps to the pointer");
    mouse(slider, QEvent::MouseMove, middle + QPointF(10.0, 0.0), Qt::ShiftModifier);
    expect(std::abs(slider.value() - (2.0 + 10.0 * perPixel)) < 1e-9,
           "after a Shift-press the value follows the pointer");
    mouse(slider, QEvent::MouseButtonRelease, middle + QPointF(10.0, 0.0));
    // The detent: a full-speed drag that comes near neutral rests on it.
    slider.setValue(0.3);
    const QPointF from(slider.positionOf(0.3), slider.height() / 2.0);
    const double towardZero = (slider.positionOf(0.03) - from.x());
    mouse(slider, QEvent::MouseButtonPress, from);
    mouse(slider, QEvent::MouseMove, from + QPointF(towardZero, 0.0));
    expect(slider.value() == 0.0, "a drag near neutral rests on it");
    mouse(slider, QEvent::MouseButtonRelease, from + QPointF(towardZero, 0.0));
    // A value past the track is drawn capped, not pinned.
    slider.setValue(6.0);
    expect(slider.value() == 6.0 && slider.overflows(),
           "a value past the track is kept and drawn as overflowing");
    // A double-click resets it as one edit.
    int finished = 0;
    QObject::connect(&slider, &ui::kit::KSlider::editFinished, [&] { ++finished; });
    mouse(slider, QEvent::MouseButtonDblClick, middle);
    expect(slider.value() == 0.0 && finished == 1, "a double-click resets the slider");
    // A jog runs past its track to its limits, a step per pixel.
    slider.setDragMode(ui::kit::KSlider::DragMode::Jog);
    slider.setJogStep(0.01);
    slider.setValue(3.9);
    mouse(slider, QEvent::MouseButtonPress, middle);
    mouse(slider, QEvent::MouseMove, middle + QPointF(50.0, 0.0));
    expect(std::abs(slider.value() - 4.4) < 1e-9, "a jog has no end stops");
    mouse(slider, QEvent::MouseMove, middle + QPointF(50.0, 0.0), Qt::ShiftModifier);
    mouse(slider, QEvent::MouseMove, middle + QPointF(60.0, 0.0), Qt::ShiftModifier);
    expect(std::abs(slider.value() - 5.4) < 1e-9, "Shift jogs at ten times the speed");
    mouse(slider, QEvent::MouseButtonRelease, middle + QPointF(60.0, 0.0));
}

// The Exposure card: one command per drag, a signed value with its unit, its socket beside it.
void exposureCard() {
    CardFixture fixture("bloom.exposure");
    auto* card = fixture.item ? qobject_cast<ui::ScalarSliderCardWidget*>(fixture.item->cardWidget())
                              : nullptr;
    expect(card != nullptr, "Exposure draws its slider card");
    if (card == nullptr)
        return;
    auto* row = card->row();
    expect(row->name() == QStringLiteral("Exposure") && row->field()->unit() == QStringLiteral("st") &&
               row->field()->paintedText() == QStringLiteral("+0.00 st"),
           "Exposure reads as a signed number of stops");
    expect(card->socketY("amount").has_value() && !card->showsRole("amount"),
           "the Exposure value's socket sits beside its own row");
    auto& slider = *row->slider();
    const auto commands = fixture.stack.size();
    const QPointF from(slider.positionOf(0.0) + 30.0, slider.height() / 2.0);
    mouse(slider, QEvent::MouseButtonPress, from);
    mouse(slider, QEvent::MouseMove, from + QPointF(slider.positionOf(1.0) - slider.positionOf(0.0), 0.0));
    mouse(slider, QEvent::MouseButtonRelease,
          from + QPointF(slider.positionOf(1.0) - slider.positionOf(0.0), 0.0));
    const auto exposure = stored(fixture.session, fixture.node, "amount");
    expect(fixture.stack.size() == commands + 1 && exposure && std::abs(*exposure - 1.0) < 1e-6,
           "an Exposure drag is one command");
    expect(row->reset() && stored(fixture.session, fixture.node, "amount") == 0.0,
           "Exposure resets to 0");
}

// The Hue/Saturation strip: the output spectrum follows the pointer, wrapping at 180 degrees.
void hueSaturationCard() {
    expect(std::abs(ui::hueStripDragValue(0.0, -50.0, 100.0, 1.0) - 180.0) < 1e-9 &&
               std::abs(ui::hueStripDragValue(175.0, -10.0, 360.0, 1.0) + 175.0) < 1e-9 &&
               std::abs(ui::hueStripDragValue(0.0, 10.0, 360.0, 0.1) + 1.0) < 1e-9,
           "a strip drag turns the hue by the pointer's travel and wraps at 180 degrees");
    CardFixture fixture(document::kHueSaturationNodeType);
    auto* card = fixture.item
                     ? qobject_cast<ui::HueSaturationCardWidget*>(fixture.item->cardWidget())
                     : nullptr;
    expect(card != nullptr, "Hue/Saturation draws its card widget");
    if (card == nullptr)
        return;
    expect(card->hueRow()->field()->paintedText() == QStringLiteral("+0.0 \u00b0") &&
               card->saturationRow()->field()->paintedText() == QStringLiteral("1.00 \u00d7"),
           "Hue reads in signed degrees and Saturation as a multiplier");
    auto& strip = *card->strip();
    const auto commands = fixture.stack.size();
    const auto band = strip.outputBand();
    const QPointF from = band.center();
    mouse(strip, QEvent::MouseButtonPress, from);
    mouse(strip, QEvent::MouseMove, from - QPointF(band.width() / 4.0, 0.0));
    mouse(strip, QEvent::MouseButtonRelease, from - QPointF(band.width() / 4.0, 0.0));
    const auto hue = stored(fixture.session, fixture.node, "hue");
    expect(fixture.stack.size() == commands + 1 && hue && std::abs(*hue - 90.0) < 1e-6,
           "a strip drag turns the hue as one command");
    mouse(strip, QEvent::MouseButtonDblClick, from);
    expect(stored(fixture.session, fixture.node, "hue") == 0.0, "a double-click resets the hue");
}

// #504: the Colorize card in every mode paints inside its card with no label cut short and shows
// only its mode's controls; the ramp's midtones stop drags the midpoint as one command, a
// Shift-press jumps it, a double-click resets it, and a click on a stop opens its colour picker.
void colorizeCard() {
    CardFixture fixture(document::kColorizeNodeType);
    auto* card = fixture.item ? qobject_cast<ui::ColorizeCardWidget*>(fixture.item->cardWidget())
                              : nullptr;
    expect(card != nullptr, "Colorize draws its card widget");
    const auto mode = ui::nodeRoleParameter(fixture.session, fixture.node, "mode");
    expect(mode.has_value(), "Colorize binds its mode");
    if (card == nullptr || !mode)
        return;
    expect(card->mode() == document::kColorizeTint, "a new Colorize is a Tint");
    const std::array<std::pair<std::int64_t, const char*>, 5> modes{
        {{document::kColorizeFill, "colorize-fill"},
         {document::kColorizeTint, "colorize-tint"},
         {document::kColorizeTritone, "colorize-tritone"},
         {document::kColorizeGradientMap, "colorize-gradient-map"},
         {document::kColorizeBlackAndWhite, "colorize-black-and-white"}}};
    for (const auto& [value, name] : modes) {
        expect(fixture.session.setParameterValue(*mode, value, QStringLiteral("Set Mode")),
               "the Colorize mode is set");
        QApplication::processEvents();
        expect(card->mode() == value, "the Colorize card follows its node's mode");
        if (!paintsInsideCard(*fixture.item)) {
            std::cerr << name << " paints outside its card\n";
            ++failures;
        }
        if (!cardContentFits(*fixture.item, name))
            ++failures;
        capture(*fixture.item, *fixture.editor, QString::fromLatin1(name));
        const bool ramp =
            value == document::kColorizeTritone || value == document::kColorizeGradientMap;
        expect(card->colorRow("fillColor")->isVisibleTo(card) == (value == document::kColorizeFill) &&
                   card->rampBar()->isVisibleTo(card) ==
                       (ramp || value == document::kColorizeTint) &&
                   card->colorRow("midtones")->isVisibleTo(card) == ramp &&
                   card->valueRow("reds")->isVisibleTo(card) ==
                       (value == document::kColorizeBlackAndWhite) &&
                   card->valueRow("amount")->isVisibleTo(card),
               "the Colorize card shows only its mode's controls");
        expect(card->socketY("amount").has_value() &&
                   card->socketY("magentas").has_value() ==
                       (value == document::kColorizeBlackAndWhite) &&
                   card->socketY("highlights").has_value() ==
                       (ramp || value == document::kColorizeTint) &&
                   card->showsRole("mode") &&
                   card->showsRole("source") == (value == document::kColorizeGradientMap) &&
                   card->showsRole("cycle") == (value == document::kColorizeGradientMap) &&
                   !card->showsRole("phase"),
               "each shown value keeps its socket beside its row; the map's choices are rows");
    }
    expect(fixture.session.setParameterValue(*mode, document::kColorizeTritone,
                                             QStringLiteral("Set Mode")),
           "the Colorize mode is Tritone");
    QApplication::processEvents();
    auto& bar = *card->rampBar();
    expect(bar.ramp() && bar.stops().size() == 3, "the Tritone ramp has three stops");
    // The gradient shows the node's own colours: black at the left, white at the right.
    expect(bar.colourAt(0.0).lightnessF() < 0.05F && bar.colourAt(1.0).lightnessF() > 0.95F,
           "the ramp bar draws the shadows and highlights colours at its ends");
    const auto commands = fixture.stack.size();
    const QPointF from = bar.stopCentre(1);
    const double travel = bar.barRect().width() * 0.2;
    mouse(bar, QEvent::MouseButtonPress, from);
    mouse(bar, QEvent::MouseMove, from + QPointF(travel, 0.0));
    mouse(bar, QEvent::MouseButtonRelease, from + QPointF(travel, 0.0));
    const auto dragged = stored(fixture.session, fixture.node, "midpoint");
    expect(fixture.stack.size() == commands + 1 && dragged && std::abs(*dragged - 0.38) < 1e-6,
           "dragging the midtones stop moves the midpoint as one command");
    const QPointF half(bar.barRect().left() + bar.barRect().width() * 0.5,
                       bar.barRect().center().y());
    mouse(bar, QEvent::MouseButtonPress, half, Qt::ShiftModifier);
    mouse(bar, QEvent::MouseButtonRelease, half);
    const auto jumped = stored(fixture.session, fixture.node, "midpoint");
    expect(fixture.stack.size() == commands + 2 && jumped && std::abs(*jumped - 0.5) < 1e-6,
           "a Shift-press on the ramp jumps the midpoint to the pointer");
    mouse(bar, QEvent::MouseButtonDblClick, half);
    expect(stored(fixture.session, fixture.node, "midpoint") == 0.18,
           "a double-click on the ramp resets the midpoint");
    const auto before = fixture.stack.size();
    mouse(bar, QEvent::MouseButtonPress, bar.stopCentre(0));
    mouse(bar, QEvent::MouseButtonRelease, bar.stopCentre(0));
    auto* chip = card->colorRow("shadows")->chip();
    expect(chip->isPickerOpen() && fixture.stack.size() == before,
           "a click on a stop opens its colour picker and changes nothing");
    chip->closePicker();
    expect(card->colorRow("shadows")->reset(), "a colour row resets to its default");
}

// A group diamond keys every value of the group as one command, and unkeys them as one.
void groupDiamond() {
    CardFixture fixture(document::kGradeNodeType);
    auto* card =
        fixture.item ? qobject_cast<ui::GradeCardWidget*>(fixture.item->cardWidget()) : nullptr;
    if (card == nullptr) {
        expect(false, "the Grade card draws its wheels");
        return;
    }
    auto* diamond = card->diamond(document::GradeWheel::Lift);
    expect(diamond->keyable() == 4 && diamond->keyed() == 0, "a wheel's diamond keys four values");
    const auto commands = fixture.stack.size();
    expect(diamond->toggle(), "the wheel's diamond keys it");
    diamond->refresh();
    expect(fixture.stack.size() == commands + 1 && diamond->keyed() == 4,
           "keying a wheel is one command for its four values");
    expect(diamond->toggle(), "the wheel's diamond unkeys it");
    diamond->refresh();
    expect(fixture.stack.size() == commands + 2 && diamond->keyed() == 0 &&
               stored(fixture.session, fixture.node, "liftRed") == 0.0,
           "removing the wheel's keys is one command and leaves constants");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    ui::kit::installKinetikTheme(application);
    everyCard();
    sliderVariant();
    exposureCard();
    hueSaturationCard();
    groupDiamond();
    colorizeCard();
    if (failures != 0)
        return 1;
    std::cout << "PASS: grading cards\n";
    return 0;
}
