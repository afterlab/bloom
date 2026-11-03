#pragma once

#include <bloom/render/gpu_processing_phase_budget.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <limits>
#include <mutex>

namespace bloom::render::detail {
// The stated envelope of a phase: phases with equal values (frames of one composition at one
// resolution, with one plan) share their measured device peaks and retained source bytes.
struct PhaseEnvelopeKey final {
    std::uint64_t workingBytes = 0;
    std::uint64_t minimumExecutorBytes = 0;
    std::uint64_t fixedHostBytes = 0;
    std::uint64_t minimumSourceBytes = 0;
    friend bool operator==(const PhaseEnvelopeKey&, const PhaseEnvelopeKey&) = default;
};
// The largest measured value of one kind (device storage held at one time by a released phase,
// or source bytes retained by a phase that finished preparation) for one envelope.
struct MeasuredEnvelopePeak final {
    PhaseEnvelopeKey key;
    std::uint64_t peakBytes = 0;
    std::uint64_t lastUse = 0;
};
inline constexpr std::size_t kMeasuredEnvelopePeaks = 16;
// The root keeps the most recently used envelopes of each kind, least recently used replaced first.
struct MeasuredEnvelopePeaks final {
    std::array<MeasuredEnvelopePeak, kMeasuredEnvelopePeaks> entries{};
    std::uint64_t use = 0;
};

struct GpuProcessingPhaseBudgetState final {
    mutable std::mutex mutex;
    // Signalled after phase releases and capacity changes (outside `mutex`).
    mutable std::condition_variable changed;
    GpuProcessingPhaseSnapshot counters;
    RootedHostBackingSnapshot rootedHostBacking;
    // The current host-capacity sample may include this much rooted backing credit. It is
    // revoked before an eligible allocation begins freeing, even if malloc retains its pages.
    std::uint64_t hostBackingCreditRemaining = 0;
    std::uint64_t nextId = 1;
    std::shared_ptr<PackedHostRetirementState> retirement;
    std::chrono::steady_clock::time_point hostValidUntil =
        std::chrono::steady_clock::time_point::max();
    // Measured per-envelope device peaks (#417) and retained source bytes (#472).
    MeasuredEnvelopePeaks measuredDevicePeaks;
    MeasuredEnvelopePeaks measuredSourceRetention;
};
[[nodiscard]] inline std::uint64_t remaining(const std::uint64_t cap,
                                             const std::uint64_t reserved) noexcept {
    return cap > reserved ? cap - reserved : 0;
}
inline void incrementSaturating(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

// Updates the claim and native-allocation high-water marks after a change that may raise them.
inline void notePeaksLocked(GpuProcessingPhaseSnapshot& counters) noexcept {
    counters.peakTotalDeviceClaimBytes =
        std::max(counters.peakTotalDeviceClaimBytes, counters.totalDeviceClaimBytes);
    const auto images = counters.ownedNativeImageBytes;
    const auto owned = images > std::numeric_limits<std::uint64_t>::max() -
                                    counters.ownedNativeBufferBytes
                           ? std::numeric_limits<std::uint64_t>::max()
                           : images + counters.ownedNativeBufferBytes;
    counters.peakOwnedNativeBytes = std::max(counters.peakOwnedNativeBytes, owned);
}

// The need of a phase with this stated envelope: the stated need until a phase of the same
// envelope has been measured, then the smaller of the stated need and the largest measured value
// plus a quarter. Device peaks (#417) are recorded when a phase is released, retained source
// bytes (#472) when a phase finishes source preparation; growth beyond the need is admitted
// against the live room.
[[nodiscard]] inline std::uint64_t measuredNeed(const MeasuredEnvelopePeaks& peaks,
                                                const PhaseEnvelopeKey& key,
                                                const std::uint64_t stated) noexcept {
    for (const auto& entry : peaks.entries) {
        if (entry.peakBytes != 0 && entry.key == key) {
            const auto margin = entry.peakBytes / 4 + (entry.peakBytes % 4 != 0 ? 1 : 0);
            const auto measured = entry.peakBytes > std::numeric_limits<std::uint64_t>::max() -
                                                        margin
                                      ? std::numeric_limits<std::uint64_t>::max()
                                      : entry.peakBytes + margin;
            return std::min(stated, measured);
        }
    }
    return stated;
}
// measuredNeed for an admission: also marks the envelope as recently used.
[[nodiscard]] inline std::uint64_t measuredNeedLocked(MeasuredEnvelopePeaks& peaks,
                                                      const PhaseEnvelopeKey& key,
                                                      const std::uint64_t stated) noexcept {
    for (auto& entry : peaks.entries) {
        if (entry.peakBytes != 0 && entry.key == key)
            entry.lastUse = ++peaks.use;
    }
    return measuredNeed(peaks, key, stated);
}
// Keeps the largest value per envelope, so an early-ended or lighter phase never lowers a need.
inline void recordMeasuredPeakLocked(MeasuredEnvelopePeaks& peaks, const PhaseEnvelopeKey& key,
                                     const std::uint64_t peak) noexcept {
    MeasuredEnvelopePeak* slot = nullptr;
    for (auto& entry : peaks.entries) {
        if (entry.peakBytes != 0 && entry.key == key) {
            slot = &entry;
            break;
        }
        if (!slot || entry.peakBytes == 0 ||
            (slot->peakBytes != 0 && entry.lastUse < slot->lastUse))
            slot = &entry;
    }
    if (slot->peakBytes == 0 || !(slot->key == key))
        *slot = {.key = key, .peakBytes = 0, .lastUse = 0};
    slot->peakBytes = std::max(slot->peakBytes, peak);
    slot->lastUse = ++peaks.use;
}

struct PresentationAdmission final {
    GpuProcessingPhaseAdmission status;
    GpuProcessingPhaseDomain domain;
    std::uint64_t revision;
};
// Independent, exact-root allocation admission. This is a native backing claim, never a phase
// count slot or a conversion of another frame's working/source/output allowance.
[[nodiscard]] inline PresentationAdmission
admitPresentation(GpuProcessingPhaseBudgetState& state, const std::uint64_t bytes, const bool local,
                  const bool image, const std::chrono::steady_clock::time_point now) noexcept {
    const std::lock_guard lock(state.mutex);
    auto& counters = state.counters;
    const auto refuse = [&](GpuProcessingPhaseAdmission status, GpuProcessingPhaseDomain domain) {
        if (status == GpuProcessingPhaseAdmission::BudgetRefused)
            incrementSaturating(counters.budgetRefusals);
        return PresentationAdmission{status, domain, counters.admissionRevision};
    };
    if (counters.hostReady && now > state.hostValidUntil) {
        counters.hostReady = false;
        incrementSaturating(counters.admissionRevision);
    }
    if (!counters.hostReady || !counters.hostCapacityBytes || now > state.hostValidUntil)
        return refuse(GpuProcessingPhaseAdmission::AwaitingCapacity,
                      GpuProcessingPhaseDomain::Host);
    if (!counters.deviceReady || !counters.deviceCapacityBytes)
        return refuse(GpuProcessingPhaseAdmission::AwaitingCapacity,
                      GpuProcessingPhaseDomain::Device);
    if (bytes > remaining(*counters.deviceCapacityBytes, counters.totalDeviceClaimBytes))
        return refuse(GpuProcessingPhaseAdmission::BudgetRefused, GpuProcessingPhaseDomain::Device);
    if (!local && bytes > remaining(*counters.hostCapacityBytes, counters.totalHostClaimBytes))
        return refuse(GpuProcessingPhaseAdmission::BudgetRefused, GpuProcessingPhaseDomain::Host);
    counters.totalDeviceClaimBytes += bytes;
    if (image) {
        counters.ownedNativeImageBytes += bytes;
        counters.ownedPresentationImageBytes += bytes;
        ++counters.ownedNativeImages;
        ++counters.ownedPresentationImages;
    } else {
        counters.ownedNativeBufferBytes += bytes;
        counters.ownedPresentationBufferBytes += bytes;
        ++counters.ownedNativeBuffers;
        ++counters.ownedPresentationBuffers;
    }
    notePeaksLocked(counters);
    if (!local) {
        counters.totalHostClaimBytes += bytes;
        if (image)
            counters.ownedNonlocalNativeHostBytes += bytes;
        else
            counters.ownedNonlocalNativeBufferHostBytes += bytes;
    }
    return {GpuProcessingPhaseAdmission::Accepted, GpuProcessingPhaseDomain::None,
            counters.admissionRevision};
}
} // namespace bloom::render::detail
