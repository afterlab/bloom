// The scope tables of a preview frame, counted on the service's device as part of the frame's job
// (#730). See gpu_preview_display_service_scope_private.hpp.

#include "gpu_preview_display_service_private.hpp"
#include "preview_frame_scopes_private.hpp"

#include <bloom/render/gpu_scope_tables.hpp>

#include <utility>

namespace bloom::runtime::detail {
namespace {

[[nodiscard]] std::shared_ptr<const PreviewFrameScopes>
failed(const PreparedFrameScopeMeasure& prepared, std::string message) {
    ScopeFrameMeasure measure;
    measure.status = ScopeFrameStatus::Failed;
    measure.message = std::move(message);
    return makePreviewFrameScopes(prepared.settings, std::move(measure), nullptr,
                                  prepared.reduction);
}

void finishMeasure(PreviewDisplayServiceCore& core, PreviewDisplayStageRecord& stage) noexcept {
    try {
        const auto& prepared = stage.gpuStage->scopeMeasure();
        const auto& fault = stage.scopeStepper->fault();
        auto& counters = core.scope;
        counters.colorDispatches.fetch_add(stage.scopeWork->colorDispatches,
                                           std::memory_order_relaxed);
        if (fault.status == GpuProcessFrameStatus::Evaluated && stage.scopeWork->counts) {
            ScopeFrameMeasure measure;
            measure.status = ScopeFrameStatus::Measured;
            measure.route = ScopeRoute::Gpu;
            measure.counts = std::move(*stage.scopeWork->counts);
            measure.area = stage.scopeWork->area;
            measure.colorDispatches = stage.scopeWork->colorDispatches;
            measure.tableReadbacks = stage.scopeWork->tableReadbacks;
            measure.tableReadbackBytes = stage.scopeWork->tableReadbackBytes;
            stage.scopes = makePreviewFrameScopes(prepared->settings, std::move(measure),
                                                  prepared->color, prepared->reduction);
            counters.framesMeasured.fetch_add(1, std::memory_order_relaxed);
            counters.tableReadbacks.fetch_add(stage.scopeWork->tableReadbacks,
                                              std::memory_order_relaxed);
            counters.tableReadbackBytes.store(stage.scopeWork->tableReadbackBytes,
                                              std::memory_order_relaxed);
            counters.tableReadbackBytesTotal.fetch_add(stage.scopeWork->tableReadbackBytes,
                                                       std::memory_order_relaxed);
        } else {
            stage.scopes =
                failed(*prepared,
                       fault.message.empty() ? std::string("The device could not count the scopes.")
                                             : fault.message);
            counters.framesFailed.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (...) {
        stage.scopes.reset();
    }
    // Nothing of the measure is in flight: its pictures and the scene alias go now.
    stage.scopeStepper.reset();
    stage.scopeWork.reset();
    stage.scopePicture.reset();
}

} // namespace

bool scopeMeasureWanted(const PreviewDisplayStageRecord& stage) noexcept {
    return stage.gpuStage != nullptr && stage.gpuStage->scopeMeasure() != nullptr;
}

bool beginFrameScopeMeasure(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                            PreviewDisplayStageRecord& stage) noexcept {
    if (stage.gpuStage == nullptr)
        return false;
    stage.scopes = stage.gpuStage->settledScopes();
    const auto& prepared = stage.gpuStage->scopeMeasure();
    if (prepared == nullptr)
        return false;
    try {
        if (stage.scopePicture == nullptr || !core->device) {
            stage.scopes = failed(*prepared, "The frame's scene picture is not available to "
                                             "count its scopes.");
            core->scope.framesFailed.fetch_add(1, std::memory_order_relaxed);
            stage.scopePicture.reset();
            return false;
        }
        if (core->scope.stage == nullptr) {
            std::string message;
            core->scope.stage = ScopeMeasureStage::create(*core->device, message);
            if (core->scope.stage == nullptr) {
                stage.scopes = failed(*prepared,
                                      message.empty() ? std::string("The device cannot count the "
                                                                    "scopes.")
                                                      : message);
                core->scope.framesFailed.fetch_add(1, std::memory_order_relaxed);
                stage.scopePicture.reset();
                return false;
            }
        }
        stage.scopeWork = std::make_unique<ScopeMeasureWork>(prepared->work);
        const auto allowance =
            stage.gpuByteAllowance != 0 ? stage.gpuByteAllowance : core->previewByteAllowance();
        stage.scopeStepper = std::make_unique<ScopeMeasureStepper>(
            *core->scope.stage, *stage.scopeWork, stage.scopePicture, allowance);
        return true;
    } catch (...) {
        stage.scopeStepper.reset();
        stage.scopeWork.reset();
        stage.scopePicture.reset();
        stage.scopes = failed(*prepared, "The scope measure could not be started.");
        return false;
    }
}

bool stepFrameScopeMeasure(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                           PreviewDisplayStageRecord& stage, const bool cancelled,
                           const bool deadlineExpired) noexcept {
    if (stage.scopeStepper == nullptr)
        return true;
    ScopeMeasureStepper::Hooks hooks;
    // The kernel's table slot is one per process: a headless read on the final-render device may
    // hold it, and the frame's count waits for it (polled, never waited on) instead of failing.
    hooks.mayBegin = [] { return render::GpuScopeTables::processSlotFree(); };
    if (core->scopeTablesPollOverride)
        hooks.pollTables = core->scopeTablesPollOverride;
    const auto state = stage.scopeStepper->step(cancelled, deadlineExpired, hooks);
    if (state != ScopeMeasureStepper::State::Settled)
        return false;
    finishMeasure(*core, stage);
    return true;
}

void releaseScopeStageIdle(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    auto& stage = core->scope.stage;
    if (stage != nullptr && !stage->hasUnretiredSubmission())
        stage.reset();
}

std::uint64_t reclaimScopePrograms(const std::shared_ptr<PreviewDisplayServiceCore>& core,
                                   const std::uint64_t bytes) noexcept {
    return core->scope.stage != nullptr ? core->scope.stage->reclaimPrograms(bytes) : 0;
}

void retireScopeNative(const std::shared_ptr<PreviewDisplayServiceCore>& core) noexcept {
    // The color step executor and the table pipeline are destroyed on the owner thread; their
    // destructors drain what is unretired.
    core->scope.stage.reset();
}

} // namespace bloom::runtime::detail
