#include "ffmpeg_stream_color_tags.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}
namespace bloom::media::ffmpeg {
namespace {
struct Tags final {
    int primaries = AVCOL_PRI_UNSPECIFIED;
    int transfer = AVCOL_TRC_UNSPECIFIED;
    int matrix = AVCOL_SPC_UNSPECIFIED;
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
};
// Code point 0 is reserved and 2 is unspecified; both say nothing.
int stated(const int code) { return code == 0 ? 2 : code; }
const char* rangeName(const AVColorRange range) {
    return range == AVCOL_RANGE_JPEG ? "full" : range == AVCOL_RANGE_MPEG ? "limited" : "unstated";
}
std::string describe(const Tags& tags) {
    return "primaries=" + std::to_string(stated(tags.primaries)) +
           " transfer=" + std::to_string(stated(tags.transfer)) +
           " matrix=" + std::to_string(stated(tags.matrix)) + " range=" + rangeName(tags.range);
}
struct Decoding final {
    AVCodecContext* context = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    Decoding() = default;
    Decoding(const Decoding&) = delete;
    Decoding& operator=(const Decoding&) = delete;
    ~Decoding() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&context);
    }
};
} // namespace

std::string h264StreamTagFailure(AVFormatContext& opened, const unsigned stream,
                                 const AVColorRange requestedRange) {
    constexpr const char* kUnreadable = "The H.264 stream's color tags could not be read";
    if (stream >= opened.nb_streams || opened.streams[stream]->codecpar == nullptr ||
        opened.streams[stream]->codecpar->codec_id != AV_CODEC_ID_H264)
        return kUnreadable;
    const auto& parameters = *opened.streams[stream]->codecpar;
    // Taken before a packet is read: these are the container's.
    const Tags container{parameters.color_primaries, parameters.color_trc, parameters.color_space,
                         parameters.color_range};

    Decoding decoding;
    const auto* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    decoding.context = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
    if (decoding.context == nullptr || decoding.packet == nullptr || decoding.frame == nullptr)
        return kUnreadable;
    // The parameter sets and nothing else: no tag of the container reaches the decoder.
    if (parameters.extradata != nullptr && parameters.extradata_size > 0) {
        const auto size = static_cast<std::size_t>(parameters.extradata_size);
        decoding.context->extradata =
            static_cast<std::uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (decoding.context->extradata == nullptr)
            return kUnreadable;
        std::memcpy(decoding.context->extradata, parameters.extradata, size);
        decoding.context->extradata_size = parameters.extradata_size;
    }
    decoding.context->thread_count = 1;
    if (avcodec_open2(decoding.context, codec, nullptr) < 0)
        return kUnreadable;
    bool decoded = false;
    while (!decoded && av_read_frame(&opened, decoding.packet) >= 0) {
        if (decoding.packet->stream_index == static_cast<int>(stream)) {
            static_cast<void>(avcodec_send_packet(decoding.context, decoding.packet));
            decoded = avcodec_receive_frame(decoding.context, decoding.frame) >= 0;
        }
        av_packet_unref(decoding.packet);
    }
    if (!decoded) {
        static_cast<void>(avcodec_send_packet(decoding.context, nullptr));
        decoded = avcodec_receive_frame(decoding.context, decoding.frame) >= 0;
    }
    if (!decoded)
        return kUnreadable;
    const Tags inStream{
        decoding.frame->color_primaries, decoding.frame->color_trc, decoding.frame->colorspace,
        // No video_full_range_flag in the stream means limited range.
        decoding.frame->color_range == AVCOL_RANGE_JPEG ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG};
    const auto requested = requestedRange == AVCOL_RANGE_JPEG ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    const bool same =
        stated(inStream.primaries) == stated(container.primaries) &&
        stated(inStream.transfer) == stated(container.transfer) &&
        stated(inStream.matrix) == stated(container.matrix) && inStream.range == requested &&
        (container.range == AVCOL_RANGE_UNSPECIFIED || container.range == inStream.range);
    if (same)
        return {};
    return "The H.264 stream's color tags differ from the container's: stream " +
           describe(inStream) + ", container " + describe(container) + ", requested range " +
           rangeName(requested);
}

std::string h264StreamTagFailure(const std::string& path, const unsigned stream,
                                 const AVColorRange requestedRange) {
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0 || format == nullptr)
        return "The H.264 stream's color tags could not be read";
    auto failure = h264StreamTagFailure(*format, stream, requestedRange);
    avformat_close_input(&format);
    return failure;
}
} // namespace bloom::media::ffmpeg
