#pragma once

// The one bounded runtime service that owns the GPU half of composition preview display.
//
// It owns a dedicated service thread, a native device/pipeline created on that thread, and one
// outstanding native display job at a time. It never exposes a native/Vulkan/Qt handle and never
// performs media I/O, decoding, evaluation, or rendering on the UI thread. Callers submit an
// ordinary preview request; the service either composes the accepted CPU stage with the display
// fallback on a CPU worker (the reference/unavailable path) or, when qualified, runs the evaluated
// stage through the fixed GPU display operation on the service thread and returns the single
// final submitGpu handle.
//
// Frames are immutable, display-only, and carry GPU provenance; no Float32 process image is
// retained by the GPU product. Completed bytes are CPU packed RGBA8 and make no
// GPU-resident/presentation claim.
//
// Scope tap (#724): the service itself, not a frame, keeps an alias of the scene-linear picture
// the latest Viewer-class frame job computed (and of at most one flagged RAM-fill frame), so the
// Scopes panel can count that picture on this device (measureScopeTap) without a second scene run
// and without reading a picture back. See GpuScopeTapKey and the methods below.
//
// Threading and lifetime: the constructor attaches the caller's TaskScheduler GPU executor and
// starts the service thread. The captured PreviewCpuStageFunction, PreviewCpuDisplayFallback, and
// the scheduler MUST outlive the service. beginShutdown() is non-blocking; the destructor joins
// the service thread and drains child/native ownership before releasing the lease.

#include <bloom/render/gpu_allocator_memory.hpp>
#include <bloom/render/gpu_neutral_display.hpp>
#include <bloom/render/gpu_queue_timing.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/runtime/gpu_memory_budget.hpp>
#include <bloom/runtime/gpu_presentation_coordinator.hpp>
#include <bloom/runtime/gpu_preview_resident_capacity.hpp>
#include <bloom/runtime/gpu_resident_frame_lease.hpp>
#include <bloom/runtime/gpu_scope_measure.hpp>
#include <bloom/runtime/gpu_resident_preview_qualification.hpp>
#include <bloom/runtime/gpu_scene_cache.hpp>
#include <bloom/runtime/gpu_scene_executor.hpp>
#include <bloom/runtime/prepared_preview_frame.hpp>
#include <bloom/runtime/preview_cpu_stage.hpp>
#include <bloom/runtime/preview_gpu_scene_stage.hpp>
#include <bloom/runtime/scope_measure.hpp>
#include <bloom/runtime/snapshot_compiler.hpp>
#include <bloom/runtime/task_scheduler.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bloom::runtime {

class GpuNeutralDisplayQualificationReport;

namespace detail {
struct GpuPreviewDisplayServiceTestAccess;
}

enum class GpuPreviewDisplayServiceState : std::uint8_t {
    // The scheduler GPU startup task (device bootstrap + exact embedded neutral CPU processor
    // build + qualification) has not reached a terminal outcome yet.
    Initializing,
    // Qualified: the pinned Bloom Neutral v1 display operation passed the frozen fixture gate and
    // the eligible pixel interval is populated.
    Ready,
    // Disabled, no loader/device, processor build failed, or qualification/parity failed. Requests
    // take the CPU stage + display-fallback path.
    Unavailable,
    // beginShutdown() has been requested; the service thread is draining.
    Stopping,
    // The service thread has drained and released its assignment.
    Stopped,
};

enum class GpuPreviewDisplayServiceDiagnosticCode : std::uint8_t {
    None,
    Disabled,
    LoaderUnavailable,
    DeviceUnavailable,
    ProcessorUnavailable,
    QualificationUnavailable,
    NativeFailure,
    NativeTimeout,
    AdmissionUnavailable,
    ShuttingDown,
    // The resident preview route was requested but its genuine qualification report is not eligible
    // (processor/device/parity/timing). Requests without a usable resident generation take the full
    // original CPU path.
    ResidentQualificationUnavailable,
    // The resident route qualified, but this service generation has no usable presentation
    // generation (mode Disabled or the Wayland capability is not Ready). Resident frames cannot be
    // presented, so requests take the full CPU path and no blank activation is claimed.
    ResidentPresentationUnavailable,
    // A resident registry publication was refused under byte/count budget pressure. The released
    // tokens are collected on the next owner pump and the same request takes the CPU fallback; the
    // live leases/pins are never invalidated and no permanent GPU poison is latched.
    ResidentLeasePressure,
};

struct GpuPreviewDisplayServiceDiagnostic final {
    GpuPreviewDisplayServiceDiagnosticCode code = GpuPreviewDisplayServiceDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuPreviewDisplayServiceDiagnostic&,
                           const GpuPreviewDisplayServiceDiagnostic&) = default;
};

// Optional owner-thread presentation ownership. The default (Disabled) reproduces the existing
// compute-only service: the device is created without a presentation bootstrap. GPU-prepared packed
// products remain eligible offscreen; NativeResident products require presentation. Wayland asks
// the same service device for the Wayland
// presentation bootstrap; the app selects it explicitly at construction (no hidden activation).
enum class GpuPreviewDisplayServicePresentationMode : std::uint8_t {
    Disabled,
    Wayland,
};

// Why a preview request took the CPU path (#452). Every `cpuFallbacks` increment names exactly one
// reason, so a CPU frame on a GPU route is never silent. A request the scheduler's bounded GPU
// admission refused is counted by `gpuAdmissionRefusals` instead.
enum class GpuPreviewCpuFallbackReason : std::uint8_t {
    // No usable GPU route: the service is not Ready (initializing, disabled, no device, failed
    // qualification or a retired pipeline), presentation is unavailable, or the request has no
    // device allowance.
    GpuUnavailable,
    // Packed readback route: the request is not the Bloom Neutral display the native op runs.
    NonNeutralRequest,
    // Packed readback route: the startup cost model measured the CPU display as faster for this
    // pixel count (outside the qualified interval, or not worth the service handoffs).
    NotWorthwhile,
    // The evaluated stage does not fit the native display's eligibility, geometry or budget.
    Ineligible,
    // Resident route: the composition is outside the prepared GPU subset.
    UnsupportedGpuSubset,
    // A native submission, readback or publication failed, or the device was lost.
    NativeFailure,
    // A native dispatch exceeded its bounded deadline.
    NativeTimeout,
};
inline constexpr std::size_t kGpuPreviewCpuFallbackReasonCount = 7;

// Cached, end-to-end counters for the resident display route. This is a bounded counter snapshot,
// not a profiling framework: it exists so a concrete integration test can assert the resident arm
// actually dispatched native work, hit the content cache, read nothing back, and fell back to the
// CPU for the unsupported subset. NativeResident performs no full-frame readback; GPU-prepared
// packed products add exactly one final display transfer, counted separately too.
struct GpuPreviewDisplayServiceCounters final {
    // Resident scene graphs admitted to the GpuSceneExecutor. One per prepared GPU scene that
    // entered the resident native route, INCLUDING a warm-cache graph that performs zero native
    // operations. This is deliberately distinct from `nativeDispatches` so "graph jobs" are never
    // confused with "actual native operations".
    std::uint64_t residentGraphJobs = 0;
    // Actual native work operations (dispatches and uploads) executed by GpuSceneExecutor,
    // including every family. Several operations may share one work submission. A warm cache hit
    // that cuts the whole subtree leaves this unchanged (zero for an identical warm request).
    std::uint64_t nativeDispatches = 0;
    // Uploads the executor began ahead of their own step, beside another native job (#405).
    std::uint64_t uploadsAhead = 0;
    // Resident scenes whose executor began while the previous frame was still displaying or
    // transferring (#477).
    std::uint64_t scenesAhead = 0;
    // Native uploads, and the work submissions that carried the native operations (a batch is one).
    std::uint64_t nativeUploads = 0;
    std::uint64_t nativeSubmissions = 0;
    // CPU wall stages, including host submission and fence observation/polling delay.
    std::uint64_t uploadSubmitNanoseconds = 0;
    std::uint64_t uploadFenceObservationNanoseconds = 0;
    std::uint64_t processingSubmitNanoseconds = 0;
    std::uint64_t processingFenceObservationNanoseconds = 0;
    std::uint64_t completedStepContinuations = 0;
    std::uint64_t packedTransferNanoseconds = 0;
    // GPU-owner thread wall time by service-loop phase (#477): scheduler GPU tasks run on the
    // owner; preview stage results and the general native display; the resident scene executor
    // (upload, record, submit, fence polls, packed transfer); cache purge, queue timings,
    // presentation and capacity refresh; and idle waits for new work or a pending fence. Busy is
    // the first four.
    std::uint64_t ownerDispatchNanoseconds = 0;
    std::uint64_t ownerStageNanoseconds = 0;
    std::uint64_t ownerResidentNanoseconds = 0;
    std::uint64_t ownerServiceNanoseconds = 0;
    std::uint64_t ownerWaitNanoseconds = 0;
    // Resident graph jobs that produced a published native or GPU-prepared packed frame.
    std::uint64_t residentCompletions = 0;
    // Resident graph jobs that failed, timed out, or were cancelled after admission.
    std::uint64_t residentFailures = 0;
    // GpuSceneExecutor content-cache observations for the resident route.
    std::uint64_t gpuCacheHits = 0;
    std::uint64_t gpuCacheMisses = 0;
    // Requests that took the full original CPU path (unsupported subset, non-neutral, over budget,
    // unavailable presentation, lease pressure, or a resident-native failure), on either route.
    std::uint64_t cpuFallbacks = 0;
    // The same requests by reason, indexed by GpuPreviewCpuFallbackReason; sums to cpuFallbacks.
    std::array<std::uint64_t, kGpuPreviewCpuFallbackReasonCount> cpuFallbacksByReason{};
    // Resident frames the executor refused for device capacity that waited for room (and asked
    // the device caches to give way) instead of taking the CPU path (#539).
    std::uint64_t capacityWaits = 0;
    // Idle trims (#539) and the device bytes they gave back (parked storage and scene cache).
    std::uint64_t idleTrims = 0;
    std::uint64_t idleTrimmedBytes = 0;
    // #544: native allocations the root refused for device room, after which parked storage and
    // the scene cache gave way inside the allocation, and the bytes they released.
    std::uint64_t deviceRoomReclaims = 0;
    std::uint64_t deviceRoomReclaimedBytes = 0;
    // GPU parent submissions refused by the scheduler's bounded per-request admission (the derived
    // host default or an explicit configured capacity) BEFORE any stage work. Non-zero means a
    // request the display stage could have handled was forced onto the CPU by ADMISSION, not by
    // device capability; this makes an admission mismatch observable instead of a silent
    // cpuFallbacks increase.
    std::uint64_t gpuAdmissionRefusals = 0;
    // Scene-executor readbacks plus final packed transfers. NativeResident keeps this zero; its
    // 4-byte status reads are separate. Packed mode transfers only the final display data window.
    std::uint64_t fullFrameReadbacks = 0;
    // 4-byte resident-display status-word invalidations. One per completed resident display job;
    // this is NOT a full-frame readback.
    std::uint64_t displayStatusReads = 0;
    // GPU-prepared packed mode only: one final RGBA8 copy, separate from native route assertions.
    std::uint64_t packedTransfers = 0;
    std::uint64_t packedTransferredBytes = 0;
    std::uint64_t packedPeakTransferBytes = 0;
    // Conservative admitted transfer ceiling, retained through scheduler detach/quarantine.
    // Actual allocations are reported by the three retained byte fields below.
    std::uint64_t packedReservedBytes = 0;
    std::uint64_t packedRetainedSourceBytes = 0;
    std::uint64_t packedRetainedStagingBytes = 0;
    std::uint64_t packedRetainedHostBytes = 0;
    // Resident registry publications refused under byte/count budget pressure. Live leases and pins
    // are never invalidated by this; the released tokens are collected on the next owner pump.
    std::uint64_t residentLeaseRefusals = 0;
    // Bounded owner-thread teardowns that had to destroy the resident pipelines because a native
    // fence could not be proven retired within the retirement budget (the pipeline destructor then
    // performs its own bounded drain/quarantine). Non-zero means the resident route is disabled for
    // this service generation; it is never a silent permanent poison for healthy budget pressure.
    std::uint64_t retirementUnprovenTeardowns = 0;
    // Wall-clock spent in the genuine startup resident qualification, in microseconds. This is a
    // display-route microbenchmark only; it is never a whole-application FPS claim.
    std::uint64_t residentQualificationMicros = 0;

    // Scope tap and device scope measure (#724). Viewer-class frames (Interactive or Visible
    // priority) are tapped always; a RAM-fill frame only when its request asks for it.
    // The scene-linear picture a tapped frame computed is kept by alias for the latest Viewer
    // frame (`scopeTapsKept`) and for at most one flagged fill frame (`scopeFillTapsKept`). A fill
    // frame with the flag off keeps nothing and is never held back: both fill counters stay zero.
    std::uint64_t scopeTapsKept = 0;
    std::uint64_t scopeFillTapsKept = 0;
    // Flagged fill stages held back (once each) because the one fill tap slot was occupied.
    std::uint64_t scopeFillStageWaits = 0;
    // Measure requests, by how they ended. A request that was Busy never queued.
    std::uint64_t scopeMeasuresRequested = 0;
    std::uint64_t scopeMeasuresStarted = 0;
    std::uint64_t scopeMeasuresCompleted = 0;
    std::uint64_t scopeMeasuresSkipped = 0;
    std::uint64_t scopeMeasuresCancelled = 0;
    std::uint64_t scopeMeasuresFailed = 0;
    std::uint64_t scopeMeasuresBusy = 0;
    // Measures that had to wait (once each) for a Viewer-class stage, or for the device to be
    // free of every frame's phase, before a step could begin.
    std::uint64_t scopeMeasureYields = 0;
    // The device work of measures: display and XYZ color program dispatches, count kernel
    // dispatches, table readbacks and the bytes they moved (last measure and total). A measure
    // never reads a picture back and never runs a scene: `residentGraphJobs`, `nativeDispatches`
    // and `fullFrameReadbacks` do not move for it.
    std::uint64_t scopeColorDispatches = 0;
    std::uint64_t scopeTableDispatches = 0;
    std::uint64_t scopeTableReadbacks = 0;
    std::uint64_t scopeTableReadbackBytes = 0;
    std::uint64_t scopeTableReadbackBytesTotal = 0;
    // Memory admission and yield of the measure (#724). The measure's native memory is admitted
    // only as a Background-class processing phase (`scopeMeasureAdmissions`); a request that
    // cannot be admitted yet waits once (`scopeMeasureAdmissionWaits`) holding nothing, and one
    // the card cannot fit beside what the Viewer needs ends Skipped(NoRoom). The need of the last
    // admission decision, in device bytes, is `scopeMeasureNeedBytes`.
    std::uint64_t scopeMeasureAdmissions = 0;
    std::uint64_t scopeMeasureAdmissionWaits = 0;
    std::uint64_t scopeMeasureNeedBytes = 0;
    // Measures that ended Skipped(NoRoom), for any reason (no room beside the Viewer, a tap or
    // the measure's memory given back for a frame that needed the room, the allowance unknown).
    std::uint64_t scopeMeasuresNoRoom = 0;
    // Measures ended or told to stop because a frame needed the device's memory or its one
    // processing-phase slot (a measure with nothing in flight ends at once; one with a
    // submission in flight is polled to its end and ends Skipped(NoRoom)).
    std::uint64_t scopeMeasuresGivenBack = 0;
    // Times the measure's phase was parked so a stage could retain its own (the measure kept
    // being polled; the stage did not wait).
    std::uint64_t scopeMeasurePhaseParks = 0;
    // Taps given back first when a frame was short of device memory (or the reusable device
    // memory reserve was), per slot, before parked storage, the scene cache, the OCIO programs or
    // a request to the UI to evict finished frames.
    std::uint64_t scopeViewerTapYields = 0;
    std::uint64_t scopeFillTapYields = 0;
    // Taps released because the service went idle (a tap matters only while frames are shown).
    std::uint64_t scopeTapIdleReleases = 0;
    // The device-room reclaims plus the finished-frame eviction requests that had already been
    // made when the first tap or measure was given back for memory. Zero shows the scope memory
    // went first, before parked storage, the scene cache, the OCIO programs or the UI's frames.
    std::uint64_t scopeReclaimsSeenAtFirstGiveBack = 0;
    // Flagged fill frames whose tap was not kept because it did not fit as Background. The frame
    // is delivered and retained as an unflagged one; the slot stays free.
    std::uint64_t scopeFillTapsRefused = 0;

    friend bool operator==(const GpuPreviewDisplayServiceCounters&,
                           const GpuPreviewDisplayServiceCounters&) = default;
};

// One consistent locked read of the service state: status, eligibility, the immutable
// qualification report, and the structured diagnostic that explains a non-Ready state. It also
// carries the presentation generation: the immutable UI-side port (null when no live presentation
// generation is owned) and the actual owner-published presentation shutdown snapshot. When
// `presentationClient` is null, presentation is not available and the host must take the
// CPU/packed path; it must never claim a blank presentation was activated.
struct GpuPreviewDisplayServiceStatus final {
    GpuPreviewDisplayServiceState state = GpuPreviewDisplayServiceState::Initializing;
    bool gpuAvailable = false;
    std::shared_ptr<const GpuNeutralDisplayQualificationReport> qualification;
    GpuPreviewDisplayServiceDiagnostic diagnostic;
    // The genuine resident-route qualification report, produced independently of the older packed
    // readback qualification. Non-null once the owner startup resident qualification has run; the
    // report's own eligible()/eligibleFor() — not the packed report — decides resident selection.
    std::shared_ptr<const GpuResidentPreviewQualificationReport> residentQualification;
    std::string residentDetail;
    // Immutable, owner-resolved resident-route capacity and its shared partition. Default
    // constructed (Unknown/all-zero) until the owner resolves it after device creation; the UI
    // installs the resolved cache sublimit through the EXISTING status poll and never queries a
    // device itself.
    GpuResidentCapacityPlan residentCapacityPlan;
    // Owner-sampled rooted foreground ceiling. This is free/plannable device space after actual
    // ownership, not another additive part of residentCapacityPlan.poolBytes. Omitted rooted
    // requests still intersect it with the live delivered host allowance and their phase grant.
    std::uint64_t foregroundRequestBytes = 0;
    // Evict-on-demand of finished resident frames (#425): when work waits for device memory and
    // the scene cache could not give enough back, the owner asks for the rest here. Each new
    // serial asks the frame cache to drop that many bytes of its least recently used resident
    // frames (the bootstrap applies it through the status poll); finished frames go last.
    std::uint64_t residentFrameEvictionSerial = 0;
    std::uint64_t residentFrameEvictionBytes = 0;
    GpuPreviewDisplayServiceCounters counters;
    // Latest owner-observed diagnostic intervals; these never establish painted-frame identity.
    render::GpuAllocatorMemorySnapshot allocatorMemory;
    std::chrono::steady_clock::time_point allocatorMemorySampledAt{};
    render::GpuQueueTimingSample graphQueueTiming;
    render::GpuQueueTimingSample packedTransferQueueTiming;

    // Presentation capability of this service generation. NotRequested for the default Disabled
    // mode; Unavailable when Wayland was requested but a required capability is missing.
    render::GpuPresentationAvailability presentationAvailability =
        render::GpuPresentationAvailability::NotRequested;
    std::string presentationDetail;
    // Immutable UI-side port owned by the service's presentation coordinator. Holds only a shared
    // mailbox and no native object; safe to call from the UI thread. Null until the owner thread
    // has created the registry + coordinator.
    std::shared_ptr<GpuPresentationClient> presentationClient;
    // Owner-published, thread-safe snapshot of the presentation generation's retirement state. A
    // host reads this after beginShutdown() and must refuse Qt teardown while it is not drained, or
    // while it reports any unproven/quarantined target.
    GpuPresentationShutdownStatus presentationShutdown;
};

// `enabled` is the explicit activation flag; when false the service never touches a device or the
// scheduler GPU executor and every request takes the ordinary CPU path. `loaderPath` is the
// explicit native loader override (empty means the platform loader); the service never hardcodes a
// workspace or build path. `previewByteAllowance` is the full per-request byte allowance reserved
// as GPU request-owned admission (capacity-aware by default via gpuPreviewRequestByteAllowance();
// a 1 GiB scheduler request-owned capacity admits two). `nativeBudgets` bounds the native
// pipeline's persistent buffers separately (160 MiB default). Prepared stages waiting for the
// native job are bounded by the scheduler's GPU admission and their admitted processing phases; a
// long wait is back-pressure and never turns an eligible GPU stage into a CPU render.
enum class GpuPreviewDisplayProductMode : std::uint8_t { NativeResident, GpuPreparedPacked };

struct GpuPreviewDisplayServiceOptions final {
    // Explicit composition-root injection, shared with final rendering; no physical accounting
    // claim.
    std::shared_ptr<GpuProcessingPhasePolicy> processingPhasePolicy = {};
    PreviewCpuStoragePlanner cpuStoragePlanner = {};
    bool enabled = false;
    GpuPreviewDisplayProductMode product = GpuPreviewDisplayProductMode::NativeResident;
    std::filesystem::path loaderPath;
    // Per-user directory for the device's persisted pipeline cache (#457); empty keeps it in
    // memory. Hosts pass platform::gpuPipelineCacheDirectory().
    std::filesystem::path pipelineCacheDirectory{};
    // Tests only: model a card with this much device-local memory
    // (render::GpuDeviceCreationOptions::device_memory_limit_bytes, #682). 0 is the whole device.
    std::uint64_t deviceMemoryLimitBytes = 0;
    // Presentation ownership. Default Disabled keeps the compute-only device exactly as before.
    GpuPreviewDisplayServicePresentationMode presentation =
        GpuPreviewDisplayServicePresentationMode::Disabled;
    // Resident-frame lease registry budget (charged in actual native allocation bytes). No byte
    // ceiling of its own by default: the owner clamps it to the device pool (#425).
    GpuResidentFrameLeaseBudgets residentLeaseBudgets{
        .maxBytes = std::numeric_limits<std::uint64_t>::max()};
    // Resident content-cache budget (retained intermediate GpuImages, actual VMA bytes). No
    // ceiling of its own by default: the owner clamps it to the device pool, never to a fraction
    // of it or of host memory (ADR 0037, #425), and evicts it when work waits for device memory.
    GpuSceneCacheBudgets residentSceneCacheBudgets{};
    // Explicit producer/structural ceilings are honored verbatim, including combined image and
    // staging peaks. A rooted omitted producer limit follows the explicit preview ceiling, or
    // the finite owner-resolved physical request ceiling when a live default view is injected.
    // Each request still applies its delivered/default or explicit allowance, admitted phase,
    // live pins and native resource limits. Unrooted omission retains legacy producer budgets.
    // Diagnostics preserve omitted producer defaults; enabling timing is not a budget override.
    bool collectQueueTimestamps = false;
    std::optional<GpuSceneExecutorBudgets> residentExecutorBudgets = {};
    // Bounded resident display pipeline owned-bytes budget per job.
    render::GpuResidentDisplayBudgets residentDisplayBudgets{};
    // Concurrent packed transfer staging/host capacity, separate from the display's native owner.
    // Composition roots reserve this ceiling in the conservative phase working envelope.
    std::uint64_t packedTransferByteAllowance = 320ULL * 1024 * 1024;
    // Bounded resident qualification native budgets (the qualification reads back only internally).
    GpuResidentPreviewBudgets residentQualificationBudgets{};
    // Bounded presentation coordinator admission/overlay/drain options.
    GpuPresentationCoordinatorOptions presentationCoordinator{};
    // Omitted limits use the explicit root numeric view when injected; no-view owners retain
    // their legacy construction-time default. Explicit finite ceilings are never enlarged.
    std::optional<std::size_t> previewByteAllowance = {};
    render::GpuNeutralDisplayBudgets nativeBudgets{};
    // Bounded per-native-dispatch deadline. Expiry drains/quarantines the native pipeline on
    // the owner thread, disables GPU, and takes the same-frame CPU fallback (or cancels).
    std::chrono::milliseconds nativeDispatchDeadline{10000};
    // #539: how long the owner stays idle (no stage, no native work) before it gives retained
    // device memory back: parked native storage and the reusable scene cache. Finished frames and
    // what is on screen stay; the next frame re-creates what it needs.
    std::chrono::milliseconds idleTrimDelay{1000};
};

// What names a tapped frame: its request identity with the Viewer adjustment and the request
// generation neutralized. The scene picture does not depend on either, so the base frame and its
// adjusted frame share one key, and the key of a shown frame is
// `GpuScopeTapKey::of(frame.desiredIdentity())`.
struct GpuScopeTapKey final {
    PreviewRequestIdentity identity;

    [[nodiscard]] static GpuScopeTapKey of(const PreviewRequestIdentity& frameIdentity);

    friend bool operator==(const GpuScopeTapKey&, const GpuScopeTapKey&) = default;
};

// The geometry of a tapped picture, for the region math of the measure (scopeMeasuredArea).
struct GpuScopeTapGeometry final {
    render::ImageWindow dataWindow;
    render::ImageWindow displayWindow;
    // The request's region of interest, when it had one.
    std::optional<render::ImageWindow> roi;
    // The composition's size in pixels: a region is in composition pixels.
    std::uint32_t compositionWidth = 0;
    std::uint32_t compositionHeight = 0;
    // True for the tap of a RAM-fill frame (its own slot), false for a Viewer frame's.
    bool fill = false;
};

enum class GpuScopeTapStatus : std::uint8_t {
    // `counts` holds the tables of `area`, counted on the Viewer's device.
    Measured,
    // The region lies outside the picture: there is nothing to count.
    EmptyArea,
    // The measure did not run; `skip` says why.
    Skipped,
    // A measure was already queued or in flight in this service.
    Busy,
    Cancelled,
    // The device could not count the picture; `message` says why. The caller falls back to the
    // retained picture or the CPU reference.
    Failed,
};

enum class GpuScopeTapSkip : std::uint8_t {
    None,
    // A newer Viewer frame replaced the tap the request named before its measure started.
    // The key decides: a request measures only the picture its key names.
    Superseded,
    // No tap has that key (never kept, dropped, freed by its measure or by shutdown).
    TapGone,
    // The device has no room for the measure now; it never takes room a frame needs. It is
    // memory pressure, never a caller's drop: the caller falls back to the retained picture's
    // measure for that frame. It also names a key whose tap was given back (under pressure or
    // when the service went idle) or was refused at publish (a fill frame's tap that did not fit
    // as Background); the service remembers the last 64 such keys.
    NoRoom,
};

struct GpuScopeTapMeasureOutcome final {
    GpuScopeTapStatus status = GpuScopeTapStatus::Failed;
    GpuScopeTapSkip skip = GpuScopeTapSkip::None;
    std::string message;
    ScopeCounts counts;
    ScopeMeasuredArea area;
    // Always the device, and the picture the Viewer's own frame job rendered.
    ScopeRoute route = ScopeRoute::Gpu;
    ScopeSource source = ScopeSource::Rendered;
    // The native work of this measure: color program dispatches, count kernel dispatches, table
    // readbacks, the bytes those moved (the layout's bytes at the job's detail) and picture
    // readbacks (always 0).
    std::uint64_t colorDispatches = 0;
    std::uint64_t tableDispatches = 0;
    std::uint64_t tableReadbacks = 0;
    std::uint64_t tableReadbackBytes = 0;
    std::uint64_t pictureReadbacks = 0;
};

class GpuPreviewDisplayService final {
  public:
    GpuPreviewDisplayService(const GpuPreviewDisplayService&) = delete;
    GpuPreviewDisplayService& operator=(const GpuPreviewDisplayService&) = delete;
    GpuPreviewDisplayService(GpuPreviewDisplayService&&) = delete;
    GpuPreviewDisplayService& operator=(GpuPreviewDisplayService&&) = delete;
    ~GpuPreviewDisplayService();

    // Attaches the scheduler GPU executor and starts the service thread/startup task. Never
    // blocks the caller on device or qualification work.
    GpuPreviewDisplayService(TaskScheduler& scheduler, PreviewCpuStageFunction stageFunction,
                             PreviewCpuDisplayFallback displayFallback,
                             GpuPreviewDisplayServiceOptions options = {});

    // Resident-route overload. The service additionally owns a genuine resident-route qualification
    // (run on the service owner thread at startup) and, for an eligible resolved scene, prepares
    // the GPU scene on a CPU child, then runs it through the existing GpuSceneExecutor ->
    // GpuResidentDisplay -> product factory on the same owner thread and publishes an opaque
    // GpuResidentFrameLease (no full-frame readback). A request whose stage is Unsupported,
    // UnsupportedGpuSubset, non-neutral, over budget, whose presentation generation is unavailable,
    // or whose resident dispatch fails takes the FULL original CPU path through `cpuStageFunction`
    // + `displayFallback`. There is no second service, thread, device, or scheduler lease.
    GpuPreviewDisplayService(TaskScheduler& scheduler,
                             PreviewGpuSceneStageFunction gpuStageFunction,
                             PreviewCpuStageFunction cpuStageFunction,
                             PreviewCpuDisplayFallback displayFallback,
                             GpuPreviewDisplayServiceOptions options = {});

    // Returns the stable final handle for the request: a submitGpu handle on the qualified GPU
    // path, or an ordinary CPU task handle (CPU stage + display fallback on the same evaluated
    // frame) on the reference/unavailable/disabled path. `pixelStorageByteLimit` is the request's
    // display byte allowance and is also the GPU request-owned admission reservation.
    //
    // `tapForScopes` (#724) matters only for a fill frame (Foreground or Background priority): the
    // service then keeps the scene picture of the frame for one scope measure, in its own single
    // fill slot, and does not start another flagged fill frame's scene work while that slot is
    // occupied. The caller MUST then free the slot for every flagged frame it received: by
    // measureScopeTap (which frees it when the measure ends) or by dropScopeTap. A frame
    // that failed or was cancelled holds nothing. With the flag off, a fill frame behaves as it
    // always did and costs nothing. A Viewer-class frame is always tapped; the flag changes nothing.
    [[nodiscard]] TaskSubmission<PreviewPreparationResultHandle>
    submit(TaskRequest request, const document::Snapshot& snapshot,
           const PreviewRequestIdentity& identity, std::size_t pixelStorageByteLimit,
           const std::vector<SnapshotParameterOverride>& overrides, bool tapForScopes = false);

    [[nodiscard]] GpuPreviewDisplayServiceStatus status() const;

    // Owner-thread clear of the resident GPU scene cache's retained operation/output images. Must
    // be called from a worker thread, never the UI thread: it posts the clear to the service owner
    // thread and waits there (bounded by `timeout`). Clearing drops only the cache's own
    // references, so a displayed or exported lease that still pins an image stays valid -- no live
    // lease is invalidated. The parked native storage is then freed (#544), so the device memory
    // goes back. Returns the device bytes that went back when the clear ran (zero when this
    // service has no resident route), nothing when the service is shutting down or the wait
    // expired.
    [[nodiscard]] std::optional<std::uint64_t>
    purgeRetainedCaches(std::chrono::milliseconds timeout) noexcept;

    // The geometry of the tap `key` names (the latest Viewer frame's, or the fill slot's). Any
    // thread, non-blocking; nothing when no tap has that key.
    [[nodiscard]] std::optional<GpuScopeTapGeometry>
    scopeTapGeometry(const GpuScopeTapKey& key) const;

    // Measures the tapped picture `key` names on this service's device: the display and XYZ
    // color steps the job asks for, then the count kernel, then one table readback, as a
    // separate low-priority submission that starts only while no Viewer-class stage is ready,
    // active or ahead and never waits on a fence (the owner polls). Prepares the job's color
    // programs on the calling thread, queues ONE request and blocks only the calling thread
    // until its result. Call it from a task worker, never the UI thread or the service thread.
    // A second request while one is queued or in flight is Busy. `cancellation` stops the
    // measure; its resources are freed only after the submission's fence is proven retired, so
    // the result of a cancelled measure arrives after that. Shutting the service down ends a
    // queued or in-flight request (Cancelled) and unblocks the caller. The measure of a fill
    // tap frees the fill slot when it ends, whatever the result; a Viewer tap stays for the
    // next measure.
    [[nodiscard]] GpuScopeTapMeasureOutcome
    measureScopeTap(const GpuScopeTapKey& key, const GpuScopeMeasureJob& job,
                    const CancellationToken& cancellation);

    // Frees the tap `key` names (any thread, non-blocking): the picture is released on the
    // service thread. A pending or in-flight measure of it ends Skipped(TapGone). A fill tap
    // freed this way lets the next flagged fill frame start.
    void dropScopeTap(const GpuScopeTapKey& key) noexcept;

    // Non-blocking. Closes admission for this service's own tasks, requests cancellation on the
    // tasks it submitted (including ordinary CPU fallback roots), and wakes the service thread.
    // It never drains and never touches unrelated scheduler tasks.
    void beginShutdown() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend struct detail::GpuPreviewDisplayServiceTestAccess;
};

} // namespace bloom::runtime
