// #644: a whole Path or Gradient value is keyed from its node card with the same diamond and the
// same command as Properties' Value row. The proof is gestures (clicks on the card's own diamond)
// and consequences (key counts, undo, Properties in step, and the rendered outline per key).
#include "node_production_harness.hpp"

#include <bloom/commands/node_layout_placement.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/ui/properties_editor.hpp>

#include <QGraphicsProxyWidget>

using namespace bloom;
using namespace bloom::ui;
using namespace bloom::ui::production_test;

namespace {
[[nodiscard]] core::RationalTime frame(const std::int64_t index) {
    const auto time = core::RationalTime::create(index, 24);
    if (!time.has_value())
        throw std::runtime_error("frame time");
    return *time;
}

[[nodiscard]] document::PathValue square(const double half) {
    document::PathContour contour;
    contour.closed = true;
    for (const auto& point : {document::Vec2d{-half, -half}, document::Vec2d{half, -half},
                              document::Vec2d{half, half}, document::Vec2d{-half, half}})
        contour.anchors.push_back(
            {point, std::nullopt, std::nullopt, document::PathAnchorMode::Corner});
    return document::PathValue::fromContour(contour);
}

[[nodiscard]] std::vector<QWidget*> cardWidgets(App& app, const document::NodeId id) {
    std::vector<QWidget*> widgets;
    if (auto* card = app.card(id))
        for (auto* child : card->childItems())
            if (auto* proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(child))
                if (proxy->widget() != nullptr)
                    widgets.push_back(proxy->widget());
    return widgets;
}

// The card's diamond bound to exactly `parameter`, or null when the card has none for it.
[[nodiscard]] KeyframeDiamond* cardDiamond(App& app, const document::NodeId id,
                                           const document::ParameterId parameter) {
    for (auto* widget : cardWidgets(app, id))
        for (auto* diamond : widget->findChildren<KeyframeDiamond*>())
            if (diamond->parameterId() == parameter)
                return diamond;
    return nullptr;
}

[[nodiscard]] QString cardReadout(App& app, const document::NodeId id) {
    for (auto* widget : cardWidgets(app, id))
        if (auto* label = widget->findChild<kit::KLabel*>(QStringLiteral("nodeReadOnlyValue")))
            return label->text();
    return {};
}

[[nodiscard]] document::ParameterId parameterIdOf(App& app, const document::NodeId node,
                                                  const std::string_view role) {
    const auto* parameter = parameterFor(app, node, role);
    return parameter == nullptr ? document::ParameterId{} : parameter->id;
}

[[nodiscard]] std::size_t keysOf(App& app, const document::ParameterId parameter) {
    const auto* record = app.session.composition()->parameters().find(parameter);
    const auto* animated =
        record == nullptr ? nullptr
                          : std::get_if<document::AnimationCurveSource>(&record->source);
    if (animated == nullptr)
        return 0;
    const auto& curves = app.session.composition()->animationCurves();
    if (const auto* path = curves.findPath(animated->curveId))
        return path->keyframes.size();
    if (const auto* gradient = curves.findGradient(animated->curveId))
        return gradient->keyframes.size();
    return 0;
}

void settle() { QCoreApplication::processEvents(); }

void run(App& app) {
    // A small composition keeps the CPU reference frames at 512 px.
    commands::Transaction format("Format", app.session.snapshot().revision());
    const auto size = document::CompositionFormat::create(512, 512);
    expect(size.has_value(), "a 512 px format exists");
    if (!size)
        return;
    format.emplace<commands::SetCompositionFormat>(app.session.compositionId(), *size);
    expect(app.stack.execute(std::move(format)).succeeded(), "the composition is 512 px");

    const auto pathNode = app.addByType(document::kPathValueNodeType, {0, 0});
    const auto valueId = parameterIdOf(app, pathNode, document::kValueParameterRole);
    expect(valueId.isValid(), "the Path node has a value parameter");
    if (!valueId.isValid())
        return;
    expect(app.session.setParameterValue(valueId, square(60.0), "Path"), "the Path holds a square");

    commands::ShapeLayerGeometry geometry;
    geometry.path = square(20.0);
    commands::ShapeLayerStyle style;
    style.fillColor = core::Color4d{0.9, 0.5, 0.1, 1.0};
    const auto addShape = [&] {
        commands::Transaction t("Shape", app.session.snapshot().revision());
        t.emplace<commands::AddShapeLayer>(app.session.compositionId(), document::ShapeKind::Path,
                                           geometry, style);
        return app.session.executeNodeTransaction(std::move(t))
            .outputId<document::NodeId>("shapeNode");
    };
    const auto drivenShape = addShape();
    const auto ownShape = addShape();
    expect(drivenShape && ownShape, "two Shape Sources are added");
    if (!drivenShape || !ownShape)
        return;
    {
        commands::Transaction t("Wire", app.session.snapshot().revision());
        t.emplace<commands::ConnectPorts>(app.session.compositionId(),
                                          document::OutputPortRef{pathNode, "value"},
                                          document::NodeInputRef{*drivenShape, "path"});
        expect(app.session.executeNodeTransaction(std::move(t)).succeeded(),
               "the Path node drives a Shape Source");
    }
    // Hide the second shape's contribution so only the driven one is measured.
    settle();

    const auto gradientNode = app.addByType(document::kLinearGradientNodeType, {0, 320});
    const auto gradientId = parameterIdOf(app, gradientNode, "gradient");
    settle();

    // Diamond presence by row.
    auto* cardDiamondPath = cardDiamond(app, pathNode, valueId);
    expect(cardDiamondPath != nullptr, "the Path node card carries a keyframe diamond");
    const auto ownPath = parameterIdOf(app, *ownShape, "path");
    auto* ownDiamond = cardDiamond(app, *ownShape, ownPath);
    // A Shape Source's own path is not a keyable parameter (only a Path node's value is), so its
    // row shows no diamond; the artist keys the Path node that drives it.
    expect(ownDiamond == nullptr || (ownDiamond->state() == KeyframeDiamondState::Unsupported &&
                                     ownDiamond->isHidden()),
           "a Shape Source's own constant path row shows no diamond because it cannot be keyed");
    const auto drivenPath = parameterIdOf(app, *drivenShape, "path");
    auto* drivenDiamond = cardDiamond(app, *drivenShape, drivenPath);
    expect(drivenDiamond == nullptr || (drivenDiamond->state() == KeyframeDiamondState::Unsupported &&
                                        drivenDiamond->isHidden()),
           "a driven path row shows no diamond");
    auto* gradientDiamond = cardDiamond(app, gradientNode, gradientId);
    expect(gradientId.isValid() && gradientDiamond != nullptr &&
               gradientDiamond->state() == KeyframeDiamondState::Constant &&
               !gradientDiamond->isHidden(),
           "a Linear Gradient card's gradient row carries a Constant diamond");

    const auto radialNode = app.addByType(document::kRadialGradientNodeType, {0, 640});
    const auto radialId = parameterIdOf(app, radialNode, "gradient");
    settle();
    auto* radialDiamond = cardDiamond(app, radialNode, radialId);
    expect(radialId.isValid() && radialDiamond != nullptr &&
               radialDiamond->state() == KeyframeDiamondState::Constant &&
               !radialDiamond->isHidden(),
           "a Radial Gradient card's gradient row carries a Constant diamond");

    // The card fits the placement estimate with the diamonds.
    for (const auto id : {pathNode, *drivenShape, *ownShape, gradientNode, radialNode}) {
        auto* card = app.card(id);
        const auto estimate = commands::estimatedNodeCardSize(*app.session.composition(), id);
        expect(card != nullptr && card->cardRect().width() <= estimate.width &&
                   card->cardRect().height() <= estimate.height,
               "a card with its diamond fits the placement estimate");
    }

    // Properties' Value row for the same node.
    PropertiesEditor properties(app.session);
    properties.resize(700, 900);
    properties.show();
    app.session.selectNode(pathNode);
    settle();
    const auto propertiesDiamond = [&]() -> KeyframeDiamond* {
        for (auto* diamond : properties.findChildren<KeyframeDiamond*>())
            if (diamond->parameterId() == valueId)
                return diamond;
        return nullptr;
    };
    auto* inProperties = propertiesDiamond();
    expect(inProperties != nullptr, "Properties shows the Path Value diamond");
    const auto inStep = [&](const KeyframeDiamondState expected, const char* message) {
        settle();
        auto* card = cardDiamond(app, pathNode, valueId);
        auto* panel = propertiesDiamond();
        if (card == nullptr || panel == nullptr || card->state() != expected ||
            panel->state() != expected)
            std::cerr << "  card " << (card ? static_cast<int>(card->state()) : -1) << " panel "
                      << (panel ? static_cast<int>(panel->state()) : -1) << " expected "
                      << static_cast<int>(expected) << '\n';
        expect(card != nullptr && panel != nullptr && card->state() == expected &&
                   panel->state() == expected,
               message);
    };

    // 1. Create, remove, re-add at frame 0; one history entry per click; one undo per click.
    (void)app.session.setCurrentTime(frame(0));
    inStep(KeyframeDiamondState::Constant, "constant before any key; card and Properties agree");
    auto history = app.stack.historyPosition();
    QTest::mouseClick(cardDiamond(app, pathNode, valueId), Qt::LeftButton);
    inStep(KeyframeDiamondState::AnimatedWithKey, "the card click keys the whole path");
    expect(keysOf(app, valueId) == 1 && app.stack.historyPosition() == history + 1,
           "one click is one key and one history entry");
    expect(app.session.undo(), "the card key undoes");
    inStep(KeyframeDiamondState::Constant, "one undo removes the card key");
    expect(keysOf(app, valueId) == 0 && app.stack.historyPosition() == history,
           "undo restores the constant path");
    QTest::mouseClick(cardDiamond(app, pathNode, valueId), Qt::LeftButton);
    inStep(KeyframeDiamondState::AnimatedWithKey, "the key is added again");
    QTest::mouseClick(cardDiamond(app, pathNode, valueId), Qt::LeftButton);
    inStep(KeyframeDiamondState::Constant, "a click on the key removes it");
    QTest::mouseClick(cardDiamond(app, pathNode, valueId), Qt::LeftButton);
    inStep(KeyframeDiamondState::AnimatedWithKey, "a click re-adds the key");
    expect(keysOf(app, valueId) == 1, "one key at frame 0");

    // 2. Between keys: the diamond follows the playhead with no reselect, and a click adds a key.
    expect(app.session.setCurrentTime(frame(12)), "the playhead moves to frame 12");
    inStep(KeyframeDiamondState::AnimatedWithoutKey, "off a key the diamond reads between keys");
    history = app.stack.historyPosition();
    QTest::mouseClick(cardDiamond(app, pathNode, valueId), Qt::LeftButton);
    inStep(KeyframeDiamondState::AnimatedWithKey, "a click between keys adds one");
    expect(keysOf(app, valueId) == 2 && app.stack.historyPosition() == history + 1,
           "two keys after the second click, one history entry");
    expect(app.session.undo(), "the second key undoes");
    inStep(KeyframeDiamondState::AnimatedWithoutKey, "undo returns to between keys");
    expect(keysOf(app, valueId) == 1, "one key after undo");
    QTest::mouseClick(cardDiamond(app, pathNode, valueId), Qt::LeftButton);
    expect(keysOf(app, valueId) == 2, "the second key is back");

    // 3. The rendered path changes across keys: edit the frame-12 key to a larger square.
    expect(app.session.setParameterValue(valueId, square(140.0), "Path"),
           "the frame-12 key takes a larger square");
    expect(keysOf(app, valueId) == 2, "editing a key does not add one");
    const auto first = composite(app, frame(0));
    const auto middle = composite(app, frame(6));
    const auto last = composite(app, frame(12));
    expect(first.ok && middle.ok && last.ok, "three keyed frames render");
    expect(first.alphaSum > 0.0 && first.alphaSum < middle.alphaSum &&
               middle.alphaSum < last.alphaSum,
           "the rendered outline grows from the first key through the middle to the last key");

    // 4. The card reads the path at the playhead, not "Animated".
    expect(app.session.setCurrentTime(frame(6)), "the playhead moves between the keys");
    settle();
    inStep(KeyframeDiamondState::AnimatedWithoutKey, "between keys on the way back");
    expect(cardReadout(app, pathNode) == QStringLiteral("4 anchors"),
           "the animated card reads the anchor count at the playhead");

    // 5. Remove a key from the card with two keys: the animation stays.
    expect(app.session.setCurrentTime(frame(12)), "back on the second key");
    inStep(KeyframeDiamondState::AnimatedWithKey, "on the second key");
    QTest::mouseClick(cardDiamond(app, pathNode, valueId), Qt::LeftButton);
    inStep(KeyframeDiamondState::AnimatedWithoutKey, "removing one of two keys keeps it animated");
    expect(keysOf(app, valueId) == 1, "one key left");
    expect(app.session.undo(), "the removal undoes");
    expect(keysOf(app, valueId) == 2, "undo restores both keys");

    // 6. A Linear Gradient's gradient keys from its card too.
    QTest::mouseClick(cardDiamond(app, gradientNode, gradientId), Qt::LeftButton);
    settle();
    expect(keysOf(app, gradientId) == 1 &&
               cardDiamond(app, gradientNode, gradientId)->state() ==
                   KeyframeDiamondState::AnimatedWithKey,
           "a Linear Gradient's gradient keys from its card");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        App fixture;
        run(fixture);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
