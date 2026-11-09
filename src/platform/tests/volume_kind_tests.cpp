// bloom.platform.volume-kind (#535): the volume classification sequence read-ahead follows.
// The pure Linux rule is checked for every network file system and a spread of local ones; the
// real classification is checked on a folder this test creates, which sits on local storage (or,
// where the temporary folder is a FUSE mount, on a volume that reports Unknown).
#include <bloom/platform/volume_kind.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}
} // namespace

int main() {
    using bloom::platform::VolumeKind;
    using bloom::platform::volumeKind;
    using bloom::platform::volumeKindForLinuxFilesystemMagic;

    for (const unsigned long network :
         {0x6969UL, 0x517BUL, 0xFF534D42UL, 0xFE534D42UL, 0x01021997UL, 0x5346414FUL, 0x00C36400UL})
        expect(volumeKindForLinuxFilesystemMagic(network) == VolumeKind::Remote,
               "a network file system is Remote");
    // ext4, btrfs, xfs, tmpfs, overlayfs, zfs, f2fs, exfat/ntfs (fuseblk is separate).
    for (const unsigned long local : {0xEF53UL, 0x9123683EUL, 0x58465342UL, 0x01021994UL,
                                      0x794C7630UL, 0x2FC12FC1UL, 0xF2F52010UL})
        expect(volumeKindForLinuxFilesystemMagic(local) == VolumeKind::Local,
               "a local file system is Local");
    expect(volumeKindForLinuxFilesystemMagic(0x65735546UL) == VolumeKind::Unknown,
           "a user-space file system cannot be told local from remote");

    const auto folder =
        std::filesystem::temp_directory_path() /
        ("bloom-volume-kind-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    {
        std::ofstream(folder / "member.exr") << "x";
    }
    const auto folderKind = volumeKind(folder);
    expect(folderKind != VolumeKind::Remote, "the temporary folder is not on a network volume");
    expect(volumeKind(folder / "member.exr") == folderKind, "a file takes its folder's kind");
    expect(volumeKind(folder / "missing" / "deeper" / "file.exr") == folderKind,
           "a missing path takes its nearest existing ancestor's kind");
    expect(volumeKind(folder) == folderKind, "the answer is stable when asked again");
    expect(volumeKind(std::filesystem::path{}) == VolumeKind::Unknown, "an empty path is Unknown");
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);

    if (failures == 0)
        std::cout << "PASS: volume kinds\n";
    return failures == 0 ? 0 : 1;
}
