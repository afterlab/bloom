#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGraphicsItem>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStringList>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <array>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/audio_effect_nodes.hpp>
#include <bloom/document/auto_exposure_nodes.hpp>
#include <bloom/document/color_cleanup_nodes.hpp>
#include <bloom/document/compositing_recipe_nodes.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/image_math_parameters.hpp>
#include <bloom/document/layer_masks.hpp>
#include <bloom/document/motion_blur_parameters.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/node_group_nodes.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/resample_path_nodes.hpp>
#include <bloom/document/shape.hpp>
#include <bloom/document/test_card_nodes.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/value_utility_nodes.hpp>
#include <bloom/document/warp_nodes.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/curve_graph.hpp>
#include <bloom/ui/grading_controls.hpp>
#include <bloom/ui/kit/angle_dial.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tab_bar.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/test_card_widget.hpp>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <string_view>
#include <vector>
using namespace bloom;
namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}
QWidget* row(QWidget& panel, const char* role) {
    for (auto* candidate : panel.findChildren<QWidget*>("propertiesRegistryRow"))
        if (candidate->property("role").toString() == role)
            return candidate;
    return nullptr;
}
document::NodeId addNode(ui::CompositionSession& session, std::string_view type) {
    commands::Transaction transaction("Add", session.snapshot().revision());
    transaction.emplace<commands::AddNode>(session.compositionId(), std::string(type),
                                           document::Vec2d{});
    const auto result = session.executeNodeTransaction(std::move(transaction));
    return result.outputId<document::NodeId>(commands::kAddNodeOutput).value_or(document::NodeId{});
}
void connectNodes(ui::CompositionSession& session, document::NodeId source, document::NodeId target,
                  const char* port) {
    commands::Transaction transaction("Connect", session.snapshot().revision());
    const auto* node = session.composition()->graph().findNode(source);
    const auto* definition =
        document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
    transaction.emplace<commands::ConnectPorts>(
        session.compositionId(), document::OutputPortRef{source, definition->outputs.front().name},
        document::NodeInputRef{target, port});
    expect(session.executeNodeTransaction(std::move(transaction)).changed(),
           "connect upstream fixture");
}
void discreteImageRows() {
    auto project =
        document::makeNewProject("Discrete", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    ui::PropertiesEditor properties(session);
    properties.show();
    const auto flip = addNode(session, "bloom.flip");
    expect(flip.isValid(), "Flip is discoverable through generic AddNode");
    session.selectNode(flip);
    QCoreApplication::processEvents();
    auto* horizontal = row(properties, "horizontal");
    auto* vertical = row(properties, "vertical");
    auto* toggle = horizontal ? horizontal->findChild<ui::kit::KSwitch*>() : nullptr;
    expect(toggle && vertical && vertical->findChild<ui::kit::KSwitch*>(),
           "Flip presents both axis controls");
    if (toggle) {
        toggle->setChecked(false);
        expect(session.undo(), "Flip axis edit is command-backed and undoable");
        expect(session.redo(), "Flip axis edit redoes");
    }
    const auto rotate = addNode(session, "bloom.rotate-90");
    expect(rotate.isValid(), "Rotate 90 is discoverable through generic AddNode");
    session.selectNode(rotate);
    QCoreApplication::processEvents();
    auto* turns = row(properties, "quarterTurns");
    auto* field =
        turns ? turns->findChild<ui::kit::KValueField*>("propertiesRegistryInteger") : nullptr;
    expect(field != nullptr && field->isIntegerMode(),
           "Rotate 90 presents its quarter turns as an integer value field");
    if (field) {
        Q_EMIT field->editStarted();
        field->setIntegerValue(-3);
        Q_EMIT field->editFinished();
        const auto* node = session.composition()->graph().findNode(rotate);
        const auto parameter = node->parameters.front().parameterId;
        const auto* record = session.composition()->parameters().find(parameter);
        expect(std::get<std::int64_t>(
                   std::get<document::ConstantValueSource>(record->source).value) == -3,
               "Rotate 90 publishes signed quarter turns through a command");
        expect(session.undo(), "Rotate 90 parameter edit undoes");
        expect(session.redo(), "Rotate 90 parameter edit redoes");
    }
}

// #336: Transform 2D is discoverable through AddNode, shows every registry control in Properties,
// and its Extent selector edits through command history with undo/redo.
void transform2DRows() {
    auto project =
        document::makeNewProject("Transform", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    ui::PropertiesEditor properties(session);
    properties.show();
    const auto node = addNode(session, "bloom.transform-2d");
    expect(node.isValid(), "Transform 2D is discoverable through generic AddNode");
    session.selectNode(node);
    QCoreApplication::processEvents();
    for (const auto* role : {"translate", "pivot", "scale", "rotation", "extent", "filter"})
        expect(row(properties, role) != nullptr, "Transform 2D presents every registry control");
    // Scale is a factor shown as a percentage: the default 1 reads 100 %, and typing 50 % in X
    // stores 0.5 through the parameter's component.
    if (auto* scale = row(properties, "scale")) {
        const auto fields = scale->findChildren<ui::kit::KValueField*>("propertiesRegistryValue");
        expect(fields.size() == 2 && fields[0]->value() == 100.0 && fields[1]->value() == 100.0 &&
                   fields[0]->unit() == QStringLiteral("%") &&
                   fields[0]->paintedText().startsWith(QStringLiteral("100")),
               "Transform 2D's default scale reads 100 %, not 1.00 %");
        if (fields.size() == 2) {
            fields[0]->setValue(50.0);
            QCoreApplication::processEvents();
            const auto stored = [&]() -> std::optional<document::Vec2d> {
                for (const auto& binding :
                     session.composition()->graph().findNode(node)->parameters)
                    if (binding.role == "scale")
                        return session.effectiveVec2Value(binding.parameterId);
                return std::nullopt;
            }();
            expect(stored && stored->x == 0.5 && stored->y == 1.0,
                   "typing 50 % in Scale X stores 0.5 and leaves Y at 1");
            expect(fields[0]->value() == 50.0 && fields[1]->value() == 100.0,
                   "the scale fields read the stored factor back as 50 % and 100 %");
            expect(session.undo(), "the scale edit undoes");
            QCoreApplication::processEvents();
            expect(fields[0]->value() == 100.0, "after undo Scale X reads 100 % again");
        }
    } else {
        expect(false, "Transform 2D has a scale row");
    }
    auto* extent = row(properties, "extent");
    auto* selector = extent ? extent->findChild<ui::kit::KDropdown*>("propertiesRegistryEnum")
                            : nullptr;
    expect(selector && selector->count() == 3 &&
               selector->currentText() == QStringLiteral("Transformed"),
           "Extent is a three-entry selector defaulting to Transformed");
    auto* filter = row(properties, "filter");
    auto* filters = filter ? filter->findChild<ui::kit::KDropdown*>("propertiesRegistryEnum")
                           : nullptr;
    expect(filters && filters->count() == 2 && filters->currentText() == QStringLiteral("Bilinear") &&
               filters->itemText(1) == QStringLiteral("Nearest"),
           "Filter offers Bilinear (the default) and Nearest (#480)");
    if (selector) {
        selector->chooseIndex(2);
        const auto* record = [&]() -> const document::ParameterRecord* {
            for (const auto& binding : session.composition()->graph().findNode(node)->parameters)
                if (binding.role == "extent")
                    return session.composition()->parameters().find(binding.parameterId);
            return nullptr;
        }();
        expect(record && std::get<std::int64_t>(
                             std::get<document::ConstantValueSource>(record->source).value) == 2,
               "choosing Display publishes the stored extent through a command");
        expect(session.undo(), "Transform 2D extent edit undoes");
        expect(session.redo(), "Transform 2D extent edit redoes");
    }
}

// #337: Matte Blur is discoverable through AddNode, presents the blur radius and edge controls in
// Properties, and its Edges selector edits through command history with undo/redo.
void matteBlurRows() {
    auto project =
        document::makeNewProject("Matte Blur", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    ui::PropertiesEditor properties(session);
    properties.show();
    const auto node = addNode(session, "bloom.matte-blur");
    expect(node.isValid(), "Matte Blur is discoverable through generic AddNode");
    session.selectNode(node);
    QCoreApplication::processEvents();
    expect(row(properties, "radius") != nullptr, "Matte Blur presents its radius control");
    auto* edge = row(properties, "edge");
    auto* selector =
        edge ? edge->findChild<ui::kit::KDropdown*>("propertiesRegistryEnum") : nullptr;
    expect(selector && selector->count() == 2 &&
               selector->currentText() == QStringLiteral("Transparent"),
           "Edges is a two-entry selector defaulting to Transparent");
    if (selector) {
        selector->chooseIndex(1);
        const auto* record = [&]() -> const document::ParameterRecord* {
            for (const auto& binding : session.composition()->graph().findNode(node)->parameters)
                if (binding.role == "edge")
                    return session.composition()->parameters().find(binding.parameterId);
            return nullptr;
        }();
        expect(record && std::get<std::int64_t>(
                             std::get<document::ConstantValueSource>(record->source).value) == 1,
               "choosing Clamp publishes the stored edge through a command");
        expect(session.undo(), "Matte Blur edge edit undoes");
        expect(session.redo(), "Matte Blur edge edit redoes");
    }
}

// #479: Combine Channels presents a five-entry source dropdown per input (red, green, blue and red
// by default) and a two-entry alpha mode (Premultiplied); a source edits through command history.
void imageChannelRows() {
    auto project =
        document::makeNewProject("Channels", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    ui::PropertiesEditor properties(session);
    properties.show();
    expect(addNode(session, "bloom.separate-channels").isValid(),
           "Separate Channels is discoverable through generic AddNode");
    const auto node = addNode(session, "bloom.combine-channels");
    expect(node.isValid(), "Combine Channels is discoverable through generic AddNode");
    session.selectNode(node);
    QCoreApplication::processEvents();
    const auto dropdown = [&](const char* role) {
        auto* line = row(properties, role);
        return line ? line->findChild<ui::kit::KDropdown*>("propertiesRegistryEnum") : nullptr;
    };
    const std::array<std::pair<const char*, QString>, 4> sources{
        std::pair{"redSource", QStringLiteral("Red")},
        std::pair{"greenSource", QStringLiteral("Green")},
        std::pair{"blueSource", QStringLiteral("Blue")},
        std::pair{"alphaSource", QStringLiteral("Red")}};
    for (const auto& [role, current] : sources) {
        auto* selector = dropdown(role);
        expect(selector && selector->count() == 5 && selector->currentText() == current,
               "each input's source is a five-entry dropdown with its channel default");
    }
    auto* mode = dropdown("alphaMode");
    expect(mode && mode->count() == 2 && mode->currentText() == QStringLiteral("Premultiplied"),
           "the alpha mode is a two-entry dropdown defaulting to Premultiplied");
    if (auto* alpha = dropdown("alphaSource")) {
        alpha->chooseIndex(4);
        const auto* record = [&]() -> const document::ParameterRecord* {
            for (const auto& binding : session.composition()->graph().findNode(node)->parameters)
                if (binding.role == "alphaSource")
                    return session.composition()->parameters().find(binding.parameterId);
            return nullptr;
        }();
        expect(record && std::get<std::int64_t>(
                             std::get<document::ConstantValueSource>(record->source).value) == 4,
               "choosing Luma publishes the stored source through a command");
        expect(session.undo(), "Combine Channels source edit undoes");
        expect(session.redo(), "Combine Channels source edit redoes");
    }
}

// #340: Levels presents every channel's black/white/gamma rows and a clamp switch (off), Curves
// (#493) its curve graphs and a clamp switch, Hue/Saturation its hue and saturation; the clamp edits
// through command history.
void colorGradeRows() {
    auto project = document::makeNewProject("Grades", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    ui::PropertiesEditor properties(session);
    properties.show();
    for (const auto* type : {"bloom.levels", "bloom.curves"}) {
        const auto node = addNode(session, type);
        expect(node.isValid(), "a grade is discoverable through generic AddNode");
        session.selectNode(node);
        QCoreApplication::processEvents();
        const bool levels = std::string_view(type) == "bloom.levels";
        if (levels) {
            for (const auto* role :
                 {"masterInputBlack", "redGamma", "greenOutputWhite", "alphaInputWhite"})
                expect(row(properties, role) != nullptr,
                       "a grade presents each channel's controls");
            auto* gammaRow = row(properties, "masterGamma");
            expect(gammaRow &&
                       gammaRow->property("rowLabel").toString() == QStringLiteral("Master Gamma"),
                   "grade rows read as channel and control");
        } else {
            // #498: Curves presents the card's curve graph, a tab per channel and one for the hue
            // curves, not rows.
            auto* graph = properties.findChild<ui::CurvesCardWidget*>("propertiesCurvesGraph");
            expect(graph && graph->tabs()->count() == 6 &&
                       graph->curve() == document::GradeCurve::Master &&
                       graph->hueCurves()->count() == 4,
                   "Curves presents the curve graph with a tab per channel and the hue curves");
            for (const auto* role : {"master", "red", "alpha", "hueVsHue", "lumaVsSaturation"})
                expect(row(properties, role) == nullptr, "a curve is a graph tab, not a row");
        }
        auto* clampRow = row(properties, "clamp");
        auto* toggle = clampRow ? clampRow->findChild<ui::kit::KSwitch*>() : nullptr;
        expect(toggle != nullptr && !toggle->isChecked(), "a grade presents an unset clamp switch");
        if (toggle) {
            toggle->setChecked(true);
            const auto* record = [&]() -> const document::ParameterRecord* {
                for (const auto& binding :
                     session.composition()->graph().findNode(node)->parameters)
                    if (binding.role == "clamp")
                        return session.composition()->parameters().find(binding.parameterId);
                return nullptr;
            }();
            expect(record && std::get<bool>(
                                 std::get<document::ConstantValueSource>(record->source).value),
                   "setting the clamp publishes the stored switch through a command");
            expect(session.undo(), "grade clamp edit undoes");
            expect(session.redo(), "grade clamp edit redoes");
        }
    }
    const auto hue = addNode(session, "bloom.hue-saturation");
    expect(hue.isValid(), "Hue/Saturation is discoverable through generic AddNode");
    session.selectNode(hue);
    QCoreApplication::processEvents();
    expect(row(properties, "hue") != nullptr && row(properties, "saturation") != nullptr,
           "Hue/Saturation presents its hue and saturation");
}

// #338: Matte Erode and Matte Dilate present the Matte Blur controls; Luma Key presents its low
// and high thresholds and an invert switch, and edits them through command history.
void matteKeyRows() {
    auto project =
        document::makeNewProject("Matte Key", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    ui::PropertiesEditor properties(session);
    properties.show();
    for (const auto* type : {"bloom.matte-erode", "bloom.matte-dilate"}) {
        const auto node = addNode(session, type);
        expect(node.isValid(), "Matte Erode/Dilate is discoverable through generic AddNode");
        session.selectNode(node);
        QCoreApplication::processEvents();
        auto* edge = row(properties, "edge");
        auto* selector =
            edge ? edge->findChild<ui::kit::KDropdown*>("propertiesRegistryEnum") : nullptr;
        expect(row(properties, "radius") != nullptr && selector && selector->count() == 2 &&
                   selector->currentText() == QStringLiteral("Transparent"),
               "Matte Erode/Dilate presents radius and a Transparent/Clamp Edges selector");
    }
    const auto key = addNode(session, "bloom.luma-key");
    expect(key.isValid(), "Luma Key is discoverable through generic AddNode");
    session.selectNode(key);
    QCoreApplication::processEvents();
    expect(row(properties, "low") != nullptr && row(properties, "high") != nullptr,
           "Luma Key presents its low and high thresholds");
    auto* invert = row(properties, "invert");
    auto* toggle = invert ? invert->findChild<ui::kit::KSwitch*>() : nullptr;
    expect(toggle != nullptr && !toggle->isChecked(), "Luma Key presents an unset invert switch");
    if (toggle) {
        toggle->setChecked(true);
        const auto* record = [&]() -> const document::ParameterRecord* {
            for (const auto& binding : session.composition()->graph().findNode(key)->parameters)
                if (binding.role == "invert")
                    return session.composition()->parameters().find(binding.parameterId);
            return nullptr;
        }();
        expect(record && std::get<bool>(
                             std::get<document::ConstantValueSource>(record->source).value),
               "inverting the key publishes the stored invert through a command");
        expect(session.undo(), "Luma Key invert edit undoes");
        expect(session.redo(), "Luma Key invert edit redoes");
    }
}

void colourEffectRows() {
    auto project = document::makeNewProject("Color", "Main", core::RationalTime::fromInteger(10));
    const auto composition = project.initialCompositionId;
    for (const auto kind : {document::AssetKind::Lut, document::AssetKind::Image}) {
        document::AssetRecord asset;
        asset.id = document::AssetId::fromRaw(kind == document::AssetKind::Lut ? 70 : 71);
        asset.kind = kind;
        asset.name = kind == document::AssetKind::Lut ? "Show LUT" : "Plate";
        asset.locator = {"file", "project-relative", "show.cube", "file:///show.cube"};
        if (kind == document::AssetKind::Image)
            asset.width = asset.height = 1;
        expect(project.project.addAsset(asset), "color picker asset fixture");
    }
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, composition);
    ui::AssetsEditor assets(session);
    const auto* tree = assets.findChild<QTreeWidget*>();
    const auto lutRows = tree->findItems("Show LUT", Qt::MatchExactly | Qt::MatchRecursive);
    expect(lutRows.size() == 1 && lutRows.front()->text(1) == "LUT",
           "Assets shows the LUT kind label");
    const auto cst = addNode(session, "bloom.ocio-colour-space-transform");
    expect(cst.isValid(), "Add supports CST");
    session.selectNode(cst);
    ui::PropertiesEditor properties(session);
    properties.show();
    QCoreApplication::processEvents();
    const auto spaces = properties.findChildren<ui::kit::KDropdown*>("propertiesEffectColorSpace");
    expect(spaces.size() == 2 && spaces.front()->currentText() == "Working space" &&
               spaces.front()->count() > 1,
           "CST reuses searchable config pickers with working default");
    const auto file = addNode(session, "bloom.ocio-file-transform");
    session.selectNode(file);
    QCoreApplication::processEvents();
    auto* picker = properties.findChild<ui::kit::KDropdown*>("propertiesLutAsset");
    expect(picker && picker->count() == 2 && picker->itemData(1).value<std::int64_t>() == 70,
           "LUT picker contains only LUT assets");
    if (picker)
        picker->chooseIndex(1);
    const auto* node = session.composition()->graph().findNode(file);
    for (const auto& binding : node->parameters)
        if (binding.role == "lut") {
            const auto* parameter = session.composition()->parameters().find(binding.parameterId);
            const auto* value = std::get_if<document::ConstantValueSource>(&parameter->source);
            expect(value && std::get<std::int64_t>(value->value) == 70,
                   "LUT selection publishes an integer asset reference through a command");
        }
    expect(session.undo(), "LUT choice is undoable");
    const auto* definition =
        document::builtInNodeDefinitions().find("bloom.ocio-file-transform", 1);
    expect(definition && definition->category == document::NodeCategory::Color &&
               definition->inputs.front().name == "input" &&
               definition->outputs.front().name == "image",
           "Color effect card definition has the image socket pair");
}

void upstreamRows() {
    auto project =
        document::makeNewProject("Upstream", "Main", core::RationalTime::fromInteger(10));
    auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);
    const auto root = addNode(session, document::kScalarMathNodeType);
    const auto first = addNode(session, document::kScalarMathNodeType);
    const auto second = addNode(session, document::kScalarMathNodeType);
    const auto third = addNode(session, document::kScalarMathNodeType);
    const auto fourth = addNode(session, document::kScalarValueNodeType);
    connectNodes(session, first, root, "a");
    connectNodes(session, first, root, "b");
    connectNodes(session, second, first, "a");
    connectNodes(session, third, second, "a");
    connectNodes(session, fourth, third, "a");
    session.selectNode(root);
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* panel = new ui::PropertiesEditor(session, &window);
    auto* canvas = new ui::NodeGraphEditor(session, &window);
    layout->addWidget(panel);
    layout->addWidget(canvas);
    window.resize(900, 800);
    window.show();
    QCoreApplication::processEvents();
    auto* drivenRow = row(*panel, "a");
    auto* value = drivenRow ? drivenRow->findChild<QLabel*>("propertiesDrivenValue") : nullptr;
    QElapsedTimer wait;
    wait.start();
    while (value && value->text() == "Resolving…" && wait.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    expect(value && value->text() == "0",
           "detached driven graph resolves through background evaluator");
    expect(drivenRow && drivenRow->findChild<ui::kit::KButton*>("propertiesDriverLink") &&
               drivenRow->findChild<ui::kit::KButton*>("propertiesDriverLink")->text() == "Math",
           "driven row names the driver node");
    if (drivenRow) {
        Q_EMIT drivenRow->customContextMenuRequested(QPoint{});
        auto* reset = drivenRow->findChild<QAction*>("propertiesResetToDefault");
        expect(reset && reset->isEnabled(), "driven row offers Reset to default");
        const auto before = stack.size();
        if (reset)
            reset->trigger();
        expect(stack.size() == before + 1,
               "driven reset disconnects to registry default in one command");
        expect(session.undo(), "driven reset is undoable");
        for (auto* menu : panel->findChildren<QMenu*>())
            menu->close();
    }
    auto* upstream = panel->findChild<QWidget*>("propertiesUpstreamPanel");
    expect(upstream && upstream->findChildren<ui::kit::KSection*>().size() == 3,
           "BFS deduplicates paths and limits sections to depth three");
    auto* more = panel->findChild<QLabel*>("propertiesMoreUpstream");
    expect(more && more->text() == "and 1 more upstream", "one exact beyond-depth count");
    if (upstream) {
        const auto sections = upstream->findChildren<ui::kit::KSection*>();
        expect(sections.front()->property("nodeId").toULongLong() == first.value(),
               "BFS nearest node first");
        auto* operand = row(*sections.front(), "b");
        if (operand) {
            operand->findChild<ui::kit::KValueField*>()->setValue(5);
            expect(session.selectedNode()->id == root,
                   "editing an upstream operand preserves selection");
        }
        sections.front()->findChild<ui::kit::KButton*>("propertiesJumpToNode")->click();
        expect(session.selectedNode()->id == first, "Jump selects its own node");
        auto* item = canvas->graphScene()->findNodeItem(first);
        const auto center =
            canvas->graphView()->mapToScene(canvas->graphView()->viewport()->rect().center());
        expect(item && QLineF(center, item->sceneBoundingRect().center()).length() < 3,
               "Jump centers the canvas on its node");
    }
    const auto vector = addNode(session, document::kVector2MathNodeType);
    connectNodes(session, fourth, vector, "a");
    session.selectNode(vector);
    value = panel->findChild<QLabel*>("propertiesDrivenValue");
    wait.restart();
    while (value && value->text() == "Resolving…" && wait.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    expect(value && value->text() == "0, 0", "driver display applies scalar-to-vector promotion");
    const auto string = addNode(session, document::kStringValueNodeType);
    const auto stringSwitch = addNode(session, document::kStringSwitchNodeType);
    const auto stringParameter =
        session.composition()->graph().findNode(string)->parameters.front().parameterId;
    expect(session.setParameterValue(stringParameter, std::string("Exact string"), "String"),
           "string driver fixture");
    connectNodes(session, string, stringSwitch, "ifFalse");
    connectNodes(session, string, stringSwitch, "ifTrue");
    const auto stringConsumer = addNode(session, document::kStringSwitchNodeType);
    connectNodes(session, stringSwitch, stringConsumer, "ifFalse");
    session.selectNode(stringConsumer);
    value = panel->findChild<QLabel*>("propertiesDrivenValue");
    wait.restart();
    while (value && value->text() == "Resolving…" && wait.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    if (value && value->text() != "Exact string")
        std::cerr << "STRING RESULT " << value->text().toStdString() << "\n";
    expect(value && value->text() == "Exact string",
           "detached String driver resolves without image lowering");
}
// Task DRIVE-1. The headline case at the panel: a text layer whose WORDS come from a String node.
// The content row has no authored value any more, so it shows the driver and the resolved text
// read-only rather than an editor holding a stale string -- the same shape every other driven kind
// already had, now that every kind can actually be driven.
void drivenTextContent() {
    auto project =
        document::makeNewProject("Driven text", "Main", core::RationalTime::fromInteger(10));
    const auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);
    expect(session.addTextLayer("Title", "Authored"), "driven-content fixture adds its text layer");
    const auto* content = session.parameterForSelection(document::kTextParameterRole);
    if (content == nullptr) {
        expect(false, "the text layer exposes its content parameter");
        return;
    }
    const auto contentId = content->id;
    const auto string = addNode(session, document::kStringValueNodeType);
    const auto stringParameter =
        session.composition()->graph().findNode(string)->parameters.front().parameterId;
    expect(session.setParameterValue(stringParameter, std::string("Driven words"), "String"),
           "the String node carries the words");
    commands::Transaction drive("Drive content", session.snapshot().revision());
    drive.emplace<commands::SetParameterSource>(
        id, contentId,
        document::DriverBindingSource{string, std::string(document::kValuePortName)});
    expect(session.executeNodeTransaction(std::move(drive)).changed(),
           "the text content becomes driven");

    ui::PropertiesEditor panel(session);
    panel.resize(420, 700);
    panel.show();
    QCoreApplication::processEvents();
    QWidget* contentRow = nullptr;
    for (auto* candidate : panel.findChildren<QWidget*>("propertiesRow"))
        if (candidate->property("parameterId").toULongLong() == contentId.value())
            contentRow = candidate;
    if (contentRow == nullptr) {
        expect(false, "the content row is realized");
        return;
    }
    auto* editor = contentRow->findChild<QWidget*>("textContentEditor");
    auto* link = contentRow->findChild<ui::kit::KButton*>("propertiesDriverLink");
    auto* value = contentRow->findChild<QLabel*>("propertiesDrivenValue");
    expect(editor != nullptr && !editor->isVisible(),
           "a driven String shows no text editor, because there is no authored string to edit");
    expect(link != nullptr && link->isVisible() && !link->text().isEmpty(),
           "and names the node its words come from");
    QElapsedTimer wait;
    wait.start();
    while (value && value->text() == "Resolving…" && wait.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    expect(value != nullptr && value->text() == "Driven words",
           "and shows the resolved text itself, read-only");
    expect(value != nullptr && value->text() == session.drivenValueText(contentId),
           "read from the session, so the timeline row for this parameter shows the same string");
}

void widthRule() {
    auto project = document::makeNewProject("Width", "Main", core::RationalTime::fromInteger(10));
    auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);
    expect(session.addSolidLayer("A very long source name for a narrow viewport", {1, 0, 0, 1}),
           "width fixture");
    ui::PropertiesEditor panel(session);
    panel.resize(ui::kit::px(ui::kit::Size::PanelMinWidth), 260);
    panel.show();
    QCoreApplication::processEvents();
    auto* scroll = panel.findChild<QScrollArea*>("propertiesScrollArea");
    expect(panel.width() == ui::kit::px(ui::kit::Size::PanelMinWidth),
           "panel stays at the unchanged 300-design-pixel minimum");
    expect(scroll && scroll->verticalScrollBar()->maximum() > 0,
           "body scrolls vertically at narrow size");
    if (!scroll)
        return;
    if (scroll->widget()->width() > scroll->viewport()->width())
        std::cerr << "WIDTH viewport=" << scroll->viewport()->width()
                  << " body=" << scroll->widget()->width()
                  << " min=" << scroll->widget()->minimumSizeHint().width() << '\n';
    expect(scroll->widget()->width() <= scroll->viewport()->width(),
           "body never exceeds viewport width");
    for (auto* child : scroll->widget()->findChildren<QWidget*>()) {
        if (!child->isVisible() || child->isWindow())
            continue;
        if (child->minimumSizeHint().width() > scroll->viewport()->width())
            std::cerr << "WIDE " << child->objectName().toStdString() << " width=" << child->width()
                      << " min=" << child->minimumSizeHint().width() << '\n';
        expect(child->width() <= scroll->viewport()->width(),
               "no visible child is wider than viewport");
        if (auto* field = qobject_cast<ui::kit::KValueField*>(child))
            expect(field->width() >= field->minimumSizeHint().width(),
                   "numeric field keeps its legible floor");
        if (auto* label = qobject_cast<QLabel*>(child);
            label && label->objectName() == "propertiesRowLabel")
            expect(!label->text().endsWith(QChar(0x2026)), "narrow labels wrap, never elide");
    }
}
// #582: no label of any built-in node's Properties section is cut, at a wide, a normal and a
// narrow panel. A label either fits its column on one line or wraps onto lines the row has room
// for; its text is always the whole name.
void everyBuiltInLabelIsShownWhole() {
    int checked = 0;
    for (const auto& definition : document::builtInNodeDefinitions().definitions()) {
        const std::string typeId = definition.key.typeId;
        auto project = document::makeNewProject("Labels", "Main", core::RationalTime::fromInteger(10));
        document::Document document(std::move(project.project));
        commands::CommandStack stack(document);
        ui::CompositionSession session(document, stack, project.initialCompositionId);
        ui::PropertiesEditor properties(session);
        const auto node = addNode(session, typeId);
        if (!node.isValid())
            continue;
        session.selectNode(node);
        // Motion blur's rows carry the longest labels; show them wherever a node has the toggle.
        for (const auto& binding : session.composition()->graph().findNode(node)->parameters)
            if (binding.role == "motionBlur")
                (void)session.setParameterValue(binding.parameterId, true, "Set");
        for (const int width : {2000, 900, 420}) {
            properties.resize(width, 1500);
            properties.show();
            QCoreApplication::processEvents();
            // BLOOM_LABEL_CAPTURE_DIR saves Properties pictures for review; BLOOM_LABEL_CAPTURE_TYPES
            // (comma-separated type ids) picks the nodes and then every audited width is saved,
            // named with BLOOM_LABEL_CAPTURE_SCALE (the scale factor the run was started with).
            if (const auto directory = qEnvironmentVariable("BLOOM_LABEL_CAPTURE_DIR");
                !directory.isEmpty()) {
                const auto types =
                    qEnvironmentVariable("BLOOM_LABEL_CAPTURE_TYPES").split(QLatin1Char(','),
                                                                            Qt::SkipEmptyParts);
                const bool listed = types.contains(QString::fromStdString(typeId));
                const bool legacy = types.isEmpty() && width == 900 &&
                                    (typeId == document::kCameraShakeNodeType ||
                                     typeId == document::kTransform2DNodeType);
                if (listed || legacy) {
                    QDir().mkpath(directory);
                    const auto scale = qEnvironmentVariable("BLOOM_LABEL_CAPTURE_SCALE");
                    properties.grab().save(directory + QLatin1Char('/') +
                                           QString::fromStdString(typeId) + QLatin1Char('-') +
                                           QString::number(width) +
                                           (scale.isEmpty() ? QString()
                                                            : QStringLiteral("-dpr") + scale) +
                                           QStringLiteral(".png"));
                }
            }
            // Two rows in one section never share a label: an artist must be able to tell them
            // apart. A section is the nearest KSection above the label.
            if (width == 2000) {
                std::map<std::pair<const QWidget*, QString>, int> seen;
                std::map<std::pair<const QWidget*, QString>, QString> where;
                for (auto* label : properties.findChildren<QLabel*>("propertiesRowLabel")) {
                    const QWidget* section = label->parentWidget();
                    while (section != nullptr && qobject_cast<const ui::kit::KSection*>(section) == nullptr)
                        section = section->parentWidget();
                    if (label->isVisibleTo(&properties) && !label->text().isEmpty())
                        ++seen[{section, label->text()}];
                    if (label->isVisibleTo(&properties)) {
                        const QWidget* owner = label->parentWidget();
                        while (owner != nullptr && owner != section && !owner->property("role").isValid() &&
                               !owner->property("valueOutputPort").isValid())
                            owner = owner->parentWidget();
                        where[{section, label->text()}] +=
                            QStringLiteral(" [") +
                            (owner && owner != section ? owner->property("role").toString() : QStringLiteral("output/other")) +
                            QLatin1Char(']');
                    }
                }
                for (const auto& [key, count] : seen) {
                    const auto& text = key.second;
                    expect(count == 1, (std::string("two Properties rows share the label \"") +
                                        text.toStdString() + "\" in " + typeId + where[key].toStdString())
                                           .c_str());
                }
            }
            for (auto* label : properties.findChildren<QLabel*>("propertiesRowLabel")) {
                if (!label->isVisibleTo(&properties) || label->toolTip().isEmpty())
                    continue;
                ++checked;
                const QFontMetrics metrics(label->font());
                const QRect needed = metrics.boundingRect(QRect(0, 0, label->width(), 100000),
                                                          Qt::TextWordWrap | Qt::TextWrapAnywhere,
                                                          label->toolTip());
                expect(label->text() == label->toolTip() &&
                           needed.height() <= label->height(),
                       (std::string("a Properties label is cut: ") +
                        label->toolTip().toStdString() + " in " + typeId +
                        " at " + std::to_string(width))
                           .c_str());
            }
        }
    }
    expect(checked > 100, "the label audit visits the built-in nodes' rows");
}
void genericKinds() {
    auto project = document::makeNewProject("Kinds", "Main", core::RationalTime::fromInteger(10));
    const auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);
    ui::PropertiesEditor panel(session);
    for (const auto type : {document::kScalarValueNodeType, document::kIntegerValueNodeType,
                            document::kBooleanValueNodeType, document::kStringValueNodeType,
                            document::kVector2ValueNodeType, document::kVector3ValueNodeType,
                            document::kColorValueNodeType, document::kVector2MathNodeType}) {
        const auto nodeId = addNode(session, type);
        session.selectNode(nodeId);
        const auto* node = session.selectedNode();
        const auto* definition =
            document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
        expect(panel.findChildren<QWidget*>("propertiesRegistryRow").size() ==
                   static_cast<qsizetype>(definition->parameters.size()),
               "every value-node registry parameter renders, including scale operands");
        auto* valueRow = row(panel, "value");
        if (!valueRow)
            continue;
        const auto parameter =
            document::ParameterId::fromRaw(valueRow->property("parameterId").toULongLong());
        if (type == document::kIntegerValueNodeType) {
            auto* field = valueRow->findChild<ui::kit::KValueField*>("propertiesRegistryInteger");
            expect(field != nullptr && field->isIntegerMode(),
                   "integer has an exact integer value field");
            if (field) {
                // Typed, as the artist would: the editor parses int64 exactly.
                QKeyEvent open(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QCoreApplication::sendEvent(field, &open);
                field->lineEdit()->setText("9223372036854775807");
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QCoreApplication::sendEvent(field->lineEdit(), &enter);
                QCoreApplication::processEvents();
            }
            const auto* record = session.composition()->parameters().find(parameter);
            const auto& constant = std::get<document::ConstantValueSource>(record->source);
            expect(std::get<std::int64_t>(constant.value) ==
                       std::numeric_limits<std::int64_t>::max(),
                   "integer field preserves all int64 digits");
        } else if (type == document::kBooleanValueNodeType) {
            auto* toggle = valueRow->findChild<ui::kit::KSwitch*>();
            expect(toggle != nullptr, "boolean renders a switch");
            if (toggle)
                toggle->setChecked(true);
        } else if (type == document::kColorValueNodeType) {
            expect(valueRow->findChild<ui::kit::KColorChip*>() &&
                       valueRow->findChildren<ui::kit::KValueField*>().size() == 4,
                   "color has a picker chip and four expanded components");
            expect(session.setParameterValue(parameter, core::Color4d{2, -1, 0.5, 0.25}, "Color"),
                   "HDR generic color authors through session");
            const auto fields = valueRow->findChildren<ui::kit::KValueField*>();
            expect(fields[0]->value() == 2 && fields[1]->value() == -1,
                   "generic components retain HDR and negative values");
        } else if (type == document::kVector2ValueNodeType ||
                   type == document::kVector3ValueNodeType) {
            expect(valueRow->findChildren<ui::kit::KValueField*>().size() ==
                       (type == document::kVector2ValueNodeType ? 2 : 3),
                   "vectors render every component");
            const bool three = type == document::kVector3ValueNodeType;
            expect(session.setParameterValue(
                       parameter,
                       three ? document::ParameterValue{document::Vec3d{2e16, 0, -2e16}}
                             : document::ParameterValue{document::Vec2d{2e16, 0}},
                       "Large vector"),
                   "large vector components are valid document values");
            auto* refreshed = row(panel, "value");
            const auto fields = refreshed->findChildren<ui::kit::KValueField*>();
            expect(fields[0]->value() == 2e16, "Properties projects the actual finite component");
            fields[1]->setValue(7);
            if (three)
                expect(session.effectiveVec3Value(parameter) == document::Vec3d{2e16, 7, -2e16},
                       "Properties Y edits preserve authoritative X and Z");
            else
                expect(session.effectiveVec2Value(parameter) == document::Vec2d{2e16, 7},
                       "Properties Y edits preserve the formerly clamped X");
        }
    }
}
void scaledProjectionGestureKeepsItsUnit() {
    auto project =
        document::makeNewProject("Projection gesture", "Main", core::RationalTime::fromInteger(2));
    const auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);
    expect(session.addTextLayer("Text", "Projection"), "projection fixture authors a text layer");
    ui::PropertiesEditor panel(session);
    panel.show();
    QCoreApplication::processEvents();
    auto* layoutRow = row(panel, "line-height");
    expect(layoutRow != nullptr, "line height has a registry row");
    if (!layoutRow)
        return;
    const auto parameter =
        document::ParameterId::fromRaw(layoutRow->property("parameterId").toULongLong());
    const auto extreme = std::numeric_limits<double>::max();
    expect(session.setParameterValue(parameter, extreme, "Native line height"),
           "large finite line height remains authored data");
    auto* field = layoutRow->findChild<ui::kit::KValueField*>();
    expect(field && field->value() == extreme && field->unit() == QStringLiteral("×"),
           "line height exposes its native factor when percentage conversion would overflow");
    if (!field)
        return;
    const auto open = [field] {
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(field, &enter);
    };
    const auto typeDigit = [field](const int key, const QString& text) {
        field->lineEdit()->selectAll();
        QKeyEvent digit(QEvent::KeyPress, key, Qt::NoModifier, text);
        QApplication::sendEvent(field->lineEdit(), &digit);
    };
    open();
    typeDigit(Qt::Key_2, "2");
    expect(field->isEditing() && field->unit() == QStringLiteral("×") &&
               field->displayScale() == 1 &&
               session.effectiveScalarValue(parameter) == 2,
           "synchronous Properties refresh leaves a live gesture's projection untouched");
    typeDigit(Qt::Key_3, "3");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(field->lineEdit(), &enter);
    expect(session.effectiveScalarValue(parameter) == 3 && field->value() == 300 &&
               field->unit() == QStringLiteral("%"),
           "commit uses the native unit then refreshes to percentage presentation");
    expect(session.setParameterValue(parameter, extreme, "Native line height"),
           "the cancel fixture restores native line height");
    open();
    typeDigit(Qt::Key_2, "2");
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(field->lineEdit(), &escape);
    // Escape confirms everywhere: the typed native factor is committed; Undo is the discard.
    expect(session.effectiveScalarValue(parameter) == 2,
           "Escape commits the typed line height in its native unit");
    expect(session.undo() && session.effectiveScalarValue(parameter) == extreme &&
               field->value() == extreme && field->unit() == QStringLiteral("×"),
           "Undo restores authoritative line height and its matching unit");
}

void registryRows() {
    auto project =
        document::makeNewProject("Registry", "Main", core::RationalTime::fromInteger(10));
    auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);
    expect(session.addSolidLayer("Solid", {1, 0, 0, 1}), "add solid");
    ui::PropertiesEditor panel(session);
    auto* solidSection = panel.findChild<ui::kit::KSection*>("propertiesSection_solid");
    QStringList solidLabels;
    if (solidSection) {
        for (int index = 0; index < solidSection->bodyLayout()->count(); ++index) {
            auto* sourceRow = solidSection->bodyLayout()->itemAt(index)->widget();
            if (!sourceRow)
                continue;
            if (sourceRow->objectName() == "propertiesRegistryRow")
                sourceRow = sourceRow->findChild<QWidget*>("propertiesRow");
            else if (sourceRow->objectName() != "propertiesRow")
                continue;
            if (sourceRow && !sourceRow->property("rowLabel").toString().isEmpty())
                solidLabels.push_back(sourceRow->property("rowLabel").toString());
        }
    }
    expect(solidLabels == QStringList{"Color", "Width", "Height"},
           "Solid Properties exposes only Color, Width, and Height source rows");
    expect(panel.findChild<QLabel*>("solidAlphaAssociation") == nullptr &&
               panel.findChild<QLabel*>("solidColorEncoding") == nullptr,
           "Solid technical alpha and encoding metadata stays out of Properties rows");
    auto* width = row(panel, "width");
    auto* height = row(panel, "height");
    expect(width && height, "registry adds both solid dimensions");
    if (width) {
        auto* field = width->findChild<ui::kit::KValueField*>();
        const auto parameter =
            document::ParameterId::fromRaw(width->property("parameterId").toULongLong());
        const auto selection = session.selection().primary;
        const auto before = stack.size();
        const QPoint start = field->rect().center();
        const QPoint finish = start + QPoint(20, 0);
        // Twenty pixels less the drag threshold's dead zone (lane B: crossing it never jumps).
        const double editedValue =
            field->value() + (20 - QApplication::startDragDistance()) * field->singleStep();
        QMouseEvent press(QEvent::MouseButtonPress, start, field->mapToGlobal(start),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(field, &press);
        QMouseEvent move(QEvent::MouseMove, finish, field->mapToGlobal(finish), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(field, &move);
        expect(field->isScrubbing() && stack.size() == before &&
                   session.effectiveScalarValue(parameter) == editedValue,
               "scrubbing publishes a live value without intermediate commands");
        QMouseEvent release(QEvent::MouseButtonRelease, finish, field->mapToGlobal(finish),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(field, &release);
        expect(stack.size() == before + 1 && session.effectiveScalarValue(parameter) == editedValue,
               "scrub commits one exact parameter edit");
        expect(session.selection().primary == selection, "registry edit preserves selection");
        expect(session.undo(), "registry edit is undoable");
        auto* section = panel.findChild<ui::kit::KSection*>("propertiesSection_solid");
        field->setValue(872);
        Q_EMIT section->resetRequested();
        expect(session.effectiveScalarValue(parameter) == 1,
               "source reset includes registry defaults");
    }
    expect(session.addTextLayer("Text", "Hello", 48, {1, 1, 1, 1}), "add text");
    panel.show();
    QCoreApplication::processEvents();
    expect(panel.findChild<QLineEdit*>("propertiesSearchField") == nullptr,
           "header search is removed");
    expect(row(panel, "line-height")->isVisible() && row(panel, "letter-spacing")->isVisible(),
           "registry rows stay visible with no search filter");
    expect(panel.findChild<ui::kit::KSection*>("propertiesSection_transform")->isVisible(),
           "sections stay visible with no search filter");
    auto* alignment = row(panel, "alignment");
    expect(alignment && row(panel, "line-height") && row(panel, "letter-spacing"),
           "registry adds every text layout parameter");
    if (alignment) {
        auto* dropdown = alignment->findChild<ui::kit::KRadioGroup*>();
        expect(dropdown && dropdown->count() == 3, "alignment is a closed segmented icon control");
        if (dropdown)
            dropdown->setCurrentIndex(2);
        expect(session.canUndo(), "enum edit uses command history");
    }
    auto* font = row(panel, "font");
    expect(font != nullptr, "registry adds the text font parameter");
    if (font) {
        auto* dropdown = font->findChild<ui::kit::KDropdown*>("propertiesTextFont");
        expect(dropdown != nullptr && dropdown->currentText() == QStringLiteral("DejaVu Sans"),
               "Font is the font field, showing the font ID's family");
    }
    const auto layer = std::get<document::LayerId>(session.selection().primary);
    const auto boundary = session.boundaryNodeForLayer(layer);
    expect(boundary.has_value(), "text layer has a boundary node");
    if (!boundary)
        return;
    session.selectNode(*boundary);
    auto* upstream = panel.findChild<QWidget*>("propertiesUpstreamPanel");
    expect(upstream && upstream->findChildren<ui::kit::KSection*>().size() == 1,
           "image input exposes one source section when its boundary node is selected");
    expect(upstream && upstream->findChild<QPlainTextEdit*>("propertiesRegistryMultiline"),
           "upstream Text content uses the generic multiline editor");
    std::vector<document::NodeId> terminals;
    for (const auto& node : session.composition()->graph().nodes())
        if (node.typeId == document::kLayerStackNodeType ||
            node.typeId == document::kCompositionOutputNodeType)
            terminals.push_back(node.id);
    for (auto terminal : terminals) {
        session.selectNode(terminal);
        upstream = panel.findChild<QWidget*>("propertiesUpstreamPanel");
        expect(!upstream || upstream->findChildren<ui::kit::KSection*>().empty(),
               "Merge and Output stop upstream traversal");
    }
}
void videoRows() {
    auto seed =
        document::makeNewProject("Video Properties", "Main", core::RationalTime::fromInteger(2));
    const auto composition = seed.initialCompositionId;
    document::Document document(std::move(seed.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, composition);
    const auto video = addNode(session, "bloom.video-source");
    expect(video.isValid(), "Video node registers for Properties");
    session.selectNode(video);
    ui::PropertiesEditor panel(session);
    for (const auto* role : {"asset", "startFrame", "loopMode", "inputColorSpaceId"})
        expect(row(panel, role) != nullptr, "Video Properties uses every registry parameter");
    // #487: the legacy Color Space is a choice of the one Input color space control.
    expect(row(panel, "colorSpace") == nullptr, "Video Properties shows one color space control");
    expect(row(panel, "premultiply") == nullptr, "Video alpha association comes from its decoder");
    if (auto* loop = row(panel, "loopMode")) {
        auto* selector = loop->findChild<ui::kit::KDropdown*>();
        expect(selector && selector->count() == 3,
               "Video loop modes use the shared registry selector");
    }
    // Document 1.25: the audio stream row lists the asset's streams and selects one by command;
    // field order and timecode type are read-only source metadata.
    expect(row(panel, "audioStream") == nullptr,
           "the audio stream uses its purpose-built row, not a generic integer");
    auto* streams = panel.findChild<ui::kit::KDropdown*>("propertiesVideoAudioStream");
    expect(streams && streams->count() == 1 && !streams->isEnabled(),
           "a Video node without an asset offers no stream choice");
    expect(panel.findChild<ui::kit::KLabel*>("propertiesVideoFieldOrder") &&
               panel.findChild<ui::kit::KLabel*>("propertiesVideoTimecode"),
           "Video Properties shows field order and timecode");
}
} // namespace
// The spurious "Choose Font" transaction (lane A, finding 5): rebuilding the font selector when
// the asynchronous system catalogue arrives -- and re-selecting the stored face in it -- is not the
// artist's choice, so it must not reach the document. Only a choice does.
void fontCatalogueRefreshIsNotAnEdit() {
    auto project = document::makeNewProject("Fonts", "Main", core::RationalTime::fromInteger(10));
    const auto id = project.initialCompositionId;
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, id);
    expect(session.addTextLayer("Title", "Bloom"), "font fixture adds its text layer");
    const auto history = stack.size();
    const auto revision = session.snapshot().revision();
    ui::PropertiesEditor panel(session);
    panel.resize(420, 900);
    panel.show();
    ui::kit::KDropdown* font = nullptr;
    int populated = 0;
    QElapsedTimer clock;
    clock.start();
    // The row polls the catalogue every 50 ms and rebuilds itself when the system faces land
    // after the embedded ones; wait for that rebuild (the list grows), then a little longer.
    int first = -1;
    qint64 grewAt = -1;
    while (clock.elapsed() < 10'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        font = panel.findChild<ui::kit::KDropdown*>("propertiesTextFont");
        if (font == nullptr)
            continue;
        if (first < 0)
            first = font->count();
        populated = std::max(populated, font->count());
        if (grewAt < 0 && populated > first)
            grewAt = clock.elapsed();
        if (grewAt >= 0 && clock.elapsed() > grewAt + 500)
            break;
    }
    expect(font != nullptr && populated > 0, "the text layer shows a populated font selector");
    expect(stack.size() == history, "building and refreshing the font list adds no undo entry");
    expect(session.snapshot().revision() == revision,
           "and no transaction reaches the document");
    if (font != nullptr && font->count() > 1) {
        font->chooseIndex(font->currentIndex() == 0 ? 1 : 0);
        QCoreApplication::processEvents();
        expect(stack.size() > history, "choosing a face is one undoable edit");
    }
}
// #339: Drop Shadow, Glow and Light Wrap present every control from the registry, and a switch
// edits through command history.
void recipeRows() {
    auto project = document::makeNewProject("Recipes", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    ui::PropertiesEditor properties(session);
    properties.show();
    struct Expected final {
        const char* type;
        std::vector<const char*> roles;
        const char* flag;
    };
    for (const auto& [type, roles, flag] :
         {Expected{"bloom.drop-shadow",
                   {"color", "opacity", "direction", "distance", "softness"},
                   "shadowOnly"},
          Expected{"bloom.glow",
                   {"threshold", "knee", "intensity", "size", "falloff", "tint", "octaves",
                    // #506
                    "mode", "exponent", "radiusRed", "radiusGreen", "radiusBlue", "points", "angle",
                    "length", "chromatic", "dirtAmount", "rollOff"},
                   "glowOnly"},
          Expected{"bloom.light-wrap", {"radius", "amount"}, "wrapOnly"},
          // #516: two values and no switch.
          Expected{"bloom.edge-extend", {"distance", "amount"}, nullptr},
          // #509: a band's width, softness, offset, colour and composite choice; no switch.
          Expected{"bloom.alpha-contour",
                   {"width", "softness", "offset", "color", "composite"},
                   nullptr},
          // #503: not a recipe, but the same shape: a mode, a colour and a switch.
          Expected{"bloom.alpha-association", {"mode", "background"}, "white"}}) {
        const auto node = addNode(session, type);
        expect(node.isValid(), "a recipe is discoverable through generic AddNode");
        session.selectNode(node);
        QCoreApplication::processEvents();
        bool present = true;
        for (const auto* role : roles)
            present = present && row(properties, role) != nullptr;
        expect(present, "a recipe presents every value control");
        // #541: Glow's tint gradient is the card widget's gradient editor, in Properties too.
        if (std::string_view(type) == "bloom.glow")
            expect(properties.findChild<QWidget*>(QStringLiteral("glowTintGradientEditor")) !=
                       nullptr,
                   "Properties edits Glow's tint gradient on a gradient editor");
        if (flag == nullptr)
            continue;
        auto* toggleRow = row(properties, flag);
        auto* toggle = toggleRow ? toggleRow->findChild<ui::kit::KSwitch*>() : nullptr;
        expect(toggle != nullptr && !toggle->isChecked(), "a recipe presents an unset switch");
        if (toggle) {
            toggle->setChecked(true);
            const auto* record = [&]() -> const document::ParameterRecord* {
                for (const auto& binding :
                     session.composition()->graph().findNode(node)->parameters)
                    if (binding.role == flag)
                        return session.composition()->parameters().find(binding.parameterId);
                return nullptr;
            }();
            expect(record && std::get<bool>(
                                 std::get<document::ConstantValueSource>(record->source).value),
                   "setting a recipe switch publishes it through a command");
            expect(session.undo(), "the recipe switch edit undoes");
            expect(session.redo(), "the recipe switch edit redoes");
        }
    }
}

// #566 Auto Exposure in Properties: a row for each value and the look-back, in plain words, each
// label shown whole, nothing wider than the panel, at the widths of a wide, a normal and a narrow
// panel. With BLOOM_GRAMMAR_ARTIFACT_DIR set, the panel is written at each width (suffixed with the
// device pixel ratio).
void autoExposureRows() {
    auto project = document::makeNewProject("Exposure", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    ui::PropertiesEditor properties(session);
    const auto exposure = addNode(session, document::kAutoExposureNodeType);
    session.selectNode(exposure);
    const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
    const QString suffix = QGuiApplication::primaryScreen()->devicePixelRatio() == 1.0
                               ? QStringLiteral("dpr1")
                               : QStringLiteral("dpr15");
    for (const int width : {2000, 900, 420}) {
        properties.resize(width, 900);
        properties.show();
        QCoreApplication::processEvents();
        for (const auto& spec : document::autoExposureParameters()) {
            auto* found = row(properties, std::string(spec.role).c_str());
            expect(found != nullptr && found->isVisibleTo(&properties),
                   "Auto Exposure presents every value");
            if (found == nullptr)
                continue;
            expect(found->property("rowLabel").toString() ==
                       QString::fromLatin1(spec.label.data(), static_cast<int>(spec.label.size())),
                   "an Auto Exposure row is named in plain words");
            QLabel* label = nullptr;
            for (auto* candidate : properties.findChildren<QLabel*>("propertiesRowLabel"))
                if (candidate->text() == QString::fromLatin1(spec.label.data(),
                                                             static_cast<int>(spec.label.size())) &&
                    candidate->isVisibleTo(&properties))
                    label = candidate;
            expect(label != nullptr &&
                       label->width() >= label->fontMetrics().horizontalAdvance(label->text()),
                   "an Auto Exposure row label is shown whole");
            for (auto* field : found->findChildren<ui::kit::KValueField*>())
                if (field->isVisibleTo(&properties))
                    expect(!field->paintedText().contains(QChar(0x2026)) && !field->isDisplayRounded(),
                           "an Auto Exposure value is shown whole");
        }
        if (auto* scroll = properties.findChild<QScrollArea*>())
            expect(scroll->widget()->width() <= scroll->viewport()->width(),
                   "Auto Exposure's rows never exceed the panel's width");
        if (!directory.isEmpty()) {
            QDir().mkpath(directory);
            if (!properties.grab().save(directory + QStringLiteral("/auto-exposure-properties-") +
                                        QString::number(width) + QLatin1Char('-') + suffix +
                                        QStringLiteral(".png")))
                std::cerr << "could not write the Auto Exposure Properties capture\n";
        }
    }
}

// #518 Tone Map in Properties (and the other color cleanup nodes): a row for each value, in plain
// words, each label shown whole, nothing wider than the panel, at the widths of a wide, a normal and
// a narrow panel. With BLOOM_GRAMMAR_ARTIFACT_DIR set, the panel is written at each width (suffixed
// with the device pixel ratio).
void colorCleanupRows() {
    for (const auto type : {document::kToneMapNodeType, document::kColorMatrixNodeType,
                            document::kHslQualifierNodeType}) {
        auto project =
            document::makeNewProject("Cleanup", "Main", core::RationalTime::fromInteger(10));
        document::Document document(std::move(project.project));
        commands::CommandStack stack(document);
        ui::CompositionSession session(document, stack, project.initialCompositionId);
        ui::PropertiesEditor properties(session);
        const auto node = addNode(session, type);
        session.selectNode(node);
        const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
        const QString suffix = QGuiApplication::primaryScreen()->devicePixelRatio() == 1.0
                                   ? QStringLiteral("dpr1")
                                   : QStringLiteral("dpr15");
        for (const int width : {2000, 900, 420}) {
            properties.resize(width, 900);
            properties.show();
            QCoreApplication::processEvents();
            for (const auto& spec : document::colorCleanupParameters(type)) {
                // The roll-off range's two values are the card widget's range row, not rows of
                // their own.
                if (type == document::kToneMapNodeType &&
                    (spec.role == "knee" || spec.role == "ceiling")) {
                    auto* range = properties.findChild<QWidget*>("rangeRow.knee");
                    expect(range != nullptr && range->isVisibleTo(&properties),
                           "Tone Map presents its roll-off range");
                    continue;
                }
                auto* found = row(properties, std::string(spec.role).c_str());
                expect(found != nullptr && found->isVisibleTo(&properties),
                       "a color cleanup node presents every value");
                if (found == nullptr)
                    continue;
                const auto text =
                    QString::fromLatin1(spec.label.data(), static_cast<int>(spec.label.size()));
                expect(found->property("rowLabel").toString() == text,
                       "a color cleanup node names a row in plain words");
                QLabel* label = nullptr;
                for (auto* candidate : properties.findChildren<QLabel*>("propertiesRowLabel"))
                    if (candidate->text() == text && candidate->isVisibleTo(&properties))
                        label = candidate;
                expect(label != nullptr &&
                           label->width() >= label->fontMetrics().horizontalAdvance(label->text()),
                       "a color cleanup node shows a row label whole");
                for (auto* field : found->findChildren<ui::kit::KValueField*>())
                    if (field->isVisibleTo(&properties))
                        expect(!field->paintedText().contains(QChar(0x2026)) &&
                                   !field->isDisplayRounded(),
                               "a color cleanup node shows a value whole");
            }
            if (auto* scroll = properties.findChild<QScrollArea*>())
                expect(scroll->widget()->width() <= scroll->viewport()->width(),
                       "a color cleanup node's rows never exceed the panel's width");
            if (!directory.isEmpty()) {
                QDir().mkpath(directory);
                if (!properties.grab().save(directory + QStringLiteral("/") +
                                            QString::fromLatin1(type.data(),
                                                                static_cast<int>(type.size())) +
                                            QStringLiteral("-properties-") +
                                            QString::number(width) + QLatin1Char('-') + suffix +
                                            QStringLiteral(".png")))
                    std::cerr << "could not write the " << type << " Properties capture\n";
            }
        }
    }
}

// #516 Edge Extend in Properties: a row for Distance and Amount, in plain words, each label shown
// whole, nothing wider than the panel, at the widths of a wide, a normal and a narrow panel. With
// BLOOM_GRAMMAR_ARTIFACT_DIR set, the panel is written at each width (suffixed with the device pixel
// ratio).
void edgeExtendRows() {
    auto project = document::makeNewProject("Edge", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    ui::PropertiesEditor properties(session);
    const auto node = addNode(session, document::kEdgeExtendNodeType);
    session.selectNode(node);
    const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
    const QString suffix = QGuiApplication::primaryScreen()->devicePixelRatio() == 1.0
                               ? QStringLiteral("dpr1")
                               : QStringLiteral("dpr15");
    for (const int width : {2000, 900, 420}) {
        properties.resize(width, 900);
        properties.show();
        QCoreApplication::processEvents();
        for (const auto& [role, text] : {std::pair{"distance", QStringLiteral("Distance")},
                                         std::pair{"amount", QStringLiteral("Amount")}}) {
            auto* found = row(properties, role);
            expect(found != nullptr && found->isVisibleTo(&properties),
                   "Edge Extend presents every value");
            if (found == nullptr)
                continue;
            expect(found->property("rowLabel").toString() == text,
                   "Edge Extend names a row in plain words");
            QLabel* label = nullptr;
            for (auto* candidate : properties.findChildren<QLabel*>("propertiesRowLabel"))
                if (candidate->text() == text && candidate->isVisibleTo(&properties))
                    label = candidate;
            expect(label != nullptr &&
                       label->width() >= label->fontMetrics().horizontalAdvance(label->text()),
                   "Edge Extend shows a row label whole");
            for (auto* field : found->findChildren<ui::kit::KValueField*>())
                if (field->isVisibleTo(&properties))
                    expect(!field->paintedText().contains(QChar(0x2026)) &&
                               !field->isDisplayRounded(),
                           "Edge Extend shows a value whole");
        }
        if (auto* scroll = properties.findChild<QScrollArea*>())
            expect(scroll->widget()->width() <= scroll->viewport()->width(),
                   "Edge Extend's rows never exceed the panel's width");
        if (!directory.isEmpty()) {
            QDir().mkpath(directory);
            if (!properties.grab().save(directory + QStringLiteral("/bloom.edge-extend-properties-") +
                                        QString::number(width) + QLatin1Char('-') + suffix +
                                        QStringLiteral(".png")))
                std::cerr << "could not write the Edge Extend Properties capture\n";
        }
    }
}

// #524 and #514: the new rows of Gaussian Blur, Transform 2D and Hue/Saturation and the whole of
// Denoise & Smooth and Four-Color Gradient in Properties: every value a visible row named in plain
// words, labels and values shown whole and nothing wider than the panel, at the widths of a wide, a
// normal and a narrow panel. With BLOOM_GRAMMAR_ARTIFACT_DIR set, the panel is written at each width
// (suffixed with the device pixel ratio).
// `hidden` are rows the node's current values leave without a row (a value that would do nothing);
// `values` are set first and `state` names the capture; `height` is the panel's.
void extensionRows(const char* type, const char* captureName,
                   const std::vector<std::pair<std::string, QString>>& rows,
                   const std::vector<std::string>& hidden = {},
                   const std::vector<std::pair<std::string, document::ParameterValue>>& values = {},
                   const char* state = "", const int height = 900) {
    auto project = document::makeNewProject("Ext", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    ui::PropertiesEditor properties(session);
    const auto node = addNode(session, type);
    session.selectNode(node);
    for (const auto& [role, value] : values)
        expect(session.setNodeParameterValue(node, role, value, QStringLiteral("Set")),
               (std::string(type) + " takes " + role).c_str());
    const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
    const QString suffix = QGuiApplication::primaryScreen()->devicePixelRatio() == 1.0
                               ? QStringLiteral("dpr1")
                               : QStringLiteral("dpr15");
    for (const int width : {2000, 900, 420}) {
        properties.resize(width, height);
        properties.show();
        QCoreApplication::processEvents();
        for (const auto& role : hidden) {
            auto* found = row(properties, role.c_str());
            expect(found == nullptr || !found->isVisibleTo(&properties),
                   (std::string(type) + " shows no row for " + role + " while it does nothing")
                       .c_str());
        }
        for (const auto& [role, text] : rows) {
            auto* found = row(properties, role.c_str());
            expect(found != nullptr && found->isVisibleTo(&properties),
                   (std::string(type) + " presents " + role).c_str());
            if (found == nullptr)
                continue;
            expect(found->property("rowLabel").toString() == text,
                   (std::string(type) + " names " + role + " in plain words").c_str());
            for (auto* field : found->findChildren<ui::kit::KValueField*>())
                if (field->isVisibleTo(&properties))
                    expect(!field->paintedText().contains(QChar(0x2026)) &&
                               !field->isDisplayRounded(),
                           (std::string(type) + " shows " + role + " whole").c_str());
        }
        if (auto* scroll = properties.findChild<QScrollArea*>())
            expect(scroll->widget()->width() <= scroll->viewport()->width(),
                   (std::string(type) + "'s rows never exceed the panel's width").c_str());
        if (!directory.isEmpty()) {
            QDir().mkpath(directory);
            if (!properties.grab().save(directory + QStringLiteral("/") +
                                        QString::fromLatin1(captureName) +
                                        QString::fromLatin1(state) +
                                        QStringLiteral("-properties-") + QString::number(width) +
                                        QLatin1Char('-') + suffix + QStringLiteral(".png")))
                std::cerr << "could not write the " << captureName << " Properties capture\n";
        }
    }
}

// #524: a new Four-Color Gradient anchors its points at the corners of its composition, whatever
// the size: 640 by 360 here, with its Properties (the points read as the corners, each label shown).
void fourColorDefaultsFollowTheComposition() {
    const auto format = document::CompositionFormat::create(640, 360);
    expect(format.has_value(), "a 640 by 360 format");
    if (!format)
        return;
    auto project = document::makeNewProject("Small", "Main", core::RationalTime::fromInteger(10),
                                            *format);
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    ui::PropertiesEditor properties(session);
    const auto node = addNode(session, "bloom.four-color-gradient");
    session.selectNode(node);
    const std::array<std::pair<const char*, document::Vec2d>, 4> corners{
        {{"point1", {-320.0, 180.0}}, {"point2", {320.0, 180.0}},
         {"point3", {-320.0, -180.0}}, {"point4", {320.0, -180.0}}}};
    for (const auto& [role, corner] : corners) {
        const auto id = ui::nodeRoleParameter(session, node, role);
        const auto value = id ? session.effectiveVec2Value(*id) : std::nullopt;
        expect(value && value->x == corner.x && value->y == corner.y,
               (std::string("a new Four-Color Gradient puts ") + role +
                " at its 640 by 360 corner")
                   .c_str());
    }
    properties.resize(900, 1300);
    properties.show();
    QCoreApplication::processEvents();
    const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
    if (!directory.isEmpty()) {
        QDir().mkpath(directory);
        const QString suffix = QGuiApplication::primaryScreen()->devicePixelRatio() == 1.0
                                   ? QStringLiteral("dpr1")
                                   : QStringLiteral("dpr15");
        if (!properties.grab().save(directory + QStringLiteral("/four-color-gradient-640x360-"
                                                               "properties-900-") +
                                    suffix + QStringLiteral(".png")))
            std::cerr << "could not write the 640 by 360 Four-Color Gradient capture\n";
    }
}

void extensionPropertiesRows() {
    fourColorDefaultsFollowTheComposition();
    // Denoise: the radius while the mode is not Temporal, the frame count only in Temporal, the
    // strength while Auto strength is off and the sample region only while it is on.
    const std::vector<std::pair<std::string, QString>> spatial{
        {"mode", QStringLiteral("Mode")},
        {"radius", QStringLiteral("Radius (pixels)")},
        {"strength", QStringLiteral("Strength (noise tolerance)")},
        {"autoStrength", QStringLiteral("Auto strength")}};
    extensionRows("bloom.denoise", "denoise", spatial, {"frames", "sampleFrom", "sampleTo"});
    // Temporal: the frame count and the patch tolerance; the radius is not read.
    extensionRows("bloom.denoise", "denoise",
                  {{"mode", QStringLiteral("Mode")},
                   {"frames", QStringLiteral("Frames")},
                   {"strength", QStringLiteral("Strength (noise tolerance)")}},
                  {"radius", "sampleFrom", "sampleTo"}, {{"mode", std::int64_t{3}}}, "-temporal");
    // Auto strength: the sample region's two corners instead of the strength.
    extensionRows("bloom.denoise", "denoise",
                  {{"mode", QStringLiteral("Mode")},
                   {"radius", QStringLiteral("Radius (pixels)")},
                   {"autoStrength", QStringLiteral("Auto strength")},
                   {"sampleFrom", QStringLiteral("Sample corner 1")},
                   {"sampleTo", QStringLiteral("Sample corner 2")}},
                  {"strength", "frames"}, {{"autoStrength", true}}, "-auto", 1100);
    extensionRows("bloom.four-color-gradient", "four-color-gradient",
                  {{"point1", QStringLiteral("Point 1")}, {"point2", QStringLiteral("Point 2")},
                   {"point3", QStringLiteral("Point 3")}, {"point4", QStringLiteral("Point 4")},
                   {"color1", QStringLiteral("Color 1")}, {"color2", QStringLiteral("Color 2")},
                   {"color3", QStringLiteral("Color 3")}, {"color4", QStringLiteral("Color 4")},
                   {"blend", QStringLiteral("Blend (sharpness)")}},
                  {}, {}, "", 1300);
    // Gaussian Blur: the vertical radius only with Separate axes, the channel radii only with
    // Per-channel radius.
    const std::vector<std::pair<std::string, QString>> blurBase{
        {"radius", QStringLiteral("Radius (pixels)")},
        {"separate", QStringLiteral("Separate axes")},
        {"perChannel", QStringLiteral("Per-channel radius")}};
    extensionRows("bloom.gaussian-blur", "gaussian-blur", blurBase,
                  {"radiusY", "redScale", "greenScale", "blueScale", "alphaScale"});
    auto separate = blurBase;
    separate.emplace_back("radiusY", QStringLiteral("Vertical radius (pixels)"));
    extensionRows("bloom.gaussian-blur", "gaussian-blur", separate,
                  {"redScale", "greenScale", "blueScale", "alphaScale"},
                  {{"separate", true}}, "-separate");
    auto perChannel = blurBase;
    for (const auto& [role, label] :
         {std::pair{"redScale", "Red radius"}, std::pair{"greenScale", "Green radius"},
          std::pair{"blueScale", "Blue radius"}, std::pair{"alphaScale", "Alpha radius"}})
        perChannel.emplace_back(role, QString::fromLatin1(label));
    extensionRows("bloom.gaussian-blur", "gaussian-blur", perChannel, {"radiusY"},
                  {{"perChannel", true}}, "-per-channel");
    // Transform 2D, tall enough to show Skew, Skew axis and Opacity under its motion blur rows.
    extensionRows("bloom.transform-2d", "transform-2d",
                  {{"skew", QStringLiteral("Skew")}, {"opacity", QStringLiteral("Opacity")}});
    extensionRows("bloom.transform-2d", "transform-2d",
                  {{"skew", QStringLiteral("Skew")}, {"opacity", QStringLiteral("Opacity")}}, {},
                  {{"skew", 20.0}, {"opacity", 0.7}}, "-tall", 1500);
}

// #348 Corner Pin, ST Map and Displace in Properties: a row for every corner, choice and value, in
// plain words, each label shown whole and nothing wider than the panel, at the widths of a wide, a
// normal and a narrow panel. With BLOOM_GRAMMAR_ARTIFACT_DIR set, the panel is written at each width
// (suffixed with the device pixel ratio).
void warpRows() {
    struct Spec final {
        std::string_view type;
        std::vector<std::pair<std::string, std::string>> rows;
    };
    const std::vector<Spec> specs{
        {document::kCornerPinNodeType,
         {{"topLeft", "Top Left"}, {"topRight", "Top Right"}, {"bottomRight", "Bottom Right"},
          {"bottomLeft", "Bottom Left"}, {"filter", "Filter"}}},
        {document::kStabilizeNodeType,
         {{"mode", "Mode"}, {"pattern", "Pattern"}, {"search", "Search"}, {"trackPoint", "Track Point"},
          {"tracked", "Tracked Position"}, {"filter", "Filter"}, {"edges", "Edges"}}},
        {document::kStMapNodeType,
         {{"uChannel", "U Channel"}, {"vChannel", "V Channel"}, {"filter", "Filter"},
          {"edges", "Edges"}}},
        {document::kDisplaceNodeType,
         {{"xChannel", "X Channel"}, {"yChannel", "Y Channel"}, {"xAmount", "X Amount"},
          {"yAmount", "Y Amount"}, {"midpoint", "Midpoint"}, {"filter", "Filter"},
          {"edges", "Edges"}}}};
    for (const auto& spec : specs) {
        auto project = document::makeNewProject("Warp", "Main", core::RationalTime::fromInteger(10));
        document::Document document(std::move(project.project));
        commands::CommandStack stack(document);
        ui::CompositionSession session(document, stack, project.initialCompositionId);
        ui::PropertiesEditor properties(session);
        const auto node = addNode(session, spec.type);
        session.selectNode(node);
        const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
        const QString suffix = QGuiApplication::primaryScreen()->devicePixelRatio() == 1.0
                                   ? QStringLiteral("dpr1")
                                   : QStringLiteral("dpr15");
        for (const int width : {2000, 900, 420}) {
            properties.resize(width, 900);
            properties.show();
            QCoreApplication::processEvents();
            for (const auto& [role, name] : spec.rows) {
                auto* found = row(properties, role.c_str());
                expect(found != nullptr && found->isVisibleTo(&properties),
                       "a warp presents every value");
                if (found == nullptr)
                    continue;
                expect(found->property("rowLabel").toString() == QString::fromStdString(name),
                       "a warp row is named in plain words");
                QLabel* shown = nullptr;
                for (auto* candidate : properties.findChildren<QLabel*>("propertiesRowLabel"))
                    if (candidate->text() == QString::fromStdString(name) &&
                        candidate->isVisibleTo(&properties))
                        shown = candidate;
                expect(shown != nullptr &&
                           shown->width() >= shown->fontMetrics().horizontalAdvance(shown->text()),
                       "a warp row label is shown whole");
                for (auto* field : found->findChildren<ui::kit::KValueField*>())
                    if (field->isVisibleTo(&properties))
                        expect(!field->paintedText().contains(QChar(0x2026)) &&
                                   !field->isDisplayRounded(),
                               "a warp value is shown whole");
            }
            if (auto* scroll = properties.findChild<QScrollArea*>())
                expect(scroll->widget()->width() <= scroll->viewport()->width(),
                       "a warp's rows never exceed the panel's width");
            if (!directory.isEmpty()) {
                QDir().mkpath(directory);
                if (!properties.grab().save(
                        directory + QStringLiteral("/") +
                        QString::fromLatin1(spec.type.data(), static_cast<int>(spec.type.size())) +
                        QStringLiteral("-properties-") + QString::number(width) + QLatin1Char('-') +
                        suffix + QStringLiteral(".png")))
                    std::cerr << "could not write the warp Properties capture\n";
            }
        }
    }
}

// #569 audio effects in Properties: a row for every value of every effect, in plain words, each label
// shown whole and nothing wider than the panel, at the widths of a wide, a normal and a narrow panel.
// With BLOOM_GRAMMAR_ARTIFACT_DIR set, the panel is written at each width (suffixed with the device
// pixel ratio).
void audioEffectRows() {
    for (const auto type : {document::kAudioEqualizerNodeType, document::kAudioCompressorNodeType,
                            document::kAudioAutoLevelNodeType, document::kAudioMonoNodeType,
                            document::kAudioHissNodeType}) {
        auto project = document::makeNewProject("Audio", "Main", core::RationalTime::fromInteger(10));
        document::Document document(std::move(project.project));
        commands::CommandStack stack(document);
        ui::CompositionSession session(document, stack, project.initialCompositionId);
        ui::PropertiesEditor properties(session);
        const auto node = addNode(session, type);
        session.selectNode(node);
        const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
        const QString suffix = QGuiApplication::primaryScreen()->devicePixelRatio() == 1.0
                                   ? QStringLiteral("dpr1")
                                   : QStringLiteral("dpr15");
        for (const int width : {2000, 900, 420}) {
            properties.resize(width, 900);
            properties.show();
            QCoreApplication::processEvents();
            for (const auto& spec : document::audioEffectParameters(type)) {
                auto* found = row(properties, std::string(spec.role).c_str());
                expect(found != nullptr && found->isVisibleTo(&properties),
                       "an audio effect presents every value");
                if (found == nullptr)
                    continue;
                const auto label = QString::fromLatin1(spec.label.data(), static_cast<int>(spec.label.size()));
                expect(found->property("rowLabel").toString() == label,
                       "an audio effect row is named in plain words");
                QLabel* shown = nullptr;
                for (auto* candidate : properties.findChildren<QLabel*>("propertiesRowLabel"))
                    if (candidate->text() == label && candidate->isVisibleTo(&properties))
                        shown = candidate;
                expect(shown != nullptr &&
                           shown->width() >= shown->fontMetrics().horizontalAdvance(shown->text()),
                       "an audio effect row label is shown whole");
                for (auto* field : found->findChildren<ui::kit::KValueField*>())
                    if (field->isVisibleTo(&properties))
                        expect(!field->paintedText().contains(QChar(0x2026)) && !field->isDisplayRounded(),
                               "an audio effect value is shown whole");
            }
            if (auto* scroll = properties.findChild<QScrollArea*>())
                expect(scroll->widget()->width() <= scroll->viewport()->width(),
                       "an audio effect's rows never exceed the panel's width");
            if (!directory.isEmpty()) {
                QDir().mkpath(directory);
                if (!properties.grab().save(directory + QStringLiteral("/") +
                                            QString::fromLatin1(type.data(), static_cast<int>(type.size())) +
                                            QStringLiteral("-properties-") + QString::number(width) +
                                            QLatin1Char('-') + suffix + QStringLiteral(".png")))
                    std::cerr << "could not write the audio effect Properties capture\n";
            }
        }
    }
}

// #342, #590, #595, #596, #344, #530: the group nodes, Function, Function Plot, Axes, Axis Tick and the path
// processing nodes present every value in
// plain words at each width, a node that reads a group offers the group chooser, and every label is shown
// whole. With BLOOM_GRAMMAR_ARTIFACT_DIR set, the panel is written at each width (suffixed with the device
// pixel ratio).
void groupAndPlotRows() {
    for (const auto type :
         {document::kNodeGroupNodeType, document::kGroupInputNodeType, document::kGroupOutputNodeType,
          document::kFunctionNodeType, document::kFunctionPlotNodeType, document::kAxesNodeType,
          document::kAxisTickNodeType, document::kTransformPathNodeType, document::kTrimPathNodeType,
          document::kOffsetPathNodeType, document::kBooleanPathsNodeType, document::kMergePathsNodeType,
          document::kRoundCornersNodeType, document::kPathStrokeNodeType,
          document::kResamplePathNodeType}) {
        auto project = document::makeNewProject("Plot", "Main", core::RationalTime::fromInteger(10));
        document::Document document(std::move(project.project));
        commands::CommandStack stack(document);
        ui::CompositionSession session(document, stack, project.initialCompositionId);
        ui::PropertiesEditor properties(session);
        const auto node = addNode(session, type);
        session.selectNode(node);
        const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
        const QString suffix = QGuiApplication::primaryScreen()->devicePixelRatio() == 1.0
                                   ? QStringLiteral("dpr1")
                                   : QStringLiteral("dpr15");
        for (const int width : {2000, 900, 420}) {
            properties.resize(width, 1500);
            properties.show();
            QCoreApplication::processEvents();
            expect((properties.findChild<ui::kit::KDropdown*>("propertiesNodeGroup") != nullptr) ==
                       document::usesGroupReference(type),
                   "exactly the nodes that read a group offer the group chooser");
            for (auto* label : properties.findChildren<QLabel*>("propertiesRowLabel"))
                if (label->isVisibleTo(&properties))
                    expect(label->width() >= label->fontMetrics().horizontalAdvance(label->text()),
                           "a group or plot row label is shown whole");
            if (auto* scroll = properties.findChild<QScrollArea*>())
                expect(scroll->widget()->width() <= scroll->viewport()->width(),
                       "the rows never exceed the panel's width");
            if (!directory.isEmpty()) {
                QDir().mkpath(directory);
                if (!properties.grab().save(directory + QStringLiteral("/") +
                                            QString::fromLatin1(type.data(), static_cast<int>(type.size())) +
                                            QStringLiteral("-properties-") + QString::number(width) +
                                            QLatin1Char('-') + suffix + QStringLiteral(".png")))
                    std::cerr << "could not write the Properties capture\n";
            }
        }
    }
}

// #564 Frame Rate Hold in Properties: the amount and the seed appear with the irregular cadence, the
// seed row carries Randomize seed, and every label is shown whole.
void frameRateHoldRows() {
    auto project = document::makeNewProject("Hold", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    ui::PropertiesEditor properties(session);
    properties.resize(420, 1000);
    properties.show();
    const auto hold = addNode(session, document::kFrameRateHoldNodeType);
    session.selectNode(hold);
    QCoreApplication::processEvents();
    expect(row(properties, "rate") != nullptr && row(properties, "irregular") != nullptr,
           "Frame Rate Hold presents its rate and its irregular cadence");
    const auto shown = [&](const char* role) {
        auto* found = row(properties, role);
        return found != nullptr && found->isVisibleTo(&properties);
    };
    expect(shown("rate") && shown("irregular") && !shown("amount") && !shown("seed"),
           "a regular Frame Rate Hold shows neither the amount nor the seed");
    const auto* node = session.composition()->graph().findNode(hold);
    for (const auto& binding : node->parameters)
        if (binding.role == "irregular")
            expect(session.setParameterValue(binding.parameterId, true, "Set"),
                   "the irregular cadence turns on");
    QCoreApplication::processEvents();
    auto* seed = row(properties, "seed");
    expect(shown("amount") && shown("seed"),
           "the irregular cadence shows the amount and the seed");
    expect(seed != nullptr && seed->findChild<ui::kit::KButton*>("randomiseSeed") != nullptr,
           "the Seed row carries Randomize seed");
    const std::array<std::pair<const char*, const char*>, 4> names{{{"rate", "Frame rate"},
                                                                   {"irregular", "Irregular cadence"},
                                                                   {"amount", "Drop and double"},
                                                                   {"seed", "Seed"}}};
    for (const auto& [role, name] : names) {
        auto* held = row(properties, role);
        expect(held != nullptr && held->property("rowLabel").toString() == QString::fromLatin1(name),
               "a Frame Rate Hold row is named in plain words");
        QLabel* label = nullptr;
        for (auto* candidate : properties.findChildren<QLabel*>("propertiesRowLabel"))
            if (candidate->text() == QString::fromLatin1(name) && candidate->isVisibleTo(&properties))
                label = candidate;
        expect(label != nullptr &&
                   label->width() >= label->fontMetrics().horizontalAdvance(label->text()),
               "a Frame Rate Hold row label is shown whole");
    }
}

// #512 Camera Shake in Properties: the roll amount and the shutter angle are rows like Transform's
// Rotation (a right-aligned label, the dial and the field together, the diamond in the diamond
// column), the Seed row carries Randomize seed, every animatable value has a diamond, and the
// offset and roll outputs read out their values in readable text.
void cameraShakeRows() {
    auto project = document::makeNewProject("Shake", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    ui::PropertiesEditor properties(session);
    properties.resize(700, 1400);
    properties.show();
    const auto shake = addNode(session, document::kCameraShakeNodeType);
    session.selectNode(shake);
    QCoreApplication::processEvents();
    const auto top = [&](QWidget* widget) { return widget->mapTo(&properties, QPoint()).y(); };
    const auto left = [&](QWidget* widget) { return widget->mapTo(&properties, QPoint()).x(); };
    const auto setValue = [&](const char* role, const document::ParameterValue& value) {
        const auto* node = session.composition()->graph().findNode(shake);
        for (const auto& binding : node->parameters)
            if (binding.role == role)
                expect(session.setParameterValue(binding.parameterId, value, "Set"),
                       "a Camera Shake value is set");
        QCoreApplication::processEvents();
    };
    auto* amplitudeX = row(properties, "amplitudeX");
    auto* amplitudeY = row(properties, "amplitudeY");
    auto* roll = row(properties, "rollAmount");
    auto* frequency = row(properties, "frequency");
    expect(amplitudeX && amplitudeY && roll && frequency, "Camera Shake presents its rows");
    if (!amplitudeX || !amplitudeY || !roll || !frequency)
        return;
    auto* dial = roll->findChild<ui::kit::KAngleDial*>();
    auto* field = roll->findChild<ui::kit::KValueField*>();
    expect(dial != nullptr && field != nullptr && left(dial) < left(field) &&
               left(field) - (left(dial) + dial->width()) < 16,
           "the roll amount's dial sits right before its field");
    auto* label = roll->findChild<QLabel*>("propertiesRowLabel");
    auto* otherLabel = amplitudeX->findChild<QLabel*>("propertiesRowLabel");
    expect(label != nullptr && otherLabel != nullptr && (label->alignment() & Qt::AlignRight) &&
               left(label) == left(otherLabel) && label->width() == otherLabel->width(),
           "the roll amount's label is right-aligned in the label column as every other row's");
    auto* diamond = roll->findChild<ui::KeyframeDiamond*>();
    auto* otherDiamond = amplitudeX->findChild<ui::KeyframeDiamond*>();
    expect(diamond != nullptr && otherDiamond != nullptr && left(diamond) == left(otherDiamond),
           "the roll amount's diamond is in the diamond column");
    expect(top(amplitudeY) < top(roll) && top(roll) < top(frequency),
           "the roll amount follows Amplitude Y");
    for (const char* role : {"frequency", "roughness", "smoothing"}) {
        auto* animatable = row(properties, role);
        auto* key = animatable ? animatable->findChild<ui::KeyframeDiamond*>() : nullptr;
        expect(key != nullptr && otherDiamond != nullptr && left(key) == left(otherDiamond),
               "Frequency, Roughness and Smoothing have a keyframe diamond as Amplitude does");
    }
    auto* seed = row(properties, "seed");
    auto* randomise = seed ? seed->findChild<ui::kit::KButton*>("randomiseSeed") : nullptr;
    auto* seedField = seed ? seed->findChild<ui::kit::KValueField*>() : nullptr;
    expect(randomise != nullptr && seedField != nullptr && left(randomise) > left(seedField),
           "Randomize seed sits on the Seed row beside its field");
    // Nothing above the rows: no widget block with a lone dial or button.
    expect(properties.findChild<QWidget*>("propertiesControls") == nullptr ||
               !properties.findChild<QWidget*>("propertiesControls")->isVisible() ||
               properties.findChild<QWidget*>("propertiesControls")->height() == 0,
           "Camera Shake has no controls block above its rows");
    if (randomise != nullptr) {
        const auto before = stack.size();
        randomise->click();
        QCoreApplication::processEvents();
        expect(stack.size() == before + 1, "Randomize seed is one command");
    }
    // The shutter angle is a dial inside the motion blur's rows.
    expect(row(properties, "shutterAngle") == nullptr || !row(properties, "shutterAngle")->isVisible(),
           "the shutter angle waits for the motion blur");
    setValue("motionBlur", true);
    auto* motionBlur = row(properties, "motionBlur");
    auto* shutter = row(properties, "shutterAngle");
    auto* shutterOpens = row(properties, "shutterOffset");
    expect(motionBlur && shutter && shutterOpens && shutter->isVisible() &&
               shutter->findChild<ui::kit::KAngleDial*>() != nullptr &&
               top(motionBlur) < top(shutter) && top(shutter) < top(shutterOpens),
           "the shutter angle is a dial after Motion blur, before Shutter opens");
    // The outputs' values: readable text, no placeholder once resolved.
    QElapsedTimer wait;
    wait.start();
    QLabel* offset = nullptr;
    QLabel* rollOutput = nullptr;
    while (wait.elapsed() < 8000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
        offset = properties.findChild<QLabel*>("propertiesValueOutput.offset");
        rollOutput = properties.findChild<QLabel*>("propertiesValueOutput.roll");
        if (offset && rollOutput && offset->text() != "Resolving…" &&
            rollOutput->text() != "Resolving…")
            break;
    }
    expect(offset != nullptr && rollOutput != nullptr && offset->text() != "Resolving…" &&
               rollOutput->text() != "Resolving…" && !offset->text().isEmpty() &&
               !rollOutput->text().isEmpty(),
           "the offset and roll outputs read out their values");
    if (offset != nullptr)
        expect(offset->palette().color(QPalette::WindowText).lightness() > 128,
               "the output readouts are in readable text");
}


// #344 #530 #349: a selected node's value-output rows are asked for and resolved on a worker, whatever
// the node: no row stays on "Resolving…", each shows the value (or the reason there is none), and a new
// input or time updates it.
struct ReadoutFixture final {
    document::NewProject project =
        document::makeNewProject("Readouts", "Main", core::RationalTime::fromInteger(10));
    document::Document document{std::move(project.project)};
    commands::CommandStack stack{document};
    ui::CompositionSession session{document, stack, project.initialCompositionId};
    ui::PropertiesEditor properties{session};
    ReadoutFixture() {
        properties.resize(900, 1400);
        properties.show();
    }
};

std::map<QString, QString> readoutTexts(QWidget& panel) {
    std::map<QString, QString> texts;
    for (auto* label : panel.findChildren<QLabel*>())
        if (const auto port = label->property("valueOutputPort").toString(); !port.isEmpty())
            texts[port] = label->text();
    return texts;
}

// Waits (bounded) until the node's rows exist and none is still on the placeholder.
std::map<QString, QString> resolvedReadouts(QWidget& panel, const int timeoutMs = 15000) {
    QElapsedTimer wait;
    wait.start();
    std::map<QString, QString> texts;
    while (wait.elapsed() < timeoutMs) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
        texts = readoutTexts(panel);
        if (!texts.empty() &&
            std::ranges::none_of(texts, [](const auto& entry) {
                return entry.second == QStringLiteral("Resolving…") || entry.second.isEmpty();
            }))
            break;
    }
    return texts;
}

void setNodeValue(ui::CompositionSession& session, document::NodeId node, const char* role,
                  const document::ParameterValue& value) {
    const auto id = ui::nodeRoleParameter(session, node, role);
    expect(id && session.setParameterValue(*id, value, "Set"),
           (std::string("a readout fixture value is set: ") + role).c_str());
    QCoreApplication::processEvents();
}

document::PathValue squarePath(const double side) {
    std::vector<document::PathAnchor> anchors;
    for (const auto point : {document::Vec2d{0, 0}, document::Vec2d{side, 0},
                             document::Vec2d{side, side}, document::Vec2d{0, side}})
        anchors.push_back({point, std::nullopt, std::nullopt, document::PathAnchorMode::Corner});
    return document::PathValue::fromContour(std::move(anchors), true);
}

void saveReadoutCapture(QWidget& panel, const char* name) {
    const auto directory = qEnvironmentVariable("BLOOM_READOUT_CAPTURE_DIR");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    panel.grab().save(directory + QLatin1Char('/') + QString::fromLatin1(name) +
                      QStringLiteral(".png"));
}

void pathReadoutsResolve() {
    const QRegularExpression pathText(QStringLiteral("^\\d+ contours, \\d+ anchors$"));
    const std::map<std::string_view, std::vector<const char*>> nodes{
        {document::kTransformPathNodeType, {"path"}},
        {document::kTrimPathNodeType, {"path", "length"}},
        {document::kOffsetPathNodeType, {"path"}},
        {document::kBooleanPathsNodeType, {"path"}},
        {document::kMergePathsNodeType, {"path"}},
        {document::kRoundCornersNodeType, {"path"}},
        {document::kPathStrokeNodeType, {"path", "core", "length"}}};
    for (const auto& [type, ports] : nodes) {
        ReadoutFixture fixture;
        const auto node = addNode(fixture.session, type);
        fixture.session.selectNode(node);
        if (type != document::kBooleanPathsNodeType)
            setNodeValue(fixture.session, node, "path", squarePath(100.0));
        else {
            setNodeValue(fixture.session, node, "first", squarePath(100.0));
            setNodeValue(fixture.session, node, "second", squarePath(50.0));
        }
        const auto texts = resolvedReadouts(fixture.properties);
        const std::string name(type);
        for (const auto* port : ports) {
            const auto found = texts.find(QString::fromLatin1(port));
            const auto shown = found == texts.end() ? QString() : found->second;
            expect(found != texts.end() && shown != QStringLiteral("Resolving…") && !shown.isEmpty(),
                   (name + " " + port + " leaves the placeholder: " + shown.toStdString()).c_str());
            if (std::string_view(port) == "length")
                expect(shown == QStringLiteral("400") || shown.startsWith(QStringLiteral("400.")) ||
                           shown.startsWith(QStringLiteral("399.99")),
                       (name + " length is the square's perimeter, 400 px: " + shown.toStdString())
                           .c_str());
            else
                expect(pathText.match(shown).hasMatch(),
                       (name + " " + port + " reads as a path: " + shown.toStdString()).c_str());
        }
        if (type == document::kTransformPathNodeType || type == document::kRoundCornersNodeType) {
            const auto path = texts.find(QStringLiteral("path"));
            expect(path != texts.end() &&
                       (type == document::kRoundCornersNodeType ||
                        path->second == QStringLiteral("1 contours, 4 anchors")),
                   "Transform Path of a square reads 1 contours, 4 anchors");
        }
        // The operand's own summary reads whole ("4 anchors · Closed"), not cut to its first word.
        for (auto* summary : fixture.properties.findChildren<QLabel*>("propertiesPathSummary"))
            if (summary->isVisibleTo(&fixture.properties)) {
                expect(summary->text() == QStringLiteral("4 anchors · Closed") ||
                           summary->text() == QStringLiteral("4 anchors · Open"),
                       ("the path operand summary reads whole: " + summary->text().toStdString()).c_str());
                expect(summary->width() >= summary->sizeHint().width(),
                       ("the path operand summary is not cut: " + std::string(type)).c_str());
            }
        if (type == document::kTrimPathNodeType) {
            saveReadoutCapture(fixture.properties, "properties-trim-path");
            // A new input updates the readout.
            setNodeValue(fixture.session, node, "path", squarePath(200.0));
            QElapsedTimer wait;
            wait.start();
            QString length;
            while (wait.elapsed() < 15000) {
                QCoreApplication::processEvents();
                QThread::msleep(5);
                const auto again = readoutTexts(fixture.properties);
                length = again.contains(QStringLiteral("length")) ? again.at(QStringLiteral("length"))
                                                                  : QString();
                if (length.startsWith(QStringLiteral("800")) || length.startsWith(QStringLiteral("799.99")))
                    break;
            }
            expect(length.startsWith(QStringLiteral("800")) || length.startsWith(QStringLiteral("799.99")),
                   ("Trim Path length follows its path: " + length.toStdString()).c_str());
        }
        if (type == document::kOffsetPathNodeType || type == document::kPathStrokeNodeType)
            saveReadoutCapture(fixture.properties, type == document::kOffsetPathNodeType
                                                       ? "properties-offset-path"
                                                       : "properties-path-stroke");
    }
}

// #509: Path Stroke's stamps in Properties (Brush stamp, Stamp direction, a Stamp Rotation dial),
// and the node Mask Path and Text Outline read offered by name, with their readouts resolved.
void pathStampAndSourceRows() {
    ReadoutFixture fixture;
    auto& session = fixture.session;
    auto& properties = fixture.properties;
    const auto labelsWhole = [&](const char* what) {
        for (auto* label : properties.findChildren<QLabel*>("propertiesRowLabel"))
            if (label->isVisibleTo(&properties))
                expect(label->width() >= label->fontMetrics().horizontalAdvance(label->text()),
                       (std::string(what) +
                        ": a row label is shown whole: " + label->text().toStdString())
                           .c_str());
    };
    const auto stroke = addNode(session, document::kPathStrokeNodeType);
    session.selectNode(stroke);
    setNodeValue(session, stroke, "path", squarePath(100.0));
    setNodeValue(session, stroke, "spacing", 20.0);
    const auto selectorIn = [&](const char* role) {
        auto* found = row(properties, role);
        return found ? found->findChild<ui::kit::KDropdown*>("propertiesRegistryEnum") : nullptr;
    };
    auto* stamp = selectorIn("stamp");
    expect(stamp != nullptr && stamp->count() == 4 && stamp->itemText(0) == QStringLiteral("Dot") &&
               stamp->itemText(1) == QStringLiteral("Square") &&
               stamp->itemText(2) == QStringLiteral("Diamond") &&
               stamp->itemText(3) == QStringLiteral("Dash") &&
               stamp->currentText() == QStringLiteral("Dot"),
           "Brush stamp offers Dot (the default), Square, Diamond and Dash");
    auto* align = selectorIn("stampAlign");
    expect(align != nullptr && align->count() == 3 &&
               align->currentText() == QStringLiteral("Automatic") &&
               align->itemText(1) == QStringLiteral("Follow path") &&
               align->itemText(2) == QStringLiteral("Fixed"),
           "Stamp direction offers Automatic (the default), Follow path and Fixed");
    auto* rotation = row(properties, "stampRotation");
    auto* rotationDial = rotation ? rotation->findChild<ui::kit::KAngleDial*>() : nullptr;
    auto* rotationField = rotation ? rotation->findChild<ui::kit::KValueField*>() : nullptr;
    expect(rotationDial != nullptr && rotationField != nullptr &&
               rotationField->unit() == QStringLiteral("\u00b0"),
           "Stamp Rotation is an angle in degrees with a dial on its own row");
    if (stamp != nullptr) {
        stamp->chooseIndex(3);
        QCoreApplication::processEvents();
        const auto id = ui::nodeRoleParameter(session, stroke, "stamp");
        const auto* record = id ? session.composition()->parameters().find(*id) : nullptr;
        expect(record != nullptr &&
                   std::get<std::int64_t>(
                       std::get<document::ConstantValueSource>(record->source).value) == 3,
               "choosing Dash stores the Dash stamp through a command");
        expect(session.undo(), "the stamp choice undoes");
        expect(session.redo(), "the stamp choice redoes");
    }
    const auto strokeTexts = resolvedReadouts(properties);
    expect(strokeTexts.contains(QStringLiteral("path")) &&
               strokeTexts.at(QStringLiteral("path")).endsWith(QStringLiteral("anchors")),
           "Path Stroke's stamped outline resolves in its readout");
    labelsWhole("Path Stroke");
    saveReadoutCapture(properties, "properties-path-stroke-stamps");

    // Mask Path: nothing to read until a mask exists, then the Mask node by name.
    const auto maskPath = addNode(session, document::kMaskPathNodeType);
    session.selectNode(maskPath);
    QCoreApplication::processEvents();
    const auto sourceIn = [&]() {
        auto* found = row(properties, "source");
        return found ? found->findChild<ui::kit::KDropdown*>("propertiesPathSource") : nullptr;
    };
    auto* source = sourceIn();
    expect(source != nullptr && source->count() == 1 &&
               source->currentText() == QStringLiteral("No masked layer or Mask node"),
           "Mask Path with no mask to read says so");
    const auto mask = addNode(session, document::kMaskNodeType);
    setNodeValue(session, mask, "path", squarePath(80.0));
    session.selectNode(maskPath);
    QCoreApplication::processEvents();
    source = sourceIn();
    expect(source != nullptr && source->count() == 1 &&
               source->currentText() ==
                   QStringLiteral("Mask node %1").arg(static_cast<qulonglong>(mask.value())),
           "Mask Path offers the Mask node and reads it");
    auto texts = resolvedReadouts(properties);
    expect(texts.contains(QStringLiteral("path")) &&
               texts.at(QStringLiteral("path")) == QStringLiteral("1 contours, 4 anchors") &&
               texts.contains(QStringLiteral("count")) &&
               texts.at(QStringLiteral("count")) == QStringLiteral("1"),
           "Mask Path reads the Mask node's one contour");
    labelsWhole("Mask Path");
    saveReadoutCapture(properties, "properties-mask-path");

    // Text Outline: a text layer is offered by what it says.
    commands::Transaction text("Text", session.snapshot().revision());
    text.emplace<commands::AddTextLayer>(session.compositionId(), "Title", "Bloom");
    expect(session.executeNodeTransaction(std::move(text)).changed(), "a text layer is added");
    const auto outline = addNode(session, document::kTextOutlineNodeType);
    session.selectNode(outline);
    QCoreApplication::processEvents();
    source = sourceIn();
    expect(source != nullptr && source->count() == 1 &&
               source->currentText() == QStringLiteral("Text \u201cBloom\u201d"),
           "Text Outline offers the text by what it says and reads it");
    texts = resolvedReadouts(properties);
    expect(texts.contains(QStringLiteral("count")) &&
               texts.at(QStringLiteral("count")).toInt() >= 5,
           "Text Outline's glyph contours resolve in its readout");
    labelsWhole("Text Outline");
    saveReadoutCapture(properties, "properties-text-outline");

    // Alpha Contour: its band's controls and the composite choice, each label whole.
    const auto contour = addNode(session, document::kAlphaContourNodeType);
    session.selectNode(contour);
    QCoreApplication::processEvents();
    std::map<QString, QString> contourRows;
    for (auto* row : properties.findChildren<QWidget*>(QStringLiteral("propertiesRegistryRow")))
        if (!row->isHidden())
            contourRows[row->property("role").toString()] = row->property("rowLabel").toString();
    expect(contourRows[QStringLiteral("width")] == QStringLiteral("Width") &&
               contourRows[QStringLiteral("softness")] == QStringLiteral("Softness") &&
               contourRows[QStringLiteral("offset")] == QStringLiteral("Offset") &&
               contourRows[QStringLiteral("color")] == QStringLiteral("Color") &&
               contourRows[QStringLiteral("composite")] == QStringLiteral("Composite"),
           "Alpha Contour presents Width, Softness, Offset, Color and Composite");
    // The colour chip shows the band's colour once the session's colour conversion is ready (it
    // is prepared off the UI thread; until then the chip is disabled and says so).
    const auto chipReady = [&properties] {
        const auto chips = properties.findChildren<ui::kit::KColorChip*>(
            QStringLiteral("propertiesRegistryColor"));
        return !chips.isEmpty() && std::ranges::all_of(chips, [](const auto* chip) {
            return chip->isHidden() || chip->isEnabled();
        });
    };
    QElapsedTimer conversion;
    conversion.start();
    while (!chipReady() && conversion.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    expect(chipReady(), "Alpha Contour's colour chip shows its colour");
    labelsWhole("Alpha Contour");
    saveReadoutCapture(properties, "properties-alpha-contour");
}

void imageStatisticsReadoutsResolve() {
    ReadoutFixture fixture;
    const auto solid = addNode(fixture.session, document::kSolidSourceNodeType);
    const auto stats = addNode(fixture.session, document::kImageStatisticsNodeType);
    // Before an image is connected, the rows say why instead of waiting.
    fixture.session.selectNode(stats);
    auto texts = resolvedReadouts(fixture.properties);
    expect(texts.size() == 5, "Image Statistics presents its five outputs");
    for (const auto& [port, text] : texts)
        expect(text != QStringLiteral("Resolving…") && !text.isEmpty(),
               ("an unconnected Image Statistics row says something: " + port.toStdString()).c_str());
    connectNodes(fixture.session, solid, stats, "input");
    setNodeValue(fixture.session, solid, "color", core::Color4d{0.5, 0.5, 0.5, 1.0});
    const auto& format = fixture.session.composition()->format();
    setNodeValue(fixture.session, solid, "width", static_cast<double>(format.width()));
    setNodeValue(fixture.session, solid, "height", static_cast<double>(format.height()));
    const auto expectedCount = static_cast<double>(format.width()) * format.height();
    const auto waitFor = [&](const char* what, const std::function<bool(const std::map<QString, QString>&)>& done) {
        QElapsedTimer wait;
        wait.start();
        std::map<QString, QString> current;
        while (wait.elapsed() < 20000) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
            current = readoutTexts(fixture.properties);
            if (current.size() == 5 && done(current))
                break;
        }
        expect(current.size() == 5 && done(current), what);
        if (!(current.size() == 5 && done(current)))
            for (const auto& [port, text] : current)
                std::cerr << "  " << port.toStdString() << " = " << text.toStdString() << '\n';
        return current;
    };
    texts = waitFor("Image Statistics of a grey Solid reads out its five numbers",
                    [&](const auto& t) {
                        return std::abs(t.at("count").toDouble() - expectedCount) < 0.5 &&
                               std::abs(t.at("mean").toDouble() - 0.5) < 1e-4;
                    });
    saveReadoutCapture(fixture.properties, "properties-image-statistics");
    expect(std::abs(texts["minimum"].toDouble() - 0.5) < 1e-4 &&
               std::abs(texts["maximum"].toDouble() - 0.5) < 1e-4 &&
               std::abs(texts["deviation"].toDouble()) < 1e-4,
           "a flat grey has equal minimum, maximum and mean, and no deviation");
    for (const auto& [port, text] : texts)
        expect(text != QStringLiteral("Resolving…"), "no Image Statistics row stays on the placeholder");
    // A new input updates the readout.
    setNodeValue(fixture.session, solid, "color", core::Color4d{0.25, 0.25, 0.25, 1.0});
    waitFor("Image Statistics follows its input", [&](const auto& t) {
        return std::abs(t.at("mean").toDouble() - 0.25) < 1e-4;
    });
}

void timeUpdatesReadout() {
    ReadoutFixture fixture;
    const auto frame = addNode(fixture.session, document::kFrameNumberNodeType);
    fixture.session.selectNode(frame);
    auto texts = resolvedReadouts(fixture.properties);
    expect(texts.size() == 1 && texts.begin()->second == QStringLiteral("0"),
           "Frame Number reads 0 at the start");
    (void)fixture.session.setCurrentTime(core::RationalTime::fromInteger(3));
    QElapsedTimer wait;
    wait.start();
    QString text;
    while (wait.elapsed() < 15000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
        texts = readoutTexts(fixture.properties);
        text = texts.empty() ? QString() : texts.begin()->second;
        if (text == QStringLiteral("72"))
            break;
    }
    expect(text == QStringLiteral("72"), ("Frame Number follows the playhead: " + text.toStdString()).c_str());
}

// An Integer or Boolean output is read like every other: each node of that kind shows its actual value,
// and no such output of any built-in node says "Unavailable" when its inputs are the defaults.
void integerAndBooleanReadouts() {
    const auto single = [](std::string_view type, const std::function<void(ReadoutFixture&, document::NodeId)>& prepare,
                           const char* port, const QString& expected, const char* what) {
        ReadoutFixture fixture;
        const auto node = addNode(fixture.session, type);
        fixture.session.selectNode(node);
        prepare(fixture, node);
        QElapsedTimer wait;
        wait.start();
        QString text;
        while (wait.elapsed() < 15000) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
            const auto texts = readoutTexts(fixture.properties);
            text = texts.contains(QString::fromLatin1(port)) ? texts.at(QString::fromLatin1(port))
                                                             : QString();
            if (text == expected)
                break;
        }
        expect(text == expected, (std::string(what) + ": expected " + expected.toStdString() +
                                  ", read " + text.toStdString())
                                     .c_str());
    };
    single(document::kIntegerValueNodeType,
           [](ReadoutFixture& f, document::NodeId n) { setNodeValue(f.session, n, "value", std::int64_t{7}); },
           "value", QStringLiteral("7"), "Integer value node reads its integer");
    single(document::kBooleanValueNodeType,
           [](ReadoutFixture& f, document::NodeId n) { setNodeValue(f.session, n, "value", true); },
           "value", QStringLiteral("True"), "Boolean value node reads True");
    single(document::kBooleanValueNodeType, [](ReadoutFixture&, document::NodeId) {}, "value",
           QStringLiteral("False"), "Boolean value node reads False by default");
    single(document::kIntegerSwitchNodeType,
           [](ReadoutFixture& f, document::NodeId n) {
               setNodeValue(f.session, n, "condition", true);
               setNodeValue(f.session, n, "ifFalse", std::int64_t{3});
               setNodeValue(f.session, n, "ifTrue", std::int64_t{9});
           },
           "result", QStringLiteral("9"), "Integer Switch reads the branch its condition picks");
    single(document::kBooleanSwitchNodeType,
           [](ReadoutFixture& f, document::NodeId n) {
               setNodeValue(f.session, n, "condition", false);
               setNodeValue(f.session, n, "ifFalse", true);
               setNodeValue(f.session, n, "ifTrue", false);
           },
           "result", QStringLiteral("True"), "Boolean Switch reads the false branch");
    single(document::kCompareNodeType,
           [](ReadoutFixture& f, document::NodeId n) {
               setNodeValue(f.session, n, "a", 1.0);
               setNodeValue(f.session, n, "b", 2.0);
               setNodeValue(f.session, n, "operation",
                            document::compareOperationStoredValue(document::CompareOperation::Less));
           },
           "result", QStringLiteral("True"), "Compare 1 < 2 reads True");
    single(document::kCompareNodeType,
           [](ReadoutFixture& f, document::NodeId n) {
               setNodeValue(f.session, n, "a", 3.0);
               setNodeValue(f.session, n, "b", 2.0);
               setNodeValue(f.session, n, "operation",
                            document::compareOperationStoredValue(document::CompareOperation::Less));
           },
           "result", QStringLiteral("False"), "Compare 3 < 2 reads False");
    int checked = 0;
    for (const auto& definition : document::builtInNodeDefinitions().definitions()) {
        const bool integerOrBoolean = std::ranges::any_of(definition.outputs, [](const auto& output) {
            return output.valueKind == document::SocketValueKind::Integer ||
                   output.valueKind == document::SocketValueKind::Boolean;
        });
        if (!integerOrBoolean || definition.lowering == document::NodeLoweringKind::NodeGroup ||
            definition.lowering == document::NodeLoweringKind::GroupBoundary)
            continue;
        ReadoutFixture fixture;
        const auto node = addNode(fixture.session, definition.key.typeId);
        if (!node.isValid())
            continue;
        fixture.session.selectNode(node);
        const auto texts = resolvedReadouts(fixture.properties);
        for (const auto& output : definition.outputs) {
            if (output.valueKind != document::SocketValueKind::Integer &&
                output.valueKind != document::SocketValueKind::Boolean)
                continue;
            ++checked;
            const auto found = texts.find(QString::fromStdString(output.name));
            expect(found != texts.end() && found->second != QStringLiteral("Unavailable") &&
                       found->second != QStringLiteral("Resolving…") && !found->second.isEmpty(),
                   ("an Integer/Boolean output shows a value: " + definition.key.typeId + " " +
                    output.name + " = " +
                    (found == texts.end() ? std::string("<missing>") : found->second.toStdString()))
                       .c_str());
        }
    }
    expect(checked >= 10, "the Integer/Boolean audit visits the nodes that have such outputs");
}

// `--capture-readouts`: Properties pictures of nodes whose readouts have resolved, cropped to their
// content so every row is in the picture (BLOOM_LABEL_CAPTURE_DIR names the folder,
// BLOOM_LABEL_CAPTURE_SCALE the device pixel ratio the run was started with).
void captureResolvedProperties() {
    const auto directory = qEnvironmentVariable("BLOOM_LABEL_CAPTURE_DIR");
    if (directory.isEmpty())
        return;
    QDir().mkpath(directory);
    const auto scale = qEnvironmentVariable("BLOOM_LABEL_CAPTURE_SCALE");
    for (const auto type : {document::kCameraShakeNodeType, document::kTransform2DNodeType}) {
        ReadoutFixture fixture;
        fixture.properties.resize(900, 2600);
        const auto node = addNode(fixture.session, type);
        fixture.session.selectNode(node);
        const auto texts = resolvedReadouts(fixture.properties);
        if (std::string_view(type) == document::kCameraShakeNodeType)
            for (const auto& [port, text] : texts)
                expect(text != QStringLiteral("Resolving…") && !text.isEmpty(),
                       ("Camera Shake readout resolved before its capture: " + port.toStdString()).c_str());
        QCoreApplication::processEvents();
        auto image = fixture.properties.grab().toImage();
        const auto background = image.pixel(image.width() - 1, image.height() - 1);
        int last = 0;
        for (int y = image.height() - 1; y >= 0 && last == 0; --y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixel(x, y) != background) {
                    last = y;
                    break;
                }
        image = image.copy(0, 0, image.width(), std::min(image.height(), last + 16));
        image.save(directory + QLatin1Char('/') + QString::fromLatin1(type.data(), static_cast<int>(type.size())) +
                   QStringLiteral("-900-dpr") + scale + QStringLiteral("-resolved.png"));
    }
}

// Every node that shows a value-output row resolves it: the request comes from the predicate that draws
// the rows, so no built-in can be left on the placeholder.
void everyValueOutputResolves() {
    int checked = 0;
    for (const auto& definition : document::builtInNodeDefinitions().definitions()) {
        ReadoutFixture fixture;
        const auto node = addNode(fixture.session, definition.key.typeId);
        if (!node.isValid())
            continue;
        fixture.session.selectNode(node);
        QCoreApplication::processEvents();
        if (readoutTexts(fixture.properties).empty())
            continue;
        ++checked;
        const auto texts = resolvedReadouts(fixture.properties);
        for (const auto& [port, text] : texts)
            expect(text != QStringLiteral("Resolving…") && !text.isEmpty(),
                   ("a value-output row is still unresolved: " + definition.key.typeId + " " +
                    port.toStdString() + " = " + text.toStdString())
                       .c_str());
    }
    expect(checked > 20, "the value-output audit visits the value nodes");
}

// #587: Properties shows only the chosen Test Card's rows, and a Patch's readout keeps the height
// its text needs once the expected value arrives, so nothing under it is clipped or overlapped.
void testCardRows() {
    auto project = document::makeNewProject("Card", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* properties = new ui::PropertiesEditor(session, &window);
    layout->addWidget(properties);
    window.resize(420, 900);
    window.show();
    const auto shown = [&](const char* role) {
        auto* found = row(*properties, role);
        return found != nullptr && found->isVisibleTo(&window);
    };
    const auto node = addNode(session, "bloom.test-card");
    session.selectNode(node);
    QCoreApplication::processEvents();
    expect(shown("card") && !shown("patchValue") && !shown("gridSpacing"),
           "a Test Card shows only its card and size at first");
    const auto card = ui::nodeRoleParameter(session, node, "card");
    expect(card && session.setParameterValue(
                       *card, std::int64_t{static_cast<std::int64_t>(document::TestCard::Patch)},
                       QStringLiteral("Set")),
           "the Test Card becomes a Patch");
    auto* readout = properties->findChild<ui::TestCardCardWidget*>();
    QElapsedTimer wait;
    wait.start();
    while ((readout == nullptr || readout->expectedText().startsWith(QStringLiteral("Expected display value: w")) ||
            readout->expectedText().isEmpty()) &&
           wait.elapsed() < 5'000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        readout = properties->findChild<ui::TestCardCardWidget*>();
    }
    QApplication::processEvents();
    expect(readout != nullptr && shown("patchValue") && !shown("gridSpacing"),
           "a Patch shows its own rows");
    expect(readout != nullptr && readout->height() >= readout->heightAt(readout->width()),
           "the Patch readout is as tall as its text needs");
}

// #599 A Line Shape Source in Properties: its two ends are position rows (an X and a Y field each,
// a keyframe diamond like every animatable value), named in plain words, shown whole at the widths
// of a wide, a normal and a narrow panel. Another kind shows neither. With
// BLOOM_GRAMMAR_ARTIFACT_DIR set, the panel is written at each width (suffixed with the device
// pixel ratio).
void lineEndRows() {
    auto project = document::makeNewProject("Line", "Main", core::RationalTime::fromInteger(10));
    document::Document document(std::move(project.project));
    commands::CommandStack stack(document);
    ui::CompositionSession session(document, stack, project.initialCompositionId);
    expect(session.addShapeLayer(document::ShapeKind::Line,
                                 {.position = document::Vec2d{100, 100},
                                  .size = {},
                                  .lineStart = document::Vec2d{-50, 10},
                                  .lineEnd = document::Vec2d{50, 20},
                                  .path = {}}),
           "add a Line");
    ui::PropertiesEditor properties(session);
    const auto directory = qEnvironmentVariable("BLOOM_GRAMMAR_ARTIFACT_DIR");
    const QString suffix = QGuiApplication::primaryScreen()->devicePixelRatio() == 1.0
                               ? QStringLiteral("dpr1")
                               : QStringLiteral("dpr15");
    for (const int width : {900, 420}) {
        properties.resize(width, 1700);
        properties.show();
        QCoreApplication::processEvents();
        for (const auto& [role, text] : {std::pair{"lineStart", QStringLiteral("Line Start")},
                                         std::pair{"lineEnd", QStringLiteral("Line End")}}) {
            auto* found = row(properties, role);
            expect(found != nullptr && found->isVisibleTo(&properties),
                   "a Line presents both ends");
            if (found == nullptr)
                continue;
            expect(found->property("rowLabel").toString() == text,
                   "a Line end is named in plain words");
            QLabel* label = nullptr;
            for (auto* candidate : properties.findChildren<QLabel*>("propertiesRowLabel"))
                if (candidate->text() == text && candidate->isVisibleTo(&properties))
                    label = candidate;
            expect(label != nullptr &&
                       label->width() >= label->fontMetrics().horizontalAdvance(label->text()),
                   "a Line end label is shown whole");
            int fields = 0;
            for (auto* field : found->findChildren<ui::kit::KValueField*>())
                if (field->isVisibleTo(&properties)) {
                    ++fields;
                    expect(!field->paintedText().contains(QChar(0x2026)) &&
                               !field->isDisplayRounded(),
                           "a Line end value is shown whole");
                }
            expect(fields == 2, "a Line end is an X and a Y position field");
            expect(found->findChild<ui::KeyframeDiamond*>() != nullptr,
                   "a Line end has a keyframe diamond");
        }
        if (auto* scroll = properties.findChild<QScrollArea*>())
            expect(scroll->widget()->width() <= scroll->viewport()->width(),
                   "a Line's rows never exceed the panel's width");
        if (!directory.isEmpty()) {
            QDir().mkpath(directory);
            if (!properties.grab().save(directory + QStringLiteral("/line-ends-properties-") +
                                        QString::number(width) + QLatin1Char('-') + suffix +
                                        QStringLiteral(".png")))
                std::cerr << "could not write the Line Properties capture\n";
        }
    }
    // Keying the end through its diamond turns the row animated; another kind has no such rows.
    const auto* end = session.parameterForSelection("lineEnd");
    expect(end != nullptr && session.toggleKeyframeForParameter(end->id),
           "a Line end keys through its diamond");
    expect(session.addShapeLayer(document::ShapeKind::Rectangle,
                                 {.position = document::Vec2d{100, 100},
                                  .size = document::Vec2d{40, 40},
                                  .lineStart = {},
                                  .lineEnd = {},
                                  .path = {}}),
           "add a Rectangle");
    QCoreApplication::processEvents();
    expect(row(properties, "lineEnd") == nullptr || !row(properties, "lineEnd")->isVisibleTo(&properties),
           "a Rectangle shows no line ends");
}

int main(int argc, char** argv) try {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("BloomPropertiesRegistryTest");
    QSettings().clear();
    if (argc > 1 && std::string_view(argv[1]) == "--readouts") {
        pathReadoutsResolve();
        pathStampAndSourceRows();
        imageStatisticsReadoutsResolve();
        timeUpdatesReadout();
        integerAndBooleanReadouts();
        QSettings().clear();
        return failures ? 1 : 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--capture-readouts") {
        captureResolvedProperties();
        QSettings().clear();
        return failures ? 1 : 0;
    }
    // `--label-audit` runs only the #582 label audit, which is what the second device pixel ratio
    // registers: the same audit over every built-in node at the same widths and assertions.
    if (argc > 1 && std::string_view(argv[1]) == "--label-audit") {
        everyBuiltInLabelIsShownWhole();
        QSettings().clear();
        return failures ? 1 : 0;
    }
    // First: it must be the first font row in the process, so the system catalogue arrives (and
    // the selector is rebuilt) while it watches.
    fontCatalogueRefreshIsNotAnEdit();
    genericKinds();
    scaledProjectionGestureKeepsItsUnit();
    registryRows();
    upstreamRows();
    drivenTextContent();
    colourEffectRows();
    discreteImageRows();
    transform2DRows();
    frameRateHoldRows();
    autoExposureRows();
    colorCleanupRows();
    edgeExtendRows();
    lineEndRows();
    extensionPropertiesRows();
    warpRows();
    audioEffectRows();
    groupAndPlotRows();
    cameraShakeRows();
    matteBlurRows();
    matteKeyRows();
    recipeRows();
    colorGradeRows();
    imageChannelRows();
    widthRule();
    everyBuiltInLabelIsShownWhole();
    videoRows();
    testCardRows();
    pathReadoutsResolve();
    pathStampAndSourceRows();
    imageStatisticsReadoutsResolve();
    timeUpdatesReadout();
    integerAndBooleanReadouts();
    everyValueOutputResolves();
    QSettings().clear();
    return failures ? 1 : 0;
}

catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
}
