// Upload lookahead for the GPU scene executor (#405).
//
// The executor runs its plan one native job at a time. Uploads are leaves: their input is the
// already-converted host image the scene carries, never another step's output. So while any native
// job is in flight, the next planned Upload step can already be begun on a second GpuImageUpload
// pipeline: its host staging copy runs on the owner thread (and the row-band pool) while the GPU
// executes the job in front of it, and its copy is queued behind that job. When the cursor reaches
// the step, the two pipelines swap and the step completes through the ordinary Upload path: the
// same proven fence, descriptor check, live-pin charge and cache insertion, in plan order.
//
// Ordering: both submissions go to the device's one queue in submission order. The upload writes a
// freshly allocated image, and its final barrier (transfer write -> compute read, to GENERAL)
// orders it before every consumer, which is only submitted after the upload's fence is observed. No
// semaphore or second queue is needed, so a device without a separate transfer queue takes exactly
// this path.
//
// Budget: the lookahead begins only within the request budget left beside the live pins and the
// in-flight job's actual bytes; its own actual bytes stay reserved (remainingBudget()) until its
// step adopts it. A later step refused beside that reservation drops the lookahead, waits for its
// fence, and is retried on the serial path with lookahead off for the rest of the job, so the
// lookahead never fails a scene the serial executor would have run.

#include "gpu_scene_executor_private.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace bloom::runtime {

std::optional<std::uint64_t> GpuSceneExecutor::Impl::inFlightNativeBytes() const noexcept {
    // Only kinds whose actual bytes are known once begun. Coverage, OCIO and Cryptomatte jobs run
    // without a lookahead beside them.
    switch (nativeKind) {
    case NativeKind::Solid:
        return solid->lastJobAllocationBytes();
    case NativeKind::Composite:
        return composite->lastJobAllocationBytes();
    case NativeKind::Upload:
        return upload->lastJobAllocationBytes();
    case NativeKind::Affine:
        return affine->lastJobAllocationBytes();
    case NativeKind::Blend:
        return blend->lastJobAllocationBytes();
    case NativeKind::PointResample:
        return pointResample->lastJobAllocationBytes();
    case NativeKind::Batch:
        return operationBatch
                   ? std::optional<std::uint64_t>(operationBatch->statistics().peakAllocationBytes)
                   : std::nullopt;
    default:
        return std::nullopt;
    }
}

void GpuSceneExecutor::Impl::swapUploadPipelines() noexcept {
    std::swap(upload, lookaheadUpload);
    std::swap(uploadRan, lookaheadUploadRan);
}

void GpuSceneExecutor::Impl::startLookahead() noexcept {
    if (!budgets.enableUploadLookahead || lookaheadDisabled || lookaheadOutstanding() ||
        lookaheadDiscarding || !nativeInFlight || cancelRequested || timeoutRequested ||
        drainRequired || deviceLost || state != GpuSceneExecutorJobState::Pending ||
        scene == nullptr || !lookaheadUpload || !upload) {
        return;
    }
    if (lookaheadUpload->hasUnretiredSubmission() ||
        lookaheadUpload->state() == render::GpuImageUploadJobState::Pending ||
        lookaheadUpload->state() == render::GpuImageUploadJobState::Ready) {
        return;
    }
    const auto inFlight = inFlightNativeBytes();
    if (!inFlight.has_value()) {
        return;
    }
    // The first Upload step after the job in flight. A batch holds [cursor, cursor + count).
    const std::size_t inFlightEnd =
        cursor + (nativeKind == NativeKind::Batch ? std::max<std::size_t>(batchStepCount, 1) : 1);
    lookaheadScan = std::max(lookaheadScan, inFlightEnd);
    while (lookaheadScan < steps.size() &&
           steps[lookaheadScan].kind != GpuSceneExecutorStepKind::Upload) {
        ++lookaheadScan;
    }
    if (lookaheadScan >= steps.size()) {
        return;
    }
    const GpuSceneExecutorStep& step = steps[lookaheadScan];
    if (step.uploadSource == nullptr && step.uploadSamples == nullptr) {
        return;
    }
    const std::uint64_t held = liveBytes > std::numeric_limits<std::uint64_t>::max() - *inFlight
                                   ? std::numeric_limits<std::uint64_t>::max()
                                   : liveBytes + *inFlight;
    if (held >= requestBudget) {
        return;
    }
    const std::uint64_t headroom = std::min(requestBudget - held, budgets.maxImageBytes);
    try {
        const auto submitStarted = std::chrono::steady_clock::now();
        const auto native = lookaheadUpload->begin(uploadParameters(step), headroom);
        if (native.code != render::GpuImageUploadDiagnosticCode::None) {
            // Pre-submit refusal (budget, pool, allocation): the step runs on the serial path.
            lookaheadDisabled = true;
            return;
        }
        lookaheadUploadRan = true;
        lookaheadStep = lookaheadScan;
        lookaheadBytes = lookaheadUpload->lastJobAllocationBytes();
        lookaheadReady = false;
        lookaheadStart = std::chrono::steady_clock::now();
        ++counters.uploads;
        ++counters.uploadsAhead;
        ++counters.dispatches;
        ++counters.nativeSubmissions;
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(lookaheadStart - submitStarted)
                .count();
        if (elapsed > 0)
            counters.uploadSubmitNanoseconds += std::min(static_cast<std::uint64_t>(elapsed),
                                                         std::numeric_limits<std::uint64_t>::max() -
                                                             counters.uploadSubmitNanoseconds);
        peakLiveBytes = std::max(peakLiveBytes, held + lookaheadBytes);
        counters.peakLiveImageBytes = peakLiveBytes;
    } catch (...) {
        // uploadParameters() may throw bad_alloc building the copy callback; nothing was begun.
        lookaheadDisabled = true;
    }
}

void GpuSceneExecutor::Impl::pollLookahead() noexcept {
    if (!lookaheadOutstanding() || lookaheadReady || !lookaheadUpload) {
        return;
    }
    // Non-blocking. Once its fence is proven the staging is gone, so only the resident image stays
    // reserved. A failure is left for the step to observe on adoption, as the serial path would.
    if (lookaheadUpload->poll() == render::GpuImageUploadPollResult::Ready) {
        lookaheadReady = true;
        if (const auto* const image = lookaheadUpload->image(); image != nullptr)
            lookaheadBytes = image->allocationBytes();
    }
}

bool GpuSceneExecutor::Impl::adoptLookahead() noexcept {
    if (!lookaheadOutstanding() || lookaheadStep != cursor || nativeInFlight || !lookaheadUpload ||
        (upload && (upload->hasUnretiredSubmission() ||
                    upload->state() == render::GpuImageUploadJobState::Pending ||
                    upload->state() == render::GpuImageUploadJobState::Ready))) {
        return false;
    }
    swapUploadPipelines();
    nativeKind = NativeKind::Upload;
    nativeInFlight = true;
    cancelIssued = false;
    timeoutRequested = false;
    // The deadline and fence observation run from the lookahead's own submission.
    nativeStart = lookaheadStart;
    liveBytesAtDispatch = liveBytes;
    lookaheadStep = kNoLookahead;
    lookaheadBytes = 0;
    lookaheadReady = false;
    return true;
}

bool GpuSceneExecutor::Impl::dropLookahead() noexcept {
    if (!lookaheadUpload) {
        lookaheadStep = kNoLookahead;
        lookaheadBytes = 0;
        lookaheadReady = false;
        lookaheadDiscarding = false;
        return true;
    }
    const bool outstanding = lookaheadOutstanding();
    if (outstanding)
        lookaheadUpload->cancel();
    if (outstanding || lookaheadUpload->hasUnretiredSubmission()) {
        if (lookaheadUpload->poll() == render::GpuImageUploadPollResult::Ready)
            static_cast<void>(lookaheadUpload->takeImage());
    }
    if (lookaheadUpload->hasUnretiredSubmission()) {
        return false;
    }
    if (outstanding)
        ++counters.uploadsAheadDropped;
    lookaheadStep = kNoLookahead;
    lookaheadBytes = 0;
    lookaheadReady = false;
    lookaheadDiscarding = false;
    // Keep the pipeline that already holds a bounded resident slot on the serial path.
    if (!uploadRan && lookaheadUploadRan)
        swapUploadPipelines();
    return true;
}

} // namespace bloom::runtime
