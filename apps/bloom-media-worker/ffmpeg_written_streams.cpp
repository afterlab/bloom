#include "ffmpeg_written_streams.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/rational.h>
}

namespace bloom::media::ffmpeg {
namespace {

using provider::Error;
using provider::Unavailable;

[[noreturn]] void refuse(const std::string& detail) { throw Unavailable{Error::Corrupt, detail}; }

provider::Rational reduced(const AVRational value) {
    if (value.num <= 0 || value.den <= 0)
        return {0, 1};
    int numerator = 0;
    int denominator = 1;
    av_reduce(&numerator, &denominator, value.num, value.den, INT32_MAX);
    return {numerator, denominator};
}

// Bits per second: the bytes over the duration (numerator / denominator seconds), to the nearest.
std::uint64_t bitrate(const std::uint64_t bytes, const provider::Rational duration) {
    if (duration.numerator <= 0 || duration.denominator <= 0)
        return 0;
    const double bits = static_cast<double>(bytes) * 8.0 * static_cast<double>(duration.denominator);
    return static_cast<std::uint64_t>(std::llround(bits / static_cast<double>(duration.numerator)));
}

} // namespace

std::vector<provider::EncodedStreamV1> readWrittenStreams(AVFormatContext& opened,
                                                          const provider::Rational videoDuration,
                                                          const provider::Rational audioDuration) {
    std::vector<std::uint64_t> bytes(opened.nb_streams, 0);
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet(av_packet_alloc(), [](AVPacket* value) {
        av_packet_free(&value);
    });
    if (!packet)
        refuse("Reopen could not read the written streams");
    int status = 0;
    while ((status = av_read_frame(&opened, packet.get())) >= 0) {
        if (packet->stream_index >= 0 &&
            static_cast<unsigned>(packet->stream_index) < opened.nb_streams && packet->size > 0)
            bytes[static_cast<std::size_t>(packet->stream_index)] +=
                static_cast<std::uint64_t>(packet->size);
        av_packet_unref(packet.get());
    }
    if (status != AVERROR_EOF)
        refuse("Reopen could not read the written streams");
    std::vector<provider::EncodedStreamV1> streams;
    for (unsigned index = 0; index < opened.nb_streams; ++index) {
        const auto& parameters = *opened.streams[index]->codecpar;
        const bool video = parameters.codec_type == AVMEDIA_TYPE_VIDEO;
        if (!video && parameters.codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (streams.size() == provider::kEncodedStreamsMaximum)
            refuse("Reopen found more picture and sound streams than were written");
        provider::EncodedStreamV1 stream;
        stream.kind = video ? provider::MediaKind::Video : provider::MediaKind::Audio;
        stream.codec = avcodec_get_name(parameters.codec_id);
        if (const auto* name = avcodec_profile_name(parameters.codec_id, parameters.profile))
            stream.profile = name;
        if (video) {
            stream.width = static_cast<std::uint32_t>(std::max(parameters.width, 0));
            stream.height = static_cast<std::uint32_t>(std::max(parameters.height, 0));
            const auto& source = *opened.streams[index];
            stream.rate = reduced(source.avg_frame_rate.num > 0 ? source.avg_frame_rate
                                                                : source.r_frame_rate);
        } else {
            stream.channels = static_cast<std::uint32_t>(std::max(parameters.ch_layout.nb_channels, 0));
            stream.sampleRate = static_cast<std::uint32_t>(std::max(parameters.sample_rate, 0));
        }
        stream.bitrate = bitrate(bytes[index], video ? videoDuration : audioDuration);
        streams.push_back(std::move(stream));
    }
    return streams;
}

std::vector<provider::EncodedStreamV1> readWrittenStreams(const std::string& path,
                                                          const provider::Rational videoDuration,
                                                          const provider::Rational audioDuration) {
    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, path.c_str(), nullptr, nullptr) < 0)
        refuse("Reopen could not read the written streams");
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> owned(
        input, [](AVFormatContext* value) { avformat_close_input(&value); });
    if (avformat_find_stream_info(input, nullptr) < 0)
        refuse("Reopen could not read the written streams");
    return readWrittenStreams(*input, videoDuration, audioDuration);
}

} // namespace bloom::media::ffmpeg
