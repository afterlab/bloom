#include <bloom/runtime/operation_cache.hpp>
#include <bloom/ui/preview_frame_cache.hpp>

#include <QLatin1StringView>
#include <QSettings>
#include <QVariant>

#include <algorithm>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace bloom::ui {
namespace {

// What the cache retains for one frame: the packed display buffer and the identity, never the
// Float32 process image (task PERF1, FORMAL AMENDMENT 1). Production CPU preparation supplies an
// already-stripped product from its worker. A legacy direct CPU insertion still copies here; the
// process image the source frame holds is never carried into the cache.
[[nodiscard]] std::shared_ptr<const runtime::PreviewDisplayOnlyFrame>
retainable(const runtime::PreparedPreviewFrame& frame) {
    if (!frame.hasProcessFrame()) {
        // Already display-only -- a frame this cache published a moment ago -- so there is nothing
        // left to strip and no copy to make.
        return frame.displayOnlyFrame();
    }
    auto stripped =
        runtime::PreviewDisplayOnlyFrame::create(frame, std::numeric_limits<std::size_t>::max());
    if (!stripped.has_value()) {
        return nullptr;
    }
    return std::make_shared<const runtime::PreviewDisplayOnlyFrame>(std::move(*stripped));
}

[[nodiscard]] PreparedPreviewFrameHandle
restamp(const std::shared_ptr<const runtime::PreviewDisplayOnlyFrame>& frame,
        const std::uint64_t requestGeneration) {
    // The display buffer is what a retained frame IS; the request generation is only which ask it
    // answered. Re-creating the envelope over the same immutable buffer is therefore a copy of one
    // small identity struct and a shared-pointer increment -- no pixel is touched.
    auto rebuilt = runtime::PreparedPreviewFrame::createDisplayOnly(requestGeneration, frame);
    if (!rebuilt.has_value()) {
        return nullptr;
    }
    return std::make_shared<const runtime::PreparedPreviewFrame>(std::move(*rebuilt));
}

} // namespace

std::size_t PreviewFrameCache::KeyHash::operator()(const PreviewFrameCacheKey& key) const noexcept {
    std::size_t hash = 0;
    const auto add = [&hash]<typename T>(const T& value) {
        hash ^= std::hash<T>{}(value) + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
    };
    add(key.projectId.value());
    add(key.compositionId.value());
    add(key.sourceRevision.value());
    add(key.time.numerator());
    add(key.time.denominator());
    add(static_cast<std::uint8_t>(key.output));
    add(key.resolution.index());
    if (const auto* proxy = std::get_if<runtime::ProxyResolution>(&key.resolution)) {
        add(proxy->extent.width());
        add(proxy->extent.height());
    }
    add(static_cast<std::uint8_t>(key.quality));
    add(key.colorIntent.workingColorSpaceId);
    add(key.colorIntent.ocioConfigUri);
    for (const auto byte : key.colorIntent.ocioConfigRevision.bytes())
        add(byte);
    add(static_cast<std::uint8_t>(key.resolutionPolicy));
    add(key.roi.has_value());
    if (key.roi) {
        add(key.roi->originX());
        add(key.roi->originY());
        add(key.roi->extent().width());
        add(key.roi->extent().height());
    }
    add(key.viewAdjust.exposure);
    add(key.viewAdjust.gamma);
    add(key.displayName);
    add(key.viewName);
    add(key.showLook);
    add(key.channelInspection.has_value());
    if (key.channelInspection) {
        add(key.channelInspection->node.value());
        add(key.channelInspection->port);
        for (const auto& channel : key.channelInspection->channels)
            add(channel);
    }
    add(key.isolation.size());
    for (const auto layer : key.isolation)
        add(layer.value());
    return hash;
}

bool PreviewFrameCache::RangeLess::operator()(const RangeKey& left,
                                              const RangeKey& right) const noexcept {
    if (left.projectId != right.projectId)
        return left.projectId.value() < right.projectId.value();
    if (left.compositionId != right.compositionId)
        return left.compositionId.value() < right.compositionId.value();
    return left.time < right.time;
}

PreviewFrameCacheKey
PreviewFrameCacheKey::forIdentity(const runtime::PreviewRequestIdentity& identity) {
    return {.projectId = identity.projectId,
            .compositionId = identity.compositionId,
            .sourceRevision = identity.sourceRevision,
            .time = identity.time,
            .output = identity.output,
            .resolution = identity.resolution,
            .quality = identity.quality,
            .colorIntent = identity.colorIntent,
            .resolutionPolicy = identity.resolutionPolicy,
            .roi = identity.roi,
            .viewAdjust = identity.viewAdjust,
            .displayName = identity.displayName,
            .viewName = identity.viewName,
            .showLook = identity.showLook,
            .channelInspection = identity.channelInspection,
            .isolation = identity.isolation};
}

PreviewFrameCache::PreviewFrameCache(const std::size_t byteBudget,
                                     runtime::MemoryBudgetLedger& ledger)
    : ledger_(ledger), byteBudget_(byteBudget) {
    notificationTimer_.setSingleShot(true);
    notificationTimer_.setInterval(50);
    connect(&notificationTimer_, &QTimer::timeout, this, &PreviewFrameCache::contentsChanged);
    residentRefreshTimer_.setInterval(250);
    connect(&residentRefreshTimer_, &QTimer::timeout, this,
            &PreviewFrameCache::refreshResidentEntries);
    ledger_.registerCache(
        this, byteBudget,
        [this] {
            const auto host = hostBytes();
            return runtime::MemoryPoolHolding{.hostBytes = host,
                                              .deviceBytes = residentBytes_ + scopeBytes_ - host};
        },
        [this](const std::size_t bytes) {
            const bool pressure = ledger_.state().retentionPercent < 100;
            if (pressure)
                trimToBytes(bytes);
            const bool changed = byteBudget_ != bytes;
            byteBudget_ = bytes;
            if (!pressure)
                evictToBudget();
            // The ledger applies budgets under its own lock; observers read it back and
            // restart work, so they hear about the change after it returns.
            if (changed)
                scheduleByteBudgetNotification();
        },
        runtime::MemoryBudgetOwnerKind::ReusableRetention,
        runtime::MemoryEvictionPriority::FinishedPreview);
    ledger_.setPoolProfile(this, {.name = "Preview frames",
                                  .kind = runtime::ProfilerPoolKind::Cache,
                                  .counters = &counters_,
                                  .rows = {},
                                  .entries = [this] { return entries_.size(); }});
    // #728: the entries belong to this thread. A budget another thread asks the ledger for (an
    // export's capacity sample, an admission reclaim) is applied here, never on that thread.
    ledger_.bindCacheToThisThread(this, [this] {
        QMetaObject::invokeMethod(
            this, [this] { ledger_.applyDeferredBudgets(this); }, Qt::QueuedConnection);
    });
}

PreviewFrameCache::~PreviewFrameCache() { ledger_.unregisterCache(this); }

void PreviewFrameCache::scheduleByteBudgetNotification() {
    if (byteBudgetNotificationPending_.exchange(true))
        return;
    QMetaObject::invokeMethod(
        this,
        [this] {
            byteBudgetNotificationPending_ = false;
            emit byteBudgetChanged();
        },
        Qt::QueuedConnection);
}

void PreviewFrameCache::scheduleNotification() {
    if (!notificationTimer_.isActive()) {
        notificationTimer_.start();
    }
}

std::size_t PreviewFrameCache::frameByteCost(const runtime::PreparedPreviewFrame& frame) noexcept {
    // The resident arm is identified by its honest provenance, not by residentFrame(): that
    // accessor is a precondition-guarded resident-only view, and calling it on a CPU frame would
    // read a variant member that is not active. The resident retention cost is the ACTUAL native
    // allocation the lease charges plus its retained geometry/metadata -- never a host copy.
    if (frame.provenance().provider == runtime::PreviewDisplayProvider::GpuResident) {
        const auto& resident = frame.residentFrame();
        return resident != nullptr ? resident->retainedByteCost() : 0;
    }
    if (frame.provenance().provider == runtime::PreviewDisplayProvider::GpuPreparedPacked) {
        const auto& packed = frame.displayOnlyFrame();
        return packed ? packed->displayByteCost() : 0;
    }
    // What RETAINING a CPU frame costs, which is not what holding it costs right now: insertion
    // keeps packed display pixels and evaluated geometry while dropping the Float32 process image.
    const auto view = frame.displayBufferView();
    const auto geometryBytes = frame.evaluatedBounds().size_bytes();
    if (!view ||
        geometryBytes > std::numeric_limits<std::size_t>::max() - view->pixels.size_bytes())
        return 0;
    return view->pixels.size_bytes() + geometryBytes;
}

std::size_t
PreviewFrameCache::hostByteCost(const runtime::PreparedPreviewFrame& frame) const noexcept {
    const bool resident =
        frame.provenance().provider == runtime::PreviewDisplayProvider::GpuResident;
    return resident && deviceBudgeted() ? 0 : frameByteCost(frame);
}

PreparedPreviewFrameHandle
PreviewFrameCache::take(const runtime::PreviewRequestIdentity& identity) {
    const auto key = PreviewFrameCacheKey::forIdentity(identity);
    ++metadataWork_.exactProbes;
    const auto found = exactIndex_.find(key);
    if (found == exactIndex_.end() || !entryIsLive(*found->second)) {
        // A dead resident entry (invalidated lease) is a miss, never a hit: it can never be served.
        if (found != exactIndex_.end())
            removeAt(found->second);
        ++statistics_.misses;
        counters_.miss();
        if (trace_)
            trace_->record(identity, PreviewTraceStage::CacheLookup, PreviewTraceOutcome::Miss);
        return nullptr;
    }
    const auto position = found->second;
    PreparedPreviewFrameHandle frame;
    if (position->resident != nullptr) {
        // Resident re-stamp: share the same opaque lease, copy no pixels and no native allocation.
        auto rebuilt = runtime::PreparedPreviewFrame::createResident(identity.requestGeneration,
                                                                     position->resident);
        if (rebuilt.has_value()) {
            frame = std::make_shared<const runtime::PreparedPreviewFrame>(std::move(*rebuilt));
        }
    } else {
        frame = restamp(position->frame, identity.requestGeneration);
    }
    if (frame == nullptr || frame->desiredIdentity() != identity) {
        // The key matched but the rebuilt envelope does not answer this request exactly. Nothing
        // here can be served honestly, so the entry is dropped rather than published under an
        // identity it does not have.
        removeAt(position);
        ++statistics_.misses;
        counters_.miss();
        if (trace_)
            trace_->record(identity, PreviewTraceStage::CacheLookup, PreviewTraceOutcome::Invalid);
        return nullptr;
    }
    entries_.splice(entries_.begin(), entries_, position);
    if (position->residentPosition)
        residentOrder_.splice(residentOrder_.begin(), residentOrder_, *position->residentPosition);
    ++statistics_.hits;
    counters_.hit();
    if (trace_)
        trace_->record(*frame, PreviewTraceStage::CacheLookup, PreviewTraceOutcome::Hit);
    return frame;
}

void PreviewFrameCache::allocationCheckpoint(const InsertAllocation boundary) {
    if (insertAllocationFault_ == boundary) {
        insertAllocationFault_ = InsertAllocation::None;
        throw std::bad_alloc{};
    }
}

PreviewFrameCache::InsertResult PreviewFrameCache::insert(const PreparedPreviewFrameHandle& frame) {
    if (frame == nullptr)
        return {};
    const auto report = [&](const InsertOutcome outcome, const PreviewTraceOutcome traceOutcome,
                            const std::uint64_t evictions = 0) {
        if (trace_)
            trace_->record(*frame, PreviewTraceStage::CacheRetention, traceOutcome);
        return InsertResult{outcome, evictions};
    };
    const bool resident =
        frame->provenance().provider == runtime::PreviewDisplayProvider::GpuResident;
    if (!frame->isDisplayValid()) {
        ++statistics_.rejections;
        counters_.refused();
        return report(InsertOutcome::RejectedStale, PreviewTraceOutcome::Invalid);
    }
    try {
        allocationCheckpoint(InsertAllocation::Key);
        auto key = PreviewFrameCacheKey::forIdentity(frame->desiredIdentity());
        if (!workAreaAllows(key)) {
            ++statistics_.rangeDrops;
            return report(InsertOutcome::RejectedStale, PreviewTraceOutcome::OutOfRange);
        }
        if (!provenanceAllows(key)) {
            ++statistics_.staleDrops;
            return report(InsertOutcome::RejectedStale, PreviewTraceOutcome::WrongRevision);
        }
        const bool qualificationChanged =
            displayQualified_.has_value() && *displayQualified_ != frame->isOcioQualified();
        ++metadataWork_.exactProbes;
        const auto existing = exactIndex_.find(key);
        if (!qualificationChanged && existing != exactIndex_.end() &&
            entryIsLive(*existing->second)) {
            entries_.splice(entries_.begin(), entries_, existing->second);
            if (existing->second->residentPosition)
                residentOrder_.splice(residentOrder_.begin(), residentOrder_,
                                      *existing->second->residentPosition);
            return report(InsertOutcome::AlreadyPresent, PreviewTraceOutcome::Reused);
        }
        const auto bytes = frameByteCost(*frame);
        // A device-budgeted resident frame takes nothing from the host byte budget.
        const bool charged = !resident || !deviceBudgeted();
        const auto hostCharge = charged ? bytes : 0;
        const auto refuse = [&] {
            ++statistics_.rejections;
            counters_.refused();
            return report(InsertOutcome::RejectedBudget, PreviewTraceOutcome::OverBudget);
        };
        // What no room can fix is refused before any pool is asked to give memory back.
        if (bytes == 0 ||
            (resident && (bytes > gpuResidentByteLimit_ || gpuResidentEntryLimit_ == 0)))
            return refuse();
        const auto host = hostBytes();
        if (hostCharge != 0 && (host > byteBudget_ || hostCharge > byteBudget_ - host)) {
            // Preview first: during a fill the ledger's Ordinary pools make room for this frame
            // before an earlier finished frame is evicted. Memory-neutral: what they release is
            // what this budget gains. Asked before any staging, with no cache state in flux.
            const auto need = host > byteBudget_ ? host - byteBudget_ + hostCharge
                                                 : hostCharge - (byteBudget_ - host);
            if (const auto granted = ledger_.yieldToPreview(this, need); granted != 0) {
                byteBudget_ +=
                    std::min(granted, std::numeric_limits<std::size_t>::max() - byteBudget_);
                statistics_.previewFirstYieldedBytes += granted;
                // Observers re-enter this cache; tell them after the insert returns.
                scheduleByteBudgetNotification();
            }
        }
        if (hostCharge > byteBudget_)
            return refuse();
        // Prepare every allocating part before changing qualification, revisions or ownership.
        allocationCheckpoint(InsertAllocation::Product);
        Entry incoming{.key = std::move(key),
                       .frame = nullptr,
                       .resident = nullptr,
                       .bytes = bytes,
                       .scopes = nullptr,
                       .scopeBytes = 0};
        if (resident)
            incoming.resident = frame->residentFrame();
        else
            incoming.frame = retainable(*frame);
        if (!resident && incoming.frame == nullptr) {
            // A validated display product with an unbounded strip budget can fail here only
            // while allocating its copy; the optional factory contains that exception.
            ++statistics_.allocationFailures;
            return report(InsertOutcome::AllocationFailure, PreviewTraceOutcome::AllocationFailure);
        }
        if (!resident && frame->hasProcessFrame())
            ++statistics_.uiPixelCopies;
        // Stage every allocation (stable owner, exact key, range key, resident order) before any
        // qualification or revision pruning. Node handles and list splices commit without copying
        // a frame or allocating after the semantic mutation begins.
        Entries staged;
        staged.push_back(std::move(incoming));
        std::unordered_map<PreviewFrameCacheKey, EntryIterator, KeyHash> stagedExact;
        stagedExact.emplace(staged.front().key, staged.begin());
        RangeIndex stagedRange;
        stagedRange.emplace(RangeKey{staged.front().key.projectId, staged.front().key.compositionId,
                                     staged.front().key.time},
                            staged.begin());
        std::list<EntryIterator> stagedResident;
        if (resident)
            stagedResident.push_front(staged.begin());
        std::unordered_map<std::uint64_t, std::uint64_t> stagedRevision;
        if (!hasProvenancePolicy() &&
            (qualificationChanged ||
             !uniformProjectRevision_.contains(staged.front().key.projectId.value()))) {
            stagedRevision.emplace(staged.front().key.projectId.value(),
                                   staged.front().key.sourceRevision.value());
        }
        allocationCheckpoint(InsertAllocation::Capacity);
        exactIndex_.reserve(exactIndex_.size() + 1);
        if (!hasProvenancePolicy())
            uniformProjectRevision_.reserve(uniformProjectRevision_.size() + 1);
        if (qualificationChanged)
            clear();
        displayQualified_ = frame->isOcioQualified();
        if (!hasProvenancePolicy()) {
            const auto project = staged.front().key.projectId.value();
            const auto revision = staged.front().key.sourceRevision.value();
            const auto known = uniformProjectRevision_.find(project);
            if (known == uniformProjectRevision_.end() || known->second != revision)
                dropStaleRevisions(staged.front().key);
            if (known == uniformProjectRevision_.end())
                uniformProjectRevision_.insert(stagedRevision.extract(stagedRevision.begin()));
            else
                known->second = revision;
        }
        ++metadataWork_.exactProbes;
        if (const auto duplicate = exactIndex_.find(staged.front().key);
            duplicate != exactIndex_.end())
            removeAt(duplicate->second);
        const auto evictionsBefore = statistics_.evictions;
        entries_.splice(entries_.begin(), staged, staged.begin());
        const auto inserted = entries_.begin();
        inserted->self = inserted;
        exactIndex_.insert(stagedExact.extract(stagedExact.begin()));
        inserted->rangePosition = rangeIndex_.insert(stagedRange.extract(stagedRange.begin()));
        if (resident) {
            residentOrder_.splice(residentOrder_.begin(), stagedResident, stagedResident.begin());
            inserted->residentPosition = residentOrder_.begin();
        }
        // Sum only after bounding the old retained set, avoiding size_t overflow at extreme caps.
        while (charged && hostBytes() > byteBudget_ - bytes) {
            const auto victim = byteBudgetVictim(inserted);
            if (victim == entries_.end())
                break;
            ++statistics_.evictions;
            counters_.evicted(victim->bytes);
            if (!entryIsLive(*victim))
                ++statistics_.staleDrops;
            removeAt(victim);
        }
        residentBytes_ += bytes;
        if (resident) {
            gpuResidentBytes_ += bytes;
            ++gpuResidentEntries_;
            if (!residentRefreshTimer_.isActive())
                residentRefreshTimer_.start();
        }
        ++statistics_.insertions;
        counters_.store();
        scheduleNotification();
        evictToBudget();
        if (resident)
            evictGpuResidentToLimits();
        // MRU admission fits both individual limits, but lease validity can change concurrently.
        const bool live = !entries_.empty() && entryIsLive(entries_.front());
        if (live)
            emit entryRetained(entries_.front().key);
        return report(live ? InsertOutcome::Retained : InsertOutcome::RejectedStale,
                      live ? PreviewTraceOutcome::Retained : PreviewTraceOutcome::Invalid,
                      statistics_.evictions - evictionsBefore);
    } catch (const std::bad_alloc&) {
        ++statistics_.allocationFailures;
        counters_.refused();
        return report(InsertOutcome::AllocationFailure, PreviewTraceOutcome::AllocationFailure);
    }
}

void PreviewFrameCache::refreshResidentEntries() {
    for (auto position = residentOrder_.begin(); position != residentOrder_.end();) {
        const auto entry = *position++;
        if (!entryIsLive(*entry)) {
            ++statistics_.staleDrops;
            removeAt(entry);
        }
    }
    if (gpuResidentEntries_ == 0)
        residentRefreshTimer_.stop();
}

void PreviewFrameCache::trimToBytes(const std::size_t bytes) {
    // Deliberately not setByteBudget(): the budget is the artist's or the ledger's decision and
    // must survive the pressure that caused this trim, so the cache can fill back up once the
    // machine recovers.
    const auto limit = std::min(bytes, byteBudget_);
    while (hostBytes() > limit) {
        const auto victim = byteBudgetVictim(entries_.end());
        if (victim == entries_.end())
            break;
        ++statistics_.pressureDrops;
        counters_.evicted(victim->bytes);
        if (!entryIsLive(*victim))
            ++statistics_.staleDrops;
        removeAt(victim);
    }
}

bool PreviewFrameCache::entryIsLive(const Entry& entry) noexcept {
    return entry.resident == nullptr || entry.resident->isDisplayValid();
}

bool PreviewFrameCache::contains(const PreviewFrameCacheKey& key) const {
    ++metadataWork_.exactProbes;
    const auto found = exactIndex_.find(key);
    return found != exactIndex_.end() && entryIsLive(*found->second);
}

bool PreviewFrameCache::attachScopes(const PreviewFrameCacheKey& key,
                                     PreviewScopeSidecarHandle sidecar) {
    const auto found = exactIndex_.find(key);
    if (sidecar == nullptr || found == exactIndex_.end() || !entryIsLive(*found->second))
        return false;
    const auto position = found->second;
    const auto bytes = sidecar->byteSize();
    scopeBytes_ -= position->scopeBytes;
    position->scopes = std::move(sidecar);
    position->scopeBytes = bytes;
    scopeBytes_ += bytes;
    // Older frames give way as they do for a frame; this entry is never its own victim.
    while (hostBytes() > byteBudget_) {
        const auto victim = byteBudgetVictim(position);
        if (victim == entries_.end()) {
            scopeBytes_ -= bytes;
            position->scopes.reset();
            position->scopeBytes = 0;
            return false;
        }
        ++statistics_.evictions;
        counters_.evicted(victim->bytes);
        removeAt(victim);
    }
    return true;
}

PreviewScopeSidecarHandle PreviewFrameCache::scopesFor(const PreviewFrameCacheKey& key,
                                                       const PreviewScopeIdentity& identity) const {
    const auto found = exactIndex_.find(key);
    if (found == exactIndex_.end() || !entryIsLive(*found->second))
        return nullptr;
    const auto& scopes = found->second->scopes;
    return scopes != nullptr && scopes->identity == identity ? scopes : nullptr;
}

std::optional<PreviewScopeSource>
PreviewFrameCache::scopeSourceFor(const PreviewFrameCacheKey& key) const {
    const auto found = exactIndex_.find(key);
    if (found == exactIndex_.end() || !entryIsLive(*found->second))
        return std::nullopt;
    const Entry& entry = *found->second;
    if (entry.resident != nullptr)
        return PreviewScopeSource{entry.resident->desiredIdentity(),
                                  entry.resident->processIdentity(),
                                  runtime::PreviewDisplayProvider::GpuResident};
    return PreviewScopeSource{entry.frame->desiredIdentity(), entry.frame->processIdentity(),
                              entry.frame->provenance().provider};
}

std::optional<PreviewScopeFrame>
PreviewFrameCache::peekScopeFrame(const PreviewFrameCacheKey& key) const {
    const auto source = scopeSourceFor(key);
    if (!source)
        return std::nullopt;
    const Entry& entry = *exactIndex_.find(key)->second;
    PreparedPreviewFrameHandle frame;
    if (entry.resident != nullptr) {
        auto rebuilt = runtime::PreparedPreviewFrame::createResident(
            entry.resident->desiredIdentity().requestGeneration, entry.resident);
        if (rebuilt.has_value())
            frame = std::make_shared<const runtime::PreparedPreviewFrame>(std::move(*rebuilt));
    } else {
        frame = restamp(entry.frame, entry.frame->desiredIdentity().requestGeneration);
    }
    if (frame == nullptr)
        return std::nullopt;
    return PreviewScopeFrame{std::move(frame), *source};
}

std::vector<core::RationalTime>
PreviewFrameCache::timesFor(const PreviewFrameCacheKey& probe) const {
    const auto lastTime = core::RationalTime::fromInteger(std::numeric_limits<std::int64_t>::max());
    auto times = timesForRange(
        probe, core::RationalTime::fromInteger(std::numeric_limits<std::int64_t>::min()), lastTime);
    // The all-times overload includes the largest representable integer time; the range overload
    // is deliberately half-open and excludes its end.
    const auto [begin, end] =
        rangeIndex_.equal_range({probe.projectId, probe.compositionId, lastTime});
    for (auto position = begin; position != end; ++position) {
        ++metadataWork_.rangeEntriesVisited;
        const auto& entry = *position->second;
        if (!entryIsLive(entry) || !retains(entry.key))
            continue;
        auto key = probe;
        key.time = entry.key.time;
        if (hasProvenancePolicy())
            key.sourceRevision = entry.key.sourceRevision;
        if (key == entry.key)
            times.push_back(entry.key.time);
    }
    return times;
}

std::vector<core::RationalTime>
PreviewFrameCache::timesForRange(const PreviewFrameCacheKey& probe, const core::RationalTime start,
                                 const core::RationalTime end) const {
    // TEMPORAL-2B: a probe names ONE revision, but after finite clip-range edits the retained
    // timeline legitimately spans several genuine revisions. Cached markers are therefore matched
    // on everything that decides pixels EXCEPT the source revision, so a sub-range captured at an
    // older retained revision still paints its "cached" bar.
    std::vector<core::RationalTime> times;
    auto key = probe;
    const auto begin = rangeIndex_.lower_bound({probe.projectId, probe.compositionId, start});
    const auto finish = rangeIndex_.lower_bound({probe.projectId, probe.compositionId, end});
    for (auto position = begin; position != finish; ++position) {
        ++metadataWork_.rangeEntriesVisited;
        const auto& entry = *position->second;
        if (!entryIsLive(entry) || !retains(entry.key)) {
            continue;
        }
        key.time = entry.key.time;
        if (hasProvenancePolicy())
            key.sourceRevision = entry.key.sourceRevision;
        if (key == entry.key) {
            times.push_back(entry.key.time);
        }
    }
    return times;
}

void PreviewFrameCache::setByteBudget(const std::size_t bytes) {
    static_cast<void>(ledger_.setCacheCeiling(this, bytes));
}

void PreviewFrameCache::releaseForShutdown() noexcept {
    notificationTimer_.stop();
    residentRefreshTimer_.stop();
    entries_.clear();
    exactIndex_.clear();
    rangeIndex_.clear();
    residentOrder_.clear();
    uniformProjectRevision_.clear();
    residentBytes_ = 0;
    gpuResidentBytes_ = 0;
    gpuResidentEntries_ = 0;
    scopeBytes_ = 0;
    retentionRange_.reset();
    retentionSnapshots_.clear();
}

void PreviewFrameCache::clear() {
    const bool hadEntries = !entries_.empty();
    if (hadEntries) {
        scheduleNotification();
    }
    entries_.clear();
    exactIndex_.clear();
    rangeIndex_.clear();
    residentOrder_.clear();
    uniformProjectRevision_.clear();
    residentBytes_ = 0;
    gpuResidentBytes_ = 0;
    gpuResidentEntries_ = 0;
    scopeBytes_ = 0;
    residentRefreshTimer_.stop();
    if (hadEntries)
        emit entriesCleared();
}

bool PreviewFrameCache::workAreaAllows(const PreviewFrameCacheKey& key) const noexcept {
    if (!retentionRange_.has_value())
        return true;
    const auto& range = *retentionRange_;
    if (key.projectId != range.projectId || key.compositionId != range.compositionId)
        return true;
    return key.time >= range.start && key.time < range.end;
}

std::optional<document::Revision>
PreviewFrameCache::acceptedRevision(const PreviewFrameCacheKey& key) const noexcept {
    for (const auto& span : retentionSnapshots_) {
        if (span.projectId == key.projectId && span.compositionId == key.compositionId &&
            key.time >= span.start && key.time < span.end) {
            return span.revision;
        }
    }
    return std::nullopt;
}

bool PreviewFrameCache::provenanceAllows(const PreviewFrameCacheKey& key) const noexcept {
    if (!hasProvenancePolicy())
        return true;
    const auto accepted = acceptedRevision(key);
    return accepted.has_value() && key.sourceRevision == *accepted;
}

bool PreviewFrameCache::retains(const PreviewFrameCacheKey& key) const noexcept {
    return workAreaAllows(key) && provenanceAllows(key);
}

void PreviewFrameCache::pruneUnaccepted() {
    for (auto position = entries_.begin(); position != entries_.end();) {
        const auto entry = position++;
        if (!workAreaAllows(entry->key)) {
            ++statistics_.rangeDrops;
            removeAt(entry);
            continue;
        }
        if (!provenanceAllows(entry->key)) {
            ++statistics_.staleDrops;
            removeAt(entry);
        }
    }
}

void PreviewFrameCache::dropStaleRevisions(const PreviewFrameCacheKey& current) {
    for (auto position = entries_.begin(); position != entries_.end();) {
        ++metadataWork_.revisionEntriesVisited;
        const auto entry = position++;
        if (entry->key.projectId != current.projectId) {
            // CACHEFIX-3. A different project is not this revision's concern; its entries stay
            // retained (the cache key and the retention policy both carry project identity).
            continue;
        }
        if (entry->key.sourceRevision == current.sourceRevision) {
            continue;
        }
        ++statistics_.staleDrops;
        removeAt(entry);
    }
}

void PreviewFrameCache::setRetentionSnapshots(std::vector<RetentionSnapshot> snapshots) {
    if (retentionSnapshots_ == snapshots)
        return;
    retentionSnapshots_ = std::move(snapshots);
    uniformProjectRevision_.clear();
    pruneUnaccepted();
}

std::size_t PreviewFrameCache::pruneToRange(const RetentionRange& range) {
    std::size_t dropped = 0;
    for (auto position = entries_.begin(); position != entries_.end();) {
        const auto entry = position++;
        if (entry->key.projectId != range.projectId ||
            entry->key.compositionId != range.compositionId ||
            (entry->key.time >= range.start && entry->key.time < range.end))
            continue;
        ++statistics_.rangeDrops;
        removeAt(entry);
        ++dropped;
    }
    return dropped;
}

void PreviewFrameCache::setRetentionRange(std::optional<RetentionRange> range) {
    if (retentionRange_ == range)
        return;
    retentionRange_ = range;
    if (retentionRange_.has_value())
        static_cast<void>(pruneToRange(*retentionRange_));
}

std::size_t PreviewFrameCache::hostBytes() const noexcept {
    return (deviceBudgeted() ? residentBytes_ - gpuResidentBytes_ : residentBytes_) + scopeBytes_;
}

PreviewFrameCache::EntryIterator PreviewFrameCache::byteBudgetVictim(const EntryIterator keep) {
    for (auto position = entries_.end(); position != entries_.begin();) {
        --position;
        if (position != keep && chargesByteBudget(*position))
            return position;
    }
    return entries_.end();
}

void PreviewFrameCache::evictToBudget() {
    while (hostBytes() > byteBudget_) {
        const auto victim = byteBudgetVictim(entries_.end());
        if (victim == entries_.end())
            break;
        ++statistics_.evictions;
        counters_.evicted(victim->bytes);
        if (!entryIsLive(*victim))
            ++statistics_.staleDrops;
        removeAt(victim);
    }
}

void PreviewFrameCache::removeAt(const EntryIterator position) {
    scheduleNotification();
    const Entry& entry = *position;
    if (trace_) {
        const auto& identity =
            entry.resident ? entry.resident->desiredIdentity() : entry.frame->desiredIdentity();
        trace_->record(identity, PreviewTraceStage::CacheRemoval, PreviewTraceOutcome::Removed,
                       entry.resident
                           ? static_cast<std::uint8_t>(runtime::PreviewDisplayProvider::GpuResident)
                           : static_cast<std::uint8_t>(entry.frame->provenance().provider));
    }
    residentBytes_ -= entry.bytes;
    scopeBytes_ -= entry.scopeBytes;
    if (entry.resident != nullptr) {
        gpuResidentBytes_ -= entry.bytes;
        --gpuResidentEntries_;
        if (gpuResidentEntries_ == 0)
            residentRefreshTimer_.stop();
    }
    exactIndex_.erase(entry.key);
    rangeIndex_.erase(entry.rangePosition);
    if (entry.residentPosition)
        residentOrder_.erase(*entry.residentPosition);
    auto removedKey = std::move(position->key);
    entries_.erase(position);
    emit entryRemoved(removedKey);
}

void PreviewFrameCache::setGpuResidentLimits(const std::size_t maxBytes,
                                             const std::size_t maxEntries) {
    gpuResidentByteLimit_ = maxBytes;
    gpuResidentEntryLimit_ = maxEntries;
    evictGpuResidentToLimits();
}

std::size_t PreviewFrameCache::evictGpuResidentBytes(const std::size_t bytes) {
    std::size_t released = 0;
    while (released < bytes && !residentOrder_.empty()) {
        const auto victim = residentOrder_.back();
        released += victim->bytes;
        ++statistics_.gpuResidentEvictions;
        ++statistics_.evictions;
        counters_.evicted(victim->bytes);
        if (!entryIsLive(*victim))
            ++statistics_.staleDrops;
        removeAt(victim);
    }
    return released;
}

void PreviewFrameCache::evictGpuResidentToLimits() {
    // The resident set is bounded independently of the overall cache budget so it can never outgrow
    // the service's lease registry. Entries are MRU-first, so the last resident entry is the least
    // recently used GPU frame. Dropping it releases only the cache's reference: the opaque lease
    // stays valid for the viewer and for any native pin that still holds it.
    while ((gpuResidentEntries_ > gpuResidentEntryLimit_ ||
            gpuResidentBytes_ > gpuResidentByteLimit_) &&
           gpuResidentEntries_ > 0) {
        if (residentOrder_.empty()) {
            break;
        }
        const auto victim = residentOrder_.back();
        // The same boundary event is both a GPU-resident eviction and, for the RAM preview
        // controller, a budget eviction: the controller only watches `evictions` to stop a run that
        // has outgrown memory, so a GPU sublimit hit must advance that signal or the run would keep
        // churning every frame against the cap instead of stopping at the prefix that fits.
        ++statistics_.gpuResidentEvictions;
        ++statistics_.evictions;
        counters_.evicted(victim->bytes);
        if (!entryIsLive(*victim))
            ++statistics_.staleDrops;
        removeAt(victim);
    }
}

std::size_t physicalMemoryBytes() noexcept {
    return runtime::processMemoryBudgetLedger().physicalByteBudget();
}

std::size_t defaultPreviewFrameCacheByteBudget() noexcept {
    return runtime::processMemoryBudgetLedger().allocate().previewFrameCacheByteBudget;
}

runtime::MemoryBudgetAllocation
cacheMemoryBudgetAllocationFromSettings(const QSettings& settings,
                                        const runtime::MemoryBudgetLedger& ledger) {
    // The playback/operation-cache-bytes and playback/ram-preview-memory-bytes targets are
    // retired: reusable caches compete inside usable memory, measured RAM minus the RAM headroom
    // preference (MemoryBudgetLedger::setRamHeadroom), and are evicted on demand. Saved values are
    // ignored.
    static_cast<void>(settings);
    return ledger.allocate();
}

// The pools share usable memory rather than splitting it (ADR 0037), so the ledger's configured
// total stays its own automatic total; it is not the sum of the pool ceilings.
runtime::MemoryBudgetAllocation cacheMemoryBudgetsFromSettings(const QSettings& settings) {
    return cacheMemoryBudgetAllocationFromSettings(settings, runtime::processMemoryBudgetLedger());
}

runtime::MemoryBudgetAllocation
applyCacheMemoryBudgetsFromSettings(const QSettings& settings, runtime::OperationCache& operations,
                                    PreviewFrameCache& preview,
                                    runtime::MemoryBudgetLedger& ledger) {
    const auto allocation = cacheMemoryBudgetAllocationFromSettings(settings, ledger);
    operations.setByteBudget(allocation.operationCacheByteBudget);
    preview.setByteBudget(allocation.previewFrameCacheByteBudget);
    return allocation;
}

std::size_t ramPreviewByteBudgetFromSettings(const QSettings& settings) {
    return cacheMemoryBudgetsFromSettings(settings).previewFrameCacheByteBudget;
}

std::size_t operationCacheByteBudgetFromSettings(const QSettings& settings) {
    return cacheMemoryBudgetsFromSettings(settings).operationCacheByteBudget;
}

} // namespace bloom::ui
