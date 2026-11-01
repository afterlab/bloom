#include "video_source.hpp"
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
        std::array<std::size_t, 3> freedByKind{};
    };
    mutable std::mutex mutex;
    std::size_t budget = 0;
    // Allocated host bytes (retained + pending), each storage once.
    std::size_t bytes = 0;
    std::array<std::size_t, 3> kindBytes{};
    std::size_t sharedBytes = 0;
    std::unordered_map<const void*, Storage> storages;
    std::uint64_t decodedMediaGeneration = 0;
    std::uint64_t preparedPayload = 0;
    std::size_t preparedCount = 0;
    std::size_t pendingCount = 0;
    std::size_t pendingCandidates = 0;
    std::size_t pendingBytes = 0;
    std::uint64_t pendingPayload = 0;
    bool preparedBound = false;
    bool preparedClosed = false;
    bool preparedReady = false;
    MemoryBudgetLedger::Clock::time_point validUntil{};
    std::optional<std::uint64_t> payloadCap;
    std::size_t entryCap = std::numeric_limits<std::size_t>::max();
    std::optional<std::size_t> configuredEntryCap;
    OperationCacheAccessStatistics preparedStatistics;
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
                if (storage.liveReferences == 0)
                    pendingBytes -= storage.bytes; // Back from pending retirement.
            }
            ++storage.liveReferences;
        }
    }
    void moveToPending(const Entry& entry) {
        pendingBytes += entry.metadataBytes;
        for (const auto& reference : entry.storage) {
            if (reference.key == nullptr)
                continue;
            auto& storage = storages.at(reference.key);
            --storage.liveReferences;
            ++storage.pendingReferences;
            if (storage.liveReferences == 0)
                pendingBytes += storage.bytes;
        }
    }
    // A synchronous removal: the entry is freed under the same lock.
    void releaseLive(const Entry& entry) {
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
                if (storage.liveReferences == 0)
                    pendingBytes += storage.bytes;
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
        kindBytes[index(release.kind)] -= release.metadataBytes;
        for (std::size_t kind = 0; kind < release.freedByKind.size(); ++kind) {
            bytes -= release.freedByKind[kind];
            pendingBytes -= release.freedByKind[kind];
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
        accounting_->preparedReady =
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
        if (pressure || (preparedCandidate_ && preparedCandidate_->standaloneBytes() > bytes))
            retireCandidateLocked();
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
}
OperationCache::~OperationCache() {
    closePreparedUploads();
    ledger_.unregisterCache(this);
    // Rooted refs never free on this caller. The explicit host owner outlives queued callbacks.
    const std::lock_guard lock(accounting_->mutex);
    retireCandidateLocked();
    removeKindLocked(OperationCacheEntryKind::Operation);
    removeKindLocked(OperationCacheEntryKind::PreparedUpload);
    removeKindLocked(OperationCacheEntryKind::DecodedMedia);
}
std::optional<OperationCacheValue> OperationCache::find(const std::string& content,
                                                        const document::Revision revision) {
    const std::lock_guard lock(accounting_->mutex);
    const Key key{content, false};
    const auto sourceUsable = [&](const Position position) {
        const auto& entry = **position;
        if (entry.value.sourceRevision && entry.registered &&
            (accounting_->preparedClosed || !accounting_->preparedReady ||
             MemoryBudgetLedger::Clock::now() > accounting_->validUntil)) {
            removeLocked(position);
            ++statistics_.misses;
            return false;
        }
        return true;
    };
    const auto exact = addresses_.find({revision, key});
    if (exact != addresses_.end()) {
        if (!sourceUsable(exact->second))
            return std::nullopt;
        touch(exact->second);
        ++statistics_.hits;
        return (*exact->second)->value;
    }
    const auto found = index_.find(key);
    if (found == index_.end()) {
        ++statistics_.misses;
        return std::nullopt;
    }
    if (!sourceUsable(found->second))
        return std::nullopt;
    auto& entry = **found->second;
    addresses_.emplace(Address{revision, {entry.content, false}}, found->second);
    addresses_.erase({entry.revision, key});
    entry.revision = revision;
    touch(found->second);
    ++statistics_.hits;
    return entry.value;
}
bool LinearPassRetention::offer(const std::string_view key, const bool prepared) {
    const auto hash = std::hash<std::string_view>{}(key) ^ std::hash<bool>{}(prepared);
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

void OperationCache::insertLocked(std::unique_ptr<Entry>& entry, const bool charge) {
    const bool prepared = entry->kind == OperationCacheEntryKind::PreparedUpload;
    try {
        entries_.push_front(std::move(entry));
    } catch (const std::bad_alloc&) {
        ++statistics_.allocationFailures;
        return;
    }
    bool indexed = false;
    try {
        auto& placed = *entries_.front();
        const Key key{placed.content, prepared};
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
            index_.erase(Key{entries_.front()->content, prepared});
        entry = std::move(entries_.front());
        entries_.pop_front();
        ++statistics_.allocationFailures;
        return;
    } catch (...) {
        if (indexed)
            index_.erase(Key{entries_.front()->content, prepared});
        entry = std::move(entries_.front());
        entries_.pop_front();
        throw;
    }
    if (!charge)
        return;
    auto& placed = *entries_.front();
    accounting_->chargeLive(placed);
    if (prepared) {
        accounting_->preparedPayload += placed.value.image->pixels().size_bytes();
        ++accounting_->preparedCount;
    }
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
    // Prepared admission needs its explicit owner and subset policy, not this ordinary API.
    if (kind == OperationCacheEntryKind::PreparedUpload)
        throw std::invalid_argument("prepared entries require storePreparedUpload");
    if (value.sourceRevision && kind != OperationCacheEntryKind::DecodedMedia)
        throw std::invalid_argument("source revisions require decoded-media ownership");
    std::unique_ptr<Entry> entry;
    try {
        entry = std::make_unique<Entry>();
    } catch (const std::bad_alloc&) {
        const std::lock_guard lock(accounting_->mutex);
        ++statistics_.allocationFailures;
        return;
    }
    entry->content = std::move(content);
    entry->revision = revision;
    entry->kind = kind;
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
    if (entry->value.sourceRevision)
        entry->storage[1] = {entry->value.sourceRevision.get(),
                             entry->value.sourceRevision->retainedBytes()};
    for (const auto& item : entry->value.values)
        if (const auto* string = std::get_if<std::string>(&item))
            entry->metadataBytes = addCharge(entry->metadataBytes, string->capacity());
    // Worker-side stores register every payload when this pool has an explicit host owner.
    // Standalone CPU pools preserve their synchronous ownership; rooted image/value and source
    // payloads all use the same admission-independent retirement lane.
    {
        const std::lock_guard lock(accounting_->mutex);
        if (accounting_->preparedClosed)
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
        if (accounting_->preparedClosed)
            return;
        if (entry->value.sourceRevision &&
            (!decodedMediaGeneration ||
             *decodedMediaGeneration != accounting_->decodedMediaGeneration ||
             accounting_->decodedMediaGeneration == std::numeric_limits<std::uint64_t>::max() ||
             (entry->registered && (!accounting_->preparedReady ||
                                    MemoryBudgetLedger::Clock::now() > accounting_->validUntil))))
            return;
        auto charge = accounting_->liveIncrement(*entry);
        if (charge > accounting_->budget || index_.contains({entry->content, false}))
            return;
        // A linear pass retains only what recurs: the first offer of a key is not stored.
        if (linearPass && !linearPass->offer(entry->content, false)) {
            ++statistics_.linearPassDeferrals;
            return;
        }
        ++accessEpoch_;
        entry->protectedUntil = kind == OperationCacheEntryKind::DecodedMedia
                                    ? accessEpoch_ + kDecodedMediaGraceAccesses
                                    : 0;
        std::uint64_t ignored = 0;
        if (accounting_->preparedCount == 0 && accounting_->pendingBytes == 0 &&
            charge <= std::numeric_limits<std::size_t>::max() - accounting_->bytes) {
            // Preserve ordinary-only grace: the incoming Operation itself can be the victim.
            insertLocked(entry);
            evictToLocked(accounting_->budget, ignored);
            return;
        }
        evictToLocked(accounting_->budget - charge, ignored);
        // Eviction may have left shared storage only pending; storing revives its charge.
        charge = accounting_->liveIncrement(*entry);
        if (charge > accounting_->budget || accounting_->bytes > accounting_->budget - charge)
            return; // Still-charged pending prepared refs cannot donate capacity.
        insertLocked(entry);
    } // Rejected Entry/image/registration destruction is outside the cache mutex.
}
void OperationCache::destroyPreparedEntry(render::detail::PackedHostRetirementNode* node) noexcept {
    auto* entry = static_cast<Entry*>(node);
    const auto accounting = entry->accounting;
    const auto payload = entry->value.image->pixels().size_bytes();
    const bool retiredCandidate = entry->retiredCandidate;
    Accounting::Release release;
    {
        const std::lock_guard lock(accounting->mutex);
        release = accounting->beginRelease(*entry);
    }
    entry->registered = false; // The generic worker releases its registration after this callback.
    delete entry; // Cache reference and metadata free before returning their logical retention
                  // charge.
    const std::lock_guard lock(accounting->mutex);
    accounting->finishRelease(release);
    accounting->preparedPayload -= payload;
    --accounting->preparedCount;
    --accounting->pendingCount;
    accounting->pendingPayload -= payload;
    if (retiredCandidate)
        --accounting->pendingCandidates;
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
    const bool prepared = entry.kind == OperationCacheEntryKind::PreparedUpload;
    const Key key{entry.content, prepared};
    addresses_.erase({entry.revision, key});
    index_.erase(key);
    if (prepared) {
        ++accounting_->pendingCount;
        accounting_->moveToPending(entry);
        accounting_->pendingPayload += entry.value.image->pixels().size_bytes();
        auto* node = candidate->release();
        entries_.erase(candidate);
        const auto retirement = node->retirement;
        render::detail::enqueuePackedHostRetirement(retirement, node);
    } else if (entry.registered) {
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
void OperationCache::removeKindLocked(const OperationCacheEntryKind kind) {
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        const auto next = std::next(entry);
        if ((*entry)->kind == kind)
            removeLocked(entry);
        entry = next;
    }
}
void OperationCache::evictToLocked(const std::size_t limit, std::uint64_t& counter) {
    while (accounting_->bytes - accounting_->pendingBytes > limit && !entries_.empty()) {
        removeLocked(evictionCandidate());
        ++counter;
    }
}
void OperationCache::trimToBytes(const std::size_t bytes) {
    const std::lock_guard lock(accounting_->mutex);
    retireCandidateLocked();
    evictToLocked(std::min(bytes, accounting_->budget), statistics_.pressureDrops);
}
void OperationCache::clearOperations() {
    const std::lock_guard lock(accounting_->mutex);
    removeKindLocked(OperationCacheEntryKind::Operation);
}
void OperationCache::clearDecodedMedia() {
    std::shared_ptr<detail::VideoSourceContext> video;
    {
        const std::lock_guard lock(accounting_->mutex);
        if (accounting_->decodedMediaGeneration != std::numeric_limits<std::uint64_t>::max())
            ++accounting_->decodedMediaGeneration;
        removeKindLocked(OperationCacheEntryKind::DecodedMedia);
        removeKindLocked(OperationCacheEntryKind::PreparedUpload);
        retireCandidateLocked();
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
            .preparedUploadBytes =
                kinds[Accounting::index(OperationCacheEntryKind::PreparedUpload)],
            .sharedReferenceBytes = accounting_->sharedBytes};
}
OperationCacheAccessStatistics OperationCache::statistics() const {
    const std::lock_guard lock(accounting_->mutex);
    return statistics_;
}
void OperationCache::bindPreparedUploads(
    const std::shared_ptr<render::PackedHostRetirementOwner>& owner,
    const std::optional<std::uint64_t> payloadCap, const std::optional<std::size_t> entryCap) {
    const auto retirement = owner ? owner->producerState() : nullptr;
    const std::lock_guard lock(accounting_->mutex);
    if (accounting_->preparedBound) {
        if ((retirement && retirement != accounting_->retirement) ||
            (payloadCap && payloadCap != accounting_->payloadCap) ||
            (entryCap && *entryCap != accounting_->entryCap))
            throw std::invalid_argument("incompatible shared prepared retention policy");
        return;
    }
    if (!retirement)
        throw std::invalid_argument("prepared retention requires explicit host owner");
    accounting_->retirement = retirement;
    accounting_->payloadCap = payloadCap;
    accounting_->entryCap = entryCap.value_or(std::numeric_limits<std::size_t>::max());
    accounting_->configuredEntryCap = entryCap;
    accounting_->preparedBound = true;
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
        closed = accounting_->preparedClosed;
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
std::shared_ptr<const render::Rgba32fImage>
OperationCache::findPreparedUpload(const std::string& content) {
    const std::lock_guard lock(accounting_->mutex);
    promoteCandidateLocked();
    const auto found = index_.find({content, true});
    if (found == index_.end()) {
        ++accounting_->preparedStatistics.misses;
        return nullptr;
    }
    touch(found->second);
    ++accounting_->preparedStatistics.hits;
    return (*found->second)->value.image;
}
void OperationCache::retireCandidateLocked() {
    if (!preparedCandidate_)
        return;
    auto* const node = preparedCandidate_.release();
    node->retiredCandidate = true;
    ++accounting_->pendingCount;
    ++accounting_->pendingCandidates;
    accounting_->moveToPending(*node);
    accounting_->pendingPayload += node->value.image->pixels().size_bytes();
    const auto retirement = node->retirement;
    render::detail::enqueuePackedHostRetirement(retirement, node);
}
void OperationCache::promoteCandidateLocked() {
    if (!preparedCandidate_)
        return;
    const auto payload = preparedCandidate_->value.image->pixels().size_bytes();
    const auto cap = accounting_->payloadCap.value_or(std::numeric_limits<std::uint64_t>::max());
    if (accounting_->preparedClosed || !accounting_->preparedReady ||
        MemoryBudgetLedger::Clock::now() > accounting_->validUntil ||
        preparedCandidate_->standaloneBytes() > accounting_->budget || payload > cap ||
        accounting_->entryCap == 0) {
        retireCandidateLocked();
        return;
    }
    // The candidate's own charge is already included in these totals. Pending victims donate
    // capacity only after their worker has deleted them and folded their charges back.
    if (accounting_->bytes > accounting_->budget || accounting_->preparedPayload > cap ||
        accounting_->preparedCount > accounting_->entryCap)
        return;
    // The candidate stays charged as it is: inserting it adds no charge.
    auto candidate = std::move(preparedCandidate_);
    const auto restoreCandidate = [&] {
        if (candidate)
            preparedCandidate_ = std::move(candidate);
    };
    try {
        insertLocked(candidate, false);
    } catch (...) {
        // An exceptional insertion returns the Entry to candidate. Restore its exact charge and
        // ownership so a later lookup can retry; the cache facade must not expose the failure.
        restoreCandidate();
        return;
    }
    restoreCandidate();
}
void OperationCache::storePreparedUpload(std::string content,
                                         std::shared_ptr<const render::Rgba32fImage> image,
                                         LinearPassRetention* const linearPass) {
    if (!image || image->pixels().empty())
        return;
    std::shared_ptr<render::detail::PackedHostRetirementState> retirement;
    {
        const std::lock_guard lock(accounting_->mutex);
        if (accounting_->preparedClosed)
            return;
        retirement = accounting_->retirement;
    }
    if (!render::detail::preparePackedHostRetirement(retirement))
        return;
    std::unique_ptr<Entry> entry;
    try {
        entry = std::make_unique<Entry>();
    } catch (const std::bad_alloc&) {
        return;
    }
    entry->content = std::move(content);
    entry->value.image = std::move(image);
    entry->kind = OperationCacheEntryKind::PreparedUpload;
    entry->metadataBytes =
        addCharge(sizeof(Entry) + sizeof(Address) + sizeof(void*) * 16, entry->content.capacity());
    const auto payload = entry->value.image->pixels().size_bytes();
    entry->storage[0] = {entry->value.image->pixels().data(),
                         imageObjectBytes(*entry->value.image)};
    entry->accounting = accounting_;
    entry->retirement = retirement;
    entry->destroy = destroyPreparedEntry;
    if (!render::detail::registerPackedHostAllocation(retirement))
        return;
    entry->registered = true;
    {
        const std::lock_guard lock(accounting_->mutex);
        promoteCandidateLocked();
        const auto cap =
            accounting_->payloadCap.value_or(std::numeric_limits<std::uint64_t>::max());
        if (accounting_->preparedClosed || !accounting_->preparedReady ||
            MemoryBudgetLedger::Clock::now() > accounting_->validUntil ||
            accounting_->liveIncrement(*entry) > accounting_->budget || payload > cap ||
            accounting_->entryCap == 0)
            return;
        if (preparedCandidate_)
            return;
        const auto found = index_.find({entry->content, true});
        if (found == index_.end() && linearPass && !linearPass->offer(entry->content, true)) {
            ++accounting_->preparedStatistics.linearPassDeferrals;
            return;
        }
        if (found != index_.end() && (*found->second)->value.image == entry->value.image) {
            touch(found->second);
            return; // Same immutable backing is already retained, not a replacement.
        }
        if (accounting_->pendingCandidates != 0)
            return; // One retiring candidate is the bound; no over-budget queue.
        if (found != index_.end())
            removeLocked(found->second);
        while (!entries_.empty() &&
               (accounting_->preparedPayload - accounting_->pendingPayload > cap - payload ||
                accounting_->preparedCount - accounting_->pendingCount >= accounting_->entryCap)) {
            auto victim = std::find_if(entries_.rbegin(), entries_.rend(), [](const auto& item) {
                return item->kind == OperationCacheEntryKind::PreparedUpload;
            });
            if (victim == entries_.rend())
                break;
            removeLocked(std::prev(victim.base()));
        }
        std::uint64_t ignored = 0;
        evictToLocked(accounting_->budget - accounting_->liveIncrement(*entry), ignored);
        // Recomputed: eviction may have left storage this entry shares only pending.
        const auto charge = accounting_->liveIncrement(*entry);
        if (charge > accounting_->budget || accounting_->bytes > accounting_->budget - charge ||
            accounting_->preparedPayload > cap - payload ||
            accounting_->preparedCount >= accounting_->entryCap) {
            // The producer already owns this decoded image. Retain ONE candidate so the cache
            // can admit it after actual retirement, while charging the extra cache reference and
            // metadata immediately. The transient total may exceed the configured ceiling by
            // at most this single individually eligible candidate; it is never reported as free.
            const bool pendingCanMakeRoom =
                accounting_->pendingBytes > 0 &&
                accounting_->bytes - accounting_->pendingBytes <= accounting_->budget - charge &&
                accounting_->preparedPayload - accounting_->pendingPayload <= cap - payload &&
                accounting_->preparedCount - accounting_->pendingCount < accounting_->entryCap &&
                charge <= std::numeric_limits<std::size_t>::max() - accounting_->bytes &&
                payload <= std::numeric_limits<std::uint64_t>::max() - accounting_->preparedPayload;
            if (pendingCanMakeRoom) {
                accounting_->chargeLive(*entry);
                accounting_->preparedPayload += payload;
                ++accounting_->preparedCount;
                preparedCandidate_ = std::move(entry);
            }
            return;
        }
        insertLocked(entry);
    }
}
void OperationCache::clearPreparedUploads() {
    const std::lock_guard lock(accounting_->mutex);
    retireCandidateLocked();
    removeKindLocked(OperationCacheEntryKind::PreparedUpload);
}
void OperationCache::closePreparedUploads() {
    std::shared_ptr<detail::VideoSourceContext> video;
    {
        const std::lock_guard lock(accounting_->mutex);
        accounting_->preparedClosed = true;
        retireCandidateLocked();
        removeKindLocked(OperationCacheEntryKind::PreparedUpload);
        // Every rooted ordinary entry must leave before the shared host owner can acknowledge
        // shutdown. Otherwise an Operation/decoded image could keep its registration forever.
        removeKindLocked(OperationCacheEntryKind::DecodedMedia);
        removeKindLocked(OperationCacheEntryKind::Operation);
        video = accounting_->videoSources.lock();
    }
    if (video)
        video->beginShutdown();
}
bool OperationCache::preparedUploadsBound() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->preparedBound;
}
PreparedUploadRetentionPolicy OperationCache::preparedRetentionPolicy() const {
    const std::lock_guard lock(accounting_->mutex);
    return {accounting_->payloadCap, accounting_->configuredEntryCap};
}
std::uint64_t OperationCache::preparedPayloadBytes() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->preparedPayload;
}
std::size_t OperationCache::preparedEntryCount() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->preparedCount - accounting_->pendingCount -
           static_cast<std::size_t>(preparedCandidate_ != nullptr);
}
std::uint64_t OperationCache::preparedPayloadCeiling() const {
    const std::lock_guard lock(accounting_->mutex);
    return std::min<std::uint64_t>(
        accounting_->budget,
        accounting_->payloadCap.value_or(std::numeric_limits<std::uint64_t>::max()));
}
std::size_t OperationCache::preparedEntryCeiling() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->entryCap;
}
OperationCacheAccessStatistics OperationCache::preparedStatistics() const {
    const std::lock_guard lock(accounting_->mutex);
    return accounting_->preparedStatistics;
}
} // namespace bloom::runtime
