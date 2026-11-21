// End-to-end export test for the macOS VideoToolbox worker. It drives the real host session, which
// launches the real bloom-media-worker process and round-trips the protocol, so failures in the
// worker handshake, colour conversion, encode, reopen QC, chunk read, or artifact digest surface
// here at a single low-resolution frame instead of only in the running application.
#include <bloom/media/provider/encode.hpp>
#include <bloom/media/provider/encode_session.hpp>
#include <bloom/media/provider/media_workers.hpp>

#if defined(BLOOM_VIDEOTOOLBOX_SUBTITLE_READBACK)
#include "ffmpeg_subtitle_tracks.hpp"

#include <filesystem>
#include <fstream>
#endif

#include <cstdio>
#include <numeric>
#include <string>
#include <variant>
#include <vector>

namespace {
int failures = 0;
void check(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

[[nodiscard]] bloom::media::provider::FrameProduct
makeFrame(const std::uint32_t width, const std::uint32_t height, const std::uint64_t index) {
    using namespace bloom::media;
    provider::FrameProduct frame;
    frame.format = provider::PixelFormat::Rgba16;
    // Canonical records require a reduced rational; frame 0 of a 24 fps clip is exactly 0.
    frame.pts = {0, 1};
    provider::CpuPlane plane;
    plane.width = width;
    plane.height = height;
    plane.stride = width * 8U;
    plane.bytes.assign(static_cast<std::size_t>(plane.stride) * height, std::byte{0});
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            auto* pixel = reinterpret_cast<unsigned char*>(
                plane.bytes.data() + static_cast<std::size_t>(y) * plane.stride +
                static_cast<std::size_t>(x) * 8);
            const bool red = x < width / 2;
            pixel[0] = red ? 255 : 0;
            pixel[1] = red ? 255 : 0;
            pixel[3] = static_cast<unsigned char>(y * 5);
            pixel[5] = static_cast<unsigned char>(index * 80);
            pixel[7] = 255;
        }
    frame.planes.push_back(std::move(plane));
    return frame;
}

// One single-frame export through the worker: encode, reopen QC, then read the whole artifact and
// confirm its staged digest. `audio` adds a PCM track.
void exportOnce(const char* codec, const char* profile, const bool audio) {
    using namespace bloom::media;
    provider::EncodeSettingsV1 settings;
    settings.container = "mov";
    settings.videoCodec = codec;
    settings.profile = profile;
    settings.width = 64;
    settings.height = 48;
    settings.rate = {24, 1};
    settings.frames = 1;
    if (audio) {
        settings.audioCodec = "pcm_s16le";
        settings.sampleRate = 48000;
        settings.channels = 2;
        settings.audioSamples = 2000; // one frame at 24 fps
    }

    provider::EncodeSessionV1 session({}, {});
    if (const auto error = session.begin(settings)) {
        check(false, std::string(codec) + " begin: " + error->detail);
        return;
    }
    if (const auto error = session.video(makeFrame(settings.width, settings.height, 0))) {
        check(false, std::string(codec) + " video: " + error->detail);
        return;
    }
    if (audio) {
        provider::AudioBlock block;
        block.pts = {0, 1};
        block.sampleRate = settings.sampleRate;
        block.channelLayout = {"L", "R"};
        block.channels.assign(2, std::vector<float>(settings.audioSamples, 0.25F));
        if (const auto error = session.audio(std::move(block))) {
            check(false, std::string(codec) + " audio: " + error->detail);
            return;
        }
    }
    auto finished = session.finish();
    if (auto* error = std::get_if<provider::Unavailable>(&finished)) {
        check(false, std::string(codec) + " finish: " + error->detail);
        return;
    }
    const auto qc = std::get<provider::EncodeQcV1>(finished);
    check(qc.frames == settings.frames, std::string(codec) + " reopened frame count");
    check(qc.bytes > 0, std::string(codec) + " artifact is not empty");
    if (audio)
        check(qc.audioSamples == settings.audioSamples, std::string(codec) + " audio sample count");

    provider::Bytes artifact;
    while (artifact.size() < qc.bytes) {
        auto reply = session.read(artifact.size());
        if (auto* error = std::get_if<provider::Unavailable>(&reply)) {
            check(false, std::string(codec) + " read: " + error->detail);
            return;
        }
        const auto& chunk = std::get<provider::EncodedChunkV1>(reply);
        check(chunk.offset == artifact.size(), std::string(codec) + " chunk offset");
        if (chunk.bytes.empty())
            break;
        artifact.insert(artifact.end(), chunk.bytes.begin(), chunk.bytes.end());
    }
    check(artifact.size() == qc.bytes, std::string(codec) + " artifact byte count");
    check(provider::digestBytes(artifact) == qc.artifact,
          std::string(codec) + " artifact digest matches QC");
    check(!session.close(), std::string(codec) + " worker shutdown acknowledged");
}
#if defined(BLOOM_VIDEOTOOLBOX_SUBTITLE_READBACK)
// #752: embedded subtitle tracks written by AVAssetWriter (the system encoder is required, so the
// FFmpeg worker cannot stand in), then read back by FFmpeg's demuxer here as on Linux and
// Windows: the same tracks, languages and titles, cue frames and text, neither default nor forced.
void exportSubtitles(const char* container) {
    using namespace bloom::media;
    const auto name = std::string("H.264 ") + container + " subtitles";
    provider::EncodeSettingsV1 settings;
    settings.container = container;
    settings.videoCodec = "h264";
    settings.profile = "high";
    settings.width = 64;
    settings.height = 48;
    settings.rate = {24000, 1001};
    settings.frames = 48;
    provider::SubtitleTrackV1 english;
    english.language = "eng";
    english.title = "English";
    english.cues = {{0, 12, "First line"},
                    {12, 30, "Two lines\nof text, with \\n and {\\b1} as written"},
                    {35, 47, "\xc3\x9cn\xc3\xafc\xc3\xb6d\xc3\xa9"}};
    provider::SubtitleTrackV1 german;
    german.language = "deu";
    german.title = "Deutsch";
    german.cues = {{5, 20, "Erste Zeile"}, {33, 48, "Zweite Zeile"}};
    settings.subtitles = {english, german};
    provider::EncodeSessionOptionsV1 options;
    options.platformEncoder = provider::PlatformEncoderV1::Required;
    provider::EncodeSessionV1 session(std::move(options));
    if (const auto error = session.begin(settings)) {
        check(false, name + " begin: " + error->detail);
        return;
    }
    check(session.provenance().provider == provider::nativeMediaProviderId(),
          name + " is written by the system encoder: " + session.provenance().diagnostic());
    for (std::uint64_t index = 0; index < settings.frames; ++index) {
        auto frame = makeFrame(settings.width, settings.height, index);
        const auto numerator = static_cast<std::int64_t>(index) * settings.rate.denominator;
        const auto divisor = std::gcd(numerator, settings.rate.numerator);
        frame.pts = {numerator / divisor, settings.rate.numerator / divisor};
        if (const auto error = session.video(std::move(frame))) {
            check(false, name + " video: " + error->detail);
            return;
        }
    }
    auto finished = session.finish();
    if (auto* error = std::get_if<provider::Unavailable>(&finished)) {
        check(false, name + " finish: " + error->detail);
        return;
    }
    const auto qc = std::get<provider::EncodeQcV1>(finished);
    check(provider::subtitleTrackDifference(settings.subtitles, qc.subtitles).empty(),
          name + ": the worker's reopen check carries the tracks sent");
    provider::Bytes artifact;
    while (artifact.size() < qc.bytes) {
        auto reply = session.read(artifact.size());
        if (std::holds_alternative<provider::Unavailable>(reply))
            break;
        const auto& chunk = std::get<provider::EncodedChunkV1>(reply);
        if (chunk.bytes.empty())
            break;
        artifact.insert(artifact.end(), chunk.bytes.begin(), chunk.bytes.end());
    }
    (void)session.close();
    const auto path = std::filesystem::temp_directory_path() /
                      (std::string("bloom-videotoolbox-subtitles.") + container);
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(artifact.data()),
                   static_cast<std::streamsize>(artifact.size()));
    }
    try {
        const auto reopened = ffmpeg::readSubtitleTracks(path.string(), settings.rate);
        const auto difference = provider::subtitleTrackDifference(settings.subtitles, reopened);
        check(difference.empty(), name + ": FFmpeg reads back the tracks sent: " + difference);
        for (const auto& track : reopened)
            std::printf("%s: track %s \"%s\" with %zu cues\n", name.c_str(), track.language.c_str(),
                        track.title.c_str(), track.cues.size());
    } catch (const provider::Unavailable& error) {
        check(false, name + ": FFmpeg reads the file: " + error.detail);
    }
    std::filesystem::remove(path);
}
#endif
} // namespace

int main() {
    exportOnce("prores", "hq", false);
    exportOnce("prores", "hq", true);
    exportOnce("h264", "high", false);
#if defined(BLOOM_VIDEOTOOLBOX_SUBTITLE_READBACK)
    exportSubtitles("mov");
    exportSubtitles("mp4");
#endif
    if (failures == 0)
        std::printf("videotoolbox export: all checks passed\n");
    return failures;
}
