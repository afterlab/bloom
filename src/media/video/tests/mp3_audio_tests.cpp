// Issue #770: an MP3 as a text-to-speech service writes it decodes completely through the media
// worker, for preview and export alike. The fixture (tagged-vbr-mono.mp3, provenance beside it)
// has the same structure: 44.1 kHz mono VBR from libmp3lame, an ID3v2.4 tag, and a Xing header
// whose LAME extension declares the encoder delay. The demuxer trims that delay, so the first
// decoded frame is timed after zero; the worker must count the audio from there. FFmpeg read
// directly is the reference: the same frame count and the same samples.
#include "unique_temp_dir.hpp"

#include <bloom/media/audio/audio.hpp>
#include <bloom/media/video/audio.hpp>
#include <bloom/media/video/session.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
namespace {
namespace media = bloom::media;
int failures = 0;
void check(const bool ok, const std::string& what) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << what << '\n';
    if (!ok)
        ++failures;
}
// The first audio stream decoded by FFmpeg alone, mono float. The decoder drops the encoder
// delay and padding the Xing header declares. Empty when the file cannot be read.
std::vector<float> reference(const std::filesystem::path& path) {
    std::vector<float> samples;
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
        const auto& parameters = *format->streams[index]->codecpar;
        const auto* decoder = avcodec_find_decoder(parameters.codec_id);
        codec = decoder ? avcodec_alloc_context3(decoder) : nullptr;
        sound = parameters.codec_id == AV_CODEC_ID_MP3 && codec &&
                avcodec_parameters_to_context(codec, &parameters) >= 0 &&
                avcodec_open2(codec, decoder, nullptr) >= 0;
    }
    const auto drain = [&] {
        while (avcodec_receive_frame(codec, frame) >= 0) {
            if (frame->ch_layout.nb_channels != 1 ||
                (frame->format != AV_SAMPLE_FMT_FLTP && frame->format != AV_SAMPLE_FMT_S16P)) {
                std::cout << "reference decoder format " << frame->format << '\n';
                sound = false;
                return;
            }
            // The worker converts to float with libswresample, which scales 16-bit by 1/32768.
            if (frame->format == AV_SAMPLE_FMT_FLTP) {
                const auto* plane = reinterpret_cast<const float*>(frame->extended_data[0]);
                samples.insert(samples.end(), plane, plane + frame->nb_samples);
            } else {
                const auto* plane = reinterpret_cast<const std::int16_t*>(frame->extended_data[0]);
                for (int i = 0; i < frame->nb_samples; ++i)
                    samples.push_back(static_cast<float>(plane[i]) / 32768.0F);
            }
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
// The first index at which two sample runs differ, or the shorter length.
std::size_t firstDifference(std::span<const float> a, std::span<const float> b) {
    const auto count = std::min(a.size(), b.size());
    std::size_t at = 0;
    while (at < count && a[at] == b[at])
        ++at;
    return at;
}
void decodesCompletely(const std::filesystem::path& path) {
    const auto expected = reference(path);
    check(expected.size() > 44'100U,
          "FFmpeg decodes the fixture: " + std::to_string(expected.size()) + " frames");
    auto session = std::make_shared<media::video::VideoDecodeSession>(path);
    const auto probe = session->probe();
    check(std::holds_alternative<media::provider::ProbeResult>(probe), "the worker probes the MP3");
    if (!std::holds_alternative<media::provider::ProbeResult>(probe))
        return;
    const auto& probed = std::get<media::provider::ProbeResult>(probe);
    const auto* stream = media::video::audioStreamAt(probed, 0);
    check(stream != nullptr && stream->sampleRate == 44'100U && stream->channelLayout.size() == 1U,
          "the probe reports one 44.1 kHz mono audio stream");
    if (stream == nullptr)
        return;
    const auto frames = media::video::audioClipFrameCount(*stream);
    check(frames == expected.size(),
          "the probe's frame count is FFmpeg's: " + std::to_string(frames.value_or(0)) + " of " +
              std::to_string(expected.size()));

    // The export and preview route: the bounded window reading forward, block by block.
    auto windowed = media::video::openWindowedVideoAudio(session, probed);
    check(std::holds_alternative<media::audio::AudioBuffer>(windowed),
          "the windowed reader opens the MP3");
    if (!std::holds_alternative<media::audio::AudioBuffer>(windowed))
        return;
    const auto& buffer = std::get<media::audio::AudioBuffer>(windowed);
    check(buffer.stream != nullptr && buffer.frames == expected.size(),
          "the windowed buffer spans the whole stream");
    std::vector<float> read(static_cast<std::size_t>(buffer.frames));
    constexpr std::size_t kBlock = 1024;
    bool complete = buffer.stream != nullptr;
    for (std::size_t at = 0; complete && at < read.size(); at += kBlock) {
        const auto count = std::min(kBlock, read.size() - at);
        const auto result =
            buffer.stream->readInterleaved(at, std::span<float>(read).subspan(at, count));
        complete = result.hasValue() && *result.value() == count;
    }
    check(complete && buffer.stream && !buffer.stream->diagnostic(),
          "every block of the stream decodes, with no diagnostic");
    const auto windowedAt = firstDifference(read, expected);
    check(read.size() == expected.size() && windowedAt == expected.size(),
          "the windowed samples are FFmpeg's (first difference at " + std::to_string(windowedAt) +
              ")");

    // A read that starts mid-stream repositions the worker and lands on the same samples.
    const std::size_t middle = expected.size() / 2 + 17;
    std::vector<float> later(4096);
    const auto seek = buffer.stream->readInterleaved(middle, later);
    check(seek.hasValue() && *seek.value() == later.size() &&
              firstDifference(later, std::span<const float>(expected).subspan(middle)) ==
                  later.size(),
          "a read from the middle lands on FFmpeg's samples");

    // The whole clip in memory: the same samples again.
    auto whole = media::video::decodeAudioClip(*session, probed, expected.size());
    check(std::holds_alternative<media::audio::AudioBuffer>(whole),
          "the whole clip decodes into memory");
    if (const auto* clip = std::get_if<media::audio::AudioBuffer>(&whole))
        check(clip->planes.size() == 1U && clip->planes.front().size() == expected.size() &&
                  firstDifference(clip->planes.front(), expected) == expected.size(),
              "the whole clip is FFmpeg's samples");
}
} // namespace
// Preview reads an audio asset in process (minimp3): it decodes every frame too, as many as FFmpeg,
// through the same bounded window, and the samples agree with FFmpeg's to within the two decoders'
// floating-point rounding.
void decodesCompletelyInProcess(const std::filesystem::path& path) {
    const auto expected = reference(path);
    const auto probe = media::audio::probeAudio(path);
    check(probe.hasValue() && probe.value()->container == media::audio::AudioContainer::Mp3 &&
              probe.value()->rate == 44'100U && probe.value()->channels == 1U,
          "the in-process reader probes a 44.1 kHz mono MP3");
    if (!probe.hasValue())
        return;
    check(probe.value()->frames == expected.size(),
          "the in-process frame count is FFmpeg's: " + std::to_string(probe.value()->frames) +
              " of " + std::to_string(expected.size()));
    // The window preview analysis opens: its sample budget is one window.
    media::audio::AudioDecodeLimits limits;
    limits.sampleBudget = std::uint64_t{media::audio::AudioStream::kWindowFrames} * 2U;
    auto opened =
        media::audio::openWindowedAudio(path, limits, media::audio::AudioStream::kWindowFrames);
    check(opened.hasValue() && opened.value()->stream != nullptr,
          "the in-process windowed reader opens the MP3" +
              (opened.error() ? ": " + media::audio::describeAudioError(*opened.error())
                              : std::string{}));
    if (!opened.hasValue() || opened.value()->stream == nullptr)
        return;
    const auto& buffer = *opened.value();
    std::vector<float> read(static_cast<std::size_t>(buffer.frames));
    constexpr std::size_t kBlock = 1024;
    bool complete = true;
    for (std::size_t at = 0; complete && at < read.size(); at += kBlock) {
        const auto count = std::min(kBlock, read.size() - at);
        const auto result =
            buffer.stream->readInterleaved(at, std::span<float>(read).subspan(at, count));
        complete = result.hasValue() && *result.value() == count;
    }
    check(complete && !buffer.stream->diagnostic(),
          "every in-process block decodes, with no diagnostic");
    double widest = 0.0;
    for (std::size_t i = 0; i < std::min(read.size(), expected.size()); ++i)
        widest = std::max(widest, static_cast<double>(std::abs(read[i] - expected[i])));
    check(read.size() == expected.size() && widest < 1e-3,
          "the in-process samples are FFmpeg's, sample for sample: widest difference " +
              std::to_string(widest));
}
// A copy cut in half keeps its Xing header, which declares the whole length. Decoding must refuse
// at the cut rather than play the missing half as silence.
void truncatedFails(const std::filesystem::path& fixture, const std::filesystem::path& directory) {
    std::ifstream in(fixture, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto cut = directory / "truncated-vbr-mono.mp3";
    {
        std::ofstream out(cut, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size() / 2));
    }
    auto session = std::make_shared<media::video::VideoDecodeSession>(cut);
    const auto probe = session->probe();
    check(std::holds_alternative<media::provider::ProbeResult>(probe),
          "the worker probes the truncated MP3");
    if (!std::holds_alternative<media::provider::ProbeResult>(probe))
        return;
    const auto& probed = std::get<media::provider::ProbeResult>(probe);
    auto whole = media::video::decodeAudioClip(*session, probed, 1U << 24U);
    const auto* refused = std::get_if<media::provider::Unavailable>(&whole);
    check(refused != nullptr, "the truncated MP3 is refused, not padded with silence" +
                                  (refused ? ": " + refused->detail : std::string{}));
    auto windowed = media::video::openWindowedVideoAudio(session, probed);
    if (const auto* buffer = std::get_if<media::audio::AudioBuffer>(&windowed)) {
        std::vector<float> tail(1024);
        const auto read = buffer->stream->readInterleaved(buffer->frames - tail.size(), tail);
        check(!read.hasValue() && buffer->stream->diagnostic().has_value(),
              "a windowed read past the cut fails with a diagnostic");
    } else {
        check(false, "the windowed reader opens the truncated MP3");
    }
    std::error_code ignored;
    std::filesystem::remove(cut, ignored);
}
int main(int argc, char** argv) {
    const auto fixture =
        argc == 2 ? std::filesystem::path{argv[1]}
                  : std::filesystem::path{BLOOM_AUDIO_FIXTURE_DIR} / "tagged-vbr-mono.mp3";
    try {
        decodesCompletely(fixture);
        decodesCompletelyInProcess(fixture);
        truncatedFails(fixture, bloom::test_support::processTempDirectory());
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
