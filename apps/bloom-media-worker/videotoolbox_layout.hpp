#pragma once

// Decoded planes to the worker protocol's frame product, for the VideoToolbox reader.
//
// A frame crosses the worker boundary as one of two layouts: 8-bit 4:2:0 planes, or 16-bit 4:4:4
// planes with alpha. The FFmpeg worker fills the second with libswscale (bilinear, accurate
// rounding, bit-exact, the source's chroma siting, no matrix or range change). H.264 and HEVC
// decoding is fixed to the bit by their standards, so the VideoToolbox reader's samples equal the
// FFmpeg decoder's; for the product to be equal as well, the same conversion has to run on them.
// This is that conversion: the same library call with the same settings, on planes laid out the
// way the FFmpeg decoder lays them out.
//
// The library is the bundled FFmpeg's libswscale. A build without the FFmpeg package has no such
// conversion: layoutConversionAvailable() is false and only the 8-bit 4:2:0 layout, which is a
// copy, can be produced.

#include <bloom/media/provider/protocol.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <variant>

namespace bloom::media::videotoolbox {

enum class ChromaSampling : std::uint8_t { S420, S422, S444 };

// Planar samples as the FFmpeg decoder of the same stream would hand them over: one plane per
// component, 8-bit samples as bytes and deeper ones as little-endian 16-bit words holding `depth`
// significant bits in the low bits.
struct PlanarFrame final {
    ChromaSampling sampling = ChromaSampling::S420;
    // 8, 10 or 16.
    unsigned depth = 8;
    bool alpha = false;
    bool fullRange = false;
    // H.273 chroma sample location type plus one, the numbering FFmpeg uses: 0 unspecified,
    // 1 left, 2 center, 3 top left, 4 top, 5 bottom left, 6 bottom.
    int chromaLocation = 0;
    std::uint32_t width = 0, height = 0;
    // Y, Cb, Cr and, with `alpha`, A.
    std::array<const std::uint8_t*, 4> data{};
    // Bytes per row of each plane.
    std::array<int, 4> stride{};
};

[[nodiscard]] bool layoutConversionAvailable() noexcept;

// The FFmpeg pixel format (an AVPixelFormat value) of planes laid out as `frame`, or -1 when
// there is none or the build has no FFmpeg package.
[[nodiscard]] int ffmpegPixelFormat(const PlanarFrame& frame) noexcept;

// Keeps the conversion's state between frames of one stream.
class LayoutConverter final {
  public:
    LayoutConverter();
    ~LayoutConverter();
    LayoutConverter(const LayoutConverter&) = delete;
    LayoutConverter& operator=(const LayoutConverter&) = delete;

    // Fills `product`'s format and planes from `frame`. Left-sited (or unsited) limited-range
    // 8-bit 4:2:0 is copied as it is; everything else becomes 16-bit 4:4:4 with alpha. The error
    // is Unavailable when the layout is not one this build converts.
    [[nodiscard]] provider::Result<std::monostate> convert(const PlanarFrame& frame,
                                                           provider::FrameProduct& product);

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace bloom::media::videotoolbox
