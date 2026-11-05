#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGraphicsItem>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStringList>
#include <QThread>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/curve_graph.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/color_chip.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/tab_bar.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <iostream>
#include <limits>
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
    bool elided = false;
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
            elided = elided || label->text().endsWith(QChar(0x2026));
    }
    expect(elided, "narrow labels elide instead of stretching viewport");
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
        auto* dropdown = font->findChild<ui::kit::KDropdown*>("propertiesRegistryEnum");
        expect(dropdown != nullptr && dropdown->count() == 4 &&
                   dropdown->currentText() == QStringLiteral("DejaVu Sans"),
               "Font is a generic Properties dropdown with the four faces in face order");
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
                    "mode", "exponent", "radiusRed", "radiusGreen", "radiusBlue",
                    "points", "angle", "length", "chromatic", "dirtAmount", "rollOff"},
                   "glowOnly"},
          Expected{"bloom.light-wrap", {"radius", "amount"}, "wrapOnly"},
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

int main(int argc, char** argv) try {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("BloomPropertiesRegistryTest");
    QSettings().clear();
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
    matteBlurRows();
    matteKeyRows();
    recipeRows();
    colorGradeRows();
    imageChannelRows();
    widthRule();
    videoRows();
    QSettings().clear();
    return failures ? 1 : 0;
}

catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
}
