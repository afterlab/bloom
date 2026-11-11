// #648: exports stay on the device while the Viewer and a RAM preview hold their frames and the
// document takes many small revisions. A 512x288 composition of solid layers (translated, rotated,
// partly transparent and blended, one of them keyed) is shown in the real Viewer; then, round after
// round, a few small edits are made and shown, a RAM preview fills the range at Half, and the real
// export route renders a PNG range from the revision just edited, one export straight after
// another. Every exported frame must have been evaluated on the device with no CPU reference
// frame, and each round's frames must match the CPU reference export of the same revision (RGB
// within one code, exact alpha). After the last export nothing of the exports stays admitted on
// the export provider's processing root.
//
// The CPU reference half always runs. The GPU half runs when --loader names a Vulkan loader and a
// qualified device exists; otherwise it prints NotRun explicitly, and --require-device turns that
// into a failure. The Viewer here publishes GPU-prepared packed frames: the native-resident Viewer
// product needs a real window system and is not available to this offscreen test.

#include <bloom/commands/animation_operations.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/graph.hpp>
#include <bloom/host/gpu_export_provider.hpp>
#include <bloom/output/media_output.hpp>
#include <bloom/output/output_analysis.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/scripting/render.hpp>
#include <bloom/scripting/session.hpp>
#include <bloom/ui/composition_commands.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#if defined(BLOOM_UI_EXPORT_REVISIONS_GPU)
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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
        if (ok) {
            return;
        }
        ++failures_;
        std::cerr << "FAIL: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

bool gRequireDevice = false;

// The preview service counters this test reads (zero on the CPU route, which has no service).
struct RouteCounters final {
    std::uint64_t nativeDispatches = 0;
    std::uint64_t cpuFallbacks = 0;
};

constexpr std::uint32_t kWidth = 512;
constexpr std::uint32_t kHeight = 288;
constexpr std::uint64_t kFps = 24;
// The session's frames: two seconds.
constexpr std::size_t kSessionFrames = 2 * kFps;
// Rounds of edits and exports, the edits in each, and the exported frames (across the keyed move).
constexpr std::size_t kRounds = 6;
constexpr std::size_t kEditsPerRound = 5;
constexpr std::uint64_t kFirstExported = 10;
constexpr std::uint64_t kLastExported = 13;

template <typename Predicate>
bool waitUntil(Predicate predicate, const std::chrono::milliseconds limit = 30s) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < limit.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

struct Pixels final {
    bool ok = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; // Straight RGBA8, row-major.

    [[nodiscard]] const std::uint8_t* at(const std::uint32_t x, const std::uint32_t y) const {
        return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4U;
    }
};

[[nodiscard]] Pixels pixelsOf(const ui::PreparedPreviewFrameHandle& frame) {
    Pixels out;
    const auto view = frame != nullptr ? frame->displayBufferView() : std::nullopt;
    if (!view.has_value() ||
        view->layout.rowStrideBytes !=
            static_cast<std::size_t>(view->displayWindow.extent().width()) * 4U) {
        return out;
    }
    out.width = view->displayWindow.extent().width();
    out.height = view->displayWindow.extent().height();
    if (view->pixels.size() != static_cast<std::size_t>(out.width) * out.height) {
        return out;
    }
    out.rgba.reserve(view->pixels.size() * 4U);
    for (const auto& pixel : view->pixels) {
        out.rgba.insert(out.rgba.end(), {pixel.red, pixel.green, pixel.blue, pixel.alpha});
    }
    out.ok = true;
    return out;
}

[[nodiscard]] Pixels decodePng(const std::filesystem::path& path) {
    Pixels out;
    QImage image(QString::fromStdString(path.string()));
    if (image.isNull()) {
        return out;
    }
    // Straight-to-straight: no premultiplication round trip.
    image = image.convertToFormat(QImage::Format_RGBA8888);
    out.width = static_cast<std::uint32_t>(image.width());
    out.height = static_cast<std::uint32_t>(image.height());
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4U);
    for (int y = 0; y < image.height(); ++y) {
        std::copy_n(image.constScanLine(y), static_cast<std::size_t>(image.width()) * 4U,
                    out.rgba.begin() + static_cast<std::ptrdiff_t>(y) * image.width() * 4);
    }
    out.ok = true;
    return out;
}

// RGB within one 8-bit code, alpha exactly.
[[nodiscard]] bool framesMatch(const Pixels& a, const Pixels& b) {
    if (!a.ok || !b.ok || a.width != b.width || a.height != b.height) {
        return false;
    }
    for (std::size_t index = 0; index < a.rgba.size(); ++index) {
        const int difference =
            std::abs(static_cast<int>(a.rgba[index]) - static_cast<int>(b.rgba[index]));
        if (index % 4U == 3U ? difference != 0 : difference > 1) {
            return false;
        }
    }
    return true;
}

struct Route final {
    const char* name;
    bool gpu;
};

// One authored composition shared by the Viewer session and the export route: five solid layers,
// smaller than the frame, at whole and fractional positions, two rotated, two partly transparent,
// two blended, and the first with its position keyed across the first second.
struct Authored final {
    std::unique_ptr<scripting::Session> scripting;
    std::unique_ptr<ui::CompositionSession> session;
    document::CompositionId composition;
    // Each layer's position and opacity parameters, for the small edits.
    std::vector<document::ParameterId> positions;
    std::vector<document::ParameterId> opacities;
};

template <typename Operation, typename... Arguments>
commands::CommandResult edit(Authored& authored, Arguments&&... arguments) {
    commands::Transaction transaction("Export revisions",
                                      authored.scripting->snapshot().revision());
    transaction.emplace<Operation>(std::forward<Arguments>(arguments)...);
    return authored.scripting->commandStack()->execute(std::move(transaction));
}

[[nodiscard]] std::optional<Authored> author(Expectations& checks) {
    const auto format = document::CompositionFormat::create(kWidth, kHeight);
    checks.expect(format.has_value(), "the composition format is valid");
    if (!format.has_value()) {
        return std::nullopt;
    }
    auto created = scripting::Session::createNew("Export revisions", "Main",
                                                 core::RationalTime::fromInteger(2), *format);
    checks.expect(static_cast<bool>(created), "the authoring session is created");
    if (!created) {
        return std::nullopt;
    }
    Authored authored;
    authored.scripting = std::move(created).takeSession();
    authored.composition = authored.scripting->snapshot().project().compositions().front().id();
    authored.session = std::make_unique<ui::CompositionSession>(
        *authored.scripting->document(), *authored.scripting->commandStack(), authored.composition);

    struct Layer final {
        core::Color4d color;
        document::Vec2d position;
        double size;
        double rotation;
        double opacity;
    };
    const std::array<Layer, 5> layers{{
        {{0.9, 0.2, 0.1, 1.0}, {120.0, 90.0}, 150.0, 0.0, 1.0},
        {{0.1, 0.7, 0.3, 1.0}, {300.5, 140.25}, 120.0, 0.0, 0.6},
        {{0.2, 0.3, 0.9, 1.0}, {220.0, 200.0}, 180.0, 20.0, 1.0},
        {{0.8, 0.8, 0.1, 1.0}, {400.0, 110.0}, 96.0, 45.0, 0.5},
        {{0.6, 0.1, 0.7, 1.0}, {256.0, 144.0}, 220.0, 0.0, 1.0},
    }};
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const auto& layer = layers.at(index);
        const auto added = edit<commands::AddSolidLayer>(
            authored, authored.composition, "Solid " + std::to_string(index), layer.color,
            std::optional<document::Vec2d>{layer.position}, layer.opacity);
        const auto solid =
            added.outputId<document::NodeId>(commands::kAddSolidLayerSolidNodeOutput);
        const auto position =
            added.outputId<document::ParameterId>(commands::kAddSolidLayerPositionParameterOutput);
        const auto opacity =
            added.outputId<document::ParameterId>(commands::kAddSolidLayerOpacityParameterOutput);
        const auto rotation =
            added.outputId<document::ParameterId>(commands::kAddSolidLayerRotationParameterOutput);
        const auto blend =
            added.outputId<document::ParameterId>(commands::kAddSolidLayerBlendModeParameterOutput);
        checks.expect(added.succeeded() && solid && position && opacity && rotation && blend,
                      "a solid layer is added with its parameters");
        if (!solid || !position || !opacity || !rotation || !blend) {
            return std::nullopt;
        }
        checks.expect(authored.session->setNodeParameterValue(
                          *solid, std::string(document::kSolidWidthParameterRole), layer.size,
                          QStringLiteral("Size")) &&
                          authored.session->setNodeParameterValue(
                              *solid, std::string(document::kSolidHeightParameterRole),
                              layer.size * 0.75, QStringLiteral("Size")),
                      "the solid is smaller than the frame");
        if (layer.rotation != 0.0) {
            checks.expect(authored.session->setParameterValue(*rotation, layer.rotation,
                                                              QStringLiteral("Rotate")),
                          "the layer is rotated");
        }
        if (index == 2 || index == 4) {
            checks.expect(authored.session->setParameterValue(
                              *blend,
                              static_cast<std::int64_t>(index == 2 ? core::BlendMode::Screen
                                                                   : core::BlendMode::Multiply),
                              QStringLiteral("Blend")),
                          "the layer is blended");
        }
        authored.positions.push_back(*position);
        authored.opacities.push_back(*opacity);
    }
    // The first layer moves across the first second.
    checks.expect(edit<commands::CreateAnimationForParameter>(authored, authored.composition,
                                                              authored.positions.front(),
                                                              core::RationalTime{})
                          .succeeded() &&
                      edit<commands::SetKeyframeAtTimeForParameter>(
                          authored, authored.composition, authored.positions.front(),
                          core::RationalTime::fromInteger(1), document::Vec2d{380.0, 210.0})
                          .succeeded(),
                  "the first layer's position is keyed");
    return authored;
}

struct PreviewRig final {
    runtime::TaskScheduler scheduler;
    runtime::NodeDefinitionRegistry definitions;
    std::unique_ptr<runtime::SnapshotCompiler> compiler;
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider qualified;
    std::unique_ptr<ui::TaskUiBridge> bridge;
};

// Exercises one route: the Viewer's process frame against the exported still.
void exerciseRoute(Expectations& checks, const Route& route, const std::filesystem::path& workDir,
                   [[maybe_unused]] const std::filesystem::path& loader) {
    const std::string prefix = std::string(route.name) + ": ";
    auto authored = author(checks);
    if (!authored.has_value()) {
        return;
    }
    auto& session = *authored->session;

    PreviewRig rig;
    checks.expect(runtime::registerBuiltInNodeDefinitions(rig.definitions),
                  prefix + "node definitions register");
    rig.definitions.freeze();
    rig.compiler = std::make_unique<runtime::SnapshotCompiler>(rig.definitions);
    rig.qualified.publish(runtime::buildBloomNeutralQualifiedDisplayProcessor());
    rig.bridge = std::make_unique<ui::TaskUiBridge>(rig.scheduler, nullptr, 1ms);

    auto pipeline = ui::makeCompositionPreviewPipeline(*rig.compiler, rig.evaluator,
                                                       rig.displayPreparer, rig.qualified);
    ui::CompositionPreviewSettings settings;
    settings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
    ui::PreviewPreparationSubmitter submitter;
    runtime::PreviewDisplayProvider expectedProvider = runtime::PreviewDisplayProvider::CpuOcio;
    std::function<RouteCounters()> counters = [] { return RouteCounters{}; };

#if defined(BLOOM_UI_EXPORT_REVISIONS_GPU)
    std::unique_ptr<runtime::GpuPreviewDisplayService> service;
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
        options.previewByteAllowance = std::size_t{1} << 28U;
        service = std::make_unique<runtime::GpuPreviewDisplayService>(
            rig.scheduler, gpuStage,
            ui::makeCompositionPreviewCpuStage(*rig.compiler, rig.evaluator, rig.qualified),
            ui::makeCompositionPreviewCpuDisplayFallback(rig.displayPreparer), options);
        waitUntil(
            [&] {
                return service->status().state !=
                       runtime::GpuPreviewDisplayServiceState::Initializing;
            },
            90s);
        if (service->status().state != runtime::GpuPreviewDisplayServiceState::Ready) {
            std::cout << "NotRun: " << route.name << " preview: no qualified device ("
                      << service->status().residentDetail << ")\n";
            checks.expect(!gRequireDevice, prefix + "--require-device needs a GPU device");
            service->beginShutdown();
            return;
        }
        submitter = [svc = service.get()](
                        runtime::TaskRequest request, const document::Snapshot& snapshot,
                        const runtime::PreviewRequestIdentity& identity, const std::size_t bytes,
                        const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            return svc->submit(std::move(request), snapshot, identity, bytes, overrides);
        };
        settings.pixelStorageByteLimit = std::size_t{1} << 28U;
        counters = [svc = service.get()] {
            const auto status = svc->status().counters;
            return RouteCounters{status.nativeDispatches, status.cpuFallbacks};
        };
        expectedProvider = runtime::PreviewDisplayProvider::GpuPreparedPacked;
    }
#endif

    ui::CompositionPreviewController controller(session, rig.scheduler, *rig.bridge, pipeline,
                                                settings, nullptr, nullptr, submitter);
    ui::ViewerEditor viewer(session, controller);
    viewer.resize(480, 320);
    viewer.show();
    // The Viewer at each tested frame, in the order an artist might scrub them.
    constexpr std::array<std::uint64_t, 4> kFrames{0, kFps / 2, kFps, kFps * 3 / 2};
    std::array<Pixels, kFrames.size()> shown;
    for (std::size_t index = 0; index < kFrames.size(); ++index) {
        const auto frame = kFrames.at(index);
        const auto label = prefix + "frame " + std::to_string(frame) + ": ";
        // The session starts at the first of them, so only the later ones move the playhead.
        checks.expect(session.setCurrentTime(*core::RationalTime::create(
                          static_cast<std::int64_t>(frame), static_cast<std::int64_t>(kFps))) ||
                          index == 0,
                      label + "the playhead moves");
        controller.requestRefresh();
        const auto before =
            index == 0 ? ui::PreparedPreviewFrameHandle{} : viewer.displayedFrameForTest();
        const bool ready = waitUntil([&] {
            const auto displayed = viewer.displayedFrameForTest();
            return controller.state().activity == ui::PreviewActivity::Ready &&
                   displayed != nullptr && (index == 0 || displayed != before || index == 3) &&
                   displayed->provenance().provider == expectedProvider &&
                   viewer.displayedPreviewState().freshness == ui::FrameFreshness::Current;
        });
        checks.expect(ready, label + "the Viewer displays a current frame from the expected route");
        shown.at(index) = pixelsOf(viewer.displayedFrameForTest());
        checks.expect(shown.at(index).ok && shown.at(index).width == kWidth &&
                          shown.at(index).height == kHeight,
                      label + "the Viewer process frame is readable at the composition size");
        if (!ready || !shown.at(index).ok) {
            return;
        }
    }

    checks.expect(counters().cpuFallbacks == 0, prefix + "the Viewer frames took no CPU fallback");
    checks.expect(!route.gpu || counters().nativeDispatches > 0,
                  prefix + "the Viewer frames ran on the device");

    // The export route, rendered from the same document. The CPU reference provider renders each
    // round's revision for the GPU route to be compared to.
    runtime::TaskScheduler exportScheduler;
    const runtime::SnapshotCompiler exportCompiler(document::builtInNodeDefinitions());
    std::shared_ptr<host::GpuExportProvider> provider;
#if defined(BLOOM_UI_EXPORT_REVISIONS_GPU)
    if (route.gpu) {
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.enabled = true;
        options.loaderPath = loader;
        options.ocioResolver = host::makePackagedGpuOcioResolver(host::currentExecutablePath());
        provider = host::GpuExportProvider::create(options);
    }
#endif
    std::shared_ptr<host::GpuExportProvider> reference;
    {
        runtime::GpuProcessFrameEvaluatorOptions options;
        options.enabled = false;
        reference = host::GpuExportProvider::create(options);
        reference->prepare(exportScheduler);
        if (!route.gpu) {
            provider = reference;
        }
    }
    if (route.gpu) {
        provider->prepare(exportScheduler);
    }
    const auto exportRange = [&](const std::shared_ptr<host::GpuExportProvider>& with,
                                 const std::filesystem::path& folder) {
        std::filesystem::create_directories(folder);
        return scripting::Render::run(
            *authored->scripting, exportScheduler, exportCompiler,
            {.composition = authored->composition,
             .frame = std::nullopt,
             .range = std::pair<std::uint64_t, std::uint64_t>{kFirstExported, kLastExported},
             .preset = output::OutputPresetV1::PngRgba8SrgbV1,
             .destination = folder / "frame.png"},
            {}, with);
    };
    const auto framesOf = [](const std::filesystem::path& folder) {
        std::vector<std::filesystem::path> members;
        if (std::filesystem::exists(folder)) {
            for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                if (entry.path().extension() == ".png") {
                    members.push_back(entry.path());
                }
            }
        }
        std::ranges::sort(members);
        return members;
    };

    controller.setResolutionPolicy(runtime::PreviewResolutionPolicy::Half);
    ui::RamPreviewController ram(session, controller, rig.scheduler, *rig.bridge, pipeline, nullptr,
                                 submitter);
    constexpr std::uint64_t exported = kLastExported - kFirstExported + 1;
    std::uint64_t exportedOnDevice = 0;
    for (std::size_t round = 0; round < kRounds; ++round) {
        const auto label = prefix + "round " + std::to_string(round) + ": ";
        // Small edits, each a revision of its own, each shown in the Viewer.
        for (std::size_t step = 0; step < kEditsPerRound; ++step) {
            const std::size_t layer = (round * kEditsPerRound + step) % authored->opacities.size();
            const double nudge = static_cast<double>(round * kEditsPerRound + step);
            const bool edited =
                step % 2 == 0
                    ? session.setParameterValue(authored->opacities.at(layer),
                                                0.35 + 0.05 * static_cast<double>(step) +
                                                    0.02 * static_cast<double>(round),
                                                QStringLiteral("Opacity"))
                    : layer != 0 && session.setParameterValue(
                                        authored->positions.at(layer),
                                        document::Vec2d{90.0 + 13.0 * nudge, 60.0 + 7.5 * nudge},
                                        QStringLiteral("Move"));
            checks.expect(edited || (step % 2 == 1 && layer == 0), label + "a small edit applies");
            const auto before = viewer.displayedFrameForTest();
            controller.requestRefresh();
            checks.expect(waitUntil([&] {
                              const auto displayed = viewer.displayedFrameForTest();
                              return controller.state().activity == ui::PreviewActivity::Ready &&
                                     displayed != nullptr && (!edited || displayed != before) &&
                                     displayed->provenance().provider == expectedProvider &&
                                     viewer.displayedPreviewState().freshness ==
                                         ui::FrameFreshness::Current;
                          }),
                          label + "the Viewer shows the edited revision from the expected route");
        }
        // The RAM preview holds this revision's frames while the exports run.
        ram.start();
        checks.expect(waitUntil([&] { return !ram.isCaching(); }, 180s) &&
                          ram.cachedFrameCount() == kSessionFrames,
                      label + "the RAM preview at Half retains every frame");

        // Two exports straight after one another, from the revision just edited.
        std::filesystem::path first;
        for (std::size_t pass = 0; pass < 2; ++pass) {
            const auto folder = workDir / (std::string(route.name) + "-round-" +
                                           std::to_string(round) + "-" + std::to_string(pass));
            const auto result = exportRange(provider, folder);
            checks.expect(result.succeeded, label + "the export publishes: " + result.diagnostic);
            checks.expect(result.publishedFrames == exported,
                          label + "the export publishes every frame");
            checks.expect(result.gpuEvaluatedFrames == (route.gpu ? exported : 0) &&
                              (!route.gpu || result.cpuReferenceFrames == 0),
                          label + "every exported frame was evaluated on the expected route (" +
                              std::to_string(result.gpuEvaluatedFrames) + " on the device, " +
                              std::to_string(result.cpuReferenceFrames) + " on the CPU reference)");
            exportedOnDevice += result.gpuEvaluatedFrames;
            if (pass == 0) {
                first = folder;
            }
        }
        if (route.gpu) {
            // The same revision through the CPU reference provider.
            const auto folder = workDir / ("reference-round-" + std::to_string(round));
            const auto result = exportRange(reference, folder);
            checks.expect(result.succeeded && result.gpuEvaluatedFrames == 0,
                          label + "the CPU reference export publishes");
            const auto device = framesOf(first);
            const auto cpu = framesOf(folder);
            checks.expect(device.size() == exported && cpu.size() == exported,
                          label + "both exports wrote every frame");
            for (std::size_t index = 0; index < device.size() && index < cpu.size(); ++index) {
                checks.expect(framesMatch(decodePng(device.at(index)), decodePng(cpu.at(index))),
                              label + "frame " + std::to_string(kFirstExported + index) +
                                  " matches the CPU reference (RGB within one code, exact alpha)");
            }
        }
    }
    checks.expect(!route.gpu || exportedOnDevice == exported * 2 * kRounds,
                  prefix + "every export frame of every round ran on the device");
    checks.expect(counters().cpuFallbacks == 0, prefix + "the previews took no CPU fallback");
    ram.beginShutdown();
    // Nothing of the exports stays admitted on the export provider's processing root.
    if (const auto policy = provider->processingPhasePolicy()) {
        checks.expect(
            waitUntil([&] { return policy->snapshot().reservations.admittedPhases == 0; }, 10s),
            prefix + "no export phase stays admitted after the last export");
    }
    checks.expect(provider->shutdownAndWait(10s), prefix + "the export provider retires");
    if (reference != provider) {
        checks.expect(reference->shutdownAndWait(10s), prefix + "the reference provider retires");
    }

    controller.beginShutdown();
    rig.bridge->beginShutdown();
    (void)waitUntil([&] { return rig.scheduler.isQuiescent(); }, 10s);
#if defined(BLOOM_UI_EXPORT_REVISIONS_GPU)
    if (service != nullptr) {
        service->beginShutdown();
        (void)waitUntil([&] {
            return service->status().state == runtime::GpuPreviewDisplayServiceState::Stopped;
        });
    }
#endif
}

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    std::filesystem::path loader;
    std::vector<char*> forwarded{argv[0]};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            loader = argv[++index];
        } else if (argument == "--require-device") {
            gRequireDevice = true;
        } else {
            forwarded.push_back(argv[index]);
        }
    }
    int forwardedCount = static_cast<int>(forwarded.size());
    QApplication application(forwardedCount, forwarded.data());
    QTemporaryDir temporary;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QCoreApplication::setOrganizationName("BloomExportRevisions");
    QCoreApplication::setApplicationName("ViewerExportRevisions");
    QSettings().setValue("viewer/resolution", "Full");

    Expectations checks;
    const std::filesystem::path workDir = temporary.path().toStdString();
    exerciseRoute(checks, {"cpu", false}, workDir, loader);

#if defined(BLOOM_UI_EXPORT_REVISIONS_GPU)
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

    if (checks.failures() != 0) {
        std::cerr << "viewer/export revisions: " << checks.failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: viewer/export revisions\n";
    return 0;
}
