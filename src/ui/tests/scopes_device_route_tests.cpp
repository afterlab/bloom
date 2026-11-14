// bloom.ui.scopes-device-route (#724 Scopes): the Scopes panel's device route, end to end, with no
// window system.
//
// The real preview display service in the application's configuration (GPU-prepared packed
// product, presentation Disabled, a declared card, the application's GPU scene stage and shared
// OCIO resolver), the real preview controller and RAM preview controller submitting to it, and a
// ScopesController with the production measure over a ScopeFillTaps owner. Asserted:
//   * a shown frame is measured through the Viewer's own scene picture (the scope tap): Rendered,
//     route GPU, and its Working tables equal the CPU reference cell for cell;
//   * the scopes ran no scene: the measures of the shown frame add no scene stage run, no resident
//     graph job, no native display dispatch and no picture readback;
//   * a fill with the panel closed is unchanged and pays nothing: no flag, no tap, no measure;
//   * a frame the Viewer shows while the fill renders it (one key, a Viewer tap and a fill tap):
//     the frame is counted once, one Rendered sidecar is kept, the shown-frame request and the
//     fill's request are both answered, the fill slot is freed and the Viewer's tap survives the
//     fill tap's release;
//   * a small fill with the panel open leaves full-detail Working tables, marked Rendered and equal
//     to the CPU reference, beside every retained frame, again with no scene run added; every flagged
//     frame was answered by its measure, and the fill never stalled.
// Skips (exit 77) without a device and fails under --require-device.

#include "await_qt.hpp"
#include "fixed_memory_budget.hpp"

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/commands/command_stack.hpp>
#include <bloom/commands/operations.hpp>
#include <bloom/commands/transaction.hpp>
#include <bloom/core/color.hpp>
#include <bloom/core/rational_time.hpp>
#include <bloom/document/document.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/host/gpu_export_tool_package.hpp>
#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/render/scope_table_layout.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/node_definition_registry.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/reference_display_preparation.hpp>
#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_plan_cache.hpp>
#include <bloom/ui/composition_preview_controller.hpp>
#include <bloom/ui/composition_preview_cpu_stage.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/gpu_viewer_bootstrap.hpp>
#include <bloom/ui/preview_frame_cache.hpp>
#include <bloom/ui/ram_preview_controller.hpp>
#include <bloom/ui/scope_fill_taps.hpp>
#include <bloom/ui/scopes_controller.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QCoreApplication>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace bloom;
using namespace std::chrono_literals;
using bloom::test_support::awaitFailureCount;
using bloom::test_support::awaitQtCondition;

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

struct Options final {
    std::filesystem::path loader;
    bool requireDevice = false;
    bool valid = true;
};

[[nodiscard]] Options parseOptions(const int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index] == nullptr ? std::string{} : argv[index];
        if (argument == "--loader" && index + 1 < argc && argv[index + 1] != nullptr) {
            options.loader = argv[++index];
        } else if (argument == "--require-device") {
            options.requireDevice = true;
        } else {
            std::cerr << "unknown argument: " << argument << '\n';
            options.valid = false;
        }
    }
    return options;
}

#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE

constexpr std::uint32_t kSize = 256;
constexpr std::size_t kBudget = std::size_t{512} * 1024U * 1024U;
// The card the service plans for (the declared card of the native tests), so no result depends on
// the machine's real free device memory.
constexpr std::uint64_t kDeclaredCard = std::uint64_t{8} << 30U;

[[nodiscard]] core::RationalTime time(const std::int64_t numerator, const std::int64_t denominator) {
    const auto value = core::RationalTime::create(numerator, denominator);
    if (!value.has_value())
        std::abort();
    return *value;
}

// The retirement worker the packed products free their host memory on: closed and collected only
// after the service, the scheduler and every product alias are gone.
struct PackedRetirementScope final {
    std::shared_ptr<render::PackedHostRetirementOwner> owner =
        render::PackedHostRetirementOwner::create();
    PackedRetirementScope() {
        if (!owner->prepare())
            throw std::runtime_error("packed retirement worker startup failed");
    }
    ~PackedRetirementScope() {
        owner->beginShutdown();
        for (int turn = 0; turn < 3000 && !owner->exitAcknowledged(); ++turn)
            std::this_thread::sleep_for(10ms);
        if (!owner->exitAcknowledged() || !owner->collect()) {
            std::cerr << "packed retirement did not exit and collect\n";
            std::terminate();
        }
    }
};

[[nodiscard]] runtime::TaskSchedulerConfig schedulerConfig() {
    runtime::TaskSchedulerConfig config;
    config.cpuWorkerCount = 4;
    config.rowBandWorkerCount = runtime::kSerialRowBandWorkers;
    config.blockingIoWorkerCount = 1;
    config.cpuQueueCapacity = 64;
    config.blockingIoQueueCapacity = 8;
    config.gpuPendingQueueCapacity = 8;
    config.gpuAdmittedStateCapacity = 16;
    config.gpuLiveContinuationCapacity = 8;
    config.gpuQueuedCommandByteCapacity = std::size_t{1} << 30U;
    config.gpuRequestOwnedByteCapacity = std::size_t{1} << 30U;
    config.terminalHistoryCapacity = 64;
    config.diagnosticsPerTask = 16;
    config.groupRegistryCapacity = 16;
    return config;
}

// The processing root the service admits against, sampled from off the interface thread.
[[nodiscard]] std::shared_ptr<runtime::GpuProcessingPhasePolicy>
processingRoot(const std::shared_ptr<render::PackedHostRetirementOwner>& retirement) {
    auto sampled = std::async(std::launch::async, [retirement] {
        auto policy = std::make_shared<runtime::GpuProcessingPhasePolicy>(32, retirement);
        auto sample = bloom::test_support::fixedMemoryBudgetSample();
        runtime::MemoryBudgetLedger ledger(sample.physicalBytes,
                                           sample.machine.availableBytes.value_or(0));
        ledger.beginProcessSampling();
        const auto at = runtime::GpuProcessingPhasePolicy::Clock::now();
        const auto state = ledger.applySample(std::move(sample), at);
        policy->applyHostSample(state, ledger.physicalByteBudget(), at);
        return policy;
    });
    if (!awaitQtCondition("the off-UI processing sample finishes", [&] {
            return sampled.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }))
        throw std::runtime_error("The off-UI processing sample did not finish.");
    return sampled.get();
}

int run(const Options& options) {
    Expectations e;
    PackedRetirementScope retirement;
    runtime::TaskScheduler scheduler(schedulerConfig());
    ui::TaskUiBridge bridge(scheduler, nullptr, 1ms);
    const auto policy = processingRoot(retirement.owner);

    // The application's pipeline pieces.
    runtime::NodeDefinitionRegistry definitions;
    runtime::SnapshotCompiler compiler(definitions);
    if (!runtime::registerBuiltInNodeDefinitions(definitions))
        return 1;
    definitions.freeze();
    runtime::CpuCompositionEvaluator evaluator;
    runtime::CpuReferenceDisplayPreparer displayPreparer;
    runtime::QualifiedDisplayProcessorProvider provider;
    provider.publish(runtime::buildBloomNeutralQualifiedDisplayProcessor());
    auto planCache = std::make_shared<ui::CompiledPlanCache>();
    auto coverageCache = std::make_shared<runtime::GpuSceneCoverageCache>();
    std::shared_ptr<runtime::GpuOcioContextResolver> resolver = host::makePackagedGpuOcioResolver(
        std::filesystem::path(QCoreApplication::applicationFilePath().toStdString()));
    if (resolver == nullptr) {
        std::cerr << "the packaged GPU tools are not available to a resolver\n";
        return options.requireDevice ? 1 : 77;
    }
    const auto resolved = resolver->resolve();
    if (!resolved.hasValue()) {
        std::cerr << "the packaged tools do not resolve to a shared OCIO context: "
                  << resolved.diagnostic << '\n';
        return 1;
    }
    const auto ocio = resolved.context;

    runtime::GpuPreviewDisplayServiceOptions serviceOptions;
    serviceOptions.enabled = true;
    serviceOptions.loaderPath = options.loader;
    serviceOptions.product = runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
    serviceOptions.presentation = runtime::GpuPreviewDisplayServicePresentationMode::Disabled;
    serviceOptions.deviceMemoryLimitBytes = kDeclaredCard;
    serviceOptions.previewByteAllowance = kBudget;
    serviceOptions.processingPhasePolicy = policy;
    serviceOptions.nativeDispatchDeadline = std::chrono::minutes(10);
    // The test is not about the idle trim, which releases the Viewer's tap.
    serviceOptions.idleTrimDelay = std::chrono::minutes(10);

    auto gpuStage = ui::makeSessionRefreshingGpuSceneStage(
        compiler, evaluator, provider, coverageCache, planCache, resolver,
        {.policy = policy,
         .envelope = {.metadataBytes = 16ULL * 1024 * 1024,
                      .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                      .growsRetainedSceneStorage = true,
                      .requestedExecutorBytes = std::nullopt,
                      .deviceWorkingBytes =
                          512ULL * 1024 * 1024 + serviceOptions.packedTransferByteAllowance}});
    // Every scene the service prepares for a frame goes through here: the count a measure of a
    // frame must never move.
    const auto sceneStages = std::make_shared<std::atomic<int>>(0);
    runtime::PreviewGpuSceneStageFunction countedStage =
        [stage = std::move(gpuStage),
         sceneStages](const document::Snapshot& snapshot,
                      const runtime::PreviewRequestIdentity& identity, const std::size_t bytes,
                      const std::vector<runtime::SnapshotParameterOverride>& overrides,
                      runtime::TaskContext& context) {
            ++*sceneStages;
            return stage(snapshot, identity, bytes, overrides, context);
        };
    serviceOptions.processingPhasePolicy = policy;
    runtime::GpuPreviewDisplayService service(
        scheduler, std::move(countedStage),
        ui::makeCompositionPreviewCpuStage(compiler, evaluator, provider, planCache),
        ui::makeCompositionPreviewCpuDisplayFallback(displayPreparer), serviceOptions);
    const bool left = awaitQtCondition("the service leaves Initializing", [&] {
        return service.status().state != runtime::GpuPreviewDisplayServiceState::Initializing;
    });
    if (!left || service.status().state != runtime::GpuPreviewDisplayServiceState::Ready) {
        std::cout << "SKIP: no device for the packed route: " << service.status().residentDetail
                  << '\n';
        service.beginShutdown();
        bridge.beginShutdown();
        return options.requireDevice ? 1 : 77;
    }

    // The document: one solid, 256 x 256, six frames.
    const auto format = document::CompositionFormat::create(kSize, kSize, core::PixelAspectRatio::square(),
                                                            *document::FrameRate::create(25, 1));
    auto created = document::makeNewProject("Scopes device route", "Main", time(6, 25), *format);
    document::Document document(std::move(created.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, created.initialCompositionId);
    e.expect(session.addSolidLayer(QStringLiteral("Solid"), core::Color4d{0.7, 0.2, 0.05, 1.0}),
             "the solid layer is added");

    auto frameCache = std::make_shared<ui::PreviewFrameCache>();
    const ui::PreviewPreparationSubmitter submitter =
        [&service](runtime::TaskRequest request, const document::Snapshot& snapshot,
                   const runtime::PreviewRequestIdentity& identity, const std::size_t limit,
                   const std::vector<runtime::SnapshotParameterOverride>& overrides) {
            return service.submit(std::move(request), snapshot, identity, limit, overrides);
        };
    const auto pipeline = ui::makeCompositionPreviewPipeline(compiler, evaluator, displayPreparer,
                                                             provider, planCache);
    ui::CompositionPreviewSettings previewSettings;
    previewSettings.resolutionPolicy = runtime::PreviewResolutionPolicy::Full;
    previewSettings.colorIntent = session.colorIntent();
    previewSettings.pixelStorageByteLimit = kBudget;
    ui::CompositionPreviewController controller(session, scheduler, bridge, pipeline,
                                                previewSettings, frameCache, nullptr, submitter,
                                                nullptr, policy);
    ui::ScopeFillTaps taps(&service, frameCache, bridge);
    ui::RamPreviewController ram(session, controller, scheduler, bridge, pipeline, nullptr,
                                 submitter);
    ram.setScopeFillTaps(&taps);

    // The CPU reference: the same frame evaluated by the CPU evaluator and counted by the CPU
    // reference counting.
    auto config = color::resolveOcioBuiltIn(
                      color::OcioConfigLocatorKind::BloomBuiltIn, color::kBloomNeutralV1ConfigUri,
                      color::ocioBuiltInContentRevision(color::OcioConfigLocatorKind::BloomBuiltIn,
                                                        color::kBloomNeutralV1ConfigUri)
                          .value_or(color::kBloomNeutralV1ConfigDigest),
                      "lin_rec709_scene")
                      .takeResolved();
    if (!config)
        return 1;
    const auto sharedConfig =
        std::make_shared<const color::ResolvedBloomNeutralConfig>(std::move(*config));
    auto prepared = runtime::ScopeSignalColor::prepare(
        {.config = sharedConfig,
         .signal = runtime::ScopeSignal::Working,
         .display = {},
         .view = {},
         .viewAdjust = {}});
    if (!prepared) {
        std::cerr << "the Working signal's color objects did not prepare: " << prepared.message
                  << '\n';
        return 1;
    }
    const auto referenceOf = [&](const core::RationalTime frameTime,
                                 const runtime::ScopeMeasureSettings& settings)
        -> std::optional<runtime::ScopeFrameMeasure> {
        const auto compiled = compiler.compile(
            {.snapshot = session.evaluationSnapshotForTime(frameTime),
             .compositionId = session.compositionId(),
             .parameterOverrides = {}},
            {});
        if (compiled.status != runtime::SnapshotCompileStatus::Compiled || compiled.plan == nullptr)
            return std::nullopt;
        const auto evaluated = evaluator.evaluate(
            compiled.plan,
            {.time = frameTime,
             .output = compiled.plan->output(),
             .resolution = runtime::CompositionFormatResolution{},
             .quality = runtime::EvaluationQuality::Reference,
             .colorIntent = session.colorIntent(),
             .pixelStorageByteLimit = kBudget},
            {});
        if (evaluated.frame() == nullptr)
            return std::nullopt;
        return runtime::measureScopeFrameCpu(*evaluated.frame(), settings, *prepared.color);
    };

    e.expect(awaitQtCondition("the foreground frame is ready",
                              [&] { return controller.state().activity == ui::PreviewActivity::Ready; }),
             "the foreground frame is ready");
    const auto shown = controller.state().frame;
    e.expect(shown != nullptr &&
                 shown->provenance().provider == runtime::PreviewDisplayProvider::GpuPreparedPacked,
             "the Viewer's frame is the GPU-prepared packed one (no CPU fallback)");
    if (shown == nullptr)
        return 1;
    e.expect(service.status().counters.cpuFallbacks == 0U, "no Viewer frame fell back to the CPU");

    ui::ScopesController scopes(
        scheduler, bridge, frameCache,
        ui::makeScopesMeasure({.previewService = &service,
                               .ocio = [ocio] { return ocio; }}),
        &taps);
    runtime::ScopeMeasureSettings settings;
    settings.scopes = render::kScopeTableHistogram | render::kScopeTableWaveform |
                      render::kScopeTableVectorscope;
    settings.signal = runtime::ScopeSignal::Working;
    const auto fullSettings = settings;
    const auto settled = [&] {
        return scopes.reading().state == ui::ScopesReading::State::Ready && !scopes.busy();
    };
    const auto tablesEqual = [](const ui::PreviewScopeSidecarHandle& sidecar,
                                const runtime::ScopeFrameMeasure& reference) {
        return sidecar != nullptr && reference.status == runtime::ScopeFrameStatus::Measured &&
               sidecar->tables.unpack() == reference.counts;
    };

    // ---- A shown frame: the Viewer's own picture, counted on its device ------------------------
    {
        const auto scenesBefore = sceneStages->load();
        const auto before = service.status().counters;
        scopes.setSettings(settings);
        scopes.showFrame(shown, {});
        e.expect(awaitQtCondition("the shown frame is measured, live then full", settled),
                 "the shown frame is measured, live then full: " + scopes.reading().message);
        const auto reading = scopes.reading().scopes;
        const auto reference = referenceOf(shown->desiredIdentity().time, fullSettings);
        e.expect(reading != nullptr && reading->identity.source == runtime::ScopeSource::Rendered &&
                     reading->tables.route() == runtime::ScopeRoute::Gpu &&
                     reading->identity.settings.detail == 0U,
                 "the shown frame's tables are Rendered, counted on the device, at full detail");
        e.expect(reference.has_value() && tablesEqual(reading, *reference),
                 "the Working tables of the tap equal the CPU reference cell for cell");
        const auto after = service.status().counters;
        e.expect(after.scopeMeasuresCompleted >= before.scopeMeasuresCompleted + 2 &&
                     scopes.measuresStarted() == 2,
                 "two measures completed on the device: live, then full");
        e.expect(sceneStages->load() == scenesBefore &&
                     after.residentGraphJobs == before.residentGraphJobs &&
                     after.nativeDispatches == before.nativeDispatches &&
                     after.fullFrameReadbacks == before.fullFrameReadbacks &&
                     after.cpuFallbacks == before.cpuFallbacks,
                 "the measures ran no scene, no graph job, no display dispatch and read no picture "
                 "back");
    }

    // ---- A frame that is both the Viewer's and a fill frame --------------------------------------------
    // The correct behavior: the frame is counted once from its float picture (the shown-frame
    // request's measures, none for the fill), one Rendered sidecar is kept beside it (the fill adds
    // none), the shown-frame request and the fill's request are both answered (the first by its
    // tables, the second by letting its tap go), neither slot's tap is lost to the other's release,
    // and the fill slot is freed so the fill is not held.
    {
        const auto shownKey =
            std::optional(ui::PreviewFrameCacheKey::forIdentity(shown->desiredIdentity()));
        const auto tapKey = runtime::GpuScopeTapKey::of(shown->desiredIdentity());
        const auto fullIdentity = ui::PreviewScopeIdentity{
            .settings = fullSettings, .viewAdjust = {}, .source = runtime::ScopeSource::Rendered};
        const auto sidecarBefore = shownKey ? frameCache->scopesFor(*shownKey, fullIdentity) : nullptr;
        e.expect(sidecarBefore != nullptr, "the shown frame holds its full Rendered tables");
        e.expect(service.scopeTapGeometry(tapKey).has_value(), "the Viewer's tap of the frame is held");
        const auto before = service.status().counters;
        const auto startedBefore = scopes.measuresStarted();
        const auto answeredBefore = taps.answeredByMeasure() + taps.answeredByDrop();
        const auto flaggedBefore = taps.flaggedSubmissions();

        // The fill renders the frame the Viewer shows (a flagged submission, the same key).
        auto fillIdentity = shown->desiredIdentity();
        fillIdentity.requestGeneration += 100000;
        runtime::TaskRequest request(
            "Cache RAM preview frame",
            {.kind = runtime::TaskOwnerKind::Composition,
             .id = runtime::TaskOwnerId::fromRaw(session.compositionId().value())},
            runtime::TaskPriority::Foreground);
        request.sourceVersion = {.documentRevision = fillIdentity.sourceRevision.value(),
                                 .requestGeneration = fillIdentity.requestGeneration};
        request.usesPurgeableCaches = true;
        auto submission = taps.submit(std::move(request),
                                      session.evaluationSnapshotForTime(fillIdentity.time),
                                      fillIdentity, kBudget, {});
        e.expect(submission.accepted(), "the fill's flagged submission of the shown frame is admitted");
        std::optional<runtime::TaskResult<runtime::PreviewPreparationResultHandle>> landed;
        if (submission.accepted()) {
            e.expect(awaitQtCondition("the fill's frame lands",
                                      [&] {
                                          landed = submission.handle.tryTakeResult();
                                          return landed.has_value();
                                      }),
                     "the fill's frame lands");
        }
        ui::PreparedPreviewFrameHandle delivered;
        if (landed.has_value() && landed->state() == runtime::TaskState::Succeeded &&
            landed->value().has_value() && *landed->value() != nullptr &&
            (*landed->value())->status() == runtime::PreviewPreparationStatus::Prepared)
            delivered = (*landed->value())->frame();
        e.expect(delivered != nullptr, "the fill's frame of the shown time is prepared");
        const auto both = service.scopeTapGeometry(tapKey);
        e.expect(service.status().counters.scopeFillTapsKept == before.scopeFillTapsKept + 1 &&
                     both && !both->fill,
                 "both slots hold the frame: the fill's tap is kept beside the Viewer's");
        taps.received(fillIdentity, delivered);
        e.expect(awaitQtCondition("the fill's request is answered",
                                  [&] {
                                      return taps.answeredByMeasure() + taps.answeredByDrop() ==
                                                 answeredBefore + 1 &&
                                             taps.pendingSubmissions() == 0 &&
                                             taps.waitingTaps() == 0 && !scopes.busy();
                                  }),
                 "the fill's request for the shown frame is answered");
        const auto after = service.status().counters;
        e.expect(taps.flaggedSubmissions() == flaggedBefore + 1 && taps.answeredByDrop() >= 1,
                 "the fill's request was answered by letting its tap go");
        e.expect(after.scopeMeasuresRequested == before.scopeMeasuresRequested &&
                     after.scopeMeasuresCompleted == before.scopeMeasuresCompleted &&
                     after.scopeTableReadbacks == before.scopeTableReadbacks &&
                     scopes.measuresStarted() == startedBefore,
                 "nothing was measured twice: no measure and no table readback for the fill");
        const auto sidecarAfter = shownKey ? frameCache->scopesFor(*shownKey, fullIdentity) : nullptr;
        e.expect(sidecarAfter != nullptr && sidecarAfter == sidecarBefore &&
                     scopes.reading().scopes == sidecarBefore,
                 "one Rendered sidecar is kept, and the shown-frame request still has its tables");
        // The fill slot is free (the tap was released) and the Viewer's tap was not lost to it.
        const auto stillTapped = service.scopeTapGeometry(tapKey);
        e.expect(stillTapped && !stillTapped->fill, "the release of the fill tap left the Viewer's tap");
        const auto stallsBefore = service.status().counters.scopeFillStageWaits;
        auto nextIdentity = fillIdentity;
        nextIdentity.requestGeneration += 1;
        nextIdentity.time = time(4, 25);
        runtime::TaskRequest nextRequest(
            "Cache RAM preview frame",
            {.kind = runtime::TaskOwnerKind::Composition,
             .id = runtime::TaskOwnerId::fromRaw(session.compositionId().value())},
            runtime::TaskPriority::Foreground);
        nextRequest.sourceVersion = {.documentRevision = nextIdentity.sourceRevision.value(),
                                     .requestGeneration = nextIdentity.requestGeneration};
        nextRequest.usesPurgeableCaches = true;
        auto next = taps.submit(std::move(nextRequest),
                                session.evaluationSnapshotForTime(nextIdentity.time), nextIdentity,
                                kBudget, {});
        std::optional<runtime::TaskResult<runtime::PreviewPreparationResultHandle>> nextLanded;
        if (next.accepted())
            e.expect(awaitQtCondition("the next flagged fill frame lands",
                                      [&] {
                                          nextLanded = next.handle.tryTakeResult();
                                          return nextLanded.has_value();
                                      }),
                     "the next flagged fill frame lands");
        e.expect(service.status().counters.scopeFillStageWaits == stallsBefore,
                 "the fill slot was freed: the next flagged fill frame was not held back");
        if (nextLanded.has_value() && nextLanded->value().has_value() &&
            *nextLanded->value() != nullptr)
            taps.received(nextIdentity, (*nextLanded->value())->frame());
        e.expect(awaitQtCondition("every tap is answered",
                                  [&] {
                                      return taps.pendingSubmissions() == 0 && taps.waitingTaps() == 0 &&
                                             !scopes.busy();
                                  }),
                 "every tap is answered");
    }

    const auto fillBase = taps.flaggedSubmissions();
    const auto answeredBase = taps.answeredByMeasure() + taps.answeredByDrop();
    const auto measuredBase = taps.answeredByMeasure();
    const auto startedBase = scopes.measuresStarted();

    // ---- A fill with the panel closed is unchanged and pays nothing ---------------------------------
    scopes.setSettings([&] {
        auto none = settings;
        none.scopes = 0;
        return none;
    }());
    e.expect(!taps.wanted(), "a closed panel does not want the fill's taps");
    {
        commands::Transaction range("Work area", session.snapshot().revision());
        range.emplace<commands::SetWorkArea>(session.compositionId(), time(0, 25), time(3, 25));
        e.expect(session.executeTransaction(std::move(range)).changed(), "work area accepted");
        const auto before = service.status().counters;
        const auto scenesBefore = sceneStages->load();
        ram.start();
        e.expect(awaitQtCondition("the closed fill ends",
                                  [&] { return !ram.isCaching() && ram.cachedFrameCount() == 3; }),
                 "the closed fill ends with its three frames");
        const auto after = service.status().counters;
        e.expect(taps.flaggedSubmissions() == fillBase && taps.waitingTaps() == 0 &&
                     after.scopeFillTapsKept == before.scopeFillTapsKept &&
                     after.scopeFillStageWaits == before.scopeFillStageWaits &&
                     after.scopeMeasuresRequested == before.scopeMeasuresRequested &&
                     after.scopeMeasuresStarted == before.scopeMeasuresStarted &&
                     after.scopeTableReadbacks == before.scopeTableReadbacks &&
                     scopes.measuresStarted() == startedBase,
                 "a closed fill flags nothing, keeps no picture, and measures nothing");
        std::cerr << "closed fill stages " << sceneStages->load() - scenesBefore << " graph "
                  << after.residentGraphJobs - before.residentGraphJobs << '\n';
        // The foreground frame (time 0) was already cached by the Viewer, so the fill renders two.
        e.expect(sceneStages->load() - scenesBefore == 2 &&
                     after.residentGraphJobs - before.residentGraphJobs == 2,
                 "the closed fill ran its own scenes only");
    }

    // ---- A small fill with the panel open ----------------------------------------------------------------
    {
        commands::Transaction range("Work area", session.snapshot().revision());
        range.emplace<commands::SetWorkArea>(session.compositionId(), time(3, 25), time(6, 25));
        e.expect(session.executeTransaction(std::move(range)).changed(), "second work area accepted");
        scopes.setSettings(settings);
        e.expect(taps.wanted(), "a panel with a scope open wants the fill's taps");
        // Opening the scopes again re-measures the shown frame: live, then full.
        e.expect(awaitQtCondition("the shown frame is measured again", settled),
                 "the shown frame is measured again");
        const auto startedBefore = scopes.measuresStarted();
        const auto before = service.status().counters;
        const auto scenesBefore = sceneStages->load();
        ram.start();
        e.expect(awaitQtCondition("the fill with the panel open ends",
                                  [&] { return !ram.isCaching() && ram.cachedFrameCount() == 3; }),
                 "the fill with the panel open ends with its three frames (it never stalled)");
        e.expect(awaitQtCondition("every flagged frame is answered",
                                  [&] {
                                      return taps.flaggedSubmissions() == fillBase + 3 &&
                                             taps.answeredByMeasure() + taps.answeredByDrop() ==
                                                 answeredBase + 3 &&
                                             taps.pendingSubmissions() == 0 &&
                                             taps.waitingTaps() == 0 && !scopes.busy();
                                  }),
                 "every flagged frame is answered");
        const auto after = service.status().counters;
        e.expect(taps.answeredByMeasure() == measuredBase + 3 && after.scopeFillTapsKept - before.scopeFillTapsKept == 3,
                 "all three fill taps were kept and measured on the device");
        std::cerr << "started " << scopes.measuresStarted() - startedBefore << " open fill stages " << sceneStages->load() - scenesBefore << " measures "
                  << after.scopeMeasuresCompleted - before.scopeMeasuresCompleted << " graph "
                  << after.residentGraphJobs - before.residentGraphJobs << '\n';
        e.expect(sceneStages->load() - scenesBefore == 3 &&
                     after.residentGraphJobs - before.residentGraphJobs == 3 &&
                     after.scopeMeasuresCompleted - before.scopeMeasuresCompleted == 3 &&
                     scopes.measuresStarted() - startedBefore == 3,
                 "the scopes added no scene run: the three scenes are the fill's own");
        for (std::int64_t frame = 3; frame < 6; ++frame) {
            const auto key = controller.cacheKeyForTime(time(frame, 25));
            const auto sidecar =
                key ? frameCache->scopesFor(*key, ui::PreviewScopeIdentity{
                                                      .settings = fullSettings,
                                                      .viewAdjust = {},
                                                      .source = runtime::ScopeSource::Rendered})
                    : nullptr;
            const auto reference = referenceOf(time(frame, 25), fullSettings);
            e.expect(sidecar != nullptr && sidecar->tables.route() == runtime::ScopeRoute::Gpu &&
                         reference.has_value() && tablesEqual(sidecar, *reference),
                     "a retained frame carries full-detail Working tables, Rendered on the "
                     "device, equal to the CPU reference");
        }
    }

    // Teardown: every owner answered, then the service.
    ram.beginShutdown();
    scopes.setSettings([&] {
        auto none = settings;
        none.scopes = 0;
        return none;
    }());
    service.beginShutdown();
    taps.shutdown();
    controller.beginShutdown();
    bridge.beginShutdown();
    e.expect(awaitQtCondition("the scheduler drains", [&] { return scheduler.isQuiescent(); }),
             "the scheduler drains");
    e.expect(taps.answeredByMeasure() + taps.answeredByDrop() == taps.flaggedSubmissions(),
             "every flagged frame was answered exactly once");
    if (e.failures() + awaitFailureCount() != 0)
        return 1;
    std::cout << "PASS: scopes device route\n";
    return 0;
}

#endif // BLOOM_GPU_TOOLS_AVAILABLE

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication application(argc, argv);
    const auto options = parseOptions(argc, argv);
    if (!options.valid)
        return 2;
#if !defined(BLOOM_GPU_TOOLS_AVAILABLE) || (BLOOM_GPU_TOOLS_AVAILABLE == 0)
    std::cout << "SKIP: the packaged GPU shader tools are unavailable\n";
    return options.requireDevice ? 1 : 77;
#else
    try {
        return run(options);
    } catch (const std::exception& error) {
        std::cerr << "Unexpected test exception: " << error.what() << '\n';
        return 1;
    }
#endif
}
