#include <bloom/host/export_capacity_wait.hpp>

#include <algorithm>
#include <optional>
#include <string>

namespace bloom::host {

std::string describeProcessingReservationsV1(const runtime::GpuProcessingPhasePolicy* policy) {
    if (!policy)
        return "reservations=unknown (no shared processing root)";
    const auto held = policy->snapshot().reservations;
    const auto domain = [](const char* name, const bool ready,
                           const std::optional<std::uint64_t>& capacity,
                           const std::uint64_t reserved, const std::uint64_t claimed) {
        std::string text =
            std::string(name) + "{ready=" + (ready ? "1" : "0") +
            ", capacity=" + (capacity ? std::to_string(*capacity) : std::string("unsampled")) +
            ", reserved=" + std::to_string(reserved) + ", claimed=" + std::to_string(claimed);
        if (capacity)
            text += ", free=" + std::to_string(*capacity > claimed ? *capacity - claimed : 0);
        return text + "}";
    };
    return "admittedPhases=" + std::to_string(held.admittedPhases) + "/" +
           std::to_string(held.requestCapacity) + "; " +
           domain("host", held.hostReady, held.hostCapacityBytes, held.hostReservedBytes,
                  held.totalHostClaimBytes) +
           "; " +
           domain("device", held.deviceReady, held.deviceCapacityBytes, held.deviceReservedBytes,
                  held.totalDeviceClaimBytes) +
           "; retainedPhases=" + std::to_string(held.retainedPhases) +
           ", retainedNativeImageBytes=" + std::to_string(held.retainedNativeImageBytes) +
           "; budgetRefusals=" + std::to_string(held.budgetRefusals);
}

bool otherProcessingPhasesAdmittedV1(const runtime::GpuProcessingPhasePolicy* policy,
                                     const std::size_t ownPhases) noexcept {
    return policy != nullptr && policy->snapshot().reservations.admittedPhases > ownPhases;
}

bool processingCapacityHeldElsewhereV1(const runtime::GpuProcessingPhasePolicy* policy,
                                       const std::size_t ownPhases) noexcept {
    if (policy == nullptr)
        return false;
    const auto held = policy->snapshot().reservations;
    // Admitted phases finish and release by themselves. Phases retired to retention, retained
    // native images and (when the caller holds no phase of its own) any remaining claim belong to
    // other work such as background caching or presentation; they give way to a waiting request,
    // so a refusal while they exist is a wait, never proof that the frame cannot fit.
    return held.admittedPhases > ownPhases || held.retainedPhases > 0 ||
           held.retainedNativeImageBytes > 0 ||
           (ownPhases == 0 && (held.totalHostClaimBytes > 0 || held.totalDeviceClaimBytes > 0));
}

namespace {
// #709: the device's room can come back by itself when the device reported but has no usable
// sample (the query failed; never read as zero room), or when its measured room is below the
// device ceiling, which means other programs hold that memory now. Neither is proof that a frame
// cannot fit; admission already treats such memory as returnable (#539).
bool deviceRoomMayReturnV1(const runtime::GpuProcessingPhasePolicy* policy) noexcept {
    if (policy == nullptr)
        return false;
    const auto snapshot = policy->snapshot();
    const auto& reservations = snapshot.reservations;
    const bool deviceUnmeasured =
        snapshot.deviceReports != 0 &&
        (!reservations.deviceReady || !reservations.deviceCapacityBytes);
    const bool deviceHeldOutside =
        reservations.deviceCapacityBytes && reservations.deviceCeilingBytes &&
        *reservations.deviceCapacityBytes < *reservations.deviceCeilingBytes;
    return deviceUnmeasured || deviceHeldOutside;
}
} // namespace

OutputAnalysisCapacityRefusalV1
outputAnalysisCapacityRefusalV1(const OutputAnalysisAttemptOutcomeV1& outcome,
                                const runtime::GpuProcessingPhasePolicy* policy,
                                const std::size_t ownPhases) noexcept {
    const auto* failure = outcome.failure();
    if (!failure || failure->cancelled())
        return OutputAnalysisCapacityRefusalV1::None;
    if (const auto* code = failure->payloadAs<runtime::GpuProcessFrameDiagnosticCode>()) {
        if (*code == runtime::GpuProcessFrameDiagnosticCode::AwaitingCapacity) {
            // Measured capacity with nothing admitted but the export's own reservations: no
            // release in this process can make room, so waiting would only wait on itself until
            // the stall rule. Unmeasured capacity (a sample still pending) keeps waiting.
            // Two device states are never proof either (#709): a device that reported but has
            // no usable sample (the query failed; never read as zero room), and device memory
            // that other programs hold now (the measured room is below the device ceiling): their
            // memory can come back, as admission already assumes (#539).
            if (policy != nullptr && !processingCapacityHeldElsewhereV1(policy, ownPhases) &&
                !deviceRoomMayReturnV1(policy)) {
                const auto reservations = policy->snapshot().reservations;
                if (reservations.hostReady && reservations.hostCapacityBytes)
                    return OutputAnalysisCapacityRefusalV1::NeverFits;
            }
            return OutputAnalysisCapacityRefusalV1::Wait;
        }
        // A refused budget is a wait by the same rule (#709): the device room it was refused
        // against may be memory other programs hold now, as under a parallel test run.
        if (*code == runtime::GpuProcessFrameDiagnosticCode::BudgetRefused)
            return processingCapacityHeldElsewhereV1(policy, ownPhases) ||
                           deviceRoomMayReturnV1(policy)
                       ? OutputAnalysisCapacityRefusalV1::Wait
                       : OutputAnalysisCapacityRefusalV1::NeverFits;
    }
    if (const auto* code = failure->payloadAs<output::OutputAnalysisAttemptErrorCodeV1>();
        code && *code == output::OutputAnalysisAttemptErrorCodeV1::ResourceReservationFailed)
        return OutputAnalysisCapacityRefusalV1::Wait;
    return OutputAnalysisCapacityRefusalV1::None;
}

void OutputAnalysisCapacityWaitV1::refused(const Clock::time_point now,
                                           const runtime::GpuProcessingPhasePolicy* policy,
                                           const std::size_t ownPhases) {
    if (!waiting_) {
        waiting_ = true;
        started_ = now;
        lastActivePhase_ = now;
        backoff_ = kFirstRetry;
    } else {
        backoff_ = std::min(backoff_ * 2, kLongestRetry);
    }
    lastRefusal_ = now;
    if (otherProcessingPhasesAdmittedV1(policy, ownPhases))
        lastActivePhase_ = now;
}

bool OutputAnalysisCapacityWaitV1::retryDue(const Clock::time_point now) const noexcept {
    return waiting_ && now - lastRefusal_ >= backoff_;
}

bool OutputAnalysisCapacityWaitV1::stalled(const Clock::time_point now,
                                           const runtime::GpuProcessingPhasePolicy* policy,
                                           const std::size_t ownPhases) {
    if (!waiting_)
        return false;
    if (otherProcessingPhasesAdmittedV1(policy, ownPhases))
        lastActivePhase_ = now;
    // Device memory coming back from other programs is progress too (#709): while the measured
    // room grows, the wait is not stalled.
    if (policy != nullptr) {
        const auto device = policy->snapshot().reservations.deviceCapacityBytes;
        if (device && lastDeviceCapacity_ && *device > *lastDeviceCapacity_)
            lastActivePhase_ = now;
        lastDeviceCapacity_ = device;
    }
    return now - lastActivePhase_ >= kStallAfter;
}

void OutputAnalysisCapacityWaitV1::succeeded() noexcept {
    if (waiting_)
        waitedTotal_ += lastRefusal_ - started_;
    waiting_ = false;
    backoff_ = kFirstRetry;
    lastDeviceCapacity_.reset();
}

} // namespace bloom::host
