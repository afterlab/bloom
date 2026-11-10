#include <bloom/ui/gpu_viewer_bootstrap.hpp>

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/runtime/gpu_ocio_context.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/ui/composition_preview_gpu_scene_stage.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace bloom::ui {

namespace {

GpuResidentBudgetPlan budgetPlanFrom(const runtime::GpuResidentCapacityPlan& plan) noexcept {
    return GpuResidentBudgetPlan{.residentPoolBytes = plan.poolBytes,
                                 .cacheBytes = plan.cacheBytes,
                                 .cacheEntries = static_cast<std::size_t>(plan.cacheEntries),
                                 .leaseBytes = plan.leaseBytes,
                                 .leaseEntries = static_cast<std::size_t>(plan.leaseEntries),
                                 .sceneCacheBytes = plan.sceneCacheBytes,
                                 .requestBytes = plan.requestBytes};
}

} // namespace

GpuResidentBudgetPlan gpuResidentBudgetPlanFor(const std::size_t ceilingBytes) noexcept {
    // One configured ceiling for every ledger, no partition (#425). The owner resolves the real
    // device budget and clamps this plan through the existing status poll. Zero stays zero.
    return budgetPlanFrom(
        runtime::gpuResidentCapacityPlanConfigured(static_cast<std::uint64_t>(ceilingBytes)));
}

void applyGpuResidentBudgetPlan(runtime::GpuPreviewDisplayServiceOptions& options,
                                const GpuResidentBudgetPlan& plan) noexcept {
    options.residentLeaseBudgets.maxBytes = plan.leaseBytes;
    options.residentLeaseBudgets.maxEntries = plan.leaseEntries;
    options.residentSceneCacheBudgets.maxRetainedBytes = plan.sceneCacheBytes;
    // Only ever lower the configured allowance.
    if (plan.requestBytes > 0 &&
        plan.requestBytes <
            options.previewByteAllowance.value_or(runtime::gpuPreviewRequestByteAllowance())) {
        options.previewByteAllowance = static_cast<std::size_t>(plan.requestBytes);
    }
}

void applyGpuResidentCacheLimits(PreviewFrameCache& cache,
                                 const GpuResidentBudgetPlan& plan) noexcept {
    cache.setGpuResidentLimits(static_cast<std::size_t>(plan.cacheBytes), plan.cacheEntries);
}

void applyGpuResidentCacheLimits(PreviewFrameCache& cache,
                                 const runtime::GpuResidentCapacityPlan& plan) noexcept {
    cache.setGpuResidentLimits(static_cast<std::size_t>(plan.cacheBytes),
                               static_cast<std::size_t>(plan.cacheEntries));
}

bool shouldRequestWaylandPresentation(const bool bundledNativeLoader,
                                      const bool waylandSession) noexcept {
    return bundledNativeLoader && waylandSession;
}

bool gpuPresentationAvailable(const render::GpuPresentationAvailability availability) noexcept {
    return availability == render::GpuPresentationAvailability::Ready;
}

bool gpuViewerClientUsable(const std::shared_ptr<runtime::GpuPresentationClient>& client,
                           const render::GpuPresentationAvailability availability) noexcept {
    return client != nullptr && gpuPresentationAvailable(availability);
}

runtime::GpuSceneMediaContext
gpuSceneMediaContextFor(const runtime::CpuCompositionEvaluator& evaluator) {
    return runtime::GpuSceneMediaContext::fromEvaluator(evaluator);
}

runtime::PreviewGpuSceneStageFunction makeSessionRefreshingGpuSceneStage(
    const runtime::SnapshotCompiler& compiler, const runtime::CpuCompositionEvaluator& evaluator,
    const runtime::QualifiedDisplayProcessorProvider& qualifiedProcessorProvider,
    std::shared_ptr<runtime::GpuSceneCoverageCache> coverageCache,
    CompiledPlanCacheHandle planCache,
    std::shared_ptr<runtime::GpuOcioContextResolver> ocioContextResolver,
    runtime::GpuScenePreparationPhase processingPhase,
    runtime::ImageStatisticsDeviceMeasure statisticsMeasure) {
    // The one general-display service is created ONCE and consumes the shared resolver, so the
    // display program uses the exact same shared GpuOcioProgramPreparer as the scene builder.
    auto displayProgramService =
        std::make_shared<const runtime::GpuDisplayProgramService>(ocioContextResolver);
    return [&compiler, &evaluator, &qualifiedProcessorProvider,
            coverageCache = std::move(coverageCache), planCache = std::move(planCache),
            ocioContextResolver = std::move(ocioContextResolver),
            displayProgramService = std::move(displayProgramService),
            processingPhase = std::move(processingPhase),
            statisticsMeasure = std::move(statisticsMeasure)](
               const document::Snapshot& snapshot,
               const runtime::PreviewRequestIdentity& desiredIdentity,
               const std::size_t pixelStorageByteLimit,
               const std::vector<runtime::SnapshotParameterOverride>& interactionOverride,
               runtime::TaskContext& context)
               -> runtime::TaskResult<runtime::PreviewGpuSceneStageOutcomeHandle> {
        using Result = runtime::TaskResult<runtime::PreviewGpuSceneStageOutcomeHandle>;
        // The local builder owns the copied media context for exactly this invocation. The
        // evaluator's getters are internally locked, so a concurrent Open/SaveAs base-directory
        // update is observed as a whole, never as a torn path.
        auto mediaContext = gpuSceneMediaContextFor(evaluator);
        // Off-UI shared-tool resolution. The one resolver is idempotent: the first request
        // qualifies the packaged tools and every later/concurrent request reuses the same context +
        // preparer. A failed resolve leaves the builder's fail-closed default context so
        // effects/media/ACES transforms refuse (Unsupported) and the request takes the CPU path
        // with a reason.
        runtime::GpuSceneOcioContext ocioContext;
        std::optional<runtime::TaskDiagnostic> unresolved;
        if (ocioContextResolver != nullptr) {
            runtime::GpuOcioCancellation cancel = [&context] {
                return context.isCancellationRequested();
            };
            auto resolved = ocioContextResolver->resolve(cancel);
            // A cold first frame qualifies the tools (hashing and starting them) on a machine that
            // may be busy: a time-out or an I/O failure says nothing about the tools, and a failed
            // resolve is not remembered, so the frame tries once more before it gives up.
            if (!resolved.hasValue() && (resolved.error == runtime::GpuOcioContextError::Timeout ||
                                         resolved.error == runtime::GpuOcioContextError::IoFailure))
                resolved = ocioContextResolver->resolve(cancel);
            if (!resolved.hasValue() && resolved.error == runtime::GpuOcioContextError::Cancelled) {
                return Result::cancelled();
            }
            if (resolved.hasValue()) {
                ocioContext = *resolved.context;
            } else {
                unresolved = runtime::TaskDiagnostic{
                    .code = "bloom.preview.gpu-scene.tools-unresolved",
                    .severity = runtime::DiagnosticSeverity::Warning,
                    .summary = "The GPU shader tools could not be prepared (" +
                               std::string(runtime::gpuOcioContextErrorName(resolved.error)) +
                               "), so GPU programs are unavailable for this frame",
                    .detail = resolved.diagnostic,
                    .suggestedAction = "Take the full CPU composition preview path."};
            }
        }
        const runtime::CpuGpuSceneBuilder builder(coverageCache, std::move(mediaContext),
                                                  std::move(ocioContext), processingPhase);
        auto stage = makeCompositionPreviewGpuSceneStage(compiler, builder,
                                                         qualifiedProcessorProvider, planCache,
                                                         displayProgramService, statisticsMeasure);
        auto result =
            stage(snapshot, desiredIdentity, pixelStorageByteLimit, interactionOverride, context);
        // A frame that leaves the GPU subset because the tools were not ready names that reason
        // first, ahead of the operation the builder could not prepare without them.
        if (unresolved && result.state() == runtime::TaskState::Succeeded &&
            result.value().has_value() && *result.value() != nullptr &&
            (*result.value())->status == runtime::PreviewGpuSceneStageStatus::UnsupportedGpuSubset) {
            auto outcome = **result.value();
            outcome.diagnostics.insert(outcome.diagnostics.begin(), std::move(*unresolved));
            return Result::succeeded(
                std::make_shared<const runtime::PreviewGpuSceneStageOutcome>(std::move(outcome)));
        }
        return result;
    };
}

GpuViewerBootstrap::GpuViewerBootstrap(runtime::TaskScheduler& scheduler,
                                       std::string vulkanLoaderPath, const double devicePixelRatio)
    : scheduler_(&scheduler), loaderPath_(std::move(vulkanLoaderPath)),
      devicePixelRatio_(devicePixelRatio) {}

void GpuViewerBootstrap::bindResidentCache(PreviewFrameCache& cache) noexcept {
    const std::lock_guard lock(mutex_);
    residentCache_ = &cache;
}

void GpuViewerBootstrap::refreshFromStatus(const runtime::GpuPreviewDisplayServiceStatus& status) {
    std::shared_ptr<runtime::GpuPresentationClient> next;
    if (gpuViewerClientUsable(status.presentationClient, status.presentationAvailability)) {
        next = status.presentationClient;
    }
    std::function<void(const ViewerGpuDependencies&)> sink;
    bool changed = false;
    {
        const std::lock_guard lock(mutex_);
        changed = next != client_;
        client_ = std::move(next);
        active_ = client_ != nullptr;
        if (changed) {
            ++publicationCount_;
        }
        // Install exactly the owner-resolved plan. A configured host-only plan or the default
        // (unresolved) status is ignored; a resolved plan is applied even when it is the
        // conservative unknown-device fallback or a known-zero device that yields no resident
        // subset, so a stale configured limit never survives a genuine owner resolution.
        const bool published = status.residentCapacityPlan.resolved;
        if (residentCache_ != nullptr && published &&
            status.residentCapacityPlan != appliedResidentPlan_) {
            applyGpuResidentCacheLimits(*residentCache_, status.residentCapacityPlan);
            appliedResidentPlan_ = status.residentCapacityPlan;
        }
        // Work waits for device memory the device caches could not give back: finished resident
        // frames go last, least recently used first (#425).
        if (residentCache_ != nullptr &&
            status.residentFrameEvictionSerial != appliedResidentFrameEvictionSerial_) {
            appliedResidentFrameEvictionSerial_ = status.residentFrameEvictionSerial;
            static_cast<void>(residentCache_->evictGpuResidentBytes(
                static_cast<std::size_t>(status.residentFrameEvictionBytes)));
        }
        sink = sink_;
    }
    if (changed && sink) {
        sink(dependencies());
    }
}

ViewerGpuDependencies GpuViewerBootstrap::dependencies() const {
    return ViewerGpuDependencies{
        .presentationClient =
            [this] {
                const std::lock_guard lock(mutex_);
                return client_;
            },
        .scheduler = scheduler_,
        .vulkanLoaderPath = loaderPath_,
        .devicePixelRatio = devicePixelRatio_,
    };
}

bool GpuViewerBootstrap::clientAvailable() const noexcept {
    const std::lock_guard lock(mutex_);
    return active_;
}

std::uint64_t GpuViewerBootstrap::publicationCount() const noexcept {
    const std::lock_guard lock(mutex_);
    return publicationCount_;
}

void GpuViewerBootstrap::setPublicationSink(
    std::function<void(const ViewerGpuDependencies&)> sink) {
    const std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
}

} // namespace bloom::ui
