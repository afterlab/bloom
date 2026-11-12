// The `route.preview.viewer` and `route.preview.ram_preview` proof harness for a platform whose
// preview is prepared on the device and painted by the host (gpuPreviewRouteDisplay() ==
// HostPainted): the application's own preview configuration, with no native presentation surface.
//
// It drives the production chain over a real document: Document, CommandStack and
// CompositionSession, the GpuPreviewDisplayService in its GPU-prepared packed mode with
// presentation disabled (what the application runs), the CompositionPreviewController, and, by
// route, the ViewerEditor that paints the frame or the RamPreviewController that fills the cache.
// No frame is injected and no second device is created.
//
// The proof carries what the run did: the cold native dispatch count from the service counters,
// the device ownership epoch, the ordered per-frame process identity digest, an evidence digest of
// GPU and CPU-oracle pixel pairs, and the final display transfers with the bytes they moved. Every
// frame must be a GPU-prepared frame, equal to the CPU oracle in plan, request identity and
// evaluated geometry, and in every pixel within one code per color channel with alpha exact. The
// run must make exactly one final display transfer per frame, no other readback, and take the CPU
// path for none. Without a device no proof is written, and the harness says so.

#include "gpu_route_proof_harness_support.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/gpu_prepared_packed_preview_product.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QPixmap>
#include <QSettings>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace bloom::ui::verticalproof;
namespace routeproof = bloom::ui::routeproof;
using namespace std::chrono_literals;

namespace {

using namespace bloom;

constexpr std::int64_t kFrameRate = 25;
constexpr std::int64_t kRamFrameCount = 4;

enum class Route : std::uint8_t { None, Viewer, RamPreview };

struct HarnessOptions final {
    std::filesystem::path loader;
    std::filesystem::path proofDirectory;
    Route route = Route::None;
    bool requireDevice = false;
    bool valid = true;
};

[[nodiscard]] HarnessOptions parseHarnessOptions(const int argc, char** argv) {
    HarnessOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            options.loader = argv[++index];
        } else if (argument == "--proof-dir" && index + 1 < argc) {
            options.proofDirectory = argv[++index];
        } else if (argument == "--route" && index + 1 < argc) {
            const std::string_view route = argv[++index];
            options.route = route == "viewer"        ? Route::Viewer
                            : route == "ram-preview" ? Route::RamPreview
                                                     : Route::None;
        } else if (argument == "--require-device") {
            options.requireDevice = true;
        } else {
            options.valid = false;
            return options;
        }
    }
    options.valid = options.route != Route::None;
    return options;
}

[[nodiscard]] core::RationalTime frameTime(const std::int64_t index) {
    const auto value = core::RationalTime::create(index, kFrameRate);
    if (!value.has_value()) {
        std::abort();
    }
    return *value;
}

[[nodiscard]] document::CompositionFormat ramFormat() {
    const auto rate = document::FrameRate::create(kFrameRate, 1);
    if (!rate.has_value()) {
        std::abort();
    }
    const auto format =
        document::CompositionFormat::create(1920U, 1080U, core::PixelAspectRatio::square(), *rate);
    if (!format.has_value()) {
        std::abort();
    }
    return *format;
}

// One frame against the CPU oracle: a GPU-prepared host-painted product, the oracle's plan,
// request identity and geometry, and every pixel. Appends its evidence when all of it holds.
void verifyFrame(Fixture& fixture, const document::Snapshot& snapshot,
                 const routeproof::PreparedPreviewFrameHandle& frame, Expectations& checks,
                 std::vector<routeproof::FrameEvidence>& evidence, const std::string& label) {
    checks.expect(frame != nullptr &&
                      frame->provenance().provider ==
                          runtime::PreviewDisplayProvider::GpuPreparedPacked &&
                      frame->provenance().gpuPreparedPacked != nullptr,
                  label + ": the frame is a GPU-prepared host-painted frame");
    if (frame == nullptr) {
        return;
    }
    const auto reference =
        snapshotCpuReferenceAtResolution(fixture, snapshot, frame->desiredIdentity());
    checks.expect(reference != nullptr && reference->displayBufferView().has_value(),
                  label + ": the CPU oracle evaluates the same identity into pixels");
    if (reference == nullptr) {
        return;
    }
    std::string detail;
    checks.expect(routeproof::sameOracleIdentity(*frame, *reference, detail),
                  label + ": the frame matches the CPU oracle identity and geometry: " + detail);
    routeproof::FrameEvidence frameEvidence;
    const bool same =
        routeproof::buildHostPaintedFrameEvidence(*frame, *reference, frameEvidence, detail);
    checks.expect(same, label + ": every GPU-prepared pixel matches the CPU oracle: " + detail);
    if (same) {
        evidence.push_back(std::move(frameEvidence));
    }
}

// The widget's own paint of the displayed frame: the pixel at the middle of the mapped display
// rectangle is the frame's middle pixel. Both solids are uniform there, so scaling cannot move it.
void verifyPainted(ui::ViewerEditor& viewer, const runtime::PreparedPreviewFrame& frame,
                   Expectations& checks) {
    const auto mapping = viewer.mappingForTest();
    const auto view = frame.displayBufferView();
    checks.expect(mapping.has_value() && view.has_value(),
                  "the Viewer maps the displayed frame into its canvas");
    if (!mapping.has_value() || !view.has_value()) {
        return;
    }
    const auto width = view->displayWindow.extent().width();
    const auto height = view->displayWindow.extent().height();
    const auto expected = view->pixels[static_cast<std::size_t>(height / 2U) * width + width / 2U];
    const QImage painted = viewer.grab().toImage().convertToFormat(QImage::Format_RGBA8888);
    const auto ratio = painted.devicePixelRatio();
    const QPointF center = mapping->displayRect.center() * ratio;
    const QPoint point{static_cast<int>(center.x()), static_cast<int>(center.y())};
    checks.expect(painted.rect().contains(point), "the display rectangle is inside the Viewer");
    if (!painted.rect().contains(point)) {
        return;
    }
    const QColor actual = painted.pixelColor(point);
    const auto near = [](const int a, const std::uint8_t b) {
        return std::abs(a - static_cast<int>(b)) <= 1;
    };
    const bool same = near(actual.red(), expected.red) && near(actual.green(), expected.green) &&
                      near(actual.blue(), expected.blue);
    checks.expect(same, "the Viewer painted the GPU-prepared frame's pixels");
    if (!same) {
        std::cout << "PHASE painted-mismatch painted=" << actual.red() << ',' << actual.green()
                  << ',' << actual.blue() << " frame=" << static_cast<int>(expected.red) << ','
                  << static_cast<int>(expected.green) << ',' << static_cast<int>(expected.blue)
                  << '\n';
    }
}

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    const HarnessOptions options = parseHarnessOptions(argc, argv);
    if (!options.valid) {
        return 2;
    }
    const bool viewerRoute = options.route == Route::Viewer;
    const std::string routeId = viewerRoute ? "route.preview.viewer" : "route.preview.ram_preview";
    QApplication application(argc, argv);
    if (runtime::gpuPreviewRouteDisplay() != runtime::GpuPreviewRouteDisplay::HostPainted) {
        std::cout << "SKIP: this platform proves the resident preview route\n";
        return options.requireDevice ? 1 : 0;
    }
    if (options.loader.empty() || options.proofDirectory.empty()) {
        std::cout << "SKIP: the host-painted route proof needs --loader and --proof-dir\n";
        return options.requireDevice ? 1 : 0;
    }

    const auto scratch =
        std::filesystem::temp_directory_path() /
        ("bloom-host-painted-route-proof-" + std::to_string(QCoreApplication::applicationPid()));
    const auto settingsRoot = scratch / "settings";
    const auto mediaDirectory = scratch / "media";
    std::filesystem::create_directories(settingsRoot);
    std::filesystem::create_directories(mediaDirectory);
    struct ScratchRemoval final {
        std::filesystem::path path;
        explicit ScratchRemoval(std::filesystem::path directory) : path(std::move(directory)) {}
        ScratchRemoval(const ScratchRemoval&) = delete;
        ScratchRemoval& operator=(const ScratchRemoval&) = delete;
        ~ScratchRemoval() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } const scratchRemoval(scratch);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString(settingsRoot.string()));
    QCoreApplication::setOrganizationName(QStringLiteral("BloomRouteProof"));
    QCoreApplication::setApplicationName(QStringLiteral("HostPaintedRouteProof"));

    std::string nonce;
    if (!routeproof::readProofNonce(options.proofDirectory, nonce)) {
        std::cout << "SKIP: no fresh run nonce in the proof directory\n";
        return options.requireDevice ? 1 : 0;
    }

    Expectations checks;
    Fixture fixture(mediaDirectory);
    if (fixture.processor == nullptr) {
        std::cout << "SKIP: the Bloom Neutral v1 processor is unavailable\n";
        return options.requireDevice ? 1 : 0;
    }

    const auto duration = core::RationalTime::create(kRamFrameCount, kFrameRate);
    if (!duration.has_value()) {
        return 2;
    }
    auto newProject =
        viewerRoute ? document::makeNewProject("Viewer Route Proof", "Main",
                                               core::RationalTime::fromInteger(10),
                                               format1920x1080NonSquare())
                    : document::makeNewProject("RAM Route Proof", "Main", *duration, ramFormat());
    const auto compositionId = newProject.initialCompositionId;
    document::Document document(std::move(newProject.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, compositionId);
    if (viewerRoute) {
        checks.expect(
            session.addSolidLayer(QStringLiteral("Lower"), core::Color4d{0.15, 0.35, 0.85, 1.0}),
            "the lower Solid layer is created");
        checks.expect(
            session.addSolidLayer(QStringLiteral("Upper"), core::Color4d{0.9, 0.25, 0.1, 1.0}),
            "the upper Solid layer is created");
        checks.expect(session.setSelectedPosition(13.5, -7.25),
                      "the upper Solid carries a fractional translation");
        checks.expect(session.setSelectedBlendMode(core::BlendMode::Normal),
                      "the upper Solid composites source-over");
        // The upper Solid moves, so every step of the playhead is a frame of its own to prepare.
        checks.expect(session.currentTime() == frameTime(0) || session.setCurrentTime(frameTime(0)),
                      "the session is at frame 0");
        checks.expect(session.toggleKeyframe("position"), "the position keyframe is created");
        checks.expect(session.setCurrentTime(frameTime(kRamFrameCount - 1)),
                      "the session moves to the last frame");
        checks.expect(session.setSelectedPosition(31.25, 11.5),
                      "the upper Solid's last position is set");
        checks.expect(session.setCurrentTime(frameTime(0)), "the session returns to frame 0");
    } else {
        checks.expect(session.addSolidLayer(QStringLiteral("Moving Solid"),
                                            core::Color4d{0.2, 0.4, 0.8, 1.0}),
                      "the animated Solid layer is created");
        checks.expect(session.currentTime() == frameTime(0) || session.setCurrentTime(frameTime(0)),
                      "the session is at frame 0");
        checks.expect(session.toggleKeyframe("position"), "the position keyframe is created");
        checks.expect(session.setCurrentTime(frameTime(kRamFrameCount - 1)),
                      "the session moves to the last frame");
        checks.expect(session.setSelectedPosition(12.0, 9.0), "the animated position is set");
        checks.expect(session.setCurrentTime(frameTime(0)), "the session returns to frame 0");
    }

    TaskScheduler scheduler(schedulerConfig());
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    // The application's preview configuration: device preparation, one final display transfer,
    // no native presentation.
    auto serviceSettings = serviceOptions(options.loader);
    serviceSettings.product = runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
    serviceSettings.presentation = runtime::GpuPreviewDisplayServicePresentationMode::Disabled;
    GpuPreviewDisplayService service(
        scheduler, fixture.productionGpuStage(), fixture.productionCpuStage(),
        fixture.productionCpuFallback(displayPreparer), serviceSettings);
    const bool terminal = waitUntil(
        [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; }, 90s);
    if (!terminal || service.status().state != GpuPreviewDisplayServiceState::Ready) {
        std::cout << "SKIP: the GPU preview service is unavailable: "
                  << service.status().residentDetail << '\n';
        service.beginShutdown();
        return options.requireDevice ? 1 : 0;
    }
    checks.expect(service.status().presentationClient == nullptr,
                  "the host-painted route has no native presentation target");
    const auto core = runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(service);
    checks.expect(core != nullptr, "the service core is reachable");
    const auto epoch =
        core == nullptr
            ? 0U
            : runtime::detail::GpuPreviewDisplayServiceTestAccess::ownershipEpoch(*core);
    checks.expect(epoch > 0U, "the service publishes a nonzero device ownership epoch");
    const auto afterReady = service.status().counters;
    // Where the counted frames begin. The RAM run counts from Ready; the Viewer run counts from
    // the moment the Viewer has settled at its size (below).
    auto windowStart = afterReady;

    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds(1));
    auto pipeline = ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                                       displayPreparer, fixture.provider);
    ui::CompositionPreviewSettings settings;
    settings.pixelStorageByteLimit = kBudget;
    const ui::PreviewPreparationSubmitter submitter =
        [&service](runtime::TaskRequest request, const document::Snapshot& snap,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides)
        -> runtime::TaskSubmission<runtime::PreviewPreparationResultHandle> {
        return service.submit(std::move(request), snap, identity, limit, overrides);
    };
    auto frameCache = viewerRoute ? nullptr : std::make_shared<ui::PreviewFrameCache>();
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                frameCache, nullptr, submitter);

    std::vector<routeproof::FrameEvidence> evidence;
    std::vector<routeproof::PreparedPreviewFrameHandle> frames;
    const auto snapshot = session.snapshot();

    if (viewerRoute) {
        // The Viewer is shown and left to settle at its size first. While it takes its size a
        // request can be superseded after its frame was prepared, and such a frame is transferred
        // without ever being published; which requests overlap there is a matter of timing, not
        // of the route. The counted frames start once nothing is in flight: one frame per step of
        // the playhead, each prepared, transferred and published exactly once.
        std::vector<routeproof::PreparedPreviewFrameHandle> published;
        // The frame on screen when the counted frames begin; it was prepared before them.
        routeproof::PreparedPreviewFrameHandle settledFrame;
        const auto record = [&] {
            const auto frame = controller.state().frame;
            if (frame != nullptr && frame != settledFrame &&
                std::find(published.begin(), published.end(), frame) == published.end()) {
                published.push_back(frame);
            }
        };
        const auto recording = QObject::connect(
            &controller, &ui::CompositionPreviewController::stateChanged, &controller, record);
        checks.expect(waitUntil(
                          [&] {
                              return controller.state().frame != nullptr &&
                                     controller.state().activity != ui::PreviewActivity::Rendering;
                          },
                          60s),
                      "the first frame is published before the Viewer is created");
        record();
        auto host = std::make_unique<QWidget>();
        host->resize(960, 640);
        auto* layout = new QVBoxLayout(host.get());
        layout->setContentsMargins(0, 0, 0, 0);
        auto viewer = std::make_unique<ui::ViewerEditor>(session, controller);
        layout->addWidget(viewer.get());
        host->show();
        viewer->show();
        const auto settled = [&] {
            const auto frame = viewer->displayedFrameForTest();
            return controller.state().activity != ui::PreviewActivity::Rendering &&
                   frame != nullptr && frame == controller.state().frame &&
                   frame->desiredIdentity().resolution ==
                       controller.state()
                           .desiredIdentity.value_or(frame->desiredIdentity())
                           .resolution &&
                   frame->provenance().provider ==
                       runtime::PreviewDisplayProvider::GpuPreparedPacked;
        };
        bool displayed = waitUntil(settled, 60s);
        // A request made as the widget took its size may still be on its way; give it the chance
        // to land, then require the settled state again.
        (void)waitUntil([] { return false; }, 500ms);
        displayed = displayed && waitUntil(settled, 60s);
        if (displayed) {
            settledFrame = controller.state().frame;
            published.clear();
            windowStart = service.status().counters;
            for (std::int64_t index = 1; index < kRamFrameCount && displayed; ++index) {
                checks.expect(session.setCurrentTime(frameTime(index)),
                              "the playhead steps to the next frame");
                displayed = waitUntil(
                    [&] {
                        const auto shown = viewer->displayedFrameForTest();
                        return settled() && shown->desiredIdentity().time == frameTime(index);
                    },
                    60s);
            }
            checks.expect(published.size() == static_cast<std::size_t>(kRamFrameCount - 1),
                          "each step of the playhead published exactly one frame");
        }
        QObject::disconnect(recording);
        const auto frame = viewer->displayedFrameForTest();
        checks.expect(displayed, "the ViewerEditor displays the controller's GPU-prepared frame");
        if (!displayed) {
            const auto shown = controller.state().frame;
            std::cout << "PHASE viewer-display-failed activity="
                      << static_cast<int>(controller.state().activity) << " provider="
                      << (shown != nullptr ? static_cast<int>(shown->provenance().provider) : -1)
                      << " cpuFallbacks=" << service.status().counters.cpuFallbacks
                      << " detail=" << service.status().residentDetail << '\n';
        } else {
            checks.expect(!viewer->residentPresentationActiveForTest(),
                          "the Viewer paints the frame; no native surface presents it");
            checks.expect(!published.empty() && published.back() == frame,
                          "the displayed frame is the last frame the controller published");
            for (std::size_t index = 0; index < published.size(); ++index) {
                verifyFrame(fixture, snapshot, published[index], checks, evidence,
                            "viewer frame " + std::to_string(index));
            }
            verifyPainted(*viewer, *frame, checks);
            frames = published;
        }
        viewer.reset();
        host.reset();
    } else {
        ui::RamPreviewController ram(session, controller, scheduler, bridge, pipeline, nullptr,
                                     submitter);
        checks.expect(
            waitUntil([&] { return controller.state().activity != ui::PreviewActivity::Rendering; },
                      60s),
            "the foreground frame settled before the RAM run");
        // The foreground frame's work is not part of the RAM fill the proof describes; the fill
        // counts from here. Its frame is already cached, so the fill prepares the other frames.
        ram.start();
        const bool complete = waitUntil([&] { return !ram.isCaching(); }, 120s);
        checks.expect(complete &&
                          ram.totalFrameCount() == static_cast<std::uint64_t>(kRamFrameCount),
                      "the requested RAM range terminates");
        checks.expect(ram.lastCompletion() == ui::RamPreviewController::Completion::Complete,
                      "the RAM fill completes");
        checks.expect(ram.canAutoPlay(), "the whole range is retained and playable");
        checks.expect(ram.cachedFrameCount() == static_cast<std::uint64_t>(kRamFrameCount),
                      "every frame of the range is retained");
        for (const auto retainedIndex : ram.retainedFrameIndices()) {
            const auto index = static_cast<std::int64_t>(retainedIndex);
            const auto key = controller.cacheKeyForTime(frameTime(index));
            checks.expect(key.has_value() && frameCache->contains(*key),
                          "the retained frame is in the preview cache");
            if (!key.has_value()) {
                continue;
            }
            const runtime::PreviewRequestIdentity identity{
                .projectId = key->projectId,
                .compositionId = key->compositionId,
                .sourceRevision = key->sourceRevision,
                .requestGeneration = static_cast<std::uint64_t>(index + 1),
                .time = key->time,
                .output = key->output,
                .resolution = key->resolution,
                .quality = key->quality,
                .colorIntent = key->colorIntent,
                .resolutionPolicy = key->resolutionPolicy,
                .roi = key->roi,
                .viewAdjust = key->viewAdjust,
                .displayName = key->displayName,
                .viewName = key->viewName,
                .showLook = key->showLook};
            const auto frame = frameCache->take(identity);
            verifyFrame(fixture, snapshot, frame, checks, evidence,
                        "RAM frame " + std::to_string(index));
            if (frame != nullptr) {
                frames.push_back(frame);
            }
        }
        checks.expect(frames.size() == static_cast<std::size_t>(kRamFrameCount),
                      "every frame of the range was verified");
        ram.beginShutdown();
    }

    // Every frame the service prepared in this run is one of the verified frames: the frames the
    // controller published to the Viewer, or the RAM range (its first frame is the foreground
    // frame). So the counters since Ready describe exactly those frames.
    const auto after = service.status().counters;
    const auto coldDispatches = after.nativeDispatches - windowStart.nativeDispatches;
    const auto transfers = after.packedTransfers - windowStart.packedTransfers;
    const auto transferredBytes = after.packedTransferredBytes - windowStart.packedTransferredBytes;
    const auto readbacks = after.fullFrameReadbacks - windowStart.fullFrameReadbacks;
    checks.expect(coldDispatches > 0U, "the frames performed native dispatches");
    checks.expect(transfers == frames.size(),
                  "exactly one final display transfer was made per verified frame");
    checks.expect(readbacks == transfers,
                  "no readback was made besides the final display transfers");
    checks.expect(after.cpuFallbacks == afterReady.cpuFallbacks, "no frame took the CPU path");
    std::uint64_t hostBytes = 0;
    for (const auto& frame : frames) {
        hostBytes += frame->provenance().gpuPreparedPacked->transfer.hostStorageBytes;
    }
    checks.expect(transferredBytes > 0U && transferredBytes == hostBytes,
                  "the transferred bytes are the frames' display pixels");
    std::cout << "PHASE host-painted route=" << routeId << " frames=" << frames.size()
              << " coldDispatches=" << coldDispatches << " transfers=" << transfers
              << " transferredBytes=" << transferredBytes << " hostBytes=" << hostBytes
              << " readbacks=" << readbacks
              << " cpuFallbacks=" << (after.cpuFallbacks - afterReady.cpuFallbacks)
              << " epoch=" << epoch << '\n';

    const auto processDigest = routeproof::frameIdentityDigest(frames);
    const auto capturedEvidenceDigest = routeproof::evidenceDigest(evidence);
    frames.clear();
    controller.beginShutdown();
    bridge.beginShutdown();
    (void)waitUntil([&] { return scheduler.isQuiescent(); }, 10s);
    service.beginShutdown();
    (void)waitUntil(
        [&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; }, 30s);

    if (checks.failures() != 0) {
        std::cerr << "host-painted route proof: " << checks.failures()
                  << " verification failure(s); no proof written\n";
        return 1;
    }
    const auto written = routeproof::publishHostPaintedPreviewProof(
        options.proofDirectory, nonce, routeId,
        viewerRoute ? runtime::GpuRouteHarnessKind::ViewerPreviewHostPainted
                    : runtime::GpuRouteHarnessKind::RamPreviewHostPainted,
        epoch, coldDispatches, evidence.size(), transfers, transferredBytes, processDigest,
        capturedEvidenceDigest);
    if (!written.written) {
        std::cerr << "host-painted route proof was rejected: " << written.detail << '\n';
        return 1;
    }
    std::cout << "PASS: " << routeId << " host-painted proof written (frames=" << evidence.size()
              << ", coldDispatches=" << coldDispatches << ", transfers=" << transfers
              << ", epoch=" << epoch << ")\n";
    return 0;
}
