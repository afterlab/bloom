#include <bloom/platform/storage_class.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <mutex>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winioctl.h>
#else
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <fstream>
#include <sstream>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#endif
#endif

namespace bloom::platform {
namespace {

std::filesystem::path nearestExisting(std::filesystem::path path) {
    std::error_code error;
    if (path.empty())
        return path;
    if (std::filesystem::is_regular_file(path, error))
        path = path.parent_path();
    while (!path.empty() && !std::filesystem::exists(path, error)) {
        const auto parent = path.parent_path();
        if (parent == path)
            break;
        path = parent;
    }
    return path;
}

bool equalsIgnoringCase(const std::string_view left, const std::string_view right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](const char a, const char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}
bool containsIgnoringCase(const std::string_view text, const std::string_view part) noexcept {
    if (part.empty() || part.size() > text.size())
        return part.empty();
    for (std::size_t offset = 0; offset + part.size() <= text.size(); ++offset)
        if (equalsIgnoringCase(text.substr(offset, part.size()), part))
            return true;
    return false;
}
template <std::size_t Count>
bool oneOf(const std::string_view value,
           const std::array<std::string_view, Count>& names) noexcept {
    return std::ranges::any_of(
        names, [value](const std::string_view name) { return equalsIgnoringCase(value, name); });
}

// File systems that are a network protocol whatever carries them.
constexpr std::array<std::string_view, 17> kNetworkFilesystems{
    "nfs",   "nfs4",   "cifs",  "smb3",      "smbfs",  "smb2",   "9p",    "afs", "ceph",
    "afpfs", "webdav", "davfs", "glusterfs", "lustre", "beegfs", "ncpfs", "coda"};
// User-space mounts of remote storage, by the name after "fuse." (Linux) or the mount's source
// prefix (macOS).
constexpr std::array<std::string_view, 22> kNetworkUserSpaceMounts{
    "sshfs",     "rclone",
    "s3fs",      "gvfsd-fuse",
    "davfs",     "davfs2",
    "curlftpfs", "smbnetfs",
    "goofys",    "gcsfuse",
    "blobfuse",  "blobfuse2",
    "juicefs",   "glusterfs",
    "ceph",      "ceph-fuse",
    "cifs",      "nfs",
    "seaweedfs", "onedriver",
    "kio-fuse",  "google-drive-ocamlfuse"};
constexpr std::array<std::string_view, 4> kMemoryFilesystems{"tmpfs", "ramfs", "devtmpfs",
                                                             "hugetlbfs"};

// A source written as a remote location: "host:/path", "//host/share" or "user@host:".
bool remoteSourceName(const std::string_view source) noexcept {
    if (source.starts_with("//") || source.starts_with("\\\\"))
        return true;
    const auto colon = source.find(':');
    return colon != std::string_view::npos && colon > 0 && !source.starts_with("/");
}

struct VolumeMemo final {
    std::mutex mutex;
    std::map<std::uint64_t, StorageClass> classes;
};
VolumeMemo& volumeMemo() {
    static VolumeMemo value;
    return value;
}

// The id FileIdentity::volume reports for a file in `folder`.
std::optional<std::uint64_t> observeVolume(const std::filesystem::path& folder) {
    if (folder.empty())
        return std::nullopt;
#if defined(_WIN32)
    const HANDLE handle =
        ::CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return std::nullopt;
    struct Closer final {
        HANDLE value;
        ~Closer() { ::CloseHandle(value); }
    } closer{handle};
    FILE_ID_INFO id{};
    if (::GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)))
        return static_cast<std::uint64_t>(id.VolumeSerialNumber);
    BY_HANDLE_FILE_INFORMATION legacy{};
    if (!::GetFileInformationByHandle(handle, &legacy))
        return std::nullopt;
    return static_cast<std::uint64_t>(legacy.dwVolumeSerialNumber);
#else
    struct stat status{};
    if (::stat(folder.c_str(), &status) != 0)
        return std::nullopt;
    return static_cast<std::uint64_t>(status.st_dev);
#endif
}

#if defined(__linux__)
struct MountEntry final {
    std::string mountPoint, filesystem, source;
};
// The mount table writes a space, tab, newline and backslash as a three-digit octal escape.
std::string unescapeMountField(const std::string_view field) {
    std::string result;
    result.reserve(field.size());
    for (std::size_t index = 0; index < field.size(); ++index) {
        if (field[index] == '\\' && field.size() - index >= 4) {
            const auto digit = [&](const std::size_t at) { return field[index + at] - '0'; };
            if (digit(1) >= 0 && digit(1) <= 7 && digit(2) >= 0 && digit(2) <= 7 && digit(3) >= 0 &&
                digit(3) <= 7) {
                result.push_back(static_cast<char>(digit(1) * 64 + digit(2) * 8 + digit(3)));
                index += 3;
                continue;
            }
        }
        result.push_back(field[index]);
    }
    return result;
}
bool mountHolds(const std::string_view mountPoint, const std::string_view folder) noexcept {
    if (mountPoint == "/")
        return true;
    return folder.starts_with(mountPoint) &&
           (folder.size() == mountPoint.size() || folder[mountPoint.size()] == '/');
}
// The mount that holds `folder`: the longest mount point that is a prefix of it, and among
// mounts stacked on one point the last (the one on top).
std::optional<MountEntry> findMount(const std::filesystem::path& folder) {
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(folder, error);
    if (error)
        canonical = folder;
    const auto text = canonical.generic_string();
    std::ifstream table("/proc/self/mountinfo");
    std::optional<MountEntry> best;
    for (std::string line; std::getline(table, line);) {
        // id parent major:minor root mount-point options [optional...] - type source super
        const auto separator = line.find(" - ");
        if (separator == std::string::npos)
            continue;
        std::istringstream head(line.substr(0, separator));
        std::string id, parent, device, root, mountPoint;
        if (!(head >> id >> parent >> device >> root >> mountPoint))
            continue;
        std::istringstream tail(line.substr(separator + 3));
        std::string filesystem, source;
        if (!(tail >> filesystem >> source))
            continue;
        auto point = unescapeMountField(mountPoint);
        if (!mountHolds(point, text))
            continue;
        if (!best || point.size() >= best->mountPoint.size())
            best = MountEntry{std::move(point), std::move(filesystem), unescapeMountField(source)};
    }
    return best;
}
std::optional<bool> readFlag(const std::filesystem::path& file) {
    std::ifstream input(file);
    char value = 0;
    if (!(input >> value) || (value != '0' && value != '1'))
        return std::nullopt;
    return value == '1';
}
struct BlockFacts final {
    std::string device;
    std::optional<bool> rotational, removable;
};
// What sysfs says of the block device `number` and of the devices under it (a device-mapper or
// software-RAID volume lists them in `slaves`).
void collectBlockFacts(const dev_t number, BlockFacts& facts, const int depth) {
    std::error_code error;
    const auto link = std::filesystem::path("/sys/dev/block") /
                      (std::to_string(major(number)) + ":" + std::to_string(minor(number)));
    auto disk = std::filesystem::canonical(link, error);
    if (error)
        return;
    // A partition's directory sits inside its disk's, which holds the queue.
    if (!std::filesystem::exists(disk / "queue", error))
        disk = disk.parent_path();
    facts.device += disk.generic_string();
    facts.device += ' ';
    if (const auto rotational = readFlag(disk / "queue" / "rotational"))
        facts.rotational = facts.rotational.value_or(false) || *rotational;
    if (const auto removable = readFlag(disk / "removable"))
        facts.removable = facts.removable.value_or(false) || *removable;
    if (depth >= 4)
        return;
    for (std::filesystem::directory_iterator slaves(disk / "slaves", error), end;
         !error && slaves != end; slaves.increment(error)) {
        struct stat status{};
        const auto node = std::filesystem::path("/dev") / slaves->path().filename();
        if (::stat(node.c_str(), &status) == 0 && S_ISBLK(status.st_mode))
            collectBlockFacts(status.st_rdev, facts, depth + 1);
    }
}
StorageClass classify(const std::filesystem::path& folder) {
    struct statfs fs{};
    if (::statfs(folder.c_str(), &fs) != 0)
        return StorageClass::Unknown;
    const auto mount = findMount(folder);
    BlockFacts block;
    struct stat status{};
    if (mount && mount->source.starts_with("/dev/") &&
        ::stat(mount->source.c_str(), &status) == 0 && S_ISBLK(status.st_mode))
        collectBlockFacts(status.st_rdev, block, 0);
    else if (::stat(folder.c_str(), &status) == 0 && major(status.st_dev) != 0)
        collectBlockFacts(status.st_dev, block, 0);
    return storageClassForLinuxVolume({.magic = static_cast<unsigned long>(fs.f_type),
                                       .filesystem = mount ? mount->filesystem : std::string{},
                                       .source = mount ? mount->source : std::string{},
                                       .device = block.device,
                                       .rotational = block.rotational,
                                       .removable = block.removable});
}
#elif defined(__APPLE__)
std::string stringOf(const CFTypeRef value) {
    if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID())
        return {};
    std::array<char, 256> text{};
    if (!CFStringGetCString(static_cast<CFStringRef>(value), text.data(),
                            static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8))
        return {};
    return text.data();
}
bool booleanOf(const CFTypeRef value) {
    return value != nullptr && CFGetTypeID(value) == CFBooleanGetTypeID() &&
           CFBooleanGetValue(static_cast<CFBooleanRef>(value));
}
StorageClass classify(const std::filesystem::path& folder) {
    struct statfs fs{};
    if (::statfs(folder.c_str(), &fs) != 0)
        return StorageClass::Unknown;
    std::string protocol, mediaKind;
    std::optional<bool> solidState;
    bool removable = false, internal = false;
    if (const DASessionRef session = DASessionCreate(kCFAllocatorDefault); session != nullptr) {
        if (const DADiskRef disk =
                DADiskCreateFromBSDName(kCFAllocatorDefault, session, fs.f_mntfromname);
            disk != nullptr) {
            if (const CFDictionaryRef description = DADiskCopyDescription(disk);
                description != nullptr) {
                protocol = stringOf(
                    CFDictionaryGetValue(description, kDADiskDescriptionDeviceProtocolKey));
                mediaKind =
                    stringOf(CFDictionaryGetValue(description, kDADiskDescriptionMediaKindKey));
                removable = booleanOf(
                    CFDictionaryGetValue(description, kDADiskDescriptionMediaRemovableKey));
                internal = booleanOf(
                    CFDictionaryGetValue(description, kDADiskDescriptionDeviceInternalKey));
                CFRelease(description);
            }
            if (const io_service_t media = DADiskCopyIOMedia(disk); media != IO_OBJECT_NULL) {
                if (const CFTypeRef characteristics = IORegistryEntrySearchCFProperty(
                        media, kIOServicePlane, CFSTR(kIOPropertyDeviceCharacteristicsKey),
                        kCFAllocatorDefault,
                        kIORegistryIterateRecursively | kIORegistryIterateParents);
                    characteristics != nullptr) {
                    if (CFGetTypeID(characteristics) == CFDictionaryGetTypeID()) {
                        const auto medium = stringOf(
                            CFDictionaryGetValue(static_cast<CFDictionaryRef>(characteristics),
                                                 CFSTR(kIOPropertyMediumTypeKey)));
                        if (medium == kIOPropertyMediumTypeSolidStateKey)
                            solidState = true;
                        else if (medium == kIOPropertyMediumTypeRotationalKey)
                            solidState = false;
                    }
                    CFRelease(characteristics);
                }
                IOObjectRelease(media);
            }
            CFRelease(disk);
        }
        CFRelease(session);
    }
    return storageClassForMacVolume({.localMount = (fs.f_flags & MNT_LOCAL) != 0,
                                     .filesystem = fs.f_fstypename,
                                     .source = fs.f_mntfromname,
                                     .protocol = protocol,
                                     .mediaKind = mediaKind,
                                     .solidState = solidState,
                                     .removableMedia = removable,
                                     .internal = internal});
}
#elif defined(_WIN32)
// A UNC path (\\server\share, \\?\UNC\server\share): a share whatever its root says.
[[nodiscard]] bool uncPath(const std::filesystem::path& path) {
    const auto& text = path.native();
    return text.rfind(L"\\\\?\\UNC\\", 0) == 0 ||
           (text.rfind(L"\\\\", 0) == 0 && text.rfind(L"\\\\?\\", 0) != 0);
}

StorageClass classify(const std::filesystem::path& folder) {
    WindowsVolumeFacts facts;
    if (uncPath(folder)) {
        facts.uncPath = true;
        return storageClassForWindowsVolume(facts);
    }
    std::error_code error;
    const auto absolute = std::filesystem::absolute(folder, error);
    if (error || !absolute.has_root_path())
        return StorageClass::Unknown;
    // The volume that holds the folder: a drive root, or a folder another volume is mounted on.
    std::array<wchar_t, MAX_PATH + 1> mountPoint{};
    if (!::GetVolumePathNameW(absolute.c_str(), mountPoint.data(),
                              static_cast<DWORD>(mountPoint.size())))
        return StorageClass::Unknown;
    switch (::GetDriveTypeW(mountPoint.data())) {
    case DRIVE_REMOTE:
        facts.drive = WindowsDriveType::Remote;
        break;
    case DRIVE_REMOVABLE:
        facts.drive = WindowsDriveType::Removable;
        break;
    case DRIVE_CDROM:
        facts.drive = WindowsDriveType::Optical;
        break;
    case DRIVE_RAMDISK:
        facts.drive = WindowsDriveType::RamDisk;
        break;
    case DRIVE_FIXED:
        facts.drive = WindowsDriveType::Fixed;
        break;
    default:
        break;
    }
    if (facts.drive != WindowsDriveType::Fixed)
        return storageClassForWindowsVolume(facts);
    std::array<wchar_t, 64> volumeName{};
    if (!::GetVolumeNameForVolumeMountPointW(mountPoint.data(), volumeName.data(),
                                             static_cast<DWORD>(volumeName.size())))
        return storageClassForWindowsVolume(facts);
    std::wstring device(volumeName.data());
    if (!device.empty() && device.back() == L'\\')
        device.pop_back(); // the volume device itself, not its root folder
    const HANDLE handle =
        ::CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return storageClassForWindowsVolume(facts);
    struct Closer final {
        HANDLE value;
        ~Closer() { ::CloseHandle(value); }
    } closer{handle};
    DWORD returned = 0;
    {
        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        alignas(STORAGE_DEVICE_DESCRIPTOR) std::array<std::byte, 1024> buffer{};
        if (::DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                              buffer.data(), static_cast<DWORD>(buffer.size()), &returned,
                              nullptr) &&
            returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            const auto* descriptor =
                reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
            facts.removableMedia = descriptor->RemovableMedia != FALSE;
            switch (descriptor->BusType) {
            case BusTypeUsb:
                facts.bus = "usb";
                break;
            case BusTypeSd:
                facts.bus = "sd";
                break;
            case BusTypeMmc:
                facts.bus = "mmc";
                break;
            case BusType1394:
                facts.bus = "1394";
                break;
            case BusTypeNvme:
                facts.bus = "nvme";
                break;
            default:
                facts.bus = "other";
                break;
            }
        }
    }
    {
        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceSeekPenaltyProperty;
        query.QueryType = PropertyStandardQuery;
        DEVICE_SEEK_PENALTY_DESCRIPTOR penalty{};
        if (::DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &penalty,
                              sizeof(penalty), &returned, nullptr) &&
            returned >= sizeof(penalty))
            facts.seekPenalty = penalty.IncursSeekPenalty != FALSE;
    }
    return storageClassForWindowsVolume(facts);
}
#else
StorageClass classify(const std::filesystem::path&) { return StorageClass::Unknown; }
#endif

} // namespace

std::string_view storageClassName(const StorageClass value) noexcept {
    switch (value) {
    case StorageClass::FastLocal:
        return "Fast local";
    case StorageClass::SlowAttached:
        return "Slow attached";
    case StorageClass::Network:
        return "Network";
    case StorageClass::Unknown:
        break;
    }
    return "Unknown";
}

StorageClass storageClassForLinuxVolume(const LinuxVolumeFacts& facts) noexcept {
    switch (facts.magic) {
    case 0x6969UL:     // NFS
    case 0x517BUL:     // SMB
    case 0xFF534D42UL: // CIFS
    case 0xFE534D42UL: // SMB2/3
    case 0x01021997UL: // 9p
    case 0x5346414FUL: // AFS
    case 0x00C36400UL: // Ceph
    case 0x47504653UL: // GPFS
    case 0x73757245UL: // Coda
    case 0x7461636FUL: // OCFS2
        return StorageClass::Network;
    default:
        break;
    }
    if (oneOf(facts.filesystem, kNetworkFilesystems))
        return StorageClass::Network;
    // A user-space file system is remote storage when its type or source says so, a block device
    // when it is "fuseblk" (NTFS, exFAT), and otherwise cannot be told.
    const bool userSpace = facts.magic == 0x65735546UL || facts.filesystem.starts_with("fuse");
    if (userSpace && !equalsIgnoringCase(facts.filesystem, "fuseblk")) {
        const auto dot = facts.filesystem.find('.');
        const auto subtype =
            dot == std::string_view::npos ? std::string_view{} : facts.filesystem.substr(dot + 1);
        if (oneOf(subtype, kNetworkUserSpaceMounts) || remoteSourceName(facts.source))
            return StorageClass::Network;
        return StorageClass::Unknown;
    }
    if (oneOf(facts.filesystem, kMemoryFilesystems) || facts.magic == 0x01021994UL ||
        facts.magic == 0x858458F6UL)
        return StorageClass::FastLocal;
    if (facts.device.empty() && !facts.rotational && !facts.removable)
        return StorageClass::Unknown; // no block device to ask (a pool, an overlay)
    if (containsIgnoringCase(facts.device, "/usb") || containsIgnoringCase(facts.device, "/mmc") ||
        containsIgnoringCase(facts.device, "/block/sr") || facts.removable.value_or(false) ||
        facts.rotational.value_or(false))
        return StorageClass::SlowAttached;
    return facts.rotational ? StorageClass::FastLocal : StorageClass::Unknown;
}

StorageClass storageClassForWindowsVolume(const WindowsVolumeFacts& facts) noexcept {
    if (facts.uncPath)
        return StorageClass::Network;
    switch (facts.drive) {
    case WindowsDriveType::Remote:
        return StorageClass::Network;
    case WindowsDriveType::Removable:
    case WindowsDriveType::Optical:
        return StorageClass::SlowAttached;
    case WindowsDriveType::RamDisk:
        return StorageClass::FastLocal;
    case WindowsDriveType::Fixed:
        break;
    case WindowsDriveType::Unknown:
        return StorageClass::Unknown;
    }
    if (facts.removableMedia ||
        oneOf(facts.bus, std::array<std::string_view, 4>{"usb", "sd", "mmc", "1394"}))
        return StorageClass::SlowAttached;
    if (facts.seekPenalty)
        return *facts.seekPenalty ? StorageClass::SlowAttached : StorageClass::FastLocal;
    return equalsIgnoringCase(facts.bus, "nvme") ? StorageClass::FastLocal : StorageClass::Unknown;
}

StorageClass storageClassForMacVolume(const MacVolumeFacts& facts) noexcept {
    if (oneOf(facts.filesystem, kNetworkFilesystems) || equalsIgnoringCase(facts.filesystem, "ftp"))
        return StorageClass::Network;
    if (containsIgnoringCase(facts.filesystem, "fuse")) {
        const auto at = facts.source.find_first_of("@:");
        const auto name = facts.source.substr(0, at);
        if (oneOf(name, kNetworkUserSpaceMounts) || remoteSourceName(facts.source))
            return StorageClass::Network;
        return StorageClass::Unknown;
    }
    if (!facts.localMount)
        return StorageClass::Network;
    if (oneOf(facts.protocol,
              std::array<std::string_view, 4>{"USB", "Secure Digital", "FireWire", "ATAPI"}) ||
        facts.removableMedia || containsIgnoringCase(facts.mediaKind, "CDMedia") ||
        containsIgnoringCase(facts.mediaKind, "DVDMedia") ||
        containsIgnoringCase(facts.mediaKind, "BDMedia"))
        return StorageClass::SlowAttached;
    if (facts.solidState)
        return *facts.solidState ? StorageClass::FastLocal : StorageClass::SlowAttached;
    // A disk image or a volume whose medium is not reported: internal storage of a current
    // computer is solid state; anything else cannot be told.
    return facts.internal ? StorageClass::FastLocal : StorageClass::Unknown;
}

StorageClass storageClass(const std::filesystem::path& path, const std::uint64_t volume) {
#if defined(_WIN32)
    // A share reached by its UNC name is Network whatever volume id its files report. That id is
    // the server volume's serial number, which a disk of this computer can carry too (the
    // computer's own share, a cloned disk), so the answer is neither looked up nor remembered
    // under it: a share is never taken for the local disk, nor the disk for a share.
    if (uncPath(path)) {
        WindowsVolumeFacts facts;
        facts.uncPath = true;
        return storageClassForWindowsVolume(facts);
    }
#endif
    auto& memo = volumeMemo();
    {
        const std::lock_guard lock(memo.mutex);
        if (const auto found = memo.classes.find(volume); found != memo.classes.end())
            return found->second;
    }
    // Classified outside the lock: a stalled share must not hold up the other volumes.
    const auto value = classify(nearestExisting(path));
    const std::lock_guard lock(memo.mutex);
    if (memo.classes.size() >= 1024)
        memo.classes.clear();
    return memo.classes.emplace(volume, value).first->second;
}

StorageVolume storageVolume(const std::filesystem::path& path) {
    const auto folder = nearestExisting(path);
    const auto volume = observeVolume(folder);
    if (!volume)
        return {};
    return {storageClass(folder, *volume), volume};
}

StorageClass storageClass(const std::filesystem::path& path) {
    return storageVolume(path).storageClass;
}

} // namespace bloom::platform
