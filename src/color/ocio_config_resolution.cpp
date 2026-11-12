#include <bloom/color/ocio_config_resolution.hpp>

#include "lut_resource_budget.hpp"
#include "lut_worker_protocol.hpp"
#include "ocio_config_protocol.hpp"
#include "ocio_config_session.hpp"
#include "ocio_internal.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_content_revision.hpp>
#include <bloom/color/ocio_loose_content_revision.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <new>
#include <utility>

#ifdef __linux__
#include <dirent.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif

namespace bloom::color {
namespace {

constexpr std::string_view kProjectArchivePrefix = "bloom-project-ocioz:";
constexpr std::string_view kArchiveSuffix = ".ocioz";
constexpr std::string_view kLooseName = "config.ocio";

[[nodiscard]] bool endsWith(const std::string_view text, const std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

[[nodiscard]] std::string escapeValue(const std::string_view value) {
    std::string text;
    for (const char character : value) {
        if (character == '%')
            text += "%25";
        else if (character == '\n')
            text += "%0A";
        else
            text += character;
    }
    return text;
}

[[nodiscard]] int hexDigit(const char character) noexcept {
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

// Percent-decodes; empty when an escape is malformed or decodes to NUL.
[[nodiscard]] std::optional<std::string> percentDecode(const std::string_view text) {
    std::string value;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '%') {
            value += text[index];
            continue;
        }
        if (index + 2 >= text.size())
            return std::nullopt;
        const int high = hexDigit(text[index + 1]);
        const int low = hexDigit(text[index + 2]);
        if (high < 0 || low < 0 || (high == 0 && low == 0))
            return std::nullopt;
        value += static_cast<char>(high * 16 + low);
        index += 2;
    }
    return value;
}

#ifdef __linux__
// Used only by the file resolution below, which needs the confined helper.

// The local absolute path a file location names; empty when it names none on this machine (a
// relative path, a file URI with an authority, a malformed escape).
[[nodiscard]] std::string localPath(const std::string_view value) {
    constexpr std::string_view scheme = "file:";
    if (value.substr(0, scheme.size()) == scheme) {
        auto rest = value.substr(scheme.size());
        if (rest.substr(0, 2) == "//") {
            // file://<authority>/path: only this machine is local.
            const auto slash = rest.find('/', 2);
            const auto authority =
                rest.substr(2, slash == std::string_view::npos ? rest.size() - 2 : slash - 2);
            if (slash == std::string_view::npos || !(authority.empty() || authority == "localhost"))
                return {};
            rest = rest.substr(slash);
        }
        if (rest.empty() || rest.front() != '/')
            return {};
        const auto decoded = percentDecode(rest);
        return decoded ? *decoded : std::string{};
    }
    if (!value.empty() && value.front() == '/' && value.find('\0') == std::string_view::npos)
        return std::string(value);
    return {};
}
#endif

[[nodiscard]] OcioBuiltInResolutionResult helperFailed(const OcioHelperError error) noexcept {
    return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper,
                                             {}, OcioBuiltInInvalidReason::None, error);
}

#ifdef __linux__

[[nodiscard]] OcioBuiltInResolutionResult
invalid(const OcioBuiltInInvalidReason reason,
        const std::optional<core::Sha256Digest>& revision = {}) noexcept {
    return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::Invalid, revision, reason);
}

// What identifies a file's content cheaply between two resolutions.
struct FileStamp final {
    std::string key;
    std::uint64_t size = 0;
    std::int64_t seconds = 0;
    std::int64_t nanoseconds = 0;
    std::uint64_t inode = 0;
    std::uint64_t device = 0;
    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

[[nodiscard]] FileStamp stamp(std::string key, const struct stat& info) {
    return {std::move(key),
            static_cast<std::uint64_t>(info.st_size),
            static_cast<std::int64_t>(info.st_mtim.tv_sec),
            static_cast<std::int64_t>(info.st_mtim.tv_nsec),
            static_cast<std::uint64_t>(info.st_ino),
            static_cast<std::uint64_t>(info.st_dev)};
}

enum class Scan : std::uint8_t { Ok, Missing, TooLarge };

// Every candidate regular file under a loose root, by canonical root-relative key. Nothing is
// opened or interpreted: names and sizes only, symbolic links never followed. A file larger than
// one resource may be is not a candidate.
[[nodiscard]] Scan scanLooseRoot(const int directory, const std::string& prefix, const int depth,
                                 std::vector<FileStamp>& files) {
    const int copy = ::dup(directory);
    DIR* const stream = copy < 0 ? nullptr : ::fdopendir(copy);
    if (stream == nullptr) {
        if (copy >= 0)
            ::close(copy);
        return Scan::Missing;
    }
    struct Close final {
        DIR* stream;
        ~Close() { ::closedir(stream); }
    } close{stream};
    ::rewinddir(stream);
    while (const dirent* const entry = ::readdir(stream)) {
        const std::string_view name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        struct stat info{};
        if (::fstatat(directory, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        const auto key = prefix + std::string(name);
        if (S_ISDIR(info.st_mode)) {
            if (depth >= 16)
                continue;
            const detail::LutFd child(::openat(directory, entry->d_name,
                                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (child.get() < 0)
                continue;
            if (const auto scanned = scanLooseRoot(child.get(), key + "/", depth + 1, files);
                scanned != Scan::Ok)
                return scanned;
            continue;
        }
        if (!S_ISREG(info.st_mode) || key.size() > kOcioLooseMaximumKeyBytes ||
            detail::canonicalConfigResourceKey(key) != key ||
            static_cast<std::uint64_t>(info.st_size) > kOcioLooseMaximumResourceBytes)
            continue;
        if (files.size() >= kOcioLooseMaximumEntryCount)
            return Scan::TooLarge;
        files.push_back(stamp(key, info));
    }
    return Scan::Ok;
}

// Opens `key` beneath `root` without following any symbolic link.
[[nodiscard]] int openBeneath(const int root, const std::string& key) {
    open_how how{};
    how.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    return static_cast<int>(::syscall(SYS_openat2, root, key.c_str(), &how, sizeof(how)));
}

// Appends exactly `size` bytes of `source` to `target`.
[[nodiscard]] bool copyBytes(const int source, const int target, std::uint64_t size) {
    std::vector<std::byte> buffer(std::size_t{1} << 20);
    while (size > 0) {
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(size, buffer.size()));
        const auto count = ::read(source, buffer.data(), want);
        if (count <= 0 ||
            !detail::writeFd(target, std::span(buffer).first(static_cast<std::size_t>(count))))
            return false;
        size -= static_cast<std::uint64_t>(count);
    }
    // The file must end where its size said it does.
    std::byte extra{};
    return ::read(source, &extra, 1) == 0;
}

class ReadOnlyMapping final {
  public:
    ReadOnlyMapping(const int fd, const std::size_t size)
        : data_(size == 0 ? MAP_FAILED : ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0)),
          size_(size) {}
    ~ReadOnlyMapping() {
        if (data_ != MAP_FAILED)
            ::munmap(data_, size_);
    }
    ReadOnlyMapping(const ReadOnlyMapping&) = delete;
    ReadOnlyMapping& operator=(const ReadOnlyMapping&) = delete;
    [[nodiscard]] bool valid() const noexcept { return data_ != MAP_FAILED; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {static_cast<const std::byte*>(data_), size_};
    }

  private:
    void* data_;
    std::size_t size_;
};

struct Key final {
    OcioConfigLocation location;
    std::string working;
    friend bool operator==(const Key&, const Key&) = default;
};

struct Kept final {
    Key key;
    std::vector<FileStamp> stamps;
    ResolvedBloomNeutralConfig resolved;
    std::shared_ptr<detail::ConfigSession> session;
};

// The kept configs and the resolutions in progress. The mutex guards only these two lists and is
// never held while a file is read or a helper parses: a slow config delays only a second request
// for that same config, which waits for it and then finds it kept.
struct KeptConfigs final {
    std::mutex mutex;
    std::condition_variable finished;
    std::deque<Kept> entries;
    std::vector<Key> inProgress;
};

[[nodiscard]] KeptConfigs& kept() {
    static KeptConfigs value;
    return value;
}

// The current stamps of a location's files: the archive, or every candidate of a loose root.
// Empty with Missing when the location does not name a readable regular file.
[[nodiscard]] Scan currentStamps(const OcioConfigLocation& location, const std::string& path,
                                 std::vector<FileStamp>& stamps) {
    stamps.clear();
    if (path.empty())
        return Scan::Missing;
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return Scan::Missing;
    if (location.kind != OcioConfigLocatorKind::ExternalLooseConfig) {
        stamps.push_back(stamp({}, info));
        return Scan::Ok;
    }
    const auto slash = path.rfind('/');
    const auto root = slash == 0 ? std::string("/") : path.substr(0, slash);
    const detail::LutFd directory(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0)
        return Scan::Missing;
    if (const auto scanned = scanLooseRoot(directory.get(), {}, 0, stamps); scanned != Scan::Ok)
        return scanned;
    std::ranges::sort(stamps, {}, &FileStamp::key);
    return Scan::Ok;
}

[[nodiscard]] OcioBuiltInResolutionResult answer(const Kept& entry,
                                                 const core::Sha256Digest& expectedRevision) {
    if (entry.resolved.expectedRevision() != expectedRevision)
        return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::Changed,
                                                 entry.resolved.expectedRevision());
    return detail::OcioConfigAccess::ready(entry.resolved);
}

[[nodiscard]] bool cancelled(const std::function<bool()>& cancellation) {
    return cancellation && cancellation();
}

[[nodiscard]] OcioBuiltInResolutionResult
resolveFile(const OcioConfigLocation& location, const core::Sha256Digest& expectedRevision,
            const std::string_view working, const std::function<bool()>& cancellation) {
    const auto path = localPath(location.value);
    const bool loose = location.kind == OcioConfigLocatorKind::ExternalLooseConfig;
    if (path.empty() || !endsWith(path, loose ? kLooseName : kArchiveSuffix) ||
        (loose && path.size() > kLooseName.size() &&
         path[path.size() - kLooseName.size() - 1] != '/'))
        return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::Missing);

    auto& cache = kept();
    if (cancelled(cancellation))
        return helperFailed(OcioHelperError::HelperCancelled);
    std::vector<FileStamp> stamps;
    switch (currentStamps(location, path, stamps)) {
    case Scan::Missing:
        return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::Missing);
    case Scan::TooLarge:
        return invalid(OcioBuiltInInvalidReason::ResourceTableLimitExceeded);
    case Scan::Ok:
        break;
    }
    const Key key{location, std::string(working)};
    // A kept config answers at once. A config another caller is parsing right now is waited for
    // (cancellably) and then found kept, or parsed here if that attempt did not keep it. Any other
    // config parses without waiting for either.
    struct Claim final {
        KeptConfigs& cache;
        const Key& key;
        bool held = false;
        ~Claim() {
            if (!held)
                return;
            {
                const std::lock_guard lock(cache.mutex);
                std::erase(cache.inProgress, key);
            }
            cache.finished.notify_all();
        }
    } claim{cache, key};
    {
        std::unique_lock lock(cache.mutex);
        while (true) {
            for (auto entry = cache.entries.begin(); entry != cache.entries.end();) {
                if (entry->session->failure() != OcioHelperError::None ||
                    (entry->key == key && entry->stamps != stamps)) {
                    entry = cache.entries.erase(entry);
                    continue;
                }
                if (entry->key == key)
                    return answer(*entry, expectedRevision);
                ++entry;
            }
            if (std::ranges::find(cache.inProgress, key) == cache.inProgress.end()) {
                cache.inProgress.push_back(key);
                claim.held = true;
                break;
            }
            if (cancelled(cancellation))
                return helperFailed(OcioHelperError::HelperCancelled);
            cache.finished.wait_for(lock, std::chrono::milliseconds(20));
        }
    }

    // Read the files into one sealed shared file: the archive itself, or the loose resource
    // table. The same sealed bytes are hashed here and parsed in the helper.
    const detail::LutFd sealed(detail::sharedFile());
    if (sealed.get() < 0)
        return helperFailed(OcioHelperError::HelperMemoryLimit);
    std::uint64_t sealedBytes = 0;
    std::vector<detail::ConfigResourceSlice> index;
    if (!loose) {
        const detail::LutFd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        struct stat info{};
        if (file.get() < 0 || ::fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode))
            return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::Missing);
        sealedBytes = static_cast<std::uint64_t>(info.st_size);
        if (sealedBytes == 0 || sealedBytes > detail::kConfigMaximumArchiveBytes)
            return invalid(sealedBytes == 0 ? OcioBuiltInInvalidReason::ParseFailed
                                            : OcioBuiltInInvalidReason::ResourceTableLimitExceeded);
        stamps = {stamp({}, info)};
        if (!copyBytes(file.get(), sealed.get(), sealedBytes))
            return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::Missing);
    } else {
        const auto root = path.size() == kLooseName.size() + 1
                              ? std::string("/")
                              : path.substr(0, path.size() - kLooseName.size() - 1);
        const detail::LutFd directory(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (directory.get() < 0)
            return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::Missing);
        std::uint64_t aggregate = 0;
        for (const auto& file : stamps) {
            aggregate += file.size;
            if (file.key == kLooseName && file.size > kOcioLooseMaximumConfigBytes)
                return invalid(OcioBuiltInInvalidReason::ResourceTableLimitExceeded);
        }
        if (aggregate > kOcioLooseMaximumAggregateBytes)
            return invalid(OcioBuiltInInvalidReason::ResourceTableLimitExceeded);
        detail::ConfigWriter header;
        header.u32(detail::kConfigRecordVersion);
        header.u32(static_cast<std::uint32_t>(stamps.size()));
        auto bytes = std::move(header).take();
        if (!detail::writeFd(sealed.get(), bytes))
            return helperFailed(OcioHelperError::HelperMemoryLimit);
        sealedBytes = bytes.size();
        for (auto& file : stamps) {
            const detail::LutFd source(openBeneath(directory.get(), file.key));
            struct stat info{};
            // A file that vanished or changed kind since the scan: the root is changing under
            // the read, so nothing is published from it.
            if (source.get() < 0 || ::fstat(source.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
                static_cast<std::uint64_t>(info.st_size) != file.size)
                return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::Missing);
            file = stamp(file.key, info);
            detail::ConfigWriter entry;
            entry.text(file.key);
            entry.u64(file.size);
            bytes = std::move(entry).take();
            if (!detail::writeFd(sealed.get(), bytes))
                return helperFailed(OcioHelperError::HelperMemoryLimit);
            sealedBytes += bytes.size();
            index.push_back({file.key, sealedBytes, file.size});
            if (!copyBytes(source.get(), sealed.get(), file.size))
                return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::Missing);
            sealedBytes += file.size;
        }
        if (std::ranges::none_of(index, [](const auto& slice) { return slice.key == kLooseName; }))
            return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::Missing);
    }
    if (!detail::sealInput(sealed.get()))
        return helperFailed(OcioHelperError::HelperMemoryLimit);
    const ReadOnlyMapping mapping(sealed.get(), static_cast<std::size_t>(sealedBytes));
    if (!mapping.valid())
        return helperFailed(OcioHelperError::HelperMemoryLimit);

    // An archive's revision covers the whole file, so a changed archive is known before any
    // helper starts.
    std::optional<core::Sha256Digest> revision;
    if (!loose) {
        if (!detail::looksLikeConfigArchive(
                mapping.bytes().first(std::min<std::size_t>(mapping.bytes().size(), 4))))
            return invalid(OcioBuiltInInvalidReason::ParseFailed);
        const auto computed = computeOcioContentRevisionV1(
            location.kind == OcioConfigLocatorKind::ProjectRelativeArchive
                ? OcioContentLocatorKind::ProjectRelativeArchive
                : OcioContentLocatorKind::ExternalArchive,
            mapping.bytes());
        if (!computed)
            return invalid(OcioBuiltInInvalidReason::ResourceTableLimitExceeded);
        revision = *computed.revision();
        if (*revision != expectedRevision)
            return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::Changed, revision);
    }

    detail::ConfigParameters parameters;
    parameters.kind = location.kind;
    parameters.workingColorSpaceId = working;
    for (const auto& variable : location.context)
        parameters.context.push_back({variable.name, variable.value});
    auto opened = detail::ConfigSession::open(parameters, sealed.get(), sealedBytes, cancellation);
    if (!opened.session || !opened.reply)
        return helperFailed(opened.helperError == OcioHelperError::None
                                ? OcioHelperError::HelperProtocolViolation
                                : opened.helperError);
    const auto& reply = *opened.reply;

    if (loose) {
        // The loose revision covers exactly the resources the config can read. Every consumed
        // key must be one that was supplied, in strict order, with config.ocio among them.
        std::vector<OcioLooseResourceView> consumed;
        bool sound = !reply.consumedKeys.empty();
        for (std::size_t at = 0; at < reply.consumedKeys.size() && sound; ++at) {
            const auto& name = reply.consumedKeys[at];
            const auto found = std::ranges::find(index, name, &detail::ConfigResourceSlice::key);
            sound = found != index.end() && (at == 0 || reply.consumedKeys[at - 1] < name);
            if (sound)
                consumed.push_back(
                    {found->key, mapping.bytes().subspan(static_cast<std::size_t>(found->offset),
                                                         static_cast<std::size_t>(found->size))});
        }
        sound =
            sound && std::ranges::find(reply.consumedKeys, kLooseName) != reply.consumedKeys.end();
        if (!sound)
            return helperFailed(OcioHelperError::HelperProtocolViolation);
        const auto computed = computeOcioLooseContentRevisionV1(consumed);
        if (!computed)
            return invalid(OcioBuiltInInvalidReason::ResourceTableLimitExceeded);
        revision = *computed.revision();
    }
    switch (reply.outcome) {
    case detail::ConfigOpenOutcome::Invalid:
        return invalid(reply.invalidReason == OcioBuiltInInvalidReason::None
                           ? OcioBuiltInInvalidReason::ParseFailed
                           : reply.invalidReason,
                       revision);
    case detail::ConfigOpenOutcome::MissingResource:
        return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::MissingResource,
                                                 revision);
    case detail::ConfigOpenOutcome::Ready:
        break;
    }
    auto ready = detail::OcioConfigAccess::remote(opened.session, *revision, reply.description);
    auto resolved = std::move(ready).takeResolved();
    if (!resolved)
        return helperFailed(OcioHelperError::HelperProtocolViolation);
    constexpr std::size_t maximumKept = 4;
    const std::lock_guard lock(cache.mutex);
    while (cache.entries.size() >= maximumKept)
        cache.entries.pop_front();
    cache.entries.push_back({key, std::move(stamps), std::move(*resolved), opened.session});
    return answer(cache.entries.back(), expectedRevision);
}

#endif

} // namespace

std::string ocioConfigLocationText(const OcioConfigLocation& location) {
    std::string text;
    if (location.kind == OcioConfigLocatorKind::ProjectRelativeArchive)
        text += kProjectArchivePrefix;
    text += location.value;
    for (const auto& variable : location.context) {
        text += '\n';
        text += variable.name;
        text += '=';
        text += escapeValue(variable.value);
    }
    return text;
}

OcioConfigLocation parseOcioConfigLocation(const std::string_view text) {
    OcioConfigLocation location;
    const auto firstBreak = text.find('\n');
    auto head = text.substr(0, firstBreak);
    auto rest =
        firstBreak == std::string_view::npos ? std::string_view{} : text.substr(firstBreak + 1);
    while (!rest.empty()) {
        const auto cut = rest.find('\n');
        const auto line = rest.substr(0, cut);
        rest = cut == std::string_view::npos ? std::string_view{} : rest.substr(cut + 1);
        const auto equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;
        auto value = percentDecode(line.substr(equals + 1));
        location.context.push_back(
            {std::string(line.substr(0, equals)), value ? std::move(*value) : std::string{}});
    }
    if (head.substr(0, kProjectArchivePrefix.size()) == kProjectArchivePrefix) {
        location.kind = OcioConfigLocatorKind::ProjectRelativeArchive;
        head.remove_prefix(kProjectArchivePrefix.size());
    } else if (head.substr(0, 8) == "bloom://" || head.substr(0, 7) == "ocio://") {
        location.kind = OcioConfigLocatorKind::BloomBuiltIn;
    } else if (endsWith(head, kArchiveSuffix)) {
        location.kind = OcioConfigLocatorKind::ExternalArchive;
    } else if (endsWith(head, kLooseName)) {
        location.kind = OcioConfigLocatorKind::ExternalLooseConfig;
    }
    location.value = head;
    return location;
}

OcioBuiltInResolutionResult resolveOcioConfig(const OcioConfigLocation& location,
                                              const core::Sha256Digest& expectedRevision,
                                              const std::string_view workingColorSpaceId,
                                              const std::function<bool()>& cancellation) noexcept {
    if (location.kind == OcioConfigLocatorKind::BloomBuiltIn)
        return resolveOcioBuiltIn(location.kind, location.value, expectedRevision,
                                  workingColorSpaceId);
#ifdef __linux__
    try {
        return resolveFile(location, expectedRevision, workingColorSpaceId, cancellation);
    } catch (const std::bad_alloc&) {
        return helperFailed(OcioHelperError::HelperMemoryLimit);
    } catch (const std::exception&) {
        return helperFailed(OcioHelperError::HelperProtocolViolation);
    }
#else
    return helperFailed(OcioHelperError::HelperUnavailable);
#endif
}

OcioBuiltInResolutionResult resolveOcioConfig(const std::string_view locationText,
                                              const core::Sha256Digest& expectedRevision,
                                              const std::string_view workingColorSpaceId,
                                              const std::function<bool()>& cancellation) noexcept {
    try {
        return resolveOcioConfig(parseOcioConfigLocation(locationText), expectedRevision,
                                 workingColorSpaceId, cancellation);
    } catch (const std::exception&) {
        return helperFailed(OcioHelperError::HelperMemoryLimit);
    }
}

std::optional<core::Sha256Digest>
ocioConfigContentRevision(const OcioConfigLocation& location) noexcept {
    if (location.kind == OcioConfigLocatorKind::BloomBuiltIn)
        return ocioBuiltInContentRevision(location.kind, location.value);
    // No revision is expected yet: the resolution answers Changed with the current one.
    const auto result = resolveOcioConfig(location, core::Sha256Digest{}, {});
    return result.recomputedRevision();
}

void releaseExternalOcioConfigs() noexcept {
#ifdef __linux__
    auto& cache = kept();
    const std::lock_guard lock(cache.mutex);
    cache.entries.clear();
#endif
}

} // namespace bloom::color
