#pragma once

// The macOS read provider: AVFoundation describes a QuickTime or MPEG-4 file and VideoToolbox
// decodes it (ADR 0035; issue #371).
//
// What a Reader answers, and how:
//
//   Probe        The file's tracks in file order, from AVFoundation: codec, profile, extent, rate,
//                frame count, sample layout, the color tags the file carries, start timecode.
//   DemuxIndex   The video sync samples, from the tracks' sample tables.
//   VideoDecode  Frame N in presentation order, exactly: the sample table gives the Nth
//                presentation time, the samples from the preceding sync sample are read and decoded
//                by VideoToolbox into its native sample layout (no conversion to 8 bits, 4:2:0 or
//                RGB), and the planes cross through the same layout conversion the FFmpeg worker
//                uses (videotoolbox_layout.hpp). Bloom's own color conversion does the matrix and
//                range afterwards, as for any other frame.
//   AudioDecode  `samples` sample frames starting at sample frame `frame`, decoded to float by
//                AVFoundation, however many buffers that takes.
//
// The source is the host's captured snapshot when the request carries one (an inherited
// descriptor, opened by its descriptor path and never by the file's own path), and the request's
// path otherwise.
//
// A file the reader will not read the way the FFmpeg worker would is refused whole, at Probe, with
// the reason, and the host's provider chain hands that source to the FFmpeg worker. That covers
// containers other than QuickTime and MPEG-4, codecs other than H.264, HEVC and ProRes, interlaced,
// rotated, mirrored and anamorphic pictures, variable cadence, edits other than one leading trim,
// more than two audio channels, and a snapshot on a system older than macOS 14 (which cannot name
// a descriptor's media type).

#include "videotoolbox_layout.hpp"

#include <bloom/media/provider/protocol.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace bloom::media::videotoolbox {

// One decoded picture as the decoder returned it, before the layout conversion: `frame` points
// into `storage`.
struct DecodedPlanes final {
    PlanarFrame frame;
    std::array<std::vector<std::uint8_t>, 4> storage;
    provider::Rational pts;
    provider::ColourTags colour;
};

class Reader final {
  public:
    Reader();
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // One read call. The opened source, its sample tables and the decoder are kept between calls
    // for the same source, so a retained worker continues where the previous frame ended.
    [[nodiscard]] provider::Payload call(const provider::CallRequest& request);

    // A VideoDecode request answered with the decoder's own planes. The export check compares
    // them in the sampling the stream declares.
    [[nodiscard]] provider::Result<DecodedPlanes> planes(const provider::CallRequest& request);

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace bloom::media::videotoolbox
