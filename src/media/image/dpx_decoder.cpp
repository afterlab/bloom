#include "dpx_decoder.hpp"
#include "image_budget.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace bloom::media::detail {
namespace {
constexpr std::uint64_t kMaxDimension = std::uint64_t{1} << 24;
constexpr std::size_t kImageInfoOffset = 768;
constexpr std::size_t kElementOffset = 780;
constexpr std::size_t kElementSize = 72;
constexpr std::size_t kMaxElements = 8;
constexpr std::size_t kFrameRateOffset = 1724;       // motion-picture film header
constexpr std::size_t kTimecodeOffset = 1920;        // television header
constexpr std::size_t kTelevisionRateOffset = 1940;  // television header temporal rate
constexpr auto kDamaged = "The DPX file is damaged or truncated";

// Destination channels: 0 R, 1 G, 2 B, 3 A, 4 luma (stands for R, G and B).
constexpr std::uint8_t kLuma = 4;

struct Element {
    std::vector<std::uint8_t> channels;
    std::uint32_t bitDepth = 8;
    std::uint16_t packing = 0;
    std::uint64_t offset = 0;
    std::uint64_t pitch = 0;
};

struct Layout {
    ImageDpxHeader header;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool luma = false;
    bool alpha = false;
    std::vector<Element> elements;
};

struct Parsed {
    Layout layout;
    std::string refusal;
};

Parsed refuse(std::string reason) {
    Parsed parsed;
    parsed.refusal = std::move(reason);
    return parsed;
}

struct Reader {
    std::span<const std::byte> bytes;
    bool bigEndian = false;
    [[nodiscard]] std::optional<std::uint64_t> read(const std::uint64_t at,
                                                    const std::size_t size) const {
        if (at > bytes.size() || size > bytes.size() - at)
            return std::nullopt;
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < size; ++index) {
            const auto byte = static_cast<std::uint64_t>(
                static_cast<unsigned char>(bytes[static_cast<std::size_t>(at) + index]));
            value |= bigEndian ? byte << (8U * (size - 1U - index)) : byte << (8U * index);
        }
        return value;
    }
    [[nodiscard]] std::optional<float> real(const std::uint64_t at) const {
        const auto bits = read(at, 4);
        if (!bits)
            return std::nullopt;
        return std::bit_cast<float>(static_cast<std::uint32_t>(*bits));
    }
};

[[nodiscard]] std::uint64_t alignUp4(const std::uint64_t value) { return (value + 3U) & ~std::uint64_t{3}; }

// "HH:MM:SS:FF" from a binary-coded-decimal time code word, empty when the word is unset or invalid.
// The top bit of each tens digit carries a flag (drop frame, colour frame, field), not time.
[[nodiscard]] std::string timecodeText(const std::uint64_t word) {
    if (word == 0xffffffffU || word == 0)
        return {};
    std::string text;
    for (int pair = 3; pair >= 0; --pair) {
        const auto tens = (word >> (8 * pair + 4)) & 0x7U;
        const auto ones = (word >> (8 * pair)) & 0xfU;
        if (tens > 9 || ones > 9)
            return {};
        if (!text.empty())
            text.push_back(':');
        text.push_back(static_cast<char>('0' + tens));
        text.push_back(static_cast<char>('0' + ones));
    }
    return text;
}

Parsed parse(const std::span<const std::byte> bytes) {
    if (bytes.size() < kElementOffset + kElementSize || !isDpxMagic(bytes))
        return refuse(kDamaged);
    Reader reader{bytes, static_cast<unsigned char>(bytes[0]) == 'S'};
    Layout layout;
    layout.header.bigEndian = reader.bigEndian;
    const auto fileOffset = reader.read(4, 4);
    const auto orientation = reader.read(kImageInfoOffset, 2);
    const auto elementCount = reader.read(kImageInfoOffset + 2, 2);
    const auto width = reader.read(kImageInfoOffset + 4, 4);
    const auto height = reader.read(kImageInfoOffset + 8, 4);
    if (!fileOffset || !orientation || !elementCount || !width || !height)
        return refuse(kDamaged);
    if (*width == 0 || *height == 0 || *width > kMaxDimension || *height > kMaxDimension)
        return refuse("The DPX image has invalid dimensions");
    if (*orientation != 0 && *orientation != 0xffffU)
        return refuse("Rotated or mirrored DPX images are not supported; save the image upright");
    if (*elementCount < 1 || *elementCount > kMaxElements)
        return refuse("The DPX file has an invalid number of image elements");
    layout.width = static_cast<std::uint32_t>(*width);
    layout.height = static_cast<std::uint32_t>(*height);
    layout.header.elementCount = static_cast<std::uint8_t>(*elementCount);

    std::array<bool, 5> seen{};
    for (std::size_t index = 0; index < *elementCount; ++index) {
        const std::uint64_t at = kElementOffset + index * kElementSize;
        const auto sign = reader.read(at, 4);
        const auto descriptor = reader.read(at + 20, 1);
        const auto transfer = reader.read(at + 21, 1);
        const auto colorimetric = reader.read(at + 22, 1);
        const auto depth = reader.read(at + 23, 1);
        const auto packing = reader.read(at + 24, 2);
        const auto encoding = reader.read(at + 26, 2);
        const auto offset = reader.read(at + 28, 4);
        const auto eol = reader.read(at + 32, 4);
        if (!sign || !descriptor || !transfer || !colorimetric || !depth || !packing || !encoding ||
            !offset || !eol)
            return refuse(kDamaged);
        Element element;
        switch (*descriptor) {
        case 1: element.channels = {0}; break;
        case 2: element.channels = {1}; break;
        case 3: element.channels = {2}; break;
        case 4: element.channels = {3}; break;
        case 6: element.channels = {kLuma}; break;
        case 50: element.channels = {0, 1, 2}; break;
        case 51: element.channels = {0, 1, 2, 3}; break;
        case 52: element.channels = {3, 2, 1, 0}; break;
        default:
            return refuse("This DPX image element layout is not supported; only RGB, RGBA, "
                          "luma and single-channel elements can be read");
        }
        if (*sign != 0)
            return refuse("Signed DPX samples are not supported");
        if (*encoding != 0)
            return refuse("Run-length encoded DPX images are not supported");
        if (*depth != 8 && *depth != 10 && *depth != 12 && *depth != 16)
            return refuse("Only 8-, 10-, 12- and 16-bit DPX images are supported");
        if (*packing > 2)
            return refuse("This DPX sample packing is not supported");
        if ((*depth == 10 || *depth == 12) && *packing == 0)
            return refuse("Packed 10-bit and 12-bit DPX samples are not supported; save the image "
                          "with filled (padded) samples");
        element.bitDepth = static_cast<std::uint32_t>(*depth);
        element.packing = static_cast<std::uint16_t>(*packing);
        element.offset = (*offset == 0 || *offset == 0xffffffffU) && index == 0 ? *fileOffset : *offset;
        if (element.offset == 0 || element.offset == 0xffffffffU)
            return refuse(kDamaged);
        for (const auto channel : element.channels) {
            if (seen[channel])
                return refuse("The DPX file names the same channel in more than one image element");
            seen[channel] = true;
        }
        const std::uint64_t samples = layout.width * element.channels.size();
        std::uint64_t lineBytes = 0;
        switch (element.bitDepth) {
        case 8: lineBytes = samples; break;
        case 10: lineBytes = (samples + 2U) / 3U * 4U; break;
        default: lineBytes = samples * 2U; break;
        }
        element.pitch = alignUp4(lineBytes) + *eol;
        // The element's rows (the last without its end-of-line padding) must lie inside the file.
        const std::uint64_t total = element.pitch * (layout.height - 1ULL) + alignUp4(lineBytes);
        if (element.offset > bytes.size() || total > bytes.size() - element.offset)
            return refuse("The DPX image data is truncated");
        if (index == 0) {
            layout.header.transfer = static_cast<std::uint8_t>(*transfer);
            layout.header.colorimetric = static_cast<std::uint8_t>(*colorimetric);
            layout.header.bitDepth = static_cast<std::uint8_t>(*depth);
            layout.header.packing = static_cast<std::uint8_t>(*packing);
            layout.header.dataOffset = static_cast<std::uint32_t>(element.offset);
        } else {
            layout.header.bitDepth = std::max(layout.header.bitDepth, static_cast<std::uint8_t>(*depth));
        }
        layout.elements.push_back(std::move(element));
    }
    layout.luma = seen[kLuma];
    layout.alpha = seen[3];
    if (layout.luma ? (seen[0] || seen[1] || seen[2]) : !(seen[0] && seen[1] && seen[2]))
        return refuse("The DPX image elements do not form a complete color image");

    // Film and television headers (optional; the generic header is 1664 bytes in most files).
    if (const auto rate = reader.real(kFrameRateOffset); rate && std::isfinite(*rate) && *rate > 0.0F && *rate < 10000.0F)
        layout.header.frameRate = static_cast<double>(*rate);
    else if (const auto tvRate = reader.real(kTelevisionRateOffset);
             tvRate && std::isfinite(*tvRate) && *tvRate > 0.0F && *tvRate < 10000.0F)
        layout.header.frameRate = static_cast<double>(*tvRate);
    if (const auto timecode = reader.read(kTimecodeOffset, 4))
        layout.header.timecode = timecodeText(*timecode);
    return Parsed{std::move(layout), {}};
}

[[nodiscard]] std::uint16_t scale(const std::uint32_t code, const std::uint32_t depth) {
    switch (depth) {
    case 8: return static_cast<std::uint16_t>(code * 257U);
    case 10: return static_cast<std::uint16_t>((code * 65535U + 511U) / 1023U);
    case 12: return static_cast<std::uint16_t>((code * 65535U + 2047U) / 4095U);
    default: return static_cast<std::uint16_t>(code);
    }
}
} // namespace

bool dpxInfo(const std::span<const std::byte> bytes, ImageInfo& info) {
    auto parsed = parse(bytes);
    if (!parsed.refusal.empty()) {
        info.refusal = std::move(parsed.refusal);
        return false;
    }
    const auto& layout = parsed.layout;
    info.width = static_cast<int>(layout.width);
    info.height = static_cast<int>(layout.height);
    info.sixteenBit = layout.header.bitDepth > 8;
    info.channels = (layout.luma ? 1 : 3) + (layout.alpha ? 1 : 0);
    info.dpx = layout.header;
    return true;
}

std::vector<std::uint16_t> dpxSamples(const std::span<const std::byte> bytes, const ImageInfo& info,
                                      const std::size_t pixelBudget) {
    const auto parsed = parse(bytes);
    if (!parsed.refusal.empty())
        return {};
    const auto& layout = parsed.layout;
    if (static_cast<int>(layout.width) != info.width || static_cast<int>(layout.height) != info.height)
        return {};
    const std::uint64_t pixels = static_cast<std::uint64_t>(layout.width) * layout.height;
    std::uint64_t resident = 0;
    if (!checkedSizeProduct(pixels, 24U, resident) ||
        !decodeWorkingSetFits(resident, 0U, 0U, pixelBudget))
        return {};
    std::vector<std::uint16_t> samples(static_cast<std::size_t>(pixels) * 4U, 0);
    for (std::size_t at = 3; at < samples.size(); at += 4)
        samples[at] = 65535;
    const auto big = layout.header.bigEndian;
    const auto word = [&](const std::uint64_t at, const std::size_t size) {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < size; ++index) {
            const auto byte = static_cast<std::uint32_t>(
                static_cast<unsigned char>(bytes[static_cast<std::size_t>(at) + index]));
            value |= big ? byte << (8U * (size - 1U - index)) : byte << (8U * index);
        }
        return value;
    };
    for (const auto& element : layout.elements) {
        const std::size_t channelCount = element.channels.size();
        for (std::uint32_t y = 0; y < layout.height; ++y) {
            const std::uint64_t line = element.offset + element.pitch * y;
            auto* row = samples.data() + static_cast<std::size_t>(y) * layout.width * 4U;
            for (std::uint32_t x = 0; x < layout.width; ++x) {
                for (std::size_t c = 0; c < channelCount; ++c) {
                    const std::uint64_t index = static_cast<std::uint64_t>(x) * channelCount + c;
                    std::uint32_t code = 0;
                    switch (element.bitDepth) {
                    case 8:
                        code = static_cast<unsigned char>(bytes[static_cast<std::size_t>(line + index)]);
                        break;
                    case 10: {
                        const auto packed = word(line + index / 3U * 4U, 4);
                        const auto slot = static_cast<unsigned>(index % 3U);
                        const unsigned shift = element.packing == 1 ? 22U - 10U * slot : 20U - 10U * slot;
                        code = (packed >> shift) & 0x3ffU;
                        break;
                    }
                    case 12: {
                        const auto value = word(line + index * 2U, 2);
                        code = element.packing == 1 ? value >> 4U : value & 0xfffU;
                        break;
                    }
                    default:
                        code = word(line + index * 2U, 2);
                        break;
                    }
                    const auto value = scale(code, element.bitDepth);
                    auto* pixel = row + static_cast<std::size_t>(x) * 4U;
                    const auto channel = element.channels[c];
                    if (channel == kLuma)
                        pixel[0] = pixel[1] = pixel[2] = value;
                    else
                        pixel[channel] = value;
                }
            }
        }
    }
    return samples;
}
} // namespace bloom::media::detail
