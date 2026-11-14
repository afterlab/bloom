// The scope tap and the device scope measure of the preview display service (#724). See
// gpu_preview_display_service_scope_private.hpp for the design and the thread rules.

#include "gpu_preview_display_service_private.hpp"

#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/render/gpu_scope_tables.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>

namespace bloom::runtime {

GpuScopeTapKey GpuScopeTapKey::of(const PreviewRequestIdentity& frameIdentity) {
    GpuScopeTapKey key;
    key.identity = frameIdentity;
    key.identity.requestGeneration = 0;
    key.identity.viewAdjust = ViewAdjust{};
    return key;
}

} // namespace bloom::runtime

namespace bloom::runtime::detail {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::chrono::steady_clock::time_point
scopeNow(const std::shared_ptr<PreviewDisplayServiceCore>& core) {
    return core->nativeClockOverride ? core->nativeClockOverride()
                                     : std::chrono::steady_clock::now();
}

[[nodiscard]] GpuScopeTapMeasureOutcome outcomeOf(const GpuScopeTapStatus status,
                                                  std::string message = {},
                                                  const GpuScopeTapSkip skip = GpuScopeTapSkip::None) {
    GpuScopeTapMeasureOutcome outcome;
    outcome.status = status;
    outcome.skip = skip;
    outcome.message = std::move(message);
    return outcome;
}

void countOutcome(ScopeTapState& scope, const GpuScopeTapMeasureOutcome& outcome) noexcept {
    switch (outcome.status) {
    case GpuScopeTapStatus::Measured:
    case GpuScopeTapStatus::EmptyArea:
        scope.measuresCompleted.fetch_add(1, std::memory_order_relaxed);
        break;
    case GpuScopeTapStatus::Skipped:
        scope.measuresSkipped.fetch_add(1, std::memory_order_relaxed);
        if (outcome.skip == GpuScopeTapSkip::NoRoom)
            scope.measuresNoRoom.fetch_add(1, std::memory_order_relaxed);
        break;
    case GpuScopeTapStatus::Busy:
        scope.measuresBusy.fetch_add(1, std::memory_order_relaxed);
        break;
    case GpuScopeTapStatus::Cancelled:
        scope.measuresCancelled.fetch_add(1, std::memory_order_relaxed);
        break;
    case GpuScopeTapStatus::Failed:
        scope.measuresFailed.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

// Under `scope.mutex`: the live entry `key` names, the Viewer's first.
[[nodiscard]] ScopeTapEntry* findEntryLocked(ScopeTapState& scope, const GpuScopeTapKey& key) {
    if (scope.viewer && !scope.viewer->dropped && scope.viewer->key == key)
        return &*scope.viewer;
    if (scope.fill && !scope.fill->dropped && scope.fill->key == key)
        return &*scope.fill;
    return nullptr;
}

// Whether a Viewer-class stage is ready, active or ahead.
[[nodiscard]] bool viewerStagePresent(const PreviewDisplayServiceCore& core) noexcept {
    const auto viewer = [](const std::shared_ptr<PreviewDisplayStageRecord>& stage) {
        return stage != nullptr && scopeViewerClass(stage->priority);
    };
    return viewer(core.residentNativeActive) || viewer(core.residentAhead) ||
           std::ranges::any_of(core.residentNativeReady, viewer);
}

// Under `scope.mutex`: whether the key's tap was given back or refused.
[[nodiscard]] bool wasGivenBackLocked(const ScopeTapState& scope, const GpuScopeTapKey& key) {
    return std::ranges::find(scope.givenBack, key) != scope.givenBack.end();
}

// Under `scope.mutex`: remembers a key whose tap was given back or refused (bounded).
void rememberGivenBackLocked(ScopeTapState& scope, const GpuScopeTapKey& key) {
    if (const auto found = std::ranges::find(scope.givenBack, key); found != scope.givenBack.end())
        scope.givenBack.erase(found);
    scope.givenBack.push_back(key);
    while (scope.givenBack.size() > kScopeGivenBackKeys)
        scope.givenBack.pop_front();
}

[[nodiscard]] std::uint64_t addSaturating(const std::uint64_t left,
                                          const std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
               ? std::numeric_limits<std::uint64_t>::max()
               : left + right;
}

// The device bytes the measure's own memory takes, from the device's allocation sizes: one color
// output per color step (display, XYZ), and the table buffer with its staging copy. The host side
// is the table readback's host copy. The picture it counts is the Viewer's and is not charged.
struct MeasureNeed final {
    std::uint64_t deviceBytes = 0;
    std::uint64_t hostBytes = 0;
};

[[nodiscard]] MeasureNeed measureNeedFor(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                         const ScopeMeasureWork& work,
                                         const render::GpuImage& picture) noexcept {
    std::shared_ptr<const render::GpuAllocationSizes> sizes;
    try {
        sizes = core->device->availableAllocationBudget().allocation_sizes;
    } catch (...) {
    }
    const std::uint64_t pixels = std::uint64_t{picture.width()} * picture.height();
    const std::uint64_t imageBytes = sizes && sizes->image
                                         ? sizes->image(picture.width(), picture.height())
                                         : pixels * 16U;
    const std::uint64_t layoutBytes = scopeMeasureTableBytes(work.job) / 3U;
    const std::uint64_t bufferBytes =
        sizes && sizes->buffer ? std::max(layoutBytes, sizes->buffer(layoutBytes)) : layoutBytes;
    const std::uint64_t images = (work.display != nullptr ? 1U : 0U) + (work.xyz != nullptr ? 1U : 0U);
    return {.deviceBytes = addSaturating(images * imageBytes, 2U * bufferBytes),
            .hostBytes = layoutBytes};
}

enum class Admission : std::uint8_t { Granted, Wait, NoRoom };

// Memory admission of the measure (#724), the exact rule. The measure's memory is admitted only as
// a Background-class processing phase through the policy's non-blocking reserve(); the service
// first asks the policy's own facts (never reserve() itself, whose refusal would ask the cache
// reclaimers to give memory back for the measure):
//   - the Viewer's last device and host need (the policy's `viewerPriority()`) is kept free beside
//     the measure, always, not only while the Viewer is active;
//   - NoRoom: the measure's need plus the Viewer's need exceeds the usable capacity of the card,
//     or exceeds the room free now while nothing transient holds it (no frame is admitted, none
//     waits or was admitted within the Viewer activity window). Free room is never made by
//     evicting anything for the measure, so a card full of retained frames and caches ends the
//     request Skipped(NoRoom) at once, and the caller falls back to the retained picture;
//   - Wait: the card could hold it, but a Viewer request waits or was admitted within the activity
//     window, or other admitted frames hold the room now. The request waits at the service
//     holding nothing, only while its tap is alive and no Viewer stage needs the device (the
//     per-turn gate), and ends NoRoom when the tap is given back;
//   - otherwise reserve() with GpuAdmissionClass::Background; its grant is retained on the device.
// A refusal after the grant (a race with a Viewer request on a worker) is a Wait.
// Without a processing-phase policy there is nothing to admit against: the measure runs as before
// (untracked) and still only when no phase is occupied.
[[nodiscard]] Admission admitMeasure(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                     ScopeMeasureRequest& request,
                                     const render::GpuImage& picture) noexcept {
    auto& scope = core->scope;
    const auto& policy = core->options.processingPhasePolicy;
    if (!policy)
        return Admission::Granted;
    const auto need = measureNeedFor(core, request.work, picture);
    scope.measureNeedBytes.store(need.deviceBytes, std::memory_order_relaxed);
    const auto capacity = policy->usableCapacity();
    const auto viewer = policy->viewerPriority();
    if (!capacity.hostReady || !capacity.deviceReady)
        return Admission::Wait;
    const std::uint64_t deviceWithViewer = addSaturating(need.deviceBytes, viewer.reserveDeviceBytes);
    const std::uint64_t hostWithViewer = addSaturating(need.hostBytes, viewer.reserveHostBytes);
    if (deviceWithViewer > capacity.deviceUsableBytes || hostWithViewer > capacity.hostUsableBytes)
        return Admission::NoRoom;
    if (viewer.waiting)
        return Admission::Wait;
    if (deviceWithViewer > capacity.deviceFreeBytes || hostWithViewer > capacity.hostFreeBytes) {
        const bool transient = viewer.active || capacity.admittedPhases != 0;
        return transient ? Admission::Wait : Admission::NoRoom;
    }
    const GpuProcessingPhaseEnvelope envelope{.output = GpuProcessingPhaseOutput::PackedPreview,
                                              // A native phase needs a non-empty extent; the
                                              // measure packs and publishes nothing.
                                              .pixels = 1,
                                              .metadataBytes = need.hostBytes,
                                              .requestedExecutorBytes = std::uint64_t{0},
                                              .deviceWorkingBytes = need.deviceBytes,
                                              .sourceFree = true,
                                              .native = true,
                                              .admissionClass = GpuAdmissionClass::Background};
    // The policy keeps the device need of the latest native phase as the room the caches keep free
    // for the next Viewer frame; a measure's need must not replace the Viewer's.
    const auto keptReserve = policy->deviceWorkReserveBytes();
    auto grant = policy->reserve(envelope);
    if (grant.status == render::GpuProcessingPhaseAdmission::Accepted && grant.lease)
        policy->noteDeviceWorkNeed(keptReserve);
    if (grant.status == render::GpuProcessingPhaseAdmission::BudgetRefused)
        return Admission::NoRoom;
    if (grant.status != render::GpuProcessingPhaseAdmission::Accepted || !grant.lease)
        return Admission::Wait;
    if (!core->device->retainProcessingPhase(grant.lease))
        return core->device->processingPhaseOccupied() ? Admission::Wait : Admission::NoRoom;
    scope.phase = std::move(grant.lease);
    scope.phaseParked = false;
    scope.phaseRetirePending = false;
    scope.measureAdmissions.fetch_add(1, std::memory_order_relaxed);
    return Admission::Granted;
}

// Whether a native memory refusal of the device's retained phase is what stopped the measure: the
// measure then yields (Skipped(NoRoom)) instead of failing.
[[nodiscard]] bool memoryRefused(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core->device == nullptr)
        return false;
    return core->device->nativeBufferStorageRefusal().has_value() ||
           core->device->nativeImageStorageRefusal().has_value();
}

// Whether this measure's phase can be the device's retained one now: a new measure needs the slot
// free; one that has begun takes its parked phase back when no stage's phase is retained.
[[nodiscard]] bool measurePhaseUsable(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                      const ScopeMeasureRequest& request) noexcept {
    auto& scope = core->scope;
    if (scope.phase == nullptr)
        return !core->device->processingPhaseOccupied();
    if (request.stepper == nullptr)
        return false; // an earlier measure's phase still awaits its retirement
    const auto retained = core->device->retainedProcessingPhase();
    if (retained == scope.phase)
        return true;
    if (scope.phaseParked && retained == nullptr &&
        core->device->selectProcessingPhase(scope.phase->id())) {
        scope.phaseParked = false;
        return true;
    }
    return false;
}

// The one gate of a measure step: the Viewer comes first, and a step begins only when no Viewer
// stage is ready, active or ahead and the measure's own phase can be the device's retained one (a
// new measure needs no phase occupied). A fill-tap measure yields to Viewer-class stages only: the
// fill stage that waits for it is not ready, so it never blocks it.
[[nodiscard]] bool scopeStepMayBegin(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                     ScopeMeasureRequest& request) noexcept {
    const bool blocked = viewerStagePresent(*core) || core->device == nullptr ||
                         !measurePhaseUsable(core, request);
    if (blocked && !request.yieldCounted) {
        request.yieldCounted = true;
        core->scope.measureYields.fetch_add(1, std::memory_order_relaxed);
    }
    return !blocked;
}

// Retires the measure's phase when the measure has ended: at once when it is the device's
// retained phase, after taking it back from the parked slot when no stage's phase is retained,
// and otherwise at a later turn. `proven` is false only when a fence of the measure was not
// proven retired: the phase is then latched unproven, as a frame's is, and recovered by the
// device-wide idle proof at the next stage start.
void endMeasurePhase(const std::shared_ptr<PreviewDisplayServiceCore>& core, const bool proven) noexcept {
    auto& scope = core->scope;
    if (scope.phase == nullptr)
        return;
    scope.phaseRetirePending = true;
    scope.phaseRetireProven = proven;
    settleScopePhase(core);
}

// Ends the request: frees its native state and, for a fill tap, the fill slot (whatever the
// result), then wakes the caller. `proven` is false only when a fence could not be proven retired:
// the native state is then handed to the destructors' bounded drain.
void finishRequest(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                   const std::shared_ptr<ScopeMeasureRequest>& request,
                   GpuScopeTapMeasureOutcome outcome, const bool proven = true) noexcept {
    auto& scope = core->scope;
    try {
        request->stepper.reset();
        if (!proven)
            scope.stage.reset();
        request->picture.reset();
        std::optional<ScopeTapEntry> freed;
        if (request->fill) {
            // The fill tap is measured once: its slot is free for the next flagged frame.
            std::lock_guard lock(scope.mutex);
            if (scope.fill && scope.fill->serial == request->tapSerial) {
                freed = std::move(scope.fill);
                scope.fill.reset();
            }
        }
        freed.reset();
        countOutcome(scope, outcome);
        {
            std::lock_guard lock(scope.mutex);
            scope.request.reset();
        }
        if (request->ownsPhase) {
            request->ownsPhase = false;
            endMeasurePhase(core, proven);
        }
        scope.active.store(false, std::memory_order_release);
        scope.stepperBegan.store(false, std::memory_order_release);
        scope.stopIssued.store(false, std::memory_order_release);
        scope.epoch.fetch_add(1, std::memory_order_acq_rel);
    } catch (...) {
        // The slot bookkeeping above cannot throw in practice; the caller must still wake.
    }
    {
        std::lock_guard lock(request->mutex);
        request->outcome = std::move(outcome);
        request->done = true;
    }
    request->condition.notify_all();
}

[[nodiscard]] GpuScopeTapMeasureOutcome
outcomeFromStepper(const ScopeMeasureRequest& request, const ScopeMeasureFault& fault,
                   const std::uint64_t tableDispatches) {
    GpuScopeTapMeasureOutcome outcome;
    outcome.area = request.work.area;
    outcome.colorDispatches = request.work.colorDispatches;
    outcome.tableDispatches = tableDispatches;
    if (fault.status == GpuProcessFrameStatus::Evaluated && request.work.counts) {
        outcome.status = GpuScopeTapStatus::Measured;
        outcome.counts = *request.work.counts;
        outcome.tableReadbacks = request.work.tableReadbacks;
        outcome.tableReadbackBytes = request.work.tableReadbackBytes;
        return outcome;
    }
    outcome.status = fault.status == GpuProcessFrameStatus::Cancelled
                         ? GpuScopeTapStatus::Cancelled
                         : GpuScopeTapStatus::Failed;
    outcome.message = fault.message;
    return outcome;
}

void stepRequest(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                 const std::shared_ptr<ScopeMeasureRequest>& request) {
    auto& scope = core->scope;
    const bool stopping = core->stopping.load(std::memory_order_acquire);
    bool cancelled = request->cancellation.isCancellationRequested() || stopping;

    // The tap the request names, and whether it is still there.
    std::shared_ptr<const render::GpuImage> picture;
    bool newerViewerTap = false;
    bool tapGivenBack = false;
    bool found = false;
    {
        std::lock_guard lock(scope.mutex);
        if (const auto* entry = findEntryLocked(scope, request->key)) {
            found = true;
            picture = entry->picture;
        } else {
            tapGivenBack = wasGivenBackLocked(scope, request->key);
            newerViewerTap = !tapGivenBack && !request->fill && scope.viewer &&
                             scope.viewer->serial > request->tapSerial;
        }
    }
    if (request->givenBack)
        cancelled = true;
    if (request->stepper == nullptr) {
        if (request->givenBack || (tapGivenBack && !cancelled)) {
            finishRequest(core, request,
                          outcomeOf(GpuScopeTapStatus::Skipped,
                                    "the device's memory was needed for a frame; the scope measure "
                                    "yielded",
                                    GpuScopeTapSkip::NoRoom));
            return;
        }
        if (cancelled) {
            finishRequest(core, request,
                          outcomeOf(GpuScopeTapStatus::Cancelled,
                                    stopping ? "the service is shutting down"
                                             : "cancelled before the scope measure began"));
            return;
        }
        if (!found || picture == nullptr) {
            finishRequest(core, request,
                          outcomeOf(GpuScopeTapStatus::Skipped,
                                    newerViewerTap
                                        ? "a newer Viewer frame replaced the frame to measure"
                                        : "the frame to measure is no longer tapped",
                                    newerViewerTap ? GpuScopeTapSkip::Superseded
                                                   : GpuScopeTapSkip::TapGone));
            return;
        }
        if (!scopeStepMayBegin(core, *request))
            return;
        if (core->device == nullptr || core->residentRouteTerminal || residentDeviceLost(core)) {
            finishRequest(core, request,
                          outcomeOf(GpuScopeTapStatus::Failed,
                                    "the device is not available for the scope measure"));
            return;
        }
        const std::uint64_t budget = core->previewByteAllowance();
        if (budget == 0) {
            // No allowance to measure under: the measure waits for none and takes nothing.
            finishRequest(core, request,
                          outcomeOf(GpuScopeTapStatus::Skipped,
                                    "the preview memory allowance is not known yet",
                                    GpuScopeTapSkip::NoRoom));
            return;
        }
        // The measure's memory is admitted as Background before it takes any (see admitMeasure).
        switch (admitMeasure(core, *request, *picture)) {
        case Admission::Wait:
            if (!request->admissionWaitCounted) {
                request->admissionWaitCounted = true;
                scope.measureAdmissionWaits.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        case Admission::NoRoom:
            finishRequest(core, request,
                          outcomeOf(GpuScopeTapStatus::Skipped,
                                    "the device has no room for the scope measure beside what "
                                    "the Viewer needs",
                                    GpuScopeTapSkip::NoRoom));
            return;
        case Admission::Granted:
            request->ownsPhase = scope.phase != nullptr;
            break;
        }
        if (scope.stage == nullptr) {
            std::string message;
            scope.stage = ScopeMeasureStage::create(*core->device, message);
            if (scope.stage == nullptr) {
                finishRequest(core, request,
                              outcomeOf(GpuScopeTapStatus::Failed,
                                        message.empty()
                                            ? std::string("the device cannot host the scope "
                                                          "color steps")
                                            : message));
                return;
            }
        }
        request->picture = std::move(picture);
        request->tableDispatchesBefore = render::GpuScopeTables::nativeDispatchCount();
        request->stepper = std::make_unique<ScopeMeasureStepper>(*scope.stage, request->work,
                                                                 request->picture, budget);
        scope.measuresStarted.fetch_add(1, std::memory_order_relaxed);
    }

    // A caller dropped the tap: the measure is not wanted any more.
    if (!found && !request->tapDropped) {
        std::lock_guard lock(scope.mutex);
        const bool dropped = (scope.viewer && scope.viewer->dropped && scope.viewer->key == request->key) ||
                             (scope.fill && scope.fill->dropped && scope.fill->key == request->key);
        request->tapDropped = dropped;
    }
    if (request->tapDropped)
        cancelled = true;

    // The dispatch deadline runs over a submission in flight, from when it was first seen.
    const auto now = scopeNow(core);
    bool deadlineExpired = false;
    if (request->stepper->submissionInFlight()) {
        if (!request->deadlineAnchored) {
            request->deadlineAnchored = true;
            request->deadlineAnchor = now;
        }
        deadlineExpired = now - request->deadlineAnchor >= core->options.nativeDispatchDeadline;
    } else {
        request->deadlineAnchored = false;
    }
    if ((cancelled || deadlineExpired) && !request->stopAnchored) {
        request->stopAnchored = true;
        request->stopAnchor = now;
    }

    ScopeMeasureStepper::Hooks hooks;
    hooks.mayBegin = [core, raw = request.get()] { return scopeStepMayBegin(core, *raw); };
    if (core->scopeTablesPollOverride && !stopping) {
        hooks.pollTables = [core](render::GpuScopeTables& tables) {
            return core->scopeTablesPollOverride(tables);
        };
    }
    scope.stepping = true;
    const auto state = request->stepper->step(cancelled, deadlineExpired, hooks);
    scope.stepping = false;
    if (request->stepper->began())
        scope.stepperBegan.store(true, std::memory_order_release);
    if (request->stepper->stopIssued())
        scope.stopIssued.store(true, std::memory_order_release);
    if (state == ScopeMeasureStepper::State::Settled) {
        auto outcome = outcomeFromStepper(
            *request, request->stepper->fault(),
            render::GpuScopeTables::nativeDispatchCount() - request->tableDispatchesBefore);
        if (outcome.status == GpuScopeTapStatus::Cancelled && request->givenBack)
            outcome = outcomeOf(GpuScopeTapStatus::Skipped,
                                "the device's memory was needed for a frame; the scope measure "
                                "yielded",
                                GpuScopeTapSkip::NoRoom);
        else if (outcome.status == GpuScopeTapStatus::Cancelled && request->tapDropped)
            outcome = outcomeOf(GpuScopeTapStatus::Skipped, "the frame's tap was dropped",
                                GpuScopeTapSkip::TapGone);
        else if (outcome.status == GpuScopeTapStatus::Failed && memoryRefused(core))
            outcome = outcomeOf(GpuScopeTapStatus::Skipped,
                                "the device refused the scope measure's memory; it yields to "
                                "the Viewer",
                                GpuScopeTapSkip::NoRoom);
        if (outcome.status == GpuScopeTapStatus::Measured) {
            scope.colorDispatches.fetch_add(outcome.colorDispatches, std::memory_order_relaxed);
            scope.tableDispatches.fetch_add(outcome.tableDispatches, std::memory_order_relaxed);
            scope.tableReadbacks.fetch_add(outcome.tableReadbacks, std::memory_order_relaxed);
            scope.tableReadbackBytes.store(outcome.tableReadbackBytes, std::memory_order_relaxed);
            scope.tableReadbackBytesTotal.fetch_add(outcome.tableReadbackBytes,
                                                    std::memory_order_relaxed);
        }
        finishRequest(core, request, std::move(outcome), request->stepper->retirementProven());
        return;
    }
    // A cancelled or expired submission whose fence is still not proven after a whole deadline:
    // the destructors of the native state perform their own bounded drain and quarantine.
    if (request->stopAnchored && now - request->stopAnchor >= core->options.nativeDispatchDeadline &&
        request->stepper->stopIssued()) {
        auto outcome = outcomeFromStepper(*request, request->stepper->fault(), 0);
        if (outcome.status == GpuScopeTapStatus::Measured || outcome.message.empty())
            outcome = outcomeOf(deadlineExpired ? GpuScopeTapStatus::Failed
                                                : GpuScopeTapStatus::Cancelled,
                                "the scope measure's submission was not proven retired in time");
        finishRequest(core, request, std::move(outcome), false);
    }
}

} // namespace

bool scopeTapWanted(const PreviewDisplayStageRecord& stage) noexcept {
    return scopeViewerClass(stage.priority) || stage.tapForScopes;
}

bool scopeFillStageHeldBack(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                            PreviewDisplayStageRecord& stage) noexcept {
    if (!stage.tapForScopes || scopeViewerClass(stage.priority))
        return false;
    bool busy = (core->residentNativeActive != nullptr && core->residentNativeActive->holdsFillTapSlot) ||
                (core->residentAhead != nullptr && core->residentAhead->holdsFillTapSlot);
    if (!busy) {
        std::lock_guard lock(core->scope.mutex);
        busy = core->scope.fill.has_value();
    }
    if (busy && !stage.fillWaitCounted) {
        stage.fillWaitCounted = true;
        core->scope.fillStageWaits.fetch_add(1, std::memory_order_relaxed);
    }
    return busy;
}

void keepScopeTap(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                  PreviewDisplayStageRecord& stage) noexcept {
    auto picture = std::move(stage.scopePicture);
    const bool fill = !scopeViewerClass(stage.priority);
    // The slot a started flagged fill stage held is now the tap's, or free again.
    const auto releaseReservation = [&] { stage.holdsFillTapSlot = false; };
    try {
        if (picture == nullptr || !scopeTapWanted(stage) || stage.gpuStage == nullptr ||
            stage.gpuStage->scene() == nullptr || !core->device) {
            releaseReservation();
            return;
        }
        const auto& scene = *stage.gpuStage->scene();
        const auto& plan = scene.processIdentity().plan;
        const auto& descriptor = scene.outputDescriptor();
        const auto extent = descriptor.dataWindow().extent();
        if (plan == nullptr || picture->width() != extent.width() ||
            picture->height() != extent.height() || !picture->isBoundTo(*core->device) ||
            // The picture outlives the frame's phase: its storage becomes the root's, exactly as
            // a scene-cache entry's does.
            !picture->promoteStorageOwnership()) {
            releaseReservation();
            return;
        }
        // A flagged fill frame's tap is kept only if it fits as Background; otherwise the frame
        // is delivered and retained as an unflagged one, the slot stays free and the caller is
        // told by Skipped(NoRoom) for its key.
        if (fill && !scopeFillTapFits(core, GpuScopeTapKey::of(stage.gpuStage->desiredIdentity()),
                                      picture->allocationBytes())) {
            releaseReservation();
            return;
        }
        ScopeTapEntry entry{
            .key = GpuScopeTapKey::of(stage.gpuStage->desiredIdentity()),
            .geometry = GpuScopeTapGeometry{.dataWindow = descriptor.dataWindow(),
                                            .displayWindow = descriptor.displayWindow(),
                                            .roi = scene.processIdentity().roi,
                                            .compositionWidth = plan->format().width(),
                                            .compositionHeight = plan->format().height(),
                                            .fill = fill},
            .picture = std::move(picture),
            .serial = 0,
            .dropped = false};
        auto& scope = core->scope;
        std::optional<ScopeTapEntry> replaced;
        {
            std::lock_guard lock(scope.mutex);
            auto& slot = fill ? scope.fill : scope.viewer;
            if (fill && slot.has_value()) {
                // Cannot happen while the fill is paced; the new picture is not kept.
                replaced = std::move(entry);
            } else {
                entry.serial = ++(fill ? scope.fillSerial : scope.viewerSerial);
                replaced = std::move(slot);
                slot = std::move(entry);
            }
        }
        // The replaced picture is released here, on the owner thread, outside the lock.
        replaced.reset();
        releaseReservation();
        (fill ? scope.fillTapsKept : scope.tapsKept).fetch_add(1, std::memory_order_relaxed);
        scope.epoch.fetch_add(1, std::memory_order_acq_rel);
    } catch (...) {
        releaseReservation();
    }
}

void reapScopeTaps(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    auto& scope = core->scope;
    std::optional<ScopeTapEntry> viewer;
    std::optional<ScopeTapEntry> fill;
    try {
        std::lock_guard lock(scope.mutex);
        // A tap the in-flight measure still reads stays until the measure has ended: it is
        // that measure's picture too, and the measure sees the drop and stops.
        const auto inUse = [&](const ScopeTapEntry& entry) {
            return scope.request != nullptr && scope.request->stepper != nullptr &&
                   scope.request->key == entry.key && !scope.request->tapDropped;
        };
        if (scope.viewer && scope.viewer->dropped && !inUse(*scope.viewer)) {
            viewer = std::move(scope.viewer);
            scope.viewer.reset();
        }
        if (scope.fill && scope.fill->dropped && !inUse(*scope.fill)) {
            fill = std::move(scope.fill);
            scope.fill.reset();
        }
    } catch (...) {
        return;
    }
    if (viewer || fill)
        scope.epoch.fetch_add(1, std::memory_order_acq_rel);
}

void stepScopeMeasure(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    settleScopePhase(core);
    std::shared_ptr<ScopeMeasureRequest> request;
    try {
        std::lock_guard lock(core->scope.mutex);
        request = core->scope.request;
    } catch (...) {
        return;
    }
    if (request == nullptr)
        return;
    try {
        stepRequest(core, request);
    } catch (...) {
        finishRequest(core, request,
                      outcomeOf(GpuScopeTapStatus::Failed, "the scope measure raised"), false);
    }
}

void shutdownScopeMeasure(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    auto& scope = core->scope;
    std::shared_ptr<ScopeMeasureRequest> request;
    try {
        std::lock_guard lock(scope.mutex);
        scope.closed = true;
        request = scope.request;
    } catch (...) {
        return;
    }
    if (request == nullptr)
        return;
    bool proven = true;
    try {
        if (request->stepper != nullptr) {
            // The shutdown path polls the real tables, never the test hold, and stops the
            // submission in flight; the native state is freed only after its fence is proven.
            const ScopeMeasureStepper::Hooks hooks;
            const auto started = std::chrono::steady_clock::now();
            auto state = request->stepper->step(true, false, hooks);
            while (state != ScopeMeasureStepper::State::Settled &&
                   std::chrono::steady_clock::now() - started < core->options.nativeDispatchDeadline) {
                std::this_thread::sleep_for(50us);
                state = request->stepper->step(true, false, hooks);
            }
            proven = state == ScopeMeasureStepper::State::Settled &&
                     request->stepper->retirementProven();
        }
    } catch (...) {
        proven = false;
    }
    finishRequest(core, request,
                  outcomeOf(GpuScopeTapStatus::Cancelled, "the service is shutting down"), proven);
}

void releaseScopeTaps(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    auto& scope = core->scope;
    std::optional<ScopeTapEntry> viewer;
    std::optional<ScopeTapEntry> fill;
    try {
        std::lock_guard lock(scope.mutex);
        viewer = std::move(scope.viewer);
        scope.viewer.reset();
        fill = std::move(scope.fill);
        scope.fill.reset();
    } catch (...) {
    }
    viewer.reset();
    fill.reset();
    scope.epoch.fetch_add(1, std::memory_order_acq_rel);
}

namespace {

// Gives the slots' pictures back. Each released tap is counted by `counter` of its slot (when
// given) and its key remembered so a later request for it ends Skipped(NoRoom).
ScopeGiveBack releaseTapSlots(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                              std::atomic<std::uint64_t>* viewerCounter,
                              std::atomic<std::uint64_t>* fillCounter) noexcept {
    auto& scope = core->scope;
    std::optional<ScopeTapEntry> viewer;
    std::optional<ScopeTapEntry> fill;
    try {
        std::lock_guard lock(scope.mutex);
        if (scope.viewer) {
            if (!scope.viewer->dropped)
                rememberGivenBackLocked(scope, scope.viewer->key);
            viewer = std::move(scope.viewer);
            scope.viewer.reset();
        }
        if (scope.fill) {
            if (!scope.fill->dropped)
                rememberGivenBackLocked(scope, scope.fill->key);
            fill = std::move(scope.fill);
            scope.fill.reset();
        }
    } catch (...) {
        return {};
    }
    if (viewer && viewerCounter != nullptr)
        viewerCounter->fetch_add(1, std::memory_order_relaxed);
    if (fill && fillCounter != nullptr)
        fillCounter->fetch_add(1, std::memory_order_relaxed);
    ScopeGiveBack result;
    result.any = viewer.has_value() || fill.has_value();
    for (const auto* entry : {&viewer, &fill}) {
        if (entry->has_value() && (*entry)->picture != nullptr &&
            (*entry)->picture.use_count() == 1)
            result.freedBytes = addSaturating(result.freedBytes, (*entry)->picture->allocationBytes());
    }
    // The pictures are released here, on the owner thread, outside the lock.
    viewer.reset();
    fill.reset();
    if (result.any)
        scope.epoch.fetch_add(1, std::memory_order_acq_rel);
    return result;
}

// Ends the measure's request for memory the Viewer needs: a request that has not begun, or whose
// begun measure has nothing in flight, ends Skipped(NoRoom) now; one with a submission in flight is
// told to stop and is polled to its end (its fence must be proven retired before its memory is
// freed), then ends Skipped(NoRoom).
bool giveBackMeasure(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    std::shared_ptr<ScopeMeasureRequest> request;
    try {
        std::lock_guard lock(core->scope.mutex);
        request = core->scope.request;
    } catch (...) {
        return false;
    }
    if (request == nullptr || request->givenBack)
        return false;
    request->givenBack = true;
    core->scope.measuresGivenBack.fetch_add(1, std::memory_order_relaxed);
    try {
        stepRequest(core, request);
    } catch (...) {
        finishRequest(core, request,
                      outcomeOf(GpuScopeTapStatus::Failed, "the scope measure raised"), false);
    }
    return true;
}

} // namespace

ScopeGiveBack giveScopeMemoryBack(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    // A measure's own allocation never asks for memory back for itself.
    if (core == nullptr || core->scope.stepping)
        return {};
    // What had been reclaimed before this first give-back (zero: the scope memory went first).
    std::uint64_t reclaimsSoFar = core->counterDeviceRoomReclaims.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(core->stateMutex);
        reclaimsSoFar += core->publishedResidentFrameEvictionSerial;
    }
    auto result = releaseTapSlots(core, &core->scope.viewerTapYields, &core->scope.fillTapYields);
    const bool measure = giveBackMeasure(core);
    result.any = result.any || measure;
    if (result.any && !core->scope.firstGiveBackNoted) {
        core->scope.firstGiveBackNoted = true;
        core->scope.reclaimsSeenAtFirstGiveBack.store(reclaimsSoFar, std::memory_order_relaxed);
    }
    return result;
}

void releaseScopeTapsIdle(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    if (core == nullptr)
        return;
    auto& scope = core->scope;
    std::uint64_t held = 0;
    try {
        std::lock_guard lock(scope.mutex);
        held = (scope.viewer ? 1U : 0U) + (scope.fill ? 1U : 0U);
    } catch (...) {
        return;
    }
    if (held == 0)
        return;
    if (releaseTapSlots(core, nullptr, nullptr).any)
        scope.tapIdleReleases.fetch_add(held, std::memory_order_relaxed);
}

bool yieldScopeDeviceToStage(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    auto& scope = core->scope;
    if (core == nullptr || core->device == nullptr || scope.phase == nullptr)
        return true;
    try {
        if (core->device->retainedProcessingPhase() != scope.phase)
            return true; // not on the slot: parked, or already retired
        std::shared_ptr<ScopeMeasureRequest> request;
        {
            std::lock_guard lock(scope.mutex);
            request = scope.request;
        }
        if (request != nullptr && request->stepper != nullptr &&
            request->stepper->submissionInFlight()) {
            // In flight: only polled. Its phase is parked so the stage retains its own; the
            // stage does not wait for the measure.
            if (!core->device->parkProcessingPhase())
                return false;
            scope.phaseParked = true;
            scope.measurePhaseParks.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (request != nullptr)
            giveBackMeasure(core);
        else
            settleScopePhase(core);
        return core->device->retainedProcessingPhase() != scope.phase;
    } catch (...) {
        return false;
    }
}

bool scopeFillTapFits(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                      const GpuScopeTapKey& key, const std::uint64_t pictureBytes) noexcept {
    bool fits = true;
    try {
        if (const auto& policy = core->options.processingPhasePolicy) {
            // The held picture is a Background claim of its allocation bytes, measured beside the
            // Viewer's last need when the frame publishes (before its own phase's room returns).
            const auto capacity = policy->usableCapacity();
            const auto viewer = policy->viewerPriority();
            fits = capacity.deviceReady && !viewer.waiting &&
                   addSaturating(pictureBytes, viewer.reserveDeviceBytes) <=
                       capacity.deviceFreeBytes;
        }
        if (!fits) {
            core->scope.fillTapsRefused.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lock(core->scope.mutex);
            rememberGivenBackLocked(core->scope, key);
        }
    } catch (...) {
    }
    return fits;
}

void settleScopePhase(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    auto& scope = core->scope;
    if (scope.phase == nullptr || !scope.phaseRetirePending)
        return;
    try {
        if (core->device == nullptr) {
            scope.phase.reset();
            scope.phaseParked = false;
            scope.phaseRetirePending = false;
            return;
        }
        const auto retained = core->device->retainedProcessingPhase();
        if (retained != scope.phase) {
            // Parked behind a stage's phase: retired once that stage's phase is gone.
            if (!(scope.phaseParked && retained == nullptr &&
                  core->device->selectProcessingPhase(scope.phase->id())))
                return;
        }
        core->device->retireProcessingPhase(scope.phase->id(), scope.phaseRetireProven);
        scope.phase.reset();
        scope.phaseParked = false;
        scope.phaseRetirePending = false;
    } catch (...) {
    }
}

std::uint64_t reclaimScopePrograms(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                   const std::uint64_t bytes) noexcept {
    if (core == nullptr || core->scope.stage == nullptr || core->scope.stepping)
        return 0;
    return core->scope.stage->reclaimPrograms(bytes);
}

void retireScopeNative(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    shutdownScopeMeasure(core);
    releaseScopeTaps(core);
    // The color step executor and the table pipeline are destroyed on the owner thread; their
    // destructors drain what is unretired.
    core->scope.stage.reset();
    settleScopePhase(core);
    core->scope.phase.reset();
    core->scope.phaseParked = false;
    core->scope.phaseRetirePending = false;
}

std::optional<GpuScopeTapGeometry>
scopeTapGeometryOf(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                   const GpuScopeTapKey& key) {
    std::lock_guard lock(core->scope.mutex);
    if (const auto* entry = findEntryLocked(core->scope, key))
        return entry->geometry;
    return std::nullopt;
}

GpuScopeTapMeasureOutcome measureScopeTapOn(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                            const GpuScopeTapKey& key,
                                            const GpuScopeMeasureJob& job,
                                            const CancellationToken& cancellation) {
    auto& scope = core->scope;
    scope.measuresRequested.fetch_add(1, std::memory_order_relaxed);
    const auto settled = [&](GpuScopeTapMeasureOutcome outcome) {
        countOutcome(scope, outcome);
        return outcome;
    };

    std::optional<GpuScopeTapGeometry> geometry;
    std::uint64_t serial = 0;
    bool givenBack = false;
    {
        std::lock_guard lock(scope.mutex);
        if (scope.closed || core->stopping.load(std::memory_order_acquire))
            return settled(outcomeOf(GpuScopeTapStatus::Cancelled, "the service is shutting down"));
        if (scope.request != nullptr)
            return settled(outcomeOf(GpuScopeTapStatus::Busy,
                                     "another scope measure is queued or in flight"));
        if (const auto* entry = findEntryLocked(scope, key)) {
            geometry = entry->geometry;
            serial = entry->serial;
        } else {
            givenBack = wasGivenBackLocked(scope, key);
        }
    }
    if (!geometry && givenBack)
        return settled(outcomeOf(GpuScopeTapStatus::Skipped,
                                 "the frame's tap was given back or refused for memory; measure "
                                 "its retained picture",
                                 GpuScopeTapSkip::NoRoom));
    if (!geometry)
        return settled(outcomeOf(GpuScopeTapStatus::Skipped,
                                 "the frame to measure is no longer tapped",
                                 GpuScopeTapSkip::TapGone));

    auto request = std::make_shared<ScopeMeasureRequest>();
    request->key = key;
    request->work.job = job;
    request->cancellation = cancellation;
    request->fill = geometry->fill;
    request->tapSerial = serial;
    // The color programs are prepared here, on the calling worker, never on the owner.
    if (const auto failed = prepareScopeMeasureForPicture(
            request->work, geometry->dataWindow, geometry->displayWindow, geometry->roi,
            geometry->compositionWidth, geometry->compositionHeight, cancellation)) {
        return settled(outcomeOf(failed->status == GpuProcessFrameStatus::Cancelled
                                     ? GpuScopeTapStatus::Cancelled
                                     : GpuScopeTapStatus::Failed,
                                 failed->message));
    }
    if (request->work.emptyArea)
        return settled(outcomeOf(GpuScopeTapStatus::EmptyArea));

    {
        std::lock_guard lock(scope.mutex);
        if (scope.closed || core->stopping.load(std::memory_order_acquire))
            return settled(outcomeOf(GpuScopeTapStatus::Cancelled, "the service is shutting down"));
        if (scope.request != nullptr)
            return settled(outcomeOf(GpuScopeTapStatus::Busy,
                                     "another scope measure is queued or in flight"));
        scope.request = request;
        scope.active.store(true, std::memory_order_release);
    }
    core->notify();

    std::unique_lock lock(request->mutex);
    request->condition.wait(lock, [&] { return request->done; });
    return request->outcome;
}

void dropScopeTapOn(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                    const GpuScopeTapKey& key) noexcept {
    try {
        {
            std::lock_guard lock(core->scope.mutex);
            // Only the fill slot is a caller's to free: the Viewer's tap lives until a newer
            // Viewer frame replaces it or memory is needed, and a fill tap of the same frame
            // (the same key) must never take it along when its owner lets go.
            auto& fill = core->scope.fill;
            if (fill.has_value() && !fill->dropped && fill->key == key)
                fill->dropped = true;
        }
        core->notify();
    } catch (...) {
    }
}

} // namespace bloom::runtime::detail
