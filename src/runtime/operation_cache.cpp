#include "video_source.hpp"
#include <bloom/core/telemetry_counters.hpp>
#include <bloom/media/image.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace bloom::runtime {
namespace {
std::size_t addCharge(const std::size_t left, const std::size_t right) noexcept {
    return left + std::min(right, std::numeric_limits<std::size_t>::max() - left);
}
} // namespace
struct OperationCache::Accounting {
    // One distinct allocation referenced by retained (live) or retiring (pending) entries.
    struct Storage {
        std::size_t bytes = 0;
        std::size_t liveReferences = 0;
        std::size_t pendingReferences = 0;
        OperationCacheEntryKind kind = OperationCacheEntryKind::Operation;
    };
    // What a retiring entry still has charged once its storage references are dropped: returned
    // only after the entry (and with it, possibly, the last reference to that storage) is freed.
    struct Release {
        std::size_t metadataBytes = 0;
        OperationCacheEntryKind kind = OperationCacheEntryKind::Operation;
        std::array<std::size_t, 2> freedByKind{};
    };
    mutable std::mutex mutex;
    std::size_t budget = 0;
    // Allocated host bytes (retained + pending), each storage once.
    std::size_t bytes = 0;
    std::array<std::size_t, 2> kindBytes{};
    // Retained entries by kind, for the Profiler's per-kind rows.
    std::array<std::size_t, 2> kindEntries{};
    std::size_t sharedBytes = 0;
    std::unordered_map<const void*, Storage> storages;
    std::uint64_t decodedMediaGeneration = 0;
    std::size_t pendingBytes = 0;
    // pendingBytes by kind, for the Profiler's per-kind rows.
    std::array<std::size_t, 2> pendingKindBytes{};
    // The Profiler's cumulative counters per kind (#423); relaxed atomics.
    std::array<MemoryPoolCounters, 2> counters{};
    bool closed = false;
    // A complete, fresh process sample has been delivered: rooted source revisions may be kept.
    bool sampleReady = false;
    MemoryBudgetLedger::Clock::time_point validUntil{};
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement;
    std::weak_ptr<detail::VideoSourceContext> videoSources;

    static std::size_t index(const OperationCacheEntryKind kind) noexcept {
        return static_cast<std::size_t>(kind);
    }
    // Retained bytes that storing `entry` adds: its metadata and every storage no retained entry
    // references yet (storage only pending retirement comes back to the retained side).
    [[nodiscard]] std::size_t liveIncrement(const Entry& entry) const {
        auto charge = entry.metadataBytes;
        for (const auto& reference : entry.storage) {
            if (reference.key == nullptr)
                continue;
            const auto found = storages.find(reference.key);
            if (found == storages.end())
                charge = addCharge(charge, reference.bytes);
            else if (found->second.liveReferences == 0)
                charge = addCharge(charge, found->second.bytes);
        }
        return charge;
    }
    void chargeLive(const Entry& entry) {
        ++kindEntries[index(entry.kind)];
        bytes += entry.metadataBytes;
        kindBytes[index(entry.kind)] += entry.metadataBytes;
        for (const auto& reference : entry.storage) {
            if (reference.key == nullptr)
                continue;
            auto [found, inserted] = storages.try_emplace(
                reference.key, Storage{.bytes = reference.bytes, .kind = entry.kind});
            auto& storage = found->second;
            if (inserted) {
                bytes += storage.bytes;
                kindBytes[index(storage.kind)] += storage.bytes;
            } else {
                sharedBytes += storage.bytes;
                if (storage.liveReferences == 0) {
                    pendingBytes -= storage.bytes; // Back from pending retirement.
                    pendingKindBytes[index(storage.kind)] -= storage.bytes;
                }
            }
            ++storage.liveReferences;
        }
    }
    void moveToPending(const Entry& entry) {
        --kindEntries[index(entry.kind)];
        pendingBytes += entry.metadataBytes;
        pendingKindBytes[index(entry.kind)] += entry.metadataBytes;
        for (const auto& reference : entry.storage) {
            if (reference.key == nullptr)
                continue;
            auto& storage = storages.at(reference.key);
            --storage.liveReferences;
            ++storage.pendingReferences;
            if (storage.liveReferences == 0) {
                pendingBytes += storage.bytes;
                pendingKindBytes[index(storage.kind)] += storage.bytes;
            }
        }
    }
    // A synchronous removal: the entry is freed under the same lock.
    void releaseLive(const Entry& entry) {
        --kindEntries[index(entry.kind)];
        bytes -= entry.metadataBytes;
        kindBytes[index(entry.kind)] -= entry.metadataBytes;
        for (const auto& reference : entry.storage) {
            if (reference.key == nullptr)
                continue;
            const auto found = storages.find(reference.key);
            auto& storage = found->second;
            --storage.liveReferences;
            if (storage.liveReferences + storage.pendingReferences != 0) {
                sharedBytes -= storage.bytes;
                if (storage.liveReferences == 0) {
                    pendingBytes += storage.bytes;
                    pendingKindBytes[index(storage.kind)] += storage.bytes;
                }
                continue;
            }
            bytes -= storage.bytes;
            kindBytes[index(storage.kind)] -= storage.bytes;
            storages.erase(found);
        }
    }
    // A retiring entry about to be freed. A storage whose last reference this is leaves the map
    // now, while the entry still holds it (so its address cannot be reused meanwhile); its bytes
    // stay charged until finishRelease, after the entry is freed.
    [[nodiscard]] Release beginRelease(const Entry& entry) {
        Release release{.metadataBytes = entry.metadataBytes, .kind = entry.kind};
        for (const auto& reference : entry.storage) {
            if (reference.key == nullptr)
                continue;
            const auto found = storages.find(reference.key);
            auto& storage = found->second;
            --storage.pendingReferences;
            if (storage.liveReferences + storage.pendingReferences != 0) {
                sharedBytes -= storage.bytes; // Another entry still references it.
                continue;
            }
            release.freedByKind[index(storage.kind)] += storage.bytes;
            storages.erase(found);
        }
        return release;
    }
    void finishRelease(const Release& release) {
        bytes -= release.metadataBytes;
        pendingBytes -= release.metadataBytes;
        pendingKindBytes[index(release.kind)] -= release.metadataBytes;
        kindBytes[index(release.kind)] -= release.metadataBytes;
        for (std::size_t kind = 0; kind < release.freedByKind.size(); ++kind) {
            bytes -= release.freedByKind[kind];
            pendingBytes -= release.freedByKind[kind];
            pendingKindBytes[kind] -= release.freedByKind[kind];
            kindBytes[kind] -= release.freedByKind[kind];
        }
    }
};
std::size_t OperationCache::Entry::standaloneBytes() const noexcept {
    auto total = metadataBytes;
    for (const auto& reference : storage)
        total = addCharge(total, reference.bytes);
    return total;
}
OperationCache::Entry::~Entry() {
    // Rejected insertion also drops refs before its registration can permit owner exit.
    value.image.reset();
    value.sourceRevision.reset();
    value.samples.reset();
    value.channels.reset();
    value.values.clear();
    if (registered)
        render::detail::releasePackedHostRegistration(retirement);
}
OperationCache::OperationCache(const std::size_t budget, MemoryBudgetLedger& ledger)
    : ledger_(ledger), accounting_(std::make_shared<Accounting>()) {
    accounting_->budget = budget;
    const auto apply = [this](const std::size_t bytes) {
        // The ledger serializes callbacks. Read it before taking the cache mutex, never inverse.
        const auto delivered = ledger_.state();
        const bool pressure = delivered.retentionPercent < 100;
        const std::lock_guard lock(accounting_->mutex);
        accounting_->budget = bytes;
        accounting_->sampleReady =
            !delivered.awaitingProcessSample &&
            (delivered.sampleSerial == 0 ||
             (delivered.processSampleFresh &&
              (isCompleteProcessMemorySample(delivered.process) ||
               delivered.process.status == ProcessMemoryStatus::Unsupported)));
        accounting_->validUntil =
            delivered.sampleSerial == 0
                ? MemoryBudgetLedger::Clock::time_point::max()
                : (delivered.process.sampledAt >
                           MemoryBudgetLedger::Clock::time_point::max() - std::chrono::seconds(15)
                       ? MemoryBudgetLedger::Clock::time_point::max()
                       : delivered.process.sampledAt + std::chrono::seconds(15));
        std::uint64_t ordinaryDrops = 0;
        evictToLocked(bytes, pressure ? statistics_.pressureDrops : ordinaryDrops);
    };
    apply(budget);
    ledger_.registerCache(
        this, budget,
        [this] {
            const std::lock_guard lock(accounting_->mutex);
            return MemoryPoolHolding{.hostBytes = accounting_->bytes - accounting_->pendingBytes,
                                     .hostPendingRetirementBytes = accounting_->pendingBytes};
        },
        apply);
    sourceWindow_->addTrimmer(this, [this] { releaseSourcesOutsideWindow(); });
    // Two Profiler rows, one per entry kind, sharing the cache's one ceiling.
    ledger_.setPoolProfile(
        this, {.name = {},
               .kind = ProfilerPoolKind::Cache,
               .counters = nullptr,
               .rows =
                   [accounting = accounting_](const std::span<ProfilerPool> out) {
                       static constexpr std::array<std::string_view, 2> names{
                           "Operation cache: operations", "Operation cache: decoded media"};
                       const std::lock_guard lock(accounting->mutex);
                       const auto count = std::min(out.size(), names.size());
                       for (std::size_t kind = 0; kind < count; ++kind) {
                           auto& row = out[kind];
                           row = ProfilerPool{};
                           setProfilerName(row.name, names[kind]);
                           row.kind = ProfilerPoolKind::Cache;
                           row.pendingBytes = accounting->pendingKindBytes[kind];
                           row.hostBytes = accounting->kindBytes[kind] - row.pendingBytes;
                           row.ceilingBytes = ProfilerBytes::of(accounting->budget);
                           row.entries = ProfilerCount::of(accounting->kindEntries[kind]);
                           accounting->counters[kind].copyTo(row);
                       }
                       return count;
                   },
               .entries = {}});
}
OperationCache::~OperationCache() {
    sourceWindow_->removeTrimmer(this);
    close();
    ledger_.unregisterCache(this);
    // Rooted refs never free on this caller. The explicit host owner outlives queued callbacks.
    const std::lock_guard lock(accounting_->mutex);
    removeKindLocked(OperationCacheEntryKind::Operation);
    removeKindLocked(OperationCacheEntryKind::DecodedMedia);
}
std::optional<OperationCacheValue> OperationCache::find(const std::string& content,
                                                        const document::Revision revision,
                                                        const OperationCacheEntryKind kind) {
    const std::lock_guard lock(accounting_->mutex);
    auto& missed = accounting_->counters[Accounting::index(kind)];
    const Key key{content};
    const auto sourceUsable = [&](const Position position) {
        const auto& entry = **position;
        if (entry.value.sourceRevision && entry.registered &&
            (accounting_->closed || !accounting_->sampleReady ||
             MemoryBudgetLedger::Clock::now() > accounting_->validUntil)) {
            removeLocked(position);
            ++statistics_.misses;
            missed.miss();
            return false;
        }
        return true;
    };
    const auto exact = addresses_.find({revision, key});
    if (exact != addresses_.end()) {
        if (!sourceUsable(exact->second))
            return std::nullopt;
        touch(exact->second);
        retag(**exact->second);
        ++statistics_.hits;
        accounting_->counters[Accounting::index((*exact->second)->kind)].hit();
        return (*exact->second)->value;
    }
    const auto found = index_.find(key);
    if (found == index_.end()) {
        ++statistics_.misses;
        missed.miss();
        return std::nullopt;
    }
    if (!sourceUsable(found->second))
        return std::nullopt;
    auto& entry = **found->second;
    addresses_.emplace(Address{revision, {entry.content}}, found->second);
    addresses_.erase({entry.revision, key});
    entry.revision = revision;
    touch(found->second);
    retag(entry);
    ++statistics_.hits;
    accounting_->counters[Accounting::index(entry.kind)].hit();
    return entry.value;
}
bool LinearPassRetention::offer(const std::string_view key) {
    const auto hash = std::hash<std::string_view>{}(key);
    const std::lock_guard lock(mutex_);
    if (offered_.contains(hash))
        return true;
    try {
        if (order_.size() >= kRememberedKeys) {
            offered_.erase(order_.front());
            order_.pop_front();
        }
        offered_.insert(hash);
        order_.push_back(hash);
    } catch (const std::bad_alloc&) {
        // Remembering is best effort; this offer is still deferred.
    }
    return false;
}

void OperationCache::insertLocked(std::unique_ptr<Entry>& entry) {
    try {
        entries_.push_front(std::move(entry));
    } catch (const std::bad_alloc&) {
        ++statistics_.allocationFailures;
        accounting_->counters[Accounting::index(entry->kind)].refused();
        return;
    }
    bool indexed = false;
    try {
        auto& placed = *entries_.front();
        const Key key{placed.content};
        const auto [indexPosition, indexInserted] = index_.emplace(key, entries_.begin());
        indexed = indexInserted;
        if (!indexInserted) {
            entry = std::move(entries_.front());
            entries_.pop_front();
            return;
        }
        const bool addressInserted =
            addresses_.emplace(Address{placed.revision, key}, entries_.begin()).second;
        if (!addressInserted) {
            index_.erase(indexPosition);
            entry = std::move(entries_.front());
            entries_.pop_front();
            return;
        }
    } catch (const std::bad_alloc&) {
        if (indexed)
            index_.erase(Key{entries_.front()->content});
        entry = std::move(entries_.front());
        entries_.pop_front();
        ++statistics_.allocationFailures;
        accounting_->counters[Accounting::index(entry->kind)].refused();
        return;
    } catch (...) {
        if (indexed)
            index_.erase(Key{entries_.front()->content});
        entry = std::move(entries_.front());
        entries_.pop_front();
        throw;
    }
    accounting_->chargeLive(*entries_.front());
    accounting_->counters[Accounting::index(entries_.front()->kind)].store();
}
namespace {
// What an entry costs on the host: per-entry metadata, and the storage it references, charged by
// the cache once per distinct allocation at its real allocation size.
std::size_t imageObjectBytes(const render::Rgba32fImage& image) noexcept {
    return addCharge(sizeof(render::Rgba32fImage), image.pixelStorageByteCost());
}
} // namespace
void OperationCache::store(std::string content, const document::Revision revision,
                           OperationCacheValue value, const OperationCacheEntryKind kind,
                           const std::optional<std::uint64_t> decodedMediaGeneration,
                           LinearPassRetention* const linearPass) {
    if (value.sourceRevision && kind != OperationCacheEntryKind::DecodedMedia)
        throw std::invalid_argument("source revisions require decoded-media ownership");
    std::unique_ptr<Entry> entry;
    try {
        entry = std::make_unique<Entry>();
    } catch (const std::bad_alloc&) {
        const std::lock_guard lock(accounting_->mutex);
        ++statistics_.allocationFailures;
        accounting_->counters[Accounting::index(kind)].refused();
        return;
    }
    entry->content = std::move(content);
    entry->revision = revision;
    entry->kind = kind;
    if (kind == OperationCacheEntryKind::DecodedMedia)
        entry->use = SourceWorkingSet::current();
    entry->value = std::move(value);
    entry->metadataBytes =
        addCharge(sizeof(Entry) + sizeof(Address) + sizeof(void*) * 16, entry->content.capacity());
    if (entry->value.image) {
        // Keyed by the pixel allocation: entries that publish the same image share it.
        const auto* pixels = static_cast<const void*>(entry->value.image->pixels().data());
        if (pixels != nullptr)
            entry->storage[0] = {pixels, imageObjectBytes(*entry->value.image)};
        else
            entry->metadataBytes = addCharge(entry->metadataBytes, sizeof(render::Rgba32fImage));
    }
    entry->metadataBytes =
        addCharge(entry->metadataBytes, entry->value.values.capacity() * sizeof(CompiledValue));
    // #589: a retained Path output pays for what it holds. (Other value kinds fit their slot, or,
    // for a string, were already bounded where they were written.)
    for (const auto& held : entry->value.values)
        if (const auto* path = std::get_if<CompiledPathValue>(&held))
            entry->metadataBytes =
                addCharge(entry->metadataBytes,
                          sizeof(document::PathValue) +
                              path->value->contours.size() * sizeof(document::PathContour) +
                              path->value->anchorCount() * sizeof(document::PathAnchor) +
                              path->value->groups.size() * sizeof(document::PathGroup));
    if (entry->value.sourceRevision)
        entry->storage[1] = {entry->value.sourceRevision.get(),
                             entry->value.sourceRevision->retainedBytes()};
    if (entry->value.samples) {
        // Keyed by the sample bytes: entries that publish the same decode share them.
        const auto bytes = addCharge(sizeof(render::NativeSampleImage),
                                     static_cast<std::size_t>(entry->value.samples->byteCount()));
        if (const auto* backing = entry->value.samples->backingIdentity(); backing != nullptr)
            entry->storage[2] = {backing, bytes};
        else
            entry->metadataBytes = addCharge(entry->metadataBytes, bytes);
    }
    if (entry->value.channels)
        // Keyed by the channel set: entries that publish the same decode share it.
        entry->storage[3] = {
            entry->value.channels.get(),
            addCharge(sizeof(render::ChannelSetImage), entry->value.channels->byteCost())};
    for (const auto& item : entry->value.values)
        if (const auto* string = std::get_if<std::string>(&item))
            entry->metadataBytes = addCharge(entry->metadataBytes, string->capacity());
    // Worker-side stores register every payload when this pool has an explicit host owner.
    // Standalone CPU pools preserve their synchronous ownership; rooted image/value and source
    // payloads all use the same admission-independent retirement lane.
    {
        const std::lock_guard lock(accounting_->mutex);
        if (accounting_->closed)
            return;
        entry->retirement = accounting_->retirement;
    }
    if (entry->retirement) {
        if (!render::detail::preparePackedHostRetirement(entry->retirement) ||
            !render::detail::registerPackedHostAllocation(entry->retirement))
            return;
        entry->registered = true;
        entry->accounting = accounting_;
        entry->destroy =
            entry->value.sourceRevision ? destroySourceRevisionEntry : destroyOrdinaryEntry;
    }
    {
        const std::lock_guard lock(accounting_->mutex);
        if (accounting_->closed)
            return;
        if (entry->value.sourceRevision &&
            (!decodedMediaGeneration ||
             *decodedMediaGeneration != accounting_->decodedMediaGeneration ||
             accounting_->decodedMediaGeneration == std::numeric_limits<std::uint64_t>::max() ||
             (entry->registered && (!accounting_->sampleReady ||
                                    MemoryBudgetLedger::Clock::now() > accounting_->validUntil))))
            return;
        auto charge = accounting_->liveIncrement(*entry);
        if (index_.contains({entry->content}))
            return;
        if (charge > accounting_->budget) {
            accounting_->counters[Accounting::index(kind)].refused();
            return;
        }
        // A linear pass retains only what recurs: the first offer of a key is not stored.
        if (linearPass && !linearPass->offer(entry->content)) {
            ++statistics_.linearPassDeferrals;
            return;
        }
        ++accessEpoch_;
        entry->protectedUntil = kind == OperationCacheEntryKind::DecodedMedia
                                    ? accessEpoch_ + kDecodedMediaGraceAccesses
                                    : 0;
        std::uint64_t ignored = 0;
        if (accounting_->pendingBytes == 0 &&
            charge <= std::numeric_limits<std::size_t>::max() - accounting_->bytes) {
            // Preserve ordinary-only grace: the incoming Operation itself can be the victim.
            insertLocked(entry);
            evictToLocked(accounting_->budget, ignored);
            return;
        }
        evictToLocked(accounting_->budget - charge, ignored);
        // Eviction may have left shared storage only pending; storing revives its charge.
        charge = accounting_->liveIncrement(*entry);
        if (charge > accounting_->budget || accounting_->bytes > accounting_->budget - charge) {
            accounting_->counters[Accounting::index(kind)].refused();
            return; // Still-charged pending refs cannot donate capacity.
        }
        insertLocked(entry);
    } // Rejected Entry/image/registration destruction is outside the cache mutex.
}
void OperationCache::destroySourceRevisionEntry(
    render::detail::PackedHostRetirementNode* node) noexcept {
    auto* entry = static_cast<Entry*>(node);
    const auto accounting = entry->accounting;
    Accounting::Release release;
    {
        const std::lock_guard lock(accounting->mutex);
        release = accounting->beginRelease(*entry);
    }
    entry->registered = false; // The generic worker releases registration after this callback.
    delete entry; // Drop encoded backing and metadata before folding back the logical debit.
    const std::lock_guard lock(accounting->mutex);
    accounting->finishRelease(release);
}
void OperationCache::destroyOrdinaryEntry(render::detail::PackedHostRetirementNode* node) noexcept {
    auto* entry = static_cast<Entry*>(node);
    const auto accounting = entry->accounting;
    Accounting::Release release;
    {
        const std::lock_guard lock(accounting->mutex);
        release = accounting->beginRelease(*entry);
    }
    entry->registered = false;
    delete entry;
    const std::lock_guard lock(accounting->mutex);
    accounting->finishRelease(release);
}
void OperationCache::removeLocked(const Position candidate) {
    auto& entry = **candidate;
    const Key key{entry.content};
    addresses_.erase({entry.revision, key});
    index_.erase(key);
    if (entry.registered) {
        accounting_->moveToPending(entry);
        auto* node = candidate->release();
        entries_.erase(candidate);
        const auto retirement = node->retirement;
        render::detail::enqueuePackedHostRetirement(retirement, node);
    } else {
        accounting_->releaseLive(entry);
        entries_.erase(candidate);
    }
}
std::size_t OperationCache::removeKindLocked(const OperationCacheEntryKind kind) {
    std::size_t removed = 0;
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        const auto next = std::next(entry);
        if ((*entry)->kind == kind) {
            removed += (*entry)->standaloneBytes();
            removeLocked(entry);
        }
        entry = next;
    }
    return removed;
}
void OperationCache::evictToLocked(const std::size_t limit, std::uint64_t& counter) {
    while (accounting_->bytes - accounting_->pendingBytes > limit && !entries_.empty()) {
        const auto victim = evictionCandidate();
        // The entry's standalone charge: its metadata and the storage it references.
        accounting_->counters[Accounting::index((*victim)->kind)].evicted(
            (*victim)->standaloneBytes());
        removeLocked(victim);
        ++counter;
    }
}
void OperationCache::retag(Entry& entry) noexcept {
    if (entry.kind != OperationCacheEntryKind::DecodedMedia)
        return;
    if (const auto use = SourceWorkingSet::current(); use.tracked)
        entry.use = use;
}
void OperationCache::releaseSourcesOutsideWindow() {
    const std::lock_guard lock(accounting_->mutex);
    if (accounting_->closed)
        return;
    for (auto position = entries_.begin(); position != entries_.end();) {
        const auto next = std::next(position);
        const auto& entry = **position;
        if (entry.kind == OperationCacheEntryKind::DecodedMedia && entry.use.tracked &&
            !sourceWindow_->retains(entry.use)) {
            accounting_->counters[Accounting::index(entry.kind)].released(entry.standaloneBytes());
            removeLocked(position);
        }
        position = next;
    }
}
void OperationCache::trimToBytes(const std::size_t bytes) {
    const std::lock_guard lock(accounting_->mutex);
    evictToLocked(std::min(bytes, accounting_->budget), statistics_.pressureDrops);
}
void OperationCache::clearOperations() {
    const std::lock_guard lock(accounting_->mutex);
    core::telemetry::addEvictedBytes(core::telemetry::EvictionCause::Purge,
                                     removeKindLocked(OperationCacheEntryKind::Operation));
}
void OperationCache::clearDecodedMedia() {
    std::shared_ptr<detail::VideoSourceContext> video;
    {
        const std::lock_guard lock(accounting_->mutex);
        if (accounting_->decodedMediaGeneration != std::numeric_limits<std::uint64_t>::max())
            ++accounting_->decodedMediaGeneration;
        core::telemetry::addEvictedBytes(core::telemetry::EvictionCause::Purge,
                                         removeKindLocked(OperationCacheEntryKind::DecodedMedia));
        video = accounting_->videoSources.lock();
    }
    if (video)
        video->clear();
}
void OperationCache::invalidateSourceRevisions() {
    std::shared_ptr<detail::VideoSourceContext> video;
    {
        const std::lock_guard lock(accounting_->mutex);
        if (accounting_->decodedMediaGeneration != std::numeric_limits<std::uint64_t>::max())
            ++accounting_->decodedMediaGeneration;
        for (auto entry = entries_.begin(); entry != entries_.end();) {
            const auto next = std::next(entry);
            if ((*entry)->value.sourceRevision)
                removeLocked(entry);
            entry = next;
        }
        video = accounting_->videoSources.lock();
    }
    if (video)
        video->invalidateSources();
}
std::uint64_t OperationCache::decodedMediaGeneration() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->decodedMediaGeneration;
}
void OperationCache::touch(const Position position) {
    ++accessEpoch_;
    (*position)->protectedUntil = (*position)->kind == OperationCacheEntryKind::DecodedMedia
                                      ? accessEpoch_ + kDecodedMediaGraceAccesses
                                      : 0;
    entries_.splice(entries_.begin(), entries_, position);
}
OperationCache::Position OperationCache::evictionCandidate() {
    for (auto candidate = entries_.end(); candidate != entries_.begin();) {
        --candidate;
        if ((*candidate)->kind != OperationCacheEntryKind::DecodedMedia ||
            accessEpoch_ >= (*candidate)->protectedUntil)
            return candidate;
    }
    return std::prev(entries_.end());
}
void OperationCache::setByteBudget(const std::size_t budget) {
    static_cast<void>(ledger_.setCacheCeiling(this, budget));
}
std::size_t OperationCache::byteBudget() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->budget;
}
std::size_t OperationCache::retainedBytes() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->bytes;
}
std::size_t OperationCache::retainedBytes(const OperationCacheEntryKind kind) const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->kindBytes[Accounting::index(kind)];
}
OperationCacheMemory OperationCache::memory() const {
    const std::lock_guard lock(accounting_->mutex);
    const auto& kinds = accounting_->kindBytes;
    return {.hostBytes = accounting_->bytes,
            .pendingRetirementBytes = accounting_->pendingBytes,
            .operationBytes = kinds[Accounting::index(OperationCacheEntryKind::Operation)],
            .decodedMediaBytes = kinds[Accounting::index(OperationCacheEntryKind::DecodedMedia)],
            .sharedReferenceBytes = accounting_->sharedBytes};
}
OperationCacheAccessStatistics OperationCache::statistics() const {
    const std::lock_guard lock(accounting_->mutex);
    return statistics_;
}
void OperationCache::bindHostRetirement(
    const std::shared_ptr<render::PackedHostRetirementOwner>& owner) {
    const auto retirement = owner ? owner->producerState() : nullptr;
    if (!retirement)
        throw std::invalid_argument("host retirement requires an explicit owner");
    const std::lock_guard lock(accounting_->mutex);
    if (accounting_->retirement && accounting_->retirement != retirement)
        throw std::invalid_argument("the pool is already rooted on another host owner");
    accounting_->retirement = retirement;
}
std::shared_ptr<render::detail::PackedHostRetirementState>
OperationCache::sourceRetirementState() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->retirement;
}
void OperationCache::bindVideoSources(const std::shared_ptr<detail::VideoSourceContext>& sources) {
    bool closed = false;
    {
        const std::lock_guard lock(accounting_->mutex);
        accounting_->videoSources = sources;
        closed = accounting_->closed;
    }
    if (closed && sources)
        sources->beginShutdown();
}
std::vector<VideoSourceReport> OperationCache::videoSourceReports() const {
    std::shared_ptr<detail::VideoSourceContext> sources;
    {
        const std::lock_guard lock(accounting_->mutex);
        sources = accounting_->videoSources.lock();
    }
    return sources ? sources->reports() : std::vector<VideoSourceReport>{};
}
void OperationCache::close() {
    std::shared_ptr<detail::VideoSourceContext> video;
    {
        const std::lock_guard lock(accounting_->mutex);
        accounting_->closed = true;
        // Every rooted entry must leave before the shared host owner can acknowledge shutdown.
        // Otherwise an Operation/decoded image could keep its registration forever.
        removeKindLocked(OperationCacheEntryKind::DecodedMedia);
        removeKindLocked(OperationCacheEntryKind::Operation);
        video = accounting_->videoSources.lock();
    }
    if (video)
        video->beginShutdown();
}
} // namespace bloom::runtime
