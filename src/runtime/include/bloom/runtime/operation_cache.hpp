#pragma once

#include <bloom/render/image.hpp>
#include <bloom/render/packed_host_retirement.hpp>
#include <bloom/runtime/compiled_plan.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>
#include <bloom/runtime/source_working_set.hpp>
#include <bloom/runtime/video_source_report.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bloom::media {
class ImageSourceRevision;
}

namespace bloom::runtime {
namespace detail {
class VideoSourceContext;
}

enum class OperationCacheEntryKind : std::uint8_t {
    Operation,
    DecodedMedia,
};

struct OperationCacheAccessStatistics final {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    // CACHEFIX-1: entries the cache refused because storing them threw std::bad_alloc. The insert
    // is dropped and the caller's frame still renders; this counter is how a test and a diagnostic
    // build can see that it happened at all.
    std::uint64_t allocationFailures = 0;
    // Entries dropped by a memory-pressure trim, as opposed to ordinary budget eviction.
    std::uint64_t pressureDrops = 0;
    // Stores a linear pass deferred because their key had not been offered before in the pass.
    std::uint64_t linearPassDeferrals = 0;

    friend bool operator==(const OperationCacheAccessStatistics&,
                           const OperationCacheAccessStatistics&) = default;
};

// What the cache holds on the host (docs/architecture/evaluation-primitives.md, "What the pools
// count"). Each distinct pixel or encoded-source allocation is charged once, at its real
// allocation size, however many entries reference it (an identity effect or a composition output
// publishes its input image again); entry metadata is charged per entry. Nothing here is device
// memory, and nothing here is a host copy of a GPU upload: staging is scratch (ADR 0037).
struct OperationCacheMemory final {
    // Everything allocated: retained entries plus entries pending retirement.
    std::size_t hostBytes = 0;
    // Evicted entries a retirement worker frees after the GPU work that reads them. Still
    // allocated, but already given back: never evicted or granted a second time.
    std::size_t pendingRetirementBytes = 0;
    // hostBytes by kind. Storage shared between kinds counts under the kind that stored it first.
    std::size_t operationBytes = 0;
    std::size_t decodedMediaBytes = 0;
    // Extra references to storage already charged: what per-entry accounting would add on top.
    std::size_t sharedReferenceBytes = 0;
    friend bool operator==(const OperationCacheMemory&, const OperationCacheMemory&) = default;
};

struct OperationCacheStatistics final {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::vector<document::NodeId> evaluatedNodes;
};
struct OperationCacheValue final {
    std::shared_ptr<const render::Rgba32fImage> image;
    std::vector<CompiledValue> values;
    EvaluatedOperationBounds bounds{};
    // Immutable encoded revision retained under the SAME decoded-media LRU/ledger. No pathname or
    // metadata-only entry can authorize hash elision; the actual verified decoder bytes are owned.
    std::shared_ptr<const media::ImageSourceRevision> sourceRevision{};
};

// Owned by one linear pass over a range (an export run) and carried by every frame it evaluates
// (EvaluationRequest::linearPass). A reusable-cache store made for the pass is retained only when
// its key was already offered earlier in the same pass, so frame-unique footage decodes and
// results stream through while static layers, held frames and looped sources stay cached. It
// remembers a bounded number of key hashes (metadata only, a few hundred KiB) and is thread-safe:
// a pass evaluates several frames at once.
class LinearPassRetention final {
  public:
    static constexpr std::size_t kRememberedKeys = 16384;
    // True when `key` was offered before within this pass; otherwise remembers it and returns
    // false. A hash collision only retains an entry, never changes pixels or cache identity.
    [[nodiscard]] bool offer(std::string_view key);

  private:
    std::mutex mutex_;
    std::unordered_set<std::size_t> offered_;
    std::deque<std::size_t> order_;
};

// Owned by the evaluator session. Concurrent preview/export tasks share this bounded LRU.
class OperationCache final {
  public:
    explicit OperationCache(std::size_t budget = defaultOperationCacheByteBudget(),
                            MemoryBudgetLedger& ledger = processMemoryBudgetLedger());
    ~OperationCache();
    // `kind` only attributes a miss to the Profiler row of the kind the caller looks for; a hit
    // counts under the found entry's kind.
    [[nodiscard]] std::optional<OperationCacheValue>
    find(const std::string& content, document::Revision revision,
         OperationCacheEntryKind kind = OperationCacheEntryKind::Operation);
    void store(std::string content, document::Revision revision, OperationCacheValue value,
               OperationCacheEntryKind kind = OperationCacheEntryKind::Operation,
               std::optional<std::uint64_t> decodedMediaGeneration = {},
               LinearPassRetention* linearPass = nullptr);
    void setByteBudget(std::size_t budget);
    [[nodiscard]] std::size_t byteBudget() const;
    // Host bytes allocated: retained plus pending retirement (OperationCacheMemory::hostBytes).
    // The ledger budgets only the retained part; the budget bounds hostBytes minus pending.
    [[nodiscard]] std::size_t retainedBytes() const;
    // CACHEFIX-1 accounting: the same total, split by what is holding it. Decoded media is the
    // expensive half (one 4K RGBA32F frame is 126.6 MiB), and it is the half a grace period keeps
    // alive, so an audit that cannot separate the two cannot say where the bytes went.
    [[nodiscard]] std::size_t retainedBytes(OperationCacheEntryKind kind) const;
    [[nodiscard]] OperationCacheMemory memory() const;
    // Explicit one-shot trim without changing admission. The ledger pressure callback separately
    // lowers admission and evicts atomically; configured ceilings remain in the ledger.
    void trimToBytes(std::size_t bytes);
    // "Purge preview cache": removes every derived OPERATION entry, leaving decoded-media entries
    // alone. A consumer that already holds a value keeps it alive -- a value is a shared_ptr -- so
    // clearing here never invalidates work already in progress; it only forces the next request to
    // re-evaluate. Lifetime access counters are left intact.
    void clearOperations();
    // "Purge media cache": removes every DECODED-MEDIA entry, leaving derived operation results
    // alone. The same shared_ptr rule applies: no live product is invalidated, only re-derived.
    void clearDecodedMedia();
    // New/Open/refresh authority boundary. Invalidates captured source tickets only, preserving
    // decoded/derived pixels and disk cache. Rooted backing/session destruction stays on workers.
    void invalidateSourceRevisions();
    // Source capture records this before I/O and passes it to store(). A concurrent refresh may
    // leave the in-flight immutable snapshot valid, but prevents it repopulating source authority.
    [[nodiscard]] std::uint64_t decodedMediaGeneration() const;
    [[nodiscard]] OperationCacheAccessStatistics statistics() const;

    // Roots this pool on an explicit host retirement owner: evicted entries are freed by its
    // worker after the GPU work that may read them. A second binding to the same owner is a no-op;
    // another owner throws.
    void bindHostRetirement(const std::shared_ptr<render::PackedHostRetirementOwner>& owner);
    // Irreversible producer close, serialized with insertion: drops every entry so the host owner
    // can acknowledge shutdown, refuses later stores and begins video-source shutdown.
    void close();
    [[nodiscard]] std::shared_ptr<render::detail::PackedHostRetirementState>
    sourceRetirementState() const;
    // Typed weak producer attachment: this pool's source close also detaches video sessions.
    void bindVideoSources(const std::shared_ptr<detail::VideoSourceContext>& sources);
    // The bound video sources' decode route and colour-note reports (most recent first).
    [[nodiscard]] std::vector<VideoSourceReport> videoSourceReports() const;
    // The playback working set this cache's source entries follow (ADR 0037, rule 3): decoded
    // media entries outside it are released after each render request; Operation entries are not
    // affected. The evaluator's requests and its decoded-video cache share it.
    [[nodiscard]] const std::shared_ptr<SourceWorkingSet>& sourceWindow() const noexcept {
        return sourceWindow_;
    }
    // Releases decoded-media entries outside the window now (the working set calls this).
    void releaseSourcesOutsideWindow();

  private:
    static constexpr std::uint64_t kDecodedMediaGraceAccesses = 8;
    struct Accounting;
    struct Key {
        std::string_view content;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            return std::hash<std::string_view>{}(key.content);
        }
    };
    struct Address {
        document::Revision revision;
        Key key;
        friend bool operator==(const Address&, const Address&) = default;
    };
    struct AddressHash {
        std::size_t operator()(const Address& address) const noexcept {
            return KeyHash{}(address.key) ^ std::hash<std::uint64_t>{}(address.revision.value());
        }
    };
    // One reference from an entry to shared storage: the image's pixels or an encoded source.
    struct StorageReference {
        const void* key = nullptr;
        std::size_t bytes = 0;
    };
    struct Entry : render::detail::PackedHostRetirementNode {
        ~Entry();
        std::string content;
        document::Revision revision;
        OperationCacheValue value;
        // Charged per entry: the entry, its key and its values. Storage is charged once.
        std::size_t metadataBytes = 0;
        std::array<StorageReference, 2> storage{};
        // What this entry would cost alone: metadata plus every storage it references.
        [[nodiscard]] std::size_t standaloneBytes() const noexcept;
        OperationCacheEntryKind kind = OperationCacheEntryKind::Operation;
        std::uint64_t protectedUntil = 0;
        // Where a decoded-media entry was last used (the playback working set).
        SourceUse use{};
        bool registered = false;
        std::shared_ptr<Accounting> accounting;
        std::shared_ptr<render::detail::PackedHostRetirementState> retirement;
    };
    using Entries = std::list<std::unique_ptr<Entry>>;
    using Position = Entries::iterator;
    static void destroySourceRevisionEntry(render::detail::PackedHostRetirementNode*) noexcept;
    static void destroyOrdinaryEntry(render::detail::PackedHostRetirementNode*) noexcept;
    void touch(Position entry);
    // Tags a decoded-media entry with the calling thread's source use, when it has one.
    static void retag(Entry& entry) noexcept;
    void evictToLocked(std::size_t limit, std::uint64_t& counter);
    void removeLocked(Position candidate);
    // Returns the removed entries' standalone charge.
    std::size_t removeKindLocked(OperationCacheEntryKind kind);
    [[nodiscard]] Position evictionCandidate();
    void insertLocked(std::unique_ptr<Entry>& entry);

    MemoryBudgetLedger& ledger_;
    std::shared_ptr<SourceWorkingSet> sourceWindow_ = std::make_shared<SourceWorkingSet>();
    std::shared_ptr<Accounting> accounting_;
    std::uint64_t accessEpoch_ = 0;
    OperationCacheAccessStatistics statistics_;
    Entries entries_;
    std::unordered_map<Key, Position, KeyHash> index_;
    std::unordered_map<Address, Position, AddressHash> addresses_;
};
} // namespace bloom::runtime
