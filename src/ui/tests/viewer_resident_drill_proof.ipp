// #645 (H, addition 4): drilling into grouped artwork on the genuine GPU-resident Viewer route. An
// SVG imported as editable shapes (three nested groups) is entered with native double-clicks on the
// Vulkan window: every entered level is presented as a resident frame whose identity names the
// level (the dimmed outside, prepared on the device), the picks inside a group run on the resident
// frame's own evaluated geometry and that geometry equals the CPU reference's, a click in the
// ring's even-odd hole picks nothing while a click on its fill picks it, and Escape backs out to
// an undimmed resident frame. Included into viewer_resident_interaction_native_tests.cpp.

#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/document/drill_levels.hpp>
#include <bloom/runtime/layer_bounds_query.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>

#include <cstring>

namespace {

constexpr auto kResidentDrillArtwork =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300" viewBox="0 0 400 300">
<rect id="backdrop" x="0" y="0" width="60" height="60" fill="#808080"/>
<g id="outer">
<rect id="box" x="20" y="200" width="100" height="60" fill="#55aa55"/>
<g id="middle">
<g id="inner">
<path id="ring" fill="#336699" fill-rule="evenodd" d="M100 40H220V160H100Z M130 70H180V120H130Z"/>
<path id="tri" fill="#cc3333" d="M240 100L320 100L320 180Z"/>
</g>
</g>
</g>
</svg>
)svg";

[[nodiscard]] std::optional<document::LayerId> residentChild(const document::Composition& composition,
                                                           const document::LayerId parent,
                                                           const std::string_view name) {
    const auto merge = parent.isValid() ? document::layerContentMerge(composition, parent)
                                        : composition.graph().outputMergeId();
    if (merge)
        for (const auto layer : document::mergeEntryLayers(composition, *merge))
            if (const auto* boundary = composition.graph().findLayer(layer);
                boundary != nullptr && boundary->name == name)
                return layer;
    return std::nullopt;
}

// The displayed frame's Layer Output geometry: what the Viewer's pick reads.
[[nodiscard]] std::vector<runtime::EvaluatedOperationBounds>
residentLayerBounds(const ui::PreparedPreviewFrameHandle& frame) {
    if (frame == nullptr || frame->processIdentity().plan == nullptr)
        return {};
    return runtime::layerOutputBounds(*frame->processIdentity().plan, frame->evaluatedBounds());
}

// A point of a layer's local box, in composition pixels, through the displayed frame's bounds.
[[nodiscard]] std::optional<document::Vec2d>
residentPointOf(const ui::PreparedPreviewFrameHandle& frame, const document::LayerId layer,
                const double fx, const double fy) {
    for (const auto& bounds : residentLayerBounds(frame))
        if (bounds.layerId == layer) {
            const auto& p = bounds.polygon;
            return document::Vec2d{p[0].x + fx * (p[1].x - p[0].x) + fy * (p[3].x - p[0].x),
                                   p[0].y + fx * (p[1].y - p[0].y) + fy * (p[3].y - p[0].y)};
        }
    return std::nullopt;
}

bool exerciseResidentDrill(Expectations& checks, ui::CompositionSession& session,
                           ui::CompositionPreviewController& controller, ui::ViewerEditor& viewer,
                           QWidget& host, GpuPreviewDisplayService& service,
                           const runtime::SnapshotCompiler& compiler,
                           const runtime::CpuCompositionEvaluator& evaluator) {
    const auto size = std::strlen(kResidentDrillArtwork);
    auto bytes = std::make_shared<std::vector<std::byte>>(size);
    std::memcpy(bytes->data(), kResidentDrillArtwork, size);
    const auto source = commands::prepareSvgSource(std::move(bytes), "drill.svg");
    checks.expect(source.source != nullptr, "the drill artwork is prepared");
    if (!source.source)
        return false;
    commands::Transaction transaction("Import drill artwork", session.snapshot().revision());
    transaction.emplace<commands::AddSvgArtwork>(session.compositionId(), source.source, "Art",
                                                 commands::SvgImportMode::Shapes, true);
    const auto imported = session.executeTransaction(std::move(transaction));
    const auto rootLayer = imported.outputId<document::LayerId>("layer");
    checks.expect(imported.succeeded() && rootLayer.has_value(), "the artwork imports as shapes");
    if (!rootLayer)
        return false;
    const auto& composition = *session.composition();
    const auto root = *rootLayer;
    const auto outer = residentChild(composition, root, "outer");
    const auto middle = outer ? residentChild(composition, *outer, "middle") : std::nullopt;
    const auto inner = middle ? residentChild(composition, *middle, "inner") : std::nullopt;
    const auto ring = inner ? residentChild(composition, *inner, "ring") : std::nullopt;
    checks.expect(outer && middle && inner && ring, "the three nested groups are imported");
    if (!ring)
        return false;
    session.clearSelection();

    const auto residentAt = [&](const std::vector<document::LayerId>& level,
                                const std::string& label) {
        const bool ok = waitUntil(
            [&] {
                const auto frame = controller.state().frame;
                const auto actual = viewer.actualDisplayDescriptor();
                return isResidentFrame(frame) && viewer.residentPresentationActiveForTest() &&
                       frame->desiredIdentity().sourceRevision == session.snapshot().revision() &&
                       frame->desiredIdentity().isolation == level && actual && actual->native &&
                       actual->frame && actual->frame->desiredIdentity() == frame->desiredIdentity() &&
                       viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current &&
                       viewer.mappingForTest().has_value();
            },
            30s);
        checks.expect(ok, label + ": a GPU-resident frame for this level is presented");
        return ok;
    };
    if (!residentAt({}, "top level"))
        return false;

    QWidget* container = nullptr;
    checks.expect(waitUntil(
                      [&] {
                          container = nativeContainer(viewer);
                          return container != nullptr && container->isVisible() &&
                                 !container->geometry().isEmpty();
                      },
                      15s),
                  "the resident Vulkan container is visible");
    QWindow* window = container ? nativeVulkanWindow(container, &host) : nullptr;
    checks.expect(window != nullptr, "the presenter owns its Vulkan window");
    if (window == nullptr)
        return false;

    // A press needs the painted mapping of the current selection overlay, as an artist's click
    // lands after the frame that shows the last change.
    const auto screenOf = [&](const document::Vec2d point) {
        static_cast<void>(waitUntil([&] { return viewer.mappingForTest().has_value(); }, 10s));
        return viewer.mappingForTest()->toScreen(point);
    };
    const auto waitPick = [&] {
        static_cast<void>(waitUntil([&] { return !viewer.drillPickPendingForTest(); }, 10s));
        QApplication::processEvents();
    };
    const auto doubleClick = [&](const document::Vec2d point) {
        const auto at = screenOf(point);
        sendNativeMouse(*window, *container, QEvent::MouseButtonPress, at, Qt::LeftButton,
                        Qt::LeftButton);
        sendNativeMouse(*window, *container, QEvent::MouseButtonRelease, at, Qt::LeftButton,
                        Qt::NoButton);
        waitPick();
        sendNativeMouse(*window, *container, QEvent::MouseButtonDblClick, at, Qt::LeftButton,
                        Qt::LeftButton);
        sendNativeMouse(*window, *container, QEvent::MouseButtonRelease, at, Qt::LeftButton,
                        Qt::NoButton);
        waitPick();
    };
    const auto click = [&](const document::Vec2d point) {
        const auto at = screenOf(point);
        sendNativeMouse(*window, *container, QEvent::MouseButtonPress, at, Qt::LeftButton,
                        Qt::LeftButton);
        sendNativeMouse(*window, *container, QEvent::MouseButtonRelease, at, Qt::LeftButton,
                        Qt::NoButton);
        waitPick();
    };
    const auto primary = [&]() -> std::optional<document::LayerId> {
        if (const auto* layer = std::get_if<document::LayerId>(&session.selection().primary))
            return *layer;
        return std::nullopt;
    };

    const auto onFill = residentPointOf(controller.state().frame, *ring, 0.1, 0.1);
    checks.expect(onFill.has_value(), "the ring's displayed bounds give a point on its fill");
    if (!onFill)
        return false;
    const auto countersBefore = service.status().counters;
    std::vector<document::LayerId> level;
    const std::vector<std::pair<document::LayerId, document::LayerId>> steps{
        {root, *outer}, {*outer, *middle}, {*middle, *inner}, {*inner, *ring}};
    for (const auto& [entered, selected] : steps) {
        doubleClick(*onFill);
        level.push_back(entered);
        const auto name = std::to_string(level.size());
        checks.expect(session.viewerLevel().groups == level && primary() == selected,
                      "native double-click " + name +
                          " enters the group and selects the child under the pointer");
        if (!residentAt(level, "level " + name))
            return false;
    }
    checks.expect(service.status().counters.nativeDispatches > countersBefore.nativeDispatches &&
                      service.status().counters.fullFrameReadbacks ==
                          countersBefore.fullFrameReadbacks,
                  "each dimmed level was prepared on the device with no full-frame readback");

    // The picks inside the group read the resident frame's own geometry: it equals the CPU's.
    const auto frame = controller.state().frame;
    const auto cpu = runtime::queryLayerBounds(compiler, evaluator,
                                               {.snapshot = session.snapshot(),
                                                .composition = session.compositionId(),
                                                .time = session.currentTime(),
                                                .layers = {}});
    checks.expect(cpu.succeeded(), "the CPU bounds query evaluates: " + cpu.diagnostic);
    const auto resident = residentLayerBounds(frame);
    bool sameGeometry = !resident.empty() && cpu.succeeded();
    for (const auto layer : {root, *outer, *middle, *inner, *ring}) {
        const auto find = [&](const auto& list) -> const runtime::EvaluatedOperationBounds* {
            for (const auto& bounds : list)
                if (bounds.layerId == layer)
                    return &bounds;
            return nullptr;
        };
        const auto* gpu = find(resident);
        const auto* reference = find(cpu.bounds);
        sameGeometry = sameGeometry && gpu != nullptr && reference != nullptr &&
                       gpu->local == reference->local && gpu->output == reference->output &&
                       gpu->polygon == reference->polygon;
    }
    checks.expect(sameGeometry, "the resident frame's evaluated geometry, which the pick reads, "
                                "equals the CPU reference's for every entered layer");
    const auto inHole = residentPointOf(frame, *ring, 0.375, 0.375);
    if (inHole) {
        click(*inHole);
        checks.expect(!primary() && session.viewerLevel().groups == level,
                      "a native click in the ring's even-odd hole picks nothing and stays inside");
    }
    click(*onFill);
    checks.expect(primary() == *ring, "a native click on the ring's fill picks the ring");

    // Back out with Escape to an undimmed resident frame.
    viewer.setFocus(Qt::OtherFocusReason);
    for (std::size_t index = 0; index < steps.size(); ++index) {
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(&viewer, &escape);
        QCoreApplication::processEvents();
    }
    checks.expect(session.viewerLevel().groups.empty() && primary() == root,
                  "Escape backs out to the top level with the artwork selected");
    return residentAt({}, "back at the top level");
}

} // namespace
