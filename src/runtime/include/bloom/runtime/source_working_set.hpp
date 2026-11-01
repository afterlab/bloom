#pragma once

// The playback working set of decoded sources (ADR 0037, rule 3): source caches (decoded video
// planes and image-sequence source revisions) hold the window around the playhead, not every frame
// played. Finished frames are not affected; they cover replays.
//
// Each render request registers itself for its lifetime with a Request scope naming its
// composition and frame. A composition's window spans its requests in flight and the most recently
// started one, plus kReadAheadFrames frames ahead (the image-sequence read-ahead's width). The
// scope also marks the calling thread, so a source entry stored or found while the request runs is
// tagged with that composition frame (SourceUse); an entry used outside any request is untracked
// and keeps the cache's ordinary retention. When a request ends, the window moves and every bound
// cache releases its tracked entries that fall outside it.
//
// Lock order: the trimmer mutex, then a cache's own lock, then the window state. A cache never
// calls into the working set while holding its lock except retains() (state only), and a request
// never ends while its thread holds a cache lock.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace bloom::runtime {

// Where a source entry was last used: a composition frame, or untracked.
struct SourceUse final {
    std::uint64_t composition = 0;
    std::int64_t frame = 0;
    bool tracked = false;
};

class SourceWorkingSet final : public std::enable_shared_from_this<SourceWorkingSet> {
  public:
    // Composition frames ahead of a request that stay in the window: the image-sequence
    // read-ahead's width, which captures exactly that many frames ahead.
    static constexpr std::int64_t kReadAheadFrames = 8;

    // One render request in flight. Marks the calling thread while alive (the outermost scope on a
    // thread wins, so a nested build joins its parent's request). Move-only; ends on destruction.
    class Request final {
      public:
        Request() noexcept = default;
        Request(std::shared_ptr<SourceWorkingSet> owner, std::uint64_t composition,
                std::int64_t frame);
        Request(Request&& other) noexcept;
        Request& operator=(Request&& other) noexcept;
        Request(const Request&) = delete;
        Request& operator=(const Request&) = delete;
        ~Request() { end(); }
        void end() noexcept;

      private:
        std::shared_ptr<SourceWorkingSet> owner_;
        SourceUse use_{};
        bool marksThread_ = false;
    };

    // Marks the calling thread with `use` (a read-ahead capture on its own worker) without
    // registering a request.
    class UseScope final {
      public:
        explicit UseScope(SourceUse use) noexcept;
        ~UseScope();
        UseScope(const UseScope&) = delete;
        UseScope& operator=(const UseScope&) = delete;

      private:
        SourceUse previous_{};
    };

    // The composition frame (floor) of a time numerator/denominator at a frame rate.
    [[nodiscard]] static std::int64_t frameAt(std::int64_t timeNumerator,
                                              std::int64_t timeDenominator,
                                              std::uint32_t rateNumerator,
                                              std::uint32_t rateDenominator) noexcept;

    // The calling thread's use: its outermost request or use scope, else untracked.
    [[nodiscard]] static SourceUse current() noexcept;

    // Whether an entry used at `use` is in its composition's window. Untracked uses and
    // compositions without a request yet are always retained.
    [[nodiscard]] bool retains(const SourceUse& use) const;

    // A bound cache's release pass, run after every request ends (never while a request runs on
    // the calling thread's stack holding a cache lock). Removal waits for a running pass.
    void addTrimmer(const void* owner, std::function<void()> trim);
    void removeTrimmer(const void* owner);
    // Runs every trimmer now (tests, and after a window change outside a request).
    void trim();

  private:
    struct Window final {
        std::int64_t latest = 0;
        std::multiset<std::int64_t> inFlight;
    };
    void begin(const SourceUse& use);
    void finish(const SourceUse& use);

    mutable std::mutex mutex_;
    std::map<std::uint64_t, Window> windows_;
    std::mutex trimMutex_;
    std::vector<std::pair<const void*, std::function<void()>>> trimmers_;
};

} // namespace bloom::runtime
