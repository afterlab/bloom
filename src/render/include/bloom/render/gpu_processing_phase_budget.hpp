#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace bloom::render {
namespace detail {
struct GpuProcessingPhaseBudgetState;
class ProcessingHostBackingToken;
struct PackedHostRetirementState;
void markHostPixelsMaterialized(void*) noexcept;
void retireHostPixelsBeforeFree(void*) noexcept;
} // namespace detail
class PackedHostRetirementOwner;
class NativeImageStorageToken;
class NativeBufferStorageToken;
class ProcessingHostStorageToken;

// What the allocator takes for one resident RGBA32F image of this extent, from the driver's own
// memory requirement (rows and tiles are padded: on one GPU a 1920-wide float image takes the room
// of a 2048-wide one). Callable from any thread, memoised per extent, and falls back to
// width * height * 16 once the device is gone or when the driver cannot say. A planner that claims
// device bytes for images rounds each one with this, so the claim is what the allocator charges.
struct GpuAllocationSizes final {
    // A resident RGBA32F image of this width and height.
    std::function<std::uint64_t(std::uint32_t, std::uint32_t)> image;
    // A metadata or status buffer of this many requested bytes (small storage buffers take at
    // least the allocator's alignment; a staging copy of one takes the same).
    std::function<std::uint64_t(std::uint64_t)> buffer;
};

// Lifecycle identity of the existing neutral budget state, never a pixel/cache content identity.
// A handle retains no phase admission, native owner or device. The incomplete const state cannot
// be mutated through this public surface and cannot be confused after root-address reuse.
using GpuProcessingRootIdentity = std::shared_ptr<const detail::GpuProcessingPhaseBudgetState>;

// Admission of simultaneous processing phases against measured capacity. Each phase claims its
// host need (fixed output/metadata, 3x its source need) at admission. The source need is the
// stated minimum source bytes until a phase with the same stated envelope has finished source
// preparation; then it is the smaller of the stated minimum and the largest source bytes such a
// phase actually retained plus a quarter (#472). Its device
// need (device working bytes plus the minimum executor bytes) must fit the free device capacity
// at admission but is claimed only during the phase's device stage: from its first native
// allocation until it holds no native storage. Phases run their device stages one at a time per
// device, so summing every admitted phase's device need would reserve several times the device
// memory in use (#417). Once a phase with the same stated envelope has been released, the need is
// the smaller of the stated need and its largest measured device peak plus a quarter; only a need
// larger than the whole capacity by the stated values is refused. Requested source/executor
// values are ceilings that are not claimed.
// Runtime owns process/device sampling and injects resolved capacity; this class performs no I/O.
struct GpuProcessingPhaseRequest final {
    std::uint64_t fixedHostBytes = 0;
    // Subset of fixedHostBytes reserved for allocator-backed processing metadata.
    std::uint64_t processingHostScratchBytes = 0;
    std::uint64_t requestedSourceBytes = 0;
    std::uint64_t minimumSourceBytes = 0;
    // #472: the largest single source of minimumSourceBytes as a float frame. Preparation decodes
    // and converts one source at a time, so its transient scratch is bounded by twice this, not by
    // twice the whole source need. Zero when unknown: the scratch is then twice the source need.
    std::uint64_t largestSourceBytes = 0;
    std::uint64_t deviceWorkingBytes = 0;
    std::uint64_t requestedExecutorBytes = 0;
    std::uint64_t minimumExecutorBytes = 0;
    bool sourceFree = false;
    bool native = true;
};

enum class GpuProcessingPhaseAdmission {
    Accepted,
    AwaitingCapacity,
    BudgetRefused,
    InvalidRequest,
    AllocationFailure,
    SerialExhausted,
};
// Viewer: an export or background phase yields to a waiting or active Viewer (ADR 0033 s.9).
enum class GpuProcessingPhaseDomain { None, Host, Device, RequestCount, Viewer };

struct NativePresentationStorageRefusal final {
    GpuProcessingPhaseAdmission status = GpuProcessingPhaseAdmission::InvalidRequest;
    GpuProcessingPhaseDomain domain = GpuProcessingPhaseDomain::None;
    std::uint64_t admissionRevision = 0;
    bool image = false;
};

struct GpuProcessingPhaseSnapshot final {
    std::optional<std::uint64_t> hostCapacityBytes;
    // The most host memory this root could ever be given (the tightest finite memory limit or the
    // physical memory, minus headroom). With it, a host need above the measured free capacity but
    // within the ceiling waits for memory to be released; only a need above it is refused.
    std::optional<std::uint64_t> hostCeilingBytes;
    std::optional<std::uint64_t> deviceCapacityBytes;
    // #539: the most device memory this root could ever be given (the device-local heap minus the
    // VRAM headroom). A device need above the measured free capacity but within it waits for
    // memory another program holds to come back; only a need above it is refused.
    std::optional<std::uint64_t> deviceCeilingBytes;
    bool hostReady = false;
    bool deviceReady = false;
    std::uint64_t hostReservedBytes = 0;
    // Only allocator-requested payload bytes for explicitly rooted packed RGBA8 storage.
    // Legacy vectors, allocator overhead and native allocations are not included.
    std::uint64_t ownedPackedHostBytes = 0;
    // Exact requested allocation header, including its intrusive retirement node. This is not
    // opaque malloc/standard-library/thread overhead; payload and metadata remain distinct.
    std::uint64_t ownedPackedHostMetadataBytes = 0;
    std::uint64_t totalHostClaimBytes = 0;
    std::size_t ownedPackedHostAllocations = 0;
    // Final process vectors/builders and encoded products use distinct allocation domains.
    std::uint64_t ownedFinalProcessHostBytes = 0;
    std::uint64_t ownedFinalEncodedHostBytes = 0;
    std::uint64_t ownedFinalHostMetadataBytes = 0;
    std::size_t ownedFinalHostAllocations = 0;
    // Exact processing scratch and adopted, measured retained host backing. Metadata applies
    // only to allocator-owned scratch; an adopted backing includes its object in payload bytes.
    std::uint64_t ownedProcessingHostBytes = 0;
    std::uint64_t ownedProcessingHostMetadataBytes = 0;
    std::size_t ownedProcessingHostAllocations = 0;
    std::uint64_t deviceReservedBytes = 0;
    // Covered image suballocations, not VMA committed blocks/driver memory. The nonlocal-host
    // subset overlaps this constraint; it is not an additive physical-memory total.
    std::uint64_t ownedNativeImageBytes = 0;
    std::uint64_t ownedNonlocalNativeHostBytes = 0;
    // #544: the device-local subset of the native images that left their phase when they were
    // published (#539): reusable retention, which gives way inside a refused native allocation.
    std::uint64_t retainedNativeImageBytes = 0;
    std::uint64_t totalDeviceClaimBytes = 0;
    std::size_t ownedNativeImages = 0;
    // Covered transient buffer suballocations. Output/upload nonlocal backing replaces planned
    // host debit; processing nonlocal backing adds a fresh headroom claim. This subset overlaps
    // the device constraint and is not additive physical memory.
    std::uint64_t ownedNativeBufferBytes = 0;
    std::uint64_t ownedNonlocalNativeBufferHostBytes = 0;
    std::size_t ownedNativeBuffers = 0;
    // Subsets of the native image/buffer counters above. Presentation has explicit independent
    // root admission and never consumes or pins a frame's processing phase. WSI-owned swapchain
    // images have unknown physical allocation size and are excluded from these owned subsets.
    std::uint64_t ownedPresentationImageBytes = 0;
    std::uint64_t ownedPresentationBufferBytes = 0;
    std::size_t ownedPresentationImages = 0;
    std::size_t ownedPresentationBuffers = 0;
    std::uint64_t peakHostReservedBytes = 0;
    std::uint64_t peakDeviceReservedBytes = 0;
    // High-water marks of the device claims (totalDeviceClaimBytes) and of the root's own native
    // allocations (ownedNativeImageBytes + ownedNativeBufferBytes), for comparing what admission
    // reserves with what Bloom actually allocated.
    std::uint64_t peakTotalDeviceClaimBytes = 0;
    std::uint64_t peakOwnedNativeBytes = 0;
    // Released native phases: how many, the sum of their stated device needs and of the most
    // native storage each held at one time, and the largest need and largest such peak.
    std::uint64_t releasedNativePhases = 0;
    std::uint64_t releasedPhaseDeviceNeedBytes = 0;
    std::uint64_t releasedPhaseDevicePeakBytes = 0;
    std::uint64_t peakPhaseDeviceNeedBytes = 0;
    std::uint64_t peakPhaseDeviceAllocationBytes = 0;
    // #539: the most native storage recently released phases held at one time, for sizing frames
    // in flight by measured room: the larger of the newest phase's peak and the previous value less
    // an eighth, so it follows the current scene within a few frames instead of holding an old
    // scene's peak forever.
    std::uint64_t recentPhaseDevicePeakBytes = 0;
    // Phases that finished source preparation: how many, the sum of the source needs they claimed
    // (3x of which covered decode, conversion and upload overlap) and of the source bytes they
    // actually retained, and the largest need and largest retention (#472).
    std::uint64_t finishedSourcePhases = 0;
    std::uint64_t finishedPhaseSourceNeedBytes = 0;
    std::uint64_t finishedPhaseRetainedSourceBytes = 0;
    std::uint64_t peakPhaseSourceNeedBytes = 0;
    std::uint64_t peakPhaseRetainedSourceBytes = 0;
    std::size_t admittedPhases = 0;
    // #586: phases retired to retention (GpuProcessingPhaseLease::retireToRetention): still alive
    // because a cached product owns a host backing, no longer counted in admittedPhases or against
    // requestCapacity. retainedProcessingHostBytes is the part of ownedProcessingHostBytes they hold.
    std::size_t retainedPhases = 0;
    std::uint64_t retainedProcessingHostBytes = 0;
    std::size_t unprovenPhases = 0;
    std::size_t requestCapacity = 0;
    std::uint64_t budgetRefusals = 0;
    // Admission-only readiness/release revision; never a process/display/cache identity.
    std::uint64_t admissionRevision = 0;
};

// Exact rooted host allocations that completed construction, and cumulative bytes whose actual
// free has begun. Retirement is recorded before deallocation so a sampler never credits bytes
// already in that operation's free gap. Reservations and new allocations stay uncredited.
struct RootedHostBackingSnapshot final {
    std::uint64_t materializedBytes = 0;
    std::uint64_t retiredMaterializedBytes = 0;
    bool retiredBytesSaturated = false;
};

// Shared aliases refer to one admission. Destruction releases its byte/count reservation once;
// the neutral state outlives the composition root if a native quarantine still owns this lease.
class GpuProcessingPhaseLease final {
  public:
    GpuProcessingPhaseLease(const GpuProcessingPhaseLease&) = delete;
    GpuProcessingPhaseLease& operator=(const GpuProcessingPhaseLease&) = delete;
    ~GpuProcessingPhaseLease();

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] GpuProcessingRootIdentity rootIdentity() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t hostBytes() const noexcept { return hostBytes_; }
    [[nodiscard]] std::uint64_t deviceBytes() const noexcept { return deviceBytes_; }
    // Ceiling for retained decoded sources: the room available at admission. Only the source
    // need (3 * sourceClaimBytes) is claimed; growth beyond it is reconciled at the handoff.
    [[nodiscard]] std::uint64_t sourceByteLimit() const noexcept { return sourceCeilingBytes_; }
    [[nodiscard]] std::uint64_t sourceClaimBytes() const noexcept { return sourceBytes_; }
    // Decode/conversion scratch claimed with the sources while they are prepared (#472).
    [[nodiscard]] std::uint64_t sourceScratchBytes() const noexcept { return sourceScratchBytes_; }
    // #539: the source ceiling was cut below what an otherwise empty root would give, because
    // other admitted work held the room. A source refused for that ceiling fits a later admission.
    [[nodiscard]] bool sourceRoomWasShort() const noexcept { return sourceRoomShort_; }
    // Logical upload occupancy ceiling; uploads acquire their actual host backing separately.
    [[nodiscard]] std::uint64_t uploadByteLimit() const noexcept { return sourceCeilingBytes_; }
    // Device ceiling: the live room at admission. The device need (working + minimum executor
    // bytes) is checked at admission and claimed only for the phase's device stage, from its
    // first native allocation until it holds none; allocations beyond the need are admitted
    // against live capacity.
    [[nodiscard]] std::uint64_t executorByteLimit() const noexcept { return executorBytes_; }
    // #539: the executor ceiling the request asked for. A limit below it was cut to the room left
    // at admission, so a frame refused for that limit fits once room returns: it waits, never
    // falls back to the CPU.
    [[nodiscard]] std::uint64_t executorByteCeiling() const noexcept {
        return requestedExecutorBytes_;
    }
    // #539: the root's measured device capacity when this phase was admitted. Capacity below it
    // later means another program took device memory meanwhile, which it can give back.
    [[nodiscard]] std::uint64_t deviceCapacityAtAdmission() const noexcept {
        return deviceCapacityAtAdmission_;
    }
    [[nodiscard]] std::uint64_t deviceClaimBytes() const noexcept { return deviceClaimBytes_; }
    // The device need claimed during the device stage, and the most native storage (images and
    // buffers) this phase has held at one time so far.
    [[nodiscard]] std::uint64_t deviceNeedBytes() const noexcept { return deviceNeedBytes_; }
    [[nodiscard]] std::uint64_t peakDeviceAllocationBytes() const noexcept;
    // After the complete scene has finished CPU preparation, retain its actual decoded backing
    // (recorded as the measured retention of this phase's envelope).
    // Future native upload buffers acquire their actual host bytes at allocation, within the
    // original upload ceiling. Spent decode/conversion scratch must not block native execution.
    // Call once, before publishing the prepared scene to the device thread.
    [[nodiscard]] bool finishSourcePreparation(std::uint64_t retainedSourceBytes) noexcept;
    // #343: claims `bytes` more host memory for what preparation retains beyond the phase's fixed
    // quota, held until the phase is released. Accepted within the room left of the live capacity;
    // AwaitingCapacity when other claims hold the room; BudgetRefused when the phase's own claim
    // could never fit. Nothing changes unless Accepted.
    [[nodiscard]] GpuProcessingPhaseAdmission claimRetainedHostBytes(std::uint64_t bytes) noexcept;
    // #343: the phase's scene is prepared and its images are known at their real sizes, which the
    // admission took as one frame each. Raises the executor bytes the phase needs on the device to
    // `bytes` when that is more than it was admitted with: the device need grows by the excess over
    // the executor bytes it already counts (the minimum, inside the stated or the measured need),
    // never back to the stated working allowance a measured envelope replaced. A stated need
    // (working allowance plus `bytes`) larger than the device is refused as at admission; the
    // larger need is checked against the live room and claimed with the rest of the device stage. Accepted (also when `bytes` is not more); AwaitingCapacity when the live device
    // room cannot hold it now; BudgetRefused when it exceeds the phase's executor ceiling or the
    // device itself. Nothing changes unless Accepted.
    [[nodiscard]] GpuProcessingPhaseAdmission raiseExecutorNeed(std::uint64_t bytes) noexcept;
    // The bytes the device's allocator takes for one resident RGBA32F image of this extent (#343),
    // as the device reported them to the root: padded rows and tiles included. Without a report
    // (no device, or one that cannot say) it is the plain width * height * 16. Any thread.
    [[nodiscard]] std::uint64_t imageAllocationBytes(std::uint32_t width,
                                                     std::uint32_t height) const noexcept;
    // The same for one metadata or status buffer of `bytes`: never less than `bytes`.
    [[nodiscard]] std::uint64_t bufferAllocationBytes(std::uint64_t bytes) const noexcept;
    // Expected watermark after this lease alone releases. Independent changes remain visible.
    [[nodiscard]] std::uint64_t revisionAfterOwnRelease() const noexcept;
    // #539: the phase's scene execution has finished. Its device claim is held only while it
    // executes: what it has not allocated goes back to the root now, and from here on the claim
    // covers at most its stated working allowance (display, transfer, readback). Storage it still
    // holds (the output image) stays allocated; later allocations are admitted against live room.
    void endDeviceExecution() noexcept;
    // #586: the phase's work is finished and only cached products it adopted remain (a decoded
    // audio buffer). It stops counting as an admitted request, and its unspent host reservation
    // goes back to the root now; the adopted backing stays claimed as evictable retention until the
    // last product is freed, which then releases the lease. Call once, after the final adoption.
    // False (nothing changes) for an unproven phase or one holding native device storage.
    bool retireToRetention() noexcept;
    void markUnproven() noexcept;
    [[nodiscard]] bool isUnproven() const noexcept;
    // Only after the owning device has gone idle (every submission that could touch this phase's
    // storage has completed). Lets the phase release its claims normally again.
    void proveRetiredAfterDeviceIdle() noexcept;

  private:
    // Callers hold state_->mutex. The first draw of a device stage claims the phase's device need
    // when the live room holds it. A draw consumes this phase's unallocated device claim; any
    // remainder is a new claim admitted against the live device capacity. A return puts the
    // bytes back into this phase's claim (up to its need) while it still holds native storage;
    // the return that leaves it none ends the stage and releases its whole remaining claim.
    [[nodiscard]] bool drawDeviceLocked(std::uint64_t bytes) noexcept;
    void releaseLocked() noexcept;
    void returnDeviceLocked(std::uint64_t bytes) noexcept;
    // #539: storage this phase allocated is published (a scene cache entry, a presented frame):
    // it stays charged to the root as retention that gives way on demand, and leaves the phase's
    // allocation, so the phase's device stage needs (and records) only its own live set.
    void transferToRetentionLocked(std::uint64_t bytes) noexcept;
    friend class GpuProcessingPhaseBudget;
    friend class PackedHostStorageAuthority;
    friend class NativeImageStorageToken;
    friend class NativeBufferStorageToken;
    friend class ProcessingHostStorageToken;
    friend class detail::ProcessingHostBackingToken;
    explicit GpuProcessingPhaseLease(
        std::shared_ptr<detail::GpuProcessingPhaseBudgetState>) noexcept;
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state_;
    std::uint64_t id_ = 0;
    std::uint64_t admissionRevision_ = 0;
    std::uint64_t hostBytes_ = 0;
    // Separate from immutable granted limits; accesses are guarded by state_->mutex.
    std::uint64_t remainingHostBytes_ = 0;
    // Ordinary owned output uses fixed F only; S/C estimates never become spare output credit.
    std::uint64_t remainingOutputHostBytes_ = 0;
    std::uint64_t remainingProcessingHostScratchBytes_ = 0;
    // Logical occupancy applies even to device-local/BAR upload staging.
    std::uint64_t remainingUploadBytes_ = 0;
    std::uint64_t deviceBytes_ = 0;
    std::uint64_t remainingDeviceBytes_ = 0;
    std::uint64_t sourceBytes_ = 0;
    std::uint64_t sourceScratchBytes_ = 0;
    bool sourceRoomShort_ = false;
    std::uint64_t sourceCeilingBytes_ = 0;
    std::uint64_t executorBytes_ = 0;
    std::uint64_t requestedExecutorBytes_ = 0;
    std::uint64_t deviceCapacityAtAdmission_ = 0;
    // Unallocated device bytes this phase holds in totalDeviceClaimBytes, at most its admitted
    // need, and only during its device stage. Allocations beyond the need are separate live
    // claims, returned to the root on release.
    std::uint64_t deviceClaimBytes_ = 0;
    std::uint64_t deviceNeedBytes_ = 0;
    // The executor image bytes deviceNeedBytes_ counts: the envelope's minimum at admission, then
    // what raiseExecutorNeed raised it to (#343).
    std::uint64_t needExecutorBytes_ = 0;
    // The most unallocated claim this phase may hold: its need while it executes, its working
    // allowance once execution has ended (#539).
    std::uint64_t deviceClaimCapBytes_ = 0;
    // Native storage this phase holds now, and the most it has held at one time.
    std::uint64_t allocatedDeviceBytes_ = 0;
    std::uint64_t peakAllocatedDeviceBytes_ = 0;
    // The stated envelope, under which the release records the measured device peak and the
    // finished preparation the retained source bytes.
    std::uint64_t envelopeWorkingBytes_ = 0;
    std::uint64_t envelopeMinimumExecutorBytes_ = 0;
    std::uint64_t envelopeFixedHostBytes_ = 0;
    std::uint64_t envelopeMinimumSourceBytes_ = 0;
    bool unproven_ = false;
    bool sourcePreparationFinished_ = false;
    // #586: retired to retention; owned host backing bytes adopted by this phase (guarded).
    bool retired_ = false;
    std::uint64_t ownedHostBackingBytes_ = 0;
};

enum class HostPixelStorageDomain { PackedPreview, FinalProcess, FinalEncoded };

class PackedHostStorageAuthority;
// One actual typed host-vector allocation. Its allocator destroys the backing block before this
// token returns provisional bytes to the originating phase or releases published bytes.
class PackedHostStorageToken final {
  public:
    PackedHostStorageToken() = default;
    PackedHostStorageToken(const PackedHostStorageToken&) = delete;
    PackedHostStorageToken& operator=(const PackedHostStorageToken&) = delete;
    PackedHostStorageToken(PackedHostStorageToken&&) noexcept;
    PackedHostStorageToken& operator=(PackedHostStorageToken&&) noexcept;
    ~PackedHostStorageToken();
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool published() const noexcept;

  private:
    friend class PackedHostStorageAuthority;
    friend void detail::markHostPixelsMaterialized(void*) noexcept;
    friend void detail::retireHostPixelsBeforeFree(void*) noexcept;
    // Called only after the typed vector has constructed/touched its complete allocation.
    void markMaterialized() noexcept;
    void retireBeforeFree() noexcept;
    PackedHostStorageToken(std::shared_ptr<PackedHostStorageAuthority>, std::uint64_t,
                           std::uint64_t) noexcept;
    void reset() noexcept;
    std::shared_ptr<PackedHostStorageAuthority> authority_;
    std::uint64_t bytes_ = 0;
    std::uint64_t metadataBytes_ = 0;
    bool materialized_ = false;
};

struct PackedHostStorageSnapshot final {
    std::uint64_t bytes = 0;
    std::uint64_t metadataBytes = 0;
    bool published = false;
};

// A bounded allocator authority under the existing phase root, never a second allowance. Empty
// allocators hold only a weak origin; actual provisional allocations keep it alive until every
// debit is folded back or the complete successful product is promoted once.
class PackedHostStorageAuthority final
    : public std::enable_shared_from_this<PackedHostStorageAuthority> {
  public:
    [[nodiscard]] static std::shared_ptr<PackedHostStorageAuthority>
    create(const std::shared_ptr<GpuProcessingPhaseLease>&, std::uint64_t simultaneousByteLimit,
           HostPixelStorageDomain domain = HostPixelStorageDomain::PackedPreview);
    [[nodiscard]] std::optional<PackedHostStorageToken>
    reserve(std::uint64_t bytes, std::uint64_t metadataBytes = 0) noexcept;
    [[nodiscard]] std::shared_ptr<detail::PackedHostRetirementState>
    retirementState() const noexcept;
    [[nodiscard]] bool promote() noexcept;
    // Complete final product has exactly a process allocation and an optional encoded allocation.
    // Validate both before changing either; detach origins outside the common neutral lock.
    [[nodiscard]] static bool
    promoteFinalProduct(const std::shared_ptr<PackedHostStorageAuthority>& process,
                        const std::shared_ptr<PackedHostStorageAuthority>& encoded = {}) noexcept;
    [[nodiscard]] PackedHostStorageSnapshot snapshot() const noexcept;
    [[nodiscard]] GpuProcessingRootIdentity rootIdentity() const noexcept { return state_; }

  private:
    friend class PackedHostStorageToken;
    PackedHostStorageAuthority(const std::shared_ptr<GpuProcessingPhaseLease>&, std::uint64_t,
                               HostPixelStorageDomain) noexcept;
    void release(std::uint64_t bytes, std::uint64_t metadataBytes, bool materialized) noexcept;
    void materialize(std::uint64_t bytes, std::uint64_t metadataBytes) noexcept;
    void retireMaterialized(std::uint64_t bytes, std::uint64_t metadataBytes) noexcept;
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state_;
    std::weak_ptr<GpuProcessingPhaseLease> origin_;
    std::shared_ptr<GpuProcessingPhaseLease> retainedOrigin_;
    HostPixelStorageDomain domain_ = HostPixelStorageDomain::PackedPreview;
    std::uint64_t originId_ = 0;
    std::uint64_t limit_ = 0;
    std::uint64_t bytes_ = 0;
    std::uint64_t metadataBytes_ = 0;
    bool published_ = false;
};

struct GpuProcessingPhaseGrant final {
    GpuProcessingPhaseAdmission status = GpuProcessingPhaseAdmission::InvalidRequest;
    GpuProcessingPhaseDomain domain = GpuProcessingPhaseDomain::None;
    std::shared_ptr<GpuProcessingPhaseLease> lease;
    std::uint64_t admissionRevision = 0;
};

class GpuProcessingPhaseBudget final {
  public:
    explicit GpuProcessingPhaseBudget(
        std::size_t requestCapacity = 32,
        const std::shared_ptr<PackedHostRetirementOwner>& retirementOwner = {});
    // Unknown/stale delivery closes new admission but does not erase the last resolved constraint
    // or free live leases. A reliable measured zero is a ready, zero capacity (BudgetRefused).
    // Runtime supplies the delivered sample deadline. Explicit legacy/manual quotas default to
    // no expiry; new supplemental host allocation cannot use an expired delivered sample.
    void setHostCapacity(std::optional<std::uint64_t> bytes, bool ready,
                         std::chrono::steady_clock::time_point validUntil =
                             std::chrono::steady_clock::time_point::max()) noexcept;
    // Apply one measured sample and its exact initial credit atomically with retirements that
    // occurred after the sampling bracket. Subsequent retirement revokes the remaining credit.
    void setSampledHostCapacity(std::optional<std::uint64_t> bytes, bool ready,
                                std::chrono::steady_clock::time_point validUntil,
                                std::uint64_t creditBytes,
                                std::uint64_t retiredBytesAtSample) noexcept;
    // Duplicate/stale delivery may tighten an existing capacity only. Keep its current readiness
    // and expiry for a duplicate; a stale delivery closes readiness at the supplied deadline.
    void tightenHostCapacity(std::optional<std::uint64_t> upperBound, bool closeReadiness,
                             std::chrono::steady_clock::time_point validUntil) noexcept;
    void setDeviceCapacity(std::optional<std::uint64_t> bytes, bool ready) noexcept;
    // The device's image allocation sizes (null clears), reported with its allocation budget.
    void setAllocationSizes(std::shared_ptr<const GpuAllocationSizes> sizes) noexcept;
    // The host ceiling described on GpuProcessingPhaseSnapshot::hostCeilingBytes.
    void setHostCeiling(std::optional<std::uint64_t> bytes) noexcept;
    // The device ceiling described on GpuProcessingPhaseSnapshot::deviceCeilingBytes.
    void setDeviceCeiling(std::optional<std::uint64_t> bytes) noexcept;
    // Moves a known host capacity by a preference change (headroom), keeping readiness/expiry.
    void shiftHostCapacity(std::uint64_t add, std::uint64_t subtract) noexcept;
    // Blocks until the admission revision differs from `seen` (a release, a capacity change) or
    // the deadline passes. Returns true when it changed. Releases from storage tokens are seen
    // within one short slice even when they do not notify.
    [[nodiscard]] bool waitForRevisionChange(std::uint64_t seen,
                                             std::chrono::steady_clock::time_point deadline) const;
    [[nodiscard]] GpuProcessingPhaseSnapshot snapshot() const noexcept;
    [[nodiscard]] RootedHostBackingSnapshot rootedHostBackingSnapshot() const noexcept;
    [[nodiscard]] GpuProcessingRootIdentity rootIdentity() const noexcept { return state_; }
    [[nodiscard]] GpuProcessingPhaseGrant reserve(const GpuProcessingPhaseRequest&) noexcept;
    // The host need reserve() would claim for this request now: its fixed bytes plus three times
    // its source need (the stated minimum, or its envelope's measured retention).
    [[nodiscard]] std::uint64_t hostNeedBytes(const GpuProcessingPhaseRequest&) const noexcept;

  private:
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state_;
};
} // namespace bloom::render
