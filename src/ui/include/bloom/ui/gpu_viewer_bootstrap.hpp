#pragma once

// Application-side GPU preview bootstrap helpers for the resident display route.
//
// This header is deliberately small and typed: it carries NO native/Vulkan/Qt handle, starts no
// thread, creates no device, and never duplicates the viewer's own CPU fallback. It only:
//
//   * derives the bounded resident lease/scene-cache budgets from the artist's UI frame-cache
//     budget so the resident registry cannot be permanently smaller than the cache that references
//     its leases (the 512 MiB-registry vs 2 GiB-cache acceleration cliff);
//   * decides whether the Wayland-only presentation capability may be requested at all;
//   * rebuilds the evaluator-owned GpuSceneMediaContext per GPU-scene request so a relative media
//     path follows the live session base directory (Open/SaveAs) instead of a stale startup copy,
//     while sharing the coverage cache across requests;
//   * caches the service's presentation client/availability and hands every current/future
//     ViewerEditor one typed ViewerGpuDependencies context.
//
// The existing ViewerGpuResidentController already owns the CPU fallback for a resident frame that
// cannot be presented; nothing here re-implements it.

#include <bloom/render/gpu_presentation_types.hpp>
#include <bloom/runtime/cpu_composition_evaluator.hpp>
#include <bloom/runtime/gpu_ocio_display_arm.hpp>
#include <bloom/runtime/gpu_preview_display_service.hpp>
#include <bloom/runtime/gpu_preview_resident_capacity.hpp>
#include <bloom/runtime/gpu_scene_coverage_cache.hpp>
#include <bloom/runtime/prepared_gpu_scene.hpp>
#include <bloom/runtime/preview_gpu_scene_stage.hpp>
#include <bloom/runtime/qualified_display_processor_provider.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>
#include <bloom/ui/composition_plan_cache.hpp>
#include <bloom/ui/editor_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace bloom::runtime {
class GpuPresentationClient;
} // namespace bloom::runtime

namespace bloom::ui {

class PreviewFrameCache;

// Configured resident-route ceilings, set before any device exists. Every ledger takes the one
// configured ceiling (#425): there is no lease/scene/request/cache split. The application passes
// no host-derived ceiling, since a RAM budget is not evidence of usable VRAM; tests and explicit
// callers may pass a smaller one, and a zero ceiling stays zero (no floor forcing a resident route).
// The GPU owner clamps each ledger to the device and publishes `runtime::GpuResidentCapacityPlan`,
// which the bootstrap installs through the existing status poll. Usable VRAM bounds what the
// ledgers hold together, and the caches are evicted when work waits for device memory.
struct GpuResidentBudgetPlan final {
    // The configured ceiling.
    std::uint64_t residentPoolBytes = 0;
    // GPU-resident limits inside the overall (unchanged) UI cache budget.
    std::uint64_t cacheBytes = 0;
    std::size_t cacheEntries = 0;
    // Service lease-registry caps; the entry cap keeps bounded headroom for leases that are in
    // flight or currently presented but not yet cached.
    std::uint64_t leaseBytes = 0;
    std::size_t leaseEntries = 0;
    std::uint64_t sceneCacheBytes = 0;
    // Per-request LIVE pinned admission ceiling.
    std::uint64_t requestBytes = 0;

    friend bool operator==(const GpuResidentBudgetPlan&, const GpuResidentBudgetPlan&) = default;
};

// Every ledger of the plan is `ceilingBytes`; pure and cannot overflow. Capacity clamping happens
// on the owner thread and is exposed through the status poll.
[[nodiscard]] GpuResidentBudgetPlan gpuResidentBudgetPlanFor(std::size_t ceilingBytes) noexcept;

// Applies the configured plan to the existing service options. It never enables the service and
// never changes native budgets or presentation mode. The per-request allowance is only ever
// lowered to the plan's request ceiling.
void applyGpuResidentBudgetPlan(runtime::GpuPreviewDisplayServiceOptions& options,
                                const GpuResidentBudgetPlan& plan) noexcept;

// Installs the configured plan's GPU-resident limits on the shared frame cache. The
// overall CPU cache budget is untouched; only the bounded resident subset is capped, and it evicts
// LRU resident entries (never invalidating a live lease) when it is exceeded.
void applyGpuResidentCacheLimits(PreviewFrameCache& cache,
                                 const GpuResidentBudgetPlan& plan) noexcept;

// Installs the owner-resolved, capacity-clamped plan from the service status. The bootstrap calls
// this through the existing poll exactly when the resolved capacity changes, so a device that can
// host a larger resident subset raises the cache limit and a smaller one lowers it without ever
// touching a device from the UI thread.
void applyGpuResidentCacheLimits(PreviewFrameCache& cache,
                                 const runtime::GpuResidentCapacityPlan& plan) noexcept;

// The Wayland presentation bootstrap is requested only when the native loader was actually packaged
// AND the live platform is Wayland. Every other platform/session keeps the compute-only or
// CPU-only path with no blank activation claim.
[[nodiscard]] bool shouldRequestWaylandPresentation(bool bundledNativeLoader,
                                                    bool waylandSession) noexcept;

// A viewer may be handed a presentation client only when the owner published a genuinely Ready
// generation AND the client exists. Otherwise the viewer keeps its unchanged CPU paint path.
[[nodiscard]] bool
gpuPresentationAvailable(render::GpuPresentationAvailability availability) noexcept;
[[nodiscard]] bool
gpuViewerClientUsable(const std::shared_ptr<runtime::GpuPresentationClient>& client,
                      render::GpuPresentationAvailability availability) noexcept;

// A fresh, thread-safe copy of the evaluator's media context. Re-call it per GPU-scene request:
// fromEvaluator() copies assetBaseDirectory, so a cached startup copy would keep resolving relative
// media against the pre-Open/SaveAs directory.
[[nodiscard]] runtime::GpuSceneMediaContext
gpuSceneMediaContextFor(const runtime::CpuCompositionEvaluator& evaluator);

// The GPU-scene stage factory. Unlike makeCompositionPreviewGpuSceneStage, it does NOT capture one
// builder for the process lifetime: each invocation constructs a local CpuGpuSceneBuilder over a
// freshly copied media context (sharing the coverage cache) and immediately
// invokes the existing stage function while that builder is alive. This is the
// media-follows-session seam; the evaluator's canonical accessors are internally locked.
//
// `ocioContextResolver` is the ONE shared, inert runtime::GpuOcioContextResolver. It is resolved on
// this CPU worker per request (idempotent after the first success) and supplies BOTH the builder's
// GpuSceneOcioContext (its single shared GpuOcioProgramPreparer and the validated executable-
// relative tool paths, enabling effects/media/arbitrary ACES working space) and the SAME preparer
// to the general display program service. A null resolver or a failed resolve keeps the builder's
// default fail-closed context and every affected request takes the CPU fallback with a reason.
// Nothing here resolves tools, hashes, or compiles on the UI thread.
[[nodiscard]] runtime::PreviewGpuSceneStageFunction makeSessionRefreshingGpuSceneStage(
    const runtime::SnapshotCompiler& compiler, const runtime::CpuCompositionEvaluator& evaluator,
    const runtime::QualifiedDisplayProcessorProvider& qualifiedDisplayProcessorProvider,
    std::shared_ptr<runtime::GpuSceneCoverageCache> coverageCache,
    CompiledPlanCacheHandle planCache = nullptr,
    std::shared_ptr<runtime::GpuOcioContextResolver> ocioContextResolver = nullptr,
    runtime::GpuScenePreparationPhase processingPhase = {});

// Owns the cached presentation capability for the whole application. It is not a QObject, holds no
// native handle, and never blocks. The application refreshes it from the EXISTING TaskUiBridge poll
// (which runs until shutdown), so a capability that is later lost is observed and published as a
// null client to future viewers (through the getter) and to live viewers (through the sink). A
// cached client is INERT until the viewer's own presenter genuinely presents; this class makes no
// presentation claim.
class GpuViewerBootstrap final {
  public:
    GpuViewerBootstrap(runtime::TaskScheduler& scheduler, std::string vulkanLoaderPath,
                       double devicePixelRatio);

    // Inert, UI-thread-safe: binds the shared frame cache whose GPU-resident sublimit follows the
    // owner-resolved capacity. It queries no device and starts no work; call it once during startup
    // before the poll begins.
    void bindResidentCache(PreviewFrameCache& cache) noexcept;

    // Cheap, UI-thread-safe: reads only the already-published status. Updates the cached client and
    // invokes the publication sink exactly when the usable client changed -- including a change to
    // null when the capability is lost. It also installs the owner-resolved resident cache limit
    // when that immutable plan changes, and applies each new finished-frame eviction the owner
    // asks for when work waits for device memory (#425).
    void refreshFromStatus(const runtime::GpuPreviewDisplayServiceStatus& status);

    [[nodiscard]] ViewerGpuDependencies dependencies() const;
    // True once a genuinely Ready capability has been cached. This is a dependency-availability
    // fact only; it is not a presentation/activation claim.
    [[nodiscard]] bool clientAvailable() const noexcept;
    [[nodiscard]] std::uint64_t publicationCount() const noexcept;
    void setPublicationSink(std::function<void(const ViewerGpuDependencies&)> sink);

  private:
    runtime::TaskScheduler* scheduler_ = nullptr;
    std::string loaderPath_;
    double devicePixelRatio_ = 1.0;
    mutable std::mutex mutex_;
    std::shared_ptr<runtime::GpuPresentationClient> client_;
    bool active_ = false;
    std::uint64_t publicationCount_ = 0;
    std::function<void(const ViewerGpuDependencies&)> sink_;

    // Borrowed; the application owns the frame cache and outlives this bootstrap.
    PreviewFrameCache* residentCache_ = nullptr;
    // Last capacity plan installed on `residentCache_`. The default (all-zero) never matches a
    // positive resolved plan, so the first publication always installs.
    runtime::GpuResidentCapacityPlan appliedResidentPlan_{};
    // Last owner finished-frame eviction request applied to `residentCache_`.
    std::uint64_t appliedResidentFrameEvictionSerial_ = 0;
};

} // namespace bloom::ui
