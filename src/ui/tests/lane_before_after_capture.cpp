// Before and after review captures for the toolbar, workspace, panels, controls and LUT group
// (#609, #610, #638, #687, #688, #692, #698, #700, #712, #713, #714, #716, #723).
// A tool, not a test. It uses only interfaces the code had before the group, so the same file
// builds on the commit before it ("before") and on the group's head ("after"):
//   bloom_lane_before_after_capture <directory> <before|after> [parts]
// writes <issue>-<phase>-<name>.png. Parts: window, layers, assets, dissolve, cards, preferences.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include "node_interaction_test_support.hpp"
#include "preferences_window.hpp"
#include "window_fixture.hpp"

#include <QDir>
#include <QHeaderView>
#include <QImage>
#include <QListWidget>
#include <QSettings>
#include <QStandardPaths>
#include <QTreeWidget>
#include <bloom/commands/entry_layer_operations.hpp>
#include <bloom/commands/layer_stack_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/context_bar.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/tokens.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/window_status_bar.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace {
using namespace bloom;
using namespace bloom::ui;

QString gDirectory;
QString gPhase;
int gFailures = 0;

void settle(const int ms = 60) {
    QApplication::processEvents();
    QTest::qWait(ms);
    QApplication::processEvents();
}

void save(const QImage& image, const char* issue, const QString& name) {
    const QString path = gDirectory + QLatin1Char('/') + QLatin1String(issue) + QLatin1Char('-') +
                         gPhase + QLatin1Char('-') + name + QStringLiteral(".png");
    if (image.isNull() || !image.save(path)) {
        std::cerr << "could not write " << path.toStdString() << '\n';
        ++gFailures;
        return;
    }
    std::cout << path.toStdString() << ' ' << image.width() << 'x' << image.height() << '\n';
}

std::filesystem::path writeSvg() {
    const auto svg = std::filesystem::temp_directory_path() / "lane-before-after-logo.svg";
    std::ofstream(svg, std::ios::binary)
        << R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120"><circle cx="60" cy="60" r="44" fill="#ee3377"/><clipPath id="c"><rect x="110" y="26" width="70" height="60"/></clipPath><rect x="100" y="0" width="100" height="120" fill="#3377ee" clip-path="url(#c)"/></svg>)svg";
    return svg;
}

void importSvgLayer(test::WindowFixture& f, const std::filesystem::path& svg) {
    commands::Transaction transaction("Import SVG", f.session.snapshot().revision());
    transaction
        .emplace<commands::AddSvgArtwork>(f.session.compositionId(), nullptr, svg, "logo", true,
                                          commands::SvgImportMode::Layer)
        .withProjectDirectory(svg.parent_path());
    (void)f.session.executeTransaction(std::move(transaction));
}

EditorArea* areaOf(test::WindowFixture& f, const char* editor) {
    for (auto* area : f.window->findChildren<EditorArea*>())
        if (area->editorId() == editor)
            return area;
    return nullptr;
}

std::optional<document::LayerId> logoLayer(test::WindowFixture& f) {
    for (const auto& layer : f.session.composition()->graph().layerOutputs())
        if (layer.name == "logo")
            return layer.layerId;
    return std::nullopt;
}

// #687, #700 the top strip; #712 the SVG layer's box in the Viewer; #713 the toolbar with the SVG
// layer selected; #698 a drag on empty Viewer space; #716 the status bar after the Output node is
// deleted (or the deletion is refused).
void captureWindow() {
    const auto svg = writeSvg();
    test::WindowFixture fixture([&](test::WindowFixture& f) { importSvgLayer(f, svg); });
    settle(300);
    if (const auto layer = logoLayer(fixture))
        fixture.session.selectLayer(*layer);
    settle(600);
    const QImage window = fixture.window->grab().toImage();
    const int strip = static_cast<int>(kit::px(kit::Size::Control) * 3 * window.devicePixelRatio());
    save(window.copy(0, 0, window.width(), std::min(strip, window.height())), "687", "top-strip");
    if (auto* bar = fixture.window->contextBar())
        save(bar->grab().toImage(), "713", "toolbar-svg-layer-selected");
    auto* viewerArea = areaOf(fixture, "bloom.viewer");
    if (viewerArea != nullptr)
        save(viewerArea->grab().toImage(), "712", "svg-layer-selected");
    // #698: a drag that starts on empty space inside the frame, above and left of the artwork.
    if (auto* viewer = viewerArea != nullptr ? viewerArea->findChild<ViewerEditor*>() : nullptr) {
        fixture.session.clearSelection();
        settle(200);
        const QPoint a(viewer->width() * 9 / 100, viewer->height() * 4 / 100);
        const QPoint b(viewer->width() * 46 / 100, viewer->height() * 60 / 100);
        QTest::mousePress(viewer, Qt::LeftButton, Qt::NoModifier, a);
        QTest::mouseMove(viewer, (a + b) / 2);
        QTest::mouseMove(viewer, b);
        settle();
        save(viewer->grab().toImage(), "698", "viewer-drag-on-empty-space");
        QTest::keyClick(viewer, Qt::Key_Escape);
        QTest::mouseRelease(viewer, Qt::LeftButton, Qt::NoModifier, b);
        settle();
    }
    if (const auto output = fixture.session.composition()->graph().compositionOutput()) {
        commands::Transaction remove("Delete", fixture.session.snapshot().revision());
        remove.emplace<commands::RemoveNodes>(fixture.session.compositionId(),
                                              std::set<document::NodeId>{output->nodeId});
        (void)fixture.session.executeTransaction(std::move(remove));
        settle(200);
        if (auto* bar = fixture.window->findChild<WindowStatusBar*>())
            save(bar->grab().toImage(), "716", "status-bar-after-deleting-output");
    }
}

// #714: the node graph with an SVG layer, and after the layer is deleted.
void captureLayerDelete() {
    const auto svg = writeSvg();
    test::WindowFixture fixture([&](test::WindowFixture& f) { importSvgLayer(f, svg); });
    settle(300);
    auto* nodes = areaOf(fixture, "bloom.nodes");
    if (nodes == nullptr) {
        ++gFailures;
        return;
    }
    Q_EMIT nodes->maximizeRequested(nodes);
    settle(300);
    save(nodes->grab().toImage(), "714", "graph-with-svg-layer");
    if (const auto layer = logoLayer(fixture)) {
        commands::Transaction remove("Delete Layer", fixture.session.snapshot().revision());
        remove.emplace<commands::DeleteLayers>(fixture.session.compositionId(),
                                               std::vector<document::LayerId>{*layer});
        (void)fixture.session.executeTransaction(std::move(remove));
    }
    settle(300);
    save(nodes->grab().toImage(), "714", "graph-after-deleting-the-layer");
}

// #692: the Assets panel, and after a drag on the edge of its first column header.
void captureAssets() {
    const auto svg = writeSvg();
    test::WindowFixture fixture([&](test::WindowFixture& f) { importSvgLayer(f, svg); });
    settle(300);
    auto* assets = areaOf(fixture, "bloom.assets");
    if (assets == nullptr) {
        ++gFailures;
        return;
    }
    save(assets->grab().toImage(), "692", "assets-panel");
    if (auto* tree = assets->findChild<QTreeWidget*>()) {
        auto* header = tree->header();
        const int edge = header->sectionViewportPosition(0) + header->sectionSize(0);
        const QPoint grip(edge - 1, header->height() / 2);
        QTest::mouseMove(header->viewport(), grip);
        QTest::mousePress(header->viewport(), Qt::LeftButton, Qt::NoModifier, grip);
        QTest::mouseMove(header->viewport(), grip + QPoint(30, 0));
        QTest::mouseMove(header->viewport(), grip + QPoint(70, 0));
        QTest::mouseRelease(header->viewport(), Qt::LeftButton, Qt::NoModifier,
                            grip + QPoint(70, 0));
        settle();
    }
    save(assets->grab().toImage(), "692", "assets-panel-after-dragging-the-name-header-edge");
}

// #723: Adjustment, Curves, Merge Content slot; then Curves dissolved.
void captureDissolve() {
    test::Fixture f;
    const auto port = [&](const document::NodeId id, const bool input) {
        const auto* node = f.session.composition()->graph().findNode(id);
        const auto* definition =
            document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion);
        if (input)
            return std::ranges::find(definition->inputs, document::SocketValueKind::Image,
                                     &document::InputPortDefinition::valueKind)
                ->name;
        return std::ranges::find(definition->outputs, document::SocketValueKind::Image,
                                 &document::OutputPortDefinition::valueKind)
            ->name;
    };
    const auto merge = f.session.composition()->graph().layerStack().nodeId();
    const auto added = f.edit<commands::AddAdjustmentLayer>(merge);
    const auto adjustment = added.outputId<document::NodeId>(commands::kEntryLayerNodeOutput);
    const auto slot = added.outputId<document::LayerSlotId>(commands::kEntryLayerSlotOutput);
    if (!adjustment || !slot) {
        ++gFailures;
        return;
    }
    const auto curves = f.add(document::kCurvesNodeType, {300, 300});
    const document::InputPortRef content = document::LayerStackInputRef{merge, *slot, "content"};
    (void)f.edit<commands::ConnectPorts>(
        document::OutputPortRef{*adjustment, port(*adjustment, false)},
        document::InputPortRef{document::NodeInputRef{curves, port(curves, true)}});
    (void)f.edit<commands::ConnectPorts>(document::OutputPortRef{curves, port(curves, false)},
                                         content);
    settle();
    f.view()->frameGraph();
    settle(200);
    save(f.editor.grab().toImage(), "723", "graph-before-dissolve");
    (void)f.edit<commands::DissolveNode>(curves);
    settle();
    f.view()->frameGraph();
    settle(200);
    save(f.editor.grab().toImage(), "723", "graph-after-dissolving-curves");
}

// #609, #610, #638: the cards and Properties of the nodes those issues changed.
void captureCards() {
    struct Card final {
        const char* issue;
        const char* type;
        const char* name;
    };
    for (const auto& [issue, type, name] :
         {Card{"609", "bloom.ocio-file-transform", "lut"},
          Card{"609", "bloom.ocio-colour-space-transform", "color-space-transform"},
          Card{"638", "bloom.trim-path", "trim-path"},
          Card{"638", "bloom.path-stroke", "path-stroke"},
          Card{"638", "bloom.hsl-qualifier", "hsl-qualifier"},
          Card{"638", "bloom.color-matrix", "color-matrix"}}) {
        test::Fixture f;
        const auto node = f.add(type, {0, 0});
        settle(150);
        const auto grabCard = [&](const QString& suffix) {
            auto* card = f.card(node);
            if (card == nullptr) {
                ++gFailures;
                return;
            }
            f.view()->centerOn(card);
            settle(100);
            const QRect region = f.view()
                                     ->mapFromScene(card->sceneBoundingRect().adjusted(-16, -16, 16, 16))
                                     .boundingRect()
                                     .intersected(f.view()->viewport()->rect());
            save(f.view()->viewport()->grab(region).toImage(), issue,
                 QLatin1String(name) + suffix);
        };
        grabCard(QStringLiteral("-card"));
        f.session.selectNode(node);
        PropertiesEditor properties(f.session);
        properties.resize(420, 700);
        properties.show();
        settle(150);
        save(properties.grab().toImage(), issue, QLatin1String(name) + QStringLiteral("-properties-420"));
        if (std::string_view(type) != "bloom.ocio-file-transform")
            continue;
        // #610: the Process Space choices, and a space the project's configuration lacks.
        (void)f.edit<commands::SetNodeParameterValue>(
            node, "processSpace", document::ParameterValue{std::string("srgb_rec709_display")});
        settle(150);
        grabCard(QStringLiteral("-card-process-space-srgb"));
        save(properties.grab().toImage(), "610", QStringLiteral("lut-properties-process-space-srgb"));
        (void)f.edit<commands::SetNodeParameterValue>(
            node, "processSpace", document::ParameterValue{std::string("ACEScct")});
        settle(150);
        grabCard(QStringLiteral("-card-process-space-missing"));
        save(properties.grab().toImage(), "610",
             QStringLiteral("lut-properties-process-space-missing"));
    }
}

// #688: Preferences, on the Shortcuts page when there is one.
void capturePreferences() {
    QWidget owner;
    PreferencesWindow preferences(owner, defaultApplicationPreferences());
    preferences.show();
    settle();
    if (auto* rail = preferences.findChild<QListWidget*>(QStringLiteral("preferencesCategoryRail")))
        for (int row = 0; row < rail->count(); ++row)
            if (rail->item(row)->text() == QStringLiteral("Shortcuts"))
                rail->setCurrentRow(row);
    settle();
    save(preferences.grab().toImage(), "688", "preferences");
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: bloom_lane_before_after_capture <directory> <before|after> [parts]\n";
        return 2;
    }
    QApplication app(argc, argv);
    app.setOrganizationName("BloomLaneCapture");
    app.setApplicationName("LaneBeforeAfterCapture");
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    kit::installAfterlabTheme(app);
    gDirectory = QString::fromLocal8Bit(argv[1]);
    gPhase = QString::fromLocal8Bit(argv[2]);
    QDir().mkpath(gDirectory);
    const QStringList only =
        argc > 3 ? QString::fromLocal8Bit(argv[3]).split(QLatin1Char(',')) : QStringList{};
    const auto wanted = [&](const char* name) {
        return only.isEmpty() || only.contains(QLatin1String(name));
    };
    try {
        if (wanted("window"))
            captureWindow();
        if (wanted("layers"))
            captureLayerDelete();
        if (wanted("assets"))
            captureAssets();
        if (wanted("dissolve"))
            captureDissolve();
        if (wanted("cards"))
            captureCards();
        if (wanted("preferences"))
            capturePreferences();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return gFailures == 0 ? 0 : 1;
}
