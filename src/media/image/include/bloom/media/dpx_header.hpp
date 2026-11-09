#pragma once

#include <cstdint>
#include <string>

namespace bloom::media {
// The DPX file header fields that drive interpretation (SMPTE ST 268-1). The transfer and
// colorimetric codes are the standard's: 1 printing density, 2 linear, 3 logarithmic, 4
// unspecified video, 6 Rec.709 video, 13 ADX; 0 is user defined.
struct ImageDpxHeader final {
    bool bigEndian = false;
    std::uint8_t transfer = 0;
    std::uint8_t colorimetric = 0;
    std::uint8_t bitDepth = 0;
    // 0 packed, 1 filled method A, 2 filled method B.
    std::uint8_t packing = 0;
    std::uint8_t elementCount = 1;
    std::uint32_t dataOffset = 0;
    // Frames per second from the film or television header; 0 when the file records none.
    double frameRate = 0.0;
    // "HH:MM:SS:FF" from the television header; empty when the file records none.
    std::string timecode;
    friend bool operator==(const ImageDpxHeader&, const ImageDpxHeader&) = default;
};
} // namespace bloom::media
