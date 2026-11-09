#include <bloom/platform/volume_kind.hpp>

#include <map>
#include <mutex>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mount.h>
#include <sys/param.h>
#if defined(__linux__)
#include <sys/vfs.h>
#else
#include <sys/types.h>
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

VolumeKind classify(const std::filesystem::path& folder) {
    if (folder.empty())
        return VolumeKind::Unknown;
#if defined(_WIN32)
    // A UNC path (\\server\share, \\?\UNC\server\share) is a share whatever its root says.
    const auto text = folder.native();
    if (text.rfind(L"\\\\?\\UNC\\", 0) == 0)
        return VolumeKind::Remote;
    if (text.rfind(L"\\\\", 0) == 0 && text.rfind(L"\\\\?\\", 0) != 0)
        return VolumeKind::Remote;
    std::error_code error;
    const auto absolute = std::filesystem::absolute(folder, error);
    if (error || !absolute.has_root_path())
        return VolumeKind::Unknown;
    const auto root = absolute.root_path().native();
    switch (GetDriveTypeW(root.c_str())) {
    case DRIVE_REMOTE:
        return VolumeKind::Remote;
    case DRIVE_FIXED:
    case DRIVE_REMOVABLE:
    case DRIVE_CDROM:
    case DRIVE_RAMDISK:
        return VolumeKind::Local;
    default:
        return VolumeKind::Unknown;
    }
#elif defined(__linux__)
    struct statfs fs{};
    if (::statfs(folder.c_str(), &fs) != 0)
        return VolumeKind::Unknown;
    return volumeKindForLinuxFilesystemMagic(static_cast<unsigned long>(fs.f_type));
#elif defined(__APPLE__)
    struct statfs fs{};
    if (::statfs(folder.c_str(), &fs) != 0)
        return VolumeKind::Unknown;
    return (fs.f_flags & MNT_LOCAL) != 0 ? VolumeKind::Local : VolumeKind::Remote;
#else
    return VolumeKind::Unknown;
#endif
}

} // namespace

VolumeKind volumeKindForLinuxFilesystemMagic(const unsigned long magic) noexcept {
    switch (magic) {
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
        return VolumeKind::Remote;
    case 0x65735546UL: // FUSE: a user-space file system may be local or a remote mount.
        return VolumeKind::Unknown;
    default:
        return VolumeKind::Local;
    }
}

VolumeKind volumeKind(const std::filesystem::path& path) {
    static std::mutex mutex;
    static std::map<std::string, VolumeKind> remembered;
    const auto folder = nearestExisting(path);
    const auto key = folder.generic_string();
    {
        const std::lock_guard lock(mutex);
        if (const auto found = remembered.find(key); found != remembered.end())
            return found->second;
    }
    // Classified outside the lock: a stalled share must not hold up the other folders.
    const auto kind = classify(folder);
    const std::lock_guard lock(mutex);
    if (remembered.size() >= 256)
        remembered.clear();
    remembered.emplace(key, kind);
    return kind;
}

} // namespace bloom::platform
