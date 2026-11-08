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
#include <bloom/document/channel_nodes.hpp>
#include <bloom/document/colorize_nodes.hpp>
#include <bloom/document/depth_effect_nodes.hpp>
#include <bloom/document/lens_flare_definition.hpp>
#include <bloom/document/film_grain_definition.hpp>
#include <bloom/document/motion_blur_parameters.hpp>
#include <bloom/document/motion_blur_nodes.hpp>
#include <bloom/document/sharpen_definition.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/parameter.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/vignette_nodes.hpp>
#include <bloom/ui/colorize_widget.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/curve_graph.hpp>
#include <bloom/ui/depth_effect_widget.hpp>
#include <bloom/ui/motion_blur_widget.hpp>
#include <bloom/ui/grade_wheels.hpp>
#include <bloom/ui/gradient_parameter_row.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/hue_saturation_widget.hpp>
#include <bloom/ui/lens_flare_widget.hpp>
#include <bloom/ui/codec_degrade_widget.hpp>
#include <bloom/ui/lens_distortion_widget.hpp>
#include <bloom/ui/chromatic_aberration_widget.hpp>
#include <bloom/ui/test_card_widget.hpp>
#include <bloom/document/test_card_nodes.hpp>
#include <bloom/ui/light_rays_widget.hpp>
#include <bloom/ui/film_grain_widget.hpp>
#include <bloom/ui/sharpen_widget.hpp>
#include <bloom/ui/kit/angle_dial.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/dropdown.hpp>
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
#include <bloom/ui/vignette_widget.hpp>

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
            // A wrapped label (a card's warning) is cut short when it is shorter than its lines.
            const bool cut = label->wordWrap()
                                 ? label->height() < label->heightForWidth(label->width())
                                 : label->width() <
                                       label->fontMetrics().horizontalAdvance(label->text());
            if (label->text().contains(QChar(0x2026)) || cut) {
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

// #505: the Vignette card in both modes paints inside its card with no label cut short and shows
// the strength its mode reads; its pad draws the falloff, which follows the centre and the shape,
// and dragging the centre moves it as one command; a slider drag is one command too.
// #507: the Lens Flare card paints inside its card with no label cut short in every part of the
// stack; the picker shows one part's rows and its sockets; the pad draws where the elements land and
// dragging the light is one command; a preset is one command and leaves the light alone.
void lensFlareCard() {
    CardFixture fixture(document::kLensFlareNodeType);
    auto* card = fixture.item ? qobject_cast<ui::LensFlareCardWidget*>(fixture.item->cardWidget())
                              : nullptr;
    expect(card != nullptr, "Lens Flare draws its card widget");
    if (card == nullptr)
        return;
    // A new card reads the node and writes nothing: every value keeps its default, however far the
    // slider's soft minimum lies above zero, and the edit does not build a second card.
    const auto defaultOf = [&](const std::string_view role) {
        const auto id = ui::nodeRoleParameter(fixture.session, fixture.node, role);
        const auto live = id ? fixture.session.liveValue(*id) : std::nullopt;
        const auto* value = live ? std::get_if<double>(&*live) : nullptr;
        return value ? *value : -1.0;
    };
    expect(defaultOf("flareSize") == 1.0 && defaultOf("sourceWhite") == 1.0 &&
               defaultOf("cameraZoom") == 2000.0,
           "a new Lens Flare card writes no value of its own");
    int cards = 0;
    for (auto* candidate : fixture.editor->graphScene()->items())
        if (auto* other = dynamic_cast<ui::node_editor::NodeItem*>(candidate);
            other != nullptr && other->cardWidget() != nullptr &&
            qobject_cast<ui::LensFlareCardWidget*>(other->cardWidget()) != nullptr)
            ++cards;
    expect(cards == 1, "a new Lens Flare is one card");
    const std::array<std::pair<document::LensFlareGroup, const char*>, 8> parts{{
        {document::LensFlareGroup::Light, "lens-flare-light"},
        {document::LensFlareGroup::Glow, "lens-flare-glow"},
        {document::LensFlareGroup::Streaks, "lens-flare-streaks"},
        {document::LensFlareGroup::Ghosts, "lens-flare-ghosts"},
        {document::LensFlareGroup::Rings, "lens-flare-ring"},
        {document::LensFlareGroup::Iris, "lens-flare-iris"},
        {document::LensFlareGroup::Hoop, "lens-flare-hoop"},
        {document::LensFlareGroup::Anamorphic, "lens-flare-anamorphic"},
    }};
    for (const auto& [group, name] : parts) {
        card->setGroup(group);
        QApplication::processEvents();
        if (!paintsInsideCard(*fixture.item)) {
            std::cerr << name << " paints outside its card\n";
            ++failures;
        }
        if (!cardContentFits(*fixture.item, name))
            ++failures;
        capture(fixture, QString::fromLatin1(name));
    }
    card->setGroup(document::LensFlareGroup::Ghosts);
    QApplication::processEvents();
    expect(card->valueRow("ghostAmount")->isVisibleTo(card) &&
               !card->valueRow("ringAmount")->isVisibleTo(card) &&
               card->valueRow("intensity")->isVisibleTo(card) &&
               card->socketY("ghostAmount").has_value() && !card->socketY("ringAmount").has_value() &&
               card->socketY("intensity").has_value() && card->socketY("flareRotation").has_value() &&
               card->showsRole("ghostCount") && card->showsRole("seed") &&
               !card->showsRole("irisBlades") && card->showsRole("lightPosition") &&
               card->socketY("lightColor").has_value() && !card->showsRole("lightColor") &&
               !card->socketY("anamorphicColor").has_value() &&
               !card->showsRole("anamorphicColor") && !card->showsRole("ghostAmount"),
           "the picker shows one part's values, its counts and their sockets");
    // The 3D values and the occlusion values show only when the state reads them.
    card->setGroup(document::LensFlareGroup::Light);
    QApplication::processEvents();
    expect(!card->valueRow("lightDistance")->isVisibleTo(card) &&
               !card->valueRow("occlusionAmount")->isVisibleTo(card) &&
               card->valueRow("sampleRadius")->isVisibleTo(card) && card->showsRole("lightMode"),
           "a new flare shows neither the 3D values nor the occlusion values");
    const auto mode = ui::nodeRoleParameter(fixture.session, fixture.node, "lightMode");
    expect(mode && fixture.session.setParameterValue(*mode, document::kLensFlareLight3D,
                                                     QStringLiteral("Set Mode")),
           "the light mode is set");
    QApplication::processEvents();
    expect(card->valueRow("lightDistance")->isVisibleTo(card) &&
               card->valueRow("cameraZoom")->isVisibleTo(card),
           "the 3D mode shows its distance and zoom");
    expect(mode && fixture.session.setParameterValue(*mode, document::kLensFlareLight2D,
                                                     QStringLiteral("Set Mode")),
           "the light mode is restored");
    QApplication::processEvents();

    // The pad draws the flare: brightest at the light. Dragging the light is one command.
    const auto brightness = [](const QImage& image, const double x, const double y) {
        return qGray(image.pixel(static_cast<int>(image.width() * x),
                                 static_cast<int>(image.height() * y)));
    };
    const auto before = card->preview(128);
    expect(brightness(before, 0.5 - 360.0 / 1920.0, 0.5 - 200.0 / 1080.0) >
               brightness(before, 0.97, 0.97) + 40,
           "the pad draws the flare at its light");
    auto& pad = *card->padRow()->pad();
    const auto commands = fixture.stack.size();
    const QPointF from = pad.screenPoint(pad.value(0));
    const QPointF to = from + QPointF(pad.frameRect().width() * 0.25, 0.0);
    mouse(pad, QEvent::MouseButtonPress, from);
    mouse(pad, QEvent::MouseMove, to);
    mouse(pad, QEvent::MouseButtonRelease, to);
    QApplication::processEvents();
    const auto position = ui::nodeRoleParameter(fixture.session, fixture.node, "lightPosition");
    const auto live = position ? fixture.session.liveValue(*position) : std::nullopt;
    const auto* moved = live ? std::get_if<document::Vec2d>(&*live) : nullptr;
    expect(fixture.stack.size() == commands + 1 && moved && moved->x > -360.0,
           "dragging the light moves it as one command");
    expect(card->preview(128) != before, "the pad redraws with the light");

    // A preset is one command, sets the stack and keeps the light where it is.
    const auto movedValue = moved ? *moved : document::Vec2d{};
    const auto presetCommands = fixture.stack.size();
    const auto& presets = document::lensFlarePresets();
    std::size_t anamorphic = presets.size();
    for (std::size_t index = 0; index < presets.size(); ++index)
        if (presets[index].name == "Anamorphic")
            anamorphic = index;
    expect(anamorphic < presets.size() && card->applyPreset(anamorphic),
           "the Anamorphic preset applies");
    QApplication::processEvents();
    const auto amount = ui::nodeRoleParameter(fixture.session, fixture.node, "anamorphicAmount");
    const auto after = position ? fixture.session.liveValue(*position) : std::nullopt;
    const auto* kept = after ? std::get_if<document::Vec2d>(&*after) : nullptr;
    expect(fixture.stack.size() == presetCommands + 1 && amount &&
               ui::liveScalar(fixture.session, *amount).value_or(0.0) > 1.0 && kept &&
               *kept == movedValue,
           "a preset is one command and keeps the light");
    capture(fixture, QStringLiteral("lens-flare-preset"));
}

// #486: the Film Grain card paints inside its card with no label cut short, writes nothing of its
// own, and shows its node's rows; the graph draws the grain as the node adds it and a handle's drag
// is one command that its row follows; a slider drag is one command too.
void filmGrainCard() {
    CardFixture fixture(document::kFilmGrainNodeType);
    auto* card = fixture.item ? qobject_cast<ui::FilmGrainCardWidget*>(fixture.item->cardWidget())
                              : nullptr;
    expect(card != nullptr, "Film Grain draws its card widget");
    if (card == nullptr)
        return;
    for (const auto& spec : document::filmGrainParameters())
        if (spec.kind == document::FilmGrainParameterKind::Scalar) {
            const auto value = stored(fixture.session, fixture.node, std::string(spec.role));
            expect(value && *value == spec.value,
                   "a new Film Grain card writes no value of its own");
        }
    int cards = 0;
    for (auto* candidate : fixture.editor->graphScene()->items())
        if (auto* other = dynamic_cast<ui::node_editor::NodeItem*>(candidate);
            other != nullptr && other->cardWidget() != nullptr &&
            qobject_cast<ui::FilmGrainCardWidget*>(other->cardWidget()) != nullptr)
            ++cards;
    expect(cards == 1, "a new Film Grain is one card");
    QApplication::processEvents();
    if (!paintsInsideCard(*fixture.item)) {
        std::cerr << "film-grain paints outside its card\n";
        ++failures;
    }
    if (!cardContentFits(*fixture.item, "film-grain"))
        ++failures;
    capture(fixture, QStringLiteral("film-grain"));
    expect(card->showsRole("animation") && card->showsRole("seed") &&
               card->showsRole("monochrome") && !card->socketY("monochrome").has_value() &&
               !card->showsRole("intensity") && !card->showsRole("shadows") &&
               card->socketY("intensity").has_value() && card->socketY("highlights").has_value() &&
               !card->socketY("seed").has_value(),
           "the card shows the node's constants as its own rows, and a socket beside every value");

    // The graph draws grain: the ramp rises left to right, and more intensity spreads it wider.
    auto& graph = *card->graph();
    const auto spread = [](const QImage& image, const int x) {
        double sum = 0.0;
        double squares = 0.0;
        for (int y = 0; y < image.height(); ++y) {
            const double value = qGreen(image.pixel(x, y));
            sum += value;
            squares += value * value;
        }
        const double count = image.height();
        return std::sqrt(std::max(0.0, squares / count - (sum / count) * (sum / count)));
    };
    const auto before = graph.preview(256, 256);
    expect(qGray(before.pixel(250, 128)) > qGray(before.pixel(5, 128)) + 100,
           "the graph's ramp rises from black to white");
    const auto intensity = ui::nodeRoleParameter(fixture.session, fixture.node, "intensity");
    expect(intensity && fixture.session.setParameterValue(*intensity, 0.1, QStringLiteral("Set")),
           "the intensity is set");
    QApplication::processEvents();
    const auto stronger = graph.preview(256, 256);
    expect(spread(stronger, 128) > spread(before, 128) * 2.0,
           "the graph's grain follows the intensity");
    capture(fixture, QStringLiteral("film-grain-strong"));

    // A handle's drag is one command, moves its own response up and its row follows.
    const auto commands = fixture.stack.size();
    const QPointF from = graph.handlePosition(1);
    const QPointF to = from + QPointF(0.0, -graph.plotRect().height() * 0.25);
    mouse(graph, QEvent::MouseButtonPress, from);
    mouse(graph, QEvent::MouseMove, to);
    mouse(graph, QEvent::MouseButtonRelease, to);
    QApplication::processEvents();
    const auto midtones = stored(fixture.session, fixture.node, "midtones");
    const auto shadows = stored(fixture.session, fixture.node, "shadows");
    expect(fixture.stack.size() == commands + 1 && midtones && *midtones > 1.3 && shadows &&
               *shadows == 0.1,
           "dragging the midtones handle raises it alone, as one command");
    expect(graph.handlePosition(1).y() < from.y() - 1.0, "the handle follows its value");
    capture(fixture, QStringLiteral("film-grain-response"));
    // A double-click resets it.
    const auto resetFrom = graph.handlePosition(1);
    mouse(graph, QEvent::MouseButtonDblClick, resetFrom);
    QApplication::processEvents();
    const auto reset = stored(fixture.session, fixture.node, "midtones");
    expect(reset && *reset == 1.0, "a double-click on a handle resets it");
    // A slider drag is one command.
    auto* slider = card->valueRow("size")->slider();
    const auto sliderCommands = fixture.stack.size();
    const QPointF start = slider->rect().center();
    mouse(*slider, QEvent::MouseButtonPress, start);
    mouse(*slider, QEvent::MouseMove, start + QPointF(slider->width() * 0.3, 0.0));
    mouse(*slider, QEvent::MouseButtonRelease, start + QPointF(slider->width() * 0.3, 0.0));
    const auto size = stored(fixture.session, fixture.node, "size");
    expect(fixture.stack.size() == sliderCommands + 1 && size && *size > 1.0,
           "dragging the size slider is one command");
    capture(fixture, QStringLiteral("film-grain-large"));

    // Monochrome is the node's own switch, off for a new node. Turned on, it is one command, the
    // graph's grain goes neutral and nothing leaves the card; undo restores it.
    {
        const auto monochrome = [&] {
            const auto id = ui::nodeRoleParameter(fixture.session, fixture.node, "monochrome");
            const auto live = id ? fixture.session.liveValue(*id) : std::nullopt;
            const auto* value = live ? std::get_if<bool>(&*live) : nullptr;
            return value ? std::optional(*value) : std::nullopt;
        };
        const auto neutral = [](const QImage& image) {
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (const auto rgb = image.pixel(x, y);
                        qRed(rgb) != qGreen(rgb) || qGreen(rgb) != qBlue(rgb))
                        return false;
            return true;
        };
        auto* toggle = qobject_cast<ui::kit::KSwitch*>(fixture.editor->graphScene()->nodeFieldForTest(
            fixture.node, QStringLiteral("nodeOperandToggle")));
        expect(toggle != nullptr && monochrome() == false && !toggle->isChecked(),
               "Monochrome is a switch on the card, off for a new node");
        expect(!neutral(graph.preview(256, 256)), "with Monochrome off the graph's grain has colour");
        capture(fixture, QStringLiteral("film-grain-monochrome-off"));
        if (toggle != nullptr) {
            const auto toggleCommands = fixture.stack.size();
            toggle->setChecked(true);
            QApplication::processEvents();
            expect(fixture.stack.size() == toggleCommands + 1 && monochrome() == true,
                   "turning Monochrome on is one command");
            expect(neutral(graph.preview(256, 256)), "with Monochrome on the graph's grain is neutral");
            if (!paintsInsideCard(*fixture.item) || !cardContentFits(*fixture.item, "film-grain-mono")) {
                std::cerr << "film-grain with Monochrome on leaves its card or cuts a label\n";
                ++failures;
            }
            capture(fixture, QStringLiteral("film-grain-monochrome-on"));
            const bool undone = fixture.session.undo();
            QApplication::processEvents();
            expect(undone && monochrome() == false, "undo turns Monochrome off");
        }
    }

    // #565 Sensor Noise is the node's own Mode, Film for a new node and for the rows above. Choosing
    // it is one command that swaps the response rows for the four Sensor Noise rows, each with its
    // socket, keeps the card inside itself with every label whole, and draws the shadow weighting in
    // the graph, whose handles go; undo restores Film.
    {
        expect(card->showsRole("mode") && !card->socketY("mode").has_value() &&
                   !card->socketY("shadowWeighting").has_value() &&
                   !card->valueRow("shadowWeighting")->isVisibleTo(card) &&
                   card->valueRow("shadows")->isVisibleTo(card),
               "a new Film Grain is in Film mode and shows no Sensor Noise row");
        const int filmHeight = card->heightAt(card->minimumWidthHint());
        const auto mode = ui::nodeRoleParameter(fixture.session, fixture.node, "mode");
        const auto modeValue = [&]() -> std::optional<std::int64_t> {
            const auto live = mode ? fixture.session.liveValue(*mode) : std::nullopt;
            const auto* value = live ? std::get_if<std::int64_t>(&*live) : nullptr;
            return value ? std::optional(*value) : std::nullopt;
        };
        expect(modeValue() == document::kFilmGrainFilm, "a new Film Grain is in Film mode");
        expect(mode && fixture.session.setParameterValue(*mode, document::kFilmGrainSensorNoise,
                                                         QStringLiteral("Set Mode")),
               "the mode is set to Sensor Noise");
        QApplication::processEvents();
        expect(modeValue() == document::kFilmGrainSensorNoise, "the mode is Sensor Noise");
        // One command: a single undo is Film again, and a redo is Sensor Noise.
        expect(fixture.session.undo() && modeValue() == document::kFilmGrainFilm &&
                   fixture.session.redo() && modeValue() == document::kFilmGrainSensorNoise,
               "choosing Sensor Noise is one command");
        QApplication::processEvents();
        for (const auto* role : {"shadowWeighting", "chromaBlotches", "blotchSize", "fixedPattern"})
            expect(card->socketY(role).has_value() && card->valueRow(role)->isVisibleTo(card),
                   "Sensor Noise shows its row and its socket");
        for (const auto* role : {"shadows", "midtones", "highlights"})
            expect(!card->socketY(role).has_value() && !card->valueRow(role)->isVisibleTo(card),
                   "Sensor Noise hides the Film response rows");
        expect(card->socketY("intensity").has_value() && card->socketY("size").has_value() &&
                   card->heightAt(card->minimumWidthHint()) > filmHeight,
               "Sensor Noise keeps the shared rows and has one more");
        if (!paintsInsideCard(*fixture.item) || !cardContentFits(*fixture.item, "film-grain-sensor")) {
            std::cerr << "film-grain in Sensor Noise leaves its card or cuts a label\n";
            ++failures;
        }
        capture(fixture, QStringLiteral("film-grain-sensor-noise"));
        // The graph's ramp shows the weighting: the noise is wider in the shadows than the highlights.
        const auto sensor = graph.preview(256, 256);
        expect(spread(sensor, 20) > spread(sensor, 235) * 2.0,
               "the graph draws noise weighted toward the shadows");
        const auto handleCommands = fixture.stack.size();
        const QPointF handle = graph.handlePosition(1);
        mouse(graph, QEvent::MouseButtonPress, handle);
        mouse(graph, QEvent::MouseMove, handle + QPointF(0.0, -20.0));
        mouse(graph, QEvent::MouseButtonRelease, handle + QPointF(0.0, -20.0));
        QApplication::processEvents();
        expect(fixture.stack.size() == handleCommands && stored(fixture.session, fixture.node,
                                                                "midtones") == 1.0,
               "Sensor Noise's graph has no handles to drag");
        // A slider drag is one command.
        auto* weighting = card->valueRow("shadowWeighting")->slider();
        const QPointF origin = weighting->rect().center();
        mouse(*weighting, QEvent::MouseButtonPress, origin);
        mouse(*weighting, QEvent::MouseMove, origin + QPointF(weighting->width() * 0.2, 0.0));
        mouse(*weighting, QEvent::MouseButtonRelease, origin + QPointF(weighting->width() * 0.2, 0.0));
        const auto moved = stored(fixture.session, fixture.node, "shadowWeighting");
        expect(moved && *moved > 1.0, "dragging Shadow weighting moves it");
        expect(fixture.session.undo() && stored(fixture.session, fixture.node, "shadowWeighting") == 1.0 &&
                   modeValue() == document::kFilmGrainSensorNoise,
               "the drag is one command");
        const bool undone = fixture.session.undo();
        QApplication::processEvents();
        expect(undone && modeValue() == document::kFilmGrainFilm &&
                   card->valueRow("shadows")->isVisibleTo(card) &&
                   !card->valueRow("shadowWeighting")->isVisibleTo(card),
               "undo restores Film and its rows");
    }
}

// #512: the Camera Shake card draws the roll amount and the shutter angle as dials on their own
// rows (the roll after Amplitude Y, the shutter angle inside the motion blur's rows), its Randomise
// seed button on the Seed row, and shows the shutter only with its motion blur on, the loop length
// only with Loop and the octaves not under Impact; Randomise seed is one command that gives the node
// a new seed. A new node already has a seed of its own, not the definition's.
void cameraShakeCard() {
    CardFixture fixture(document::kCameraShakeNodeType);
    if (fixture.item == nullptr) {
        expect(false, "Camera Shake draws a card");
        return;
    }
    QApplication::processEvents();
    if (!paintsInsideCard(*fixture.item)) {
        std::cerr << "camera shake paints outside its card\n";
        ++failures;
    }
    if (!cardContentFits(*fixture.item, "camera-shake"))
        ++failures;
    capture(fixture, QStringLiteral("camera-shake"));
    // The card's rows by the parameter role they edit.
    const auto rowOf = [&](const char* role) -> QWidget* {
        for (auto* child : fixture.item->childItems())
            if (auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child);
                proxy != nullptr && proxy->isVisible() && proxy->widget() != nullptr &&
                proxy->widget()->property("nodeParameterRole").toString() == QLatin1String(role))
                return proxy->widget();
        return nullptr;
    };
    const auto rowTop = [&](const char* role) {
        auto* row = rowOf(role);
        for (auto* child : fixture.item->childItems())
            if (auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child);
                proxy != nullptr && row != nullptr && proxy->widget() == row)
                return proxy->pos().y();
        return -1.0;
    };
    const auto seedOf = [&]() -> std::optional<std::int64_t> {
        const auto id = ui::nodeRoleParameter(fixture.session, fixture.node, "seed");
        const auto live = id ? fixture.session.liveValue(*id) : std::nullopt;
        const auto* value = live ? std::get_if<std::int64_t>(&*live) : nullptr;
        return value ? std::optional(*value) : std::nullopt;
    };
    const auto first = seedOf();
    expect(first && *first != 1, "a new Camera Shake has a seed of its own");

    // The roll's dial sits on its own row, before its field, between Amplitude Y and Frequency.
    auto* roll = rowOf("rollAmount");
    auto* rollDial = roll ? roll->findChild<ui::kit::KAngleDial*>() : nullptr;
    auto* rollField = roll ? roll->findChild<ui::kit::KValueField*>() : nullptr;
    expect(rollDial != nullptr && rollField != nullptr && rollDial->x() < rollField->x(),
           "the roll amount is a dial before its field on its own row");
    expect(rowTop("amplitudeY") < rowTop("rollAmount") && rowTop("rollAmount") < rowTop("frequency"),
           "the roll amount follows Amplitude Y");
    expect(fixture.item->cardWidget() != nullptr &&
               !qobject_cast<ui::NodeControlsCardWidget*>(fixture.item->cardWidget())
                    ->showsRole("loopLength") &&
               !qobject_cast<ui::NodeControlsCardWidget*>(fixture.item->cardWidget())
                    ->showsRole("quality"),
           "the card hides the loop length and the shutter until they are read");
    expect(rowOf("loopLength") == nullptr && rowOf("quality") == nullptr &&
               rowOf("shutterAngle") == nullptr,
           "their rows are not shown");

    // Frequency, roughness and smoothing carry a keyframe diamond as the amplitudes do.
    for (const char* role : {"amplitudeX", "frequency", "roughness", "smoothing"}) {
        auto* row = rowOf(role);
        expect(row != nullptr && row->findChild<ui::KeyframeDiamond*>() != nullptr,
               "an animatable Camera Shake value has a keyframe diamond");
    }

    // Randomise seed sits on the Seed row beside its field.
    auto* seedRow = rowOf("seed");
    auto* randomise = seedRow ? seedRow->findChild<ui::kit::KButton*>(QStringLiteral("randomiseSeed"))
                              : nullptr;
    auto* seedField = seedRow ? seedRow->findChild<ui::kit::KValueField*>() : nullptr;
    expect(randomise != nullptr && randomise->isEnabled() && seedField != nullptr &&
               randomise->x() > seedField->x(),
           "Randomise seed is on the Seed row, after its field");
    if (randomise != nullptr) {
        const auto commands = fixture.stack.size();
        randomise->click();
        QApplication::processEvents();
        const auto second = seedOf();
        expect(fixture.stack.size() == commands + 1 && second && first && *second != *first &&
                   *second >= 0,
               "Randomise seed is one command that sets a new seed");
        expect(fixture.session.undo() && seedOf() == first, "undo restores the seed");
    }

    // The dial follows the value.
    const auto set = [&](const std::string& role, const document::ParameterValue& value) {
        const auto id = ui::nodeRoleParameter(fixture.session, fixture.node, role);
        expect(id && fixture.session.setParameterValue(*id, value, QStringLiteral("Set")),
               "a value is set");
        QApplication::processEvents();
    };
    set("rollAmount", 3.0);
    rollDial = rowOf("rollAmount")->findChild<ui::kit::KAngleDial*>();
    expect(rollDial != nullptr && std::abs(rollDial->value() - 3.0) < 1e-9,
           "the roll dial follows the roll amount");
    set("rollAmount", 0.5);

    // Loop reveals the loop length; the motion blur reveals the shutter, its angle a dial after
    // Motion blur and before Shutter opens.
    set("loop", true);
    expect(rowOf("loopLength") != nullptr, "Loop shows the loop length");
    set("motionBlur", true);
    expect(rowOf("quality") != nullptr && rowOf("shutterOffset") != nullptr,
           "Motion blur shows the shutter's rows");
    auto* shutter = rowOf("shutterAngle");
    auto* shutterDial = shutter ? shutter->findChild<ui::kit::KAngleDial*>() : nullptr;
    expect(shutterDial != nullptr && rowTop("motionBlur") < rowTop("shutterAngle") &&
               rowTop("shutterAngle") < rowTop("shutterOffset"),
           "the shutter angle is a dial inside the motion blur's rows");
    capture(fixture, QStringLiteral("camera-shake-motion-blur"));
    set("mode", std::int64_t{1});
    expect(rowOf("octaves") == nullptr && rowOf("roughness") == nullptr,
           "Impact hides the octaves and roughness");
    capture(fixture, QStringLiteral("camera-shake-impact"));
}

// #563: the Codec Degrade card paints inside its card with no label cut short, writes nothing of its
// own, shows Quality and P-frame persistence as sliders with a socket beside each, leaves the block
// size, the GOP length and the chroma bleed to the node's own rows; a slider drag is one command.
void codecDegradeCard() {
    CardFixture fixture(document::kCodecDegradeNodeType);
    auto* card = fixture.item ? qobject_cast<ui::CodecDegradeCardWidget*>(fixture.item->cardWidget())
                              : nullptr;
    expect(card != nullptr, "Codec Degrade draws its card widget");
    if (card == nullptr)
        return;
    for (const auto& spec : document::codecDegradeParameters())
        if (spec.kind == document::CodecDegradeParameterKind::Scalar) {
            const auto value = stored(fixture.session, fixture.node, std::string(spec.role));
            expect(value && *value == spec.value,
                   "a new Codec Degrade card writes no value of its own");
        }
    QApplication::processEvents();
    if (!paintsInsideCard(*fixture.item)) {
        std::cerr << "codec-degrade paints outside its card\n";
        ++failures;
    }
    if (!cardContentFits(*fixture.item, "codec-degrade"))
        ++failures;
    capture(fixture, QStringLiteral("codec-degrade"));
    expect(card->showsRole("blockSize") && card->showsRole("gopLength") &&
               card->showsRole("chromaBleed") && !card->showsRole("quality") &&
               !card->showsRole("persistence") && card->socketY("quality").has_value() &&
               card->socketY("persistence").has_value() && !card->socketY("blockSize").has_value(),
           "the card shows the node's constants as its own rows, and a socket beside each slider");
    // A slider drag is one command.
    auto* slider = card->valueRow("quality")->slider();
    const auto commands = fixture.stack.size();
    const QPointF start = slider->rect().center();
    mouse(*slider, QEvent::MouseButtonPress, start);
    mouse(*slider, QEvent::MouseMove, start + QPointF(slider->width() * 0.3, 0.0));
    mouse(*slider, QEvent::MouseButtonRelease, start + QPointF(slider->width() * 0.3, 0.0));
    const auto quality = stored(fixture.session, fixture.node, "quality");
    expect(fixture.stack.size() == commands + 1 && quality && *quality > 40.0,
           "dragging the quality slider is one command");
    capture(fixture, QStringLiteral("codec-degrade-quality"));
}

// #486: the Lens Distortion card paints inside its card with no label cut short, writes nothing of
// its own, draws the four coefficients as sliders with a socket beside each and the centre on a point
// pad, leaves the mode to the node's own row; a slider drag is one command.
void lensDistortionCard() {
    CardFixture fixture(document::kLensDistortionNodeType);
    auto* card = fixture.item
                     ? qobject_cast<ui::LensDistortionCardWidget*>(fixture.item->cardWidget())
                     : nullptr;
    expect(card != nullptr, "Lens Distortion draws its card widget");
    if (card == nullptr)
        return;
    for (const auto& spec : document::lensDistortionParameters())
        if (spec.kind == document::LensDistortionParameterKind::Scalar) {
            const auto value = stored(fixture.session, fixture.node, std::string(spec.role));
            expect(value && *value == spec.value,
                   "a new Lens Distortion card writes no value of its own");
        }
    QApplication::processEvents();
    if (!paintsInsideCard(*fixture.item)) {
        std::cerr << "lens-distortion paints outside its card\n";
        ++failures;
    }
    if (!cardContentFits(*fixture.item, "lens-distortion"))
        ++failures;
    expect(ui::node_editor::displayTypeName(document::kLensDistortionStMapPort) ==
               QStringLiteral("ST Map"),
           "the second output reads ST Map, its stored id unchanged");
    capture(fixture, QStringLiteral("lens-distortion"));
    expect(card->showsRole("mode") && card->showsRole("center") && !card->showsRole("k1") &&
               !card->showsRole("k2") && !card->showsRole("p1") && !card->showsRole("p2") &&
               card->socketY("k1").has_value() && card->socketY("k2").has_value() &&
               card->socketY("p1").has_value() && card->socketY("p2").has_value() &&
               !card->socketY("mode").has_value() && card->padRow() != nullptr,
           "the card shows the mode and centre as the node's rows, and a socket beside each slider");
    expect(card->valueRow("k1")->name() == QStringLiteral("Radial k1") &&
               card->valueRow("p2")->name() == QStringLiteral("Tangential p2"),
           "the coefficient rows are named in full");
    // A slider drag is one command.
    auto* slider = card->valueRow("k1")->slider();
    const auto commands = fixture.stack.size();
    const QPointF start = slider->rect().center();
    mouse(*slider, QEvent::MouseButtonPress, start);
    mouse(*slider, QEvent::MouseMove, start + QPointF(slider->width() * 0.3, 0.0));
    mouse(*slider, QEvent::MouseButtonRelease, start + QPointF(slider->width() * 0.3, 0.0));
    const auto k1 = stored(fixture.session, fixture.node, "k1");
    expect(fixture.stack.size() == commands + 1 && k1 && *k1 > 0.0,
           "dragging the k1 slider is one command");
    capture(fixture, QStringLiteral("lens-distortion-k1"));
}

// #486: the Chromatic Aberration card paints inside its card with no label cut short, writes nothing
// of its own, draws the amount as a slider with a socket beside it and the centre on a point pad; a
// slider drag is one command.
void chromaticAberrationCard() {
    CardFixture fixture(document::kChromaticAberrationNodeType);
    auto* card = fixture.item
                     ? qobject_cast<ui::ChromaticAberrationCardWidget*>(fixture.item->cardWidget())
                     : nullptr;
    expect(card != nullptr, "Chromatic Aberration draws its card widget");
    if (card == nullptr)
        return;
    const auto value = stored(fixture.session, fixture.node, "amount");
    expect(value && *value == 0.0, "a new Chromatic Aberration card writes no value of its own");
    QApplication::processEvents();
    if (!paintsInsideCard(*fixture.item)) {
        std::cerr << "chromatic-aberration paints outside its card\n";
        ++failures;
    }
    if (!cardContentFits(*fixture.item, "chromatic-aberration"))
        ++failures;
    capture(fixture, QStringLiteral("chromatic-aberration"));
    expect(card->showsRole("center") && !card->showsRole("amount") &&
               card->socketY("amount").has_value() && card->padRow() != nullptr,
           "the card shows the centre as the node's row, and a socket beside the amount slider");
    expect(card->valueRow("amount")->name() == QStringLiteral("Amount"),
           "the amount row is named in full");
    // A slider drag is one command.
    auto* slider = card->valueRow("amount")->slider();
    const auto commands = fixture.stack.size();
    const QPointF start = slider->rect().center();
    mouse(*slider, QEvent::MouseButtonPress, start);
    mouse(*slider, QEvent::MouseMove, start + QPointF(slider->width() * 0.3, 0.0));
    mouse(*slider, QEvent::MouseButtonRelease, start + QPointF(slider->width() * 0.3, 0.0));
    const auto amount = stored(fixture.session, fixture.node, "amount");
    expect(fixture.stack.size() == commands + 1 && amount && *amount > 0.0,
           "dragging the amount slider is one command");
    capture(fixture, QStringLiteral("chromatic-aberration-amount"));
}

// #587: the Test Card card shows only the controls of the chosen card (Width and Height only for a
// Custom size), paints inside its card, and a Patch shows its signal and the display value the Viewer
// should produce.
void testCardCard() {
    CardFixture fixture(document::kTestCardNodeType);
    auto* card = fixture.item ? qobject_cast<ui::TestCardCardWidget*>(fixture.item->cardWidget())
                              : nullptr;
    expect(card != nullptr, "Test Card draws its card widget");
    if (card == nullptr)
        return;
    const auto set = [&](const std::string_view role, const document::ParameterValue& value) {
        const auto id = ui::nodeRoleParameter(fixture.session, fixture.node, role);
        expect(id && fixture.session.setParameterValue(*id, value, QStringLiteral("Set")),
               "a Test Card value is set");
        QApplication::processEvents();
    };
    expect(card->showsRole("card") && card->showsRole("sizeMode") && !card->showsRole("width") &&
               !card->showsRole("patchValue") && !card->showsRole("gridSpacing") &&
               !card->showsRole("label") && !card->showsReadout(),
           "SD Bars shows only the card and the size");
    if (!paintsInsideCard(*fixture.item)) {
        std::cerr << "test-card paints outside its card\n";
        ++failures;
    }
    if (!cardContentFits(*fixture.item, "test-card"))
        ++failures;
    capture(fixture, QStringLiteral("test-card-sd-bars"));
    set("sizeMode", std::int64_t{1});
    expect(card->showsRole("width") && card->showsRole("height"),
           "a Custom size shows Width and Height");
    set("sizeMode", std::int64_t{0});
    set("card", std::int64_t{static_cast<std::int64_t>(document::TestCard::Grid)});
    expect(card->showsRole("gridSpacing") && card->showsRole("gridWidth") && card->showsRole("label") &&
               !card->showsRole("patchValue") && !card->showsRole("starSpokes"),
           "Grid shows its own controls and no other card's");
    set("card", std::int64_t{static_cast<std::int64_t>(document::TestCard::Patch)});
    expect(card->showsRole("patchValue") && card->showsRole("patchWindow") &&
               card->showsRole("patchSpace") && !card->showsRole("label") && card->showsReadout(),
           "Patch shows its value, window and color space, no label, and the readout");
    set("patchStep", true);
    expect(!card->showsRole("patchValue"), "a stepping Patch hides its single value");
    set("patchStep", false);
    QElapsedTimer wait;
    wait.start();
    while (card->expectedText().isEmpty() && wait.elapsed() < 5'000)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    expect(!card->signalText().isEmpty() && !card->expectedText().isEmpty(),
           "a Patch shows its signal and the expected display value");
    if (!paintsInsideCard(*fixture.item)) {
        std::cerr << "test-card patch paints outside its card\n";
        ++failures;
    }
    if (!cardContentFits(*fixture.item, "test-card-patch"))
        ++failures;
    capture(fixture, QStringLiteral("test-card-patch"));
}

// #508: the Light Rays card paints inside its card with no label cut short, writes nothing of its
// own, draws the length, falloff and boost as sliders with a socket beside each and the centre on a
// point pad; the source and the colour stay the node's own rows; a slider drag is one command.
void lightRaysCard() {
    CardFixture fixture(document::kLightRaysNodeType);
    auto* card = fixture.item ? qobject_cast<ui::LightRaysCardWidget*>(fixture.item->cardWidget())
                              : nullptr;
    expect(card != nullptr, "Light Rays draws its card widget");
    if (card == nullptr)
        return;
    const auto value = stored(fixture.session, fixture.node, "length");
    expect(value && *value == document::kLightRaysLengthDefault,
           "a new Light Rays card writes no value of its own");
    QApplication::processEvents();
    if (!paintsInsideCard(*fixture.item)) {
        std::cerr << "light-rays paints outside its card\n";
        ++failures;
    }
    if (!cardContentFits(*fixture.item, "light-rays"))
        ++failures;
    {
        // The tint chip converts its reference color for display (a constant color key
        // must not fall back to the unconverted gray).
        QElapsedTimer converter;
        converter.start();
        while (!fixture.session.colorConverterReady() && converter.elapsed() < 5'000)
            QApplication::processEvents(QEventLoop::AllEvents, 20);
        const auto reference = ui::kit::KColor::fromRgba(1.0F, 1.0F, 1.0F, 1.0F,
                                                         ui::kit::ColorSpace::Reference);
        const auto display = reference.converted(
            ui::kit::ColorSpace::Display,
            fixture.session.colorConverter(document::kLightRaysColorSchemaKey));
        expect(display.has_value(), "the Light Rays tint converts for display");
    }
    capture(fixture, QStringLiteral("light-rays"));
    expect(card->showsRole("center") && card->showsRole("source") && !card->showsRole("tint") &&
               !card->showsRole("length") && !card->showsRole("falloff") &&
               !card->showsRole("boost") && card->padRow() != nullptr &&
               card->colorRow() != nullptr && card->colorRow()->chip() != nullptr,
           "the card shows the source and centre as the node's rows and draws the colour chip");
    for (const auto* role : {"length", "falloff", "boost"})
        expect(card->socketY(role).has_value() && card->valueRow(role) != nullptr,
               "a socket sits beside each slider");
    expect(card->valueRow("length")->name() == QStringLiteral("Length") &&
               card->valueRow("falloff")->name() == QStringLiteral("Falloff") &&
               card->valueRow("boost")->name() == QStringLiteral("Boost"),
           "the rows are named in full");
    // A slider drag is one command.
    auto* slider = card->valueRow("boost")->slider();
    const auto commands = fixture.stack.size();
    const QPointF start = slider->rect().center();
    mouse(*slider, QEvent::MouseButtonPress, start);
    mouse(*slider, QEvent::MouseMove, start + QPointF(slider->width() * 0.3, 0.0));
    mouse(*slider, QEvent::MouseButtonRelease, start + QPointF(slider->width() * 0.3, 0.0));
    const auto boost = stored(fixture.session, fixture.node, "boost");
    expect(fixture.stack.size() == commands + 1 && boost &&
               *boost != document::kLightRaysBoostDefault,
           "dragging the boost slider is one command");
    capture(fixture, QStringLiteral("light-rays-boost"));
}

// #486: the Sharpen card paints inside its card with no label cut short, writes nothing of its own,
// and shows its node's rows; the graph follows the values, each handle's drag is one command that
// moves its own value alone and its row follows; a slider drag is one command too.
void sharpenCard() {
    CardFixture fixture(document::kSharpenNodeType);
    auto* card =
        fixture.item ? qobject_cast<ui::SharpenCardWidget*>(fixture.item->cardWidget()) : nullptr;
    expect(card != nullptr, "Sharpen draws its card widget");
    if (card == nullptr)
        return;
    for (const auto& spec : document::sharpenParameters()) {
        const auto value = stored(fixture.session, fixture.node, std::string(spec.role));
        expect(value && *value == spec.value, "a new Sharpen card writes no value of its own");
    }
    QApplication::processEvents();
    if (!paintsInsideCard(*fixture.item)) {
        std::cerr << "sharpen paints outside its card\n";
        ++failures;
    }
    if (!cardContentFits(*fixture.item, "sharpen"))
        ++failures;
    capture(fixture, QStringLiteral("sharpen"));
    expect(!card->showsRole("amount") && !card->showsRole("radius") && !card->showsRole("threshold") &&
               card->socketY("amount").has_value() && card->socketY("threshold").has_value(),
           "the card shows every value on its own row with a socket beside it");

    // The graph draws the node's edge: an overshoot each side that follows the amount.
    auto& graph = *card->graph();
    const double peak = graph.sharpenedAt(0);
    expect(peak > 0.6 && graph.sharpenedAt(-1) < 0.2, "the edge overshoots on both sides");
    expect(std::abs(graph.sharpenedAt(16) - 0.6) < 1e-9, "past the radius the edge is unchanged");
    const auto before = graph.preview(256, 8);
    expect(qGray(before.pixel(250, 4)) > qGray(before.pixel(5, 4)) + 40,
           "the picture is dark then bright across the edge");
    const auto amount = ui::nodeRoleParameter(fixture.session, fixture.node, "amount");
    expect(amount && fixture.session.setParameterValue(*amount, 2.5, QStringLiteral("Set")),
           "the amount is set");
    QApplication::processEvents();
    expect(graph.sharpenedAt(0) > peak + 0.1, "the graph follows the amount");
    expect(graph.preview(256, 8) != before, "the picture follows the amount");
    capture(fixture, QStringLiteral("sharpen-strong"));
    const auto threshold = ui::nodeRoleParameter(fixture.session, fixture.node, "threshold");
    expect(threshold && fixture.session.setParameterValue(*threshold, 0.5, QStringLiteral("Set")) &&
               graph.sharpenedAt(0) == 0.6,
           "detail under the threshold leaves the edge unchanged");
    expect(fixture.session.setParameterValue(*threshold, 0.0, QStringLiteral("Set")) &&
               fixture.session.setParameterValue(*amount, 1.0, QStringLiteral("Set")),
           "the values go back");
    QApplication::processEvents();

    // The amount handle's drag is one command, moves the amount alone and the handle follows.
    const auto commands = fixture.stack.size();
    const QPointF from = graph.handlePosition(0);
    const QPointF to = from + QPointF(0.0, -graph.plotRect().height() * 0.1);
    mouse(graph, QEvent::MouseButtonPress, from);
    mouse(graph, QEvent::MouseMove, to);
    mouse(graph, QEvent::MouseButtonRelease, to);
    QApplication::processEvents();
    const auto raised = stored(fixture.session, fixture.node, "amount");
    const auto unmoved = stored(fixture.session, fixture.node, "radius");
    expect(fixture.stack.size() == commands + 1 && raised && *raised > 1.0 && unmoved &&
               *unmoved == 3.0,
           "dragging the overshoot raises the amount alone, as one command");
    expect(std::abs(graph.handlePosition(0).y() - to.y()) < 2.0,
           "the amount handle follows the pointer");
    capture(fixture, QStringLiteral("sharpen-amount"));

    // The radius handle's drag is one command, moves the radius alone and the handle follows.
    const auto radiusCommands = fixture.stack.size();
    const QPointF radiusFrom = graph.handlePosition(1);
    const QPointF radiusTo = radiusFrom + QPointF(graph.plotRect().width() * 0.15, 0.0);
    mouse(graph, QEvent::MouseButtonPress, radiusFrom);
    mouse(graph, QEvent::MouseMove, radiusTo);
    mouse(graph, QEvent::MouseButtonRelease, radiusTo);
    QApplication::processEvents();
    const auto wider = stored(fixture.session, fixture.node, "radius");
    const auto sameAmount = stored(fixture.session, fixture.node, "amount");
    expect(fixture.stack.size() == radiusCommands + 1 && wider && *wider > 3.0 && sameAmount &&
               sameAmount == raised,
           "dragging the end of the halo widens the radius alone, as one command");
    expect(std::abs(graph.handlePosition(1).x() - radiusTo.x()) < 2.0,
           "the radius handle follows the pointer");
    capture(fixture, QStringLiteral("sharpen-wide"));
    // A double-click resets it.
    mouse(graph, QEvent::MouseButtonDblClick, graph.handlePosition(1));
    QApplication::processEvents();
    const auto reset = stored(fixture.session, fixture.node, "radius");
    expect(reset && *reset == 3.0, "a double-click on a handle resets it");
    // A slider drag is one command.
    auto* slider = card->valueRow("threshold")->slider();
    const auto sliderCommands = fixture.stack.size();
    const QPointF start = slider->rect().center();
    mouse(*slider, QEvent::MouseButtonPress, start);
    mouse(*slider, QEvent::MouseMove, start + QPointF(slider->width() * 0.3, 0.0));
    mouse(*slider, QEvent::MouseButtonRelease, start + QPointF(slider->width() * 0.3, 0.0));
    const auto dragged = stored(fixture.session, fixture.node, "threshold");
    expect(fixture.stack.size() == sliderCommands + 1 && dragged && *dragged > 0.0,
           "dragging the threshold slider is one command");
}

void vignetteCard() {
    CardFixture fixture(document::kVignetteNodeType);
    auto* card = fixture.item ? qobject_cast<ui::VignetteCardWidget*>(fixture.item->cardWidget())
                              : nullptr;
    expect(card != nullptr, "Vignette draws its card widget");
    const auto mode = ui::nodeRoleParameter(fixture.session, fixture.node, "mode");
    const auto shape = ui::nodeRoleParameter(fixture.session, fixture.node, "shape");
    expect(mode && shape, "Vignette binds its mode and shape");
    if (card == nullptr || !mode || !shape)
        return;
    expect(card->mode() == document::kVignetteMultiply, "a new Vignette multiplies");
    const std::array<std::tuple<std::int64_t, std::int64_t, const char*>, 3> looks{
        {{document::kVignetteMultiply, document::kVignetteElliptical, "vignette-multiply"},
         {document::kVignetteExposure, document::kVignetteElliptical, "vignette-exposure"},
         {document::kVignetteExposure, document::kVignetteRectangular, "vignette-rectangular"}}};
    for (const auto& [value, form, name] : looks) {
        expect(fixture.session.setParameterValue(*mode, value, QStringLiteral("Set Mode")) &&
                   fixture.session.setParameterValue(*shape, form, QStringLiteral("Set Shape")),
               "the Vignette mode and shape are set");
        QApplication::processEvents();
        expect(card->mode() == value, "the Vignette card follows its node's mode");
        if (!paintsInsideCard(*fixture.item)) {
            std::cerr << name << " paints outside its card\n";
            ++failures;
        }
        if (!cardContentFits(*fixture.item, name))
            ++failures;
        capture(fixture, QString::fromLatin1(name));
        const bool exposure = value == document::kVignetteExposure;
        expect(card->valueRow("amount")->isVisibleTo(card) == !exposure &&
                   card->valueRow("exposure")->isVisibleTo(card) == exposure &&
                   card->valueRow("feather")->isVisibleTo(card) &&
                   card->valueRow("highlightProtection")->isVisibleTo(card) &&
                   card->socketY("amount").has_value() == !exposure &&
                   card->socketY("exposure").has_value() == exposure &&
                   card->socketY("roundness").has_value() && card->showsRole("mode") &&
                   card->showsRole("shape") && card->showsRole("center") &&
                   !card->showsRole("midpoint") && !card->showsRole("amount"),
               "the Vignette card shows the strength its mode reads and every shared value");
        const auto& backdrop = card->padRow()->pad()->backdrop();
        expect(!backdrop.isNull() &&
                   qGray(backdrop.pixel(backdrop.width() / 2, backdrop.height() / 2)) >
                       qGray(backdrop.pixel(1, 1)) + 20,
               "the pad draws the falloff: the corners darker than the centre");
    }
    expect(fixture.session.setParameterValue(*mode, document::kVignetteMultiply,
                                             QStringLiteral("Set Mode")) &&
               fixture.session.setParameterValue(*shape, document::kVignetteElliptical,
                                                 QStringLiteral("Set Shape")),
           "the Vignette is an elliptical multiply again");
    QApplication::processEvents();
    // Dragging the centre is one command and moves the darkening with it.
    auto& pad = *card->padRow()->pad();
    const auto before = card->preview(64);
    const auto commands = fixture.stack.size();
    const QPointF from = pad.screenPoint(pad.value(0));
    const QPointF to = from + QPointF(pad.frameRect().width() * 0.25, 0.0);
    mouse(pad, QEvent::MouseButtonPress, from);
    mouse(pad, QEvent::MouseMove, to);
    mouse(pad, QEvent::MouseButtonRelease, to);
    QApplication::processEvents();
    const auto centre = ui::nodeRoleParameter(fixture.session, fixture.node, "center");
    const auto live = centre ? fixture.session.liveValue(*centre) : std::nullopt;
    const auto* moved = live ? std::get_if<document::Vec2d>(&*live) : nullptr;
    expect(fixture.stack.size() == commands + 1 && moved && moved->x > 100.0 &&
               std::abs(moved->y) < 1e-6,
           "dragging the centre moves it to the right as one command");
    const auto after = card->preview(64);
    const int probe = after.width() * 85 / 100;
    expect(qGray(after.pixel(probe, after.height() / 2)) >
               qGray(before.pixel(probe, before.height() / 2)),
           "the falloff follows the centre");
    capture(fixture, QStringLiteral("vignette-moved"));
    // A slider drag is one command.
    auto* slider = card->valueRow("feather")->slider();
    const auto sliderCommands = fixture.stack.size();
    const QPointF start = slider->rect().center();
    mouse(*slider, QEvent::MouseButtonPress, start);
    mouse(*slider, QEvent::MouseMove, start + QPointF(-slider->width() * 0.3, 0.0));
    mouse(*slider, QEvent::MouseButtonRelease, start + QPointF(-slider->width() * 0.3, 0.0));
    const auto feather = stored(fixture.session, fixture.node, "feather");
    expect(fixture.stack.size() == sliderCommands + 1 && feather && *feather < 0.5,
           "dragging the feather slider is one command");
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

// #546: the gradient's blend pickers on the card and in Properties. Choosing a colour mode, a hue
// direction or an interpolation is one command each and is stored on the gradient; HSV and HSL
// swap the Interpolation dropdown for the Hue one; the bar draws the chosen blend (Constant holds
// the first stop's colour up to the second); one undo restores the previous blend.
void gradientBlendModes() {
    CardFixture fixture(document::kLinearGradientNodeType);
    auto* card = fixture.item
                     ? qobject_cast<ui::NodeControlsCardWidget*>(fixture.item->cardWidget())
                     : nullptr;
    if (card == nullptr || card->gradientRows().size() != 1)
        return expect(false, "the Linear Gradient card draws its gradient row");
    const auto refreshRow = [&] {
        QApplication::processEvents();
        fixture.item = ui::node_editor::nodeItemAncestor(
            fixture.editor->graphScene()->findNodeItem(fixture.node));
        card = fixture.item ? qobject_cast<ui::NodeControlsCardWidget*>(fixture.item->cardWidget())
                            : nullptr;
        return card != nullptr && card->gradientRows().size() == 1 ? card->gradientRows().front()
                                                                   : nullptr;
    };
    // Three saturated stops, so each blend draws a visibly different bar.
    const std::vector<document::GradientStop> colourful{{0.0, core::Color4d{1.0, 0.1, 0.0, 1.0}},
                                                        {0.35, core::Color4d{0.1, 0.8, 0.1, 1.0}},
                                                        {1.0, core::Color4d{0.1, 0.2, 1.0, 1.0}}};
    {
        const auto gradient = ui::nodeRoleParameter(fixture.session, fixture.node, "gradient");
        commands::Transaction transaction("Stops", fixture.session.snapshot().revision());
        transaction.emplace<commands::SetParameterSource>(
            fixture.session.compositionId(), gradient.value_or(document::ParameterId{}),
            document::ConstantValueSource{document::GradientValue{colourful}});
        expect(gradient &&
                   fixture.session.executeNodeTransaction(std::move(transaction)).succeeded(),
               "the gradient takes three stops");
    }
    auto* row = refreshRow();
    if (row == nullptr)
        return expect(false, "the gradient row draws");
    expect(row->colorModeDropdown()->isVisible() && row->interpolationDropdown()->isVisible() &&
               !row->hueDropdown()->isVisible() &&
               row->colorModeDropdown()->currentText() == QStringLiteral("RGB") &&
               row->interpolationDropdown()->currentText() == QStringLiteral("Linear") &&
               row->interpolationDropdown()->count() == 6 &&
               row->colorModeDropdown()->count() == 3 && row->hueDropdown()->count() == 4,
           "a new gradient shows RGB and Linear, with every mode offered");
    expect(!row->editor()->hasColorSampler(), "Linear RGB draws the bar as a linear blend");
    expect(cardContentFits(*fixture.item, "linear-gradient blend"), "the blend rows fit the card");
    capture(fixture, QStringLiteral("gradient-blend-linear"));

    const auto choose = [&](ui::kit::KDropdown* box, const QString& item) {
        for (int index = 0; index < box->count(); ++index)
            if (box->itemText(index) == item) {
                box->chooseIndex(index);
                return true;
            }
        return false;
    };
    auto commands = fixture.stack.size();
    expect(choose(row->interpolationDropdown(), QStringLiteral("Constant")), "Constant is offered");
    auto stored = storedGradient(fixture.session, fixture.node);
    expect(stored && stored->modes.interpolation == document::GradientInterpolation::Constant &&
               stored->stops == colourful && fixture.stack.size() == commands + 1,
           "choosing Constant stores it on the gradient as one command");
    row = refreshRow();
    if (row == nullptr)
        return expect(false, "the gradient row draws after the edit");
    const auto held = row->editor()->colorAt(0.75);
    expect(row->editor()->hasColorSampler() && held.red == 0.1F && held.green == 0.8F &&
               held.blue == 0.1F,
           "the bar draws Constant: the green stop holds up to the blue one");
    capture(fixture, QStringLiteral("gradient-blend-constant"));
    for (const auto* name : {"Ease", "Cardinal", "B-Spline", "Exponential"}) {
        expect(choose(row->interpolationDropdown(), QString::fromLatin1(name)),
               (std::string(name) + " is offered").c_str());
        row = refreshRow();
        if (row == nullptr)
            return expect(false, "the gradient row draws after the edit");
        capture(fixture, QStringLiteral("gradient-blend-") + QString::fromLatin1(name).toLower());
    }
    stored = storedGradient(fixture.session, fixture.node);
    expect(stored && stored->modes.interpolation == document::GradientInterpolation::Exponential,
           "each interpolation is stored as chosen");

    commands = fixture.stack.size();
    expect(choose(row->colorModeDropdown(), QStringLiteral("HSV")), "HSV is offered");
    row = refreshRow();
    if (row == nullptr)
        return expect(false, "the gradient row draws after the edit");
    stored = storedGradient(fixture.session, fixture.node);
    expect(stored && stored->modes.colorMode == document::GradientColorMode::Hsv &&
               stored->modes.interpolation == document::GradientInterpolation::Exponential &&
               fixture.stack.size() == commands + 1,
           "choosing HSV is one command and keeps the interpolation for RGB");
    expect(row->hueDropdown()->isVisible() && !row->interpolationDropdown()->isVisible() &&
               row->hueDropdown()->currentText() == QStringLiteral("Near"),
           "HSV shows the Hue dropdown in the Interpolation one's place");
    capture(fixture, QStringLiteral("gradient-blend-hsv-near"));
    expect(choose(row->hueDropdown(), QStringLiteral("Counter-clockwise")),
           "Counter-clockwise is offered");
    stored = storedGradient(fixture.session, fixture.node);
    expect(stored && stored->modes.hueDirection == document::GradientHueDirection::CounterClockwise,
           "the hue direction is stored");
    row = refreshRow();
    if (row == nullptr)
        return expect(false, "the gradient row draws after the edit");
    capture(fixture, QStringLiteral("gradient-blend-hsv-counter-clockwise"));
    for (const auto* name : {"Far", "Clockwise"}) {
        expect(choose(row->hueDropdown(), QString::fromLatin1(name)),
               (std::string(name) + " is offered").c_str());
        row = refreshRow();
        if (row == nullptr)
            return expect(false, "the gradient row draws after the edit");
        capture(fixture,
                QStringLiteral("gradient-blend-hsv-") + QString::fromLatin1(name).toLower());
    }
    expect(choose(row->hueDropdown(), QStringLiteral("Counter-clockwise")),
           "Counter-clockwise is offered again");
    row = refreshRow();
    if (row == nullptr)
        return expect(false, "the gradient row draws after the edit");
    expect(choose(row->colorModeDropdown(), QStringLiteral("HSL")), "HSL is offered");
    row = refreshRow();
    if (row == nullptr)
        return expect(false, "the gradient row draws after the edit");
    capture(fixture, QStringLiteral("gradient-blend-hsl-counter-clockwise"));

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
    expect(inProperties && inProperties->colorModeDropdown()->currentText() == "HSL" &&
               inProperties->hueDropdown()->currentText() == "Counter-clockwise" &&
               inProperties->hueDropdown()->isVisibleTo(properties),
           "Properties shows the same blend");
    if (const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
        !directory.isEmpty())
        properties->grab().save(directory + QStringLiteral("/gradient-blend-properties.png"));

    expect(fixture.session.undo(), "undo runs");
    stored = storedGradient(fixture.session, fixture.node);
    expect(stored && stored->modes.colorMode == document::GradientColorMode::Hsv,
           "one undo restores the previous colour mode");
    row = refreshRow();
    expect(row != nullptr && row->reset() &&
               storedGradient(fixture.session, fixture.node) ==
                   document::defaultGeneratorGradient(),
           "resetting the gradient restores Linear in RGB");
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

// #483: the Z Defocus and Depth Fog cards in each mode paint inside their card with no label cut
// short and show only what the mode reads: the focus pad and blade dial when used, the range
// rows and the fog curve; "Pick focus in Viewer" arms the Viewer's focus pick; a depth read through
// a colour transform warns, and "Read as data" removes the transform as one command.
void depthEffectCards() {
    {
        CardFixture fixture(document::kZDefocusNodeType);
        auto* card = fixture.item
                         ? qobject_cast<ui::DepthEffectCardWidget*>(fixture.item->cardWidget())
                         : nullptr;
        expect(card != nullptr && card->defocus(), "Z Defocus draws its card widget");
        const auto role = [&](const char* name) {
            return ui::nodeRoleParameter(fixture.session, fixture.node, name);
        };
        if (card == nullptr || !role("mode") || !role("focusFrom") || !role("shape"))
            return;
        const auto check = [&](const char* name) {
            QApplication::processEvents();
            if (!paintsInsideCard(*fixture.item)) {
                std::cerr << name << " paints outside its card\n";
                ++failures;
            }
            if (!cardContentFits(*fixture.item, name))
                ++failures;
            capture(fixture, QString::fromLatin1(name));
        };
        check("z-defocus-depth");
        expect(card->showsRole("focusDistance") && !card->showsRole("focusPoint") &&
                   !card->showsRole("focalLength") && card->showsRole("maxSize") &&
                   !card->showsRole("blades") && !card->showsRole("near") &&
                   !card->showsRole("rotation") && card->showsRole("depthChannel") &&
                   card->pickFocusButton()->isVisibleTo(card) &&
                   !card->focusPad()->isVisibleTo(card) && !card->rotationDial()->isVisibleTo(card),
               "a Depth Z Defocus shows the focus distance and the pick, not the lens or blades");
        // Physical lens, focused from a point, through a polygon.
        expect(fixture.session.setParameterValue(*role("mode"), document::kDefocusModePhysical,
                                                 QStringLiteral("Mode")) &&
                   fixture.session.setParameterValue(*role("focusFrom"),
                                                     document::kFocusFromPoint,
                                                     QStringLiteral("Focus")) &&
                   fixture.session.setParameterValue(*role("shape"), document::kBokehPolygon,
                                                     QStringLiteral("Shape")),
               "the lens becomes a physical polygon focused from a point");
        check("z-defocus-physical");
        expect(card->showsRole("focalLength") && card->showsRole("fStop") &&
                   card->showsRole("sensorWidth") && card->showsRole("units") &&
                   !card->showsRole("size") && !card->showsRole("focusDistance") &&
                   card->showsRole("focusPoint") && card->showsRole("blades") &&
                   !card->showsRole("rotation") && card->focusPad()->isVisibleTo(card) &&
                   card->rotationDial()->isVisibleTo(card) && card->socketY("rotation"),
               "a physical polygon from a point shows the lens, the pad and the blade dial");
        // The pick button arms the Viewer's focus pick for this node.
        card->pickFocusButton()->click();
        expect(!card->pickFocusButton()->isEnabled() || fixture.session.focusPickNode() ==
                                                            std::optional(fixture.node),
               "Pick focus in Viewer arms the focus pick for the node");
        fixture.session.cancelFocusPick();
        expect(!fixture.session.focusPickNode(), "the focus pick cancels");
        expect(fixture.session.setParameterValue(*role("mode"), document::kDefocusModeUniform,
                                                 QStringLiteral("Mode")),
               "the lens becomes uniform");
        check("z-defocus-uniform");
        expect(!card->showsRole("interpretation") && !card->showsRole("depthChannel") &&
                   !card->showsRole("focusFrom") && card->showsRole("size") &&
                   !card->pickFocusButton()->isVisibleTo(card) &&
                   !card->focusPad()->isVisibleTo(card),
               "a uniform blur reads no depth: no depth, focus or pick rows");
    }
    {
        CardFixture fixture(document::kDepthFogNodeType);
        auto* card = fixture.item
                         ? qobject_cast<ui::DepthEffectCardWidget*>(fixture.item->cardWidget())
                         : nullptr;
        expect(card != nullptr && !card->defocus(), "Depth Fog draws its card widget");
        const auto falloff = ui::nodeRoleParameter(fixture.session, fixture.node, "falloff");
        if (card == nullptr || !falloff)
            return;
        QApplication::processEvents();
        if (!paintsInsideCard(*fixture.item) || !cardContentFits(*fixture.item, "depth-fog"))
            ++failures;
        capture(fixture, QStringLiteral("depth-fog-linear"));
        expect(card->fogRangeRow() != nullptr && card->nearFarRow() != nullptr &&
                   !card->showsRole("start") && !card->showsRole("end") &&
                   !card->showsRole("near") && !card->showsRole("density") &&
                   !card->curveGraph()->isVisibleTo(card) && card->showsRole("color"),
               "a linear mist fog draws its start/end and near/far ranges, no density or curve");
        expect(fixture.session.setParameterValue(*falloff, document::kFogFalloffCurve,
                                                 QStringLiteral("Falloff")),
               "the fog follows a curve");
        QApplication::processEvents();
        if (!paintsInsideCard(*fixture.item) || !cardContentFits(*fixture.item, "depth-fog-curve"))
            ++failures;
        capture(fixture, QStringLiteral("depth-fog-curve"));
        expect(card->curveGraph()->isVisibleTo(card) && card->curveGraph()->parameter() &&
                   card->fogRangeRow() != nullptr,
               "a curve fog draws its falloff curve");
        expect(fixture.session.setParameterValue(*falloff, document::kFogFalloffExponential,
                                                 QStringLiteral("Falloff")),
               "the fog is exponential");
        QApplication::processEvents();
        expect(card->fogRangeRow() == nullptr && card->showsRole("start") &&
                   !card->showsRole("end") && card->showsRole("density") &&
                   !card->curveGraph()->isVisibleTo(card),
               "an exponential fog reads its start and density, not its end");
        // A depth read from an image source through a colour transform warns, and the fix reads
        // the file as data in one command.
        const auto source = addNode(fixture.session, document::kImageSourceNodeType);
        {
            commands::Transaction transaction("Link", fixture.session.snapshot().revision());
            transaction.emplace<commands::ConnectPorts>(
                fixture.session.compositionId(), document::OutputPortRef{source, "image"},
                document::NodeInputRef{fixture.node, std::string(document::kDepthEffectDepthPort)});
            expect(fixture.session.executeNodeTransaction(std::move(transaction)).succeeded(),
                   "an image source feeds the depth");
        }
        QApplication::processEvents();
        expect(!card->colorWarning()->text().isEmpty() && card->colorWarning()->isVisibleTo(card) &&
                   card->readAsDataButton()->isVisibleTo(card),
               "a depth through a colour transform warns with a Read as data fix");
        capture(fixture, QStringLiteral("depth-fog-warning"));
        if (!paintsInsideCard(*fixture.item) ||
            !cardContentFits(*fixture.item, "depth-fog-warning"))
            ++failures;
        const auto commands = fixture.stack.size();
        card->readAsDataButton()->click();
        QApplication::processEvents();
        expect(fixture.stack.size() == commands + 1 &&
                   !document::depthInputColorTransform(fixture.session.snapshot().project(),
                                                       *fixture.session.composition(),
                                                       fixture.node) &&
                   !card->colorWarning()->isVisibleTo(card),
               "Read as data removes the transform as one command and the warning goes");
    }
}

// #484: every motion-blur card: the shutter angle on a dial, the shutter curve on a curve editor when
// it is Custom, direction and spin on dials, the centre on a pad, and only what the current values
// read.
void motionBlurCards() {
    const auto setInteger = [](ui::CompositionSession& session, const document::NodeId node,
                               const char* role, const std::int64_t value) {
        const auto id = ui::nodeRoleParameter(session, node, role);
        expect(id && session.setParameterValue(*id, value, QStringLiteral("Set")), role);
    };
    const auto check = [](CardFixture& fixture, const char* name) {
        QApplication::processEvents();
        if (!paintsInsideCard(*fixture.item)) {
            std::cerr << name << " paints outside its card\n";
            ++failures;
        }
        if (!cardContentFits(*fixture.item, name))
            ++failures;
        capture(fixture, QString::fromLatin1(name));
    };
    for (const auto& [type, name] :
         std::initializer_list<std::pair<std::string_view, const char*>>{
             {document::kMotionBlurNodeType, "motion-blur"},
             {document::kVectorMotionBlurNodeType, "vector-motion-blur"},
             {document::kOpticalFlowNodeType, "optical-flow"},
             {document::kOpticalFlowMotionBlurNodeType, "optical-flow-motion-blur"},
             {document::kEchoNodeType, "echo"},
             {document::kFrameRateHoldNodeType, "frame-rate-hold"},
             {document::kDirectionalBlurNodeType, "directional-blur"},
             {document::kSpinBlurNodeType, "spin-blur"},
             {document::kZoomBlurNodeType, "zoom-blur"}}) {
        CardFixture fixture(type);
        auto* card = fixture.item
                         ? qobject_cast<ui::MotionBlurCardWidget*>(fixture.item->cardWidget())
                         : nullptr;
        expect(card != nullptr, "a motion-blur node draws its card widget");
        if (card != nullptr)
            check(fixture, name);
    }
    {
        // Motion Blur: the shutter dial edits as one command, the cost follows the quality, the
        // curve editor and the phase only when they are read.
        CardFixture fixture(document::kMotionBlurNodeType);
        auto* card = qobject_cast<ui::MotionBlurCardWidget*>(fixture.item->cardWidget());
        if (card == nullptr)
            return;
        expect(card->shutterDial() != nullptr && card->shutterDial()->isVisibleTo(card),
               "Motion Blur's shutter is a dial");
        expect(!card->showsRole("shutterAngle"), "the dial replaces the shutter's number row");
        expect(!card->phaseDial()->isVisibleTo(card), "a Centred shutter has no phase");
        expect(!card->curveEditor()->isVisibleTo(card), "a Box shutter has no curve editor");
        const auto cost = card->costLabel()->text();
        expect(cost.contains(QStringLiteral("16")), "a Standard Motion Blur costs 16 upstream renders");
        setInteger(fixture.session, fixture.node, "quality", document::kMotionQualityDraft);
        QApplication::processEvents();
        expect(card->costLabel()->text().contains(QStringLiteral("4")),
               "a Draft Motion Blur costs 4 upstream renders");
        setInteger(fixture.session, fixture.node, "shutterOffset", document::kShutterOffsetCustom);
        setInteger(fixture.session, fixture.node, "shutterCurve", document::kShutterCurveCustom);
        QApplication::processEvents();
        expect(card->phaseDial()->isVisibleTo(card), "a Custom offset shows its phase on a dial");
        expect(card->curveEditor()->isVisibleTo(card), "a Custom shutter shows its curve editor");
        check(fixture, "motion-blur-custom-shutter");
        expect(!card->showsRole("minSamples"), "only Final reads the adaptive sample range");
        setInteger(fixture.session, fixture.node, "quality", document::kMotionQualityFinal);
        QApplication::processEvents();
        expect(card->showsRole("minSamples") && card->showsRole("maxSamples"),
               "Final shows the adaptive sample range");
        setInteger(fixture.session, fixture.node, "footage", document::kFootageHold);
        QApplication::processEvents();
        expect(!card->showsRole("vectorLayer"), "held footage reads no vector layer");
        // The dial itself: a drag is one command.
        auto* dial = card->shutterDial()->dial();
        const auto before = stored(fixture.session, fixture.node, "shutterAngle");
        const auto stack = fixture.stack.size();
        const QPointF middle = QRectF(dial->rect()).center();
        mouse(*dial, QEvent::MouseButtonPress, middle + QPointF(0.0, -8.0));
        mouse(*dial, QEvent::MouseMove, middle + QPointF(8.0, 0.0));
        mouse(*dial, QEvent::MouseMove, middle + QPointF(0.0, 8.0));
        mouse(*dial, QEvent::MouseButtonRelease, middle + QPointF(0.0, 8.0));
        QApplication::processEvents();
        const auto after = stored(fixture.session, fixture.node, "shutterAngle");
        expect(before && after && *before != *after, "dragging the dial sets the shutter");
        expect(fixture.stack.size() == stack + 1, "the dial's drag is one command");
    }
    {
        // Directional: the direction on a dial; no shutter angle (its length is the reach).
        CardFixture fixture(document::kDirectionalBlurNodeType);
        auto* card = qobject_cast<ui::MotionBlurCardWidget*>(fixture.item->cardWidget());
        if (card == nullptr)
            return;
        expect(card->directionDial() != nullptr && !card->showsRole("angle") &&
                   card->showsRole("length"),
               "Directional's direction is a dial, its length a row");
        expect(card->shutterDial() == nullptr, "Directional has no shutter dial");
    }
    {
        // Spin and Zoom: the centre on a point pad; Spin's turn on a dial.
        CardFixture spin(document::kSpinBlurNodeType);
        auto* card = qobject_cast<ui::MotionBlurCardWidget*>(spin.item->cardWidget());
        if (card == nullptr)
            return;
        expect(card->directionDial() != nullptr && card->centrePad() != nullptr &&
                   !card->showsRole("amount"),
               "Spin's turn is a dial and its centre a pad");
        CardFixture zoom(document::kZoomBlurNodeType);
        auto* zoomCard = qobject_cast<ui::MotionBlurCardWidget*>(zoom.item->cardWidget());
        expect(zoomCard != nullptr && zoomCard->centrePad() != nullptr &&
                   zoomCard->directionDial() == nullptr && zoomCard->showsRole("amount"),
               "Zoom's centre is a pad, its amount a row");
    }
    {
        // Vector Motion Blur: the convention's own rows appear with it.
        CardFixture fixture(document::kVectorMotionBlurNodeType);
        auto* card = qobject_cast<ui::MotionBlurCardWidget*>(fixture.item->cardWidget());
        if (card == nullptr)
            return;
        expect(!card->showsRole("vectorScale") && !card->showsRole("maxDisplace") &&
                   !card->showsRole("depthChannel"),
               "a preset convention hides its scale, MaxDisplace and an unconnected depth's rows");
        setInteger(fixture.session, fixture.node, "convention", document::kVectorConventionRsmb);
        QApplication::processEvents();
        expect(card->showsRole("maxDisplace") && !card->showsRole("vectorScale"),
               "RSMB reads MaxDisplace");
        setInteger(fixture.session, fixture.node, "convention", document::kVectorConventionCustom);
        QApplication::processEvents();
        expect(card->showsRole("vectorScale") && card->showsRole("flipY") &&
                   card->showsRole("layout") && !card->showsRole("maxDisplace"),
               "Custom shows the whole decoding");
        check(fixture, "vector-motion-blur-custom");
    }
    {
        // Echo: Frame Blend reads no decay; a posterized rate only with Posterize Time.
        CardFixture fixture(document::kEchoNodeType);
        auto* card = qobject_cast<ui::MotionBlurCardWidget*>(fixture.item->cardWidget());
        if (card == nullptr)
            return;
        expect(card->showsRole("decay") && !card->showsRole("posterizeRate"),
               "Echo reads its decay, not the rate");
        setInteger(fixture.session, fixture.node, "mode", document::kEchoModeFrameBlend);
        QApplication::processEvents();
        expect(!card->showsRole("decay") && !card->showsRole("operator"),
               "Frame Blend has no decay or operator");
    }
    {
        // Frame Rate Hold: the amount and the seed are read only with the irregular cadence on.
        CardFixture fixture(document::kFrameRateHoldNodeType);
        auto* card = qobject_cast<ui::MotionBlurCardWidget*>(fixture.item->cardWidget());
        if (card == nullptr)
            return;
        expect(card->showsRole("rate") && card->showsRole("irregular") &&
                   !card->showsRole("amount") && !card->showsRole("seed"),
               "a regular Frame Rate Hold reads its rate only");
        check(fixture, "frame-rate-hold-regular");
        const auto id = ui::nodeRoleParameter(fixture.session, fixture.node, "irregular");
        expect(id && fixture.session.setParameterValue(*id, true, QStringLiteral("Set")),
               "Frame Rate Hold's irregular cadence turns on");
        QApplication::processEvents();
        expect(card->showsRole("amount") && card->showsRole("seed"),
               "an irregular cadence reads its amount and its seed");
        check(fixture, "frame-rate-hold-irregular");
    }
    {
        // Transform 2D: the shutter's rows, dial and curve only with Motion blur on.
        CardFixture fixture(document::kTransform2DNodeType);
        const auto read = [&](const char* role) {
            return ui::motionRoleRead(fixture.session, fixture.node, document::kTransform2DNodeType,
                                      role);
        };
        expect(!read("shutterAngle") && !read("quality") && !read("jitter"),
               "Transform 2D hides its shutter until motion blur is on");
        check(fixture, "transform-2d");
        const auto id = ui::nodeRoleParameter(fixture.session, fixture.node, "motionBlur");
        expect(id && fixture.session.setParameterValue(*id, true, QStringLiteral("Set")),
               "Transform 2D's motion blur turns on");
        QApplication::processEvents();
        expect(read("shutterAngle") && read("quality") && read("jitter"),
               "Motion blur on shows the shutter");
        check(fixture, "transform-2d-motion-blur");
    }
}


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
    vignetteCard();
    lensFlareCard();
    filmGrainCard();
    sharpenCard();
    codecDegradeCard();
    lensDistortionCard();
    chromaticAberrationCard();
    testCardCard();
    lightRaysCard();
    cameraShakeCard();
    depthEffectCards();
    motionBlurCards();
    nodeControlCards();
    linkedGradientStop();
    gradientBlendModes();
    if (failures != 0)
        return 1;
    std::cout << "PASS: grading cards\n";
    return 0;
}
