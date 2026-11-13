#include <algorithm>
#include <array>
#include <atomic>
#include <bloom/platform/source_identity.hpp>
#include <bloom/platform/source_store.hpp>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winioctl.h>
#else
#include <sys/stat.h>
#endif

namespace bloom::platform {
namespace {
#if defined(_WIN32)
// A FILETIME (100 ns ticks since 1601-01-01) as nanoseconds since the Unix epoch, the unit POSIX
// reports, so a modification time recorded on one platform compares on another (#473 stamps).
std::int64_t unixNanoseconds(const LONGLONG ticks) {
    constexpr LONGLONG kUnixEpochTicks = 116444736000000000LL;
    constexpr auto kLimit = std::numeric_limits<std::int64_t>::max() / 100;
    const auto sinceEpoch = ticks - kUnixEpochTicks;
    if (sinceEpoch > kLimit)
        return std::numeric_limits<std::int64_t>::max();
    if (sinceEpoch < -kLimit)
        return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(sinceEpoch) * 100;
}
#else
std::int64_t nanoseconds(const timespec& value) {
    constexpr std::int64_t billion = 1000000000;
    const auto seconds = static_cast<std::int64_t>(value.tv_sec);
    if (seconds > std::numeric_limits<std::int64_t>::max() / billion ||
        seconds < std::numeric_limits<std::int64_t>::min() / billion)
        return seconds < 0 ? std::numeric_limits<std::int64_t>::min()
                           : std::numeric_limits<std::int64_t>::max();
    return seconds * billion + static_cast<std::int64_t>(value.tv_nsec);
}
#endif
void saturatingAdd(std::atomic<std::uint64_t>& counter, const std::uint64_t amount) noexcept {
    auto current = counter.load(std::memory_order_relaxed);
    std::uint64_t next = 0;
    do {
        next = amount > std::numeric_limits<std::uint64_t>::max() - current
                   ? std::numeric_limits<std::uint64_t>::max()
                   : current + amount;
    } while (!counter.compare_exchange_weak(current, next, std::memory_order_relaxed));
}
struct Counters final {
    std::atomic<std::uint64_t> fullReads{0}, fullReadBytes{0}, identityChecks{0}, identityHits{0};
    std::atomic<std::uint64_t> memoryHits{0}, storeHits{0}, storeHitBytes{0};
};
Counters& counters() noexcept {
    static Counters value;
    return value;
}
std::string memoKey(const std::filesystem::path& path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error)
        absolute = path;
    const auto text = absolute.lexically_normal().generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
struct MemoEntry final {
    FileIdentity identity;
    core::Sha256Digest digest;
    std::uint64_t used = 0;
    // False for an identity admitted without a full hash (admit()): it vouches for no digest.
    bool verified = true;
};
struct MemoState final {
    mutable std::mutex mutex;
    std::map<std::string, MemoEntry, std::less<>> entries;
    mutable std::uint64_t clock = 0;
    // #718: verified entries that persist across runs (see persistTo()). `removed` are the
    // paths dropped in this run, so a save does not bring them back from the file.
    std::map<std::string, MemoEntry, std::less<>> persisted;
    std::set<std::string, std::less<>> removed;
    std::filesystem::path file;
    bool loading = false, changed = false;
    std::condition_variable loaded;
    int loadWaitMilliseconds = VerifiedSourceDigests::kPersistedLoadWaitMilliseconds;
    std::size_t loadWaiters = 0;
};
constexpr std::string_view kPersistedHeader = "bloom-verified-sources 1";
// Whether two observations name the same unchanged file, whatever volume id it is mounted under.
bool sameFile(const FileIdentity& a, const FileIdentity& b) noexcept {
    return a.size == b.size && a.modifiedNanoseconds == b.modifiedNanoseconds &&
           a.changedNanoseconds == b.changedNanoseconds && a.fileIdHigh == b.fileIdHigh &&
           a.fileIdLow == b.fileIdLow;
}
// Caller holds the mutex. Remembers a verified entry for the next run, within the limit.
void persistLocked(MemoState& state, const std::string& key, const MemoEntry& entry) {
    if (state.file.empty())
        return;
    state.removed.erase(key);
    state.persisted.insert_or_assign(key, entry);
    if (state.persisted.size() > VerifiedSourceDigests::kPersistedEntryLimit)
        state.persisted.erase(std::ranges::min_element(
            state.persisted, {}, [](const auto& item) { return item.second.used; }));
    state.changed = true;
}
// Caller holds the mutex. The path is known under another identity now, or forgotten.
void unpersistLocked(MemoState& state, const std::string& key) {
    if (state.file.empty())
        return;
    if (state.persisted.erase(key) != 0)
        state.changed = true;
    state.removed.insert(key);
}
// Caller holds `lock`. When the path has no entry of this run, takes the one an earlier run
// persisted for the same file: it becomes an ordinary verified entry under `current`.
void adoptPersistedLocked(MemoState& state, std::unique_lock<std::mutex>& lock,
                          const std::string& key, const FileIdentity& current) {
    if (state.file.empty() || state.entries.contains(key))
        return;
    if (state.loading) {
        ++state.loadWaiters;
        state.loaded.wait_for(lock, std::chrono::milliseconds(state.loadWaitMilliseconds),
                              [&] { return !state.loading; });
        --state.loadWaiters;
    }
    if (state.entries.contains(key))
        return;
    const auto found = state.persisted.find(key);
    if (found == state.persisted.end() || !sameFile(found->second.identity, current))
        return;
    found->second.used = ++state.clock;
    state.entries.insert_or_assign(key,
                                   MemoEntry{current, found->second.digest, state.clock, true});
    if (state.entries.size() > VerifiedSourceDigests::kEntryLimit)
        state.entries.erase(std::ranges::min_element(
            state.entries, {}, [](const auto& item) { return item.second.used; }));
}
// One line per entry: the identity's numbers, the digest, and the path with '%', line feed and
// carriage return written as %25, %0A and %0D (a file name may hold any of them).
std::string persistedLine(const std::string& key, const MemoEntry& entry) {
    std::ostringstream out;
    const auto hex = entry.digest.toLowercaseHex();
    out << entry.identity.size << ' ' << entry.identity.modifiedNanoseconds << ' '
        << entry.identity.changedNanoseconds << ' ' << entry.identity.fileIdHigh << ' '
        << entry.identity.fileIdLow << ' ' << std::string_view(hex.data(), hex.size()) << ' ';
    for (const char value : key)
        if (value == '%')
            out << "%25";
        else if (value == '\n')
            out << "%0A";
        else if (value == '\r')
            out << "%0D";
        else
            out << value;
    return out.str();
}
bool parsePersistedLine(const std::string& line, std::string& key, MemoEntry& entry) {
    std::istringstream in(line);
    std::string hex;
    if (!(in >> entry.identity.size >> entry.identity.modifiedNanoseconds >>
          entry.identity.changedNanoseconds >> entry.identity.fileIdHigh >>
          entry.identity.fileIdLow >> hex) ||
        in.get() != ' ')
        return false;
    const auto digest = core::Sha256Digest::fromLowercaseHex(hex);
    if (!digest)
        return false;
    entry.digest = *digest;
    entry.verified = true;
    entry.used = 0;
    std::string escaped;
    std::getline(in, escaped);
    key.clear();
    for (std::size_t i = 0; i < escaped.size(); ++i) {
        if (escaped[i] != '%') {
            key.push_back(escaped[i]);
            continue;
        }
        const auto code = std::string_view(escaped).substr(i, 3);
        if (code == "%25")
            key.push_back('%');
        else if (code == "%0A")
            key.push_back('\n');
        else if (code == "%0D")
            key.push_back('\r');
        else
            return false;
        i += 2;
    }
    return !key.empty();
}
std::map<std::string, MemoEntry, std::less<>> readPersisted(const std::filesystem::path& file) {
    std::map<std::string, MemoEntry, std::less<>> result;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line) || line != kPersistedHeader)
        return result;
    while (result.size() < VerifiedSourceDigests::kPersistedEntryLimit && std::getline(in, line)) {
        std::string key;
        MemoEntry entry;
        if (parsePersistedLine(line, key, entry))
            result.insert_or_assign(std::move(key), entry);
    }
    return result;
}
MemoState& memoState() noexcept {
    // Never destroyed: a source store that is released while the process exits still saves the
    // record, whatever order the process's other statics go in.
    static auto* const state = new MemoState; // NOLINT(cppcoreguidelines-owning-memory)
    return *state;
}
} // namespace

std::optional<FileIdentity> observeFileIdentity(const std::filesystem::path& path) {
    if (path.empty())
        return {};
#if defined(_WIN32)
    const HANDLE handle = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {};
    struct Closer final {
        HANDLE value;
        ~Closer() { ::CloseHandle(value); }
    } closer{handle};
    FILE_STANDARD_INFO standard{};
    FILE_BASIC_INFO basic{};
    if (!::GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ||
        standard.Directory || standard.EndOfFile.QuadPart < 0 ||
        !::GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)))
        return {};
    FileIdentity identity;
    identity.size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
    identity.modifiedNanoseconds = unixNanoseconds(basic.LastWriteTime.QuadPart);
    identity.changedNanoseconds = basic.ChangeTime.QuadPart;
    // The change time is read from the system clock, which can stand still for several
    // milliseconds: a file rewritten in place inside one tick, with its size and written time put
    // back, keeps its change time too. A volume's change journal numbers every change to a file,
    // so that number is folded in where the volume keeps a journal.
    {
        alignas(USN_RECORD_V2) std::array<std::byte, 1024> record{};
        DWORD returned = 0;
        if (::DeviceIoControl(handle, FSCTL_READ_FILE_USN_DATA, nullptr, 0, record.data(),
                              static_cast<DWORD>(record.size()), &returned, nullptr) &&
            returned >= sizeof(USN_RECORD_COMMON_HEADER)) {
            const auto* header = reinterpret_cast<const USN_RECORD_COMMON_HEADER*>(record.data());
            std::uint64_t number = 0;
            if (header->MajorVersion == 2 && returned >= sizeof(USN_RECORD_V2))
                number = static_cast<std::uint64_t>(
                    reinterpret_cast<const USN_RECORD_V2*>(record.data())->Usn);
            else if (header->MajorVersion == 3 && returned >= sizeof(USN_RECORD_V3))
                number = static_cast<std::uint64_t>(
                    reinterpret_cast<const USN_RECORD_V3*>(record.data())->Usn);
            identity.changedNanoseconds =
                static_cast<std::int64_t>(static_cast<std::uint64_t>(identity.changedNanoseconds) ^
                                          (number * 0x9E3779B97F4A7C15ULL));
        }
    }
    FILE_ID_INFO id{};
    if (::GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))) {
        identity.volume = static_cast<std::uint64_t>(id.VolumeSerialNumber);
        std::array<std::uint8_t, 16> bytes{};
        std::copy(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), bytes.begin());
        // The identifier is a little-endian 128-bit number; a volume with 64-bit file numbers
        // fills only its low half.
        for (std::size_t i = 8; i-- > 0;) {
            identity.fileIdLow = (identity.fileIdLow << 8U) | bytes[i];
            identity.fileIdHigh = (identity.fileIdHigh << 8U) | bytes[i + 8];
        }
    } else {
        BY_HANDLE_FILE_INFORMATION legacy{};
        if (!::GetFileInformationByHandle(handle, &legacy))
            return {};
        identity.volume = legacy.dwVolumeSerialNumber;
        identity.fileIdLow =
            (static_cast<std::uint64_t>(legacy.nFileIndexHigh) << 32U) | legacy.nFileIndexLow;
    }
    return identity;
#else
    struct stat status{};
    if (::stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0)
        return {};
    FileIdentity identity;
    identity.size = static_cast<std::uint64_t>(status.st_size);
#if defined(__APPLE__)
    identity.modifiedNanoseconds = nanoseconds(status.st_mtimespec);
    identity.changedNanoseconds = nanoseconds(status.st_ctimespec);
#else
    identity.modifiedNanoseconds = nanoseconds(status.st_mtim);
    identity.changedNanoseconds = nanoseconds(status.st_ctim);
#endif
    identity.volume = static_cast<std::uint64_t>(status.st_dev);
    identity.fileIdLow = static_cast<std::uint64_t>(status.st_ino);
    return identity;
#endif
}

SourceReadWork sourceReadWork() noexcept {
    auto& value = counters();
    return {value.fullReads.load(std::memory_order_relaxed),
            value.fullReadBytes.load(std::memory_order_relaxed),
            value.identityChecks.load(std::memory_order_relaxed),
            value.identityHits.load(std::memory_order_relaxed)};
}
void recordFullSourceRead(const std::uint64_t bytes) noexcept {
    saturatingAdd(counters().fullReads, 1);
    saturatingAdd(counters().fullReadBytes, bytes);
}
namespace {
struct ProcessStore final {
    std::mutex mutex;
    std::shared_ptr<SourceStore> store;
};
ProcessStore& processStore() {
    static ProcessStore value;
    return value;
}
} // namespace
void setProcessSourceStore(std::shared_ptr<SourceStore> store) {
    auto& value = processStore();
    // The previous store is released outside the lock: releasing the last reference joins the
    // store's own thread.
    std::shared_ptr<SourceStore> previous;
    {
        const std::lock_guard lock(value.mutex);
        previous = std::exchange(value.store, std::move(store));
    }
}
std::shared_ptr<SourceStore> processSourceStore() {
    auto& value = processStore();
    const std::lock_guard lock(value.mutex);
    return value.store;
}
SourceTierWork sourceTierWork() noexcept {
    auto& value = counters();
    return {value.memoryHits.load(std::memory_order_relaxed),
            value.storeHits.load(std::memory_order_relaxed),
            value.storeHitBytes.load(std::memory_order_relaxed)};
}
void recordSourceMemoryHit() noexcept { saturatingAdd(counters().memoryHits, 1); }
void recordSourceStoreHit(const std::uint64_t bytes) noexcept {
    saturatingAdd(counters().storeHits, 1);
    saturatingAdd(counters().storeHitBytes, bytes);
}

std::optional<core::Sha256Digest> VerifiedSourceDigests::find(const std::filesystem::path& path,
                                                              const FileIdentity& identity) const {
    auto& state = memoState();
    const auto key = memoKey(path);
    std::unique_lock lock(state.mutex);
    adoptPersistedLocked(state, lock, key, identity);
    const auto found = state.entries.find(key);
    if (found == state.entries.end() || found->second.identity != identity ||
        !found->second.verified)
        return {};
    found->second.used = ++state.clock;
    return found->second.digest;
}
std::optional<FileIdentity>
VerifiedSourceDigests::knownIdentity(const std::filesystem::path& path,
                                     const FileIdentity* const current) const {
    auto& state = memoState();
    const auto key = memoKey(path);
    std::unique_lock lock(state.mutex);
    if (current != nullptr)
        adoptPersistedLocked(state, lock, key, *current);
    const auto found = state.entries.find(key);
    if (found == state.entries.end())
        return {};
    found->second.used = ++state.clock;
    return found->second.identity;
}
void VerifiedSourceDigests::admit(const std::filesystem::path& path, const FileIdentity& identity) {
    auto& state = memoState();
    auto key = memoKey(path);
    std::unique_lock lock(state.mutex);
    adoptPersistedLocked(state, lock, key, identity);
    if (const auto found = state.entries.find(key);
        found != state.entries.end() && found->second.identity == identity)
        return; // never downgrade a verified entry of the same identity
    // What an earlier run verified for this path stands while it names the same file.
    if (const auto kept = state.persisted.find(key);
        kept != state.persisted.end() && !sameFile(kept->second.identity, identity))
        unpersistLocked(state, key);
    state.entries.insert_or_assign(std::move(key),
                                   MemoEntry{identity, core::Sha256Digest{}, ++state.clock, false});
    if (state.entries.size() > kEntryLimit) {
        const auto oldest = std::ranges::min_element(
            state.entries, {}, [](const auto& entry) { return entry.second.used; });
        state.entries.erase(oldest);
    }
}
void VerifiedSourceDigests::record(const std::filesystem::path& path, const FileIdentity& identity,
                                   const core::Sha256Digest& digest) {
    auto& state = memoState();
    auto key = memoKey(path);
    const std::lock_guard lock(state.mutex);
    const MemoEntry entry{identity, digest, ++state.clock, true};
    if (const auto kept = state.persisted.find(key);
        kept == state.persisted.end() || kept->second.digest != digest ||
        !sameFile(kept->second.identity, identity))
        persistLocked(state, key, entry);
    state.entries.insert_or_assign(std::move(key), entry);
    if (state.entries.size() > kEntryLimit) {
        const auto oldest = std::ranges::min_element(
            state.entries, {}, [](const auto& entry) { return entry.second.used; });
        state.entries.erase(oldest);
    }
}
void VerifiedSourceDigests::forget(const std::filesystem::path& path) {
    auto& state = memoState();
    const auto key = memoKey(path);
    const std::lock_guard lock(state.mutex);
    state.entries.erase(key);
    unpersistLocked(state, key);
}
void VerifiedSourceDigests::clear() {
    auto& state = memoState();
    const std::lock_guard lock(state.mutex);
    state.entries.clear();
    for (const auto& [key, entry] : state.persisted)
        state.removed.insert(key);
    if (!state.persisted.empty())
        state.changed = true;
    state.persisted.clear();
}
void VerifiedSourceDigests::persistTo(const std::filesystem::path& file) {
    auto& state = memoState();
    {
        const std::lock_guard lock(state.mutex);
        state.file = file;
        state.persisted.clear();
        state.removed.clear();
        state.changed = false;
        state.loading = !file.empty();
    }
    state.loaded.notify_all();
}
void VerifiedSourceDigests::load() {
    auto& state = memoState();
    std::filesystem::path file;
    {
        const std::lock_guard lock(state.mutex);
        file = state.file;
    }
    auto read = file.empty() ? std::map<std::string, MemoEntry, std::less<>>{}
                             : readPersisted(file);
    {
        const std::lock_guard lock(state.mutex);
        if (state.file == file) {
            // What this run already recorded or dropped is newer than the file.
            for (auto& [key, entry] : read)
                if (!state.removed.contains(key))
                    state.persisted.try_emplace(key, entry);
            state.loading = false;
        }
    }
    state.loaded.notify_all();
}
void VerifiedSourceDigests::save() {
    auto& state = memoState();
    std::filesystem::path file;
    std::map<std::string, MemoEntry, std::less<>> entries;
    std::set<std::string, std::less<>> removed;
    {
        const std::lock_guard lock(state.mutex);
        if (state.file.empty() || state.loading || !state.changed)
            return;
        state.changed = false;
        file = state.file;
        entries = state.persisted;
        removed = state.removed;
    }
    // Another process of the same user may have saved since this one loaded: its entries stay.
    for (auto& [key, entry] : readPersisted(file))
        if (entries.size() < kPersistedEntryLimit && !removed.contains(key))
            entries.try_emplace(key, entry);
    std::error_code ignored;
    std::filesystem::create_directories(file.parent_path(), ignored);
    std::ostringstream suffix;
    suffix << ".tmp-" << std::this_thread::get_id() << '-'
           << std::chrono::steady_clock::now().time_since_epoch().count();
    auto temporary = file;
    temporary += suffix.str();
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << kPersistedHeader << '\n';
        for (const auto& [key, entry] : entries)
            out << persistedLine(key, entry) << '\n';
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(temporary, ignored);
            const std::lock_guard lock(state.mutex);
            state.changed = true;
            return;
        }
    }
    std::filesystem::rename(temporary, file, ignored);
    if (ignored) {
        std::filesystem::remove(temporary, ignored);
        const std::lock_guard lock(state.mutex);
        state.changed = true;
    }
}
std::filesystem::path VerifiedSourceDigests::persistedFile() const {
    auto& state = memoState();
    const std::lock_guard lock(state.mutex);
    return state.file;
}
void VerifiedSourceDigests::setPersistedLoadWaitForTest(const int milliseconds) {
    auto& state = memoState();
    const std::lock_guard lock(state.mutex);
    state.loadWaitMilliseconds = milliseconds;
}
std::size_t VerifiedSourceDigests::persistedLoadWaitersForTest() const {
    auto& state = memoState();
    const std::lock_guard lock(state.mutex);
    return state.loadWaiters;
}
std::size_t VerifiedSourceDigests::persistedSize() const {
    auto& state = memoState();
    const std::lock_guard lock(state.mutex);
    return state.persisted.size();
}
std::size_t VerifiedSourceDigests::size() const {
    auto& state = memoState();
    const std::lock_guard lock(state.mutex);
    return state.entries.size();
}
VerifiedSourceDigests& verifiedSourceDigests() noexcept {
    static VerifiedSourceDigests value;
    return value;
}

SourceObservation observeSource(const std::filesystem::path& path) {
    SourceObservation observation;
    observation.identity = observeFileIdentity(path);
    saturatingAdd(counters().identityChecks, 1);
    if (observation.identity) {
        observation.digest = verifiedSourceDigests().find(path, *observation.identity);
        if (observation.digest)
            saturatingAdd(counters().identityHits, 1);
    }
    return observation;
}

SourceHash hashSourceFile(const std::filesystem::path& path, const std::function<bool()>& cancel,
                          const std::uint64_t sizeLimit) {
    SourceHash result;
    if (cancel && cancel()) {
        result.error = SourceHashError::Cancelled;
        return result;
    }
    const auto before = observeFileIdentity(path);
    if (!before) {
        result.error = SourceHashError::Missing;
        return result;
    }
    if (before->size > sizeLimit) {
        result.error = SourceHashError::Oversized;
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = SourceHashError::Missing;
        return result;
    }
    core::Sha256Hasher hasher;
    std::array<char, 65536> chunk{};
    std::uint64_t total = 0;
    while (input) {
        if (cancel && cancel()) {
            result.error = SourceHashError::Cancelled;
            return result;
        }
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto count = input.gcount();
        if (count <= 0)
            break;
        total += static_cast<std::uint64_t>(count);
        if (total > sizeLimit) {
            result.error = SourceHashError::Oversized;
            return result;
        }
        if (!hasher.update(
                std::as_bytes(std::span(chunk.data(), static_cast<std::size_t>(count))))) {
            result.error = SourceHashError::Oversized;
            return result;
        }
    }
    recordFullSourceRead(total);
    if (input.bad() || !input.eof()) {
        result.error = SourceHashError::Io;
        return result;
    }
    const auto after = observeFileIdentity(path);
    if (!after || *after != *before || total != before->size) {
        result.error = SourceHashError::Changed;
        return result;
    }
    result.digest = hasher.finalize();
    result.identity = *after;
    verifiedSourceDigests().record(path, result.identity, result.digest);
    return result;
}

SourceHash digestSourceFile(const std::filesystem::path& path, const std::function<bool()>& cancel,
                            const std::uint64_t sizeLimit) {
    if (const auto observed = observeSource(path); observed.digest && observed.identity) {
        if (observed.identity->size > sizeLimit)
            return {.error = SourceHashError::Oversized};
        return {.error = SourceHashError::None,
                .digest = *observed.digest,
                .identity = *observed.identity};
    }
    return hashSourceFile(path, cancel, sizeLimit);
}

SourceCheck verifySourceDigest(const std::filesystem::path& path,
                               const core::Sha256Digest& expected,
                               const std::function<bool()>& cancel, const std::uint64_t sizeLimit) {
    const auto hashed = digestSourceFile(path, cancel, sizeLimit);
    switch (hashed.error) {
    case SourceHashError::None:
        return hashed.digest == expected ? SourceCheck::Matches : SourceCheck::Changed;
    case SourceHashError::Cancelled:
        return SourceCheck::Cancelled;
    case SourceHashError::Missing:
    case SourceHashError::Io:
        return SourceCheck::Missing;
    case SourceHashError::Changed:
    case SourceHashError::Oversized:
        break;
    }
    return SourceCheck::Changed;
}
} // namespace bloom::platform
