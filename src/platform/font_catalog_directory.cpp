#include <bloom/platform/font_catalog.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <string_view>

namespace bloom::platform {
namespace {

constexpr std::uintmax_t kMaximumFontBytes = static_cast<std::uintmax_t>(64) * 1024U * 1024U;

[[nodiscard]] std::uint32_t be32(std::span<const std::uint8_t> bytes, const std::size_t at) {
    return (static_cast<std::uint32_t>(bytes[at]) << 24U) |
           (static_cast<std::uint32_t>(bytes[at + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[at + 2]) << 8U) | bytes[at + 3];
}
[[nodiscard]] std::uint16_t be16(std::span<const std::uint8_t> bytes, const std::size_t at) {
    return static_cast<std::uint16_t>((bytes[at] << 8U) | bytes[at + 1]);
}

[[nodiscard]] bool inRange(std::span<const std::uint8_t> bytes, const std::size_t at,
                           const std::size_t length) {
    return at <= bytes.size() && length <= bytes.size() - at;
}

struct Table final {
    std::size_t offset = 0;
    std::size_t length = 0;
};

[[nodiscard]] std::optional<Table> findTable(std::span<const std::uint8_t> bytes,
                                             const std::size_t fontOffset,
                                             const std::string_view tag) {
    if (!inRange(bytes, fontOffset, 12))
        return std::nullopt;
    const std::size_t count = be16(bytes, fontOffset + 4);
    for (std::size_t index = 0; index < count; ++index) {
        const auto record = fontOffset + 12 + index * 16;
        if (!inRange(bytes, record, 16))
            return std::nullopt;
        if (std::string_view(reinterpret_cast<const char*>(&bytes[record]), 4) != tag)
            continue;
        const Table table{be32(bytes, record + 8), be32(bytes, record + 12)};
        if (!inRange(bytes, table.offset, table.length))
            return std::nullopt;
        return table;
    }
    return std::nullopt;
}

void appendUtf8(std::string& out, const char32_t scalar) {
    if (scalar < 0x80) {
        out += static_cast<char>(scalar);
    } else if (scalar < 0x800) {
        out += static_cast<char>(0xc0U | (scalar >> 6U));
        out += static_cast<char>(0x80U | (scalar & 0x3fU));
    } else if (scalar < 0x10000) {
        out += static_cast<char>(0xe0U | (scalar >> 12U));
        out += static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU));
        out += static_cast<char>(0x80U | (scalar & 0x3fU));
    } else {
        out += static_cast<char>(0xf0U | (scalar >> 18U));
        out += static_cast<char>(0x80U | ((scalar >> 12U) & 0x3fU));
        out += static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU));
        out += static_cast<char>(0x80U | (scalar & 0x3fU));
    }
}

// The best string for a name id: Windows Unicode English first, any Windows Unicode next, then a
// Macintosh Roman (ASCII) one.
[[nodiscard]] std::string nameString(std::span<const std::uint8_t> bytes, const Table& table,
                                     const std::uint16_t wanted) {
    if (table.length < 6)
        return {};
    const auto base = table.offset;
    const std::size_t count = be16(bytes, base + 2);
    const std::size_t stringBase = base + be16(bytes, base + 4);
    int bestRank = 0;
    std::string best;
    for (std::size_t index = 0; index < count; ++index) {
        const auto record = base + 6 + index * 12;
        if (record + 12 > base + table.length)
            break;
        if (be16(bytes, record + 6) != wanted)
            continue;
        const auto platform = be16(bytes, record);
        const auto language = be16(bytes, record + 4);
        const std::size_t length = be16(bytes, record + 8);
        const auto start = stringBase + be16(bytes, record + 10);
        if (!inRange(bytes, start, length))
            continue;
        std::string text;
        int rank = 0;
        if (platform == 3 || platform == 0) {
            for (std::size_t at = 0; at + 1 < length; at += 2) {
                char32_t scalar = be16(bytes, start + at);
                if (scalar >= 0xd800 && scalar < 0xdc00 && at + 3 < length) {
                    const char32_t low = be16(bytes, start + at + 2);
                    if (low >= 0xdc00 && low < 0xe000) {
                        scalar = 0x10000 + ((scalar - 0xd800) << 10U) + (low - 0xdc00);
                        at += 2;
                    }
                }
                appendUtf8(text, scalar);
            }
            rank = platform == 3 && language == 0x409 ? 3 : 2;
        } else if (platform == 1) {
            for (std::size_t at = 0; at < length; ++at)
                if (bytes[start + at] < 0x80)
                    text += static_cast<char>(bytes[start + at]);
            rank = 1;
        }
        if (!text.empty() && rank > bestRank) {
            bestRank = rank;
            best = std::move(text);
        }
    }
    return best;
}

[[nodiscard]] std::optional<SfntFaceNames> readFace(std::span<const std::uint8_t> bytes,
                                                    const std::size_t fontOffset,
                                                    const std::uint32_t faceIndex) {
    const auto name = findTable(bytes, fontOffset, "name");
    if (!name)
        return std::nullopt;
    SfntFaceNames face;
    face.faceIndex = faceIndex;
    face.family = nameString(bytes, *name, 16);
    if (face.family.empty())
        face.family = nameString(bytes, *name, 1);
    face.style = nameString(bytes, *name, 17);
    if (face.style.empty())
        face.style = nameString(bytes, *name, 2);
    face.postscriptName = nameString(bytes, *name, 6);
    if (face.family.empty() || face.style.empty())
        return std::nullopt;
    if (const auto os2 = findTable(bytes, fontOffset, "OS/2"); os2 && os2->length >= 64) {
        const int weight = be16(bytes, os2->offset + 4);
        if (weight >= 1 && weight <= 1000)
            face.weight = weight;
        face.italic = (be16(bytes, os2->offset + 62) & 0x1U) != 0;
    } else if (const auto head = findTable(bytes, fontOffset, "head"); head && head->length >= 46) {
        face.italic = (be16(bytes, head->offset + 44) & 0x2U) != 0;
    }
    return face;
}

} // namespace

std::vector<SfntFaceNames> readSfntFaceNames(std::span<const std::uint8_t> bytes) {
    std::vector<SfntFaceNames> faces;
    if (bytes.size() < 12)
        return faces;
    if (std::string_view(reinterpret_cast<const char*>(bytes.data()), 4) == "ttcf") {
        const std::size_t count = std::min<std::size_t>(be32(bytes, 8), 256);
        for (std::size_t index = 0; index < count; ++index) {
            if (!inRange(bytes, 12 + index * 4, 4))
                break;
            if (auto face = readFace(bytes, be32(bytes, 12 + index * 4),
                                     static_cast<std::uint32_t>(index)))
                faces.push_back(*std::move(face));
        }
        return faces;
    }
    if (auto face = readFace(bytes, 0, 0))
        faces.push_back(*std::move(face));
    return faces;
}

std::optional<core::Sha256Digest> FontCatalogueProvider::digestFile(
    const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumFontBytes)
        return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return std::nullopt;
    core::Sha256Hasher hasher;
    std::array<std::byte, static_cast<std::size_t>(64) * 1024U> buffer{};
    std::uintmax_t remaining = size;
    while (remaining != 0) {
        const auto requested = static_cast<std::streamsize>(
            std::min<std::uintmax_t>(remaining, static_cast<std::uintmax_t>(buffer.size())));
        stream.read(reinterpret_cast<char*>(buffer.data()), requested);
        if (stream.gcount() != requested)
            return std::nullopt;
        if (!hasher.update(
                std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(requested))))
            return std::nullopt;
        remaining -= static_cast<std::uintmax_t>(requested);
    }
    return hasher.finalize();
}

FontCatalogue FontCatalogueProvider::enumerateDirectories(
    const std::span<const std::filesystem::path> directories, const Cancellation& cancelled,
    const bool digests) {
    FontCatalogue result;
    result.status = FontCatalogueStatus::Available;
    for (const auto& directory : directories) {
        std::error_code error;
        std::filesystem::recursive_directory_iterator iterator(
            directory, std::filesystem::directory_options::skip_permission_denied, error);
        if (error)
            continue;
        for (const auto end = std::filesystem::recursive_directory_iterator{}; iterator != end;
             iterator.increment(error)) {
            if (error)
                break;
            if (cancelled && cancelled())
                return result;
            std::error_code entryError;
            if (!iterator->is_regular_file(entryError))
                continue;
            auto extension = iterator->path().extension().string();
            std::ranges::transform(extension, extension.begin(), [](const unsigned char c) {
                return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
            });
            if (extension != ".ttf" && extension != ".otf" && extension != ".ttc" &&
                extension != ".otc")
                continue;
            const auto size = iterator->file_size(entryError);
            if (entryError || size == 0 || size > kMaximumFontBytes)
                continue;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            {
                std::ifstream stream(iterator->path(), std::ios::binary);
                if (!stream ||
                    !stream.read(reinterpret_cast<char*>(bytes.data()),
                                 static_cast<std::streamsize>(bytes.size())))
                    continue;
            }
            core::Sha256Digest digest;
            if (digests) {
                const auto hashed = core::Sha256Hasher::hash(std::as_bytes(std::span(bytes)));
                if (!hashed)
                    continue;
                digest = *hashed;
            }
            for (auto& names : readSfntFaceNames(bytes))
                result.faces.push_back({std::move(names.family), std::move(names.style),
                                        names.weight, 100, names.italic, iterator->path(), digest,
                                        FontSource::System, names.faceIndex,
                                        std::move(names.postscriptName)});
        }
    }
    return result;
}

} // namespace bloom::platform
