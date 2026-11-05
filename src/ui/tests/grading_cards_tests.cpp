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
#include <QElapsedTimer>
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
#include <bloom/document/parameter.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/ui/colorize_widget.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/grade_wheels.hpp>
#include <bloom/ui/gradient_parameter_row.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/hue_saturation_widget.hpp>
#include <bloom/ui/kit/angle_dial.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/gradient_editor.hpp>
#include <bloom/ui/kit/point_pad.hpp>
#include <bloom/ui/kit/range_selector.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/tab_bar.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/node_controls_widget.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/scalar_slider_widget.hpp>

#include <cmath>
#include <iostream>
#include <string_view>
#include <tuple>

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
// is clipped, and no value field -- the card's own widgets and its shared compact rows alike --
// drops its digits or its unit.
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

void capture(CardFixture& fixture, const QString& name) {
    const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
    if (directory.isEmpty() || fixture.item == nullptr)
        return;
    QDir().mkpath(directory);
    // The colour chips and gradient bars draw in display colour once the converter lands.
    QElapsedTimer converter;
    converter.start();
    while (!fixture.session.colorConverterReady() && converter.elapsed() < 5'000)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    QApplication::processEvents();
    auto& item = *fixture.item;
    auto& editor = *fixture.editor;
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
        capture(fixture, QString::fromLatin1(name));
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
        capture(fixture, QString::fromLatin1(name));
        const bool ramp = value == document::kColorizeTritone;
        const bool map = value == document::kColorizeGradientMap;
        expect(card->colorRow("fillColor")->isVisibleTo(card) == (value == document::kColorizeFill) &&
                   card->rampBar()->isVisibleTo(card) ==
                       (ramp || value == document::kColorizeTint) &&
                   card->gradientRow()->isVisibleTo(card) == map &&
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
                   !card->showsRole("phase") && !card->showsRole("gradient"),
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

std::optional<document::GradientValue> storedGradient(ui::CompositionSession& session,
                                                      const document::NodeId node,
                                                      const std::string_view role = "gradient") {
    const auto id = ui::nodeRoleParameter(session, node, role);
    const auto value = id ? session.liveValue(*id) : std::nullopt;
    const auto* gradient = value ? std::get_if<document::GradientValue>(&*value) : nullptr;
    return gradient ? std::optional(*gradient) : std::nullopt;
}

// #541: the Gradient Map's multi-stop gradient: a stop drags along the bar as one command, a press
// on the bar adds one, a stop pulled off the bar goes, the selected stop's position field moves it,
// and the gradient keys as one whole value whose keys interpolate.
void colorizeGradientMap() {
    CardFixture fixture(document::kColorizeNodeType);
    auto* card = fixture.item ? qobject_cast<ui::ColorizeCardWidget*>(fixture.item->cardWidget())
                              : nullptr;
    const auto mode = ui::nodeRoleParameter(fixture.session, fixture.node, "mode");
    if (card == nullptr || !mode) {
        expect(false, "Colorize draws its card widget");
        return;
    }
    expect(fixture.session.setParameterValue(*mode, document::kColorizeGradientMap,
                                             QStringLiteral("Set Mode")),
           "the Colorize mode is Gradient Map");
    QApplication::processEvents();
    auto* row = card->gradientRow();
    auto& editor = *row->editor();
    const auto initial = storedGradient(fixture.session, fixture.node);
    expect(initial && *initial == document::defaultColorizeGradient() &&
               editor.stops().size() == initial->stops.size(),
           "a new Colorize binds the default gradient and the editor shows its stops");
    const auto bar = editor.barRect();
    auto commands = fixture.stack.size();
    const QPointF from = editor.stopCentre(1);
    mouse(editor, QEvent::MouseButtonPress, from);
    mouse(editor, QEvent::MouseMove, from + QPointF(bar.width() * 0.2, 0.0));
    mouse(editor, QEvent::MouseButtonRelease, from + QPointF(bar.width() * 0.2, 0.0));
    auto gradient = storedGradient(fixture.session, fixture.node);
    expect(fixture.stack.size() == commands + 1 && gradient &&
               std::abs(gradient->stops[1].position - 0.38) < 1e-6,
           "dragging a stop moves it as one command");
    commands = fixture.stack.size();
    const QPointF at(bar.left() + bar.width() * 0.7, bar.center().y());
    mouse(editor, QEvent::MouseButtonPress, at);
    mouse(editor, QEvent::MouseButtonRelease, at);
    gradient = storedGradient(fixture.session, fixture.node);
    expect(fixture.stack.size() == commands + 1 && gradient && gradient->stops.size() == 4 &&
               std::abs(gradient->stops.back().position - 0.7) < 1e-6,
           "a press on the bar adds a stop as one command, after the stored stops so each "
           "stop keeps its index");
    QApplication::processEvents();
    commands = fixture.stack.size();
    const QPointF added = editor.stopCentre(2);
    const QPointF away = added + QPointF(0.0, ui::kit::px(ui::kit::Size::DropdownWidth));
    mouse(editor, QEvent::MouseButtonPress, added);
    mouse(editor, QEvent::MouseMove, away);
    mouse(editor, QEvent::MouseButtonRelease, away);
    gradient = storedGradient(fixture.session, fixture.node);
    expect(fixture.stack.size() == commands + 1 && gradient && gradient->stops.size() == 3,
           "a stop pulled off the bar is removed as one command");
    QApplication::processEvents();
    // The selected stop's position field moves it.
    editor.setSelectedStop(1);
    QApplication::processEvents();
    commands = fixture.stack.size();
    auto* position = row->stopPosition();
    expect(position->isEnabled() && std::abs(position->value() - 38.0) < 1e-6,
           "the position field shows the selected stop as a percentage");
    position->setValue(60.0);
    QApplication::processEvents();
    gradient = storedGradient(fixture.session, fixture.node);
    expect(gradient && std::abs(gradient->stops[1].position - 0.6) < 1e-6 &&
               fixture.stack.size() == commands + 1,
           "typing the stop's position moves it as one command");
    expect(row->stopChip()->isEnabled(), "the selected stop's colour chip is editable");
    // Keyed, the gradient is one whole-value key; a drag at another time keys there too.
    const auto id = ui::nodeRoleParameter(fixture.session, fixture.node, "gradient");
    expect(id && fixture.session.toggleKeyframeForParameter(*id), "the gradient keys");
    const auto keyed = storedGradient(fixture.session, fixture.node);
    expect(fixture.session.setCurrentTime(core::RationalTime::fromInteger(2)),
           "the playhead moves");
    QApplication::processEvents();
    const QPointF last = editor.stopCentre(2);
    mouse(editor, QEvent::MouseButtonPress, last);
    mouse(editor, QEvent::MouseMove, last - QPointF(bar.width() * 0.2, 0.0));
    mouse(editor, QEvent::MouseButtonRelease, last - QPointF(bar.width() * 0.2, 0.0));
    const auto later = storedGradient(fixture.session, fixture.node);
    expect(later && std::abs(later->stops[2].position - 0.8) < 1e-6,
           "a drag on an animated gradient keys the playhead");
    expect(fixture.session.setCurrentTime(core::RationalTime::fromInteger(1)),
           "the playhead moves between the keys");
    const auto between = storedGradient(fixture.session, fixture.node);
    expect(between && keyed && std::abs(between->stops[2].position - 0.9) < 1e-6,
           "the gradient's keys interpolate stop by stop");
    expect(fixture.session.setCurrentTime(core::RationalTime::fromInteger(0)),
           "the playhead returns");
    expect(storedGradient(fixture.session, fixture.node) == keyed,
           "the first key keeps the gradient it was set with");
    capture(fixture, QStringLiteral("colorize-gradient-map-stops"));
}

// #541: an angle turns on a dial, a range drags both its ends on one track and a gradient's start
// and end drag over a picture of its frame -- each one command, the numbers beside them.
void nodeControlCards() {
    const std::array<std::pair<std::string_view, const char*>, 16> cards{{
        {"bloom.drop-shadow", "drop-shadow"},
        {"bloom.glow", "glow"},
        {"bloom.transform-2d", "transform-2d"},
        {"bloom.layer-output", "layer-output"},
        {"bloom.linear-gradient", "linear-gradient"},
        {"bloom.radial-gradient", "radial-gradient"},
        {"bloom.luma-key", "luma-key"},
        {"bloom.map-range", "map-range"},
        {"bloom.clamp", "clamp"},
        {"bloom.in-range", "in-range"},
        {"bloom.random", "random"},
        {"bloom.wrap", "wrap"},
        {"bloom.smoothstep", "smoothstep"},
        {"bloom.hue-shift", "hue-shift"},
        {"bloom.rotate-2d", "rotate-2d"},
        {"bloom.radians-to-degrees", "radians-to-degrees"},
    }};
    for (const auto& [type, name] : cards) {
        CardFixture fixture(type);
        auto* card = fixture.item ? qobject_cast<ui::NodeControlsCardWidget*>(
                                        fixture.item->cardWidget())
                                  : nullptr;
        if (card == nullptr) {
            std::cerr << name << " draws no controls widget\n";
            ++failures;
            continue;
        }
        QApplication::processEvents();
        if (!paintsInsideCard(*fixture.item)) {
            std::cerr << name << " paints outside its card\n";
            ++failures;
        }
        if (!cardContentFits(*fixture.item, name))
            ++failures;
        capture(fixture, QString::fromLatin1(name));
    }

    {
        CardFixture fixture("bloom.drop-shadow");
        auto* card = fixture.item ? qobject_cast<ui::NodeControlsCardWidget*>(
                                        fixture.item->cardWidget())
                                  : nullptr;
        if (card == nullptr || card->angleRows().empty())
            return expect(false, "Drop Shadow draws its direction dial");
        expect(!card->showsRole("direction") && card->socketY("direction").has_value() &&
                   card->showsRole("distance"),
               "the dial's row keeps the direction's socket; other values keep their rows");
        auto& dial = *card->angleRows().front()->dial();
        const auto before = stored(fixture.session, fixture.node, "direction");
        const auto commands = fixture.stack.size();
        const auto centre = dial.discRect().center();
        const double reach = dial.discRect().width() * 0.4;
        // From pointing up to pointing right is a quarter turn clockwise.
        mouse(dial, QEvent::MouseButtonPress, centre + QPointF(0.0, -reach));
        mouse(dial, QEvent::MouseMove, centre + QPointF(reach, 0.0));
        mouse(dial, QEvent::MouseButtonRelease, centre + QPointF(reach, 0.0));
        const auto after = stored(fixture.session, fixture.node, "direction");
        expect(before && after && std::abs(*after - (*before + 90.0)) < 1e-6 &&
                   fixture.stack.size() == commands + 1,
               "turning the dial a quarter turn adds 90 degrees as one command");
        QApplication::processEvents();
        expect(std::abs(card->angleRows().front()->valueRow()->field()->value() - *after) < 1e-6,
               "the direction's number follows the dial");
    }
    {
        CardFixture fixture("bloom.luma-key");
        auto* card = fixture.item ? qobject_cast<ui::NodeControlsCardWidget*>(
                                        fixture.item->cardWidget())
                                  : nullptr;
        if (card == nullptr || card->rangeRows().empty())
            return expect(false, "Luma Key draws its key range");
        expect(card->rangeRows().front()->lowRow()->name() !=
                   card->rangeRows().front()->highRow()->name(),
               "the key range's two rows carry their own labels");
        auto& range = *card->rangeRows().front()->selector();
        const auto low = stored(fixture.session, fixture.node, "low");
        const auto high = stored(fixture.session, fixture.node, "high");
        const auto commands = fixture.stack.size();
        const auto handle = range.handleRect(ui::kit::KRangeSelector::Handle::Lower).center();
        const double quarter = range.trackRect().width() * 0.25;
        mouse(range, QEvent::MouseButtonPress, handle);
        mouse(range, QEvent::MouseMove, handle + QPointF(quarter, 0.0));
        mouse(range, QEvent::MouseButtonRelease, handle + QPointF(quarter, 0.0));
        const auto movedLow = stored(fixture.session, fixture.node, "low");
        expect(low && high && movedLow &&
                   std::abs(*movedLow - (*low + 0.25)) < 0.02 &&
                   stored(fixture.session, fixture.node, "high") == high &&
                   fixture.stack.size() == commands + 1,
               "dragging the low end moves the low value only, as one command");
        expect(fixture.session.undo() && stored(fixture.session, fixture.node, "low") == low,
               "one undo restores the range");
    }
    {
        CardFixture fixture("bloom.linear-gradient");
        auto* card = fixture.item ? qobject_cast<ui::NodeControlsCardWidget*>(
                                        fixture.item->cardWidget())
                                  : nullptr;
        if (card == nullptr || card->padRow() == nullptr)
            return expect(false, "Linear Gradient draws its start and end pad");
        expect(card->showsRole("start") && card->showsRole("end"),
               "the start and end keep their exact X/Y rows beside the pad");
        auto& pad = *card->padRow()->pad();
        QApplication::processEvents();
        const auto commands = fixture.stack.size();
        const auto before = pad.value(1);
        const auto end = pad.screenPoint(before);
        const double units = pad.frame().width() / pad.frameRect().width();
        mouse(pad, QEvent::MouseButtonPress, end);
        mouse(pad, QEvent::MouseMove, end + QPointF(0.0, 20.0));
        mouse(pad, QEvent::MouseButtonRelease, end + QPointF(0.0, 20.0));
        const auto id = ui::nodeRoleParameter(fixture.session, fixture.node, "end");
        const auto value = id ? fixture.session.liveValue(*id) : std::nullopt;
        const auto* moved = value ? std::get_if<document::Vec2d>(&*value) : nullptr;
        expect(moved && std::abs(moved->x - before.x()) < 1e-6 &&
                   std::abs(moved->y - (before.y() + 20.0 * units)) < 1e-6 &&
                   fixture.stack.size() == commands + 1,
               "dragging the end point moves the gradient's end as one command");
    }
    // The generators' colours and Glow's tint gradient: a gradient row on the card, its value
    // kept off the generic rows, a press on the bar adding a stop as one command.
    for (const auto& [type, role, initial] :
         {std::tuple{std::string_view("bloom.linear-gradient"), std::string_view("gradient"),
                     document::defaultGeneratorGradient()},
          std::tuple{std::string_view("bloom.radial-gradient"), std::string_view("gradient"),
                     document::defaultGeneratorGradient()},
          std::tuple{std::string_view("bloom.glow"), std::string_view("tintGradient"),
                     document::defaultGlowTintGradient()}}) {
        CardFixture fixture(type);
        auto* card = fixture.item ? qobject_cast<ui::NodeControlsCardWidget*>(
                                        fixture.item->cardWidget())
                                  : nullptr;
        if (card == nullptr || card->gradientRows().size() != 1) {
            expect(false, "the card draws one gradient row");
            continue;
        }
        auto* row = card->gradientRows().front();
        QApplication::processEvents();
        expect(row->role() == role && !card->showsRole(role) && row->isVisible(),
               "the gradient row draws the gradient in place of a generic row");
        auto& editor = *row->editor();
        expect(storedGradient(fixture.session, fixture.node, role) == initial &&
                   editor.stops().size() == initial.stops.size(),
               "a new node binds its default gradient and the editor shows its stops");
        const auto bar = editor.barRect();
        const auto commands = fixture.stack.size();
        const QPointF at(bar.left() + bar.width() * 0.5, bar.center().y());
        mouse(editor, QEvent::MouseButtonPress, at);
        mouse(editor, QEvent::MouseButtonRelease, at);
        const auto gradient = storedGradient(fixture.session, fixture.node, role);
        expect(fixture.stack.size() == commands + 1 && gradient && gradient->stops.size() == 3 &&
                   std::abs(gradient->stops.back().position - 0.5) < 1e-6,
               "a press on the gradient bar adds a stop as one command");
        expect(fixture.session.undo() &&
                   storedGradient(fixture.session, fixture.node, role) == initial,
               "one undo restores the gradient");
        QApplication::processEvents();
        expect(row->reset(), "the gradient row resets to the node's default");
    }
}

// #543: a Colour node linked to a gradient's second stop colour. The card draws a socket per stop
// colour and position with the link's wire, the stop's Values row reads "Linked to <node>", the
// stop's handle shows the driven colour and is marked linked, and the Stop row's chip is disabled.
// Properties draws the same rows. Moving the stop past another keeps its link; removing it removes
// the link in the same undo step.
void linkedGradientStop() {
    CardFixture fixture(document::kLinearGradientNodeType);
    auto* card = fixture.item
                     ? qobject_cast<ui::NodeControlsCardWidget*>(fixture.item->cardWidget())
                     : nullptr;
    if (card == nullptr || card->gradientRows().size() != 1)
        return expect(false, "the Linear Gradient card draws its gradient row");
    auto* row = card->gradientRows().front();
    const auto colour = addNode(fixture.session, document::kColorValueNodeType);
    const auto value = ui::nodeRoleParameter(fixture.session, colour, "value");
    constexpr core::Color4d kDriven{0.9, 0.35, 0.05, 1.0};
    const auto driven = [&](const ui::kit::KColor& shown) {
        return std::abs(static_cast<double>(shown.red) - kDriven.red) < 1e-6 &&
               std::abs(static_cast<double>(shown.green) - kDriven.green) < 1e-6 &&
               std::abs(static_cast<double>(shown.blue) - kDriven.blue) < 1e-6;
    };
    {
        commands::Transaction transaction("Colour", fixture.session.snapshot().revision());
        transaction.emplace<commands::SetParameterSource>(fixture.session.compositionId(),
                                                          value.value_or(document::ParameterId{}),
                                                          document::ConstantValueSource{kDriven});
        expect(value && fixture.session.executeNodeTransaction(std::move(transaction)).succeeded(),
               "the Colour node takes an orange");
    }
    const auto port =
        document::gradientStopPortName("gradient", 1, document::GradientStopComponent::Color);
    {
        commands::Transaction transaction("Link", fixture.session.snapshot().revision());
        transaction.emplace<commands::ConnectPorts>(fixture.session.compositionId(),
                                                    document::OutputPortRef{colour, "value"},
                                                    document::NodeInputRef{fixture.node, port});
        expect(fixture.session.executeNodeTransaction(std::move(transaction)).succeeded(),
               "a Colour output links to a gradient stop colour");
    }
    // The card and Properties refresh, and the driven value resolves.
    QElapsedTimer settle;
    settle.start();
    const auto linkRow = [&]() -> const ui::GradientParameterRow::StopRow* {
        for (const auto& candidate : row->stopRows())
            if (candidate.stop == 1 &&
                candidate.component == document::GradientStopComponent::Color)
                return &candidate;
        return nullptr;
    };
    while (settle.elapsed() < 5'000 &&
           !(row->editor()->stops().size() == 2 && driven(row->editor()->stops()[1].color)))
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    fixture.item =
        ui::node_editor::nodeItemAncestor(fixture.editor->graphScene()->findNodeItem(fixture.node));
    card = fixture.item ? qobject_cast<ui::NodeControlsCardWidget*>(fixture.item->cardWidget())
                        : nullptr;
    if (card == nullptr || card->gradientRows().size() != 1)
        return expect(false, "the card still draws its gradient row after the link");
    row = card->gradientRows().front();
    QApplication::processEvents();
    const auto* stopRow = linkRow();
    expect(row->stopLink(1, document::GradientStopComponent::Color).has_value() && stopRow &&
               stopRow->shown && stopRow->link && stopRow->link->isVisible() &&
               stopRow->link->text() == QStringLiteral("Linked to Color") && stopRow->chip &&
               !stopRow->chip->isVisible(),
           "the linked stop's colour row reads Linked to Colour in place of its chip");
    const auto stops = row->editor()->stops();
    expect(stops.size() == 2 && stops[1].colorLinked && !stops[1].positionLinked &&
               driven(stops[1].color) && !stops[0].colorLinked,
           "the editor shows the driven colour on the linked stop and marks it linked");
    row->editor()->setSelectedStop(1);
    QApplication::processEvents();
    expect(!row->stopChip()->isEnabled() && row->stopPosition()->isEnabled(),
           "the selected linked stop's chip is disabled while its position stays editable");
    bool socket = false;
    for (const auto* item : fixture.item->sockets())
        socket = socket ||
                 item->accepts(document::InputPortRef{document::NodeInputRef{fixture.node, port}});
    expect(socket, "the card draws the stop colour's socket");

    QWidget panel;
    auto* properties = new ui::PropertiesEditor(fixture.session, &panel);
    properties->setGeometry(0, 0, 420, 900);
    panel.resize(420, 900);
    panel.show();
    fixture.session.selectNode(fixture.node);
    QApplication::processEvents();
    const ui::GradientParameterRow* inProperties = nullptr;
    for (auto* candidate : properties->findChildren<ui::GradientParameterRow*>())
        inProperties = candidate;
    settle.restart();
    while (settle.elapsed() < 5'000 && inProperties &&
           !(inProperties->editor()->stops().size() == 2 &&
             driven(inProperties->editor()->stops()[1].color)))
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    bool propertiesLinked = false;
    if (inProperties)
        for (const auto& candidate : inProperties->stopRows())
            propertiesLinked =
                propertiesLinked ||
                (candidate.stop == 1 && candidate.link && candidate.link->isVisible() &&
                 candidate.link->text() == QStringLiteral("Linked to Color"));
    expect(propertiesLinked && inProperties->editor()->stops()[1].colorLinked,
           "Properties shows the same linked stop row");
    capture(fixture, QStringLiteral("gradient-stop-linked"));
    row->setValuesShown(true);
    QApplication::processEvents();
    std::size_t shownRows = 0;
    for (const auto& candidate : row->stopRows())
        shownRows += candidate.shown ? 1U : 0U;
    expect(shownRows == 4, "the Values disclosure shows a colour and a position row per stop");
    capture(fixture, QStringLiteral("gradient-stop-values"));
    row->setValuesShown(false);
    QApplication::processEvents();
    if (const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
        !directory.isEmpty())
        properties->grab().save(directory + QStringLiteral("/gradient-stop-linked-properties.png"));

    // The linked stop moves to a quarter, then the other stop moves past it to three quarters:
    // each keeps its stored index, so the link stays on the orange stop.
    auto& editor = *row->editor();
    const auto bar = editor.barRect();
    const auto drag = [&](const std::size_t shown, const double to) {
        const QPointF from = editor.stopCentre(shown);
        const QPointF target(bar.left() + bar.width() * to, from.y());
        mouse(editor, QEvent::MouseButtonPress, from);
        mouse(editor, QEvent::MouseMove, target);
        mouse(editor, QEvent::MouseButtonRelease, target);
        QApplication::processEvents();
    };
    drag(1, 0.25);
    drag(0, 0.75);
    const auto link = ui::nodeRoleParameter(fixture.session, fixture.node, port);
    const auto moved = storedGradient(fixture.session, fixture.node);
    expect(link && moved && moved->stops.size() == 2 &&
               std::abs(moved->stops[0].position - 0.75) < 0.02 &&
               std::abs(moved->stops[1].position - 0.25) < 0.02 && editor.stops()[0].colorLinked,
           "stops moving past each other keep their stored indices and the link stays on its "
           "stop");
    // A third stop, then the linked one pulled off the bar: it goes with its link in one command.
    const QPointF at(bar.left() + bar.width() * 0.5, bar.center().y());
    mouse(editor, QEvent::MouseButtonPress, at);
    mouse(editor, QEvent::MouseButtonRelease, at);
    QApplication::processEvents();
    // Pulling it off the bar removes it and its link as one command.
    const auto commands = fixture.stack.size();
    const auto linked = editor.stopCentre(0);
    mouse(editor, QEvent::MouseButtonPress, linked);
    const auto away = linked + QPointF(0.0, ui::kit::px(ui::kit::Size::DropdownWidth));
    mouse(editor, QEvent::MouseMove, away);
    mouse(editor, QEvent::MouseButtonRelease, away);
    QApplication::processEvents();
    expect(fixture.stack.size() == commands + 1 &&
               !ui::nodeRoleParameter(fixture.session, fixture.node, port),
           "removing the linked stop removes its link in the same command");
    expect(fixture.session.undo() && ui::nodeRoleParameter(fixture.session, fixture.node, port),
           "one undo brings the stop back with its link");
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
    colorizeGradientMap();
    nodeControlCards();
    linkedGradientStop();
    if (failures != 0)
        return 1;
    std::cout << "PASS: grading cards\n";
    return 0;
}
