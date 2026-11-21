// #752: embedded subtitle tracks through the FFmpeg worker. Encodes H.264 MP4, ProRes MOV and VP9
// WebM with cue tracks, then reads each published file back here through FFmpeg alone (not the
// worker's own reopen check): codec, language, title, neither default nor forced, the MOV and MP4
// track's time base is the video's exact frame rate, and every cue's frames and text equal the
// cues sent. Also: the worker's reopen result carries the same tracks, embedding in MXF is refused,
// and an altered reopened track is a difference that fails the output.
#include "ffmpeg_subtitle_tracks.hpp"
#include "support.hpp"

#include <bloom/media/provider/encode_session.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

using namespace bloom::media::provider;

namespace {

FrameProduct grey(const EncodeSettingsV1& settings, const std::uint64_t index) {
    FrameProduct frame;
    frame.format = PixelFormat::Rgba16;
    // A reduced fraction, as the wire requires.
    const auto numerator = static_cast<std::int64_t>(index) * settings.rate.denominator;
    const auto divisor = std::gcd(numerator, settings.rate.numerator);
    frame.pts = {numerator / divisor, settings.rate.numerator / divisor};
    CpuPlane plane;
    plane.width = settings.width;
    plane.height = settings.height;
    plane.stride = plane.width * 8;
    plane.bytes.resize(static_cast<std::size_t>(plane.stride) * plane.height);
    for (std::size_t at = 0; at < plane.bytes.size(); at += 2) {
        const bool alpha = (at / 2) % 4 == 3;
        const std::uint16_t value = alpha ? 65535U : static_cast<std::uint16_t>(20000U + index * 400U);
        plane.bytes[at] = static_cast<std::byte>(value & 255U);
        plane.bytes[at + 1] = static_cast<std::byte>(value >> 8U);
    }
    frame.planes.push_back(std::move(plane));
    return frame;
}

// Encodes `settings`, writes the artifact to `destination` and returns the worker's reopen result.
// Empty when this FFmpeg has no subtitle encoder (a dependency prefix older than the lock).
std::optional<EncodeQcV1> encode(const EncodeSettingsV1& settings,
                                 const std::filesystem::path& destination) {
    EncodeSessionOptionsV1 options;
    options.platformEncoder = PlatformEncoderV1::Skipped;
    EncodeSessionV1 session(std::move(options));
    if (const auto refused = session.begin(settings)) {
        if (refused->reason == Error::Unavailable &&
            (refused->detail == "Subtitle encoder is not in the intake" ||
             refused->detail == "Encoder is not in the intake"))
            return std::nullopt;
        throw std::runtime_error(refused->detail);
    }
    for (std::uint64_t index = 0; index < settings.frames; ++index)
        if (const auto refused = session.video(grey(settings, index)))
            throw std::runtime_error(refused->detail);
    auto finished = session.finish();
    if (const auto* refused = std::get_if<Unavailable>(&finished))
        throw std::runtime_error(refused->detail);
    const auto qc = std::get<EncodeQcV1>(finished);
    std::ofstream file(destination, std::ios::binary);
    for (std::uint64_t offset = 0; offset < qc.bytes;) {
        auto read = session.read(offset);
        if (const auto* refused = std::get_if<Unavailable>(&read))
            throw std::runtime_error(refused->detail);
        const auto& chunk = std::get<EncodedChunkV1>(read);
        file.write(reinterpret_cast<const char*>(chunk.bytes.data()),
                   static_cast<std::streamsize>(chunk.bytes.size()));
        offset += chunk.bytes.size();
    }
    file.close();
    (void)session.close();
    return qc;
}

// The published file's own streams, through FFmpeg directly.
void inspect(const std::filesystem::path& path, const EncodeSettingsV1& settings) {
    AVFormatContext* opened = nullptr;
    test::check(avformat_open_input(&opened, path.string().c_str(), nullptr, nullptr) >= 0 &&
                    avformat_find_stream_info(opened, nullptr) >= 0,
                "the published file opens");
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> owned(
        opened, [](AVFormatContext* value) { avformat_close_input(&value); });
    const bool webm = settings.container == "webm";
    unsigned found = 0;
    for (unsigned index = 0; index < opened->nb_streams; ++index) {
        const auto* stream = opened->streams[index];
        if (stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
            continue;
        ++found;
        test::check(stream->codecpar->codec_id == (webm ? AV_CODEC_ID_WEBVTT : AV_CODEC_ID_MOV_TEXT),
                    "the track is WebVTT in WebM, 3GPP Timed Text in MOV and MP4");
        test::check((stream->disposition & (AV_DISPOSITION_DEFAULT | AV_DISPOSITION_FORCED)) == 0,
                    "the track is neither default nor forced");
        // The MOV and MP4 track's timescale is the frame rate's numerator, as the video track's
        // is, so every sample time below is a whole number of frames.
        if (!webm)
            test::check(stream->time_base.num == 1 &&
                            stream->time_base.den == settings.rate.numerator,
                        "the MOV and MP4 track counts in the video's exact frame rate");
    }
    test::check(found == settings.subtitles.size(), "every track is in the file");
    if (!webm) {
        AVPacket* packet = av_packet_alloc();
        test::check(packet != nullptr, "packet allocation");
        while (av_read_frame(opened, packet) >= 0) {
            const auto* stream = opened->streams[packet->stream_index];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
                test::check(packet->pts % settings.rate.denominator == 0 &&
                                packet->duration % settings.rate.denominator == 0,
                            "every MOV and MP4 sample starts and ends on an exact frame");
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }
    const auto reopened = bloom::media::ffmpeg::readSubtitleTracks(path.string(), settings.rate);
    const auto difference = subtitleTrackDifference(settings.subtitles, reopened);
    if (!difference.empty())
        std::cerr << path.filename().string() << ": " << difference << '\n';
    test::check(difference.empty(), "the file read back by FFmpeg holds exactly the cues sent");
    for (const auto& track : reopened)
        std::cout << path.filename().string() << " track " << track.language << " \""
                  << track.title << "\" cues=" << track.cues.size() << '\n';
}

EncodeSettingsV1 base() {
    EncodeSettingsV1 settings;
    settings.width = 128;
    settings.height = 64;
    settings.frames = 72;
    settings.audioCodec.clear();
    settings.audioSamples = 0;
    // 24000/1001: exact frames never land on whole milliseconds.
    settings.rate = {24000, 1001};
    SubtitleTrackV1 english;
    english.language = "eng";
    english.title = "English";
    english.cues = {{0, 12, "First line"},
                    {12, 30, "Two lines\nof text, with \\n and {\\b1} as written"},
                    {41, 60, "Ünïcödé — 字幕"},
                    {60, 61, "One frame"},
                    {65, 72, "To the end"}};
    SubtitleTrackV1 indonesian;
    indonesian.language = "ind";
    indonesian.title = "Bahasa Indonesia";
    indonesian.cues = {{5, 20, "Baris pertama"}, {33, 50, "Baris kedua"}};
    settings.subtitles = {english, indonesian};
    return settings;
}

} // namespace

int main(int argc, char** argv) {
    try {
        test::check(argc == 2, "test output directory");
        av_log_set_level(AV_LOG_ERROR);
        const std::filesystem::path directory(argv[1]);
        std::filesystem::create_directories(directory);
        unsigned exercised = 0;
        struct Case {
            const char* name;
            const char* videoCodec;
            const char* profile;
            const char* container;
        };
        for (const auto& c : {Case{"subtitles-h264.mp4", "h264", "high", "mp4"},
                              Case{"subtitles-h264.mov", "h264", "high", "mov"},
                              Case{"subtitles-prores.mov", "prores_ks", "hq", "mov"},
                              Case{"subtitles-vp9.webm", "vp9", "8bit", "webm"}}) {
            auto settings = base();
            settings.videoCodec = c.videoCodec;
            settings.profile = c.profile;
            settings.container = c.container;
            if (settings.container == "webm") {
                // WebM cues may overlap: two cues show together over frames 20-24, and a cue keeps
                // its cue text markup.
                settings.subtitles[0].cues.insert(settings.subtitles[0].cues.begin() + 2,
                                                  {20, 25, "<i>Overlapping</i> &amp; marked up"});
            }
            const auto qc = encode(settings, directory / c.name);
            if (!qc) {
                std::cout << "SKIP: " << c.name
                          << " — this FFmpeg has no subtitle encoder (a dependency prefix older "
                             "than the lock; rebuild it)\n";
                continue;
            }
            test::check(subtitleTrackDifference(settings.subtitles, qc->subtitles).empty(),
                        "the worker's reopen result carries the tracks sent");
            inspect(directory / c.name, settings);
            ++exercised;
        }
        // Embedding is for MOV, MP4 and WebM only.
        {
            auto mxf = base();
            mxf.videoCodec = "dnxhd";
            mxf.profile = "dnxhr_hq";
            mxf.container = "mxf";
            test::check(!valid(mxf), "subtitle tracks in MXF are not a valid request");
            EncodeSessionV1 session({});
            const auto refused = session.begin(mxf);
            test::check(refused.has_value() && refused->reason == Error::InvalidValue,
                        "an MXF encode with subtitle tracks is refused before it starts");
            auto overlap = base();
            overlap.subtitles[0].cues[1].first = 10;
            test::check(!valid(overlap), "overlapping cues in a MOV track are not a valid request");
            auto language = base();
            language.subtitles[0].language = "en";
            test::check(!valid(language), "a two-letter language is not a valid request");
        }
        // The reopen comparison: each altered property is a difference.
        {
            const auto sent = base().subtitles;
            auto changed = sent;
            changed[0].cues[2].end += 1;
            test::check(!subtitleTrackDifference(sent, changed).empty(), "a late cue end");
            changed = sent;
            changed[1].cues[0].first -= 1;
            test::check(!subtitleTrackDifference(sent, changed).empty(), "an early cue start");
            changed = sent;
            changed[0].cues[0].text = "First Line";
            test::check(!subtitleTrackDifference(sent, changed).empty(), "changed text");
            changed = sent;
            changed[0].cues.pop_back();
            test::check(!subtitleTrackDifference(sent, changed).empty(), "a missing cue");
            changed = sent;
            changed.pop_back();
            test::check(!subtitleTrackDifference(sent, changed).empty(), "a missing track");
            changed = sent;
            changed[1].language = "msa";
            test::check(!subtitleTrackDifference(sent, changed).empty(), "another language");
            changed = sent;
            changed[1].title = "Indonesian";
            test::check(!subtitleTrackDifference(sent, changed).empty(), "another title");
            test::check(subtitleTrackDifference(sent, sent).empty(), "equal tracks");
            std::cout << "injected: " << subtitleTrackDifference(sent, [&] {
                auto late = sent;
                late[0].cues[2].end += 1;
                return late;
            }()) << '\n';
        }
        if (exercised == 0)
            return 77;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
