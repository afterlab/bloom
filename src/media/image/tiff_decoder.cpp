#include "tiff_decoder.hpp"
#include "image_budget.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <zlib.h>

namespace bloom::media::detail {
namespace {
// The per-dimension ceiling the PNG and JPEG codec uses.
constexpr std::uint64_t kMaxDimension = std::uint64_t{1} << 24;

enum Tag : std::uint16_t {
    ImageWidth = 256,
    ImageLength = 257,
    BitsPerSample = 258,
    Compression = 259,
    Photometric = 262,
    FillOrder = 266,
    StripOffsets = 273,
    Orientation = 274,
    SamplesPerPixel = 277,
    RowsPerStrip = 278,
    StripByteCounts = 279,
    PlanarConfiguration = 284,
    Predictor = 317,
    TileWidth = 322,
    TileLength = 323,
    TileOffsets = 324,
    TileByteCounts = 325,
    ExtraSamples = 338,
    SampleFormat = 339,
};

enum class Codec : std::uint8_t { None, Lzw, Deflate, PackBits };

struct Layout {
    bool littleEndian = true;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t bytesPerSample = 1;
    std::uint32_t samplesPerPixel = 1;
    bool grey = false;
    bool whiteIsZero = false;
    bool hasAlpha = false;
    bool associatedAlpha = false;
    Codec codec = Codec::None;
    bool predictor = false;
    bool tiled = false;
    std::uint32_t unitWidth = 0;
    std::uint32_t unitHeight = 0;
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint64_t> counts;
};

struct Reader {
    std::span<const std::byte> bytes;
    bool littleEndian = true;
    [[nodiscard]] std::optional<std::uint64_t> read(const std::uint64_t at,
                                                    const std::size_t size) const {
        if (at > bytes.size() || size > bytes.size() - at)
            return std::nullopt;
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < size; ++index) {
            const auto byte = static_cast<std::uint64_t>(
                static_cast<unsigned char>(bytes[static_cast<std::size_t>(at) + index]));
            value |= littleEndian ? byte << (8U * index) : byte << (8U * (size - 1U - index));
        }
        return value;
    }
};

struct Entry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint64_t count = 0;
    std::uint64_t valueAt = 0; // file offset of the values
};

[[nodiscard]] std::size_t typeSize(const std::uint16_t type) {
    switch (type) {
    case 1: // byte
    case 2: // ascii
    case 6: // signed byte
    case 7: // undefined
        return 1;
    case 3: // short
    case 8: // signed short
        return 2;
    case 4: // long
    case 9: // signed long
    case 11: // float
        return 4;
    case 5:  // rational
    case 10: // signed rational
    case 12: // double
        return 8;
    default:
        return 0;
    }
}

// The integer values of an entry (byte, short or long); nullopt for any other type or when the
// values lie outside the file. The count is checked against the file before anything is allocated.
[[nodiscard]] std::optional<std::vector<std::uint64_t>> values(const Reader& reader,
                                                               const Entry& entry) {
    if (entry.type != 1 && entry.type != 3 && entry.type != 4)
        return std::nullopt;
    const auto size = typeSize(entry.type);
    if (entry.count > reader.bytes.size() / size)
        return std::nullopt;
    std::vector<std::uint64_t> result;
    result.reserve(static_cast<std::size_t>(entry.count));
    for (std::uint64_t index = 0; index < entry.count; ++index) {
        const auto value = reader.read(entry.valueAt + index * size, size);
        if (!value)
            return std::nullopt;
        result.push_back(*value);
    }
    return result;
}

struct Parsed {
    Layout layout;
    std::string refusal;
    [[nodiscard]] bool ok() const { return refusal.empty(); }
};

Parsed refuse(std::string reason) {
    Parsed parsed;
    parsed.refusal = std::move(reason);
    return parsed;
}

Parsed parse(const std::span<const std::byte> bytes) {
    constexpr auto kDamaged = "The TIFF file is damaged or truncated";
    if (bytes.size() < 8)
        return refuse(kDamaged);
    Reader reader{bytes};
    const auto first = static_cast<unsigned char>(bytes[0]);
    const auto second = static_cast<unsigned char>(bytes[1]);
    if (first == 'I' && second == 'I')
        reader.littleEndian = true;
    else if (first == 'M' && second == 'M')
        reader.littleEndian = false;
    else
        return refuse(kDamaged);
    const auto magic = reader.read(2, 2);
    if (magic && *magic == 43)
        return refuse("Large (64-bit offset) TIFF files are not supported; save the image as a "
                      "standard TIFF");
    if (!magic || *magic != 42)
        return refuse(kDamaged);
    const auto ifd = reader.read(4, 4);
    if (!ifd)
        return refuse(kDamaged);
    const auto entryCount = reader.read(*ifd, 2);
    if (!entryCount || *entryCount == 0)
        return refuse(kDamaged);
    std::vector<Entry> entries;
    for (std::uint64_t index = 0; index < *entryCount; ++index) {
        const auto at = *ifd + 2 + index * 12;
        const auto tag = reader.read(at, 2);
        const auto type = reader.read(at + 2, 2);
        const auto count = reader.read(at + 4, 4);
        if (!tag || !type || !count)
            return refuse(kDamaged);
        Entry entry{static_cast<std::uint16_t>(*tag), static_cast<std::uint16_t>(*type), *count, 0};
        const auto size = typeSize(entry.type);
        if (size == 0)
            continue;
        if (size * entry.count <= 4) {
            entry.valueAt = at + 8;
        } else {
            const auto offset = reader.read(at + 8, 4);
            if (!offset)
                return refuse(kDamaged);
            entry.valueAt = *offset;
        }
        entries.push_back(entry);
    }
    const auto find = [&](const std::uint16_t tag) -> const Entry* {
        for (const auto& entry : entries)
            if (entry.tag == tag)
                return &entry;
        return nullptr;
    };
    // One scalar value of a tag, or `fallback` when the tag is absent.
    const auto scalar = [&](const std::uint16_t tag, const std::uint64_t fallback,
                            bool& valid) -> std::uint64_t {
        const auto* entry = find(tag);
        if (entry == nullptr)
            return fallback;
        const auto list = values(reader, *entry);
        if (!list || list->empty()) {
            valid = false;
            return fallback;
        }
        return list->front();
    };
    bool valid = true;
    Layout layout;
    layout.littleEndian = reader.littleEndian;
    const auto width = scalar(ImageWidth, 0, valid);
    const auto height = scalar(ImageLength, 0, valid);
    if (!valid)
        return refuse(kDamaged);
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension)
        return refuse("The TIFF image has invalid dimensions");
    layout.width = static_cast<std::uint32_t>(width);
    layout.height = static_cast<std::uint32_t>(height);

    const auto samples = scalar(SamplesPerPixel, 1, valid);
    const auto photometric = scalar(Photometric, 1, valid);
    if (!valid)
        return refuse(kDamaged);
    if (samples < 1 || samples > 16)
        return refuse("The TIFF image has an unsupported number of channels");
    layout.samplesPerPixel = static_cast<std::uint32_t>(samples);
    if (photometric == 0 || photometric == 1) {
        layout.grey = true;
        layout.whiteIsZero = photometric == 0;
    } else if (photometric == 2) {
        if (samples < 3)
            return refuse(kDamaged);
    } else {
        return refuse("Only grey and RGB TIFF images are supported; palette, CMYK, YCbCr and "
                      "Lab images must be converted first");
    }
    const std::uint32_t colourSamples = layout.grey ? 1U : 3U;
    layout.hasAlpha = layout.samplesPerPixel > colourSamples;
    if (layout.hasAlpha) {
        bool extraValid = true;
        if (const auto* extra = find(ExtraSamples)) {
            const auto list = values(reader, *extra);
            if (!list || list->empty())
                extraValid = false;
            else
                layout.associatedAlpha = list->front() == 1;
        }
        if (!extraValid)
            return refuse(kDamaged);
    }

    const auto* bits = find(BitsPerSample);
    std::vector<std::uint64_t> bitList{1};
    if (bits != nullptr) {
        auto list = values(reader, *bits);
        if (!list || list->empty())
            return refuse(kDamaged);
        bitList = std::move(*list);
    }
    if (bitList.size() < layout.samplesPerPixel && bitList.size() != 1)
        return refuse(kDamaged);
    for (const auto depth : bitList)
        if (depth != bitList.front())
            return refuse("TIFF images with mixed bit depths are not supported");
    if (bitList.front() != 8 && bitList.front() != 16)
        return refuse("Only 8-bit and 16-bit TIFF images are supported");
    layout.bytesPerSample = static_cast<std::uint32_t>(bitList.front() / 8U);
    if (const auto* format = find(SampleFormat)) {
        const auto list = values(reader, *format);
        if (!list || list->empty())
            return refuse(kDamaged);
        for (const auto kind : *list)
            if (kind != 1)
                return refuse("Floating-point and signed TIFF images are not supported");
    }

    const auto compression = scalar(Compression, 1, valid);
    switch (compression) {
    case 1:
        layout.codec = Codec::None;
        break;
    case 5:
        layout.codec = Codec::Lzw;
        break;
    case 8:
    case 32946:
        layout.codec = Codec::Deflate;
        break;
    case 32773:
        layout.codec = Codec::PackBits;
        break;
    default:
        return refuse("This TIFF compression type is not supported; save the image uncompressed "
                      "or with lossless compression");
    }
    const auto predictor = scalar(Predictor, 1, valid);
    if (predictor != 1 && predictor != 2)
        return refuse("This TIFF predictor is not supported");
    layout.predictor = predictor == 2;
    if (scalar(PlanarConfiguration, 1, valid) != 1)
        return refuse("Planar TIFF images are not supported; save the image with interleaved "
                      "channels");
    if (scalar(Orientation, 1, valid) != 1)
        return refuse("Rotated or mirrored TIFF images are not supported; save the image upright");
    if (scalar(FillOrder, 1, valid) != 1)
        return refuse("This TIFF bit order is not supported");
    if (!valid)
        return refuse(kDamaged);

    layout.tiled = find(TileWidth) != nullptr;
    const Entry* offsetEntry = nullptr;
    const Entry* countEntry = nullptr;
    if (layout.tiled) {
        const auto tileWidth = scalar(TileWidth, 0, valid);
        const auto tileLength = scalar(TileLength, 0, valid);
        if (!valid || tileWidth == 0 || tileLength == 0 || tileWidth > kMaxDimension ||
            tileLength > kMaxDimension)
            return refuse(kDamaged);
        layout.unitWidth = static_cast<std::uint32_t>(tileWidth);
        layout.unitHeight = static_cast<std::uint32_t>(tileLength);
        offsetEntry = find(TileOffsets);
        countEntry = find(TileByteCounts);
    } else {
        const auto rows = scalar(RowsPerStrip, height, valid);
        if (!valid || rows == 0)
            return refuse(kDamaged);
        layout.unitWidth = layout.width;
        layout.unitHeight = static_cast<std::uint32_t>(std::min<std::uint64_t>(rows, height));
        offsetEntry = find(StripOffsets);
        countEntry = find(StripByteCounts);
    }
    if (offsetEntry == nullptr)
        return refuse(kDamaged);
    auto offsets = values(reader, *offsetEntry);
    if (!offsets)
        return refuse(kDamaged);
    layout.offsets = std::move(*offsets);
    if (countEntry != nullptr) {
        auto counts = values(reader, *countEntry);
        if (!counts)
            return refuse(kDamaged);
        layout.counts = std::move(*counts);
    } else if (layout.offsets.size() == 1 && layout.codec == Codec::None) {
        layout.counts = {bytes.size() > layout.offsets[0] ? bytes.size() - layout.offsets[0] : 0};
    } else {
        return refuse(kDamaged);
    }
    const std::uint64_t across = (layout.width + layout.unitWidth - 1ULL) / layout.unitWidth;
    const std::uint64_t down = (layout.height + layout.unitHeight - 1ULL) / layout.unitHeight;
    if (layout.offsets.size() < across * down || layout.counts.size() < across * down)
        return refuse(kDamaged);
    return Parsed{std::move(layout), {}};
}

[[nodiscard]] std::uint64_t unitRawBytes(const Layout& layout) {
    return static_cast<std::uint64_t>(layout.unitWidth) * layout.unitHeight *
           layout.samplesPerPixel * layout.bytesPerSample;
}

// Variable-width Lempel-Ziv-Welch with the TIFF "early change" code-width rule, most significant
// bit first. Fills `out` exactly; false when the data ends or breaks before then.
[[nodiscard]] bool lzwDecode(const std::span<const std::byte> in, const std::span<std::byte> out) {
    std::array<std::uint16_t, 4096> prefix{};
    std::array<std::uint8_t, 4096> suffix{};
    std::array<std::uint32_t, 4096> length{};
    for (std::uint32_t code = 0; code < 256; ++code) {
        suffix[code] = static_cast<std::uint8_t>(code);
        length[code] = 1;
    }
    std::uint32_t bits = 9;
    std::uint32_t next = 258;
    std::int32_t previous = -1;
    std::uint64_t buffer = 0;
    std::uint32_t buffered = 0;
    std::size_t position = 0;
    std::size_t written = 0;
    const auto firstByte = [&](std::uint32_t code) {
        while (length[code] > 1)
            code = prefix[code];
        return suffix[code];
    };
    // Writes the string for `code`, cut at the end of `out` (the last string of a unit may be).
    const auto emit = [&](std::uint32_t code) {
        const auto size = length[code];
        auto at = written + size;
        while (true) {
            --at;
            if (at < out.size())
                out[at] = static_cast<std::byte>(suffix[code]);
            if (length[code] == 1)
                break;
            code = prefix[code];
        }
        written = std::min<std::size_t>(out.size(), written + size);
    };
    while (written < out.size()) {
        while (buffered < bits) {
            if (position >= in.size())
                return false;
            buffer = (buffer << 8U) | static_cast<unsigned char>(in[position++]);
            buffered += 8;
        }
        const auto code = static_cast<std::uint32_t>((buffer >> (buffered - bits)) &
                                                     ((std::uint64_t{1} << bits) - 1U));
        buffered -= bits;
        if (code == 257)
            return false; // end of data before the expected size
        if (code == 256) {
            bits = 9;
            next = 258;
            previous = -1;
            continue;
        }
        if (previous < 0) {
            if (code > 255)
                return false;
            emit(code);
            previous = static_cast<std::int32_t>(code);
            continue;
        }
        const auto before = static_cast<std::uint32_t>(previous);
        std::uint32_t added = 0;
        if (code < next) {
            added = firstByte(code);
            emit(code);
        } else if (code == next) {
            added = firstByte(before);
            emit(before);
            if (written < out.size())
                out[written++] = static_cast<std::byte>(added);
        } else {
            return false;
        }
        if (next < 4096) {
            prefix[next] = static_cast<std::uint16_t>(before);
            suffix[next] = static_cast<std::uint8_t>(added);
            length[next] = length[before] + 1U;
            ++next;
            if (next + 1U >= (1U << bits) && bits < 12)
                ++bits;
        }
        previous = static_cast<std::int32_t>(code);
    }
    return true;
}

[[nodiscard]] bool packBitsDecode(const std::span<const std::byte> in,
                                  const std::span<std::byte> out) {
    std::size_t position = 0;
    std::size_t written = 0;
    while (written < out.size()) {
        if (position >= in.size())
            return false;
        const auto control = static_cast<std::int8_t>(in[position++]);
        if (control >= 0) {
            const std::size_t run = static_cast<std::size_t>(control) + 1U;
            if (run > in.size() - position)
                return false;
            const auto take = std::min(run, out.size() - written);
            std::memcpy(out.data() + written, in.data() + position, take);
            position += run;
            written += take;
        } else if (control != -128) {
            const std::size_t run = static_cast<std::size_t>(1 - control);
            if (position >= in.size())
                return false;
            const auto value = in[position++];
            const auto take = std::min(run, out.size() - written);
            std::fill_n(out.data() + written, take, value);
            written += take;
        }
    }
    return true;
}

[[nodiscard]] bool deflateDecode(const std::span<const std::byte> in,
                                 const std::span<std::byte> out) {
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK)
        return false;
    std::size_t consumed = 0;
    std::size_t produced = 0;
    int status = Z_OK;
    while (produced < out.size() && status == Z_OK) {
        if (stream.avail_in == 0) {
            if (consumed >= in.size())
                break;
            const auto chunk = std::min<std::size_t>(in.size() - consumed,
                                                     std::numeric_limits<uInt>::max() / 2U);
            stream.next_in =
                reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data())) + consumed;
            stream.avail_in = static_cast<uInt>(chunk);
            consumed += chunk;
        }
        const auto room = std::min<std::size_t>(out.size() - produced,
                                                std::numeric_limits<uInt>::max() / 2U);
        stream.next_out = reinterpret_cast<Bytef*>(out.data()) + produced;
        stream.avail_out = static_cast<uInt>(room);
        status = inflate(&stream, Z_NO_FLUSH);
        produced += room - stream.avail_out;
    }
    inflateEnd(&stream);
    return produced == out.size() && (status == Z_OK || status == Z_STREAM_END);
}

[[nodiscard]] std::uint16_t sampleAt(const std::byte* data, const std::uint32_t bytesPerSample,
                                     const bool littleEndian) {
    if (bytesPerSample == 1)
        return static_cast<std::uint16_t>(static_cast<unsigned char>(data[0]) * 257U);
    const auto a = static_cast<unsigned char>(data[0]);
    const auto b = static_cast<unsigned char>(data[1]);
    return littleEndian ? static_cast<std::uint16_t>(a | (b << 8U))
                        : static_cast<std::uint16_t>((a << 8U) | b);
}
} // namespace

bool tiffInfo(const std::span<const std::byte> bytes, ImageInfo& info) {
    info.tiff = true;
    auto parsed = parse(bytes);
    if (!parsed.ok()) {
        info.refusal = std::move(parsed.refusal);
        return false;
    }
    const auto& layout = parsed.layout;
    info.width = static_cast<int>(layout.width);
    info.height = static_cast<int>(layout.height);
    info.sixteenBit = layout.bytesPerSample == 2;
    info.channels = (layout.grey ? 1 : 3) + (layout.hasAlpha ? 1 : 0);
    return true;
}

std::vector<std::uint16_t> tiffSamples(const std::span<const std::byte> bytes,
                                       const ImageInfo& info, const std::size_t pixelBudget) {
    const auto parsed = parse(bytes);
    if (!parsed.ok())
        return {};
    const auto& layout = parsed.layout;
    if (static_cast<int>(layout.width) != info.width ||
        static_cast<int>(layout.height) != info.height)
        return {};
    const std::uint64_t pixels = static_cast<std::uint64_t>(layout.width) * layout.height;
    // Final RGBA32F (16 bytes) and these RGBA16 samples (8 bytes) per pixel, plus one unit's
    // decompressed scratch, must fit the explicit budget before anything is allocated.
    std::uint64_t resident = 0;
    if (!checkedSizeProduct(pixels, 24U, resident))
        return {};
    const auto raw = unitRawBytes(layout);
    if (!decodeWorkingSetFits(resident, raw, 0U, pixelBudget))
        return {};
    const std::uint64_t across = (layout.width + layout.unitWidth - 1ULL) / layout.unitWidth;
    const std::uint64_t down = (layout.height + layout.unitHeight - 1ULL) / layout.unitHeight;
    std::vector<std::uint16_t> samples(static_cast<std::size_t>(pixels) * 4U);
    std::vector<std::byte> unit(static_cast<std::size_t>(raw));
    const std::size_t pixelBytes = static_cast<std::size_t>(layout.samplesPerPixel) *
                                   layout.bytesPerSample;
    const std::size_t rowBytes = static_cast<std::size_t>(layout.unitWidth) * pixelBytes;
    for (std::uint64_t row = 0; row < down; ++row) {
        for (std::uint64_t column = 0; column < across; ++column) {
            const auto index = static_cast<std::size_t>(row * across + column);
            const auto offset = layout.offsets[index];
            const auto count = layout.counts[index];
            if (offset > bytes.size() || count > bytes.size() - offset)
                return {};
            const auto input = bytes.subspan(static_cast<std::size_t>(offset),
                                             static_cast<std::size_t>(count));
            // A strip stores only its own rows; a tile always stores its full extent.
            const std::uint64_t y0 = row * layout.unitHeight;
            const std::uint32_t rows = layout.tiled
                                           ? layout.unitHeight
                                           : static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                                 layout.unitHeight, layout.height - y0));
            const auto expected = static_cast<std::size_t>(rows) * rowBytes;
            const auto out = std::span(unit).first(expected);
            bool decoded = false;
            switch (layout.codec) {
            case Codec::None:
                decoded = input.size() >= expected;
                if (decoded)
                    std::memcpy(out.data(), input.data(), expected);
                break;
            case Codec::Lzw:
                decoded = lzwDecode(input, out);
                break;
            case Codec::Deflate:
                decoded = deflateDecode(input, out);
                break;
            case Codec::PackBits:
                decoded = packBitsDecode(input, out);
                break;
            }
            if (!decoded)
                return {};
            if (layout.predictor) {
                for (std::uint32_t y = 0; y < rows; ++y) {
                    auto* line = out.data() + static_cast<std::size_t>(y) * rowBytes;
                    for (std::size_t at = pixelBytes; at < rowBytes; at += layout.bytesPerSample) {
                        if (layout.bytesPerSample == 1) {
                            line[at] = static_cast<std::byte>(
                                static_cast<unsigned char>(line[at]) +
                                static_cast<unsigned char>(line[at - pixelBytes]));
                        } else {
                            const auto left = sampleAt(line + at - pixelBytes, 2, layout.littleEndian);
                            const auto here = sampleAt(line + at, 2, layout.littleEndian);
                            const auto sum = static_cast<std::uint16_t>(left + here);
                            const auto low = static_cast<std::byte>(sum & 0xffU);
                            const auto high = static_cast<std::byte>(sum >> 8U);
                            line[at] = layout.littleEndian ? low : high;
                            line[at + 1] = layout.littleEndian ? high : low;
                        }
                    }
                }
            }
            const std::uint64_t x0 = column * layout.unitWidth;
            const auto width = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(layout.unitWidth, layout.width - x0));
            const auto visibleRows = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(rows, layout.height - y0));
            for (std::uint32_t y = 0; y < visibleRows; ++y) {
                const auto* line = out.data() + static_cast<std::size_t>(y) * rowBytes;
                auto* target = samples.data() +
                               (static_cast<std::size_t>(y0 + y) * layout.width + x0) * 4U;
                for (std::uint32_t x = 0; x < width; ++x, target += 4) {
                    const auto* pixel = line + static_cast<std::size_t>(x) * pixelBytes;
                    std::array<std::uint16_t, 4> rgba{};
                    const auto at = [&](const std::uint32_t channel) {
                        return sampleAt(pixel + static_cast<std::size_t>(channel) *
                                                    layout.bytesPerSample,
                                        layout.bytesPerSample, layout.littleEndian);
                    };
                    if (layout.grey) {
                        auto grey = at(0);
                        if (layout.whiteIsZero)
                            grey = static_cast<std::uint16_t>(65535U - grey);
                        rgba = {grey, grey, grey, 65535};
                        if (layout.hasAlpha)
                            rgba[3] = at(1);
                    } else {
                        rgba = {at(0), at(1), at(2), 65535};
                        if (layout.hasAlpha)
                            rgba[3] = at(3);
                    }
                    // Straight alpha is the stored form everywhere else; undo an associated
                    // (premultiplied) file's multiplication.
                    if (layout.hasAlpha && layout.associatedAlpha) {
                        for (std::size_t c = 0; c < 3; ++c)
                            rgba[c] = rgba[3] == 0
                                          ? std::uint16_t{0}
                                          : static_cast<std::uint16_t>(std::min<std::uint32_t>(
                                                65535U, (static_cast<std::uint32_t>(rgba[c]) *
                                                             65535U +
                                                         rgba[3] / 2U) /
                                                            rgba[3]));
                    }
                    std::copy(rgba.begin(), rgba.end(), target);
                }
            }
        }
    }
    return samples;
}
} // namespace bloom::media::detail
