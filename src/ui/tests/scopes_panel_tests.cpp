// #724, #730 Scopes: the Scopes panel over genuine CPU reference frames; no device and no Viewer.
//
// Asserted:
//   * `bloom.scopes`, "Scopes", is registered like every editor; the area hosts its header menus
//     (Signal, Region, Scopes) and the panel holds nothing but its five collapsible panes: no
//     status, warning or other text, no slider, dropdown, check box or button;
//   * the header menus write the session's settings: Signal (Display, Working, the Working axis
//     maximum, Ignore Transparent Pixels), Region (Draw and Clear Scope Region, disabled with no
//     Viewer) and each scope's modes, which draw again from the same tables;
//   * the panel draws the tables the frame the Viewer shows carries, when they were counted under
//     the session's settings; a shown panel makes requests ask for every table, a hidden one for
//     none;
//   * #730, never dim: a frame's new tables leave the trace on show untouched, at full strength,
//     until the new image replaces it in one swap; a frame without tables of the current settings
//     changes nothing; every picture painted between two valid traces is one of the two;
//   * the panel's layout never changes while frames change, and a frame with nothing to count
//     draws the empty graticule silently;
//   * mapping runs on a task worker; the hover readout names the value and the count of a cell;
//   * the sections persist per panel area.

#include "await_qt.hpp"
#include <bloom/core/color.hpp>
#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/composition_settings.hpp>
#include <bloom/render/image_types.hpp>
#include <bloom/render/scope_table_layout.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/preview_frame_scopes.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/dropdown.hpp>
#include <bloom/ui/kit/panel_switcher.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/slider.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/scopes_editor.hpp>
#include <bloom/ui/scopes_region_store.hpp>
#include <bloom/ui/scopes_trace_renderer.hpp>
#include <bloom/ui/scopes_view.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QScrollArea>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace document = bloom::document;
namespace render = bloom::render;
namespace runtime = bloom::runtime;
namespace scopes = bloom::ui::scopes;
namespace ui = bloom::ui;
namespace kit = bloom::ui::kit;
using bloom::core::Color4d;
using bloom::runtime::PreparedPreviewFrame;
using bloom::test_support::awaitFailureCount;
using bloom::test_support::awaitQtCondition;
using bloom::ui::PreviewFrameCache;
using bloom::ui::PreviewFrameCacheKey;
using bloom::ui::ScopesEditor;
using namespace std::chrono_literals;

class Expectations final {
  public:
    void expect(const bool condition, const std::string& message,
                const std::source_location location = std::source_location::current()) {
        if (condition)
            return;
        ++failures_;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

constexpr auto kProjectId = document::ProjectId::fromRaw(1);
constexpr auto kCompositionId = document::CompositionId::fromRaw(2);
constexpr auto kSolidNode = document::NodeId::fromRaw(10);
constexpr auto kLayerNode = document::NodeId::fromRaw(11);
constexpr auto kStackNode = document::NodeId::fromRaw(12);
constexpr auto kOutputNode = document::NodeId::fromRaw(13);
constexpr auto kLayer = document::LayerId::fromRaw(20);
constexpr auto kSlot = document::LayerSlotId::fromRaw(30);

[[nodiscard]] document::CompositionFormat format() {
    const auto value = document::CompositionFormat::create(64, 36);
    if (!value.has_value())
        std::abort();
    return *value;
}

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan>
oneSolidPlan(const Color4d color = Color4d{1.0, 0.0, 0.0, 1.0}) {
    const auto compositionFormat = format();
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNode,
                               {document::ParameterId::fromRaw(40), color},
                               {document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1000),
                                static_cast<double>(compositionFormat.width())},
                               {document::ParameterId::fromRaw(kSolidNode.value() * 100 + 1001),
                                static_cast<double>(compositionFormat.height())}});
    operations.emplace_back(runtime::CompiledLayerOutput{
        kLayerNode, kLayer, runtime::OperationIndex::fromRaw(0),
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(41),
                                       document::Vec2d{2.0, 1.0}},
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(43),
                                       document::kDefaultAnchor},
        runtime::CompiledVec2Parameter{document::ParameterId::fromRaw(44), document::kDefaultScale},
        runtime::CompiledScalarParameter{document::ParameterId::fromRaw(45),
                                         document::kDefaultRotationDegrees},
        runtime::CompiledScalarParameter{document::ParameterId::fromRaw(42), 1.0},
        document::ParameterId::fromRaw(46), bloom::core::kDefaultBlendMode});
    operations.emplace_back(runtime::CompiledMerge{
        kStackNode,
        {runtime::CompiledMergeInput{kSlot, kLayer, runtime::OperationIndex::fromRaw(1)}}});
    operations.emplace_back(
        runtime::CompiledCompositionOutput{kOutputNode, runtime::OperationIndex::fromRaw(2)});
    return std::make_shared<const runtime::CompiledCompositionPlan>(
        runtime::CompiledCompositionPlanDefinition{
            document::Revision::fromRaw(7), kProjectId, kCompositionId, compositionFormat,
            std::move(operations), runtime::OperationIndex::fromRaw(3)});
}

[[nodiscard]] std::shared_ptr<const PreparedPreviewFrame>
cpuFrame(const std::shared_ptr<const runtime::CompiledCompositionPlan>& plan,
         const std::uint64_t generation, const std::int64_t time,
         const std::shared_ptr<const runtime::PreviewScopeRequest>& scopes = nullptr) {
    const runtime::CpuCompositionEvaluator evaluator;
    const auto evaluated = evaluator.evaluate(
        plan,
        runtime::EvaluationRequest{.time = bloom::core::RationalTime::fromInteger(time),
                                   .output = plan->output(),
                                   .resolution = runtime::CompositionFormatResolution{},
                                   .quality = runtime::EvaluationQuality::Reference,
                                   .colorIntent = runtime::EvaluationColorIntent::LinearRec709Scene,
                                   .pixelStorageByteLimit = std::size_t{1} << 22},
        {});
    if (evaluated.frame() == nullptr)
        return nullptr;
    const runtime::CpuReferenceDisplayPreparer preparer;
    const auto display =
        preparer.prepare(evaluated.frame(),
                         runtime::ReferenceDisplayPreparationRequest{
                             .intent = runtime::ReferenceDisplayIntent::LinearRec709SceneToSrgb,
                             .aggregatePixelStorageByteLimit = std::size_t{1} << 22,
                             .viewAdjust = {},
                             .displayName = {},
                             .viewName = {},
                             .showLook = true},
                         {});
    if (display.frame() == nullptr)
        return nullptr;
    const auto prepared = PreparedPreviewFrame::create(generation, display.frame());
    if (!prepared.has_value())
        return nullptr;
    if (scopes == nullptr)
        return std::make_shared<const PreparedPreviewFrame>(*prepared);
    // The tables the frame's own preparation counts (the CPU route's worker does this).
    auto tables = runtime::measurePreviewFrameScopesCpu(*scopes, prepared->desiredIdentity(),
                                                        *evaluated.frame(), {});
    return std::make_shared<const PreparedPreviewFrame>(prepared->withScopes(std::move(tables)));
}

constexpr std::array<scopes::Kind, scopes::kKindCount> kKinds{
    scopes::Kind::Waveform, scopes::Kind::Parade, scopes::Kind::Vectorscope,
    scopes::Kind::Histogram, scopes::Kind::Chromaticity};

[[nodiscard]] bool tracesSettled(const ScopesEditor& panel) {
    return panel.renderer() != nullptr && !panel.renderer()->busy();
}

// What every scope paints now.
[[nodiscard]] std::vector<QImage> grabAll(ScopesEditor& panel) {
    std::vector<QImage> pictures;
    for (const auto kind : kKinds)
        pictures.push_back(panel.view(kind)->grab().toImage());
    return pictures;
}

[[nodiscard]] std::vector<int> heights(ScopesEditor& panel) {
    std::vector<int> values;
    for (const auto kind : kKinds)
        values.push_back(panel.view(kind)->height()), values.push_back(panel.view(kind)->width());
    values.push_back(panel.sizeHint().height());
    return values;
}

[[nodiscard]] QAction* action(const ScopesEditor& panel, const char* name) {
    return panel.findChild<QAction*>(QString::fromLatin1(name));
}

void testRegistrationAndChrome(Expectations& e, const ui::ScopesServices& services) {
    ui::EditorRegistry registry;
    e.expect(ui::registerScopesEditor(registry, services) &&
                 !ui::registerScopesEditor(registry, services),
             "Scopes registers once");
    e.expect(registry.editors().size() == 1 && registry.editors().front().id == "bloom.scopes" &&
                 registry.editors().front().displayName == QStringLiteral("Scopes"),
             "the editor is bloom.scopes, named Scopes");
    ui::EditorArea area(registry, "bloom.scopes", QString{});
    auto* picker = area.findChild<kit::KPanelSwitcher*>(QStringLiteral("editorTypePicker"));
    e.expect(picker != nullptr && picker->findData(QStringLiteral("bloom.scopes")) >= 0,
             "the area's switcher offers Scopes");
    auto* panel = area.findChild<ScopesEditor*>(QStringLiteral("scopesEditor"));
    e.expect(panel != nullptr, "the area hosts a ScopesEditor");
    for (const char* name :
         {"scopesSignalMenuButton", "scopesRegionMenuButton", "scopesModesMenuButton"})
        e.expect(area.findChild<QToolButton*>(QString::fromLatin1(name)) != nullptr,
                 std::string("the area's header holds the menu button ") + name);
    if (panel == nullptr)
        return;
    // The only buttons are the panes' collapse arrows (a section's Reset is never shown).
    bool onlyArrows = true;
    for (auto* button : panel->findChildren<kit::KButton*>())
        onlyArrows =
            onlyArrows &&
            (button->objectName() == QStringLiteral("kSectionChevron") ||
             (button->objectName() == QStringLiteral("kSectionReset") && button->isHidden()));
    e.expect(panel->findChildren<kit::KSlider*>().isEmpty() &&
                 panel->findChildren<kit::KDropdown*>().isEmpty() &&
                 panel->findChildren<kit::KCheckBox*>().isEmpty() && onlyArrows,
             "the panel holds no slider, dropdown or check box, and no button but the collapse "
             "arrows");
    const QStringList titles{QStringLiteral("Waveform"), QStringLiteral("Parade"),
                             QStringLiteral("Vectorscope"), QStringLiteral("Histogram"),
                             QStringLiteral("CIE Chromaticity")};
    bool onlyTitles = true;
    for (auto* label : panel->findChildren<QLabel*>())
        onlyTitles = onlyTitles && (label->text().isEmpty() || titles.contains(label->text()));
    for (auto* label : panel->findChildren<kit::KLabel*>())
        onlyTitles = onlyTitles && (label->text().isEmpty() || titles.contains(label->text()));
    e.expect(onlyTitles, "the only text in the panel is the five pane titles");
}

void testHeaderMenus(Expectations& e, ScopesEditor& panel, ui::ScopesRegionStore& store) {
    auto* display = action(panel, "scopesSignalDisplay");
    auto* working = action(panel, "scopesSignalWorking");
    auto* axis = panel.findChild<QMenu*>(QStringLiteral("scopesAxisMaximumMenu"));
    e.expect(display != nullptr && working != nullptr && axis != nullptr && display->isChecked() &&
                 !working->isChecked() && !axis->isEnabled(),
             "Signal: Display is checked and the Working axis maximum is disabled");
    if (working != nullptr)
        working->trigger();
    e.expect(store.signal() == runtime::ScopeSignal::Working && working->isChecked() &&
                 axis->isEnabled(),
             "choosing Working writes the session's signal and enables the axis maximum");
    if (auto* four = action(panel, "scopesAxisMaximum4"))
        four->trigger();
    e.expect(store.workingAxisMaximum() == 4U &&
                 store.settings({kProjectId, kCompositionId}).axisMaximum == 4U,
             "Working Axis Maximum 4 is the Working signal's M");
    if (display != nullptr)
        display->trigger();
    e.expect(store.signal() == runtime::ScopeSignal::Display &&
                 store.settings({kProjectId, kCompositionId}).axisMaximum == 1U,
             "back to Display, whose M is 1");
    auto* ignore = action(panel, "scopesIgnoreTransparent");
    e.expect(ignore != nullptr && ignore->isCheckable() && !ignore->isChecked(),
             "Ignore Transparent Pixels is a check item, off");
    if (ignore != nullptr)
        ignore->trigger();
    e.expect(store.ignoreTransparent(), "it writes the session's setting");
    if (ignore != nullptr)
        ignore->trigger();
    auto* draw = action(panel, "scopesDrawRegion");
    auto* clear = action(panel, "scopesClearRegion");
    e.expect(draw != nullptr && draw->isCheckable() && !draw->isEnabled() && clear != nullptr &&
                 !clear->isEnabled(),
             "Draw and Clear Scope Region are disabled with no Viewer to draw in");
    if (auto* luma = action(panel, "scopesWaveformLuma"))
        luma->trigger();
    e.expect(store.drawing().waveformChannels == scopes::WaveformChannels::Luma &&
                 panel.view(scopes::Kind::Waveform)->currentRequest().waveformChannels ==
                     scopes::WaveformChannels::Luma,
             "Scopes > Waveform > Luma draws the Waveform's luma");
    if (auto* rgb = action(panel, "scopesWaveformRgb"))
        rgb->trigger();
    if (auto* logarithmic = action(panel, "scopesHistogramLogarithmic"))
        logarithmic->trigger();
    e.expect(store.drawing().histogramScale == scopes::HistogramScale::Logarithmic &&
                 panel.view(scopes::Kind::Histogram)->currentRequest().histogramScale ==
                     scopes::HistogramScale::Logarithmic,
             "Scopes > Histogram > Logarithmic draws a logarithmic Histogram");
    if (auto* linear = action(panel, "scopesHistogramLinear"))
        linear->trigger();
    e.expect(store.drawing() == ui::ScopesRegionStore::Drawing{},
             "the modes are back to their defaults");
}

} // namespace

int main(int argc, char** argv) {
    try {
        QApplication application(argc, argv);
        // Section state is stored in a settings folder of this run, never in a user's.
        QTemporaryDir settingsDirectory;
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDirectory.path());
        application.setOrganizationName(QStringLiteral("BloomScopesPanelTests"));
        application.setApplicationName(QStringLiteral("ScopesPanel"));
        kit::installAfterlabTheme(application);
        Expectations e;

        runtime::TaskScheduler scheduler{runtime::TaskSchedulerConfig{.cpuWorkerCount = 2,
                                                                      .blockingIoWorkerCount = 1,
                                                                      .cpuQueueCapacity = 32,
                                                                      .blockingIoQueueCapacity = 4,
                                                                      .terminalHistoryCapacity = 64,
                                                                      .diagnosticsPerTask = 8,
                                                                      .groupRegistryCapacity = 8}};
        ui::TaskUiBridge bridge{scheduler, nullptr, 1ms};
        ui::ScopesRegionStore store;
        ui::ScopesServices services;
        services.scheduler = &scheduler;
        services.bridge = &bridge;
        services.store = &store;

        testRegistrationAndChrome(e, services);

        QWidget host;
        host.resize(520, 1500);
        auto* panel = new ScopesEditor(services, &host);
        auto* layout = new QVBoxLayout(&host);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(panel);
        const ui::ScopesRegionStore::Key key{kProjectId, kCompositionId};
        e.expect(!store.active() && store.request(key) == nullptr,
                 "with no panel shown no request asks for tables");
        host.show();
        e.expect(awaitQtCondition("the panel is shown", [&] { return panel->panelShown(); }),
                 "the panel is shown");
        const auto request = store.request(key);
        e.expect(store.active() && request != nullptr &&
                     request->settings.scopes == render::kScopeTableAll &&
                     request->settings.signal == runtime::ScopeSignal::Display,
                 "a shown panel makes requests ask for every table of the session's settings");

        testHeaderMenus(e, *panel, store);

        // The five panes, in order, open by default.
        const std::array<const char*, 5> titles{"Waveform", "Parade", "Vectorscope", "Histogram",
                                                "CIE Chromaticity"};
        bool titlesOk = true;
        int lastY = -1;
        for (std::size_t index = 0; index < kKinds.size(); ++index) {
            auto* section = panel->section(kKinds[index]);
            titlesOk = titlesOk && section != nullptr &&
                       section->title() == QString::fromLatin1(titles[index]) &&
                       !section->isCollapsed();
            if (section != nullptr) {
                const int y = section->mapTo(&host, QPoint(0, 0)).y();
                titlesOk = titlesOk && y > lastY;
                lastY = y;
            }
        }
        e.expect(titlesOk, "five panes in order, open by default");

        // ---- Frames and their tables ---------------------------------------------------------
        const auto displayRequest = store.request(key);
        const auto red = cpuFrame(oneSolidPlan(Color4d{1.0, 0.0, 0.0, 1.0}), 1, 0, displayRequest);
        const auto green =
            cpuFrame(oneSolidPlan(Color4d{0.0, 0.6, 0.2, 1.0}), 2, 1, displayRequest);
        const auto bare = cpuFrame(oneSolidPlan(Color4d{0.0, 0.0, 1.0, 1.0}), 3, 2, nullptr);
        e.expect(red && green && bare && red->scopes() != nullptr &&
                     red->scopes()->status == runtime::PreviewFrameScopesStatus::Measured &&
                     bare->scopes() == nullptr,
                 "the frames are built, two carrying their tables");
        if (!red || !green || !bare)
            return 1;

        std::vector<int> emptyHeights;
        for (const auto kind : kKinds)
            emptyHeights.push_back(panel->view(kind)->heightForWidth(400));
        panel->showViewerFrame(red);
        e.expect(panel->scopes() == red->scopes(), "the panel takes the frame's tables");
        e.expect(
            awaitQtCondition("the red traces are mapped", [&] { return tracesSettled(*panel); }),
            "the red traces are mapped");
        bool allTraces = true;
        for (const auto kind : kKinds)
            allTraces = allTraces && !panel->view(kind)->traceImage().isNull() &&
                        panel->view(kind)->shownScopes() == red->scopes();
        e.expect(allTraces, "every scope paints a trace of the red frame's tables");
        const auto mainThread = std::this_thread::get_id();
        bool offThread = true;
        for (const auto kind : kKinds) {
            const auto finished = panel->renderer()->finished(kind);
            offThread = offThread && finished != nullptr && finished->workerThread != mainThread;
        }
        e.expect(offThread, "every trace was mapped on a task worker");
        {
            std::vector<int> tracedHeights;
            for (const auto kind : kKinds)
                tracedHeights.push_back(panel->view(kind)->heightForWidth(400));
            e.expect(tracedHeights == emptyHeights,
                     "a scope is as tall with a trace as with the empty graticule");
        }
        // The scopes' sizes, settled: no later frame, trace or empty area changes them.
        const auto layoutBefore = heights(*panel);
        const auto redPictures = grabAll(*panel);
        std::vector<QImage> redTraces;
        for (const auto kind : kKinds)
            redTraces.push_back(panel->view(kind)->traceImage());

        // ---- #730: never dim, never blank -------------------------------------------------------
        panel->showViewerFrame(green);
        e.expect(panel->scopes() == green->scopes(), "the green frame's tables are asked for");
        bool untouched = true;
        for (std::size_t index = 0; index < kKinds.size(); ++index) {
            const auto* view = panel->view(kKinds[index]);
            untouched = untouched && view->traceImage() == redTraces[index] &&
                        view->shownScopes() == red->scopes();
        }
        e.expect(untouched && grabAll(*panel) == redPictures,
                 "until the new traces are mapped every scope paints the red traces as they were: "
                 "nothing dims, fades or blanks");
        // A frame without tables of the current settings changes nothing.
        panel->showViewerFrame(bare);
        e.expect(panel->scopes() == green->scopes() && grabAll(*panel) == redPictures,
                 "a frame without tables leaves the scopes on show untouched");
        // Every picture painted until the swaps are done is the red one or the final green one.
        std::vector<std::vector<QImage>> painted;
        e.expect(awaitQtCondition("the green traces are mapped",
                                  [&] {
                                      painted.push_back(grabAll(*panel));
                                      return tracesSettled(*panel);
                                  }),
                 "the green traces are mapped");
        const auto greenPictures = grabAll(*panel);
        bool oneSwap = true;
        for (const auto& pictures : painted)
            for (std::size_t index = 0; index < kKinds.size(); ++index)
                oneSwap = oneSwap && (pictures[index] == redPictures[index] ||
                                      pictures[index] == greenPictures[index]);
        bool swapped = true;
        for (std::size_t index = 0; index < kKinds.size(); ++index) {
            const auto* view = panel->view(kKinds[index]);
            swapped = swapped && view->shownScopes() == green->scopes() &&
                      !view->traceImage().isNull() && view->traceImage() != redTraces[index];
        }
        e.expect(oneSwap, "no dimmed or empty trace was painted between the red and the green one");
        e.expect(swapped, "every scope swapped to the green frame's trace");
        {
            const auto layoutAfter = heights(*panel);
            std::string sizes;
            for (std::size_t index = 0; index < layoutAfter.size(); ++index)
                sizes += std::to_string(layoutBefore[index]) + "->" +
                         std::to_string(layoutAfter[index]) + " ";
            e.expect(layoutAfter == layoutBefore,
                     "the layout did not change while frames changed: " + sizes);
        }

        // A change of what is counted keeps the traces until a frame of the new settings arrives.
        store.setSignal(runtime::ScopeSignal::Working);
        panel->showViewerFrame(green);
        e.expect(panel->scopes() == green->scopes() && grabAll(*panel) == greenPictures,
                 "a frame of the old signal's tables leaves the traces on show");
        const auto working =
            cpuFrame(oneSolidPlan(Color4d{0.0, 0.6, 0.2, 1.0}), 4, 1, store.request(key));
        e.expect(working && working->scopes() != nullptr &&
                     working->scopes()->settings.signal == runtime::ScopeSignal::Working,
                 "a frame counted under the Working signal");
        if (working) {
            panel->showViewerFrame(working);
            e.expect(awaitQtCondition("the Working traces are mapped",
                                      [&] { return tracesSettled(*panel); }) &&
                         panel->view(scopes::Kind::Waveform)->shownScopes() == working->scopes(),
                     "the Working frame's tables replace them");
        }
        store.setSignal(runtime::ScopeSignal::Display);

        // ---- Nothing to count: the empty graticule, silently ---------------------------------
        store.setRegion(key, runtime::ScopeRegion{.x = 500, .y = 0, .width = 10, .height = 10});
        const auto outside =
            cpuFrame(oneSolidPlan(Color4d{1.0, 1.0, 1.0, 1.0}), 5, 3, store.request(key));
        e.expect(outside && outside->scopes() != nullptr &&
                     outside->scopes()->status == runtime::PreviewFrameScopesStatus::EmptyArea,
                 "a region outside the picture has nothing to count");
        if (outside) {
            panel->showViewerFrame(outside);
            bool empty = true;
            for (const auto kind : kKinds)
                empty = empty && panel->view(kind)->traceImage().isNull();
            e.expect(empty, "an empty area draws the empty graticule");
            e.expect(heights(*panel) == layoutBefore, "and the layout does not change");
        }
        store.clearRegion(key);
        panel->showViewerFrame(red);
        e.expect(awaitQtCondition("the red traces are mapped again",
                                  [&] { return tracesSettled(*panel); }),
                 "the red traces are mapped again");

        // ---- The hover readout -----------------------------------------------------------------
        {
            auto* histogram = panel->view(scopes::Kind::Histogram);
            const auto plot = histogram->plotRect();
            const auto text = histogram->readoutAt(QPointF(plot.left() + 1, plot.center().y()));
            e.expect(text.contains(QStringLiteral("Code")) &&
                         text.contains(QStringLiteral("Count")),
                     "the Histogram's readout names the code and the count: " + text.toStdString());
            e.expect(histogram->readoutAt(QPointF(-5, -5)).isEmpty(),
                     "outside the plot there is no readout");
        }

        // ---- Sections persist per panel area; a hidden panel asks for nothing ----------------
        panel->section(scopes::Kind::Parade)->setCollapsed(true);
        e.expect(store.request(key) != nullptr &&
                     store.request(key)->settings.scopes == render::kScopeTableAll,
                 "a collapsed pane still leaves every table asked for");
        {
            ScopesEditor second(ui::ScopesServices{});
            second.show();
            e.expect(second.section(scopes::Kind::Parade)->isCollapsed() &&
                         !second.section(scopes::Kind::Waveform)->isCollapsed(),
                     "collapsed state is persisted");
        }
        panel->section(scopes::Kind::Parade)->setCollapsed(false);
        host.hide();
        e.expect(awaitQtCondition("the panel is hidden", [&] { return !panel->panelShown(); }) &&
                     !store.active() && store.request(key) == nullptr,
                 "a hidden panel makes requests ask for no tables");

        delete panel;
        e.expect(awaitFailureCount() == 0, "every wait finished");
        if (e.failures() != 0)
            return 1;
        std::cout << "PASS: the Scopes panel\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
