// Review tool, not a test (#724 Scopes): offscreen pictures of the Scopes panel and the Viewer's
// scope region overlay, for comparing the result with the drawing style of the specification.
//
//   bloom_ui_scopes_capture <directory> <dpr label>
//
// The device pixel ratio follows QT_SCALE_FACTOR, and the label (1 or 1.5) names the files:
// <directory>/724-<what>-w<width>-dpr<label>.png. The panel and the Viewer are the real ones, over
// the real preview pipeline, and every scope is the production measure of the frame the Viewer
// shows (the CPU reference), never a stand-in. Two scenes are authored here: 75% color bars (eight
// solids) and a generated sunset (an SVG of smooth gradients; the repository holds no photograph).
// Not registered with CTest.

#include "await_qt.hpp"
#include <bloom/commands/operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/scope_read.hpp>
#include <bloom/scripting/session.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/kit/section.hpp>
#include <bloom/ui/kit/theme.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/scopes_editor.hpp>
#include <bloom/ui/scopes_region_store.hpp>
#include <bloom/ui/scopes_trace_renderer.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QImage>
#include <QLayout>
#include <QScrollArea>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace bloom;
using bloom::test_support::awaitFailureCount;
using bloom::test_support::awaitQtCondition;
using namespace std::chrono_literals;
namespace ui = bloom::ui;
namespace scopes = bloom::ui::scopes;

constexpr std::int64_t kFps = 24;

// ---- The scenes ---------------------------------------------------------------------------------

// Eight bars at 75% of the display signal: the linear value whose sRGB encoding is 0.75.
constexpr double kLinear75 = 0.52252;
constexpr std::array<std::array<double, 3>, 8> kBars{{{kLinear75, kLinear75, kLinear75},
                                                      {kLinear75, kLinear75, 0.0},
                                                      {0.0, kLinear75, kLinear75},
                                                      {0.0, kLinear75, 0.0},
                                                      {kLinear75, 0.0, kLinear75},
                                                      {kLinear75, 0.0, 0.0},
                                                      {0.0, 0.0, kLinear75},
                                                      {0.0, 0.0, 0.0}}};

// A sunset: a sky that runs from deep blue to a warm horizon, a low sun with a soft halo, two
// ranges of hills and a dark foreground, all smooth gradients, so every scope sees a continuous
// spread of values rather than a few flat ones.
constexpr auto kSunset =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="360" viewBox="0 0 640 360">
<defs>
<linearGradient id="sky" x1="0" y1="0" x2="0" y2="1">
<stop offset="0" stop-color="#101c40"/><stop offset="0.3" stop-color="#3d5a96"/>
<stop offset="0.55" stop-color="#c77f78"/><stop offset="0.75" stop-color="#f1b46c"/>
<stop offset="1" stop-color="#f8dc9a"/></linearGradient>
<radialGradient id="halo" cx="0.5" cy="0.5" r="0.5">
<stop offset="0" stop-color="#fff8e0" stop-opacity="1"/><stop offset="0.25" stop-color="#ffe3a0" stop-opacity="0.85"/>
<stop offset="0.6" stop-color="#ffc070" stop-opacity="0.25"/><stop offset="1" stop-color="#ffb060" stop-opacity="0"/></radialGradient>
<linearGradient id="far" x1="0" y1="0" x2="0" y2="1">
<stop offset="0" stop-color="#7a6a86"/><stop offset="1" stop-color="#3f3a5a"/></linearGradient>
<linearGradient id="near" x1="0" y1="0" x2="0" y2="1">
<stop offset="0" stop-color="#3a4552"/><stop offset="1" stop-color="#1c2630"/></linearGradient>
<linearGradient id="ground" x1="0" y1="0" x2="0" y2="1">
<stop offset="0" stop-color="#26301f"/><stop offset="1" stop-color="#0d120c"/></linearGradient>
</defs>
<rect width="640" height="360" fill="url(#sky)"/>
<circle cx="420" cy="215" r="170" fill="url(#halo)"/>
<path d="M0 235 C80 205 150 215 230 228 C310 240 380 208 470 222 C550 234 600 214 640 220 L640 360 L0 360 Z" fill="url(#far)"/>
<path d="M0 270 C90 240 170 262 260 268 C350 274 430 246 520 258 C580 266 620 256 640 252 L640 360 L0 360 Z" fill="url(#near)"/>
<path d="M0 312 C120 296 210 316 320 314 C430 312 520 296 640 308 L640 360 L0 360 Z" fill="url(#ground)"/>
</svg>
)svg";

struct Scene final {
    std::unique_ptr<scripting::Session> scripting;
    document::CompositionId composition;
};

template <typename Operation, typename... Arguments>
scripting::SessionCommandResult run(Scene& scene, Arguments&&... args) {
    commands::Transaction transaction("Capture", scene.scripting->snapshot().revision());
    transaction.emplace<Operation>(std::forward<Arguments>(args)...);
    auto result = scene.scripting->execute(std::move(transaction));
    if (!result.succeeded())
        throw std::runtime_error("a scene command was refused");
    return result;
}

[[nodiscard]] Scene newScene(const char* name) {
    const auto format = document::CompositionFormat::create(
        640, 360, core::PixelAspectRatio::square(), *document::FrameRate::create(kFps, 1));
    auto created =
        scripting::Session::createNew(name, "Main", core::RationalTime::fromInteger(2), *format);
    if (!created)
        throw std::runtime_error("the session is not created");
    Scene scene;
    scene.scripting = std::move(created).takeSession();
    scene.composition = scene.scripting->snapshot().project().compositions().front().id();
    return scene;
}

[[nodiscard]] document::ParameterId role(Scene& scene, const document::NodeId node,
                                         const std::string_view name) {
    const auto snapshot = scene.scripting->snapshot();
    const auto* record =
        snapshot.project().findComposition(scene.composition)->graph().findNode(node);
    for (const auto& binding : record->parameters)
        if (binding.role == name)
            return binding.parameterId;
    throw std::runtime_error("no such parameter");
}

[[nodiscard]] Scene barsScene() {
    auto scene = newScene("Color bars");
    for (std::size_t index = 0; index < kBars.size(); ++index) {
        const auto added = run<commands::AddSolidLayer>(
            scene, scene.composition, "Bar " + std::to_string(index),
            core::Color4d{kBars[index][0], kBars[index][1], kBars[index][2], 1.0},
            document::Vec2d{80.0 * static_cast<double>(index) + 40.0, 180.0});
        const auto solid =
            added.command->outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput)
                .value();
        run<commands::SetParameterSource>(scene, scene.composition,
                                          role(scene, solid, document::kSolidWidthParameterRole),
                                          document::ConstantValueSource{80.0});
        run<commands::SetParameterSource>(scene, scene.composition,
                                          role(scene, solid, document::kSolidHeightParameterRole),
                                          document::ConstantValueSource{360.0});
    }
    return scene;
}

[[nodiscard]] Scene sunsetScene() {
    auto scene = newScene("Sunset");
    const auto size = std::char_traits<char>::length(kSunset);
    auto bytes = std::make_shared<std::vector<std::byte>>(size);
    std::memcpy(bytes->data(), kSunset, size);
    auto prepared = commands::prepareSvgSource(std::move(bytes), "sunset.svg");
    if (!prepared.source)
        throw std::runtime_error("the sunset is not prepared");
    run<commands::AddSvgArtwork>(scene, scene.composition, prepared.source, "Sunset",
                                 commands::SvgImportMode::Layer, true);
    return scene;
}

// ---- The workspace: the real Viewer and panel over the real preview pipeline
// ---------------------

struct Rig final {
    runtime::TaskScheduler scheduler;
    runtime::NodeDefinitionRegistry definitions;
    std::unique_ptr<runtime::SnapshotCompiler> compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualified;
    std::unique_ptr<ui::TaskUiBridge> bridge;
    Rig() {
        if (!runtime::registerBuiltInNodeDefinitions(definitions))
            throw std::runtime_error("node definitions register");
        definitions.freeze();
        compiler = std::make_unique<runtime::SnapshotCompiler>(definitions);
        qualified.publish(runtime::buildBloomNeutralQualifiedDisplayProcessor());
        bridge = std::make_unique<ui::TaskUiBridge>(scheduler, nullptr, 1ms);
    }
};

class Workspace final {
  public:
    explicit Workspace(Scene scene) : scene_(std::move(scene)) {
        // Section and Viewer state persist in the settings; every workspace starts with the
        // defaults.
        QSettings().clear();
        session_ = std::make_unique<ui::CompositionSession>(
            *scene_.scripting->document(), *scene_.scripting->commandStack(), scene_.composition);
        if (const auto* settings = scene_.scripting->hostSession()->colorSettings())
            session_->setColorSettings(*settings);
        cache_ = std::make_shared<ui::PreviewFrameCache>(std::size_t{1} << 28);
        ui::CompositionPreviewSettings settings;
        settings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
        settings.colorIntent = session_->colorIntent();
        controller_ = std::make_unique<ui::CompositionPreviewController>(
            *session_, rig_.scheduler, *rig_.bridge,
            ui::makeCompositionPreviewPipeline(*rig_.compiler, rig_.evaluator, rig_.displayPreparer,
                                               rig_.qualified),
            settings, cache_);
        viewer_ = std::make_unique<ui::ViewerEditor>(*session_, *controller_);
        viewer_->setScopesRegionStore(&store_);

        const auto production = ui::makeScopesMeasure({});
        ui::ScopesServices services;
        services.scheduler = &rig_.scheduler;
        services.bridge = rig_.bridge.get();
        services.cache = cache_;
        services.regions = &store_;
        // The production measure; a held frame waits (so the panel can be shown behind it).
        services.measure = [production, this](const ui::ScopesMeasureInput& input,
                                              const ui::PreviewScopeIdentity& identity,
                                              const runtime::CancellationToken& token) {
            while (hold_.load() && !token.isCancellationRequested())
                std::this_thread::sleep_for(1ms);
            return production(input, identity, token);
        };
        services.compositionName = [](document::ProjectId, document::CompositionId) {
            return QStringLiteral("Main");
        };
        panel_ = std::make_unique<ui::ScopesEditor>(services);
    }

    ~Workspace() {
        hold_ = false;
        panel_.reset();
        viewer_.reset();
        controller_->beginShutdown();
        rig_.bridge->beginShutdown();
        (void)awaitQtCondition("the scheduler drains after the rig shuts down",
                               [this] { return rig_.scheduler.isQuiescent(); });
    }

    [[nodiscard]] ui::ViewerEditor& viewer() { return *viewer_; }
    [[nodiscard]] ui::ScopesEditor& panel() { return *panel_; }
    [[nodiscard]] ui::CompositionSession& session() { return *session_; }
    [[nodiscard]] ui::ScopesRegionStore& store() { return store_; }
    [[nodiscard]] Scene& scene() { return scene_; }
    void hold(const bool value) { hold_ = value; }

    [[nodiscard]] ui::ScopesRegionStore::Key key() const {
        return {session_->snapshot().project().id(), session_->compositionId()};
    }

    // Shows both and waits for the Viewer's frame and the panel's tables and traces of it.
    void show(const int viewerWidth, const int panelWidth, const int panelHeight) {
        viewer_->resize(viewerWidth, viewerWidth * 11 / 20);
        viewer_->show();
        panel_->resize(panelWidth, panelHeight);
        panel_->show();
        panel_->followViewer(viewer_.get());
        settle();
    }

    // True when the panel shows the Viewer's frame at full detail and every trace on show was
    // mapped from those tables, with nothing still running.
    [[nodiscard]] bool settled() const {
        const auto& state = controller_->state();
        const auto reading = panel_->controller()->reading();
        if (state.activity != ui::PreviewActivity::Ready || state.frame == nullptr ||
            !viewer_->mappingForTest().has_value() ||
            reading.state != ui::ScopesReading::State::Ready || reading.behind ||
            reading.detail != 0 || reading.scopes == nullptr || panel_->renderer()->busy())
            return false;
        for (std::size_t kind = 0; kind < ui::scopes::kKindCount; ++kind) {
            const auto finished = panel_->renderer()->finished(static_cast<ui::scopes::Kind>(kind));
            if (finished != nullptr && finished->request.tables.get() != &reading.scopes->tables)
                return false;
        }
        return true;
    }

    // After a resize: every trace asked for at the new size has arrived and no other is asked
    // for once the event loop has turned. A grab before that paints the trace of the size before.
    void awaitTraces() {
        const bool idle = awaitQtCondition("the traces of the current size arrive", [&] {
            if (panel_->renderer()->busy())
                return false;
            const auto maps = panel_->renderer()->mapsStarted();
            for (int turn = 0; turn < 10; ++turn)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            return !panel_->renderer()->busy() && panel_->renderer()->mapsStarted() == maps;
        });
        if (!idle)
            throw std::runtime_error("the panel's traces never settled after a resize");
    }

    void settle() {
        // Settled, and still settled with no new mapping after the event loop has turned: a
        // layout pass or a newer reading may ask for another trace.
        const bool ready = awaitQtCondition("the panel settles on the Viewer's frame", [&] {
            if (!settled())
                return false;
            const auto maps = panel_->renderer()->mapsStarted();
            for (int turn = 0; turn < 10; ++turn)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            return settled() && panel_->renderer()->mapsStarted() == maps;
        });
        if (!ready) {
            const auto reading = panel_->controller()->reading();
            throw std::runtime_error(
                "the panel did not settle on the Viewer's frame: preview activity " +
                std::to_string(static_cast<int>(controller_->state().activity)) + ", frame " +
                std::to_string(controller_->state().frame != nullptr) + ", reading " +
                std::to_string(static_cast<int>(reading.state)) + " '" + reading.message +
                "', behind " + std::to_string(reading.behind) + ", detail " +
                std::to_string(reading.detail));
        }
        (void)viewer_->grab();
    }

  private:
    Rig rig_;
    Scene scene_;
    std::unique_ptr<ui::CompositionSession> session_;
    std::shared_ptr<ui::PreviewFrameCache> cache_;
    std::unique_ptr<ui::CompositionPreviewController> controller_;
    ui::ScopesRegionStore store_;
    std::unique_ptr<ui::ViewerEditor> viewer_;
    std::unique_ptr<ui::ScopesEditor> panel_;
    std::atomic<bool> hold_{false};
};

// ---- Output
// ---------------------------------------------------------------------------------------

struct Output final {
    QString directory;
    QString label;
    int failures = 0;

    void save(const QImage& image, const QString& what, const int width) {
        const QString path =
            directory +
            QStringLiteral("/724-%1-w%2-dpr%3.png").arg(what, QString::number(width), label);
        if (image.isNull() || !image.save(path)) {
            std::cerr << "cannot write " << path.toStdString() << '\n';
            ++failures;
            return;
        }
        std::cout << path.toStdString() << ' ' << image.width() << 'x' << image.height() << '\n';
    }
};

// The panel at `width`, as tall as its content wants (capped), or `height` when that is given.
QImage grabPanel(Workspace& work, const int width, const int height = 0) {
    auto& panel = work.panel();
    panel.resize(width, 4000);
    work.awaitTraces();
    int wanted = height;
    if (wanted == 0) {
        auto* header = panel.findChild<QWidget*>(QStringLiteral("scopesHeader"));
        auto* scroll = panel.findChild<QScrollArea*>(QStringLiteral("scopesScroll"));
        auto* content = scroll != nullptr ? scroll->widget() : nullptr;
        if (header == nullptr || content == nullptr || content->layout() == nullptr)
            throw std::runtime_error("the panel's parts are missing");
        const int contentHeight = content->layout()->totalHeightForWidth(
            scroll->viewport()->width() > 0 ? scroll->viewport()->width() : width);
        wanted = header->height() + std::max(contentHeight, content->sizeHint().height()) + 4;
    }
    panel.resize(width, wanted);
    work.awaitTraces();
    return panel.grab().toImage();
}

// The top of the panel (header controls and the status lines), down to the scroll area.
QImage grabStatus(Workspace& work, const int width) {
    auto& panel = work.panel();
    panel.resize(width, 900);
    work.awaitTraces();
    auto* scroll = panel.findChild<QWidget*>(QStringLiteral("scopesScroll"));
    const int bottom = scroll != nullptr ? scroll->geometry().top() : 300;
    return panel.grab(QRect(0, 0, width, bottom + 24)).toImage();
}

QImage grabViewer(Workspace& work) {
    for (int turn = 0; turn < 10; ++turn)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return work.viewer().grab().toImage();
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    if (argc < 3) {
        std::cerr << "usage: bloom_ui_scopes_capture <directory> <dpr label>\n";
        return 2;
    }
    QTemporaryDir settingsDirectory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    application.setOrganizationName(QStringLiteral("BloomScopesCapture"));
    application.setApplicationName(QStringLiteral("ScopesCapture"));
    ui::kit::installAfterlabTheme(application);
    Output out{QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2])};
    const QString before = out.directory + QStringLiteral("/../before");
    try {
        Workspace probe(barsScene());
        probe.show(900, 400, 900);
        const int narrow = probe.panel().minimumSizeHint().width();
        std::cout << "panel minimum width " << narrow << '\n';
        const std::array<int, 3> widths{2000, 900, narrow};

        // Before: the Viewer with no overlay. After: the region (and the region of interest, for
        // contrast).
        // The Viewer's own minimum width is wider than the panel's, so its narrow case is 320.
        for (const int width : {2000, 900, 320}) {
            Workspace work(barsScene());
            work.show(width, std::max(width / 3, narrow), 900);
            Output beforeOut{before, out.label};
            beforeOut.save(grabViewer(work), QStringLiteral("viewer-without-region"), width);
            out.failures += beforeOut.failures;
            work.store().setRegion(work.key(), runtime::ScopeRegion{120, 70, 320, 200});
            ui::ViewerEditor::RemoteChange change;
            change.regionOfInterest = std::optional<QRectF>(QRectF(300.0, 140.0, 280.0, 170.0));
            const auto refusal = work.viewer().applyRemoteChange(change);
            if (!refusal.isEmpty())
                throw std::runtime_error(refusal.toStdString());
            work.settle();
            out.save(grabViewer(work), QStringLiteral("viewer-region-and-roi"), width);
        }

        for (const int width : widths) {
            // The panel with all five sections open, on bars and on the sunset.
            {
                Workspace work(barsScene());
                work.show(900, width, 900);
                out.save(grabPanel(work, width, width == narrow ? 800 : 0),
                         QStringLiteral("panel-bars"), width);
                // Collapsed: only the Waveform and Vectorscope stay open.
                for (const auto kind :
                     {scopes::Kind::Parade, scopes::Kind::Histogram, scopes::Kind::Chromaticity})
                    work.panel().section(kind)->setCollapsed(true);
                work.settle();
                out.save(grabPanel(work, width), QStringLiteral("panel-collapsed"), width);
                for (const auto kind : {scopes::Kind::Waveform, scopes::Kind::Vectorscope})
                    work.panel().section(kind)->setCollapsed(true);
                for (int turn = 0; turn < 10; ++turn)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                out.save(grabPanel(work, width), QStringLiteral("panel-all-collapsed"), width);
            }
            {
                Workspace work(sunsetScene());
                work.show(900, width, 900);
                out.save(grabPanel(work, width, width == narrow ? 800 : 0),
                         QStringLiteral("panel-photo"), width);
            }
            // The Working signal, with the Vectorscope's "Linear values" note.
            {
                Workspace work(barsScene());
                work.show(900, width, 900);
                work.panel().setSignal(runtime::ScopeSignal::Working);
                work.settle();
                out.save(grabPanel(work, width), QStringLiteral("panel-working-linear"), width);
            }
            // The status line with a warning (exposure not neutral) and with "Behind".
            {
                Workspace work(barsScene());
                work.show(900, width, 900);
                ui::ViewerEditor::RemoteChange change;
                change.exposure = 1.0;
                const auto refusal = work.viewer().applyRemoteChange(change);
                if (!refusal.isEmpty())
                    throw std::runtime_error(refusal.toStdString());
                // The change reaches the panel with the Viewer's next frame; settle() alone would
                // accept the tables of the picture before it.
                if (!awaitQtCondition("the scopes on show are of the exposed picture", [&] {
                        const auto reading = work.panel().controller()->reading();
                        return reading.scopes != nullptr &&
                               reading.scopes->identity.viewAdjust.exposure == 1.0;
                    }))
                    throw std::runtime_error("the exposure change never reached the scopes");
                work.settle();
                out.save(grabStatus(work, width), QStringLiteral("status-warning"), width);
                // The tables on show are not the shown frame's under the current settings while
                // the new measure runs: Behind. (A time change drops the Viewer's frame, and with
                // it the tables, until the next frame arrives, so a setting change is what holds
                // the earlier tables on show.)
                work.hold(true);
                work.panel().setIgnoreTransparent(true);
                const bool behind =
                    awaitQtCondition("the panel shows Behind while the held measure runs", [&] {
                        const auto reading = work.panel().controller()->reading();
                        return reading.behind &&
                               reading.state == ui::ScopesReading::State::Measuring;
                    });
                if (!behind) {
                    const auto reading = work.panel().controller()->reading();
                    throw std::runtime_error(
                        "the panel never showed Behind: reading " +
                        std::to_string(static_cast<int>(reading.state)) + " behind " +
                        std::to_string(reading.behind) + " started " +
                        std::to_string(work.panel().controller()->measuresStarted()) + " time " +
                        std::to_string(work.session().currentTime().numerator()) + " shown " +
                        std::to_string(reading.shown.has_value()));
                }
                for (int turn = 0; turn < 10; ++turn)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                out.save(grabPanel(work, width), QStringLiteral("status-behind"), width);
                work.hold(false);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "capture failed: " << error.what() << '\n';
        return 1;
    }
    return out.failures + awaitFailureCount() == 0 ? 0 : 1;
}
