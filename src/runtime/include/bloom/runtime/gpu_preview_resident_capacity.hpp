#pragma once

// Capacity-aware admission policy for the GPU-resident preview route.
//
// The resident frame cache must never be sized from host RAM: a large RAM-preview budget is not
// evidence of usable VRAM. The GPU owner thread resolves the real device allocation budget
// (render::GpuDevice::availableAllocationBudget()) into ONE device pool. Usable VRAM (the driver's
// live budget minus the VRAM headroom preference, applied by GpuProcessingPhasePolicy at every
// admission and native allocation) is the only device ceiling (#425): the frame-cache,
// lease-registry, scene-cache and per-request ledgers are NOT partitioned by fixed fractions. Each
// is the device pool, or a smaller explicit configured ceiling, and none is additive with another.
// Caches are evicted on demand when work waits for device memory. Every value stays finite, is
// never raised above an explicit configured ceiling, and an unknown device budget falls back to a
// small safe pool.
//
// Pure functions only: no device, no thread, no allocation. The owner thread produces the resolved
// capacity and the clamps; tests exercise the policy with synthetic budgets.

#include <bloom/render/gpu_device.hpp>

#include <cstdint>

namespace bloom::runtime {

// Mirrors render::GpuAllocationBudgetSource in the runtime vocabulary. `MemoryBudget` is an actual
// live available figure; `DeviceLocalEstimate` is a nominal DEVICE_LOCAL capacity; `Unknown` means
// no budget could be resolved (null/stub/foreign-thread device or an empty heap report).
enum class GpuResidentCapacitySource : std::uint8_t {
    Unknown,
    MemoryBudget,
    DeviceLocalEstimate,
};

// Immutable resolved device capacity for the resident route.
struct GpuResidentCapacity final {
    GpuResidentCapacitySource source = GpuResidentCapacitySource::Unknown;
    // Conservative available allocation budget. Only meaningful when `source != Unknown`; a known
    // live budget may legitimately report zero free bytes, which must NOT be confused with an
    // unresolved device.
    std::uint64_t availableBytes = 0;
    // Summed DEVICE_LOCAL heap size; a diagnostic fact, never an availability claim.
    std::uint64_t deviceLocalBytes = 0;

    // True once the device actually answered (even with a zero budget). The safe 256 MiB fallback
    // is reserved for a genuinely unresolved device, never for a known-zero one.
    [[nodiscard]] bool isKnown() const noexcept {
        return source != GpuResidentCapacitySource::Unknown;
    }

    friend bool operator==(const GpuResidentCapacity&, const GpuResidentCapacity&) = default;
};

// Safe pool when the device budget is genuinely unknown.
inline constexpr std::uint64_t kUnknownResidentPoolBytes = 256ULL * 1024ULL * 1024ULL;
// Finite metadata limits independent of image extent. Actual native byte admission remains
// authoritative. One eighth of the lease slots stays outside the cache for active/tombstoned work.
inline constexpr std::uint64_t kResidentCacheEntryCeiling = 4096;
inline constexpr std::uint64_t kResidentLeaseEntryCeiling = 4096;

// The artist-visible configured upper bounds for the resident route, already partitioned. The
// owner clamps each against the capacity pool; none is ever raised.
struct GpuResidentConfiguredBudgets final {
    std::uint64_t leaseBytes = 0;
    std::uint64_t sceneCacheBytes = 0;
    std::uint64_t requestBytes = 0;

    friend bool operator==(const GpuResidentConfiguredBudgets&,
                           const GpuResidentConfiguredBudgets&) = default;
};

// The resolved resident admission plan. `poolBytes` is the device pool the route may use; every
// ledger is at most the pool and they are not additive: what each actually holds is real device
// memory, admitted against usable VRAM when it was allocated and evicted on demand.
struct GpuResidentCapacityPlan final {
    // True only for a plan the GPU owner actually resolved and published. A configured host-only
    // plan or a default-constructed plan is NOT resolved, so a consumer can tell "not resolved yet"
    // apart from "resolved to the conservative unknown-device fallback" and from a known-zero
    // device. This is the sole discriminator the UI bootstrap acts on.
    bool resolved = false;
    GpuResidentCapacity capacity;
    std::uint64_t poolBytes = 0;
    std::uint64_t cacheBytes = 0;
    std::uint64_t cacheEntries = 0;
    std::uint64_t leaseBytes = 0;
    std::uint64_t leaseEntries = 0;
    std::uint64_t sceneCacheBytes = 0;
    std::uint64_t requestBytes = 0;

    friend bool operator==(const GpuResidentCapacityPlan&,
                           const GpuResidentCapacityPlan&) = default;
};

// Maps the render device budget into the runtime vocabulary without inventing availability.
[[nodiscard]] GpuResidentCapacity
gpuResidentCapacityFromAllocationBudget(const render::GpuAllocationBudget& allocation) noexcept;

// Stable construction limit for rooted retention and producer pipelines. Live free bytes can be
// zero during startup and must not become a lifetime cache/registry/image ceiling, so this is the
// whole DEVICE_LOCAL heap; actual allocation still needs the current foreground and shared-root
// admission checks, which apply the live budget minus the VRAM headroom.
[[nodiscard]] GpuResidentCapacity
gpuResidentConstructionCapacity(const GpuResidentCapacity& observed) noexcept;

// Pure host-configured plan for an explicit ceiling: every ledger is that ceiling (no partition).
// Used at construction, before any device exists, to set the upper bounds the owner later clamps.
// A zero ceiling yields an all-zero plan (an explicit "no resident caching" choice is respected,
// never floored upward).
[[nodiscard]] GpuResidentCapacityPlan
gpuResidentCapacityPlanConfigured(std::uint64_t configuredCeilingBytes) noexcept;

// Pure capacity-aware clamp: each ledger is its configured value clamped to the resolved device
// pool, independently (no proportional share). An unknown budget uses the safe fallback pool.
[[nodiscard]] GpuResidentCapacityPlan
gpuResidentCapacityPlanFor(const GpuResidentConfiguredBudgets& configured,
                           const GpuResidentCapacity& capacity) noexcept;

// A rooted foreground request may use device space that cache/lease ceilings have not actually
// claimed. This is a NON-additive admission ceiling, sampled on the GPU owner: MemoryBudget is
// already free space after all VMA allocations, while a nominal/unknown report subtracts the
// generation's actually owned image bytes from the heap size. The shared phase
// reservation and native allocation checks still bound active work; cache targets are retention
// preferences, not reservations that reduce an otherwise idle foreground request.
[[nodiscard]] std::uint64_t
gpuResidentForegroundRequestBudget(const GpuResidentCapacity& capacity,
                                   std::uint64_t ownedImageBytes) noexcept;

// One request's DEVICE-stage byte budget: the caller's host/decode pixel-storage ceiling clamped
// down to the owner-resolved GPU request ceiling. A host ceiling larger than device capacity is
// clamped, never turned into a GPU refusal; the host ceiling itself is preserved for decoding and
// the CPU fallback. A zero host ceiling or a zero GPU ceiling yields zero, which is the honest
// "no device admission" signal the caller turns into the CPU path.
[[nodiscard]] std::uint64_t gpuResidentRequestBudget(std::uint64_t hostByteAllowance,
                                                     std::uint64_t gpuRequestCeiling) noexcept;

} // namespace bloom::runtime
