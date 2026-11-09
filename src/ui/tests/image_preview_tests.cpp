// #490: the Image Preview node. Its card thumbnail and the node editor background are prepared on
// workers through the Viewer's preview routes at the size they are drawn, kept by identity,
// cancelled when what is drawn changes, and never change the composition. The background is
// session state with its own Fit / 100% / zoom / pan.

#include <QApplication>
#include <QElapsedTimer>
#include <QMenu>

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_editor_operations.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/document/color_grade_nodes.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/image_math_parameters.hpp>
#include <bloom/document/image_preview.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/document/render_reachability.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/curve_graph.hpp>
#include <bloom/ui/image_preview_controller.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/node_editor.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include "node_editor_items.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace bloom;
using namespace std::chrono_literals;
using PreviewDisplay = document::ImagePreviewDisplay;
using State = ui::ImagePreviewController::State;

namespace {
int failures = 0;
void expect(const bool value, const char* message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 20000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

struct Pipeline final {
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualifiedProcessorProvider;
    ui::PreviewPreparationFunction pipeline;
    Pipeline() : compiler(definitions) {
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            std::abort();
        definitions.freeze();
        pipeline = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                      qualifiedProcessorProvider);
    }
};

struct Fixture final {
    document::NewProject initial =
        document::makeNewProject("Previews", "Main", core::RationalTime::fromInteger(2),
                                 *document::CompositionFormat::create(320, 180));
    document::Document document{std::move(initial.project)};
    commands::CommandStack commands{document};
    ui::CompositionSession session{document, commands, initial.initialCompositionId};
    runtime::TaskScheduler scheduler{runtime::TaskSchedulerConfig{.cpuWorkerCount = 2,
                                                                  .blockingIoWorkerCount = 1,
                                                                  .cpuQueueCapacity = 32,
                                                                  .blockingIoQueueCapacity = 4,
                                                                  .terminalHistoryCapacity = 64,
                                                                  .diagnosticsPerTask = 8,
                                                                  .groupRegistryCapacity = 8}};
    ui::TaskUiBridge bridge{scheduler, nullptr, 1ms};
    Pipeline pipeline;
    ui::CompositionPreviewController preview{session, scheduler, bridge, pipeline.pipeline};
    ui::ImagePreviewController previews{session, preview, scheduler, bridge};
    ~Fixture() {
        previews.cancel();
        preview.beginShutdown();
        bridge.beginShutdown();
        scheduler.beginShutdown();
        for (int i = 0; i < 400 && !scheduler.isQuiescent(); ++i)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }

    [[nodiscard]] std::optional<document::NodeId> nodeOfType(const std::string_view type,
                                                             const std::size_t skip = 0) const {
        std::size_t seen = 0;
        for (const auto& node : session.composition()->graph().nodes())
            if (node.typeId == type && seen++ == skip)
                return node.id;
        return std::nullopt;
    }

    bool add(const std::string_view type, std::optional<document::OutputPortRef> from) {
        commands::Transaction transaction("Add Node", session.snapshot().revision());
        transaction.emplace<commands::AddEditorNode>(session.compositionId(), std::string(type),
                                                     document::Vec2d{400.0, 0.0}, std::nullopt,
                                                     std::move(from));
        return session.executeNodeTransaction(std::move(transaction)).changed();
    }

    bool setDisplay(const document::NodeId node, const PreviewDisplay display) {
        commands::Transaction transaction("Set Display", session.snapshot().revision());
        transaction.emplace<commands::SetNodeParameterValue>(
            session.compositionId(), node, std::string(document::kImagePreviewDisplayParameterRole),
            document::ParameterValue{static_cast<std::int64_t>(display)});
        return session.executeNodeTransaction(std::move(transaction)).changed();
    }

    // Asks until the drawn identity is ready; every call is what a paint does.
    ui::ImagePreviewController::Preview ready(const document::NodeId node, const QSize size) {
        ui::ImagePreviewController::Preview shown;
        waitUntil([&] {
            shown = previews.preview(node, size);
            return shown.state == State::Ready || shown.state == State::Failed;
        });
        return shown;
    }
};

QColor centre(const QImage& image) {
    return image.isNull() ? QColor{} : image.pixelColor(image.width() / 2, image.height() / 2);
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ui::kit::installKinetikTheme(app);
    Fixture fixture;
    auto& session = fixture.session;
    auto& previews = fixture.previews;
    expect(session.imagePreviews() == &previews, "the controller registers with the session");
    expect(session.addSolidLayer("Red", core::Color4d{1.0, 0.0, 0.0, 1.0}), "red solid added");
    const auto solid = fixture.nodeOfType(document::kSolidSourceNodeType);
    expect(solid.has_value(), "the solid source exists");
    if (!solid)
        return 1;
    const auto planBefore = document::RenderReachability::compute(*session.composition());

    // Added from the solid's output in one undoable step: the node, its display and its link.
    expect(fixture.add(document::kImagePreviewNodeType, document::OutputPortRef{*solid, "image"}),
           "an Image Preview is added fed by the solid");
    const auto node = fixture.nodeOfType(document::kImagePreviewNodeType);
    expect(node.has_value(), "the Image Preview node exists");
    if (!node)
        return 1;
    expect(document::imagePreviewSource(session.composition()->graph(), *node)->output ==
               document::OutputPortRef{*solid, "image"},
           "the preview reads the solid");
    expect(previews.display(*node) == PreviewDisplay::Rgba, "the display defaults to RGBA");
    const auto planAfter = document::RenderReachability::compute(*session.composition());
    expect(planAfter.nodes().size() == planBefore.nodes().size() && !planAfter.reaches(*node),
           "the composition output reaches exactly what it reached before");
    expect(session.undo() && !fixture.nodeOfType(document::kImagePreviewNodeType) &&
               session.redo() && fixture.nodeOfType(document::kImagePreviewNodeType) == node,
           "adding the Image Preview undoes and redoes as one step");

    // Unconnected: a placeholder, nothing submitted.
    expect(fixture.add(document::kImagePreviewNodeType, std::nullopt), "a bare preview is added");
    const auto bare = fixture.nodeOfType(document::kImagePreviewNodeType, 1);
    expect(bare && previews.preview(*bare, {64, 64}).state == State::Unconnected &&
               previews.submissionCount() == 0,
           "an unconnected preview shows a placeholder and evaluates nothing");

    // The first paint asks; the worker answers at the drawn size, never on this thread.
    const auto first = previews.preview(*node, {64, 64});
    expect(first.state == State::Pending && first.image.isNull(),
           "the first paint returns at once with a placeholder");
    expect(previews.submissionCount() == 1, "the first paint submits one request");
    auto shown = fixture.ready(*node, {64, 64});
    expect(shown.state == State::Ready && centre(shown.image).red() > 200 &&
               centre(shown.image).green() < 60,
           "the thumbnail shows the red solid");
    expect(shown.image.width() == 64 && shown.image.height() == 36,
           "the thumbnail is evaluated at the drawn size (a proxy of 320x180)");
    expect(previews.lastWorkerThread().has_value() &&
               *previews.lastWorkerThread() != std::this_thread::get_id(),
           "the thumbnail was made on a worker, not the interface thread");
    expect(shown.cpu, "a frame from the CPU route is labelled CPU");

    // Kept by identity: the same paint costs nothing; a new size asks once; the old size is
    // still kept.
    const auto submitted = previews.submissionCount();
    expect(previews.preview(*node, {64, 64}).state == State::Ready &&
               previews.submissionCount() == submitted,
           "repainting the same identity prepares nothing");
    shown = fixture.ready(*node, {160, 160});
    expect(shown.image.width() == 160 && previews.submissionCount() == submitted + 1,
           "a larger drawn size is evaluated once at that size");
    expect(previews.preview(*node, {64, 64}).state == State::Ready &&
               previews.submissionCount() == submitted + 1,
           "a size seen before is drawn from what is kept");
    expect(previews.preview(*node, {4000, 4000}).state == State::Pending, "a huge size asks");
    shown = fixture.ready(*node, {4000, 4000});
    expect(shown.image.width() == 320, "the preview never exceeds the composition's resolution");

    // A change while a request is in flight cancels it; only the newest identity is drawn.
    const auto cancelled = previews.cancellationCount();
    expect(previews.preview(*node, {96, 96}).state == State::Pending, "a new size asks");
    expect(session.setCurrentTime(core::RationalTime::fromInteger(1)), "the playhead moves");
    expect(previews.preview(*node, {96, 96}).state == State::Pending, "a new time asks");
    expect(previews.cancellationCount() == cancelled + 1,
           "the request for the previous time is cancelled");
    shown = fixture.ready(*node, {96, 96});
    expect(shown.state == State::Ready && centre(shown.image).red() > 200,
           "the newest identity is drawn");

    // The display modes: the Viewer's channel presentations and the normalized data view.
    expect(fixture.setDisplay(*node, PreviewDisplay::Alpha), "the display changes to Alpha");
    shown = fixture.ready(*node, {96, 96});
    expect(centre(shown.image).red() > 250 && centre(shown.image).green() > 250,
           "Alpha shows the solid's full coverage as white");
    expect(fixture.setDisplay(*node, PreviewDisplay::Green), "the display changes to Green");
    shown = fixture.ready(*node, {96, 96});
    expect(centre(shown.image).red() < 5 && centre(shown.image).green() < 5,
           "Green shows the red solid's green channel as black");
    expect(fixture.setDisplay(*node, PreviewDisplay::Red), "the display changes to Red");
    shown = fixture.ready(*node, {96, 96});
    expect(centre(shown.image).green() > 250, "Red shows the red channel as white grey");
    expect(fixture.setDisplay(*node, PreviewDisplay::Normalized),
           "the display changes to Normalized");
    shown = fixture.ready(*node, {96, 96});
    expect(shown.state == State::Ready && centre(shown.image).red() == 255 &&
               centre(shown.image).green() == 0 && shown.cpu,
           "Normalized stretches the scene values to black..white and is labelled CPU");
    expect(session.undo() && previews.display(*node) == PreviewDisplay::Red,
           "a display change is one undoable edit");

    // A Mask is shown as its image: a Luma Key fed by the solid, into a second preview.
    expect(fixture.add("bloom.luma-key", document::OutputPortRef{*solid, "image"}),
           "a Luma Key is added");
    const auto key = fixture.nodeOfType("bloom.luma-key");
    expect(key &&
               fixture.add(document::kImagePreviewNodeType, document::OutputPortRef{*key, "mask"}),
           "an Image Preview is added fed by a Mask");
    const auto maskPreview = fixture.nodeOfType(document::kImagePreviewNodeType, 2);
    expect(maskPreview &&
               document::imagePreviewSource(session.composition()->graph(), *maskPreview)->kind ==
                   document::SocketValueKind::Mask,
           "the preview reads the Mask");
    if (maskPreview) {
        shown = fixture.ready(*maskPreview, {64, 64});
        if (shown.state != State::Ready)
            std::cerr << "  mask preview: " << shown.message.toStdString() << '\n';
        expect(shown.state == State::Ready && !shown.image.isNull(), "a Mask is previewed");
    }

    // #347: Audio Waveform and Audio Spectrum are drawn at the size the preview is, which is always
    // below the plot's own size. Each is a canvas the plot follows at the render's scale.
    for (const auto type : {document::kAudioWaveformNodeType, document::kAudioSpectrumNodeType}) {
        const std::size_t previewsBefore = [&] {
            std::size_t count = 0;
            for (const auto& item : session.composition()->graph().nodes())
                if (item.typeId == document::kImagePreviewNodeType)
                    ++count;
            return count;
        }();
        expect(fixture.add(type, std::nullopt), "an audio plot node is added");
        const auto plot = fixture.nodeOfType(type);
        expect(plot.has_value(), "the audio plot node exists");
        if (!plot)
            continue;
        expect(fixture.add(document::kImagePreviewNodeType, document::OutputPortRef{*plot, "image"}),
               "an Image Preview is added fed by an audio plot");
        const auto plotPreview = fixture.nodeOfType(document::kImagePreviewNodeType, previewsBefore);
        expect(plotPreview.has_value(), "the audio plot's Image Preview exists");
        if (!plotPreview)
            continue;
        shown = fixture.ready(*plotPreview, {64, 64});
        if (shown.state != State::Ready)
            std::cerr << "  " << type << " preview: " << shown.message.toStdString() << '\n';
        expect(shown.state == State::Ready && !shown.image.isNull(),
               "an audio plot is previewed below its own size");
        // The plot's background covers the whole picture: the corner is not transparent black.
        expect(shown.state == State::Ready && shown.image.pixelColor(1, 1).alpha() == 255,
               "the audio plot's canvas fills the preview");
    }

    // The background: session state, one node at a time.
    expect(!previews.backgroundNode().has_value(), "no background by default");
    previews.setBackgroundNode(*node);
    expect(previews.backgroundNode() == node && previews.backgroundVisible(),
           "an Image Preview becomes the background");
    if (maskPreview) {
        previews.setBackgroundNode(*maskPreview);
        expect(previews.backgroundNode() == maskPreview, "choosing another moves the background");
    }
    previews.setBackgroundVisible(false);
    expect(!previews.backgroundVisible(), "the background hides");
    previews.setBackgroundVisible(true);
    previews.setBackgroundNode(*node);
    const auto revision = session.snapshot().revision();
    previews.setBackgroundNode(std::nullopt);
    previews.setBackgroundNode(*node);
    expect(session.snapshot().revision() == revision,
           "choosing the background is not a document edit");

    // The node editor: the card draws a thumbnail; its menu sets the background; the background
    // has its own view.
    ui::NodeGraphEditor editor(session);
    editor.resize(900, 600);
    editor.show();
    QApplication::processEvents();
    auto* card = dynamic_cast<ui::node_editor::NodeItem*>(editor.graphScene()->findNodeItem(*node));
    expect(card != nullptr && card->isImagePreview() && !card->previewRect().isEmpty() &&
               card->previewRect().height() < card->previewRect().width(),
           "an Image Preview card has a thumbnail at the composition's aspect");
    previews.setBackgroundNode(std::nullopt);
    session.selectNode(*node);
    QApplication::processEvents();
    auto* menu = editor.contextMenuForTest(true);
    auto* toggle = menu ? menu->findChild<QAction*>("nodeBackgroundToggleAction") : nullptr;
    if (toggle == nullptr)
        for (auto* action : menu ? menu->actions() : QList<QAction*>{})
            if (action->objectName() == "nodeBackgroundToggleAction")
                toggle = action;
    expect(toggle != nullptr && toggle->isCheckable() && !toggle->isChecked(),
           "the card menu offers Show as Node Editor Background");
    if (toggle != nullptr)
        toggle->trigger();
    expect(previews.backgroundNode() == node, "the card menu sets the background");
    auto* view = editor.graphView();
    QApplication::processEvents();
    const auto fitted = view->backgroundRect();
    expect(!fitted.isEmpty() && view->backgroundTransform().fitToWindow,
           "a new background starts fitted to the editor");
    view->backgroundActualSize();
    expect(!view->backgroundTransform().fitToWindow &&
               std::abs(view->backgroundRect().width() - 320.0) < 1.0,
           "100% shows one screen pixel per composition pixel");
    const auto graphZoom = view->zoomFactor();
    view->zoomBackgroundStep(1);
    expect(std::abs(view->backgroundTransform().zoom - ui::kZoomStepFactor) < 1e-9 &&
               view->zoomFactor() == graphZoom,
           "zooming the background leaves the graph's zoom alone");
    view->zoomToActualSize();
    expect(std::abs(view->backgroundTransform().zoom - ui::kZoomStepFactor) < 1e-9,
           "zooming the graph leaves the background's zoom alone");
    view->fitBackground();
    expect(view->backgroundRect() == fitted, "Fit returns the background to the editor's size");
    // A paint asks for the background at its drawn size (capped at the composition's), on a
    // worker.
    view->viewport()->grab();
    expect(waitUntil([&] {
               view->viewport()->grab();
               return previews.preview(*node, (fitted.size() * view->devicePixelRatioF()).toSize())
                          .state == State::Ready;
           }),
           "the background is prepared at its drawn size");
    previews.setBackgroundVisible(false);
    expect(view->backgroundRect().isEmpty(), "a hidden background is not drawn");
    previews.setBackgroundVisible(true);

    // Removing the background node clears the background.
    commands::Transaction remove("Remove", session.snapshot().revision());
    remove.emplace<commands::RemoveNodes>(session.compositionId(),
                                          std::set<document::NodeId>{*node});
    expect(session.executeNodeTransaction(std::move(remove)).changed(), "the preview is removed");
    expect(!previews.backgroundNode().has_value(), "a removed node is no longer the background");

    // #493: a Curves node's input histogram, requested only by a painted curve graph, prepared on
    // a worker from the input's downsampled frame; and (#498) its card draws the curve graph.
    expect(fixture.add(document::kCurvesNodeType, document::OutputPortRef{*solid, "image"}),
           "a Curves node is added fed by the solid");
    const auto curves = fixture.nodeOfType(document::kCurvesNodeType);
    if (curves) {
        const auto renderBefore = document::RenderReachability::compute(*session.composition());
        ui::CurvesCardWidget field(session, *curves, false);
        field.resize(field.sizeHint());
        QApplication::processEvents();
        const auto submittedBefore = previews.submissionCount();
        expect(previews.submissionCount() == submittedBefore,
               "an unshown curve graph asks for no histogram");
        previews.setHistogramsShown(false);
        field.show();
        static_cast<void>(field.canvas()->grab());
        QApplication::processEvents();
        expect(previews.submissionCount() == submittedBefore,
               "a graph with the histogram hidden asks for none");
        previews.setHistogramsShown(true);
        static_cast<void>(field.canvas()->grab());
        expect(previews.submissionCount() == submittedBefore + 1,
               "painting the graph asks for its input histogram once");
        ui::ImagePreviewController::Histogram histogram;
        expect(waitUntil([&] {
                   histogram = previews.histogram(*curves);
                   return histogram.state == State::Ready || histogram.state == State::Failed;
               }),
               "the histogram is prepared");
        std::size_t filled = 0;
        float fullest = 0.0F;
        for (const auto bin : histogram.bins) {
            filled += bin > 0.0F ? 1U : 0U;
            fullest = std::max(fullest, bin);
        }
        expect(histogram.state == State::Ready &&
                   histogram.bins.size() == ui::ImagePreviewController::kHistogramBins &&
                   fullest == 1.0F && filled == 1,
               "a solid's histogram is one full bin");
        expect(previews.lastWorkerThread().has_value() &&
                   *previews.lastWorkerThread() != std::this_thread::get_id(),
               "the histogram is binned on a worker");
        expect(document::RenderReachability::compute(*session.composition()).nodes().size() ==
                   renderBefore.nodes().size(),
               "the histogram never changes what the output renders");
        const auto kept = previews.submissionCount();
        static_cast<void>(field.canvas()->grab());
        expect(previews.histogram(*curves).state == State::Ready &&
                   previews.submissionCount() == kept,
               "repainting the same input asks for nothing");
        field.setCurve(document::GradeCurve::Red);
        expect(previews.histogramsShown(), "the histogram is shown by default");
        QApplication::processEvents();
        auto* curveCard =
            dynamic_cast<ui::node_editor::NodeItem*>(editor.graphScene()->findNodeItem(*curves));
        expect(curveCard != nullptr &&
                   qobject_cast<ui::CurvesCardWidget*>(curveCard->cardWidget()) != nullptr,
               "a Curves card draws its curve graph");
    }

    if (failures != 0)
        return 1;
    std::cout << "PASS: image preview\n";
    return 0;
}
