#include <bloom/runtime/scope_picture.hpp>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace bloom::runtime::scopes {
namespace {

constexpr std::array<std::uint8_t, 8> kSignature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

void putU32(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24U));
    out.push_back(static_cast<std::uint8_t>(value >> 16U));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value));
}

void putChunk(std::vector<std::uint8_t>& out, const char (&type)[5], const std::uint8_t* data,
              const std::size_t size) {
    putU32(out, static_cast<std::uint32_t>(size));
    const auto start = out.size();
    out.insert(out.end(), type, type + 4);
    if (size != 0)
        out.insert(out.end(), data, data + size);
    const auto crc = crc32_z(0L, reinterpret_cast<const Bytef*>(out.data() + start),
                             static_cast<z_size_t>(out.size() - start));
    putU32(out, static_cast<std::uint32_t>(crc));
}

// Straight (not premultiplied) 8-bit from a premultiplied component.
[[nodiscard]] std::uint8_t straight(const unsigned component, const unsigned alpha) {
    if (alpha == 0U || alpha == 255U)
        return static_cast<std::uint8_t>(component);
    return static_cast<std::uint8_t>(std::min(255U, (component * 255U + alpha / 2U) / alpha));
}

} // namespace

PngResult encodeScopePng(const ScopeImage& image, const std::size_t byteLimit) {
    PngResult result;
    if (image.empty() || image.pixels.size() != static_cast<std::uint64_t>(image.width) *
                                                    static_cast<std::uint64_t>(image.height)) {
        result.error = PngError::InvalidImage;
        return result;
    }
    const auto width = static_cast<std::uint64_t>(image.width);
    const auto height = static_cast<std::uint64_t>(image.height);
    const std::uint64_t rowBytes = 1U + width * 4U;
    const std::uint64_t rawBytes = rowBytes * height;
    // The encoded size is not known before compressing, but it can never be smaller than the
    // headers; a raw size zlib's 32-bit lengths cannot hold is refused as over any limit.
    if (rawBytes > std::numeric_limits<uInt>::max() / 2U) {
        result.error = PngError::ByteLimitExceeded;
        return result;
    }
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(rawBytes));
    std::size_t at = 0;
    for (int y = 0; y < image.height; ++y) {
        raw[at++] = 0; // filter type None
        for (int x = 0; x < image.width; ++x) {
            const auto pixel = image.pixel(x, y);
            const auto alpha = alphaOf(pixel);
            raw[at++] = straight(redOf(pixel), alpha);
            raw[at++] = straight(greenOf(pixel), alpha);
            raw[at++] = straight(blueOf(pixel), alpha);
            raw[at++] = static_cast<std::uint8_t>(alpha);
        }
    }
    uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> compressed(static_cast<std::size_t>(compressedSize));
    if (compress2(compressed.data(), &compressedSize, raw.data(), static_cast<uLong>(raw.size()),
                  6) != Z_OK) {
        result.error = PngError::CompressorFailure;
        return result;
    }
    constexpr std::size_t overhead = 8 + (12 + 13) + (12 + 1) + 12 + 12;
    if (static_cast<std::uint64_t>(compressedSize) + overhead > byteLimit) {
        result.error = PngError::ByteLimitExceeded;
        return result;
    }
    auto& out = result.bytes;
    out.reserve(static_cast<std::size_t>(compressedSize) + overhead);
    out.insert(out.end(), kSignature.begin(), kSignature.end());
    std::vector<std::uint8_t> header;
    putU32(header, static_cast<std::uint32_t>(image.width));
    putU32(header, static_cast<std::uint32_t>(image.height));
    header.push_back(8); // bit depth
    header.push_back(6); // color type: RGBA
    header.push_back(0); // compression
    header.push_back(0); // filter
    header.push_back(0); // interlace
    putChunk(out, "IHDR", header.data(), header.size());
    const std::uint8_t intent = 0; // perceptual
    putChunk(out, "sRGB", &intent, 1);
    putChunk(out, "IDAT", compressed.data(), static_cast<std::size_t>(compressedSize));
    putChunk(out, "IEND", nullptr, 0);
    return result;
}

} // namespace bloom::runtime::scopes
