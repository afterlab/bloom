#include "ocio_config_archive.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace bloom::color::detail {
namespace {

constexpr std::uint32_t kEndSignature = 0x06054b50U;
constexpr std::uint32_t kCentralSignature = 0x02014b50U;
constexpr std::uint32_t kLocalSignature = 0x04034b50U;
constexpr std::size_t kEndBytes = 22;
constexpr std::size_t kCentralBytes = 46;
constexpr std::size_t kLocalBytes = 30;

class Cursor final {
  public:
    explicit Cursor(const std::span<const std::byte> bytes) : bytes_(bytes) {}
    [[nodiscard]] bool has(const std::uint64_t at, const std::uint64_t count) const {
        return at <= bytes_.size() && count <= bytes_.size() - at;
    }
    [[nodiscard]] std::uint16_t u16(const std::size_t at) const {
        return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes_[at]) |
                                          std::to_integer<unsigned>(bytes_[at + 1]) << 8U);
    }
    [[nodiscard]] std::uint32_t u32(const std::size_t at) const {
        return static_cast<std::uint32_t>(u16(at)) | static_cast<std::uint32_t>(u16(at + 2)) << 16U;
    }
    [[nodiscard]] std::span<const std::byte> slice(const std::size_t at,
                                                   const std::size_t count) const {
        return bytes_.subspan(at, count);
    }
    [[nodiscard]] std::size_t size() const { return bytes_.size(); }

  private:
    std::span<const std::byte> bytes_;
};

[[nodiscard]] bool inflateExact(const std::span<const std::byte> compressed,
                                const std::span<std::byte> output) {
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        return false;
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    const int status = inflate(&stream, Z_FINISH);
    const bool exact = status == Z_STREAM_END && stream.total_out == output.size() &&
                       stream.avail_in == 0;
    inflateEnd(&stream);
    return exact;
}

} // namespace

std::optional<ConfigArchiveTable> readConfigArchive(const std::span<const std::byte> bytes) {
    const Cursor in(bytes);
    if (bytes.size() < kEndBytes || bytes.size() > kConfigMaximumArchiveBytes)
        return std::nullopt;
    // The end record is the last 22 bytes plus a comment of up to 65535.
    std::size_t end = bytes.size() - kEndBytes;
    const std::size_t floor = bytes.size() > kEndBytes + 65535 ? bytes.size() - kEndBytes - 65535 : 0;
    while (in.u32(end) != kEndSignature) {
        if (end == floor)
            return std::nullopt;
        --end;
    }
    const auto entryCount = in.u16(end + 10);
    const auto directoryBytes = in.u32(end + 12);
    const auto directoryOffset = in.u32(end + 16);
    if (in.u16(end + 4) != 0 || in.u16(end + 6) != 0 || in.u16(end + 8) != entryCount ||
        entryCount == 0xffffU || directoryBytes == 0xffffffffU || directoryOffset == 0xffffffffU ||
        entryCount > kOcioLooseMaximumEntryCount ||
        static_cast<std::uint64_t>(directoryOffset) + directoryBytes > end)
        return std::nullopt;

    ConfigArchiveTable table;
    std::uint64_t total = 0;
    std::size_t at = directoryOffset;
    const std::size_t directoryEnd = static_cast<std::size_t>(directoryOffset) + directoryBytes;
    for (std::uint16_t entry = 0; entry < entryCount; ++entry) {
        if (!in.has(at, kCentralBytes) || at + kCentralBytes > directoryEnd ||
            in.u32(at) != kCentralSignature)
            return std::nullopt;
        const auto flags = in.u16(at + 8);
        const auto method = in.u16(at + 10);
        const auto crc = in.u32(at + 16);
        const auto compressedBytes = in.u32(at + 20);
        const auto size = in.u32(at + 24);
        const auto nameBytes = in.u16(at + 28);
        const auto extraBytes = in.u16(at + 30);
        const auto commentBytes = in.u16(at + 32);
        const auto diskStart = in.u16(at + 34);
        const auto attributes = in.u32(at + 38);
        const auto localOffset = in.u32(at + 42);
        const std::size_t next =
            at + kCentralBytes + static_cast<std::size_t>(nameBytes) + extraBytes + commentBytes;
        if (!in.has(at + kCentralBytes, nameBytes) || next > directoryEnd || diskStart != 0)
            return std::nullopt;
        const std::string name(reinterpret_cast<const char*>(in.slice(at + kCentralBytes, nameBytes).data()),
                               nameBytes);
        at = next;
        if (!name.empty() && name.back() == '/') {
            // A directory entry holds nothing.
            if (size != 0)
                return std::nullopt;
            continue;
        }
        // Encrypted, or a symbolic link: neither is a config resource.
        const bool unixLink = (attributes >> 16U & 0170000U) == 0120000U;
        if ((flags & 0x1U) != 0 || unixLink || (method != 0 && method != 8) ||
            compressedBytes == 0xffffffffU || size == 0xffffffffU ||
            size > kOcioLooseMaximumResourceBytes ||
            (method == 0 && compressedBytes != size) || name.size() > kOcioLooseMaximumKeyBytes ||
            canonicalConfigResourceKey(name) != name)
            return std::nullopt;
        total += size;
        if (total > kOcioLooseMaximumAggregateBytes)
            return std::nullopt;

        if (!in.has(localOffset, kLocalBytes) || in.u32(localOffset) != kLocalSignature)
            return std::nullopt;
        const std::size_t dataAt = static_cast<std::size_t>(localOffset) + kLocalBytes +
                                   in.u16(localOffset + 26) + in.u16(localOffset + 28);
        if (!in.has(dataAt, compressedBytes) || dataAt + compressedBytes > directoryOffset)
            return std::nullopt;

        const std::size_t offset = table.payload.size();
        table.payload.resize(offset + size);
        const auto output = std::span(table.payload).subspan(offset, size);
        const auto stored = in.slice(dataAt, compressedBytes);
        if (size == 0) {
            // An empty file has no stream to read; its CRC is zero.
        } else if (method == 0) {
            std::memcpy(output.data(), stored.data(), size);
        } else if (!inflateExact(stored, output)) {
            return std::nullopt;
        }
        if (static_cast<std::uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(output.data()),
                                              static_cast<uInt>(output.size()))) != crc)
            return std::nullopt;
        table.index.push_back({name, offset, size});
    }
    if (table.index.empty())
        return std::nullopt;
    std::ranges::sort(table.index, {}, &ConfigResourceSlice::key);
    for (std::size_t index = 1; index < table.index.size(); ++index)
        if (!(table.index[index - 1].key < table.index[index].key))
            return std::nullopt;
    return table;
}

} // namespace bloom::color::detail
