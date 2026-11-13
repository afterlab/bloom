// #645 isolation in the real Viewer route (docs/user-guide/viewer.md, "Drilling into groups").
// While the Viewer is inside a group every layer outside it draws at 30% of its own opacity. The
// dim state is part of the preview request and of every cache identity, and nothing but the
// Viewer's own frames ever sees it. On a real document (an SVG imported as shapes, three nested
// groups, beside a one-layer SVG; an outside layer's opacity and the badge's position keyed), with
// the real CompositionPreviewController, ViewerEditor and shared PreviewFrameCache:
//   * entering a group re-renders the Viewer frame with the outside dimmed (alpha 0.3 of the
//     undimmed frame's, within one code) and the entered group's pixels untouched, over three
//     frames; the frame's identity names the level;
//   * leaving shows the undimmed frame again from the cache with no new preparation, and entering
//     again shows the dimmed one from the cache: neither is ever served for the other;
//   * a RAM Preview filled inside caches only dimmed frames: none of them is found outside, where
//     the frames are prepared again undimmed; filled outside, none is found inside;
//   * a still, a PNG range and a scripted render (the route CLI, Python and MCP renders take) made
//     while the Viewer is inside equal the undimmed Viewer frames;
//   * on the GPU route (the GPU-prepared packed product, when --loader names a Vulkan loader) the
//     dimmed Viewer frames come from the device with no CPU fallback and equal the CPU route's
//     (RGB within one code, exact alpha), and a dimmed Viewer frame at 1920 x 1080 and 3840 x 2160
//     renders on the device with no fallback.
// The CPU route always runs; the GPU route prints NotRun without a loader or device, and
// --require-device makes that a failure.

#include "await_qt.hpp"
#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/svg_import_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/drill_levels.hpp>
#include <bloom/document/merge_entries.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#if defined(BLOOM_UI_ISOLATION_GPU)
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/ui/gpu_viewer_bootstrap.hpp>
#endif

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using namespace std::chrono_literals;

class Expectations final {
  public:
    void expect(const bool ok, const std::string& message) {
        if (ok)
            return;
        ++failures_;
        std::cerr << "FAIL: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

bool gRequireDevice = false;

constexpr std::uint32_t kWidth = 512;
constexpr std::uint32_t kHeight = 288;
constexpr std::int64_t kFps = 24;
constexpr std::uint64_t kFrames = 3;

constexpr auto kArtwork =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="288" viewBox="0 0 512 288">
<defs>
<linearGradient id="fade"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient>
<clipPath id="window"><rect x="300" y="200" width="40" height="40"/></clipPath>
</defs>
<rect id="backdrop" x="0" y="0" width="60" height="60" fill="#808080"/>
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

constexpr auto kBadge =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="288" viewBox="0 0 512 288">
<rect id="badge" x="430" y="200" width="50" height="50" fill="#33cc66"/>
</svg>
)svg";

struct Pixels final {
    bool ok = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; // Straight RGBA8, row-major.
    [[nodiscard]] std::array<int, 4> at(const std::uint32_t x, const std::uint32_t y) const {
        const auto index = (static_cast<std::size_t>(y) * width + x) * 4U;
        return {rgba[index], rgba[index + 1], rgba[index + 2], rgba[index + 3]};
    }
};

[[nodiscard]] Pixels pixelsOf(const ui::PreparedPreviewFrameHandle& frame) {
    Pixels out;
    const auto view = frame != nullptr ? frame->displayBufferView() : std::nullopt;
    if (!view.has_value() ||
        view->layout.rowStrideBytes !=
            static_cast<std::size_t>(view->displayWindow.extent().width()) * 4U)
        return out;
    out.width = view->displayWindow.extent().width();
    out.height = view->displayWindow.extent().height();
    if (view->pixels.size() != static_cast<std::size_t>(out.width) * out.height)
        return out;
    out.rgba.reserve(view->pixels.size() * 4U);
    for (const auto& pixel : view->pixels)
        out.rgba.insert(out.rgba.end(), {pixel.red, pixel.green, pixel.blue, pixel.alpha});
    out.ok = true;
    return out;
}

[[nodiscard]] Pixels decodePng(const std::filesystem::path& path) {
    Pixels out;
    QImage image(QString::fromStdString(path.string()));
    if (image.isNull())
        return out;
    image = image.convertToFormat(QImage::Format_RGBA8888);
    out.width = static_cast<std::uint32_t>(image.width());
    out.height = static_cast<std::uint32_t>(image.height());
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4U);
    for (int y = 0; y < image.height(); ++y)
        std::copy_n(image.constScanLine(y), static_cast<std::size_t>(image.width()) * 4U,
                    out.rgba.begin() + static_cast<std::ptrdiff_t>(y) * image.width() * 4);
    out.ok = true;
    return out;
}

// RGB within one 8-bit code, alpha exactly.
[[nodiscard]] bool framesMatch(const Pixels& a, const Pixels& b) {
    if (!a.ok || !b.ok || a.width != b.width || a.height != b.height)
        return false;
    for (std::size_t index = 0; index < a.rgba.size(); ++index) {
        const int difference =
            std::abs(static_cast<int>(a.rgba[index]) - static_cast<int>(b.rgba[index]));
        if (index % 4U == 3U ? difference != 0 : difference > 1)
            return false;
    }
    return true;
}

[[nodiscard]] std::shared_ptr<const commands::PreparedSvgSource> prepared(const char* text,
                                                                         const char* file) {
    const auto size = std::strlen(text);
    auto bytes = std::make_shared<std::vector<std::byte>>(size);
    std::memcpy(bytes->data(), text, size);
    auto read = commands::prepareSvgSource(std::move(bytes), file);
    if (!read.source)
        throw std::runtime_error("the SVG could not be prepared: " + read.diagnostic);
    return read.source;
}

[[nodiscard]] core::RationalTime frameTime(const std::uint64_t frame) {
    return *core::RationalTime::create(static_cast<std::int64_t>(frame), kFps);
}

struct Authored final {
    std::unique_ptr<scripting::Session> scripting;
    std::unique_ptr<ui::CompositionSession> session;
    document::CompositionId composition;
    document::LayerId root, badge, backdrop, outer, ring;

    [[nodiscard]] const document::Composition& current() const {
        return *scripting->snapshot().project().findComposition(composition);
    }
    commands::CommandResult run(commands::Transaction transaction) {
        return scripting->commandStack()->execute(std::move(transaction));
    }
    document::LayerId import(const char* text, const commands::SvgImportMode mode,
                             const std::string& name) {
        commands::Transaction transaction("Import " + name, scripting->snapshot().revision());
        transaction.emplace<commands::AddSvgArtwork>(composition, prepared(text, "isolation.svg"),
                                                     name, mode, true);
        const auto result = run(std::move(transaction));
        const auto layer = result.outputId<document::LayerId>("layer");
        if (!result.succeeded() || !layer)
            throw std::runtime_error("the SVG import failed: " + name);
        return *layer;
    }
    [[nodiscard]] document::LayerId child(const document::LayerId parent,
                                          const std::string_view name) const {
        if (const auto merge = document::layerContentMerge(current(), parent))
            for (const auto layer : document::mergeEntryLayers(current(), *merge))
                if (const auto* boundary = current().graph().findLayer(layer);
                    boundary != nullptr && boundary->name == name)
                    return layer;
        throw std::runtime_error("no layer named " + std::string(name));
    }
    [[nodiscard]] document::ParameterId role(const document::LayerId layer,
                                             const std::string_view name) const {
        const auto* boundary = current().graph().findLayer(layer);
        const auto* binding =
            boundary ? document::layerParameterBinding(current().graph(), *boundary, name) : nullptr;
        if (binding == nullptr)
            throw std::runtime_error("the layer has no " + std::string(name));
        return binding->parameterId;
    }
    template <typename Value>
    void key(const document::LayerId layer, const std::string_view name, const std::uint64_t frame,
             const Value value) {
        commands::Transaction transaction("Key", scripting->snapshot().revision());
        transaction.emplace<commands::SetKeyframeAtTimeForParameter>(composition, role(layer, name),
                                                                     frameTime(frame), value);
        if (!run(std::move(transaction)).succeeded())
            throw std::runtime_error("the key was refused");
    }
};

[[nodiscard]] std::unique_ptr<Authored> author(const std::uint32_t width,
                                               const std::uint32_t height) {
    const auto format = document::CompositionFormat::create(
        width, height, core::PixelAspectRatio::square(), *document::FrameRate::create(kFps, 1));
    auto created = scripting::Session::createNew("Isolation", "Main",
                                                 frameTime(kFrames), *format);
    if (!created)
        throw std::runtime_error("the authoring session is not created");
    auto authored = std::make_unique<Authored>();
    authored->scripting = std::move(created).takeSession();
    authored->composition = authored->scripting->snapshot().project().compositions().front().id();
    authored->session = std::make_unique<ui::CompositionSession>(
        *authored->scripting->document(), *authored->scripting->commandStack(),
        authored->composition);
    authored->root = authored->import(kArtwork, commands::SvgImportMode::Shapes, "Art");
    authored->badge = authored->import(kBadge, commands::SvgImportMode::Layer, "Badge");
    authored->backdrop = authored->child(authored->root, "backdrop");
    authored->outer = authored->child(authored->root, "outer");
    authored->ring = authored->child(
        authored->child(authored->child(authored->outer, "middle"), "inner"), "ring");
    // A first key holds the constant's own value, so the last key goes in first.
    authored->key(authored->backdrop, document::kOpacityParameterRole, kFrames - 1, 0.5);
    authored->key(authored->backdrop, document::kOpacityParameterRole, 0, 1.0);
    const auto* record = authored->current().parameters().find(
        authored->role(authored->badge, document::kPositionParameterRole));
    const auto* constant =
        record ? std::get_if<document::ConstantValueSource>(&record->source) : nullptr;
    const auto* at = constant ? std::get_if<document::Vec2d>(&constant->value) : nullptr;
    if (at == nullptr)
        throw std::runtime_error("the badge's position is not a constant");
    authored->key(authored->badge, document::kPositionParameterRole, kFrames - 1,
                  document::Vec2d{at->x - 12.0, at->y - 6.0});
    authored->key(authored->badge, document::kPositionParameterRole, 0, *at);
    return authored;
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
};

// One route's Viewer: the CPU pipeline, or the GPU-prepared packed product on a device.
struct Route final {
    const char* name;
    bool gpu;
};

struct RouteCounters final {
    std::uint64_t nativeDispatches = 0;
    std::uint64_t cpuFallbacks = 0;
};

struct Viewer final {
    ui::PreviewPreparationFunction pipeline;
    ui::PreviewPreparationSubmitter submitter;
    std::function<RouteCounters()> counters = [] { return RouteCounters{}; };
    runtime::PreviewDisplayProvider provider = runtime::PreviewDisplayProvider::CpuOcio;
#if defined(BLOOM_UI_ISOLATION_GPU)
    std::unique_ptr<runtime::GpuPreviewDisplayService> service;
#endif
    std::atomic<int> preparations{0};
    bool ready = true;

    ~Viewer() {
#if defined(BLOOM_UI_ISOLATION_GPU)
        if (service)
            service->beginShutdown();
#endif
    }
};

// Builds the route's preparation. Counts every preparation it runs, so a frame served from the
// cache is told apart from a frame prepared again.
std::unique_ptr<Viewer> viewerFor(Rig& rig, const Route& route,
                                  [[maybe_unused]] const std::filesystem::path& loader) {
    auto viewer = std::make_unique<Viewer>();
    auto cpu = ui::makeCompositionPreviewPipeline(*rig.compiler, rig.evaluator,
                                                  rig.displayPreparer, rig.qualified);
    viewer->pipeline = [cpu, count = &viewer->preparations](
                           const document::Snapshot& snapshot,
                           const runtime::PreviewRequestIdentity& identity, const std::size_t bytes,
                           const std::vector<runtime::SnapshotParameterOverride>& overrides,
                           runtime::TaskContext& context) {
        ++*count;
        return cpu(snapshot, identity, bytes, overrides, context);
    };
#if defined(BLOOM_UI_ISOLATION_GPU)
    if (route.gpu) {
        auto resolver = host::makePackagedGpuOcioResolver(
            QCoreApplication::applicationFilePath().toStdString());
        auto gpuStage = ui::makeSessionRefreshingGpuSceneStage(
            *rig.compiler, rig.evaluator, rig.qualified,
            std::make_shared<runtime::GpuSceneCoverageCache>(), nullptr, resolver);
        runtime::GpuPreviewDisplayServiceOptions options;
        options.enabled = true;
        options.loaderPath = loader;
        options.product = runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
        options.presentation = runtime::GpuPreviewDisplayServicePresentationMode::Disabled;
        options.previewByteAllowance = std::size_t{1} << 30U;
        viewer->service = std::make_unique<runtime::GpuPreviewDisplayService>(
            rig.scheduler, gpuStage,
            ui::makeCompositionPreviewCpuStage(*rig.compiler, rig.evaluator, rig.qualified),
            ui::makeCompositionPreviewCpuDisplayFallback(rig.displayPreparer), options);
        const bool initialized = bloom::test_support::awaitQtCondition(
            "the GPU preview service leaves Initializing",
            [&] {
                return viewer->service->status().state !=
                       runtime::GpuPreviewDisplayServiceState::Initializing;
            },
            90s);
        if (!initialized ||
            viewer->service->status().state != runtime::GpuPreviewDisplayServiceState::Ready) {
            std::cout << "NotRun: " << route.name << ": no qualified device ("
                      << viewer->service->status().residentDetail << ")\n";
            viewer->ready = false;
            return viewer;
        }
        viewer->submitter = [svc = viewer->service.get(), count = &viewer->preparations](
                                runtime::TaskRequest request, const document::Snapshot& snapshot,
                                const runtime::PreviewRequestIdentity& identity,
                                const std::size_t bytes,
                                const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            ++*count;
            return svc->submit(std::move(request), snapshot, identity, bytes, overrides);
        };
        viewer->counters = [svc = viewer->service.get()] {
            const auto status = svc->status().counters;
            return RouteCounters{status.nativeDispatches, status.cpuFallbacks};
        };
        viewer->provider = runtime::PreviewDisplayProvider::GpuPreparedPacked;
    }
#else
    static_cast<void>(route);
#endif
    return viewer;
}

// The CPU route's dimmed Viewer frames, kept for the GPU route to be held against.
std::array<Pixels, kFrames> gCpuDimmed;

void exerciseRoute(Expectations& checks, const Route& route, const std::filesystem::path& workDir,
                   const std::filesystem::path& loader) {
    const std::string prefix = std::string(route.name) + ": ";
    auto authored = author(kWidth, kHeight);
    auto& session = *authored->session;
    Rig rig;
    auto route_ = viewerFor(rig, route, loader);
    if (!route_->ready) {
        checks.expect(!gRequireDevice, prefix + "--require-device needs a GPU device");
        return;
    }
    auto& v = *route_;
    ui::CompositionPreviewSettings settings;
    settings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
    settings.pixelStorageByteLimit = std::size_t{1} << 30U;
    auto cache = std::make_shared<ui::PreviewFrameCache>();
    ui::CompositionPreviewController controller(session, rig.scheduler, *rig.bridge, v.pipeline,
                                                settings, cache, nullptr, v.submitter);
    ui::ViewerEditor viewer(session, controller);
    viewer.resize(560, 340);
    viewer.show();

    const std::vector chain{authored->root, authored->outer};
    const auto shownAt = [&](const std::uint64_t frame, const std::vector<document::LayerId>& level,
                             const std::string& label) {
        if (session.currentTime() != frameTime(frame))
            static_cast<void>(session.setCurrentTime(frameTime(frame)));
        const bool ready = bloom::test_support::awaitQtCondition(
            "Viewer shows a current frame from the expected route", [&] {
                const auto displayed = viewer.displayedFrameForTest();
                return controller.state().activity == ui::PreviewActivity::Ready &&
                       displayed != nullptr &&
                       displayed->desiredIdentity().time == frameTime(frame) &&
                       displayed->desiredIdentity().isolation == level &&
                       displayed->provenance().provider == v.provider &&
                       viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current;
            });
        checks.expect(ready, label + ": the Viewer shows a current frame from the expected route");
        return pixelsOf(viewer.displayedFrameForTest());
    };

    // Outside, then inside outer, at each frame.
    const auto fallbacks = v.counters().cpuFallbacks;
    std::array<Pixels, kFrames> outside;
    std::array<Pixels, kFrames> inside;
    for (std::uint64_t frame = 0; frame < kFrames; ++frame) {
        const auto tag = prefix + "frame " + std::to_string(frame);
        checks.expect(session.setViewerLevel({}).ok(), tag + ": back at the top level");
        outside.at(frame) = shownAt(frame, {}, tag + " outside");
        checks.expect(controller.isolation().empty(), tag + ": no isolation at the top level");
        checks.expect(session.setViewerLevel({chain, std::nullopt}).ok(), tag + ": enter outer");
        checks.expect(controller.isolation() == chain, tag + ": the request carries the level");
        inside.at(frame) = shownAt(frame, chain, tag + " inside outer");
        const auto& out = outside.at(frame);
        const auto& in = inside.at(frame);
        if (!out.ok || !in.ok) {
            checks.expect(false, tag + ": both frames are readable");
            return;
        }
        // The backdrop's centre (its keyed opacity) and the badge's: alpha at 0.3 of outside's.
        const auto dimmedAt = [&](const std::uint32_t x, const std::uint32_t y) {
            const auto a = out.at(x, y)[3];
            const auto b = in.at(x, y)[3];
            return a > 100 && std::abs(b - 0.3 * a) <= 1.0;
        };
        checks.expect(dimmedAt(30, 30), tag + ": the backdrop draws at 0.3 inside (" +
                                            std::to_string(out.at(30, 30)[3]) + " -> " +
                                            std::to_string(in.at(30, 30)[3]) + ")");
        const auto badgeX = static_cast<std::uint32_t>(455 - 6 * frame);
        const auto badgeY = static_cast<std::uint32_t>(225 - 3 * frame);
        checks.expect(dimmedAt(badgeX, badgeY), tag + ": the moving badge draws at 0.3 inside");
        // The ring, the triangle, the gradient fill and the clipped square: untouched.
        bool untouched = true;
        for (std::uint32_t y = 0; y < kHeight; ++y)
            for (std::uint32_t x = 0; x < kWidth; ++x) {
                const bool nearBackdrop = x <= 62 && y <= 62;
                const bool nearBadge = x >= 410 && y >= 185;
                if (!nearBackdrop && !nearBadge && out.at(x, y) != in.at(x, y))
                    untouched = false;
            }
        checks.expect(untouched, tag + ": the entered group's pixels are untouched");
        checks.expect(in.at(150, 50) == out.at(150, 50) && out.at(150, 50)[3] == 255,
                      tag + ": the ring inside the group is fully drawn in both");
        if (!route.gpu)
            gCpuDimmed.at(frame) = in;
        else if (gCpuDimmed.at(frame).ok)
            checks.expect(framesMatch(gCpuDimmed.at(frame), in),
                          tag + ": the dimmed frame from the device equals the CPU route's "
                                "(RGB within one code, exact alpha)");
    }
    checks.expect(v.counters().cpuFallbacks == fallbacks,
                  prefix + "every frame came from the expected route, none from a fallback");

    // Leaving and entering again: each picture from the cache, never the other's.
    const auto last = kFrames - 1;
    const auto before = v.preparations.load();
    checks.expect(session.setViewerLevel({}).ok(), prefix + "leave outer");
    checks.expect(framesMatch(shownAt(last, {}, prefix + "left"), outside.at(last)),
                  prefix + "leaving shows the undimmed frame");
    checks.expect(session.setViewerLevel({chain, std::nullopt}).ok(), prefix + "enter outer again");
    checks.expect(framesMatch(shownAt(last, chain, prefix + "entered again"), inside.at(last)),
                  prefix + "entering again shows the dimmed frame");
    checks.expect(v.preparations.load() == before,
                  prefix + "both came from the cache with no new preparation (" +
                      std::to_string(v.preparations.load() - before) + " new)");
    const auto keyInside = controller.cacheKeyForTime(frameTime(last));
    checks.expect(keyInside && keyInside->isolation == chain && cache->contains(*keyInside),
                  prefix + "the inside frame is cached under a key that names the level");

    // A RAM Preview filled inside is never shown outside, and the reverse.
    const auto fill = [&](const std::string& label) {
        ui::RamPreviewController ram(session, controller, rig.scheduler, *rig.bridge, v.pipeline,
                                     nullptr, v.submitter);
        ram.start();
        const bool done = bloom::test_support::awaitQtCondition(
            "RAM Preview finishes caching every frame", [&] { return !ram.isCaching(); }, 60s);
        checks.expect(done && ram.cachedFrameCount() == kFrames,
                      label + ": the RAM Preview retains every frame (" +
                          std::to_string(ram.cachedFrameCount()) + " of " +
                          std::to_string(kFrames) + ")");
        ram.beginShutdown();
    };
    const auto keysAt = [&](const std::vector<document::LayerId>& level) {
        std::vector<ui::PreviewFrameCacheKey> keys;
        const auto saved = session.viewerLevel();
        static_cast<void>(session.setViewerLevel({level, std::nullopt}));
        for (std::uint64_t frame = 0; frame < kFrames; ++frame)
            if (const auto key = controller.cacheKeyForTime(frameTime(frame)))
                keys.push_back(*key);
        static_cast<void>(session.setViewerLevel(saved));
        return keys;
    };
    for (const auto& [fillLevel, otherLevel, label] :
         {std::tuple{chain, std::vector<document::LayerId>{}, std::string("filled inside")},
          std::tuple{std::vector<document::LayerId>{}, chain, std::string("filled outside")}}) {
        const auto tag = prefix + label;
        static_cast<void>(session.setViewerLevel({}));
        checks.expect(
            bloom::test_support::awaitQtCondition(
                "preview reaches Ready",
                [&] { return controller.state().activity == ui::PreviewActivity::Ready; }),
            tag + ": the preview reaches Ready");
        cache->clear();
        checks.expect(session.setViewerLevel({fillLevel, std::nullopt}).ok(), tag + ": level set");
        checks.expect(
            bloom::test_support::awaitQtCondition(
                "preview reaches Ready",
                [&] { return controller.state().activity == ui::PreviewActivity::Ready; }),
            tag + ": the preview reaches Ready");
        fill(tag);
        const auto filled = keysAt(fillLevel);
        const auto other = keysAt(otherLevel);
        checks.expect(filled.size() == kFrames &&
                          std::ranges::all_of(filled,
                                              [&](const auto& key) {
                                                  return key.isolation == fillLevel &&
                                                         cache->contains(key);
                                              }),
                      tag + ": every frame is cached under the level it was filled at");
        checks.expect(other.size() == kFrames &&
                          std::ranges::none_of(other,
                                               [&](const auto& key) { return cache->contains(key); }),
                      tag + ": none of them is found at the other level");
        // At the other level the Viewer prepares its own picture.
        const auto prepared = v.preparations.load();
        checks.expect(session.setViewerLevel({otherLevel, std::nullopt}).ok(),
                      tag + ": go to the other level");
        const auto shown = shownAt(session.currentTime() == frameTime(last) ? last : 0, otherLevel,
                                   tag + " other level");
        const auto frame = session.currentTime() == frameTime(last) ? last : 0;
        checks.expect(framesMatch(shown, otherLevel.empty() ? outside.at(frame) : inside.at(frame)),
                      tag + ": the other level shows its own picture, prepared again");
        checks.expect(v.preparations.load() > prepared,
                      tag + ": that picture was prepared, not taken from the fill");
    }

    // While the Viewer is inside, the export and scripted render routes are undimmed.
    checks.expect(session.setViewerLevel({chain, std::nullopt}).ok(), prefix + "inside for export");
    runtime::TaskScheduler exportScheduler;
    const runtime::SnapshotCompiler exportCompiler(document::builtInNodeDefinitions());
    std::shared_ptr<host::GpuExportProvider> provider;
    {
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.enabled = route.gpu;
#if defined(BLOOM_UI_ISOLATION_GPU)
        if (route.gpu) {
            options.loaderPath = loader;
            options.ocioResolver =
                host::makePackagedGpuOcioResolver(host::currentExecutablePath());
        }
#endif
        provider = host::GpuExportProvider::create(options);
    }
    provider->prepare(exportScheduler);
    for (std::uint64_t frame = 0; frame < kFrames; ++frame) {
        const auto still = workDir / (std::string(route.name) + "-still-" +
                                      std::to_string(frame) + ".png");
        const auto result =
            scripting::Render::run(*authored->scripting, exportScheduler, exportCompiler,
                                   {.composition = authored->composition,
                                    .frame = frame,
                                    .range = std::nullopt,
                                    .preset = output::OutputPresetV1::PngRgba8SrgbV1,
                                    .destination = still},
                                   {}, provider);
        const auto tag = prefix + "still " + std::to_string(frame) + " made inside";
        checks.expect(result.succeeded, tag + " publishes: " + result.diagnostic);
        checks.expect(result.gpuEvaluatedFrames == (route.gpu ? 1U : 0U),
                      tag + (route.gpu ? " was evaluated on the device" : " took the CPU route"));
        const auto exported = decodePng(still);
        checks.expect(framesMatch(exported, outside.at(frame)),
                      tag + " equals the undimmed Viewer frame");
        checks.expect(exported.ok && exported.at(30, 30) != inside.at(frame).at(30, 30),
                      tag + " is not the dimmed picture");
    }
    const auto folder = workDir / (std::string(route.name) + "-range");
    std::filesystem::create_directories(folder);
    const auto ranged = scripting::Render::run(
        *authored->scripting, exportScheduler, exportCompiler,
        {.composition = authored->composition,
         .frame = std::nullopt,
         .range = std::pair<std::uint64_t, std::uint64_t>{0, kFrames - 1},
         .preset = output::OutputPresetV1::PngRgba8SrgbV1,
         .destination = folder / "range.png"},
        {}, provider);
    checks.expect(ranged.succeeded && ranged.publishedFrames == kFrames,
                  prefix + "a range rendered inside publishes every frame: " + ranged.diagnostic);
    std::vector<std::filesystem::path> members;
    for (const auto& entry : std::filesystem::directory_iterator(folder))
        if (entry.path().extension() == ".png")
            members.push_back(entry.path());
    std::ranges::sort(members);
    bool rangeUndimmed = members.size() == kFrames;
    for (std::size_t index = 0; rangeUndimmed && index < members.size(); ++index)
        rangeUndimmed = framesMatch(decodePng(members[index]), outside.at(index));
    checks.expect(rangeUndimmed, prefix + "every frame of the range equals the undimmed Viewer's");
    checks.expect(provider->shutdownAndWait(10s), prefix + "the export provider retires");

    // The GPU route at 1080p and 4K: the dimmed Viewer frame renders on the device, no fallback.
    if (route.gpu) {
        for (const auto [width, height] : {std::pair{1920U, 1080U}, std::pair{3840U, 2160U}}) {
            const auto label = prefix + std::to_string(width) + "x" + std::to_string(height);
            auto large = author(width, height);
            auto& largeSession = *large->session;
            ui::CompositionPreviewController largeController(largeSession, rig.scheduler,
                                                             *rig.bridge, v.pipeline, settings,
                                                             nullptr, nullptr, v.submitter);
            ui::ViewerEditor largeViewer(largeSession, largeController);
            largeViewer.resize(560, 340);
            largeViewer.show();
            const auto largeChain = std::vector{large->root, large->outer};
            checks.expect(largeSession.setViewerLevel({largeChain, std::nullopt}).ok(),
                          label + ": enter outer");
            const auto countersBefore = v.counters();
            const bool shown = bloom::test_support::awaitQtCondition(
                "dimmed Viewer frame from the device is shown",
                [&] {
                    const auto displayed = largeViewer.displayedFrameForTest();
                    return largeController.state().activity == ui::PreviewActivity::Ready &&
                           displayed != nullptr &&
                           displayed->desiredIdentity().isolation == largeChain &&
                           displayed->provenance().provider == v.provider;
                },
                120s);
            const auto countersAfter = v.counters();
            checks.expect(shown, label + ": the dimmed Viewer frame comes from the device (" +
                                     std::to_string(static_cast<int>(
                                         largeController.state().activity)) +
                                     ")");
            checks.expect(countersAfter.cpuFallbacks == countersBefore.cpuFallbacks &&
                              countersAfter.nativeDispatches > countersBefore.nativeDispatches,
                          label + ": it ran on the device with no CPU fallback");
            largeController.beginShutdown();
        }
    }
    const auto counters = v.counters();
    std::cout << prefix << "ran; preparations " << v.preparations.load() << ", device dispatches "
              << counters.nativeDispatches << ", CPU fallbacks " << counters.cpuFallbacks << '\n';
    controller.beginShutdown();
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path loader;
    std::vector<char*> forwarded{argv[0]};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc)
            loader = argv[++index];
        else if (argument == "--require-device")
            gRequireDevice = true;
        else
            forwarded.push_back(argv[index]);
    }
    int forwardedCount = static_cast<int>(forwarded.size());
    QApplication application(forwardedCount, forwarded.data());
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName("BloomViewerIsolation");
    QCoreApplication::setApplicationName("ViewerIsolation");
    QSettings().setValue("viewer/resolution", "Full");

    Expectations checks;
    const std::filesystem::path workDir = temporary.path().toStdString();
    try {
        exerciseRoute(checks, {"cpu", false}, workDir, loader);
#if defined(BLOOM_UI_ISOLATION_GPU)
        if (loader.empty()) {
            std::cout << "NotRun: gpu route: no Vulkan loader was given (--loader)\n";
            checks.expect(!gRequireDevice, "--require-device needs --loader");
        } else {
            exerciseRoute(checks, {"gpu", true}, workDir, loader);
        }
#else
        std::cout << "NotRun: gpu route: this build has no Vulkan support\n";
        checks.expect(!gRequireDevice, "--require-device needs a Vulkan build");
#endif
    } catch (const std::exception& exception) {
        checks.expect(false, std::string("unexpected exception: ") + exception.what());
    }
    if (checks.failures() + bloom::test_support::awaitFailureCount() != 0) {
        std::cerr << "viewer isolation: " << checks.failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: viewer isolation\n";
    return 0;
}
