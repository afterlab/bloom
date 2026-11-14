// #724 Scopes: the scope tap and the device scope measure of the preview display service.
//
// The real GpuPreviewDisplayService runs with the GPU-prepared packed product, presentation
// Disabled and a declared card (no window system). A Viewer-class frame keeps the scene-linear
// picture its own frame job computed (the tap); a measure counts that picture on the service's
// device with the display and XYZ color steps and the count kernel, reads back only the tables
// and never runs the scene again.
//
// What is asserted:
//   (1) parity: the tap measure of the Working signal equals the CPU reference cell for cell; the
//       Display signal and the CIE table are within one cell along every axis, every pixel counted;
//   (2) no scene run: across a measure the scene counters and the preparation count do not move,
//       the device runs only the color programs and the count kernel, no picture is read back, and
//       one table readback moves the layout's bytes at the detail (0 and 1);
//   (3) hold: with the table readback held through the seam, further packed requests each complete
//       with a GpuPreparedPacked frame while the measure stays in flight; releasing the hold
//       delivers the tables of the frame that was tapped;
//   (4) a second request in flight is Busy; a tap replaced by a newer Viewer frame before its
//       measure began gives Skipped(Superseded); a newer tap of the same key is measured;
//   (5) cancelling mid-measure delivers Cancelled only after the submission's fence is proven
//       retired, and the service keeps working;
//   (6) a fill frame is tapped only when its request asks; a second flagged fill frame does not
//       start its scene work until the first one's tap is measured or dropped; an unflagged fill
//       frame keeps nothing and is never held back; a fill tap never replaces the Viewer's tap and
//       the reverse;
//   (7) shutting the service down with a request queued, or in flight, ends it and unblocks the
//       caller;
//   (8) memory (the pressureCases below): the measure is admitted as a Background phase and yields
//       to the Viewer on a declared card. A card that fits the Viewer's frame but not the frame
//       plus the measure ends it Skipped(NoRoom) with the Viewer's counters unmoved; a waiting
//       request is admitted when the room returns and ends NoRoom when its tap is given back; the
//       taps are the first memory given back for a frame that needs room, a measure in flight is
//       tracked against its phase and never makes a frame wait, a fill tap that does not fit as
//       Background is refused without holding the fill back, and the idle trim releases the tap.
//
// Requires the Vulkan backend and the pinned shader tools; skips (77) without a device and fails
// under --require-device.

#include "gpu_preview_display_service_packed_support.hpp"
#include "scope_tables_compare_support.hpp"

#include "await.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_scope_tables.hpp>
#include <bloom/render/native_image_storage.hpp>
#include <bloom/render/scope_table_layout.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>
#include <bloom/runtime/scope_frame_measure.hpp>
#include <bloom/runtime/scope_measure.hpp>

#include <atomic>
#include <future>
#include <optional>

namespace {
#if defined(BLOOM_GPU_TOOLS_AVAILABLE) && BLOOM_GPU_TOOLS_AVAILABLE

using bloom::render::kScopeTableAll;
using bloom::render::kScopeTableHistogram;
using bloom::render::kScopeTableVectorscope;
using bloom::render::kScopeTableWaveform;
using bloom::runtime::GpuScopeTapKey;
using bloom::runtime::GpuScopeTapMeasureOutcome;
using bloom::runtime::GpuScopeTapSkip;
using bloom::runtime::GpuScopeTapStatus;
using bloom::runtime::ScopeFrameStatus;
using bloom::runtime::ScopeMeasureSettings;
using bloom::runtime::ScopeSignal;
using bloom::runtime::ScopeSignalColor;
using bloom::runtime::TaskPriority;
using bloom::runtime::scope_test::tablesWithinOneCell;
using bloom::test_support::awaitCondition;
using Access = bloom::runtime::detail::GpuPreviewDisplayServiceTestAccess;

constexpr std::uint32_t kSize = 256;
// The picture of the frame a pressure case sends last: larger than any frame its rig rendered
// before, so no storage a device parked or reuses for the earlier frames fits it, and it must
// allocate new native storage.
constexpr std::uint32_t kLargeSize = 2 * kSize;
// Frames of this request generation or later render the larger plan (the stage picks by it).
constexpr std::uint64_t kLargeGeneration = 1000;
// An idle service trims its retention after a delay of its own (the taps with it); a case that is
// not about the trim keeps the service from trimming while it waits for the books.
constexpr std::uint64_t kNoIdleTrim = 10ULL * 60 * 1000;
constexpr std::uint32_t kNoChromaticity =
    kScopeTableHistogram | kScopeTableWaveform | kScopeTableVectorscope;
// Several requests are in flight together in some cases: they must fit the request-owned
// capacity together.
constexpr std::size_t kRequestBudget = kBudget / 4;

GpuPreviewDisplayServiceOptions packedOptions(const TestOptions& options) {
    auto configured = serviceOptions(options.loaderPath);
    configured.product = bloom::runtime::GpuPreviewDisplayProductMode::GpuPreparedPacked;
    configured.presentation = GpuPreviewDisplayServicePresentationMode::Disabled;
    // A held measure never meets the dispatch deadline.
    configured.nativeDispatchDeadline = std::chrono::minutes(10);
    return configured;
}

// What a rig plans for: the card (0 keeps the suite's declared card) and the working allowance
// the Viewer's frames state in their envelope (0 keeps the suite's default).
struct RigShape final {
    std::uint64_t card = 0;
    std::uint64_t viewerWorkingBytes = 832ULL * 1024 * 1024;
    // The scene cache's retained-byte ceiling (the suite's default when empty).
    std::optional<std::uint64_t> sceneCacheBytes = std::nullopt;
    // How long the idle service waits before it trims its retention (the default when 0).
    std::uint64_t idleTrimMilliseconds = 0;
    // The plan of the frames from `kLargeGeneration` on (the same composition at a larger size).
    std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> largePlan = nullptr;
};

// The declared card every pressure case runs on, found by `pressureCases` (0 until then: a case's
// card is then declared as it is). A device needs a card of a size of its own before its service
// qualifies the resident route and before its allocator's blocks leave room for a frame (a modelled
// card smaller than that never runs a native frame on it, whatever the policy's books say), so each
// case declares this roomy card and then shapes the policy's usable capacity to the numbers it
// plans for (`deviceReported`).
std::uint64_t modelledCardBytes = 0;

// The service, its scheduler and the counting scene preparation, torn down in reverse order.
struct Rig final {
    PackedRetirementScope retirement;
    TaskScheduler scheduler;
    std::shared_ptr<bloom::runtime::GpuProcessingPhasePolicy> policy;
    bloom::runtime::MemoryBudgetLedger ledger;
    std::shared_ptr<std::atomic<std::uint64_t>> preparations =
        std::make_shared<std::atomic<std::uint64_t>>(0);
    std::shared_ptr<std::atomic<std::uint64_t>> viewerWorking =
        std::make_shared<std::atomic<std::uint64_t>>(832ULL * 1024 * 1024);
    std::unique_ptr<GpuPreviewDisplayService> service;
    std::shared_ptr<bloom::runtime::detail::PreviewDisplayServiceCore> core;
    std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> plan;
    bloom::document::Snapshot snapshot;
    std::atomic<std::uint64_t> nextOwner{100};
    // The usable device capacity the rig's case plans for (0: the card as declared). See
    // `modelledCardBytes` and `deviceReported`.
    std::uint64_t usableTarget = 0;

    static bloom::runtime::MemoryBudgetLedger makeLedger() {
        auto sample = bloom::test_support::fixedMemoryBudgetSample();
        return bloom::runtime::MemoryBudgetLedger(sample.physicalBytes,
                                                  sample.machine.availableBytes.value_or(0));
    }

    static TaskSchedulerConfig config() {
        auto value = schedulerConfig();
        value.cpuWorkerCount = 4;
        return value;
    }

    Rig(const TestOptions& options,
        std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> planValue,
        std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle> processor,
        bloom::document::Snapshot snapshotValue, const RigShape shape = {})
        : scheduler(config()),
          policy(std::make_shared<bloom::runtime::GpuProcessingPhasePolicy>(32, retirement.owner)),
          ledger(makeLedger()), plan(std::move(planValue)), snapshot(std::move(snapshotValue)) {
        ledger.beginProcessSampling();
        refresh();
        viewerWorking->store(shape.viewerWorkingBytes);
        // The Viewer's frames state their working allowance in their envelope; a test that needs a
        // frame with a need of its own sets `viewerWorking` before submitting it (an envelope with
        // no measured peaks yet is admitted at exactly its stated need).
        auto count = preparations;
        auto working = viewerWorking;
        auto planCopy = plan;
        auto largeCopy = shape.largePlan;
        auto policyCopy = policy;
        PreviewGpuSceneStageFunction counted =
            [count, working, planCopy, largeCopy, policyCopy, processor](
                const bloom::document::Snapshot& snap, const PreviewRequestIdentity& identity,
                const std::size_t bytes,
                const std::vector<bloom::runtime::SnapshotParameterOverride>& overrides,
                TaskContext& context) {
                count->fetch_add(1);
                const bloom::runtime::GpuScenePreparationPhase phase{
                    .policy = policyCopy,
                    .envelope = {.metadataBytes = 1024ULL * 1024,
                                 .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                                 .requestedExecutorBytes = kBudget,
                                 .deviceWorkingBytes = working->load()}};
                auto builder = std::make_shared<CpuGpuSceneBuilder>(
                    nullptr, bloom::runtime::GpuSceneMediaContext{},
                    bloom::runtime::GpuSceneOcioContext{}, phase);
                const auto& framePlan =
                    largeCopy != nullptr && identity.requestGeneration >= kLargeGeneration
                        ? largeCopy
                        : planCopy;
                return packedStageFunction(framePlan, builder, {}, processor)(snap, identity, bytes,
                                                                              overrides, context);
            };
        auto configured = packedOptions(options);
        configured.processingPhasePolicy = policy;
        if (shape.card != 0) {
            // A card smaller than `modelledCardBytes` is modelled on it: see `deviceReported`.
            usableTarget = modelledCardBytes != 0 ? shape.card : 0;
            configured.deviceMemoryLimitBytes =
                modelledCardBytes != 0 ? modelledCardBytes : shape.card;
        }
        if (shape.sceneCacheBytes)
            configured.residentSceneCacheBudgets.maxRetainedBytes = *shape.sceneCacheBytes;
        if (shape.idleTrimMilliseconds != 0)
            configured.idleTrimDelay = std::chrono::milliseconds(shape.idleTrimMilliseconds);
        service = std::make_unique<GpuPreviewDisplayService>(
            scheduler, counted,
            generalCpuStageFunction(plan, processor, std::make_shared<CpuCompositionEvaluator>()),
            generalFallback(processor), configured);
        core = Access::coreOf(*service);
    }

    ~Rig() {
        if (service) {
            service->beginShutdown();
            service.reset();
        }
    }

    // The host memory sample goes stale; each request refreshes it.
    void refresh() {
        auto sample = bloom::test_support::fixedMemoryBudgetSample();
        const auto at = bloom::runtime::GpuProcessingPhasePolicy::Clock::now();
        const auto state = ledger.applySample(std::move(sample), at);
        policy->applyHostSample(state, ledger.physicalByteBudget(), at);
    }

    [[nodiscard]] bool ready() const {
        return awaitCondition("the service reaches a terminal startup state", [&] {
                   return service->status().state != GpuPreviewDisplayServiceState::Initializing;
               }) &&
               service->status().state == GpuPreviewDisplayServiceState::Ready;
    }

    [[nodiscard]] PreviewRequestIdentity identity(const std::uint64_t generation,
                                                  const std::int64_t frame) const {
        auto value = makeIdentity(*plan, generation, {}, {}, {});
        value.time = RationalTime::fromInteger(frame);
        return value;
    }

    [[nodiscard]] bloom::runtime::TaskSubmission<PreviewPreparationResultHandle>
    submit(const PreviewRequestIdentity& frameIdentity, const TaskPriority priority,
           const bool tapForScopes = false, const std::size_t budget = kRequestBudget) {
        refresh();
        const bloom::runtime::TaskOwner owner{
            .kind = bloom::runtime::TaskOwnerKind::Composition,
            .id = bloom::runtime::TaskOwnerId::fromRaw(nextOwner.fetch_add(1))};
        return service->submit(TaskRequest("scope tap frame", owner, priority), snapshot,
                               frameIdentity, budget, {}, tapForScopes);
    }

    // A completed frame of the GPU route, or null.
    [[nodiscard]] std::shared_ptr<const bloom::runtime::PreparedPreviewFrame>
    finish(bloom::runtime::TaskSubmission<PreviewPreparationResultHandle>& submission,
           const std::string& what) {
        if (!submission.accepted()) {
            std::cerr << what << ": the request was not admitted\n";
            return {};
        }
        std::optional<TaskResult<PreviewPreparationResultHandle>> result;
        if (!awaitCondition(what, [&] {
                result = submission.handle.tryTakeResult();
                return result.has_value();
            }))
            return {};
        auto frame = packedResult(result);
        if (!frame) {
            for (const auto& diagnostic : result->diagnostics())
                std::cerr << what << ": " << diagnostic.code << ": " << diagnostic.summary << '\n';
            return {};
        }
        return frame;
    }

    [[nodiscard]] std::shared_ptr<const bloom::runtime::PreparedPreviewFrame>
    frame(const std::uint64_t generation, const std::int64_t index, const TaskPriority priority,
          const bool tapForScopes = false) {
        auto submission = submit(identity(generation, index), priority, tapForScopes);
        return finish(submission, "frame " + std::to_string(generation));
    }
};

[[nodiscard]] bool isPacked(const bloom::runtime::PreparedPreviewFrame& frame) {
    return frame.provenance().provider ==
               bloom::runtime::PreviewDisplayProvider::GpuPreparedPacked &&
           frame.processIdentity().provider == bloom::runtime::EvaluationProvider::GpuResident;
}

[[nodiscard]] GpuScopeTapKey keyOf(const bloom::runtime::PreparedPreviewFrame& frame) {
    return GpuScopeTapKey::of(frame.desiredIdentity());
}

// A measure running on its own thread.
[[nodiscard]] std::future<GpuScopeTapMeasureOutcome>
measureAsync(GpuPreviewDisplayService& service, const GpuScopeTapKey& key,
             const bloom::runtime::GpuScopeMeasureJob& job) {
    return std::async(std::launch::async, [&service, key, &job] {
        return service.measureScopeTap(key, job, bloom::runtime::CancellationToken{});
    });
}

[[nodiscard]] bool notReady(std::future<GpuScopeTapMeasureOutcome>& future) {
    return future.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

[[nodiscard]] std::optional<GpuScopeTapMeasureOutcome>
take(std::future<GpuScopeTapMeasureOutcome>& future, const std::string& what) {
    if (!awaitCondition(what, [&] {
            return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }))
        return std::nullopt;
    return future.get();
}

// Holds the table readback of the next measure through the seam.
struct TableHold final {
    std::shared_ptr<bloom::runtime::detail::PreviewDisplayServiceCore> core;
    std::shared_ptr<std::atomic_bool> held = std::make_shared<std::atomic_bool>(true);
    // Set once the owner polled the held table readback: the measure's submission is in flight
    // and its readback is held (a measure between its steps has nothing in flight).
    std::shared_ptr<std::atomic_bool> polled = std::make_shared<std::atomic_bool>(false);

    explicit TableHold(std::shared_ptr<bloom::runtime::detail::PreviewDisplayServiceCore> target)
        : core(std::move(target)) {
        core->scopeTablesPollOverride = [held = held, polled = polled](
                                            bloom::render::GpuScopeTables& tables) {
            if (!held->load())
                return tables.poll();
            polled->store(true);
            return bloom::render::GpuScopeTablesState::Pending;
        };
    }
    [[nodiscard]] bool awaitHeld(const std::string& what) const {
        return awaitCondition(what, [&] { return polled->load(); });
    }
    void release() {
        held->store(false);
        core->notify();
    }
    ~TableHold() {
        held->store(false);
        core->notify();
    }
};

// Holds the packed transfer of the active frame through the existing seam.
struct TransferHold final {
    std::shared_ptr<bloom::runtime::detail::PreviewDisplayServiceCore> core;
    std::shared_ptr<std::atomic_bool> held = std::make_shared<std::atomic_bool>(true);

    explicit TransferHold(std::shared_ptr<bloom::runtime::detail::PreviewDisplayServiceCore> target)
        : core(std::move(target)) {
        core->packedTransferPollOverride = [held = held](bloom::render::GpuDisplayTransfer& transfer) {
            return held->load() ? bloom::render::GpuDisplayTransferState::Pending
                                : transfer.poll();
        };
    }
    void release() {
        held->store(false);
        core->notify();
    }
    ~TransferHold() {
        held->store(false);
        core->notify();
        core->packedTransferPollOverride = {};
    }
};

struct Scope final {
    std::shared_ptr<const ScopeSignalColor> working;
    std::shared_ptr<const ScopeSignalColor> display;
    std::shared_ptr<const bloom::runtime::GpuSceneOcioContext> ocio;
    std::shared_ptr<const bloom::runtime::ProcessFrame> cpuFrame;

    [[nodiscard]] ScopeMeasureSettings settings(const ScopeSignal signal,
                                                const std::uint32_t scopes,
                                                const std::uint32_t detail = 0) const {
        ScopeMeasureSettings value;
        value.signal = signal;
        value.scopes = scopes;
        value.detail = detail;
        return value;
    }
    [[nodiscard]] const ScopeSignalColor& colorOf(const ScopeSignal signal) const {
        return signal == ScopeSignal::Working ? *working : *display;
    }
    [[nodiscard]] bloom::runtime::GpuScopeMeasureJob job(const ScopeMeasureSettings& value) const {
        return bloom::runtime::makeScopeMeasureJob(value, colorOf(value.signal), *ocio);
    }
    [[nodiscard]] bloom::runtime::ScopeFrameMeasure
    reference(const ScopeMeasureSettings& value) const {
        return bloom::runtime::measureScopeFrameCpu(*cpuFrame, value, colorOf(value.signal));
    }
};

bool run(Expectations& checks, const TestOptions& options,
         const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
         const std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>& processor,
         const bloom::document::Snapshot& snapshot, const Scope& scope) {
    Rig rig(options, plan, processor, snapshot);
    auto& service = *rig.service;
    if (!rig.ready()) {
        std::cout << "Unsupported: genuine offscreen GPU preparation unavailable: "
                  << service.status().residentDetail << '\n';
        checks.expect(!options.requireDevice, "required genuine offscreen GPU device available");
        return false;
    }
    const auto counters = [&] { return service.status().counters; };

    // ---- (1) parity ------------------------------------------------------------------------
    const auto first = rig.frame(1, 0, TaskPriority::Visible);
    if (!first)
        return true;
    checks.expect(isPacked(*first), "the Viewer frame is a GPU-prepared packed frame");
    const auto key = keyOf(*first);
    checks.expect(counters().scopeTapsKept == 1 && counters().scopeFillTapsKept == 0,
                  "the Viewer frame is tapped; no fill tap was kept");
    const auto geometry = service.scopeTapGeometry(key);
    checks.expect(geometry && !geometry->fill && geometry->compositionWidth == kSize &&
                      geometry->compositionHeight == kSize &&
                      geometry->dataWindow.extent().width() == kSize &&
                      geometry->dataWindow.extent().height() == kSize,
                  "the tap names the picture's windows and the composition's size");
    checks.expect(!service.scopeTapGeometry(
                      GpuScopeTapKey::of(rig.identity(1, 99))),
                  "a key no frame has names no tap");

    {
        const auto settings = scope.settings(ScopeSignal::Working, kNoChromaticity);
        const auto job = scope.job(settings);
        const auto measured =
            service.measureScopeTap(key, job, bloom::runtime::CancellationToken{});
        const auto reference = scope.reference(settings);
        checks.expect(measured.status == GpuScopeTapStatus::Measured &&
                          measured.route == bloom::runtime::ScopeRoute::Gpu &&
                          measured.source == bloom::runtime::ScopeSource::Rendered,
                      "the tap measures the Working signal on the device: " + measured.message);
        checks.expect(reference.status == ScopeFrameStatus::Measured &&
                          measured.counts == reference.counts && measured.area == reference.area,
                      "Working: the tap's tables equal the CPU reference's cell for cell");
        checks.expect(measured.counts.counted() == kSize * kSize,
                      "Working: every pixel is counted");
    }
    {
        auto settings = scope.settings(ScopeSignal::Working, kNoChromaticity);
        settings.region = bloom::runtime::ScopeRegion{.x = 40, .y = 30, .width = 100, .height = 90};
        const auto measured = service.measureScopeTap(key, scope.job(settings),
                                                     bloom::runtime::CancellationToken{});
        const auto reference = scope.reference(settings);
        checks.expect(measured.status == GpuScopeTapStatus::Measured &&
                          measured.counts == reference.counts && measured.area == reference.area &&
                          measured.counts.counted() == 100U * 90U,
                      "a region: the tap's tables equal the CPU reference's");
        settings.region = bloom::runtime::ScopeRegion{.x = 400, .y = 30, .width = 20, .height = 20};
        const auto outside = service.measureScopeTap(key, scope.job(settings),
                                                    bloom::runtime::CancellationToken{});
        checks.expect(outside.status == GpuScopeTapStatus::EmptyArea,
                      "a region outside the picture has nothing to count");
    }
    {
        const auto settings = scope.settings(ScopeSignal::Working, kScopeTableAll);
        const auto measured = service.measureScopeTap(key, scope.job(settings),
                                                     bloom::runtime::CancellationToken{});
        const auto reference = scope.reference(settings);
        checks.expect(measured.status == GpuScopeTapStatus::Measured &&
                          tablesWithinOneCell(measured.counts, reference.counts) &&
                          measured.counts.counted() == kSize * kSize,
                      "Working with Chromaticity: within one cell, every pixel counted");
    }
    {
        const auto settings = scope.settings(ScopeSignal::Display, kScopeTableAll);
        const auto measured = service.measureScopeTap(key, scope.job(settings),
                                                     bloom::runtime::CancellationToken{});
        const auto reference = scope.reference(settings);
        checks.expect(measured.status == GpuScopeTapStatus::Measured &&
                          reference.status == ScopeFrameStatus::Measured &&
                          tablesWithinOneCell(measured.counts, reference.counts) &&
                          measured.counts.counted() == kSize * kSize,
                      "Display with Chromaticity: within one cell, every pixel counted: " +
                          measured.message);
        checks.expect(measured.colorDispatches >= 2,
                      "the Display measure ran the display and the XYZ programs");
    }

    // ---- (2) no scene run -------------------------------------------------------------------
    for (const std::uint32_t detail : {0U, 1U}) {
        const auto before = counters();
        const auto preparationsBefore = rig.preparations->load();
        const auto dispatchesBefore = bloom::render::GpuScopeTables::nativeDispatchCount();
        auto settings = scope.settings(ScopeSignal::Display, kScopeTableAll, detail);
        const auto measured = service.measureScopeTap(key, scope.job(settings),
                                                     bloom::runtime::CancellationToken{});
        const auto after = counters();
        const auto layout =
            bloom::render::scopeTableLayout(settings.scopes, measured.area.width, detail);
        const std::string name = "detail " + std::to_string(detail) + ": ";
        checks.expect(measured.status == GpuScopeTapStatus::Measured &&
                          measured.counts.layout == layout,
                      name + "the Display measure is Measured at its detail: " + measured.message);
        checks.expect(after.residentGraphJobs == before.residentGraphJobs &&
                          after.nativeDispatches == before.nativeDispatches &&
                          after.nativeSubmissions == before.nativeSubmissions &&
                          after.scenesAhead == before.scenesAhead &&
                          rig.preparations->load() == preparationsBefore &&
                          after.cpuFallbacks == before.cpuFallbacks,
                      name + "no scene job, no scene dispatch and no preparation ran");
        checks.expect(after.fullFrameReadbacks == before.fullFrameReadbacks &&
                          after.packedTransfers == before.packedTransfers &&
                          measured.pictureReadbacks == 0,
                      name + "no picture was read back");
        checks.expect(measured.tableReadbacks == 1 && measured.tableReadbackBytes == layout.byteCount() &&
                          after.scopeTableReadbacks == before.scopeTableReadbacks + 1 &&
                          after.scopeTableReadbackBytes == layout.byteCount() &&
                          after.scopeTableReadbackBytesTotal ==
                              before.scopeTableReadbackBytesTotal + layout.byteCount(),
                      name + "one table readback of exactly the layout's bytes");
        checks.expect(measured.colorDispatches >= 2 &&
                          after.scopeColorDispatches ==
                              before.scopeColorDispatches + measured.colorDispatches &&
                          measured.tableDispatches >= 1 &&
                          bloom::render::GpuScopeTables::nativeDispatchCount() >=
                              dispatchesBefore + measured.tableDispatches &&
                          after.scopeMeasuresStarted == before.scopeMeasuresStarted + 1 &&
                          after.scopeMeasuresCompleted == before.scopeMeasuresCompleted + 1,
                      name + "only the color programs and the count kernel ran");
    }

    // ---- (3) hold ---------------------------------------------------------------------------
    {
        const auto settings = scope.settings(ScopeSignal::Working, kNoChromaticity);
        const auto job = scope.job(settings);
        const auto reference = scope.reference(settings);
        TableHold hold(rig.core);
        const auto before = counters();
        auto measure = measureAsync(service, key, job);
        if (!hold.awaitHeld("the held measure is in flight"))
            return true;
        for (std::uint64_t index = 0; index < 3; ++index) {
            const auto frame = rig.frame(10 + index, static_cast<std::int64_t>(10 + index),
                                         TaskPriority::Visible);
            checks.expect(frame && isPacked(*frame),
                          "a packed request completes while the measure is held");
        }
        const auto during = counters();
        checks.expect(notReady(measure) && Access::scopeRequestPending(*rig.core) &&
                          during.scopeMeasuresStarted == before.scopeMeasuresStarted + 1 &&
                          during.scopeMeasuresCompleted == before.scopeMeasuresCompleted,
                      "the measure stays in flight while the readback is held");
        checks.expect(during.cpuFallbacks == before.cpuFallbacks &&
                          during.packedTransfers == before.packedTransfers + 3 &&
                          during.residentCompletions == before.residentCompletions + 3,
                      "the Viewer's frames took the GPU route, with no CPU fallback");
        hold.release();
        const auto outcome = take(measure, "the released measure delivers");
        checks.expect(outcome && outcome->status == GpuScopeTapStatus::Measured &&
                          outcome->counts == reference.counts,
                      "releasing the hold delivers the tables of the frame that was tapped");
    }

    // ---- (4) Busy, Superseded --------------------------------------------------------------
    {
        const auto settings = scope.settings(ScopeSignal::Working, kNoChromaticity);
        const auto job = scope.job(settings);
        const auto current = rig.frame(20, 20, TaskPriority::Visible);
        if (!current)
            return true;
        const auto currentKey = keyOf(*current);
        {
            TableHold hold(rig.core);
            auto measure = measureAsync(service, currentKey, job);
            if (!hold.awaitHeld("the measure to hold is in flight"))
                return true;
            const auto busy = service.measureScopeTap(currentKey, job,
                                                      bloom::runtime::CancellationToken{});
            checks.expect(busy.status == GpuScopeTapStatus::Busy &&
                              counters().scopeMeasuresBusy == 1,
                          "a second request while one is in flight is Busy");
            hold.release();
            const auto outcome = take(measure, "the first measure delivers after Busy");
            checks.expect(outcome && outcome->status == GpuScopeTapStatus::Measured,
                          "the request in flight is not disturbed by the Busy one");
        }
        // A Viewer frame is active (its transfer held) when the measure is requested, so the
        // measure waits; the frame then replaces the tap before the measure began.
        {
            TransferHold transfer(rig.core);
            auto newer = rig.submit(rig.identity(21, 21), TaskPriority::Visible);
            if (!awaitCondition("the newer frame reaches its held transfer", [&] {
                    return Access::residentActivePhase(*rig.core) ==
                           bloom::runtime::detail::ResidentNativePhase::Transfer;
                }))
                return true;
            const auto yieldsBefore = counters().scopeMeasureYields;
            auto measure = measureAsync(service, currentKey, job);
            if (!awaitCondition("the measure yields to the Viewer frame", [&] {
                    return counters().scopeMeasureYields == yieldsBefore + 1;
                }))
                return true;
            checks.expect(Access::scopeRequestPending(*rig.core) &&
                              !Access::scopeStepperBegan(*rig.core) && notReady(measure),
                          "a measure does not begin while a Viewer-class stage is active");
            transfer.release();
            const auto frame = rig.finish(newer, "the newer frame");
            checks.expect(frame && isPacked(*frame), "the newer Viewer frame completes");
            const auto outcome = take(measure, "the superseded measure ends");
            checks.expect(outcome && outcome->status == GpuScopeTapStatus::Skipped &&
                              outcome->skip == GpuScopeTapSkip::Superseded,
                          "a tap replaced by a newer Viewer frame before its measure began "
                          "gives Skipped(Superseded)");
            if (frame) {
                const auto newerOutcome = service.measureScopeTap(
                    keyOf(*frame), job, bloom::runtime::CancellationToken{});
                checks.expect(newerOutcome.status == GpuScopeTapStatus::Measured,
                              "the newer frame's own key is measured");
            }
        }
        // A newer tap of the SAME key (the same frame again) is what the key names: it is
        // measured.
        const auto again = rig.frame(22, 22, TaskPriority::Visible);
        const auto again2 = rig.frame(23, 22, TaskPriority::Visible);
        if (again && again2) {
            checks.expect(keyOf(*again) == keyOf(*again2),
                          "two generations of one frame share a tap key");
            const auto outcome = service.measureScopeTap(keyOf(*again), job,
                                                         bloom::runtime::CancellationToken{});
            checks.expect(outcome.status == GpuScopeTapStatus::Measured,
                          "a newer tap of the same key is measured");
        }
    }

    // ---- (5) cancel mid-measure ----------------------------------------------------------------
    {
        const auto settings = scope.settings(ScopeSignal::Working, kNoChromaticity);
        const auto job = scope.job(settings);
        const auto current = rig.frame(30, 30, TaskPriority::Visible);
        if (!current)
            return true;
        const auto currentKey = keyOf(*current);
        TableHold hold(rig.core);
        auto outcome = std::make_shared<GpuScopeTapMeasureOutcome>();
        auto done = std::make_shared<std::atomic_bool>(false);
        auto task = rig.scheduler.submit<int>(
            TaskRequest("scope measure to cancel",
                        {.kind = bloom::runtime::TaskOwnerKind::PanelRequest,
                         .id = bloom::runtime::TaskOwnerId::fromRaw(5)},
                        TaskPriority::Visible),
            [&service, currentKey, &job, outcome, done](TaskContext& context) {
                *outcome = service.measureScopeTap(currentKey, job, context.cancellation());
                done->store(true);
                return TaskResult<int>::succeeded(0);
            });
        checks.expect(task.accepted(), "the cancellable measure task is admitted");
        if (!task.accepted() || !hold.awaitHeld("the measure to cancel is in flight"))
            return true;
        task.handle.cancel();
        if (!awaitCondition("cancellation reaches the submission in flight",
                            [&] { return Access::scopeStopIssued(*rig.core); }))
            return true;
        checks.expect(!done->load() && Access::scopeRequestPending(*rig.core),
                      "a cancelled measure delivers nothing before its fence is proven retired");
        hold.release();
        if (!awaitCondition("the cancelled measure delivers", [&] { return done->load(); }))
            return true;
        checks.expect(outcome->status == GpuScopeTapStatus::Cancelled &&
                          outcome->counts.words.empty() && counters().scopeMeasuresCancelled == 1,
                      "the cancelled measure ends Cancelled and publishes no table");
        checks.expect(!Access::scopeRequestPending(*rig.core),
                      "the request and its native state are gone after the fence proof");
        // The service keeps working: the process table slot is free again and frames complete.
        const auto next = rig.frame(31, 31, TaskPriority::Visible);
        checks.expect(next && isPacked(*next), "a frame completes after the cancelled measure");
        if (next) {
            const auto measured = service.measureScopeTap(keyOf(*next), job,
                                                          bloom::runtime::CancellationToken{});
            checks.expect(measured.status == GpuScopeTapStatus::Measured,
                          "a measure succeeds after the cancelled one");
        }
    }

    // ---- (6) fill tap -----------------------------------------------------------------------
    {
        const auto settings = scope.settings(ScopeSignal::Working, kNoChromaticity);
        const auto job = scope.job(settings);
        const auto viewer = rig.frame(40, 40, TaskPriority::Visible);
        if (!viewer)
            return true;
        const auto viewerKey = keyOf(*viewer);
        // A fill frame with the flag off keeps nothing and is never held back.
        const auto base = counters();
        const auto plain = rig.frame(41, 41, TaskPriority::Foreground, false);
        const auto plainKey = plain ? keyOf(*plain) : GpuScopeTapKey{};
        checks.expect(plain && isPacked(*plain) && !service.scopeTapGeometry(plainKey) &&
                          !Access::scopeFillTapHeld(*rig.core) &&
                          counters().scopeFillTapsKept == base.scopeFillTapsKept &&
                          counters().scopeFillStageWaits == base.scopeFillStageWaits &&
                          counters().scopeTapsKept == base.scopeTapsKept,
                      "an unflagged fill frame keeps no tap and is not paced");
        checks.expect(service.scopeTapGeometry(viewerKey).has_value(),
                      "the unflagged fill frame did not replace the Viewer's tap");
        // A flagged fill frame is tapped, in its own slot.
        const auto firstFill = rig.frame(42, 42, TaskPriority::Foreground, true);
        if (!firstFill)
            return true;
        const auto firstKey = keyOf(*firstFill);
        const auto firstGeometry = service.scopeTapGeometry(firstKey);
        checks.expect(isPacked(*firstFill) && firstGeometry && firstGeometry->fill &&
                          counters().scopeFillTapsKept == base.scopeFillTapsKept + 1 &&
                          counters().scopeTapsKept == base.scopeTapsKept,
                      "a flagged fill frame is tapped, in the fill slot");
        const auto viewerGeometry = service.scopeTapGeometry(viewerKey);
        checks.expect(viewerGeometry && !viewerGeometry->fill,
                      "the fill tap did not replace the Viewer's tap");
        // A second flagged fill frame does not start its scene work while the slot is occupied.
        const auto beforeSecond = counters();
        auto second = rig.submit(rig.identity(43, 43), TaskPriority::Foreground, true);
        checks.expect(second.accepted(), "the second flagged fill frame is admitted");
        if (!second.accepted() ||
            !awaitCondition("the second fill frame is held back", [&] {
                return counters().scopeFillStageWaits == beforeSecond.scopeFillStageWaits + 1;
            }))
            return true;
        checks.expect(counters().residentGraphJobs == beforeSecond.residentGraphJobs &&
                          !second.handle.tryTakeResult().has_value(),
                      "the held fill frame has started no scene work and has not completed");
        // A Viewer frame meanwhile is not held and does not replace the fill tap.
        const auto during = rig.frame(44, 44, TaskPriority::Visible);
        checks.expect(during && isPacked(*during) &&
                          service.scopeTapGeometry(firstKey).has_value() &&
                          service.scopeTapGeometry(keyOf(*during)).has_value() &&
                          Access::scopeFillTapHeld(*rig.core),
                      "a Viewer frame completes beside the held fill and replaces only the "
                      "Viewer's tap");
        // The measure of the fill tap frees the slot; the held frame then proceeds.
        const auto measured = service.measureScopeTap(firstKey, job,
                                                      bloom::runtime::CancellationToken{});
        const auto reference = scope.reference(settings);
        checks.expect(measured.status == GpuScopeTapStatus::Measured &&
                          measured.counts == reference.counts,
                      "the fill tap is measured from its own slot");
        checks.expect(!service.scopeTapGeometry(firstKey).has_value(),
                      "a measured fill tap frees its slot");
        const auto secondFrame = rig.finish(second, "the held fill frame");
        checks.expect(secondFrame && isPacked(*secondFrame) &&
                          counters().scopeFillTapsKept == base.scopeFillTapsKept + 2 &&
                          counters().residentGraphJobs > beforeSecond.residentGraphJobs,
                      "after the measure the held fill frame proceeds and is tapped");
        if (!secondFrame)
            return true;
        // A dropped fill tap also lets the next flagged frame start.
        const auto secondKey = keyOf(*secondFrame);
        auto third = rig.submit(rig.identity(45, 45), TaskPriority::Foreground, true);
        if (!third.accepted() ||
            !awaitCondition("the third fill frame is held back", [&] {
                return counters().scopeFillStageWaits == beforeSecond.scopeFillStageWaits + 2;
            }))
            return true;
        service.dropScopeTap(secondKey);
        checks.expect(!service.scopeTapGeometry(secondKey).has_value(),
                      "a dropped tap names nothing at once");
        const auto thirdFrame = rig.finish(third, "the third fill frame");
        checks.expect(thirdFrame && isPacked(*thirdFrame) &&
                          counters().scopeFillTapsKept == base.scopeFillTapsKept + 3,
                      "after a drop the held fill frame proceeds");
        if (thirdFrame) {
            const auto dropped = service.measureScopeTap(secondKey, job,
                                                         bloom::runtime::CancellationToken{});
            checks.expect(dropped.status == GpuScopeTapStatus::Skipped &&
                              dropped.skip == GpuScopeTapSkip::TapGone,
                          "a dropped tap is Skipped(TapGone)");
            service.dropScopeTap(keyOf(*thirdFrame));
            checks.expect(awaitCondition("the dropped fill tap is freed",
                                         [&] { return !Access::scopeFillTapHeld(*rig.core); }),
                          "the owner frees a dropped fill tap");
        }
    }

    // ---- (6b) a Viewer tap and a fill tap of one frame ------------------------------------------
    // The frame the Viewer shows is also a fill frame (the same key: the request generation and
    // the view adjust are not in it), so both slots hold it. The correct behavior, the owner's:
    //   * the frame is counted once, from its float picture (one measure, one table readback), and
    //     the tables are the CPU reference's;
    //   * both requesters are answered: the shown frame's request by that measure, and the fill's
    //     by the same tables (it does not measure again; it lets its tap go);
    //   * neither slot's tap is lost to the other's drop: letting the fill tap go leaves the
    //     Viewer's tap, which a later measure of the frame still counts;
    //   * the fill slot is freed, so the next flagged fill frame is not held back;
    //   * nothing is measured twice: the counters of measures and table readbacks move once.
    {
        const auto settings = scope.settings(ScopeSignal::Working, kNoChromaticity);
        const auto job = scope.job(settings);
        const auto reference = scope.reference(settings);
        const auto viewer = rig.frame(50, 50, TaskPriority::Visible);
        const auto fill = rig.frame(51, 50, TaskPriority::Foreground, true);
        if (!viewer || !fill)
            return true;
        const auto sharedKey = keyOf(*viewer);
        checks.expect(sharedKey == keyOf(*fill),
                      "the Viewer's frame and the fill's frame of one time share a key");
        const auto sharedGeometry = service.scopeTapGeometry(sharedKey);
        checks.expect(sharedGeometry && !sharedGeometry->fill && Access::scopeViewerTapHeld(*rig.core) &&
                          Access::scopeFillTapHeld(*rig.core),
                      "both slots hold the frame: the Viewer's answers the key");
        const auto before = counters();
        const auto measured = service.measureScopeTap(sharedKey, job,
                                                      bloom::runtime::CancellationToken{});
        checks.expect(measured.status == GpuScopeTapStatus::Measured &&
                          measured.counts == reference.counts,
                      "the frame is counted from its float picture: the CPU reference's tables");
        checks.expect(counters().scopeMeasuresCompleted == before.scopeMeasuresCompleted + 1 &&
                          counters().scopeTableReadbacks == before.scopeTableReadbacks + 1,
                      "one measure and one table readback for the frame");
        // The fill's requester is answered by the tables above; it lets its tap go.
        service.dropScopeTap(sharedKey);
        checks.expect(awaitCondition("the fill slot is freed",
                                     [&] { return !Access::scopeFillTapHeld(*rig.core); }),
                      "the fill slot is freed, so the fill is not held");
        const auto after = service.scopeTapGeometry(sharedKey);
        checks.expect(after && !after->fill && Access::scopeViewerTapHeld(*rig.core),
                      "the fill's drop did not take the Viewer's tap of the same frame");
        const auto again = service.measureScopeTap(sharedKey, job,
                                                   bloom::runtime::CancellationToken{});
        checks.expect(again.status == GpuScopeTapStatus::Measured &&
                          again.counts == reference.counts,
                      "the Viewer's tap still measures the frame after the fill tap went");
        const auto next = rig.frame(52, 51, TaskPriority::Foreground, true);
        checks.expect(next && isPacked(*next) && Access::scopeFillTapHeld(*rig.core),
                      "the next flagged fill frame is not held back and is tapped");
        if (next)
            service.dropScopeTap(keyOf(*next));
    }
    checks.expect(counters().cpuFallbacks == 0, "no frame of the test took the CPU path");
    return true;
}

// (7) Shutdown with a request queued, and with one in flight, unblocks the caller.
void shutdownCases(Expectations& checks, const TestOptions& options,
                   const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
                   const std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>&
                       processor,
                   const bloom::document::Snapshot& snapshot, const Scope& scope) {
    const auto settings = scope.settings(ScopeSignal::Working, kNoChromaticity);
    const auto job = scope.job(settings);
    for (const bool inFlight : {false, true}) {
        const std::string name = inFlight ? "in flight: " : "queued: ";
        // The holds are destroyed after the service: the owner may still read their seams while
        // it drains.
        std::unique_ptr<TransferHold> transfer;
        std::unique_ptr<TableHold> tables;
        std::optional<bloom::runtime::TaskSubmission<PreviewPreparationResultHandle>> busy;
        std::future<GpuScopeTapMeasureOutcome> measure;
        Rig rig(options, plan, processor, snapshot);
        if (!rig.ready())
            return;
        const auto frame = rig.frame(1, 0, TaskPriority::Visible);
        if (!frame)
            return;
        const auto key = keyOf(*frame);
        transfer = std::make_unique<TransferHold>(rig.core);
        if (inFlight) {
            tables = std::make_unique<TableHold>(rig.core);
            measure = measureAsync(*rig.service, key, job);
            if (!tables->awaitHeld("the measure is in flight"))
                return;
        } else {
            // An active Viewer frame (its transfer held) keeps the measure queued.
            busy = rig.submit(rig.identity(2, 1), TaskPriority::Visible);
            if (!awaitCondition("the Viewer frame reaches its held transfer", [&] {
                    return Access::residentActivePhase(*rig.core) ==
                           bloom::runtime::detail::ResidentNativePhase::Transfer;
                }))
                return;
            measure = measureAsync(*rig.service, key, job);
            if (!awaitCondition("the measure is queued",
                                [&] { return rig.service->status().counters.scopeMeasureYields == 1; }))
                return;
            checks.expect(!Access::scopeStepperBegan(*rig.core), name + "the measure has not begun");
        }
        checks.expect(notReady(measure), name + "the caller is blocked");
        rig.service->beginShutdown();
        const auto outcome = take(measure, name + "shutdown unblocks the caller");
        checks.expect(outcome && outcome->status == GpuScopeTapStatus::Cancelled &&
                          outcome->counts.words.empty(),
                      name + "the request ends Cancelled and publishes no table");
        // Only now may the held transfer finish: the caller did not wait for it.
        transfer->release();
        if (tables)
            tables->release();
    }
}


// ---- memory admission and yield (#724) ------------------------------------------------------
//
// Every byte size below comes from the device's own allocation sizes (a probe device) and the
// layout of the scope tables, and every card is a declared card (`deviceMemoryLimitBytes`), so
// nothing here depends on the machine's real free memory. A Viewer frame's need is stated in its
// envelope; an envelope with no measured peak yet is admitted at exactly its stated need, so a
// test that wants a frame with a need of its own states a new working allowance for it.
//
//   (a) a card the size of the measure's own memory fits the Viewer's frame but not the frame plus
//       the measure's memory: the measure ends Skipped(NoRoom) holding nothing, the Viewer's frames
//       keep completing, and nothing is made to wait or give anything back for the measure;
//       a measure the card could hold but the room is held right now waits at the service holding
//       nothing and is admitted when the room returns, and ends Skipped(NoRoom) when its tap is
//       given back meanwhile;
//   (b) the taps are given back first when a Viewer frame needs their bytes: before the scene
//       cache, the parked storage or the UI's finished frames;
//   (c) a measure in flight (readback held) never makes a Viewer frame wait: it is tracked against
//       its Background phase, the frames park that phase and complete, and a frame that is short
//       of memory tells the measure to stop (it is polled to its end, then Skipped(NoRoom));
//   (d) a fill tap that does not fit as Background is refused: the frame is delivered, the slot
//       stays free, no later frame is held back, and the key measures Skipped(NoRoom);
//   (e) the idle trim releases the Viewer tap; the next shown frame is tapped again.


struct Derived final {
    std::uint64_t image = 0;
    // The same for the larger picture of the frames that must be short of memory.
    std::uint64_t largeImage = 0;
    std::uint64_t layoutBytes = 0;
    // The measure's device need for a Display job with Chromaticity (two color outputs, the
    // table buffer and its staging copy), as the service computes it from the same sizes.
    std::uint64_t measureNeed = 0;
};

[[nodiscard]] std::optional<Derived> deriveSizes(const TestOptions& options) {
    bloom::render::GpuDeviceCreationOptions probeOptions;
    probeOptions.loader_path = options.loaderPath;
    auto created = bloom::render::GpuDevice::create(probeOptions);
    if (!created.hasValue())
        return std::nullopt;
    const auto budget = created.device->availableAllocationBudget();
    if (!budget.allocation_sizes || !budget.allocation_sizes->image ||
        !budget.allocation_sizes->buffer)
        return std::nullopt;
    Derived derived;
    derived.image = budget.allocation_sizes->image(kSize, kSize);
    derived.largeImage = budget.allocation_sizes->image(kLargeSize, kLargeSize);
    const auto layout = bloom::render::scopeTableLayout(
        kScopeTableAll, bloom::render::kScopeWaveformMaxColumns, 0);
    derived.layoutBytes = layout.byteCount();
    derived.measureNeed =
        2 * derived.image +
        2 * std::max(derived.layoutBytes, budget.allocation_sizes->buffer(derived.layoutBytes));
    return derived;
}

// Room the test holds on the policy's books (a Viewer-class phase with a claim on its device need),
// so a frame is short of exactly what the test leaves. Released by destruction.
struct HeldRoom final {
    bloom::render::GpuProcessingPhaseGrant grant;
    std::optional<bloom::render::NativeImageStorageToken> token;
};

[[nodiscard]] HeldRoom holdRoom(bloom::runtime::GpuProcessingPhasePolicy& policy,
                                const std::uint64_t bytes, const bool claim = true) {
    HeldRoom held;
    held.grant = policy.reserve(
        {.output = bloom::runtime::GpuProcessingPhaseOutput::PackedPreview,
         .pixels = 1,
         .metadataBytes = 4096,
         .requestedExecutorBytes = std::uint64_t{0},
         .deviceWorkingBytes = bytes,
         .sourceFree = true,
         .native = true,
         .admissionClass = bloom::runtime::GpuAdmissionClass::Viewer});
    // Without a claim only the policy's record of the Viewer's last need changes.
    if (!held.grant.lease || !claim)
        return held;
    const bloom::render::NativeImageStorageIdentity identity{
        .allocatorGeneration = 1, .memoryType = 0, .heap = 0};
    auto reserved = bloom::render::NativeImageStorageToken::reserve(held.grant.lease, bytes, identity);
    held.token = std::move(reserved.token);
    return held;
}

// Waits for the service's card report, then shapes the usable device capacity to the card the
// rig's case plans for: the headroom (the policy's own, public setting for memory that is not the
// compositor's) takes the rest of the declared card, so the books say exactly the stated size on
// every device whatever its allocator holds (a plain shortfall of the case's own, never a
// reading of real free memory). A failing check names both sizes.
[[nodiscard]] bool deviceReported(const Rig& rig, Expectations& checks) {
    if (!awaitCondition("the service reports its card to the policy",
                        [&] { return rig.policy->usableCapacity().deviceReady; }))
        return false;
    if (rig.usableTarget == 0)
        return true;
    const auto usable = rig.policy->usableCapacity().deviceUsableBytes;
    checks.expect(usable >= rig.usableTarget,
                  "the declared card holds the usable capacity the case plans for (usable " +
                      std::to_string(usable) + ", planned " + std::to_string(rig.usableTarget) +
                      ", declared " + std::to_string(modelledCardBytes) + ")");
    if (usable < rig.usableTarget)
        return false;
    rig.policy->setHeadroom({.ramBytes = 0, .vramBytes = usable - rig.usableTarget});
    return awaitCondition("the policy's books say the planned card", [&] {
        return rig.policy->usableCapacity().deviceUsableBytes <= rig.usableTarget;
    });
}

// Waits until the service has taken a device report of its own after everything the test has done
// so far. The service counts its reports (`counters.deviceReports`): a report counted after the
// test's last action may have been sampled before it, so the wait is for two more, each woken by a
// notify, and then for two turns of the loop.
[[nodiscard]] bool awaitDeviceReport(const Rig& rig) {
    const auto reportsBefore = rig.service->status().counters.deviceReports;
    if (!awaitCondition("the service takes two device reports", [&] {
            rig.core->notify();
            return rig.service->status().counters.deviceReports >= reportsBefore + 2;
        }))
        return false;
    // A turn that was already running when the wake came may end before it; the next one is whole.
    rig.core->notify();
    const auto first = rig.service->status().counters.ownerServiceNanoseconds;
    std::optional<std::uint64_t> second;
    return awaitCondition("the service takes two turns", [&] {
        rig.core->notify();
        const auto now = rig.service->status().counters.ownerServiceNanoseconds;
        if (!second && now != first)
            second = now;
        return second && now != *second;
    });
}

// Shapes the usable device capacity so that `room` bytes are free beside the claims held now: the
// same headroom setting as `deviceReported`, so the books say exactly that room on every device
// and no claim the test holds stands in for memory (a device that estimates its free memory from
// its images would count such a claim against them). The policy's capacity is the device's own
// estimate less the headroom, and the estimate moves with the device's images, so the headroom is
// moved by what a fresh report says is still missing, until the report says the room.
[[nodiscard]] bool leaveRoom(const Rig& rig, const std::uint64_t room) {
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (!awaitDeviceReport(rig))
            return false;
        const auto capacity = rig.policy->usableCapacity();
        if (capacity.deviceFreeBytes == room)
            return true;
        const auto headroom = rig.policy->headroom();
        std::uint64_t vram = headroom.vramBytes;
        if (capacity.deviceFreeBytes > room) {
            vram += capacity.deviceFreeBytes - room;
        } else {
            const auto missing = room - capacity.deviceFreeBytes;
            if (vram < missing)
                return false;
            vram -= missing;
        }
        rig.policy->setHeadroom({.ramBytes = headroom.ramBytes, .vramBytes = vram});
    }
    return false;
}

void pressureCases(Expectations& checks, const TestOptions& options,
                   const std::shared_ptr<const bloom::runtime::CompiledCompositionPlan>& plan,
                   const std::shared_ptr<const bloom::color::PreparedCpuDisplayProcessorHandle>& processor,
                   const bloom::document::Snapshot& snapshot, const Scope& scope) {
    const auto derived = deriveSizes(options);
    if (!derived) {
        checks.expect(false, "the probe device reports its allocation sizes");
        return;
    }
    const auto displayJob = scope.job(scope.settings(ScopeSignal::Display, kScopeTableAll));
    const auto displayReference = scope.reference(scope.settings(ScopeSignal::Display, kScopeTableAll));
    const auto none = bloom::runtime::CancellationToken{};
    const auto text = [](const std::uint64_t value) { return std::to_string(value); };

    // A device's own numbers, measured on its own service: the smallest declared card (doubling
    // from 16 MiB) on which its first Viewer frame completes as a device frame; every case then
    // declares a card of at least twice that and at least twice the largest card it plans for
    // (`modelledCardBytes`), and shapes the usable capacity to its plan. The Viewer's own need is
    // what the policy records after that first frame on the roomy card.
    const auto firstFrame = [&](const std::uint64_t card, const std::uint64_t working,
                                std::uint64_t& need) {
        Rig probe(options, plan, processor, snapshot, {.card = card, .viewerWorkingBytes = working});
        if (!probe.ready() || !awaitCondition("the probe service reports its card", [&] {
                return probe.policy->usableCapacity().deviceReady;
            }))
            return false;
        const auto frame = probe.frame(1, 0, TaskPriority::Visible);
        if (!frame || !isPacked(*frame))
            return false;
        need = probe.policy->viewerPriority().reserveDeviceBytes;
        return true;
    };
    std::uint64_t smallestCard = 0;
    std::uint64_t viewerNeed4 = 0;
    for (std::uint64_t card = 16ULL * 1024 * 1024; card <= 2048ULL * 1024 * 1024 && smallestCard == 0;
         card *= 2) {
        if (firstFrame(card, 4 * derived->image, viewerNeed4))
            smallestCard = card;
    }
    const auto sizes = "image " + text(derived->image) + ", table layout " +
                       text(derived->layoutBytes) + ", measure need " + text(derived->measureNeed) +
                       ", Viewer need " + text(viewerNeed4) + ", smallest card with a device frame " +
                       text(smallestCard);
    checks.expect(smallestCard != 0 && viewerNeed4 != 0,
                  "the probe Viewer frame completes on a declared card (" + sizes + ")");
    if (smallestCard == 0 || viewerNeed4 == 0)
        return;
    modelledCardBytes = std::max({2 * smallestCard, 2 * (viewerNeed4 + 4 * derived->measureNeed),
                                  64 * derived->largeImage});
    struct ModelledCardReset final {
        ~ModelledCardReset() { modelledCardBytes = 0; }
    } resetModelledCard;

    // The frame that must be short of memory is a larger picture than any frame its rig rendered
    // before (`kLargeSize`): no storage the device parked or reuses for the earlier frames has its
    // shape, so the frame allocates new native storage whatever a device reuses. What it allocates
    // and the need its envelope states are measured on this device, on a roomy card, after the same
    // frames the case renders first (a Viewer frame, and a flagged fill frame when `withFill`).
    const auto largePlan = makePlan(kLargeSize, kLargeSize);
    // What the device's native storage reuse did for the frames so far.
    const auto reuseNumbers = [&](const Rig& rig) {
        const auto r = rig.core->device->nativeStorageReuseStats();
        return "parked " + text(r.parkedDeviceBytes) + " bytes in " + text(r.parkedEntries) +
               " entries, reused " + text(r.reused) + ", created " + text(r.created);
    };
    struct LargeFrame final {
        // The need its envelope stated, as the policy recorded it.
        std::uint64_t need = 0;
        // How far the device claims rose above what they were before the frame (the policy's
        // high-water mark of the claims): the room the frame takes while it runs, new native
        // storage and storage it takes back from the device's parked storage alike.
        std::uint64_t growth = 0;
        // The device's storage reuse before and after the frame on the probe.
        std::string reuseBefore;
        std::string reuseAfter;
    };
    const auto probeLargeFrame = [&](const bool withFill, const std::uint64_t working,
                                     LargeFrame& out) {
        Rig probe(options, plan, processor, snapshot,
                  {.card = modelledCardBytes / 4,
                   .viewerWorkingBytes = working,
                   .sceneCacheBytes = std::optional<std::uint64_t>{0},
                   .idleTrimMilliseconds = kNoIdleTrim,
                   .largePlan = largePlan});
        if (!probe.ready() || !deviceReported(probe, checks))
            return false;
        if (!probe.frame(1, 0, TaskPriority::Visible))
            return false;
        if (withFill && !probe.frame(2, 1, TaskPriority::Foreground, true))
            return false;
        if (!awaitCondition("the probe's phases retire", [&] {
                return probe.policy->snapshot().reservations.admittedPhases == 0;
            }))
            return false;
        const auto claimsBefore = probe.policy->usableCapacity().deviceClaimBytes;
        out.reuseBefore = reuseNumbers(probe);
        const auto large = probe.frame(kLargeGeneration, 2, TaskPriority::Visible);
        if (!large || !isPacked(*large))
            return false;
        out.reuseAfter = reuseNumbers(probe);
        if (!awaitCondition("the probe's phases retire", [&] {
                return probe.policy->snapshot().reservations.admittedPhases == 0;
            }))
            return false;
        const auto peakClaims = probe.policy->snapshot().reservations.peakTotalDeviceClaimBytes;
        out.growth = peakClaims > claimsBefore ? peakClaims - claimsBefore : 0;
        out.need = probe.policy->viewerPriority().reserveDeviceBytes;
        return true;
    };

    // ---- (a) no room beside the Viewer ------------------------------------------------------
    {
        Rig rig(options, plan, processor, snapshot,
                {.card = viewerNeed4 + derived->measureNeed / 2,
                 .viewerWorkingBytes = 4 * derived->image});
        auto& service = *rig.service;
        if (!rig.ready() || !deviceReported(rig, checks))
            return;
        const auto counters = [&] { return service.status().counters; };
        const auto first = rig.frame(1, 0, TaskPriority::Visible);
        if (!first)
            return;
        checks.expect(isPacked(*first), "(a) the Viewer's frame completes on the small card");
        const auto viewerNeed = rig.policy->viewerPriority().reserveDeviceBytes;
        const auto usable = rig.policy->usableCapacity().deviceUsableBytes;
        checks.expect(viewerNeed != 0 && viewerNeed <= usable &&
                          derived->measureNeed + viewerNeed > usable,
                      "(a) the card fits the Viewer's frame but not the frame plus the measure "
                      "(Viewer need " + text(viewerNeed) + ", usable card " + text(usable) +
                          ", measure need " + text(derived->measureNeed) + ", picture allocation " +
                          text(derived->image) + ")");
        const auto owned = rig.policy->snapshot().reservations;
        const auto before = counters();
        const auto outcome = service.measureScopeTap(keyOf(*first), displayJob, none);
        const auto after = counters();
        checks.expect(outcome.status == GpuScopeTapStatus::Skipped &&
                          outcome.skip == GpuScopeTapSkip::NoRoom,
                      "(a) the measure ends Skipped(NoRoom): " + outcome.message);
        checks.expect(after.scopeMeasureNeedBytes == derived->measureNeed,
                      "(a) the measure's need is the color outputs and the table buffers from the "
                      "device's allocation sizes");
        checks.expect(after.scopeMeasuresNoRoom == before.scopeMeasuresNoRoom + 1 &&
                          after.scopeMeasuresStarted == before.scopeMeasuresStarted &&
                          after.scopeMeasureAdmissions == 0 && after.scopeColorDispatches == 0 &&
                          after.scopeMeasuresGivenBack == 0,
                      "(a) the measure was never admitted, never began and gave nothing back");
        const auto held = rig.policy->snapshot().reservations;
        checks.expect(held.peakOwnedNativeBytes == owned.peakOwnedNativeBytes &&
                          held.ownedNativeBufferBytes == 0 &&
                          held.admittedPhases == owned.admittedPhases,
                      "(a) the refused measure took no native memory and no phase");
        for (std::uint64_t index = 0; index < 3; ++index) {
            const auto frame = rig.frame(10 + index, static_cast<std::int64_t>(10 + index),
                                         TaskPriority::Visible);
            checks.expect(frame && isPacked(*frame),
                          "(a) the Viewer's frames keep completing as GpuPreparedPacked");
        }
        const auto end = counters();
        checks.expect(end.capacityWaits == before.capacityWaits &&
                          end.cpuFallbacks == before.cpuFallbacks &&
                          end.deviceRoomReclaims == before.deviceRoomReclaims &&
                          service.status().residentFrameEvictionSerial == 0 &&
                          end.scopeViewerTapYields == 0,
                      "(a) the Viewer's capacity-wait and CPU-fallback counters did not move, and "
                      "nothing was reclaimed or given back for the measure");
    }

    // ---- (a2) a measure the card could hold waits, then is admitted or yields ----------------
    {
        Rig rig(options, plan, processor, snapshot,
                {.card = viewerNeed4 + 4 * derived->measureNeed,
                 .viewerWorkingBytes = 4 * derived->image});
        auto& service = *rig.service;
        if (!rig.ready() || !deviceReported(rig, checks))
            return;
        const auto counters = [&] { return service.status().counters; };
        const auto first = rig.frame(1, 0, TaskPriority::Visible);
        if (!first)
            return;
        const auto key = keyOf(*first);
        // Room is held right now by another admitted phase (the test's own claim): the card could
        // hold the measure beside the Viewer, the free room cannot.
        const auto viewerNeed = rig.policy->viewerPriority().reserveDeviceBytes;
        const auto free = rig.policy->usableCapacity().deviceFreeBytes;
        checks.expect(free > derived->measureNeed + viewerNeed,
                      "(a2) the card holds the measure beside the Viewer's need (free " + text(free) +
                          ", Viewer need " + text(viewerNeed) + ", measure need " +
                          text(derived->measureNeed) + ")");
        if (free <= derived->measureNeed + viewerNeed)
            return;
        const auto claim = free - (derived->measureNeed + viewerNeed) + 1;
        const bloom::render::NativeImageStorageIdentity identity{
            .allocatorGeneration = 1, .memoryType = 0, .heap = 0};
        const auto reserveClaim = [&] {
            auto grant = rig.policy->reserve(
                {.output = bloom::runtime::GpuProcessingPhaseOutput::PackedPreview,
                 .pixels = 1,
                 .metadataBytes = 4096,
                 .requestedExecutorBytes = std::uint64_t{0},
                 .deviceWorkingBytes = claim,
                 .sourceFree = true,
                 .native = true,
                 .admissionClass = bloom::runtime::GpuAdmissionClass::Background});
            return grant;
        };
        {
            auto grant = reserveClaim();
            auto token = grant.lease ? bloom::render::NativeImageStorageToken::reserve(
                                           grant.lease, claim, identity)
                                     : bloom::render::NativeImageStorageGrant{};
            checks.expect(grant.lease && token.token.has_value(),
                          "(a2) the test's phase claims room beside the measure (grant " +
                              std::to_string(static_cast<int>(grant.status)) + ", domain " +
                              std::to_string(static_cast<int>(grant.domain)) + ", token " +
                              std::to_string(static_cast<int>(token.status)) + ", claim " +
                              std::to_string(claim) + ", free " + std::to_string(free) + ")");
            if (!grant.lease || !token.token)
                return;
            auto measure = measureAsync(service, key, displayJob);
            if (!awaitCondition("the measure waits for room", [&] {
                    return counters().scopeMeasureAdmissionWaits == 1;
                }))
                return;
            checks.expect(notReady(measure) && Access::scopeRequestPending(*rig.core) &&
                              counters().scopeMeasuresStarted == 0 &&
                              counters().scopeMeasureAdmissions == 0 &&
                              rig.policy->snapshot().reservations.admittedPhases == 1,
                          "(a2) the waiting measure holds nothing and has not begun");
            // The room returns: the measure is admitted and measured.
            token.token.reset();
            grant.lease.reset();
            const auto outcome = take(measure, "the measure admitted when the room returned");
            checks.expect(outcome && outcome->status == GpuScopeTapStatus::Measured &&
                              tablesWithinOneCell(outcome->counts, displayReference.counts) &&
                              counters().scopeMeasureAdmissions == 1,
                          "(a2) the measure is admitted as Background once the room returns");
        }
        if (!awaitCondition("the measure's phase retires", [&] {
                return rig.policy->snapshot().reservations.admittedPhases == 0;
            }))
            return;
        // Again with the room held: this time the tap is given back while the measure waits.
        {
            auto grant = reserveClaim();
            auto token = grant.lease ? bloom::render::NativeImageStorageToken::reserve(
                                           grant.lease, claim, identity)
                                     : bloom::render::NativeImageStorageGrant{};
            if (!grant.lease || !token.token) {
                checks.expect(false, "(a2) the test's phase claims room again");
                return;
            }
            const auto waitsBefore = counters().scopeMeasureAdmissionWaits;
            auto measure = measureAsync(service, key, displayJob);
            if (!awaitCondition("the second measure waits for room", [&] {
                    return counters().scopeMeasureAdmissionWaits == waitsBefore + 1;
                }))
                return;
            // A frame needs device memory: the service reads the shortfall the same way it reads
            // a frame's wait, and gives the tap (and with it the request) back first.
            rig.core->deviceReclaim->request(1);
            rig.core->notify();
            const auto outcome = take(measure, "the waiting measure whose tap was given back");
            checks.expect(outcome && outcome->status == GpuScopeTapStatus::Skipped &&
                              outcome->skip == GpuScopeTapSkip::NoRoom &&
                              counters().scopeViewerTapYields == 1 &&
                              !Access::scopeViewerTapHeld(*rig.core),
                          "(a2) a waiting request whose tap was given back ends Skipped(NoRoom), "
                          "not TapGone");
            const auto later = service.measureScopeTap(key, displayJob, none);
            checks.expect(later.status == GpuScopeTapStatus::Skipped &&
                              later.skip == GpuScopeTapSkip::NoRoom,
                          "(a2) a later measure for the given-back key is Skipped(NoRoom)");
            const auto unknown = service.measureScopeTap(GpuScopeTapKey::of(rig.identity(1, 99)),
                                                         displayJob, none);
            checks.expect(unknown.status == GpuScopeTapStatus::Skipped &&
                              unknown.skip == GpuScopeTapSkip::TapGone,
                          "(a2) a key no frame had is TapGone, not NoRoom");
        }
    }

    // ---- (b) the taps are given back first --------------------------------------------------
    // Without a scene cache the taps are the only holders of their pictures, so giving them back
    // alone makes the room and nothing else is reclaimed. With one (b2) the cache gives way only
    // after them: the tap's alias would otherwise keep a cached picture's bytes allocated.
    {
        const std::string name = "(b) ";
        LargeFrame large;
        if (!probeLargeFrame(true, derived->image, large))
            return;
        Rig rig(options, plan, processor, snapshot,
                {.card = 8 * derived->largeImage,
                 .viewerWorkingBytes = derived->image,
                 .sceneCacheBytes = std::optional<std::uint64_t>{0},
                 .idleTrimMilliseconds = kNoIdleTrim,
                 .largePlan = largePlan});
        auto& service = *rig.service;
        if (!rig.ready() || !deviceReported(rig, checks))
            return;
        const auto counters = [&] { return service.status().counters; };
        const auto viewer = rig.frame(1, 0, TaskPriority::Visible);
        const auto fill = rig.frame(2, 1, TaskPriority::Foreground, true);
        if (!viewer || !fill)
            return;
        checks.expect(Access::scopeViewerTapHeld(*rig.core) && Access::scopeFillTapHeld(*rig.core),
                      name + "the Viewer's tap and the fill tap are held");
        // The larger frame is admitted (the room left holds the need its envelope states) but it
        // takes more room than that while it runs, as measured on this device: the room left is
        // what it takes less half of what the taps hold, so it is short of room beside the taps
        // and has enough once the taps' pictures are given back. The root refuses the allocation
        // for room and the service gives way inside it, taps first.
        if (!awaitCondition("the frames' phases retire", [&] {
                return rig.policy->snapshot().reservations.admittedPhases == 0;
            }))
            return;
        const auto tapBytes = rig.policy->snapshot().reservations.retainedNativeImageBytes;
        const auto left = std::max(large.need, large.growth - std::min(large.growth, tapBytes / 2));
        std::string reuseBefore;
        const auto numbers = [&] {
            const auto c = counters();
            const auto r = rig.policy->snapshot().reservations;
            return " (room left for the frame " + text(left) + ", the room it took on the probe " +
                   text(large.growth) + ", its stated need " + text(large.need) +
                   ", the taps' bytes " + text(tapBytes) + ", picture allocation " +
                   text(derived->image) + ", larger picture allocation " +
                   text(derived->largeImage) + ", Viewer tap yields " +
                   text(c.scopeViewerTapYields) + ", fill tap yields " + text(c.scopeFillTapYields) +
                   ", capacity waits " + text(c.capacityWaits) + ", CPU fallbacks " +
                   text(c.cpuFallbacks) + ", device room reclaims " + text(c.deviceRoomReclaims) +
                   ", retained image bytes " + text(r.retainedNativeImageBytes) +
                   ", owned image bytes " + text(r.ownedNativeImageBytes) + ", graph jobs " +
                   text(c.residentGraphJobs) + ", peak claims " +
                   text(r.peakTotalDeviceClaimBytes) + "; storage reuse before the frame (" +
                   reuseBefore + "), after (" + reuseNumbers(rig) + "); on the probe before (" +
                   large.reuseBefore + "), after (" + large.reuseAfter + "))";
        };
        const bool fits = tapBytes >= 2 * derived->image && large.need <= left &&
                          large.growth > left && large.growth - left <= tapBytes;
        checks.expect(fits, name + "the card holds the larger frame beside the taps only once "
                                   "the taps are given back" + numbers());
        if (!fits)
            return;
        const bool shaped = leaveRoom(rig, left);
        checks.expect(shaped, name + "the books leave the room planned for the frame" + numbers());
        if (!shaped)
            return;
        const auto before = counters();
        reuseBefore = reuseNumbers(rig);
        const auto next = rig.frame(kLargeGeneration, 2, TaskPriority::Visible);
        checks.expect(next && isPacked(*next),
                      name + "the Viewer frame that needed the taps' bytes completes on the GPU" +
                          numbers());
        const auto after = counters();
        checks.expect(after.scopeViewerTapYields == 1 && after.scopeFillTapYields == 1 &&
                          !Access::scopeFillTapHeld(*rig.core),
                      name + "both taps were given back, one per slot" + numbers());
        checks.expect(after.scopeReclaimsSeenAtFirstGiveBack == 0 && after.idleTrims == 0 &&
                          service.status().residentFrameEvictionSerial == 0,
                      name + "the taps went first: no parked storage, scene cache or finished "
                             "frame had been reclaimed when they were given back, and the UI was "
                             "not asked to evict finished frames");
        checks.expect(after.cpuFallbacks == before.cpuFallbacks &&
                          after.scopeMeasuresGivenBack == 0 && after.scopeMeasuresRequested == 0,
                      name + "the frame took no CPU fallback and there was no measure to wait on");
        const auto gone = service.measureScopeTap(keyOf(*fill), displayJob, none);
        checks.expect(gone.status == GpuScopeTapStatus::Skipped &&
                          gone.skip == GpuScopeTapSkip::NoRoom,
                      name + "the given-back fill tap measures Skipped(NoRoom)");
    }

    {
        Rig rig(options, plan, processor, snapshot,
                {.card = 16 * derived->image, .viewerWorkingBytes = 4 * derived->image});
        auto& service = *rig.service;
        if (!rig.ready() || !deviceReported(rig, checks))
            return;
        const auto counters = [&] { return service.status().counters; };
        const auto viewer = rig.frame(1, 0, TaskPriority::Visible);
        if (!viewer)
            return;
        const auto retained = rig.policy->snapshot().reservations.retainedNativeImageBytes;
        checks.expect(retained >= 2 * derived->image && Access::scopeViewerTapHeld(*rig.core),
                      "(b2) the scene cache and the Viewer's tap hold the frame's pictures");
        // A frame is short of everything those pictures hold: the request a frame's wait raises.
        rig.core->deviceReclaim->request(retained);
        rig.core->notify();
        checks.expect(awaitCondition("the scene cache gives its pictures back", [&] {
                          return rig.policy->snapshot().reservations.retainedNativeImageBytes <
                                 retained;
                      }),
                      "(b2) the scene cache gave pictures back after the tap");
        const auto after = counters();
        checks.expect(after.scopeViewerTapYields == 1 && !Access::scopeViewerTapHeld(*rig.core) &&
                          after.scopeReclaimsSeenAtFirstGiveBack == 0,
                      "(b2) the tap was given back before the scene cache, parked storage or the "
                      "OCIO programs");
    }

    // ---- (c) a measure in flight never makes a Viewer frame wait ----------------------------
    {
        Rig rig(options, plan, processor, snapshot,
                {.card = 4 * derived->measureNeed, .viewerWorkingBytes = 4 * derived->image});
        auto& service = *rig.service;
        if (!rig.ready() || !deviceReported(rig, checks))
            return;
        const auto counters = [&] { return service.status().counters; };
        const auto first = rig.frame(1, 0, TaskPriority::Visible);
        if (!first)
            return;
        TableHold hold(rig.core);
        auto measure = measureAsync(service, keyOf(*first), displayJob);
        if (!hold.awaitHeld("the held measure is in flight"))
            return;
        const auto inFlight = rig.policy->snapshot().reservations;
        checks.expect(counters().scopeMeasureAdmissions == 1 && inFlight.admittedPhases == 1 &&
                          inFlight.ownedNativeBufferBytes >= derived->layoutBytes &&
                          inFlight.ownedNativeImageBytes >= 2 * derived->image,
                      "(c) the measure's color outputs and table buffers are tracked against its "
                      "one Background phase");
        const auto before = counters();
        for (std::uint64_t index = 0; index < 3; ++index) {
            const auto frame = rig.frame(20 + index, static_cast<std::int64_t>(20 + index),
                                         TaskPriority::Visible);
            checks.expect(frame && isPacked(*frame),
                          "(c) a Viewer frame completes while the measure is held in flight");
        }
        const auto during = counters();
        checks.expect(notReady(measure) && Access::scopeRequestPending(*rig.core) &&
                          during.scopeMeasurePhaseParks >= 1 &&
                          during.scopeMeasuresGivenBack == 0 &&
                          during.capacityWaits == before.capacityWaits &&
                          during.cpuFallbacks == before.cpuFallbacks &&
                          during.packedTransfers == before.packedTransfers + 3,
                      "(c) the frames parked the measure's phase and neither waited nor fell back; "
                      "the measure was only polled");
        hold.release();
        const auto outcome = take(measure, "the released measure");
        checks.expect(outcome && outcome->status == GpuScopeTapStatus::Measured &&
                          tablesWithinOneCell(outcome->counts, displayReference.counts),
                      "(c) the measure delivers the tables of the frame it counted");
        checks.expect(awaitCondition("the measure's memory and phase are retired", [&] {
                          const auto snap = rig.policy->snapshot().reservations;
                          // The cached color program keeps its small uniform buffer.
                          return snap.admittedPhases == 0 &&
                                 snap.ownedNativeBufferBytes < derived->layoutBytes;
                      }),
                      "(c) the measure's tracked memory and its phase are retired at its end");
    }
    {
        // A frame whose allocations are short of memory while the measure is in flight. The
        // measure is not the Viewer's to wait for: pressure tells it to stop and gives the tap
        // back, and it is polled to its end; its memory (and its alias of the tapped picture)
        // returns only once its fence is proven retired. A frame that still cannot get memory then
        // waits for memory like any other frame (a capacity wait, never the CPU); the memory the
        // measure was admitted beside (the Viewer's last need) is what keeps that case rare.
        // The short frame is a larger picture than the earlier frame (see `probeLargeFrame`).
        LargeFrame large;
        if (!probeLargeFrame(false, derived->image, large))
            return;
        Rig rig(options, plan, processor, snapshot,
                {.card = 4 * derived->measureNeed,
                 .viewerWorkingBytes = derived->image,
                 .sceneCacheBytes = std::optional<std::uint64_t>{0},
                 .idleTrimMilliseconds = kNoIdleTrim,
                 .largePlan = largePlan});
        auto& service = *rig.service;
        if (!rig.ready() || !deviceReported(rig, checks))
            return;
        const auto counters = [&] { return service.status().counters; };
        const auto first = rig.frame(1, 0, TaskPriority::Visible);
        if (!first)
            return;
        TableHold hold(rig.core);
        auto measure = measureAsync(service, keyOf(*first), displayJob);
        if (!hold.awaitHeld("the held measure is in flight"))
            return;
        // The frame's stated need fits; it takes more room than that while it runs, as measured
        // on this device, so it is short of the room the test leaves beside the tap and the
        // measure and has enough once the tap's picture is given back (the room left is what the
        // frame takes less half of the tap's bytes).
        const auto tapBytes = rig.policy->snapshot().reservations.retainedNativeImageBytes;
        const auto left = std::max(large.need, large.growth - std::min(large.growth, tapBytes / 2));
        const bool fits = tapBytes >= derived->image && large.need <= left &&
                          large.growth > left && large.growth - left <= tapBytes;
        checks.expect(fits, "(c2) the card holds the larger frame beside the measure only once the "
                            "tap is given back (room left for the frame " + text(left) +
                                ", the room it took on the probe " + text(large.growth) +
                                ", its stated need " + text(large.need) + ", the tap's bytes " +
                                text(tapBytes) + ", measure need " + text(derived->measureNeed) +
                                ")");
        if (!fits) {
            hold.release();
            return;
        }
        if (!leaveRoom(rig, left)) {
            checks.expect(false, "(c2) the books leave the room planned for the frame (" +
                                     text(left) + ")");
            hold.release();
            return;
        }
        const auto before = counters();
        const auto reuseBefore = reuseNumbers(rig);
        auto submission = rig.submit(rig.identity(kLargeGeneration, 30), TaskPriority::Visible);
        if (!awaitCondition("the measure is told to stop for the frame", [&] {
                return counters().scopeMeasuresGivenBack == 1 && Access::scopeStopIssued(*rig.core);
            })) {
            const auto c = counters();
            checks.expect(false, "(c2) the measure was not told to stop (room left for the frame " +
                                     text(left) + ", the room it took on the probe " +
                                     text(large.growth) +
                                     ", its stated need " + text(large.need) +
                                     ", measure need " + text(derived->measureNeed) +
                                     ", given back " + text(c.scopeMeasuresGivenBack) +
                                     ", Viewer tap yields " + text(c.scopeViewerTapYields) +
                                     ", capacity waits " + text(c.capacityWaits) +
                                     ", CPU fallbacks " + text(c.cpuFallbacks) +
                                     ", device room reclaims " + text(c.deviceRoomReclaims) +
                                     ", retained image bytes " +
                                     text(rig.policy->snapshot().reservations
                                              .retainedNativeImageBytes) +
                                     ", peak claims " +
                                     text(rig.policy->snapshot().reservations
                                              .peakTotalDeviceClaimBytes) +
                                     "; storage reuse before the frame (" + reuseBefore +
                                     "), after (" + reuseNumbers(rig) + "); on the probe before (" +
                                     large.reuseBefore + "), after (" + large.reuseAfter + "))");
            hold.release();
            return;
        }
        // The frame completes while the readback is still held: it never waits for the measure.
        const auto done = rig.finish(submission, "the short frame");
        checks.expect(done && isPacked(*done) && counters().cpuFallbacks == before.cpuFallbacks &&
                          counters().capacityWaits == before.capacityWaits,
                      "(c2) the short frame completes on the GPU with the readback still held: it "
                      "neither waits for the measure nor falls back");
        checks.expect(notReady(measure) && Access::scopeRequestPending(*rig.core) &&
                          counters().scopeViewerTapYields == 1 &&
                          counters().scopeReclaimsSeenAtFirstGiveBack == 0,
                      "(c2) the measure in flight was told to stop and is polled to its end, its "
                      "memory not freed before its fence is proven; the tap went first");
        hold.release();
        const auto outcome = take(measure, "the stopped measure");
        checks.expect(outcome && outcome->status == GpuScopeTapStatus::Skipped &&
                          outcome->skip == GpuScopeTapSkip::NoRoom,
                      "(c2) the stopped measure ends Skipped(NoRoom)");
        checks.expect(awaitCondition("the measure's phase retires", [&] {
                          return rig.policy->snapshot().reservations.admittedPhases == 0;
                      }),
                      "(c2) the stopped measure's phase is retired once its fence is proven");
    }

    // ---- (d) a fill tap that does not fit as Background is refused --------------------------
    {
        Rig rig(options, plan, processor, snapshot,
                {.card = 16 * derived->image, .viewerWorkingBytes = 4 * derived->image});
        auto& service = *rig.service;
        if (!rig.ready() || !deviceReported(rig, checks))
            return;
        const auto counters = [&] { return service.status().counters; };
        const auto viewer = rig.frame(1, 0, TaskPriority::Visible);
        if (!viewer)
            return;
        // A flagged fill frame held at its transfer; the Viewer then holds nearly all the room
        // that is free (a Viewer-class request admitted with a claim, which records its need): the
        // frame's tap, one picture, cannot be held beside it as Background when it publishes.
        const auto fillFrame = [&](const std::uint64_t generation,
                                   const std::int64_t time) -> std::shared_ptr<const bloom::runtime::PreparedPreviewFrame> {
            TransferHold transfer(rig.core);
            auto submission = rig.submit(rig.identity(generation, time), TaskPriority::Foreground, true);
            if (!awaitCondition("the fill frame reaches its held transfer", [&] {
                    return Access::residentActivePhase(*rig.core) ==
                           bloom::runtime::detail::ResidentNativePhase::Transfer;
                }))
                return {};
            const auto free = rig.policy->usableCapacity().deviceFreeBytes;
            const auto need = holdRoom(*rig.policy, free - derived->image / 2);
            if (!need.token)
                return {};
            transfer.release();
            return rig.finish(submission, "the flagged fill frame");
        };
        const auto base = counters();
        const auto first = fillFrame(2, 1);
        checks.expect(first && isPacked(*first),
                      "(d) the flagged fill frame completes and is delivered");
        const auto second = fillFrame(3, 2);
        const auto after = counters();
        checks.expect(second && isPacked(*second) &&
                          after.scopeFillStageWaits == base.scopeFillStageWaits,
                      "(d) a second flagged fill frame is not held back");
        checks.expect(after.scopeFillTapsRefused == base.scopeFillTapsRefused + 2 &&
                          after.scopeFillTapsKept == base.scopeFillTapsKept &&
                          !Access::scopeFillTapHeld(*rig.core) &&
                          Access::scopeViewerTapHeld(*rig.core) &&
                          after.scopeTapsKept == base.scopeTapsKept,
                      "(d) both refusals are counted, the fill slot stays free and the Viewer's "
                      "tap is untouched");
        if (first) {
            const auto fallback = service.measureScopeTap(keyOf(*first), displayJob, none);
            checks.expect(fallback.status == GpuScopeTapStatus::Skipped &&
                              fallback.skip == GpuScopeTapSkip::NoRoom &&
                              counters().scopeMeasuresNoRoom == base.scopeMeasuresNoRoom + 1,
                          "(d) the refused frame's key measures Skipped(NoRoom): the UI measures "
                          "its retained picture");
        }
        checks.expect(after.cpuFallbacks == base.cpuFallbacks &&
                          after.scopeFillTapYields == 0 && after.scopeViewerTapYields == 0,
                      "(d) the fill frames took the GPU route and nothing was given back");
    }

    // ---- (e) the idle trim releases the Viewer tap ------------------------------------------
    {
        Rig rig(options, plan, processor, snapshot,
                {.card = 0, .viewerWorkingBytes = 4 * derived->image, .idleTrimMilliseconds = 50});
        auto& service = *rig.service;
        if (!rig.ready())
            return;
        const auto counters = [&] { return service.status().counters; };
        const auto first = rig.frame(1, 0, TaskPriority::Visible);
        if (!first)
            return;
        const auto key = keyOf(*first);
        const auto settings = scope.settings(ScopeSignal::Working, kNoChromaticity);
        const auto job = scope.job(settings);
        checks.expect(Access::scopeViewerTapHeld(*rig.core), "(e) the shown frame is tapped");
        if (!awaitCondition("the idle service trims its retention", [&] {
                rig.core->notify();
                return counters().scopeTapIdleReleases == 1 && counters().idleTrims >= 1;
            }))
            return;
        checks.expect(!Access::scopeViewerTapHeld(*rig.core) && counters().idleTrims >= 1 &&
                          counters().scopeViewerTapYields == 0,
                      "(e) the idle trim released the Viewer tap, and it is not memory pressure");
        const auto skipped = service.measureScopeTap(key, job, none);
        checks.expect(skipped.status == GpuScopeTapStatus::Skipped &&
                          skipped.skip == GpuScopeTapSkip::NoRoom,
                      "(e) a later measure for that key gives a Skipped result");
        const auto tapsBefore = counters().scopeTapsKept;
        const auto next = rig.frame(2, 1, TaskPriority::Visible);
        checks.expect(next && counters().scopeTapsKept == tapsBefore + 1 &&
                          Access::scopeViewerTapHeld(*rig.core),
                      "(e) the next shown frame is tapped again");
        if (next) {
            const auto measured = service.measureScopeTap(keyOf(*next), job, none);
            checks.expect(measured.status == GpuScopeTapStatus::Measured,
                          "(e) and measures");
        }
    }
}

int runAll(int argc, char** argv) {
    const auto options = parseOptions(argc, argv);
    if (!options.valid)
        return 2;
    auto resolution = bloom::color::resolveBloomNeutralV1BuiltIn(
        bloom::color::OcioConfigLocatorKind::BloomBuiltIn, bloom::color::kBloomNeutralV1ConfigUri,
        bloom::color::kBloomNeutralV1ConfigDigest);
    auto neutral = std::move(resolution).takeResolved();
    if (!neutral)
        return 1;
    auto processor = makeNeutralProcessor(*neutral);
    if (!processor)
        return 1;

    GpuOcioContextRequest ocioRequest;
    ocioRequest.applicationExecutable = std::filesystem::path{BLOOM_SCOPE_TAP_TEST_EXECUTABLE};
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
    GpuOcioContextResolver resolver(std::move(ocioRequest));
    const auto resolved = resolver.resolve();
    if (!resolved.hasValue()) {
        std::cerr << "the packaged tools do not resolve to a shared OCIO context: "
                  << resolved.diagnostic << '\n';
        return 1;
    }

    const auto plan = makePlan(kSize, kSize);
    bloom::document::Project project(kProjectId, "scope-tap");
    const auto seed = bloom::document::makeNewProject("Fixture", "Main",
                                                      RationalTime::fromInteger(4), plan->format());
    if (!project.addComposition(bloom::document::Composition(
            plan->compositionId(), "Main", RationalTime::fromInteger(4),
            seed.project.findComposition(seed.initialCompositionId)->graph(), plan->format())))
        return 1;
    bloom::document::Document document(std::move(project));

    // The scope signals' color objects and the CPU reference's frame of the plan.
    auto config = bloom::color::resolveOcioBuiltIn(
                      bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                      bloom::color::kBloomNeutralV1ConfigUri,
                      bloom::color::ocioBuiltInContentRevision(
                          bloom::color::OcioConfigLocatorKind::BloomBuiltIn,
                          bloom::color::kBloomNeutralV1ConfigUri)
                          .value_or(bloom::color::kBloomNeutralV1ConfigDigest),
                      "lin_rec709_scene")
                      .takeResolved();
    if (!config)
        return 1;
    const auto sharedConfig =
        std::make_shared<const bloom::color::ResolvedBloomNeutralConfig>(std::move(*config));
    Scope scope;
    for (const auto signal : {ScopeSignal::Working, ScopeSignal::Display}) {
        auto prepared = ScopeSignalColor::prepare(
            {.config = sharedConfig, .signal = signal, .display = {}, .view = {}, .viewAdjust = {}});
        if (!prepared) {
            std::cerr << "the scope signal's color objects did not prepare: " << prepared.message
                      << '\n';
            return 1;
        }
        (signal == ScopeSignal::Working ? scope.working : scope.display) = prepared.color;
    }
    scope.ocio = resolved.context;
    {
        CpuCompositionEvaluator evaluator;
        const auto identity = makeIdentity(*plan, 1, {}, {}, {});
        auto evaluated = evaluator.evaluate(plan,
                                            {.time = identity.time,
                                             .output = plan->output(),
                                             .resolution = identity.resolution,
                                             .quality = identity.quality,
                                             .colorIntent = identity.colorIntent,
                                             .pixelStorageByteLimit = kBudget,
                                             .roi = identity.roi,
                                             .bypassLookNodes = !identity.showLook},
                                            {});
        if (evaluated.frame() == nullptr) {
            std::cerr << "the CPU reference did not evaluate the plan\n";
            return 1;
        }
        scope.cpuFrame = evaluated.frame();
    }

    Expectations checks;
    if (!run(checks, options, plan, processor, document.snapshot(), scope))
        return options.requireDevice ? 1 : kSkipExit;
    shutdownCases(checks, options, plan, processor, document.snapshot(), scope);
    pressureCases(checks, options, plan, processor, document.snapshot(), scope);
    if (checks.failures() + bloom::test_support::awaitFailureCount() != 0)
        return 1;
    std::cout << "PASS: scope tap and device scope measure on the preview display service\n";
    return 0;
}
#else
int runAll(int argc, char** argv) {
    const auto options = parseOptions(argc, argv);
    std::cout << "Unsupported: genuine GPU preparation tools unavailable\n";
    return options.requireDevice ? 1 : kSkipExit;
}
#endif
} // namespace

int main(int argc, char** argv) {
    try {
        return runAll(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
