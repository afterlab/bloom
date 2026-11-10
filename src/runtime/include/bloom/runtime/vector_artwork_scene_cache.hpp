#pragma once

// #642: the parsed scenes of Vector Artwork layers' SVG data blocks, kept between compiles.
//
// The snapshot compiler expands every Vector Artwork node into the nodes of the editable SVG import
// on every compile (document::expandVectorArtwork). Parsing the block's bytes is the costly part,
// and the bytes of a block never change, so the parsed scene is kept here by the block's SHA-256
// and size: a new revision, another composition or a second layer with the same file reads it
// again without parsing. A block whose recorded digest does not match its bytes is parsed every
// time and never kept.
//
// Bounded: the scenes held are counted by an estimate of their size (anchors and elements) against
// a byte budget. A scene larger than a quarter of the budget is not admitted (it is parsed for the
// compile that asked and dropped with it); admitting one past the budget retires the least
// recently used scenes first. retire() drops every scene (project close, memory pressure, tests).
// Thread-safe; parsing happens outside the lock and is cancellable.
//
// Qt-free. See docs/architecture/vector-artwork.md, "Compiling a Vector Artwork layer".

#include <bloom/core/sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>

namespace bloom::document {
struct DataBlockRecord;
} // namespace bloom::document
namespace bloom::media {
struct SvgScene;
} // namespace bloom::media

namespace bloom::runtime {

inline constexpr std::size_t kDefaultVectorArtworkSceneCacheBytes = std::size_t{64} * 1024U * 1024U;

struct VectorArtworkSceneCacheStatistics final {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    // Parses that finished with a scene (cached or not).
    std::uint64_t parses = 0;
    // Scenes parsed but too large to keep.
    std::uint64_t refusedAdmissions = 0;
    // Scenes dropped to make room, or by retire().
    std::uint64_t retirements = 0;
    std::size_t entries = 0;
    std::size_t retainedBytes = 0;
    std::size_t byteBudget = 0;
};

class VectorArtworkSceneCache final {
  public:
    explicit VectorArtworkSceneCache(std::size_t byteBudget = kDefaultVectorArtworkSceneCacheBytes);
    VectorArtworkSceneCache(const VectorArtworkSceneCache&) = delete;
    VectorArtworkSceneCache& operator=(const VectorArtworkSceneCache&) = delete;

    struct Lookup final {
        std::shared_ptr<const media::SvgScene> scene;
        // Why there is no scene: the block is not an SVG, too large, or does not parse.
        std::string diagnostic;
        bool cancelled = false;
    };
    // The block's scene, parsed on a miss. Polls `cancelled` while parsing; a cancelled parse keeps
    // nothing.
    [[nodiscard]] Lookup scene(const document::DataBlockRecord& block,
                               const std::function<bool()>& cancelled = {});

    // Drops every scene held. Scenes a compile already holds stay alive with it.
    void retire();
    // A smaller budget retires least recently used scenes until the rest fit.
    void setByteBudget(std::size_t bytes);
    [[nodiscard]] VectorArtworkSceneCacheStatistics statistics() const;

    // What a parsed scene is counted as: its elements and anchors, not the file's bytes.
    [[nodiscard]] static std::size_t estimatedBytes(const media::SvgScene& scene) noexcept;

  private:
    struct Entry final {
        core::Sha256Digest digest;
        std::size_t payloadBytes = 0;
        std::size_t bytes = 0;
        std::shared_ptr<const media::SvgScene> scene;
    };
    void trimLocked();

    mutable std::mutex mutex_;
    std::size_t byteBudget_;
    // Most recently used first.
    std::list<Entry> entries_;
    VectorArtworkSceneCacheStatistics statistics_;
};

// The one the snapshot compiler uses.
[[nodiscard]] VectorArtworkSceneCache& processVectorArtworkSceneCache();

} // namespace bloom::runtime
