#pragma once

#include <filesystem>

// What kind of volume holds a path (#535). Bloom reads ahead of the playhead on network and remote
// volumes, where overlapping reads hide latency, and not on local storage, where the extra reads
// compete with the frames' own. Blocking filesystem query: call it off the UI thread.
namespace bloom::platform {

enum class VolumeKind : unsigned char {
    // The volume could not be classified. Callers treat it like Remote: the behavior that was
    // measured to help on shares.
    Unknown,
    // A volume attached to this computer: internal and external disks, memory file systems.
    Local,
    // A network or remote file system: SMB/CIFS, NFS, 9p, AFS, Ceph and the like.
    Remote,
};

// Classifies `path` (an existing file or folder; for a missing one, the nearest existing ancestor).
// Linux reads the file system type, macOS the mount's local flag, Windows the drive type or a UNC
// root. A result is remembered per folder for the life of the process, so calling it per frame is
// cheap.
[[nodiscard]] VolumeKind volumeKind(const std::filesystem::path& path);

// The classification of a file system type name or magic is platform specific; this is the pure
// rule behind the Linux reading, exposed for tests: the file system magic number from statfs.
[[nodiscard]] VolumeKind volumeKindForLinuxFilesystemMagic(unsigned long magic) noexcept;

} // namespace bloom::platform
