// The scope tap and the device scope measure of the preview display service (#724). See
// gpu_preview_display_service_scope_private.hpp for the design and the thread rules.

#include "gpu_preview_display_service_private.hpp"

#include <bloom/render/gpu_scope_tables.hpp>

#include <algorithm>
#include <chrono>
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

void countOutcome(ScopeTapState& scope, const GpuScopeTapStatus status) noexcept {
    switch (status) {
    case GpuScopeTapStatus::Measured:
    case GpuScopeTapStatus::EmptyArea:
        scope.measuresCompleted.fetch_add(1, std::memory_order_relaxed);
        break;
    case GpuScopeTapStatus::Skipped:
        scope.measuresSkipped.fetch_add(1, std::memory_order_relaxed);
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

// The one gate of a measure step: the Viewer comes first, and a step starts only when no frame's
// processing phase is retained or parked on the device, so the measure's native storage is never
// charged to a frame's phase and never outlives it. A fill-tap measure yields to Viewer-class
// stages only: the fill stage that waits for it is not ready, so it never blocks it.
// WP2b hook: the measure's own Background-class admission goes here.
[[nodiscard]] bool scopeStepMayBegin(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                     ScopeMeasureRequest& request) noexcept {
    const bool blocked = viewerStagePresent(*core) || core->device == nullptr ||
                         core->device->processingPhaseOccupied();
    if (blocked && !request.yieldCounted) {
        request.yieldCounted = true;
        core->scope.measureYields.fetch_add(1, std::memory_order_relaxed);
    }
    return !blocked;
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
        countOutcome(scope, outcome.status);
        {
            std::lock_guard lock(scope.mutex);
            scope.request.reset();
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
    bool found = false;
    {
        std::lock_guard lock(scope.mutex);
        if (const auto* entry = findEntryLocked(scope, request->key)) {
            found = true;
            picture = entry->picture;
        } else {
            newerViewerTap = !request->fill && scope.viewer && scope.viewer->serial > request->tapSerial;
        }
    }
    if (request->stepper == nullptr) {
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
    const auto state = request->stepper->step(cancelled, deadlineExpired, hooks);
    if (request->stepper->began())
        scope.stepperBegan.store(true, std::memory_order_release);
    if (request->stepper->stopIssued())
        scope.stopIssued.store(true, std::memory_order_release);
    if (state == ScopeMeasureStepper::State::Settled) {
        auto outcome = outcomeFromStepper(
            *request, request->stepper->fault(),
            render::GpuScopeTables::nativeDispatchCount() - request->tableDispatchesBefore);
        if (outcome.status == GpuScopeTapStatus::Cancelled && request->tapDropped)
            outcome = outcomeOf(GpuScopeTapStatus::Skipped, "the frame's tap was dropped",
                                GpuScopeTapSkip::TapGone);
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

void retireScopeNative(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    shutdownScopeMeasure(core);
    releaseScopeTaps(core);
    // The color step executor and the table pipeline are destroyed on the owner thread; their
    // destructors drain what is unretired.
    core->scope.stage.reset();
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
        countOutcome(scope, outcome.status);
        return outcome;
    };

    std::optional<GpuScopeTapGeometry> geometry;
    std::uint64_t serial = 0;
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
        }
    }
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
            for (auto* slot : {&core->scope.viewer, &core->scope.fill}) {
                if (slot->has_value() && !(*slot)->dropped && (*slot)->key == key)
                    (*slot)->dropped = true;
            }
        }
        core->notify();
    } catch (...) {
    }
}

} // namespace bloom::runtime::detail
