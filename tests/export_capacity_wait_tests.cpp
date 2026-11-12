#include <bloom/host/export_capacity_wait.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <cstdint>
#include <iostream>
#include <memory>

// A capacity refusal during an export is a wait while anything other than the export's own phases
// holds the shared root: admitted phases, but also phases retired to retention (background
// caching) and remaining claims. NeverFits is reserved for a root nothing else holds.
namespace {

using namespace bloom;

int failures = 0;
void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::uint64_t kGib = 1024ULL * 1024 * 1024;
constexpr std::uint64_t kMib = 1024ULL * 1024;

std::shared_ptr<runtime::GpuProcessingPhasePolicy> makePolicy() {
    static const auto retirement = render::PackedHostRetirementOwner::create();
    auto policy = std::make_shared<runtime::GpuProcessingPhasePolicy>(32, retirement);
    runtime::MemoryBudgetState sample;
    sample.machine.availableBytes = 60 * kGib;
    sample.process.status = runtime::ProcessMemoryStatus::Finite;
    sample.process.source = runtime::ProcessMemorySource::LinuxCgroupV2;
    sample.process.sampledAt = runtime::GpuProcessingPhasePolicy::Clock::now();
    sample.process.domains.push_back({.path = "/fixture",
                                      .kind = runtime::ProcessMemoryLimitKind::Finite,
                                      .limitBytes = 8 * kGib,
                                      .currentBytes = 0});
    sample.sampleSerial = 1;
    sample.processSampleFresh = true;
    policy->applyHostSample(sample, 60 * kGib, sample.process.sampledAt);
    return policy;
}

host::OutputAnalysisAttemptOutcomeV1 refusal(const runtime::GpuProcessFrameDiagnosticCode code) {
    return host::OutputAnalysisAttemptOutcomeV1::failure(host::OutputAnalysisAttemptFailureV1(
        host::OutputAnalysisAttemptStageV1::Evaluating, false, code));
}

render::GpuProcessingPhaseGrant hold(runtime::GpuProcessingPhasePolicy& policy) {
    return policy.reserve({.output = runtime::GpuProcessingPhaseOutput::FloatFinal,
                           .pixels = 0,
                           .metadataBytes = 64 * kMib,
                           .sourceFree = true,
                           .native = false});
}

} // namespace

int main() {
    using host::OutputAnalysisCapacityRefusalV1;
    using Code = runtime::GpuProcessFrameDiagnosticCode;
    const auto classify = [](const Code code, const runtime::GpuProcessingPhasePolicy& policy) {
        return host::outputAnalysisCapacityRefusalV1(refusal(code), &policy);
    };

    {
        auto policy = makePolicy();
        check(classify(Code::AwaitingCapacity, *policy) == OutputAnalysisCapacityRefusalV1::NeverFits,
              "a root nothing holds reports NeverFits for an unfittable frame");
        check(classify(Code::BudgetRefused, *policy) == OutputAnalysisCapacityRefusalV1::NeverFits,
              "a budget refusal on an empty root reports NeverFits");
    }
    {
        auto policy = makePolicy();
        auto grant = hold(*policy);
        check(grant.lease != nullptr, "an admitted phase is granted");
        check(classify(Code::AwaitingCapacity, *policy) == OutputAnalysisCapacityRefusalV1::Wait,
              "an admitted phase makes the refusal a wait");
    }
    {
        // Background caching: the phase finished and only its cached product remains. It is no
        // longer an admitted phase, but it still holds capacity and gives way when asked.
        auto policy = makePolicy();
        auto grant = hold(*policy);
        check(grant.lease != nullptr && grant.lease->retireToRetention(),
              "the phase retires to retention");
        check(policy->snapshot().reservations.admittedPhases == 0,
              "a retained phase is not an admitted phase");
        check(policy->snapshot().reservations.retainedPhases == 1, "the retention is counted");
        check(classify(Code::AwaitingCapacity, *policy) == OutputAnalysisCapacityRefusalV1::Wait,
              "retained capacity makes an AwaitingCapacity refusal a wait, not NeverFits");
        check(classify(Code::BudgetRefused, *policy) == OutputAnalysisCapacityRefusalV1::Wait,
              "retained capacity makes a BudgetRefused refusal a wait, not NeverFits");
        const auto text = host::describeProcessingReservationsV1(policy.get());
        check(text.find("retainedPhases=1") != std::string::npos,
              "the log names what holds memory");
    }
    {
        auto policy = makePolicy();
        auto grant = hold(*policy);
        check(host::outputAnalysisCapacityRefusalV1(refusal(Code::AwaitingCapacity), policy.get(),
                                                    1) == OutputAnalysisCapacityRefusalV1::NeverFits,
              "the caller's own phase never counts as other work");
    }
    {
        // #709: a device whose measured room is zero because other programs hold its memory is a
        // wait; that memory can come back.
        auto policy = makePolicy();
        check(policy->reportDevice(7, {.source = render::GpuAllocationBudgetSource::MemoryBudget,
                                       .device_local_bytes = 24 * kGib,
                                       .available_bytes = 0}),
              "a device reports a measured zero");
        check(classify(Code::AwaitingCapacity, *policy) == OutputAnalysisCapacityRefusalV1::Wait,
              "device memory held by other programs makes AwaitingCapacity a wait");
    }
    {
        // #709: a query the driver did not answer is no sample, never zero capacity.
        auto policy = makePolicy();
        check(policy->reportDevice(7, {.source = render::GpuAllocationBudgetSource::Unavailable,
                                       .device_local_bytes = 24 * kGib,
                                       .available_bytes = 0}),
              "a device reports an unanswered query");
        const auto reservations = policy->snapshot().reservations;
        check(!reservations.deviceReady && !reservations.deviceCapacityBytes,
              "an unanswered query leaves the device unsampled, not at zero");
        check(classify(Code::AwaitingCapacity, *policy) == OutputAnalysisCapacityRefusalV1::Wait,
              "an unsampled device makes AwaitingCapacity a wait");
    }
    {
        // A device nothing else holds still reports NeverFits for a frame that waits on itself.
        auto policy = makePolicy();
        check(policy->reportDevice(7, {.source = render::GpuAllocationBudgetSource::MemoryBudget,
                                       .device_local_bytes = 24 * kGib,
                                       .available_bytes = 24 * kGib}),
              "a device reports all of its memory free");
        check(classify(Code::AwaitingCapacity, *policy) == OutputAnalysisCapacityRefusalV1::NeverFits,
              "an idle device and an empty root report NeverFits");
    }
    {
        // Device room that grows while a frame waits restarts the stall clock.
        auto policy = makePolicy();
        check(policy->reportDevice(7, {.source = render::GpuAllocationBudgetSource::MemoryBudget,
                                       .device_local_bytes = 24 * kGib,
                                       .available_bytes = 0}),
              "the stall fixture starts with no room");
        host::OutputAnalysisCapacityWaitV1 wait;
        const auto start = host::OutputAnalysisCapacityWaitV1::Clock::now();
        wait.refused(start, policy.get());
        check(!wait.stalled(start, policy.get()), "a new wait is not stalled");
        const auto late = start + host::OutputAnalysisCapacityWaitV1::kStallAfter;
        check(policy->reportDevice(7, {.source = render::GpuAllocationBudgetSource::MemoryBudget,
                                       .device_local_bytes = 24 * kGib,
                                       .available_bytes = 4 * kGib}),
              "other programs give memory back");
        check(!wait.stalled(late, policy.get()), "returning device memory is progress");
        check(wait.stalled(late + host::OutputAnalysisCapacityWaitV1::kStallAfter, policy.get()),
              "a wait with no progress afterwards still stalls");
    }
    return failures == 0 ? 0 : 1;
}
