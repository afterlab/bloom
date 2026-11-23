#include "ffmpeg_audio_loudness.hpp"

#include <bloom/media/audio/loudness.hpp>

#include <algorithm>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

namespace bloom::media::ffmpeg {

struct WrittenAudioMeter::State final {
    unsigned channels = 0;
    audio::loudness::Meter meter;
    std::vector<float> interleaved;
    bool failed = false;
    State(const unsigned count, const unsigned rate) : channels(count), meter(rate, count) {}
};

WrittenAudioMeter::WrittenAudioMeter(const unsigned channels, const unsigned sampleRate)
    : state_(std::make_unique<State>(channels, sampleRate)) {}

WrittenAudioMeter::~WrittenAudioMeter() = default;

bool WrittenAudioMeter::push(const AVFrame& frame, const std::uint64_t count) {
    auto& s = *state_;
    const auto format = static_cast<AVSampleFormat>(frame.format);
    if (s.failed || !s.meter.valid() ||
        frame.ch_layout.nb_channels != static_cast<int>(s.channels) ||
        count > static_cast<std::uint64_t>(std::max(frame.nb_samples, 0)))
        return !(s.failed = true);
    const bool planar = av_sample_fmt_is_planar(format) != 0;
    const auto packed = av_get_packed_sample_fmt(format);
    if (packed != AV_SAMPLE_FMT_S16 && packed != AV_SAMPLE_FMT_S32 && packed != AV_SAMPLE_FMT_FLT &&
        packed != AV_SAMPLE_FMT_DBL)
        return !(s.failed = true);
    const auto frames = static_cast<std::size_t>(count);
    s.interleaved.resize(frames * s.channels);
    for (std::size_t c = 0; c < s.channels; ++c) {
        const auto* base = frame.extended_data[planar ? c : 0];
        for (std::size_t i = 0; i < frames; ++i) {
            const auto index = planar ? i : i * s.channels + c;
            float value = 0.0F;
            switch (packed) {
            case AV_SAMPLE_FMT_S16:
                value = static_cast<float>(reinterpret_cast<const std::int16_t*>(base)[index]) /
                        32768.0F;
                break;
            case AV_SAMPLE_FMT_S32:
                value = static_cast<float>(
                    static_cast<double>(reinterpret_cast<const std::int32_t*>(base)[index]) /
                    2147483648.0);
                break;
            case AV_SAMPLE_FMT_FLT:
                value = reinterpret_cast<const float*>(base)[index];
                break;
            default:
                value = static_cast<float>(reinterpret_cast<const double*>(base)[index]);
                break;
            }
            s.interleaved[i * s.channels + c] = value;
        }
    }
    if (!s.meter.add(s.interleaved))
        return !(s.failed = true);
    return true;
}

WrittenAudioLoudness WrittenAudioMeter::finish() const {
    const auto& s = *state_;
    WrittenAudioLoudness result;
    if (s.failed || !s.meter.valid()) {
        result.failure = "The written audio could not be measured (its sample format or channel "
                         "count is not one the loudness meter reads)";
        return result;
    }
    const auto measured = s.meter.measurement();
    result.loudnessMeasured = measured.loudnessMeasured;
    result.integratedLufs = measured.integratedLufs;
    result.truePeakDbtp = measured.truePeakDbtp;
    result.frames = measured.frames;
    return result;
}

WrittenAudioLoudness measureWrittenAudio(const std::string& path, const unsigned channels,
                                         const unsigned sampleRate, const std::uint64_t frames) {
    WrittenAudioLoudness refused;
    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, path.c_str(), nullptr, nullptr) < 0) {
        refused.failure = "The written audio could not be reopened for its loudness";
        return refused;
    }
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> owned(
        input, [](AVFormatContext* value) { avformat_close_input(&value); });
    if (avformat_find_stream_info(input, nullptr) < 0) {
        refused.failure = "The written audio could not be reopened for its loudness";
        return refused;
    }
    const int index = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (index < 0) {
        refused.failure = "The written file has no audio stream to measure";
        return refused;
    }
    const auto* parameters = input->streams[index]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> decoder(
        codec != nullptr ? avcodec_alloc_context3(codec) : nullptr,
        [](AVCodecContext* value) { avcodec_free_context(&value); });
    if (!decoder || avcodec_parameters_to_context(decoder.get(), parameters) < 0 ||
        avcodec_open2(decoder.get(), codec, nullptr) < 0) {
        refused.failure = "The written audio could not be decoded for its loudness";
        return refused;
    }
    for (int other = 0; other < static_cast<int>(input->nb_streams); ++other)
        if (other != index)
            input->streams[other]->discard = AVDISCARD_ALL;
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet(
        av_packet_alloc(), [](AVPacket* value) { av_packet_free(&value); });
    std::unique_ptr<AVFrame, void (*)(AVFrame*)> frame(
        av_frame_alloc(), [](AVFrame* value) { av_frame_free(&value); });
    if (!packet || !frame) {
        refused.failure = "The written audio could not be decoded for its loudness";
        return refused;
    }
    WrittenAudioMeter meter(channels, sampleRate);
    std::uint64_t measured = 0;
    bool readable = true;
    const auto drain = [&] {
        while (avcodec_receive_frame(decoder.get(), frame.get()) == 0) {
            const auto count = std::min<std::uint64_t>(
                static_cast<std::uint64_t>(std::max(frame->nb_samples, 0)), frames - measured);
            if (frame->sample_rate != static_cast<int>(sampleRate) || !meter.push(*frame, count))
                readable = false;
            measured += count;
            av_frame_unref(frame.get());
        }
    };
    bool ended = false;
    while (readable && measured < frames && !ended) {
        const int read = av_read_frame(input, packet.get());
        if (read < 0) {
            ended = true;
            if (avcodec_send_packet(decoder.get(), nullptr) < 0)
                readable = false;
        } else {
            if (packet->stream_index == index &&
                avcodec_send_packet(decoder.get(), packet.get()) < 0)
                readable = false;
            av_packet_unref(packet.get());
        }
        drain();
    }
    if (!readable || measured != frames) {
        refused.failure = "The written audio could not be decoded for its loudness (" +
                          std::to_string(measured) + " of " + std::to_string(frames) +
                          " frames read)";
        return refused;
    }
    return meter.finish();
}

} // namespace bloom::media::ffmpeg
