// bloom.media.source-mirror (#718, #732): the local mirror of source media on slow storage. Covers
// the metadata identity check, the local-copy policy, write-through and its queue, the disk
// headroom (growing into free space, giving it back, refusing instead of evicting what is in
// use), corruption, restart, cancellation and shutdown, and the byte ranges of large sources
// (chunk files). No test here reads a source through the mirror: the mirror never opens a
// source, which the tests prove by deleting it. No test reads the machine's real free space or
// waits for time to pass: the cache volume and the clock are injected.
#include <bloom/core/sha256.hpp>
#include <bloom/media/cache/source_mirror.hpp>
#include <bloom/media/cache/source_mirror_settings.hpp>
#include <bloom/platform/source_identity.hpp>
#include <bloom/runtime/memory_budget_ledger.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
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
constexpr std::uint64_t kMiB = std::uint64_t{1} << 20U;
constexpr std::uint64_t kGiB = std::uint64_t{1} << 30U;
// A mirror on a large, mostly empty cache volume: the cases that are not about space never meet
// the headroom, whatever the machine running them has free.
[[nodiscard]] cache::SourceMirrorConfig config(const Scratch& scratch) {
    cache::SourceMirrorConfig value;
    value.rootDirectory = scratch.cacheRoot();
    value.driveSpace = cache::roomyDriveSpaceForTest();
    return value;
}

// A clock a test moves by hand.
struct FakeClock final {
    std::atomic<std::int64_t> nanos{1'000'000'000'000};
    void advance(const std::chrono::nanoseconds by) { nanos += by.count(); }
    [[nodiscard]] std::function<std::int64_t()> source() {
        return [this] { return nanos.load(); };
    }
};
// A cache volume of a chosen size shared with "other programs": what is free is the capacity
// minus their bytes and minus what the attached mirror stores. No real disk size is read.
struct FakeDrive final {
    std::atomic<std::uint64_t> capacity{0};
    std::atomic<std::uint64_t> other{0};
    std::atomic<bool> unreadable{false};
    std::atomic<cache::SourceMirror*> mirror{nullptr};
    // Leaves room for `bytes` more than the attached mirror stores now, beyond `headroom`.
    void leaveRoomFor(const std::uint64_t bytes, const std::uint64_t headroom) {
        const auto* const attached = mirror.load();
        const auto stored = attached != nullptr ? attached->statistics().storedBytes : 0;
        other = capacity.load() - headroom - bytes - stored;
    }
    [[nodiscard]] std::function<std::optional<cache::SourceMirrorDriveSpace>()> probe() {
        return [this]() -> std::optional<cache::SourceMirrorDriveSpace> {
            if (unreadable.load())
                return std::nullopt;
            const auto* const attached = mirror.load();
            const auto used =
                other.load() + (attached != nullptr ? attached->statistics().storedBytes : 0);
            const auto size = capacity.load();
            return cache::SourceMirrorDriveSpace{size, size > used ? size - used : 0};
        };
    }
};

// A mirror on a fake 1 GiB cache volume with a 64 MiB headroom, a fake clock, and room for
// exactly `roomBytes` more than it stores when it is made.
constexpr std::uint64_t kTestHeadroom = 64 * kMiB;
[[nodiscard]] cache::SourceMirrorConfig spaceConfig(const Scratch& scratch, FakeClock& clock,
                                                    FakeDrive& drive,
                                                    const std::uint64_t roomBytes) {
    auto value = config(scratch);
    value.clock = clock.source();
    value.driveSpace = drive.probe();
    value.headroomBytes = kTestHeadroom;
    value.freeSpaceReserveBytes = 1; // the 1 GiB floor would hide the test's small headroom
    drive.capacity = kGiB;
    drive.leaveRoomFor(roomBytes, kTestHeadroom);
    return value;
}

// What a frame does per sequence member: ask the mirror, else read the source and keep it. The
// writer is flushed after each keep so the counts are about the space, never about the queue.
struct PassCounts final {
    std::size_t hits = 0, sourceReads = 0;
};
[[nodiscard]] PassCounts playPass(cache::SourceMirror& mirror, const std::vector<Source>& frames) {
    PassCounts counts;
    for (const auto& frame : frames) {
        if (mirror.find(frame.path, frame.identity, {})) {
            ++counts.hits;
            continue;
        }
        ++counts.sourceReads;
        keep(mirror, frame);
        mirror.flush();
    }
    return counts;
}

constexpr std::size_t kLoopFrameBytes = 64 * 1024;
constexpr std::size_t kLoopEntryBytes = kLoopFrameBytes + 80; // the frame and its entry header

[[nodiscard]] std::vector<Source> loopFrames(const Scratch& scratch, const std::string& stem,
                                             const unsigned count, const unsigned seed = 0) {
    std::vector<Source> frames;
    for (unsigned i = 0; i < count; ++i)
        frames.push_back(writeSource(scratch, stem + "." + std::to_string(1000 + i) + ".exr",
                                     kLoopFrameBytes, seed + i));
    return frames;
}
[[nodiscard]] std::size_t countCached(cache::SourceMirror& mirror, const std::vector<Source>& frames) {
    std::size_t cached = 0;
    for (const auto& frame : frames)
        cached += mirror.localCopyState(frame.path) == cache::LocalCopyState::Cached ? 1 : 0;
    return cached;
}

// #732 (R1): the default headroom is the smaller of a tenth of the cache volume and 32 GiB, never
// under the 1 GiB floor, and a setting replaces it live.
void testDefaultHeadroom() {
    const auto headroomOn = [](const std::uint64_t capacity, const std::uint64_t setting) {
        const Scratch scratch("headroom");
        FakeDrive drive;
        drive.capacity = capacity;
        auto settings = config(scratch);
        settings.driveSpace = drive.probe();
        settings.headroomBytes = setting;
        cache::SourceMirror mirror(settings);
        drive.mirror = &mirror;
        mirror.flush();
        return mirror.statistics();
    };
    expect(cache::defaultSourceMirrorHeadroom(100 * kGiB) == 10 * kGiB &&
               cache::defaultSourceMirrorHeadroom(4096 * kGiB) == 32 * kGiB,
           "the default headroom is the smaller of a tenth of the volume and 32 GiB");
    const auto small = headroomOn(100 * kGiB, 0);
    expect(small.driveMeasured && small.capacityBytes == 100 * kGiB &&
               small.headroomBytes == 10 * kGiB && small.freeBytes == 100 * kGiB,
           "a 100 GiB cache volume keeps 10 GiB free by default");
    expect(small.availableBytes == 90 * kGiB,
           "an empty mirror may grow into all the free space beyond the headroom");
    expect(headroomOn(2048 * kGiB, 0).headroomBytes == 32 * kGiB,
           "a large cache volume keeps 32 GiB free by default");
    expect(headroomOn(5 * kGiB, 0).headroomBytes == 1 * kGiB,
           "the headroom is never under the 1 GiB floor");
    expect(headroomOn(100 * kGiB, 1).headroomBytes == 1 * kGiB,
           "a headroom setting under the floor is raised to it");
    expect(headroomOn(100 * kGiB, 25 * kGiB).headroomBytes == 25 * kGiB,
           "a headroom setting replaces the default");

    // A new setting applies live, on the mirror's own thread.
    const Scratch scratch("headroom-live");
    FakeDrive drive;
    drive.capacity = 100 * kGiB;
    auto settings = config(scratch);
    settings.driveSpace = drive.probe();
    cache::SourceMirror mirror(settings);
    drive.mirror = &mirror;
    mirror.flush();
    mirror.setHeadroom(40 * kGiB);
    mirror.flush();
    expect(mirror.headroom() == 40 * kGiB && mirror.statistics().headroomBytes == 40 * kGiB &&
               mirror.statistics().availableBytes == 60 * kGiB,
           "a new headroom applies without a restart");
    mirror.setHeadroom(0);
    mirror.flush();
    expect(mirror.statistics().headroomBytes == 10 * kGiB, "headroom 0 is the default again");

    // A cache volume that cannot be read: nothing to measure, so a fixed limit applies.
    drive.unreadable = true;
    mirror.flush();
    const auto blind = mirror.statistics();
    expect(!blind.driveMeasured && blind.availableBytes == cache::kSourceMirrorUnmeasuredByteLimit,
           "an unreadable cache volume caps the mirror at a fixed size");
    const auto source = writeSource(scratch, "blind.exr", 2'000, 9);
    keep(mirror, source);
    mirror.flush();
    expect(mirror.find(source.path, source.identity, {}).has_value(),
           "an unreadable cache volume does not stop the mirror");

    // Without an injected probe the file system is asked: the answer is whatever the machine
    // has, so only its presence is checked.
    const Scratch real("headroom-real");
    cache::SourceMirrorConfig plain;
    plain.rootDirectory = real.cacheRoot();
    cache::SourceMirror measured(plain);
    measured.flush();
    expect(measured.statistics().driveMeasured && measured.statistics().capacityBytes != 0,
           "the cache volume's real capacity is measured on the mirror's own thread");
}

// #732 (R2): free space is measured live. The mirror grows into what is free beyond the headroom
// with its own bytes counting as usable, gives space back least recently used first when other
// programs take it, and grows again when they let go.
void testLiveHeadroom() {
    const Scratch scratch("live");
    FakeClock clock;
    FakeDrive drive;
    const auto frames = loopFrames(scratch, "plate", 9);
    const auto settings = spaceConfig(scratch, clock, drive, 6 * kLoopEntryBytes);
    {
        cache::SourceMirror mirror(settings);
        drive.mirror = &mirror;
        mirror.flush();
        expect(mirror.statistics().availableBytes == 6 * kLoopEntryBytes,
               "the mirror may hold the free space beyond the headroom");
        for (unsigned i = 0; i < 6; ++i)
            keep(mirror, frames[i]);
        mirror.flush();
        auto statistics = mirror.statistics();
        expect(statistics.writes == 6 && statistics.refusedWrites == 0 &&
                   statistics.storedBytes == 6 * kLoopEntryBytes,
               "the mirror grows into the free space beyond the headroom");
        expect(statistics.freeBytes == kTestHeadroom,
               "a full mirror leaves exactly the headroom free");
        expect(statistics.availableBytes == 6 * kLoopEntryBytes && statistics.evictions == 0,
               "the mirror's own bytes count as usable: filling it does not shrink it");
        drive.mirror = nullptr;
    }
    clock.advance(std::chrono::hours(1)); // the next run starts later
    {
        // A restart measures the same volume: what the mirror stores is still usable, so
        // nothing is evicted (a budget taken from free space at startup shrank on every start).
        cache::SourceMirror mirror(settings);
        drive.mirror = &mirror;
        mirror.flush();
        auto statistics = mirror.statistics();
        expect(statistics.entryCount == 6 && statistics.evictions == 0 &&
                   statistics.availableBytes == 6 * kLoopEntryBytes,
               "a restart keeps a full mirror whole");
        // Use order: 2, 0, then 5 (the rest keep their times from the first run).
        for (const unsigned index : {2U, 0U, 5U})
            expect(mirror.find(frames[index].path, frames[index].identity, {}).has_value(),
                   "an entry is served after a restart");
        // Another program takes two and a half entries' worth of the volume. Nothing is being
        // written: the mirror's idle pass gives the space back, least recently used first, and
        // does not spare entries used a moment ago.
        drive.other += 2 * kLoopEntryBytes + kLoopEntryBytes / 2;
        mirror.flush();
        statistics = mirror.statistics();
        expect(statistics.evictions == 3 && statistics.entryCount == 3,
               "space other programs take from the headroom is given back");
        expect(countCached(mirror, {frames[1], frames[3], frames[4]}) == 0 &&
                   countCached(mirror, {frames[2], frames[0], frames[5]}) == 3,
               "space is given back least recently used first");
        expect(!std::filesystem::exists(mirror.entryPathForTest(frames[1].path)),
               "an entry given back is removed from the volume");
        expect(statistics.freeBytes >= kTestHeadroom, "the headroom is free again");
        // The half entry of room that is left is not enough for a write, and the entries that
        // could make room were just used: the write is refused.
        keep(mirror, frames[6]);
        mirror.flush();
        expect(mirror.statistics().refusedWrites == 1 && mirror.statistics().evictions == 3,
               "a write that would evict an entry in use is refused");
        // The other program lets go: the mirror grows again.
        drive.other -= 2 * kLoopEntryBytes + kLoopEntryBytes / 2;
        clock.advance(std::chrono::seconds(2)); // the next write measures the volume again
        for (unsigned i = 6; i < 9; ++i)
            keep(mirror, frames[i]);
        mirror.flush();
        statistics = mirror.statistics();
        expect(statistics.entryCount == 6 && statistics.evictions == 3 &&
                   countCached(mirror, {frames[6], frames[7], frames[8]}) == 3,
               "the mirror grows again when free space comes back");
        // A larger headroom gives space back too, and applies live.
        mirror.setHeadroom(kTestHeadroom + 4 * kLoopEntryBytes);
        mirror.flush();
        expect(mirror.statistics().entryCount == 2 &&
                   mirror.statistics().headroomBytes == kTestHeadroom + 4 * kLoopEntryBytes,
               "a larger headroom is given its space at once");
        drive.mirror = nullptr;
    }
}

// #732 (R3): a loop larger than the space keeps a stable set of disk hits. The writes that do not
// fit are refused and counted; nothing the loop still needs is evicted. `frameInterval` is the
// time between two frames: a pass shorter or longer than the hold window.
void testLoopLargerThanTheSpace(const std::chrono::seconds frameInterval, const bool report) {
    const Scratch scratch("loop");
    const auto frames = loopFrames(scratch, "plate", 16);
    FakeClock clock;
    FakeDrive drive;
    // Room for 10 of the 16, and a half.
    cache::SourceMirror mirror(
        spaceConfig(scratch, clock, drive, 10 * kLoopEntryBytes + kLoopEntryBytes / 2));
    drive.mirror = &mirror;
    mirror.flush();
    const auto pass = [&] {
        PassCounts counts;
        for (const auto& frame : frames) {
            clock.advance(frameInterval);
            const auto one = playPass(mirror, {frame});
            counts.hits += one.hits;
            counts.sourceReads += one.sourceReads;
        }
        return counts;
    };
    const auto first = pass();
    expect(first.hits == 0 && first.sourceReads == 16, "the first pass reads every frame");
    expect(mirror.statistics().entryCount == 10, "the first pass fills the room there is");
    for (int number = 2; number <= 3; ++number) {
        const auto before = mirror.statistics();
        const auto counts = pass();
        const auto after = mirror.statistics();
        if (report)
            std::cout << "loop of 16 with room for 10, " << frameInterval.count()
                      << " s a frame, pass " << number << ": hits=" << counts.hits
                      << " sourceReads=" << counts.sourceReads
                      << " evictions=" << after.evictions - before.evictions
                      << " refused=" << after.refusedWrites - before.refusedWrites << '\n';
        const auto label = "pass " + std::to_string(number) + " of a loop larger than the space (" +
                           std::to_string(frameInterval.count()) + " s a frame) ";
        expect(counts.hits == 10, label + "gets the 10 frames that fit from disk");
        expect(counts.sourceReads == 6, label + "reads only the 6 that do not fit");
        expect(after.evictions == before.evictions, label + "evicts nothing");
        expect(after.refusedWrites == before.refusedWrites + 6,
               label + "counts the 6 writes it refuses");
    }
    expect(mirror.statistics().diskPressure, "a refused write is reported as no room");
    if (frameInterval * static_cast<int>(frames.size()) >= cache::kSourceMirrorHoldWindow)
        return;

    // The work stops (after passes shorter than the hold window, so that every entry was used a
    // moment ago). New work that arrives now is refused; once nothing has used the loop's
    // entries for the hold window it replaces them, oldest first.
    const auto next = loopFrames(scratch, "other", 4, 100);
    clock.advance(std::chrono::minutes(1));
    keep(mirror, next[0]);
    mirror.flush();
    expect(countCached(mirror, next) == 0, "new work does not evict entries used a minute ago");
    clock.advance(cache::kSourceMirrorHoldWindow + std::chrono::minutes(1));
    const auto before = mirror.statistics();
    const auto rotated = playPass(mirror, next);
    const auto after = mirror.statistics();
    expect(rotated.sourceReads == 4 && countCached(mirror, next) == 4,
           "new work replaces entries nothing has used for the hold window");
    expect(after.evictions == before.evictions + 4 && after.refusedWrites == before.refusedWrites,
           "rotation evicts one idle entry for each new one and refuses nothing");
    expect(playPass(mirror, next).hits == 4, "the new work is served from disk");
}

// #732 (R1): the published settings carry the headroom, and the byte budget of an earlier build is
// dropped: its line is not read.
void testPublishedSettings() {
    const Scratch scratch("settings");
    const auto file = scratch.root / "settings.v1";
    cache::SourceMirrorUserSettings settings;
    settings.headroomBytes = 48 * kGiB;
    settings.policy.preference = cache::LocalCopyPreference::Always;
    expect(cache::writeSourceMirrorSettings(file, settings), "the settings file is written");
    expect(cache::readSourceMirrorSettings(file) == settings,
           "the headroom reads back from the published settings");
    std::string text;
    {
        std::ifstream in(file, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    expect(text.find("headroom 51539607552\n") != std::string::npos &&
               text.find("budget") == std::string::npos,
           "the published settings name the headroom and no budget");
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "bloom-source-mirror-settings 1\nenabled 1\nbudget 64424509440\ncopies never\n";
    }
    const auto old = cache::readSourceMirrorSettings(file);
    expect(old.headroomBytes == 0 && old.policy.preference == cache::LocalCopyPreference::Never,
           "a byte budget published by an earlier build is dropped; the rest is read");
}

// #732 (D6): the entry limit follows the same rule as the bytes.
void testEntryLimit() {
    const Scratch scratch("entry-limit");
    FakeClock clock;
    FakeDrive drive;
    auto settings = spaceConfig(scratch, clock, drive, 64 * kMiB);
    settings.maxEntryCount = 4;
    cache::SourceMirror mirror(settings);
    drive.mirror = &mirror;
    std::vector<Source> sources;
    for (unsigned i = 0; i < 6; ++i) {
        sources.push_back(writeSource(scratch, "g" + std::to_string(i) + ".png", 50, i));
        keep(mirror, sources.back());
        mirror.flush();
    }
    auto statistics = mirror.statistics();
    expect(statistics.entryCount == 4 && statistics.refusedWrites == 2 && statistics.evictions == 0,
           "a full index refuses new entries while the ones it holds are in use");
    clock.advance(cache::kSourceMirrorHoldWindow + std::chrono::minutes(1));
    keep(mirror, sources[5]);
    mirror.flush();
    statistics = mirror.statistics();
    expect(statistics.entryCount == 4 && statistics.evictions == 1 &&
               mirror.localCopyState(sources[5].path) == cache::LocalCopyState::Cached &&
               mirror.localCopyState(sources[0].path) == cache::LocalCopyState::NotCached,
           "an idle entry makes way for a new one at the entry limit");
}

// #732 (R4): the write queue is live work with a fixed ceiling. A RAM Preview fill does not shrink
// it, memory pressure does not drop what is waiting, and during a fill a full queue makes the
// read wait for the writer instead of dropping a write.
void testWriteQueueIsLiveWork() {
    namespace runtime = bloom::runtime;
    const Scratch scratch("fill");
    runtime::MemoryBudgetLedger ledger(64 * kGiB, 32 * kGiB);
    ledger.setRamHeadroom(2 * kGiB);
    runtime::MachineMemorySample sample;
    sample.availableBytes = 32 * kGiB;
    static_cast<void>(ledger.poll(sample));
    {
        // The default ceiling, with plenty of memory: the queue is not offered the free memory.
        cache::SourceMirror mirror(config(scratch), ledger);
        mirror.flush();
        static_cast<void>(ledger.poll(sample));
        expect(mirror.writeQueueByteCapacity() == cache::kSourceMirrorWriteQueueByteCapacity,
               "outside a fill the write queue's capacity is its fixed ceiling");
        auto fill = ledger.beginPreviewFill();
        static_cast<void>(ledger.poll(sample));
        expect(mirror.writeQueueByteCapacity() == cache::kSourceMirrorWriteQueueByteCapacity,
               "a RAM Preview fill leaves the write queue's capacity at its ceiling");
        // Three layers' members arrive together (one composite frame) while the writer is busy.
        mirror.setWriterPausedForTest(true);
        const auto frames = loopFrames(scratch, "layer", 3);
        for (const auto& frame : frames)
            keep(mirror, frame);
        auto during = mirror.statistics();
        expect(during.pendingWrites == 3 && during.droppedWrites == 0,
               "three writes handed over during a fill are all pending and none is dropped");
        // Memory pressure and an admission reclaim are for reusable caches: pending writes stay.
        runtime::MachineMemorySample tight;
        tight.availableBytes = kGiB; // under the RAM headroom
        static_cast<void>(ledger.poll(tight));
        static_cast<void>(ledger.reclaim(64 * kMiB));
        during = mirror.statistics();
        expect(during.pendingWrites == 3 && during.droppedWrites == 0 &&
                   mirror.writeQueueByteCapacity() == cache::kSourceMirrorWriteQueueByteCapacity,
               "memory pressure and a reclaim leave the pending writes and the ceiling alone");
        mirror.setWriterPausedForTest(false);
        mirror.flush();
        fill.end();
        expect(mirror.statistics().writes == 3 && mirror.statistics().droppedWrites == 0,
               "every write handed over during a fill reaches the disk");
    }

    // A queue with room for two frames, and a writer held before its next write.
    auto small = config(scratch);
    small.rootDirectory = scratch.root / "cache-small";
    small.writeQueueByteCapacity = 2 * kLoopFrameBytes + kLoopFrameBytes / 2;
    cache::SourceMirror mirror(small, ledger);
    mirror.flush();
    const auto frames = loopFrames(scratch, "queued", 6);
    mirror.setWriterPausedForTest(true);
    {
        // In a fill the third read waits for room: observed through the mirror's own count of
        // waiting reads, not through a timer.
        auto fill = ledger.beginPreviewFill();
        keep(mirror, frames[0]);
        keep(mirror, frames[1]);
        std::thread reader([&] { keep(mirror, frames[2]); });
        while (mirror.statistics().waitingWrites == 0)
            std::this_thread::yield();
        auto waiting = mirror.statistics();
        expect(waiting.waitingWrites == 1 && waiting.pendingWrites == 2 &&
                   waiting.droppedWrites == 0 &&
                   waiting.pendingWriteBytes <= small.writeQueueByteCapacity,
               "in a fill a full queue makes the read wait and drops nothing");
        mirror.setWriterPausedForTest(false);
        reader.join();
        mirror.flush();
        expect(mirror.statistics().writes == 3 && mirror.statistics().droppedWrites == 0 &&
                   mirror.statistics().waitingWrites == 0,
               "the waiting read resumes when the writer makes room, and every write lands");
        expect(mirror.statistics().peakPendingWriteBytes <= small.writeQueueByteCapacity,
               "the queue never grows past its ceiling");

        // The end of the fill releases a waiting read; it then follows the rule outside a fill.
        mirror.setWriterPausedForTest(true);
        keep(mirror, frames[3]);
        keep(mirror, frames[4]);
        std::thread late([&] { keep(mirror, frames[5]); });
        while (mirror.statistics().waitingWrites == 0)
            std::this_thread::yield();
        fill.end();
        late.join();
        const auto after = mirror.statistics();
        expect(after.waitingWrites == 0 && after.droppedWrites == 1 && after.pendingWrites == 2,
               "a read still waiting when the fill ends drops the oldest pending write instead");
        mirror.setWriterPausedForTest(false);
        mirror.flush();
    }
    {
        // A purge releases a waiting read and abandons its write, like the pending ones.
        auto fill = ledger.beginPreviewFill();
        mirror.clear();
        mirror.setWriterPausedForTest(true);
        keep(mirror, frames[0]);
        keep(mirror, frames[1]);
        std::thread reader([&] { keep(mirror, frames[2]); });
        while (mirror.statistics().waitingWrites == 0)
            std::this_thread::yield();
        mirror.clear();
        reader.join();
        mirror.setWriterPausedForTest(false);
        mirror.flush();
        expect(mirror.statistics().entryCount == 0 && mirror.statistics().waitingWrites == 0,
               "a purge releases a waiting read and keeps nothing of it");
    }
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
    // A cache volume with nothing free beyond the headroom refuses the write, and that is not a
    // write backlog.
    auto tight = config(scratch);
    tight.driveSpace = [] {
        return std::optional(cache::SourceMirrorDriveSpace{100 * kGiB, 2 * kGiB});
    };
    cache::SourceMirror mirror(tight);
    mirror.clear();
    keep(mirror, first);
    mirror.flush();
    expect(mirror.statistics().refusedWrites == 1 &&
               !mirror.find(first.path, first.identity, {}).has_value(),
           "a write is refused while the cache volume has nothing free beyond the headroom");
    expect(mirror.statistics().diskPressure && !mirror.underPressure(),
           "a mirror with no room is reported, and is not a write backlog");
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

// Byte ranges of a large source (a video container): kept and served by whole 1 MiB chunks.
void testChunks() {
    constexpr std::size_t kChunk = std::size_t{1} << 20U;
    const Scratch scratch("chunks");
    const auto source = writeSource(scratch, "clip.mov", kChunk * 2 + kChunk / 2, 21);
    const auto range = [&](const std::size_t index) {
        const auto begin = index * kChunk;
        return std::span<const std::byte>(source.bytes->data() + begin,
                                          std::min(kChunk, source.bytes->size() - begin));
    };
    const auto serves = [&](platform::SourceChunkStore& store, const std::size_t index) {
        Bytes read(range(index).size());
        return store.read(index * kChunk, read) &&
               std::equal(read.begin(), read.end(), range(index).begin());
    };
    std::filesystem::path chunkPath;
    {
        cache::SourceMirror mirror(config(scratch));
        mirror.flush();
        const auto store = mirror.chunks(source.path, source.identity);
        expect(store != nullptr, "a kept source has a chunk store");
        if (!store)
            return;
        expect(mirror.chunks(source.path, source.identity) == store,
               "one chunk store object serves a source");
        Bytes scratchBytes(kChunk);
        expect(!store->read(0, scratchBytes), "a chunk that was never kept is not served");
        store->keep(0, range(0));
        store->keep(2 * kChunk, range(2));
        // The mirror never opens the source: everything below is served without it.
        std::filesystem::remove(source.path);
        expect(serves(*store, 0) && serves(*store, 2), "kept chunks are served exactly");
        expect(!store->read(kChunk, scratchBytes), "a missing chunk between kept ones is a miss");
        Bytes two(kChunk * 2);
        expect(!store->read(0, two), "a range with a missing chunk is not served");
        Bytes unaligned(kChunk);
        expect(!store->read(17, unaligned), "only whole chunks at chunk boundaries are served");
        expect(mirror.localCopyState(source.path) == cache::LocalCopyState::Partial,
               "a source with some chunks reads Partial");
        const auto statistics = mirror.statistics();
        expect(statistics.rangeWrites == 2 && statistics.rangeHits == 2 &&
                   statistics.rangeHitBytes == range(0).size() + range(2).size(),
               "range writes and hits are counted");
        expect(statistics.networkReadBytes + statistics.otherSourceReadBytes ==
                   range(0).size() + range(2).size(),
               "bytes kept from the source count as source reads");
        expect(statistics.storedBytes >= range(0).size() + range(2).size() &&
                   statistics.storedBytes < source.bytes->size(),
               "a sparse chunk file is charged for the chunks it holds");
        chunkPath = mirror.chunkPathForTest(source.path);
        mirror.flush();
    }
    {
        // Restart: the chunk file's own table says what it holds.
        cache::SourceMirror mirror(config(scratch));
        mirror.flush();
        expect(mirror.localCopyState(source.path) == cache::LocalCopyState::Partial,
               "a restart finds the partly copied source");
        expect(mirror.statistics().storedBytes >= range(0).size() + range(2).size(),
               "a restart counts the chunks on disk");
        const auto store = mirror.chunks(source.path, source.identity);
        expect(store && serves(*store, 0) && serves(*store, 2), "chunks survive a restart");
        if (!store)
            return;
        store->keep(kChunk, range(1));
        Bytes whole(source.bytes->size());
        expect(store->read(0, whole) && whole == *source.bytes,
               "a source whose every chunk is local is served whole");
        expect(mirror.localCopyState(source.path) == cache::LocalCopyState::Cached,
               "a source with every chunk reads Cached");
        // Corruption: one flipped byte fails that chunk only, and it can be kept again.
        {
            std::fstream file(chunkPath, std::ios::binary | std::ios::in | std::ios::out);
            const auto at = static_cast<std::streamoff>(
                cache::SourceMirror::chunkDataOffsetForTest(source.bytes->size(), 1) + 5);
            file.seekg(at);
            char value = 0;
            file.get(value);
            file.seekp(at);
            file.put(static_cast<char>(value ^ 0x40));
        }
        Bytes scratchBytes(kChunk);
        expect(!store->read(kChunk, scratchBytes), "a corrupt chunk is never served");
        expect(mirror.statistics().corruptDropped == 1, "a corrupt chunk is counted");
        expect(serves(*store, 0) && serves(*store, 2), "the other chunks are untouched");
        expect(mirror.localCopyState(source.path) == cache::LocalCopyState::Partial,
               "a dropped chunk makes the source Partial again");
        store->keep(kChunk, range(1));
        expect(serves(*store, 1), "a dropped chunk is kept again from the source");
        // The source changed: its chunk file is purged, and the old store serves nothing.
        auto changed = source.identity;
        changed.modifiedNanoseconds += 1'000'000;
        const auto fresh = mirror.chunks(source.path, changed);
        expect(fresh && fresh != store, "a changed source gets a new chunk store");
        expect(!store->read(0, scratchBytes), "the old identity's store serves nothing");
        expect(fresh && !fresh->read(0, scratchBytes), "the new identity starts with no chunks");
        expect(mirror.statistics().staleDropped == 1, "a changed source's chunks are counted stale");
        // Purge: an open store stops serving and keeping.
        if (fresh)
            fresh->keep(0, range(0));
        mirror.clear();
        expect(fresh && !fresh->read(0, scratchBytes), "a purge empties an open chunk store");
        if (fresh)
            fresh->keep(0, range(0));
        expect(!std::filesystem::exists(chunkPath), "a purged chunk store writes nothing more");
    }
    {
        // Space: chunks follow the same rule as whole entries. A chunk that needs the room of a
        // chunk file in use is refused; an idle chunk file goes whole; a source larger than the
        // space gets no store; a store that outlives the mirror holds nothing.
        FakeClock clock;
        FakeDrive drive;
        std::shared_ptr<platform::SourceChunkStore> outliving;
        const auto first = writeSource(scratch, "first.mov", kChunk * 2, 22);
        const auto second = writeSource(scratch, "second.mov", kChunk * 2, 23);
        Bytes scratchBytes(kChunk);
        {
            const Scratch space("chunk-space");
            // Room for three chunks and the two files' tables.
            cache::SourceMirror mirror(spaceConfig(space, clock, drive, kChunk * 3 + 16 * 4096));
            drive.mirror = &mirror;
            mirror.flush();
            const auto a = mirror.chunks(first.path, first.identity);
            const auto b = mirror.chunks(second.path, second.identity);
            expect(a && b, "two sources that each fit get chunk stores");
            if (!a || !b)
                return;
            const auto piece = [](const Source& from, const std::size_t index) {
                return std::span<const std::byte>(from.bytes->data() + index * kChunk, kChunk);
            };
            a->keep(0, piece(first, 0));
            a->keep(kChunk, piece(first, 1));
            b->keep(0, piece(second, 0));
            b->keep(kChunk, piece(second, 1)); // no room, and the other file was just used
            auto statistics = mirror.statistics();
            expect(statistics.refusedWrites == 1 && statistics.evictions == 0 &&
                       statistics.rangeWrites == 3,
                   "a chunk that would evict a chunk file in use is refused and counted");
            expect(a->read(0, scratchBytes) && b->read(0, scratchBytes) &&
                       !b->read(kChunk, scratchBytes),
                   "a refused chunk leaves every kept chunk in place");
            expect(b->underPressure(), "a chunk store with no room asks its background fill to wait");
            // The first file is used no more; the second goes on after the hold window.
            clock.advance(cache::kSourceMirrorHoldWindow + std::chrono::minutes(1));
            expect(b->read(0, scratchBytes), "the chunk file in use is served");
            expect(!b->underPressure(), "an idle chunk file can make room");
            b->keep(kChunk, piece(second, 1));
            statistics = mirror.statistics();
            expect(!a->read(0, scratchBytes) && b->read(kChunk, scratchBytes),
                   "an idle chunk file is evicted for a chunk of the one in use");
            expect(mirror.localCopyState(first.path) == cache::LocalCopyState::NotCached &&
                       !std::filesystem::exists(mirror.chunkPathForTest(first.path)),
                   "an evicted chunk file is removed");
            expect(statistics.evictions == 1 && statistics.freeBytes >= kTestHeadroom,
                   "chunk files leave the headroom free");
            auto huge = first.identity;
            huge.size = kChunk * 8;
            const auto refusedBefore = mirror.statistics().refusedWrites;
            expect(mirror.chunks(first.path, huge) == nullptr &&
                       mirror.statistics().refusedWrites == refusedBefore + 1,
                   "a source larger than the space gets no chunk store");
            outliving = b;
            drive.mirror = nullptr;
        }
        expect(!outliving->read(0, scratchBytes), "a store that outlives the mirror serves nothing");
        outliving->keep(0, std::span<const std::byte>(second.bytes->data(), kChunk));
        expect(!outliving->underPressure(), "a store that outlives the mirror reports no pressure");
    }
    {
        // A cache volume with nothing free beyond the headroom starts no chunk file.
        auto full = config(scratch);
        full.driveSpace = [] {
            return std::optional(cache::SourceMirrorDriveSpace{100 * kGiB, 2 * kGiB});
        };
        cache::SourceMirror mirror(full);
        mirror.flush();
        const auto other = writeSource(scratch, "full.mov", kChunk, 24);
        expect(mirror.chunks(other.path, other.identity) == nullptr &&
                   mirror.statistics().refusedWrites == 1,
               "a cache volume with no room starts no chunk file");
    }
    {
        // Several workers fill and read one source at once.
        cache::SourceMirror mirror(config(scratch));
        mirror.flush();
        const auto wide = writeSource(scratch, "wide.mov", kChunk * 3, 25);
        const auto store = mirror.chunks(wide.path, wide.identity);
        expect(store != nullptr, "a chunk store for the concurrent fill");
        if (!store)
            return;
        std::atomic<int> wrong{0};
        std::vector<std::thread> workers;
        for (unsigned worker = 0; worker < 3; ++worker)
            workers.emplace_back([&, worker] {
                for (std::size_t step = 0; step < 6; ++step) {
                    const auto index = (step + worker) % 3;
                    const std::span<const std::byte> piece(wide.bytes->data() + index * kChunk,
                                                           kChunk);
                    Bytes read(kChunk);
                    if (store->read(index * kChunk, read)) {
                        if (!std::equal(read.begin(), read.end(), piece.begin()))
                            ++wrong;
                    } else {
                        store->keep(index * kChunk, piece);
                    }
                }
            });
        for (auto& worker : workers)
            worker.join();
        Bytes whole(wide.bytes->size());
        expect(wrong.load() == 0 && store->read(0, whole) && whole == *wide.bytes,
               "concurrent fills and reads agree with the source");
    }
}

int main() {
    testChunks();
    testIdentity();
    testRestartAndIndex();
    testCorruption();
    testPolicy();
    testDefaultHeadroom();
    testLiveHeadroom();
    testLoopLargerThanTheSpace(std::chrono::seconds(1), false);
    testLoopLargerThanTheSpace(std::chrono::minutes(2), false);
    testEntryLimit();
    testPublishedSettings();
    testWriteQueue();
    testWriteQueueIsLiveWork();
    testShutdownAndCancellation();
    testLegacyEntriesRemoved();
    testConcurrency();
    if (failures == 0)
        std::cout << "PASS: source mirror\n";
    return failures == 0 ? 0 : 1;
}
