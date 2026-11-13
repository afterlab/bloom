// #718: a snapshot that is usable before it is whole. Byte ranges are made local on demand, from
// the chunk store when it holds them and from the source otherwise; what the source gave is
// kept; the digest is proved once the whole file is local.
#include <bloom/platform/file_snapshot.hpp>
#include <bloom/platform/source_identity.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace bloom::platform;
class Expectations final {
  public:
    void expect(const bool condition, const std::string_view message) {
        if (condition)
            return;
        ++failures_;
        std::cerr << "FAILED: " << message << '\n';
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

constexpr std::uint64_t kChunk = ReadOnlyFileSnapshot::kProgressiveChunkBytes;

std::vector<std::byte> pattern(const std::size_t size) {
    std::vector<std::byte> bytes(size);
    std::uint32_t value = 0x9e3779b9U;
    for (auto& byte : bytes) {
        value = value * 1664525U + 1013904223U;
        byte = static_cast<std::byte>(value >> 24U);
    }
    return bytes;
}
void write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

// Records what the snapshot keeps and, when `serve` is set, answers from `content`.
class RecordingStore final : public SourceChunkStore {
  public:
    explicit RecordingStore(std::vector<std::byte> content = {}, const bool serve = false)
        : content_(std::move(content)), serve_(serve) {}
    bool read(const std::uint64_t offset, std::span<std::byte> bytes) override {
        const std::lock_guard lock(mutex_);
        ++reads_;
        if (!serve_ || offset + bytes.size() > content_.size())
            return false;
        std::copy_n(content_.begin() + static_cast<std::ptrdiff_t>(offset), bytes.size(),
                    bytes.begin());
        return true;
    }
    void keep(const std::uint64_t offset, std::span<const std::byte> bytes) override {
        const std::lock_guard lock(mutex_);
        kept_[offset].assign(bytes.begin(), bytes.end());
        ++keeps_;
    }
    [[nodiscard]] bool underPressure() const override { return pressure.load(); }
    [[nodiscard]] std::size_t keeps() const {
        const std::lock_guard lock(mutex_);
        return keeps_;
    }
    [[nodiscard]] std::size_t reads() const {
        const std::lock_guard lock(mutex_);
        return reads_;
    }
    [[nodiscard]] std::vector<std::byte> kept(const std::uint64_t offset) const {
        const std::lock_guard lock(mutex_);
        const auto found = kept_.find(offset);
        return found == kept_.end() ? std::vector<std::byte>{} : found->second;
    }
    std::atomic_bool pressure{false};

  private:
    mutable std::mutex mutex_;
    std::vector<std::byte> content_;
    bool serve_ = false;
    std::map<std::uint64_t, std::vector<std::byte>> kept_;
    std::size_t keeps_ = 0, reads_ = 0;
};

std::unique_ptr<ReadOnlyFileSnapshot> take(ProcessResult<std::unique_ptr<ReadOnlyFileSnapshot>> made) {
    auto* snapshot = std::get_if<std::unique_ptr<ReadOnlyFileSnapshot>>(&made);
    return snapshot ? std::move(*snapshot) : nullptr;
}
} // namespace

int main() {
    Expectations expectations;
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("bloom-progressive-snapshot-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto spool = directory / "spool";
    const auto source = directory / "clip.mov";
    // Three whole chunks and a half one.
    const auto bytes = pattern(static_cast<std::size_t>(3 * kChunk + kChunk / 2));
    write(source, bytes);
    const auto digest = *bloom::core::Sha256Hasher::hash(bytes);
    const auto quota = std::make_shared<FileSnapshotQuota>(std::uint64_t{64} * kChunk, 4);
    const auto request = [&](std::shared_ptr<SourceChunkStore> chunks, const bool background) {
        ProgressiveSnapshotRequest value;
        value.source = source;
        value.spoolDirectory = spool;
        value.quota = quota;
        value.sourceLimit = std::uint64_t{1} << 40U;
        value.claimed = digest;
        value.chunks = std::move(chunks);
        value.backgroundFill = background;
        return value;
    };

    {
        const auto store = std::make_shared<RecordingStore>();
        const auto snapshot = take(ReadOnlyFileSnapshot::captureProgressive(request(store, false)));
        expectations.expect(snapshot != nullptr, "a progressive snapshot opens");
        if (!snapshot)
            return 1;
        expectations.expect(snapshot->progressive() && snapshot->size() == bytes.size() &&
                                snapshot->digest() == digest && snapshot->localBytes() == 0 &&
                                snapshot->verification() == SnapshotVerification::Pending &&
                                store->keeps() == 0,
                            "it has the source's length and claimed digest and nothing local yet");
        expectations.expect(quota->residentBytes() == bytes.size() && quota->residentFiles() == 1,
                            "the whole length is charged to the disk quota at once");
        // One byte range in the third chunk: that chunk and no other is read and kept.
        auto made = snapshot->ensureRange(2 * kChunk + 300, 100);
        const auto* range = std::get_if<SnapshotRange>(&made);
        expectations.expect(range && range->begin == 2 * kChunk && range->end == 3 * kChunk &&
                                !range->complete && store->keeps() == 1 &&
                                snapshot->localBytes() == kChunk,
                            "a range brings in the chunk that holds it");
        const auto kept = store->kept(2 * kChunk);
        expectations.expect(kept.size() == kChunk &&
                                std::equal(kept.begin(), kept.end(),
                                           bytes.begin() + static_cast<std::ptrdiff_t>(2 * kChunk)),
                            "what the source gave is handed to the chunk store, byte for byte");
        // The tail, asked past the end: clamped, and reported with its local neighbor.
        made = snapshot->ensureRange(3 * kChunk + 10, 8 * kChunk);
        range = std::get_if<SnapshotRange>(&made);
        expectations.expect(range && range->begin == 2 * kChunk && range->end == bytes.size() &&
                                !range->complete && store->keeps() == 2 &&
                                store->kept(3 * kChunk).size() == kChunk / 2,
                            "a range past the end is clamped and names what is local around it");
        made = snapshot->ensureRange(2 * kChunk, kChunk);
        expectations.expect(std::holds_alternative<SnapshotRange>(made) && store->keeps() == 2,
                            "a local range is not read again");
        expectations.expect(std::holds_alternative<ProcessFailure>(
                                snapshot->ensureRange(bytes.size() + 1, 1)),
                            "a range that starts past the end is refused");
        // The rest, then the proof: the hash of the local file is the claimed digest.
        std::uint64_t reported = 0;
        const auto verified = snapshot->waitVerified(
            {}, [&](const std::uint64_t done, std::uint64_t) { reported = done; });
        const auto* state = std::get_if<SnapshotVerification>(&verified);
        expectations.expect(state && *state == SnapshotVerification::Verified &&
                                snapshot->verification() == SnapshotVerification::Verified &&
                                snapshot->localBytes() == bytes.size() &&
                                reported == bytes.size() && store->keeps() == 4,
                            "the whole file is local and hashes to the claimed digest");
        made = snapshot->ensureRange(0, 1);
        range = std::get_if<SnapshotRange>(&made);
        expectations.expect(range && range->begin == 0 && range->end == bytes.size() &&
                                range->complete,
                            "a complete snapshot reports the whole file");
        const auto observed = observeSource(source);
        expectations.expect(observed.digest && *observed.digest == digest,
                            "a verified fill records the source's identity for its digest");
    }
    expectations.expect(quota->residentBytes() == 0 && quota->residentFiles() == 0,
                        "closing the snapshot returns its disk quota");
    expectations.expect(!std::filesystem::exists(spool) || std::filesystem::is_empty(spool),
                        "the private file leaves no name behind");

    {
        // A store that already holds the file: nothing is read from the source or kept again.
        const auto store = std::make_shared<RecordingStore>(bytes, true);
        const auto snapshot = take(ReadOnlyFileSnapshot::captureProgressive(request(store, false)));
        const auto verified = snapshot ? snapshot->waitVerified()
                                       : ProcessResult<SnapshotVerification>{ProcessFailure{}};
        const auto* state = std::get_if<SnapshotVerification>(&verified);
        expectations.expect(state && *state == SnapshotVerification::Verified &&
                                store->keeps() == 0 && store->reads() == 4,
                            "chunks the store holds are served locally and kept once only");
    }
    {
        // A digest the bytes do not have: the fill completes and the proof fails.
        auto wrong = request(nullptr, false);
        wrong.claimed = *bloom::core::Sha256Hasher::hash(std::span(bytes).first(16));
        const auto snapshot = take(ReadOnlyFileSnapshot::captureProgressive(wrong));
        const auto verified = snapshot ? snapshot->waitVerified()
                                       : ProcessResult<SnapshotVerification>{ProcessFailure{}};
        const auto* state = std::get_if<SnapshotVerification>(&verified);
        expectations.expect(state && *state == SnapshotVerification::Failed,
                            "bytes that do not hash to the claimed digest fail verification");
    }
    {
        // Several threads asking for overlapping ranges: each chunk is read once.
        const auto store = std::make_shared<RecordingStore>();
        const auto snapshot = take(ReadOnlyFileSnapshot::captureProgressive(request(store, false)));
        std::atomic_int failures{0};
        std::vector<std::thread> threads;
        for (int thread = 0; snapshot && thread < 4; ++thread)
            threads.emplace_back([&] {
                for (std::uint64_t offset = 0; offset < bytes.size(); offset += kChunk / 3)
                    if (!std::holds_alternative<SnapshotRange>(
                            snapshot->ensureRange(offset, kChunk)))
                        ++failures;
            });
        for (auto& thread : threads)
            thread.join();
        const auto verified = snapshot ? snapshot->waitVerified()
                                       : ProcessResult<SnapshotVerification>{ProcessFailure{}};
        const auto* state = std::get_if<SnapshotVerification>(&verified);
        expectations.expect(failures.load() == 0 && store->keeps() == 4 && state &&
                                *state == SnapshotVerification::Verified,
                            "concurrent ranges read each chunk once and the file verifies");
    }
    {
        // The background fill completes and verifies on its own, and holds off under pressure.
        const auto store = std::make_shared<RecordingStore>();
        store->pressure.store(true);
        const auto snapshot = take(ReadOnlyFileSnapshot::captureProgressive(request(store, true)));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        expectations.expect(snapshot && snapshot->localBytes() == 0,
                            "the background fill holds off while the store is under pressure");
        auto made = snapshot ? snapshot->ensureRange(kChunk, 1)
                             : ProcessResult<SnapshotRange>{ProcessFailure{}};
        expectations.expect(std::holds_alternative<SnapshotRange>(made) && store->keeps() == 1,
                            "a frame's range is served under pressure all the same");
        store->pressure.store(false);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (snapshot && snapshot->verification() == SnapshotVerification::Pending &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        expectations.expect(snapshot &&
                                snapshot->verification() == SnapshotVerification::Verified &&
                                store->keeps() == 4,
                            "the background fill brings in the rest and verifies it");
    }
    {
        // Closing a snapshot in the middle of its background fill ends the fill.
        const auto store = std::make_shared<RecordingStore>();
        store->pressure.store(true);
        auto snapshot = take(ReadOnlyFileSnapshot::captureProgressive(request(store, true)));
        snapshot.reset();
        expectations.expect(quota->residentFiles() == 0,
                            "a snapshot closes while its fill is waiting");
    }
    {
        // A source that shrank: the missing range fails and the snapshot stays usable.
        const auto snapshot = take(ReadOnlyFileSnapshot::captureProgressive(request(nullptr, false)));
        std::filesystem::resize_file(source, kChunk);
        const bool tailFails = snapshot && std::holds_alternative<ProcessFailure>(
                                               snapshot->ensureRange(3 * kChunk, 1));
        const bool headWorks =
            snapshot && std::holds_alternative<SnapshotRange>(snapshot->ensureRange(0, 1));
        expectations.expect(tailFails && headWorks,
                            "a range the source no longer has fails; other ranges still work");
        write(source, bytes);
    }
    {
        // An ordinary capture is whole and verified from the start.
        const auto snapshot = take(ReadOnlyFileSnapshot::capture(source, spool, quota,
                                                                 std::uint64_t{1} << 40U));
        const auto made = snapshot ? snapshot->ensureRange(5, 5)
                                   : ProcessResult<SnapshotRange>{ProcessFailure{}};
        const auto* range = std::get_if<SnapshotRange>(&made);
        expectations.expect(snapshot && !snapshot->progressive() &&
                                snapshot->verification() == SnapshotVerification::Verified &&
                                snapshot->localBytes() == bytes.size() && range &&
                                range->begin == 0 && range->end == bytes.size() &&
                                range->complete,
                            "an ordinary capture is whole and verified");
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    if (expectations.failures() != 0)
        return 1;
    std::cout << "progressive snapshot tests passed\n";
    return 0;
}
