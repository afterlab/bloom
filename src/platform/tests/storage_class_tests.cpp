// bloom.platform.storage-class (#535, #718): the storage classification that read-ahead and local
// copies of source media follow. The pure rules of all three platforms run everywhere; the real
// classification is checked on a folder this test creates, which is never on a network volume.
#include <bloom/platform/source_identity.hpp>
#include <bloom/platform/storage_class.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

using bloom::platform::StorageClass;
using Flag = std::optional<bool>;

struct Linux final {
    unsigned long magic = 0;
    std::string_view filesystem{}, source{}, device{};
    Flag rotational{}, removable{};
    [[nodiscard]] StorageClass classify() const {
        return bloom::platform::storageClassForLinuxVolume(
            {magic, filesystem, source, device, rotational, removable});
    }
};
struct Windows final {
    bool uncPath = false;
    bloom::platform::WindowsDriveType drive = bloom::platform::WindowsDriveType::Unknown;
    std::string_view bus{};
    Flag seekPenalty{};
    bool removableMedia = false;
    [[nodiscard]] StorageClass classify() const {
        return bloom::platform::storageClassForWindowsVolume(
            {uncPath, drive, bus, seekPenalty, removableMedia});
    }
};
struct Mac final {
    bool localMount = false;
    std::string_view filesystem{}, source{}, protocol{}, mediaKind{};
    Flag solidState{};
    bool removableMedia = false, internal = false;
    [[nodiscard]] StorageClass classify() const {
        return bloom::platform::storageClassForMacVolume({localMount, filesystem, source, protocol,
                                                          mediaKind, solidState, removableMedia,
                                                          internal});
    }
};

void testLinuxRule() {
    for (const unsigned long network :
         {0x6969UL, 0x517BUL, 0xFF534D42UL, 0xFE534D42UL, 0x01021997UL, 0x5346414FUL, 0x00C36400UL})
        expect(Linux{.magic = network}.classify() == StorageClass::Network,
               "a network file system is Network");
    expect(Linux{.magic = 0xEF53UL,
                 .filesystem = "ext4",
                 .source = "/dev/nvme0n1p2",
                 .device = "/sys/devices/pci0000:00/nvme/nvme0/nvme0n1 ",
                 .rotational = false,
                 .removable = false}
                   .classify() == StorageClass::FastLocal,
           "a solid-state disk is Fast local");
    expect(Linux{.magic = 0xEF53UL,
                 .filesystem = "ext4",
                 .source = "/dev/sda1",
                 .device = "/sys/devices/pci0000:00/ata1/host0/block/sda ",
                 .rotational = true,
                 .removable = false}
                   .classify() == StorageClass::SlowAttached,
           "a rotational disk is Slow attached");
    expect(Linux{.magic = 0x2011BAB0UL,
                 .filesystem = "exfat",
                 .source = "/dev/sdb1",
                 .device = "/sys/devices/pci0000:00/usb2/2-1/host6/block/sdb ",
                 .rotational = false,
                 .removable = true}
                   .classify() == StorageClass::SlowAttached,
           "a USB stick is Slow attached");
    expect(Linux{.magic = 0xEF53UL,
                 .filesystem = "ext4",
                 .source = "/dev/sdc1",
                 .device = "/sys/devices/pci0000:00/usb4/4-2/host7/block/sdc ",
                 .rotational = false,
                 .removable = false}
                   .classify() == StorageClass::SlowAttached,
           "a solid-state disk on USB is Slow attached");
    expect(Linux{.magic = 0x4D44UL,
                 .filesystem = "vfat",
                 .source = "/dev/mmcblk0p1",
                 .device = "/sys/devices/platform/mmc_host/mmc0/mmc0:0001/block/mmcblk0 ",
                 .rotational = false,
                 .removable = false}
                   .classify() == StorageClass::SlowAttached,
           "an SD or MMC card is Slow attached");
    expect(Linux{.magic = 0x9660UL,
                 .filesystem = "iso9660",
                 .source = "/dev/sr0",
                 .device = "/sys/devices/pci0000:00/ata2/host1/block/sr0 ",
                 .rotational = true,
                 .removable = true}
                   .classify() == StorageClass::SlowAttached,
           "an optical disc is Slow attached");
    // A device-mapper volume lists the disks under it: one rotational disk makes it slow.
    expect(
        Linux{.magic = 0x58465342UL,
              .filesystem = "xfs",
              .source = "/dev/mapper/pool-media",
              .device = "/sys/devices/virtual/block/dm-0 /sys/devices/pci0000:00/ata1/block/sda ",
              .rotational = true,
              .removable = false}
                .classify() == StorageClass::SlowAttached,
        "a mapped volume over a rotational disk is Slow attached");
    expect(Linux{.magic = 0x01021994UL, .filesystem = "tmpfs"}.classify() ==
               StorageClass::FastLocal,
           "a memory file system is Fast local");
    expect(Linux{.magic = 0x2FC12FC1UL, .filesystem = "zfs", .source = "tank/media"}.classify() ==
               StorageClass::Unknown,
           "a volume without a block device to ask is Unknown");
    expect(Linux{.magic = 0x65735546UL, .filesystem = "fuse.sshfs", .source = "render@farm:/shots"}
                   .classify() == StorageClass::Network,
           "an SSHFS mount is Network");
    expect(
        Linux{.magic = 0x65735546UL, .filesystem = "fuse.rclone", .source = "bucket:"}.classify() ==
            StorageClass::Network,
        "an rclone mount is Network");
    expect(Linux{.magic = 0x65735546UL, .filesystem = "fuse.custom", .source = "//server/share"}
                   .classify() == StorageClass::Network,
           "a user-space mount whose source is a share is Network");
    expect(Linux{.magic = 0x65735546UL, .filesystem = "fuse.encfs", .source = "encfs"}.classify() ==
               StorageClass::Unknown,
           "a user-space file system of an unknown kind is Unknown");
    expect(Linux{.magic = 0x65735546UL,
                 .filesystem = "fuseblk",
                 .source = "/dev/nvme1n1p3",
                 .device = "/sys/devices/pci0000:00/nvme/nvme1/nvme1n1 ",
                 .rotational = false,
                 .removable = false}
                   .classify() == StorageClass::FastLocal,
           "a block file system served from user space follows its disk");
    expect(Linux{.magic = 0x794C7630UL, .filesystem = "nfs4"}.classify() == StorageClass::Network,
           "a network file system is recognized by name as well");
}

void testWindowsRule() {
    using bloom::platform::WindowsDriveType;
    expect(Windows{.uncPath = true}.classify() == StorageClass::Network, "a UNC path is Network");
    expect(Windows{.drive = WindowsDriveType::Remote}.classify() == StorageClass::Network,
           "a mapped network drive is Network");
    expect(Windows{.drive = WindowsDriveType::Removable}.classify() == StorageClass::SlowAttached,
           "a removable drive is Slow attached");
    expect(Windows{.drive = WindowsDriveType::Optical}.classify() == StorageClass::SlowAttached,
           "an optical drive is Slow attached");
    expect(Windows{.drive = WindowsDriveType::RamDisk}.classify() == StorageClass::FastLocal,
           "a memory disk is Fast local");
    expect(
        Windows{.drive = WindowsDriveType::Fixed, .bus = "nvme", .seekPenalty = false}.classify() ==
            StorageClass::FastLocal,
        "a fixed disk without a seek penalty is Fast local");
    expect(
        Windows{.drive = WindowsDriveType::Fixed, .bus = "other", .seekPenalty = true}.classify() ==
            StorageClass::SlowAttached,
        "a fixed disk with a seek penalty is Slow attached");
    expect(
        Windows{.drive = WindowsDriveType::Fixed, .bus = "usb", .seekPenalty = false}.classify() ==
            StorageClass::SlowAttached,
        "a fixed disk on USB is Slow attached");
    expect(Windows{.drive = WindowsDriveType::Fixed, .bus = "sd"}.classify() ==
               StorageClass::SlowAttached,
           "an SD card is Slow attached");
    expect(Windows{.drive = WindowsDriveType::Fixed, .bus = "other"}.classify() ==
               StorageClass::Unknown,
           "a fixed disk that reports nothing is Unknown");
    expect(Windows{}.classify() == StorageClass::Unknown, "an unrecognized drive is Unknown");
}

void testMacRule() {
    expect(Mac{.localMount = false, .filesystem = "smbfs", .source = "//user@server/share"}
                   .classify() == StorageClass::Network,
           "an SMB mount is Network");
    expect(Mac{.localMount = false, .filesystem = "nfs"}.classify() == StorageClass::Network,
           "an NFS mount is Network");
    expect(
        Mac{.localMount = false, .filesystem = "macfuse", .source = "sshfs@macfuse0"}.classify() ==
            StorageClass::Network,
        "an SSHFS mount is Network");
    expect(
        Mac{.localMount = false, .filesystem = "macfuse", .source = "encfs@macfuse1"}.classify() ==
            StorageClass::Unknown,
        "a user-space file system of an unknown kind is Unknown");
    expect(Mac{.localMount = true,
               .filesystem = "apfs",
               .source = "/dev/disk3s1",
               .protocol = "Apple Fabric",
               .solidState = true,
               .internal = true}
                   .classify() == StorageClass::FastLocal,
           "the internal solid-state disk is Fast local");
    expect(Mac{.localMount = true,
               .filesystem = "apfs",
               .source = "/dev/disk5s1",
               .protocol = "USB",
               .solidState = true}
                   .classify() == StorageClass::SlowAttached,
           "a disk on USB is Slow attached");
    expect(Mac{.localMount = true,
               .filesystem = "exfat",
               .source = "/dev/disk6s1",
               .protocol = "Secure Digital",
               .removableMedia = true}
                   .classify() == StorageClass::SlowAttached,
           "an SD card is Slow attached");
    expect(Mac{.localMount = true,
               .filesystem = "hfs",
               .source = "/dev/disk4s2",
               .protocol = "SATA",
               .solidState = false,
               .internal = true}
                   .classify() == StorageClass::SlowAttached,
           "a rotational disk is Slow attached");
    expect(Mac{.localMount = true,
               .filesystem = "cd9660",
               .source = "/dev/disk7",
               .protocol = "ATAPI",
               .mediaKind = "IOCDMedia"}
                   .classify() == StorageClass::SlowAttached,
           "an optical disc is Slow attached");
    expect(Mac{.localMount = true,
               .filesystem = "apfs",
               .source = "/dev/disk9s1",
               .protocol = "Thunderbolt"}
                   .classify() == StorageClass::Unknown,
           "an external disk that reports no medium is Unknown");
}

void testRealFolder() {
    using bloom::platform::storageClass;
    using bloom::platform::storageVolume;
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("bloom-storage-class-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    const auto member = folder / "member.exr";
    {
        std::ofstream(member) << "x";
    }
    const auto volume = storageVolume(folder);
    expect(volume.volume.has_value(), "an existing folder has a volume id");
    expect(volume.storageClass != StorageClass::Network,
           "the temporary folder is not on a network volume");
    expect(storageClass(member) == volume.storageClass, "a file takes its folder's class");
    expect(storageClass(folder / "missing" / "deeper" / "file.exr") == volume.storageClass,
           "a missing path takes its nearest existing ancestor's class");
    expect(storageClass(folder) == volume.storageClass, "the answer is stable when asked again");
    // The id a file's identity reports is the id the classification is remembered under, so a
    // caller that observed the file asks nothing more of the file system.
    const auto identity = bloom::platform::observeFileIdentity(member);
    expect(identity && volume.volume && identity->volume == *volume.volume,
           "a file's identity names the volume the classification is remembered under");
    if (identity)
        expect(storageClass(std::filesystem::path{"bloom-no-such-folder"} / "file.exr",
                            identity->volume) == volume.storageClass,
               "a known volume id answers without looking at the path");
#if defined(_WIN32)
    // #718: a real share. The same folder reached through this computer's own drive share is a
    // network location, although its files report the volume id the disk was just remembered
    // under; and asking about the share does not change what the disk is.
    {
        const auto drive = folder.root_name().wstring();
        std::filesystem::path share;
        if (drive.size() == 2 && drive[1] == L':')
            share = std::filesystem::path(std::wstring(L"\\\\localhost\\") + drive[0] + L"$") /
                    folder.relative_path();
        std::error_code error;
        if (!share.empty() && std::filesystem::exists(share / "member.exr", error) && !error) {
            expect(storageClass(share / "member.exr") == StorageClass::Network &&
                       storageVolume(share).storageClass == StorageClass::Network,
                   "a file reached through a share is on network storage");
            expect(storageClass(share / "missing" / "file.exr") == StorageClass::Network,
                   "a missing path on a share is on network storage too");
            const auto shared = bloom::platform::observeFileIdentity(share / "member.exr");
            if (shared)
                expect(storageClass(share / "member.exr", shared->volume) == StorageClass::Network,
                       "the share is network storage for a caller that brings the volume id");
            expect(storageClass(member) == volume.storageClass &&
                       storageClass(folder) == volume.storageClass,
                   "the disk keeps its own class after its share was asked about");
            std::cout << "CONFIRMED a share's storage class\n";
        } else {
            std::cout << "NOT RUN a share's storage class: this account cannot reach the "
                         "computer's own drive share\n";
        }
    }
#endif
    expect(storageVolume(std::filesystem::path{}).storageClass == StorageClass::Unknown &&
               !storageVolume(std::filesystem::path{}).volume,
           "an empty path is Unknown");
    std::cout << "temporary folder: " << bloom::platform::storageClassName(volume.storageClass)
              << '\n';
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
}
} // namespace

int main() {
    using bloom::platform::storageClassName;
    testLinuxRule();
    testWindowsRule();
    testMacRule();
    testRealFolder();
    expect(storageClassName(StorageClass::Network) == "Network" &&
               storageClassName(StorageClass::SlowAttached) == "Slow attached" &&
               storageClassName(StorageClass::FastLocal) == "Fast local" &&
               storageClassName(StorageClass::Unknown) == "Unknown",
           "the classes carry their user-facing names");
    expect(bloom::platform::isSlowStorage(StorageClass::Network) &&
               bloom::platform::isSlowStorage(StorageClass::SlowAttached) &&
               bloom::platform::isSlowStorage(StorageClass::Unknown) &&
               !bloom::platform::isSlowStorage(StorageClass::FastLocal),
           "every class but Fast local is slow storage");
    if (failures == 0)
        std::cout << "PASS: storage classes\n";
    return failures == 0 ? 0 : 1;
}
