#include <bloom/render/gpu_processing_phase_budget.hpp>
#include <bloom/render/native_image_storage.hpp>
#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/render/processing_host_storage.hpp>

#include "gpu_processing_phase_budget_private.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace bloom::render {
using detail::incrementSaturating;
using detail::remaining;
namespace {
void retireHostBackingLocked(detail::GpuProcessingPhaseBudgetState& state,
                             const std::uint64_t total) noexcept {
    auto& backing = state.rootedHostBacking;
    backing.materializedBytes -= total;
    if (total > std::numeric_limits<std::uint64_t>::max() - backing.retiredMaterializedBytes) {
        backing.retiredMaterializedBytes = std::numeric_limits<std::uint64_t>::max();
        backing.retiredBytesSaturated = true;
    } else {
        backing.retiredMaterializedBytes += total;
    }
    const auto revoked = std::min(total, state.hostBackingCreditRemaining);
    state.hostBackingCreditRemaining -= revoked;
    if (revoked != 0 && state.counters.hostCapacityBytes) {
        auto& capacity = *state.counters.hostCapacityBytes;
        const auto reduced = capacity > revoked ? capacity - revoked : 0;
        if (reduced != capacity) {
            capacity = reduced;
            incrementSaturating(state.counters.admissionRevision);
        }
    }
}
// #472: preparation decodes and converts one source at a time. Its transient scratch is twice the
// largest single source (the decoded and converted copies beside the retained ones), never more
// than twice the whole source need; an unknown largest source keeps that whole-need bound.
std::uint64_t sourceScratchFor(const GpuProcessingPhaseRequest& request,
                               const std::uint64_t sourceNeed) noexcept {
    const auto largest = request.largestSourceBytes != 0
                             ? std::min(request.largestSourceBytes, request.minimumSourceBytes)
                             : sourceNeed;
    const auto max = std::numeric_limits<std::uint64_t>::max();
    return largest > max / 2 ? max : 2 * largest;
}
detail::PhaseEnvelopeKey envelopeKey(const GpuProcessingPhaseRequest& request) noexcept {
    return {request.deviceWorkingBytes, request.minimumExecutorBytes, request.fixedHostBytes,
            request.minimumSourceBytes};
}
} // namespace

GpuProcessingPhaseLease::GpuProcessingPhaseLease(
    std::shared_ptr<detail::GpuProcessingPhaseBudgetState> state) noexcept
    : state_(std::move(state)) {}

GpuProcessingPhaseLease::~GpuProcessingPhaseLease() {
    if (id_ == 0) {
        return;
    }
    releaseLocked();
    state_->changed.notify_all();
}

void GpuProcessingPhaseLease::releaseLocked() noexcept {
    const std::lock_guard lock(state_->mutex);
    auto& counters = state_->counters;
    counters.hostReservedBytes -= remainingHostBytes_;
    counters.totalHostClaimBytes -= remainingHostBytes_;
    counters.deviceReservedBytes -= deviceClaimBytes_;
    counters.totalDeviceClaimBytes -= deviceClaimBytes_;
    if (deviceBytes_ != 0 && !unproven_ && peakAllocatedDeviceBytes_ != 0)
        detail::recordMeasuredPeakLocked(state_->measuredDevicePeaks,
                                         {envelopeWorkingBytes_, envelopeMinimumExecutorBytes_,
                                          envelopeFixedHostBytes_, envelopeMinimumSourceBytes_},
                                         peakAllocatedDeviceBytes_);
    if (deviceBytes_ != 0) {
        ++counters.releasedNativePhases;
        counters.releasedPhaseDeviceNeedBytes += deviceNeedBytes_;
        counters.releasedPhaseDevicePeakBytes += peakAllocatedDeviceBytes_;
        counters.peakPhaseDeviceNeedBytes =
            std::max(counters.peakPhaseDeviceNeedBytes, deviceNeedBytes_);
        counters.peakPhaseDeviceAllocationBytes =
            std::max(counters.peakPhaseDeviceAllocationBytes, peakAllocatedDeviceBytes_);
        if (!unproven_ && peakAllocatedDeviceBytes_ != 0)
            counters.recentPhaseDevicePeakBytes =
                std::max(peakAllocatedDeviceBytes_, counters.recentPhaseDevicePeakBytes -
                                                        counters.recentPhaseDevicePeakBytes / 8);
    }
    if (retired_) {
        --counters.retainedPhases;
        counters.retainedProcessingHostBytes -= ownedHostBackingBytes_;
    } else {
        --counters.admittedPhases;
    }
    incrementSaturating(counters.admissionRevision);
    if (unproven_) {
        --counters.unprovenPhases;
    }
}

namespace detail {
ProcessingHostBackingToken::ProcessingHostBackingToken(
    std::shared_ptr<GpuProcessingPhaseLease> phase, const std::uint64_t allocatedBytes,
    const std::uint64_t initializedBytes) noexcept
    : phase_(std::move(phase)), allocatedBytes_(allocatedBytes),
      initializedBytes_(initializedBytes), materialized_(true) {}
ProcessingHostBackingToken::ProcessingHostBackingToken(ProcessingHostBackingToken&& other) noexcept
    : phase_(std::move(other.phase_)), allocatedBytes_(std::exchange(other.allocatedBytes_, 0)),
      initializedBytes_(std::exchange(other.initializedBytes_, 0)),
      materialized_(std::exchange(other.materialized_, false)) {}
ProcessingHostBackingToken::~ProcessingHostBackingToken() {
    retireBeforeFree();
    releaseAfterFree();
}
std::optional<ProcessingHostBackingToken>
ProcessingHostBackingToken::transfer(const std::shared_ptr<GpuProcessingPhaseLease>& phase,
                                     const std::uint64_t allocatedBytes,
                                     const std::uint64_t initializedBytes) noexcept {
    if (!phase || initializedBytes == 0 || initializedBytes > allocatedBytes)
        return {};
    const std::lock_guard lock(phase->state_->mutex);
    auto& state = *phase->state_;
    auto& counters = state.counters;
    if (phase->id_ == 0 || phase->unproven_ || phase->retired_ || allocatedBytes > phase->remainingOutputHostBytes_ ||
        allocatedBytes > phase->remainingHostBytes_)
        return {};
    // The allocation was constructed while the fixed phase claim was held. The full allocated
    // capacity stays claimed; only initialized bytes can offset a later physical sample.
    // This atomic transfer changes ownership, never the total claim or admission capacity.
    phase->remainingOutputHostBytes_ -= allocatedBytes;
    phase->remainingHostBytes_ -= allocatedBytes;
    phase->ownedHostBackingBytes_ += allocatedBytes;
    counters.hostReservedBytes -= allocatedBytes;
    counters.ownedProcessingHostBytes += allocatedBytes;
    ++counters.ownedProcessingHostAllocations;
    state.rootedHostBacking.materializedBytes += initializedBytes;
    return ProcessingHostBackingToken(phase, allocatedBytes, initializedBytes);
}
void ProcessingHostBackingToken::retireBeforeFree() noexcept {
    if (!phase_ || !materialized_)
        return;
    const std::lock_guard lock(phase_->state_->mutex);
    retireHostBackingLocked(*phase_->state_, initializedBytes_);
    materialized_ = false;
}
void ProcessingHostBackingToken::releaseAfterFree() noexcept {
    if (!phase_)
        return;
    std::shared_ptr<GpuProcessingPhaseLease> releasedPhase;
    {
        const std::lock_guard lock(phase_->state_->mutex);
        auto& counters = phase_->state_->counters;
        counters.ownedProcessingHostBytes -= allocatedBytes_;
        --counters.ownedProcessingHostAllocations;
        phase_->ownedHostBackingBytes_ -= allocatedBytes_;
        if (phase_->retired_)
            counters.retainedProcessingHostBytes -= allocatedBytes_;
        phase_->remainingOutputHostBytes_ += allocatedBytes_;
        phase_->remainingHostBytes_ += allocatedBytes_;
        counters.hostReservedBytes += allocatedBytes_;
        releasedPhase = std::move(phase_);
        allocatedBytes_ = 0;
        initializedBytes_ = 0;
    }
    releasedPhase.reset();
}
} // namespace detail

std::uint64_t GpuProcessingPhaseLease::peakDeviceAllocationBytes() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return peakAllocatedDeviceBytes_;
}

std::uint64_t GpuProcessingPhaseLease::revisionAfterOwnRelease() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return admissionRevision_ == std::numeric_limits<std::uint64_t>::max() ? admissionRevision_
                                                                           : admissionRevision_ + 1;
}

bool GpuProcessingPhaseLease::drawDeviceLocked(const std::uint64_t bytes) noexcept {
    auto& counters = state_->counters;
    if (bytes > remainingDeviceBytes_)
        return false;
    // The first allocation of a device stage claims the phase's whole need when the live device
    // room holds it, so the rest of the stage cannot be crowded out. Without that room it claims
    // nothing up front and every allocation is admitted against live capacity, as growth is.
    // Callers have checked this draw against live capacity, so it cannot fail after the claim.
    if (allocatedDeviceBytes_ == 0 && deviceClaimBytes_ == 0 && deviceClaimCapBytes_ != 0 &&
        counters.deviceCapacityBytes &&
        deviceClaimCapBytes_ <=
            remaining(*counters.deviceCapacityBytes, counters.totalDeviceClaimBytes)) {
        deviceClaimBytes_ = deviceClaimCapBytes_;
        counters.deviceReservedBytes += deviceClaimCapBytes_;
        counters.totalDeviceClaimBytes += deviceClaimCapBytes_;
        counters.peakDeviceReservedBytes =
            std::max(counters.peakDeviceReservedBytes, counters.deviceReservedBytes);
    }
    const auto fromClaim = std::min(bytes, deviceClaimBytes_);
    const auto growth = bytes - fromClaim;
    if (growth != 0 &&
        (!counters.deviceCapacityBytes ||
         growth > remaining(*counters.deviceCapacityBytes, counters.totalDeviceClaimBytes)))
        return false;
    remainingDeviceBytes_ -= bytes;
    allocatedDeviceBytes_ += bytes;
    peakAllocatedDeviceBytes_ = std::max(peakAllocatedDeviceBytes_, allocatedDeviceBytes_);
    deviceClaimBytes_ -= fromClaim;
    counters.deviceReservedBytes -= fromClaim;
    counters.totalDeviceClaimBytes += growth;
    return true;
}

void GpuProcessingPhaseLease::returnDeviceLocked(const std::uint64_t bytes) noexcept {
    auto& counters = state_->counters;
    remainingDeviceBytes_ += bytes;
    allocatedDeviceBytes_ -= std::min(bytes, allocatedDeviceBytes_);
    // Within a stage, freed storage goes back to the phase's claim (up to its need) for its next
    // allocation. Once the phase holds no native storage its device stage is over: the remaining
    // claim is released, so a phase that is preparing, reading back or publishing claims nothing.
    // Like any return of the phase's own storage this is quiet (no admission revision): waiters
    // resume on the phase's release, so a failed attempt's own cleanup cannot retry itself. An
    // unproven phase keeps its whole claim until it is proven retired, as before.
    if (allocatedDeviceBytes_ == 0 && !unproven_) {
        counters.deviceReservedBytes -= deviceClaimBytes_;
        counters.totalDeviceClaimBytes -= deviceClaimBytes_ + bytes;
        deviceClaimBytes_ = 0;
        return;
    }
    const auto refill = std::min(
        bytes, deviceClaimCapBytes_ - std::min(deviceClaimCapBytes_, deviceClaimBytes_));
    deviceClaimBytes_ += refill;
    counters.deviceReservedBytes += refill;
    counters.totalDeviceClaimBytes -= bytes - refill;
}

void GpuProcessingPhaseLease::endDeviceExecution() noexcept {
    {
        const std::lock_guard lock(state_->mutex);
        if (id_ == 0 || unproven_)
            return;
        auto& counters = state_->counters;
        deviceClaimCapBytes_ = std::min(deviceClaimCapBytes_, envelopeWorkingBytes_);
        if (deviceClaimBytes_ <= deviceClaimCapBytes_)
            return;
        const auto released = deviceClaimBytes_ - deviceClaimCapBytes_;
        deviceClaimBytes_ = deviceClaimCapBytes_;
        counters.deviceReservedBytes -= released;
        counters.totalDeviceClaimBytes -= released;
        // Released room is observable: waiting work retries on it.
        incrementSaturating(counters.admissionRevision);
    }
    state_->changed.notify_all();
}

void GpuProcessingPhaseLease::transferToRetentionLocked(const std::uint64_t bytes) noexcept {
    auto& counters = state_->counters;
    const auto moved = std::min(bytes, allocatedDeviceBytes_);
    allocatedDeviceBytes_ -= moved;
    remainingDeviceBytes_ += moved;
    // The moved bytes stay in totalDeviceClaimBytes: the published storage still exists and is
    // released to the root, not to this phase. A phase left holding no storage of its own ends its
    // device stage as a return would.
    if (allocatedDeviceBytes_ == 0 && !unproven_) {
        counters.deviceReservedBytes -= deviceClaimBytes_;
        counters.totalDeviceClaimBytes -= deviceClaimBytes_;
        deviceClaimBytes_ = 0;
    }
}

bool GpuProcessingPhaseLease::finishSourcePreparation(
    const std::uint64_t retainedSourceBytes) noexcept {
    const std::lock_guard lock(state_->mutex);
    if (id_ == 0 || unproven_ || sourcePreparationFinished_ ||
        retainedSourceBytes > sourceCeilingBytes_ || remainingUploadBytes_ != sourceCeilingBytes_)
        return false;
    // S/C/U protected preparation at the claimed source need before actual source sizes were
    // known. Only the actual retained sources survive; sources beyond the need already exist in
    // memory and are charged as they are. Uploads start after this handoff; their actual backing
    // is admitted separately against live capacity. The retained bytes are recorded under the
    // phase's envelope, so later phases of it claim from what a phase actually retains (#472).
    auto& counters = state_->counters;
    const auto record = [&] {
        sourcePreparationFinished_ = true;
        if (retainedSourceBytes != 0)
            detail::recordMeasuredPeakLocked(state_->measuredSourceRetention,
                                             {envelopeWorkingBytes_, envelopeMinimumExecutorBytes_,
                                              envelopeFixedHostBytes_, envelopeMinimumSourceBytes_},
                                             retainedSourceBytes);
        ++counters.finishedSourcePhases;
        counters.finishedPhaseSourceNeedBytes += sourceBytes_;
        counters.finishedPhaseRetainedSourceBytes += retainedSourceBytes;
        counters.peakPhaseSourceNeedBytes =
            std::max(counters.peakPhaseSourceNeedBytes, sourceBytes_);
        counters.peakPhaseRetainedSourceBytes =
            std::max(counters.peakPhaseRetainedSourceBytes, retainedSourceBytes);
    };
    const auto claimed = sourceBytes_ + sourceScratchBytes_;
    if (retainedSourceBytes > claimed) {
        const auto growth = retainedSourceBytes - claimed;
        record();
        remainingHostBytes_ += growth;
        hostBytes_ += growth;
        counters.hostReservedBytes += growth;
        counters.totalHostClaimBytes += growth;
        counters.peakHostReservedBytes =
            std::max(counters.peakHostReservedBytes, counters.hostReservedBytes);
        return true;
    }
    const auto released = claimed - retainedSourceBytes;
    if (released > remainingHostBytes_)
        return false;
    record();
    remainingHostBytes_ -= released;
    counters.hostReservedBytes -= released;
    counters.totalHostClaimBytes -= released;
    if (released != 0) {
        incrementSaturating(counters.admissionRevision);
        // This phase's own partial release must not make a later failed request retry itself.
        // Increment only its own watermark, preserving intervening independent release events.
        incrementSaturating(admissionRevision_);
    }
    return true;
}

GpuProcessingPhaseAdmission
GpuProcessingPhaseLease::claimRetainedHostBytes(const std::uint64_t bytes) noexcept {
    const std::lock_guard lock(state_->mutex);
    auto& counters = state_->counters;
    if (id_ == 0 || unproven_ || retired_ || !counters.hostCapacityBytes)
        return GpuProcessingPhaseAdmission::InvalidRequest;
    if (bytes > remaining(*counters.hostCapacityBytes, counters.totalHostClaimBytes)) {
        const auto limit =
            std::max(*counters.hostCapacityBytes, counters.hostCeilingBytes.value_or(0));
        if (bytes > std::numeric_limits<std::uint64_t>::max() - hostBytes_ ||
            hostBytes_ + bytes > limit) {
            incrementSaturating(counters.budgetRefusals);
            return GpuProcessingPhaseAdmission::BudgetRefused;
        }
        return GpuProcessingPhaseAdmission::AwaitingCapacity;
    }
    remainingHostBytes_ += bytes;
    hostBytes_ += bytes;
    counters.hostReservedBytes += bytes;
    counters.totalHostClaimBytes += bytes;
    counters.peakHostReservedBytes =
        std::max(counters.peakHostReservedBytes, counters.hostReservedBytes);
    detail::notePeaksLocked(counters);
    return GpuProcessingPhaseAdmission::Accepted;
}

GpuProcessingPhaseAdmission
GpuProcessingPhaseLease::raiseExecutorNeed(const std::uint64_t bytes) noexcept {
    const std::lock_guard lock(state_->mutex);
    auto& counters = state_->counters;
    if (id_ == 0 || unproven_ || retired_ || deviceBytes_ == 0 || !counters.deviceCapacityBytes)
        return GpuProcessingPhaseAdmission::InvalidRequest;
    const auto max = std::numeric_limits<std::uint64_t>::max();
    if (bytes > max - envelopeWorkingBytes_)
        return GpuProcessingPhaseAdmission::BudgetRefused;
    const std::uint64_t need = envelopeWorkingBytes_ + bytes;
    if (need <= deviceNeedBytes_ && bytes <= executorBytes_)
        return GpuProcessingPhaseAdmission::Accepted;
    const std::uint64_t deviceLimit =
        std::max(*counters.deviceCapacityBytes, counters.deviceCeilingBytes.value_or(0));
    if ((requestedExecutorBytes_ != 0 && bytes > requestedExecutorBytes_) || need > deviceLimit) {
        incrementSaturating(counters.budgetRefusals);
        return GpuProcessingPhaseAdmission::BudgetRefused;
    }
    // Before the device stage the whole need must fit the live room, as at admission; during it
    // only the growth beyond the claim already held is new.
    const bool staged = allocatedDeviceBytes_ != 0 || deviceClaimBytes_ != 0;
    const std::uint64_t growth =
        need > deviceClaimCapBytes_ ? need - deviceClaimCapBytes_ : std::uint64_t{0};
    if ((staged ? growth : need) >
        remaining(*counters.deviceCapacityBytes, counters.totalDeviceClaimBytes))
        return GpuProcessingPhaseAdmission::AwaitingCapacity;
    if (staged && growth != 0) {
        deviceClaimBytes_ += growth;
        counters.deviceReservedBytes += growth;
        counters.totalDeviceClaimBytes += growth;
        counters.peakDeviceReservedBytes =
            std::max(counters.peakDeviceReservedBytes, counters.deviceReservedBytes);
    }
    deviceNeedBytes_ = std::max(deviceNeedBytes_, need);
    deviceClaimCapBytes_ = std::max(deviceClaimCapBytes_, need);
    if (bytes > executorBytes_) {
        const auto added = bytes - executorBytes_;
        executorBytes_ = bytes;
        deviceBytes_ += added;
        remainingDeviceBytes_ += added;
    }
    return GpuProcessingPhaseAdmission::Accepted;
}

bool GpuProcessingPhaseLease::retireToRetention() noexcept {
    {
        const std::lock_guard lock(state_->mutex);
        if (id_ == 0 || retired_ || unproven_ || deviceClaimBytes_ != 0 ||
            allocatedDeviceBytes_ != 0)
            return false;
        auto& counters = state_->counters;
        // Give back the unspent reservation. A backing freed later returns its bytes to
        // remainingHostBytes_, which the release subtracts, so the totals stay exact.
        counters.hostReservedBytes -= remainingHostBytes_;
        counters.totalHostClaimBytes -= remainingHostBytes_;
        remainingHostBytes_ = 0;
        remainingOutputHostBytes_ = 0;
        remainingProcessingHostScratchBytes_ = 0;
        remainingUploadBytes_ = 0;
        retired_ = true;
        --counters.admittedPhases;
        ++counters.retainedPhases;
        counters.retainedProcessingHostBytes += ownedHostBackingBytes_;
        incrementSaturating(counters.admissionRevision);
        incrementSaturating(admissionRevision_);
    }
    state_->changed.notify_all();
    return true;
}

bool GpuProcessingPhaseLease::isUnproven() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return unproven_;
}

void GpuProcessingPhaseLease::markUnproven() noexcept {
    const std::lock_guard lock(state_->mutex);
    if (id_ != 0 && !unproven_) {
        unproven_ = true;
        ++state_->counters.unprovenPhases;
    }
}

void GpuProcessingPhaseLease::proveRetiredAfterDeviceIdle() noexcept {
    const std::lock_guard lock(state_->mutex);
    if (id_ != 0 && unproven_) {
        unproven_ = false;
        --state_->counters.unprovenPhases;
        incrementSaturating(state_->counters.admissionRevision);
    }
}

PackedHostStorageToken::PackedHostStorageToken(
    std::shared_ptr<PackedHostStorageAuthority> authority, const std::uint64_t bytes,
    const std::uint64_t metadataBytes) noexcept
    : authority_(std::move(authority)), bytes_(bytes), metadataBytes_(metadataBytes) {}
PackedHostStorageToken::PackedHostStorageToken(PackedHostStorageToken&& other) noexcept
    : authority_(std::move(other.authority_)), bytes_(std::exchange(other.bytes_, 0)),
      metadataBytes_(std::exchange(other.metadataBytes_, 0)),
      materialized_(std::exchange(other.materialized_, false)) {}
PackedHostStorageToken& PackedHostStorageToken::operator=(PackedHostStorageToken&& other) noexcept {
    if (this != &other) {
        reset();
        authority_ = std::move(other.authority_);
        bytes_ = std::exchange(other.bytes_, 0);
        metadataBytes_ = std::exchange(other.metadataBytes_, 0);
        materialized_ = std::exchange(other.materialized_, false);
    }
    return *this;
}
PackedHostStorageToken::~PackedHostStorageToken() { reset(); }
void PackedHostStorageToken::reset() noexcept {
    if (authority_) {
        authority_->release(bytes_, metadataBytes_, materialized_);
        authority_.reset();
        bytes_ = 0;
        metadataBytes_ = 0;
        materialized_ = false;
    }
}
bool PackedHostStorageToken::published() const noexcept {
    return authority_ && authority_->snapshot().published;
}
void PackedHostStorageToken::markMaterialized() noexcept {
    if (authority_ && !materialized_) {
        authority_->materialize(bytes_, metadataBytes_);
        materialized_ = true;
    }
}
void PackedHostStorageToken::retireBeforeFree() noexcept {
    if (authority_ && materialized_) {
        authority_->retireMaterialized(bytes_, metadataBytes_);
        materialized_ = false;
    }
}
PackedHostStorageAuthority::PackedHostStorageAuthority(
    const std::shared_ptr<GpuProcessingPhaseLease>& phase, const std::uint64_t limit,
    const HostPixelStorageDomain domain) noexcept
    : state_(phase->state_), origin_(phase), domain_(domain), originId_(phase->id()),
      limit_(limit) {}
std::shared_ptr<PackedHostStorageAuthority>
PackedHostStorageAuthority::create(const std::shared_ptr<GpuProcessingPhaseLease>& phase,
                                   const std::uint64_t limit, const HostPixelStorageDomain domain) {
    if (!phase) {
        return {};
    }
    return std::shared_ptr<PackedHostStorageAuthority>(
        new PackedHostStorageAuthority(phase, limit, domain));
}
std::optional<PackedHostStorageToken>
PackedHostStorageAuthority::reserve(const std::uint64_t bytes,
                                    const std::uint64_t metadataBytes) noexcept {
    if (bytes == 0) {
        return {};
    }
    auto self = weak_from_this().lock();
    auto origin = origin_.lock();
    if (!self || !origin) {
        return {};
    }
    const std::lock_guard lock(state_->mutex);
    if (published_ || (domain_ != HostPixelStorageDomain::PackedPreview && origin->unproven_) ||
        bytes_ > limit_ || bytes > limit_ - bytes_ || bytes > origin->remainingOutputHostBytes_ ||
        metadataBytes > origin->remainingOutputHostBytes_ - bytes) {
        return {};
    }
    // Both counters change under one lock; the total never changes during an ownership handoff.
    origin->remainingOutputHostBytes_ -= bytes + metadataBytes;
    origin->remainingHostBytes_ -= bytes + metadataBytes;
    state_->counters.hostReservedBytes -= bytes + metadataBytes;
    if (domain_ == HostPixelStorageDomain::PackedPreview) {
        state_->counters.ownedPackedHostBytes += bytes;
        state_->counters.ownedPackedHostMetadataBytes += metadataBytes;
        ++state_->counters.ownedPackedHostAllocations;
    } else {
        if (domain_ == HostPixelStorageDomain::FinalProcess)
            state_->counters.ownedFinalProcessHostBytes += bytes;
        else
            state_->counters.ownedFinalEncodedHostBytes += bytes;
        state_->counters.ownedFinalHostMetadataBytes += metadataBytes;
        ++state_->counters.ownedFinalHostAllocations;
    }
    bytes_ += bytes;
    metadataBytes_ += metadataBytes;
    retainedOrigin_ = std::move(origin);
    return PackedHostStorageToken(std::move(self), bytes, metadataBytes);
}
std::shared_ptr<detail::PackedHostRetirementState>
PackedHostStorageAuthority::retirementState() const noexcept {
    return state_->retirement;
}
void PackedHostStorageAuthority::release(const std::uint64_t bytes,
                                         const std::uint64_t metadataBytes,
                                         const bool materialized) noexcept {
    std::shared_ptr<GpuProcessingPhaseLease> releasedOrigin;
    {
        const std::lock_guard lock(state_->mutex);
        if (materialized)
            retireHostBackingLocked(*state_, bytes + metadataBytes);
        bytes_ -= bytes;
        metadataBytes_ -= metadataBytes;
        if (domain_ == HostPixelStorageDomain::PackedPreview) {
            state_->counters.ownedPackedHostBytes -= bytes;
            state_->counters.ownedPackedHostMetadataBytes -= metadataBytes;
            --state_->counters.ownedPackedHostAllocations;
        } else {
            if (domain_ == HostPixelStorageDomain::FinalProcess)
                state_->counters.ownedFinalProcessHostBytes -= bytes;
            else
                state_->counters.ownedFinalEncodedHostBytes -= bytes;
            state_->counters.ownedFinalHostMetadataBytes -= metadataBytes;
            --state_->counters.ownedFinalHostAllocations;
        }
        if (published_) {
            state_->counters.totalHostClaimBytes -= bytes + metadataBytes;
            incrementSaturating(state_->counters.admissionRevision);
        } else {
            retainedOrigin_->remainingOutputHostBytes_ += bytes + metadataBytes;
            retainedOrigin_->remainingHostBytes_ += bytes + metadataBytes;
            state_->counters.hostReservedBytes += bytes + metadataBytes;
            if (bytes_ == 0) {
                releasedOrigin = std::move(retainedOrigin_);
            }
        }
    }
    // The final phase alias can run its destructor, which takes the same mutex.
    releasedOrigin.reset();
}
void PackedHostStorageAuthority::materialize(const std::uint64_t bytes,
                                             const std::uint64_t metadataBytes) noexcept {
    const std::lock_guard lock(state_->mutex);
    // These bytes already belong to the bounded total claim; this is only a sampling proof.
    state_->rootedHostBacking.materializedBytes += bytes + metadataBytes;
}
void PackedHostStorageAuthority::retireMaterialized(const std::uint64_t bytes,
                                                    const std::uint64_t metadataBytes) noexcept {
    const std::lock_guard lock(state_->mutex);
    retireHostBackingLocked(*state_, bytes + metadataBytes);
}
bool PackedHostStorageAuthority::promote() noexcept {
    std::shared_ptr<GpuProcessingPhaseLease> releasedOrigin;
    {
        const std::lock_guard lock(state_->mutex);
        if (published_) {
            return true;
        }
        if (retainedOrigin_ && retainedOrigin_->unproven_) {
            return false;
        }
        published_ = true;
        releasedOrigin = std::move(retainedOrigin_);
    }
    releasedOrigin.reset();
    return true;
}
bool PackedHostStorageAuthority::promoteFinalProduct(
    const std::shared_ptr<PackedHostStorageAuthority>& process,
    const std::shared_ptr<PackedHostStorageAuthority>& encoded) noexcept {
    if (!process || process->domain_ != HostPixelStorageDomain::FinalProcess ||
        (encoded &&
         (encoded->domain_ != HostPixelStorageDomain::FinalEncoded ||
          encoded->state_ != process->state_ || encoded->originId_ != process->originId_)))
        return false;
    std::shared_ptr<GpuProcessingPhaseLease> releasedProcess;
    std::shared_ptr<GpuProcessingPhaseLease> releasedEncoded;
    {
        const std::lock_guard lock(process->state_->mutex);
        if (process->bytes_ == 0 ||
            (encoded &&
             (encoded->bytes_ == 0 || process->published_ != encoded->published_ ||
              (!process->published_ && process->retainedOrigin_ != encoded->retainedOrigin_))) ||
            (process->retainedOrigin_ && process->retainedOrigin_->unproven_) ||
            (encoded && encoded->retainedOrigin_ && encoded->retainedOrigin_->unproven_))
            return false;
        process->published_ = true;
        releasedProcess = std::move(process->retainedOrigin_);
        if (encoded) {
            encoded->published_ = true;
            releasedEncoded = std::move(encoded->retainedOrigin_);
        }
    }
    releasedProcess.reset();
    releasedEncoded.reset();
    return true;
}
PackedHostStorageSnapshot PackedHostStorageAuthority::snapshot() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return {.bytes = bytes_, .metadataBytes = metadataBytes_, .published = published_};
}

GpuProcessingPhaseBudget::GpuProcessingPhaseBudget(
    const std::size_t requestCapacity,
    const std::shared_ptr<PackedHostRetirementOwner>& retirementOwner)
    : state_(std::make_shared<detail::GpuProcessingPhaseBudgetState>()) {
    if (requestCapacity == 0 || requestCapacity > 1024) {
        throw std::invalid_argument("processing phase request capacity must be in [1, 1024]");
    }
    state_->counters.requestCapacity = requestCapacity;
    state_->retirement = retirementOwner ? retirementOwner->producerState() : nullptr;
}

void GpuProcessingPhaseBudget::setHostCapacity(
    const std::optional<std::uint64_t> bytes, const bool ready,
    const std::chrono::steady_clock::time_point validUntil) noexcept {
    {
        const std::lock_guard lock(state_->mutex);
        const auto beforeBytes = state_->counters.hostCapacityBytes;
        const bool beforeReady = state_->counters.hostReady;
        if (bytes) {
            state_->counters.hostCapacityBytes = bytes;
        }
        state_->counters.hostReady = ready && bytes.has_value();
        state_->hostValidUntil = validUntil;
        if (beforeBytes != state_->counters.hostCapacityBytes ||
            beforeReady != state_->counters.hostReady)
            incrementSaturating(state_->counters.admissionRevision);
    }
    state_->changed.notify_all();
}
void GpuProcessingPhaseBudget::setHostCeiling(const std::optional<std::uint64_t> bytes) noexcept {
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->counters.hostCeilingBytes == bytes)
            return;
        state_->counters.hostCeilingBytes = bytes;
        incrementSaturating(state_->counters.admissionRevision);
    }
    state_->changed.notify_all();
}
void GpuProcessingPhaseBudget::setDeviceCeiling(const std::optional<std::uint64_t> bytes) noexcept {
    {
        const std::lock_guard lock(state_->mutex);
        if (state_->counters.deviceCeilingBytes == bytes)
            return;
        state_->counters.deviceCeilingBytes = bytes;
        incrementSaturating(state_->counters.admissionRevision);
    }
    state_->changed.notify_all();
}
void GpuProcessingPhaseBudget::setSampledHostCapacity(
    const std::optional<std::uint64_t> bytes, const bool ready,
    const std::chrono::steady_clock::time_point validUntil, const std::uint64_t creditBytes,
    const std::uint64_t retiredBytesAtSample) noexcept {
    {
        const std::lock_guard lock(state_->mutex);
        const auto beforeBytes = state_->counters.hostCapacityBytes;
        const bool beforeReady = state_->counters.hostReady;
        const auto& backing = state_->rootedHostBacking;
        const auto retired =
            backing.retiredBytesSaturated || backing.retiredMaterializedBytes < retiredBytesAtSample
                ? creditBytes
                : std::min(creditBytes, backing.retiredMaterializedBytes - retiredBytesAtSample);
        state_->hostBackingCreditRemaining = creditBytes - retired;
        if (bytes) {
            state_->counters.hostCapacityBytes = *bytes > retired ? *bytes - retired : 0;
        }
        state_->counters.hostReady = ready && bytes.has_value();
        state_->hostValidUntil = validUntil;
        if (beforeBytes != state_->counters.hostCapacityBytes ||
            beforeReady != state_->counters.hostReady)
            incrementSaturating(state_->counters.admissionRevision);
    }
    state_->changed.notify_all();
}
void GpuProcessingPhaseBudget::tightenHostCapacity(
    const std::optional<std::uint64_t> upperBound, const bool closeReadiness,
    const std::chrono::steady_clock::time_point validUntil) noexcept {
    const std::lock_guard lock(state_->mutex);
    const auto beforeBytes = state_->counters.hostCapacityBytes;
    const bool beforeReady = state_->counters.hostReady;
    if (upperBound && state_->counters.hostCapacityBytes)
        state_->counters.hostCapacityBytes =
            std::min(*upperBound, *state_->counters.hostCapacityBytes);
    if (closeReadiness) {
        state_->counters.hostReady = false;
        state_->hostValidUntil = validUntil;
    }
    if (beforeBytes != state_->counters.hostCapacityBytes ||
        beforeReady != state_->counters.hostReady)
        incrementSaturating(state_->counters.admissionRevision);
}
void GpuProcessingPhaseBudget::setDeviceCapacity(const std::optional<std::uint64_t> bytes,
                                                 const bool ready) noexcept {
    {
        const std::lock_guard lock(state_->mutex);
        const auto beforeBytes = state_->counters.deviceCapacityBytes;
        const bool beforeReady = state_->counters.deviceReady;
        if (bytes) {
            state_->counters.deviceCapacityBytes = bytes;
        }
        state_->counters.deviceReady = ready && bytes.has_value();
        if (beforeBytes != state_->counters.deviceCapacityBytes ||
            beforeReady != state_->counters.deviceReady)
            incrementSaturating(state_->counters.admissionRevision);
    }
    state_->changed.notify_all();
}
void GpuProcessingPhaseBudget::shiftHostCapacity(const std::uint64_t add,
                                                 const std::uint64_t subtract) noexcept {
    {
        const std::lock_guard lock(state_->mutex);
        auto& capacity = state_->counters.hostCapacityBytes;
        if (!capacity || add == subtract)
            return;
        const auto raised = *capacity > std::numeric_limits<std::uint64_t>::max() - add
                                ? std::numeric_limits<std::uint64_t>::max()
                                : *capacity + add;
        capacity = raised > subtract ? raised - subtract : 0;
        incrementSaturating(state_->counters.admissionRevision);
    }
    state_->changed.notify_all();
}
bool GpuProcessingPhaseBudget::waitForRevisionChange(
    const std::uint64_t seen, const std::chrono::steady_clock::time_point deadline) const {
    std::unique_lock lock(state_->mutex);
    for (;;) {
        if (state_->counters.admissionRevision != seen)
            return true;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        state_->changed.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(20)));
    }
}
GpuProcessingPhaseSnapshot GpuProcessingPhaseBudget::snapshot() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->counters;
}
RootedHostBackingSnapshot GpuProcessingPhaseBudget::rootedHostBackingSnapshot() const noexcept {
    const std::lock_guard lock(state_->mutex);
    return state_->rootedHostBacking;
}

std::uint64_t
GpuProcessingPhaseBudget::hostNeedBytes(const GpuProcessingPhaseRequest& request) const noexcept {
    const std::lock_guard lock(state_->mutex);
    const auto source =
        request.sourceFree ? 0
                           : detail::measuredNeed(state_->measuredSourceRetention,
                                                  envelopeKey(request), request.minimumSourceBytes);
    const auto max = std::numeric_limits<std::uint64_t>::max();
    const auto scratch = request.sourceFree ? 0 : sourceScratchFor(request, source);
    if (scratch == max || source > max - scratch || source + scratch > max - request.fixedHostBytes)
        return max;
    return request.fixedHostBytes + source + scratch;
}

GpuProcessingPhaseGrant
GpuProcessingPhaseBudget::reserve(const GpuProcessingPhaseRequest& request) noexcept {
    if (request.processingHostScratchBytes > request.fixedHostBytes ||
        request.minimumSourceBytes > request.requestedSourceBytes ||
        request.minimumExecutorBytes > request.requestedExecutorBytes ||
        (request.sourceFree && request.minimumSourceBytes != 0) ||
        (!request.native &&
         (request.deviceWorkingBytes != 0 || request.requestedExecutorBytes != 0 ||
          request.minimumExecutorBytes != 0))) {
        return {};
    }
    std::shared_ptr<GpuProcessingPhaseLease> lease;
    try {
        // Allocation precedes the lock. Failure cannot consume either half of the paired grant.
        lease = std::shared_ptr<GpuProcessingPhaseLease>(new GpuProcessingPhaseLease(state_));
    } catch (...) {
        return {
            .status = GpuProcessingPhaseAdmission::AllocationFailure, .domain = {}, .lease = {}};
    }
    const std::lock_guard lock(state_->mutex);
    auto& counters = state_->counters;
    const auto refused = [&](const GpuProcessingPhaseDomain domain) {
        incrementSaturating(counters.budgetRefusals);
        return GpuProcessingPhaseGrant{.status = GpuProcessingPhaseAdmission::BudgetRefused,
                                       .domain = domain,
                                       .lease = {},
                                       .admissionRevision = counters.admissionRevision};
    };
    // Free capacity measured at zero is exhaustion only when nothing more could ever be given;
    // memory other work holds (live allocations of other processes in the same limit) can return.
    if (counters.hostReady && counters.hostCapacityBytes && *counters.hostCapacityBytes == 0 &&
        counters.hostCeilingBytes.value_or(0) == 0) {
        return refused(GpuProcessingPhaseDomain::Host);
    }
    // The same for the device (#539): another program's VRAM can come back, so a measured zero
    // within a non-zero ceiling waits below.
    if (request.native && counters.deviceReady && counters.deviceCapacityBytes &&
        *counters.deviceCapacityBytes == 0 && counters.deviceCeilingBytes.value_or(0) == 0) {
        return refused(GpuProcessingPhaseDomain::Device);
    }
    if (!counters.hostReady || !counters.hostCapacityBytes) {
        return {.status = GpuProcessingPhaseAdmission::AwaitingCapacity,
                .domain = GpuProcessingPhaseDomain::Host,
                .lease = {},
                .admissionRevision = counters.admissionRevision};
    }
    if (request.native && (!counters.deviceReady || !counters.deviceCapacityBytes)) {
        return {.status = GpuProcessingPhaseAdmission::AwaitingCapacity,
                .domain = GpuProcessingPhaseDomain::Device,
                .lease = {},
                .admissionRevision = counters.admissionRevision};
    }
    // A need that cannot fit even an otherwise empty root is true exhaustion. A need that fits the
    // capacity but not what other admitted work holds right now waits for a release.
    const auto waiting = [&](const GpuProcessingPhaseDomain domain) {
        return GpuProcessingPhaseGrant{.status = GpuProcessingPhaseAdmission::AwaitingCapacity,
                                       .domain = domain,
                                       .lease = {},
                                       .admissionRevision = counters.admissionRevision};
    };
    if (counters.admittedPhases >= counters.requestCapacity) {
        return waiting(GpuProcessingPhaseDomain::RequestCount);
    }
    if (state_->nextId == 0) {
        return {.status = GpuProcessingPhaseAdmission::SerialExhausted, .domain = {}, .lease = {}};
    }
    // Size the source need from what finished phases of this envelope actually retained (#472):
    // the stated need sums every reachable source reference as a float frame, while preparation
    // decodes each distinct source once.
    const std::uint64_t sourceNeed =
        request.sourceFree
            ? 0
            : detail::measuredNeedLocked(state_->measuredSourceRetention, envelopeKey(request),
                                         request.minimumSourceBytes);
    const auto max = std::numeric_limits<std::uint64_t>::max();
    // The sources it retains plus one source's decode/conversion scratch (#472).
    const std::uint64_t sourceScratch = request.sourceFree ? 0 : sourceScratchFor(request, sourceNeed);
    if (sourceScratch == max || sourceNeed > max - sourceScratch ||
        sourceNeed + sourceScratch > max - request.fixedHostBytes) {
        return refused(GpuProcessingPhaseDomain::Host);
    }
    const std::uint64_t hostNeed = request.fixedHostBytes + sourceNeed + sourceScratch;
    const std::uint64_t hostLimit =
        std::max(*counters.hostCapacityBytes, counters.hostCeilingBytes.value_or(0));
    if (hostNeed > hostLimit) {
        return refused(GpuProcessingPhaseDomain::Host);
    }
    const std::uint64_t hostRemaining =
        remaining(*counters.hostCapacityBytes, counters.totalHostClaimBytes);
    if (hostNeed > hostRemaining) {
        return waiting(GpuProcessingPhaseDomain::Host);
    }
    // The source ceiling is the room left after this phase's own claim; it is not claimed.
    const std::uint64_t sourceCeiling =
        request.sourceFree ? 0
                           : std::max(sourceNeed, std::min(request.requestedSourceBytes,
                                                           hostRemaining - hostNeed + sourceNeed));
    // A phase that asks for sources needs at least one byte of room for them.
    if (!request.sourceFree && request.requestedSourceBytes != 0 && sourceCeiling == 0) {
        return hostNeed >= hostLimit ? refused(GpuProcessingPhaseDomain::Host)
                                     : waiting(GpuProcessingPhaseDomain::Host);
    }
    std::uint64_t executorBytes = 0;
    std::uint64_t deviceNeed = 0;
    if (request.native) {
        if (request.deviceWorkingBytes > max - request.minimumExecutorBytes) {
            return refused(GpuProcessingPhaseDomain::Device);
        }
        deviceNeed = request.deviceWorkingBytes + request.minimumExecutorBytes;
        const std::uint64_t deviceLimit =
            std::max(*counters.deviceCapacityBytes, counters.deviceCeilingBytes.value_or(0));
        if (deviceNeed > deviceLimit) {
            return refused(GpuProcessingPhaseDomain::Device);
        }
        // Size the need from what phases of this envelope actually allocated (#417): the stated
        // need covers the working allowance's ceiling, which a phase often does not use.
        const std::uint64_t statedDeviceNeed = deviceNeed;
        deviceNeed = detail::measuredNeedLocked(state_->measuredDevicePeaks, envelopeKey(request),
                                                deviceNeed);
        const std::uint64_t deviceRemaining =
            remaining(*counters.deviceCapacityBytes, counters.totalDeviceClaimBytes);
        if (deviceNeed > deviceRemaining) {
            return waiting(GpuProcessingPhaseDomain::Device);
        }
        // The requested executor size is a ceiling: the phase may allocate up to it while live
        // capacity allows, but claims only its stated minimum (its live-image peak) up front.
        // Published retention does not cut that ceiling (#544): it gives way inside a native
        // allocation the root refuses for room, so a device full of reusable cache must not
        // squeeze a final render onto the CPU or a preview frame into a capacity wait.
        const std::uint64_t executorRoom =
            remaining(*counters.deviceCapacityBytes,
                      counters.totalDeviceClaimBytes - std::min(counters.retainedNativeImageBytes,
                                                                counters.totalDeviceClaimBytes));
        executorBytes = std::max(request.minimumExecutorBytes,
                                 std::min(request.requestedExecutorBytes,
                                          remaining(executorRoom, request.deviceWorkingBytes)));
        // Never below a measured need (#417): the most native storage phases of this envelope
        // held, plus a quarter, which fits the remaining room. Retained device caches are claims
        // that give way on demand (#425), so a cache that fills the device must not squeeze the
        // executor below what it is known to use. A stated need keeps the working allowance apart.
        if (deviceNeed < statedDeviceNeed)
            executorBytes =
                std::max(executorBytes, std::min(request.requestedExecutorBytes, deviceNeed));
        if (request.requestedExecutorBytes != 0 && executorBytes == 0) {
            return deviceNeed >= deviceLimit
                       ? refused(GpuProcessingPhaseDomain::Device)
                       : waiting(GpuProcessingPhaseDomain::Device);
        }
    }
    // Division/subtraction bounds above prove these additions cannot overflow capacity.
    lease->hostBytes_ = hostNeed;
    lease->remainingHostBytes_ = lease->hostBytes_;
    lease->remainingOutputHostBytes_ = request.fixedHostBytes - request.processingHostScratchBytes;
    lease->remainingProcessingHostScratchBytes_ = request.processingHostScratchBytes;
    lease->remainingUploadBytes_ = sourceCeiling;
    lease->deviceBytes_ = request.native ? request.deviceWorkingBytes + executorBytes : 0;
    lease->remainingDeviceBytes_ = lease->deviceBytes_;
    // The device need is checked here but claimed only when the phase's device stage begins (its
    // first native allocation; see drawDeviceLocked): phases preparing on the CPU, waiting for
    // the device or reading back hold no device claim, because they hold no device memory.
    lease->deviceClaimBytes_ = 0;
    lease->deviceNeedBytes_ = deviceNeed;
    lease->deviceClaimCapBytes_ = deviceNeed;
    lease->envelopeWorkingBytes_ = request.deviceWorkingBytes;
    lease->envelopeMinimumExecutorBytes_ = request.minimumExecutorBytes;
    lease->envelopeFixedHostBytes_ = request.fixedHostBytes;
    lease->envelopeMinimumSourceBytes_ = request.minimumSourceBytes;
    lease->sourceBytes_ = sourceNeed;
    lease->sourceScratchBytes_ = sourceScratch;
    lease->sourceRoomShort_ =
        !request.sourceFree &&
        sourceCeiling < std::min(request.requestedSourceBytes, hostLimit - hostNeed + sourceNeed);
    lease->sourceCeilingBytes_ = sourceCeiling;
    lease->executorBytes_ = executorBytes;
    lease->requestedExecutorBytes_ = request.native ? request.requestedExecutorBytes : 0;
    lease->deviceCapacityAtAdmission_ = counters.deviceCapacityBytes.value_or(0);
    lease->id_ = state_->nextId;
    lease->admissionRevision_ = counters.admissionRevision;
    state_->nextId =
        state_->nextId == std::numeric_limits<std::uint64_t>::max() ? 0 : state_->nextId + 1;
    counters.hostReservedBytes += lease->hostBytes_;
    counters.totalHostClaimBytes += lease->hostBytes_;
    ++counters.admittedPhases;
    counters.peakHostReservedBytes =
        std::max(counters.peakHostReservedBytes, counters.hostReservedBytes);
    counters.peakDeviceReservedBytes =
        std::max(counters.peakDeviceReservedBytes, counters.deviceReservedBytes);
    detail::notePeaksLocked(counters);
    return {.status = GpuProcessingPhaseAdmission::Accepted,
            .domain = GpuProcessingPhaseDomain::None,
            .lease = std::move(lease),
            .admissionRevision = counters.admissionRevision};
}
} // namespace bloom::render
