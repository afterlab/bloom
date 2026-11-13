#pragma once

// The pieces of an OpenType font file the variable font instancers share: a bounds-checked reader,
// the table directory (of a single font or of one font of a collection), the region scalar of a
// variation store, the HVAR advance deltas, and the writer of a static font file.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bloom::runtime::fonts::detail {

// A bounds-checked big-endian reader over a span. Reads past the end set `failed` and return zero,
// so a damaged table ends in one check rather than in an out-of-range read.
class Reader final {
  public:
    Reader(std::span<const std::uint8_t> data, const std::size_t offset = 0)
        : data_(data), position_(offset) {}

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    void seek(const std::size_t offset) noexcept { position_ = offset; }
    void skip(const std::size_t count) noexcept { position_ += count; }

    [[nodiscard]] std::uint8_t u8() noexcept {
        if (position_ >= data_.size()) {
            failed_ = true;
            return 0;
        }
        return data_[position_++];
    }
    [[nodiscard]] std::uint16_t u16() noexcept {
        const auto high = u8();
        return static_cast<std::uint16_t>((high << 8U) | u8());
    }
    [[nodiscard]] std::int16_t i16() noexcept { return static_cast<std::int16_t>(u16()); }
    [[nodiscard]] std::uint32_t u32() noexcept {
        const auto high = u16();
        return (static_cast<std::uint32_t>(high) << 16U) | u16();
    }
    [[nodiscard]] std::int32_t i32() noexcept { return static_cast<std::int32_t>(u32()); }
    [[nodiscard]] std::int8_t i8() noexcept { return static_cast<std::int8_t>(u8()); }
    // 2.14 and 16.16 fixed point.
    [[nodiscard]] double f2dot14() noexcept { return static_cast<double>(i16()) / 16384.0; }
    [[nodiscard]] double fixed() noexcept { return static_cast<double>(i32()) / 65536.0; }

  private:
    std::span<const std::uint8_t> data_;
    std::size_t position_ = 0;
    bool failed_ = false;
};

[[nodiscard]] inline std::string tagString(const std::uint32_t tag) {
    std::string out(4, ' ');
    for (std::size_t i = 0; i < 4; ++i)
        out[i] = static_cast<char>((tag >> (24U - 8U * i)) & 0xFFU);
    return out;
}

constexpr std::uint32_t makeTag(const char (&text)[5]) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) << 24U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(text[3]));
}

struct Sfnt final {
    std::map<std::uint32_t, std::span<const std::uint8_t>> tables;
    std::uint32_t version = 0;
    [[nodiscard]] std::span<const std::uint8_t> table(const std::uint32_t tag) const {
        const auto found = tables.find(tag);
        return found == tables.end() ? std::span<const std::uint8_t>{} : found->second;
    }
};

// One font of a file: the file's own directory, or font `collectionIndex` of a collection, whose
// table offsets are from the start of the file like any other's. Null when it does not parse or
// the collection has no such font.
[[nodiscard]] inline std::optional<Sfnt> parseSfnt(const std::span<const std::uint8_t> font,
                                            const std::uint32_t collectionIndex) {
    Reader reader(font);
    Sfnt sfnt;
    sfnt.version = reader.u32();
    if (reader.failed())
        return std::nullopt;
    if (sfnt.version == makeTag("ttcf")) {
        reader.skip(4);
        const auto fonts = reader.u32();
        if (reader.failed() || collectionIndex >= fonts)
            return std::nullopt;
        reader.skip(static_cast<std::size_t>(collectionIndex) * 4U);
        const auto directory = reader.u32();
        reader.seek(directory);
        sfnt.version = reader.u32();
        if (reader.failed() || sfnt.version == makeTag("ttcf"))
            return std::nullopt;
    } else if (collectionIndex != 0) {
        return std::nullopt;
    }
    const auto count = reader.u16();
    reader.skip(6);
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto tag = reader.u32();
        reader.skip(4);
        const auto offset = reader.u32();
        const auto length = reader.u32();
        if (reader.failed() || offset > font.size() || length > font.size() - offset)
            return std::nullopt;
        sfnt.tables[tag] = font.subspan(offset, length);
    }
    return sfnt;
}

// How much of a region a normalized location is inside, 0 to 1.
[[nodiscard]] inline double regionScalar(const std::vector<double>& location,
                                  const std::vector<double>& start,
                                  const std::vector<double>& peak,
                                  const std::vector<double>& end) {
    double scalar = 1.0;
    for (std::size_t i = 0; i < peak.size(); ++i) {
        const auto p = peak[i];
        const auto lower = start[i];
        const auto upper = end[i];
        if (p == 0.0 || lower > p || p > upper || (lower < 0.0 && upper > 0.0))
            continue;
        const auto v = location[i];
        if (v == p)
            continue;
        if (v <= lower || upper <= v)
            return 0.0;
        scalar *= v < p ? (v - lower) / (p - lower) : (v - upper) / (p - upper);
    }
    return scalar;
}

// ---- HVAR -------------------------------------------------------------------------------------

struct Hvar final {
    std::span<const std::uint8_t> table;
    std::size_t storeOffset = 0;
    std::size_t advanceMapOffset = 0;
};

// The advance-width delta for `glyph` at `location`, or nullopt when the table is damaged.
[[nodiscard]] inline std::optional<double> hvarAdvanceDelta(const Hvar& hvar, const std::uint32_t glyph,
                                                     const std::vector<double>& location) {
    const auto table = hvar.table;
    std::uint32_t outer = 0;
    std::uint32_t inner = glyph;
    if (hvar.advanceMapOffset != 0) {
        Reader map(table, hvar.advanceMapOffset);
        const auto format = map.u8();
        const auto entryFormat = map.u8();
        const std::uint32_t count = format == 0 ? map.u16() : map.u32();
        const auto innerBits = static_cast<std::uint32_t>((entryFormat & 0x0FU) + 1U);
        const auto entrySize = static_cast<std::uint32_t>(((entryFormat & 0x30U) >> 4U) + 1U);
        if (map.failed() || count == 0)
            return std::nullopt;
        const auto index = std::min(glyph, count - 1U);
        map.seek(map.position() + static_cast<std::size_t>(index) * entrySize);
        std::uint32_t entry = 0;
        for (std::uint32_t i = 0; i < entrySize; ++i)
            entry = (entry << 8U) | map.u8();
        if (map.failed())
            return std::nullopt;
        outer = entry >> innerBits;
        inner = entry & ((1U << innerBits) - 1U);
    }
    Reader store(table, hvar.storeOffset);
    store.skip(2);
    const auto regionListOffset = store.u32();
    const auto dataCount = store.u16();
    if (store.failed() || outer >= dataCount)
        return std::nullopt;
    store.seek(hvar.storeOffset + 8 + static_cast<std::size_t>(outer) * 4);
    const auto dataOffset = store.u32();

    Reader regions(table, hvar.storeOffset + regionListOffset);
    const auto axisCount = regions.u16();
    const auto regionCount = regions.u16();
    if (regions.failed())
        return std::nullopt;

    Reader data(table, hvar.storeOffset + dataOffset);
    const auto itemCount = data.u16();
    const auto wordCountField = data.u16();
    const auto regionIndexCount = data.u16();
    if (data.failed() || inner >= itemCount)
        return std::nullopt;
    const bool longWords = (wordCountField & 0x8000U) != 0;
    const auto wordCount = static_cast<std::size_t>(wordCountField & 0x7FFFU);
    if (wordCount > regionIndexCount || axisCount != location.size())
        return std::nullopt;
    std::vector<std::uint16_t> regionIndexes;
    for (std::uint16_t i = 0; i < regionIndexCount; ++i)
        regionIndexes.push_back(data.u16());
    const auto wordSize = longWords ? 4U : 2U;
    const auto rowSize = wordCount * wordSize + (regionIndexCount - wordCount) * (wordSize / 2U);
    data.skip(static_cast<std::size_t>(inner) * rowSize);
    double total = 0.0;
    for (std::uint16_t i = 0; i < regionIndexCount; ++i) {
        double delta = 0.0;
        if (i < wordCount)
            delta = longWords ? static_cast<double>(data.i32()) : static_cast<double>(data.i16());
        else
            delta = longWords ? static_cast<double>(data.i16()) : static_cast<double>(data.i8());
        if (data.failed() || regionIndexes[i] >= regionCount)
            return std::nullopt;
        Reader region(table, hvar.storeOffset + regionListOffset + 4 +
                                 static_cast<std::size_t>(regionIndexes[i]) * axisCount * 6U);
        std::vector<double> start(axisCount);
        std::vector<double> peak(axisCount);
        std::vector<double> end(axisCount);
        for (std::uint16_t a = 0; a < axisCount; ++a) {
            start[a] = region.f2dot14();
            peak[a] = region.f2dot14();
            end[a] = region.f2dot14();
        }
        if (region.failed())
            return std::nullopt;
        total += delta * regionScalar(location, start, peak, end);
    }
    return total;
}

inline void put16(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}
inline void put32(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    put16(out, value >> 16U);
    put16(out, value & 0xFFFFU);
}
inline void putS16(std::vector<std::uint8_t>& out, const int value) {
    put16(out, static_cast<std::uint32_t>(static_cast<std::uint16_t>(static_cast<std::int16_t>(
                   std::clamp(value, -32768, 32767)))));
}

// The four-byte sums an sfnt directory records.
[[nodiscard]] inline std::uint32_t checksum(const std::vector<std::uint8_t>& data) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < data.size(); i += 4) {
        std::uint32_t word = 0;
        for (std::size_t b = 0; b < 4; ++b)
            word = (word << 8U) | (i + b < data.size() ? data[i + b] : 0U);
        sum += word;
    }
    return sum;
}

// A single-font file of `tables` (which hold a `head` of at least 12 bytes): the directory, the
// tables padded to four bytes, and the file checksum in `head`.
[[nodiscard]] inline std::vector<std::uint8_t>
writeSfnt(const std::uint32_t version, std::map<std::uint32_t, std::vector<std::uint8_t>> tables) {
    auto& head = tables[makeTag("head")];
    if (head.size() < 12)
        head.resize(12, 0);
    head[8] = head[9] = head[10] = head[11] = 0; // checkSumAdjustment, set below.
    const auto count = static_cast<std::uint16_t>(tables.size());
    std::uint16_t searchRange = 1;
    std::uint16_t entrySelector = 0;
    while (static_cast<std::uint32_t>(searchRange) * 2U <= count) {
        searchRange = static_cast<std::uint16_t>(searchRange * 2U);
        ++entrySelector;
    }
    searchRange = static_cast<std::uint16_t>(searchRange * 16U);
    std::vector<std::uint8_t> file;
    put32(file, version);
    put16(file, count);
    put16(file, searchRange);
    put16(file, entrySelector);
    put16(file, static_cast<std::uint32_t>(count * 16U - searchRange));
    auto offset = static_cast<std::uint32_t>(12U + count * 16U);
    for (const auto& [tag, data] : tables) {
        put32(file, tag);
        put32(file, checksum(data));
        put32(file, offset);
        put32(file, static_cast<std::uint32_t>(data.size()));
        offset += static_cast<std::uint32_t>((data.size() + 3U) & ~static_cast<std::size_t>(3U));
    }
    std::size_t headPosition = 0;
    for (const auto& [tag, data] : tables) {
        if (tag == makeTag("head"))
            headPosition = file.size();
        file.insert(file.end(), data.begin(), data.end());
        while (file.size() % 4 != 0)
            file.push_back(0);
    }
    const auto adjustment = 0xB1B0AFBAU - checksum(file);
    for (std::size_t i = 0; i < 4; ++i)
        file[headPosition + 8 + i] =
            static_cast<std::uint8_t>((adjustment >> (24U - 8U * i)) & 0xFFU);
    return file;
}

// #489: a variable font with CFF2 outlines at `location` (normalized, after avar), as a static
// font with a `CFF ` table. Null when its tables are damaged. variable_font_cff2.cpp.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> instantiateCff2(const Sfnt& sfnt,
                                                                       const std::vector<double>& location);

} // namespace bloom::runtime::fonts::detail
