// bloom.media.source-mirror (#718): the local mirror of source media on slow storage. Covers the
// metadata identity check, the local-copy policy, write-through and its queue, eviction,
// corruption, restart, cancellation and shutdown. No test here reads a source through the mirror:
// the mirror never opens a source, which the tests prove by deleting it.
#include <bloom/core/sha256.hpp>
#include <bloom/media/cache/source_mirror.hpp>
#include <bloom/platform/source_identity.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace cache = bloom::media::cache;
namespace platform = bloom::platform;
using platform::StorageClass;

int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

struct Scratch final {
    std::filesystem::path root;
    explicit Scratch(const std::string& name)
        : root(std::filesystem::temp_directory_path() /
               ("bloom-source-mirror-" + name + "-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(root / "sources");
        std::filesystem::create_directories(root / "cache");
    }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    [[nodiscard]] std::filesystem::path cacheRoot() const { return root / "cache"; }
};

using Bytes = std::vector<std::byte>;
[[nodiscard]] std::shared_ptr<const Bytes> makeBytes(const std::size_t count, const unsigned seed) {
    auto bytes = std::make_shared<Bytes>(count);
    for (std::size_t i = 0; i < count; ++i)
        (*bytes)[i] = static_cast<std::byte>((i * 31U + seed * 17U + (i >> 8U)) & 0xFFU);
    return bytes;
}
// Writes a source file and returns its bytes with the identity the file system reports.
struct Source final {
    std::filesystem::path path;
    std::shared_ptr<const Bytes> bytes;
    platform::FileIdentity identity;
};
[[nodiscard]] Source writeSource(const Scratch& scratch, const std::string& name,
                                 const std::size_t count, const unsigned seed) {
    Source source{scratch.root / "sources" / name, makeBytes(count, seed), {}};
    {
        std::ofstream out(source.path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(source.bytes->data()),
                  static_cast<std::streamsize>(source.bytes->size()));
    }
    const auto identity = platform::observeFileIdentity(source.path);
    expect(identity.has_value(), "a written source has an identity");
    if (identity)
        source.identity = *identity;
    return source;
}
void keep(cache::SourceMirror& mirror, const Source& source) {
    mirror.keep(source.path, source.identity, source.bytes,
                std::span<const std::byte>(*source.bytes));
}
[[nodiscard]] cache::SourceMirrorConfig config(const Scratch& scratch) {
    cache::SourceMirrorConfig value;
    value.rootDirectory = scratch.cacheRoot();
    return value;
}

void testIdentity() {
    const Scratch scratch("identity");
    cache::SourceMirror mirror(config(scratch));
    const auto source = writeSource(scratch, "frame.0001.exr", 70'000, 1);
    expect(!mirror.find(source.path, source.identity, {}), "nothing is mirrored before a read");
    keep(mirror, source);
    mirror.flush();

    // Unchanged: the exact bytes come back, with the digest of those bytes.
    const auto hit = mirror.find(source.path, source.identity, {});
    expect(hit && hit->bytes == *source.bytes, "an unchanged source is served from the mirror");
    expect(hit && hit->digest == *bloom::core::Sha256Hasher::hash(*source.bytes),
           "a hit carries the SHA-256 of the mirrored bytes");
    // The device number and the status-change time are not part of the identity: a network
    // mount is given a new device number each time, and a rename moves the change time.
    auto remounted = source.identity;
    remounted.volume += 7;
    remounted.changedNanoseconds += 1'000'000;
    expect(mirror.find(source.path, remounted, {}).has_value(),
           "a new device number or status-change time alone still hits");
    // The mirror never opens the source: with the file gone, the identity still finds the bytes.
    std::filesystem::remove(source.path);
    expect(mirror.find(source.path, source.identity, {}).has_value(),
           "a lookup never touches the source file");

    const auto changed = [&](const char* what, const auto& edit) {
        keep(mirror, source);
        mirror.flush();
        auto identity = source.identity;
        edit(identity);
        const auto before = mirror.statistics();
        expect(!mirror.find(source.path, identity, {}).has_value(),
               std::string("a changed ") + what + " misses");
        const auto after = mirror.statistics();
        expect(after.staleDropped == before.staleDropped + 1,
               std::string("a changed ") + what + " drops the source's own entry");
        expect(!std::filesystem::exists(mirror.entryPathForTest(source.path)),
               std::string("a changed ") + what + " removes the stale file");
        expect(!mirror.find(source.path, source.identity, {}).has_value(),
               std::string("after a changed ") + what + " the old identity misses too");
    };
    changed("size", [](platform::FileIdentity& identity) { identity.size += 1; });
    changed("modification time",
            [](platform::FileIdentity& identity) { identity.modifiedNanoseconds += 1; });
    changed("file id", [](platform::FileIdentity& identity) { identity.fileIdLow += 1; });
    changed("file id (high half)",
            [](platform::FileIdentity& identity) { identity.fileIdHigh += 1; });

    // A real rewrite of the file changes what the file system reports, and so misses.
    const auto original = writeSource(scratch, "plate.png", 4'000, 2);
    keep(mirror, original);
    mirror.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto rewritten = writeSource(scratch, "plate.png", 4'100, 3);
    expect(!mirror.find(rewritten.path, rewritten.identity, {}).has_value(),
           "a rewritten source is not served from its old mirror");
    keep(mirror, rewritten);
    mirror.flush();
    const auto fresh = mirror.find(rewritten.path, rewritten.identity, {});
    expect(fresh && fresh->bytes == *rewritten.bytes, "the rewritten source is mirrored anew");
}

void testRestartAndIndex() {
    const Scratch scratch("restart");
    std::vector<Source> sources;
    {
        cache::SourceMirror mirror(config(scratch));
        for (unsigned i = 0; i < 5; ++i) {
            sources.push_back(writeSource(scratch, "seq." + std::to_string(i) + ".exr", 9'000, i));
            keep(mirror, sources.back());
        }
        mirror.flush();
        const auto statistics = mirror.statistics();
        expect(statistics.entryCount == 5 && statistics.writes == 5, "five sources are mirrored");
        expect(statistics.indexLoaded, "the index is loaded once the mirror is idle");
        expect(std::filesystem::exists(scratch.cacheRoot() / "sources" / "index.v2"),
               "a flush writes the index file");
    }
    {
        cache::SourceMirror mirror(config(scratch));
        // A lookup does not wait for the index: the entry file carries its own identity.
        const auto hit = mirror.find(sources[2].path, sources[2].identity, {});
        expect(hit && hit->bytes == *sources[2].bytes, "a mirrored source survives a restart");
        mirror.flush();
        expect(mirror.statistics().entryCount == 5, "a restart recovers every entry");
        expect(mirror.localCopyState(sources[4].path) == cache::LocalCopyState::Cached,
               "the index names a mirrored source as cached");
        expect(mirror.localCopyState(scratch.root / "sources" / "other.exr") ==
                   cache::LocalCopyState::NotCached,
               "the index names an unmirrored source as not cached");
        mirror.forget(sources[4].path);
        expect(mirror.localCopyState(sources[4].path) == cache::LocalCopyState::NotCached &&
                   !mirror.find(sources[4].path, sources[4].identity, {}).has_value(),
               "forgetting a source drops its entry");
        expect(mirror.find(sources[3].path, sources[3].identity, {}).has_value(),
               "forgetting a source leaves the others");
    }
    // An entry that reached the disk without reaching the index is still found and counted.
    std::filesystem::remove(scratch.cacheRoot() / "sources" / "index.v2");
    {
        cache::SourceMirror mirror(config(scratch));
        mirror.flush();
        expect(mirror.statistics().entryCount == 4, "a lost index is rebuilt from the entry files");
        expect(mirror.find(sources[0].path, sources[0].identity, {}).has_value(),
               "a rebuilt index still serves its entries");
        mirror.clear();
        expect(mirror.statistics().entryCount == 0 &&
                   !mirror.find(sources[0].path, sources[0].identity, {}).has_value(),
               "clearing the mirror removes every entry");
    }
}

void testCorruption() {
    const Scratch scratch("corruption");
    cache::SourceMirror mirror(config(scratch));
    const auto source = writeSource(scratch, "frame.exr", 20'000, 4);
    keep(mirror, source);
    mirror.flush();
    const auto entry = mirror.entryPathForTest(source.path);
    {
        std::fstream file(entry, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(500);
        file.put('\x5a');
    }
    expect(!mirror.find(source.path, source.identity, {}).has_value(),
           "a mirror file with a flipped byte is not served");
    expect(mirror.statistics().corruptDropped == 1 && !std::filesystem::exists(entry),
           "a corrupt mirror file is counted and removed");
    keep(mirror, source);
    mirror.flush();
    std::filesystem::resize_file(entry, 5'000);
    expect(!mirror.find(source.path, source.identity, {}).has_value(),
           "a truncated mirror file is not served");
    {
        std::ofstream(entry, std::ios::binary | std::ios::trunc) << "not a mirror entry";
    }
    expect(!mirror.find(source.path, source.identity, {}).has_value(),
           "an unrelated file under an entry's name is not served");
}

void testPolicy() {
    using cache::keepsLocalCopy;
    using cache::LocalCopyOverride;
    using cache::LocalCopyPolicy;
    using cache::LocalCopyPreference;
    const std::filesystem::path source =
        std::filesystem::temp_directory_path() / "shots" / "a" / "plate.exr";
    const LocalCopyPolicy automatic;
    // Automatic: Network, Slow attached and Unknown are copied, Fast local never, and never a
    // source on the cache's own volume.
    for (const auto storage :
         {StorageClass::Network, StorageClass::SlowAttached, StorageClass::Unknown}) {
        expect(keepsLocalCopy(automatic, LocalCopyOverride::Inherit, source, storage, false),
               "Automatic copies slow storage");
        expect(!keepsLocalCopy(automatic, LocalCopyOverride::Inherit, source, storage, true),
               "Automatic never copies a source on the cache's own volume");
    }
    expect(!keepsLocalCopy(automatic, LocalCopyOverride::Inherit, source, StorageClass::FastLocal,
                           false),
           "Automatic never copies Fast local storage");
    // The preference beats the automatic rule.
    const LocalCopyPolicy always{LocalCopyPreference::Always, {}};
    const LocalCopyPolicy never{LocalCopyPreference::Never, {}};
    for (const auto storage : {StorageClass::Network, StorageClass::SlowAttached,
                               StorageClass::Unknown, StorageClass::FastLocal}) {
        expect(keepsLocalCopy(always, LocalCopyOverride::Inherit, source, storage, true),
               "Always copies every storage class");
        expect(!keepsLocalCopy(never, LocalCopyOverride::Inherit, source, storage, false),
               "Never copies no storage class");
        // The asset's override beats everything.
        expect(keepsLocalCopy(never, LocalCopyOverride::Always, source, storage, true),
               "an asset set to Always is copied whatever the preference");
        expect(!keepsLocalCopy(always, LocalCopyOverride::Never, source, storage, false),
               "an asset set to Never is not copied whatever the preference");
    }
    // A folder rule beats the preference; the longest folder that holds the source wins.
    const auto shots = std::filesystem::temp_directory_path() / "shots";
    LocalCopyPolicy folders{LocalCopyPreference::Never, {{shots, true}, {shots / "a", false}}};
    expect(
        !keepsLocalCopy(folders, LocalCopyOverride::Inherit, source, StorageClass::Network, false),
        "the longest folder rule wins");
    expect(keepsLocalCopy(folders, LocalCopyOverride::Inherit, shots / "b" / "plate.exr",
                          StorageClass::FastLocal, false),
           "a folder rule beats the preference");
    expect(!keepsLocalCopy(folders, LocalCopyOverride::Inherit,
                           std::filesystem::temp_directory_path() / "shots-other" / "plate.exr",
                           StorageClass::Network, false),
           "a folder rule does not cover a sibling whose name merely starts the same");
    expect(keepsLocalCopy(folders, LocalCopyOverride::Always, source, StorageClass::Network, false),
           "the asset's override beats a folder rule");
    folders.preference = LocalCopyPreference::Automatic;
    expect(keepsLocalCopy(folders, LocalCopyOverride::Inherit,
                          std::filesystem::temp_directory_path() / "elsewhere" / "plate.exr",
                          StorageClass::Network, false),
           "a source no folder rule holds falls through to the preference");

    // The mirror applies the policy to a real path: the scratch folder shares the cache's
    // volume, so Automatic refuses it and Always takes it.
    const Scratch scratch("policy");
    auto settings = config(scratch);
    cache::SourceMirror mirror(settings);
    const auto real = writeSource(scratch, "plate.exr", 100, 5);
    expect(!mirror.keepsLocalCopy(real.path, real.identity.volume),
           "Automatic refuses a source on the cache's own volume");
    mirror.setPolicy({LocalCopyPreference::Always, {}});
    expect(mirror.keepsLocalCopy(real.path, real.identity.volume), "Always takes it");
    expect(!mirror.keepsLocalCopy(real.path, real.identity.volume, LocalCopyOverride::Never),
           "the asset's override refuses it again");
    mirror.setPolicy({LocalCopyPreference::Always, {{scratch.root / "sources", false}}});
    expect(!mirror.keepsLocalCopy(real.path, real.identity.volume),
           "a folder rule refuses it under Always");
    expect(mirror.policy().folders.size() == 1, "the policy reads back");
}

void testEviction() {
    const Scratch scratch("eviction");
    auto settings = config(scratch);
    settings.byteBudget = 6'000; // room for about five 1,000-byte sources and their headers
    cache::SourceMirror mirror(settings);
    std::vector<Source> sources;
    for (unsigned i = 0; i < 12; ++i) {
        sources.push_back(writeSource(scratch, "f" + std::to_string(i) + ".exr", 1'000, i));
        keep(mirror, sources.back());
        mirror.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto statistics = mirror.statistics();
    expect(statistics.storedBytes <= 6'000, "the mirror stays inside its byte budget");
    expect(statistics.evictions >= 6, "older entries were evicted");
    expect(!mirror.find(sources.front().path, sources.front().identity, {}).has_value() &&
               !std::filesystem::exists(mirror.entryPathForTest(sources.front().path)),
           "the least recently used entry is gone, file included");
    expect(mirror.find(sources.back().path, sources.back().identity, {}).has_value(),
           "the newest entry is kept");
    mirror.setByteBudget(1'500);
    expect(mirror.statistics().storedBytes <= 1'500, "a smaller budget evicts at once");

    // The entry count is bounded independently of the bytes.
    const Scratch counted("eviction-count");
    auto few = config(counted);
    few.maxEntryCount = 4;
    cache::SourceMirror small(few);
    for (unsigned i = 0; i < 9; ++i) {
        keep(small, writeSource(counted, "g" + std::to_string(i) + ".png", 50, i));
        small.flush();
    }
    expect(small.statistics().entryCount <= 4, "the entry count stays inside its bound");
}

void testWriteQueue() {
    const Scratch scratch("queue");
    auto settings = config(scratch);
    settings.writeQueueByteCapacity = 2'500;
    cache::SourceMirror mirror(settings);
    mirror.flush();
    mirror.setWriterPausedForTest(true);
    const auto first = writeSource(scratch, "a.exr", 1'000, 1);
    const auto second = writeSource(scratch, "b.exr", 1'000, 2);
    const auto third = writeSource(scratch, "c.exr", 1'000, 3);
    keep(mirror, first);
    keep(mirror, first); // the same source twice is one pending write
    keep(mirror, second);
    expect(mirror.statistics().pendingWrites == 2 && mirror.statistics().pendingWriteBytes == 2'000,
           "pending writes are counted in bytes");
    expect(mirror.underPressure(), "a write queue over half full asks background fills to wait");
    // keep() returned at once each time: the caller never waited for the disk. The third write
    // does not fit, so the oldest pending write is dropped for it.
    keep(mirror, third);
    auto statistics = mirror.statistics();
    expect(statistics.droppedWrites == 1 && statistics.pendingWrites == 2,
           "a full queue drops its oldest pending write");
    expect(statistics.peakPendingWriteBytes <= 2'500, "the queue never exceeds its byte capacity");
    expect(statistics.networkReads + statistics.otherSourceReads == 4,
           "every source read handed to the mirror is counted");
    mirror.setWriterPausedForTest(false);
    mirror.flush();
    expect(!mirror.find(first.path, first.identity, {}).has_value(),
           "the dropped write left no entry");
    expect(mirror.find(second.path, second.identity, {}).has_value() &&
               mirror.find(third.path, third.identity, {}).has_value(),
           "the writes that stayed queued reached the disk");
    expect(!mirror.underPressure(), "an idle mirror is not under pressure");

    // A single source larger than the whole queue is still mirrored (one at a time).
    const auto large = writeSource(scratch, "large.exr", 9'000, 4);
    keep(mirror, large);
    mirror.flush();
    expect(mirror.find(large.path, large.identity, {}).has_value(),
           "a source larger than the queue capacity is still mirrored");

    // Bytes whose length is not the identity's size are refused: they are not the source.
    auto wrong = writeSource(scratch, "wrong.exr", 300, 5);
    wrong.identity.size = 299;
    keep(mirror, wrong);
    mirror.flush();
    expect(mirror.localCopyState(wrong.path) == cache::LocalCopyState::NotCached,
           "bytes that do not match the identity's size are not mirrored");

    // A disabled mirror neither serves nor keeps.
    mirror.setEnabled(false);
    expect(!mirror.find(large.path, large.identity, {}).has_value(), "a disabled mirror misses");
    const auto unkept = writeSource(scratch, "unkept.exr", 300, 6);
    keep(mirror, unkept);
    mirror.setEnabled(true);
    mirror.flush();
    expect(!mirror.find(unkept.path, unkept.identity, {}).has_value(),
           "a disabled mirror keeps nothing");
}

void testShutdownAndCancellation() {
    const Scratch scratch("shutdown");
    const auto first = writeSource(scratch, "a.exr", 50'000, 1);
    const auto second = writeSource(scratch, "b.exr", 50'000, 2);
    {
        cache::SourceMirror mirror(config(scratch));
        mirror.flush();
        mirror.setWriterPausedForTest(true);
        keep(mirror, first);
        keep(mirror, second);
        // Destroyed with writes pending: the destructor returns without writing them.
    }
    bool leftovers = false;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator files(scratch.cacheRoot(), error), end;
         !error && files != end; files.increment(error))
        if (files->is_regular_file() && files->path().filename() != "index.v2")
            leftovers = true;
    expect(!leftovers, "shutdown with pending writes leaves no entry and no temporary file");
    {
        cache::SourceMirror mirror(config(scratch));
        expect(!mirror.find(first.path, first.identity, {}).has_value(),
               "a write abandoned at shutdown is simply absent afterwards");
        // "Purge media cache" abandons queued writes as well.
        mirror.flush();
        mirror.setWriterPausedForTest(true);
        keep(mirror, first);
        mirror.clear();
        mirror.setWriterPausedForTest(false);
        mirror.flush();
        expect(!mirror.find(first.path, first.identity, {}).has_value(),
               "a write queued before a purge is abandoned");
        // A cancelled lookup returns nothing and leaves the entry alone.
        keep(mirror, second);
        mirror.flush();
        expect(!mirror.find(second.path, second.identity, [] { return true; }).has_value(),
               "a cancelled lookup returns nothing");
        expect(mirror.find(second.path, second.identity, {}).has_value(),
               "a cancelled lookup leaves the entry in place");
    }
    // A cache volume short of space skips the write and asks background fills to wait.
    auto tight = config(scratch);
    tight.freeSpaceReserveBytes = std::uint64_t{1} << 62;
    cache::SourceMirror mirror(tight);
    mirror.clear();
    keep(mirror, first);
    mirror.flush();
    expect(mirror.statistics().refusedForDiskSpace == 1 &&
               !mirror.find(first.path, first.identity, {}).has_value(),
           "a write is skipped while the cache volume is short of free space");
    expect(mirror.statistics().diskPressure && !mirror.underPressure(),
           "a cache volume short of space is reported, and is not a write backlog");
}

void testLegacyEntriesRemoved() {
    const Scratch scratch("legacy");
    std::filesystem::create_directories(scratch.cacheRoot() / "ab");
    {
        std::ofstream(scratch.cacheRoot() / "index.v1") << "old index";
        std::ofstream(scratch.cacheRoot() / "ab" / "abcdef") << "old decoded frame";
    }
    cache::SourceMirror mirror(config(scratch));
    mirror.flush();
    expect(!std::filesystem::exists(scratch.cacheRoot() / "index.v1") &&
               !std::filesystem::exists(scratch.cacheRoot() / "ab"),
           "the retired decoded-frame entries are removed at startup");
}

void testConcurrency() {
    const Scratch scratch("concurrency");
    cache::SourceMirror mirror(config(scratch));
    std::vector<Source> sources;
    for (unsigned i = 0; i < 24; ++i)
        sources.push_back(writeSource(scratch, "c" + std::to_string(i) + ".exr", 3'000 + i, i));
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 6; ++t)
        threads.emplace_back([&, t] {
            for (unsigned round = 0; round < 40; ++round) {
                const auto& source = sources[(t * 7 + round) % sources.size()];
                if (const auto hit = mirror.find(source.path, source.identity, {}))
                    wrong += hit->bytes == *source.bytes ? 0 : 1;
                else
                    keep(mirror, source);
            }
        });
    for (auto& thread : threads)
        thread.join();
    mirror.flush();
    expect(wrong == 0, "concurrent readers and writers never see foreign bytes");
    int served = 0;
    for (const auto& source : sources)
        served += mirror.find(source.path, source.identity, {}).has_value() ? 1 : 0;
    expect(served == static_cast<int>(sources.size()), "every source ends up mirrored");
}
} // namespace

int main() {
    testIdentity();
    testRestartAndIndex();
    testCorruption();
    testPolicy();
    testEviction();
    testWriteQueue();
    testShutdownAndCancellation();
    testLegacyEntriesRemoved();
    testConcurrency();
    if (failures == 0)
        std::cout << "PASS: source mirror\n";
    return failures == 0 ? 0 : 1;
}
