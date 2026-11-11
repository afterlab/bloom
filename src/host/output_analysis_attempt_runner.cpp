#include <bloom/host/output_analysis_attempt_runner.hpp>

#include "output_analysis_attempt_color_private.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/runtime/channel_layers.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_process_frame.hpp>
#include <bloom/runtime/gpu_processing_phase_policy.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace bloom::host {

namespace {

// Plain names for the log: a frame that takes the CPU reference path always says why (#553).
const char* frameStatusName(const runtime::GpuProcessFrameStatus status) noexcept {
    using Status = runtime::GpuProcessFrameStatus;
    switch (status) {
    case Status::Evaluated:
        return "evaluated";
    case Status::Disabled:
        return "GPU rendering is disabled";
    case Status::DeviceUnavailable:
        return "no usable GPU device";
    case Status::UnsupportedGpuSubset:
        return "the composition is outside the GPU-prepared subset";
    case Status::Cancelled:
        return "cancelled";
    case Status::Failed:
        return "the GPU render failed";
    }
    return "unknown status";
}

const char* frameCodeName(const runtime::GpuProcessFrameDiagnosticCode code) noexcept {
    using Code = runtime::GpuProcessFrameDiagnosticCode;
    switch (code) {
    case Code::None:
        return "none";
    case Code::Disabled:
        return "disabled";
    case Code::LoaderUnavailable:
        return "loader-unavailable";
    case Code::DeviceUnavailable:
        return "device-unavailable";
    case Code::SceneUnsupported:
        return "scene-unsupported";
    case Code::InvalidArgument:
        return "invalid-argument";
    case Code::InvalidRequest:
        return "invalid-request";
    case Code::OverBudget:
        return "over-budget";
    case Code::AwaitingCapacity:
        return "awaiting-capacity";
    case Code::BudgetRefused:
        return "budget-refused";
    case Code::DeadlineExceeded:
        return "deadline-exceeded";
    case Code::Cancelled:
        return "cancelled";
    case Code::DeviceLost:
        return "device-lost";
    case Code::ReadbackFailed:
        return "readback-failed";
    case Code::ReadbackOverBudget:
        return "readback-over-budget";
    case Code::CacheUnavailable:
        return "cache-unavailable";
    case Code::ExecutorUnavailable:
        return "executor-unavailable";
    case Code::BadAllocation:
        return "bad-allocation";
    case Code::InternalInvariant:
        return "internal-invariant";
    }
    return "unknown";
}

} // namespace

OutputMediaDisplayCommandV1::OutputMediaDisplayCommandV1(
    std::shared_ptr<const output::PreparedOutputDisplayV1> display,
    const runtime::EvaluationColorIntent& intent)
    : display_(std::move(display)), workingColorSpaceId_(intent.workingColorSpaceId),
      ocioConfigRevision_(intent.ocioConfigRevision), ocioConfigUri_(intent.ocioConfigUri) {
    if (display_ == nullptr) {
        throw std::invalid_argument("a media display command needs the export display");
    }
}

std::shared_ptr<const runtime::PreparedGpuOcioCommand>
OutputMediaDisplayCommandV1::command(GpuExportProvider& provider,
                                     const runtime::GpuOcioCommandGeometry geometry,
                                     const runtime::GpuOcioCancellation& cancel) {
    const std::lock_guard lock(mutex_);
    if (attempted_) {
        return command_ != nullptr && command_->geometry().width == geometry.width &&
                       command_->geometry().height == geometry.height
                   ? command_
                   : nullptr;
    }
    if (!provider.gpuDisplayPreparationAvailable()) {
        // The bootstrap may still publish the shared OCIO context; try again on a later frame.
        unavailableReason_ = "GPU color preparation is unavailable on this provider";
        return nullptr;
    }
    const runtime::EvaluationColorIntent intent{.workingColorSpaceId = workingColorSpaceId_,
                                                .ocioConfigRevision = ocioConfigRevision_,
                                                .ocioConfigUri = ocioConfigUri_};
    auto config = output::resolveOutputDisplayConfigV1(intent);
    if (!config) {
        attempted_ = true;
        unavailableReason_ = "the output OCIO config did not resolve";
        return nullptr;
    }
    auto prepared = provider.prepareGpuMediaDisplayCommand(*config, display_->displayName(),
                                                           display_->viewName(), geometry, cancel);
    if (cancel && cancel()) {
        return nullptr;
    }
    attempted_ = true;
    if (!prepared.hasValue()) {
        unavailableReason_ = "GPU display preparation failed: " + prepared.diagnostic;
        return nullptr;
    }
    command_ = std::move(prepared.command);
    unavailableReason_.clear();
    return command_;
}

core::Sha256Digest OutputMediaDisplayCommandV1::commandIdentity() const {
    const std::lock_guard lock(mutex_);
    return command_ != nullptr ? command_->identity() : core::Sha256Digest{};
}

std::string OutputMediaDisplayCommandV1::unavailableReason() const {
    const std::lock_guard lock(mutex_);
    return unavailableReason_;
}

namespace {

// Everything the blocking stage hands the Cpu stage, behind ONE shared_ptr: runtime::
// TaskResultValue caps a task result at four pointers (task_scheduler.hpp), and the retained
// target plus the PNG display products exceed that on their own. Bundling them keeps the task
// result a single small handle while still transferring both products by value semantics.
struct ResolvedAttemptInputsV1 final {
    output::OutputAnalysisAttemptTargetV1 target;
    detail::ColorResolutionOutcomeV1 color;
};

struct ResolvingOutcomeV1 final {
    bool succeeded = false;
    // Distinguishes an allocation/internal failure of the PNG color stage (which fails the attempt
    // AT ColorPreparing) from a target-preflight failure (which fails it at Resolving). A merely
    // non-Ready color resolution is neither: it travels in `resolved->color` to the analyzer.
    bool colorStageFailed = false;
    platform::StagedArtifactError error = platform::StagedArtifactError::None;
    std::shared_ptr<const ResolvedAttemptInputsV1> resolved = nullptr;
};
static_assert(runtime::TaskResultValue<ResolvingOutcomeV1>);

enum class BuildFailureKindV1 : std::uint8_t {
    None,
    Evaluation,
    Identity,
    Analyzer,
    AttemptBuild,
    ProcessingAdmission,
};

// The successful Evaluating/Identifying/Analyzing product, heap-shared as one handle so the task
// result stays within TaskResultValue's 4-pointer cap even though the retained native provenance
// carries the full counter set.
struct BuildProductV1 final {
    std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt;
    OutputAnalysisAttemptGpuProvenanceV1 gpuProvenance;
    // Set on a failed evaluation only (attempt is null): the evaluator's technical reason and
    // subject (the asset's display name).
    std::string failureMessage;
    std::string failureSubject;
};

struct BuildOutcomeV1 final {
    bool succeeded = false;
    BuildFailureKindV1 failureKind = BuildFailureKindV1::None;
    std::uint8_t rawCode = 0;
    std::shared_ptr<const BuildProductV1> product = nullptr;
};
static_assert(runtime::TaskResultValue<BuildOutcomeV1>);

[[nodiscard]] OutputAnalysisAttemptFailureV1
translateBuildFailure(const BuildOutcomeV1& outcome) {
    switch (outcome.failureKind) {
    case BuildFailureKindV1::Evaluation:
        return OutputAnalysisAttemptFailureV1{
            OutputAnalysisAttemptStageV1::Evaluating, false,
            static_cast<runtime::EvaluationDiagnosticCode>(outcome.rawCode)}
            .withDiagnostic(outcome.product ? outcome.product->failureSubject : std::string{},
                            outcome.product ? outcome.product->failureMessage : std::string{});
    case BuildFailureKindV1::ProcessingAdmission:
        return {OutputAnalysisAttemptStageV1::Evaluating, false,
                static_cast<runtime::GpuProcessFrameDiagnosticCode>(outcome.rawCode)};
    case BuildFailureKindV1::Identity:
        return {OutputAnalysisAttemptStageV1::Identifying, false,
                static_cast<output::ProcessFrameSemanticIdentityErrorCode>(outcome.rawCode)};
    case BuildFailureKindV1::Analyzer:
        return {OutputAnalysisAttemptStageV1::Analyzing, false,
                static_cast<output::OutputAnalysisAnalyzerErrorCodeV1>(outcome.rawCode)};
    case BuildFailureKindV1::AttemptBuild:
        return {OutputAnalysisAttemptStageV1::Analyzing, false,
                static_cast<output::OutputAnalysisAttemptErrorCodeV1>(outcome.rawCode)};
    case BuildFailureKindV1::None:
        break;
    }
    return {OutputAnalysisAttemptStageV1::Analyzing, false, std::monostate{}};
}

[[nodiscard]] const char* ownerKindName(const runtime::TaskOwnerKind kind) noexcept {
    switch (kind) {
    case runtime::TaskOwnerKind::Application:
        return "application";
    case runtime::TaskOwnerKind::Project:
        return "project";
    case runtime::TaskOwnerKind::Composition:
        return "composition";
    case runtime::TaskOwnerKind::PanelRequest:
        return "panel";
    case runtime::TaskOwnerKind::Export:
        return "export";
    }
    return "unknown";
}

// The current UTC time to the millisecond, as 2026-01-02T03:04:05.678Z, for diagnostics.
[[nodiscard]] std::string utcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
    const std::time_t time = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::array<char, 40> text{};
    std::snprintf(text.data(), text.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                  utc.tm_sec, static_cast<int>(milliseconds));
    return text.data();
}

[[nodiscard]] runtime::TaskOwner attemptOwner(const runtime::TaskOwner requested) noexcept {
    return requested.isValid() ? requested
                               : runtime::TaskOwner{.kind = runtime::TaskOwnerKind::Export,
                                                    .id = runtime::TaskOwnerId::fromRaw(1)};
}

} // namespace

OutputAnalysisAttemptOutcomeV1 OutputAnalysisAttemptOutcomeV1::completed(
    std::shared_ptr<const output::OutputAnalysisAttemptV1> attempt,
    std::optional<OutputAnalysisAttemptGpuProvenanceV1> gpuProvenance) noexcept {
    OutputAnalysisAttemptOutcomeV1 outcome;
    outcome.attempt_ = std::move(attempt);
    outcome.gpuProvenance_ = gpuProvenance;
    return outcome;
}

OutputAnalysisAttemptOutcomeV1
OutputAnalysisAttemptOutcomeV1::failure(OutputAnalysisAttemptFailureV1 failureValue) noexcept {
    OutputAnalysisAttemptOutcomeV1 outcome;
    outcome.failure_.emplace(failureValue); // trivially copyable; std::move() would be a no-op
    return outcome;
}

namespace {

template <typename> struct PayloadName;
template <> struct PayloadName<runtime::TaskSubmissionStatus> {
    static constexpr std::string_view value = "TaskSubmissionStatus";
};
template <> struct PayloadName<platform::StagedArtifactError> {
    static constexpr std::string_view value = "StagedArtifactError";
};
template <> struct PayloadName<runtime::EvaluationDiagnosticCode> {
    static constexpr std::string_view value = "EvaluationDiagnosticCode";
};
template <> struct PayloadName<runtime::GpuProcessFrameDiagnosticCode> {
    static constexpr std::string_view value = "GpuProcessFrameDiagnosticCode";
};
template <> struct PayloadName<output::ProcessFrameSemanticIdentityErrorCode> {
    static constexpr std::string_view value = "ProcessFrameSemanticIdentityErrorCode";
};
template <> struct PayloadName<output::OutputAnalysisAnalyzerErrorCodeV1> {
    static constexpr std::string_view value = "OutputAnalysisAnalyzerErrorCodeV1";
};
template <> struct PayloadName<output::OutputAnalysisAttemptErrorCodeV1> {
    static constexpr std::string_view value = "OutputAnalysisAttemptErrorCodeV1";
};

std::string_view gpuCodeName(const runtime::GpuProcessFrameDiagnosticCode code) noexcept {
    using C = runtime::GpuProcessFrameDiagnosticCode;
    switch (code) {
    case C::AwaitingCapacity:
        return "AwaitingCapacity";
    case C::BudgetRefused:
        return "BudgetRefused";
    case C::OverBudget:
        return "OverBudget";
    case C::DeviceUnavailable:
        return "DeviceUnavailable";
    case C::DeviceLost:
        return "DeviceLost";
    case C::InvalidRequest:
        return "InvalidRequest";
    default:
        return {};
    }
}

} // namespace

std::string describeOutputAnalysisAttemptV1(const OutputAnalysisAttemptOutcomeV1& outcome) {
    static constexpr std::array<std::string_view, 5> stages{
        "resolving", "color-preparing", "evaluating", "identifying", "analyzing"};
    std::string detail;
    const auto append = [&detail](std::string_view text) {
        if (!detail.empty())
            detail += "; ";
        detail += text;
    };
    if (const auto* failure = outcome.failure()) {
        const auto stage = static_cast<std::size_t>(failure->stage());
        append("stage=" +
               (stage < stages.size() ? std::string(stages[stage]) : std::to_string(stage)));
        std::visit(
            [&](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (!std::is_same_v<Value, std::monostate>) {
                    std::string text = "code=" + std::string(PayloadName<Value>::value) + ":" +
                                       std::to_string(static_cast<unsigned>(value));
                    if constexpr (std::is_same_v<Value, runtime::GpuProcessFrameDiagnosticCode>)
                        if (const auto name = gpuCodeName(value); !name.empty())
                            text += " (" + std::string(name) + ")";
                    append(text);
                }
            },
            failure->payload());
        if (!failure->subject().empty())
            append("asset=" + failure->subject());
        if (!failure->message().empty())
            append("reason=" + failure->message());
        if (failure->cancelled())
            append("cancelled");
    }
    if (const auto attempt = outcome.attempt()) {
        append(attempt->approvable() ? "approvable" : "not approvable");
        if (const auto& report = attempt->report())
            for (const auto& facet : report->view().facets)
                append("facet " + std::string(output::outputFacetDisplayNameV1(facet.facet)) + "=" +
                       std::to_string(static_cast<unsigned>(facet.state)) + ":" +
                       std::to_string(static_cast<unsigned>(facet.stableCode)));
    }
    return detail.empty() ? std::string("no attempt") : detail;
}

namespace detail {

struct OutputAnalysisAttemptRunnerState final {
    runtime::TaskScheduler* scheduler = nullptr;
    platform::StagedArtifactCoordinator* artifacts = nullptr;
    output::ExportResourceLedgerV1* ledger = nullptr;
    OutputAnalysisAttemptRequestV1 request;
    OutputAnalysisAttemptStageV1 stage = OutputAnalysisAttemptStageV1::Resolving;
    runtime::TaskHandle<ResolvingOutcomeV1> resolvingHandle;
    std::optional<runtime::TaskHandle<BuildOutcomeV1>> buildHandle;
    // Set once Resolving succeeds and cleared when the Cpu evaluation task is submitted. While it
    // is set, the runner may be deferring on the GPU provider's lazy bootstrap.
    std::shared_ptr<const ResolvedAttemptInputsV1> resolvedInputs;
    bool completed = false;
};

} // namespace detail

OutputAnalysisAttemptRunnerV1::OutputAnalysisAttemptRunnerV1(
    OutputAnalysisAttemptRunnerV1&&) noexcept = default;
OutputAnalysisAttemptRunnerV1&
OutputAnalysisAttemptRunnerV1::operator=(OutputAnalysisAttemptRunnerV1&&) noexcept = default;

OutputAnalysisAttemptRunnerV1::OutputAnalysisAttemptRunnerV1(
    std::unique_ptr<detail::OutputAnalysisAttemptRunnerState> state) noexcept
    : state_(std::move(state)) {}

OutputAnalysisAttemptRunnerV1::~OutputAnalysisAttemptRunnerV1() {
    if (state_ && !state_->completed) {
        requestCancellation();
    }
}

bool OutputAnalysisAttemptRunnerV1::isReady() const noexcept {
    if (!state_ || state_->completed || state_->scheduler == nullptr) {
        return false;
    }
    const auto id =
        state_->buildHandle.has_value() ? state_->buildHandle->id() : state_->resolvingHandle.id();
    const auto snapshot = state_->scheduler->snapshot(id);
    return snapshot.has_value() && runtime::isTerminal(snapshot->state);
}

void OutputAnalysisAttemptRunnerV1::requestCancellation() noexcept {
    if (!state_) {
        return;
    }
    if (state_->buildHandle.has_value()) {
        state_->buildHandle->cancel();
    } else {
        state_->resolvingHandle.cancel();
    }
}

std::optional<OutputAnalysisAttemptOutcomeV1> OutputAnalysisAttemptRunnerV1::tryComplete() {
    if (!state_ || state_->completed) {
        return std::nullopt;
    }

    if (!state_->buildHandle.has_value()) {
        if (state_->resolvedInputs == nullptr) {
            // Still waiting on / just reached the Resolving stage.
            auto taken = state_->resolvingHandle.tryTakeResult();
            if (!taken.has_value()) {
                return std::nullopt;
            }
            if (taken->state() == runtime::TaskState::Cancelled) {
                state_->completed = true;
                return OutputAnalysisAttemptOutcomeV1::failure(
                    {OutputAnalysisAttemptStageV1::Resolving, true, std::monostate{}});
            }
            if (taken->state() != runtime::TaskState::Succeeded || !taken->value().has_value() ||
                !taken->value()->succeeded) {
                state_->completed = true;
                if (taken->value().has_value() && taken->value()->colorStageFailed) {
                    return OutputAnalysisAttemptOutcomeV1::failure(
                        {OutputAnalysisAttemptStageV1::ColorPreparing, false, std::monostate{}});
                }
                const auto error = taken->value().has_value() ? taken->value()->error
                                                              : platform::StagedArtifactError::None;
                return OutputAnalysisAttemptOutcomeV1::failure(
                    {OutputAnalysisAttemptStageV1::Resolving, false, error});
            }
            // Resolving succeeded: retain its typed result (the target, and for PNG the retained
            // display products) until the evaluation stage is actually submitted -- possibly after
            // a bounded GPU-bootstrap deferral -- then consume it directly into the next task's
            // closure, never a wait/get/join.
            state_->resolvedInputs = taken->value()->resolved;
            if (state_->resolvedInputs == nullptr) {
                state_->completed = true;
                return OutputAnalysisAttemptOutcomeV1::failure(
                    {OutputAnalysisAttemptStageV1::Resolving, false, std::monostate{}});
            }
        }

        // GPU bootstrap gate: defer the evaluation stage until the provider's lazy bootstrap is
        // terminal, so the first frame of an export on a supported device is genuinely GPU. This
        // is owner-driven -- tryComplete() simply reports "not yet" and the authoring surface's
        // existing poll loop re-drives it -- so no worker is blocked and no CPU task is submitted
        // while waiting. There is no time bound: a fixed 5 s bound let frame 0 race a bootstrap
        // that was queued behind other provider roots and render on the CPU. The bootstrap itself
        // ends on failure, cancellation or shutdown (prepared() is then true and an unavailable
        // device takes the labelled CPU fallback), and the artist can still stop the export.
        const auto& provider = state_->request.gpuProvider;
        if (provider != nullptr && !provider->prepared()) {
            return std::nullopt;
        }

        auto resolved = std::move(state_->resolvedInputs);
        state_->resolvedInputs.reset();
        const auto preset = state_->request.preset;
        auto* ledger = state_->ledger;
        auto gpuProvider = state_->request.gpuProvider; // shared ownership travels with the task
        auto processingPolicy = gpuProvider ? gpuProvider->processingPhasePolicy()
                                            : state_->request.processingPhasePolicy;
        // The command the combined readback will run. The canonical command was prepared on the
        // Resolving CPU task from the EXACT resolved config/working space/display/view and the
        // exact data-window geometry. `request.outputColorCommand` is an explicit test seam and is
        // accepted ONLY when it byte-identifies that canonical command; any other command (wrong
        // transform, stale config revision, or a command prepared for a different frame) is
        // refused, and the attempt falls back honestly to the CPU reference/display path instead of
        // laundering foreign pixels under the canonical display identity.
        const auto& canonicalCommand = resolved->color.gpuDisplayCommand;
        const auto& requestedCommand = state_->request.outputColorCommand;
        const bool refusedRequestedCommand =
            requestedCommand != nullptr &&
            (canonicalCommand == nullptr ||
             requestedCommand->identity() != canonicalCommand->identity());
        const bool forceCpuFallback = refusedRequestedCommand;
        auto outputColorCommand =
            refusedRequestedCommand
                ? nullptr
                : (requestedCommand != nullptr ? requestedCommand : canonicalCommand);
        auto plan = state_->request.plan;
        auto evaluation = state_->request.evaluation;
        // The media display command rides the same combined readback when no canonical PNG
        // command exists and the frame is the full composition extent.
        auto mediaDisplay = state_->request.mediaDisplay;
        if (mediaDisplay != nullptr &&
            (outputColorCommand != nullptr || forceCpuFallback ||
             preset == output::OutputPresetV1::PngRgba8SrgbV1 || plan == nullptr ||
             evaluation.roi.has_value() ||
             !std::holds_alternative<runtime::CompositionFormatResolution>(
                 evaluation.resolution))) {
            mediaDisplay.reset();
        }
        const bool useRootPixelCapacity = state_->request.useRootPixelCapacity;
        auto assetBaseDirectory = state_->request.assetBaseDirectory;
        auto fallbackEvaluator = state_->request.fallbackEvaluator;

        const auto requestOwner = attemptOwner(state_->request.owner);
        const std::string requestLabel = std::string("request=") +
                                         ownerKindName(requestOwner.kind) + ":" +
                                         std::to_string(requestOwner.id.value()) +
                                         " time=" + std::to_string(evaluation.time.numerator()) +
                                         "/" + std::to_string(evaluation.time.denominator());
        runtime::TaskRequest cpuRequest(
            "Analyze a frame export attempt", attemptOwner(state_->request.owner),
            runtime::TaskPriority::Foreground, runtime::TaskExecutor::Cpu);
        cpuRequest.usesPurgeableCaches = true;
        auto submission = state_->scheduler->submit<BuildOutcomeV1>(
            std::move(cpuRequest),
            [plan = std::move(plan), evaluation, useRootPixelCapacity,
             assetBaseDirectory = std::move(assetBaseDirectory),
             fallbackEvaluator = std::move(fallbackEvaluator), resolved, preset, ledger,
             gpuProvider = std::move(gpuProvider), processingPolicy = std::move(processingPolicy),
             forceCpuFallback, outputColorCommand = std::move(outputColorCommand),
             mediaDisplay = std::move(mediaDisplay), displayName = state_->request.displayName,
             viewName = state_->request.viewName, requestLabel](
                runtime::TaskContext& context) mutable -> runtime::TaskResult<BuildOutcomeV1> {
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }

                context.reportProgress(
                    {.phase = "Evaluating", .subphase = "", .completed = 0, .total = std::nullopt});
                // GPU final-render bridge: when an available device and a prepared-GPU-subset scene
                // exist, evaluate through the genuine native scene executor and read the final
                // RGBA32F back exactly once. An unavailable device, a disabled provider, or a
                // scene outside the subset falls through to the CPU reference evaluator on the
                // same snapshot/identity. The typed native provenance/counters are retained for
                // diagnostics; they never alter the analysis or the approval digest. A refused
                // requested command forces the full CPU reference path: the identity arm must never
                // silently substitute for the display transform the caller asked to run.
                OutputAnalysisAttemptGpuProvenanceV1 gpuProvenance;
                output::OutputAnalysisAttemptGpuDisplayV1 gpuDisplay;
                output::OutputAnalysisAttemptGpuMediaDisplayV1 gpuMediaDisplay;
                std::shared_ptr<render::GpuProcessingPhaseLease> hostPhase;
                std::shared_ptr<const runtime::ProcessFrame> evaluatedFrame;
                bool frameOnGpu = false;
                const auto admissionFailure =
                    [](const runtime::GpuProcessFrameDiagnosticCode code) {
                        return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                            {.succeeded = false,
                             .failureKind = BuildFailureKindV1::ProcessingAdmission,
                             .rawCode = static_cast<std::uint8_t>(code)});
                    };
                if (!processingPolicy) {
                    return admissionFailure(runtime::GpuProcessFrameDiagnosticCode::InvalidRequest);
                }
                if (gpuProvider) {
                    gpuProvider->refreshProcessingCapacity(context.cancellation());
                }
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }
                if (useRootPixelCapacity) {
                    const auto capacity = processingPolicy->snapshot().reservations;
                    if (!capacity.hostReady || !capacity.hostCapacityBytes)
                        return admissionFailure(
                            runtime::GpuProcessFrameDiagnosticCode::AwaitingCapacity);
                    evaluation.pixelStorageByteLimit =
                        static_cast<std::size_t>(std::min<std::uint64_t>(
                            *capacity.hostCapacityBytes, std::numeric_limits<std::size_t>::max()));
                }
                std::shared_ptr<const runtime::PreparedGpuOcioCommand> mediaDisplayCommand;
                if (gpuProvider != nullptr && mediaDisplay != nullptr) {
                    mediaDisplayCommand = mediaDisplay->command(
                        *gpuProvider, {plan->format().width(), plan->format().height()},
                        [&context] { return context.isCancellationRequested(); });
                    if (mediaDisplayCommand != nullptr) {
                        outputColorCommand = mediaDisplayCommand;
                    }
                }
                if (gpuProvider != nullptr && !forceCpuFallback) {
                    auto evaluator = gpuProvider->evaluator();
                    if (evaluator != nullptr) {
                        auto gpuOutcome =
                            evaluator->evaluate(plan, evaluation, context.cancellation(), {},
                                                outputColorCommand, assetBaseDirectory);
                        if (context.isCancellationRequested() ||
                            gpuOutcome.status == runtime::GpuProcessFrameStatus::Cancelled) {
                            return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                        }
                        // A capacity wait or refusal returns to the export runner, which waits
                        // at the frame boundary (ADR 0033 section 9) and retries; the root's
                        // counters are logged so a refusal is diagnosable from the field.
                        if (gpuOutcome.diagnostic.code ==
                                runtime::GpuProcessFrameDiagnosticCode::AwaitingCapacity ||
                            gpuOutcome.diagnostic.code ==
                                runtime::GpuProcessFrameDiagnosticCode::BudgetRefused) {
                            if (gpuOutcome.diagnostic.code ==
                                runtime::GpuProcessFrameDiagnosticCode::BudgetRefused)
                                std::clog
                                    << "Bloom export: frame refused: "
                                    << gpuOutcome.diagnostic.message << "; "
                                    << runtime::describeAdmission(
                                           {.status =
                                                render::GpuProcessingPhaseAdmission::BudgetRefused,
                                            .domain = render::GpuProcessingPhaseDomain::None,
                                            .lease = {},
                                            .admissionRevision = 0},
                                           processingPolicy->usableCapacity())
                                    << '\n';
                            return admissionFailure(gpuOutcome.diagnostic.code);
                        }
                        // #648: the line names when it happened and which request and frame
                        // time it belongs to, so each one can be matched to its render.
                        if (gpuOutcome.status != runtime::GpuProcessFrameStatus::Evaluated)
                            std::clog << "Bloom export: frame taking the CPU reference path: "
                                      << utcTimestamp() << " " << requestLabel << ": "
                                      << "status " << static_cast<int>(gpuOutcome.status)
                                      << ", code " << static_cast<int>(gpuOutcome.diagnostic.code)
                                      << ": " << gpuOutcome.diagnostic.message << '\n';
                        gpuProvenance.status = gpuOutcome.status;
                        gpuProvenance.counters = gpuOutcome.counters;
                        gpuProvenance.deviceOwnershipEpoch = gpuOutcome.deviceOwnershipEpoch;
                        gpuProvenance.encodedArm = gpuOutcome.encodedArm;
                        gpuProvenance.readbackSubmissions =
                            gpuOutcome.outputColorCounters.readbackSubmissions;
                        gpuProvenance.transferredPayloads =
                            gpuOutcome.outputColorCounters.transferredPayloads;
                        gpuProvenance.processPayloadBytes =
                            gpuOutcome.outputColorCounters.processPayloadBytes;
                        gpuProvenance.encodedPayloadBytes =
                            gpuOutcome.outputColorCounters.encodedPayloadBytes;
                        gpuProvenance.outputCommandIdentity = gpuOutcome.outputCommandIdentity;
                        if (gpuOutcome.status != runtime::GpuProcessFrameStatus::Evaluated)
                            gpuProvenance.cpuFallbackReason =
                                std::string(frameStatusName(gpuOutcome.status)) + " (" +
                                frameCodeName(gpuOutcome.diagnostic.code) + "): " +
                                gpuOutcome.diagnostic.message;
                        if (gpuOutcome.status == runtime::GpuProcessFrameStatus::Evaluated) {
                            evaluatedFrame = gpuOutcome.frame;
                            frameOnGpu = evaluatedFrame != nullptr;
                            // Retain the verified display payload, bound to the exact process
                            // geometry and the SAME canonical display identity the attempt retains.
                            const auto* descriptor =
                                gpuOutcome.frame != nullptr
                                    ? gpuOutcome.frame->processImage().descriptor()
                                    : nullptr;
                            if (mediaDisplayCommand != nullptr && descriptor != nullptr &&
                                gpuOutcome.encodedArm ==
                                    runtime::GpuOutputColorArm::ProcessEffect &&
                                gpuOutcome.outputCommandIdentity ==
                                    mediaDisplayCommand->identity()) {
                                const auto extent = descriptor->dataWindow().extent();
                                if (static_cast<std::size_t>(extent.width()) * extent.height() ==
                                    gpuOutcome.encodedEffectRgba32f.size()) {
                                    gpuMediaDisplay.pixels =
                                        std::move(gpuOutcome.encodedEffectRgba32f);
                                    gpuMediaDisplay.width = extent.width();
                                    gpuMediaDisplay.height = extent.height();
                                    gpuMediaDisplay.commandIdentity =
                                        gpuOutcome.outputCommandIdentity;
                                    gpuMediaDisplay.displayDigest =
                                        mediaDisplay->display().digest();
                                }
                            }
                            if (gpuOutcome.encodedArm == runtime::GpuOutputColorArm::DisplayRgba8 &&
                                !gpuOutcome.encodedDisplayRgba8.empty() && descriptor != nullptr &&
                                resolved->color.display.identity != nullptr) {
                                const auto extent = descriptor->dataWindow().extent();
                                if (static_cast<std::size_t>(extent.width()) * extent.height() ==
                                    gpuOutcome.encodedDisplayRgba8.size()) {
                                    gpuDisplay.pixels = std::move(gpuOutcome.encodedDisplayRgba8);
                                    gpuDisplay.width = extent.width();
                                    gpuDisplay.height = extent.height();
                                    gpuDisplay.commandIdentity = gpuOutcome.outputCommandIdentity;
                                    gpuDisplay.displayIdentity = resolved->color.display.identity;
                                }
                            }
                        }
                    } else {
                        gpuProvenance.status = runtime::GpuProcessFrameStatus::DeviceUnavailable;
                    }
                }
                if (evaluatedFrame == nullptr) {
                    // The CPU reference frame names why it is not a GPU frame (#553).
                    if (forceCpuFallback)
                        gpuProvenance.cpuFallbackReason =
                            "the requested display transform could not be prepared on the GPU";
                    else if (gpuProvider == nullptr)
                        gpuProvenance.cpuFallbackReason =
                            "this export has no GPU provider (GPU rendering was not selected)";
                    else if (gpuProvenance.status ==
                             runtime::GpuProcessFrameStatus::DeviceUnavailable &&
                             gpuProvenance.cpuFallbackReason.empty())
                        gpuProvenance.cpuFallbackReason = "no usable GPU device";
                    if (gpuProvenance.cpuFallbackReason.empty())
                        gpuProvenance.cpuFallbackReason =
                            gpuProvenance.status == runtime::GpuProcessFrameStatus::Evaluated
                                ? "the GPU evaluation returned no frame"
                                : "the GPU evaluator is not available";
                    if (context.isCancellationRequested()) {
                        return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                    }
                    if (!plan) {
                        return admissionFailure(
                            runtime::GpuProcessFrameDiagnosticCode::InvalidRequest);
                    }
                    std::uint64_t pixels = static_cast<std::uint64_t>(plan->format().width()) *
                                           plan->format().height();
                    if (const auto* proxy =
                            std::get_if<runtime::ProxyResolution>(&evaluation.resolution)) {
                        pixels = static_cast<std::uint64_t>(proxy->extent.width()) *
                                 proxy->extent.height();
                    }
                    if (evaluation.roi) {
                        pixels = static_cast<std::uint64_t>(evaluation.roi->extent().width()) *
                                 evaluation.roi->extent().height();
                    }
                    const auto evaluatorOwner =
                        fallbackEvaluator
                            ? fallbackEvaluator
                            : std::make_shared<const runtime::CpuCompositionEvaluator>();
                    const auto& evaluator = *evaluatorOwner;
                    evaluator.setAssetBaseDirectory(assetBaseDirectory);
                    // Claim this frame's own preflight live-image peak. A limit taken from the
                    // room left at that instant shrank as other frames in flight and caches held
                    // memory, until a heavy frame failed evaluation partway through a long export.
                    // Admission now waits until the peak fits, and the evaluation gets at least it.
                    auto planning = evaluation;
                    planning.pixelStorageByteLimit = std::numeric_limits<std::size_t>::max();
                    const auto storage =
                        evaluator.planPreflightPixelStorage(plan, planning, context.cancellation());
                    if (storage.cancelled || context.isCancellationRequested()) {
                        return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                    }
                    const std::uint64_t peak = storage.requiredPreflightBytes.value_or(0);
                    auto grant = runtime::reserveHostEvaluationPhase(
                        *processingPolicy, pixels,
                        std::max<std::uint64_t>(evaluation.pixelStorageByteLimit, peak),
                        runtime::GpuProcessingPhaseOutput::FloatFinal,
                        preset == output::OutputPresetV1::PngRgba8SrgbV1,
                        runtime::GpuAdmissionClass::Export, peak);
                    if (grant.status != render::GpuProcessingPhaseAdmission::Accepted ||
                        !grant.lease) {
                        return admissionFailure(
                            grant.status == render::GpuProcessingPhaseAdmission::AwaitingCapacity
                                ? runtime::GpuProcessFrameDiagnosticCode::AwaitingCapacity
                                : runtime::GpuProcessFrameDiagnosticCode::BudgetRefused);
                    }
                    hostPhase = std::move(grant.lease);
                    // Revalidate is an identity check against verified revisions; a changed or
                    // missing original still fails before memoized pixels are accepted.
                    auto cpuEvaluation = evaluation;
                    cpuEvaluation.mediaSourceRevisionPolicy =
                        runtime::MediaSourceRevisionPolicy::Revalidate;
                    cpuEvaluation.requireVerifiedMediaSources = true;
                    cpuEvaluation.pixelStorageByteLimit =
                        static_cast<std::size_t>(hostPhase->sourceByteLimit());
                    cpuEvaluation.finalOutputPhase = hostPhase;
                    auto evalResult =
                        evaluator.evaluate(plan, cpuEvaluation, context.cancellation());
                    if (evalResult.status() == runtime::EvaluationStatus::Cancelled) {
                        return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                    }
                    if (evalResult.status() != runtime::EvaluationStatus::Evaluated ||
                        evalResult.frame() == nullptr) {
                        const auto code = evalResult.diagnostics().empty()
                                              ? runtime::EvaluationDiagnosticCode::InternalInvariant
                                              : evalResult.diagnostics().front().code;
                        BuildOutcomeV1 failed{.succeeded = false,
                                              .failureKind = BuildFailureKindV1::Evaluation,
                                              .rawCode = static_cast<std::uint8_t>(code)};
                        if (!evalResult.diagnostics().empty()) {
                            auto detail = std::make_shared<BuildProductV1>();
                            detail->failureMessage = evalResult.diagnostics().front().summary;
                            if (code == runtime::EvaluationDiagnosticCode::MediaSourceUnavailable)
                                detail->failureSubject = evalResult.diagnostics().front().detail;
                            failed.product = std::move(detail);
                        }
                        return runtime::TaskResult<BuildOutcomeV1>::succeeded(std::move(failed));
                    }
                    evaluatedFrame = evalResult.frame();
                }
                // The published final image retains its own rooted backing. Drop the evaluation
                // phase's remaining source/overlap claim after its temporary evaluator/cache has
                // gone out of scope, before admitting retained attempt metadata.
                hostPhase.reset();
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }

                context.reportProgress({.phase = "Identifying",
                                        .subphase = "",
                                        .completed = 0,
                                        .total = std::nullopt});
                const output::ProcessFrameSemanticIdentityV1Preparer identityPreparer;
                auto identityResult =
                    identityPreparer.prepare(evaluatedFrame, context.cancellation());
                if (identityResult.status() ==
                    output::ProcessFrameSemanticIdentityPreparationStatus::Cancelled) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }
                if (identityResult.status() !=
                        output::ProcessFrameSemanticIdentityPreparationStatus::Prepared ||
                    identityResult.identity() == nullptr) {
                    return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                        {.succeeded = false,
                         .failureKind = BuildFailureKindV1::Identity,
                         .rawCode = static_cast<std::uint8_t>(identityResult.error())});
                }
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }

                context.reportProgress(
                    {.phase = "Analyzing", .subphase = "", .completed = 0, .total = std::nullopt});
                const output::OutputAnalysisProcessSourceV1 processSource{
                    .state = output::OutputAnalysisProcessSourceStateV1::Ready,
                    .readyIdentity = identityResult.identity(),
                    .missingDescriptor = std::nullopt};
                // The preset-specific analyzer entry points -- never a preset enum plus a union of
                // optional fields (frame-output.md: "There is no public entry point that accepts a
                // preset enum plus a union of optional fields"). The PNG input's expected OCIO
                // revision is the persisted Bloom Neutral v1 expectedRevision, required whether or
                // not resolution succeeded; when resolution DID succeed it byte-equals the revision
                // embedded in the retained canonical DisplayProcessorIdentity, which the digest
                // stage independently re-checks.
                output::OutputAnalysisAnalyzerResultV1 analyzed =
                    preset == output::OutputPresetV1::PngRgba8SrgbV1
                        ? output::analyzePngRgba8SrgbV1(
                              {.process = processSource,
                               .expectedOcioRevision =
                                   evaluation.colorIntent.ocioConfigRevision == core::Sha256Digest{}
                                       ? color::kBloomNeutralV1ConfigDigest
                                       : evaluation.colorIntent.ocioConfigRevision,
                               .colorResolution = resolved->color.colorResolution,
                               .adapter = resolved->color.adapter})
                    : preset == output::OutputPresetV1::TiffRgba16SrgbV1
                        ? output::analyzeTiffRgba16SrgbV1(
                              {.process = processSource,
                               .adapter = output::outputPresetAvailabilityV1(
                                              output::OutputPresetV1::TiffRgba16SrgbV1)
                                                  .available
                                              ? output::OutputAnalysisAdapterStateV1::Qualified
                                              : output::OutputAnalysisAdapterStateV1::Unavailable,
                               .displayName = displayName,
                               .viewName = viewName})
                    : output::isDpxPresetV1(preset)
                        ? (preset == output::OutputPresetV1::DpxRgb10V1
                               ? output::analyzeDpxRgb10V1
                               : output::analyzeDpxRgb16V1)(
                              {.process = processSource,
                               .adapter = output::outputPresetAvailabilityV1(preset).available
                                              ? output::OutputAnalysisAdapterStateV1::Qualified
                                              : output::OutputAnalysisAdapterStateV1::Unavailable})
                        : output::analyzeFlatExrRgba32fLinRec709SceneV1({.process = processSource});
                if (!analyzed.hasReport()) {
                    return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                        {.succeeded = false,
                         .failureKind = BuildFailureKindV1::Analyzer,
                         .rawCode = static_cast<std::uint8_t>(analyzed.error())});
                }
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }

                // Bind the retained encoded display product to the exact process-pixel identity of
                // the frame it was transferred with; buildOutputAnalysisAttemptV1 re-validates the
                // pairing, so a cross-frame payload can never be retained.
                if (gpuDisplay.isPresent()) {
                    gpuDisplay.processPixelDigest = identityResult.identity()->processPixelDigest();
                }
                if (gpuMediaDisplay.isPresent()) {
                    gpuMediaDisplay.processPixelDigest =
                        identityResult.identity()->processPixelDigest();
                }

                if (gpuProvider) {
                    // Analysis and identity preparation can outlive the sampled root's freshness
                    // window. Refresh on this CPU worker before the attempt's retained claim.
                    gpuProvider->refreshProcessingCapacity(context.cancellation());
                    if (context.isCancellationRequested())
                        return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                }

                // ADR 0026 §7: a layered OpenEXR writes the output's channel layers beside RGBA.
                // They are source samples placed on the final data window at the codec boundary.
                std::shared_ptr<const render::ChannelSetImage> channelLayers;
                if (preset == output::OutputPresetV1::FlatExrRgba32fLinRec709SceneV1 && plan) {
                    const auto* descriptor = evaluatedFrame->processImage().descriptor();
                    // #480: an Image to Channel Set layer evaluates its image through the frame's
                    // own route (the GPU scene executor when the frame ran there, else the CPU
                    // reference) and reads it back once at this codec boundary.
                    const auto layerImage =
                        [&](const std::shared_ptr<const runtime::CompiledCompositionPlan>&
                                layerPlan) {
                        runtime::ChannelLayerImage layer;
                        if (layerPlan == nullptr) {
                            layer.failed = true;
                            return layer;
                        }
                        std::shared_ptr<const runtime::ProcessFrame> frame;
                        if (frameOnGpu && gpuProvider != nullptr) {
                            const auto evaluator = gpuProvider->evaluator();
                            if (evaluator == nullptr) {
                                layer.failed = true;
                                return layer;
                            }
                            auto outcome = evaluator->evaluate(layerPlan, evaluation,
                                                               context.cancellation(), {}, nullptr,
                                                               assetBaseDirectory);
                            if (outcome.status == runtime::GpuProcessFrameStatus::Cancelled) {
                                layer.cancelled = true;
                                return layer;
                            }
                            if (outcome.status == runtime::GpuProcessFrameStatus::Evaluated)
                                frame = std::move(outcome.frame);
                        } else {
                            const auto evaluatorOwner =
                                fallbackEvaluator
                                    ? fallbackEvaluator
                                    : std::make_shared<const runtime::CpuCompositionEvaluator>();
                            evaluatorOwner->setAssetBaseDirectory(assetBaseDirectory);
                            auto cpuEvaluation = evaluation;
                            cpuEvaluation.mediaSourceRevisionPolicy =
                                runtime::MediaSourceRevisionPolicy::Revalidate;
                            cpuEvaluation.requireVerifiedMediaSources = true;
                            auto evaluated = evaluatorOwner->evaluate(layerPlan, cpuEvaluation,
                                                                      context.cancellation());
                            if (evaluated.status() == runtime::EvaluationStatus::Cancelled) {
                                layer.cancelled = true;
                                return layer;
                            }
                            if (evaluated.status() == runtime::EvaluationStatus::Evaluated)
                                frame = evaluated.frame();
                        }
                        if (frame == nullptr) {
                            layer.failed = true;
                            return layer;
                        }
                        layer.image = std::shared_ptr<const render::Rgba32fImage>(
                            frame, &frame->processImage());
                        return layer;
                    };
                    auto assembled = runtime::assembleOutputChannelLayers(
                        *plan, evaluation.time, assetBaseDirectory, evaluation.colorIntent,
                        descriptor->dataWindow(), descriptor->displayWindow(),
                        evaluation.pixelStorageByteLimit, context.cancellation(), layerImage);
                    if (assembled.cancelled)
                        return runtime::TaskResult<BuildOutcomeV1>::cancelled();
                    if (!assembled.diagnostic.empty())
                        return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                            {.succeeded = false,
                             .failureKind = BuildFailureKindV1::Evaluation,
                             .rawCode = static_cast<std::uint8_t>(
                                 runtime::EvaluationDiagnosticCode::InvalidParameter)});
                    channelLayers = std::move(assembled.layers);
                }

                auto buildResult = output::buildOutputAnalysisAttemptV1(
                    {.frame = evaluatedFrame,
                     .processIdentity = identityResult.identity(),
                     .report = analyzed.report(),
                     .target = resolved->target,
                     .display = resolved->color.display,
                     .gpuDisplay = std::move(gpuDisplay),
                     .gpuMediaDisplay = std::move(gpuMediaDisplay),
                     .processingPolicy = processingPolicy,
                     .capacityRefresh =
                         [provider = gpuProvider](const runtime::CancellationToken& cancellation) {
                             if (provider)
                                 provider->refreshProcessingCapacity(cancellation);
                         },
                     .channelLayers = std::move(channelLayers)},
                    *ledger);
                if (!buildResult) {
                    return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                        {.succeeded = false,
                         .failureKind = BuildFailureKindV1::AttemptBuild,
                         .rawCode = static_cast<std::uint8_t>(buildResult.error())});
                }
                return runtime::TaskResult<BuildOutcomeV1>::succeeded(
                    {.succeeded = true,
                     .product = std::make_shared<const BuildProductV1>(
                         BuildProductV1{buildResult.attempt(), gpuProvenance, {}, {}})});
            });

        if (!submission.accepted()) {
            state_->completed = true;
            return OutputAnalysisAttemptOutcomeV1::failure(
                {OutputAnalysisAttemptStageV1::Evaluating, false, submission.status});
        }
        state_->buildHandle.emplace(std::move(submission.handle));
        return std::nullopt;
    }

    auto taken = state_->buildHandle->tryTakeResult();
    if (!taken.has_value()) {
        return std::nullopt;
    }
    state_->completed = true;
    if (taken->state() == runtime::TaskState::Cancelled) {
        return OutputAnalysisAttemptOutcomeV1::failure(
            {OutputAnalysisAttemptStageV1::Evaluating, true, std::monostate{}});
    }
    if (!taken->value().has_value()) {
        return OutputAnalysisAttemptOutcomeV1::failure(
            {OutputAnalysisAttemptStageV1::Analyzing, false, std::monostate{}});
    }
    const auto& buildValue = taken->value();
    const auto& product = buildValue->product;
    if (taken->state() != runtime::TaskState::Succeeded || !buildValue->succeeded ||
        product == nullptr || product->attempt == nullptr) {
        return OutputAnalysisAttemptOutcomeV1::failure(translateBuildFailure(*buildValue));
    }
    return OutputAnalysisAttemptOutcomeV1::completed(product->attempt, product->gpuProvenance);
}

OutputAnalysisAttemptRunnerResultV1::OutputAnalysisAttemptRunnerResultV1(
    const runtime::TaskSubmissionStatus status) noexcept
    : submissionFailure_(status) {}

OutputAnalysisAttemptRunnerResultV1::OutputAnalysisAttemptRunnerResultV1(
    OutputAnalysisAttemptRunnerV1 handle) noexcept
    : handle_(std::move(handle)) {}

OutputAnalysisAttemptRunnerV1 OutputAnalysisAttemptRunnerResultV1::takeHandle() && noexcept {
    if (!handle_.has_value()) {
        std::terminate();
    }
    return std::move(*handle_);
}

std::optional<ExistingExportTargetV1>
sampleExistingExportTargetV1(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return std::nullopt;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return std::nullopt;
    const auto time = std::filesystem::last_write_time(path, error);
    if (error)
        return std::nullopt;
    return ExistingExportTargetV1{.byteSize = static_cast<std::uint64_t>(size),
                                  .modificationTicks =
                                      static_cast<std::int64_t>(time.time_since_epoch().count())};
}

OutputAnalysisAttemptRunnerResultV1 beginOutputAnalysisAttemptV1(
    runtime::TaskScheduler& scheduler, platform::StagedArtifactCoordinator& artifacts,
    output::ExportResourceLedgerV1& ledger, OutputAnalysisAttemptRequestV1 request) {
    const auto targetPath = request.targetPath;
    const auto overwritePolicy = request.overwritePolicy;
    const auto confirmedExisting = request.confirmedExisting;
    const auto capturedColor =
        runtime::captureEvaluationColorIntent(request.evaluation.colorIntent);
    if (!capturedColor) {
        return OutputAnalysisAttemptRunnerResultV1(runtime::TaskSubmissionStatus::InvalidRequest);
    }
    request.evaluation.colorIntent = *capturedColor;
    // Direct aggregate-brace construction (never T's own implicit default constructor, which
    // std::make_unique<T>() would call with zero arguments): OutputAnalysisAttemptRequestV1 has no
    // default constructor of its own (runtime::EvaluationRequest's OperationIndex member has none
    // either), so `request` -- the one field with no usable default -- must be supplied inline
    // here rather than default-constructed then assigned.
    auto state = std::unique_ptr<detail::OutputAnalysisAttemptRunnerState>(
        new detail::OutputAnalysisAttemptRunnerState{
            .scheduler = &scheduler,
            .artifacts = &artifacts,
            .ledger = &ledger,
            .request = std::move(request),
            .stage = OutputAnalysisAttemptStageV1::Resolving,
            .resolvingHandle = {},
            .buildHandle = std::nullopt,
            .resolvedInputs = nullptr,
            .completed = false,
        });

    const auto preset = state->request.preset;
    auto* const displayProvider = state->request.displayProcessorProvider;
    auto* const gpuProvider = state->request.gpuProvider.get();
    const auto capturedColorIntent = state->request.evaluation.colorIntent;
    auto displayName = state->request.displayName;
    auto viewName = state->request.viewName;
    const runtime::GpuOcioCommandGeometry colorGeometry{
        .width = state->request.plan != nullptr ? state->request.plan->format().width() : 0,
        .height = state->request.plan != nullptr ? state->request.plan->format().height() : 0};

    runtime::TaskRequest resolvingRequest(
        "Resolve an export target", attemptOwner(state->request.owner),
        runtime::TaskPriority::Foreground, runtime::TaskExecutor::BlockingIo);
    auto submission = scheduler.submit<ResolvingOutcomeV1>(
        std::move(resolvingRequest),
        [&artifacts, targetPath, overwritePolicy, confirmedExisting, preset, displayProvider,
         gpuProvider, capturedColorIntent, displayName = std::move(displayName),
         viewName = std::move(viewName),
         colorGeometry](runtime::TaskContext& context) -> runtime::TaskResult<ResolvingOutcomeV1> {
            if (context.isCancellationRequested()) {
                return runtime::TaskResult<ResolvingOutcomeV1>::cancelled();
            }
            context.reportProgress(
                {.phase = "Resolving", .subphase = "", .completed = 0, .total = std::nullopt});
            auto preflightResult = artifacts.preflight({.targetPath = targetPath,
                                                        .overwritePolicy = overwritePolicy,
                                                        .expectedTarget = std::nullopt});
            if (!preflightResult) {
                return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                    {.succeeded = false, .error = preflightResult.error()});
            }
            auto target = std::move(preflightResult).takeTarget();
            const auto targetKey = target.targetKey();
            const auto observation = target.observation();
            // #453: a confirmed replacement covers only the file that was confirmed. Sampled after
            // the preflight observation, so a change before it fails here and a change after it
            // fails the publication guard.
            if (observation.exists && confirmedExisting.has_value()) {
                const auto current = sampleExistingExportTargetV1(targetPath);
                if (!current || *current != *confirmedExisting ||
                    current->byteSize != observation.fingerprint.byteSize) {
                    return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                        {.succeeded = false,
                         .error = platform::StagedArtifactError::ExternalModificationConflict});
                }
            }
            // `target` goes out of scope here: the live platform active-target admission it holds
            // is released immediately rather than kept open for the duration of a pending artist
            // decision (see this file's own header-level rationale comment).
            ResolvedAttemptInputsV1 inputs{.target = {.targetKey = targetKey,
                                                      .observation = observation,
                                                      .targetPath = targetPath,
                                                      .overwritePolicy = overwritePolicy},
                                           .color = {}};

            // EXR has no display product and therefore never enters ColorPreparing at all.
            if (preset == output::OutputPresetV1::PngRgba8SrgbV1) {
                if (context.isCancellationRequested()) {
                    return runtime::TaskResult<ResolvingOutcomeV1>::cancelled();
                }
                context.reportProgress({.phase = "ColorPreparing",
                                        .subphase = "",
                                        .completed = 0,
                                        .total = std::nullopt});
                auto color = detail::resolvePngDisplayProducts(
                    displayProvider, gpuProvider, capturedColorIntent, displayName, viewName,
                    colorGeometry, [&context] { return context.isCancellationRequested(); });
                if (!color.has_value()) {
                    return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                        {.succeeded = false, .colorStageFailed = true});
                }
                inputs.color = std::move(*color);
            }

            std::shared_ptr<const ResolvedAttemptInputsV1> retained;
            try {
                retained = std::make_shared<const ResolvedAttemptInputsV1>(std::move(inputs));
            } catch (const std::bad_alloc&) {
                return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                    {.succeeded = false,
                     .error = platform::StagedArtifactError::ResourceUnavailable});
            }
            return runtime::TaskResult<ResolvingOutcomeV1>::succeeded(
                {.succeeded = true, .resolved = std::move(retained)});
        });

    if (!submission.accepted()) {
        return OutputAnalysisAttemptRunnerResultV1(submission.status);
    }
    state->resolvingHandle = std::move(submission.handle);
    return OutputAnalysisAttemptRunnerResultV1(OutputAnalysisAttemptRunnerV1(std::move(state)));
}

} // namespace bloom::host
