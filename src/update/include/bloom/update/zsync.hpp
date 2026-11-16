#pragma once

#include <bloom/update/check.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The AppImage's delta update (ADR 0041). A release publishes, beside the AppImage, a control
// file in the zsync format: the checksums of every block of the new file. Bloom finds the
// blocks the installed AppImage already holds, asks the server only for the ranges it lacks,
// and then checks the assembled file exactly as it checks a whole download: size, SHA-256 and
// Ed25519 signature from the signed feed. The control file decides nothing about trust: a wrong
// or hostile one can only make the delta fail, and the whole file is downloaded instead.
namespace bloom::update {

struct ZsyncControl final {
    struct Block final {
        // The stored bytes of the rolling checksum, as a number (its low `weakBytes` bytes).
        std::uint32_t weak = 0;
        // The first `strongBytes` bytes of the block's MD4.
        std::array<std::uint8_t, 16> strong{};
    };

    std::uint64_t length = 0;
    std::uint32_t blockSize = 0;
    std::uint8_t sequentialMatches = 1;
    std::uint8_t weakBytes = 0;
    std::uint8_t strongBytes = 0;
    std::vector<Block> blocks;
};

// The largest control file that is read (a 4 GiB file in 4096-byte blocks needs less).
inline constexpr std::uint64_t kMaximumZsyncControlBytes = 24ULL << 20U;

// Reads a control file. Nothing is returned for one this reader does not fully understand:
// a damaged header or block list, a block size or checksum length outside the format's range,
// or the format's compressed-stream additions, which a Bloom release never uses.
[[nodiscard]] std::optional<ZsyncControl> readZsyncControl(std::span<const std::byte> bytes);

struct ZsyncControlHeader final {
    // The `Filename` and `URL` lines: the file's name, as it is published beside the control file.
    std::string fileName;
    // The `MTime` line, as seconds since 1970.
    std::int64_t modifiedSeconds = 0;
};

// Writes the control file of `file`, byte for byte as the reference tool (zsyncmake 0.6.6)
// writes it for the same file, name and time. Nothing when the file cannot be read.
[[nodiscard]] std::optional<std::vector<std::byte>>
makeZsyncControl(const std::filesystem::path& file, const ZsyncControlHeader& header,
                 const std::function<bool()>& cancelled = {});

struct ZsyncMatch final {
    // For each block of the new file, where the same bytes start in the local file, or -1.
    std::vector<std::int64_t> localOffsets;
    std::uint64_t reusedBytes = 0;
};

// Finds the blocks of the new file inside a local file, at any offset. Nothing when the local
// file cannot be read or the search was cancelled.
[[nodiscard]] std::optional<ZsyncMatch> matchLocalBlocks(const ZsyncControl& control,
                                                         const std::filesystem::path& local,
                                                         const std::function<bool()>& cancelled);

struct DeltaStatistics final {
    // The delta was used for the file that was accepted.
    bool used = false;
    std::uint64_t reusedBytes = 0;
    std::uint64_t fetchedBytes = 0;
    // Why the whole file was downloaded instead, when it was.
    std::string fallbackReason;
};

// Downloads `target` to `destination`, taking what it can from `installedFile` through the
// control file `control` (both entries of one verified feed). Whatever goes wrong with the
// delta, the whole file is downloaded the ordinary way; the result is the same either way: a
// file that holds the size, SHA-256 and signature the feed gives, or nothing.
[[nodiscard]] DownloadResult downloadWithDelta(UpdateTransport& transport, const FeedFile& target,
                                               const FeedFile& control,
                                               const std::filesystem::path& installedFile,
                                               std::span<const Ed25519PublicKey> trustedKeys,
                                               const std::filesystem::path& destination,
                                               const std::function<bool()>& cancelled,
                                               const TransportProgress& progress,
                                               DeltaStatistics* statistics = nullptr);

} // namespace bloom::update
