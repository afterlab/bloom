// #645 drilling into grouped and SVG artwork in the Viewer, on a real document and Viewer:
//  - the structure answers (document/drill_levels.hpp): groups, children topmost first, matte
//    sources never children, small-group shapes, refused chains;
//  - the pick (runtime::ViewerPicker) on the evaluated outlines of a keyed frame: inside the quad
//    but outside the outline misses, the topmost of overlapping shapes wins, a matte source is
//    never picked, a gradient small group hits on its outline, kept geometry is reused;
//  - double-click enters three nested groups of an imported SVG and a shape's points, Escape and a
//    double-click on empty space back out, Enter enters the selection's group;
//  - a drag of a point on a keyed Path node writes one whole-path key at the playhead, one undo;
//  - the pick runs on a worker, a result for a stale time is dropped and a newer click cancels the
//    older pick;
//  - the selection stays in step both ways with the Timeline, the node editor, Properties and MCP;
//  - MCP viewer.set level/points and the viewer state;
//  - a double-click on a one-layer SVG offers Break Apart and drills into the result; Cancel
//    changes nothing; one undo restores the one-layer SVG.

#include "await_qt.hpp"
#include <bloom/commands/layer_mask_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/drill_levels.hpp>
#include <bloom/document/project.hpp>
#include <bloom/document/value_nodes.hpp>
#include <bloom/document/vector_artwork_nodes.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/layer_bounds_query.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/runtime/viewer_pick.hpp>
#include <bloom/ui/asset_controller.hpp>
#include <bloom/ui/composition_authoring.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/breadcrumb.hpp>
#include <bloom/ui/kit/dialog.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/mcp_live_ui_binding.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/timeline_editor.hpp>
#include <bloom/ui/viewer_editor.hpp>
#include <bloom/ui/viewer_overlays.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QTest>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
// The ceiling for a broken run; a passing wait returns as soon as its condition holds.
constexpr std::chrono::milliseconds kWait{20000};

using namespace bloom;
using namespace std::chrono_literals;

int failures = 0;

void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << location.file_name() << ':' << location.line() << ": FAIL: " << message
                  << '\n';
    }
}

// 400x300, as the composition. Top level: the SVG's root group. Inside it: backdrop and outer;
// outer holds gradbox (a gradient fill: a small group of a matte outline and a gradient) and
// middle; middle holds clipped (with its clip path as a matte source) and inner; inner holds ring
// (an even-odd ring) and tri (on top of ring).
constexpr auto kArtwork =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300" viewBox="0 0 400 300">
<defs>
<linearGradient id="fade"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient>
<clipPath id="window"><rect x="300" y="200" width="40" height="40"/></clipPath>
</defs>
<rect id="backdrop" x="0" y="0" width="60" height="60" fill="#222222"/>
<g id="outer">
<rect id="gradbox" x="20" y="200" width="100" height="60" fill="url(#fade)"/>
<g id="middle">
<rect id="clipped" x="280" y="180" width="100" height="100" fill="#00ff00" clip-path="url(#window)"/>
<g id="inner">
<path id="ring" fill="#336699" fill-rule="evenodd" d="M100 40H220V160H100Z M130 70H180V120H130Z"/>
<path id="tri" fill="#cc3333" d="M180 100L260 100L260 180Z"/>
</g>
</g>
</g>
</svg>
)svg";

// A one-layer SVG to break apart: a box and a ring.
constexpr auto kOneLayer =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300" viewBox="0 0 400 300">
<rect id="box" x="40" y="40" width="120" height="100" fill="#cc3333"/>
<path id="hoop" fill="#336699" fill-rule="evenodd" d="M220 60H360V200H220Z M260 100H320V160H260Z"/>
</svg>
)svg";

std::shared_ptr<const commands::PreparedSvgSource> prepare(const char* text, const char* file) {
    const auto size = std::strlen(text);
    auto bytes = std::make_shared<std::vector<std::byte>>(size);
    std::memcpy(bytes->data(), text, size);
    auto read = commands::prepareSvgSource(std::move(bytes), file);
    if (!read.source)
        throw std::runtime_error("the SVG could not be prepared: " + read.diagnostic);
    return read.source;
}

struct Pipeline final {
    runtime::NodeDefinitionRegistry definitions;
    std::unique_ptr<runtime::SnapshotCompiler> compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualified;
    Pipeline() {
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            throw std::runtime_error("built-in definitions");
        definitions.freeze();
        compiler = std::make_unique<runtime::SnapshotCompiler>(definitions);
    }
};

const auto kOne = core::RationalTime::fromInteger(1);
const auto kZero = core::RationalTime::fromInteger(0);

struct App final {
    runtime::TaskScheduler scheduler;
    ui::ProjectHost host{scheduler};
    ui::CompositionSession session{*host.liveDocumentAndStack().first,
                                   *host.liveDocumentAndStack().second, host.lowestCompositionId()};
    ui::TaskUiBridge bridge{scheduler, nullptr, 1ms};
    ui::AssetController assets{session, host, scheduler, bridge};
    Pipeline pipeline;
    std::unique_ptr<ui::CompositionPreviewController> preview;
    std::unique_ptr<QWidget> window;
    ui::ViewerEditor* viewer = nullptr;

    App() {
        commands::Transaction seed("Seed", session.snapshot().revision());
        seed.emplace<commands::AddComposition>(
            "Drill",
            *document::CompositionFormat::create(400, 300, core::PixelAspectRatio::square(),
                                                 *document::FrameRate::create(24, 1)),
            core::RationalTime::fromInteger(10));
        const auto seeded = session.executeTransaction(std::move(seed));
        const auto id =
            seeded.succeeded()
                ? seeded.outputId<document::CompositionId>(commands::kAddCompositionOutput)
                : std::nullopt;
        if (!id || !session.setComposition(*id))
            throw std::runtime_error("seed the composition");
        session.setAssetController(&assets);
        preview = std::make_unique<ui::CompositionPreviewController>(
            session, scheduler, bridge,
            ui::makeCompositionPreviewPipeline(*pipeline.compiler, pipeline.evaluator,
                                               pipeline.displayPreparer, pipeline.qualified));
        window = std::make_unique<QWidget>();
        viewer = new ui::ViewerEditor(session, *preview, nullptr, window.get());
        viewer->setGeometry(0, 0, 900, 640);
        window->resize(900, 640);
        window->show();
        QApplication::processEvents();
    }
    ~App() {
        window.reset();
        session.setAssetController(nullptr);
        if (!bloom::test_support::awaitQtCondition(
                "the asset controller settles at teardown", [this] { return !assets.busy(); },
                kWait))
            std::cerr << "FAIL: the asset controller did not settle at teardown\n";
        preview->beginShutdown();
    }
    [[nodiscard]] const document::Composition& composition() const {
        return *session.composition();
    }

    document::LayerId import(const char* text, const commands::SvgImportMode mode,
                             const std::string& name) {
        commands::Transaction transaction("Import " + name, session.snapshot().revision());
        transaction.emplace<commands::AddSvgArtwork>(session.compositionId(),
                                                     prepare(text, "drill.svg"), name, mode, false);
        const auto result = session.executeTransaction(std::move(transaction));
        const auto layer = result.outputId<document::LayerId>("layer");
        if (!result.succeeded() || !layer)
            throw std::runtime_error("the SVG import failed");
        return *layer;
    }
    // The entry layer of `parent` (the top level when invalid) named `name`.
    [[nodiscard]] document::LayerId child(const document::LayerId parent,
                                          const std::string_view name) const {
        const auto& current = composition();
        const auto merge = parent.isValid() ? document::layerContentMerge(current, parent)
                                            : current.graph().outputMergeId();
        std::string present;
        if (merge)
            for (const auto layer : document::mergeEntryLayers(current, *merge))
                if (const auto* boundary = current.graph().findLayer(layer); boundary != nullptr) {
                    if (boundary->name == name)
                        return layer;
                    present += " '" + boundary->name + "'";
                }
        throw std::runtime_error("no layer named " + std::string(name) + "; present:" + present);
    }
    [[nodiscard]] document::MergeEntryRef entry(const document::LayerId layer) const {
        const auto* boundary = composition().graph().findLayer(layer);
        if (boundary == nullptr || !boundary->entrySlot)
            throw std::runtime_error("not an entry layer");
        return {boundary->nodeId, *boundary->entrySlot};
    }
    [[nodiscard]] std::optional<document::LayerId> primary() const {
        if (const auto* layer = std::get_if<document::LayerId>(&session.selection().primary))
            return *layer;
        return std::nullopt;
    }
    // The Viewer shows a frame of the current document and time.
    bool settle() {
        const bool ok = bloom::test_support::awaitQtCondition(
            "the Viewer shows a painted frame",
            [this] {
                const auto state = viewer->displayedPreviewState();
                // mappingForTest() holds only for a painted frame of the current document and time.
                return state.frame != nullptr &&
                       state.frame->desiredIdentity().time == session.currentTime() &&
                       viewer->mappingForTest().has_value();
            },
            kWait);
        QApplication::processEvents();
        return ok;
    }
    // The screen point of a composition point, once the Viewer has painted what it shows.
    [[nodiscard]] QPointF at(const document::Vec2d point) {
        if (!settle())
            throw std::runtime_error("the Viewer shows no frame to point at");
        return viewer->mappingForTest()->toScreen(point);
    }
    void mouse(const QEvent::Type type, const QPointF point, const Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, viewer->mapToGlobal(point), Qt::LeftButton, buttons,
                          Qt::NoModifier);
        QApplication::sendEvent(viewer, &event);
    }
    void waitPick() {
        expect(bloom::test_support::awaitQtCondition(
                   "the drill pick finishes", [this] { return !viewer->drillPickPendingForTest(); },
                   kWait),
               "the drill pick finishes");
        QApplication::processEvents();
    }
    // A platform double-click: press, release, double-click, release. Between the two the event
    // loop runs, as it does between an artist's two clicks, so the first click's selection is
    // painted.
    void doubleClick(const document::Vec2d point) {
        const auto screen = at(point);
        mouse(QEvent::MouseButtonPress, screen, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, screen, Qt::NoButton);
        QTest::qWait(30);
        settle();
        mouse(QEvent::MouseButtonDblClick, screen, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, screen, Qt::NoButton);
        waitPick();
    }
    void click(const document::Vec2d point) {
        const auto screen = at(point);
        mouse(QEvent::MouseButtonPress, screen, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, screen, Qt::NoButton);
        waitPick();
    }
    void key(const Qt::Key key) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QApplication::sendEvent(viewer, &press);
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
        QApplication::sendEvent(viewer, &release);
        waitPick();
    }
    [[nodiscard]] std::vector<document::LayerId> level() const {
        return session.viewerLevel().groups;
    }
};

// The Path node that draws `shape` and its value parameter.
document::ParameterId pathParameter(const document::Composition& composition,
                                    const document::LayerId shape) {
    const auto info = document::drillShape(composition, shape);
    const auto* node =
        info && info->pathNode ? composition.graph().findNode(*info->pathNode) : nullptr;
    if (node == nullptr)
        throw std::runtime_error("the shape has no Path node");
    const auto found = std::ranges::find(node->parameters, document::kValueParameterRole,
                                         &document::ParameterBinding::role);
    if (found == node->parameters.end())
        throw std::runtime_error("the Path node has no value");
    return found->parameterId;
}

std::size_t pathKeys(const document::Composition& composition, const document::ParameterId id) {
    const auto* parameter = composition.parameters().find(id);
    const auto* curve =
        parameter ? std::get_if<document::AnimationCurveSource>(&parameter->source) : nullptr;
    const auto* path = curve ? composition.animationCurves().findPath(curve->curveId) : nullptr;
    return path ? path->keyframes.size() : 0;
}

document::PathValue shifted(document::PathValue value, const document::Vec2d by) {
    for (auto& contour : value.contours)
        for (auto& anchor : contour.anchors) {
            anchor.point = {anchor.point.x + by.x, anchor.point.y + by.y};
            if (anchor.inHandle)
                anchor.inHandle =
                    document::Vec2d{anchor.inHandle->x + by.x, anchor.inHandle->y + by.y};
            if (anchor.outHandle)
                anchor.outHandle =
                    document::Vec2d{anchor.outHandle->x + by.x, anchor.outHandle->y + by.y};
        }
    return value;
}

struct Scene final {
    document::LayerId root, backdrop, outer, gradbox, middle, clipped, clip, inner, ring, tri;
    document::ParameterId triPath;
};

// Imports the artwork as editable shapes and keys tri's Path node: at 0 s where the file draws it,
// at 1 s 40 px lower.
Scene buildScene(App& app) {
    Scene scene;
    scene.root = app.import(kArtwork, commands::SvgImportMode::Shapes, "Art");
    scene.backdrop = app.child(scene.root, "backdrop");
    scene.outer = app.child(scene.root, "outer");
    scene.gradbox = app.child(scene.outer, "gradbox");
    scene.middle = app.child(scene.outer, "middle");
    scene.clipped = app.child(scene.middle, "clipped");
    scene.clip = app.child(scene.middle, "Clip window");
    scene.inner = app.child(scene.middle, "inner");
    scene.ring = app.child(scene.inner, "ring");
    scene.tri = app.child(scene.inner, "tri");
    scene.triPath = pathParameter(app.composition(), scene.tri);
    const auto* parameter = app.composition().parameters().find(scene.triPath);
    const auto* constant =
        parameter ? std::get_if<document::ConstantValueSource>(&parameter->source) : nullptr;
    const auto* base = constant ? std::get_if<document::PathValue>(&constant->value) : nullptr;
    if (base == nullptr)
        throw std::runtime_error("tri's path is a constant at first");
    commands::Transaction keys("Key tri", app.session.snapshot().revision());
    keys.emplace<commands::SetKeyframeAtTimeForParameter>(app.session.compositionId(),
                                                          scene.triPath, kZero, *base);
    keys.emplace<commands::SetKeyframeAtTimeForParameter>(
        app.session.compositionId(), scene.triPath, kOne, shifted(*base, {0.0, 40.0}));
    if (!app.session.executeTransaction(std::move(keys)).succeeded())
        throw std::runtime_error("key tri's path");
    return scene;
}

void testStructure(App& app, const Scene& s) {
    const auto& c = app.composition();
    expect(document::isDrillGroup(c, s.root) && document::isDrillGroup(c, s.outer) &&
               document::isDrillGroup(c, s.middle) && document::isDrillGroup(c, s.inner),
           "the root group and the three nested groups are groups");
    expect(!document::isDrillGroup(c, s.gradbox) && document::drillShape(c, s.gradbox) &&
               document::drillShape(c, s.gradbox)->smallGroup,
           "the gradient fill is a small-group shape, not a group to enter");
    expect(document::drillShape(c, s.ring) && !document::drillShape(c, s.ring)->smallGroup &&
               document::drillShape(c, s.ring)->edited == s.ring,
           "ring is a shape edited through itself");
    const auto innerMerge = document::layerContentMerge(c, s.inner);
    const auto innerChildren =
        innerMerge ? document::drillChildren(c, *innerMerge) : std::vector<document::LayerId>{};
    expect(innerChildren == std::vector{s.tri, s.ring}, "inner's children, topmost first");
    const auto middleMerge = document::layerContentMerge(c, s.middle);
    const auto middleChildren =
        middleMerge ? document::drillChildren(c, *middleMerge) : std::vector<document::LayerId>{};
    expect(std::ranges::find(middleChildren, s.clip) == middleChildren.end() &&
               std::ranges::find(middleChildren, s.clipped) != middleChildren.end() &&
               middleMerge && document::isTrackMatteSource(c, *middleMerge, s.clip),
           "a clip path's matte source is never a child");
    expect(document::drillAncestors(c, s.tri) ==
               std::optional(std::vector{s.root, s.outer, s.middle, s.inner}),
           "tri's enclosing groups, outermost first");
    expect(app.session.enclosingGroups(s.ring) == std::vector{s.root, s.outer, s.middle, s.inner} &&
               app.session.enclosingGroups(s.root).empty(),
           "the session answers the same enclosing groups");
    // Refused chains: a group that is not a child of the level before it, a matte source, a shape,
    // an unknown id and points that are not a shape child of the last group.
    using R = document::DrillLevelRefusal;
    const std::vector<document::LayerId> skips{s.outer};
    expect(document::checkDrillLevel(c, skips, std::nullopt).refusal == R::NotAChain,
           "a chain must start at the top level");
    const std::vector<document::LayerId> matte{s.root, s.outer, s.middle, s.clip};
    expect(!document::checkDrillLevel(c, matte, std::nullopt).ok(),
           "a matte source is never a level");
    const std::vector<document::LayerId> shape{s.root, s.outer, s.gradbox};
    expect(document::checkDrillLevel(c, shape, std::nullopt).refusal == R::NotAGroup,
           "a small-group shape is not a group to enter");
    const std::vector<document::LayerId> unknown{document::LayerId::fromRaw(999999)};
    expect(document::checkDrillLevel(c, unknown, std::nullopt).refusal == R::UnknownLayer,
           "an unknown id is refused as unknown");
    const std::vector<document::LayerId> chain{s.root, s.outer, s.middle, s.inner};
    expect(document::checkDrillLevel(c, chain, s.tri).ok() &&
               document::checkDrillLevel(c, chain, s.middle).refusal == R::NotAShape,
           "points must be a shape child of the last group");
    expect(!document::drillLevelRefusalText(c, document::checkDrillLevel(c, skips, std::nullopt))
                .empty(),
           "a refusal says why in words");
}

// The pick on the evaluated outlines at the keyed frame, called directly.
void testPicker(App& app, const Scene& s) {
    std::atomic<std::uint64_t> compiles = 0;
    runtime::ViewerPicker picker([&](const runtime::SnapshotCompileRequest& request,
                                     const runtime::CancellationToken& cancellation) {
        ++compiles;
        return app.pipeline.compiler->compile(request, cancellation);
    });
    const auto snapshot = app.session.snapshot();
    const auto boundsAt = [&](const core::RationalTime time) {
        const auto result =
            runtime::queryLayerBounds(*app.pipeline.compiler, app.pipeline.evaluator,
                                      {.snapshot = snapshot,
                                       .composition = app.session.compositionId(),
                                       .time = time,
                                       .layers = {}});
        expect(result.succeeded(), "the bounds query evaluates: " + result.diagnostic);
        return result.bounds;
    };
    const auto boundsOne = boundsAt(kOne);
    const auto boundsZero = boundsAt(kZero);
    const auto pick = [&](const std::vector<document::LayerId>& level, const document::Vec2d point,
                          const core::RationalTime time = kOne) {
        runtime::ViewerPickRequest request{.snapshot = snapshot,
                                           .composition = app.session.compositionId(),
                                           .time = time,
                                           .level = level,
                                           .point = point,
                                           .tolerance = 0.0,
                                           .bounds = time == kOne ? boundsOne : boundsZero,
                                           .plan = nullptr};
        const auto result = picker.pick(request, {});
        expect(result.status == runtime::ViewerPickStatus::Picked,
               "the pick answers: " + result.diagnostic);
        return result;
    };
    const std::vector<document::LayerId> inner{s.root, s.outer, s.middle, s.inner};
    const std::vector<document::LayerId> middle{s.root, s.outer, s.middle};
    const std::vector<document::LayerId> outer{s.root, s.outer};
    // Overlapping ring and tri at 1 s: the topmost, tri, wins.
    expect(pick(inner, {205.0, 150.0}).child == s.tri, "the topmost of overlapping shapes wins");
    expect(pick(inner, {110.0, 50.0}).child == s.ring, "ring hits on its fill");
    // Inside tri's bounds quad, outside the triangle.
    const auto quadOnly = pick(inner, {190.0, 210.0});
    expect(!quadOnly.child, "inside the quad but outside the outline misses");
    expect(!pick(inner, {155.0, 95.0}).child, "the ring's even-odd hole misses");
    // The keyed frame: tri is 40 px lower at 1 s than at 0 s.
    expect(pick(inner, {250.0, 190.0}).child == s.tri, "tri hits where it is at 1 s");
    expect(!pick(inner, {250.0, 190.0}, kZero).child, "and not there at 0 s");
    expect(pick(inner, {250.0, 120.0}, kZero).child == s.tri, "tri hits where it is at 0 s");
    // The clip: only where the matte also covers; the matte source itself is never picked.
    expect(pick(middle, {320.0, 220.0}).child == s.clipped,
           "the clipped rectangle hits in its clip");
    expect(!pick(middle, {290.0, 190.0}).child, "outside its clip it misses");
    // The gradient small group hits on its outline only.
    expect(pick(outer, {70.0, 230.0}).child == s.gradbox,
           "the gradient fill hits inside its outline");
    expect(!pick(outer, {130.0, 230.0}).child, "and misses just outside it");
    // From further out, the child holding the leaf.
    const auto deep = pick({s.root}, {205.0, 150.0});
    expect(deep.child == s.outer && deep.leaf == s.tri, "a group hits through its descendants");
    expect(pick({}, {205.0, 150.0}).child == s.root, "the top level picks the SVG's root");
    const auto stats = picker.statistics();
    // One frame is kept: 1 s, then 0 s, then 1 s again are three evaluations; every other pick
    // reuses the frame's geometry.
    expect(stats.reused == stats.picks - 3 && stats.evaluations == 3,
           "further picks on the same frame reuse its geometry (" +
               std::to_string(stats.evaluations) + " evaluations, " + std::to_string(stats.reused) +
               " reused)");
    // A pick given the frame's own plan compiles nothing.
    const auto compiled = app.pipeline.compiler->compile(
        {.snapshot = snapshot, .compositionId = app.session.compositionId()}, {});
    if (compiled.plan) {
        runtime::ViewerPicker planned([&](const runtime::SnapshotCompileRequest& request,
                                          const runtime::CancellationToken& cancellation) {
            ++compiles;
            return app.pipeline.compiler->compile(request, cancellation);
        });
        const auto before = compiles.load();
        const auto result = planned.pick({.snapshot = snapshot,
                                          .composition = app.session.compositionId(),
                                          .time = kOne,
                                          .level = inner,
                                          .point = {205.0, 150.0},
                                          .tolerance = 0.0,
                                          .bounds = boundsOne,
                                          .plan = compiled.plan},
                                         {});
        expect(result.child == s.tri && compiles.load() == before &&
                   planned.statistics().compiles == 0,
               "the displayed frame's plan is used without a compile");
        const auto gradient = planned.pick({.snapshot = snapshot,
                                            .composition = app.session.compositionId(),
                                            .time = kOne,
                                            .level = outer,
                                            .point = {70.0, 230.0},
                                            .tolerance = 5.0,
                                            .bounds = boundsOne,
                                            .plan = compiled.plan},
                                           {});
        expect(gradient.child == s.gradbox && compiles.load() == before,
               "the gradient fill hits on the plan's outline too: " + gradient.diagnostic);
    }
    // A level that no longer resolves is refused, not guessed.
    const auto refused = picker.pick({.snapshot = snapshot,
                                      .composition = app.session.compositionId(),
                                      .time = kOne,
                                      .level = {s.outer},
                                      .point = {205.0, 150.0},
                                      .tolerance = 0.0,
                                      .bounds = boundsOne,
                                      .plan = nullptr},
                                     {});
    expect(refused.status == runtime::ViewerPickStatus::Unsupported && !refused.child,
           "a chain that does not resolve is refused");
}

void testDrillGestures(App& app, const Scene& s) {
    expect(app.session.setCurrentTime(kOne), "go to the keyed frame at 1 s");
    expect(app.settle(), "the Viewer shows 1 s");
    const auto stack = app.session.undoLabel();
    const auto revision = app.session.snapshot().revision();
    // Three nested levels, then the shape's points.
    const document::Vec2d onRing{110.0, 50.0};
    app.doubleClick(onRing);
    expect(app.level() == std::vector{s.root} && app.primary() == s.outer,
           "double-click enters the SVG's root and selects the child under the pointer");
    app.doubleClick(onRing);
    expect(app.level() == std::vector{s.root, s.outer} && app.primary() == s.middle,
           "double-click enters outer");
    app.doubleClick(onRing);
    expect(app.level() == std::vector{s.root, s.outer, s.middle} && app.primary() == s.inner,
           "double-click enters middle");
    app.doubleClick(onRing);
    expect(app.level() == std::vector{s.root, s.outer, s.middle, s.inner} &&
               app.primary() == s.ring,
           "double-click enters inner and selects ring");
    // Viewer -> Timeline, node editor and Properties: one session selection all of them read.
    expect(app.session.isEntrySelected(app.entry(s.ring)) &&
               app.session.selection().contextualLayer == s.ring &&
               app.session.parameterForSelection("path") != nullptr,
           "the Timeline row, the node editor and Properties follow the Viewer's pick");
    // A click picks the topmost child under the pointer inside the group.
    app.click({205.0, 150.0});
    expect(app.primary() == s.tri && app.level().size() == 4,
           "a click inside the group picks the topmost child and keeps the level");
    app.click({190.0, 210.0});
    expect(!app.primary() && app.level().size() == 4,
           "a click inside a quad but off the outline selects nothing and stays in the group");
    app.doubleClick(onRing);
    expect(app.session.viewerLevel().points == s.ring && app.primary() == s.ring,
           "double-click on a shape edits its points");
    expect(app.viewer->anchorEditTargetForTest().has_value(), "the ring's points are shown");
    // Escape: points, then one group at a time.
    app.key(Qt::Key_Escape);
    expect(!app.session.viewerLevel().points && app.level().size() == 4 && app.primary() == s.ring,
           "Escape leaves point editing with the shape selected");
    app.key(Qt::Key_Escape);
    expect(app.level() == std::vector{s.root, s.outer, s.middle} && app.primary() == s.inner,
           "Escape backs out of inner with it selected");
    // Double-click on empty space backs out one level too.
    app.doubleClick({300.0, 60.0});
    expect(app.level() == std::vector{s.root, s.outer} && app.primary() == s.middle,
           "double-click on empty space backs out one level");
    app.key(Qt::Key_Escape);
    app.key(Qt::Key_Escape);
    expect(app.level().empty() && app.primary() == s.root, "Escape returns to the top level");
    // Enter on a group selected in the Timeline enters it at its own level.
    app.session.selectEntry(app.entry(s.middle));
    expect(app.level() == std::vector{s.root, s.outer},
           "selecting a nested layer in the Timeline sets the level to its groups");
    app.viewer->setFocus();
    app.key(Qt::Key_Return);
    expect(app.level() == std::vector{s.root, s.outer, s.middle} && app.primary() == s.inner,
           "Enter enters the selected group and selects its topmost child");
    // Enter on a shape edits its points.
    app.session.selectEntry(app.entry(s.ring));
    app.key(Qt::Key_Return);
    expect(app.session.viewerLevel().points == s.ring &&
               app.level() == std::vector{s.root, s.outer, s.middle, s.inner},
           "Enter on a shape edits its points");
    app.key(Qt::Key_Escape);
    app.key(Qt::Key_Escape);
    // A double-click on the gradient fill edits its outline's points through the matte shape.
    app.session.selectEntry(app.entry(s.middle));
    expect(app.level() == std::vector{s.root, s.outer}, "back at outer");
    app.doubleClick({70.0, 230.0});
    const auto edited = document::drillShape(app.composition(), s.gradbox);
    expect(app.session.viewerLevel().points == s.gradbox && edited &&
               app.primary() == edited->edited,
           "double-click on the gradient fill edits the outline it is filled through");
    // The level is session state: no command, no revision.
    expect(app.session.snapshot().revision() == revision && app.session.undoLabel() == stack,
           "drilling writes nothing to the document or the undo history");
    app.key(Qt::Key_Escape);
}

void testSelectionFollowsLevel(App& app, const Scene& s) {
    // Node editor -> level: selecting tri's Path node enters tri's groups.
    const auto info = document::drillShape(app.composition(), s.tri);
    app.session.selectNode(*info->pathNode);
    expect(app.level() == std::vector{s.root, s.outer, s.middle, s.inner},
           "selecting a Path node in the node editor sets the level to its shape's groups");
    // Timeline -> level.
    app.session.selectEntry(app.entry(s.clipped));
    expect(app.level() == std::vector{s.root, s.outer, s.middle},
           "selecting a row in the Timeline sets the level");
    // A part of a small-group shape: its group's level, never the shape itself.
    const auto gradientMerge = document::layerContentMerge(app.composition(), s.gradbox);
    const auto parts = gradientMerge ? document::mergeEntryLayers(app.composition(), *gradientMerge)
                                     : std::vector<document::LayerId>{};
    expect(!parts.empty(), "the gradient fill has parts");
    if (!parts.empty()) {
        app.session.selectEntry(app.entry(parts.front()));
        expect(app.level() == std::vector{s.root, s.outer},
               "a part of a small-group shape sets its group's level");
    }
    // Clearing the selection keeps the groups.
    app.session.clearSelection();
    expect(app.level() == std::vector{s.root, s.outer}, "a cleared selection keeps the level");
    // A top-level selection returns to the top.
    app.session.selectEntry(app.entry(s.root));
    expect(app.level().empty(), "a top-level selection returns to the top level");

    // MCP: selection.set and viewer.set, and the viewer state.
    ui::McpLiveUiBinding binding(app.host, app.session, app.preview->playbackController());
    expect(binding.setSelection({.composition = app.session.compositionId(),
                                 .layers = {s.tri},
                                 .nodes = {},
                                 .mode = mcp::McpSelectionMode::Set})
                   .ok() &&
               app.level() == std::vector{s.root, s.outer, s.middle, s.inner},
           "MCP selection.set sets the level to the layer's groups");
    mcp::McpViewerRequest request;
    request.level = std::vector{s.root, s.outer};
    expect(binding.setViewer(request).ok() && app.level() == std::vector{s.root, s.outer},
           "MCP viewer.set level enters a chain without a Viewer window");
    auto state = binding.viewerState();
    expect(state.level == std::vector{s.root, s.outer} && !state.points,
           "the viewer state reports the level");
    request = {};
    request.level = std::vector{s.root, s.outer, s.middle, s.inner};
    request.points = std::optional(s.ring);
    expect(binding.setViewer(request).ok() && app.session.viewerLevel().points == s.ring &&
               app.primary() == s.ring,
           "MCP viewer.set points edits a shape's points and selects it");
    state = binding.viewerState();
    expect(state.points == s.ring && state.level.size() == 4,
           "the viewer state reports the points");
    const auto before = app.session.viewerLevel();
    request = {};
    request.level = std::vector{s.outer};
    const auto skip = binding.setViewer(request);
    expect(skip.refusal == mcp::McpUiRefusal::InvalidArgument && !skip.message.empty() &&
               app.session.viewerLevel() == before,
           "a chain that does not start at the top is refused and changes nothing");
    request.level = std::vector{s.root, s.outer, s.middle, s.clip};
    expect(!binding.setViewer(request).ok() && app.session.viewerLevel() == before,
           "a matte source is refused as a level");
    request.level = std::vector{document::LayerId::fromRaw(999999)};
    expect(binding.setViewer(request).refusal == mcp::McpUiRefusal::UnknownTarget,
           "an unknown layer is refused as unknown");
    request = {};
    request.level = std::vector{s.root, s.outer};
    request.points = std::optional(s.ring);
    expect(binding.setViewer(request).refusal == mcp::McpUiRefusal::InvalidArgument &&
               app.session.viewerLevel() == before,
           "points that are not a child of the last group are refused");
    request = {};
    request.level = std::vector<document::LayerId>{};
    expect(binding.setViewer(request).ok() && app.level().empty() &&
               !app.session.viewerLevel().points,
           "an empty level returns to the top");
}

void testKeyedPointDrag(App& app, const Scene& s) {
    // Between the keys, 0.5 s.
    const auto half = *core::RationalTime::create(1, 2);
    expect(app.session.setCurrentTime(half), "go to 0.5 s");
    mcp::McpViewerRequest request;
    ui::McpLiveUiBinding binding(app.host, app.session, app.preview->playbackController());
    request.level = std::vector{s.root, s.outer, s.middle, s.inner};
    request.points = std::optional(s.tri);
    expect(binding.setViewer(request).ok() && app.primary() == s.tri, "edit tri's points");
    expect(app.settle(), "the Viewer shows 0.5 s");
    expect(pathKeys(app.composition(), s.triPath) == 2, "tri's path has two keys");
    std::optional<std::pair<document::PathContour, QTransform>> target;
    expect(bloom::test_support::awaitQtCondition(
               "tri's points are on the Viewer",
               [&] {
                   target = app.viewer->anchorEditTargetForTest();
                   return target.has_value();
               },
               kWait),
           "tri's points are on the Viewer");
    if (!target || target->first.anchors.empty())
        return;
    const auto history = app.session.undoLabel();
    const auto anchor = target->first.anchors.front().point;
    const auto world = target->second.map(QPointF(anchor.x, anchor.y));
    const auto from = app.at({world.x(), world.y()});
    const auto to = from + QPointF(18.0, 12.0);
    app.mouse(QEvent::MouseButtonPress, from, Qt::LeftButton);
    for (int step = 1; step <= 4; ++step) {
        const auto point = from + (to - from) * (step / 4.0);
        QMouseEvent move(QEvent::MouseMove, point, app.viewer->mapToGlobal(point), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(app.viewer, &move);
    }
    const auto revision = app.session.snapshot().revision();
    app.mouse(QEvent::MouseButtonRelease, to, Qt::NoButton);
    QApplication::processEvents();
    expect(app.session.snapshot().revision().value() == revision.value() + 1,
           "the drag is one command");
    expect(pathKeys(app.composition(), s.triPath) == 3,
           "the drag writes one whole-path key at the playhead and keeps the others");
    const auto value = app.session.liveValue(s.triPath);
    const auto* path = value ? std::get_if<document::PathValue>(&*value) : nullptr;
    expect(path != nullptr && !path->contours.empty() &&
               path->contours.front().anchors.front().point != anchor,
           "the key at the playhead holds the dragged point");
    expect(app.session.viewerLevel().points == s.tri, "point editing stays after the drag");
    expect(app.session.undo() && pathKeys(app.composition(), s.triPath) == 2,
           "one undo removes the key the drag wrote");
    expect(app.session.undoLabel() == history, "and nothing else");
    app.key(Qt::Key_Escape);
    request = {};
    request.level = std::vector<document::LayerId>{};
    (void)binding.setViewer(request);
}

void testPickThreadStaleAndCancel(App& app, const Scene& s) {
    // An observing picker: it notes the thread every pick runs on and can hold the pick.
    std::mutex mutex;
    std::vector<std::thread::id> threads;
    std::atomic<bool> hold = false;
    std::atomic<int> held = 0;
    auto picker = std::make_shared<runtime::ViewerPicker>(
        [&app](const runtime::SnapshotCompileRequest& request,
               const runtime::CancellationToken& cancellation) {
            return app.pipeline.compiler->compile(request, cancellation);
        });
    picker->setPickObserverForTesting([&] {
        {
            const std::lock_guard lock(mutex);
            threads.push_back(std::this_thread::get_id());
        }
        ++held;
        while (hold.load())
            std::this_thread::sleep_for(1ms);
        --held;
    });
    app.preview->setViewerPicker(picker);
    const auto uiThread = std::this_thread::get_id();

    expect(app.session.setCurrentTime(kOne), "go to 1 s");
    ui::McpLiveUiBinding binding(app.host, app.session, app.preview->playbackController());
    mcp::McpViewerRequest request;
    request.level = std::vector{s.root, s.outer, s.middle, s.inner};
    expect(binding.setViewer(request).ok(), "enter inner");
    app.session.clearSelection();
    expect(app.settle(), "the Viewer shows 1 s");

    // A plain click: picked on a worker.
    app.click({205.0, 150.0});
    {
        const std::lock_guard lock(mutex);
        expect(!threads.empty() &&
                   std::ranges::none_of(threads, [&](const auto id) { return id == uiThread; }),
               "the pick runs on a worker, never on the UI thread");
    }
    expect(app.primary() == s.tri, "the worker's answer selects tri");

    // Stale: the playhead moves while the pick is out; its answer is dropped.
    app.session.clearSelection();
    hold = true;
    const auto screen = app.at({110.0, 50.0});
    app.mouse(QEvent::MouseButtonPress, screen, Qt::LeftButton);
    app.mouse(QEvent::MouseButtonRelease, screen, Qt::NoButton);
    expect(app.viewer->drillPickPendingForTest(), "the click's pick is out");
    expect(bloom::test_support::awaitQtCondition(
               "the pick is held on its worker", [&] { return held.load() == 1; }, kWait),
           "the pick is held on its worker");
    expect(app.session.setCurrentTime(kZero), "the playhead moves to 0 s");
    hold = false;
    app.waitPick();
    QTest::qWait(50);
    expect(!app.primary(), "an answer for a frame no longer shown is dropped");

    // Cancel: a newer click cancels the older pick; only the newer answer applies.
    expect(app.session.setCurrentTime(kOne), "back to 1 s");
    expect(app.settle(), "the Viewer shows 1 s again");
    hold = true;
    const auto first = app.at({110.0, 50.0});
    const auto second = app.at({250.0, 190.0});
    app.mouse(QEvent::MouseButtonPress, first, Qt::LeftButton);
    app.mouse(QEvent::MouseButtonRelease, first, Qt::NoButton);
    expect(bloom::test_support::awaitQtCondition(
               "the first pick is held", [&] { return held.load() == 1; }, kWait),
           "the first pick is held");
    app.mouse(QEvent::MouseButtonPress, second, Qt::LeftButton);
    app.mouse(QEvent::MouseButtonRelease, second, Qt::NoButton);
    hold = false;
    app.waitPick();
    QTest::qWait(50);
    expect(app.primary() == s.tri, "the newer click's answer applies, the older pick's never does");

    // The controller's route: a cancelled pick task answers Cancelled, never a stale selection.
    hold = true;
    auto submission = app.preview->submitViewerPick(
        {.snapshot = app.session.snapshot(),
         .composition = app.session.compositionId(),
         .time = kOne,
         .level = app.level(),
         .point = {110.0, 50.0},
         .tolerance = 0.0,
         .bounds = app.preview->currentLayerBounds(app.viewer->displayedPreviewState().frame),
         .plan = nullptr});
    expect(submission.accepted(), "a pick task is accepted");
    if (submission.accepted()) {
        expect(bloom::test_support::awaitQtCondition(
                   "the pick task is held on its worker", [&] { return held.load() == 1; }, kWait),
               "the pick task is held on its worker");
        submission.handle.cancel();
        hold = false;
        std::optional<runtime::TaskResult<std::shared_ptr<const runtime::ViewerPickResult>>> result;
        expect(bloom::test_support::awaitQtCondition(
                   "the cancelled pick finishes",
                   [&] {
                       if (!result)
                           result = submission.handle.tryTakeResult();
                       return result.has_value();
                   },
                   kWait),
               "the cancelled pick finishes");
        expect(result && result->state() == runtime::TaskState::Cancelled,
               "a cancelled pick answers Cancelled");
    }
    hold = false;
    request = {};
    request.level = std::vector<document::LayerId>{};
    (void)binding.setViewer(request);
    app.preview->setViewerPicker(nullptr);
}

ui::kit::KDialog* offer() {
    for (auto* widget : QApplication::topLevelWidgets())
        if (auto* dialog = qobject_cast<ui::kit::KDialog*>(widget);
            dialog != nullptr && dialog->objectName() == QStringLiteral("viewerBreakApartOffer") &&
            dialog->isVisible())
            return dialog;
    return nullptr;
}

void testBreakApartOffer() {
    App app;
    const auto layer = app.import(kOneLayer, commands::SvgImportMode::Layer, "Logo");
    expect(app.settle(), "the one-layer SVG is on the Viewer");
    const auto artworks = [&] {
        return std::ranges::count_if(app.composition().graph().nodes(), [](const auto& node) {
            return node.typeId == document::kVectorArtworkNodeType;
        });
    };
    expect(artworks() == 1, "one Vector Artwork");
    const auto revision = app.session.snapshot().revision();

    // Cancel changes nothing.
    app.doubleClick({100.0, 90.0});
    ui::kit::KDialog* dialog = nullptr;
    expect(bloom::test_support::awaitQtCondition(
               "double-click on a one-layer SVG offers Break Apart",
               [&] { return (dialog = offer()) != nullptr; }, std::chrono::milliseconds{5000}),
           "double-click on a one-layer SVG offers Break Apart");
    if (dialog == nullptr)
        return;
    expect(dialog->activate(ui::kit::KDialog::Role::Reject), "Cancel the offer");
    QTest::qWait(50);
    expect(app.session.snapshot().revision() == revision && artworks() == 1 && app.level().empty(),
           "Cancel breaks nothing apart and enters nothing");

    // Break Apart: one step, then the Viewer is inside the result with the part under the pointer.
    app.doubleClick({100.0, 90.0});
    dialog = nullptr;
    expect(bloom::test_support::awaitQtCondition(
               "the offer again", [&] { return (dialog = offer()) != nullptr; },
               std::chrono::milliseconds{5000}),
           "the offer again");
    if (dialog == nullptr)
        return;
    expect(dialog->activate(ui::kit::KDialog::Role::Accept), "Break Apart");
    expect(bloom::test_support::awaitQtCondition(
               "the SVG is broken apart and the Viewer enters it",
               [&] {
                   return !app.assets.busy() && artworks() == 0 &&
                          app.level() == std::vector{layer} &&
                          !app.viewer->breakApartDrillPendingForTest() &&
                          !app.viewer->drillPickPendingForTest() && app.primary().has_value();
               },
               kWait),
           "the SVG is broken apart and the Viewer enters it");
    const auto box = app.child(layer, "box");
    expect(app.level() == std::vector{layer} && app.primary() == box,
           "the Viewer is inside the broken-apart layer with the part under the pointer selected");
    expect(app.session.snapshot().revision().value() == revision.value() + 1 &&
               app.session.undoLabel() == QStringLiteral("Break Apart SVG"),
           "Break Apart is one undo step");
    expect(app.session.undo() && artworks() == 1, "one undo restores the one-layer SVG");
    expect(app.level().empty(), "and the level, no longer a group, returns to the top");
}


// ---- H: isolation, the breadcrumb, masks, deep gestures, stroke small groups ----

[[nodiscard]] document::ParameterId layerRole(const App& app, const document::LayerId layer,
                                              const std::string_view role) {
    const auto& graph = app.composition().graph();
    const auto* boundary = graph.findLayer(layer);
    const auto* binding = boundary ? document::layerParameterBinding(graph, *boundary, role) : nullptr;
    if (binding == nullptr)
        throw std::runtime_error("the layer has no " + std::string(role));
    return binding->parameterId;
}

// The CPU reference frame of the session's document at `time` (no cache).
std::shared_ptr<const runtime::ProcessFrame> renderNow(App& app, const core::RationalTime time) {
    const auto compiled = app.pipeline.compiler->compile(
        {.snapshot = app.session.snapshot(), .compositionId = app.session.compositionId()}, {});
    if (!compiled.plan)
        return nullptr;
    runtime::EvaluationRequest request{.time = time,
                                       .output = compiled.plan->output(),
                                       .resolution = runtime::CompositionFormatResolution{},
                                       .quality = runtime::EvaluationQuality::Reference,
                                       .colorIntent =
                                           runtime::EvaluationColorIntent::LinearRec709Scene,
                                       .pixelStorageByteLimit = std::size_t{1} << 30U};
    request.bypassOperationCache = true;
    return runtime::CpuCompositionEvaluator{}.evaluate(compiled.plan, request, {}).frame();
}

float alphaIn(const std::shared_ptr<const runtime::ProcessFrame>& frame, const std::int64_t x,
              const std::int64_t y) {
    if (frame == nullptr)
        return -1.0F;
    const auto& image = frame->processImage();
    const auto window = image.descriptor()->dataWindow();
    if (x < window.originX() || y < window.originY() || x >= window.maxXExclusive() ||
        y >= window.maxYExclusive())
        return 0.0F;
    const auto width = static_cast<std::int64_t>(window.extent().width());
    return image
        .pixels()[static_cast<std::size_t>((y - window.originY()) * width + (x - window.originX()))]
        .alpha();
}

bool samePixels(const std::shared_ptr<const runtime::ProcessFrame>& a,
                const std::shared_ptr<const runtime::ProcessFrame>& b) {
    if (a == nullptr || b == nullptr)
        return false;
    const auto left = a->processImage().pixels();
    const auto right = b->processImage().pixels();
    return left.size_bytes() == right.size_bytes() &&
           std::memcmp(left.data(), right.data(), left.size_bytes()) == 0;
}

// The Viewer shows a frame of the session's current revision and time.
bool settleRevision(App& app) {
    // A render-neutral revision keeps the frame of an earlier one (retained provenance): the
    // frame must be the controller's current answer, for this level, painted.
    const bool ok = bloom::test_support::awaitQtCondition(
        "the Viewer shows the current revision",
        [&] {
            const auto state = app.viewer->displayedPreviewState();
            const auto& desired = app.preview->state().desiredIdentity;
            return state.frame != nullptr && desired.has_value() &&
                   app.preview->state().activity == ui::PreviewActivity::Ready &&
                   state.freshness == ui::FrameFreshness::Current &&
                   state.frame->desiredIdentity().time == app.session.currentTime() &&
                   state.frame->desiredIdentity().isolation == app.preview->isolation() &&
                   app.viewer->mappingForTest().has_value();
        },
        kWait);
    if (!ok) {
        const auto state = app.viewer->displayedPreviewState();
        std::cerr << "settleRevision: frame " << (state.frame != nullptr);
        if (state.frame)
            std::cerr << " time " << (state.frame->desiredIdentity().time == app.session.currentTime())
                      << " revision " << state.frame->desiredIdentity().sourceRevision.value() << "/"
                      << app.session.snapshot().revision().value() << " isolation "
                      << state.frame->desiredIdentity().isolation.size() << "/"
                      << app.preview->isolation().size();
        std::cerr << " mapping " << app.viewer->mappingForTest().has_value() << '\n';
    }
    QApplication::processEvents();
    return ok;
}

// Inside a group the outside is dimmed and not pickable; the Viewer's request carries the level.
void testIsolation(App& app, const Scene& s) {
    expect(app.session.currentTime() == kOne || app.session.setCurrentTime(kOne), "go to 1 s");
    const std::vector<document::LayerId> outer{s.root, s.outer};
    expect(app.session.setViewerLevel({outer, std::nullopt}).ok(), "enter outer");
    expect(app.preview->isolation() == outer, "the Viewer's request names the entered groups");
    expect(settleRevision(app), "the Viewer shows the dimmed frame");
    const auto shown = app.viewer->displayedPreviewState().frame;
    expect(shown && shown->desiredIdentity().isolation == outer,
           "the displayed frame is the one for this level");
    // The backdrop, outside outer, draws dimmed and cannot be picked.
    runtime::ViewerPicker picker([&](const runtime::SnapshotCompileRequest& request,
                                     const runtime::CancellationToken& cancellation) {
        return app.pipeline.compiler->compile(request, cancellation);
    });
    const auto bounds = runtime::queryLayerBounds(*app.pipeline.compiler, app.pipeline.evaluator,
                                                  {.snapshot = app.session.snapshot(),
                                                   .composition = app.session.compositionId(),
                                                   .time = kOne,
                                                   .layers = {}});
    const auto picked = picker.pick({.snapshot = app.session.snapshot(),
                                     .composition = app.session.compositionId(),
                                     .time = kOne,
                                     .level = outer,
                                     .point = {30.0, 30.0},
                                     .tolerance = 0.0,
                                     .bounds = bounds.bounds,
                                     .plan = nullptr},
                                    {});
    expect(picked.status == runtime::ViewerPickStatus::Picked && !picked.child,
           "a dimmed layer outside the group is not picked");
    app.session.selectLayerFromViewer(s.middle);
    app.click({30.0, 30.0});
    expect(!app.primary() && app.level() == outer,
           "a click on the dimmed backdrop selects nothing and stays inside");
    expect(app.session.setViewerLevel({}).ok() && app.preview->isolation().empty(),
           "back at the top level the request carries no isolation");
    expect(settleRevision(app), "the Viewer shows the undimmed frame");
}

void testBreadcrumb(App& app, const Scene& s) {
    auto* crumb = app.viewer->breadcrumbForTest();
    expect(crumb != nullptr && crumb->objectName() == QStringLiteral("viewerBreadcrumb"),
           "the Viewer header has the breadcrumb");
    if (crumb == nullptr)
        return;
    expect(app.session.setViewerLevel({}).ok(), "top level");
    expect(crumb->property("chromeSuppressed").toBool() && crumb->crumbs().empty(),
           "the breadcrumb is hidden at the top level");
    const std::vector<document::LayerId> deep{s.root, s.outer, s.middle, s.inner};
    expect(app.session.setViewerLevel({deep, std::nullopt}).ok(), "inside inner");
    const auto texts = [&] {
        std::vector<QString> out;
        for (const auto& item : crumb->crumbs())
            out.push_back(item.text);
        return out;
    };
    expect(!crumb->property("chromeSuppressed").toBool() &&
               texts() == std::vector<QString>{QStringLiteral("Drill"), QStringLiteral("Art"),
                                               QStringLiteral("outer"), QStringLiteral("middle"),
                                               QStringLiteral("inner")},
           "the breadcrumb names the composition and every entered group, outermost first");
    expect(app.session.enterViewerPoints(s.ring), "edit ring's points");
    expect(texts().back() == QStringLiteral("ring") && crumb->suffix() == QStringLiteral("Points") &&
               crumb->toolTip() ==
                   QStringLiteral("Drill ▸ Art ▸ outer ▸ middle ▸ inner ▸ "
                                  "ring · Points"),
           "point editing shows the shape and Points; the tooltip has the whole path");
    // A click on a crumb of the Viewer's own breadcrumb.
    const auto clickOn = [](ui::kit::KBreadcrumb& widget, const int target) {
        for (const auto& entry : widget.shown())
            if (entry.target == target) {
                const QPointF point = QRectF(entry.rect).center();
                QMouseEvent press(QEvent::MouseButtonPress, point, widget.mapToGlobal(point),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(&widget, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, point, widget.mapToGlobal(point),
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(&widget, &release);
                return true;
            }
        return false;
    };
    crumb->resize(crumb->sizeHint());
    auto shown = crumb->shown();
    expect(shown.size() == 6 && std::ranges::none_of(shown, [](const auto& entry) {
               return entry.elided;
           }),
           "with room, every crumb shows in full");
    expect(clickOn(*crumb, 5) && app.session.viewerLevel().points == s.ring,
           "a click on the current crumb does nothing");
    expect(clickOn(*crumb, 4) && app.level() == deep && !app.session.viewerLevel().points &&
               app.primary() == s.ring,
           "a click on the innermost group leaves the points with the shape selected");
    expect(clickOn(*crumb, 2) && app.level() == std::vector{s.root, s.outer} &&
               app.primary() == s.middle,
           "a click on a crumb jumps to that level with the group left selected");
    expect(clickOn(*crumb, 0) && app.level().empty() && app.primary() == s.root,
           "a click on the composition returns to the top level");

    // Eliding and the keyboard, on a shown breadcrumb with the Viewer's crumbs and its jump (this
    // Viewer is not hosted by an EditorArea, so its header row, and the breadcrumb, is not shown).
    expect(app.session.setViewerLevel({deep, std::nullopt}).ok(), "inside inner again");
    QWidget host;
    auto* mirror = new ui::kit::KBreadcrumb(&host);
    mirror->setCrumbs(crumb->crumbs(), crumb->suffix());
    QObject::connect(mirror, &ui::kit::KBreadcrumb::crumbActivated, app.viewer,
                     &ui::ViewerEditor::jumpToViewerCrumb);
    host.resize(mirror->sizeHint().width() + 40, mirror->sizeHint().height() + 20);
    mirror->resize(mirror->sizeHint());
    host.show();
    QApplication::processEvents();
    const auto full = mirror->sizeHint().width();
    mirror->resize(full - 1, mirror->height());
    shown = mirror->shown();
    expect(shown.size() >= 3 && shown.size() <= 5 && shown[1].text == QStringLiteral("…") &&
               shown.front().text == QStringLiteral("Drill") && !shown.front().elided &&
               shown.back().text == QStringLiteral("inner") && !shown.back().elided,
           "a little short of room, the middle crumbs fold into … first and the ends stay whole");
    expect(mirror->toolTip().contains(QStringLiteral("outer")),
           "the tooltip still names every level");
    mirror->resize(mirror->minimumSizeHint().width(), mirror->height());
    shown = mirror->shown();
    expect(shown.size() == 3 && shown[1].text == QStringLiteral("…"),
           "at its narrowest the path is the first crumb, one … and the last crumb");
    // Keyboard: Tab lands on the innermost way back; Left and Enter go further out.
    mirror->resize(mirror->sizeHint());
    // Focus as Tab gives it (the offscreen platform does not activate windows).
    QFocusEvent tabIn(QEvent::FocusIn, Qt::TabFocusReason);
    QApplication::sendEvent(mirror, &tabIn);
    expect(mirror->focusPolicy() == Qt::StrongFocus && mirror->focusedCrumb() == 3,
           "keyboard focus lands on the innermost crumb to go back to");
    QTest::keyClick(mirror, Qt::Key_Left);
    expect(mirror->focusedCrumb() == 2, "Left moves out one crumb");
    QTest::keyClick(mirror, Qt::Key_Return);
    expect(app.level() == std::vector{s.root, s.outer} && app.primary() == s.middle,
           "Enter on a crumb jumps to its level");
    expect(app.session.setViewerLevel({}).ok(), "back to the top");
    app.viewer->setFocus();
}

// A pick honours the layer's masks, as the frame draws them.
void testMaskedPick(App& app, const Scene& s) {
    runtime::ViewerPicker picker([&](const runtime::SnapshotCompileRequest& request,
                                     const runtime::CancellationToken& cancellation) {
        return app.pipeline.compiler->compile(request, cancellation);
    });
    const auto pickAt = [&](const std::vector<document::LayerId>& level,
                            const document::Vec2d point) {
        const auto snapshot = app.session.snapshot();
        const auto bounds = runtime::queryLayerBounds(*app.pipeline.compiler, app.pipeline.evaluator,
                                                      {.snapshot = snapshot,
                                                       .composition = app.session.compositionId(),
                                                       .time = kZero,
                                                       .layers = {}});
        return picker.pick({.snapshot = snapshot,
                            .composition = app.session.compositionId(),
                            .time = kZero,
                            .level = level,
                            .point = point,
                            .tolerance = 0.0,
                            .bounds = bounds.bounds,
                            .plan = nullptr},
                           {});
    };
    // The left part of the canvas: x from 0 to 160.
    const auto corner = [](const double x, const double y) {
        return document::PathAnchor{.point = {x, y}, .inHandle = {}, .outHandle = {}};
    };
    const auto left = document::PathValue::fromContour(
        {corner(0.0, 0.0), corner(160.0, 0.0), corner(160.0, 300.0), corner(0.0, 300.0)}, true);
    const std::vector<document::LayerId> inner{s.root, s.outer, s.middle, s.inner};
    const std::vector<document::LayerId> middle{s.root, s.outer, s.middle};
    for (const auto& [masked, level, label] :
         {std::tuple{s.ring, inner, std::string("a mask on ring")},
          std::tuple{s.inner, middle, std::string("a mask on the inner group")}}) {
        const auto expectedChild = masked;
        expect(pickAt(level, {210.0, 50.0}).child == expectedChild,
               label + ": before the mask, the ring's right side picks");
        commands::Transaction transaction("Mask", app.session.snapshot().revision());
        transaction.emplace<commands::AddLayerMask>(app.session.compositionId(), masked, left);
        expect(app.session.executeTransaction(std::move(transaction)).succeeded(),
               label + ": the mask is added");
        const auto frame = renderNow(app, kZero);
        expect(alphaIn(frame, 210, 50) == 0.0F && alphaIn(frame, 110, 50) > 0.5F,
               label + ": the frame draws the ring only inside the mask");
        expect(!pickAt(level, {210.0, 50.0}).child,
               label + ": a click outside the mask misses, as nothing is drawn there");
        expect(pickAt(level, {110.0, 50.0}).child == expectedChild,
               label + ": a click inside the mask still picks");
        expect(app.session.undo(), label + ": undo the mask");
    }
}

// Move, scale and rotate on a layer three groups deep (inner, inside middle, outer and the
// artwork's root), inside turned and scaled groups: each one command, and the result equals the
// same values set as Properties sets them. A group, not a Path shape: a selected Path shape shows
// its points, whose handles answer before the transform gizmo does.
void testDeepGestures(App& app, const Scene& s) {
    expect(app.session.currentTime() == kZero || app.session.setCurrentTime(kZero), "go to 0 s");
    expect(app.session.setParameterValue(layerRole(app, s.middle, document::kRotationParameterRole),
                                         15.0, QStringLiteral("Rotate")),
           "turn middle");
    expect(app.session.setParameterValue(layerRole(app, s.outer, document::kScaleParameterRole),
                                         document::Vec2d{1.2, 1.2}, QStringLiteral("Scale")),
           "scale outer");
    const std::vector<document::LayerId> deep{s.root, s.outer, s.middle};
    enum class Grip { Move, Scale, Rotate };
    for (const auto grip : {Grip::Move, Grip::Scale, Grip::Rotate}) {
        const std::string label = grip == Grip::Move    ? "move inner"
                                  : grip == Grip::Scale ? "scale inner"
                                                        : "rotate inner";
        expect(app.session.setViewerLevel({deep, std::nullopt}).ok(), label + ": inside middle");
        app.session.selectLayerFromViewer(s.inner);
        expect(settleRevision(app), label + ": the Viewer shows the current frame");
        const auto mapping = app.viewer->mappingForTest();
        const auto bounds = app.preview->selectedLayerBounds(app.preview->state().frame);
        expect(mapping && bounds.size() == 1 && bounds.front().layerId == s.inner,
               label + ": inner's gizmo is composed through its three groups");
        if (!mapping || bounds.empty())
            continue;
        const auto handles = ui::viewerHandlePoints(*mapping, bounds.front());
        // Inside its box, off its anchor (which has its own handle) and on the canvas.
        const auto& quad = bounds.front().polygon;
        QPointF press = mapping->toScreen(
            {quad[0].x + 0.35 * (quad[1].x - quad[0].x) + 0.65 * (quad[3].x - quad[0].x),
             quad[0].y + 0.35 * (quad[1].y - quad[0].y) + 0.65 * (quad[3].y - quad[0].y)});
        QPointF drag = press + QPointF(24.0, 14.0);
        auto region = ui::ViewerHitRegion::Move;
        if (grip == Grip::Scale) {
            press = handles[4];
            drag = press + QPointF(18.0, 12.0);
            region = ui::ViewerHitRegion::Scale;
        } else if (grip == Grip::Rotate) {
            press = handles[4] + (handles[4] - handles[0]) * (12.0 / std::max(1.0, QLineF(handles[0], handles[4]).length()));
            drag = press + QPointF(-30.0, 20.0);
            region = ui::ViewerHitRegion::Rotate;
        }
        expect(ui::hitTestViewer(*mapping, press, bounds, bounds).region == region &&
                   app.viewer->contentRectForTest().contains(press) &&
                   app.viewer->contentRectForTest().contains(drag),
               label + ": the press lands on its gizmo region, on the canvas");
        const auto centreBefore = bounds.front().output.centre();
        const auto revision = app.session.snapshot().revision();
        const auto history = app.session.undoLabel();
        // inner's own transform values, before and after the gesture.
        std::vector<document::ParameterId> transform;
        for (const auto role : {document::kPositionParameterRole, document::kAnchorParameterRole,
                                document::kScaleParameterRole, document::kRotationParameterRole})
            transform.push_back(layerRole(app, s.inner, role));
        const auto stored = [&](const document::ParameterId id) -> std::optional<document::ParameterValue> {
            const auto* parameter = app.composition().parameters().find(id);
            const auto* constant =
                parameter ? std::get_if<document::ConstantValueSource>(&parameter->source) : nullptr;
            return constant ? std::optional(constant->value) : std::nullopt;
        };
        std::vector<std::optional<document::ParameterValue>> valuesBefore;
        for (const auto id : transform)
            valuesBefore.push_back(stored(id));
        app.mouse(QEvent::MouseButtonPress, press, Qt::LeftButton);
        for (int step = 1; step <= 4; ++step) {
            const auto point = press + (drag - press) * (step / 4.0);
            QMouseEvent move(QEvent::MouseMove, point, app.viewer->mapToGlobal(point),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(app.viewer, &move);
        }
        app.mouse(QEvent::MouseButtonRelease, drag, Qt::NoButton);
        QApplication::processEvents();
        expect(app.session.snapshot().revision().value() == revision.value() + 1 &&
                   app.session.undoLabel() != history,
               label + ": the drag is one command");
        std::vector<std::pair<document::ParameterId, document::ParameterValue>> values;
        for (std::size_t index = 0; index < transform.size(); ++index)
            if (const auto value = stored(transform[index]); value && value != valuesBefore[index])
                values.emplace_back(transform[index], *value);
        expect(!values.empty(), label + ": the drag edited inner's transform");
        if (grip == Grip::Move) {
            expect(settleRevision(app), label + ": the moved frame is shown");
            const auto after = app.preview->selectedLayerBounds(app.preview->state().frame);
            const auto moved = mapping->toComposition(drag);
            const auto from = mapping->toComposition(press);
            expect(!after.empty() &&
                       std::abs(after.front().output.centre().x - centreBefore.x -
                                (moved.x - from.x)) < 0.75 &&
                       std::abs(after.front().output.centre().y - centreBefore.y -
                                (moved.y - from.y)) < 0.75,
                   label + ": inner follows the pointer through its turned, scaled groups");
        }
        const auto gestured = renderNow(app, kZero);
        expect(app.session.undo() && app.session.snapshot().revision() != revision &&
                   app.session.undoLabel() == history,
               label + ": one undo restores inner");
        expect(app.session.setParameterValues(values, QStringLiteral("Properties")),
               label + ": the same values are set as Properties sets them");
        const auto typed = renderNow(app, kZero);
        expect(samePixels(gestured, typed),
               label + ": the gesture's frame equals the Properties edit's, pixel for pixel");
        expect(app.session.undo(), label + ": undo the Properties edit");
    }
    expect(app.session.undo() && app.session.undo(), "restore outer and middle");
    expect(app.session.setViewerLevel({}).ok(), "back to the top");
}

// The importer's dashed-stroke and gradient-stroke shapes are small groups: they pick on the drawn
// stroke (a dash, not a gap) and a double-click edits their outline.
constexpr auto kStrokes =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300" viewBox="0 0 400 300">
<defs><linearGradient id="edge"><stop offset="0" stop-color="#ffff00"/><stop offset="1" stop-color="#00ffff"/></linearGradient></defs>
<g id="set">
<path id="dashed" fill="#446688" stroke="#ffffff" stroke-width="10" stroke-dasharray="20 20" d="M60 60H160V160H60Z"/>
<path id="graded" fill="#cccccc" stroke="url(#edge)" stroke-width="10" d="M240 60H340V160H240Z"/>
<rect id="plain" x="60" y="220" width="40" height="40" fill="#999999"/>
</g>
</svg>
)svg";

void testStrokeSmallGroups() {
    App app;
    const auto root = app.import(kStrokes, commands::SvgImportMode::Shapes, "Strokes");
    const auto set = app.child(root, "set");
    const auto dashed = app.child(set, "dashed");
    const auto graded = app.child(set, "graded");
    const auto& c = app.composition();
    const auto dashedShape = document::drillShape(c, dashed);
    const auto gradedShape = document::drillShape(c, graded);
    expect(dashedShape && dashedShape->smallGroup && !document::isDrillGroup(c, dashed),
           "a dashed stroke over a fill imports as a small-group shape");
    expect(gradedShape && gradedShape->smallGroup && !document::isDrillGroup(c, graded),
           "a gradient stroke over a fill imports as a small-group shape");
    runtime::ViewerPicker picker([&](const runtime::SnapshotCompileRequest& request,
                                     const runtime::CancellationToken& cancellation) {
        return app.pipeline.compiler->compile(request, cancellation);
    });
    const auto snapshot = app.session.snapshot();
    const auto bounds = runtime::queryLayerBounds(*app.pipeline.compiler, app.pipeline.evaluator,
                                                  {.snapshot = snapshot,
                                                   .composition = app.session.compositionId(),
                                                   .time = kZero,
                                                   .layers = {}});
    const std::vector level{root, set};
    const auto pick = [&](const document::Vec2d point) {
        return picker
            .pick({.snapshot = snapshot,
                   .composition = app.session.compositionId(),
                   .time = kZero,
                   .level = level,
                   .point = point,
                   .tolerance = 0.0,
                   .bounds = bounds.bounds,
                   .plan = nullptr},
                  {})
            .child;
    };
    const auto frame = renderNow(app, kZero);
    expect(alphaIn(frame, 70, 57) > 0.5F && alphaIn(frame, 90, 57) == 0.0F,
           "the dashed stroke draws a dash at (70, 57) and a gap at (90, 57)");
    expect(pick({70.0, 57.0}) == dashed, "a dash of the dashed stroke picks the shape");
    expect(!pick({90.0, 57.0}), "a gap between dashes, outside the fill, misses");
    expect(pick({110.0, 110.0}) == dashed, "the dashed shape's fill picks it");
    expect(pick({290.0, 57.0}) == graded, "the gradient stroke picks its shape on the stroke");
    expect(!pick({290.0, 50.0}), "just outside the gradient stroke misses");
    expect(pick({290.0, 110.0}) == graded, "the gradient-stroked shape's fill picks it");
    // Enter: the root, the set, then the dashed shape's points.
    app.doubleClick({70.0, 57.0});
    app.doubleClick({70.0, 57.0});
    expect(app.level() == level && app.primary() == dashed,
           "double-clicks enter the set and select the dashed shape");
    app.doubleClick({70.0, 57.0});
    expect(app.session.viewerLevel().points == dashed && app.primary() == dashedShape->edited,
           "a double-click on the dashed shape edits its outline");
    app.key(Qt::Key_Escape);
    app.doubleClick({290.0, 57.0});
    expect(app.session.viewerLevel().points == graded && app.primary() == gradedShape->edited,
           "a double-click on the gradient-stroked shape edits its outline");
    app.key(Qt::Key_Escape);
}

// A single click that lands before the changed selection's overlay is painted is not dropped:
// inside a group the displayed frame's mapping picks it.
void testClickBeforeOverlayPaints(App& app, const Scene& s) {
    expect(app.session.setViewerLevel({{s.root, s.outer, s.middle, s.inner}, std::nullopt}).ok(),
           "inside inner");
    app.session.selectLayerFromViewer(s.tri);
    expect(app.settle(), "the Viewer paints the selection");
    const auto screen = app.at({110.0, 50.0}); // on the ring
    app.session.selectLayerFromViewer(s.tri);
    app.session.clearSelectionFromViewer();
    app.session.selectLayerFromViewer(s.tri);
    // No event loop since the selection changed: the overlay of this selection is not painted.
    expect(!app.viewer->mappingForTest().has_value(),
           "the selection overlay is not painted yet, so currentMapping() is empty");
    app.mouse(QEvent::MouseButtonPress, screen, Qt::LeftButton);
    app.mouse(QEvent::MouseButtonRelease, screen, Qt::NoButton);
    app.waitPick();
    expect(app.primary() == s.ring && app.level().size() == 4,
           "the click before the overlay painted still picks the ring and keeps the level");
    expect(app.session.setViewerLevel({}).ok(), "back to the top");
}

// #644: a Path node drawing a nested shape is keyed from the Timeline: the layer's expanded rows list
// the Path node's Value with the same diamond, and one click is one whole-path key at the playhead.
void testTimelineKeysAPathNode(App& app, const Scene& s) {
    const auto keys = [&] { return pathKeys(app.composition(), s.triPath); };
    expect(keys() == 2, "tri's Path node starts with the two keys of the scene");
    ui::TimelineEditor editor(app.session, *app.preview);
    editor.resize(1400, 3200); // Tall enough that no opened row is scrolled out of the table.
    editor.show();
    app.session.selectLayerFromViewer(s.tri);
    QApplication::processEvents();
    // The shape sits three groups deep: open each group, then the shape's own rows.
    for (const auto layer : {s.root, s.outer, s.middle, s.inner, s.tri}) {
        editor.layerStackForTest()->expansionRequested(layer);
        QApplication::processEvents();
    }
    const auto diamond = [&]() -> ui::KeyframeDiamond* {
        for (auto* candidate : editor.findChildren<ui::KeyframeDiamond*>())
            if (candidate->isVisible() && candidate->parameterId() == s.triPath)
                return candidate;
        return nullptr;
    };
    expect(diamond() != nullptr, "the Timeline row of the Path node's Value carries a key diamond");
    if (diamond() == nullptr)
        return;
    const auto half = core::RationalTime::create(12, 24);
    expect(half && app.session.setCurrentTime(*half), "the playhead moves between the two keys");
    QApplication::processEvents();
    expect(diamond()->state() == ui::KeyframeDiamondState::AnimatedWithoutKey,
           "between keys the Timeline diamond is animated without a key");
    const auto history = app.session.snapshot().revision();
    QTest::mouseClick(diamond(), Qt::LeftButton);
    QApplication::processEvents();
    expect(keys() == 3 && diamond()->state() == ui::KeyframeDiamondState::AnimatedWithKey &&
               app.session.snapshot().revision() != history,
           "one click on the Timeline diamond adds one whole-path key at the playhead");
    QTest::mouseClick(diamond(), Qt::LeftButton);
    QApplication::processEvents();
    expect(keys() == 2 && diamond()->state() == ui::KeyframeDiamondState::AnimatedWithoutKey,
           "a click on that key removes it");
    expect(app.session.undo() && keys() == 3, "one undo brings the key back");
    expect(app.session.undo() && keys() == 2, "one more undo removes it");
    expect(app.session.setCurrentTime(kOne), "back to 1 s");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("BloomViewerDrill"));
    QApplication::setApplicationName(QStringLiteral("ViewerDrill"));
    ui::kit::installAfterlabTheme(application);
    try {
        {
            App app;
            const auto scene = buildScene(app);
            testStructure(app, scene);
            testPicker(app, scene);
            testDrillGestures(app, scene);
            testSelectionFollowsLevel(app, scene);
            testKeyedPointDrag(app, scene);
            testPickThreadStaleAndCancel(app, scene);
            testIsolation(app, scene);
            testBreadcrumb(app, scene);
            testMaskedPick(app, scene);
            testDeepGestures(app, scene);
            testClickBeforeOverlayPaints(app, scene);
            testTimelineKeysAPathNode(app, scene);
        }
        testBreakApartOffer();
        testStrokeSmallGroups();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures + bloom::test_support::awaitFailureCount() != 0) {
        std::cerr << failures << " viewer drill expectation(s) failed\n";
        return 1;
    }
    std::cout << "PASS: viewer drill\n";
    return 0;
}
