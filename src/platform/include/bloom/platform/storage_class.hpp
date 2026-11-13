#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

// What kind of storage holds a path (#535, #718). Bloom keeps local copies of source media that
// sit on slow storage and reads ahead of the playhead there; fast local media is read directly.
// The first look at a volume is a blocking filesystem query: call it off the UI thread.
namespace bloom::platform {

enum class StorageClass : unsigned char {
    // The volume could not be classified (a user-space file system of an unknown kind, a volume
    // whose device is hidden). Callers treat it as slow.
    Unknown,
    // Solid-state storage attached to this computer, and memory file systems.
    FastLocal,
    // Attached but slow: removable media, USB, SD and MMC cards, optical discs, rotational disks.
    SlowAttached,
    // A network or remote file system: SMB/CIFS, NFS, 9p, AFS, Ceph, and user-space mounts of
    // remote storage (SSHFS and the like).
    Network,
};

// "Network", "Slow attached", "Fast local" or "Unknown": the name shown to the user.
[[nodiscard]] std::string_view storageClassName(StorageClass value) noexcept;

// True for every class but FastLocal: the classes read-ahead serves and Automatic local copies
// cover.
[[nodiscard]] constexpr bool isSlowStorage(const StorageClass value) noexcept {
    return value != StorageClass::FastLocal;
}

struct StorageVolume final {
    StorageClass storageClass = StorageClass::Unknown;
    // The volume id FileIdentity::volume reports for files on it; nullopt when the path (and
    // every ancestor) is missing.
    std::optional<std::uint64_t> volume;
};

// Classifies the volume that actually holds `path` (an existing file or folder; for a missing
// one, the nearest existing ancestor): mount points inside a drive are followed, and a
// user-space mount of remote storage reads Network. The answer is remembered per volume id for
// the life of the process, so a volume already seen costs one identity query and no more.
[[nodiscard]] StorageVolume storageVolume(const std::filesystem::path& path);
[[nodiscard]] StorageClass storageClass(const std::filesystem::path& path);
// For a caller that already observed a file's identity: `volume` is its FileIdentity::volume.
// A volume already classified costs no filesystem call at all.
[[nodiscard]] StorageClass storageClass(const std::filesystem::path& path, std::uint64_t volume);

// The pure Linux rule, exposed for tests. `magic` is the file system magic from statfs,
// `filesystem` and `source` the type and source columns of the volume's mount table entry.
// `device` is the block device's path in sysfs (its bus shows there), empty when the volume has
// no block device; `rotational` and `removable` are that device's flags when they could be read.
struct LinuxVolumeFacts final {
    unsigned long magic = 0;
    std::string_view filesystem;
    std::string_view source;
    std::string_view device;
    std::optional<bool> rotational;
    std::optional<bool> removable;
};
[[nodiscard]] StorageClass storageClassForLinuxVolume(const LinuxVolumeFacts& facts) noexcept;

// The pure Windows rule, exposed for tests: the drive type and, for a fixed drive, what the
// storage device reports. `bus` uses the names "usb", "sd", "mmc", "1394", "nvme" or "other".
enum class WindowsDriveType : unsigned char { Unknown, Remote, Removable, Optical, RamDisk, Fixed };
struct WindowsVolumeFacts final {
    bool uncPath = false;
    WindowsDriveType drive = WindowsDriveType::Unknown;
    std::string_view bus;
    std::optional<bool> seekPenalty;
    bool removableMedia = false;
};
[[nodiscard]] StorageClass storageClassForWindowsVolume(const WindowsVolumeFacts& facts) noexcept;

// The pure macOS rule, exposed for tests: the mount's local flag and type name, and what the
// disk reports. `protocol` is the device protocol name ("USB", "Secure Digital", "ATAPI", ...).
struct MacVolumeFacts final {
    bool localMount = false;
    std::string_view filesystem;
    std::string_view source;
    std::string_view protocol;
    std::string_view mediaKind;
    std::optional<bool> solidState;
    bool removableMedia = false;
    bool internal = false;
};
[[nodiscard]] StorageClass storageClassForMacVolume(const MacVolumeFacts& facts) noexcept;

} // namespace bloom::platform
