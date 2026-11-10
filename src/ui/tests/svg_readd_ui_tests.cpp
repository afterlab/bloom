// #646: adding an imported SVG asset to a composition again, from the desktop.
//  - the Viewer and Timeline drop, the Node editor drop, the Assets panel's Add to Composition and
//    New Composition from Selection, and a file drop with its placement all build the artwork on
//    the asset's own record (never an image layer), as one undo step;
//  - the chooser is asked once per batch, or the remembered answer is used without a prompt;
//  - the project's stored copy is used when the file is gone; with neither the add is refused by the
//    asset's name and nothing is added;
//  - a project that already holds an image layer made from an SVG asset names the warning on the
//    card, the Timeline row and Properties.

#include "asset_drop.hpp"
#include "assets_editor_internal.hpp"
#include "layer_drop.hpp"
#include "node_editor_items.hpp"

#include <bloom/commands/layer_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/assets_editor.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/properties_editor.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/timeline_editor.hpp>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGraphicsView>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <iostream>
#include <stdexcept>

namespace {
using namespace bloom;

void require(const bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <typename Predicate> bool waitUntil(Predicate predicate, const int timeoutMs = 20000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QTest::qWait(5);
    return predicate();
}

constexpr auto kFirst = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 200 100">
<rect id="box" x="10" y="10" width="80" height="60" fill="#cc3333"/>
<path id="ring" fill="#336699" fill-rule="evenodd" d="M110 10H190V90H110Z M130 30H170V70H130Z"/></svg>
)svg";
constexpr auto kSecond = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120" viewBox="0 0 120 120">
<circle id="dot" cx="60" cy="60" r="40" fill="#33aa66"/></svg>
)svg";

constexpr auto kPathNodes = "bloom.value-path";

struct Harness final {
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host{scheduler};
    ui::CompositionSession session{*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId()};
    ui::TaskUiBridge bridge{scheduler};
    ui::AssetController controller{session, host, scheduler, bridge};
    QStringList diagnostics;
    Harness() {
        if (session.composition() == nullptr) {
            commands::Transaction seed("Seed", session.snapshot().revision());
            seed.emplace<commands::AddComposition>("Composition 1", document::CompositionFormat{},
                                                   core::RationalTime::fromInteger(10));
            const auto seeded = session.executeTransaction(std::move(seed));
            const auto id = seeded.succeeded()
                                ? seeded.outputId<document::CompositionId>(commands::kAddCompositionOutput)
                                : std::nullopt;
            require(id && session.setComposition(*id), "seed a composition");
        }
        session.setAssetController(&controller);
        QObject::connect(&controller, &ui::AssetController::diagnostic,
                         [this](const QString& text) { diagnostics.push_back(text); });
    }
    ~Harness() {
        session.setAssetController(nullptr);
        waitUntil([this] { return !controller.busy(); });
    }
    [[nodiscard]] const document::Composition& composition() const {
        return *session.snapshot().project().findComposition(session.compositionId());
    }
    [[nodiscard]] std::size_t count(const std::string_view type) const {
        return static_cast<std::size_t>(std::ranges::count_if(
            composition().graph().nodes(), [&](const auto& node) { return node.typeId == type; }));
    }
    // The composition's own stack (an editable import also makes entry layers inside its groups).
    [[nodiscard]] std::size_t layers() const { return composition().graph().layerStack().entries().size(); }
    [[nodiscard]] std::size_t assets() const { return session.snapshot().project().assets().size(); }
    [[nodiscard]] std::size_t blocks() const { return session.snapshot().project().typedDataBlocks().size(); }
    [[nodiscard]] bool mentions(const QString& text) const {
        return std::ranges::any_of(diagnostics, [&](const QString& line) { return line.contains(text); });
    }
    [[nodiscard]] document::AssetId assetNamed(const QString& name) const {
        for (const auto& asset : session.snapshot().project().assets())
            if (QString::fromStdString(asset.name) == name)
                return asset.id;
        return {};
    }
    // Imports `path` in `mode` (the chooser seam answers) and returns its Assets record.
    document::AssetId import(const QString& path, const commands::SvgImportMode mode) {
        controller.setSvgImportChooserForTesting(
            [mode](std::size_t) { return std::optional<commands::SvgImportMode>(mode); });
        const auto before = assets();
        controller.importFiles({path});
        require(waitUntil([&] { return !controller.busy() && assets() > before; }), "the SVG imports");
        return assetNamed(QFileInfo(path).fileName());
    }
    void settle() { require(waitUntil([&] { return !controller.busy(); }), "the controller settles"); }
};

void rememberChoice(const ui::SvgImportChoice choice) {
    QSettings settings;
    auto preferences = ui::loadApplicationPreferences(settings);
    preferences.importSvgMode = choice;
    ui::saveApplicationPreferences(settings, preferences);
}

QStringList contextMenu(QTreeWidget& tree, const QPoint position, const QString& trigger) {
    QStringList names;
    QTimer::singleShot(0, &tree, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr)
            return;
        QAction* chosen = nullptr;
        for (auto* action : menu->actions()) {
            if (!action->objectName().isEmpty())
                names.push_back(action->objectName());
            if (action->objectName() == trigger)
                chosen = action;
        }
        menu->close();
        if (chosen != nullptr && chosen->isEnabled())
            chosen->trigger();
    });
    emit tree.customContextMenuRequested(position);
    QApplication::processEvents();
    return names;
}

// A drag the way the platform delivers it: enter, then the drop (Qt routes a drop only to a widget that
// accepted the enter).
bool dropOn(QWidget& target, const QMimeData& mime, const QPoint position) {
    QDragEnterEvent enter(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&target, &enter);
    QDropEvent drop(QPointF(position), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&target, &drop);
    return drop.isAccepted();
}

QTreeWidgetItem* findRow(QTreeWidget& tree, const QString& name) {
    for (QTreeWidgetItemIterator it(&tree); *it; ++it)
        if ((*it)->text(ui::assets::NameColumn) == name)
            return *it;
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomSvgReadd"));
    QApplication::setApplicationName(QStringLiteral("SvgReadd"));
    QStandardPaths::setTestModeEnabled(true);
    ui::kit::installKinetikTheme(app);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "temporary directory");
        const QString first = directory.filePath("one.svg");
        const QString second = directory.filePath("two.svg");
        for (const auto& [path, text] : {std::pair{first, kFirst}, std::pair{second, kSecond}}) {
            QFile file(path);
            require(file.open(QIODevice::WriteOnly) && file.write(text) > 0, "write the SVG");
        }
        QSettings().clear();

        // Both modes: the chooser is asked once for a batch of two; the artwork is built on the
        // existing records; one undo removes the whole add.
        for (const auto mode : {commands::SvgImportMode::Layer, commands::SvgImportMode::Shapes}) {
            QSettings().clear();
            Harness harness;
            const auto a = harness.import(first, commands::SvgImportMode::Layer);
            const auto b = harness.import(second, commands::SvgImportMode::Layer);
            require(a.isValid() && b.isValid(), "both SVG files are in Assets");
            const auto layersBefore = harness.layers();
            const auto assetsBefore = harness.assets();
            const auto blocksBefore = harness.blocks();
            std::vector<std::size_t> asked;
            harness.controller.setSvgImportChooserForTesting([&](const std::size_t count) {
                asked.push_back(count);
                return std::optional(mode);
            });
            std::size_t committed = 0;
            harness.controller.addToComposition({a, b}, {}, [&](const commands::CommandResult&) { ++committed; });
            require(waitUntil([&] { return !harness.controller.busy() && harness.layers() > layersBefore; }),
                    "the add commits");
            require(asked == std::vector<std::size_t>{2}, "the chooser is asked once, for the two SVG assets");
            require(committed == 1 && harness.layers() == layersBefore + 2, "two layers join, in one commit");
            require(harness.assets() == assetsBefore, "no second Assets record");
            require(harness.count("bloom.image-source") == 0, "never an image layer");
            if (mode == commands::SvgImportMode::Layer)
                require(harness.count(document::kVectorArtworkNodeType) == 4 && harness.blocks() == blocksBefore,
                        "one layer: two more Vector Artwork layers, the stored copies are shared");
            else
                require(harness.count(kPathNodes) == 3 && harness.count(document::kVectorArtworkNodeType) == 2,
                        "shapes: the shapes are built again");
            require(harness.session.undo(), "undo");
            require(harness.layers() == layersBefore && harness.assets() == assetsBefore,
                    "one undo removes the whole add and no record");
        }

        // The remembered answer: no chooser.
        {
            QSettings().clear();
            rememberChoice(ui::SvgImportChoice::Shapes);
            Harness harness;
            const auto a = harness.import(first, commands::SvgImportMode::Layer);
            harness.controller.setSvgImportChooserForTesting([](std::size_t) -> std::optional<commands::SvgImportMode> {
                throw std::runtime_error("the chooser must not be asked");
            });
            const auto before = harness.count(kPathNodes);
            harness.controller.addToComposition({a}, {}, {});
            require(waitUntil([&] { return !harness.controller.busy() && harness.count(kPathNodes) > before; }),
                    "the remembered mode builds the shapes without a prompt");
            QSettings().clear();
        }

        // Cancelling the chooser adds nothing.
        {
            Harness harness;
            const auto a = harness.import(first, commands::SvgImportMode::Layer);
            const auto before = harness.layers();
            harness.controller.setSvgImportChooserForTesting(
                [](std::size_t) { return std::optional<commands::SvgImportMode>{}; });
            harness.controller.addToComposition({a}, {}, {});
            QTest::qWait(100);
            require(!harness.controller.busy() && harness.layers() == before, "Cancel adds nothing");
        }

        // The file is gone: one layer uses the stored copy; shapes keep none, so the add is refused by
        // name and nothing is added.
        {
            QSettings().clear();
            const QString keep = directory.filePath("gone-layer.svg");
            const QString lose = directory.filePath("gone-shapes.svg");
            for (const auto& path : {keep, lose}) {
                QFile file(path);
                require(file.open(QIODevice::WriteOnly) && file.write(kSecond) > 0, "write the SVG");
            }
            Harness harness;
            // Same bytes would share one digest; make the second different.
            {
                QFile file(lose);
                require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(kFirst) > 0, "rewrite");
            }
            const auto held = harness.import(keep, commands::SvgImportMode::Layer);
            const auto bare = harness.import(lose, commands::SvgImportMode::Shapes);
            QFile::remove(keep);
            QFile::remove(lose);
            const auto before = harness.layers();
            harness.controller.setSvgImportChooserForTesting(
                [](std::size_t) { return std::optional(commands::SvgImportMode::Shapes); });
            harness.controller.addToComposition({held}, {}, {});
            require(waitUntil([&] { return !harness.controller.busy() && harness.layers() > before; }),
                    "an SVG the project holds adds with its file gone");
            const auto afterHeld = harness.layers();
            harness.diagnostics.clear();
            harness.controller.addToComposition({bare}, {}, {});
            harness.settle();
            QTest::qWait(50);
            require(harness.layers() == afterHeld, "the refused add adds nothing");
            require(harness.mentions(QStringLiteral("gone-shapes.svg")) && harness.mentions(QStringLiteral("missing")),
                    "the refusal names the missing asset");
            // One missing SVG in a batch refuses the whole batch.
            const auto both = harness.layers();
            harness.controller.addToComposition({held, bare}, {}, {});
            harness.settle();
            QTest::qWait(50);
            require(harness.layers() == both, "a batch with one missing SVG adds nothing");
        }

        // Viewer and Timeline drop: stack position and start come from where it lands.
        {
            QSettings().clear();
            rememberChoice(ui::SvgImportChoice::Layer);
            Harness harness;
            const auto a = harness.import(first, commands::SvgImportMode::Layer);
            QWidget target;
            ui::LayerDropHooks hooks;
            hooks.hit = [](QPoint) {
                ui::LayerDropHit hit;
                hit.stack = ui::LayerDropPlacement::Stack::Bottom;
                hit.time = core::RationalTime::fromInteger(1);
                return hit;
            };
            ui::installLayerDropTarget(target, harness.session, hooks);
            target.resize(200, 100);
            target.show();
            QApplication::processEvents();
            QMimeData mime;
            mime.setData(ui::kAssetMimeType, ui::assetMimePayload(harness.session, a));
            const auto layersBefore = harness.layers();
            require(dropOn(target, mime, QPoint(5, 5)), "the drop is accepted");
            const bool grew = waitUntil([&] { return !harness.controller.busy() && harness.layers() > layersBefore; });
            require(grew, "the drop adds the artwork");
            require(harness.count(document::kVectorArtworkNodeType) == 2 && harness.count("bloom.image-source") == 0,
                    "the dropped SVG is artwork, not an image layer");
            const auto entries = harness.composition().graph().layerStack().entries();
            const auto* added = harness.composition().graph().findLayer(entries.back().layerId);
            require(added != nullptr && added->inPoint == core::RationalTime::fromInteger(1),
                    "it sits at the bottom of the stack and starts where it was dropped");
            const auto selected = harness.session.selectedEntries();
            require(!selected.empty(), "the new layer ends up selected");
            require(harness.session.undo() && harness.layers() == layersBefore, "one undo removes it");
        }

        // File drop with its placement (F8): the SVG lands where the drop put it.
        {
            QSettings().clear();
            Harness harness;
            harness.controller.setSvgImportChooserForTesting(
                [](std::size_t) { return std::optional(commands::SvgImportMode::Layer); });
            const auto layersBefore = harness.layers();
            commands::LayerDropPlacement placement;
            placement.stack = commands::LayerDropPlacement::Stack::Bottom;
            placement.start = core::RationalTime::fromInteger(2);
            harness.controller.importFiles({first}, harness.controller.importFolder(), {}, {}, placement);
            require(waitUntil([&] { return !harness.controller.busy() && harness.layers() > layersBefore; }),
                    "the dropped file imports");
            const auto entries = harness.composition().graph().layerStack().entries();
            const auto* added = harness.composition().graph().findLayer(entries.back().layerId);
            require(added != nullptr && added->inPoint == core::RationalTime::fromInteger(2),
                    "a dropped SVG file is placed at the drop's stack position and start");
        }

        // Node editor drop: unconnected artwork nodes at the drop point.
        for (const auto mode : {commands::SvgImportMode::Layer, commands::SvgImportMode::Shapes}) {
            QSettings().clear();
            Harness harness;
            const auto a = harness.import(first, commands::SvgImportMode::Layer);
            harness.controller.setSvgImportChooserForTesting(
                [mode](std::size_t) { return std::optional(mode); });
            QWidget widget;
            QGraphicsView view;
            ui::installAssetDropTarget(widget, harness.session, &view);
            widget.resize(200, 100);
            widget.show();
            QApplication::processEvents();
            QMimeData mime;
            mime.setData(ui::kAssetMimeType, ui::assetMimePayload(harness.session, a));
            const auto nodesBefore = harness.composition().graph().nodes().size();
            const auto layersBefore = harness.layers();
            require(dropOn(widget, mime, QPoint(30, 40)), "the node drop is accepted");
            require(waitUntil([&] { return !harness.controller.busy() &&
                                           harness.composition().graph().nodes().size() > nodesBefore; }),
                    "the node drop adds nodes");
            require(harness.count("bloom.image-source") == 0, "never an image node");
            if (mode == commands::SvgImportMode::Layer)
                require(harness.count(document::kVectorArtworkNodeType) == 2 && harness.layers() == layersBefore + 0,
                        "one layer: a Vector Artwork node, unconnected, no layer");
            else
                require(harness.count(kPathNodes) == 2 && harness.layers() == layersBefore,
                        "shapes: the artwork's nodes, unconnected, no layer in the stack");
            const auto point = view.mapToScene(QPoint(30, 40));
            bool placed = false;
            for (const auto& [id, layout] : harness.composition().nodeLayout())
                if (std::abs(layout.position.x - point.x()) < 0.01 && std::abs(layout.position.y - point.y()) < 0.01)
                    placed = true;
            require(placed, "a card sits at the drop point");
            require(harness.assets() == 1, "the asset record is reused");
            require(harness.session.undo(), "undo");
            require(harness.composition().graph().nodes().size() == nodesBefore, "one undo removes the nodes");
        }

        // The Assets panel: Add to Composition and New Composition from Selection.
        {
            QSettings().clear();
            rememberChoice(ui::SvgImportChoice::Layer);
            Harness harness;
            const auto a = harness.import(first, commands::SvgImportMode::Layer);
            ui::AssetsEditor editor(harness.session);
            editor.resize(700, 500);
            editor.show();
            QApplication::processEvents();
            auto* tree = editor.findChild<QTreeWidget*>("assetsTree");
            require(tree != nullptr, "the assets tree");
            auto* row = findRow(*tree, QStringLiteral("one.svg"));
            require(row != nullptr, "the SVG is listed");
            const auto at = [&] { return tree->visualItemRect(row).center(); };
            const auto layersBefore = harness.layers();
            auto names = contextMenu(*tree, at(), QStringLiteral("assetsAddToCompositionAction"));
            require(names.contains(QStringLiteral("assetsAddToCompositionAction")), "the menu offers Add to Composition");
            require(waitUntil([&] { return !harness.controller.busy() && harness.layers() > layersBefore; }),
                    "Add to Composition builds the artwork");
            require(harness.count(document::kVectorArtworkNodeType) == 2 && harness.count("bloom.image-source") == 0 &&
                        harness.assets() == 1,
                    "it is artwork on the existing record");
            require(harness.session.undo() && harness.layers() == layersBefore, "one undo removes it");

            const auto compositions = harness.session.snapshot().project().compositions().size();
            row = findRow(*tree, QStringLiteral("one.svg"));
            (void)contextMenu(*tree, at(), QStringLiteral("assetsNewCompositionFromSelectionAction"));
            require(waitUntil([&] { return !harness.controller.busy() &&
                                           harness.session.snapshot().project().compositions().size() > compositions; }),
                    "New Composition from Selection builds a composition");
            require(harness.count(document::kVectorArtworkNodeType) == 1 && harness.count("bloom.image-source") == 0 &&
                        harness.session.composition()->format().width() == 200,
                    "the new composition holds the SVG's artwork, sized from it, and is opened");
            (void)a;
        }

        // A project that holds an image layer made from an SVG asset before it could be added again.
        {
            auto seed = document::makeNewProject("Legacy", "Main", core::RationalTime::fromInteger(4));
            document::AssetRecord asset;
            asset.id = document::AssetId::fromRaw(1);
            asset.kind = document::AssetKind::Image;
            asset.name = "legacy.png";
            asset.locator = {"file", "project-relative", "legacy.png", "file:///media/legacy.png"};
            asset.width = 64;
            asset.height = 48;
            require(seed.project.addAsset(asset), "legacy asset");
            document::Document stage(std::move(seed.project));
            commands::CommandStack stageStack(stage);
            commands::Transaction layer("Add", stage.snapshot().revision());
            layer.emplace<commands::AddImageLayer>(seed.initialCompositionId, asset.id);
            require(stageStack.execute(std::move(layer)).succeeded(), "legacy image layer");
            document::Project legacy = stage.snapshot().project();
            legacy.findAsset(asset.id)->name = "legacy.svg";
            legacy.findAsset(asset.id)->locator.path = "legacy.svg";
            document::Document document(std::move(legacy));
            commands::CommandStack stack(document);
            ui::CompositionSession session(document, stack, seed.initialCompositionId);
            const auto& composition = *session.composition();
            const auto warning = QStringLiteral(
                "'legacy.svg' is an SVG source, not footage: add it again from Assets to rebuild the artwork");
            const document::NodeRecord* source = nullptr;
            for (const auto& node : composition.graph().nodes())
                if (node.typeId == "bloom.image-source")
                    source = &node;
            require(source != nullptr, "the legacy image source");
            require(document::svgFootageWarning(session.snapshot().project(), composition, *source).value_or("") ==
                        warning.toStdString(),
                    "the named warning is the same text everywhere");
            const auto layerId = composition.graph().layerOutputs().front().layerId;
            require(document::layerSvgFootageWarning(session.snapshot().project(), composition, layerId).has_value(),
                    "the layer carries the warning");

            // The card: warning badge and the named tooltip.
            runtime::NodeDefinitionRegistry definitions;
            require(runtime::registerBuiltInNodeDefinitions(definitions), "definitions");
            definitions.freeze();
            document::NodeDefinitionRegistry registry;
            require(document::registerBuiltInNodeDefinitions(registry), "registry");
            registry.freeze();
            ui::node_editor::NodeItem card(source->id, &session);
            card.refresh(*source, composition, {{0, 0}, 240, false, false}, registry);
            require(card.assetProblem() && card.svgFootageWarning() == warning && card.toolTip().endsWith(warning),
                    "the Image Source card shows the warning badge and names it");

            // Properties: a Warning row under the source section.
            ui::PropertiesEditor properties(session);
            properties.resize(420, 700);
            properties.show();
            session.selectLayer(layerId);
            QApplication::processEvents();
            auto* label = properties.findChild<QLabel*>(QStringLiteral("propertiesSvgFootageWarning"));
            require(label != nullptr && label->text() == warning, "Properties names the warning in the source section");

            // The Timeline row: a marker with the warning as its tooltip.
            runtime::SnapshotCompiler compiler(definitions);
            runtime::CpuCompositionEvaluator evaluator;
            runtime::CpuReferenceDisplayPreparer displayPreparer;
            runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
            runtime::TaskScheduler scheduler;
            ui::TaskUiBridge bridge(scheduler);
            ui::CompositionPreviewController preview(
                session, scheduler, bridge,
                ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer, qualifiedProcessorProvider));
            QWidget host;
            auto* layout = new QVBoxLayout(&host);
            auto* timeline = new ui::TimelineEditor(session, preview);
            layout->addWidget(timeline);
            host.resize(1200, 420);
            host.show();
            QApplication::processEvents();
            auto* stackWidget = timeline->layerStackForTest();
            require(!stackWidget->entries().empty() && stackWidget->entries().front().warning == warning,
                    "the Timeline entry carries the warning");
            auto* marker = stackWidget->findChild<QLabel*>(QStringLiteral("timelineWarningMarker"));
            require(marker != nullptr && marker->isVisible() && marker->toolTip() == warning,
                    "the Timeline row shows a warning marker with the name");
            preview.beginShutdown();
            bridge.beginShutdown();
        }
        std::cout << "PASS: an imported SVG asset adds again as artwork from every desktop route\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
