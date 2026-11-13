// Review captures for the UI bug batch (#685, #686, #690, #691, #696, #697, #702, #704, #705, #706).
// A tool, not a test: it writes <issue>-<phase>-<name>.png (and a node-type probe for #704) into the
// directory named by BLOOM_BUG_CAPTURE_DIR (or the first argument). BLOOM_BUG_CAPTURE_PHASE names the
// phase ("before" on the unmodified code, "after" on the fix; default "after"). It only uses
// interfaces that exist before the fixes, so the same file builds on both.
//   bloom_bug_batch_capture [directory]
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#include "export_test_support.hpp"
#include "node_interaction_test_support.hpp"
#include "native_gesture_support.hpp"
#include "window_fixture.hpp"

#include "export_job_lines.hpp"
#include "export_render_list.hpp"
#include "preferences_window.hpp"

#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QListWidget>
#include <bloom/ui/kit/tokens.hpp>
#include <set>
#include <QPainter>
#include <QStandardPaths>
#include <QTextStream>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/channel_nodes.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/document/node_definition_registry.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/layer_bounds_query.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <filesystem>
#include <fstream>
#include <bloom/ui/application_preferences.hpp>
#include <bloom/ui/context_bar.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/export_editor.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/layer_alignment_actions.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/kit/value_field.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/timeline_graph_view.hpp>
#include <bloom/ui/timeline_ruler.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/window_status_bar.hpp>

#include <iostream>
#include <map>

namespace {
using namespace bloom;
using namespace bloom::ui;

QString gDirectory;
QString gPhase;
int gFailures = 0;

// Delivers everything already queued (layout, polish, deferred updates) until it is quiet. The
// captures need the UI to have processed its own posted work, not a stretch of wall-clock time.
void settle() {
    for (int pass = 0; pass < 3; ++pass) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}

void save(const QImage& image, const QString& issue, const QString& name) {
    const auto path = QStringLiteral("%1/%2-%3-%4.png").arg(gDirectory, issue, gPhase, name);
    if (image.isNull() || !image.save(path)) {
        std::cerr << "could not write " << path.toStdString() << '\n';
        ++gFailures;
        return;
    }
    std::cout << path.toStdString() << ' ' << image.width() << 'x' << image.height() << '\n';
}

QImage grabWidget(QWidget* widget) {
    widget->ensurePolished();
    return widget->grab().toImage();
}

// A popup menu grabbed whole, on a mid-gray plate so a translucent frame reads.
QImage grabMenu(QMenu* menu) {
    menu->popup(QPoint(20, 20));
    settle();
    QImage shot = menu->grab().toImage();
    menu->hide();
    return shot;
}

// ---- #702, #705, #704, #697: the Node editor -------------------------------------------------

void captureAddMenu() {
    test::Fixture f;
    std::unique_ptr<QMenu> menu(f.editor.contextMenuForTest());
    auto* addMenu = menu->findChild<QMenu*>(QStringLiteral("nodeAddMenu"));
    auto* section = menu->findChild<QMenu*>(QStringLiteral("nodeAddCategoryMenu.Text & Paths"));
    if (section != nullptr)
        save(grabMenu(section), QStringLiteral("702"), QStringLiteral("add-node-text-and-paths"));
    else
        ++gFailures;
    if (addMenu != nullptr)
        save(grabMenu(addMenu), QStringLiteral("702"), QStringLiteral("add-node-menu"));
    save(grabMenu(menu.get()), QStringLiteral("702"), QStringLiteral("canvas-menu"));
}

void captureMarquee() {
    test::Fixture f;
    (void)f.add(document::kSolidSourceNodeType, {0, 100});
    (void)f.add(document::kLayerOutputNodeType, {280, 100});
    for (const int percent : {50, 200}) {
        f.view()->zoomToPercent(percent);
        settle();
        QPoint topLeft(60, 60);
        for (int y = 30; y < 500; y += 40)
            for (int x = 30; x < 800; x += 40) {
                bool empty = true;
                for (int dy = 0; dy <= 260 && empty; dy += 20)
                    for (int dx = 0; dx <= 320 && empty; dx += 20)
                        empty = f.view()->items(QPoint(x + dx, y + dy)).isEmpty();
                if (empty) {
                    topLeft = QPoint(x, y);
                    y = 1000;
                    break;
                }
            }
        const QPointF origin = f.view()->mapToScene(topLeft);
        const QPointF corner = f.view()->mapToScene(topLeft + QPoint(300, 200));
        f.press(origin);
        f.move(corner);
        settle();
        QImage image = f.view()->viewport()->grab().toImage();
        // Crop to the marquee's corner so the border reads at full size.
        const QRect crop(topLeft - QPoint(20, 20), QSize(340, 240));
        save(image.copy(crop), QStringLiteral("697"), QStringLiteral("marquee-zoom%1").arg(percent));
        f.release(corner);
        settle();
    }
}

void captureRightClick() {
    test::Fixture f;
    const auto a = f.add(document::kSolidSourceNodeType, {0, 100});
    const auto b = f.add(document::kLayerOutputNodeType, {280, 100});
    const auto c = f.add(document::kSolidSourceNodeType, {0, 320});
    f.session.selectNodes({a, b}, a);
    settle();
    const auto header = [&](const document::NodeId id) {
        const auto box = f.card(id)->sceneBoundingRect();
        return QPointF(box.center().x(), box.top() + 8);
    };
    const QPointF onA = header(a);
    const QPoint viewportPoint = f.view()->mapFromScene(onA);
    const auto rightClick = [&](const QPointF scenePoint) {
        const QPoint vp = f.view()->mapFromScene(scenePoint);
        f.press(scenePoint, Qt::NoModifier, Qt::RightButton);
        std::cout << "after press selection=" << f.session.selectedNodes().size() << '\n';
        QContextMenuEvent menuEvent(QContextMenuEvent::Mouse, vp, f.view()->viewport()->mapToGlobal(vp));
        QCoreApplication::sendEvent(f.view()->viewport(), &menuEvent);
        std::cout << "after menu selection=" << f.session.selectedNodes().size() << '\n';
        f.release(scenePoint, Qt::NoModifier, Qt::RightButton);
        settle();
    };
    (void)viewportPoint;
    const auto shotMenu = [&](const QString& name) {
        QMenu* open = nullptr;
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* menu = qobject_cast<QMenu*>(widget);
                menu != nullptr && menu->isVisible() && menu->objectName() == QStringLiteral("nodeContextMenu"))
                open = menu;
        QImage editor = f.editor.grab().toImage();
        if (open != nullptr) {
            const QImage popup = open->grab().toImage();
            QPainter painter(&editor);
            painter.drawImage(f.view()->mapTo(&f.editor, f.view()->mapFromScene(onA)) + QPoint(8, 8), popup);
            open->close();
        }
        std::cout << name.toStdString() << " selection=" << f.session.selectedNodes().size() << '\n';
        save(editor, QStringLiteral("705"), name);
    };
    rightClick(onA);
    shotMenu(QStringLiteral("rightclick-in-selection"));
    f.session.selectNodes({a, b}, a);
    settle();
    rightClick(header(c));
    shotMenu(QStringLiteral("rightclick-outside-selection"));
}

// Every registered node type: can it be added, removed, dissolved (alone in the graph)?
void probeRemoval() {
    QFile out(gDirectory + QStringLiteral("/704-") + gPhase + QStringLiteral("-node-type-probe.txt"));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream text(&out);
    test::Fixture f;
    text << "type | added | canRemove | removeOk | canDissolve | dissolveOk\n";
    for (const auto& definition : document::builtInNodeDefinitions().definitions()) {
        const std::string type = definition.key.typeId;
        std::optional<document::NodeId> id;
        try {
            commands::Transaction add("Probe add", f.session.snapshot().revision());
            add.emplace<commands::AddNode>(f.session.compositionId(), type, document::Vec2d{0, 0});
            const auto result = f.stack.execute(std::move(add));
            if (result.succeeded())
                id = result.outputId<document::NodeId>(commands::kAddNodeOutput);
        } catch (const std::exception&) {
        }
        if (!id) {
            text << type.c_str() << " | no | - | - | - | -\n";
            continue;
        }
        f.session.rebind(f.document, f.stack, f.session.compositionId());
        const auto& composition = *f.session.composition();
        const bool canRemove = commands::canRemoveNodes(composition, {*id});
        const bool canDissolve = commands::canDissolveNode(composition, *id);
        const auto attempt = [&](const bool dissolve) {
            commands::Transaction t("Probe", f.session.snapshot().revision());
            if (dissolve)
                t.emplace<commands::DissolveNode>(f.session.compositionId(), *id);
            else
                t.emplace<commands::RemoveNodes>(f.session.compositionId(), std::set{*id});
            const auto result = f.stack.execute(std::move(t));
            const bool ok = result.succeeded() && f.session.composition() != nullptr;
            if (result.changed()) {
                f.session.rebind(f.document, f.stack, f.session.compositionId());
                (void)f.stack.undo();
                f.session.rebind(f.document, f.stack, f.session.compositionId());
            }
            return ok;
        };
        const bool removeOk = attempt(false);
        const bool dissolveOk = attempt(true);
        text << type.c_str() << " | yes | " << canRemove << " | " << removeOk << " | "
             << canDissolve << " | " << dissolveOk << '\n';
        // Leave the graph as it was: the probe node itself goes.
        commands::Transaction clean("Probe clean", f.session.snapshot().revision());
        clean.emplace<commands::RemoveNodes>(f.session.compositionId(), std::set{*id});
        (void)f.stack.execute(std::move(clean));
        f.session.rebind(f.document, f.stack, f.session.compositionId());
    }
    std::cout << "704 probe written\n";
}

// ---- #686: the tool strip --------------------------------------------------------------------

void captureContextBar() {
    test::Fixture f;
    ContextBar bar(f.session, nullptr);
    bar.resize(1100, 44);
    bar.show();
    settle();
    f.session.clearSelection();
    settle();
    save(grabWidget(&bar), QStringLiteral("686"), QStringLiteral("toolbar-no-cryptomatte"));
    const auto crypto = f.add(document::kCryptomatteNodeType, {0, 100});
    f.session.selectNode(crypto);
    settle();
    save(grabWidget(&bar), QStringLiteral("686"), QStringLiteral("toolbar-cryptomatte-selected"));
}

// ---- #706: the dropdown shadow ---------------------------------------------------------------

void captureDropdownShadow() {
    QWidget window;
    window.setWindowTitle(QStringLiteral("Shadow"));
    auto* dropdown = new kit::KDropdown(&window);
    for (const char* name : {"Normal", "Add", "Multiply", "Screen", "Overlay", "Darken"})
        dropdown->addItem(QString::fromLatin1(name));
    dropdown->move(40, 30);
    window.resize(360, 300);
    window.show();
    settle();
    dropdown->showPopup();
    settle();
    if (auto* popup = dropdown->findChild<QWidget*>(QStringLiteral("kDropdownPopup"))) {
        const QImage shot = popup->grab().toImage();
        for (const auto& [name, color] : {std::pair{"light", QColor(235, 235, 235)},
                                          std::pair{"dark", QColor(48, 48, 52)}}) {
            QImage plate(shot.size(), QImage::Format_ARGB32_Premultiplied);
            plate.fill(color);
            QPainter painter(&plate);
            painter.drawImage(0, 0, shot);
            painter.end();
            save(plate, QStringLiteral("706"), QStringLiteral("dropdown-shadow-%1").arg(QLatin1String(name)));
        }
        const QString flags = QStringLiteral("flags=0x%1")
                                  .arg(static_cast<quint32>(popup->windowFlags()), 0, 16);
        std::cout << "706 popup " << flags.toStdString() << '\n';
    } else {
        std::cerr << "706: no popup to capture\n";
        ++gFailures;
    }
}

// ---- #690, #691, #685: the Viewer footer, the Export panel, the status bar ---------------------

void captureWindowSurfaces() {
    test::WindowFixture fixture;
    EditorArea* viewer = nullptr;
    for (auto* area : fixture.window->findChildren<EditorArea*>())
        if (area->editorId() == "bloom.viewer")
            viewer = area;
    if (viewer == nullptr) {
        ++gFailures;
        return;
    }
    Q_EMIT viewer->maximizeRequested(viewer);
    settle();
    for (const auto& [name, size] : {std::pair{"normal", QSize(1400, 800)},
                                     std::pair{"narrow", QSize(640, 600)}}) {
        for (int pass = 0; pass < 3; ++pass) {
            settle();
            const QSize delta = size - viewer->size();
            if (!delta.isNull())
                fixture.window->resize(fixture.window->size() + delta);
        }
        settle();
        if (auto* footer = viewer->findChild<QWidget*>(QStringLiteral("editorFooter")))
            save(grabWidget(footer), QStringLiteral("690"), QStringLiteral("viewer-footer-%1").arg(QLatin1String(name)));
        if (auto* field = viewer->findChild<kit::KValueField*>(QStringLiteral("viewerExposure")))
            std::cout << "690 exposure " << name << " visible=" << field->isVisible()
                      << " width=" << field->width() << " text=" << field->paintedText().toStdString() << '\n';
    }
    // #685: the main window's status bar.
    if (auto* bar = fixture.window->findChild<WindowStatusBar*>())
        save(grabWidget(bar), QStringLiteral("685"), QStringLiteral("status-bar"));
}

void captureExportPanel() {
    using namespace bloom::ui::test;
    auto seed = document::makeNewProject("Export Capture", "Shot010", core::RationalTime::fromInteger(90));
    document::Document doc(std::move(seed.project));
    commands::CommandStack stack(doc);
    CompositionSession session(doc, stack, seed.initialCompositionId);
    FakeExportPort port;
    ExportQueueAdapter adapter(port);
    EditorRegistry registry;
    (void)registerExportEditor(registry, session, &adapter);
    EditorArea area(registry, "bloom.export", QString{});
    area.resize(2000, 520);
    area.show();
    settle();
    auto* editor = area.findChild<ExportEditor*>();
    if (editor == nullptr) {
        ++gFailures;
        return;
    }
    const auto composition = session.compositionId();
    port.publish([&](host::ExportQueueSnapshot& s) {
        s.commands = allAvailable();
        auto output = makeOutput(11, "builtin:prores-422hq-mov", document::ExportFormat::ProResMov,
                                 "Exports/Shot010.mov");
        auto job = makeJob(1, "Shot010", composition, {output});
        job.resolvedRange = host::ExportResolvedRange{0, 2155};
        job.state = host::ExportJobState::Rendering;
        job.run = host::ExportRunLink{.run = 1};
        job.progress = {.framesCompleted = 403, .framesTotal = 2156, .outputIndex = 0, .outputCount = 1,
                        .currentFrame = 403, .fraction = 0.187,
                        .elapsed = std::chrono::milliseconds(28000),
                        .remaining = std::chrono::milliseconds(122000),
                        .route = host::ExportDeviceRoute::Gpu};
        auto second = makeJob(2, "Shot020", composition, {makeOutput(21)});
        s.jobs = {job, second};
        s.activeJob = job.id;
    });
    adapter.drain();
    editor->setCurrentRenderExpanded(true);
    for (const int width : {900, 2000}) {
        area.resize(width, 520);
        settle();
        save(area.grab().toImage(), QStringLiteral("691"), QStringLiteral("export-panel-%1").arg(width));
    }
}

// ---- #696: pinch to zoom ---------------------------------------------------------------------

void capturePinch() {
    {
        test::Fixture f;
        (void)f.add(document::kSolidSourceNodeType, {0, 100});
        (void)f.add(document::kLayerOutputNodeType, {280, 100});
        settle();
        const double before = f.view()->zoomFactor();
        save(f.view()->viewport()->grab().toImage(), QStringLiteral("696"), QStringLiteral("nodes-0-start"));
        for (int step = 0; step < 4; ++step)
            test::sendPinch(f.view()->viewport(), QPointF(300, 200), 0.15);
        settle();
        std::cout << "696 nodes zoom " << before << " -> " << f.view()->zoomFactor() << '\n';
        save(f.view()->viewport()->grab().toImage(), QStringLiteral("696"), QStringLiteral("nodes-1-pinched"));
    }
    test::WindowFixture fixture;
    auto* ruler = fixture.window->findChild<TimelineRuler*>();
    auto* lanes = fixture.window->findChild<QWidget*>(QStringLiteral("timelineLaneRegion"));
    auto* timeline = fixture.window->findChild<TimelineEditor*>();
    if (ruler != nullptr && lanes != nullptr && timeline != nullptr) {
        settle();
        const auto span = [&] {
            const auto axis = ruler->axisForWidth(ruler->width());
            return axis ? axis->t1 - axis->t0 : 0.0;
        };
        const double before = span();
        save(timeline->grab().toImage(), QStringLiteral("696"), QStringLiteral("timeline-0-start"));
        for (int step = 0; step < 4; ++step)
            test::sendPinch(lanes, QPointF(lanes->width() / 2.0, 20), 0.15);
        settle();
        std::cout << "696 timeline span " << before << " -> " << span() << '\n';
        save(timeline->grab().toImage(), QStringLiteral("696"), QStringLiteral("timeline-1-pinched"));
    } else {
        std::cerr << "696: no timeline\n";
        ++gFailures;
    }
    if (auto* viewer = fixture.window->findChild<ViewerEditor*>()) {
        settle();
        const double before = viewer->viewTransformForTest().zoom;
        save(viewer->grab().toImage(), QStringLiteral("696"), QStringLiteral("viewer-0-start"));
        for (int step = 0; step < 4; ++step)
            test::sendPinch(viewer, QPointF(viewer->width() / 2.0, viewer->height() / 2.0), 0.15);
        settle();
        std::cout << "696 viewer zoom " << before << " -> " << viewer->viewTransformForTest().zoom
                  << '\n';
        save(viewer->grab().toImage(), QStringLiteral("696"), QStringLiteral("viewer-1-pinched"));
    }
}

// ---- #693: which layers of an imported SVG have bounds ----------------------------------------

void probeSvgBounds() {
    const auto svg = std::filesystem::temp_directory_path() / "bug-batch-logo.svg";
    std::ofstream(svg, std::ios::binary)
        << R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120"><circle cx="60" cy="60" r="44" fill="#ee3377"/><g id="box"><rect x="110" y="26" width="72" height="68" rx="10" fill="#3377dd"/><path d="M120 80 L146 40 L172 80 Z" fill="#ffffff"/></g></svg>)svg";
    runtime::NodeDefinitionRegistry definitions;
    (void)runtime::registerBuiltInNodeDefinitions(definitions);
    definitions.freeze();
    runtime::SnapshotCompiler compiler(definitions);
    runtime::CpuCompositionEvaluator evaluator;
    QFile out(gDirectory + QStringLiteral("/693-") + gPhase + QStringLiteral("-svg-bounds-probe.txt"));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream text(&out);
    for (const auto mode : {commands::SvgImportMode::Layer, commands::SvgImportMode::Shapes}) {
        auto seed = document::makeNewProject("Svg", "Main", core::RationalTime::fromInteger(10));
        document::Document doc(std::move(seed.project));
        commands::CommandStack stack(doc);
        commands::Transaction transaction("Import SVG", doc.snapshot().revision());
        transaction
            .emplace<commands::AddSvgArtwork>(seed.initialCompositionId, nullptr, svg, "logo", true, mode)
            .withProjectDirectory(svg.parent_path());
        const auto result = stack.execute(std::move(transaction));
        text << (mode == commands::SvgImportMode::Layer ? "mode=layer" : "mode=shapes")
             << " import=" << result.succeeded() << '\n';
        const auto snapshot = doc.snapshot();
        const auto* composition = snapshot.project().findComposition(seed.initialCompositionId);
        if (composition == nullptr)
            continue;
        const auto bounds = runtime::queryLayerBounds(
            compiler, evaluator,
            {.snapshot = snapshot, .composition = seed.initialCompositionId,
             .time = core::RationalTime::fromInteger(0), .layers = {}});
        text << "  query ok=" << bounds.succeeded() << " bounds=" << bounds.bounds.size() << " "
             << bounds.diagnostic.c_str() << '\n';
        for (const auto& layer : composition->graph().layerOutputs()) {
            const auto found = std::ranges::find(bounds.bounds, layer.layerId,
                                                 &runtime::EvaluatedOperationBounds::layerId);
            text << "  layer " << layer.layerId.value() << " '" << layer.name.c_str()
                 << "' hasBounds=" << (found != bounds.bounds.end()) << '\n';
        }
    }
    std::cout << "693 probe written\n";
}

// The toolbar's alignment actions with an SVG layer, or a part of an editable SVG, selected.
void probeSvgAlignmentUi() {
    const auto svg = std::filesystem::temp_directory_path() / "bug-batch-logo-ui.svg";
    std::ofstream(svg, std::ios::binary)
        << R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120"><circle cx="60" cy="60" r="44" fill="#ee3377"/><g id="box"><rect x="110" y="26" width="72" height="68" rx="10" fill="#3377dd"/><path d="M120 80 L146 40 L172 80 Z" fill="#ffffff"/></g></svg>)svg";
    QFile out(gDirectory + QStringLiteral("/693-") + gPhase + QStringLiteral("-svg-alignment-ui.txt"));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream text(&out);
    for (const auto mode : {commands::SvgImportMode::Layer, commands::SvgImportMode::Shapes}) {
        test::WindowFixture fixture([&](test::WindowFixture& f) {
            commands::Transaction transaction("Import SVG", f.session.snapshot().revision());
            transaction
                .emplace<commands::AddSvgArtwork>(f.session.compositionId(), nullptr, svg, "logo", true, mode)
                .withProjectDirectory(svg.parent_path());
            (void)f.session.executeTransaction(std::move(transaction));
            (void)f.session.addSolidLayer("Backdrop", {0.2, 0.2, 0.2, 1.0});
        });
        LayerAlignmentActions actions(fixture.session, fixture.preview.get());
        settle();
        const auto report = [&](const QString& what, const std::vector<document::LayerId>& layers) {
            std::set<document::NodeId> nodes;
            for (const auto id : layers)
                if (const auto* boundary = fixture.session.composition()->graph().findLayer(id))
                    nodes.insert(boundary->nodeId);
            if (nodes.empty())
                return;
            for (const auto layer : layers)
                fixture.session.selectLayer(layer);
            settle();
            text << "   nodes=" << fixture.session.selectedNodes().size() << " entries="
                 << fixture.session.selectedEntries().size() << " selectedLayers=" << actions.selectedLayers().size() << '\n';
            for (const auto& entry : fixture.session.selectedEntries()) {
                const auto* viaDocument = document::mergeEntryLayer(fixture.session.composition()->graph(), entry.merge, entry.slot);
                text << "   entry merge=" << entry.merge.value() << " slot=" << entry.slot.value()
                     << " mergeEntryLayer=" << (viaDocument ? static_cast<long long>(viaDocument->layerId.value()) : -1LL)
                     << " entryLayer=" << (fixture.session.entryLayer(entry) ? static_cast<long long>(fixture.session.entryLayer(entry)->value()) : -1LL) << '\n';
            }
            for (const auto id : layers)
                if (const auto* boundary = fixture.session.composition()->graph().findLayer(id))
                    text << "   boundary " << id.value() << " nodeId=" << boundary->nodeId.value() << " entryOwned=" << boundary->entryOwned()
                         << " slot=" << (boundary->entrySlot ? static_cast<long long>(boundary->entrySlot->value()) : -1LL) << '\n';
            for (const auto node : fixture.session.selectedNodes())
                text << "   node " << node.value() << " layerForNode="
                     << (fixture.session.layerForNode(node) ? static_cast<long long>(fixture.session.layerForNode(node)->value()) : -1LL) << '\n';
            auto* left = actions.action(commands::LayerArrangement::AlignLeft);
            text << (mode == commands::SvgImportMode::Layer ? "layer " : "shapes ") << what.toStdString().c_str()
                 << ": AlignLeft enabled=" << left->isEnabled() << " tip="
                 << left->toolTip().replace('\n', " / ").toStdString().c_str() << '\n';
        };
        std::vector<document::LayerId> svgLayers;
        std::vector<document::LayerId> parts;
        for (const auto& layer : fixture.session.composition()->graph().layerOutputs()) {
            if (layer.name == "Backdrop")
                continue;
            if (layer.name == "logo" || layer.name == "box")
                svgLayers.push_back(layer.layerId);
            else if (layer.name == "circle" || layer.name == "rect" || layer.name == "path")
                parts.push_back(layer.layerId);
        }
        // With the Backdrop, "to the composition" is the default target: one layer is enough.
        for (const auto id : svgLayers)
            report(QStringLiteral("svg layer/group %1").arg(id.value()), {id});
        for (const auto id : parts)
            report(QStringLiteral("part %1").arg(id.value()), {id});
    }
    std::cout << "693 ui probe written\n";
}

// ---- #685: which widgets repaint while time advances -------------------------------------------

class PaintCounter final : public QObject {
  public:
    std::map<QString, int> counts;
    QWidget* statusBar = nullptr;
    QWidget* nodes = nullptr;
    QWidget* properties = nullptr;
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::Paint) {
            if (event->type() != QEvent::Timer && event->type() != QEvent::MetaCall &&
                event->type() != QEvent::UpdateLater && event->type() != QEvent::Polish) {
                auto* other = qobject_cast<QWidget*>(watched);
                if (other != nullptr && statusBar != nullptr && statusBar->isAncestorOf(other) &&
                    (other->objectName() == QStringLiteral("windowStatusBarMessage") ||
                     other->objectName() == QStringLiteral("windowStatusBarViewer") ||
                     other->objectName() == QStringLiteral("windowStatusBarExposure")))
                    ++counts[QStringLiteral("event %1 type %2").arg(other->objectName()).arg(static_cast<int>(event->type()))];
            }
            return false;
        }
        auto* widget = qobject_cast<QWidget*>(watched);
        if (widget == nullptr)
            return false;
        const char* area = nullptr;
        if (statusBar != nullptr && (widget == statusBar || statusBar->isAncestorOf(widget)))
            area = "status";
        else if (nodes != nullptr && (widget == nodes || nodes->isAncestorOf(widget)))
            area = "nodes";
        else if (properties != nullptr && (widget == properties || properties->isAncestorOf(widget)))
            area = "properties";
        if (area != nullptr)
            ++counts[QStringLiteral("%1 %2 '%3'").arg(QLatin1String(area), QLatin1String(widget->metaObject()->className()), widget->objectName())];
        return false;
    }
};

void probeFlicker() {
    test::WindowFixture fixture;
    PaintCounter counter;
    counter.statusBar = fixture.window->findChild<WindowStatusBar*>();
    for (auto* area : fixture.window->findChildren<EditorArea*>()) {
        if (area->editorId() == "bloom.nodes")
            counter.nodes = area;
        if (area->editorId() == "bloom.properties")
            counter.properties = area;
    }
    settle();
    qApp->installEventFilter(&counter);
    constexpr int kFrames = 24;
    for (int frame = 1; frame <= kFrames; ++frame) {
        if (const auto time = core::RationalTime::create(frame, 24))
            static_cast<void>(fixture.session.setCurrentTime(*time));
        settle();
    }
    qApp->removeEventFilter(&counter);
    QFile out(gDirectory + QStringLiteral("/685-") + gPhase + QStringLiteral("-paint-counts.txt"));
    if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream text(&out);
        text << "frames advanced: " << kFrames << '\n';
        for (const auto& [name, count] : counter.counts)
            text << count << "  " << name << '\n';
    }
    for (const auto& [name, count] : counter.counts)
        std::cout << "685 " << count << "  " << name.toStdString() << '\n';
}

// The toolbar's alignment group and the Node menu with an SVG layer / a driver node selected.
void captureAlignToolbar() {
    const auto svg = std::filesystem::temp_directory_path() / "bug-batch-logo-bar.svg";
    std::ofstream(svg, std::ios::binary)
        << R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120"><circle cx="60" cy="60" r="44" fill="#ee3377"/><g id="box"><rect x="110" y="26" width="72" height="68" rx="10" fill="#3377dd"/></g></svg>)svg";
    test::WindowFixture fixture([&](test::WindowFixture& f) {
        commands::Transaction transaction("Import SVG", f.session.snapshot().revision());
        transaction
            .emplace<commands::AddSvgArtwork>(f.session.compositionId(), nullptr, svg, "logo", true,
                                              commands::SvgImportMode::Shapes)
            .withProjectDirectory(svg.parent_path());
        (void)f.session.executeTransaction(std::move(transaction));
    });
    for (const char* name : {"logo", "rect"}) {
        for (const auto& layer : fixture.session.composition()->graph().layerOutputs())
            if (layer.name == name)
                fixture.session.selectLayer(layer.layerId);
        settle();
        for (auto* widget : fixture.window->findChildren<QWidget*>(QStringLiteral("contextBarAlign")))
            if (widget->isVisible()) {
                save(grabWidget(widget), QStringLiteral("693"), QStringLiteral("align-buttons-%1").arg(QLatin1String(name)));
                break;
            }
    }
}

void captureDriverMenu() {
    test::Fixture f;
    const auto scalar = f.add(document::kScalarValueNodeType, {0, 400});
    const auto layer = f.add(document::kLayerOutputNodeType, {280, 400});
    (void)f.edit<commands::ConnectPorts>(
        document::OutputPortRef{scalar, std::string(document::kValuePortName)},
        document::InputPortRef{document::NodeInputRef{layer, std::string(document::kOpacityParameterRole)}});
    f.session.selectNode(scalar);
    settle();
    std::unique_ptr<QMenu> menu(f.editor.contextMenuForTest(true));
    save(grabMenu(menu.get()), QStringLiteral("704"), QStringLiteral("driver-node-menu"));
    std::cout << "704 driver menu actions:";
    for (auto* action : menu->actions())
        std::cout << " [" << action->text().toStdString() << ']';
    std::cout << '\n';
}

} // namespace

// Lane captures of the main window (#687, #700 the top strip with the workspace dropdown; #712 an
// SVG layer selected in the Viewer; #716 the status bar after the Output node is deleted) and of
// the Preferences Shortcuts page (#688).
void captureLaneWindow() {
    const auto svg = std::filesystem::temp_directory_path() / "lane-capture-logo.svg";
    std::ofstream(svg, std::ios::binary)
        << R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="120"><circle cx="60" cy="60" r="44" fill="#ee3377"/><rect x="110" y="26" width="70" height="60" fill="#3377ee"/></svg>)svg";
    test::WindowFixture fixture([&](test::WindowFixture& f) {
        commands::Transaction transaction("Import SVG", f.session.snapshot().revision());
        transaction
            .emplace<commands::AddSvgArtwork>(f.session.compositionId(), nullptr, svg, "logo", true,
                                              commands::SvgImportMode::Layer)
            .withProjectDirectory(svg.parent_path());
        (void)f.session.executeTransaction(std::move(transaction));
    });
    settle();
    for (const auto& layer : fixture.session.composition()->graph().layerOutputs())
        if (layer.name == "logo")
            fixture.session.selectLayer(layer.layerId);
    settle();
    const QImage window = fixture.window->grab().toImage();
    save(window, QStringLiteral("687"), QStringLiteral("window"));
    const int strip = static_cast<int>(kit::px(kit::Size::Control) * 3 * window.devicePixelRatio());
    save(window.copy(0, 0, window.width(), std::min(strip, window.height())), QStringLiteral("687"),
         QStringLiteral("top-strip"));
    for (auto* area : fixture.window->findChildren<EditorArea*>())
        if (area->editorId() == "bloom.viewer")
            save(area->grab().toImage(), QStringLiteral("712"), QStringLiteral("svg-layer-selected"));
    // #716: with the Output node deleted the status bar says nothing can play or export.
    if (const auto output = fixture.session.composition()->graph().compositionOutput()) {
        commands::Transaction remove("Delete", fixture.session.snapshot().revision());
        remove.emplace<commands::RemoveNodes>(fixture.session.compositionId(),
                                              std::set<document::NodeId>{output->nodeId});
        (void)fixture.session.executeTransaction(std::move(remove));
        settle();
        if (auto* bar = fixture.window->findChild<WindowStatusBar*>())
            save(grabWidget(bar), QStringLiteral("716"), QStringLiteral("no-output-status-bar"));
    }
    // #688: the Shortcuts page.
    PreferencesWindow preferences(*fixture.window, defaultApplicationPreferences());
    preferences.show();
    settle();
    if (auto* rail = preferences.findChild<QListWidget*>(QStringLiteral("preferencesCategoryRail")))
        for (int row = 0; row < rail->count(); ++row)
            if (rail->item(row)->text() == QStringLiteral("Shortcuts")) {
                rail->setCurrentRow(row);
                settle();
                save(preferences.grab().toImage(), QStringLiteral("688"),
                     QStringLiteral("preferences-shortcuts"));
            }
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("BloomBugBatchCapture");
    app.setApplicationName("BugBatchCapture");
    QStandardPaths::setTestModeEnabled(true);
    QSettings().clear();
    kit::installAfterlabTheme(app);
    gPhase = qEnvironmentVariable("BLOOM_BUG_CAPTURE_PHASE", QStringLiteral("after"));
    gDirectory = argc > 1 ? QString::fromLocal8Bit(argv[1])
                          : qEnvironmentVariable("BLOOM_BUG_CAPTURE_DIR", QStringLiteral("."));
    QDir().mkpath(gDirectory);
    const QStringList only = argc > 2 ? QString::fromLocal8Bit(argv[2]).split(QLatin1Char(','))
                                      : QStringList{};
    const auto wanted = [&](const char* name) {
        return only.isEmpty() || only.contains(QLatin1String(name));
    };
    if (wanted("menu"))
        captureAddMenu();
    if (wanted("marquee"))
        captureMarquee();
    if (wanted("rightclick"))
        captureRightClick();
    if (wanted("probe"))
        probeRemoval();
    if (wanted("toolbar"))
        captureContextBar();
    if (wanted("shadow"))
        captureDropdownShadow();
    if (wanted("export"))
        captureExportPanel();
    if (wanted("pinch"))
        capturePinch();
    if (wanted("svg"))
        probeSvgBounds();
    if (wanted("svgui"))
        probeSvgAlignmentUi();
    if (wanted("flicker"))
        probeFlicker();
    if (wanted("alignbar"))
        captureAlignToolbar();
    if (wanted("drivermenu"))
        captureDriverMenu();
    if (wanted("window"))
        captureWindowSurfaces();
    if (wanted("lane"))
        captureLaneWindow();
    return gFailures == 0 ? 0 : 1;
}
