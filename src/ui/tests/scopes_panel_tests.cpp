// #724 Scopes: the Scopes panel over genuine CPU reference frames; no device and no Viewer.
//
// Asserted:
//   * `bloom.scopes`, "Scopes", is registered like every editor and shows in the area's switcher;
//   * the five sections, in order, collapse and persist per panel area; a collapsed section's
//     scope is left out of the measure (Waveform and Parade share a table) and a hidden panel
//     measures nothing;
//   * the status line states what was measured and why nothing is shown, and "Behind" names the
//     measured frame while another frame's tables are on show;
//   * a changed signal, axis maximum, Ignore Transparent Pixels or region measures again, and a
//     Brightness change maps again and does not;
//   * the scope region seam: set, clear, arm, and the signals the Viewer's gesture will use;
//   * no measuring and no mapping runs on the interface thread; a scope with no frame draws an
//     empty graticule; the hover readout names the value and the count of a cell;
//   * during a RAM preview fill, with the panel shown, the frames the cache retains are measured.
// The status line's every item, warning and reason is asserted as a pure function here too.

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
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
#include <bloom/ui/kit/button.hpp>
#include <bloom/ui/kit/controls.hpp>
#include <bloom/ui/kit/panel_switcher.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/switch_control.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/scopes_editor.hpp>
#include <bloom/ui/scopes_status.hpp>
#include <bloom/ui/scopes_trace_renderer.hpp>
#include <bloom/ui/scopes_view.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <QVBoxLayout>

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
using bloom::ui::PreviewFrameCache;
using bloom::ui::PreviewFrameCacheKey;
using bloom::ui::ScopesEditor;
using bloom::ui::ScopesReading;
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

template <typename Predicate> [[nodiscard]] bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 60000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return predicate();
}

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

[[nodiscard]] std::shared_ptr<const runtime::CompiledCompositionPlan> oneSolidPlan() {
    const auto compositionFormat = format();
    std::vector<runtime::CompiledOperation> operations;
    operations.emplace_back(
        runtime::CompiledSolid{kSolidNode,
                               {document::ParameterId::fromRaw(40), Color4d{1.0, 0.0, 0.0, 1.0}},
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
         const std::uint64_t generation, const std::int64_t time) {
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
    return std::make_shared<const PreparedPreviewFrame>(*prepared);
}

using render::Rgba32f;

[[nodiscard]] Rgba32f px(const float r, const float g, const float b, const float a = 1.0F) {
    const auto result = Rgba32f::fromPremultiplied(r, g, b, a);
    if (!result)
        throw std::logic_error("invalid fixture pixel");
    return *result.value();
}

constexpr std::array<scopes::Kind, scopes::kKindCount> kKinds{
    scopes::Kind::Waveform, scopes::Kind::Parade, scopes::Kind::Vectorscope,
    scopes::Kind::Histogram, scopes::Kind::Chromaticity};

// ---- The status line ------------------------------------------------------------------------

// The frame every status fixture is cloned from: a real source, so its plan names the frame rate.
std::shared_ptr<const PreparedPreviewFrame> gBaseFrame;

[[nodiscard]] ui::PreviewScopeSource sourceAt(const std::int64_t seconds) {
    ui::PreviewScopeSource source{gBaseFrame->desiredIdentity(), gBaseFrame->processIdentity(),
                                  runtime::PreviewDisplayProvider::CpuReference};
    source.identity.time = bloom::core::RationalTime::fromInteger(seconds);
    source.identity.displayName = "sRGB";
    source.identity.viewName = "Standard";
    return source;
}

[[nodiscard]] ui::PreviewScopeSidecarHandle
sidecarFor(const runtime::ScopeMeasureSettings& settings, const runtime::ScopeRoute route) {
    std::vector<Rgba32f> pixels(4, px(0.5F, 0.5F, 0.5F));
    auto sidecar = std::make_shared<ui::PreviewScopeSidecar>();
    sidecar->identity.settings = settings;
    runtime::ScopeMeasureInput input;
    input.signal = pixels;
    input.width = 4;
    input.height = 1;
    input.areaWidth = 4;
    input.areaHeight = 1;
    input.scopes = render::kScopeTableHistogram;
    input.luma = runtime::scopeLuma(0.2126F, 0.7152F, 0.0722F);
    sidecar->tables = runtime::ScopeTables::pack(*runtime::measureScopesCpu(input), route);
    sidecar->area = {0, 0, 4, 1};
    return sidecar;
}

[[nodiscard]] ui::ScopesStatusInput readyInput() {
    ui::ScopesStatusInput input;
    input.hasViewer = true;
    input.settings.scopes = render::kScopeTableHistogram;
    input.compositionName = [](bloom::document::ProjectId, bloom::document::CompositionId) {
        return QStringLiteral("Titles");
    };
    input.reading.state = ui::ScopesReading::State::Ready;
    input.reading.shown = sourceAt(2);
    input.reading.measured = sourceAt(2);
    input.reading.scopes = sidecarFor(input.settings, runtime::ScopeRoute::Cpu);
    return input;
}

void testStatusItems(Expectations& e) {
    auto input = readyInput();
    auto status = ui::describeScopesStatus(input);
    const auto has = [&](const QString& text) { return status.line.contains(text); };
    e.expect(status.measured && has("Titles") && has("frame 48") && has("2.000 s"),
             "the composition and the frame time measured: " + status.line.toStdString());
    e.expect(has("Display sRGB / Standard"), "the display and view");
    e.expect(has("Resolution Full"), "the resolution");
    e.expect(has("CPU") && !has("GPU"), "the route");
    e.expect(has("4 pixels counted"), "the pixels counted");
    e.expect(!has("Region") && status.warnings.isEmpty() && status.behind.isEmpty(),
             "no region, no warning and no Behind on a plain frame");

    // The route and its reason.
    input.reading.scopes = sidecarFor(input.settings, runtime::ScopeRoute::Gpu);
    status = ui::describeScopesStatus(input);
    e.expect(status.line.contains("GPU") && !status.line.contains("CPU"),
             "a device frame says GPU");
    auto withNote = std::make_shared<ui::PreviewScopeSidecar>(*input.reading.scopes);
    withNote->note = "no device";
    input.reading.scopes = withNote;
    status = ui::describeScopesStatus(input);
    e.expect(status.line.contains("no device"), "the reason the device did not count is kept");

    // The measured rectangle: a region and a Viewer region of interest.
    input = readyInput();
    input.settings.region = runtime::ScopeRegion{4, 5, 6, 7};
    auto regioned = std::make_shared<ui::PreviewScopeSidecar>(*input.reading.scopes);
    regioned->identity.settings.region = input.settings.region;
    regioned->area = {4, 5, 6, 7};
    input.reading.scopes = regioned;
    status = ui::describeScopesStatus(input);
    e.expect(status.line.contains("Region 4, 5, 6 x 7"), "the region's rectangle");
    input = readyInput();
    const auto window = render::ImageWindow::create(1, 2, 3, 4);
    input.reading.measured->identity.roi = *window.value();
    status = ui::describeScopesStatus(input);
    e.expect(status.line.contains("Region of interest"), "the Viewer's region of interest");

    // The Working signal names the working space, not a display.
    input = readyInput();
    auto working = runtime::ScopeMeasureSettings{};
    working.signal = runtime::ScopeSignal::Working;
    input.settings = working;
    input.reading.scopes = sidecarFor(working, runtime::ScopeRoute::Cpu);
    status = ui::describeScopesStatus(input);
    e.expect(status.line.contains("Working space") && !status.line.contains("Display"),
             "the Working signal says so");

    // A proxy resolution is named.
    input = readyInput();
    status = ui::describeScopesStatus(input);
    e.expect(status.line.contains("Full"), "an unreduced frame is Full");
}

void testStatusWarnings(Expectations& e) {
    auto input = readyInput();
    input.viewAdjust = runtime::ViewAdjust{.exposure = 1.5, .gamma = 1.0};
    auto status = ui::describeScopesStatus(input);
    e.expect(status.warnings.size() == 1 && status.warnings[0].contains("Exposure"),
             "exposure that is not neutral warns");
    input.viewAdjust = runtime::ViewAdjust{.exposure = 0.0, .gamma = 2.2};
    status = ui::describeScopesStatus(input);
    e.expect(status.warnings.size() == 1 && status.warnings[0].contains("Gamma"),
             "gamma that is not neutral warns");
    input.viewAdjust = {};
    input.reading.shown->identity.showLook = false;
    status = ui::describeScopesStatus(input);
    e.expect(status.warnings.size() == 1 && status.warnings[0].contains("Look off"),
             "a look that is off warns");
    input.reading.shown->identity.showLook = true;
    input.reading.shown->identity.channelInspection =
        runtime::ChannelInspection{bloom::document::NodeId::fromRaw(1), "out", {"R"}};
    status = ui::describeScopesStatus(input);
    e.expect(status.warnings.size() == 1 && status.warnings[0].contains("Channel"),
             "a channel shown warns");
    input.reading.shown->identity.channelInspection.reset();
    input.reading.shown->identity.isolation = {bloom::document::LayerId::fromRaw(3)};
    status = ui::describeScopesStatus(input);
    e.expect(status.warnings.size() == 1 && status.warnings[0].contains("Isolation"),
             "isolation warns");
    // Every setting at once names each.
    input.viewAdjust = runtime::ViewAdjust{.exposure = -1.0, .gamma = 1.8};
    input.reading.shown->identity.showLook = false;
    input.reading.shown->identity.channelInspection =
        runtime::ChannelInspection{bloom::document::NodeId::fromRaw(1), "out", {"A"}};
    status = ui::describeScopesStatus(input);
    e.expect(status.warnings.size() == 5, "every setting that differs from the export is named");
    // The Working signal is taken before the display: exposure, gamma and the look do not apply.
    auto working = runtime::ScopeMeasureSettings{};
    working.signal = runtime::ScopeSignal::Working;
    input.settings = working;
    input.reading.scopes = sidecarFor(working, runtime::ScopeRoute::Cpu);
    status = ui::describeScopesStatus(input);
    e.expect(status.warnings.size() == 2, "the Working signal warns only of channel and isolation");
}

void testStatusBehindAndReasons(Expectations& e) {
    auto input = readyInput();
    input.reading.shown = sourceAt(5);
    input.reading.state = ui::ScopesReading::State::Measuring;
    input.reading.behind = true;
    auto status = ui::describeScopesStatus(input);
    e.expect(status.behind.startsWith("Behind") && status.behind.contains("2.000 s") &&
                 status.behind.contains("Titles"),
             "Behind names the measured frame: " + status.behind.toStdString());
    e.expect(status.line.contains("2.000 s") && !status.line.contains("5.000 s"),
             "the line describes the frame the scopes are of");
    input.reading.behind = false;
    input.reading.state = ui::ScopesReading::State::Ready;
    e.expect(ui::describeScopesStatus(input).behind.isEmpty(), "no Behind when the frames agree");

    // The reasons nothing is shown.
    ui::ScopesStatusInput none;
    none.available = false;
    e.expect(ui::describeScopesStatus(none).line.contains("not available"), "no controller");
    none.available = true;
    e.expect(ui::describeScopesStatus(none).line.contains("No Viewer"), "no Viewer");
    none.hasViewer = true;
    none.hidden = true;
    e.expect(ui::describeScopesStatus(none).line.contains("hidden"), "a hidden panel");
    none.hidden = false;
    none.measuring = false;
    e.expect(ui::describeScopesStatus(none).line.contains("collapsed"), "every section collapsed");
    none.measuring = true;
    e.expect(ui::describeScopesStatus(none).line.contains("no frame"), "a Viewer with no frame");
    none.reading.shown = sourceAt(0);
    none.reading.state = ui::ScopesReading::State::Measuring;
    e.expect(ui::describeScopesStatus(none).line.contains("Measuring"), "the first measure");
    none.reading.state = ui::ScopesReading::State::Failed;
    none.reading.message = "no color config";
    e.expect(ui::describeScopesStatus(none).line == "no color config", "a failure says why");
    none.reading.state = ui::ScopesReading::State::Ready;
    auto empty = std::make_shared<ui::PreviewScopeSidecar>();
    none.reading.scopes = empty;
    none.reading.measured = sourceAt(0);
    e.expect(ui::describeScopesStatus(none).line.contains("outside the picture"),
             "a region outside the picture");
}

struct Measured final {
    std::atomic<int> started{0};
    std::atomic<bool> onInterfaceThread{false};
};

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
        auto cache = std::make_shared<PreviewFrameCache>(std::size_t{1} << 28);

        const runtime::CpuCompositionEvaluator cpuEvaluator;
        runtime::ScopeFrameRoutes routes;
        routes.evaluateCpu = [&cpuEvaluator](auto plan, const runtime::EvaluationRequest& request,
                                             const runtime::CancellationToken& cancellation) {
            return cpuEvaluator.evaluate(std::move(plan), request, cancellation);
        };
        const auto production = ui::makeScopesMeasure(routes, {}, std::size_t{1} << 26);
        const auto measured = std::make_shared<Measured>();
        const auto interfaceThread = std::this_thread::get_id();
        ui::ScopesServices services;
        services.scheduler = &scheduler;
        services.bridge = &bridge;
        services.cache = cache;
        services.measure = [production, measured,
                            interfaceThread](const ui::PreviewScopeSource& source,
                                             const ui::PreviewScopeIdentity& identity,
                                             const runtime::CancellationToken& cancellation) {
            if (std::this_thread::get_id() == interfaceThread)
                measured->onInterfaceThread = true;
            ++measured->started;
            return production(source, identity, cancellation);
        };
        services.compositionName = [](document::ProjectId, document::CompositionId) {
            return QStringLiteral("Titles");
        };

        const auto plan = oneSolidPlan();
        const auto frame0 = cpuFrame(plan, 1, 0);
        const auto frame1 = cpuFrame(plan, 2, 1);
        e.expect(frame0 && frame1, "the CPU frames are built");
        if (!frame0 || !frame1)
            return 1;
        gBaseFrame = frame0;
        testStatusItems(e);
        testStatusWarnings(e);
        testStatusBehindAndReasons(e);
        const auto keyOf = [](const std::shared_ptr<const PreparedPreviewFrame>& frame) {
            return PreviewFrameCacheKey::forIdentity(frame->desiredIdentity());
        };
        e.expect(cache->insert(frame0).retained() && cache->insert(frame1).retained(),
                 "the frames are cached");

        // ---- Registration ------------------------------------------------------------------
        {
            ui::EditorRegistry registry;
            e.expect(ui::registerScopesEditor(registry, services, nullptr) &&
                         !ui::registerScopesEditor(registry, services, nullptr),
                     "Scopes registers once");
            e.expect(registry.editors().size() == 1 &&
                         registry.editors().front().id == "bloom.scopes" &&
                         registry.editors().front().displayName == QStringLiteral("Scopes"),
                     "the editor is bloom.scopes, named Scopes");
            ui::EditorArea area(registry, "bloom.scopes", QString{});
            auto* picker = area.findChild<kit::KPanelSwitcher*>(QStringLiteral("editorTypePicker"));
            e.expect(picker != nullptr && picker->findData(QStringLiteral("bloom.scopes")) >= 0,
                     "the area's switcher offers Scopes");
            e.expect(area.findChild<ScopesEditor*>(QStringLiteral("scopesEditor")) != nullptr,
                     "the area hosts a ScopesEditor");
        }

        // ---- A panel with nothing to measure with --------------------------------------------
        {
            ScopesEditor unavailable(ui::ScopesServices{});
            unavailable.show();
            e.expect(unavailable.controller() == nullptr &&
                         unavailable.statusText().line.contains("not available"),
                     "a panel without services says scopes are not available");
        }

        // ---- The panel, sections and settings -------------------------------------------------
        QWidget host;
        host.resize(520, 1500);
        auto* panel = new ScopesEditor(services, nullptr, &host);
        auto* layout = new QVBoxLayout(&host);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(panel);
        auto& controller = *panel->controller();
        e.expect(panel->measureSettings().scopes == 0 && controller.settings().scopes == 0,
                 "a panel that was never shown measures nothing");
        e.expect(panel->statusText().line.contains("No Viewer"),
                 "with no Viewer the status line says why");
        // A frame arrives while the panel is hidden: nothing is measured.
        panel->showViewerFrame(frame0, {});
        e.expect(measured->started == 0 && controller.measuresStarted() == 0 &&
                     controller.reading().state == ScopesReading::State::Idle &&
                     panel->statusText().line.contains("hidden"),
                 "a hidden panel measures nothing");

        host.show();
        e.expect(waitUntil([&] { return panel->isVisible(); }), "the panel is shown");
        e.expect(panel->panelShown() && panel->measureSettings().scopes == render::kScopeTableAll &&
                     controller.settings().scopes == render::kScopeTableAll,
                 "a shown panel asks for every scope");
        e.expect(
            waitUntil([&] { return controller.reading().state == ScopesReading::State::Ready; }),
            "the frame is measured once the panel is shown");
        e.expect(measured->started == 1 && !measured->onInterfaceThread,
                 "it was measured once, on a task worker");

        // The five sections, in order, with the names of the grammar.
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
        e.expect(titlesOk, "five sections in order, open by default");

        // The status line after the measure.
        auto status = panel->statusText();
        e.expect(status.measured && status.line.contains("Titles") &&
                     status.line.contains("frame 0") && status.line.contains("Display") &&
                     status.line.contains("Full") && status.line.contains("CPU") &&
                     status.line.contains("2304 pixels counted") && status.warnings.isEmpty() &&
                     status.behind.isEmpty(),
                 "the status line states what was measured: " + status.line.toStdString());
        auto* statusLabel = panel->findChild<kit::KLabel*>(QStringLiteral("scopesStatusLine"));
        e.expect(statusLabel != nullptr && statusLabel->text() == status.line &&
                     statusLabel->isVisibleTo(panel),
                 "the status line is on the panel, always visible");

        // Collapsing sections leaves their scopes out of the measure.
        const auto bitsAfter = [&](const scopes::Kind kind, const bool collapsed) {
            panel->section(kind)->setCollapsed(collapsed);
            return controller.settings().scopes;
        };
        e.expect(bitsAfter(scopes::Kind::Histogram, true) ==
                     (render::kScopeTableAll & ~render::kScopeTableHistogram),
                 "a collapsed Histogram is not measured");
        e.expect(bitsAfter(scopes::Kind::Waveform, true) ==
                     (render::kScopeTableAll & ~render::kScopeTableHistogram),
                 "Waveform and Parade share a table: it stays while the Parade is open");
        e.expect(bitsAfter(scopes::Kind::Parade, true) ==
                     (render::kScopeTableVectorscope | render::kScopeTableChromaticity),
                 "with both collapsed the Waveform table is left out");
        e.expect(bitsAfter(scopes::Kind::Chromaticity, true) == render::kScopeTableVectorscope,
                 "a collapsed CIE Chromaticity is not measured");
        const auto startedBeforeCollapse = controller.measuresStarted();
        e.expect(bitsAfter(scopes::Kind::Vectorscope, true) == 0 &&
                     controller.reading().state == ScopesReading::State::Idle &&
                     controller.measuresStarted() == startedBeforeCollapse &&
                     panel->statusText().line.contains("collapsed"),
                 "with every section collapsed nothing is measured and the line says so");
        for (const auto kind : kKinds)
            panel->section(kind)->setCollapsed(false);
        e.expect(
            waitUntil([&] { return controller.reading().state == ScopesReading::State::Ready; }) &&
                controller.settings().scopes == render::kScopeTableAll,
            "opening them again measures them");

        // Persisted per panel area.
        panel->section(scopes::Kind::Parade)->setCollapsed(true);
        {
            ScopesEditor second(ui::ScopesServices{});
            second.show();
            e.expect(second.section(scopes::Kind::Parade)->isCollapsed() &&
                         !second.section(scopes::Kind::Waveform)->isCollapsed(),
                     "collapsed state is persisted");
        }
        panel->section(scopes::Kind::Parade)->setCollapsed(false);

        // A hidden panel measures nothing; shown again it asks again.
        panel->hide();
        e.expect(!panel->panelShown() && panel->measureSettings().scopes == 0 &&
                     controller.settings().scopes == 0,
                 "a hidden panel asks for no scope");
        const auto startedHidden = controller.measuresStarted();
        panel->showViewerFrame(frame1, {});
        e.expect(controller.measuresStarted() == startedHidden &&
                     controller.reading().state == ScopesReading::State::Idle,
                 "a frame shown while hidden is not measured");
        panel->show();
        e.expect(
            waitUntil([&] { return controller.reading().state == ScopesReading::State::Ready; }),
            "shown again, the panel measures the Viewer's frame");

        // ---- Behind ---------------------------------------------------------------------------
        panel->showViewerFrame(frame0, {});
        e.expect(
            waitUntil([&] { return controller.reading().state == ScopesReading::State::Ready; }) &&
                panel->statusText().behind.isEmpty(),
            "no Behind while the frames agree");
        // Hold the measure by asking for a setting the frame has no sidecar for.
        panel->setIgnoreTransparent(true);
        status = panel->statusText();
        e.expect(controller.reading().behind && status.behind.startsWith("Behind") &&
                     status.behind.contains("frame 0"),
                 "Behind names the measured frame while the new measure runs: " +
                     status.behind.toStdString());
        auto* behindLabel = panel->findChild<kit::KLabel*>(QStringLiteral("scopesStatusBehind"));
        e.expect(behindLabel != nullptr && behindLabel->isVisibleTo(panel) &&
                     behindLabel->tone() == kit::Color::Warn,
                 "Behind is shown in the warning color");
        e.expect(
            waitUntil([&] { return controller.reading().state == ScopesReading::State::Ready; }) &&
                panel->statusText().behind.isEmpty(),
            "the label goes when the measure arrives");
        panel->setIgnoreTransparent(false);
        e.expect(waitUntil([&] {
                     return controller.reading().state == ScopesReading::State::Ready &&
                            !controller.busy();
                 }),
                 "every pixel is counted again");

        // ---- Warnings, through the panel
        // ----------------------------------------------------------
        panel->showViewerFrame(frame0, runtime::ViewAdjust{.exposure = -1.0, .gamma = 1.0});
        status = panel->statusText();
        e.expect(status.warnings.size() == 1 && status.warnings[0].contains("Exposure"),
                 "the Viewer's exposure is a warning");
        auto* warningLabel = panel->findChild<kit::KLabel*>(QStringLiteral("scopesStatusWarnings"));
        e.expect(warningLabel != nullptr && warningLabel->tone() == kit::Color::Warn &&
                     warningLabel->isVisibleTo(panel) && warningLabel->text().contains("Exposure"),
                 "warnings are in the warning color");
        e.expect(
            waitUntil([&] { return controller.reading().state == ScopesReading::State::Ready; }),
            "the exposed frame is measured");
        panel->showViewerFrame(frame0, {});
        e.expect(
            waitUntil([&] { return controller.reading().state == ScopesReading::State::Ready; }) &&
                panel->statusText().warnings.isEmpty(),
            "a neutral Viewer has no warning");

        // ---- Settings that measure again
        // ----------------------------------------------------------
        const auto settled = [&] {
            return waitUntil([&] {
                return controller.reading().state == ScopesReading::State::Ready &&
                       !controller.busy();
            });
        };
        e.expect(settled(), "the panel is settled");
        auto started = controller.measuresStarted();
        panel->setSignal(runtime::ScopeSignal::Working);
        e.expect(settled() && controller.measuresStarted() == started + 1 &&
                     controller.settings().signal == runtime::ScopeSignal::Working,
                 "the Working signal measures again");
        e.expect(panel->statusText().line.contains("Working space"),
                 "and the line names the working space");
        auto* linear = panel->findChild<kit::KLabel*>(QStringLiteral("scopesLinearNote"));
        e.expect(linear != nullptr && linear->isVisibleTo(panel) &&
                     linear->text() == QStringLiteral("Linear values"),
                 "the Vectorscope header notes linear values for the Working signal");
        panel->setAxisMaximum(4);
        e.expect(settled() && controller.measuresStarted() == started + 2 &&
                     controller.settings().axisMaximum == 4,
                 "a new axis maximum measures again");
        panel->setAxisMaximum(3);
        e.expect(panel->axisMaximum() == 4, "an axis maximum that is not valid is refused");
        panel->setSignal(runtime::ScopeSignal::Display);
        e.expect(settled() && controller.settings().axisMaximum == 1,
                 "Display is measured on its own axis");
        started = controller.measuresStarted();
        panel->setIgnoreTransparent(true);
        e.expect(settled() && controller.measuresStarted() == started + 1 &&
                     controller.settings().ignoreTransparent,
                 "Ignore Transparent Pixels measures again");
        e.expect(controller.reading().scopes != nullptr &&
                     controller.reading().scopes->tables.counted() == 34U * 19U,
                 "and counts only the pixels of the solid");
        panel->setIgnoreTransparent(false);
        e.expect(settled(), "back to every pixel");

        // ---- The region seam
        // ----------------------------------------------------------------------
        int regionSignals = 0;
        std::optional<runtime::ScopeRegion> lastRegion;
        int armedSignals = 0;
        bool lastArmed = false;
        QObject::connect(panel, &ScopesEditor::regionChanged,
                         [&](const std::optional<runtime::ScopeRegion>& region) {
                             ++regionSignals;
                             lastRegion = region;
                         });
        QObject::connect(panel, &ScopesEditor::regionArmedChanged, [&](const bool armed) {
            ++armedSignals;
            lastArmed = armed;
        });
        auto* regionButton = panel->findChild<kit::KButton*>(QStringLiteral("scopesRegion"));
        auto* clearButton = panel->findChild<kit::KButton*>(QStringLiteral("scopesClearRegion"));
        e.expect(regionButton != nullptr && clearButton != nullptr && !panel->region() &&
                     !panel->regionArmed() && !clearButton->isEnabled(),
                 "no region at first, and Clear Region has nothing to clear");
        regionButton->click();
        e.expect(panel->regionArmed() && armedSignals == 1 && lastArmed,
                 "the Region toggle arms a drag");
        panel->setRegionArmed(true);
        e.expect(armedSignals == 1, "arming again says nothing");
        started = controller.measuresStarted();
        panel->setRegion(runtime::ScopeRegion{4, 2, 16, 8});
        e.expect(regionSignals == 1 && lastRegion && lastRegion->width == 16 &&
                     panel->measureSettings().region == lastRegion &&
                     controller.settings().region == lastRegion && clearButton->isEnabled(),
                 "a region reaches the measure settings and says so");
        e.expect(settled() && controller.measuresStarted() == started + 1 &&
                     controller.reading().scopes != nullptr &&
                     controller.reading().scopes->tables.counted() == 16U * 8U &&
                     controller.reading().scopes->area == runtime::ScopeMeasuredArea{4, 2, 16, 8},
                 "the region measures only its rectangle");
        e.expect(panel->statusText().line.contains("Region 4, 2, 16 x 8") &&
                     panel->statusText().line.contains("128 pixels counted"),
                 "the status line gives the rectangle and the pixels counted: " +
                     panel->statusText().line.toStdString());
        panel->setRegion(runtime::ScopeRegion{4, 2, 16, 8});
        e.expect(regionSignals == 1, "setting the same region says nothing");
        panel->setRegion(runtime::ScopeRegion{4, 2, 0, 8});
        e.expect(regionSignals == 2 && !panel->region() && !lastRegion,
                 "an empty rectangle is no region");
        panel->setRegion(runtime::ScopeRegion{0, 0, 8, 8});
        clearButton->click();
        e.expect(!panel->region() && regionSignals == 4 && !lastRegion &&
                     !clearButton->isEnabled() && !controller.settings().region &&
                     panel->regionArmed(),
                 "Clear Region clears the region and keeps the toggle");
        regionButton->click();
        e.expect(!panel->regionArmed() && armedSignals == 2 && !lastArmed, "the toggle disarms");
        e.expect(settled(), "the cleared region is measured");

        // ---- Mapping runs on workers, Brightness maps again and does not measure
        // ----------------------
        auto* renderer = panel->renderer();
        e.expect(renderer != nullptr, "the panel has a trace renderer");
        const auto allMapped = [&] {
            for (const auto kind : kKinds) {
                if (panel->view(kind)->traceImage().isNull() || renderer->busy(kind) ||
                    panel->view(kind)->showsBehind())
                    return false;
            }
            return true;
        };
        e.expect(waitUntil(allMapped), "every open scope's trace is mapped");
        bool workers = true;
        for (const auto kind : kKinds) {
            const auto finished = renderer->finished(kind);
            workers = workers && finished != nullptr && finished->workerThread != interfaceThread;
        }
        e.expect(workers, "every mapping ran on a task worker");
        e.expect(!measured->onInterfaceThread, "no measure ran on the interface thread");
        started = controller.measuresStarted();
        const auto mapsBefore = renderer->mapsStarted();
        panel->setBrightness(2.0F);
        e.expect(waitUntil([&] { return !renderer->busy(); }) &&
                     renderer->mapsStarted() > mapsBefore &&
                     controller.measuresStarted() == started,
                 "a Brightness change maps again and does not measure again");
        e.expect(panel->view(scopes::Kind::Waveform)->currentRequest().gain == 2.0F,
                 "the gain reaches the mapping");

        // Hover: the value under the pointer and the count of its cell.
        auto* histogram = panel->view(scopes::Kind::Histogram);
        const auto plot = histogram->plotRect();
        // The red solid is at full red: the rightmost bin of the red row.
        const QPointF redTop(plot.left() + 1023.5 / 1024.0 * plot.width(), plot.top() + 10);
        e.expect(histogram->readoutAt(redTop).contains("Code 1023") &&
                     histogram->readoutAt(redTop).contains("Count 646"),
                 "the histogram readout names the code and the count: " +
                     histogram->readoutAt(redTop).toStdString());
        e.expect(histogram->readoutAt(QPointF(-5, -5)).isEmpty(), "outside the plot, no readout");
        auto* waveform = panel->view(scopes::Kind::Waveform);
        const auto waveformPlot = waveform->plotRect();
        e.expect(waveform->readoutAt(waveformPlot.center()).contains("Column") &&
                     waveform->readoutAt(waveformPlot.center()).contains("Count"),
                 "the waveform readout names the column, the code and the count");
        auto* vectorscope = panel->view(scopes::Kind::Vectorscope);
        e.expect(vectorscope->readoutAt(vectorscope->plotRect().center()).contains("Cb") &&
                     vectorscope->readoutAt(vectorscope->plotRect().center()).contains("Count"),
                 "the vectorscope readout names cb, cr and the count");
        auto* chromaticity = panel->view(scopes::Kind::Chromaticity);
        e.expect(chromaticity->readoutAt(chromaticity->plotRect().center()).contains("x 0.5") &&
                     chromaticity->readoutAt(chromaticity->plotRect().center()).contains("Count"),
                 "the chromaticity readout names x, y and the count");

        // A scope whose tables are another frame's is dimmed and labeled.
        panel->showViewerFrame(frame1, {});
        e.expect(controller.reading().behind ||
                     controller.reading().state == ScopesReading::State::Ready,
                 "the next frame is being measured");
        e.expect(
            waitUntil([&] { return controller.reading().state == ScopesReading::State::Ready; }),
            "the next frame is measured");

        // ---- No Viewer, and no frame: an empty graticule
        // ------------------------------------------
        panel->showViewerFrame(nullptr, {});
        e.expect(controller.reading().scopes == nullptr &&
                     panel->statusText().line.contains("no frame"),
                 "with no frame the line says why");
        e.expect(
            panel->view(scopes::Kind::Waveform)->traceImage().isNull() &&
                panel->view(scopes::Kind::Vectorscope)->traceImage().isNull() &&
                panel->view(scopes::Kind::Waveform)->readoutAt(waveformPlot.center()).isEmpty(),
            "each open section shows an empty graticule");
        panel->followViewer(nullptr);
        e.expect(panel->statusText().line.contains("No Viewer"), "and with no Viewer, says so");
        panel->showViewerFrame(frame0, {});
        e.expect(
            waitUntil([&] { return controller.reading().state == ScopesReading::State::Ready; }),
            "a frame comes back");

        // ---- A RAM preview fill measures the frames the cache retains
        // -----------------------------
        const auto frame2 = cpuFrame(plan, 3, 2);
        const auto frame3 = cpuFrame(plan, 4, 3);
        e.expect(frame2 && frame3, "the fill's frames are built");
        if (frame2 && frame3) {
            const auto settings = panel->measureSettings();
            const auto identity = ui::PreviewScopeIdentity{.settings = settings, .viewAdjust = {}};
            panel->setRamFillRunning(true);
            e.expect(cache->insert(frame2).retained(), "a fill frame is cached");
            e.expect(waitUntil([&] {
                         return cache->scopesFor(keyOf(frame2), identity) != nullptr &&
                                !controller.busy();
                     }),
                     "during a fill the retained frames are measured");
            panel->setRamFillRunning(false);
            const auto before = controller.measuresStarted();
            e.expect(cache->insert(frame3).retained(), "another frame is cached");
            for (int turn = 0; turn < 20; ++turn)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            e.expect(controller.measuresStarted() == before &&
                         cache->scopesFor(keyOf(frame3), identity) == nullptr,
                     "without a fill the retained frames are not measured");
            // A hidden panel does not measure a fill either.
            panel->setRamFillRunning(true);
            panel->hide();
            const auto hiddenBefore = controller.measuresStarted();
            const auto frame4 = cpuFrame(plan, 5, 4);
            e.expect(frame4 && cache->insert(frame4).retained(), "a frame is cached while hidden");
            for (int turn = 0; turn < 20; ++turn)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            e.expect(controller.measuresStarted() == hiddenBefore,
                     "a hidden panel measures nothing during a fill");
            panel->show();
            panel->setRamFillRunning(false);
        }

        e.expect(!measured->onInterfaceThread, "no measure ran on the interface thread");
        host.hide();
        bridge.beginShutdown();
        for (int turn = 0; turn < 2000 && !scheduler.isQuiescent(); ++turn)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (e.failures() != 0)
            return 1;
        std::cout << "PASS: scopes panel\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected test exception: " << exception.what() << '\n';
        return 1;
    }
}
