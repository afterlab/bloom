// Review tool, not a test (#724, #730 Scopes): offscreen pictures of the Scopes panel in its editor
// area (with the header menus), the menus themselves, and the Viewer's scope region overlay, for
// comparing the result with the drawing of the specification.
//
//   bloom_ui_scopes_capture <directory> <dpr label> [photograph]
//
// The device pixel ratio follows QT_SCALE_FACTOR, and the label (1 or 1.5) names the files:
// <directory>/730-<what>-w<width>-dpr<label>.png. The area, the panel and the Viewer are the real
// ones, over the real preview pipeline, and every scope is drawn from the tables the frame's own
// preparation counted (the CPU route), never a stand-in. Two scenes are authored here: 75% color
// bars (eight solids) and a photograph given on the command line (scaled to 640 x 360 and imported
// as an image layer; the repository holds no photograph), or, without one, a generated sunset (an
// SVG of smooth gradients). Not registered with CTest.

#include "await_qt.hpp"
#include <bloom/commands/asset_operations.hpp>
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
#include <bloom/ui/editor_area.hpp>
#include <bloom/ui/editor_registry.hpp>
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
#include <QMenu>
#include <QScrollArea>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
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

// A photograph, scaled to the composition's 640 x 360 and imported as a still image layer.
[[nodiscard]] Scene photoScene(const QString& photograph, const QString& scratch) {
    QImage image(photograph);
    if (image.isNull())
        throw std::runtime_error("the photograph does not load");
    const QString scaled = scratch + QStringLiteral("/photograph-640x360.png");
    if (!image.scaled(640, 360, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation)
             .copy(0, 0, 640, 360)
             .save(scaled))
        throw std::runtime_error("the scaled photograph is not written");
    auto scene = newScene("Photograph");
    const std::filesystem::path path = scaled.toStdString();
    {
        commands::Transaction transaction("Import", scene.scripting->snapshot().revision());
        if (!transaction.add(std::make_unique<commands::ImportAssets>(
                std::vector<std::filesystem::path>{path}, path.parent_path())))
            throw std::runtime_error("the photograph import is not prepared");
        if (!scene.scripting->commandStack()->execute(std::move(transaction)).changed())
            throw std::runtime_error("the photograph is not imported");
    }
    const auto assets = scene.scripting->snapshot().project().assets();
    if (assets.empty())
        throw std::runtime_error("the project holds no photograph");
    run<commands::AddImageLayer>(scene, scene.composition, assets.front().id);
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
        // The Viewer hands the store to its preview controller: while a Scopes panel is shown,
        // every frame's preparation counts the tables too.
        viewer_->setScopesRegionStore(&store_);

        ui::ScopesServices services;
        services.scheduler = &rig_.scheduler;
        services.bridge = rig_.bridge.get();
        services.store = &store_;
        if (!ui::registerScopesEditor(registry_, services))
            throw std::runtime_error("the Scopes editor does not register");
        // The panel in its editor area, which hosts its header menus.
        area_ = std::make_unique<ui::EditorArea>(registry_, "bloom.scopes", QString{});
        panel_ = area_->findChild<ui::ScopesEditor*>(QStringLiteral("scopesEditor"));
        if (panel_ == nullptr)
            throw std::runtime_error("the area holds no Scopes panel");
    }

    ~Workspace() {
        area_.reset();
        viewer_.reset();
        controller_->beginShutdown();
        rig_.bridge->beginShutdown();
        (void)awaitQtCondition("the scheduler drains after the rig shuts down",
                               [this] { return rig_.scheduler.isQuiescent(); });
    }

    [[nodiscard]] ui::ViewerEditor& viewer() { return *viewer_; }
    [[nodiscard]] ui::ScopesEditor& panel() { return *panel_; }
    [[nodiscard]] ui::EditorArea& area() { return *area_; }
    [[nodiscard]] ui::CompositionSession& session() { return *session_; }
    [[nodiscard]] ui::CompositionPreviewController& controller() { return *controller_; }
    [[nodiscard]] ui::ScopesRegionStore& store() { return store_; }
    [[nodiscard]] Scene& scene() { return scene_; }

    [[nodiscard]] ui::ScopesRegionStore::Key key() const {
        return {session_->snapshot().project().id(), session_->compositionId()};
    }

    // Shows both and waits for the Viewer's frame and the panel's tables and traces of it.
    void show(const int viewerWidth, const int panelWidth, const int panelHeight) {
        viewer_->resize(viewerWidth, viewerWidth * 11 / 20);
        viewer_->show();
        area_->resize(panelWidth, panelHeight);
        area_->show();
        panel_->followViewer(viewer_.get());
        settle();
    }

    // True when the Viewer's frame carries tables of the session's settings, the panel shows them
    // and every trace on show was mapped from them, with nothing still running.
    [[nodiscard]] bool settled() const {
        const auto& state = controller_->state();
        if (state.activity != ui::PreviewActivity::Ready || state.frame == nullptr ||
            !viewer_->mappingForTest().has_value())
            return false;
        const auto& scopes = state.frame->scopes();
        if (scopes == nullptr || scopes->settings != store_.settings(key()) ||
            panel_->scopes() != scopes || panel_->renderer()->busy())
            return false;
        for (std::size_t kind = 0; kind < ui::scopes::kKindCount; ++kind) {
            const auto finished = panel_->renderer()->finished(static_cast<ui::scopes::Kind>(kind));
            if (finished != nullptr && finished->request.tables.get() != &scopes->tables)
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
            const auto& frame = controller_->state().frame;
            throw std::runtime_error(
                "the panel did not settle on the Viewer's frame: preview activity " +
                std::to_string(static_cast<int>(controller_->state().activity)) + ", frame " +
                std::to_string(frame != nullptr) + ", tables " +
                std::to_string(frame != nullptr && frame->scopes() != nullptr) + ", shown " +
                std::to_string(panel_->scopes() != nullptr));
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
    ui::EditorRegistry registry_;
    std::unique_ptr<ui::EditorArea> area_;
    ui::ScopesEditor* panel_ = nullptr;
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
            QStringLiteral("/730-%1-w%2-dpr%3.png").arg(what, QString::number(width), label);
        if (image.isNull() || !image.save(path)) {
            std::cerr << "cannot write " << path.toStdString() << '\n';
            ++failures;
            return;
        }
        std::cout << path.toStdString() << ' ' << image.width() << 'x' << image.height() << '\n';
    }
};

// The panel's area (its header with the menus, then the panes) at `width`, as tall as its content
// wants, or `height` when that is given.
QImage grabPanel(Workspace& work, const int width, const int height = 0) {
    auto& area = work.area();
    area.resize(width, 4000);
    work.awaitTraces();
    int wanted = height;
    if (wanted == 0) {
        auto* scroll = work.panel().findChild<QScrollArea*>(QStringLiteral("scopesScroll"));
        auto* content = scroll != nullptr ? scroll->widget() : nullptr;
        if (content == nullptr || content->layout() == nullptr)
            throw std::runtime_error("the panel's parts are missing");
        const int contentHeight = content->layout()->totalHeightForWidth(
            scroll->viewport()->width() > 0 ? scroll->viewport()->width() : width);
        const int above = work.panel().mapTo(&area, QPoint(0, 0)).y();
        wanted = above + std::max(contentHeight, content->sizeHint().height()) + 4;
    }
    area.resize(width, wanted);
    work.awaitTraces();
    return area.grab().toImage();
}

// A header menu, open, as the area shows it below its button.
QImage grabMenu(Workspace& work, QMenu* menu) {
    if (menu == nullptr)
        throw std::runtime_error("the header menu is missing");
    menu->popup(work.area().mapToGlobal(QPoint(0, 0)));
    for (int turn = 0; turn < 10; ++turn)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    const auto image = menu->grab().toImage();
    menu->hide();
    return image;
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
    const QString photograph = argc > 3 ? QString::fromLocal8Bit(argv[3]) : QString();
    QTemporaryDir scratch;
    const auto pictureScene = [&] {
        return photograph.isEmpty() ? sunsetScene() : photoScene(photograph, scratch.path());
    };
    try {
        Workspace probe(barsScene());
        probe.show(900, 400, 900);
        const int narrow = probe.area().minimumSizeHint().width();
        std::cout << "panel minimum width " << narrow << '\n';
        const std::array<int, 3> widths{2000, 900, narrow};

        // The Viewer with the region (and the region of interest, for contrast).
        // The Viewer's own minimum width is wider than the panel's, so its narrow case is 320.
        for (const int width : {2000, 900, 320}) {
            Workspace work(barsScene());
            work.show(width, std::max(width / 3, narrow), 900);
            work.store().setRegion(work.key(), runtime::ScopeRegion{120, 70, 320, 200});
            ui::ViewerEditor::RemoteChange change;
            change.regionOfInterest = std::optional<QRectF>(QRectF(300.0, 140.0, 280.0, 170.0));
            const auto refusal = work.viewer().applyRemoteChange(change);
            if (!refusal.isEmpty())
                throw std::runtime_error(refusal.toStdString());
            work.settle();
            out.save(grabViewer(work), QStringLiteral("viewer-region-and-roi"), width);
        }

        // The header menus, open.
        {
            Workspace work(barsScene());
            work.show(900, 600, 900);
            out.save(grabMenu(work, work.panel().signalMenu()), QStringLiteral("menu-signal"), 600);
            out.save(grabMenu(work, work.panel().regionMenu()), QStringLiteral("menu-region"), 600);
            out.save(grabMenu(work, work.panel().scopesMenu()), QStringLiteral("menu-scopes"), 600);
        }

        for (const int width : widths) {
            // The panel with all five panes open, on bars and on the photograph.
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
                Workspace work(pictureScene());
                work.show(900, width, 900);
                out.save(grabPanel(work, width, width == narrow ? 800 : 0),
                         QStringLiteral("panel-photo"), width);
            }
            // The Working signal.
            {
                Workspace work(barsScene());
                work.show(900, width, 900);
                work.store().setSignal(runtime::ScopeSignal::Working);
                work.settle();
                out.save(grabPanel(work, width), QStringLiteral("panel-working-linear"), width);
            }
            // A region outside the picture: nothing to count, the empty graticule, silently.
            {
                Workspace work(barsScene());
                work.show(900, width, 900);
                work.store().setRegion(work.key(), runtime::ScopeRegion{700, 10, 20, 20});
                const bool empty = awaitQtCondition("the frame counts nothing in the region", [&] {
                    const auto& frame = work.controller().state().frame;
                    return frame != nullptr && frame->scopes() != nullptr &&
                           frame->scopes()->status ==
                               runtime::PreviewFrameScopesStatus::EmptyArea &&
                           work.panel().scopes() == frame->scopes();
                });
                if (!empty)
                    throw std::runtime_error("the empty region never reached the panel");
                for (int turn = 0; turn < 10; ++turn)
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                out.save(grabPanel(work, width), QStringLiteral("panel-empty-region"), width);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "capture failed: " << error.what() << '\n';
        return 1;
    }
    return out.failures + awaitFailureCount() == 0 ? 0 : 1;
}
