// The Display signal tables of the frame the Viewer shows, counted on the service's device at
// display time (#733). See gpu_preview_display_service_shown_scopes_private.hpp.

#include "gpu_preview_display_service_private.hpp"

#include <bloom/render/gpu_scope_tables.hpp>

#include <limits>
#include <utility>

namespace bloom::runtime::detail {
namespace {

// Measures that may wait for the owner at one time. A caller keeps one measure in flight, so more
// than a few waiting means several callers; the rest are dropped and counted on a worker.
constexpr std::size_t kShownScopePendingLimit = 4;
// Owner turns a measure that was told to stop (the service is stopping, or its deadline passed)
// may take to prove its submission retired, as a frame's retirement has
// (kResidentRetirementPumpBudget).
constexpr std::size_t kShownScopeStopTurns = 512;
// Room on the device for the CIE XYZ color program and its tables.
constexpr std::uint64_t kShownScopeColorProgramBytes = 32ULL * 1024 * 1024;
constexpr std::uint64_t kShownScopePaddingBytes = 1024ULL * 1024;

[[nodiscard]] std::chrono::steady_clock::time_point
shownScopeNow(const PreviewDisplayServiceCore& core) {
    return core.nativeClockOverride ? core.nativeClockOverride() : std::chrono::steady_clock::now();
}

// True while a frame has native work on the device or is about to.
[[nodiscard]] bool framesUseDevice(const PreviewDisplayServiceCore& core) noexcept {
    return !core.residentNativeReady.empty() || core.residentNativeInFlight ||
           core.residentAhead != nullptr || !core.nativeReady.empty() || core.nativeInFlight;
}

void complete(const std::shared_ptr<ShownFrameScopeJob>& job, ShownScopeJobOutcome outcome) noexcept {
    if (job == nullptr)
        return;
    auto done = std::move(job->done);
    job->done = {};
    try {
        if (done)
            done(std::move(outcome));
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // A caller's fault must not reach the owner loop.
    }
}

[[nodiscard]] ShownScopeJobOutcome outcomeOf(const ShownScopeJobStatus status, std::string message) {
    ShownScopeJobOutcome outcome;
    outcome.status = status;
    outcome.message = std::move(message);
    return outcome;
}

void drop(PreviewDisplayServiceCore& core, const std::shared_ptr<ShownFrameScopeJob>& job,
          std::string message) noexcept {
    core.shownScopes.dropped.fetch_add(1, std::memory_order_relaxed);
    try {
        complete(job, outcomeOf(ShownScopeJobStatus::Dropped, std::move(message)));
    } catch (...) {
        complete(job, {});
    }
}

// Makes the measure's phase the device's retained phase for one step of the measure. Returns the
// phase that was retained before (a frame's), to be selected again after the step.
[[nodiscard]] std::shared_ptr<render::GpuProcessingPhaseLease>
selectMeasurePhase(PreviewDisplayServiceCore& core) noexcept {
    auto& state = core.shownScopes;
    if (!state.phaseArmed || !core.device || state.active == nullptr || !state.active->phase)
        return nullptr;
    auto previous = core.device->retainedProcessingPhase();
    if (previous && previous->id() == state.active->phase->id())
        return nullptr;
    static_cast<void>(core.device->selectProcessingPhase(state.active->phase->id()));
    return previous;
}

void restorePhase(PreviewDisplayServiceCore& core,
                  const std::shared_ptr<render::GpuProcessingPhaseLease>& previous) noexcept {
    if (previous && core.device)
        static_cast<void>(core.device->selectProcessingPhase(previous->id()));
}

// Ends the measure in flight. Nothing of it is in flight any more when `proven`; otherwise its
// phase stays latched on the device until the device proves it idle, as a frame's does.
void finishActive(PreviewDisplayServiceCore& core, ShownScopeJobOutcome outcome,
                  const bool proven) noexcept {
    auto& state = core.shownScopes;
    auto job = std::move(state.active);
    state.active.reset();
    // The stepper goes before the work it reads; its pictures go with it.
    state.stepper.reset();
    if (state.upload != nullptr && proven) {
        // A cancelled upload that retired holds a picture nothing will take.
        if (state.upload->state() == render::GpuImageUploadJobState::Ready)
            static_cast<void>(state.upload->takeImage());
    }
    if (state.phaseArmed && core.device && job != nullptr && job->phase) {
        const auto previous = core.device->retainedProcessingPhase();
        const bool other = previous && previous->id() != job->phase->id();
        static_cast<void>(core.device->selectProcessingPhase(job->phase->id()));
        core.device->retireProcessingPhase(job->phase->id(), proven);
        if (other)
            static_cast<void>(core.device->selectProcessingPhase(previous->id()));
    }
    state.phaseArmed = false;
    state.cancelIssued = false;
    state.stopTurns = 0;
    state.phase = ShownScopeState::Phase::Upload;
    ++state.progress;
    switch (outcome.status) {
    case ShownScopeJobStatus::Measured:
        state.measured.fetch_add(1, std::memory_order_relaxed);
        state.colorDispatches.fetch_add(outcome.colorDispatches, std::memory_order_relaxed);
        state.tableReadbacks.fetch_add(outcome.tableReadbacks, std::memory_order_relaxed);
        state.tableReadbackBytes.store(outcome.tableReadbackBytes, std::memory_order_relaxed);
        break;
    case ShownScopeJobStatus::Dropped:
        state.dropped.fetch_add(1, std::memory_order_relaxed);
        break;
    case ShownScopeJobStatus::Cancelled:
        break;
    case ShownScopeJobStatus::Failed:
        state.failed.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    // The job's cells and its phase are released before its caller hears of the result.
    auto done = job != nullptr ? std::move(job->done) : std::function<void(ShownScopeJobOutcome)>{};
    job.reset();
    try {
        if (done)
            done(std::move(outcome));
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // A caller's fault must not reach the owner loop.
    }
}

// A failure of the measure in flight: no room on the device is a drop (the caller counts on a
// worker), anything else a failure with its reason.
[[nodiscard]] ShownScopeJobOutcome failureOutcome(const bool noRoom, const bool cancelled,
                                                  std::string message) {
    if (cancelled)
        return outcomeOf(ShownScopeJobStatus::Cancelled, std::move(message));
    return outcomeOf(noRoom ? ShownScopeJobStatus::Dropped : ShownScopeJobStatus::Failed,
                     std::move(message));
}

// Takes `job` up: its phase, the upload of its cells. False when it ended instead (dropped).
[[nodiscard]] bool begin(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                         std::shared_ptr<ShownFrameScopeJob> job) {
    auto& state = core->shownScopes;
    if (!core->device || job->cells.empty() || !job->descriptor) {
        drop(*core, job, "The device is not available to count the shown frame.");
        return false;
    }
    if (framesUseDevice(*core) || core->device->processingPhaseOccupied()) {
        drop(*core, job, "The device is preparing frames.");
        return false;
    }
    if (core->options.processingPhasePolicy && !job->phase) {
        drop(*core, job, "The device has no room for the scope measure now.");
        return false;
    }
    if (state.stage == nullptr) {
        std::string message;
        state.stage = ScopeMeasureStage::create(*core->device, message);
        if (state.stage == nullptr) {
            state.failed.fetch_add(1, std::memory_order_relaxed);
            complete(job, outcomeOf(ShownScopeJobStatus::Failed,
                                    message.empty() ? std::string("The device cannot count the "
                                                                  "scopes.")
                                                    : std::move(message)));
            return false;
        }
    }
    if (state.upload == nullptr) {
        auto created = render::GpuImageUpload::create(
            *core->device, {.maxImageBytes = std::numeric_limits<std::uint64_t>::max(),
                            .maxStagingBytes = std::numeric_limits<std::uint64_t>::max()});
        if (!created) {
            state.failed.fetch_add(1, std::memory_order_relaxed);
            complete(job, outcomeOf(ShownScopeJobStatus::Failed,
                                    std::move(created.diagnostic.message)));
            return false;
        }
        state.upload = std::move(created.upload);
    }
    if (job->phase && !core->device->retainProcessingPhase(job->phase)) {
        drop(*core, job, "The device holds another processing phase.");
        return false;
    }
    state.phaseArmed = static_cast<bool>(job->phase);
    state.active = std::move(job);
    state.phase = ShownScopeState::Phase::Upload;
    state.cancelIssued = false;
    state.stopTurns = 0;
    state.startedAt = shownScopeNow(*core);
    ++state.progress;

    const auto& active = *state.active;
    render::GpuImageUploadParameters parameters;
    parameters.phase = active.phase;
    parameters.descriptorOverride = active.descriptor;
    parameters.displayCells =
        render::GpuUploadDisplayCells{active.owner, active.cells, active.layout};
    const std::uint64_t pixels = active.cells.size();
    // The staging copy and the unpacked picture, with room for the allocator's rounding.
    const std::uint64_t budget = 2 * (pixels * 4 + pixels * 16) + kShownScopePaddingBytes;
    const auto begun = state.upload->begin(parameters, budget);
    if (begun.code != render::GpuImageUploadDiagnosticCode::None) {
        const bool noRoom = begun.code == render::GpuImageUploadDiagnosticCode::OverBudget ||
                            begun.code == render::GpuImageUploadDiagnosticCode::AllocationFailed ||
                            begun.code == render::GpuImageUploadDiagnosticCode::Busy;
        finishActive(*core, failureOutcome(noRoom, false, begun.message),
                     !state.upload->hasUnretiredSubmission());
        return false;
    }
    state.uploads.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void stepUpload(const std::shared_ptr<PreviewDisplayServiceCore>& core, const bool stop,
                const bool deadlineExpired) {
    auto& state = core->shownScopes;
    auto& upload = *state.upload;
    if ((stop || deadlineExpired) && !state.cancelIssued) {
        state.cancelIssued = true;
        upload.cancel();
    }
    const auto polled = state.uploadPollOverride && !state.cancelIssued
                            ? state.uploadPollOverride(upload)
                            : upload.poll();
    if (polled == render::GpuImageUploadPollResult::Pending)
        return;
    if (polled != render::GpuImageUploadPollResult::Ready) {
        if (upload.hasUnretiredSubmission())
            return; // Not proven retired: polled again next turn.
        const auto code = upload.diagnostic().code;
        const bool noRoom = code == render::GpuImageUploadDiagnosticCode::OverBudget ||
                            code == render::GpuImageUploadDiagnosticCode::AllocationFailed;
        finishActive(*core,
                     failureOutcome(noRoom, stop,
                                    deadlineExpired && !stop
                                        ? std::string("The scope measure's upload did not retire "
                                                      "within the deadline.")
                                        : upload.diagnostic().message),
                     true);
        return;
    }
    if (state.cancelIssued) {
        finishActive(*core,
                     failureOutcome(false, stop,
                                    "The scope measure's upload did not retire within the "
                                    "deadline."),
                     true);
        return;
    }
    auto picture = std::make_shared<const render::GpuImage>(upload.takeImage());
    const std::uint64_t pixels = state.active->cells.size();
    const std::uint64_t budget = scopeMeasureTableBytes(state.active->work.job) +
                                 2 * pixels * 16 + kShownScopeColorProgramBytes;
    const auto allowance = core->previewByteAllowance();
    state.stepper = std::make_unique<ScopeMeasureStepper>(
        *state.stage, state.active->work, std::move(picture),
        allowance != 0 ? std::max<std::uint64_t>(allowance, budget) : budget);
    state.phase = ShownScopeState::Phase::Count;
    ++state.progress;
}

void stepCount(const std::shared_ptr<PreviewDisplayServiceCore>& core, const bool stop,
               const bool deadlineExpired) {
    auto& state = core->shownScopes;
    ScopeMeasureStepper::Hooks hooks;
    // The kernel's table slot is one per process: a frame's count or a headless read may hold it,
    // and the measure waits for it (polled, never waited on).
    hooks.mayBegin = [] { return render::GpuScopeTables::processSlotFree(); };
    const bool inFlightBefore = state.stepper->submissionInFlight();
    const auto stepped = state.stepper->step(stop, deadlineExpired, hooks);
    if (stepped != ScopeMeasureStepper::State::Settled) {
        if (state.stepper->submissionInFlight() != inFlightBefore)
            ++state.progress;
        return;
    }
    const auto& fault = state.stepper->fault();
    auto& work = state.active->work;
    const bool proven = state.stepper->retirementProven();
    if (fault.status == GpuProcessFrameStatus::Evaluated && work.counts) {
        ShownScopeJobOutcome outcome;
        outcome.status = ShownScopeJobStatus::Measured;
        outcome.counts = std::move(work.counts);
        outcome.uploads = 1;
        outcome.colorDispatches = work.colorDispatches;
        outcome.tableReadbacks = work.tableReadbacks;
        outcome.tableReadbackBytes = work.tableReadbackBytes;
        finishActive(*core, std::move(outcome), proven);
        return;
    }
    const bool noRoom = fault.code == GpuProcessFrameDiagnosticCode::OverBudget ||
                        fault.code == GpuProcessFrameDiagnosticCode::AwaitingCapacity ||
                        fault.code == GpuProcessFrameDiagnosticCode::BudgetRefused;
    auto outcome = failureOutcome(noRoom, fault.status == GpuProcessFrameStatus::Cancelled,
                                  fault.message.empty()
                                      ? std::string("The device could not count the scopes.")
                                      : fault.message);
    outcome.uploads = 1;
    finishActive(*core, std::move(outcome), proven);
}

void stepActive(const std::shared_ptr<PreviewDisplayServiceCore>& core, const bool stop) {
    auto& state = core->shownScopes;
    const bool deadlineExpired =
        shownScopeNow(*core) - state.startedAt >= core->options.nativeDispatchDeadline;
    const auto previous = selectMeasurePhase(*core);
    try {
        if (state.phase == ShownScopeState::Phase::Upload)
            stepUpload(core, stop, deadlineExpired);
        else
            stepCount(core, stop, deadlineExpired);
    } catch (...) {
        // Nothing of the measure is known to be retired: its phase stays latched.
        finishActive(*core, outcomeOf(ShownScopeJobStatus::Failed, "The scope measure raised."),
                     false);
    }
    if (state.active != nullptr && (stop || deadlineExpired) &&
        ++state.stopTurns > kShownScopeStopTurns) {
        // The submission could not be proven retired: its phase stays latched and the native
        // objects quarantine what they still own when they are destroyed.
        core->counterRetirementUnprovenTeardowns.fetch_add(1, std::memory_order_relaxed);
        finishActive(*core,
                     failureOutcome(false, stop,
                                    "The scope measure did not retire within its deadline."),
                     false);
    }
    restorePhase(*core, previous);
}

[[nodiscard]] std::shared_ptr<ShownFrameScopeJob> takePending(ShownScopeState& state) {
    const std::lock_guard lock(state.mutex);
    if (state.pending.empty())
        return nullptr;
    auto job = std::move(state.pending.front());
    state.pending.pop_front();
    state.hasPending.store(!state.pending.empty(), std::memory_order_release);
    return job;
}

} // namespace

std::shared_ptr<render::GpuProcessingPhaseLease>
reserveShownFrameScopePhase(const PreviewDisplayServiceCore& core, const std::uint64_t pixels,
                            const std::uint64_t tableBytes, const bool colorStep) noexcept {
    if (!core.options.processingPhasePolicy || pixels == 0)
        return nullptr;
    // The cells' staging copy is this measure's only source. The device holds the unpacked
    // picture (and the XYZ picture beside it), the tables and their staging copy.
    const std::uint64_t pictures = (colorStep ? 2U : 1U) * pixels * 16;
    auto grant = core.options.processingPhasePolicy->reserve(
        {.output = GpuProcessingPhaseOutput::PackedPreview,
         .pixels = pixels,
         .metadataBytes = tableBytes + kShownScopePaddingBytes,
         .requestedSourceBytes = pixels * 4,
         .minimumSourceBytes = pixels * 4,
         .requestedExecutorBytes = pictures,
         .minimumExecutorBytes = pictures,
         .deviceWorkingBytes = pixels * 4 + tableBytes +
                               (colorStep ? kShownScopeColorProgramBytes : 0) +
                               kShownScopePaddingBytes,
         .admissionClass = GpuAdmissionClass::Background});
    return std::move(grant.lease);
}

bool postShownFrameScopes(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                          std::shared_ptr<ShownFrameScopeJob> job) noexcept {
    if (core == nullptr || job == nullptr)
        return false;
    auto& state = core->shownScopes;
    std::shared_ptr<ShownFrameScopeJob> overflow;
    {
        const std::lock_guard lock(state.mutex);
        // Checked under the mailbox lock: the drain takes it before it empties the mailbox, so a
        // job is either refused here or ended by the drain.
        if (core->stopping.load(std::memory_order_acquire) ||
            core->shutdownRequested.load(std::memory_order_acquire))
            return false;
        try {
            if (state.pending.size() >= kShownScopePendingLimit) {
                overflow = std::move(state.pending.front());
                state.pending.pop_front();
            }
            state.pending.push_back(std::move(job));
        } catch (...) {
            return false;
        }
        state.hasPending.store(true, std::memory_order_release);
    }
    if (overflow != nullptr)
        drop(*core, overflow, "A newer scope measure took this one's place.");
    core->notify();
    return true;
}

bool shownFrameScopesActive(const PreviewDisplayServiceCore& core) noexcept {
    return core.shownScopes.active != nullptr;
}

void processShownFrameScopes(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    auto& state = core->shownScopes;
    try {
        if (state.active != nullptr) {
            stepActive(core, core->stopping.load(std::memory_order_acquire));
            return;
        }
        if (!state.hasPending.load(std::memory_order_acquire))
            return;
        if (auto job = takePending(state))
            static_cast<void>(begin(core, std::move(job)));
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // A single turn's fault is contained like the rest of the owner loop's.
    }
}

void shownFrameScopesGiveWay(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    auto& state = core->shownScopes;
    if (state.active == nullptr)
        return;
    state.framesStartedDuringMeasure.fetch_add(1, std::memory_order_relaxed);
    if (!state.phaseArmed || !core->device || !state.active->phase)
        return;
    const auto retained = core->device->retainedProcessingPhase();
    if (retained && retained->id() == state.active->phase->id())
        static_cast<void>(core->device->parkProcessingPhase());
}

void releaseShownFrameScopesIdle(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    auto& state = core->shownScopes;
    if (state.active != nullptr)
        return;
    if (state.upload != nullptr && !state.upload->hasUnretiredSubmission())
        state.upload.reset();
    if (state.stage != nullptr && !state.stage->hasUnretiredSubmission())
        state.stage.reset();
}

bool drainShownFrameScopes(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    auto& state = core->shownScopes;
    try {
        std::deque<std::shared_ptr<ShownFrameScopeJob>> waiting;
        {
            const std::lock_guard lock(state.mutex);
            waiting.swap(state.pending);
            state.hasPending.store(false, std::memory_order_release);
        }
        for (const auto& job : waiting)
            complete(job, outcomeOf(ShownScopeJobStatus::Cancelled, "The service is stopping."));
        if (state.active == nullptr)
            return false;
        stepActive(core, true);
    } catch (...) { // NOLINT(bugprone-empty-catch)
    }
    return state.active != nullptr;
}

void retireShownFrameScopesNative(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    auto& state = core->shownScopes;
    if (state.active != nullptr)
        finishActive(*core, outcomeOf(ShownScopeJobStatus::Cancelled, "The service is stopping."),
                     false);
    state.stepper.reset();
    state.upload.reset();
    // The color step executor and the table pipeline are destroyed on the owner thread; their
    // destructors drain what is unretired.
    state.stage.reset();
}

} // namespace bloom::runtime::detail
