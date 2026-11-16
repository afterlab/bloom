// #724, #730, #733 Scopes: the Scopes panel over genuine CPU reference frames; no device and no
// Viewer.
//
// Asserted:
//   * `bloom.scopes`, "Scopes", is registered like every editor; the area hosts its header menus
//     (Signal, Region, Scopes) and the panel holds nothing but its five collapsible panes: no
//     status, warning or other text, no slider, dropdown, check box or button;
//   * the header menus write the session's settings: Signal (Display, Working, the Working axis
//     maximum, Ignore Transparent Pixels), Region (Draw and Clear Scope Region, disabled with no
//     Viewer) and each scope's modes, which draw again from the same tables;
//   * #733, the Display signal: the panel hands the frame the Viewer shows to the runtime's
//     shown-frame measure, which counts it on a worker, never the interface thread; no request
//     asks a frame's preparation for Display tables, and the same frame under the same settings
//     is asked once;
//   * #733, testSettingsChangeRendersNothing: Ignore Transparent Pixels, a region and the signal
//     each recount the frame on show with one measure and ask no frame's preparation for
//     anything; the Working axis maximum is not a Display setting and counts nothing;
//   * #733, pane size: the session asks for the tables the open panes draw, at the smallest size
//     not below their device pixels; the panel's tables are of that size; a collapsed pane asks
//     for no table, and a pane resize or collapse counts the frame on show again and nothing
//     else;
//   * the Working signal: the panel draws the tables the frame carries, when they were counted
//     under the session's settings; a shown panel then makes requests ask for every table, a
//     hidden one for none;
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

// What the runtime's CPU count of a shown frame ran on.
struct MeasureThreads final {
    std::thread::id interfaceThread = std::this_thread::get_id();
    std::atomic<int> counted = 0;
    std::atomic<int> onInterfaceThread = 0;
};

// Shows `frame` and waits for the tables the panel is handed for it.
[[nodiscard]] std::shared_ptr<const runtime::PreviewFrameScopes>
countShown(Expectations& e, ScopesEditor& panel, const ui::PreparedPreviewFrameHandle& frame,
           const std::string& what) {
    const auto previous = panel.scopes();
    panel.showViewerFrame(frame);
    e.expect(awaitQtCondition(what,
                              [&] {
                                  return !panel.measuring() && panel.scopes() != nullptr &&
                                         panel.scopes() != previous;
                              }),
             what);
    return panel.scopes();
}

// #733 (R8): a settings change on the Display signal counts the frame on show again, with one
// measure, off the interface thread, and asks no frame's preparation for anything.
void testSettingsChangeRendersNothing(Expectations& e, ScopesEditor& panel,
                                      ui::ScopesRegionStore& store,
                                      const ui::ScopesRegionStore::Key& key,
                                      const ui::PreparedPreviewFrameHandle& frame,
                                      const MeasureThreads& threads) {
    const auto pixels = static_cast<std::uint64_t>(format().width()) * format().height();
    const auto changed = [&](const std::string& what, const auto& change, const auto& holds) {
        const auto before = panel.measureCounters();
        const int countedBefore = threads.counted.load();
        change();
        e.expect(store.request(key) == nullptr,
                 what + ": no request asks a frame's preparation for tables");
        const auto scopes = countShown(e, panel, frame, what + ": the frame on show is counted");
        const auto after = panel.measureCounters();
        e.expect(after.requested == before.requested + 1 &&
                     after.cpuMeasures + after.unmeasured ==
                         before.cpuMeasures + before.unmeasured + 1 &&
                     threads.counted.load() == countedBefore + 1,
                 what + ": exactly one measure ran");
        e.expect(scopes != nullptr && scopes->settings == store.settings(key) &&
                     scopes->settings.signal == runtime::ScopeSignal::Display && holds(*scopes),
                 what + ": the panel is handed tables of the new settings");
    };
    const auto measured = [](const runtime::PreviewFrameScopes& scopes) {
        return scopes.status == runtime::PreviewFrameScopesStatus::Measured;
    };
    changed(
        "Ignore Transparent Pixels", [&] { store.setIgnoreTransparent(true); },
        [&](const runtime::PreviewFrameScopes& scopes) {
            // The solid sits two pixels right and one down: the uncovered edge is transparent.
            return measured(scopes) && scopes.settings.ignoreTransparent &&
                   scopes.tables.counted() > 0U && scopes.tables.counted() < pixels;
        });
    changed(
        "Ignore Transparent Pixels off", [&] { store.setIgnoreTransparent(false); },
        [&](const runtime::PreviewFrameScopes& scopes) {
            return measured(scopes) && scopes.tables.counted() == pixels;
        });
    changed(
        "a region",
        [&] {
            store.setRegion(key, runtime::ScopeRegion{.x = 0, .y = 0, .width = 32, .height = 36});
        },
        [&](const runtime::PreviewFrameScopes& scopes) {
            return measured(scopes) && scopes.tables.counted() == 32U * 36U &&
                   scopes.area.width == 32U && scopes.area.height == 36U;
        });
    changed(
        "the region cleared", [&] { store.clearRegion(key); },
        [&](const runtime::PreviewFrameScopes& scopes) {
            return measured(scopes) && scopes.tables.counted() == pixels;
        });
    changed(
        "the signal back to Display",
        [&] {
            store.setSignal(runtime::ScopeSignal::Working);
            // On the Working signal the panel asks the measure for nothing.
            const auto asked = panel.measureCounters().requested;
            panel.showViewerFrame(frame);
            e.expect(panel.measureCounters().requested == asked,
                     "the Working signal asks the shown-frame measure for nothing");
            store.setSignal(runtime::ScopeSignal::Display);
        },
        [&](const runtime::PreviewFrameScopes& scopes) {
            return measured(scopes) && scopes.tables.counted() == pixels;
        });
    // The Working axis maximum is not a setting of the Display signal.
    {
        const auto before = panel.measureCounters();
        const auto shown = panel.scopes();
        store.setWorkingAxisMaximum(2);
        panel.showViewerFrame(frame);
        e.expect(panel.measureCounters().requested == before.requested && panel.scopes() == shown &&
                     store.request(key) == nullptr,
                 "the Working axis maximum counts nothing on the Display signal");
        store.setWorkingAxisMaximum(1);
    }
    e.expect(threads.onInterfaceThread.load() == 0 && threads.counted.load() > 0,
             "no frame was counted on the interface thread");
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
        MeasureThreads measureThreads;
        services.measureObserver = [&measureThreads] {
            ++measureThreads.counted;
            if (std::this_thread::get_id() == measureThreads.interfaceThread)
                ++measureThreads.onInterfaceThread;
        };

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
        e.expect(store.active() && store.signal() == runtime::ScopeSignal::Display &&
                     store.request(key) == nullptr,
                 "a shown panel on the Display signal asks no frame's preparation for tables");
        store.setSignal(runtime::ScopeSignal::Working);
        {
            const auto request = store.request(key);
            e.expect(request != nullptr && request->settings.scopes == render::kScopeTableAll &&
                         request->settings.signal == runtime::ScopeSignal::Working &&
                         request->reduction == store.tableReduction(),
                     "on the Working signal requests ask for every table of the session's "
                     "settings, at the size the panels draw");
        }
        store.setSignal(runtime::ScopeSignal::Display);

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

        // ---- #733: the tables are asked at the size the open panes draw -----------------------
        const auto panesOf = [&](const std::vector<scopes::Kind>& open) {
            runtime::scopes::ScopePaneCells cells;
            for (const auto kind : open) {
                const auto request = panel->view(kind)->currentRequest();
                cells = runtime::scopes::mergeScopePaneCells(
                    cells, runtime::scopes::scopePaneCells(
                               kind, {request.size.width(), request.size.height()},
                               request.gapPixels));
            }
            return cells;
        };
        const std::vector<scopes::Kind> everyKind(kKinds.begin(), kKinds.end());
        {
            const auto cells = panesOf(everyKind);
            const auto reduction = runtime::scopes::scopeTableReductionFor(cells);
            e.expect(cells.waveformColumns > 0U && cells.waveformLevels > 0U &&
                         cells.vectorscope > 0U && cells.chromaticity > 0U && cells.histogram,
                     "every open pane draws a trace of some size");
            e.expect(store.tables() == render::kScopeTableAll &&
                         store.tableReduction() == reduction &&
                         store.settings(key).scopes == render::kScopeTableAll,
                     "the session asks for every table at the size the open panes draw");
            e.expect(reduction.waveformLevelShift > 0U &&
                         (render::kScopeWaveformLevels >> reduction.waveformLevelShift) >=
                             cells.waveformLevels &&
                         (render::kScopeWaveformLevels >> (reduction.waveformLevelShift + 1U)) <
                             cells.waveformLevels,
                     "the Waveform's levels are the smallest table not below the pane");
        }

        // ---- Frames and their tables ---------------------------------------------------------
        // #733: on the Display signal the panel hands the frame on show to the runtime's
        // shown-frame measure, which counts its stored cells off this thread.
        const auto red = cpuFrame(oneSolidPlan(Color4d{1.0, 0.0, 0.0, 1.0}), 1, 0);
        const auto green = cpuFrame(oneSolidPlan(Color4d{0.0, 0.6, 0.2, 1.0}), 2, 1);
        e.expect(red && green && red->scopes() == nullptr && green->scopes() == nullptr,
                 "the frames are built; they carry no tables");
        if (!red || !green)
            return 1;
        const auto pixels = static_cast<std::uint64_t>(format().width()) * format().height();

        std::vector<int> emptyHeights;
        for (const auto kind : kKinds)
            emptyHeights.push_back(panel->view(kind)->heightForWidth(400));
        const auto countersBefore = panel->measureCounters();
        panel->showViewerFrame(red);
        e.expect(panel->scopes() == nullptr && panel->measureCounters().requested ==
                                                   countersBefore.requested + 1,
                 "the shown frame is handed to the measure and nothing is counted on this thread");
        e.expect(awaitQtCondition("the red frame is counted",
                                  [&] { return panel->scopes() != nullptr; }),
                 "the red frame is counted");
        const auto redScopes = panel->scopes();
        e.expect(redScopes != nullptr &&
                     redScopes->status == runtime::PreviewFrameScopesStatus::Measured &&
                     redScopes->settings == store.settings(key) &&
                     redScopes->tables.counted() == pixels &&
                     redScopes->tables.route() == runtime::ScopeRoute::Cpu &&
                     redScopes->color != nullptr,
                 "the panel takes Display tables of every pixel of the shown frame");
        e.expect(measureThreads.counted.load() == 1 && measureThreads.onInterfaceThread.load() == 0,
                 "the frame was counted on a worker, not the interface thread");
        {
            const auto asked = store.tableReduction();
            const auto counted =
                render::scopeReductionForWidth(asked, static_cast<std::uint32_t>(format().width()));
            const auto full =
                render::scopeTableLayout(render::kScopeTableAll,
                                         static_cast<std::uint32_t>(format().width()))
                    .byteCount();
            e.expect(redScopes != nullptr && redScopes->reduction == asked &&
                         redScopes->tables.layout().reduction == counted &&
                         redScopes->tables.layout().byteCount() < full,
                     "the tables are counted at the size the panes draw, smaller than the full "
                     "tables: " +
                         std::to_string(redScopes != nullptr
                                            ? redScopes->tables.layout().byteCount()
                                            : 0U) +
                         " of " + std::to_string(full) + " bytes");
        }
        panel->showViewerFrame(red);
        e.expect(panel->measureCounters().requested == countersBefore.requested + 1,
                 "the same frame under the same settings is not asked for twice");
        e.expect(
            awaitQtCondition("the red traces are mapped", [&] { return tracesSettled(*panel); }),
            "the red traces are mapped");
        bool allTraces = true;
        for (const auto kind : kKinds)
            allTraces = allTraces && !panel->view(kind)->traceImage().isNull() &&
                        panel->view(kind)->shownScopes() == redScopes;
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
        bool untouched = panel->scopes() == redScopes;
        for (std::size_t index = 0; index < kKinds.size(); ++index) {
            const auto* view = panel->view(kKinds[index]);
            untouched = untouched && view->traceImage() == redTraces[index] &&
                        view->shownScopes() == redScopes;
        }
        e.expect(untouched && grabAll(*panel) == redPictures,
                 "until the new frame is counted and its traces are mapped every scope paints the "
                 "red traces as they were: nothing dims, fades or blanks");
        // Every picture painted until the swaps are done is the red one or the final green one.
        std::vector<std::vector<QImage>> painted;
        e.expect(awaitQtCondition("the green traces are mapped",
                                  [&] {
                                      painted.push_back(grabAll(*panel));
                                      return panel->scopes() != redScopes && !panel->measuring() &&
                                             tracesSettled(*panel);
                                  }),
                 "the green traces are mapped");
        const auto greenScopes = panel->scopes();
        const auto greenPictures = grabAll(*panel);
        bool oneSwap = true;
        for (const auto& pictures : painted)
            for (std::size_t index = 0; index < kKinds.size(); ++index)
                oneSwap = oneSwap && (pictures[index] == redPictures[index] ||
                                      pictures[index] == greenPictures[index]);
        bool swapped = greenScopes != nullptr && greenScopes != redScopes;
        for (std::size_t index = 0; index < kKinds.size(); ++index) {
            const auto* view = panel->view(kKinds[index]);
            swapped = swapped && view->shownScopes() == greenScopes &&
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

        // ---- The Working signal: the tables the frame carries ----------------------------------
        // A change of what is counted keeps the traces until tables of the new settings arrive.
        store.setSignal(runtime::ScopeSignal::Working);
        {
            const auto asked = panel->measureCounters().requested;
            panel->showViewerFrame(green);
            e.expect(panel->scopes() == greenScopes && grabAll(*panel) == greenPictures &&
                         panel->measureCounters().requested == asked,
                     "a frame without Working tables leaves the traces on show and asks the "
                     "shown-frame measure for nothing");
        }
        const auto working =
            cpuFrame(oneSolidPlan(Color4d{0.0, 0.6, 0.2, 1.0}), 4, 1, store.request(key));
        e.expect(working && working->scopes() != nullptr &&
                     working->scopes()->settings.signal == runtime::ScopeSignal::Working,
                 "a frame counted under the Working signal");
        if (working) {
            panel->showViewerFrame(working);
            e.expect(panel->scopes() == working->scopes(),
                     "the panel takes the Working tables the frame carries");
            e.expect(awaitQtCondition("the Working traces are mapped",
                                      [&] { return tracesSettled(*panel); }) &&
                         panel->view(scopes::Kind::Waveform)->shownScopes() == working->scopes(),
                     "the Working frame's tables replace them");
            // Tables of other Working settings change nothing on show.
            store.setWorkingAxisMaximum(4);
            panel->showViewerFrame(working);
            e.expect(panel->scopes() == working->scopes(),
                     "a frame whose tables are of other settings leaves the traces on show");
            store.setWorkingAxisMaximum(1);
        }
        store.setSignal(runtime::ScopeSignal::Display);

        // ---- Nothing to count: the empty graticule, silently ---------------------------------
        store.setRegion(key, runtime::ScopeRegion{.x = 500, .y = 0, .width = 10, .height = 10});
        const auto outside = cpuFrame(oneSolidPlan(Color4d{1.0, 1.0, 1.0, 1.0}), 5, 3);
        e.expect(outside != nullptr, "a frame for a region outside the picture");
        if (outside) {
            const auto scopes =
                countShown(e, *panel, outside, "a region outside the picture is answered");
            e.expect(scopes != nullptr &&
                         scopes->status == runtime::PreviewFrameScopesStatus::EmptyArea,
                     "a region outside the picture has nothing to count");
            bool empty = true;
            for (const auto kind : kKinds)
                empty = empty && panel->view(kind)->traceImage().isNull();
            e.expect(empty, "an empty area draws the empty graticule");
            e.expect(heights(*panel) == layoutBefore, "and the layout does not change");
        }
        store.clearRegion(key);
        {
            const auto scopes = countShown(e, *panel, red, "the red frame is counted again");
            e.expect(scopes != nullptr &&
                         scopes->status == runtime::PreviewFrameScopesStatus::Measured,
                     "the red frame's tables are back");
        }
        e.expect(awaitQtCondition("the red traces are mapped again",
                                  [&] { return tracesSettled(*panel); }),
                 "the red traces are mapped again");

        // ---- #733: a collapsed pane asks for no table; a pane change counts the shown frame ----
        {
            const auto before = panel->measureCounters();
            panel->section(scopes::Kind::Vectorscope)->setCollapsed(true);
            e.expect(store.tables() ==
                             (render::kScopeTableAll & ~render::kScopeTableVectorscope) &&
                         store.request(key) == nullptr,
                     "a collapsed pane asks for no table and no frame is asked for anything");
            const auto scopes = countShown(e, *panel, red, "the shown frame is counted again");
            e.expect(scopes != nullptr &&
                         scopes->status == runtime::PreviewFrameScopesStatus::Measured &&
                         !scopes->tables.layout().has(render::kScopeTableVectorscope) &&
                         scopes->tables.layout().has(render::kScopeTableWaveform) &&
                         panel->measureCounters().requested == before.requested + 1,
                     "one measure of the frame on show, without the collapsed pane's table");
            panel->section(scopes::Kind::Vectorscope)->setCollapsed(false);
            e.expect(awaitQtCondition("the pane is open again",
                                      [&] { return store.tables() == render::kScopeTableAll; }),
                     "an opened pane asks for its table again");
            const auto again = countShown(e, *panel, red, "the shown frame is counted once more");
            e.expect(again != nullptr &&
                         again->tables.layout().has(render::kScopeTableVectorscope),
                     "the opened pane's table is counted");
            e.expect(awaitQtCondition("the traces settle after the pane changes",
                                      [&] {
                                          return tracesSettled(*panel) &&
                                                 !panel->view(scopes::Kind::Vectorscope)
                                                      ->traceImage()
                                                      .isNull();
                                      }),
                     "the opened pane draws its trace");
        }
        {
            // A narrower panel: smaller panes, smaller tables, one measure, no frame request.
            const auto wide = store.tableReduction();
            const auto before = panel->measureCounters();
            host.resize(260, 1500);
            e.expect(awaitQtCondition("the panes take the narrower size",
                                      [&] {
                                          return store.tableReduction() ==
                                                     runtime::scopes::scopeTableReductionFor(
                                                         panesOf(everyKind)) &&
                                                 store.tableReduction() != wide;
                                      }),
                     "narrower panes ask for smaller tables");
            e.expect(store.request(key) == nullptr, "a pane resize asks no frame for anything");
            const auto scopes = countShown(e, *panel, red, "the shown frame is counted smaller");
            e.expect(scopes != nullptr && scopes->reduction == store.tableReduction() &&
                         panel->measureCounters().requested == before.requested + 1,
                     "one measure of the frame on show at the new size");
            host.resize(520, 1500);
            e.expect(awaitQtCondition("the panes take the first size again",
                                      [&] { return store.tableReduction() == wide; }),
                     "the first size asks for the first tables");
            (void)countShown(e, *panel, red, "the shown frame is counted at the first size");
            e.expect(awaitQtCondition("the traces settle after the resize",
                                      [&] { return tracesSettled(*panel); }),
                     "the traces settle after the resize");
        }

        // ---- #733: a settings change renders nothing ------------------------------------------
        testSettingsChangeRendersNothing(e, *panel, store, key, red, measureThreads);
        e.expect(awaitQtCondition("the traces settle after the settings changes",
                                  [&] { return tracesSettled(*panel); }),
                 "the traces settle after the settings changes");

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

            // #733: values outside the axis are named from the shown frame's own counters. The
            // Display tables are counted from the frame's stored codes, which hold no value
            // outside the axis.
            const auto bright = cpuFrame(oneSolidPlan(Color4d{2.0, 0.25, 0.0, 1.0}), 60, 9);
            const auto counters = bright != nullptr ? bright->displayCounters() : std::nullopt;
            e.expect(counters.has_value() && counters->over[0] > 0 && counters->over[1] == 0 &&
                         counters->under[0] == 0,
                     "a frame brighter than the display's white carries its over count");
            const auto brightScopes = countShown(e, *panel, bright, "the bright frame is counted");
            e.expect(awaitQtCondition("the bright frame's histogram is on show",
                                      [&] {
                                          return tracesSettled(*panel) &&
                                                 histogram->shownScopes() == brightScopes;
                                      }),
                     "the bright frame's histogram is on show");
            e.expect(brightScopes != nullptr && brightScopes->frameCounters == counters &&
                         brightScopes->tables.histogram(0, render::kScopeCodeOver) == 0,
                     "the tables are handed the frame's counters; their own end cells are empty");
            const auto shownPlot = histogram->plotRect();
            QString redEnd;
            QString redStart;
            QString greenEnd;
            for (qreal y = shownPlot.top() + 0.5; y < shownPlot.bottom(); y += 1.0) {
                const auto atEnd = histogram->readoutAt(QPointF(shownPlot.right(), y));
                if (redEnd.isEmpty() && atEnd.startsWith(QStringLiteral("R "))) {
                    redEnd = atEnd;
                    redStart = histogram->readoutAt(QPointF(shownPlot.left(), y));
                }
                if (greenEnd.isEmpty() && atEnd.startsWith(QStringLiteral("G ")))
                    greenEnd = atEnd;
            }
            const auto overText =
                QStringLiteral("Over %1").arg(counters.has_value() ? counters->over[0] : 0U);
            e.expect(redEnd.contains(QStringLiteral("Code 1023")) && redEnd.endsWith(overText),
                     "the red row's last code names the frame's over count: " +
                         redEnd.toStdString());
            e.expect(redStart.contains(QStringLiteral("Code 0 ")) &&
                         !redStart.contains(QStringLiteral("Under")) &&
                         greenEnd.contains(QStringLiteral("Code 1023")) &&
                         !greenEnd.contains(QStringLiteral("Over")),
                     "a channel with nothing outside the axis names nothing: " +
                         redStart.toStdString() + " / " + greenEnd.toStdString());
        }

        // ---- Sections persist per panel area; a hidden panel asks for nothing ----------------
        panel->section(scopes::Kind::Parade)->setCollapsed(true);
        store.setSignal(runtime::ScopeSignal::Working);
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
        store.setSignal(runtime::ScopeSignal::Display);
        {
            // A hidden panel asks the shown-frame measure for nothing either.
            const auto asked = panel->measureCounters().requested;
            panel->showViewerFrame(green);
            e.expect(panel->measureCounters().requested == asked && !panel->measuring(),
                     "a hidden panel counts nothing");
        }

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
