#include "packed_host_retirement_private.hpp"

#include <bloom/core/pixel_aspect_ratio.hpp>
#include <bloom/core/telemetry_counters.hpp>
#include <bloom/render/image.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/operation_cache.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// What the operation cache charges to the memory ledger (#416): each distinct allocation once at
// its real size, evicted entries pending asynchronous retirement exactly once, and never more host
// memory than the process actually allocated for what it retains.

// Every heap allocation in this binary is counted, so a fill can be compared with what the pools
// report. A header in front of each block remembers its size.
namespace {
std::atomic<std::int64_t> gLiveHeapBytes{0};
constexpr std::size_t kHeader = alignof(std::max_align_t);

void* countedAllocate(const std::size_t size, const std::size_t alignment) {
    const auto header = alignment > kHeader ? alignment : kHeader;
    void* block = nullptr;
    if (alignment > kHeader) {
        const auto total = (header + size + alignment - 1) / alignment * alignment;
        block = std::aligned_alloc(alignment, total);
    } else {
        block = std::malloc(header + size);
    }
    if (block == nullptr)
        throw std::bad_alloc();
    auto* const user = static_cast<unsigned char*>(block) + header;
    reinterpret_cast<std::size_t*>(user)[-1] = size;
    reinterpret_cast<std::size_t*>(user)[-2] = header;
    gLiveHeapBytes.fetch_add(static_cast<std::int64_t>(size), std::memory_order_relaxed);
    return user;
}

void countedFree(void* pointer) noexcept {
    if (pointer == nullptr)
        return;
    auto* const user = static_cast<unsigned char*>(pointer);
    const auto size = reinterpret_cast<std::size_t*>(user)[-1];
    const auto header = reinterpret_cast<std::size_t*>(user)[-2];
    gLiveHeapBytes.fetch_sub(static_cast<std::int64_t>(size), std::memory_order_relaxed);
    std::free(user - header);
}
} // namespace

void* operator new(const std::size_t size) { return countedAllocate(size, kHeader); }
void* operator new[](const std::size_t size) { return countedAllocate(size, kHeader); }
void* operator new(const std::size_t size, const std::align_val_t alignment) {
    return countedAllocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](const std::size_t size, const std::align_val_t alignment) {
    return countedAllocate(size, static_cast<std::size_t>(alignment));
}
void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return countedAllocate(size, kHeader);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return countedAllocate(size, kHeader);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* pointer) noexcept { countedFree(pointer); }
void operator delete[](void* pointer) noexcept { countedFree(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { countedFree(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { countedFree(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { countedFree(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { countedFree(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    countedFree(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    countedFree(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept { countedFree(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { countedFree(pointer); }

namespace {
using bloom::render::ImageWindow;
using bloom::render::Rgba32fImage;
using bloom::render::Rgba32fImageBuilder;
using bloom::render::Rgba32fImageDescriptor;
using bloom::runtime::MemoryBudgetLedger;
using bloom::runtime::OperationCache;
using bloom::runtime::OperationCacheEntryKind;
using bloom::runtime::OperationCacheValue;

int failures = 0;
void expect(const bool condition, const std::string_view message,
            const std::source_location location = std::source_location::current()) {
    if (condition)
        return;
    ++failures;
    std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
}

std::shared_ptr<const Rgba32fImage> makeImage(const std::uint32_t width,
                                              const std::uint32_t height) {
    const auto window = ImageWindow::create(0, 0, width, height);
    const auto descriptor = Rgba32fImageDescriptor::create(*window.value(), *window.value(),
                                                           bloom::core::PixelAspectRatio::square());
    auto builder = Rgba32fImageBuilder::create(*descriptor.value(), 1U << 30U);
    auto frozen = std::move(*builder.value()).freeze();
    return std::make_shared<const Rgba32fImage>(std::move(*frozen.value()));
}

std::size_t storageBytes(const Rgba32fImage& image) {
    return sizeof(Rgba32fImage) + image.pixelStorageByteCost();
}

OperationCacheValue imageValue(std::shared_ptr<const Rgba32fImage> image) {
    return {.image = std::move(image), .values = {}, .bounds = {}};
}

// The worker's free can be held, so evicted entries stay pending retirement until released.
struct RetirementScope final {
    std::shared_ptr<bloom::render::PackedHostRetirementOwner> owner =
        bloom::render::PackedHostRetirementOwner::create();
    std::mutex mutex;
    std::condition_variable wake;
    bool blocked = false;
    RetirementScope() {
        auto state = owner->producerState();
        state->testContext = this;
        state->beforeFree = [](void* context) noexcept {
            auto& self = *static_cast<RetirementScope*>(context);
            std::unique_lock lock(self.mutex);
            self.wake.wait(lock, [&] { return !self.blocked; });
        };
    }
    void release() {
        {
            const std::lock_guard lock(mutex);
            blocked = false;
        }
        wake.notify_all();
    }
    ~RetirementScope() {
        release();
        owner->beginShutdown();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!owner->exitAcknowledged() && std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        if (!owner->exitAcknowledged() || !owner->collect())
            std::terminate();
    }
};

template <class Predicate> bool until(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < end)
        std::this_thread::yield();
    return predicate();
}

// An identity effect and a composition output publish their input image again: the pixels are
// charged once and freed with their last reference.
void sharedStorageIsChargedOnce() {
    MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    OperationCache cache(1U << 30U, ledger);
    const auto image = makeImage(64, 64);
    const auto pixels = storageBytes(*image);
    cache.store("source", {}, imageValue(image));
    const auto one = cache.retainedBytes();
    cache.store("identity", {}, imageValue(image));
    cache.store("output", {}, imageValue(image));
    const auto three = cache.memory();
    const auto metadata = (three.hostBytes - pixels) / 3;
    expect(one > pixels && one < pixels + pixels / 8, "one entry charges its pixels and metadata");
    expect(three.hostBytes < pixels + pixels / 4 && three.operationBytes == three.hostBytes,
           "three entries publishing one image charge its pixels once");
    expect(three.sharedReferenceBytes == 2 * pixels,
           "the two extra references are reported, not charged");
    expect(ledger.reusableRetainedBytes() == three.hostBytes &&
               ledger.reusableHolding().hostBytes == three.hostBytes,
           "the ledger sees the same once-counted host bytes");
    // Evict the oldest reference only: the image is still retained by the other two.
    cache.trimToBytes(three.hostBytes - metadata / 2);
    expect(!cache.find("source", {}) && cache.find("output", {}).has_value(),
           "the trim evicts the least recently used reference");
    const auto two = cache.memory();
    expect(two.hostBytes > pixels && two.sharedReferenceBytes == pixels,
           "evicting one reference keeps the shared pixels charged");
    cache.clearOperations();
    expect(cache.retainedBytes() == 0 && cache.memory().sharedReferenceBytes == 0,
           "the last reference returns the pixels");
    // Distinct images of the same size are distinct allocations.
    cache.store("a", {}, imageValue(makeImage(64, 64)));
    cache.store("b", {}, imageValue(makeImage(64, 64)));
    expect(cache.retainedBytes() > 2 * pixels && cache.memory().sharedReferenceBytes == 0,
           "distinct allocations are each charged");
}

// Evicted entries in a pool with an explicit host owner free on the retirement worker after
// their GPU work. Until then their bytes are allocated host memory (counted in the host total)
// but already given back (not reclaimable a second time); a store of the same image meanwhile
// brings them back without charging them twice.
void pendingRetirementIsNeitherLostNorDoubled() {
    RetirementScope scope;
    scope.blocked = true;
    MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    auto pool = std::make_shared<OperationCache>(1U << 30U, ledger);
    pool->bindHostRetirement(scope.owner);
    auto image = makeImage(64, 64);
    const std::weak_ptr<const Rgba32fImage> backing = image;
    const auto pixels = storageBytes(*image);
    pool->store("decoded", {}, imageValue(image), OperationCacheEntryKind::DecodedMedia,
                pool->decodedMediaGeneration());
    pool->store("operation", {}, imageValue(image));
    const auto charged = pool->memory();
    expect(charged.decodedMediaBytes > pixels && charged.operationBytes < pixels / 8,
           "decoded media and an operation share one charge, under the kind that stored it first");

    pool->clearDecodedMedia();
    const auto decodedEvicted = pool->memory();
    expect(decodedEvicted.hostBytes == charged.hostBytes &&
               decodedEvicted.pendingRetirementBytes > 0 &&
               decodedEvicted.pendingRetirementBytes < pixels / 8,
           "an evicted reference to pixels another entry retains is pending only its metadata");

    pool->clearOperations();
    const auto allEvicted = pool->memory();
    const auto holding = ledger.reusableHolding();
    expect(allEvicted.hostBytes == charged.hostBytes &&
               allEvicted.pendingRetirementBytes == allEvicted.hostBytes && !backing.expired(),
           "with every reference retiring, nothing is lost while the pixels are still allocated");
    expect(holding.hostBytes == 0 && holding.hostPendingRetirementBytes == allEvicted.hostBytes &&
               ledger.reusableRetainedBytes() == allEvicted.hostBytes,
           "the ledger counts pending bytes as allocated host memory, never as reclaimable");

    pool->store("again", {}, imageValue(image));
    const auto revived = pool->memory();
    expect(revived.hostBytes > allEvicted.hostBytes &&
               revived.hostBytes < allEvicted.hostBytes + pixels / 8 &&
               revived.pendingRetirementBytes < pixels / 8,
           "storing pending pixels again adds only metadata and takes them out of pending");
    image.reset();

    scope.release();
    expect(until([&] { return pool->memory().pendingRetirementBytes == 0; }),
           "the worker returns every pending charge once it frees");
    const auto settled = pool->memory();
    expect(settled.hostBytes > pixels && settled.hostBytes < pixels + pixels / 8 &&
               settled.sharedReferenceBytes == 0 && !backing.expired(),
           "after retirement only the live entry remains charged, its pixels once");
    pool->clearOperations();
    expect(until([&] { return pool->retainedBytes() == 0 && backing.expired(); }),
           "the last reference frees the pixels and returns the charge");
}

// A synthetic fill shaped like a CPU fallback frame: a decoded source, an identity effect and the
// composition output share one image; a processed image is retained once (no upload copy is kept,
// ADR 0037). The budget forces eviction and part of it retires on a held worker. At every step
// what the ledger counts as host memory never exceeds what the process holds allocated for it.
void ledgerHostTotalsNeverExceedAllocation() {
    RetirementScope scope;
    MemoryBudgetLedger ledger(64ULL << 30U, 48ULL << 30U);
    const auto frameBytes = storageBytes(*makeImage(128, 128));
    const auto baseline = gLiveHeapBytes.load();
    std::int64_t perEntryExcess = std::numeric_limits<std::int64_t>::min();
    {
        auto pool = std::make_shared<OperationCache>(12 * frameBytes, ledger);
        pool->bindHostRetirement(scope.owner);
        bool overcounted = false;
        bool anyShared = false;
        for (int frame = 0; frame < 40; ++frame) {
            if (frame == 20) {
                const std::lock_guard lock(scope.mutex);
                scope.blocked = true;
            }
            const auto tag = std::to_string(frame);
            {
                auto decoded = makeImage(128, 128);
                pool->store("decoded-" + tag, {}, imageValue(decoded),
                            OperationCacheEntryKind::DecodedMedia, pool->decodedMediaGeneration());
                pool->store("source-" + tag, {}, imageValue(decoded));
                pool->store("identity-" + tag, {}, imageValue(decoded));
                pool->store("output-" + tag, {}, imageValue(decoded));
                auto processed = makeImage(128, 128);
                pool->store("processed-" + tag, {}, imageValue(processed));
            }
            // A free returns its charge just after the memory (never before), so measure while
            // the worker is idle: drained before the hold, held after it.
            if (frame < 20)
                static_cast<void>(
                    until([&] { return pool->memory().pendingRetirementBytes == 0; }));
            const auto memory = pool->memory();
            const auto allocated = gLiveHeapBytes.load() - baseline;
            const auto host = ledger.reusableRetainedBytes();
            overcounted = overcounted || static_cast<std::int64_t>(host) > allocated;
            anyShared = anyShared || memory.sharedReferenceBytes > 0;
            perEntryExcess =
                std::max(perEntryExcess,
                         static_cast<std::int64_t>(memory.hostBytes + memory.sharedReferenceBytes) -
                             allocated);
            if (frame == 30)
                expect(memory.pendingRetirementBytes > 0,
                       "the held worker leaves evicted bytes pending retirement");
        }
        expect(!overcounted, "the ledger's host total never exceeds the pool's live allocation");
        expect(anyShared && perEntryExcess > 0,
               "per-entry accounting would have counted more than was allocated");
        scope.release();
        expect(until([&] { return pool->memory().pendingRetirementBytes == 0; }),
               "held retirement drains");
        expect(static_cast<std::int64_t>(ledger.reusableRetainedBytes()) <=
                   gLiveHeapBytes.load() - baseline,
               "after retirement the host total is still within the allocation");
        pool->close();
    }
    expect(until([&] { return ledger.reusableRetainedBytes() == 0; }),
           "a destroyed pool leaves nothing on the ledger");
}
} // namespace

// The Profiler's per-kind rows (#423): hits under the found entry's kind, misses under the kind the
// caller looked for, stores, evictions with their bytes, and refusals over the budget, read
// through the ledger without the cache's lock order changing.
void profilerRowsCountPerKind() {
    namespace telemetry = bloom::core::telemetry;
    const auto causeBytes = [](const telemetry::EvictionCause cause) {
        return telemetry::evictedBytesByCause()[static_cast<std::size_t>(cause)];
    };
    const auto ceilingBefore = causeBytes(telemetry::EvictionCause::Ceiling);
    MemoryBudgetLedger ledger(16ULL << 30U);
    const auto image = makeImage(16, 16);
    const auto entry = storageBytes(*image);
    OperationCache cache(entry * 4, ledger);
    cache.store("a", {}, imageValue(image));
    cache.store("b", {}, imageValue(makeImage(16, 16)), OperationCacheEntryKind::DecodedMedia);
    expect(cache.find("a", {}).has_value(), "a stored operation is found");
    expect(cache.find("b", {}).has_value(), "a stored decoded frame is found");
    expect(!cache.find("missing", {}).has_value(), "an operation miss");
    expect(!cache.find("missing", {}, OperationCacheEntryKind::DecodedMedia).has_value(),
           "a decoded-media miss");
    cache.store("huge", {}, imageValue(makeImage(64, 64)));
    for (int index = 0; index < 6; ++index)
        cache.store("fill" + std::to_string(index), {}, imageValue(makeImage(16, 16)));

    std::array<bloom::runtime::ProfilerPool, 4> rows{};
    std::uint32_t dropped = 7;
    const auto count = ledger.copyProfilerPools(rows, dropped);
    expect(count == 2 && dropped == 0, "the cache is two rows");
    const auto& operations = rows[0];
    const auto& decoded = rows[1];
    expect(bloom::runtime::profilerNameView(operations.name) == "Operation cache: operations" &&
               bloom::runtime::profilerNameView(decoded.name) == "Operation cache: decoded media",
           "rows are named by kind");
    expect(operations.hits.valueOr(0) == 1 && decoded.hits.valueOr(0) == 1, "hits per kind");
    expect(operations.misses.valueOr(0) == 1 && decoded.misses.valueOr(0) == 1,
           "misses under the kind looked for");
    expect(operations.stores.valueOr(0) == 7 && decoded.stores.valueOr(0) == 1, "stores per kind");
    expect(operations.refusals.valueOr(0) == 1, "the over-budget store is refused");
    // Four entries' storage is the budget and each entry also charges its metadata, so at most
    // three of the eight stored entries stay.
    const auto evictions = operations.evictions.valueOr(0) + decoded.evictions.valueOr(0);
    expect(evictions >= 5 && evictions <= 8, "filling past the budget evicts");
    const auto evictedBytes = operations.evictedBytes.valueOr(0) + decoded.evictedBytes.valueOr(0);
    expect(evictedBytes >= evictions * entry && evictedBytes < evictions * (entry + 4096),
           "evicted bytes are the entries' standalone charges");
    expect(operations.ceilingBytes.valueOr(0) == entry * 4, "the cache's one ceiling");
    const auto memory = cache.memory();
    expect(operations.hostBytes + operations.pendingBytes + decoded.hostBytes +
                   decoded.pendingBytes ==
               memory.hostBytes,
           "the rows add up to the cache's host bytes");
    // Entries now: every stored entry not yet evicted, per kind.
    expect(operations.entries.present && decoded.entries.present &&
               operations.entries.value ==
                   operations.stores.valueOr(0) - operations.evictions.valueOr(0) &&
               decoded.entries.value == decoded.stores.valueOr(0) - decoded.evictions.valueOr(0),
           "each row counts its retained entries");
    // A store evicting past the budget is the Ceiling cause; a purge is the Purge cause.
    expect(causeBytes(telemetry::EvictionCause::Ceiling) - ceilingBefore == evictedBytes,
           "evictions on a store are attributed to the pool's ceiling");
    const auto purgedBefore = causeBytes(telemetry::EvictionCause::Purge);
    const auto retainedEntries = operations.entries.value;
    cache.clearOperations();
    expect(causeBytes(telemetry::EvictionCause::Purge) - purgedBefore >= retainedEntries * entry,
           "a purge counts the cleared entries' standalone charges");
    static_cast<void>(ledger.copyProfilerPools(rows, dropped));
    expect(rows[0].entries.valueOr(1) == 0 && rows[1].entries.value == decoded.entries.value,
           "a purge of operations leaves no operation entries and keeps decoded media");
    expect(rows[0].evictions.valueOr(0) == operations.evictions.valueOr(0),
           "a purge is not a pool eviction");
}

// ADR 0037 rule 3: decoded-media entries hold the playback window. Behind the oldest request in
// flight they are released; an entry a request in flight still needs is kept; Operation entries
// and entries used outside any request keep the ordinary retention.
void sourceEntriesFollowThePlaybackWindow() {
    using bloom::runtime::SourceWorkingSet;
    MemoryBudgetLedger ledger(16ULL << 30U);
    OperationCache cache(64ULL << 20U, ledger);
    const auto window = cache.sourceWindow();
    constexpr std::uint64_t kComposition = 5;
    const auto storeAt = [&](const std::int64_t frame) {
        const SourceWorkingSet::Request request(window, kComposition, frame);
        cache.store("source:" + std::to_string(frame), {}, imageValue(makeImage(8, 8)),
                    OperationCacheEntryKind::DecodedMedia);
        cache.store("result:" + std::to_string(frame), {}, imageValue(makeImage(8, 8)));
    };
    const auto has = [&](const std::string& key) {
        // A find outside any request does not re-tag the entry.
        return cache.find(key, {}, OperationCacheEntryKind::DecodedMedia).has_value();
    };
    cache.store("untracked", {}, imageValue(makeImage(8, 8)),
                OperationCacheEntryKind::DecodedMedia);
    for (std::int64_t frame = 0; frame <= 20; ++frame)
        storeAt(frame);
    expect(!has("source:0") && !has("source:19"), "sources behind the playhead are released");
    expect(has("source:20"), "the playhead's source stays while paused");
    expect(has("result:0") && has("result:19"), "Operation entries are not affected");
    expect(has("untracked"), "a source used outside any request keeps ordinary retention");

    // A request in flight at frame 21, on its own thread as a render task is, keeps its source
    // while later frames render and end.
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool stored = false;
        bool release = false;
        std::thread renderer([&] {
            const SourceWorkingSet::Request inFlight(window, kComposition, 21);
            cache.store("source:21", {}, imageValue(makeImage(8, 8)),
                        OperationCacheEntryKind::DecodedMedia);
            std::unique_lock lock(mutex);
            stored = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        });
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [&] { return stored; });
        }
        for (std::int64_t frame = 22; frame <= 40; ++frame)
            storeAt(frame);
        expect(has("source:21") && has("source:30"),
               "entries a request in flight still needs are kept, with everything after it");
        {
            const std::lock_guard lock(mutex);
            release = true;
        }
        changed.notify_all();
        renderer.join();
    }
    expect(!has("source:21") && !has("source:39") && has("source:40"),
           "once the request ends its frame leaves the window");

    // Ahead: captures for the next frames (read-ahead) stay inside the read-ahead width.
    {
        const SourceWorkingSet::UseScope ahead(
            {kComposition, 40 + SourceWorkingSet::kReadAheadFrames, true});
        cache.store("ahead:inside", {}, imageValue(makeImage(8, 8)),
                    OperationCacheEntryKind::DecodedMedia);
    }
    {
        const SourceWorkingSet::UseScope ahead(
            {kComposition, 41 + SourceWorkingSet::kReadAheadFrames, true});
        cache.store("ahead:outside", {}, imageValue(makeImage(8, 8)),
                    OperationCacheEntryKind::DecodedMedia);
    }
    window->trim();
    expect(has("ahead:inside"), "a capture inside the read-ahead width stays");
    expect(!has("ahead:outside"), "a capture beyond it is released");

    // Composition frames come from the request time at the composition rate.
    expect(SourceWorkingSet::frameAt(1001, 24000, 24000, 1001) == 1 &&
               SourceWorkingSet::frameAt(-1, 48, 24, 1) == -1,
           "composition frames floor the time");
    std::array<bloom::runtime::ProfilerPool, 4> rows{};
    std::uint32_t dropped = 0;
    const auto count = ledger.copyProfilerPools(rows, dropped);
    expect(count == 2 && rows[1].evictions.valueOr(0) >= 40,
           "window releases count as decoded-media evictions");
}

// #462: a composition keeps a playback window while it is in use, not for the life of the cache.
// An idle composition's window is dropped when another composition starts rendering; one with a
// request in flight keeps it.
void playbackWindowsFollowTheCompositionsInUse() {
    using bloom::runtime::SourceWorkingSet;
    const auto set = std::make_shared<SourceWorkingSet>();
    for (std::uint64_t composition = 1; composition <= 100; ++composition)
        const SourceWorkingSet::Request request(set, composition, 0);
    expect(set->windowCount() == 1, "only the composition rendered last keeps a window");
    {
        const SourceWorkingSet::Request first(set, 1, 0);
        const SourceWorkingSet::Request second(set, 2, 0);
        expect(set->windowCount() == 2, "a composition with a request in flight keeps its window");
    }
    expect(set->windowCount() == 2, "the window outlives its last request until another begins");
}

int main() try {
    sharedStorageIsChargedOnce();
    pendingRetirementIsNeitherLostNorDoubled();
    ledgerHostTotalsNeverExceedAllocation();
    profilerRowsCountPerKind();
    sourceEntriesFollowThePlaybackWindow();
    playbackWindowsFollowTheCompositionsInUse();
    if (failures != 0) {
        std::cerr << "FAIL: operation cache accounting expectations failed\n";
        return 1;
    }
    std::cout << "operation cache accounting: all checks passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
