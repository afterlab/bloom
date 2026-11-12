#include <bloom/color/ocio_config_resolution.hpp>

#include "lut_resource_budget.hpp"
#include "lut_worker_protocol.hpp"
#include "ocio_config_protocol.hpp"
#include "ocio_config_session.hpp"
#include "ocio_internal.hpp"

#include <bloom/color/bloom_neutral_builtin.hpp>
#include <bloom/color/ocio_content_revision.hpp>
#include <bloom/color/ocio_loose_content_revision.hpp>
#include <bloom/core/file_uri.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <memory>
#include <new>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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

[[nodiscard]] OcioBuiltInResolutionResult helperFailed(const OcioHelperError error) noexcept {
    return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::LocatorKindRequiresHelper,
                                             {}, OcioBuiltInInvalidReason::None, error);
}

// Everything from here to the end of the anonymous namespace reads the files of an external config
// on the host and hands the bytes to the confined helper. Linux and macOS share one POSIX
// implementation (descriptor-relative, never following a symbolic link); Windows has its own
// (handles opened without following a link). The two differ only in how a file is found and
// read: the table, the revision and the helper request that follow are the same code.
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
#define BLOOM_OCIO_HOST_RESOLVES_FILES 1

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

enum class Scan : std::uint8_t { Ok, Missing, TooLarge };

// The local absolute path a file location names; empty when it names none on this machine (a
// relative path, a file URI with an authority, a malformed escape).
[[nodiscard]] std::filesystem::path localPath(const std::string_view value);

// The current stamps of a location's files: the archive, or every candidate of a loose root.
[[nodiscard]] Scan currentStamps(const OcioConfigLocation& location,
                                 const std::filesystem::path& path,
                                 std::vector<FileStamp>& stamps);

// Appends exactly the bytes of the archive at `path` to `bytes`, and refreshes `stamp` from the
// opened file.
enum class ReadResult : std::uint8_t { Ok, Missing };
[[nodiscard]] ReadResult readArchive(const std::filesystem::path& path,
                                     std::vector<std::byte>& bytes, FileStamp& stamp,
                                     std::uint64_t& size);

// Opens the loose root that holds `config.ocio` and appends each stamped candidate to `bytes`
// through `append(key, expectedSize, stamp)`, which a platform backend provides as a reader.
class LooseRoot {
  public:
    virtual ~LooseRoot() = default;
    [[nodiscard]] virtual bool valid() const = 0;
    // Appends the file's exact bytes; false when it vanished, changed kind or size.
    [[nodiscard]] virtual bool read(const std::string& key, std::uint64_t size,
                                    std::vector<std::byte>& bytes, FileStamp& stamp) = 0;
};
[[nodiscard]] std::unique_ptr<LooseRoot> openLooseRoot(const std::filesystem::path& configPath);

#if defined(__linux__) || defined(__APPLE__)

#ifdef __linux__
[[nodiscard]] FileStamp stamp(std::string key, const struct stat& info) {
    return {std::move(key),
            static_cast<std::uint64_t>(info.st_size),
            static_cast<std::int64_t>(info.st_mtim.tv_sec),
            static_cast<std::int64_t>(info.st_mtim.tv_nsec),
            static_cast<std::uint64_t>(info.st_ino),
            static_cast<std::uint64_t>(info.st_dev)};
}
#else
[[nodiscard]] FileStamp stamp(std::string key, const struct stat& info) {
    return {std::move(key),
            static_cast<std::uint64_t>(info.st_size),
            static_cast<std::int64_t>(info.st_mtimespec.tv_sec),
            static_cast<std::int64_t>(info.st_mtimespec.tv_nsec),
            static_cast<std::uint64_t>(info.st_ino),
            static_cast<std::uint64_t>(info.st_dev)};
}
#endif

std::filesystem::path localPath(const std::string_view value) {
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
        return decoded ? std::filesystem::path(*decoded) : std::filesystem::path{};
    }
    if (!value.empty() && value.front() == '/' && value.find('\0') == std::string_view::npos)
        return std::filesystem::path(std::string(value));
    return {};
}

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
#ifdef __linux__
    open_how how{};
    how.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    return static_cast<int>(::syscall(SYS_openat2, root, key.c_str(), &how, sizeof(how)));
#else
    // No openat2 here: walk the canonical key (no empty, "." or ".." segment) one component at a
    // time, each opened relative to the last without following a link.
    int current = ::dup(root);
    std::size_t start = 0;
    while (current >= 0) {
        const auto end = key.find('/', start);
        const bool last = end == std::string::npos;
        const auto part = key.substr(start, last ? std::string::npos : end - start);
        const int next = ::openat(current, part.c_str(),
                                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (last ? 0 : O_DIRECTORY));
        ::close(current);
        if (next < 0 || last)
            return next;
        current = next;
        start = end + 1;
    }
    return -1;
#endif
}

// Appends exactly `size` bytes of `source` to `bytes`.
[[nodiscard]] bool copyBytes(const int source, std::vector<std::byte>& bytes, std::uint64_t size) {
    const auto at = bytes.size();
    bytes.resize(at + static_cast<std::size_t>(size));
    auto target = std::span(bytes).subspan(at);
    while (!target.empty()) {
        const auto count = ::read(source, target.data(), target.size());
        if (count <= 0)
            return false;
        target = target.subspan(static_cast<std::size_t>(count));
    }
    // The file must end where its size said it does.
    std::byte extra{};
    return ::read(source, &extra, 1) == 0;
}

Scan currentStamps(const OcioConfigLocation& location, const std::filesystem::path& file,
                   std::vector<FileStamp>& stamps) {
    stamps.clear();
    const auto& path = file.native();
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

ReadResult readArchive(const std::filesystem::path& path, std::vector<std::byte>& bytes,
                       FileStamp& result, std::uint64_t& size) {
    const detail::LutFd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat info{};
    if (file.get() < 0 || ::fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode))
        return ReadResult::Missing;
    size = static_cast<std::uint64_t>(info.st_size);
    result = stamp({}, info);
    if (size == 0 || size > detail::kConfigMaximumArchiveBytes)
        return ReadResult::Ok;
    return copyBytes(file.get(), bytes, size) ? ReadResult::Ok : ReadResult::Missing;
}

class PosixLooseRoot final : public LooseRoot {
  public:
    explicit PosixLooseRoot(const std::filesystem::path& configPath)
        : directory_(::open(rootOf(configPath).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {}
    [[nodiscard]] bool valid() const override { return directory_.get() >= 0; }
    [[nodiscard]] bool read(const std::string& key, const std::uint64_t size,
                            std::vector<std::byte>& bytes, FileStamp& result) override {
        const detail::LutFd source(openBeneath(directory_.get(), key));
        struct stat info{};
        // A file that vanished or changed kind since the scan: the root is changing under the
        // read, so nothing is published from it.
        if (source.get() < 0 || ::fstat(source.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
            static_cast<std::uint64_t>(info.st_size) != size)
            return false;
        result = stamp(key, info);
        return copyBytes(source.get(), bytes, size);
    }

  private:
    [[nodiscard]] static std::string rootOf(const std::filesystem::path& configPath) {
        const auto& path = configPath.native();
        return path.size() == kLooseName.size() + 1
                   ? std::string("/")
                   : path.substr(0, path.size() - kLooseName.size() - 1);
    }
    detail::LutFd directory_;
};

std::unique_ptr<LooseRoot> openLooseRoot(const std::filesystem::path& configPath) {
    return std::make_unique<PosixLooseRoot>(configPath);
}

#else // _WIN32

std::filesystem::path localPath(const std::string_view value) {
    if (value.find('\0') != std::string_view::npos)
        return {};
    std::filesystem::path path;
    if (value.substr(0, 5) == "file:") {
        const auto resolved = core::absolutePathFromFileUri(value);
        if (!resolved)
            return {};
        path = *resolved;
    } else {
        path = std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
    }
    // A drive-qualified path only: a relative path, a root-relative one and a network share
    // (UNC, or the extended `\\?\` form) are not local files of this machine.
    const auto text = path.generic_string();
    if (!path.is_absolute() || !path.has_root_name() || text.substr(0, 2) == "//" ||
        path.root_name().native().size() != 2 || path.root_name().native()[1] != L':')
        return {};
    return path;
}

struct Facts final {
    std::uint64_t size = 0;
    std::int64_t written = 0;
    std::uint64_t index = 0;
    std::uint64_t volume = 0;
    bool regular = false;
};

[[nodiscard]] FileStamp stampOf(std::string key, const Facts& facts) {
    return {std::move(key), facts.size, facts.written, 0, facts.index, facts.volume};
}

class Handle final {
  public:
    explicit Handle(const HANDLE value) : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE get() const { return value_; }
    [[nodiscard]] bool valid() const { return value_ != INVALID_HANDLE_VALUE; }

  private:
    HANDLE value_;
};

// Opens a file for reading without following a link in its last name. A name-surrogate reparse
// point (a symbolic link, a junction) is refused; any other (a cloud-synced file) is opened as the
// file it stands for, as the LUT reader does.
[[nodiscard]] Handle openNoLink(const std::filesystem::path& path, const DWORD access) {
    const auto open = [&](const DWORD flags) {
        return CreateFileW(path.c_str(), access,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, flags, nullptr);
    };
    HANDLE file = open(FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN);
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (file != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) &&
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(file);
        file = IsReparseTagNameSurrogate(tag.ReparseTag) ? INVALID_HANDLE_VALUE
                                                         : open(FILE_FLAG_SEQUENTIAL_SCAN);
    }
    return Handle(file);
}

[[nodiscard]] bool factsOf(const Handle& file, Facts& facts) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!file.valid() || !GetFileInformationByHandle(file.get(), &info))
        return false;
    facts.size = static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U | info.nFileSizeLow;
    facts.written = static_cast<std::int64_t>(
        static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32U |
        info.ftLastWriteTime.dwLowDateTime);
    facts.index = static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U | info.nFileIndexLow;
    facts.volume = info.dwVolumeSerialNumber;
    facts.regular = GetFileType(file.get()) == FILE_TYPE_DISK &&
                    (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    return true;
}

// Appends exactly `size` bytes of `file` to `bytes`.
[[nodiscard]] bool readBytes(const Handle& file, std::vector<std::byte>& bytes,
                             const std::uint64_t size) {
    const auto at = bytes.size();
    bytes.resize(at + static_cast<std::size_t>(size));
    auto target = std::span(bytes).subspan(at);
    while (!target.empty()) {
        DWORD count = 0;
        if (!ReadFile(file.get(), target.data(),
                      static_cast<DWORD>(std::min<std::size_t>(1U << 20U, target.size())), &count,
                      nullptr) ||
            count == 0)
            return false;
        target = target.subspan(count);
    }
    // The file must end where its size said it does.
    std::byte extra{};
    DWORD more = 0;
    return ReadFile(file.get(), &extra, 1, &more, nullptr) && more == 0;
}

Scan currentStamps(const OcioConfigLocation& location, const std::filesystem::path& path,
                   std::vector<FileStamp>& stamps) {
    stamps.clear();
    Facts facts;
    const auto config = openNoLink(path, FILE_READ_ATTRIBUTES);
    if (!factsOf(config, facts) || !facts.regular)
        return Scan::Missing;
    if (location.kind != OcioConfigLocatorKind::ExternalLooseConfig) {
        stamps.push_back(stampOf({}, facts));
        return Scan::Ok;
    }
    const auto root = path.parent_path();
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::none,
                                                     error);
    if (error)
        return Scan::Missing;
    for (; it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (error)
            return Scan::Missing;
        const auto status = it->symlink_status(error);
        if (error) {
            error.clear();
            continue;
        }
        if (status.type() == std::filesystem::file_type::directory) {
            // Directories are entered unless too deep; a link or junction is a different type and
            // is never entered.
            if (it.depth() >= 15)
                it.disable_recursion_pending();
            continue;
        }
        it.disable_recursion_pending();
        if (status.type() != std::filesystem::file_type::regular)
            continue;
        const auto key = it->path().lexically_relative(root).generic_string();
        Facts one;
        const auto file = openNoLink(it->path(), FILE_READ_ATTRIBUTES);
        if (!factsOf(file, one) || !one.regular || key.size() > kOcioLooseMaximumKeyBytes ||
            detail::canonicalConfigResourceKey(key) != key ||
            one.size > kOcioLooseMaximumResourceBytes)
            continue;
        if (stamps.size() >= kOcioLooseMaximumEntryCount)
            return Scan::TooLarge;
        stamps.push_back(stampOf(key, one));
    }
    std::ranges::sort(stamps, {}, &FileStamp::key);
    return Scan::Ok;
}

ReadResult readArchive(const std::filesystem::path& path, std::vector<std::byte>& bytes,
                       FileStamp& result, std::uint64_t& size) {
    const auto file = openNoLink(path, GENERIC_READ);
    Facts facts;
    if (!factsOf(file, facts) || !facts.regular)
        return ReadResult::Missing;
    size = facts.size;
    result = stampOf({}, facts);
    if (size == 0 || size > detail::kConfigMaximumArchiveBytes)
        return ReadResult::Ok;
    return readBytes(file, bytes, size) ? ReadResult::Ok : ReadResult::Missing;
}

class WindowsLooseRoot final : public LooseRoot {
  public:
    explicit WindowsLooseRoot(const std::filesystem::path& configPath)
        : root_(configPath.parent_path()) {}
    [[nodiscard]] bool valid() const override { return !root_.empty(); }
    [[nodiscard]] bool read(const std::string& key, const std::uint64_t size,
                            std::vector<std::byte>& bytes, FileStamp& result) override {
        const auto file = openNoLink(root_ / std::filesystem::path(key), GENERIC_READ);
        Facts facts;
        if (!factsOf(file, facts) || !facts.regular || facts.size != size)
            return false;
        result = stampOf(key, facts);
        return readBytes(file, bytes, size);
    }

  private:
    std::filesystem::path root_;
};

std::unique_ptr<LooseRoot> openLooseRoot(const std::filesystem::path& configPath) {
    return std::make_unique<WindowsLooseRoot>(configPath);
}

#endif

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
    const auto text = path.generic_string();
    const bool loose = location.kind == OcioConfigLocatorKind::ExternalLooseConfig;
    if (path.empty() || !endsWith(text, loose ? kLooseName : kArchiveSuffix) ||
        (loose && text.size() > kLooseName.size() && text[text.size() - kLooseName.size() - 1] != '/'))
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

    // Read the files into one buffer: the archive itself, or the loose resource table. The same
    // bytes are hashed here and parsed in the helper.
    std::vector<std::byte> bytes;
    std::vector<detail::ConfigResourceSlice> index;
    if (!loose) {
        std::uint64_t size = 0;
        FileStamp archiveStamp;
        if (readArchive(path, bytes, archiveStamp, size) != ReadResult::Ok)
            return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::Missing);
        if (size == 0 || size > detail::kConfigMaximumArchiveBytes)
            return invalid(size == 0 ? OcioBuiltInInvalidReason::ParseFailed
                                     : OcioBuiltInInvalidReason::ResourceTableLimitExceeded);
        stamps = {archiveStamp};
    } else {
        const auto root = openLooseRoot(path);
        if (!root || !root->valid())
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
        bytes = std::move(header).take();
        bytes.reserve(static_cast<std::size_t>(aggregate) + bytes.size() + stamps.size() * 96);
        for (auto& file : stamps) {
            if (cancelled(cancellation))
                return helperFailed(OcioHelperError::HelperCancelled);
            detail::ConfigWriter entry;
            entry.text(file.key);
            entry.u64(file.size);
            const auto head = std::move(entry).take();
            bytes.insert(bytes.end(), head.begin(), head.end());
            const auto offset = bytes.size();
            FileStamp read;
            // A file that vanished or changed since the scan: the root is changing under the
            // read, so nothing is published from it.
            if (!root->read(file.key, file.size, bytes, read))
                return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::Missing);
            file = read;
            index.push_back({file.key, offset, file.size});
        }
        if (std::ranges::none_of(index, [](const auto& slice) { return slice.key == kLooseName; }))
            return detail::OcioConfigAccess::outcome(OcioBuiltInRegistryOutcome::Missing);
    }

    // An archive's revision covers the whole file, so a changed archive is known before any
    // helper starts.
    std::optional<core::Sha256Digest> revision;
    if (!loose) {
        if (!detail::looksLikeConfigArchive(
                std::span<const std::byte>(bytes).first(std::min<std::size_t>(bytes.size(), 4))))
            return invalid(OcioBuiltInInvalidReason::ParseFailed);
        const auto computed = computeOcioContentRevisionV1(
            location.kind == OcioConfigLocatorKind::ProjectRelativeArchive
                ? OcioContentLocatorKind::ProjectRelativeArchive
                : OcioContentLocatorKind::ExternalArchive,
            bytes);
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
    auto opened = detail::ConfigSession::open(parameters, bytes, cancellation);
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
                    {found->key, std::span<const std::byte>(bytes).subspan(
                                     static_cast<std::size_t>(found->offset),
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
#ifdef BLOOM_OCIO_HOST_RESOLVES_FILES
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

OcioBuiltInResolutionResult resolveOcioConfigIfKept(const std::string_view locationText,
                                                    const core::Sha256Digest& expectedRevision,
                                                    const std::string_view workingColorSpaceId) noexcept {
    try {
        const auto location = parseOcioConfigLocation(locationText);
        if (location.kind == OcioConfigLocatorKind::BloomBuiltIn)
            return resolveOcioBuiltIn(location.kind, location.value, expectedRevision,
                                      workingColorSpaceId);
#ifdef BLOOM_OCIO_HOST_RESOLVES_FILES
        auto& cache = kept();
        const Key key{location, std::string(workingColorSpaceId)};
        const std::lock_guard lock(cache.mutex);
        for (const auto& entry : cache.entries)
            if (entry.key == key && entry.session->failure() == OcioHelperError::None)
                return answer(entry, expectedRevision);
#endif
        return helperFailed(OcioHelperError::HelperPending);
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
#ifdef BLOOM_OCIO_HOST_RESOLVES_FILES
    auto& cache = kept();
    const std::lock_guard lock(cache.mutex);
    cache.entries.clear();
#endif
}

} // namespace bloom::color
