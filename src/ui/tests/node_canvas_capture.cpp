// Review captures of the Node editor (task N1), maximized at about 2000 and 900 px wide: an
// arranged multi-layer graph, collapsed cards, a coloured group frame, a Merge with three inputs,
// the canvas at 25%, the footer and every Fields card (#525). A tool, not a test:
//   bloom_node_canvas_capture <directory> <1|1.5> [path-socket]
// `path-socket` takes only the Path socket (#589) beside its neighbors, at 2000, 900 and 480 wide.
#include "window_fixture.hpp"

#include "node_editor_items.hpp"

#include <QDir>
#include <QImage>
#include <bloom/commands/node_layout_arrange.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/document/field_nodes.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace bloom;
using namespace bloom::ui;

int gFailures = 0;
QString gDirectory;
QString gRatio;

void settle(const int ms = 80) {
    QApplication::processEvents();
    QTest::qWait(ms);
}

void save(const QImage& image, const QString& name) {
    const auto path = gDirectory + QLatin1Char('/') + name + QLatin1Char('-') + gRatio +
                      QStringLiteral(".png");
    if (image.isNull() || !image.save(path)) {
        std::cerr << "could not write " << path.toStdString() << '\n';
        ++gFailures;
        return;
    }
    std::cout << path.toStdString() << ' ' << image.width() << 'x' << image.height() << '\n';
}

EditorArea* nodesArea(MainWindow& window) {
    for (auto* area : window.findChildren<EditorArea*>())
        if (area->editorId() == "bloom.nodes")
            return area;
    return nullptr;
}

void sizeArea(MainWindow& window, EditorArea& area, const QSize target) {
    for (int pass = 0; pass < 4; ++pass) {
        QApplication::processEvents();
        const QSize delta = target - area.size();
        if (delta.isNull())
            return;
        window.resize(window.size() + delta);
        QTest::qWait(30);
    }
}

bool run(CompositionSession& session, commands::Transaction&& transaction) {
    const std::string label(transaction.label());
    const auto result = session.executeNodeTransaction(std::move(transaction));
    if (!result.succeeded()) {
        std::cerr << "fixture transaction refused: " << label;
        if (!result.operationFailures.empty())
            std::cerr << ": " << result.operationFailures.front().issue.message;
        std::cerr << '\n';
        ++gFailures;
    }
    settle(20);
    return result.succeeded();
}

int capture(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomGrammarTests");
    app.setApplicationName("NodeCanvasCapture");
    kit::installKinetikTheme(app);
    gDirectory = QString::fromLocal8Bit(argv[1]);
    gRatio = QString::fromLatin1(argv[2]) == QStringLiteral("1") ? QStringLiteral("dpr1")
                                                                 : QStringLiteral("dpr15");
    QDir().mkpath(gDirectory);
    test::WindowFixture fixture;
    auto& session = fixture.session;
    const auto composition = session.compositionId();

    // The fixture's sample graph goes first, so the Merge below carries exactly three layers.
    {
        std::set<document::NodeId> sample;
        for (const auto& node : session.composition()->graph().nodes())
            if (node.typeId != document::kLayerStackNodeType &&
                node.typeId != document::kCompositionOutputNodeType)
                sample.insert(node.id);
        if (!sample.empty()) {
            commands::Transaction clear("Clear Sample", session.snapshot().revision());
            clear.emplace<commands::RemoveNodes>(composition, sample);
            run(session, std::move(clear));
        }
    }
    // Three layers; the middle one with a blur between its source and its Layer card, and a
    // Scalar driving the top layer's opacity, so Arrange has an upstream chain and a driver.
    std::vector<document::NodeId> layers;
    std::vector<document::NodeId> sources;
    for (const auto* name : {"Background", "Plate", "Title"}) {
        commands::Transaction add("Add Layer", session.snapshot().revision());
        add.emplace<commands::AddSolidLayer>(composition, name, core::Color4d{0.3, 0.5, 0.7, 1.0});
        const auto result = session.executeNodeTransaction(std::move(add));
        if (const auto layer = result.outputId<document::NodeId>("layerOutputNode"))
            layers.push_back(*layer);
        if (const auto source = result.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput))
            sources.push_back(*source);
    }
    if (layers.size() != 3 || sources.size() != 3)
        return 1;
    document::NodeId blurNode;
    {
        commands::Transaction blur("Add Blur", session.snapshot().revision());
        blur.emplace<commands::AddNode>(composition, "bloom.gaussian-blur", document::Vec2d{0, 0});
        const auto added = session.executeNodeTransaction(std::move(blur));
        const auto node = added.outputId<document::NodeId>(commands::kAddNodeOutput);
        if (!node)
            return 1;
        blurNode = *node;
        commands::Transaction wire("Wire Blur", session.snapshot().revision());
        wire.emplace<commands::ConnectPorts>(composition, document::OutputPortRef{sources[1], "image"},
                                             document::NodeInputRef{*node, "input"});
        wire.emplace<commands::ConnectPorts>(composition, document::OutputPortRef{*node, "image"},
                                             document::NodeInputRef{layers[1], "image"});
        run(session, std::move(wire));
    }
    {
        commands::Transaction scalar("Add Scalar", session.snapshot().revision());
        scalar.emplace<commands::AddNode>(composition, std::string(document::kScalarValueNodeType),
                                          document::Vec2d{0, 0});
        const auto added = session.executeNodeTransaction(std::move(scalar));
        const auto node = added.outputId<document::NodeId>(commands::kAddNodeOutput);
        if (node) {
            commands::Transaction drive("Drive Opacity", session.snapshot().revision());
            drive.emplace<commands::ConnectPorts>(composition,
                                                  document::OutputPortRef{*node, "value"},
                                                  document::NodeInputRef{layers[0], "opacity"});
            run(session, std::move(drive));
        }
    }

    auto* area = nodesArea(*fixture.window);
    if (area == nullptr) {
        std::cerr << "no Nodes area\n";
        return 1;
    }
    Q_EMIT area->maximizeRequested(area);
    settle();
    auto* editor = area->findChild<NodeGraphEditor*>();
    if (editor == nullptr)
        return 1;
    fixture.clearInteraction();
    session.clearSelection();

    // #644: a Path node card with its keyframe diamond in the three states: constant, on a key and
    // between keys, then a Linear Gradient card beside it.
    if (argc > 3 && QString::fromLatin1(argv[3]) == QStringLiteral("path-key")) {
        const auto addNode = [&](std::string_view type, document::Vec2d at) {
            commands::Transaction add("Add Node", session.snapshot().revision());
            add.emplace<commands::AddNode>(composition, std::string(type), at);
            return session.executeNodeTransaction(std::move(add))
                .outputId<document::NodeId>(commands::kAddNodeOutput);
        };
        const auto path = addNode(document::kPathValueNodeType, {-6000.0, 0.0});
        const auto gradient = addNode(document::kLinearGradientNodeType, {-6000.0, 260.0});
        if (!path || !gradient)
            return 1;
        document::ParameterId value;
        for (const auto& binding : session.composition()->graph().findNode(*path)->parameters)
            if (binding.role == document::kValueParameterRole)
                value = binding.parameterId;
        const auto square = [](const double half) {
            document::PathContour contour;
            contour.closed = true;
            for (const auto& point : {document::Vec2d{-half, -half}, document::Vec2d{half, -half},
                                      document::Vec2d{half, half}, document::Vec2d{-half, half}})
                contour.anchors.push_back(
                    {point, std::nullopt, std::nullopt, document::PathAnchorMode::Corner});
            return document::PathValue::fromContour(contour);
        };
        const auto frame = [](const std::int64_t index) {
            return *core::RationalTime::create(index, 24);
        };
        const auto shootCards = [&](const char* state) {
            sizeArea(*fixture.window, *area, QSize(900, 700));
            settle();
            QRectF bounds;
            for (const auto id : {*path, *gradient})
                if (auto* card = editor->graphScene()->findNodeItem(id))
                    bounds = bounds.isNull() ? card->sceneBoundingRect()
                                             : bounds.united(card->sceneBoundingRect());
            editor->graphView()->frameRect(bounds.adjusted(-60, -60, 60, 60));
            settle(120);
            save(area->grab().toImage(), QStringLiteral("nodes-path-key-%1").arg(state));
        };
        (void)session.setParameterValue(value, square(60.0), "Path");
        (void)session.setCurrentTime(frame(0));
        shootCards("constant");
        (void)session.toggleKeyframeForParameter(value);
        shootCards("on-key");
        (void)session.setCurrentTime(frame(12));
        (void)session.toggleKeyframeForParameter(value);
        (void)session.setParameterValue(value, square(140.0), "Path");
        (void)session.setCurrentTime(frame(6));
        shootCards("between-keys");
        return gFailures == 0 ? 0 : 1;
    }

    // #589: the Path socket beside its neighbors: a Path node driving a Shape Source, with Scalar,
    // Color and String literals alongside it.
    if (argc > 3 && QString::fromLatin1(argv[3]) == QStringLiteral("path-socket")) {
        commands::Transaction addShape("Add Shape", session.snapshot().revision());
        commands::ShapeLayerGeometry geometry;
        geometry.path = document::PathValue::fromContour(
            {{{10.0, 10.0}, {}, {}}, {{90.0, 10.0}, {}, {}}, {{50.0, 80.0}, {}, {}}}, true);
        addShape.emplace<commands::AddShapeLayer>(composition, document::ShapeKind::Path, geometry);
        const auto shape = session.executeNodeTransaction(std::move(addShape))
                               .outputId<document::NodeId>("shapeNode");
        if (!shape)
            return 1;
        std::vector<document::NodeId> shown{*shape};
        double y = -500.0;
        std::optional<document::NodeId> path;
        for (const auto type : {document::kScalarValueNodeType, document::kPathValueNodeType,
                                document::kColorValueNodeType, document::kStringValueNodeType}) {
            commands::Transaction add("Add Value", session.snapshot().revision());
            add.emplace<commands::AddNode>(composition, std::string(type),
                                           document::Vec2d{-500.0, y});
            y += 200.0;
            const auto node = session.executeNodeTransaction(std::move(add))
                                  .outputId<document::NodeId>(commands::kAddNodeOutput);
            if (!node)
                return 1;
            shown.push_back(*node);
            if (type == document::kPathValueNodeType)
                path = node;
        }
        commands::Transaction wire("Wire Path", session.snapshot().revision());
        wire.emplace<commands::ConnectPorts>(composition, document::OutputPortRef{*path, "value"},
                                             document::NodeInputRef{*shape, "path"});
        run(session, std::move(wire));
        // Beside the Shape Source card and clear of the sample graph.
        {
            std::map<document::NodeId, document::Vec2d> positions;
            constexpr std::array rows{0.0, 120.0, 270.0, 500.0};
            for (std::size_t index = 1; index < shown.size(); ++index)
                positions[shown[index]] = document::Vec2d{-6400.0, rows.at(index - 1)};
            positions[*shape] = document::Vec2d{-6000.0, 0.0};
            commands::Transaction arrange("Place", session.snapshot().revision());
            arrange.emplace<commands::MoveNodes>(composition, std::move(positions));
            run(session, std::move(arrange));
        }
        for (const auto& [name, size] : std::vector<std::pair<const char*, QSize>>{
                 {"2000", QSize(2000, 1000)}, {"900", QSize(900, 700)}, {"480", QSize(480, 700)}}) {
            sizeArea(*fixture.window, *area, size);
            settle();
            QRectF bounds;
            for (const auto id : shown)
                if (auto* card = editor->graphScene()->findNodeItem(id))
                    bounds = bounds.isNull() ? card->sceneBoundingRect()
                                             : bounds.united(card->sceneBoundingRect());
            editor->graphView()->frameRect(bounds.adjusted(-60, -60, 60, 60));
            settle(120);
            save(area->grab().toImage(), QStringLiteral("nodes-path-socket-%1").arg(name));
        }
        return gFailures == 0 ? 0 : 1;
    }

    const std::vector<std::pair<const char*, QSize>> sizes{{"wide", QSize(2000, 1000)},
                                                           {"narrow", QSize(900, 700)}};
    const auto shoot = [&](const char* state, const std::function<void()>& frame) {
        for (const auto& [name, size] : sizes) {
            sizeArea(*fixture.window, *area, size);
            settle();
            frame();
            settle(120);
            save(area->grab().toImage(),
                 QStringLiteral("nodes-%1-%2").arg(QLatin1String(state), QLatin1String(name)));
        }
    };
    const auto fit = [&] { editor->graphView()->frameGraph(); };

    // 1. The footer's Arrange: the layer stack read right to left.
    // The footer is reparented into the area's own footer row.
    auto* arrange = area->findChild<kit::KButton*>(QStringLiteral("nodeArrangeButton"));
    if (arrange == nullptr) {
        std::cerr << "no Arrange button\n";
        return 1;
    }
    sizeArea(*fixture.window, *area, sizes.front().second);
    settle();
    arrange->click();
    settle();
    shoot("arranged", fit);

    // 2. The footer itself, at both widths.
    for (const auto& [name, size] : sizes) {
        sizeArea(*fixture.window, *area, size);
        settle();
        const auto whole = area->grab().toImage();
        const auto ratio = whole.devicePixelRatio();
        const int strip = static_cast<int>(48 * ratio);
        save(whole.copy(0, whole.height() - strip, whole.width(), strip),
             QStringLiteral("nodes-footer-%1").arg(QLatin1String(name)));
    }

    // 3. A Merge carrying three layers, framed close.
    const auto merge = session.composition()->graph().layerStack().nodeId();
    shoot("merge3", [&] {
        QRectF bounds;
        for (const auto id : {merge, layers[0], layers[1], layers[2]})
            if (auto* card = editor->graphScene()->findNodeItem(id))
                bounds = bounds.isNull() ? card->sceneBoundingRect()
                                         : bounds.united(card->sceneBoundingRect());
        editor->graphView()->frameRect(bounds.adjusted(-80, -80, 80, 80));
    });

    // 4. The canvas at 25%.
    shoot("zoom25", [&] { editor->graphView()->zoomToPercent(25); });

    // 5. A coloured group frame around one layer's upstream chain: its source and its blur.
    {
        commands::Transaction group("Group", session.snapshot().revision());
        group.emplace<commands::GroupNodes>(composition, std::set{sources[1], blurNode},
                                            std::string("Plates"));
        const auto result = session.executeNodeTransaction(std::move(group));
        const auto id = result.outputId<document::NodeGroupId>(commands::kGroupNodesOutput);
        if (id) {
            const auto label = kit::color(kit::Color::Label6);
            commands::Transaction color("Color", session.snapshot().revision());
            color.emplace<commands::SetGroupColor>(
                composition, *id,
                std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(label.red()),
                                            static_cast<std::uint8_t>(label.green()),
                                            static_cast<std::uint8_t>(label.blue())});
            run(session, std::move(color));
        }
    }
    shoot("group", fit);

    // 6. Collapsed cards: a source, its blur and two Layer cards folded to their headers.
    {
        commands::Transaction collapse("Collapse", session.snapshot().revision());
        for (const auto id : {sources[1], layers[1], layers[2]})
            collapse.emplace<commands::SetNodeCollapsed>(composition, id, true);
        for (const auto& node : session.composition()->graph().nodes())
            if (node.typeId == "bloom.gaussian-blur")
                collapse.emplace<commands::SetNodeCollapsed>(composition, node.id, true);
        run(session, std::move(collapse));
    }
    shoot("collapsed", [&] {
        QRectF bounds;
        for (const auto id : {sources[1], layers[1], layers[2], merge})
            if (auto* card = editor->graphScene()->findNodeItem(id))
                bounds = bounds.isNull() ? card->sceneBoundingRect()
                                         : bounds.united(card->sceneBoundingRect());
        editor->graphView()->frameRect(bounds.adjusted(-120, -120, 120, 120));
    });

    // 7. #525: every Fields card with its controls (point pads, dials, ranges, ramp, curves),
    // each framed alone in a tall area so the card reads at about 100%.
    double x = 6000.0;
    for (const auto& spec : document::fieldNodeSpecs()) {
        commands::Transaction add("Add Field", session.snapshot().revision());
        add.emplace<commands::AddNode>(composition, std::string(spec.typeId),
                                       document::Vec2d{x, 0.0});
        x += 1000.0;
        const auto added = session.executeNodeTransaction(std::move(add));
        const auto node = added.outputId<document::NodeId>(commands::kAddNodeOutput);
        if (!node) {
            std::cerr << "could not add " << spec.typeId << '\n';
            ++gFailures;
            continue;
        }
        settle(20);
        sizeArea(*fixture.window, *area, QSize(900, 1300));
        settle();
        if (auto* card = editor->graphScene()->findNodeItem(*node))
            editor->graphView()->frameRect(card->sceneBoundingRect().adjusted(-40, -40, 40, 40));
        settle(120);
        auto name = QString::fromLatin1(spec.typeId.data(),
                                        static_cast<qsizetype>(spec.typeId.size()));
        name.replace(QLatin1Char('.'), QLatin1Char('-'));
        save(area->grab().toImage(), QStringLiteral("nodes-card-%1").arg(name));
    }
    return gFailures == 0 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: bloom_node_canvas_capture <directory> <1|1.5> [path-socket]\n";
        return 2;
    }
    qputenv("QT_SCALE_FACTOR", argv[2]);
    try {
        return capture(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
