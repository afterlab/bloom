// Real production scene -> native display -> registry refusal/recovery, without a desktop window.
// The same checks prove two routes (#750). By default the NativeResident route, whose frames are
// leased through the presentation registry; that presentation is Wayland's, so the case runs on
// Linux. With --packed, the GpuPreparedPacked product with presentation disabled, which the app
// runs on every platform: the same GPU scene, executor and processing phases, then one final
// transfer.
#include "fixed_memory_budget.hpp"
#include "gpu_service_chain_fixture.hpp"

#include <bloom/commands/command_stack.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/ui/composition_preview_pipeline.hpp>
#include <bloom/ui/composition_session.hpp>
#include <bloom/ui/task_ui_bridge.hpp>

#include <QGuiApplication>
#include <QTemporaryDir>

#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace bloom;
using namespace bloom::ui::verticalproof;
using namespace std::chrono_literals;

namespace {
// --packed: the GpuPreparedPacked product instead of the NativeResident route.
bool gPackedRoute = false;
// The packed product's host pixels are retired by the composition root's worker, as in the app
// (apps/bloom/main.cpp); the resident route allocates none and runs without it.
std::shared_ptr<render::PackedHostRetirementOwner> gPackedRetirement;

// The processing-phase root every service here shares, with the route's host retirement.
[[nodiscard]] std::shared_ptr<runtime::GpuProcessingPhasePolicy>
makePhasePolicy(const std::size_t requestCapacity) {
    return std::make_shared<runtime::GpuProcessingPhasePolicy>(requestCapacity, gPackedRetirement);
}

// Stops and collects the retirement worker after every service and phase is gone.
class PackedRetirementTeardown final {
  public:
    PackedRetirementTeardown() = default;
    PackedRetirementTeardown(const PackedRetirementTeardown&) = delete;
    PackedRetirementTeardown& operator=(const PackedRetirementTeardown&) = delete;
    ~PackedRetirementTeardown() {
        if (!gPackedRetirement)
            return;
        gPackedRetirement->beginShutdown();
        (void)bloom::test_support::awaitCondition(
            "the retirement worker acknowledges its exit",
            [] { return gPackedRetirement->exitAcknowledged(); });
        (void)bloom::test_support::awaitCondition("the retirement worker is collected",
                                                  [] { return gPackedRetirement->collect(); });
    }
};

// Every service here plans for a stated card: the driver's live budget moves with every other
// program on the device, and admission would move with it. The host reading is injected below.
[[nodiscard]] runtime::GpuPreviewDisplayServiceOptions
phaseServiceOptions(const std::filesystem::path& loader) {
    auto options = serviceOptions(loader);
    options.deviceMemoryLimitBytes = test_support::kFixedDeviceMemoryBytes;
    if (gPackedRoute) {
        options.product = runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
        options.presentation = GpuPreviewDisplayServicePresentationMode::Disabled;
    }
    return options;
}

// A frame the route's GPU scene and display produced: a resident frame, or a GPU-prepared packed
// product with its one final transfer.
[[nodiscard]] bool isGpuFrame(const std::shared_ptr<const runtime::PreparedPreviewFrame>& frame) {
    if (!gPackedRoute)
        return isResident(frame);
    return frame != nullptr &&
           frame->provenance().provider == runtime::PreviewDisplayProvider::GpuPreparedPacked;
}

// A frame the CPU fallback produced: CPU pixels from neither GPU route.
[[nodiscard]] bool isCpuFrame(const std::shared_ptr<const runtime::PreparedPreviewFrame>& frame) {
    return hasCpuPixels(frame) && !isResident(frame) &&
           frame->provenance().provider != runtime::PreviewDisplayProvider::GpuPreparedPacked;
}
} // namespace

#include "gpu_processing_phase_retry_native.ipp"

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape)
    std::vector<char*> arguments;
    for (int index = 0; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--packed")
            gPackedRoute = true;
        else
            arguments.push_back(argv[index]);
    }
    int count = static_cast<int>(arguments.size());
    const auto parsed = parseOptions(count, arguments.data());
    if (!parsed.valid)
        return 2;
    if (gPackedRoute)
        gPackedRetirement = render::PackedHostRetirementOwner::create();
    // Declared first, so it runs after every service, phase and frame below is destroyed.
    const PackedRetirementTeardown retirementTeardown;
    // No window: it runs on any Qt platform, offscreen included (#726).
    QGuiApplication application(count, arguments.data());
    QTemporaryDir temporary;
    Expectations checks;
    Fixture fixture(temporary.path().toStdString());
    auto project = document::makeNewProject(
        "Phase recovery", "Main", core::RationalTime::fromInteger(4), format1920x1080NonSquare());
    document::Document document(std::move(project.project));
    commands::CommandStack commands(document);
    ui::CompositionSession session(document, commands, project.initialCompositionId);
    checks.expect(session.addSolidLayer("Solid", {0.125, 0.25, 0.5, 1.0}),
                  "ordinary 1080p solid is authored");
    checks.expect(session.addTextLayer(QStringLiteral("Text"), QStringLiteral("Phase recovery")),
                  "ordinary 1080p text is authored");
    const auto policy = makePhasePolicy(1);
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    runtime::MemoryBudgetState sample;
    sample.machine.availableBytes = 60 * gib;
    sample.process.status = runtime::ProcessMemoryStatus::Finite;
    sample.process.source = runtime::ProcessMemorySource::LinuxCgroupV2;
    sample.process.sampledAt = runtime::GpuProcessingPhasePolicy::Clock::now();
    sample.process.domains.push_back({.path = "/fixture",
                                      .kind = runtime::ProcessMemoryLimitKind::Finite,
                                      .limitBytes = 8 * gib,
                                      .currentBytes = 0});
    sample.processSampleFresh = true;
    sample.sampleSerial = 1;
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    const runtime::GpuScenePreparationPhase phase{
        .policy = policy,
        .envelope = {.metadataBytes = 1024ULL * 1024,
                     .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                     .requestedExecutorBytes = kBudget,
                     .deviceWorkingBytes = 832ULL * 1024 * 1024}};
    fixture.sceneBuilder = std::make_shared<runtime::CpuGpuSceneBuilder>(
        nullptr, runtime::GpuSceneMediaContext::fromEvaluator(fixture.evaluator),
        runtime::GpuSceneOcioContext{}, phase);
    TaskScheduler scheduler(schedulerConfig());
    const runtime::CpuReferenceDisplayPreparer referenceDisplay;
    // Early disabled/unavailable CPU roots must reserve before their heavy callback, independently
    // of native startup. The same root/count is shared with the native producer below.
    auto disabledOptions = phaseServiceOptions(parsed.loader_path);
    disabledOptions.enabled = false;
    disabledOptions.processingPhasePolicy = policy;
    std::atomic_size_t cpuPreparations{0};
    const auto actualCpuStage = fixture.productionCpuStage();
    GpuPreviewDisplayService disabledService(
        scheduler,
        [&cpuPreparations,
         actualCpuStage](const document::Snapshot& snapshot, const PreviewRequestIdentity& identity,
                         const std::size_t limit,
                         const std::vector<runtime::SnapshotParameterOverride>& overrides,
                         TaskContext& context) {
            cpuPreparations.fetch_add(1, std::memory_order_relaxed);
            return actualCpuStage(snapshot, identity, limit, overrides, context);
        },
        fixture.productionCpuFallback(referenceDisplay), disabledOptions);
    const auto cpuSnapshot = session.snapshot();
    const auto cpuIdentity = makeIdentity(100, cpuSnapshot, session.compositionId());
    const TaskOwner cpuOwner{.kind = TaskOwnerKind::Composition, .id = TaskOwnerId::fromRaw(8)};
    auto heldHost = runtime::reserveHostEvaluationPhase(
        *policy, 1920ULL * 1080, kBudget, runtime::GpuProcessingPhaseOutput::FloatFinal, true);
    checks.expect(heldHost.lease != nullptr, "CPU fixture holds one shared competing phase");
    auto deniedCpu = disabledService.submit(TaskRequest("CPU phase refusal", cpuOwner), cpuSnapshot,
                                            cpuIdentity, kBudget, {});
    auto deniedCpuResult = deniedCpu.accepted() ? awaitResult(deniedCpu.handle) : std::nullopt;
    checks.expect(deniedCpuResult && deniedCpuResult->state() == TaskState::Failed &&
                      cpuPreparations.load(std::memory_order_relaxed) == 0,
                  "disabled CPU root refuses shared contention before evaluation callback");
    heldHost.lease.reset();
    auto acceptedCpu = disabledService.submit(TaskRequest("CPU phase recovery", cpuOwner),
                                              cpuSnapshot, cpuIdentity, kBudget, {});
    auto acceptedCpuResult =
        acceptedCpu.accepted() ? awaitResult(acceptedCpu.handle) : std::nullopt;
    const auto cpuFrame = acceptedCpuResult ? residentFrameOf(*acceptedCpuResult) : nullptr;
    checks.expect(
        isCpuFrame(cpuFrame) && cpuPreparations.load(std::memory_order_relaxed) == 1 &&
            policy->snapshot().reservations.admittedPhases == 0,
        "capacity release admits genuine 1080p solid/text CPU fallback and releases phase");
    acceptedCpuResult.reset();
    disabledService.beginShutdown();
    { // Release this completed owner slot before constructing the resident service below.
        auto legacyOptions = phaseServiceOptions(parsed.loader_path);
        legacyOptions.presentation = GpuPreviewDisplayServicePresentationMode::Disabled;
        legacyOptions.processingPhasePolicy = policy;
        std::atomic_size_t legacyPreparations{0};
        GpuPreviewDisplayService legacyService(
            scheduler,
            [&legacyPreparations,
             actualCpuStage](const document::Snapshot& snapshot,
                             const PreviewRequestIdentity& identity, const std::size_t limit,
                             const std::vector<runtime::SnapshotParameterOverride>& overrides,
                             TaskContext& context) {
                legacyPreparations.fetch_add(1, std::memory_order_relaxed);
                return actualCpuStage(snapshot, identity, limit, overrides, context);
            },
            fixture.productionCpuFallback(referenceDisplay), legacyOptions);
        checks.expect(
            awaitProof(
                "legacy native owner reports capacity independently before CPU preparation",
                [&] {
                    return legacyService.status().state !=
                           GpuPreviewDisplayServiceState::Initializing;
                },
                90s) &&
                legacyService.status().state == GpuPreviewDisplayServiceState::Ready,
            "legacy native owner reports capacity independently before CPU preparation");
        sample.process.sampledAt = runtime::GpuProcessingPhasePolicy::Clock::now();
        sample.sampleSerial += 1;
        policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
        auto legacyHeld = runtime::reserveHostEvaluationPhase(
            *policy, 1920ULL * 1080, kBudget, runtime::GpuProcessingPhaseOutput::FloatFinal, true);
        checks.expect(legacyHeld.lease != nullptr, "legacy fixture holds competing phase");
        auto legacyDenied = legacyService.submit(TaskRequest("legacy paired refusal", cpuOwner),
                                                 cpuSnapshot, cpuIdentity, kBudget, {});
        auto legacyDeniedResult =
            legacyDenied.accepted() ? awaitResult(legacyDenied.handle) : std::nullopt;
        checks.expect(legacyDeniedResult && legacyDeniedResult->state() == TaskState::Failed &&
                          legacyPreparations.load(std::memory_order_relaxed) == 0,
                      "legacy paired contention refuses before CPU callback");
        legacyHeld.lease.reset();
        auto legacyAccepted = legacyService.submit(TaskRequest("legacy paired recovery", cpuOwner),
                                                   cpuSnapshot, cpuIdentity, kBudget, {});
        auto legacyResult =
            legacyAccepted.accepted() ? awaitResult(legacyAccepted.handle) : std::nullopt;
        const auto legacyFrame = legacyResult ? residentFrameOf(*legacyResult) : nullptr;
        checks.expect(legacyFrame && hasCpuPixels(legacyFrame) &&
                          legacyFrame->provenance().provider ==
                              runtime::PreviewDisplayProvider::GpuNeutral &&
                          legacyPreparations.load(std::memory_order_relaxed) == 1,
                      "released phase admits real1080 CPU-prepared pixels with genuine Neutral "
                      "native display");
        checks.expect(
            awaitProof("legacy fence-proven phase releases after stage storage disposal",
                       [&] { return policy->snapshot().reservations.admittedPhases == 0; }) &&
                policy->snapshot().reservations.unprovenPhases == 0,
            "legacy fence-proven phase releases after stage storage disposal");
        const auto legacyCore =
            runtime::detail::GpuPreviewDisplayServiceTestAccess::coreOf(legacyService);
        for (const int phaseFault : {1, 3}) {
            legacyCore->testNativeExceptionPhase.store(phaseFault, std::memory_order_release);
            auto faultSubmission = legacyService.submit(TaskRequest("legacy exception", cpuOwner),
                                                        cpuSnapshot, cpuIdentity, kBudget, {});
            auto faultResult =
                faultSubmission.accepted() ? awaitResult(faultSubmission.handle) : std::nullopt;
            checks.expect(
                faultResult && faultResult->state() == TaskState::Failed &&
                    awaitProof(
                        "pre-begin/post-Ready exceptions terminally fail without stranded phase",
                        [&] { return policy->snapshot().reservations.admittedPhases == 0; }),
                "pre-begin/post-Ready exceptions terminally fail without stranded phase");
            auto healthySubmission =
                legacyService.submit(TaskRequest("legacy exception recovery", cpuOwner),
                                     cpuSnapshot, cpuIdentity, kBudget, {});
            auto healthyResult =
                healthySubmission.accepted() ? awaitResult(healthySubmission.handle) : std::nullopt;
            const auto healthyFrame = healthyResult ? residentFrameOf(*healthyResult) : nullptr;
            checks.expect(healthyFrame && healthyFrame->provenance().provider ==
                                              runtime::PreviewDisplayProvider::GpuNeutral,
                          "native generation remains usable after proven exception cleanup");
        }
        legacyCore->testNativeExceptionPhase.store(2, std::memory_order_release);
        auto unknownSubmission =
            legacyService.submit(TaskRequest("legacy submitted exception", cpuOwner), cpuSnapshot,
                                 cpuIdentity, kBudget, {});
        auto unknownResult =
            unknownSubmission.accepted() ? awaitResult(unknownSubmission.handle) : std::nullopt;
        checks.expect(
            unknownResult && unknownResult->state() == TaskState::Failed &&
                policy->snapshot().reservations.admittedPhases == 1 &&
                policy->snapshot().reservations.unprovenPhases == 1,
            "post-submit exception terminates stage but retains exact unknown generation quota");
        legacyService.beginShutdown();
        checks.expect(awaitProof("legacy native service retires before resident fixture",
                                 [&] {
                                     return legacyService.status().state ==
                                            GpuPreviewDisplayServiceState::Stopped;
                                 }),
                      "legacy native service retires before resident fixture");
    }
    checks.expect(policy->snapshot().reservations.admittedPhases == 0,
                  "destroyed legacy allocator generation releases conservative unknown quota");
    retryNativeChecks(checks, fixture, session, scheduler, parsed.loader_path, referenceDisplay);
    nativeImageCacheRetryChecks(checks, fixture, scheduler, parsed.loader_path, referenceDisplay);
    if (checks.failures() != 0)
        return 1;
    auto options = phaseServiceOptions(parsed.loader_path);
    options.processingPhasePolicy = policy;
    // The configured lease ceiling holds exactly one Full packed-size native image. It bounds the
    // resident frame cache only: the lease registry takes the whole device pool (#425,
    // gpu-backend.md "One device ceiling"), so a second live frame beside the first is still
    // published while the device has room, and never becomes a CPU frame (#539).
    options.residentLeaseBudgets.maxBytes = 12ULL * 1024 * 1024;
    GpuPreviewDisplayService service(scheduler, fixture.productionGpuStage(),
                                     fixture.productionCpuStage(),
                                     fixture.productionCpuFallback(referenceDisplay), options);
    checks.expect(
        awaitProof(
            "cold owner reports capacity and reaches genuine native readiness",
            [&] { return service.status().state != GpuPreviewDisplayServiceState::Initializing; },
            90s) &&
            service.status().state == GpuPreviewDisplayServiceState::Ready,
        "cold owner reports capacity and reaches genuine native readiness");
    if (service.status().state != GpuPreviewDisplayServiceState::Ready) {
        std::cerr << service.status().residentDetail << '\n';
        return parsed.require_device ? 1 : 77;
    }
    sample.process.sampledAt = runtime::GpuProcessingPhasePolicy::Clock::now();
    sample.sampleSerial += 1;
    policy->applyHostSample(sample, 60 * gib, sample.process.sampledAt);
    const auto snapshot = session.snapshot();
    TaskOwner owner;
    owner.kind = TaskOwnerKind::Composition;
    owner.id = TaskOwnerId::fromRaw(7);
    auto submit = [&](const std::uint64_t generation) {
        const auto identity = makeIdentity(generation, snapshot, session.compositionId());
        auto admitted = service.submit(TaskRequest("phase native recovery", owner), snapshot,
                                       identity, kBudget, {});
        checks.expect(admitted.accepted(), "the real request is admitted");
        if (!admitted.accepted())
            return std::optional<TaskResult<PreviewPreparationResultHandle>>{};
        return awaitResult(admitted.handle);
    };
    auto firstResult = submit(1);
    auto firstFrame = firstResult ? residentFrameOf(*firstResult) : nullptr;
    checks.expect(isGpuFrame(firstFrame), "first solid/text frame is a genuine GPU frame");
    firstResult.reset();
    const auto beforeSecond = service.status().counters;
    auto secondResult = submit(2);
    const auto second = secondResult ? residentFrameOf(*secondResult) : nullptr;
    const auto leaseBytes = [](const std::shared_ptr<const runtime::PreparedPreviewFrame>& frame) {
        return isResident(frame) && frame->residentFrame()
                   ? frame->residentFrame()->lease().allocationBytes()
                   : std::uint64_t{0};
    };
    checks.expect(isGpuFrame(firstFrame) && isGpuFrame(second) && second != firstFrame,
                  "a second live frame beside the first is a genuine GPU frame");
    // Resident only: the packed product holds no resident lease once its transfer is done.
    if (!gPackedRoute)
        checks.expect(
            !hasCpuPixels(second) &&
                &second->residentFrame()->lease() != &firstFrame->residentFrame()->lease() &&
                leaseBytes(firstFrame) + leaseBytes(second) > options.residentLeaseBudgets.maxBytes,
            "the two live frames, together over the lease ceiling, are both published resident");
    checks.expect(service.status().counters.residentGraphJobs ==
                          beforeSecond.residentGraphJobs + 1 &&
                      service.status().counters.cpuFallbacks == beforeSecond.cpuFallbacks,
                  "the second frame is an actual native scene and display, never a CPU frame");
    checks.expect(policy->snapshot().reservations.unprovenPhases == 0 &&
                      policy->snapshot().reservations.admittedPhases == 0,
                  "positive native retirement releases the phase of each published frame");
    firstFrame.reset();
    secondResult.reset();
    auto recoveredResult = submit(3);
    const auto recovered = recoveredResult ? residentFrameOf(*recoveredResult) : nullptr;
    checks.expect(isGpuFrame(recovered) && recovered != second,
                  "the next frame after the first is released is a genuine GPU frame");
    checks.expect(policy->snapshot().reservations.admittedPhases == 0,
                  "successful publication releases both phase quotas before terminal delivery");
    service.beginShutdown();
    checks.expect(awaitProof("the native phase service retires without an unproven phase",
                             [&] {
                                 return service.status().state ==
                                        GpuPreviewDisplayServiceState::Stopped;
                             }),
                  "the native phase service retires without an unproven phase");
    if (checks.failures() != 0)
        return 1;
    std::cout << "PASS: 1080p solid/text native phase publication beside a live frame ("
              << (gPackedRoute ? "GPU-prepared packed" : "native resident") << ")\n";
    return 0;
}
