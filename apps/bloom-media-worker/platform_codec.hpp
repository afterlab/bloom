#pragma once
// The platform media framework's decoders and encoders behind the FFmpeg worker's demuxer and
// muxer (ADR 0035; issue #370). Worker-private: FFmpeg types never leave the media worker.
//
// On Windows the worker has a Media Foundation generation. libavformat reads and writes the
// container exactly as it does for the software generation, so a file is described, indexed and
// timed the same way whichever generation serves it; only the coding of the pictures goes to the
// system's transforms:
//
//   decode  A stream's packets, in the order the demuxer returns them, go to the system's H.264
//           or HEVC decoder, and its pictures come back as the planar frames the software decoder
//           of the same stream returns. Both codecs are decoded to the bit by their standards, so
//           the frames are equal, and the worker proves it on the first frame of every source.
//           The system decoder runs on the graphics device (Direct3D 11) when it can and on the
//           processor otherwise; a device whose first frame is not the software decoder's is
//           left for the rest of the source.
//   encode  Frames in the encoder's own sampling go to the system's H.264 or HEVC encoder, and
//           its packets go to libavformat's muxer.
//
// Every function here throws provider::Unavailable with the reason when the platform does not
// take the stream; the provider chain then hands the request to the software generation.
#include <bloom/media/provider/protocol.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
}

namespace bloom::media::ffmpeg {

class PlatformDecoder {
  public:
    enum class Step : std::uint8_t {
        // `frame` holds the next picture in presentation order.
        Frame,
        // The decoder wants the stream's next packet.
        NeedsPacket,
        // Every picture of an ended stream has been returned.
        End
    };
    virtual ~PlatformDecoder() = default;
    // One packet of the stream as the demuxer returned it; null ends the stream.
    virtual void send(const AVPacket* packet) = 0;
    // Fills `frame` with planar 4:2:0 samples (8-bit, or 10-bit little-endian words), the
    // packet's presentation time and the stream's color description.
    [[nodiscard]] virtual Step receive(AVFrame& frame) = 0;
    // Forgets everything sent so far: the next packet is a random-access point.
    virtual void flush() = 0;
    // Whether the pictures are decoded on the graphics device, and the decoder's own account of
    // how it runs ("Direct3D 11 hardware decode", or "software decode (" and the reason the
    // device is not used).
    [[nodiscard]] virtual bool hardware() const = 0;
    [[nodiscard]] virtual const std::string& note() const = 0;
};

// The system decoder for `stream`. Refused: a codec, sample layout or picture structure the
// system decoder does not return exactly as the software decoder does, and a computer without
// the decoder (HEVC is a separate component of Windows). With `hardware` the decoder runs on the
// graphics device (Direct3D 11) when the computer and the system decoder take it, and on the
// processor otherwise, saying why in note(); the choice never refuses a stream.
[[nodiscard]] std::unique_ptr<PlatformDecoder> platformDecoder(const AVStream& stream,
                                                               bool hardware);

struct PlatformEncoding final {
    AVCodecID codec = AV_CODEC_ID_NONE; // H.264 or HEVC
    int width = 0, height = 0;
    // Frames per second.
    AVRational rate{0, 1};
    // The frames the encoder is given: AV_PIX_FMT_YUV420P, or AV_PIX_FMT_YUV420P10LE for HEVC
    // Main 10.
    AVPixelFormat format = AV_PIX_FMT_NONE;
    std::int64_t bitRate = 0;
    // Frames between random-access points.
    int interval = 1;
    // H.273 code points, and FFmpeg's range numbering (1 limited, 2 full).
    provider::ColourTags colour;
};

class PlatformEncoder {
  public:
    virtual ~PlatformEncoder() = default;
    // The next frame (`frame->pts` counts frames from zero); null ends the stream.
    virtual void send(const AVFrame* frame) = 0;
    // Fills `packet` with the next coded picture, its times in frames; false when the encoder
    // has none ready (after the end of the stream: none left).
    [[nodiscard]] virtual bool receive(AVPacket& packet) = 0;
    // The stream's parameter sets when the encoder states them ahead of the first picture; empty
    // when they travel in the stream only.
    [[nodiscard]] virtual std::span<const std::uint8_t> parameterSets() const = 0;
    // The system's own name of the encoder that runs, and whether it is a hardware one.
    [[nodiscard]] virtual const std::string& name() const = 0;
    [[nodiscard]] virtual bool hardware() const = 0;
};

// A system encoder for `encoding`: a hardware one when the computer has it, a software one
// otherwise. Refused: a codec, profile or color description no installed encoder takes.
[[nodiscard]] std::unique_ptr<PlatformEncoder> platformEncoder(const PlatformEncoding& encoding);

} // namespace bloom::media::ffmpeg
