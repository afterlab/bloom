// Node-specific interactive-viewer evidence (#524, #514).
//
// The generic viewer route proof (gpu_viewer_route_proof_tests.cpp) shows the viewer chain works for
// a pair of Solid layers. This harness drives the same production chain -- real Document and
// CommandStack, the real GpuPreviewDisplayService (resident overload, Wayland presentation), the
// real CompositionPreviewController and the real ViewerEditor presenting the resident frame -- over
// a composition that uses ONE new node, and reports, for that frame:
//
//   * the nodes the composition holds,
//   * that the viewer's production preparation produced a GPU-resident frame (not the CPU reference),
//   * the native dispatches the cold frame performed beyond the same composition without the node,
//   * that no full frame was read back,
//   * that the resident pixels match the CPU oracle at the sampled coordinates, and
//   * that a warm identical refresh performs no new native work.
//
// The scenario is chosen with --scenario <name>; --project <file> runs the same checks over a saved
// project instead (its lowest composition). --cold makes the node's frame the process's first: no
// baseline frame, and the viewer stage the application builds (makeSessionRefreshingGpuSceneStage),
// which resolves the packaged tools lazily on the frame's own worker, so a cold first frame is
// proved to prepare on the GPU. It writes no route proof: it is evidence, and it SKIPs
// when there is no Wayland platform or device (and fails then with --require-device).

#include "gpu_route_proof_harness_support.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/node_operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/document/four_color_gradient_nodes.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/gpu_viewer_bootstrap.hpp>
#include <bloom/ui/project_host.hpp>
#include <bloom/ui/task_ui_bridge.hpp>
#include <bloom/ui/viewer_editor.hpp>

#include <QApplication>
#include <QCoreApplication>
#include <QSettings>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace bloom::ui::verticalproof;
namespace routeproof = bloom::ui::routeproof;
using namespace std::chrono_literals;

namespace {

using namespace bloom;

struct HarnessOptions final {
    std::filesystem::path loader;
    std::filesystem::path project;
    std::string scenario;
    bool requireDevice = false;
    bool cold = false;
    bool valid = true;
};

[[nodiscard]] HarnessOptions parseHarnessOptions(const int argc, char** argv) {
    HarnessOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--loader" && index + 1 < argc) {
            options.loader = argv[++index];
        } else if (argument == "--project" && index + 1 < argc) {
            options.project = argv[++index];
        } else if (argument == "--scenario" && index + 1 < argc) {
            options.scenario = argv[++index];
        } else if (argument == "--require-device") {
            options.requireDevice = true;
        } else if (argument == "--cold") {
            options.cold = true;
        } else {
            options.valid = false;
            return options;
        }
    }
    options.valid = options.valid && (!options.project.empty() || !options.scenario.empty());
    return options;
}

using Values = std::vector<std::pair<std::string, document::ParameterValue>>;

// One composition: a source, optionally one effect on it, into a Layer. The effect is the node the
// frame is about; the baseline is the same composition without it.
struct Scenario final {
    std::string source = "bloom.checker-source";
    Values sourceValues;
    std::string effect; // empty: the source is the node under test
    Values effectValues;
};

[[nodiscard]] std::map<std::string, Scenario> scenarios() {
    using document::Vec2d;
    std::map<std::string, Scenario> out;
    out["four-color-gradient"] = {"bloom.four-color-gradient", {{"blend", 3.0}}, {}, {}};
    out["denoise-median"] = {"bloom.checker-source", {}, "bloom.denoise",
                             {{"mode", std::int64_t{0}}, {"radius", 2.0}, {"strength", 0.1}}};
    out["denoise-bilateral"] = {"bloom.checker-source", {}, "bloom.denoise",
                                {{"mode", std::int64_t{1}}, {"radius", 2.0}, {"strength", 0.5}}};
    out["denoise-spatial"] = {"bloom.checker-source", {}, "bloom.denoise",
                              {{"mode", std::int64_t{2}}, {"radius", 2.0}, {"strength", 0.4}}};
    out["blur-separate"] = {"bloom.checker-source", {}, "bloom.gaussian-blur",
                            {{"radius", 12.0}, {"separate", true}, {"radiusY", 2.0}}};
    out["blur-per-channel"] = {"bloom.checker-source", {}, "bloom.gaussian-blur",
                               {{"radius", 10.0},
                                {"perChannel", true},
                                {"redScale", 1.0},
                                {"greenScale", 0.4},
                                {"blueScale", 0.0}}};
    out["transform-skew"] = {"bloom.four-color-gradient", {}, "bloom.transform-2d",
                             {{"skew", 20.0}, {"skewAxis", 10.0}, {"opacity", 0.7},
                              {"translate", Vec2d{30.0, 0.0}}}};
    out["hue-vibrance"] = {"bloom.four-color-gradient", {}, "bloom.hue-saturation",
                           {{"vibrance", 1.0}}};
    out["hue-colorize"] = {"bloom.four-color-gradient", {}, "bloom.hue-saturation",
                           {{"colorize", true}, {"hue", 200.0}, {"saturation", 1.0}}};
    return out;
}

// The harness's oracle identity check, except that the colour identity compares the working space
// and configuration URI: the production GPU stage resolves the configuration revision into the
// frame (the CPU oracle stamps the unresolved linear Rec.709 identity). Plan, time, output,
// quality, ROI and evaluated geometry must still match exactly.
[[nodiscard]] bool sameOracleIdentity(const runtime::PreparedPreviewFrame& gpu,
                                      const runtime::PreparedPreviewFrame& cpu,
                                      std::string& detail) {
    const auto& g = gpu.processIdentity();
    const auto& c = cpu.processIdentity();
    if (g.plan == nullptr || c.plan == nullptr || !(*g.plan == *c.plan)) {
        detail = "the production plan differs from the CPU oracle's plan";
        return false;
    }
    if (g.time != c.time || g.output.value() != c.output.value() || g.quality != c.quality ||
        g.colorIntent.workingColorSpaceId != c.colorIntent.workingColorSpaceId ||
        g.colorIntent.ocioConfigUri != c.colorIntent.ocioConfigUri || g.roi != c.roi ||
        g.bypassLookNodes != c.bypassLookNodes) {
        detail = "the production request identity differs from the CPU oracle's";
        return false;
    }
    const auto gb = gpu.evaluatedBounds();
    const auto cb = cpu.evaluatedBounds();
    if (gb.size() != cb.size() || !std::equal(gb.begin(), gb.end(), cb.begin())) {
        detail = "the production evaluated geometry differs from the CPU oracle's";
        return false;
    }
    return true;
}

[[nodiscard]] std::string nodeList(const document::Composition& composition) {
    std::string text;
    for (const auto& node : composition.graph().nodes())
        text += (text.empty() ? "" : ",") + node.typeId;
    return text;
}

bool runTransaction(ui::CompositionSession& session, commands::Transaction transaction) {
    return session.executeNodeTransaction(std::move(transaction)).changed();
}

[[nodiscard]] document::NodeId addNode(ui::CompositionSession& session, const std::string& type) {
    commands::Transaction transaction("Add", session.snapshot().revision());
    transaction.emplace<commands::AddNode>(session.compositionId(), type, document::Vec2d{});
    return session.executeNodeTransaction(std::move(transaction))
        .outputId<document::NodeId>(commands::kAddNodeOutput)
        .value_or(document::NodeId{});
}

[[nodiscard]] bool connect(ui::CompositionSession& session, const document::NodeId from,
                           const document::NodeId to, const std::string& port) {
    const auto* node = session.composition()->graph().findNode(from);
    const auto* definition =
        node ? document::builtInNodeDefinitions().find(node->typeId, node->schemaVersion)
             : nullptr;
    if (definition == nullptr || definition->outputs.empty())
        return false;
    commands::Transaction transaction("Connect", session.snapshot().revision());
    transaction.emplace<commands::ConnectPorts>(
        session.compositionId(), document::OutputPortRef{from, definition->outputs.front().name},
        document::NodeInputRef{to, port});
    return runTransaction(session, std::move(transaction));
}

// The node feeding the first Layer's content, found through its edge.
[[nodiscard]] std::pair<document::NodeId, document::NodeId>
layerAndContent(const ui::CompositionSession& session) {
    for (const auto& edge : session.composition()->graph().edges())
        if (const auto* input = std::get_if<document::NodeInputRef>(&edge.destination);
            input != nullptr && input->port == document::kLayerOutputContentInputPort) {
            // The composition output has an "image" input too: only a Layer's counts.
            const auto* node = session.composition()->graph().findNode(input->nodeId);
            if (node != nullptr && node->typeId == "bloom.layer-output")
                return {input->nodeId, edge.source.nodeId};
        }
    return {};
}

[[nodiscard]] bool setValues(ui::CompositionSession& session, const document::NodeId node,
                             const Values& values) {
    bool ok = true;
    for (const auto& [role, value] : values)
        ok = session.setNodeParameterValue(node, role, value, QStringLiteral("Set")) && ok;
    return ok;
}

} // namespace

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    const HarnessOptions options = parseHarnessOptions(argc, argv);
    if (!options.valid) {
        std::cerr << "usage: --loader <vulkan loader> (--scenario <name> | --project <file>) "
                     "[--require-device]\n";
        return 2;
    }
    QApplication application(argc, argv);
    const QString platformName = QApplication::platformName();
    if (platformName != QStringLiteral("wayland") &&
        platformName != QStringLiteral("wayland-egl")) {
        std::cout << "SKIP: node viewer evidence needs a real Wayland platform (platform="
                  << platformName.toStdString() << ")\n";
        return options.requireDevice ? 1 : 0;
    }
    if (options.loader.empty()) {
        std::cout << "SKIP: node viewer evidence needs --loader\n";
        return options.requireDevice ? 1 : 0;
    }
    qputenv("QT_VULKAN_LIB", options.loader.string().c_str());

    const auto settingsRoot =
        std::filesystem::temp_directory_path() /
        ("bloom-node-viewer-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(settingsRoot);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       QString::fromStdString(settingsRoot.string()));
    QCoreApplication::setOrganizationName(QStringLiteral("BloomNodeViewer"));
    QCoreApplication::setApplicationName(QStringLiteral("NodeViewerEvidence"));

    Expectations checks;
    const auto mediaDirectory =
        std::filesystem::temp_directory_path() /
        ("bloom-node-viewer-media-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(mediaDirectory);
    Fixture fixture(mediaDirectory);
    if (fixture.processor == nullptr) {
        std::cout << "SKIP: the Bloom Neutral v1 processor is unavailable\n";
        return options.requireDevice ? 1 : 0;
    }

    TaskScheduler scheduler(schedulerConfig());
    const runtime::CpuReferenceDisplayPreparer displayPreparer;
    // The production shape: the GPU scene builder carries the shared OCIO context resolved from the
    // packaged tools (without it field programs and effects are outside the GPU subset).
    runtime::GpuOcioContextRequest ocioRequest;
    ocioRequest.applicationExecutable = std::filesystem::path{BLOOM_NODE_EVIDENCE_EXECUTABLE};
    ocioRequest.toolPackage.toolsDirectory = BLOOM_GPU_TOOLS_DIR;
    ocioRequest.toolPackage.inventoryName = BLOOM_GPU_TOOLS_INVENTORY_NAME;
    ocioRequest.toolPackage.glslangValidatorName = BLOOM_GPU_TOOLS_GLSLANG_NAME;
    ocioRequest.toolPackage.spirvValName = BLOOM_GPU_TOOLS_SPIRV_VAL_NAME;
    ocioRequest.toolPackage.relocated = static_cast<bool>(BLOOM_GPU_TOOLS_RELOCATED);
#ifdef BLOOM_GPU_TOOLS_BUNDLE_RELATIVE
    ocioRequest.toolPackage.bundleRelative = true;
#endif
#ifdef BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256
    ocioRequest.toolPackage.glslangStagedDigest = bloom::core::Sha256Digest::fromLowercaseHex(
        std::string_view{BLOOM_GPU_TOOLS_GLSLANG_STAGED_SHA256}.substr(7));
#endif
#ifdef BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256
    ocioRequest.toolPackage.spirvValStagedDigest = bloom::core::Sha256Digest::fromLowercaseHex(
        std::string_view{BLOOM_GPU_TOOLS_SPIRV_VAL_STAGED_SHA256}.substr(7));
#endif
    auto ocioResolver = std::make_shared<runtime::GpuOcioContextResolver>(std::move(ocioRequest));
    runtime::GpuSceneOcioContext ocioContext;
    if (!options.cold) {
        runtime::GpuOcioCancellation never = [] { return false; };
        auto resolved = ocioResolver->resolve(never);
        if (!resolved.hasValue()) {
            std::cout << "SKIP: the packaged GPU tools are unavailable\n";
            return options.requireDevice ? 1 : 0;
        }
        ocioContext = *resolved.context;
    }
    auto sceneMediaContext = runtime::GpuSceneMediaContext::fromEvaluator(fixture.evaluator);
    sceneMediaContext.assetBaseDirectory = mediaDirectory;
    const runtime::CpuGpuSceneBuilder sceneBuilder(nullptr, std::move(sceneMediaContext),
                                                   std::move(ocioContext));
    // Cold: the stage the application builds, resolving the tools on the first frame's worker.
    auto gpuStage =
        options.cold
            ? ui::makeSessionRefreshingGpuSceneStage(fixture.compiler, fixture.evaluator,
                                                     fixture.provider, nullptr, nullptr, ocioResolver)
            : ui::makeCompositionPreviewGpuSceneStage(fixture.compiler, sceneBuilder,
                                                      fixture.provider);
    GpuPreviewDisplayService service(scheduler, std::move(gpuStage), fixture.productionCpuStage(),
                                     fixture.productionCpuFallback(displayPreparer),
                                     serviceOptions(options.loader));

    // The document: a saved project, or the scenario's composition.
    ui::ProjectHost projectHost(scheduler);
    std::optional<document::Document> ownDocument;
    std::unique_ptr<commands::CommandStack> ownStack;
    document::Document* documentPointer = nullptr;
    commands::CommandStack* stackPointer = nullptr;
    document::CompositionId compositionId{};
    if (!options.project.empty()) {
        bool replaced = false;
        std::optional<ui::ProjectHostOperationOutcome> outcome;
        QString message;
        QObject::connect(&projectHost, &ui::ProjectHost::sessionReplaced,
                         [&] { replaced = true; });
        QObject::connect(&projectHost, &ui::ProjectHost::openFinished,
                         [&](const ui::ProjectHostOperationOutcome result, const QString& text) {
                             outcome = result;
                             message = text;
                         });
        projectHost.setUnsavedChangeDecisionProvider(
            [] { return ui::UnsavedChangeDecision::Discard; });
        const auto path = std::filesystem::absolute(options.project);
        projectHost.setOpenPathProvider([&] { return std::optional(path); });
        projectHost.requestOpen();
        if (!waitUntil([&] { return replaced || outcome.has_value(); }, 120s) || !replaced) {
            std::cerr << "could not open the project: " << message.toStdString() << '\n';
            return 1;
        }
        fixture.evaluator.setAssetBaseDirectory(path.parent_path());
        auto [document, stack] = projectHost.liveDocumentAndStack();
        documentPointer = document;
        stackPointer = stack;
        compositionId = projectHost.lowestCompositionId();
    } else {
        auto newProject = document::makeNewProject("Node Viewer Evidence", "Main",
                                                   core::RationalTime::fromInteger(10),
                                                   format1920x1080NonSquare());
        compositionId = newProject.initialCompositionId;
        ownDocument.emplace(std::move(newProject.project));
        ownStack = std::make_unique<commands::CommandStack>(*ownDocument);
        documentPointer = &*ownDocument;
        stackPointer = ownStack.get();
    }
    if (documentPointer == nullptr || stackPointer == nullptr) {
        std::cerr << "the document is unavailable\n";
        return 1;
    }
    ui::CompositionSession session(*documentPointer, *stackPointer, compositionId);

    const Scenario* scenario = nullptr;
    static const auto table = scenarios();
    if (options.project.empty()) {
        const auto found = table.find(options.scenario);
        if (found == table.end()) {
            std::cerr << "unknown scenario " << options.scenario << '\n';
            return 2;
        }
        scenario = &found->second;
        checks.expect(session.addSolidLayer(QStringLiteral("Layer"),
                                            core::Color4d{0.2, 0.2, 0.2, 1.0}),
                      "the baseline layer is created");
    }

    const bool terminal = waitUntil(
        [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; }, 90s);
    const auto status = service.status();
    const bool residentReady =
        terminal && status.state == GpuPreviewDisplayServiceState::Ready &&
        status.residentQualification != nullptr && status.residentQualification->eligible() &&
        status.presentationClient != nullptr &&
        status.presentationAvailability == render::GpuPresentationAvailability::Ready;
    if (!residentReady) {
        std::cout << "SKIP: viewer resident route unavailable: " << status.residentDetail << " / "
                  << status.presentationDetail << '\n';
        service.beginShutdown();
        return options.requireDevice ? 1 : 0;
    }
    const std::shared_ptr<GpuPresentationClient> client = status.presentationClient;

    ui::TaskUiBridge bridge(scheduler, nullptr, std::chrono::milliseconds(1));
    auto pipeline = ui::makeCompositionPreviewPipeline(fixture.compiler, fixture.evaluator,
                                                       displayPreparer, fixture.provider);
    ui::CompositionPreviewSettings settings;
    settings.pixelStorageByteLimit = kBudget;
    ui::PreviewPreparationSubmitter submitter =
        [&service](runtime::TaskRequest request, const document::Snapshot& snap,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides)
        -> runtime::TaskSubmission<runtime::PreviewPreparationResultHandle> {
        return service.submit(std::move(request), snap, identity, limit, overrides);
    };
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline, settings,
                                                nullptr, nullptr, submitter);

    auto host = std::make_unique<QWidget>();
    host->setWindowTitle(QString::fromStdString(
        "BloomNodeViewer-" + std::to_string(QCoreApplication::applicationPid())));
    host->resize(960, 640);
    auto* layout = new QVBoxLayout(host.get());
    layout->setContentsMargins(0, 0, 0, 0);
    auto viewer = std::make_unique<ui::ViewerEditor>(session, controller);
    layout->addWidget(viewer.get());
    host->show();
    viewer->show();
    viewer->setGpuPresentationDependencies(client, &scheduler, options.loader.string(), 1.0);

    // Show the current document through the viewer; returns the displayed frame and the native
    // dispatches the frame performed, or null when the viewer never presented a resident frame.
    struct Shown final {
        std::shared_ptr<const runtime::PreparedPreviewFrame> frame;
        std::uint64_t dispatches = 0;
    };
    const auto present = [&](const std::string& label) -> Shown {
        const auto before = service.status().counters;
        const auto previous = controller.state().frame;
        controller.requestRefresh();
        const bool presented = waitUntil(
            [&] {
                const auto frame = controller.state().frame;
                return frame != nullptr && frame != previous &&
                       frame->provenance().provider == runtime::PreviewDisplayProvider::GpuResident &&
                       viewer->residentPresentationActiveForTest();
            },
            45s);
        const auto frame = controller.state().frame;
        checks.expect(presented && frame != nullptr,
                      label + ": the viewer presented a new resident frame");
        if (!presented || frame == nullptr) {
            {
                const auto state = controller.state();
                std::cout << label << ": activity=" << static_cast<int>(state.activity)
                          << " freshness=" << static_cast<int>(state.freshness)
                          << " message=" << state.message.toStdString() << '\n';
                const auto counters = service.status().counters;
                std::cout << "  cpuFallbacks=" << counters.cpuFallbacks
                          << " residentFailures=" << counters.residentFailures << " byReason=";
                for (const auto value : counters.cpuFallbacksByReason)
                    std::cout << value << ',';
                std::cout << '\n';
                for (const auto& diagnostic : state.diagnostics)
                    std::cout << "  diagnostic: " << diagnostic.code << " " << diagnostic.summary << " " << diagnostic.detail << '\n';
            }
            if (frame != nullptr)
                std::cout << label << ": displayed provider="
                          << static_cast<int>(frame->provenance().provider)
                          << " residentPresentationActive="
                          << viewer->residentPresentationActiveForTest()
                          << " newFrame=" << (frame != previous) << '\n';
            return {};
        }
        const auto after = service.status().counters;
        checks.expect(after.fullFrameReadbacks == before.fullFrameReadbacks,
                      label + ": the resident frame performed no full-frame readback");
        return {frame, after.nativeDispatches - before.nativeDispatches};
    };
    const auto verify = [&](const Shown& shown, const std::string& label) {
        if (shown.frame == nullptr)
            return;
        const auto reference = snapshotCpuReferenceAtResolution(fixture, session.snapshot(),
                                                                shown.frame->desiredIdentity());
        checks.expect(reference != nullptr && reference->displayBufferView().has_value(),
                      label + ": the CPU oracle evaluates the displayed identity");
        if (reference == nullptr)
            return;
        std::string detail;
        checks.expect(sameOracleIdentity(*shown.frame, *reference, detail),
                      label + ": the resident frame matches the oracle identity: " + detail);
        routeproof::FrameEvidence evidence;
        checks.expect(routeproof::buildFrameEvidence(service, *shown.frame, *reference, evidence,
                                                     detail),
                      label + ": the resident GPU pixels match the CPU oracle: " + detail);
    };

    std::uint64_t baselineDispatches = 0;
    if (scenario != nullptr) {
        // The baseline: the same composition, a plain source, without the node under test. A cold
        // run has none: the node's frame is the first the process prepares.
        if (!options.cold)
            baselineDispatches = present("baseline").dispatches;
        // Build the scenario's chain between the source and the Layer.
        const auto [layerNode, solid] = layerAndContent(session);
        checks.expect(layerNode.isValid() && solid.isValid(), "the layer's content is found");
        const auto source = addNode(session, scenario->source);
        checks.expect(source.isValid() && setValues(session, source, scenario->sourceValues),
                      "the source node is added");
        document::NodeId last = source;
        if (!scenario->effect.empty()) {
            const auto effect = addNode(session, scenario->effect);
            checks.expect(effect.isValid() && connect(session, source, effect, "input") &&
                              setValues(session, effect, scenario->effectValues),
                          "the effect node is added and wired");
            last = effect;
        }
        checks.expect(connect(session, last, layerNode, std::string(document::kLayerOutputContentInputPort)),
                      "the chain feeds the layer");
    }

    const auto composition = nodeList(*session.composition());
    const auto cold = present("scene");
    verify(cold, "scene");
    checks.expect(cold.frame == nullptr || cold.dispatches > 0U,
                  "the cold viewer frame performed real native dispatches");

    // Warm identical refresh: the content cache serves it, with no new native work.
    const auto beforeWarm = service.status().counters;
    const auto presentBefore = viewer->gpuPresentAcceptedCountForTest();
    controller.requestRefresh();
    const bool warm = waitUntil(
        [&] { return viewer->gpuPresentAcceptedCountForTest() > presentBefore; }, 20s);
    const auto afterWarm = service.status().counters;
    checks.expect(warm, "the warm viewer refresh re-presented");
    checks.expect(afterWarm.nativeDispatches == beforeWarm.nativeDispatches,
                  "the warm viewer refresh performed zero additional native dispatches");

    if (options.cold)
        checks.expect(ocioResolver->counters().resolves == 1,
                      "cold: the first frame resolved the packaged tools on its own worker");
    std::cout << "NODE-VIEWER scenario="
              << (options.project.empty() ? options.scenario : options.project.filename().string())
              << (options.cold ? " cold=1" : "")
              << " nodes=[" << composition << "] provider="
              << (cold.frame != nullptr &&
                          cold.frame->provenance().provider ==
                              runtime::PreviewDisplayProvider::GpuResident
                      ? "GpuResident"
                      : "NOT-RESIDENT")
              << " baselineDispatches=" << baselineDispatches << " sceneDispatches=" << cold.dispatches
              << " fullFrameReadbacks=" << afterWarm.fullFrameReadbacks
              << " warmDispatches=" << afterWarm.nativeDispatches - beforeWarm.nativeDispatches
              << '\n';

    bool retired = false;
    static_cast<void>(viewer->prepareNativeSurfaceMutation(
        93, [&](const std::uint64_t, const ui::EditorNativeSurface::PrepareResult& result) {
            retired = result.safeToMutate;
        }));
    checks.expect(waitUntil([&] { return retired; }, 20s),
                  "the viewer native surface retired before teardown");
    viewer->setGpuPresentationDependencies(nullptr, nullptr, std::string{}, 1.0);
    viewer.reset();
    host.reset();
    controller.beginShutdown();
    bridge.beginShutdown();
    (void)waitUntil([&] { return scheduler.isQuiescent(); }, 10s);
    service.beginShutdown();
    (void)waitUntil(
        [&] { return service.status().state == GpuPreviewDisplayServiceState::Stopped; }, 30s);
    std::error_code ignored;
    std::filesystem::remove_all(mediaDirectory, ignored);
    std::filesystem::remove_all(settingsRoot, ignored);

    if (checks.failures() != 0) {
        std::cerr << "node viewer evidence: " << checks.failures() << " verification failure(s)\n";
        return 1;
    }
    std::cout << "PASS: node viewer evidence\n";
    return 0;
}
