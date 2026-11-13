#include <bloom/runtime/audio_analysis.hpp>
#include <bloom/runtime/image_statistics.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>

#include "composed_artwork_bounds.hpp"
#include "gpu_media_preparation.hpp"
#include "gpu_scene_coverage.hpp"
#include "gpu_scene_effect_emission.hpp"
#include "gpu_scene_layer_emission.hpp"
#include "gpu_scene_media_layer.hpp"
#include "gpu_scene_nested.hpp"
#include "gpu_scene_preparation_builders.hpp"
#include "gpu_scene_preparation_leaf_emission.hpp"
#include "gpu_scene_preparation_private.hpp"
#include "gpu_scene_vector_emission.hpp"

#include "operation_key.hpp"
#include "visible_support.hpp"
#include <bloom/core/blend_mode.hpp>
#include <bloom/render/cpu_image_primitives.hpp>
#include <bloom/render/image.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace bloom::runtime {

struct GpuSceneSourceBackingLedger final {
    std::unordered_set<const void*> identities;
    std::uint64_t chargedBytes = 0;
};

namespace {

[[nodiscard]] PreparedGpuSceneBuildResult failed(const PreparedGpuSceneDiagnosticCode code,
                                                 std::string message,
                                                 GpuSceneMediaStatistics statistics = {}) {
    return {nullptr, PreparedGpuSceneDiagnostic{code, std::move(message), statistics}};
}

// The CPU evaluator's vector chain: a solid/text/shape leaf, the composed author-space matrix every
// enclosing layer multiplied into it, and the accumulated layer opacity. It is propagated through a
// Layer Output exactly as the evaluator does, so a layer fed by another vector layer rasterizes the
// ORIGINAL leaf geometry through the full chain rather than resampling an intermediate raster.
// Defined in gpu_scene_effect_emission.hpp so an identity image effect can propagate it unchanged.
using detail::GpuSceneVectorChain;

// Operations whose pixels are colour-space agnostic under any resolvable working space, verified
// against the CPU reference:
//  * Solid / Text / Shape authored colours are numeric values in the effective working space; the
//    CPU evaluator only premultiplies them (render::solidPixelFromStraightLinearRec709Scene is a
//    pure premultiply, no working-space transform), and the GPU solid / coverage commands store the
//    same premultiplied values, so they are correct for any working space.
//  * Layer Output / Affine / Translation / Merge / Blend operate on already-resolved premultiplied
//    values and are colour-space agnostic.
//  * Image Source / Video Source are decoded raw and converted to the working space through a real
//    input->working OCIO ProcessEffect command.
//  * Image Effect resolves its from/to ids in the effective working space.
//  * A nested composition inherits the working space; the composition output is a passthrough.
// An operation not listed here is refused for a non-neutral working space, so a future
// working-space-specific operation cannot silently mis-colour.
[[nodiscard]] bool isWorkingSpaceAgnosticOperation(const CompiledOperation& operation) noexcept {
    return std::holds_alternative<CompiledSolid>(operation) ||
           std::holds_alternative<CompiledChecker>(operation) ||
           std::holds_alternative<CompiledText>(operation) ||
           std::holds_alternative<CompiledShape>(operation) ||
           std::holds_alternative<CompiledImageSource>(operation) ||
           std::holds_alternative<CompiledVideoSource>(operation) ||
           std::holds_alternative<CompiledImageEffect>(operation) ||
           std::holds_alternative<CompiledLayerOutput>(operation) ||
           std::holds_alternative<CompiledMerge>(operation) ||
           std::holds_alternative<CompiledCompositionSource>(operation) ||
           std::holds_alternative<CompiledCompositionOutput>(operation) ||
           std::holds_alternative<CompiledChannelShuffle>(operation) ||
           std::holds_alternative<CompiledCryptomatte>(operation);
}

// Retained scene storage past the stated allowance is claimed in steps, so a dense scene takes the
// budget's lock a few times rather than once per shape.
constexpr std::uint64_t kRetainedCoverageClaimStep = 16ULL * 1024 * 1024;

// One command, its keys, and the executor's step for it.
[[nodiscard]] std::uint64_t gpuSceneCommandStorageBytes(const std::size_t commands) noexcept {
    constexpr std::uint64_t kKeyBytes = 2 * 96;
    constexpr std::uint64_t kExecutorStepBytes = 512;
    return commands * (sizeof(GpuSceneCommand) + kKeyBytes + kExecutorStepBytes);
}

struct RetainedStorageRefusal final {
    PreparedGpuSceneDiagnosticCode code = PreparedGpuSceneDiagnosticCode::None;
    std::string message;
};

[[nodiscard]] std::optional<RetainedStorageRefusal>
claimRetainedSceneStorage(const std::shared_ptr<render::GpuProcessingPhaseLease>& lease,
                          const std::uint64_t bytes) {
    switch (lease->claimRetainedHostBytes(bytes)) {
    case render::GpuProcessingPhaseAdmission::Accepted:
        return std::nullopt;
    case render::GpuProcessingPhaseAdmission::AwaitingCapacity:
        return RetainedStorageRefusal{PreparedGpuSceneDiagnosticCode::AwaitingCapacity,
                                      "The prepared scene is awaiting host-memory capacity"};
    default:
        return RetainedStorageRefusal{PreparedGpuSceneDiagnosticCode::BudgetRefused,
                                      "The prepared scene does not fit the host-memory capacity"};
    }
}

} // namespace

PreparedGpuSceneBuildResult
CpuGpuSceneBuilder::build(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                          const EvaluationRequest& request,
                          const CancellationToken& cancellation) const {
    try {
        auto capturedRequest = request;
        const auto color = captureEvaluationColorIntent(request.colorIntent);
        if (!color)
            return failed(PreparedGpuSceneDiagnosticCode::AllocationFailure,
                          "Scene color intent could not be captured");
        capturedRequest.colorIntent = *color;
        return buildImpl(plan, capturedRequest, cancellation);
    } catch (const std::bad_alloc&) {
        return failed(PreparedGpuSceneDiagnosticCode::AllocationFailure,
                      "Scene preparation ran out of memory");
    } catch (...) {
        return failed(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                      "Scene preparation failed unexpectedly");
    }
}

PreparedGpuSceneBuildResult
CpuGpuSceneBuilder::buildImpl(const std::shared_ptr<const CompiledCompositionPlan>& plan,
                              const EvaluationRequest& request,
                              const CancellationToken& cancellation) const {
    // This build's upload scratch (ADR 0037): identical uploads share one backing inside the
    // build, a nested child joins its parent's, and nothing of it survives the build.
    auto media = mediaContext_;
    if (!media.buildUploads)
        media.buildUploads = std::make_shared<GpuSceneBuildUploads>();
    auto phaseLease = phase_.admittedLease;
    std::optional<TaskAdmissionRetry> refusalRetry;
    std::optional<TaskDefaultBudgetRetry> defaultBudgetRetry;
    const auto fail = [&](const PreparedGpuSceneDiagnosticCode code, std::string message,
                          GpuSceneMediaStatistics statistics = {}) {
        auto result = failed(code, std::move(message), statistics);
        if (code == PreparedGpuSceneDiagnosticCode::AwaitingCapacity ||
            code == PreparedGpuSceneDiagnosticCode::BudgetRefused) {
            result.diagnostic.admissionRetry =
                phaseLease
                    ? std::optional<TaskAdmissionRetry>{{phaseLease->revisionAfterOwnRelease(),
                                                         phase_.envelope.native}}
                    : refusalRetry;
        }
        return result;
    };
    if (plan == nullptr) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "No compiled plan");
    }
    if (plan->planSemanticsVersion() != kCompiledCompositionPlanSemanticsVersion ||
        plan->animationSamplingSemanticsVersion() != kAnimationSamplingSemanticsVersion) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Compiled plan semantics are unsupported");
    }
    if (request.quality != EvaluationQuality::Reference ||
        request.colorIntent.workingColorSpaceId.find('\0') != std::string_view::npos) {
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedRequest,
                    "Only Reference quality is prepared");
    }
    // The non-media prepared subset (solid pixels, vector coverage, image effects) assumes
    // lin_rec709_scene pixel semantics, so a different working space is refused for it. A
    // media-only scene carries no such operation: its pixels are resolved through the exact
    // configured input->working OCIO transform, so an ACES scene-linear working space (or any other
    // resolvable one) is prepared. The reachable-subset check runs below.
    const bool neutralWorkingSpace =
        request.colorIntent.workingColorSpaceId == kLinearRec709SceneColorSpaceId;
    if (plan->operations().empty() || request.output.value() >= plan->operations().size() ||
        request.output != plan->output() ||
        !std::holds_alternative<CompiledCompositionOutput>(
            plan->operations()[request.output.value()])) {
        return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                    "Request output is not the plan's terminal Composition Output");
    }
    if (cancellation.isCancellationRequested()) {
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
    }
    // This build is a render request in the playback working set (ADR 0037): the sources it uses
    // are tagged with its frame and stay while that frame is in the window. A nested build joins
    // its parent's request.
    SourceWorkingSet::Request sourceRequest;
    if (media.operationCache && !SourceWorkingSet::current().tracked) {
        const auto rate = plan->format().frameRate();
        sourceRequest = SourceWorkingSet::Request(
            media.operationCache->sourceWindow(), plan->compositionId().value(),
            SourceWorkingSet::frameAt(request.time.numerator(), request.time.denominator(),
                                      rate.numerator(), rate.denominator()));
    }

    const std::size_t operationCount = plan->operations().size();
    // The reachable-operation subset classifier is extracted to gpu_scene_preparation_builders.hpp.
    std::vector<bool> reachable;
    if (const auto error = detail::computeReachablePreparedOperations(*plan, request.output.value(),
                                                                      cancellation, reachable)) {
        return fail(error->code, error->message);
    }
    // A non-neutral working space is admitted for every operation whose pixels the CPU reference
    // resolves in the effective working space (see isWorkingSpaceAgnosticOperation). The display
    // consumer remains a separate lane.
    if (!neutralWorkingSpace) {
        for (std::size_t index = 0; index < operationCount; ++index) {
            if (reachable[index] && !isWorkingSpaceAgnosticOperation(plan->operations()[index])) {
                return fail(PreparedGpuSceneDiagnosticCode::UnsupportedRequest,
                            "A non-lin_rec709_scene working space reached an operation without "
                            "verified working-space semantics");
            }
        }
    }

    // Reuse semantic/geometry validation, but do not apply the CPU evaluator's hypothetical
    // full-frame working set to GPU commands. Host retention is charged below against the real
    // source/coverage allowances; native images and scratch belong to the device lease/executor.
    auto validationRequest = request;
    validationRequest.pixelStorageByteLimit = std::numeric_limits<std::size_t>::max();
    // #347: an Audio Amplitude in the value graph prepares its analysis on this worker.
    const AudioAnalysisPreparation audioPreparation(plan, media.assetBaseDirectory, cancellation);
    // #349: Image Statistics are measured by a route that can run the picture's plan for this
    // frame (a final render's device route installs one). A scene built where none is installed is
    // not prepared: the frame takes the CPU reference path, which measures them itself.
    if (planReadsImageStatistics(*plan) && !ImageStatisticsPreparation::installedHere())
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation,
                    "Image Statistics are measured by the CPU reference route here");
    auto checked = detail::preflight(plan, validationRequest, cancellation, {}, nullptr, nullptr);
    if (checked.cancelled || cancellation.isCancellationRequested()) {
        return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
    }
    if (!checked.resolved.has_value()) {
        const auto code = checked.diagnostic.has_value()
                              ? checked.diagnostic->code
                              : EvaluationDiagnosticCode::InternalInvariant;
        std::string message;
        if (checked.diagnostic.has_value()) {
            message = checked.diagnostic->summary;
            if (message.empty()) {
                message = checked.diagnostic->detail;
            }
        }
        if (message.empty()) {
            message = "Preflight failed";
        }
        return fail(code == EvaluationDiagnosticCode::PixelStorageBudgetExceeded
                        ? PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded
                        : PreparedGpuSceneDiagnosticCode::PreflightFailure,
                    std::move(message));
    }
    const detail::ResolvedEvaluation& frameResolved = *checked.resolved;
    detail::pruneIdentityCodecInputs(*plan, frameResolved, request.time, request.output.value(),
                                     reachable);
    if (frameResolved.imageDescriptor.dataWindow().extent().width() == 0) {
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "Preflight produced an invalid process descriptor");
    }
    std::uint64_t retainedAllowance = request.pixelStorageByteLimit;
    std::uint64_t sourceAllowance = request.pixelStorageByteLimit;
    std::uint64_t coverageAllowance = request.pixelStorageByteLimit;
    if (phase_.policy && !phaseLease) {
        auto envelope = phase_.envelope;
        // #539: speculative frames yield to the Viewer's (ADR 0033 section 9 classes).
        if (request.background && envelope.admissionClass == GpuAdmissionClass::Viewer)
            envelope.admissionClass = GpuAdmissionClass::Background;
        bool derivedCeiling = false;
        if (envelope.native && !envelope.requestedExecutorBytes && phase_.policy->budgetView()) {
            const auto delivered = phase_.policy->budgetView()->load();
            if (!delivered || !delivered->fresh()) {
                defaultBudgetRetry =
                    TaskDefaultBudgetRetry{delivered ? delivered->sampleSerial : 0};
                auto outcome = fail(PreparedGpuSceneDiagnosticCode::AwaitingCapacity,
                                    "Awaiting delivered preview request capacity");
                outcome.diagnostic.defaultBudgetRetry = defaultBudgetRetry;
                return outcome;
            }
            envelope.requestedExecutorBytes =
                phase_.policy->snapshot().reservations.deviceCapacityBytes.value_or(0);
            derivedCeiling = true;
        }
        if (envelope.native && envelope.requestedExecutorBytes) {
            // The executor needs at least its live-image peak (one float frame at minimum),
            // whether its ceiling was derived above or stated by the caller (a final render,
            // #417); the ceiling is only an upper bound shared with other phases. The preflight's
            // peak is the CPU reference's in plan order, which holds a whole Merge at once; the
            // executor does not (#343), and never holds more than that. This is the admission
            // floor, taken before any window is known. Once the scene is prepared the phase raises
            // its claim to the executor's live set with every image at its prepared size (an image
            // larger than the frame, such as a gradient drawn in its own window, included).
            const auto extent = frameResolved.imageDescriptor.dataWindow().extent();
            const auto frameBytes = static_cast<std::uint64_t>(extent.width()) * extent.height() *
                                    sizeof(render::Rgba32f);
            const auto peak = std::max(
                frameBytes,
                std::min(static_cast<std::uint64_t>(frameResolved.peakPixelStorageBytes),
                         detail::gpuExecutorLivePeakBytes(*plan, request.output.value(),
                                                          frameBytes)));
            const auto ceiling = *envelope.requestedExecutorBytes;
            if (derivedCeiling)
                envelope.minimumExecutorBytes =
                    std::min(ceiling, std::max(envelope.minimumExecutorBytes, peak));
            else if (envelope.minimumExecutorBytes <= ceiling)
                // A stated minimum is raised toward the peak within the stated ceiling, never
                // lowered; one above the ceiling stays an invalid request.
                envelope.minimumExecutorBytes =
                    std::max(envelope.minimumExecutorBytes, std::min(ceiling, peak));
        }
        const auto extent = frameResolved.imageDescriptor.dataWindow().extent();
        envelope.pixels = static_cast<std::uint64_t>(extent.width()) * extent.height();
        envelope.requestedSourceBytes = request.pixelStorageByteLimit;
        envelope.sourceFree = true;
        for (std::size_t index = 0; index < operationCount; ++index) {
            if (reachable[index] &&
                (std::holds_alternative<CompiledImageSource>(plan->operations()[index]) ||
                 std::holds_alternative<CompiledVideoSource>(plan->operations()[index]) ||
                 std::holds_alternative<CompiledChannelShuffle>(plan->operations()[index]) ||
                 std::holds_alternative<CompiledCryptomatte>(plan->operations()[index]) ||
                 std::holds_alternative<CompiledCompositionSource>(plan->operations()[index]))) {
                envelope.sourceFree = false;
                break;
            }
        }
        // The phase claims its actual source need: every reachable decoded source as one float
        // RGBA frame, a channel read as what it holds (#549, below), or the preflight live-image
        // peak when a source has no known extent (nested compositions, unprobed media). The
        // caller's limit stays an unclaimed ceiling.
        if (!envelope.sourceFree) {
            std::uint64_t sourceNeed = 0;
            std::uint64_t largestSource = 0;
            bool unknownExtent = false;
            // #549: a channel read holds frames of its file sources' extent. A Shuffle retains its
            // one RGBA32F gather. A Cryptomatte type retains its rank layers (one RGBA32F frame per
            // two ranks, shared by every matte of the type) after decoding their ranks' channels;
            // the ranks are known only from the file, so the claim takes the Cryptomatte
            // specification's default depth of six ranks (three layers), and a phase that retains
            // more grows its claim when its sources are prepared. An image-backed or nested channel
            // source, or one without a recorded extent, keeps the preflight peak.
            std::vector<std::string> cryptomatteTypes;
            const auto channelReadFrame =
                [&unknownExtent](const CompiledChannelExpression& channels) -> std::uint64_t {
                std::uint64_t frame = 0;
                for (const auto& source : channels.sources) {
                    if (source.imageBacked || !source.nesting.empty() || !source.asset ||
                        source.asset->width == 0 || source.asset->height == 0) {
                        unknownExtent = true;
                        return 0;
                    }
                    frame = std::max<std::uint64_t>(
                        frame, static_cast<std::uint64_t>(source.asset->width) *
                                   source.asset->height * sizeof(render::Rgba32f));
                }
                if (frame == 0)
                    unknownExtent = true;
                return frame;
            };
            // Nested compositions contribute their own media sources; only a source without a
            // recorded extent falls back to the preflight peak.
            std::vector<const CompiledCompositionPlan*> pending{plan.get()};
            std::vector<const CompiledCompositionPlan*> visited;
            while (!pending.empty()) {
                const auto* current = pending.back();
                pending.pop_back();
                if (std::find(visited.begin(), visited.end(), current) != visited.end())
                    continue;
                visited.push_back(current);
                const bool root = current == plan.get();
                for (std::size_t index = 0; index < current->operations().size(); ++index) {
                    if (root && !reachable[index])
                        continue;
                    const auto& operation = current->operations()[index];
                    const document::AssetRecord* asset = nullptr;
                    if (const auto* image = std::get_if<CompiledImageSource>(&operation))
                        asset = image->asset ? &*image->asset : nullptr;
                    else if (const auto* video = std::get_if<CompiledVideoSource>(&operation))
                        asset = video->asset ? &*video->asset : nullptr;
                    else if (const auto* nested =
                                 std::get_if<CompiledCompositionSource>(&operation)) {
                        if (nested->nestedPlanIndex < current->nestedPlans().size() &&
                            current->nestedPlans()[nested->nestedPlanIndex])
                            pending.push_back(
                                current->nestedPlans()[nested->nestedPlanIndex].get());
                        else
                            unknownExtent = true;
                        continue;
                    } else if (const auto* shuffle =
                                   std::get_if<CompiledChannelShuffle>(&operation)) {
                        const auto bytes = channelReadFrame(shuffle->channels);
                        sourceNeed += bytes;
                        largestSource = std::max(largestSource, bytes);
                        continue;
                    } else if (const auto* cryptomatte =
                                   std::get_if<CompiledCryptomatte>(&operation)) {
                        constexpr std::uint64_t kDefaultCryptomatteRankLayers = 3;
                        const auto frame = channelReadFrame(cryptomatte->channels);
                        // Every matte of one type over the same sources reads one set of layers.
                        std::string type = cryptomatte->layer;
                        for (const auto& source : cryptomatte->channels.sources)
                            if (source.asset) {
                                const auto hex = source.asset->contentDigest.toLowercaseHex();
                                type.append(1, '\0').append(hex.begin(), hex.end());
                                type.append(1, '\0').append(source.part);
                            }
                        if (frame == 0 ||
                            std::ranges::find(cryptomatteTypes, type) != cryptomatteTypes.end())
                            continue;
                        cryptomatteTypes.push_back(std::move(type));
                        const auto bytes = frame * kDefaultCryptomatteRankLayers;
                        sourceNeed += bytes;
                        largestSource = std::max(largestSource, bytes);
                        continue;
                    } else {
                        continue;
                    }
                    if (asset == nullptr || asset->width == 0 || asset->height == 0) {
                        unknownExtent = true;
                        continue;
                    }
                    const auto bytes = static_cast<std::uint64_t>(asset->width) * asset->height *
                                       sizeof(render::Rgba32f);
                    sourceNeed += bytes;
                    largestSource = std::max(largestSource, bytes);
                }
            }
            if (unknownExtent)
                sourceNeed =
                    std::max<std::uint64_t>(sourceNeed, frameResolved.peakPixelStorageBytes);
            envelope.minimumSourceBytes = std::min(
                envelope.requestedSourceBytes, std::max(envelope.minimumSourceBytes, sourceNeed));
            // Sources are decoded and converted one at a time (#472): their scratch follows the
            // largest one. An unknown extent keeps the whole-need scratch bound.
            envelope.largestSourceBytes = unknownExtent ? 0 : largestSource;
        }
        if (cancellation.isCancellationRequested()) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
        }
        auto grant = phase_.policy->reserve(envelope);
        if (!grant.lease) {
            using Admission = render::GpuProcessingPhaseAdmission;
            switch (grant.status) {
            case Admission::AwaitingCapacity:
                refusalRetry = TaskAdmissionRetry{grant.admissionRevision, envelope.native};
                if (grant.domain == render::GpuProcessingPhaseDomain::Host) {
                    return fail(PreparedGpuSceneDiagnosticCode::AwaitingCapacity,
                                "GPU preparation is awaiting host-memory capacity");
                }
                if (grant.domain == render::GpuProcessingPhaseDomain::Device) {
                    return fail(PreparedGpuSceneDiagnosticCode::AwaitingCapacity,
                                "GPU preparation is awaiting device-memory capacity");
                }
                if (grant.domain == render::GpuProcessingPhaseDomain::RequestCount) {
                    return fail(
                        PreparedGpuSceneDiagnosticCode::AwaitingCapacity,
                        "GPU preparation is waiting for another processing request to finish");
                }
                if (grant.domain == render::GpuProcessingPhaseDomain::Viewer) {
                    return fail(PreparedGpuSceneDiagnosticCode::AwaitingCapacity,
                                "GPU preparation is waiting for the Viewer");
                }
                return fail(PreparedGpuSceneDiagnosticCode::AwaitingCapacity,
                            "GPU preparation is awaiting sampled processing capacity");
            case Admission::BudgetRefused:
                refusalRetry = TaskAdmissionRetry{grant.admissionRevision, envelope.native};
                if (grant.domain == render::GpuProcessingPhaseDomain::Host) {
                    return fail(PreparedGpuSceneDiagnosticCode::BudgetRefused,
                                "GPU preparation does not fit the current host-memory capacity");
                }
                if (grant.domain == render::GpuProcessingPhaseDomain::Device) {
                    return fail(PreparedGpuSceneDiagnosticCode::BudgetRefused,
                                "GPU preparation does not fit the current device-memory capacity");
                }
                if (grant.domain == render::GpuProcessingPhaseDomain::RequestCount) {
                    return fail(
                        PreparedGpuSceneDiagnosticCode::BudgetRefused,
                        "GPU preparation is waiting for another processing request to finish");
                }
                return fail(PreparedGpuSceneDiagnosticCode::BudgetRefused,
                            "The paired processing phase envelope was refused before preparation");
            case Admission::InvalidRequest:
                return fail(PreparedGpuSceneDiagnosticCode::InvalidRequest,
                            "The processing phase envelope is invalid");
            case Admission::AllocationFailure:
                return fail(PreparedGpuSceneDiagnosticCode::AllocationFailure,
                            "Processing phase reservation storage allocation failed");
            case Admission::SerialExhausted:
                return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                            "Processing phase admission identifiers are exhausted");
            case Admission::Accepted:
                return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                            "Processing phase admission returned no accepted lease");
            }
        }
        phaseLease = std::move(grant.lease);
    }
    if (phaseLease) {
        sourceAllowance =
            std::min(phase_.remainingSourceBytes.value_or(phaseLease->sourceByteLimit()),
                     phaseLease->sourceByteLimit());
        coverageAllowance = std::min(
            phase_.remainingCoverageBytes.value_or(phase_.envelope.retainedCoverageByteLimit),
            phase_.envelope.retainedCoverageByteLimit);
        if (sourceAllowance > std::numeric_limits<std::uint64_t>::max() - coverageAllowance) {
            return fail(PreparedGpuSceneDiagnosticCode::BudgetRefused,
                        "The admitted source/coverage ceilings overflow");
        }
        retainedAllowance = sourceAllowance + coverageAllowance;
        if (phase_.admittedLease && coverageAllowance == 0 &&
            !phase_.envelope.growsRetainedSceneStorage) {
            for (std::size_t index = 0; index < operationCount; ++index) {
                if (reachable[index] &&
                    (std::holds_alternative<CompiledText>(plan->operations()[index]) ||
                     std::holds_alternative<CompiledShape>(plan->operations()[index]))) {
                    return fail(
                        PreparedGpuSceneDiagnosticCode::BudgetRefused,
                        "Borrowed coverage capacity is exhausted before vector preparation");
                }
            }
        }
    }

    const auto fullDescriptor = render::Rgba32fImageDescriptor::create(
        frameResolved.imageDescriptor.displayWindow(),
        frameResolved.imageDescriptor.displayWindow(), frameResolved.imageDescriptor.pixelAspect());
    if (!fullDescriptor) {
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "Composition descriptor is invalid");
    }
    const auto fullWindow = fullDescriptor.value()->dataWindow();
    const auto fullDisplayWindow = fullDescriptor.value()->displayWindow();
    const auto fullPixelAspect = fullDescriptor.value()->pixelAspect();

    const double hScale = frameResolved.horizontalScale;
    const double vScale = frameResolved.verticalScale;

    // Screen every reachable layer for an unsupported blend or transform BEFORE any media leaf is
    // resolved or decoded, so a graph containing an unsupported layer never prepares -- and never
    // decodes -- its media. An inactive layer, exactly as in the CPU evaluator, publishes nothing
    // and is therefore not screened.
    if (const auto error =
            detail::screenUnsupportedLayers(*plan, request, frameResolved, cancellation)) {
        return fail(error->code, error->message);
    }

    // #343: what of each image the frame can show, exactly as the CPU reference computes it.
    // #642 B4: a content-pivot layer above an oversampled picture pivots on its input's bounds with
    // nothing clipped, exactly as on the CPU reference.
    std::vector<std::optional<ContentBounds>> contentPivots;
    const auto visible =
        detail::visibleSupportWindows(*plan, frameResolved, request.time, request.output.value(),
                                      fullDisplayWindow, &contentPivots, cancellation);
    const auto matteBound = detail::matteBoundedLayers(*plan, visible);

    // This build's own CPU work counters. They are local to the call -- never shared across
    // concurrent preparation jobs -- and are published either on the prepared scene or, for a
    // failed build, on the returned diagnostic.
    GpuSceneMediaStatistics mediaStatistics;

    std::vector<GpuSceneCommand> commands;
    std::vector<GpuSceneCommandIndex> commandForOperation(operationCount, kInvalidGpuSceneCommand);
    std::vector<EvaluatedOperationBounds> bounds(operationCount);
    std::vector<std::optional<render::ImageWindow>> outputWindowOf(operationCount);
    std::vector<std::string> keyOf(operationCount);
    // Per-operation author-space composed layer matrix (parent*child) and vector chain, exactly as
    // the CPU evaluator computes them in operation order before it evaluates any layer.
    std::vector<std::optional<detail::LayerMatrix>> layerMatrices(operationCount);
    std::vector<std::optional<GpuSceneVectorChain>> vectors(operationCount);
    // #502: a raster layer's resample from the first non-layer image up its chain of layers, as
    // on the CPU: that operation, the composed AUTHOR-space content matrix and the opacity product.
    struct RasterChain {
        std::size_t source;
        detail::LayerMatrix matrix;
        double opacity = 1;
    };
    std::vector<std::optional<RasterChain>> rasterChains(operationCount);
    // Reachable text leaves publish no command of their own; the Layer Output that consumes one
    // builds the coverage command from the layer matrix. This records that consumption so a text
    // that reaches the output by any other route fails closed instead of silently disappearing.
    std::vector<bool> textConsumed(operationCount, false);
    std::vector<bool> shapeConsumed(operationCount, false);

    // #386: footage several layers use is uploaded and converted once per scene.
    detail::GpuSceneMediaCommandMemo mediaCommands;
    const auto emit = [&](GpuSceneCommand command) -> GpuSceneCommandIndex {
        const auto index = static_cast<GpuSceneCommandIndex>(commands.size());
        std::visit([index](auto& item) { item.index = index; }, command);
        commands.push_back(std::move(command));
        return index;
    };

    // The prepared scene RETAINS only host allocations: the frozen upload images and the coverage
    // rasters. Every other command (solid, translation, affine, blend, merge, composition output)
    // is a window + transform description whose RGBA32F output is allocated at executor time under
    // the executor's own LIVE-pin budget. Summing those mutually exclusive output lifetimes into
    // one per-request total refuses graphs the executor -- and the CPU reference -- can actually
    // run (for example a 4608x3164 source over an FHD solid and text). Only the retained host set
    // is bounded here; GPU residency is bounded by GpuSceneExecutor::begin(scene, budget), which
    // takes the honest CPU fallback on refusal. Overflow-checked with subtraction-only comparators.
    std::uint64_t allowance = retainedAllowance;
    std::uint64_t retainedBytes = 0;
    std::uint64_t coverageBytes = 0;
    std::uint64_t sourceBytes = 0;
    auto sourceBackings = phase_.sourceBackings ? phase_.sourceBackings
                                                : std::make_shared<GpuSceneSourceBackingLedger>();
    std::unordered_set<const void*> countedCoverage;
    const auto chargeRetained =
        [&](const void* backing,
            const std::uint64_t bytes) -> std::optional<detail::GpuSceneLeafFailure> {
        if (backing == nullptr || bytes == 0) {
            return detail::GpuSceneLeafFailure{PreparedGpuSceneDiagnosticCode::InvalidPlan,
                                               "Prepared source has no pixel backing"};
        }
        if (sourceBackings->identities.contains(backing)) {
            return std::nullopt;
        }
        if (sourceBackings->chargedBytes > sourceAllowance ||
            bytes > sourceAllowance - sourceBackings->chargedBytes ||
            (!phaseLease && (retainedBytes > allowance || bytes > allowance - retainedBytes))) {
            return detail::GpuSceneLeafFailure{
                PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
                "Prepared scene exceeds the request pixel allowance"};
        }
        sourceBackings->identities.insert(backing);
        sourceBackings->chargedBytes += bytes;
        sourceBytes += bytes;
        retainedBytes += bytes;
        return std::nullopt;
    };
    // GPU-transient outputs are not summed here; the executor's live-pin ledger bounds them.
    const auto chargeTransient = [](const std::uint64_t, const std::uint64_t, const std::uint64_t)
        -> std::optional<detail::GpuSceneLeafFailure> { return std::nullopt; };
    // Charge the ACTUAL retained host coverage bytes once per unique representation, with the exact
    // helper the nested child total uses. It is never width*height*1: the geometry retains O(rows)
    // ranges plus spans, so a simple full-width vector is far smaller than its R8 mask and a
    // complexity-heavy one can be larger.
    const auto chargeCoverage = [&](const GpuSceneCoverageSolidCommand& command)
        -> std::optional<detail::GpuSceneLeafFailure> {
        const void* coverageIdentity = command.coverageIdentity();
        if (coverageIdentity == nullptr || !countedCoverage.insert(coverageIdentity).second) {
            return std::nullopt;
        }
        const std::uint64_t bytes = detail::gpuSceneCoverageHostBytes(command);
        if (phaseLease &&
            (coverageBytes > coverageAllowance || bytes > coverageAllowance - coverageBytes)) {
            if (!phase_.envelope.growsRetainedSceneStorage) {
                return detail::GpuSceneLeafFailure{
                    PreparedGpuSceneDiagnosticCode::BudgetRefused,
                    "Prepared coverage exceeds its admitted retained coverage ceiling"};
            }
            // #343: a dense vector scene retains more geometry than the stated allowance. The rest
            // is claimed from the host root as it is rasterized, so the claim stays what the scene
            // holds and a scene that cannot be held is refused or waits for room.
            const auto growth = std::max<std::uint64_t>(
                bytes - std::min(bytes, coverageAllowance - std::min(coverageAllowance, coverageBytes)),
                kRetainedCoverageClaimStep);
            if (const auto refused = claimRetainedSceneStorage(phaseLease, growth)) {
                return detail::GpuSceneLeafFailure{refused->code, std::move(refused->message)};
            }
            coverageAllowance += growth;
            allowance += growth;
        }
        coverageBytes += bytes;
        if (retainedBytes > allowance || bytes > allowance - retainedBytes) {
            return detail::GpuSceneLeafFailure{
                PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
                "Prepared scene exceeds the request pixel allowance"};
        }
        retainedBytes += bytes;
        return std::nullopt;
    };

    // #549: what the build's channel reads take from each source part, registered before any leaf
    // decodes, so the first decode of a part (its colour layer's, usually) reads them all.
    bool registeredChannelReads = false;
    for (std::size_t index = 0; index < operationCount; ++index) {
        const auto& operation = plan->operations()[index];
        if (!reachable[index] || (!std::holds_alternative<CompiledCryptomatte>(operation) &&
                                  !std::holds_alternative<CompiledChannelShuffle>(operation)))
            continue;
        if (cancellation.isCancellationRequested())
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
        const auto operandClock = plan->operationClock(OperationIndex::fromRaw(index));
        const auto operandTime = detail::timeForClock(frameResolved, request.time, operandClock);
        if (!operandTime)
            continue;
        auto clockRequest = request;
        clockRequest.time = *operandTime;
        detail::registerChannelReads(
            operation, clockRequest, *plan, detail::resolvedForClock(frameResolved, operandClock),
            media, request.bypassOperationCache, plan->bypassOperationCache(), cancellation);
        registeredChannelReads = true;
    }
    // The Image leaves of the GPU colour split that decode a part a channel read takes channels
    // from, so a channel read prepared first decodes that colour layer with its channels.
    for (std::size_t index = 0;
         registeredChannelReads && ocioContext_.preparer != nullptr && index < operationCount;
         ++index) {
        const auto* image = std::get_if<CompiledImageSource>(&plan->operations()[index]);
        if (!reachable[index] || image == nullptr)
            continue;
        if (cancellation.isCancellationRequested())
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
        const auto operandClock = plan->operationClock(OperationIndex::fromRaw(index));
        const auto operandTime = detail::timeForClock(frameResolved, request.time, operandClock);
        if (!operandTime)
            continue;
        auto clockRequest = request;
        clockRequest.time = *operandTime;
        detail::registerColourRead(*image, clockRequest, *plan, media, request.bypassOperationCache,
                                   plan->bypassOperationCache(), cancellation);
    }

    // Test-only checkpoint (empty in production): a test can pause preparation here, request
    // cancellation, then release it so the per-operation cancellation check below observes a
    // genuinely mid-build request deterministically.
    if (checkpoint_) {
        checkpoint_();
    }
    for (std::size_t index = 0; index < operationCount; ++index) {
        if (!reachable[index]) {
            continue;
        }
        if (cancellation.isCancellationRequested()) {
            return fail(PreparedGpuSceneDiagnosticCode::Cancelled, "Preparation was cancelled");
        }
        const OperationIndex operationIndex = OperationIndex::fromRaw(index);
        const auto& operation = plan->operations()[index];
        // ADR 0024: operands, media and nested time on the operation's clock, layer visibility on
        // its placement clock. An absent clock publishes nothing, exactly as the CPU reference.
        const auto operandClock = plan->operationClock(operationIndex);
        const auto& resolved = detail::resolvedForClock(frameResolved, operandClock);
        const auto operandTime = detail::timeForClock(frameResolved, request.time, operandClock);
        const auto placementTime =
            detail::timeForClock(frameResolved, request.time, plan->placementClock(operationIndex));
        if (!operandTime || !placementTime)
            continue;
        auto clockRequest = request;
        clockRequest.time = *operandTime;

        if (const auto* solid = std::get_if<CompiledSolid>(&operation)) {
            if (const auto error = detail::emitSolidCommand(
                    *solid, *plan, resolved, *operandTime, index, operationIndex, fullWindow,
                    fullDisplayWindow, fullPixelAspect, hScale, vScale, emit, chargeTransient,
                    chargeCoverage, commandForOperation, keyOf, outputWindowOf, bounds, vectors)) {
                return fail(error->code, error->message);
            }
            continue;
        }

        if (const auto* checker = std::get_if<CompiledChecker>(&operation)) {
            if (const auto error = detail::emitCheckerCommand(
                    *checker, *plan, resolved, index, operationIndex, fullDisplayWindow,
                    fullPixelAspect, hScale, vScale, emit, chargeTransient, commandForOperation,
                    keyOf, outputWindowOf, bounds)) {
                return fail(error->code, error->message);
            }
            continue;
        }

        if (std::holds_alternative<CompiledText>(operation) ||
            std::holds_alternative<CompiledShape>(operation)) {
            // A text or shape leaf emits no command of its own: the Layer Output that consumes it
            // builds the vector-coverage command from the layer matrix. Consumption is recorded
            // there and checked after the loop, so a vector leaf reaching the output by any other
            // route fails closed. Its own evaluated bounds are still published here so the prepared
            // scene's bounds match the CPU frame exactly, leaf included.
            if (const auto error = detail::publishVectorLeafBounds(
                    operation, *plan, resolved, allowance, hScale, vScale, cancellation, index,
                    outputWindowOf, bounds)) {
                return fail(error->code, error->message, mediaStatistics);
            }
            vectors[index] = GpuSceneVectorChain{index, {}, 1.0};
            continue;
        }

        if (std::holds_alternative<CompiledImageSource>(operation) ||
            std::holds_alternative<CompiledVideoSource>(operation) ||
            std::holds_alternative<CompiledChannelShuffle>(operation) ||
            std::holds_alternative<CompiledCryptomatte>(operation)) {
            // The RAW upload is host-retained (charged against `retainedBytes`); the point-resample
            // and OCIO outputs are GPU-transient and charged through `chargeTransient` (a no-op
            // under main's host-only accounting, where the executor's live-pin ledger bounds GPU
            // residency).
            const auto consumedSource = phaseLease ? sourceBytes : retainedBytes;
            const auto remainingSource =
                sourceAllowance > consumedSource ? sourceAllowance - consumedSource : 0;
            // S caps retained uploads. The existing decoder accepts a working-set ceiling (final
            // image plus band/row scratch), so admit its work from the phase's reserved source
            // scratch (#472: one source's decode/conversion copies, S + scratch in all) while
            // enforcing the returned upload against S through chargeRetained.
            const auto remainingBudget =
                phaseLease ? std::min<std::uint64_t>(
                                 request.pixelStorageByteLimit,
                                 remainingSource > std::numeric_limits<std::uint64_t>::max() -
                                                       phaseLease->sourceScratchBytes()
                                     ? std::numeric_limits<std::uint64_t>::max()
                                     : remainingSource + phaseLease->sourceScratchBytes())
                           : remainingSource;
            detail::GpuSceneUploadLeafResult leaf;
            const auto error = detail::buildAndEmitMediaLeaf(
                operation, operationIndex, clockRequest, *plan, resolved, media, ocioContext_,
                remainingBudget, hScale, vScale, chargeRetained, chargeTransient, cancellation,
                mediaStatistics, emit, mediaCommands, outputWindowOf[index],
                commandForOperation[index], keyOf[index], leaf);
            if (error) {
                // PixelStorageBudgetExceeded names the request's own byte budget. When the
                // admitted phase source envelope, not the request, bounded this leaf, the request
                // could not have been raised to admit it. If the envelope was cut to the room left
                // at admission, a later admission with more room fits it: the frame waits for
                // capacity instead of taking the CPU path (#539). Otherwise it stays
                // MediaUnavailable.
                const bool requestBound =
                    !phaseLease || request.pixelStorageByteLimit <= remainingBudget;
                auto code = error->code;
                if (code == PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded &&
                    !requestBound)
                    code = phaseLease->sourceRoomWasShort()
                               ? PreparedGpuSceneDiagnosticCode::AwaitingCapacity
                               : PreparedGpuSceneDiagnosticCode::MediaUnavailable;
                return fail(code, error->message, mediaStatistics);
            }
            bounds[index] = leaf.bounds;
            continue;
        }

        if (const auto* effect = std::get_if<CompiledImageEffect>(&operation)) {
            const detail::GpuSceneEffectEmissionContext effectContext{
                .plan = plan.get(),
                .request = &clockRequest,
                .resolved = &resolved,
                .frameResolved = &frameResolved,
                .vectors = &vectors,
                .commandForOperation = &commandForOperation,
                .keyOf = &keyOf,
                .outputWindowOf = &outputWindowOf,
                .bounds = &bounds,
                .commands = &commands,
                .fullDisplayWindow = fullDisplayWindow,
                .fullPixelAspect = fullPixelAspect,
                .hScale = hScale,
                .vScale = vScale,
                .allowance = allowance,
                .assetBaseDirectory = media.assetBaseDirectory,
                .coverageCache = coverageCache_,
            };
            if (const auto error = detail::emitImageEffectOperation(
                    *effect, operationIndex, effectContext, ocioContext_, cancellation, emit,
                    chargeTransient, chargeCoverage, commandForOperation, keyOf, outputWindowOf,
                    bounds, vectors, textConsumed, shapeConsumed)) {
                return fail(error->code, error->message, mediaStatistics);
            }
            continue;
        }

        if (const auto* source = std::get_if<CompiledCompositionSource>(&operation)) {
            // A nested composition is prepared by recursively building the CHILD plan with this
            // same production builder and splicing its genuine GPU commands into this list. The
            // child's terminal Composition Output command is aliased as this operation's image; the
            // child's content-addressed command keys are preserved, so an unrelated child edit
            // still reuses every untouched branch. No finished child frame is ever CPU-uploaded.
            detail::GpuSceneNestedResult nested;
            const auto sourceBytesBefore = sourceBackings->chargedBytes;
            const auto error = detail::prepareNestedComposition(
                *source, *plan, clockRequest, resolved, hScale, vScale,
                detail::nestedCompositionDepth(),
                phase_.envelope.growsRetainedSceneStorage ? std::numeric_limits<std::uint64_t>::max()
                                                          : allowance,
                retainedBytes, cancellation,
                [this, &media, &phaseLease, &sourceBackings, sourceAllowance, coverageAllowance,
                 coverageBytes, foregroundLimit = request.pixelStorageByteLimit](
                    const std::shared_ptr<const CompiledCompositionPlan>& childPlan,
                    const EvaluationRequest& childRequest, const CancellationToken& cancel) {
                    // The parent phase covers this bounded nested preparation; a child must not
                    // create another independent reservation for the same retained source set.
                    auto childPhase = phase_;
                    childPhase.admittedLease = phaseLease;
                    childPhase.sourceBackings = sourceBackings;
                    childPhase.remainingSourceBytes = sourceAllowance;
                    childPhase.remainingCoverageBytes =
                        coverageAllowance > coverageBytes ? coverageAllowance - coverageBytes : 0;
                    const CpuGpuSceneBuilder child(coverageCache_, media, ocioContext_,
                                                   std::move(childPhase));
                    auto admittedRequest = childRequest;
                    // Retained quotas are carried separately. They must not become the child's
                    // foreground preflight/decoder working-set ceiling, even without a phase.
                    admittedRequest.pixelStorageByteLimit = foregroundLimit;
                    return child.build(childPlan, admittedRequest, cancel);
                },
                commands, nested, /*sourceChargedExternally=*/true);
            if (error) {
                return fail(error->code, error->message, mediaStatistics);
            }
            const auto newlyCharged = sourceBackings->chargedBytes - sourceBytesBefore;
            // A child that grew the phase's retained storage claimed it from the root already.
            if (phase_.envelope.growsRetainedSceneStorage &&
                nested.coverageResidentBytes > coverageAllowance - coverageBytes) {
                const auto grown =
                    nested.coverageResidentBytes - (coverageAllowance - coverageBytes);
                coverageAllowance += grown;
                allowance += grown;
            }
            if (sourceBytes > sourceAllowance || newlyCharged > sourceAllowance - sourceBytes ||
                nested.coverageResidentBytes > coverageAllowance - coverageBytes) {
                return fail(PreparedGpuSceneDiagnosticCode::BudgetRefused,
                            "Nested retained source/coverage exceeds its parent's admitted quotas");
            }
            if (!phaseLease &&
                (retainedBytes > allowance || newlyCharged > allowance - retainedBytes)) {
                return fail(PreparedGpuSceneDiagnosticCode::PixelStorageBudgetExceeded,
                            "Nested retained sources exceed the request pixel allowance");
            }
            sourceBytes += newlyCharged;
            retainedBytes += newlyCharged;
            coverageBytes += nested.coverageResidentBytes;
            if (!nested.outputWindow.has_value()) {
                return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                            "Nested composition published no output window", mediaStatistics);
            }
            bounds[index] = nested.bounds;
            outputWindowOf[index] = *nested.outputWindow;
            keyOf[index] = nested.outputKey;
            commandForOperation[index] = nested.outputCommand;
            continue;
        }

        if (const auto* layer = std::get_if<CompiledLayerOutput>(&operation)) {
            const auto position = detail::resolveParameter(layer->position, *plan, resolved);
            const auto anchor = detail::resolveParameter(layer->anchor, *plan, resolved);
            const auto scale = detail::resolveParameter(layer->scale, *plan, resolved);
            const auto rotation = detail::resolveParameter(layer->rotation, *plan, resolved);
            const auto opacity = detail::resolveLayerOpacity(*layer, *plan, resolved);
            if (!position || !anchor || !scale || !rotation || !opacity) {
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                            "Layer parameters are not evaluable");
            }
            const std::size_t inputIndexValue = layer->input.value();
            if (inputIndexValue >= operationCount) {
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan, "Layer input is invalid");
            }
            // The input's command, window and authored content bounds. A vector leaf (solid, text,
            // shape) published them when it was reached; a merge or another layer published its own
            // raster command and bounds. A parent chain is resolved through the parent matrix.
            const auto& inputVec = vectors[inputIndexValue];
            // A layer that publishes no image (zero scale, outside its range, empty or degenerate
            // bounds, a non-invertible matrix) still accounts for the vector leaf it carries: the
            // leaf is invisible by design, not unrepresented. Without this the unconsumed-leaf
            // check below sent the whole frame to the CPU route.
            const auto accountForInvisibleLeaf = [&] {
                if (!inputVec.has_value()) {
                    return;
                }
                const auto source = inputVec->source;
                if (std::holds_alternative<CompiledText>(plan->operations()[source])) {
                    textConsumed[source] = true;
                } else if (std::holds_alternative<CompiledShape>(plan->operations()[source])) {
                    shapeConsumed[source] = true;
                }
            };
            const GpuSceneCommandIndex inputIndex = commandForOperation[inputIndexValue];
            std::optional<render::ImageWindow> sourceWindowHolder;
            ContentBounds inputLocal;
            if (inputVec.has_value()) {
                const auto& leafWindow = outputWindowOf[inputIndexValue];
                if (!leafWindow) {
                    accountForInvisibleLeaf();
                    continue;
                }
                sourceWindowHolder = *leafWindow;
                inputLocal = bounds[inputIndexValue].output;
            } else {
                const auto& inputWindow = outputWindowOf[inputIndexValue];
                if (inputIndex == kInvalidGpuSceneCommand || !inputWindow) {
                    continue;
                }
                sourceWindowHolder = *inputWindow;
                inputLocal = bounds[inputIndexValue].output;
            }
            // #642 B4: a clipped content pivot's input bounds are the unclipped frame's, as on the
            // CPU reference.
            if (contentPivots[index])
                inputLocal = *contentPivots[index];
            // The CPU evaluator builds the matrix from the input's CONTENT-BOUNDS centre, composes
            // the parent matrix (which retains shear and inherits neither visibility nor opacity),
            // and records it for every layer whether or not the layer is active. The layer's own
            // published bounds are only written inside its active range, exactly as on the CPU.
            // #502: a frame pivot is the composition frame's centre, as on the CPU.
            // #642: a content scale's input is that many times larger; the layer's pivot, anchor
            // and local bounds are the picture's at the frame's scale, as on the CPU.
            const double contentScale = layer->contentScale;
            // #642: drawing into an oversampled group resamples through the content matrix too
            // (which carries the output scale), as on the CPU.
            const bool scaledContent = contentScale != 1.0 || layer->outputScale != 1.0;
            const auto inputCentre = inputLocal.pivot();
            const auto centre =
                layer->framePivot
                    ? decltype(inputLocal.pivot()){static_cast<double>(plan->format().width()) /
                                                       2.0,
                                                   static_cast<double>(plan->format().height()) /
                                                       2.0}
                    : decltype(inputLocal.pivot()){inputCentre.x / contentScale,
                                                   inputCentre.y / contentScale};
            const auto authoredMatrix = detail::LayerMatrix::authored(
                position->value, {centre.x + anchor->value.x, centre.y + anchor->value.y},
                scale->value, rotation->value);
            // #381: the layer's own content carries its footage pixel aspect; children compose
            // the matrix without it.
            const auto contentAuthored = detail::LayerMatrix::content(
                position->value, {centre.x + anchor->value.x, centre.y + anchor->value.y},
                scale->value, rotation->value, layer->sourcePixelAspect, contentScale);
            detail::LayerMatrix contentComposed = contentAuthored;
            detail::LayerMatrix composed = authoredMatrix;
            if (layer->parent) {
                const auto parentIndex = layer->parent->value();
                if (parentIndex >= operationCount) {
                    return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                                "Layer parent matrix is unavailable");
                }
                const auto& parentMatrix = layerMatrices[parentIndex];
                if (!parentMatrix.has_value()) {
                    return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                                "Layer parent matrix is unavailable");
                }
                composed = parentMatrix->times(authoredMatrix);
                contentComposed = parentMatrix->times(contentAuthored);
            }
            layerMatrices[index] = composed;
            // #642: drawn into an oversampled group, its pixels land that many times larger; its
            // children compose its plain matrix.
            contentComposed = contentComposed.enlarged(layer->outputScale);
            std::optional<detail::GpuSceneVectorChain> chained;
            if (inputVec.has_value()) {
                chained = GpuSceneVectorChain{inputVec->source,
                                              (contentScale != 1.0
                                                   ? contentComposed
                                                   : composed.enlarged(layer->outputScale))
                                                  .times(inputVec->matrix),
                                              inputVec->opacity * opacity->value};
                vectors[index] = *chained;
            } else {
                const auto& inner = rasterChains[inputIndexValue];
                rasterChains[index] =
                    inner ? RasterChain{inner->source, contentComposed.times(inner->matrix),
                                        inner->opacity * opacity->value}
                          : RasterChain{inputIndexValue, contentComposed, opacity->value};
            }
            // The CPU Layer Output stage publishes no image and no bounds outside its active range.
            if (*placementTime < layer->inPoint ||
                (layer->outPoint.has_value() && *placementTime >= *layer->outPoint)) {
                accountForInvisibleLeaf();
                continue;
            }
            // Like the CPU evaluator, a layer that collapses to no area still publishes its layer
            // id and local bounds; it publishes no output bounds and no image.
            auto& geometry = bounds[index];
            geometry.layerId = layer->layerId;
            geometry.local = inputLocal;
            if (scale->value.x == 0.0 || scale->value.y == 0.0 || inputLocal.empty()) {
                accountForInvisibleLeaf();
                continue;
            }
            const auto sourceWindow = *sourceWindowHolder;
            const auto bufferCentre =
                detail::boundsForWindow(sourceWindow, hScale, vScale).centre();
            render::LayerTransform::Authored authored{
                .anchorX = anchor->value.x,
                .anchorY = anchor->value.y,
                .scaleX = scale->value.x * layer->sourcePixelAspect,
                .scaleY = scale->value.y,
                .rotationDegrees = rotation->value,
                .opacity = opacity->value,
            };
            authored.translationX = position->value.x - centre.x - anchor->value.x;
            authored.translationY = position->value.y - centre.y - anchor->value.y;
            authored.anchorX = centre.x - bufferCentre.x + anchor->value.x;
            authored.anchorY = centre.y - bufferCentre.y + anchor->value.y;
            if (!std::isfinite(authored.translationX * hScale) ||
                !std::isfinite(authored.translationY * vScale)) {
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                            "Layer position produces a non-finite translation");
            }
            const auto transform =
                render::LayerTransform::create(authored, sourceWindow, hScale, vScale);
            if (!transform) {
                return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                            "Layer transform is not evaluable");
            }
            std::optional<detail::ParentedLayerTransform> parented;
            if (layer->parent || scaledContent) {
                parented.emplace(contentComposed, sourceWindow, hScale, vScale,
                                 static_cast<float>(opacity->value));
                if (!parented->finite()) {
                    return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                                "Parented transform exceeds the supported numeric range");
                }
                if (!parented->invertible()) {
                    accountForInvisibleLeaf();
                    continue;
                }
            }
            const auto workingWindow =
                render::ImageWindow::create(-16777216, -16777216, 33554432, 33554432);
            const auto supportWindow =
                parented ? parented->supportBounds(*workingWindow.value())
                         : transform.value()->supportBounds(*workingWindow.value());
            if (!supportWindow) {
                accountForInvisibleLeaf();
                continue;
            }
            const auto& transformValue = *transform.value();
            const auto forward = [&](const double x, const double y) {
                return parented ? parented->forwardMap(x, y) : transformValue.forwardMap(x, y);
            };
            const auto map = [&](const document::Vec2d point) {
                const auto mapped =
                    forward(point.x * hScale - 0.5 - static_cast<double>(sourceWindow.originX()),
                            point.y * vScale - 0.5 - static_cast<double>(sourceWindow.originY()));
                return document::Vec2d{(mapped.x + 0.5) / hScale, (mapped.y + 0.5) / vScale};
            };
            geometry.polygon = detail::boundsCorners(geometry.local);
            for (auto& point : geometry.polygon) {
                point = map(point);
            }
            geometry.output = {geometry.polygon[0].x, geometry.polygon[0].y, geometry.polygon[0].x,
                               geometry.polygon[0].y};
            for (const auto point : geometry.polygon) {
                geometry.output.left = std::min(geometry.output.left, point.x);
                geometry.output.top = std::min(geometry.output.top, point.y);
                geometry.output.right = std::max(geometry.output.right, point.x);
                geometry.output.bottom = std::max(geometry.output.bottom, point.y);
            }
            geometry.anchor = map({(centre.x + anchor->value.x) * contentScale,
                                   (centre.y + anchor->value.y) * contentScale});
            if (scaledContent)
                geometry.local = {geometry.local.left / contentScale, geometry.local.top / contentScale,
                                  geometry.local.right / contentScale, geometry.local.bottom / contentScale,
                                  geometry.local.pivotX ? std::optional(*geometry.local.pivotX / contentScale)
                                                        : std::nullopt};
            // #642: a composed Vector Artwork layer reports its artwork's box through the carrier's
            // matrix, exactly as on the CPU.
            if (layer->composedArtwork) {
                const auto carrierIndex = layer->composedArtwork->carrier.value();
                if (carrierIndex >= operationCount || !layerMatrices[carrierIndex].has_value() ||
                    !detail::publishComposedArtworkBounds(*layer, *plan, frameResolved, bounds,
                                                          *layerMatrices[carrierIndex], geometry)) {
                    return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                                "Vector Artwork layer's transform carrier is unavailable");
                }
            }
            // #343: only what the frame can show (and its track matte lets through) is drawn, as on
            // the CPU reference; the drawn window is the lattice the command resamples over.
            const auto drawnWindow =
                [&](const render::ImageWindow window) -> std::optional<render::ImageWindow> {
                auto drawn = detail::clipToVisible(window, visible[index]);
                if (drawn && matteBound[index])
                    drawn = detail::clipToVisible(*drawn, outputWindowOf[*matteBound[index]]);
                return drawn;
            };
            const auto layerWindow = drawnWindow(*supportWindow);
            if (!layerWindow) {
                accountForInvisibleLeaf();
                continue;
            }
            outputWindowOf[index] = *layerWindow;

            const auto isNativeGrid = [&](const detail::LayerMatrix& matrix) {
                return matrix.a == 1 && matrix.b == 0 && matrix.c == 0 && matrix.d == 1 &&
                       matrix.x * hScale == std::floor(matrix.x * hScale) &&
                       matrix.y * vScale == std::floor(matrix.y * vScale);
            };
            const bool nativeGrid = isNativeGrid(contentComposed);
            // The full AUTHOR-space chain matrix the CPU rasterizes through: the enclosing layers'
            // composed matrix multiplied into the input's own chain. For a direct leaf this is just
            // the layer's own matrix.
            const detail::LayerMatrix chainMatrix =
                chained.has_value() ? chained->matrix : composed;

            if (inputVec.has_value()) {
                // A vector layer (direct leaf or a chain through enclosing layers) is rasterized by
                // the CPU evaluator's PathRaster through the FULL composed matrix, never resampled
                // as an intermediate raster. A direct solid on the integer grid keeps the exact
                // optimized translation command; every other vector layer emits coverage.
                const std::size_t chainSource = inputVec->source;
                const auto& leafOperation = plan->operations()[chainSource];
                const bool direct = chainSource == inputIndexValue;
                if (direct && !layer->parent &&
                    std::holds_alternative<CompiledSolid>(leafOperation) && nativeGrid) {
                    const auto delta = detail::translationOnlyDeviceDelta(
                        transformValue, sourceWindow, *layerWindow);
                    if (!delta.has_value()) {
                        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                                    "Translation-only transform lost its device translation");
                    }
                    if (const auto error = detail::emitTranslationOpacityCommand(
                            inputIndex, keyOf[inputIndexValue], sourceWindow, *layerWindow,
                            delta->first, delta->second, opacity->value, fullPixelAspect,
                            operationIndex, chargeTransient, emit, commandForOperation[index],
                            keyOf[index])) {
                        return fail(error->code, error->message);
                    }
                    continue;
                }
                GpuSceneCommandIndex composedIndex = kInvalidGpuSceneCommand;
                std::string semanticKey;
                bool consumedTextLeaf = false;
                bool consumedShapeLeaf = false;
                const bool leafPath = direct && nativeGrid;
                if (const auto error = detail::emitVectorLeafLayer(
                        leafOperation, *plan,
                        detail::resolvedForClock(
                            frameResolved,
                            plan->operationClock(OperationIndex::fromRaw(chainSource))),
                        chainMatrix, *layerWindow, fullDisplayWindow, fullPixelAspect, hScale,
                        vScale, inputVec->opacity * opacity->value, leafPath && nativeGrid,
                        allowance, operationIndex, coverageCache_, cancellation, emit,
                        chargeTransient, chargeCoverage, composedIndex, semanticKey,
                        consumedTextLeaf, consumedShapeLeaf)) {
                    return fail(error->code, error->message);
                }
                keyOf[index] = semanticKey;
                commandForOperation[index] = composedIndex;
                textConsumed[chainSource] = textConsumed[chainSource] || consumedTextLeaf;
                shapeConsumed[chainSource] = shapeConsumed[chainSource] || consumedShapeLeaf;
                continue;
            }

            // #502: fed by another raster layer, this layer resamples the chain's first non-layer
            // image once through the composed matrix, exactly as the CPU evaluator does. Its
            // bounds still follow its own transform of the earlier layer's bounds.
            if (const auto& chain = rasterChains[inputIndexValue];
                chain && commandForOperation[chain->source] != kInvalidGpuSceneCommand &&
                outputWindowOf[chain->source].has_value()) {
                const auto chainWindow = *outputWindowOf[chain->source];
                const auto concatenatedMatrix = contentComposed.times(chain->matrix);
                const auto concatenatedOpacity = opacity->value * chain->opacity;
                const detail::ParentedLayerTransform concatenated(
                    concatenatedMatrix, chainWindow, hScale, vScale,
                    static_cast<float>(concatenatedOpacity));
                if (!concatenated.finite()) {
                    return fail(PreparedGpuSceneDiagnosticCode::InvalidPlan,
                                "Concatenated transform exceeds the supported numeric range");
                }
                if (!concatenated.invertible()) {
                    outputWindowOf[index].reset();
                    continue;
                }
                const auto chainSupport = concatenated.supportBounds(*workingWindow.value());
                const auto chainLayerWindow =
                    chainSupport ? drawnWindow(*chainSupport) : std::nullopt;
                if (!chainLayerWindow) {
                    outputWindowOf[index].reset();
                    continue;
                }
                outputWindowOf[index] = *chainLayerWindow;
                if (const auto error = detail::emitAffineLayerCommand(
                        commandForOperation[chain->source], keyOf[chain->source], chainWindow,
                        *chainLayerWindow, concatenatedMatrix, hScale, vScale, concatenatedOpacity,
                        fullPixelAspect, operationIndex, chargeTransient, emit,
                        commandForOperation[index], keyOf[index])) {
                    return fail(error->code, error->message);
                }
                continue;
            }
            // A raster input (media, merge, or another raster layer). The exact translation path is
            // retained when it applies; every other affine appears on the accepted GpuAffine
            // command so rotation, nonuniform/signed scale, anchor and any parent-composed shear
            // are exact.
            if (!layer->parent && !scaledContent) {
                if (const auto delta = detail::translationOnlyDeviceDelta(
                        transformValue, sourceWindow, *layerWindow)) {
                    if (const auto error = detail::emitTranslationOpacityCommand(
                            inputIndex, keyOf[inputIndexValue], sourceWindow, *layerWindow,
                            delta->first, delta->second, opacity->value, fullPixelAspect,
                            operationIndex, chargeTransient, emit, commandForOperation[index],
                            keyOf[index])) {
                        return fail(error->code, error->message);
                    }
                    continue;
                }
            }
            if (const auto error = detail::emitAffineLayerCommand(
                    inputIndex, keyOf[inputIndexValue], sourceWindow, *layerWindow, contentComposed,
                    hScale, vScale, opacity->value, fullPixelAspect, operationIndex,
                    chargeTransient, emit, commandForOperation[index], keyOf[index])) {
                return fail(error->code, error->message);
            }
            continue;
        }

        if (const auto* stack = std::get_if<CompiledMerge>(&operation)) {
            // A vector leaf entry is rasterized at its own window with its chain matrix, as an
            // image effect over the same leaf materializes it, and is counted as consumed.
            const auto materialize =
                [&](const std::size_t entryIndex, const render::ImageWindow window,
                    GpuSceneCommandIndex& command, std::string& key,
                    bool& materialized) -> std::optional<detail::GpuSceneLeafFailure> {
                const auto& chain = vectors[entryIndex];
                if (!chain.has_value()) {
                    return std::nullopt;
                }
                if (chain->source >= operationCount) {
                    return detail::GpuSceneLeafFailure{PreparedGpuSceneDiagnosticCode::InvalidPlan,
                                                       "A merge entry's vector chain is invalid"};
                }
                bool consumedText = false;
                bool consumedShape = false;
                if (const auto error = detail::emitVectorLeafLayer(
                        plan->operations()[chain->source], *plan,
                        detail::resolvedForClock(
                            frameResolved,
                            plan->operationClock(OperationIndex::fromRaw(chain->source))),
                        chain->matrix, window, fullDisplayWindow, fullPixelAspect, hScale, vScale,
                        chain->opacity, false, allowance, operationIndex, coverageCache_,
                        cancellation, emit, chargeTransient, chargeCoverage, command, key,
                        consumedText, consumedShape, true)) {
                    return error;
                }
                textConsumed[chain->source] = textConsumed[chain->source] || consumedText;
                shapeConsumed[chain->source] = shapeConsumed[chain->source] || consumedShape;
                materialized = true;
                return std::nullopt;
            };
            if (const auto error = detail::emitMergeCommand(
                    *stack, *plan, resolved, index, bounds, outputWindowOf, commandForOperation,
                    keyOf, operationIndex, emit, chargeTransient, materialize)) {
                return fail(error->code, error->message);
            }
            continue;
        }

        if (const auto* output = std::get_if<CompiledCompositionOutput>(&operation)) {
            if (const auto error = detail::emitCompositionOutputCommand(
                    *output, resolved, index, operationIndex, emit, chargeTransient,
                    commandForOperation, keyOf, bounds)) {
                return fail(error->code, error->message);
            }
            continue;
        }

        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "Reachable operation was not handled");
    }

    // A reachable vector leaf that no prepared layer consumed has no representation in this subset;
    // fail closed rather than publish a scene whose output silently omits it.
    if (auto unconsumed =
            detail::describeUnconsumedVectorLeaf(*plan, reachable, textConsumed, shapeConsumed)) {
        return fail(PreparedGpuSceneDiagnosticCode::UnsupportedOperation, std::move(*unconsumed));
    }
    // #343: the admission took each image as one frame. Now that every image has its window, the
    // phase needs the executor's live set at those sizes, and claims it when that is more (an image
    // larger than the frame, such as a gradient drawn in its own window). The device admits, waits
    // for or refuses the larger need as it did the first. Each image is counted at its requested
    // extent rounded to what the device's allocator takes for it (probed from the driver and
    // reported to the root with the device's budget), so the claim is what the executor is charged.
    if (phaseLease && !phase_.admittedLease && phase_.envelope.native) {
        const auto extent = frameResolved.imageDescriptor.dataWindow().extent();
        // Each image at what the device's allocator takes for it, not its requested extent.
        const render::GpuAllocationSizes allocated{
            [&phaseLease](const std::uint32_t width, const std::uint32_t height) {
                return phaseLease->imageAllocationBytes(width, height);
            },
            [&phaseLease](const std::uint64_t bytes) {
                return phaseLease->bufferAllocationBytes(bytes);
            }};
        const auto frameBytes = allocated.image(extent.width(), extent.height());
        const auto live = detail::gpuExecutorLivePeakBytes(*plan, request.output.value(),
                                                           frameBytes, &outputWindowOf, &allocated,
                                                           {extent.width(), extent.height()});
        switch (phaseLease->raiseExecutorNeed(live)) {
        case render::GpuProcessingPhaseAdmission::Accepted:
            // #682: the device caches keep this much free for the next frame of the same work.
            if (phase_.policy)
                phase_.policy->noteDeviceWorkNeed(phaseLease->deviceNeedBytes());
            break;
        case render::GpuProcessingPhaseAdmission::AwaitingCapacity:
            // #682: a phase that must grow waits like a reservation does, so the device caches
            // are asked for the shortfall now instead of holding the device until the next
            // capacity sample.
            if (phase_.policy)
                phase_.policy->reclaimFor(render::GpuProcessingPhaseDomain::Device,
                                          phaseLease->deviceNeedBytes() + live);
            return fail(PreparedGpuSceneDiagnosticCode::AwaitingCapacity,
                        "GPU preparation is awaiting device-memory capacity for the scene's images",
                        mediaStatistics);
        case render::GpuProcessingPhaseAdmission::BudgetRefused:
            return fail(PreparedGpuSceneDiagnosticCode::BudgetRefused,
                        "The scene's images do not fit the current device-memory capacity",
                        mediaStatistics);
        default:
            return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                        "The scene's device need could not be raised", mediaStatistics);
        }
    }

    ProcessFrameIdentity identity{
        .plan = plan,
        .time = request.time,
        .output = request.output,
        .resolution = request.resolution,
        .quality = request.quality,
        .colorIntent = request.colorIntent,
        .provider = EvaluationProvider::CpuReference,
        .evaluatorSemanticsVersion = kCpuCompositionEvaluatorSemanticsVersion,
        .animationSamplingSemanticsVersion = plan->animationSamplingSemanticsVersion(),
        .imagePrimitiveSemanticsVersion = render::kCpuImagePrimitiveSemanticsVersion,
        .roi = request.roi,
        .bypassLookNodes = request.bypassLookNodes,
    };
    const GpuSceneCommandIndex outputCommand = commandForOperation[request.output.value()];
    // #642: a layer drawing into an oversampled group publishes its bounds at a scale of 1, as on
    // the CPU.
    detail::publishOutputScaledBounds(*plan, bounds);
    auto scene = std::shared_ptr<const PreparedGpuScene>(new PreparedGpuScene(
        std::move(commands), std::move(commandForOperation), outputCommand, std::move(identity),
        std::move(bounds), frameResolved.imageDescriptor, mediaStatistics));
    // Nested builders borrow the root phase and must not retire its preparation scratch while
    // siblings can still decode. The shared backing ledger deduplicates all nested source pixels.
    // #343: the command list of a scene past the phase's metadata allowance, and the executor's plan
    // of it, are host memory like its coverage. A nested build's commands are spliced into the root's.
    if (phaseLease && !phase_.admittedLease && phase_.envelope.growsRetainedSceneStorage) {
        const auto storage = gpuSceneCommandStorageBytes(scene->commands().size());
        if (storage > phase_.envelope.metadataBytes) {
            if (auto refused = claimRetainedSceneStorage(
                    phaseLease, storage - phase_.envelope.metadataBytes)) {
                return fail(refused->code, std::move(refused->message), mediaStatistics);
            }
        }
    }
    if (phaseLease && !phase_.admittedLease &&
        !phaseLease->finishSourcePreparation(sourceBackings->chargedBytes)) {
        return fail(PreparedGpuSceneDiagnosticCode::InternalInvariant,
                    "Prepared source storage could not finish its processing reservation");
    }
    return {std::move(scene), PreparedGpuSceneDiagnostic{}, std::move(phaseLease)};
}

} // namespace bloom::runtime
