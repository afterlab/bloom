// Issue #650: an independent read of the thirty-second H.264 MP4 with AAC that bloom.media.encode
// writes. FFmpeg alone opens the file and decodes the audio, honoring the priming the container
// declares, and the result is held against the samples the encoder was given: the same number of
// frames, every isolated click within one AAC frame (1024 frames) of where the source has it, and
// each second's level within 1 dB of the source's.
#include "program_audio.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
namespace {
using namespace bloom::media::testing;
int failures = 0;
void check(const bool ok, const std::string& what) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << what << '\n';
    if (!ok)
        ++failures;
}
// The first audio stream, decoded to interleaved stereo float, and the length the track declares
// in frames. The decoder drops the priming the container declares; a player ends the track at its
// declared length, which drops the padding of the last AAC frame. Empty when it cannot be read.
std::vector<float> decode(const std::filesystem::path& path, std::int64_t& declaredFrames) {
    std::vector<float> samples;
    declaredFrames = -1;
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path.string().c_str(), nullptr, nullptr) < 0)
        return samples;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const int index = avformat_find_stream_info(format, nullptr) >= 0
                          ? av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)
                          : -1;
    bool sound = false;
    if (index >= 0 && packet && frame) {
        const auto& stream = *format->streams[index];
        const auto& parameters = *stream.codecpar;
        if (stream.duration != AV_NOPTS_VALUE && parameters.sample_rate > 0)
            declaredFrames =
                av_rescale_q(stream.duration, stream.time_base, {1, parameters.sample_rate});
        const auto* decoder = avcodec_find_decoder(parameters.codec_id);
        codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
        sound = parameters.codec_id == AV_CODEC_ID_AAC && codec &&
                avcodec_parameters_to_context(codec, &parameters) >= 0 &&
                avcodec_open2(codec, decoder, nullptr) >= 0;
    }
    const auto drain = [&] {
        while (avcodec_receive_frame(codec, frame) >= 0) {
            if (frame->format != AV_SAMPLE_FMT_FLTP || frame->ch_layout.nb_channels != 2) {
                sound = false;
                return;
            }
            for (int i = 0; i < frame->nb_samples; ++i)
                for (int c = 0; c < 2; ++c)
                    samples.push_back(reinterpret_cast<const float*>(frame->extended_data[c])[i]);
            av_frame_unref(frame);
        }
    };
    while (sound && av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == index && avcodec_send_packet(codec, packet) >= 0)
            drain();
        av_packet_unref(packet);
    }
    if (sound && avcodec_send_packet(codec, nullptr) >= 0)
        drain();
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec);
    avformat_close_input(&format);
    if (!sound)
        samples.clear();
    return samples;
}
double decibels(const std::vector<float>& stream, const std::size_t first, const std::size_t count,
                const std::size_t channel) {
    double squared = 0;
    for (std::size_t i = first; i < first + count; ++i) {
        const auto value = static_cast<double>(stream[i * kChannels + channel]);
        squared += value * value;
    }
    return 10.0 * std::log10(squared / static_cast<double>(count) + 1e-12);
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: <encode-fixtures>\n";
        return 2;
    }
    const auto path = std::filesystem::path(argv[1]) / "export-h264-program.mp4";
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        // bloom.media.encode writes it wherever the media worker encodes H.264 and AAC.
        std::cout << "SKIP: " << path.filename().string() << " was not produced\n";
        return 77;
    }
    const auto source = programAudio();
    std::int64_t declared = -1;
    auto decoded = decode(path, declared);
    check(!decoded.empty(), "FFmpeg decodes the published AAC as stereo float");
    check(declared == static_cast<std::int64_t>(kFrames),
          "the track declares the source's length: " + std::to_string(declared) + " of " +
              std::to_string(kFrames) + " frames");
    // What follows the declared length is the last AAC frame's padding: less than one frame.
    check(decoded.size() >= source.size() && decoded.size() - source.size() < 1024U * kChannels,
          "the decoded stream is the source's length plus less than one AAC frame of padding: " +
              std::to_string(decoded.size() / kChannels) + " frames");
    if (decoded.size() < source.size())
        return 1;
    decoded.resize(source.size());

    // Start sync: each click is one full-scale frame with quiet around it. The loudest decoded
    // frame near it is where the click landed.
    long worst = 0;
    for (std::size_t click = 0; click < kClicks; ++click) {
        const auto at = clickFrame(click);
        if (at < 4096 || at + 4096 >= kFrames)
            continue;
        std::size_t loudest = at - 4096;
        for (std::size_t frame = at - 4096; frame <= at + 4096; ++frame)
            if (std::abs(decoded[frame * kChannels]) > std::abs(decoded[loudest * kChannels]))
                loudest = frame;
        const long lag = static_cast<long>(loudest) - static_cast<long>(at);
        if (std::labs(lag) > std::labs(worst))
            worst = lag;
    }
    check(std::labs(worst) < 1024, "every click is within one AAC frame of the source: worst " +
                                       std::to_string(worst) + " frames");

    // Levels: each second of each channel against the source.
    double widest = 0;
    for (std::size_t second = 0; second < kFrames / kRate; ++second)
        for (std::size_t channel = 0; channel < kChannels; ++channel) {
            const double difference = std::abs(decibels(decoded, second * kRate, kRate, channel) -
                                               decibels(source, second * kRate, kRate, channel));
            widest = std::max(widest, difference);
        }
    check(widest <= 1.0, "every second's level is within 1 dB of the source: widest " +
                             std::to_string(widest) + " dB");
    return failures == 0 ? 0 : 1;
}
