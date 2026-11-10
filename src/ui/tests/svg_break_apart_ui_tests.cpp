// #642: the desktop route for the two SVG import modes and Break Apart.
//  - the chooser: asked once per batch, As one layer by default, "Remember my choice" stores the
//    answer as the import preference, a remembered answer is used without a prompt, Cancel imports
//    no SVG;
//  - Preferences > Import changes the remembered answer;
//  - Break Apart is reachable from the Composition menu and from the Timeline's layer context menu,
//    reads the held SVG on the import worker, and is one undo step.

#include "import_progress_test_support.hpp"

#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/radio_group.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/main_window.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_ruler.hpp>

#include "svg_import_mode_dialog.hpp"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>

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

constexpr auto kSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 200 100">
<rect id="box" x="10" y="10" width="80" height="60" fill="#cc3333"/>
<path id="ring" fill="#336699" fill-rule="evenodd" d="M110 10H190V90H110Z M130 30H170V70H130Z"/></svg>
)svg";

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
    [[nodiscard]] std::size_t layers() const { return composition().graph().layerOutputs().size(); }
    // The top layer: the one an import just added.
    [[nodiscard]] document::LayerId topLayer() const {
        const auto& graph = composition().graph();
        const auto* stack = graph.merge(graph.layerStack().nodeId());
        require(stack != nullptr && !stack->entries().empty(), "a layer is present");
        return stack->entries().back().layerId;
    }
};

void rememberChoice(const ui::SvgImportChoice choice) {
    QSettings settings;
    auto preferences = ui::loadApplicationPreferences(settings);
    preferences.importSvgMode = choice;
    ui::saveApplicationPreferences(settings, preferences);
}

constexpr auto kPathNodes = "bloom.value-path";

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomSvgBreakApart"));
    QApplication::setApplicationName(QStringLiteral("SvgBreakApart"));
    QStandardPaths::setTestModeEnabled(true);
    ui::kit::installKinetikTheme(app);
    try {
        QTemporaryDir directory;
        require(directory.isValid(), "temporary directory");
        const QString first = directory.filePath("one.svg");
        const QString second = directory.filePath("two.svg");
        for (const auto& path : {first, second}) {
            QFile file(path);
            require(file.open(QIODevice::WriteOnly) && file.write(kSvg) > 0, "write the SVG");
        }
        QSettings().clear();

        // Preference round trip: ask by default, then the stored spelling.
        {
            require(ui::loadApplicationPreferences(QSettings{}).importSvgMode == ui::SvgImportChoice::Ask,
                    "SVG files ask by default");
            rememberChoice(ui::SvgImportChoice::Shapes);
            require(ui::loadApplicationPreferences(QSettings{}).importSvgMode == ui::SvgImportChoice::Shapes,
                    "the SVG import preference persists");
            QSettings().clear();
        }

        // The dialog: As one layer is selected, Remember is off, the note explains each choice.
        {
            QWidget owner;
            owner.show();
            ui::SvgImportModeDialog dialog(2, commands::kDefaultSvgImportMode, owner);
            require(dialog.choices()->currentIndex() == 0 && dialog.mode() == commands::SvgImportMode::Layer,
                    "the chooser opens on As one layer");
            require(!dialog.remember(), "Remember my choice is off at first");
            const auto layerNote = dialog.note()->text();
            dialog.choices()->setCurrentIndex(1);
            require(dialog.mode() == commands::SvgImportMode::Shapes && dialog.note()->text() != layerNote,
                    "the second option is As editable shapes and its note changes");
            dialog.rememberBox()->setChecked(true);
            require(dialog.remember(), "the check remembers the choice");
        }

        // Asked once per batch, with the number of SVG files; the answer applies to all of them.
        {
            Harness harness;
            std::vector<std::size_t> asked;
            harness.controller.setSvgImportChooserForTesting([&](const std::size_t count) {
                asked.push_back(count);
                return std::optional(commands::SvgImportMode::Shapes);
            });
            harness.controller.importFiles({first, second});
            require(waitUntil([&] { return !harness.controller.busy() && harness.count(kPathNodes) >= 4; }),
                    "both SVG files import");
            require(asked == std::vector<std::size_t>{2}, "the chooser is asked once for the batch of two");
            require(harness.count(document::kVectorArtworkNodeType) == 0 && harness.count(kPathNodes) == 4,
                    "the answer, editable shapes, applies to both files");
        }

        // Cancel imports no SVG.
        {
            Harness harness;
            const auto layersBefore = harness.layers();
            harness.controller.setSvgImportChooserForTesting(
                [](std::size_t) { return std::optional<commands::SvgImportMode>{}; });
            harness.controller.importFiles({first});
            QTest::qWait(100);
            require(!harness.controller.busy() && harness.layers() == layersBefore,
                    "cancelling the chooser imports nothing");
        }

        // As one layer, the default answer when nobody can be asked: one layer, one node.
        {
            Harness harness;
            const auto layersBefore = harness.layers();
            harness.controller.importFiles({first});
            require(waitUntil([&] { return !harness.controller.busy() && harness.layers() > layersBefore; }),
                    "the SVG imports");
            require(harness.count(document::kVectorArtworkNodeType) == 1 && harness.count(kPathNodes) == 0 &&
                        harness.layers() == layersBefore + 1,
                    "with no window to ask in, the default imports one layer and one node");
        }

        // The real chooser, over a window: Cancel imports nothing; choosing shapes and remembering
        // stores the answer; the next import uses it without opening the chooser.
        {
            Harness harness;
            QWidget owner;
            owner.setObjectName(QStringLiteral("chooserOwner"));
            owner.resize(400, 300);
            owner.show();
            owner.activateWindow();
            require(waitUntil([&] { return QApplication::activeWindow() == &owner; }, 5000),
                    "the owner window is active");
            std::optional<std::optional<commands::SvgImportMode>> answer;
            harness.controller.chooseSvgImportMode(
                1, [&](const std::optional<commands::SvgImportMode> mode) { answer = mode; });
            const auto shown = [&]() -> ui::SvgImportModeDialog* {
                for (auto* candidate : owner.findChildren<ui::SvgImportModeDialog*>())
                    if (candidate->isVisible())
                        return candidate;
                return nullptr;
            };
            auto* dialog = shown();
            require(dialog != nullptr && !answer, "the chooser opens and waits");
            require(dialog->mode() == commands::SvgImportMode::Layer, "it offers As one layer first");
            dialog->reject();
            require(waitUntil([&] { return answer.has_value(); }, 5000) && !*answer,
                    "Cancel answers nothing");
            require(ui::loadApplicationPreferences(QSettings{}).importSvgMode == ui::SvgImportChoice::Ask,
                    "Cancel remembers nothing");
            answer.reset();
            harness.controller.chooseSvgImportMode(
                1, [&](const std::optional<commands::SvgImportMode> mode) { answer = mode; });
            dialog = shown();
            require(dialog != nullptr, "the chooser opens again");
            dialog->choices()->setCurrentIndex(1);
            dialog->rememberBox()->setChecked(true);
            require(dialog->activate(ui::kit::KDialog::Role::Accept), "Import accepts the chooser");
            require(waitUntil([&] { return answer.has_value(); }, 5000) && *answer == commands::SvgImportMode::Shapes,
                    "the answer is As editable shapes");
            require(ui::loadApplicationPreferences(QSettings{}).importSvgMode == ui::SvgImportChoice::Shapes,
                    "Remember my choice stores the answer as the import preference");
            // Remembered: no chooser, the stored mode, at once.
            std::optional<commands::SvgImportMode> remembered;
            harness.controller.chooseSvgImportMode(
                1, [&](const std::optional<commands::SvgImportMode> mode) { remembered = mode; });
            require(remembered == commands::SvgImportMode::Shapes && shown() == nullptr,
                    "a remembered answer is used without a prompt");
            // And a real import uses it.
            harness.controller.setSvgImportChooserForTesting([](std::size_t) -> std::optional<commands::SvgImportMode> {
                throw std::runtime_error("the chooser must not be asked");
            });
            harness.controller.importFiles({first});
            require(waitUntil([&] { return !harness.controller.busy() && harness.count(kPathNodes) == 2; }),
                    "the remembered mode imports editable shapes");
            QSettings().clear();
        }

        // Break Apart from the Composition menu and from the Timeline's layer context menu.
        {
            rememberChoice(ui::SvgImportChoice::Layer);
            Harness harness;
            runtime::NodeDefinitionRegistry definitions;
            require(runtime::registerBuiltInNodeDefinitions(definitions), "node definitions");
            definitions.freeze();
            runtime::SnapshotCompiler compiler(definitions);
            runtime::CpuCompositionEvaluator evaluator;
            runtime::CpuReferenceDisplayPreparer displayPreparer;
            runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
            ui::CompositionPreviewController preview(
                harness.session, harness.scheduler, harness.bridge,
                ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer, qualifiedProcessorProvider));

            ui::EditorRegistry registry;
            for (const auto& [id, name] : {std::pair{"bloom.viewer", "Compositor"}, {"bloom.nodes", "Nodes"},
                                           {"bloom.timeline", "Timeline"}, {"bloom.assets", "Assets"},
                                           {"bloom.properties", "Properties"}})
                require(registry.registerEditor({.id = id, .displayName = QString::fromLatin1(name),
                                                 .create = [](QWidget* parent) { return new QLabel("stand-in", parent); }}),
                        "stand-in editors");
            ui::MainWindow window(registry, harness.session, harness.host);
            window.resize(1280, 800);
            window.show();
            QApplication::processEvents();
            auto* menuAction = window.findChild<QAction*>(QStringLiteral("compositionBreakApartSvgAction"));
            require(menuAction != nullptr, "Composition > Break Apart SVG exists");
            require(!menuAction->isEnabled(), "Break Apart SVG is disabled with no layer selected");

            harness.controller.importFiles({first});
            require(waitUntil([&] { return !harness.controller.busy() && harness.count(document::kVectorArtworkNodeType) == 1; }),
                    "the SVG imports as one layer");
            const auto layer = harness.topLayer();
            const auto* boundary = harness.composition().graph().findLayer(layer);
            require(boundary != nullptr && boundary->entrySlot.has_value(), "the layer is a Merge entry layer");
            harness.session.selectEntry({boundary->nodeId, *boundary->entrySlot});
            require(harness.controller.holdsVectorArtwork(layer) &&
                        harness.controller.selectedVectorArtworkLayer() == layer,
                    "the selected layer is found to hold a Vector Artwork");
            require(menuAction->isEnabled(), "Break Apart SVG is enabled with the SVG layer selected");

            // The Timeline's layer context menu offers it too.
            {
                QWidget host;
                auto* layout = new QVBoxLayout(&host);
                auto* timeline = new ui::TimelineEditor(harness.session, preview);
                layout->addWidget(timeline);
                host.resize(1200, 420);
                host.show();
                QApplication::processEvents();
                auto* stack = timeline->layerStackForTest();
                const QPoint rowPoint(stack->width() / 2, ui::kTimelineRowHeight / 2);
                QContextMenuEvent event(QContextMenuEvent::Mouse, rowPoint, stack->mapToGlobal(rowPoint));
                QApplication::sendEvent(stack, &event);
                auto* menu = stack->findChild<QMenu*>(QStringLiteral("timelineLayerContextMenu"));
                require(menu != nullptr && menu->isVisible(), "the layer context menu opens");
                auto* action = menu->findChild<QAction*>(QStringLiteral("timelineBreakApartSvgAction"));
                require(action != nullptr && action->isEnabled(), "the context menu offers Break Apart SVG for the SVG layer");
                menu->hide();
            }

            const auto nodesBefore = harness.composition().graph().nodes().size();
            const auto revisionBefore = harness.session.snapshot().revision();
            menuAction->trigger();
            require(waitUntil([&] { return !harness.controller.busy() && harness.count(document::kVectorArtworkNodeType) == 0; }),
                    "Break Apart replaces the Vector Artwork");
            require(harness.count(kPathNodes) == 2 && harness.composition().graph().findLayer(layer) != nullptr,
                    "the shapes are there and the layer is the same layer");
            require(harness.session.undoLabel() == QStringLiteral("Break Apart SVG"),
                    "Break Apart is one undo step named Break Apart SVG");
            require(harness.session.snapshot().revision() != revisionBefore, "it committed");
            require(!menuAction->isEnabled(), "the menu entry is disabled once nothing is left to break apart");
            require(harness.session.undo() && harness.count(document::kVectorArtworkNodeType) == 1 &&
                        harness.count(kPathNodes) == 0 && harness.composition().graph().nodes().size() == nodesBefore,
                    "one undo gives the one-layer SVG back");
            require(menuAction->isEnabled(), "and the menu entry is available again");

            // The context menu's action does the same.
            {
                QWidget host;
                auto* layout = new QVBoxLayout(&host);
                auto* timeline = new ui::TimelineEditor(harness.session, preview);
                layout->addWidget(timeline);
                host.resize(1200, 420);
                host.show();
                QApplication::processEvents();
                auto* stack = timeline->layerStackForTest();
                const QPoint rowPoint(stack->width() / 2, ui::kTimelineRowHeight / 2);
                QContextMenuEvent event(QContextMenuEvent::Mouse, rowPoint, stack->mapToGlobal(rowPoint));
                QApplication::sendEvent(stack, &event);
                auto* menu = stack->findChild<QMenu*>(QStringLiteral("timelineLayerContextMenu"));
                auto* action = menu ? menu->findChild<QAction*>(QStringLiteral("timelineBreakApartSvgAction")) : nullptr;
                require(action != nullptr, "the context menu offers Break Apart SVG again");
                action->trigger();
                require(waitUntil([&] { return !harness.controller.busy() && harness.count(document::kVectorArtworkNodeType) == 0; }),
                        "Break Apart from the context menu replaces the Vector Artwork");
                require(harness.count(kPathNodes) == 2, "the context menu breaks it into the same shapes");
            }
            // The layer is no longer an SVG layer: nothing more to offer.
            require(!harness.controller.holdsVectorArtwork(layer) && !harness.controller.selectedVectorArtworkLayer(),
                    "a broken-apart layer holds no Vector Artwork");
            preview.beginShutdown();
        }

        // Several selected SVG layers break apart together, in one undo step; one scaled up is named in
        // the job log as resampled.
        {
            rememberChoice(ui::SvgImportChoice::Layer);
            Harness harness;
            harness.controller.importFiles({first});
            require(waitUntil([&] { return !harness.controller.busy() && harness.count(document::kVectorArtworkNodeType) == 1; }),
                    "the first SVG imports");
            harness.controller.importFiles({second});
            require(waitUntil([&] { return !harness.controller.busy() && harness.count(document::kVectorArtworkNodeType) == 2; }),
                    "the second SVG imports");
            std::vector<document::LayerId> both;
            for (const auto& entry : harness.composition().graph().layerStack().entries())
                both.push_back(entry.layerId);
            require(both.size() == 2, "two layers");
            // The second layer is scaled up 2x.
            {
                const auto& graph = harness.composition().graph();
                const auto* boundary = graph.findLayer(both.front());
                const auto* binding = boundary ? document::layerParameterBinding(graph, *boundary, document::kScaleParameterRole) : nullptr;
                require(binding != nullptr, "the layer has a scale");
                commands::Transaction scale("Scale", harness.session.snapshot().revision());
                scale.emplace<commands::SetParameterSource>(
                    harness.session.compositionId(), binding->parameterId,
                    document::ConstantValueSource{document::ParameterValue{document::Vec2d{2.0, 2.0}}});
                require(harness.session.executeTransaction(std::move(scale)).succeeded(), "scale the layer up");
            }
            for (std::size_t index = 0; index < both.size(); ++index) {
                const auto* boundary = harness.composition().graph().findLayer(both[index]);
                harness.session.selectEntry({boundary->nodeId, *boundary->entrySlot}, index != 0);
            }
            require(harness.controller.selectedVectorArtworkLayers().size() == 2, "both SVG layers are selected");
            const auto revision = harness.session.snapshot().revision();
            harness.diagnostics.clear();
            harness.controller.breakApartSvg(harness.session.compositionId(), harness.controller.selectedVectorArtworkLayers());
            require(waitUntil([&] { return !harness.controller.busy() && harness.count(document::kVectorArtworkNodeType) == 0; }),
                    "both layers are broken apart");
            require(harness.count(kPathNodes) == 4, "both SVGs became their shapes");
            require(harness.session.snapshot().revision().value() == revision.value() + 1,
                    "one transaction commits for both");
            const auto noted = std::ranges::count_if(harness.diagnostics, [](const QString& line) {
                return line.contains(QStringLiteral("scaled up or turned")) && line.contains(QStringLiteral("softer"));
            });
            require(noted == 1, "only the scaled layer is named as resampled, once, as a note");
            require(harness.session.undo() && harness.count(document::kVectorArtworkNodeType) == 2 && harness.count(kPathNodes) == 0,
                    "one undo gives both one-layer SVGs back");
        }
        QSettings().clear();
        std::cout << "svg break apart ui: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
