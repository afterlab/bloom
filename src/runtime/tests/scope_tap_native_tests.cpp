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
//       caller.
//
// Requires the Vulkan backend and the pinned shader tools; skips (77) without a device and fails
// under --require-device.

#include "gpu_preview_display_service_packed_support.hpp"
#include "scope_tables_compare_support.hpp"

#include "await.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/document/new_project.hpp>
#include <bloom/render/gpu_scope_tables.hpp>
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

// The service, its scheduler and the counting scene preparation, torn down in reverse order.
struct Rig final {
    PackedRetirementScope retirement;
    TaskScheduler scheduler;
    std::shared_ptr<bloom::runtime::GpuProcessingPhasePolicy> policy;
    bloom::runtime::MemoryBudgetLedger ledger;
    std::shared_ptr<std::atomic<std::uint64_t>> preparations =
        std::make_shared<std::atomic<std::uint64_t>>(0);
    std::unique_ptr<GpuPreviewDisplayService> service;
    std::shared_ptr<bloom::runtime::detail::PreviewDisplayServiceCore> core;
    std::shared_ptr<const bloom::runtime::CompiledCompositionPlan> plan;
    bloom::document::Snapshot snapshot;
    std::atomic<std::uint64_t> nextOwner{100};

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
        bloom::document::Snapshot snapshotValue)
        : scheduler(config()),
          policy(std::make_shared<bloom::runtime::GpuProcessingPhasePolicy>(32, retirement.owner)),
          ledger(makeLedger()), plan(std::move(planValue)), snapshot(std::move(snapshotValue)) {
        ledger.beginProcessSampling();
        refresh();
        const bloom::runtime::GpuScenePreparationPhase phase{
            .policy = policy,
            .envelope = {.metadataBytes = 1024ULL * 1024,
                         .retainedCoverageByteLimit = 16ULL * 1024 * 1024,
                         .requestedExecutorBytes = kBudget,
                         .deviceWorkingBytes = 832ULL * 1024 * 1024}};
        auto builder =
            std::make_shared<CpuGpuSceneBuilder>(nullptr, bloom::runtime::GpuSceneMediaContext{},
                                                 bloom::runtime::GpuSceneOcioContext{}, phase);
        auto baseStage = packedStageFunction(plan, builder, {}, processor);
        auto count = preparations;
        PreviewGpuSceneStageFunction counted =
            [baseStage, count](
                const bloom::document::Snapshot& snap, const PreviewRequestIdentity& identity,
                const std::size_t bytes,
                const std::vector<bloom::runtime::SnapshotParameterOverride>& overrides,
                TaskContext& context) {
                count->fetch_add(1);
                return baseStage(snap, identity, bytes, overrides, context);
            };
        auto configured = packedOptions(options);
        configured.processingPhasePolicy = policy;
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

    explicit TableHold(std::shared_ptr<bloom::runtime::detail::PreviewDisplayServiceCore> target)
        : core(std::move(target)) {
        core->scopeTablesPollOverride = [held = held](bloom::render::GpuScopeTables& tables) {
            return held->load() ? bloom::render::GpuScopeTablesState::Pending : tables.poll();
        };
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
        if (!awaitCondition("the held measure is in flight",
                            [&] { return Access::scopeStepperBegan(*rig.core); }))
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
            if (!awaitCondition("the measure to hold is in flight",
                                [&] { return Access::scopeStepperBegan(*rig.core); }))
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
        if (!task.accepted() ||
            !awaitCondition("the measure to cancel is in flight",
                            [&] { return Access::scopeStepperBegan(*rig.core); }))
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
            if (!awaitCondition("the measure is in flight",
                                [&] { return Access::scopeStepperBegan(*rig.core); }))
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
