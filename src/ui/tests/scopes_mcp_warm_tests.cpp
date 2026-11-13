// bloom.ui.scopes-mcp-warm (#724 Scopes): the application's side of the MCP and Python scopes read.
//
// The real McpLiveUiBinding, the real CompositionPreviewController and the shared PreviewFrameCache
// of a 64 x 64 composition (an SVG imported as shapes: a red half and, inside a group, a blue half)
// answer the scripting read's warm-sidecar port, with the CPU reference as the only measure route.
// Asserted:
//   * a frame the preview shows, with a sidecar attached under the identity a default read would
//     measure, is answered from the cache with exactly the sidecar's tables and with no measure
//     run (the CPU route is counted);
//   * the same read is measured when the scope settings differ (signal, region, Ignore
//     Transparent Pixels, the scopes asked for, the axis maximum), when the resolution differs,
//     when the display and view differ, when the composition read is not the one the Viewer shows,
//     and when the Viewer has a region of interest, an isolation, a channel view or looks off;
//   * a binding with no preview or cache measures everything;
//   * the warm answer equals a cold measure of the same frame, cell for cell.
// The port is called from the read's worker and marshaled to the interface thread, as the MCP
// service does.

#include <bloom/commands/operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/drill_levels.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_preparation.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/scope_read.hpp>
#include <bloom/scripting/session.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/mcp_live_ui_binding.hpp>
#include <bloom/ui/playback_controller.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QSettings>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using namespace std::chrono_literals;

int gFailures = 0;

void expect(const bool condition, const std::string& message,
            const std::source_location location = std::source_location::current()) {
    if (!condition) {
        ++gFailures;
        std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
    }
}

template <typename Predicate> bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 60000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate())
            return true;
        std::this_thread::sleep_for(1ms);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

constexpr auto kArtwork =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
<rect id="left" x="0" y="0" width="32" height="64" fill="#cc3333"/>
<g id="grp"><rect id="right" x="32" y="0" width="32" height="64" fill="#3366cc"/></g>
</svg>
)svg";

[[nodiscard]] core::RationalTime frameTime(const std::uint64_t frame) {
    return *core::RationalTime::create(static_cast<std::int64_t>(frame), 24);
}

struct Project final {
    std::unique_ptr<scripting::Session> scripting;
    document::CompositionId composition;
    document::CompositionId other;
    document::LayerId root;
    document::LayerId group;
};

[[nodiscard]] Project author() {
    const auto format = document::CompositionFormat::create(
        64, 64, core::PixelAspectRatio::square(), *document::FrameRate::create(24, 1));
    auto created =
        scripting::Session::createNew("Warm", "Main", core::RationalTime::fromInteger(1), *format);
    if (!created)
        throw std::runtime_error("the session is not created");
    Project project;
    project.scripting = std::move(created).takeSession();
    project.composition = project.scripting->snapshot().project().compositions().front().id();
    const auto run = [&](commands::Transaction transaction) {
        auto result = project.scripting->execute(std::move(transaction));
        if (!result.succeeded())
            throw std::runtime_error("a fixture command was refused");
        return std::move(*result.command);
    };
    {
        const auto size = std::char_traits<char>::length(kArtwork);
        auto bytes = std::make_shared<std::vector<std::byte>>(size);
        std::memcpy(bytes->data(), kArtwork, size);
        auto prepared = commands::prepareSvgSource(std::move(bytes), "warm.svg");
        if (!prepared.source)
            throw std::runtime_error("the artwork is not prepared");
        commands::Transaction transaction("Artwork", project.scripting->snapshot().revision());
        transaction.emplace<commands::AddSvgArtwork>(project.composition, prepared.source, "Art",
                                                     commands::SvgImportMode::Shapes, true);
        const auto layer = run(std::move(transaction)).outputId<document::LayerId>("layer");
        if (!layer)
            throw std::runtime_error("the artwork has no layer");
        project.root = *layer;
    }
    const auto& current =
        *project.scripting->snapshot().project().findComposition(project.composition);
    if (const auto merge = document::layerContentMerge(current, project.root))
        for (const auto layer : document::mergeEntryLayers(current, *merge))
            if (const auto* boundary = current.graph().findLayer(layer);
                boundary != nullptr && boundary->name == "grp")
                project.group = layer;
    if (!project.group)
        throw std::runtime_error("the group is missing");
    {
        commands::Transaction transaction("Second", project.scripting->snapshot().revision());
        transaction.emplace<commands::AddComposition>("Other", *format,
                                                      core::RationalTime::fromInteger(1));
        const auto id = run(std::move(transaction))
                            .outputId<document::CompositionId>(commands::kAddCompositionOutput);
        if (!id)
            throw std::runtime_error("the second composition is missing");
        project.other = *id;
    }
    return project;
}

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
    ~Rig() {
        bridge->beginShutdown();
        scheduler.beginShutdown();
        waitUntil([this] { return scheduler.isQuiescent(); });
    }
};

class Warm final {
  public:
    Warm()
        : project_(author()), session_(*project_.scripting->document(),
                                       *project_.scripting->commandStack(), project_.composition) {
        // As the application does: the session holds the project's color settings and the
        // controller's settings carry the session's color intent.
        if (const auto* projectSettings = project_.scripting->hostSession()->colorSettings())
            session_.setColorSettings(*projectSettings);
        cache_ = std::make_shared<ui::PreviewFrameCache>(ui::kMinimumPreviewFrameCacheByteBudget);
        ui::CompositionPreviewSettings settings;
        settings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
        settings.colorIntent = session_.colorIntent();
        controller_ = std::make_unique<ui::CompositionPreviewController>(
            session_, rig_.scheduler, *rig_.bridge,
            ui::makeCompositionPreviewPipeline(*rig_.compiler, rig_.evaluator, rig_.displayPreparer,
                                               rig_.qualified),
            settings, cache_);
        host_ = std::make_unique<ui::ProjectHost>(rig_.scheduler);
        binding_ = std::make_unique<ui::McpLiveUiBinding>(*host_, session_,
                                                          controller_->playbackController());
        routes_.evaluateCpu = [this](std::shared_ptr<const runtime::CompiledCompositionPlan> plan,
                                     const runtime::EvaluationRequest& request,
                                     const runtime::CancellationToken& token) {
            ++measures_;
            return cpu_.evaluate(std::move(plan), request, token);
        };
        binding_->setScopes(routes_, controller_.get(), cache_);
    }
    ~Warm() {
        controller_->beginShutdown();
        rig_.bridge->beginShutdown();
    }

    [[nodiscard]] const Project& project() const { return project_; }
    [[nodiscard]] ui::CompositionSession& session() { return session_; }
    [[nodiscard]] ui::CompositionPreviewController& controller() { return *controller_; }
    [[nodiscard]] ui::PreviewFrameCache& cache() { return *cache_; }
    [[nodiscard]] int measures() const { return measures_.load(); }

    // The preview shows the frame at 0 and the cache holds it under the controller's own key.
    [[nodiscard]] std::optional<ui::PreviewFrameCacheKey> shown() {
        std::optional<ui::PreviewFrameCacheKey> key;
        const bool ready = waitUntil([&] {
            key = controller_->cacheKeyForTime(frameTime(0));
            return controller_->state().activity == ui::PreviewActivity::Ready && key &&
                   cache_->contains(*key);
        });
        return ready ? key : std::nullopt;
    }

    // One read of frame 0, run on its own thread (as the MCP service does) while this thread runs
    // the interface events; the warm port is marshaled to this thread.
    [[nodiscard]] scripting::ScopeReadResult read(scripting::ScopeReadRequest request,
                                                  const bool warm = true) {
        if (request.composition.value() == 0)
            request.composition = project_.composition;
        request.firstFrame = 0;
        request.lastFrame = 0;
        scripting::ScopeReadPorts ports;
        ports.scheduler = &rig_.scheduler;
        ports.routes = binding_->scopeRoutes();
        if (warm)
            ports.warm = [this](const scripting::ScopeWarmQuery& query) {
                std::optional<scripting::ScopeWarmHit> hit;
                QMetaObject::invokeMethod(
                    binding_.get(), [&] { hit = binding_->warmScopes(query); },
                    Qt::BlockingQueuedConnection);
                return hit;
            };
        scripting::ScopeReadResult result;
        std::atomic<bool> finished = false;
        std::thread worker([&] {
            result = scripting::readScopes(*project_.scripting, request, ports);
            finished = true;
        });
        waitUntil([&] { return finished.load(); });
        worker.join();
        return result;
    }

    // A read with no warm port: the frame's genuine tables, measured by the CPU reference.
    [[nodiscard]] scripting::ScopeReadFrame cold(const scripting::ScopeReadRequest& request) {
        const auto result = read(request, false);
        expect(result.ok() && result.frames.size() == 1, "a cold read measures the frame");
        return result.ok() && result.frames.size() == 1 ? result.frames.front()
                                                        : scripting::ScopeReadFrame{};
    }

    // Attaches `frame`'s tables as the sidecar of the live entry `key`, under the identity of a
    // read that asked for `settings`.
    [[nodiscard]] bool attach(const ui::PreviewFrameCacheKey& key,
                              const scripting::ScopeReadFrame& frame,
                              const runtime::ScopeMeasureSettings& settings) {
        auto sidecar = std::make_shared<ui::PreviewScopeSidecar>();
        sidecar->identity = ui::PreviewScopeIdentity{settings, {}};
        sidecar->tables = runtime::ScopeTables::pack(frame.counts, frame.route);
        sidecar->area = frame.area;
        return cache_->attachScopes(key, std::move(sidecar));
    }

    [[nodiscard]] ui::McpLiveUiBinding& binding() { return *binding_; }
    [[nodiscard]] runtime::ScopeFrameRoutes routes() const { return routes_; }
    [[nodiscard]] Rig& rig() { return rig_; }

  private:
    Rig rig_;
    Project project_;
    ui::CompositionSession session_;
    std::shared_ptr<ui::PreviewFrameCache> cache_;
    std::unique_ptr<ui::CompositionPreviewController> controller_;
    std::unique_ptr<ui::ProjectHost> host_;
    std::unique_ptr<ui::McpLiveUiBinding> binding_;
    runtime::CpuCompositionEvaluator cpu_;
    runtime::ScopeFrameRoutes routes_;
    std::atomic<int> measures_ = 0;
};

[[nodiscard]] scripting::ScopeReadRequest defaultRead() { return {}; }

[[nodiscard]] runtime::ScopeMeasureSettings defaultSettings() { return {}; }

// A read that must be measured, not answered from the cache, with the genuine tables.
void expectMeasured(Warm& warm, scripting::ScopeReadRequest request,
                    const scripting::ScopeReadFrame& genuine, const std::string& what,
                    const bool compareToDefault = true) {
    const int before = warm.measures();
    const auto result = warm.read(std::move(request));
    expect(result.ok() && result.frames.size() == 1, what + ": the read answers");
    if (!result.ok() || result.frames.size() != 1)
        return;
    const auto& frame = result.frames.front();
    expect(!frame.fromCache, what + ": is measured, not answered from the cache");
    expect(warm.measures() > before, what + ": the CPU route ran");
    if (compareToDefault)
        expect(frame.counts == genuine.counts,
               what + ": the measure is of the whole frame, not the sidecar's tables");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName("BloomScopesWarm");
    QCoreApplication::setApplicationName("ScopesWarm");
    try {
        Warm warm;
        auto key = warm.shown();
        expect(key.has_value(), "the preview shows the frame and the cache holds it");
        if (!key)
            return 1;

        // The genuine tables of the whole frame, and a different picture's: the left half only.
        const auto genuine = warm.cold(defaultRead());
        auto leftRead = defaultRead();
        leftRead.region = runtime::ScopeRegion{0, 0, 32, 64};
        const auto leftHalf = warm.cold(leftRead);
        expect(!genuine.counts.words.empty() && genuine.counted == 64U * 64U &&
                   leftHalf.counted == 32U * 64U && !(genuine.counts == leftHalf.counts),
               "the whole frame and its left half count 4096 and 2048 pixels");

        // Without a sidecar the frame is measured.
        expectMeasured(warm, defaultRead(), genuine, "no sidecar");

        // A sidecar under the default identity, holding the left half's tables, is the answer.
        expect(warm.attach(*key, leftHalf, defaultSettings()), "the sidecar is attached");
        {
            const int before = warm.measures();
            const auto result = warm.read(defaultRead());
            expect(result.ok() && result.frames.size() == 1, "the warm read answers");
            if (result.ok() && result.frames.size() == 1) {
                const auto& frame = result.frames.front();
                expect(frame.fromCache, "a shown frame with a sidecar is answered from the cache");
                expect(frame.counts == leftHalf.counts && frame.counted == leftHalf.counted &&
                           frame.area == leftHalf.area && frame.route == leftHalf.route,
                       "its tables, area and route are the sidecar's");
                expect(warm.measures() == before, "no measure ran");
            }
        }
        // The sidecar's tables are the frame's own: the warm answer equals a cold measure.
        expect(warm.attach(*key, genuine, defaultSettings()), "the genuine sidecar is attached");
        {
            const int before = warm.measures();
            const auto result = warm.read(defaultRead());
            expect(result.ok() && result.frames.size() == 1 && result.frames.front().fromCache &&
                       result.frames.front().counts == genuine.counts &&
                       result.frames.front().area == genuine.area,
                   "the warm answer equals a cold measure of the frame, cell for cell");
            expect(warm.measures() == before, "the genuine sidecar needed no measure either");
        }
        // A summary measures the Histogram and the Vectorscope alone, so it is another identity
        // than the sidecar of all five tables; a sidecar of those two answers it.
        {
            auto request = defaultRead();
            request.output = scripting::ScopeReadOutput::Summary;
            expectMeasured(warm, request, genuine, "a summary read against a sidecar of all five",
                           false);
            auto summaryTables = defaultRead();
            summaryTables.scopes = render::kScopeTableHistogram | render::kScopeTableVectorscope;
            const auto summaryFrame = warm.cold(summaryTables);
            auto summarySettings = defaultSettings();
            summarySettings.scopes = summaryTables.scopes;
            expect(warm.attach(*key, summaryFrame, summarySettings),
                   "a sidecar of the summary's two tables is attached");
            const auto result = warm.read(request);
            expect(result.ok() && result.frames.size() == 1 && result.frames.front().fromCache &&
                       result.frames.front().summary.has_value(),
                   "a summary is answered from the sidecar of its two tables");
            expect(warm.attach(*key, genuine, defaultSettings()), "the full sidecar is back");
        }

        // The scope settings: each one differs from the sidecar's identity.
        {
            auto request = defaultRead();
            request.signal = runtime::ScopeSignal::Working;
            expectMeasured(warm, request, genuine, "a Working signal read", false);
            request = defaultRead();
            request.region = runtime::ScopeRegion{0, 0, 32, 64};
            expectMeasured(warm, request, leftHalf, "a read with a region");
            request = defaultRead();
            request.ignoreTransparent = true;
            expectMeasured(warm, request, genuine, "a read that ignores transparent pixels", false);
            request = defaultRead();
            request.scopes = render::kScopeTableHistogram;
            expectMeasured(warm, request, genuine, "a read of the Histogram alone", false);
            // The axis maximum of the Working signal: a sidecar of the Working signal at 1 does
            // not answer a read at 4.
            auto working = defaultSettings();
            working.signal = runtime::ScopeSignal::Working;
            auto workingRead = defaultRead();
            workingRead.signal = runtime::ScopeSignal::Working;
            const auto workingFrame = warm.cold(workingRead);
            expect(warm.attach(*key, workingFrame, working), "a Working sidecar is attached");
            workingRead.axisMaximum = 4;
            expectMeasured(warm, workingRead, workingFrame, "a Working read at another maximum",
                           false);
            workingRead.axisMaximum = 1;
            const auto answered = warm.read(workingRead);
            expect(answered.ok() && answered.frames.size() == 1 &&
                       answered.frames.front().fromCache &&
                       answered.frames.front().counts == workingFrame.counts,
                   "a Working read with the sidecar's own settings is answered from it");
            // The Display sidecar is still attached? Attaching replaces: put it back.
            expect(warm.attach(*key, genuine, defaultSettings()), "the Display sidecar is back");
        }

        // The resolution: a Half read does not take a Full frame's sidecar.
        {
            auto request = defaultRead();
            request.resolution = scripting::ScopeReadResolution::Half;
            expectMeasured(warm, request, genuine, "a half resolution read", false);
        }

        // The display and view. A read that names the pair measures; a Viewer that chose another
        // pair is another frame; a Viewer that chose the very pair the read names is the same.
        {
            const auto defaultRead_ = warm.read(defaultRead());
            const std::string display = defaultRead_.ok() ? defaultRead_.color.display : "";
            const std::string view = defaultRead_.ok() ? defaultRead_.color.view : "";
            expect(!display.empty() && !view.empty(), "the read reports the pair it measured");
            auto named = defaultRead();
            named.display = display;
            named.view = view;
            expectMeasured(warm, named, genuine,
                           "a read that names a pair the Viewer has not chosen", true);
            warm.controller().setViewerDisplayView("Review Display", "Review View");
            auto other = warm.shown();
            expect(other.has_value(), "the preview shows the frame under another display and view");
            if (other) {
                expect(warm.attach(*other, genuine, defaultSettings()), "its sidecar is attached");
                expectMeasured(warm, defaultRead(), genuine,
                               "a Viewer on another display and view");
                // The Working signal never goes through the display: that sidecar answers.
                auto working = defaultSettings();
                working.signal = runtime::ScopeSignal::Working;
                auto workingRead = defaultRead();
                workingRead.signal = runtime::ScopeSignal::Working;
                const auto workingFrame = warm.cold(workingRead);
                expect(warm.attach(*other, workingFrame, working), "a Working sidecar is attached");
                const auto answered = warm.read(workingRead);
                expect(answered.ok() && answered.frames.size() == 1 &&
                           answered.frames.front().fromCache,
                       "the Working signal does not depend on the Viewer's display and view");
            }
            warm.controller().setViewerDisplayView(display, view);
            auto chosen = warm.shown();
            expect(chosen.has_value(), "the preview shows the frame under the read's own pair");
            if (chosen) {
                expect(warm.attach(*chosen, genuine, defaultSettings()), "its sidecar is attached");
                const auto result = warm.read(named);
                expect(result.ok() && result.frames.size() == 1 && result.frames.front().fromCache,
                       "a read that names the pair the Viewer chose is answered from the cache");
            }
            warm.controller().setViewerDisplayView({}, {});
            key = warm.shown();
        }

        // The Viewer's own inspection: each makes the shown frame another picture than the read's.
        const auto viewerState = [&](const std::string& what, const auto& set, const auto& reset) {
            set();
            const auto shown = warm.shown();
            expect(shown.has_value(), what + ": the preview shows the frame");
            if (shown) {
                expect(warm.attach(*shown, genuine, defaultSettings()),
                       what + ": its sidecar is attached");
                expectMeasured(warm, defaultRead(), genuine, what);
            }
            reset();
            key = warm.shown();
        };
        viewerState(
            "a Viewer with a region of interest",
            [&] { warm.controller().setRegionOfInterest(QRectF(8.0, 8.0, 32.0, 32.0)); },
            [&] { warm.controller().setRegionOfInterest(std::nullopt); });
        viewerState(
            "a Viewer inside a group",
            [&] {
                expect(
                    warm.session()
                        .setViewerLevel({{warm.project().root, warm.project().group}, std::nullopt})
                        .ok(),
                    "the Viewer enters the group");
            },
            [&] { static_cast<void>(warm.session().setViewerLevel({})); });
        viewerState(
            "a Viewer with looks off", [&] { warm.controller().setViewerLookEnabled(false); },
            [&] { warm.controller().setViewerLookEnabled(true); });
        {
            // A channel view needs a node that produces a channel set; this composition has none,
            // so the Viewer cannot show a frame under that key and the cache holds no entry for
            // it. The key still carries the inspection, and the read is measured.
            const auto output = warm.session().composition()->graph().compositionOutput().value();
            warm.controller().setViewerChannelInspection(
                runtime::ChannelInspection{output.nodeId, output.port, {"R", "G", "B"}});
            const auto inspected = warm.controller().cacheKeyForTime(frameTime(0));
            expect(inspected && inspected->channelInspection.has_value() &&
                       !warm.cache().contains(*inspected),
                   "a channel view is part of the Viewer's key and has no cached frame here");
            expectMeasured(warm, defaultRead(), genuine, "a Viewer on a channel view");
            warm.controller().setViewerChannelInspection(std::nullopt);
            key = warm.shown();
        }

        // After all of that, the Viewer is back as it was and its sidecar answers again.
        expect(key.has_value(), "the preview shows the frame again");
        if (key) {
            expect(warm.attach(*key, genuine, defaultSettings()), "the sidecar is attached again");
            const auto result = warm.read(defaultRead());
            expect(result.ok() && result.frames.size() == 1 && result.frames.front().fromCache &&
                       result.frames.front().counts == genuine.counts,
                   "with the Viewer as it was, the sidecar answers again");
        }

        // A read of another composition than the one the Viewer shows is measured.
        {
            auto request = defaultRead();
            request.composition = warm.project().other;
            const int before = warm.measures();
            const auto result = warm.read(request);
            expect(result.ok() && result.frames.size() == 1 && !result.frames.front().fromCache &&
                       warm.measures() > before,
                   "a read of the other composition is measured");
        }

        // A binding that was never given a preview and cache measures every frame.
        {
            ui::ProjectHost host(warm.rig().scheduler);
            ui::McpLiveUiBinding unbound(host, warm.session(),
                                         warm.controller().playbackController());
            scripting::ScopeWarmQuery query;
            query.composition = warm.project().composition;
            query.time = frameTime(0);
            query.resolution = runtime::CompositionFormatResolution{};
            query.colorIntent = warm.session().colorIntent();
            query.settings = defaultSettings();
            expect(!unbound.warmScopes(query).has_value(),
                   "a binding with no preview and cache answers nothing");
            // The same query on the real binding: the sidecar is there and answers.
            expect(warm.binding().warmScopes(query).has_value(),
                   "the same query on the bound binding answers from the cache");
        }
    } catch (const std::exception& error) {
        expect(false, std::string("unexpected exception: ") + error.what());
    }
    if (gFailures != 0) {
        std::cerr << "scopes mcp warm: " << gFailures << " failure(s)\n";
        return 1;
    }
    std::cout << "scopes mcp warm: ok\n";
    return 0;
}
